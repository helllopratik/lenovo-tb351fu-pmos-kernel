#!/bin/sh
# Redundant TopRGU feeder (pmos-guard trap, see PMOS_TB351FU_PORTING_GUIDE.md Issue 27).
# systemd's RuntimeWatchdog opens /dev/watchdog0 exclusively -> our open gets
# EBUSY, which is GOOD (means someone else is feeding). If that holder dies or
# never appears, our open succeeds and we feed instead.
# NOTE: no `exec 3>file` fd tricks: a redirection failure of exec terminates
# busybox ash with status 1 (POSIX special-builtin rule) -> restart loop.
while :; do
    echo K > /dev/watchdog0 2>/dev/null || true
    sleep 5
done
