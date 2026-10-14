// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017-18 Linaro Limited

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/reboot-mode.h>
#include <linux/regmap.h>

#define PON_SOFT_RB_SPARE		0x8f

/* PON peripheral (pm8941 family): cause of the previous reset, sticky. */
#define PON_REASON1			0x08

#define GEN1_REASON_SHIFT		2
#define GEN2_REASON_SHIFT		1

#define NO_REASON_SHIFT			0

struct pm8916_pon {
	struct device *dev;
	struct regmap *regmap;
	u32 baseaddr;
	struct reboot_mode_driver reboot_mode;
	long reason_shift;
};

static int pm8916_reboot_mode_write(struct reboot_mode_driver *reboot,
				    unsigned int magic)
{
	struct pm8916_pon *pon = container_of
			(reboot, struct pm8916_pon, reboot_mode);
	int ret;

	ret = regmap_update_bits(pon->regmap,
				 pon->baseaddr + PON_SOFT_RB_SPARE,
				 GENMASK(7, pon->reason_shift),
				 magic << pon->reason_shift);
	if (ret < 0)
		dev_err(pon->dev, "update reboot mode bits failed\n");

	return ret;
}

static int pm8916_pon_probe(struct platform_device *pdev)
{
	struct pm8916_pon *pon;
	long reason_shift;
	int error;

	pon = devm_kzalloc(&pdev->dev, sizeof(*pon), GFP_KERNEL);
	if (!pon)
		return -ENOMEM;

	pon->dev = &pdev->dev;

	pon->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!pon->regmap) {
		dev_err(&pdev->dev, "failed to locate regmap\n");
		return -ENODEV;
	}

	error = of_property_read_u32(pdev->dev.of_node, "reg",
				     &pon->baseaddr);
	if (error)
		return error;

	reason_shift = (long)of_device_get_match_data(&pdev->dev);

	if (reason_shift != NO_REASON_SHIFT) {
		pon->reboot_mode.dev = &pdev->dev;
		pon->reason_shift = reason_shift;
		pon->reboot_mode.write = pm8916_reboot_mode_write;
		error = devm_reboot_mode_register(&pdev->dev, &pon->reboot_mode);
		if (error) {
			dev_err(&pdev->dev, "can't register reboot mode\n");
			return error;
		}
	}

	/*
	 * Snapshot60 run9x diagnostic: report the previous reset's PON
	 * cause as raw register, decoded offline. Plain read, no side
	 * effects; one line per boot, like any probe message.
	 * Run115: also snapshot POFF causes and the PMIC WDT S1/S2 state
	 * (offsets from the pm8916 PON block; same regmap). A firmware-
	 * armed PMIC WDT with S2 enabled and no kernel petter would bite
	 * silently, like the APPS WDT dumped from ramoops_probe.
	 */
#define PON_POFF_REASON1	0x0c
#define PON_POFF_REASON2	0x0d
#define PON_WD_S1_TIMER		0x54
#define PON_WD_S2_TIMER		0x55
#define PON_WD_S2_CTL		0x56
#define PON_WD_S2_CTL2		0x57
	{
		unsigned int reason = 0, poff1 = 0, poff2 = 0;
		unsigned int s1 = 0, s2 = 0, s2ctl = 0, s2ctl2 = 0;
		int rerr;

		rerr = regmap_read(pon->regmap,
				   pon->baseaddr + PON_REASON1, &reason);
		regmap_read(pon->regmap,
			    pon->baseaddr + PON_POFF_REASON1, &poff1);
		regmap_read(pon->regmap,
			    pon->baseaddr + PON_POFF_REASON2, &poff2);
		regmap_read(pon->regmap,
			    pon->baseaddr + PON_WD_S1_TIMER, &s1);
		regmap_read(pon->regmap,
			    pon->baseaddr + PON_WD_S2_TIMER, &s2);
		regmap_read(pon->regmap,
			    pon->baseaddr + PON_WD_S2_CTL, &s2ctl);
		regmap_read(pon->regmap,
			    pon->baseaddr + PON_WD_S2_CTL2, &s2ctl2);
		pr_info("DIAG pon_reason reason1=0x%02x poff1=0x%02x poff2=0x%02x wd_s1=0x%02x wd_s2=0x%02x s2ctl=0x%02x s2ctl2=0x%02x err=%d\n",
			reason & 0xff, poff1 & 0xff, poff2 & 0xff,
			s1 & 0xff, s2 & 0xff, s2ctl & 0xff, s2ctl2 & 0xff,
			rerr);
	}

	platform_set_drvdata(pdev, pon);

	return devm_of_platform_populate(&pdev->dev);
}

static const struct of_device_id pm8916_pon_id_table[] = {
	{ .compatible = "qcom,pm8916-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8941-pon", .data = (void *)NO_REASON_SHIFT },
	{ .compatible = "qcom,pms405-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8998-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ .compatible = "qcom,pmk8350-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ }
};
MODULE_DEVICE_TABLE(of, pm8916_pon_id_table);

static struct platform_driver pm8916_pon_driver = {
	.probe = pm8916_pon_probe,
	.driver = {
		.name = "pm8916-pon",
		.of_match_table = pm8916_pon_id_table,
	},
};
module_platform_driver(pm8916_pon_driver);

MODULE_DESCRIPTION("pm8916 Power On driver");
MODULE_LICENSE("GPL v2");
