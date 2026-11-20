# AGENTS.md

TB351FU (Lenovo Tab Plus, MT6789) postmarketOS kernel and provisioning tree.
This is a GKI 6.12 kernel tree plus a freestanding `/init`, rootfs provisioning
scripts, and a GTK4 installer. The rootfs is a prebuilt Phosh image stored as
`/data/pmos/rootfs.img`; Android stays on slot A.

`/mnt/Nvme` is a data workspace, not this repository. The git root is
`/home/hellopratik/Desktop/pmos/common`.

## Hard safety rules

- **Never write slot A.** Allowed slot-B targets: `boot_b`, `init_boot_b`,
  `vbmeta_b`, `vbmeta_system_b`, `vbmeta_vendor_b`, `vendor_boot_b`, `dtbo_b`.
  Never write `misc`, `super`, `metadata`, `frp`, raw `userdata`, or slot-A
  firmware without explicit approval.
- **`build_and_flash_pmos.sh` flashes automatically when an adb device is
  connected.** Use `--init-only` or `NO_FLASH=1` unless the user explicitly
  asked you to flash. Run `deploy/scripts/flash_slot_b.sh --dry-run` first.
- **Do not reboot after a crash.** Capture `/sys/fs/pstore` and the previous
  journal first; a deliberate reboot can overwrite the evidence. Do not reboot,
  change slots, or flash without explicit approval.
- **`artifacts/boot_latest.img` and `artifacts/init_boot_latest.img` contain
  storage keys embedded in `/init`.** Keys live in
  `~/.config/pmos/storage-keys.env`; never commit, print, copy to the repo, or
  publish those images or the key file.
- **Do not delete `/data/pmos` or `/data/pmos_legacy/rootfs.img`.** The legacy
  image may be the only Android-readable copy. Removal needs explicit approval.
- **`images/` is the keyless public template.** Its boot/init images do not
  contain storage keys, so they cannot unlock this tablet's encrypted
  `/data/pmos`. The current stable build is `artifacts/boot_latest.img` and
  `artifacts/init_boot_latest.img` (Attempt 280 / kernel `#413`); those images
  embed the local storage keys and must never be committed or published. The
  installer now selects the newer of `images/` and `artifacts/`, so a fresh
  local build is used automatically.
- **Per-user keyed chain:** run `install_pmos_rootfs.sh` once to create
  `~/.config/pmos/storage-keys.env`, then
  `NO_FLASH=1 ./build_and_flash_pmos.sh --init-only` to produce matching keyed
  images in `artifacts/`. Pin them with `TB351FU_BOOT_IMAGE`,
  `TB351FU_INIT_BOOT_IMAGE` and `TB351FU_VBMETA_IMAGE` if the timestamps are
  ambiguous.

## Build

```sh
./apply_pmos_kernel_config.sh          # apply arch/arm64/configs/pmos_tb351fu.fragment
NO_FLASH=1 ./build_and_flash_pmos.sh   # package only; writes ../artifacts
./build_and_flash_pmos.sh --init-only  # /init + kernel + images, no flash
```

- Outputs go to `../artifacts` (`ARTIFACTS_DIR` overrides).
- The kernel output tree is `out/`; config is `out/.config`.
- Current release:
  `6.12.23-android16-5-gd2c91f0c3b11-4k-g9cfa397996e7-dirty`.
  Out-of-tree modules must match it; check with
  `modinfo -F vermagic <module.ko>`.
- There is no TB351FU DTS in this tree. The device tree/bootconfig come from
  the stock slot-B `vendor_boot_b` and `dtbo_b`.
- On this host the module toolchain is:
  `export PATH="/mnt/Nvme/evolutionx/sync/prebuilts/kernel-build-tools/linux-x86/bin:/mnt/Nvme/evolutionx/sync/prebuilts/clang/host/linux-x86/clang-r547379/bin:$PATH"`

## Module payload (highest-risk area)

`deploy/modules.tar.xz` is extracted into `/usr/lib/modules/extra` by
`install_pmos_rootfs.sh`. Rebuild it with:

```sh
./deploy/update_modules_payload.sh
```

The payload must contain 34 modules, including the GPU trio
(`panfrost.ko`, `gpu-sched.ko`, `mt6789_mfg_pd.ko`),
`pmos_sensor_bridge.ko`, and the charger stack under `charger/`
(`charger_class.ko`, `sc8989x_charger.ko`, `pmos_charge_cap.ko`,
`pmos_otg_vbus.ko`, plus the `pmos_chg_stub`/`dev-info`/`adapter_class`/
`mtk_charger_algorithm_class`/`mt6375` helpers). GNSS
(`gps_drv_stp.ko`, `gps_pwr.ko`) is separate: `deploy/gnss/modules` is
packaged by `deploy/tools/gnss_payload.py`, not by `modules.tar.xz`.

The charger modules intentionally live in `/usr/lib/modules/extra/charger`,
not the flat payload root: `init_charger.sh` loads them in a fixed order and
expects that directory. The vendor `.ko` files are ignored by git (`*.ko`);
they are staged locally from the stock vendor ramdisk and shipped inside the
tracked `modules.tar.xz`. A fresh checkout that needs to rebuild the payload
must restore them to `deploy/modules/charger/` first.

**`hf_manager.ko` must be the patched build**, sha256
`f2d8828a2ddedcbeb36960a1b0846a5134ac6d4ffab9e87342e9cc5ef0fbeed4`.
The stock vendor binary (`36cf2327…`) uses Android `task_struct` offsets and
NULL-derefs at `hf_client_create+0x84` (faulting instruction `b9470908`) when the
sensor bridge opens it. That is a boot loop, not a sensor bug.
`deploy/tools/patch_hf_manager.py` performs the six-word patch;
`deploy/update_modules_payload.sh` applies it idempotently and verifies the
archive. `deploy/scripts/init_sensors.sh` also refuses to load the bridge unless
the patched hash is present. Verified 2026-11-20 on slot B: `renderD128` exists,
Phoc reports GLES2/`Mali-G57 MC2 (Panfrost)`, and `pmos-accel`, `pmos-gyro`,
`pmos-als` feed `iio-sensor-proxy`.

Module build scripts live in `deploy/modules/*/build.sh`; the bridge source is
`deploy/modules/pmos_sensor_bridge/`. A manual module build is:

```sh
make -C . O=out ARCH=arm64 LLVM=1 M="$PWD/deploy/modules/NAME" \
  KBUILD_MODPOST_WARN=1 modules
```

Load order matters. `init_wifi_sensors.sh` powers MFG and loads the GPU stack
before greetd so Phoc gets GLES2/Panfrost; otherwise it falls back to Pixman.
`init_sensors.sh` brings up the SCP chain
(`mtk-mbox → mtk_rpmsg_mbox → mtk_tinysys_ipi → scp → hf_manager → sensorhub`)
before the bridge. Do not load `ts_scp_common.ko`; touch must stay on the AP.

Charging uses the vendor `charger_class` API, started by
`pmos-charger.service` → `deploy/scripts/init_charger.sh`. The SC8989x charger
cannot bind without `pmos_chg_stub.ko`; `pmos_charge_cap.ko` applies the
persisted limit from `/etc/pmos-charge-cap.conf` (`LIMIT=`, `INPUT_UA=`).
Defaults are 80 % with 5 % hysteresis and 900 mA input current. The installer
also ships the **Charge limit** app-grid entry, an 80 % default
`/etc/pmos-charge-cap.conf`, and a sudoers rule that lets `wheel` run only
`/usr/local/bin/pmos-charge-cap`. Verify with
`/run/pmos_charger_status` (`OK`) and
`/sys/kernel/pmos_charge_cap/{limit,capacity,charging,input_current}`. The
charge-cap and OTG modules are rebuilt by
`deploy/modules/charger/build.sh` and
`deploy/modules/pmos_otg_vbus/build.sh`; their `charger_dev_*` symbols resolve
from `charger_class.ko` at insmod time, so modpost warnings are expected.

## Tests

```sh
cd installer
python3 -m unittest discover -s tests -v    # 20 backend tests
python3 -m tb351fu_installer --self-test    # 15 checks, scripts + images + GTK
python3 -m tb351fu_installer --gui-smoke-test
cd ..
bash -n install_pmos_rootfs.sh
bash -n build_and_flash_pmos.sh
```

`tb351fu-installer --detect` is read-only and safe to run repeatedly.

## Installer and rootfs

- `install_pmos_rootfs.sh` runs on the host and drives Android root adb; the
  device-side half is `deploy/installer/device_install.sh`.
- The GUI requires a pmOS username and password for every rootfs install. Only
  a SHA-512 crypt hash leaves the host. Do not reintroduce a default account or
  silently fall back to `user`.
- The GUI must stay dark-themed, resizable, and fit the 1200×2000 panel.
- A fresh install enables `sshd` on first boot and sets the hostname to
  `lenovo-tab-plus-tb351fu`; the stock base image otherwise keeps
  `pine64-pinephonepro` and leaves SSH disabled.
- Dual-boot mode only touches `/data/pmos`; whole-device mode is destructive
  and erases Android. Whole-device preparation is not an excuse to write
  `misc` or raw userdata while Android is running.
- `/data/pmos` is fscrypt v2. To inspect or repair `rootfs.img` from Android,
  add the key with `deploy/tools/fscrypt_helper addkey2f /data KEYFILE`, mount
  the ext4 image read-write, then `delkey2` and remove the key file. Never
  leave the key in the Android keyring.

## Device access and known gaps

- USB network: tablet `172.16.42.1`, host `172.16.42.2`. Root telnet is port
  23; SSH may be disabled in the installed image.
- Current rootfs uses hostname `pine64-pinephonepro`; a TB351FU-specific name
  still needs to be set by the installer.
- Keep GeoClue NMEA-only, audio on the AP side (PipeWire), camera modules on
  manual load, and microSD/USB storage on udisks2. These are intentional.
- `README.md` is the primary guide. `verification.md` is the newest bring-up
  log; `pmos_kernel.md` and `device.md` are historical and contain stale paths.
  `images/README.md` has the partition map, but see the stale-bundle warning
  above.
