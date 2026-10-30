/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ACK 6.12 compat shim for the legacy <soc/qcom/scm.h> interface used by
 * the qseecom port (drivers/misc/qseecom.c).
 *
 * The legacy tree provided struct scm_desc + scm_call2() backed by its
 * own smc driver, with results returned via desc->ret[]. On ACK 6.12 the
 * transport is drivers/firmware/qcom/qcom_scm + qcom_tzmem: a call is
 * described by struct qcom_scm_desc and issued with
 * __scm_smc_call_legacy(), which keeps the donor scm.c semantics this
 * firmware expects (carveout spill, resume with the returned x0..x6,
 * negative-x0-only errors, no WAITQ), returns the remapped SCM errno and
 * delivers response words via struct qcom_scm_res.
 *
 * Legacy smc_id encoding (SCM_SIP_FNID/SCM_SMC_FNID) carries the owner in
 * bits [29:24] (0x02000000 == ARM_SMCCC_OWNER_SIP), the service in
 * bits [15:8] and the command in bits [7:0]; the modern SMC layer builds
 * the identical function ID from (owner, svc, cmd), so the translation
 * below is exact. Legacy SCM_ARGS arginfo encoding (count in low bits,
 * 2-bit arg types from bit 4, VAL/RO/RW/BUFVAL order) matches the modern
 * QCOM_SCM_ARGS layout for the argument counts used here, so arginfo is
 * passed through verbatim.
 *
 * struct qcom_scm_desc / struct qcom_scm_res mirror
 * drivers/firmware/qcom/qcom_scm.h (internal to the firmware driver, not
 * installed under include/); kept in sync by inspection.
 */

#ifndef __SOC_QCOM_SCM_H
#define __SOC_QCOM_SCM_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/firmware/qcom/qcom_scm.h>

/* Service IDs kept from the legacy interface */
#define SCM_SVC_BOOT		0x1
#define SCM_SVC_PIL		0x2
#define SCM_SVC_UTIL		0x3
#define SCM_SVC_TZ		0x4
#define SCM_SVC_IO		0x5
#define SCM_SVC_INFO		0x6
#define SCM_SVC_SSD		0x7
#define SCM_SVC_FUSE		0x8
#define SCM_SVC_PWR		0x9
#define SCM_SVC_MP		0xC
#define SCM_SVC_ES		0x10
#define SCM_SVC_HDCP		0x11
#define SCM_SVC_MDTP		0x12
#define SCM_SVC_LMH		0x13
#define SCM_SVC_SMMU_PROGRAM	0x15
#define SCM_SVC_TZSCHEDULER	0xFC

#define SCM_SIP_FNID(s, c) (((((s) & 0xFF) << 8) | ((c) & 0xFF)) | 0x02000000)
#define SCM_SMC_FNID(s, c) ((((s) & 0xFF) << 8) | ((c) & 0xFF))
#define SCM_SVC_ID(s) (((s) & 0xFF00) >> 8)
#define SCM_OWNER_ID(s) (((s) >> 24) & 0x3F)

#define MAX_SCM_ARGS 10
#define MAX_SCM_RETS 3

enum scm_arg_types {
	SCM_VAL,
	SCM_RO,
	SCM_RW,
	SCM_BUFVAL,
};

#define SCM_ARGS_IMPL(num, a, b, c, d, e, f, g, h, i, j, ...) ( \
			(((a) & 0xff) << 4) | \
			(((b) & 0xff) << 6) | \
			(((c) & 0xff) << 8) | \
			(((d) & 0xff) << 10) | \
			(((e) & 0xff) << 12) | \
			(((f) & 0xff) << 14) | \
			(((g) & 0xff) << 16) | \
			(((h) & 0xff) << 18) | \
			(((i) & 0xff) << 20) | \
			(((j) & 0xff) << 22) | \
			((num) & 0xffff))

#define SCM_ARGS(...) SCM_ARGS_IMPL(__VA_ARGS__, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)

/* Mirrors drivers/firmware/qcom/qcom_scm.h */
#define MAX_QCOM_SCM_ARGS 10
#define MAX_QCOM_SCM_RETS 3

struct qcom_scm_desc {
	u32 svc;
	u32 cmd;
	u32 arginfo;
	u64 args[MAX_QCOM_SCM_ARGS];
	u32 owner;
};

struct qcom_scm_res {
	u64 result[MAX_QCOM_SCM_RETS];
};

/*
 * struct qcom_scm_spill_mem - carveout allocator for >4-arg SMC spill
 * Mirrors drivers/firmware/qcom/qcom_scm.h (kept in sync by inspection):
 * legacy TrustZone without shmbridge cannot read the default tzmem spill
 * buffer, so qseecom registers a carveout-backed pair at probe. It backs
 * only __scm_smc_call_legacy() calls.
 */
struct qcom_scm_spill_mem {
	void *(*alloc)(size_t len, dma_addr_t *phys, gfp_t flags);
	void (*free)(void *virt, size_t len, dma_addr_t phys);
};

void qcom_scm_set_spill_mem(const struct qcom_scm_spill_mem *mem);

int __scm_smc_call_legacy(const struct qcom_scm_desc *desc,
			  struct qcom_scm_res *res, bool atomic);

/**
 * struct scm_desc - legacy secure-world call descriptor
 * @arginfo: Metadata describing the arguments in args[]
 * @args: The array of arguments for the secure syscall
 * @ret: The values returned by the secure syscall
 */
struct scm_desc {
	u32 arginfo;
	u64 args[MAX_SCM_ARGS];
	u64 ret[MAX_SCM_RETS];
};

static inline int scm_call2_noretry(u32 smc_id, struct scm_desc *desc)
{
	struct qcom_scm_desc mdesc = {
		.svc = (smc_id >> 8) & 0xFF,
		.cmd = smc_id & 0xFF,
		.arginfo = desc->arginfo,
		.owner = SCM_OWNER_ID(smc_id),
	};
	struct qcom_scm_res res = { { 0 } };
	int i, ret;

	for (i = 0; i < MAX_SCM_ARGS; i++)
		mdesc.args[i] = desc->args[i];

	if (SCM_OWNER_ID(smc_id) == 50 || SCM_OWNER_ID(smc_id) == 48)
		pr_info("snapshot43 qsee_send pid=%d smc=%08x argc=%u arginfo=0x%x args0=0x%llx args1=0x%llx args2=0x%llx args3=0x%llx\n",
			current->pid, smc_id, desc->arginfo & 0xf,
			desc->arginfo, desc->args[0], desc->args[1],
			desc->args[2], desc->args[3]);

	ret = __scm_smc_call_legacy(&mdesc, &res, false);

	desc->ret[0] = res.result[0];
	desc->ret[1] = res.result[1];
	desc->ret[2] = res.result[2];
	/* Propagate the modern errno unchanged. Callers that treat a specific
	 * error as "unsupported" (e.g. the qseecom listener smcinvoke probe)
	 * test for it explicitly at their own site.
	 */
	if (SCM_OWNER_ID(smc_id) == 50 || SCM_OWNER_ID(smc_id) == 48)
		pr_info("snapshot43 qsee_ret  pid=%d smc=%08x -> %d ret0=0x%llx ret1=0x%llx ret2=0x%llx\n",
			current->pid, smc_id, ret, res.result[0],
			res.result[1], res.result[2]);
	return ret;
}

static inline int scm_call2(u32 smc_id, struct scm_desc *desc)
{
	/* The modern SMC layer already retries -EBUSY internally. */
	return scm_call2_noretry(smc_id, desc);
}

static inline int scm_call2_atomic(u32 smc_id, struct scm_desc *desc)
{
	struct qcom_scm_desc mdesc = {
		.svc = (smc_id >> 8) & 0xFF,
		.cmd = smc_id & 0xFF,
		.arginfo = desc->arginfo,
		.owner = SCM_OWNER_ID(smc_id),
	};
	struct qcom_scm_res res = { { 0 } };
	int i, ret;

	for (i = 0; i < MAX_SCM_ARGS; i++)
		mdesc.args[i] = desc->args[i];

	ret = __scm_smc_call_legacy(&mdesc, &res, true);

	desc->ret[0] = res.result[0];
	desc->ret[1] = res.result[1];
	desc->ret[2] = res.result[2];
	/* Same as above: no blanket translation (see scm_call2_noretry). */
	return ret;
}

#endif /* __SOC_QCOM_SCM_H */
