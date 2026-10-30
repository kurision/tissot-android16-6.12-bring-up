/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The CAF 4.19 subsystem framework is not in 6.12.  The CAF video codec
 * (vidc_3x) uses it for two things: the Venus firmware load, and the
 * subsystem restart notification.
 *
 * The firmware load is done here, the way this port's own upstream Venus
 * driver does it: request_firmware() for the .mdt, qcom_mdt_load() into
 * the reserved memory the device tree names, and
 * qcom_scm_pas_auth_and_reset() to let the secure world start it.
 *
 * The subsystem restart notification is a no-op, and a Venus crash does
 * not need it: the driver recovers by itself.  On SYS_ERROR,
 * handle_sys_error() releases the core (__unload_fw() ->
 * msm_subsystem_put(), which shuts the firmware down), and the next
 * session's core init loads and starts it again (__load_fw() ->
 * msm_subsystem_get_with_fwname()).  The donor's subsystem_crashed()
 * only asked for a ramdump on the next put
 * (../oc-test/drivers/soc/qcom/subsystem_restart.c:1287), so the ramdump
 * is what is lost.  The notifier is used only by venus_boot.c, for the
 * non-secure PIL, which msm8953 does not use.
 */
#ifndef _SOC_QCOM_MSM_SUBSYSTEM_COMPAT_H
#define _SOC_QCOM_MSM_SUBSYSTEM_COMPAT_H

#include <linux/notifier.h>
#include <linux/types.h>

struct device;
struct platform_device;

enum crash_status {
	CRASH_STATUS_NO_CRASH = 0,
	CRASH_STATUS_ERR_FATAL,
	CRASH_STATUS_WDOG_BITE,
};

/**
 * struct notif_data - additional notif information
 * @crashed: indicates if subsystem has crashed due to wdog bite or err fatal
 * @enable_ramdump: ramdumps disabled if set to 0
 * @enable_mini_ramdumps: enable flag for minimized critical-memory-only
 * ramdumps
 * @no_auth: set if subsystem does not use PIL to bring it out of reset
 * @pdev: subsystem platform device pointer
 */
struct notif_data {
	enum crash_status crashed;
	int enable_ramdump;
	int enable_mini_ramdumps;
	bool no_auth;
	struct platform_device *pdev;
};

/**
 * msm_subsystem_get_with_fwname() - load a subsystem's firmware
 * @dev: the device the firmware is loaded for
 * @name: the name of the subsystem ("venus")
 * @fw_name: the name of the firmware image ("venus")
 *
 * Returns a handle on success, an error pointer on failure.
 */
void *msm_subsystem_get_with_fwname(struct device *dev, const char *name,
				const char *fw_name);

/**
 * msm_subsystem_put() - release a subsystem
 * @subsystem: the handle from msm_subsystem_get_with_fwname()
 */
void msm_subsystem_put(void *subsystem);

/**
 * msm_subsys_notif_register_notifier() - register for a subsystem's restart
 * @name: the name of the subsystem
 * @nb: the notifier block
 *
 * There is no subsystem framework in this tree, so nothing is ever
 * notified and the returned handle is a placeholder.
 */
void *msm_subsys_notif_register_notifier(const char *name,
				     struct notifier_block *nb);

/**
 * msm_subsys_notif_unregister_notifier() - unregister a restart notifier
 */
void msm_subsys_notif_unregister_notifier(void *handle,
				       struct notifier_block *nb);

/**
 * msm_subsystem_crashed() - tell the framework that a subsystem crashed
 * @name: the name of the subsystem
 *
 * There is no subsystem framework in this tree, so this does nothing.
 * The CAF video codec calls it when the firmware reports an error.
 */
void msm_subsystem_crashed(const char *name);

#endif /* _SOC_QCOM_MSM_SUBSYSTEM_COMPAT_H */
