/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The entry points of the platform drivers in platform_dlkm. Each file's
 * init registers its own platform driver; platform_init.c calls them in
 * the donor's order. From CAF techpack/audio asoc/platform_init.h, trimmed
 * to the drivers this port builds.
 */
#ifndef __PLATFORM_INIT_H__
#define __PLATFORM_INIT_H__

int msm_fe_dai_init(void);
int msm_dai_q6_init(void);
int msm_dai_stub_init(void);
int msm_pcm_afe_init(void);
int msm_pcm_hostless_init(void);
int msm_pcm_loopback_init(void);
int msm_pcm_noirq_init(void);
int msm_pcm_dsp_init(void);
int msm_soc_routing_platform_init(void);
int msm_pcm_voice_init(void);

void msm_fe_dai_exit(void);
void msm_dai_q6_exit(void);
void msm_dai_stub_exit(void);
void msm_pcm_afe_exit(void);
void msm_pcm_hostless_exit(void);
void msm_pcm_loopback_exit(void);
void msm_pcm_noirq_exit(void);
void msm_pcm_dsp_exit(void);
void msm_soc_routing_platform_exit(void);
void msm_pcm_voice_exit(void);

#endif /* __PLATFORM_INIT_H__ */
