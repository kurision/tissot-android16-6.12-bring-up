/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2018, 2020 The Linux Foundation. All rights reserved.
 * Copyright (c) 2026, The tissot kgsl-port authors.
 *
 * Minimal vendor secure-buffer API surface (VMIDs, PERM_* and the
 * hyp_assign_table prototype) built on the mainline qcom_scm_assign_mem().
 */
#ifndef __SOC_QCOM_SECURE_BUFFER_H
#define __SOC_QCOM_SECURE_BUFFER_H

#include <linux/scatterlist.h>
#include <linux/firmware/qcom/qcom_scm.h>

#ifndef VMID_HLOS
#define VMID_HLOS QCOM_SCM_VMID_HLOS
#endif

#ifndef VMID_CP_PIXEL
#define VMID_CP_PIXEL QCOM_SCM_VMID_CP_PIXEL
#endif

#ifndef PERM_READ
#define PERM_READ QCOM_SCM_PERM_READ
#endif

#ifndef PERM_WRITE
#define PERM_WRITE QCOM_SCM_PERM_WRITE
#endif

#ifndef PERM_EXEC
#define PERM_EXEC QCOM_SCM_PERM_EXEC
#endif

int hyp_assign_table(struct sg_table *table, int *source_vm_list,
		int source_nelements, int *dest_vm_list, int *dest_perms,
		int dest_nelements);

#endif
