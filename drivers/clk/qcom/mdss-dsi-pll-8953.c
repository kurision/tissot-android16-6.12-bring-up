// SPDX-License-Identifier: GPL-2.0-only
/*
 * CCF clock provider for the MSM8953 (8996-family, rev 2) DSI PLL.
 *
 * Replaces the 4.19 donor's private clock framework
 * (mdss-dsi-pll-8996.c) with upstream struct clk_hw. Register sequences
 * and rate math are adapted from mdss-dsi-pll-8996-util.c; the private
 * struct clk/div_clk/mux_clk framework is gone.
 *
 * Topology per PLL (shadows cut: tissot panel has no dynamic refresh):
 *   vco (19.2 MHz ref, 1.3-2.6 GHz)
 *    `- postdiv N1 (1-15, DSIPHY_CMN_CLK_CFG0[3:0])
 *        +- n2div N2 (1-15, DSIPHY_CMN_CLK_CFG0[7:4], dsiclk_sel=1 doubles)
 *        |   `- pixel_src fixed /2
 *        `- byte_src fixed /8
 * DT clock IDs (include/dt-bindings/clock/qcom,mdss-dsi-pll-8953.h):
 * byte-mux and pixel-mux IDs resolve straight to the fixed src dividers;
 * the donor's shadow mux input does not exist on a static panel.
 */

#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <dt-bindings/clock/qcom,mdss-dsi-pll-8953.h>

/* Opaque donor-framework types used only in mdss-dsi-pll-8996.h
 * prototypes, which this provider never calls. */
struct clk;
struct div_clk;
struct mux_clk;
struct mdss_pll_resources;

#include "mdss-dsi-pll-8996.h"

#define DSI_PLL_POLL_MAX_READS		15
#define DSI_PLL_POLL_TIMEOUT_US		1000
#define DSI_PHY_SPARE_VAL		0x6a
#define DSI_PLL_DEFAULT_POSTDIV		1
#define VCO_MIN_RATE			1300000000UL
#define VCO_MAX_RATE			2600000000UL
#define VCO_REF_RATE			19200000UL

#define CEIL(x, y)			(((x) + ((y) - 1)) / (y))

/* MDSS_PLL_REG_W needs (base, offset, data); keep donor semantics. */
#undef MDSS_PLL_REG_W
#undef MDSS_PLL_REG_R
#define MDSS_PLL_REG_W(base, offset, data)	writel_relaxed((data), \
							(base) + (offset))
#define MDSS_PLL_REG_R(base, offset)		readl_relaxed((base) + (offset))

struct mdss_8953_pll {
	struct device		*dev;
	void __iomem		*pll_base;
	int			index;
	struct clk		*iface_clk;
	/* rate/programming state, adapted from dsi_pll_db in/out */
	struct dsi_pll_input	in;
	struct dsi_pll_output	out;
	u64			vco_current_rate;
	u32			vco_ref_clk_rate;
	bool			ssc_en;
	u32			ssc_freq;
	u32			ssc_ppm;
	int			n1div;
	int			n2div;
	/* Owns a runtime-PM + iface reference until unprepare. */
	bool			prepared;
	struct mutex		lock;
	struct clk_hw		vco_hw;
	struct clk_hw		postdiv_hw;
	struct clk_hw		n2div_hw;
	struct clk_fixed_factor	byte;
	struct clk_fixed_factor	pixel;
};

#define to_mdss_8953_pll(hw, member) \
	container_of(hw, struct mdss_8953_pll, member)

/*
 * Power around register access: the PLL block lives behind MDSS_GDSC
 * (power-domains in DT) with the GCC AHB iface clock. CCF serializes
 * prepare/rate paths. Register access needs a balanced runtime-PM + iface
 * pair; prepare retains that pair for the entire programmed PLL lifetime.
 */
static int mdss_8953_pll_power_get(struct mdss_8953_pll *pll)
{
	int rc;

	rc = pm_runtime_resume_and_get(pll->dev);
	if (rc < 0)
		return rc;
	rc = clk_prepare_enable(pll->iface_clk);
	if (rc) {
		pm_runtime_put(pll->dev);
		return rc;
	}
	return 0;
}

static void mdss_8953_pll_power_put(struct mdss_8953_pll *pll)
{
	clk_disable_unprepare(pll->iface_clk);
	pm_runtime_put(pll->dev);
}

static bool mdss_8953_pll_locked(struct mdss_8953_pll *pll)
{
	u32 status;

	if (readl_poll_timeout_atomic(pll->pll_base +
			DSIPHY_PLL_RESET_SM_READY_STATUS,
			status, (status & BIT(5)) > 0,
			DSI_PLL_POLL_MAX_READS,
			DSI_PLL_POLL_TIMEOUT_US)) {
		pr_err("PLL ndx=%d status=%x failed to lock\n",
			pll->index, status);
		return false;
	}
	if (readl_poll_timeout_atomic(pll->pll_base +
			DSIPHY_PLL_RESET_SM_READY_STATUS,
			status, (status & BIT(0)) > 0,
			DSI_PLL_POLL_MAX_READS,
			DSI_PLL_POLL_TIMEOUT_US)) {
		pr_err("PLL ndx=%d status=%x not ready\n",
			pll->index, status);
		return false;
	}
	return true;
}

/*
 * Diagnostic-only dump on lock failure: read-only snapshot of the PLL
 * block, power state and requested rate so a failing boot identifies
 * whether the config never latched (power), the sequencer is stuck, or
 * the math programmed an impossible target. Called only from the
 * lock failure path; success path behavior is unchanged.
 */
static void mdss_8953_dump_fail_state(struct mdss_8953_pll *pll)
{
	void __iomem *base = pll->pll_base;
	struct device *dev = pll->dev;

	dev_err(dev, "PLL%d fail state: vco=%llu ref=%u ssc=%u pm_active=%u iface_rate=%lu\n",
		pll->index, pll->vco_current_rate, pll->vco_ref_clk_rate,
		pll->ssc_en, pm_runtime_active(dev),
		clk_get_rate(pll->iface_clk));
	dev_err(dev, "PLL%d regs: CNTRL=%x CTRL0=%x CTRL1=%x LDO=%x CLKCFG0=%x CLKCFG1=%x VREF=%x\n",
		pll->index, MDSS_PLL_REG_R(base, DSIPHY_CMN_PLL_CNTRL),
		MDSS_PLL_REG_R(base, DSIPHY_CMN_CTRL_0),
		MDSS_PLL_REG_R(base, DSIPHY_CMN_CTRL_1),
		MDSS_PLL_REG_R(base, DSIPHY_CMN_LDO_CNTRL),
		MDSS_PLL_REG_R(base, DSIPHY_CMN_CLK_CFG0),
		MDSS_PLL_REG_R(base, DSIPHY_CMN_CLK_CFG1),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_VREF_CFG1));
	dev_err(dev, "PLL%d regs: RESETSM=%x VCO_TUNE=%x KVCO_CODE=%x DEC=%x FRAC=%x%x%x LOCKCMP=%x%x%x CMPEN=%x\n",
		pll->index, MDSS_PLL_REG_R(base, DSIPHY_PLL_RESETSM_CNTRL),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_PLL_VCO_TUNE),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_KVCO_CODE),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_DEC_START),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_DIV_FRAC_START1),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_DIV_FRAC_START2),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_DIV_FRAC_START3),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_PLLLOCK_CMP1),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_PLLLOCK_CMP2),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_PLLLOCK_CMP3),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_PLLLOCK_CMP_EN));
	dev_err(dev, "PLL%d regs: VCOCNT=%x%x KVCOCNT=%x%x LPF2_POSTDIV=%x CLKBUFLR=%x GLBLTEST=%x SYSCLK=%x TXCLK=%x STATUS=%x\n",
		pll->index, MDSS_PLL_REG_R(base, DSIPHY_PLL_VCO_COUNT1),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_VCO_COUNT2),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_KVCO_COUNT1),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_KVCO_COUNT2),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_PLL_LPF2_POSTDIV),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_CLKBUFLR_EN),
		MDSS_PLL_REG_R(base, DSIPHY_CMN_GLBL_TEST_CTRL),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_SYSCLK_EN_RESET),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_TXCLK_EN),
		MDSS_PLL_REG_R(base, DSIPHY_PLL_RESET_SM_READY_STATUS));
}

static void mdss_8953_pll_start(struct mdss_8953_pll *pll)
{
	MDSS_PLL_REG_W(pll->pll_base, DSIPHY_PLL_VREF_CFG1, 0x10);
	MDSS_PLL_REG_W(pll->pll_base, DSIPHY_CMN_PLL_CNTRL, 1);
	wmb();
}

static void mdss_8953_pll_stop(struct mdss_8953_pll *pll)
{
	MDSS_PLL_REG_W(pll->pll_base, DSIPHY_CMN_PLL_CNTRL, 0);
	wmb();
}

/* Pure rate math, adapted from pll_8996_*_calc. */

static void mdss_8953_ssc_calc(struct mdss_8953_pll *pll)
{
	struct dsi_pll_input *pin = &pll->in;
	struct dsi_pll_output *pout = &pll->out;
	u32 period, ssc_period;
	u32 ref, rem;
	s64 step_size;

	ssc_period = pin->ssc_freq / 500;
	period = (unsigned long)pll->vco_ref_clk_rate / 1000;
	ssc_period = CEIL(period, ssc_period);
	ssc_period -= 1;
	pout->ssc_period = ssc_period;

	step_size = (u32)pll->vco_current_rate;
	ref = pll->vco_ref_clk_rate;
	ref /= 1000;
	step_size = div_s64(step_size, ref);
	step_size <<= 20;
	step_size = div_s64(step_size, 1000);
	step_size *= pin->ssc_spread;
	step_size = div_s64(step_size, 1000);
	step_size *= (pin->ssc_adj_period + 1);

	rem = 0;
	step_size = div_s64_rem(step_size, ssc_period + 1, &rem);
	if (rem)
		step_size++;

	step_size &= 0x0ffff;
	pout->ssc_step_size = step_size;
}

static void mdss_8953_dec_frac_calc(struct mdss_8953_pll *pll)
{
	struct dsi_pll_input *pin = &pll->in;
	struct dsi_pll_output *pout = &pll->out;
	s64 multiplier = BIT(20);
	s64 dec_start_multiple, dec_start;
	u64 pll_comp_val;
	s32 duration, div_frac_start;
	s64 vco_clk_rate = pll->vco_current_rate;
	s64 fref = pll->vco_ref_clk_rate;

	dec_start_multiple = div_s64(vco_clk_rate * multiplier, fref);
	div_s64_rem(dec_start_multiple, multiplier, &div_frac_start);

	dec_start = div_s64(dec_start_multiple, multiplier);

	pout->dec_start = (u32)dec_start;
	pout->div_frac_start = div_frac_start;

	if (pin->plllock_cnt == 0)
		duration = 1024;
	else if (pin->plllock_cnt == 1)
		duration = 256;
	else if (pin->plllock_cnt == 2)
		duration = 128;
	else
		duration = 32;

	pll_comp_val = duration * dec_start_multiple;
	pll_comp_val = div_s64(pll_comp_val, multiplier);
	do_div(pll_comp_val, 10);

	pout->plllock_cmp = (u32)pll_comp_val;

	pout->pll_txclk_en = 1;
	/* 8953 is revision 2 */
	pout->cmn_ldo_cntrl = 0x3c;
}

static u32 mdss_8953_kvco_slop(u32 vrate)
{
	u32 slop = 0;

	if (vrate >= 1600000000)
		slop = 150;
	else
		slop = 110;
	return slop;
}

static inline u32 mdss_8953_calc_kvco_code(s64 vco_clk_rate)
{
	if (vco_clk_rate < 1700000000)
		return 0x08;
	else
		return 0x10;
}

static void mdss_8953_calc_vco_count(struct mdss_8953_pll *pll)
{
	struct dsi_pll_input *pin = &pll->in;
	struct dsi_pll_output *pout = &pll->out;
	s64 vco_clk_rate = pll->vco_current_rate;
	s64 fref = pll->vco_ref_clk_rate;
	u64 data;
	u64 cnt;

	data = fref * pin->vco_measure_time;
	do_div(data, 1000000);
	data &= 0x03ff;
	data -= 2;
	pout->pll_vco_div_ref = data;

	data = (unsigned long)vco_clk_rate / 1000000;
	data *= pin->vco_measure_time;
	do_div(data, 10);
	pout->pll_vco_count = data;

	data = fref * pin->kvco_measure_time;
	do_div(data, 1000000);
	data &= 0x03ff;
	data -= 1;
	pout->pll_kvco_div_ref = data;

	cnt = mdss_8953_kvco_slop(vco_clk_rate);
	cnt *= 2;
	do_div(cnt, 100);
	cnt *= pin->kvco_measure_time;
	pout->pll_kvco_count = cnt;

	pout->pll_misc1 = 16;
	pout->pll_resetsm_cntrl = 48;
	pout->pll_resetsm_cntrl2 = pin->bandgap_timer << 3;
	pout->pll_resetsm_cntrl5 = pin->pll_wakeup_timer;
	pout->pll_kvco_code = mdss_8953_calc_kvco_code(vco_clk_rate);
}

static void mdss_8953_commit_common(struct mdss_8953_pll *pll)
{
	void __iomem *pll_base = pll->pll_base;
	struct dsi_pll_input *pin = &pll->in;
	struct dsi_pll_output *pout = &pll->out;
	char data;

	data = 0;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_SYSCLK_EN_RESET, data);

	data = pout->pll_txclk_en;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_TXCLK_EN, data);

	data = pout->pll_resetsm_cntrl;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_RESETSM_CNTRL, data);
	data = pout->pll_resetsm_cntrl2;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_RESETSM_CNTRL2, data);
	data = pout->pll_resetsm_cntrl5;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_RESETSM_CNTRL5, data);

	data = pout->pll_vco_div_ref;
	data &= 0x0ff;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_VCO_DIV_REF1, data);
	data = (pout->pll_vco_div_ref >> 8);
	data &= 0x03;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_VCO_DIV_REF2, data);

	data = pout->pll_kvco_div_ref;
	data &= 0x0ff;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_KVCO_DIV_REF1, data);
	data = (pout->pll_kvco_div_ref >> 8);
	data &= 0x03;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_KVCO_DIV_REF2, data);

	data = pout->pll_misc1;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_PLL_MISC1, data);

	data = pin->pll_ie_trim;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_IE_TRIM, data);

	data = pin->pll_ip_trim;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_IP_TRIM, data);

	data = ((pin->pll_cpmset_cur << 3) | pin->pll_cpcset_cur);
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_CP_SET_CUR, data);

	data = ((pin->pll_icpcset_p << 3) | pin->pll_icpcset_m);
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_PLL_ICPCSET, data);

	data = ((pin->pll_icpmset_p << 3) | pin->pll_icpcset_m);
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_PLL_ICPMSET, data);

	data = ((pin->pll_icpmset << 3) | pin->pll_icpcset);
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_PLL_ICP_SET, data);

	data = ((pin->pll_lpf_cap2 << 4) | pin->pll_lpf_cap1);
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_PLL_LPF1, data);

	data = pin->pll_iptat_trim;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_IPTAT_TRIM, data);

	data = (pin->pll_c3ctrl | (pin->pll_r3ctrl << 4));
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_PLL_CRCTRL, data);
}

static void mdss_8953_commit_ssc(struct mdss_8953_pll *pll)
{
	void __iomem *pll_base = pll->pll_base;
	struct dsi_pll_input *pin = &pll->in;
	struct dsi_pll_output *pout = &pll->out;
	char data;

	data = pin->ssc_adj_period;
	data &= 0x0ff;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_SSC_ADJ_PER1, data);
	data = (pin->ssc_adj_period >> 8);
	data &= 0x03;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_SSC_ADJ_PER2, data);

	data = pout->ssc_period;
	data &= 0x0ff;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_SSC_PER1, data);
	data = (pout->ssc_period >> 8);
	data &= 0x0ff;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_SSC_PER2, data);

	data = pout->ssc_step_size;
	data &= 0x0ff;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_SSC_STEP_SIZE1, data);
	data = (pout->ssc_step_size >> 8);
	data &= 0x0ff;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_SSC_STEP_SIZE2, data);

	data = (pin->ssc_center & 0x01);
	data <<= 1;
	data |= 0x01;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_SSC_EN_CENTER, data);

	wmb();
}

static void mdss_8953_commit(struct mdss_8953_pll *pll)
{
	void __iomem *pll_base = pll->pll_base;
	struct dsi_pll_input *pin = &pll->in;
	struct dsi_pll_output *pout = &pll->out;
	char data;

	data = pout->cmn_ldo_cntrl;
	MDSS_PLL_REG_W(pll_base, DSIPHY_CMN_LDO_CNTRL, data);

	mdss_8953_commit_common(pll);

	MDSS_PLL_REG_W(pll_base, DSIPHY_CMN_PLL_CNTRL, 0);

	/* Keep the PLL software reset asserted for the donor's 10 us. */
	MDSS_PLL_REG_W(pll_base, DSIPHY_CMN_CTRL_1, 0x20);
	wmb();
	udelay(10);

	MDSS_PLL_REG_W(pll_base, DSIPHY_CMN_CTRL_1, 0);
	wmb();

	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_PLL_VCO_TUNE, 0);
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_KVCO_CODE, 0);
	wmb();

	data = pin->dsiclk_sel;
	MDSS_PLL_REG_W(pll_base, DSIPHY_CMN_CLK_CFG1, data);

	data = 0xff;
	MDSS_PLL_REG_W(pll_base, DSIPHY_CMN_CTRL_0, data);

	data = pout->dec_start;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_DEC_START, data);

	data = pout->div_frac_start;
	data &= 0x0ff;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_DIV_FRAC_START1, data);
	data = (pout->div_frac_start >> 8);
	data &= 0x0ff;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_DIV_FRAC_START2, data);
	data = (pout->div_frac_start >> 16);
	data &= 0x0f;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_DIV_FRAC_START3, data);

	data = pout->plllock_cmp;
	data &= 0x0ff;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_PLLLOCK_CMP1, data);
	data = (pout->plllock_cmp >> 8);
	data &= 0x0ff;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_PLLLOCK_CMP2, data);
	data = (pout->plllock_cmp >> 16);
	data &= 0x03;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_PLLLOCK_CMP3, data);

	data = ((pin->plllock_cnt << 1) | (pin->plllock_rng << 3));
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_PLLLOCK_CMP_EN, data);

	data = pout->pll_vco_count;
	data &= 0x0ff;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_VCO_COUNT1, data);
	data = (pout->pll_vco_count >> 8);
	data &= 0x0ff;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_VCO_COUNT2, data);

	data = pout->pll_kvco_count;
	data &= 0x0ff;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_KVCO_COUNT1, data);
	data = (pout->pll_kvco_count >> 8);
	data &= 0x03;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_KVCO_COUNT2, data);

	data = (((pout->pll_postdiv - 1) << 4) | pin->pll_lpf_res1);
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_PLL_LPF2_POSTDIV, data);

	data = pout->pll_kvco_code;
	MDSS_PLL_REG_W(pll_base, DSIPHY_PLL_KVCO_CODE, data);

	data = (pout->pll_n1div | (pout->pll_n2div << 4));
	MDSS_PLL_REG_W(pll_base, DSIPHY_CMN_CLK_CFG0, data);

	if (pll->ssc_en)
		mdss_8953_commit_ssc(pll);

	wmb();
}

static int mdss_8953_enable_seq(struct mdss_8953_pll *pll)
{
	mdss_8953_pll_start(pll);

	if (!mdss_8953_pll_locked(pll)) {
		pr_debug("PLL ndx=%d lock failed, retry full sequence\n",
			pll->index);
		mdss_8953_commit(pll);
		mdss_8953_pll_start(pll);
		if (!mdss_8953_pll_locked(pll)) {
			pr_err("PLL ndx=%d lock failed\n", pll->index);
			mdss_8953_dump_fail_state(pll);
			return -EINVAL;
		}
	}

	MDSS_PLL_REG_W(pll->pll_base, DSIPHY_CMN_GLBL_DIGTOP_SPARE2,
		DSI_PHY_SPARE_VAL);
	pr_debug("PLL ndx:%d locked rate=%llu\n",
		pll->index, pll->vco_current_rate);
	return 0;
}


static void mdss_8953_input_init(struct mdss_8953_pll *pll)
{
	struct dsi_pll_input *pin = &pll->in;

	pin->fref = 19200000;
	pin->fdata = 0;
	pin->dsiclk_sel = 1;
	pin->ssc_en = pll->ssc_en;
	pin->ldo_en = 0;

	pin->refclk_dbler_en = 0;
	pin->vco_measure_time = 5;
	pin->kvco_measure_time = 5;
	pin->bandgap_timer = 4;
	pin->pll_wakeup_timer = 5;
	pin->plllock_cnt = 1;
	pin->plllock_rng = 0;
	pin->ssc_center = 0;
	pin->ssc_adj_period = 37;
	pin->ssc_spread = pll->ssc_ppm / 1000;
	pin->ssc_freq = pll->ssc_freq;

	pin->pll_ie_trim = 4;
	pin->pll_ip_trim = 4;
	pin->pll_cpcset_cur = 1;
	pin->pll_cpmset_cur = 1;
	pin->pll_icpmset = 7;
	pin->pll_icpcset = 7;
	pin->pll_icpmset_p = 0;
	pin->pll_icpmset_m = 0;
	pin->pll_icpcset_p = 0;
	pin->pll_icpcset_m = 0;
	pin->pll_lpf_res1 = 3;
	pin->pll_lpf_cap1 = 11;
	pin->pll_lpf_cap2 = 1;
	pin->pll_iptat_trim = 7;
	pin->pll_c3ctrl = 2;
	pin->pll_r3ctrl = 1;
}

static void mdss_8953_vco_calc_rate(struct mdss_8953_pll *pll,
				    unsigned long rate)
{
	pll->vco_current_rate = rate;
	pll->vco_ref_clk_rate = VCO_REF_RATE;

	mdss_8953_input_init(pll);
	pll->out.pll_postdiv = DSI_PLL_DEFAULT_POSTDIV;
	pll->out.pll_n1div = pll->n1div;
	pll->out.pll_n2div = pll->n2div;

	mdss_8953_dec_frac_calc(pll);
	if (pll->ssc_en)
		mdss_8953_ssc_calc(pll);
	mdss_8953_calc_vco_count(pll);
}

/* The caller must keep runtime-PM and the iface clock active. */
static unsigned long mdss_8953_pll_read_rate(struct mdss_8953_pll *pll)
{
	void __iomem *base = pll->pll_base;
	u32 dec, frac;
	u64 rate;

	dec = MDSS_PLL_REG_R(base, DSIPHY_PLL_DEC_START) & 0xff;
	frac = (MDSS_PLL_REG_R(base, DSIPHY_PLL_DIV_FRAC_START3) & 0xf)
		<< 16;
	frac |= (MDSS_PLL_REG_R(base, DSIPHY_PLL_DIV_FRAC_START2) & 0xff)
		<< 8;
	frac |= MDSS_PLL_REG_R(base, DSIPHY_PLL_DIV_FRAC_START1) & 0xff;
	rate = (u64)VCO_REF_RATE * dec;
	rate += div_u64((u64)VCO_REF_RATE * frac, BIT(20));

	/* DEC/FRAC truncation must not make a valid minimum rate too low. */
	return DIV_ROUND_UP_ULL(rate, 1000) * 1000;
}

static void mdss_8953_pll_read_state(struct mdss_8953_pll *pll)
{
	unsigned long rate = mdss_8953_pll_read_rate(pll);
	u32 cfg0 = MDSS_PLL_REG_R(pll->pll_base, DSIPHY_CMN_CLK_CFG0);
	u32 cfg1 = MDSS_PLL_REG_R(pll->pll_base, DSIPHY_CMN_CLK_CFG1);
	int n1 = cfg0 & 0xf;
	int n2 = (cfg0 >> 4) & 0xf;

	/* The exported pixel path is the /2 input of the hardware mux. */
	if (rate < VCO_MIN_RATE || rate > VCO_MAX_RATE ||
	    !n1 || !n2 || (cfg1 & 0x3) != 1)
		return;

	pll->n1div = n1;
	pll->n2div = n2;
	mdss_8953_vco_calc_rate(pll, rate);
}

static bool mdss_8953_pll_matches_state(struct mdss_8953_pll *pll)
{
	void __iomem *base = pll->pll_base;
	u32 status = MDSS_PLL_REG_R(base, DSIPHY_PLL_RESET_SM_READY_STATUS);
	u32 cfg0 = MDSS_PLL_REG_R(base, DSIPHY_CMN_CLK_CFG0);
	u32 cfg1 = MDSS_PLL_REG_R(base, DSIPHY_CMN_CLK_CFG1);

	return pll->vco_current_rate &&
		(MDSS_PLL_REG_R(base, DSIPHY_CMN_PLL_CNTRL) & BIT(0)) &&
		(status & (BIT(5) | BIT(0))) == (BIT(5) | BIT(0)) &&
		mdss_8953_pll_read_rate(pll) == pll->vco_current_rate &&
		(cfg0 & 0xff) == (pll->n1div | (pll->n2div << 4)) &&
		(cfg1 & 0x3) == 1;
}

static long mdss_8953_vco_round_rate(unsigned long rate)
{
	if (rate < VCO_MIN_RATE)
		return VCO_MIN_RATE;
	if (rate > VCO_MAX_RATE)
		return VCO_MAX_RATE;
	return rate;
}

/* Divider helpers: N1 (postdiv) and N2, identity-mapped 1-15 in CLK_CFG0. */

static int mdss_8953_div_determine(struct clk_hw *hw,
				   struct clk_rate_request *req)
{
	return divider_determine_rate(hw, req, NULL, 4,
				      CLK_DIVIDER_ONE_BASED);
}

static void mdss_8953_postdiv_set(struct mdss_8953_pll *pll, int div)
{
	u32 cfg0;

	pll->out.pll_postdiv = DSI_PLL_DEFAULT_POSTDIV;
	pll->out.pll_n1div = div;
	pll->n1div = div;
	if (!pll->prepared)
		return;

	/* A successful prepare already holds power for this register access. */
	cfg0 = MDSS_PLL_REG_R(pll->pll_base, DSIPHY_CMN_CLK_CFG0);
	cfg0 &= ~0xf;
	cfg0 |= (div & 0xf);
	MDSS_PLL_REG_W(pll->pll_base, DSIPHY_CMN_CLK_CFG0, cfg0);
	wmb();
}

static void mdss_8953_n2div_set(struct mdss_8953_pll *pll, int div)
{
	u32 cfg0;

	pll->out.pll_n2div = div;
	pll->n2div = div;
	if (!pll->prepared)
		return;

	cfg0 = MDSS_PLL_REG_R(pll->pll_base, DSIPHY_CMN_CLK_CFG0);
	cfg0 &= ~0xf0;
	cfg0 |= ((div & 0xf) << 4);
	MDSS_PLL_REG_W(pll->pll_base, DSIPHY_CMN_CLK_CFG0, cfg0);
	MDSS_PLL_REG_W(pll->pll_base, DSIPHY_CMN_CLK_CFG1, 1);
	wmb();
}

/* CCF operations. */

static unsigned long mdss_8953_vco_recalc(struct clk_hw *hw,
					  unsigned long parent_rate)
{
	struct mdss_8953_pll *pll = to_mdss_8953_pll(hw, vco_hw);

	return pll->vco_current_rate;
}

static int mdss_8953_vco_determine(struct clk_hw *hw,
				   struct clk_rate_request *req)
{
	req->rate = mdss_8953_vco_round_rate(req->rate);
	return 0;
}

static int mdss_8953_vco_set(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent_rate)
{
	struct mdss_8953_pll *pll = to_mdss_8953_pll(hw, vco_hw);

	if (rate < VCO_MIN_RATE || rate > VCO_MAX_RATE)
		return -EINVAL;

	/*
	 * CCF does not propagate set_rate callback failures. Cache the
	 * configuration here; all fallible power/program/lock work belongs
	 * in prepare. CLK_SET_RATE_GATE prevents retuning a prepared VCO.
	 */
	mutex_lock(&pll->lock);
	mdss_8953_vco_calc_rate(pll, rate);
	mutex_unlock(&pll->lock);
	return 0;
}

static int mdss_8953_vco_prepare(struct clk_hw *hw)
{
	struct mdss_8953_pll *pll = to_mdss_8953_pll(hw, vco_hw);
	int rc;

	mutex_lock(&pll->lock);
	rc = mdss_8953_pll_power_get(pll);
	if (rc)
		goto unlock;

	/* Adopt a still-locked bootloader configuration without resetting it. */
	if (!mdss_8953_pll_matches_state(pll)) {
		/*
		 * Orphan reparenting can prepare the PLL before the first
		 * set_rate. As in upstream 14nm DSI, program a valid minimum
		 * rate rather than starting an unconfigured zero-rate PLL.
		 * Restore the cached configuration after a power/PHY reset.
		 */
		mdss_8953_vco_calc_rate(pll, pll->vco_current_rate ?
				      pll->vco_current_rate : VCO_MIN_RATE);
		mdss_8953_commit(pll);
		rc = mdss_8953_enable_seq(pll);
		if (rc) {
			mdss_8953_pll_stop(pll);
			mdss_8953_pll_power_put(pll);
			goto unlock;
		}
	}

	pll->prepared = true;
unlock:
	mutex_unlock(&pll->lock);
	return rc;
}

static void mdss_8953_vco_unprepare(struct clk_hw *hw)
{
	struct mdss_8953_pll *pll = to_mdss_8953_pll(hw, vco_hw);

	mutex_lock(&pll->lock);
	if (pll->prepared) {
		mdss_8953_pll_stop(pll);
		pll->prepared = false;
		mdss_8953_pll_power_put(pll);
	}
	mutex_unlock(&pll->lock);
}

static const struct clk_ops mdss_8953_vco_ops = {
	.recalc_rate = mdss_8953_vco_recalc,
	.determine_rate = mdss_8953_vco_determine,
	.set_rate = mdss_8953_vco_set,
	.prepare = mdss_8953_vco_prepare,
	.unprepare = mdss_8953_vco_unprepare,
	/* clk_unregister unpublishes ops and holds the CCF prepare lock. */
	.terminate = mdss_8953_vco_unprepare,
};

static unsigned long mdss_8953_postdiv_recalc(struct clk_hw *hw,
					      unsigned long parent_rate)
{
	struct mdss_8953_pll *pll = to_mdss_8953_pll(hw, postdiv_hw);
	int div = pll->n1div > 0 ? pll->n1div : 1;

	return DIV_ROUND_UP(parent_rate, div);
}

static unsigned long mdss_8953_n2div_recalc(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	struct mdss_8953_pll *pll = to_mdss_8953_pll(hw, n2div_hw);
	int div = pll->n2div > 0 ? pll->n2div : 1;

	return DIV_ROUND_UP(parent_rate, div);
}

static int mdss_8953_postdiv_set_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long parent_rate)
{
	struct mdss_8953_pll *pll = to_mdss_8953_pll(hw, postdiv_hw);
	int div;

	if (!rate || !parent_rate)
		return -EINVAL;
	div = divider_get_val(rate, parent_rate, NULL, 4,
			      CLK_DIVIDER_ONE_BASED);
	if (div < 0)
		return div;

	mutex_lock(&pll->lock);
	mdss_8953_postdiv_set(pll, div);
	mutex_unlock(&pll->lock);
	return 0;
}

static int mdss_8953_n2div_set_rate(struct clk_hw *hw, unsigned long rate,
				    unsigned long parent_rate)
{
	struct mdss_8953_pll *pll = to_mdss_8953_pll(hw, n2div_hw);
	int div;

	if (!rate || !parent_rate)
		return -EINVAL;
	div = divider_get_val(rate, parent_rate, NULL, 4,
			      CLK_DIVIDER_ONE_BASED);
	if (div < 0)
		return div;

	mutex_lock(&pll->lock);
	mdss_8953_n2div_set(pll, div);
	mutex_unlock(&pll->lock);
	return 0;
}

static const struct clk_ops mdss_8953_postdiv_ops = {
	.recalc_rate = mdss_8953_postdiv_recalc,
	.determine_rate = mdss_8953_div_determine,
	.set_rate = mdss_8953_postdiv_set_rate,
};

static const struct clk_ops mdss_8953_n2div_ops = {
	.recalc_rate = mdss_8953_n2div_recalc,
	.determine_rate = mdss_8953_div_determine,
	.set_rate = mdss_8953_n2div_set_rate,
};

static struct clk_hw *mdss_8953_clk_xlate(struct of_phandle_args *clkspec,
					  void *data)
{
	struct mdss_8953_pll *pll = data;

	if (!pll || clkspec->args_count != 1)
		return ERR_PTR(-EINVAL);

	switch (clkspec->args[0]) {
	case MDSS_DSI_PLL_BYTE_MUX:
	case MDSS_DSI_PLL_BYTE_SRC:
		return &pll->byte.hw;
	case MDSS_DSI_PLL_PIXEL_MUX:
	case MDSS_DSI_PLL_PIXEL_SRC:
		return &pll->pixel.hw;
	case MDSS_DSI_PLL_VCO:
		return &pll->vco_hw;
	default:
		return ERR_PTR(-EINVAL);
	}
}

static int mdss_8953_pll_register(struct mdss_8953_pll *pll)
{
	struct device *dev = pll->dev;
	struct clk_init_data vco_init = {}, postdiv_init = {}, n2div_init = {};
	struct clk_init_data byte_init = {}, pixel_init = {};
	char *vco_name, *postdiv_name, *n2div_name, *byte_name, *pixel_name;
	int rc;

	vco_name = devm_kasprintf(dev, GFP_KERNEL, "mdss_dsi_pll%u_vco",
				  pll->index);
	postdiv_name = devm_kasprintf(dev, GFP_KERNEL,
				      "mdss_dsi_pll%u_postdiv", pll->index);
	n2div_name = devm_kasprintf(dev, GFP_KERNEL, "mdss_dsi_pll%u_n2div",
				    pll->index);
	byte_name = devm_kasprintf(dev, GFP_KERNEL, "mdss_dsi_pll%u_byte",
				   pll->index);
	pixel_name = devm_kasprintf(dev, GFP_KERNEL, "mdss_dsi_pll%u_pixel",
				    pll->index);
	if (!vco_name || !postdiv_name || !n2div_name || !byte_name ||
	    !pixel_name)
		return -ENOMEM;

	vco_init.name = vco_name;
	vco_init.ops = &mdss_8953_vco_ops;
	vco_init.flags = CLK_GET_RATE_NOCACHE | CLK_SET_RATE_GATE;
	pll->vco_hw.init = &vco_init;
	rc = devm_clk_hw_register(dev, &pll->vco_hw);
	if (rc)
		return rc;

	postdiv_init.name = postdiv_name;
	postdiv_init.ops = &mdss_8953_postdiv_ops;
	postdiv_init.parent_hws = (const struct clk_hw *[]){ &pll->vco_hw };
	postdiv_init.num_parents = 1;
	postdiv_init.flags = CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT;
	pll->postdiv_hw.init = &postdiv_init;
	rc = devm_clk_hw_register(dev, &pll->postdiv_hw);
	if (rc)
		return rc;

	n2div_init.name = n2div_name;
	n2div_init.ops = &mdss_8953_n2div_ops;
	n2div_init.parent_hws = (const struct clk_hw *[]){ &pll->postdiv_hw };
	n2div_init.num_parents = 1;
	/* Pixel-rate changes must not retune the shared byte-clock parent. */
	n2div_init.flags = CLK_GET_RATE_NOCACHE;
	pll->n2div_hw.init = &n2div_init;
	rc = devm_clk_hw_register(dev, &pll->n2div_hw);
	if (rc)
		return rc;

	pll->byte.mult = 1;
	pll->byte.div = 8;
	pll->byte.hw.init = &byte_init;
	byte_init.name = byte_name;
	byte_init.ops = &clk_fixed_factor_ops;
	byte_init.parent_hws = (const struct clk_hw *[]){ &pll->postdiv_hw };
	byte_init.num_parents = 1;
	byte_init.flags = CLK_SET_RATE_PARENT;
	rc = devm_clk_hw_register(dev, &pll->byte.hw);
	if (rc)
		return rc;

	pll->pixel.mult = 1;
	pll->pixel.div = 2;
	pll->pixel.hw.init = &pixel_init;
	pixel_init.name = pixel_name;
	pixel_init.ops = &clk_fixed_factor_ops;
	pixel_init.parent_hws = (const struct clk_hw *[]){ &pll->n2div_hw };
	pixel_init.num_parents = 1;
	pixel_init.flags = CLK_SET_RATE_PARENT;
	rc = devm_clk_hw_register(dev, &pll->pixel.hw);
	if (rc)
		return rc;

	return devm_of_clk_add_hw_provider(dev, mdss_8953_clk_xlate, pll);
}

static int mdss_8953_pll_probe(struct platform_device *pdev)
{
	struct mdss_8953_pll *pll;
	u32 index = 0;
	int rc;

	pll = devm_kzalloc(&pdev->dev, sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return -ENOMEM;

	pll->dev = &pdev->dev;
	mutex_init(&pll->lock);
	of_property_read_u32(pdev->dev.of_node, "cell-index", &index);
	pll->index = index;

	pll->pll_base = devm_platform_ioremap_resource_byname(pdev,
							       "pll_base");
	if (IS_ERR(pll->pll_base))
		return PTR_ERR(pll->pll_base);

	pll->iface_clk = devm_clk_get(&pdev->dev, "iface_clk");
	if (IS_ERR(pll->iface_clk))
		return PTR_ERR(pll->iface_clk);

	pll->vco_ref_clk_rate = VCO_REF_RATE;
	pll->ssc_en = of_property_read_bool(pdev->dev.of_node,
					     "qcom,dsi-pll-ssc-en");
	pll->ssc_freq = 31500;
	pll->ssc_ppm = 5000;
	pll->n1div = 1;
	pll->n2div = 1;

	/* Release PM only after the provider and its clocks are unpublished. */
	rc = devm_pm_runtime_enable(&pdev->dev);
	if (rc)
		return rc;
	rc = mdss_8953_pll_power_get(pll);
	if (rc)
		return rc;
	mdss_8953_pll_read_state(pll);
	mdss_8953_pll_power_put(pll);
	rc = mdss_8953_pll_register(pll);
	if (rc)
		return rc;

	dev_info(&pdev->dev, "Registered DSI PLL ndx=%u clocks\n", pll->index);
	return 0;
}

static const struct of_device_id mdss_8953_pll_match[] = {
	{ .compatible = "qcom,mdss-dsi-pll-8953", },
	{}
};
MODULE_DEVICE_TABLE(of, mdss_8953_pll_match);

static struct platform_driver mdss_8953_pll_driver = {
	.probe = mdss_8953_pll_probe,
	.driver = {
		.name = "mdss-dsi-pll-8953",
		.of_match_table = mdss_8953_pll_match,
	},
};
module_platform_driver(mdss_8953_pll_driver);

MODULE_DESCRIPTION("MSM8953 DSI PLL CCF provider");
MODULE_LICENSE("GPL");
