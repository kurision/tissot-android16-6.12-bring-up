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

#
# The WCD9xxx codec is not built: tissot has no such codec. The donor's
# Xiaomi boards delete the tasha (sound-9335) node and the WSA881x
# nodes, and their audio is the analog CDC plus the MAX98927 speaker
# amplifier. The sources stay here because the machine driver includes
# their headers, and the header's declarations are guarded by the same
# symbols.
#
# The microphone bias and headset control helper is built: it is not
# the codec but the headset detect and bias regulator handling, and the
# analog CDC uses it.
wcdmbhc-objs += wcd-mbhc-v2.o wcd-mbhc-adc.o wcd-clsh.o

obj-$(CONFIG_SND_SOC_MSM_WCD_MBHC) += wcdmbhc.o

#
# The legacy pair is the CAF CDC the machine driver and the WCD9335 were
# written against. The other pair is the same driver in its upstream form
# and defines the same symbols, so only one of them can be built.
cdc-objs += msm-analog-cdc.o msm-digital-cdc-legacy.o \
	    msm-digital-cdc-legacy-regmap.o sdm660-cdc-irq.o

obj-$(CONFIG_SND_SOC_SDM660_CDC) += cdc.o

# The slimbus codec machine is not built either: it is the machine for
# the tasha codec, which this board does not have.
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
msmplatform-objs += msm-cdc-supply.o
msmplatform-objs += msm-pcm-q6-v2.o msm-pcm-q6-noirq.o msm-pcm-hostless.o \
		     msm-pcm-voice-v2.o msm-pcm-routing-v2.o \
		     msm-pcm-routing-devdep.o msm-pcm-loopback-v2.o \
		     msm-dai-q6-v2.o msm-dai-fe.o msm-dai-stub-v2.o

obj-$(CONFIG_SND_SOC_MSM8952) += msmplatform.o

afedrv-objs += q6afe.o

obj-$(CONFIG_WCD9XXX_CODEC_CORE) += afedrv.o

#
# CAF 4.19's DSP library. The machine driver and the PCM platforms above
# are CAF 4.19's, and they reach the DSP through it: the ASM's cpu
# buffer model for the data, the ASM's volume and calibration, the
# voice service and the ADM. This kernel carries CAF's later AVD stack
# instead, which is a different model and not a rename of this one, so
# the library comes from the donor as well and the port is CAF 4.19
# consistent from the machine down to the DSP.
#
# The tree's own sound/soc/qcom/qdsp6 stack has to be off: it defines
# the same names (q6asm_cmd, q6asm_read, q6core_is_adsp_ready, ...) and
# nothing in this port uses it.
dsplib-objs += q6asm.o q6core.o q6adm.o q6voice.o q6audio-v2.o \
	       q6common.o adsp_err.o rtac.o sp_params.o avtimer.o \
	       audio_cal_utils.o audio_calibration.o msm_audio_ion.o \
	       digital-cdc-rsc-mgr.o msm-dts-srs-tm-config.o \
	       msm-audio-event-notify.o q6afecal-hwdep.o q6_init.o apr.o apr_v2.o apr_tal_rpmsg.o \
	       msm_mdf.o q6lsm.o wcd-dsp-glink.o wcdcal-hwdep.o

obj-$(CONFIG_SND_SOC_MSM8952) += dsplib.o
