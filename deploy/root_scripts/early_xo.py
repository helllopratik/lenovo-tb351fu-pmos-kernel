#!/usr/bin/env python3
# early_xo.py — v7: PMIC-only XO_WCN enable, fully instrumented.
#
# v7 (2026-10-31, clkbuf.ko descriptor-table decode): XO-WCN enable bit is
# CW00 (0x788) BIT5 (mask 0x20) — confirmed by stock clkbuf.ko's mt6366/
# mt6366_tb xo_bufs tables: XO-WCN(xo_id=1) descriptors are
#   {reg 0x788, width 3, pos 3} = 3-bit MODE field  @ CW00[5:3] (default 3)
#   {reg 0x788, width 1, pos 5} = 1-bit EN_M field  @ CW00[5]   (default 0)
# v6 accidentally treated bit0 as "EN" (0x4e05 leaves EN=0: no XO!). v7
# writes CW00 = 0x4e25 = mode SW (bits[4:3]=0) + EN_M=1 (bit5) — the
# software force-on we need since this kernel has NO clkbuf/SRCLKEN-RC
# stack (stock keeps mode=CO_BUF(3) and lets SRCLKEN-RC gate it; with no
# RC driver present only an explicit SW+EN keeps the 26M buffer alive).
#
# v5 lesson (boot 4205ba79, 2026-10-31): the v5 script exited non-zero
# ~35 ms in WITHOUT WRITING A SINGLE kmsg LINE (no "start" ever seen in
# journal), while later python heredocs in the same init script clearly
# ran. v6 therefore: (a) klogs at import before anything else, (b) wraps
# every step, (c) logs ABSOLUTE stage markers, (d) on ANY exception logs
# the full traceback line-by-line to kmsg, then returns 0 so bring-up
# continues.
#
# NO CONSYS MMIO in here (golden rule: never touch 0x18xxxxxx here).
import fcntl
import os
import struct
import sys
import time


def klog(msg):
    """Write to /dev/kmsg AND stdout. Never raises."""
    try:
        m = os.open('/dev/kmsg', os.O_WRONLY)
        os.write(m, ('<6>PMOSWIFI: xo: ' + str(msg) + '\n').encode('utf-8', 'replace'))
        os.close(m)
    except BaseException:
        pass
    try:
        print('xo: ' + str(msg), flush=True)
    except BaseException:
        pass


klog("import ok py=%d.%d.%d" % sys.version_info[:3])

PMIC_DEV = '/dev/pmicpoke'
CW00 = 0x788
CW19 = 0x7b6      # XO2 auxout reg, logged for reference only
CW_OFF = 0x4e1d   # CO_BUF(3) + EN=0  (power-on default; earlier v6 note wrong:
                  # 0x4e05 is NOT "EN=1", EN is bit5 = 0x20)
CW_ON = 0x4e25    # SW(0) + EN_M=1 (bit5) — v7; v6's 0x4e05 had EN=0

FMT = '<QQII'


def pmcmd(n):
    # _IOWR('p', n, 24): dir=3, size=24, type='p' (0x70), nr=n
    return (3 << 30) | (24 << 16) | (0x70 << 8) | n


def main():
    klog("stage1 main enter")
    if not os.path.exists(PMIC_DEV):
        klog("dev %s MISSING - skipping (module not loaded?)" % PMIC_DEV)
        return 0
    try:
        pd = os.open(PMIC_DEV, os.O_RDWR)
    except BaseException as e:
        klog("open failed %r - skipping" % e)
        return 0
    klog("stage2 dev open ok")

    buf = bytearray(struct.pack(FMT, 0, 0, 0, 0))

    def rd(reg):
        b = bytearray(struct.pack(FMT, reg & 0xffff, 0, 0, 0))
        b2 = fcntl.ioctl(pd, pmcmd(1), bytes(b))
        try:
            vals = struct.unpack(FMT, b2)
        except BaseException:
            vals = struct.unpack(FMT, bytes(b))
        return vals[1]

    def wr(reg, val):
        struct.pack_into(FMT, buf, 0, reg & 0xffff, val & 0xffff, 0, 0)
        fcntl.ioctl(pd, pmcmd(2), bytes(buf))
        return True
    klog("stage3 helpers built")

    try:
        cw = rd(CW00)
    except BaseException as e:
        klog("stage4 RD FAILED: %r" % e)
        os.close(pd)
        return 0
    try:
        cw19 = rd(CW19)
    except BaseException:
        cw19 = -1
    klog("stage4 CW00=0x%04x CW19=0x%04x" % (cw & 0xffff, cw19 & 0xffff))

    if cw == CW_ON:
        klog("stage5 XO_WCN already ON (retained through warm reset)")
        os.close(pd)
        return 0

    klog("stage5 XO is OFF (0x%04x) - enabling" % (cw & 0xffff))
    ok = False
    for attempt in (1, 2, 3):
        try:
            wr(CW00, CW_ON)
            time.sleep(0.05)
            cw2 = rd(CW00)
            klog("stage6 attempt %d wrote 0x4e25, readback 0x%04x" % (attempt, cw2 & 0xffff))
            if cw2 == CW_ON:
                ok = True
                break
        except BaseException as e:
            klog("stage6 attempt %d EXCEPTION %r" % (attempt, e))
    if not ok:
        klog("stage6 FAILED to enable XO_WCN after 3 attempts (continuing)")
    else:
        klog("stage6 XO_WCN ENABLED")

    os.close(pd)
    klog("stage7 done")
    return 0


try:
    _rc = main()
except BaseException as e:
    klog("TOP EXCEPTION: %r" % e)
    import traceback
    for line in traceback.format_exc().strip().splitlines():
        klog("tb| " + line)
    _rc = 0
klog("main rc=%s" % _rc)
raise SystemExit(_rc)
