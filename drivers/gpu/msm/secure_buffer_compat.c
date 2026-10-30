// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, The tissot kgsl-port authors.
 *
 * hyp_assign_table() on top of the mainline qcom_scm_assign_mem() API.
 * Only the secure GPU memory paths in kgsl use it; -EAGAIN retries follow
 * the vendor implementation.
 */

#include <linux/bits.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/firmware/qcom/qcom_scm.h>

#include <soc/qcom/secure_buffer.h>

/*
 * The vendor secure_buffer.c serialises the assign calls on this mutex; the
 * SCM assign path shares state and must not be entered concurrently.
 */
static DEFINE_MUTEX(secure_buffer_mutex);

int hyp_assign_table(struct sg_table *table, int *source_vm_list,
		     int source_nelements, int *dest_vm_list, int *dest_perms,
		     int dest_nelements)
{
	struct qcom_scm_vmperm perm;
	struct scatterlist *sg;
	int ret = 0, i;

	if (!table || !table->sgl || !table->nents || !source_vm_list ||
	    !dest_vm_list || !dest_perms ||
	    source_nelements != 1 || dest_nelements != 1)
		return -EINVAL;

	if ((unsigned int)*source_vm_list >= 64 ||
	    (unsigned int)*dest_vm_list >= 32)
		return -EINVAL;

	perm.vmid = *dest_vm_list;
	perm.perm = *dest_perms;

	mutex_lock(&secure_buffer_mutex);

	for_each_sg(table->sgl, sg, table->nents, i) {
		u64 source = BIT_ULL(*source_vm_list);

		do {
			ret = qcom_scm_assign_mem(sg_phys(sg), sg->length,
						  &source, &perm, 1);
		} while (ret == -EAGAIN);

		/* Failed SCM calls can leave ownership unknown: never free it. */
		if (ret) {
			ret = -EADDRNOTAVAIL;
			break;
		}
	}

	mutex_unlock(&secure_buffer_mutex);

	return ret;
}
