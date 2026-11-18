# TB351FU postmarketOS installer

A GTK4 wizard that installs postmarketOS on the Lenovo Tab Plus (TB351FU)
without repartitioning UFS and without touching the Android slot.

## Safety model

- **Slot A is read-only.** The installer never writes `boot_a`, `init_boot_a`,
  `vendor_boot_a`, `dtbo_a`, `vbmeta*_a`, `super`, or any Android logical
  partition.
- **Only slot B is flashed:** `boot_b`, `init_boot_b`, `vbmeta_b`,
  `vbmeta_system_b`, `vbmeta_vendor_b`, `vendor_boot_b` and `dtbo_b`. The
  stock `vendor_boot` and `dtbo` images come from the slot-B bundle, so the
  postmarketOS kernel gets the DTB, bootconfig and hardware overlays it needs.
- **Firmware is opt-in:** with the firmware checkbox enabled, the installer
  also writes `lk_b`, `tee_b`, `gz_b`, `scp_b`, `sspm_b`, `mcupm_b`, `dpm_b`,
  `spmfw_b` and `pi_img_b` from the same bundle. Slot A firmware is never
  touched.
- **The rootfs is a file, not a partition:** `/data/pmos/rootfs.img`, a
  loopback ext4 image inside Android userdata. The size is selectable and can
  be resized later with the normal rootfs tools.
- **Android keeps breathing room.** The wizard suggests 64 GiB (capped by free
  space minus an 8 GiB reserve) and the helper script refuses to leave less
  than 1 GiB free on `/data`.

## Requirements

Host:

- Linux with Python 3, PyGObject + GTK 4 (`python3-gi`, `gir1.2-gtk-4.0`)
- `adb` and `fastboot` in `PATH`
- the repository checkout (the installer locates it from its own path, or set
  `TB351FU_COMMON_DIR=/path/to/lenovo-tb351fu-pmos-kernel`)
- helper payloads `deploy/modules.tar.xz` and `deploy/firmware.tar.xz`
- the images below, normally under `images/`:

| Image | Default |
| --- | --- |
| boot | `images/boot_b.img` |
| init_boot | `images/init_boot_b.img` |
| vbmeta | `images/vbmeta_b.img` |
| vendor_boot | `slot_b_bundle/vendor_boot.img` |
| dtbo | `slot_b_bundle/dtbo.img` |
| slot-B firmware | `slot_b_bundle/{lk,tee,gz,scp,sspm,mcupm,dpm,spmfw,pi_img}.img` |
| rootfs | `images/rootfs_base.img.zst` (release asset), then `images/rootfs_base.img` |

The default bundle location is `/mnt/Nvme/slot_b_bundle`; set
`TB351FU_BUNDLE_DIR` to use another folder. `vendor_boot` and `dtbo` are
required. The nine firmware images are optional but recommended for a fresh
slot B. If the vendor_boot/dtbo fields are left empty, the flasher still falls
back to copying the matching slot-A partitions only when the slot-B partition is
completely empty.

Tablet:

- unlocked bootloader
- Android booted on slot A with **USB debugging** and **root adb**
  (Magisk/KernelSU) for a full installation, *or* fastboot mode for a
  boot-chain-only update

## Run from the checkout

```bash
cd installer
./tb351fu-installer
```

Install it into the desktop menu and `~/.local/bin`:

```bash
./install.sh
tb351fu-installer
```

System-wide:

```bash
PREFIX=/usr/local sudo ./install.sh
```

The installer keeps a private copy of the Python package and records the
checkout path, so rebuilding images in the checkout is picked up without
reinstalling.

## Wizard flow

1. **Device** — auto-detects adb and fastboot devices, model/codename, active
   slot, root access and battery. `Refresh` re-scans. Buttons reboot to
   bootloader or back to the device.
2. **Partitions and size** — lists every by-name partition with size, slot,
   role and block node, reads `/data` usage, then suggests a rootfs size. You
   can lower or raise it (4 GiB–256 GiB) before continuing.
3. **Images** — asks for the slot-B bundle folder (default
   `/mnt/Nvme/slot_b_bundle`) and auto-detects the boot chain, `vendor_boot`,
   `dtbo`, the slot-B firmware and the rootfs. It validates magic and partition
   limits, and lets you override each path. Choose whether to flash the boot
   chain, provision the firmware, install the rootfs, and switch to slot B.
4. **Install** — shows the exact plan in a confirmation dialog, then streams
   the helper output with progress. Slot B is only written after you confirm.

In fastboot mode the rootfs option is disabled: only the boot chain can be
written. Boot Android with root adb to install the rootfs.

## Command line

```bash
tb351fu-installer --detect          # JSON: devices, /data, partitions, images
tb351fu-installer --self-test       # backend, scripts, GPT and GTK checks
tb351fu-installer --gui-smoke-test  # build all pages, don't show the window
tb351fu-installer --version
```

`--detect` is safe to run repeatedly; it only reads device state.

## Environment

| Variable | Meaning |
| --- | --- |
| `TB351FU_COMMON_DIR` | pmOS `common/` checkout with the helper scripts |
| `TB351FU_BUNDLE_DIR` | slot-B bundle with vendor_boot, dtbo and firmware |
| `TB351FU_ROOTFS_IMAGE` | override the auto-detected rootfs image |
| `ARTIFACTS_DIR` | override the boot/init_boot/vbmeta directory |
| `ANDROID_SERIAL` | target a specific adb/fastboot serial |

## Helper scripts

The GUI is a front end for the proven scripts; they remain usable directly:

```bash
# rootfs only (Android + root adb)
../install_pmos_rootfs.sh --image /path/to/pmos.img --size 64G

# slot-B boot chain only (bundle images)
../deploy/scripts/flash_slot_b.sh \
  --boot ../images/boot_b.img \
  --init-boot ../images/init_boot_b.img \
  --vbmeta ../images/vbmeta_b.img \
  --vendor-boot /mnt/Nvme/slot_b_bundle/vendor_boot.img \
  --dtbo /mnt/Nvme/slot_b_bundle/dtbo.img \
  --dry-run

# also provision the slot-B firmware
../deploy/scripts/flash_slot_b.sh \
  --boot ../images/boot_b.img \
  --init-boot ../images/init_boot_b.img \
  --vbmeta ../images/vbmeta_b.img \
  --vendor-boot /mnt/Nvme/slot_b_bundle/vendor_boot.img \
  --dtbo /mnt/Nvme/slot_b_bundle/dtbo.img \
  --firmware-dir /mnt/Nvme/slot_b_bundle
```

`flash_slot_b.sh` auto-detects adb vs fastboot. In adb mode it checks root,
verifies partition capacity, writes only slot B, and compares a checksum of
exactly the bytes written. In fastboot mode it flashes slot B and can
`--set-active b` and `--reboot` when asked. Without `--vendor-boot` / `--dtbo`
it keeps the old fallback: copy the slot-A partition only when slot B is empty.

## Troubleshooting

- **No device**: authorize the USB debugging prompt; try `adb devices -l`.
  For fastboot use `adb reboot bootloader` or volume-down + power.
- **Root access: No**: grant the su prompt for the shell/installer, or use
  fastboot for the boot chain only.
- **`/data` free space unavailable**: rootfs installation needs Android's
  `df`; boot Android first.
- **Wrong device detected**: the wizard refuses to install unless the
  codename/model contains `tb351fu`.
- **Fastboot reports a different serial**: the installer falls back to the
  only connected fastboot device; if several are connected, use
  `ANDROID_SERIAL`.
- **Vendor boot / dtbo missing**: point the Images page at
  `/mnt/Nvme/slot_b_bundle` and press **Use bundle images**. A boot chain
  without them may hand the kernel the wrong DTB or no overlays.
- **Slot switch fails**: run `fastboot set_active b` manually and reboot.

## Tests

```bash
cd installer
python3 -m unittest discover -s tests -v
python3 -m tb351fu_installer --self-test
```
