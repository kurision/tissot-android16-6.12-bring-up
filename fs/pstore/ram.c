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
#include <linux/device/class.h>
#endif
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/timekeeping.h>
#include <linux/vmalloc.h>

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
#define RAMOOPS_RAW_PMSG 0xf7000
#define RAMOOPS_RAW_FTRACE 0xff000

static bool ramoops_raw_zone_valid(struct persistent_ram_zone *prz,
				  size_t offset, size_t size)
{
	return prz && prz->vaddr && prz->size == size &&
		prz->paddr == RAMOOPS_RAW_PHYS + offset;
}

static bool ramoops_raw_geometry_valid(struct ramoops_context *cxt)
{
	unsigned int i;

	if (cxt->phys_addr != RAMOOPS_RAW_PHYS || cxt->size != SZ_1M ||
	    cxt->record_size != SZ_4K || cxt->console_size != SZ_512K ||
	    cxt->pmsg_size != SZ_32K || cxt->ftrace_size != SZ_4K ||
	    cxt->max_dump_cnt != 119 || cxt->max_ftrace_cnt != 1 ||
	    !cxt->dprzs || !cxt->fprzs || cxt->ecc_info.ecc_size ||
	    (cxt->flags & RAMOOPS_FLAG_FTRACE_PER_CPU))
		return false;
	for (i = 0; i < 119; i++)
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
#define RAMOOPS_SNAP_MAGIC		0x52414d4fULL	/* "RAMO" */

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
	struct task_struct *task;
	struct completion wake;
	u64 seq;
	unsigned int slot;		/* next slot to write */
	bool warned;
};

static struct ramoops_snapper ramoops_snapper;

static_assert(sizeof(struct ramoops_snap_header) == RAMOOPS_SNAP_HDR_SIZE);

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
	s->bdev = file_bdev(s->file);
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

		if (!page || bio_add_page(bio, page, chunk, 0) < 0) {
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

static int ramoops_snap_take(struct ramoops_snapper *s)
{
	struct ramoops_snap_header *hdr = s->buf;
	void *payload = s->buf + PAGE_SIZE;
	unsigned int slot = s->slot % RAMOOPS_SNAP_SLOTS;
	u64 base = RAMOOPS_SNAP_OFFSET + slot * RAMOOPS_SNAP_SLOT_STRIDE;
	ssize_t copied;
	int err;

	copied = ramoops_raw_copy(s->cxt, payload, 0, SZ_1M);
	if (copied < 0)
		return (int)copied;

	s->seq++;

	memset(s->buf, 0, PAGE_SIZE);
	hdr->magic = cpu_to_le64(RAMOOPS_SNAP_MAGIC);
	hdr->seq = cpu_to_le64(s->seq);
	hdr->index = cpu_to_le64(s->seq);	/* sample index within this boot */
	hdr->timestamp_ns = cpu_to_le64(ktime_get_ns());
	hdr->payload_offset = cpu_to_le32(PAGE_SIZE);
	hdr->payload_len = cpu_to_le32(SZ_1M);
	hdr->phys_addr_lo = cpu_to_le32(RAMOOPS_RAW_PHYS);
	hdr->slot = cpu_to_le32(slot);

	/* Header page first, so a recovery sees this sample's identity. */
	err = ramoops_snap_bio_write(s->bdev, s->buf, PAGE_SIZE, base);
	if (err)
		return err;

	err = ramoops_snap_bio_write(s->bdev, payload, SZ_1M,
				     base + RAMOOPS_SNAP_PAYLOAD_OFFSET -
				     RAMOOPS_SNAP_OFFSET);
	if (err)
		return err;

	/* Reach stable storage: the last sample before a freeze is the
	 * one this whole channel exists to preserve. */
	err = blkdev_issue_flush(s->bdev);
	if (err)
		pr_warn("snapshot38 logdump flush failed errno=%d\n", err);

	/* Advance only after a fully flushed sample. */
	s->slot = slot + 1;
	return 0;
}

/*
 * The thread retries the open because MMC block devices register at
 * device_initcall, long after ramoops probes at postcore_initcall.
 */
static int ramoops_snap_thread(void *unused)
{
	struct ramoops_snapper *s = &ramoops_snapper;
	(void)unused;

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

		if (ramoops_snap_take(s)) {
			pr_err("snapshot38 logdump snapshot failed; disabled\n");
			break;
		}
		if (!s->warned) {
			s->warned = true;
			pr_info("snapshot38 logdump snapshotter ready offset=%llu slots=%u size=%u\n",
				RAMOOPS_SNAP_OFFSET, RAMOOPS_SNAP_SLOTS, SZ_1M);
		}
		wait_for_completion_interruptible_timeout(
			&s->wake, msecs_to_jiffies(RAMOOPS_SNAP_INTERVAL_MS));
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

/*
 * QSEE trace ring (snapshot45 diagnostic): one FUA write per record so it
 * survives a reset. The record format is defined in drivers/misc/qseecom.c.
 */
#define RAMOOPS_QTRACE_OFFSET	0x3f10000ULL
#define RAMOOPS_QTRACE_SLOTS	128

static struct file *ramoops_witness_file;
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

int ramoops_witness_write(const void *data, size_t len)
{
	int err;

	mutex_lock(&ramoops_witness_lock);
	err = ramoops_witness_write_at(RAMOOPS_WITNESS_OFFSET, REQ_OP_WRITE,
				       data, len);
	mutex_unlock(&ramoops_witness_lock);
	return err;
}
EXPORT_SYMBOL_GPL(ramoops_witness_write);

int ramoops_qtrace_write(u64 seq, const void *data, size_t len)
{
	int err;

	mutex_lock(&ramoops_witness_lock);
	err = ramoops_witness_write_at(RAMOOPS_QTRACE_OFFSET +
			(seq % RAMOOPS_QTRACE_SLOTS) * RAMOOPS_WITNESS_LEN,
			REQ_OP_WRITE | REQ_SYNC | REQ_FUA, data, len);
	mutex_unlock(&ramoops_witness_lock);
	return err;
}
EXPORT_SYMBOL_GPL(ramoops_qtrace_write);

int ramoops_witness_read(void *data, size_t len)
{
	struct bio *bio;
	int err;

	if (len > RAMOOPS_WITNESS_LEN)
		return -EINVAL;
	mutex_lock(&ramoops_witness_lock);
	err = ramoops_witness_open();
	if (err)
		goto out;
	memset(ramoops_witness_buf, 0, RAMOOPS_WITNESS_LEN);

	bio = bio_alloc(ramoops_witness_bdev, 1, REQ_OP_READ,
			GFP_KERNEL | __GFP_NOFAIL);
	if (!bio) {
		err = -ENOMEM;
		goto out;
	}
	if (bio_add_page(bio, vmalloc_to_page(ramoops_witness_buf),
			 RAMOOPS_WITNESS_LEN, 0) != RAMOOPS_WITNESS_LEN) {
		bio_put(bio);
		err = -EIO;
		goto out;
	}
	bio->bi_iter.bi_sector = RAMOOPS_WITNESS_OFFSET >> SECTOR_SHIFT;
	bio->bi_iter.bi_size = RAMOOPS_WITNESS_LEN;
	submit_bio_wait(bio);
	err = blk_status_to_errno(bio->bi_status);
	if (!err)
		memcpy(data, ramoops_witness_buf, len);
	bio_put(bio);
out:
	mutex_unlock(&ramoops_witness_lock);
	return err;
}
EXPORT_SYMBOL_GPL(ramoops_witness_read);

static void ramoops_snap_start(struct ramoops_context *cxt)
{
	struct ramoops_snapper *s = &ramoops_snapper;

	memset(s, 0, sizeof(*s));
	s->cxt = cxt;
	init_completion(&s->wake);

	s->buf = __vmalloc(PAGE_SIZE + SZ_1M, GFP_KERNEL);
	if (!s->buf) {
		pr_err("snapshot38 staging buffer alloc failed; snapshotter disabled\n");
		return;
	}

	s->task = kthread_create(ramoops_snap_thread, s, "ramoops_snap");
	if (IS_ERR(s->task)) {
		pr_err("snapshot38 thread create failed errno=%ld; snapshotter disabled\n",
		       PTR_ERR(s->task));
		s->task = NULL;
		vfree(s->buf);
		s->buf = NULL;
		return;
	}
	wake_up_process(s->task);
}


/* Drain the worker before any zone mapping is freed or unmapped. */
static void ramoops_snap_stop(void)
{
	struct ramoops_snapper *s = &ramoops_snapper;

	if (s->task) {
		kthread_stop(s->task);
		s->task = NULL;
	}
	if (s->buf) {
		vfree(s->buf);
		s->buf = NULL;
	}
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
			- cxt->pmsg_size;
	err = ramoops_init_przs("dmesg", dev, cxt, &cxt->dprzs, &paddr,
				dump_mem_sz, cxt->record_size,
				&cxt->max_dump_cnt, 0, 0);
	if (err)
		goto fail_init;

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
	if (cxt->raw_published)
		ramoops_snap_start(cxt);
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
