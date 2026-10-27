/*
 * Copyright (C) 2010 - 2022 Novatek, Inc.
 *
 * $Revision: 108741 $
 * $Date: 2022-11-21 10:31:27 +0800 (週一, 21 十一月 2022) $
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/arm-smccc.h>
#include <linux/reboot.h>
#include <linux/panic_notifier.h>
#include <linux/kmsg_dump.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/vmalloc.h>
#include <linux/utsname.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/proc_fs.h>
#include <linux/input/mt.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include "nt36xxx.h"
#include <linux/platform_data/spi-mt65xx.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/gpio/consumer.h>

#if IS_ENABLED(NVT_DRM_PANEL_NOTIFY)
#include <drm/drm_panel.h>
#elif IS_ENABLED(NVT_MSM_DRM_NOTIFY)
#include <linux/msm_drm_notify.h>
#elif IS_ENABLED(NVT_FB_NOTIFY)
#include <linux/notifier.h>
#include <linux/fb.h>
#elif IS_ENABLED(NVT_EARLYSUSPEND_NOTIFY)
#include <linux/earlysuspend.h>
#elif IS_ENABLED(NVT_QCOM_PANEL_EVENT_NOTIFY)
#include <linux/soc/qcom/panel_event_notifier.h>
#elif IS_ENABLED(NVT_MTK_DRM_NOTIFY)
#include "mtk_panel_ext.h"
#include "mtk_disp_notify.h"
#endif

#if IS_ENABLED(CONFIG_TINNO_DEVINFO)
#include "../dev_info/dev_info.h"
#endif
//#if NVT_TOUCH_ESD_PROTECT
#include <linux/jiffies.h>
//#endif /* #if NVT_TOUCH_ESD_PROTECT */
#include <linux/string.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/bitops.h>
#include <linux/stdarg.h>

#define PENRAW_DRIVER_NAME "lenovo_penraw_driver"
#define PENRAW_DEVICE_NAME "lenovo_penraw"

static const unsigned int MINOR_NUMBER_START = 0; /* the minor number starts at */
static const unsigned int NUMBER_MINOR_NUMBER = 1; /* the number of minor numbers */
static unsigned int major_number; /* the major number of the device */
/* ioctl for direct access */
static struct cdev penraw_char_dev; /* character device */
static struct class* penraw_char_dev_class = NULL; /* class object */
static struct lenovo_pen_info pen_buffer[LENOVO_MAX_BUFFER];
static unsigned char pen_report_num;
static unsigned char pen_frame_no;
static unsigned char pen_buffer_wp;

char *boot_update_firmware_name = NULL;
EXPORT_SYMBOL_GPL(boot_update_firmware_name);
char *mp_update_firmware_name = NULL;
EXPORT_SYMBOL_GPL(mp_update_firmware_name);

void (*nvt_lcd_esd_resume)(bool) = NULL;
EXPORT_SYMBOL_GPL(nvt_lcd_esd_resume);
void nvt_esd_resume(bool status);

#if NVT_TOUCH_ESD_PROTECT
static struct delayed_work nvt_esd_check_work;
static struct workqueue_struct *nvt_esd_check_wq;
static unsigned long irq_timer = 0;
uint8_t esd_check = false;
uint8_t esd_retry = 0;
#endif /* #if NVT_TOUCH_ESD_PROTECT */

#if NVT_TOUCH_EXT_PROC
extern int32_t nvt_extra_proc_init(void);
extern void nvt_extra_proc_deinit(void);
#endif

#if NVT_TOUCH_MP
extern int32_t nvt_mp_proc_init(void);
extern void nvt_mp_proc_deinit(void);
#endif

#if NVT_CUST_PROC_CMD
#if NVT_EDGE_REJECT
extern int32_t nvt_edge_reject_set(int32_t status);
#endif
#if NVT_EDGE_GRID_ZONE
extern int32_t nvt_edge_grid_zone_set(uint8_t deg, uint8_t dir, uint16_t y1, uint16_t y2);
#endif
#if NVT_PALM_MODE
extern int32_t nvt_game_mode_set(uint8_t status);
#endif
#if NVT_SUPPORT_PEN
extern int32_t nvt_support_pen_set(uint8_t status);
#endif
struct work_struct ts_restore_cmd_work;
#endif

struct nvt_ts_data *ts;
struct workqueue_struct *nvt_fwu_wq;

/* Bring-up knobs: allow forcing continuous event-buffer polling and an
 * experimental "continuous scan" kick, so the digitizer can produce frames
 * even when the DSI clock is idle (simpledrm / no V-Blank case). */
static int nvt_poll_ms = 16;
module_param(nvt_poll_ms, int, 0644);
MODULE_PARM_DESC(nvt_poll_ms, "Event-buffer polling interval in ms (0 = disable polling kthread)");

static int nvt_poll_report = 1;
	module_param(nvt_poll_report, int, 0644);
MODULE_PARM_DESC(nvt_poll_report, "Report touch input events from the poll kthread (1=on)");

static int nvt_poll_exclusive = 1;
	module_param(nvt_poll_exclusive, int, 0644);
MODULE_PARM_DESC(nvt_poll_exclusive,
		 "Make the poll kthread the single consumer of the touch event "
		 "buffer; the IRQ threaded handler returns without reading/reporting "
		 "(1=on). Fixes one-frame lag from IRQ+poll double-read race.");

static unsigned int nvt_poll_base = 0;
module_param(nvt_poll_base, uint, 0644);
MODULE_PARM_DESC(nvt_poll_base, "Poll/read event-buffer base address (0 = default EVENT_BUF_ADDR)");

static unsigned int nvt_diag_len = POINT_DATA_LEN;
module_param(nvt_diag_len, uint, 0644);
MODULE_PARM_DESC(nvt_diag_len, "Byte length for /proc/nvt_frame (<= POINT_DATA_LEN)");

static int nvt_asyn_scan = 0;
module_param(nvt_asyn_scan, int, 0644);
MODULE_PARM_DESC(nvt_asyn_scan, "Attempt experimental NT36523 continuous-scan wake sequence at probe (0 = disable)");

extern int panic_timeout;
extern int panic_on_oops;
static int nvt_auto_reboot = 0;
module_param(nvt_auto_reboot, int, 0644);
MODULE_PARM_DESC(nvt_auto_reboot, "Convert kernel oops to panic + 30s force reboot for crash capture (1=on)");

static bool nvt_panic_reboot_done;
static int nvt_panic_handler(struct notifier_block *nb, unsigned long ev, void *unused)
{
	int i;

	if (nvt_panic_reboot_done)
		return NOTIFY_DONE;
	nvt_panic_reboot_done = true;

	NVT_ERR("PANICDUMP: forcing reboot in 30s\n");
	for (i = 0; i < 30; i++)
		mdelay(1000);
	NVT_ERR("PANICDUMP: rebooting now\n");

	for (;;) {
		struct arm_smccc_res res;

		emergency_restart();
		arm_smccc_smc(0x84000009, 0, 0, 0, 0, 0, 0, 0, &res);
		arm_smccc_hvc(0x84000009, 0, 0, 0, 0, 0, 0, 0, &res);
		mdelay(1000);
	}
	return NOTIFY_DONE;
}

static struct notifier_block nvt_panic_nb = {
	.notifier_call = nvt_panic_handler,
	.priority = 0,
};

/*
 * Crash-log capture: on OOPS/PANIC dump the full kernel log to a file on
 * the built-in storage, so it survives the reboot and is pullable from
 * Android.  pmOS mounts the Android "sdcard" area (userdata) at /, so:
 *   "/media/0/nvt_crash.log"  -> Android /sdcard/nvt_crash.log
 *   "/var/log/nvt_crash.log"  -> Android /data/var/log/nvt_crash.log (root)
 * The file is opened lazily once the rootfs is up (deferred work), and the
 * kmsg_dumper writes to it at panic/oops time (VFS path that works on this
 * redacted GKI, which has no raw blkdev APIs).
 */
#define NVT_CRASH_FILE_MAX (256 * 1024)
#define NVT_CRASH_RING_MAX (16 * 1024)

static struct file *nvt_crash_file;
static char *nvt_crash_buf;
static char *nvt_crash_ring;
static size_t nvt_crash_ring_w, nvt_crash_ring_f;
static struct mutex nvt_crash_mtx;
static struct delayed_work nvt_crash_open_work;

static void nvt_crash_close(void)
{
	if (nvt_crash_file) {
		filp_close(nvt_crash_file, NULL);
		nvt_crash_file = NULL;
	}
}

/* ---- pre-open ring: preserves trace lines even before the fs is up ---- */

static void nvt_crash_ring_put(const void *data, size_t len)
{
	const char *p = (const char *)data;
	size_t have, free_, drop;

	if (!nvt_crash_ring || !len)
		return;
	if (len >= NVT_CRASH_RING_MAX) {
		nvt_crash_ring_w = nvt_crash_ring_f = 0;
		p += len - NVT_CRASH_RING_MAX;
		len = NVT_CRASH_RING_MAX;
	}
	have = (nvt_crash_ring_w >= nvt_crash_ring_f) ?
	       (nvt_crash_ring_w - nvt_crash_ring_f) :
	       (NVT_CRASH_RING_MAX - nvt_crash_ring_f + nvt_crash_ring_w);
	free_ = NVT_CRASH_RING_MAX - have;
	drop = (len > free_) ? (len - free_) : 0;
	if (drop) {
		nvt_crash_ring_f += drop;
		nvt_crash_ring_f %= NVT_CRASH_RING_MAX;
	}
	while (len) {
		size_t chunk = min(len, NVT_CRASH_RING_MAX - nvt_crash_ring_w);

		memcpy(nvt_crash_ring + nvt_crash_ring_w, p, chunk);
		nvt_crash_ring_w += chunk;
		nvt_crash_ring_w %= NVT_CRASH_RING_MAX;
		p += chunk;
		len -= chunk;
	}
}

static void nvt_crash_write_locked(const void *data, size_t len)
{
	loff_t pos = 0;
	size_t n;
	const char *p = (const char *)data;

	while (len) {
		n = min(len, (size_t)4096);
		if (kernel_write(nvt_crash_file, p, n, &pos) != (ssize_t)n)
			break;
		p += n;
		len -= n;
	}
}

static void nvt_crash_flush_ring_locked(void)
{
	if (!nvt_crash_file || !nvt_crash_ring)
		return;
	if (nvt_crash_ring_w >= nvt_crash_ring_f) {
		nvt_crash_write_locked(nvt_crash_ring + nvt_crash_ring_f,
					nvt_crash_ring_w - nvt_crash_ring_f);
	} else {
		nvt_crash_write_locked(nvt_crash_ring + nvt_crash_ring_f,
					NVT_CRASH_RING_MAX - nvt_crash_ring_f);
		nvt_crash_write_locked(nvt_crash_ring, nvt_crash_ring_w);
	}
	nvt_crash_ring_f = nvt_crash_ring_w = 0;
	vfs_fsync(nvt_crash_file, 0);
}

static int nvt_crash_open(void)
{
	const char *paths[] = { "/nvt_crash.log",
				"/var/log/nvt_crash.log",
				"/run/nvt_crash.log",
				"/media/0/nvt_crash.log",
				"/data/nvt_crash.log", NULL };
	int i;

	for (i = 0; paths[i]; i++) {
		nvt_crash_file = filp_open(paths[i],
					   O_WRONLY | O_CREAT | O_APPEND,
					   0644);
		if (!IS_ERR(nvt_crash_file)) {
			NVT_LOG("crashlog: using %s\n", paths[i]);
			return 0;
		}
		pr_warn_ratelimited("NVT crashlog: open %s failed (%ld)\n",
				    paths[i],
				    PTR_ERR(nvt_crash_file));
		nvt_crash_file = NULL;
	}
	return -1;
}

static void nvt_crash_open_worker(struct work_struct *work)
{
	mutex_lock(&nvt_crash_mtx);
	nvt_crash_close();
	if (nvt_crash_open() == 0)
		nvt_crash_flush_ring_locked();
	mutex_unlock(&nvt_crash_mtx);
}

static void nvt_crash_dump(struct kmsg_dumper *d,
			   struct kmsg_dump_detail *detail);
static struct kmsg_dumper nvt_kmsg_dumper = {
	.dump = nvt_crash_dump,
	.max_reason = KMSG_DUMP_EMERG,
};

static int nvt_crashlog_init(void)
{
	nvt_crash_buf = kvmalloc(NVT_CRASH_FILE_MAX, GFP_KERNEL);
	nvt_crash_ring = kvmalloc(NVT_CRASH_RING_MAX, GFP_KERNEL);
	if (!nvt_crash_buf || !nvt_crash_ring) {
		NVT_ERR("crashlog: alloc failed\n");
		return -ENOMEM;
	}
	mutex_init(&nvt_crash_mtx);
	INIT_DELAYED_WORK(&nvt_crash_open_work, nvt_crash_open_worker);
	schedule_delayed_work(&nvt_crash_open_work, 5 * HZ);
	kmsg_dump_register(&nvt_kmsg_dumper);
	NVT_LOG("crashlog armed (ring %d KB)\n", NVT_CRASH_RING_MAX / 1024);
	return 0;
}

static void nvt_crash_dump(struct kmsg_dumper *d,
			   struct kmsg_dump_detail *detail)
{
	struct kmsg_dump_iter iter;
	size_t len = 0;
	loff_t pos = 0;
	int ret;

	if (!nvt_crash_buf)
		return;
	if (!nvt_crash_file && nvt_crash_open() != 0)
		return;
	kmsg_dump_rewind(&iter);
	if (!kmsg_dump_get_buffer(&iter, true, nvt_crash_buf,
				  NVT_CRASH_FILE_MAX, &len))
		return;
	ret = kernel_write(nvt_crash_file,
			   "\n==== NVT KMSG CAPTURE ====\n", 27, &pos);
	if (ret >= 0)
		kernel_write(nvt_crash_file, nvt_crash_buf, len, &pos);
	ret = vfs_fsync(nvt_crash_file, 0);
	if (ret)
		NVT_ERR("crashlog: fsync failed %d\n", ret);
	NVT_ERR("crashlog: stored %zu bytes (%s)\n", len,
		kmsg_dump_reason_str(detail->reason));
}

static void nvt_crashlog_shutdown(void)
{
	kmsg_dump_unregister(&nvt_kmsg_dumper);
	cancel_delayed_work_sync(&nvt_crash_open_work);
	mutex_lock(&nvt_crash_mtx);
	nvt_crash_flush_ring_locked();
	nvt_crash_close();
	mutex_unlock(&nvt_crash_mtx);
	if (nvt_crash_buf) {
		kvfree(nvt_crash_buf);
		nvt_crash_buf = NULL;
	}
	if (nvt_crash_ring) {
		kvfree(nvt_crash_ring);
		nvt_crash_ring = NULL;
	}
}

/* Always-on tracer: append a line to the crash capture file, buffering into a
 * pre-open ring while the rootfs is down.  Used by the poll kthread for
 * periodic diag and by the fw update path for download milestones, so even a
 * graceful failure leaves a trail on the storage. */
void nvt_crash_append(const char *fmt, ...)
{
	char buf[256];
	va_list args;
	int n;

	if (!nvt_crash_ring)
		return;
	va_start(args, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	if (n <= 0)
		return;
	n = min_t(int, n, sizeof(buf) - 1);
	mutex_lock(&nvt_crash_mtx);
	if (nvt_crash_file) {
		nvt_crash_write_locked(buf, (size_t)n);
		vfs_fsync(nvt_crash_file, 0);
	} else if (nvt_crash_open() == 0) {
		nvt_crash_flush_ring_locked();
		nvt_crash_write_locked(buf, (size_t)n);
	} else {
		nvt_crash_ring_put(buf, (size_t)n);
	}
	mutex_unlock(&nvt_crash_mtx);
}

static unsigned long nvt_crash_last_tick;

int nvt_panic_on_fail = 0;
module_param(nvt_panic_on_fail, int, 0644);
MODULE_PARM_DESC(nvt_panic_on_fail, "panic() after a failed firmware download (1=on)");

static void nvt_reg_rd(u32 addr, u8 *out, int len);
static u32 nvt_reg24(u32 addr);
static u32 nvt_reg_rd_as_u8(u32 addr);
void nvt_dump_and_panic(const char *why)
{
	uint8_t rv[6] = {0};
	u32 base = nvt_poll_base ? nvt_poll_base :
		   (ts->mmap ? ts->mmap->EVENT_BUF_ADDR : 0x2FD00);

	nvt_reg_rd(base + 0x60, rv, 1);
	NVT_ERR("PANICDUMP: [%s] reset=0x%02X\n", why, rv[0]);
	if (ts->mmap) {
		NVT_ERR("PANICDUMP: ilmd=0x%06X dlmd=0x%06X dma=0x%02X/%02X/%02X\n",
			nvt_reg24(ts->mmap->ILM_LENGTH_ADDR), nvt_reg24(ts->mmap->DLM_LENGTH_ADDR),
			nvt_reg_rd_as_u8(0x3F7D0), nvt_reg_rd_as_u8(0x3F7F1),
			nvt_reg_rd_as_u8(0x3F7E8));
	}
	panic("%s", why);
}

#if BOOT_UPDATE_FIRMWARE
extern void Boot_Update_Firmware(struct work_struct *work);
#endif

#if IS_ENABLED(NVT_DRM_PANEL_NOTIFY)
static struct drm_panel *active_panel;
static int nvt_drm_panel_notifier_callback(struct notifier_block *self, unsigned long event, void *data);
#elif IS_ENABLED(NVT_MSM_DRM_NOTIFY)
static int nvt_drm_notifier_callback(struct notifier_block *self, unsigned long event, void *data);
#elif IS_ENABLED(NVT_FB_NOTIFY)
static int nvt_fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data);
#elif IS_ENABLED(NVT_EARLYSUSPEND_NOTIFY)
static void nvt_ts_early_suspend(struct early_suspend *h);
static void nvt_ts_late_resume(struct early_suspend *h);
#elif IS_ENABLED(NVT_QCOM_PANEL_EVENT_NOTIFY)
static struct drm_panel *active_panel;
static void *notifier_cookie;
static void nvt_panel_notifier_callback(enum panel_event_notifier_tag tag,
		struct panel_event_notification *event, void *client_data);
#elif IS_ENABLED(NVT_MTK_DRM_NOTIFY)
static int nvt_mtk_drm_notifier_callback(struct notifier_block *nb,
		unsigned long event, void *data);
#endif

uint32_t ENG_RST_ADDR  = 0x7FFF80;
uint32_t SPI_RD_FAST_ADDR = 0;	//read from dtsi

#if TOUCH_KEY_NUM > 0
const uint16_t touch_key_array[TOUCH_KEY_NUM] = {
	KEY_BACK,
	KEY_HOME,
	KEY_MENU
};
#endif

#if WAKEUP_GESTURE
const uint16_t gesture_key_array[] = {
	KEY_POWER,  //GESTURE_WORD_C
	KEY_POWER,  //GESTURE_WORD_W
	KEY_POWER,  //GESTURE_WORD_V
	KEY_POWER,  //GESTURE_DOUBLE_CLICK
	KEY_POWER,  //GESTURE_WORD_Z
	KEY_POWER,  //GESTURE_WORD_M
	KEY_POWER,  //GESTURE_WORD_O
	KEY_POWER,  //GESTURE_WORD_e
	KEY_POWER,  //GESTURE_WORD_S
	KEY_POWER,  //GESTURE_SLIDE_UP
	KEY_POWER,  //GESTURE_SLIDE_DOWN
	KEY_POWER,  //GESTURE_SLIDE_LEFT
	KEY_POWER,  //GESTURE_SLIDE_RIGHT
};
#endif

#ifdef CONFIG_MTK_SPI
const struct mt_chip_conf spi_ctrdata = {
	.setuptime = 25,
	.holdtime = 25,
	.high_time = 5,	/* 10MHz (SPI_SPEED=100M / (high_time+low_time(10ns)))*/
	.low_time = 5,
	.cs_idletime = 2,
	.ulthgh_thrsh = 0,
	.cpol = 0,
	.cpha = 0,
	.rx_mlsb = 1,
	.tx_mlsb = 1,
	.tx_endian = 0,
	.rx_endian = 0,
	.com_mod = DMA_TRANSFER,
	.pause = 0,
	.finish_intr = 1,
	.deassert = 0,
	.ulthigh = 0,
	.tckdly = 0,
};
#endif

#ifdef CONFIG_SPI_MT65XX
const struct mtk_chip_config spi_ctrdata = {
    .sample_sel = 0,
};
#endif

static uint8_t bTouchIsAwake = 1;

static int double_click_wakeup = 0;
void nvt_gesture_enable(int enable)
{
	if(enable) {
		double_click_wakeup = 1;
		NVT_LOG("enable=%d, gesture enable !\n", enable);
	} else {
		double_click_wakeup = 0;
		NVT_LOG("enable=%d, gesture disable !\n", enable);
	}
}
EXPORT_SYMBOL_GPL(nvt_gesture_enable);
int double_click_wakeup_support(void)
{
	NVT_LOG("lcm control !\n");
	return double_click_wakeup;
}
EXPORT_SYMBOL_GPL(double_click_wakeup_support);
/*******************************************************
Description:
	Novatek touchscreen irq enable/disable function.

return:
	n.a.
*******************************************************/
static void nvt_irq_enable(bool enable)
{
	struct irq_desc *desc;

	if (enable) {
		if (!ts->irq_enabled) {
			enable_irq(ts->client->irq);
			ts->irq_enabled = true;
		}
	} else {
		if (ts->irq_enabled) {
			disable_irq(ts->client->irq);
			ts->irq_enabled = false;
		}
	}

	desc = irq_to_desc(ts->client->irq);
	NVT_LOG("enable=%d, desc->depth=%d\n", enable, desc->depth);
}

/*******************************************************
Description:
	Novatek touchscreen spi read/write core function.

return:
	Executive outcomes. 0---succeed.
*******************************************************/
static inline int32_t spi_read_write(struct spi_device *client, uint8_t *buf, size_t len , NVT_SPI_RW rw)
{
	struct spi_message m;
	struct spi_transfer t = {
		.len    = len,
	};

	memset(ts->xbuf, 0, len + DUMMY_BYTES);
	memcpy(ts->xbuf, buf, len);

	switch (rw) {
		case NVTREAD:
			t.tx_buf = ts->xbuf;
			t.rx_buf = ts->rbuf;
			t.len    = (len + DUMMY_BYTES);
			break;

		case NVTWRITE:
			t.tx_buf = ts->xbuf;
			t.rx_buf = ts->rbuf;
			break;
	}

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	return spi_sync(client, &m);
}

/*******************************************************
Description:
	Novatek touchscreen spi read function.

return:
	Executive outcomes. 2---succeed. -5---I/O error
*******************************************************/
int32_t CTP_SPI_READ(struct spi_device *client, uint8_t *buf, uint16_t len)
{
	int32_t ret = -1;
	int32_t retries = 0;

	mutex_lock(&ts->xbuf_lock);

	buf[0] = SPI_READ_MASK(buf[0]);

	while (retries < 5) {
		ret = spi_read_write(client, buf, len, NVTREAD);
		if (ret == 0) break;
		retries++;
	}

	if (unlikely(retries == 5)) {
		NVT_ERR("read error, ret=%d\n", ret);
		ret = -EIO;
	} else {
		memcpy((buf+1), (ts->rbuf+2), (len-1));
	}

	mutex_unlock(&ts->xbuf_lock);

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen spi write function.

return:
	Executive outcomes. 1---succeed. -5---I/O error
*******************************************************/
int32_t CTP_SPI_WRITE(struct spi_device *client, uint8_t *buf, uint16_t len)
{
	int32_t ret = -1;
	int32_t retries = 0;

	mutex_lock(&ts->xbuf_lock);

	buf[0] = SPI_WRITE_MASK(buf[0]);

	while (retries < 5) {
		ret = spi_read_write(client, buf, len, NVTWRITE);
		if (ret == 0)	break;
		retries++;
	}

	if (unlikely(retries == 5)) {
		NVT_ERR("error, ret=%d\n", ret);
		ret = -EIO;
	}

	mutex_unlock(&ts->xbuf_lock);

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen set index/page/addr address.

return:
	Executive outcomes. 0---succeed. -5---access fail.
*******************************************************/
int32_t nvt_set_page(uint32_t addr)
{
	uint8_t buf[4] = {0};

	buf[0] = 0xFF;	//set index/page/addr command
	buf[1] = (addr >> 15) & 0xFF;
	buf[2] = (addr >> 7) & 0xFF;

	return CTP_SPI_WRITE(ts->client, buf, 3);
}

/*******************************************************
Description:
	Novatek touchscreen write data to specify address.

return:
	Executive outcomes. 0---succeed. -5---access fail.
*******************************************************/
int32_t nvt_write_addr(uint32_t addr, uint8_t data)
{
	int32_t ret = 0;
	uint8_t buf[4] = {0};

	//---set xdata index---
	buf[0] = 0xFF;	//set index/page/addr command
	buf[1] = (addr >> 15) & 0xFF;
	buf[2] = (addr >> 7) & 0xFF;
	ret = CTP_SPI_WRITE(ts->client, buf, 3);
	if (ret) {
		NVT_ERR("set page 0x%06X failed, ret = %d\n", addr, ret);
		return ret;
	}

	//---write data to index---
	buf[0] = addr & (0x7F);
	buf[1] = data;
	ret = CTP_SPI_WRITE(ts->client, buf, 2);
	if (ret) {
		NVT_ERR("write data to 0x%06X failed, ret = %d\n", addr, ret);
		return ret;
	}

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen read value to specific register.

return:
	Executive outcomes. 0---succeed. -5---access fail.
*******************************************************/
int32_t nvt_read_reg(nvt_ts_reg_t reg, uint8_t *val)
{
	int32_t ret = 0;
	uint32_t addr = 0;
	uint8_t mask = 0;
	uint8_t shift = 0;
	uint8_t buf[8] = {0};
	uint8_t temp = 0;

	addr = reg.addr;
	mask = reg.mask;
	/* get shift */
	temp = reg.mask;
	shift = 0;
	while (1) {
		if ((temp >> shift) & 0x01)
			break;
		if (shift == 8) {
			NVT_ERR("mask all bits zero!\n");
			ret = -1;
			break;
		}
		shift++;
	}
	/* read the byte of the register is in */
	nvt_set_page(addr);
	buf[0] = addr & 0xFF;
	buf[1] = 0x00;
	ret = CTP_SPI_READ(ts->client, buf, 2);
	if (ret < 0) {
		NVT_ERR("CTP_SPI_READ failed!(%d)\n", ret);
		goto nvt_read_register_exit;
	}
	/* get register's value in its field of the byte */
	*val = (buf[1] & mask) >> shift;

nvt_read_register_exit:
	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen enable hw bld crc function.
This function will enable the bootloader CRC self-check so
that the MCU boots the just-downloaded firmware.

return:
	N/A.
*******************************************************/
void nvt_bld_crc_enable(void)
{
	uint8_t buf[2] = {0};

	//---set xdata index to BLD_CRC_EN_ADDR---
	nvt_set_page(ts->mmap->BLD_CRC_EN_ADDR);

	//---read data from index---
	buf[0] = ts->mmap->BLD_CRC_EN_ADDR & (0x7F);
	buf[1] = 0xFF;
	CTP_SPI_READ(ts->client, buf, 2);

	//---write data to index---
	buf[0] = ts->mmap->BLD_CRC_EN_ADDR & (0x7F);
	buf[1] = buf[1] | (0x01 << 7);
	CTP_SPI_WRITE(ts->client, buf, 2);
}

/*******************************************************
Description:
	Novatek touchscreen clear status & enable fw crc function.

return:
	N/A.
*******************************************************/
void nvt_fw_crc_enable(void)
{
	uint8_t buf[8] = {0};

	//---set xdata index to EVENT BUF ADDR---
	nvt_set_page(ts->mmap->EVENT_BUF_ADDR);

	//---clear fw reset status---
	buf[0] = SPI_WRITE_MASK(EVENT_MAP_RESET_COMPLETE & 0x7F);
	buf[1] = 0x00;
	buf[2] = 0x00;
	buf[3] = 0x00;
	buf[4] = 0x00;
	buf[5] = 0x00;
	buf[6] = 0x00;
	CTP_SPI_WRITE(ts->client, buf, 7);

	//---enable fw crc---
	buf[0] = SPI_WRITE_MASK(EVENT_MAP_HOST_CMD & 0x7F);
	buf[1] = 0xAE;	//enable fw crc command
	buf[2] = 0x00;
	CTP_SPI_WRITE(ts->client, buf, 3);
}

/*******************************************************
Description:
	Novatek touchscreen set boot ready function.

return:
	N/A.
*******************************************************/
void nvt_boot_ready(void)
{
	//---write BOOT_RDY status cmds---
	nvt_write_addr(ts->mmap->BOOT_RDY_ADDR, 1);

	mdelay(5);

	if (ts->hw_crc == HWCRC_NOSUPPORT) {
		//---write BOOT_RDY status cmds---
		nvt_write_addr(ts->mmap->BOOT_RDY_ADDR, 0);

		//---write POR_CD cmds---
		nvt_write_addr(ts->mmap->POR_CD_ADDR, 0xA0);
	}
}

/*******************************************************
Description:
	Novatek touchscreen enable auto copy mode function.

return:
	N/A.
*******************************************************/
void nvt_tx_auto_copy_mode(void)
{
	if (ts->auto_copy == CHECK_SPI_DMA_TX_INFO) {
		//---write TX_AUTO_COPY_EN cmds---
		nvt_write_addr(ts->mmap->TX_AUTO_COPY_EN, 0x69);
	} else if (ts->auto_copy == CHECK_TX_AUTO_COPY_EN) {
		//---write SPI_MST_AUTO_COPY cmds---
		nvt_write_addr(ts->mmap->TX_AUTO_COPY_EN, 0x56);
	}

	NVT_ERR("tx auto copy mode %d enable\n", ts->auto_copy);
}

/*******************************************************
Description:
	Novatek touchscreen check spi dma tx info function.

return:
	Executive outcomes. 0---succeed. -1---fail.
*******************************************************/
int32_t nvt_check_spi_dma_tx_info(void)
{
	uint8_t buf[8] = {0};
	int32_t i = 0;
	const int32_t retry = 2000;

	if (ts->mmap->SPI_DMA_TX_INFO == 0) {
		NVT_ERR("error, SPI_DMA_TX_INFO = 0\n");
		return -1;
	}

	for (i = 0; i < retry; i++) {
		//---set xdata index to SPI_DMA_TX_INFO---
		nvt_set_page(ts->mmap->SPI_DMA_TX_INFO);

		//---read spi dma status---
		buf[0] = ts->mmap->SPI_DMA_TX_INFO & 0x7F;
		buf[1] = 0xFF;
		CTP_SPI_READ(ts->client, buf, 2);

		if (buf[1] == 0x00)
			break;

		usleep_range(1000, 1000);
	}

	if (i >= retry) {
		NVT_ERR("failed, i=%d, buf[1]=0x%02X\n", i, buf[1]);
		return -1;
	} else {
		return 0;
	}
}

/*******************************************************
Description:
	Novatek touchscreen check tx auto copy state function.

return:
	Executive outcomes. 0---succeed. -1---fail.
*******************************************************/
int32_t nvt_check_tx_auto_copy(void)
{
	uint8_t buf[8] = {0};
	int32_t i = 0;
	const int32_t retry = 2000;

	if (ts->mmap->TX_AUTO_COPY_EN == 0) {
		NVT_ERR("error, TX_AUTO_COPY_EN = 0\n");
		return -1;
	}

	for (i = 0; i < retry; i++) {
		//---set xdata index to SPI_MST_AUTO_COPY---
		nvt_set_page(ts->mmap->TX_AUTO_COPY_EN);

		//---read auto copy status---
		buf[0] = ts->mmap->TX_AUTO_COPY_EN & 0x7F;
		buf[1] = 0xFF;
		CTP_SPI_READ(ts->client, buf, 2);

		if (buf[1] == 0x00)
			break;

		usleep_range(1000, 1000);
	}

	if (i >= retry) {
		NVT_ERR("failed, i=%d, buf[1]=0x%02X\n", i, buf[1]);
		return -1;
	} else {
		return 0;
	}
}

/*******************************************************
Description:
	Novatek touchscreen wait auto copy finished function.

return:
	Executive outcomes. 0---succeed. -1---fail.
*******************************************************/
int32_t nvt_wait_auto_copy(void)
{
	if (ts->auto_copy == CHECK_SPI_DMA_TX_INFO) {
		return nvt_check_spi_dma_tx_info();
	} else if (ts->auto_copy == CHECK_TX_AUTO_COPY_EN) {
		return nvt_check_tx_auto_copy();
	} else {
		NVT_ERR("failed, not support mode %d!\n", ts->auto_copy);
		return -1;
	}
}

/*******************************************************
Description:
	Novatek touchscreen eng reset cmd
    function.

return:
	n.a.
*******************************************************/
void nvt_eng_reset(void)
{
	//---eng reset cmds to ENG_RST_ADDR---
	nvt_write_addr(ENG_RST_ADDR, 0x5A);

	mdelay(1);	//wait tMCU_Idle2TP_REX_Hi after TP_RST
}

/*******************************************************
Description:
	Novatek touchscreen reset MCU
    function.

return:
	n.a.
*******************************************************/
void nvt_sw_reset(void)
{
	//---software reset cmds to SWRST_SIF_ADDR---
	nvt_write_addr(ts->swrst_sif_addr, 0x55);

	msleep(10);
}

/*******************************************************
Description:
	Novatek touchscreen reset MCU then into idle mode
    function.

return:
	n.a.
*******************************************************/
void nvt_sw_reset_idle(void)
{
	//---MCU idle cmds to SWRST_SIF_ADDR---
	nvt_write_addr(ts->swrst_sif_addr, 0xAA);

	msleep(15);
}

/*******************************************************
Description:
	Novatek touchscreen reset MCU (boot) function.

return:
	n.a.
*******************************************************/
void nvt_bootloader_reset(void)
{
	//---reset cmds to SWRST_SIF_ADDR---
	nvt_write_addr(ts->swrst_sif_addr, 0x69);

	mdelay(5);	//wait tBRST2FR after Bootload RST

	if (SPI_RD_FAST_ADDR) {
		/* disable SPI_RD_FAST */
		nvt_write_addr(SPI_RD_FAST_ADDR, 0x00);
	}

	NVT_LOG("end\n");
}

/*******************************************************
Description:
	Novatek touchscreen clear FW status function.

return:
	Executive outcomes. 0---succeed. -1---fail.
*******************************************************/
int32_t nvt_clear_fw_status(void)
{
	uint8_t buf[8] = {0};
	int32_t i = 0;
	const int32_t retry = 20;

	for (i = 0; i < retry; i++) {
		//---set xdata index to EVENT BUF ADDR---
		nvt_set_page(ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE);

		//---clear fw status---
		buf[0] = EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE;
		buf[1] = 0x00;
		CTP_SPI_WRITE(ts->client, buf, 2);

		//---read fw status---
		buf[0] = EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE;
		buf[1] = 0xFF;
		CTP_SPI_READ(ts->client, buf, 2);

		if (buf[1] == 0x00)
			break;

		usleep_range(10000, 10000);
	}

	if (i >= retry) {
		NVT_ERR("failed, i=%d, buf[1]=0x%02X\n", i, buf[1]);
		return -1;
	} else {
		return 0;
	}
}

/*******************************************************
Description:
	Novatek touchscreen check FW status function.

return:
	Executive outcomes. 0---succeed. -1---failed.
*******************************************************/
int32_t nvt_check_fw_status(void)
{
	uint8_t buf[8] = {0};
	int32_t i = 0;
	const int32_t retry = 50;

	usleep_range(20000, 20000);

	for (i = 0; i < retry; i++) {
		//---set xdata index to EVENT BUF ADDR---
		nvt_set_page(ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE);

		//---read fw status---
		buf[0] = EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE;
		buf[1] = 0x00;
		CTP_SPI_READ(ts->client, buf, 2);

		if ((buf[1] & 0xF0) == 0xA0)
			break;

		usleep_range(10000, 10000);
	}

	if (i >= retry) {
		NVT_ERR("failed, i=%d, buf[1]=0x%02X\n", i, buf[1]);
		return -1;
	} else {
		return 0;
	}
}

/*******************************************************
Description:
	Novatek touchscreen check FW reset state function.

return:
	Executive outcomes. 0---succeed. -1---failed.
*******************************************************/
int32_t nvt_check_fw_reset_state(RST_COMPLETE_STATE check_reset_state)
{
	uint8_t buf[8] = {0};
	int32_t ret = 0;
	int32_t retry = 0;
	int32_t retry_max = (check_reset_state == RESET_STATE_INIT) ? 600 : 50;

	//---set xdata index to EVENT BUF ADDR---
	nvt_set_page(ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_RESET_COMPLETE);

	while (1) {
		//---read reset state---
		buf[0] = EVENT_MAP_RESET_COMPLETE;
		buf[1] = 0x00;
		CTP_SPI_READ(ts->client, buf, 6);

		if (((buf[1] >= 0x90) && (buf[1] <= RESET_STATE_MAX)) || (buf[1] == 0x1A)) {
			ret = 0;
			break;
		}

		if (retry == 1)
			NVT_LOG("reset-state first sample: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
				buf[1], buf[2], buf[3], buf[4], buf[5]);

		retry++;
		if(unlikely(retry > retry_max)) {
			NVT_ERR("error, retry=%d, buf[1]=0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X\n",
				retry, buf[1], buf[2], buf[3], buf[4], buf[5]);
			ret = -1;
			break;
		}

		usleep_range(10000, 10000);
	}

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen get firmware related information
	function.

return:
	Executive outcomes. 0---success. -1---fail.
*******************************************************/
int32_t nvt_get_fw_info(void)
{
	uint8_t buf[64] = {0};
	uint32_t retry_count = 0;
	int32_t ret = 0;

info_retry:
	msleep(100);

	//---set xdata index to EVENT BUF ADDR---
	nvt_set_page(ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_FWINFO);

	//---read fw info---
	buf[0] = EVENT_MAP_FWINFO;
	CTP_SPI_READ(ts->client, buf, 39);
	if ((buf[1] + buf[2]) != 0xFF) {
		{
			int _i; char _hx[256] = {0}; int _p = 0;
			for (_i = 0; _i < 39 && _p < 250; _i++)
				_p += scnprintf(_hx + _p, sizeof(_hx) - _p, "%02X ", buf[_i]);
			NVT_ERR("FW info is broken! fw_ver=0x%02X, ~fw_ver=0x%02X raw=[%s]\n", buf[1], buf[2], _hx);
		}
		if (retry_count < 3) {
			retry_count++;
			NVT_LOG("retry_count=%d\n", retry_count);
			goto info_retry;
		} else {
			ts->fw_ver = 0;
			ts->max_button_num = TOUCH_KEY_NUM;
			ret = -EIO;
			goto out;
		}
	}
	ts->fw_ver = buf[1];
	ts->x_num = buf[3];
	ts->y_num = buf[4];
	ts->max_button_num = buf[11];
	ts->nvt_pid = (uint16_t)((buf[36] << 8) | buf[35]);
	if (ts->pen_support) {
		ts->x_gang_num = buf[37];
		ts->y_gang_num = buf[38];
	}
	NVT_LOG("fw_ver=0x%02X, fw_type=0x%02X, PID=0x%04X\n", ts->fw_ver, buf[14], ts->nvt_pid);

	ret = 0;
out:

	return ret;
}

/*******************************************************
  Create Device Node (Proc Entry)
*******************************************************/
#if NVT_TOUCH_PROC
static struct proc_dir_entry *NVT_proc_entry;
static struct proc_dir_entry *NVT_proc_frame_entry;
static struct proc_dir_entry *NVT_proc_diag_entry;
static struct proc_dir_entry *NVT_proc_fwctl_entry;
#define DEVICE_NAME	"NVTSPI"

static const struct proc_ops nvt_frame_fops;
static const struct proc_ops nvt_diag_fops;
static const struct proc_ops nvt_fwctl_fops;

/*******************************************************
Description:
	Novatek touchscreen /proc/NVTSPI read function.

return:
	Executive outcomes. 2---succeed. -5,-14---failed.
*******************************************************/
static ssize_t nvt_flash_read(struct file *file, char __user *buff, size_t count, loff_t *offp)
{
	uint8_t *str = NULL;
	int32_t ret = 0;
	int32_t retries = 0;
	int8_t spi_wr = 0;
	uint8_t *buf;

	if ((count > NVT_TRANSFER_LEN + 3) || (count < 3)) {
		NVT_ERR("invalid transfer len!\n");
		return -EFAULT;
	}

	/* allocate buffer for spi transfer */
	str = (uint8_t *)kzalloc((count), GFP_KERNEL);
	if(str == NULL) {
		NVT_ERR("kzalloc for buf failed!\n");
		ret = -ENOMEM;
		goto kzalloc_failed;
	}

	buf = (uint8_t *)kzalloc((count), GFP_KERNEL | GFP_DMA);
	if(buf == NULL) {
		NVT_ERR("kzalloc for buf failed!\n");
		ret = -ENOMEM;
		kfree(str);
		str = NULL;
		goto kzalloc_failed;
	}

	if (copy_from_user(str, buff, count)) {
		NVT_ERR("copy from user error\n");
		ret = -EFAULT;
		goto out;
	}

#if NVT_TOUCH_ESD_PROTECT
	/*
	 * stop esd check work to avoid case that 0x77 report righ after here to enable esd check again
	 * finally lead to trigger esd recovery bootloader reset
	 */
	cancel_delayed_work_sync(&nvt_esd_check_work);
	nvt_esd_check_enable(false);
#endif /* #if NVT_TOUCH_ESD_PROTECT */

	spi_wr = str[0] >> 7;
	memcpy(buf, str+2, ((str[0] & 0x7F) << 8) | str[1]);

	if (spi_wr == NVTWRITE) {	//SPI write
		while (retries < 20) {
			ret = CTP_SPI_WRITE(ts->client, buf, ((str[0] & 0x7F) << 8) | str[1]);
			if (!ret)
				break;
			else
				NVT_ERR("error, retries=%d, ret=%d\n", retries, ret);

			retries++;
		}

		if (unlikely(retries == 20)) {
			NVT_ERR("error, ret = %d\n", ret);
			ret = -EIO;
			goto out;
		}
	} else if (spi_wr == NVTREAD) {	//SPI read
		while (retries < 20) {
			ret = CTP_SPI_READ(ts->client, buf, ((str[0] & 0x7F) << 8) | str[1]);
			if (!ret)
				break;
			else
				NVT_ERR("error, retries=%d, ret=%d\n", retries, ret);

			retries++;
		}

		memcpy(str+2, buf, ((str[0] & 0x7F) << 8) | str[1]);
		// copy buff to user if spi transfer
		if (retries < 20) {
			if (copy_to_user(buff, str, count)) {
				ret = -EFAULT;
				goto out;
			}
		}

		if (unlikely(retries == 20)) {
			NVT_ERR("error, ret = %d\n", ret);
			ret = -EIO;
			goto out;
		}
	} else {
		NVT_ERR("Call error, str[0]=%d\n", str[0]);
		ret = -EFAULT;
		goto out;
	}

out:
	kfree(str);
    kfree(buf);
kzalloc_failed:
	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen /proc/NVTSPI open function.

return:
	Executive outcomes. 0---succeed. -12---failed.
*******************************************************/
static int32_t nvt_flash_open(struct inode *inode, struct file *file)
{
	struct nvt_flash_data *dev;

	dev = kmalloc(sizeof(struct nvt_flash_data), GFP_KERNEL);
	if (dev == NULL) {
		NVT_ERR("Failed to allocate memory for nvt flash data\n");
		return -ENOMEM;
	}

	rwlock_init(&dev->lock);
	file->private_data = dev;

	return 0;
}

/*******************************************************
Description:
	Novatek touchscreen /proc/NVTSPI close function.

return:
	Executive outcomes. 0---succeed.
*******************************************************/
static int32_t nvt_flash_close(struct inode *inode, struct file *file)
{
	struct nvt_flash_data *dev = file->private_data;

	if (dev)
		kfree(dev);

	return 0;
}

#ifdef HAVE_PROC_OPS
static const struct proc_ops nvt_flash_fops = {
	.proc_open = nvt_flash_open,
	.proc_release = nvt_flash_close,
	.proc_read = nvt_flash_read,
};
#else
static const struct file_operations nvt_flash_fops = {
	.owner = THIS_MODULE,
	.open = nvt_flash_open,
	.release = nvt_flash_close,
	.read = nvt_flash_read,
};
#endif

/*******************************************************
Description:
	Novatek touchscreen /proc/NVTSPI initial function.

return:
	Executive outcomes. 0---succeed. -12---failed.
*******************************************************/
static int32_t nvt_flash_proc_init(void)
{
	NVT_proc_entry = proc_create(DEVICE_NAME, 0444, NULL,&nvt_flash_fops);
	if (NVT_proc_entry == NULL) {
		NVT_ERR("Failed!\n");
		return -ENOMEM;
	} else {
		NVT_LOG("Succeeded!\n");
	}

	NVT_LOG("============================================================\n");
	NVT_LOG("Create /proc/%s\n", DEVICE_NAME);
	NVT_LOG("============================================================\n");

	NVT_proc_frame_entry = proc_create("nvt_frame", 0444, NULL, &nvt_frame_fops);
	if (NVT_proc_frame_entry == NULL)
		NVT_ERR("create /proc/nvt_frame failed!\n");
	else
		NVT_LOG("Create /proc/nvt_frame\n");

	NVT_proc_diag_entry = proc_create("nvt_diag", 0444, NULL, &nvt_diag_fops);
	if (NVT_proc_diag_entry == NULL)
		NVT_ERR("create /proc/nvt_diag failed!\n");
	else
		NVT_LOG("Create /proc/nvt_diag\n");

	NVT_proc_fwctl_entry = proc_create("nvt_fw_control", 0666, NULL, &nvt_fwctl_fops);
	if (NVT_proc_fwctl_entry == NULL)
		NVT_ERR("create /proc/nvt_fw_control failed!\n");
	else
		NVT_LOG("Create /proc/nvt_fw_control\n");

	return 0;
}

/*******************************************************
Description:
	Novatek touchscreen /proc/NVTSPI deinitial function.

return:
	n.a.
*******************************************************/
static void nvt_flash_proc_deinit(void)
{
	if (NVT_proc_entry != NULL) {
		remove_proc_entry(DEVICE_NAME, NULL);
		NVT_proc_entry = NULL;
		NVT_LOG("Removed /proc/%s\n", DEVICE_NAME);
	}
	if (NVT_proc_frame_entry != NULL) {
		remove_proc_entry("nvt_frame", NULL);
		NVT_proc_frame_entry = NULL;
	}
	if (NVT_proc_diag_entry != NULL) {
		remove_proc_entry("nvt_diag", NULL);
		NVT_proc_diag_entry = NULL;
	}
	if (NVT_proc_fwctl_entry != NULL) {
		remove_proc_entry("nvt_fw_control", NULL);
		NVT_proc_fwctl_entry = NULL;
	}
}

/*******************************************************
Description:
	Bring-up diagnostics: update per-frame counters so live
	scanning can be verified under simpledrm via /proc/nvt_diag.

return:
	N/A.
*******************************************************/
static void nvt_diag_update(uint8_t *point_data)
{
	u32 mask = 0;
	int fi;

	if (!ts)
		return;

	ts->diag_frames++;
	for (fi = 0; fi < ts->max_touch_num; fi++) {
		u32 s = point_data[1 + 6 * fi] & 0x07;
		if ((s == 0x01) || (s == 0x02))
			mask |= (1U << fi);
	}
	ts->diag_last_status_mask = mask;
	if (mask) {
		ts->diag_touch_frames++;
		if (hweight32(mask) > ts->diag_max_fingers)
			ts->diag_max_fingers = hweight32(mask);
	}
	ts->diag_last_jiffies = jiffies;
}

/*******************************************************
Description:
	Experimental continuous / free-running touch scan kick
	(guide "Option 1"). The NT36523 is a TDDI whose touch AFE
	normally scans inside the DSI v-blank window; with simpledrm
	there is no DSI clock, so we poke the MCU to keep scanning
	with its own timing. Guarded by nvt_asyn_scan (default on),
	recoverable via /proc/nvt_fw_control or boot param nvt_asyn_scan=0.

return:
	N/A.
*******************************************************/
void nvt_force_continuous_scan(void)
{
	/* 0x03F102 write is known to corrupt MCU state into 0xFE error mode; keep no-op */
	return;
}

#if NVT_TOUCH_PROC
/* /proc/nvt_frame: single 108-byte coordinate frame. Layout matches the IRQ
 * path (see nvt_ts_work_func): finger i status/id byte at index 1+6*i,
 * X=(b<<8)|c, Y=(d<<8)|e, major=b, pressure=1+98+i. */
static ssize_t nvt_frame_proc_read(struct file *file, char __user *buff,
				   size_t count, loff_t *offp)
{
	uint8_t point_data[POINT_DATA_LEN + 1 + DUMMY_BYTES] = {0};
	unsigned int base, len;
	int32_t ret;

	if (!ts || !ts->client || !ts->mmap)
		return -ENODEV;

	base = nvt_poll_base ? nvt_poll_base : ts->mmap->EVENT_BUF_ADDR;
	len = nvt_diag_len;
	if (len > POINT_DATA_LEN)
		len = POINT_DATA_LEN;
	if (count < len)
		return -EINVAL;

	mutex_lock(&ts->lock);
	nvt_set_page(base);
	point_data[0] = 0x00;
	ret = CTP_SPI_READ(ts->client, point_data, len + 1);
	if (ret) {
		ts->diag_spi_errors++;
		mutex_unlock(&ts->lock);
		return -EIO;
	}
	nvt_diag_update(point_data);
	mutex_unlock(&ts->lock);

	if (copy_to_user(buff, point_data, len))
		return -EFAULT;

	return len;
}
static const struct proc_ops nvt_frame_fops = {
	.proc_read = nvt_frame_proc_read,
};

/* Read a small register window over SPI at the chip page base. */
static void nvt_reg_rd(u32 addr, u8 *out, int len)
{
	u8 buf[8] = {0};

	if (!ts || !ts->client)
		return;

	nvt_set_page(addr);
	buf[0] = addr & 0x7F;
	if (CTP_SPI_READ(ts->client, buf, len + 1))
		memset(out, 0, len);
	else
		memcpy(out, buf + 1, len);
}

static u32 nvt_reg32(u32 addr)
{
	u8 v[4];
	nvt_reg_rd(addr, v, 4);
	return ((u32)v[0] << 24) | ((u32)v[1] << 16) | ((u32)v[2] << 8) | v[3];
}

static u32 nvt_reg_rd_as_u8(u32 addr)
{
	u8 v[1];
	nvt_reg_rd(addr, v, 1);
	return v[0];
}

static u32 nvt_reg24(u32 addr)
{
	u8 v[3];
	nvt_reg_rd(addr, v, 3);
	return ((u32)v[0] << 16) | ((u32)v[1] << 8) | v[2];
}

/* /proc/nvt_diag: bring-up counters to prove live scanning, plus the
 * TDDI boot state (reset byte, fw info, CRC latches, descriptor addrs). */
static ssize_t nvt_diag_proc_read(struct file *file, char __user *buff,
				  size_t count, loff_t *offp)
{
	char kbuf[512];
	int n;
	unsigned long last;
	u8 rv[6];
	unsigned int base;

	if (*offp > 0)
		return 0;
	if (count < sizeof(kbuf))
		return -EINVAL;
	if (!ts)
		return -ENODEV;

	base = nvt_poll_base ? nvt_poll_base : ts->mmap->EVENT_BUF_ADDR;
	last = ts->diag_last_jiffies;
	n = scnprintf(kbuf, sizeof(kbuf),
		"frames=%llu touch_frames=%llu spi_errors=%llu "
		"last_mask=0x%x max_fingers=%u last_ms_ago=%u poll_ms=%d asyn=%d base=0x%06X\n",
		(unsigned long long)ts->diag_frames,
		(unsigned long long)ts->diag_touch_frames,
		(unsigned long long)ts->diag_spi_errors,
		ts->diag_last_status_mask,
		ts->diag_max_fingers,
		last ? jiffies_to_msecs(jiffies - last) : 0U,
		nvt_poll_ms, nvt_asyn_scan, base);

	mutex_lock(&ts->lock);
	nvt_reg_rd(base + 0x60, rv, 1);
	n += scnprintf(kbuf + n, sizeof(kbuf) - n,
		"regs: reset=0x%02X ", rv[0]);
	nvt_reg_rd(base + 0x78, rv, 6);
	n += scnprintf(kbuf + n, sizeof(kbuf) - n,
		"fwinfo=%02X %02X/%02X %02X %02X %02X ", rv[0], rv[1], rv[2], rv[3], rv[4], rv[5]);
	nvt_reg_rd(0x3F0FE, rv, 1);
	n += scnprintf(kbuf + n, sizeof(kbuf) - n,
		"swrst=0x%02X ", rv[0]);
	n += scnprintf(kbuf + n, sizeof(kbuf) - n,
		"gilm=0x%08X gdlm=0x%08X ", nvt_reg32(0x3F100), nvt_reg32(0x3F104));
	n += scnprintf(kbuf + n, sizeof(kbuf) - n,
		"ilmd=0x%06X dlmd=0x%06X ", nvt_reg24(0x3F128), nvt_reg24(0x3F12C));
	nvt_reg_rd(0x3F134, rv, 3);
	n += scnprintf(kbuf + n, sizeof(kbuf) - n,
		"crcflag=0x%02X crcen=0x%02X chipver=0x%02X ", rv[0], rv[1], rv[2]);
	n += scnprintf(kbuf + n, sizeof(kbuf) - n,
		"dmaval=0x%02X txinfo=0x%02X autocopy=0x%02X ",
		nvt_reg_rd_as_u8(0x3F7D0), nvt_reg_rd_as_u8(0x3F7F1),
		nvt_reg_rd_as_u8(0x3F7E8));
	nvt_set_page(base);
	mutex_unlock(&ts->lock);
	if (gpio_is_valid(ts->irq_gpio) && gpio_is_valid(ts->reset_gpio))
		n += scnprintf(kbuf + n, sizeof(kbuf) - n,
			"gpio: irq=%d rst=%d\n",
			gpio_get_value(ts->irq_gpio), gpio_get_value(ts->reset_gpio));
	else
		n += scnprintf(kbuf + n, sizeof(kbuf) - n, "gpio: n/a\n");

	if (copy_to_user(buff, kbuf, n))
		return -EFAULT;
	*offp += n;
	return n;
}
static const struct proc_ops nvt_diag_fops = {
	.proc_read = nvt_diag_proc_read,
};

/* /proc/nvt_fw_control: re-arm / experiment with touch MCU state.
 * Write 's' = full normal-run restart, 'r' = soft reset, 'a' = asyn kick. */
static ssize_t nvt_fwctl_proc_read(struct file *file, char __user *buff,
				   size_t count, loff_t *offp)
{
	const char msg[] = "Usage: echo <cmd> > /proc/nvt_fw_control\n"
			   "  s = full normal-run restart\n"
			   "  r = software reset\n"
			   "  a = continuous-scan (asyn) kick\n"
			   "  F = full firmware re-download over SPI\n"
			   "  R = bootloader (hard) reset of MCU\n"
			   "  T = transport probe: single 31KB SPI write+readback\n"
			   "  t = transport probe: 2KB-chunked write+readback\n";
	if (count < sizeof(msg))
		return -EINVAL;
	if (copy_to_user(buff, msg, sizeof(msg)))
		return -EFAULT;
	return sizeof(msg);
}
static ssize_t nvt_fwctl_proc_write(struct file *file, const char __user *buffer,
				    size_t count, loff_t *offp)
{
	char msg[64];
	char cmd = 0;
	uint32_t addr = 0, val = 0;
	uint8_t rv[4] = {0};

	if (!ts || !ts->client)
		return -ENODEV;
	if (count < 1 || count > sizeof(msg))
		return -EINVAL;
	if (copy_from_user(msg, buffer, count))
		return -EFAULT;
	sscanf(msg, "%c%x%x", &cmd, &addr, &val);

	switch (cmd) {
	case 'w':
	case 'W': {
		uint8_t before[4] = {0};
		uint32_t waddr = addr & 0xFFFFF, wval = val & 0xFF;

		mutex_lock(&ts->lock);
		nvt_reg_rd(waddr, before, 4);
		nvt_write_addr(waddr, wval);
		msleep(20);
		nvt_reg_rd(waddr, rv, 4);
		mutex_unlock(&ts->lock);
		NVT_LOG("[fwctl] W 0x%X=0x%02X before=%02X%02X%02X%02X "
			"after=%02X%02X%02X%02X\n", waddr, wval,
			before[0], before[1], before[2], before[3],
			rv[0], rv[1], rv[2], rv[3]);
		break;
	}
	case 's':
		mutex_lock(&ts->lock);
		nvt_sw_reset_idle();
		nvt_boot_ready();
		nvt_tx_auto_copy_mode();
		nvt_sw_reset();
		mutex_unlock(&ts->lock);
		msleep(20);
		nvt_force_continuous_scan();
		break;
	case 'r':
		mutex_lock(&ts->lock);
		nvt_sw_reset_idle();
		nvt_sw_reset();
		mutex_unlock(&ts->lock);
		break;
	case 'a':
		nvt_force_continuous_scan();
		break;
	case 'F':
		mutex_lock(&ts->lock);
		nvt_update_firmware(boot_update_firmware_name);
		mutex_unlock(&ts->lock);
		msleep(20);
		nvt_force_continuous_scan();
		NVT_LOG("[fwctl] full firmware re-download done\n");
		break;
	case 'R':
		mutex_lock(&ts->lock);
		nvt_bootloader_reset();
		mutex_unlock(&ts->lock);
		msleep(200);
		NVT_LOG("[fwctl] bootloader reset done\n");
		break;
	case 'T':
		mutex_lock(&ts->lock);
		nvt_dbg_probe(0);
		mutex_unlock(&ts->lock);
		break;
	case 't':
		mutex_lock(&ts->lock);
		nvt_dbg_probe(1);
		mutex_unlock(&ts->lock);
		break;
	default:
		return -EINVAL;
	}

	return count;
}
static const struct proc_ops nvt_fwctl_fops = {
	.proc_read = nvt_fwctl_proc_read,
	.proc_write = nvt_fwctl_proc_write,
};
#endif /* NVT_TOUCH_PROC */
#endif

/* customized gesture/stylus data protocol */
#define DATA_PROTOCOL           30
#define FUNCPAGE_GESTURE         1

#if WAKEUP_GESTURE
#define GESTURE_WORD_C          12
#define GESTURE_WORD_W          13
#define GESTURE_WORD_V          14
#define GESTURE_DOUBLE_CLICK    15
#define GESTURE_WORD_Z          16
#define GESTURE_WORD_M          17
#define GESTURE_WORD_O          18
#define GESTURE_WORD_e          19
#define GESTURE_WORD_S          20
#define GESTURE_SLIDE_UP        21
#define GESTURE_SLIDE_DOWN      22
#define GESTURE_SLIDE_LEFT      23
#define GESTURE_SLIDE_RIGHT     24
/* customized gesture id */
#define DATA_PROTOCOL           30

/* function page definition */
#define FUNCPAGE_GESTURE         1

/*******************************************************
Description:
	Novatek touchscreen wake up gesture key report function.

return:
	n.a.
*******************************************************/
void nvt_ts_wakeup_gesture_report(uint8_t gesture_id, uint8_t *data)
{
	uint32_t keycode = 0;
	uint8_t func_type = data[2];
	uint8_t func_id = data[3];

	/* support fw specifal data protocol */
	if ((gesture_id == DATA_PROTOCOL) && (func_type == FUNCPAGE_GESTURE)) {
		gesture_id = func_id;
	} else if (gesture_id > DATA_PROTOCOL) {
		NVT_ERR("gesture_id %d is invalid, func_type=%d, func_id=%d\n", gesture_id, func_type, func_id);
		return;
	}

	NVT_LOG("gesture_id = %d\n", gesture_id);

	switch (gesture_id) {
		case GESTURE_WORD_C:
			NVT_LOG("Gesture : Word-C.\n");
			keycode = gesture_key_array[0];
			break;
		case GESTURE_WORD_W:
			NVT_LOG("Gesture : Word-W.\n");
			keycode = gesture_key_array[1];
			break;
		case GESTURE_WORD_V:
			NVT_LOG("Gesture : Word-V.\n");
			keycode = gesture_key_array[2];
			break;
		case GESTURE_DOUBLE_CLICK:
			NVT_LOG("Gesture : Double Click.\n");
			keycode = gesture_key_array[3];
			break;
		case GESTURE_WORD_Z:
			NVT_LOG("Gesture : Word-Z.\n");
			keycode = gesture_key_array[4];
			break;
		case GESTURE_WORD_M:
			NVT_LOG("Gesture : Word-M.\n");
			keycode = gesture_key_array[5];
			break;
		case GESTURE_WORD_O:
			NVT_LOG("Gesture : Word-O.\n");
			keycode = gesture_key_array[6];
			break;
		case GESTURE_WORD_e:
			NVT_LOG("Gesture : Word-e.\n");
			keycode = gesture_key_array[7];
			break;
		case GESTURE_WORD_S:
			NVT_LOG("Gesture : Word-S.\n");
			keycode = gesture_key_array[8];
			break;
		case GESTURE_SLIDE_UP:
			NVT_LOG("Gesture : Slide UP.\n");
			keycode = gesture_key_array[9];
			break;
		case GESTURE_SLIDE_DOWN:
			NVT_LOG("Gesture : Slide DOWN.\n");
			keycode = gesture_key_array[10];
			break;
		case GESTURE_SLIDE_LEFT:
			NVT_LOG("Gesture : Slide LEFT.\n");
			keycode = gesture_key_array[11];
			break;
		case GESTURE_SLIDE_RIGHT:
			NVT_LOG("Gesture : Slide RIGHT.\n");
			keycode = gesture_key_array[12];
			break;
		default:
			break;
	}

	if (keycode > 0) {
		input_report_key(ts->input_dev, keycode, 1);
		input_sync(ts->input_dev);
		input_report_key(ts->input_dev, keycode, 0);
		input_sync(ts->input_dev);
	}
}
#endif

/*******************************************************
Description:
	Novatek touchscreen set charger flag function.
	If insert usb charger called nvt_set_charger(USB_DETECT_IN)
	Otherwise called nvt_set_charger(USB_DETECT_OUT)
		
return:
	n.a.
*******************************************************/
#define USB_DETECT_IN 1
#define USB_DETECT_OUT 0
#define CMD_CHARGER_ON	(0x53)
#define CMD_CHARGER_OFF (0x51)
int32_t nvt_set_charger(uint8_t charger_on_off)
{
	uint8_t buf[8] = {0};
	int32_t ret = 0;

	NVT_LOG("set charger: %d\n", charger_on_off);

	msleep(20);
	
	mutex_lock(&ts->lock);
	//---set xdata index to EVENT BUF ADDR---
	ret = nvt_set_page(ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_HOST_CMD);
	if (ret < 0) {
		NVT_ERR("Set event buffer index fail!\n");
		goto nvt_set_charger_out;
	}

	if (charger_on_off == USB_DETECT_IN) {
		buf[0] = EVENT_MAP_HOST_CMD;
		buf[1] = CMD_CHARGER_ON;
		ret = CTP_SPI_WRITE(ts->client, buf, 2);
		if (ret < 0) {
			NVT_ERR("Write set charger command fail!\n");
			goto nvt_set_charger_out;
		} else {
			NVT_LOG("set charger on cmd succeeded\n");
		}
	} else if (charger_on_off == USB_DETECT_OUT) {
		buf[0] = EVENT_MAP_HOST_CMD;
		buf[1] = CMD_CHARGER_OFF;
		ret = CTP_SPI_WRITE(ts->client, buf, 2);
		if (ret < 0) {
			NVT_ERR("Write set charger command fail!\n");
			goto nvt_set_charger_out;
		} else {
			NVT_LOG("set charger off cmd succeeded\n");
		}
	} else {
		NVT_ERR("Invalid charger parameter!\n");
		ret = -EINVAL;
	}

    nvt_set_charger_out:

	mutex_unlock(&ts->lock);

	return ret;
}
EXPORT_SYMBOL(nvt_set_charger);

/*******************************************************
Description:
	Novatek touchscreen parse device tree function.

return:
	n.a.
*******************************************************/
#ifdef CONFIG_OF
static int32_t nvt_parse_dt(struct device *dev)
{
	struct device_node *np = dev->of_node;
	int32_t ret = 0;

#if NVT_TOUCH_SUPPORT_HW_RST
	ts->reset_gpio = of_get_named_gpio_flags(np, "novatek,reset-gpio", 0, &ts->reset_flags);
	NVT_LOG("novatek,reset-gpio=%d\n", ts->reset_gpio);
#endif
	ts->irq_gpio = of_get_named_gpio_flags(np, "novatek,irq-gpio", 0, &ts->irq_flags);
	NVT_LOG("novatek,irq-gpio=%d\n", ts->irq_gpio);

	ts->id0_gpio = of_get_named_gpio_flags(np, "novatek,id0-gpio", 0, &ts->id0_flags);
	NVT_LOG("novatek,id0-gpio=%d\n", ts->id0_gpio);
	ts->id1_gpio = of_get_named_gpio_flags(np, "novatek,id1-gpio", 0, &ts->id1_flags);
	NVT_LOG("novatek,id1-gpio=%d\n", ts->id1_gpio);
	ts->pen_support = of_property_read_bool(np, "novatek,pen-support");
	NVT_LOG("novatek,pen-support=%d\n", ts->pen_support);

	ret = of_property_read_u32(np, "novatek,spi-rd-fast-addr", &SPI_RD_FAST_ADDR);
	if (ret) {
		NVT_LOG("not support novatek,spi-rd-fast-addr\n");
		SPI_RD_FAST_ADDR = 0;
		ret = 0;
	} else {
		NVT_LOG("SPI_RD_FAST_ADDR=0x%06X\n", SPI_RD_FAST_ADDR);
	}

	return ret;
}
#else
static int32_t nvt_parse_dt(struct device *dev)
{
#if NVT_TOUCH_SUPPORT_HW_RST
	ts->reset_gpio = NVTTOUCH_RST_PIN;
#endif
	ts->irq_gpio = NVTTOUCH_INT_PIN;
	ts->pen_support = false;
	return 0;
}
#endif

/*******************************************************
Description:
	Novatek touchscreen config and request gpio

return:
	Executive outcomes. 0---succeed. not 0---failed.
*******************************************************/
static int nvt_gpio_config(struct nvt_ts_data *ts)
{
	int32_t ret = 0;

#if NVT_TOUCH_SUPPORT_HW_RST
	/* request RST-pin (Output/High) */
	if (gpio_is_valid(ts->reset_gpio)) {
		ret = gpio_request_one(ts->reset_gpio, GPIOF_OUT_INIT_LOW, "NVT-tp-rst");
		if (ret) {
			NVT_ERR("Failed to request NVT-tp-rst GPIO\n");
			goto err_request_reset_gpio;
		}
	}
#endif

	/* request INT-pin (Input with internal pull-up for open-drain) */
	if (gpio_is_valid(ts->irq_gpio)) {
		ret = gpio_request_one(ts->irq_gpio, GPIOF_IN, "NVT-int");
		if (ret) {
			NVT_ERR("Failed to request NVT-int GPIO\n");
			goto err_request_irq_gpio;
		}
		if (gpio_to_desc(ts->irq_gpio)) {
			gpiod_set_config(gpio_to_desc(ts->irq_gpio), PIN_CONF_PACKED(PIN_CONFIG_BIAS_PULL_UP, 1));
		}
	}

	return ret;

err_request_irq_gpio:
#if NVT_TOUCH_SUPPORT_HW_RST
	gpio_free(ts->reset_gpio);
err_request_reset_gpio:
#endif
	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen deconfig gpio

return:
	n.a.
*******************************************************/
static void nvt_gpio_deconfig(struct nvt_ts_data *ts)
{
	if (gpio_is_valid(ts->irq_gpio))
		gpio_free(ts->irq_gpio);
#if NVT_TOUCH_SUPPORT_HW_RST
	if (gpio_is_valid(ts->reset_gpio))
		gpio_free(ts->reset_gpio);
#endif
}

static __maybe_unused uint8_t nvt_fw_recovery(uint8_t *point_data)
{
	uint8_t i = 0;
	uint8_t detected = true;

	/* check pattern */
	for (i=1 ; i<7 ; i++) {
		if (point_data[i] != 0x77) {
			detected = false;
			break;
		}
	}

	return detected;
}

#if NVT_TOUCH_ESD_PROTECT
void nvt_esd_check_enable(uint8_t enable)
{
	/* update interrupt timer */
	irq_timer = jiffies;
	/* clear esd_retry counter, if protect function is enabled */
	esd_retry = enable ? 0 : esd_retry;
	/* enable/disable esd check flag */
	esd_check = enable;
}

static void nvt_esd_check_func(struct work_struct *work)
{
	unsigned int timer = jiffies_to_msecs(jiffies - irq_timer);

	//NVT_LOG("esd_check = %d (retry %d)\n", esd_check, esd_retry);	//DEBUG

	if ((timer > NVT_TOUCH_ESD_CHECK_PERIOD) && esd_check) {
		mutex_lock(&ts->lock);
		NVT_ERR("do ESD recovery, timer = %d, retry = %d\n", timer, esd_retry);
		/* do esd recovery, reload fw */
		nvt_update_firmware(boot_update_firmware_name);
		mutex_unlock(&ts->lock);
		/* update interrupt timer */
		irq_timer = jiffies;
		/* update esd_retry counter */
		esd_retry++;
	}

	queue_delayed_work(nvt_esd_check_wq, &nvt_esd_check_work,
			msecs_to_jiffies(NVT_TOUCH_ESD_CHECK_PERIOD));
}
#endif /* #if NVT_TOUCH_ESD_PROTECT */

#define PEN_DATA_LEN 14
#if CHECK_PEN_DATA_CHECKSUM
static int32_t nvt_ts_pen_data_checksum(uint8_t *buf, uint8_t length)
{
	uint8_t checksum = 0;
	int32_t i = 0;

	// Calculate checksum
	for (i = 0; i < length - 1; i++) {
		checksum += buf[i];
	}
	checksum = (~checksum + 1);

	// Compare ckecksum and dump fail data
	if (checksum != buf[length - 1]) {
		NVT_ERR("pen packet checksum not match. (buf[%d]=0x%02X, checksum=0x%02X)\n",
			length - 1, buf[length - 1], checksum);
		//--- dump pen buf ---
		for (i = 0; i < length; i++) {
			printk("%02X ", buf[i]);
		}
		printk("\n");

		return -1;
	}

	return 0;
}
#endif // #if CHECK_PEN_DATA_CHECKSUM

#if NVT_TOUCH_WDT_RECOVERY
static uint8_t recovery_cnt = 0;
static uint8_t nvt_wdt_fw_recovery(uint8_t *point_data)
{
   uint32_t recovery_cnt_max = 10;
   uint8_t recovery_enable = false;
   uint8_t i = 0;

   recovery_cnt++;

   /* check pattern */
   for (i=1 ; i<7 ; i++) {
       if ((point_data[i] != 0xFD) && (point_data[i] != 0xFE)) {
           recovery_cnt = 0;
           break;
       }
   }

   if (recovery_cnt > recovery_cnt_max){
       recovery_enable = true;
       recovery_cnt = 0;
   }

   return recovery_enable;
}

void nvt_clear_aci_error_flag(void)
{
	if (ts->mmap->ACI_ERR_CLR_ADDR == 0)
		return;

	nvt_write_addr(ts->mmap->ACI_ERR_CLR_ADDR, 0xA5);

	nvt_set_page(ts->mmap->EVENT_BUF_ADDR);
}
#endif	/* #if NVT_TOUCH_WDT_RECOVERY */

void nvt_read_fw_history(uint32_t fw_history_addr)
{
	uint8_t i = 0;
	uint8_t buf[65];
	char str[128];

	if (fw_history_addr == 0)
		return;

    nvt_set_page(fw_history_addr);

    buf[0] = (uint8_t) (fw_history_addr & 0x7F);
    CTP_SPI_READ(ts->client, buf, 64+1);	//read 64bytes history

	//print all data
	NVT_LOG("fw history 0x%X: \n", fw_history_addr);
	for (i = 0; i < 4; i++) {
		snprintf(str, sizeof(str),
				"%02X %02X %02X %02X %02X %02X %02X %02X  "
				"%02X %02X %02X %02X %02X %02X %02X %02X\n",
				buf[1+i*16], buf[2+i*16], buf[3+i*16], buf[4+i*16],
				buf[5+i*16], buf[6+i*16], buf[7+i*16], buf[8+i*16],
				buf[9+i*16], buf[10+i*16], buf[11+i*16], buf[12+i*16],
				buf[13+i*16], buf[14+i*16], buf[15+i*16], buf[16+i*16]);
		NVT_LOG("%s", str);
	}

	nvt_set_page(ts->mmap->EVENT_BUF_ADDR);
}

void nvt_read_fw_history_all(void) {

	/* ICM History */
	nvt_read_fw_history(ts->mmap->MMAP_HISTORY_EVENT0);
	nvt_read_fw_history(ts->mmap->MMAP_HISTORY_EVENT1);

	/* ICS History */
	if (ts->is_cascade) {
		nvt_read_fw_history(ts->mmap->MMAP_HISTORY_EVENT0_ICS);
		nvt_read_fw_history(ts->mmap->MMAP_HISTORY_EVENT1_ICS);
	}
}

#if POINT_DATA_CHECKSUM
static int32_t nvt_ts_point_data_checksum(uint8_t *buf, uint8_t length)
{
	uint8_t checksum = 0;
	int32_t i = 0;

	// Generate checksum
	for (i = 0; i < length - 1; i++) {
		checksum += buf[i + 1];
	}
	checksum = (~checksum + 1);

	// Compare ckecksum and dump fail data
	if (checksum != buf[length]) {
		NVT_ERR("i2c/spi packet checksum not match. (point_data[%d]=0x%02X, checksum=0x%02X)\n",
				length, buf[length], checksum);

		for (i = 0; i < 10; i++) {
			NVT_LOG("%02X %02X %02X %02X %02X %02X\n",
					buf[1 + i*6], buf[2 + i*6], buf[3 + i*6], buf[4 + i*6], buf[5 + i*6], buf[6 + i*6]);
		}

		NVT_LOG("%02X %02X %02X %02X %02X\n", buf[61], buf[62], buf[63], buf[64], buf[65]);

		return -1;
	}

	return 0;
}
#endif /* POINT_DATA_CHECKSUM */
#if NVT_DPR_SWITCH
#define FUNCPAGE_STYLUS 5
#define LEAVE_PEN_MODE 0
#define DETECT_PEN 1
#define ENTER_PEN_MODE 2
int32_t nvt_check_stylus_state(uint8_t input_id, uint8_t *data)
{
	int32_t ret = 0;
	uint8_t func_type = data[2];
	uint8_t stylus_state = data[3];
	if ((input_id == DATA_PROTOCOL) && (func_type == FUNCPAGE_STYLUS)) {
		ret = stylus_state;
		if (stylus_state == DETECT_PEN) {
			NVT_LOG("Enter stylus mode\n");
			input_report_key(ts->input_dev, KEY_STYLUS, 1);
			input_sync(ts->input_dev);
			input_report_key(ts->input_dev, KEY_STYLUS, 0);
			input_sync(ts->input_dev);
		} else if(stylus_state == ENTER_PEN_MODE) {
			ts->fw_pen_state = 1;
		} else if(stylus_state == LEAVE_PEN_MODE) {
			ts->fw_pen_state = 0;
			ret = 1;
		} else {
			NVT_ERR("invalid state %d!\n", stylus_state);
			ret = -1;
		}
	}
	return ret;
}
#endif
/*******************************************************
Description:
	Novatek touchscreen work function.

return:
	n.a.
*******************************************************/
static irqreturn_t nvt_ts_work_func(int irq, void *data)
{
	int32_t ret = -1;
	uint8_t point_data[POINT_DATA_LEN + PEN_DATA_LEN + 1 + DUMMY_BYTES] = {0};
	uint32_t position = 0;
	uint32_t input_x = 0;
	uint32_t input_y = 0;
	uint32_t input_w = 0;
	uint32_t input_p = 0;
	uint8_t input_id = 0;
#if MT_PROTOCOL_B
	uint8_t press_id[TOUCH_MAX_FINGER_NUM] = {0};
#endif /* MT_PROTOCOL_B */
	int32_t i = 0;
	int32_t finger_cnt = 0;
	uint8_t pen_format_id = 0;
	uint32_t pen_x = 0;
	uint32_t pen_y = 0;
	uint32_t pen_pressure = 0;
	uint32_t pen_distance = 0;
	int8_t pen_tilt_x = 0;
	int8_t pen_tilt_y = 0;
	uint32_t pen_btn1 = 0;
	uint32_t pen_btn2 = 0;
	uint32_t pen_battery = 0;
	u16 frame_time	= 0;
	struct lenovo_pen_info *ppen_info;

#if NVT_CUST_PROC_CMD
	uint8_t wdt_recovery = false;
#endif

#if WAKEUP_GESTURE
	if (bTouchIsAwake == 0) {
		pm_wakeup_event(&ts->input_dev->dev, 5000);
	}
#endif

	/* Single-consumer guard: the poll kthread calls this with irq==0. When
	 * nvt_poll_exclusive is set, let ONLY the poll thread read/report the
	 * touch event buffer. The IRQ-driven invocation (irq != 0) returns
	 * immediately, eliminating the IRQ+poll double-read that made touches
	 * register one frame late (coordinate shown only on the next scan). */
	if (nvt_poll_exclusive && irq != 0) {
		return IRQ_HANDLED;
	}

	mutex_lock(&ts->lock);

#if NVT_PM_WAIT_BUS_RESUME_COMPLETE
	if (ts->dev_pm_suspend) {
		ret = wait_for_completion_timeout(&ts->dev_pm_resume_completion, msecs_to_jiffies(500));
		if (!ret) {
			NVT_ERR("system(bus) can't finished resuming procedure, skip it!\n");
			goto XFER_ERROR;
		}
	}
#endif /* NVT_PM_WAIT_BUS_RESUME_COMPLETE */

	point_data[0] = 0x00;
	if (nvt_poll_ms > 0) {
		/* Polling path: the MCU page can be left at any address after a
		 * skipped/complete firmware update (probe no longer sets the page),
		 * so pin it to EVENT_BUF_ADDR before every read. Reading the wrong
		 * page yields a FIXED set of coords with an incrementing counter. */
		nvt_set_page(nvt_poll_base ? nvt_poll_base : ts->mmap->EVENT_BUF_ADDR);
	}
#if NVT_SUPER_RESOLUTION_N
	ret = CTP_SPI_READ(ts->client, point_data, POINT_DATA_LEN + 1);
#else /* #if NVT_SUPER_RESOLUTION_N */
	if (ts->pen_support)
		ret = CTP_SPI_READ(ts->client, point_data, POINT_DATA_LEN + PEN_DATA_LEN + 1);
	else
		ret = CTP_SPI_READ(ts->client, point_data, POINT_DATA_LEN + 1);
#endif /* #if NVT_SUPER_RESOLUTION_N */
	if (ret < 0) {
		ts->diag_spi_errors++;
		if (time_after(jiffies, ts->diag_last_err_jiffies + HZ / 4))
			NVT_ERR("CTP_SPI_READ failed.(%d)\n", ret);
		ts->diag_last_err_jiffies = jiffies;
		goto XFER_ERROR;
	}

	nvt_diag_update(point_data);

	/* Remove noisy 60Hz poll log to prevent journald rotation loops */
#if NVT_TOUCH_WDT_RECOVERY
	/* ESD protect by WDT */
	if (nvt_wdt_fw_recovery(point_data)) {
		NVT_ERR("Recover for fw reset, %02X\n", point_data[1]);
		nvt_hw_reset_pulse();
		msleep(50);
		nvt_bootloader_reset();
		msleep(10);
		nvt_update_firmware(boot_update_firmware_name);
#if NVT_CUST_PROC_CMD
		nvt_extra_proc_init();
#endif
		mutex_unlock(&ts->lock);
		return IRQ_HANDLED;
	}
#endif /* #if NVT_TOUCH_WDT_RECOVERY */

#if 0
	if (nvt_fw_recovery(point_data)) {
		NVT_ERR("Recover for fw reset, %02X\n", point_data[1]);
		nvt_sw_reset_idle();
		nvt_read_fw_history_all();
		nvt_update_firmware(boot_update_firmware_name);
		nvt_read_fw_history_all();
		mutex_unlock(&ts->lock);
		return IRQ_HANDLED;
	}
#endif

#if POINT_DATA_CHECKSUM
	if ((point_data[1] != 0x00) && (point_data[1] != 0x80)) {
		ret = nvt_ts_point_data_checksum(point_data, POINT_DATA_CHECKSUM_LEN);
		if (ret) {
			NVT_ERR("Packet checksum error\n");
			goto XFER_ERROR;
		}
	}
#endif /* POINT_DATA_CHECKSUM */

#if NVT_DPR_SWITCH
input_id = (uint8_t)(point_data[1] >> 3);
if (nvt_check_stylus_state(input_id, point_data)) {
	goto XFER_ERROR; /* to skip point data parsing */
}
#endif
#if WAKEUP_GESTURE
	if (bTouchIsAwake == 0) {
		input_id = (uint8_t)(point_data[1] >> 3);
		nvt_ts_wakeup_gesture_report(input_id, point_data);
		mutex_unlock(&ts->lock);
		return IRQ_HANDLED;
	}
#endif
	finger_cnt = 0;

	for (i = 0; i < ts->max_touch_num; i++) {
		position = 1 + 6 * i;

		input_id = (uint8_t)(point_data[position + 0] >> 3);
		if ((input_id == 0) || (input_id > ts->max_touch_num))
			continue;

		if (((point_data[position] & 0x07) == 0x01) || ((point_data[position] & 0x07) == 0x02)) {	//finger down (enter & moving)
#if NVT_TOUCH_ESD_PROTECT
			/* update interrupt timer */
			irq_timer = jiffies;
#endif /* #if NVT_TOUCH_ESD_PROTECT */
#if NVT_SUPER_RESOLUTION_N
			input_x = (uint32_t)(point_data[position + 1] << 8) + (uint32_t) (point_data[position + 2]);
			input_y = (uint32_t)(point_data[position + 3] << 8) + (uint32_t) (point_data[position + 4]);
#else /* #if NVT_SUPER_RESOLUTION_N */
			input_x = (uint32_t)(point_data[position + 1] << 4) + (uint32_t) (point_data[position + 3] >> 4);
			input_y = (uint32_t)(point_data[position + 2] << 4) + (uint32_t) (point_data[position + 3] & 0x0F);
#endif /* #if NVT_SUPER_RESOLUTION_N */
			if (input_x > TOUCH_MAX_WIDTH)
				input_x = TOUCH_MAX_WIDTH;
			if (input_y > TOUCH_MAX_HEIGHT)
				input_y = TOUCH_MAX_HEIGHT;
			input_x = TOUCH_MAX_WIDTH - input_x;
			input_y = TOUCH_MAX_HEIGHT - input_y;
#if NVT_SUPER_RESOLUTION_N
			input_w = (uint32_t)(point_data[position + 5]);
			if (input_w == 0)
				input_w = 1;
			input_p = (uint32_t)(point_data[1 + 98 + i]);
			if (input_p == 0)
				input_p = 1;
#else /* #if NVT_SUPER_RESOLUTION_N */
			input_w = (uint32_t)(point_data[position + 4]);
			if (input_w == 0)
				input_w = 1;
			if (i < 2) {
				input_p = (uint32_t)(point_data[position + 5]) + (uint32_t)(point_data[i + 63] << 8);
				if (input_p > TOUCH_FORCE_NUM)
					input_p = TOUCH_FORCE_NUM;
			} else {
				input_p = (uint32_t)(point_data[position + 5]);
			}
			if (input_p == 0)
				input_p = 1;
#endif /* #if NVT_SUPER_RESOLUTION_N */

#if MT_PROTOCOL_B
			press_id[input_id - 1] = 1;
			input_mt_slot(ts->input_dev, input_id - 1);
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);
#else /* MT_PROTOCOL_B */
			input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, input_id - 1);
			input_report_key(ts->input_dev, BTN_TOUCH, 1);
#endif /* MT_PROTOCOL_B */

			input_report_abs(ts->input_dev, ABS_MT_POSITION_X, input_x);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, input_y);
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, input_w);
			input_report_abs(ts->input_dev, ABS_MT_PRESSURE, input_p);

			if (finger_cnt == 0) {
				input_report_abs(ts->input_dev, ABS_X, input_x);
				input_report_abs(ts->input_dev, ABS_Y, input_y);
			}

			finger_cnt++;
		}
	}

	if (finger_cnt > 0 || ts->prev_finger_cnt > 0) {
#if MT_PROTOCOL_B
		for (i = 0; i < ts->max_touch_num; i++) {
			if (press_id[i] != 1) {
				input_mt_slot(ts->input_dev, i);
				input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0);
				input_report_abs(ts->input_dev, ABS_MT_PRESSURE, 0);
				input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);
			}
		}

		input_report_key(ts->input_dev, BTN_TOUCH, (finger_cnt > 0));
#else /* MT_PROTOCOL_B */
		if (finger_cnt == 0) {
			input_report_key(ts->input_dev, BTN_TOUCH, 0);
			input_mt_sync(ts->input_dev);
		}
#endif /* MT_PROTOCOL_B */

#if TOUCH_KEY_NUM > 0
		if (point_data[61] == 0xF8) {
#if NVT_TOUCH_ESD_PROTECT
			/* update interrupt timer */
			irq_timer = jiffies;
#endif /* #if NVT_TOUCH_ESD_PROTECT */
			for (i = 0; i < ts->max_button_num; i++) {
				input_report_key(ts->input_dev, touch_key_array[i], ((point_data[62] >> i) & 0x01));
			}
		} else {
			for (i = 0; i < ts->max_button_num; i++) {
				input_report_key(ts->input_dev, touch_key_array[i], 0);
			}
		}
#endif

		input_sync(ts->input_dev);
		ts->prev_finger_cnt = finger_cnt;
	}

	if (ts->pen_support) {
/*
		//--- dump pen buf ---
		printk("%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
			point_data[66], point_data[67], point_data[68], point_data[69], point_data[70],
			point_data[71], point_data[72], point_data[73], point_data[74], point_data[75],
			point_data[76], point_data[77], point_data[78], point_data[79]);
*/
#if CHECK_PEN_DATA_CHECKSUM
		if (nvt_ts_pen_data_checksum(&point_data[66], PEN_DATA_LEN)) {
			// pen data packet checksum not match, skip it
			goto XFER_ERROR;
		}
#endif // #if CHECK_PEN_DATA_CHECKSUM

		// parse and handle pen report
		pen_format_id = point_data[66];
		if (pen_format_id != 0xFF) {
			if (pen_format_id == 0x01) {
				// report pen data
				pen_x = (uint32_t)(point_data[67] << 8) + (uint32_t)(point_data[68]);
				pen_y = (uint32_t)(point_data[69] << 8) + (uint32_t)(point_data[70]);
				if ((pen_x > PEN_MAX_WIDTH) || (pen_y > PEN_MAX_HEIGHT))
					goto XFER_ERROR;
				pen_x = PEN_MAX_WIDTH - pen_x;
				pen_y = PEN_MAX_HEIGHT - pen_y;
				pen_pressure = (uint32_t)(point_data[71] << 8) + (uint32_t)(point_data[72]);
				pen_tilt_x = (int32_t)point_data[73];
				pen_tilt_y = (int32_t)point_data[74];
				pen_distance = (uint32_t)(point_data[75] << 8) + (uint32_t)(point_data[76]);
				pen_btn1 = (uint32_t)(point_data[77] & 0x01);
				pen_btn2 = (uint32_t)((point_data[77] >> 1) & 0x01);
				pen_battery = (uint32_t)point_data[78];
//				printk("x=%d,y=%d,p=%d,tx=%d,ty=%d,d=%d,b1=%d,b2=%d,bat=%d\n", pen_x, pen_y, pen_pressure,
//						pen_tilt_x, pen_tilt_y, pen_distance, pen_btn1, pen_btn2, pen_battery);

				input_report_abs(ts->pen_input_dev, ABS_X, pen_x);
				input_report_abs(ts->pen_input_dev, ABS_Y, pen_y);
				input_report_abs(ts->pen_input_dev, ABS_PRESSURE, pen_pressure);
				input_report_key(ts->pen_input_dev, BTN_TOUCH, !!pen_pressure);
				input_report_abs(ts->pen_input_dev, ABS_TILT_X, pen_tilt_x);
				input_report_abs(ts->pen_input_dev, ABS_TILT_Y, pen_tilt_y);
				input_report_abs(ts->pen_input_dev, ABS_DISTANCE, pen_distance);
				input_report_key(ts->pen_input_dev, BTN_TOOL_PEN, !!pen_distance || !!pen_pressure);
				input_report_key(ts->pen_input_dev, BTN_STYLUS, pen_btn1);
				input_report_key(ts->pen_input_dev, BTN_STYLUS2, pen_btn2);
				// TBD: pen battery event report
				// NVT_LOG("pen_battery=%d\n", pen_battery);

				// ioctl-DAA Buffering pen raw data
				frame_time = (u16)jiffies_to_msecs(jiffies);
				ppen_info  = (struct lenovo_pen_info *)&pen_buffer[0];
				ppen_info += pen_buffer_wp;
				memset(ppen_info, 0, sizeof(struct lenovo_pen_info));
				ppen_info->coords.status = 1;
				ppen_info->coords.tool_type = DATA_TYPE_RAW;
				ppen_info->coords.tilt_x = pen_tilt_x;
				ppen_info->coords.tilt_y = pen_tilt_y;
				ppen_info->coords.x = pen_x/10;
				ppen_info->coords.y = pen_y/10;
				ppen_info->coords.p = pen_pressure;
				ppen_info->frame_no = pen_frame_no;
				ppen_info->frame_t = frame_time;
				ppen_info->data_type = DATA_TYPE_RAW;
				pen_frame_no++;
				if(MAX_IO_CONTROL_REPORT > pen_report_num) {	// Max count: MAX_IO_CONTROL_REPORT
					pen_report_num++;
				}
				pen_buffer_wp++;
				if(LENOVO_MAX_BUFFER == pen_buffer_wp) {
					pen_buffer_wp = 0;
				}

			} else if (pen_format_id == 0xF0) {
				// report Pen ID
			} else {
				// unhandled / idle pen state, clear pen inputs gracefully without error
				input_report_abs(ts->pen_input_dev, ABS_X, 0);
				input_report_abs(ts->pen_input_dev, ABS_Y, 0);
				input_report_abs(ts->pen_input_dev, ABS_PRESSURE, 0);
				input_report_abs(ts->pen_input_dev, ABS_TILT_X, 0);
				input_report_abs(ts->pen_input_dev, ABS_TILT_Y, 0);
				input_report_abs(ts->pen_input_dev, ABS_DISTANCE, 0);
				input_report_key(ts->pen_input_dev, BTN_TOUCH, 0);
				input_report_key(ts->pen_input_dev, BTN_TOOL_PEN, 0);
				input_report_key(ts->pen_input_dev, BTN_STYLUS, 0);
				input_report_key(ts->pen_input_dev, BTN_STYLUS2, 0);
			}
		} else { // pen_format_id = 0xFF, i.e. no pen present
			input_report_abs(ts->pen_input_dev, ABS_X, 0);
			input_report_abs(ts->pen_input_dev, ABS_Y, 0);
			input_report_abs(ts->pen_input_dev, ABS_PRESSURE, 0);
			input_report_abs(ts->pen_input_dev, ABS_TILT_X, 0);
			input_report_abs(ts->pen_input_dev, ABS_TILT_Y, 0);
			input_report_abs(ts->pen_input_dev, ABS_DISTANCE, 0);
			input_report_key(ts->pen_input_dev, BTN_TOUCH, 0);
			input_report_key(ts->pen_input_dev, BTN_TOOL_PEN, 0);
			input_report_key(ts->pen_input_dev, BTN_STYLUS, 0);
			input_report_key(ts->pen_input_dev, BTN_STYLUS2, 0);
		}

		input_sync(ts->pen_input_dev);
	} /* if (ts->pen_support) */

XFER_ERROR:

	mutex_unlock(&ts->lock);

#if NVT_CUST_PROC_CMD
	if(wdt_recovery) {
		queue_work(nvt_fwu_wq, &ts_restore_cmd_work);
	}
#endif
	return IRQ_HANDLED;
}



static int nvt_ts_thread_func(void *data)
{
	struct nvt_ts_data *ts_data = (struct nvt_ts_data *)data;
	uint64_t prev_spi_errors = 0;
	uint8_t dead_ticks = 0;
	struct sched_param sp = { .sched_priority = 50 };

	/* Run at FIFO RT priority: simpledrm + software rendering can peg
	 * the CPU for hundreds of ms; a CFS poll thread gets starved and
	 * the watchdog fires spurious "touch dead" recoveries (observed 4x
	 * in boot #298). RT keeps frame reads on time regardless of load. */
	sched_setscheduler(current, SCHED_FIFO, &sp);

	while (!kthread_should_stop()) {
		unsigned long now = jiffies;

		nvt_ts_work_func(0, ts_data);
		if (time_after(now, nvt_crash_last_tick + 10 * HZ)) {
			u8 rv[1] = {0};
			unsigned int base = nvt_poll_base ? nvt_poll_base
							   : ts->mmap->EVENT_BUF_ADDR;
			bool dead = false;

			nvt_crash_last_tick = now;
			mutex_lock(&ts->lock);
			nvt_reg_rd(base + 0x60, rv, 1);

			/*
			 * Watchdog: ONLY detect genuine SPI bus communication failures.
			 * Idle periods without user touch input are completely normal
			 * and must never trigger a hardware reset or firmware re-flash.
			 */
			if (ts->diag_spi_errors > prev_spi_errors) {
				dead_ticks++;
				dead = true;
			} else {
				dead_ticks = 0;
			}
			prev_spi_errors = ts->diag_spi_errors;

			nvt_crash_append("NVT diag boot=%lus reset=0x%02X "
					 "dma=%02X/%02X/%02X dead=%d/%d\n",
					 (ulong)(jiffies_to_msecs(now) / 1000UL),
					 rv[0],
					 nvt_reg_rd_as_u8(0x3F7D0),
					 nvt_reg_rd_as_u8(0x3F7F1),
					 nvt_reg_rd_as_u8(0x3F7E8),
					 dead_ticks, dead);
			nvt_set_page(base);

			if (dead_ticks >= 2) {
				/* Do NOT destroy the running firmware by pulsing HW reset or entering bootloader.
				 * Simply re-assert the active scan page and clear error counters. */
				NVT_LOG("watchdog: transient error tick, re-asserting event page\n");
				dead_ticks = 0;
				prev_spi_errors = 0;
				nvt_set_page(base);
			}
			mutex_unlock(&ts->lock);
		}
		if (nvt_poll_ms > 0)
			msleep(nvt_poll_ms);
		else
			usleep_range(16000, 20000);
	}
	return 0;
}

/* Called by Boot_Update_Firmware once the MCU is armed (downloaded or already
 * running). The poll thread must NOT start before that: probing SPI on a cold
 * chip hangs the board pre-fs. nvt_poll_report defaults on. */
void nvt_poll_start(struct nvt_ts_data *ts_data)
{
	if (!ts_data || nvt_poll_ms <= 0 || ts_data->poll_task)
		return;
	ts_data->poll_task = kthread_run(nvt_ts_thread_func, ts_data, "nvt-ts-poll");
	if (IS_ERR(ts_data->poll_task)) {
		NVT_ERR("failed to start poll kthread\n");
		ts_data->poll_task = NULL;
	} else {
		NVT_LOG("poll kthread started (interval=%dms)\n", nvt_poll_ms);
	}
}


/*******************************************************
Description:
	Novatek touchscreen check chip version trim function.

return:
	Executive outcomes. 0---NVT IC. -1---not NVT IC.
*******************************************************/
static __maybe_unused int32_t nvt_ts_check_chip_ver_trim(struct nvt_ts_hw_reg_addr_info hw_regs)
{
	uint8_t buf[8] = {0};
	int32_t retry = 0;
	int32_t list = 0;
	int32_t i = 0;
	int32_t found_nvt_chip = 0;
	int32_t ret = -1;
	uint8_t enb_casc = 0;

	/* hw reg mapping */
	ts->chip_ver_trim_addr = hw_regs.chip_ver_trim_addr;
	ts->swrst_sif_addr = hw_regs.swrst_sif_addr;
	ts->crc_err_flag_addr = hw_regs.crc_err_flag_addr;

	NVT_LOG("check chip ver trim with chip_ver_trim_addr=0x%06x, "
			"swrst_sif_addr=0x%06x, crc_err_flag_addr=0x%06x\n",
			ts->chip_ver_trim_addr, ts->swrst_sif_addr, ts->crc_err_flag_addr);

	//---Check for 5 times---
	for (retry = 5; retry > 0; retry--) {

		nvt_bootloader_reset();

		nvt_set_page(ts->chip_ver_trim_addr);

		buf[0] = ts->chip_ver_trim_addr & 0x7F;
		buf[1] = 0x00;
		buf[2] = 0x00;
		buf[3] = 0x00;
		buf[4] = 0x00;
		buf[5] = 0x00;
		buf[6] = 0x00;
		CTP_SPI_WRITE(ts->client, buf, 7);

		buf[0] = ts->chip_ver_trim_addr & 0x7F;
		buf[1] = 0x00;
		buf[2] = 0x00;
		buf[3] = 0x00;
		buf[4] = 0x00;
		buf[5] = 0x00;
		buf[6] = 0x00;
		CTP_SPI_READ(ts->client, buf, 7);
		NVT_LOG("buf[1]=0x%02X, buf[2]=0x%02X, buf[3]=0x%02X, buf[4]=0x%02X, buf[5]=0x%02X, buf[6]=0x%02X\n",
			buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);

		// compare read chip id on supported list
		for (list = 0; list < (sizeof(trim_id_table) / sizeof(struct nvt_ts_trim_id_table)); list++) {
			found_nvt_chip = 0;

			// compare each byte
			for (i = 0; i < NVT_ID_BYTE_MAX; i++) {
				if (trim_id_table[list].mask[i]) {
					if (buf[i + 1] != trim_id_table[list].id[i])
						break;
				}
			}

			if (i == NVT_ID_BYTE_MAX) {
				found_nvt_chip = 1;
			}

			if (found_nvt_chip) {
				NVT_LOG("This is NVT touch IC\n");
				if (trim_id_table[list].mmap->ENB_CASC_REG.addr) {
					/* check single or cascade */
					nvt_read_reg(trim_id_table[list].mmap->ENB_CASC_REG, &enb_casc);
					/* NVT_LOG("ENB_CASC=0x%02X\n", enb_casc); */
					if (enb_casc & 0x01) {
						NVT_LOG("Single Chip\n");
						ts->mmap = trim_id_table[list].mmap;
						ts->is_cascade = false;
					} else {
						NVT_LOG("Cascade Chip\n");
						ts->mmap = trim_id_table[list].mmap_casc;
						ts->is_cascade = true;
					}
				} else {
					/* for chip that do not have ENB_CASC */
					ts->mmap = trim_id_table[list].mmap;
				}
				ts->hw_crc = trim_id_table[list].hwinfo->hw_crc;
				ts->auto_copy = trim_id_table[list].hwinfo->auto_copy;

				/* hw reg re-mapping */
				ts->chip_ver_trim_addr = trim_id_table[list].hwinfo->hw_regs->chip_ver_trim_addr;
				ts->swrst_sif_addr = trim_id_table[list].hwinfo->hw_regs->swrst_sif_addr;
				ts->crc_err_flag_addr = trim_id_table[list].hwinfo->hw_regs->crc_err_flag_addr;

				NVT_LOG("set reg chip_ver_trim_addr=0x%06x, "
						"swrst_sif_addr=0x%06x, crc_err_flag_addr=0x%06x\n",
						ts->chip_ver_trim_addr, ts->swrst_sif_addr, ts->crc_err_flag_addr);

				ret = 0;
				goto out;
			} else {
				ts->mmap = NULL;
				ret = -1;
			}
		}

		msleep(10);
	}

out:
	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen check chip version trim loop
	function. Check chip version trim via hw regs table.

return:
	Executive outcomes. 0---NVT IC. -1---not NVT IC.
*******************************************************/
static __maybe_unused int32_t nvt_ts_check_chip_ver_trim_loop(void) {
    uint8_t i = 0;
	int32_t ret = 0;

	struct nvt_ts_hw_reg_addr_info hw_regs_table[] = {
		hw_reg_addr_info,
		hw_reg_addr_info_old_w_isp,
		hw_reg_addr_info_legacy_w_isp
	};

    for (i = 0; i < (sizeof(hw_regs_table) / sizeof(struct nvt_ts_hw_reg_addr_info)); i++) {
        //---check chip version trim---
        ret = nvt_ts_check_chip_ver_trim(hw_regs_table[i]);
		if (!ret) {
			break;
		}
    }

	if (ret || !ts->mmap) {
		NVT_LOG("Applying guaranteed NT36523N cascade memory map fallback\n");
		ts->mmap = &NT36523N_memory_map;
		ts->is_cascade = true;
		ts->hw_crc = NT36523_hw_info.hw_crc;
		ts->auto_copy = NT36523_hw_info.auto_copy;
		ts->chip_ver_trim_addr = NT36523_hw_info.hw_regs->chip_ver_trim_addr;
		ts->swrst_sif_addr = NT36523_hw_info.hw_regs->swrst_sif_addr;
		ts->crc_err_flag_addr = NT36523_hw_info.hw_regs->crc_err_flag_addr;
		ret = 0;
	}

	return ret;
}

/*******************************************************
Description:
	Identify the actual chip (trim id) and apply its memory map.
	Called at the deferred 60s point (SPI is safe there). Unlike
	nvt_ts_check_chip_ver_trim*, this does NOT bootloader_reset the
	chip, so a self-running MCU is left untouched. If the id matches a
	table entry, ts->mmap/hw_crc/auto_copy are updated to that chip's
	config (the correct EVENT_BUF_ADDR is what makes live touch data
	parseable); otherwise the NT36523N fallback stays and that fact is
	logged to the crash file.
*******************************************************/
void nvt_hw_reset_pulse(void)
{
	nvt_crash_append("NVT hw-reset: rst_gpio=%d valid=%d\n",
			 ts->reset_gpio, gpio_is_valid(ts->reset_gpio));
	if (gpio_is_valid(ts->reset_gpio)) {
		gpio_set_value(ts->reset_gpio, 0);
		msleep(50);
		gpio_set_value(ts->reset_gpio, 1);
		msleep(150);
		nvt_crash_append("NVT hw-reset: pulse sent, MCU rebooting\n");
	} else {
		nvt_crash_append("NVT hw-reset: skipped (no reset gpio)\n");
	}
}

void nvt_identify_chip(void)
{
	uint8_t buf[8] = {0};
	uint32_t trim = ts->chip_ver_trim_addr;
	uint32_t l = 0;
	int32_t i = 0;
	uint8_t enb = 0;
	uint32_t n = sizeof(trim_id_table) / sizeof(struct nvt_ts_trim_id_table);

	if (!ts->client || !ts->mmap)
		return;

	nvt_set_page(trim);
	buf[0] = trim & 0x7F;
	CTP_SPI_WRITE(ts->client, buf, 7);

	buf[0] = trim & 0x7F;
	CTP_SPI_READ(ts->client, buf, 7);
	nvt_crash_append("NVT id read: %02X %02X %02X %02X %02X %02X\n",
			 buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);
	NVT_LOG("chip trim id: %02X %02X %02X %02X %02X %02X\n",
		buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);

	for (l = 0; l < n; l++) {
		for (i = 0; i < NVT_ID_BYTE_MAX; i++) {
			if (trim_id_table[l].mask[i]) {
				if (buf[i + 1] != trim_id_table[l].id[i])
					break;
			}
		}
		if (i == NVT_ID_BYTE_MAX)
			break;
	}

	if (l < n) {
		if (trim_id_table[l].mmap->ENB_CASC_REG.addr) {
			nvt_read_reg(trim_id_table[l].mmap->ENB_CASC_REG, &enb);
			if (enb & 0x01) {
				ts->mmap = trim_id_table[l].mmap;
				ts->is_cascade = false;
			} else {
				ts->mmap = trim_id_table[l].mmap_casc;
				ts->is_cascade = true;
			}
		} else {
			ts->mmap = trim_id_table[l].mmap;
		}
		ts->hw_crc = trim_id_table[l].hwinfo->hw_crc;
		ts->auto_copy = trim_id_table[l].hwinfo->auto_copy;
		nvt_crash_append("NVT chip match #%u EVENT_BUF=0x%06X hw_crc=%d auto_copy=%d\n",
				 l, ts->mmap->EVENT_BUF_ADDR, ts->hw_crc,
				 ts->auto_copy);
		NVT_LOG("chip matched table#%u EVENT_BUF=0x%06X hw_crc=%d auto_copy=%d\n",
			l, ts->mmap->EVENT_BUF_ADDR, ts->hw_crc, ts->auto_copy);
		nvt_crash_append("NVT SPI mode=0x%X max_hz=%u\n",
				 ts->client->mode, ts->client->max_speed_hz);
		NVT_LOG("SPI mode=0x%X max_hz=%u\n",
			ts->client->mode, ts->client->max_speed_hz);
	} else {
		nvt_crash_append("NVT chip id NOT in table, keeping EVENT_BUF=0x%06X\n",
				 ts->mmap->EVENT_BUF_ADDR);
	}
}

#if IS_ENABLED(NVT_DRM_PANEL_NOTIFY) || IS_ENABLED(NVT_QCOM_PANEL_EVENT_NOTIFY)
static int nvt_ts_check_dt(struct device_node *np)
{
	int i;
	int count;
	struct device_node *node;
	struct drm_panel *panel;

	count = of_count_phandle_with_args(np, "panel", NULL);
	if (count <= 0)
		return 0;

	for (i = 0; i < count; i++) {
		node = of_parse_phandle(np, "panel", i);
		panel = of_drm_find_panel(node);
		of_node_put(node);
		if (!IS_ERR(panel)) {
			active_panel = panel;
			return 0;
		}
	}

	extern char *saved_command_line;
	if (saved_command_line && (strstr(saved_command_line, "androidboot.mode=recovery") || strstr(saved_command_line, "recovery"))) {
		pr_info("[NVT] Recovery mode detected, bypassing DRM panel check\n");
		return 0;
	}

	return PTR_ERR(panel);
}
#endif


#if NVT_CUST_PROC_CMD
static void nvt_restore_cmd_func(struct work_struct *work){
	int ret = 0;
	NVT_LOG("%s++\n", __func__);
	if (bTouchIsAwake) {
#if NVT_EDGE_REJECT
	nvt_edge_reject_set(ts->edge_reject_state);
#endif
#if NVT_EDGE_GRID_ZONE
	nvt_edge_grid_zone_set(ts->edge_grid_zone_info.degree, ts->edge_grid_zone_info.direction, ts->edge_grid_zone_info.y1, ts->edge_grid_zone_info.y2);
#endif
#if NVT_PALM_MODE
	nvt_game_mode_set(ts->game_mode_state);  
#endif
#if NVT_SUPPORT_PEN
	nvt_support_pen_set(ts->pen_state);
#endif
		ts->fw_pen_state= 0;
#if NVT_CHARGER_NOTIFIER_CALLBACK
	if (ts->usb_plug_status) {
		NVT_LOG("enter charger mode");
		ret = nvt_set_charger(USB_DETECT_IN);
		if (ret < 0) {
			NVT_ERR("enter charger mode fail ! ret = %d", ret);
		}
	} else if (!(ts->usb_plug_status)) {
		NVT_LOG("exit charger mode");
		ret = nvt_set_charger(USB_DETECT_OUT);
		if (ret < 0) {
			NVT_ERR("exit charger mode fail ! ret = %d", ret);
		}
	}
#endif
	} else {
		NVT_LOG("Touch is already suspend, skip restore cmd\n");
	}
	NVT_LOG("%s--\n", __func__);
}
#endif

static const struct mtk_chip_config nvt_spidata = {
	.cs_setuptime = 25,
	.cs_holdtime = 25,
	/* Chip select setup/hold time = (COUNT + 1) * CLK_PERIOD,
	where CLK_PERIOD is the cycle time of the clock SPI engine adopts.
	25 matches Android's vendor setuptime/holdtime for NT36xxx on mt6765. */
};
#if NVT_CHARGER_NOTIFIER_CALLBACK
static int nvt_charger_notifier_callback(struct notifier_block *nb, unsigned long val, void *v)
{
	int ret = 0;
	struct power_supply *psy = NULL;
	union power_supply_propval prop;
	psy = power_supply_get_by_name("mtk-master-charger");
	if (IS_ERR_OR_NULL(psy)) {
		return -EINVAL;
	}
	if (!psy) {
		NVT_ERR("Couldn't get usbpsy\n");
		return -EINVAL;
	}
	if (!(strcmp(psy->desc->name, "mtk-master-charger"))) {
		if (psy && val == POWER_SUPPLY_PROP_STATUS) {
			ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE, &prop);
			if (ret < 0) {
				NVT_LOG("Couldn't get POWER_SUPPLY_PROP_ONLINE rc=%d\n", ret);
				return ret;
			} else {
				if (ts->usb_plug_status == 2)
					ts->usb_plug_status = prop.intval;
				if (ts->usb_plug_status != prop.intval) {
					NVT_LOG("usb prop.intval =%d psy->desc->name =%s\n",
							prop.intval, psy->desc->name);
					ts->usb_plug_status = prop.intval;
					if (bTouchIsAwake && (ts->charger_notify_wq != NULL))
						queue_work(ts->charger_notify_wq, &ts->update_charger);
				}
			}
		}
	}
	return 0;
}
static void nvt_update_charger(struct work_struct *work)
{
	int ret = 0;
	if (ts->usb_plug_status) {
		NVT_LOG("enter charger mode");
		ret = nvt_set_charger(USB_DETECT_IN);
		if (ret < 0) {
			NVT_ERR("enter charger mode fail ! ret = %d", ret);
		}
	} else if (!(ts->usb_plug_status)) {
		NVT_LOG("exit charger mode");
		ret = nvt_set_charger(USB_DETECT_OUT);
		if (ret < 0) {
			NVT_ERR("exit charger mode fail ! ret = %d", ret);
		}
	}
}
int nvt_plat_charger_init(void)
{
	int ret = 0;
	ts->usb_plug_status = 2;
	ts->charger_notify_wq = create_singlethread_workqueue("nvt_charger_wq");
	if (!ts->charger_notify_wq) {
		NVT_ERR("allocate nvt_charger_notify_wq failed\n");
		return 1;
	}
	INIT_WORK(&ts->update_charger, nvt_update_charger);
	ts->notifier_charger.notifier_call = nvt_charger_notifier_callback;
	ret = power_supply_reg_notifier(&ts->notifier_charger);
	if (ret < 0) {
		NVT_ERR("power_supply_reg_notifier failed\n");
		return 1;
	}
	return 0;
}
#endif

static int nvt_get_panel(struct nvt_ts_data *ts)
{
	int panel_id0 = 0;
	int panel_id1 = 0;

	if (ts->id0_gpio >= 0)
		panel_id0 = gpio_get_value(ts->id0_gpio);

	if (ts->id1_gpio >= 0)
		panel_id1 = gpio_get_value(ts->id1_gpio);

	NVT_LOG(" DT: panel_id0=%d, panel_id1=%d\n", panel_id0, panel_id1);

	if ((panel_id0 == 0) && (panel_id1 == 0)) {
		NVT_LOG(" DT: panel is boe\n");
		strncpy(ts->panel_name, NVT_PANEL_BOE, sizeof(ts->panel_name));
		boot_update_firmware_name = "NOVATEK_TS_FW_BOE.bin";
		mp_update_firmware_name = "NOVATEK_TS_MP_BOE.bin";

	} else {
		NVT_LOG(" DT: panel is csot\n");
		strncpy(ts->panel_name, NVT_PANEL_CSOT, sizeof(ts->panel_name));
		boot_update_firmware_name = "NOVATEK_TS_FW_CSOT.bin";
                mp_update_firmware_name = "NOVATEK_TS_MP_CSOT.bin";
	}

	NVT_LOG(" DT: fw name=%s, mp name=%s\n", boot_update_firmware_name, mp_update_firmware_name);
	if (ts->id0_gpio >= 0)
		gpio_free(ts->id0_gpio);
	if (ts->id1_gpio >= 0)
		gpio_free(ts->id1_gpio);

	return 0;
}
#if IS_ENABLED(CONFIG_TINNO_DEVINFO)
static int nvt_get_tp_info(char *buf, void *arg0)
{
    return sprintf(buf,"%s-%s-%s-0X%02x\n",
        ts->panel_name, "T808AA", "NT36523N", ts->fw_ver);
}
#endif
/*******************************************************
Description:
	Novatek touchscreen driver probe function.

return:
	Executive outcomes. 0---succeed. negative---failed
*******************************************************/
static int32_t nvt_ts_probe(struct spi_device *client)
{
	int32_t ret = 0;
#if IS_ENABLED(NVT_DRM_PANEL_NOTIFY) || IS_ENABLED(NVT_QCOM_PANEL_EVENT_NOTIFY)
	struct device_node *dp = NULL;
#endif
#if IS_ENABLED(NVT_QCOM_PANEL_EVENT_NOTIFY)
	void *cookie = NULL;
#endif
#if ((TOUCH_KEY_NUM > 0) || WAKEUP_GESTURE)
	int32_t retry = 0;
#endif

#if IS_ENABLED(NVT_DRM_PANEL_NOTIFY) || IS_ENABLED(NVT_QCOM_PANEL_EVENT_NOTIFY)
	dp = client->dev.of_node;

	ret = nvt_ts_check_dt(dp);
	if (ret == -EPROBE_DEFER) {
		return ret;
	}

	if (ret) {
		ret = -ENODEV;
		return ret;
	}
#endif

	NVT_LOG("start\n");

	nvt_crashlog_init();

	ts = (struct nvt_ts_data *)kzalloc(sizeof(struct nvt_ts_data), GFP_KERNEL);
	if (ts == NULL) {
		NVT_ERR("failed to allocated memory for nvt ts data\n");
		return -ENOMEM;
	}

	ts->xbuf = (uint8_t *)kzalloc(NVT_XBUF_LEN, GFP_KERNEL);
	if (ts->xbuf == NULL) {
		NVT_ERR("kzalloc for xbuf failed!\n");
		ret = -ENOMEM;
		goto err_malloc_xbuf;
	}

	ts->rbuf = (uint8_t *)kzalloc(NVT_XBUF_LEN, GFP_KERNEL);
	if(ts->rbuf == NULL) {
		NVT_ERR("kzalloc for rbuf failed!\n");
		ret = -ENOMEM;
		goto err_malloc_rbuf;
	}

#if NVT_PM_WAIT_BUS_RESUME_COMPLETE
	ts->dev_pm_suspend = false;
	init_completion(&ts->dev_pm_resume_completion);
#endif /* NVT_PM_WAIT_BUS_RESUME_COMPLETE */

	ts->client = client;
	spi_set_drvdata(client, ts);

	//---prepare for spi parameter---
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	if (ts->client->controller->flags & SPI_CONTROLLER_HALF_DUPLEX) {
		NVT_ERR("Full duplex not supported by controller\n");
#else
	if (ts->client->master->flags & SPI_MASTER_HALF_DUPLEX) {
		NVT_ERR("Full duplex not supported by master\n");
#endif
		ret = -EIO;
		goto err_ckeck_full_duplex;
	}
	ts->client->bits_per_word = 8;
	ts->client->mode = SPI_MODE_0;
	ts->client->controller_data = (void *)&nvt_spidata;

#ifdef CONFIG_MTK_SPI
	/* old usage of MTK spi API */
	memcpy(&ts->spi_ctrl, &spi_ctrdata, sizeof(struct mt_chip_conf));
	ts->client->controller_data = (void *)&ts->spi_ctrl;
#endif

#ifdef CONFIG_SPI_MT65XX
	/* new usage of MTK spi API */
	memcpy(&ts->spi_ctrl, &spi_ctrdata, sizeof(struct mtk_chip_config));
	ts->client->controller_data = (void *)&ts->spi_ctrl;
#endif

	/*
	 * Force CS setup/hold timing in the struct mtk_chip_config that the
	 * upstream spi-mt65xx driver reads via controller_data.  Match the
	 * WORKING Android register state exactly: CFG0 low16 CS_HOLD=0x13(19),
	 * high16 CS_SETUP=0.  (25-1=24 was wrong; Android uses hold=0x13, no
	 * setup.)  cs_holdtime=20 -> (20-1)=19=0x13; cs_setuptime=0 -> CFG0
	 * high16 stays 0.
	 */
	{
		struct mtk_chip_config *cd =
			(struct mtk_chip_config *)ts->client->controller_data;
		cd->cs_setuptime = 0;
		cd->cs_holdtime = 20;
	}

	ts->client->bits_per_word = 8;

	/* NT36523N TDDI speaks MSB-first SPI.  Ground-truth register dump of the
	 * WORKING Android driver shows CMD SPI_CMD_TXMSBF|RXMSBF set (0x00033c10),
	 * i.e. SPI_CMD_TXMSBF=1 AND RXMSBF=1 -> MSB-first.  SPI_MODE_0 (no
	 * SPI_LSB_FIRST) keeps spi-mt65xx from clearing TXMSBF/RXMSBF, so bytes
	 * are clocked MSB-first exactly as the chip expects.  (An earlier comment
	 * claimed LSB-first; that was wrong and bit-reversed every byte sent.) */
	ts->client->mode = SPI_MODE_0;

	/* Use DT-specified SPI speed (9.6MHz for this panel).  Previous
	 *3MHz cap was for cadence experiments on SRAM writes — reads are
	 * clean at 9.6MHz since the DT is tuned for this TDDI. */

	ret = spi_setup(ts->client);
	if (ret < 0) {
		NVT_ERR("Failed to perform SPI setup\n");
		goto err_spi_setup;
	}

	NVT_LOG("mode=%d, max_speed_hz=%d\n", ts->client->mode, ts->client->max_speed_hz);

	//---parse dts---
	ret = nvt_parse_dt(&client->dev);
	if (ret) {
		NVT_ERR("parse dt error\n");
		goto err_spi_setup;
	}

	ret = nvt_get_panel(ts);
	if (ret) {
		NVT_ERR("get panel error\n");
		goto err_spi_setup;
	}
	//---request and config GPIOs---
	ret = nvt_gpio_config(ts);
	if (ret) {
		NVT_ERR("gpio config error!\n");
		goto err_gpio_config_failed;
	}

	mutex_init(&ts->lock);
	mutex_init(&ts->xbuf_lock);

	// need sufficient delay after power on for IC to boot
	msleep(200);

	/*
	 * EXPERIMENT: mirror Android's probe chip-boot exactly.  Android runs
	 * nvt_eng_reset() (SPI cmd to ENG_RST_ADDR) BEFORE pulling reset high,
	 * raises RST, waits 10ms, then nvt_ts_check_chip_ver_trim_loop() which
	 * resets the MCU into a known idle state and identifies the chip from the
	 * trim register.  pmOS previously SKIPPED all of this assuming LK already
	 * booted the TDDI; the result is the MCU sitting in bootloader/undefined
	 * state so its committed app fw (ver 0x19) never runs and fwinfo reads
	 * garbage.  Run the real boot sequence here.
	 */
	nvt_eng_reset();
#if NVT_TOUCH_SUPPORT_HW_RST
	gpio_set_value(ts->reset_gpio, 1);
#endif
	msleep(10);
	ret = nvt_ts_check_chip_ver_trim_loop();
	NVT_LOG("chip-boot trim loop ret=%d (mmap=%s)\n", ret,
		ts->mmap == &NT36523N_memory_map ? "NT36523N" : "other");

	if (!ret) {
		/* trim identified the chip properly (Android path) */
	} else {
		NVT_LOG("trim loop failed; applying NT36523N hardcoded map\n");
		ts->mmap = &NT36523N_memory_map;
	}

	// Preserve whatever map trim selected as the running TDDI memory map
	if (!ts->mmap)
		ts->mmap = &NT36523N_memory_map;
	ts->hw_crc = HWCRC_LEN_3Bytes;
	ts->auto_copy = CHECK_SPI_DMA_TX_INFO;
	ts->chip_ver_trim_addr = 0x03F004;
	ts->swrst_sif_addr = 0x03F0FE;
	ts->crc_err_flag_addr = 0x03F135;
	ts->is_cascade = true;
	NVT_LOG("Using persistent LittleKernel TDDI runtime state for NT36523\n");

	/* Force MT6789 hardware pinmux, IES (Input Enable) & SMT for SPI2 (38..41), EINT9 (pin 9), and RST (pin 36) */
	{
		void __iomem *gpio_base = ioremap(0x10005000, 0x1000);
		void __iomem *iocfg_lm  = ioremap(0x11E20000, 0x1000);
		void __iomem *iocfg_rt  = ioremap(0x11EA0000, 0x1000);

		if (gpio_base) {
			u32 val;
			/* Pin 9 -> Mode 0 (EINT9) */
			val = readl(gpio_base + 0x0310);
			val &= ~(0xFU << 4);
			writel(val, gpio_base + 0x0310);

			/* Pin 36 -> Mode 0 (GPIO36), Output DIR=1, Data DO=1 (RST Deasserted) */
			val = readl(gpio_base + 0x0340);
			val &= ~(0xFU << 16);
			writel(val, gpio_base + 0x0340);

			val = readl(gpio_base + 0x0010);
			val |= (1U << 4);
			writel(val, gpio_base + 0x0010);

			val = readl(gpio_base + 0x0110);
			val |= (1U << 4);
			writel(val, gpio_base + 0x0110);

			/* Pins 38, 39 -> Mode 1 (SPI2_CLK, SPI2_CSB) */
			val = readl(gpio_base + 0x0340);
			val &= ~((0xFU << 24) | (0xFU << 28));
			val |= (1U << 24) | (1U << 28);
			writel(val, gpio_base + 0x0340);

			/* Pins 40, 41 -> Mode 1 (SPI2_MO, SPI2_MI) */
			val = readl(gpio_base + 0x0350);
			val &= ~((0xFU << 0) | (0xFU << 4));
			val |= (1U << 0) | (1U << 4);
			writel(val, gpio_base + 0x0350);

			iounmap(gpio_base);
		}

		if (iocfg_lm) {
			u32 val;
			/* Enable IES (Input Enable Schmitt Trigger) for Pins 38..41 */
			val = readl(iocfg_lm + 0x00E0);
			val |= (1U << 16) | (1U << 17) | (1U << 18) | (1U << 19);
			writel(val, iocfg_lm + 0x00E0);

			/* Enable SMT for Pins 38..41 */
			val = readl(iocfg_lm + 0x01F0);
			val |= (1U << 13) | (1U << 14) | (1U << 15) | (1U << 16);
			writel(val, iocfg_lm + 0x01F0);

			iounmap(iocfg_lm);
		}

		if (iocfg_rt) {
			u32 val;
			/* Enable IES for Pin 9 (EINT9) */
			val = readl(iocfg_rt + 0x0080);
			val |= (1U << 0);
			writel(val, iocfg_rt + 0x0080);

			iounmap(iocfg_rt);
		}

		NVT_LOG("Forced MT6789 SPI2 (pins 38-41), EINT9 (pin 9), and RST (pin 36) IES/SMT/Pinmux\n");
	}

	//---allocate input device---
	ts->input_dev = input_allocate_device();
	if (ts->input_dev == NULL) {
		NVT_ERR("allocate input device failed\n");
		ret = -ENOMEM;
		goto err_input_dev_alloc_failed;
	}

	ts->max_touch_num = TOUCH_MAX_FINGER_NUM;

#if TOUCH_KEY_NUM > 0
	ts->max_button_num = TOUCH_KEY_NUM;
#endif

	ts->int_trigger_type = INT_TRIGGER_TYPE;


	//---set input device info.---
	ts->input_dev->evbit[0] = BIT_MASK(EV_SYN) | BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	ts->input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
	ts->input_dev->propbit[0] = BIT(INPUT_PROP_DIRECT);

#if MT_PROTOCOL_B
	input_mt_init_slots(ts->input_dev, ts->max_touch_num, 0);
#endif

	input_set_abs_params(ts->input_dev, ABS_MT_PRESSURE, 0, TOUCH_FORCE_NUM, 0, 0);    //pressure = TOUCH_FORCE_NUM

#if TOUCH_MAX_FINGER_NUM > 1
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);    //area = 255

	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0, TOUCH_MAX_WIDTH, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0, TOUCH_MAX_HEIGHT, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_X, 0, TOUCH_MAX_WIDTH, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_Y, 0, TOUCH_MAX_HEIGHT, 0, 0);
	input_abs_set_res(ts->input_dev, ABS_MT_POSITION_X, 40);
	input_abs_set_res(ts->input_dev, ABS_MT_POSITION_Y, 40);
	input_abs_set_res(ts->input_dev, ABS_X, 40);
	input_abs_set_res(ts->input_dev, ABS_Y, 40);
#if MT_PROTOCOL_B
	// no need to set ABS_MT_TRACKING_ID, input_mt_init_slots() already set it
#else
	input_set_abs_params(ts->input_dev, ABS_MT_TRACKING_ID, 0, ts->max_touch_num, 0, 0);
#endif //MT_PROTOCOL_B
#endif //TOUCH_MAX_FINGER_NUM > 1

#if TOUCH_KEY_NUM > 0
	for (retry = 0; retry < ts->max_button_num; retry++) {
		input_set_capability(ts->input_dev, EV_KEY, touch_key_array[retry]);
	}
#endif

#if WAKEUP_GESTURE
	for (retry = 0; retry < (sizeof(gesture_key_array) / sizeof(gesture_key_array[0])); retry++) {
		input_set_capability(ts->input_dev, EV_KEY, gesture_key_array[retry]);
	}
#endif

	sprintf(ts->phys, "input/ts");
	ts->input_dev->name = NVT_TS_NAME;
	ts->input_dev->phys = ts->phys;
	ts->input_dev->id.bustype = BUS_SPI;
	ts->input_dev->dev.parent = &client->dev;

	//---register input device---
	ret = input_register_device(ts->input_dev);
	if (ret) {
		NVT_ERR("register input device (%s) failed. ret=%d\n", ts->input_dev->name, ret);
		goto err_input_register_device_failed;
	}

	if (ts->pen_support) {
		//---allocate pen input device---
		ts->pen_input_dev = input_allocate_device();
		if (ts->pen_input_dev == NULL) {
			NVT_ERR("allocate pen input device failed\n");
			ret = -ENOMEM;
			goto err_pen_input_dev_alloc_failed;
		}

		//---set pen input device info.---
		ts->pen_input_dev->evbit[0] = BIT_MASK(EV_SYN) | BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
		ts->pen_input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
		ts->pen_input_dev->keybit[BIT_WORD(BTN_TOOL_PEN)] |= BIT_MASK(BTN_TOOL_PEN);
		ts->pen_input_dev->keybit[BIT_WORD(KEY_STYLUS)] |= BIT_MASK(KEY_STYLUS);
		//ts->pen_input_dev->keybit[BIT_WORD(BTN_TOOL_RUBBER)] |= BIT_MASK(BTN_TOOL_RUBBER);
		ts->pen_input_dev->keybit[BIT_WORD(BTN_STYLUS)] |= BIT_MASK(BTN_STYLUS);
		ts->pen_input_dev->keybit[BIT_WORD(BTN_STYLUS2)] |= BIT_MASK(BTN_STYLUS2);
		ts->pen_input_dev->propbit[0] = BIT(INPUT_PROP_DIRECT);

		input_set_abs_params(ts->pen_input_dev, ABS_X, 0, PEN_MAX_WIDTH, 0, 0);
		input_set_abs_params(ts->pen_input_dev, ABS_Y, 0, PEN_MAX_HEIGHT, 0, 0);
		input_abs_set_res(ts->pen_input_dev, ABS_X, 40);
		input_abs_set_res(ts->pen_input_dev, ABS_Y, 40);
		input_set_abs_params(ts->pen_input_dev, ABS_PRESSURE, 0, PEN_PRESSURE_MAX, 0, 0);
		input_set_abs_params(ts->pen_input_dev, ABS_DISTANCE, 0, PEN_DISTANCE_MAX, 0, 0);
		input_set_abs_params(ts->pen_input_dev, ABS_TILT_X, PEN_TILT_MIN, PEN_TILT_MAX, 0, 0);
		input_set_abs_params(ts->pen_input_dev, ABS_TILT_Y, PEN_TILT_MIN, PEN_TILT_MAX, 0, 0);

		sprintf(ts->pen_phys, "input/pen");
		ts->pen_input_dev->name = NVT_PEN_NAME;
		ts->pen_input_dev->phys = ts->pen_phys;
		ts->pen_input_dev->id.bustype = BUS_SPI;
		ts->pen_input_dev->dev.parent = &client->dev;

		//---register pen input device---
		ret = input_register_device(ts->pen_input_dev);
		if (ret) {
			NVT_ERR("register pen input device (%s) failed. ret=%d\n", ts->pen_input_dev->name, ret);
			goto err_pen_input_register_device_failed;
		}
	} /* if (ts->pen_support) */

	//---set int-pin & request irq---
	client->irq = gpio_to_irq(ts->irq_gpio);
	if (client->irq) {
		NVT_LOG("int_trigger_type=%d\n", ts->int_trigger_type);
		ts->irq_enabled = true;
		ret = request_threaded_irq(client->irq, NULL, nvt_ts_work_func,
				ts->int_trigger_type | IRQF_ONESHOT, NVT_SPI_NAME, ts);
		if (ret != 0) {
			NVT_ERR("request irq failed. ret=%d\n", ret);
			goto err_int_request_failed;
		} else {
			nvt_irq_enable(false);
			NVT_LOG("request irq %d succeed\n", client->irq);
		}
	}

#if WAKEUP_GESTURE
	device_init_wakeup(&ts->client->dev, 1);
#endif

	nvt_fwu_wq = alloc_workqueue("nvt_fwu_wq", WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!nvt_fwu_wq) {
		NVT_ERR("nvt_fwu_wq create workqueue failed\n");
		ret = -ENOMEM;
		goto err_create_nvt_fwu_wq_failed;
	}
#if BOOT_UPDATE_FIRMWARE
	INIT_DELAYED_WORK(&ts->nvt_fwu_work, Boot_Update_Firmware);
	/* Re-enable firmware load: on this boot path (simpledrm/U-Boot) LittleKernel
	 * never initializes the TDDI MCU, so the driver must load the active-mode
	 * firmware itself. Boot_Update_Firmware() skips safely if fw is already running.
	 * Match Android's timing (probe + 7s, after display RESX): the TDDI parks in
	 * the bootloader at the start of Boot_Update_Firmware; deferring much later
	 * lets the chip auto-boot its internal read-only firmware so host writes are
	 * rejected and the download can never commit. */
	queue_delayed_work(nvt_fwu_wq, &ts->nvt_fwu_work, msecs_to_jiffies(7000));
#endif

	NVT_LOG("NVT_TOUCH_ESD_PROTECT is %d\n", NVT_TOUCH_ESD_PROTECT);
#if NVT_TOUCH_ESD_PROTECT
	INIT_DELAYED_WORK(&nvt_esd_check_work, nvt_esd_check_func);
	nvt_esd_check_wq = alloc_workqueue("nvt_esd_check_wq", WQ_MEM_RECLAIM, 1);
	if (!nvt_esd_check_wq) {
		NVT_ERR("nvt_esd_check_wq create workqueue failed\n");
		ret = -ENOMEM;
		goto err_create_nvt_esd_check_wq_failed;
	}
	queue_delayed_work(nvt_esd_check_wq, &nvt_esd_check_work,
			msecs_to_jiffies(NVT_TOUCH_ESD_CHECK_PERIOD));
#endif /* #if NVT_TOUCH_ESD_PROTECT */

	//---set device node---
#if NVT_TOUCH_PROC
	ret = nvt_flash_proc_init();
	if (ret != 0) {
		NVT_ERR("nvt flash proc init failed. ret=%d\n", ret);
		goto err_flash_proc_init_failed;
	}
#endif

#if NVT_TOUCH_EXT_PROC
	ret = nvt_extra_proc_init();
	if (ret != 0) {
		NVT_ERR("nvt extra proc init failed. ret=%d\n", ret);
		goto err_extra_proc_init_failed;
	}
#endif

#if NVT_TOUCH_MP
	ret = nvt_mp_proc_init();
	if (ret != 0) {
		NVT_ERR("nvt mp proc init failed. ret=%d\n", ret);
		goto err_mp_proc_init_failed;
	}
#endif

#if NVT_CHARGER_NOTIFIER_CALLBACK
    ret = nvt_plat_charger_init();
    if (ret) {
        NVT_ERR("nvt_plat_charger_init fail");
    }
#endif
#if IS_ENABLED(NVT_DRM_PANEL_NOTIFY)
	ts->drm_panel_notif.notifier_call = nvt_drm_panel_notifier_callback;
	if (active_panel) {
		ret = drm_panel_notifier_register(active_panel, &ts->drm_panel_notif);
		if (ret < 0) {
			NVT_ERR("register drm_panel_notifier failed. ret=%d\n", ret);
			goto err_register_suspend_resume_failed;
		}
	}
#elif IS_ENABLED(NVT_MSM_DRM_NOTIFY)
	ts->drm_notif.notifier_call = nvt_drm_notifier_callback;
	ret = msm_drm_register_client(&ts->drm_notif);
	if(ret) {
		NVT_ERR("register drm_notifier failed. ret=%d\n", ret);
		goto err_register_suspend_resume_failed;
	}
#elif IS_ENABLED(NVT_FB_NOTIFY)
	ts->fb_notif.notifier_call = nvt_fb_notifier_callback;
	ret = fb_register_client(&ts->fb_notif);
	if(ret) {
		NVT_ERR("register fb_notifier failed. ret=%d\n", ret);
		goto err_register_suspend_resume_failed;
	}
#elif IS_ENABLED(NVT_EARLYSUSPEND_NOTIFY)
	ts->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	ts->early_suspend.suspend = nvt_ts_early_suspend;
	ts->early_suspend.resume = nvt_ts_late_resume;
	ret = register_early_suspend(&ts->early_suspend);
	if(ret) {
		NVT_ERR("register early suspend failed. ret=%d\n", ret);
		goto err_register_suspend_resume_failed;
	}
#elif IS_ENABLED(NVT_QCOM_PANEL_EVENT_NOTIFY)
	if (active_panel) {
		cookie = panel_event_notifier_register(PANEL_EVENT_NOTIFICATION_PRIMARY,
				PANEL_EVENT_NOTIFIER_CLIENT_PRIMARY_TOUCH, active_panel,
				&nvt_panel_notifier_callback, ts);
	}

	if (!cookie) {
		NVT_ERR("register qcom panel_event_notifier failed\n");
		goto err_register_suspend_resume_failed;
	}

	notifier_cookie = cookie;
#elif IS_ENABLED(NVT_MTK_DRM_NOTIFY)
    ts->disp_notifier.notifier_call = nvt_mtk_drm_notifier_callback;
	ret = mtk_disp_notifier_register(NVT_SPI_NAME, &ts->disp_notifier);
    if (ret) {
        NVT_ERR("register mtk_disp_notifier failed\n");
        goto err_register_suspend_resume_failed;
    }
#endif

	bTouchIsAwake = 1;

#if NVT_CUST_PROC_CMD
	ts->edge_reject_state = 0;
	ts->game_mode_state = 0;
	ts->pen_state = 0;
	ts->fw_pen_state = 0;
	ts->gesture_mode = 0;
	INIT_WORK(&ts_restore_cmd_work, nvt_restore_cmd_func);
	ts->probe_state = 1;
#endif

#if IS_ENABLED(CONFIG_TINNO_DEVINFO)
	FULL_PRODUCT_DEVICE_CB(ID_TP, nvt_get_tp_info, NULL);
#endif
	{
		struct pinctrl *ts_pinctrl;
		struct pinctrl_state *ts_pinctrl_state;

		ts_pinctrl = devm_pinctrl_get(&client->dev);
		if (!IS_ERR(ts_pinctrl)) {
			ts_pinctrl_state = pinctrl_lookup_state(ts_pinctrl, "default");
			if (IS_ERR(ts_pinctrl_state))
				ts_pinctrl_state = pinctrl_lookup_state(ts_pinctrl, "resume");
			if (!IS_ERR(ts_pinctrl_state)) {
				ret = pinctrl_select_state(ts_pinctrl, ts_pinctrl_state);
				NVT_LOG("pinctrl select default/resume state result: %d\n", ret);
			}
		}
	}

	/* No SPI access happens here anymore. The MCU is only touched at the
	 * 60s deferred Boot_Update_Firmware (nt36xxx_fw_update.c), which skips the
	 * download when firmware is already running. Probe-time SPI (eng_reset etc.)
	 * intermittently oopsed the kernel in the pre-fs window, producing hard
	 * reboot loops (MT65xx Preloader cycling on USB). */
	nvt_crash_append("probe: SPI untouched this boot, chip reveal deferred\n");

	if (!nvt_poll_exclusive)
		nvt_irq_enable(true);

	/* NO SPI access until the deferred firmware step has armed the MCU.
	 * Reading a cold / half-booted chip (poll kthread, drain read, etc.)
	 * intermittently hangs the board in the pre-fs window. The poll kthread
	 * is started by Boot_Update_Firmware once the chip is running. */

	if (nvt_auto_reboot) {
		panic_timeout = 0;
		panic_on_oops = 1;
		atomic_notifier_chain_register(&panic_notifier_list, &nvt_panic_nb);
		NVT_LOG("auto-reboot armed: panic->30s force reboot, panic_on_oops=%d\n",
			panic_on_oops);
	}

	return 0;

#if IS_ENABLED(NVT_DRM_PANEL_NOTIFY)
	if (active_panel) {
		if (drm_panel_notifier_unregister(active_panel, &ts->drm_panel_notif))
			NVT_ERR("Error occurred while unregistering drm_panel_notifier.\n");
	}
#elif IS_ENABLED(NVT_MSM_DRM_NOTIFY)
	if (msm_drm_unregister_client(&ts->drm_notif))
		NVT_ERR("Error occurred while unregistering drm_notifier.\n");
#elif IS_ENABLED(NVT_FB_NOTIFY)
	if (fb_unregister_client(&ts->fb_notif))
		NVT_ERR("Error occurred while unregistering fb_notifier.\n");
#elif IS_ENABLED(NVT_EARLYSUSPEND_NOTIFY)
	unregister_early_suspend(&ts->early_suspend);
#elif IS_ENABLED(NVT_QCOM_PANEL_EVENT_NOTIFY)
	if (active_panel && notifier_cookie)
		panel_event_notifier_unregister(notifier_cookie);
#elif IS_ENABLED(NVT_MTK_DRM_NOTIFY)
    mtk_disp_notifier_unregister(&ts->disp_notifier);
#endif
#if defined(NVT_DRM_PANEL_NOTIFY) || defined(NVT_MSM_DRM_NOTIFY) || \
    defined(NVT_FB_NOTIFY) || defined(NVT_EARLYSUSPEND_NOTIFY) || \
    defined(NVT_QCOM_PANEL_EVENT_NOTIFY) || defined(NVT_MTK_DRM_NOTIFY)
err_register_suspend_resume_failed:
#endif
#if NVT_TOUCH_MP
	nvt_mp_proc_deinit();
err_mp_proc_init_failed:
#endif
#if NVT_TOUCH_EXT_PROC
	nvt_extra_proc_deinit();
err_extra_proc_init_failed:
#endif
#if NVT_TOUCH_PROC
	nvt_flash_proc_deinit();
err_flash_proc_init_failed:
#endif
#if NVT_TOUCH_ESD_PROTECT
	if (nvt_esd_check_wq) {
		cancel_delayed_work_sync(&nvt_esd_check_work);
		destroy_workqueue(nvt_esd_check_wq);
		nvt_esd_check_wq = NULL;
	}
err_create_nvt_esd_check_wq_failed:
#endif
	if (nvt_fwu_wq) {
#if BOOT_UPDATE_FIRMWARE
		cancel_delayed_work_sync(&ts->nvt_fwu_work);
#endif
		destroy_workqueue(nvt_fwu_wq);
		nvt_fwu_wq = NULL;
	}
err_create_nvt_fwu_wq_failed:
#if WAKEUP_GESTURE
	device_init_wakeup(&ts->client->dev, 0);
#endif
	free_irq(client->irq, ts);
err_int_request_failed:
	if (ts->pen_support) {
		input_unregister_device(ts->pen_input_dev);
		ts->pen_input_dev = NULL;
	}
err_pen_input_register_device_failed:
	if (ts->pen_support) {
		if (ts->pen_input_dev) {
			input_free_device(ts->pen_input_dev);
			ts->pen_input_dev = NULL;
		}
	}
err_pen_input_dev_alloc_failed:
	input_unregister_device(ts->input_dev);
	ts->input_dev = NULL;
err_input_register_device_failed:
	if (ts->input_dev) {
		input_free_device(ts->input_dev);
		ts->input_dev = NULL;
	}
err_input_dev_alloc_failed:
	mutex_destroy(&ts->xbuf_lock);
	mutex_destroy(&ts->lock);
	nvt_gpio_deconfig(ts);
err_gpio_config_failed:
err_spi_setup:
err_ckeck_full_duplex:
	spi_set_drvdata(client, NULL);
	if (ts->rbuf) {
		kfree(ts->rbuf);
		ts->rbuf = NULL;
	}
err_malloc_rbuf:
	if (ts->xbuf) {
		kfree(ts->xbuf);
		ts->xbuf = NULL;
	}
err_malloc_xbuf:
	if (ts) {
		kfree(ts);
		ts = NULL;
	}
	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen driver release function.

return:
	Executive outcomes. 0---succeed.
*******************************************************/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static void nvt_ts_remove(struct spi_device *client)
#else
static int32_t nvt_ts_remove(struct spi_device *client)
#endif
{
	NVT_LOG("Removing driver...\n");

	nvt_crashlog_shutdown();
	if (nvt_auto_reboot)
		atomic_notifier_chain_unregister(&panic_notifier_list, &nvt_panic_nb);

#if IS_ENABLED(NVT_DRM_PANEL_NOTIFY)
	if (active_panel) {
		if (drm_panel_notifier_unregister(active_panel, &ts->drm_panel_notif))
			NVT_ERR("Error occurred while unregistering drm_panel_notifier.\n");
	}
#elif IS_ENABLED(NVT_MSM_DRM_NOTIFY)
	if (msm_drm_unregister_client(&ts->drm_notif))
		NVT_ERR("Error occurred while unregistering drm_notifier.\n");
#elif IS_ENABLED(NVT_FB_NOTIFY)
	if (fb_unregister_client(&ts->fb_notif))
		NVT_ERR("Error occurred while unregistering fb_notifier.\n");
#elif IS_ENABLED(NVT_EARLYSUSPEND_NOTIFY)
	unregister_early_suspend(&ts->early_suspend);
#elif IS_ENABLED(NVT_QCOM_PANEL_EVENT_NOTIFY)
	if (active_panel && notifier_cookie)
		panel_event_notifier_unregister(notifier_cookie);
#elif IS_ENABLED(NVT_MTK_DRM_NOTIFY)
    mtk_disp_notifier_unregister(&ts->disp_notifier);
#endif

#if NVT_TOUCH_MP
	nvt_mp_proc_deinit();
#endif
#if NVT_TOUCH_EXT_PROC
	nvt_extra_proc_deinit();
#endif
#if NVT_TOUCH_PROC
	nvt_flash_proc_deinit();
#endif

#if NVT_TOUCH_ESD_PROTECT
	if (nvt_esd_check_wq) {
		cancel_delayed_work_sync(&nvt_esd_check_work);
		nvt_esd_check_enable(false);
		destroy_workqueue(nvt_esd_check_wq);
		nvt_esd_check_wq = NULL;
	}
#endif

#if BOOT_UPDATE_FIRMWARE
	if (nvt_fwu_wq) {
		cancel_delayed_work_sync(&ts->nvt_fwu_work);
		destroy_workqueue(nvt_fwu_wq);
		nvt_fwu_wq = NULL;
	}
#endif

#if WAKEUP_GESTURE
	device_init_wakeup(&ts->client->dev, 0);
#endif

	nvt_irq_enable(false);
	free_irq(client->irq, ts);

	if (ts->poll_task) {
		kthread_stop(ts->poll_task);
		ts->poll_task = NULL;
	}

	mutex_destroy(&ts->xbuf_lock);
	mutex_destroy(&ts->lock);

	nvt_gpio_deconfig(ts);

	if (ts->pen_support) {
		if (ts->pen_input_dev) {
			input_unregister_device(ts->pen_input_dev);
			ts->pen_input_dev = NULL;
		}
	}

	if (ts->input_dev) {
		input_unregister_device(ts->input_dev);
		ts->input_dev = NULL;
	}

	spi_set_drvdata(client, NULL);

	if (ts->xbuf) {
		kfree(ts->xbuf);
		ts->xbuf = NULL;
	}

	if (ts) {
		kfree(ts);
		ts = NULL;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	return 0;
#endif
}

static void nvt_ts_shutdown(struct spi_device *client)
{
	uint8_t buf[4] = {0};

	NVT_LOG("Shutdown driver...\n");

	mutex_lock(&ts->lock);

	if(ts->gesture_mode) {
	//---write command to enter "wakeup gesture mode"---
	buf[0] = EVENT_MAP_HOST_CMD;
	buf[1] = 0x13;
	CTP_SPI_WRITE(ts->client, buf, 2);

	enable_irq_wake(ts->client->irq);

	NVT_LOG("Enabled touch wakeup gesture\n");

	} else {
	//---write command to enter "deep sleep mode"---
	buf[0] = EVENT_MAP_HOST_CMD;
	buf[1] = 0x11;
	CTP_SPI_WRITE(ts->client, buf, 2);
	}
	msleep(50);

	mutex_unlock(&ts->lock);

	ts->is_suspend = true;
	nvt_irq_enable(false);

#if IS_ENABLED(NVT_DRM_PANEL_NOTIFY)
	if (active_panel) {
		if (drm_panel_notifier_unregister(active_panel, &ts->drm_panel_notif))
			NVT_ERR("Error occurred while unregistering drm_panel_notifier.\n");
	}
#elif IS_ENABLED(NVT_MSM_DRM_NOTIFY)
	if (msm_drm_unregister_client(&ts->drm_notif))
		NVT_ERR("Error occurred while unregistering drm_notifier.\n");
#elif IS_ENABLED(NVT_FB_NOTIFY)
	if (fb_unregister_client(&ts->fb_notif))
		NVT_ERR("Error occurred while unregistering fb_notifier.\n");
#elif IS_ENABLED(NVT_EARLYSUSPEND_NOTIFY)
	unregister_early_suspend(&ts->early_suspend);
#elif IS_ENABLED(NVT_QCOM_PANEL_EVENT_NOTIFY)
	if (active_panel && notifier_cookie)
		panel_event_notifier_unregister(notifier_cookie);
#elif IS_ENABLED(NVT_MTK_DRM_NOTIFY)
    mtk_disp_notifier_unregister(&ts->disp_notifier);
#endif

#if NVT_TOUCH_MP
	nvt_mp_proc_deinit();
#endif
#if NVT_TOUCH_EXT_PROC
	nvt_extra_proc_deinit();
#endif
#if NVT_TOUCH_PROC
	nvt_flash_proc_deinit();
#endif

#if NVT_TOUCH_ESD_PROTECT
	if (nvt_esd_check_wq) {
		cancel_delayed_work_sync(&nvt_esd_check_work);
		nvt_esd_check_enable(false);
		destroy_workqueue(nvt_esd_check_wq);
		nvt_esd_check_wq = NULL;
	}
#endif /* #if NVT_TOUCH_ESD_PROTECT */

#if BOOT_UPDATE_FIRMWARE
	if (nvt_fwu_wq) {
		cancel_delayed_work_sync(&ts->nvt_fwu_work);
		destroy_workqueue(nvt_fwu_wq);
		nvt_fwu_wq = NULL;
	}
#endif

#if WAKEUP_GESTURE
	device_init_wakeup(&ts->client->dev, 0);
#endif
}

/*******************************************************
Description:
	Novatek touchscreen driver suspend function.

return:
	Executive outcomes. 0---succeed.
*******************************************************/
static int32_t nvt_ts_suspend(struct device *dev)
{
	NVT_LOG("keeping touch alive across suspend (deep sleep & pinmux disable prevented)\n");
	return 0;
}

/*******************************************************
Description:
	Novatek touchscreen driver resume function.

return:
	Executive outcomes. 0---succeed.
*******************************************************/
static int32_t nvt_ts_resume(struct device *dev)
{
	struct pinctrl *spi_module_pinctrl;
	struct pinctrl_state *spi_pinctrl_state;
	int ret = 0;
	ts->is_suspend = false;

	if (bTouchIsAwake) {
		NVT_LOG("Touch is already resume\n");
		return 0;
	}

if(!ts->gesture_mode) {
	spi_module_pinctrl = devm_pinctrl_get(dev);
	NVT_LOG("enter spi mode\n");
	if(!IS_ERR(spi_module_pinctrl)) {
		NVT_LOG("spi_module_pinctrl enter!\n");
		spi_pinctrl_state = pinctrl_lookup_state(spi_module_pinctrl, "resume");
		if(!IS_ERR(spi_pinctrl_state)) {
			NVT_LOG("spi_pinctrl_state enter!\n");
			ret = pinctrl_select_state(spi_module_pinctrl, spi_pinctrl_state);
			if(ret < 0)
				NVT_ERR("enter spi mode fail!\n");
		}
	}
}

	mutex_lock(&ts->lock);

	NVT_LOG("start\n");

	// please make sure display reset(RESX) sequence and mipi dsi cmds sent before this
#if NVT_TOUCH_SUPPORT_HW_RST
	gpio_set_value(ts->reset_gpio, 1);
#endif
	nvt_sw_reset_idle();
	nvt_boot_ready();
	nvt_tx_auto_copy_mode();
	nvt_sw_reset();

	if (!nvt_poll_exclusive)
		nvt_irq_enable(true);

#if NVT_TOUCH_ESD_PROTECT
	nvt_esd_check_enable(false);
	queue_delayed_work(nvt_esd_check_wq, &nvt_esd_check_work,
			msecs_to_jiffies(NVT_TOUCH_ESD_CHECK_PERIOD));
#endif /* #if NVT_TOUCH_ESD_PROTECT */
	nvt_lcd_esd_resume = nvt_esd_resume;

	bTouchIsAwake = 1;

	mutex_unlock(&ts->lock);

	/* Kick autonomous continuous scan immediately on resume so touch
	 * wakes without waiting for the 20s watchdog cycle under simpledrm/pmosfb */
	msleep(20);
	nvt_force_continuous_scan();

#if NVT_CUST_PROC_CMD
	queue_work(nvt_fwu_wq, &ts_restore_cmd_work);
#endif

	NVT_LOG("end\n");

	return 0;
}

void nvt_esd_resume(bool status){
	if(status) {
		NVT_LOG("yc status = %d\n", status);
		nvt_ts_resume(&ts->client->dev);

	} else {
		NVT_LOG("yc status = %d\n", status);
		nvt_ts_suspend(&ts->client->dev);
	}
}

#if IS_ENABLED(NVT_DRM_PANEL_NOTIFY)
static int nvt_drm_panel_notifier_callback(struct notifier_block *self, unsigned long event, void *data)
{
	struct drm_panel_notifier *evdata = data;
	int *blank;
	struct nvt_ts_data *ts =
		container_of(self, struct nvt_ts_data, drm_panel_notif);

	if (!evdata)
		return 0;

	if (!(event == DRM_PANEL_EARLY_EVENT_BLANK ||
		event == DRM_PANEL_EVENT_BLANK)) {
		//NVT_LOG("event(%lu) not need to process\n", event);
		return 0;
	}

	if (evdata->data && ts) {
		blank = evdata->data;
		if (event == DRM_PANEL_EARLY_EVENT_BLANK) {
			if (*blank == DRM_PANEL_BLANK_POWERDOWN) {
				NVT_LOG("event=%lu, *blank=%d\n", event, *blank);
				nvt_ts_suspend(&ts->client->dev);
			}
		} else if (event == DRM_PANEL_EVENT_BLANK) {
			if (*blank == DRM_PANEL_BLANK_UNBLANK) {
				NVT_LOG("event=%lu, *blank=%d\n", event, *blank);
				nvt_ts_resume(&ts->client->dev);
			}
		}
	}

	return 0;
}
#elif IS_ENABLED(NVT_MSM_DRM_NOTIFY)
static int nvt_drm_notifier_callback(struct notifier_block *self, unsigned long event, void *data)
{
	struct msm_drm_notifier *evdata = data;
	int *blank;
	struct nvt_ts_data *ts =
		container_of(self, struct nvt_ts_data, drm_notif);

	if (!evdata || (evdata->id != 0))
		return 0;

	if (evdata->data && ts) {
		blank = evdata->data;
		if (event == MSM_DRM_EARLY_EVENT_BLANK) {
			if (*blank == MSM_DRM_BLANK_POWERDOWN) {
				NVT_LOG("event=%lu, *blank=%d\n", event, *blank);
				nvt_ts_suspend(&ts->client->dev);
			}
		} else if (event == MSM_DRM_EVENT_BLANK) {
			if (*blank == MSM_DRM_BLANK_UNBLANK) {
				NVT_LOG("event=%lu, *blank=%d\n", event, *blank);
				nvt_ts_resume(&ts->client->dev);
			}
		}
	}

	return 0;
}
#elif IS_ENABLED(NVT_FB_NOTIFY)
static int nvt_fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;
	struct nvt_ts_data *ts =
		container_of(self, struct nvt_ts_data, fb_notif);

	if (evdata && evdata->data && event == FB_EARLY_EVENT_BLANK) {
		blank = evdata->data;
		if (*blank == FB_BLANK_POWERDOWN) {
			NVT_LOG("event=%lu, *blank=%d\n", event, *blank);
			nvt_ts_suspend(&ts->client->dev);
		}
	} else if (evdata && evdata->data && event == FB_EVENT_BLANK) {
		blank = evdata->data;
		if (*blank == FB_BLANK_UNBLANK) {
			NVT_LOG("event=%lu, *blank=%d\n", event, *blank);
			nvt_ts_resume(&ts->client->dev);
		}
	}

	return 0;
}
#elif IS_ENABLED(NVT_EARLYSUSPEND_NOTIFY)
/*******************************************************
Description:
	Novatek touchscreen driver early suspend function.

return:
	n.a.
*******************************************************/
static void nvt_ts_early_suspend(struct early_suspend *h)
{
	nvt_ts_suspend(ts->client, PMSG_SUSPEND);
}

/*******************************************************
Description:
	Novatek touchscreen driver late resume function.

return:
	n.a.
*******************************************************/
static void nvt_ts_late_resume(struct early_suspend *h)
{
	nvt_ts_resume(ts->client);
}
#elif IS_ENABLED(NVT_QCOM_PANEL_EVENT_NOTIFY)
static void nvt_panel_notifier_callback(enum panel_event_notifier_tag tag,
		struct panel_event_notification *notification, void *client_data)
{
	struct nvt_ts_data *ts = client_data;

	if (!notification) {
		NVT_ERR("Invalid notification\n");
		return;
	}

	NVT_LOG("Notification type:%d, early_trigger:%d",
			notification->notif_type,
			notification->notif_data.early_trigger);

	switch (notification->notif_type) {
		case DRM_PANEL_EVENT_UNBLANK:
			if (notification->notif_data.early_trigger)
				NVT_LOG("resume notification pre commit\n");
			else
				nvt_ts_resume(&ts->client->dev);
			break;

		case DRM_PANEL_EVENT_BLANK:
			if (notification->notif_data.early_trigger)
				nvt_ts_suspend(&ts->client->dev);
			else
				NVT_LOG("suspend notification post commit\n");
			break;

		case DRM_PANEL_EVENT_BLANK_LP:
			NVT_LOG("received lp event\n");
			break;

		case DRM_PANEL_EVENT_FPS_CHANGE:
			NVT_LOG("Received fps change old fps:%d new fps:%d\n",
					notification->notif_data.old_fps,
					notification->notif_data.new_fps);
			break;
		default:
			NVT_LOG("notification serviced :%d\n",
					notification->notif_type);
			break;
	}
}
#elif IS_ENABLED(NVT_MTK_DRM_NOTIFY)
static int nvt_mtk_drm_notifier_callback(struct notifier_block *nb,
		unsigned long event, void *data)
{
    int *blank = (int *)data;

    if (!blank) {
        NVT_ERR("Invalid blank\n");
        return -1;
    }

    if (event == MTK_DISP_EARLY_EVENT_BLANK) {
        if (*blank == MTK_DISP_BLANK_POWERDOWN) {
			NVT_LOG("event=%lu, *blank=%d\n", event, *blank);
            nvt_ts_suspend(&ts->client->dev);
        }
	} else if (event == MTK_DISP_EVENT_BLANK) {
		if (*blank == MTK_DISP_BLANK_UNBLANK) {
			NVT_LOG("event=%lu, *blank=%d\n", event, *blank);
			nvt_ts_resume(&ts->client->dev);
		}
	}

    return 0;
}
#endif

#if NVT_PM_WAIT_BUS_RESUME_COMPLETE
static int nvt_ts_pm_suspend(struct device *dev)
{
	ts->dev_pm_suspend = true;
	reinit_completion(&ts->dev_pm_resume_completion);

	return 0;
}

static int nvt_ts_pm_resume(struct device *dev)
{
	ts->dev_pm_suspend = false;
	complete(&ts->dev_pm_resume_completion);

	return 0;
}

static const struct dev_pm_ops nvt_ts_dev_pm_ops = {
	.suspend = nvt_ts_pm_suspend,
	.resume  = nvt_ts_pm_resume,
};
#endif /* NVT_PM_WAIT_BUS_RESUME_COMPLETE */

static const struct spi_device_id nvt_ts_id[] = {
	{ NVT_SPI_NAME, 0 },
	{ }
};

#ifdef CONFIG_OF
static struct of_device_id nvt_match_table[] = {
	{ .compatible = "novatek,NVT-ts-spi",},
	{ },
};
#endif

static struct spi_driver nvt_spi_driver = {
	.probe		= nvt_ts_probe,
	.remove		= nvt_ts_remove,
	.shutdown	= nvt_ts_shutdown,
	.id_table	= nvt_ts_id,
	.driver = {
		.name	= NVT_SPI_NAME,
		.owner	= THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = nvt_match_table,
#endif
#if NVT_PM_WAIT_BUS_RESUME_COMPLETE
		.pm = &nvt_ts_dev_pm_ops,
#endif /* NVT_PM_WAIT_BUS_RESUME_COMPLETE */
	},
};

// Linux 2.0/2.2
static int penraw_open(struct inode * inode, struct file * file)
{
	return 0;
}

// Linux 2.1: int type
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 1, 0)
static int penraw_close(struct inode * inode, struct file * file)
{
	return 0;
}
#else
static void penraw_close(struct inode * inode, struct file * file)
{
}
#endif

#define PENRAW_IOC_TYPE 'P'
#define PENRAW_GET_VALUES _IOR(PENRAW_IOC_TYPE, 0, struct io_pen_report)

static struct io_pen_report pen_report;	// return report
static long penraw_ioctl(struct file *file,
		unsigned int cmd, unsigned long arg)
{
	int len = 0;
	unsigned long	flags;
	struct lenovo_pen_info *ppen_info;
	unsigned char cnt;
	unsigned char pen_buffer_rp;
	unsigned char wp;
	unsigned char num;

//	printk(KERN_ERR "penraw: ioctl: cmd=%04X, arg=%08lX\n",cmd,arg);

	switch(cmd) {
		case PENRAW_GET_VALUES:
			local_irq_save(flags);
			wp = pen_buffer_wp;
			num = pen_report_num;
			local_irq_restore(flags);
			if(MAX_IO_CONTROL_REPORT <= num) {
				pen_buffer_rp = (unsigned char)((wp + (LENOVO_MAX_BUFFER - MAX_IO_CONTROL_REPORT)) % LENOVO_MAX_BUFFER);
			} else {
				pen_buffer_rp = 0;
			}
			memset(&pen_report, 0, sizeof(pen_report));
			pen_report.report_num = num;
			ppen_info = (struct lenovo_pen_info *)&pen_report.pen_info[0];
			for(cnt = 0; cnt < num; cnt++) {
				memcpy(ppen_info, &pen_buffer[pen_buffer_rp], sizeof(struct lenovo_pen_info));
				ppen_info++;
				pen_buffer_rp++;
				if(LENOVO_MAX_BUFFER == pen_buffer_rp) {
					pen_buffer_rp = 0;
				}
			}
			if (copy_to_user((void __user *)arg, &pen_report, sizeof(pen_report))) {
				return -EFAULT;
			}
			break;
		default:
			printk(KERN_WARNING "unsupported command %d\n", cmd);
			return -EFAULT;
	}
	return len;
}

static struct file_operations penraw_fops = {
	.owner = THIS_MODULE,
	.open = penraw_open,
	.release = penraw_close,
	.unlocked_ioctl = penraw_ioctl,
};

/*******************************************************
Description:
	Driver Install function.

return:
	Executive Outcomes. 0---succeed. not 0---failed.
********************************************************/
static int32_t __init nvt_driver_init(void)
{
	int32_t ret = 0;
	int alloc_ret;
	dev_t dev;
	int cdev_err;

	NVT_LOG("start\n");

	//---add spi driver---
	ret = spi_register_driver(&nvt_spi_driver);
	if (ret) {
		NVT_ERR("failed to add spi driver");
		goto err_driver;
	}
	pen_report_num = 0;
	pen_frame_no = 0;
	pen_buffer_wp = 0;

	/* get not assigned major numbers */
	alloc_ret = alloc_chrdev_region(&dev, MINOR_NUMBER_START, NUMBER_MINOR_NUMBER, PENRAW_DRIVER_NAME);
	if (alloc_ret != 0) {
		printk(KERN_ERR "failed to alloc_chrdev_region()\n");
		return -EINVAL;
	}

	/* get one number from the not-assigend numbers */
	major_number = MAJOR(dev);

	/* initialize cdev and function table */
	cdev_init(&penraw_char_dev, &penraw_fops);
	penraw_char_dev.owner = THIS_MODULE;

	/* register the driver */
	cdev_err = cdev_add(&penraw_char_dev, dev, NUMBER_MINOR_NUMBER);
	if (cdev_err != 0) {
		printk(KERN_ERR "failed to cdev_add()\n");
		unregister_chrdev_region(dev, NUMBER_MINOR_NUMBER);
		return -EINVAL;
	}

	/* register a class */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	penraw_char_dev_class = class_create(PENRAW_DEVICE_NAME);
#else
	penraw_char_dev_class = class_create(THIS_MODULE, PENRAW_DEVICE_NAME);
#endif
	if (IS_ERR(penraw_char_dev_class)) {
		printk(KERN_ERR "class_create()\n");
		cdev_del(&penraw_char_dev);
		unregister_chrdev_region(dev, NUMBER_MINOR_NUMBER);
		return -EINVAL;
	}

	/* create "/sys/class/my_device/my_device" */
	device_create(penraw_char_dev_class, NULL, MKDEV(major_number, 0), NULL, PENRAW_DEVICE_NAME);

	NVT_LOG("finished\n");

err_driver:
	return ret;
}

/*******************************************************
Description:
	Driver uninstall function.

return:
	n.a.
********************************************************/
static void __exit nvt_driver_exit(void)
{
	dev_t dev = MKDEV(major_number, MINOR_NUMBER_START);

	spi_unregister_driver(&nvt_spi_driver);

	/* remove "/dev/lenovo_penraw */
	device_destroy(penraw_char_dev_class, MKDEV(major_number, 0));

	/* remove class */
	class_destroy(penraw_char_dev_class);

	/* remove driver */
	cdev_del(&penraw_char_dev);

	/* release the major number */
	unregister_chrdev_region(dev, NUMBER_MINOR_NUMBER);
}

#if IS_ENABLED(NVT_DRM_PANEL_NOTIFY)
late_initcall(nvt_driver_init);
#else
module_init(nvt_driver_init);
#endif
module_exit(nvt_driver_exit);

MODULE_DESCRIPTION("Novatek Touchscreen Driver");
MODULE_LICENSE("GPL");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif
