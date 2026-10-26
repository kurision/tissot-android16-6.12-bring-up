// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013-2014, 2018, 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2026, The tissot kgsl-port authors.
 *
 * Port of the donor's devfreq_devbw.c
 * (../oc-test/drivers/devfreq/devfreq_devbw.c) on top of the
 * interconnect framework.
 *
 * The device is a devfreq device whose "frequency" is the bandwidth of
 * the path between the application processor and the memory. The
 * operating points are the donor's table, and the vote goes to the
 * interconnect framework, which adds the requests of the clients of the
 * path and hands the result to the RPM.
 *
 * The power HAL of the ROM writes the bandwidth it wants to the min_freq
 * attribute of the device, so the name of the device is the one the ROM
 * expects, /sys/class/devfreq/soc:qcom,cpu-cpu-ddr-bw. The device tree
 * gives that name to the node, with the container it sits in named
 * "soc" (arch/arm64/boot/dts/qcom/msm8953.dtsi).
 */

#include <linux/devfreq.h>
#include <linux/interconnect.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>

/* The donor's unit of the operating points: MB/s. */
#define DEVBW_MBYTE	(1ULL << 20)

struct msm_devbw {
	struct icc_path *path;
	struct devfreq *df;
	struct devfreq_dev_profile profile;
	u64 kbps;	/* the bandwidth voted, in kB/s */
};

/*
 * The bandwidth of the path. The unit of the interconnect framework is
 * kB/s (include/linux/interconnect.h Bps_to_icc()), and the donor
 * multiplied its operating point by its MBYTE (1 << 20) to get the
 * bytes/s it voted.
 */
static int devbw_set_bw(struct msm_devbw *d, unsigned long mbps)
{
	u64 bps = (u64)mbps * DEVBW_MBYTE;
	u64 kbps = bps / 1000;
	int ret;

	if (!d->path)
		return 0;

	/*
	 * Peak only, like the donor: its devfreq_devbw.c voted the
	 * bandwidth as ib and the governor's ab, which is 0 for the
	 * governors this node uses. The average is a sum over every
	 * client of the path, the peak a maximum.
	 */
	ret = icc_set_bw(d->path, 0, kbps);
	if (ret)
		return ret;

	d->kbps = kbps;

	return 0;
}

static int devbw_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct msm_devbw *d = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);

	return devbw_set_bw(d, *freq);
}

static int devbw_get_dev_status(struct device *dev,
				struct devfreq_dev_status *stat)
{
	struct msm_devbw *d = dev_get_drvdata(dev);

	stat->private_data = &d->kbps;

	return 0;
}

static int msm_devbw_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct msm_devbw *d;
	struct devfreq_dev_profile *p;
	const char *gov_name;
	int ret;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	dev_set_drvdata(dev, d);

	/*
	 * The path of the application processor to the memory. The
	 * interconnect provider is a platform device itself, so the
	 * lookup can fail with -EPROBE_DEFER until it has probed.
	 */
	d->path = devm_of_icc_get(dev, "cpu-ddr");
	if (IS_ERR(d->path))
		return dev_err_probe(dev, PTR_ERR(d->path),
				     "no cpu-ddr interconnect path\n");
	if (!d->path)
		return dev_err_probe(dev, -ENODEV,
				     "no interconnect path for the cpu\n");

	ret = devm_pm_opp_of_add_table(dev);
	if (ret)
		return dev_err_probe(dev, ret, "no operating points\n");

	p = &d->profile;
	p->polling_ms = 50;
	p->target = devbw_target;
	p->get_dev_status = devbw_get_dev_status;

	if (of_property_read_string(dev->of_node, "governor", &gov_name))
		gov_name = "performance";

	d->df = devm_devfreq_add_device(dev, p, gov_name, NULL);
	if (IS_ERR(d->df))
		return PTR_ERR(d->df);

	return 0;
}

static const struct of_device_id msm_devbw_match[] = {
	{ .compatible = "qcom,devbw" },
	{ }
};

static struct platform_driver msm_devbw_driver = {
	.probe = msm_devbw_probe,
	.driver = {
		.name = "msm-devbw",
		.of_match_table = msm_devbw_match,
	},
};
module_platform_driver(msm_devbw_driver);

MODULE_DESCRIPTION("Qualcomm MSM DDR bandwidth voting driver");
MODULE_LICENSE("GPL v2");
