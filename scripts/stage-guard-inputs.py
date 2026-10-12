#!/usr/bin/env python3
"""Stage the diag init's guard inputs into a boot image's ramdisk.

scripts/initramfs-diag/init.c opens three files at boot:

    /ack-protected-fstab.qcom   read-only, bound over /vendor/etc/fstab.qcom
    /ack-vendor-file-contexts   read-only, bound over the vendor contexts
    /dev/block/ack-usb-handoff.rc   written by the guard itself

The first two must already exist in the ramdisk or install_android_guard()
never runs, which also means guard_usb_handoff() never hands the UDC to adb
and adbd can never come online (init.c:1840, init.c:1466). Nothing else
stages them, so a base image without them loses adb for the whole run.

The fstab is not the captured one: init.c validate_guard_fstab() rejects any
line carrying "formattable" and requires exactly one /data entry, pointing at
/dev/block/bootdevice/by-name/userdata as f2fs. The stock capture has an ext4
formattable /data line, which is the vold format hazard this guard exists to
block, so that line is dropped here.
"""

import argparse
import gzip
import hashlib
import struct
import subprocess
import sys
from pathlib import Path

PAGE = 2048
CPIO_MAGIC = b"070701"
CPIO_FIELDS = 13
CPIO_HDR = 6 + 8 * CPIO_FIELDS
FILESIZE_FIELD, MODE_FIELD, NAMESIZE_FIELD = 6, 1, 11
NLINK_FIELD = 4
S_IFREG = 0o100000


# Both files are taken verbatim from a base image known to boot, never
# derived. Deriving the fstab from evidence/capture/fstab.qcom loses the
# comment header and tab-padded columns and produced a file that validated
# but was not the one that worked.
STAGED = (
    ("ack-protected-fstab.qcom", 0o100644),
    ("ack-vendor-file-contexts", 0o100444),
)

# The working base's debuggable pair (fa37accb3): first-stage init loads the
# patched userdebug policy only when force_debuggable exists. snapshot59's
# base carries neither, so the stock policy loads and qseecomd (tee) is
# denied the SSD node on mmcblk0p4 (block_device) -> exit 255 loop (run30;
# bypass rules in bc871cd44). Modes are taken from the donor.
DEBUGGABLE = ("force_debuggable", "userdebug_plat_sepolicy.cil")


def die(msg):
    sys.exit(f"stage-guard-inputs: {msg}")


def align4(n):
    return (n + 3) & ~3


def parse_cpio(data: bytes):
    records, off = [], 0
    while off < len(data):
        hdr = data[off:off + CPIO_HDR]
        if len(hdr) != CPIO_HDR or hdr[:6] != CPIO_MAGIC:
            die(f"bad newc header at offset {off}")
        fields = [hdr[6 + 8 * i:14 + 8 * i].decode() for i in range(CPIO_FIELDS)]
        namesize = int(fields[NAMESIZE_FIELD], 16)
        filesize = int(fields[FILESIZE_FIELD], 16)
        name = data[off + CPIO_HDR:off + CPIO_HDR + namesize - 1].decode()
        data_off = align4(off + CPIO_HDR + namesize)
        records.append({
            "fields": fields, "name": name, "data": data[data_off:data_off + filesize],
        })
        off = align4(data_off + filesize)
        # Bases padded after the trailer (snapshot59) carry zeros, not
        # headers, past this point; keep them as the tail.
        if name == "TRAILER!!!":
            break
    return records, data[off:]


def build_cpio(records, tail):
    out = bytearray()
    for r in records:
        out += CPIO_MAGIC + "".join(r["fields"]).encode()
        out += r["name"].encode() + b"\0"
        out += b"\0" * (align4(len(out)) - len(out))
        out += r["data"]
        out += b"\0" * (align4(len(out)) - len(out))
    return bytes(out) + tail


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base-image", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--donor", default=None,
                    help="image to copy the guard files from verbatim; "
                         "required unless the base already carries them")
    ap.add_argument("--report", default=None)
    ap.add_argument("--with-debuggable", action="store_true",
                    help="also stage %s verbatim from --donor" % ", ".join(DEBUGGABLE))
    ap.add_argument("--cil", default=None,
                    help="with --with-debuggable: take userdebug_plat_sepolicy.cil "
                         "from this file instead of the donor (donor cil + appended "
                         "diagnostic rules)")
    args = ap.parse_args()
    out = Path(args.out)
    if out.exists():
        die(f"refusing to overwrite {out}")

    base = bytearray(Path(args.base_image).read_bytes())
    if base[:8] != b"ANDROID!":
        die("base image magic is not ANDROID!")
    ksize, _ka, rsize, _ra, ssize, _sa, _tags, page, version = struct.unpack_from("<9I", base, 8)
    if page != PAGE or version != 0 or ssize != 0:
        die(f"unsupported base header page={page} version={version} second={ssize}")

    ramdisk_off = PAGE + ((ksize + PAGE - 1) // PAGE * PAGE)
    records, tail = parse_cpio(gzip.decompress(bytes(base[ramdisk_off:ramdisk_off + rsize])))
    existing = {r["name"] for r in records}
    fmt = "%08X" if all(c in "0123456789ABCDEF" for r in records
                        for c in "".join(r["fields"])) else "%08x"

    donor_records = {}
    if args.donor:
        donor = bytearray(Path(args.donor).read_bytes())
        if donor[:8] != b"ANDROID!":
            die("donor image magic is not ANDROID!")
        dks, _dka, drs, _dra, dss, _dsa, _dtags, dpage, dver = struct.unpack_from("<9I", donor, 8)
        if dpage != PAGE or dver != 0 or dss != 0:
            die(f"unsupported donor header page={dpage} version={dver} second={dss}")
        doff = PAGE + ((dks + PAGE - 1) // PAGE * PAGE)
        drecs, _dtail = parse_cpio(gzip.decompress(bytes(donor[doff:doff + drs])))
        donor_records = {r["name"]: r["data"] for r in drecs}
        donor_modes = {r["name"]: int(r["fields"][MODE_FIELD], 16) for r in drecs}

    staged = list(STAGED)
    if args.with_debuggable:
        if not args.donor:
            die("--with-debuggable needs --donor")
        for name in DEBUGGABLE:
            if name not in donor_records:
                die(f"donor image has no {name}")
            staged.append((name, donor_modes[name]))
        if args.cil:
            donor_records["userdebug_plat_sepolicy.cil"] = Path(args.cil).read_bytes()

    payloads, added, replaced = [], [], []
    for name, mode in staged:
        if name in existing:
            replaced.append(name)
            continue
        if name not in donor_records:
            die(f"donor image has no {name}; point --donor at a base that boots")
        data = donor_records[name]
        if b"\0" in data:
            die(f"{name} contains a NUL byte")
        payloads.append((name, data, mode))

    trailer = records.pop() if records and records[-1]["name"] == "TRAILER!!!" else None
    for name, data, mode in payloads:
        records.append({
            "name": name,
            "fields": [fmt % 0] * CPIO_FIELDS,
            "data": data,
        })
        r = records[-1]
        r["fields"][MODE_FIELD] = fmt % (mode | S_IFREG)
        r["fields"][FILESIZE_FIELD] = fmt % len(data)
        r["fields"][NAMESIZE_FIELD] = fmt % (len(name) + 1)
        r["fields"][NLINK_FIELD] = fmt % 1
        added.append(name)
        print(f"  staged {name} ({len(data)} bytes)")
    if trailer:
        records.append(trailer)

    new_ramdisk = subprocess.run(["gzip", "-9", "-n"], input=build_cpio(records, tail),
                                 stdout=subprocess.PIPE, check=True).stdout
    base[ramdisk_off:ramdisk_off + rsize] = new_ramdisk.ljust(rsize, b"\0")
    struct.pack_into("<I", base, 16, len(new_ramdisk))
    digest = hashlib.sha1(bytes(base[PAGE:ramdisk_off]) + struct.pack("<I", ksize) +
                          new_ramdisk + struct.pack("<I", len(new_ramdisk)) +
                          struct.pack("<I", 0)).digest()
    base[576:608] = digest + b"\0" * (32 - len(digest))
    out.write_bytes(bytes(base))

    report = {
        "image": str(out), "size": len(base), "sha256": hashlib.sha256(bytes(base)).hexdigest(),
        "source": args.base_image, "cpio_records": len(records), "added": added,
        "replaced": replaced, "ramdisk_size": len(new_ramdisk), "cmdline_unchanged": True,
    }
    print(report)
    if args.report:
        Path(args.report).write_text(__import__("json").dumps(report, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
