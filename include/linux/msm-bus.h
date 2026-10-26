/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2010-2018, The Linux Foundation. All rights reserved.
 * Copyright (c) 2026, The tissot kgsl-port authors.
 *
 * Vendor msm-bus client API, struct/prototype surface from the
 * 4.19 donor (include/linux/msm-bus.h). Implementation is
 * interconnect-backed; votes without a configured interconnect path are
 * accepted as success and logged once (bus voting is perf/power tuning,
 * not functional). Shared by KGSL and MDSS; single implementation in
 * drivers/gpu/msm/msm-bus-compat.c until the soc move.
 */
#ifndef _LINUX_MSM_BUS_H
#define _LINUX_MSM_BUS_H

#include <linux/types.h>
#include <linux/platform_device.h>

#define MSM_BUS_MAX_TCS_CMDS 16

/*
 * The vendor endpoint ids of the msm-bus tables, from the donor's
 * include/dt-bindings/msm/msm-bus-ids.h. The tables name their master
 * and their slave with these; the interconnect-backed implementation
 * maps them onto the endpoints of this SoC's interconnect.
 */
#ifndef MSM_BUS_MASTER_AMPSS_M0
#define MSM_BUS_MASTER_AMPSS_M0		1
#endif

#ifndef MSM_BUS_MASTER_MDP_PORT0
#define MSM_BUS_MASTER_MDP_PORT0		22
#endif

#ifndef MSM_BUS_MASTER_GRAPHICS_3D
#define MSM_BUS_MASTER_GRAPHICS_3D	26
#endif

#ifndef MSM_BUS_MASTER_VIDEO_P0
#define MSM_BUS_MASTER_VIDEO_P0		63
#endif

#ifndef MSM_BUS_SLAVE_EBI_CH0
#define MSM_BUS_SLAVE_EBI_CH0		512
#endif

#ifndef MSM_BUS_SLAVE_DISPLAY_CFG
#define MSM_BUS_SLAVE_DISPLAY_CFG	590
#endif

struct msm_bus_vectors {
	int src;
	int dst;
	uint64_t ab;
	uint64_t ib;
};

struct msm_bus_paths {
	int num_paths;
	struct msm_bus_vectors *vectors;
};

struct msm_bus_lat_vectors {
	uint64_t fal_ns;
	uint64_t idle_t_ns;
};

struct msm_bus_scale_pdata {
	struct msm_bus_paths *usecase;
	struct msm_bus_lat_vectors *usecase_lat;
	int num_usecases;
	const char *name;
	unsigned int active_only;
	unsigned int alc;
	/*
	 * The device the table was read from. The interconnect paths of
	 * the client are the ones of that device, so the votes of two
	 * clients do not overwrite each other.
	 */
	struct device *dev;
	/* Static and caller-allocated tables remain owned by their callers. */
	bool owned_by_shim;
};

#include <soc/qcom/tcs.h>

struct msm_bus_tcs_usecase {
	int num_cmds;
	struct tcs_cmd cmds[MSM_BUS_MAX_TCS_CMDS];
};

struct msm_bus_tcs_handle {
	int num_usecases;
	struct msm_bus_tcs_usecase *usecases;
};

struct msm_bus_scale_pdata *msm_bus_pdata_from_node(
		struct platform_device *pdev, struct device_node *node);
struct msm_bus_scale_pdata *msm_bus_cl_get_pdata(
		struct platform_device *pdev);
uint32_t msm_bus_scale_register_client(
		struct msm_bus_scale_pdata *pdata);
int msm_bus_scale_client_update_request(uint32_t cl, unsigned int index);
int msm_bus_scale_client_update_context(uint32_t cl, bool active_only,
						unsigned int ctx_idx);
void msm_bus_scale_unregister_client(uint32_t cl);
int msm_bus_scale_query_tcs_cmd_all(struct msm_bus_tcs_handle *hdl,
	uint32_t cl);

#endif
