// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
 *
 * Port of the donor's techpack/audio-legacy/asoc/codecs/msm-cdc-pinctrl.c.
 *
 * Each "qcom,msm-cdc-pinctrl" node holds the "aud_active" and "aud_sleep"
 * pinctrl states of one group of audio pins, and the machine driver and
 * the headset helper switch them by node when a DAI starts and stops.
 * Without this driver the header's stubs return success and nothing is
 * switched: the MI2S lines to the speaker amplifier stay unmuxed.
 *
 * Left out from the donor, because no tissot node or caller uses them:
 * the MPM and chip wakeup registers (msm_gpio_mpm_wake_set() is a CAF
 * pinctrl export this tree does not have, and
 * msm_cdc_pinctrl_set_wakeup_capable() has no caller), and the LPI GPIO
 * case. The reset line is a GPIO descriptor rather than a legacy GPIO
 * number. Unlike the donor, the device reference that
 * of_find_device_by_node() takes is dropped.
 */

#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <asoc/msm-cdc-pinctrl.h>

struct msm_cdc_pinctrl_info {
	struct pinctrl *pinctrl;
	struct pinctrl_state *pinctrl_active;
	struct pinctrl_state *pinctrl_sleep;
	struct pinctrl_state *pinctrl_alt_active;
	struct gpio_desc *rst_gpio;
	bool state;
};

static struct msm_cdc_pinctrl_info *msm_cdc_pinctrl_get_gpiodata(
						struct device_node *np)
{
	struct platform_device *pdev;
	struct msm_cdc_pinctrl_info *gpio_data;

	if (!np) {
		pr_err("%s: device node is null\n", __func__);
		return NULL;
	}

	pdev = of_find_device_by_node(np);
	if (!pdev) {
		pr_err("%s: platform device not found!\n", __func__);
		return NULL;
	}

	gpio_data = dev_get_drvdata(&pdev->dev);
	if (!gpio_data)
		dev_err(&pdev->dev, "%s: cannot find cdc gpio info\n",
			__func__);
	put_device(&pdev->dev);

	return gpio_data;
}

/*
 * msm_cdc_get_gpio_state: get the value of the reset GPIO
 * @np: pointer to struct device_node
 *
 * Returns error code for failure and GPIO value on success
 */
int msm_cdc_get_gpio_state(struct device_node *np)
{
	struct msm_cdc_pinctrl_info *gpio_data;

	gpio_data = msm_cdc_pinctrl_get_gpiodata(np);
	if (!gpio_data || !gpio_data->rst_gpio)
		return -EINVAL;

	return gpiod_get_raw_value_cansleep(gpio_data->rst_gpio);
}
EXPORT_SYMBOL(msm_cdc_get_gpio_state);

/*
 * msm_cdc_pinctrl_select_sleep_state: select pinctrl sleep state
 * @np: pointer to struct device_node
 *
 * Returns error code for failure
 */
int msm_cdc_pinctrl_select_sleep_state(struct device_node *np)
{
	struct msm_cdc_pinctrl_info *gpio_data;

	gpio_data = msm_cdc_pinctrl_get_gpiodata(np);
	if (!gpio_data)
		return -EINVAL;

	if (!gpio_data->pinctrl_sleep) {
		pr_err("%s: pinctrl sleep state is null\n", __func__);
		return -EINVAL;
	}
	gpio_data->state = false;

	return pinctrl_select_state(gpio_data->pinctrl,
				    gpio_data->pinctrl_sleep);
}
EXPORT_SYMBOL(msm_cdc_pinctrl_select_sleep_state);

/*
 * msm_cdc_pinctrl_select_alt_active_state: select pinctrl alt_active state
 * @np: pointer to struct device_node
 *
 * Returns error code for failure
 */
int msm_cdc_pinctrl_select_alt_active_state(struct device_node *np)
{
	struct msm_cdc_pinctrl_info *gpio_data;

	gpio_data = msm_cdc_pinctrl_get_gpiodata(np);
	if (!gpio_data)
		return -EINVAL;

	if (!gpio_data->pinctrl_alt_active) {
		pr_err("%s: pinctrl alt_active state is null\n", __func__);
		return -EINVAL;
	}
	gpio_data->state = true;

	return pinctrl_select_state(gpio_data->pinctrl,
				    gpio_data->pinctrl_alt_active);
}
EXPORT_SYMBOL(msm_cdc_pinctrl_select_alt_active_state);

/*
 * msm_cdc_pinctrl_select_active_state: select pinctrl active state
 * @np: pointer to struct device_node
 *
 * Returns error code for failure
 */
int msm_cdc_pinctrl_select_active_state(struct device_node *np)
{
	struct msm_cdc_pinctrl_info *gpio_data;

	gpio_data = msm_cdc_pinctrl_get_gpiodata(np);
	if (!gpio_data)
		return -EINVAL;

	if (!gpio_data->pinctrl_active) {
		pr_err("%s: pinctrl active state is null\n", __func__);
		return -EINVAL;
	}
	gpio_data->state = true;

	return pinctrl_select_state(gpio_data->pinctrl,
				    gpio_data->pinctrl_active);
}
EXPORT_SYMBOL(msm_cdc_pinctrl_select_active_state);

/*
 * msm_cdc_pinctrl_get_state: get curren pinctrl state
 * @np: pointer to struct device_node
 *
 * Returns 0 for sleep state, 1 for active state,
 * error code for failure
 */
int msm_cdc_pinctrl_get_state(struct device_node *np)
{
	struct msm_cdc_pinctrl_info *gpio_data;

	gpio_data = msm_cdc_pinctrl_get_gpiodata(np);
	if (!gpio_data)
		return -EINVAL;

	return gpio_data->state;
}
EXPORT_SYMBOL(msm_cdc_pinctrl_get_state);

/*
 * msm_cdc_pinctrl_set_wakeup_capable: Set a pinctrl to wakeup capable
 * @np: pointer to struct device_node
 * @enable: wakeup capable when set to true
 *
 * The donor programmed the MPM and chip wakeup registers named in the
 * node; no tissot node names any, so there is nothing to program.
 */
int msm_cdc_pinctrl_set_wakeup_capable(struct device_node *np, bool enable)
{
	return msm_cdc_pinctrl_get_gpiodata(np) ? 0 : -EINVAL;
}
EXPORT_SYMBOL(msm_cdc_pinctrl_set_wakeup_capable);

static int msm_cdc_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct msm_cdc_pinctrl_info *gpio_data;
	int ret;

	gpio_data = devm_kzalloc(dev, sizeof(*gpio_data), GFP_KERNEL);
	if (!gpio_data)
		return -ENOMEM;

	gpio_data->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(gpio_data->pinctrl))
		return dev_err_probe(dev, PTR_ERR(gpio_data->pinctrl),
				     "Cannot get cdc gpio pinctrl\n");

	gpio_data->pinctrl_active = pinctrl_lookup_state(gpio_data->pinctrl,
							 "aud_active");
	if (IS_ERR(gpio_data->pinctrl_active))
		return dev_err_probe(dev, PTR_ERR(gpio_data->pinctrl_active),
				     "Cannot get aud_active pinctrl state\n");

	gpio_data->pinctrl_sleep = pinctrl_lookup_state(gpio_data->pinctrl,
							"aud_sleep");
	if (IS_ERR(gpio_data->pinctrl_sleep))
		return dev_err_probe(dev, PTR_ERR(gpio_data->pinctrl_sleep),
				     "Cannot get aud_sleep pinctrl state\n");

	gpio_data->pinctrl_alt_active = pinctrl_lookup_state(gpio_data->pinctrl,
							     "aud_alt_active");
	if (IS_ERR(gpio_data->pinctrl_alt_active)) {
		dev_dbg(dev, "no aud_alt_active pinctrl state\n");
		gpio_data->pinctrl_alt_active = NULL;
	}

	/* Set pinctrl state to aud_sleep by default */
	ret = pinctrl_select_state(gpio_data->pinctrl, gpio_data->pinctrl_sleep);
	if (ret)
		dev_err(dev, "set cdc gpio sleep state fail: %d\n", ret);

	gpio_data->rst_gpio = devm_gpiod_get_optional(dev, "qcom,cdc-rst-n",
						      GPIOD_ASIS);
	if (IS_ERR(gpio_data->rst_gpio))
		return dev_err_probe(dev, PTR_ERR(gpio_data->rst_gpio),
				     "Failed to get the reset gpio\n");

	platform_set_drvdata(pdev, gpio_data);
	return 0;
}

static const struct of_device_id msm_cdc_pinctrl_match[] = {
	{.compatible = "qcom,msm-cdc-pinctrl"},
	{}
};
MODULE_DEVICE_TABLE(of, msm_cdc_pinctrl_match);

static struct platform_driver msm_cdc_pinctrl_driver = {
	.driver = {
		.name = "msm-cdc-pinctrl",
		.of_match_table = msm_cdc_pinctrl_match,
		.suppress_bind_attrs = true,
	},
	.probe = msm_cdc_pinctrl_probe,
};

int msm_cdc_pinctrl_drv_init(void)
{
	return platform_driver_register(&msm_cdc_pinctrl_driver);
}

void msm_cdc_pinctrl_drv_exit(void)
{
	platform_driver_unregister(&msm_cdc_pinctrl_driver);
}

/*
 * The donor registered the driver from the WCD9xxx core module, which
 * tissot does not build, so it is a module of its own here.
 */
static int __init msm_cdc_pinctrl_init(void)
{
	return msm_cdc_pinctrl_drv_init();
}
module_init(msm_cdc_pinctrl_init);

static void __exit msm_cdc_pinctrl_exit(void)
{
	msm_cdc_pinctrl_drv_exit();
}
module_exit(msm_cdc_pinctrl_exit);

MODULE_DESCRIPTION("MSM CODEC pin control platform driver");
MODULE_LICENSE("GPL");
