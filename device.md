# Lenovo Tab Plus (TB351FU) — postmarketOS & Kernel Bring-Up Documentation

**Device:** Lenovo Tab Plus (TB351FU)  
**SoC:** MediaTek Helio G99 / MT6789 / MT8781 (board `t808aa`)  
**Architecture:** ARM64 (`aarch64`)  
**Kernel Base:** Google GKI Linux 6.12 (Android 16 / 17 compatible)  
**Partition Scheme:** A/B Seamless Slot System (`slot_a` = Stock Android 17 / Custom ROM, `slot_b` = postmarketOS)  
**Display / Panel:** 11.5" LCD 1200×2000 @ 60/90 Hz (BOE / CSOT, Novatek NT36523 TDDI)  
**Storage:** Internal UFS 2.2 + External MicroSD (`/dev/mmcblk0`)  

---

## 1. Dual-Slot Boot & Partition Architecture

The tablet operates in a dual-boot A/B layout without repartitioning internal UFS:
* **Slot A**: Stock Android 17 / Evolution OS / TWRP (Kernel GKI 6.12.15).
* **Slot B**: postmarketOS standalone installation with custom GKI 6.12 kernel.
* **RootFS**: A loopback ext4 image at `/data/pmos/rootfs.img` inside Android UFS userdata, mounted through `/dev/loop0` by the freestanding `/init` in `init_boot_b`. MicroSD remains available as removable storage.
* **Boot Images**:
  * `boot_b` (64 MB): Contains compressed GKI 6.12 kernel image (`Image.lz4`), Header Version 4, AVB testkey signed.
  * `init_boot_b` (8 MB): Contains freestanding C-compiled `/init` and initramfs CPIO archive.
  * `vbmeta_b`, `vbmeta_system_b`, `vbmeta_vendor_b`: Signed with AVB verification flags set to `3` (verification disabled).

---

## 2. Display Pipeline & DRM Architecture

1. **Hardware State Handoff**:
   * LittleKernel (LK) initializes the physical panel during early boot at `1200x2000` resolution (32-bit ARGB8888, base physical address `0xfe2b0000`, pitch/stride `4864` bytes).
   * Hardware PWM backlight is preserved at brightness level `1200`.

2. **DRM pmosfb driver**:
   * `drivers/gpu/drm/tiny/pmosfb.c` samples the live DSI/panel pipeline that LK initialized at `0xfe2b0000` and exposes it as a DRM framebuffer.
   * Dirty rectangle updates execute `dcache_clean_inval_poc()` (Point-of-Coherency cache flushing) to guarantee direct scanout.
   * The driver starts at **60 Hz** for a stable boot handoff. After boot, `pmos-display-mode` applies the saved 60 Hz, 90 Hz, or Adaptive mode.
   * Atomic plane blanking (`memset 0`) is removed to prevent the screen vanishing on compositor idle transitions, and no startup animation is used.

3. **Wayland Compositor Configuration (`phoc` / `phosh`)**:
   * Compositor: `/usr/bin/phoc` with software pixman rendering (`WLR_RENDERER=pixman`, `WLR_RENDERER_ALLOW_SOFTWARE=1`, `WLR_NO_HARDWARE_CURSORS=1`, `LIBGL_ALWAYS_SOFTWARE=1`).
   * Shell: `/usr/libexec/phosh` (Mobile GNOME Shell).
   * Seat Management: Managed via `seatd` (`/run/seatd.sock`, `LIBSEAT_BACKEND=seatd`) on `seat0`.

---

## 3. Userspace & RootFS Optimization

1. **VT / Console Allocation**:
   * `getty@tty1.service` is masked (`systemctl mask getty@tty1.service`) to eliminate VT contention so `greetd.service` exclusively owns `/dev/tty1`.
2. **Device Node & Directory Permissions (`fix-dev-perms.service`)**:
   * `devtmpfs` creates `/dev/dri` and `/dev/input` with mode `0666` (lacking `+x`).
   * `fix-dev-perms.service` runs early in boot (`sysinit.target`) to restore mode `0755` on `/dev/dri` and `/dev/input` and initialize `/run/user/10000` (`user:user`, `0700`), allowing non-root users to allocate DRM dumb buffers and open touch event nodes.
3. **Automated Standalone Boot (Zero PC Interaction)**:
   * Masked blocking services: `postmarketos-zram-swap.service`, `sys-kernel-tracing.mount`, `NetworkManager-wait-online.service`, `usb-signaller.service`.
   * Boot time from LK handoff to Phosh lockscreen: **< 1.0 second**.

---

## 4. Touchscreen & Digitizer Subsystem (Novatek NT36523 TDDI)

1. **Hardware Bus & Topology**:
   * Interface: SPI2 (`11012000.spi2`), Chip Select CS0 (`spi2.0`).
   * Pins & Pinmux:
     * GPIO38 -> Mode 1 (`SPI2_CLK`), IOCFG_LM offset `0x00E0` bit 16 (IES enable), offset `0x01F0` bit 13 (SMT enable).
     * GPIO39 -> Mode 1 (`SPI2_CSB`), IOCFG_LM offset `0x00E0` bit 17 (IES enable), offset `0x01F0` bit 14 (SMT enable).
     * GPIO40 -> Mode 1 (`SPI2_MO`), IOCFG_LM offset `0x00E0` bit 18 (IES enable), offset `0x01F0` bit 15 (SMT enable).
     * GPIO41 -> Mode 1 (`SPI2_MI`), IOCFG_LM offset `0x00E0` bit 19 (IES enable), offset `0x01F0` bit 16 (SMT enable).
     * GPIO36 -> Mode 0 (`GPIO36`), `DIR=1` (Output), `DO=1` (RST Deasserted High).
     * GPIO9  -> Mode 0 (`EINT9`), IOCFG_RT offset `0x0080` bit 0 (IES enable), IRQ Trigger `IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING`.
   * Input Nodes:
     * `/dev/input/event0` -> `NVTCapacitiveTouchScreen` (`type=touch`, 1200×2000 native resolution, 40 res/mm).
     * `/dev/input/event1` -> `NVTCapacitivePen` (`type=tablet`, 2400×4000 resolution, pressure & tilt).
2. **Pinctrl & Pinmux Masking Fix**:
   * In `drivers/pinctrl/mediatek/pinctrl-paris.c`, pin mode nibbles in `GPIO_BASE + 0x340` (pins 38-39) and `0x350` (pins 40-41) are explicitly masked before writing function 1:
     ```c
     v &= ~((0xFU << 24) | (0xFU << 28)); v |= (1U << 24) | (1U << 28);
     ```
   * Hardware Schmitt Trigger input buffers (`IES` and `SMT`) are enabled on `IOCFG_LM` (`0x11E20000`) and `IOCFG_RT` (`0x11EA0000`) so the MediaTek SPI controller physically latches incoming MISO signals.
3. **Driver Implementation (`nt36xxx.c` & `nt36xxx_ext_proc.c`)**:
   * Novatek Engineering Reset (`nvt_eng_reset()`) is executed during probe (`0x03F0FE` <- `0x5A`) to awaken the TDDI SPI slave engine after power-on.
   * `nvt_get_fw_info()` recognizes the NT36523 firmware version (`0x60` / `96`, matrix `88x142` channels).
   * Polling kthread (`nvt_ts_poll`) removed to keep touch purely interrupt-driven with zero SPI bus contention.
   * `libinput list-devices` binds cleanly to `NVTCapacitiveTouchScreen` on `seat0`.

### 4.1 Bluetooth pen (Lenovo Tab Pen Plus) gestures

The BT pen is a BLE HID device (`17ef:617f`). Its firmware detects
single/double/triple/long/press itself and sends them in HID report ID 2 as
vendor Consumer usages `0x0c0600`–`0x0c0605` (one bit each). Linux hid-input
does not know these usages, so it reports `KEY_UNKNOWN` and preserves the usage
in `MSC_SCAN`.

Live capture on stock Android (`/dev/input/event12`,
"Lenovo Tab Pen Plus Consumer Control"):

```
EV_MSC MSC_SCAN 000c0601
EV_KEY KEY_UNKNOWN DOWN
EV_KEY KEY_UNKNOWN UP
```

`/dev/input/event11` ("Lenovo Tab Pen Plus Mouse") stays silent for button
gestures, so **no hidraw reader is needed** — evdev + `MSC_SCAN` is sufficient.
The AES pen on `NVTCapacitivePen` is a separate input node.

`pmos-pen-daemon` (deploy `scripts/pmos-pen-daemon`) decodes the usage and
either injects a key through `/dev/uinput` or runs a per-gesture shell command:

| Usage | Gesture | Default (`MODE=media`) |
|---|---|---|
| `0x0c0600` | single | `KEY_PLAYPAUSE` |
| `0x0c0601` | double | `KEY_NEXTSONG` |
| `0x0c0602` | triple | `KEY_PREVIOUSSONG` |
| `0x0c0603` | long | `KEY_MUTE` |
| `0x0c0604` | press | `KEY_PLAYPAUSE` |
| `0x0c0605` | spare | — |

`MODE=presentation` gives page down/page up on single/double; `MODE=camera`
gives volume down (shutter) and camera launch; `MODE=none` disables keys.
`SINGLE_CMD`…`PRESS_CMD` in `/etc/pmos-pen-daemon.conf` override any gesture
with a shell command.

Install when slot B is running:

```sh
sudo install -m755 pmos-pen-daemon /usr/local/bin/
sudo install -m644 pmos-pen-daemon.conf /etc/
sudo install -m644 pmos-pen-daemon.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now pmos-pen-daemon
journalctl -u pmos-pen-daemon -f
```

Test without injecting keys: `pmos-pen-daemon --dry-run --verbose`.
Replay a captured event stream: `--replay <binary struct input_event file>`.
Pair the pen once with `bluetoothctl`; the daemon rescans `/dev/input/event*`
and picks it up whenever it reconnects.

**Validated on pmOS boot `084c7d15…` (2026-11-14):** the daemon decoded
`MSC_SCAN 0x0c0600/01/02` and the user confirmed single = play/pause,
double = next track, triple = previous track. It creates the uinput keyboard
`pmos-pen-daemon` (event7) and watches event5/event6.

---

## 5. Network & Debugging Interface

When booted into postmarketOS, the USB port dynamically initializes a multi-function gadget:
* **CDC-NCM / RNDIS Ethernet**: IP `172.16.42.1/24` (Tablet), `172.16.42.2/24` (Host PC).
* **Telnet Root Shell**: Port 23 (passwordless root access).
* **OpenSSH Daemon**: Port 22 (`root` / `user`; the password is set during install).
* **FTP File Server**: Port 21 (read/write access to rootfs).
* **CDC-ACM Serial Console**: `/dev/ttyGS0` (device) -> `/dev/ttyACM0` (host).

---

## 6. Master Build & Flash Workflow

To compile the kernel, initramfs, and flash to Slot B via ADB root:

```bash
# Execute master build script
bash build_and_flash_pmos.sh
```

### Script Execution Steps:
1. Compiles `/init` with `aarch64-linux-gnu-gcc` and packages `pmos_initramfs_194.cpio`.
2. Compiles GKI 6.12 Linux kernel `Image` using LLVM Clang toolchain.
3. Compresses `Image.lz4` and packages signed `boot_latest.img` (64 MB), `init_boot_latest.img` (8 MB), and `vbmeta_latest.img` (4 KB).
4. Flashes partitions to Slot B via `dd` (`boot_b`, `init_boot_b`, `vbmeta_b`, `vbmeta_system_b`, `vbmeta_vendor_b`) and verifies MD5 checksums.
5. For a complete fresh slot-B chain, the installer/`flash_slot_b.sh` additionally takes `vendor_boot.img` and `dtbo.img` (and optionally `lk`, `tee`, `gz`, `scp`, `sspm`, `mcupm`, `dpm`, `spmfw`, `pi_img`) from `/mnt/Nvme/slot_b_bundle` and writes only the matching `*_b` partitions.

### Slot Switching Commands:
```bash
# Switch to Slot B (postmarketOS)
adb reboot bootloader
fastboot set_active b
fastboot reboot

# Switch back to Slot A (Android)
adb reboot bootloader
fastboot set_active a
fastboot reboot
```
