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
#                       given, create an update for an existing ACK ramdisk.
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
#   5. Without a base, packages an update for an installed ACK ramdisk.
#      With a base, packages a complete AnyKernel ZIP and diagnostic boot
#      image using that ramdisk and its command line.
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
#   ACK_EXPERIMENTAL_WCNSS_FALLBACK=1  use ueventd instead of embedding NV
#   ACK_WCNSS_FIRMWARE_DIR the staged wcnss.* + WCNSS_qcom_wlan_nv.bin
#                          tree (default: porting/artifacts/firmware-wcnss;
#                          populate it with pull-wcnss-firmware.sh)
#   ACK_VENUS_FIRMWARE_DIR the staged venus.mdt + venus.bNN tree (default:
#                          porting/artifacts/firmware-venus; populate it
#                          with pull-venus-firmware.sh)
#   RUST_TOOLCHAIN         the rustup toolchain directory
#   RUSTC, HOSTRUSTC       the Rust compiler and the host compiler
#   BINDGEN                the bindgen binary
#   RUST_LIB_SRC           the Rust standard library sources
#   LIBCLANG_PATH          the directory holding libclang.so
#   CLANG, CROSS_CC        the C compiler and the aarch64 cross compiler
#   LLVM_NM                llvm-nm, for the symbol check at the end
#   RUST_PROFILE_FLAGS     the AutoFDO flags
#   ACK_LK_KERNEL_LIMIT    the largest image_size the bootloader can load
#                          (default 54001664: the DTB sits at 0x13400000)
#   ACK_LK_HEADROOM_WARN   warn below this much headroom (default 1 MiB)
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
# The WCNSS tree carries both the remoteproc firmware and the WLAN NV blob.
# wcnss.mdt is requested before the vendor partition is mounted and is
# packaged into the ramdisk; the NV blob is requested when wlan0 is brought
# up, long after switch_root, so it is built into the kernel instead. See
# the built-in firmware block in "the configuration" below.
FW_DIR=${ACK_WCNSS_FIRMWARE_DIR:-$REPO/porting/artifacts/firmware-wcnss}
NV_FIRMWARE="$FW_DIR/wlan/prima/WCNSS_qcom_wlan_nv.bin"
# The Venus video codec firmware (venus.mdt + venus.b00..b04) is requested
# by the Venus driver at probe time, before the vendor partition is mounted,
# so it is packaged into the ramdisk the same way the WCNSS blobs are.
VENUS_FW_DIR=${ACK_VENUS_FIRMWARE_DIR:-$REPO/porting/artifacts/firmware-venus}
WCNSS_FALLBACK=${ACK_EXPERIMENTAL_WCNSS_FALLBACK:-0}
case "$WCNSS_FALLBACK" in 0|1) ;; *) echo "error: ACK_EXPERIMENTAL_WCNSS_FALLBACK must be 0 or 1" >&2; exit 1;; esac

BASE_IMAGE=${2:-${ACK_TISSOT_BASE_IMAGE:-}}
if [ -n "$BASE_IMAGE" ] && [ ! -f "$BASE_IMAGE" ]; then
	echo "error: base boot image not found: $BASE_IMAGE" >&2
	exit 1
fi

# Firmware is gitignored and does not follow a branch into a new worktree.
# Refuse an incomplete staging tree before changing out/.config or building.
if [ -n "$BASE_IMAGE" ]; then
for file in wcnss.mdt wcnss.b00 wcnss.b01 wcnss.b02 wcnss.b04 \
	    wcnss.b06 wcnss.b09 wcnss.b10 wcnss.b11 wcnss.b12; do
	if [ ! -s "$FW_DIR/$file" ]; then
		echo "error: missing or empty WCNSS firmware: $FW_DIR/$file" >&2
		echo "Stage it with pull-wcnss-firmware.sh or set ACK_WCNSS_FIRMWARE_DIR." >&2
		exit 1
	fi
done
for file in venus.mdt venus.b00 venus.b01 venus.b02 venus.b03 venus.b04; do
	if [ ! -s "$VENUS_FW_DIR/$file" ]; then
		echo "error: missing or empty Venus firmware: $VENUS_FW_DIR/$file" >&2
		echo "Stage it with pull-venus-firmware.sh or set ACK_VENUS_FIRMWARE_DIR." >&2
		exit 1
	fi
done
fi
if [ "$WCNSS_FALLBACK" = 0 ] && [ ! -s "$NV_FIRMWARE" ]; then
	echo "error: missing or empty WCNSS calibration: $NV_FIRMWARE" >&2
	exit 1
fi

# --- the base image ---------------------------------------------------------
# A real, SHA-verified image. Not redistributable, so it is not in the
# repository: the builder supplies it, or keeps it next to the checkout.

# The current init installs overlays whose staging labels must survive restorecon.
check_ramdisk_contexts() {
	python3 -B - "$1" "${2:-boot}" <<'PY'
import gzip
from pathlib import Path
import runpy
import struct
import sys
import zipfile

try:
    package = runpy.run_path('porting/logging/package-diag-boot.py')
    if sys.argv[2] == 'anykernel':
        with zipfile.ZipFile(sys.argv[1]) as archive:
            cpio = gzip.decompress(archive.read('ramdisk-new.cpio.gz'))
            cmdline = archive.read('cmdline').decode()
        package['verify_ack_ramdisk'](cpio, cmdline)
    else:
        image = Path(sys.argv[1]).read_bytes()
        if image[:8] != b'ANDROID!':
            raise ValueError('not an Android boot image')
        kernel_size, _, ramdisk_size, _, second_size, _, _, page, version = struct.unpack_from('<9I', image, 8)
        if page != 2048 or version != 0 or second_size != 0:
            raise ValueError('unsupported boot header')
        start = page + (kernel_size + page - 1) // page * page
        cpio = gzip.decompress(image[start:start + ramdisk_size])
    records, _ = package['parse_cpio'](cpio)
    contexts = [r['data'] for r in records if r['name'] == 'ack-vendor-file-contexts']
    if len(contexts) != 1:
        raise ValueError('expected exactly one ack-vendor-file-contexts record')
    # Check the inherited base after the same path translation used by packaging.
    package['sdhci_block_contexts'](contexts[0])
    lines = [line.split() for line in contexts[0].decode().splitlines()]
    for name in (r'ack-config-ueventd\.rc', r'ack-qrtr-vendor\.rc',
                 r'ack-usb-controller\.prop', r'ack-flags-health-check\.rc',
                 r'ack-debuggable-0\.prop'):
        pattern = '/dev/block/' + name
        matches = [line for line in lines if line and line[0] == pattern]
        if matches != [[pattern, 'u:object_r:vendor_configs_file:s0']]:
            raise ValueError(f'missing, duplicate or incorrect staging label: {pattern}')
except (OSError, ValueError, EOFError, KeyError, zipfile.BadZipFile, struct.error) as error:
    sys.exit(f'error: incompatible ramdisk in {sys.argv[1]}: {error}\n'
             'Select a verified base with the required staging and partition labels, e.g.\n'
             '  bash porting/tools/build-kernel.sh <run-name> '
             'porting/artifacts/snapshot60/boot-run380-camera.img')
print(f'Ramdisk staging and partition labels verified: {sys.argv[1]}')
PY
}
if [ -n "$BASE_IMAGE" ]; then
check_ramdisk_contexts "$BASE_IMAGE"
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
#
# With CLANG_AUTOFDO_PROFILE set below, scripts/Makefile.autofdo would
# compute the profile-use variant of these flags, but this variable is
# passed on the make command line and therefore overrides it, so the
# profile-use flags are mirrored here explicitly.
AFDO_PROFILE="$REPO/gki/aarch64/afdo/kernel.afdo"
RUST_PROFILE_FLAGS=${RUST_PROFILE_FLAGS:-"-Zdebuginfo-for-profiling -Cllvm-args=-enable-fs-discriminator=true -Cllvm-args=-improved-fs-discriminator=true -Zprofile-sample-use=$AFDO_PROFILE -Zfunction-sections=y -Cllvm-args=-split-machine-functions"}

echo "=== the toolchain ==="
"$CLANG" --version | head -1
"$RUSTC" -vV | grep -E "^release|LLVM version" || true
"$BINDGEN" --version || true
echo "=== the source tree ==="
echo "  $REPO"
echo "=== the output directory ==="
echo "  $OUT_DIR"
echo "=== the base image ==="
echo "  ${BASE_IMAGE:-existing ACK ramdisk on device}"

# --- the run's outputs ------------------------------------------------------
mkdir -p "$SNAP" "$OUT_DIR"
for path in "$SNAP/ack-tissot-$RUN.zip" "$SNAP/kdir-$RUN" "$SNAP/package-$RUN.json" \
	    "$SNAP/boot-$RUN.img" "$SNAP/package-$RUN-boot.json" \
	    "$SNAP/modules-$RUN" "$SNAP/modules-$RUN-install"; do
	if [ -e "$path" ]; then
		echo "error: $path already exists" >&2
		echo "error: choose a new run name, or move that run's outputs aside" >&2
		exit 1
	fi
done
trap 'rc=$?; printf "status=%s\n" "$rc" > "$SNAP/$RUN-build.status"' EXIT

if [ ! -s "$AFDO_PROFILE" ]; then
	echo "error: missing AutoFDO profile: $AFDO_PROFILE" >&2
	exit 1
fi
make_args=(make -C "$REPO" "O=$OUT_DIR" ARCH=arm64 LLVM=1 LLVM_IAS=1 "CC=$CLANG"
 "RUSTC=$RUSTC" "HOSTRUSTC=$HOSTRUSTC" "BINDGEN=$BINDGEN"
 "RUSTFLAGS_AUTOFDO_CLANG=$RUST_PROFILE_FLAGS"
 "CLANG_AUTOFDO_PROFILE=$AFDO_PROFILE"
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
#   The audio stack is not built here. It is CAF 4.19's machine driver
#   and PCM platforms, and they need CAF's DSP library, which this
#   kernel does not carry, so with them in the kernel does not link. The
#   work continues on the ack-audio branch; the gap and the two ways
#   forward are in
#   porting/findings/2026-10-20-audio-dsp-library-gap.md.
#   CONFIG_SLIMBUS and CONFIG_SLIM_QCOM_CTRL: the codec is a slimbus
#   slave, and the tree's slimbus subsystem is off by default.
#   CONFIG_ZRAM: the ROM's own init.rc already does
#   "chown root system /sys/block/zram0/idle" and the same for
#   writeback, so the ROM expects a zram device to be there and sizes
#   it itself.
# The CAF audio stack is not built here: it needs CAF's DSP library,
# which this kernel does not carry, and with it in the kernel does not
# link. It is built on the ack-audio branch.
for sym in KSU SLIMBUS SLIM_QCOM_CTRL ZRAM ZRAM_BACKEND_LZ4 ZRAM_BACKEND_ZSTD MSM_SMD_PKT; do
 grep -q "^CONFIG_$sym=" "$OUT_DIR/.config" && sed -i "s/^CONFIG_$sym=.*/CONFIG_$sym=y/" "$OUT_DIR/.config" || \
	echo "CONFIG_$sym=y" >> "$OUT_DIR/.config"
done
# The QTI Bluetooth HAL opens the WCNSS channels through /dev/smd2 and
# /dev/smd3, which the smd_pkt driver provides.  btqcomsmd claims the same
# channels through the WCNSS control interface and registers hci0, which
# this HAL ignores; keep it out of the kernel so the two cannot race.
sed -i "/^CONFIG_BT_QCOMSMD=/d" "$OUT_DIR/.config"
echo "# CONFIG_BT_QCOMSMD is not set" >> "$OUT_DIR/.config"
# The upstream WCD9335 and its microphone bias helper have to be off: the
# port's own versions of both are built, they register the same DAIs, and
# they define the same symbols. They are turned off here, and not only
# checked, so that a configuration which still carries them cannot produce
# a kernel with two codecs in it.
for sym in SND_SOC_WCD9335 SND_SOC_WCD_MBHC; do
 sed -i "/^CONFIG_$sym=/d" "$OUT_DIR/.config"
 echo "# CONFIG_$sym is not set" >> "$OUT_DIR/.config"
done
# The WLAN NV blob has to be available after switch_root. The vendor tree
# points /vendor/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin at
# /mnt/vendor/persist, but the firmware loader runs a read with kernel
# credentials (request_firmware() overrides current->cred), and the
# Android policy denies the kernel domain search on mnt_vendor_file. The
# ramdisk copy is gone by then, so the direct filesystem load fails with
# -EACCES and the interface never comes up.
#
# Built-in firmware is checked before any filesystem path in
# _request_firmware_prepare(), so embedding this one blob removes both the
# switch_root timing and the SELinux path from the WLAN bring-up. Only the
# NV blob is embedded: wcnss.mdt is still needed before the vendor
# partition is mounted, and it is packaged into the ramdisk below.
if [ "$WCNSS_FALLBACK" = 1 ]; then
	scripts/config --file "$OUT_DIR/.config" \
		--set-str EXTRA_FIRMWARE "" --enable FW_LOADER_USER_HELPER_FALLBACK
else
	scripts/config --file "$OUT_DIR/.config" \
		--set-str EXTRA_FIRMWARE "wlan/prima/WCNSS_qcom_wlan_nv.bin" \
		--set-str EXTRA_FIRMWARE_DIR "$(realpath "$FW_DIR")"
fi
"${make_args[@]}" olddefconfig
scripts/diffconfig "$SNAP/$RUN-prebuild.config" "$OUT_DIR/.config" | tee "$SNAP/$RUN-config.diff"
# The fingerprint pair is asserted as well: both sensor nodes are in the
# board file, and with the driver interface missing the vendor HAL
# null-derefs at enrollment (run417 shipped without them because the =y
# lived only in scripts/tissot-ack.config; kernel.config carries it now).
for sym in KSU RUST RUST_IS_AVAILABLE ASHMEM SHMEM SLIMBUS ZRAM MSM_SMD_PKT FINGERPRINT_FPC1020 FINGERPRINT_GOODIX_GF3208; do
 grep -qx "CONFIG_$sym=y" "$OUT_DIR/.config" || { echo "CONFIG_$sym is not set" >&2; exit 1; }
done
grep -qx '# CONFIG_BT_QCOMSMD is not set' "$OUT_DIR/.config" || {
 echo "CONFIG_BT_QCOMSMD is still enabled" >&2; exit 1; }
if [ "$WCNSS_FALLBACK" = 1 ]; then
 grep -qx 'CONFIG_EXTRA_FIRMWARE=""' "$OUT_DIR/.config" || {
  echo "CONFIG_EXTRA_FIRMWARE is not empty" >&2; exit 1; }
 grep -qx 'CONFIG_FW_LOADER_USER_HELPER_FALLBACK=y' "$OUT_DIR/.config" || {
  echo "CONFIG_FW_LOADER_USER_HELPER_FALLBACK is not set" >&2; exit 1; }
else
 grep -qx 'CONFIG_EXTRA_FIRMWARE="wlan/prima/WCNSS_qcom_wlan_nv.bin"' "$OUT_DIR/.config" || {
  echo "CONFIG_EXTRA_FIRMWARE was not accepted" >&2; exit 1; }
 grep -qx "CONFIG_EXTRA_FIRMWARE_DIR=\"$(realpath "$FW_DIR")\"" "$OUT_DIR/.config" || {
  echo "CONFIG_EXTRA_FIRMWARE_DIR was not accepted" >&2; exit 1; }
fi
for key in RUST_IS_AVAILABLE RUST ASHMEM SHMEM STAGING IKCONFIG IKCONFIG_PROC \
	    PSTORE_RAM QCOM_IOMMU ARM_SMMU QCOM_SCM ANDROID_BINDER_IPC \
	    USB_CONFIGFS USB_CONFIGFS_F_FS COMPAT CPUFREQ_DT \
	    QCOM_CLK_APCC_MSM8953 QCOM_APCS_IPC QCOM_RPMPD QCOM_TSENS \
	    PM_GENERIC_DOMAINS_OF; do
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
# The kernel's SELinux directory is built before the KernelSU driver,
# because the driver includes the SELinux headers and those include the
# generated flask.h. The header is generated by that directory, and the
# KernelSU objects have no dependency on it, so in a parallel build they
# would be compiled before the header exists.
"${make_args[@]}" -j"$(nproc)" security/selinux/ 2>&1 | tee -a "$LOG"
"${make_args[@]}" -j"$(nproc)" drivers/kernelsu/ 2>&1 | tee -a "$LOG"
"${make_args[@]}" -j"$(nproc)" Image.gz 2>&1 | tee -a "$LOG"

# --- the bootloader's limit -------------------------------------------------
# The phone's LK ignores the boot header's addresses: it puts the kernel at
# 0x10080000, then copies the DTB to 0x13400000 and the ramdisk to
# 0x13600000, and it never checks the kernel against either. A kernel whose
# in-memory size reaches past the DTB has its own image overwritten by the
# DTB, then zeroes the DTB with its .bss, and dies before it has a console:
# no log in any channel and no reset. run323 to run326 and run328 died that
# way (porting/findings/2026-10-21-kernel-overruns-the-dtb.md).
#
# The size that matters is image_size in the arm64 Image header: the whole
# in-memory footprint including .bss, not the file size.
LK_KERNEL_LIMIT=${ACK_LK_KERNEL_LIMIT:-54001664}	# 0x13400000 - 0x10080000
LK_HEADROOM_WARN=${ACK_LK_HEADROOM_WARN:-1048576}
image_size=$(python3 - "$OUT_DIR/arch/arm64/boot/Image" <<'PY'
import struct, sys

with open(sys.argv[1], 'rb') as image:
    head = image.read(64)
assert head[56:60] == b'ARM\x64', 'not an arm64 Image'
print(struct.unpack_from('<Q', head, 16)[0])
PY
)
headroom=$((LK_KERNEL_LIMIT - image_size))
echo "image_size=$image_size limit=$LK_KERNEL_LIMIT headroom=$headroom" | tee -a "$LOG"
if [ "$headroom" -lt 0 ]; then
	echo "error: the kernel's image_size is $((-headroom)) bytes over the" >&2
	echo "error: bootloader's limit; it would overwrite its own DTB and not boot" >&2
	exit 1
fi
if [ "$headroom" -lt "$LK_HEADROOM_WARN" ]; then
	echo "warning: only $headroom bytes left below the DTB" >&2
fi

# --- the modules ------------------------------------------------------------
# What does not fit below the DTB is built as modules: the CAF audio stack
# first. They are installed stripped into a staging tree, and the ones named
# in porting/tools/ack-modules.load, with everything modules.dep says they
# need ahead of them, go into the ramdisk's /ack-modules with a load-order
# file. The diag init loads them before Android's init starts.
MOD_LIST="$REPO/porting/tools/ack-modules.load"
MOD_INSTALL="$SNAP/modules-$RUN-install"
MOD_DIR="$SNAP/modules-$RUN"
"${make_args[@]}" -j"$(nproc)" modules 2>&1 | tee -a "$LOG"
"${make_args[@]}" INSTALL_MOD_PATH="$MOD_INSTALL" INSTALL_MOD_STRIP=1 \
	modules_install 2>&1 | tee -a "$LOG"
python3 - "$MOD_LIST" "$MOD_INSTALL" "$MOD_DIR" <<'PY'
import os, pathlib, shutil, sys

listing, install, out = (pathlib.Path(p) for p in sys.argv[1:])
releases = list((install / 'lib' / 'modules').iterdir())
assert len(releases) == 1, f'expected one kernel release, found {releases}'
release = releases[0]
deps = {}
for line in (release / 'modules.dep').read_text().splitlines():
    module, _, needs = line.partition(':')
    deps[os.path.basename(module)] = (module, [os.path.basename(n) for n in needs.split()])
wanted = [l.strip() for l in listing.read_text().splitlines()
          if l.strip() and not l.lstrip().startswith('#')]
missing = [m for m in wanted if m not in deps]
assert not missing, f'not built: {missing}'
order = []
def add(name, path=()):
    assert name not in path, f'dependency cycle: {path + (name,)}'
    if name in order:
        return
    # modules.dep lists every dependency, nearest first; load the far ones
    # first.
    for need in reversed(deps[name][1]):
        add(need, path + (name,))
    order.append(name)
for name in wanted:
    add(name)
out.mkdir()
for name in order:
    shutil.copy2(release / deps[name][0], out / name)
(out / 'load-order').write_text(''.join(n + '\n' for n in order))
print('modules:', ' '.join(order))
PY

"${make_args[@]}" dt_binding_check DT_SCHEMA_FILES=clock/qcom,apcc-msm8953.yaml
"${make_args[@]}" CHECK_DTBS=y qcom/msm8953-xiaomi-tissot.dtb
BOOT="$OUT_DIR/arch/arm64/boot"
cat "$BOOT/Image.gz" "$BOOT/dts/qcom/msm8953-xiaomi-tissot.dtb" > "$BOOT/Image.gz-dtb"

# --- the /init, rebuilt from source every time ------------------------------
"$CROSS_CC" -Os -static -DANDROID_CAPTURE \
	-DBOOT_DEADLINE_SECONDS=0 scripts/initramfs-diag/init.c \
	-o "$SNAP/init-$RUN"

if [ -z "$BASE_IMAGE" ]; then
	python3 -B porting/logging/package-kernel-update.py \
		--stage-kernel "$BOOT" --kernel-dir "$SNAP/kdir-$RUN" \
		--init "$SNAP/init-$RUN" --modules-dir "$MOD_DIR" \
		--out "$SNAP/ack-tissot-$RUN.zip" --report "$SNAP/package-$RUN.json"
	scripts/extract-ikconfig "$SNAP/kdir-$RUN/Image.gz" > "$SNAP/$RUN-embedded.config"
	for key in RUST_IS_AVAILABLE RUST ASHMEM CPUFREQ_DT QCOM_CLK_APCC_MSM8953 \
		QCOM_RPMPD PSTORE_RAM KSU ZRAM; do
		grep -qx "CONFIG_${key}=y" "$SNAP/$RUN-embedded.config" || {
			echo "error: CONFIG_$key missing from embedded config" >&2; exit 1; }
	done
	python3 - "$SNAP/ack-tissot-$RUN.zip" <<'PY'
import hashlib, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    assert archive.testzip() is None, 'corrupt AnyKernel archive'
    for line in archive.read('payload.sha256').decode().splitlines():
        digest, name = line.split()
        assert hashlib.sha256(archive.read(name)).hexdigest() == digest, name
PY
	echo "Flashable ACK kernel-update ZIP: $SNAP/ack-tissot-$RUN.zip"
	exit 0
fi

# --- the packaging ----------------------------------------------------------
# WCNSS firmware is requested by the remoteproc before the vendor partition
# is mounted, so it has to come from the ramdisk. The NV blob is built into
# the kernel, but packing the whole tree keeps the early load working. The
# staging tree is required by the preflight above; otherwise both WLAN and
# Bluetooth lose their WCNSS firmware on a fresh worktree.
#
# The UART console is dropped from the command line. The command line comes
# from the base image, so every image inherits "earlycon console=ttyMSM0,115200"
# from it. The early console is what killed the failed boots: the port's
# UART clock state is not what the bootloader left, and the first write from
# msm_serial_probe()'s own printk faults synchronously
# (porting/findings/2026-10-21-boot-reset-loop-analysis.md). The tokens are
# only dropped when the base actually carries them, so a base without them
# still packages.
fw_args=(--firmware-dir "$FW_DIR" --firmware-dir "$VENUS_FW_DIR")
sha256sum "$BASE_IMAGE" > "$SNAP/$RUN-base.sha256"
# The command line is inherited from the base image, so it carries whatever
# console the base was built with. Drop the UART console: see the comment
# above. --drop-cmdline dies on a token that is not there, so the tokens are
# filtered against the base's own command line first.
mapfile -t drop_cmdline < <(python3 - "$BASE_IMAGE" <<'PY'
import sys

CMDLINE_OFF, CMDLINE_LEN = 64, 512
with open(sys.argv[1], 'rb') as image:
    head = image.read(CMDLINE_OFF + CMDLINE_LEN)
present = set(head[CMDLINE_OFF:].split(b'\0', 1)[0].decode().split())
for token in present:
    if token in ('earlycon', 'console=ttyMSM0,115200',
                 'firmware_class.path=/lib/firmware,/vendor/firmware',
                 'firmware_class.path=/lib/firmware,/vendor/firmware,/vendor/firmware_mnt/image',
                 'androidboot.bootdevice=../..') \
            or token.startswith('androidboot.boot_devices='):
        print(token)
PY
)
drop_args=()
for token in "${drop_cmdline[@]}"; do
	drop_args+=(--drop-cmdline "$token")
done
# Preserve the working GPU firmware path and append the modem image path.
# Replacing the list with firmware_mnt alone breaks a530_pm4.fw loading.
# The DT restores the donor controller name (sdhci@7824900); the boot device
# tokens follow it, so donor userspace that greps for "sdhci" (librpmb) and
# first-stage init's boot_devices match both resolve. All tokens go in ONE
# --append-cmdline argument: the packager treats repeated flags as
# last-one-wins.
boot_tokens="androidboot.boot_devices=soc@0/7824900.sdhci"
boot_tokens="$boot_tokens androidboot.bootdevice=7824900.sdhci"
fw_path="firmware_class.path=/lib/firmware,/vendor/firmware"
fw_path="$fw_path,/vendor/firmware_mnt/image"
append_args=(--append-cmdline "$fw_path $boot_tokens")
package_args=(--base-image "$BASE_IMAGE"
 --kernel-dir "$SNAP/kdir-$RUN" --init "$SNAP/init-$RUN"
 --keep-stock-console --policy-append "$REPO/porting/policy/port-genfs-kgsl.cil"
 --modem-firmware-readable
 --modules-dir "$MOD_DIR" "${fw_args[@]}" "${drop_args[@]}" "${append_args[@]}")
python3 -B porting/logging/package-diag-boot.py "${package_args[@]}" \
 --stage-kernel "$BOOT" --format anykernel \
 --out "$SNAP/ack-tissot-$RUN.zip" --report "$SNAP/package-$RUN.json"
python3 -B porting/logging/package-diag-boot.py "${package_args[@]}" --format boot \
 --out "$SNAP/boot-$RUN.img" --report "$SNAP/package-$RUN-boot.json"
check_ramdisk_contexts "$SNAP/ack-tissot-$RUN.zip" anykernel
check_ramdisk_contexts "$SNAP/boot-$RUN.img"
scripts/extract-ikconfig "$SNAP/kdir-$RUN/Image.gz" > "$SNAP/$RUN-embedded.config"

# --- check the result -------------------------------------------------------
LLVM_NM=${LLVM_NM:-$(command -v llvm-nm || ls /usr/lib/llvm-*/bin/llvm-nm 2>/dev/null | head -1)}
SNAP="$SNAP" LLVM_NM="$LLVM_NM" RUN="$RUN" python3 - <<'PY'
import hashlib, os, pathlib, json, gzip, re, struct, subprocess, zipfile
s=pathlib.Path(os.environ['SNAP'])
run=os.environ['RUN']
new=json.loads((s/f'package-{run}.json').read_text())
assert new['format'] == 'anykernel'
artifact=s/f'ack-tissot-{run}.zip'
assert hashlib.sha256(artifact.read_bytes()).hexdigest() == new['sha256']
with zipfile.ZipFile(artifact) as archive:
    assert archive.testzip() is None, 'corrupt AnyKernel archive'
    kernel=archive.read('Image.gz-dtb')
    ramdisk=archive.read('ramdisk-new.cpio.gz')
    assert hashlib.sha256(kernel).hexdigest() == new['kernel_payload_sha256']
    assert hashlib.sha256(ramdisk).hexdigest() == new['ramdisk_sha256']
    assert archive.read('cmdline').decode().strip() == new['cmdline']
boot=json.loads((s/f'package-{run}-boot.json').read_text())
image=(s/f'boot-{run}.img').read_bytes()
assert boot['format'] == 'boot'
assert len(image) == boot['size'] == new['boot_image_size']
assert hashlib.sha256(image).hexdigest() == boot['sha256']
for key in ('kernel_payload_sha256', 'ramdisk_sha256', 'init_sha256', 'modules',
            'firmware', 'policy_append_sha256', 'modem_firmware_readable', 'cmdline'):
    assert boot[key] == new[key], f'boot image and ZIP differ: {key}'
assert image[:8] == b'ANDROID!', 'invalid boot image magic'
ksize, _, rsize, _, second, _, _, page, version=struct.unpack_from('<9I', image, 8)
assert page == 2048 and version == 0 and second == 0, 'unsupported boot header'
start=page + (ksize + page - 1) // page * page
assert image[page:page + ksize] == kernel, 'boot image and ZIP kernels differ'
assert image[start:start + rsize] == ramdisk, 'boot image and ZIP ramdisks differ'
assert image[64:576].split(b'\0', 1)[0].decode() == new['cmdline'], \
    'boot image and ZIP command lines differ'
print('Boot image and flashable ZIP payloads match.')
assert new['modem_firmware_readable'], 'the modem mount read masks were not patched'
fragment=pathlib.Path('porting/policy/port-genfs-kgsl.cil').read_bytes()
assert new['policy_append_sha256'] == hashlib.sha256(fragment).hexdigest(), \
    'the kgsl genfs fragment was not appended to the ramdisk policy'
symbols=subprocess.check_output([os.environ['LLVM_NM'],'-a','out/vmlinux'],text=True)
for name in ['ramoops_stage_write','ramoops_reset_note',
	     'kgsl_timeline_signal','snd_pcm_add_volume_ctls','snd_soc_info_multi_ext']:
    assert name in symbols, name
config=(s/f'{run}-embedded.config').read_text()
for key in ['RUST_IS_AVAILABLE','RUST','ASHMEM','CPUFREQ_DT','QCOM_CLK_APCC_MSM8953',
	    'QCOM_RPMPD','PSTORE_RAM','KSU','ZRAM']:
    assert f'CONFIG_{key}=y\n' in config, key
# The UART console must not be in the shipped command line: the early
# console faults the port's first kernel writes. See the packaging section.
cmdline_tokens=new['cmdline'].split()
assert 'earlycon' not in cmdline_tokens, 'the UART early console is still in the command line'
assert 'console=ttyMSM0,115200' not in cmdline_tokens, \
    'the UART console is still in the command line'
assert 'firmware_class.path=/lib/firmware,/vendor/firmware,/vendor/firmware_mnt/image' in cmdline_tokens, \
    'the GPU and modem firmware search paths are not in the command line'
assert 'firmware_class.path=/lib/firmware,/vendor/firmware' not in cmdline_tokens, \
    'the base firmware path was not replaced'
# Every module in the load order, and the load order itself, is in the
# ramdisk.
order=(s/f'modules-{run}'/'load-order').read_text().split()
shipped=new['modules']
assert 'ack-modules/load-order' in shipped, 'the module load order is not in the ramdisk'
for name in order:
    assert f'ack-modules/{name}' in shipped, f'{name} is not in the ramdisk'
print('The diagnostic symbols and the shipped config are as expected.')
print('modules shipped, in load order:', ' '.join(order))
print(json.dumps(new,indent=2))
for m in re.finditer(rb'Linux version [^\0\n]+',gzip.decompress((s/f'kdir-{run}/Image.gz').read_bytes())):
    print('EMBEDDED BANNER:',m.group().decode())
PY
echo "Flashable ZIP: $SNAP/ack-tissot-$RUN.zip"
echo "Boot image: $SNAP/boot-$RUN.img"
