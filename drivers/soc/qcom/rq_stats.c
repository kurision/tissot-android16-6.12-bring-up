// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2010-2015, 2017, 2019-2020, The Linux Foundation. All rights reserved.
 *
 * Port of the donor's drivers/soc/qcom/rq_stats.c: the deferred timer
 * the ROM's msm_irqbalance sleeps on.
 *
 * The daemon writes an interval in milliseconds to
 * /sys/devices/system/cpu/cpu0/rq-stats/def_timer_ms, polls the file, and
 * is woken by a sysfs_notify() once the interval has passed, but only at
 * a wakeup that is happening anyway, so its sleep never wakes an idle
 * CPU. Without the file it logs "deferred_sleep: Unable to open ..." and
 * exits, and init restarts it every five seconds.
 *
 * The donor got its wakeups from register_tick_sched_wakeup_callback(), a
 * CAF hook in tick-sched that this kernel does not have. A deferrable
 * timer has the same property (it never wakes an idle CPU and runs at the
 * next wakeup), so the interval is kept by one here, armed from the first
 * write and re-armed each time it fires.
 */

#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/sysfs.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

static struct kobject *rq_kobj;
static struct timer_list def_timer;
static struct work_struct def_timer_work;
static unsigned long def_timer_jiffies;
static u64 def_start_time;

static void def_work_fn(struct work_struct *work)
{
	/* Notify polling threads on change of value */
	sysfs_notify(rq_kobj, NULL, "def_timer_ms");
}

static void def_timer_fn(struct timer_list *t)
{
	unsigned long interval = READ_ONCE(def_timer_jiffies);

	schedule_work(&def_timer_work);
	if (interval)
		mod_timer(&def_timer, jiffies + interval);
}

static ssize_t def_timer_ms_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	u64 diff = ktime_get_ns() - READ_ONCE(def_start_time);

	return sysfs_emit(buf, "%u\n", (unsigned int)div_u64(diff, NSEC_PER_MSEC));
}

static ssize_t def_timer_ms_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	unsigned int val;
	unsigned long interval;

	if (kstrtouint(buf, 0, &val))
		return -EINVAL;

	interval = msecs_to_jiffies(val);
	WRITE_ONCE(def_timer_jiffies, interval);
	WRITE_ONCE(def_start_time, ktime_get_ns());

	if (interval)
		mod_timer(&def_timer, jiffies + interval);
	else
		timer_delete(&def_timer);

	return count;
}

static struct kobj_attribute def_timer_ms_attr = __ATTR_RW_MODE(def_timer_ms, 0600);

static struct attribute *rq_attrs[] = {
	&def_timer_ms_attr.attr,
	NULL,
};

static const struct attribute_group rq_attr_group = {
	.attrs = rq_attrs,
};

static int __init msm_rq_stats_init(void)
{
	struct device *cpu0 = get_cpu_device(0);
	int err;

	if (!cpu0)
		return -ENODEV;

	INIT_WORK(&def_timer_work, def_work_fn);
	timer_setup(&def_timer, def_timer_fn, TIMER_DEFERRABLE);

	/* Create /sys/devices/system/cpu/cpu0/rq-stats/... */
	rq_kobj = kobject_create_and_add("rq-stats", &cpu0->kobj);
	if (!rq_kobj)
		return -ENOMEM;

	err = sysfs_create_group(rq_kobj, &rq_attr_group);
	if (err) {
		kobject_put(rq_kobj);
		rq_kobj = NULL;
		return err;
	}

	kobject_uevent(rq_kobj, KOBJ_ADD);
	return 0;
}
late_initcall(msm_rq_stats_init);
