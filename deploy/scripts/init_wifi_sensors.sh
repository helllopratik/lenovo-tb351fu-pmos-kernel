#!/bin/sh
# postmarketOS WiFi bring-up for Lenovo TB351FU (mt6789 / Helio G99 CONSYS)
# v12: adds INFRA_AO CCIF clock-gate SET pokes just before the single
#      wmtWifi write. Ground truth (boot f33396ce + stock clk-mt6789-bus.ko
#      teardown): the consys DT asks for clock 'ccif' (id 0x38) which maps to
#      infra_ao gate ifrao_ccif4_ap {set 0x100010c0, bit 28}; no mt6789
#      clk driver exists in the pmOS kernel so that gate stays OFF and the
#      consys CR domain reads 0x0 even with the island powered+acked.
#      wmt_drv never clk_get()s it (consys_clk_get_from_dts has no
#      callers), so we poke the gate directly from userspace.
# v5: kmsg telemetry (every step), stamp-clear REMOVED (2 boots died in its
#     RD of 0x18002600 - cold CONSYS AHB pre/island-defunct), XO-early PMIC-only,
#     status file for the wifi supervisor service.
# Logs: /tmp/pmos_wifi_init.log (detail) + /dev/kmsg (PMOSWIFI lines).
KMSG=/dev/kmsg
kp() { echo "PMOSWIFI: $*" > "$KMSG" 2>/dev/null; }
exec > /tmp/pmos_wifi_init.log 2>&1
kp "=== bring-up starting ==="

# ------------------------------------------------------------------
# -1. v17 AUX: aw88395 flood suppression (TEST-BOOT ONLY, flag-gated).
#     The smart-amp driver spams ~20k console lines/60 s (~640 KB/s),
#     wrapping both pstore records to a ~1.4 s tail (v16 lesson: a crash
#     mid-flood leaves nothing usable). Gated by /root/aw_flood_suppress:
#     a normal boot without the flag keeps stock console behavior.
#     Order: source-level first (rmmod / dynamic-debug off), printk
#     console level last (kmsg ring + journald keep recording regardless).
# ------------------------------------------------------------------
if [ -f /root/aw_flood_suppress ]; then
    kp "flood-suppress: flag present - silencing aw88395 spam"
    before=$(dmesg 2>/dev/null | grep -c aw88395)
    # 1) unload if modular (audio stays silent for this test boot; next
    #    non-flagged boot restores it)
    for m in $(lsmod 2>/dev/null | grep -iE "aw88|aw87" | awk '{print $1}'); do
        modprobe -r "$m" 2>/dev/null && kp "flood-suppress: rmmod $m ok" || kp "flood-suppress: rmmod $m failed (built-in or in-use)"
    done
    # 2) dynamic-debug off for the driver sources (silences prints while
    #    keeping the driver alive if it is built-in)
    if [ -d /sys/kernel/debug/dynamic_debug ]; then
        for f in aw88395.c aw88395_lib.c aw88395_algo.c aw88395_monitor.c aw88395_parse.c aw87xxx.c; do
            echo "file $f -p" > /sys/kernel/debug/dynamic_debug/control 2>/dev/null
        done
        kp "flood-suppress: ddebug -p pushed for aw88395/aw87xxx sources"
    else
        kp "flood-suppress: no dynamic_debug in this kernel"
    fi
    # 3) belt: console loglevel 1 (KERN_ALERT+: arm64 oopses go out at
    #    pr_alert, so they stay visible on console-ramoops; the info-level
    #    flood stops reaching the console driver)
    echo 1 > /proc/sys/kernel/printk 2>/dev/null || dmesg --console-off 2>/dev/null
    kp "flood-suppress: printk console level -> 1"
    sleep 3
    after=$(dmesg 2>/dev/null | grep -c aw88395)
    kp "flood-suppress check: aw88395 rings before=$before after3s=$after (delta 0-2 = engaged)"
else
    kp "no flood-suppress flag (stock console behavior)"
fi

# ------------------------------------------------------------------
# 0. genpd provider stub for "mediatek,mt6789-scpsys"  (REQUIRED!)
#    This kernel has no MT6789 SPM/SCPSYS power-domain driver, so every DT
#    node with power-domains = <&scpsys N> (notably soc/consys@18002000)
#    fails dev_pm_domain_attach() with -ETIMEDOUT and can never probe.
#    The stub registers token always-on domains; WMT does the real HW
#    power sequencing (pmic/EMI/SPM), so no SPM interaction is needed.
# ------------------------------------------------------------------
if [ ! -d /sys/module/mt6789_pd_stub ]; then
    if insmod /lib/modules/extra/mt6789_pd_stub.ko; then kp "pd_stub loaded"; else kp "WARN pd_stub insmod failed"; fi
fi

# ------------------------------------------------------------------
# 0.5  XO-EARLY: CONN 26 MHz XO via PMIC CW00 BEFORE any connsys module.
#      CW00 (0x788): 0x4e1d (CO_BUF, EN=0) -> 0x4e05 (SW, EN=1).
#      early_xo.py is PMIC-only (NO CONSYS MMIO - cold-AHB sync external
#      abort on boot d0d6b96a; don't re-introduce stamp clearing here).
#      Module names: mt6789_pmicpoke (/dev/pmicpoke), mt6789_spm_probe
#      (/dev/spmpoke). spmpoke is loaded HERE for the hbguard feeder.
# ------------------------------------------------------------------
kp "loading poke+spmpoke, running early_xo"
if [ ! -d /sys/module/mt6789_pmicpoke ]; then
    if insmod /root/mt6789_pmicpoke.ko; then kp "pmicpoke loaded"; else kp "WARN pmicpoke insmod failed"; fi
fi
if [ ! -d /sys/module/mt6789_spm_probe ]; then
    if insmod /root/spmpoke.ko; then kp "spmpoke loaded"; else kp "WARN spmpoke insmod failed"; fi
fi
# Python canary: proves interpreter works + version visible in kmsg.
python3 -c "open('/dev/kmsg','w').write('<6>PMOSWIFI: python canary: %s\n' % __import__('sys').version)" || kp "WARN python canary FAILED - python env broken"
python3 /root/early_xo.py || kp "WARN early_xo.py rc=$?"

# ------------------------------------------------------------------
# 0.6  v8: consys VCN LDO enable via pmicpoke (MT6366/MT6358 map).
#      The consys DT node has NO regulator-* supplies -> mtk_wmt gets
#      DUMMY regulators ("supply vcn18 not found") and its vcn turn-on
#      steps become no-ops; that starves CONSYS of its power rails and
#      goto_idle then reads 0x0 / 0x70 (boot 4205ba79 idle-poll -7).
#      Stock enables: VCN18, VCN33_1(BT/WiFi shared), VCN28(efuse).
#      Registers (mt6358../include/linux/mfd/mt6358/registers.h):
#        LDO_VCN18_CON0 0x1c58 bit0=1
#        LDO_VCN33_CON0_0 0x1d1c bit0=1;  CON0_1 0x1d2a bit0=0
#          (mt6358_sync_vcn33_setting: CON0_0.set(BIT0), CON0_1.clr(BIT0))
#        LDO_VCN28_CON0 0x1d88 bit0=1
#      All are read-modify-write, kmsg-logged, and non-fatal on error.
# ------------------------------------------------------------------
python3 - <<'PYEOF'
import fcntl, os, struct
try:
    fd = os.open('/dev/pmicpoke', os.O_RDWR)
except OSError as e:
    print('LDOSKIP no pmicpoke %r' % e)
    raise SystemExit(0)
FMT = '<QQII'
def cmd(n): return (3 << 30) | (24 << 16) | (0x70 << 8) | n
def rd(a):
    b = struct.pack(FMT, a & 0xffff, 0, 0, 0)
    try:
        b2 = fcntl.ioctl(fd, cmd(1), b)
    except OSError as e:
        return -1
    return struct.unpack(FMT, b2)[1]
def wr(a, v):
    fcntl.ioctl(fd, cmd(2), struct.pack(FMT, a & 0xffff, v & 0xffff, 0, 0))
def kline(s):
    try:
        open('/dev/kmsg', 'w').write('<6>PMOSWIFI: vcnldo: %s\n' % s)
    except OSError:
        pass
    print('vcnldo: ' + s)

cn18, cn33_a, cn33_b, cn28 = 0x1c58, 0x1d1c, 0x1d2a, 0x1d88
r18, r33a, r33b, r28 = rd(cn18), rd(cn33_a), rd(cn33_b), rd(cn28)
kline('pre VCN18=0x%04x VCN33A=0x%04x VCN33B=0x%04x VCN28=0x%04x' % (r18 & 0xffff, r33a & 0xffff, r33b & 0xffff, r28 & 0xffff))
if r18 >= 0:
    wr(cn18, r18 | 0x1)
    wr(cn33_a, r33a | 0x1)
    wr(cn33_b, r33b & ~0x1)
    wr(cn28, r28 | 0x1)
    p18, p33a, p33b, p28 = rd(cn18), rd(cn33_a), rd(cn33_b), rd(cn28)
    kline('post VCN18=0x%04x VCN33A=0x%04x VCN33B=0x%04x VCN28=0x%04x' % (p18 & 0xffff, p33a & 0xffff, p33b & 0xffff, p28 & 0xffff))
os.close(fd)
PYEOF
[ $? -eq 0 ] && kp "vcn LDO write step done" || kp "WARN vcnldo step rc=$?"

# ------------------------------------------------------------------
# 1. Base connectivity adapters & WMT core driver
# ------------------------------------------------------------------
kp "loading connadp/btif_drv/connfem/wmt_drv"
insmod /lib/modules/extra/connadp.ko && kp "connadp ok"
insmod /lib/modules/extra/btif_drv.ko && kp "btif_drv ok"
insmod /lib/modules/extra/connfem.ko && kp "connfem ok"
insmod /lib/modules/extra/wmt_drv.ko && kp "wmt_drv ok"
sleep 0.5

# 2. Chip ID + WMT common driver init via /dev/wmtdetect
kp "wmtdetect chip-id 0x6789 + WMT common init"
python3 - <<'PYEOF'
import os, fcntl
fd = os.open('/dev/wmtdetect', os.O_RDWR)
# v6: pass chip id BY VALUE (int arg). v5 passed a bytes buffer and the
# kernel logged a raw userspace pointer as the chip id
# ("chipid(548969680240) ... set to 0xd1248d70" garbage). int arg = one
# syscall register value, no deref ambiguity.
try:
    fcntl.ioctl(fd, 0x40047701, 0x6789)
    print('chip id set to 0x6789 (int arg)')
except OSError as e:
    print('chip id int arg err %r - retrying as bytes' % e)
    fcntl.ioctl(fd, 0x40047701, (0x6789).to_bytes(8, 'little'))
    print('chip id set via 8-byte buffer')
try:
    fcntl.ioctl(fd, 0x80047704, bytes([0] * 4))
    print('WMT common drv init done')
except OSError as e:
    print('WMT common drv init note: %r' % e)
PYEOF

# ------------------------------------------------------------------
# 3. Register the CONSYS platform driver.
#    ioctl 0x8004770A registers "mtk_wmt" and waits for its probe.
# ------------------------------------------------------------------
kp "registering mtk_wmt for 18002000.consys"
setsid python3 -c "
import os, fcntl
fd = os.open('/dev/wmtdetect', os.O_RDWR)
fcntl.ioctl(fd, 0x8004770A, bytes([0]*4))
print('consys hw-init done')
" </dev/null >/dev/null 2>&1 &

n=0
while [ "$n" -lt 30 ]; do
    readlink /sys/bus/platform/devices/18002000.consys/driver >/dev/null 2>&1 && break
    sleep 0.5
    n=$((n + 1))
done
if readlink /sys/bus/platform/devices/18002000.consys/driver >/dev/null 2>&1; then
    kp "CONSYS bound to mtk_wmt"
else
    kp "WARN CONSYS not bound after 15 s (probe may finish later; 4.5 will wait)"
fi

# 4. WiFi chrdev & WLAN network driver
kp "loading wmt_chrdev_wifi + wlan_drv_gen4m_6789"
insmod /lib/modules/extra/wmt_chrdev_wifi.ko && kp "wmt_chrdev_wifi ok"
insmod /lib/modules/extra/wlan_drv_gen4m_6789.ko && kp "wlan_drv ok"
sleep 1

# ------------------------------------------------------------------
# 4.5  v8: WAIT for the mtk_wmt platform probe to finish BEFORE the
#      single wmtWifi write. Boot 1aed4768 proof: with the probe still
#      mid-flight (EMI not yet allocated), a pwr_on attempt fails early
#      with "EMI base address is invalid" (-4) and lands the core in
#      the half-w梦 state we later oopsed out of (polling_consys_chipid).
#      Probe completion marker = the kernel line
#      "mtk_wmt_probe:Clearing Connsys EMI"; hard timeout 90 s.
# ------------------------------------------------------------------
kp "waiting for mtk_wmt probe completion (EMI marker)"
n=0
PROBE_DONE=0
while [ "$n" -lt 90 ]; do
    if dmesg 2>/dev/null | grep -aq "Clearing Connsys EMI"; then
        PROBE_DONE=1
        break
    fi
    sleep 1
    n=$((n + 1))
done
if [ "$PROBE_DONE" -eq 1 ]; then
    kp "mtk_wmt probe COMPLETE after ${n}s (EMI allocated)"
else
    kp "WARN probe not complete after 90 s - proceeding anyway (single attempt)"
fi

# ------------------------------------------------------------------
# 4.9  v12: INFRA_AO CCIF clock gates for CONSYS (via /dev/spmpoke).
#      Consys DT request: clocks = <infracfg_ao 0x38>  ==  ifrao_ccif4_ap:
#        set reg 0x100010c0, clr reg 0x100010c4, status read 0x100010c8;
#        ccif4_ap = bit 28 (shift 0x1c, per stock clk-mt6789-bus.ko table).
#      On boot f33396ce the status word read 0x01004000 (bit28 clear) and
#      the whole chip-side CR domain (0x180b1010 "0x100c0000" version reg
#      and friends) answered 0x0 even though SPM says the island is
#      powered + acked (0x10006300/04 = 0x0d/0x0d) - consistent with
#      "powered but unclocked".
#      Here we SET (write-to-set, atomic, idempotent):
#        ccif4_ap  bit28 + ccif2_ap bit16  @ 0x100010c0
#        ccif_ap   bit23 + ccif1_ap bit12  @ 0x10001088
#      then log the resulting status + a chip liveness read.
# ------------------------------------------------------------------
if [ -e /dev/spmpoke ]; then
python3 - <<'PYEOF'
import fcntl, os, struct
try:
    fd = os.open('/dev/spmpoke', os.O_RDONLY)
except OSError as e:
    open('/dev/kmsg', 'w').write('<4>PMOSWIFI: v12ccif: no spmpoke %r\n' % (e,))
    raise SystemExit(0)
RD, WR = 0xC0185301, 0xC0185302

def wr(a, v):
    b = bytearray(struct.pack('QQII', a, v, 0, 0))
    fcntl.ioctl(fd, WR, b)
    return struct.unpack('QQII', bytes(b))[1]

def rd(a):
    b = bytearray(struct.pack('QQII', a, 0, 0, 0))
    fcntl.ioctl(fd, RD, b)
    return struct.unpack('QQII', bytes(b))[1]

def k(s):
    open('/dev/kmsg', 'w').write('<6>PMOSWIFI: v12ccif: %s\n' % s)
    print(s)

try:
    k('pre sta0xc8=0x%08x sta0x94=0x%08x' % (rd(0x100010c8), rd(0x10001094)))
    wr(0x100010c0, 0x10010000)            # SET ccif4_ap(28) + ccif2_ap(16)
    wr(0x10001088, 0x00801000)            # SET ccif_ap(23)  + ccif1_ap(12)
    k('post sta0xc8=0x%08x sta0x94=0x%08x' % (rd(0x100010c8), rd(0x10001094)))
    v = rd(0x180b1010)
    k('chip version reg 0x180b1010 = 0x%08x (alive if 0x100c0000)' % v)
except OSError as e:
    k('poke failed %r' % (e,))
PYEOF
else
    kp "WARN v12ccif: /dev/spmpoke not present"
fi

# 5. Turn on WiFi function in WMT — SINGLE ATTEMPT ONLY (v7).
#    v6 evidence (boot 4205ba79): the SECOND wmtWifi write after a first
#    -7 hits "already powered on, WMT DRV_STS_[0x1]" — the core then sits
#    in a half-powered state and the system wedges ~9 s later. Stock
#    Android never chains a second power-on over a failed one, so we
#    don't either. NO rfkill meddling with the WMT state either.
# 4.9b v15: replicate the stock mtk_wmtd userspace pre-setup that pmOS lacks:
#    SET-HIF (ioctl 0x4004a005 arg 0x13 = BTIF + co-clock 1; v14's 0x14 meant
#    SDIO and sent stp_init into the first-time SDIO arm where
#    mtk_wcn_hif_sdio_wmt_control -> "no supported func probed" ->
#    SDIO_FUNC_STP func fail (-8)) and the ROMpatch table entries
#    (ioctl 0x4008a01f, <II256s> = type/EMI-offset/filename for
#    types 0/3/4) on /dev/stpwmt, BEFORE the wmtWifi write. Non-fatal on any
#    error (falls back to bare v13 behavior). Idempotent — safe every boot.
# 4.9c v16: the shim also forked a /dev/stpwmt fd-holder (gWmtClose stays 0:
#    WMT_close only fires the flag when the LAST fd closes; the v15 per-op
#    open/close left gWmtClose=1 at wifi-on time and the rx path shed the
#    chip REG EVT, "Rx REG_EVT fail!(0) len(0,16)"). v16 CRASHED the boot
#    (console pstore ends abruptly t=34.2585 s mid-flood, no oops) —
#    postmortem: stock wmt_launcher (pulled + disassembled) holds the fd AND
#    CONSUMES it: blocking poll(POLLIN, -1) -> read(<=0x100) forever. A held
#    fd with no reader is not stock state; the suspected wedge is the
#    kernel-side rx queue with no consumer.
# 4.9d v17: the holder now runs the stock watchdog loop — poll(POLLIN,
#    500 ms) -> one read(0x100) per wake, reopen at 300 ms on EOF, matching
#    stock binary VA 0x6650-0x6ab0 (disasm: /tmp/opencode/stock/
#    wmt_full_dis.txt). Also opens with stock flags O_RDWR|O_NOCTTY and
#    probes chip id via the read ioctl 0x8004a00c (stock VA 0x4174).
#    All idempotent; every boot may stack a fresh holder (stock reopen.
if [ -f /root/wmtshim.py ]; then
    kp "v17: wmtshim.py (SET-HIF + ROMpatch table + poll/read fd-holder) starting"
    python3 /root/wmtshim.py && kp "v17: wmtshim rc=0 (holder draining)" || kp "v17: wmtshim rc=$? (check holder)"
else
    kp "WARN v17: /root/wmtshim.py missing (bare v13 path)"
fi
kp "echo 1 > /dev/wmtWifi (SINGLE attempt)"
echo 1 > /dev/wmtWifi && kp "wmtWifi write accepted" || kp "WARN wmtWifi write failed"
sleep 3

# 6. Report status + write supervisor state
if ip link show wlan0 >/dev/null 2>&1; then
    echo OK > /run/pmos_wifi_status
    kp "STATUS=WLAN0-UP (bring-up complete)"
else
    echo NOK > /run/pmos_wifi_status
    kp "STATUS=NOK - no retry by design (see v7 notes); grab kmsg now"
fi
kp "=== bring-up sequence complete ==="

# 7. Issue 41: Bluetooth bring-up via MTK stpbt + pty bridge + btattach
if [ -x /usr/local/bin/init_bt.sh ]; then
    kp "Starting Bluetooth bring-up"
    /usr/local/bin/init_bt.sh > /tmp/pmos_bt_init.log 2>&1 &
fi

# 8. SensorHub / SCP disabled to protect SPI2 touch bus
kp "SensorHub / SCP modules disabled to prevent SPI2 timeouts"
# 9. Active pen / game mode: MATCH STOCK NO-PEN STEADY STATE.
#    Stock init.stylus.rc writes support_pen=1 then game_mode=1 at boot,
#    but Lenovo PalmGuard immediately drops game_mode to 0 while no pen
#    is connected and keeps support_pen=1 for the whole uptime.  We have
#    no PalmGuard, so write that steady state directly (1/0).  Our old
#    0/0 default was non-stock.  The "1/1 kills touch after 20 s" note
#    came from the wrong 123-byte read; this kernel reads 109 bytes and
#    parses the pen packet at offset 66 like stock Android.
if [ -e /proc/support_pen ]; then echo 1 > /proc/support_pen 2>/dev/null; fi
if [ -e /proc/game_mode ]; then echo 0 > /proc/game_mode 2>/dev/null; fi
if [ -e /sys/bus/spi/devices/spi2.0/power/control ]; then
    echo on > /sys/bus/spi/devices/spi2.0/power/control 2>/dev/null
fi
