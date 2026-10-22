# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the CAF camera stack (the donor's techpack/camera-legacy).
#
# The donor built this tree with $(DISABLE_CFI_CLANG) and
# $(DISABLE_LTO_CLANG); this kernel builds with CFI and LTO on, so the
# flags are not carried over and the code is converted instead.

ccflags-y += -I $(srctree)/drivers/media/platform/msm/camera
ccflags-y += -I $(srctree)/drivers/media/platform/msm/camera/sensor
ccflags-y += -I $(srctree)/drivers/media/platform/msm/camera/pproc
ccflags-y += -I $(srctree)/drivers/media/platform/msm/camera/msm_vb2
ccflags-y += -I $(srctree)/drivers/media/platform/msm/camera/camera
ccflags-y += -I $(srctree)/drivers/media/platform/msm/camera/jpeg_10
ccflags-y += -I $(srctree)/drivers/media/platform/msm/camera/jpeg_dma
ccflags-y += -I $(srctree)/drivers/media/platform/msm/camera/fd
ccflags-y += -I $(srctree)/drivers/media/platform/msm/camera/common

obj-$(CONFIG_MSMB_CAMERA_LEGACY) += init.o

obj-$(CONFIG_MSMB_CAMERA) += common/
obj-$(CONFIG_MSMB_CAMERA) += msm.o
obj-$(CONFIG_MSMB_CAMERA) += camera/
obj-$(CONFIG_MSMB_CAMERA) += msm_vb2/
obj-$(CONFIG_MSMB_CAMERA) += sensor/
obj-$(CONFIG_MSMB_CAMERA) += pproc/
obj-$(CONFIG_MSMB_CAMERA) += isp/
obj-$(CONFIG_MSMB_CAMERA) += ispif/
obj-$(CONFIG_MSMB_JPEG) += jpeg_10/
obj-$(CONFIG_MSM_JPEGDMA) += jpeg_dma/
obj-$(CONFIG_MSMB_CAMERA) += msm_buf_mgr/
obj-$(CONFIG_MSM_FD) += fd/
