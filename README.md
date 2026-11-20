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
| Audio | Working on the AP side: ALSA/ASoC + PipeWire (PulseAudio API), MT6366 + AW88395 amplifiers, UCM2 profiles. |
| Wi-Fi / Bluetooth | Working through the MediaTek CONSYS vendor modules and firmware. |
| GNSS | Working. Vendor `mnld` feeds a filtered NMEA socket to GeoClue. |
| Storage | Internal shared storage, read-only Android views, microSD and USB via udisks2/Nautilus. |
| Charging | Charge-limit GUI, 50–99 % or full, default 80 %, 5 % hysteresis. |
| GPU | Working. Panfrost Mali-G57; Phoc uses GLES2. A cold boot may be required after a warm reset. |
| Sensors | Working. Accelerometer, gyroscope and ambient light via the SCP stack and `pmos_sensor_bridge`; `iio-sensor-proxy` feeds Phosh. |
| Camera | Bring-up only. Mainline MTK SENINF/CAMSV drivers; modules are manual-load and capture is not reliable yet. |
| Widevine | L3 verified in Flatpak Firefox/Chromium with a user-supplied ARM64 CDM; native Firefox is musl and cannot load the glibc CDM. No proprietary blob is shipped. |
| USB OTG | Manual role switch (`pmos-usb-role`); automatic Type-C role detection is not available. |
| Remote access | SSH is enabled on first boot; the hostname is `lenovo-tab-plus-tb351fu`. |

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

1. Detects adb/fastboot devices, model/codename, active slot, root access, battery,
   and any existing pmOS installation in `/data`.
2. Lists every by-name partition with size, slot, and role. Logical/mapper partitions
   are display-only and are never written.
3. Reads `/data` free space and suggests a rootfs size (default 64 GiB, leaving an
   8 GiB reserve).
4. Validates the boot-chain images and rootfs image, then offers the installation
   mode, image source, account details, and existing-install action.
5. Shows a confirmation plan, then flashes only slot B, installs the rootfs, or
   prepares a destructive whole-device package. It switches the active slot only
   for a dual-boot install when requested.

Whole-device mode *prepares* a destructive offline package in a temporary
encrypted staging directory. Android is not changed while the package is being
prepared. Applying the package later, from recovery with /data unmounted,
erases Android and replaces the boot chain in slots A and B.

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

# 3) Install the rootfs into /data/pmos (asks source and account details)
./install_pmos_rootfs.sh --source auto --mode dual

#    Or use a local image:
./install_pmos_rootfs.sh --source manual \
  --image images/rootfs_base.img.zst --size 64G --mode dual
```

`install_pmos_rootfs.sh` can download the matching rootfs from the server
(`--source auto`, URL in `deploy/configs/pmos_image_url`) or use a local
`.img`, `.img.zst`, `.img.xz`, or `.img.gz` file. A GPT image is accepted; the
installer extracts partition 2. It decompresses and prepares the image on the
host, because Android's toybox has no `zstd`. The ext4 `orphan_file` feature is
cleared on the host so Android's e2fsprogs 1.46 can run `e2fsck`/`resize2fs`.
During payload application the image is temporarily labeled
`media_rw_data_file` for the read-write loop mount and restored to
`system_data_file` afterwards. The script refuses to leave too little free
space on `/data`.

The installer asks for the pmOS username and password and applies exactly the
login you enter. The GUI requires both fields whenever a rootfs is installed;
the interactive CLI prompts for a username with no default and then for the
password. Only a SHA-512 crypt hash is sent to the tablet; the plaintext
password is never stored or passed on a command line. `--no-account` is an
advanced CLI option that keeps the image's default `user` account.
`--password-file` accepts a plaintext password for unattended runs, and
`--password-hash-file` accepts a precomputed `$6$...` hash. The GUI uses the
hash-file form, so only the hash leaves the host.

If `/data/pmos` already exists, the installer asks whether to replace it, remove
it, or cancel. Non-interactive runs use `--replace-existing` or
`--remove-existing`. The legacy Android-readable image is deleted only when
`--remove-legacy` is passed explicitly.

`--mode whole` prepares a destructive offline package for a device that will no
longer run Android. It uses a temporary encrypted staging install, packs the
finalized image, copies the matching pmOS `boot`/`init_boot`/`vbmeta` chain and
the Android metadata key into the output folder, and does not erase userdata
while Android is running. Applying that package requires recovery with `/data`
unmounted; it erases `super`, `metadata`, `frp` and `userdata`, writes the ext4
rootfs directly to userdata through dm-default-key, flashes the pmOS boot chain
to slots A and B, preserves firmware/calibration partitions, never writes
`misc`, and leaves slot B active. It requires `--remove-legacy` and typed
confirmation. See `PMOS_ENCRYPTED_STORAGE.md`.

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
- **Panel off.** pmosfb disables the CRTC without blanking the panel, so the
  last scanout can stay visible. `pmos-display-guard.py` drives the real
  backlight bias chip over i2c and treats DRM `DPMS Off` as an authoritative
  blank; it discovers the graphical session through logind, so a username
  other than the old hard-coded `user` still locks/blank correctly.
- **Touch** is interrupt-driven; there is no continuous SPI polling thread. Keep it
  that way to avoid bus contention with scanout.

---

## Audio

Audio is entirely **AP-side ALSA/ASoC + PipeWire**:

- MT6366 codec and eight AW88395 smart amplifiers.
- UCM2 profiles in `deploy/configs/ucm2/`.
- Phosh uses PipeWire with `pipewire-pulse`, `pipewire-alsa` and the BlueZ SPA
  plugin. The PulseAudio backend metapackage is wrong for this desktop: its
  `pulseaudio-wireplumber` package disables WirePlumber's hardware audio and
  Phosh never starts PulseAudio.
- Images built from the stock pmOS base rootfs still select the PulseAudio
  backend. `pmos-audio-backend-migrate.service` (plus a retry timer) performs
  the one-time switch to `postmarketos-base-ui-audio-backend-pipewire` after
  the network is reachable, is idempotent, and records completion in
  `/var/lib/pmos/audio-backend-migrated`.
- `mt6789_audio_pd` and `mt6789_i2s3_mclk` are order-sensitive. Do not change
  their load order or power-domain/clock sequence without hardware testing; a
  mistake can leave playback stalled with `-EIO` or destabilize the boot.
  Their sources and build script live in `deploy/modules/audio/`, and
  `deploy/update_modules_payload.sh` refreshes `deploy/modules.tar.xz` after
  each kernel build so the installer always ships matching modules.
- The headphone-jack watcher is a **user** service
  (`deploy/services/user/pmos-jack-profile.service`). It must run inside the
  graphical session to reach the per-user PipeWire-Pulse socket; the old
  system-wide unit ran as root and could never connect. It accepts the numeric
  `.N` suffix module-alsa-card adds when a sink is recreated; without that it
  resets the card every few seconds and tears down playback.
- `pmos-audio-backend-migrate` restarts `pipewire-pulse.socket` as well as the
  service. pipewire-pulse unlinks `/run/user/UID/pulse/native` on shutdown
  while systemd keeps the listening fd, so restarting only the service leaves
  PulseAudio clients with `Connection refused`.
- `failed to set tdm slot` messages during profile changes are harmless on
  this machine driver; playback still completes.

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
- Appears as **Charge limit** in the Phosh app grid. The Apply button runs
  `/usr/local/bin/pmos-charge-cap` through a sudoers rule that allows only that
  validated helper; a one-tap **Charge limit (quick)** toggle is also installed.

The limit remains user-settable; the current device setting is 80 %.

---

## GNSS

The GNSS stack is **NMEA-only**:

- Vendor `mnld` talks to the MTK GNSS hardware and writes NMEA to
  `/run/tablet-gps/nmea`.
- `tablet-gnss-bridge.py` filters the stream and exposes
  `/run/tablet-gps/nmea.sock`.
- GeoClue uses the `network-nmea` source pointed at that socket.

Both dual-boot and whole-device installs include the complete working GNSS
payload. `deploy/tools/gnss_payload.py` copies the proprietary `mnld`, its
vendor/system library closure, `/vendor/etc/MNL_Config.xml`, `mpe.conf`, and the
Bionic runtime APEX from stock slot A while Android is still running. It also
installs the NMEA-only GeoClue configuration, the socket-activated units, the
stock GPS kernel modules, and the host CA bundle used for EPO downloads. Nothing
proprietary is stored in this repository, and the helper only reads slot A.

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
- **Widevine L3** requires a user-supplied ARM64 CDM and a glibc browser runtime
  (Flatpak Firefox or Chromium); native `firefox-esr` is musl-linked and cannot
  load the glibc CDM. L1/L2 are not possible without the vendor TEE.
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
