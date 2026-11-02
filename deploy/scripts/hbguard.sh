#!/bin/sh
# hbguard.sh — v5 TopRGU raw-MMIO feeder + systemd-liveness direct-reset.
# WHY: (1) /dev/watchdog0 takes one opener only (systemd holds it => any
# extra feeder dies EBUSY; raw MMIO bypasses that). (2) On the two hard
# pmOS wedges (v3/v4 boots) the HW watchdog did NOT fire even with
# systemd armed - here we FORCE-ARM TopRGU directly and log the mode.
#   WDT_RST  = 0x10007008, reload key 0x1971
#   WDT_MODE = 0x10007000, key 0x22000000, EN(1)|EXRST_EN(4)|DUAL_EN(64)
#   WDT_SWRST= 0x10007014, full-AP warm reset key 0x1209
kp() { echo "PMOSHB: $*" > /dev/kmsg 2>/dev/null; }
POKE=/root/poke.py

n=0
while [ ! -e /dev/spmpoke ] && [ "$n" -lt 30 ]; do
    sleep 1; n=$((n + 1))
done
if [ ! -e /dev/spmpoke ]; then
    kp "no /dev/spmpoke after 30 s - exiting (HW wk left to systemd)"
    exit 0
fi

# Force-arm TopRGU hardware once (key-protected, idempotent):
MODE=$(/usr/bin/python3 "$POKE" wr 0x10007000 0x22000045 2>/dev/null)
kp "TopRGU force-arm: $MODE"

DEAD=0
N=0
while :; do
    if /usr/bin/python3 "$POKE" wr 0x10007008 0x1971 >/dev/null 2>&1; then
        DEAD=0
    else
        N=$((N + 1))
        kp "reload write FAIL #$N"
    fi
    if [ $((N % 2)) -eq 0 ]; then
        OUT=$(/usr/bin/systemctl is-system-running 2>/dev/null)
        case "$OUT" in
            running|degraded|starting|initializing|stopping) : ;;
            *) DEAD=$((DEAD + 1)); kp "systemctl strike #$DEAD ($OUT)" ;;
        esac
        if [ "$DEAD" -ge 2 ]; then
            kp "systemd unreachable $DEAD strikes -> direct SWRST (0x1209)"
            sync
            /usr/bin/python3 "$POKE" wr 0x10007014 0x1209 >/dev/null 2>&1
            sleep 10
            /usr/bin/python3 "$POKE" wr 0x10007014 0x1209 >/dev/null 2>&1
        fi
    fi
    sleep 5
done
