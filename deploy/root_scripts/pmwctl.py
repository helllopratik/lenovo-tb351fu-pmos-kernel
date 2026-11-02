#!/usr/bin/env python3
# pmwctl.py — PMIC reg access via /dev/pmicpoke (pwrap regmap)
# usage:
#   pmwctl.py rd <reg>            read 16-bit PMIC reg
#   pmwctl.py wr <reg> <val>      write 16-bit PMIC reg
#   pmwctl.py set|clr <reg> <msk> set/clear bits
import os, struct, sys, fcntl

DEV = '/dev/pmicpoke'
MAGIC = ord('p')
# ioctl op numbers (verified by disassembly of mt6789_pmicpoke.ko):
#   cmd & 0xffff -> 0x7001 RD, 0x7002 WR, 0x7003 SET, 0x7004 CLR
RD, WR, SET, CLR = 1, 2, 3, 4
# _IOWR('p', n, struct poke_arg)  -> (3<<30)|(24<<16)|('p'<<8)|n
def cmd(n):
    return (3 << 30) | (24 << 16) | (MAGIC << 8) | n

FMT = '<QQII'
SIZE = struct.calcsize(FMT)

def op(field, reg, val=0, mask=0):
    fd = os.open(DEV, os.O_RDWR)
    buf = struct.pack(FMT, reg & 0xffff, val & 0xffff, mask, 0)
    buf = fcntl.ioctl(fd, cmd(field), buf)
    r, v, m, c = struct.unpack(FMT, buf)
    os.close(fd)
    return v

def main():
    if len(sys.argv) < 2:
        print(__doc__); return 1
    act = sys.argv[1].lower()
    if act == 'rd':
        r = int(sys.argv[2], 0)
        print(f"0x{r:04x}: 0x{op(RD, r):04x}")
    elif act == 'wr':
        r, v = int(sys.argv[2], 0), int(sys.argv[3], 0)
        op(WR, r, v)
        print(f"0x{r:04x} <- 0x{v:04x}; readback 0x{op(RD, r):04x}")
    elif act in ('set', 'clr'):
        r, m = int(sys.argv[2], 0), int(sys.argv[3], 0)
        fld = SET if act == 'set' else CLR
        old = op(RD, r)
        op(fld, r, 0, m)
        new = op(RD, r)
        print(f"0x{r:04x}: 0x{old:04x} -> 0x{new:04x}")
    else:
        print(f"unknown action {act}"); return 2
    return 0

sys.exit(main())
