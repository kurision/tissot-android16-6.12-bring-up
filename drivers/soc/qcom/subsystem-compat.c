// SPDX-License-Identifier: GPL-2.0-only
/*
 * The CAF 4.19 subsystem framework, as far as the CAF video codec
 * (vidc_3x) uses it.
 *
 * 6.12 has no subsystem framework: the driver that owns the subsystem
 * (the Venus firmware loader, the clocks and the power domain) does the
 * work itself.  This file provides the two entry points the CAF video
 * codec calls:
 *
 *  - msm_subsystem_get_with_fwname() loads the firmware the way this port's
 *    own upstream Venus driver does it: request_firmware() for the .mdt,
 *    qcom_mdt_load() into the reserved memory the device tree names,
 *    and qcom_scm_pas_auth_and_reset() to let the secure world start it.
 *
 *  - the subsystem restart notification is a no-op, because there is no
 *    framework to register with.  A Venus crash does not need it: the
 *    driver releases the core on SYS_ERROR, which shuts the firmware
 *    down through msm_subsystem_put(), and the next session loads it
 *    again through msm_subsystem_get_with_fwname().  What is lost is
 *    the donor's ramdump on crash (subsystem_crashed() only set
 *    do_ramdump_on_put).
 */

#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <soc/qcom/msm_subsystem_compat.h>

#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/soc/qcom/mdt_loader.h>

#define VENUS_PAS_ID		9
#define VENUS_FW_MEM_SIZE	(6 * SZ_1M)

struct subsys_cookie {
	struct device *dev;
	void *mem_va;
	phys_addr_t mem_phys;
	size_t mem_size;
};

/**
 * msm_subsystem_get_with_fwname() - load a subsystem's firmware
 * @dev: the device the firmware is loaded for
 * @name: the name of the subsystem
 * @fw_name: the name of the firmware image
 *
 * The firmware is loaded into the reserved memory the device tree names
 * with "memory-region", the same way this port's upstream Venus driver
 * loads it.
 */
void *msm_subsystem_get_with_fwname(struct device *dev, const char *name,
				const char *fw_name)
{
	struct subsys_cookie *cookie;
	struct device_node *node;
	struct reserved_mem *rmem;
	const struct firmware *mdt;
	ssize_t fw_size;
	char fwname[64];
	int ret;

	cookie = kzalloc(sizeof(*cookie), GFP_KERNEL);
	if (!cookie)
		return ERR_PTR(-ENOMEM);
	cookie->dev = dev;

	node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!node) {
		ret = -EINVAL;
		goto err_cookie;
	}
	rmem = of_reserved_mem_lookup(node);
	of_node_put(node);
	if (!rmem) {
		ret = -EINVAL;
		goto err_cookie;
	}

	snprintf(fwname, sizeof(fwname), "%s.mdt", fw_name);
	ret = request_firmware(&mdt, fwname, dev);
	if (ret)
		goto err_cookie;

	fw_size = qcom_mdt_get_size(mdt);
	if (fw_size < 0) {
		ret = fw_size;
		goto err_release_fw;
	}

	cookie->mem_phys = rmem->base;
	cookie->mem_size = rmem->size;
	if (cookie->mem_size < fw_size || fw_size > VENUS_FW_MEM_SIZE) {
		ret = -EINVAL;
		goto err_release_fw;
	}

	cookie->mem_va = memremap(cookie->mem_phys, cookie->mem_size,
				  MEMREMAP_WC);
	if (!cookie->mem_va) {
		ret = -ENOMEM;
		goto err_release_fw;
	}

	ret = qcom_mdt_load(dev, mdt, fwname, VENUS_PAS_ID, cookie->mem_va,
			    cookie->mem_phys, cookie->mem_size, NULL);
	if (ret)
		goto err_unmap;

	/* Let the secure world start the firmware. */
	ret = qcom_scm_pas_auth_and_reset(VENUS_PAS_ID);
	if (ret)
		goto err_unmap;

	release_firmware(mdt);

	return cookie;

err_unmap:
	memunmap(cookie->mem_va);
err_release_fw:
	release_firmware(mdt);
err_cookie:
	kfree(cookie);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(msm_subsystem_get_with_fwname);

/**
 * msm_subsystem_put() - release a subsystem
 * @subsystem: the handle from msm_subsystem_get_with_fwname()
 */
void msm_subsystem_put(void *subsystem)
{
	struct subsys_cookie *cookie = subsystem;

	if (IS_ERR_OR_NULL(cookie))
		return;

	qcom_scm_pas_shutdown(VENUS_PAS_ID);
	memunmap(cookie->mem_va);
	kfree(cookie);
}
EXPORT_SYMBOL_GPL(msm_subsystem_put);

/**
 * msm_subsys_notif_register_notifier() - register for a subsystem's restart
 *
 * There is no subsystem framework in this tree, so nothing is ever
 * notified.  The handle is a placeholder that
 * msm_subsys_notif_unregister_notifier() accepts.
 */
void *msm_subsys_notif_register_notifier(const char *name,
				     struct notifier_block *nb)
{
	return NULL;
}
EXPORT_SYMBOL_GPL(msm_subsys_notif_register_notifier);

/**
 * msm_subsys_notif_unregister_notifier() - unregister a restart notifier
 */
void msm_subsys_notif_unregister_notifier(void *handle,
				       struct notifier_block *nb)
{
}
EXPORT_SYMBOL_GPL(msm_subsys_notif_unregister_notifier);

/**
 * msm_subsystem_crashed() - tell the framework that a subsystem crashed
 * @name: the name of the subsystem
 *
 * There is no subsystem framework in this tree, so there is nothing to
 * tell.
 */
void msm_subsystem_crashed(const char *name)
{
}
EXPORT_SYMBOL_GPL(msm_subsystem_crashed);
