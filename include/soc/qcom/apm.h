/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2019-2020, The Linux Foundation. All rights reserved. */
#ifndef __SOC_QCOM_APM_H__
#define __SOC_QCOM_APM_H__

struct device;
struct msm_apm_ctrl_dev;

enum msm_apm_supply {
	MSM_APM_SUPPLY_APCC,
	MSM_APM_SUPPLY_MX,
};

/* Probe-only acquisition; the handle is managed for the consumer lifetime. */
struct msm_apm_ctrl_dev *msm_apm_ctrl_dev_get(struct device *dev);
int msm_apm_get_supply(struct msm_apm_ctrl_dev *ctrl);
int msm_apm_set_supply(struct msm_apm_ctrl_dev *ctrl,
		       enum msm_apm_supply supply);

#endif
