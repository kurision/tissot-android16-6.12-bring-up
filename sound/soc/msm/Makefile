# SPDX-License-Identifier: GPL-2.0
# WCD9xxx codecs for tissot, ported from CAF techpack/audio.
# CAF's sources include their private headers as <asoc/...>, <dsp/...>,
# <soc/...>, <ipc/...> and <sound/...>, so those trees are staged alongside.

ccflags-y += -I$(srctree)/sound/soc/msm/caf-include
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

obj-$(CONFIG_SND_SOC_WCD9335) += wcd9335.o
obj-$(CONFIG_WCD9XXX_CODEC_CORE) += wcd9xxx.o
obj-$(CONFIG_SND_SOC_WCD_MBHC) += wcdmbhc.o
obj-$(CONFIG_WCD_DSP_GLINK) += wcddsp.o
