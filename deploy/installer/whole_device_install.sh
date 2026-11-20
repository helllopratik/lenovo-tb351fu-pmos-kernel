#!/system/bin/sh
# Offline whole-device postmarketOS installer for the Lenovo TB351FU.
#
# THIS ERASES ANDROID.  It removes the Android system (super), the metadata
# encryption keys (metadata), factory-reset protection (frp) and the entire
# Android userdata filesystem, then writes a postmarketOS ext4 rootfs directly
# to userdata through a dm-default-key mapping.  It also replaces the boot
# chain in slots A and B with the pmOS images shipped in this package.
#
# It must run from a recovery/initramfs environment with /data unmounted,
# never from a running Android.  It never writes the misc partition and never
# changes the active slot; slot B remains active.
#
#   PMOS_WHOLE_CONFIRM='ERASE ANDROID SLOT A' \
#     sh install_whole_device.sh --remove-legacy
#
# Use --dry-run to print the exact plan without writing anything.
set -e

EXPECTED_SIZE="@ROOTFS_SIZE_BYTES@"
EXPECTED_PACK_SHA256="@ROOTFS_PACK_SHA256@"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR"

CONFIRM_PHRASE="ERASE ANDROID SLOT A"
MAP_NAME="pmos_whole_install"
MAP_DEV=""
DM_CREATED=0
KEEP_MAPPING=0
REMOVE_LEGACY=0
DRY_RUN=0
MAPPED_ARG=""
USERDATA_ARG=""
TMP_MOUNT="/tmp/pmos_whole_check"

die() { echo "WHOLE_ERROR: $*" >&2; exit 1; }
log() { echo "WHOLE: $*"; }
warn() { echo "WHOLE_WARNING: $*" >&2; }

cleanup() {
  rc=$?
  if [ -d "$TMP_MOUNT" ]; then
    umount "$TMP_MOUNT" 2>/dev/null || true
    rmdir "$TMP_MOUNT" 2>/dev/null || true
  fi
  if [ "$DM_CREATED" = "1" ] && [ "$KEEP_MAPPING" != "1" ]; then
    dmsetup remove "$MAP_NAME" >/dev/null 2>&1 || \
      dmsetup remove --force "$MAP_NAME" >/dev/null 2>&1 || \
      warn "could not remove the dm mapping $MAP_NAME"
  fi
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

usage() {
  cat <<EOF
Usage: sh install_whole_device.sh [OPTIONS] [USERDATA]

Erase Android and install postmarketOS as the only system.  USERDATA defaults
to /dev/block/by-name/userdata (or the equivalent by-partlabel node).

Options:
  --remove-legacy   Required for a real run.  Acknowledges that the Android
                    userdata tree, including /data/pmos_legacy/rootfs.img, is
                    destroyed.  There is no way to preserve it.
  --dry-run         Print the plan and exit without writing anything.
  --mapped DEV      Use an existing dm-default-key mapping instead of creating
                    one with dmsetup.  DEV is usually /dev/dm-N.
  --userdata DEV    Userdata partition to map and overwrite.
  --keep-mapping    Do not remove the dm mapping before exiting.
  -h, --help        Show this help.

Confirmation:
  Type the phrase 'ERASE ANDROID SLOT A' at the prompt, or set
  PMOS_WHOLE_CONFIRM to that exact phrase for a non-interactive run.

The package is self-contained except for dmsetup, dd, sha256sum and mount from
the recovery environment.  The dm key file is removed after a successful run.
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --remove-legacy) REMOVE_LEGACY=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    --keep-mapping) KEEP_MAPPING=1; shift ;;
    --mapped)
      [ "$#" -ge 2 ] || die "--mapped needs a device"
      MAPPED_ARG="$2"; shift 2 ;;
    --userdata)
      [ "$#" -ge 2 ] || die "--userdata needs a device"
      USERDATA_ARG="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    --*) die "unknown option: $1" ;;
    *)
      [ -z "$USERDATA_ARG" ] || die "only one userdata device can be given"
      USERDATA_ARG="$1"; shift ;;
  esac
done

resolve_part() {
  name=$1
  for p in "/dev/block/by-name/$name" "/dev/disk/by-partlabel/$name"; do
    if [ -e "$p" ]; then
      readlink -f "$p" 2>/dev/null || echo "$p"
      return 0
    fi
  done
  for u in /sys/class/block/*/uevent; do
    [ -f "$u" ] || continue
    if grep -qx "PARTNAME=$name" "$u" 2>/dev/null; then
      echo "/dev/$(basename "$(dirname "$u")")"
      return 0
    fi
  done
  return 1
}

part_size_bytes() {
  dev=$1
  size=$(blockdev --getsize64 "$dev" 2>/dev/null || true)
  if [ -z "$size" ]; then
    node=$(basename "$(readlink -f "$dev" 2>/dev/null || echo "$dev")")
    [ -r "/sys/class/block/$node/size" ] || return 1
    size=$(( $(cat "/sys/class/block/$node/size") * 512 ))
  fi
  printf '%s\n' "$size"
}

sha256_of() {
  sha256sum "$1" | awk '{print $1}'
}

file_size() {
  stat -c %s "$1" 2>/dev/null || wc -c < "$1"
}

readback_hash() {
  dev=$1
  size=$2
  head -c "$size" "$dev" | sha256sum | awk '{print $1}'
}

dm_device_path() {
  name=$1
  if [ -b "/dev/mapper/$name" ]; then
    echo "/dev/mapper/$name"
    return 0
  fi
  dmsetup mknodes "$name" >/dev/null 2>&1 || true
  if [ -b "/dev/mapper/$name" ]; then
    echo "/dev/mapper/$name"
    return 0
  fi
  for d in /sys/class/block/dm-*; do
    [ -r "$d/dm/name" ] || continue
    if [ "$(cat "$d/dm/name")" = "$name" ]; then
      min=$(basename "$d")
      for p in "/dev/$min" "/dev/block/$min"; do
        if [ -b "$p" ]; then
          echo "$p"
          return 0
        fi
      done
      maj=$(cat "/sys/class/block/$min/dev" 2>/dev/null | cut -d: -f1)
      mi=$(cat "/sys/class/block/$min/dev" 2>/dev/null | cut -d: -f2)
      [ -n "$maj" ] && [ -n "$mi" ] || return 1
      mknod "/dev/$min" b "$maj" "$mi" 2>/dev/null || true
      if [ -b "/dev/$min" ]; then
        echo "/dev/$min"
        return 0
      fi
    fi
  done
  return 1
}

refuse_if_mounted() {
  dev=$1
  if grep -q " $dev " /proc/mounts 2>/dev/null; then
    die "$dev is mounted; unmount it first"
  fi
}

flash_image() {
  img=$1
  part=$2
  [ -f "$img" ] || die "missing image: $img"
  [ -b "$part" ] || die "$part is not a block device"
  refuse_if_mounted "$part"
  isize=$(file_size "$img")
  psize=$(part_size_bytes "$part") || die "cannot determine the size of $part"
  [ "$isize" -le "$psize" ] || \
    die "$img ($isize bytes) does not fit in $part ($psize bytes)"
  log "flashing $(basename "$img") -> $part"
  if command -v sha256sum >/dev/null 2>&1; then
    want=$(sha256_of "$img")
  else
    want=""
  fi
  dd if="$img" of="$part" bs=1048576 conv=fsync 2>/dev/null || \
    dd if="$img" of="$part" bs=1048576 2>/dev/null || \
    die "could not write $img to $part"
  sync
  if [ -n "$want" ]; then
    got=$(readback_hash "$part" "$isize")
    [ "$got" = "$want" ] || die "verification failed after writing $part"
  fi
}

erase_partition() {
  name=$1
  dev=$2
  refuse_if_mounted "$dev"
  size=$(part_size_bytes "$dev") || die "cannot determine the size of $dev"
  log "erasing $name ($dev, $size bytes)"
  if command -v blkdiscard >/dev/null 2>&1 && blkdiscard "$dev" 2>/dev/null; then
    log "discarded $name"
    sync
    return 0
  fi
  log "discard unavailable for $name; zero-filling"
  mib=$((size / 1048576))
  if [ "$mib" -gt 0 ]; then
    dd if=/dev/zero of="$dev" bs=1048576 count="$mib" conv=fsync 2>/dev/null || \
      dd if=/dev/zero of="$dev" bs=1048576 count="$mib" 2>/dev/null || \
      die "could not zero-fill $name"
  fi
  rem=$((size % 1048576))
  if [ "$rem" -gt 0 ]; then
    dd if=/dev/zero of="$dev" bs=512 count=$((rem / 512)) \
       seek=$((mib * 2048)) conv=notrunc 2>/dev/null || \
      warn "could not zero the final $rem bytes of $name"
  fi
  sync
}

partition_has_data() {
  dev=$1
  if [ "$(head -c 4096 "$dev" 2>/dev/null | tr -d '\000' | wc -c)" -eq 0 ]; then
    return 1
  fi
  return 0
}

# Some stock devices leave vendor_boot_b or dtbo_b zeroed until first use.
# They contain the DTB/bootconfig, so pmOS cannot boot slot B without them.
# The package does not ship these images; copy slot A's preserved copy when B
# is empty.  If a package has vendor_boot.img/dtbo.img, the flash loop below
# writes those to both slots instead.
provision_slot_b_from_a() {
  name=$1
  if [ -f "$name.img" ]; then
    return 0
  fi
  a=$(resolve_part "${name}_a" || true)
  b=$(resolve_part "${name}_b" || true)
  if [ -z "$a" ] || [ -z "$b" ]; then
    warn "${name}_a or ${name}_b is absent; cannot provision slot B"
    return 0
  fi
  if partition_has_data "$b"; then
    log "${name}_b already populated; leaving it unchanged"
    return 0
  fi
  if ! partition_has_data "$a"; then
    warn "${name}_a is empty too; slot B may not boot"
    return 0
  fi
  log "provisioning ${name}_b from ${name}_a"
  dd if="$a" of="$b" bs=1048576 conv=fsync 2>/dev/null || \
    dd if="$a" of="$b" bs=1048576 2>/dev/null || \
    die "could not copy $a to $b"
  sync
}

confirm_destructive() {
  answer=${PMOS_WHOLE_CONFIRM:-}
  if [ -z "$answer" ]; then
    if [ -t 0 ]; then
      printf "Type '%s' to erase Android and install postmarketOS: " "$CONFIRM_PHRASE"
      IFS= read -r answer || answer=""
    else
      die "set PMOS_WHOLE_CONFIRM='$CONFIRM_PHRASE' or run interactively"
    fi
  fi
  [ "$answer" = "$CONFIRM_PHRASE" ] || die "confirmation phrase did not match"
}

# --- package verification -------------------------------------------------

for f in rootfs.sparsepack sparse_pack boot.img init_boot.img vbmeta.img \
         pmos-dm-key pmos-dm.conf SHA256SUMS; do
  [ -f "$f" ] || die "package is incomplete: $f is missing"
done
[ -x sparse_pack ] || chmod 700 sparse_pack 2>/dev/null || true
[ -x sparse_pack ] || die "sparse_pack is not executable"
command -v sha256sum >/dev/null 2>&1 || die "sha256sum is required"
command -v dd >/dev/null 2>&1 || die "dd is required"

log "verifying the package checksums"
sha256sum -c SHA256SUMS >/dev/null 2>&1 || die "SHA256SUMS did not verify"

ACTUAL_PACK_SHA=$(sha256_of rootfs.sparsepack)
[ "$ACTUAL_PACK_SHA" = "$EXPECTED_PACK_SHA256" ] || \
  die "rootfs.sparsepack checksum mismatch"

# --- key and mapping configuration ---------------------------------------

PMOS_DM_CIPHER="aes-xts-plain64"
PMOS_DM_SECTOR_SIZE="4096"
PMOS_DM_WRAPPEDKEY="0"
# shellcheck disable=SC1091
. ./pmos-dm.conf
case "$PMOS_DM_CIPHER" in
  ""|*[!A-Za-z0-9-]*) die "invalid PMOS_DM_CIPHER" ;;
esac
case "$PMOS_DM_SECTOR_SIZE" in
  512|4096) ;;
  *) die "PMOS_DM_SECTOR_SIZE must be 512 or 4096" ;;
esac
case "$PMOS_DM_WRAPPEDKEY" in
  0|1) ;;
  *) die "PMOS_DM_WRAPPEDKEY must be 0 or 1" ;;
esac
DMKEY=$(tr -d ' \t\r\n' < pmos-dm-key)
case "$DMKEY" in
  ""|*[!0-9a-fA-F]*) die "pmos-dm-key is not hexadecimal" ;;
esac
[ "${#DMKEY}" -ge 64 ] && [ "${#DMKEY}" -le 128 ] && \
  [ $(( ${#DMKEY} % 2 )) -eq 0 ] || die "pmos-dm-key has an invalid length"

# --- partition plan -------------------------------------------------------

if [ -n "$MAPPED_ARG" ]; then
  MAP_DEV=$(readlink -f "$MAPPED_ARG" 2>/dev/null || echo "$MAPPED_ARG")
  [ -b "$MAP_DEV" ] || die "$MAPPED_ARG is not a block device"
  USERDATA_DEV=""
else
  if [ -z "$USERDATA_ARG" ]; then
    USERDATA_DEV=$(resolve_part userdata) || \
      die "cannot find the userdata partition"
  else
    USERDATA_DEV=$(readlink -f "$USERDATA_ARG" 2>/dev/null || echo "$USERDATA_ARG")
  fi
  [ -b "$USERDATA_DEV" ] || die "$USERDATA_DEV is not a block device"
fi

SUPER_DEV=$(resolve_part super || true)
METADATA_DEV=$(resolve_part metadata || true)
FRP_DEV=$(resolve_part frp || true)

BOOT_IMG_SIZE=$(file_size boot.img)
INIT_BOOT_IMG_SIZE=$(file_size init_boot.img)
VBMETA_IMG_SIZE=$(file_size vbmeta.img)

if [ -n "$MAPPED_ARG" ]; then
  MAP_SIZE=$(part_size_bytes "$MAP_DEV") || die "cannot determine the mapping size"
else
  MAP_SIZE=$(part_size_bytes "$USERDATA_DEV") || die "cannot determine the userdata size"
fi
[ "$MAP_SIZE" -ge "$EXPECTED_SIZE" ] || \
  die "userdata/mapping is $MAP_SIZE bytes, smaller than the $EXPECTED_SIZE-byte image"

plan() {
  log "PLAN (nothing has been written yet)"
  if [ -n "$USERDATA_DEV" ]; then
    log "  userdata:  $USERDATA_DEV ($MAP_SIZE bytes)"
  else
    log "  mapping:   $MAP_DEV ($MAP_SIZE bytes)"
  fi
  log "  rootfs:    rootfs.sparsepack, $EXPECTED_SIZE logical bytes, dm-default-key/$PMOS_DM_CIPHER"
  log "  boot A/B:  boot.img ($BOOT_IMG_SIZE), init_boot.img ($INIT_BOOT_IMG_SIZE), vbmeta.img ($VBMETA_IMG_SIZE)"
  log "             vbmeta.img also to vbmeta_system_* and vbmeta_vendor_* when present"
  if [ -f vendor_boot.img ]; then
    log "  optional:  vendor_boot.img to both slots"
  else
    log "  vendor_boot: keep A/B, copy empty B from A"
  fi
  if [ -f dtbo.img ]; then
    log "  optional:  dtbo.img to both slots"
  else
    log "  dtbo:      keep A/B, copy empty B from A"
  fi
  log "  erase:     ${SUPER_DEV:-super (absent)}, ${METADATA_DEV:-metadata (absent)}, ${FRP_DEV:-frp (absent)}"
  log "  preserve:  misc (never written), lk/gz/tee/scp/sspm/mcupm/dpm/spmfw/pi_img,"
  log "             persist, nvdata, nvcfg, nvram, proinfo, protect1/2, seccfg, otp, efuse, sec1"
  log "  slot B stays active; misc is never changed"
  log "  the dm key file is removed after a successful run"
}

plan

if [ "$DRY_RUN" = "1" ]; then
  if [ "$REMOVE_LEGACY" != "1" ]; then
    log "--remove-legacy is required for a real run (this package erases userdata)"
  fi
  log "DRY RUN complete; no writes were made"
  exit 0
fi

[ "$REMOVE_LEGACY" = "1" ] || \
  die "--remove-legacy is required: this erases Android userdata, including /data/pmos_legacy/rootfs.img"

if grep -q ' /data ' /proc/mounts 2>/dev/null; then
  die "/data is mounted; unmount it and run this from recovery, never from Android"
fi
if command -v getprop >/dev/null 2>&1 && \
   [ "$(getprop init.svc.zygote 2>/dev/null)" = "running" ]; then
  die "zygote is running; this must run from recovery, never from live Android"
fi

confirm_destructive
log "confirmation accepted"

# --- create the dm-default-key mapping -----------------------------------

if [ -z "$MAPPED_ARG" ]; then
  command -v dmsetup >/dev/null 2>&1 || \
    die "dmsetup is required; use a recovery that provides it, or pass --mapped"
  SECTORS=$((MAP_SIZE / 512))
  if [ "$PMOS_DM_SECTOR_SIZE" != "512" ]; then
    SECTORS=$((SECTORS / 8 * 8))
  fi
  [ "$SECTORS" -gt 0 ] || die "userdata is too small"

  UD_NODE=$(basename "$(readlink -f "$USERDATA_DEV" 2>/dev/null || echo "$USERDATA_DEV")")
  MAJMIN=$(cat "/sys/class/block/$UD_NODE/dev" 2>/dev/null || true)
  [ -n "$MAJMIN" ] || die "cannot determine the major:minor of $USERDATA_DEV"

  # Mirror pmos_init.c: allow_discards is always enabled; the 4K sector
  # option adds iv_large_sectors; wrappedkey_v0 is appended when configured.
  OPTS="allow_discards"
  NOPTS=1
  if [ "$PMOS_DM_SECTOR_SIZE" != "512" ]; then
    OPTS="$OPTS sector_size:$PMOS_DM_SECTOR_SIZE iv_large_sectors"
    NOPTS=$((NOPTS + 2))
  fi
  if [ "$PMOS_DM_WRAPPEDKEY" = "1" ]; then
    OPTS="$OPTS wrappedkey_v0"
    NOPTS=$((NOPTS + 1))
  fi
  log "creating dm-default-key mapping $MAP_NAME"
  if ! printf '%s\n' "0 $SECTORS default-key $PMOS_DM_CIPHER $DMKEY 0 $MAJMIN 0 $NOPTS $OPTS" | \
       dmsetup create "$MAP_NAME"; then
    die "could not create the dm-default-key mapping"
  fi
  DM_CREATED=1
  MAP_DEV=$(dm_device_path "$MAP_NAME") || die "mapping created but its device node is missing"
  [ -b "$MAP_DEV" ] || die "mapping device $MAP_DEV is not a block device"
  log "mapping ready: $MAP_DEV"
else
  log "using the existing mapping $MAP_DEV"
fi

# --- write and verify the rootfs -----------------------------------------

if command -v blkdiscard >/dev/null 2>&1; then
  log "discarding old userdata blocks (best effort)"
  blkdiscard "$MAP_DEV" >/dev/null 2>&1 || \
    warn "discard is not supported; old blocks outside the new rootfs may remain recoverable"
fi

log "writing the postmarketOS rootfs to $MAP_DEV"
./sparse_pack apply rootfs.sparsepack "$MAP_DEV" || die "sparse apply failed"
./sparse_pack verify rootfs.sparsepack "$MAP_DEV" || die "sparse verify failed"
sync

# A read-only mount is possible in most recoveries.  It confirms that the
# staging image really carries the whole-device marker before we replace the
# boot chain.
if command -v mount >/dev/null 2>&1; then
  mkdir -p "$TMP_MOUNT"
  if mount -t ext4 -o ro "$MAP_DEV" "$TMP_MOUNT" 2>/dev/null; then
    if [ ! -f "$TMP_MOUNT/pmos_whole_device" ]; then
      umount "$TMP_MOUNT" 2>/dev/null || true
      rmdir "$TMP_MOUNT" 2>/dev/null || true
      die "the written rootfs has no /pmos_whole_device marker"
    fi
    umount "$TMP_MOUNT" 2>/dev/null || true
    rmdir "$TMP_MOUNT" 2>/dev/null || true
    log "rootfs marker verified"
  else
    rmdir "$TMP_MOUNT" 2>/dev/null || true
    warn "could not mount the new rootfs read-only; relying on sparse verify"
  fi
fi

# --- replace the boot chain in both slots --------------------------------

provision_slot_b_from_a vendor_boot
provision_slot_b_from_a dtbo

for slot in a b; do
  boot_part=$(resolve_part "boot_$slot") || die "cannot find boot_$slot"
  flash_image boot.img "$boot_part"

  # CONFIG_INITRAMFS_FORCE=y links /init into the kernel in boot.img, so
  # init_boot is a matching fallback copy rather than the primary initramfs.
  init_part=$(resolve_part "init_boot_$slot" || true)
  if [ -n "$init_part" ]; then
    flash_image init_boot.img "$init_part"
  else
    warn "init_boot_$slot is absent; boot.img's built-in initramfs is authoritative"
  fi

  vbmeta_part=$(resolve_part "vbmeta_$slot" || true)
  if [ -n "$vbmeta_part" ]; then
    flash_image vbmeta.img "$vbmeta_part"
  else
    warn "vbmeta_$slot is absent; leaving verification as-is"
  fi
  for vbmeta_extra in vbmeta_system vbmeta_vendor; do
    extra_part=$(resolve_part "${vbmeta_extra}_$slot" || true)
    if [ -n "$extra_part" ]; then
      flash_image vbmeta.img "$extra_part"
    else
      warn "${vbmeta_extra}_$slot is absent; skipping"
    fi
  done

  if [ -f vendor_boot.img ]; then
    vendor_part=$(resolve_part "vendor_boot_$slot" || true)
    if [ -n "$vendor_part" ]; then
      flash_image vendor_boot.img "$vendor_part"
    else
      warn "vendor_boot_$slot is absent; skipping vendor_boot.img"
    fi
  fi
  if [ -f dtbo.img ]; then
    dtbo_part=$(resolve_part "dtbo_$slot" || true)
    if [ -n "$dtbo_part" ]; then
      flash_image dtbo.img "$dtbo_part"
    else
      warn "dtbo_$slot is absent; skipping dtbo.img"
    fi
  fi
done

# --- erase the remaining Android state -----------------------------------

if [ -n "$SUPER_DEV" ]; then
  erase_partition super "$SUPER_DEV"
else
  warn "no super partition found; skipping"
fi
if [ -n "$METADATA_DEV" ]; then
  erase_partition metadata "$METADATA_DEV"
else
  warn "no metadata partition found; skipping"
fi
if [ -n "$FRP_DEV" ]; then
  erase_partition frp "$FRP_DEV"
else
  warn "no frp partition found; skipping"
fi

sync

if [ "$DM_CREATED" = "1" ] && [ "$KEEP_MAPPING" != "1" ]; then
  log "removing the dm mapping"
  dmsetup remove "$MAP_NAME" >/dev/null 2>&1 || \
    dmsetup remove --force "$MAP_NAME" >/dev/null 2>&1 || \
    warn "could not remove the dm mapping; it will disappear on reboot"
  DM_CREATED=0
fi

if [ -f pmos-dm-key ]; then
  rm -f pmos-dm-key || warn "could not remove pmos-dm-key; remove it manually"
  log "removed the local dm key file"
fi

log "whole-device install complete"
log "Android system, metadata and userdata are gone; slot B remains active"
log "misc was not written and the active slot was not changed"
log "reboot manually when ready; the next boot mounts userdata as the rootfs"
log "this was not a secure erase: old blocks outside the new filesystem may remain"
