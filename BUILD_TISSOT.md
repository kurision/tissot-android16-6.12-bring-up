# Building and boot-testing the port

This is the entry point for building this port on a Linux host. Read
`porting/REFERENCE-boot-testing.md` before a boot. Do not build or boot
without the user's authorization.

The port kernel is **flashed to the boot partition**, not RAM-booted: it
lives on slot b and the phone comes back on it by itself after a power
cycle. The RAM-boot tooling in `porting/logging/` still works and is still
the way to observe a boot, but the image under test is normally written to
`boot_b`.

**The whole flow is scripted**: `porting/tools/build-kernel.sh` resolves the
toolchain, writes the configuration, builds the kernel out of the
repository, checks the device tree, rebuilds the ramdisk's `/init`,
packages the result and verifies it. Read this document to know what it
does and why; run it rather than the individual steps.

```bash
bash porting/tools/build-kernel.sh run289      # an unused run number
```

The run name is only a label: the script refuses to overwrite the outputs
of an existing run, so pick a new one for every build.

## Layout: what is where

Every path below is relative to the repository root.

| What | Where |
|---|---|
| The repository | `.` — the kernel is built from it, so a fresh clone builds |
| The port's kernel configuration | `porting/tools/kernel.config` |
| The build script | `porting/tools/build-kernel.sh` |
| The output directory (one, reused) | `out/` |
| The per-run evidence and the build log | `porting/artifacts/snapshot60/` |
| The boot images, the stock boot backup | `../ack-kgsl-artifacts/` |
| The documentation | `porting/`, this file, `porting/findings/` |

The images are far too large for git, so they live outside the repository in
`../ack-kgsl-artifacts`, with a README there that lists each one and its
checksum. `stock-boot-live.bin` in that directory is **the only copy of the
phone's original boot partition**. Do not delete it.

## The toolchain

Nothing is hardcoded: the script finds the tools and makes every path
absolute, so a fresh clone builds on a host that has them installed.

| Tool | How it is found | Version used for the known good images |
|---|---|---|
| C compiler | `clang` on the PATH | 23.1.3 |
| Rust compiler | `rustup which rustc`, then the PATH | 1.99.0 (LLVM 23.1.1) |
| Rust standard library sources | `$toolchain/lib/rustlib/src/rust/library` | same toolchain |
| bindgen | the PATH, then the toolchain | 0.73.2 |
| libclang | `LIBCLANG_PATH`, else `/usr/lib` and friends | 23.1.3 |
| aarch64 cross compiler | `aarch64-linux-gnu-gcc` on the PATH | the distribution's |
| Python | `python3` on the PATH | 3.x |

A build with a different clang produces a different kernel, so record the
version in the run's notes. The stable toolchain directory can change
after a rustup update; verify the versions every time.

The Rust standard library sources are not part of a default rustup
toolchain:

```bash
rustup component add rust-src
```

bindgen is not part of a rustup toolchain either; it is installed
separately:

```bash
cargo install bindgen --version 0.73.2
```

A plain `rustc` in an interactive shell resolves through `~/.cargo/bin`,
which is not necessarily the toolchain the known build used. The script
resolves the toolchain itself and exports absolute paths; if the
resolution is wrong, override it:

```bash
export RUST_TOOLCHAIN=/path/to/rustup/toolchain
export RUSTC="$RUST_TOOLCHAIN/bin/rustc"
export HOSTRUSTC="$RUSTC"
export BINDGEN=/path/to/bindgen
export RUST_LIB_SRC="$RUST_TOOLCHAIN/lib/rustlib/src/rust/library"
export LIBCLANG_PATH=/usr/lib
"$RUSTC" -vV
"$BINDGEN" --version
test -r "$RUST_LIB_SRC/core/src/lib.rs"
test -r "$LIBCLANG_PATH/libclang.so"
```

The script honours these and a few more: `ACK_TISSOT_BASE_IMAGE`,
`ACK_SNAPSHOT_DIR`, `CLANG`, `CROSS_CC`, `LLVM_NM` and
`RUST_PROFILE_FLAGS`. Its own header lists them all.

Rust 1.99 renamed `-Zdebug-info-for-profiling`. The existing AutoFDO
settings are preserved with a make-command override, without editing
kernel sources:

```bash
RUST_PROFILE_FLAGS='-Zdebuginfo-for-profiling -Cllvm-args=-enable-fs-discriminator=true -Cllvm-args=-improved-fs-discriminator=true'
```

Adding the new spelling through KRUSTFLAGS does not remove the old option
and still fails.

## The base image

The packaging step takes a **real** boot image as its base: its ramdisk
and its command line are what the result carries. The base is not
redistributable, so it is not in the repository. Supply it as the second
argument, or in `ACK_TISSOT_BASE_IMAGE`:

```bash
bash porting/tools/build-kernel.sh run289 ../ack-kgsl-artifacts/boot-run258.img
```

**Never reconstruct the base.** The diag ramdisk carries a patched SELinux
policy (`userdebug_plat_sepolicy.cil`) and a `force_debuggable` trigger
that nothing in this tree regenerates. A base rebuilt from a stock
`boot_a`, or from a captured fstab, packages cleanly, boots, and then
stalls in userspace several stages later than it should — which reads
exactly like a kernel bug and is not one. If a base image is needed,
extract a real one from git history and change only what is being tested.

## The configuration

The configuration is not the defconfig and not whatever the last build
left behind. `arch/arm64/configs/defconfig` is dead code here: the
port's configuration is `porting/tools/kernel.config`, and that file is
the source of truth for every build. The script copies it into `out/.config`
and resolves it there.

Two things about the resolution are not optional:

* **`make rustavailable` runs before `olddefconfig`.** RUST is off until
  the probe has run and ashmem depends on RUST, so a configuration
  resolved without the probe loses both *while the C kernel still builds
  successfully* — and the framework then loses `/dev/ashmem`.
* **The port's own symbols are forced afterwards.** The configuration is
  the port's, so the script is the only place they can be turned on.

| Symbol | Why |
|---|---|
| `CONFIG_KSU` | KernelSU-Next is built in, and the manager only talks to a driver that reports the released version (33355, pinned with `KSU_GIT_VERSION=3355 KSU_GIT_VERSION_VALID=1 KSU_GIT_TAG=v3.4.2`). |
| `CONFIG_SND_SOC_MSM8952`, `CONFIG_SND_SOC_MSM_WCD9335`, `CONFIG_SND_SOC_MSM_WCD_MBHC`, `CONFIG_SND_SOC_WCD_MBHC_ADC`, `CONFIG_WCD9XXX_CODEC_CORE`, `CONFIG_SND_SOC_SDM660_CDC` | The audio port: the AFE, the WCD9335 codec, the microphone bias and headset control, and the machine driver. |
| `CONFIG_SLIMBUS`, `CONFIG_SLIM_QCOM_CTRL` | The codec is a slimbus slave, and the tree's slimbus subsystem is off by default. |
| `CONFIG_ZRAM` | The ROM's own `init.rc` already does `chown root system /sys/block/zram0/idle` and the same for writeback, so the ROM expects a zram device to be there and sizes it itself. It has to be built in: the port has no module autoload, so a modular zram means `/sys/block/zram0` never exists. |

The audio symbols are on so that a boot shows whether the drivers probe.
The sound card's device tree is not written yet, so **they will not make
sound**: the port's device tree has no sound card node and no audio
pinctrl, and the donor
(`oc-test/arch/arm64/boot/dts/vendor/qcom/mi8953/tissot/audio.dtsi`) has
the template.

Two upstream symbols must stay **off**: `CONFIG_SND_SOC_WCD9335` and
`CONFIG_SND_SOC_WCD_MBHC`. The port's own copies of the codec and the
microphone bias define the same symbols, and the tree already carries the
upstream ones. The script fails the build if either is on.

The script checks the required symbols after the final `olddefconfig` and
fails loudly if any of them is missing, and it writes the configuration
difference to `porting/artifacts/snapshot60/<run>-config.diff` so the
change is visible.

## The output directory: one, reused

There is one output directory, `out/` in the repository, and it is reused.
A clean rebuild means emptying it, not making a new one:

```bash
rm -rf out/* out/.[!.]*
```

Do not use bare make with an unpinned environment against it: a
configuration resync with the wrong toolchain can remove Rust and ashmem
while the C kernel still builds successfully, and the framework then loses
`/dev/ashmem`. The build script's `rustavailable` and required-symbol
guards exist to prevent this.

The kernel is built **from this repository** (`make -C . O=out`). There is
no staged tree: what is committed is what is compiled. A stale staged tree
once shipped an image without the repository's fixes, which is why the
staged-tree step is gone rather than fixed.

## The build

The script builds the port's own objects with `W=1` (the kgsl and the
ported CAF audio are vendor code that does not satisfy it), builds
KernelSU-Next, and then the image:

```bash
make -C . O=out ARCH=arm64 LLVM=1 LLVM_IAS=1 CC=clang Image.gz
```

The device tree is checked, not just compiled: the binding for the APCC
clock is validated with `dt_binding_check`, and the board's dtb is built
with `CHECK_DTBS=y`.

The `/init` is rebuilt from source every time, so the image never carries
a stale init binary:

```bash
aarch64-linux-gnu-gcc -Os -static -DANDROID_CAPTURE -DANDROID_EARLY_LOGCAT \
  -DBOOT_DEADLINE_SECONDS=0 scripts/initramfs-diag/init.c \
  -o porting/artifacts/snapshot60/init-<run>
```

## Packaging and verification

The base image's ramdisk and command line, this kernel and this init are
combined by `porting/logging/package-diag-boot.py`, which writes the
image and a report with the payload hashes:

```bash
python3 -B porting/logging/package-diag-boot.py \
  --base-image "$BASE" --kernel-dir porting/artifacts/snapshot60/kdir-<run> \
  --stage-kernel out/arch/arm64/boot --init porting/artifacts/snapshot60/init-<run> \
  --keep-stock-console --out porting/artifacts/snapshot60/boot-<run>.img \
  --report porting/artifacts/snapshot60/package-<run>.json
```

Verify the package report and the **shipped** embedded config, not just
the output-tree config — the kernel embeds its own configuration, and
that is the one that runs:

```bash
scripts/extract-ikconfig porting/artifacts/snapshot60/kdir-<run>/Image.gz \
  > porting/artifacts/snapshot60/<run>-embedded.config
```

The script does this itself and fails the run if a required symbol is
missing from the embedded config, if the diagnostic symbols are not in
`out/vmlinux`, or if the port's configuration is not what it should be.

For a kernel-only test, the ramdisk hash, the init hash, the command line
and the DTB must match the chosen base unless an explicit change was
approved. Do not append inherited policy or firmware and do not repeat
`--debuggable` without a reason. Preserve `ramoops.reset_capture=1`; do
not restore `panic=0`.

## Flash it

The partition is 64 MiB and the image is not, so pad it:

```bash
cd porting/artifacts/snapshot60
python3 -c "
import os
sz=os.path.getsize('boot-<run>.img')
open('boot-<run>-padded.img','wb').write(
    open('boot-<run>.img','rb').read()+b'\0'*(67108864-sz))"
adb reboot bootloader
fastboot getvar current-slot                     # slot b
fastboot flash boot_b boot-<run>-padded.img
fastboot reboot
```

Add `-s <serial>` to `adb` and `fastboot` when more than one device is
attached.

The image that is currently on the phone is in the archive
(`boot-run282-padded.img`); flash that back to return to the last known
good kernel.

## Preserve evidence and observe

1. Confirm the selected serial, that the phone boots the port kernel
   (`uname -r` is 6.12.93-4k), and that the adb shell is root (`id`
   shows uid=0 and `u:r:ksu:s0`). The root comes from KernelSU-Next,
   which is built into the kernel; adbd as `shell` cannot read the kgsl
   debugfs, which is where the GPU's retired count is.
2. Ensure no old USB reader or logcat observer remains. Archive
   early-logcat files and verify the device logdump SHA against a
   preserved 64 MiB local copy. Never clear the only evidence. Leaving
   logdump uncleared requires comparing the new capture against that
   baseline and excluding stale tails.
3. Verify pyserial: `python3 -B -c 'import serial; print(serial.__version__)'`.
   A virtual environment is ephemeral; do not assume it survives.
4. Start USB capture before the boot. Save exact commands, return codes
   and timestamps. Poll ADB every four seconds, including uname, boot ID,
   uptime, boot_completed and process IDs. Capture logcat continuously
   once the port appears.
5. Observe up to eight minutes unless it demonstrably returns to stock.
   Confirm `/dev/ashmem` exists. Boot completion on stock is not a port
   result.
6. Ask for physical panel observation; do not use screencap as proof of
   visible output on this device.

`usb-boot-capture.py` always starts with `adb reboot bootloader` and
therefore cannot start from an already-fastboot device. Bound its
lifetime: its reader can wait indefinitely when no diagnostic gadget
appears. An empty USB log, an ACM disconnect, or fastboot OKAY alone does
not establish whether the kernel booted.

Distinguish these two failures in the host log:

* If `adb reboot bootloader` returns success but `fastboot devices`
  remains empty, the image has not been sent. Check USB enumeration and
  the physical screen; allow a bounded enumeration window before calling
  the transition a failure.
* If `fastboot boot` logs Sending and Booting OKAY, the bootloader
  accepted the command. Establish kernel execution from a fresh banner or
  boot ID; the splash image alone cannot distinguish a bootloader wait
  from an early kernel failure.

For an already-fastboot device, `fastboot-boot-capture.py` skips the ADB
reboot step and has `--boot-timeout`. Stop its reader before a later
boot. To investigate orchestration separately, start a reader first and
record explicit fastboot commands and their responses.

For a repeat from stock, keep live ADB observation in another terminal,
then use:

```bash
adb reboot bootloader
timeout 600 python3 -B porting/logging/fastboot-boot-capture.py \
  --image porting/artifacts/snapshot60/boot-<run>.img \
  --log porting/artifacts/snapshot60/<run>-usb.log \
  --status porting/artifacts/snapshot60/<run>-usb-capture.status \
  --boot-timeout 480
```

The helper waits up to 60 seconds for fastboot. Capture can finish at the
USB handoff, before the eight-minute device observation ends. Continue
the ADB observation independently. If the phone is already in fastboot,
skip the adb reboot command. Never run a second reader against the same
serial port.

## Recover and interpret

Save live evidence before returning the phone to the previous image. If a
reboot is needed for persistent recovery, stop capture children first.
Confirm stock identity and root before:

```bash
cd porting/artifacts/snapshot60
bash ../../logging/pull-run-evidence.sh "<run>" "boot-<run>.img"
python3 -B ../../logging/decode-reset.py "<run>-logdump.bin"
```

The recovery script refuses to overwrite existing evidence. Compare image
payload hashes and boot IDs; check snapshot timestamps and stale capture
tails. CRC validation proves stored bytes, not boot freshness.

**A hung kernel is recovered by forcing the phone off** (long press on
the power button) and flashing the previous image from fastboot. The
phone comes back on the port kernel by itself after the power cycle, so
the port kernel is never lost - but a hung kernel gives no adb and no
USB, so the fastboot recovery needs the physical buttons.

Two failures have been seen and are worth recognising:

* **A dim lit screen with no adb and no USB at all.** The kernel booted
  and then the port's own bootdevice guard lost its race with the ROM's
  init: the guard has to install the read-only fstab and the vendor
  contexts *before* the ROM's init opens its SELinux policy, and the
  window between the two is about 1.35 seconds. The guard failing means
  the ROM's init waits for a boot device that nobody releases. Do not
  delay the guard by anything.
* **The ROM asking for a reboot with "Checkpoint commit failed"**, and
  the reboot never finishing. The checkpoint commit fails because the
  6.12 kernel puts the per-device wakeup statistics where the ROM's
  4.19-era policy does not expect them, so the suspend service is denied
  reading them. The fix is a label, not a policy change: the port's diag
  init relabels those nodes, and the label has to be set in the
  initramdisk's own mount namespace, before the guard chroots into the
  ROM's root, where /sys is not the kernel's.

Record run results in `porting/HANDOFF.md`, `porting/FAILING-COMPONENTS.md`
and the relevant tracker, with raw evidence under `porting/artifacts/`.
Keep this guide procedural. Record the final device state, hashes, capture
limits, observed failures and unverified claims; never infer a rendering
fix from service state or a single sample.
