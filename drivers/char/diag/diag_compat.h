/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Compat glue between the CAF 4.19 diag driver and this 6.12 tree.
 *
 * The diag driver calls four CAF 4.19 interfaces this tree does not have:
 *
 * - soc/qcom/socinfo.h: socinfo_get_id() and socinfo_get_msm_cpu().
 *   socinfo_get_id() is replaced by the qcom_smem_get_soc_id() helper of
 *   this tree's SMEM driver, which reads the same SMEM_HW_SW_BUILD_ID
 *   item the CAF socinfo driver did (drivers/soc/qcom/socinfo.c in
 *   ../oc-test).  socinfo_get_msm_cpu() cannot be reconstructed; it
 *   returns MSM_CPU_UNKNOWN.  That matches the donor's behaviour on
 *   msm8953: its switch in chk_config_get_id() has no MSM_CPU_8953 case,
 *   so the real donor also fell through to the default branch.
 *
 * - soc/qcom/restart.h: msm_set_restart_mode(RESTART_DLOAD) set the
 *   restart reason of the CAF PS_HOLD restart driver so the next reboot
 *   entered download mode.  This tree has no such restart driver; the
 *   modern qcom_scm_set_download_mode() is not exported.  The DLOAD
 *   request is logged and dropped: diag transport is unaffected, only
 *   the field-engineering "restart into download mode" command loses
 *   its effect.
 *
 * - soc/qcom/subsystem_notif.h and soc/qcom/subsystem_restart.h:
 *   subsys_notif_register_notifier() is routed to the existing
 *   msm_subsys_notif_register_notifier() compat of this tree, which is
 *   a placeholder handle and never notifies — the same semantics as the
 *   donor's own static-inline stub used when the subsystem framework is
 *   compiled out (../oc-test/include/soc/qcom/subsystem_notif.h:70).
 */
#ifndef _DRIVERS_CHAR_DIAG_DIAG_COMPAT_H
#define _DRIVERS_CHAR_DIAG_DIAG_COMPAT_H

#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/soc/qcom/smem.h>
#include <soc/qcom/msm_subsystem_compat.h>

/* Donor soc/qcom/socinfo.h */
enum msm_cpu {
	MSM_CPU_UNKNOWN = 0,
	MSM_CPU_8960,
	MSM_CPU_8960AB,
	MSM_CPU_8064,
	MSM_CPU_8974,
	MSM_CPU_8974PRO_AA,
	MSM_CPU_8974PRO_AB,
	MSM_CPU_8974PRO_AC,
	MSM_CPU_8916,
	MSM_CPU_8084,
	MSM_CPU_8996,
	MSM_CPU_8953,
};

static inline uint32_t socinfo_get_id(void)
{
	u32 id = 0;

	qcom_smem_get_soc_id(&id);
	return id;
}

static inline enum msm_cpu socinfo_get_msm_cpu(void)
{
	return MSM_CPU_UNKNOWN;
}

#define machine_is_msm8974()	of_machine_is_compatible("qcom,msm8974")

/* Donor soc/qcom/restart.h */
#define RESTART_NORMAL		0x0
#define RESTART_DLOAD		0x1

static inline void msm_set_restart_mode(int mode)
{
	if (mode == RESTART_DLOAD)
		pr_warn_ratelimited("diag: DLOAD restart requested but download mode is unavailable\n");
}

/* Donor soc/qcom/subsystem_notif.h */
enum subsys_notif_type {
	SUBSYS_BEFORE_SHUTDOWN,
	SUBSYS_AFTER_SHUTDOWN,
	SUBSYS_BEFORE_POWERUP,
	SUBSYS_AFTER_POWERUP,
	SUBSYS_BEFORE_AUTH_AND_RESET,
	SUBSYS_RAMDUMP_NOTIFICATION,
	SUBSYS_POWERUP_FAILURE,
	SUBSYS_PROXY_VOTE,
	SUBSYS_PROXY_UNVOTE,
	SUBSYS_SOC_RESET,
	SUBSYS_NOTIF_TYPE_COUNT
};

static inline void *subsys_notif_register_notifier(const char *subsys_name,
						   struct notifier_block *nb)
{
	return msm_subsys_notif_register_notifier(subsys_name, nb);
}

static inline int subsys_notif_unregister_notifier(void *subsys_handle,
						   struct notifier_block *nb)
{
	msm_subsys_notif_unregister_notifier(subsys_handle, nb);
	return 0;
}

/*
 * Donor include/linux/coresight-stm.h: stm_log_inv_ts() forwarded to
 * stm_trace(), whose inline stub returned 0 with the STM framework
 * compiled out — which is how the donor msm8953-perf_defconfig built
 * (CONFIG_CORESIGHT disabled). This tree ships the mainline uapi-only
 * header with no stm_trace(), so provide the same no-op. The OST entity
 * id lived in the donor uapi header; mainline's has only the flag and
 * option bits.
 */
#define OST_ENTITY_DIAG			0xEE

static inline int stm_log_inv_ts(u8 entity_id, u8 proto_id,
				 const void *data, u32 size)
{
	return 0;
}

#endif /* _DRIVERS_CHAR_DIAG_DIAG_COMPAT_H */
