// SPDX-License-Identifier: GPL-2.0-only
/*
 * MSM8953 APCS CPU clock controller, adapted from
 * oc-test/drivers/clk/msm/clock-cpu-8953.c.
 * Copyright (c) 2013-2018, The Linux Foundation. All rights reserved.
 *
 * All eight Cortex-A53 cores and the system CCI run from one APCS HF PLL on
 * this SoC. The PLL feeds three RCG mux-dividers (c0, c1, cci) which can also
 * be parked on GPLL0 or GPLL2, the sources boot firmware leaves behind.
 *
 * The three exported clocks also own the shared APC voltage rail, the way the
 * donor did with its vdd_class. That regulator is corner encoded, not
 * microvolts, so the rate to corner plan comes from DT and the highest corner
 * voted by any consumer is programmed before that consumer's frequency moves.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include <dt-bindings/clock/qcom,apcc-msm8953.h>

#include "clk-alpha-pll.h"
#include "clk-regmap-mux-div.h"

/* Highest registers the mux-dividers and the PLL touch. */
#define APCC_MUX_MAX_REGISTER	0xc
#define APCC_PLL_MAX_REGISTER	0x34

/* RCGCGR src values; parent_map maps a parent index onto one of these. */
#define APCC_SRC_GPLL2		2
#define APCC_SRC_GPLL0		4
#define APCC_SRC_APCS_PLL	5

/* hid_width is 5, so the divider field runs 0..31 and divides by 2 * (n + 1). */
#define APCC_MUX_DIV_MAX	(BIT(5) - 1)

#define PTE_EFUSE_SPEED_BIN_SHIFT	8
#define PTE_EFUSE_SPEED_BIN_MASK	0x7

/* Ascending rate to corner table, read from DT. */
struct apcc_plan {
	unsigned long	*rates;
	int		*corners;
	size_t		num;
};

/* One consumer of the shared APC rail, wrapping the mux it runs on. */
struct apcc_vdd {
	struct clk_hw		hw;
	struct apcc_msm8953	*apcc;
	const struct apcc_plan	*plan;
	struct clk_hw		*parent;
	struct clk		*clk;
	int			corner;
};

struct apcc_msm8953 {
	struct platform_device	*pdev;
	struct device		*dev;
	struct clk_alpha_pll	pll;
	struct clk_regmap_mux_div mux[APCC_NUM_CLOCKS];
	struct apcc_vdd		vdd[APCC_NUM_CLOCKS];
	struct apcc_plan	cpu_plan;
	struct apcc_plan	cci_plan;
	struct regulator	*apc_vreg;
	/* Serialises the corner votes of the three rail consumers. */
	struct mutex		lock;
	int			corner;
	struct notifier_block	pll_nb;
};

/* Parent 0 is the local APCS PLL, the other two are the GPLL early outputs. */
static const u32 apcc_mux_parent_map[] = {
	APCC_SRC_APCS_PLL,
	APCC_SRC_GPLL0,
	APCC_SRC_GPLL2,
};

static const char * const apcc_rcg_reg_names[APCC_NUM_CLOCKS] = {
	[APCC_CPU0_CLK] = "cpu0-rcg-clk",
	[APCC_CPU4_CLK] = "cpu4-rcg-clk",
	[APCC_CCI_CLK] = "cci-rcg-clk",
};

static const char * const apcc_rcg_names[APCC_NUM_CLOCKS] = {
	[APCC_CPU0_CLK] = "apcc_cpu0_rcg",
	[APCC_CPU4_CLK] = "apcc_cpu4_rcg",
	[APCC_CCI_CLK] = "apcc_cci_rcg",
};

static const char * const apcc_clk_names[APCC_NUM_CLOCKS] = {
	[APCC_CPU0_CLK] = "apcc_cpu0",
	[APCC_CPU4_CLK] = "apcc_cpu4",
	[APCC_CCI_CLK] = "apcc_cci",
};

/* Stock fused bin 2 tops out at 2016 MHz; the donor allowed OC to 2784 MHz. */
static const struct pll_vco apcc_pll_vco[] = {
	VCO(0, 652800000, 2016000000),
};

static const struct alpha_pll_config apcc_pll_config = {
	.l = 34,
	.config_ctl_val = 0x200d4828,
	.config_ctl_hi_val = 0x6,
	.test_ctl_val = 0x1c000000,
	.test_ctl_hi_val = 0x4000,
	.main_output_mask = BIT(0),
	.early_output_mask = BIT(3),
};

static const struct regmap_config apcc_pll_regmap_config = {
	.max_register = APCC_PLL_MAX_REGISTER,
	.fast_io = true,
};

static const struct regmap_config apcc_mux_regmap_config = {
	.max_register = APCC_MUX_MAX_REGISTER,
	.fast_io = true,
};

static inline struct apcc_vdd *to_apcc_vdd(struct clk_hw *hw)
{
	return container_of(hw, struct apcc_vdd, hw);
}

/*
 * Lowest plan entry whose rate covers the request, matching the donor's
 * find_vdd_level(). Returns -ERANGE above the fused plan.
 */
static int apcc_plan_corner(const struct apcc_plan *plan, unsigned long rate,
			    int *corner)
{
	size_t i;

	for (i = 0; i < plan->num; i++)
		if (rate <= plan->rates[i]) {
			*corner = plan->corners[i];
			return 0;
		}

	return -ERANGE;
}

/* Programs the highest voted corner. Caller holds apcc->lock. */
static int apcc_vdd_commit(struct apcc_msm8953 *apcc)
{
	int corner = 0;
	int i, ret;

	for (i = 0; i < APCC_NUM_CLOCKS; i++)
		corner = max(corner, apcc->vdd[i].corner);

	if (corner == apcc->corner)
		return 0;

	ret = regulator_set_voltage(apcc->apc_vreg, corner, corner);
	if (ret)
		return ret;

	apcc->corner = corner;

	return 0;
}

static int apcc_vdd_vote(struct apcc_vdd *vdd, unsigned long rate)
{
	struct apcc_msm8953 *apcc = vdd->apcc;
	int corner, prev, ret;

	ret = apcc_plan_corner(vdd->plan, rate, &corner);
	if (ret)
		return ret;

	mutex_lock(&apcc->lock);

	prev = vdd->corner;
	vdd->corner = corner;
	ret = apcc_vdd_commit(apcc);
	if (ret)
		vdd->corner = prev;

	mutex_unlock(&apcc->lock);

	return ret;
}

static void apcc_vdd_unvote(struct apcc_vdd *vdd)
{
	struct apcc_msm8953 *apcc = vdd->apcc;
	int prev, ret;

	mutex_lock(&apcc->lock);

	prev = vdd->corner;
	vdd->corner = 0;
	ret = apcc_vdd_commit(apcc);
	if (ret)
		vdd->corner = prev;

	mutex_unlock(&apcc->lock);
}

static int apcc_clk_determine_rate(struct clk_hw *hw,
				   struct clk_rate_request *req)
{
	struct apcc_vdd *vdd = to_apcc_vdd(hw);

	return __clk_determine_rate(vdd->parent, req);
}

static int apcc_clk_set_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long prate)
{
	struct apcc_vdd *vdd = to_apcc_vdd(hw);
	int ret;

	/* Voltage first: the rail has to be up before the core speeds up. */
	ret = apcc_vdd_vote(vdd, rate);
	if (ret)
		return ret;

	ret = clk_set_rate(vdd->clk, rate);
	if (ret)
		apcc_vdd_unvote(vdd);

	return ret;
}

static const struct clk_ops apcc_vdd_ops = {
	.determine_rate = apcc_clk_determine_rate,
	.set_rate = apcc_clk_set_rate,
};

/*
 * CCF changes the PLL before the divider that divides it down. Park any mux
 * still sitting on the PLL at its largest divider first, so the transient
 * overshoot cannot outrun the corner that was just voted. The src used here is
 * the last one CCF programmed, which is all a clock driver can know about
 * writes made outside it.
 */
static void apcc_mux_park(struct clk_regmap_mux_div *md)
{
	if (md->src != APCC_SRC_APCS_PLL)
		return;

	if (mux_div_set_src_div(md, md->src, APCC_MUX_DIV_MAX))
		return;

	md->div = APCC_MUX_DIV_MAX;
}

static int apcc_pll_rate_change(struct notifier_block *nb,
				unsigned long event, void *data)
{
	struct apcc_msm8953 *apcc = container_of(nb, struct apcc_msm8953,
						 pll_nb);
	int i;

	if (event != PRE_RATE_CHANGE)
		return NOTIFY_DONE;

	for (i = 0; i < APCC_NUM_CLOCKS; i++)
		apcc_mux_park(&apcc->mux[i]);

	return NOTIFY_OK;
}

static int apcc_read_plan(struct device *dev, const char *prop,
			  struct apcc_plan *plan)
{
	u32 *raw;
	size_t count, i;
	int ret;

	count = device_property_count_u32(dev, prop);
	if (!count || count & 1)
		return -EINVAL;

	raw = devm_kmalloc_array(dev, count, sizeof(*raw), GFP_KERNEL);
	if (!raw)
		return -ENOMEM;

	ret = device_property_read_u32_array(dev, prop, raw, count);
	if (ret)
		goto err_free;

	plan->num = count / 2;
	plan->rates = devm_kmalloc_array(dev, plan->num,
					 sizeof(*plan->rates), GFP_KERNEL);
	if (!plan->rates) {
		ret = -ENOMEM;
		goto err_free;
	}

	plan->corners = devm_kmalloc_array(dev, plan->num,
					   sizeof(*plan->corners), GFP_KERNEL);
	if (!plan->corners) {
		ret = -ENOMEM;
		goto err_free;
	}

	for (i = 0; i < plan->num; i++) {
		/* A plan that does not ascend would pick a wrong corner. */
		if (i && raw[2 * i] <= raw[2 * i - 2]) {
			ret = -EINVAL;
			goto err_free;
		}

		plan->rates[i] = raw[2 * i];
		plan->corners[i] = raw[2 * i + 1];
	}

	return 0;

err_free:
	devm_kfree(dev, raw);
	return ret;
}

/*
 * Reads "qcom,speedN-bin-v0-<kind>" for this part's fused bin, falling back to
 * bin 0's slower plan when DT has no entry for it, the way the donor did.
 */
static int apcc_read_bin_plan(struct device *dev, int speed_bin, char *kind,
			      struct apcc_plan *plan)
{
	char prop[32];
	int ret;

	snprintf(prop, sizeof(prop), "qcom,speed%d-bin-v0-%s", speed_bin, kind);
	ret = apcc_read_plan(dev, prop, plan);
	if (!ret)
		return 0;

	if (speed_bin) {
		snprintf(prop, sizeof(prop), "qcom,speed0-bin-v0-%s", kind);
		dev_warn(dev, "no plan for speed bin %d, using %s\n",
			 speed_bin, prop);
	}

	return apcc_read_plan(dev, prop, plan);
}

static int apcc_register_pll(struct apcc_msm8953 *apcc)
{
	struct clk_init_data init = { };
	struct device *dev = apcc->dev;
	void __iomem *base;
	struct regmap *regmap;

	base = devm_platform_ioremap_resource_byname(apcc->pdev, "pll");
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &apcc_pll_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	apcc->pll.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_HUAYRA_APSS];
	apcc->pll.flags = SUPPORTS_DYNAMIC_UPDATE;
	apcc->pll.vco_table = apcc_pll_vco;
	apcc->pll.num_vco = ARRAY_SIZE(apcc_pll_vco);
	apcc->pll.clkr.regmap = regmap;

	clk_alpha_pll_configure(&apcc->pll, regmap, &apcc_pll_config);

	init.name = "apcc_apcs_pll";
	init.parent_data = (const struct clk_parent_data[]){
		{ .fw_name = "xo" },
	};
	init.num_parents = 1;
	init.ops = &clk_alpha_pll_ops;

	apcc->pll.clkr.hw.init = &init;

	return devm_clk_hw_register(dev, &apcc->pll.clkr.hw);
}

static int apcc_register_mux(struct apcc_msm8953 *apcc, int id)
{
	struct clk_parent_data parents[ARRAY_SIZE(apcc_mux_parent_map)] = {
		{ .hw = &apcc->pll.clkr.hw },
		{ .fw_name = "gpll0_early" },
		{ .fw_name = "gpll2_early" },
	};
	struct device *dev = apcc->dev;
	struct clk_regmap_mux_div *md = &apcc->mux[id];
	struct clk_init_data init = { };
	void __iomem *base;
	struct regmap *regmap;
	int ret;

	base = devm_platform_ioremap_resource_byname(apcc->pdev,
						     apcc_rcg_reg_names[id]);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &apcc_mux_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	init.name = apcc_rcg_names[id];
	init.parent_data = parents;
	init.num_parents = ARRAY_SIZE(parents);
	init.ops = &clk_regmap_mux_div_ops;

	md->clkr.hw.init = &init;
	md->clkr.regmap = regmap;
	md->reg_offset = 0;
	md->hid_width = 5;
	md->hid_shift = 0;
	md->src_width = 3;
	md->src_shift = 8;
	md->parent_map = apcc_mux_parent_map;

	ret = devm_clk_hw_register(dev, &md->clkr.hw);
	if (ret)
		return ret;

	apcc->vdd[id].clk = devm_clk_hw_get_clk(dev, &md->clkr.hw,
						apcc_rcg_names[id]);
	if (IS_ERR(apcc->vdd[id].clk))
		return PTR_ERR(apcc->vdd[id].clk);

	init.name = apcc_clk_names[id];
	init.parent_hws = (const struct clk_hw *[]){ &md->clkr.hw };
	init.num_parents = 1;
	init.ops = &apcc_vdd_ops;

	apcc->vdd[id].hw.init = &init;
	apcc->vdd[id].apcc = apcc;
	apcc->vdd[id].parent = &md->clkr.hw;

	return devm_clk_hw_register(dev, &apcc->vdd[id].hw);
}

static int apcc_msm8953_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *data;
	struct apcc_msm8953 *apcc;
	struct device *dev = &pdev->dev;
	struct regulator *apc_vreg;
	struct clk *pll_clk;
	void __iomem *efuse;
	int bin, i, ret;

	apcc = devm_kzalloc(dev, sizeof(*apcc), GFP_KERNEL);
	if (!apcc)
		return -ENOMEM;

	apcc->dev = dev;
	apcc->pdev = pdev;
	apcc->corner = -1;
	mutex_init(&apcc->lock);
	platform_set_drvdata(pdev, apcc);

	apc_vreg = devm_regulator_get(dev, "vdd-cl");
	if (IS_ERR(apc_vreg))
		return dev_err_probe(dev, PTR_ERR(apc_vreg),
				     "failed to get the APC rail\n");
	apcc->apc_vreg = apc_vreg;

	efuse = devm_platform_ioremap_resource_byname(pdev, "pte-efuse");
	if (IS_ERR(efuse))
		return dev_err_probe(dev, PTR_ERR(efuse),
				     "failed to map PTE EFUSE\n");

	bin = (readl(efuse) >> PTE_EFUSE_SPEED_BIN_SHIFT) &
		PTE_EFUSE_SPEED_BIN_MASK;
	dev_info(dev, "speed bin %d\n", bin);

	ret = apcc_read_bin_plan(dev, bin, "cl", &apcc->cpu_plan);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read the CPU corner plan\n");

	ret = apcc_read_bin_plan(dev, bin, "cci", &apcc->cci_plan);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read the CCI corner plan\n");

	ret = apcc_register_pll(apcc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register the PLL\n");

	for (i = 0; i < APCC_NUM_CLOCKS; i++) {
		ret = apcc_register_mux(apcc, i);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to register %s\n",
					     apcc_rcg_reg_names[i]);
	}

	apcc->vdd[APCC_CPU0_CLK].plan = &apcc->cpu_plan;
	apcc->vdd[APCC_CPU4_CLK].plan = &apcc->cpu_plan;
	apcc->vdd[APCC_CCI_CLK].plan = &apcc->cci_plan;

	apcc->pll_nb.notifier_call = apcc_pll_rate_change;
	pll_clk = devm_clk_hw_get_clk(dev, &apcc->pll.clkr.hw, "apcc_apcs_pll");
	if (IS_ERR(pll_clk))
		return PTR_ERR(pll_clk);

	ret = clk_notifier_register(pll_clk, &apcc->pll_nb);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register the PLL notifier\n");

	data = devm_kzalloc(dev, struct_size(data, hws, APCC_NUM_CLOCKS),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->num = APCC_NUM_CLOCKS;
	for (i = 0; i < APCC_NUM_CLOCKS; i++)
		data->hws[i] = &apcc->vdd[i].hw;

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
}

static const struct of_device_id apcc_msm8953_match[] = {
	{ .compatible = "qcom,apcc-msm8953" },
	{ }
};
MODULE_DEVICE_TABLE(of, apcc_msm8953_match);

static struct platform_driver apcc_msm8953_driver = {
	.probe = apcc_msm8953_probe,
	.driver = {
		.name = "qcom-apcc-msm8953",
		.of_match_table = apcc_msm8953_match,
	},
};
module_platform_driver(apcc_msm8953_driver);

MODULE_DESCRIPTION("Qualcomm MSM8953 APCS CPU clock controller");
MODULE_LICENSE("GPL");
