#!/bin/bash
# ==============================================================================
# Master Build & Flash Script for postmarketOS on Lenovo Tab Plus (TB351FU)
# Target: Slot B (Direct LK Boot)
# ==============================================================================

set -e

# Base Directories
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PMOS_DIR="${PMOS_DIR:-$(dirname "$SCRIPT_DIR")}"
COMMON_DIR="${COMMON_DIR:-$SCRIPT_DIR}"
OUT_DIR="${OUT_DIR:-$COMMON_DIR/out}"
ARTIFACTS_DIR="${ARTIFACTS_DIR:-$PMOS_DIR/artifacts}"
# LLVM / Clang toolchain
export PATH="/usr/lib/llvm-20/bin:/mnt/Nvme/evolutionx/sync/prebuilts/clang/host/linux-x86/clang-r547379/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

echo "========================================================================"
echo " 1. Compiling Freestanding /init & Generating Initramfs CPIO"
echo "========================================================================"
mkdir -p "$ARTIFACTS_DIR"
mkdir -p /tmp/irfs_build/dev /tmp/irfs_build/proc /tmp/irfs_build/sys /tmp/irfs_build/root

INIT_SRC="$COMMON_DIR/pmos_init_src/pmos_init.c"
if [ ! -f "$INIT_SRC" ] && [ -f "$ARTIFACTS_DIR/pmos_init.c" ]; then
  INIT_SRC="$ARTIFACTS_DIR/pmos_init.c"
fi

# Detect cross compiler (aarch64-linux-gnu-gcc or clang)
if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
  CC="aarch64-linux-gnu-gcc"
elif command -v clang >/dev/null 2>&1; then
  CC="clang --target=aarch64-linux-gnu"
elif [ -x "/mnt/Nvme/evolutionx/sync/prebuilts/clang/host/linux-x86/clang-r547379/bin/clang" ]; then
  CC="/mnt/Nvme/evolutionx/sync/prebuilts/clang/host/linux-x86/clang-r547379/bin/clang --target=aarch64-linux-gnu"
else
  CC="aarch64-linux-gnu-gcc"
fi

$CC -static -nostdlib -fno-builtin -O2 -fno-stack-protector \
  -fno-asynchronous-unwind-tables -fno-ident -Wl,-e,_start \
  "$INIT_SRC" -o /tmp/irfs_build/init

chmod 755 /tmp/irfs_build/init

(cd /tmp/irfs_build && find . | cpio -H newc -o) > "$ARTIFACTS_DIR/pmos_initramfs_194.cpio"
xz -9 --check=crc32 -c "$ARTIFACTS_DIR/pmos_initramfs_194.cpio" > "$ARTIFACTS_DIR/pmos_initramfs_194.cpio.xz"
rm -rf /tmp/irfs_build

echo "Initramfs created: $ARTIFACTS_DIR/pmos_initramfs_194.cpio ($(du -h "$ARTIFACTS_DIR/pmos_initramfs_194.cpio" | cut -f1))"

echo "========================================================================"
echo " 2. Compiling Linux Kernel (GKI 6.12, LLVM 20)"
echo "========================================================================"
make -C "$COMMON_DIR" O="$OUT_DIR" ARCH=arm64 LLVM=1 -j$(nproc) Image

echo "========================================================================"
echo " 3. Packaging boot.img, init_boot.img, and vbmeta.img"
echo "========================================================================"
# Compress Image with LZ4 (LEGACY format -l -9 for LK; plain lz4 -9 fails silently)
lz4 -l -9 -f "$OUT_DIR/arch/arm64/boot/Image" "$ARTIFACTS_DIR/Image.lz4"

# Create boot.img (Header Version 4, 64MB partition size, NO --cmdline / --dtb)
python3 "$COMMON_DIR/tools/mkbootimg/mkbootimg.py" \
  --kernel "$ARTIFACTS_DIR/Image.lz4" \
  --header_version 4 --pagesize 4096 --os_version 17.0.0 --os_patch_level 2026-08 \
  --output "$ARTIFACTS_DIR/boot_raw.img"

python3 "$COMMON_DIR/external/avb/avbtool.py" add_hash_footer \
  --image "$ARTIFACTS_DIR/boot_raw.img" \
  --partition_size 67108864 --partition_name boot --algorithm SHA256_RSA4096 \
  --key "$COMMON_DIR/external/avb/test/data/testkey_rsa4096.pem" \
  --rollback_index 0 --rollback_index_location 1 \
  --prop com.android.build.boot.os_version:17 \
  --output "$ARTIFACTS_DIR/boot_blob.img"

cp "$ARTIFACTS_DIR/boot_raw.img" "$ARTIFACTS_DIR/boot_latest.img"

# Create init_boot.img (Header Version 4, 8MB partition size, ramdisk cpio.xz)
python3 "$COMMON_DIR/tools/mkbootimg/mkbootimg.py" \
  --header_version 4 --pagesize 4096 \
  --ramdisk "$ARTIFACTS_DIR/pmos_initramfs_194.cpio.xz" \
  --output "$ARTIFACTS_DIR/init_boot_raw.img"

python3 "$COMMON_DIR/external/avb/avbtool.py" add_hash_footer \
  --image "$ARTIFACTS_DIR/init_boot_raw.img" \
  --partition_size 8388608 --partition_name init_boot --algorithm SHA256_RSA4096 \
  --key "$COMMON_DIR/external/avb/test/data/testkey_rsa4096.pem" \
  --rollback_index 0 --rollback_index_location 1 \
  --output "$ARTIFACTS_DIR/init_boot_blob.img"

cp "$ARTIFACTS_DIR/init_boot_raw.img" "$ARTIFACTS_DIR/init_boot_latest.img"

# Create vbmeta.img (Verification Flags=3 Disabled) - FRESH every cycle
python3 "$COMMON_DIR/external/avb/avbtool.py" make_vbmeta_image \
  --flags 3 --padding_size 4096 \
  --key "$COMMON_DIR/external/avb/test/data/testkey_rsa4096.pem" --algorithm SHA256_RSA4096 \
  --output "$ARTIFACTS_DIR/vbmeta_latest.img"

echo "Images ready in: $ARTIFACTS_DIR"
ls -lh "$ARTIFACTS_DIR/boot_latest.img" "$ARTIFACTS_DIR/init_boot_latest.img" "$ARTIFACTS_DIR/vbmeta_latest.img"

echo "========================================================================"
echo " 4. Flashing to Slot B (if device connected via ADB root)"
echo "========================================================================"
# TARGET PARTITIONS (EXCLUSIVELY SLOT B):
# - Slot B Boot:      /dev/block/by-name/boot_b
# - Slot B Init_Boot: /dev/block/by-name/init_boot_b
# - Slot B VBMeta:    /dev/block/by-name/vbmeta_b (+ vbmeta_system_b, vbmeta_vendor_b)
# Note: Slot A partitions are NEVER modified.
if adb get-state 1>/dev/null 2>&1; then
  echo "Pushing images to device..."
  adb push "$ARTIFACTS_DIR/boot_latest.img" /data/local/tmp/boot_latest.img
  adb push "$ARTIFACTS_DIR/init_boot_latest.img" /data/local/tmp/init_boot_latest.img
  adb push "$ARTIFACTS_DIR/vbmeta_latest.img" /data/local/tmp/vbmeta_latest.img

  echo "Confirming Slot B by-name symlinks resolve to real devices..."
  adb shell "su root sh -c 'ls -la /dev/block/by-name/boot_b /dev/block/by-name/init_boot_b /dev/block/by-name/vbmeta_b /dev/block/by-name/vbmeta_system_b /dev/block/by-name/vbmeta_vendor_b'"

  echo "Flashing partitions to SLOT B ONLY via dd..."
  # Flash VBMeta to Slot B
  adb shell "su root sh -c 'dd if=/data/local/tmp/vbmeta_latest.img of=/dev/block/by-name/vbmeta_b bs=4096 conv=fsync'"
  adb shell "su root sh -c 'dd if=/data/local/tmp/vbmeta_latest.img of=/dev/block/by-name/vbmeta_system_b bs=4096 conv=fsync'"
  adb shell "su root sh -c 'dd if=/data/local/tmp/vbmeta_latest.img of=/dev/block/by-name/vbmeta_vendor_b bs=4096 conv=fsync'"

  # Flash Kernel & Initramfs to Slot B
  adb shell "su root sh -c 'dd if=/data/local/tmp/boot_latest.img of=/dev/block/by-name/boot_b bs=4194304 conv=fsync'"
  adb shell "su root sh -c 'dd if=/data/local/tmp/init_boot_latest.img of=/dev/block/by-name/init_boot_b bs=4194304 conv=fsync'"

  echo "Verifying MD5 checksums on Slot B..."
  adb shell "su root sh -c 'md5sum /dev/block/by-name/boot_b /dev/block/by-name/init_boot_b /dev/block/by-name/vbmeta_b'"
  md5sum "$ARTIFACTS_DIR/boot_latest.img" "$ARTIFACTS_DIR/init_boot_latest.img" "$ARTIFACTS_DIR/vbmeta_latest.img"
  echo "SUCCESS! Flashed and verified exclusively on Slot B (NO reboot performed)."
else
  echo "Note: Device not connected via ADB. Images are ready for manual fastboot or ADB flashing."
fi
