#!/bin/sh
# pmos-wifi-supervisor.sh — v7: OBSERVE-ONLY.
# v6 lesson (boots 4205ba79 + c9acbce0, 2026-10-31): both auto-reboots
# after a NOK wedge produced WORSE next-boot states (boot after warm
# reset oopsed in polling_consys_chipid_mt6789_gen with dummy
# regulators). The wedge class is not software-reset recoverable, so the
# supervisor must NOT reboot: log state, leave fail_count untouched,
# let a human power-button reset (cold boot) do the recovery.
# hbguard/HW-WDT still covers full kernel death.
kp() { echo "PMOSWIFISUP: $*" > /dev/kmsg 2>/dev/null; }
kp "supervisor started (pid $$)"

n=0
while [ ! -f /run/pmos_wifi_status ] && [ "$n" -lt 45 ]; do
    sleep 2; n=$((n + 1))
done
ST="MISSING"
[ -f /run/pmos_wifi_status ] && ST=$(cat /run/pmos_wifi_status)
kp "status=$ST after $((n * 2)) s"
[ "$ST" = "NOK" ] || exit 0

kp "NOK: observing only (no auto-reboot by policy) - logs:"
journalctl -b --no-pager 2>/dev/null | grep -aE "WMT-CONSYS|goto_idle|VOORG|VOOT|pwr_on|chipid|Oops|abort" \
  | grep -avE "aw88395|iis|IIOS" | tail -12 > /tmp/pmsup_wmt_tail.txt 2>/dev/null
while IFS= read -r ln; do kp "$ln"; done < /tmp/pmsup_wmt_tail.txt
journalctl --flush 2>/dev/null
exit 0
