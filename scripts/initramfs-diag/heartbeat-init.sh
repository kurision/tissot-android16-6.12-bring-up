#!/bin/busybox sh
# PID 1 stays alive; no Android init or enforcement transition is performed.
export PATH=/bin
if ! { /bin/busybox mount -t proc proc /proc &&
       /bin/busybox mount -t sysfs sysfs /sys &&
       /bin/busybox mount -t devtmpfs devtmpfs /dev; }; then
	printf 'HB: initial mounts failed; keeping PID 1 alive\n' >&2
	while :; do /bin/busybox sleep 60; done
fi
exec >/dev/kmsg 2>&1

/bin/recorder &
(
	while ! /bin/recorder --usb-setup; do /bin/busybox sleep 5; done
	while :; do
		if [ -c /dev/ttyGS0 ]; then
			/bin/busybox setsid -c /bin/busybox sh -i \
				</dev/ttyGS0 >/dev/ttyGS0 2>&1
		fi
		/bin/busybox sleep 1
	done
) &
(
	i=0
	while :; do
		printf 'HB %s\n' "$i" >/dev/kmsg
		i=$((i + 1))
		/bin/busybox sleep 1
	done
) &

/bin/busybox sleep 40
mounted=0
for d in /dev/mmcblk0p25 /dev/mmcblk1p25; do
	if /bin/busybox mount -t ext4 -o ro,noload "$d" /mnt; then
		mounted=1
		break
	fi
done
if [ "$mounted" = 1 ] &&
   /bin/busybox mount -t selinuxfs selinuxfs /sys/fs/selinux; then
	policy=/mnt/system/vendor/etc/selinux/precompiled_sepolicy
	if [ -f "$policy" ]; then
		printf 'HB: enforcement before load: '
		/bin/busybox cat /sys/fs/selinux/enforce
		printf '\nHB: loading %s\n' "$policy"
		if /bin/load-policy "$policy"; then
			printf 'HB: load returned successfully; enforcement: '
			/bin/busybox cat /sys/fs/selinux/enforce
			printf '\n'
		else
			printf 'HB: policy load failed, status=%s\n' "$?"
		fi
	else
		printf 'HB: policy file missing: %s\n' "$policy"
	fi
else
	printf 'HB: system or selinuxfs mount failed; policy not loaded\n'
fi
while :; do /bin/busybox sleep 60; done
