# Current state of the tissot port — 2026-10-22

This is the authoritative statement of what the port does today. Every other
document in `porting/` is either evidence for a claim made here or the
historical log of how the port got here; the dated run reports are not
rewritten, they are what happened.

* The build, the packaging and the flash procedure: [BUILD_TISSOT.md](../BUILD_TISSOT.md),
  scripted as `porting/tools/build-kernel.sh`.
* The evidence for the claims below: `porting/findings/`, and
  `porting/FAILING-COMPONENTS.md` for the per-component table.
* The porting plan and the component audits: `porting/plan/`,
  `porting/findings/2026-10-18-live-component-audit.md`.

## The device

The port kernel is **flashed to the boot partition**, slot b. The phone
comes back on the port kernel by itself after a power cycle, so the
RAM-boot-and-pull-the-logdump cycle is no longer needed for a normal test.
A hung kernel is recovered by forcing the phone off and flashing the
previous image from fastboot; the images are in `../ack-kgsl-artifacts`.

**Root is KernelSU-Next, built into the kernel.** `drivers/kernelsu/` is the
v3.4.2 tag vendored in, built in rather than loaded as a module, with the
version pinned to 33355 on the make command line. The manager's *adb root*
feature puts the adb shell in the `u:r:ksu:s0` domain as uid 0, which is
what makes the kgsl debugfs readable — and the kgsl debugfs is where the
GPU's retired count is.

**The grant-root fix is in run302.** The path that crashed is
`SET_APP_PROFILE`, not `KSU_IOCTL_SET_SEPOLICY`. It calls
`ksu_mark_running_process_locked()`, which walks every thread under
`tracepoint_reg_lock` with IRQs disabled and used to print one `tp_marker:`
line per thread. On this phone (4,211 threads) that was ~5,000 synchronous
printk lines; the watchdog declared a 25 s soft lockup at
`do_set_app_profile` and panicked the kernel
(`porting/findings/ksu-grant-root-softlockup-20261020.md`). The local fix
removes the per-thread logging and keeps one summary line. run302 boots with
no soft lockup; the grant path itself has **not been exercised** on the new
image: grant root to an app other than FKM and check for one
`tp_marker: refreshed running processes` line and no soft lockup. The adb
root feature is unaffected.

## What works

| Area | State | Evidence |
|---|---|---|
| Kernel boots, Android comes up | Working | every run from 234 on |
| CPU clock policy | Working | one shared `schedutil` policy over CPUs 0-7, 480-2784 MHz, the APCC handoff completes, no failed transitions |
| Display | Working | the panel is driven; the homescreen renders |
| GPU | Working, with one open fault | `/dev/kgsl-3d0` is driven with the stock ioctl ABI; the retirement stall below is open |
| Touch | Working | the `edt-ft5x06` panel, the multi-touch slots |
| Volume and power keys | Working | the PMIC's pwrkey and resin, and a gpio-keys node |
| Capacitive navigation keys | Working | the touch controller's key row reports `KEY_MENU`, `KEY_BACK` and `KEY_HOMEPAGE`; the recents key is `KEY_APPSELECT` (580) |
| Hall sensor | Working | `SW_LID` on the gpio-keys device |
| Root | Working | KernelSU-Next's adb root, `uid=0` / `u:r:ksu:s0` |
| Log capture | Working | the `logdump` partition, the USB console, the ramoops sampler |
| The checkpoint reboot loop | In run302, not verified | the wakeup statistics relabel fix is in `init-run302`; the previous boot still reports `sys.boot.reason=reboot,checkpoint_commit_failed` and the early relabel diagnostics show `labels=0 skipped=1`, so the result is not closed |
| The `adsprpcd` CAF ioctl layer | In the kernel, one command missing | the port decodes CAF's ioctl numbering; on the phone the daemon now fails on exactly one command, `GETINFO`, whose handler is committed but not in the flashed image |
| Wi-Fi | Working on run304, persistent fix not yet booted | the NV blob was staged at `/data/vendor/wifi/firmware` and the interface associated; `CONFIG_EXTRA_FIRMWARE` in `7c1515865` makes that a kernel built-in for the next run |
| Cellular, Bluetooth | Bluetooth working on run334; modem offline | the `smd_pkt` driver, the WCNSS device tree node, the config change and the initramfs MSS start are in the run334 image; `/dev/smd2`/`/dev/smd3` and the QTI HAL are up and the controller is `ON`. The modem is still `offline` (`remoteproc0`, `mba.mbn`), `gsm.sim.state` is empty; the initramfs `firmware_class.path` write failed with `errno=13` so the MSS start was never reached, and the reset cause is unverified. See [findings/2026-10-21-bt-modem-bringup.md](findings/2026-10-21-bt-modem-bringup.md) |
| Audio playback | Working on run344/run346 | the speaker path plays: the HAL opens the deep buffer stream with the donor's parameters (`acdb_dev_id 1058`, `snd_device_be_idx 52`, the mixer path `deep-buffer-playback speaker`) and the DEEP_BUFFER thread writes frames. See [findings/2026-10-22-audio-playback-run344.md](findings/2026-10-22-audio-playback-run344.md) |
| Hardware video decode | Being ported | the ROM's OMX service needs CAF's `vidc_3x`; the driver and its user space interface are in the tree, the device tree and the switch-over are not. See [findings/2026-10-22-vidc3x-port-state.md](findings/2026-10-22-vidc3x-port-state.md) |
| Cameras | Failing | see `FAILING-COMPONENTS.md` |

## What is open, and what is known about it

Section 3 below is closed: it is kept because the constraints it lists are
the reason the fix works, and because the same failure would come back if
the relabel were moved in front of the bootdevice guard again.

### 1. The GPU retirement stall — the display hang

The stall is one thing, and it is in the kgsl debugfs:

```
context-6: last=1489029
	6:1490816 age=25 func=kgsl_sync_fence_event_cb [retired=1489029]
```

The retired count is frozen at 1489029 while a fence waits for timestamp
1490816. The retired count is the GPU's own shared-memstore `eoptimestamp`
slot, so the GPU stopped retiring. The dispatcher then stops, and a fence
that is not already retired is never released — the display waits for it
forever. The release path is dispatcher → `kgsl_process_event_groups()` →
`kgsl_sync_fence_event_cb()` → `kgsl_sync_timeline_signal()`.

Also seen in the stalling boot: `ft_hang_intr_status=1` (the fault detector
saw a hang), the GPU interrupt path taking 19 ms with interrupts disabled,
and the system tick rate falling from 204/s to 141/s.

**A lead found on 2026-10-20, not yet proven.** In a healthy boot the GPU
does a full start — a complete ME_INIT — every three seconds, and each start
ends in `adreno_ringbuffer_submit_spin(rb, NULL, 2000)`, a **busy spin of up
to two seconds** waiting for the RBBM to go idle. That is both a CPU-burn
source and a stall candidate: if the spin times out with `-ETIMEDOUT` the
start fails, the work is never submitted, its timestamp never retires, and
the fence waits forever — exactly the observed symptom. The next thing to do
is to catch a stalling boot and read whether the spin failed.

`porting/tools/kgsl-stall-watch.sh` runs on the phone and does that
automatically: it samples the debugfs, and when a context's retired count
stops advancing while a fence is still pending, it captures dmesg,
`/proc/interrupts`, `/proc/softirqs` and the kgsl state into
`/data/local/tmp/kgsl-stall/`.

### 2. The CPU thermal path is enabled and boot-verified

The clock policy works. The pieces that can throttle it are live on the
phone; only the sustained passive-trip behaviour remains to be exercised.

* `CONFIG_QCOM_TSENS=y` is now in `porting/tools/kernel.config`, which is
  the configuration the build copies to `out/.config`; the defconfig is
  not read. `porting/tools/build-kernel.sh` also checks the symbol after
  `olddefconfig`. `drivers/thermal/qcom/` compiles with it on.
* The CPU nodes already carry `#cooling-cells = <2>`, and `msm8953.dtsi`
  defines `cpu0-thermal` through `cpu7-thermal` plus `gpu-thermal` with
  cooling maps. The earlier note that the CPU nodes had no cooling cells
  was stale.
* The `msm8953` TSENS node uses `qcom,msm8953-tsens` with the generic
  `qcom,tsens-v2` fallback and `#qcom,sensors = <16>`. The v2 generic ops
  have no software calibration callback, so no separate `tsens-8953.c`
  is required.
* The phone now runs `run304` with `CONFIG_QCOM_TSENS=y` and reports the
  eight CPU zones plus `gpu-thermal`, `pm8953-thermal`, `battery` and
  `bms`, all with sane temperatures. This verifies the TSENS probe and the
  thermal-zone wiring. The CPU passive trip has now been exercised; see
  section 8. The critical trip and the GPU trip are still untested.

The historical failure remains the reason this is first: with no CPU
zones, the policy sat at the top OPP (1.958-2.016 GHz) and the phone died
of heat, run261 at about 326 s and run265 at about 581 s. The CPU burn is
not the governor. The load is uninterruptible sleep — the fence waiters
— and the binder storm is PowerManagerService raising SIGBUS on itself.
The frequency is a symptom.

### 3. The early hang: fixed by the relabel

`run278`'s logdump ends like this:

```
init: Received sys.powerctl='reboot,Checkpoint commit failed' from pid: 903
init: Got shutdown_command 'reboot,Checkpoint commit failed' ...
sysrq: Terminate All Tasks
```

and nothing after it. So it is not a kernel panic and not a hang in a
driver: `system_server` asked for a reboot, Android's init shut its services
down, the kernel was told to terminate all tasks, and the reboot never
reached the reset.

The reason was in the same logdump: **SELinux denials** for the
`system_suspend` domain reading the wakeup statistics. The ROM's policy was
written for the 4.19 CAF kernel, where the statistics lived under
`/sys/devices/virtual/wakeup`; the 6.12 kernel puts them per device, so
they land on the generic `sysfs` type and the suspend service is denied.
The snapshot fails, the checkpoint commit fails, and the phone asks for a
reboot it cannot finish.

**The fix is a label, not a policy change.** The ROM's policy already has
the type it expects for them (`sysfs_wakeup`), and the port's own base
policy has the matching `genfscon`. Relabelling the nodes by hand, live,
made the denials stop: 230 → 0 on the running phone.

**But the relabel in the initramdisk never did it.** The log said
`ret=-1` and the nodes stayed on the generic type, because the relabel
child is forked in the initramdisk's own mount namespace and that
namespace has no `/sys` at all — the kernel mounts nothing there, and the
ROM mounts its own in the ROM's own namespace. The walk found nothing.
The child now mounts the kernel's sysfs for itself, and the log says how
many labels it set.

The same fault cost the GPU its power control: the kgsl's `max_pwrlevel`,
`min_pwrlevel`, `force_rail_on` and `force_clk_on` are on the generic type
as well, so the writes that set the GPU's power level are denied
(`NodeLooperThread` in the avc records), and the GPU stays at its lowest
rate.

**Where it stands:** the fix is committed (`9a58f8fbb`) and is in no image
yet. The hand relabel of two wakeup directories on the running phone
dropped the `system_suspend` denials from 230 to 0, which is the mechanism
working; what the next boot has to show is the initramdisk's own relabel
doing the same thing for all of them.

The two constraints on doing it in the initramdisk are still the ones that
were learned the hard way, and both are kept:

* The relabel must not run in the ROM's mount namespace before the ROM has
  mounted its `/sys`. It runs in the initramdisk's own namespace, and
  mounts the kernel's sysfs there itself.
* It must **not delay the bootdevice guard**. `run283` put the relabel in
  front of the guard and the phone came up on a dim lit screen with no adb
  and no USB. The guard has to install the read-only fstab and the vendor
  contexts *before* the ROM's init opens its SELinux policy, and the window
  between the two is about **1.35 seconds**. The guard losing that race
  means the ROM's init waits forever for a boot device that nobody
  releases. The relabel is a sibling process that waits for the guard on a
  pipe, so the guard is never delayed.

**On the phone:** `run296` is up, the navigation keys are there
(`KEY_BACK`, `KEY_HOMEPAGE`, `KEY_APPSELECT` and `BTN_TOUCH` from the
`generic ft5x06 (3b)` input device), `/dev/ashmem` is there, the adbd
shell is `uid=0` in `u:r:ksu:s0`, and `adsprpcd` is running with
`/dev/adsprpc-smd` open and without its restart loop.

### 4. The audio: the drivers and DT are in, the link is missing legacy symbols

Merged from the `ack-fix` worktree on 2026-10-20: CAF's AFE driver, the
SDM450 soundcard and the machine driver, the SDM660 CDC, the WCD9xxx codec
and the microphone bias and headset control, the CAF ALSA volume API, and
the CAF fastrpc ioctl ABI. The whole `sound/` tree compiles clean with
`W=1`, and `867dfaaac` added the donor's sound card node, routing,
microphone bias and amplifier GPIOs in
`msm8953-xiaomi-tissot-audio.dtsi`.

**There is still no sound image.** The current `build-kernel.sh` leaves the
CAF audio stack out of the build. Enabling it leaves the full kernel
unlinked on undefined `slim_*`, `msm_cdc_*`, `wcd_dsp_mgr_*`,
`audio_ref_clk_*`, `wcdcal_get_fw_cal` and `apr_get_subsys_state` symbols
referenced from `sound/soc/msm/`. The per-object compile is clean; the
missing legacy SLIMBUS/APR/WCD shims are the blocker, not the device tree.

### 5. The capacitive navigation keys: fixed in the tree, and now in the flashed kernel

The volume and power keys work. The three capacitive navigation keys — home,
back and recent — do not, and the reason is not a missing driver.

The repository has both halves of the fix: the `edt,keys` property in
`arch/arm64/boot/dts/qcom/msm8953-xiaomi-tissot.dts` and the
`edt-ft5x06` change that reports the keys the controller reports on its
sensor row below the panel. **Neither was in the flashed kernel.** The
kernel was built from a staged source tree, and that tree was a day older
than the commits. The same staleness kept the CAF fastrpc ioctl ABI out of
the kernel, which is why the `adsprpcd` daemon crash-looped with `failed to
send del client cmd`.

**The staged tree is gone.** The kernel is compiled from this repository
(`make -C . O=out`), so a commit is in the next kernel by construction and
the whole class of "the tree was older than the commits" failures is
closed. The `run289` image carries both fixes: its device tree has
`edt,keys`, and the device reports `KEY_MENU`, `KEY_BACK` and
`KEY_HOMEPAGE` from the touch controller.

### 6. The adsprpcd loop: the CAF ioctl layer is in, one command is missing

The vendor `adsprpcd` speaks CAF's ioctl numbering, not upstream's, and the
port's CAF translation layer is in the running kernel. The daemon still
loops — 60 refusals a boot — but it now refuses **one** command:

```
qcom,fastrpc ...: ioctl nr 8, cmd 0xc0045208, 4-byte payload
qcom,fastrpc ...: CAF ioctl 8 is not implemented
```

Number 8 is CAF's `GETINFO`: the channel id in, the session's SMMU flag
out. The handler for it is committed (`a526ed9a1`), but the image on the
phone — `run294`, built at 11:38 — predates that commit at 12:21, so the
running kernel still refuses it.

**The GETINFO handler is necessary and not sufficient.** The library issues
that command and then asks for attribute 257 through CAF ioctl 17
(`0xc00c5211`, twelve bytes, `{domain, attribute}` in and the value out at
offset 8). The port recognises that command and refuses it.

**What that command is, is now known.** CAF's 2022 and later
`adsprpc_shared.h` has it: number 17 is `GET_DSP_INFO`, one attribute —
the domain, the attribute id, and the capability the kernel answers with.
The ids below 256 are the DSP's own attributes; the ids above it are the
kernel's own capabilities, and 257 is "the driver has the second
generation of the error codes". The implementation is in the tree.

The measurement, the disassembly of the shipped library and the errno
argument are in
[findings/2026-10-20-fastrpc-getinfo-measured.md](findings/2026-10-20-fastrpc-getinfo-measured.md).

### 7. The vendor crash loops: qrtr-ns is fixed, the rest are disabled

The health-check removal stops the `flags_health_check` exec reaction,
not the services that trigger it.  `vendor.per_mgr`/`vendor.per_proxy`
and `qcamerasvr` are the camera peripheral-manager stack, out of scope
for this pass, and are disabled rather than left crash-looping.
`vendor.dataqti` waits for the rmnet/MHI node that only appears once
the modem data path is up; it is stopped until then.

`vendor.qrtr-ns` was a real fix: 6.12's `qrtr_port_assign()` requires
`CAP_NET_ADMIN` for the userspace name-service port
(`net/qrtr/af_qrtr.c:736`), while the ROM grants only `NET_BIND_SERVICE`
(`/vendor/etc/init/hw/init.qcom.rc:409`), and the `qrtr` domain is
denied the capability.  The initramfs overlay grants `NET_ADMIN` and
the policy fragment adds the matching capability allow; the in-kernel
QRTR NS keeps the control port.

**The override now applies, and the crash loop is gone.**  The service
definition lives in the `/vendor` subcontext
(`/vendor/etc/init/hw/init.qcom.rc:405`), so an override in a `/system`
file is rejected by init's Treble check ("overrides another service
across the treble boundary").  The override is therefore installed in
`/vendor/etc/init/qcrild.rc`, which init parses after the definition
(28.889 s vs 29.062 s in the device's own init log).  On the device:

```
service vendor.qrtr-ns /vendor/bin/qrtr-ns -f
    capabilities NET_BIND_SERVICE NET_ADMIN
CapEff: 0000000000001400        # NET_BIND_SERVICE | NET_ADMIN
```

`vendor.qrtr-ns` is started once and never restarts, and the phone no
longer asks for `reboot,bootloader` 330 s into the boot.  The requester
is **adbd**: AOSP's adb watchdog
(`packages/modules/adb/daemon/watchdog.cpp`) runs
`execl("/system/bin/reboot", "/system/bin/reboot", "bootloader", nullptr)`
when no adb client has been connected for the watchdog timeout, and
`/apex/com.android.adbd/bin/adbd` is the only file on the device that
contains the string `bin/reboot`.  With the crash loop gone the reboot
only moves from 330 s to ~1367 s; it is not removed.  The measurements,
the falsified ROM-rescue-party attribution and the thermal findings are
in
[findings/2026-10-21-boot-reset-loop-analysis.md](findings/2026-10-21-boot-reset-loop-analysis.md).

**The CPU has no thermal throttle path on this port.**  The CPU thermal
zones trip only at 80 C (`passive`, mapped to `THERMAL_NO_LIMIT`) and
100 C (`critical`), and only `policy0` registers a cpufreq cooling
device, so the big cluster (cpu4-7, which uses the legacy per-cpu
cpufreq directories) has none.  A Geekbench run therefore reaches the
100 C critical trip, and the port's `kernel/reboot.c`
(`__hw_protection_shutdown()`) forces a poweroff instead of the CPU
backing off.

Two defects remain open and are separate from the reset:
`vendor.bluetooth-1-0-qti` is SIGKILLed by init every ~10 s before it
settles, and the kernel still logs `failed to send del client cmd` once
per second even with the name service alive.

**Why run302 missed it:** `flags_health_check.rc` is in
`/system/etc/init`, which init parses before the vendor files and before
the late guard steps.  The health-check bind was the last pre-policy
step, after the firmware and debuggable passes; it could not land.  The
call is moved next to the early-logcat overlay in
`scripts/initramfs-diag/init.c`.  The evidence, the AOSP import-order
check and the post-fix checks are in
[findings/run302-overlay-not-installed-20261020.md](findings/run302-overlay-not-installed-20261020.md);
the design rationale and the pre-fix evidence remain in
[findings/qrtr-ns-and-crash-loops-20261020.md](findings/qrtr-ns-and-crash-loops-20261020.md).

### 8. Bluetooth and modem: the missing interfaces are implemented

The run334 image (#41, `6.12.93-4k-g9ca2c978871f-dirty`) carries both. On
hardware, Bluetooth is working: `/dev/smd2`/`/dev/smd3` exist under the
`smdpkt` class, the QTI HAL and `com.android.bluetooth` are running,
`dumpsys bluetooth_manager` reports `State: ON`, and the user confirmed the
controller works. The modem is still `offline`: `remoteproc0` is `offline`
with `firmware=mba.mbn` and `gsm.sim.state` is empty. The raw logdump shows
the initramfs child ran: it waited for the modem mount, which appeared at
40.8 s, then failed to set `firmware_class.path` with `errno=13`
(`custom path set=0 errno=13`) and exited before the ADSP/WCNSS rebinds and
the MSS `start`. The modem is offline because the start was never reached.
The phone reset during the first boots; the MSS start never ran, so the
reset is not explained by it, and the cause is unverified. The runtime
diagnosis, the full change list and the run334 evidence are in
[findings/2026-10-21-bt-modem-bringup.md](findings/2026-10-21-bt-modem-bringup.md).

- **Bluetooth** failed because the QTI HAL opens `/dev/smd2` and `/dev/smd3`
  directly, and this port had no `smd_pkt` character driver. The mainline
  `btqcomsmd` path creates `hci0`, which the QTI HAL ignores. The port now
  carries the donor `msm_smd_pkt` driver, the two WCNSS children under a
  `qcom,smdpkt` node, `CONFIG_MSM_SMD_PKT=y`, and `CONFIG_BT_QCOMSMD` forced
  off so the two cannot claim the same channels. The donor's rpmsg signal
  API does not exist upstream; the local TIOCM state and the reset
  notification are preserved.
- **Modem** failed because the MSS (`remoteproc0`) is `auto_boot = false`
  and nothing started it. `harvest_modem_child()` now writes `start` to the
  remoteproc whose firmware is `mba.mbn`, after the existing ADSP/WCNSS
  rebinds and after `firmware_class.path` points at
  `/vendor/firmware_mnt/image`.
- **Policy:** the QTI HAL's observed `qipcrtr_socket` `create` denial is
  allowed. Only `create` is granted; the next AVC names any remaining
  `connect`/`send`/`recv` rule. The `/data/vendor/ssmdump` denial is not
  fixed because no AVC with its types was captured.

The Bluetooth half is now met. The modem half is not: `remoteproc0` is
still `offline`, the MSS QRTR node is absent, `gsm.sim.state` is empty and
there are no `rmnet_data*` interfaces. The initramfs child gets as far as
the modem mount but cannot set `firmware_class.path` (`errno=13`), so the
ADSP/WCNSS rebind and `start` never run. The next step is the firmware-path
credential/permission problem; only after `custom path set=1` can the start
be tested.

## The repository

* The repository holds the documentation and the tooling. The per-run
  evidence — boot images, logdumps, per-run records — is generated,
  git-ignored and out of the tree.
* The boot images and the stock boot backup live outside the repository, in
  `../ack-kgsl-artifacts`, with a README there listing each one and its
  checksum. The stock boot backup is the only copy of the phone's original
  boot partition.
* The build is scripted: `porting/tools/build-kernel.sh`. It builds the
  repository itself, uses `porting/tools/kernel.config` as the
  configuration, and writes to `out/` in the repository, reused, and
  emptied for a clean rebuild.
* The image on the phone is the run334 Bluetooth/modem build (`#41`,
  `6.12.93-4k-g9ca2c978871f-dirty`), built from the `9ca2c9788` tree plus
  the Bluetooth/modem change before that change was committed. It carries
  the navigation keys, the CAF `GETINFO` ioctl, `CONFIG_ZRAM=y`, the TSENS
  zones, the 480-2784 MHz CPU / 725 MHz GPU overclock, the DT2W feature and
  the Bluetooth/modem code. Wi-Fi is working through the runtime `/data`
  staging described in section 9; the built-in NV fix is not in this image.
  Bluetooth is working; the modem is still offline. The previous #40 image
  is preserved as `boot-run335.img`.
  The earlier run294 image was built before the ZRAM fix, so
  `CONFIG_ZRAM` was a module there and `/sys/block/zram0` did not exist on
  the device. The ROM's `init.rc` chowns and chmods
  `/sys/block/zram0/idle` and `/sys/block/zram0/writeback` at boot
  (`system/etc/init/hw/init.rc:1208-1211`), and its `mmd_setup` service
  sets zram up; that is why the port needs `CONFIG_ZRAM=y` and no module
  autoload.

### 8. The CPU/GPU overclock is live on hardware; sustained-load checks remain

The donor's 19-corner 480-2784 MHz CPU table and 725 MHz GPU step are in
`msm8953-xiaomi-tissot-oc.dtsi`, included at the end of the board DTS.

* `apcc-msm8953.c` accepts 480-2784 MHz. The board overlay replaces all four
  `qcom,speed*-bin-v0-{cl,cci}` plans with the donor's 19-corner tables.
* The shared CPU OPP table gains the 12 missing grid points, so it has the
  same 19 rates. `apc_vreg` uses the donor's 19-corner CPR ceilings, floors,
  corner map and misc-fuse margins; `cpr4-apss-regulator.c` already
  extrapolates corners 12-19 above Turbo.
* The GPU gains the 725 MHz Turbo+ power level and the 100 MHz underclock.
  The board's legacy `qcom,gpu-pwrlevels` is replaced with the donor's ten
  levels; initial level is 320 MHz and the CA target is 400 MHz.
* The generic thermal zones stay at 80 C CPU passive, 100 C CPU critical,
  and 70 C GPU passive / 90 C GPU critical. The donor's 88 C
  `deca-cpu-max-step` trip belongs to the CAF thermal driver and is not
  ported.

**Verified in the tree:** `drivers/clk/qcom/apcc-msm8953.o` compiles with
`W=1`, the board DTB builds, the new DTS file is checkpatch-clean, and
`CHECK_DTBS=y` returns 0 (the vendor GPU/battery/ion nodes still have no
schema and produce the pre-existing warnings). The built DTB contains the
19 OPPs, 19-corner CPR tables, 725 MHz GPU level and 480-2784 MHz APCC
plans.

**Verified on hardware (2026-10-20):** the `run304` image booted to Android
(`sys.boot_completed=1`) and is running
`6.12.93-4k-gc6104317ff23-dirty #18`, built 19:55 +0545. Both CPU clusters
expose and run the 19 OPPs from 480 to 2784 MHz; `apcc_cpu0` reads
2784 MHz, `apcc_cci` 1113.6 MHz, and `apc_corner` is 19. The CPR driver
reports speed-bin fuse 2, 19 corners, closed-loop enabled, aging succeeded,
and 1.14 V at the top corner. The GPU exposes 100-725 MHz, has a 725 MHz
maximum, and its clock statistics show time spent at 725 MHz. The eight CPU
zones report 80 C passive / 100 C critical; the GPU zone reports 70 C
passive / 90 C critical. Sampled temperatures were 45-55 C CPU and 41-46 C
GPU (an earlier sample reached 74 C CPU / 52.8 C GPU); both cooling devices
were at state 0. No critical trip, panic or pstore record was present.

**CPU thermal trip test (2026-10-20):** eight busy loops on the device.
The CPU zones crossed the 80 C passive trip; at the 91.6 C peak the CPU
cooling device moved to state 4 and `scaling_max_freq` fell from 2784 to
2304 MHz. The test script's 90 C abort threshold stopped the load at that
point, and the peak stayed below the 100 C critical trip. After the load
stopped, the cooling device returned to state 0, `scaling_max_freq`
returned to 2784 MHz, and the hottest zone fell to 67.4 C within 10 s
(44-49 C at the final check). The GPU reached 65.2 C during the CPU test
but stayed below its 70 C passive trip. No panic or pstore record was
present. Log: `porting/artifacts/snapshot60/run304-thermal-stress.log`.

**Still unverified:** the GPU passive trip and both critical trips. There
was no GPU workload tool on the device, and the CPU test stopped before
the 100 C critical trip. The boot dmesg ring had already rolled over, so
this is live-sysfs evidence, not a full boot log.

**Build and package:** `run304` links successfully with the current
no-audio configuration. Its first package attempt failed because
`package-diag-boot.py` added duplicate `lib` and `lib/firmware` directory
records over the base ramdisk. The packager now reuses existing parent
directories and refuses conflicting firmware files. `boot-run303.img` and
`boot-run304.img` were generated after the fix: 45,799,424 bytes each, 696
unique cpio records, 11 WCNSS firmware files. `run304` has since booted;
`run303` carries the same kernel payload. The `run303-build.status` and
`run304-build.status` files still say `status=1` because they record the
original failed packaging attempt; the images were repackaged separately.

### 9. Wi-Fi: the NV blob is loaded after switch_root

`wlan0` exists on run304 with a valid serial-derived MAC, but every
attempt to bring it up ends before association.  `wcn36xx` is bound and
`remoteproc1` runs `wcnss.mdt`; the failure is the NV upload:

```
wcn36xx: ERROR Failed to load nv file wlan/prima/WCNSS_qcom_wlan_nv.bin: -2
wcn36xx: ERROR Failed to push NV to chip
loading /vendor/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin failed with error -13
Direct firmware load for wlan/prima/WCNSS_qcom_wlan_nv.bin failed with error -2
```

The request happens when the interface is opened, long after
`switch_root`, so the copy that packaging puts in the ramdisk at
`lib/firmware` is no longer mounted.  The vendor tree has a symlink at
`/vendor/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin`, but it points at
`/mnt/vendor/persist`, and `request_firmware()` does the read with kernel
credentials (`prepare_kernel_cred(&init_task)`), not the caller's.  The
policy denies the kernel domain search on `mnt_vendor_file`:

```
avc: denied { search } for comm="ip" name="vendor" dev="tmpfs"
  scontext=u:r:kernel:s0 tcontext=u:object_r:mnt_vendor_file:s0
```

The path was verified on run304 by copying the blob to
`/data/vendor/wifi/firmware/wlan/prima/` and adding that directory to
`firmware_class.path`.  The load was confirmed with SELinux permissive;
after returning to enforcing with the firmware already loaded, `wlan0`
associated to the 5 GHz "Private" AP and received `192.168.1.85`
(`cmd wifi status`, `ip addr show wlan0`).  That proves the file and the
path, but not a clean enforcing load, and the runtime directory does not
survive a reboot.

Commit `7c1515865` embeds only
`wlan/prima/WCNSS_qcom_wlan_nv.bin` with `CONFIG_EXTRA_FIRMWARE`.  The
firmware loader checks built-in firmware before any filesystem path in
`_request_firmware_prepare()`, which removes both the switch_root timing
and the SELinux denial from the NV load.  `wcnss.mdt` and its segments
are still packaged into the ramdisk because the remoteproc requests them
before `/vendor` is mounted.  The next clean boot has to confirm this.

### 10. The CPU stays at 2784 MHz because `system_server` is busy

The governor is not holding the maximum.  The policy is one shared
`schedutil` over CPUs 0-7, `scaling_min_freq` is 480000,
`scaling_max_freq` is 2784000, and a sample during the investigation
showed individual CPUs at 1804/2304/2784 MHz while the load moved.  There
is real work on the device:

* `system_server` reached 87% of one CPU in `top`; `dumpsys cpuinfo`
  attributed 47% to it over the sample.
* `dumpsys media.audio_policy` reports `AudioPolicyManager: 0x0`,
  `listAudioPorts n=24458` and `setMinVolumeIndexForGroup n=1115`.
  `AS.AudioService` logs `Failed to init min/max for volume group -1 for
  stream 11` about every two seconds.
* `/proc/asound/cards` reports no soundcards, `/sys/bus/apr` does not
  exist and `/sys/bus/slimbus/devices` is empty, while `adsp.mdt` is
  running and the audio HAL and `audioserver` are alive.  This is the
  audio-stack gap in section 4: the policy manager cannot be created, so
  the framework keeps retrying port and volume discovery.

**Stopping `audioserver` alone** (leaving `vendor.audio-hal` running)
dropped `system_server` from 87% to 9.6%, idle from 616% to 716%, and
load average from 5.1 to 2.52; the max-frequency share in
`time_in_state` fell from 71% of wall time over 30 s to about 25% over
8 s.  Starting `audioserver` again restored the loop.  So the audio retry
is the dominant CPU consumer, not the governor, and the single shared
`schedutil` policy then takes every CPU to 2784 MHz because one policy
sees the busy core.  This is a real framework cost of the missing audio
stack, not something to fix by editing OPPs or governor tunables.

The other reports are real but secondary.  `libperfmgr` cannot write
`/sys/class/devfreq/soc:qcom,cpu-cpu-ddr-bw/min_freq` (no DDR bandwidth
devfreq device); that is a DDR gap, not CPU frequency.  The DT2W gap is
closed: `edt-ft5x06` now provides
`/proc/sys/dev/xiaomi_msm8953_touchscreen/enable_dt2w` for panels with
`edt,gesture-wakeup`, and run334a verified the ROM toggle's write and a
double-tap wake (`porting/findings/2026-10-21-dt2w-hardware.md`).  `vendor.qti.data.factory` is missing and is restarted as a
lazy HAL, `vendor.qrtr-ns` no longer exits but the kernel still logs
`failed to send del client cmd`, and DevCheck's reads of `cur_freq` are
denied and produce
AVC lines.  Those should be cleaned up, but they do not explain the
audio-driven `system_server` load.

Consequently, do not tune `schedutil` or the OPP table to make the idle
frequency fall.  The actionable item is the audio link (section 4), or a
deliberate no-audio configuration that stops the framework retry.

### 11. The audio playback works; the low latency path and the hardware video are not there yet

**The speaker path works** (run344, run346): the ROM's audio HAL opens the
deep buffer stream with the same parameters as on the donor
(`app_type 69936`, `acdb_dev_id 1058`, `snd_device_be_idx 52`, the mixer
path `deep-buffer-playback speaker`), the DEEP_BUFFER thread of
`dumpsys media.audio_flinger` writes frames (310080 frames in 6.5 s), and
the kernel log of a full playback run has no warning, no BUG and no call
trace. The two fixes that got it there were the `dma_buf_vunmap()`
argument type (`d67199a80`) and the `copy_{from,to}_iter()` return value
(`b250002a2`).

**Two more bugs of the same class came out of the boot that tested them**
(`fdfca326d`, `4698570a2`): the q6 platform's delay callback asked the DSP
for the path delay from the pointer path, which is under the PCM stream
lock with interrupts disabled - a "BUG: scheduling while atomic" that froze
the phone half a second into the first playback; and a fortify warning
about a copy into a zero-length array member that cannot overflow.

**The low latency path is still broken, and the reason was only found
afterwards**: the vendor HAL opens the front end by PCM device *number*,
and the port's dai link table had a different order than the donor's, so
the low latency usecase opened the MultiMedia9 front end (number 12 on
the port) instead of MultiMedia5 (number 12 on the donor), and then wrote
the mixer control for MultiMedia5. The Media9 front end has no route to a
back end, so the prepare failed and the HAL logged "pcm_prepare returned
-1" (`ecad3d95c`).

**The hardware video decode is not there yet.** The ROM's OMX service
needs CAF's `vidc_3x`; the driver, its governors and its user space
interface are in the tree, and the user space interface is checked
against the donor's with a script. What is left is the bus bandwidth
voting, the power domain, the device tree, the switch-over from the
upstream Venus driver, and the boot test.

## The order of the remaining work

1. Build **run305** with `7c1515865`, which embeds the WLAN NV blob, and
   boot it from a clean enforcing start.  Check that `wlan0` comes up
   without the `-13`/`-2` NV errors and that it associates.  The same
   build should keep the run302 health-check overlay list (`errno=0` for
   the override, no `UPDATABLE_CRASHING` exec, qrtr-ns running, camera
   and dataqti stopped), the wakeup labels, `/sys/block/zram0` and the
   navigation keys.
2. Read the next boot's `adsprpcd` result: with `GETINFO` and ioctl 17
   both answered, the library should stop asking, and whatever it asks
   for next will be named in the kernel log.
3. Finish the CAF audio link: the device tree is in, but enabling the
   stack still needs the legacy SLIMBUS/APR/WCD shims; then make sound
   work.
4. Catch a GPU stall with the watcher and read the ME_INIT spin.
5. The CPU passive trip is verified (section 8): it throttled and
   recovered. The GPU passive trip and both critical trips are still
   unexercised; a GPU workload is needed before claiming the GPU path, and
   the critical trips need a deliberate safety plan.
6. The overclock is live on hardware: `cpufreq` exposes 480-2784 MHz and is
   running at 2784, APCC and CPR corner 19 are active, and KGSL exposes and
   has used the 725 MHz level. CPU sustained-load thermal behaviour is now
   verified; GPU sustained-load validation remains.
