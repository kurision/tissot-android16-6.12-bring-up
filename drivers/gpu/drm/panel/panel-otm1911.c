// SPDX-License-Identifier: GPL-2.0
/*
 * Orise OTM1911 5.5" 1080x1920 MIPI-DSI video-mode panel, as fitted to the
 * Xiaomi Mi A1 (tissot). Init sequence and timings ported from the downstream
 * MSM8953 panel descriptor (dsi-panel-otm1911-fhd-video.dtsi), which drives
 * this panel in burst video mode with DCS on-commands in LP mode.
 *
 * No reset GPIO is used: tissot has no kernel-controlled panel reset line
 * (the downstream tree defines none either), so the bootloader's splash
 * state is taken as the reset. lab/ibb rails are likewise left in their
 * bootloader state; only vdd/vddio are modelled as regulators.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct otm1911 {
	struct device *dev;
	struct drm_panel panel;

	struct gpio_desc *reset_gpio;

	struct regulator *vdd;
	struct regulator *vddio;
};

static inline struct otm1911 *panel_to_otm1911(struct drm_panel *panel)
{
	return container_of(panel, struct otm1911, panel);
}

static int otm1911_enable(struct drm_panel *panel)
{
	struct otm1911 *ctx = panel_to_otm1911(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };
	ssize_t status_ret;
	u8 status = 0;

	/* Downstream status check is cmd 0x0a reading 0x9c; this both proves
	 * the DSI link and identifies the glass. Never fails the modeset. */
	status_ret = mipi_dsi_dcs_read(dsi, 0x0a, &status, 1);
	if (status_ret < 0)
		dev_warn(ctx->dev, "panel status read failed: %zd\n", status_ret);
	else
		dev_info(ctx->dev, "panel status 0x%02x (downstream expects 0x9c)\n",
			 status);
	/* Downstream on-command, sent in LP mode */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x51, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x53, 0x2c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x55, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 120);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int otm1911_disable(struct drm_panel *panel)
{
	struct otm1911 *ctx = panel_to_otm1911(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int otm1911_unprepare(struct drm_panel *panel)
{
	struct otm1911 *ctx = panel_to_otm1911(panel);

	if (ctx->reset_gpio)
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	regulator_disable(ctx->vddio);
	regulator_disable(ctx->vdd);

	return 0;
}

static int otm1911_prepare(struct drm_panel *panel)
{
	struct otm1911 *ctx = panel_to_otm1911(panel);
	int ret;

	ret = regulator_enable(ctx->vdd);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to enable vdd supply: %d\n", ret);
		return ret;
	}

	ret = regulator_enable(ctx->vddio);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to enable vddio supply: %d\n", ret);
		regulator_disable(ctx->vdd);
		return ret;
	}

	/* Downstream reset sequence is <1 10>, <0 10>, <1 10> */
	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		usleep_range(10000, 11000);
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		usleep_range(10000, 11000);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		usleep_range(10000, 11000);
	}

	return 0;
}

static const struct drm_display_mode otm1911_default_mode = {
	.hdisplay    = 1080,
	.hsync_start = 1080 + 24,
	.hsync_end   = 1080 + 24 + 20,
	.htotal	     = 1080 + 24 + 20 + 24,
	.vdisplay    = 1920,
	.vsync_start = 1920 + 14,
	.vsync_end   = 1920 + 14 + 2,
	.vtotal	     = 1920 + 14 + 2 + 6,
	.clock	     = 133766,
	.flags	     = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	.width_mm    = 69,
	.height_mm   = 122,
};

static const u32 otm1911_bus_formats[] = {
	MEDIA_BUS_FMT_RGB888_1X24,
};

static int otm1911_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &otm1911_default_mode);
	if (!mode) {
		dev_err(panel->dev, "Failed to add mode 1080x1920@60\n");
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	drm_display_info_set_bus_formats(&connector->display_info,
					 otm1911_bus_formats,
					 ARRAY_SIZE(otm1911_bus_formats));

	return 1;
}

static const struct drm_panel_funcs otm1911_drm_funcs = {
	.disable   = otm1911_disable,
	.unprepare = otm1911_unprepare,
	.prepare   = otm1911_prepare,
	.enable	   = otm1911_enable,
	.get_modes = otm1911_get_modes,
};

static int otm1911_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct otm1911 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	/* Optional: tissot wires no kernel-controlled panel reset line */
	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to request reset gpio\n");

	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dev = dev;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO |
		MIPI_DSI_MODE_VIDEO_BURST | MIPI_DSI_MODE_VIDEO_SYNC_PULSE;

	ctx->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(ctx->vdd))
		return dev_err_probe(dev, PTR_ERR(ctx->vdd),
				     "Failed to request vdd regulator\n");

	ctx->vddio = devm_regulator_get(dev, "vddio");
	if (IS_ERR(ctx->vddio))
		return dev_err_probe(dev, PTR_ERR(ctx->vddio),
				     "Failed to request vddio regulator\n");

	drm_panel_init(&ctx->panel, dev, &otm1911_drm_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "mipi_dsi_attach failed (%d). Is host ready?\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	dev_info(dev, "1080x1920@60 24bpp dsi 4dl - ready\n");

	return 0;
}

static void otm1911_shutdown(struct mipi_dsi_device *dsi)
{
	struct otm1911 *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_unprepare(&ctx->panel);
	drm_panel_disable(&ctx->panel);
}

static void otm1911_remove(struct mipi_dsi_device *dsi)
{
	struct otm1911 *ctx = mipi_dsi_get_drvdata(dsi);

	otm1911_shutdown(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id otm1911_of_match[] = {
	{ .compatible = "orisetech,otm1911" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, otm1911_of_match);

static struct mipi_dsi_driver otm1911_driver = {
	.probe	= otm1911_probe,
	.remove = otm1911_remove,
	.shutdown = otm1911_shutdown,
	.driver = {
		.name = "panel-otm1911",
		.of_match_table = otm1911_of_match,
	},
};
module_mipi_dsi_driver(otm1911_driver);

MODULE_AUTHOR("omp");
MODULE_DESCRIPTION("DRM driver for Orise OTM1911 1080x1920 MIPI-DSI panel");
MODULE_LICENSE("GPL v2");
