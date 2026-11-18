# postmarketOS on the Lenovo Tab Plus (TB351FU)

A hardware-enablement tree and installer for running **postmarketOS / Phosh** on the
**Lenovo Tab Plus (TB351FU, MediaTek Helio G99 / MT6789 / MT8781)** alongside stock
Android. It uses the tablet's existing A/B slots instead of repartitioning UFS:

- **Slot A** stays stock Android and is treated as read-only.
- **Slot B** holds the postmarketOS boot chain.
- The root filesystem is a **loopback ext4 image** at `/data/pmos/rootfs.img` inside
  Android userdata, not a new partition.

> **Status:** usable for daily use with the limitations listed below. Camera capture,
> Panfrost cold-boot reliability, and automatic Type-C role detection are still
> bring-up areas. The boot chain and rootfs installer are conservative and only write
> slot B.

---

## Safety first

- **Never flash slot A.** `boot_a`, `init_boot_a`, `vendor_boot_a`, `dtbo_a`,
  `vbmeta*_a`, `super`, and Android logical partitions are off limits. The installer
  refuses to write them.
- **Only slot B is written:** `boot_b`, `init_boot_b`, `vbmeta_b`,
  `vbmeta_system_b`, `vbmeta_vendor_b`, `vendor_boot_b` and `dtbo_b`. The stock
  `vendor_boot`, `dtbo` and optional slot-B firmware images come from
  `/mnt/Nvme/slot_b_bundle`; the flasher never writes their slot-A counterparts.
- **Flashing is explicit.** The GUI shows the exact plan and asks for confirmation.
  The helper scripts support `--dry-run`; use it first. Do not run any flash command
  until you have verified the target device, slot, and image.
- **The rootfs is a file, not a partition.** It lives at
  `/data/pmos/rootfs.img` on Android userdata and is mounted through `/dev/loop0`.
  Android `/data` is otherwise exposed read-only to pmOS; `/data_host/pmos` is
  protected with a read-only self-bind so a desktop process cannot corrupt the live
  rootfs image.
- **Crash policy: do not reboot after a crash.** Auto-reboot is disabled
  (`kernel.panic=0`, `panic_on_oops=0`, `hung_task_panic=0`) and the bring-up
  watchdog is disarmed once the rootfs is mounted. Pull `/sys/fs/pstore` and the
  previous journal **before** any restart. `deploy/scripts/pstore_snapshot.sh`
  snapshots pstore on the next warm boot, but a deliberate reboot can overwrite the
  evidence.
- **Back up.** Keep a known-good Android boot image and your data backed up. This is
  a hobbyist hardware port, not a vendor-supported update.

---

## What works

| Area | Status |
| --- | --- |
| Boot / dual-boot | Working. LK boots the slot-B kernel directly; Android remains on slot A. |
| Display | Working. 1200×2000 panel, portrait 180°, `pmosfb` boots at 60 Hz, then 60 / 90 / Adaptive modes. |
| Touch | Working. Novatek NT36523 TDDI over SPI2, interrupt-driven. |
| Stylus | Working. AES pen pressure/tilt at 90 Hz; Lenovo Tab Pen Plus BLE gestures via `pmos-pen-daemon`. |
| Audio | Working on the AP side: ALSA/ASoC + PulseAudio, MT6366 + AW88395 amplifiers, UCM2 profiles. |
| Wi-Fi / Bluetooth | Working through the MediaTek CONSYS vendor modules and firmware. |
| GNSS | Working. Vendor `mnld` feeds a filtered NMEA socket to GeoClue. |
| Storage | Internal shared storage, read-only Android views, microSD and USB via udisks2/Nautilus. |
| Charging | Charge-limit GUI, 50–99 % or full, default 80 %, 5 % hysteresis. |
| GPU | Experimental. Panfrost Mali-G57; a cold boot may be required after a warm reset. |
| Camera | Bring-up only. Mainline MTK SENINF/CAMSV drivers; modules are manual-load and capture is not reliable yet. |
| Widevine | L3 only with a user-supplied ARM64 CDM (no proprietary blob is shipped). |
| USB OTG | Manual role switch (`pmos-usb-role`); automatic Type-C role detection is not available. |

---

## Hardware

- **Device:** Lenovo Tab Plus (TB351FU, board `t808aa`)
- **SoC:** MediaTek Helio G99 / MT6789 / MT8781 (ARM64)
- **Kernel:** Google GKI 6.12-based tree with MediaTek and TB351FU changes
- **Display:** 11.5″ 1200×2000 LCD, 60/90 Hz, Novatek NT36523 TDDI touch
- **Storage:** UFS 2.2 internal, microSD
- **Audio:** MT6366 codec + AW88395 smart amplifiers
- **Cameras:** Samsung S5K4H7YX and SmartSens SC820CS sensors
- **GNSS:** MediaTek vendor `mnld` userspace

---

## Architecture

```text
Slot A (untouched)                 Slot B (postmarketOS)
┌──────────────────────┐           ┌──────────────────────────────┐
│ boot_a               │           │ boot_b       kernel Image.lz4│
│ init_boot_a          │           │ init_boot_b  pmOS /init      │
│ vendor_boot_a        │  bundle   │ vendor_boot_b  stock DTB     │
│ dtbo_a               │  bundle   │ dtbo_b         stock overlays│
│ vbmeta*_a            │           │ vbmeta*_b    verify off      │
│ Android system/data  │           └──────────────┬───────────────┘
└──────────────────────┘                          │
                                                  ▼
                                   /data/pmos/rootfs.img (ext4)
                                   inside Android userdata
                                                  │
                                      loop0 ──► systemd ──► Phosh
```

Boot flow: `LK` → `boot_b` kernel → `init_boot_b` `/init` → mount Android userdata
at `/data_host` → attach `/data_host/pmos/rootfs.img` to `/dev/loop0` → mount ext4 →
`switch_root` → systemd → Phosh.

The freestanding `/init` keeps `/data_host` mounted after the switch so the loopback
backing file stays open. `/data_host/pmos` is then remounted read-only for the life
of the pmOS session.

---

## Repository layout

| Path | Purpose |
| --- | --- |
| `arch/arm64/configs/pmos_tb351fu.fragment` | TB351FU kernel configuration fragment. |
| `drivers/gpu/drm/tiny/pmosfb.c` | DRM framebuffer driver with POC cache flushing and 60/90 Hz pacing. |
| `drivers/media/platform/mediatek/isp/` | Mainline MTK SENINF + CAMSV camera pipeline. |
| `drivers/media/i2c/s5k4h7yx.c`, `sc820cs.c` | Camera sensor drivers. |
| `pmos_init_src/pmos_init.c` | Freestanding aarch64 `/init` that finds and mounts the rootfs. |
| `deploy/` | Modules, firmware payloads, scripts, services, configs, and subsystem docs. |
| `installer/` | GTK4/Python installer wizard and backend. |
| `images/` | Flashable boot-chain bundle, checksums, and the image-to-partition map. |
| `build_and_flash_pmos.sh` | Builds the kernel/initramfs and packages/slot-B-flashes the boot chain. |
| `install_pmos_rootfs.sh` | Decompresses, resizes, provisions, and verifies the loopback rootfs. |
| `pmos_kernel.md`, `device.md`, `verification.md` | Historical bring-up notes; useful, but some paths are outdated. |

---

## Quick start

### Requirements

Host:

- Linux with `adb`, `fastboot`, `python3`, `zstd`, `lz4`, and a Clang/LLVM toolchain
  for rebuilding the kernel.
- GTK4 + PyGObject for the GUI installer (`python3-gi`, `gir1.2-gtk-4.0`). The
  installer uses GTK4 without libadwaita.

Tablet:

- Unlocked bootloader.
- Android booted on **slot A** with USB debugging and **root adb** (Magisk/KernelSU)
  for a full rootfs install.
- Fastboot or fastbootd is enough for a boot-chain-only update; the rootfs install
  needs Android running with root adb.

### 1. Get the images

```bash
git clone https://github.com/helllopratik/lenovo-tb351fu-pmos-kernel.git
cd lenovo-tb351fu-pmos-kernel
```

The small slot-B images are committed under `images/`. The compressed rootfs is a
GitHub Release asset because it is larger than GitHub's 100 MB per-file limit:

```bash
# Download the release asset into images/ (URL is also in images/README.md)
curl -L -o images/rootfs_base.img.zst \
  https://github.com/helllopratik/lenovo-tb351fu-pmos-kernel/releases/download/v0.1.0/rootfs_base.img.zst

cd images
sha256sum -c SHA256SUMS
```

See [`images/README.md`](images/README.md) for the full file-to-partition map.

### 2. Install with the GUI (recommended)

```bash
cd installer
./install.sh          # adds tb351fu-installer to ~/.local/bin and the app menu
tb351fu-installer
```

Or run it directly from the checkout:

```bash
./installer/tb351fu-installer
```

The wizard:

1. Detects adb/fastboot devices, model/codename, active slot, root access, and battery.
2. Lists every by-name partition with size, slot, and role. Logical/mapper partitions
   are display-only and are never written.
3. Reads `/data` free space and suggests a rootfs size (default 64 GiB, leaving an
   8 GiB reserve).
4. Validates the boot-chain images and rootfs image, then shows a confirmation plan.
5. Flashes only slot B, installs the rootfs, and optionally switches the active slot.

In fastboot mode the rootfs step is disabled: flash the boot chain there, then boot
Android with root adb to install the rootfs.

### 3. Install from the command line

With Android booted on slot A and root adb authorized:

```bash
BUNDLE=/mnt/Nvme/slot_b_bundle

# 1) Check the boot-chain plan without writing anything
./deploy/scripts/flash_slot_b.sh \
  --boot images/boot_b.img \
  --init-boot images/init_boot_b.img \
  --vbmeta images/vbmeta_b.img \
  --vendor-boot "$BUNDLE/vendor_boot.img" \
  --dtbo "$BUNDLE/dtbo.img" \
  --dry-run

# 2) Write the slot-B boot chain
#    (add --firmware-dir "$BUNDLE" for a fresh slot B)
./deploy/scripts/flash_slot_b.sh \
  --boot images/boot_b.img \
  --init-boot images/init_boot_b.img \
  --vbmeta images/vbmeta_b.img \
  --vendor-boot "$BUNDLE/vendor_boot.img" \
  --dtbo "$BUNDLE/dtbo.img" \
  --firmware-dir "$BUNDLE"

# 3) Decompress, resize, provision, and verify the rootfs
./install_pmos_rootfs.sh --image images/rootfs_base.img.zst --size 64G
```

`install_pmos_rootfs.sh` accepts either a full postmarketOS GPT image (it extracts
partition 2) or a pre-compressed raw ext4 image (`.img.zst`). It decompresses and
prepares the image on the host, because Android's toybox has no `zstd`. The ext4
`orphan_file` feature is cleared on the host so Android's e2fsprogs 1.46 can run
`e2fsck`/`resize2fs`. During payload application the image is temporarily labeled
`media_rw_data_file` for the read-write loop mount and restored to
`system_data_file` afterwards. The script refuses to leave less than 1 GiB free
on `/data`.

### 4. Boot slot B

Only after both steps complete, and only when you are ready to test:

```bash
adb reboot bootloader
fastboot set_active b
fastboot reboot
```

To return to Android:

```bash
# From fastboot
fastboot set_active a
fastboot reboot

# Or from the pmOS shell
pmos-reboot android
```

---

## Building from source

`build_and_flash_pmos.sh` compiles the freestanding `/init`, the GKI 6.12 kernel, and
the boot images, then flashes slot B if a device is connected. Set `NO_FLASH=1` to
package the images without writing anything:

```bash
NO_FLASH=1 ./build_and_flash_pmos.sh
```

The script produces:

- `artifacts/boot_latest.img` (64 MiB, kernel + AVB hash footer)
- `artifacts/init_boot_latest.img` (8 MiB, pmOS initramfs + AVB hash footer)
- `artifacts/vbmeta_latest.img` (4 KiB, verification flags disabled)
- `artifacts/Image.lz4` (kernel payload, for reference)

The rootfs is installed separately with `install_pmos_rootfs.sh` (or the GUI).

Kernel configuration lives in `arch/arm64/configs/pmos_tb351fu.fragment`; the helper
`apply_pmos_kernel_config.sh` applies it to a GKI tree. The build expects the Android
Clang toolchain described in `build_and_flash_pmos.sh`.

---

## Image bundle

The committed bundle is intentionally small:

| File | Target | Size | Contents |
| --- | --- | --- | --- |
| `images/boot_b.img` | `boot_b` | 64 MiB | LZ4-legacy kernel `Image`, boot header v4, AVB hash footer. |
| `images/init_boot_b.img` | `init_boot_b` | 8 MiB | Freestanding pmOS `/init` + `cpio.lz4` initramfs, AVB hash footer. |
| `images/vbmeta_b.img` | `vbmeta_b`, `vbmeta_system_b`, `vbmeta_vendor_b` | 4 KiB | AVB vbmeta with verification flags `3`. |
| `slot_b_bundle/vendor_boot.img` | `vendor_boot_b` | 64 MiB | Stock DTB/bootconfig; external bundle, not committed. |
| `slot_b_bundle/dtbo.img` | `dtbo_b` | 8 MiB | Stock hardware overlays; external bundle. |
| `slot_b_bundle/{lk,tee,gz,scp,sspm,mcupm,dpm,spmfw,pi_img}.img` | matching `*_b` | varies | Optional slot-B firmware for a fresh install; external bundle. |
| `images/rootfs_base.img.zst` | not a partition | 734 MB | Base ext4 rootfs for `/data/pmos/rootfs.img`; release asset. |

`vendor_boot_b` and `dtbo_b` are stock images from `/mnt/Nvme/slot_b_bundle`
(`TB351FU_BUNDLE_DIR` overrides the path); the installer and `flash_slot_b.sh`
write them explicitly. If no image is supplied, the flasher only falls back to
copying slot A when the slot-B partition is completely empty. The same bundle can
provide the `lk`/`tee`/`gz`/`scp`/`sspm`/`mcupm`/`dpm`/`spmfw`/`pi_img` firmware
for a fresh slot B. The kernel payload inside `boot_b.img` is also available as
`artifacts/Image.lz4` after a build.

See [`images/README.md`](images/README.md) for exact commands, checksums, and the
rootfs release instructions.

---

## Display, touch, and pen

- **Portrait 180°** is the supported orientation. `phoc.ini` uses `scale = 2` and
  `rotate = 180`.
- **`pmosfb` starts at 60 Hz** for a stable LK handoff. After boot,
  `pmos-display-mode` applies the saved mode from
  `org.postmarketos.Display refresh-mode`:
  - `60` — power saving. The NT36523 reports the stylus as a finger at 60 Hz, so
    pressure and tilt are unavailable.
  - `90` — full stylus mode (default).
  - `adaptive` — 90 Hz while the screen changes or input arrives, 60 Hz after a
    static period, and 90 Hz held for five minutes after stylus activity.
- **No startup animation.** The framebuffer console/pmosfb hands off directly to
  Phosh. Do not add a boot animation to the display path.
- **Touch** is interrupt-driven; there is no continuous SPI polling thread. Keep it
  that way to avoid bus contention with scanout.

---

## Audio

Audio is entirely **AP-side ALSA/ASoC + PulseAudio**:

- MT6366 codec and eight AW88395 smart amplifiers.
- UCM2 profiles in `deploy/configs/ucm2/`.
- `mt6789_audio_pd` and `mt6789_i2s3_mclk` are order-sensitive. Do not change their
  load order or power-domain/clock sequence without hardware testing; a mistake can
  leave playback stalled with `-EIO` or destabilize the boot.

---

## Networking

The USB gadget provides CDC-NCM/RNDIS Ethernet plus a CDC-ACM console:

| Interface | Address |
| --- | --- |
| Tablet (`usb0`) | `172.16.42.1/24` |
| Host | `172.16.42.2/24` |

- SSH on port 22; the password is set during installation, not a fixed default.
- Telnet root shell on port 23 and an ACM console on `/dev/ttyGS0` are intended for
  bring-up on a trusted host only.
- Wi-Fi and Bluetooth use the MediaTek CONSYS vendor modules/firmware. Keep the
  module load order from `deploy/scripts/init_wifi_sensors.sh` and
  `init_bt.sh`; the CONSYS power/reset sequence is fragile.

---

## Storage

- The rootfs is `/data/pmos/rootfs.img` (ext4 loopback), resized by the installer.
- `/data_host` is Android userdata mounted read-only in pmOS.
- `/data_host/pmos` is protected with a read-only self-bind for the whole pmOS
  session. Do not undo it while pmOS is running; it would let a desktop process
  corrupt the live rootfs image.
- `/media/internal` is a read-write bind of Android shared storage
  (`/data_host/media/0`).
- Android system/vendor/product partitions are exposed read-only through loop
  devices. `userdata` and `super` are never mounted raw.
- **microSD and USB storage are handled by udisks2/Nautilus**, not by the Android
  partition service.

---

## Charging

`pmos-charge-limit` provides a small GUI for the charge cap:

- Range: 50–99 %, or full (100 %).
- Default: **80 %**.
- 5 % hysteresis to avoid charge toggling around the limit.
- Persisted in `/etc/pmos-charge-cap.conf`; the kernel interface is
  `/sys/kernel/pmos_charge_cap`.

The limit remains user-settable; the current device setting is 80 %.

---

## GNSS

The GNSS stack is **NMEA-only**:

- Vendor `mnld` talks to the MTK GNSS hardware and writes NMEA to
  `/run/tablet-gps/nmea`.
- `tablet-gnss-bridge.py` filters the stream and exposes
  `/run/tablet-gps/nmea.sock`.
- GeoClue uses the `network-nmea` source pointed at that socket.

Do not replace this with GeoClue's Wi-Fi/network provider or edit
`/etc/geoclue/geoclue.conf` without understanding the NMEA-only design. See
[`deploy/gnss/README.md`](deploy/gnss/README.md).

---

## Camera

Camera support is deliberately **manual-load only**:

```sh
insmod /lib/modules/extra/mtk-seninf.ko
insmod /lib/modules/extra/mtk-camsv.ko
insmod /lib/modules/extra/s5k4h7yx.ko
insmod /lib/modules/extra/sc820cs.ko
```

`deploy/configs/90-camera-manual.conf` blacklists the modules so they are not
autoloaded at boot. The pipeline uses the mainline MTK SENINF + CAMSV drivers, not
the vendor ISP stack. Rear capture currently produces no frames; front SENINF/CAMSV
routing is still being debugged. Treat camera work as bring-up, not a supported
feature.

---

## Crash forensics and the no-reboot rule

This port intentionally favors evidence over automatic recovery:

- `kernel.panic = 0`, `kernel.panic_on_oops = 0`, `kernel.hung_task_panic = 0`,
  `kernel.softlockup_panic = 0`.
- The 120 s bring-up watchdog is disarmed after the rootfs mounts.
- A crash does **not** reboot the tablet. Do not deliberately reboot it before
  pulling `/sys/fs/pstore` and the previous journal.
- `deploy/scripts/pstore_snapshot.sh` copies pstore records to `/root/pstore_auto`
  on the next warm boot, but a manual reboot can overwrite the records first.

If you are debugging, capture pstore and `journalctl -b -1` before changing slots or
power-cycling. This rule exists because earlier bring-up cycles lost crash traces to
automatic reboots.

---

## Known limitations

- **Camera capture** is not working reliably; rear frames are absent and front
  routing is incomplete.
- **Panfrost** is experimental; a cold boot may be needed after a warm reset.
- **USB Type-C role detection** is unavailable; use `pmos-usb-role host|device`.
- **Widevine L3** requires a user-supplied ARM64 CDM; L1/L2 are not possible without
  the vendor TEE.
- **60 Hz stylus trade-off:** pressure/tilt require 90 Hz; 60 Hz reports the pen as a
  finger.
- **Vendor firmware/modules** remain subject to their own licenses. They are included
  only to make the owned hardware work.

---

## Documentation

- [`images/README.md`](images/README.md) — image-to-partition map and flashing.
- [`installer/README.md`](installer/README.md) — GUI installer, CLI modes, safety model.
- [`deploy/docs/pmos-storage.md`](deploy/docs/pmos-storage.md) — internal storage and
  Android partition exposure.
- [`deploy/docs/USB_OTG.md`](deploy/docs/USB_OTG.md) — host/device role switching.
- [`deploy/docs/DRM_WIDEVINE.md`](deploy/docs/DRM_WIDEVINE.md) — Widevine status.
- [`deploy/gnss/README.md`](deploy/gnss/README.md) — GNSS/GeoClue bridge.
- [`deploy/phosh/README.md`](deploy/phosh/README.md) — patched Phosh build.
- `device.md`, `verification.md`, `pmos_kernel.md` — historical bring-up notes. Some
  paths and attempts are outdated; the README and `images/README.md` are the
  authoritative install instructions.

---

## Credits and license

Kernel and original project code are licensed under **GPL-2.0** (see `COPYING`).
postmarketOS, Phosh, GNOME, Alpine, and the Linux kernel are the work of their
respective communities. MediaTek vendor modules, firmware, and other proprietary
blobs remain the property of their owners and are not relicensed by this project.

See [`CREDITS.md`](CREDITS.md) for the upstream camera-patch authors whose work
this tree builds on.
