// SPDX-License-Identifier: GPL-2.0-only
/*
 * MSM8953 subset of oc-test/drivers/regulator/mem-acc-regulator.c.
 * Copyright (c) 2014-2018, 2020, The Linux Foundation. All rights reserved.
 *
 * The donor regulator ABI uses corner numbers 1 and 2, NOT physical uV.
 * CPR owns the ordering of corner changes relative to APC voltage changes.
 */

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>

struct msm8953_mem_acc {
	void __iomem *base[2];
};

static int msm8953_mem_acc_get_sel(struct regulator_dev *rdev)
{
	struct msm8953_mem_acc *acc = rdev_get_drvdata(rdev);
	u32 l1 = readl(acc->base[0]) & BIT(0);
	u32 l2 = readl(acc->base[1]) & BIT(0);

	if (l1 != l2)
		return -EIO;

	/* Hardware 1 means corner 1 (selector 0), hardware 0 means corner 2. */
	return !l1;
}

static int msm8953_mem_acc_set_sel(struct regulator_dev *rdev, unsigned int sel)
{
	struct msm8953_mem_acc *acc = rdev_get_drvdata(rdev);
	unsigned int i;
	u32 value;
	int old;

	if (sel >= 2)
		return -EINVAL;
	old = msm8953_mem_acc_get_sel(rdev);
	if (old < 0)
		return old;
	if (old == (int)sel)
		return 0;

	/* Preserve donor L1-then-L2 order and all unrelated register bits. */
	for (i = 0; i < ARRAY_SIZE(acc->base); i++) {
		value = readl(acc->base[i]);
		value = (value & ~BIT(0)) | !sel;
		writel(value, acc->base[i]);
	}

	old = msm8953_mem_acc_get_sel(rdev);
	return old < 0 ? old : (old == (int)sel ? 0 : -EIO);
}

static const struct regulator_ops msm8953_mem_acc_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = msm8953_mem_acc_get_sel,
	.set_voltage_sel = msm8953_mem_acc_set_sel,
};

static const struct regulator_desc msm8953_mem_acc_desc = {
	.name = "apc_mem_acc_corner",
	.owner = THIS_MODULE,
	.type = REGULATOR_VOLTAGE,
	.ops = &msm8953_mem_acc_ops,
	.n_voltages = 2,
	.min_uV = 1,
	.uV_step = 1,
};

static int msm8953_mem_acc_probe(struct platform_device *pdev)
{
	static const char * const names[] = { "acc-sel-l1", "acc-sel-l2" };
	static const char * const positions[] = {
		"qcom,acc-sel-l1-bit-pos", "qcom,acc-sel-l2-bit-pos",
	};
	static const char * const widths[] = {
		"qcom,acc-sel-l1-bit-size", "qcom,acc-sel-l2-bit-size",
	};
	struct device *dev = &pdev->dev;
	struct regulator_config config = { };
	struct msm8953_mem_acc *acc;
	struct regulator_dev *rdev;
	struct resource *res;
	u32 map[2], value;
	int i, ret;

	/* Do not claim the donor's generic compatible on other SoCs. */
	if (!of_machine_is_compatible("qcom,msm8953"))
		return -ENODEV;
	if (of_property_count_u32_elems(dev->of_node, "qcom,corner-acc-map") != 2)
		return -EINVAL;
	ret = of_property_read_u32_array(dev->of_node, "qcom,corner-acc-map", map, 2);
	if (ret)
		return ret;
	if (map[0] != 1 || map[1] != 0)
		return dev_err_probe(dev, -EINVAL, "unsupported memory-ACC corner map\n");

	acc = devm_kzalloc(dev, sizeof(*acc), GFP_KERNEL);
	if (!acc)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (of_property_count_u32_elems(dev->of_node, positions[i]) != 1 ||
		    of_property_count_u32_elems(dev->of_node, widths[i]) != 1)
			return -EINVAL;
		ret = of_property_read_u32(dev->of_node, positions[i], &value);
		if (ret || value)
			return -EINVAL;
		ret = of_property_read_u32(dev->of_node, widths[i], &value);
		if (ret || value != 1)
			return -EINVAL;
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, names[i]);
		if (!res || resource_size(res) < sizeof(u32))
			return -EINVAL;
		acc->base[i] = devm_platform_ioremap_resource_byname(pdev, names[i]);
		if (IS_ERR(acc->base[i]))
			return PTR_ERR(acc->base[i]);
	}

	config.dev = dev;
	config.of_node = dev->of_node;
	config.driver_data = acc;
	config.init_data = of_get_regulator_init_data(dev, dev->of_node,
						      &msm8953_mem_acc_desc);
	rdev = devm_regulator_register(dev, &msm8953_mem_acc_desc, &config);
	return PTR_ERR_OR_ZERO(rdev);
}

static const struct of_device_id msm8953_mem_acc_match[] = {
	{ .compatible = "qcom,mem-acc-regulator" },
	{ }
};

static struct platform_driver msm8953_mem_acc_driver = {
	.probe = msm8953_mem_acc_probe,
	.driver = {
		.name = "qcom-msm8953-mem-acc",
		.of_match_table = msm8953_mem_acc_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(msm8953_mem_acc_driver);

MODULE_DESCRIPTION("Qualcomm MSM8953 CPU memory-ACC corner regulator");
MODULE_LICENSE("GPL");
