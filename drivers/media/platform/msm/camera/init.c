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
#include <linux/workqueue.h>
#include "msm_sd.h"

bool camera_legacy_enable = true;

static struct workqueue_struct *camera_late_wq;

/*
 * Run a late driver registration off the boot path. The EEPROM probes read
 * several kilobytes of calibration data per module over CCI and hold up
 * kernel_init() for seconds; nothing reads that data until the camera HAL
 * starts. The queue is ordered, so registrations run in initcall order and
 * the media entities keep the IDs the HAL enumerates.
 */
void camera_queue_late_init(struct work_struct *work)
{
	if (camera_late_wq)
		queue_work(camera_late_wq, work);
	else
		work->func(work);
}

static int __init camera_legacy_init(void)
{
	pr_info("Enable Legacy Camera Stack.\n");

	camera_late_wq = alloc_ordered_workqueue("msm_cam_late", 0);
	if (!camera_late_wq)
		pr_warn("no late init queue; registering inline\n");

	return 0;
}

module_init(camera_legacy_init);
