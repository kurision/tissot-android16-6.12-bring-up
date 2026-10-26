# ACK Tissot AnyKernel3

The installer replaces the active slot's kernel, appended DTB, entire ramdisk
and command line. This allows installation over stock/Lucifer as well as an
existing ACK port. ROM compatibility still requires a hardware boot test;
the Tissot device check does not prove that every Tissot ROM is compatible.

Runtime files are copied unchanged from osm0sis/AnyKernel3 commit
`020dfeccf9d7e962a48400fc94d3e451df92eead`:

- `tools/ak3-core.sh`, `tools/busybox`, `tools/magiskboot`
- `META-INF/com/google/android/update-binary` and `updater-script`
- `LICENSE`

Source: https://github.com/osm0sis/AnyKernel3/tree/020dfeccf9d7e962a48400fc94d3e451df92eead

The Tissot A/B layout follows Lucifer AnyKernel3. `BLOCK=boot` uses upstream
by-name resolution across the vendor and ACK device-tree paths:
https://github.com/d4rk-lucif3r/Anykernel3-Tissot

Only the runtime needed for this header-v0 boot partition is included. The
installer does not use systemless module injection: matching modules live in
the supplied ramdisk's `/ack-modules` and are loaded by the port's init.
Magisk ramdisk changes are replaced; KernelSU support is built into the port.

Use `porting/tools/build-kernel.sh` to produce a ZIP. See `BUILD_TISSOT.md`
for packaging from existing outputs, installation, and rollback. Generated
ZIPs contain the device-derived base ramdisk and firmware; treat them with
the same distribution restrictions as the existing diagnostic boot images.
