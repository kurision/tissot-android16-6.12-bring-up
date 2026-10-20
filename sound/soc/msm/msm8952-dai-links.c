// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2015-2018, 2020-2021, The Linux Foundation. All rights reserved.
 */

#include <linux/of.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include "msm8952-slimbus.h"
#include "msm-pcm-routing-v2.h"
#include "codecs/wcd9335.h"

#define DEV_NAME_STR_LEN            32

/* dummy definition of below deprecated FE DAI's*/
enum {
	MSM_FRONTEND_DAI_CS_VOICE = 39,
	MSM_FRONTEND_DAI_VOICE2,
	MSM_FRONTEND_DAI_VOLTE,
	MSM_FRONTEND_DAI_VOWLAN,
};

enum TASHA_LITE_DEVICE {
	MSM8952_TASHA_LITE = 0,
	MSM8953_TASHA_LITE,
	NUM_OF_TASHA_LITE_DEVICE,
};

static struct snd_soc_card snd_soc_card_msm_card;

static struct snd_soc_ops msm8952_quat_mi2s_be_ops = {
	.startup = msm_quat_mi2s_snd_startup,
	.hw_params = msm_mi2s_snd_hw_params,
	.shutdown = msm_quat_mi2s_snd_shutdown,
};

static struct snd_soc_ops msm8952_quin_mi2s_be_ops = {
	.startup = msm_quin_mi2s_snd_startup,
	.hw_params = msm_mi2s_snd_hw_params,
	.shutdown = msm_quin_mi2s_snd_shutdown,
};

static struct snd_soc_ops msm_pri_auxpcm_be_ops = {
	.startup = msm_prim_auxpcm_startup,
	.shutdown = msm_prim_auxpcm_shutdown,
};

static struct snd_soc_ops msm8952_slimbus_be_ops = {
	.hw_params = msm_snd_hw_params,
};


static struct snd_soc_ops msm8952_cpe_ops = {
	.hw_params = msm_snd_cpe_hw_params,
};

static struct snd_soc_ops msm8952_slimbus_2_be_ops = {
	.hw_params = msm8952_slimbus_2_hw_params,
};

static struct snd_soc_ops msm_tdm_be_ops = {
	.startup = msm_tdm_startup,
	.hw_params = msm_tdm_snd_hw_params,
	.shutdown = msm_tdm_shutdown,
};



static struct snd_soc_dai_link msm8952_common_fe_dai[] = {
	/* FrontEnd DAI Links */
	{/* hw:x,0 */
		.name = "MSM8X16 Media1",
		.stream_name = "MultiMedia1",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia1" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-dsp.0"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		/* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA1
	},
	{/* hw:x,1 */
		.name = "MSM8X16 Media2",
		.stream_name = "MultiMedia2",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia2" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-dsp.0"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA2,
	},
	{/* hw:x,2 */
		.name = "Circuit-Switch Voice",
		.stream_name = "CS-Voice",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "VoiceMMode1" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-voice"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_CS_VOICE,
	},
	{/* hw:x,3 */
		.name = "MSM VoIP",
		.stream_name = "VoIP",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "VoIP" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-voip-dsp"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_VOIP,
	},
	{/* hw:x,4 */
		.name = "MSM8X16 ULL",
		.stream_name = "ULL",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia3" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-dsp.2"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA3,
	},
	/* Hostless PCM purpose */
	{/* hw:x,5 */
		.name = "SLIMBUS_0 Hostless",
		.stream_name = "SLIMBUS_0 Hostless",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "SLIMBUS0_HOSTLESS" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-hostless"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		 /* This dai link has MI2S support */
	},
	{/* hw:x,6 */
		.name = "INT_FM Hostless",
		.stream_name = "INT_FM Hostless",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "INT_FM_HOSTLESS" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-hostless"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dai link has playback support */
	},
	{/* hw:x,7 */
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-dev.241" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-afe"),
		},
		.num_platforms = 1,

		.name = "MSM AFE-PCM RX",
		.stream_name = "AFE-PROXY RX",
		.ignore_suspend = 1,
		/* this dai link has playback support */
	},
	{/* hw:x,8 */
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-dev.240" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-afe"),
		},
		.num_platforms = 1,

		.name = "MSM AFE-PCM TX",
		.stream_name = "AFE-PROXY TX",
		.ignore_suspend = 1,
	},
	{/* hw:x,9 */
		.name = "MSM8X16 Compress1",
		.stream_name = "Compress1",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia4" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-compress-dsp"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		 /* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA4,
	},
	{/* hw:x,10 */
		.name = "AUXPCM Hostless",
		.stream_name = "AUXPCM Hostless",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "AUXPCM_HOSTLESS" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-hostless"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dai link has playback support */
	},
	{/* hw:x,11 */
		.name = "SLIMBUS_1 Hostless",
		.stream_name = "SLIMBUS_1 Hostless",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "SLIMBUS1_HOSTLESS" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-hostless"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1, /* dai link has playback support */
	},
	{/* hw:x,12 */
		.name = "MSM8952 LowLatency",
		.stream_name = "MultiMedia5",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia5" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-dsp.1"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA5,
	},
	{/* hw:x,13 */
		.name = "Voice2",
		.stream_name = "Voice2",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "VoiceMMode1" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-voice"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_VOICE2,
	},
	{/* hw:x,14 */
		.name = "MSM8952 Media9",
		.stream_name = "MultiMedia9",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia9" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-dsp.0"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* This dailink has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA9,
	},
	{ /* hw:x,15 */
		.name = "VoLTE",
		.stream_name = "VoLTE",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "VoiceMMode1" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-voice"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_VOLTE,
	},
	{ /* hw:x,16 */
		.name = "VoWLAN",
		.stream_name = "VoWLAN",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "VoiceMMode1" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-voice"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.id = MSM_FRONTEND_DAI_VOWLAN,
	},
	{/* hw:x,17 */
		.name = "INT_HFP_BT Hostless",
		.stream_name = "INT_HFP_BT Hostless",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "INT_HFP_BT_HOSTLESS" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-hostless"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dai link has playback support */
	},
	{/* hw:x,18 */
		.name = "MSM8916 HFP",
		.stream_name = "MultiMedia6",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia6" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-loopback"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA6,
	},
	/* LSM FE */
	{/* hw:x,19 */
		.name = "Listen 1 Audio Service",
		.stream_name = "Listen 1 Audio Service",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "LSM1" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-lsm-client"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST },
		.ignore_suspend = 1,
		.id = MSM_FRONTEND_DAI_LSM1,
	},
	{/* hw:x,20 */
		.name = "Listen 2 Audio Service",
		.stream_name = "Listen 2 Audio Service",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "LSM2" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-lsm-client"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST },
		.ignore_suspend = 1,
		.id = MSM_FRONTEND_DAI_LSM2,
	},
	{/* hw:x,21 */
		.name = "Listen 3 Audio Service",
		.stream_name = "Listen 3 Audio Service",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "LSM3" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-lsm-client"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST },
		.ignore_suspend = 1,
		.id = MSM_FRONTEND_DAI_LSM3,
	},
	{/* hw:x,22 */
		.name = "Listen 4 Audio Service",
		.stream_name = "Listen 4 Audio Service",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "LSM4" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-lsm-client"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST },
		.ignore_suspend = 1,
		.id = MSM_FRONTEND_DAI_LSM4,
	},
	{/* hw:x,23 */
		.name = "Listen 5 Audio Service",
		.stream_name = "Listen 5 Audio Service",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "LSM5" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-lsm-client"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST },
		.ignore_suspend = 1,
		.id = MSM_FRONTEND_DAI_LSM5,
	},
	{/* hw:x,24 */
		.name = "MSM8X16 Compress2",
		.stream_name = "Compress2",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia7" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-compress-dsp"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA7,
	},
	{/* hw:x,25 */
		.name = "SLIMBUS_3 Hostless",
		.stream_name = "SLIMBUS_3 Hostless",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "SLIMBUS3_HOSTLESS" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-hostless"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1, /* dai link has playback support */
	},
	{/* hw:x,26 */
		.name = "SLIMBUS_4 Hostless",
		.stream_name = "SLIMBUS_4 Hostless",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "SLIMBUS4_HOSTLESS" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-hostless"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1, /* dai link has playback support */
	},
	{ /* hw:x,27 */
		.name = "QUAT_MI2S Hostless",
		.stream_name = "QUAT_MI2S Hostless",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "QUAT_MI2S_RX_HOSTLESS" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-hostless"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dai link has playback support */
	},
	{/* hw:x,28 */
		.name = "MSM8X16 MultiMedia10",
		.stream_name = "MultiMedia10",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia10" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-dsp.1"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		 /* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA10,
	},
	{/* hw:x,29 */
		.name = "MSM8X16 Compress4",
		.stream_name = "Compress4",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia11" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-compress-dsp"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		 /* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA11,
	},
	{/* hw:x,30 */
		.name = "MSM8X16 Compress5",
		.stream_name = "Compress5",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia12" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-compress-dsp"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		 /* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA12,
	},
	{/* hw:x,31 */
		.name = "MSM8X16 Compress6",
		.stream_name = "Compress6",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia13" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-compress-dsp"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		 /* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA13,
	},
	{/* hw:x,32 */
		.name = "MSM8X16 Compress7",
		.stream_name = "Compress7",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia14" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-compress-dsp"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		 /* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA14,
	},
	{/* hw:x,33 */
		.name = "MSM8X16 Compress8",
		.stream_name = "Compress8",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia15" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-compress-dsp"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		 /* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA15,
	},
	{/* hw:x,34 */
		.name = "MSM8X16 Compress9",
		.stream_name = "Compress9",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia16" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-dsp-noirq"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		 /* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA16,
	},
	{/* hw:x,35 */
		.name = "VoiceMMode1",
		.stream_name = "VoiceMMode1",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "VoiceMMode1" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-voice"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.id = MSM_FRONTEND_DAI_VOICEMMODE1,
	},
	{/* hw:x,36 */
		.name = "VoiceMMode2",
		.stream_name = "VoiceMMode2",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "VoiceMMode2" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-voice"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.id = MSM_FRONTEND_DAI_VOICEMMODE2,
	},
	{/* hw:x,37 */
		.name = "MSM8X16 Compress10",
		.stream_name = "Compress10",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia17" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-compress-dsp"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		 /* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA17,
	},
	{/* hw:x,38 */
		.name = "MSM8X16 Compress11",
		.stream_name = "Compress11",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia18" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-compress-dsp"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		 /* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA18,
	},
	{/* hw:x,39 */
		.name = "MSM8X16 Compress12",
		.stream_name = "Compress12",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "MultiMedia19" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-compress-dsp"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			 SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		 /* this dai link has playback support */
		.id = MSM_FRONTEND_DAI_MULTIMEDIA19,
	},
};

static struct snd_soc_dai_link msm8952_tdm_fe_dai[] = {
	/* FE TDM DAI links */
	{
		.name = "Primary TDM RX 0 Hostless",
		.stream_name = "Primary TDM RX 0 Hostless",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "PRI_TDM_RX_0_HOSTLESS" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-hostless"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
	},
	{
		.name = "Primary TDM TX 0 Hostless",
		.stream_name = "Primary TDM TX 0 Hostless",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "PRI_TDM_TX_0_HOSTLESS" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-hostless"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
	},
	{
		.name = "Secondary TDM RX 0 Hostless",
		.stream_name = "Secondary TDM RX 0 Hostless",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "SEC_TDM_RX_0_HOSTLESS" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-hostless"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_playback = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
	},
	{
		.name = "Secondary TDM TX 0 Hostless",
		.stream_name = "Secondary TDM TX 0 Hostless",
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "SEC_TDM_TX_0_HOSTLESS" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-hostless"),
		},
		.num_platforms = 1,
		.dynamic = 1,
		.dpcm_capture = 1,
		.codecs = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "snd-soc-dummy-dai" },
		},
		.num_codecs = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
	},
};

static struct snd_soc_dai_link msm8952_common_be_dai[] = {
	/* Backend I2S DAI Links */
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-mi2s.3" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_QUAT_MI2S_RX,
		.stream_name = "Quaternary MI2S Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_QUATERNARY_MI2S_RX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ops = &msm8952_quat_mi2s_be_ops, /* dai link has playback support */
		.ignore_suspend = 1,
	},
	/* Primary AUX PCM Backend DAI Links */
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-auxpcm.1" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_AUXPCM_RX,
		.stream_name = "AUX PCM Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_AUXPCM_RX,
		.be_hw_params_fixup = msm_auxpcm_be_params_fixup,
		.ops = &msm_pri_auxpcm_be_ops,
		.ignore_suspend = 1,
	},
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-auxpcm.1" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_AUXPCM_TX,
		.stream_name = "AUX PCM Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_AUXPCM_TX,
		.be_hw_params_fixup = msm_auxpcm_be_params_fixup,
		.ops = &msm_pri_auxpcm_be_ops,
		.ignore_suspend = 1,
	},
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-mi2s.3" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_QUAT_MI2S_TX,
		.stream_name = "Quaternary MI2S Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_QUATERNARY_MI2S_TX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ops = &msm8952_quat_mi2s_be_ops,
		.ignore_suspend = 1,
	},
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-dev.12288" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_INT_BT_SCO_RX,
		.stream_name = "Internal BT-SCO Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_INT_BT_SCO_RX,
		.be_hw_params_fixup = msm_btsco_be_hw_params_fixup,
		/* this dai link has playback support */
		.ignore_suspend = 1,
	},
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-dev.12289" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_INT_BT_SCO_TX,
		.stream_name = "Internal BT-SCO Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_INT_BT_SCO_TX,
		.be_hw_params_fixup = msm_btsco_be_hw_params_fixup,
		.ignore_suspend = 1,
	},
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-dev.12292" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_INT_FM_RX,
		.stream_name = "Internal FM Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_INT_FM_RX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		/* this dai link has playback support */
		.ignore_suspend = 1,
	},
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-dev.12293" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_INT_FM_TX,
		.stream_name = "Internal FM Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_INT_FM_TX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ignore_suspend = 1,
	},
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-dev.224" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_AFE_PCM_RX,
		.stream_name = "AFE Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_AFE_PCM_RX,
		.be_hw_params_fixup = msm_proxy_rx_be_hw_params_fixup,
		/* this dai link has playback support */
		.ignore_suspend = 1,
	},
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-dev.225" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_AFE_PCM_TX,
		.stream_name = "AFE Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_AFE_PCM_TX,
		.be_hw_params_fixup = msm_proxy_tx_be_hw_params_fixup,
		.ignore_suspend = 1,
	},
	/* Incall Record Uplink BACK END DAI Link */
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-dev.32772" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_INCALL_RECORD_TX,
		.stream_name = "Voice Uplink Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_INCALL_RECORD_TX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ignore_suspend = 1,
	},
	/* Incall Record Downlink BACK END DAI Link */
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-dev.32771" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_INCALL_RECORD_RX,
		.stream_name = "Voice Downlink Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_INCALL_RECORD_RX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ignore_suspend = 1,
	},
	/* Incall Music BACK END DAI Link */
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-dev.32773" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_VOICE_PLAYBACK_TX,
		.stream_name = "Voice Farend Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_VOICE_PLAYBACK_TX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ignore_suspend = 1,
	},
	/* Incall Music 2 BACK END DAI Link */
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-dev.32770" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_VOICE2_PLAYBACK_TX,
		.stream_name = "Voice2 Farend Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_VOICE2_PLAYBACK_TX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ignore_suspend = 1,
	},

	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-mi2s.5" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_QUIN_MI2S_TX,
		.stream_name = "Quinary MI2S Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_QUINARY_MI2S_TX,
		.be_hw_params_fixup = msm_be_hw_params_fixup,
		.ops = &msm8952_quin_mi2s_be_ops,
		.ignore_suspend = 1,
	},
	/* Proxy Tx BACK END DAI Link */
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-dev.8195" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_PROXY_TX,
		.stream_name = "Proxy Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_PROXY_TX,
		.ignore_suspend = 1,
	},
	/* Proxy Rx BACK END DAI Link */
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-dev.8194" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_PROXY_RX,
		.stream_name = "Proxy Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_PROXY_RX,
		.ignore_suspend = 1,
	},
};

static struct snd_soc_dai_link msm8952_hdmi_dba_dai_link[] = {
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-mi2s.5" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_QUIN_MI2S_RX,
		.stream_name = "Quinary MI2S Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_QUINARY_MI2S_RX,
		.be_hw_params_fixup = msm_quin_be_hw_params_fixup,
		.ops = &msm8952_quin_mi2s_be_ops, /* dai link has playback support */
		.ignore_suspend = 1,
	},
};

static struct snd_soc_dai_link msm8952_quin_dai_link[] = {
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-mi2s.5" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_QUIN_MI2S_RX,
		.stream_name = "Quinary MI2S Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_QUINARY_MI2S_RX,
		.be_hw_params_fixup = msm_quin_be_hw_params_fixup,
		.ops = &msm8952_quin_mi2s_be_ops, /* dai link has playback support */
		.ignore_suspend = 1,
	},
};

static struct snd_soc_dai_link msm8952_tdm_be_dai_link[] = {
	/* TDM be dai links */
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-tdm.36864" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_PRI_TDM_RX_0,
		.stream_name = "Primary TDM0 Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_PRI_TDM_RX_0,
		.be_hw_params_fixup = msm_tdm_be_hw_params_fixup,
		.ops = &msm_tdm_be_ops,
		.ignore_suspend = 1,
	},
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-tdm.36865" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_PRI_TDM_TX_0,
		.stream_name = "Primary TDM0 Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_PRI_TDM_TX_0,
		.be_hw_params_fixup = msm_tdm_be_hw_params_fixup,
		.ops = &msm_tdm_be_ops,
		.ignore_suspend = 1,
	},
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-tdm.36880" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_SEC_TDM_RX_0,
		.stream_name = "Secondary TDM0 Playback",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.id = MSM_BACKEND_DAI_SEC_TDM_RX_0,
		.be_hw_params_fixup = msm_tdm_be_hw_params_fixup,
		.ops = &msm_tdm_be_ops,
		.ignore_suspend = 1,
	},
	{
		.cpus = (struct snd_soc_dai_link_component[]){
			{ .dai_name = "msm-dai-q6-tdm.36881" },
		},
		.num_cpus = 1,
		.platforms = (struct snd_soc_dai_link_component[]){
			COMP_PLATFORM("msm-pcm-routing"),
		},
		.num_platforms = 1,

		.name = LPASS_BE_SEC_TDM_TX_0,
		.stream_name = "Secondary TDM0 Capture",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.id = MSM_BACKEND_DAI_SEC_TDM_TX_0,
		.be_hw_params_fixup = msm_tdm_be_hw_params_fixup,
		.ops = &msm_tdm_be_ops,
		.ignore_suspend = 1,
	},
};

struct msm895x_wsa881x_dev_info {
	struct device_node *of_node;
	u32 index;
};

static struct snd_soc_aux_dev *msm895x_aux_dev;
static struct snd_soc_codec_conf *msm895x_codec_conf;


int msm8952_init_wsa_dev(struct platform_device *pdev,
			struct snd_soc_card *card)
{
	struct device_node *wsa_of_node;
	u32 wsa_max_devs;
	u32 wsa_dev_cnt;
	char *dev_name_str = NULL;
	struct msm895x_wsa881x_dev_info *wsa881x_dev_info;
	const char *wsa_auxdev_name_prefix[1];
	int found = 0;
	int i;
	int ret;

	/* Get maximum WSA device count for this platform */
	ret = of_property_read_u32(pdev->dev.of_node,
					"qcom,wsa-max-devs", &wsa_max_devs);
	if (ret) {
		dev_dbg(&pdev->dev,
			"%s: wsa-max-devs property missing in DT %s, ret = %d\n",
			__func__, pdev->dev.of_node->full_name, ret);
		card->num_aux_devs = 0;
		return 0;
	}
	if (wsa_max_devs == 0) {
		dev_warn(&pdev->dev,
			"%s: Max WSA devices is 0 for this target?\n",
			__func__);
		card->num_aux_devs = 0;
		return 0;
	}

	/* Get count of WSA device phandles for this platform */
	wsa_dev_cnt = of_count_phandle_with_args(pdev->dev.of_node,
						"qcom,wsa-devs", NULL);
	if (wsa_dev_cnt == -ENOENT) {
		dev_warn(&pdev->dev, "%s: No wsa device defined in DT.\n",
			__func__);
		return 0;
	} else if (wsa_dev_cnt <= 0) {
		dev_err(&pdev->dev,
			"%s: Error reading wsa device from DT. wsa_dev_cnt = %d\n",
			__func__, wsa_dev_cnt);
		return -EINVAL;
	}

	/*
	 * Expect total phandles count to be NOT less than maximum possible
	 * WSA count. However, if it is less, then assign same value to
	 * max count as well.
	 */

	if (wsa_dev_cnt < wsa_max_devs) {
		dev_dbg(&pdev->dev,
			"%s: wsa_max_devs = %d cannot exceed wsa_dev_cnt = %d\n",
			__func__, wsa_max_devs, wsa_dev_cnt);
		wsa_max_devs = wsa_dev_cnt;
	}

	/* Make sure prefix string passed for each WSA device */
	ret = of_property_count_strings(pdev->dev.of_node,
			"qcom,wsa-aux-dev-prefix");
	if (ret != wsa_dev_cnt) {
		dev_err(&pdev->dev,
				"%s: expecting %d wsa prefix. Defined only %d in DT\n",
				__func__, wsa_dev_cnt, ret);
		return -EINVAL;
	}

	/*
	 * Alloc mem to store phandle and index info of WSA device, if already
	 * registered with ALSA core
	 */
	wsa881x_dev_info = devm_kcalloc(&pdev->dev, wsa_max_devs,
			sizeof(struct msm895x_wsa881x_dev_info),
			GFP_KERNEL);
	if (!wsa881x_dev_info)
		return -ENOMEM;

	/*
	 * search and check whether all WSA devices are already
	 * registered with ALSA core or not. If found a node, store
	 * the node and the index in a local array of struct for later
	 * use.
	 */
	for (i = 0; i < wsa_dev_cnt; i++) {
		wsa_of_node = of_parse_phandle(pdev->dev.of_node,
				"qcom,wsa-devs", i);
		if (unlikely(!wsa_of_node)) {
			/* we should not be here */
			dev_err(&pdev->dev,
					"%s: wsa dev node is not present\n",
					__func__);
			ret = -EINVAL;
			goto err_free_dev_info;
		}
		/*
		 * CAF asks the ALSA core whether this node is already a
		 * registered component. This tree keeps that list private to the
		 * core, so a node that we have already recorded counts instead.
		 */
		if (of_device_is_available(wsa_of_node)) {
			wsa881x_dev_info[found].of_node = wsa_of_node;
			wsa881x_dev_info[found].index = i;
			found++;
			if (found == wsa_max_devs)
				break;
		}
	}

	if (found < wsa_max_devs) {
		dev_dbg(&pdev->dev,
			"%s: failed to find %d components. Found only %d\n",
			__func__, wsa_max_devs, found);
		return -EPROBE_DEFER;
	}
	dev_info(&pdev->dev,
		"%s: found %d wsa881x devices registered with ALSA core\n",
		__func__, found);

	card->num_aux_devs = wsa_max_devs;
	card->num_configs = wsa_max_devs;

	/* Alloc array of AUX devs struct */
	msm895x_aux_dev = devm_kcalloc(&pdev->dev, card->num_aux_devs,
			sizeof(struct snd_soc_aux_dev),
			GFP_KERNEL);
	if (!msm895x_aux_dev) {
		ret = -ENOMEM;
		goto err_free_dev_info;
	}

	/* Alloc array of codec conf struct */
	msm895x_codec_conf = devm_kcalloc(&pdev->dev, card->num_aux_devs,
			sizeof(struct snd_soc_codec_conf),
			GFP_KERNEL);
	if (!msm895x_codec_conf) {
		ret = -ENOMEM;
		goto err_free_aux_dev;
	}

	for (i = 0; i < card->num_aux_devs; i++) {
		dev_name_str = devm_kzalloc(&pdev->dev, DEV_NAME_STR_LEN,
				GFP_KERNEL);
		if (!dev_name_str) {
			ret = -ENOMEM;
			goto err_free_cdc_conf;
		}

		ret = of_property_read_string_index(pdev->dev.of_node,
				"qcom,wsa-aux-dev-prefix",
				wsa881x_dev_info[i].index,
				wsa_auxdev_name_prefix);
		if (ret) {
			dev_err(&pdev->dev,
					"%s: failed to read wsa aux dev prefix, ret = %d\n",
					__func__, ret);
			ret = -EINVAL;
			goto err_free_dev_name_str;
		}

		scnprintf(dev_name_str, sizeof(dev_name_str), "wsa881x.%d", i);
		msm895x_aux_dev[i].dlc.name = dev_name_str;
		msm895x_aux_dev[i].dlc.of_node = wsa881x_dev_info[i].of_node;
		msm895x_aux_dev[i].init = msm895x_wsa881x_init;
		msm895x_codec_conf[i].dlc.of_node = wsa881x_dev_info[i].of_node;
		msm895x_codec_conf[i].name_prefix = wsa_auxdev_name_prefix[0];
	}
	card->codec_conf = msm895x_codec_conf;
	card->aux_dev = msm895x_aux_dev;

	return 0;
err_free_dev_name_str:
	devm_kfree(&pdev->dev, dev_name_str);
err_free_cdc_conf:
	devm_kfree(&pdev->dev, msm895x_codec_conf);
err_free_aux_dev:
	devm_kfree(&pdev->dev, msm895x_aux_dev);
err_free_dev_info:
	devm_kfree(&pdev->dev, wsa881x_dev_info);

	return ret;
}

struct snd_soc_card *populate_snd_card_dailinks(struct device *dev)
{
	struct snd_soc_card *card = &snd_soc_card_msm_card;
	struct snd_soc_dai_link *msm8952_dai_links = NULL;
	int num_links, ret, len1, len2, len3, len4, len5 = 0;

	card->dev = dev;
	ret = snd_soc_of_parse_card_name(card, "qcom,model");
	if (ret) {
		dev_err(dev, "%s: parse card name failed, err:%d\n",
				__func__, ret);
		return NULL;
	}
	/* The tasha (WCD9335 over slimbus) codec is not a codec on this
	 * board: the donor's Xiaomi boards delete the sound-9335 node, and
	 * their audio is the analog CDC plus the MAX98927 amplifier. The
	 * table that would carry it is gone with it.
	 */
	if (of_property_read_bool(dev->of_node, "qcom,hdmi-dba-codec-rx")) {
		dev_dbg(dev, "%s(): hdmi dba audio support present\n",
				__func__);
		memcpy(msm8952_dai_links + len5, msm8952_hdmi_dba_dai_link,
			sizeof(msm8952_hdmi_dba_dai_link));
		len5 += ARRAY_SIZE(msm8952_hdmi_dba_dai_link);
	} else {
		dev_dbg(dev, "%s(): No hdmi dba present, add quin dai\n",
				__func__);
		memcpy(msm8952_dai_links + len5, msm8952_quin_dai_link,
			sizeof(msm8952_quin_dai_link));
		len5 += ARRAY_SIZE(msm8952_quin_dai_link);
	}
	if (of_property_read_bool(dev->of_node, "qcom,tdm-audio-intf")) {
		dev_dbg(dev, "%s(): TDM support present\n",
				__func__);
		memcpy(msm8952_dai_links + len5, msm8952_tdm_be_dai_link,
			sizeof(msm8952_tdm_be_dai_link));
		len5 += ARRAY_SIZE(msm8952_tdm_be_dai_link);
	}
	card->dai_link = msm8952_dai_links;
	card->num_links = len5;
	card->dev = dev;
	return card;
}

void msm895x_free_auxdev_mem(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	int i;

	if (card->num_aux_devs > 0) {
		for (i = 0; i < card->num_aux_devs; i++) {
			kfree(msm895x_aux_dev[i].dlc.name);
			kfree(msm895x_codec_conf[i].dlc.name);
			kfree(msm895x_codec_conf[i].name_prefix);
		}
	}
}
