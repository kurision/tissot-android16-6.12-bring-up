# Building and boot-testing tissot

This is the entry point for building this port on the current Linux host. Read `porting/REFERENCE-boot-testing.md` before a boot. Do not build or boot without the user's authorization. Never flash a partition for these tests.

## Toolchain: pin executable paths

Use these pinned tools and verify their versions before building:

| Tool | Path | Version |
|---|---|---|
| System C compiler | /usr/bin/clang | 22.1.8 |
| System Rust (wrong for reproducing the known build) | /usr/bin/rustc | 1.98.1, LLVM 22.1.8 |
| Known Rust | /home/kurision/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin/rustc | 1.99.0, LLVM 23.1.1 |
| Cached bindgen | /home/kurision/.cache/run164-bindgen0732/bin/bindgen | 0.73.2 |
| libclang | /usr/lib/libclang.so | 22.1.8 |

Plain `rustc` resolves through ~/.cargo/bin in the interactive environment, but `scripts/build-ack-tissot.sh` prepends /usr/bin. Set absolute RUSTC, HOSTRUSTC and BINDGEN paths. The cached bindgen exists even though bindgen is absent from PATH. The stable toolchain directory can change after rustup updates; verify versions every time.

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

Rust 1.99 renamed -Zdebug-info-for-profiling. Preserve the existing AutoFDO settings using this make-command override, without editing kernel sources:

```bash
RUST_PROFILE_FLAGS='-Zdebuginfo-for-profiling -Cllvm-args=-enable-fs-discriminator=true -Cllvm-args=-improved-fs-discriminator=true'
```

Adding the new spelling through KRUSTFLAGS does not remove the old option and still fails.

## Source tree, output and configuration

Use a fresh run number. Existing images, kernel directories and evidence are immutable; check disk space first. /tmp is ephemeral.

Select the staged source tree explicitly. Verify that it exists and matches every source file being tested; extend the comparison list below for the experiment. Do not silently compile a stale staged copy.

```bash
REPO=$PWD
SNAP="$REPO/porting/artifacts/snapshot60"
ACK_TREE=/tmp/ack-run164-source
RUN=188 # replace with an UNUSED run number
OUT_DIR="$REPO/out"
test -f "$ACK_TREE/Makefile"
test ! -e "$OUT_DIR"
df -h "$REPO" /tmp
for path in drivers/gpu/msm/kgsl_pool.c drivers/gpu/msm/adreno.c \
  drivers/iommu/arm/arm-smmu/qcom_iommu.c \
  arch/arm64/boot/dts/qcom/msm8953.dtsi \
  arch/arm64/boot/dts/qcom/msm8953-xiaomi-tissot.dts; do
  cmp "$REPO/$path" "$ACK_TREE/$path" || exit 1
done
```

For an incremental bring-up experiment, preserve a known configuration rather than merging an old fragment over it. Select an archived kernel configuration with Rust/ashmem enabled; the example below names an existing configuration source. Verify the required symbols after configuration resolves.

```bash
mkdir "$OUT_DIR"
scripts/extract-ikconfig "$SNAP/kdir-run185/Image.gz" > "$OUT_DIR/.config"
test -s "$OUT_DIR/.config"
make_args=(make -C "$ACK_TREE" "O=$OUT_DIR"
  ARCH=arm64 LLVM=1 LLVM_IAS=1 CC=clang
  "RUSTC=$RUSTC" "HOSTRUSTC=$HOSTRUSTC" "BINDGEN=$BINDGEN"
  "RUSTFLAGS_AUTOFDO_CLANG=$RUST_PROFILE_FLAGS")
"${make_args[@]}" rustavailable || exit 1
"${make_args[@]}" olddefconfig || exit 1
for key in RUST_IS_AVAILABLE RUST ASHMEM SHMEM STAGING IKCONFIG IKCONFIG_PROC \
  PSTORE_RAM QCOM_IOMMU ARM_SMMU QCOM_SCM ANDROID_BINDER_IPC \
  USB_CONFIGFS USB_CONFIGFS_F_FS COMPAT; do
  rg -qx "CONFIG_${key}=y" "$OUT_DIR/.config" || exit 1
done
```

Inspect configuration differences before compiling. Do not use bare make with an unpinned environment: a configuration resync can remove Rust and ashmem while the C kernel still builds successfully. The wrapper's rustavailable and required-symbol guards exist to prevent this. Its gki_defconfig/fragment path is for deliberate baseline regeneration, not an unnoticed change to an experiment's config.

## Build and package

```bash
set -o pipefail
"${make_args[@]}" -j8 Image.gz 2>&1 | tee "$SNAP/run$RUN-build.log"
test "${PIPESTATUS[0]}" -eq 0 || exit 1
"${make_args[@]}" qcom/msm8953-xiaomi-tissot.dtb || exit 1
BOOT="$OUT_DIR/arch/arm64/boot"
cat "$BOOT/Image.gz" "$BOOT/dts/qcom/msm8953-xiaomi-tissot.dtb" > "$BOOT/Image.gz-dtb"
BASE="$SNAP/boot-run183.img" # real, SHA-verified diagnostic base; never reconstruct
python3 -B porting/logging/package-diag-boot.py \
  --base-image "$BASE" --kernel-dir "$SNAP/kdir-run$RUN" \
  --stage-kernel "$BOOT" --init "$SNAP/init-run127" \
  --keep-stock-console --out "$SNAP/boot-run$RUN.img" \
  --report "$SNAP/package-run$RUN.json"
```

Record the base hash before packaging. Verify the package report and shipped embedded config, not just the output-tree config. For a kernel-only test, ramdisk hash, init hash, command line and DTB must match the chosen base unless an explicit change was approved. Do not append inherited policy/firmware or repeat --debuggable without a reason. Preserve ramoops.reset_capture=1; do not restore panic=0.

```bash
scripts/extract-ikconfig "$SNAP/kdir-run$RUN/Image.gz" > "$SNAP/run$RUN-embedded.config"
for key in RUST_IS_AVAILABLE RUST ASHMEM; do
  rg -qx "CONFIG_${key}=y" "$SNAP/run$RUN-embedded.config" || exit 1
done
sha256sum "$BASE" "$SNAP/boot-run$RUN.img"
cat "$SNAP/package-run$RUN.json"
```

## Preserve evidence, RAM boot and observe

1. Confirm the selected serial is 2819f2320604, stock uname starts with 4.19, and stock su works.
2. Ensure no old USB reader or logcat observer remains. Archive early-logcat files and verify the device logdump SHA against a preserved 64 MiB local copy. Never clear the only evidence. Leaving logdump uncleared requires comparing the new capture against that baseline and excluding stale tails.
3. Verify pyserial: /tmp/diagvenv/bin/python -B -c 'import serial; print(serial.__version__)'. This venv is ephemeral; do not assume it survives.
4. Start USB capture before fastboot boot. Save exact commands, return codes and timestamps. Poll ADB every four seconds, including uname, boot ID, uptime, boot_completed and process IDs. Capture logcat continuously once the port appears.
5. Observe up to eight minutes unless it demonstrably returns to stock. Confirm /dev/ashmem exists. Boot completion on stock is not a port result.
6. Ask for physical panel observation; do not use screencap as proof of visible output on this device.

usb-boot-capture.py always starts with adb reboot bootloader and therefore cannot start from an already-fastboot device. Bound its lifetime: its reader can wait indefinitely when no diagnostic gadget appears. An empty USB log, an ACM disconnect, or fastboot OKAY alone does not establish whether the kernel booted.

Distinguish these two failures in the host log:

- If adb reboot bootloader returns success but fastboot devices remains empty, the image has not been sent. Check USB enumeration and the physical screen; allow a bounded enumeration window before calling the transition a failure.
- If fastboot boot logs Sending and Booting OKAY, the bootloader accepted the command. Establish kernel execution from a fresh banner or boot ID; the splash image alone cannot distinguish a bootloader wait from an early kernel failure.

For an already-fastboot device, fastboot-boot-capture.py skips the ADB reboot step and has --boot-timeout. Stop its reader before a later boot. To investigate orchestration separately, start a reader first and record explicit fastboot commands and their responses.

For a repeat from stock, keep live ADB observation in another terminal, then use:

```bash
adb -s 2819f2320604 reboot bootloader
timeout 600 /tmp/diagvenv/bin/python -B porting/logging/fastboot-boot-capture.py \
  --image "$SNAP/boot-run$RUN.img" --log "$SNAP/run$RUN-usb.log" \
  --status "$SNAP/run$RUN-usb-capture.status" --boot-timeout 480
```

The helper waits up to 60 seconds for fastboot. Capture can finish at the USB handoff, before the eight-minute device observation ends. Continue the ADB observation independently. If the phone is already in fastboot, skip the adb reboot command. Never run a second reader against the same serial port.

## Recover and interpret

Save live evidence before returning the RAM boot to stock. If a reboot is needed for persistent recovery, stop capture children first. Confirm stock identity and root before:

```bash
cd "$SNAP"
bash ../../logging/pull-run-evidence.sh "$RUN" "boot-run$RUN.img"
python3 -B ../../logging/decode-reset.py "run$RUN-logdump.bin"
```

The recovery script refuses to overwrite existing evidence. Compare image payload hashes and boot IDs; check snapshot timestamps and stale capture tails. CRC validation proves stored bytes, not boot freshness.

Record run results in porting/HANDOFF.md, porting/FAILING-COMPONENTS.md and the relevant tracker, with raw evidence under porting/artifacts/. Keep this guide procedural. Record the final device state, hashes, capture limits, observed failures and unverified claims; never infer a rendering fix from service state or a single sample.

