#!/bin/sh
# Lenovo TB351FU (MT6789) Bluetooth bring-up via MTK stpbt + pty bridge + btattach
KMSG=/dev/kmsg
kp() { echo "PMOSBT: $*" > "$KMSG" 2>/dev/null; echo "PMOSBT: $*"; }

kp "=== Starting Bluetooth bring-up ==="

# 1. Clean up any stale instances
killall -9 btattach 2>/dev/null || true
pkill -9 -f btbridge.py 2>/dev/null || true
sleep 0.2

# 2. Ensure dependency modules are loaded in strict dependency order:
#    bluetooth.ko MUST be loaded BEFORE btbcm, btqca, and hci_uart!
for mod in bluetooth pwrseq-core btbcm btqca hci_uart bt_drv_connac1x; do
    mod_sys=$(echo "$mod" | tr - _)
    if [ ! -d "/sys/module/$mod_sys" ]; then
        if [ -f "/lib/modules/extra/$mod.ko" ]; then
            insmod "/lib/modules/extra/$mod.ko" 2>/dev/null && kp "insmod $mod ok" || kp "insmod $mod failed"
        fi
    fi
done

# 3. Wait for /dev/stpbt
for i in $(seq 1 15); do
    [ -c /dev/stpbt ] && break
    sleep 0.2
done

if [ ! -c /dev/stpbt ]; then
    kp "ERROR: /dev/stpbt not available after timeout"
    exit 1
fi
kp "/dev/stpbt is ready"

# 4. Start btbridge.py
rm -f /tmp/btbridge.log
python3 /root/btbridge.py > /tmp/btbridge.log 2>&1 &
BRIDGE_PID=$!
kp "Started btbridge.py (pid $BRIDGE_PID)"

SLAVE=""
for i in $(seq 1 25); do
    if [ -f /tmp/btbridge.log ]; then
        SLAVE=$(grep -oE "/dev/pts/[0-9]+" /tmp/btbridge.log | head -n 1)
        [ -n "$SLAVE" ] && [ -e "$SLAVE" ] && break
    fi
    sleep 0.1
done

if [ -z "$SLAVE" ] || [ ! -e "$SLAVE" ]; then
    kp "ERROR: Slave pty not found in /tmp/btbridge.log"
    cat /tmp/btbridge.log >&2
    exit 2
fi
kp "Using pty slave: $SLAVE"

# 5. Start btattach
btattach -B "$SLAVE" > /tmp/btattach.log 2>&1 &
ATTACH_PID=$!
kp "Started btattach (pid $ATTACH_PID)"

# 6. Wait for hci0 to appear
for i in $(seq 1 25); do
    [ -d /sys/class/bluetooth/hci0 ] && break
    sleep 0.2
done

if [ -d /sys/class/bluetooth/hci0 ]; then
    kp "hci0 is UP!"
    systemctl start bluetooth 2>/dev/null || true
    # bluetoothd needs a moment after hci0 appears; the first boot-time
    # `power on` often races it and fails with org.bluez.Error.Failed.
    # Retry until the adapter actually reports Powered: yes.
    BT_ON=0
    for i in $(seq 1 30); do
        rfkill unblock bluetooth 2>/dev/null || true
        bluetoothctl power on >/dev/null 2>&1 || true
        if bluetoothctl show 2>/dev/null | grep -q "Powered: yes"; then
            BT_ON=1
            kp "Bluetooth adapter powered on (attempt $i)"
            break
        fi
        sleep 1
    done
    if [ "$BT_ON" -eq 1 ]; then
        kp "=== Bluetooth bring-up SUCCESS ==="
    else
        kp "WARN: adapter present but could not power on"
        exit 4
    fi
else
    kp "WARN: hci0 did not appear within timeout"
    exit 3
fi
