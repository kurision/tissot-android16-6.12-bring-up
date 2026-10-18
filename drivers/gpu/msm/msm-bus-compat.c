// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2026, The tissot kgsl-port authors.
 *
 * interconnect-backed stand-in for the vendor msm-bus client API.
 * DT format is the vendor legacy one read by msm_bus_cl_get_pdata():
 * qcom,msm-bus,name, qcom,msm-bus,num-cases, qcom,msm-bus,num-paths,
 * qcom,msm-bus,vectors-KBps = <src dst ab ib>, ... (ab/ib in kBps).
 * Vendor vector fields hold bytes/s; convert to kB/s at the ICC boundary.
 * Votes go to the DT path named "gpu_icc_path"; absent path = no-op.
 */

#include <linux/interconnect.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "msm-bus.h"

static struct platform_device *bus_pdev;
static struct icc_path *bus_path;
static bool path_warned;

#define MAX_CLIENTS 8
static struct msm_bus_scale_pdata *clients[MAX_CLIENTS];

static int parse_table(struct platform_device *pdev, struct device_node *node,
		struct msm_bus_scale_pdata *pdata)
{
	u32 vectors[4];
	int num_cases, num_paths, i, j, n, count;

	if (of_property_read_string(node, "qcom,msm-bus,name", &pdata->name))
		pdata->name = "kgsl";

	if (of_property_read_u32(node, "qcom,msm-bus,num-cases",
			&num_cases) || num_cases <= 0)
		return -EINVAL;

	if (of_property_read_u32(node, "qcom,msm-bus,num-paths",
			&num_paths) || num_paths <= 0)
		return -EINVAL;

	count = of_property_count_elems_of_size(node,
			"qcom,msm-bus,vectors-KBps", sizeof(u32) * 4);
	if (count < 0 || count % num_paths || count / num_paths != num_cases)
		return -EINVAL;

	pdata->usecase = kcalloc(num_cases, sizeof(*pdata->usecase),
			GFP_KERNEL);
	if (!pdata->usecase)
		return -ENOMEM;
	pdata->num_usecases = num_cases;

	for (i = 0; i < num_cases; i++) {
		struct msm_bus_paths *paths = &pdata->usecase[i];

		paths->num_paths = num_paths;
		paths->vectors = kcalloc(num_paths, sizeof(*paths->vectors),
				GFP_KERNEL);
		if (!paths->vectors)
			return -ENOMEM;

		for (j = 0; j < num_paths; j++) {
			int k, rc = 0;

			n = (i * num_paths + j) * 4;
			for (k = 0; k < 4; k++) {
				rc |= of_property_read_u32_index(node,
						"qcom,msm-bus,vectors-KBps",
						n + k, &vectors[k]);
				if (rc)
					break;
			}
			if (rc)
				return -EINVAL;
			paths->vectors[j].src = vectors[0];
			paths->vectors[j].dst = vectors[1];
			paths->vectors[j].ab = (u64)vectors[2] * 1000;
			paths->vectors[j].ib = (u64)vectors[3] * 1000;
		}
	}

	bus_pdev = pdev;
	return 0;
}

static void free_pdata(struct msm_bus_scale_pdata *pdata)
{
	int i;

	if (!pdata)
		return;

	for (i = 0; i < pdata->num_usecases; i++)
		kfree(pdata->usecase[i].vectors);
	kfree(pdata->usecase);
	kfree(pdata);
}

struct msm_bus_scale_pdata *msm_bus_cl_get_pdata(
		struct platform_device *pdev)
{
	struct msm_bus_scale_pdata *pdata;

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return NULL;

	if (parse_table(pdev, pdev->dev.of_node, pdata)) {
		free_pdata(pdata);
		return NULL;
	}

	return pdata;
}

struct msm_bus_scale_pdata *msm_bus_pdata_from_node(
		struct platform_device *pdev, struct device_node *node)
{
	struct msm_bus_scale_pdata *pdata;

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return NULL;

	if (parse_table(pdev, node, pdata)) {
		free_pdata(pdata);
		return NULL;
	}

	return pdata;
}

uint32_t msm_bus_scale_register_client(
		struct msm_bus_scale_pdata *pdata)
{
	if (!pdata)
		return 0;

	if (bus_pdev && !bus_path) {
		struct icc_path *path;

		path = of_icc_get(&bus_pdev->dev, "gpu_icc_path");
		if (IS_ERR(path)) {
			/* The legacy API can report failure only as a zero handle. */
			dev_err(&bus_pdev->dev,
				"gpu_icc_path interconnect lookup failed: %ld\n",
				PTR_ERR(path));
			return 0;
		}
		bus_path = path;
		if (!bus_path && !path_warned) {
			path_warned = true;
			dev_warn_ratelimited(&bus_pdev->dev,
					     "no gpu_icc_path interconnect; bus votes accepted but not applied\n");
		}
	}

	{
		int i;

		for (i = 0; i < MAX_CLIENTS; i++) {
			if (!clients[i]) {
				clients[i] = pdata;
				return i + 1;
			}
		}
	}

	return 0;
}

int msm_bus_scale_client_update_request(uint32_t cl, unsigned int index)
{
	struct msm_bus_scale_pdata *pdata;

	/*
	 * The vendor handle is a 1-based slot index into the client's own
	 * handle table (msm_bus_arb_adhoc.c gen_handle()/update_request_adhoc():
	 * handle_list.cl_list[cl]), not a tagged pointer. Decoding it as
	 * "cl & ~1UL" turned every handle into NULL for cl=1 and made each
	 * GPU power transition log "GPU BW scaling failure: -22".
	 */
	if (!cl || cl > MAX_CLIENTS)
		return -EINVAL;

	pdata = clients[cl - 1];
	if (!pdata)
		return -EINVAL;

	if (index >= pdata->num_usecases)
		return -EINVAL;

	if (!bus_path)
		return 0;

	return icc_set_bw(bus_path,
			Bps_to_icc(pdata->usecase[index].vectors[0].ab),
			Bps_to_icc(pdata->usecase[index].vectors[0].ib));
}

int msm_bus_scale_client_update_context(uint32_t cl, bool active_only,
						unsigned int ctx_idx)
{
	if (!cl || cl > MAX_CLIENTS || !clients[cl - 1])
		return -EINVAL;

	/* No interconnect path: voting is tuning, not functional. */
	if (!bus_path)
		return 0;

	return 0;
}

void msm_bus_scale_unregister_client(uint32_t cl)
{
	if (cl == 0 || cl > MAX_CLIENTS)
		return;

	free_pdata(clients[cl - 1]);
	clients[cl - 1] = NULL;
}

int msm_bus_scale_query_tcs_cmd_all(struct msm_bus_tcs_handle *hdl,
		uint32_t cl)
{
	if (hdl && hdl->usecases)
		memset(hdl->usecases, 0,
			hdl->num_usecases * sizeof(*hdl->usecases));

	return 0;
}
