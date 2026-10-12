/* SPDX-License-Identifier: GPL-2.0-only */
/* Tissot-only board helper: this port binds tissot hardware exclusively. */
#ifndef _XIAOMI_MSM8953_MACH_H
#define _XIAOMI_MSM8953_MACH_H

enum xiaomi_msm8953_mach_family_types {
	XIAOMI_MSM8953_MACH_FAMILY_UNKNOWN = 0,
	XIAOMI_MSM8953_MACH_FAMILY_MTP,
	XIAOMI_MSM8953_MACH_FAMILY_QRD,
	XIAOMI_MSM8953_MACH_FAMILY_MAX,
};

enum xiaomi_msm8953_mach_types {
	XIAOMI_MSM8953_MACH_UNKNOWN = 0,
	XIAOMI_MSM8953_MACH_UTER,
	XIAOMI_MSM8953_MACH_ROSY,
	XIAOMI_MSM8953_MACH_YSL,
	XIAOMI_MSM8953_MACH_VINCE,
	XIAOMI_MSM8953_MACH_MIDO,
	XIAOMI_MSM8953_MACH_MARKW,
	XIAOMI_MSM8953_MACH_DAISY,
	XIAOMI_MSM8953_MACH_SAKURA,
	XIAOMI_MSM8953_MACH_TISSOT,
	XIAOMI_MSM8953_MACH_MAX,
};

static inline enum xiaomi_msm8953_mach_family_types
xiaomi_msm8953_mach_get_family(void)
{
	return XIAOMI_MSM8953_MACH_FAMILY_UNKNOWN;
}

static inline enum xiaomi_msm8953_mach_types xiaomi_msm8953_mach_get(void)
{
	return XIAOMI_MSM8953_MACH_TISSOT;
}

#endif
