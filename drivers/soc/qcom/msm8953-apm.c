// SPDX-License-Identifier: GPL-2.0-only
/*
 * MSM8953 Array Power Mux, adapted from oc-test/drivers/power/qcom/apm.c.
 * Copyright (c) 2015-2017, The Linux Foundation. All rights reserved.
 *
 * CPR must switch the memory-array supply at the donor's voltage threshold.
 * This provider only performs the switch; it must not choose voltage policy.
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <soc/qcom/apm.h>

/* Offsets within the donor's pm-apcc-glb resource. */
#define MSM8953_APM_MODE		0x2a8
#define MSM8953_APM_DELAY		0x2ac
#define MSM8953_APM_STATUS	0x2b0
#define MSM8953_APM_STATUS_MASK	GENMASK(4, 0)
#define MSM8953_APM_MX_MODE	0
#define MSM8953_APM_APCC_MODE	2
#define MSM8953_APM_MX_DONE	0
#define MSM8953_APM_APCC_DONE	3
#define MSM8953_APM_TIMEOUT_US	500

struct msm_apm_ctrl_dev {
	void __iomem *base;
	/* Serialize a complete supply switch against other switches and reads. */
	spinlock_t lock;
};

/* The mapping covers only the APM registers, not the surrounding CPU muxes. */
static int msm8953_apm_read_supply(struct msm_apm_ctrl_dev *ctrl)
{
	u32 status = readl(ctrl->base + MSM8953_APM_STATUS - MSM8953_APM_MODE);

	switch (status & MSM8953_APM_STATUS_MASK) {
	case MSM8953_APM_APCC_DONE:
		return MSM_APM_SUPPLY_APCC;
	case MSM8953_APM_MX_DONE:
		return MSM_APM_SUPPLY_MX;
	default:
		return -EIO;
	}
}

int msm_apm_get_supply(struct msm_apm_ctrl_dev *ctrl)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&ctrl->lock, flags);
	ret = msm8953_apm_read_supply(ctrl);
	spin_unlock_irqrestore(&ctrl->lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(msm_apm_get_supply);

int msm_apm_set_supply(struct msm_apm_ctrl_dev *ctrl,
		       enum msm_apm_supply supply)
{
	unsigned long flags;
	u32 mode, expected, status;
	int ret;

	switch (supply) {
	case MSM_APM_SUPPLY_APCC:
		mode = MSM8953_APM_APCC_MODE;
		expected = MSM8953_APM_APCC_DONE;
		break;
	case MSM_APM_SUPPLY_MX:
		mode = MSM8953_APM_MX_MODE;
		expected = MSM8953_APM_MX_DONE;
		break;
	default:
		return -EINVAL;
	}

	spin_lock_irqsave(&ctrl->lock, flags);
	writel(mode, ctrl->base);
	ret = readl_poll_timeout_atomic(ctrl->base + MSM8953_APM_STATUS -
				       MSM8953_APM_MODE, status,
				       (status & MSM8953_APM_STATUS_MASK) == expected,
				       1, MSM8953_APM_TIMEOUT_US);
	spin_unlock_irqrestore(&ctrl->lock, flags);

	/* A timeout is not a successful switch and must reach the CPR caller. */
	return ret;
}
EXPORT_SYMBOL_GPL(msm_apm_set_supply);

static void msm8953_apm_put_device(void *data)
{
	put_device(data);
}

/**
 * msm_apm_ctrl_dev_get() - acquire the APM supplier during consumer probe
 * @dev: consumer carrying the donor qcom,apm-ctrl phandle
 *
 * Unlike the donor's unreferenced global list, use a managed device link to
 * order supplier removal after consumer unbind. Release the platform-device
 * reference when the consumer's devres are released. No explicit put is needed.
 *
 * Return: handle or an error pointer, including -EPROBE_DEFER until bound.
 */
struct msm_apm_ctrl_dev *msm_apm_ctrl_dev_get(struct device *dev)
{
	struct msm_apm_ctrl_dev *ctrl;
	struct platform_device *supplier;
	struct device_node *np;
	int ret;

	np = of_parse_phandle(dev->of_node, "qcom,apm-ctrl", 0);
	if (!np)
		return ERR_PTR(-ENODEV);
	if (!of_device_is_available(np) ||
	    !of_device_is_compatible(np, "qcom,msm8953-apm")) {
		of_node_put(np);
		return ERR_PTR(-ENODEV);
	}

	supplier = of_find_device_by_node(np);
	of_node_put(np);
	if (!supplier)
		return ERR_PTR(-EPROBE_DEFER);

	if (!device_link_add(dev, &supplier->dev, DL_FLAG_AUTOREMOVE_CONSUMER)) {
		put_device(&supplier->dev);
		return ERR_PTR(-EINVAL);
	}

	ctrl = platform_get_drvdata(supplier);
	if (!device_is_bound(&supplier->dev) || !ctrl) {
		put_device(&supplier->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	ret = devm_add_action_or_reset(dev, msm8953_apm_put_device, &supplier->dev);
	if (ret)
		return ERR_PTR(ret);

	return ctrl;
}
EXPORT_SYMBOL_GPL(msm_apm_ctrl_dev_get);

static int msm8953_apm_probe(struct platform_device *pdev)
{
	static const char * const delays[] = {
		"qcom,apm-sel-switch-delay",
		"qcom,apm-resume-clk-delay",
		"qcom,apm-halt-clk-delay",
		"qcom,apm-post-halt-delay",
	};
	struct device *dev = &pdev->dev;
	struct msm_apm_ctrl_dev *ctrl;
	struct resource *res;
	resource_size_t start, size;
	u32 original, value, delay;
	int i, ret;

	ctrl = devm_kzalloc(dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;
	spin_lock_init(&ctrl->lock);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pm-apcc-glb");
	if (!res || resource_size(res) < MSM8953_APM_STATUS + sizeof(u32))
		return dev_err_probe(dev, -EINVAL, "missing or short pm-apcc-glb resource\n");

	/* The donor window also contains CPU clock registers. Reserve only APM. */
	start = res->start + MSM8953_APM_MODE;
	size = MSM8953_APM_STATUS - MSM8953_APM_MODE + sizeof(u32);
	if (!devm_request_mem_region(dev, start, size, dev_name(dev)))
		return -EBUSY;
	ctrl->base = devm_ioremap(dev, start, size);
	if (!ctrl->base)
		return -ENOMEM;
	/* Never assume APCC just because the zero-initialized donor cache did. */
	ret = msm_apm_get_supply(ctrl);
	if (ret < 0)
		return dev_err_probe(dev, ret, "APM supply state is not settled\n");

	original = readl(ctrl->base + MSM8953_APM_DELAY - MSM8953_APM_MODE);
	value = original;
	for (i = 0; i < ARRAY_SIZE(delays); i++) {
		if (!of_find_property(dev->of_node, delays[i], NULL))
			continue;
		ret = of_property_read_u32(dev->of_node, delays[i], &delay);
		if (ret)
			return ret;
		if (delay > 0xff)
			return dev_err_probe(dev, -EINVAL, "invalid %s\n", delays[i]);
		value &= ~(0xffU << (8 * i));
		value |= delay << (8 * i);
	}
	if (value != original) {
		writel(value, ctrl->base + MSM8953_APM_DELAY - MSM8953_APM_MODE);
		readl(ctrl->base + MSM8953_APM_DELAY - MSM8953_APM_MODE);
	}

	platform_set_drvdata(pdev, ctrl);

	return 0;
}

static const struct of_device_id msm8953_apm_match[] = {
	{ .compatible = "qcom,msm8953-apm" },
	{ }
};

static struct platform_driver msm8953_apm_driver = {
	.probe = msm8953_apm_probe,
	.driver = {
		.name = "qcom-msm8953-apm",
		.of_match_table = msm8953_apm_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(msm8953_apm_driver);

MODULE_DESCRIPTION("Qualcomm MSM8953 Array Power Mux");
MODULE_LICENSE("GPL");
