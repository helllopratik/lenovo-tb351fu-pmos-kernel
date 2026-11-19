// SPDX-License-Identifier: GPL-2.0
/*
 * mt6789_audio_pd - power the MT6789 "audio" SPM power domain (TB351FU pmOS)
 *
 * Why this is needed
 * ------------------
 * The pmOS kernel has no mt6789-scpsys power-domain driver.  At early boot
 * arch/arm64/kernel/setup.c:pmos_afe_dt_fixup() removes the AFE node's
 * power-domains property because android,mt6789_pd_stub only registers eight
 * fake domains (0..7) while the AFE asks for domain 0xB ("audio").  As a
 * result nothing ever powers the SPM audio domain: the whole AFE register
 * block reads 0, the I2S pins stay dead and the AW88395 amps report NOCLKS.
 *
 * Values decoded from stock mtk-scpsys-mt6789.ko (scp_domain_data_mt6789,
 * entry 11 "audio") and mtk-scpsys.ko:
 *   ctl_offs  = 0x358   (SPM_AUDIO_PWR_CON, SPM base 0x10006000)
 *   sta_mask  = BIT(22)
 *   pwr status= 0x16c / 0x170
 *   sram pdn  = BIT(8), sram ack = BIT(12)
 *
 * The SPM is always-on, so this can be done from a standalone module without
 * touching the kernel Image or the DTB.  The domain is deliberately left
 * powered on module unload: the AFE has no genpd owner, and unloading while
 * audio is streaming would kill it.
 */

#include <linux/module.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/bitops.h>

#define SPM_PHYS_BASE		0x10006000UL
#define SPM_MAP_SIZE		0x1000UL

#define SPM_AUDIO_PWR_CON	0x0358
#define SPM_PWR_STATUS		0x016c
#define SPM_PWR_STATUS_2ND	0x0170

#define AUDIO_STA_MASK		BIT(22)
#define PWR_RST_B		BIT(0)
#define PWR_ISO			BIT(1)
#define PWR_ON			BIT(2)
#define PWR_ON_2ND		BIT(3)
#define PWR_CLK_DIS		BIT(4)
#define SRAM_PDN		BIT(8)
#define SRAM_ACK		BIT(12)

static void __iomem *spm;

static bool audio_pd_is_on(void)
{
	u32 st1 = readl(spm + SPM_PWR_STATUS) & AUDIO_STA_MASK;
	u32 st2 = readl(spm + SPM_PWR_STATUS_2ND) & AUDIO_STA_MASK;

	return st1 && st2;
}

static int __init mt6789_audio_pd_init(void)
{
	u32 v, w, ctl;
	int i;

	spm = ioremap(SPM_PHYS_BASE, SPM_MAP_SIZE);
	if (!spm)
		return -ENOMEM;

	v = readl(spm + SPM_AUDIO_PWR_CON);
	pr_info("mt6789_audio_pd: ctl=%08x status=%08x/%08x\n", v,
		readl(spm + SPM_PWR_STATUS),
		readl(spm + SPM_PWR_STATUS_2ND));

	if (audio_pd_is_on()) {
		pr_info("mt6789_audio_pd: audio domain already on\n");
		return 0;
	}

	/* 1. start power-up; the status bits do NOT assert until the whole
	 *    sequence below has run (stock scpsys behaves the same way), so
	 *    do not abort on an intermediate poll timeout.
	 */
	writel(v | PWR_ON, spm + SPM_AUDIO_PWR_CON);
	udelay(2000);
	/* 2. second power-on bit, release clock gate and isolation */
	w = v | PWR_ON | PWR_ON_2ND;
	writel(w, spm + SPM_AUDIO_PWR_CON);
	writel(w & ~PWR_CLK_DIS, spm + SPM_AUDIO_PWR_CON);
	writel(w & ~(PWR_ISO | PWR_CLK_DIS), spm + SPM_AUDIO_PWR_CON);
	udelay(5000);

	/* 3. release reset, then power the SRAMs (clear PDN, ack may still
	 *    be set until the AFE functional clocks are enabled by the ASoC
	 *    driver at first playback - do not treat that as fatal)
	 */
	w = (w & ~(PWR_ISO | PWR_CLK_DIS)) | PWR_RST_B;
	writel(w, spm + SPM_AUDIO_PWR_CON);
	writel(w & ~SRAM_PDN, spm + SPM_AUDIO_PWR_CON);

	for (i = 0; i < 50; i++) {
		if (!(readl(spm + SPM_AUDIO_PWR_CON) & SRAM_ACK))
			break;
		usleep_range(1000, 2000);
	}

	/* 4. now wait for both status registers to report the domain on */
	for (i = 0; i < 200; i++) {
		if (audio_pd_is_on())
			break;
		usleep_range(1000, 2000);
	}
	if (i == 200) {
		pr_err("mt6789_audio_pd: power-on ack timeout, ctl=%08x status=%08x/%08x\n",
		       readl(spm + SPM_AUDIO_PWR_CON),
		       readl(spm + SPM_PWR_STATUS),
		       readl(spm + SPM_PWR_STATUS_2ND));
		iounmap(spm);
		spm = NULL;
		return -ETIMEDOUT;
	}

	ctl = readl(spm + SPM_AUDIO_PWR_CON);
	pr_info("mt6789_audio_pd: powered on ctl=%08x status=%08x/%08x sram_ack=%d\n",
		ctl, readl(spm + SPM_PWR_STATUS), readl(spm + SPM_PWR_STATUS_2ND),
		!!(ctl & SRAM_ACK));

	return 0;
}

static void __exit mt6789_audio_pd_exit(void)
{
	/* Keep the domain powered; only drop our mapping. */
	if (spm)
		iounmap(spm);
}

module_init(mt6789_audio_pd_init);
module_exit(mt6789_audio_pd_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Power the MT6789 audio SPM power domain for TB351FU pmOS");
MODULE_AUTHOR("PMOS TB351FU porting");
