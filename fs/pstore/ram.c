// SPDX-License-Identifier: GPL-2.0-only
/*
 * RAM Oops/Panic logger
 *
 * Copyright (C) 2010 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright (C) 2011 Kees Cook <keescook@chromium.org>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/pstore.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/compiler.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/mm.h>
#include <linux/cred.h>
#include <linux/kobject.h>
#include <linux/sizes.h>
#include <linux/sysfs.h>
#if IS_BUILTIN(CONFIG_PSTORE_RAM) && IS_ENABLED(CONFIG_BLOCK)
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/completion.h>
#include <linux/device/class.h>
#include <linux/kmsg_dump.h>
#include <linux/crc32.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/sched/signal.h>
#include <linux/interrupt.h>
#include <linux/irqflags.h>
#endif
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/timekeeping.h>
#include <linux/vmalloc.h>
#include <linux/random.h>
#include <linux/delay.h>
#include <linux/smp.h>

#include "internal.h"
#include "ram_internal.h"

#define RAMOOPS_KERNMSG_HDR "===="
#define MIN_MEM_SIZE 4096UL

static ulong record_size = MIN_MEM_SIZE;
module_param(record_size, ulong, 0400);
MODULE_PARM_DESC(record_size,
		"size of each dump done on oops/panic");

static ulong ramoops_console_size = MIN_MEM_SIZE;
module_param_named(console_size, ramoops_console_size, ulong, 0400);
MODULE_PARM_DESC(console_size, "size of kernel console log");

static ulong ramoops_ftrace_size = MIN_MEM_SIZE;
module_param_named(ftrace_size, ramoops_ftrace_size, ulong, 0400);
MODULE_PARM_DESC(ftrace_size, "size of ftrace log");

static ulong ramoops_pmsg_size = MIN_MEM_SIZE;
module_param_named(pmsg_size, ramoops_pmsg_size, ulong, 0400);
MODULE_PARM_DESC(pmsg_size, "size of user space message log");

static unsigned long long mem_address;
module_param_hw(mem_address, ullong, other, 0400);
MODULE_PARM_DESC(mem_address,
		"start of reserved RAM used to store oops/panic logs");

static char *mem_name;
module_param_named(mem_name, mem_name, charp, 0400);
MODULE_PARM_DESC(mem_name, "name of kernel param that holds addr");

static ulong mem_size;
module_param(mem_size, ulong, 0400);
MODULE_PARM_DESC(mem_size,
		"size of reserved RAM used to store oops/panic logs");

static unsigned int mem_type;
module_param(mem_type, uint, 0400);
MODULE_PARM_DESC(mem_type,
		"memory type: 0=write-combined (default), 1=unbuffered, 2=cached");

static int ramoops_max_reason = -1;
module_param_named(max_reason, ramoops_max_reason, int, 0400);
MODULE_PARM_DESC(max_reason,
		 "maximum reason for kmsg dump (default 2: Oops and Panic) ");

static int ramoops_ecc;
module_param_named(ecc, ramoops_ecc, int, 0400);
MODULE_PARM_DESC(ramoops_ecc,
		"if non-zero, the option enables ECC support and specifies "
		"ECC buffer size in bytes (1 is a special value, means 16 "
		"bytes ECC)");

static int ramoops_dump_oops = -1;
module_param_named(dump_oops, ramoops_dump_oops, int, 0400);
MODULE_PARM_DESC(dump_oops,
		 "(deprecated: use max_reason instead) set to 1 to dump oopses & panics, 0 to only dump panics");

struct ramoops_context {
	struct persistent_ram_zone **dprzs;	/* Oops dump zones */
	struct persistent_ram_zone *cprz;	/* Console zone */
	struct persistent_ram_zone **fprzs;	/* Ftrace zones */
	struct persistent_ram_zone *mprz;	/* PMSG zone */
	phys_addr_t phys_addr;
	unsigned long size;
	unsigned int memtype;
	size_t record_size;
	size_t console_size;
	size_t ftrace_size;
	size_t pmsg_size;
	u32 flags;
	struct persistent_ram_ecc_info ecc_info;
	unsigned int max_dump_cnt;
	unsigned int dump_write_cnt;
	/* _read_cnt need clear on ramoops_pstore_open */
	unsigned int dump_read_cnt;
	unsigned int console_read_cnt;
	unsigned int max_ftrace_cnt;
	unsigned int ftrace_read_cnt;
	unsigned int pmsg_read_cnt;
	struct pstore_info pstore;
	bool raw_published;
};

static struct platform_device *dummy;

static int ramoops_pstore_open(struct pstore_info *psi)
{
	struct ramoops_context *cxt = psi->data;

	cxt->dump_read_cnt = 0;
	cxt->console_read_cnt = 0;
	cxt->ftrace_read_cnt = 0;
	cxt->pmsg_read_cnt = 0;
	return 0;
}

static struct persistent_ram_zone *
ramoops_get_next_prz(struct persistent_ram_zone *przs[], int id,
		     struct pstore_record *record)
{
	struct persistent_ram_zone *prz;

	/* Give up if we never existed or have hit the end. */
	if (!przs)
		return NULL;

	prz = przs[id];
	if (!prz)
		return NULL;

	/* Update old/shadowed buffer. */
	if (prz->type == PSTORE_TYPE_DMESG)
		persistent_ram_save_old(prz);

	if (!persistent_ram_old_size(prz))
		return NULL;

	record->type = prz->type;
	record->id = id;

	return prz;
}

static int ramoops_read_kmsg_hdr(char *buffer, struct timespec64 *time,
				  bool *compressed)
{
	char data_type;
	int header_length = 0;

	if (sscanf(buffer, RAMOOPS_KERNMSG_HDR "%lld.%lu-%c\n%n",
		   (time64_t *)&time->tv_sec, &time->tv_nsec, &data_type,
		   &header_length) == 3) {
		time->tv_nsec *= 1000;
		if (data_type == 'C')
			*compressed = true;
		else
			*compressed = false;
	} else if (sscanf(buffer, RAMOOPS_KERNMSG_HDR "%lld.%lu\n%n",
			  (time64_t *)&time->tv_sec, &time->tv_nsec,
			  &header_length) == 2) {
		time->tv_nsec *= 1000;
		*compressed = false;
	} else {
		time->tv_sec = 0;
		time->tv_nsec = 0;
		*compressed = false;
	}
	return header_length;
}

static bool prz_ok(struct persistent_ram_zone *prz)
{
	return !!prz && !!(persistent_ram_old_size(prz) +
			   persistent_ram_ecc_string(prz, NULL, 0));
}

static ssize_t ramoops_pstore_read(struct pstore_record *record)
{
	ssize_t size = 0;
	struct ramoops_context *cxt = record->psi->data;
	struct persistent_ram_zone *prz = NULL;
	int header_length = 0;
	bool free_prz = false;

	/*
	 * Ramoops headers provide time stamps for PSTORE_TYPE_DMESG, but
	 * PSTORE_TYPE_CONSOLE and PSTORE_TYPE_FTRACE don't currently have
	 * valid time stamps, so it is initialized to zero.
	 */
	record->time.tv_sec = 0;
	record->time.tv_nsec = 0;
	record->compressed = false;

	/* Find the next valid persistent_ram_zone for DMESG */
	while (cxt->dump_read_cnt < cxt->max_dump_cnt && !prz) {
		prz = ramoops_get_next_prz(cxt->dprzs, cxt->dump_read_cnt++,
					   record);
		if (!prz_ok(prz))
			continue;
		header_length = ramoops_read_kmsg_hdr(persistent_ram_old(prz),
						      &record->time,
						      &record->compressed);
		/* Clear and skip this DMESG record if it has no valid header */
		if (!header_length) {
			persistent_ram_free_old(prz);
			persistent_ram_zap(prz);
			prz = NULL;
		}
	}

	if (!prz_ok(prz) && !cxt->console_read_cnt++)
		prz = ramoops_get_next_prz(&cxt->cprz, 0 /* single */, record);

	if (!prz_ok(prz) && !cxt->pmsg_read_cnt++)
		prz = ramoops_get_next_prz(&cxt->mprz, 0 /* single */, record);

	/* ftrace is last since it may want to dynamically allocate memory. */
	if (!prz_ok(prz)) {
		if (!(cxt->flags & RAMOOPS_FLAG_FTRACE_PER_CPU) &&
		    !cxt->ftrace_read_cnt++) {
			prz = ramoops_get_next_prz(cxt->fprzs, 0 /* single */,
						   record);
		} else {
			/*
			 * Build a new dummy record which combines all the
			 * per-cpu records including metadata and ecc info.
			 */
			struct persistent_ram_zone *tmp_prz, *prz_next;

			tmp_prz = kzalloc(sizeof(struct persistent_ram_zone),
					  GFP_KERNEL);
			if (!tmp_prz)
				return -ENOMEM;
			prz = tmp_prz;
			free_prz = true;

			while (cxt->ftrace_read_cnt < cxt->max_ftrace_cnt) {
				prz_next = ramoops_get_next_prz(cxt->fprzs,
						cxt->ftrace_read_cnt++, record);

				if (!prz_ok(prz_next))
					continue;

				tmp_prz->ecc_info = prz_next->ecc_info;
				tmp_prz->corrected_bytes +=
						prz_next->corrected_bytes;
				tmp_prz->bad_blocks += prz_next->bad_blocks;

				size = pstore_ftrace_combine_log(
						&tmp_prz->old_log,
						&tmp_prz->old_log_size,
						prz_next->old_log,
						prz_next->old_log_size);
				if (size)
					goto out;
			}
			record->id = 0;
		}
	}

	if (!prz_ok(prz)) {
		size = 0;
		goto out;
	}

	size = persistent_ram_old_size(prz) - header_length;

	/* ECC correction notice */
	record->ecc_notice_size = persistent_ram_ecc_string(prz, NULL, 0);

	record->buf = kvzalloc(size + record->ecc_notice_size + 1, GFP_KERNEL);
	if (record->buf == NULL) {
		size = -ENOMEM;
		goto out;
	}

	memcpy(record->buf, (char *)persistent_ram_old(prz) + header_length,
	       size);

	persistent_ram_ecc_string(prz, record->buf + size,
				  record->ecc_notice_size + 1);

out:
	if (free_prz) {
		kvfree(prz->old_log);
		kfree(prz);
	}

	return size;
}

static size_t ramoops_write_kmsg_hdr(struct persistent_ram_zone *prz,
				     struct pstore_record *record)
{
	char hdr[36]; /* "===="(4), %lld(20), "."(1), %06lu(6), "-%c\n"(3) */
	size_t len;

	len = scnprintf(hdr, sizeof(hdr),
		RAMOOPS_KERNMSG_HDR "%lld.%06lu-%c\n",
		(time64_t)record->time.tv_sec,
		record->time.tv_nsec / 1000,
		record->compressed ? 'C' : 'D');
	persistent_ram_write(prz, hdr, len);

	return len;
}

static int notrace ramoops_pstore_write(struct pstore_record *record)
{
	struct ramoops_context *cxt = record->psi->data;
	struct persistent_ram_zone *prz;
	size_t size, hlen;

	if (record->type == PSTORE_TYPE_CONSOLE) {
		if (!cxt->cprz)
			return -ENOMEM;
		persistent_ram_write(cxt->cprz, record->buf, record->size);
		return 0;
	} else if (record->type == PSTORE_TYPE_FTRACE) {
		int zonenum;

		if (!cxt->fprzs)
			return -ENOMEM;
		/*
		 * Choose zone by if we're using per-cpu buffers.
		 */
		if (cxt->flags & RAMOOPS_FLAG_FTRACE_PER_CPU)
			zonenum = smp_processor_id();
		else
			zonenum = 0;

		persistent_ram_write(cxt->fprzs[zonenum], record->buf,
				     record->size);
		return 0;
	} else if (record->type == PSTORE_TYPE_PMSG) {
		pr_warn_ratelimited("PMSG shouldn't call %s\n", __func__);
		return -EINVAL;
	}

	if (record->type != PSTORE_TYPE_DMESG)
		return -EINVAL;

	/*
	 * We could filter on record->reason here if we wanted to (which
	 * would duplicate what happened before the "max_reason" setting
	 * was added), but that would defeat the purpose of a system
	 * changing printk.always_kmsg_dump, so instead log everything that
	 * the kmsg dumper sends us, since it should be doing the filtering
	 * based on the combination of printk.always_kmsg_dump and our
	 * requested "max_reason".
	 */

	/*
	 * Explicitly only take the first part of any new crash.
	 * If our buffer is larger than kmsg_bytes, this can never happen,
	 * and if our buffer is smaller than kmsg_bytes, we don't want the
	 * report split across multiple records.
	 */
	if (record->part != 1)
		return -ENOSPC;

	if (!cxt->dprzs)
		return -ENOSPC;

	prz = cxt->dprzs[cxt->dump_write_cnt];

	/*
	 * Since this is a new crash dump, we need to reset the buffer in
	 * case it still has an old dump present. Without this, the new dump
	 * will get appended, which would seriously confuse anything trying
	 * to check dump file contents. Specifically, ramoops_read_kmsg_hdr()
	 * expects to find a dump header in the beginning of buffer data, so
	 * we must to reset the buffer values, in order to ensure that the
	 * header will be written to the beginning of the buffer.
	 */
	persistent_ram_zap(prz);

	/* Build header and append record contents. */
	hlen = ramoops_write_kmsg_hdr(prz, record);
	if (!hlen)
		return -ENOMEM;

	size = record->size;
	if (size + hlen > prz->buffer_size)
		size = prz->buffer_size - hlen;
	persistent_ram_write(prz, record->buf, size);

	cxt->dump_write_cnt = (cxt->dump_write_cnt + 1) % cxt->max_dump_cnt;

	return 0;
}

static int notrace ramoops_pstore_write_user(struct pstore_record *record,
					     const char __user *buf)
{
	if (record->type == PSTORE_TYPE_PMSG) {
		struct ramoops_context *cxt = record->psi->data;

		if (!cxt->mprz)
			return -ENOMEM;
		return persistent_ram_write_user(cxt->mprz, buf, record->size);
	}

	return -EINVAL;
}

static int ramoops_pstore_erase(struct pstore_record *record)
{
	struct ramoops_context *cxt = record->psi->data;
	struct persistent_ram_zone *prz;

	switch (record->type) {
	case PSTORE_TYPE_DMESG:
		if (record->id >= cxt->max_dump_cnt)
			return -EINVAL;
		prz = cxt->dprzs[record->id];
		break;
	case PSTORE_TYPE_CONSOLE:
		prz = cxt->cprz;
		break;
	case PSTORE_TYPE_FTRACE:
		if (record->id >= cxt->max_ftrace_cnt)
			return -EINVAL;
		prz = cxt->fprzs[record->id];
		break;
	case PSTORE_TYPE_PMSG:
		prz = cxt->mprz;
		break;
	default:
		return -EINVAL;
	}

	persistent_ram_free_old(prz);
	persistent_ram_zap(prz);

	return 0;
}

static struct ramoops_context oops_cxt = {
	.pstore = {
		.owner	= THIS_MODULE,
		.name	= "ramoops",
		.open	= ramoops_pstore_open,
		.read	= ramoops_pstore_read,
		.write	= ramoops_pstore_write,
		.write_user	= ramoops_pstore_write_user,
		.erase	= ramoops_pstore_erase,
	},
};

#define RAMOOPS_RAW_PHYS 0x86800000ULL
#define RAMOOPS_RAW_CONSOLE 0x77000
/* 119 4K zones before run117; the last is now the diag mark page. */
#define RAMOOPS_RAW_DMESG_CNT 118
#define RAMOOPS_RAW_PMSG 0xf7000
#define RAMOOPS_RAW_FTRACE 0xff000

static void __iomem *ramoops_diag_marks;
static phys_addr_t ramoops_diag_phys;

static bool ramoops_raw_zone_valid(struct persistent_ram_zone *prz,
				  size_t offset, size_t size)
{
	return prz && prz->vaddr && prz->size == size &&
		prz->paddr == RAMOOPS_RAW_PHYS + offset;
}

static bool ramoops_raw_geometry_valid(struct ramoops_context *cxt)
{
	unsigned int i;

	if (PAGE_SHIFT != 12 ||
	    cxt->phys_addr != RAMOOPS_RAW_PHYS || cxt->size != SZ_1M ||
	    cxt->record_size != SZ_4K || cxt->console_size != SZ_512K ||
	    cxt->pmsg_size != SZ_32K || cxt->ftrace_size != SZ_4K ||
	    cxt->max_dump_cnt != RAMOOPS_RAW_DMESG_CNT ||
	    cxt->max_ftrace_cnt != 1 || !ramoops_diag_marks ||
	    ramoops_diag_phys != RAMOOPS_RAW_PHYS +
				 RAMOOPS_RAW_DMESG_CNT * SZ_4K ||
	    !cxt->dprzs || !cxt->fprzs || cxt->ecc_info.ecc_size ||
	    (cxt->flags & RAMOOPS_FLAG_FTRACE_PER_CPU))
		return false;
	for (i = 0; i < RAMOOPS_RAW_DMESG_CNT; i++)
		if (!ramoops_raw_zone_valid(cxt->dprzs[i], i * SZ_4K, SZ_4K))
			return false;
	return ramoops_raw_zone_valid(cxt->cprz, RAMOOPS_RAW_CONSOLE, SZ_512K) &&
		ramoops_raw_zone_valid(cxt->mprz, RAMOOPS_RAW_PMSG, SZ_32K) &&
		ramoops_raw_zone_valid(cxt->fprzs[0], RAMOOPS_RAW_FTRACE, SZ_4K);
}


/* Copy live, non-atomic bytes, including each zone's persistent-RAM header. */
static ssize_t ramoops_raw_copy(struct ramoops_context *cxt, char *buf,
				size_t off, size_t count)
{
	size_t copied = 0;

	while (copied < count) {
		struct persistent_ram_zone *prz;
		size_t pos = off + copied;
		size_t start, size, n;

		if (pos < RAMOOPS_RAW_CONSOLE) {
			unsigned int index = pos / SZ_4K;

			/* The diag mark page sits right after the last
			 * dmesg zone; snapshots must carry it to logdump.
			 */
			if (index == cxt->max_dump_cnt && ramoops_diag_marks) {
				start = index * SZ_4K;
				n = min(count - copied,
					RAMOOPS_DIAG_MARK_SIZE - (pos - start));
				memcpy_fromio(buf + copied,
					      ramoops_diag_marks + (pos - start), n);
				copied += n;
				continue;
			}
			if (!cxt->dprzs || index >= cxt->max_dump_cnt)
				return -EIO;
			prz = cxt->dprzs[index];
			start = index * SZ_4K;
			size = SZ_4K;
		} else if (pos < RAMOOPS_RAW_PMSG) {
			prz = cxt->cprz;
			start = RAMOOPS_RAW_CONSOLE;
			size = SZ_512K;
		} else if (pos < RAMOOPS_RAW_FTRACE) {
			prz = cxt->mprz;
			start = RAMOOPS_RAW_PMSG;
			size = SZ_32K;
		} else {
			if (!cxt->fprzs || cxt->max_ftrace_cnt != 1)
				return -EIO;
			prz = cxt->fprzs[0];
			start = RAMOOPS_RAW_FTRACE;
			size = SZ_4K;
		}
		if (!ramoops_raw_zone_valid(prz, start, size) ||
		    pos - start >= size)
			return -EIO;
		n = min(count - copied, size - (pos - start));
		memcpy_fromio(buf + copied,
			      (void __iomem *)prz->vaddr + (pos - start), n);
		copied += n;
	}
	return copied;
}

static ssize_t ramoops_raw_read(struct file *file, struct kobject *kobj,
			       struct bin_attribute *attr, char *buf,
			       loff_t off, size_t count)
{
	(void)file;
	(void)kobj;
	(void)attr;
	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID))
		return -EACCES;
	if (off < 0)
		return -EINVAL;
	if (off >= SZ_1M)
		return 0;
	return ramoops_raw_copy(&oops_cxt, buf, off,
				min_t(size_t, count, SZ_1M - (size_t)off));
}

static BIN_ATTR_ADMIN_RO(ramoops_raw, SZ_1M);

/* Partition diagnostics require the non-exported built-in block class. */
#if IS_BUILTIN(CONFIG_PSTORE_RAM) && IS_ENABLED(CONFIG_BLOCK)

/*
 * Kernel-side periodic snapshot of the live reservation.
 *
 * The userspace sampler dies with the SELinux domain transition, so this
 * keeps copying the 1 MiB reservation to the "logdump" block device
 * without any process.
 *
 * Two slots alternate, so a freeze cannot destroy every sample: the slot
 * being written is overwritten two samples later, leaving the previous
 * sample intact. A single slot was proven to miss a cutoff by ~1 s.
 *
 * Each slot is one reserved page (header in its first 64 bytes) followed
 * by the 1048576 raw ramoops bytes:
 *
 *   slot 0  header 62914560..62918655  payload 62918656..63967231
 *   slot 1  header 63967232..63971327  payload 63971328..65019903
 *
 * Both sit inside the 64 MiB partition, above the growing kmsg text
 * region. The payload starts a page in because the whole snapshot is
 * written from vmalloc pages, which bio_add_page() requires.
 */
#define RAMOOPS_SNAP_OFFSET		62914560ULL
#define RAMOOPS_SNAP_HDR_SIZE		64
#define RAMOOPS_SNAP_PAYLOAD_OFFSET	(RAMOOPS_SNAP_OFFSET + PAGE_SIZE)
#define RAMOOPS_SNAP_SLOT_STRIDE	(PAGE_SIZE + SZ_1M)
#define RAMOOPS_SNAP_SLOTS		2
#define RAMOOPS_SNAP_INTERVAL_MS	2000
/*
 * 0 since 2026-10-24: the dim-screen boot-death era this monitored is closed,
 * and the 2026-10-23 camera diagnosis no longer needs a pre-probe flush (its
 * JPEG failures show in dmesg). Explicit panic capture and orderly-reset
 * tails remain; with this 0 the camera late-init gates are released at
 * snapshotter start instead of waiting for a flush that never comes.
 */
#define RAMOOPS_PERIODIC_SNAPSHOTS	0
/*
 * run225: once the first KGSL stage record lands, shorten the cadence so the
 * last sample falls inside the pre-reset blind window (~150 ms wide). The
 * 1 MiB console payload makes this ~5 MiB/s, so it is bounded to the period
 * between the GPU starting and the reset rather than the whole boot.
 */
#define RAMOOPS_SNAP_FAST_MS		200

/*
 * run226: bounded busy-wait used by ramoops_panic_snapshot(). A flushed
 * 1 MiB console sample plus the tail commit normally completes in tens of
 * milliseconds (the 41.7 s and 41.77 s headers in run225 are ~56 ms apart),
 * so 4 s of budget is generous. The cap only bounds the panic path; it is
 * never expected to be reached.
 */
#define RAMOOPS_PANIC_SNAP_UDELAY	100	/* us per spin */
#define RAMOOPS_PANIC_SNAP_SPINS	40000	/* => up to ~4 s */

/*
 * run226: consecutive commit failures tolerated before the snapshotter
 * disables itself. High enough to ride out a transient eMMC error, low
 * enough that a permanently wedged logdump does not spin forever.
 */
#define RAMOOPS_SNAP_MAX_FAILURES	10
#define RAMOOPS_SNAP_MAGIC		0x52414d4fULL	/* "RAMO" */
#define RAMOOPS_SNAP_VERSION		1
#define RAMOOPS_TAIL_OFFSET		(RAMOOPS_SNAP_OFFSET + \
					 RAMOOPS_SNAP_SLOTS * RAMOOPS_SNAP_SLOT_STRIDE)
#define RAMOOPS_TAIL_SLOT_SIZE		SZ_128K
#define RAMOOPS_TAIL_PAYLOAD_SIZE	(RAMOOPS_TAIL_SLOT_SIZE - PAGE_SIZE)
#define RAMOOPS_TAIL_MAGIC		0x314c4941544b4341ULL /* "ACKTAIL1" */

/* Opt-in diagnostics, separate from snapshots, stall witness and QSEE ring. */
#define RAMOOPS_RESET_OFFSET		0x3f30000ULL
#define RAMOOPS_RESET_SIZE		SZ_64K
#define RAMOOPS_RESET_HEADER		512
#define RAMOOPS_PATH_OFFSET		0x3f40000ULL
#define RAMOOPS_PATH_MAGIC		0x31485441504b4341ULL /* "ACKPATH1" */

static u8 ramoops_reset_previous[RAMOOPS_DIAG_MARK_SIZE];
static bool ramoops_reset_ready;
static unsigned long ramoops_reset_seen;

static bool ramoops_reset_capture;
module_param_named(reset_capture, ramoops_reset_capture, bool, 0400);
MODULE_PARM_DESC(reset_capture, "Capture orderly reset and process/signal evidence to logdump");

static_assert(sizeof(struct ramoops_reset_record) == 256);
static_assert(RAMOOPS_RESET_MARK_OFFSET +
	      RAMOOPS_RESET_PSHOLD * sizeof(struct ramoops_reset_record) <
	      RAMOOPS_DIAG_MARK_HDR);
static_assert(RAMOOPS_PATH_OFFSET + PAGE_SIZE + RAMOOPS_DIAG_MARK_SIZE <= SZ_64M);

void ramoops_reset_mark(enum ramoops_reset_path path, const char *reason)
{
	struct ramoops_reset_record rec = { };
	void __iomem *pos;

	/* The mapping is prepared at probe: never map, allocate or printk here. */
	if (!smp_load_acquire(&ramoops_reset_ready) ||
	    path < RAMOOPS_RESET_PANIC ||
	    path > RAMOOPS_RESET_PANIC_SNAPSHOT_DONE ||
	    test_and_set_bit(path - 1, &ramoops_reset_seen))
		return;

	rec.version = cpu_to_le32(1);
	rec.path = cpu_to_le32(path);
	rec.boot_id = cpu_to_le32(readl(ramoops_diag_marks +
					RAMOOPS_DIAG_MARK_HDR + 4));
	rec.timestamp_ns = cpu_to_le64(ktime_get_mono_fast_ns());
	rec.cpu = cpu_to_le32(raw_smp_processor_id());
	rec.pid = cpu_to_le32(task_pid_nr(current));
	memcpy(rec.comm, current->comm, sizeof(rec.comm));
	if (reason)
		strscpy(rec.reason, reason, sizeof(rec.reason));
	rec.crc32 = cpu_to_le32(crc32_le(~0U, (u8 *)&rec + sizeof(rec.magic),
			offsetof(struct ramoops_reset_record, crc32) -
			sizeof(rec.magic)) ^ ~0U);

	pos = ramoops_diag_marks + RAMOOPS_RESET_MARK_OFFSET +
		(path - 1) * sizeof(rec);
	writel(0, pos);
	memcpy_toio(pos + sizeof(rec.magic), (u8 *)&rec + sizeof(rec.magic),
		    sizeof(rec) - sizeof(rec.magic));
	wmb();
	writel(RAMOOPS_RESET_MARK_MAGIC, pos);
	/* Drain the write-combined mapping before the caller enters firmware. */
	(void)readl(pos);
}
EXPORT_SYMBOL_GPL(ramoops_reset_mark);

struct ramoops_snap_header {
	__le64 magic;
	__le64 seq;
	__le64 index;
	__le64 timestamp_ns;
	__le32 payload_offset;
	__le32 payload_len;
	__le32 phys_addr_lo;
	__le32 slot;
	__le32 reserved[4];	/* struct is exactly 64 bytes */
};

struct ramoops_snapper {
	struct ramoops_context *cxt;
	struct file *file;
	struct block_device *bdev;
	void *buf;			/* header page + SZ_1M payload, vmalloc'd once */
	void *tail_buf;			/* header page + bounded printk tail */
	struct task_struct *task;
	struct completion wake;
	u64 seq;
	unsigned int slot;		/* next slot to write */
	unsigned int failures;		/* consecutive commit failures */
	bool warned;
	struct mutex reset_lock;		/* serialize reset callbacks only */
	void *reset_buf;
	struct notifier_block reset_nb;
	bool reset_registered;
};

static struct ramoops_snapper ramoops_snapper;
static DECLARE_COMPLETION(ramoops_snapshot_ready);

int ramoops_wait_snapshot_ready(unsigned int timeout_ms)
{
	if (!wait_for_completion_timeout(&ramoops_snapshot_ready,
					 msecs_to_jiffies(timeout_ms)))
		return -ETIMEDOUT;
	return 0;
}
EXPORT_SYMBOL_GPL(ramoops_wait_snapshot_ready);

/* run225: set by the first stage record (KGSL up), never cleared. */
static bool ramoops_snap_fast;

static void ramoops_debug_apps_wdt(void);

static_assert(sizeof(struct ramoops_snap_header) == RAMOOPS_SNAP_HDR_SIZE);
static_assert(RAMOOPS_TAIL_OFFSET +
	      RAMOOPS_SNAP_SLOTS * RAMOOPS_TAIL_SLOT_SIZE <= 0x3f00000ULL);

/* Find the "logdump" partition the way uevent PARTNAME=logdump names it. */
static int ramoops_snap_find_logdump(struct device *dev, void *data)
{
	dev_t *found = data;
	struct block_device *bdev;

	if (!dev->devt || !dev->type || strcmp(dev->type->name, "partition"))
		return 0;
	bdev = dev_to_bdev(dev);
	if (!bdev->bd_meta_info || !bdev->bd_meta_info->volname[0])
		return 0;
	if (memcmp(bdev->bd_meta_info->volname, "logdump", 7) ||
	    bdev->bd_meta_info->volname[7])
		return 0;
	*found = dev->devt;
	return 1;
}

/* Resolved late: MMC block devices register after ramoops probes. */
static int ramoops_snap_open_logdump(struct ramoops_snapper *s)
{
	dev_t found = 0;
	int err;

	/*
	 * block_class registers at subsys_initcall, after this probe path:
	 * walking it earlier WARNs in class_for_each_device. Retry later.
	 */
	if (!class_is_registered(&block_class))
		return -ENODEV;

	class_for_each_device(&block_class, NULL, &found,
			      ramoops_snap_find_logdump);
	if (!found)
		return -ENODEV;

	s->file = bdev_file_open_by_dev(found, BLK_OPEN_WRITE, &ramoops_snapper,
					NULL);
	if (IS_ERR(s->file)) {
		err = PTR_ERR(s->file);
		s->file = NULL;
		return err;
	}
	if (bdev_nr_bytes(file_bdev(s->file)) <
	    RAMOOPS_PATH_OFFSET + PAGE_SIZE + RAMOOPS_DIAG_MARK_SIZE) {
		pr_err_once("DIAG: logdump is too small for diagnostic records\n");
		filp_close(s->file, &ramoops_snapper);
		s->file = NULL;
		return -ENOSPC;
	}
	/* The notifier uses a separate buffer; publish the held device last. */
	smp_store_release(&s->bdev, file_bdev(s->file));
	return 0;
}

/* Copy @len bytes of vmalloc-backed @data to byte @offset of @bdev. */
static int ramoops_snap_bio_write(struct block_device *bdev, void *data,
				  size_t len, u64 offset)
{
	unsigned int nr_pages = (len + PAGE_SIZE - 1) >> PAGE_SHIFT;
	struct bio *bio;
	size_t done = 0;
	int err = 0;

	bio = bio_alloc(bdev, bio_max_segs(nr_pages), REQ_OP_WRITE,
			GFP_KERNEL | __GFP_NOFAIL);
	if (!bio)
		return -ENOMEM;

	while (done < len) {
		size_t chunk = min_t(size_t, len - done, PAGE_SIZE);
		struct page *page = vmalloc_to_page((char *)data + done);

		if (!page || bio_add_page(bio, page, chunk, 0) != chunk) {
			bio_put(bio);
			return -EIO;
		}
		done += chunk;
	}

	bio->bi_iter.bi_sector = offset >> SECTOR_SHIFT;
	bio->bi_iter.bi_size = len;
	submit_bio_wait(bio);
	err = blk_status_to_errno(bio->bi_status);
	bio_put(bio);

	return err;
}

static void ramoops_snap_init_header(void *buf, u64 magic, u64 seq,
				     u32 len, u32 phys_addr, unsigned int slot)
{
	struct ramoops_snap_header *hdr = buf;

	memset(buf, 0, PAGE_SIZE);
	hdr->magic = cpu_to_le64(magic);
	hdr->seq = cpu_to_le64(seq);
	hdr->index = cpu_to_le64(seq);
	hdr->timestamp_ns = cpu_to_le64(ktime_get_ns());
	hdr->payload_offset = cpu_to_le32(PAGE_SIZE);
	hdr->payload_len = cpu_to_le32(len);
	hdr->phys_addr_lo = cpu_to_le32(phys_addr);
	hdr->slot = cpu_to_le32(slot);
	hdr->reserved[0] = cpu_to_le32(RAMOOPS_SNAP_VERSION);
	hdr->reserved[1] = cpu_to_le32(crc32_le(~0U, buf + PAGE_SIZE, len) ^ ~0U);
	hdr->reserved[2] = cpu_to_le32(crc32_le(~0U, buf, sizeof(*hdr)) ^ ~0U);
}

/*
 * Process context only. An incomplete overwrite is invalid until both the
 * payload and its final checksummed identity have reached stable storage.
 */
static int ramoops_snap_commit(struct ramoops_snapper *s, void *buf, u64 base)
{
	struct ramoops_snap_header *hdr = buf;
	__le64 magic = hdr->magic;
	u32 len = le32_to_cpu(hdr->payload_len);
	size_t write_len = ALIGN(len, PAGE_SIZE);
	int err;

	hdr->magic = 0;
	err = ramoops_snap_bio_write(s->bdev, buf, PAGE_SIZE, base);
	if (err)
		return err;
	err = blkdev_issue_flush(s->bdev);
	if (err)
		return err;

	if (write_len > len)
		memset(buf + PAGE_SIZE + len, 0, write_len - len);
	err = ramoops_snap_bio_write(s->bdev, buf + PAGE_SIZE, write_len,
				     base + PAGE_SIZE);
	if (err)
		return err;
	err = blkdev_issue_flush(s->bdev);
	if (err)
		return err;

	hdr->magic = magic;
	err = ramoops_snap_bio_write(s->bdev, buf, PAGE_SIZE, base);
	if (err)
		return err;
	return blkdev_issue_flush(s->bdev);
}

static int ramoops_snap_take(struct ramoops_snapper *s)
{
	struct kmsg_dump_iter iter;
	void *payload = s->buf + PAGE_SIZE;
	unsigned int slot = s->slot % RAMOOPS_SNAP_SLOTS;
	u64 base = RAMOOPS_SNAP_OFFSET + slot * RAMOOPS_SNAP_SLOT_STRIDE;
	size_t tail_len = 0;
	ssize_t copied;
	int err;

	copied = ramoops_raw_copy(s->cxt, payload, 0, SZ_1M);
	if (copied < 0)
		return (int)copied;
	if (s->tail_buf) {
		kmsg_dump_rewind(&iter);
		kmsg_dump_get_buffer(&iter, false, s->tail_buf + PAGE_SIZE,
				     RAMOOPS_TAIL_PAYLOAD_SIZE, &tail_len);
	}

	s->seq++;
	ramoops_snap_init_header(s->buf, RAMOOPS_SNAP_MAGIC, s->seq,
				 SZ_1M, RAMOOPS_RAW_PHYS, slot);
	err = ramoops_snap_commit(s, s->buf, base);
	if (err)
		return err;
	if (s->tail_buf && tail_len) {
		ramoops_snap_init_header(s->tail_buf, RAMOOPS_TAIL_MAGIC, s->seq,
					 tail_len, 0, slot);
		err = ramoops_snap_commit(s, s->tail_buf,
					  RAMOOPS_TAIL_OFFSET +
					  slot * RAMOOPS_TAIL_SLOT_SIZE);
		if (err)
			return err;
	}

	/* Advance only after a fully flushed sample. */
	s->slot = slot + 1;
	return 0;
}

/*
 * run226: commit one snapshot from panic().
 *
 * panic() disables IRQs and preemption early, then calls
 * panic_other_cpus_shutdown() -> smp_send_stop() before it ever reaches
 * emergency_restart(). Two consequences that made the fault unobservable:
 *
 *   - the snapshot kthread freezes on whichever CPU smp_send_stop() halts,
 *     so no sample is ever taken after the panic; and
 *   - ramoops_reset_note()'s guard requires !irqs_disabled(), so the
 *     durable RESETNOTE slot in the logdump stays zero even though
 *     emergency_restart() runs later.
 *
 * Called at the top of panic() -- before smp_send_stop(), while every
 * sibling CPU is still running and the eMMC block layer is still alive --
 * this nudges the snapshotter and busy-waits, with IRQs off and no
 * sleeping, for one fully flushed sample. That sample carries the panic
 * message and stack trace into the logdump, where it survives the reset.
 *
 * Bounded and best-effort: a bounded spin on the snapshot sequence number
 * cannot deadlock the panic path even if the snapshotter never runs.
 */
void ramoops_panic_snapshot(void)
{
	struct ramoops_snapper *s = &ramoops_snapper;
	unsigned int slot;
	int spins;
	bool ok;

	/*
	 * Nothing durable to write into if the logdump was never opened, or
	 * if this is a nested panic on the snapshotter's own CPU (the wakeup
	 * below could never be serviced).
	 */
	if (!s->file || !s->bdev || !s->buf)
		return;
	if (s->task && s->task == current)
		return;

	/* Record the attempt in RAM first: cheap, and needs no I/O. */
	ramoops_reset_mark(RAMOOPS_RESET_PANIC_SNAPSHOT, "panic");

	/*
	 * Wait on s->slot, not s->seq. ramoops_snap_take() bumps s->seq
	 * before it writes, so a seq change would only mean the write had
	 * started; s->slot is advanced strictly after the console and tail
	 * commits have both flushed, so a slot change means the sample is
	 * durable.
	 */
	slot = READ_ONCE(s->slot);

	/* Nudge the snapshotter out of its timed wait. */
	complete(&s->wake);

	/*
	 * Spin, do not sleep: this runs with IRQs and preemption disabled.
	 * Re-enabling interrupts here would be wrong (the panic path may be
	 * holding them deliberately), so just burn cycles waiting for the
	 * snapshotter to finish a flushed sample. The cap keeps the panic
	 * path bounded no matter what the block layer does.
	 */
	for (spins = 0; spins < RAMOOPS_PANIC_SNAP_SPINS; spins++) {
		if (READ_ONCE(s->slot) != slot)
			break;
		udelay(RAMOOPS_PANIC_SNAP_UDELAY);
	}
	ok = READ_ONCE(s->slot) != slot;

	/*
	 * Leave a RAM breadcrumb saying whether the flush landed. If the
	 * logdump shows no post-panic sample, "timeout" here means the
	 * snapshotter never completed (block layer stuck, or its CPU was
	 * already halted); "ok" means it did and the sample is in the logdump.
	 */
	ramoops_reset_mark(RAMOOPS_RESET_PANIC_SNAPSHOT_DONE,
			   ok ? "ok" : "timeout");
}

/*
 * Blocking reboot notifiers run before device_shutdown(), while MMC is live.
 * Never panic or replace the requested reboot. Emergency/atomic resets cannot
 * use this sleeping I/O path and remain covered only by periodic snapshots.
 */
static int ramoops_reset_notify(struct notifier_block *nb,
				unsigned long action, void *command)
{
	struct ramoops_snapper *s = container_of(nb, struct ramoops_snapper,
					       reset_nb);
	struct kmsg_dump_iter iter;
	struct block_device *bdev;
	char *buf = s->reset_buf;
	size_t len = 0;
	int err;

	pr_emerg("DIAG: orderly reset action=%lu pid=%d comm=%s command=%.128s\n",
		 action, task_pid_nr(current), current->comm,
		 command ? (char *)command : "");
	/* Avoid waiting if another reset callback is already doing I/O. */
	if (!mutex_trylock(&s->reset_lock)) {
		pr_warn("DIAG: reset capture skipped: another reset capture is busy\n");
		return NOTIFY_DONE;
	}
	/* Paired with the worker's publication of its held block device. */
	bdev = smp_load_acquire(&s->bdev);
	if (!bdev ||
	    bdev_nr_bytes(bdev) < RAMOOPS_RESET_OFFSET + RAMOOPS_RESET_SIZE) {
		pr_warn("DIAG: reset capture skipped: logdump unavailable or too small\n");
		goto out;
	}
	memset(buf, 0, RAMOOPS_RESET_SIZE);
	kmsg_dump_rewind(&iter);
	kmsg_dump_get_buffer(&iter, false, buf + RAMOOPS_RESET_HEADER,
			     RAMOOPS_RESET_SIZE - RAMOOPS_RESET_HEADER, &len);
	scnprintf(buf, RAMOOPS_RESET_HEADER,
		  "ACKRESET1 timestamp_ns=%llu action=%lu pid=%d comm=%s command=%.128s tail_bytes=%zu\n",
		  ktime_get_ns(), action, task_pid_nr(current), current->comm,
		  command ? (char *)command : "", len);
	err = ramoops_snap_bio_write(bdev, buf, RAMOOPS_RESET_SIZE,
				     RAMOOPS_RESET_OFFSET);
	if (!err)
		err = blkdev_issue_flush(bdev);
	if (err)
		pr_warn("DIAG: reset capture failed errno=%d\n", err);
	else
		pr_info("DIAG: reset capture persisted tail_bytes=%zu\n", len);
out:
	mutex_unlock(&s->reset_lock);
	return NOTIFY_DONE;
}

/*
 * The thread retries the open because MMC block devices register at
 * device_initcall, long after ramoops probes at postcore_initcall.
 */
static int ramoops_snap_thread(void *unused)
{
	struct ramoops_snapper *s = &ramoops_snapper;
	unsigned long timeout = msecs_to_jiffies(RAMOOPS_SNAP_INTERVAL_MS);
	int ret;
	bool previous_attempted = false;
	(void)unused;

	if (!RAMOOPS_PERIODIC_SNAPSHOTS) {
		/*
		 * No periodic sample will flush, so the camera late-init
		 * callers parked on ramoops_snapshot_ready would burn their
		 * 10 s timeout and then skip registration entirely. Release
		 * them at once; the panic completion still captures on
		 * demand.
		 */
		complete_all(&ramoops_snapshot_ready);
	}

	while (!kthread_should_stop()) {
		if (!s->file) {
			int err = ramoops_snap_open_logdump(s);

			if (err) {
				/* Storage is not ready yet; keep waiting. */
				wait_for_completion_interruptible_timeout(
					&s->wake,
					msecs_to_jiffies(RAMOOPS_SNAP_INTERVAL_MS));
				continue;
			}
		}

		if (ramoops_reset_capture && !previous_attempted) {
			memcpy(s->buf + PAGE_SIZE, ramoops_reset_previous,
			       sizeof(ramoops_reset_previous));
			ramoops_snap_init_header(s->buf, RAMOOPS_PATH_MAGIC, 0,
				sizeof(ramoops_reset_previous),
				RAMOOPS_RAW_PHYS +
				RAMOOPS_RAW_DMESG_CNT * SZ_4K, 0);
			ret = ramoops_snap_commit(s, s->buf, RAMOOPS_PATH_OFFSET);
			previous_attempted = true;
			pr_info("DIAG: previous reset page commit ret=%d offset=%llu\n",
				ret, RAMOOPS_PATH_OFFSET);
		}

		/*
		 * Without periodic sampling, only an explicit panic completion may
		 * request a snapshot. Sleep until it arrives instead of waking every
		 * RAMOOPS_SNAP_INTERVAL_MS: the timeout was only there to let
		 * kthread_stop() drain a worker that had no other wakeup. The stop
		 * path now completes the wake before kthread_stop(), so the worker
		 * stays parked between panics. A failed explicit capture keeps its
		 * existing bounded retry budget.
		 */
		if (!RAMOOPS_PERIODIC_SNAPSHOTS && !s->failures) {
			ret = wait_for_completion_interruptible(&s->wake);
			if (ret || kthread_should_stop())
				continue;
		}

		/*
		 * With periodic sampling the sample follows the logdump
		 * partition as soon as it is there: the boot that has to be
		 * caught can die before the first tick, and the sample that
		 * matters is the one that holds the console log up to that
		 * point.
		 */

		ret = ramoops_snap_take(s);
		if (ret) {
			/*
			 * run226: a single transient commit error used to
			 * disable the snapshotter permanently. That is a
			 * poor trade during bring-up, where the evidence
			 * channel matters most and errors are the thing
			 * being diagnosed: the most likely cause of a
			 * failure here is the very fault we are trying to
			 * observe, and losing the channel hides it.
			 *
			 * Tolerate consecutive failures up to a limit, then
			 * give up so a genuinely wedged logdump does not
			 * spin forever.
			 */
			if (++s->failures <= RAMOOPS_SNAP_MAX_FAILURES) {
				pr_err("DIAG: logdump commit failed errno=%d (%u/%u); retrying\n",
				       ret, s->failures, RAMOOPS_SNAP_MAX_FAILURES);
				continue;
			}
			pr_err("DIAG: logdump commit failed errno=%d (%u consecutive); snapshotter disabled\n",
			       ret, s->failures);
			break;
		}
		s->failures = 0;
		if (!s->warned) {
			s->warned = true;
			pr_info("snapshot38 logdump snapshotter ready offset=%llu slots=%u size=%u\n",
				RAMOOPS_SNAP_OFFSET, RAMOOPS_SNAP_SLOTS, SZ_1M);
			pr_info("DIAG: snapshot format=%u CRC32 committed, periodic printk tail=%s offset=%llu\n",
				RAMOOPS_SNAP_VERSION, s->tail_buf ? "on" : "off",
				RAMOOPS_TAIL_OFFSET);
			complete_all(&ramoops_snapshot_ready);
		}
		/*
		 * Every tenth snapshot was ~1260 lines per boot, which is
		 * spam rather than evidence. Every hundredth keeps the
		 * liveness witness this port relies on, at about one line
		 * per boot.
		 */
		if (RAMOOPS_PERIODIC_SNAPSHOTS && ramoops_reset_capture &&
		    !(s->seq % 100))
			pr_info("DIAG: snapshot heartbeat seq=%llu uptime_ns=%llu\n",
				s->seq, ktime_get_ns());
		/*
		 * run225/226: this tree builds no watchdog driver, so a
		 * firmware-armed APPS WDT would bite unseen. Re-read the
		 * block on every fast-cadence tick to catch it arming.
		 *
		 * run225 gated this on !(seq % 8) to limit printk volume,
		 * but seq only reached 21 before the reset, so the re-reads
		 * that did fire landed at seq 8 and 16 -- before the fast
		 * cadence was even enabled at the first KGSL stage record,
		 * i.e. exactly when the read is not interesting. Each read
		 * is a handful of MMIO reads and is non-blocking, so run226
		 * samples it on every fast tick instead.
		 */
		if (RAMOOPS_PERIODIC_SNAPSHOTS) {
			bool fast;

			/* Pairs with the stage writer's release of the fast flag. */
			fast = smp_load_acquire(&ramoops_snap_fast);
			if (fast)
				ramoops_debug_apps_wdt();
			timeout = msecs_to_jiffies(fast ? RAMOOPS_SNAP_FAST_MS :
						RAMOOPS_SNAP_INTERVAL_MS);
			wait_for_completion_interruptible_timeout(&s->wake,
								  timeout);
		}
	}
	return 0;
}

/*
 * Stall witness (snapshot43 diagnostic).
 *
 * Keep a sampled execution witness in the partition tail, separate from
 * the historical RAM witness. A correctly attributed later record shows
 * execution/write progress at that time; old or missing records do not
 * distinguish storage failure, scheduling failure, or a kernel-wide halt.
 * The caller records the previous canary sample before updating it.
 * Plain writes wait for I/O but do not provide an explicit flush/FUA
 * guarantee. Saved records alone do not establish reset durability.
 */
#define RAMOOPS_WITNESS_OFFSET	0x3f00000ULL
#define RAMOOPS_WITNESS_LEN	512

/* Formerly the QSEE trace ring; kept as the end of the stage region. */
#define RAMOOPS_QTRACE_OFFSET	0x3f10000ULL
#define RAMOOPS_QTRACE_SLOTS	128

static_assert(RAMOOPS_QTRACE_OFFSET +
	      RAMOOPS_QTRACE_SLOTS * RAMOOPS_WITNESS_LEN <= RAMOOPS_RESET_OFFSET);
static_assert(RAMOOPS_RESET_OFFSET + RAMOOPS_RESET_SIZE <= SZ_64M);

/* The diagnostic writer keeps this file open for system lifetime. */
static struct file *ramoops_witness_file __maybe_unused;
static struct block_device *ramoops_witness_bdev;
static void *ramoops_witness_buf;
/* Serialises the lazy open and the shared ramoops_witness_buf. */
static DEFINE_MUTEX(ramoops_witness_lock);

static int ramoops_witness_open(void)
{
	dev_t found = 0;
	struct file *file;
	void *buf;

	if (ramoops_witness_bdev)
		return 0;
	/* Same init-order guard as ramoops_snap_open_logdump above. */
	if (!class_is_registered(&block_class))
		return -ENODEV;
	class_for_each_device(&block_class, NULL, &found,
			      ramoops_snap_find_logdump);
	if (!found)
		return -ENODEV;
	file = bdev_file_open_by_dev(found, BLK_OPEN_READ | BLK_OPEN_WRITE,
				     &ramoops_snapper, NULL);
	if (IS_ERR(file))
		return PTR_ERR(file);
	buf = vmalloc(RAMOOPS_WITNESS_LEN);
	if (!buf) {
		filp_close(file, &ramoops_snapper);
		return -ENOMEM;
	}
	ramoops_witness_file = file;
	ramoops_witness_buf = buf;
	ramoops_witness_bdev = file_bdev(file);
	return 0;
}

/* Caller holds ramoops_witness_lock. */
static int ramoops_witness_write_at(u64 offset, blk_opf_t opf,
				    const void *data, size_t len)
{
	struct bio *bio;
	int err;

	if (len > RAMOOPS_WITNESS_LEN)
		return -EINVAL;
	err = ramoops_witness_open();
	if (err)
		return err;
	memset(ramoops_witness_buf, 0, RAMOOPS_WITNESS_LEN);
	memcpy(ramoops_witness_buf, data, len);

	bio = bio_alloc(ramoops_witness_bdev, 1, opf,
			GFP_KERNEL | __GFP_NOFAIL);
	if (!bio)
		return -ENOMEM;
	if (bio_add_page(bio, vmalloc_to_page(ramoops_witness_buf),
			 RAMOOPS_WITNESS_LEN, 0) != RAMOOPS_WITNESS_LEN) {
		bio_put(bio);
		return -EIO;
	}
	bio->bi_iter.bi_sector = offset >> SECTOR_SHIFT;
	bio->bi_iter.bi_size = RAMOOPS_WITNESS_LEN;
	submit_bio_wait(bio);
	err = blk_status_to_errno(bio->bi_status);
	bio_put(bio);
	return err;
}

/*
 * Stage records occupy the gap between the witness and the QSEE ring.
 * GPU callers only enqueue owned copies; a storage stall must not hold
 * their device mutex.
 * shortcut: best-effort queue; revisit if reset-time durability is required.
 */
#define RAMOOPS_STAGE_OFFSET	0x3f01000ULL
#define RAMOOPS_STAGE_SLOTS	32

static_assert(RAMOOPS_STAGE_OFFSET >=
	      RAMOOPS_WITNESS_OFFSET + RAMOOPS_WITNESS_LEN);
static_assert(RAMOOPS_STAGE_OFFSET + RAMOOPS_STAGE_SLOTS *
	      RAMOOPS_WITNESS_LEN <= RAMOOPS_QTRACE_OFFSET);

struct ramoops_stage_entry {
	unsigned int slot;
	size_t len;
	u8 data[RAMOOPS_WITNESS_LEN];
};

static struct ramoops_stage_entry ramoops_stage_queue[RAMOOPS_STAGE_SLOTS];
static unsigned int ramoops_stage_head, ramoops_stage_tail, ramoops_stage_count;
static DEFINE_SPINLOCK(ramoops_stage_lock);
static struct workqueue_struct *ramoops_stage_wq;

static void ramoops_stage_worker(struct work_struct *work)
{
	struct ramoops_stage_entry entry;
	unsigned long flags;
	int err;

	for (;;) {
		spin_lock_irqsave(&ramoops_stage_lock, flags);
		if (!ramoops_stage_count) {
			spin_unlock_irqrestore(&ramoops_stage_lock, flags);
			return;
		}
		entry = ramoops_stage_queue[ramoops_stage_head];
		ramoops_stage_head = (ramoops_stage_head + 1) % RAMOOPS_STAGE_SLOTS;
		ramoops_stage_count--;
		spin_unlock_irqrestore(&ramoops_stage_lock, flags);

		mutex_lock(&ramoops_witness_lock);
		err = ramoops_witness_write_at(RAMOOPS_STAGE_OFFSET +
				entry.slot * RAMOOPS_WITNESS_LEN,
				REQ_OP_WRITE | REQ_SYNC | REQ_FUA,
				entry.data, entry.len);
		mutex_unlock(&ramoops_witness_lock);
		pr_info("DIAG stage slot=%u write_ret=%d\n", entry.slot, err);
	}
}

static DECLARE_WORK(ramoops_stage_work, ramoops_stage_worker);

static void ramoops_stage_start(void)
{
	struct workqueue_struct *wq;
	unsigned long flags;

	wq = alloc_ordered_workqueue("ramoops_stage", 0);
	if (!wq) {
		pr_warn("stage worker unavailable; stage capture disabled\n");
		return;
	}
	spin_lock_irqsave(&ramoops_stage_lock, flags);
	ramoops_stage_head = 0;
	ramoops_stage_tail = 0;
	ramoops_stage_count = 0;
	ramoops_stage_wq = wq;
	spin_unlock_irqrestore(&ramoops_stage_lock, flags);
}

static void ramoops_stage_stop(void)
{
	struct workqueue_struct *wq;
	unsigned long flags;

	spin_lock_irqsave(&ramoops_stage_lock, flags);
	wq = ramoops_stage_wq;
	ramoops_stage_wq = NULL;
	spin_unlock_irqrestore(&ramoops_stage_lock, flags);
	/* Removal drains accepted records; in-flight I/O can delay removal. */
	if (wq)
		destroy_workqueue(wq);
}

int ramoops_stage_write(unsigned int slot, const void *data, size_t len)
{
	struct ramoops_stage_entry *entry;
	unsigned long flags;
	int err = 0;

	if (slot >= RAMOOPS_STAGE_SLOTS || len > RAMOOPS_WITNESS_LEN)
		return -EINVAL;
	spin_lock_irqsave(&ramoops_stage_lock, flags);
	if (!ramoops_stage_wq) {
		err = -ENODEV;
		goto out;
	}
	if (ramoops_stage_count == RAMOOPS_STAGE_SLOTS) {
		err = -ENOSPC;
		goto out;
	}
	entry = &ramoops_stage_queue[ramoops_stage_tail];
	entry->slot = slot;
	entry->len = len;
	memcpy(entry->data, data, len);
	ramoops_stage_tail = (ramoops_stage_tail + 1) % RAMOOPS_STAGE_SLOTS;
	ramoops_stage_count++;
	queue_work(ramoops_stage_wq, &ramoops_stage_work);
out:
	spin_unlock_irqrestore(&ramoops_stage_lock, flags);

	/*
	 * run225: stage records only come from the GPU bring-up path, so the
	 * first one marks "KGSL is up". Switch the snapshotter to the fast
	 * cadence and cut short the wait that is already in progress, so the
	 * sample immediately after the GPU start is taken at once.
	 */
	if (RAMOOPS_PERIODIC_SNAPSHOTS) {
		bool fast;

		/* Match the release below before enabling the fast cadence. */
		fast = smp_load_acquire(&ramoops_snap_fast);
		if (!fast) {
			/* Publish the fast cadence before waking the snapshotter. */
			smp_store_release(&ramoops_snap_fast, true);
			pr_info("DIAG: snapshot cadence -> %u ms (stage activity)\n",
				RAMOOPS_SNAP_FAST_MS);
			complete(&ramoops_snapper.wake);
		}
	}

	return err;
}
EXPORT_SYMBOL_GPL(ramoops_stage_write);

/*
 * run225: durable note for a kernel-initiated restart.
 *
 * The orderly path (kernel_restart -> kernel_restart_prepare) is already
 * covered by the blocking reboot notifier, which persists the whole kmsg tail
 * as ACKRESET1. The emergency path (emergency_restart -> machine_restart ->
 * psci_sys_reset) skips that notifier entirely, and it reaches the reset
 * point with interrupts disabled, so nothing sleeping can run there either.
 *
 * The RAM mark page (ramoops_reset_mark) is not a usable channel for this
 * workflow: the stock boot that always follows a port death wipes that range,
 * so the record is gone by the time a port boot can read it back.
 *
 * So: one line to the console (cheap, atomic-safe) plus a best-effort durable
 * copy in the stage area's last slot. The queued copy is atomic-safe; the
 * synchronous FUA copy runs only when a sleeping context exists, which covers
 * the common "panic() -> emergency_restart()" and sysrq/hotplug callers.
 */
#define RAMOOPS_RESET_NOTE_SLOT		(RAMOOPS_STAGE_SLOTS - 1)
#define RAMOOPS_RESET_NOTE_OFFSET	(RAMOOPS_STAGE_OFFSET + \
					 RAMOOPS_RESET_NOTE_SLOT * \
					 RAMOOPS_WITNESS_LEN)
void ramoops_reset_note(const char *what)
{
	char buf[RAMOOPS_WITNESS_LEN] = { 0 };
	size_t len;

	len = scnprintf(buf, sizeof(buf),
			"RESETNOTE what=%s pid=%d comm=%s t_ns=%llu",
			what, task_pid_nr(current), current->comm,
			ktime_get_ns());
	pr_emerg("DIAG reset note: %s\n", buf);
	ramoops_stage_write(RAMOOPS_RESET_NOTE_SLOT, buf, len + 1);

	if (!in_interrupt() && !in_nmi() && !irqs_disabled()) {
		int err;

		mutex_lock(&ramoops_witness_lock);
		err = ramoops_witness_write_at(RAMOOPS_RESET_NOTE_OFFSET,
				REQ_OP_WRITE | REQ_SYNC | REQ_FUA, buf,
				RAMOOPS_WITNESS_LEN);
		mutex_unlock(&ramoops_witness_lock);
		pr_emerg("DIAG reset note: offset=%llu sync_ret=%d\n",
			 (unsigned long long)RAMOOPS_RESET_NOTE_OFFSET, err);
	}
}
EXPORT_SYMBOL_GPL(ramoops_reset_note);

static void ramoops_snap_start(struct ramoops_context *cxt)
{
	struct ramoops_snapper *s = &ramoops_snapper;

	memset(s, 0, sizeof(*s));
	s->cxt = cxt;
	init_completion(&s->wake);
	mutex_init(&s->reset_lock);

	s->buf = __vmalloc(PAGE_SIZE + SZ_1M, GFP_KERNEL);
	if (!s->buf) {
		pr_err("snapshot38 staging buffer alloc failed; snapshotter disabled\n");
		return;
	}
	s->tail_buf = vzalloc(RAMOOPS_TAIL_SLOT_SIZE);

	s->task = kthread_create(ramoops_snap_thread, s, "ramoops_snap");
	if (IS_ERR(s->task)) {
		pr_err("snapshot38 thread create failed errno=%ld; snapshotter disabled\n",
		       PTR_ERR(s->task));
		s->task = NULL;
		vfree(s->buf);
		s->buf = NULL;
		vfree(s->tail_buf);
		s->tail_buf = NULL;
		return;
	}
	if (ramoops_reset_capture) {
		int err;

		s->reset_buf = vzalloc(RAMOOPS_RESET_SIZE);
		if (s->reset_buf) {
			s->reset_nb.notifier_call = ramoops_reset_notify;
			err = register_reboot_notifier(&s->reset_nb);
			if (!err) {
				s->reset_registered = true;
				pr_info("DIAG: orderly reset capture armed offset=%llu size=%u\n",
					RAMOOPS_RESET_OFFSET, RAMOOPS_RESET_SIZE);
			} else {
				pr_warn("DIAG: reset notifier registration failed errno=%d\n",
					err);
				vfree(s->reset_buf);
				s->reset_buf = NULL;
			}
		} else {
			pr_warn("DIAG: reset buffer unavailable; capture disabled\n");
		}
	}
	wake_up_process(s->task);
}

/* Drain the worker before any zone mapping is freed or unmapped. */
static void ramoops_snap_stop(void)
{
	struct ramoops_snapper *s = &ramoops_snapper;

	if (s->reset_registered) {
		unregister_reboot_notifier(&s->reset_nb);
		s->reset_registered = false;
	}
	vfree(s->reset_buf);
	s->reset_buf = NULL;
	if (s->task) {
		/*
		 * The worker parks on s->wake between panics. kthread_stop()
		 * would set KTHREAD_SHOULD_STOP, but a task sleeping in the
		 * completion wait is not woken by that flag alone, so release
		 * the completion first and let the loop observe the stop.
		 */
		complete(&s->wake);
		kthread_stop(s->task);
		s->task = NULL;
	}
	if (s->buf) {
		vfree(s->buf);
		s->buf = NULL;
	}
	vfree(s->tail_buf);
	s->tail_buf = NULL;
	if (s->file) {
		filp_close(s->file, &ramoops_snapper);
		s->file = NULL;
		s->bdev = NULL;
	}
}
#endif

static void ramoops_free_przs(struct ramoops_context *cxt)
{
	int i;

	/* Free pmsg PRZ */
	persistent_ram_free(&cxt->mprz);

	/* Free console PRZ */
	persistent_ram_free(&cxt->cprz);

	/* Free dump PRZs */
	if (cxt->dprzs) {
		for (i = 0; i < cxt->max_dump_cnt; i++)
			persistent_ram_free(&cxt->dprzs[i]);

		kfree(cxt->dprzs);
		cxt->dprzs = NULL;
		cxt->max_dump_cnt = 0;
	}

	/* Free ftrace PRZs */
	if (cxt->fprzs) {
		for (i = 0; i < cxt->max_ftrace_cnt; i++)
			persistent_ram_free(&cxt->fprzs[i]);
		kfree(cxt->fprzs);
		cxt->fprzs = NULL;
		cxt->max_ftrace_cnt = 0;
	}
}

static int ramoops_init_przs(const char *name,
			     struct device *dev, struct ramoops_context *cxt,
			     struct persistent_ram_zone ***przs,
			     phys_addr_t *paddr, size_t mem_sz,
			     ssize_t record_size,
			     unsigned int *cnt, u32 sig, u32 flags)
{
	int err = -ENOMEM;
	int i;
	size_t zone_sz;
	struct persistent_ram_zone **prz_ar;

	/* Allocate nothing for 0 mem_sz or 0 record_size. */
	if (mem_sz == 0 || record_size == 0) {
		*cnt = 0;
		return 0;
	}

	/*
	 * If we have a negative record size, calculate it based on
	 * mem_sz / *cnt. If we have a positive record size, calculate
	 * cnt from mem_sz / record_size.
	 */
	if (record_size < 0) {
		if (*cnt == 0)
			return 0;
		record_size = mem_sz / *cnt;
		if (record_size == 0) {
			dev_err(dev, "%s record size == 0 (%zu / %u)\n",
				name, mem_sz, *cnt);
			goto fail;
		}
	} else {
		*cnt = mem_sz / record_size;
		if (*cnt == 0) {
			dev_err(dev, "%s record count == 0 (%zu / %zu)\n",
				name, mem_sz, record_size);
			goto fail;
		}
	}

	if (*paddr + mem_sz - cxt->phys_addr > cxt->size) {
		dev_err(dev, "no room for %s mem region (0x%zx@0x%llx) in (0x%lx@0x%llx)\n",
			name,
			mem_sz, (unsigned long long)*paddr,
			cxt->size, (unsigned long long)cxt->phys_addr);
		goto fail;
	}

	zone_sz = mem_sz / *cnt;
	zone_sz = ALIGN_DOWN(zone_sz, 2);
	if (!zone_sz) {
		dev_err(dev, "%s zone size == 0\n", name);
		goto fail;
	}

	prz_ar = kcalloc(*cnt, sizeof(**przs), GFP_KERNEL);
	if (!prz_ar)
		goto fail;

	for (i = 0; i < *cnt; i++) {
		char *label;

		if (*cnt == 1)
			label = kasprintf(GFP_KERNEL, "ramoops:%s", name);
		else
			label = kasprintf(GFP_KERNEL, "ramoops:%s(%d/%d)",
					  name, i, *cnt - 1);
		prz_ar[i] = persistent_ram_new(*paddr, zone_sz, sig,
					       &cxt->ecc_info,
					       cxt->memtype, flags, label);
		kfree(label);
		if (IS_ERR(prz_ar[i])) {
			err = PTR_ERR(prz_ar[i]);
			dev_err(dev, "failed to request %s mem region (0x%zx@0x%llx): %d\n",
				name, record_size,
				(unsigned long long)*paddr, err);

			while (i > 0) {
				i--;
				persistent_ram_free(&prz_ar[i]);
			}
			kfree(prz_ar);
			prz_ar = NULL;
			goto fail;
		}
		*paddr += zone_sz;
		prz_ar[i]->type = pstore_name_to_type(name);
	}

	*przs = prz_ar;
	return 0;

fail:
	*cnt = 0;
	return err;
}

static int ramoops_init_prz(const char *name,
			    struct device *dev, struct ramoops_context *cxt,
			    struct persistent_ram_zone **prz,
			    phys_addr_t *paddr, size_t sz, u32 sig)
{
	char *label;

	if (!sz)
		return 0;

	if (*paddr + sz - cxt->phys_addr > cxt->size) {
		dev_err(dev, "no room for %s mem region (0x%zx@0x%llx) in (0x%lx@0x%llx)\n",
			name, sz, (unsigned long long)*paddr,
			cxt->size, (unsigned long long)cxt->phys_addr);
		return -ENOMEM;
	}

	label = kasprintf(GFP_KERNEL, "ramoops:%s", name);
	*prz = persistent_ram_new(*paddr, sz, sig, &cxt->ecc_info,
				  cxt->memtype, PRZ_FLAG_ZAP_OLD, label);
	kfree(label);
	if (IS_ERR(*prz)) {
		int err = PTR_ERR(*prz);

		dev_err(dev, "failed to request %s mem region (0x%zx@0x%llx): %d\n",
			name, sz, (unsigned long long)*paddr, err);
		return err;
	}

	*paddr += sz;
	(*prz)->type = pstore_name_to_type(name);

	return 0;
}

/* Read a u32 from a dt property and make sure it's safe for an int. */
static int ramoops_parse_dt_u32(struct platform_device *pdev,
				const char *propname,
				u32 default_value, u32 *value)
{
	u32 val32 = 0;
	int ret;

	ret = of_property_read_u32(pdev->dev.of_node, propname, &val32);
	if (ret == -EINVAL) {
		/* field is missing, use default value. */
		val32 = default_value;
	} else if (ret < 0) {
		dev_err(&pdev->dev, "failed to parse property %s: %d\n",
			propname, ret);
		return ret;
	}

	/* Sanity check our results. */
	if (val32 > INT_MAX) {
		dev_err(&pdev->dev, "%s %u > INT_MAX\n", propname, val32);
		return -EOVERFLOW;
	}

	*value = val32;
	return 0;
}

static int ramoops_parse_dt(struct platform_device *pdev,
			    struct ramoops_platform_data *pdata)
{
	struct device_node *of_node = pdev->dev.of_node;
	struct device_node *parent_node;
	struct resource *res;
	u32 value;
	int ret;

	dev_dbg(&pdev->dev, "using Device Tree\n");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev,
			"failed to locate DT /reserved-memory resource\n");
		return -EINVAL;
	}

	pdata->mem_size = resource_size(res);
	pdata->mem_address = res->start;
	/*
	 * Setting "unbuffered" is deprecated and will be ignored if
	 * "mem_type" is also specified.
	 */
	pdata->mem_type = of_property_read_bool(of_node, "unbuffered");
	/*
	 * Setting "no-dump-oops" is deprecated and will be ignored if
	 * "max_reason" is also specified.
	 */
	if (of_property_read_bool(of_node, "no-dump-oops"))
		pdata->max_reason = KMSG_DUMP_PANIC;
	else
		pdata->max_reason = KMSG_DUMP_OOPS;

#define parse_u32(name, field, default_value) {				\
		ret = ramoops_parse_dt_u32(pdev, name, default_value,	\
					    &value);			\
		if (ret < 0)						\
			return ret;					\
		field = value;						\
	}

	parse_u32("mem-type", pdata->mem_type, pdata->mem_type);
	parse_u32("record-size", pdata->record_size, 0);
	parse_u32("console-size", pdata->console_size, 0);
	parse_u32("ftrace-size", pdata->ftrace_size, 0);
	parse_u32("pmsg-size", pdata->pmsg_size, 0);
	parse_u32("ecc-size", pdata->ecc_info.ecc_size, 0);
	parse_u32("flags", pdata->flags, 0);
	parse_u32("max-reason", pdata->max_reason, pdata->max_reason);

#undef parse_u32

	/*
	 * Some old Chromebooks relied on the kernel setting the
	 * console_size and pmsg_size to the record size since that's
	 * what the downstream kernel did.  These same Chromebooks had
	 * "ramoops" straight under the root node which isn't
	 * according to the current upstream bindings (though it was
	 * arguably acceptable under a prior version of the bindings).
	 * Let's make those old Chromebooks work by detecting that
	 * we're not a child of "reserved-memory" and mimicking the
	 * expected behavior.
	 */
	parent_node = of_get_parent(of_node);
	if (!of_node_name_eq(parent_node, "reserved-memory") &&
	    !pdata->console_size && !pdata->ftrace_size &&
	    !pdata->pmsg_size && !pdata->ecc_info.ecc_size) {
		pdata->console_size = pdata->record_size;
		pdata->pmsg_size = pdata->record_size;
	}
	of_node_put(parent_node);

	return 0;
}

/* DEBUG run115: silent resets are traceless and now proven independent
 * of any MDSS/display path (runs 110/114). Stock 4.19 pets the APPS WDT
 * from its msm_watchdog thread; this tree builds no watchdog driver at
 * all, so a firmware-armed WDT would bark unheard and bite silently.
 * Snapshot the KPSS WDT block (stock: b017000.qcom,wdt) at probe time.
 * Pure reads; no side effects.
 */
#define TISSOT_APPS_WDT_BASE	0xb017000
#define TISSOT_APPS_WDT_EN	0x40
#define TISSOT_APPS_WDT_STS	0x44
#define TISSOT_APPS_WDT_BARK	0x4c
#define TISSOT_APPS_WDT_BITE	0x5c

static u32 ramoops_apps_wdt_last[4];
static void ramoops_debug_apps_wdt(void)
{
	void __iomem *wdt;
	u32 en, sts, bark, bite;
	u32 en2, sts2, bark2, bite2;
	static unsigned int calls;

	/*
	 * run266: this runs from the panic and reset paths and from the
	 * periodic snapshot, and it printed 3070 times in one boot - two
	 * lines per call - which is a quarter of the console the kgsl
	 * evidence needs. The registers do not change between calls
	 * (en/sts/bark/bite are stable in every run266 sample), so print the
	 * first few calls and then only when the values change.
	 */
	wdt = ioremap(TISSOT_APPS_WDT_BASE, SZ_4K);
	if (!wdt) {
		pr_info("DIAG apps_wdt ioremap failed\n");
		return;
	}
	en = readl_relaxed(wdt + TISSOT_APPS_WDT_EN);
	sts = readl_relaxed(wdt + TISSOT_APPS_WDT_STS);
	bark = readl_relaxed(wdt + TISSOT_APPS_WDT_BARK);
	bite = readl_relaxed(wdt + TISSOT_APPS_WDT_BITE);
	/* Same block, APCS-timer layout (used by newer low-tier APSS
	 * WDTs): disambiguates "disabled" from "wrong offsets".
	 */
	en2 = readl_relaxed(wdt + 0x8);
	sts2 = readl_relaxed(wdt + 0xc);
	bark2 = readl_relaxed(wdt + 0x10);
	bite2 = readl_relaxed(wdt + 0x14);
	iounmap(wdt);

	if (calls < 8) {
		calls++;
		pr_info("DIAG apps_wdt en=%08x sts=%08x bark=%08x bite=%08x\n",
			en, sts, bark, bite);
		pr_info("DIAG apps_wdt2 en=%08x sts=%08x bark=%08x bite=%08x\n",
			en2, sts2, bark2, bite2);
		return;
	}

	/* After that, only when something actually moved. */
	if (en != ramoops_apps_wdt_last[0] || sts != ramoops_apps_wdt_last[1] ||
	    bark != ramoops_apps_wdt_last[2] || bite != ramoops_apps_wdt_last[3]) {
		ramoops_apps_wdt_last[0] = en;
		ramoops_apps_wdt_last[1] = sts;
		ramoops_apps_wdt_last[2] = bark;
		ramoops_apps_wdt_last[3] = bite;
		pr_info("DIAG apps_wdt changed en=%08x sts=%08x bark=%08x bite=%08x\n",
			en, sts, bark, bite);
	}
}

static void __iomem *ramoops_diag_map(void)
{
	struct page *page;

	/* Marks live in 4K carved from the dmesg area (between the dmesg
	 * and console zones), since the 1MB reservation packs zones with
	 * zero slack. The carve happens in ramoops_probe below; until it
	 * succeeds there is no window and marks refuse loudly.
	 *
	 * The reservation is linear-mapped RAM ("map non-reusable"), which
	 * arm64 ioremap() rejects with a WARN; vmap it write-combined like
	 * persistent_ram_vmap() does for the other zones. Probe maps it
	 * first, so later marks never reach the (sleeping) vmap.
	 */
	if (!ramoops_diag_phys)
		return NULL;
	if (!ramoops_diag_marks) {
		if (pfn_valid(PHYS_PFN(ramoops_diag_phys))) {
			page = pfn_to_page(PHYS_PFN(ramoops_diag_phys));
			ramoops_diag_marks = (void __iomem *)vmap(&page, 1,
					VM_MAP | VM_IOREMAP,
					pgprot_writecombine(PAGE_KERNEL));
		} else {
			ramoops_diag_marks = ioremap_wc(ramoops_diag_phys,
							RAMOOPS_DIAG_MARK_SIZE);
		}
	}
	return ramoops_diag_marks;
}

void ramoops_diag_mark(loff_t slot, u32 tag, u32 val)
{
	void __iomem *pos;
	u32 check_tag, check_val;

	if (slot < 0 || slot + 8 > RAMOOPS_DIAG_MARK_HDR) {
		pr_err("ramoops: diag mark slot out of range (%llx)\n", slot);
		return;
	}
	if (!ramoops_diag_map()) {
		pr_err("ramoops: diag mark unmapped (slot %llx)\n", slot);
		return;
	}
	/* Device-memory writes complete on the bus in order; the readback
	 * proves the pair reached the reserved region. Disjoint 8-byte
	 * slots need no locking (idempotent rewrites are harmless).
	 */
	pos = ramoops_diag_marks + slot;
	writel_relaxed(tag, pos);
	writel_relaxed(val, pos + 4);
	check_tag = readl_relaxed(pos);
	check_val = readl_relaxed(pos + 4);
	if (check_tag != tag || check_val != val)
		pr_err("ramoops: diag mark readback slot %llx got %08x/%08x want %08x/%08x\n",
		       slot, check_tag, check_val, tag, val);
}
EXPORT_SYMBOL_GPL(ramoops_diag_mark);

/* Dump the previous boot's marks before this boot writes its own, then
 * reset the page. Only a page carrying the mark magic is trusted: after
 * a silent reset the phone boots stock 4.19, which does not reserve
 * this range, so the next port boot usually finds stock's leftovers.
 * The reliable read path is the logdump snapshot, which copies this
 * page every RAMOOPS_SNAP_INTERVAL_MS; the boot id ties those copies
 * to this boot's log.
 */
static void ramoops_diag_dump_prev(void)
{
	void __iomem *page = ramoops_diag_map();
	u32 magic, boot_id;
	loff_t slot;

	if (!page) {
		pr_err("ramoops: diag mark page unmapped\n");
		return;
	}
#if IS_BUILTIN(CONFIG_PSTORE_RAM) && IS_ENABLED(CONFIG_BLOCK)
	if (ramoops_reset_capture)
		memcpy_fromio(ramoops_reset_previous, page,
			      sizeof(ramoops_reset_previous));
#endif
	magic = readl_relaxed(page + RAMOOPS_DIAG_MARK_HDR);
	boot_id = readl_relaxed(page + RAMOOPS_DIAG_MARK_HDR + 4);
	if (magic != RAMOOPS_DIAG_MARK_MAGIC) {
		pr_info("DIAG mark prev none (magic %08x)\n", magic);
	} else {
		pr_info("DIAG mark prev boot_id %08x\n", boot_id);
		for (slot = 0; slot < RAMOOPS_DIAG_MARK_HDR; slot += 8) {
			u32 tag = readl_relaxed(page + slot);
			u32 val = readl_relaxed(page + slot + 4);

			if (tag || val)
				pr_info("DIAG mark prev slot %02llx tag %08x val %08x\n",
					slot, tag, val);
		}
	}

	boot_id = get_random_u32();
	memset_io(page, 0, RAMOOPS_DIAG_MARK_HDR);
	writel_relaxed(boot_id, page + RAMOOPS_DIAG_MARK_HDR + 4);
	writel_relaxed(RAMOOPS_DIAG_MARK_MAGIC, page + RAMOOPS_DIAG_MARK_HDR);
#if IS_BUILTIN(CONFIG_PSTORE_RAM) && IS_ENABLED(CONFIG_BLOCK)
	ramoops_reset_seen = 0;
#endif
	pr_info("DIAG mark boot_id %08x\n", boot_id);
}

/*
 * DEBUG run120 (tissot): inspect the boot CPU clock registers. CPU clock
 * handoff is owned by apcc-msm8953, not this diagnostic reader.
 * Layout from stock 4.19 clock-cpu-8953.c / msm8953.dtsi:975:
 * one APCS HF PLL (rate = 19.2 MHz * L), and per-cluster RCG muxes
 * whose CFG (CMD_RCGR + 4) holds SRC_SEL[10:8] and a half-integer
 * divider DIV[4:0] (divide by (DIV + 1) / 2). Stock selects SRC 5
 * (PLL early) for both clusters and CCI. Reads only.
 */
static void ramoops_debug_cpuclk(void)
{
	static const struct {
		const char *name;
		phys_addr_t rcgr;
	} mux[] = {
		{ "c0", 0xb111050 },	/* little cluster, CPU0-3 */
		{ "c1", 0xb011050 },	/* big cluster, CPU4-7 */
		{ "cci", 0xb1d1050 },
	};
	void __iomem *pll, *rcg;
	u32 mode, l_val, user, cmd, cfg, src, div;
	unsigned long pll_khz;
	int i;

	pll = ioremap(0xb116000, SZ_4K);
	if (!pll) {
		pr_info("DIAG cpuclk pll ioremap failed\n");
		return;
	}
	mode = readl_relaxed(pll + 0x0);
	l_val = readl_relaxed(pll + 0x8);
	user = readl_relaxed(pll + 0x18);
	iounmap(pll);
	pll_khz = 19200UL * l_val;
	pr_info("DIAG cpuclk pll mode=%08x lock=%u l=%u user=%08x rate=%lu kHz\n",
		mode, !!(mode & BIT(31)), l_val, user, pll_khz);

	for (i = 0; i < ARRAY_SIZE(mux); i++) {
		rcg = ioremap(mux[i].rcgr & ~(phys_addr_t)0xfff, SZ_4K);
		if (!rcg) {
			pr_info("DIAG cpuclk %s ioremap failed\n", mux[i].name);
			continue;
		}
		cmd = readl_relaxed(rcg + (mux[i].rcgr & 0xfff));
		cfg = readl_relaxed(rcg + (mux[i].rcgr & 0xfff) + 4);
		iounmap(rcg);
		src = (cfg >> 8) & 0x7;
		div = cfg & 0x1f;
		pr_info("DIAG cpuclk %s cmd=%08x cfg=%08x src=%u div=%u rate=%lu kHz%s\n",
			mux[i].name, cmd, cfg, src, div,
			src == 5 ? pll_khz * 2 / (div + 1) : 0,
			src == 5 ? "" : " (src not PLL: rate unknown)");
	}
}

static int ramoops_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ramoops_platform_data *pdata = dev->platform_data;
	struct ramoops_platform_data pdata_local;
	struct ramoops_context *cxt = &oops_cxt;
	size_t dump_mem_sz;
	phys_addr_t paddr;
	int err = -EINVAL;

	/*
	 * Only a single ramoops area allowed at a time, so fail extra
	 * probes.
	 */
	if (cxt->max_dump_cnt) {
		pr_err("already initialized\n");
		goto fail_out;
	}

	ramoops_debug_apps_wdt();
	ramoops_debug_cpuclk();

	if (dev_of_node(dev) && !pdata) {
		pdata = &pdata_local;
		memset(pdata, 0, sizeof(*pdata));

		err = ramoops_parse_dt(pdev, pdata);
		if (err < 0)
			goto fail_out;
	}

	/* Make sure we didn't get bogus platform data pointer. */
	if (!pdata) {
		pr_err("NULL platform data\n");
		err = -EINVAL;
		goto fail_out;
	}

	if (!pdata->mem_size || (!pdata->record_size && !pdata->console_size &&
			!pdata->ftrace_size && !pdata->pmsg_size)) {
		pr_err("The memory size and the record/console size must be "
			"non-zero\n");
		err = -EINVAL;
		goto fail_out;
	}

	if (pdata->record_size && !is_power_of_2(pdata->record_size))
		pdata->record_size = rounddown_pow_of_two(pdata->record_size);
	if (pdata->console_size && !is_power_of_2(pdata->console_size))
		pdata->console_size = rounddown_pow_of_two(pdata->console_size);
	if (pdata->ftrace_size && !is_power_of_2(pdata->ftrace_size))
		pdata->ftrace_size = rounddown_pow_of_two(pdata->ftrace_size);
	if (pdata->pmsg_size && !is_power_of_2(pdata->pmsg_size))
		pdata->pmsg_size = rounddown_pow_of_two(pdata->pmsg_size);

	cxt->size = pdata->mem_size;
	cxt->phys_addr = pdata->mem_address;
	cxt->memtype = pdata->mem_type;
	cxt->record_size = pdata->record_size;
	cxt->console_size = pdata->console_size;
	cxt->ftrace_size = pdata->ftrace_size;
	cxt->pmsg_size = pdata->pmsg_size;
	cxt->flags = pdata->flags;
	cxt->ecc_info = pdata->ecc_info;

	paddr = cxt->phys_addr;

	dump_mem_sz = cxt->size - cxt->console_size - cxt->ftrace_size
			- cxt->pmsg_size - RAMOOPS_DIAG_MARK_SIZE;
	err = ramoops_init_przs("dmesg", dev, cxt, &cxt->dprzs, &paddr,
				dump_mem_sz, cxt->record_size,
				&cxt->max_dump_cnt, 0, 0);
	if (err)
		goto fail_init;

	/* DEBUG run117: 4K for diag marks between dmesg and console. One
	 * fewer dmesg record; console/pmsg/ftrace follow unshifted in
	 * content, 4K later in address — total still exactly 1MB.
	 */
	ramoops_diag_phys = paddr;
	paddr += RAMOOPS_DIAG_MARK_SIZE;

	err = ramoops_init_prz("console", dev, cxt, &cxt->cprz, &paddr,
			       cxt->console_size, 0);
	if (err)
		goto fail_init;

	err = ramoops_init_prz("pmsg", dev, cxt, &cxt->mprz, &paddr,
				cxt->pmsg_size, 0);
	if (err)
		goto fail_init;

	cxt->max_ftrace_cnt = (cxt->flags & RAMOOPS_FLAG_FTRACE_PER_CPU)
				? nr_cpu_ids
				: 1;
	err = ramoops_init_przs("ftrace", dev, cxt, &cxt->fprzs, &paddr,
				cxt->ftrace_size, -1,
				&cxt->max_ftrace_cnt, LINUX_VERSION_CODE,
				(cxt->flags & RAMOOPS_FLAG_FTRACE_PER_CPU)
					? PRZ_FLAG_NO_LOCK : 0);
	if (err)
		goto fail_init;

	ramoops_diag_dump_prev();

	cxt->pstore.data = cxt;
	/*
	 * Prepare frontend flags based on which areas are initialized.
	 * For ramoops_init_przs() cases, the "max count" variable tells
	 * if there are regions present. For ramoops_init_prz() cases,
	 * the single region size is how to check.
	 */
	cxt->pstore.flags = 0;
	if (cxt->max_dump_cnt) {
		cxt->pstore.flags |= PSTORE_FLAGS_DMESG;
		cxt->pstore.max_reason = pdata->max_reason;
	}
	if (cxt->console_size)
		cxt->pstore.flags |= PSTORE_FLAGS_CONSOLE;
	if (cxt->max_ftrace_cnt)
		cxt->pstore.flags |= PSTORE_FLAGS_FTRACE;
	if (cxt->pmsg_size)
		cxt->pstore.flags |= PSTORE_FLAGS_PMSG;

	/*
	 * Since bufsize is only used for dmesg crash dumps, it
	 * must match the size of the dprz record (after PRZ header
	 * and ECC bytes have been accounted for).
	 */
	if (cxt->pstore.flags & PSTORE_FLAGS_DMESG) {
		cxt->pstore.bufsize = cxt->dprzs[0]->buffer_size;
		cxt->pstore.buf = kvzalloc(cxt->pstore.bufsize, GFP_KERNEL);
		if (!cxt->pstore.buf) {
			pr_err("cannot allocate pstore crash dump buffer\n");
			err = -ENOMEM;
			goto fail_clear;
		}
	}

	err = pstore_register(&cxt->pstore);
	if (err) {
		pr_err("registering with pstore failed\n");
		goto fail_buf;
	}

	if (!ramoops_raw_geometry_valid(cxt)) {
		pr_err("snapshot35 ramoops_raw geometry mismatch; exporter omitted\n");
	} else {
		err = sysfs_create_bin_file(kernel_kobj, &bin_attr_ramoops_raw);
		if (err) {
			pr_err("snapshot35 ramoops_raw creation failed errno=%d; pstore continues\n",
			       err);
		} else {
			cxt->raw_published = true;
			pr_info("snapshot35 ramoops_raw ready phys=0x86800000 size=1048576\n");
		}
	}

#if IS_BUILTIN(CONFIG_PSTORE_RAM) && IS_ENABLED(CONFIG_BLOCK)
	/*
	 * Start the kernel-side snapshotter only once pstore is registered
	 * and the same validated geometry backed the exporter, so it can
	 * only copy mappings that are actually live.
	 */
	if (cxt->raw_published) {
		smp_store_release(&ramoops_reset_ready, ramoops_reset_capture);
		ramoops_stage_start();
		ramoops_snap_start(cxt);
	}
#endif

	/*
	 * Update the module parameter variables as well so they are visible
	 * through /sys/module/ramoops/parameters/
	 */
	mem_size = pdata->mem_size;
	mem_address = pdata->mem_address;
	record_size = pdata->record_size;
	ramoops_max_reason = pdata->max_reason;
	ramoops_console_size = pdata->console_size;
	ramoops_pmsg_size = pdata->pmsg_size;
	ramoops_ftrace_size = pdata->ftrace_size;

	pr_info("using 0x%lx@0x%llx, ecc: %d\n",
		cxt->size, (unsigned long long)cxt->phys_addr,
		cxt->ecc_info.ecc_size);

	return 0;

fail_buf:
	kvfree(cxt->pstore.buf);
fail_clear:
	cxt->pstore.bufsize = 0;
fail_init:
	ramoops_free_przs(cxt);
fail_out:
	return err;
}

static void ramoops_remove(struct platform_device *pdev)
{
	struct ramoops_context *cxt = &oops_cxt;

#if IS_BUILTIN(CONFIG_PSTORE_RAM) && IS_ENABLED(CONFIG_BLOCK)
	smp_store_release(&ramoops_reset_ready, false);
	ramoops_stage_stop();
	/*
	 * Drain the snapshot worker first: it reads the same zone mappings
	 * the exporter serves, and kthread_stop() waits for the in-flight
	 * block write to finish before any zone is freed.
	 */
	ramoops_snap_stop();
#endif

	/* kernfs drains active reads before the existing mappings are freed. */
	if (cxt->raw_published) {
		sysfs_remove_bin_file(kernel_kobj, &bin_attr_ramoops_raw);
		cxt->raw_published = false;
	}

	pstore_unregister(&cxt->pstore);

	kvfree(cxt->pstore.buf);
	cxt->pstore.bufsize = 0;

	ramoops_free_przs(cxt);
}

static const struct of_device_id dt_match[] = {
	{ .compatible = "ramoops" },
	{}
};
MODULE_DEVICE_TABLE(of, dt_match);

static struct platform_driver ramoops_driver = {
	.probe		= ramoops_probe,
	.remove_new	= ramoops_remove,
	.driver		= {
		.name		= "ramoops",
		.of_match_table	= dt_match,
	},
};

static inline void ramoops_unregister_dummy(void)
{
	platform_device_unregister(dummy);
	dummy = NULL;
}

static void __init ramoops_register_dummy(void)
{
	struct ramoops_platform_data pdata;

	if (mem_name) {
		phys_addr_t start;
		phys_addr_t size;

		if (reserve_mem_find_by_name(mem_name, &start, &size)) {
			mem_address = start;
			mem_size = size;
		}
	}

	/*
	 * Prepare a dummy platform data structure to carry the module
	 * parameters. If mem_size isn't set, then there are no module
	 * parameters, and we can skip this.
	 */
	if (!mem_size)
		return;

	pr_info("using module parameters\n");

	memset(&pdata, 0, sizeof(pdata));
	pdata.mem_size = mem_size;
	pdata.mem_address = mem_address;
	pdata.mem_type = mem_type;
	pdata.record_size = record_size;
	pdata.console_size = ramoops_console_size;
	pdata.ftrace_size = ramoops_ftrace_size;
	pdata.pmsg_size = ramoops_pmsg_size;
	/* If "max_reason" is set, its value has priority over "dump_oops". */
	if (ramoops_max_reason >= 0)
		pdata.max_reason = ramoops_max_reason;
	/* Otherwise, if "dump_oops" is set, parse it into "max_reason". */
	else if (ramoops_dump_oops != -1)
		pdata.max_reason = ramoops_dump_oops ? KMSG_DUMP_OOPS
						     : KMSG_DUMP_PANIC;
	/* And if neither are explicitly set, use the default. */
	else
		pdata.max_reason = KMSG_DUMP_OOPS;
	pdata.flags = RAMOOPS_FLAG_FTRACE_PER_CPU;

	/*
	 * For backwards compatibility ramoops.ecc=1 means 16 bytes ECC
	 * (using 1 byte for ECC isn't much of use anyway).
	 */
	pdata.ecc_info.ecc_size = ramoops_ecc == 1 ? 16 : ramoops_ecc;

	dummy = platform_device_register_data(NULL, "ramoops", -1,
			&pdata, sizeof(pdata));
	if (IS_ERR(dummy)) {
		pr_info("could not create platform device: %ld\n",
			PTR_ERR(dummy));
		dummy = NULL;
	}
}

static int __init ramoops_init(void)
{
	int ret;

	ramoops_register_dummy();
	ret = platform_driver_register(&ramoops_driver);
	if (ret != 0)
		ramoops_unregister_dummy();

	return ret;
}
postcore_initcall(ramoops_init);

static void __exit ramoops_exit(void)
{
	platform_driver_unregister(&ramoops_driver);
	ramoops_unregister_dummy();
}
module_exit(ramoops_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Marco Stornelli <marco.stornelli@gmail.com>");
MODULE_DESCRIPTION("RAM Oops/Panic logger/driver");
