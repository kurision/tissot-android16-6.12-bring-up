/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2010-2018, The Linux Foundation. All rights reserved.
 * Copyright (c) 2026, The tissot kgsl-port authors.
 *
 * Vendor msm-bus client API, verbatim struct/prototype surface from the
 * 4.19 donor (include/linux/msm-bus.h) so no driver file needed editing
 * beyond the include line. Implementation is interconnect-backed; when no
 * DT "gpu_icc_path" exists the votes are accepted as success and logged
 * once (bus voting is perf/power tuning, not functional).
 */
#ifndef _MSM_BUS_H_COMPAT
#define _MSM_BUS_H_COMPAT

#include <linux/types.h>
#include <linux/platform_device.h>

#define MSM_BUS_MAX_TCS_CMDS 16

#ifndef MSM_BUS_SLAVE_EBI_CH0
#define MSM_BUS_SLAVE_EBI_CH0 512
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
void msm_bus_scale_unregister_client(uint32_t cl);
int msm_bus_scale_query_tcs_cmd_all(struct msm_bus_tcs_handle *hdl,
		uint32_t cl);

#endif
