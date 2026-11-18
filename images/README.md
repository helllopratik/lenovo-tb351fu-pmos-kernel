# TB351FU postmarketOS image bundle

This folder contains the flashable **slot-B** images for the Lenovo Tab Plus
(TB351FU), plus a checksum file and this map. The small boot-chain images are
committed to Git; the compressed rootfs is too large for GitHub's 100 MB per-file
limit and is published as a release asset.

> **Slot A is stock Android and must never be written.** The commands below only
> target slot B. Do not reboot after a crash until `/sys/fs/pstore` and the previous
> journal have been copied.

## File → target map

| File | Target partition(s) | Size | Contents |
| --- | --- | --- | --- |
| `boot_b.img` | `boot_b` | 67,108,864 B (64 MiB) | GKI 6.12 kernel `Image.lz4`, Android boot header v4, AVB hash footer (SHA256_RSA4096, test key). No cmdline or DTB; the kernel uses its built-in cmdline and the slot-B `vendor_boot_b`/`dtbo_b` supplied by LK. |
| `init_boot_b.img` | `init_boot_b` | 8,388,608 B (8 MiB) | Freestanding pmOS `/init` plus the `cpio.lz4` initramfs (LZ4 legacy frame), boot header v4, AVB hash footer. |
| `vbmeta_b.img` | `vbmeta_b`, `vbmeta_system_b`, `vbmeta_vendor_b` | 4,096 B (4 KiB) | AVB vbmeta with verification flags `3` (verification and dm-verity disabled for slot B). The same 4 KiB image is written to all three partitions. |
| `rootfs_base.img.zst` | **not a partition** | 734,090,277 B (700 MiB) | Zstd-compressed raw ext4 rootfs (2,517,630,976 B uncompressed). Decompress to `/data/pmos/rootfs.img` on Android userdata. This is the base postmarketOS Phosh aarch64 rootfs (GPT partition 2 of `pmos-phosh-aarch64.img`); `install_pmos_rootfs.sh` resizes it and installs the TB351FU modules, firmware, services, and configs. |

`vendor_boot_b` and `dtbo_b` have no separate files here. `flash_slot_b.sh` copies
`vendor_boot_a` → `vendor_boot_b` and `dtbo_a` → `dtbo_b` only when the B partition
is empty. They carry the stock DTB/bootconfig and hardware overlays that LK needs.

`Image.lz4` and the initramfs CPIO are payloads inside `boot_b.img` and
`init_boot_b.img`; they are not flashed separately. A freshly built `Image.lz4` is
available at `artifacts/Image.lz4` after running `build_and_flash_pmos.sh`.

## Verify the files

The release asset must be downloaded into this folder before `sha256sum -c` can
check it:

```bash
cd images

# Download the compressed rootfs (release asset)
curl -L -o rootfs_base.img.zst \
  https://github.com/helllopratik/lenovo-tb351fu-pmos-kernel/releases/download/v0.1.0/rootfs_base.img.zst

sha256sum -c SHA256SUMS
```

Expected checksums are in [`SHA256SUMS`](SHA256SUMS). The rootfs line is:

```text
11493759ed503fdbec863eb14dfd20a4caac9aee59dcea8abc27a8991f447587  rootfs_base.img.zst
```

## Flash the slot-B boot chain

Requirements: unlocked bootloader, Android booted on slot A with USB debugging and
root adb, and `adb`/`fastboot` in `PATH`.

From the repository root:

```bash
# Always dry-run first
./deploy/scripts/flash_slot_b.sh \
  --boot images/boot_b.img \
  --init-boot images/init_boot_b.img \
  --vbmeta images/vbmeta_b.img \
  --dry-run

# Write only slot B
./deploy/scripts/flash_slot_b.sh \
  --boot images/boot_b.img \
  --init-boot images/init_boot_b.img \
  --vbmeta images/vbmeta_b.img
```

The ADB path checks root, validates that each image fits its target partition,
provisions empty `vendor_boot_b`/`dtbo_b` from slot A, writes
`vbmeta_b`/`vbmeta_system_b`/`vbmeta_vendor_b`, `boot_b`, and `init_boot_b`, then
compares a checksum of exactly the bytes written. It does not reboot unless you pass
`--reboot`.

Fastboot alternative (device already in bootloader/fastbootd):

```bash
fastboot flash vbmeta_b images/vbmeta_b.img
fastboot flash vbmeta_system_b images/vbmeta_b.img
fastboot flash vbmeta_vendor_b images/vbmeta_b.img
fastboot flash boot_b images/boot_b.img
fastboot flash init_boot_b images/init_boot_b.img
```

Never substitute `boot_a`, `init_boot_a`, `vbmeta_a`, or any `*_a` partition.

## Install the rootfs

With Android still booted on slot A and root adb available:

```bash
./install_pmos_rootfs.sh --image images/rootfs_base.img.zst --size 64G
```

What it does:

1. Pushes the compressed image to `/data/local/tmp/`.
2. Decompresses it to `/data/pmos/rootfs.img`.
3. Expands the ext4 filesystem to the requested size (default 64 GiB; minimum 2 GiB;
   the script refuses to leave less than 1 GiB free on `/data`).
4. Loop-mounts the image and installs the TB351FU payload: out-of-tree modules,
   firmware, systemd services, scripts, udev/sysctl/nftables configs, ALSA UCM2
   profiles, and Phosh tweaks.
5. Preserves the merged `/usr` symlinks and verifies `systemd` and BusyBox inside a
   chroot before unmounting.
6. Does not reboot.

The GUI installer can use the same file directly:

```bash
TB351FU_ROOTFS_IMAGE="$PWD/images/rootfs_base.img.zst" ./installer/tb351fu-installer
```

## Boot slot B

Only after the boot chain and rootfs are both written, and only when you are ready:

```bash
adb reboot bootloader
fastboot set_active b
fastboot reboot
```

Return to Android with `fastboot set_active a` and `fastboot reboot`, or from pmOS
with `pmos-reboot android`.

## What is intentionally not here

- **Full GPT disk image:** the loopback rootfs only needs the ext4 partition. The
  installer accepts the `.img.zst` directly, so the 487 MiB EFI partition from
  `pmos-phosh-aarch64.img` is not shipped.
- **Stock Android images:** slot A is preserved on the device. Do not overwrite it
  with anything from this folder.
- **Vendor firmware/modules:** these are provided by the repository's `deploy/`
  payload and remain subject to their own licenses.
