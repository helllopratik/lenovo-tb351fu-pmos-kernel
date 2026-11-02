#!/bin/sh
echo "PMOSUSB: starting gadget configuration..." > /dev/kmsg 2>/dev/null || true
echo 1 > /proc/pmos_cancel 2>/dev/null || true
echo "PMOSUSB: auto-reboot watchdog disarmed" > /dev/kmsg 2>/dev/null || true

mount -t configfs none /sys/kernel/config 2>/dev/null || true
GADGET=/sys/kernel/config/usb_gadget/g1
if [ ! -d "$GADGET" ]; then
    mkdir -p $GADGET
    cd $GADGET
    echo 0x18d1 > idVendor
    echo 0x4ee7 > idProduct
    echo 0x0200 > bcdUSB
    mkdir -p strings/0x409
    echo "TB351FU_PMOS" > strings/0x409/serialnumber
    echo "Lenovo" > strings/0x409/manufacturer
    echo "Tab Plus pmOS" > strings/0x409/product

    mkdir -p functions/acm.usb0
    mkdir -p functions/ncm.usb0

    # Static persistent MAC addresses so host PC never sees random adapters
    echo "02:00:42:00:00:01" > functions/ncm.usb0/host_addr 2>/dev/null || true
    echo "02:00:42:00:00:02" > functions/ncm.usb0/dev_addr 2>/dev/null || true

    mkdir -p configs/c.1/strings/0x409
    echo "USB Network and Serial Console" > configs/c.1/strings/0x409/configuration
    ln -sf functions/acm.usb0 configs/c.1/
    ln -sf functions/ncm.usb0 configs/c.1/

    UDC=$(ls /sys/class/udc 2>/dev/null | head -n 1)
    if [ -n "$UDC" ]; then
        echo "$UDC" > UDC
        echo "PMOSUSB: bound gadget to UDC $UDC" > /dev/kmsg 2>/dev/null || true
    fi
fi

ip link set usb0 up 2>/dev/null || ifconfig usb0 up 2>/dev/null || true
ip addr add 172.16.42.1/24 dev usb0 2>/dev/null || ifconfig usb0 172.16.42.1 netmask 255.255.255.0 2>/dev/null || true

# Continuous keepalive daemon for usb0 network
(
  while true; do
    if ip link show usb0 2>/dev/null | grep -q 'state DOWN'; then
        ip link set usb0 up 2>/dev/null
    fi
    if ! ip addr show usb0 2>/dev/null | grep -q '172.16.42.1'; then
        ip addr add 172.16.42.1/24 dev usb0 2>/dev/null
    fi
    sleep 2
  done
) &

# Serial console on ttyGS0 (provides instant shell on /dev/ttyACM0 even without IP)
if ! pgrep -f 'agetty.*ttyGS0' >/dev/null 2>&1; then
    nohup /sbin/agetty -a root -L -s 115200 ttyGS0 vt100 >/dev/null 2>&1 &
fi

# Telnet daemon on port 23
if ! pgrep -f 'telnetd' >/dev/null 2>&1; then
    if command -v telnetd >/dev/null 2>&1; then
        telnetd -l /bin/sh -p 23 2>/dev/null &
    elif command -v busybox >/dev/null 2>&1; then
        busybox telnetd -l /bin/sh -p 23 2>/dev/null &
    fi
fi
