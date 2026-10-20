#!/usr/bin/env bash
#
# Build and package the port kernel.
#
#   usage: build-kernel.sh [run-name] [base-boot-image]
#
#     run-name          a label for this build's output, e.g. run289
#                       (default: run-$(date -u +%Y%m%dT%H%M%SZ))
#     base-boot-image   the boot image whose ramdisk and command line the
#                       result carries. It must be a real image: the
#                       diag ramdisk carries a patched SELinux policy and a
#                       force_debuggable trigger that nothing in this tree
#                       regenerates, so a reconstructed base packages
#                       cleanly, boots, and then stalls in userspace
#                       several stages later than it should. If it is not
#                       given, look for it next to the repository (see
#                       BASE_IMAGE_HINT below).
#
# What this does, in order:
#
#   1. Checks the toolchain. The C compiler is clang; the Rust compiler
#      and the bindgen are found (rustup's toolchain, then the PATH), and
#      every path is made absolute, because a plain `rustc` resolves
#      through ~/.cargo/bin and that is not necessarily the toolchain the
#      known build used.
#   2. Writes the configuration. The kernel's configuration is not the
#      defconfig: the port's configuration is in porting/tools/kernel.config
#      and is what the known good images were built with. The symbols the
#      port needs on top of it are forced here, because the
#      configuration is not the builder's to edit.
#   3. Builds the kernel out of tree, into out/ in this repository. The
#      source tree is this repository, so a clone builds.
#   4. Rebuilds the ramdisk's /init from scripts/initramfs-diag/init.c, so
#      an image never carries a stale init binary.
#   5. Packages: the base image's ramdisk and command line, this kernel,
#      this init.
#
# The output directory is out/ in this repository and it is reused. A
# clean rebuild means emptying it, not making a new one:
#
#   rm -rf out/* out/.[!.]*
#
# Everything the script needs is found, not hardcoded. These environment
# variables override the search when the toolchain is not where the
# defaults look for it:
#
#   ACK_TISSOT_BASE_IMAGE  the base boot image (also the second argument)
#   ACK_SNAPSHOT_DIR       where the per-run evidence goes
#   RUST_TOOLCHAIN         the rustup toolchain directory
#   RUSTC, HOSTRUSTC       the Rust compiler and the host compiler
#   BINDGEN                the bindgen binary
#   RUST_LIB_SRC           the Rust standard library sources
#   LIBCLANG_PATH          the directory holding libclang.so
#   CLANG, CROSS_CC        the C compiler and the aarch64 cross compiler
#   LLVM_NM                llvm-nm, for the symbol check at the end
#   RUST_PROFILE_FLAGS     the AutoFDO flags
#
# See BUILD_TISSOT.md for the toolchain requirements and the rest of the
# procedure.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO"

RUN=${1:-run-$(date -u +%Y%m%dT%H%M%SZ)}
SNAP=${ACK_SNAPSHOT_DIR:-$REPO/porting/artifacts/snapshot60}
OUT_DIR="$REPO/out"
CONFIG="$REPO/porting/tools/kernel.config"

# --- the base image ---------------------------------------------------------
# A real, SHA-verified image. Not redistributable, so it is not in the
# repository: the builder supplies it, or keeps it next to the checkout.
BASE_IMAGE_HINT=${ACK_TISSOT_BASE_IMAGE:-}
if [ $# -ge 2 ]; then
	BASE_IMAGE=$2
elif [ -n "$BASE_IMAGE_HINT" ] && [ -f "$BASE_IMAGE_HINT" ]; then
	BASE_IMAGE=$BASE_IMAGE_HINT
else
	for candidate in "$REPO/../ack-kgsl-artifacts/base.img" \
			  "$REPO/../ack-kgsl-artifacts/boot-run258.img" \
			  "$REPO/porting/artifacts/base/boot-run258.img" \
			  "$REPO/porting/artifacts/snapshot60/boot-run258.img"; do
		if [ -f "$candidate" ]; then
			BASE_IMAGE=$candidate
			break
		fi
	done
fi
if [ -z "${BASE_IMAGE:-}" ] || [ ! -f "$BASE_IMAGE" ]; then
	echo "error: no base boot image." >&2
	echo "Give one as the second argument, or set ACK_TISSOT_BASE_IMAGE." >&2
	echo "It must be a real image: the diag ramdisk carries a patched" >&2
	echo "SELinux policy and a force_debuggable trigger that nothing in" >&2
	echo "this tree regenerates." >&2
	exit 1
fi

# --- the toolchain ----------------------------------------------------------
# The tools are found, not hardcoded, so a fresh clone builds on any host
# that has them. A configuration resync with the wrong toolchain turns Rust
# and ashmem off while the C kernel still builds, and the framework then
# loses /dev/ashmem, so the toolchain is resolved once, here, and every
# path is absolute from then on.
TC=${RUST_TOOLCHAIN:-}
if [ -z "$TC" ]; then
	# rustup's default toolchain, if there is one.
	TC=$(rustup which rustc 2>/dev/null | sed 's|/bin/rustc$||' || true)
fi
RUSTC=${RUSTC:-$TC/bin/rustc}
HOSTRUSTC=${HOSTRUSTC:-$RUSTC}
BINDGEN=${BINDGEN:-}
if [ -z "$BINDGEN" ]; then
	# On the PATH first: bindgen is not part of a rustup toolchain, so it
	# is usually installed separately (cargo install bindgen --version
	# 0.73.2).
	BINDGEN=$(command -v bindgen || true)
fi
if [ -z "$BINDGEN" ] && [ -n "$TC" ]; then
	BINDGEN=$TC/bin/bindgen
fi
RUST_LIB_SRC=${RUST_LIB_SRC:-$TC/lib/rustlib/src/rust/library}
CLANG=${CLANG:-$(command -v clang || true)}
CROSS_CC=${CROSS_CC:-$(command -v aarch64-linux-gnu-gcc || command -v aarch64-none-linux-gnu-gcc || true)}
LIBCLANG_PATH=${LIBCLANG_PATH:-}
if [ -z "$LIBCLANG_PATH" ]; then
	for d in /usr/lib /usr/lib64 /usr/lib/llvm-*/lib; do
		if [ -r "$d/libclang.so" ]; then LIBCLANG_PATH=$d; break; fi
	done
fi

test -n "$CLANG" && test -x "$CLANG" || {
	echo "error: no clang on PATH; set CLANG" >&2; exit 1; }
test -x "$RUSTC" || { echo "error: no rustc at $RUSTC; set RUSTC" >&2; exit 1; }
test -x "$BINDGEN" || {
	echo "error: bindgen not found." >&2
	echo "Install it (cargo install bindgen --version 0.73.2) or set BINDGEN." >&2
	exit 1; }
test -r "$RUST_LIB_SRC/core/src/lib.rs" || {
	echo "error: the Rust standard library source is not at $RUST_LIB_SRC;" >&2
	echo "set RUST_LIB_SRC" >&2; exit 1; }
test -r "$LIBCLANG_PATH/libclang.so" || {
	echo "error: no libclang.so in $LIBCLANG_PATH; set LIBCLANG_PATH" >&2
	exit 1; }
test -n "$CROSS_CC" && test -x "$CROSS_CC" || {
	echo "error: no aarch64 cross compiler on PATH; set CROSS_CC" >&2; exit 1; }

# Rust 1.99 renamed -Zdebug-info-for-profiling. Keep the existing AutoFDO
# settings through the make command; the new spelling does not remove the
# old option and still fails.
RUST_PROFILE_FLAGS=${RUST_PROFILE_FLAGS:-'-Zdebuginfo-for-profiling -Cllvm-args=-enable-fs-discriminator=true -Cllvm-args=-improved-fs-discriminator=true'}

echo "=== the toolchain ==="
"$CLANG" --version | head -1
"$RUSTC" -vV | grep -E "^release|LLVM version" || true
"$BINDGEN" --version || true
echo "=== the source tree ==="
echo "  $REPO"
echo "=== the output directory ==="
echo "  $OUT_DIR"
echo "=== the base image ==="
echo "  $BASE_IMAGE"

# --- the run's outputs ------------------------------------------------------
mkdir -p "$SNAP" "$OUT_DIR"
test ! -e "$SNAP/boot-$RUN.img"
test ! -e "$SNAP/kdir-$RUN"
test ! -e "$SNAP/package-$RUN.json"
trap 'rc=$?; printf "status=%s\n" "$rc" > "$SNAP/$RUN-build.status"' EXIT

make_args=(make -C "$REPO" "O=$OUT_DIR" ARCH=arm64 LLVM=1 LLVM_IAS=1 "CC=$CLANG"
 "RUSTC=$RUSTC" "HOSTRUSTC=$HOSTRUSTC" "BINDGEN=$BINDGEN"
 "RUSTFLAGS_AUTOFDO_CLANG=$RUST_PROFILE_FLAGS"
 KSU_GIT_VERSION=3355 KSU_GIT_VERSION_VALID=1 KSU_GIT_TAG=v3.4.2)

# --- the configuration ------------------------------------------------------
# The port's own configuration, not the defconfig and not whatever an
# earlier build left behind.
cp "$CONFIG" "$OUT_DIR/.config"
cp "$OUT_DIR/.config" "$SNAP/$RUN-prebuild.config"
# The rust toolchain has to be probed before the configuration is
# resolved: RUST is off until the probe has run, and ashmem depends on
# RUST, so a configuration resolved without the probe loses both while
# the C kernel still builds.
"${make_args[@]}" rustavailable || exit 1
"${make_args[@]}" olddefconfig

# The symbols that have to be on. The configuration is the port's, so this
# is the only place they can be turned on.
#   CONFIG_KSU: KernelSU-Next is built in, and the manager only talks to
#   a driver that reports the released version.
#   The SND_SOC_* symbols: the audio port - the AFE, the WCD9335 codec,
#   the SDM660 CDC, the microphone bias and headset control, and the
#   machine driver. They are enabled so the boot shows whether they
#   probe; the sound card's device tree is not written yet, so they will
#   not produce sound. The codec and the microphone bias symbols are the
#   port's own (SND_SOC_MSM_*), not the upstream ones of the same name:
#   the tree already carries upstream's WCD9335 and its microphone bias
#   helper, and the port's machine driver wants the CAF ones, which
#   register their DAIs as tasha_*.
#   CONFIG_SLIMBUS and CONFIG_SLIM_QCOM_CTRL: the codec is a slimbus
#   slave, and the tree's slimbus subsystem is off by default.
#   CONFIG_ZRAM: the ROM's own init.rc already does
#   "chown root system /sys/block/zram0/idle" and the same for
#   writeback, so the ROM expects a zram device to be there and sizes
#   it itself.
for sym in KSU SND_SOC_MSM8952 SND_SOC_MSM_WCD9335 SND_SOC_MSM_WCD_MBHC \
	   SND_SOC_WCD_MBHC_ADC WCD9XXX_CODEC_CORE SND_SOC_SDM660_CDC \
	   SLIMBUS SLIM_QCOM_CTRL ZRAM ZRAM_BACKEND_LZ4 ZRAM_BACKEND_ZSTD; do
 grep -q "^CONFIG_$sym=" "$OUT_DIR/.config" && sed -i "s/^CONFIG_$sym=.*/CONFIG_$sym=y/" "$OUT_DIR/.config" || \
	echo "CONFIG_$sym=y" >> "$OUT_DIR/.config"
done
# The upstream WCD9335 and its microphone bias helper have to be off: the
# port's own versions of both are built, they register the same DAIs, and
# they define the same symbols. They are turned off here, and not only
# checked, so that a configuration which still carries them cannot produce
# a kernel with two codecs in it.
for sym in SND_SOC_WCD9335 SND_SOC_WCD_MBHC; do
 sed -i "/^CONFIG_$sym=/d" "$OUT_DIR/.config"
 echo "# CONFIG_$sym is not set" >> "$OUT_DIR/.config"
done
"${make_args[@]}" olddefconfig
scripts/diffconfig "$SNAP/$RUN-prebuild.config" "$OUT_DIR/.config" | tee "$SNAP/$RUN-config.diff"
for sym in KSU RUST RUST_IS_AVAILABLE ASHMEM SHMEM SND_SOC_MSM8952 \
	   SND_SOC_MSM_WCD9335 SND_SOC_MSM_WCD_MBHC WCD9XXX_CODEC_CORE \
	   SND_SOC_SDM660_CDC SLIMBUS ZRAM; do
 grep -qx "CONFIG_$sym=y" "$OUT_DIR/.config" || { echo "CONFIG_$sym is not set" >&2; exit 1; }
done
for key in RUST_IS_AVAILABLE RUST ASHMEM SHMEM STAGING IKCONFIG IKCONFIG_PROC \
	    PSTORE_RAM QCOM_IOMMU ARM_SMMU QCOM_SCM ANDROID_BINDER_IPC \
	    USB_CONFIGFS USB_CONFIGFS_F_FS COMPAT CPUFREQ_DT \
	    QCOM_CLK_APCC_MSM8953 QCOM_APCS_IPC QCOM_RPMPD PM_GENERIC_DOMAINS_OF; do
 rg -qx "CONFIG_${key}=y" "$OUT_DIR/.config" || { echo "CONFIG_$key is not set" >&2; exit 1; }
done
# The upstream WCD9335 and its microphone bias helper must stay off: the
# port's versions define the same symbols.
for sym in SND_SOC_WCD9335 SND_SOC_WCD_MBHC; do
 if grep -qx "CONFIG_$sym=y" "$OUT_DIR/.config"; then
  echo "CONFIG_$sym must not be set" >&2; exit 1
 fi
done

# --- the build --------------------------------------------------------------
# W=1 for the port's own objects only; the kgsl and the ported CAF audio
# are vendor code that does not satisfy W=1. The log is kept: it is the
# record of what this run actually compiled.
LOG="$SNAP/$RUN-build.log"
"${make_args[@]}" -j"$(nproc)" W=1 fs/pstore/ram.o 2>&1 | tee "$LOG"
"${make_args[@]}" -j"$(nproc)" drivers/kernelsu/ 2>&1 | tee -a "$LOG"
"${make_args[@]}" -j"$(nproc)" Image.gz 2>&1 | tee -a "$LOG"
"${make_args[@]}" dt_binding_check DT_SCHEMA_FILES=clock/qcom,apcc-msm8953.yaml
"${make_args[@]}" CHECK_DTBS=y qcom/msm8953-xiaomi-tissot.dtb
BOOT="$OUT_DIR/arch/arm64/boot"
cat "$BOOT/Image.gz" "$BOOT/dts/qcom/msm8953-xiaomi-tissot.dtb" > "$BOOT/Image.gz-dtb"

# --- the /init, rebuilt from source every time ------------------------------
"$CROSS_CC" -Os -static -DANDROID_CAPTURE -DANDROID_EARLY_LOGCAT \
	-DBOOT_DEADLINE_SECONDS=0 scripts/initramfs-diag/init.c \
	-o "$SNAP/init-$RUN"

# --- the packaging ----------------------------------------------------------
sha256sum "$BASE_IMAGE" > "$SNAP/$RUN-base.sha256"
python3 -B porting/logging/package-diag-boot.py --base-image "$BASE_IMAGE" \
 --kernel-dir "$SNAP/kdir-$RUN" --stage-kernel "$BOOT" --init "$SNAP/init-$RUN" \
 --keep-stock-console --out "$SNAP/boot-$RUN.img" --report "$SNAP/package-$RUN.json"
scripts/extract-ikconfig "$SNAP/kdir-$RUN/Image.gz" > "$SNAP/$RUN-embedded.config"

# --- check the result -------------------------------------------------------
LLVM_NM=${LLVM_NM:-$(command -v llvm-nm || ls /usr/lib/llvm-*/bin/llvm-nm 2>/dev/null | head -1)}
SNAP="$SNAP" LLVM_NM="$LLVM_NM" RUN="$RUN" python3 - <<'PY'
import os, pathlib, json, gzip, re, subprocess
s=pathlib.Path(os.environ['SNAP'])
run=os.environ['RUN']
new=json.loads((s/f'package-{run}.json').read_text())
symbols=subprocess.check_output([os.environ['LLVM_NM'],'-a','out/vmlinux'],text=True)
for name in ['ramoops_witness_write','ramoops_stage_write','ramoops_reset_note',
	     'kgsl_timeline_signal','snd_pcm_add_volume_ctls','snd_soc_info_multi_ext']:
    assert name in symbols, name
config=(s/f'{run}-embedded.config').read_text()
for key in ['RUST_IS_AVAILABLE','RUST','ASHMEM','CPUFREQ_DT','QCOM_CLK_APCC_MSM8953',
	    'QCOM_RPMPD','PSTORE_RAM','KSU','SND_SOC_MSM8952','SND_SOC_MSM_WCD9335','ZRAM']:
    assert f'CONFIG_{key}=y\n' in config, key
print('The diagnostic symbols and the shipped config are as expected.')
print(json.dumps(new,indent=2))
for m in re.finditer(rb'Linux version [^\0\n]+',gzip.decompress((s/f'kdir-{run}/Image.gz').read_bytes())):
    print('EMBEDDED BANNER:',m.group().decode())
PY
