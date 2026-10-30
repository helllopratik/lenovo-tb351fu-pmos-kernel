/*
 * Framebuffer text dump for TB351FU bring-up (attempt 184 rewrite).
 *
 * Renders a kernel-log tail as 8x16 text on the LK scanout framebuffer
 * at physical 0xfe2b0000.  Storage- and console-independent: a panic
 * reason can be read straight off the panel even when UFS/rootfs/USB
 * are all dead.
 *
 * Fixes vs pre-178 versions:
 *  - The old code mapped only 100 pixel-rows but rendered 24 glyph rows
 *    x 8px = 192 rows, i.e. it wrote ~441KB past the end of its own
 *    early_memremap() window on every call -> memory corruption and a
 *    hard freeze right after free_initmem().  That was the root cause
 *    of the "black screen + thin red line" hangs.
 *  - early_memremap() use from kthread context raced with the color-bar
 *    draws in init/main.c (same fixmap slots).  We now create ONE
 *    ioremap_wc() window at device_initcall time and never remap.
 *  - The corrupted 8x8 font table (index-prefix bytes polluting the
 *    glyphs) is replaced with the real VGA fontdata_8x16 glyphs.
 *  - A kmsg dumper renders the panic/oops log tail on screen.
 */

#include <linux/kmsg_dump.h>
#include <linux/io.h>
#include <linux/string.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/reboot.h>
#include <asm/cacheflush.h>

#define DBG_FB_PHYS	0xfe2b0000UL
#define DBG_FB_BPP	4
#define DBG_FB_WIDTH	1200			/* visible pixels */
#define DBG_FB_PITCH	(1216 * DBG_FB_BPP)	/* real scanout stride: 4864 B!
						 * panel scans the portrait buffer
						 * rotated; pitch is 64-byte aligned
						 * (1216 px), NOT 1200*4. Using 4800
						 * garbles text progressively. */

/* Full-screen console, 2x-scaled glyphs for photographable output.
 * 8x16 font drawn as 16x32 px cells: 75 cols x 62 rows on 1200x2000. */
#define DBG_TEXT_TOPROW	0
#define DBG_MAP_ROWS	2000			/* pixel rows covered */
#define DBG_SCALE	1
#define DBG_CELL_W	(8 * DBG_SCALE)
#define DBG_CELL_H	(16 * DBG_SCALE)
#define DBG_TEXT_ROWS	(DBG_MAP_ROWS / DBG_CELL_H)
#define DBG_TEXT_COLS	(1200 / DBG_CELL_W)

/* Filtered-log window rendered each refresh. */
#define DBG_KEEP_LINES	(DBG_TEXT_ROWS - 1)
#define DBG_LINE_MAX	(DBG_TEXT_COLS + 2)

#define DBG_WIN_SIZE	(DBG_MAP_ROWS * DBG_FB_PITCH)

/*
 * 0xfe2b0000 is plain DRAM (LK mblock "framebuffer"), so ioremap_wc()
 * refuses to map it on arm64 (it is kernel System RAM) and silently
 * returns NULL - which is why no text ever appeared with the previous
 * approach.  Use the linear-map alias via phys_to_virt() instead and
 * clean the D-cache to Point-of-Coherency after each render so the
 * scanout DMA engine sees the pixels.
 */
static unsigned char *fb_lin;
static DEFINE_SPINLOCK(fb_win_lock);

/*
 * VGA 8x16 font, first 128 glyphs of Linux fontdata_8x16
 * (extracted verbatim from lib/fonts/font_8x16.c).
 */
static const unsigned char dbg_font8x16[128 * 16] = {
#include "../pmos_init_src/font8x16_128.inc"
};

/* Substring match, case-insensitive, no libc. */
static bool fb_line_matches(const char *s, const char *key)
{
	size_t n = strlen(key);

	while (*s) {
		size_t i;

		for (i = 0; i < n; i++) {
			char a = s[i], b = key[i];

			if (a >= 'A' && a <= 'Z')
				a += 32;
			if (b >= 'A' && b <= 'Z')
				b += 32;
			if (a != b)
				break;
		}
		if (i == n)
			return true;
		s++;
	}
	return false;
}

static const char * const fb_keep_keys[] = {
	"pmosboot", "pmos", "tb351fu", "attempt",
	"pinctrl", "clk-mt6789", "clkcommon", "pwrap", "mt6397", "mt6358", "regulator",
	"mmcblk", "msdc", "sdhci", "ufshci", "dvfsrc", "ulposc",
	"rc=", "card", "mount", "vfat", "ext4", "switch_root", "newroot", "initramfs",
	"failed", "error", "deferred", "panic", "oops", "bug:", "call trace",
	"unable to handle", "internal error", "fatal", "pmoscapture", "pmos-dd", NULL,
};

/* High-volume probe noise that would otherwise drown the window. */
static const char * const fb_drop_keys[] = {
	"minix filesystem found", "minix", "ramdisk:", "RAMDISK:",
	"pinctrl deferred", "deferred probe", "spi transfer timed out",
	"noqueue transfer failed", "failed to transfer one message", NULL,
};

static bool fb_line_dropped(const char *s)
{
	int i;

	for (i = 0; fb_drop_keys[i]; i++)
		if (fb_line_matches(s, fb_drop_keys[i]))
			return true;
	return false;
}

/* Render ONLY the lines that matter for debugging: milestones, display
 * handoff census and every failure signature, newest-last, sized to fit
 * the 2x-scaled grid so a single photo captures everything. */
static unsigned long fb_seen, fb_dropped;
static char fb_last_raw[DBG_LINE_MAX];
static unsigned int fb_last_rep;
static int fb_last_idx = -1;

static void fb_klog_ring_tail(char *ring, size_t cap)
{
	static char kept[DBG_KEEP_LINES][DBG_LINE_MAX];
	int khead = 0, kcount = 0;
	struct kmsg_dump_iter iter;
	size_t rlen = 0;
	int i, row;

	fb_seen = 0;
	fb_dropped = 0;
	kmsg_dump_rewind(&iter);
	for (;;) {
		char line[256];
		const char *s = line;
		bool keep = false;
		size_t len = 0;

		if (!kmsg_dump_get_line(&iter, false, line, sizeof(line), &len))
			break;
		line[sizeof(line) - 1] = '\0';

		/* Strip syslog level prefix <N> */
		while (*s == '<' || (*s >= '0' && *s <= '7') || *s == '>')
			s++;
		while (*s == ' ')
			s++;

		/* Strip kernel timestamps/caller prefixes [  1.234567][   T1] */
		while (*s == '[') {
			const char *cl = strchr(s, ']');
			if (cl) {
				s = cl + 1;
				while (*s == ' ')
					s++;
			} else {
				break;
			}
		}

		fb_seen++;
		for (i = 0; fb_keep_keys[i]; i++)
			if (fb_line_matches(s, fb_keep_keys[i])) {
				keep = true;
				break;
			}
		if (keep && fb_line_dropped(s))
			keep = false;
		if (!keep) {
			fb_dropped++;
			continue;
		}
		if (fb_last_idx >= 0 &&
		    strncmp(fb_last_raw, s, DBG_LINE_MAX - 1) == 0) {
			fb_last_rep++;
			snprintf(kept[fb_last_idx], DBG_LINE_MAX,
				 "%.*s [+%u]", 58, fb_last_raw, fb_last_rep);
			kept[fb_last_idx][DBG_LINE_MAX - 1] = '\0';
			continue;
		}
		memcpy(fb_last_raw, s, DBG_LINE_MAX - 1);
		fb_last_raw[DBG_LINE_MAX - 1] = '\0';
		memcpy(kept[khead], s, DBG_LINE_MAX - 1);
		kept[khead][DBG_LINE_MAX - 1] = '\0';
		fb_last_rep = 1;
		fb_last_idx = khead;
		khead = (khead + 1) % DBG_KEEP_LINES;
		if (kcount < DBG_KEEP_LINES)
			kcount++;
	}

	row = 0;
	i = (kcount == DBG_KEEP_LINES) ? khead : 0;
	while (row < kcount && rlen + DBG_LINE_MAX + 1 < cap) {
		size_t l = strnlen(kept[i], DBG_LINE_MAX);

		memcpy(ring + rlen, kept[i], l);
		rlen += l;
		ring[rlen++] = '\n';
		i = (i + 1) % DBG_KEEP_LINES;
		row++;
	}
	ring[rlen] = '\0';
}

void __ref fb_text_dump(const char *title, const char *text)
{
	unsigned long flags;
	bool locked;
	int row;

	if (!fb_lin)
		return;

	/*
	 * Trylock only: during panic() the other CPUs may have been stopped
	 * while holding this lock, so blocking would deadlock the panic
	 * path.  Worst case without the lock is a torn frame.
	 */
	locked = spin_trylock_irqsave(&fb_win_lock, flags);

	for (row = 0; row < DBG_TEXT_ROWS; row++) {
		char buf[DBG_LINE_MAX];
		const char *s;
		int x;

		if (row == 0) {
			s = title;
		} else if (text && *text) {
			size_t l = 0;

			while (*text && *text != '\n' && l < DBG_LINE_MAX - 1)
				buf[l++] = *text++;
			if (*text == '\n')
				text++;
			buf[l] = '\0';
			s = buf;
		} else {
			s = "";
		}

		for (x = 0; x < DBG_TEXT_COLS; x++) {
			unsigned char c = s[x] ? (unsigned char)s[x] : 0x20;
			const unsigned char *g;
			int r, cc;

			if (c < 0x20 || c > 0x7e)
				c = 0x20;
			g = &dbg_font8x16[(c & 0x7f) * 16];
			for (r = 0; r < DBG_CELL_H; r++) {
				unsigned char bits = g[r / DBG_SCALE];
				unsigned int *pp = (unsigned int *)
					(fb_lin + (row * DBG_CELL_H + r) *
						  DBG_FB_PITCH) +
					x * DBG_CELL_W;

				for (cc = 0; cc < DBG_CELL_W; cc++)
					pp[cc] =
					(bits & (0x80 >> (cc / DBG_SCALE))) ?
						0xFFFFFFFF : 0xFF000000;
			}
		}
	}

	/* Push pixels out of the caches so scanout DMA can read them. */
	dcache_clean_poc((unsigned long)fb_lin,
			 (unsigned long)fb_lin + DBG_WIN_SIZE);

	if (locked)
		spin_unlock_irqrestore(&fb_win_lock, flags);
}

void __ref fb_text_panic_dump(void)
{
	static char ring[20480];

	fb_klog_ring_tail(ring, sizeof(ring));
	fb_text_dump("PANIC/OOPS: <kernel log tail>", ring);
}

static void fb_panic_dump_fn(struct kmsg_dumper *dumper,
			     struct kmsg_dump_detail *detail)
{
	void __iomem *wdt;

	if (!detail || detail->reason == KMSG_DUMP_SHUTDOWN)
		return;
	fb_text_panic_dump();
	mdelay(3000);

	/* Pulse MediaTek TopRGU Hardware Watchdog Reset directly */
	wdt = ioremap(0x10007000, 0x100);
	if (wdt) {
		writel(0x22000000 | (1 << 0) | (1 << 2) | (1 << 6), wdt + 0x00);
		wmb();
		writel(0x1209, wdt + 0x14);
		wmb();
	}
	emergency_restart();
}

static struct kmsg_dumper fb_panic_kd = {
	.dump = fb_panic_dump_fn,
	.max_reason = KMSG_DUMP_MAX,
};

static int __init fbtext_init(void)
{
	int ret;

	/* Linear-map alias over rows 400..599 of the reserved framebuffer. */
	fb_lin = phys_to_virt(DBG_FB_PHYS + (DBG_TEXT_TOPROW * DBG_FB_PITCH));
	if (!fb_lin) {
		pr_err("fbtext: no linear alias for debug framebuffer\n");
		return -ENOMEM;
	}

	ret = kmsg_dump_register(&fb_panic_kd);
	if (ret)
		pr_err("fbtext: kmsg_dump_register failed %d\n", ret);

	/* Paint an initial banner so we know the window works. */
	fb_text_dump("TB351FU ATTEMPT 213 FLOODFIX",
		     "on-screen logging is ALIVE");

	return 0;
}
device_initcall(fbtext_init);

bool fb_klog_stop = false;
EXPORT_SYMBOL(fb_klog_stop);

static int fb_klog_thread(void *arg)
{
	static char ring[20480];
	static char title[DBG_LINE_MAX + 32];
	unsigned long paints = 0;

	for (;;) {
		if (fb_klog_stop)
			break;
		fb_klog_ring_tail(ring, sizeof(ring));
		scnprintf(title, sizeof(title),
			  "T267 s=%lu k=%lu d=%lu paint=%lu",
			  fb_seen, fb_seen - fb_dropped, fb_dropped,
			  ++paints);
		fb_text_dump(title, ring);
		ssleep(2);
	}
	(void)arg;
	return 0;
}

void fb_klog_start(void)
{
	struct task_struct *t;

	t = kthread_create(fb_klog_thread, NULL, "fbklog");
	if (IS_ERR(t))
		return;
	wake_up_process(t);
}
