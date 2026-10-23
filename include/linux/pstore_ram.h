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

/* run225: durable + console note for a kernel-initiated restart. */
void ramoops_reset_note(const char *what);

/* Tissot diagnostic records in the reserved logdump partition. */
int ramoops_stage_write(unsigned int slot, const void *data, size_t len);

/* Diagnostic process-context wait for the first fully flushed flash snapshot. */
#if IS_BUILTIN(CONFIG_PSTORE_RAM) && IS_ENABLED(CONFIG_BLOCK)
int ramoops_wait_snapshot_ready(unsigned int timeout_ms);
#else
static inline int ramoops_wait_snapshot_ready(unsigned int timeout_ms)
{
	return -ENODEV;
}
#endif

/* Disjoint records in the existing Tissot diagnostic page, committed last. */
#define RAMOOPS_RESET_MARK_OFFSET	0x100
#define RAMOOPS_RESET_MARK_MAGIC	0x31545352	/* "RST1" */
enum ramoops_reset_path {
	RAMOOPS_RESET_PANIC = 1,
	RAMOOPS_RESET_EMERGENCY,
	RAMOOPS_RESET_PSCI,
	RAMOOPS_RESET_PSHOLD,
	RAMOOPS_RESET_PANIC_SNAPSHOT,
	RAMOOPS_RESET_PANIC_SNAPSHOT_DONE,
};

struct ramoops_reset_record {
	__le32 magic;
	__le32 version;
	__le32 path;
	__le32 boot_id;
	__le64 timestamp_ns;
	__le32 cpu;
	__le32 pid;
	char comm[16];
	char reason[200];
	__le32 crc32;
	__le32 reserved;
};

#if IS_BUILTIN(CONFIG_PSTORE_RAM) && IS_ENABLED(CONFIG_BLOCK)
void ramoops_reset_mark(enum ramoops_reset_path path, const char *reason);
#else
static inline void ramoops_reset_mark(enum ramoops_reset_path path,
				     const char *reason)
{
}
#endif

/*
 * run226: force one logdump snapshot to be committed from panic().
 *
 * Called from panic() after the panic text and stack trace are in the
 * printk log buffer but BEFORE smp_send_stop() freezes the sibling CPUs,
 * so the snapshot kthread can still run and drive eMMC I/O. Without this
 * the snapshotter freezes at the panic and the panic text is lost, which
 * is why every prior run ended with no durable evidence of the fault.
 *
 * Context: panic() only. IRQs disabled, preemption disabled, other CPUs
 * still running. Must not sleep, allocate, or printk-spin. Bounded.
 */
#if IS_BUILTIN(CONFIG_PSTORE_RAM) && IS_ENABLED(CONFIG_BLOCK)
void ramoops_panic_snapshot(void);
#else
static inline void ramoops_panic_snapshot(void)
{
}
#endif

#endif
