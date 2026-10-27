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
void connectivity_export_dump_thread_state(void)
{
}
EXPORT_SYMBOL(connectivity_export_dump_thread_state);

int connectivity_export_mmc_io_rw_direct(void *c, int w, unsigned int f, unsigned int a, u8 b, u8 *o)
{
	return 0;
}
EXPORT_SYMBOL(connectivity_export_mmc_io_rw_direct);

void connectivity_export_show_stack(struct task_struct *t, unsigned long *s, const char *l)
{
}
EXPORT_SYMBOL(connectivity_export_show_stack);

/* Platform bridge exports for WMT SDIO */
int wmt_export_platform_bridge_register(void *ops)
{
	return 0;
}
EXPORT_SYMBOL(wmt_export_platform_bridge_register);

int wmt_export_platform_bridge_unregister(void)
{
	return 0;
}
EXPORT_SYMBOL(wmt_export_platform_bridge_unregister);

int wmt_export_mtk_wcn_cmb_sdio_disable_eirq(void)
{
	return 0;
}
EXPORT_SYMBOL(wmt_export_mtk_wcn_cmb_sdio_disable_eirq);

int wmt_export_mtk_wcn_sdio_irq_flag_set(int flag)
{
	return 0;
}
EXPORT_SYMBOL(wmt_export_mtk_wcn_sdio_irq_flag_set);

int wifi_irq = -1;
EXPORT_SYMBOL(wifi_irq);

void *mtk_wcn_cmb_sdio_pm_cb = NULL;
EXPORT_SYMBOL(mtk_wcn_cmb_sdio_pm_cb);

void *mtk_wcn_cmb_sdio_pm_data = NULL;
EXPORT_SYMBOL(mtk_wcn_cmb_sdio_pm_data);

void mtk_wcn_set_connsys_power_off_flag(int flag)
{
}
EXPORT_SYMBOL(mtk_wcn_set_connsys_power_off_flag);

/* Alias mtk_mux_clr_set_ops to mtk_mux_clr_set_upd_ops */
const struct clk_ops mtk_mux_clr_set_ops = {
	.get_parent = NULL,
	.set_parent = NULL,
};
EXPORT_SYMBOL(mtk_mux_clr_set_ops);

MODULE_LICENSE("GPL");
