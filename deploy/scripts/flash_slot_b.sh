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
VENDOR_BOOT_IMG=""
DTBO_IMG=""
FIRMWARE_DIR=""
SET_ACTIVE=0
REBOOT=0
DRY_RUN=0
INIT_BOOT_ONLY=0
BOOT_ONLY=0
SERIAL="${ANDROID_SERIAL:-}"

# Firmware that belongs to slot B's boot chain.  Every file is written to the
# matching *_b partition only.
FIRMWARE_PARTS=(lk tee gz scp sspm mcupm dpm spmfw pi_img)

usage() {
  cat <<EOF
Usage: $0 [OPTIONS]

Flash boot_b, init_boot_b, vbmeta_b, vbmeta_system_b, vbmeta_vendor_b,
vendor_boot_b and dtbo_b.  With --firmware-dir, also write the slot-B
firmware partitions (lk_b, tee_b, gz_b, scp_b, sspm_b, mcupm_b, dpm_b,
spmfw_b, pi_img_b).  Slot A is never modified.

  --boot PATH        boot image (default: artifacts/boot_latest.img)
  --init-boot PATH   init_boot image (default: artifacts/init_boot_latest.img)
  --vbmeta PATH      vbmeta image (default: artifacts/vbmeta_latest.img)
  --vendor-boot PATH vendor_boot image (default: copy slot A only if B is empty)
  --dtbo PATH        dtbo image (default: copy slot A only if B is empty)
  --firmware-dir DIR directory with lk.img, tee.img, gz.img, scp.img,
                     sspm.img, mcupm.img, dpm.img, spmfw.img and pi_img.img
  --serial SERIAL    adb/fastboot serial (default: ANDROID_SERIAL or first)
  --set-active       run 'fastboot set_active b' (fastboot mode only)
  --reboot           reboot when flashing finishes
  --dry-run          print the plan without writing anything
  --only-init-boot   write init_boot_b only (skips boot, vbmeta,
                     vendor_boot, dtbo and firmware)
  --only-boot        write boot_b only (skips init_boot, vbmeta,
                     vendor_boot, dtbo and firmware)
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
    --vendor-boot) VENDOR_BOOT_IMG="$2"; shift 2 ;;
    --dtbo) DTBO_IMG="$2"; shift 2 ;;
    --firmware-dir) FIRMWARE_DIR="$2"; shift 2 ;;
    --serial) SERIAL="$2"; shift 2 ;;
    --set-active) SET_ACTIVE=1; shift ;;
    --reboot) REBOOT=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    --only-init-boot) INIT_BOOT_ONLY=1; shift ;;
    --only-boot) BOOT_ONLY=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[ -n "$BOOT_IMG" ] || BOOT_IMG="$ARTIFACTS_DIR/boot_latest.img"
[ -n "$INIT_BOOT_IMG" ] || INIT_BOOT_IMG="$ARTIFACTS_DIR/init_boot_latest.img"
[ -n "$VBMETA_IMG" ] || VBMETA_IMG="$ARTIFACTS_DIR/vbmeta_latest.img"

if [ "$INIT_BOOT_ONLY" -eq 1 ] && [ "$BOOT_ONLY" -eq 1 ]; then
  echo "ERROR: --only-init-boot and --only-boot are mutually exclusive." >&2
  exit 2
fi

if [ "$BOOT_ONLY" -eq 1 ]; then
  REQUIRED_IMAGES=("$BOOT_IMG")
elif [ "$INIT_BOOT_ONLY" -eq 1 ]; then
  REQUIRED_IMAGES=("$INIT_BOOT_IMG")
else
  REQUIRED_IMAGES=("$BOOT_IMG" "$INIT_BOOT_IMG" "$VBMETA_IMG")
  [ -n "$VENDOR_BOOT_IMG" ] && REQUIRED_IMAGES+=("$VENDOR_BOOT_IMG")
  [ -n "$DTBO_IMG" ] && REQUIRED_IMAGES+=("$DTBO_IMG")
fi

for f in "${REQUIRED_IMAGES[@]}"; do
  [ -f "$f" ] || { echo "ERROR: image not found: $f" >&2; exit 1; }
done

if [ -n "$FIRMWARE_DIR" ]; then
  [ -d "$FIRMWARE_DIR" ] || {
    echo "ERROR: firmware directory not found: $FIRMWARE_DIR" >&2
    exit 1
  }
  for part in "${FIRMWARE_PARTS[@]}"; do
    [ -f "$FIRMWARE_DIR/$part.img" ] || {
      echo "ERROR: firmware image not found: $FIRMWARE_DIR/$part.img" >&2
      exit 1
    }
  done
fi

# The 64 MiB boot image is mostly AVB padding, the real payload is inside it.
echo "boot:        $BOOT_IMG ($(stat -c%s "$BOOT_IMG") bytes)"
echo "init_boot:   $INIT_BOOT_IMG ($(stat -c%s "$INIT_BOOT_IMG") bytes)"
echo "vbmeta:      $VBMETA_IMG ($(stat -c%s "$VBMETA_IMG") bytes)"
if [ -n "$VENDOR_BOOT_IMG" ]; then
  echo "vendor_boot: $VENDOR_BOOT_IMG ($(stat -c%s "$VENDOR_BOOT_IMG") bytes)"
else
  echo "vendor_boot: (copy slot A only if vendor_boot_b is empty)"
fi
if [ -n "$DTBO_IMG" ]; then
  echo "dtbo:        $DTBO_IMG ($(stat -c%s "$DTBO_IMG") bytes)"
else
  echo "dtbo:        (copy slot A only if dtbo_b is empty)"
fi
if [ -n "$FIRMWARE_DIR" ]; then
  echo "firmware:    $FIRMWARE_DIR (${FIRMWARE_PARTS[*]})"
else
  echo "firmware:    (not provisioned)"
fi

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
  # silently run past the end of the target partition.
  part_bytes() {
    local name="$1" dev node sectors
    dev="$("${ADB[@]}" shell "su -c 'readlink -f /dev/block/by-name/$name'" 2>/dev/null | tr -d '\r')"
    node="${dev##*/}"
    sectors="$("${ADB[@]}" shell "su -c 'cat /sys/class/block/$node/size'" 2>/dev/null | tr -d '\r')"
    if [[ "$sectors" =~ ^[0-9]+$ ]]; then
      echo $((sectors * 512))
    else
      echo 0
    fi
  }

  if [ "$BOOT_ONLY" -eq 1 ]; then
    local boot_size boot_part host_md5 dev_md5
    boot_size="$(stat -c%s "$BOOT_IMG")"
    boot_part="$(part_bytes boot_b)"
    echo "boot_b size: ${boot_part:-unknown}"
    if [ "$boot_part" -gt 0 ] && [ "$boot_size" -gt "$boot_part" ]; then
      echo "ERROR: $BOOT_IMG is larger than boot_b ($boot_part bytes)." >&2
      exit 1
    fi

    if [ "$DRY_RUN" -eq 1 ]; then
      echo "[dry-run] would push and dd:"
      echo "  boot -> boot_b"
      progress 100 "Dry run complete"
      printf '%%%%PMOS_DONE\n'
      return 0
    fi

    step transfer "Pushing boot image to the tablet"
    progress 15 "Pushing boot"
    "${ADB[@]}" push "$BOOT_IMG" /data/local/tmp/pmos_boot.img

    step write "Writing boot_b"
    progress 80 "Writing boot_b"
    "${ADB[@]}" shell "su -c 'dd if=/data/local/tmp/pmos_boot.img of=/dev/block/by-name/boot_b bs=4194304 conv=fsync'"

    step verify "Verifying boot_b checksum"
    progress 92 "Verifying checksum"
    host_md5="$(md5sum "$BOOT_IMG" | awk '{print $1}')"
    dev_md5="$("${ADB[@]}" exec-out su -c "head -c $boot_size /dev/block/by-name/boot_b" | md5sum | awk '{print $1}')"
    if [ "$host_md5" != "$dev_md5" ]; then
      echo "ERROR: checksum mismatch after writing boot_b" >&2
      echo "host:   $host_md5" >&2
      echo "device: $dev_md5" >&2
      exit 1
    fi
    echo "Checksum matches."

    "${ADB[@]}" shell "rm -f /data/local/tmp/pmos_boot.img"

    step done "boot_b flashed"
    progress 100 "boot_b flashed"
    printf '%%%%PMOS_DONE\n'
    return 0
  fi

  if [ "$INIT_BOOT_ONLY" -eq 1 ]; then
    local init_size init_part host_md5 dev_md5
    init_size="$(stat -c%s "$INIT_BOOT_IMG")"
    init_part="$(part_bytes init_boot_b)"
    echo "init_boot_b size: ${init_part:-unknown}"
    if [ "$init_part" -gt 0 ] && [ "$init_size" -gt "$init_part" ]; then
      echo "ERROR: $INIT_BOOT_IMG is larger than init_boot_b ($init_part bytes)." >&2
      exit 1
    fi

    if [ "$DRY_RUN" -eq 1 ]; then
      echo "[dry-run] would push and dd:"
      echo "  init_boot -> init_boot_b"
      progress 100 "Dry run complete"
      printf '%%%%PMOS_DONE\n'
      return 0
    fi

    step transfer "Pushing init_boot image to the tablet"
    progress 15 "Pushing init_boot"
    "${ADB[@]}" push "$INIT_BOOT_IMG" /data/local/tmp/pmos_init_boot.img

    step write "Writing init_boot_b"
    progress 80 "Writing init_boot_b"
    "${ADB[@]}" shell "su -c 'dd if=/data/local/tmp/pmos_init_boot.img of=/dev/block/by-name/init_boot_b bs=4194304 conv=fsync'"

    step verify "Verifying init_boot_b checksum"
    progress 92 "Verifying checksum"
    host_md5="$(md5sum "$INIT_BOOT_IMG" | awk '{print $1}')"
    dev_md5="$("${ADB[@]}" exec-out su -c "head -c $init_size /dev/block/by-name/init_boot_b" | md5sum | awk '{print $1}')"
    if [ "$host_md5" != "$dev_md5" ]; then
      echo "ERROR: checksum mismatch after writing init_boot_b" >&2
      echo "host:   $host_md5" >&2
      echo "device: $dev_md5" >&2
      exit 1
    fi
    echo "Checksum matches."

    "${ADB[@]}" shell "rm -f /data/local/tmp/pmos_init_boot.img"

    step done "init_boot_b flashed"
    progress 100 "init_boot_b flashed"
    printf '%%%%PMOS_DONE\n'
    return 0
  fi

  local boot_size init_size vb_size vendor_size=0 dtbo_size=0
  local boot_part init_part vendor_part=0 dtbo_part=0
  boot_size="$(stat -c%s "$BOOT_IMG")"
  init_size="$(stat -c%s "$INIT_BOOT_IMG")"
  vb_size="$(stat -c%s "$VBMETA_IMG")"
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

  if [ -n "$VENDOR_BOOT_IMG" ]; then
    vendor_size="$(stat -c%s "$VENDOR_BOOT_IMG")"
    vendor_part="$(part_bytes vendor_boot_b)"
    echo "vendor_boot_b size: ${vendor_part:-unknown}"
    if [ "$vendor_part" -gt 0 ] && [ "$vendor_size" -gt "$vendor_part" ]; then
      echo "ERROR: $VENDOR_BOOT_IMG is larger than vendor_boot_b ($vendor_part bytes)." >&2
      exit 1
    fi
  fi
  if [ -n "$DTBO_IMG" ]; then
    dtbo_size="$(stat -c%s "$DTBO_IMG")"
    dtbo_part="$(part_bytes dtbo_b)"
    echo "dtbo_b size: ${dtbo_part:-unknown}"
    if [ "$dtbo_part" -gt 0 ] && [ "$dtbo_size" -gt "$dtbo_part" ]; then
      echo "ERROR: $DTBO_IMG is larger than dtbo_b ($dtbo_part bytes)." >&2
      exit 1
    fi
  fi
  if [ -n "$FIRMWARE_DIR" ]; then
    local part fw_size fw_part
    for part in "${FIRMWARE_PARTS[@]}"; do
      fw_size="$(stat -c%s "$FIRMWARE_DIR/$part.img")"
      fw_part="$(part_bytes "${part}_b")"
      if [ "$fw_part" -gt 0 ] && [ "$fw_size" -gt "$fw_part" ]; then
        echo "ERROR: $FIRMWARE_DIR/$part.img is larger than ${part}_b ($fw_part bytes)." >&2
        exit 1
      fi
    done
  fi

  if [ "$DRY_RUN" -eq 1 ]; then
    echo "[dry-run] would push and dd:"
    echo "  vbmeta -> vbmeta_b, vbmeta_system_b, vbmeta_vendor_b"
    echo "  boot -> boot_b"
    echo "  init_boot -> init_boot_b"
    if [ -n "$VENDOR_BOOT_IMG" ]; then
      echo "  vendor_boot -> vendor_boot_b"
    else
      echo "  vendor_boot_b <- slot A only if empty"
    fi
    if [ -n "$DTBO_IMG" ]; then
      echo "  dtbo -> dtbo_b"
    else
      echo "  dtbo_b <- slot A only if empty"
    fi
    if [ -n "$FIRMWARE_DIR" ]; then
      for part in "${FIRMWARE_PARTS[@]}"; do
        echo "  $part.img -> ${part}_b"
      done
    fi
    progress 100 "Dry run complete"
    printf '%%%%PMOS_DONE\n'
    return 0
  fi

  step transfer "Pushing boot images to the tablet"
  progress 15 "Pushing images"
  "${ADB[@]}" push "$BOOT_IMG" /data/local/tmp/pmos_boot.img
  "${ADB[@]}" push "$INIT_BOOT_IMG" /data/local/tmp/pmos_init_boot.img
  "${ADB[@]}" push "$VBMETA_IMG" /data/local/tmp/pmos_vbmeta.img
  if [ -n "$VENDOR_BOOT_IMG" ]; then
    "${ADB[@]}" push "$VENDOR_BOOT_IMG" /data/local/tmp/pmos_vendor_boot.img
  fi
  if [ -n "$DTBO_IMG" ]; then
    "${ADB[@]}" push "$DTBO_IMG" /data/local/tmp/pmos_dtbo.img
  fi
  if [ -n "$FIRMWARE_DIR" ]; then
    for part in "${FIRMWARE_PARTS[@]}"; do
      "${ADB[@]}" push "$FIRMWARE_DIR/$part.img" "/data/local/tmp/pmos_fw_$part.img"
    done
  fi

  if [ -z "$VENDOR_BOOT_IMG" ] || [ -z "$DTBO_IMG" ]; then
    step provision "Ensuring slot-B vendor_boot and dtbo are populated"
    # An all-zero 16-byte prefix means the partition is still erased.  Use od
    # instead of tr so NUL handling does not depend on the device shell.
    if [ -z "$VENDOR_BOOT_IMG" ]; then
      "${ADB[@]}" shell "su -c '
        zeros=00000000000000000000000000000000
        if [ \"\$(dd if=/dev/block/by-name/vendor_boot_b bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d \" \\n\")\" = \"\$zeros\" ]; then
          echo \"Provisioning vendor_boot_b from slot A\"
          dd if=/dev/block/by-name/vendor_boot_a of=/dev/block/by-name/vendor_boot_b bs=4194304 conv=fsync
        fi
      '"
    fi
    if [ -z "$DTBO_IMG" ]; then
      "${ADB[@]}" shell "su -c '
        zeros=00000000000000000000000000000000
        if [ \"\$(dd if=/dev/block/by-name/dtbo_b bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d \" \\n\")\" = \"\$zeros\" ]; then
          echo \"Provisioning dtbo_b from slot A\"
          dd if=/dev/block/by-name/dtbo_a of=/dev/block/by-name/dtbo_b bs=4194304 conv=fsync
        fi
      '"
    fi
  fi

  if [ -n "$FIRMWARE_DIR" ]; then
    step firmware "Writing slot-B firmware"
    local idx=0
    for part in "${FIRMWARE_PARTS[@]}"; do
      progress $((30 + idx)) "Writing ${part}_b"
      "${ADB[@]}" shell "su -c 'dd if=/data/local/tmp/pmos_fw_$part.img of=/dev/block/by-name/${part}_b bs=4096 conv=fsync'"
      idx=$((idx + 1))
    done
  fi

  if [ -n "$DTBO_IMG" ]; then
    step dtbo "Writing dtbo_b"
    progress 45 "Writing dtbo_b"
    "${ADB[@]}" shell "su -c 'dd if=/data/local/tmp/pmos_dtbo.img of=/dev/block/by-name/dtbo_b bs=4194304 conv=fsync'"
  fi

  if [ -n "$VENDOR_BOOT_IMG" ]; then
    step vendor_boot "Writing vendor_boot_b"
    progress 55 "Writing vendor_boot_b"
    "${ADB[@]}" shell "su -c 'dd if=/data/local/tmp/pmos_vendor_boot.img of=/dev/block/by-name/vendor_boot_b bs=4194304 conv=fsync'"
  fi

  step write "Writing slot B"
  progress 65 "Writing vbmeta"
  "${ADB[@]}" shell "su -c 'dd if=/data/local/tmp/pmos_vbmeta.img of=/dev/block/by-name/vbmeta_b bs=4096 conv=fsync'"
  "${ADB[@]}" shell "su -c 'dd if=/data/local/tmp/pmos_vbmeta.img of=/dev/block/by-name/vbmeta_system_b bs=4096 conv=fsync'"
  "${ADB[@]}" shell "su -c 'dd if=/data/local/tmp/pmos_vbmeta.img of=/dev/block/by-name/vbmeta_vendor_b bs=4096 conv=fsync'"

  progress 75 "Writing boot_b"
  "${ADB[@]}" shell "su -c 'dd if=/data/local/tmp/pmos_boot.img of=/dev/block/by-name/boot_b bs=4194304 conv=fsync'"

  progress 85 "Writing init_boot_b"
  "${ADB[@]}" shell "su -c 'dd if=/data/local/tmp/pmos_init_boot.img of=/dev/block/by-name/init_boot_b bs=4194304 conv=fsync'"

  step verify "Verifying slot-B checksums"
  progress 92 "Verifying checksums"
  # Compare exactly the bytes we wrote.  Reading the whole partition would
  # include erased tail bytes and produce a false mismatch for images that
  # are smaller than their partition.  exec-out avoids adb's PTY newline
  # translation, which would corrupt a binary checksum stream.
  local -a verify_imgs=("$BOOT_IMG" "$INIT_BOOT_IMG" "$VBMETA_IMG")
  local -a verify_parts=(boot_b init_boot_b vbmeta_b)
  if [ -n "$VENDOR_BOOT_IMG" ]; then
    verify_imgs+=("$VENDOR_BOOT_IMG")
    verify_parts+=(vendor_boot_b)
  fi
  if [ -n "$DTBO_IMG" ]; then
    verify_imgs+=("$DTBO_IMG")
    verify_parts+=(dtbo_b)
  fi
  if [ -n "$FIRMWARE_DIR" ]; then
    for part in "${FIRMWARE_PARTS[@]}"; do
      verify_imgs+=("$FIRMWARE_DIR/$part.img")
      verify_parts+=("${part}_b")
    done
  fi

  local i host_md5 dev_md5 size target
  for i in "${!verify_imgs[@]}"; do
    size="$(stat -c%s "${verify_imgs[$i]}")"
    target="${verify_parts[$i]}"
    host_md5="$(md5sum "${verify_imgs[$i]}" | awk '{print $1}')"
    dev_md5="$("${ADB[@]}" exec-out su -c "head -c $size /dev/block/by-name/$target" | md5sum | awk '{print $1}')"
    if [ "$host_md5" != "$dev_md5" ]; then
      echo "ERROR: checksum mismatch after writing $target" >&2
      echo "host:   $host_md5" >&2
      echo "device: $dev_md5" >&2
      exit 1
    fi
  done
  echo "Checksums match."

  "${ADB[@]}" shell "rm -f /data/local/tmp/pmos_boot.img /data/local/tmp/pmos_init_boot.img /data/local/tmp/pmos_vbmeta.img /data/local/tmp/pmos_vendor_boot.img /data/local/tmp/pmos_dtbo.img /data/local/tmp/pmos_fw_*.img"

  step done "Slot B boot chain flashed"
  progress 100 "Slot B boot chain flashed"
  printf '%%%%PMOS_DONE\n'

  if [ "$REBOOT" -eq 1 ]; then
    echo "Rebooting..."
    "${ADB[@]}" reboot
  fi
}

flash_fastboot() {
  if [ "$BOOT_ONLY" -eq 1 ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
      echo "[dry-run] would run:"
      echo "  fastboot flash boot_b $BOOT_IMG"
      progress 100 "Dry run complete"
      printf '%%%%PMOS_DONE\n'
      return 0
    fi

    step write "Writing boot_b via fastboot"
    progress 85 "Flashing boot_b"
    "${FB[@]}" flash boot_b "$BOOT_IMG"

    step done "boot_b flashed"
    progress 100 "boot_b flashed"
    printf '%%%%PMOS_DONE\n'
    return 0
  fi

  if [ "$INIT_BOOT_ONLY" -eq 1 ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
      echo "[dry-run] would run:"
      echo "  fastboot flash init_boot_b $INIT_BOOT_IMG"
      progress 100 "Dry run complete"
      printf '%%%%PMOS_DONE\n'
      return 0
    fi

    step write "Writing init_boot_b via fastboot"
    progress 85 "Flashing init_boot_b"
    "${FB[@]}" flash init_boot_b "$INIT_BOOT_IMG"

    step done "init_boot_b flashed"
    progress 100 "init_boot_b flashed"
    printf '%%%%PMOS_DONE\n'
    return 0
  fi

  step verify "Checking fastboot state"
  "${FB[@]}" getvar current-slot 2>&1 | sed 's/^/  /' || true

  if [ "$DRY_RUN" -eq 1 ]; then
    echo "[dry-run] would run:"
    echo "  fastboot flash boot_b $BOOT_IMG"
    echo "  fastboot flash init_boot_b $INIT_BOOT_IMG"
    echo "  fastboot flash vbmeta_b $VBMETA_IMG"
    echo "  fastboot flash vbmeta_system_b $VBMETA_IMG"
    echo "  fastboot flash vbmeta_vendor_b $VBMETA_IMG"
    if [ -n "$VENDOR_BOOT_IMG" ]; then
      echo "  fastboot flash vendor_boot_b $VENDOR_BOOT_IMG"
    fi
    if [ -n "$DTBO_IMG" ]; then
      echo "  fastboot flash dtbo_b $DTBO_IMG"
    fi
    if [ -n "$FIRMWARE_DIR" ]; then
      for part in "${FIRMWARE_PARTS[@]}"; do
        echo "  fastboot flash ${part}_b $FIRMWARE_DIR/$part.img"
      done
    fi
    progress 100 "Dry run complete"
    printf '%%%%PMOS_DONE\n'
    return 0
  fi

  if [ -n "$FIRMWARE_DIR" ]; then
    step firmware "Writing slot-B firmware via fastboot"
    progress 20 "Flashing slot-B firmware"
    for part in "${FIRMWARE_PARTS[@]}"; do
      "${FB[@]}" flash "${part}_b" "$FIRMWARE_DIR/$part.img"
    done
  fi

  step write "Writing slot B via fastboot"
  progress 35 "Flashing vbmeta"
  "${FB[@]}" flash vbmeta_b "$VBMETA_IMG"
  "${FB[@]}" flash vbmeta_system_b "$VBMETA_IMG"
  "${FB[@]}" flash vbmeta_vendor_b "$VBMETA_IMG"

  if [ -n "$VENDOR_BOOT_IMG" ]; then
    progress 45 "Flashing vendor_boot_b"
    "${FB[@]}" flash vendor_boot_b "$VENDOR_BOOT_IMG"
  fi
  if [ -n "$DTBO_IMG" ]; then
    progress 55 "Flashing dtbo_b"
    "${FB[@]}" flash dtbo_b "$DTBO_IMG"
  fi

  progress 70 "Flashing boot_b"
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
