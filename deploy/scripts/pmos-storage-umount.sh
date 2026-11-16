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

if [ -d "$PARTS" ]; then
	# $PARTS itself is a plain directory, not a mountpoint, so findmnt -R
	# cannot walk it.  Select every mount whose target is below it instead.
	findmnt -rn -o TARGET |
	awk -v p="$PARTS/" 'index($0, p) == 1' | sort -r |
	while IFS= read -r mnt; do
		[ -n "$mnt" ] || continue
		umount "$mnt" 2>/dev/null && log "unmounted $mnt" || log "WARN: cannot unmount $mnt"
	done
fi

if [ -f "$STATE/loops" ]; then
	left=""
	while IFS= read -r loop; do
		[ -n "$loop" ] || continue
		if findmnt -rn -o SOURCE | grep -Fqx "$loop"; then
			log "WARN: $loop is still mounted; not detaching"
			if [ -z "$left" ]; then left="$loop"; else left="$left
$loop"; fi
			continue
		fi
		losetup -d "$loop" 2>/dev/null || true
		if [ -s "/sys/block/${loop##*/}/loop/backing_file" ]; then
			# Still attached: either a mount is using it or detach
			# failed.  Keep it tracked for the next attempt.
			log "WARN: cannot detach $loop"
			if [ -z "$left" ]; then left="$loop"; else left="$left
$loop"; fi
		else
			# losetup -d can race with the last close and return
			# failure for a loop the kernel has already released.
			log "detached $loop"
		fi
	done < "$STATE/loops"
	if [ -n "$left" ]; then
		printf '%s\n' "$left" > "$STATE/loops"
	else
		rm -f "$STATE/loops"
	fi
fi

for mnt in "$ANDROID_DATA" "$INTERNAL"; do
	if is_mounted "$mnt"; then
		umount "$mnt" 2>/dev/null && log "unmounted $mnt" || log "WARN: cannot unmount $mnt"
	fi
done

log "=== done (rootfs protection left in place) ==="
exit 0
