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

/*
 * 6.12 removed iommu_domain_{get,set}_attr() and the vendor DOMAIN_ATTR_*
 * enum entries the 4.19 kgsl used. The placeholder values below just let
 * the code keep naming what it means. Every call site left is inert on
 * a506: gpuhtw LLC (msm8953 has no LLCC, so _enable_gpuhtw_llc() returns
 * early), the LLC/upstream-hint reads in _get_protection_flags() (same
 * reason), and the secure context in kgsl_gmu.c / _setup_secure_context()
 * (needs mmu->secured, off on this device). The secure and per-process
 * pagetable initialisers no longer call these at all. The wrappers fail
 * the call rather than pretend success.
 */
#define DOMAIN_ATTR_PROCID		60
#define DOMAIN_ATTR_CONTEXT_BANK	61
#define DOMAIN_ATTR_SECURE_VMID		62
#define DOMAIN_ATTR_DYNAMIC		63
#define DOMAIN_ATTR_USE_LLC_NWA		64
#define DOMAIN_ATTR_USE_UPSTREAM_HINT	65
#define DOMAIN_ATTR_TTBR0		66
#define DOMAIN_ATTR_CONTEXTIDR		67

static inline int iommu_domain_get_attr(struct iommu_domain *domain,
		int attr, void *data)
{
	return -ENODEV;
}

static inline int iommu_domain_set_attr(struct iommu_domain *domain,
		int attr, void *data)
{
	return -ENODEV;
}

static inline unsigned int cmd_db_read_aux_data_len(const char *res_id)
{
	size_t len = 0;
	const void *aux = cmd_db_read_aux_data(res_id, &len);

	return IS_ERR_OR_NULL(aux) ? 0 : (unsigned int)len;
}

#endif
