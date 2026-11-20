// pmos_charge_cap: user-selectable battery charge cap through the vendor
// charger_class API.  "limit" is a percentage (e.g. 80) or "full"/"100".
// Charging is disabled when capacity reaches the limit and re-enabled with
// 5% hysteresis (stock Android: stop at 80, resume at 75).  Exposes
// capacity / charging / input_current in sysfs.
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

struct charger_device;

extern struct charger_device *get_charger_by_name(const char *name);
extern int charger_dev_enable(struct charger_device *chg_dev, bool en);
extern int charger_dev_is_enabled(struct charger_device *chg_dev, bool *en);
extern int charger_dev_set_input_current(struct charger_device *chg_dev, int ua);

#define CAP_HYSTERESIS	5
#define CAP_POLL_MS	5000
#define CAP_SETTLE_MS	3000
#define CAP_FULL	100

static struct charger_device *chg;
static struct power_supply *battery;
static struct kobject *cap_kobj;
static struct delayed_work cap_work;
static DEFINE_MUTEX(cap_lock);

static int cap_limit = CAP_FULL;
static int cap_input_ua = 900000;
static int cap_capacity = -1;
static bool cap_charging;

module_param_named(limit, cap_limit, int, 0444);
MODULE_PARM_DESC(limit, "Initial charge limit in percent (100 = full)");
module_param_named(input_current_ua, cap_input_ua, int, 0444);
MODULE_PARM_DESC(input_current_ua, "Input current limit in uA");

static int read_capacity(void)
{
	union power_supply_propval val;
	int ret;

	if (!battery)
		return -ENODEV;

	ret = power_supply_get_property(battery, POWER_SUPPLY_PROP_CAPACITY, &val);
	if (ret)
		return ret;

	return val.intval;
}

static void __cap_apply(void)
{
	int ret;

	if (cap_limit <= 0 || cap_limit >= CAP_FULL) {
		if (!cap_charging) {
			ret = charger_dev_enable(chg, true);
			if (!ret)
				cap_charging = true;
			pr_info("pmos_charge_cap: full -> enable ret=%d\n", ret);
		}
		return;
	}

	ret = read_capacity();
	if (ret >= 0)
		cap_capacity = ret;

	if (cap_capacity < 0)
		return;

	if (cap_charging && cap_capacity >= cap_limit) {
		ret = charger_dev_enable(chg, false);
		if (!ret)
			cap_charging = false;
		pr_info("pmos_charge_cap: %d%% >= %d%% -> disable ret=%d\n",
			cap_capacity, cap_limit, ret);
	} else if (!cap_charging && cap_capacity <= cap_limit - CAP_HYSTERESIS) {
		ret = charger_dev_enable(chg, true);
		if (!ret)
			cap_charging = true;
		pr_info("pmos_charge_cap: %d%% <= %d%% -> enable ret=%d\n",
			cap_capacity, cap_limit - CAP_HYSTERESIS, ret);
	}
}

static void cap_work_fn(struct work_struct *work)
{
	mutex_lock(&cap_lock);
	__cap_apply();
	mutex_unlock(&cap_lock);
	schedule_delayed_work(&cap_work, msecs_to_jiffies(CAP_POLL_MS));
}

static ssize_t limit_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	if (cap_limit >= CAP_FULL)
		return sysfs_emit(buf, "full\n");
	return sysfs_emit(buf, "%d\n", cap_limit);
}

static ssize_t limit_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	int val;

	if (sysfs_streq(buf, "full"))
		val = CAP_FULL;
	else if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	if (val < 50 || val > CAP_FULL)
		return -EINVAL;

	mutex_lock(&cap_lock);
	cap_limit = val;
	__cap_apply();
	mutex_unlock(&cap_lock);
	return count;
}

static ssize_t capacity_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	int ret;

	mutex_lock(&cap_lock);
	ret = read_capacity();
	if (ret >= 0)
		cap_capacity = ret;
	mutex_unlock(&cap_lock);
	return sysfs_emit(buf, "%d\n", cap_capacity);
}

static ssize_t charging_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	bool en = false;

	mutex_lock(&cap_lock);
	charger_dev_is_enabled(chg, &en);
	cap_charging = en;
	mutex_unlock(&cap_lock);
	return sysfs_emit(buf, "%d\n", en);
}

static ssize_t input_current_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", cap_input_ua);
}

static ssize_t input_current_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int val, ret;

	if (kstrtoint(buf, 10, &val) || val < 100000 || val > 3000000)
		return -EINVAL;

	ret = charger_dev_set_input_current(chg, val);
	if (ret)
		return ret;

	cap_input_ua = val;
	return count;
}

static struct kobj_attribute cap_limit_attr =
	__ATTR(limit, 0644, limit_show, limit_store);
static struct kobj_attribute cap_capacity_attr =
	__ATTR(capacity, 0444, capacity_show, NULL);
static struct kobj_attribute cap_charging_attr =
	__ATTR(charging, 0444, charging_show, NULL);
static struct kobj_attribute cap_input_attr =
	__ATTR(input_current, 0644, input_current_show, input_current_store);

static int __init pmos_charge_cap_init(void)
{
	int ret;

	chg = get_charger_by_name("primary_chg");
	if (!chg) {
		pr_err("pmos_charge_cap: primary_chg not found\n");
		return -ENODEV;
	}

	battery = power_supply_get_by_name("battery");
	if (!battery) {
		pr_err("pmos_charge_cap: battery supply not found\n");
		return -ENODEV;
	}

	if (cap_limit < 50 || cap_limit > CAP_FULL)
		cap_limit = CAP_FULL;

	if (cap_input_ua >= 100000)
		charger_dev_set_input_current(chg, cap_input_ua);

	/*
	 * Bring the charger/power path up once even if the cap is already
	 * reached at boot, so the tablet can run from VBUS.  The first
	 * monitor pass (after CAP_SETTLE_MS) re-applies the cap.
	 */
	ret = charger_dev_enable(chg, true);
	if (ret)
		pr_warn("pmos_charge_cap: initial enable failed: %d\n", ret);
	cap_charging = !ret;

	cap_kobj = kobject_create_and_add("pmos_charge_cap", kernel_kobj);
	if (!cap_kobj)
		return -ENOMEM;

	ret = sysfs_create_file(cap_kobj, &cap_limit_attr.attr);
	ret |= sysfs_create_file(cap_kobj, &cap_capacity_attr.attr);
	ret |= sysfs_create_file(cap_kobj, &cap_charging_attr.attr);
	ret |= sysfs_create_file(cap_kobj, &cap_input_attr.attr);
	if (ret) {
		pr_err("pmos_charge_cap: sysfs create failed\n");
		kobject_put(cap_kobj);
		return ret;
	}

	INIT_DELAYED_WORK(&cap_work, cap_work_fn);
	schedule_delayed_work(&cap_work, msecs_to_jiffies(CAP_SETTLE_MS));

	pr_info("pmos_charge_cap: loaded limit=%d input_ua=%d capacity=%d charging=%d\n",
		cap_limit, cap_input_ua, cap_capacity, cap_charging);
	return 0;
}

static void __exit pmos_charge_cap_exit(void)
{
	cancel_delayed_work_sync(&cap_work);
	sysfs_remove_file(cap_kobj, &cap_limit_attr.attr);
	sysfs_remove_file(cap_kobj, &cap_capacity_attr.attr);
	sysfs_remove_file(cap_kobj, &cap_charging_attr.attr);
	sysfs_remove_file(cap_kobj, &cap_input_attr.attr);
	kobject_put(cap_kobj);
	charger_dev_enable(chg, true);
	power_supply_put(battery);
	pr_info("pmos_charge_cap: unloaded, charging re-enabled\n");
}

module_init(pmos_charge_cap_init);
module_exit(pmos_charge_cap_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("pmOS user-selectable battery charge cap (vendor charger_class)");
