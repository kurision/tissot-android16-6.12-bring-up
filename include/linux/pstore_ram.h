/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2010 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright (C) 2011 Kees Cook <keescook@chromium.org>
 * Copyright (C) 2011 Google, Inc.
 */

#ifndef __LINUX_PSTORE_RAM_H__
#define __LINUX_PSTORE_RAM_H__

#include <linux/pstore.h>
#include <linux/sizes.h>

struct persistent_ram_ecc_info {
	int block_size;
	int ecc_size;
	int symsize;
	int poly;
	uint16_t *par;
};

/*
 * Ramoops platform data
 * @mem_size	memory size for ramoops
 * @mem_address	physical memory address to contain ramoops
 */

#define RAMOOPS_FLAG_FTRACE_PER_CPU	BIT(0)

struct ramoops_platform_data {
	unsigned long	mem_size;
	phys_addr_t	mem_address;
	unsigned int	mem_type;
	unsigned long	record_size;
	unsigned long	console_size;
	unsigned long	ftrace_size;
	unsigned long	pmsg_size;
	int		max_reason;
	u32		flags;
	struct persistent_ram_ecc_info ecc_info;
};

/* DEBUG run117 (tissot): write a tag+value marker into 4K carved from
 * the ramoops dmesg area (between dmesg and console zones; the 1MB
 * reservation packs zones with zero slack). Works from any context
 * (no sleep, no VFS); slots are disjoint and idempotent so no
 * locking. The logdump snapshot copies the page every 2 s; a page
 * with valid magic also dumps at the next port boot's probe. The last
 * 8 bytes hold magic + boot id. Implemented in fs/pstore/ram.c;
 * out-of-range slots warn and drop.
 */
#define RAMOOPS_DIAG_MARK_SIZE	SZ_4K
#define RAMOOPS_DIAG_MARK_HDR	(RAMOOPS_DIAG_MARK_SIZE - 8)
#define RAMOOPS_DIAG_MARK_MAGIC	0x4B52414D	/* "MARK" */
void ramoops_diag_mark(loff_t slot, u32 tag, u32 val);

#endif
