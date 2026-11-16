#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Mount internal storage and read-only Android partitions for the pmOS desktop.
#
# Layout:
#   /media/internal          rw  /data_host/media/0   (Android shared storage)
#   /media/android-data      ro  /data_host           (Android /data)
#   /media/android-parts/*   ro  super logical partitions + raw UFS partitions
#
# Safety rules:
#   * /data_host/pmos is self-bound and remounted read-only before anything is
#     exposed.  It contains rootfs.img, the backing store of the running pmOS
#     root filesystem, and must never be writable from the desktop.
#   * Android /data is only ever exposed through a read-only bind.
#   * Raw UFS partitions are mounted read-only with ext4 journal replay
#     disabled (noload) so a slot-A Android filesystem is never modified.
#   * SD cards and USB storage are intentionally left to udisks2/Nautilus.
#
# This script is idempotent: re-running it leaves existing mounts alone.

set -u

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

DATA_HOST=/data_host
MEDIA_ROOT=/media
INTERNAL="$MEDIA_ROOT/internal"
ANDROID_DATA="$MEDIA_ROOT/android-data"
PARTS="$MEDIA_ROOT/android-parts"
STATE=/run/pmos-storage
LOG=/run/pmos-storage.log
SUPERMAP=/usr/local/bin/pmos-supermap.py
KMSG=/dev/kmsg

mkdir -p "$STATE" "$MEDIA_ROOT" "$PARTS"
: > "$LOG"

log() {
	msg="PMOSSTORAGE: $*"
	echo "$msg"
	echo "$msg" >> "$LOG"
	echo "$msg" > "$KMSG" 2>/dev/null || true
}

is_mounted() {
	findmnt -n -M "$1" >/dev/null 2>&1
}

# Wait for the initramfs to finish mounting Android userdata.  It is normally
# already there before switch_root, but a slow UFS probe must not wedge boot.
wait_for_data_host() {
	i=0
	while [ ! -d "$DATA_HOST/media/0" ] && [ "$i" -lt 100 ]; do
		sleep 0.1
		i=$((i + 1))
	done
	[ -d "$DATA_HOST/media/0" ]
}

find_super() {
	for p in /dev/disk/by-partlabel/super /dev/block/by-name/super; do
		if [ -e "$p" ]; then
			readlink -f "$p"
			return 0
		fi
	done
	for u in /sys/class/block/*/uevent; do
		[ -f "$u" ] || continue
		if grep -qx 'PARTNAME=super' "$u" 2>/dev/null; then
			dev=$(sed -n 's/^DEVNAME=//p' "$u")
			if [ -n "$dev" ]; then
				echo "/dev/$dev"
				return 0
			fi
		fi
	done
	return 1
}

# Protect the pmOS rootfs image before exposing any part of Android /data.
protect_pmos_dir() {
	[ -d "$DATA_HOST/pmos" ] || return 0
	if ! is_mounted "$DATA_HOST/pmos"; then
		if ! mount --bind "$DATA_HOST/pmos" "$DATA_HOST/pmos"; then
			log "ERROR: cannot self-bind $DATA_HOST/pmos"
			return 1
		fi
	fi
	if ! mount -o remount,bind,ro "$DATA_HOST/pmos"; then
		log "ERROR: cannot remount $DATA_HOST/pmos read-only"
		return 1
	fi
	log "protected $DATA_HOST/pmos (read-only)"
	return 0
}

mount_internal() {
	if is_mounted "$INTERNAL"; then
		log "$INTERNAL already mounted"
		return 0
	fi
	if ! mount --bind "$DATA_HOST/media/0" "$INTERNAL"; then
		log "ERROR: cannot bind $DATA_HOST/media/0 on $INTERNAL"
		return 1
	fi
	log "internal storage (rw) at $INTERNAL"
	return 0
}

mount_android_data() {
	if is_mounted "$ANDROID_DATA"; then
		# Make sure a previous run did not leave it writable.
		mount -o remount,bind,ro "$ANDROID_DATA" 2>/dev/null || true
		log "$ANDROID_DATA already mounted"
		return 0
	fi
	if ! mount --bind "$DATA_HOST" "$ANDROID_DATA"; then
		log "ERROR: cannot bind $DATA_HOST on $ANDROID_DATA"
		return 1
	fi
	if ! mount -o remount,bind,ro "$ANDROID_DATA"; then
		log "ERROR: cannot remount $ANDROID_DATA read-only"
		umount "$ANDROID_DATA" 2>/dev/null || true
		return 1
	fi
	log "Android /data (ro) at $ANDROID_DATA"
	return 0
}

mount_one() {
	dev=$1
	label=$2
	fs=$3
	mnt="$PARTS/$label"

	case "$fs" in
	ext4) opts="ro,nosuid,nodev,noload" ;;
	f2fs|erofs) opts="ro,nosuid,nodev" ;;
	*) opts="ro,nosuid,nodev" ;;
	esac

	if is_mounted "$mnt"; then
		return 0
	fi
	mkdir -p "$mnt"
	if mount -t "$fs" -o "$opts" "$dev" "$mnt"; then
		log "mounted $label ($fs, ro) at $mnt"
		return 0
	fi
	log "WARN: cannot mount $label ($fs) from $dev"
	rmdir "$mnt" 2>/dev/null || true
	return 1
}

mount_super_partitions() {
	super=""
	i=0
	while [ "$i" -lt 100 ]; do
		super=$(find_super) && break
		sleep 0.1
		i=$((i + 1))
	done
	if [ -z "$super" ]; then
		log "WARN: no super partition found; Android logical partitions unavailable"
		return 0
	fi
	log "super partition: $super"

	if [ ! -x "$SUPERMAP" ]; then
		log "ERROR: $SUPERMAP is missing"
		return 1
	fi

	map=$(python3 "$SUPERMAP" --device "$super" 2>>"$LOG") || {
		log "ERROR: liblp metadata parse failed for $super"
		return 1
	}
	[ -n "$map" ] || {
		log "WARN: super metadata contains no populated partitions"
		return 0
	}

	echo "$map" | while IFS='|' read -r name offset size group; do
		[ -n "$name" ] || continue
		mnt="$PARTS/$name"
		if is_mounted "$mnt"; then
			continue
		fi
		mkdir -p "$mnt"
		loop=$(losetup --find --show --read-only \
			--offset "$offset" --sizelimit "$size" "$super" 2>>"$LOG") || {
			log "WARN: losetup failed for $name"
			rmdir "$mnt" 2>/dev/null || true
			continue
		}
		echo "$loop" >> "$STATE/loops"
		fs=$(blkid -p -o value -s TYPE "$loop" 2>/dev/null)
		if [ -z "$fs" ]; then
			log "WARN: no filesystem detected in $name"
			losetup -d "$loop" 2>/dev/null || true
			rmdir "$mnt" 2>/dev/null || true
			continue
		fi
		case "$fs" in
		ext4) opts="ro,nosuid,nodev,noload" ;;
		f2fs|erofs) opts="ro,nosuid,nodev" ;;
		*) opts="ro,nosuid,nodev" ;;
		esac
		if mount -t "$fs" -o "$opts" "$loop" "$mnt"; then
			log "mounted $name ($fs, ro, offset $offset) at $mnt"
		else
			log "WARN: cannot mount $name ($fs) at $mnt"
			losetup -d "$loop" 2>/dev/null || true
			rmdir "$mnt" 2>/dev/null || true
		fi
	done
	return 0
}

mount_raw_ufs_partitions() {
	super=$(find_super) || return 0
	parent=$(lsblk -no PKNAME "$super" 2>/dev/null | head -n 1)
	[ -n "$parent" ] || {
		log "WARN: cannot determine UFS parent disk for $super"
		return 0
	}

	lsblk -rno NAME,FSTYPE,PARTLABEL "/dev/$parent" 2>/dev/null |
	while IFS=' ' read -r name fs label; do
		[ -n "$fs" ] || continue
		# Android userdata is already live at /data_host and super is
		# handled through liblp, never mount either raw.
		[ "$label" = "userdata" ] && continue
		[ "$label" = "super" ] && continue
		case "$fs" in
		ext4|f2fs|erofs|vfat|exfat) ;;
		*) continue ;;
		esac
		[ -n "$label" ] || label=$name
		mount_one "/dev/$name" "$label" "$fs"
	done
	return 0
}

add_bookmark() {
	uri=$1
	label=$2
	dir=/home/user/.config/gtk-3.0
	file="$dir/bookmarks"
	[ -d /home/user ] || return 0
	mkdir -p "$dir"
	[ -f "$file" ] || : > "$file"
	grep -Fqx "$uri $label" "$file" 2>/dev/null && return 0
	printf '%s %s\n' "$uri" "$label" >> "$file"
}

setup_bookmarks() {
	add_bookmark "file://$INTERNAL" "Internal Storage"
	add_bookmark "file://$ANDROID_DATA" "Android Data (RO)"
	add_bookmark "file://$PARTS" "Android Partitions"
	chown -R 10000:10000 /home/user/.config/gtk-3.0 2>/dev/null || true
}

check_group_membership() {
	groups=$(id -G user 2>/dev/null || true)
	for pair in "1000 system" "1023 media_rw" "1028 sdcard_r" "1078 ext_data_rw" "1079 ext_obb_rw"; do
		gid=${pair%% *}
		name=${pair#* }
		case " $groups " in
		*" $gid "*) ;;
		*) log "WARN: user is not in group $name ($gid); files may be unreadable" ;;
		esac
	done
}

log "=== internal storage bring-up ==="

if ! wait_for_data_host; then
	log "ERROR: $DATA_HOST/media/0 did not appear; internal storage skipped"
else
	if ! protect_pmos_dir; then
		log "ERROR: refusing to expose Android /data without protecting $DATA_HOST/pmos"
	else
		mount_internal || true
		mount_android_data || true
		mount_super_partitions || true
		mount_raw_ufs_partitions || true
		setup_bookmarks
		check_group_membership
	fi
fi

log "=== done: $(findmnt -rn -o TARGET | grep -c '^/media/') mounts under /media ==="
exit 0
