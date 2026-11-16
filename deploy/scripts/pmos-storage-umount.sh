#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Tear down the mounts created by pmos-storage-mount.sh.
#
# The read-only self-bind of /data_host/pmos is deliberately left in place:
# it is the protection for rootfs.img, and unmounting it while pmOS is running
# would make the live root filesystem image writable again.  systemd removes
# it during shutdown like any other mount.

set -u

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

PARTS=/media/android-parts
INTERNAL=/media/internal
ANDROID_DATA=/media/android-data
STATE=/run/pmos-storage
LOG=/run/pmos-storage.log
KMSG=/dev/kmsg

log() {
	msg="PMOSSTORAGE: $*"
	echo "$msg"
	echo "$msg" >> "$LOG"
	echo "$msg" > "$KMSG" 2>/dev/null || true
}

is_mounted() {
	findmnt -n -M "$1" >/dev/null 2>&1
}

log "=== internal storage teardown ==="

if is_mounted "$PARTS"; then
	findmnt -rn -o TARGET -R "$PARTS" 2>/dev/null | sort -r |
	while IFS= read -r mnt; do
		[ -n "$mnt" ] || continue
		[ "$mnt" = "$PARTS" ] && continue
		umount "$mnt" 2>/dev/null && log "unmounted $mnt" || log "WARN: cannot unmount $mnt"
	done
fi

if [ -f "$STATE/loops" ]; then
	while IFS= read -r loop; do
		[ -n "$loop" ] || continue
		losetup -d "$loop" 2>/dev/null && log "detached $loop" || true
	done < "$STATE/loops"
	rm -f "$STATE/loops"
fi

for mnt in "$ANDROID_DATA" "$INTERNAL"; do
	if is_mounted "$mnt"; then
		umount "$mnt" 2>/dev/null && log "unmounted $mnt" || log "WARN: cannot unmount $mnt"
	fi
done

log "=== done (rootfs protection left in place) ==="
exit 0
