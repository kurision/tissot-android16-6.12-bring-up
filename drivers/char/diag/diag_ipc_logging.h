/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2015, 2017-2019 The Linux Foundation. All rights reserved.
 */

#ifndef DIAGIPCLOG_H
#define DIAGIPCLOG_H

/*
 * This tree does not carry the CAF ipc_logging framework; the DIAG_LOG
 * macro below compiles out without it. The donor included the vendor
 * header unconditionally.
 */
#ifdef CONFIG_IPC_LOGGING
#include <linux/ipc_logging.h>
#endif

#define DIAG_IPC_LOG_PAGES	50

#define DIAG_DEBUG_USERSPACE	0x0001
#define DIAG_DEBUG_MUX		0x0002
#define DIAG_DEBUG_DCI		0x0004
#define DIAG_DEBUG_PERIPHERALS	0x0008
#define DIAG_DEBUG_MASKS	0x0010
#define DIAG_DEBUG_POWER	0x0020
#define DIAG_DEBUG_BRIDGE	0x0040
#define DIAG_DEBUG_CMD_INFO	0x0080
#define DIAG_DEBUG_MHI		0x0100

#ifdef CONFIG_IPC_LOGGING
extern uint16_t diag_debug_mask;
extern void *diag_ipc_log;

#define DIAG_LOG(log_lvl, msg, ...)					\
	do {								\
		if (diag_ipc_log && (log_lvl & diag_debug_mask)) {	\
			ipc_log_string(diag_ipc_log,			\
				"[%s] " msg, __func__, ##__VA_ARGS__);	\
		}							\
	} while (0)
#else
/*
 * No ipc_logging in this tree. The donor discarded these lines entirely,
 * but several of its local variables are only read inside DIAG_LOG; an
 * empty macro turns them into -Wunused-but-set-variable errors under
 * CONFIG_WERROR. Route the lines to dynamic debug instead so the debug
 * output stays available and gated like the donor's mask.
 */
#define DIAG_LOG(log_lvl, msg, ...) \
	pr_debug_ratelimited("diag: " msg, ##__VA_ARGS__)
#endif

#endif
