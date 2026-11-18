// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S5K4H7YX 8M CMOS Image Sensor Driver
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

#define S5K4H7YX_CHIP_ID_HI		0x48
#define S5K4H7YX_CHIP_ID_LO		0x7b
#define S5K4H7YX2ST_CHIP_ID_LO		0x7c

#define S5K4H7YX_REG_CHIP_ID_HI		0x0000
#define S5K4H7YX_REG_CHIP_ID_LO		0x0001
#define S5K4H7YX_REG_MODE_SELECT	0x0100
#define S5K4H7YX_MODE_STANDBY		0x00
#define S5K4H7YX_MODE_STREAMING		0x01

#define S5K4H7YX_DEFAULT_WIDTH		1632
#define S5K4H7YX_DEFAULT_HEIGHT		1224

struct s5k4h7yx_reg {
	u16 addr;
	u8 val;
};

static const struct s5k4h7yx_reg s5k4h7_init_regs[] = {
	{ 0x0100, 0x00 },
	{ 0x0000, 0x12 },
	{ 0x0000, 0x48 },
	{ 0x0A02, 0x15 },
	{ 0x0B05, 0x01 },
	{ 0x3074, 0x06 },
	{ 0x3075, 0x2F },
	{ 0x308A, 0x20 },
	{ 0x308B, 0x08 },
	{ 0x308C, 0x0B },
	{ 0x3081, 0x07 },
	{ 0x307B, 0x85 },
	{ 0x307A, 0x0A },
	{ 0x3079, 0x0A },
	{ 0x306E, 0x71 },
	{ 0x306F, 0x28 },
	{ 0x301F, 0x20 },
	{ 0x306B, 0x9A },
	{ 0x3091, 0x1F },
	{ 0x30C4, 0x06 },
	{ 0x3200, 0x09 },
	{ 0x306A, 0x79 },
	{ 0x30B0, 0xFF },
	{ 0x306D, 0x08 },
	{ 0x3080, 0x00 },
	{ 0x3929, 0x3F },
	{ 0x3084, 0x16 },
	{ 0x3070, 0x0F },
	{ 0x3B45, 0x01 },
	{ 0x30C2, 0x05 },
	{ 0x3069, 0x87 },
	{ 0x3924, 0x7F },
	{ 0x3925, 0xFD },
	{ 0x3C08, 0xFF },
	{ 0x3C09, 0xFF },
	{ 0x3C31, 0xFF },
	{ 0x3C32, 0xFF },
	{ 0x0100, 0x00 },
	{ 0x0136, 0x18 },
	{ 0x0137, 0x00 },
	{ 0x0305, 0x06 },
	{ 0x0306, 0x00 },
	{ 0x0307, 0x8C },
	{ 0x030D, 0x06 },
	{ 0x030E, 0x00 },
	{ 0x030F, 0xAF },
	{ 0x3C1F, 0x00 },
	{ 0x3C17, 0x00 },
	{ 0x3C1C, 0x05 },
	{ 0x3C1D, 0x15 },
	{ 0x0301, 0x04 },
	{ 0x0820, 0x02 },
	{ 0x0821, 0xBC },
	{ 0x0822, 0x00 },
	{ 0x0823, 0x00 },
	{ 0x0112, 0x0A },
	{ 0x0113, 0x0A },
	{ 0x0114, 0x03 },
	{ 0x3906, 0x04 },
	{ 0x0344, 0x00 },
	{ 0x0345, 0x00 },
	{ 0x0346, 0x00 },
	{ 0x0347, 0x00 },
	{ 0x0348, 0x0C },
	{ 0x0349, 0xCF },
	{ 0x034A, 0x09 },
	{ 0x034B, 0x9F },
	{ 0x034C, 0x0C },
	{ 0x034D, 0xD0 },
	{ 0x034E, 0x09 },
	{ 0x034F, 0xA0 },
	{ 0x0900, 0x00 },
	{ 0x0901, 0x00 },
	{ 0x0381, 0x01 },
	{ 0x0383, 0x01 },
	{ 0x0385, 0x01 },
	{ 0x0387, 0x01 },
	{ 0x0101, 0x00 },
	{ 0x0340, 0x13 },
	{ 0x0341, 0xC4 },
	{ 0x0342, 0x0E },
	{ 0x0343, 0x68 },
	{ 0x0200, 0x0D },
	{ 0x0201, 0xD8 },
	{ 0x0202, 0x10 },
	{ 0x0203, 0x02 },
	{ 0x3400, 0x01 },
	{ 0x0B00, 0x00 },
	{ 0x0100, 0x01 },
};

static const struct s5k4h7yx_reg s5k4h7_preview_regs[] = {
	{ 0x0100, 0x00 },
	{ 0x0136, 0x18 },
	{ 0x0137, 0x00 },
	{ 0x0305, 0x06 },
	{ 0x0306, 0x00 },
	{ 0x0307, 0x8C },
	{ 0x030D, 0x06 },
	{ 0x030E, 0x00 },
	{ 0x030F, 0xAF },
	{ 0x3C1F, 0x00 },
	{ 0x3C17, 0x00 },
	{ 0x3C1C, 0x05 },
	{ 0x3C1D, 0x15 },
	{ 0x0301, 0x04 },
	{ 0x0820, 0x02 },
	{ 0x0821, 0xBC },
	{ 0x0822, 0x00 },
	{ 0x0823, 0x00 },
	{ 0x0112, 0x0A },
	{ 0x0113, 0x0A },
	{ 0x0114, 0x03 },
	{ 0x3906, 0x00 },
	{ 0x0344, 0x00 },
	{ 0x0345, 0x08 },
	{ 0x0346, 0x00 },
	{ 0x0347, 0x08 },
	{ 0x0348, 0x0C },
	{ 0x0349, 0xC7 },
	{ 0x034A, 0x09 },
	{ 0x034B, 0x97 },
	{ 0x034C, 0x06 },
	{ 0x034D, 0x60 },
	{ 0x034E, 0x04 },
	{ 0x034F, 0xC8 },
	{ 0x0900, 0x01 },
	{ 0x0901, 0x22 },
	{ 0x0381, 0x01 },
	{ 0x0383, 0x01 },
	{ 0x0385, 0x01 },
	{ 0x0387, 0x03 },
	{ 0x0101, 0x00 },
	{ 0x0340, 0x09 },
	{ 0x0341, 0xE2 },
	{ 0x0342, 0x0E },
	{ 0x0343, 0x68 },
	{ 0x0200, 0x0D },
	{ 0x0201, 0xD8 },
	{ 0x0202, 0x02 },
	{ 0x0203, 0x08 },
	{ 0x3400, 0x01 },
};

struct s5k4h7yx {
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

static inline struct s5k4h7yx *to_s5k4h7yx(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5k4h7yx, sd);
}

static int s5k4h7yx_read_reg(struct i2c_client *client, u16 reg, u8 *val)
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

static int s5k4h7yx_write_reg(struct i2c_client *client, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xff, val };
	int ret = i2c_master_send(client, buf, 3);

	return ret == 3 ? 0 : -EIO;
}

static int s5k4h7yx_write_array(struct i2c_client *client,
				const struct s5k4h7yx_reg *regs,
				unsigned int num_regs)
{
	unsigned int i;
	int ret;

	for (i = 0; i < num_regs; i++) {
		ret = s5k4h7yx_write_reg(client, regs[i].addr, regs[i].val);
		if (ret)
			return ret;
	}
	return 0;
}

static int s5k4h7yx_power_on(struct s5k4h7yx *sensor)
{
	int ret;

	pr_emerg("s5k4h7yx: power_on: resetting gpio low\n");
	/* 1. Assert RST low first */
	if (sensor->reset_gpio)
		gpiod_set_value_cansleep(sensor->reset_gpio, 0);

	/* 2. Turn on MCLK 24 MHz first */
	if (sensor->mclk) {
		pr_emerg("s5k4h7yx: power_on: enabling mclk 24MHz\n");
		clk_set_rate(sensor->mclk, 24000000);
		ret = clk_prepare_enable(sensor->mclk);
		if (ret) {
			pr_emerg("s5k4h7yx: failed to enable mclk: %d\n", ret);
			return ret;
		}
	}
	usleep_range(1000, 2000);

	/* 3. AVDD 2.8V */
	if (sensor->vcama_gpio) {
		pr_emerg("s5k4h7yx: power_on: asserting vcama 2.8V\n");
		gpiod_set_value_cansleep(sensor->vcama_gpio, 1);
	}
	usleep_range(1000, 2000);

	/* 4. DVDD 1.2V */
	if (sensor->vcamd_gpio) {
		pr_emerg("s5k4h7yx: power_on: asserting vcamd 1.2V\n");
		gpiod_set_value_cansleep(sensor->vcamd_gpio, 1);
	}
	usleep_range(1000, 2000);

	/* 5. DOVDD 1.8V is handled by mt6366_cam_ldo (VRF18) */
	usleep_range(2000, 3000);

	/* 6. Release RST high */
	if (sensor->reset_gpio) {
		pr_emerg("s5k4h7yx: power_on: releasing reset gpio high\n");
		gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	}
	msleep(10);
	pr_emerg("s5k4h7yx: power_on: complete\n");

	return 0;
}

static bool keep_power = true;
module_param(keep_power, bool, 0644);
MODULE_PARM_DESC(keep_power, "Keep sensor powered on probe failure for debugging");

static void s5k4h7yx_power_off(struct s5k4h7yx *sensor)
{
	pr_emerg("s5k4h7yx: power_off\n");
	if (sensor->reset_gpio)
		gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	usleep_range(1000, 2000);
	if (sensor->vcamd_gpio)
		gpiod_set_value_cansleep(sensor->vcamd_gpio, 0);
	if (sensor->vcama_gpio)
		gpiod_set_value_cansleep(sensor->vcama_gpio, 0);
	if (sensor->mclk)
		clk_disable_unprepare(sensor->mclk);
}

static int s5k4h7yx_identify(struct s5k4h7yx *sensor)
{
	u8 hi = 0, lo = 0;
	int ret;

	pr_emerg("s5k4h7yx: reading chip ID\n");
	ret = s5k4h7yx_read_reg(sensor->client, S5K4H7YX_REG_CHIP_ID_HI, &hi);
	if (!ret)
		ret = s5k4h7yx_read_reg(sensor->client, S5K4H7YX_REG_CHIP_ID_LO, &lo);
	if (ret) {
		pr_emerg("s5k4h7yx: failed to read chip ID: %d\n", ret);
		return ret;
	}

	if (hi != S5K4H7YX_CHIP_ID_HI ||
	    (lo != S5K4H7YX_CHIP_ID_LO && lo != S5K4H7YX2ST_CHIP_ID_LO)) {
		pr_emerg("s5k4h7yx: unexpected chip ID: 0x%02x%02x\n", hi, lo);
		return -ENODEV;
	}

	pr_emerg("s5k4h7yx: Samsung S5K4H7YX detected (ID 0x%02x%02x)\n", hi, lo);
	return 0;
}

static int s5k4h7yx_start_streaming(struct s5k4h7yx *sensor)
{
	int ret;

	pr_emerg("s5k4h7yx: start_streaming: writing %zu init regs\n",
		 ARRAY_SIZE(s5k4h7_init_regs));
	ret = s5k4h7yx_write_array(sensor->client, s5k4h7_init_regs,
				   ARRAY_SIZE(s5k4h7_init_regs));
	if (ret) {
		pr_emerg("s5k4h7yx: write init regs failed: %d\n", ret);
		return ret;
	}

	pr_emerg("s5k4h7yx: start_streaming: writing %zu preview regs\n",
		 ARRAY_SIZE(s5k4h7_preview_regs));
	ret = s5k4h7yx_write_array(sensor->client, s5k4h7_preview_regs,
				   ARRAY_SIZE(s5k4h7_preview_regs));
	if (ret) {
		pr_emerg("s5k4h7yx: write preview regs failed: %d\n", ret);
		return ret;
	}

	pr_emerg("s5k4h7yx: start_streaming: setting MODE_STREAMING (0x0100 = 0x01)\n");
	ret = s5k4h7yx_write_reg(sensor->client, S5K4H7YX_REG_MODE_SELECT,
				 S5K4H7YX_MODE_STREAMING);
	if (ret)
		pr_emerg("s5k4h7yx: setting MODE_STREAMING failed: %d\n", ret);
	else
		pr_emerg("s5k4h7yx: MODE_STREAMING set successfully!\n");
	return ret;
}

static int s5k4h7yx_stop_streaming(struct s5k4h7yx *sensor)
{
	pr_emerg("s5k4h7yx: stop_streaming called\n");
	return s5k4h7yx_write_reg(sensor->client, S5K4H7YX_REG_MODE_SELECT,
				  S5K4H7YX_MODE_STANDBY);
}

static int s5k4h7yx_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct s5k4h7yx *sensor = to_s5k4h7yx(sd);
	int ret = 0;

	pr_emerg("s5k4h7yx: s_stream called: enable=%d current_streaming=%d\n",
		 enable, sensor->streaming);

	if (sensor->streaming == enable)
		return 0;

	if (enable) {
		ret = pm_runtime_resume_and_get(&sensor->client->dev);
		if (ret < 0) {
			pr_emerg("s5k4h7yx: pm_runtime_resume_and_get failed: %d\n", ret);
			return ret;
		}
		ret = s5k4h7yx_start_streaming(sensor);
		if (ret) {
			pr_emerg("s5k4h7yx: start_streaming failed: %d\n", ret);
			pm_runtime_put(&sensor->client->dev);
			return ret;
		}
	} else {
		s5k4h7yx_stop_streaming(sensor);
		pm_runtime_put(&sensor->client->dev);
	}

	sensor->streaming = enable;
	pr_emerg("s5k4h7yx: s_stream completed successfully (enable=%d)\n", enable);
	return ret;
}

static int s5k4h7yx_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	return 0;
}

static int s5k4h7yx_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index > 0 || fse->code != MEDIA_BUS_FMT_SGRBG10_1X10)
		return -EINVAL;

	fse->min_width = S5K4H7YX_DEFAULT_WIDTH;
	fse->max_width = S5K4H7YX_DEFAULT_WIDTH;
	fse->min_height = S5K4H7YX_DEFAULT_HEIGHT;
	fse->max_height = S5K4H7YX_DEFAULT_HEIGHT;
	return 0;
}

static int s5k4h7yx_get_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *fmt)
{
	struct s5k4h7yx *sensor = to_s5k4h7yx(sd);
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

static int s5k4h7yx_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *fmt)
{
	struct s5k4h7yx *sensor = to_s5k4h7yx(sd);
	struct v4l2_mbus_framefmt *format;

	if (fmt->pad != 0)
		return -EINVAL;

	fmt->format.width = S5K4H7YX_DEFAULT_WIDTH;
	fmt->format.height = S5K4H7YX_DEFAULT_HEIGHT;
	fmt->format.code = MEDIA_BUS_FMT_SGRBG10_1X10;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_SRGB;

	format = v4l2_subdev_state_get_format(sd_state, fmt->pad);
	if (format)
		*format = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		sensor->format = fmt->format;

	return 0;
}

static int s5k4h7yx_init_state(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *sd_state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.width = S5K4H7YX_DEFAULT_WIDTH,
			.height = S5K4H7YX_DEFAULT_HEIGHT,
			.code = MEDIA_BUS_FMT_SGRBG10_1X10,
			.field = V4L2_FIELD_NONE,
			.colorspace = V4L2_COLORSPACE_SRGB,
		},
	};

	return s5k4h7yx_set_fmt(sd, sd_state, &fmt);
}

static const struct v4l2_subdev_internal_ops s5k4h7yx_internal_ops = {
	.init_state = s5k4h7yx_init_state,
};

static const struct media_entity_operations s5k4h7yx_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_video_ops s5k4h7yx_video_ops = {
	.s_stream = s5k4h7yx_s_stream,
};

static const struct v4l2_subdev_pad_ops s5k4h7yx_pad_ops = {
	.enum_mbus_code = s5k4h7yx_enum_mbus_code,
	.enum_frame_size = s5k4h7yx_enum_frame_size,
	.get_fmt = s5k4h7yx_get_fmt,
	.set_fmt = s5k4h7yx_set_fmt,
};

static const struct v4l2_subdev_ops s5k4h7yx_subdev_ops = {
	.video = &s5k4h7yx_video_ops,
	.pad = &s5k4h7yx_pad_ops,
};

static const s64 link_freq_menu_items[] = {
	280000000,
};

static int s5k4h7yx_init_controls(struct s5k4h7yx *sensor)
{
	struct v4l2_ctrl_handler *hdl = &sensor->ctrl_handler;

	v4l2_ctrl_handler_init(hdl, 2);

	sensor->link_freq = v4l2_ctrl_new_int_menu(hdl, NULL,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(link_freq_menu_items) - 1,
						   0, link_freq_menu_items);
	if (sensor->link_freq)
		sensor->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->pixel_rate = v4l2_ctrl_new_std(hdl, NULL,
					       V4L2_CID_PIXEL_RATE,
					       280000000, 280000000, 1,
					       280000000);
	if (sensor->pixel_rate)
		sensor->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	sensor->sd.ctrl_handler = hdl;
	return hdl->error;
}

static int s5k4h7yx_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct s5k4h7yx *sensor;
	int ret;

	pr_emerg("s5k4h7yx: probe entry for client %s at addr 0x%02x\n",
		 dev_name(dev), client->addr);

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->client = client;

	pr_emerg("s5k4h7yx: v4l2_i2c_subdev_init\n");
	v4l2_i2c_subdev_init(&sensor->sd, client, &s5k4h7yx_subdev_ops);
	sensor->sd.fwnode = dev_fwnode(dev);
	sensor->sd.internal_ops = &s5k4h7yx_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.ops = &s5k4h7yx_subdev_entity_ops;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;

	pr_emerg("s5k4h7yx: getting clk/gpios\n");
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

	pr_emerg("s5k4h7yx: resources: mclk=%p (%ld Hz) reset=%p vcamd=%p vcama=%p vcamio=%p\n",
		 sensor->mclk, sensor->mclk ? clk_get_rate(sensor->mclk) : 0,
		 sensor->reset_gpio, sensor->vcamd_gpio, sensor->vcama_gpio, sensor->vcamio);

	pr_emerg("s5k4h7yx: powering on\n");
	ret = s5k4h7yx_power_on(sensor);
	if (ret)
		return ret;

	pr_emerg("s5k4h7yx: identifying sensor\n");
	ret = s5k4h7yx_identify(sensor);
	if (ret) {
		pr_emerg("s5k4h7yx: identify failed: %d\n", ret);
		if (!keep_power)
			s5k4h7yx_power_off(sensor);
		return ret;
	}

	pr_emerg("s5k4h7yx: initializing controls\n");
	ret = s5k4h7yx_init_controls(sensor);
	if (ret) {
		s5k4h7yx_power_off(sensor);
		return ret;
	}

	pr_emerg("s5k4h7yx: initializing media entity pads\n");
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		goto err_ctrl;

	sensor->sd.state_lock = sensor->ctrl_handler.lock;
	pr_emerg("s5k4h7yx: finalizing subdev\n");
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret) {
		pr_emerg("s5k4h7yx: finalize subdev failed: %d\n", ret);
		goto err_entity;
	}

	sensor->format.width = S5K4H7YX_DEFAULT_WIDTH;
	sensor->format.height = S5K4H7YX_DEFAULT_HEIGHT;
	sensor->format.code = MEDIA_BUS_FMT_SGRBG10_1X10;
	sensor->format.field = V4L2_FIELD_NONE;
	sensor->format.colorspace = V4L2_COLORSPACE_SRGB;

	pr_emerg("s5k4h7yx: setting pm runtime active and enable\n");
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	pr_emerg("s5k4h7yx: registering subdev with v4l2 async\n");
	ret = v4l2_async_register_subdev(&sensor->sd);
	if (ret) {
		pr_emerg("s5k4h7yx: async register subdev failed: %d\n", ret);
		goto err_pm;
	}

	pr_emerg("s5k4h7yx: probe completed successfully!\n");
	return 0;

err_pm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&sensor->sd);
err_entity:
	media_entity_cleanup(&sensor->sd.entity);
err_ctrl:
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
	s5k4h7yx_power_off(sensor);
	return ret;
}

static void s5k4h7yx_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k4h7yx *sensor = to_s5k4h7yx(sd);

	pr_emerg("s5k4h7yx: remove start\n");
	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		s5k4h7yx_power_off(sensor);
	pm_runtime_set_suspended(&client->dev);
	pr_emerg("s5k4h7yx: remove done\n");
}

static const struct of_device_id s5k4h7yx_of_match[] = {
	{ .compatible = "samsung,s5k4h7yx" },
	{ .compatible = "samsung,s5k4h7" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, s5k4h7yx_of_match);

static struct i2c_driver s5k4h7yx_driver = {
	.driver = {
		.name = "s5k4h7yx",
		.of_match_table = s5k4h7yx_of_match,
	},
	.probe = s5k4h7yx_probe,
	.remove = s5k4h7yx_remove,
};
module_i2c_driver(s5k4h7yx_driver);

MODULE_AUTHOR("helllopratik <pratikgondane07@gmail.com>");
MODULE_DESCRIPTION("Samsung S5K4H7YX camera sensor driver");
MODULE_LICENSE("GPL");
