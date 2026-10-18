/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SOC_QCOM_MSM8953_SPM_H__
#define __SOC_QCOM_MSM8953_SPM_H__

#include <linux/types.h>

struct device;
struct msm8953_spm;

/* Probe-only acquisition through qcom,saw; managed for the consumer lifetime. */
struct msm8953_spm *devm_msm8953_spm_get(struct device *dev);

/*
 * 0 once the shared SAW4 controller has probed, -EPROBE_DEFER before that.
 * Equivalent to the donor's msm_spm_probe_done() for the single MSM8953
 * voltage domain; it is a readiness query, not a success stub.
 */
int msm8953_spm_probe_done(void);

/* Voltage levels are encoded PMIC data, not microvolts or CPR corners. */
int msm8953_spm_get_vlevel(struct msm8953_spm *spm);
/* Process context only: routes the request to an online CPU in the domain. */
int msm8953_spm_set_vlevel(struct msm8953_spm *spm, unsigned int level);
int msm8953_spm_set_avs_limits(struct msm8953_spm *spm,
			       unsigned int min_level, unsigned int max_level);
int msm8953_spm_set_avs(struct msm8953_spm *spm, bool enable);
int msm8953_spm_avs_enabled(struct msm8953_spm *spm);

#endif
