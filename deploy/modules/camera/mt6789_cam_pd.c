// SPDX-License-Identifier: GPL-2.0
/*
 * mt6789_cam_pd - power the MT6789 camera SPM power domains (TB351FU pmOS)
 *
 * Why this is needed
 * ------------------
 * The pmOS kernel has no mt6789-scpsys power-domain data.  mt6789_pd_stub.ko
 * only registers eight fake domains (0..7), so every camera consumer whose
 * DT asks for domain 0xC (CAM), 0xD (CAM_RAWA) or 0xE (CAM_RAWB) fails
 * dev_pm_domain_attach() and never probes.  On top of that the stock DTB
 * carries disable-unused-pd-cam{,_rawa,_rawb} nodes, so at the end of a
 * normal Android boot the vendor scpsys driver has already powered the
 * three camera domains off and re-enabled the CAM bus protection.
 *
 * This module powers them back on straight through the always-on SPM, using
 * the exact register values from the stock mtk-scpsys-mt6789.ko:
 *
 *   domain    ctl_offs  status  sram pdn/ack
 *   disp      0x354     BIT(21) BIT(8)/BIT(12)   (parent, normally on)
 *   cam       0x35C     BIT(23) BIT(8)/BIT(12)
 *   cam_rawa  0x360     BIT(24) BIT(8)/BIT(12)
 *   cam_rawb  0x364     BIT(25) BIT(8)/BIT(12)
 *
 * SPM base is 0x10006000, PWR_STATUS at 0x16C and PWR_STATUS_2ND at 0x170.
 * The CAM domain also needs the topckgen cam_sel clock (reg 0x30, mux bits
 * 16..19, gate bit 23, update bit 10, active-low), the two camsys LARB gates,
 * and its AXI bus protection must be released (infracfg 0x10001000, clr
 * 0x02D8, masks 0x5 and 0xA) or the first access to 0x1a000000 hangs the
 * interconnect.
 *
 * The domains are deliberately left powered on module unload by default:
 * the camera drivers have no genpd owner yet and unloading while a capture
 * is running would kill the subsystem.  Use power_off=1 to drop them.
 */

#include <linux/module.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/moduleparam.h>

#define SPM_PHYS_BASE		0x10006000UL
#define SPM_MAP_SIZE		0x1000UL

#define INFRACFG_PHYS_BASE	0x10001000UL
#define INFRACFG_MAP_SIZE	0x1000UL

#define TOPCKGEN_PHYS_BASE	0x10000000UL
#define TOPCKGEN_MAP_SIZE	0x1000UL

#define CAMSYS_MAIN_PHYS_BASE	0x1a000000UL
#define CAMSYS_RAWA_PHYS_BASE	0x1a04f000UL
#define CAMSYS_RAWB_PHYS_BASE	0x1a06f000UL
#define CAMSYS_MAP_SIZE		0x1000UL

/* SPM */
#define SPM_PWR_STATUS		0x016c
#define SPM_PWR_STATUS_2ND	0x0170

#define PWR_RST_B		BIT(0)
#define PWR_ISO			BIT(1)
#define PWR_ON			BIT(2)
#define PWR_ON_2ND		BIT(3)
#define PWR_CLK_DIS		BIT(4)
#define SRAM_PDN		BIT(8)
#define SRAM_ACK		BIT(12)

/*
 * topckgen cam_sel mux+gate.
 *
 * This must match the pmOS clock driver (clk-mt6789-topckgen.c):
 *
 *   MUX_GATE_CLR_SET_UPD(CLK_TOP_CAM_SEL, "cam_sel", cam_parents,
 *                        0x30, 0x34, 0x38, 16, 4, 23, 0x04, 10)
 *
 * i.e. mux bits 16..19, gate bit 23, update bit 10 of CLK_CFG_UPDATE (0x4).
 * The mux gate is active-low: enable writes BIT(23) to the CLR register and
 * disables writes it to SET.  At boot the gate bit is set (clock gated off),
 * which is why camsys registers read as zero until this runs.
 */
#define TOP_CAM_SEL		0x0030
#define TOP_CAM_SEL_SET		0x0034
#define TOP_CAM_SEL_CLR		0x0038
#define TOP_CLK_CFG_UPDATE	0x0004
#define TOP_CAM_MUX_SHIFT	16
#define TOP_CAM_GATE		BIT(23)
#define TOP_CAM_UPD_SHIFT	10

/*
 * camsys clock gate controller.
 *
 * Vendor struct mtk_gate_regs is { sta_ofs, clr_ofs, set_ofs }
 * (drivers/clk/mediatek/clk-gate.h).  clk-mt6789-cam.ko stores
 * 0x0/0x8/0x4, so status is 0x0, CLR is 0x8 and SET is 0x4.
 */
#define CAMSYS_CG_STA		0x0000
#define CAMSYS_CG_SET		0x0004
#define CAMSYS_CG_CLR		0x0008

#define CAMSYS_MAIN_LARB13	BIT(0)
#define CAMSYS_MAIN_LARB14	BIT(2)
#define CAMSYS_MAIN_CAM		BIT(6)
#define CAMSYS_MAIN_CAMTG	BIT(7)
#define CAMSYS_MAIN_SENINF	BIT(8)
#define CAMSYS_RA_LARBX		BIT(0)

/* infracfg CAM bus protection: clr 0x02D8, masks 0x5 / 0xA (ignore ack) */
#define IFR_MM_CAM_PROT_CLR	0x02d8
#define IFR_MM_CAM_PROT_MASK0	0x5
#define IFR_MM_CAM_PROT_MASK1	0xa

struct cam_domain {
	const char *name;
	u32 ctl_offs;
	u32 sta_mask;
	bool is_parent;
};

static const struct cam_domain cam_domains[] = {
	{ "disp",     0x354, BIT(21), true  },
	{ "cam",      0x35c, BIT(23), false },
	{ "cam_rawa", 0x360, BIT(24), false },
	{ "cam_rawb", 0x364, BIT(25), false },
};

#define CAM_DOM_DISP	0
#define CAM_DOM_CAM	1
#define CAM_DOM_RAWA	2
#define CAM_DOM_RAWB	3

static void __iomem *spm;
static void __iomem *infracfg;
static void __iomem *topckgen;
static void __iomem *camsys_main;
static void __iomem *camsys_rawa;
static void __iomem *camsys_rawb;

static bool do_power = true;
module_param(do_power, bool, 0644);
MODULE_PARM_DESC(do_power, "actually power the camera domains (default 1)");

static bool power_disp;
module_param(power_disp, bool, 0644);
MODULE_PARM_DESC(power_disp, "power the DISP parent too if it is off (default 0)");

static bool power_off;
module_param(power_off, bool, 0644);
MODULE_PARM_DESC(power_off, "power the camera domains back off on unload (default 0)");

static bool enable_clocks = true;
module_param(enable_clocks, bool, 0644);
MODULE_PARM_DESC(enable_clocks, "enable cam_sel/LARB gates (default 1)");

static bool clear_bus_prot = true;
module_param(clear_bus_prot, bool, 0644);
MODULE_PARM_DESC(clear_bus_prot, "release CAM AXI bus protection (default 1)");

static u32 spm_status(void)
{
	return readl(spm + SPM_PWR_STATUS);
}

static u32 spm_status2(void)
{
	return readl(spm + SPM_PWR_STATUS_2ND);
}

static bool dom_is_on(const struct cam_domain *d)
{
	return (spm_status() & d->sta_mask) &&
	       (spm_status2() & d->sta_mask);
}

static void dump_domains(const char *tag)
{
	int i;

	pr_info("%s: status=%08x/%08x\n", tag, spm_status(), spm_status2());
	for (i = 0; i < ARRAY_SIZE(cam_domains); i++) {
		const struct cam_domain *d = &cam_domains[i];

		pr_info("%s:   %-8s ctl=%08x sta=%s%s\n", tag, d->name,
			readl(spm + d->ctl_offs),
			(spm_status() & d->sta_mask) ? "1" : "0",
			(spm_status2() & d->sta_mask) ? "/1" : "/0");
	}
}

static void camsys_gate_enable(void __iomem *base, u32 bits)
{
	u32 before = readl(base + CAMSYS_CG_STA);

	/*
	 * Active-low gate: mtk_clk_gate_ops_setclr enables by writing the
	 * bit to CLR and disables by writing it to SET.
	 */
	writel(bits, base + CAMSYS_CG_CLR);
	pr_info("camsys gate %p: sta %08x -> %08x (enabled %08x, raw %08x/%08x/%08x)\n",
		base, before, readl(base + CAMSYS_CG_STA), bits,
		readl(base + CAMSYS_CG_STA), readl(base + CAMSYS_CG_SET),
		readl(base + CAMSYS_CG_CLR));
}

static int cam_top_cam_clock_on(void)
{
	u32 mux, gate;

	mux = readl(topckgen + TOP_CAM_SEL);
	gate = mux & TOP_CAM_GATE;

	pr_info("topckgen cam_sel=%08x (parent=%u gate=%u)\n", mux,
		(mux >> TOP_CAM_MUX_SHIFT) & 0xf, !!(gate));

	if (!gate)
		return 0;

	/*
	 * Active-low mux gate: write the gate bit to the CLR register, then
	 * pulse the update bit so the mux/gate takes effect.
	 */
	writel(TOP_CAM_GATE, topckgen + TOP_CAM_SEL_CLR);
	writel(BIT(TOP_CAM_UPD_SHIFT), topckgen + TOP_CLK_CFG_UPDATE);
	udelay(1);

	pr_info("topckgen cam_sel gate enabled: %08x -> %08x\n", mux,
		readl(topckgen + TOP_CAM_SEL));
	return 0;
}

static int cam_domain_power_on(const struct cam_domain *d)
{
	u32 val;
	int i;

	if (dom_is_on(d)) {
		pr_info("%s already on (ctl=%08x)\n", d->name,
			readl(spm + d->ctl_offs));
		return 0;
	}

	val = readl(spm + d->ctl_offs);
	pr_info("%s power-on: ctl pre=%08x\n", d->name, val);

	/* Same order as vendor mtk-scpsys: PWR_ON then PWR_ON_2ND. */
	writel(val | PWR_ON, spm + d->ctl_offs);
	writel(val | PWR_ON | PWR_ON_2ND, spm + d->ctl_offs);

	/* Wait for the domain status bit; a partial status is not an ack. */
	for (i = 0; i < 100; i++) {
		if (dom_is_on(d))
			break;
		usleep_range(1000, 2000);
	}
	if (i == 100) {
		pr_err("%s power-on ack timeout: ctl=%08x status=%08x/%08x\n",
		       d->name, readl(spm + d->ctl_offs), spm_status(),
		       spm_status2());
		return -ETIMEDOUT;
	}
	pr_info("%s PWR_ACK after %d us\n", d->name, (i + 1) * 1000);

	udelay(100);
	val = readl(spm + d->ctl_offs);
	val &= ~PWR_CLK_DIS;
	writel(val, spm + d->ctl_offs);
	val &= ~PWR_ISO;
	writel(val, spm + d->ctl_offs);
	val |= PWR_RST_B;
	writel(val, spm + d->ctl_offs);

	return 0;
}

static int cam_domain_sram_on(const struct cam_domain *d)
{
	u32 val;
	int i;

	val = readl(spm + d->ctl_offs);
	val &= ~SRAM_PDN;
	writel(val, spm + d->ctl_offs);

	/*
	 * Vendor poll timeout is one second; CAM SRAM can take longer than
	 * the old 50 ms budget on a cold power-up.
	 */
	for (i = 0; i < 200; i++) {
		if (!(readl(spm + d->ctl_offs) & SRAM_ACK))
			break;
		usleep_range(1000, 2000);
	}
	if (i == 200) {
		pr_err("%s SRAM ack timeout: ctl=%08x\n", d->name,
		       readl(spm + d->ctl_offs));
		return -ETIMEDOUT;
	}
	if (i)
		pr_info("%s SRAM ack after %d us\n", d->name, (i + 1) * 1000);

	return 0;
}

static void cam_bus_protect_clear(void)
{
	u32 sta_before = readl(infracfg + 0x02ec);

	/* BUS_PROT_IGN: the vendor table ignores the clear ack. */
	writel(IFR_MM_CAM_PROT_MASK0, infracfg + IFR_MM_CAM_PROT_CLR);
	writel(IFR_MM_CAM_PROT_MASK1, infracfg + IFR_MM_CAM_PROT_CLR);
	udelay(1);

	pr_info("infracfg CAM bus prot: sta %08x -> %08x\n", sta_before,
		readl(infracfg + 0x02ec));
}

static int __init mt6789_cam_pd_init(void)
{
	const struct cam_domain *disp = &cam_domains[CAM_DOM_DISP];
	const struct cam_domain *cam = &cam_domains[CAM_DOM_CAM];
	const struct cam_domain *rawa = &cam_domains[CAM_DOM_RAWA];
	const struct cam_domain *rawb = &cam_domains[CAM_DOM_RAWB];
	int ret;

	spm = ioremap(SPM_PHYS_BASE, SPM_MAP_SIZE);
	infracfg = ioremap(INFRACFG_PHYS_BASE, INFRACFG_MAP_SIZE);
	topckgen = ioremap(TOPCKGEN_PHYS_BASE, TOPCKGEN_MAP_SIZE);
	camsys_main = ioremap(CAMSYS_MAIN_PHYS_BASE, CAMSYS_MAP_SIZE);
	camsys_rawa = ioremap(CAMSYS_RAWA_PHYS_BASE, CAMSYS_MAP_SIZE);
	camsys_rawb = ioremap(CAMSYS_RAWB_PHYS_BASE, CAMSYS_MAP_SIZE);

	if (!spm || !infracfg || !topckgen || !camsys_main ||
	    !camsys_rawa || !camsys_rawb) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	dump_domains("pre");

	if (!do_power)
		return 0;

	if (!dom_is_on(disp)) {
		pr_warn("DISP parent is off (display should be alive, but it is not)\n");
		if (power_disp) {
			ret = cam_domain_power_on(disp);
			if (ret)
				goto err_unmap;
			ret = cam_domain_sram_on(disp);
			if (ret)
				goto err_unmap;
		} else {
			pr_warn("continuing anyway; set power_disp=1 to force it\n");
		}
	} else {
		pr_info("DISP parent is on\n");
	}

	if (enable_clocks)
		cam_top_cam_clock_on();

	ret = cam_domain_power_on(cam);
	if (ret)
		goto err_unmap;

	/* Vendor scpsys enables the subsys clocks ("cam-0"/"cam-1", i.e. the
	 * two camsys LARB gates) after RST_B and before SRAM power-up.
	 */
	if (enable_clocks)
		camsys_gate_enable(camsys_main,
				CAMSYS_MAIN_LARB13 | CAMSYS_MAIN_LARB14);

	ret = cam_domain_sram_on(cam);
	if (ret)
		goto err_unmap;

	if (clear_bus_prot)
		cam_bus_protect_clear();

	/* Engine gates on top of LARB13/14; the real SENINF/CAMSV drivers
	 * will own these through CCF, but nothing else does today.
	 */
	if (enable_clocks)
		camsys_gate_enable(camsys_main,
				CAMSYS_MAIN_CAM | CAMSYS_MAIN_CAMTG |
				CAMSYS_MAIN_SENINF);

	ret = cam_domain_power_on(rawa);
	if (ret)
		goto err_unmap;
	if (enable_clocks)
		camsys_gate_enable(camsys_rawa, CAMSYS_RA_LARBX);
	ret = cam_domain_sram_on(rawa);
	if (ret)
		goto err_unmap;

	ret = cam_domain_power_on(rawb);
	if (ret)
		goto err_unmap;
	if (enable_clocks)
		camsys_gate_enable(camsys_rawb, CAMSYS_RA_LARBX);
	ret = cam_domain_sram_on(rawb);
	if (ret)
		goto err_unmap;

	dump_domains("post");
	pr_info("camera power domains are up\n");

	return 0;

err_unmap:
	if (spm)
		iounmap(spm);
	if (infracfg)
		iounmap(infracfg);
	if (topckgen)
		iounmap(topckgen);
	if (camsys_main)
		iounmap(camsys_main);
	if (camsys_rawa)
		iounmap(camsys_rawa);
	if (camsys_rawb)
		iounmap(camsys_rawb);
	spm = infracfg = topckgen = NULL;
	camsys_main = camsys_rawa = camsys_rawb = NULL;
	return ret;
}

static void __exit mt6789_cam_pd_exit(void)
{
	if (power_off && spm) {
		const struct cam_domain *order[] = {
			&cam_domains[CAM_DOM_RAWB],
			&cam_domains[CAM_DOM_RAWA],
			&cam_domains[CAM_DOM_CAM],
		};
		int i;

		for (i = 0; i < ARRAY_SIZE(order); i++) {
			const struct cam_domain *d = order[i];
			u32 val;

			if (!dom_is_on(d))
				continue;

			if (d->ctl_offs == 0x35c && clear_bus_prot) {
				/* Re-arm CAM bus protect before dropping it. */
				writel(IFR_MM_CAM_PROT_MASK0,
				       infracfg + 0x02d4);
				writel(IFR_MM_CAM_PROT_MASK1,
				       infracfg + 0x02d4);
			}

			val = readl(spm + d->ctl_offs);
			val |= SRAM_PDN;
			writel(val, spm + d->ctl_offs);
			val |= PWR_ISO;
			writel(val, spm + d->ctl_offs);
			val &= ~PWR_RST_B;
			writel(val, spm + d->ctl_offs);
			val |= PWR_CLK_DIS;
			writel(val, spm + d->ctl_offs);
			val &= ~PWR_ON;
			writel(val, spm + d->ctl_offs);
			val &= ~PWR_ON_2ND;
			writel(val, spm + d->ctl_offs);
			usleep_range(1000, 2000);
			pr_info("%s powered off: ctl=%08x status=%08x/%08x\n",
				d->name, readl(spm + d->ctl_offs),
				spm_status(), spm_status2());
		}
	}

	if (spm)
		iounmap(spm);
	if (infracfg)
		iounmap(infracfg);
	if (topckgen)
		iounmap(topckgen);
	if (camsys_main)
		iounmap(camsys_main);
	if (camsys_rawa)
		iounmap(camsys_rawa);
	if (camsys_rawb)
		iounmap(camsys_rawb);
}

module_init(mt6789_cam_pd_init);
module_exit(mt6789_cam_pd_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Power MT6789 camera SPM domains for TB351FU pmOS");
MODULE_AUTHOR("PMOS TB351FU porting");
