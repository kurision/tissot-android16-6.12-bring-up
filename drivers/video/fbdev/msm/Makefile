ccflags-y += -Wno-missing-prototypes
# SPDX-License-Identifier: GPL-2.0-only
# Tissot MDSS fbdev port, Phase 1. Keep composite object name mdss-mdp.o:
# KBUILD_MODNAME becomes mdss_mdp so LK mdss_mdp.panel= keeps binding.
ccflags-y += -I$(src)
ccflags-y += -Wno-implicit-fallthrough -Wno-unused-function

mdss-mdp-objs := mdss_mdp.o mdss_mdp_ctl.o mdss_mdp_pipe.o mdss_mdp_util.o dsi_status_6g.o
mdss-mdp-objs += mdss_mdp_pp.o mdss_mdp_pp_debug.o mdss_mdp_pp_cache_config.o mdss_sync.o
mdss-mdp-objs += mdss_mdp_intf_video.o
mdss-mdp-objs += mdss_mdp_intf_cmd.o
mdss-mdp-objs += mdss_mdp_intf_writeback.o
mdss-mdp-objs += mdss_rotator.o
mdss-mdp-objs += mdss_mdp_overlay.o
mdss-mdp-objs += mdss_mdp_layer.o
mdss-mdp-objs += mdss_mdp_splash_logo.o
mdss-mdp-objs += mdss_mdp_cdm.o
mdss-mdp-objs += mdss_smmu.o
mdss-mdp-objs += mdss_mdp_wfd.o
mdss-mdp-objs += mdss_io_util.o
obj-$(CONFIG_FB_MSM_MDSS) += mdss-mdp.o

mdss-mdp-objs += mdss_mdp_pp_v1_7.o
mdss-mdp-objs += mdss_mdp_pp_v3.o
mdss-mdp-objs += mdss_mdp_pp_common.o
mdss-mdp-objs += mdss_mdp_pp_stub.o

ifeq ($(CONFIG_FB_MSM_MDSS),y)
obj-$(CONFIG_DEBUG_FS) += mdss_debug.o mdss_debug_xlog.o mdss_mdp_debug.o
endif

mdss-dsi-objs := mdss_dsi.o mdss_dsi_host.o mdss_dsi_cmd.o mdss_dsi_status.o
mdss-dsi-objs += mdss_dsi_panel.o
mdss-dsi-objs += msm_mdss_io_8974.o
mdss-dsi-objs += mdss_dsi_phy.o
mdss-dsi-objs += mdss_dsi_phy_v3.o
mdss-dsi-objs += mdss_dsi_phy_12nm.o
mdss-dsi-objs += mdss_dsi_clk.o
obj-$(CONFIG_FB_MSM_MDSS) += mdss-dsi.o
obj-$(CONFIG_FB_MSM_MDSS) += mdss_panel.o

ifneq ($(CONFIG_FB_MSM_MDP3), y)
obj-$(CONFIG_FB_MSM_MDSS) += mdss_hdmi_util.o
obj-$(CONFIG_FB_MSM_MDSS) += mdss_hdmi_edid.o
obj-$(CONFIG_FB_MSM_MDSS) += mdss_cec_core.o
obj-$(CONFIG_FB_MSM_MDSS) += mdss_dba_utils.o
endif

obj-$(CONFIG_FB_MSM_MDSS_WRITEBACK) += mdss_wb.o

obj-$(CONFIG_FB_MSM_MDSS) += mdss_fb.o mdss_util.o
# mdss_compat_utils.o deferred: uses removed compat_alloc_user_space/
# copy_in_user; no 32-bit client identified (plan Q5).

obj-$(CONFIG_FB_MSM_MDSS) += mdss_livedisplay.o
