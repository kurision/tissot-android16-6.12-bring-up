ccflags-y += -Wno-missing-prototypes
ccflags-y += -I $(srctree)/drivers/media/platform/msm/camera
ccflags-y += -I $(srctree)/drivers/media/platform/msm/camera/sensor/io
ccflags-y += -I $(srctree)/drivers/media/platform/msm/camera/common/
ifeq ($(CONFIG_MSM_ISP_V1),y)
obj-$(CONFIG_MSM_CSID) += msm_ispif_32.o
else
obj-$(CONFIG_MSM_CSID) += msm_ispif.o
endif