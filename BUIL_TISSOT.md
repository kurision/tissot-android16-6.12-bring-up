# Building and boot-testing tissot

This is the entry point for building this port on the current Linux host. Read
`porting/REFERENCE-boot-testing.md` before a boot. Do not build or boot without
the user's authorization.

The port kernel is now **flashed to the boot partition**, not RAM-booted: it
lives on slot b and the phone comes back on it by itself after a power cycle.
The RAM-boot tooling in `porting/logging/` still works and is still the way to
observe a boot, but the image under test is normally written to `boot_b`.

**The whole flow is scripted**: `porting/tools/build-kernel.sh` does the staged
tree refresh, the configuration, the build, the init and the packaging, and it
checks the result. Read this document to know what it does and why; run it
rather than the individual steps.

```bash
cd /home/kurision/development/kernel/ack-kgsl
bash porting/tools/build-kernel.sh run289      # an unused run number
```

## Layout: what is where

| What | Where |
|---|---|
| The repository | `/home/kurision/development/kernel/ack-kgsl` |
| The staged source tree the compiler reads | `../ack-staged-run239` |
| The output directory (one, reused) | `out/` in the repository |
| The boot images, the stock boot backup | `../ack-kgsl-artifacts` |
| The per-run evidence and the build log | `porting/artifacts/snapshot60/` |
| The documentation | `porting/`, this file, `porting/findings/` |

The images are far too large for git, so they live outside the repository in
`../ack-kgsl-artifacts`, with a README there that lists each one and its
checksum. `stock-boot-live.bin` in that directory is **the only copy of the
phone's original boot partition**. Do not delete it.

## Toolchain: pin executable paths

Use these pinned tools and verify their versions before building:

| Tool | Path | Version |
|---|---|---|
| System C compiler | /usr/bin/clang | 23.1.3 |
| System Rust (not the one the known build used) | /usr/bin/rustc | 1.99.1 |
| Known Rust | /home/kurision/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin/rustc | 1.99.0, LLVM 23.1.1 |
| Cached bindgen | /home/kurision/.cache/run164-bindgen0732/bin/bindgen | 0.73.2 |
| libclang | /usr/lib/libclang.so | 23.1.3 |

The C compiler and libclang moved from 22.1.8 to 23.1.3 since this document was
first written; the known Rust toolchain is unchanged. A build with a different
clang produces a different kernel, so record the version in the run's notes.
The stable toolchain directory can change after a rustup update; verify the
versions every time.

Plain `rustc` resolves through ~/.cargo/bin in the interactive environment.
Set absolute RUSTC, HOSTRUSTC and BINDGEN paths. The cached bindgen exists
even though bindgen is absent from PATH.

```bash
TC=/home/kurision/.rustup/toolchains/stable-x86_64-unknown-linux-gnu
export RUSTC="$TC/bin/rustc"
export HOSTRUSTC="$TC/bin/rustc"
export BINDGEN=/home/kurision/.cache/run164-bindgen0732/bin/bindgen
export RUST_LIB_SRC="$TC/lib/rustlib/src/rust/library"
export LIBCLANG_PATH=/usr/lib
"$RUSTC" -vV
"$BINDGEN" --version
test -r "$RUST_LIB_SRC/core/src/lib.rs"
test -r "$LIBCLANG_PATH/libclang.so"
```

Rust 1.99 renamed -Zdebug-info-for-profiling. Preserve the existing AutoFDO
settings using this make-command override, without editing kernel sources:

```bash
RUST_PROFILE_FLAGS='-Zdebuginfo-for-profiling -Cllvm-args=-enable-fs-discriminator=true -Cllvm-args=-improved-fs-discriminator=true'
```

Adding the new spelling through KRUSTFLAGS does not remove the old option and
still fails.

## Source tree: the staged tree, and the trap in it

The kernel is **not** built from the repository. It is built from a staged
source tree that the build refreshes from the repository first, and then
verifies file by file. That verification is not optional: the staged tree is
what the compiler reads, so a stale one silently produces a kernel that does
not contain the repository's fixes.

This happened. The capacitive navigation keys were committed to the repository
(arm64: dts: qcom: msm8953-xiaomi-tissot: enable navigation keys, and the
edt-ft5x06 change that reports them), and the image that was flashed did not
have them, because the staged tree was a day older than the commits. The
symptom was that the on-screen navigation keys did nothing while the volume
keys worked. The same staleness also kept the CAF fastrpc ioctl ABI out of the
kernel, which is why the adsprpcd daemon crash-looped.

The refresh covers the areas the port touches, and `sound/` is one of them -
the audio port lives there, and a build that does not stage it produces a
kernel without the AFE, the WCD9335 codec, the SDM660 CDC and the machine
driver.

```bash
REPO=/home/kurision/development/kernel/ack-kgsl
ACK_TREE=/home/kurision/development/kernel/ack-staged-run239
test -f "$ACK_TREE/Makefile"
df -h "$REPO" /tmp
```

## Output directory: one, reused

There is one output directory, `out/` in the repository, and it is reused. A
clean rebuild means emptying it, not making a new one:

```bash
cd /home/kurision/development/kernel/ack-kgsl
rm -rf out/* out/.[!.]*
```

Do not use bare make with an unpinned environment against it: a configuration
resync with the wrong toolchain can remove Rust and ashmem while the C kernel
still builds successfully, and the framework then loses /dev/ashmem. The build
script's `rustavailable` and required-symbol guards exist to prevent this.

## Configuration: from an archived image, and the symbols that must be on

The configuration is not the defconfig and not whatever the last build left
behind. The port's defconfig is dead code: the build takes the configuration
from the previous image's embedded config, and the defconfig's
`CONFIG_QCOM_TSENS=y` is not what runs. Take it from an archived image that
is known to have Rust and ashmem enabled - `scripts/extract-ikconfig` reads
the kernel straight out of a boot image, so the image is the source:

```bash
ARCHIVE=/home/kurision/development/kernel/ack-kgsl-artifacts
scripts/extract-ikconfig "$ARCHIVE/boot-run282.img" > out/.config
test -s out/.config
```

Then `rustavailable` before `olddefconfig`, and the symbols that have to be on
afterwards. Two of them are not in the archived configuration and have to be
forced, because the configuration is the archived one:

| Symbol | Why |
|---|---|
| `CONFIG_KSU` | KernelSU-Next is built in, and the manager only talks to a driver that reports the released version (33355, pinned with `KSU_GIT_VERSION=3355 KSU_GIT_VERSION_VALID=1 KSU_GIT_TAG=v3.4.2`). |
| `CONFIG_SND_SOC_MSM8952`, `CONFIG_SND_SOC_WCD9335`, `CONFIG_WCD9XXX_CODEC_CORE`, `CONFIG_SND_SOC_SDM660_CDC`, `CONFIG_SND_SOC_WCD_MBHC`, `CONFIG_SND_SOC_WCD_MBHC_ADC` | The audio port: the AFE, the WCD9335 codec, the SDM660 CDC, the microphone bias and headset control, and the machine driver. |

The audio symbols are on so that a boot shows whether the drivers probe. The
sound card's device tree is not written yet, so **they will not make sound**:
the port's device tree has no sound card node and no audio pinctrl, and the
donor (`oc-test/arch/arm64/boot/dts/vendor/qcom/mi8953/tissot/audio.dtsi`) has
the template.

```bash
make_args=(make -C "$ACK_TREE" "O=$REPO/out"
  ARCH=arm64 LLVM=1 LLVM_IAS=1 CC=clang
  "RUSTC=$RUSTC" "HOSTRUSTC=$HOSTRUSTC" "BINDGEN=$BINDGEN"
  "RUSTFLAGS_AUTOFDO_CLANG=$RUST_PROFILE_FLAGS"
  KSU_GIT_VERSION=3355 KSU_GIT_VERSION_VALID=1 KSU_GIT_TAG=v3.4.2)
"${make_args[@]}" rustavailable || exit 1
"${make_args[@]}" olddefconfig || exit 1
for sym in KSU SND_SOC_MSM8952 SND_SOC_WCD9335 WCD9XXX_CODEC_CORE SND_SOC_SDM660_CDC; do
  grep -q "^CONFIG_$sym=" out/.config && \
    sed -i "s/^CONFIG_$sym=.*/CONFIG_$sym=y/" out/.config || \
    echo "CONFIG_$sym=y" >> out/.config
done
"${make_args[@]}" olddefconfig || exit 1
for key in RUST_IS_AVAILABLE RUST ASHMEM SHMEM STAGING IKCONFIG IKCONFIG_PROC \
  PSTORE_RAM QCOM_IOMMU ARM_SMMU QCOM_SCM ANDROID_BINDER_IPC \
  USB_CONFIGFS USB_CONFIGFS_F_FS COMPAT CPUFREQ_DT \
  QCOM_CLK_APCC_MSM8953 QCOM_APCS_IPC QCOM_RPMPD PM_GENERIC_DOMAINS_OF; do
  rg -qx "CONFIG_${key}=y" out/.config || exit 1
done
```

Inspect the configuration difference before compiling:
`scripts/diffconfig` against the prebuild copy. Its gki_defconfig/fragment
path is for deliberate baseline regeneration, not an unnoticed change to an
experiment's config.

## Build and package

```bash
set -o pipefail
"${make_args[@]}" -j8 Image.gz 2>&1 | tee "porting/artifacts/snapshot60/run$RUN-build.log"
test "${PIPESTATUS[0]}" -eq 0 || exit 1
"${make_args[@]}" qcom/msm8953-xiaomi-tissot.dtb || exit 1
BOOT=out/arch/arm64/boot
cat "$BOOT/Image.gz" "$BOOT/dts/qcom/msm8953-xiaomi-tissot.dtb" > "$BOOT/Image.gz-dtb"
```

The `/init` is rebuilt from source every time, so the image never carries a
stale init binary:

```bash
aarch64-linux-gnu-gcc -Os -static -DANDROID_CAPTURE -DANDROID_EARLY_LOGCAT \
  -DBOOT_DEADLINE_SECONDS=0 scripts/initramfs-diag/init.c \
  -o "porting/artifacts/snapshot60/init-run$RUN"
```

The base image is a real, SHA-verified diagnostic base from the archive; never
reconstruct it. The diag ramdisk carries a patched SELinux policy and a
`force_debuggable` trigger that nothing in the tree regenerates, and a
reconstructed base packages cleanly, boots, and then stalls in userspace
several stages later than it should.

```bash
BASE=/home/kurision/development/kernel/ack-kgsl-artifacts/boot-run258.img
python3 -B porting/logging/package-diag-boot.py \
  --base-image "$BASE" --kernel-dir "porting/artifacts/snapshot60/kdir-run$RUN" \
  --stage-kernel "$BOOT" --init "porting/artifacts/snapshot60/init-run$RUN" \
  --keep-stock-console --out "porting/artifacts/snapshot60/boot-run$RUN.img" \
  --report "porting/artifacts/snapshot60/package-run$RUN.json"
```

Record the base hash before packaging. Verify the package report and the
shipped embedded config, not just the output-tree config. For a kernel-only
test, the ramdisk hash, the init hash, the command line and the DTB must match
the chosen base unless an explicit change was approved. Do not append inherited
policy or firmware and do not repeat --debuggable without a reason. Preserve
ramoops.reset_capture=1; do not restore panic=0.

```bash
scripts/extract-ikconfig "porting/artifacts/snapshot60/kdir-run$RUN/Image.gz" \
  > "porting/artifacts/snapshot60/run$RUN-embedded.config"
for key in RUST_IS_AVAILABLE RUST ASHMEM KSU SND_SOC_MSM8952 SND_SOC_WCD9335; do
  rg -qx "CONFIG_${key}=y" "porting/artifacts/snapshot60/run$RUN-embedded.config" || exit 1
done
sha256sum "$BASE" "porting/artifacts/snapshot60/boot-run$RUN.img"
cat "porting/artifacts/snapshot60/package-run$RUN.json"
```

## Flash it

The partition is 64 MiB and the image is not, so pad it:

```bash
cd porting/artifacts/snapshot60
python3 -c "
import os
sz=os.path.getsize('boot-run$RUN.img')
open('boot-run$RUN-padded.img','wb').write(
    open('boot-run$RUN.img','rb').read()+b'\0'*(67108864-sz))"
adb -s 2819f2320604 reboot bootloader
fastboot -s 2819f2320604 getvar current-slot          # slot b
fastboot -s 2819f2320604 flash boot_b boot-run$RUN-padded.img
fastboot -s 2819f2320604 reboot
```

The image that is currently on the phone is in the archive
(`boot-run282-padded.img`); flash that back to return to the last known good
kernel.

## Preserve evidence and observe

1. Confirm the selected serial is 2819f2320604, that the phone boots the port
   kernel (`uname -r` is 6.12.93-4k), and that the adb shell is root
   (`id` shows uid=0 and `u:r:ksu:s0`). The root comes from KernelSU-Next,
   which is built into the kernel; adbd as `shell` cannot read the kgsl
   debugfs, which is where the GPU's retired count is.
2. Ensure no old USB reader or logcat observer remains. Archive early-logcat
   files and verify the device logdump SHA against a preserved 64 MiB local
   copy. Never clear the only evidence. Leaving logdump uncleared requires
   comparing the new capture against that baseline and excluding stale tails.
3. Verify pyserial: /tmp/diagvenv/bin/python -B -c 'import serial; print(serial.__version__)'. This venv is ephemeral; do not assume it survives.
4. Start USB capture before the boot. Save exact commands, return codes and
   timestamps. Poll ADB every four seconds, including uname, boot ID, uptime,
   boot_completed and process IDs. Capture logcat continuously once the port
   appears.
5. Observe up to eight minutes unless it demonstrably returns to stock.
   Confirm /dev/ashmem exists. Boot completion on stock is not a port result.
6. Ask for physical panel observation; do not use screencap as proof of visible
   output on this device.

usb-boot-capture.py always starts with adb reboot bootloader and therefore
cannot start from an already-fastboot device. Bound its lifetime: its reader
can wait indefinitely when no diagnostic gadget appears. An empty USB log, an
ACM disconnect, or fastboot OKAY alone does not establish whether the kernel
booted.

Distinguish these two failures in the host log:

- If adb reboot bootloader returns success but fastboot devices remains empty, the image has not been sent. Check USB enumeration and the physical screen; allow a bounded enumeration window before calling the transition a failure.
- If fastboot boot logs Sending and Booting OKAY, the bootloader accepted the command. Establish kernel execution from a fresh banner or boot ID; the splash image alone cannot distinguish a bootloader wait from an early kernel failure.

For an already-fastboot device, fastboot-boot-capture.py skips the ADB reboot
step and has --boot-timeout. Stop its reader before a later boot. To
investigate orchestration separately, start a reader first and record explicit
fastboot commands and their responses.

For a repeat from stock, keep live ADB observation in another terminal, then
use:

```bash
adb -s 2819f2320604 reboot bootloader
timeout 600 /tmp/diagvenv/bin/python -B porting/logging/fastboot-boot-capture.py \
  --image "porting/artifacts/snapshot60/boot-run$RUN.img" \
  --log "porting/artifacts/snapshot60/run$RUN-usb.log" \
  --status "porting/artifacts/snapshot60/run$RUN-usb-capture.status" \
  --boot-timeout 480
```

The helper waits up to 60 seconds for fastboot. Capture can finish at the USB
handoff, before the eight-minute device observation ends. Continue the ADB
observation independently. If the phone is already in fastboot, skip the adb
reboot command. Never run a second reader against the same serial port.

## Recover and interpret

Save live evidence before returning the phone to the previous image. If a
reboot is needed for persistent recovery, stop capture children first.
Confirm stock identity and root before:

```bash
cd porting/artifacts/snapshot60
bash ../../logging/pull-run-evidence.sh "$RUN" "boot-run$RUN.img"
python3 -B ../../logging/decode-reset.py "run$RUN-logdump.bin"
```

The recovery script refuses to overwrite existing evidence. Compare image
payload hashes and boot IDs; check snapshot timestamps and stale capture tails.
CRC validation proves stored bytes, not boot freshness.

**A hung kernel is recovered by forcing the phone off** (long press on the
power button) and flashing the previous image from fastboot. The phone comes
back on the port kernel by itself after the power cycle, so the port kernel is
never lost - but a hung kernel gives no adb and no USB, so the fastboot
recovery needs the physical buttons.

Two failures have been seen and are worth recognising:

* **A dim lit screen with no adb and no USB at all.** The kernel booted and
  then the port's own bootdevice guard lost its race with the ROM's init:
  the guard has to install the read-only fstab and the vendor contexts
  *before* the ROM's init opens its SELinux policy, and the window between
  the two is about 1.35 seconds. The guard failing means the ROM's init waits
  for a boot device that nobody releases. Do not delay the guard by
  anything.
* **The ROM asking for a reboot with "Checkpoint commit failed"**, and the
  reboot never finishing. The checkpoint commit fails because the 6.12
  kernel puts the per-device wakeup statistics where the ROM's 4.19-era
  policy does not expect them, so the suspend service is denied reading
  them. The fix is a label, not a policy change: the port's diag init
  relabels those nodes, and the label has to be set in the initramdisk's
  own mount namespace, before the guard chroots into the ROM's root, where
  /sys is not the kernel's.

Record run results in porting/HANDOFF.md, porting/FAILING-COMPONENTS.md and
the relevant tracker, with raw evidence under porting/artifacts/. Keep this
guide procedural. Record the final device state, hashes, capture limits,
observed failures and unverified claims; never infer a rendering fix from
service state or a single sample.
