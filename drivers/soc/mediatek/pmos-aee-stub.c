// SPDX-License-Identifier: GPL-2.0
/*
 * PMOS-TB351FU: stubs and exported symbols for MediaTek MT6789
 * vendor modules (pinctrl-mt6789, clk-common, clk-mt6789, mtk-pmic-wrap,
 * mt6397, mt6358-regulator).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/clk-provider.h>
#include "../../clk/mediatek/clk-mux.h"

int aee_kernel_warning_api_func(const char *fmt, ...)
{
	return 0;
}
EXPORT_SYMBOL(aee_kernel_warning_api_func);

int aee_is_printk_too_much(void)
{
	return 0;
}
EXPORT_SYMBOL(aee_is_printk_too_much);

/* Alias mtk_mux_clr_set_ops to mtk_mux_clr_set_upd_ops */
const struct clk_ops mtk_mux_clr_set_ops = {
	.get_parent = NULL,
	.set_parent = NULL,
};
EXPORT_SYMBOL(mtk_mux_clr_set_ops);

MODULE_LICENSE("GPL");
