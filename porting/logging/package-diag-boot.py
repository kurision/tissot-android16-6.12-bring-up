#!/usr/bin/env python3
"""Package a Tissot boot image or AnyKernel ZIP from a real base ramdisk.

The cpio record named "init" and explicitly supplied module files are replaced;
sdhci boot-device tokens also translate inherited MMC partition label paths.
Other ramdisk changes require the options below. Every header byte except
kernel_size, ramdisk_size, cmdline and id must stay identical. Opt-in
extensions: --firmware-dir stages files as lib/firmware/<relpath> (with
explicit dir records), --modules-dir stages files as ack-modules/<relpath>
(replacing explicitly supplied regular files in place; the diag init loads
them, not lib/modules, which Android's first-stage init
scans itself and fails the boot over), --debuggable forces ro.secure=0
ro.debuggable=1.
"""

import argparse
import gzip
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import zipfile
from pathlib import Path


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
S_IFMT, S_IFREG, S_IFDIR = 0o170000, 0o100000, 0o040000


def die(msg):
    sys.exit(f"package-diag-boot: {msg}")


def warn(msg):
    print(f"package-diag-boot: WARNING: {msg}", file=sys.stderr)


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



def stage_kernel(build_dir, kdir):
    """Copy a fresh build into the kernel dir, rebuilding Image.gz-dtb.

    A kernel dir left over from an earlier run passes verify_kernel_dir: it is
    internally consistent, just old. Booting it silently discards the change
    under test, so the copy is made here rather than trusted from the shell.
    """
    image = Path(build_dir) / "Image.gz"
    dtb = Path(build_dir) / "dts" / "qcom" / "msm8953-xiaomi-tissot.dtb"
    for src in (image, dtb):
        if not src.is_file():
            die(f"no such build artifact: {src}")
    payload = image.read_bytes() + dtb.read_bytes()
    kdir = Path(kdir)
    kdir.mkdir(parents=True, exist_ok=True)
    (kdir / "Image.gz").write_bytes(image.read_bytes())
    (kdir / "msm8953-xiaomi-tissot.dtb").write_bytes(dtb.read_bytes())
    (kdir / "Image.gz-dtb").write_bytes(payload)
    lines = [f"{sha256((kdir / n).read_bytes())}  {n}"
             for n in ("Image.gz", "msm8953-xiaomi-tissot.dtb",
                       "Image.gz-dtb")]
    (kdir / "SHA256SUMS").write_text("\n".join(lines) + "\n")
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


POLICY_CIL_RECORD = "userdebug_plat_sepolicy.cil"


def uses_sdhci(cmdline):
    return any(token in cmdline.split() for token in (
        "androidboot.boot_devices=soc@0/7824900.sdhci",
        "androidboot.bootdevice=7824900.sdhci"))


def verify_sdhci_block_contexts(data):
    prefix = "/dev/block/platform/soc@0/7824900\\.sdhci/by-name/"
    lines = [line.split() for line in data.decode().splitlines()
             if line.strip() and not line.lstrip().startswith("#")]
    if any(line[0].startswith("/dev/block/platform/soc@0/7824900\\.mmc/by-name/")
           for line in lines):
        die("stale MMC partition labels for sdhci boot device")
    for name, label in (("misc", "misc_block_device"), ("ssd", "ssd_device"),
                        ("config", "frp_block_device"),
                        ("userdata", "userdata_block_device")):
        path = prefix.replace("\\.", ".") + name
        matches = [line for line in lines if line[0].startswith(prefix)
                   and re.fullmatch(line[0], path)]
        if len(matches) != 1 or matches[0][1:] != [f"u:object_r:{label}:s0"]:
            die(f"missing, duplicate or incorrect partition label: {path}")


def sdhci_block_contexts(data):
    old = b"/dev/block/platform/soc@0/7824900\\.mmc/by-name/"
    new = b"/dev/block/platform/soc@0/7824900\\.sdhci/by-name/"
    data = b"".join(line.replace(old, new, 1) if line.lstrip().startswith(old) else line
                    for line in data.splitlines(keepends=True))
    verify_sdhci_block_contexts(data)
    return data


def readable_modem_fstab(data):
    """Change only the read masks of the existing read-only modem mount."""
    lines = data.splitlines(keepends=True)
    hits = 0
    for i, line in enumerate(lines):
        fields = line.split(b"#", 1)[0].split()
        if len(fields) < 2 or fields[1] != b"/vendor/firmware_mnt":
            continue
        if (len(fields) != 5 or
                fields[0] != b"/dev/block/bootdevice/by-name/modem" or
                fields[2] != b"vfat"):
            die("unexpected protected modem fstab entry")
        options = fields[3].split(b",")
        required = {b"ro", b"uid=1000", b"gid=1000",
                    b"context=u:object_r:firmware_file:s0"}
        if not required.issubset(options) or b"rw" in options:
            die("modem mount must remain read-only with its existing label/owner")
        for key, old, new in ((b"dmask=", b"227", b"222"),
                              (b"fmask=", b"337", b"333")):
            masks = [o for o in options if o.startswith(key)]
            if len(masks) != 1 or masks[0] not in (key + old, key + new):
                die(f"unexpected modem mount mask: {masks!r}")
            options[options.index(masks[0])] = key + new
        lines[i] = line.replace(fields[3], b",".join(options), 1)
        hits += 1
    if hits != 1:
        die(f"expected one protected modem mount, found {hits}")
    return b"".join(lines)


def customize_ramdisk(cpio, init, fw_entries, debuggable, policy_append,
                      modem_firmware_readable=False, mod_entries=(), cmdline=""):
    """Replace /init; optionally inject lib/firmware files, debuggable props,
    and extra sepolicy CIL text (harness-only policy deltas)."""
    records, tail = parse_cpio(cpio)
    hits = [i for i, r in enumerate(records)
            if r["name"] == "init" and int(r["fields"][1], 16) & S_IFMT == S_IFREG]
    if len(hits) != 1:
        die(f"expected exactly one regular-file record named init, found {len(hits)}")
    fmt = "%08X" if hex_upper(records) else "%08x"
    new = [dict(r, fields=list(r["fields"])) for r in records]
    new[hits[0]]["fields"][FILESIZE_FIELD] = fmt % len(init)
    new[hits[0]]["data"] = init
    if uses_sdhci(cmdline):
        contexts = [r for r in new if r["name"] == "ack-vendor-file-contexts"
                    and int(r["fields"][1], 16) & S_IFMT == S_IFREG]
        if len(contexts) != 1:
            die("expected exactly one regular vendor contexts record")
        contexts[0]["data"] = sdhci_block_contexts(contexts[0]["data"])
        contexts[0]["fields"][FILESIZE_FIELD] = fmt % len(contexts[0]["data"])
    if modem_firmware_readable:
        fstab = [r for r in new if r["name"] == "ack-protected-fstab.qcom"
                 and int(r["fields"][1], 16) & S_IFMT == S_IFREG]
        if len(fstab) != 1:
            die("expected exactly one regular protected fstab record")
        fstab[0]["data"] = readable_modem_fstab(fstab[0]["data"])
        fstab[0]["fields"][FILESIZE_FIELD] = fmt % len(fstab[0]["data"])
    patched_prop = None
    if debuggable:
        props = [i for i, r in enumerate(new) if r["name"] == "prop.default"]
        if len(props) != 1:
            die("debuggable: expected exactly one prop.default record")
        patched, found = [], set()
        for line in new[props[0]]["data"].decode().splitlines(keepends=True):
            key = line.split("=", 1)[0]
            if key == "ro.secure":
                line = "ro.secure=0\n"
                found.add(key)
            elif key == "ro.debuggable":
                line = "ro.debuggable=1\n"
                found.add(key)
            patched.append(line)
        if found != {"ro.secure", "ro.debuggable"}:
            die(f"debuggable: prop.default lacks expected keys: {sorted(found)}")
        patched_prop = props[0]
        new[patched_prop]["data"] = "".join(patched).encode()
        new[patched_prop]["fields"][FILESIZE_FIELD] = fmt % len(new[patched_prop]["data"])

    module_names = {name for name, _ in mod_entries}
    fw_entries = fw_entries + list(mod_entries)
    added = []
    if fw_entries:
        template = list(new[hits[0]]["fields"])
        ino = max(int(r["fields"][0], 16) for r in new) + 1
        existing = {r["name"]: r for r in new}
        parents = set()
        for arcname, _ in fw_entries:
            parts = arcname.split("/")[:-1]
            for i in range(1, len(parts) + 1):
                parents.add("/".join(parts[:i]))
        for name in sorted(parents):
            old = existing.get(name)
            if old is not None:
                if int(old["fields"][1], 16) & S_IFMT != S_IFDIR:
                    die(f"firmware parent {name!r} exists in the base "
                        "ramdisk but is not a directory")
                continue
            fields = list(template)
            fields[0] = fmt % ino
            fields[1] = fmt % 0o40755
            fields[FILESIZE_FIELD] = fmt % 0
            fields[NAMESIZE_FIELD] = fmt % (len(name) + 1)
            rec = {"fields": fields, "name": name, "data": b""}
            added.append(rec)
            existing[name] = rec
            ino += 1
        for arcname, blob in sorted(fw_entries):
            old = existing.get(arcname)
            if old is not None:
                if arcname in module_names:
                    if int(old["fields"][1], 16) & S_IFMT != S_IFREG:
                        die(f"module file {arcname!r} is not a regular file")
                    old["data"] = blob
                    old["fields"][FILESIZE_FIELD] = fmt % len(blob)
                    continue
                if old["data"] == blob:
                    # The base already carries this exact file.  Keep its
                    # record rather than adding a second one with the same
                    # name; cpio consumers do not agree on which one wins.
                    continue
                die(f"firmware file {arcname!r} already exists in the base "
                    "ramdisk with different contents")
            fields = list(template)
            fields[0] = fmt % ino
            fields[1] = fmt % 0o100644
            fields[FILESIZE_FIELD] = fmt % len(blob)
            fields[NAMESIZE_FIELD] = fmt % (len(arcname) + 1)
            rec = {"fields": fields, "name": arcname, "data": blob}
            added.append(rec)
            existing[arcname] = rec
            ino += 1
        trailer = [i for i, r in enumerate(new) if r["name"] == "TRAILER!!!"]
        if len(trailer) != 1:
            die("expected exactly one TRAILER!!! record")
        new[trailer[0]:trailer[0]] = added
    if policy_append:
        cil = [i for i, r in enumerate(new)
               if r["name"] == POLICY_CIL_RECORD]
        if len(cil) != 1:
            die(f"policy-append: expected exactly one {POLICY_CIL_RECORD} record")
        new[cil[0]]["data"] += policy_append
        new[cil[0]]["fields"][FILESIZE_FIELD] = fmt % len(new[cil[0]]["data"])

    out = build_cpio(new, tail)

    again, again_tail = parse_cpio(out)
    if again_tail != tail or len(again) != len(new):
        die("rebuilt cpio record count or tail differs")
    for i, (a, b) in enumerate(zip(new, again)):
        if a["name"] != b["name"]:
            die(f"record {i} name changed: {a['name']} -> {b['name']}")
        if i == hits[0]:
            fa = a["fields"][:FILESIZE_FIELD] + a["fields"][FILESIZE_FIELD + 1:]
            fb = b["fields"][:FILESIZE_FIELD] + b["fields"][FILESIZE_FIELD + 1:]
            if fa != fb or b["data"] != init:
                die("init record changed beyond filesize/data")
        elif i == patched_prop:
            if a["fields"] != b["fields"] or b["data"] != a["data"]:
                die("prop.default record mismatch after rebuild")
        elif a["fields"] != b["fields"] or a["data"] != b["data"]:
            die(f"record {i} ({a['name']}) changed")
    assert_unique_ramdisk(again)
    return out, len(new)


def assert_unique_ramdisk(records):
    """Refuse duplicate cpio names; warn about duplicate genfscon paths.

    Staging firmware onto a base that already carried it added a second
    lib/firmware/adsp.mdt, which is invisible in the per-record diff below and
    ships a ramdisk with two files of the same name. That is worth dying for.

    Duplicate genfscon paths are only a warning: appending a cumulative policy
    fragment to a base that already had part of it does produce them
    (boot-run142.img has 3, boot-run143.img has 5), and those images boot
    normally with the labels applied -- secilc accepts a repeated genfscon for
    the same path. So it is untidy provenance, not a broken policy.
    """
    seen = {}
    for r in records:
        seen[r["name"]] = seen.get(r["name"], 0) + 1
    dupes = sorted(n for n, c in seen.items() if c > 1)
    if dupes:
        die(f"duplicate ramdisk record names ({len(dupes)}): "
            + ", ".join(dupes[:6]) + (" ..." if len(dupes) > 6 else ""))

    cil = [r for r in records if r["name"] == POLICY_CIL_RECORD]
    if cil:
        paths = re.findall(r'genfscon\s+(\w+)\s+"([^"]*)"',
                           cil[0]["data"].decode("utf-8", "replace"))
        counts = {}
        for key in paths:
            counts[key] = counts.get(key, 0) + 1
        dupes = sorted(p for p, c in counts.items() if c > 1)
        if dupes:
            warn(f"{len(dupes)} duplicate genfscon path(s); harmless but the "
                 "base already carried them -- append only the new ones: "
                 + ", ".join(p[1][:48] for p in dupes[:3])
                 + (" ..." if len(dupes) > 3 else ""))


def new_cmdline(base, keep_console=False):
    if keep_console:
        return base
    if base.endswith(OLD_CONSOLE):
        return base[:-len(OLD_CONSOLE)] + NEW_CONSOLE
    if base.endswith(NEW_CONSOLE):
        return base
    die(f"unexpected base cmdline: {base!r}")


def verify_ack_ramdisk(cpio, cmdline=""):
    records, _ = parse_cpio(cpio)
    assert_unique_ramdisk(records)
    files = {r["name"]: r["data"] for r in records}
    for name in ("init", POLICY_CIL_RECORD, "ack-vendor-file-contexts",
                 "ack-modules/load-order"):
        if not files.get(name):
            die(f"ACK ramdisk lacks {name}")
    if "force_debuggable" not in files:
        die("ACK ramdisk lacks force_debuggable")
    if uses_sdhci(cmdline):
        verify_sdhci_block_contexts(files["ack-vendor-file-contexts"])
    lines = [line.split() for line in files["ack-vendor-file-contexts"].decode().splitlines()]
    for name in (r"ack-config-ueventd\.rc", r"ack-qrtr-vendor\.rc",
                 r"ack-usb-controller\.prop", r"ack-flags-health-check\.rc",
                 r"ack-debuggable-0\.prop"):
        pattern = "/dev/block/" + name
        matches = [line for line in lines if line and line[0] == pattern]
        if matches != [[pattern, "u:object_r:vendor_configs_file:s0"]]:
            die(f"missing, duplicate or incorrect staging label: {pattern}")
    for name in files["ack-modules/load-order"].decode().split():
        if "/" in name or not name.endswith(".ko") or not files.get("ack-modules/" + name):
            die(f"missing or invalid ACK module: {name}")


def write_anykernel(out, kernel_dir, kernel, ramdisk, cmdline):
    verify_ack_ramdisk(gzip.decompress(ramdisk), cmdline)
    image = gzip.decompress((Path(kernel_dir) / "Image.gz").read_bytes())
    if len(image) < 64 or image[56:60] != b"ARM\x64":
        die("not an arm64 Image")
    image_size = struct.unpack_from("<Q", image, 16)[0]
    # LK puts the DTB at 0x13400000 and the kernel at 0x10080000.
    if not 0 < image_size <= 54001664:
        die(f"kernel image_size={image_size} exceeds the Tissot bootloader limit")
    tokens = cmdline.split()
    if ("\n" in cmdline or "\r" in cmdline or
            any(t == "earlycon" or t.startswith("earlycon=") or
                t.startswith("console=ttyMSM") for t in tokens) or
            "firmware_class.path=/lib/firmware,/vendor/firmware,/vendor/firmware_mnt/image"
            not in tokens):
        die("AnyKernel requires the ACK console and firmware command line")
    payload = {"Image.gz-dtb": kernel, "ramdisk-new.cpio.gz": ramdisk,
               "cmdline": (cmdline + "\n").encode()}
    template = Path(__file__).resolve().parents[1] / "anykernel"
    runtime = ["anykernel.sh", "LICENSE", "README.md", "tools/ak3-core.sh",
               "tools/busybox", "tools/magiskboot",
               "META-INF/com/google/android/update-binary",
               "META-INF/com/google/android/updater-script"]
    for name in runtime:
        if not (template / name).is_file():
            die(f"missing AnyKernel runtime: {template / name}")
    with zipfile.ZipFile(out, "x", compression=zipfile.ZIP_DEFLATED) as archive:
        for name in runtime:
            archive.write(template / name, name)
        for name, data in payload.items():
            archive.writestr(name, data)
        archive.writestr("payload.sha256", "".join(
            f"{sha256(data)}  {name}\n" for name, data in payload.items()))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base-image", required=True)
    ap.add_argument("--kernel-dir", required=True)
    ap.add_argument("--init", required=True)
    ap.add_argument("--firmware-dir", default=None, action="append",
                    help="stage every file under DIR as lib/firmware/<relpath>; "
                         "may be given more than once")
    ap.add_argument("--modules-dir", default=None,
                    help="stage every file under DIR as ack-modules/<relpath>: "
                         "the kernel modules and their load-order list, which "
                         "the diag init loads")
    ap.add_argument("--debuggable", action="store_true",
                    help="force ro.secure=0 ro.debuggable=1 in prop.default")
    ap.add_argument("--keep-stock-console", action="store_true",
                    help="keep the base cmdline's console (do not force ttynull); "
                         "for fault-isolation boots where the panic must hit the panel")
    ap.add_argument("--append-cmdline", default="",
                    help="append this text to the produced cmdline "
                         "(e.g. earlycon=... console=ttyMSM0,115200)")
    ap.add_argument("--drop-cmdline", action="append", default=[],
                    metavar="TOKEN",
                    help="drop this whitespace-separated token from the "
                         "cmdline (repeatable)")
    ap.add_argument("--stage-kernel", default=None,
                    help="copy a fresh build from DIR (holding Image.gz and "
                         "dts/qcom/msm8953-xiaomi-tissot.dtb) into "
                         "--kernel-dir first, so a stale kernel dir cannot "
                         "ship silently")
    ap.add_argument("--policy-append", default=None, metavar="FILE",
                    help="append FILE's bytes to the ramdisk sepolicy CIL "
                         "record (harness-only policy deltas, e.g. to allow "
                          "a HAL past a generic-label denial)")
    ap.add_argument("--modem-firmware-readable", action="store_true",
                    help="set only the protected modem fstab masks to "
                         "0444 files/0555 directories, retaining ro and label")
    ap.add_argument("--format", choices=("boot", "anykernel"), default="boot",
                    help="output format (build-kernel.sh uses anykernel)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--report", required=True)
    args = ap.parse_args()

    if args.format == "anykernel" and not args.modules_dir:
        die("AnyKernel requires --modules-dir with modules matching this kernel")
    for path in (args.out, args.report):
        if os.path.exists(path):
            die(f"refusing to overwrite {path}")

    if args.stage_kernel:
        stage_kernel(args.stage_kernel, args.kernel_dir)
    kernel = verify_kernel_dir(args.kernel_dir)
    with open(args.init, "rb") as f:
        init = f.read()
    with open(args.base_image, "rb") as f:
        base = f.read()
    fw_entries = []
    for fw_dir in (args.firmware_dir or []):
        dir_entries = []
        for root, _dirs, files in os.walk(fw_dir):
            for fn in sorted(files):
                full = os.path.join(root, fn)
                arc = os.path.relpath(full, fw_dir).replace(os.sep, "/")
                with open(full, "rb") as f:
                    dir_entries.append(("lib/firmware/" + arc, f.read()))
        if not dir_entries:
            die(f"no files under {fw_dir}")
        seen = {name for name, _ in fw_entries}
        for name, blob in dir_entries:
            if name in seen:
                die(f"firmware file {name} staged twice (two --firmware-dir trees)")
            seen.add(name)
        fw_entries += dir_entries
    mod_entries = []
    if args.modules_dir:
        for root, _dirs, files in os.walk(args.modules_dir):
            for fn in sorted(files):
                full = os.path.join(root, fn)
                arc = os.path.relpath(full, args.modules_dir).replace(os.sep, "/")
                with open(full, "rb") as f:
                    mod_entries.append(("ack-modules/" + arc, f.read()))
        if not mod_entries:
            die(f"no files under {args.modules_dir}")

    if len(base) < PAGE or base[:8] != b"ANDROID!":
        die("base image magic is not ANDROID!")
    (ksize, _kaddr, rsize, _raddr, ssize, _saddr, _tags, page,
     version) = struct.unpack_from("<9I", base, 8)
    if page != PAGE or version != 0 or ssize != 0:
        die(f"unsupported base header page={page} version={version} second={ssize}")
    base_ramdisk = base[PAGE + pad(ksize):PAGE + pad(ksize) + rsize]

    base_cmd = base[CMDLINE_OFF:CMDLINE_OFF + CMDLINE_LEN].split(b"\0", 1)[0].decode()
    cmdline = new_cmdline(base_cmd, args.keep_stock_console)
    if args.drop_cmdline:
        present = cmdline.split()
        missing = set(args.drop_cmdline) - set(present)
        if missing:
            die(f"--drop-cmdline token not in cmdline: {sorted(missing)}")
        dropped = set(args.drop_cmdline)
        cmdline = " ".join(t for t in present if t not in dropped)
    if args.append_cmdline.strip():
        cmdline += " " + args.append_cmdline.strip()
    if len(cmdline) >= CMDLINE_LEN:
        die("cmdline too long")

    policy_append = b""
    if args.policy_append:
        with open(args.policy_append, "rb") as f:
            policy_append = f.read()
    cpio, records = customize_ramdisk(gzip.decompress(base_ramdisk), init,
                                      fw_entries, args.debuggable,
                                      policy_append, args.modem_firmware_readable,
                                      mod_entries, cmdline)
    ramdisk = subprocess.run(["gzip", "-9", "-n"], input=cpio,
                             stdout=subprocess.PIPE, check=True).stdout

    boot_size = PAGE + pad(len(kernel)) + pad(len(ramdisk))
    if boot_size > MAX_IMAGE:
        die(f"image is {boot_size} bytes, over {MAX_IMAGE}")

    if args.format == "anykernel":
        write_anykernel(args.out, args.kernel_dir, kernel, ramdisk, cmdline)
        image = Path(args.out).read_bytes()
    else:
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
        with open(args.out, "xb") as f:
            f.write(image)
    report = {
        "format": args.format,
        "image": args.out,
        "size": len(image),
        "boot_image_size": boot_size,
        "sha256": sha256(image),
        "kernel_payload_sha256": sha256(kernel),
        "kernel_size": len(kernel),
        "ramdisk_size": len(ramdisk),
        "ramdisk_sha256": sha256(ramdisk),
        "cpio_records": records,
        "init_sha256": sha256(init),
        "firmware": [name for name, _ in fw_entries],
        "modules": {name: sha256(blob) for name, blob in mod_entries},
        "debuggable": args.debuggable,
        "policy_append_sha256": sha256(policy_append) if policy_append else None,
        "modem_firmware_readable": args.modem_firmware_readable,
        "cmdline": cmdline,
        "base_image": args.base_image,
    }
    with open(args.report, "x") as f:
        json.dump(report, f, indent=2)
        f.write("\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
