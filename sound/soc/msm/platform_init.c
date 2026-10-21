// SPDX-License-Identifier: GPL-2.0-only
/*
 * The single entry point of platform_dlkm. None of the PCM, DAI and routing
 * files registers its own driver: in CAF each exports an init that this
 * file calls. The port had left this file out, so in the built-in audio
 * kernels these drivers were never registered at all. From CAF
 * techpack/audio asoc/platform_init.c, trimmed to the drivers this port
 * builds, in the donor's order.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include "platform_init.h"

static int __init audio_platform_init(void)
{
	msm_fe_dai_init();
	msm_dai_q6_init();
	msm_dai_stub_init();
	msm_pcm_afe_init();
	msm_pcm_hostless_init();
	msm_pcm_loopback_init();
	msm_pcm_noirq_init();
	msm_pcm_dsp_init();
	msm_soc_routing_platform_init();
	msm_pcm_voice_init();
	return 0;
}

static void __exit audio_platform_exit(void)
{
	msm_pcm_voice_exit();
	msm_soc_routing_platform_exit();
	msm_pcm_dsp_exit();
	msm_pcm_noirq_exit();
	msm_pcm_loopback_exit();
	msm_pcm_hostless_exit();
	msm_pcm_afe_exit();
	msm_dai_stub_exit();
	msm_dai_q6_exit();
	msm_fe_dai_exit();
}

module_init(audio_platform_init);
module_exit(audio_platform_exit);
MODULE_DESCRIPTION("Audio Platform driver");
MODULE_LICENSE("GPL v2");
