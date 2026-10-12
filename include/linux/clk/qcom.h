/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * 4.19 Qualcomm clock flags (linux/clk/qcom.h) are gone from 6.12;
 * retain/noretain control lives in the clock controller and power domains
 * now. Callers that only vote retention around register access get a no-op;
 * drivers that need real retention must model it in their power domain.
 */
#ifndef __LINUX_CLK_QCOM_H
#define __LINUX_CLK_QCOM_H

#include <linux/bitops.h>

#define CLKFLAG_NORETAIN_PERIPH	BIT(0)
#define CLKFLAG_NORETAIN_MEM	BIT(1)
#define CLKFLAG_RETAIN_PERIPH	BIT(2)
#define CLKFLAG_RETAIN_MEM	BIT(3)
#define CLKFLAG_PERIPH_OFF_SET	BIT(4)
#define CLKFLAG_PERIPH_OFF_CLEAR BIT(5)

struct clk;

static inline int clk_set_flags(struct clk *clk, unsigned long flags)
{
	return 0;
}

#endif
