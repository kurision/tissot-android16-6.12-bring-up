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
 *
 * It also provides the userspace side of the framework, for the vendor
 * peripheral manager (pm-service, through libmdmdetect): the
 * "msm_subsys" bus with one subsysN device per remote processor
 * (name, state, crash_count, restart_level, firmware_name), and a
 * /dev/subsys_<name> node whose open() votes the processor on and whose
 * release() drops the vote.  The donor's subsystem_restart.c did both
 * with subsystem_get()/subsystem_put(); here they are rproc_boot() and
 * rproc_shutdown(), which are reference counted the same way, so a vote
 * never stops a processor that the remoteproc core booted by itself.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
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

/*
 * The CAF names userspace looks the processors up by, and the remote
 * processor that backs each one.  The names are the donor's
 * qcom,firmware-name values (msm8953.dtsi); libmdmdetect matches on
 * them, not on the subsysN index.
 */
static const struct {
	const char *name;
	const char *compatible;
} ssr_map[] = {
	{ "modem", "qcom,msm8953-mss-pil" },
	{ "adsp", "qcom,msm8953-adsp-pil" },
	{ "wcnss", "qcom,pronto-v3-pil" },
};

struct ssr_subsys {
	struct device dev;
	struct cdev cdev;
	struct rproc *rproc;
	const char *name;
	bool restart_system;
};

static DEFINE_MUTEX(ssr_lock);
static struct ssr_subsys *ssr_subsys[ARRAY_SIZE(ssr_map)];
static struct class *ssr_class;
static dev_t ssr_devt;

#define to_ssr(d) container_of(d, struct ssr_subsys, dev)

static ssize_t name_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	return sysfs_emit(buf, "%s\n", to_ssr(dev)->name);
}
static DEVICE_ATTR_RO(name);

/* The donor's three states; a crashed processor is being restarted. */
static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	switch (READ_ONCE(to_ssr(dev)->rproc->state)) {
	case RPROC_RUNNING:
	case RPROC_ATTACHED:
		return sysfs_emit(buf, "ONLINE\n");
	case RPROC_CRASHED:
		return sysfs_emit(buf, "OFFLINING\n");
	default:
		return sysfs_emit(buf, "OFFLINE\n");
	}
}
static DEVICE_ATTR_RO(state);

static ssize_t crash_count_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(to_ssr(dev)->rproc->crash_cnt));
}
static DEVICE_ATTR_RO(crash_count);

/*
 * The vendor init writes RELATED here for every subsystem.  RELATED is
 * what the remoteproc core does anyway (it recovers the processor on
 * its own); SYSTEM, which made the donor panic the phone on a crash, is
 * recorded and reported but not acted on.
 */
static ssize_t restart_level_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  to_ssr(dev)->restart_system ? "SYSTEM" : "RELATED");
}

static ssize_t restart_level_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	if (sysfs_streq(buf, "SYSTEM"))
		to_ssr(dev)->restart_system = true;
	else if (sysfs_streq(buf, "RELATED"))
		to_ssr(dev)->restart_system = false;
	else
		return -EINVAL;

	return count;
}
static DEVICE_ATTR_RW(restart_level);

static ssize_t firmware_name_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", to_ssr(dev)->rproc->firmware);
}
static DEVICE_ATTR_RO(firmware_name);

static struct attribute *ssr_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_state.attr,
	&dev_attr_crash_count.attr,
	&dev_attr_restart_level.attr,
	&dev_attr_firmware_name.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ssr);

static const struct bus_type ssr_bus = {
	.name		= "msm_subsys",
	.dev_groups	= ssr_groups,
};

/*
 * A vote.  The remoteproc core booted the processor itself (auto_boot)
 * and holds its own reference, so rproc_boot() only takes another one
 * and rproc_shutdown() only drops it: closing the last vote does not
 * stop a processor the core started.  rproc_shutdown() keeps the
 * reference if the processor is not running at that moment (crashed and
 * being recovered); the donor's subsystem_put() had the same window.
 */
static int ssr_open(struct inode *inode, struct file *file)
{
	struct ssr_subsys *s = container_of(inode->i_cdev, struct ssr_subsys,
					    cdev);
	int ret;

	ret = rproc_boot(s->rproc);
	if (ret)
		return ret;

	file->private_data = s;
	return 0;
}

static int ssr_release(struct inode *inode, struct file *file)
{
	struct ssr_subsys *s = file->private_data;

	rproc_shutdown(s->rproc);
	return 0;
}

static const struct file_operations ssr_fops = {
	.owner		= THIS_MODULE,
	.open		= ssr_open,
	.release	= ssr_release,
	.llseek		= noop_llseek,
};

/* Nothing removes a subsystem: the remote processors are built in. */
static void ssr_dev_release(struct device *dev)
{
	kfree(to_ssr(dev));
}

static int ssr_is_rproc(struct device *dev, const void *data)
{
	return dev->class && !strcmp(dev->class->name, "remoteproc");
}

/* Called with ssr_lock held. */
static struct ssr_subsys *ssr_create(struct device *pdev, unsigned int i)
{
	struct device *rdev, *cdev;
	struct ssr_subsys *s;
	int ret;

	/* The remote processor registers itself in probe, before the bind. */
	rdev = device_find_child(pdev, NULL, ssr_is_rproc);
	if (!rdev) {
		dev_warn(pdev, "no remote processor for subsys_%s\n",
			 ssr_map[i].name);
		return NULL;
	}

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) {
		put_device(rdev);
		return NULL;
	}
	s->rproc = container_of(rdev, struct rproc, dev);
	s->name = ssr_map[i].name;

	device_initialize(&s->dev);
	s->dev.bus = &ssr_bus;
	s->dev.parent = pdev;
	s->dev.release = ssr_dev_release;
	ret = dev_set_name(&s->dev, "subsys%u", i);
	if (!ret)
		ret = device_add(&s->dev);
	if (ret)
		goto err_put;

	cdev_init(&s->cdev, &ssr_fops);
	s->cdev.owner = THIS_MODULE;
	ret = cdev_add(&s->cdev, MKDEV(MAJOR(ssr_devt), i), 1);
	if (ret)
		goto err_del;

	cdev = device_create(ssr_class, pdev, MKDEV(MAJOR(ssr_devt), i), s,
			     "subsys_%s", s->name);
	if (IS_ERR(cdev)) {
		ret = PTR_ERR(cdev);
		goto err_cdev;
	}

	dev_info(pdev, "subsys%u: %s\n", i, s->name);
	return s;

err_cdev:
	cdev_del(&s->cdev);
err_del:
	device_del(&s->dev);
err_put:
	dev_err(pdev, "subsys_%s: %d\n", ssr_map[i].name, ret);
	put_device(rdev);
	put_device(&s->dev);
	return NULL;
}

static void ssr_add(struct device *pdev)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ssr_map); i++)
		if (of_device_is_compatible(pdev->of_node, ssr_map[i].compatible))
			break;
	if (i == ARRAY_SIZE(ssr_map))
		return;

	mutex_lock(&ssr_lock);
	if (!ssr_subsys[i])
		ssr_subsys[i] = ssr_create(pdev, i);
	mutex_unlock(&ssr_lock);
}

static int ssr_platform_notify(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	if (action == BUS_NOTIFY_BOUND_DRIVER)
		ssr_add(data);
	return NOTIFY_DONE;
}

static struct notifier_block ssr_platform_nb = {
	.notifier_call = ssr_platform_notify,
};

static int ssr_add_bound(struct device *dev, void *data)
{
	if (dev->driver)
		ssr_add(dev);
	return 0;
}

static int __init ssr_init(void)
{
	int ret;

	ret = bus_register(&ssr_bus);
	if (ret)
		goto err;

	ssr_class = class_create("subsys");
	if (IS_ERR(ssr_class)) {
		ret = PTR_ERR(ssr_class);
		goto err_bus;
	}

	ret = alloc_chrdev_region(&ssr_devt, 0, ARRAY_SIZE(ssr_map), "subsys");
	if (ret)
		goto err_class;

	/* Processors bound later are added from the notifier. */
	ret = bus_register_notifier(&platform_bus_type, &ssr_platform_nb);
	if (ret)
		goto err_region;
	bus_for_each_dev(&platform_bus_type, NULL, NULL, ssr_add_bound);

	return 0;

err_region:
	unregister_chrdev_region(ssr_devt, ARRAY_SIZE(ssr_map));
err_class:
	class_destroy(ssr_class);
err_bus:
	bus_unregister(&ssr_bus);
err:
	pr_err("msm_subsys: %d\n", ret);
	return ret;
}
subsys_initcall(ssr_init);
