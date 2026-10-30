// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2015,2019 The Linux Foundation. All rights reserved.
 */

#include <linux/cleanup.h>
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/firmware/qcom/qcom_tzmem.h>
#include <linux/arm-smccc.h>
#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <linux/printk.h>

#include "qcom_scm.h"

/**
 * struct arm_smccc_args
 * @args:	The array of values used in registers in smc instruction
 */
struct arm_smccc_args {
	unsigned long args[8];
};

static DEFINE_MUTEX(qcom_scm_lock);

#define QCOM_SCM_EBUSY_WAIT_MS 30
#define QCOM_SCM_EBUSY_MAX_RETRY 20

#define SCM_SMC_N_REG_ARGS	4
#define SCM_SMC_FIRST_EXT_IDX	(SCM_SMC_N_REG_ARGS - 1)
#define SCM_SMC_N_EXT_ARGS	(MAX_QCOM_SCM_ARGS - SCM_SMC_N_REG_ARGS + 1)
#define SCM_SMC_FIRST_REG_IDX	2
#define SCM_SMC_LAST_REG_IDX	(SCM_SMC_FIRST_REG_IDX + SCM_SMC_N_REG_ARGS - 1)

/*
 * Legacy Qualcomm TrustZone (MSM8953-era) has no shmbridge, so it cannot be
 * granted the tzmem spill buffer used for >4-arg SMCs. qseecom registers a
 * carveout-backed allocator at probe. It is used only by legacy calls
 * (__scm_smc_call_legacy(), i.e. the <soc/qcom/scm.h> shim); every other
 * client keeps tzmem. NULL (default) preserves native behavior exactly.
 */
static const struct qcom_scm_spill_mem *spill_mem_override;

void qcom_scm_set_spill_mem(const struct qcom_scm_spill_mem *mem)
{
	spill_mem_override = mem;
}
EXPORT_SYMBOL_GPL(qcom_scm_set_spill_mem);

static void __scm_smc_do_quirk(const struct arm_smccc_args *smc,
			       struct arm_smccc_res *res)
{
	unsigned long a0 = smc->args[0];
	struct arm_smccc_quirk quirk = { .id = ARM_SMCCC_QUIRK_QCOM_A6 };

	quirk.state.a6 = 0;

	do {
		arm_smccc_smc_quirk(a0, smc->args[1], smc->args[2],
				    smc->args[3], smc->args[4], smc->args[5],
				    quirk.state.a6, smc->args[7], res, &quirk);

		if (res->a0 == QCOM_SCM_INTERRUPTED)
			a0 = res->a0;

	} while (res->a0 == QCOM_SCM_INTERRUPTED);
}

#ifdef CONFIG_ARM64
/* One SMC with x0..x7 as inputs and x0..x7 as outputs. */
static void scm_smc_full_regs(const unsigned long *in, unsigned long *out)
{
	register unsigned long x0 asm("x0") = in[0];
	register unsigned long x1 asm("x1") = in[1];
	register unsigned long x2 asm("x2") = in[2];
	register unsigned long x3 asm("x3") = in[3];
	register unsigned long x4 asm("x4") = in[4];
	register unsigned long x5 asm("x5") = in[5];
	register unsigned long x6 asm("x6") = in[6];
	register unsigned long x7 asm("x7") = in[7];

	asm volatile("smc	#0\n"
		: "=r" (x0), "=r" (x1), "=r" (x2), "=r" (x3),
		  "=r" (x4), "=r" (x5), "=r" (x6), "=r" (x7)
		: "r" (x0), "r" (x1), "r" (x2), "r" (x3),
		  "r" (x4), "r" (x5), "r" (x6), "r" (x7)
		: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
		  "x16", "x17", "memory");

	out[0] = x0; out[1] = x1; out[2] = x2; out[3] = x3;
	out[4] = x4; out[5] = x5; out[6] = x6; out[7] = x7;
}

/*
 * Legacy QSEE call loop, matching the donor msm-4.19 __scm_call_armv8_64():
 * x6 starts at 0 and, while the firmware returns INTERRUPTED, the call is
 * re-issued with the *returned* x0..x6 as inputs. The upstream quirk loop
 * instead re-sends the original x1..x5 and only follows x0/x6.
 */
static void __scm_smc_do_legacy(const struct arm_smccc_args *smc,
				struct arm_smccc_res *res)
{
	unsigned long in[8], out[8];
	int i;

	for (i = 0; i < 8; i++)
		in[i] = smc->args[i];
	in[6] = 0;

	for (;;) {
		scm_smc_full_regs(in, out);

		if (out[0] != QCOM_SCM_INTERRUPTED)
			break;

		for (i = 0; i < 7; i++)
			in[i] = out[i];
	}

	res->a0 = out[0];
	res->a1 = out[1];
	res->a2 = out[2];
	res->a3 = out[3];
}
#endif

/* One complete firmware call, resuming INTERRUPTED returns internally. */
static void __scm_smc_do_resumable(const struct arm_smccc_args *smc,
				   struct arm_smccc_res *res, bool legacy)
{
#ifdef CONFIG_ARM64
	if (legacy) {
		__scm_smc_do_legacy(smc, res);
		return;
	}
#endif
	__scm_smc_do_quirk(smc, res);
}

static void fill_wq_resume_args(struct arm_smccc_args *resume, u32 smc_call_ctx)
{
	memset(resume->args, 0, sizeof(resume->args[0]) * ARRAY_SIZE(resume->args));

	resume->args[0] = ARM_SMCCC_CALL_VAL(ARM_SMCCC_STD_CALL,
					ARM_SMCCC_SMC_64, ARM_SMCCC_OWNER_SIP,
					SCM_SMC_FNID(QCOM_SCM_SVC_WAITQ, QCOM_SCM_WAITQ_RESUME));

	resume->args[1] = QCOM_SCM_ARGS(1);

	resume->args[2] = smc_call_ctx;
}

int scm_get_wq_ctx(u32 *wq_ctx, u32 *flags, u32 *more_pending)
{
	int ret;
	struct arm_smccc_res get_wq_res;
	struct arm_smccc_args get_wq_ctx = {0};

	get_wq_ctx.args[0] = ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,
				ARM_SMCCC_SMC_64, ARM_SMCCC_OWNER_SIP,
				SCM_SMC_FNID(QCOM_SCM_SVC_WAITQ, QCOM_SCM_WAITQ_GET_WQ_CTX));

	/* Guaranteed to return only success or error, no WAITQ_* */
	__scm_smc_do_quirk(&get_wq_ctx, &get_wq_res);
	ret = get_wq_res.a0;
	if (ret)
		return ret;

	*wq_ctx = get_wq_res.a1;
	*flags  = get_wq_res.a2;
	*more_pending = get_wq_res.a3;

	return 0;
}

static int __scm_smc_do_quirk_handle_waitq(struct device *dev, struct arm_smccc_args *waitq,
					   struct arm_smccc_res *res)
{
	int ret;
	u32 wq_ctx, smc_call_ctx;
	struct arm_smccc_args resume;
	struct arm_smccc_args *smc = waitq;

	do {
		__scm_smc_do_quirk(smc, res);

		if (res->a0 == QCOM_SCM_WAITQ_SLEEP) {
			wq_ctx = res->a1;
			smc_call_ctx = res->a2;

			ret = qcom_scm_wait_for_wq_completion(wq_ctx);
			if (ret)
				return ret;

			fill_wq_resume_args(&resume, smc_call_ctx);
			smc = &resume;
		}
	} while (res->a0 == QCOM_SCM_WAITQ_SLEEP);

	return 0;
}

/*
 * Legacy calls skip the WAITQ protocol: MSM8953-era QSEE predates it, and
 * the donor treats every non-negative x0 other than INTERRUPTED as a
 * finished call. Waiting on the WAITQ completion here would block forever
 * with qcom_scm_lock held, because this firmware never signals it.
 */
static int __scm_smc_do(struct device *dev, struct arm_smccc_args *smc,
			struct arm_smccc_res *res, bool atomic, bool legacy)
{
	int ret, retry_count = 0;

	if (atomic) {
		__scm_smc_do_resumable(smc, res, legacy);
		return 0;
	}

	do {
		mutex_lock(&qcom_scm_lock);

		if (legacy) {
			__scm_smc_do_resumable(smc, res, true);
			ret = 0;
		} else {
			ret = __scm_smc_do_quirk_handle_waitq(dev, smc, res);
		}

		mutex_unlock(&qcom_scm_lock);

		if (ret)
			return ret;

		if (res->a0 == QCOM_SCM_V2_EBUSY) {
			if (retry_count++ > QCOM_SCM_EBUSY_MAX_RETRY)
				break;
			msleep(QCOM_SCM_EBUSY_WAIT_MS);
		}
	}  while (res->a0 == QCOM_SCM_V2_EBUSY);

	return 0;
}

static int __scm_smc_call_common(struct device *dev,
				 const struct qcom_scm_desc *desc,
				 enum qcom_scm_convention qcom_convention,
				 struct qcom_scm_res *res, bool atomic,
				 bool legacy)
{
	int arglen = desc->arginfo & 0xf;
	int i, ret;
	void *args_virt __free(qcom_tzmem) = NULL;
	void *ext_virt = NULL;
	dma_addr_t ext_phys = 0;
	gfp_t flag = atomic ? GFP_ATOMIC : GFP_KERNEL;
	u32 smccc_call_type = atomic ? ARM_SMCCC_FAST_CALL : ARM_SMCCC_STD_CALL;
	u32 qcom_smccc_convention = (qcom_convention == SMC_CONVENTION_ARM_32) ?
				    ARM_SMCCC_SMC_32 : ARM_SMCCC_SMC_64;
	struct arm_smccc_res smc_res;
	struct arm_smccc_args smc = {0};

	smc.args[0] = ARM_SMCCC_CALL_VAL(
		smccc_call_type,
		qcom_smccc_convention,
		desc->owner,
		SCM_SMC_FNID(desc->svc, desc->cmd));
	smc.args[1] = desc->arginfo;
	for (i = 0; i < SCM_SMC_N_REG_ARGS; i++)
		smc.args[i + SCM_SMC_FIRST_REG_IDX] = desc->args[i];

	if (unlikely(arglen > SCM_SMC_N_REG_ARGS)) {
		if (legacy && spill_mem_override) {
			ext_virt = spill_mem_override->alloc(
					SCM_SMC_N_EXT_ARGS * sizeof(u64),
					&ext_phys, flag);
			if (!ext_virt)
				return -ENOMEM;

			if (qcom_smccc_convention == ARM_SMCCC_SMC_32) {
				__le32 *args = ext_virt;

				for (i = 0; i < SCM_SMC_N_EXT_ARGS; i++)
					args[i] = cpu_to_le32(desc->args[i +
						      SCM_SMC_FIRST_EXT_IDX]);
			} else {
				__le64 *args = ext_virt;

				for (i = 0; i < SCM_SMC_N_EXT_ARGS; i++)
					args[i] = cpu_to_le64(desc->args[i +
						      SCM_SMC_FIRST_EXT_IDX]);
			}

			smc.args[SCM_SMC_LAST_REG_IDX] = ext_phys;
			pr_debug("qcom_scm: spilled %d args to carveout %pad (svc %x cmd %x)\n",
				arglen, &ext_phys, desc->svc, desc->cmd);
		} else {
			struct qcom_tzmem_pool *mempool =
				qcom_scm_get_tzmem_pool();

			if (!mempool)
				return -EINVAL;

			args_virt = qcom_tzmem_alloc(mempool,
						     SCM_SMC_N_EXT_ARGS * sizeof(u64),
						     flag);
			if (!args_virt)
				return -ENOMEM;

			if (qcom_smccc_convention == ARM_SMCCC_SMC_32) {
				__le32 *args = args_virt;

				for (i = 0; i < SCM_SMC_N_EXT_ARGS; i++)
					args[i] = cpu_to_le32(desc->args[i +
							      SCM_SMC_FIRST_EXT_IDX]);
			} else {
				__le64 *args = args_virt;

				for (i = 0; i < SCM_SMC_N_EXT_ARGS; i++)
					args[i] = cpu_to_le64(desc->args[i +
							      SCM_SMC_FIRST_EXT_IDX]);
			}

			smc.args[SCM_SMC_LAST_REG_IDX] = qcom_tzmem_to_phys(args_virt);
		}
	}

	ret = __scm_smc_do(dev, &smc, &smc_res, atomic, legacy);
	if (ext_virt)
		spill_mem_override->free(ext_virt,
				SCM_SMC_N_EXT_ARGS * sizeof(u64), ext_phys);
	if (ret)
		return ret;

	if (res) {
		res->result[0] = smc_res.a1;
		res->result[1] = smc_res.a2;
		res->result[2] = smc_res.a3;
	}

	/* Donor semantics: only a negative x0 is a firmware error. */
	if (legacy)
		return (int)smc_res.a0 < 0 ?
			qcom_scm_remap_error((int)smc_res.a0) : 0;

	return (long)smc_res.a0 ? qcom_scm_remap_error(smc_res.a0) : 0;
}

int __scm_smc_call(struct device *dev, const struct qcom_scm_desc *desc,
		   enum qcom_scm_convention qcom_convention,
		   struct qcom_scm_res *res, bool atomic)
{
	return __scm_smc_call_common(dev, desc, qcom_convention, res, atomic,
				     false);
}

/*
 * Entry for the legacy <soc/qcom/scm.h> shim (qseecom): carveout spill,
 * donor-style INTERRUPTED resume and donor errno semantics, no WAITQ.
 */
int __scm_smc_call_legacy(const struct qcom_scm_desc *desc,
			  struct qcom_scm_res *res, bool atomic)
{
	return __scm_smc_call_common(NULL, desc, qcom_scm_convention, res,
				     atomic, true);
}
EXPORT_SYMBOL_GPL(__scm_smc_call_legacy);
