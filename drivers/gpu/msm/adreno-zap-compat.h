/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026, The tissot kgsl-port authors.
 *
 * 6.12 deleted the 4.19 subsystem_get() "look up a remoteproc by name"
 * registry. The only callers are the a5xx/a6xx zap-shader load paths,
 * which this SoC never reaches (a506 runs with device->mmu.secured
 * false, so the zap block is skipped; a6xx hardware is absent). The
 * stand-in fails exactly like "no zap subsystem named", which is what
 * the callers' error path already implements. A real zap load on newer
 * hardware would go through the zap-shader DT child node + signed
 * firmware, as drivers/gpu/drm/msm/adreno/adreno_gpu.c does.
 */

#ifndef _KGSL_ZAP_COMPAT_H
#define _KGSL_ZAP_COMPAT_H

#include <linux/err.h>

static inline void *adreno_zap_subsystem_get(const char *name)
{
	return ERR_PTR(-ENXIO);
}

#endif /* _KGSL_ZAP_COMPAT_H */
