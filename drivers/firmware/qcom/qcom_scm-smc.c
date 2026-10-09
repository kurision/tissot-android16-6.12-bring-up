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
#include <linux/ratelimit.h>
#include <linux/sched.h>

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
 * carveout-backed allocator at probe; all other clients keep tzmem.
 * NULL (default) preserves native behavior exactly.
 */
static const struct qcom_scm_spill_mem *spill_mem_override;

void qcom_scm_set_spill_mem(const struct qcom_scm_spill_mem *mem)
{
	spill_mem_override = mem;
}
EXPORT_SYMBOL_GPL(qcom_scm_set_spill_mem);

/* Bound the snapshot43 post-SMC register capture. Three attempts is the
 * first return plus the first two resume attempts; enough to compare the
 * returned register set against what we feed back, without spamming.
 */
#define SCM_SMC_INT_CAPTURE_ATTEMPTS	3

/*
 * snapshot43: capture the complete SMC result register set (x0..x7).
 *
 * arm_smccc_smc() retains only x0..x3 (plus x6 for the QCOM quirk; see
 * arch/arm64/kernel/smccc-call.S), so what the firmware returns in x4/x5/x7
 * on an interrupted call is otherwise invisible to this driver. The donor
 * kernel (drivers/soc/qcom/scm.c __scm_call_armv8_64) declares x0..x6 as
 * in-out operands of its SMC asm, i.e. it feeds the *returned* x0..x6 back
 * into the resume call; this driver keeps the original x1..x5 and only
 * follows a0 and the quirk's a6. x4/x5 are therefore where the two resume
 * semantics can diverge, and they have never been observed.
 *
 * Inputs here are identical to what the resume call in the loop would have
 * issued (a0, x1..x5 unchanged, x6 = quirk a6, x7 unchanged), so this only
 * widens what we can see - the resume inputs themselves are not altered.
 */
#ifdef CONFIG_ARM64
static void scm_capture_result_regs(const unsigned long *in, unsigned long *out)
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
#endif

static void __scm_smc_do_quirk(const struct arm_smccc_args *smc,
			       struct arm_smccc_res *res, bool trace_send)
{
	unsigned long a0 = smc->args[0];
	unsigned int attempts = 0;
	struct arm_smccc_quirk quirk = { .id = ARM_SMCCC_QUIRK_QCOM_A6 };
	static DEFINE_RATELIMIT_STATE(scm_pre_rs, HZ, 8);

	quirk.state.a6 = 0;

	do {
		if (trace_send && __ratelimit(&scm_pre_rs))
			pr_info("snapshot35 scm_pre pid=%d a0=0x%lx a1=0x%lx a2=0x%lx a3=0x%lx a4=0x%lx a5=0x%lx a6=0x%lx a7=0x%lx\n",
				current->pid, a0, smc->args[1], smc->args[2],
				smc->args[3], smc->args[4], smc->args[5],
				quirk.state.a6, smc->args[7]);
		attempts++;
		if (trace_send && attempts == 1)
			pr_info("snapshot43 scm_raw_enter pid=%d fn=0x%lx\n",
				current->pid, smc->args[0]);

#ifdef CONFIG_ARM64
		if (trace_send && attempts <= SCM_SMC_INT_CAPTURE_ATTEMPTS) {
			unsigned long in[8], out[8];

			in[0] = a0;
			in[1] = smc->args[1];
			in[2] = smc->args[2];
			in[3] = smc->args[3];
			in[4] = smc->args[4];
			in[5] = smc->args[5];
			in[6] = quirk.state.a6;
			in[7] = smc->args[7];
			scm_capture_result_regs(in, out);

			pr_info("snapshot43 scm_regs pid=%d fn=0x%lx attempt=%u x0=0x%lx x1=0x%lx x2=0x%lx x3=0x%lx x4=0x%lx x5=0x%lx x6=0x%lx x7=0x%lx\n",
				current->pid, smc->args[0], attempts,
				out[0], out[1], out[2], out[3],
				out[4], out[5], out[6], out[7]);

			res->a0 = out[0];
			res->a1 = out[1];
			res->a2 = out[2];
			res->a3 = out[3];
			/* Keep the quirk state exactly where the standard
			 * smccc-call.S quirk path would have left it.
			 */
			quirk.state.a6 = out[6];
		} else
#endif
		{
			arm_smccc_smc_quirk(a0, smc->args[1], smc->args[2],
					    smc->args[3], smc->args[4], smc->args[5],
					    quirk.state.a6, smc->args[7], res, &quirk);
		}

		if (trace_send && attempts == 1)
			pr_info("snapshot43 scm_raw_first_ret pid=%d fn=0x%lx a0=0x%lx a1=0x%lx a2=0x%lx a3=0x%lx\n",
				current->pid, smc->args[0], res->a0,
				res->a1, res->a2, res->a3);

		if (res->a0 == QCOM_SCM_INTERRUPTED)
			a0 = res->a0;

	} while (res->a0 == QCOM_SCM_INTERRUPTED);
	if (trace_send)
		pr_info("snapshot43 scm_raw_done pid=%d fn=0x%lx attempts=%u a0=0x%lx\n",
			current->pid, smc->args[0], attempts, res->a0);
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
	__scm_smc_do_quirk(&get_wq_ctx, &get_wq_res, false);
	ret = get_wq_res.a0;
	if (ret)
		return ret;

	*wq_ctx = get_wq_res.a1;
	*flags  = get_wq_res.a2;
	*more_pending = get_wq_res.a3;

	return 0;
}

static int __scm_smc_do_quirk_handle_waitq(struct device *dev, struct arm_smccc_args *waitq,
					   struct arm_smccc_res *res, bool trace_send)
{
	int ret;
	u32 wq_ctx, smc_call_ctx;
	struct arm_smccc_args resume;
	struct arm_smccc_args *smc = waitq;
	static DEFINE_RATELIMIT_STATE(scm_waitq_rs, HZ, 2);
	bool log_wait;

	do {
		__scm_smc_do_quirk(smc, res, trace_send);

		if (res->a0 == QCOM_SCM_WAITQ_SLEEP) {
			wq_ctx = res->a1;
			smc_call_ctx = res->a2;
			log_wait = trace_send && __ratelimit(&scm_waitq_rs);
			if (log_wait)
				pr_info("snapshot35 scm_waitq sleep pid=%d wq_ctx=%u call_ctx=%u\n",
					current->pid, wq_ctx, smc_call_ctx);

			ret = qcom_scm_wait_for_wq_completion(wq_ctx);
			if (log_wait)
				pr_info("snapshot35 scm_waitq resume pid=%d wq_ctx=%u call_ctx=%u ret=%d\n",
					current->pid, wq_ctx, smc_call_ctx, ret);
			if (ret)
				return ret;

			fill_wq_resume_args(&resume, smc_call_ctx);
			smc = &resume;
		}
	} while (res->a0 == QCOM_SCM_WAITQ_SLEEP);

	return 0;
}

static int __scm_smc_do(struct device *dev, struct arm_smccc_args *smc,
			struct arm_smccc_res *res, bool atomic, bool trace_send)
{
	int ret, retry_count = 0;

	if (atomic) {
		__scm_smc_do_quirk(smc, res, trace_send);
		return 0;
	}

	do {
		if (trace_send && !retry_count)
			pr_info("snapshot43 scm_lock_wait pid=%d fn=0x%lx\n",
				current->pid, smc->args[0]);
		mutex_lock(&qcom_scm_lock);
		if (trace_send && !retry_count)
			pr_info("snapshot43 scm_lock_acquired pid=%d fn=0x%lx\n",
				current->pid, smc->args[0]);

		ret = __scm_smc_do_quirk_handle_waitq(dev, smc, res, trace_send);

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


int __scm_smc_call(struct device *dev, const struct qcom_scm_desc *desc,
		   enum qcom_scm_convention qcom_convention,
		   struct qcom_scm_res *res, bool atomic)
{
	struct qcom_tzmem_pool *mempool = qcom_scm_get_tzmem_pool();
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
	/* Trace app transactions only; owner-50 listener calls have shim markers. */
	bool trace_send = desc->owner == 48 && desc->svc == 0;

	smc.args[0] = ARM_SMCCC_CALL_VAL(
		smccc_call_type,
		qcom_smccc_convention,
		desc->owner,
		SCM_SMC_FNID(desc->svc, desc->cmd));
	smc.args[1] = desc->arginfo;
	for (i = 0; i < SCM_SMC_N_REG_ARGS; i++)
		smc.args[i + SCM_SMC_FIRST_REG_IDX] = desc->args[i];

	if (unlikely(arglen > SCM_SMC_N_REG_ARGS)) {
		if (spill_mem_override) {
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
			pr_info("qcom_scm: spilled %d args to carveout %pad (svc %x cmd %x)\n",
				arglen, &ext_phys, desc->svc, desc->cmd);
		} else {
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

	if (trace_send)
		pr_info("snapshot35 scm_send_desc pid=%d owner=%u svc=%u cmd=%u arginfo=0x%x args0=0x%llx args1=0x%llx args2=0x%llx args3=0x%llx args4=0x%llx args5=0x%llx args6=0x%llx ext_pa=0x%lx ext_alloc_len=%zu ext_width=%u atomic=%u\n",
			current->pid, desc->owner, desc->svc, desc->cmd,
			desc->arginfo, (unsigned long long)desc->args[0],
			(unsigned long long)desc->args[1],
			(unsigned long long)desc->args[2],
			(unsigned long long)desc->args[3],
			(unsigned long long)desc->args[4],
			(unsigned long long)desc->args[5],
			(unsigned long long)desc->args[6],
			arglen > SCM_SMC_N_REG_ARGS ? smc.args[5] : 0UL,
			arglen > SCM_SMC_N_REG_ARGS ?
				SCM_SMC_N_EXT_ARGS * sizeof(u64) : (size_t)0,
			qcom_smccc_convention == ARM_SMCCC_SMC_32 ? 32 : 64,
			atomic);
	ret = __scm_smc_do(dev, &smc, &smc_res, atomic, trace_send);
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

	return (long)smc_res.a0 ? qcom_scm_remap_error(smc_res.a0) : 0;

}
