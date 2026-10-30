ccflags-y += -Wno-missing-prototypes
ccflags-y += -I $(srctree)/drivers/media/platform/msm/camera
ccflags-y += -I $(srctree)/drivers/media/platform/msm/camera/sensor/io
ccflags-y += -I $(srctree)/drivers/media/platform/msm/camera/common/
ifeq ($(CONFIG_MSM_ISP_V1),y)
obj-$(CONFIG_MSMB_CAMERA) += msm_isp_32.o msm_buf_mgr_32.o msm_isp_util_32.o msm_isp_axi_util_32.o msm_isp_stats_util_32.o
obj-$(CONFIG_MSMB_CAMERA) += msm_isp32.o
else
obj-$(CONFIG_MSMB_CAMERA) += msm_isp.o msm_buf_mgr.o msm_isp_util.o msm_isp_axi_util.o msm_isp_stats_util.o
# The donor builds the ISP of four generations and picks one at run
# time.  This board's VFE is the 4.0 (the camera nodes say "qcom,vfe40"),
# and the kernel of this port has to fit under the bootloader's limit, so
# the generations of the other chips are left out.  The 4.0 uses the
# 4.7's interrupt handling, so that one stays.
obj-$(CONFIG_MSMB_CAMERA) += msm_isp47.o msm_isp40.o
endif
