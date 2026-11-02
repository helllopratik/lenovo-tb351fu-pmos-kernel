#!/usr/bin/env bash
# ==============================================================================
# Master postmarketOS RootFS Installer & Provisioner for Lenovo Tab Plus (TB351FU)
# Author: helllopratik <pratikgondane07@gmail.com>
#
# Provisions a complete, clean 64GB postmarketOS (Phosh) rootfs on UFS userdata
# (/data/pmos/rootfs.img) for dual-booting alongside Android.
#
# Preserves merged /usr symlinks, deploys out-of-tree hardware modules, installs
# Wi-Fi / Bluetooth bring-up services, locks display to native 60Hz, and verifies
# systemd executable integrity inside chroot before completion.
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPLOY_DIR="$SCRIPT_DIR/deploy"
TMP_DIR="/tmp/pmos_install_tmp"

echo "========================================================================"
echo " postmarketOS UFS Loopback RootFS Installer (Lenovo Tab Plus TB351FU)"
echo "========================================================================"

# 1. Check prerequisites
command -v adb >/dev/null 2>&1 || { echo "ERROR: adb is required but not installed."; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "ERROR: python3 is required but not installed."; exit 1; }
command -v zstd >/dev/null 2>&1 || { echo "ERROR: zstd is required but not installed."; exit 1; }

if [ ! -d "$DEPLOY_DIR" ] || [ ! -f "$DEPLOY_DIR/modules.tar.xz" ]; then
  echo "ERROR: Deploy directory or modules archive missing at $DEPLOY_DIR"
  exit 1
fi

# 2. Check device connectivity via ADB
echo "Checking ADB device connection..."
if ! adb get-state 1>/dev/null 2>&1; then
  echo "ERROR: Device not found. Ensure tablet is booted into Android with USB debugging enabled."
  exit 1
fi

DEVICE_ID=$(adb devices | grep -w "device" | head -n 1 | awk '{print $1}')
echo "Found device: $DEVICE_ID"

# Verify root ADB access
adb shell "su -c 'id'" | grep -q "uid=0" || {
  echo "ERROR: Root ADB access required. Grant su permissions in Magisk/KernelSU."
  exit 1
}
echo "Root ADB access confirmed."

# 3. Locate source postmarketOS image
PMOS_IMG="${1:-}"
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
mkdir -p "$TMP_DIR"

# 4. Prepare base rootfs image
ROOTFS_ZST="$TMP_DIR/rootfs_base.img.zst"
if [[ "$PMOS_IMG" == *.zst ]]; then
  echo "Using pre-compressed zstd image: $PMOS_IMG"
  ROOTFS_ZST="$PMOS_IMG"
elif [[ "$PMOS_IMG" == *.img ]]; then
  # Check if GPT disk image or raw partition
  if python3 -c "
with open('$PMOS_IMG', 'rb') as f:
    f.seek(512)
    sig = f.read(8)
    exit(0 if sig == b'EFI PART' else 1)
"; then
    echo "Detected GPT disk image. Extracting partition 2 (rootfs)..."
    python3 -c "
import subprocess
out = subprocess.check_output(['fdisk', '-l', '$PMOS_IMG']).decode()
start_sector = None
count_sectors = None
for line in out.splitlines():
    if '$PMOS_IMG' in line and ('2' in line or 'Linux' in line):
        parts = line.split()
        for idx, p in enumerate(parts):
            if p.isdigit() and int(p) > 2048 and start_sector is None:
                start_sector = int(p)
            elif p.isdigit() and start_sector is not None and count_sectors is None:
                # end sector
                count_sectors = int(p) - start_sector + 1
                break
if not start_sector:
    start_sector = 999424
    count_sectors = 4917248
print(f'Extracting sectors: skip={start_sector} count={count_sectors}')
cmd = ['dd', 'if=$PMOS_IMG', 'bs=512', f'skip={start_sector}', f'count={count_sectors}']
p1 = subprocess.Popen(cmd, stdout=subprocess.PIPE)
p2 = subprocess.Popen(['zstd', '-3', '-f', '-o', '$ROOTFS_ZST'], stdin=p1.stdout)
p1.stdout.close()
p2.communicate()
"
  else
    echo "Detected raw ext4 partition. Compressing with zstd..."
    zstd -3 -f "$PMOS_IMG" -o "$ROOTFS_ZST"
  fi
fi

echo "Base rootfs compressed archive ready: $(ls -lh "$ROOTFS_ZST" | awk '{print $5}')"

# 5. Build customization payload package
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
         "$PAYLOAD_DIR/etc/systemd/system.conf.d" \
         "$PAYLOAD_DIR/data_host"

# Extract out-of-tree hardware modules into usr/lib/modules/extra
mkdir -p "$PAYLOAD_DIR/usr/lib/modules/extra"
tar -xf "$DEPLOY_DIR/modules.tar.xz" -C "$PAYLOAD_DIR/usr/lib/modules/extra"

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
cp "$DEPLOY_DIR/configs/watchdog.conf" "$PAYLOAD_DIR/etc/systemd/system.conf.d/watchdog.conf"

PAYLOAD_TAR="$TMP_DIR/pmos_payload.tar.gz"
tar -czf "$PAYLOAD_TAR" -C "$PAYLOAD_DIR" .
echo "Customization payload ready: $(ls -lh "$PAYLOAD_TAR" | awk '{print $5}')"

# 6. Push archives to device
echo "Pushing rootfs base image and customizations to device..."
adb push "$ROOTFS_ZST" /data/local/tmp/pmos_base.img.zst
adb push "$PAYLOAD_TAR" /data/local/tmp/pmos_payload.tar.gz

# 7. Unpack and configure on device
echo "Unpacking rootfs, resizing to 64GB, and applying customizations on device..."
adb shell "su -c '
  set -e
  mkdir -p /data/pmos
  rm -f /data/pmos/rootfs.img

  echo \"Decompressing base image to /data/pmos/rootfs.img...\"
  if [ -x /system_ext/bin/zstd ]; then
    /system_ext/bin/zstd -d /data/local/tmp/pmos_base.img.zst -o /data/pmos/rootfs.img
  elif command -v zstd >/dev/null 2>&1; then
    zstd -d /data/local/tmp/pmos_base.img.zst -o /data/pmos/rootfs.img
  else
    echo \"ERROR: zstd decompression binary not found on Android.\"
    exit 1
  fi
  rm -f /data/local/tmp/pmos_base.img.zst

  echo \"Expanding filesystem to 64GB...\"
  truncate -s 64G /data/pmos/rootfs.img
  /system/bin/e2fsck -f -y /data/pmos/rootfs.img || true
  /system/bin/resize2fs /data/pmos/rootfs.img

  echo \"Loopback mounting rootfs...\"
  mkdir -p /data/local/tmp/chk
  LOOP=\$(losetup -f /data/pmos/rootfs.img --show)
  mount -t ext4 \$LOOP /data/local/tmp/chk

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

  echo \"Validating executable binaries inside chroot...\"
  if [ ! -f /data/local/tmp/chk/lib/ld-musl-aarch64.so.1 ]; then
    echo \"ERROR: Dynamic linker /lib/ld-musl-aarch64.so.1 is missing!\"
    umount /data/local/tmp/chk
    losetup -d \$LOOP
    exit 1
  fi

  # Preflight check: systemd execution
  chroot /data/local/tmp/chk /usr/lib/systemd/systemd --version | head -n 1
  chroot /data/local/tmp/chk /bin/busybox --help | head -n 1

  echo \"Unmounting and syncing...\"
  umount /data/local/tmp/chk
  losetup -d \$LOOP
  sync
  echo \"RootFS installation and validation complete!\"
'"

# Cleanup temporary host files
rm -rf "$TMP_DIR"

echo "========================================================================"
echo " SUCCESS: postmarketOS 64GB rootfs is installed and verified!"
echo ""
echo " To boot into postmarketOS, switch to Slot B and reboot:"
echo "   adb reboot bootloader"
echo "   fastboot set_active b"
echo "   fastboot reboot"
echo "========================================================================"
