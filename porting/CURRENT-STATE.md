# Current state of the tissot port — 2026-10-20

This is the authoritative statement of what the port does today. Every other
document in `porting/` is either evidence for a claim made here or the
historical log of how the port got here; the dated run reports are not
rewritten, they are what happened.

* The build, the packaging and the flash procedure: [BUIL_TISSOT.md](../BUIL_TISSOT.md),
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

**Do not use the manager's grant-root flow.** The live-policy rewrite behind
it (`KSU_IOCTL_SET_SEPOLICY`, which duplicates and rewrites the whole
policydb in the kernel) **deadlocks this kernel**: it froze the phone on the
first attempt, and the manager's "allow an app root" does the same thing.
The adb root feature is fine; the allowlist path is not.

## What works

| Area | State | Evidence |
|---|---|---|
| Kernel boots, Android comes up | Working | every run from 234 on |
| CPU clock policy | Working | one shared `schedutil` policy over CPUs 0-7, 652.8-2016 MHz, the APCC handoff completes, no failed transitions |
| Display | Working | the panel is driven; the homescreen renders |
| GPU | Working, with one open fault | `/dev/kgsl-3d0` is driven with the stock ioctl ABI; the retirement stall below is open |
| Touch | Working | the `edt-ft5x06` panel, the multi-touch slots |
| Volume and power keys | Working | the PMIC's pwrkey and resin, and a gpio-keys node |
| Hall sensor | Working | `SW_LID` on the gpio-keys device |
| Root | Working | KernelSU-Next's adb root, `uid=0` / `u:r:ksu:s0` |
| Log capture | Working | the `logdump` partition, the USB console, the ramoops sampler |
| Wi-Fi, cellular, cameras, Bluetooth | Failing | see `FAILING-COMPONENTS.md`; unchanged by this session's work |

## What is open, and what is known about it

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
  defconfig says `=y`, but the build never reads the defconfig: it takes
  the configuration from the previous image's embedded configuration, so
  that line is dead code.
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

### 3. The early hang is a reboot that never finishes

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

The reason is in the same logdump: **460 SELinux denials** for the
`system_suspend` domain reading the wakeup statistics. The ROM's policy was
written for the 4.19 CAF kernel, where the statistics lived under
`/sys/devices/virtual/wakeup`; the 6.12 kernel puts them per device, so
they land on the generic `sysfs` type and the suspend service is denied.
The snapshot fails, the checkpoint commit fails, and the phone asks for a
reboot it cannot finish.

**The fix is a label, not a policy change.** The ROM's policy already has
the type it expects for them (`sysfs_wakeup`), and the port's own base
policy has the matching `genfscon`. Relabelling the nodes by hand, live,
made the denials stop, and the boot that followed had no "Checkpoint commit
failed" and no reboot request.

Two constraints on doing it in the initramdisk, both learned the hard way:

* The relabel must run in the **initramdisk's own mount namespace**, before
  the guard chroots into the ROM's root, where `/sys` is not the kernel's.
  In `run282` the relabel ran after the chroot and reported `ret=-1`: the
  walk could not open its directory, and the labels never changed.
* It must **not delay the bootdevice guard**. `run283` put the relabel in
  front of the guard and the phone came up on a dim lit screen with no adb
  and no USB. The guard has to install the read-only fstab and the vendor
  contexts *before* the ROM's init opens its SELinux policy, and the window
  between the two is about **1.35 seconds**. The guard losing that race
  means the ROM's init waits forever for a boot device that nobody
  releases. The fix is a separate process that relabels in parallel with
  the guard, not a delay in front of it.

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

### 5. The capacitive navigation keys: fixed in the tree, not in the flashed kernel

The volume and power keys work. The three capacitive navigation keys — home,
back and recent — do not, and the reason is not a missing driver.

The repository has both halves of the fix: the `edt,keys` property in
`arch/arm64/boot/dts/qcom/msm8953-xiaomi-tissot.dts` and the
`edt-ft5x06` change that reports the keys the controller reports on its
sensor row below the panel. **Neither was in the flashed kernel.** The
kernel is built from a staged source tree, and that tree was a day older
than the commits. The same staleness kept the CAF fastrpc ioctl ABI out of
the kernel, which is why the `adsprpcd` daemon crash-looped with `failed to
send del client cmd`.

The build script now refreshes the staged tree and verifies it against the
repository before every build, and prints what it staged. `run289` carries
both fixes.

## The repository

* The repository holds the documentation and the tooling. The per-run
  evidence — boot images, logdumps, per-run records — is generated,
  git-ignored and out of the tree.
* The boot images and the stock boot backup live outside the repository, in
  `../ack-kgsl-artifacts`, with a README there listing each one and its
  checksum. The stock boot backup is the only copy of the phone's original
  boot partition.
* The build is scripted: `porting/tools/build-kernel.sh`.
* The staged source tree is `../ack-staged-run239`; the output directory is
  `out/` in the repository, reused, and emptied for a clean rebuild.

## The order of the remaining work

1. Build and flash `run289`; check the navigation keys and that the
   `adsprpcd` crash loop has stopped.
2. Write the sound card's device tree from the donor's `audio.dtsi` and
   make sound work.
3. Catch a GPU stall with the watcher and read the ME_INIT spin.
4. Add the CPU thermal path: TSENS in the built configuration, and
   `#cooling-cells` on the CPU nodes.
5. Relabel the wakeup statistics from a separate process in the initramdisk
   and end the checkpoint reboot.
