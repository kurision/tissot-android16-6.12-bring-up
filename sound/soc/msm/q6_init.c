// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017, 2019-2020 The Linux Foundation. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <dsp/q6_init.h>

static bool msm_dsplib_off;
module_param(msm_dsplib_off, bool, 0644);
MODULE_PARM_DESC(msm_dsplib_off, "do not initialise the DSP library");

static int __init audio_q6_init(void)
{
	if (msm_dsplib_off) {
		pr_info("DIAG: the DSP library is off by the command line\n");
		return 0;
	}

	adsp_err_init();
	audio_cal_init();
	rtac_init();
	adm_init();
	afe_init();
	spk_params_init();
	q6asm_init();
	q6lsm_init();
	voice_init();
	core_init();
	msm_audio_ion_init();
	/* This board has no slimbus codec: the WCD is not fitted and the
	 * audio is the analog CDC, so there is no slimbus slave to set up.
	 */
	avtimer_init();
	msm_mdf_init();
	voice_mhi_init();
	digital_cdc_rsc_mgr_init();
	return 0;
}

static void __exit audio_q6_exit(void)
{
	digital_cdc_rsc_mgr_exit();
	msm_mdf_exit();
	avtimer_exit();

	msm_audio_ion_exit();
	core_exit();
	voice_exit();
	q6lsm_exit();
	q6asm_exit();
	afe_exit();
	spk_params_exit();
	adm_exit();
	rtac_exit();
	audio_cal_exit();
	adsp_err_exit();
	voice_mhi_exit();
}

module_init(audio_q6_init);
module_exit(audio_q6_exit);
MODULE_DESCRIPTION("Q6 module");
MODULE_LICENSE("GPL v2");
