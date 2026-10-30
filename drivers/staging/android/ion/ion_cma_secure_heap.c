// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) Linaro 2012
 * Author: <benjamin.gaignard@linaro.org> for ST-Ericsson.
 * Copyright (c) 2013-2018, The Linux Foundation. All rights reserved.
 *
 * Port note: the secure-CMA backend needs the proprietary QCOM
 * secure-table/hyp-assign transport, the long-removed dma_alloc_attrs()
 * API, donor-only trace events, and the old embedded-shrinker API.
 * None of that exists on 6.12 ACK, so the heap is
 * compiled down to a create stub: ION_IOC_ALLOC with the secure-DMA heap
 * mask fails with -ENODEV instead of oopsing. Heaps 27/19 are served by
 * the regular CMA heap or fail the same way; they are never remapped onto
 * the system heap.
 */

#include <linux/err.h>
#include <linux/printk.h>

#include "ion.h"

#ifdef CONFIG_CMA
struct ion_heap *ion_secure_cma_heap_create(struct ion_platform_heap *data)
{
	pr_info_once("ion: secure-cma not backed on msm8953/6.12\n");
	return ERR_PTR(-ENODEV);
}

void ion_secure_cma_heap_destroy(struct ion_heap *heap)
{
}
#endif
