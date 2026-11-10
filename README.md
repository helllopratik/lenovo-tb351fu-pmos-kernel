# postmarketOS Linux Kernel (GKI 6.12) for Lenovo Tab Plus (TB351FU)

This repository contains the customized Linux GKI 6.12 kernel source, hardware drivers, userspace provisioning tools, and boot packaging scripts for dual-booting **postmarketOS (Phosh UI)** on the **Lenovo Tab Plus (TB351FU / Helio G99 / MT6789 / MT8781)** via Direct LK Boot.

---

## 1. Dual-Boot Architecture Overview

The device uses an A/B partition layout. PostmarketOS is configured as a completely non-destructive dual-boot setup:

* **Slot A**: Preserved **Stock Android** system (safe fallback). Never modified.
* **Slot B**: Dedicated to **postmarketOS kernel & ramdisk**:
  - `boot_b` (`/dev/block/sdc50`, 64 MB): GKI 6.12 Linux kernel with AVB hash footer.
  - `init_boot_b` (`/dev/block/sdc52`, 8 MB): Freestanding `/init` ramdisk (LZ4 legacy frame) with AVB hash footer.
  - `vbmeta_b` (`/dev/block/sdc10`, 4 KB): Verification disabled (flags = 3).
  - `vendor_boot_b` & `dtbo_b`: Copied from Slot A to provide hardware device tree blobs (DTB) and bootconfig to LK.
* **Root Filesystem (RootFS)**:
  - Resides on internal UFS userdata (`/dev/block/sdc64`) at `/data/pmos/rootfs.img` (64 GB ext4 loopback image).
  - Mounted by `pmos_init` via `/dev/loop0`.
  - Android userdata is kept mounted on `/data_host` (inside postmarketOS at `/data_host`) so the loopback backing file remains permanently open.

---

## 2. Directory Structure

* **`common/`**: Linux GKI 6.12 kernel source tree.
  - `drivers/gpu/drm/tiny/pmosfb.c`: DRM framebuffer driver with Point-of-Coherency hardware cache flush (`dcache_clean_poc`) and native 60Hz pacing.
  - `drivers/input/touchscreen/nt36xxx/`: Novatek NT36523N TDDI touchscreen driver with hardware interrupt processing and zero SPI bus contention.
  - `arch/arm64/kernel/setup.c`: MediaTek TopRGU hardware watchdog integration, display carveout reservation (`0xfe2b0000`), and priority 255 instant restart handler.
  - `pmos_init_src/pmos_init.c`: Freestanding aarch64 `/init` implementation that attaches `/dev/sdc64`, mounts `/data_host/pmos/rootfs.img` onto `/root`, and executes `/sbin/init`.
* **`common/deploy/`**: Turnkey postmarketOS hardware provisioning assets:
  - `modules.tar.xz`: Out-of-tree vendor modules (`bluetooth.ko`, `wmt_drv.ko`, `connfem.ko`, `btif_drv.ko`, `hci_uart.ko`, etc.).
  - `scripts/`: System bring-up scripts (`init_wifi_sensors.sh`, `init_bt.sh`, `pmos-usb-setup.sh`, `phosh-session`, etc.).
  - `root_scripts/`: Hardware initialization scripts (`btbridge.py`, `early_xo.py`, `wmtshim.py`, etc.).
  - `services/`: Systemd services (`pmos-hardware-init.service`, `pmos-usb-network.service`, etc.).
  - `configs/`: Display configuration (`phoc.ini`), udev rules, sysctl panic parameters, and watchdog configs.
* **`tools/mkbootimg/`**: Android boot image creation tool (`mkbootimg.py`).
* **`external/avb/`**: Android Verified Boot signing tools (`avbtool.py`) and test RSA-4096 keys.

---

## 3. Step-by-Step Installation Guide

### Prerequisites
1. **Host Machine**: Linux with `adb`, `fastboot`, `python3`, `zstd`, `lz4`, `tar`, and `llvm`/`clang` (LLVM 20 or Android Clang `clang-r547379`).
2. **Device**: Lenovo Tab Plus (TB351FU) with unlocked bootloader.
3. **postmarketOS Base Image**: An aarch64 Phosh image (e.g. `pmos-phosh-aarch64.img`).

---

### Step 1: Install & Provision the PostmarketOS RootFS

Boot the tablet into Android (Slot A), ensure USB debugging is enabled and authorized, then run the automated rootfs installer:

```bash
./install_pmos_rootfs.sh /path/to/pmos-phosh-aarch64.img
```

#### What this script does automatically:
1. Validates root ADB access on the tablet.
2. Extracts partition 2 from the postmarketOS image and pushes it compressed with `zstd` to the device.
3. Decompresses the image to `/data/pmos/rootfs.img` on internal UFS userdata.
4. Expands the ext4 filesystem to **64 GB** using `resize2fs`.
5. Loopback mounts the image and **strictly enforces merged `/usr` symlinks**:
   - `/lib -> usr/lib`
   - `/bin -> usr/bin`
   - `/sbin -> usr/sbin`
   *(Prevents dynamic linker breakage; see Pitfalls below).*
6. Installs all hardware modules into `/usr/lib/modules/extra/`.
7. Installs and enables systemd bring-up services for Bluetooth, USB networking, and hardware peripherals.
8. Configures Phosh display to native 60Hz portrait 180° mode (`scale = 2`, `rotate = 180`). The unlock-time rotation reset needs the patched phosh build in `deploy/phosh/`; see that directory's README.
9. Disables the non-existent EFI `/boot` entry in `/etc/fstab` to eliminate 90s boot stalls.
10. Executes a preflight sanity check (`chroot ... systemd --version`) to confirm userspace binary integrity before completing.

---

### Step 2: Build & Flash the Kernel to Slot B

With the device still connected in Android via ADB, compile and flash the kernel, ramdisk, and vbmeta:

```bash
./build_and_flash_pmos.sh
```

#### What this script does:
1. Compiles the freestanding `/init` binary and packages it into `pmos_initramfs_194.cpio.lz4` using **LZ4 legacy frame format** (`lz4 -l -9`).
2. Builds the GKI 6.12 kernel `Image` using Clang/LLVM.
3. Generates and signs `boot_latest.img` (64 MB) and `init_boot_latest.img` (8 MB) with AVB hash footers.
4. Generates `vbmeta_latest.img` with verification flags disabled (`--flags 3`).
5. Flashes exclusively to **Slot B** (`boot_b`, `init_boot_b`, `vbmeta_b`, `vbmeta_system_b`, `vbmeta_vendor_b`) via `dd`.
6. Compares partition MD5 checksums on device against host build artifacts.
7. Does **not** reboot the device, leaving you in full control.

---

### Step 3: Boot into postmarketOS

Reboot into fastboot, set Slot B active, and boot:

```bash
adb reboot bootloader
fastboot set_active b
fastboot reboot
```

The device will boot through LK, mount `/data_host/pmos/rootfs.img`, launch `systemd 261`, and start the graphical Phosh desktop.

---

## 4. Accessing postmarketOS (Networking & Terminal)

The system automatically configures USB gadget networking on boot:
* **Tablet IP**: `172.16.42.1`
* **Host IP**: `172.16.42.2` (assigned automatically or configure on `enx020042000001`)

### Shell Access
* **Root Telnet Shell**:
  ```bash
  telnet 172.16.42.1
  ```
* **SSH**:
  ```bash
  ssh user@172.16.42.1  # password: user (or root)
  ```

---

## 5. Critical Hard-Won Facts & Pitfall Prevention

### A. The Merged `/usr` Symlink Trap (`/lib -> usr/lib`)
* **Problem**: In postmarketOS (Alpine / musl), `/lib` is a symlink to `usr/lib`. All ELF binaries request `/lib/ld-musl-aarch64.so.1`. If an installation tarball or script creates a real directory `/lib/` on the rootfs, it clobbers the symlink.
* **Result**: `/lib/ld-musl-aarch64.so.1` ceases to exist, and `execve("/sbin/init")` instantly fails with `ENOENT` (`-2`: No such file or directory).
* **Fix**: Always install modules into `/usr/lib/modules/extra/` and ensure `/lib` remains a symlink pointing to `usr/lib`.

### B. Display Cadence & TDDI Glitch Elimination
* **Hardware Reality**: The NT36523N TDDI panel integrates both display driver and touch controller into a single chip.
* **Problem**: Running touch background polling threads or attempting 90Hz modeline overrides causes bus contention over SPI2 during display scanout, creating horizontal line artifacts and pixel glitches.
* **Fix**:
  - Keep display locked to native **60Hz** in DRM driver and `/etc/phosh/phoc.ini` (`scale = 2`, `rotate = 180`).
  - Rely purely on hardware interrupt-driven touch handling without continuous scan polling.

### C. Ramdisk Compression (`lz4 -l -9`)
* **Problem**: The MediaTek LK bootloader supports only the legacy LZ4 frame format (`0x184C2102`). Using standard `lz4 -9` or `xz -9` causes LK's decompressor to fail silently. The kernel falls back to treating block memory as an initrd, falsely matches `MINIX_SUPER_MAGIC`, and crashes with a `64-bit el1t` SError exception.
* **Fix**: Always package the ramdisk with `lz4 -l -9 -f`.

### D. Auto-Reboot on Crash vs. Smooth Boot
* **Kernel Command Line**:
  ```text
  panic=5 panic_on_oops=1 hung_task_panic=1 hung_task_timeout_secs=60
  ```
* **Behavior**:
  - **Normal Operation**: When booting and running services, the reboot watchdog is completely disarmed so boot completes uninterrupted.
  - **Fatal Lockup**: If a kernel panic occurs or a task hangs in `D` state for >60s, the MediaTek TopRGU hardware watchdog triggers a clean warm restart after 5 seconds.

### E. MediaTek MT6789 Audio DMA & AFE Agent Gatekeeping
* **Hardware Reality**: On MT6789 (Helio G99 / MT8781), the MediaTek Audio Front End (AFE) memory interface channels (DL1, DL2..DL8, DL12) require asserting the `AFE_AGENT_ON` register bank (`0x11211b00` = `AFE_AGENT_ON`, `0x11211b04` = `AFE_AGENT_ON_SET`, `0x11211b08` = `AFE_AGENT_ON_CLR`) in order to fetch audio PCM data from DRAM over system bus.
* **Problem**: Upstream `mt8186-afe-pcm.c` only toggled bit 0 of `AFE_DAC_CON0` (`0x11210010`). On MT6789, `AFE_DAC_CON0` bit 0 enables the audio AFE clock core, but the DMA bus master engine remains unclocked until bit `enable_shift` (bit 4 for DL1) is written to `AFE_AGENT_ON_SET`. Without this, `AFE_DL1_CUR` never advances past 0, causing ALSA PCM playback (`aplay`, `speaker-test`, `paplay`) to stall and fail with `-EIO` after a 10-second timeout.
* **Fix**:
  - Added definitions for `AFE_AGENT_ON`, `AFE_AGENT_ON_SET`, and `AFE_AGENT_ON_CLR` to `sound/soc/mediatek/mt8186/mt8186-reg.h`.
  - Expanded `AFE_MAX_REGISTER` from `0x18bc` to `0x1c00` so regmap boundaries allow reading/writing `0x1b00..0x1b08`.
  - Updated `mtk_memif_set_enable()` and `mtk_memif_set_disable()` in `sound/soc/mediatek/common/mtk-afe-fe-dai.c` to assert and clear agent channels.
  - In the AW88395 smart amplifier codec driver, `0` represents 0 dB attenuation (full volume) and `720` (`AW88395_MUTE_VOL`) represents hardware mute. Configured ALSA UCM2 (`mt8186.conf` / `HiFi.conf`) to initialize all 8 amplifier volume registers to 0 dB attenuation at boot.

---

## 6. Switching Back to Android

Whenever you want to return to stock Android:

```bash
# From fastboot:
fastboot set_active a
fastboot reboot

# Or from postmarketOS shell:
pmos-reboot android  # or: bootctl set-active-boot-slot 0 && reboot
```
Android on Slot A remains 100% intact with all user data preserved.
