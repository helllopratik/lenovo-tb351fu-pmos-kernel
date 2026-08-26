# postmarketOS Linux Kernel (GKI 6.12) for Lenovo Tab Plus (TB351FU)

This repository contains the customized Linux GKI 6.12.15 kernel source and packaging tools for booting **postmarketOS (Phosh UI)** on the **Lenovo Tab Plus (TB351FU)** with MediaTek Helio G99 (`MT6789` / `MT8781`) via Direct LK Boot.

---

## 1. Directory Structure

* **`common/`**: The complete Linux kernel source code.
  - `common/drivers/gpu/drm/tiny/simpledrm.c`: DRM simple-framebuffer driver with ARM64 linear-map fallback and Point-of-Coherency hardware cache flushing.
  - `common/drivers/mmc/host/mtk-sd.c`: MediaTek MSDC SD/MMC host controller driver with unconditional hardware initialization and dual-voltage `ocr_avail` support.
  - `common/drivers/regulator/mt6358-regulator.c`: MT6366 PMIC regulator driver with forced 3.3V power rails for the MicroSD card (`VMC`, `VMCH`).
  - `common/drivers/pinctrl/mediatek/pinctrl-paris.c`: MediaTek pinctrl driver with graceful handling of unsupported drive properties.
  - `common/drivers/pinctrl/pinmux.c`: Pinmux subsystem with dual-ownership / shared GPIO support.
  - `common/init/fbtext.c`: 1x scale VGA (`150x125`) full-screen on-screen debug console.
* **`common/out/`**: Kernel build output directory (`vmlinux`, `.config`, `arch/arm64/boot/Image`).
* **`tools/mkbootimg/`**: Android boot image creation tool (`mkbootimg.py`).
* **`external/avb/`**: Android Verified Boot signing tools (`avbtool.py`) and test RSA-4096 keys.

---

## 2. Compilation Instructions

To build the kernel Image manually using Clang/LLVM:

```bash
# Add Clang/LLVM to PATH (e.g. clang-r547379 or llvm-20)
export PATH=/path/to/clang/bin:$PATH

# Build kernel Image
make ARCH=arm64 LLVM=1 -j$(nproc) Image
```

---

## 3. Automated Build & Flash Script

Use the master automation script:

```bash
./build_and_flash_pmos.sh
```
