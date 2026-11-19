// SPDX-License-Identifier: GPL-2.0
/*
 * mt6789_i2s3_mclk - enable the vendor MT6789 I2S3 MCLK tree (TB351FU pmOS)
 *
 * Mainline mt8186-afe-clk maps DAI_I2S3 to the I2S4 MCK clocks
 * (top_i2s4_m_sel + top_apll12_div4).  The MT6789 hardware I2S3 DAI,
 * however, expects its own tree (vendor mt6789-afe-clk.c):
 *
 *     top_mux_aud_2 (aud_2_sel) -> parent apll2_ck (196.608 MHz)
 *     top_i2s3_m_sel -> aud_2_sel
 *     top_apll12_div3 -> /32 = 6.144 MHz, gate enabled
 *
 * Mainline leaves aud_2_sel on tck_26m_mx9_ck, so selecting APLL2 for that
 * mux is part of this module as well.  Without it the divider yields 5.2 MHz.
 *
 * Without this the AFE registers look alive, DAPM says I2S3 On, but the
 * I2S3 MCLK pad never toggles, so BCLK/LRCK never reach the AW88395 amps
 * and they report NOCLKS (0x20).  The PCM also stalls, which shows up as
 * an endlessly buffering YouTube/Firefox stream.
 *
 * This module uses the same CCF sequence as the vendor driver:
 *   clk_set_parent(top_i2s3_m_sel, aud_2_sel);
 *   clk_prepare_enable(top_i2s3_m_sel);
 *   clk_prepare_enable(top_apll12_div3);
 *   clk_set_rate(top_apll12_div3, 6144000);
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>

#define AFE_DEV_NAME	"11210000.mt6789-afe-pcm"
#define I2S3_RATE	6144000UL

static struct device *afe_dev;
static struct clk *mux3, *div3, *aud2, *apll2;

static int __init mt6789_i2s3_mclk_init(void)
{
	int ret;

	afe_dev = bus_find_device_by_name(&platform_bus_type, NULL,
					  AFE_DEV_NAME);
	if (!afe_dev) {
		pr_err("mt6789_i2s3_mclk: AFE device %s not found\n",
		       AFE_DEV_NAME);
		return -ENODEV;
	}

	mux3 = clk_get(afe_dev, "top_i2s3_m_sel");
	if (IS_ERR(mux3)) {
		pr_err("mt6789_i2s3_mclk: clk_get top_i2s3_m_sel failed: %ld\n",
		       PTR_ERR(mux3));
		mux3 = NULL;
		return PTR_ERR(mux3);
	}

	div3 = clk_get(afe_dev, "top_apll12_div3");
	if (IS_ERR(div3)) {
		pr_err("mt6789_i2s3_mclk: clk_get top_apll12_div3 failed: %ld\n",
		       PTR_ERR(div3));
		div3 = NULL;
		return PTR_ERR(div3);
	}

	aud2 = clk_get(afe_dev, "top_mux_aud_2");
	if (IS_ERR(aud2)) {
		pr_warn("mt6789_i2s3_mclk: clk_get top_mux_aud_2 failed: %ld (leaving parent unchanged)\n",
			PTR_ERR(aud2));
		aud2 = NULL;
	} else {
		/*
		 * aud_2_sel defaults to tck_26m_mx9_ck on mainline.  The
		 * vendor audio init selects APLL2 (196.608 MHz) so that
		 * apll12_div3 / 32 = 6.144 MHz.  Without this the mux stays
		 * at 26 MHz and the divider produces 5.2 MHz.
		 */
		apll2 = clk_get(afe_dev, "top_apll2_ck");
		if (IS_ERR(apll2)) {
			pr_warn("mt6789_i2s3_mclk: clk_get top_apll2_ck failed: %ld (leaving aud_2_sel parent unchanged)\n",
				PTR_ERR(apll2));
			apll2 = NULL;
		} else {
			ret = clk_set_parent(aud2, apll2);
			if (ret)
				pr_warn("mt6789_i2s3_mclk: clk_set_parent(top_mux_aud_2, apll2) failed: %d\n",
					ret);
		}

		ret = clk_set_parent(mux3, aud2);
		if (ret)
			pr_warn("mt6789_i2s3_mclk: clk_set_parent failed: %d\n",
				ret);
	}

	ret = clk_prepare_enable(mux3);
	if (ret) {
		pr_err("mt6789_i2s3_mclk: enable top_i2s3_m_sel failed: %d\n",
		       ret);
		return ret;
	}

	ret = clk_set_rate(div3, I2S3_RATE);
	if (ret)
		pr_warn("mt6789_i2s3_mclk: clk_set_rate div3 failed: %d\n", ret);

	ret = clk_prepare_enable(div3);
	if (ret) {
		pr_err("mt6789_i2s3_mclk: enable top_apll12_div3 failed: %d\n",
		       ret);
		clk_disable_unprepare(mux3);
		return ret;
	}

	pr_info("mt6789_i2s3_mclk: mux3=%lu div3=%lu (want %lu)\n",
		clk_get_rate(mux3), clk_get_rate(div3), I2S3_RATE);

	return 0;
}

static void __exit mt6789_i2s3_mclk_exit(void)
{
	/*
	 * Leave the tree enabled: the AFE has no genpd/clock owner for it
	 * and unloading while a stream is active would kill playback.
	 */
	if (div3)
		clk_put(div3);
	if (mux3)
		clk_put(mux3);
	if (aud2)
		clk_put(aud2);
	if (apll2)
		clk_put(apll2);
	if (afe_dev)
		put_device(afe_dev);
}

module_init(mt6789_i2s3_mclk_init);
module_exit(mt6789_i2s3_mclk_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Enable the MT6789 I2S3 MCLK tree for TB351FU pmOS");
MODULE_AUTHOR("PMOS TB351FU porting");
