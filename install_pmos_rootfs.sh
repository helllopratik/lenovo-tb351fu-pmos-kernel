#!/usr/bin/env bash
# ==============================================================================
# Master postmarketOS RootFS Installer & Provisioner for Lenovo Tab Plus (TB351FU)
# Author: helllopratik <pratikgondane07@gmail.com>
#
# Provisions a complete, clean 64GB postmarketOS (Phosh) rootfs on UFS userdata
# (/data/pmos/rootfs.img) for dual-booting alongside Android.
#
# Preserves merged /usr symlinks, deploys out-of-tree hardware modules, installs
# Wi-Fi / Bluetooth bring-up services, installs the 60/90/adaptive display mode
# panel, and verifies systemd executable integrity inside chroot before completion.
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPLOY_DIR="$SCRIPT_DIR/deploy"
TMP_DIR="${TMPDIR:-/var/tmp}/pmos_install_tmp"

usage() {
  cat <<EOF
Usage: $0 [OPTIONS] [IMAGE]

Install a postmarketOS rootfs image into /data/pmos/rootfs.img on the
Lenovo Tab Plus (TB351FU) while Android is running.

  -i, --image PATH   RootFS image (.img or .img.zst). Auto-detected when omitted.
  -s, --size SIZE    Size of the loopback image on the tablet (default: 64G).
                     Accepts K/M/G/T suffixes, e.g. 32G, 64G.
      --yes          Reserved for non-interactive callers (no prompts today).
  -h, --help         Show this help.

Environment:
  PMOS_IMG           Same as --image.
  ROOTFS_SIZE        Same as --size.
  ANDROID_SERIAL     Target a specific adb device.
EOF
}

step() { printf '%%%%PMOS_STEP:%s:%s\n' "$1" "$2"; }
progress() { printf '%%%%PMOS_PROGRESS:%s:%s\n' "$1" "$2"; }
trap 'rc=$?; echo "ERROR: rootfs installer failed at line $LINENO (exit $rc)" >&2; printf "%%%%PMOS_ERROR:%s\n" "$rc"' ERR

# Machine-readable protocol check used by the installer self-test.
if [ "${PMOS_MARKER_TEST:-}" = "1" ]; then
  step selftest ok
  progress 50 selftest
  printf '%%%%PMOS_DONE\n'
  exit 0
elif [ "${PMOS_MARKER_TEST:-}" = "error" ]; then
  false
fi

ROOTFS_SIZE="${ROOTFS_SIZE:-64G}"
PMOS_IMG="${PMOS_IMG:-}"

while [ "$#" -gt 0 ]; do
  case "$1" in
    -i|--image)
      [ "$#" -ge 2 ] || { echo "ERROR: --image needs a path" >&2; exit 2; }
      PMOS_IMG="$2"; shift 2 ;;
    -s|--size)
      [ "$#" -ge 2 ] || { echo "ERROR: --size needs a value" >&2; exit 2; }
      ROOTFS_SIZE="$2"; shift 2 ;;
    --yes) shift ;;
    -h|--help) usage; exit 0 ;;
    --) shift; break ;;
    -*) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
    *) PMOS_IMG="$1"; shift ;;
  esac
done

if ! [[ "$ROOTFS_SIZE" =~ ^[0-9]+[KkMmGgTt]?$ ]]; then
  echo "ERROR: invalid --size '$ROOTFS_SIZE' (use e.g. 32G or 64G)" >&2
  exit 2
fi

ROOTFS_SIZE_BYTES="$(python3 -c '
import sys
s = sys.argv[1].upper()
mult = {"K": 1024, "M": 1024**2, "G": 1024**3, "T": 1024**4}
print(int(s[:-1]) * mult[s[-1]] if s[-1] in mult else int(s))
' "$ROOTFS_SIZE")"

if [ "$ROOTFS_SIZE_BYTES" -lt 2147483648 ]; then
  echo "ERROR: --size must be at least 2G" >&2
  exit 2
fi

echo "========================================================================"
echo " postmarketOS UFS Loopback RootFS Installer (Lenovo Tab Plus TB351FU)"
echo "========================================================================"
echo "RootFS size: $ROOTFS_SIZE ($ROOTFS_SIZE_BYTES bytes)"

step check "Checking host tools and deploy payload"
progress 2 "Checking host tools"

# 1. Check prerequisites
command -v adb >/dev/null 2>&1 || { echo "ERROR: adb is required but not installed."; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "ERROR: python3 is required but not installed."; exit 1; }
command -v zstd >/dev/null 2>&1 || { echo "ERROR: zstd is required but not installed."; exit 1; }

if [ ! -d "$DEPLOY_DIR" ] || [ ! -f "$DEPLOY_DIR/modules.tar.xz" ]; then
  echo "ERROR: Deploy directory or modules archive missing at $DEPLOY_DIR"
  exit 1
fi

# 2. Check device connectivity via ADB
step device "Waiting for an Android device over ADB"
progress 5 "Checking ADB connection"
echo "Checking ADB device connection..."
if ! adb get-state 1>/dev/null 2>&1; then
  echo "ERROR: Device not found. Ensure tablet is booted into Android with USB debugging enabled."
  exit 1
fi

if [ -n "${ANDROID_SERIAL:-}" ]; then
  DEVICE_ID="$ANDROID_SERIAL"
else
  DEVICE_ID=$(adb devices | grep -w "device" | head -n 1 | awk '{print $1}')
  export ANDROID_SERIAL="$DEVICE_ID"
fi
if [ -z "${DEVICE_ID:-}" ]; then
  echo "ERROR: no adb device in 'device' state."
  exit 1
fi
echo "Found device: $DEVICE_ID"

# Verify root ADB access
adb shell "su -c 'id'" | grep -q "uid=0" || {
  echo "ERROR: Root ADB access required. Grant su permissions in Magisk/KernelSU."
  exit 1
}
echo "Root ADB access confirmed."
progress 10 "Root access confirmed"

# 3. Locate source postmarketOS image
step image "Preparing the postmarketOS rootfs image"
if [ -z "$PMOS_IMG" ]; then
  # Auto-detect candidates
  CANDIDATES=(
    "$SCRIPT_DIR/../pmos-phosh-aarch64.img"
    "$HOME/Desktop/pmos/pmos-phosh-aarch64.img"
    "/mnt/Nvme/pmos_rootfs_base.img"
    "/mnt/Nvme/pmos_rootfs_base.img.zst"
  )
  for c in "${CANDIDATES[@]}"; do
    if [ -f "$c" ]; then
      PMOS_IMG="$c"
      break
    fi
  done
fi

if [ -z "$PMOS_IMG" ] || [ ! -f "$PMOS_IMG" ]; then
  echo "Usage: $0 /path/to/pmos-phosh-aarch64.img"
  echo "ERROR: Source postmarketOS image not specified or found."
  exit 1
fi

echo "Source image: $PMOS_IMG"

# Refuse to create an image that would leave Android without breathing room.
# Android toybox does not support `df -B1`; use -k (1 KiB blocks) and let a
# failed probe fall through instead of tripping `set -e`.
DATA_AVAIL_KB=$(adb shell 'df -k /data' 2>/dev/null | awk 'NR==2 {print $4}' | tr -d '\r' || true)
if ! [[ "$DATA_AVAIL_KB" =~ ^[0-9]+$ ]]; then
  DATA_AVAIL_KB=$(adb shell 'df /data' 2>/dev/null | awk 'NR==2 {print $4}' | tr -d '\r' || true)
fi
if [[ "$DATA_AVAIL_KB" =~ ^[0-9]+$ ]]; then
  DATA_AVAIL=$((DATA_AVAIL_KB * 1024))
  echo "Free space on /data: $DATA_AVAIL bytes"
  if [ "$DATA_AVAIL" -lt $((ROOTFS_SIZE_BYTES + 1073741824)) ]; then
    echo "ERROR: a $ROOTFS_SIZE rootfs would leave less than 1 GiB free on /data." >&2
    exit 1
  fi
else
  echo "WARNING: could not read free space on /data; continuing." >&2
fi
progress 12 "Rootfs image selected"
mkdir -p "$TMP_DIR"

# 4. Prepare base rootfs image
# Android has no zstd binary, so decompress/extract on the host and push a
# raw ext4 image.  The image is only moved into /data/pmos after transfer.
ROOTFS_RAW="$TMP_DIR/rootfs_base.img"
if [[ "$PMOS_IMG" == *.zst ]]; then
  echo "Decompressing zstd image on the host: $PMOS_IMG"
  zstd -d -f "$PMOS_IMG" -o "$ROOTFS_RAW"
elif [[ "$PMOS_IMG" == *.img ]]; then
  # Check if GPT disk image or a raw ext4 partition.
  if python3 - "$PMOS_IMG" <<'PY'
import sys
with open(sys.argv[1], "rb") as f:
    f.seek(512)
    sys.exit(0 if f.read(8) == b"EFI PART" else 1)
PY
  then
    echo "Detected GPT disk image. Extracting partition 2 (rootfs)..."
    # Parse the GPT directly instead of relying on fdisk, whose output format
    # and availability differ between hosts.  The rootfs is partition 2.
    python3 - "$PMOS_IMG" "$ROOTFS_RAW" <<'PY'
import struct
import subprocess
import sys

image, output = sys.argv[1], sys.argv[2]
with open(image, "rb") as fh:
    fh.seek(512)
    header = fh.read(92)
    if len(header) < 92 or header[:8] != b"EFI PART":
        sys.exit("not a GPT image")
    entries_lba, count, entry_size = struct.unpack_from("<QII", header, 72)
    if entry_size < 128 or count > 256:
        sys.exit("unsupported GPT layout")
    fh.seek(entries_lba * 512)
    entries = fh.read(count * entry_size)

partitions = []
for index in range(count):
    entry = entries[index * entry_size:(index + 1) * entry_size]
    if len(entry) < 128 or entry[:16] == b"\x00" * 16:
        continue
    first, last = struct.unpack_from("<QQ", entry, 32)
    partitions.append((index + 1, first, last))

if len(partitions) < 2:
    sys.exit("expected at least two GPT partitions")

_, first, sectors = partitions[1]
count_sectors = sectors - first + 1
print(f"Extracting partition 2: skip={first} count={count_sectors}")
with open(output, "wb") as out:
    dd = subprocess.Popen(
        ["dd", f"if={image}", "bs=512", f"skip={first}",
         f"count={count_sectors}"],
        stdout=out,
    )
    rc = dd.wait()
sys.exit(rc)
PY
  else
    echo "Detected raw ext4 partition. Copying to a temporary image..."
    cp -f "$PMOS_IMG" "$ROOTFS_RAW"
  fi
else
  echo "ERROR: unsupported rootfs image type: $PMOS_IMG" >&2
  exit 1
fi

if [ ! -f "$ROOTFS_RAW" ]; then
  echo "ERROR: prepared rootfs image not found: $ROOTFS_RAW" >&2
  exit 1
fi

# Android 13/14 ship e2fsprogs 1.46, which refuses filesystems with the newer
# orphan_file feature and therefore cannot resize the image.  Clear it on the
# host; the filesystem remains fully compatible with the pmOS kernel.
if ! command -v tune2fs >/dev/null 2>&1; then
  echo "WARNING: tune2fs not found; Android may not be able to resize this image." >&2
elif tune2fs -l "$ROOTFS_RAW" 2>/dev/null | grep -qw orphan_file; then
  echo "Clearing ext4 orphan_file for Android e2fsprogs compatibility..."
  tune2fs -O ^orphan_file "$ROOTFS_RAW" || {
    echo "ERROR: could not clear orphan_file from $ROOTFS_RAW" >&2
    exit 1
  }
fi

echo "Base rootfs image ready: $(ls -lh "$ROOTFS_RAW" | awk '{print $5}')"

# 5. Build customization payload package
step payload "Building the device customization payload"
progress 25 "Packaging modules, services and configs"
echo "Packaging system customizations (modules, services, scripts, configs)..."
PAYLOAD_DIR="$TMP_DIR/payload"
rm -rf "$PAYLOAD_DIR"
mkdir -p "$PAYLOAD_DIR/usr/lib/modules" \
         "$PAYLOAD_DIR/usr/local/bin" \
         "$PAYLOAD_DIR/usr/bin" \
         "$PAYLOAD_DIR/root" \
         "$PAYLOAD_DIR/etc/systemd/system/multi-user.target.wants" \
         "$PAYLOAD_DIR/etc/udev/rules.d" \
         "$PAYLOAD_DIR/etc/phosh" \
         "$PAYLOAD_DIR/etc/sysctl.d" \
         "$PAYLOAD_DIR/etc/modprobe.d" \
         "$PAYLOAD_DIR/etc/systemd/system.conf.d" \
         "$PAYLOAD_DIR/etc/nftables.d" \
         "$PAYLOAD_DIR/usr/share/glib-2.0/schemas" \
         "$PAYLOAD_DIR/usr/share/postmarketos-tweaks" \
         "$PAYLOAD_DIR/data_host"

# Extract out-of-tree hardware modules into usr/lib/modules/extra
mkdir -p "$PAYLOAD_DIR/usr/lib/modules/extra"
tar -xf "$DEPLOY_DIR/modules.tar.xz" -C "$PAYLOAD_DIR/usr/lib/modules/extra"

# Built-in module index for the target kernel.  zram is built in, but
# zramstart calls modprobe; without modules.builtin + depmod, kmod
# reports "Module zram not found" and the script aborts under set -e.
BUILTIN_STAGE="$SCRIPT_DIR/../artifacts/modules_builtin"
if [ -f "$BUILTIN_STAGE/kernel.release" ]; then
  KREL="$(cat "$BUILTIN_STAGE/kernel.release")"
  mkdir -p "$PAYLOAD_DIR/usr/lib/modules/$KREL"
  cp "$BUILTIN_STAGE/$KREL/modules.builtin" \
     "$BUILTIN_STAGE/$KREL/modules.builtin.modinfo" \
     "$PAYLOAD_DIR/usr/lib/modules/$KREL/"
  echo "$KREL" > "$PAYLOAD_DIR/usr/lib/modules/.pmos_kernel_release"
fi

# Extract proprietary vendor firmware into usr/lib/firmware
if [ -f "$DEPLOY_DIR/firmware.tar.xz" ]; then
  mkdir -p "$PAYLOAD_DIR/usr/lib/firmware"
  tar -xf "$DEPLOY_DIR/firmware.tar.xz" -C "$PAYLOAD_DIR/usr/lib/firmware"
  # Also create convenience symlink for aw88395_acf.bin
  if [ -f "$PAYLOAD_DIR/usr/lib/firmware/aw883xx_acf.bin" ]; then
    ln -sf aw883xx_acf.bin "$PAYLOAD_DIR/usr/lib/firmware/aw88395_acf.bin"
  fi
  # Link /vendor/firmware and /etc/firmware
  mkdir -p "$PAYLOAD_DIR/vendor" "$PAYLOAD_DIR/etc"
  ln -sf /usr/lib/firmware "$PAYLOAD_DIR/vendor/firmware"
  ln -sf /usr/lib/firmware "$PAYLOAD_DIR/etc/firmware"
fi

# Copy ALSA UCM2 configurations for MT8186 audio
if [ -d "$DEPLOY_DIR/configs/ucm2" ]; then
  mkdir -p "$PAYLOAD_DIR/usr/share/alsa/ucm2"
  cp -a "$DEPLOY_DIR/configs/ucm2/"* "$PAYLOAD_DIR/usr/share/alsa/ucm2/"
fi

# Copy local bin scripts
cp -a "$DEPLOY_DIR/scripts/"* "$PAYLOAD_DIR/usr/local/bin/"
chmod 755 "$PAYLOAD_DIR/usr/local/bin/"*
if [ -f "$PAYLOAD_DIR/usr/local/bin/phosh-session" ]; then
  mv "$PAYLOAD_DIR/usr/local/bin/phosh-session" "$PAYLOAD_DIR/usr/bin/phosh-session"
fi

# Copy root scripts
cp -a "$DEPLOY_DIR/root_scripts/"* "$PAYLOAD_DIR/root/"

# Copy systemd services and wire symlinks
for s in "$DEPLOY_DIR/services/"*.service; do
  [ -f "$s" ] || continue
  sname=$(basename "$s")
  cp "$s" "$PAYLOAD_DIR/etc/systemd/system/$sname"
  ln -sf "/etc/systemd/system/$sname" "$PAYLOAD_DIR/etc/systemd/system/multi-user.target.wants/$sname"
done

# Copy service drop-in overrides (e.g. upower.service.d)
if [ -d "$DEPLOY_DIR/services/upower.service.d" ]; then
  mkdir -p "$PAYLOAD_DIR/etc/systemd/system/upower.service.d"
  cp -a "$DEPLOY_DIR/services/upower.service.d/"* "$PAYLOAD_DIR/etc/systemd/system/upower.service.d/"
  mkdir -p "$PAYLOAD_DIR/etc/systemd/system/graphical.target.wants"
  ln -sf "/usr/lib/systemd/system/upower.service" "$PAYLOAD_DIR/etc/systemd/system/graphical.target.wants/upower.service"
fi

# Copy configurations
cp "$DEPLOY_DIR/configs/70-touchscreen-pen.rules" "$PAYLOAD_DIR/etc/udev/rules.d/"
cp "$DEPLOY_DIR/configs/phoc.ini" "$PAYLOAD_DIR/etc/phosh/phoc.ini"
cp "$DEPLOY_DIR/configs/99-panic.conf" "$PAYLOAD_DIR/etc/sysctl.d/99-panic.conf"
# Must sort after 99-panic.conf so kernel faults do not auto-reboot.
cp "$DEPLOY_DIR/configs/99-zz-no-auto-reboot.conf" "$PAYLOAD_DIR/etc/sysctl.d/99-zz-no-auto-reboot.conf"
# Camera modules stay manual-load only.
cp "$DEPLOY_DIR/configs/90-camera-manual.conf" "$PAYLOAD_DIR/etc/modprobe.d/90-camera-manual.conf"
cp "$DEPLOY_DIR/configs/watchdog.conf" "$PAYLOAD_DIR/etc/systemd/system.conf.d/watchdog.conf"

# Display mode: GSettings schema + Mobile Settings (phosh-mobile-settings)
# YAML panel.  The daemon applies 60/90/adaptive to pmosfb.refresh_hz.
cp "$DEPLOY_DIR/configs/org.postmarketos.Display.gschema.xml" \
   "$PAYLOAD_DIR/usr/share/glib-2.0/schemas/"
cp "$DEPLOY_DIR/configs/postmarketos-tweaks/display.yml" \
   "$PAYLOAD_DIR/usr/share/postmarketos-tweaks/"

# zram swap choice: GSettings schema + Mobile Settings panel.  The
# pmos-zram-mode daemon starts/stops postmarketos-zram-swap.service.
cp "$DEPLOY_DIR/configs/org.postmarketos.Zram.gschema.xml" \
   "$PAYLOAD_DIR/usr/share/glib-2.0/schemas/"
cp "$DEPLOY_DIR/configs/postmarketos-tweaks/zram.yml" \
   "$PAYLOAD_DIR/usr/share/postmarketos-tweaks/"

# nftables: allow DHCPv4 client replies so the default-drop ruleset
# cannot break DHCP.  SSH is already allowed by 50_sshd.nft.
cp "$DEPLOY_DIR/configs/nftables.d/10_dhcp-client.nft" \
   "$PAYLOAD_DIR/etc/nftables.d/"

# Copy ALSA UCM2 audio configurations
if [ -d "$DEPLOY_DIR/configs/ucm2" ]; then
  mkdir -p "$PAYLOAD_DIR/usr/share/alsa/ucm2"
  cp -a "$DEPLOY_DIR/configs/ucm2/"* "$PAYLOAD_DIR/usr/share/alsa/ucm2/"
fi

PAYLOAD_TAR="$TMP_DIR/pmos_payload.tar.gz"
tar -czf "$PAYLOAD_TAR" -C "$PAYLOAD_DIR" .
echo "Customization payload ready: $(ls -lh "$PAYLOAD_TAR" | awk '{print $5}')"
progress 35 "Customization payload ready"

# 6. Push archives to device
step transfer "Pushing rootfs and payload to the tablet"
progress 38 "Transferring rootfs to the tablet"
echo "Pushing rootfs base image and customizations to device..."
adb push "$ROOTFS_RAW" /data/local/tmp/pmos_base.img
adb push "$PAYLOAD_TAR" /data/local/tmp/pmos_payload.tar.gz
progress 55 "Transfer complete"

# 7. Unpack and configure on device
step unpack "Unpacking and resizing the rootfs on the tablet"
progress 60 "Preparing rootfs on the tablet"
echo "Unpacking rootfs, resizing to $ROOTFS_SIZE, and applying customizations on device..."
adb shell "su -c '
  set -e
  mkdir -p /data/pmos

  LOOP=\"\"
  cleanup() {
    umount /data/local/tmp/chk 2>/dev/null || true
    if [ -n \"\$LOOP\" ]; then
      losetup -d \"\$LOOP\" 2>/dev/null || true
    fi
    chcon u:object_r:system_data_file:s0 /data/pmos/rootfs.img 2>/dev/null || true
  }
  trap cleanup EXIT

  echo \"%%PMOS_STEP:unpack:Unpacking rootfs on the tablet\"
  echo \"Moving base image to /data/pmos/rootfs.img...\"
  # Same filesystem: rename is atomic and keeps the old image until the new
  # one has transferred successfully.
  mv -f /data/local/tmp/pmos_base.img /data/pmos/rootfs.img
  rm -f /data/local/tmp/pmos_base.img.zst

  # SELinux denies the kernel write access to system_data_file, so use the
  # media_rw type for the read-write loop mount and restore the stricter type
  # in the EXIT trap.
  chcon u:object_r:media_rw_data_file:s0 /data/pmos/rootfs.img || {
    echo \"ERROR: could not label the rootfs for a read-write loop mount.\"
    exit 1
  }

  echo \"%%PMOS_PROGRESS:68:Resizing rootfs to $ROOTFS_SIZE\"
  echo \"Expanding filesystem to $ROOTFS_SIZE...\"
  truncate -s \"$ROOTFS_SIZE_BYTES\" /data/pmos/rootfs.img
  /system/bin/e2fsck -f -y /data/pmos/rootfs.img || true
  /system/bin/resize2fs /data/pmos/rootfs.img

  echo \"%%PMOS_PROGRESS:72:Mounting rootfs image\"
  echo \"Loopback mounting rootfs...\"
  mkdir -p /data/local/tmp/chk
  LOOP=\$(losetup -f /data/pmos/rootfs.img --show)
  mount -t ext4 \$LOOP /data/local/tmp/chk

  echo \"%%PMOS_STEP:payload-device:Applying the customization payload\"
  echo \"%%PMOS_PROGRESS:78:Applying modules, services and configs\"
  echo \"Applying customization payload...\"
  tar -xzf /data/local/tmp/pmos_payload.tar.gz -C /data/local/tmp/chk
  rm -f /data/local/tmp/pmos_payload.tar.gz

  echo \"Normalizing system directory ownership...\"
  chown 0:0 /data/local/tmp/chk /data/local/tmp/chk/etc /data/local/tmp/chk/usr /data/local/tmp/chk/var /data/local/tmp/chk/root 2>/dev/null || true
  chmod 755 /data/local/tmp/chk /data/local/tmp/chk/etc /data/local/tmp/chk/usr 2>/dev/null || true

  echo \"Enforcing merged /usr symlink invariants...\"
  # CRITICAL: /lib, /bin, and /sbin MUST be symbolic links into /usr
  if [ ! -L /data/local/tmp/chk/lib ]; then
    echo \"Fixing /lib directory -> converting to symlink /usr/lib...\"
    if [ -d /data/local/tmp/chk/lib ]; then
      cp -a /data/local/tmp/chk/lib/* /data/local/tmp/chk/usr/lib/ 2>/dev/null || true
      rm -rf /data/local/tmp/chk/lib
    fi
    ln -s usr/lib /data/local/tmp/chk/lib
  fi

  if [ ! -L /data/local/tmp/chk/bin ]; then
    ln -sf usr/bin /data/local/tmp/chk/bin
  fi
  if [ ! -L /data/local/tmp/chk/sbin ]; then
    ln -sf usr/sbin /data/local/tmp/chk/sbin
  fi

  # Ensure /bin/bash exists
  if [ ! -e /data/local/tmp/chk/bin/bash ]; then
    ln -sf /bin/sh /data/local/tmp/chk/bin/bash
  fi

  # Prevent systemd 90s timeout waiting for non-existent boot partition
  if [ -f /data/local/tmp/chk/etc/fstab ]; then
    sed -i \"s|^UUID=B171-53C0|# UUID=B171-53C0|\" /data/local/tmp/chk/etc/fstab || true
    sed -i \"s|/boot|# /boot|\" /data/local/tmp/chk/etc/fstab || true
  fi

  echo \"%%PMOS_STEP:verify:Verifying the installed system\"
  echo \"%%PMOS_PROGRESS:90:Verifying userspace binaries\"
  echo \"Validating executable binaries inside chroot...\"
  if [ ! -f /data/local/tmp/chk/lib/ld-musl-aarch64.so.1 ]; then
    echo \"ERROR: Dynamic linker /lib/ld-musl-aarch64.so.1 is missing!\"
    umount /data/local/tmp/chk || true
    losetup -d \"\$LOOP\" 2>/dev/null || true
    exit 1
  fi

  # Preflight check: systemd execution
  chroot /data/local/tmp/chk /usr/lib/systemd/systemd --version | head -n 1
  chroot /data/local/tmp/chk /bin/busybox --help | head -n 1

  echo \"Compiling GSettings schemas...\"
  if [ -x /data/local/tmp/chk/usr/bin/glib-compile-schemas ]; then
    chroot /data/local/tmp/chk /usr/bin/glib-compile-schemas /usr/share/glib-2.0/schemas
  else
    echo \"WARNING: glib-compile-schemas missing; Display settings panel may not register\"
  fi

  echo \"Building built-in module index...\"
  if [ -f /data/local/tmp/chk/usr/lib/modules/.pmos_kernel_release ]; then
    KREL=\$(cat /data/local/tmp/chk/usr/lib/modules/.pmos_kernel_release)
    echo \"  kernel release: \$KREL\"
    chroot /data/local/tmp/chk /usr/sbin/depmod -a \"\$KREL\" >/dev/null 2>&1 || true
  fi

  echo \"Configuring nftables and zram services...\"
  # The kernel now has nftables built in; the stock ruleset already
  # allows SSH and the extra DHCPv4 client rule is in /etc/nftables.d.
  chroot /data/local/tmp/chk /usr/bin/systemctl enable nftables.service >/dev/null 2>&1 || true
  # zram is controlled by pmos-zram-mode.service (Mobile Settings), so
  # the stock swap unit must not start it unconditionally at boot.
  chroot /data/local/tmp/chk /usr/bin/systemctl disable postmarketos-zram-swap.service >/dev/null 2>&1 || true

  echo \"%%PMOS_PROGRESS:96:Unmounting and syncing\"
  echo \"Unmounting and syncing...\"
  umount /data/local/tmp/chk
  # The loop device may auto-detach when the last mount reference goes away.
  losetup -d \"\$LOOP\" 2>/dev/null || true
  sync
  echo \"RootFS installation and validation complete!\"
'"

# Cleanup temporary host files
rm -rf "$TMP_DIR"

echo "========================================================================"
echo " SUCCESS: postmarketOS $ROOTFS_SIZE rootfs is installed and verified!"
echo ""
echo " To boot into postmarketOS, switch to Slot B and reboot:"
echo "   adb reboot bootloader"
echo "   fastboot set_active b"
echo "   fastboot reboot"
echo "========================================================================"
progress 100 "RootFS installed and verified"
step done "RootFS installation complete"
printf '%%%%PMOS_DONE\n'
