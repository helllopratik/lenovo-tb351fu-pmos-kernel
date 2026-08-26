#!/bin/bash
# ==============================================================================
# MicroSD Card Partitioning & postmarketOS RootFS Installer for TB351FU
# ==============================================================================

set -e

if [ "$#" -ne 1 ]; then
  echo "Usage: sudo $0 /dev/sdX  (or /dev/mmcblkX)"
  echo "Example: sudo $0 /dev/sdb"
  echo "WARNING: All data on the target drive will be wiped!"
  exit 1
fi

DISK="$1"

if [ "$EUID" -ne 0 ]; then
  echo "Error: Please run as root (sudo $0 $DISK)"
  exit 1
fi

if [ ! -b "$DISK" ]; then
  echo "Error: Device $DISK does not exist or is not a block device."
  exit 1
fi

echo "Target Device: $DISK"
read -p "Are you ABSOLUTELY sure you want to format and partition $DISK? (type 'yes'): " CONFIRM
if [ "$CONFIRM" != "yes" ]; then
  echo "Aborted."
  exit 1
fi

# Determine partition naming (e.g. /dev/sdb1 vs /dev/mmcblk0p1)
if [[ "$DISK" =~ [0-9]$ ]]; then
  P1="${DISK}p1"
  P2="${DISK}p2"
else
  P1="${DISK}1"
  P2="${DISK}2"
fi

# Unmount existing mounts
echo "Unmounting any existing mounts on $DISK..."
umount ${DISK}* 2>/dev/null || true

# Partitioning with GPT (256MB FAT32 Boot + Remaining ext4 RootFS)
echo "Partitioning $DISK with GPT..."
parted --script "$DISK" \
  mklabel gpt \
  mkpart primary fat32 1MiB 256MiB \
  set 1 boot on \
  mkpart primary ext4 256MiB 100%

sleep 2
partprobe "$DISK" 2>/dev/null || true
sleep 1

# Format Partitions
echo "Formatting Partition 1 ($P1) as FAT32 (PMOS_BOOT)..."
mkfs.vfat -F 32 -n "PMOS_BOOT" "$P1"

echo "Formatting Partition 2 ($P2) as ext4 (pmOS_root)..."
mkfs.ext4 -F -L "pmOS_root" -O "^metadata_csum_seed" "$P2"

echo "========================================================================"
echo " MicroSD Card Successfully Partitioned & Formatted!"
echo " Partition 1: $P1 (FAT32, 256MB) -> Label: PMOS_BOOT"
echo " Partition 2: $P2 (ext4, Remaining) -> Label: pmOS_root"
echo "========================================================================"
echo "To copy postmarketOS rootfs: extract your rootfs tarball or rsync into $P2"
echo "Example:"
echo "  mkdir -p /mnt/sdroot"
echo "  mount $P2 /mnt/sdroot"
echo "  tar -xf postmarketos-rootfs.tar.gz -C /mnt/sdroot"
echo "  umount /mnt/sdroot"
echo "========================================================================"
