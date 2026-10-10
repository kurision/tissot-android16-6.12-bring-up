// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, The tissot kgsl-port authors.
 *
 * hyp_assign_table() on top of the mainline qcom_scm_assign_mem() API.
 * Only the secure GPU memory paths in kgsl use it; -EAGAIN retries follow
 * the vendor implementation.
 */

#include <linux/kernel.h>
#include <linux/scatterlist.h>
#include <linux/firmware/qcom/qcom_scm.h>

#include <soc/qcom/secure_buffer.h>

int hyp_assign_table(struct sg_table *table, int *source_vm_list,
		int source_nelements, int *dest_vm_list, int *dest_perms,
		int dest_nelements)
{
	struct qcom_scm_vmperm perm;
	struct scatterlist *sg;
	int ret = 0, i;

	if (source_nelements != 1 || dest_nelements != 1)
		return -EINVAL;

	perm.vmid = *dest_vm_list;
	perm.perm = *dest_perms;

	for_each_sg(table->sgl, sg, table->nents, i) {
		do {
			ret = qcom_scm_assign_mem(sg_phys(sg), sg->length,
					(u64 *)source_vm_list, &perm, 1);
		} while (ret == -EAGAIN);

		if (ret)
			break;
	}

	return ret;
}
