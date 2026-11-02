#!/bin/sh
# A warm TopRGU reset preserves RAM -> pstore holds the previous crash trace.
P=/sys/fs/pstore
[ -d "$P" ] || exit 0
ls "$P"/console-ramoops* "$P"/dmesg-ramoops* >/dev/null 2>&1 || exit 0
D=/root/pstore_auto
mkdir -p "$D"
cp -a "$P"/* "$D"/ 2>/dev/null || true
echo "pstore snapshot -> $D (from warm-reboot crash)"
