// SPDX-License-Identifier: GPL-2.0
/*
 * pmos_sensor_bridge.c - expose MediaTek SCP sensorhub sensors through IIO
 *
 * The vendor sensor stack (scp.ko -> hf_manager.ko -> sensorhub.ko) exports
 * the hf_client_* API.  This module creates one hf_manager client, enables
 * accelerometer, gyroscope and ambient light sensors, and republishes the
 * cached samples as regular IIO devices so that iio-sensor-proxy (and
 * therefore Phosh auto-brightness / rotation) can consume them.
 *
 * Units:
 *   accel: raw = milli-m/s^2, scale = 0.001
 *   gyro:  raw = micro-rad/s,  scale = 0.000001
 *   light: raw = lux,          scale = 1
 */

#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "hf_manager.h"
#include "hf_sensor_io.h"
#include "hf_sensor_type.h"

#define PMOS_ACCEL_HZ		50
#define PMOS_GYRO_HZ		50
#define PMOS_LIGHT_HZ		5
#define PMOS_POLL_TIMEOUT_MS	500
#define PMOS_EVENT_BATCH	16

struct pmos_bridge {
	struct hf_client *client;
	struct task_struct *thread;

	struct iio_dev *accel_dev;
	struct iio_dev *gyro_dev;
	struct iio_dev *light_dev;

	struct mutex lock;	/* protects the cached samples below */
	int accel[3];
	int gyro[3];
	int lux;
	bool accel_valid;
	bool gyro_valid;
	bool light_valid;
};

struct pmos_iio_priv {
	struct pmos_bridge *bridge;
};

static struct pmos_bridge *pmos_bridge;

/* ------------------------------------------------------------------ */
/* IIO channel descriptions                                           */
/* ------------------------------------------------------------------ */

#define PMOS_ACCEL_CHANNEL(_axis)					\
	{								\
		.type = IIO_ACCEL,					\
		.modified = 1,						\
		.channel2 = IIO_MOD_##_axis,				\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
	}

static const struct iio_chan_spec pmos_accel_channels[] = {
	PMOS_ACCEL_CHANNEL(X),
	PMOS_ACCEL_CHANNEL(Y),
	PMOS_ACCEL_CHANNEL(Z),
};

#define PMOS_GYRO_CHANNEL(_axis)					\
	{								\
		.type = IIO_ANGL_VEL,					\
		.modified = 1,						\
		.channel2 = IIO_MOD_##_axis,				\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
	}

static const struct iio_chan_spec pmos_gyro_channels[] = {
	PMOS_GYRO_CHANNEL(X),
	PMOS_GYRO_CHANNEL(Y),
	PMOS_GYRO_CHANNEL(Z),
};

static const struct iio_chan_spec pmos_light_channels[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
	},
};

/* ------------------------------------------------------------------ */
/* hf_manager plumbing                                                */
/* ------------------------------------------------------------------ */

static int pmos_sensor_cmd(struct pmos_bridge *b, u8 type, u8 action,
			   s64 delay_ns)
{
	struct hf_manager_cmd cmd;
	struct hf_manager_batch batch;

	memset(&cmd, 0, sizeof(cmd));
	memset(&batch, 0, sizeof(batch));
	batch.delay = delay_ns;
	batch.latency = 0;

	cmd.sensor_type = type;
	cmd.action = action;
	cmd.length = 0;
	if (action == HF_MANAGER_SENSOR_ENABLE)
		memcpy(cmd.data, &batch, sizeof(batch));

	return hf_client_control_sensor(b->client, &cmd);
}

static int pmos_sensor_enable(struct pmos_bridge *b)
{
	int ret;

	ret = pmos_sensor_cmd(b, SENSOR_TYPE_ACCELEROMETER,
			      HF_MANAGER_SENSOR_ENABLE,
			      NSEC_PER_SEC / PMOS_ACCEL_HZ);
	if (ret) {
		pr_err("pmos_sensor_bridge: enable accel failed: %d\n", ret);
		return ret;
	}

	ret = pmos_sensor_cmd(b, SENSOR_TYPE_GYROSCOPE,
			      HF_MANAGER_SENSOR_ENABLE,
			      NSEC_PER_SEC / PMOS_GYRO_HZ);
	if (ret) {
		pr_err("pmos_sensor_bridge: enable gyro failed: %d\n", ret);
		goto disable_accel;
	}

	ret = pmos_sensor_cmd(b, SENSOR_TYPE_LIGHT,
			      HF_MANAGER_SENSOR_ENABLE,
			      NSEC_PER_SEC / PMOS_LIGHT_HZ);
	if (ret) {
		pr_err("pmos_sensor_bridge: enable light failed: %d\n", ret);
		goto disable_gyro;
	}

	return 0;

disable_gyro:
	pmos_sensor_cmd(b, SENSOR_TYPE_GYROSCOPE, HF_MANAGER_SENSOR_DISABLE, 0);
disable_accel:
	pmos_sensor_cmd(b, SENSOR_TYPE_ACCELEROMETER,
			HF_MANAGER_SENSOR_DISABLE, 0);
	return ret;
}

static void pmos_sensor_disable(struct pmos_bridge *b)
{
	pmos_sensor_cmd(b, SENSOR_TYPE_LIGHT, HF_MANAGER_SENSOR_DISABLE, 0);
	pmos_sensor_cmd(b, SENSOR_TYPE_GYROSCOPE, HF_MANAGER_SENSOR_DISABLE, 0);
	pmos_sensor_cmd(b, SENSOR_TYPE_ACCELEROMETER,
			HF_MANAGER_SENSOR_DISABLE, 0);
}

static void pmos_handle_event(struct pmos_bridge *b,
			      const struct hf_manager_event *ev)
{
	mutex_lock(&b->lock);
	switch (ev->sensor_type) {
	case SENSOR_TYPE_ACCELEROMETER:
		b->accel[0] = ev->word[0];
		b->accel[1] = ev->word[1];
		b->accel[2] = ev->word[2];
		b->accel_valid = true;
		break;
	case SENSOR_TYPE_GYROSCOPE:
		b->gyro[0] = ev->word[0];
		b->gyro[1] = ev->word[1];
		b->gyro[2] = ev->word[2];
		b->gyro_valid = true;
		break;
	case SENSOR_TYPE_LIGHT:
		b->lux = ev->word[0];
		b->light_valid = true;
		break;
	default:
		break;
	}
	mutex_unlock(&b->lock);
}

static int pmos_poll_thread(void *data)
{
	struct pmos_bridge *b = data;
	struct hf_manager_event events[PMOS_EVENT_BATCH];
	int i, n;

	while (!kthread_should_stop()) {
		n = hf_client_poll_sensor_timeout(b->client, events,
						  ARRAY_SIZE(events),
						  msecs_to_jiffies(PMOS_POLL_TIMEOUT_MS));
		if (n == -ETIMEDOUT)
			continue;
		if (n < 0) {
			if (kthread_should_stop())
				break;
			pr_warn_ratelimited("pmos_sensor_bridge: poll failed: %d\n",
					    n);
			msleep(100);
			continue;
		}

		for (i = 0; i < n; i++)
			pmos_handle_event(b, &events[i]);
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* IIO callbacks                                                      */
/* ------------------------------------------------------------------ */

static int pmos_read_raw(struct iio_dev *indio_dev,
			 struct iio_chan_spec const *chan,
			 int *val, int *val2, long mask)
{
	struct pmos_iio_priv *priv = iio_priv(indio_dev);
	struct pmos_bridge *b = priv->bridge;
	int axis, ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (chan->type == IIO_ACCEL)
			axis = chan->channel2 - IIO_MOD_X;
		else if (chan->type == IIO_ANGL_VEL)
			axis = chan->channel2 - IIO_MOD_X;
		else
			axis = 0;

		mutex_lock(&b->lock);
		switch (chan->type) {
		case IIO_ACCEL:
			*val = b->accel[axis];
			ret = b->accel_valid;
			break;
		case IIO_ANGL_VEL:
			*val = b->gyro[axis];
			ret = b->gyro_valid;
			break;
		case IIO_LIGHT:
			*val = b->lux;
			ret = b->light_valid;
			break;
		default:
			ret = 0;
			break;
		}
		mutex_unlock(&b->lock);

		return ret ? IIO_VAL_INT : -EAGAIN;

	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_ACCEL:
			/* milli-m/s^2 -> m/s^2 */
			*val = 0;
			*val2 = 1000;
			return IIO_VAL_INT_PLUS_MICRO;
		case IIO_ANGL_VEL:
			/* micro-rad/s -> rad/s */
			*val = 0;
			*val2 = 1;
			return IIO_VAL_INT_PLUS_MICRO;
		case IIO_LIGHT:
			*val = 1;
			*val2 = 0;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

static const struct iio_info pmos_iio_info = {
	.read_raw = pmos_read_raw,
};

/* ------------------------------------------------------------------ */
/* Module setup                                                       */
/* ------------------------------------------------------------------ */

static struct iio_dev *pmos_iio_register(struct pmos_bridge *b,
					 const char *name,
					 const struct iio_chan_spec *channels,
					 int num_channels)
{
	struct iio_dev *indio_dev;
	struct pmos_iio_priv *priv;
	int ret;

	indio_dev = iio_device_alloc(NULL, sizeof(*priv));
	if (!indio_dev)
		return ERR_PTR(-ENOMEM);

	priv = iio_priv(indio_dev);
	priv->bridge = b;

	indio_dev->name = name;
	indio_dev->info = &pmos_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = channels;
	indio_dev->num_channels = num_channels;

	ret = iio_device_register(indio_dev);
	if (ret) {
		pr_err("pmos_sensor_bridge: register %s failed: %d\n", name, ret);
		iio_device_free(indio_dev);
		return ERR_PTR(ret);
	}

	return indio_dev;
}

static int __init pmos_sensor_bridge_init(void)
{
	struct pmos_bridge *b;
	int ret;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	mutex_init(&b->lock);

	b->client = hf_client_create();
	if (!b->client) {
		pr_err("pmos_sensor_bridge: hf_client_create failed\n");
		ret = -ENOMEM;
		goto err_free;
	}

	if (hf_client_find_sensor(b->client, SENSOR_TYPE_ACCELEROMETER) ||
	    hf_client_find_sensor(b->client, SENSOR_TYPE_GYROSCOPE) ||
	    hf_client_find_sensor(b->client, SENSOR_TYPE_LIGHT)) {
		pr_err("pmos_sensor_bridge: required sensors missing\n");
		ret = -ENODEV;
		goto err_client;
	}

	b->accel_dev = pmos_iio_register(b, "pmos-accel", pmos_accel_channels,
					 ARRAY_SIZE(pmos_accel_channels));
	if (IS_ERR(b->accel_dev)) {
		ret = PTR_ERR(b->accel_dev);
		goto err_client;
	}

	b->gyro_dev = pmos_iio_register(b, "pmos-gyro", pmos_gyro_channels,
					ARRAY_SIZE(pmos_gyro_channels));
	if (IS_ERR(b->gyro_dev)) {
		ret = PTR_ERR(b->gyro_dev);
		goto err_accel;
	}

	b->light_dev = pmos_iio_register(b, "pmos-als", pmos_light_channels,
					 ARRAY_SIZE(pmos_light_channels));
	if (IS_ERR(b->light_dev)) {
		ret = PTR_ERR(b->light_dev);
		goto err_gyro;
	}

	ret = pmos_sensor_enable(b);
	if (ret)
		goto err_light;

	b->thread = kthread_run(pmos_poll_thread, b, "pmos-sensors");
	if (IS_ERR(b->thread)) {
		ret = PTR_ERR(b->thread);
		b->thread = NULL;
		pr_err("pmos_sensor_bridge: kthread failed: %d\n", ret);
		goto err_disable;
	}

	pmos_bridge = b;
	pr_info("pmos_sensor_bridge: sensors exposed via IIO\n");
	return 0;

err_disable:
	pmos_sensor_disable(b);
err_light:
	iio_device_unregister(b->light_dev);
	iio_device_free(b->light_dev);
err_gyro:
	iio_device_unregister(b->gyro_dev);
	iio_device_free(b->gyro_dev);
err_accel:
	iio_device_unregister(b->accel_dev);
	iio_device_free(b->accel_dev);
err_client:
	hf_client_destroy(b->client);
err_free:
	kfree(b);
	return ret;
}

static void __exit pmos_sensor_bridge_exit(void)
{
	struct pmos_bridge *b = pmos_bridge;

	if (!b)
		return;

	pmos_bridge = NULL;

	if (b->thread)
		kthread_stop(b->thread);
	pmos_sensor_disable(b);
	iio_device_unregister(b->light_dev);
	iio_device_free(b->light_dev);
	iio_device_unregister(b->gyro_dev);
	iio_device_free(b->gyro_dev);
	iio_device_unregister(b->accel_dev);
	iio_device_free(b->accel_dev);
	hf_client_destroy(b->client);
	kfree(b);
}

module_init(pmos_sensor_bridge_init);
module_exit(pmos_sensor_bridge_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Expose MediaTek SCP sensorhub sensors through IIO");
MODULE_AUTHOR("postmarketOS");
