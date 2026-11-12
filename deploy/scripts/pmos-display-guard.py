#!/usr/bin/env python3
"""TB351FU true backlight control (i2c-3 0x11, registers 0x04/0x05).

pmosfb's lcd-backlight device only writes DISP_PWM, which is not wired to the
visible WLED.  The stock panel driver instead maps the 0-255 brightness level
through backlight_i2c_map and writes the resulting 11-bit value to the
i2c_lcd_bias chip at 3-0011 (boe_setbacklight_cmdq: reg 0x04 = hw & 7,
reg 0x05 = hw >> 3).

This daemon:
  * mirrors /sys/class/backlight/lcd-backlight/brightness through the exact
    stock map to 3-0011;
  * writes 0/0 for a genuine panel-off on lock or idle, restoring on input;
  * still keeps DPMS On and never touches GPIO43 or the scanout.

The 0/0 blank is fully reversible: 21 s and 60 s holds were verified on
hardware with the desktop image returning intact (2026-11-07).
"""

import ctypes
import errno
import fcntl
import glob
import os
import select
import signal
import struct
import subprocess
import sys
import time

I2C_BUS = "/dev/i2c-3"
I2C_ADDR = 0x11
BACKLIGHT = "/sys/class/backlight/lcd-backlight/brightness"
STATE_DIR = "/var/lib/pmos-backlight"
STATE_BRIGHTNESS = STATE_DIR + "/brightness"
CONFIG = "/etc/pmos-backlight.conf"

SESSION_USER = "user"
SESSION_UID = 10000
SESSION_RUNTIME = "/run/user/10000"

# struct input_event on 64-bit Linux: timeval (2x long) + type/code/value.
INPUT_EVENT = struct.Struct("llHHi")
EV_SYN = 0x00
EV_KEY = 0x01
EV_MSC = 0x04
KEY_POWER = 116

DEFAULTS = {
    "IDLE_TIMEOUT": 300,   # seconds without input while unlocked; 0 disables
    "LOCK_DELAY": 3,       # seconds after lock before blanking
    "LOCK_GRACE": 30,      # seconds lit after the last input while locked
    "RESYNC": 15,          # seconds between i2c read-back checks
}

# stock backlight_i2c_map, 256 u32 entries at file offset 0x2a858 of
# panel-boe-nt36523n-vdo.ko (.rodata+0x580).
TABLE = (
    0, 400, 450, 500, 533, 566, 599, 632,
    665, 698, 731, 764, 797, 830, 862, 894,
    926, 958, 990, 1022, 1054, 1086, 1118, 1150,
    1182, 1214, 1244, 1252, 1260, 1268, 1276, 1284,
    1292, 1300, 1308, 1316, 1324, 1332, 1340, 1348,
    1356, 1364, 1372, 1380, 1388, 1396, 1404, 1412,
    1420, 1428, 1436, 1444, 1452, 1460, 1468, 1476,
    1484, 1492, 1500, 1508, 1516, 1524, 1532, 1540,
    1550, 1555, 1560, 1564, 1568, 1572, 1576, 1580,
    1584, 1588, 1592, 1596, 1600, 1604, 1608, 1612,
    1616, 1620, 1624, 1628, 1632, 1636, 1640, 1644,
    1648, 1652, 1656, 1660, 1664, 1668, 1672, 1676,
    1680, 1683, 1686, 1689, 1692, 1695, 1698, 1701,
    1704, 1707, 1710, 1713, 1716, 1719, 1722, 1725,
    1728, 1731, 1734, 1737, 1740, 1743, 1746, 1749,
    1752, 1755, 1758, 1761, 1764, 1767, 1770, 1773,
    1776, 1779, 1782, 1785, 1788, 1791, 1794, 1797,
    1800, 1803, 1806, 1809, 1812, 1815, 1818, 1821,
    1823, 1825, 1827, 1829, 1831, 1833, 1835, 1837,
    1839, 1841, 1843, 1845, 1847, 1849, 1851, 1853,
    1855, 1857, 1859, 1861, 1863, 1865, 1867, 1869,
    1871, 1873, 1875, 1877, 1879, 1881, 1883, 1885,
    1887, 1889, 1891, 1893, 1895, 1897, 1899, 1901,
    1903, 1905, 1907, 1909, 1911, 1913, 1915, 1918,
    1921, 1924, 1926, 1928, 1930, 1932, 1934, 1936,
    1938, 1940, 1942, 1944, 1946, 1948, 1950, 1952,
    1954, 1956, 1958, 1960, 1962, 1964, 1966, 1968,
    1970, 1972, 1974, 1976, 1978, 1980, 1982, 1984,
    1986, 1988, 1990, 1992, 1994, 1996, 1998, 2000,
    2002, 2004, 2006, 2008, 2010, 2012, 2014, 2016,
    2018, 2020, 2022, 2024, 2026, 2028, 2030, 2032,
    2034, 2036, 2038, 2040, 2042, 2044, 2046, 2047
)

I2C_RDWR = 0x0707
I2C_M_RD = 0x0001


def log(message):
    print(message, flush=True)


class I2CMsg(ctypes.Structure):
    _fields_ = [
        ("addr", ctypes.c_uint16),
        ("flags", ctypes.c_uint16),
        ("len", ctypes.c_uint16),
        ("buf", ctypes.POINTER(ctypes.c_uint8)),
    ]


class I2CRdwrData(ctypes.Structure):
    _fields_ = [
        ("msgs", ctypes.POINTER(I2CMsg)),
        ("nmsgs", ctypes.c_uint32),
    ]


def load_config():
    cfg = dict(DEFAULTS)
    try:
        with open(CONFIG, "r", encoding="ascii") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if not line or "=" not in line:
                    continue
                key, value = line.split("=", 1)
                key = key.strip()
                if key in cfg:
                    try:
                        cfg[key] = int(value.strip(), 0)
                    except ValueError:
                        pass
    except OSError:
        pass
    return cfg


def read_brightness():
    try:
        with open(BACKLIGHT, "r", encoding="ascii") as f:
            return max(0, min(255, int(f.read().strip())))
    except (OSError, ValueError):
        return None


def write_brightness(value):
    try:
        with open(BACKLIGHT, "w", encoding="ascii") as f:
            f.write("%d\n" % value)
        return True
    except OSError:
        return False


def load_saved():
    try:
        with open(STATE_BRIGHTNESS, "r", encoding="ascii") as f:
            return max(0, min(255, int(f.read().strip())))
    except (OSError, ValueError):
        return None


def save_brightness(value):
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        tmp = STATE_BRIGHTNESS + ".tmp"
        with open(tmp, "w", encoding="ascii") as f:
            f.write("%d\n" % value)
        os.replace(tmp, STATE_BRIGHTNESS)
    except OSError as exc:
        log("could not save brightness: %s" % exc)


def closest_index(hw):
    """Index in the monotonic stock table nearest to an 11-bit value."""
    lo, hi = 0, len(TABLE) - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if TABLE[mid] < hw:
            lo = mid + 1
        else:
            hi = mid
    if lo > 0 and abs(TABLE[lo - 1] - hw) <= abs(TABLE[lo] - hw):
        return lo - 1
    return lo


class Chip:
    def __init__(self):
        self.fd = None

    def open(self):
        if self.fd is None:
            self.fd = os.open(I2C_BUS, os.O_RDWR)
        return self.fd

    def close(self):
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None

    def read_reg(self, reg):
        fd = self.open()
        wbuf = (ctypes.c_uint8 * 1)(reg)
        rbuf = (ctypes.c_uint8 * 1)()
        msgs = (I2CMsg * 2)(
            I2CMsg(I2C_ADDR, 0, 1, wbuf),
            I2CMsg(I2C_ADDR, I2C_M_RD, 1, rbuf),
        )
        arg = bytearray(bytes(I2CRdwrData(msgs, 2)))
        fcntl.ioctl(fd, I2C_RDWR, arg, True)
        return rbuf[0]

    def write_reg(self, reg, value):
        fd = self.open()
        buf = (ctypes.c_uint8 * 2)(reg, value)
        msgs = (I2CMsg * 1)(I2CMsg(I2C_ADDR, 0, 2, buf))
        arg = bytearray(bytes(I2CRdwrData(msgs, 1)))
        fcntl.ioctl(fd, I2C_RDWR, arg, True)

    def read_hw(self):
        low = self.read_reg(0x04)
        high = self.read_reg(0x05)
        return ((high & 0xFF) << 3) | (low & 0x07)

    def write_hw(self, hw):
        # same order as the stock boe_setbacklight_cmdq
        self.write_reg(0x04, hw & 0x07)
        self.write_reg(0x05, (hw >> 3) & 0xFF)


class ScreenSaver:
    """Tracks org.gnome.ScreenSaver.ActiveChanged via a persistent monitor."""

    def __init__(self):
        self.proc = None
        self.buf = b""
        self.locked = False
        self.wake_requested = False
        self.restart_at = 0.0

    def _gdbus(self, *args, timeout=5):
        return subprocess.run(
            ["runuser", "-u", SESSION_USER, "--", "env",
             "XDG_RUNTIME_DIR=" + SESSION_RUNTIME, *args],
            capture_output=True, text=True, timeout=timeout)

    def query(self):
        try:
            result = self._gdbus(
                "gdbus", "call", "--session",
                "--dest", "org.gnome.ScreenSaver",
                "--object-path", "/org/gnome/ScreenSaver",
                "--method", "org.gnome.ScreenSaver.GetActive")
            if "(true" in result.stdout:
                return True
            if "(false" in result.stdout:
                return False
        except Exception:
            pass
        return None

    def start(self, now):
        if self.proc is not None and self.proc.poll() is None:
            return
        if not os.path.exists(SESSION_RUNTIME + "/bus"):
            self.restart_at = now + 2
            return
        try:
            self.proc = subprocess.Popen(
                ["runuser", "-u", SESSION_USER, "--", "env",
                 "XDG_RUNTIME_DIR=" + SESSION_RUNTIME,
                 "gdbus", "monitor", "--session",
                 "--dest", "org.gnome.ScreenSaver",
                 "--object-path", "/org/gnome/ScreenSaver"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.buf = b""
            state = self.query()
            if state is not None:
                self.locked = state
            log("screensaver monitor started (locked=%s)" % self.locked)
        except Exception as exc:
            self.proc = None
            self.restart_at = now + 5
            log("screensaver monitor failed to start: %s" % exc)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def readable(self):
        return self.alive() and self.proc.stdout is not None

    def fileno(self):
        return self.proc.stdout.fileno()

    def feed(self):
        """Read available output, return list of observed lock states."""
        changes = []
        try:
            chunk = os.read(self.fileno(), 4096)
        except OSError:
            chunk = b""
        if not chunk:
            try:
                self.proc.wait(timeout=1)
            except Exception:
                pass
            self.proc = None
            return changes
        self.buf += chunk
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace")
            if "WakeUpScreen" in text:
                # Phosh only emits a signal now that the output-power shim
                # keeps its screensaver state inactive (notifications,
                # lockscreen wakeup).  Light the panel for LOCK_GRACE.
                self.wake_requested = True
            if "ActiveChanged" in text:
                if "true" in text:
                    self.locked = True
                    changes.append(True)
                elif "false" in text:
                    self.locked = False
                    changes.append(False)
        return changes


class LogindLock:
    """Tracks the graphical session's logind LockedHint property.

    phosh normally mirrors its screensaver state over org.gnome.ScreenSaver,
    but the output-power shim prevents the compositor-side blank, so phosh's
    own active flag stays FALSE on lock and ActiveChanged never fires.
    logind's LockedHint is then the authoritative lock source and also covers
    `loginctl lock-session`.
    """

    POLL_INTERVAL = 1.0
    RETRY_INTERVAL = 5.0

    def __init__(self):
        self.session = None
        self.locked = None
        self.next_poll = 0.0

    def _discover(self):
        try:
            result = subprocess.run(
                ["loginctl", "list-sessions", "--no-legend"],
                capture_output=True, text=True, timeout=3)
        except Exception:
            return None
        candidates = []
        for line in result.stdout.splitlines():
            fields = line.split()
            if len(fields) < 3:
                continue
            session, uid, user = fields[0], fields[1], fields[2]
            if uid == str(SESSION_UID) and user == SESSION_USER:
                candidates.append(session)
        # runuser creates a `manager` session and SSH creates `tty` sessions;
        # only the phosh session is Type=wayland and carries the LockedHint.
        for session in candidates:
            try:
                info = subprocess.run(
                    ["loginctl", "show-session", session,
                     "-p", "Type", "--value"],
                    capture_output=True, text=True, timeout=3)
            except Exception:
                continue
            if info.returncode == 0 and info.stdout.strip() == "wayland":
                return session
        return None

    def poll(self, now, force=False):
        if not force and now < self.next_poll:
            return self.locked
        self.next_poll = now + self.POLL_INTERVAL
        if self.session is None:
            self.session = self._discover()
            if self.session is None:
                self.next_poll = now + self.RETRY_INTERVAL
                return self.locked
            log("logind session: %s" % self.session)
        try:
            result = subprocess.run(
                ["loginctl", "show-session", self.session,
                 "-p", "LockedHint", "--value"],
                capture_output=True, text=True, timeout=3)
        except Exception:
            self.session = None
            return self.locked
        if result.returncode != 0:
            self.session = None
            return self.locked
        value = result.stdout.strip()
        if value == "yes":
            self.locked = True
        elif value == "no":
            self.locked = False
        return self.locked


def idle_inhibited():
    """True when a session app holds an idle inhibitor (video playback etc.)."""
    try:
        result = subprocess.run(
            ["runuser", "-u", SESSION_USER, "--", "env",
             "XDG_RUNTIME_DIR=" + SESSION_RUNTIME,
             "gdbus", "call", "--session",
             "--dest", "org.gnome.SessionManager",
             "--object-path", "/org/gnome/SessionManager",
             "--method", "org.gnome.SessionManager.IsInhibited", "8"],
            capture_output=True, text=True, timeout=3)
        return "(true" in result.stdout
    except Exception:
        return False


def open_inputs(existing):
    """Add newly appeared input event devices; return closed paths."""
    for path in sorted(glob.glob("/dev/input/event*")):
        if path in existing:
            continue
        try:
            existing[path] = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            log("watching %s" % path)
        except OSError:
            pass


def prune_inputs(existing):
    for path, fd in list(existing.items()):
        try:
            os.fstat(fd)
        except OSError:
            try:
                os.close(fd)
            except OSError:
                pass
            del existing[path]
            log("stopped watching %s" % path)


def main():
    cfg = load_config()
    idle_timeout = cfg["IDLE_TIMEOUT"]
    lock_delay = cfg["LOCK_DELAY"]
    lock_grace = cfg["LOCK_GRACE"]
    resync = cfg["RESYNC"]

    os.makedirs(STATE_DIR, exist_ok=True)

    deadline = time.monotonic() + 30
    while read_brightness() is None and time.monotonic() < deadline:
        time.sleep(0.5)

    chip = Chip()
    brightness = None
    saved = load_saved()
    hw_now = None
    try:
        hw_now = chip.read_hw()
    except OSError:
        chip.close()

    if saved is not None:
        brightness = saved
    elif hw_now is not None and hw_now > 0:
        brightness = closest_index(hw_now)
        log("aligned brightness to hardware level %d -> index %d" % (hw_now, brightness))
    else:
        current = read_brightness()
        brightness = current if current else 62

    write_brightness(brightness)
    save_brightness(brightness)
    log("starting: brightness=%d hw=%s" % (
        brightness, "unknown" if hw_now is None else hw_now))

    def on_term(_signum, _frame):
        try:
            chip.write_hw(TABLE[brightness] if brightness else 0)
        except OSError:
            pass
        log("terminating: backlight restored")
        sys.exit(0)

    signal.signal(signal.SIGTERM, on_term)
    signal.signal(signal.SIGINT, on_term)

    ss = ScreenSaver()
    ss.start(time.monotonic())
    lk = LogindLock()
    lk.poll(time.monotonic(), force=True)
    locked = bool(ss.locked or lk.locked)
    start = time.monotonic()
    locked_since = (start - lock_delay) if locked else None

    inputs = {}
    open_inputs(inputs)
    # Starting while already locked must not relight the panel: treat the
    # lock and the last activity as old so the first loop blanks immediately.
    last_activity = (start - lock_grace - lock_delay) if locked else start
    power_blank = False

    def apply_lock(state):
        nonlocal locked, locked_since, last_activity, power_blank
        if state is None or state == locked:
            return
        locked = state
        if locked:
            locked_since = time.monotonic()
            log("session locked")
        else:
            power_blank = False
            last_activity = time.monotonic()
            log("session unlocked")

    next_input_scan = last_activity + 5
    next_inhibit_check = 0.0
    idle_blocked = False
    applied = None
    last_resync = 0.0
    last_error_log = 0.0
    next_ss_retry = 0.0

    while True:
        now = time.monotonic()

        if ss.proc is not None and ss.proc.poll() is not None:
            try:
                ss.proc.wait(timeout=0)
            except Exception:
                pass
            ss.proc = None
            ss.restart_at = now + 2
            log("screensaver monitor exited, restarting")

        if ss.proc is None and now >= max(ss.restart_at, next_ss_retry):
            ss.start(now)
            next_ss_retry = now + 5
            apply_lock(bool(ss.locked or lk.locked))

        if now >= next_input_scan:
            open_inputs(inputs)
            prune_inputs(inputs)
            next_input_scan = now + 5

        ready = []
        rlist = list(inputs.values())
        if ss.readable():
            rlist.append(ss.fileno())
        if rlist:
            try:
                ready, _, _ = select.select(rlist, [], [], 0.25)
            except OSError:
                ready = []
        else:
            time.sleep(0.25)

        dead_fds = []
        for fd in ready:
            if ss.proc is not None and ss.readable() and fd == ss.fileno():
                ss.feed()
                if ss.wake_requested:
                    ss.wake_requested = False
                    last_activity = now
                    log("wakeup request (notification)")
                apply_lock(bool(ss.locked or lk.locked))
            else:
                try:
                    data = os.read(fd, 4096)
                except OSError as exc:
                    data = b""
                    if exc.errno in (errno.ENODEV, errno.EIO):
                        dead_fds.append(fd)
                if data:
                    power_press = False
                    other_activity = False
                    for offset in range(0, len(data) - INPUT_EVENT.size + 1,
                                        INPUT_EVENT.size):
                        _sec, _usec, etype, code, value = \
                            INPUT_EVENT.unpack_from(data, offset)
                        if etype == EV_KEY and code == KEY_POWER:
                            if value == 1:
                                power_press = True
                        elif etype not in (EV_SYN, EV_MSC):
                            other_activity = True
                    if power_press:
                        if applied:
                            # Lit -> a single power press is an explicit
                            # screen-off request: blank now, do not wait for
                            # the lock grace or phosh's (shimmed) DPMS Off.
                            power_blank = True
                            log("power key: display off")
                        else:
                            # Dark -> the press wakes the panel; the wakeup
                            # itself must not be blanked again.
                            power_blank = False
                            last_activity = now
                    if other_activity:
                        power_blank = False
                        last_activity = now

        if dead_fds:
            for path, fd in list(inputs.items()):
                if fd in dead_fds:
                    try:
                        os.close(fd)
                    except OSError:
                        pass
                    del inputs[path]
                    log("input device lost: %s" % path)

        lk.poll(now)
        apply_lock(bool(ss.locked or lk.locked))

        current = read_brightness()
        if current is not None and current != brightness:
            log("brightness %d -> %d" % (brightness, current))
            brightness = current
            save_brightness(current)

        if power_blank:
            want_on = False
            reason = "power key"
        elif locked:
            if locked_since is None:
                locked_since = now
            deadline = max(locked_since + lock_delay, last_activity + lock_grace)
            want_on = now < deadline
            reason = "locked"
        else:
            reason = "idle"
            if idle_timeout > 0:
                want_on = (now - last_activity) < idle_timeout
                if not want_on and applied != 0:
                    # Only consult inhibitors while the panel is still lit;
                    # once blanked, physical input is the wake source.
                    if now >= next_inhibit_check:
                        next_inhibit_check = now + 30
                        idle_blocked = idle_inhibited()
                    if idle_blocked:
                        want_on = True
            else:
                want_on = True

        want = TABLE[brightness] if (want_on and brightness > 0) else 0

        try:
            if applied is None or want != applied:
                chip.write_hw(want)
                if applied != want:
                    if want:
                        log("panel on: brightness=%d hw=%d" % (brightness, want))
                    else:
                        log("panel off (%s), brightness saved %d" % (reason, brightness))
                applied = want
            elif now - last_resync >= resync:
                last_resync = now
                actual = chip.read_hw()
                if actual != want:
                    log("resync: chip %d != %d" % (actual, want))
                    chip.write_hw(want)
                    applied = want
        except OSError as exc:
            chip.close()
            applied = None
            if now - last_error_log > 60:
                log("i2c error: %s" % exc)
                last_error_log = now
            time.sleep(0.5)


if __name__ == "__main__":
    main()
