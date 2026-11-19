#!/system/bin/sh
# Offline whole-device postmarketOS installer.
#
# THIS ERASES THE ANDROID userdata PARTITION.  It must be run from a recovery
# or initramfs environment with /data unmounted, never from a running Android.
#
#   PMOS_WHOLE_CONFIRM=ERASE-ANDROID sh install_whole_device.sh [DEVICE]
#
# The default DEVICE is /dev/block/by-name/userdata.  The rootfs image was
# prepared with the requested account, system customizations, and the
# /pmos_whole_device marker.  A boot image that mounts userdata directly is
# still required; this package intentionally contains no boot image.
set -e

EXPECTED_SIZE="@ROOTFS_SIZE_BYTES@"
EXPECTED_PACK_SHA256="@ROOTFS_PACK_SHA256@"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR"

die() { echo "WHOLE_ERROR: $*" >&2; exit 1; }
log() { echo "WHOLE: $*"; }

[ "${PMOS_WHOLE_CONFIRM:-}" = "ERASE-ANDROID" ] || \
  die "set PMOS_WHOLE_CONFIRM=ERASE-ANDROID to confirm the destructive install"
[ -z "${1:-}" ] || DST="$1"
DST="${DST:-/dev/block/by-name/userdata}"

[ -b "$DST" ] || die "$DST is not a block device"
[ -f rootfs.sparsepack ] || die "rootfs.sparsepack is missing"
[ -f sparse_pack ] || die "sparse_pack is missing"
if [ ! -x sparse_pack ]; then
  chmod 700 sparse_pack 2>/dev/null || true
fi
[ -x sparse_pack ] || die "sparse_pack is not executable"

log "verify this package on the host before applying it: sha256sum -c SHA256SUMS"
if [ -f SHA256SUMS ] && command -v sha256sum >/dev/null 2>&1; then
  log "checking SHA256SUMS"
  if ! sha256sum -c SHA256SUMS >/dev/null 2>&1; then
    log "WARNING: SHA256SUMS did not verify; continue only if you trust this copy"
  fi
fi

if grep -q ' /data ' /proc/mounts; then
  die "/data is mounted; unmount it before writing userdata"
fi

SIZE=$(blockdev --getsize64 "$DST" 2>/dev/null || true)
if [ -z "$SIZE" ]; then
  NAME=$(basename "$DST")
  [ -r "/sys/class/block/$NAME/size" ] || die "cannot determine the size of $DST"
  SIZE=$(( $(cat "/sys/class/block/$NAME/size") * 512 ))
fi
[ "$SIZE" -ge "$EXPECTED_SIZE" ] || \
  die "$DST is $SIZE bytes, smaller than the $EXPECTED_SIZE-byte image"

if [ -n "$EXPECTED_PACK_SHA256" ] && command -v sha256sum >/dev/null 2>&1; then
  log "checking the rootfs pack checksum"
  ACTUAL=$(sha256sum rootfs.sparsepack | sed 's/ .*//')
  [ "$ACTUAL" = "$EXPECTED_PACK_SHA256" ] || die "rootfs.sparsepack checksum mismatch"
fi

log "writing the postmarketOS rootfs to $DST"
./sparse_pack apply rootfs.sparsepack "$DST"
log "verifying the written extents"
./sparse_pack verify rootfs.sparsepack "$DST"
sync

log "whole-device rootfs written and verified"
log "this package does not contain a boot image"
log "flash a boot/init_boot image that boots userdata directly before restarting"
log "Android is gone from this device; keep a backup if you need it"
