#!/usr/bin/python3
"""Bridge the vendor GPS daemon (mnld) to NMEA consumers.

Speaks the vendor HAL socket protocol (abstract AF_UNIX datagrams
"mtk_hal2mnl" / "mtk_mnl2hal"), writes standard NMEA to a pty
(/run/tablet-gps/nmea) and to a UNIX stream socket
(/run/tablet-gps/nmea.sock) which GeoClue's network-NMEA source connects to.

The pty carries everything (diagnostics).  The GeoClue socket only carries
sentences that describe a real fix, so the receiver's 90N/0E no-fix
placeholder can never surface as a location.

When started by tb351fu-gps.socket (GNSS_SOCKET_ACTIVATED=1), the listening
socket is inherited from systemd and the bridge exits after IDLE_EXIT seconds
with no NMEA clients, so the GNSS receiver only runs while something is
actually asking for a location.
"""
import errno
import os
import re
import signal
import socket
import struct
import sys
import time
import tty

RUN = "/run/tablet-gps"
LINK = RUN + "/nmea"
SOCK = RUN + "/nmea.sock"
ACTIVATED = os.environ.get("GNSS_SOCKET_ACTIVATED") == "1"
IDLE_EXIT = 60

# Standard NMEA sentence types we forward; MTK $PMTK... diagnostics are dropped.
STANDARD = {b"GGA", b"RMC", b"GSA", b"GSV", b"VTG", b"GLL", b"ZDA",
            b"TXT", b"GBS", b"GNS"}
NMEA_RE = re.compile(rb"\$[^\r\n\x00]*")


def valid_for_geoclue(line):
    """Drop sentences carrying the receiver's no-fix placeholder (90N/0E)."""
    body = line.split(b"*")[0]
    fields = body.split(b",")
    typ = line[3:6]
    if typ == b"GGA":
        return len(fields) > 6 and fields[6] not in (b"", b"0")
    if typ == b"RMC":
        return len(fields) > 2 and fields[2] == b"A"
    if typ == b"GLL":
        return len(fields) > 6 and fields[6] == b"A"
    if typ == b"GSA":
        return len(fields) > 2 and fields[2] not in (b"", b"1")
    return typ != b"VTG"


def addr(name):
    return b"\0" + name.encode() + b"\0" * (107 - len(name))


i32 = lambda v: struct.pack("<i", v)
i64 = lambda v: struct.pack("<q", v)

rx = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
rx.bind(addr("mtk_mnl2hal"))
rx.settimeout(0.5)
tx = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)


def send(*fields):
    try:
        tx.sendto(b"".join(fields), addr("mtk_hal2mnl"))
        return True
    except OSError as e:
        if e.errno not in (errno.ECONNREFUSED, errno.ENOENT):
            print("send failed: %s" % e, file=sys.stderr, flush=True)
        return False


def start():
    if not send(i32(1), i32(101)):
        return False
    time.sleep(0.05)
    now_ms = int(time.time() * 1000)
    boot_ms = int(time.clock_gettime(time.CLOCK_BOOTTIME) * 1000)
    send(i32(1), i32(105), i64(now_ms), i64(boot_ms), i32(1000))
    send(i32(1), i32(102), struct.pack("<f", 1.4))
    print("GPS start requested", flush=True)
    return True


running = True


def stop(*_):
    global running
    running = False


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)

os.makedirs(RUN, exist_ok=True)
master, slave = os.openpty()
tty.setraw(slave)
os.chmod(os.ttyname(slave), 0o660)
try:
    import grp
    os.chown(os.ttyname(slave), 0, grp.getgrnam("uucp").gr_gid)
except (KeyError, OSError):
    pass
try:
    os.unlink(LINK)
except FileNotFoundError:
    pass
os.symlink(os.ttyname(slave), LINK)
os.set_blocking(master, False)

if ACTIVATED:
    try:
        srv = socket.socket(fileno=3)
        srv.setblocking(False)
    except OSError as e:
        print("socket activation fd 3 unavailable: %s" % e, flush=True)
        sys.exit(1)
    print("NMEA socket: inherited from systemd", flush=True)
else:
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(SOCK)
    os.chmod(SOCK, 0o666)
    srv.listen(8)
    srv.setblocking(False)
    print("NMEA socket: %s" % SOCK, flush=True)

clients = []
version_printed = False


def drop_client(c):
    try:
        clients.remove(c)
    except ValueError:
        pass
    try:
        c.close()
    except OSError:
        pass


def prune_clients():
    """Detect clients that disconnected while we had nothing to send."""
    for c in clients[:]:
        try:
            if c.recv(1, socket.MSG_PEEK) == b"":
                drop_client(c)
        except BlockingIOError:
            pass
        except OSError:
            drop_client(c)


def broadcast(line):
    for c in clients[:]:
        try:
            c.sendall(line)
        except OSError:
            drop_client(c)


start()
last_nmea = 0.0
last_start = time.time()
last_client = time.time()
fix_reported = False
while running:
    try:
        c, _ = srv.accept()
        c.setblocking(False)
        clients.append(c)
        last_client = time.time()
        print("NMEA client connected (%d)" % len(clients), flush=True)
    except BlockingIOError:
        pass
    except OSError:
        pass

    prune_clients()
    if clients:
        last_client = time.time()

    try:
        d = rx.recv(65536)
    except socket.timeout:
        d = b""
    except InterruptedError:
        continue

    if len(d) >= 8 and struct.unpack_from("<i", d, 4)[0] == 104:
        text = d[8:]
        sentences = NMEA_RE.findall(text)
        for line in sentences:
            line = line.strip()
            if len(line) < 7 or line[3:6] not in STANDARD:
                continue
            last_nmea = time.time()
            try:
                os.write(master, line + b"\r\n")
            except BlockingIOError:
                pass
            if valid_for_geoclue(line):
                broadcast(line + b"\r\n")
            if not fix_reported and line[3:6] == b"GGA":
                fields = line.split(b",")
                if len(fields) > 6 and fields[6] not in (b"", b"0"):
                    print("first fix: %s" % line.decode(errors="replace"), flush=True)
                    fix_reported = True
    elif len(d) >= 8:
        typ = struct.unpack_from("<i", d, 4)[0]
        if typ == 308 and not version_printed:
            print("mnld version reply: %r" % d[8:80], flush=True)
            version_printed = True

    try:
        os.read(master, 4096)
    except (BlockingIOError, OSError):
        pass

    now = time.time()
    if last_nmea == 0.0 and now - last_start > 2:
        if start():
            last_start = now
    elif last_nmea and now - last_nmea > 15 and now - last_start > 2:
        print("no NMEA for 15 s, restarting GPS", flush=True)
        if start():
            last_start = now

    if ACTIVATED and not clients and now - last_client > IDLE_EXIT:
        print("no NMEA clients for %d s, stopping" % IDLE_EXIT, flush=True)
        break

send(i32(1), i32(103))
print("GPS stop sent", flush=True)
for c in clients[:]:
    drop_client(c)
if not ACTIVATED:
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass
try:
    os.unlink(LINK)
except FileNotFoundError:
    pass
