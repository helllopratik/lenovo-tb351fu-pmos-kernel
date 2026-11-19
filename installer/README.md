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
  loopback ext4 image inside Android userdata. The directory is protected with
  a per-install fscrypt key; its size is selectable and can be resized later
  with the normal rootfs tools.
- **Dual boot or whole-device package:** dual boot keeps Android and installs
  into `/data/pmos`. Whole-device mode only prepares an offline package in a
  temporary encrypted staging directory. It never erases or reformats
  userdata while Android is running.
- **Existing installs are explicit:** the wizard detects `/data/pmos`, its
  marker and `/data/pmos_legacy/rootfs.img`, then asks whether to replace or
  remove. The legacy image is only deleted when its checkbox is selected.
- **Android keeps breathing room.** The wizard suggests 64 GiB (capped by free
  space minus an 8 GiB reserve) and the helper script refuses to leave less
  than 1 GiB free on `/data`.

## Audio stack

The installer payload always includes the two MT6789 audio helper modules
(`mt6789_audio_pd.ko`, `mt6789_i2s3_mclk.ko`) in `deploy/modules.tar.xz`; they
are rebuilt from `deploy/modules/audio/` after each kernel build and loaded in
order by `init_wifi_sensors.sh`. Do not remove them from the payload.

The stock pmOS base image still selects
`postmarketos-base-ui-audio-backend-pulseaudio`, which leaves Phosh silent
because `pulseaudio-wireplumber` disables WirePlumber's hardware audio and
PulseAudio is never started. Every installation therefore gets:

- `pmos-audio-backend-migrate.service`, a guarded one-time migration to
  `postmarketos-base-ui-audio-backend-pipewire`. It waits for the network,
  treats the known mkinitfs trigger failure on this custom boot chain as
  non-fatal, restores `/etc/apk/world` on real failure, and retries through
  `pmos-audio-backend-migrate.timer` every 10 minutes until it succeeds.
- A completion marker at `/var/lib/pmos/audio-backend-migrated`, so the
  migration is a no-op afterwards.
- `pmos-jack-profile.service` under `/usr/lib/systemd/user/`, enabled through
  `/etc/systemd/user/default.target.wants/`. It runs inside the graphical
  session and drives the PipeWire-Pulse card profile when headphones are
  plugged or unplugged.

Audio output is verified with `speaker-test -D pulse -c 2 -t sine -f 440 -l 1`
after the migration and a restart of `pipewire`, `wireplumber` and
`pipewire-pulse` in the user session.

## Requirements

Host:

- Linux with Python 3, PyGObject + GTK 4 (`python3-gi`, `gir1.2-gtk-4.0`)
- `adb` and `fastboot` in `PATH`
- `perl` or `openssl` to hash the pmOS password on the host
- `curl` or `wget` only when installing from the server
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
   slot, root access and battery. It also reads the existing pmOS state from
   `/data` without modifying it. `Refresh` re-scans. Buttons reboot to
   bootloader or back to the device.
2. **Partitions and size** — lists every by-name partition with size, slot,
   role and block node, reads `/data` usage, then suggests a rootfs size. You
   can lower or raise it (4 GiB–256 GiB) before continuing. Whole-device mode
   ignores this value and uses the real userdata size.
3. **Images** — asks for the slot-B bundle folder (default
   `/mnt/Nvme/slot_b_bundle`) and auto-detects the boot chain, `vendor_boot`,
   `dtbo`, the slot-B firmware and the rootfs. It validates magic and partition
   limits, and lets you override each path. Choose whether to flash the boot
   chain, provision the firmware, install the rootfs, and switch to slot B.
4. **Installation options** — choose the mode, image source and account:
   - **Existing installation:** replace the rootfs, or remove the installation
     and exit. Removing the legacy `/data/pmos_legacy/rootfs.img` is a separate
     checkbox and is never implicit.
   - **Mode:** dual boot (Android kept) or whole-device package preparation.
     Whole-device mode uses an output folder, does not flash slot B and never
     erases Android. Removal and whole-device mode are mutually exclusive.
   - **Image source:** download from the server (URL plus optional SHA-256) or
     use the local image selected on the Images page.
   - **pmOS login:** username, password and confirmation. The password is
     hashed on the host with SHA-512 crypt; only the hash is sent to the tablet.
     Disable the account to keep the image's default account.
5. **Install** — shows the exact plan in a confirmation dialog, then streams
   the helper output with progress. Slot B is only written after you confirm.
   On success the wizard reports "Installation complete", "Removal complete"
   or "Whole-device package ready" as appropriate.

In fastboot mode the rootfs option is disabled: only the boot chain can be
written. Boot Android with root adb to install the rootfs or prepare a
whole-device package.

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
| `PMOS_IMAGE_URL` | default server rootfs URL for the auto source |
| `ARTIFACTS_DIR` | override the boot/init_boot/vbmeta directory |
| `ANDROID_SERIAL` | target a specific adb/fastboot serial |

## Helper scripts

The GUI is a front end for the proven scripts; they remain usable directly.
`install_pmos_rootfs.sh` needs `zstd` and `e2fsprogs` on the host: it decompresses
the image, clears `orphan_file` for Android's older `e2fsck`/`resize2fs`, and
pushes a raw image. The device needs root adb and about 64 GiB free on `/data`.

```bash
# rootfs only, ask for source and account (Android + root adb)
../install_pmos_rootfs.sh --mode dual

# rootfs from the configured server, unattended hash-only account
../install_pmos_rootfs.sh --source auto --mode dual \
  --username tablet --password-hash-file /path/to/password.hash

# local rootfs image
../install_pmos_rootfs.sh --source manual --image /path/to/pmos.img \
  --size 64G --mode dual

# replace an existing dual-boot installation
../install_pmos_rootfs.sh --source auto --mode dual --replace-existing

# remove an existing installation (legacy image kept unless requested)
../install_pmos_rootfs.sh --mode dual --remove-existing --yes

# prepare an offline whole-device package; Android is not erased
../install_pmos_rootfs.sh --mode whole --source auto \
  --whole-output ../artifacts/whole_device_prep \
  --username tablet --password-hash-file /path/to/password.hash

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

`install_pmos_rootfs.sh` and the GUI send only a SHA-512 crypt hash to the
tablet. `--password-hash-file` must contain one `$6$...` hash. The GUI creates
that file with mode 0600, passes it to the script, and deletes it when the
install finishes. `--password-file` still accepts a plaintext password for
unattended runs, but the hash form avoids sending one.

`--mode whole` requires an empty output folder (or a new one). The package
contains `rootfs.sparsepack`, `sparse_pack`, `install_whole_device.sh`,
`README.txt` and `SHA256SUMS`; run it from recovery only, with userdata
unmounted.

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
- **"an installation already exists"**: choose Replace or Remove in
  Installation options, or pass `--replace-existing` / `--remove-existing` to
  the helper. The GUI sets these flags for you.
- **Whole-device output is not empty**: move the previous package away. The
  helper refuses to mix a new package with old files.
- **Password hashing fails**: install `perl` or `openssl` on the host. Only the
  hash is required; the GUI never writes the plaintext password to disk.
- **Legacy image left behind**: `/data/pmos_legacy/rootfs.img` is intentionally
  kept unless Remove is combined with the legacy checkbox (or
  `--remove-legacy` is passed).

## Tests

```bash
cd installer
python3 -m unittest discover -s tests -v
python3 -m tb351fu_installer --self-test
```
