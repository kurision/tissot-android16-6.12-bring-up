#!/usr/bin/env bash
# Repack a boot image for `fastboot boot`: swap in a freshly built kernel and
# rewrite the kernel command line. Header-only change plus the payload, so this
# is fast and needs no magiskboot, no zip, and no flashing.
#
# Defaults produce ../ack-boot-test/boot-ack-612.img from the known-good
# boot-ack-6.18.img base (which carries the unmodified CAF ramdisk and has
# already reached the splash under fastboot boot).
#
# Nothing is flashed by this script.
set -euo pipefail

if [[ $# != 0 ]]; then
    echo "Usage: $0"
    echo "Overrides: BASE_IMAGE, KERNEL, OUT_IMAGE"
    if [[ ${1:-} == --help ]]; then exit 0; fi
    exit 2
fi

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
base_image=${BASE_IMAGE:-$repo_dir/../ack-boot-test/boot-ack-6.18.img}
kernel=${KERNEL:-$repo_dir/../ack-android16-6.12/out/arch/arm64/boot/Image.gz-dtb}
out_image=${OUT_IMAGE:-$repo_dir/../ack-boot-test/boot-ack-612.img}

# Optional RAMDISK=<path> replaces the Android ramdisk wholesale. Used by the
# diagnostic initramfs in scripts/initramfs-diag, which substitutes a static
# /init for Android's first stage so the kernel log can be captured without a
# UART and without RAM surviving the reset.
ramdisk_override=${RAMDISK:-}
command -v python3 >/dev/null || { echo "Missing tool: python3" >&2; exit 1; }

# androidboot.boot_devices must name the platform device the way the running
# kernel names it. CAF's DTS calls the eMMC host soc/7824900.sdhci; upstream
# builds the name from of_device_make_bus_id() as "<addr-hex>.<node-name>" with
# the unit address stripped by %pOFn, i.e. 7824900.mmc. With the CAF name, init
# never marks the eMMC as the boot device and never creates
# /dev/block/by-name/system, so first-stage init cannot mount /system.
#
# pstore_blk.best_effort=1 is required: without it pstore/blk registers its
# write path only on a panic, and a stall produces no panic. PARTLABEL is
# required because early_boot_devpath() resolves the name through
# block/early-lookup.c before Android creates /dev/block/by-name.
CMDLINE=${CMDLINE:-androidboot.hardware=qcom msm_rtb.filter=0x237 ehci-hcd.park=3 androidboot.bootdevice=7824900.mmc loop.max_part=7 androidboot.boot_devices=7824900.mmc buildvariant=user ignore_loglevel printk.time=1 oops=panic panic=10 pstore_blk.blkdev=PARTLABEL=logdump pstore_blk.best_effort=1}

python3 - "$base_image" "$kernel" "$out_image" "$CMDLINE" "$ramdisk_override" <<'PY'
import struct
import sys
from pathlib import Path

args = sys.argv[1:]
base_path, kernel_path, out_path = (Path(a) for a in args[:3])
cmdline, ramdisk_override = args[3], args[4]
MAGIC = b'ANDROID!'
PAGE = 2048
ARGS_SIZE = 512          # boot_img_hdr.cmdline
ID_OFF = 64 + ARGS_SIZE  # boot_img_hdr.id, 32 bytes; not rewritten, see below

for path in (base_path, kernel_path):
    if not path.is_file():
        sys.exit(f'Missing input: {path}')
if ramdisk_override and not Path(ramdisk_override).is_file():
    sys.exit(f'Missing ramdisk override: {ramdisk_override}')

base = base_path.read_bytes()
if base[:8] != MAGIC:
    sys.exit(f'{base_path} is not an Android boot image')

hdr = bytearray(base[:PAGE])
page_size = struct.unpack_from('<I', hdr, 36)[0] or PAGE
header_version = struct.unpack_from('<I', hdr, 40)[0]
if page_size != PAGE or header_version != 0:
    sys.exit(f'Unsupported header: page_size={page_size} version={header_version}')

kernel_size = struct.unpack_from('<I', hdr, 8)[0]
ramdisk_size = struct.unpack_from('<I', hdr, 16)[0]

# Payload layout: header page, kernel padded to a page, ramdisk padded to a page,
# then trailing padding out to the image size.
kernel_off = PAGE
ramdisk_off = kernel_off + (kernel_size + PAGE - 1) // PAGE * PAGE
if ramdisk_off + ramdisk_size > len(base):
    sys.exit(f'{base_path} is truncated: needs {ramdisk_off + ramdisk_size} bytes, has {len(base)}')
ramdisk = Path(ramdisk_override).read_bytes() if ramdisk_override \
    else base[ramdisk_off:ramdisk_off + ramdisk_size]
struct.pack_into('<I', hdr, 16, len(ramdisk))

encoded = cmdline.encode()
if len(encoded) >= ARGS_SIZE:
    sys.exit(f'cmdline is {len(encoded)} bytes, must be under {ARGS_SIZE}')

# The header id is a SHA-1 over kernel/ramdisk/second payload only. mkbootimg
# never hashes cmdline or extra_cmdline, so rewriting the command line leaves the
# existing digest valid; changing the kernel only changes its length.
struct.pack_into('<I', hdr, 8, kernel_path.stat().st_size)
hdr[64:ID_OFF] = encoded + b'\0' * (ARGS_SIZE - len(encoded))

payload = kernel_path.read_bytes()
def pad(blob):
    return blob + b'\0' * (-len(blob) % PAGE)

image = bytes(hdr) + pad(payload) + pad(ramdisk)
image = image + b'\0' * (len(base) - len(image)) if len(image) < len(base) else image
out_path.write_bytes(image)

print(f'base    {base_path} ({len(base)} bytes, ramdisk {ramdisk_size} bytes)')
print(f'kernel  {kernel_path} ({len(payload)} bytes)')
print(f'wrote   {out_path} ({len(image)} bytes)')
print(f'cmdline {cmdline}')
PY

echo
echo "verify:"
python3 - "$out_image" <<'PY'
import struct
import sys
from pathlib import Path
h = Path(sys.argv[1]).read_bytes()[:2048]
print('  magic       ', h[:8].decode())
print('  kernel_size ', struct.unpack_from('<I', h, 8)[0])
print('  ramdisk_size', struct.unpack_from('<I', h, 16)[0])
print('  cmdline     ', h[64:576].split(b'\0')[0].decode())
PY