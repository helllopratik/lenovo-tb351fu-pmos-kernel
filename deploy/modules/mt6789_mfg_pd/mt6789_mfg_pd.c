// SPDX-License-Identifier: GPL-2.0
/*
 * MT6789 MFG power + clock bring-up for Panfrost.
 *
 * Mainline has no MT6789 SCPSYS driver, so the MFG MTCMOS domains are
 * powered here with the exact sequence the vendor mtk_gpufreq_mt6789
 * module uses (decoded from the shipped vendor_dlkm binary):
 *
 *   mfg0..3 CTL 0x308/0x30c/0x310/0x314, status bits 2..5 of both
 *   SPM_PWR_STATUS (0x16c) and SPM_PWR_STATUS2 (0x170).
 *
 * Enable sequence per domain (__gpufreq_mfgN_control(1)):
 *
 *   v = readl(ctl); writel(v | BIT(2));
 *   v = readl(ctl); writel(v | BIT(3));
 *   poll 0x16c and 0x170 status bit set, 10us steps, 501 tries;
 *   udelay(100);
 *   v = readl(ctl); writel(v & ~BIT(4));
 *   v = readl(ctl); writel(v & ~BIT(1));
 *   v = readl(ctl); writel(v | BIT(0));
 *   v = readl(ctl); writel(v & ~BIT(8));
 *   poll BIT(12) clear, 10us steps, 501 tries.
 *
 * mfg1 additionally releases its EMI handshake in infracfg_ao after the
 * ack clears.  The reference mux (scpsys "mfg" clock, topckgen
 * mfg_ref_sel) is enabled between mfg1 and mfg2, exactly like the vendor
 * __gpufreq_power_control().
 *
 * The vendor then runs __gpufreq_clock_control(1): enable mfg_pll_sel,
 * reparent it to mfgpll_ck, wait for topckgen+0x50 BIT(18), enable the
 * mfgcfg bg3d gate, and program the mfg_top_config block.  Finally the
 * vendor probe sets infracfg_ao+0x290 BIT(9) and mfg_top_config+0x130=3.
 *
 * All clocks/regulators stay enabled on module exit (validation build).
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/regulator/consumer.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/kernel.h>

#define SPM_PWR_STATUS		0x16c
#define SPM_PWR_STATUS2		0x170
#define MFG0_CTL		0x308

#define MFG_CTL_OFF(i)		(MFG0_CTL + 4 * (i))
#define MFG_STA_BIT(i)		BIT(2 + (i))

#define POLL_TRIES		501
#define POLL_UDELAY		10

static void __iomem *spm;
static void __iomem *topckgen;
static void __iomem *mfg_top;
static void __iomem *infracfg_ao;

static struct clk *clk_mfg_pll_sel;	/* gpufreq "clk_mux" */
static struct clk *clk_mfg_ref_sel;	/* gpufreq "clk_ref_mux" */
static struct clk *clk_mfgpll_ck;	/* gpufreq "clk_main_parent" */
static struct clk *clk_subsys_bg3d;	/* gpufreq "subsys_bg3d" */

static struct regulator *vgpu;
static struct regulator *vsram_gpu;

static int mfg_domain_on(unsigned int idx)
{
	void __iomem *ctl = spm + MFG_CTL_OFF(idx);
	u32 bit = MFG_STA_BIT(idx);
	u32 v, s1, s2;
	int i;

	v = readl(ctl);
	s1 = readl(spm + SPM_PWR_STATUS);
	s2 = readl(spm + SPM_PWR_STATUS2);
	pr_info("mt6789_mfg_pd: mfg%u pre  ctl=%08x status=%08x/%08x\n",
		idx, v, s1, s2);

	if ((s1 & bit) && (s2 & bit)) {
		pr_info("mt6789_mfg_pd: mfg%u already on\n", idx);
		return 0;
	}

	v = readl(ctl);
	writel(v | BIT(2), ctl);
	v = readl(ctl);
	writel(v | BIT(3), ctl);

	for (i = 0; i < POLL_TRIES; i++) {
		s1 = readl(spm + SPM_PWR_STATUS);
		s2 = readl(spm + SPM_PWR_STATUS2);
		if ((s1 & bit) && (s2 & bit))
			break;
		udelay(POLL_UDELAY);
	}
	if (i == POLL_TRIES) {
		pr_err("mt6789_mfg_pd: mfg%u power status timeout ctl=%08x status=%08x/%08x\n",
		       idx, readl(ctl), s1, s2);
		return -ETIMEDOUT;
	}
	pr_info("mt6789_mfg_pd: mfg%u status up after %d polls\n", idx, i + 1);

	udelay(100);

	v = readl(ctl);
	writel(v & ~BIT(4), ctl);
	v = readl(ctl);
	writel(v & ~BIT(1), ctl);
	v = readl(ctl);
	writel(v | BIT(0), ctl);
	v = readl(ctl);
	writel(v & ~BIT(8), ctl);

	for (i = 0; i < POLL_TRIES; i++) {
		if (!(readl(ctl) & BIT(12)))
			break;
		udelay(POLL_UDELAY);
	}
	if (i == POLL_TRIES) {
		pr_err("mt6789_mfg_pd: mfg%u ack timeout ctl=%08x\n",
		       idx, readl(ctl));
		return -ETIMEDOUT;
	}

	/* mfg1 carries the EMI interface: release it exactly like the vendor. */
	if (idx == 1) {
		writel(0x80, infracfg_ao + 0x718);
		writel(0x600000, infracfg_ao + 0x2a4);
		writel(0x60, infracfg_ao + 0x718);
		writel(0x200000, infracfg_ao + 0x2ac);
	}

	s1 = readl(spm + SPM_PWR_STATUS);
	s2 = readl(spm + SPM_PWR_STATUS2);
	pr_info("mt6789_mfg_pd: mfg%u post ctl=%08x status=%08x/%08x\n",
		idx, readl(ctl), s1, s2);
	return 0;
}

static void mfg_clock_control(void)
{
	u32 v;
	int i, ret;

	ret = clk_prepare_enable(clk_mfg_pll_sel);
	pr_info("mt6789_mfg_pd: clk_mux enable ret=%d rate=%lu\n",
		ret, clk_get_rate(clk_mfg_pll_sel));
	if (ret)
		return;

	ret = clk_set_parent(clk_mfg_pll_sel, clk_mfgpll_ck);
	pr_info("mt6789_mfg_pd: reparent mfg_pll_sel -> %s ret=%d\n",
		__clk_get_name(clk_mfgpll_ck), ret);

	/* Vendor waits for the mux status bit, retrying the switch 6 times. */
	for (i = 0; i < 6; i++) {
		if (readl(topckgen + 0x50) & BIT(18)) {
			pr_info("mt6789_mfg_pd: mfg mux on main parent (try %d)\n",
				i + 1);
			break;
		}
		udelay(10);
		clk_set_parent(clk_mfg_pll_sel, clk_mfgpll_ck);
	}
	if (!(readl(topckgen + 0x50) & BIT(18)))
		pr_warn("mt6789_mfg_pd: mfg mux status still not main, top+50=%08x\n",
			readl(topckgen + 0x50));

	ret = clk_prepare_enable(clk_subsys_bg3d);
	pr_info("mt6789_mfg_pd: subsys_bg3d enable ret=%d rate=%lu\n",
		ret, clk_get_rate(clk_subsys_bg3d));
	if (ret)
		return;

	v = readl(mfg_top + 0x20);
	writel(v | 0x03c00000, mfg_top + 0x20);
	v = readl(mfg_top + 0xb0);
	writel(v & ~0x600, mfg_top + 0xb0);
	v = readl(mfg_top + 0xb4);
	writel(v | 0x10, mfg_top + 0xb4);
	v = readl(mfg_top + 0xb0);
	writel((v & ~0x100) | 0x80000, mfg_top + 0xb0);
	v = readl(mfg_top + 0x24);
	writel(v | 0x1, mfg_top + 0x24);
	v = readl(mfg_top + 0xf60);
	writel(v | 0x1, mfg_top + 0xf60);

	pr_info("mt6789_mfg_pd: mfg_top +20=%08x +24=%08x +b0=%08x +b4=%08x +f60=%08x\n",
		readl(mfg_top + 0x20), readl(mfg_top + 0x24),
		readl(mfg_top + 0xb0), readl(mfg_top + 0xb4),
		readl(mfg_top + 0xf60));
}

static int __init mfg_pd_init(void)
{
	struct device_node *np;
	int idx, ret;

	np = of_find_compatible_node(NULL, NULL, "mediatek,gpufreq");
	if (!np) {
		pr_err("mt6789_mfg_pd: gpufreq node not found\n");
		return -ENODEV;
	}

	idx = of_property_match_string(np, "reg-names", "sleep");
	if (idx < 0) {
		pr_err("mt6789_mfg_pd: no sleep reg\n");
		ret = idx;
		goto err_put;
	}
	spm = of_iomap(np, idx);
	idx = of_property_match_string(np, "reg-names", "mfg_top_config");
	if (idx < 0) {
		pr_err("mt6789_mfg_pd: no mfg_top_config reg\n");
		ret = idx;
		goto err_unmap;
	}
	mfg_top = of_iomap(np, idx);
	idx = of_property_match_string(np, "reg-names", "infracfg_ao");
	if (idx < 0) {
		pr_err("mt6789_mfg_pd: no infracfg_ao reg\n");
		ret = idx;
		goto err_unmap;
	}
	infracfg_ao = of_iomap(np, idx);
	idx = of_property_match_string(np, "reg-names", "topckgen");
	if (idx < 0) {
		pr_err("mt6789_mfg_pd: no topckgen reg\n");
		ret = idx;
		goto err_unmap;
	}
	topckgen = of_iomap(np, idx);
	if (!spm || !mfg_top || !infracfg_ao || !topckgen) {
		pr_err("mt6789_mfg_pd: ioremap failed\n");
		ret = -ENOMEM;
		goto err_unmap;
	}

	clk_mfg_pll_sel = of_clk_get_by_name(np, "clk_mux");
	clk_mfg_ref_sel = of_clk_get_by_name(np, "clk_ref_mux");
	clk_mfgpll_ck = of_clk_get_by_name(np, "clk_main_parent");
	clk_subsys_bg3d = of_clk_get_by_name(np, "subsys_bg3d");
	if (IS_ERR(clk_mfg_pll_sel) || IS_ERR(clk_mfg_ref_sel) ||
	    IS_ERR(clk_mfgpll_ck) || IS_ERR(clk_subsys_bg3d)) {
		pr_err("mt6789_mfg_pd: clock lookup failed: %ld %ld %ld %ld\n",
		       PTR_ERR(clk_mfg_pll_sel), PTR_ERR(clk_mfg_ref_sel),
		       PTR_ERR(clk_mfgpll_ck), PTR_ERR(clk_subsys_bg3d));
		ret = -ENODEV;
		goto err_unmap;
	}
	pr_info("mt6789_mfg_pd: clocks mux=%s ref=%s main=%s bg3d=%s\n",
		__clk_get_name(clk_mfg_pll_sel), __clk_get_name(clk_mfg_ref_sel),
		__clk_get_name(clk_mfgpll_ck), __clk_get_name(clk_subsys_bg3d));

	vgpu = regulator_get(NULL, "VGPU");
	vsram_gpu = regulator_get(NULL, "VSRAM_GPU");
	if (IS_ERR(vgpu) || IS_ERR(vsram_gpu)) {
		pr_err("mt6789_mfg_pd: regulator lookup failed: %ld %ld\n",
		       PTR_ERR(vgpu), PTR_ERR(vsram_gpu));
		ret = -ENODEV;
		goto err_unmap;
	}

	ret = regulator_enable(vgpu);
	pr_info("mt6789_mfg_pd: VGPU enable ret=%d\n", ret);
	if (ret)
		goto err_unmap;
	ret = regulator_enable(vsram_gpu);
	pr_info("mt6789_mfg_pd: VSRAM_GPU enable ret=%d\n", ret);
	if (ret)
		goto err_unmap;

	ret = mfg_domain_on(0);
	if (ret)
		goto err_out;
	ret = mfg_domain_on(1);
	if (ret)
		goto err_out;

	ret = clk_prepare_enable(clk_mfg_ref_sel);
	pr_info("mt6789_mfg_pd: mfg_ref_sel enable ret=%d rate=%lu\n",
		ret, clk_get_rate(clk_mfg_ref_sel));
	if (ret)
		goto err_out;

	ret = mfg_domain_on(2);
	if (ret)
		goto err_out;
	ret = mfg_domain_on(3);
	if (ret)
		goto err_out;

	mfg_clock_control();

	/* __gpufreq_power_control(1) tail + probe-time bus setup. */
	writel(3, mfg_top + 0x130);
	writel(readl(infracfg_ao + 0x290) | 0x200, infracfg_ao + 0x290);
	pr_info("mt6789_mfg_pd: mfg_top+130=%08x infracfg_ao+290=%08x\n",
		readl(mfg_top + 0x130), readl(infracfg_ao + 0x290));

	of_node_put(np);
	pr_info("mt6789_mfg_pd: MFG bring-up complete\n");
	return 0;

err_out:
	pr_err("mt6789_mfg_pd: bring-up failed at step, clocks/rails left on\n");
err_unmap:
	of_node_put(np);
	return ret;
err_put:
	of_node_put(np);
	return ret;
}

static void __exit mfg_pd_exit(void)
{
	/*
	 * Validation build: leave the GPU rails, clocks and MTCMOS domains
	 * up.  Panfrost keeps using them after this module is unloaded.
	 */
}

module_init(mfg_pd_init);
module_exit(mfg_pd_exit);

MODULE_DESCRIPTION("MT6789 MFG power domains + clocks for Panfrost");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("PMOS TB351FU porting");
