#!/usr/bin/env python3
"""Package a diagnostic Tissot boot image: base header/ramdisk, new kernel, new /init.

Only the cpio record named "init" is replaced; every other ramdisk record and
every header byte except kernel_size, ramdisk_size, cmdline and id must stay
identical, which this script asserts before writing anything.
"""

import argparse
import gzip
import hashlib
import json
import os
import struct
import subprocess
import sys

PAGE = 2048
MAX_IMAGE = 64 * 1024 * 1024
CMDLINE_OFF, CMDLINE_LEN = 64, 512
ID_OFF, ID_LEN = 576, 32
OLD_CONSOLE = " console=tty0 loglevel=8"
NEW_CONSOLE = " console=ttynull"
CPIO_MAGIC = b"070701"
CPIO_FIELDS = 13
CPIO_HDR = 6 + 8 * CPIO_FIELDS
FILESIZE_FIELD = 6
NAMESIZE_FIELD = 11
S_IFMT, S_IFREG = 0o170000, 0o100000


def die(msg):
    sys.exit(f"package-diag-boot: {msg}")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def pad(n):
    return (n + PAGE - 1) // PAGE * PAGE


def align4(n):
    return (n + 3) & ~3


def verify_kernel_dir(kdir):
    def read(name):
        with open(os.path.join(kdir, name), "rb") as f:
            return f.read()

    payload = read("Image.gz-dtb")
    if payload != read("Image.gz") + read("msm8953-xiaomi-tissot.dtb"):
        die("Image.gz-dtb != Image.gz + msm8953-xiaomi-tissot.dtb")
    for line in read("SHA256SUMS").decode().splitlines():
        if not line.strip():
            continue
        digest, name = line.split(None, 1)
        name = name.lstrip("*").strip()
        if sha256(read(name)) != digest:
            die(f"SHA256SUMS mismatch for {name}")
    return payload


def parse_cpio(data):
    """Return (records, tail). Each record: dict(fields, name, data, raw_hdr)."""
    records, off = [], 0
    while True:
        hdr = data[off:off + CPIO_HDR]
        if len(hdr) != CPIO_HDR or hdr[:6] != CPIO_MAGIC:
            die(f"bad newc header at offset {off}")
        fields = [hdr[6 + 8 * i:14 + 8 * i].decode() for i in range(CPIO_FIELDS)]
        namesize = int(fields[NAMESIZE_FIELD], 16)
        filesize = int(fields[FILESIZE_FIELD], 16)
        name_off = off + CPIO_HDR
        name = data[name_off:name_off + namesize - 1].decode()
        data_off = align4(name_off + namesize)
        end = align4(data_off + filesize)
        records.append({
            "fields": fields,
            "name": name,
            "data": data[data_off:data_off + filesize],
        })
        off = end
        if name == "TRAILER!!!":
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


def hex_upper(records):
    text = "".join("".join(r["fields"]) for r in records)
    return any(c in "ABCDEF" for c in text) and not any(c in "abcdef" for c in text)


def replace_init(cpio, init):
    records, tail = parse_cpio(cpio)
    hits = [i for i, r in enumerate(records)
            if r["name"] == "init" and int(r["fields"][1], 16) & S_IFMT == S_IFREG]
    if len(hits) != 1:
        die(f"expected exactly one regular-file record named init, found {len(hits)}")
    fmt = "%08X" if hex_upper(records) else "%08x"
    new = [dict(r, fields=list(r["fields"])) for r in records]
    new[hits[0]]["fields"][FILESIZE_FIELD] = fmt % len(init)
    new[hits[0]]["data"] = init
    out = build_cpio(new, tail)

    again, again_tail = parse_cpio(out)
    if again_tail != tail or len(again) != len(records):
        die("rebuilt cpio record count or tail differs")
    for i, (a, b) in enumerate(zip(records, again)):
        if a["name"] != b["name"]:
            die(f"record {i} name changed: {a['name']} -> {b['name']}")
        if i == hits[0]:
            fa = a["fields"][:FILESIZE_FIELD] + a["fields"][FILESIZE_FIELD + 1:]
            fb = b["fields"][:FILESIZE_FIELD] + b["fields"][FILESIZE_FIELD + 1:]
            if fa != fb or b["data"] != init:
                die("init record changed beyond filesize/data")
        elif a["fields"] != b["fields"] or a["data"] != b["data"]:
            die(f"record {i} ({a['name']}) changed")
    return out, len(records)


def new_cmdline(base):
    if base.endswith(OLD_CONSOLE):
        return base[:-len(OLD_CONSOLE)] + NEW_CONSOLE
    if base.endswith(NEW_CONSOLE):
        return base
    die(f"unexpected base cmdline: {base!r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base-image", required=True)
    ap.add_argument("--kernel-dir", required=True)
    ap.add_argument("--init", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--report", required=True)
    args = ap.parse_args()

    for path in (args.out, args.report):
        if os.path.exists(path):
            die(f"refusing to overwrite {path}")

    kernel = verify_kernel_dir(args.kernel_dir)
    with open(args.init, "rb") as f:
        init = f.read()
    with open(args.base_image, "rb") as f:
        base = f.read()

    if base[:8] != b"ANDROID!":
        die("base image magic is not ANDROID!")
    (ksize, _kaddr, rsize, _raddr, ssize, _saddr, _tags, page,
     version) = struct.unpack_from("<9I", base, 8)
    if page != PAGE or version != 0 or ssize != 0:
        die(f"unsupported base header page={page} version={version} second={ssize}")
    base_ramdisk = base[PAGE + pad(ksize):PAGE + pad(ksize) + rsize]

    cpio, records = replace_init(gzip.decompress(base_ramdisk), init)
    ramdisk = subprocess.run(["gzip", "-9", "-n"], input=cpio,
                             stdout=subprocess.PIPE, check=True).stdout

    base_cmd = base[CMDLINE_OFF:CMDLINE_OFF + CMDLINE_LEN].split(b"\0", 1)[0].decode()
    cmdline = new_cmdline(base_cmd)
    if len(cmdline) >= CMDLINE_LEN:
        die("cmdline too long")

    header = bytearray(base[:PAGE])
    struct.pack_into("<I", header, 8, len(kernel))
    struct.pack_into("<I", header, 16, len(ramdisk))
    header[CMDLINE_OFF:CMDLINE_OFF + CMDLINE_LEN] = cmdline.encode().ljust(CMDLINE_LEN, b"\0")
    digest = hashlib.sha1(kernel + struct.pack("<I", len(kernel)) + ramdisk +
                          struct.pack("<I", len(ramdisk)) + struct.pack("<I", 0)).digest()
    header[ID_OFF:ID_OFF + ID_LEN] = digest + b"\0" * (ID_LEN - len(digest))

    allowed = set(range(8, 12)) | set(range(16, 20)) | \
        set(range(CMDLINE_OFF, CMDLINE_OFF + CMDLINE_LEN)) | set(range(ID_OFF, ID_OFF + ID_LEN))
    changed = [i for i in range(PAGE) if header[i] != base[i] and i not in allowed]
    if changed:
        die(f"unexpected header bytes changed: {changed[:8]}")

    image = bytes(header) + kernel.ljust(pad(len(kernel)), b"\0") + \
        ramdisk.ljust(pad(len(ramdisk)), b"\0")
    if len(image) > MAX_IMAGE:
        die(f"image is {len(image)} bytes, over {MAX_IMAGE}")

    with open(args.out, "xb") as f:
        f.write(image)
    report = {
        "image": args.out,
        "size": len(image),
        "sha256": sha256(image),
        "kernel_payload_sha256": sha256(kernel),
        "kernel_size": len(kernel),
        "ramdisk_size": len(ramdisk),
        "ramdisk_sha256": sha256(ramdisk),
        "cpio_records": records,
        "init_sha256": sha256(init),
        "base_image": args.base_image,
        "cmdline": cmdline,
    }
    with open(args.report, "x") as f:
        json.dump(report, f, indent=2)
        f.write("\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
