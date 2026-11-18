#!/usr/bin/env bash
# ==============================================================================
# Flash the postmarketOS boot chain to slot B only.
#
# Works in two modes, auto-detected:
#   adb       - Android is running with root adb (dd to /dev/block/by-name/*_b)
#   fastboot  - device is in bootloader/fastbootd (fastboot flash *_b)
#
# Slot A is never written.  No reboot is performed unless --reboot is given.
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PMOS_DIR="$(dirname "$REPO_ROOT")"
ARTIFACTS_DIR="${ARTIFACTS_DIR:-$PMOS_DIR/artifacts}"

BOOT_IMG=""
INIT_BOOT_IMG=""
VBMETA_IMG=""
SET_ACTIVE=0
REBOOT=0
DRY_RUN=0
SERIAL="${ANDROID_SERIAL:-}"

usage() {
  cat <<EOF
Usage: $0 [OPTIONS]

Flash boot_b, init_boot_b, vbmeta_b, vbmeta_system_b and vbmeta_vendor_b.
Slot A is never modified.

  --boot PATH        boot image (default: artifacts/boot_latest.img)
  --init-boot PATH   init_boot image (default: artifacts/init_boot_latest.img)
  --vbmeta PATH      vbmeta image (default: artifacts/vbmeta_latest.img)
  --serial SERIAL    adb/fastboot serial (default: ANDROID_SERIAL or first)
  --set-active       run 'fastboot set_active b' (fastboot mode only)
  --reboot           reboot when flashing finishes
  --dry-run          print the plan without writing anything
  -h, --help         show this help

Environment:
  ARTIFACTS_DIR      directory holding the default images
  ANDROID_SERIAL     adb/fastboot serial
EOF
}

step() { printf '%%%%PMOS_STEP:%s:%s\n' "$1" "$2"; }
progress() { printf '%%%%PMOS_PROGRESS:%s:%s\n' "$1" "$2"; }
trap 'rc=$?; printf "%%%%PMOS_ERROR:%s\n" "$rc"' ERR

# Machine-readable protocol check used by the installer self-test.
if [ "${PMOS_MARKER_TEST:-}" = "1" ]; then
  step selftest ok
  progress 50 selftest
  printf '%%%%PMOS_DONE\n'
  exit 0
elif [ "${PMOS_MARKER_TEST:-}" = "error" ]; then
  false
fi

while [ "$#" -gt 0 ]; do
  case "$1" in
    --boot) BOOT_IMG="$2"; shift 2 ;;
    --init-boot) INIT_BOOT_IMG="$2"; shift 2 ;;
    --vbmeta) VBMETA_IMG="$2"; shift 2 ;;
    --serial) SERIAL="$2"; shift 2 ;;
    --set-active) SET_ACTIVE=1; shift ;;
    --reboot) REBOOT=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[ -n "$BOOT_IMG" ] || BOOT_IMG="$ARTIFACTS_DIR/boot_latest.img"
[ -n "$INIT_BOOT_IMG" ] || INIT_BOOT_IMG="$ARTIFACTS_DIR/init_boot_latest.img"
[ -n "$VBMETA_IMG" ] || VBMETA_IMG="$ARTIFACTS_DIR/vbmeta_latest.img"

for f in "$BOOT_IMG" "$INIT_BOOT_IMG" "$VBMETA_IMG"; do
  [ -f "$f" ] || { echo "ERROR: image not found: $f" >&2; exit 1; }
done

# The 64 MiB boot image is mostly AVB padding, the real payload is inside it.
echo "boot:      $BOOT_IMG ($(stat -c%s "$BOOT_IMG") bytes)"
echo "init_boot: $INIT_BOOT_IMG ($(stat -c%s "$INIT_BOOT_IMG") bytes)"
echo "vbmeta:    $VBMETA_IMG ($(stat -c%s "$VBMETA_IMG") bytes)"

ADB=(adb)
FB=(fastboot)
[ -n "$SERIAL" ] && ADB+=(-s "$SERIAL") && FB+=(-s "$SERIAL")

MODE=""
if "${ADB[@]}" get-state >/dev/null 2>&1; then
  MODE="adb"
elif "${FB[@]}" devices 2>/dev/null | grep -qwE 'fastbootd?'; then
  MODE="fastboot"
fi

if [ -z "$MODE" ]; then
  echo "ERROR: no device found via adb or fastboot." >&2
  echo "       Boot Android with USB debugging, or enter fastboot." >&2
  exit 1
fi

step mode "Device detected in $MODE mode"
echo "Mode: $MODE"

flash_adb() {
  if [ "$SET_ACTIVE" -eq 1 ]; then
    echo "ERROR: --set-active requires fastboot mode. Run 'adb reboot bootloader' first." >&2
    exit 2
  fi

  step verify "Checking root access and partition sizes"
  "${ADB[@]}" shell "su -c 'id'" | grep -q "uid=0" || {
    echo "ERROR: root adb is required to dd the slot-B partitions." >&2
    exit 1
  }

  # Read the real partition size from sysfs so an oversized image cannot
  # silently run past the end of boot_b/init_boot_b.
  part_bytes() {
    local name="$1" dev node sectors
    dev="$("${ADB[@]}" shell "su -c 'readlink -f /dev/block/by-name/$name'" 2>/dev/null | tr -d '\r')"
    node="${dev##*/}"
    sectors="$("${ADB[@]}" shell "cat /sys/class/block/$node/size" 2>/dev/null | tr -d '\r')"
    if [[ "$sectors" =~ ^[0-9]+$ ]]; then
      echo $((sectors * 512))
    else
      echo 0
    fi
  }

  local boot_size init_size vb_size
  boot_size="$(stat -c%s "$BOOT_IMG")"
  init_size="$(stat -c%s "$INIT_BOOT_IMG")"
  vb_size="$(stat -c%s "$VBMETA_IMG")"
  local boot_part init_part
  boot_part="$(part_bytes boot_b)"
  init_part="$(part_bytes init_boot_b)"
  echo "boot_b size:      ${boot_part:-unknown}"
  echo "init_boot_b size: ${init_part:-unknown}"

  if [ "$boot_part" -gt 0 ] && [ "$boot_size" -gt "$boot_part" ]; then
    echo "ERROR: $BOOT_IMG is larger than boot_b ($boot_part bytes)." >&2
    exit 1
  fi
  if [ "$init_part" -gt 0 ] && [ "$init_size" -gt "$init_part" ]; then
    echo "ERROR: $INIT_BOOT_IMG is larger than init_boot_b ($init_part bytes)." >&2
    exit 1
  fi

  if [ "$DRY_RUN" -eq 1 ]; then
    echo "[dry-run] would push and dd:"
    echo "  vbmeta_latest.img -> vbmeta_b, vbmeta_system_b, vbmeta_vendor_b"
    echo "  boot_latest.img   -> boot_b"
    echo "  init_boot_latest.img -> init_boot_b"
    progress 100 "Dry run complete"
    printf '%%%%PMOS_DONE\n'
    return 0
  fi

  step transfer "Pushing boot images to the tablet"
  progress 20 "Pushing images"
  "${ADB[@]}" push "$BOOT_IMG" /data/local/tmp/pmos_boot.img
  "${ADB[@]}" push "$INIT_BOOT_IMG" /data/local/tmp/pmos_init_boot.img
  "${ADB[@]}" push "$VBMETA_IMG" /data/local/tmp/pmos_vbmeta.img

  step provision "Ensuring slot-B vendor_boot and dtbo are populated"
  # An all-zero 16-byte prefix means the partition is still erased.  Use od
  # instead of tr so NUL handling does not depend on the device shell.
  "${ADB[@]}" shell "su -c '
    zeros=00000000000000000000000000000000
    if [ \"\$(dd if=/dev/block/by-name/vendor_boot_b bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d \" \\n\")\" = \"\$zeros\" ]; then
      echo \"Provisioning vendor_boot_b from slot A\"
      dd if=/dev/block/by-name/vendor_boot_a of=/dev/block/by-name/vendor_boot_b bs=4194304 conv=fsync
    fi
    if [ \"\$(dd if=/dev/block/by-name/dtbo_b bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d \" \\n\")\" = \"\$zeros\" ]; then
      echo \"Provisioning dtbo_b from slot A\"
      dd if=/dev/block/by-name/dtbo_a of=/dev/block/by-name/dtbo_b bs=4194304 conv=fsync
    fi
  '"

  step write "Writing slot B"
  progress 45 "Writing vbmeta"
  "${ADB[@]}" shell "su -c 'dd if=/data/local/tmp/pmos_vbmeta.img of=/dev/block/by-name/vbmeta_b bs=4096 conv=fsync'"
  "${ADB[@]}" shell "su -c 'dd if=/data/local/tmp/pmos_vbmeta.img of=/dev/block/by-name/vbmeta_system_b bs=4096 conv=fsync'"
  "${ADB[@]}" shell "su -c 'dd if=/data/local/tmp/pmos_vbmeta.img of=/dev/block/by-name/vbmeta_vendor_b bs=4096 conv=fsync'"

  progress 65 "Writing boot_b"
  "${ADB[@]}" shell "su -c 'dd if=/data/local/tmp/pmos_boot.img of=/dev/block/by-name/boot_b bs=4194304 conv=fsync'"

  progress 80 "Writing init_boot_b"
  "${ADB[@]}" shell "su -c 'dd if=/data/local/tmp/pmos_init_boot.img of=/dev/block/by-name/init_boot_b bs=4194304 conv=fsync'"

  step verify "Verifying slot-B checksums"
  progress 90 "Verifying checksums"
  # Compare exactly the bytes we wrote.  Reading the whole partition would
  # include erased tail bytes and produce a false mismatch for images that
  # are smaller than their partition.  exec-out avoids adb's PTY newline
  # translation, which would corrupt a binary checksum stream.
  local host_md5 dev_md5
  host_md5="$(cat "$BOOT_IMG" "$INIT_BOOT_IMG" "$VBMETA_IMG" | md5sum | awk '{print $1}')"
  dev_md5="$("${ADB[@]}" exec-out su -c "head -c $boot_size /dev/block/by-name/boot_b; head -c $init_size /dev/block/by-name/init_boot_b; head -c $vb_size /dev/block/by-name/vbmeta_b" | md5sum | awk '{print $1}')"
  if [ "$host_md5" != "$dev_md5" ]; then
    echo "ERROR: checksum mismatch after writing slot B" >&2
    echo "host:   $host_md5" >&2
    echo "device: $dev_md5" >&2
    exit 1
  fi
  echo "Checksums match."

  "${ADB[@]}" shell "rm -f /data/local/tmp/pmos_boot.img /data/local/tmp/pmos_init_boot.img /data/local/tmp/pmos_vbmeta.img"

  step done "Slot B boot chain flashed"
  progress 100 "Slot B boot chain flashed"
  printf '%%%%PMOS_DONE\n'

  if [ "$REBOOT" -eq 1 ]; then
    echo "Rebooting..."
    "${ADB[@]}" reboot
  fi
}

flash_fastboot() {
  step verify "Checking fastboot state"
  "${FB[@]}" getvar current-slot 2>&1 | sed 's/^/  /' || true

  if [ "$DRY_RUN" -eq 1 ]; then
    echo "[dry-run] would run:"
    echo "  fastboot flash boot_b $BOOT_IMG"
    echo "  fastboot flash init_boot_b $INIT_BOOT_IMG"
    echo "  fastboot flash vbmeta_b $VBMETA_IMG"
    echo "  fastboot flash vbmeta_system_b $VBMETA_IMG"
    echo "  fastboot flash vbmeta_vendor_b $VBMETA_IMG"
    progress 100 "Dry run complete"
    printf '%%%%PMOS_DONE\n'
    return 0
  fi

  step write "Writing slot B via fastboot"
  progress 30 "Flashing vbmeta"
  "${FB[@]}" flash vbmeta_b "$VBMETA_IMG"
  "${FB[@]}" flash vbmeta_system_b "$VBMETA_IMG"
  "${FB[@]}" flash vbmeta_vendor_b "$VBMETA_IMG"

  progress 60 "Flashing boot_b"
  "${FB[@]}" flash boot_b "$BOOT_IMG"

  progress 85 "Flashing init_boot_b"
  "${FB[@]}" flash init_boot_b "$INIT_BOOT_IMG"

  if [ "$SET_ACTIVE" -eq 1 ]; then
    step active "Setting slot B active"
    "${FB[@]}" set_active b
  fi

  step done "Slot B boot chain flashed"
  progress 100 "Slot B boot chain flashed"
  printf '%%%%PMOS_DONE\n'

  if [ "$REBOOT" -eq 1 ]; then
    echo "Rebooting..."
    "${FB[@]}" reboot
  fi
}

if [ "$MODE" = "adb" ]; then
  flash_adb
else
  flash_fastboot
fi
