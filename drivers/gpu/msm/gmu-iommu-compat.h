/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026, The tissot kgsl-port authors.
 *
 * Vendor-only APIs used by the GMU (a6xx) files, which are compiled but
 * inert on this a506 device. Each shim is a dead-path stand-in so the
 * file builds; nothing here runs without GMU hardware.
 */

#ifndef _KGSL_GMU_COMPAT_H
#define _KGSL_GMU_COMPAT_H

#include <linux/iommu.h>
#include <linux/of_device.h>
#include <linux/err.h>
#include <soc/qcom/cmd-db.h>

#define IOMMU_FAULT_TRANSLATION		(1 << 8)
#define IOMMU_FAULT_PERMISSION		(1 << 9)
#define IOMMU_FAULT_EXTERNAL		(1 << 10)
#define IOMMU_FAULT_TRANSACTION_STALLED	(1 << 11)

#ifndef DOMAIN_ATTR_FAULT_MODEL_NO_STALL
#define DOMAIN_ATTR_FAULT_MODEL_NO_STALL 44
#endif

static inline int iommu_domain_set_attr(struct iommu_domain *domain,
		int attr, void *data)
{
	return 0;
}

static inline unsigned int cmd_db_read_aux_data_len(const char *res_id)
{
	size_t len = 0;
	const void *aux = cmd_db_read_aux_data(res_id, &len);

	return IS_ERR_OR_NULL(aux) ? 0 : (unsigned int)len;
}

#endif
