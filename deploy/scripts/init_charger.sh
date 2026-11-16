#!/bin/sh
# Vendor charger stack + user-selectable charge cap for the Lenovo TB351FU.
#
# The tablet charges through SouthChip SC8989x chargers driven by the vendor
# charger_class stack.  The in-tree mt6375 charger cannot bind.  This script
# loads the vendor stack, points the SC8989x VBUS channel at the fixed MT6358
# VCDT auxadc (kernel-side), and starts pmos_charge_cap.ko.
#
# Limit selection lives in /etc/pmos-charge-cap.conf and is changed with
# `pmos-charge-cap full|80`.  Runs from pmos-charger.service on every boot.

KMSG=/dev/kmsg
MODDIR=/lib/modules/extra/charger
CONF=/etc/pmos-charge-cap.conf
STATUS=/run/pmos_charger_status

kp() { echo "PMOSCHG: $*"; echo "PMOSCHG: $*" > "$KMSG" 2>/dev/null; }

# pmos_otg_vbus returns -ENODEV until sc8989x has registered "primary_chg".
# insmod is synchronous, but probe ordering on the vendor stack is not fully
# deterministic, so retry briefly instead of losing OTG for the whole boot.
load_otg_vbus() {
	[ -f "$MODDIR/pmos_otg_vbus.ko" ] || return 0
	grep -q "^pmos_otg_vbus " /proc/modules 2>/dev/null && return 0
	i=0
	while [ "$i" -lt 40 ]; do
		if insmod "$MODDIR/pmos_otg_vbus.ko"; then
			kp "loaded pmos_otg_vbus (attempt $((i + 1)))"
			return 0
		fi
		grep -q "^pmos_otg_vbus " /proc/modules 2>/dev/null && return 0
		i=$((i + 1))
		sleep 0.25
	done
	kp "WARN insmod pmos_otg_vbus failed after $i attempts"
	return 1
}

exec > /tmp/pmos_charger_init.log 2>&1

kp "=== charger stack starting ==="
echo NOK > "$STATUS"

LIMIT=full
INPUT_UA=900000
# shellcheck disable=SC1090
[ -f "$CONF" ] && . "$CONF"

# sc8989x depends on symbols from these vendor modules; there is no
# modules.dep entry for the Android .ko files, so load them in order.
for m in charger_class mtk_charger_algorithm_class adapter_class dev-info mt6375 pmos_chg_stub; do
	if [ ! -f "$MODDIR/$m.ko" ]; then
		kp "WARN $m.ko missing"
		continue
	fi
	if ! grep -q "^$(echo "$m" | tr '-' '_') " /proc/modules 2>/dev/null; then
		insmod "$MODDIR/$m.ko" || kp "WARN insmod $m failed"
	fi
done

if ! grep -q "^sc8989x_charger " /proc/modules 2>/dev/null; then
	insmod "$MODDIR/sc8989x_charger.ko" || kp "WARN insmod sc8989x failed"
fi

# USB OTG VBUS control.  This wrapper resolves charger_class's
# get_charger_by_name()/charger_dev_enable_otg() at load time, so it must come
# after sc8989x has registered "primary_chg".  Userspace toggles it through
# /sys/kernel/pmos_otg_vbus/enable (pmos-usb-role host|device).
if [ -f "$MODDIR/pmos_otg_vbus.ko" ] &&
	! grep -q "^pmos_otg_vbus " /proc/modules 2>/dev/null; then
	load_otg_vbus
fi

# The cap module needs the mm8013 gauge.
i=0
while [ ! -e /sys/class/power_supply/battery/capacity ] && [ "$i" -lt 40 ]; do
	sleep 0.5
	i=$((i + 1))
done

case "$LIMIT" in
	full|100|"") L=100 ;;
	*) L=$LIMIT ;;
esac

if ! grep -q "^pmos_charge_cap " /proc/modules 2>/dev/null; then
	insmod "$MODDIR/pmos_charge_cap.ko" limit="$L" input_current_ua="$INPUT_UA" ||
		kp "WARN charge cap load failed"
fi

if [ -e /sys/kernel/pmos_charge_cap/limit ]; then
	echo OK > "$STATUS"
else
	echo NOK > "$STATUS"
fi

kp "charger stack ready: limit=$(cat /sys/kernel/pmos_charge_cap/limit 2>/dev/null) charging=$(cat /sys/kernel/pmos_charge_cap/charging 2>/dev/null)"
