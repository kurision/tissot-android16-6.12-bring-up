# AnyKernel3 installer for an existing ACK Tissot ramdisk.
properties() { '
kernel.string=ACK 6.12 kernel update for Xiaomi Mi A1
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

BLOCK=boot;
IS_SLOT_DEVICE=1;
SLOT_SELECT=active;
PATCH_VBMETA_FLAG=0;
NO_MAGISK_CHECK=1;
. tools/ak3-core.sh;

case "$SLOT:$BLOCK" in
  _a:*/boot_a|_b:*/boot_b) ;;
  *) abort "Invalid active boot slot. Aborting...";;
esac;
sha256sum -c payload.sha256 || abort "ACK payload checksum failed.";
split_boot;
[ "$(dd if="$BOOTIMG" bs=1 count=8 2>/dev/null)" = 'ANDROID!' ] &&
  [ "$(od -An -tu4 -j24 -N4 "$BOOTIMG" | tr -d ' \n')" = 0 ] &&
  [ "$(od -An -tu4 -j36 -N4 "$BOOTIMG" | tr -d ' \n')" = 2048 ] &&
  [ "$(od -An -tu4 -j40 -N4 "$BOOTIMG" | tr -d ' \n')" = 0 ] ||
  abort "Expected Android boot header v0 with 2048-byte pages.";
[ -f "$SPLITIMG/header" ] && [ -f "$SPLITIMG/ramdisk.cpio" ] ||
  abort "Missing boot header or ramdisk.";
cmdline=$(file_getprop "$SPLITIMG/header" cmdline);
case " $cmdline " in
  *" earlycon "*|*" earlycon="*|*" console=ttyMSM"*)
    abort "Installed boot command line enables the unsupported UART console.";;
esac;
case " $cmdline " in
  *" firmware_class.path=/lib/firmware,/vendor/firmware,/vendor/firmware_mnt/image "*) ;;
  *) abort "Installed boot command line lacks the ACK firmware path.";;
esac;

# Only update an existing ACK ramdisk; never turn a stock ramdisk into one.
for entry in userdebug_plat_sepolicy.cil force_debuggable ack-vendor-file-contexts ack-modules/load-order; do
  magiskboot cpio "$SPLITIMG/ramdisk.cpio" "exists $entry" ||
    abort "Installed ramdisk is not an ACK port ramdisk: missing $entry.";
done;
cp "$SPLITIMG/ramdisk.cpio" "$AKHOME/ramdisk-new.cpio" || abort "Cannot stage ramdisk.";
magiskboot cpio "$AKHOME/ramdisk-new.cpio" \
  "add 0750 init $AKHOME/ack-init" "rm -r ack-modules" "mkdir 0755 ack-modules" ||
  abort "Cannot update ACK ramdisk.";
for module in "$AKHOME"/ack-modules/*; do
  [ -f "$module" ] || abort "Missing ACK modules.";
  name=${module##*/};
  magiskboot cpio "$AKHOME/ramdisk-new.cpio" "add 0644 ack-modules/$name $module" ||
    abort "Cannot stage module $name.";
done;
ui_print "Installing ACK kernel and matching modules on boot$SLOT.";
flash_boot;
