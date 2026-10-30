/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Userspace voltage limit control for CPR3/CPR4 regulators.
 */

#ifndef __REGULATOR_CPR3_UV_H__
#define __REGULATOR_CPR3_UV_H__

#include <linux/errno.h>

struct regulator;

#ifdef CONFIG_REGULATOR_CPR3

int cpr3_regulator_get_corner_limits(struct regulator *regulator, int corner,
				     int *floor_volt, int *ceiling_volt);
int cpr3_regulator_set_corner_ceiling(struct regulator *regulator, int corner,
				      int ceiling_volt);

#else

static inline int cpr3_regulator_get_corner_limits(struct regulator *regulator,
			int corner, int *floor_volt, int *ceiling_volt)
{
	return -ENODEV;
}

static inline int cpr3_regulator_set_corner_ceiling(
			struct regulator *regulator, int corner,
			int ceiling_volt)
{
	return -ENODEV;
}

#endif

#endif /* __REGULATOR_CPR3_UV_H__ */
