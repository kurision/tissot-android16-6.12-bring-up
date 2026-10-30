/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026, The tissot kgsl-port authors.
 *
 * 6.12 deleted the 4.19 subsystem_get() "look up a remoteproc by name"
 * registry that the a5xx/a6xx zap-shader paths used. Stock gets the zap
 * shader through PIL (soc:qcom,kgsl-hyp, qcom,pas-id = 13,
 * firmware-name "a506_zap"); mainline loads the same image itself from
 * the GPU node's zap-shader child, which msm8953.dtsi already carries.
 * This follows drivers/gpu/drm/msm/adreno/adreno_gpu.c
 * zap_shader_load_mdt().
 *
 * The A5xx comes out of reset in secure mode. Only an authenticated zap
 * shader lets the CP leave it (CP_SET_SECURE_MODE 0); the fallback write
 * of RBBM_SECVID_TRUST_CNTL from the CPU is blocked on content-protected
 * parts (see a5xx_hw_init() in drm/msm).
 *
 * The callers only test the result for an error, so a non-error pointer
 * means "loaded".
 */

#ifndef _KGSL_ZAP_COMPAT_H
#define _KGSL_ZAP_COMPAT_H

#include <linux/bitops.h>
#include <linux/build_bug.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pstore_ram.h>
#include <linux/soc/qcom/mdt_loader.h>

#define ADRENO_ZAP_PAS_ID	13

/*
 * run203 reset the board ~41 s into boot, at the first GPU start, with no
 * log surviving. Stages are queued for FUA writes to the logdump by
 * fs/pstore/ram.c; enqueue completion does not establish persistence.
 * Slots 7-15 follow GPU startup and fault capture after authentication;
 * slots 16-30 follow first-open and power
 * transitions. Each has one call site. Slot = stage.
 * Slots 16-22 use mem_phys/size for PID/current state; slots 23-30 use them
 * for current/requested state. These are software values, not register reads.
 */
#define ADRENO_ZAP_STAGE_MAGIC	0x454741545350415aULL	/* "ZAPSTAGE" */

enum adreno_zap_stage {
	ZAP_STAGE_ENTER,
	ZAP_STAGE_FW_LOADED,
	ZAP_STAGE_PAS_INIT,	/* before qcom_mdt_pas_init: init_image+mem_setup */
	ZAP_STAGE_PAS_INIT_DONE,
	ZAP_STAGE_COPY_DONE,	/* after qcom_mdt_load_no_init */
	ZAP_STAGE_AUTH,		/* before qcom_scm_pas_auth_and_reset */
	ZAP_STAGE_AUTH_DONE,
	ZAP_STAGE_UNSECURE_DONE,
	ZAP_STAGE_POST_START_ENTER,
	ZAP_STAGE_POST_START_DONE,
	ZAP_STAGE_GPU_START_DONE,
	ZAP_STAGE_SUBMIT_ENTER,
	ZAP_STAGE_SUBMIT_DONE,
	ZAP_STAGE_SNAPSHOT_ENTER,
	ZAP_STAGE_CRASHDUMP_DONE,
	ZAP_STAGE_CPU_FALLBACK,
	ZAP_STAGE_OPEN_START_DONE,
	ZAP_STAGE_OPEN_ACTIVE_DONE,
	ZAP_STAGE_OPEN_PUT_DONE,
	ZAP_STAGE_OPEN_DEVICE_DONE,
	ZAP_STAGE_PROCESS_OPEN_ENTER,
	ZAP_STAGE_PROCESS_OPEN_DONE,
	ZAP_STAGE_OPEN_DONE,
	ZAP_STAGE_IDLE_ENTER,
	ZAP_STAGE_IDLE_DONE,
	ZAP_STAGE_ACTIVE_ENTER,
	ZAP_STAGE_ACTIVE_DONE,
	ZAP_STAGE_NAP_ENTER,
	ZAP_STAGE_NAP_DONE,
	ZAP_STAGE_SLUMBER_ENTER,
	ZAP_STAGE_SLUMBER_DONE,
};

static_assert(ZAP_STAGE_SLUMBER_DONE < 32);

struct adreno_zap_stage_rec {
	__le64 magic;
	__le32 stage;
	__le32 ret;
	__le64 ktime_ns;
	__le64 mem_phys;
	__le64 size;
};

static_assert(sizeof(struct adreno_zap_stage_rec) == 40);

static inline void adreno_zap_stage(struct device *dev,
		enum adreno_zap_stage stage, int ret, phys_addr_t phys,
		ssize_t size)
{
#if 0 /* Legacy GPU bring-up stage records. */
	static unsigned long recorded;
	struct adreno_zap_stage_rec rec = {
		.magic = cpu_to_le64(ADRENO_ZAP_STAGE_MAGIC),
		.stage = cpu_to_le32(stage),
		.ret = cpu_to_le32(ret),
		.ktime_ns = cpu_to_le64(ktime_get_ns()),
		.mem_phys = cpu_to_le64(phys),
		.size = cpu_to_le64(size),
	};

	/* Preserve the first visit when recovery restarts the GPU. */
	if (stage >= ZAP_STAGE_UNSECURE_DONE &&
	    test_and_set_bit(stage, &recorded))
		return;

	ramoops_stage_write(stage, &rec, sizeof(rec));
#endif
}

static inline void *adreno_zap_subsystem_get(struct device *dev,
		const char *name)
{
	struct device_node *np, *mem_np;
	const struct firmware *fw;
	struct resource r;
	char *fwname;
	void *mem;
	ssize_t size;
	int ret;

	np = of_get_child_by_name(dev->of_node, "zap-shader");
	if (!of_device_is_available(np)) {
		of_node_put(np);
		return ERR_PTR(-ENODEV);
	}

	mem_np = of_parse_phandle(np, "memory-region", 0);
	of_node_put(np);
	if (!mem_np)
		return ERR_PTR(-EINVAL);

	ret = of_address_to_resource(mem_np, 0, &r);
	of_node_put(mem_np);
	if (ret)
		return ERR_PTR(ret);

	if (!qcom_scm_is_available() ||
	    !qcom_scm_pas_supported(ADRENO_ZAP_PAS_ID)) {
		dev_err(dev, "DIAG zap: SCM PAS %d not supported\n",
			ADRENO_ZAP_PAS_ID);
		return ERR_PTR(-EOPNOTSUPP);
	}

	adreno_zap_stage(dev, ZAP_STAGE_ENTER, 0, r.start, resource_size(&r));

	fwname = kasprintf(GFP_KERNEL, "%s.mdt", name);
	if (!fwname)
		return ERR_PTR(-ENOMEM);

	ret = request_firmware(&fw, fwname, dev);
	if (ret) {
		dev_err(dev, "DIAG zap: request %s failed: %d\n", fwname, ret);
		goto out_name;
	}

	size = qcom_mdt_get_size(fw);
	adreno_zap_stage(dev, ZAP_STAGE_FW_LOADED, 0, r.start, size);
	if (size < 0) {
		ret = size;
		goto out_fw;
	}
	if (size > resource_size(&r)) {
		dev_err(dev, "DIAG zap: %zd bytes do not fit %pR\n", size, &r);
		ret = -E2BIG;
		goto out_fw;
	}

	mem = memremap(r.start, size, MEMREMAP_WC);
	if (!mem) {
		ret = -ENOMEM;
		goto out_fw;
	}

	/*
	 * qcom_mdt_load() split in two so each SCM step gets its own stage
	 * record. The pair is exactly qcom_mdt_load(); the .bNN segment names
	 * are derived from fwname.
	 */
	adreno_zap_stage(dev, ZAP_STAGE_PAS_INIT, 0, r.start, size);
	ret = qcom_mdt_pas_init(dev, fw, fwname, ADRENO_ZAP_PAS_ID, r.start,
			NULL);
	adreno_zap_stage(dev, ZAP_STAGE_PAS_INIT_DONE, ret, r.start, size);
	if (!ret) {
		ret = qcom_mdt_load_no_init(dev, fw, fwname,
				mem, r.start, size, NULL);
	}
	adreno_zap_stage(dev, ZAP_STAGE_COPY_DONE, ret, r.start, size);
	if (ret) {
		dev_err(dev, "DIAG zap: mdt load failed: %d\n", ret);
		goto out_mem;
	}

	/*
	 * Authenticate before dropping the CPU mapping. mainline
	 * adreno_gpu.c zap_shader_load_mdt() and OSS kgsl both run
	 * qcom_scm_pas_auth_and_reset() first and memunmap() at their out:
	 * label; the image only has to be in physical memory.
	 */
	adreno_zap_stage(dev, ZAP_STAGE_AUTH, 0, r.start, size);
	ret = qcom_scm_pas_auth_and_reset(ADRENO_ZAP_PAS_ID);
	adreno_zap_stage(dev, ZAP_STAGE_AUTH_DONE, ret, r.start, size);

out_mem:
	memunmap(mem);
out_fw:
	release_firmware(fw);
out_name:
	kfree(fwname);
	return ret ? ERR_PTR(ret) : (void *)dev;
}

#endif /* _KGSL_ZAP_COMPAT_H */
