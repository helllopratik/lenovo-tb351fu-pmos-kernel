#!/usr/bin/env python3
"""Apply the user's zram swap choice from Mobile Settings.

The boolean key org.postmarketos.Zram enabled is exposed in Mobile
Settings -> Memory.  This root daemon maps it to
postmarketos-zram-swap.service: true starts the service (zramstart),
false stops it (zramstop).

The service is left disabled at boot so this daemon is the single source
of truth for the user's choice.  It waits for the user session bus before
the first read, so a saved "off" is honored before any swap is created.
"""

import os
import subprocess
import sys
import time

SCHEMA = "org.postmarketos.Zram"
KEY = "enabled"
SERVICE = "postmarketos-zram-swap.service"
SESSION_USER = "user"
RUNTIME_DIR = "/run/user/10000"
BUS = RUNTIME_DIR + "/bus"
DCONF = "/home/user/.config/dconf/user"
POLL = 1.0
FORCE_RELOAD = 30.0


def log(msg):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), msg, flush=True)


def read_enabled():
    """Return True/False, or None if the setting cannot be read yet."""
    try:
        res = subprocess.run(
            ["runuser", "-u", SESSION_USER, "--", "env",
             "XDG_RUNTIME_DIR=" + RUNTIME_DIR,
             "DBUS_SESSION_BUS_ADDRESS=unix:path=" + BUS,
             "gsettings", "get", SCHEMA, KEY],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, timeout=5)
        if res.returncode == 0:
            value = res.stdout.strip()
            if value in ("true", "false"):
                return value == "true"
    except (OSError, subprocess.SubprocessError):
        pass
    return None


def service_active():
    try:
        res = subprocess.run(["systemctl", "is-active", "--quiet", SERVICE],
                             timeout=10)
        return res.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def apply(enabled):
    action = "start" if enabled else "stop"
    try:
        res = subprocess.run(["systemctl", action, SERVICE],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, timeout=60)
        if res.returncode == 0:
            log("%s %s ok" % (action, SERVICE))
        else:
            log("%s %s failed: %s"
                % (action, SERVICE, res.stdout.strip()))
    except (OSError, subprocess.SubprocessError) as exc:
        log("%s %s failed: %s" % (action, SERVICE, exc))


def dconf_mtime():
    try:
        return os.stat(DCONF).st_mtime_ns
    except OSError:
        return None


def wait_for_session():
    """Do not create swap before the user's saved choice is readable."""
    while not os.path.exists(BUS):
        time.sleep(2)


def main():
    log("starting")
    wait_for_session()

    enabled = read_enabled()
    for _ in range(10):
        if enabled is not None:
            break
        time.sleep(1)
        enabled = read_enabled()
    if enabled is None:
        # The schema should always exist; if it does not, follow the
        # upstream postmarketOS default (zram swap enabled).
        log("cannot read %s %s; defaulting to enabled" % (SCHEMA, KEY))
        enabled = True

    if service_active() != enabled:
        apply(enabled)
    log("enabled=%s" % enabled)

    last_mtime = dconf_mtime()
    last_check = time.monotonic()

    while True:
        now = time.monotonic()
        mtime = dconf_mtime()
        if mtime != last_mtime or now - last_check >= FORCE_RELOAD:
            last_mtime = mtime
            last_check = now
            new_value = read_enabled()
            if new_value is not None and new_value != enabled:
                enabled = new_value
                log("enabled=%s" % enabled)
                if service_active() != enabled:
                    apply(enabled)
        time.sleep(POLL)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
