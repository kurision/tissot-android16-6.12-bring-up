// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2026, The tissot kgsl-port authors.
 *
 * interconnect-backed stand-in for the vendor msm-bus client API.
 * DT format is the vendor legacy one read by msm_bus_cl_get_pdata():
 * qcom,msm-bus,name, qcom,msm-bus,num-cases, qcom,msm-bus,num-paths,
 * qcom,msm-bus,vectors-KBps = <src dst ab ib>, ... (ab/ib in kBps).
 *
 * Every client votes on a path of its own. The paths are the ones of the
 * "interconnects" property of the device the table was read from, and the
 * vendor endpoint ids of the table pick which of them the client uses.
 * The vote is applied with icc_set_bw() on that path, so two clients
 * that share an endpoint add up instead of overwriting each other.
 *
 * A device without an "interconnects" property has no path: its votes
 * are accepted and dropped, because bus voting is tuning and not
 * function.
 */

#include <linux/interconnect.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <dt-bindings/interconnect/qcom,rpm-icc.h>

#include "msm-bus.h"

#define MAX_CLIENTS 16
#define MAX_PATHS 4

enum path_state {
	PATH_UNRESOLVED,	/* the path was not looked up yet */
	PATH_PENDING,		/* the interconnect provider is not there yet */
	PATH_READY,		/* the path is there */
	PATH_NONE,		/* the device has no interconnect path */
};

struct msm_bus_client {
	struct msm_bus_scale_pdata *pdata;
	struct device *dev;
	struct icc_path *paths[MAX_PATHS];
	enum path_state state;
	unsigned int cur_uc;	/* the usecase last applied */
	bool active_only;
};

static struct msm_bus_client clients[MAX_CLIENTS];

/*
 * The vendor tables name their endpoints with the msm-bus ids of the
 * donor. These are the paths of this SoC's interconnect they map to: the
 * name is an "interconnect-names" entry of the device the table was read
 * from, whose "interconnects" property holds the endpoints of the path.
 */
static const struct msm_bus_icc_map {
	int src;
	int dst;
	const char *name;
} icc_map[] = {
	{ MSM_BUS_MASTER_GRAPHICS_3D,	MSM_BUS_SLAVE_EBI_CH0,	"gpu-ddr" },
	{ MSM_BUS_MASTER_MDP_PORT0,	MSM_BUS_SLAVE_EBI_CH0,	"mdp-ddr" },
	{ MSM_BUS_MASTER_AMPSS_M0,	MSM_BUS_SLAVE_EBI_CH0,	"apps-ddr" },
	{ MSM_BUS_MASTER_AMPSS_M0,	MSM_BUS_SLAVE_DISPLAY_CFG, "display-cfg" },
};

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

	pdata->active_only = of_property_read_bool(node, "qcom,msm-bus,active-only");
	pdata->dev = &pdev->dev;
	pdata->owned_by_shim = true;

	return 0;
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

/*
 * Look the paths of the client up. The interconnect providers are
 * platform devices, so they may not have probed when the client
 * registers: the lookup is done again on every vote until it works.
 */
static int resolve_paths(struct msm_bus_client *client)
{
	struct msm_bus_scale_pdata *pdata = client->pdata;
	unsigned int i, j;

	for (i = 0; i < pdata->usecase[0].num_paths; i++) {
		struct icc_path *path;
		const char *name = NULL;

		if (client->paths[i])
			continue;

		for (j = 0; j < ARRAY_SIZE(icc_map); j++) {
			if (icc_map[j].src != pdata->usecase[0].vectors[i].src)
				continue;
			if (icc_map[j].dst != pdata->usecase[0].vectors[i].dst)
				continue;
			name = icc_map[j].name;
			break;
		}

		if (!name) {
			dev_warn_ratelimited(client->dev,
					     "no interconnect path for the endpoints %d and %d of %s\n",
					     pdata->usecase[0].vectors[i].src,
					     pdata->usecase[0].vectors[i].dst,
					     pdata->name);
			client->state = PATH_NONE;
			return -ENOENT;
		}

		path = of_icc_get(client->dev, name);
		if (IS_ERR(path)) {
			if (PTR_ERR(path) == -EPROBE_DEFER) {
				client->state = PATH_PENDING;
				return -EPROBE_DEFER;
			}

			dev_warn_ratelimited(client->dev,
					     "interconnect path %s lookup failed: %ld\n",
					     name, PTR_ERR(path));
			client->state = PATH_NONE;
			return PTR_ERR(path);
		}

		if (!path) {
			/* The device has no "interconnects" property. */
			dev_warn_ratelimited(client->dev,
					     "no interconnect path; bus votes accepted but not applied\n");
			client->state = PATH_NONE;
			return -ENODEV;
		}

		/*
		 * The tag of the path decides in which state of the system
		 * the request counts. The vendor table says the same with
		 * its "qcom,msm-bus,active-only" flag, and that one is
		 * about this client, so it wins over the default of the
		 * device tree.
		 */
		icc_set_tag(path,
			    client->active_only ? RPM_ACTIVE_TAG : RPM_ALWAYS_TAG);
		client->paths[i] = path;
	}

	client->state = PATH_READY;

	return 0;
}

/*
 * Apply the bandwidth of a usecase on the paths of the client.
 */
static int apply_bw(struct msm_bus_client *client, unsigned int index)
{
	struct msm_bus_scale_pdata *pdata = client->pdata;
	unsigned int i;
	int ret;

	for (i = 0; i < pdata->usecase[index].num_paths; i++) {
		if (!client->paths[i])
			continue;

		ret = icc_set_bw(client->paths[i],
				 Bps_to_icc(pdata->usecase[index].vectors[i].ab),
				  Bps_to_icc(pdata->usecase[index].vectors[i].ib));
		if (ret)
			return ret;
	}

	client->cur_uc = index;

	return 0;
}

uint32_t msm_bus_scale_register_client(
		struct msm_bus_scale_pdata *pdata)
{
	struct msm_bus_client *client;
	unsigned int i;

	if (!pdata)
		return 0;

	for (i = 0; i < MAX_CLIENTS; i++) {
		client = &clients[i];
		if (client->pdata)
			continue;

		memset(client, 0, sizeof(*client));
		client->pdata = pdata;
		client->dev = pdata->dev;
		client->active_only = pdata->active_only;
		client->state = PATH_UNRESOLVED;

		/*
		 * The lookup may fail with -EPROBE_DEFER when the
		 * interconnect provider has not probed yet. The vote tries
		 * again.
		 */
		resolve_paths(client);

		return i + 1;
	}

	return 0;
}

int msm_bus_scale_client_update_request(uint32_t cl, unsigned int index)
{
	struct msm_bus_client *client;
	struct msm_bus_scale_pdata *pdata;
	int ret;

	/*
	 * The vendor handle is a 1-based slot index into the client's own
	 * handle table (msm_bus_arb_adhoc.c gen_handle()/update_request_adhoc():
	 * handle_list.cl_list[cl]), not a tagged pointer. Decoding it as
	 * "cl & ~1UL" turned every handle into NULL for cl=1 and made each
	 * GPU power transition log "GPU BW scaling failure: -22".
	 */
	if (!cl || cl > MAX_CLIENTS)
		return -EINVAL;

	client = &clients[cl - 1];
	pdata = client->pdata;
	if (!pdata)
		return -EINVAL;

	if (index >= pdata->num_usecases)
		return -EINVAL;

	if (client->state == PATH_UNRESOLVED ||
	    client->state == PATH_PENDING) {
		ret = resolve_paths(client);
		if (ret == -EPROBE_DEFER) {
			/*
			 * The provider is not there yet; try again on the
			 * next vote. fw_devlink holds back the probe of a
			 * device whose "interconnects" property names a
			 * provider that has not probed
			 * (drivers/of/property.c parse_interconnects), so
			 * this only happens when fw_devlink is off or has
			 * given up on the link.
			 */
			return 0;
		}
	}

	if (client->state != PATH_READY)
		return 0;

	return apply_bw(client, index);
}

int msm_bus_scale_client_update_context(uint32_t cl, bool active_only,
						unsigned int ctx_idx)
{
	struct msm_bus_client *client;
	struct msm_bus_scale_pdata *pdata;
	unsigned int i;

	if (!cl || cl > MAX_CLIENTS)
		return -EINVAL;

	client = &clients[cl - 1];
	pdata = client->pdata;
	if (!pdata)
		return -EINVAL;

	client->active_only = active_only;

	if (client->state != PATH_READY)
		return 0;

	/*
	 * The vendor API moves the request between the "active only" and
	 * the "active and sleep" contexts. The interconnect framework
	 * expresses the same with the tag of the path: a request tagged
	 * RPM_ACTIVE_TAG counts in the active state only.
	 */
	if (ctx_idx >= pdata->num_usecases)
		ctx_idx = client->cur_uc;

	for (i = 0; i < pdata->usecase[ctx_idx].num_paths; i++) {
		if (!client->paths[i])
			continue;

		icc_set_tag(client->paths[i],
			    active_only ? RPM_ACTIVE_TAG : RPM_ALWAYS_TAG);
	}

	return apply_bw(client, ctx_idx);
}

void msm_bus_scale_unregister_client(uint32_t cl)
{
	struct msm_bus_client *client;
	unsigned int i;

	if (cl == 0 || cl > MAX_CLIENTS)
		return;

	client = &clients[cl - 1];

	for (i = 0; i < ARRAY_SIZE(client->paths); i++) {
		icc_put(client->paths[i]);
		client->paths[i] = NULL;
	}

	if (client->pdata && client->pdata->owned_by_shim)
		free_pdata(client->pdata);
	memset(client, 0, sizeof(*client));
}

int msm_bus_scale_query_tcs_cmd_all(struct msm_bus_tcs_handle *hdl,
		uint32_t cl)
{
	if (hdl && hdl->usecases)
		memset(hdl->usecases, 0,
			hdl->num_usecases * sizeof(*hdl->usecases));

	return 0;
}
