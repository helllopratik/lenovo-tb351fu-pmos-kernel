// SPDX-License-Identifier: GPL-2.0
/*
 * MT6789 Camera Subsystem Fixup (SENINF & CAMSV) for Lenovo TB351FU
 *
 * 1. Strips power-domains from seninf_top@1a004000 to prevent dev_pm_domain_attach
 *    from timing out (-ETIMEDOUT / -110) against unhandled scpsys domains.
 * 2. Powers on CAM, CAM_RAWA, CAM_RAWB domains via SPM registers.
 * 3. Releases CAM AXI bus protection in infracfg.
 * 4. Enables topckgen cam_sel clock and camsys clock gates.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/bitops.h>

#define SPM_PHYS_BASE		0x10006000UL
#define SPM_MAP_SIZE		0x1000UL
#define INFRACFG_PHYS_BASE	0x10001000UL
#define INFRACFG_MAP_SIZE	0x1000UL
#define TOPCKGEN_PHYS_BASE	0x10000000UL
#define TOPCKGEN_MAP_SIZE	0x1000UL
#define CAMSYS_MAIN_PHYS_BASE	0x1a000000UL
#define CAMSYS_MAP_SIZE		0x1000UL

#define SPM_PWR_STATUS		0x016c
#define SPM_PWR_STATUS_2ND	0x0170

#define PWR_RST_B		BIT(0)
#define PWR_ISO			BIT(1)
#define PWR_ON			BIT(2)
#define PWR_ON_2ND		BIT(3)
#define PWR_CLK_DIS		BIT(4)
#define SRAM_PDN		BIT(8)
#define SRAM_ACK		BIT(12)

#define TOP_CAM_SEL		0x0030
#define TOP_CAM_SEL_CLR		0x0038
#define TOP_CLK_CFG_UPDATE	0x0004
#define TOP_CAM_GATE		BIT(23)
#define TOP_CAM_UPD_SHIFT	10

#define CAMSYS_CG_CLR		0x0008
#define CAMSYS_MAIN_LARB13	BIT(0)
#define CAMSYS_MAIN_LARB14	BIT(2)
#define CAMSYS_MAIN_CAM		BIT(6)
#define CAMSYS_MAIN_CAMTG	BIT(7)
#define CAMSYS_MAIN_SENINF	BIT(8)

#define IFR_MM_CAM_PROT_CLR	0x02d8
#define IFR_MM_CAM_PROT_MASK0	0x5
#define IFR_MM_CAM_PROT_MASK1	0xa

struct cam_domain {
	const char *name;
	u32 ctl_offs;
	u32 sta_mask;
};

static const struct cam_domain cam_domains[] = {
	{ "cam",      0x35c, BIT(23) },
	{ "cam_rawa", 0x360, BIT(24) },
	{ "cam_rawb", 0x364, BIT(25) },
};

static int cam_domain_power_on(void __iomem *spm, const struct cam_domain *d)
{
	u32 val;
	int i;

	if ((readl(spm + SPM_PWR_STATUS) & d->sta_mask) &&
	    (readl(spm + SPM_PWR_STATUS_2ND) & d->sta_mask))
		return 0;

	val = readl(spm + d->ctl_offs);
	writel(val | PWR_ON, spm + d->ctl_offs);
	writel(val | PWR_ON | PWR_ON_2ND, spm + d->ctl_offs);

	for (i = 0; i < 100; i++) {
		if ((readl(spm + SPM_PWR_STATUS) & d->sta_mask) &&
		    (readl(spm + SPM_PWR_STATUS_2ND) & d->sta_mask))
			break;
		usleep_range(500, 1000);
	}
	if (i == 100)
		return -ETIMEDOUT;

	udelay(100);
	val = readl(spm + d->ctl_offs);
	val &= ~PWR_CLK_DIS;
	writel(val, spm + d->ctl_offs);
	val &= ~PWR_ISO;
	writel(val, spm + d->ctl_offs);
	val |= PWR_RST_B;
	writel(val, spm + d->ctl_offs);

	val = readl(spm + d->ctl_offs);
	val &= ~SRAM_PDN;
	writel(val, spm + d->ctl_offs);

	for (i = 0; i < 200; i++) {
		if (!(readl(spm + d->ctl_offs) & SRAM_ACK))
			break;
		usleep_range(500, 1000);
	}

	return 0;
}

static int __init mtk_mt6789_cam_fixup(void)
{
	struct device_node *seninf_node;
	struct property *prop;
	void __iomem *spm, *infracfg, *topckgen, *camsys;
	int i;

	/* 1. Strip power-domains from seninf_top to avoid probe timeouts */
	seninf_node = of_find_node_by_path("/soc/seninf_top@1a004000");
	if (seninf_node) {
		prop = of_find_property(seninf_node, "power-domains", NULL);
		if (prop) {
			int ret = of_remove_property(seninf_node, prop);
			pr_info("pmos-cam-fixup: removed power-domains from seninf_top: %d\n", ret);
		}
		of_node_put(seninf_node);
	}

	/* 2. Power on camera hardware domains and clocks */
	spm = ioremap(SPM_PHYS_BASE, SPM_MAP_SIZE);
	infracfg = ioremap(INFRACFG_PHYS_BASE, INFRACFG_MAP_SIZE);
	topckgen = ioremap(TOPCKGEN_PHYS_BASE, TOPCKGEN_MAP_SIZE);
	camsys = ioremap(CAMSYS_MAIN_PHYS_BASE, CAMSYS_MAP_SIZE);

	if (!spm || !infracfg || !topckgen || !camsys) {
		pr_warn("pmos-cam-fixup: ioremap failed\n");
		goto out_unmap;
	}

	/* Enable topckgen cam_sel clock */
	if (readl(topckgen + TOP_CAM_SEL) & TOP_CAM_GATE) {
		writel(TOP_CAM_GATE, topckgen + TOP_CAM_SEL_CLR);
		writel(BIT(TOP_CAM_UPD_SHIFT), topckgen + TOP_CLK_CFG_UPDATE);
		udelay(1);
	}

	/* Power on domains: CAM, CAM_RAWA, CAM_RAWB */
	for (i = 0; i < ARRAY_SIZE(cam_domains); i++)
		cam_domain_power_on(spm, &cam_domains[i]);

	/* Release AXI bus protection */
	writel(IFR_MM_CAM_PROT_MASK0, infracfg + IFR_MM_CAM_PROT_CLR);
	writel(IFR_MM_CAM_PROT_MASK1, infracfg + IFR_MM_CAM_PROT_CLR);
	udelay(1);

	/* Enable camsys gates */
	writel(CAMSYS_MAIN_LARB13 | CAMSYS_MAIN_LARB14 | CAMSYS_MAIN_CAM |
	       CAMSYS_MAIN_CAMTG | CAMSYS_MAIN_SENINF, camsys + CAMSYS_CG_CLR);
	udelay(1);

	pr_info("pmos-cam-fixup: MT6789 camera power domains and clocks initialized\n");

out_unmap:
	if (camsys) iounmap(camsys);
	if (topckgen) iounmap(topckgen);
	if (infracfg) iounmap(infracfg);
	if (spm) iounmap(spm);

	return 0;
}
core_initcall(mtk_mt6789_cam_fixup);
