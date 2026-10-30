/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */

#ifndef _DT_BINDINGS_CLK_QCOM_APCC_MSM8953_H
#define _DT_BINDINGS_CLK_QCOM_APCC_MSM8953_H

/*
 * All eight Cortex-A53 cores and the system CCI share a single APCS PLL on
 * MSM8953, so the PLL is deliberately not exported: three independent clock
 * IDs pointing at one register block would let two consumers reprogram it
 * behind each other's back. It reaches the three muxes as their first parent.
 */
#define APCC_CPU0_CLK		0
#define APCC_CPU4_CLK		1
#define APCC_CCI_CLK		2
#define APCC_NUM_CLOCKS		3

#endif /* _DT_BINDINGS_CLK_QCOM_APCC_MSM8953_H */
