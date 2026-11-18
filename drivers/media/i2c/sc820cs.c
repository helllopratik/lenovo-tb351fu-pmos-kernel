// SPDX-License-Identifier: GPL-2.0
/*
 * SmartSens SC820CS 5M CMOS Image Sensor Driver
 *
 * Copyright (C) 2026 postmarketOS
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#define SC820CS_CHIP_ID_HI		0xd1
#define SC820CS_CHIP_ID_LO		0x54
#define SC820CS2ST_CHIP_ID_LO		0x55

#define SC820CS_REG_CHIP_ID_HI		0x3107
#define SC820CS_REG_CHIP_ID_LO		0x3108
#define SC820CS_REG_MODE_SELECT		0x0100
#define SC820CS_REG_SOFTWARE_RESET	0x0103
#define SC820CS_MODE_STANDBY		0x00
#define SC820CS_MODE_STREAMING		0x01

#define SC820CS_DEFAULT_WIDTH		1632
#define SC820CS_DEFAULT_HEIGHT		1224

struct sc820cs_reg {
	u16 addr;
	u8 val;
};

static const struct sc820cs_reg sc820cs_init_regs[] = {
	{ 0x36e9, 0x80 }, { 0x37f9, 0x80 }, { 0x36e9, 0x24 }, { 0x37f9, 0x24 },
	{ 0x301f, 0x0e }, { 0x3205, 0xc7 }, { 0x3211, 0x04 }, { 0x3270, 0x00 },
	{ 0x3271, 0x00 }, { 0x3272, 0x00 }, { 0x3273, 0x03 }, { 0x3301, 0x08 },
	{ 0x3303, 0x10 }, { 0x3306, 0x84 }, { 0x3309, 0x88 }, { 0x330a, 0x01 },
	{ 0x330b, 0x0c }, { 0x330c, 0x10 }, { 0x330d, 0x18 }, { 0x330e, 0x30 },
	{ 0x3314, 0x15 }, { 0x331f, 0x79 }, { 0x3326, 0x0e }, { 0x3327, 0x0a },
	{ 0x3329, 0x0b }, { 0x3333, 0x10 }, { 0x3334, 0x40 }, { 0x335d, 0x60 },
	{ 0x3364, 0x56 }, { 0x336c, 0xce }, { 0x3390, 0x08 }, { 0x3391, 0x09 },
	{ 0x3392, 0x0f }, { 0x3393, 0x10 }, { 0x3394, 0x20 }, { 0x3395, 0x28 },
	{ 0x33ad, 0x3c }, { 0x33af, 0x70 }, { 0x33b2, 0x70 }, { 0x33b3, 0x40 },
	{ 0x349f, 0x1e }, { 0x34a6, 0x09 }, { 0x34a7, 0x0f }, { 0x34a8, 0x30 },
	{ 0x34a9, 0x20 }, { 0x34f8, 0x1f }, { 0x34f9, 0x08 }, { 0x3637, 0x43 },
	{ 0x363c, 0x8d }, { 0x3670, 0x4a }, { 0x3674, 0xf6 }, { 0x3675, 0xdc },
	{ 0x3676, 0xcc }, { 0x367c, 0x09 }, { 0x367d, 0x0f }, { 0x3690, 0x34 },
	{ 0x3691, 0x44 }, { 0x3692, 0x55 }, { 0x3698, 0x86 }, { 0x3699, 0x8d },
	{ 0x369a, 0x99 }, { 0x369b, 0xb7 }, { 0x369c, 0x0f }, { 0x369d, 0x1f },
	{ 0x36a2, 0x09 }, { 0x36a3, 0x0b }, { 0x36a4, 0x0f }, { 0x36b0, 0x48 },
	{ 0x36b1, 0x38 }, { 0x36b2, 0x41 }, { 0x370f, 0x01 }, { 0x3724, 0xc1 },
	{ 0x3771, 0x07 }, { 0x3772, 0x03 }, { 0x3773, 0x63 }, { 0x377a, 0x08 },
	{ 0x377b, 0x0f }, { 0x3901, 0x04 }, { 0x3903, 0xa0 }, { 0x3905, 0x8d },
	{ 0x391d, 0x01 }, { 0x3926, 0x23 }, { 0x393f, 0x80 }, { 0x3940, 0x00 },
	{ 0x3941, 0x00 }, { 0x3942, 0x00 }, { 0x3943, 0x63 }, { 0x3944, 0x5f },
	{ 0x3e00, 0x01 }, { 0x3e01, 0x38 }, { 0x3e02, 0x00 }, { 0x4401, 0x13 },
	{ 0x4402, 0x03 }, { 0x4403, 0x0e }, { 0x4404, 0x28 }, { 0x4405, 0x34 },
	{ 0x4407, 0x0e }, { 0x440c, 0x42 }, { 0x440d, 0x42 }, { 0x440e, 0x32 },
	{ 0x440f, 0x53 }, { 0x4412, 0x01 }, { 0x4424, 0x01 }, { 0x442d, 0x00 },
	{ 0x442e, 0x00 }, { 0x4509, 0x28 }, { 0x450d, 0x18 }, { 0x451d, 0xc8 },
	{ 0x4526, 0x09 }, { 0x5000, 0x0e }, { 0x550e, 0x00 }, { 0x550f, 0xbc },
	{ 0x5780, 0x66 }, { 0x578d, 0x40 }, { 0x57aa, 0xeb },
};

static const struct sc820cs_reg sc820cs_preview_regs[] = {
	{ 0x301f, 0x04 }, { 0x3200, 0x00 }, { 0x3201, 0x00 }, { 0x3202, 0x00 },
	{ 0x3203, 0x00 }, { 0x3204, 0x0c }, { 0x3206, 0x09 }, { 0x3207, 0x9f },
	{ 0x3208, 0x06 }, { 0x3209, 0x60 }, { 0x320a, 0x04 }, { 0x320b, 0xc8 },
	{ 0x3210, 0x00 }, { 0x3212, 0x00 }, { 0x3213, 0x04 }, { 0x3215, 0x31 },
	{ 0x3220, 0x01 }, { 0x5000, 0x4e }, { 0x5900, 0xf1 }, { 0x5901, 0x04 },
};

struct sc820cs {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct i2c_client *client;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *vcamd_gpio;
	struct gpio_desc *vcama_gpio;
	struct regulator *vcamio;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *link_freq;
	struct v4l2_mbus_framefmt format;
	bool streaming;
};

static inline struct sc820cs *to_sc820cs(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sc820cs, sd);
}

static int sc820cs_read_reg(struct i2c_client *client, u16 reg, u8 *val)
{
	u8 buf[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[2] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 2,
			.buf = buf,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = val,
		},
	};
	int ret = i2c_transfer(client->adapter, msgs, 2);

	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static int sc820cs_write_reg(struct i2c_client *client, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xff, val };
	int ret = i2c_master_send(client, buf, 3);

	return ret == 3 ? 0 : -EIO;
}

static int sc820cs_write_array(struct i2c_client *client,
			       const struct sc820cs_reg *regs,
			       unsigned int num_regs)
{
	unsigned int i;
	int ret;

	for (i = 0; i < num_regs; i++) {
		ret = sc820cs_write_reg(client, regs[i].addr, regs[i].val);
		if (ret)
			return ret;
	}
	return 0;
}

static int sc820cs_power_on(struct sc820cs *sensor)
{
	int ret;

	pr_emerg("sc820cs: power_on: resetting gpio low\n");
	/* 1. Assert RST low first */
	if (sensor->reset_gpio)
		gpiod_set_value_cansleep(sensor->reset_gpio, 0);

	/* 2. Turn on MCLK 24 MHz first */
	if (sensor->mclk) {
		pr_emerg("sc820cs: power_on: enabling mclk 24MHz\n");
		clk_set_rate(sensor->mclk, 24000000);
		ret = clk_prepare_enable(sensor->mclk);
		if (ret) {
			pr_emerg("sc820cs: failed to enable mclk: %d\n", ret);
			return ret;
		}
	}
	usleep_range(1000, 2000);

	/* 3. DOVDD 1.8V is handled by mt6366_cam_ldo (VRF18) */
	usleep_range(1000, 2000);

	/* 4. DVDD 1.2V */
	if (sensor->vcamd_gpio) {
		pr_emerg("sc820cs: power_on: asserting vcamd 1.2V\n");
		gpiod_set_value_cansleep(sensor->vcamd_gpio, 1);
	}
	usleep_range(1000, 2000);

	/* 5. AVDD 2.8V */
	if (sensor->vcama_gpio) {
		pr_emerg("sc820cs: power_on: asserting vcama 2.8V\n");
		gpiod_set_value_cansleep(sensor->vcama_gpio, 1);
	}
	usleep_range(2000, 3000);

	/* 6. Release RST high */
	if (sensor->reset_gpio) {
		pr_emerg("sc820cs: power_on: releasing reset gpio high\n");
		gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	}
	msleep(10);
	pr_emerg("sc820cs: power_on: complete\n");

	return 0;
}

static bool keep_power = true;
module_param(keep_power, bool, 0644);
MODULE_PARM_DESC(keep_power, "Keep sensor powered on probe failure for debugging");

static void sc820cs_power_off(struct sc820cs *sensor)
{
	pr_emerg("sc820cs: power_off\n");
	if (sensor->reset_gpio)
		gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	usleep_range(1000, 2000);
	if (sensor->vcama_gpio)
		gpiod_set_value_cansleep(sensor->vcama_gpio, 0);
	if (sensor->vcamd_gpio)
		gpiod_set_value_cansleep(sensor->vcamd_gpio, 0);
	if (sensor->mclk)
		clk_disable_unprepare(sensor->mclk);
}

static int sc820cs_identify(struct sc820cs *sensor)
{
	u8 hi = 0, lo = 0;
	int ret;

	pr_emerg("sc820cs: reading chip ID\n");
	ret = sc820cs_read_reg(sensor->client, SC820CS_REG_CHIP_ID_HI, &hi);
	if (!ret)
		ret = sc820cs_read_reg(sensor->client, SC820CS_REG_CHIP_ID_LO, &lo);
	if (ret) {
		pr_emerg("sc820cs: failed to read chip ID: %d\n", ret);
		return ret;
	}

	if (hi != SC820CS_CHIP_ID_HI ||
	    (lo != SC820CS_CHIP_ID_LO && lo != SC820CS2ST_CHIP_ID_LO)) {
		pr_emerg("sc820cs: unexpected chip ID: 0x%02x%02x\n", hi, lo);
		return -ENODEV;
	}

	pr_emerg("sc820cs: SmartSens SC820CS detected (ID 0x%02x%02x)\n", hi, lo);
	return 0;
}

static int sc820cs_start_streaming(struct sc820cs *sensor)
{
	int ret;

	pr_emerg("sc820cs: resetting and initializing\n");
	sc820cs_write_reg(sensor->client, SC820CS_REG_SOFTWARE_RESET, 0x01);
	msleep(10);
	sc820cs_write_reg(sensor->client, SC820CS_REG_MODE_SELECT, 0x00);

	pr_emerg("sc820cs: writing %zu init regs\n", ARRAY_SIZE(sc820cs_init_regs));
	ret = sc820cs_write_array(sensor->client, sc820cs_init_regs,
				  ARRAY_SIZE(sc820cs_init_regs));
	if (ret) {
		pr_emerg("sc820cs: write init regs failed: %d\n", ret);
		return ret;
	}

	pr_emerg("sc820cs: writing %zu preview regs\n", ARRAY_SIZE(sc820cs_preview_regs));
	ret = sc820cs_write_array(sensor->client, sc820cs_preview_regs,
				  ARRAY_SIZE(sc820cs_preview_regs));
	if (ret) {
		pr_emerg("sc820cs: write preview regs failed: %d\n", ret);
		return ret;
	}

	pr_emerg("sc820cs: setting MODE_STREAMING (0x0100 = 0x01)\n");
	ret = sc820cs_write_reg(sensor->client, SC820CS_REG_MODE_SELECT,
				SC820CS_MODE_STREAMING);
	if (ret)
		pr_emerg("sc820cs: setting MODE_STREAMING failed: %d\n", ret);
	else
		pr_emerg("sc820cs: MODE_STREAMING set successfully!\n");
	return ret;
}

static int sc820cs_stop_streaming(struct sc820cs *sensor)
{
	pr_emerg("sc820cs: stop_streaming called\n");
	return sc820cs_write_reg(sensor->client, SC820CS_REG_MODE_SELECT,
				 SC820CS_MODE_STANDBY);
}

static int sc820cs_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct sc820cs *sensor = to_sc820cs(sd);
	int ret = 0;

	pr_emerg("sc820cs: s_stream called: enable=%d current_streaming=%d\n",
		 enable, sensor->streaming);

	if (sensor->streaming == enable)
		return 0;

	if (enable) {
		ret = pm_runtime_resume_and_get(&sensor->client->dev);
		if (ret < 0) {
			pr_emerg("sc820cs: pm_runtime_resume_and_get failed: %d\n", ret);
			return ret;
		}
		ret = sc820cs_start_streaming(sensor);
		if (ret) {
			pr_emerg("sc820cs: start_streaming failed: %d\n", ret);
			pm_runtime_put(&sensor->client->dev);
			return ret;
		}
	} else {
		sc820cs_stop_streaming(sensor);
		pm_runtime_put(&sensor->client->dev);
	}

	sensor->streaming = enable;
	pr_emerg("sc820cs: s_stream completed successfully (enable=%d)\n", enable);
	return ret;
}

static int sc820cs_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	return 0;
}

static int sc820cs_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index > 0 || fse->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;

	fse->min_width = SC820CS_DEFAULT_WIDTH;
	fse->max_width = SC820CS_DEFAULT_WIDTH;
	fse->min_height = SC820CS_DEFAULT_HEIGHT;
	fse->max_height = SC820CS_DEFAULT_HEIGHT;
	return 0;
}

static int sc820cs_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct sc820cs *sensor = to_sc820cs(sd);
	struct v4l2_mbus_framefmt *format;

	if (fmt->pad != 0)
		return -EINVAL;

	format = v4l2_subdev_state_get_format(sd_state, fmt->pad);
	if (format) {
		fmt->format = *format;
		return 0;
	}

	fmt->format = sensor->format;
	return 0;
}

static int sc820cs_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *fmt)
{
	struct sc820cs *sensor = to_sc820cs(sd);
	struct v4l2_mbus_framefmt *format;

	if (fmt->pad != 0)
		return -EINVAL;

	fmt->format.width = SC820CS_DEFAULT_WIDTH;
	fmt->format.height = SC820CS_DEFAULT_HEIGHT;
	fmt->format.code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_SRGB;

	format = v4l2_subdev_state_get_format(sd_state, fmt->pad);
	if (format)
		*format = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		sensor->format = fmt->format;

	return 0;
}

static int sc820cs_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.width = SC820CS_DEFAULT_WIDTH,
			.height = SC820CS_DEFAULT_HEIGHT,
			.code = MEDIA_BUS_FMT_SBGGR10_1X10,
			.field = V4L2_FIELD_NONE,
			.colorspace = V4L2_COLORSPACE_SRGB,
		},
	};

	return sc820cs_set_fmt(sd, sd_state, &fmt);
}

static const struct v4l2_subdev_internal_ops sc820cs_internal_ops = {
	.init_state = sc820cs_init_state,
};

static const struct media_entity_operations sc820cs_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_video_ops sc820cs_video_ops = {
	.s_stream = sc820cs_s_stream,
};

static const struct v4l2_subdev_pad_ops sc820cs_pad_ops = {
	.enum_mbus_code = sc820cs_enum_mbus_code,
	.enum_frame_size = sc820cs_enum_frame_size,
	.get_fmt = sc820cs_get_fmt,
	.set_fmt = sc820cs_set_fmt,
};

static const struct v4l2_subdev_ops sc820cs_subdev_ops = {
	.video = &sc820cs_video_ops,
	.pad = &sc820cs_pad_ops,
};

static const s64 sc820cs_link_freq_menu_items[] = {
	264000000,
};

static int sc820cs_init_controls(struct sc820cs *sensor)
{
	struct v4l2_ctrl_handler *hdl = &sensor->ctrl_handler;

	v4l2_ctrl_handler_init(hdl, 2);

	sensor->link_freq = v4l2_ctrl_new_int_menu(hdl, NULL,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(sc820cs_link_freq_menu_items) - 1,
						   0, sc820cs_link_freq_menu_items);
	if (sensor->link_freq)
		sensor->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->pixel_rate = v4l2_ctrl_new_std(hdl, NULL,
					       V4L2_CID_PIXEL_RATE,
					       264000000, 264000000, 1,
					       264000000);
	if (sensor->pixel_rate)
		sensor->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->sd.ctrl_handler = hdl;
	return hdl->error;
}

static int sc820cs_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sc820cs *sensor;
	int ret;

	pr_emerg("sc820cs: probe entry for client %s at addr 0x%02x\n",
		 dev_name(dev), client->addr);

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->client = client;

	pr_emerg("sc820cs: v4l2_i2c_subdev_init\n");
	v4l2_i2c_subdev_init(&sensor->sd, client, &sc820cs_subdev_ops);
	sensor->sd.fwnode = dev_fwnode(dev);
	sensor->sd.internal_ops = &sc820cs_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.ops = &sc820cs_subdev_entity_ops;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;

	pr_emerg("sc820cs: getting clk/gpios\n");
	sensor->mclk = devm_clk_get_optional(dev, "mclk");
	if (IS_ERR(sensor->mclk))
		return PTR_ERR(sensor->mclk);

	sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(sensor->reset_gpio))
		return PTR_ERR(sensor->reset_gpio);

	sensor->vcamd_gpio = devm_gpiod_get_optional(dev, "vcamd", GPIOD_OUT_LOW);
	if (IS_ERR(sensor->vcamd_gpio))
		return PTR_ERR(sensor->vcamd_gpio);

	sensor->vcama_gpio = devm_gpiod_get_optional(dev, "vcama", GPIOD_OUT_LOW);
	if (IS_ERR(sensor->vcama_gpio))
		return PTR_ERR(sensor->vcama_gpio);

	sensor->vcamio = devm_regulator_get_optional(dev, "vcamio");
	if (IS_ERR(sensor->vcamio))
		sensor->vcamio = NULL;

	pr_emerg("sc820cs: resources: mclk=%p (%ld Hz) reset=%p vcamd=%p vcama=%p vcamio=%p\n",
		 sensor->mclk, sensor->mclk ? clk_get_rate(sensor->mclk) : 0,
		 sensor->reset_gpio, sensor->vcamd_gpio, sensor->vcama_gpio, sensor->vcamio);

	pr_emerg("sc820cs: powering on\n");
	ret = sc820cs_power_on(sensor);
	if (ret)
		return ret;

	pr_emerg("sc820cs: identifying sensor\n");
	ret = sc820cs_identify(sensor);
	if (ret) {
		pr_emerg("sc820cs: identify failed: %d\n", ret);
		if (!keep_power)
			sc820cs_power_off(sensor);
		return ret;
	}

	pr_emerg("sc820cs: initializing controls\n");
	ret = sc820cs_init_controls(sensor);
	if (ret) {
		sc820cs_power_off(sensor);
		return ret;
	}

	pr_emerg("sc820cs: initializing media entity pads\n");
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		goto err_ctrl;

	sensor->sd.state_lock = sensor->ctrl_handler.lock;
	pr_emerg("sc820cs: finalizing subdev\n");
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret) {
		pr_emerg("sc820cs: finalize subdev failed: %d\n", ret);
		goto err_entity;
	}

	sensor->format.width = SC820CS_DEFAULT_WIDTH;
	sensor->format.height = SC820CS_DEFAULT_HEIGHT;
	sensor->format.code = MEDIA_BUS_FMT_SBGGR10_1X10;
	sensor->format.field = V4L2_FIELD_NONE;
	sensor->format.colorspace = V4L2_COLORSPACE_SRGB;

	pr_emerg("sc820cs: setting pm runtime active and enable\n");
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	pr_emerg("sc820cs: registering subdev with v4l2 async\n");
	ret = v4l2_async_register_subdev(&sensor->sd);
	if (ret) {
		pr_emerg("sc820cs: async register subdev failed: %d\n", ret);
		goto err_pm;
	}

	pr_emerg("sc820cs: probe completed successfully!\n");
	return 0;

err_pm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&sensor->sd);
err_entity:
	media_entity_cleanup(&sensor->sd.entity);
err_ctrl:
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
	sc820cs_power_off(sensor);
	return ret;
}

static void sc820cs_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sc820cs *sensor = to_sc820cs(sd);

	pr_emerg("sc820cs: remove start\n");
	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		sc820cs_power_off(sensor);
	pm_runtime_set_suspended(&client->dev);
	pr_emerg("sc820cs: remove done\n");
}

static const struct of_device_id sc820cs_of_match[] = {
	{ .compatible = "smartsens,sc820cs" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sc820cs_of_match);

static struct i2c_driver sc820cs_driver = {
	.driver = {
		.name = "sc820cs",
		.of_match_table = sc820cs_of_match,
	},
	.probe = sc820cs_probe,
	.remove = sc820cs_remove,
};
module_i2c_driver(sc820cs_driver);

MODULE_AUTHOR("helllopratik <pratikgondane07@gmail.com>");
MODULE_DESCRIPTION("SmartSens SC820CS camera sensor driver");
MODULE_LICENSE("GPL");
