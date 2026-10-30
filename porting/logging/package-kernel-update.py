#!/usr/bin/env python3
"""Package an AnyKernel update for an existing ACK Tissot ramdisk."""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import runpy
import struct
import zipfile


ROOT = Path(__file__).resolve().parents[2]
package = runpy.run_path(str(ROOT / "porting/logging/package-diag-boot.py"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("stage-kernel", "kernel-dir", "init", "modules-dir", "out", "report"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    kdir, modules, out, report = map(Path, (args.kernel_dir, args.modules_dir, args.out, args.report))
    if out.exists() or report.exists() or kdir.exists():
        parser.error("output, report or kernel directory already exists")
    order = (modules / "load-order").read_text().splitlines()
    if not order or any(not re.fullmatch(r"[A-Za-z0-9_.+-]+\.ko", name) for name in order):
        parser.error("invalid module load order")
    if len(order) != len(set(order)):
        parser.error("duplicate module in load order")
    payload = {"ack-init": Path(args.init).read_bytes(),
               "ack-modules/load-order": (modules / "load-order").read_bytes()}
    for name in order:
        payload["ack-modules/" + name] = (modules / name).read_bytes()
    if any(not data for data in payload.values()):
        parser.error("empty init or module payload")
    kernel = package["stage_kernel"](args.stage_kernel, kdir)
    image = gzip.decompress((kdir / "Image.gz").read_bytes())
    if len(image) < 64 or image[56:60] != b"ARM\x64":
        parser.error("not an arm64 Image")
    size = struct.unpack_from("<Q", image, 16)[0]
    if not 0 < size <= 54001664:
        parser.error("kernel image exceeds the Tissot bootloader limit")
    payload["Image.gz-dtb"] = kernel
    template = ROOT / "porting/anykernel"
    runtime = ("LICENSE", "tools/ak3-core.sh", "tools/busybox", "tools/magiskboot",
               "META-INF/com/google/android/update-binary", "META-INF/com/google/android/updater-script")
    with zipfile.ZipFile(out, "x", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.write(template / "kernel-update.sh", "anykernel.sh")
        for name in runtime:
            archive.write(template / name, name)
        for name, data in payload.items():
            archive.writestr(name, data)
        archive.writestr("payload.sha256", "".join(
            f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in payload.items()))
    report.write_text(json.dumps({"format": "kernel-update", "sha256": package["sha256"](out.read_bytes()),
                                  "kernel_sha256": package["sha256"](kernel), "modules": order}, indent=2) + "\n")


if __name__ == "__main__":
    main()
