// SPDX-License-Identifier: GPL-2.0-only
/*
 * pmosfb -- pmOS DRM driver for MediaTek bootloader-initialized scanout.
 *
 * The LK bootloader brings up and leaves running the MT6789 display
 * pipeline (OVL0 -> RDMA0 -> DSI -> NT36523N) scanning a framebuffer that
 * it propped up at a fixed physical address. setup.c keeps the mmsys
 * clocks alive, memblock-reserves that buffer, and registers this driver's
 * platform device. pmosfb samples that running pipeline:
 *
 *   - full-screen flips every commit; FB_DAMAGE_CLIPS is not advertised so
 *     the compositor must re-render the full buffer every frame, preventing
 *     stale lockscreen content from persisting after screen-unlock,
 *   - D-cache clean to the Point-of-Coherency before the panel DMA reads,
 *   - no_vblank so page-flip completion is delivered synchronously,
 *   - direct DISP_PWM0 register access for backlight (no clock/pwm tree
 *     requirements), preserving the period the bootloader programmed.
 *
 * It is fully self-contained: no mediatek-drm, no scpsys, no cmdq, no
 * power-domain or regulator drivers required.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/platform_data/simplefb.h>
#include <linux/platform_device.h>
#include <asm/cacheflush.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_panic.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#define DRIVER_NAME	"pmosfb"
#define DRIVER_DESC	"pmOS framebuffer driver for MediaTek bootloader scanout"
#define DRIVER_DATE	"20260907"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	2

#define PMOSFB_VBLANK_HZ	60

/*
 * Backlight via the DISP_PWM0 block. Layout follows the mt8183-style
 * registers used across MTK SoCs:
 *
 *	0x00 DISP_PWM_EN	bit0  = enable
 *	0x18 DISP_PWM_CON0	[26:16] clkdiv, [15] source selection
 *	0x1c DISP_PWM_CON1	[11:0]  period,  [31:16] high width
 *
 * We never touch CON0 or the enable bit: the bootloader already left the
 * PWM running at the right frequency and brightness. Only the high-width
 * field is rewritten, linearly scaled from userspace brightness.
 */

#define DISP_PWM_CON1		0x1c
#define DISP_PWM_CON1_HIGH_WIDTH_MASK	(0x1fff << 16)

/*
 * pmosfb device
 */

struct pmosfb_device {
	struct drm_device dev;

	/* screen / scanout buffer */
	void *screen_base;
	unsigned int pitch;

	/* display parameters */
	struct drm_display_mode mode;
	const struct drm_format_info *format;

	/* backlight */
	struct backlight_device *bl;
	void __iomem *disp_pwm_base;
	u32 disp_pwm_con1_shadow;
	u32 disp_pwm_high_width_max;
	int bl_enable_gpio;

	/* modesetting */
	uint32_t formats[8];
	size_t nformats;
	struct drm_plane primary_plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;

	/* synthetic vblank pacing (no hardware IRQ is wired for the LCD) */
	struct hrtimer vblank_timer;
	ktime_t vblank_period;
	bool vblank_enabled;
	spinlock_t vblank_lock;

	/* present-rate gate: last blit start timestamp, one blit per period */
	ktime_t last_present;
};

static struct pmosfb_device *pmosfb_device_of_dev(struct drm_device *dev)
{
	return container_of(dev, struct pmosfb_device, dev);
}

/*
 * Modesetting
 */

static const uint64_t pmosfb_primary_plane_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static int pmosfb_primary_plane_helper_atomic_check(struct drm_plane *plane,
						    struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *new_shadow_plane_state =
		to_drm_shadow_plane_state(new_plane_state);
	struct drm_framebuffer *new_fb = new_plane_state->fb;
	struct drm_crtc *new_crtc = new_plane_state->crtc;
	struct drm_crtc_state *new_crtc_state = NULL;
	struct drm_device *dev = plane->dev;
	struct pmosfb_device *sdev = pmosfb_device_of_dev(dev);
	int ret;

	if (new_crtc)
		new_crtc_state = drm_atomic_get_new_crtc_state(state, new_crtc);

	ret = drm_atomic_helper_check_plane_state(new_plane_state, new_crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  false, false);
	if (ret)
		return ret;
	else if (!new_plane_state->visible)
		return 0;

	if (new_fb->format != sdev->format) {
		void *buf;

		/* format conversion necessary; reserve buffer */
		buf = drm_format_conv_state_reserve(&new_shadow_plane_state->fmtcnv_state,
						    sdev->pitch, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
	}

	return 0;
}

static void pmosfb_pace_present(struct pmosfb_device *sdev);

static void pmosfb_primary_plane_helper_atomic_update(struct drm_plane *plane,
						      struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow_plane_state = to_drm_shadow_plane_state(plane_state);
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_device *dev = plane->dev;
	struct pmosfb_device *sdev = pmosfb_device_of_dev(dev);
	struct drm_rect dst_clip;
	struct iosys_map dst;
	unsigned int cpp = sdev->format->cpp[0];
	unsigned int row_bytes;
	unsigned int pad_bytes;
	int ret, idx;

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_TO_DEVICE);
	if (ret)
		return;

	if (!drm_dev_enter(dev, &idx))
		goto out_drm_gem_fb_end_cpu_access;

	/*
	 * Gate the blit to the panel refresh cadence.  This deliberately runs
	 * before the full-buffer copy below so that back-to-back compositor
	 * presents (touch ripples, unlock morph) are spread one per refresh
	 * period instead of being dumped into the live scanout mid-frame.
	 */
	pmosfb_pace_present(sdev);

	/*
	 * Always blit the FULL source buffer into the scanout framebuffer on
	 * every update.  Combined with the absence of FB_DAMAGE_CLIPS (which
	 * forces the compositor to re-render the full GEM buffer each frame),
	 * this guarantees that the scanout memory always mirrors the latest
	 * presented frame with no stale regions from previous frames.
	 */
	dst_clip = plane_state->dst;
	dst_clip.x1 = 0;
	dst_clip.y1 = 0;
	dst_clip.x2 = min_t(unsigned int, plane_state->dst.x2, fb->width);
	dst_clip.y2 = fb->height;
	iosys_map_set_vaddr(&dst, sdev->screen_base);

	iosys_map_incr(&dst, drm_fb_clip_offset((unsigned int)sdev->pitch,
						sdev->format, &dst_clip));
	drm_fb_blit(&dst, &sdev->pitch, sdev->format->format, shadow_plane_state->data,
		    fb, &dst_clip, &shadow_plane_state->fmtcnv_state);

	row_bytes = fb->width * cpp;
	pad_bytes = sdev->pitch - row_bytes;
	if (pad_bytes) {
		unsigned int y;

		for (y = dst_clip.y1; y < dst_clip.y2; y++) {
			void *line_end = (char *)sdev->screen_base + (y * sdev->pitch) + row_bytes;
			u32 edge_pixel = *(u32 *)(line_end - cpp);
			u32 *p = (u32 *)line_end;
			unsigned int n = pad_bytes / 4;
			while (n--)
				*p++ = edge_pixel;
		}
	}
	/* Flush CPU D-cache to the Point-of-Coherency so the scanout DMA sees
	 * our renders. The framebuffer is plain System RAM mapped via the
	 * cacheable linear map (phys_to_virt), so wmb() alone is NOT sufficient
	 * on arm64 -- the panel's DMA engine reads DRAM directly and would
	 * otherwise latch stale/incoherent lines (scattered black pixels).
	 */
	if (dst_clip.y2 > dst_clip.y1) {
		unsigned long start = (unsigned long)sdev->screen_base + dst_clip.y1 * sdev->pitch;
		unsigned long len = (dst_clip.y2 - dst_clip.y1) * sdev->pitch;
		dcache_clean_poc(start, start + len);
	}
	wmb();

	drm_dev_exit(idx);
out_drm_gem_fb_end_cpu_access:
	drm_gem_fb_end_cpu_access(fb, DMA_TO_DEVICE);
}

static void pmosfb_primary_plane_helper_atomic_disable(struct drm_plane *plane,
						       struct drm_atomic_state *state)
{
	struct drm_device *dev = plane->dev;
	int idx;

	if (!drm_dev_enter(dev, &idx))
		return;

	/* Keep active framebuffer intact during DPMS / disable transitions */

	drm_dev_exit(idx);
}

static int pmosfb_primary_plane_helper_get_scanout_buffer(struct drm_plane *plane,
							  struct drm_scanout_buffer *sb)
{
	struct pmosfb_device *sdev = pmosfb_device_of_dev(plane->dev);

	sb->width = sdev->mode.hdisplay;
	sb->height = sdev->mode.vdisplay;
	sb->format = sdev->format;
	sb->pitch[0] = sdev->pitch;
	iosys_map_set_vaddr(&sb->map[0], sdev->screen_base);

	return 0;
}

static const struct drm_plane_helper_funcs pmosfb_primary_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = pmosfb_primary_plane_helper_atomic_check,
	.atomic_update = pmosfb_primary_plane_helper_atomic_update,
	.atomic_disable = pmosfb_primary_plane_helper_atomic_disable,
	.get_scanout_buffer = pmosfb_primary_plane_helper_get_scanout_buffer,
};

static const struct drm_plane_funcs pmosfb_primary_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

static enum drm_mode_status pmosfb_crtc_helper_mode_valid(struct drm_crtc *crtc,
							  const struct drm_display_mode *mode)
{
	struct pmosfb_device *sdev = pmosfb_device_of_dev(crtc->dev);

	return drm_crtc_helper_mode_valid_fixed(crtc, mode, &sdev->mode);
}

/*
 * Synthetic VBLANK source.
 *
 * The bootloader-seam LCD has no interrupt line exposed to pmosfb (the
 * runtime FDT carries no `interrupts` property for disp_ovl0/rdma/dsi and
 * the mediatek-drm pipeline is disabled).  Without a vblank signal the
 * compositor (wlroots/Phosh) cannot pace page-flips: on the unlock->desktop
 * morph it may present under-composited frames that survive for minutes as a
 * stale region in the scanout (the "black spot").  We therefore synthesize a
 * 60 Hz vblank with a high-resolution timer, giving wlroots a real vsync to
 * complete each frame as an atomic, full repaint before presenting.  This is
 * the same approach used by several DRM drivers that lack a hardware vblank
 * interrupt.
 */
static enum hrtimer_restart pmosfb_vblank_timer_cb(struct hrtimer *timer)
{
	struct pmosfb_device *sdev =
		container_of(timer, struct pmosfb_device, vblank_timer);
	unsigned long flags;

	spin_lock_irqsave(&sdev->vblank_lock, flags);
	if (sdev->vblank_enabled)
		drm_crtc_handle_vblank(&sdev->crtc);
	spin_unlock_irqrestore(&sdev->vblank_lock, flags);

	hrtimer_forward_now(timer, sdev->vblank_period);
	return HRTIMER_RESTART;
}

static void pmosfb_vblank_start(struct pmosfb_device *sdev)
{
	unsigned long flags;

	spin_lock_irqsave(&sdev->vblank_lock, flags);
	sdev->vblank_enabled = true;
	spin_unlock_irqrestore(&sdev->vblank_lock, flags);
	hrtimer_start(&sdev->vblank_timer, sdev->vblank_period, HRTIMER_MODE_REL);
}

static void pmosfb_vblank_stop(struct pmosfb_device *sdev)
{
	unsigned long flags;

	hrtimer_cancel(&sdev->vblank_timer);
	spin_lock_irqsave(&sdev->vblank_lock, flags);
	sdev->vblank_enabled = false;
	spin_unlock_irqrestore(&sdev->vblank_lock, flags);
}

/*
 * Present-rate gate.
 *
 * The panel refresh period is vblank_period (60 Hz).  With no_vblank set,
 * page-flip completion is synchronous, so a compositor (wlroots) is not
 * throttled by any vsync: on touch/unlock it can commit several full frames
 * within a single panel refresh period.  Those unpaced, back-to-back whole
 * framebuffer blits race the RDMA0 -> DSC -> DSI scanout of the *same*
 * memory and the panel's DSC/TDDI transport can latch a stale region (the
 * persistent black patch) until a blank forces a full re-sync.
 *
 * We therefore gate every atomic_update so at most one full scanout write
 * begins per refresh period.  This gives the display transport a clean
 * one-frame-per-period cadence without involving the DRM vblank machinery
 * (keeping synchronous flip completion -> no GUI deadlock, as proven by the
 * known-good baseline).  usleep_range() is safe here: the default atomic
 * helper runs the commit in a workqueue (sleepable context).
 */
static void pmosfb_pace_present(struct pmosfb_device *sdev)
{
	/* The synthetic vblank timer already delivers 60 Hz frame pacing to
	 * Mutter/Phosh. Extra sleeping inside the commit worker stalls video
	 * frames and causes playback lag. We update timestamp without sleeping.
	 */
	sdev->last_present = ktime_get();
}

/*
 * The CRTC is always enabled. Screen updates are performed by
 * the primary plane's atomic_update function. Disabling clears
 * the screen in the primary plane's atomic_disable function.
 *
 * We keep no_vblank = true so page-flip completion is delivered
 * synchronously by drm_atomic_helper_fake_vblank(): this is what makes
 * the compositor's very first frames complete reliably and prevents the
 * deadlock that occurs if flip completion is made to depend on a vblank
 * source that isn't yet running.  In parallel the hrtimer vblank source
 * below feeds drm_crtc_handle_vblank() so wlroots/Phosh still receive a
 * real vsync for frame pacing (fixing the stale-region / "black spot"
 * artefact after unlock), without ever blocking a page-flip.
 */
static int pmosfb_crtc_atomic_check(struct drm_crtc *crtc,
				    struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);

	crtc_state->no_vblank = true;

	return drm_crtc_helper_atomic_check(crtc, state);
}

static void pmosfb_crtc_atomic_enable(struct drm_crtc *crtc,
				      struct drm_atomic_state *state)
{
	struct pmosfb_device *sdev = pmosfb_device_of_dev(crtc->dev);

	drm_crtc_vblank_on(crtc);
	pmosfb_vblank_start(sdev);
}

static void pmosfb_crtc_atomic_disable(struct drm_crtc *crtc,
				       struct drm_atomic_state *state)
{
	struct pmosfb_device *sdev = pmosfb_device_of_dev(crtc->dev);

	pmosfb_vblank_stop(sdev);
	drm_crtc_vblank_off(crtc);
}

/*
 * The synthetic vblank timer is already feeding drm_crtc_handle_vblank
 * while the CRTC is enabled, so the DRM core's enable/disable_vblank
 * callbacks are no-ops that only need to report success.
 */
static int pmosfb_crtc_enable_vblank(struct drm_crtc *crtc)
{
	return 0;
}

static void pmosfb_crtc_disable_vblank(struct drm_crtc *crtc)
{
}

static const struct drm_crtc_helper_funcs pmosfb_crtc_helper_funcs = {
	.mode_valid = pmosfb_crtc_helper_mode_valid,
	.atomic_check = pmosfb_crtc_atomic_check,
	.atomic_enable = pmosfb_crtc_atomic_enable,
	.atomic_disable = pmosfb_crtc_atomic_disable,
};

static const struct drm_crtc_funcs pmosfb_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = pmosfb_crtc_enable_vblank,
	.disable_vblank = pmosfb_crtc_disable_vblank,
};

static const struct drm_encoder_funcs pmosfb_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int pmosfb_connector_helper_get_modes(struct drm_connector *connector)
{
	struct pmosfb_device *sdev = pmosfb_device_of_dev(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, &sdev->mode);
}

static const struct drm_connector_helper_funcs pmosfb_connector_helper_funcs = {
	.get_modes = pmosfb_connector_helper_get_modes,
};

static const struct drm_connector_funcs pmosfb_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs pmosfb_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/*
 * Backlight
 */

static int pmosfb_bl_update_status(struct backlight_device *bl)
{
	struct pmosfb_device *sdev = bl_get_data(bl);
	unsigned int brightness = bl->props.brightness;
	u32 value;

	if (!sdev->disp_pwm_base)
		return 0;

	if (brightness > bl->props.max_brightness)
		brightness = bl->props.max_brightness;

	value = sdev->disp_pwm_con1_shadow;
	if (sdev->disp_pwm_high_width_max) {
		u32 high_width = (sdev->disp_pwm_high_width_max * brightness)
				  / (bl->props.max_brightness + 1);

		value &= ~DISP_PWM_CON1_HIGH_WIDTH_MASK;
		value |= (high_width << 16) & DISP_PWM_CON1_HIGH_WIDTH_MASK;
		sdev->disp_pwm_con1_shadow = value;
	}
	writel(value, sdev->disp_pwm_base + DISP_PWM_CON1);

	return 0;
}

static int pmosfb_bl_get_brightness(struct backlight_device *bl)
{
	struct pmosfb_device *sdev = bl_get_data(bl);

	if (!sdev->disp_pwm_base || !sdev->disp_pwm_high_width_max)
		return 0;

	return (sdev->disp_pwm_high_width_max * bl->props.brightness)
	       / (bl->props.max_brightness + 1);
}

static const struct backlight_ops pmosfb_bl_ops = {
	.update_status = pmosfb_bl_update_status,
	.get_brightness = pmosfb_bl_get_brightness,
};

/*
 * The boot DT describes the backlight as led_mode=4 (DISP_PWM) with
 * max-brightness 0xff. We map userspace 0..255 onto the PWM high width
 * the bootloader already programmed (which caps the usable range).
 */
static void pmosfb_backlight_init(struct pmosfb_device *sdev,
				  struct platform_device *pdev)
{
	struct device_node *pwm_np, *panel_np;
	struct backlight_properties props = { 0 };
	struct backlight_device *bl;

	pwm_np = of_find_compatible_node(NULL, NULL, "mediatek,mt6789-disp-pwm");
	if (!pwm_np)
		pwm_np = of_find_compatible_node(NULL, NULL, "mediatek,mt6789-disp-pwm0");
	if (!pwm_np)
		pwm_np = of_find_compatible_node(NULL, NULL, "mediatek,disp-pwm");
	panel_np = of_find_node_by_name(NULL, "panel1");
	if (!panel_np)
		panel_np = of_find_compatible_node(NULL, NULL, "boe,nt36523n,vdo");

	if (pwm_np) {
		sdev->disp_pwm_base = of_iomap(pwm_np, 0);
		if (sdev->disp_pwm_base) {
			u32 con1 = readl(sdev->disp_pwm_base + DISP_PWM_CON1);

			sdev->disp_pwm_con1_shadow = con1;
			sdev->disp_pwm_high_width_max =
				(con1 >> 16) & 0x1fff;
			if (!sdev->disp_pwm_high_width_max)
				sdev->disp_pwm_high_width_max = 0x7ff;
			pr_info("pmosfb: disp-pwm @ %px con1=0x%08x hw_max=0x%x\n",
				sdev->disp_pwm_base, con1,
				sdev->disp_pwm_high_width_max);
		} else {
			pr_warn("pmosfb: could not iomap disp-pwm node\n");
		}
		of_node_put(pwm_np);
	} else {
		pr_warn("pmosfb: no disp-pwm node found; backlight sleep\n");
	}

	if (panel_np) {
		int gpio = of_get_named_gpio(panel_np, "bl-enable", 0);

		if (gpio_is_valid(gpio)) {
			int ret = devm_gpio_request_one(&pdev->dev, gpio,
							GPIOF_OUT_INIT_HIGH,
							"pmosfb-bl-en");

			if (ret) {
				sdev->bl_enable_gpio = -EINVAL;
				pr_warn("pmosfb: bl-enable gpio %d request failed: %d\n",
					gpio, ret);
			} else {
				sdev->bl_enable_gpio = gpio;
				pr_info("pmosfb: bl-enable gpio %d high\n", gpio);
			}
		} else {
			sdev->bl_enable_gpio = -EINVAL;
			pr_warn("pmosfb: no bl-enable gpio (%d); panel already lit\n",
				gpio);
		}
		of_node_put(panel_np);
	} else {
		sdev->bl_enable_gpio = -EINVAL;
		pr_warn("pmosfb: no panel1 node; backlight stays at boot value\n");
	}

	props.type = BACKLIGHT_RAW;
	props.max_brightness = 255;
	props.brightness = 255;

	bl = devm_backlight_device_register(&pdev->dev, "lcd-backlight", &pdev->dev,
					    sdev, &pmosfb_bl_ops, &props);
	if (IS_ERR(bl)) {
		pr_warn("pmosfb: backlight register failed: %ld\n", PTR_ERR(bl));
		return;
	}
	sdev->bl = bl;
	backlight_update_status(bl);
}

/*
 * DRM driver
 */

DEFINE_DRM_GEM_FOPS(pmosfb_fops);

static struct drm_driver pmosfb_driver = {
	DRM_GEM_SHMEM_DRIVER_OPS,
	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.date			= DRIVER_DATE,
	.major			= DRIVER_MAJOR,
	.minor			= DRIVER_MINOR,
	.driver_features	= DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,
	.fops			= &pmosfb_fops,
};

/*
 * Platform driver
 */

static struct drm_display_mode pmosfb_mode(unsigned int width,
					   unsigned int height,
					   unsigned int width_mm,
					   unsigned int height_mm)
{
	struct drm_display_mode mode = {
		DRM_MODE_INIT(60, width, height, width_mm, height_mm)
	};

	mode.type |= DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
	drm_mode_set_name(&mode);

	return mode;
}

static const struct drm_format_info *
pmosfb_get_validated_format(struct drm_device *dev, const char *format_name)
{
	static const struct simplefb_format formats[] = SIMPLEFB_FORMATS;
	const struct simplefb_format *fmt = formats;
	const struct simplefb_format *end = fmt + ARRAY_SIZE(formats);

	if (!format_name) {
		drm_err(dev, "pmosfb: missing framebuffer format\n");
		return ERR_PTR(-EINVAL);
	}

	while (fmt < end) {
		if (!strcmp(format_name, fmt->name)) {
			const struct drm_format_info *info;

			info = drm_format_info(fmt->fourcc);
			if (!info)
				return ERR_PTR(-EINVAL);
			return info;
		}
		++fmt;
	}

	drm_err(dev, "pmosfb: unknown framebuffer format %s\n", format_name);
	return ERR_PTR(-EINVAL);
}

static struct pmosfb_device *pmosfb_device_create(struct drm_driver *drv,
						  struct platform_device *pdev)
{
	const struct simplefb_platform_data *pd = dev_get_platdata(&pdev->dev);
	struct pmosfb_device *sdev;
	struct drm_device *dev;
	int width, height, stride;
	int width_mm = 0, height_mm = 0;
	const struct drm_format_info *format;
	struct resource *res;
	struct drm_plane *primary_plane;
	struct drm_crtc *crtc;
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	unsigned long max_width, max_height;
	size_t nformats;
	int ret;

	sdev = devm_drm_dev_alloc(&pdev->dev, drv, struct pmosfb_device, dev);
	if (IS_ERR(sdev))
		return ERR_CAST(sdev);
	dev = &sdev->dev;
	platform_set_drvdata(pdev, sdev);

	if (!pd) {
		drm_err(dev, "no pmosfb platform data found\n");
		return ERR_PTR(-ENODEV);
	}

	/*
	 * Hardware settings
	 */

	width = pd->width;
	height = pd->height;
	stride = pd->stride;
	format = pmosfb_get_validated_format(dev, pd->format);
	if (IS_ERR(format) || !width || !height) {
		drm_err(dev, "pmosfb: bad framebuffer geometry %dx%d fmt=%s\n",
			width, height, pd->format ? pd->format : "(null)");
		return ERR_CAST(format);
	}
	/*
	 * Assume a monitor resolution of 96 dpi if physical dimensions
	 * are not specified to get a somewhat reasonable screen size.
	 */
	width_mm = DRM_MODE_RES_MM(width, 96ul);
	height_mm = DRM_MODE_RES_MM(height, 96ul);

	sdev->mode = pmosfb_mode(width, height, width_mm, height_mm);
	sdev->format = format;
	sdev->pitch = stride;

	drm_dbg(dev, "display mode={" DRM_MODE_FMT "}\n", DRM_MODE_ARG(&sdev->mode));
	drm_dbg(dev, "framebuffer format=%p4cc, size=%dx%d, stride=%d byte\n",
		&format->format, width, height, stride);

	/*
	 * Memory management
	 */

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return ERR_PTR(-EINVAL);

	drm_dbg(dev, "using I/O memory framebuffer at %pr\n", res);

	/*
	 * The scanout buffer is plain System RAM in the reserved carveout.
	 * Map it via the cacheable linear map (this is what the D-cache
	 * clean in the atomic_update path expects).
	 */
	sdev->screen_base = phys_to_virt(res->start);
	if (!sdev->screen_base)
		return ERR_PTR(-ENOMEM);
	drm_info(dev, "pmosfb: using linear map at %p for phys %pr\n",
		 sdev->screen_base, res);

	/*
	 * Modesetting
	 */

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ERR_PTR(ret);

	max_width = max_t(unsigned long, width, DRM_SHADOW_PLANE_MAX_WIDTH);
	max_height = max_t(unsigned long, height, DRM_SHADOW_PLANE_MAX_HEIGHT);

	dev->mode_config.min_width = width;
	dev->mode_config.max_width = max_width;
	dev->mode_config.min_height = height;
	dev->mode_config.max_height = max_height;
	dev->mode_config.preferred_depth = format->depth;
	dev->mode_config.funcs = &pmosfb_mode_config_funcs;

	/* Primary plane */

	nformats = drm_fb_build_fourcc_list(dev, &format->format, 1,
					    sdev->formats, ARRAY_SIZE(sdev->formats));

	primary_plane = &sdev->primary_plane;
	ret = drm_universal_plane_init(dev, primary_plane, 0, &pmosfb_primary_plane_funcs,
				       sdev->formats, nformats,
				       pmosfb_primary_plane_format_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ERR_PTR(ret);
	drm_plane_helper_add(primary_plane, &pmosfb_primary_plane_helper_funcs);
	/*
	 * Do NOT advertise FB_DAMAGE_CLIPS.  Without this property wlroots /
	 * phoc cannot use buffer-age-based partial rendering and is forced to
	 * re-render the full framebuffer on every frame.  This eliminates the
	 * stale-content / "torn lockscreen wallpaper" artefact that appears
	 * after unlock: with damage-clip support the compositor may skip
	 * repainting regions it considers undamaged, leaving old frame content
	 * in the GEM buffer that our full-screen blit faithfully copies into
	 * the scanout memory.  Screen blank / wake forces a full repaint which
	 * is why the artefact auto-resolves after the idle timeout.
	 */

	/* CRTC */

	crtc = &sdev->crtc;
	ret = drm_crtc_init_with_planes(dev, crtc, primary_plane, NULL,
					&pmosfb_crtc_funcs, NULL);
	if (ret)
		return ERR_PTR(ret);
	drm_crtc_helper_add(crtc, &pmosfb_crtc_helper_funcs);

	/* Encoder */

	encoder = &sdev->encoder;
	ret = drm_encoder_init(dev, encoder, &pmosfb_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ERR_PTR(ret);
	encoder->possible_crtcs = drm_crtc_mask(crtc);

	/* Connector */

	connector = &sdev->connector;
	ret = drm_connector_init(dev, connector, &pmosfb_connector_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret)
		return ERR_PTR(ret);
	drm_connector_helper_add(connector, &pmosfb_connector_helper_funcs);
	drm_connector_set_panel_orientation_with_quirk(connector,
						       DRM_MODE_PANEL_ORIENTATION_UNKNOWN,
						       width, height);

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret)
		return ERR_PTR(ret);

	/*
	 * Provide a vblank for the compositor. drm_vblank_init() must be
	 * called once; the synthetic 60 Hz hrtimer below emits the vblank
	 * events.  It is parked until atomic_enable starts it.
	 */
	ret = drm_vblank_init(dev, 1);
	if (ret)
		return ERR_PTR(ret);

	spin_lock_init(&sdev->vblank_lock);
	sdev->vblank_period = ns_to_ktime(NSEC_PER_SEC / PMOSFB_VBLANK_HZ);
	sdev->vblank_enabled = false;
	hrtimer_init(&sdev->vblank_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	sdev->vblank_timer.function = pmosfb_vblank_timer_cb;

	drm_mode_config_reset(dev);

	pmosfb_backlight_init(sdev, pdev);

	return sdev;
}

static int pmosfb_probe(struct platform_device *pdev)
{
	struct pmosfb_device *sdev;
	struct drm_device *dev;
	unsigned int color_mode;
	int ret;

	sdev = pmosfb_device_create(&pmosfb_driver, pdev);
	if (IS_ERR(sdev))
		return PTR_ERR(sdev);
	dev = &sdev->dev;

	ret = drm_dev_register(dev, 0);
	if (ret)
		return ret;

	color_mode = drm_format_info_bpp(sdev->format, 0);
	if (color_mode == 16)
		color_mode = sdev->format->depth; // can be 15 or 16

	drm_fbdev_shmem_setup(dev, color_mode);

	{
		extern bool fb_klog_stop;
		fb_klog_stop = true;
	}

	return 0;
}

static void pmosfb_remove(struct platform_device *pdev)
{
	struct pmosfb_device *sdev = platform_get_drvdata(pdev);
	struct drm_device *dev = &sdev->dev;

	pmosfb_vblank_stop(sdev);
	drm_dev_unplug(dev);
}

static struct platform_driver pmosfb_platform_driver = {
	.driver = {
		.name = "pmos-framebuffer",
	},
	.probe = pmosfb_probe,
	.remove_new = pmosfb_remove,
};

module_platform_driver(pmosfb_platform_driver);

MODULE_AUTHOR("pmOS");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL v2");