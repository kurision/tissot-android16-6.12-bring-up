/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014, 2019-2020, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __VENUS_BOOT_H__
#define __VENUS_BOOT_H__
#include <linux/errno.h>
#include "msm_vidc_resources.h"

/*
 * CAF's non-secure PIL (venus_boot.c) loaded the firmware through the
 * Venus SMMU and started the core itself, for a tree that sets
 * "qcom,use-non-secure-pil" and has a "qcom,fw-context-bank" child.
 * msm8953 boots Venus through TrustZone instead (PAS, see
 * msm_subsystem_get_with_fwname()), and the donor's msm8953-vidc.dtsi
 * has neither property. The non-secure PIL depends on the ARM DMA-IOMMU
 * mapping API (<asm/dma-iommu.h>), which arm64 does not have on 6.12, so
 * it is not ported. A tree that asks for it fails here.
 */
static inline int venus_boot_init(struct msm_vidc_platform_resources *res,
		struct context_bank_info *cb)
{
	return -ENODEV;
}

static inline void venus_boot_deinit(void)
{
}

#endif /* __VENUS_BOOT_H__ */
