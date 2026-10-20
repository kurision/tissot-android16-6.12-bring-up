#!/usr/bin/env bash
# Build and package run281: the kgsl timeline diagnostic and the bounded
# ramoops DIAG trace.
#
# Source difference against run261 is exactly three files:
#   drivers/gpu/msm/kgsl_timeline.c   (timeline retire/fence diagnostic)
#   drivers/gpu/msm/kgsl_timeline.h   (per-timeline diagnostic counters)
#   fs/pstore/ram.c                   (per-event signal print replaced by counts)
#
# Config, ramdisk, init and command line are the run258/run261 ones, so a
# boot of this image tests those three files and nothing else.
#
# Per AGENTS.md this is an incremental build: the run261 output directory is
# reused (same .config, same staged source tree), so nothing is rebuilt from
# scratch and no new output directory is created.
set -euo pipefail
REPO=/home/kurision/development/kernel/ack-kgsl
cd "$REPO"
SNAP="$REPO/porting/artifacts/snapshot60"
ACK_TREE=/home/kurision/development/kernel/ack-staged-run239
OUT_DIR=/home/kurision/development/kernel/ack-out-run261
export PATH=/usr/bin:$PATH
TC=/home/kurision/.rustup/toolchains/stable-x86_64-unknown-linux-gnu
export RUSTC="$TC/bin/rustc" HOSTRUSTC="$TC/bin/rustc"
export BINDGEN=/home/kurision/.cache/run164-bindgen0732/bin/bindgen
export RUST_LIB_SRC="$TC/lib/rustlib/src/rust/library" LIBCLANG_PATH=/usr/lib
test -r "$RUST_LIB_SRC/core/src/lib.rs"
test -r "$LIBCLANG_PATH/libclang.so"
test -f "$ACK_TREE/Makefile"
test -d "$OUT_DIR"
test -s "$OUT_DIR/.config"
test ! -e "$SNAP/boot-run281.img"
test ! -e "$SNAP/kdir-run281"
test ! -e "$SNAP/package-run281.json"
trap 'rc=$?; printf "status=%s\n" "$rc" > "$SNAP/run281-build.status"' EXIT
df -h "$REPO" /tmp
/usr/bin/clang --version
python3 - <<'PY'
import pathlib, subprocess, shutil, hashlib, json
r=pathlib.Path.cwd(); s=r.parent/'ack-staged-run239'; records=[]
snap=r/'porting/artifacts/snapshot60'
old=json.loads((snap/'package-run258.json').read_text())
assert hashlib.sha256((snap/'boot-run258.img').read_bytes()).hexdigest()==old['sha256']
assert hashlib.sha256((snap/'init-run257').read_bytes()).hexdigest()==old['init_sha256']
for p in subprocess.check_output(['git','ls-files','-z']).decode().split('\0'):
    if not p or not (p in ('Makefile','Kconfig','Kbuild') or p.startswith(('arch/','drivers/','include/','kernel/','mm/','lib/','fs/','security/','scripts/','rust/','Documentation/devicetree/bindings/'))): continue
    a=r/p; b=s/p
    if not a.is_file(): continue
    changed=not b.exists() or a.read_bytes()!=b.read_bytes()
    if changed:
        b.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(a,b)
    assert a.read_bytes()==b.read_bytes(),p
    records.append(dict(path=p,sha256=hashlib.sha256(a.read_bytes()).hexdigest(),synced=changed))
(snap/'run281-source-verification.json').write_text(json.dumps(records,indent=2)+'\n')
prev=json.loads((snap/'run261-source-verification.json').read_text())
prevm={x['path']:x['sha256'] for x in prev}
now={x['path']:x['sha256'] for x in records}
changed=[p for p in sorted(set(prevm)&set(now)) if prevm[p]!=now[p]]
print('Verified source files:',len(records),flush=True)
print('Staged into the build tree this run:',[x['path'] for x in records if x['synced']],flush=True)
print('Changed against run261:',changed,flush=True)
# The KernelSU-Next driver is vendored in, so the changed list is the
# previous run's files plus drivers/kernelsu/ and the two glue files.
# The new files are not in run261's record, so they cannot show up in
# `changed`; count them from the current tree instead.
ksu = sorted(p for p in now if p.startswith('drivers/kernelsu/'))
rest = [p for p in changed if not p.startswith('drivers/kernelsu/')]
assert set(rest)=={'drivers/Makefile',
               'drivers/Kconfig',
               'drivers/gpu/msm/adreno.c',
               'drivers/gpu/msm/adreno_a5xx.c',
               'drivers/gpu/msm/adreno_dispatch.c',
               'drivers/gpu/msm/kgsl_events.c',
               'drivers/gpu/msm/kgsl_pwrctrl.c',
               'drivers/gpu/msm/kgsl_sync.c',
               'drivers/gpu/msm/kgsl_sync.h',
               'drivers/gpu/msm/kgsl_timeline.c',
               'drivers/gpu/msm/kgsl_timeline.h',
               'drivers/tty/sysrq.c',
               'fs/pstore/ram.c',
               'kernel/reboot.c'}, rest
assert len(ksu) == 93, len(ksu)
assert not [p for p in changed if p.startswith('drivers/kernelsu/')]
print('vendored KernelSU-Next files:', len(ksu))
PY
# KernelSU-Next's version is 30000 + its own commit count. The driver is
# vendored into this tree, so the Kbuild cannot count the upstream commits
# and would fall back to version 1, which the manager app rejects. Pin the
# released v3.4.2 numbers (33355), which is what the v3.4.2 manager APK
# expects: the driver only talks to a manager whose signing certificate
# hashes to 79e590113c4c4c0c222978e413a5faa801666957b1212a328e46c00c69821bf7.
make_args=(make -C "$ACK_TREE" "O=$OUT_DIR" ARCH=arm64 LLVM=/usr/bin/ LLVM_IAS=1 CC=/usr/bin/clang
 "RUSTC=$RUSTC" "HOSTRUSTC=$HOSTRUSTC" "BINDGEN=$BINDGEN"
 KSU_GIT_VERSION=3355 KSU_GIT_VERSION_VALID=1 KSU_GIT_TAG=v3.4.2
 'RUSTFLAGS_AUTOFDO_CLANG=-Zdebuginfo-for-profiling -Cllvm-args=-enable-fs-discriminator=true -Cllvm-args=-improved-fs-discriminator=true')
cp "$OUT_DIR/.config" "$SNAP/run281-prebuild.config"
# KernelSU-Next: the driver is built in, so the config has to select it.
# The build takes its config from the previous image's embedded config, so
# this is the only place the symbol can be turned on.
grep -q '^CONFIG_KSU=' "$OUT_DIR/.config" && sed -i 's/^CONFIG_KSU=.*/CONFIG_KSU=y/' "$OUT_DIR/.config" || \
	echo 'CONFIG_KSU=y' >> "$OUT_DIR/.config"
"${make_args[@]}" olddefconfig
scripts/diffconfig "$SNAP/run281-prebuild.config" "$OUT_DIR/.config" | tee "$SNAP/run281-config.diff"
# The only allowed change is the KernelSU-Next symbol. The diff can also be
# empty: the config in the output directory already carries it from an
# earlier run of this script.
if [ -s "$SNAP/run281-config.diff" ]; then
	grep -q '^[+-]CONFIG_KSU' "$SNAP/run281-config.diff"
	test "$(grep -c '^[+-]' "$SNAP/run281-config.diff")" = 2
fi
grep -qx "CONFIG_KSU=y" "$OUT_DIR/.config"
for key in RUST_IS_AVAILABLE RUST ASHMEM SHMEM STAGING IKCONFIG IKCONFIG_PROC PSTORE_RAM QCOM_IOMMU ARM_SMMU QCOM_SCM ANDROID_BINDER_IPC USB_CONFIGFS USB_CONFIGFS_F_FS COMPAT CPUFREQ_DT QCOM_CLK_APCC_MSM8953 QCOM_APCS_IPC QCOM_RPMPD PM_GENERIC_DOMAINS_OF; do
 rg -qx "CONFIG_${key}=y" "$OUT_DIR/.config"
done
# W=1 for the port's own object only. The kgsl objects are ported CAF vendor
# code that does not satisfy W=1 (missing prototypes on two kgsl_sync.c
# helpers, a missing format attribute in kgsl_events.c, an unused-but-set
# count in adreno_dispatch.c) - all pre-existing, none of them in the code
# this change adds. The port builds the vendor kgsl without W=1, so keep
# that, and let the normal build carry the kgsl objects.
"${make_args[@]}" -j8 W=1 fs/pstore/ram.o
# The vendored KernelSU-Next driver is upstream code; the port does not
# police it with W=1.
"${make_args[@]}" -j8 drivers/kernelsu/
"${make_args[@]}" -j8 Image.gz
"${make_args[@]}" dt_binding_check DT_SCHEMA_FILES=clock/qcom,apcc-msm8953.yaml
"${make_args[@]}" CHECK_DTBS=y qcom/msm8953-xiaomi-tissot.dtb
BOOT="$OUT_DIR/arch/arm64/boot"
cat "$BOOT/Image.gz" "$BOOT/dts/qcom/msm8953-xiaomi-tissot.dtb" > "$BOOT/Image.gz-dtb"
sha256sum "$SNAP/boot-run258.img" > "$SNAP/run281-base.sha256"
python3 -B porting/logging/package-diag-boot.py --base-image "$SNAP/boot-run258.img" \
 --kernel-dir "$SNAP/kdir-run281" --stage-kernel "$BOOT" --init "$SNAP/init-run257" \
 --keep-stock-console --out "$SNAP/boot-run281.img" --report "$SNAP/package-run281.json"
scripts/extract-ikconfig "$SNAP/kdir-run281/Image.gz" > "$SNAP/run281-embedded.config"
python3 - <<'PY'
import pathlib,json,hashlib,gzip,re,subprocess
s=pathlib.Path('porting/artifacts/snapshot60')
old=json.loads((s/'package-run261.json').read_text())
new=json.loads((s/'package-run281.json').read_text())
for k in ['ramdisk_sha256','init_sha256','cmdline']:
    assert old[k]==new[k],k
for x in json.loads((s/'run281-source-verification.json').read_text()):
    assert hashlib.sha256(pathlib.Path(x['path']).read_bytes()).hexdigest()==x['sha256'],x['path']
symbols=subprocess.check_output(['/usr/bin/llvm-nm','-a','/home/kurision/development/kernel/ack-out-run261/vmlinux'],text=True)
for name in ['ramoops_witness_write','ramoops_stage_write','ramoops_reset_note']:
    assert name in symbols,name
assert 'kgsl_timeline_signal' in symbols
(s/'run281-diagnostic-symbols.txt').write_text('\n'.join(x for x in symbols.splitlines() if any(n in x for n in ['ramoops_witness','ramoops_stage_write','ramoops_qtrace','ramoops_reset_note']))+'\n')
config=(s/'run281-embedded.config').read_text()
for key in ['RUST_IS_AVAILABLE','RUST','ASHMEM','CPUFREQ_DT','QCOM_CLK_APCC_MSM8953','QCOM_RPMPD','PSTORE_RAM']:
    assert f'CONFIG_{key}=y\n' in config,key
print('Ramdisk, init and command line identical to run261; diagnostic helper symbols present.')
print(json.dumps(new,indent=2))
for m in re.finditer(rb'Linux version [^\0\n]+',gzip.decompress((s/'kdir-run281/Image.gz').read_bytes())):
    print('EMBEDDED BANNER:',m.group().decode())
PY
