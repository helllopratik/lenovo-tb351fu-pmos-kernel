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
- **Dual boot or destructive whole-device package:** dual boot keeps Android
  and installs into `/data/pmos`. Whole-device mode prepares a destructive
  offline package in temporary encrypted staging: applying it from recovery
  erases Android (super, metadata, frp and userdata), writes pmOS directly to
  userdata through dm-default-key, and replaces the boot chain in slots A and
  B. Android is not changed while the package is being prepared.
- **Existing installs are explicit:** the wizard detects `/data/pmos`, its
  marker and `/data/pmos_legacy/rootfs.img`, then asks whether to replace or
  remove. The legacy image is only deleted when its checkbox is selected.
- **Android keeps breathing room.** The wizard suggests 64 GiB (capped by free
  space minus an 8 GiB reserve) and the helper script refuses to leave less
  than 1 GiB free on `/data`.
- **Remote access on first boot.** The installed rootfs enables `sshd` and sets
  the hostname to `lenovo-tab-plus-tb351fu` (the stock base image would leave
  `pine64-pinephonepro` and SSH disabled).

## Audio stack

The installer payload always includes the two MT6789 audio helper modules
(`mt6789_audio_pd.ko`, `mt6789_i2s3_mclk.ko`) in `deploy/modules.tar.xz`; they
are rebuilt from `deploy/modules/audio/` after each kernel build and loaded in
order by `init_wifi_sensors.sh`. Do not remove them from the payload.

It also carries the vendor charger stack and helpers under `charger/`
(`charger_class.ko`, `sc8989x_charger.ko`, `pmos_charge_cap.ko`,
`pmos_otg_vbus.ko`); `init_charger.sh` loads them from
`/usr/lib/modules/extra/charger`. A fresh install gets the 80 % default
`/etc/pmos-charge-cap.conf`, the **Charge limit** app-grid entries, and a
sudoers rule limited to `/usr/local/bin/pmos-charge-cap`.

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

## GNSS payload

Every dual-boot and whole-device install includes the working NMEA-only GNSS
stack. The proprietary MediaTek daemon cannot be redistributed, so
`deploy/tools/gnss_payload.py` collects it from stock slot A while Android is
still running:

- `/vendor/bin/mnld`, `/vendor/etc/MNL_Config.xml`, `mpe.conf`, and the
  `DT_NEEDED` closure (28 vendor libraries plus `mnld` in the reference
  build),
- the system namespace closure under `/system/lib64` (14 libraries), resolved
  system-first and verified against the actual APEX contents,
- the Bionic runtime APEX, packed as a 14,884,864-byte ext2
  `runtime-apex.img`,
- the scripts, shim, socket/service units, stock `gps_pwr.ko` and
  `gps_drv_stp.ko`, NMEA-only `geoclue.conf`, and 121 host CA certificates for
  EPO downloads.

The helper only reads slot A; it never writes Android, `misc`, or the boot
chain. A missing `mnld`, `MNL_Config.xml`, or unresolved library aborts the
package before anything is transferred to the tablet. The GNSS step runs before
the rootfs payload is applied, so both install modes get the same files.

Building the APEX image needs host `mke2fs` and `e2fsck` (e2fsprogs) in addition
to `zstd`.

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
| boot | newer of `images/boot_b.img` and `artifacts/boot_latest.img` |
| init_boot | newer of `images/init_boot_b.img` and `artifacts/init_boot_latest.img` |
| vbmeta | newer of `images/vbmeta_b.img` and `artifacts/vbmeta_latest.img` |
| vendor_boot | `slot_b_bundle/vendor_boot.img` |
| dtbo | `slot_b_bundle/dtbo.img` |
| slot-B firmware | `slot_b_bundle/{lk,tee,gz,scp,sspm,mcupm,dpm,spmfw,pi_img}.img` |
| rootfs | `images/rootfs_base.img.zst` (release asset), then `images/rootfs_base.img` |

A local `build_and_flash_pmos.sh --init-only` writes the current keyed boot
chain to `artifacts/`. When those files are newer than the packaged `images/`
copies, the installer selects them automatically, so it never flashes a stale
kernel after a rebuild. Set `TB351FU_BOOT_IMAGE`, `TB351FU_INIT_BOOT_IMAGE` or
`TB351FU_VBMETA_IMAGE` to pin an explicit chain, and `ARTIFACTS_DIR` if the
build output lives elsewhere. `images/` remains the keyless public template:
do not copy the keyed `artifacts/` images into git.

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
   - **Mode:** dual boot (Android kept) or destructive whole-device package
     preparation. Whole-device mode uses an output folder, does not flash or
     switch slot B, and does not erase Android while preparing. Applying the
     package later erases Android and replaces the boot chain in slots A and
     B. Removal and whole-device mode are mutually exclusive.
   - **Image source:** download from the server (URL plus optional SHA-256) or
     use the local image selected on the Images page.
   - **pmOS login:** username, password and confirmation. Both fields are
     required when a rootfs is installed, and the entered name is applied on
     the tablet. The password is hashed on the host with SHA-512 crypt; only
     the hash is sent. Whole-device preparation uses the same login.
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
| `TB351FU_BOOT_IMAGE` | pin the `boot_b` image |
| `TB351FU_INIT_BOOT_IMAGE` | pin the `init_boot_b` image |
| `TB351FU_VBMETA_IMAGE` | pin the vbmeta image |
| `TB351FU_VENDOR_BOOT_IMAGE` | pin the vendor_boot image |
| `TB351FU_DTBO_IMAGE` | pin the dtbo image |
| `TB351FU_ROOTFS_IMAGE` | override the auto-detected rootfs image |
| `PMOS_IMAGE_URL` | default server rootfs URL for the auto source |
| `ARTIFACTS_DIR` | override the boot/init_boot/vbmeta build directory |
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

# prepare a destructive whole-device package; Android is not erased yet
../install_pmos_rootfs.sh --mode whole --source auto \
  --whole-output ../artifacts/whole_device_prep \
  --whole-boot-image ../artifacts/boot_latest.img \
  --whole-init-boot-image ../artifacts/init_boot_latest.img \
  --whole-vbmeta-image ../artifacts/vbmeta_latest.img \
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
tablet. The GUI always asks for the username, password and confirmation when a
rootfs is installed and creates the hash file with mode 0600, passes it to the
script, and deletes it when the install finishes. The interactive script
prompts for a username with no default and then for the password.
`--password-hash-file` must contain one `$6$...` hash. `--password-file` still
accepts a plaintext password for unattended runs, but the hash form avoids
sending one. `--no-account` remains available only as an explicit advanced
override that keeps the image's default account.

`--mode whole` requires an empty output folder (or a new one), the existing
`PMOS_DM_METADATA_KEY` in the storage key file, and a matching pmOS boot chain
(`boot_latest.img`, `init_boot_latest.img`, `vbmeta_latest.img`). The package
contains `rootfs.sparsepack`, `sparse_pack`, `boot.img`, `init_boot.img`,
`vbmeta.img`, `pmos-dm-key` (0600), `pmos-dm.conf`, `install_whole_device.sh`,
`README.txt` and `SHA256SUMS`. Run it only from recovery with `/data` unmounted:

  PMOS_WHOLE_CONFIRM='ERASE ANDROID SLOT A' \
    sh install_whole_device.sh --remove-legacy

The apply erases `super`, `metadata`, `frp` and `userdata`, writes the rootfs
directly to userdata with dm-default-key, flashes pmOS boot/init_boot/vbmeta to
slots A and B, preserves firmware/calibration partitions, never writes `misc`,
and leaves slot B active. Add `--dry-run` to print the plan without writing.

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
- **Whole-device mode cannot find the boot chain**: select the pmOS
  `boot_latest.img`, `init_boot_latest.img` and `vbmeta_latest.img` on the
  Images page, or set `PMOS_WHOLE_BOOT_IMAGE` and friends. The apply package
  needs the same storage key that built those images.
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
