# Current state of the tissot port — 2026-10-20

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

**Do not use the manager's grant-root flow until the fix is in an image.** The
path that crashed is `SET_APP_PROFILE`, not `KSU_IOCTL_SET_SEPOLICY`. It calls
`ksu_mark_running_process_locked()`, which walks every thread under
`tracepoint_reg_lock` with IRQs disabled and used to print one `tp_marker:`
line per thread. On this phone (4,211 threads) that was ~5,000 synchronous
printk lines; the watchdog declared a 25 s soft lockup at
`do_set_app_profile` and panicked the kernel
(`porting/findings/ksu-grant-root-softlockup-20261020.md`). The local fix
removes the per-thread logging and keeps one summary line. It is **not yet
verified on device**: rebuild, flash, grant root to an app, and check for the
summary line and no soft lockup. The adb root feature is unaffected.

## What works

| Area | State | Evidence |
|---|---|---|
| Kernel boots, Android comes up | Working | every run from 234 on |
| CPU clock policy | Working | one shared `schedutil` policy over CPUs 0-7, 652.8-2016 MHz, the APCC handoff completes, no failed transitions |
| Display | Working | the panel is driven; the homescreen renders |
| GPU | Working, with one open fault | `/dev/kgsl-3d0` is driven with the stock ioctl ABI; the retirement stall below is open |
| Touch | Working | the `edt-ft5x06` panel, the multi-touch slots |
| Volume and power keys | Working | the PMIC's pwrkey and resin, and a gpio-keys node |
| Capacitive navigation keys | Working | the touch controller's key row reports `KEY_MENU`, `KEY_BACK` and `KEY_HOMEPAGE`; the recents key is `KEY_APPSELECT` (580) |
| Hall sensor | Working | `SW_LID` on the gpio-keys device |
| Root | Working | KernelSU-Next's adb root, `uid=0` / `u:r:ksu:s0` |
| Log capture | Working | the `logdump` partition, the USB console, the ramoops sampler |
| The checkpoint reboot loop | Fix committed, not in an image yet | the wakeup statistics are relabelled `sysfs_wakeup` by the initramdisk's relabel, which had no `/sys` to walk and so never labelled anything; the fix mounts one |
| The `adsprpcd` CAF ioctl layer | In the kernel, one command missing | the port decodes CAF's ioctl numbering; on the phone the daemon now fails on exactly one command, `GETINFO`, whose handler is committed but not in the flashed image |
| Wi-Fi, cellular, cameras, Bluetooth | Failing | see `FAILING-COMPONENTS.md`; unchanged by this session's work |

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

### 2. The CPU has no thermal protection

The clock policy works. What is missing is anything that can throttle it.

* `CONFIG_QCOM_TSENS` is not set **in the built configuration**. The
  defconfig says `=y`, but the build never reads the defconfig: the
  configuration is `porting/tools/kernel.config`, and that file has it
  off.
* The CPU nodes have **no `#cooling-cells`**, so `&CPU0` is not a cooling
  device and the four CPU thermal zones in the device tree have nothing to
  throttle. The donor has `#cooling-cells = <2>` on all eight CPU nodes.

On the running phone there are three thermal zones — `pm8953-thermal`,
`battery`, `bms` — and **no CPU zones at all**. The policy therefore sits at
the top OPP (1.958-2.016 GHz) and the phone dies of heat: run261 at about
326 s, run265 at about 581 s.

The CPU burn is not the governor. The load is uninterruptible sleep — the
fence waiters — and the binder storm is PowerManagerService raising SIGBUS
on itself. The frequency is a symptom.

Fixing it is two changes: put `CONFIG_QCOM_TSENS=y` where it survives (the
build script's forced symbols, not the defconfig) and add `#cooling-cells` to
the eight CPU nodes, with the OPP `cooling-device` link.

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

### 4. The audio: the drivers are in, the device tree is not

Merged from the `ack-fix` worktree on 2026-10-20: CAF's AFE driver, the
SDM450 soundcard and the machine driver, the SDM660 CDC, the WCD9xxx codec
and the microphone bias and headset control, the CAF ALSA volume API, and
the CAF fastrpc ioctl ABI. The whole `sound/` tree compiles clean with
`W=1`.

**There is still no sound**, because the port's device tree has no sound
card node and no audio pinctrl: the machine driver has nothing to bind to.
The donor has the template —
`oc-test/arch/arm64/boot/dts/vendor/qcom/mi8953/tissot/audio.dtsi`, the
`int_codec: sound` node with the audio routing, the microphone bias
properties, the CDC pinctrl states and the external amplifier's GPIO. The
audio symbols are enabled in the build so that a boot shows whether the
drivers probe.

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

### 7. The vendor crash loops: qrtr-ns is fixed, camera is parked

The health-check removal stops the `flags_health_check` exec reaction,
not the services that trigger it.  The current boot still had
`sys.init.updatable_crashing=1` and init restarting `vendor.qrtr-ns`,
`vendor.per_mgr`, `vendor.per_proxy`, `qcamerasvr` and `vendor.dataqti`.

`vendor.qrtr-ns` is a real fix: 6.12's `qrtr_port_assign()` requires
`CAP_NET_ADMIN` for the userspace name-service port
(`net/qrtr/af_qrtr.c:736`), while the ROM grants only `NET_BIND_SERVICE`
(`/vendor/etc/init/hw/init.qcom.rc:409`), and the `qrtr` domain is
denied the capability.  The initramfs overlay grants `NET_ADMIN` and
the policy fragment adds the matching capability allow; the in-kernel
QRTR NS keeps the control port.

`vendor.per_mgr`/`vendor.per_proxy` and `qcamerasvr` are the camera
peripheral-manager stack, out of scope for this pass, and are disabled
rather than left crash-looping.  `vendor.dataqti` waits for the
rmnet/MHI node that only appears once the modem data path is up; it is
stopped until then.  The evidence and the verification plan are in
[findings/qrtr-ns-and-crash-loops-20261020.md](findings/qrtr-ns-and-crash-loops-20261020.md).

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
* The image on the phone is **run294** (flashed at 12:07, booted at 12:08
  on 2026-10-20). Its kernel carries the navigation keys and the CAF
  `GETINFO` ioctl, but it was built before the ZRAM fix, so
  `CONFIG_ZRAM` is a module in it and `/sys/block/zram0` does not exist on
  the device. The ROM's `init.rc` chowns and chmods
  `/sys/block/zram0/idle` and `/sys/block/zram0/writeback` at boot
  (`system/etc/init/hw/init.rc:1208-1211`), and its `mmd_setup` service
  sets zram up, so both fail on the running kernel. The next build has
  `CONFIG_ZRAM=y`, which is what the ROM needs: the port has no module
  autoload, so a modular zram means the device is never there.

## The order of the remaining work

1. Rebuild with the relabel fix and `CONFIG_ZRAM=y`, flash it to
   `boot_b`, and check on the device: the wakeup nodes labelled
   `sysfs_wakeup`, the `system_suspend` denials at 0, `/sys/block/zram0`
   present, and the navigation keys.
2. Read the next boot's `adsprpcd` result: with `GETINFO` and ioctl 17
   both answered, the library should stop asking, and whatever it asks
   for next will be named in the kernel log.
3. Write the sound card's device tree from the donor's `audio.dtsi` and
   make sound work.
4. Catch a GPU stall with the watcher and read the ME_INIT spin.
5. Add the CPU thermal path: TSENS in the built configuration, and
   `#cooling-cells` on the CPU nodes.
