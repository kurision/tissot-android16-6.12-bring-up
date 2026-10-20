# SPDX-License-Identifier: GPL-2.0
# WCD9xxx codecs for tissot, ported from CAF techpack/audio.
# CAF's sources include their private headers as <asoc/...>, <dsp/...>,
# <soc/...>, <ipc/...> and <sound/...>, so those trees are staged alongside.

ccflags-y += -I$(srctree)/sound/soc/msm/caf-include
# CAF's audio stack includes its uapi headers as <sound/...>, and the
# kernel tree dropped some of them upstream, so they are staged here.
ccflags-y += -I$(srctree)/sound/soc/msm/caf-include/uapi
#
# The imported CAF sources predate 6.12 warnings that are fatal here. Their
# unused locals and parameters are left alone rather than edited out of vendor
# code; the real API changes are fixed in the sources themselves.
ccflags-y += -Wno-unused-but-set-variable -Wno-unused-variable \
	     -Wno-unused-but-set-parameter -Wno-unused-parameter -Wno-unused-function -Wno-unused-const-variable

wcd9335-objs += wcd9335.o wcd9335-regmap.o wcd9335-tables.o
wcd9xxx-objs += wcd9xxx-core.o wcd9xxx-core-init.o wcd9xxx-irq.o \
		wcd9xxx-resmgr-v2.o wcd9xxx-slimslave.o wcd9xxx-utils.o \
		wcd9xxx-rst.o wcd9xxx-soc-init.o wcd9xxx-common-v2.o
wcdmbhc-objs += wcd-mbhc-v2.o wcd-mbhc-adc.o wcd-clsh.o
wcddsp-objs += wcd-dsp-mgr.o

obj-$(CONFIG_SND_SOC_MSM_WCD9335) += wcd9335.o
obj-$(CONFIG_WCD9XXX_CODEC_CORE) += wcd9xxx.o
obj-$(CONFIG_SND_SOC_MSM_WCD_MBHC) += wcdmbhc.o
obj-$(CONFIG_WCD_DSP_GLINK) += wcddsp.o

#
# The legacy pair is the CAF CDC the machine driver and the WCD9335 were
# written against. The other pair is the same driver in its upstream form
# and defines the same symbols, so only one of them can be built.
cdc-objs += msm-analog-cdc.o msm-digital-cdc-legacy.o \
	    msm-digital-cdc-legacy-regmap.o sdm660-cdc-irq.o

obj-$(CONFIG_SND_SOC_SDM660_CDC) += cdc.o

msm8952-objs += msm8952.o msm8952-dai-links.o msm8952-slimbus.o
msm8952-objs += msm-pcm-afe-v2.o

obj-$(CONFIG_SND_SOC_MSM8952) += msm8952.o

#
# The PCM platform layer and the CPU DAIs. The machine driver's backend
# links all use the routing PCM, and its frontend links use the DSP PCM
# and the FE DAIs, so without these the card registers with no routes
# at all and the vendor audio HAL finds no device to open.
#
# The compress (offload) and the listen-service platforms are not built
# yet: they need CAF's compress_params.h, whose layout differs from the
# one this kernel's userspace expects, so they are a separate decision.
msmplatform-objs += msm-pcm-q6-v2.o msm-pcm-q6-noirq.o msm-pcm-hostless.o \
		     msm-pcm-voice-v2.o msm-pcm-routing-v2.o \
		     msm-pcm-routing-devdep.o msm-pcm-loopback-v2.o \
		     msm-dai-q6-v2.o msm-dai-fe.o msm-dai-stub-v2.o

obj-$(CONFIG_SND_SOC_MSM8952) += msmplatform.o

afedrv-objs += q6afe.o

obj-$(CONFIG_WCD9XXX_CODEC_CORE) += afedrv.o
