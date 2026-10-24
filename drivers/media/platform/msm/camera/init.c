// SPDX-License-Identifier: GPL-2.0-only
/*
 * The donor's camera tree carried two stacks for the same silicon ("legacy"
 * and "legacy-n") and this flag chose between them per board: the vendor
 * tree's techpack/camera-legacy/init.c set it for the Xiaomi phones of that
 * platform, tissot among them.
 *
 * This tree has only the legacy stack, so the flag is always set.  The
 * drivers that consult it (the VFE, the face detection and the SMMU) use it
 * to stay out of the way on the boards the donor did not use the legacy
 * stack on.
 */
#include <linux/module.h>
#include <linux/printk.h>
#include "msm_sd.h"

bool camera_legacy_enable = true;

static int __init camera_legacy_init(void)
{
	pr_info("Enable Legacy Camera Stack.\n");

	return 0;
}

module_init(camera_legacy_init);
