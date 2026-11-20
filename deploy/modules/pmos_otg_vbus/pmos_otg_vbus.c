// SPDX-License-Identifier: GPL-2.0
/*
 * pmos_otg_vbus - expose the SC8989x OTG boost to userspace.
 *
 * The vendor charger stack exports get_charger_by_name() and
 * charger_dev_enable_otg() from charger_class.ko.  Those exports are plain
 * EXPORT_SYMBOL, so symbol_get() would reject them (it only accepts
 * EXPORT_SYMBOL_GPL).  Use ordinary external references instead: the module
 * loader resolves them against the already-loaded charger_class.ko and keeps
 * that module pinned while this one is loaded.
 *
 * Userspace (pmos-usb-role) writes 1/0 to
 * /sys/kernel/pmos_otg_vbus/enable around the MUSB role switch.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>

/* Provided by charger_class.ko (vendor module, plain EXPORT_SYMBOL). */
extern void *get_charger_by_name(const char *name);
extern int charger_dev_enable_otg(void *chg, bool en);

static void *primary_chg;
static struct kobject *pmos_otg_kobj;
static DEFINE_MUTEX(pmos_otg_lock);
static bool otg_enabled;

static ssize_t enable_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return sysfs_emit(buf, "%d\n", otg_enabled ? 1 : 0);
}

static ssize_t enable_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	if (!primary_chg)
		return -ENODEV;

	mutex_lock(&pmos_otg_lock);
	ret = charger_dev_enable_otg(primary_chg, enable);
	if (!ret)
		otg_enabled = enable;
	mutex_unlock(&pmos_otg_lock);

	if (ret)
		return ret;

	pr_info("pmos_otg_vbus: OTG boost %s\n", enable ? "enabled" : "disabled");
	return count;
}

static struct kobj_attribute enable_attr = __ATTR_RW(enable);

static int __init pmos_otg_vbus_init(void)
{
	int ret;

	primary_chg = get_charger_by_name("primary_chg");
	if (!primary_chg) {
		pr_err("pmos_otg_vbus: primary_chg is not registered\n");
		return -ENODEV;
	}

	pmos_otg_kobj = kobject_create_and_add("pmos_otg_vbus", kernel_kobj);
	if (!pmos_otg_kobj)
		return -ENOMEM;

	ret = sysfs_create_file(pmos_otg_kobj, &enable_attr.attr);
	if (ret) {
		kobject_put(pmos_otg_kobj);
		pmos_otg_kobj = NULL;
		return ret;
	}

	pr_info("pmos_otg_vbus: ready (charger=primary_chg)\n");
	return 0;
}

static void __exit pmos_otg_vbus_exit(void)
{
	if (otg_enabled && primary_chg)
		charger_dev_enable_otg(primary_chg, false);

	if (pmos_otg_kobj) {
		sysfs_remove_file(pmos_otg_kobj, &enable_attr.attr);
		kobject_put(pmos_otg_kobj);
	}
}

module_init(pmos_otg_vbus_init);
module_exit(pmos_otg_vbus_exit);

MODULE_DESCRIPTION("SC8989x OTG VBUS control for pmOS");
MODULE_AUTHOR("pmOS TB351FU bring-up");
MODULE_LICENSE("GPL");
