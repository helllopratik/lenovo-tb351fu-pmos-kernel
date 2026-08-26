/*
 * Checkpoint log: persists a stage marker plus the printk ring tail to a
 * fixed region of the expdb partition (sdc3, 8:35) on every boot
 * checkpoint.  Survives any force-reboot because partition contents are
 * persistent; read it back from recovery with:
 *   dd if=/dev/block/sdc3 bs=4096 skip=2048 count=256
 * (records start at offset 0x800000, 64 slots x 4 KB)
 */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/ckptlog.h>
#include <linux/crc32.h>
#include <linux/jiffies.h>
#include <linux/kmsg_dump.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/string.h>

#define CKPT_MAGIC 0x4B435046
#define CKPT_MAJOR 8
#define CKPT_MINOR 35
#define CKPT_BASE  0x800000ULL
#define CKPT_MAX   64

struct ckpt_hdr {
	u32 magic;
	u32 seq;
	u64 jiffies;
	u64 nsec;
	u32 len;
	u32 crc;
	char stage[64];
	u8 payload[3968];
};

static struct block_device *ckpt_bdev;
static struct ckpt_hdr *ckpt_page;
static u32 ckpt_seq;
static unsigned int ckpt_open_fails;
static bool ckpt_open_disabled;
#define CKPT_OPEN_MAX_FAILS 10

/*
 * Capture the printk ring tail directly WITHOUT triggering the global
 * kmsg dumpers (the old per-second kmsg_dump_desc(KMSG_DUMP_OOPS)
 * invoked pstore/ramoops and every other dumper continuously).
 */
static void ckpt_fill_ring_tail(void)
{
	struct kmsg_dump_iter iter;
	size_t rlen = 0;

	ckpt_page->payload[0] = '\0';
	kmsg_dump_rewind(&iter);
	for (;;) {
		char line[512];
		size_t len = 0;

		if (!kmsg_dump_get_line(&iter, false, line, sizeof(line), &len))
			break;
		if (rlen + len + 1 > sizeof(ckpt_page->payload)) {
			size_t drop = rlen + len + 1 - sizeof(ckpt_page->payload);
			memmove(ckpt_page->payload, ckpt_page->payload + drop,
				rlen - drop);
			memcpy(ckpt_page->payload + rlen - drop, line, len);
			ckpt_page->payload[sizeof(ckpt_page->payload) - 1] = '\0';
			rlen = sizeof(ckpt_page->payload) - 1;
		} else {
			memcpy(ckpt_page->payload + rlen, line, len);
			rlen += len;
			ckpt_page->payload[rlen++] = '\n';
		}
	}
	ckpt_page->payload[rlen] = '\0';
	ckpt_page->len = rlen;
}

static void ckpt_open(void)
{
	struct file *f;

	if (ckpt_bdev || ckpt_open_disabled)
		return;
	f = bdev_file_open_by_dev(MKDEV(CKPT_MAJOR, CKPT_MINOR),
				  BLK_OPEN_WRITE, NULL, NULL);
	if (!IS_ERR(f)) {
		ckpt_bdev = file_bdev(f);
		ckpt_bdev = file_bdev(f);
		pr_info("ckptlog: opened expdb (%d:%d), %llu sectors\n",
			CKPT_MAJOR, CKPT_MINOR,
			(unsigned long long)bdev_nr_sectors(ckpt_bdev));
	} else {
		if (++ckpt_open_fails >= CKPT_OPEN_MAX_FAILS) {
			ckpt_open_disabled = true;
			pr_info("ckptlog: giving up after %d open failures\n",
				ckpt_open_fails);
		}
	}
}

void ckpt_checkpoint(const char *stage)
{
	struct bio *bio;
	struct page *page;
	u32 seq;
	u64 sector;
	gfp_t gfp;

	if (!ckpt_page)
		return;

	seq = ++ckpt_seq;
	memset(ckpt_page, 0, sizeof(*ckpt_page));
	ckpt_page->magic = CKPT_MAGIC;
	ckpt_page->seq = seq;
	ckpt_page->jiffies = get_jiffies_64();
	ckpt_page->nsec = ktime_get_ns();
	strscpy(ckpt_page->stage, stage, sizeof(ckpt_page->stage));

	ckpt_fill_ring_tail();
	ckpt_page->crc = crc32(~0, ckpt_page->payload, ckpt_page->len);

	ckpt_open();
	if (!ckpt_bdev)
		return;

	sector = (CKPT_BASE + (u64)(seq % CKPT_MAX) * sizeof(*ckpt_page)) >>
		 SECTOR_SHIFT;
	/* Refuse to write past the end of the partition. */
	if (sector + (sizeof(*ckpt_page) >> SECTOR_SHIFT) >=
	    (u64)bdev_nr_sectors(ckpt_bdev))
		return;

	page = virt_to_page(ckpt_page);
	gfp = in_atomic() || in_interrupt() ? GFP_ATOMIC : GFP_KERNEL;
	bio = bio_alloc(ckpt_bdev, 1, REQ_OP_WRITE | REQ_SYNC | REQ_FUA, gfp);
	if (!bio)
		return;
	bio->bi_iter.bi_sector = sector;
	if (bio_add_page(bio, page, sizeof(*ckpt_page), offset_in_page(ckpt_page)) != sizeof(*ckpt_page)) {
		bio_put(bio);
		return;
	}
	if (!submit_bio_wait(bio)) {
		extern void draw_debug_color_bar(unsigned int color, int vertical_offset);
		static bool shown;

		if (!shown) {
			shown = true;
			draw_debug_color_bar(0xFF00FF00, 1800); /* Y=1800 GREEN - checkpoint write OK */
		}
	}
	bio_put(bio);
}

#include <linux/kthread.h>
#include <linux/delay.h>

static int ckpt_kthread_fn(void *data)
{
	while (!kthread_should_stop()) {
		ckpt_checkpoint("periodic");
		msleep(5000);
	}
	return 0;
}

static int __init ckpt_init(void)
{
	struct page *p;

	p = alloc_page(GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	ckpt_page = page_address(p);

	kthread_run(ckpt_kthread_fn, NULL, "ckptlogd");
	return 0;
}
device_initcall(ckpt_init);
