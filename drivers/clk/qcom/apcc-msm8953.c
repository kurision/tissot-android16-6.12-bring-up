// SPDX-License-Identifier: GPL-2.0-only
/*
 * MSM8953 shared CPU/CCI clock domain.
 * Adapted from oc-test/drivers/clk/msm/{clock-cpu-8953,clock-pll}.c.
 * Copyright (c) 2013-2018, The Linux Foundation. All rights reserved.
 *
 * One integer PLL drives both clusters (divide by 1) and CCI (divide by 2.5).
 * The PLL and RCGs are deliberately private: an independently adjusted parent
 * would bypass the shared voltage transaction. Both CPU clock IDs alias one
 * CCF clock; CCI is a read-only domain output. Rates are read from hardware.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include <dt-bindings/clock/qcom,apcc-msm8953.h>
#include <dt-bindings/power/qcom-rpmpd.h>

#define APCC_PLL_MODE		0x00
#define APCC_PLL_L		0x08
#define APCC_PLL_ALPHA		0x10
#define APCC_PLL_USER		0x18
#define APCC_PLL_CONFIG		0x20
#define APCC_PLL_CONFIG_HI	0x24
#define APCC_PLL_TEST		0x30
#define APCC_PLL_TEST_HI		0x34
#define APCC_PLL_ON		GENMASK(2, 0)
#define APCC_PLL_LOCK		BIT(31)
#define APCC_PLL_ALPHA_EN	BIT(24)
#define APCC_PLL_PRE_DIV		BIT(12)
#define APCC_PLL_POST_DIV	GENMASK(9, 8)
#define APCC_PLL_OUTPUTS		(BIT(0) | BIT(3))
#define APCC_RCG_CMD		0x0
#define APCC_RCG_CFG		0x4
#define APCC_RCG_CBCR		0x8
#define APCC_RCG_ENABLE		BIT(0)
#define APCC_RCG_UPDATE		BIT(0)
#define APCC_RCG_DIRTY		BIT(4)
#define APCC_RCG_MASK		(GENMASK(10, 8) | GENMASK(4, 0))
#define APCC_SRC_XO		0
#define APCC_SRC_GPLL2		2
#define APCC_SRC_GPLL0		4
#define APCC_SRC_PLL		5
#define APCC_CPU_DIV		1
#define APCC_CCI_DIV		4
#define APCC_TIMEOUT_US		500
/*
 * The donor de-asserts the PLL reset and then waits 200 us before it
 * trusts the lock indication, because the hardware shows early transient
 * locks around 16 us after reset. It then polls the lock bit for up to
 * 200 us, reading it twice so a transient lock is not accepted. The port
 * used a single 200 us poll after a 50 us delay, which is shorter than the
 * donor's budget and turned a lockable PLL into a spurious timeout.
 */
#define APCC_PLL_LOCK_DELAY_US	200
#define APCC_PLL_LOCK_LOOPS	200
/*
 * The donor overclock table runs the APCS HFPLL on the same 19.2 MHz grid
 * from 480 MHz to 2784 MHz. The board's qcom,speed*-bin-v0 plans select the
 * matching CPR corner; this range is the PLL's own limit.
 */
#define APCC_MIN_RATE		480000000UL
#define APCC_MAX_RATE		2784000000UL
/* The donor's clock-cpu-8953 early handoff leaves CPU4 at 652.8 MHz. */
#define APCC_BOOT_RATE		652800000UL
#define APCC_XO_RATE		19200000UL
/* The mux ids are the clock ids, so a set of muxes is a bit mask. */
#define APCC_MUX_ALL		((1U << APCC_NUM_CLOCKS) - 1)

static const unsigned int apcc_pll_offsets[] = {
	APCC_PLL_MODE, APCC_PLL_L, APCC_PLL_ALPHA, APCC_PLL_USER,
	APCC_PLL_CONFIG, APCC_PLL_CONFIG_HI, APCC_PLL_TEST, APCC_PLL_TEST_HI,
};

struct apcc_plan {
	unsigned long *rates;
	int *corners;
	size_t num;
};

struct apcc_state {
	u32 pll[ARRAY_SIZE(apcc_pll_offsets)];
	u32 cfg[APCC_NUM_CLOCKS];
	u32 cpu4_cbcr;
	int corner;
};

struct apcc_output {
	struct clk_hw hw;
	struct notifier_block nb;
	struct apcc_msm8953 *apcc;
	unsigned int id;
};

struct apcc_msm8953 {
	struct device *dev;
	struct regmap *pll;
	struct regmap *mux[APCC_NUM_CLOCKS];
	struct apcc_output output[APCC_NUM_CLOCKS];
	struct apcc_plan cpu_plan;
	struct apcc_plan cci_plan;
	struct regulator *supply;
	unsigned long input_rates[3];
	/* Serializes PRE/POST/ABORT state and uncached rate readback. */
	struct mutex lock;
	struct apcc_state saved;
	int corner;
	int target_corner;
	bool pending;
	bool faulted;
};

static const char * const apcc_input_names[] = {
	"xo", "gpll0_early", "gpll2_early",
};

/*
 * These are boot CPU sources, not optional device clocks. Like the donor's
 * permanent CPU clock references, keep successful acquisitions for system
 * lifetime, including on probe failure. A devm disable could gate a source
 * firmware is still using. Deferred probes reuse the pins rather than leak
 * another enable/exclusive reference on each attempt. This driver is built-in.
 */
static struct {
	struct clk *clock;
	bool enabled;
	bool protected;
} apcc_inputs[ARRAY_SIZE(apcc_input_names)];
static DEFINE_MUTEX(apcc_inputs_lock);

/* Like the source clocks, successful power pins survive a failed probe. */
static struct {
	struct regulator *supply;
	struct device *mx;
	bool enabled;
	bool mx_active;
	bool mx_voted;
	bool faulted;
	int corner;
} apcc_power;
static DEFINE_MUTEX(apcc_power_lock);

static const char * const apcc_rcg_names[] = {
	"cpu0-rcg-clk", "cpu4-rcg-clk", "cci-rcg-clk",
};

static const char * const apcc_output_names[] = {
	"apcc_cpu0", "apcc_cpu4", "apcc_cci",
};

static const struct regmap_config apcc_pll_regmap_config = {
	.name = "apcs-pll",
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = APCC_PLL_TEST_HI,
	.fast_io = true,
};

/* Lowest corner covering the rate, as in the donor's find_vdd_level(). */
static int apcc_plan_corner(const struct apcc_plan *plan, unsigned long rate)
{
	size_t i;

	for (i = 0; i < plan->num; i++)
		if (rate <= plan->rates[i])
			return plan->corners[i];
	return -ERANGE;
}

static unsigned long apcc_cci_rate(unsigned long rate)
{
	return div_u64((u64)rate * 2, 5);
}

static int apcc_set_corner(struct apcc_msm8953 *apcc, int corner)
{
	int ret = 0;

	mutex_lock(&apcc_power_lock);
	if (corner != apcc_power.corner) {
		ret = regulator_set_voltage(apcc->supply, corner, corner);
		if (!ret)
			apcc_power.corner = corner;
	}
	if (!ret)
		apcc->corner = corner;
	mutex_unlock(&apcc_power_lock);
	return ret;
}

static int apcc_enable_supply(struct apcc_msm8953 *apcc)
{
	int ret = 0;

	mutex_lock(&apcc_power_lock);
	if (!apcc_power.enabled) {
		ret = regulator_enable(apcc->supply);
		if (!ret)
			apcc_power.enabled = true;
	}
	mutex_unlock(&apcc_power_lock);
	return ret;
}

static int apcc_read_state(struct apcc_msm8953 *apcc, struct apcc_state *state)
{
	u32 cmd;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(apcc_pll_offsets); i++) {
		ret = regmap_read(apcc->pll, apcc_pll_offsets[i], &state->pll[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < APCC_NUM_CLOCKS; i++) {
		ret = regmap_read(apcc->mux[i], APCC_RCG_CMD, &cmd);
		if (ret)
			return ret;
		if (cmd & (APCC_RCG_UPDATE | APCC_RCG_DIRTY))
			return -EBUSY;
		ret = regmap_read(apcc->mux[i], APCC_RCG_CFG, &state->cfg[i]);
		if (ret)
			return ret;
	}
	ret = regmap_read(apcc->mux[APCC_CPU4_CLK], APCC_RCG_CBCR, &state->cpu4_cbcr);
	if (ret)
		return ret;
	state->corner = apcc->corner;
	return 0;
}

static unsigned long apcc_state_rate(struct apcc_msm8953 *apcc,
				     const struct apcc_state *state, unsigned int id)
{
	unsigned int src = (state->cfg[id] >> 8) & 7;
	unsigned int div = (state->cfg[id] & 31) + 1;
	unsigned long rate;

	switch (src) {
	case APCC_SRC_XO:
		rate = apcc->input_rates[0];
		break;
	case APCC_SRC_GPLL0:
		rate = apcc->input_rates[1];
		break;
	case APCC_SRC_GPLL2:
		rate = apcc->input_rates[2];
		break;
	case APCC_SRC_PLL:
		/* Only the donor's integer, undivided early PLL output is supported. */
		if (!(state->pll[0] & APCC_PLL_LOCK) ||
		    (state->pll[0] & APCC_PLL_ON) != APCC_PLL_ON ||
		    !(state->pll[3] & BIT(3)) ||
		    (state->pll[3] & (APCC_PLL_ALPHA_EN | APCC_PLL_PRE_DIV)))
			return 0;
		rate = apcc->input_rates[0] * state->pll[1];
		break;
	default:
		return 0;
	}
	/* HID field zero bypasses the divider, as in clk-rcg2.c's calc_rate(). */
	return div_u64((u64)rate * 2, max(div, 2U));
}

static int apcc_state_corner(struct apcc_msm8953 *apcc,
			     const struct apcc_state *state)
{
	const struct apcc_plan *plan;
	unsigned long rate;
	int i, corner = 1, ret;

	for (i = 0; i < APCC_NUM_CLOCKS; i++) {
		plan = i == APCC_CCI_CLK ? &apcc->cci_plan : &apcc->cpu_plan;
		rate = apcc_state_rate(apcc, state, i);
		if (!rate)
			return -EINVAL;
		ret = apcc_plan_corner(plan, rate);
		if (ret < 0)
			return ret;
		corner = max(corner, ret);
	}
	return corner;
}

static int apcc_write_mux(struct apcc_msm8953 *apcc, unsigned int id, u32 cfg)
{
	u32 value;
	int ret;

	ret = regmap_update_bits(apcc->mux[id], APCC_RCG_CFG, APCC_RCG_MASK, cfg);
	if (ret)
		return ret;
	ret = regmap_update_bits(apcc->mux[id], APCC_RCG_CMD,
				 APCC_RCG_UPDATE, APCC_RCG_UPDATE);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(apcc->mux[id], APCC_RCG_CMD, value,
				       !(value & APCC_RCG_UPDATE), 1, APCC_TIMEOUT_US);
	if (ret)
		return ret;
	ret = regmap_read(apcc->mux[id], APCC_RCG_CFG, &value);
	if (ret)
		return ret;
	return (value & APCC_RCG_MASK) == (cfg & APCC_RCG_MASK) ? 0 : -EIO;
}

/*
 * Park the selected outputs on the independent, rate-protected GPLL0 source
 * before the shared PLL is touched.
 *
 * The divider is deliberately left alone: the donor switches a mux source
 * and divider in a single write, and changing the CCI divider changes the
 * rate of the interconnect the running CPUs use. A mux that already sits on
 * GPLL0 is not written at all, because an update handshake on the mux that
 * clocks the running CPU0 cluster is not free.
 */
static int apcc_park(struct apcc_msm8953 *apcc, unsigned int muxs)
{
	unsigned int i;
	int ret;

	for (i = 0; i < APCC_NUM_CLOCKS; i++) {
		u32 cfg;

		if (!(muxs & BIT(i)))
			continue;
		ret = regmap_read(apcc->mux[i], APCC_RCG_CFG, &cfg);
		if (ret)
			return ret;
		if (((cfg >> 8) & 7) == APCC_SRC_GPLL0)
			continue;
		ret = apcc_write_mux(apcc, i, (APCC_SRC_GPLL0 << 8) |
				     (cfg & GENMASK(4, 0)));
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * The donor waits 200 us after it de-asserts the reset before it trusts the
 * lock indication, then polls the status bit for up to 200 us and reads it
 * twice, so that the early transient locks the hardware shows are not taken
 * for a stable lock. The lock bit is MODE bit 31 of the APCS HF PLL.
 */
static int apcc_wait_lock(struct apcc_msm8953 *apcc)
{
	u32 mode;
	int i, ret;

	usleep_range(APCC_PLL_LOCK_DELAY_US, APCC_PLL_LOCK_DELAY_US + 10);
	for (i = 0; i < APCC_PLL_LOCK_LOOPS; i++) {
		ret = regmap_read(apcc->pll, APCC_PLL_MODE, &mode);
		if (ret)
			return ret;
		if (mode & APCC_PLL_LOCK) {
			/* Re-read: the donor rejects the transient locks. */
			usleep_range(1, 2);
			ret = regmap_read(apcc->pll, APCC_PLL_MODE, &mode);
			if (ret)
				return ret;
			if (mode & APCC_PLL_LOCK)
				return 0;
		}
		usleep_range(1, 2);
	}
	return -ETIMEDOUT;
}

static int apcc_write_cpu4_branch(struct apcc_msm8953 *apcc, u32 value)
{
	u32 observed;
	int ret;

	ret = regmap_update_bits(apcc->mux[APCC_CPU4_CLK], APCC_RCG_CBCR,
				 APCC_RCG_ENABLE, value);
	if (ret)
		return ret;
	ret = regmap_read(apcc->mux[APCC_CPU4_CLK], APCC_RCG_CBCR, &observed);
	if (ret)
		return ret;
	return (observed & APCC_RCG_ENABLE) == (value & APCC_RCG_ENABLE) ? 0 : -EIO;
}

/*
 * Program the shared PLL. In the rate-change path every consumer is parked
 * on GPLL0 first; in the boot handoff the PLL feeds nothing yet, because the
 * firmware left it off. Either way the PLL is not carrying a live consumer
 * while it is reprogrammed.
 */
static int apcc_program_pll(struct apcc_msm8953 *apcc, unsigned long rate)
{
	u32 mode, user, l;
	int ret;

	if (rate < APCC_MIN_RATE || rate > APCC_MAX_RATE || rate % APCC_XO_RATE)
		return -ERANGE;
	ret = regmap_read(apcc->pll, APCC_PLL_MODE, &mode);
	if (ret)
		return ret;
	ret = regmap_read(apcc->pll, APCC_PLL_USER, &user);
	if (ret)
		return ret;
	if (mode & APCC_PLL_LOCK) {
		if ((mode & APCC_PLL_ON) != APCC_PLL_ON || !(user & BIT(3)) ||
		    user & (APCC_PLL_ALPHA_EN | APCC_PLL_PRE_DIV))
			return -EINVAL;
	} else {
		/* A partially enabled PLL is not a documented cold-init state. */
		if (mode & APCC_PLL_ON)
			return -EINVAL;
		ret = regmap_update_bits(apcc->pll, APCC_PLL_USER,
					 APCC_PLL_PRE_DIV | APCC_PLL_POST_DIV |
					 APCC_PLL_ALPHA_EN | APCC_PLL_OUTPUTS,
					 0x100 | APCC_PLL_OUTPUTS);
		if (ret)
			return ret;
		ret = regmap_write(apcc->pll, APCC_PLL_ALPHA, 0);
		if (ret)
			return ret;
		ret = regmap_write(apcc->pll, APCC_PLL_CONFIG, 0x200d4828);
		if (ret)
			return ret;
		ret = regmap_write(apcc->pll, APCC_PLL_CONFIG_HI, 0x6);
		if (ret)
			return ret;
		ret = regmap_write(apcc->pll, APCC_PLL_TEST, 0x1c000000);
		if (ret)
			return ret;
		ret = regmap_write(apcc->pll, APCC_PLL_TEST_HI, 0x4000);
		if (ret)
			return ret;
	}
	l = rate / APCC_XO_RATE;
	ret = regmap_write(apcc->pll, APCC_PLL_L, l);
	if (ret)
		return ret;
	/* The donor changes integer L directly; there is no alpha latch request. */
	mb();
	if (!(mode & APCC_PLL_LOCK)) {
		ret = regmap_update_bits(apcc->pll, APCC_PLL_MODE, BIT(1), BIT(1));
		if (ret)
			return ret;
		usleep_range(10, 20);
		ret = regmap_update_bits(apcc->pll, APCC_PLL_MODE, BIT(2), BIT(2));
		if (ret)
			return ret;
	}
	ret = apcc_wait_lock(apcc);
	if (ret)
		return ret;
	ret = regmap_update_bits(apcc->pll, APCC_PLL_MODE, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = regmap_read(apcc->pll, APCC_PLL_MODE, &user);
	if (ret)
		return ret;
	if ((user & (APCC_PLL_ON | APCC_PLL_LOCK)) != (APCC_PLL_ON | APCC_PLL_LOCK))
		return -EIO;
	ret = regmap_read(apcc->pll, APCC_PLL_USER, &user);
	if (ret)
		return ret;
	if (!(user & BIT(3)) || user & (APCC_PLL_ALPHA_EN | APCC_PLL_PRE_DIV))
		return -EIO;
	ret = regmap_read(apcc->pll, APCC_PLL_L, &user);
	return ret ? ret : (user == l ? 0 : -EIO);
}

/*
 * Restore the saved PLL only while the consumers of the selected muxes are
 * on the parking source. Only the muxes this transaction moved are touched:
 * a mux that already carries the saved configuration is not rewritten, so a
 * rollback never issues an update handshake on a mux it did not change.
 */
static int apcc_restore(struct apcc_msm8953 *apcc, unsigned int muxs)
{
	struct apcc_state *saved = &apcc->saved;
	struct apcc_state restored;
	int i, ret;

	ret = apcc_park(apcc, muxs);
	if (ret)
		goto fault;
	if (saved->pll[0] & APCC_PLL_LOCK) {
		ret = regmap_write(apcc->pll, APCC_PLL_L, saved->pll[1]);
		if (ret)
			goto fault;
		/* Commit the saved L value before polling the PLL lock indication. */
		mb();
		ret = apcc_wait_lock(apcc);
		if (ret)
			goto fault;
	} else {
		ret = regmap_update_bits(apcc->pll, APCC_PLL_MODE, APCC_PLL_ON, 0);
		if (ret)
			goto fault;
		for (i = 1; i < ARRAY_SIZE(apcc_pll_offsets); i++) {
			ret = regmap_write(apcc->pll, apcc_pll_offsets[i], saved->pll[i]);
			if (ret)
				goto fault;
		}
	}
	for (i = 0; i < APCC_NUM_CLOCKS; i++) {
		u32 cfg;

		if (!(muxs & BIT(i)))
			continue;
		ret = regmap_read(apcc->mux[i], APCC_RCG_CFG, &cfg);
		if (ret)
			goto fault;
		if ((cfg & APCC_RCG_MASK) == (saved->cfg[i] & APCC_RCG_MASK))
			continue;
		ret = apcc_write_mux(apcc, i, saved->cfg[i]);
		if (ret)
			goto fault;
	}
	ret = apcc_write_cpu4_branch(apcc, saved->cpu4_cbcr);
	if (ret)
		goto fault;
	ret = apcc_read_state(apcc, &restored);
	if (ret)
		goto fault;
	/* A successful bus write is not proof that the old PLL L/config latched. */
	for (i = 1; i < ARRAY_SIZE(apcc_pll_offsets); i++) {
		if (restored.pll[i] != saved->pll[i]) {
			ret = -EIO;
			goto fault;
		}
	}
	if ((restored.pll[0] & APCC_PLL_ON) != (saved->pll[0] & APCC_PLL_ON)) {
		ret = -EIO;
		goto fault;
	}
	/* A failed voltage decrease is safe: keep the higher vote. */
	ret = apcc_set_corner(apcc, saved->corner);
	if (ret)
		dev_warn(apcc->dev, "rollback retained higher corner: %d\n", ret);
	return 0;
fault:
	/* Do not drop voltage or attempt another retune after an incomplete restore. */
	apcc->faulted = true;
	apcc_power.faulted = true;
	dev_err(apcc->dev, "clock rollback failed: %d; retaining corner %d\n",
		ret, apcc->corner);
	return ret;
}

/*
 * The rate a mux runs at once it is parked on GPLL0. The divider is kept, so
 * the parked rate follows the mux's own divider and not the target ratio.
 */
static unsigned long apcc_parked_rate(struct apcc_msm8953 *apcc,
				      const struct apcc_state *state,
				      unsigned int id)
{
	unsigned int div = (state->cfg[id] & GENMASK(4, 0)) + 1;

	return div_u64((u64)apcc->input_rates[1] * 2, max(div, 2U));
}

static int apcc_begin(struct apcc_msm8953 *apcc, unsigned long rate)
{
	struct apcc_state observed;
	int old_corner, target, park, ret;
	size_t i;

	if (apcc->faulted || apcc_power.faulted)
		return -EIO;
	if (!apcc_power.mx_active || !apcc_power.mx_voted || !apcc_power.enabled)
		return -EIO;
	if (apcc->pending)
		return -EBUSY;
	/* Only exact, fused stock rates may reach the hardware transaction. */
	if (rate < APCC_MIN_RATE || rate > APCC_MAX_RATE || rate % APCC_XO_RATE)
		return -ERANGE;
	for (i = 1; i < apcc->cpu_plan.num; i++)
		if (rate == apcc->cpu_plan.rates[i])
			break;
	if (i == apcc->cpu_plan.num)
		return -ERANGE;
	target = apcc_plan_corner(&apcc->cpu_plan, rate);
	if (target < 0)
		return target;
	park = apcc_plan_corner(&apcc->cci_plan, apcc_cci_rate(rate));
	if (park < 0)
		return park;
	target = max(target, park);
	ret = apcc_read_state(apcc, &apcc->saved);
	if (ret)
		return ret;
	old_corner = apcc_state_corner(apcc, &apcc->saved);
	if (old_corner < 0)
		return old_corner;
	apcc->saved.corner = max(apcc->corner, old_corner);
	/*
	 * The corner that covers the parked rates. Every output keeps its own
	 * divider while it is parked, so each one is costed separately.
	 */
	park = 0;
	for (i = 0; i < APCC_NUM_CLOCKS; i++) {
		const struct apcc_plan *plan = i == APCC_CCI_CLK ?
					       &apcc->cci_plan : &apcc->cpu_plan;
		int corner = apcc_plan_corner(plan,
					      apcc_parked_rate(apcc, &apcc->saved, i));

		if (corner < 0)
			return corner;
		park = max(park, corner);
	}
	ret = apcc_set_corner(apcc, max(max(apcc->saved.corner, target), park));
	if (ret)
		return ret;
	ret = apcc_park(apcc, APCC_MUX_ALL);
	if (ret)
		goto rollback;
	ret = apcc_program_pll(apcc, rate);
	if (ret)
		goto rollback;
	/* Bring CCI to its final ratio before either CPU is accelerated. */
	ret = apcc_write_mux(apcc, APCC_CCI_CLK,
			     (APCC_SRC_PLL << 8) | APCC_CCI_DIV);
	if (ret)
		goto rollback;
	ret = apcc_write_mux(apcc, APCC_CPU0_CLK,
			     (APCC_SRC_PLL << 8) | APCC_CPU_DIV);
	if (ret)
		goto rollback;
	ret = apcc_write_mux(apcc, APCC_CPU4_CLK,
			     (APCC_SRC_PLL << 8) | APCC_CPU_DIV);
	if (ret)
		goto rollback;
	/* Firmware may leave CPU4's software branch-enable bit clear. */
	ret = apcc_write_cpu4_branch(apcc, APCC_RCG_ENABLE);
	if (ret)
		goto rollback;
	ret = apcc_read_state(apcc, &observed);
	if (ret)
		goto rollback;
	for (i = 0; i < APCC_NUM_CLOCKS; i++) {
		if (apcc_state_rate(apcc, &observed, i) !=
		    (i == APCC_CCI_CLK ? apcc_cci_rate(rate) : rate)) {
			ret = -EIO;
			goto rollback;
		}
	}
	apcc->target_corner = target;
	apcc->pending = true;
	return 0;
rollback:
	apcc_restore(apcc, APCC_MUX_ALL);
	return ret;
}

static void apcc_finish(struct apcc_msm8953 *apcc, bool abort)
{
	int ret;

	if (!apcc->pending)
		return;
	if (abort) {
		apcc_restore(apcc, APCC_MUX_ALL);
	} else {
		ret = apcc_set_corner(apcc, apcc->target_corner);
		if (ret)
			dev_warn(apcc->dev, "retaining higher corner after transition: %d\n", ret);
	}
	apcc->pending = false;
}

/* CCF propagates PRE vetoes, but ignores errors from a provider's .set_rate. */
static int apcc_rate_notify(struct notifier_block *nb, unsigned long event, void *data)
{
	struct apcc_output *output = container_of(nb, struct apcc_output, nb);
	struct apcc_msm8953 *apcc = output->apcc;
	struct clk_notifier_data *change = data;
	int ret = 0;

	mutex_lock(&apcc->lock);
	switch (event) {
	case PRE_RATE_CHANGE:
		ret = apcc_begin(apcc, change->new_rate);
		if (ret)
			dev_err_ratelimited(apcc->dev, "CPU transition to %lu Hz failed: %d\n",
					    change->new_rate, ret);
		break;
	case POST_RATE_CHANGE:
		apcc_finish(apcc, false);
		break;
	case ABORT_RATE_CHANGE:
		apcc_finish(apcc, true);
		break;
	}
	mutex_unlock(&apcc->lock);
	return notifier_from_errno(ret);
}

static unsigned long apcc_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	struct apcc_output *output = container_of(hw, struct apcc_output, hw);
	struct apcc_state state;
	unsigned long rate = 0;

	mutex_lock(&output->apcc->lock);
	if (!apcc_read_state(output->apcc, &state))
		rate = apcc_state_rate(output->apcc, &state, output->id);
	mutex_unlock(&output->apcc->lock);
	return rate;
}

static int apcc_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	struct apcc_output *output = container_of(hw, struct apcc_output, hw);
	const struct apcc_plan *plan = &output->apcc->cpu_plan;
	size_t i;

	/* These are domain outputs, not forwarding muxes; no parent is adjusted. */
	for (i = 1; i < plan->num; i++) {
		if (plan->rates[i] < req->rate || plan->rates[i] < req->min_rate)
			continue;
		if (plan->rates[i] < APCC_MIN_RATE)
			continue;
		if (plan->rates[i] > req->max_rate || plan->rates[i] > APCC_MAX_RATE)
			break;
		req->rate = plan->rates[i];
		return 0;
	}
	return -ERANGE;
}

static int apcc_set_rate(struct clk_hw *hw, unsigned long rate, unsigned long parent_rate)
{
	/* Hardware was committed, with error reporting, by PRE_RATE_CHANGE. */
	return 0;
}

static const struct clk_ops apcc_cpu_ops = {
	.recalc_rate = apcc_recalc_rate,
	.determine_rate = apcc_determine_rate,
	.set_rate = apcc_set_rate,
};

/* The donor programs CCI's ratio once; its rate follows the shared CPU PLL. */
static const struct clk_ops apcc_cci_ops = {
	.recalc_rate = apcc_recalc_rate,
};

static int apcc_read_plan(struct device *dev, const char *prop, struct apcc_plan *plan)
{
	u32 *raw;
	int count, i, ret;

	count = device_property_count_u32(dev, prop);
	if (count < 0)
		return count;
	if (count < 4 || count & 1)
		return -EINVAL;
	raw = devm_kmalloc_array(dev, count, sizeof(*raw), GFP_KERNEL);
	if (!raw)
		return -ENOMEM;
	ret = device_property_read_u32_array(dev, prop, raw, count);
	if (ret)
		goto out;
	plan->num = count / 2;
	plan->rates = devm_kmalloc_array(dev, plan->num, sizeof(*plan->rates), GFP_KERNEL);
	plan->corners = devm_kmalloc_array(dev, plan->num, sizeof(*plan->corners), GFP_KERNEL);
	if (!plan->rates || !plan->corners) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < plan->num; i++) {
		if ((!i && (raw[0] || raw[1])) ||
		    (i && (raw[2 * i] <= raw[2 * i - 2] || !raw[2 * i + 1] ||
			   raw[2 * i + 1] < raw[2 * i - 1]))) {
			ret = -EINVAL;
			goto out;
		}
		plan->rates[i] = raw[2 * i];
		plan->corners[i] = raw[2 * i + 1];
	}
out:
	devm_kfree(dev, raw);
	return ret;
}

static int apcc_read_bin_plan(struct device *dev, unsigned int bin,
			      const char *kind, struct apcc_plan *plan)
{
	char prop[32];

	snprintf(prop, sizeof(prop), "qcom,speed%u-bin-v0-%s", bin, kind);
	/* A malformed present plan is an error, not permission to use another bin. */
	if (!device_property_present(dev, prop))
		snprintf(prop, sizeof(prop), "qcom,speed0-bin-v0-%s", kind);
	return apcc_read_plan(dev, prop, plan);
}

static int apcc_pin_inputs(struct device *dev, unsigned long *rates)
{
	int i, ret = 0;

	mutex_lock(&apcc_inputs_lock);
	for (i = 0; i < ARRAY_SIZE(apcc_inputs); i++) {
		if (!apcc_inputs[i].clock) {
			struct clk *clock = clk_get(dev, apcc_input_names[i]);

			if (IS_ERR(clock)) {
				ret = PTR_ERR(clock);
				break;
			}
			apcc_inputs[i].clock = clock;
		}
		if (!apcc_inputs[i].enabled) {
			ret = clk_prepare_enable(apcc_inputs[i].clock);
			if (ret) {
				clk_put(apcc_inputs[i].clock);
				apcc_inputs[i].clock = NULL;
				break;
			}
			apcc_inputs[i].enabled = true;
		}
		if (!apcc_inputs[i].protected) {
			ret = clk_rate_exclusive_get(apcc_inputs[i].clock);
			if (ret)
				break;
			apcc_inputs[i].protected = true;
		}
		rates[i] = clk_get_rate(apcc_inputs[i].clock);
		if (!rates[i]) {
			ret = -EINVAL;
			break;
		}
	}
	mutex_unlock(&apcc_inputs_lock);
	return ret;
}

static int apcc_pin_power(struct apcc_msm8953 *apcc)
{
	struct device *dev = apcc->dev;
	struct regulator *supply;
	struct device *mx;
	int index, ret = 0;

	mutex_lock(&apcc_power_lock);
	if (!apcc_power.supply) {
		supply = regulator_get(dev, "vdd-cl");
		if (IS_ERR(supply)) {
			ret = PTR_ERR(supply);
			goto out;
		}
		apcc_power.supply = supply;
	}
	apcc->supply = apcc_power.supply;
	apcc->corner = apcc_power.corner;
	if (!apcc_power.mx) {
		index = of_property_match_string(dev->of_node, "power-domain-names", "mx");
		if (index < 0) {
			ret = index;
			goto out;
		}
		/*
		 * A single domain is also attached by the platform bus. Use a
		 * separate genpd device: probe-error detach of the bus attachment
		 * must not remove the PLL's lifetime MX vote.
		 */
		mx = genpd_dev_pm_attach_by_id(dev, index);
		if (IS_ERR_OR_NULL(mx)) {
			ret = mx ? PTR_ERR(mx) : -ENODEV;
			goto out;
		}
		apcc_power.mx = mx;
	}
	if (!apcc_power.mx_active) {
		ret = pm_runtime_resume_and_get(apcc_power.mx);
		if (ret < 0) {
			/* Runtime PM caches resume errors; retry with a fresh attachment. */
			dev_pm_domain_detach(apcc_power.mx, false);
			apcc_power.mx = NULL;
			goto out;
		}
		apcc_power.mx_active = true;
	}
	if (!apcc_power.mx_voted) {
		/* Donor vdd_pll requests MX SVS throughout the stock PLL range. */
		ret = dev_pm_genpd_set_performance_state(apcc_power.mx, RPM_SMD_LEVEL_SVS);
		if (ret)
			goto out;
		apcc_power.mx_voted = true;
	}
	ret = 0;
out:
	mutex_unlock(&apcc_power_lock);
	return ret;
}

/*
 * Boot handoff.
 *
 * This follows the donor's early_initcall (clock-cpu-8953.c:
 * cpu_clock_pwr_init): program the APCS HF PLL, then move the CPU4 alias1
 * mux to that PLL and enable the CPU4 core branch. The donor leaves the
 * running CPU0 cluster and the CCI on the firmware's GPLL0 source and
 * reaches the coupled CPU/CCI state later, from the first cpufreq
 * transition, which starts from exactly this state.
 *
 * Re-programming the mux of the running CPU0 cluster and the live CCI
 * divider during early boot is not something the donor does, and this port
 * has never executed it on hardware. The handoff therefore stays inside the
 * donor's proven writes; the coupled transaction remains in the rate-change
 * path, where the donor performs the same kind of mux writes.
 */
static int apcc_handoff(struct apcc_msm8953 *apcc)
{
	struct apcc_state boot, observed;
	int corner, ret;

	if (apcc_power.faulted || !apcc_power.mx_active || !apcc_power.mx_voted)
		return -EIO;
	ret = apcc_read_state(apcc, &boot);
	if (ret)
		return ret;
	dev_info(apcc->dev, "boot state: pll %08x %08x %08x %08x mux %08x %08x %08x\n",
		 boot.pll[0], boot.pll[1], boot.pll[2], boot.pll[3],
		 boot.cfg[APCC_CPU0_CLK], boot.cfg[APCC_CPU4_CLK], boot.cfg[APCC_CCI_CLK]);
	if ((boot.pll[0] & APCC_PLL_LOCK) &&
	    ((boot.pll[0] & APCC_PLL_ON) != APCC_PLL_ON ||
	     !(boot.pll[3] & BIT(3)) ||
	     boot.pll[3] & (APCC_PLL_ALPHA_EN | APCC_PLL_PRE_DIV)))
		return -EINVAL;
	if (!(boot.pll[0] & APCC_PLL_LOCK) && boot.pll[0])
		return -EINVAL;
	if (apcc_state_rate(apcc, &boot, APCC_CPU0_CLK) > APCC_MAX_RATE ||
	    apcc_state_rate(apcc, &boot, APCC_CPU4_CLK) > APCC_MAX_RATE)
		return -ERANGE;
	corner = apcc_state_corner(apcc, &boot);
	if (corner < 0)
		return corner;
	/* Never reduce a vote retained by a previous failed probe. */
	ret = apcc_set_corner(apcc, max(corner, apcc->corner));
	if (ret)
		return ret;
	ret = apcc_enable_supply(apcc);
	if (ret)
		return ret;
	/* The snapshot a failed handoff is rolled back to. */
	apcc->saved = boot;
	apcc->saved.corner = max(apcc->corner, corner);
	/*
	 * The PLL feeds nothing yet: this is the only write in the handoff
	 * that cannot disturb the running CPUs.
	 */
	ret = apcc_program_pll(apcc, APCC_BOOT_RATE);
	if (ret)
		goto restore;
	/* The donor's CPU4 handoff: the alias1 mux to the PLL, then the
	 * core branch that lets the CPU4-7 cluster leave its reset.
	 */
	ret = apcc_write_mux(apcc, APCC_CPU4_CLK,
			     (APCC_SRC_PLL << 8) | APCC_CPU_DIV);
	if (ret)
		goto restore;
	ret = apcc_write_cpu4_branch(apcc, APCC_RCG_ENABLE);
	if (ret)
		goto restore;
	/*
	 * Verify the donor's end state: CPU4 on the PLL at the boot rate and
	 * its branch enabled, with the running CPU0 cluster and the CCI
	 * exactly as the firmware left them.
	 */
	ret = apcc_read_state(apcc, &observed);
	if (ret)
		goto restore;
	if (apcc_state_rate(apcc, &observed, APCC_CPU4_CLK) != APCC_BOOT_RATE ||
	    observed.cfg[APCC_CPU0_CLK] != boot.cfg[APCC_CPU0_CLK] ||
	    observed.cfg[APCC_CCI_CLK] != boot.cfg[APCC_CCI_CLK] ||
	    (observed.cpu4_cbcr & APCC_RCG_ENABLE) != APCC_RCG_ENABLE) {
		ret = -EIO;
		goto restore;
	}
	/* The rail must still cover every rate that is now running. */
	corner = apcc_state_corner(apcc, &observed);
	if (corner < 0) {
		ret = corner;
		goto restore;
	}
	ret = apcc_set_corner(apcc, max(corner, apcc->corner));
	if (ret)
		goto restore;
	apcc->target_corner = apcc->corner;
	return 0;
restore:
	apcc_restore(apcc, BIT(APCC_CPU4_CLK));
	return ret;
}

static int apcc_msm8953_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *clocks;
	struct apcc_msm8953 *apcc;
	struct clk *clock;
	struct resource *res;
	void __iomem *base;
	u32 bin;
	int i, ret;

	apcc = devm_kzalloc(dev, sizeof(*apcc), GFP_KERNEL);
	clocks = devm_kzalloc(dev, struct_size(clocks, hws, APCC_NUM_CLOCKS), GFP_KERNEL);
	if (!apcc || !clocks)
		return -ENOMEM;
	apcc->dev = dev;
	mutex_init(&apcc->lock);
	/*
	 * Attach the MX domain before anything that can fail. The platform
	 * bus attaches the same domain to this device and, when the probe
	 * fails, detaches it with power_off, which would drop the shared MX
	 * rail from under its other users. The domain reference taken here
	 * keeps the rail up for the lifetime of the driver.
	 */
	ret = apcc_pin_power(apcc);
	if (ret)
		return dev_err_probe(dev, ret, "CPU rail/MX acquisition failed\n");
	ret = apcc_pin_inputs(dev, apcc->input_rates);
	if (ret)
		return dev_err_probe(dev, ret, "boot clock source acquisition failed\n");
	if (apcc->input_rates[0] != APCC_XO_RATE)
		return dev_err_probe(dev, -EINVAL, "unsupported XO rate\n");
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pte-efuse");
	if (!res || resource_size(res) < sizeof(u32))
		return dev_err_probe(dev, -EINVAL, "missing or short PTE EFUSE window\n");
	base = devm_platform_ioremap_resource_byname(pdev, "pte-efuse");
	if (IS_ERR(base))
		return PTR_ERR(base);
	bin = (readl(base) >> 8) & 7;
	ret = apcc_read_bin_plan(dev, bin, "cl", &apcc->cpu_plan);
	if (ret)
		return dev_err_probe(dev, ret, "invalid CPU corner plan\n");
	ret = apcc_read_bin_plan(dev, bin, "cci", &apcc->cci_plan);
	if (ret)
		return dev_err_probe(dev, ret, "invalid CCI corner plan\n");
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pll");
	if (!res || resource_size(res) < APCC_PLL_TEST_HI + sizeof(u32))
		return dev_err_probe(dev, -EINVAL, "missing or short PLL window\n");
	base = devm_platform_ioremap_resource_byname(pdev, "pll");
	if (IS_ERR(base))
		return PTR_ERR(base);
	apcc->pll = devm_regmap_init_mmio(dev, base, &apcc_pll_regmap_config);
	if (IS_ERR(apcc->pll))
		return PTR_ERR(apcc->pll);
	for (i = 0; i < APCC_NUM_CLOCKS; i++) {
		const struct regmap_config apcc_mux_regmap_config = {
			.name = apcc_rcg_names[i],
			.reg_bits = 32,
			.val_bits = 32,
			.reg_stride = 4,
			.max_register = 0xc,
			.fast_io = true,
		};

		/* APM reserves only its own 12 bytes at 0xb1112a8, not this RCG. */
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   apcc_rcg_names[i]);
		if (!res || resource_size(res) < 0x10)
			return dev_err_probe(dev, -EINVAL, "missing or short RCG window\n");
		base = devm_platform_ioremap_resource_byname(pdev, apcc_rcg_names[i]);
		if (IS_ERR(base))
			return PTR_ERR(base);
		apcc->mux[i] = devm_regmap_init_mmio(dev, base, &apcc_mux_regmap_config);
		if (IS_ERR(apcc->mux[i]))
			return PTR_ERR(apcc->mux[i]);
	}
	ret = apcc_handoff(apcc);
	if (ret)
		return dev_err_probe(dev, ret, "CPU/CCI boot handoff failed\n");
	clocks->num = APCC_NUM_CLOCKS;
	for (i = 0; i < APCC_NUM_CLOCKS; i++) {
		struct clk_init_data init = { };
		struct apcc_output *output = &apcc->output[i];

		/* One policy and one clock cache for the two lockstep CPU clusters. */
		if (i == APCC_CPU4_CLK) {
			clocks->hws[i] = &apcc->output[APCC_CPU0_CLK].hw;
			continue;
		}
		output->apcc = apcc;
		output->id = i;
		init.name = apcc_output_names[i];
		init.ops = i == APCC_CCI_CLK ? &apcc_cci_ops : &apcc_cpu_ops;
		/* No gate ops: neither unused-clock cleanup nor unbind can gate a CPU. */
		init.flags = CLK_GET_RATE_NOCACHE;
		output->hw.init = &init;
		ret = devm_clk_hw_register(dev, &output->hw);
		if (ret)
			return dev_err_probe(dev, ret, "failed to register %s\n", init.name);
		clocks->hws[i] = &output->hw;
		if (i == APCC_CCI_CLK)
			continue;
		clock = devm_clk_hw_get_clk(dev, &output->hw, init.name);
		if (IS_ERR(clock))
			return PTR_ERR(clock);
		output->nb.notifier_call = apcc_rate_notify;
		ret = devm_clk_notifier_register(dev, clock, &output->nb);
		if (ret)
			return dev_err_probe(dev, ret, "failed to register rate notifier\n");
	}
	platform_set_drvdata(pdev, apcc);
	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clocks);
	if (!ret)
		dev_info(dev, "shared CPU/CCI domain, speed bin %u, CPU %lu CCI %lu Hz, corner %d\n",
			 bin, apcc_recalc_rate(&apcc->output[APCC_CPU0_CLK].hw, 0),
			 apcc_recalc_rate(&apcc->output[APCC_CCI_CLK].hw, 0), apcc->corner);
	return ret;
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
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(apcc_msm8953_driver);

MODULE_DESCRIPTION("Qualcomm MSM8953 shared CPU/CCI clock controller");
MODULE_LICENSE("GPL");
