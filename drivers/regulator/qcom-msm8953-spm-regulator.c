// SPDX-License-Identifier: GPL-2.0-only
/*
 * MSM8953 SPM-backed APC rail regulator.
 * Copyright (c) 2013-2020, The Linux Foundation. All rights reserved.
 *
 * Adapted from oc-test/drivers/regulator/spm-regulator.c.
 *
 * The PM8953 S5/S6 SMPS that feeds VDD_APC is driven through the shared
 * system-CCI SAW4, not by writing the PMIC VSET directly.  The SPM owns the
 * PMIC voltage data so that hardware AVS can adjust it around the programmed
 * level while the CPUs are online and so that a power-collapse entry restores
 * the correct level.  A child node exposes the AVS minimum/maximum limit
 * regulator that CPR4 drives.
 */

#include <linux/delay.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/spmi.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <soc/qcom/msm8953-spm.h>

#define SPM_REGULATOR_DRIVER_NAME	"qcom,spm-regulator"

struct voltage_range {
	int min_uv;
	int set_point_min_uv;
	int max_uv;
	int step_uv;
};

enum qpnp_regulator_uniq_type {
	QPNP_TYPE_HF,
	QPNP_TYPE_FTS2,
	QPNP_TYPE_FTS2p5,
	QPNP_TYPE_FTS426,
	QPNP_TYPE_ULT_HF,
	QPNP_TYPE_HFS430,
};

#define QPNP_FTS2_TYPE		0x1C
#define QPNP_HF_TYPE		0x03
#define QPNP_ULT_HF_TYPE	0x22

#define QPNP_FTS2_SUBTYPE	0x08
#define QPNP_HF_SUBTYPE		0x08
#define QPNP_FTS2p5_SUBTYPE	0x09
#define QPNP_FTS426_SUBTYPE	0x0A
#define QPNP_HFS430_SUBTYPE	0x0A
#define QPNP_ULT_HF_SUBTYPE	0x0D

enum qpnp_logical_mode {
	QPNP_LOGICAL_MODE_AUTO,
	QPNP_LOGICAL_MODE_PWM,
};

static const struct voltage_range fts2_range0 = {0, 350000, 1275000, 5000};
static const struct voltage_range fts2_range1 = {0, 700000, 2040000, 10000};
static const struct voltage_range fts2p5_range0 = {80000, 350000, 1355000, 5000};
static const struct voltage_range fts2p5_range1 = {160000, 700000, 2200000, 10000};
static const struct voltage_range fts426_range = {0, 320000, 1352000, 4000};
static const struct voltage_range hfs430_range = {0, 320000, 2040000, 8000};
static const struct voltage_range ult_hf_range0 = {375000, 375000, 1562500, 12500};
static const struct voltage_range ult_hf_range1 = {750000, 750000, 1525000, 25000};
static const struct voltage_range hf_range0 = {375000, 375000, 1562500, 12500};
static const struct voltage_range hf_range1 = {1550000, 1550000, 3125000, 25000};

#define QPNP_SMPS_REG_TYPE		0x04
#define QPNP_SMPS_REG_SUBTYPE		0x05
#define QPNP_SMPS_REG_VOLTAGE_RANGE	0x40
#define QPNP_SMPS_REG_VOLTAGE_SETPOINT	0x41
#define QPNP_SMPS_REG_MODE		0x45
#define QPNP_SMPS_REG_STEP_CTRL		0x61
#define QPNP_SMPS_REG_UL_LL_CTRL	0x68

#define QPNP_FTS426_HFS430_REG_VOLTAGE_LB	0x40
#define QPNP_FTS426_HFS430_REG_VOLTAGE_VALID_LB	0x42

#define QPNP_HF_REG_VOLTAGE_ULS		0x69
#define QPNP_HF_REG_VOLTAGE_LLS		0x6B
#define QPNP_FTS_REG_VOLTAGE_ULS_VALID	0x6A
#define QPNP_FTS_REG_VOLTAGE_LLS_VALID	0x6C
#define QPNP_FTS426_HFS430_REG_VOLTAGE_ULS_LB	0x68

#define QPNP_COMMON_UL_EN_MASK		0x80
#define QPNP_COMMON_LL_EN_MASK		0x40

#define QPNP_SMPS_MODE_PWM		0x80
#define QPNP_SMPS_MODE_AUTO		0x40
#define QPNP_FTS426_HFS430_MODE_PWM	0x07
#define QPNP_FTS426_HFS430_MODE_AUTO	0x06

/*
 * regmap compares the absolute register address against max_register, and
 * every access here is spmi_base_addr (up to 0xffff) plus a register offset
 * that reaches 0x6c. A window of 0x6c therefore rejected the type register
 * read at 0x2004 outright.
 */
static const struct regmap_config spmi_regulator_regmap_config = {
	.max_register = 0xffff,
	/* Preserve the peripheral base in absolute addresses such as 0x2004. */
	.reg_bits = 16,
	.val_bits = 8,
};

#define QPNP_SMPS_STEP_CTRL_STEP_MASK	0x18
#define QPNP_SMPS_STEP_CTRL_STEP_SHIFT	3
#define QPNP_SMPS_STEP_CTRL_DELAY_MASK	0x07
#define QPNP_SMPS_STEP_CTRL_DELAY_SHIFT	0
#define QPNP_FTS426_HFS430_STEP_CTRL_DELAY_MASK	0x03
#define QPNP_FTS426_HFS430_STEP_CTRL_DELAY_SHIFT	0

#define QPNP_SMPS_CLOCK_RATE		19200
#define QPNP_FTS426_CLOCK_RATE		4800
#define QPNP_HFS430_CLOCK_RATE		1600

#define QPNP_FTS2_MODE_CHANGE_DELAY	50
#define QPNP_SPMI_WRITE_MIN_DELAY	8

#define QPNP_FTS2_STEP_DELAY		8
#define QPNP_HF_STEP_DELAY		20
#define QPNP_FTS426_HFS430_STEP_DELAY	2

#define SPM_REGULATOR_MAX_STEP_UV	10000000

#define QPNP_FTS2_STEP_MARGIN_NUM		4
#define QPNP_FTS2_STEP_MARGIN_DEN		5
#define QPNP_FTS426_HFS430_STEP_MARGIN_NUM	10
#define QPNP_FTS426_HFS430_STEP_MARGIN_DEN	11

#define FTS2P5_SETTLING_DELAY_US	70

#define ULT_SMPS_RANGE_SPLIT		0x60

struct qcom_spm_reg {
	struct device			*dev;
	struct regmap			*regmap;
	const struct voltage_range	*range;
	struct msm8953_spm		*spm;
	enum qpnp_regulator_uniq_type	regulator_type;
	enum qpnp_logical_mode		init_mode;
	enum qpnp_logical_mode		mode;
	u16				spmi_base_addr;
	u8				type_reg[2];
	int				step_rate;
	u32				max_step_uv;
	unsigned int			vlevel;
	unsigned int			last_set_vlevel;
	int				uv;
	int				last_set_uv;
	bool				bypass_spm;
	bool				online;
	struct regulator_desc		rdesc;
	struct regulator_dev		*rdev;
	struct regulator_desc		avs_rdesc;
	struct regulator_dev		*avs_rdev;
	int				avs_min_uv;
	int				avs_max_uv;
};

/* Reads are only taken from the SPM when the AVS limit child exists. */
static bool qcom_spm_reg_using_avs(struct qcom_spm_reg *reg)
{
	return reg->avs_rdev && !reg->bypass_spm;
}

static int qcom_spm_reg_uv_to_vlevel(struct qcom_spm_reg *reg, int uv)
{
	int vlevel;

	if (reg->regulator_type == QPNP_TYPE_FTS426 ||
	    reg->regulator_type == QPNP_TYPE_HFS430)
		return roundup(uv, reg->range->step_uv) / 1000;

	vlevel = DIV_ROUND_UP(uv - reg->range->min_uv, reg->range->step_uv);

	if (reg->regulator_type == QPNP_TYPE_ULT_HF &&
	    reg->range == &ult_hf_range1) {
		vlevel &= 0x1F;
		vlevel |= ULT_SMPS_RANGE_SPLIT;
	}

	return vlevel;
}

static int qcom_spm_reg_vlevel_to_uv(struct qcom_spm_reg *reg, int vlevel)
{
	if (reg->regulator_type == QPNP_TYPE_FTS426 ||
	    reg->regulator_type == QPNP_TYPE_HFS430)
		return vlevel * 1000;

	if (reg->regulator_type == QPNP_TYPE_ULT_HF &&
	    reg->range == &ult_hf_range1)
		vlevel &= ~ULT_SMPS_RANGE_SPLIT;

	return vlevel * reg->range->step_uv + reg->range->min_uv;
}

static unsigned int qcom_spm_reg_vlevel_to_selector(struct qcom_spm_reg *reg,
						    unsigned int vlevel)
{
	if (reg->regulator_type == QPNP_TYPE_ULT_HF &&
	    reg->range == &ult_hf_range1)
		vlevel &= ~ULT_SMPS_RANGE_SPLIT;

	if (reg->regulator_type == QPNP_TYPE_HFS430)
		vlevel = qcom_spm_reg_vlevel_to_uv(reg, vlevel) /
						     reg->range->step_uv;

	return vlevel - (reg->range->set_point_min_uv - reg->range->min_uv) /
							   reg->range->step_uv;
}

static int qpnp_smps_read_voltage(struct qcom_spm_reg *reg)
{
	u8 val[2] = {0};
	int rc;

	if (reg->regulator_type == QPNP_TYPE_FTS426 ||
	    reg->regulator_type == QPNP_TYPE_HFS430) {
		rc = regmap_bulk_read(reg->regmap,
				      reg->spmi_base_addr + QPNP_FTS426_HFS430_REG_VOLTAGE_VALID_LB,
				      val, 2);
		if (rc)
			return rc;

		reg->last_set_vlevel = ((unsigned int)val[1] << 8) | val[0];
	} else {
		rc = regmap_bulk_read(reg->regmap,
				      reg->spmi_base_addr + QPNP_SMPS_REG_VOLTAGE_SETPOINT,
				      val, 1);
		if (rc)
			return rc;

		reg->last_set_vlevel = val[0];
	}

	reg->last_set_uv = qcom_spm_reg_vlevel_to_uv(reg, reg->last_set_vlevel);

	return 0;
}

static int qpnp_smps_write_voltage(struct qcom_spm_reg *reg,
				   unsigned int vlevel)
{
	u8 val[2];

	val[0] = vlevel & 0xFF;
	val[1] = (vlevel >> 8) & 0xFF;

	if (reg->regulator_type == QPNP_TYPE_FTS426 ||
	    reg->regulator_type == QPNP_TYPE_HFS430)
		return regmap_bulk_write(reg->regmap,
			reg->spmi_base_addr + QPNP_FTS426_HFS430_REG_VOLTAGE_LB,
			val, 2);

	return regmap_write(reg->regmap,
			reg->spmi_base_addr + QPNP_SMPS_REG_VOLTAGE_SETPOINT,
			val[0]);
}

static enum qpnp_logical_mode qpnp_regval_to_mode(struct qcom_spm_reg *reg,
						  u8 regval)
{
	if (reg->regulator_type == QPNP_TYPE_FTS426 ||
	    reg->regulator_type == QPNP_TYPE_HFS430)
		return (regval == QPNP_FTS426_HFS430_MODE_PWM) ?
			QPNP_LOGICAL_MODE_PWM : QPNP_LOGICAL_MODE_AUTO;

	return (regval & QPNP_SMPS_MODE_PWM) ?
		QPNP_LOGICAL_MODE_PWM : QPNP_LOGICAL_MODE_AUTO;
}

static u8 qpnp_mode_to_regval(struct qcom_spm_reg *reg,
			      enum qpnp_logical_mode mode)
{
	if (reg->regulator_type == QPNP_TYPE_FTS426 ||
	    reg->regulator_type == QPNP_TYPE_HFS430)
		return (mode == QPNP_LOGICAL_MODE_PWM) ?
			QPNP_FTS426_HFS430_MODE_PWM :
			QPNP_FTS426_HFS430_MODE_AUTO;

	return (mode == QPNP_LOGICAL_MODE_PWM) ?
		QPNP_SMPS_MODE_PWM : QPNP_SMPS_MODE_AUTO;
}

static int qpnp_smps_set_mode(struct qcom_spm_reg *reg,
			      enum qpnp_logical_mode mode)
{
	return regmap_write(reg->regmap,
			reg->spmi_base_addr + QPNP_SMPS_REG_MODE,
			qpnp_mode_to_regval(reg, mode));
}

static int qcom_spm_reg_get_voltage(struct regulator_dev *rdev)
{
	struct qcom_spm_reg *reg = rdev_get_drvdata(rdev);
	int vlevel, rc;

	if (!qcom_spm_reg_using_avs(reg))
		return reg->uv;

	vlevel = msm8953_spm_get_vlevel(reg->spm);
	if (vlevel < 0) {
		rc = qpnp_smps_read_voltage(reg);
		if (rc)
			return rc;

		return reg->last_set_uv;
	}

	reg->last_set_vlevel = vlevel;
	reg->last_set_uv = qcom_spm_reg_vlevel_to_uv(reg, vlevel);

	return reg->last_set_uv;
}

static int qcom_spm_reg_write_voltage(struct qcom_spm_reg *reg, int uv)
{
	unsigned int vlevel = qcom_spm_reg_uv_to_vlevel(reg, uv);
	bool spm_failed = false;
	u32 slew_delay;
	int rc = 0;

	if (!reg->bypass_spm) {
		rc = msm8953_spm_set_vlevel(reg->spm, vlevel);
		if (rc)
			spm_failed = true;
	}

	if (unlikely(reg->bypass_spm || spm_failed)) {
		rc = qpnp_smps_write_voltage(reg, vlevel);
		if (rc)
			return rc;
	}

	if (uv > reg->last_set_uv) {
		slew_delay = DIV_ROUND_UP(uv - reg->last_set_uv,
					  reg->step_rate);
		if (reg->regulator_type == QPNP_TYPE_FTS2p5)
			slew_delay += FTS2P5_SETTLING_DELAY_US;
		udelay(slew_delay);
	} else if (reg->regulator_type == QPNP_TYPE_FTS2p5) {
		slew_delay = DIV_ROUND_UP(reg->last_set_uv - uv,
					  reg->step_rate) +
			     FTS2P5_SETTLING_DELAY_US;
		udelay(slew_delay);
	}

	reg->last_set_uv = uv;
	reg->last_set_vlevel = vlevel;

	return rc;
}

static int qcom_spm_reg_apply_voltage(struct regulator_dev *rdev)
{
	struct qcom_spm_reg *reg = rdev_get_drvdata(rdev);
	bool pwm_required;
	int rc, uv;

	rc = qcom_spm_reg_get_voltage(rdev);
	if (rc < 0)
		return rc;

	if (reg->vlevel == reg->last_set_vlevel)
		return 0;

	pwm_required = reg->regulator_type == QPNP_TYPE_FTS2 &&
		       reg->init_mode != QPNP_LOGICAL_MODE_PWM &&
		       reg->uv > reg->last_set_uv;
	if (pwm_required) {
		rc = qpnp_smps_set_mode(reg, QPNP_LOGICAL_MODE_PWM);
		if (rc)
			return rc;
	}

	do {
		if (reg->uv > reg->last_set_uv)
			uv = min(reg->uv, reg->last_set_uv +
					 (int)reg->max_step_uv);
		else
			uv = max(reg->uv, reg->last_set_uv -
					 (int)reg->max_step_uv);

		rc = qcom_spm_reg_write_voltage(reg, uv);
		if (rc)
			return rc;
	} while (reg->last_set_uv != reg->uv);

	if (pwm_required) {
		udelay(QPNP_FTS2_MODE_CHANGE_DELAY - QPNP_SPMI_WRITE_MIN_DELAY);
		rc = qpnp_smps_set_mode(reg, QPNP_LOGICAL_MODE_AUTO);
		if (rc)
			return rc;
	}

	return 0;
}

static int qcom_spm_reg_set_voltage(struct regulator_dev *rdev, int min_uv,
				    int max_uv, unsigned int *selector)
{
	struct qcom_spm_reg *reg = rdev_get_drvdata(rdev);
	const struct voltage_range *range = reg->range;
	unsigned int vlevel;
	int uv = min_uv;

	if (uv < range->set_point_min_uv && max_uv >= range->set_point_min_uv)
		uv = range->set_point_min_uv;

	if (uv < range->set_point_min_uv || uv > range->max_uv)
		return -EINVAL;

	vlevel = qcom_spm_reg_uv_to_vlevel(reg, uv);
	uv = qcom_spm_reg_vlevel_to_uv(reg, vlevel);

	if (uv > max_uv)
		return -EINVAL;

	*selector = qcom_spm_reg_vlevel_to_selector(reg, vlevel);
	reg->vlevel = vlevel;
	reg->uv = uv;

	if (!reg->online)
		return 0;

	return qcom_spm_reg_apply_voltage(rdev);
}

static int qcom_spm_reg_list_voltage(struct regulator_dev *rdev,
				     unsigned int selector)
{
	struct qcom_spm_reg *reg = rdev_get_drvdata(rdev);

	if (selector >= reg->rdesc.n_voltages)
		return 0;

	return selector * reg->range->step_uv + reg->range->set_point_min_uv;
}

static int qcom_spm_reg_enable(struct regulator_dev *rdev)
{
	struct qcom_spm_reg *reg = rdev_get_drvdata(rdev);
	int rc;

	rc = qcom_spm_reg_apply_voltage(rdev);
	if (!rc)
		reg->online = true;

	return rc;
}

static int qcom_spm_reg_disable(struct regulator_dev *rdev)
{
	struct qcom_spm_reg *reg = rdev_get_drvdata(rdev);

	reg->online = false;

	return 0;
}

static int qcom_spm_reg_is_enabled(struct regulator_dev *rdev)
{
	struct qcom_spm_reg *reg = rdev_get_drvdata(rdev);

	return reg->online;
}

static unsigned int qcom_spm_reg_get_mode(struct regulator_dev *rdev)
{
	struct qcom_spm_reg *reg = rdev_get_drvdata(rdev);

	return reg->mode == QPNP_LOGICAL_MODE_PWM ?
		REGULATOR_MODE_NORMAL : REGULATOR_MODE_IDLE;
}

static int qcom_spm_reg_set_mode(struct regulator_dev *rdev, unsigned int mode)
{
	struct qcom_spm_reg *reg = rdev_get_drvdata(rdev);

	/* NORMAL maps to PWM; IDLE returns to the configured init_mode. */
	reg->mode = (mode == REGULATOR_MODE_NORMAL) ? QPNP_LOGICAL_MODE_PWM :
						      reg->init_mode;

	return qpnp_smps_set_mode(reg, reg->mode);
}

static const struct regulator_ops qcom_spm_reg_ops = {
	.get_voltage	= qcom_spm_reg_get_voltage,
	.set_voltage	= qcom_spm_reg_set_voltage,
	.list_voltage	= qcom_spm_reg_list_voltage,
	.get_mode	= qcom_spm_reg_get_mode,
	.set_mode	= qcom_spm_reg_set_mode,
	.enable		= qcom_spm_reg_enable,
	.disable	= qcom_spm_reg_disable,
	.is_enabled	= qcom_spm_reg_is_enabled,
};

static int qcom_spm_reg_avs_set_voltage(struct regulator_dev *rdev, int min_uv,
					int max_uv, unsigned int *selector)
{
	struct qcom_spm_reg *reg = rdev_get_drvdata(rdev);
	const struct voltage_range *range = reg->range;
	unsigned int vlevel_min, vlevel_max;
	int uv, avs_min_uv, avs_max_uv, rc;

	uv = min_uv;
	if (uv < range->set_point_min_uv && max_uv >= range->set_point_min_uv)
		uv = range->set_point_min_uv;

	if (uv < range->set_point_min_uv || uv > range->max_uv)
		return -EINVAL;

	vlevel_min = qcom_spm_reg_uv_to_vlevel(reg, uv);
	avs_min_uv = qcom_spm_reg_vlevel_to_uv(reg, vlevel_min);

	if (avs_min_uv > max_uv)
		return -EINVAL;

	uv = max_uv;
	if (uv > range->max_uv && min_uv <= range->max_uv)
		uv = range->max_uv;

	if (uv < range->set_point_min_uv || uv > range->max_uv)
		return -EINVAL;

	vlevel_max = qcom_spm_reg_uv_to_vlevel(reg, uv);
	avs_max_uv = qcom_spm_reg_vlevel_to_uv(reg, vlevel_max);

	if (avs_max_uv < min_uv)
		return -EINVAL;

	if (likely(!reg->bypass_spm)) {
		rc = msm8953_spm_set_avs_limits(reg->spm, vlevel_min,
						vlevel_max);
		if (rc)
			return rc;
	}

	*selector = qcom_spm_reg_vlevel_to_selector(reg, vlevel_min);
	reg->avs_min_uv = avs_min_uv;
	reg->avs_max_uv = avs_max_uv;

	return 0;
}

static int qcom_spm_reg_avs_get_voltage(struct regulator_dev *rdev)
{
	struct qcom_spm_reg *reg = rdev_get_drvdata(rdev);

	return reg->avs_min_uv;
}

static int qcom_spm_reg_avs_enable(struct regulator_dev *rdev)
{
	struct qcom_spm_reg *reg = rdev_get_drvdata(rdev);
	int rc;

	if (likely(!reg->bypass_spm)) {
		rc = msm8953_spm_set_avs(reg->spm, true);
		if (rc)
			return rc;
	}

	return 0;
}

static int qcom_spm_reg_avs_disable(struct regulator_dev *rdev)
{
	struct qcom_spm_reg *reg = rdev_get_drvdata(rdev);
	int rc;

	if (likely(!reg->bypass_spm)) {
		rc = msm8953_spm_set_avs(reg->spm, false);
		if (rc)
			return rc;
	}

	return 0;
}

static int qcom_spm_reg_avs_is_enabled(struct regulator_dev *rdev)
{
	struct qcom_spm_reg *reg = rdev_get_drvdata(rdev);

	if (reg->bypass_spm)
		return 0;

	return msm8953_spm_avs_enabled(reg->spm);
}

static const struct regulator_ops qcom_spm_reg_avs_ops = {
	.get_voltage	= qcom_spm_reg_avs_get_voltage,
	.set_voltage	= qcom_spm_reg_avs_set_voltage,
	.list_voltage	= qcom_spm_reg_list_voltage,
	.enable		= qcom_spm_reg_avs_enable,
	.disable	= qcom_spm_reg_avs_disable,
	.is_enabled	= qcom_spm_reg_avs_is_enabled,
};

static int qpnp_smps_check_type(struct qcom_spm_reg *reg)
{
	u8 type[2];
	int rc;

	rc = regmap_bulk_read(reg->regmap,
			      reg->spmi_base_addr + QPNP_SMPS_REG_TYPE, type, 2);
	if (rc)
		return rc;
	memcpy(reg->type_reg, type, sizeof(type));

	if (type[0] == QPNP_FTS2_TYPE && type[1] == QPNP_FTS2_SUBTYPE)
		reg->regulator_type = QPNP_TYPE_FTS2;
	else if (type[0] == QPNP_FTS2_TYPE && type[1] == QPNP_FTS2p5_SUBTYPE)
		reg->regulator_type = QPNP_TYPE_FTS2p5;
	else if (type[0] == QPNP_FTS2_TYPE && type[1] == QPNP_FTS426_SUBTYPE)
		reg->regulator_type = QPNP_TYPE_FTS426;
	else if (type[0] == QPNP_HF_TYPE && type[1] == QPNP_HFS430_SUBTYPE)
		reg->regulator_type = QPNP_TYPE_HFS430;
	else if (type[0] == QPNP_ULT_HF_TYPE && type[1] == QPNP_ULT_HF_SUBTYPE)
		reg->regulator_type = QPNP_TYPE_ULT_HF;
	else if (type[0] == QPNP_HF_TYPE && type[1] == QPNP_HF_SUBTYPE)
		reg->regulator_type = QPNP_TYPE_HF;
	else
		return -ENODEV;

	return 0;
}

static int qpnp_smps_init_range(struct qcom_spm_reg *reg,
				const struct voltage_range *range0,
				const struct voltage_range *range1)
{
	unsigned int val;
	int rc;

	rc = regmap_read(reg->regmap,
			 reg->spmi_base_addr + QPNP_SMPS_REG_VOLTAGE_RANGE,
			 &val);
	if (rc)
		return rc;

	if (val == 0x00)
		reg->range = range0;
	else if (val == 0x01)
		reg->range = range1;
	else
		return -EINVAL;

	return 0;
}

static int qpnp_ult_hf_init_range(struct qcom_spm_reg *reg)
{
	unsigned int val;
	int rc;

	rc = regmap_read(reg->regmap,
			 reg->spmi_base_addr + QPNP_SMPS_REG_VOLTAGE_SETPOINT,
			 &val);
	if (rc)
		return rc;

	reg->range = (val < ULT_SMPS_RANGE_SPLIT) ? &ult_hf_range0 :
						    &ult_hf_range1;

	return 0;
}

static int qpnp_smps_init_voltage(struct qcom_spm_reg *reg)
{
	int rc;

	rc = qpnp_smps_read_voltage(reg);
	if (rc)
		return rc;

	reg->vlevel = reg->last_set_vlevel;
	reg->uv = reg->last_set_uv;

	/*
	 * No SPM write here: like the donor, the SPM's PMIC data is left as
	 * the bootloader programmed it, and the first real request re-reads
	 * and compares against it.
	 */

	return 0;
}

static int qpnp_smps_init_mode(struct qcom_spm_reg *reg)
{
	const char *mode_name;
	unsigned int val;
	int rc;

	rc = of_property_read_string(reg->dev->of_node, "qcom,mode",
				     &mode_name);
	if (!rc) {
		if (!strcmp("pwm", mode_name))
			reg->init_mode = QPNP_LOGICAL_MODE_PWM;
		else if (!strcmp("auto", mode_name) &&
			 reg->regulator_type != QPNP_TYPE_ULT_HF)
			reg->init_mode = QPNP_LOGICAL_MODE_AUTO;
		else
			return -EINVAL;

		rc = qpnp_smps_set_mode(reg, reg->init_mode);
		if (rc)
			return rc;
	} else {
		rc = regmap_read(reg->regmap,
				 reg->spmi_base_addr + QPNP_SMPS_REG_MODE,
				 &val);
		if (rc)
			return rc;

		reg->init_mode = qpnp_regval_to_mode(reg, val);
	}

	reg->mode = reg->init_mode;

	return 0;
}

static int qpnp_smps_init_step_rate(struct qcom_spm_reg *reg)
{
	unsigned int val;
	int step = 0, delay;
	u8 regval;
	int rc;

	rc = regmap_read(reg->regmap,
			 reg->spmi_base_addr + QPNP_SMPS_REG_STEP_CTRL, &val);
	if (rc)
		return rc;
	regval = val;

	if (reg->regulator_type != QPNP_TYPE_ULT_HF &&
	    reg->regulator_type != QPNP_TYPE_FTS426 &&
	    reg->regulator_type != QPNP_TYPE_HFS430)
		step = (regval & QPNP_SMPS_STEP_CTRL_STEP_MASK) >>
		       QPNP_SMPS_STEP_CTRL_STEP_SHIFT;

	if (reg->regulator_type == QPNP_TYPE_FTS426 ||
	    reg->regulator_type == QPNP_TYPE_HFS430) {
		delay = (regval & QPNP_FTS426_HFS430_STEP_CTRL_DELAY_MASK) >>
			QPNP_FTS426_HFS430_STEP_CTRL_DELAY_SHIFT;

		reg->step_rate = (reg->regulator_type == QPNP_TYPE_FTS426 ?
				  QPNP_FTS426_CLOCK_RATE :
				  QPNP_HFS430_CLOCK_RATE) *
				 reg->range->step_uv;
	} else {
		delay = (regval & QPNP_SMPS_STEP_CTRL_DELAY_MASK) >>
			QPNP_SMPS_STEP_CTRL_DELAY_SHIFT;

		reg->step_rate = QPNP_SMPS_CLOCK_RATE * reg->range->step_uv *
				 (1 << step);
	}

	if (reg->regulator_type == QPNP_TYPE_ULT_HF ||
	    reg->regulator_type == QPNP_TYPE_HF)
		reg->step_rate /= 1000 * (QPNP_HF_STEP_DELAY << delay);
	else if (reg->regulator_type == QPNP_TYPE_FTS426 ||
		 reg->regulator_type == QPNP_TYPE_HFS430)
		reg->step_rate /= 1000 * (QPNP_FTS426_HFS430_STEP_DELAY << delay);
	else
		reg->step_rate /= 1000 * (QPNP_FTS2_STEP_DELAY << delay);

	if (reg->regulator_type == QPNP_TYPE_FTS426 ||
	    reg->regulator_type == QPNP_TYPE_HFS430)
		reg->step_rate = reg->step_rate *
			QPNP_FTS426_HFS430_STEP_MARGIN_NUM /
			QPNP_FTS426_HFS430_STEP_MARGIN_DEN;
	else
		reg->step_rate = reg->step_rate * QPNP_FTS2_STEP_MARGIN_NUM /
				 QPNP_FTS2_STEP_MARGIN_DEN;

	reg->step_rate = max(reg->step_rate, 1);

	return 0;
}

static int qpnp_smps_check_constraints(struct qcom_spm_reg *reg,
				       struct regulator_init_data *init_data)
{
	int limit_min_uv = 0, limit_max_uv = INT_MAX;
	u16 ul_reg = QPNP_FTS_REG_VOLTAGE_ULS_VALID;
	u16 ll_reg = QPNP_FTS_REG_VOLTAGE_LLS_VALID;
	u8 val[2];
	int rc = 0;

	switch (reg->regulator_type) {
	case QPNP_TYPE_HF:
		ul_reg = QPNP_HF_REG_VOLTAGE_ULS;
		ll_reg = QPNP_HF_REG_VOLTAGE_LLS;
		fallthrough;
	case QPNP_TYPE_FTS2:
	case QPNP_TYPE_FTS2p5:
		rc = regmap_bulk_read(reg->regmap,
				      reg->spmi_base_addr + QPNP_SMPS_REG_UL_LL_CTRL, val, 1);
		if (rc)
			return rc;

		if (val[0] & QPNP_COMMON_UL_EN_MASK) {
			rc = regmap_bulk_read(reg->regmap,
					      reg->spmi_base_addr + ul_reg, &val[1], 1);
			if (rc)
				return rc;

			limit_max_uv = qcom_spm_reg_vlevel_to_uv(reg, val[1]);
		}

		if (val[0] & QPNP_COMMON_LL_EN_MASK) {
			rc = regmap_bulk_read(reg->regmap,
					      reg->spmi_base_addr + ll_reg, &val[1], 1);
			if (rc)
				return rc;

			limit_min_uv = qcom_spm_reg_vlevel_to_uv(reg, val[1]);
		}
		break;
	case QPNP_TYPE_FTS426:
	case QPNP_TYPE_HFS430:
		rc = regmap_bulk_read(reg->regmap,
				      reg->spmi_base_addr + QPNP_FTS426_HFS430_REG_VOLTAGE_ULS_LB,
				      val, 2);
		if (rc)
			return rc;

		limit_max_uv = qcom_spm_reg_vlevel_to_uv(reg,
							 ((unsigned int)val[1] << 8) | val[0]);
		break;
	case QPNP_TYPE_ULT_HF:
		break;
	}

	if (init_data->constraints.min_uV < limit_min_uv ||
	    init_data->constraints.max_uV > limit_max_uv)
		return -EINVAL;

	return rc;
}

/* Register the child node (if any) as the AVS minimum/maximum regulator. */
static int qcom_spm_reg_avs_register(struct qcom_spm_reg *reg,
				     struct device_node *node)
{
	struct device_node *avs_node;
	struct regulator_config config = {};
	struct regulator_init_data *init_data;

	avs_node = of_get_next_available_child(node, NULL);
	if (!avs_node)
		return 0;

	init_data = of_get_regulator_init_data(reg->dev, avs_node,
					       &reg->avs_rdesc);
	if (!init_data)
		return -ENOMEM;

	init_data->constraints.input_uV = init_data->constraints.max_uV;
	init_data->constraints.valid_ops_mask |= REGULATOR_CHANGE_STATUS |
						 REGULATOR_CHANGE_VOLTAGE;

	if (!init_data->constraints.name)
		return -EINVAL;

	reg->avs_rdesc.name = init_data->constraints.name;
	reg->avs_rdesc.type = REGULATOR_VOLTAGE;
	reg->avs_rdesc.owner = THIS_MODULE;
	reg->avs_rdesc.ops = &qcom_spm_reg_avs_ops;
	reg->avs_rdesc.n_voltages = (reg->range->max_uv -
			reg->range->set_point_min_uv) / reg->range->step_uv + 1;

	config.dev = reg->dev;
	config.init_data = init_data;
	config.driver_data = reg;
	config.of_node = avs_node;
	reg->avs_rdev = devm_regulator_register(reg->dev, &reg->avs_rdesc,
						&config);
	if (IS_ERR(reg->avs_rdev))
		return dev_err_probe(reg->dev, PTR_ERR(reg->avs_rdev),
				     "failed to register AVS limit regulator\n");

	return 0;
}

static int qcom_spm_reg_probe(struct platform_device *pdev)
{
	struct regulator_config config = {};
	struct regulator_init_data *init_data;
	struct qcom_spm_reg *reg;
	u32 base;
	int rc;

	reg = devm_kzalloc(&pdev->dev, sizeof(*reg), GFP_KERNEL);
	if (!reg)
		return -ENOMEM;
	reg->dev = &pdev->dev;
	reg->bypass_spm = of_property_read_bool(pdev->dev.of_node,
						"qcom,bypass-spm");

	/*
	 * The SPMI core hands out no regmap of its own, so this driver builds
	 * one over its PMIC parent, the way every regulator that is a child of
	 * an SPMI PMIC does. Asking the parent for an existing regmap
	 * (dev_get_regmap) always failed with -EINVAL, so the rail never came
	 * up and everything downstream of it deferred forever.
	 */
	if (!pdev->dev.parent)
		return dev_err_probe(&pdev->dev, -EINVAL, "no SPMI parent\n");

	reg->regmap =
		devm_regmap_init_spmi_ext(to_spmi_device(pdev->dev.parent),
					  &spmi_regulator_regmap_config);
	if (IS_ERR(reg->regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(reg->regmap),
				     "SPMI regmap creation failed\n");

	if (!reg->bypass_spm) {
		reg->spm = devm_msm8953_spm_get(&pdev->dev);
		if (IS_ERR(reg->spm))
			return dev_err_probe(&pdev->dev, PTR_ERR(reg->spm),
					     "SAW4 voltage controller lookup failed\n");
	}

	rc = of_property_read_u32(pdev->dev.of_node, "reg", &base);
	if (rc)
		return dev_err_probe(&pdev->dev, rc, "bad reg property\n");
	if (base > 0xffff)
		return -EINVAL;
	reg->spmi_base_addr = base;

	rc = qpnp_smps_check_type(reg);
	if (rc)
		return dev_err_probe(&pdev->dev, rc,
				     "unsupported SMPS type %02x/%02x at 0x%x\n",
				     reg->type_reg[0], reg->type_reg[1],
				     reg->spmi_base_addr);

	switch (reg->regulator_type) {
	case QPNP_TYPE_FTS2:
		rc = qpnp_smps_init_range(reg, &fts2_range0, &fts2_range1);
		break;
	case QPNP_TYPE_FTS2p5:
		rc = qpnp_smps_init_range(reg, &fts2p5_range0, &fts2p5_range1);
		break;
	case QPNP_TYPE_FTS426:
		reg->range = &fts426_range;
		break;
	case QPNP_TYPE_HFS430:
		reg->range = &hfs430_range;
		break;
	case QPNP_TYPE_HF:
		rc = qpnp_smps_init_range(reg, &hf_range0, &hf_range1);
		break;
	case QPNP_TYPE_ULT_HF:
		rc = qpnp_ult_hf_init_range(reg);
		break;
	}
	if (rc)
		return dev_err_probe(&pdev->dev, rc, "invalid voltage range\n");

	rc = qpnp_smps_init_voltage(reg);
	if (rc)
		return dev_err_probe(&pdev->dev, rc, "voltage init failed\n");

	rc = qpnp_smps_init_mode(reg);
	if (rc)
		return dev_err_probe(&pdev->dev, rc, "mode init failed\n");

	rc = qpnp_smps_init_step_rate(reg);
	if (rc)
		return dev_err_probe(&pdev->dev, rc, "step rate init failed\n");

	init_data = of_get_regulator_init_data(&pdev->dev, pdev->dev.of_node,
					       &reg->rdesc);
	if (!init_data)
		return -ENOMEM;
	init_data->constraints.input_uV = init_data->constraints.max_uV;
	init_data->constraints.valid_ops_mask |= REGULATOR_CHANGE_STATUS |
						 REGULATOR_CHANGE_VOLTAGE |
						 REGULATOR_CHANGE_MODE;
	init_data->constraints.valid_modes_mask = REGULATOR_MODE_NORMAL |
						  REGULATOR_MODE_IDLE;

	if (!init_data->constraints.name)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing regulator name\n");

	rc = qpnp_smps_check_constraints(reg, init_data);
	if (rc)
		return dev_err_probe(&pdev->dev, rc, "constraints rejected\n");

	reg->rdesc.name = init_data->constraints.name;
	reg->rdesc.type = REGULATOR_VOLTAGE;
	reg->rdesc.owner = THIS_MODULE;
	reg->rdesc.ops = &qcom_spm_reg_ops;
	reg->rdesc.n_voltages = (reg->range->max_uv -
			reg->range->set_point_min_uv) / reg->range->step_uv + 1;

	reg->max_step_uv = SPM_REGULATOR_MAX_STEP_UV;
	of_property_read_u32(pdev->dev.of_node, "qcom,max-voltage-step",
			     &reg->max_step_uv);
	reg->max_step_uv = min_t(u32, reg->max_step_uv,
				 SPM_REGULATOR_MAX_STEP_UV);
	reg->max_step_uv = rounddown(reg->max_step_uv, reg->range->step_uv);
	/* A zero step would loop forever in the ramped voltage request. */
	reg->max_step_uv = max_t(u32, reg->max_step_uv, reg->range->step_uv);

	config.dev = &pdev->dev;
	config.init_data = init_data;
	config.driver_data = reg;
	config.of_node = pdev->dev.of_node;
	reg->rdev = devm_regulator_register(&pdev->dev, &reg->rdesc, &config);
	if (IS_ERR(reg->rdev))
		return dev_err_probe(&pdev->dev, PTR_ERR(reg->rdev),
				     "failed to register regulator\n");

	rc = qcom_spm_reg_avs_register(reg, pdev->dev.of_node);
	if (rc)
		return dev_err_probe(&pdev->dev, rc,
				     "AVS limit regulator failed\n");

	platform_set_drvdata(pdev, reg);

	dev_info(&pdev->dev,
		 "name=%s, type=%d, voltage=%d uV, mode=%s, step rate=%d uV/us\n",
		 reg->rdesc.name, reg->regulator_type, reg->uv,
		 reg->init_mode == QPNP_LOGICAL_MODE_PWM ? "PWM" : "AUTO",
		 reg->step_rate);

	return 0;
}

static const struct of_device_id qcom_spm_reg_match[] = {
	{ .compatible = SPM_REGULATOR_DRIVER_NAME },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_spm_reg_match);

static struct platform_driver qcom_spm_reg_driver = {
	.probe = qcom_spm_reg_probe,
	.driver = {
		.name = "qcom-msm8953-spm-regulator",
		.of_match_table = qcom_spm_reg_match,
	},
};
module_platform_driver(qcom_spm_reg_driver);

MODULE_DESCRIPTION("Qualcomm MSM8953 SPM-backed APC rail regulator");
MODULE_LICENSE("GPL");
