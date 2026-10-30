# AnyKernel3 installer for the ACK Tissot port.
# osm0sis @ xda-developers; Tissot partition layout from Lucifer AnyKernel3.

properties() { '
kernel.string=ACK 6.12 for Xiaomi Mi A1
do.devicecheck=1
do.modules=0
do.systemless=0
do.cleanup=1
do.cleanuponabort=0
device.name1=tissot
supported.versions=
supported.patchlevels=
supported.vendorpatchlevels=
'; }

# Resolve by-name aliases on both the vendor and ACK device-tree layouts.
BLOCK=boot;
IS_SLOT_DEVICE=1;
SLOT_SELECT=active;
RAMDISK_COMPRESSION=auto;
# The complete ACK ramdisk replaces Magisk; KernelSU is built into this kernel.
NO_MAGISK_CHECK=1;
PATCH_VBMETA_FLAG=0;

. tools/ak3-core.sh;

case "$SLOT" in
  _a|_b) ;;
  *) abort "Invalid Tissot boot slot. Aborting...";;
esac;
case "$BLOCK" in
  */boot"$SLOT") ;;
  *) abort "Boot partition does not match the active slot. Aborting...";;
esac;

sha256sum -c payload.sha256 || abort "ACK payload checksum failed. Aborting...";
split_boot;

# Only the boot layout supported by the existing Tissot packager is accepted.
[ "$(dd if="$BOOTIMG" bs=1 count=8 2>/dev/null)" = 'ANDROID!' ] &&
  [ "$(od -An -tu4 -j24 -N4 "$BOOTIMG" | tr -d ' \n')" = 0 ] &&
  [ "$(od -An -tu4 -j36 -N4 "$BOOTIMG" | tr -d ' \n')" = 2048 ] &&
  [ "$(od -An -tu4 -j40 -N4 "$BOOTIMG" | tr -d ' \n')" = 0 ] ||
  abort "Expected Android boot header v0, 2048-byte pages, no second stage. Aborting...";
[ -f "$SPLITIMG/header" ] || abort "Missing boot header. Aborting...";

# Replace the whole command line, including any stock UART or Magisk arguments.
printf 'cmdline=%s\n' "$(cat "$AKHOME/cmdline")" > "$SPLITIMG/header" ||
  abort "Cannot stage ACK command line. Aborting...";
ui_print "Installing the ACK kernel and complete port ramdisk on boot$SLOT." \
  "Existing Magisk ramdisk modifications will be replaced.";
flash_boot;
