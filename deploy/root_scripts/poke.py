#!/usr/bin/env python3
# poke.py rd|wr <hexaddr> [hexval] — generic MMIO via /dev/spmpoke
# Used by hbguard.sh and pmos-wifi-supervisor.sh for TopRGU accesses.
import fcntl, os, struct, sys

RD, WR = 0xC0185301, 0xC0185302
fd = os.open('/dev/spmpoke', os.O_RDONLY)

def rd(a):
    b = bytearray(struct.pack('QQII', a, 0, 0, 0))
    fcntl.ioctl(fd, RD, b)
    return struct.unpack('QQII', bytes(b))[1]

def wr(a, v):
    fcntl.ioctl(fd, WR, struct.pack('QQII', a, v, 0, 0))
    return rd(a)

act = sys.argv[1].lower()
a = int(sys.argv[2], 16)
v = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0
if act == 'rd':
    print('0x%08x = 0x%08x' % (a, rd(a)))
elif act == 'wr':
    print('0x%08x <- 0x%08x (rb 0x%08x)' % (a, v, wr(a, v)))
else:
    print('usage: poke.py rd|wr <addr> [val]'); sys.exit(2)
