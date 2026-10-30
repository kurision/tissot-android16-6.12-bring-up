// SPDX-License-Identifier: GPL-2.0-only
/*
 * MSM8953 system-CCI SAW4 voltage control.
 * Copyright (c) 2011-2017, 2020, The Linux Foundation. All rights reserved.
 *
 * Adapted from oc-test/drivers/soc/qcom/{msm-spm,spm_devices}.c.
 * This is not the idle/sequence engine, or the older SAW1.1 AVS interface.
 */

#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <soc/qcom/msm8953-spm.h>

#define SAW4_SPM_CTL		0x000
#define SAW4_SPM_DELAY		0x004
#define SAW4_CFG			0x00c
#define SAW4_PMIC_DATA3		0x04c
#define SAW4_VCTL		0x900
#define SAW4_AVS_CTL		0x904
#define SAW4_AVS_LIMIT		0x908
#define SAW4_ID			0xc04
#define SAW4_PMIC_STATUS		0xc18
#define SAW4_RESET		0xc1c
#define SAW4_VERSION		0xfd0
#define SAW4_PMIC_ARB		BIT(2)
#define SAW4_AVS_ENABLE		BIT(0)
#define SAW4_LEVEL_MASK		GENMASK(7, 0)
#define SAW4_VCTL_MASK		(GENMASK(18, 16) | SAW4_LEVEL_MASK)
#define SAW4_STATUS_MASK		(GENMASK(17, 16) | SAW4_LEVEL_MASK)
#define SAW4_LIMIT_MASK		(GENMASK(23, 16) | SAW4_LEVEL_MASK)
#define SAW4_TIMEOUT_US		500

struct msm8953_spm {
	void __iomem *base;
	cpumask_t cpus;
	/* Also taken by the IRQ-disabled synchronous voltage-request callback. */
	raw_spinlock_t lock;
};

struct msm8953_spm_request {
	struct msm8953_spm *spm;
	unsigned int level;
	int ret;
};

/* Set once the single shared domain has probed; queried by CPR. */
static bool msm8953_spm_ready;

int msm8953_spm_probe_done(void)
{
	return READ_ONCE(msm8953_spm_ready) ? 0 : -EPROBE_DEFER;
}
EXPORT_SYMBOL_GPL(msm8953_spm_probe_done);

/* Callers hold the raw lock, so AVS and VCTL updates cannot interleave. */
static int msm8953_spm_update(struct msm8953_spm *spm, u32 offset,
			      u32 mask, u32 value)
{
	u32 old = readl(spm->base + offset);

	writel((old & ~mask) | value, spm->base + offset);
	return (readl(spm->base + offset) & mask) == value ? 0 : -EIO;
}

int msm8953_spm_get_vlevel(struct msm8953_spm *spm)
{
	unsigned long flags;
	u32 level;

	raw_spin_lock_irqsave(&spm->lock, flags);
	level = readl(spm->base + SAW4_PMIC_STATUS) & SAW4_LEVEL_MASK;
	raw_spin_unlock_irqrestore(&spm->lock, flags);

	/* The donor public get_vdd() also rejects an uninitialized zero level. */
	return level ? (int)level : -EIO;
}
EXPORT_SYMBOL_GPL(msm8953_spm_get_vlevel);

int msm8953_spm_avs_enabled(struct msm8953_spm *spm)
{
	unsigned long flags;
	u32 ctl;

	raw_spin_lock_irqsave(&spm->lock, flags);
	ctl = readl(spm->base + SAW4_AVS_CTL);
	raw_spin_unlock_irqrestore(&spm->lock, flags);

	return !!(ctl & SAW4_AVS_ENABLE);
}
EXPORT_SYMBOL_GPL(msm8953_spm_avs_enabled);

int msm8953_spm_set_avs_limits(struct msm8953_spm *spm,
			       unsigned int min_level, unsigned int max_level)
{
	unsigned long flags;
	int ret;

	/* The FTS2-family floor maps to level 0, so a zero minimum is valid. */
	if (min_level > max_level || max_level > SAW4_LEVEL_MASK)
		return -EINVAL;

	raw_spin_lock_irqsave(&spm->lock, flags);
	ret = msm8953_spm_update(spm, SAW4_AVS_LIMIT, SAW4_LIMIT_MASK,
				 (max_level << 16) | min_level);
	raw_spin_unlock_irqrestore(&spm->lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(msm8953_spm_set_avs_limits);

int msm8953_spm_set_avs(struct msm8953_spm *spm, bool enable)
{
	unsigned long flags;
	u32 limits, min_level, max_level;
	int ret = -EINVAL;

	raw_spin_lock_irqsave(&spm->lock, flags);
	if (enable) {
		limits = readl(spm->base + SAW4_AVS_LIMIT);
		min_level = limits & SAW4_LEVEL_MASK;
		max_level = (limits >> 16) & SAW4_LEVEL_MASK;
		/* Refuse to arm AVS while the ceiling is still unprogrammed. */
		if (!max_level || min_level > max_level)
			goto unlock;
	}
	ret = msm8953_spm_update(spm, SAW4_AVS_CTL, SAW4_AVS_ENABLE,
				 enable ? SAW4_AVS_ENABLE : 0);
unlock:
	raw_spin_unlock_irqrestore(&spm->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(msm8953_spm_set_avs);

static void msm8953_spm_do_set_vlevel(void *data)
{
	struct msm8953_spm_request *request = data;
	struct msm8953_spm *spm = request->spm;
	unsigned long flags;
	u32 value, status, expected = request->level;
	bool avs_enabled;
	int ret;

	raw_spin_lock_irqsave(&spm->lock, flags);
	if (!(readl(spm->base + SAW4_ID) & SAW4_PMIC_ARB)) {
		request->ret = -ENODEV;
		goto unlock;
	}

	/* The raw lock excludes software only; pause the autonomous AVS engine. */
	avs_enabled = readl(spm->base + SAW4_AVS_CTL) & SAW4_AVS_ENABLE;
	if (avs_enabled) {
		request->ret = msm8953_spm_update(spm, SAW4_AVS_CTL, SAW4_AVS_ENABLE, 0);
		if (request->ret)
			goto restore_avs;
	}

	/* Port 0 only, matching MSM8953's system-CCI voltage controller. */
	writel(1, spm->base + SAW4_RESET);
	value = readl(spm->base + SAW4_VCTL);
	writel((value & ~SAW4_VCTL_MASK) | request->level, spm->base + SAW4_VCTL);
	value = readl(spm->base + SAW4_PMIC_DATA3);
	writel((value & ~SAW4_VCTL_MASK) | request->level, spm->base + SAW4_PMIC_DATA3);

	/* Match both the returned level and PMIC FSM idle, after the donor delay. */
	request->ret = read_poll_timeout_atomic(readl, status,
						(status & SAW4_STATUS_MASK) == expected,
						1, SAW4_TIMEOUT_US, true,
						spm->base + SAW4_PMIC_STATUS);
	/* SAW4 limits live in AVS_LIMIT; do not apply the SAW1.1 bit-27 scheme. */
restore_avs:
	if (avs_enabled) {
		ret = msm8953_spm_update(spm, SAW4_AVS_CTL,
					 SAW4_AVS_ENABLE, SAW4_AVS_ENABLE);
		if (!request->ret)
			request->ret = ret;
	}
unlock:
	raw_spin_unlock_irqrestore(&spm->lock, flags);
}

int msm8953_spm_set_vlevel(struct msm8953_spm *spm, unsigned int level)
{
	struct msm8953_spm_request request = { .spm = spm, .level = level };
	int ret;

	if (!level || level > SAW4_LEVEL_MASK)
		return -EINVAL;
	might_sleep();
	cpus_read_lock();
	ret = smp_call_function_any(&spm->cpus, msm8953_spm_do_set_vlevel,
				    &request, true);
	cpus_read_unlock();

	return ret ? ret : request.ret;
}
EXPORT_SYMBOL_GPL(msm8953_spm_set_vlevel);

static void msm8953_spm_put_device(void *data)
{
	put_device(data);
}

struct msm8953_spm *devm_msm8953_spm_get(struct device *dev)
{
	struct platform_device *supplier;
	struct msm8953_spm *spm;
	struct device_node *np;
	int ret;

	np = of_parse_phandle(dev->of_node, "qcom,saw", 0);
	if (!np)
		return ERR_PTR(-ENODEV);
	if (!of_device_is_available(np) ||
	    !of_device_is_compatible(np, "qcom,msm8953-saw2-v4.1-cci")) {
		of_node_put(np);
		return ERR_PTR(-ENODEV);
	}
	supplier = of_find_device_by_node(np);
	of_node_put(np);
	if (!supplier)
		return ERR_PTR(-EPROBE_DEFER);
	if (!device_link_add(dev, &supplier->dev, DL_FLAG_AUTOREMOVE_CONSUMER)) {
		put_device(&supplier->dev);
		return ERR_PTR(dev_err_probe(dev, -EINVAL,
					     "device link to the SAW failed\n"));
	}
	spm = platform_get_drvdata(supplier);
	if (!device_is_bound(&supplier->dev) || !spm) {
		put_device(&supplier->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}
	ret = devm_add_action_or_reset(dev, msm8953_spm_put_device, &supplier->dev);
	return ret ? ERR_PTR(ret) : spm;
}
EXPORT_SYMBOL_GPL(devm_msm8953_spm_get);

static int msm8953_spm_parse_cpus(struct device_node *np, cpumask_t *mask)
{
	struct device_node *cpu_node;
	int cpu, i, count;

	count = of_count_phandle_with_args(np, "qcom,cpu-vctl-list", NULL);
	if (count != num_possible_cpus())
		return -EINVAL;
	cpumask_clear(mask);
	for (i = 0; i < count; i++) {
		cpu_node = of_parse_phandle(np, "qcom,cpu-vctl-list", i);
		if (!cpu_node)
			return -EINVAL;
		cpu = of_cpu_node_to_id(cpu_node);
		of_node_put(cpu_node);
		if (cpu < 0 || !cpu_possible(cpu) || cpumask_test_cpu(cpu, mask))
			return -EINVAL;
		cpumask_set_cpu(cpu, mask);
	}

	return cpumask_equal(mask, cpu_possible_mask) ? 0 : -EINVAL;
}

static int msm8953_spm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct msm8953_spm *spm;
	struct resource *res;
	u32 version;
	int ret;

	spm = devm_kzalloc(dev, sizeof(*spm), GFP_KERNEL);
	if (!spm)
		return -ENOMEM;
	raw_spin_lock_init(&spm->lock);
	ret = msm8953_spm_parse_cpus(dev->of_node, &spm->cpus);
	if (ret)
		return dev_err_probe(dev, ret, "invalid shared CPU voltage domain\n");
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) < SAW4_VERSION + sizeof(u32))
		return -EINVAL;
	spm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(spm->base))
		return PTR_ERR(spm->base);

	version = readl(spm->base + SAW4_VERSION);
	if ((version >> 28) != 4 || ((version >> 16) & 0xfff) != 1)
		return dev_err_probe(dev, -ENODEV, "unsupported SAW version %#x\n", version);
	if (!(readl(spm->base + SAW4_ID) & SAW4_PMIC_ARB))
		return dev_err_probe(dev, -ENODEV, "SAW has no PMIC arbiter\n");

	/* Exact system-CCI settings from the donor's msm8953-pm.dtsi:16-34. */
	writel(0x14, spm->base + SAW4_CFG);
	writel(0x10, spm->base + SAW4_AVS_CTL);
	writel(0x3c102800, spm->base + SAW4_SPM_DELAY);
	writel(0x0e, spm->base + SAW4_SPM_CTL);
	if (readl(spm->base + SAW4_CFG) != 0x14 ||
	    readl(spm->base + SAW4_AVS_CTL) != 0x10 ||
	    readl(spm->base + SAW4_SPM_DELAY) != 0x3c102800 ||
	    readl(spm->base + SAW4_SPM_CTL) != 0x0e)
		return dev_err_probe(dev, -EIO, "SAW configuration readback failed\n");
	/* No voltage request, AVS enable, sequence RAM or idle registration here. */
	platform_set_drvdata(pdev, spm);
	WRITE_ONCE(msm8953_spm_ready, true);
	return 0;
}

static const struct of_device_id msm8953_spm_match[] = {
	{ .compatible = "qcom,msm8953-saw2-v4.1-cci" },
	{ }
};

static struct platform_driver msm8953_spm_driver = {
	.probe = msm8953_spm_probe,
	.driver = {
		.name = "qcom-msm8953-spm",
		.of_match_table = msm8953_spm_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(msm8953_spm_driver);

MODULE_DESCRIPTION("Qualcomm MSM8953 shared SAW4 voltage controller");
MODULE_LICENSE("GPL");
