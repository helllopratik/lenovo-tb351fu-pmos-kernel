#!/usr/bin/env python3
"""Panel refresh mode for pmosfb: 60 Hz, 90 Hz, or adaptive.

The mode is stored in the per-user GSettings key
org.postmarketos.Display refresh-mode (set from Mobile Settings -> Display).
This daemon runs as root, applies the mode to
/sys/module/pmosfb/parameters/refresh_hz, and implements adaptive mode.

Adaptive mode samples the live pmosfb scanout buffer through /dev/mem and
watches the touch/pen input nodes.  The screen is considered active while
it is changing or while input is arriving; 90 Hz is selected then, and 60 Hz
after STATIC_TIMEOUT seconds of a static screen with no input.  After a
stylus event 90 Hz is held for PEN_GRACE seconds so that pauses while
drawing do not silently drop the pen back to finger emulation.

Why /dev/mem: pmosfb has no fbdev node and no frame counter.  Its scanout
buffer is a reserved System RAM carveout (the same buffer the driver blits
into), so a sparse sample of it is a cheap and accurate content-change
detector.  If the carveout cannot be mapped the daemon stays at 90 Hz,
which is the stylus-safe fallback.
"""

import glob
import mmap
import os
import select
import struct
import subprocess
import sys
import time

PARAM = "/sys/module/pmosfb/parameters/refresh_hz"
SCHEMA = "org.postmarketos.Display"
KEY = "refresh-mode"
SESSION_USER = "user"
DCONF = "/home/user/.config/dconf/user"

# pmosfb scanout geometry (XR24, 1200x2000).  The compositor FB uses a
# 4800-byte pitch, but the scanout carveout is blitted with the bootloader
# stride, which is 4864 bytes (64 bytes of row padding).
FB_W = 1200
FB_H = 2000
FB_STRIDE = 4864
STEP_X = 16
STEP_Y = 16

# A single pixel changing is noise (cursor, clock); a real animation changes
# thousands of the sparse samples.  A small spinner can be missed, which is
# fine: it is not worth doubling the render rate for.
ACTIVITY_DIFF = 16
STATIC_TIMEOUT = 10.0
PEN_GRACE = 300.0
POLL = 0.1
FORCE_RELOAD = 30.0

INPUT_EVENT = struct.Struct("llHHi")

MODES = ("60", "90", "adaptive")


def log(msg):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), msg, flush=True)


def find_fb_range():
    """Return (physical address, size) of the pmosfb scanout carveout."""
    try:
        with open("/proc/iomem") as f:
            for line in f:
                if "pmos-framebuffer" not in line:
                    continue
                rng = line.split()[0]
                start, end = rng.split("-")
                return int(start, 16), int(end, 16) - int(start, 16) + 1
    except (OSError, ValueError, IndexError):
        pass
    return None, None


def open_fb():
    addr, size = find_fb_range()
    need = FB_STRIDE * FB_H
    if addr is None or size < need:
        log("scanout carveout not found (addr=%s size=%s), adaptive will stay at 90 Hz"
            % (addr, size))
        return None, None, None
    try:
        fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
        mm = mmap.mmap(fd, need, mmap.MAP_SHARED, mmap.PROT_READ, offset=addr)
    except OSError as exc:
        log("cannot map scanout at 0x%x: %s; adaptive will stay at 90 Hz"
            % (addr, exc))
        return None, None, None
    log("scanout 0x%x size %d, sampling every %dx%d px"
        % (addr, need, STEP_X, STEP_Y))
    return fd, mm, addr


def sample(mm):
    out = bytearray()
    for y in range(0, FB_H, STEP_Y):
        base = y * FB_STRIDE
        out += mm[base:base + FB_W * 4:STEP_X * 4]
    return bytes(out)


def changed_samples(a, b):
    if a == b:
        return 0
    return sum(1 for x, y in zip(a, b) if x != y)


def open_inputs():
    """Open the touchscreen and pen event nodes.  Any event is activity."""
    devs = {}
    for p in glob.glob("/sys/class/input/event*/device/name"):
        try:
            with open(p) as f:
                name = f.read().strip()
        except OSError:
            continue
        if not (name == "NVTCapacitiveTouchScreen"
                or name == "NVTCapacitivePen"
                or "Lenovo Tab Pen Plus" in name):
            continue
        path = "/dev/input/" + p.split("/")[4]
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        except OSError as exc:
            log("cannot open %s (%s): %s" % (path, name, exc))
            continue
        kind = "pen" if ("Pen" in name or "pen" in name) else "touch"
        devs[fd] = (kind, name)
        log("input %s: %s" % (kind, name))
    return devs


def drain(fd):
    """Read pending events; return byte count, or None if the node died."""
    total = 0
    while True:
        try:
            data = os.read(fd, INPUT_EVENT.size * 64)
        except BlockingIOError:
            return total
        except OSError:
            return None
        if not data:
            return None
        total += len(data)


def read_mode():
    try:
        res = subprocess.run(
            ["runuser", "-u", SESSION_USER, "--", "gsettings", "get",
             SCHEMA, KEY],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, timeout=3)
        if res.returncode == 0:
            mode = res.stdout.strip().strip("'\"")
            if mode in MODES:
                return mode
    except (OSError, subprocess.SubprocessError):
        pass
    return "90"


def dconf_mtime():
    try:
        return os.stat(DCONF).st_mtime_ns
    except OSError:
        return None


def write_hz(hz):
    try:
        with open(PARAM) as f:
            cur = f.read().strip()
        if cur != str(hz):
            with open(PARAM, "w") as f:
                f.write(str(hz))
        return True
    except OSError as exc:
        log("cannot write %s=%s: %s" % (PARAM, hz, exc))
        return False


def main():
    mode = read_mode()
    log("starting, mode=%s" % mode)

    fb_fd, fb_mm, fb_addr = open_fb()
    inputs = open_inputs()
    if not inputs:
        log("no touch/pen input nodes; adaptive will rely on the framebuffer")

    last_activity = time.monotonic()
    # None means no stylus event has been seen yet.  Using 0.0 here would
    # apply PEN_GRACE from boot, holding 90 Hz for the first five minutes
    # of uptime even though the screen is static.
    last_pen = None
    current_hz = None
    last_mtime_check = 0.0
    last_force_reload = 0.0
    last_mtime = dconf_mtime()
    prev = sample(fb_mm) if fb_mm else None

    while True:
        now = time.monotonic()

        # Re-read the per-user setting when dconf changes (or periodically in
        # case the file appeared after login).
        if now - last_mtime_check >= 1.0:
            last_mtime_check = now
            mtime = dconf_mtime()
            if (mtime != last_mtime
                    or now - last_force_reload >= FORCE_RELOAD):
                last_mtime = mtime
                last_force_reload = now
                new_mode = read_mode()
                if new_mode != mode:
                    mode = new_mode
                    last_activity = now
                    log("mode=%s" % mode)

        if inputs:
            try:
                ready, _, _ = select.select(list(inputs), [], [], POLL)
            except (OSError, ValueError):
                ready = []
                # A node disappeared; rebuild the set.
                for fd in list(inputs):
                    try:
                        os.close(fd)
                    except OSError:
                        pass
                inputs = open_inputs()
            dead = False
            for fd in ready:
                kind = inputs.get(fd, ("touch", "?"))[0]
                count = drain(fd)
                if count is None:
                    # A disconnected node reports EOF/ENODEV and select
                    # keeps marking it readable.  Drop it instead of
                    # treating that as endless input activity.
                    dead = True
                    inputs.pop(fd, None)
                    try:
                        os.close(fd)
                    except OSError:
                        pass
                    continue
                if count:
                    last_activity = now
                    if kind == "pen":
                        last_pen = now
            if dead:
                log("input node disappeared; rebuilding input watch")
                time.sleep(1.0)
                for fd in list(inputs):
                    try:
                        os.close(fd)
                    except OSError:
                        pass
                inputs = open_inputs()
        else:
            time.sleep(POLL)

        if mode == "adaptive" and fb_mm:
            cur = sample(fb_mm)
            if cur != prev:
                diff = changed_samples(prev, cur)
                if diff >= ACTIVITY_DIFF:
                    last_activity = now
            prev = cur

        if mode == "60":
            target = 60
        elif mode == "90":
            target = 90
        elif fb_mm is None:
            target = 90
        elif ((now - last_activity) < STATIC_TIMEOUT
              or (last_pen is not None
                  and (now - last_pen) < PEN_GRACE)):
            target = 90
        else:
            target = 60

        if target != current_hz:
            if write_hz(target):
                current_hz = target
                log("refresh_hz=%d (mode=%s)" % (target, mode))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
