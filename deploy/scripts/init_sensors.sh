#!/bin/sh
# SCP sensorhub bring-up for Lenovo TB351FU (MT6789).
#
# The accelerometer/gyro (bmi3xy) and ambient light sensor (mn29xxx) sit
# behind the MTK SCP, not on the AP buses.  The Android vendor stack exposes
# them through scp.ko -> hf_manager.ko -> sensorhub.ko and /dev/hf_manager.
#
# ts_scp_common.ko is intentionally NOT loaded: it hands the Novatek touch
# controller off to SCP.  We keep touch on the AP (spi2) and only use SCP for
# the real sensors.
#
# Runs in the background from init_wifi_sensors.sh so a slow/failed SCP probe
# can never delay the graphical boot.  Log: /tmp/pmos_sensors_init.log and
# PMOSSENS lines in kmsg.

KMSG=/dev/kmsg
MODDIR=/lib/modules/extra
STATUS=/run/pmos_sensors_status

# The vendor hf_manager.ko reads task_struct fields at Android offsets and
# NULL-derefs in hf_client_create() with this kernel.  The patched module is
# mandatory; refuse to load the bridge when the wrong binary is staged, or a
# single bad package turns into a boot loop.
HF_MANAGER_PATCHED_SHA256=f2d8828a2ddedcbeb36960a1b0846a5134ac6d4ffab9e87342e9cc5ef0fbeed4

kp() { echo "PMOSSENS: $*" > "$KMSG" 2>/dev/null; }
exec > /tmp/pmos_sensors_init.log 2>&1

kp "=== SCP sensor stack starting ==="
echo NOK > "$STATUS"

load_one() {
    mod="$1"
    # /proc/modules reports hyphens as underscores (mtk-mbox -> mtk_mbox).
    name=$(echo "$mod" | tr '-' '_')
    if grep -q "^${name} " /proc/modules 2>/dev/null; then
        kp "$mod already loaded"
        return 0
    fi
    if [ ! -f "$MODDIR/$mod.ko" ]; then
        kp "WARN $mod.ko missing"
        return 1
    fi
    if insmod "$MODDIR/$mod.ko"; then
        kp "$mod loaded"
        return 0
    fi
    kp "WARN $mod insmod failed rc=$?"
    return 1
}

# Dependency order required by the vendor modules.
for m in mtk-mbox mtk_rpmsg_mbox mtk_tinysys_ipi scp hf_manager sensorhub; do
    load_one "$m" || kp "continuing after $m failure"
done

# scp.ko binds to 10500000.scp and sensorhub creates /dev/hf_manager.
n=0
while [ "$n" -lt 40 ]; do
    [ -e /dev/hf_manager ] && break
    sleep 0.5
    n=$((n + 1))
done

if [ -e /sys/bus/platform/drivers/scp/10500000.scp ]; then
    kp "SCP platform device bound"
else
    kp "WARN SCP platform device not bound"
fi

if [ -e /dev/hf_manager ]; then
    kp "STATUS=HF_MANAGER_UP after ${n} polls"
    ls -l /dev/hf_manager >> /tmp/pmos_sensors_init.log 2>&1
    echo OK > "$STATUS"

    # Expose the SCP sensors through IIO.  The bridge creates one hf_manager
    # client and republishes accel/gyro/ALS as iio:deviceN, which lets udev
    # start iio-sensor-proxy for Phosh auto-brightness and rotation.
    #
    # sensorhub publishes its sensor list asynchronously after probe, so the
    # bridge's hf_client_find_sensor() can still see an empty list right
    # after /dev/hf_manager appears (cold boot failed with -ENODEV at 28 s).
    # Retry until the module stays loaded.  BusyBox insmod reports success
    # even when the initcall fails, so /proc/modules is the only reliable
    # indicator that the bridge actually initialised.
    hf_sha=$(sha256sum "$MODDIR/hf_manager.ko" 2>/dev/null | awk '{print $1}')
    if [ "$hf_sha" != "$HF_MANAGER_PATCHED_SHA256" ]; then
        kp "WARN hf_manager.ko is not the patched build (${hf_sha:-missing}); skipping sensor bridge"
    else
        retry=0
        while [ "$retry" -lt 60 ]; do
            grep -q "^pmos_sensor_bridge " /proc/modules 2>/dev/null && break
            insmod "$MODDIR/pmos_sensor_bridge.ko" 2>/dev/null
            retry=$((retry + 1))
            sleep 0.5
        done
        if grep -q "^pmos_sensor_bridge " /proc/modules 2>/dev/null; then
            kp "pmos_sensor_bridge loaded after ${retry} attempts"

            # udev may start iio-sensor-proxy when the first IIO device appears,
            # before all three exist.  Re-trigger after the bridge has finished
            # and restart the proxy if it already enumerated a partial set.
            sleep 1
            udevadm trigger --subsystem-match=iio 2>/dev/null
            systemctl try-restart iio-sensor-proxy 2>/dev/null
        else
            kp "WARN pmos_sensor_bridge not loaded after ${retry} attempts"
        fi
    fi
else
    kp "STATUS=NO_HF_MANAGER after ${n} polls"
fi

# Read-only proof of what SCP advertised.  Keep this light: no debugfs.
if [ -d /proc/sensor_info ]; then
    for f in /proc/sensor_info/*; do
        [ -f "$f" ] || continue
        echo "== $f" >> /tmp/pmos_sensors_init.log
        cat "$f" >> /tmp/pmos_sensors_init.log 2>&1
    done
fi
if [ -f /proc/hf_manager ]; then
    echo "== /proc/hf_manager" >> /tmp/pmos_sensors_init.log
    cat /proc/hf_manager >> /tmp/pmos_sensors_init.log 2>&1
fi

# Keep the useful kernel lines, not the whole ring.
dmesg 2>/dev/null | grep -iE 'scp|sensorhub|hf_manager|tinysys' | tail -60 \
    >> /tmp/pmos_sensors_init.log 2>&1
kp "=== SCP sensor stack probe complete ==="
