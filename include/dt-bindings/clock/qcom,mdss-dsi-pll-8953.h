/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Clock IDs for the standalone MSM8953 DSI PLL provider
 * (drivers/clk/qcom/mdss-dsi-pll-8953.c, compatible
 * "qcom,mdss_dsi_pll_8953"). Small dense indexes, not the donor's hashed
 * MSM bus style IDs: the DT using them is written in this port, and the
 * only consumers are GCC byte/pixel RCG parents plus the MDSS DSI ext
 * clocks. The byte/pixel mux IDs resolve to the fixed /8 and /2 divider
 * outputs; the donor's shadow mux input does not exist (static panel).
 */
#ifndef __DT_BINDINGS_CLOCK_QCOM_MDSS_DSI_PLL_8953_H
#define __DT_BINDINGS_CLOCK_QCOM_MDSS_DSI_PLL_8953_H

#define MDSS_DSI_PLL_BYTE_MUX	0
#define MDSS_DSI_PLL_PIXEL_MUX	1
#define MDSS_DSI_PLL_BYTE_SRC	2
#define MDSS_DSI_PLL_PIXEL_SRC	3
#define MDSS_DSI_PLL_VCO	4
#define MDSS_DSI_PLL_NR_CLOCKS	5

#endif
