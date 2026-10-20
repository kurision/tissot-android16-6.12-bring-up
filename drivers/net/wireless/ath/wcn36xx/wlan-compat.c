// SPDX-License-Identifier: GPL-2.0-only
/*
 * Android/vendor compatibility glue for the mainline wcn36xx driver.
 *
 * Two platform details are not part of the mainline driver:
 *
 *  - The stock Android Wi-Fi HAL (libwifi-hal.so) still speaks the
 *    prima/qcacld driver contract.  It probes
 *    /sys/module/wlan/parameters/fwpath before it will load the driver or
 *    create an interface.  The firmware is already running by the time the
 *    HAL starts, so the node only needs to exist and be writable.
 *
 *  - The stock bootloader does not add local-mac-address to the WLAN node,
 *    while the Android user space expects a stable, non-zero MAC.  Derive
 *    one from the device serial number when the board asks for it with
 *    qcom,mac-address-from-serial (the same 02:00:<serial[0:7]> convention
 *    lk2nd uses for this board).
 */

#include <linux/etherdevice.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "wcn36xx.h"

#ifdef CONFIG_SYSFS
#ifndef MODULE
/*
 * The vendor HAL opens this path with O_WRONLY after access(W_OK), and runs
 * as uid wifi with the sysfs inode owned by root, so the node is 0666.
 * SELinux still restricts writers through the genfscon rule for
 * "/module/wlan/parameters/fwpath" (sysfs_wlan_fwpath); that rule is why
 * this has to be a real sysfs node under /sys/module/wlan and not a symlink
 * to /sys/module/wcn36xx, whose target would carry the generic sysfs label.
 */
#define WCN36XX_FWPATH_MAX	256

static char wcn36xx_fwpath[WCN36XX_FWPATH_MAX];
static struct kobject *wcn36xx_wlan_kobj;
static struct kobject *wcn36xx_wlan_params_kobj;

static ssize_t wcn36xx_fwpath_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", wcn36xx_fwpath);
}

static ssize_t wcn36xx_fwpath_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	size_t len = count;

	/* The HAL writes strlen(state) + 1, including the trailing NUL. */
	if (len && buf[len - 1] == '\0')
		len--;
	if (len >= sizeof(wcn36xx_fwpath))
		len = sizeof(wcn36xx_fwpath) - 1;
	memcpy(wcn36xx_fwpath, buf, len);
	wcn36xx_fwpath[len] = '\0';
	return count;
}

static struct kobj_attribute wcn36xx_fwpath_attr =
	__ATTR(fwpath, 0644, wcn36xx_fwpath_show, wcn36xx_fwpath_store);

static int __init wcn36xx_wlan_compat_init(void)
{
	int ret;

	if (!module_kset)
		return -ENODEV;

	wcn36xx_wlan_kobj = kobject_create_and_add("wlan", &module_kset->kobj);
	if (!wcn36xx_wlan_kobj)
		return -ENOMEM;

	wcn36xx_wlan_params_kobj =
		kobject_create_and_add("parameters", wcn36xx_wlan_kobj);
	if (!wcn36xx_wlan_params_kobj) {
		ret = -ENOMEM;
		goto put_wlan;
	}

	ret = sysfs_create_file(wcn36xx_wlan_params_kobj,
				&wcn36xx_fwpath_attr.attr);
	if (ret)
		goto put_parameters;

	/*
	 * __ATTR cannot carry a world-writable mode (VERIFY_OCTAL_PERMISSIONS
	 * rejects it), but access(W_OK) from uid wifi has to succeed.  The
	 * sysfs inode is root-owned, so widen the mode after creation.  The
	 * kernel side is root here and kernfs_setattr() bypasses the VFS
	 * permission check; SELinux still decides who may actually write.
	 */
	ret = sysfs_chmod_file(wcn36xx_wlan_params_kobj,
			       &wcn36xx_fwpath_attr.attr, 0666);
	if (ret)
		pr_warn("wcn36xx: cannot make fwpath writable: %d\n", ret);

	pr_info("wcn36xx: created /sys/module/wlan/parameters/fwpath\n");
	return 0;

put_parameters:
	kobject_put(wcn36xx_wlan_params_kobj);
put_wlan:
	kobject_put(wcn36xx_wlan_kobj);
	return ret;
}
late_initcall(wcn36xx_wlan_compat_init);
#endif /* !MODULE */
#endif /* CONFIG_SYSFS */

static int wcn36xx_hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool wcn36xx_serial_to_mac(const char *serial, u8 *addr)
{
	int i;

	if (!serial || strlen(serial) < 8)
		return false;

	memset(addr, 0, ETH_ALEN);
	addr[0] = 0x02;
	addr[1] = 0x00;
	for (i = 0; i < 4; i++) {
		int hi = wcn36xx_hex_nibble(serial[2 * i]);
		int lo = wcn36xx_hex_nibble(serial[2 * i + 1]);

		if (hi < 0 || lo < 0)
			return false;
		addr[2 + i] = (hi << 4) | lo;
	}

	return is_valid_ether_addr(addr);
}

static bool wcn36xx_serial_from_cmdline(char *serial, size_t size)
{
#ifndef MODULE
	static const char key[] = "androidboot.serialno=";
	const char *p = saved_command_line;
	size_t len;

	if (!p)
		return false;
	p = strstr(p, key);
	if (!p)
		return false;
	p += sizeof(key) - 1;
	len = strcspn(p, " \t\n");
	if (!len || len >= size)
		return false;
	memcpy(serial, p, len);
	serial[len] = '\0';
	return true;
#else
	(void)serial;
	(void)size;
	return false;
#endif
}

bool wcn36xx_compat_mac_from_serial(u8 *addr)
{
	const char *serial = NULL;
	char cmdline_serial[32];

	if (!of_property_read_string(of_root, "serial-number", &serial) &&
	    wcn36xx_serial_to_mac(serial, addr))
		return true;

	if (wcn36xx_serial_from_cmdline(cmdline_serial,
					sizeof(cmdline_serial)) &&
	    wcn36xx_serial_to_mac(cmdline_serial, addr))
		return true;

	return false;
}
