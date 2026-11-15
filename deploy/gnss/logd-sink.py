#!/usr/bin/python3
"""Minimal Android logd writer sink: receive liblog datagrams on a unix socket, print text lines."""
import socket, struct, sys, os, time
path = sys.argv[1]
try: os.unlink(path)
except FileNotFoundError: pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); s.bind(path); os.chmod(path, 0o666)
P = "??VDIWEF"
out = open(sys.argv[2], "a", buffering=1) if len(sys.argv) > 2 else sys.stdout
while True:
    d = s.recv(65536)
    if len(d) < 13 or d[0] == 2: continue          # binary events (security/stats) skipped
    body = d[11:]; prio = body[0]; rest = body[1:]
    tag, _, msg = rest.partition(b"\0")
    msg = msg.rstrip(b"\0").decode(errors="replace")
    out.write("%s %s/%s: %s\n" % (time.strftime("%H:%M:%S"), P[prio] if prio < 8 else prio, tag.decode(errors="replace"), msg))
