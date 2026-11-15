#!/usr/bin/python3
"""Probe the vendor mnld HAL socket protocol: send init+start, log replies.

hal2mnl messages (from gps.default.so): little-endian int32 version (1),
int32 command, arguments. 101 init, 102 start (+float 1.4), 103 stop.
Both sockets are abstract AF_UNIX datagram names padded to 108 bytes.
"""
import socket, struct, sys, time, collections

def addr(name):
    return b"\0" + name.encode() + b"\0" * (107 - len(name))

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
rx = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
rx.bind(addr("mtk_mnl2hal"))
rx.settimeout(0.5)
tx = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)

def send(*fields):
    tx.sendto(b"".join(fields), addr("mtk_hal2mnl"))

i32 = lambda v: struct.pack("<i", v)
send(i32(1), i32(101)); time.sleep(0.5)
send(i32(1), i32(102), struct.pack("<f", 1.4))
seen = collections.Counter(); end = time.time() + secs
while time.time() < end:
    try:
        d = rx.recv(65536)
    except socket.timeout:
        continue
    if len(d) < 8:
        continue
    ver, typ = struct.unpack_from("<ii", d)
    seen[typ] += 1
    text = d[8:]
    k = text.find(b"$G")
    if k >= 0:
        print("type %d NMEA: %s" % (typ, text[k:].split(b"\0")[0].decode(errors="replace").strip()), flush=True)
    elif seen[typ] <= 2:
        print("type %d len %d head %s" % (typ, len(d), d[8:40].hex()), flush=True)
send(i32(1), i32(103))
print("message counts by type:", dict(seen))
