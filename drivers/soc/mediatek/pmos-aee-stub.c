// SPDX-License-Identifier: GPL-2.0
/*
 * PMOS-TB351FU: stubs and exported symbols for MediaTek MT6789
 * vendor modules (pinctrl-mt6789, clk-common, clk-mt6789, mtk-pmic-wrap,
 * mt6397, mt6358-regulator, and connectivity stack connadp/wmt_drv/wlan).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/clk-provider.h>
#include <linux/notifier.h>
#include <linux/sched.h>
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

void aed_combo_exception_api(void *p, int l, const char *m)
{
}
EXPORT_SYMBOL(aed_combo_exception_api);

/* Connectivity battery notify */
int register_low_battery_notify(int (*func)(int), int prio, void *data)
{
	return 0;
}
EXPORT_SYMBOL(register_low_battery_notify);

/* MTK Display notifier for WMT blank/unblank events */
static BLOCKING_NOTIFIER_HEAD(mtk_disp_notifier_list);

int mtk_disp_notifier_register(const char *name, struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&mtk_disp_notifier_list, nb);
}
EXPORT_SYMBOL(mtk_disp_notifier_register);

int mtk_disp_notifier_unregister(const char *name, struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&mtk_disp_notifier_list, nb);
}
EXPORT_SYMBOL(mtk_disp_notifier_unregister);

/* Connectivity thread dump & MMC debug exports */
/* Platform bridge exports for WMT SDIO */
/* WiFi & DVFSRC / Battery / EMI-MPU stubs */
int dvfsrc_get_required_opp_peak_bw(void *dev, int opp)
{
	return 0;
}
EXPORT_SYMBOL(dvfsrc_get_required_opp_peak_bw);

int lbat_user_register(void *cb, int v1, int v2, int v3, void *data)
{
	return 0;
}
EXPORT_SYMBOL(lbat_user_register);

int mtk_emimpu_init_region(void *region, int id)
{
	return 0;
}
EXPORT_SYMBOL(mtk_emimpu_init_region);

int mtk_emimpu_set_addr(void *region, unsigned long start, unsigned long end)
{
	return 0;
}
EXPORT_SYMBOL(mtk_emimpu_set_addr);

int mtk_emimpu_set_apc(void *region, int d1, int d2)
{
	return 0;
}
EXPORT_SYMBOL(mtk_emimpu_set_apc);

int mtk_emimpu_lock_region(void *region, int lock)
{
	return 0;
}
EXPORT_SYMBOL(mtk_emimpu_lock_region);

int mtk_emimpu_set_protection(void *region)
{
	return 0;
}
EXPORT_SYMBOL(mtk_emimpu_set_protection);

int mtk_emimpu_free_region(void *region)
{
	return 0;
}
EXPORT_SYMBOL(mtk_emimpu_free_region);



/* SCP & SensorHub stubs */
void aed_common_exception_api(void *p, int l, const char *m)
{
}
EXPORT_SYMBOL(aed_common_exception_api);

int aee_get_mode(void)
{
	return 0;
}
EXPORT_SYMBOL(aee_get_mode);

void gpio_dump_regs_range(int s, int e)
{
}
EXPORT_SYMBOL(gpio_dump_regs_range);

unsigned int mt_get_fmeter_freq(int id)
{
	return 26000000;
}
EXPORT_SYMBOL(mt_get_fmeter_freq);

int register_3way_semaphore_notifier(void *nb)
{
	return 0;
}
EXPORT_SYMBOL(register_3way_semaphore_notifier);

int unregister_3way_semaphore_notifier(void *nb)
{
	return 0;
}
EXPORT_SYMBOL(unregister_3way_semaphore_notifier);

/* Alias mtk_mux_clr_set_ops to mtk_mux_clr_set_upd_ops */
const struct clk_ops mtk_mux_clr_set_ops = {
	.get_parent = NULL,
	.set_parent = NULL,
};
EXPORT_SYMBOL(mtk_mux_clr_set_ops);

MODULE_LICENSE("GPL");
