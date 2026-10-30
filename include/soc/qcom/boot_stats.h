/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2013-2018, The Linux Foundation. All rights reserved.
 *
 * The CAF 4.19 boot statistics driver (drivers/soc/qcom/boot_stats.c) is
 * not in this tree: it reads the bootloader timestamps out of a memory
 * region that this board does not have.  The CAF video codec calls
 * boot_stats_init() once, so the call is a no-op here.
 */
#ifndef _SOC_QCOM_BOOT_STATS_H
#define _SOC_QCOM_BOOT_STATS_H

static inline int boot_stats_init(void) { return 0; }

#endif /* _SOC_QCOM_BOOT_STATS_H */
