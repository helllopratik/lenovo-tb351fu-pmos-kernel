#!/usr/bin/env python3
# Issue 41 bridge: /dev/stpbt (MTK BT HCI transport) <-> PTY master
#
# Why a pty: hci_uart (kernel N_HCI_UART ldisc) only binds to TTYs, and the
# MTK stpbt driver is a plain chardev. btattach opens the PTY SLAVE, forces
# the ldisc, and the kernel hci_uart layer talks to our pump through the
# pty master side. Stock-equivalent: the Android HAL was the hci host with
# direct fd access; here the pump + btattach do the same job.
#
# Lifecycle:
#   open /dev/stpbt          -> triggers [MTK-BT]BT_open -> WMT OPID(3) BT-on
#                               + firmware + STP ready (whole trigger)
#   stay open forever        -> like the wmtshim fd holder
#   select() pump both ways  -> bytes are plain H4 both sides (module decode)
#   SIGTERM = cleanup close  -> module auto BT func_off + STP release
import os, sys, select, signal

DEV = "/dev/stpbt"
LOG = "/tmp/btbridge.log"
POLL = 1000  # ms, plenty fast for HCI


def log(msg):
    with open(LOG, "a") as f:
        f.write("%s %s\n" % (time_str(), msg))


def time_str():
    import time
    return time.strftime("%H:%M:%S")


fd = None
stop = False


def on_term(signum, frame):
    global stop
    stop = True


def main():
    global fd
    signal.signal(signal.SIGTERM, on_term)
    signal.signal(signal.SIGINT, on_term)

    # PTY pair first so btattach can find it
    m, s = os.openpty()
    slave_path = os.ttyname(s)
    # keep slave open in THIS process too (so kernel keeps master->slave pipe)
    # but btattach will open it again for ldisc attach; keeping ours adds ref
    log("bridge: pty master=%d slave=%s" % (m, slave_path))
    print("BTBRIDGE: pty slave = %s" % slave_path, flush=True)

    fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY)
    log("bridge: stpbt fd=%d opened (BT-on trigger sent)")
    print("BTBRIDGE: " + DEV + " opened fd=%d" % fd, flush=True)

    total_rx = 0
    total_tx = 0
    while not stop:
        r, _, _ = select.select([m, fd], [], [], POLL / 1000.0)
        for sr in r:
            if sr == fd:
                try:
                    d = os.read(fd, 512)
                except OSError:
                    d = b""
                if d:
                    total_rx += len(d)
                    b = d
                    while b:
                        n = os.write(m, b)
                        b = b[n:]
            elif sr == m:
                try:
                    d = os.read(m, 512)
                except OSError:
                    d = b""
                if d:
                    total_tx += len(d)
                    b = d
                    while b:
                        n = os.write(fd, b)
                        b = b[n:]
        # heartbeat every ~30 s
        if int(total_rx + total_tx) != -1:
            continue

    log("bridge: closing (rx=%d tx=%d)" % (total_rx, total_tx))
    os.close(fd)
    os.close(m)
    os.close(s)


if __name__ == "__main__":
    main()
