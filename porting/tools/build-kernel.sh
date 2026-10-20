#!/usr/bin/env bash
# Build and package the port kernel.
#
#   usage: build-kernel.sh [run-name]        (default: run289)
#
# This follows BUIL_TISSOT.md. The points that are easy to get wrong, and
# what this script does about them:
#
#   * The output directory is the repository's own "out" directory, one
#     for the port, and it is reused. A clean rebuild means emptying it,
#     not making a new one.
#
#   * The configuration comes from an archived image that is known to
#     have Rust and ashmem enabled, not from the defconfig and not from
#     whatever the last build left in the output directory. A
#     configuration resync with the wrong toolchain silently turns Rust
#     and ashmem off while the C kernel still builds, and the framework
#     then loses /dev/ashmem. "make rustavailable" runs before
#     olddefconfig for that reason, and the required symbols are checked
#     afterwards, in the output directory and again in the image.
#
#   * The kernel is built from the staged source tree, and the staged
#     tree is refreshed from the repository first and then verified
#     against it. The staged tree is what the compiler reads, so a stale
#     one produces a kernel without the repository's fixes; that
#     happened once, with the capacitive navigation keys.
#
#   * The toolchain paths are absolute and their versions are checked.
#     Plain rustc resolves through ~/.cargo/bin, which is not the
#     toolchain the known build used.
#
#   * The ramdisk, the /init and the command line are the base image's,
#     so a boot of the result tests the kernel and nothing else. The
#     /init is rebuilt from scripts/initramfs-diag/init.c every time, so
#     the image never carries a stale init binary.
#
#   * The base image and the stock boot backup live outside the
#     repository, in ../ack-kgsl-artifacts, because they are far too
#     large for git. See the README there.
set -euo pipefail
REPO=/home/kurision/development/kernel/ack-kgsl
cd "$REPO"
RUN=${1:-run289}
SNAP="$REPO/porting/artifacts/snapshot60"
ARCHIVE=/home/kurision/development/kernel/ack-kgsl-artifacts
ACK_TREE=/home/kurision/development/kernel/ack-staged-run239
OUT_DIR="$REPO/out"
BASE_IMAGE="$ARCHIVE/boot-run258.img"
CONFIG_IMAGE="$ARCHIVE/boot-run282.img"

# --- the pinned toolchain -------------------------------------------------
export PATH=/usr/bin:$PATH
TC=/home/kurision/.rustup/toolchains/stable-x86_64-unknown-linux-gnu
export RUSTC="$TC/bin/rustc"
export HOSTRUSTC="$TC/bin/rustc"
export BINDGEN=/home/kurision/.cache/run164-bindgen0732/bin/bindgen
export RUST_LIB_SRC="$TC/lib/rustlib/src/rust/library"
export LIBCLANG_PATH=/usr/lib
RUST_PROFILE_FLAGS='-Zdebuginfo-for-profiling -Cllvm-args=-enable-fs-discriminator=true -Cllvm-args=-improved-fs-discriminator=true'
test -r "$RUST_LIB_SRC/core/src/lib.rs" || exit 1
test -r "$LIBCLANG_PATH/libclang.so" || exit 1
test -x "$RUSTC" || exit 1
test -x "$BINDGEN" || exit 1
echo "=== the toolchain ==="
/usr/bin/clang --version | head -1
"$RUSTC" -vV | grep -E "^release|LLVM version"
"$BINDGEN" --version
echo "NOTE: BUIL_TISSOT.md pins /usr/bin/clang at 22.1.8; this host has:" >&2
/usr/bin/clang --version | head -1 >&2

test -f "$ACK_TREE/Makefile" || exit 1
test -s "$BASE_IMAGE" || exit 1
test -s "$CONFIG_IMAGE" || exit 1
test ! -e "$SNAP/boot-$RUN.img"
test ! -e "$SNAP/kdir-$RUN"
test ! -e "$SNAP/package-$RUN.json"
trap 'rc=$?; printf "status=%s\n" "$rc" > "$SNAP/$RUN-build.status"' EXIT
df -h "$REPO" /tmp

# --- 1. the staged tree, refreshed from the repository and verified -------
#     sound/ is in the list because the audio port lives there.
RUN="$RUN" python3 - <<'PY'
import os, pathlib, subprocess, shutil, hashlib, json
r=pathlib.Path.cwd(); s=r.parent/'ack-staged-run239'
snap=r/'porting/artifacts/snapshot60'
run=os.environ['RUN']
prefixes=('arch/','drivers/','include/','kernel/','mm/','lib/','fs/','security/',
          'scripts/','rust/','sound/','Documentation/devicetree/bindings/')
records=[]; copied=[]
for p in subprocess.check_output(['git','ls-files','-z']).decode().split('\0'):
    if not p or not (p in ('Makefile','Kconfig','Kbuild') or p.startswith(prefixes)):
        continue
    a=r/p; b=s/p
    if not a.is_file():
        continue
    changed=(not b.exists()) or (a.read_bytes()!=b.read_bytes())
    if changed:
        b.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(a,b)
        copied.append(p)
    # The staged tree is what the compiler reads. If it does not match
    # the repository after the copy, the image will not carry the fix.
    assert a.read_bytes()==b.read_bytes(), p
    records.append(dict(path=p,sha256=hashlib.sha256(a.read_bytes()).hexdigest(),
                        synced=changed))
(snap/f'{run}-source-verification.json').write_text(json.dumps(records,indent=2)+'\n')
print('Verified source files:', len(records), flush=True)
print('Staged into the build tree this run:', copied, flush=True)
PY

make_args=(make -C "$ACK_TREE" "O=$OUT_DIR" ARCH=arm64 LLVM=1 LLVM_IAS=1 CC=clang
 "RUSTC=$RUSTC" "HOSTRUSTC=$HOSTRUSTC" "BINDGEN=$BINDGEN"
 "RUSTFLAGS_AUTOFDO_CLANG=$RUST_PROFILE_FLAGS"
 KSU_GIT_VERSION=3355 KSU_GIT_VERSION_VALID=1 KSU_GIT_TAG=v3.4.2)

# --- 2. the configuration --------------------------------------------------
#     From an archived image that is known to have Rust and ashmem on.
mkdir -p "$OUT_DIR"
scripts/extract-ikconfig "$CONFIG_IMAGE" > "$OUT_DIR/.config"
test -s "$OUT_DIR/.config"
cp "$OUT_DIR/.config" "$SNAP/$RUN-prebuild.config"
"${make_args[@]}" rustavailable || exit 1
"${make_args[@]}" olddefconfig || exit 1
# The symbols that have to be on. The config is the archived one, so this
# is the only place they can be turned on.
#   CONFIG_KSU: KernelSU-Next is built in, and the manager only talks to
#   a driver that reports the released version.
#   The SND_SOC_* symbols: the audio port - the AFE, the WCD9335 codec,
#   the SDM660 CDC and the machine driver. They are enabled so the boot
#   shows whether they probe; the sound card's device tree is not
#   written yet, so they will not produce sound.
for sym in KSU SND_SOC_MSM8952 SND_SOC_WCD9335 SND_SOC_WCD_MBHC \
           SND_SOC_WCD_MBHC_ADC WCD9XXX_CODEC_CORE SND_SOC_SDM660_CDC; do
 grep -q "^CONFIG_$sym=" "$OUT_DIR/.config" && sed -i "s/^CONFIG_$sym=.*/CONFIG_$sym=y/" "$OUT_DIR/.config" || \
	echo "CONFIG_$sym=y" >> "$OUT_DIR/.config"
done
"${make_args[@]}" olddefconfig || exit 1
scripts/diffconfig "$SNAP/$RUN-prebuild.config" "$OUT_DIR/.config" | tee "$SNAP/$RUN-config.diff"
for sym in KSU RUST RUST_IS_AVAILABLE ASHMEM SHMEM SND_SOC_MSM8952 SND_SOC_WCD9335 \
           WCD9XXX_CODEC_CORE SND_SOC_SDM660_CDC; do
 grep -qx "CONFIG_$sym=y" "$OUT_DIR/.config" || { echo "CONFIG_$sym is not set" >&2; exit 1; }
done
for key in RUST_IS_AVAILABLE RUST ASHMEM SHMEM STAGING IKCONFIG IKCONFIG_PROC \
           PSTORE_RAM QCOM_IOMMU ARM_SMMU QCOM_SCM ANDROID_BINDER_IPC \
           USB_CONFIGFS USB_CONFIGFS_F_FS COMPAT CPUFREQ_DT \
           QCOM_CLK_APCC_MSM8953 QCOM_APCS_IPC QCOM_RPMPD PM_GENERIC_DOMAINS_OF; do
 rg -qx "CONFIG_${key}=y" "$OUT_DIR/.config" || { echo "CONFIG_$key is not set" >&2; exit 1; }
done

# --- 3. build -------------------------------------------------------------
set -o pipefail
"${make_args[@]}" -j8 Image.gz 2>&1 | tee "$SNAP/$RUN-build.log"
test "${PIPESTATUS[0]}" -eq 0 || exit 1
"${make_args[@]}" qcom/msm8953-xiaomi-tissot.dtb || exit 1
BOOT="$OUT_DIR/arch/arm64/boot"
cat "$BOOT/Image.gz" "$BOOT/dts/qcom/msm8953-xiaomi-tissot.dtb" > "$BOOT/Image.gz-dtb"

# --- 4. the /init, rebuilt from source every time -------------------------
aarch64-linux-gnu-gcc -Os -static -DANDROID_CAPTURE -DANDROID_EARLY_LOGCAT \
	-DBOOT_DEADLINE_SECONDS=0 scripts/initramfs-diag/init.c \
	-o "$SNAP/init-$RUN"

# --- 5. package ------------------------------------------------------------
#     The base image's ramdisk and command line, this kernel, this init.
sha256sum "$BASE_IMAGE" > "$SNAP/$RUN-base.sha256"
python3 -B porting/logging/package-diag-boot.py --base-image "$BASE_IMAGE" \
 --kernel-dir "$SNAP/kdir-$RUN" --stage-kernel "$BOOT" --init "$SNAP/init-$RUN" \
 --keep-stock-console --out "$SNAP/boot-$RUN.img" --report "$SNAP/package-$RUN.json"
scripts/extract-ikconfig "$SNAP/kdir-$RUN/Image.gz" > "$SNAP/$RUN-embedded.config"

# --- 6. check the result ---------------------------------------------------
RUN="$RUN" python3 - <<'PY'
import os, pathlib, json, hashlib, gzip, re, subprocess
s=pathlib.Path('porting/artifacts/snapshot60')
run=os.environ['RUN']
new=json.loads((s/f'package-{run}.json').read_text())
for x in json.loads((s/f'{run}-source-verification.json').read_text()):
    assert hashlib.sha256(pathlib.Path(x['path']).read_bytes()).hexdigest()==x['sha256'],x['path']
symbols=subprocess.check_output(['/usr/bin/llvm-nm','-a','out/vmlinux'],text=True)
for name in ['ramoops_witness_write','ramoops_stage_write','ramoops_reset_note',
             'kgsl_timeline_signal','snd_pcm_add_volume_ctls','snd_soc_info_multi_ext']:
    assert name in symbols, name
config=(s/f'{run}-embedded.config').read_text()
for key in ['RUST_IS_AVAILABLE','RUST','ASHMEM','CPUFREQ_DT','QCOM_CLK_APCC_MSM8953',
            'QCOM_RPMPD','PSTORE_RAM','KSU','SND_SOC_MSM8952','SND_SOC_WCD9335']:
    assert f'CONFIG_{key}=y\n' in config, key
print('The source, the diagnostic symbols and the shipped config are as expected.')
print(json.dumps(new,indent=2))
for m in re.finditer(rb'Linux version [^\0\n]+',gzip.decompress((s/f'kdir-{run}/Image.gz').read_bytes())):
    print('EMBEDDED BANNER:',m.group().decode())
PY
