/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The gpio_request_array()/gpio_free_array() helpers and the struct gpio
 * they take were removed from include/linux/gpio.h, but the numeric GPIO
 * API this driver tree is written against is still there. Keep the donor
 * interface: request each entry in order and unwind on the first failure,
 * exactly as the removed helpers did.
 */
#ifndef _MSM_CAMERA_LEGACY_GPIO_ARRAY_H
#define _MSM_CAMERA_LEGACY_GPIO_ARRAY_H

#include <linux/gpio.h>
#include <linux/types.h>

struct gpio {
	unsigned	gpio;
	unsigned long	flags;
	const char	*label;
};

static inline int gpio_request_array(const struct gpio *array, size_t n)
{
	int i, rc;

	for (i = 0; i < n; i++) {
		rc = gpio_request_one(array[i].gpio, array[i].flags,
				      array[i].label);
		if (rc)
			goto err_free;
	}
	return 0;

err_free:
	while (--i >= 0)
		gpio_free(array[i].gpio);
	return rc;
}

static inline void gpio_free_array(const struct gpio *array, size_t n)
{
	while (n--)
		gpio_free(array[n].gpio);
}

#endif
