/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2017-2018,2020, The Linux Foundation. All rights reserved.
 */

#include "ion.h"

#ifndef _ION_SECURE_UTIL_H
#define _ION_SECURE_UTIL_H

/*
 * TrustZone VMID ABI values copied from the donor QCOM secure-buffer
 * header. There is no secure-buffer/hyp transport on 6.12 ACK, so these
 * are used only for ION flag parsing; every hyp assign/unassign call
 * below is stubbed to -ENODEV in ion_secure_util.c.
 */
enum vmid {
	VMID_HLOS = 0x3,
	VMID_CP_TOUCH = 0x8,
	VMID_CP_BITSTREAM = 0x9,
	VMID_CP_PIXEL = 0xA,
	VMID_CP_NON_PIXEL = 0xB,
	VMID_CP_CAMERA = 0xD,
	VMID_HLOS_FREE = 0xE,
	VMID_MSS_MSA = 0xF,
	VMID_MSS_NONMSA = 0x10,
	VMID_CP_SEC_DISPLAY = 0x11,
	VMID_CP_APP = 0x12,
	VMID_WLAN = 0x18,
	VMID_WLAN_CE = 0x19,
	VMID_CP_SPSS_SP = 0x1A,
	VMID_CP_CAMERA_PREVIEW = 0x1D,
	VMID_CP_SPSS_SP_SHARED = 0x22,
	VMID_CP_SPSS_HLOS_SHARED = 0x24,
	VMID_CP_CDSP = 0x2A,
	VMID_NAV = 0x2B,
	VMID_LAST,
	VMID_INVAL = -1
};

int get_secure_vmid(unsigned long flags);
bool is_secure_vmid_valid(int vmid);
int ion_hyp_assign_sg(struct sg_table *sgt, int *dest_vm_list,
		      int dest_nelems, bool set_page_private);
int ion_hyp_unassign_sg(struct sg_table *sgt, int *source_vm_list,
			int source_nelems, bool clear_page_private,
			bool try_lock);
int ion_hyp_unassign_sg_from_flags(struct sg_table *sgt, unsigned long flags,
				   bool set_page_private);
int ion_hyp_assign_sg_from_flags(struct sg_table *sgt, unsigned long flags,
				 bool set_page_private);
int ion_hyp_assign_from_flags(u64 base, u64 size, unsigned long flags);

bool hlos_accessible_buffer(struct ion_buffer *buffer);

bool is_secure_allocation(unsigned long flags);

#endif /* _ION_SECURE_UTIL_H */
