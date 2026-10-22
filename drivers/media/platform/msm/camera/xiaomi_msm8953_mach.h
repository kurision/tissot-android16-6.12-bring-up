// SPDX-License-Identifier: GPL-2.0-only
/*
 * The per-board switch of the donor's camera tree.
 *
 * The donor carried a header of its own (techpack/xiaomi-msm8953, the
 * mach of the Xiaomi phones of that platform) and the camera tree asked
 * it which phone it was running on, to take the quirks of that phone
 * (the ROSY, the YSL and the VINCE).  This tree is one phone, so the
 * answer is always the tissot and the quirks of the other phones never
 * run.
 *
 * The names and the order of the enumeration are the donor's.
 */
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

static inline enum xiaomi_msm8953_mach_types xiaomi_msm8953_mach_get(void)
{
	return XIAOMI_MSM8953_MACH_TISSOT;
}

static inline enum xiaomi_msm8953_mach_family_types
xiaomi_msm8953_mach_get_family(void)
{
	return XIAOMI_MSM8953_MACH_FAMILY_MTP;
}

#endif /* _XIAOMI_MSM8953_MACH_H */
