// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2026, The tissot kgsl-port authors.
 *
 * interconnect-backed stand-in for the vendor msm-bus client API.
 * DT format is the vendor legacy one read by msm_bus_cl_get_pdata():
 * qcom,msm-bus,name, qcom,msm-bus,num-cases, qcom,msm-bus,num-paths,
 * qcom,msm-bus,vectors-KBps = <src dst ib ab>, ... (ab/ib in kBps).
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
	if (count != num_cases * num_paths)
		return -EINVAL;

	pdata->num_usecases = num_cases;
	pdata->usecase = kcalloc(num_cases, sizeof(*pdata->usecase),
			GFP_KERNEL);
	if (!pdata->usecase)
		return -ENOMEM;

	for (i = 0; i < num_cases; i++) {
		struct msm_bus_paths *paths = &pdata->usecase[i];

		paths->num_paths = num_paths;
		paths->vectors = kcalloc(num_paths, sizeof(*paths->vectors),
				GFP_KERNEL);
		if (!paths->vectors)
			return -ENOMEM;

		for (j = 0; j < num_paths; j++) {
			n = i * num_paths + j;
			if (of_property_read_u32_array_index(node,
					"qcom,msm-bus,vectors-KBps", n,
					vectors, 4)) {
				kfree(paths->vectors);
				return -EINVAL;
			}
			paths->vectors[j].src = vectors[0];
			paths->vectors[j].dst = vectors[1];
			paths->vectors[j].ib = vectors[2];
			paths->vectors[j].ab = vectors[3];
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
		bus_path = of_icc_get(&bus_pdev->dev, "gpu_icc_path");
		if (IS_ERR(bus_path)) {
			bus_path = NULL;
			if (!path_warned) {
				path_warned = true;
				dev_warn_ratelimited(&bus_pdev->dev,
					"no gpu_icc_path interconnect; bus votes accepted but not applied\n");
			}
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
	struct msm_bus_scale_pdata *pdata = (struct msm_bus_scale_pdata *)(uintptr_t)(cl & ~1UL);

	if (!pdata)
		return -EINVAL;

	if (index >= pdata->num_usecases)
		return -EINVAL;

	if (!bus_path)
		return 0;

	return icc_set_bw(bus_path,
			pdata->usecase[index].vectors[0].ab,
			pdata->usecase[index].vectors[0].ib);
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
