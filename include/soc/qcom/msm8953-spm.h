/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SOC_QCOM_MSM8953_SPM_H__
#define __SOC_QCOM_MSM8953_SPM_H__

#include <linux/types.h>

struct device;
struct msm8953_spm;

/* Probe-only acquisition through qcom,saw; managed for the consumer lifetime. */
struct msm8953_spm *devm_msm8953_spm_get(struct device *dev);

/* Voltage levels are encoded PMIC data, not microvolts or CPR corners. */
int msm8953_spm_get_vlevel(struct msm8953_spm *spm);
/* Process context only: routes the request to an online CPU in the domain. */
int msm8953_spm_set_vlevel(struct msm8953_spm *spm, unsigned int level);
int msm8953_spm_set_avs_limits(struct msm8953_spm *spm,
			       unsigned int min_level, unsigned int max_level);
int msm8953_spm_set_avs(struct msm8953_spm *spm, bool enable);
int msm8953_spm_avs_enabled(struct msm8953_spm *spm);

#endif
