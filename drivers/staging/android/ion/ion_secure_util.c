// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2020, The Linux Foundation. All rights reserved.
 *
 * Port note: there is no qcom secure-buffer/hyp transport on 6.12 ACK,
 * so every hyp assign/unassign entry point below is stubbed to -ENODEV.
 * Pure flag parsing (vmid lookup, vm list population, hlos accessibility)
 * is kept functional so secure allocations fail cleanly with -EINVAL or
 * -ENODEV instead of dereferencing NULL.
 */

#include <linux/printk.h>
#include "ion_secure_util.h"
#include "ion.h"

bool is_secure_vmid_valid(int vmid)
{
	return (vmid == VMID_CP_TOUCH ||
		vmid == VMID_CP_BITSTREAM ||
		vmid == VMID_CP_PIXEL ||
		vmid == VMID_CP_NON_PIXEL ||
		vmid == VMID_CP_CAMERA ||
		vmid == VMID_CP_SEC_DISPLAY ||
		vmid == VMID_CP_APP ||
		vmid == VMID_CP_CAMERA_PREVIEW ||
		vmid == VMID_CP_SPSS_SP ||
		vmid == VMID_CP_SPSS_SP_SHARED ||
		vmid == VMID_CP_SPSS_HLOS_SHARED ||
		vmid == VMID_CP_CDSP);
}

bool is_secure_allocation(unsigned long flags)
{
	return !!(flags & (ION_FLAGS_CP_MASK | ION_FLAG_SECURE));
}

int get_secure_vmid(unsigned long flags)
{
	if (flags & ION_FLAG_CP_TOUCH)
		return VMID_CP_TOUCH;
	if (flags & ION_FLAG_CP_BITSTREAM)
		return VMID_CP_BITSTREAM;
	if (flags & ION_FLAG_CP_PIXEL)
		return VMID_CP_PIXEL;
	if (flags & ION_FLAG_CP_NON_PIXEL)
		return VMID_CP_NON_PIXEL;
	if (flags & ION_FLAG_CP_CAMERA)
		return VMID_CP_CAMERA;
	if (flags & ION_FLAG_CP_SEC_DISPLAY)
		return VMID_CP_SEC_DISPLAY;
	if (flags & ION_FLAG_CP_APP)
		return VMID_CP_APP;
	if (flags & ION_FLAG_CP_CAMERA_PREVIEW)
		return VMID_CP_CAMERA_PREVIEW;
	if (flags & ION_FLAG_CP_SPSS_SP)
		return VMID_CP_SPSS_SP;
	if (flags & ION_FLAG_CP_SPSS_SP_SHARED)
		return VMID_CP_SPSS_SP_SHARED;
	if (flags & ION_FLAG_CP_SPSS_HLOS_SHARED)
		return VMID_CP_SPSS_HLOS_SHARED;
	if (flags & ION_FLAG_CP_CDSP)
		return VMID_CP_CDSP;
	return -EINVAL;
}

static int get_vmid(unsigned long flags)
{
	int vmid;

	vmid = get_secure_vmid(flags);
	if (vmid < 0) {
		if (flags & ION_FLAG_CP_HLOS)
			vmid = VMID_HLOS;
	}
	return vmid;
}

int ion_populate_vm_list(unsigned long flags, unsigned int *vm_list,
			 int nelems)
{
	unsigned int itr = 0;
	int vmid;

	flags = flags & ION_FLAGS_CP_MASK;
	if (!flags)
		return -EINVAL;

	for_each_set_bit(itr, &flags, BITS_PER_LONG) {
		vmid = get_vmid(0x1UL << itr);
		if (vmid < 0 || !nelems)
			return -EINVAL;

		vm_list[nelems - 1] = vmid;
		nelems--;
	}
	return 0;
}
EXPORT_SYMBOL(ion_populate_vm_list);

int ion_hyp_unassign_sg(struct sg_table *sgt, int *source_vm_list,
			int source_nelems, bool clear_page_private,
			bool try_lock)
{
	pr_info_once("ion: hyp unassign not backed on msm8953/6.12\n");
	return -ENODEV;
}

int ion_hyp_assign_sg(struct sg_table *sgt, int *dest_vm_list,
		      int dest_nelems, bool set_page_private)
{
	pr_info_once("ion: hyp assign not backed on msm8953/6.12\n");
	return -ENODEV;
}

int ion_hyp_unassign_sg_from_flags(struct sg_table *sgt, unsigned long flags,
				   bool set_page_private)
{
	pr_info_once("ion: hyp unassign not backed on msm8953/6.12\n");
	return -ENODEV;
}

int ion_hyp_assign_sg_from_flags(struct sg_table *sgt, unsigned long flags,
				 bool set_page_private)
{
	pr_info_once("ion: hyp assign not backed on msm8953/6.12\n");
	return -ENODEV;
}

bool hlos_accessible_buffer(struct ion_buffer *buffer)
{
	if ((buffer->flags & ION_FLAG_SECURE) &&
	    !(buffer->flags & ION_FLAG_CP_HLOS) &&
	    !(buffer->flags & ION_FLAG_CP_SPSS_HLOS_SHARED))
		return false;
	else if ((get_secure_vmid(buffer->flags) > 0) &&
		 !(buffer->flags & ION_FLAG_CP_HLOS) &&
		 !(buffer->flags & ION_FLAG_CP_SPSS_HLOS_SHARED))
		return false;

	return true;
}

int ion_hyp_assign_from_flags(u64 base, u64 size, unsigned long flags)
{
	pr_info_once("ion: hyp assign not backed on msm8953/6.12\n");
	return -ENODEV;
}
