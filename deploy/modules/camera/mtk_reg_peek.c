// SPDX-License-Identifier: GPL-2.0
/*
 * Tiny MMIO peek/poke helper for SENINF/CAMSV bring-up debugging.
 *
 * All accesses are bounded by [off, off + len) inside a single ioremap of
 * `count` bytes.  Never add fixed-offset reads outside that window: doing so
 * from a non-page-aligned base causes a translation fault and, with
 * panic_on_oops=1, an automatic reboot.
 */
#include <linux/module.h>
#include <linux/io.h>

static unsigned long base = 0x1a004000;
module_param(base, ulong, 0444);

static unsigned int count = 0x1000;
module_param(count, uint, 0444);

static unsigned int off;
module_param(off, uint, 0444);

static unsigned int len = 0x100;
module_param(len, uint, 0444);

static unsigned int wr_off = 0xffffffff;
module_param(wr_off, uint, 0444);

static unsigned int wr_val;
module_param(wr_val, uint, 0444);

static int __init peek_init(void)
{
	void __iomem *m;
	unsigned int i, end;

	if (count == 0 || count > 0x10000)
		count = 0x1000;

	off &= ~3u;
	if (off >= count)
		off = 0;

	if (len == 0 || len > count - off)
		len = count - off;

	end = off + len;

	m = ioremap(base, count);
	if (!m) {
		pr_err("mtk_reg_peek: ioremap 0x%lx+0x%x failed\n", base, count);
		return -ENOMEM;
	}

	for (i = off; i + 4 <= end; i += 4)
		pr_info("mtk_reg_peek: 0x%08lx = 0x%08x\n",
			base + i, readl_relaxed(m + i));

	if (wr_off != 0xffffffff) {
		if (wr_off + 4 > count) {
			pr_err("mtk_reg_peek: write offset 0x%x outside 0x%x mapping\n",
			       wr_off, count);
		} else {
			unsigned int old = readl_relaxed(m + wr_off);

			writel_relaxed(wr_val, m + wr_off);
			pr_info("mtk_reg_peek: write 0x%08lx <- 0x%08x (old 0x%08x) reads 0x%08x\n",
				base + wr_off, wr_val, old,
				readl_relaxed(m + wr_off));
		}
	}

	iounmap(m);
	return 0;
}

static void __exit peek_exit(void)
{
}

module_init(peek_init);
module_exit(peek_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MTK SENINF MMIO peek helper");
