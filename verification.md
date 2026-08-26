# TB351FU (MT8781 / Helio G99) — Image Build & Boot Verification Knowledge

This document captures everything needed to safely rebuild, sign, and flash boot images
(`boot`, `vendor_boot`, `init_boot`) for the Lenovo Tab Plus TB351FU so that the device
accepts them AND pmOS boots. It is the distilled result of extensive debugging, including
a bootloop caused by NOT re-signing after modifying a DTB.

> **TL;DR / golden rule:** Any time you change the content of an image (kernel, DTB, ramdisk,
> bootconfig), you MUST regenerate the AVB hash footer (`add_hash_footer`) over the
> **mkbootimg-sized** (content-only) image. Do NOT splice a new DTB into a full 64 MB image and
> keep the old stock footer — the bootloader verifies a SHA256 over the content and will reject it.

---

## 1. Device / Partition Background

- **SoC**: MediaTek MT8781 (a.k.a. Helio G99 / MT6789), board `t808aa`, hardware `mt8781`.
- **GPU**: Arm Mali-G57 MC2 (Valhall `arm,mali-valhall`), registers at `0x13000000`, size `0x4000`.
  MFGCFG / `g3d_config` / gpufreq block at `0x13fbf000` (`mediatek,mt6789-mfg`, `syscon`, `#clock-cells=<1>`, **phandle `0x88`**).
- **Bootloader**: LK (Little Kernel). It does NOT support modifying `vendor_boot` DTBO contents
  without breaking AVB (see below).
- **A/B slots**: `_a` (stock Android 16) and `_b` (pmOS from SD card). GPIO/slot switching is manual
  via fastboot `set_active`.
- **Root on SD**: pmOS rootfs on `/dev/mmcblk0p2` (UUID `50602725-3455-46c0-88d8-50a940396f58`).

### Partition map (from `BOOT_PROCESS.md`)
| Partition | Slot A | Slot B (pmOS) | Notes |
|-----------|--------|---------------|-------|
| `boot`     | Stock A17 boot.img | Pure GKI v4 pmOS kernel | kernel + cmdline (v4 has NO dtb section) |
| `init_boot`| Stock A17 init_boot | pmOS initramfs | 8 MB partition |
| `vendor_boot`| Stock vendor_boot | **Stock** vendor_boot (for DTBO) | supplies the DTB the kernel boots with |
| `dtbo`, `vbmeta`, `vbmeta_system`, ... | Stock | Stock | vbmeta flags=3 (verification off) |
| `misc` | cleared | cleared | |

**Critical fact:** For a GKI `boot` image built with `--header_version 4` and **no `--dtb`**, the
kernel receives its device tree (DTB) from the **`vendor_boot` DTBO table**, NOT from `boot`.
So the mali node panfrost sees at runtime comes from the vendor_boot DTBO.

---

## 2. AVB Signing Parameters (verified, authoritative)

Measured from the actual images with `avbtool info_image` and direct parsing.

### 2.1 Working `boot_b` (pmOS kernel — PROVEN to boot)
- VBMeta (`AVB0`) offset: `0xf19000`
- Authentication block: `0x240` (512) bytes
- Auxiliary block: `1344` bytes
- Flags: `0x2` (`AVB_VBMETA_IMAGE_FLAGS_HASHTREE_DISABLED`? – but the device accepts it)
- Algorithm: `SHA256_RSA4096` (test key)
- Rollback index: 0
- Hash footer (`AVBf`) at partition end: `0x3ffffc0`
- `original_image_size`: `0xf19000`

### 2.2 Stock `vendor_boot` (Lenovo factory) — device REQUIRES this boot-valid signature scheme
- VBMeta (`AVB0`) offset: `0x18a9000`
- Authentication block: `0` (Algorithm **NONE** — NO RSA signature, hash descriptor only!)
- Auxiliary block: `384` bytes
- Flags: `0x0`
- Algorithm: `NONE` (release string `avbtool 1.3.0`)
- Hash descriptor:
  - `Image Size`: `25858048` bytes = **`0x18a9000`** (this is the mkbootimg content size, NOT 64 MB)
  - Algorithm: `sha256`, Salt present, Digest present
  - `com.android.build.vendor_boot.fingerprint`: `alps/TB351FU/TB351FU:16/BP2A.250605.031.A3/ZUI_17.5.10.110_260414_ROW`

**Interpretation:** Lenovo's stock vendor_boot does NOT use an RSA signature. It uses a plain
SHA256 **hash descriptor** over the mkbootimg-sized content `[0 .. 0x18a9000]`. The bootloader computes
SHA256 over that content region and compares it to the stored digest. Changing any covered content
(changing the DTB) invalidates the digest → bootloop.

### 2.3 Why `add_hash_footer` failed on the 64 MB image
```
Image size of 67108864 exceeds maximum image size of 67039232 in order to fit in a partition size of 67108864
```
`add_hash_footer` computes `original_image_size = file byte length`. If you feed it a full
64 MB (partition-padded) file, there is no room left for the VBMeta/footer. **You must feed it the
mkbootimg-sized (content-only) image** (e.g. `25858048` bytes for stock vendor_boot). mkbootimg
(v4) outputs exactly this content size; it is NOT padded to the partition.

---

## 3. Bootloop Root Cause (the bug we fixed)

**Symptom:** Lenovo logo → orange "bootloader unlocked" → power-press → bootloop, no pmOS GUI.

**Cause (misdiagnosed initially):** We patched the mali DTB in-place inside the full 64 MB
`vendor_boot_surgical2.img`, keeping the stock AVB footer. Because the stock vendor_boot uses a
SHA256 hash descriptor over `[0 .. 0x18a9000]`, and our DTB edit changed bytes in that range, the
stored digest no longer matched. The bootloader rejected the vendor_boot → bootloop.

**Fix:** regenerate the AVB hash footer (or rebuild via mkbootimg) over the changed content so the
digest matches. The working `boot_b` uses RSA(testkey)+flags=0x2 and boots, so a re-signed
vendor_boot with the same scheme is accepted by the bootloader.

**However — the practical, proven solution for pmOS slot B is to leave `vendor_boot_b` 100% STOCK**
and make the mali fix in the **kernel** (see §7). Modifying vendor_boot's DTBO is fragile because
it is AVB-protected and MT6789's mali node is not yet mainline-shaped.

---

## 4. Verified Rebuild Recipe (`rebuild_vendor_boot.sh` + `splice_header.py`)

This is the PROVEN process that produces a boot-valid vendor_boot (used for the working recovery
image). Steps:

1. **mkbootimg** — produce content-only image (NOT padded to 64 MB):
   ```bash
   python3 tools/mkbootimg/mkbootimg.py \
     --header_version 4 --pagesize 4096 \
     --vendor_cmdline "bootopt=64S3,32N2,64N2 bootconfig" \
     --dtb <DTBO_TABLE.dtbo> \
     --vendor_bootconfig <bootconfig> \
     --ramdisk_type platform --ramdisk_name "" --vendor_ramdisk_fragment <vendor_ramdisk00_pristine> \
     --ramdisk_type recovery --ramdisk_name "recovery" --vendor_ramdisk_fragment <vendor_ramdisk01> \
     --vendor_boot vendor_boot_raw.img
   ```
   - `--dtb` expects a **DTBO table** (magic `0xd7b7ab1e`), not a raw FDT.
   - The `--vendor_ramdisk_fragment` for fragment 0 MUST be the **pristine stock** fragment
     (do NOT repack with mkbootfs). Signature: `vendor_ramdisk00` = 24,240,050 bytes, type 0x1.
2. **splice_header.py** — restore the Lenovo-validated 4096-byte header page
   (copies fields at offsets 24:28, 112:116, 124:128, 136:140 from the new header into `stock_header.bin`).
   ```bash
   python3 splice_header.py vendor_boot_raw.img
   ```
3. **add_hash_footer** — regenerate AVB hash footer (partition_size 67108864):
   ```bash
   python3 avbtool.py add_hash_footer \
     --image vendor_boot_raw.img \
     --partition_size 67108864 --partition_name vendor_boot \
     --algorithm SHA256_RSA4096 --key testkey_rsa4096.pem
   ```
   This pads the image to 64 MB and writes the footer. Because the input was the small content-only
   image, it fits and produces `original_image_size` = content size.

**Verification of the result (all must hold):**
- `header [0:0x1000]` matches the Lenovo `stock_header.bin` (except the 4 spliced mutable fields).
- `vendor_ramdisk00`, `vendor_ramdisk01`, `bootconfig` are byte-identical to pristine stock.
- AVB footer `AVBf` at `0x3ffffc0`; VBMeta `AVB0` present; `flags` = `0x2`; testkey RSA4096.
- `avbtool verify_image` reports `vbmeta: Successfully verified footer and SHA256_RSA4096`.
  (A "sha256 digest does not match digest in descriptor" message is a **red herring** — the
  known-good `boot_b` produces the same message and boots fine. It comes from comparing the hash
  over the padded partition image rather than the content region.)

---

## 5. DTBO Table Layout (vendor_boot `--dtb`)

Parsed from stock (big-endian):
- Table at file offset `0x1877000`:
  - `magic` `0xd7b7ab1e`, `total_size` `194978`, `header_size` 32, `dt_entry_size` 32,
    `dt_entry_count` 1, `dt_entries_offset` 32, `page_size` 2048, `version` 0.
  - entry: `dt_size`, `dt_offset` 64.
- FDT at `0x1877040`, `194914` bytes (stock). The **slot budget is exactly `194978`** — the DTBO
  table MUST stay exactly 194978 bytes so the bootconfig/footer don't shift.
- After the DTBO table comes `bootconfig` (size 94), then the AVB VBMeta at `0x18a9000`.

Verifier header fields (file offsets, little-endian): `header_size@0x830`, `dtb_size@0x834`,
`dtb_addr@0x838` (`0x47c80000`), `bootconfig_size@0x84c`, bootconfig starts at `0x18a69a2`.
FDT header (big-endian): bytes 8-11 `off_dt_struct`, 12-15 `off_dt_strings`, 16-19 `off_mem_rsvmap`.

---

## 6. Boot / vendor_boot Build for pmOS (`boot_b`)

From `new_pmos.md` — the pure GKI v4 boot image:
```bash
python3 tools/mkbootimg/mkbootimg.py \
  --kernel out/Image.lz4 \
  --cmdline "console=tty0,115200 androidboot.hardware=mt8781 androidboot.boot_devices=soc/11270000.ufshci androidboot.selinux=permissive ramoops.mem_address=0x48090000 ramoops.mem_size=0xe0000 ... root=/dev/mmcblk0p2 pmos_root_uuid=50602725-3455-46c0-88d8-50a940396f58 rootwait rw pmos.debug-shell pmos.force_display" \
  --header_version 4 --pagesize 4096 -o boot_unsigned.img
python3 avbtool.py add_hash_footer \
  --image boot_unsigned.img --partition_size 67108864 --partition_name boot \
  --algorithm SHA256_RSA4096 --key testkey_rsa4096.pem
```
Flash to `/dev/block/by-name/boot_b` (dd from Android root). Works with the device.

**Known-good `boot_b` md5:** `17ec4d6e34e6579996cb49687326d60a`
(MFGCFG-mali patched kernel). This image is verified to boot pmOS on slot B with stock vendor_boot.

---

## 7. Mali GPU (panfrost) — the Correct Layer

**This is the answer to "how do other devs import mali/gpu support":** other developers add the
mali node and clock wiring to the **SoC's device tree in the KERNEL** (`*.dtsi` / kernel DTB), NOT to
the bootloader's vendor_boot DTBO. Panfrost on mainline requires a **mainline-shaped** mali node.

**The mali node we have** (from the stock DTBO / `mt8781_pmos.dts`) is MediaTek/proprietary-shaped:
```
mali@13000000 {
    compatible = "mediatek,mali", "arm,mali-valhall";
    reg = <0 0x13000000 0 0x4000>;
    interrupts = <0 0x18c 4 0 ... 0x190 4 0>;
    interrupt-names = "GPU","MMU","JOB","EVENT","PWR";
    operating-points-v2 = <0x86>;
    #cooling-cells = <2>;
    ged-supply = <0x87>;          // <-- proprietary, blocks panfrost
    phandle = <0x1d>;
}
```
- `ged-supply` (phandle `0x87` = `mediatek,ged`) is a **devlink blocker**: panfrost requests the
  `mediatek,ged` supplier for a regulator that doesn't exist in mainline → `-EPROBE_DEFER` forever
  → panfrost never binds → **llvmpipe** (software) everywhere. This is the ~86% phoc CPU / slow
  rendering symptom.
- There is **no `clocks`/`clock-names`** property, so panfrost cannot gate the GPU clock.

**To enable panfrost, the mali node must be mainline-shaped** — add:
```
clocks = <&mfgcfg 0>;      /* &mfgcfg = phandle 0x88 = syscon@13fbf000 */
clock-names = "core";
```
and **remove** `ged-supply`. The kernel's MFGCFG clk driver
(`drivers/clk/mediatek/clk-mt6789-mfgcfg.c`) provides that clock (id 0 = `subsys_bg3d`).
`CONFIG_DRM_PANFROST=m` is already in defconfig.

**Because the vendor_boot DTBO is AVB-protected and MT6789 has no mainline `.dtsi`, the fix must be
applied in the KERNEL's device-tree view at boot**, not by editing the bootloader vendor_boot.
Approaches:
- Kernel-side DT fixup at boot (board/early fixup that rewrites the mali node: add `clocks`,
  remove `ged-supply`) before panfrost probes. This is the robust path.
- Rebuild `boot` with a bundled DTB (needs change to the GKI packaging) — larger change.

**Panfrost hardware notes:** Mali-G57 = Valhall arch v9 (GLES 3.1). The MT8370 patchset
("Add Mali GPU support for Mediatek MT8370", Mali-G57 MC2) is the closest upstream reference for
the identical GPU and its `mfgcfg`/SCPSYS wiring. On some MTK SoCs (e.g. MT8188) panfrost can
constantly fault even when bound, so full bring-up may also require DVFS/SCPSYS power-domain wiring.

### §7a. IMPLEMENTED: panfrost Mali-G57 bring-up (kernel-side, all in-tree)

Done for the GPU-fix boot (`623f73a9`). All changes are in the kernel boot image; `vendor_boot_b`
stays stock and the SD rootfs/initramfs are untouched.

1. **`CONFIG_DRM_PANFROST=y`** + **`CONFIG_COMMON_CLK_MT6789_MFGCFG=y`** added to the device
   `kernel.config` fragment (`twrp/sync/device/lenovo/tb351fu/kernel.config`). Panfrost is built-in
   (`=y`) so NO `.ko` needs to be pushed to the SD rootfs, and deprecates the "ged"-driven path.
2. **`drivers/gpu/drm/panfrost/panfrost_drv.c`**: added `mediatek_mt6789_mali_data`
   (`num_supplies=1` ["mali"], `num_pm_domains=1`, NULL names, `GPU_PM_CLK_DIS|GPU_PM_VREG_OFF`)
   and a `mediatek,mt6789-mali` compatible. Using `num_pm_domains=1` means panfrost skips named
   power-domain attach (avoids MT6789 SCPSYS exposing `mfg0..mfg3` instead of `core0/core1`).
3. **`drivers/soc/mediatek/mtk-mt6789-gpu-fixup.c`** (new, `core_initcall`, added to Makefile):
   rewrites the mali node (from the locked vendor_boot DTBO, which is proprietary-shaped) on the
   live unflattened tree BEFORE `of_platform_default_populate` (arch_initcall_sync) binds panfrost:
   - `compatible` → `"mediatek,mt6789-mali"` `"arm,mali-valhall-jm"`
   - `interrupt-names` → `"gpu","mmu","job","event"` (lowercase; panfrost uses get_irq_byname)
   - add `clocks = <&syscon@13fbf000 0>` + `clock-names = "core"` (mfgcfg clock, id 0 = bg3d)
   - add `mali-supply = <&buck-vgpu>` (resolves to the vgpu PMIC rail)
   - remove `ged-supply` and `operating-points-v2` (defers DVFS; clock gated + mali rail enabled only)
4. **Incremental rebuild** of the `build_61223_sdcard` tree (NOT a clean `rm -rf` run — rerun the
   config merge + olddefconfig + `make Image` against the EXISTING build dir to keep it fast).
   UTS kept `6.12.15-android16-5-gd2c91f0c3b11-4k` so vendor modules retain vermagic match.
5. **Package exactly like known-good boot_b**: `lz4 -l -9` (legacy, magic `0x184C2102`), mkbootimg
   header v4 (EMPTY cmdline, no ramdisk, no `--dtb`, os 17.0.0/2026-08), then
   `add_hash_footer --image` IN-PLACE (partition_size 67108864, testkey SHA256_RSA4096, rollback 0)
   → 64 MB flashable image. Do NOT use `--output` (that writes only a 2176-byte footer blob, not
   the padded boot image).

**Flash:** push `boot_gpufix.img` to `/data/local/tmp`, then
`adb shell su 0 dd if=/data/local/tmp/boot_gpufix.img of=/dev/block/by-name/boot_b bs=4194304 conv=fsync`,
md5-verify, reboot to slot B. Verify panfrost bind via
`dmesg | grep -i panfrost` (look for "clock rate =", absence of "failed to get pm-domain",
and `/dev/dri/renderD*` present / llvmpipe gone from phoc logs).

---

## 8. Flash & Recovery Procedure

- Verify every partition md5 right after (dd) flashing, matching the local file.
- If a bad `vendor_boot`/`boot` causes a bootloop, the device falls back / can be brought back to
  slot A (adb works in Android slot A). Restore the **stock** backup:
  - Stock `vendor_boot_b`: `/data/local/tmp/vendor_boot_b_stock_backup.img` on device
    (md5 `56f18044a0d4c806c6980846a80f1817`), also `artifacts/vendor_boot_b_stock_backup.img`.
- Stock `boot_b` / `init_boot_b` are side-by-side in `artifacts/` with a restore path.
- Slot switching: `fastboot set_active b` then `fastboot reboot` (UFS). Manual power-off/power-on
  also enters the desired slot if `misc` cleared.

---

## 9. Known-Good md5sums (slot B, pmOS bootable)

| Partition | Content | md5 |
|-----------|---------|-----|
| `boot_b` | MFGCFG-mali GKI kernel | `17ec4d6e34e6579996cb49687326d60a` |
| `vendor_boot_b` | **Stock** (must remain stock) | `56f18044a0d4c806c6980846a80f1817` |
| `init_boot_b` | pmOS initramfs | `daeb308b620603ac556e18afb29d1fa9` |
| (amended `vendor_boot_b` attempt) | surgical mali DTBO + testkey RSA re-sign | `9b5f930f8cb89ea5e8d751e8cad15e66` — REJECTED by device |
| `boot_b` (panfrost GPU fix) | GKI kernel + `CONFIG_DRM_PANFROST=y`, mfgcfg clk, DT mali fixup | `623f73a9bdeffd76a1cba310abd1e05b` — flash to boot_b, TEST (md5 of 64 MB `boot_gpufix.img`) |

> GPU-fix boot (`623f73a9`): all four changes are kernel-side, so `vendor_boot_b` stays stock and NO
> changes are needed on the SD rootfs or initramfs. Kernel UTS kept `6.12.15-android16-5-gd2c91f0c3b11-4k`
> to preserve vendor-module vermagic. See §7 note below.

> The `9b5f930f` image (mkbootimg + splice + testkey add_hash_footer, correct AVB) was still
> **rejected by the device**. Conclusion: LK rejects a modified vendor_boot DTBO regardless of a
> valid re-sign. Keep `vendor_boot_b` stock and do the mali fix in the kernel.

---

## 10. Tool Paths

- avbtool: `kernel_gki_6.12/external/avb/avbtool.py`
- test key: `kernel_gki_6.12/external/avb/test/data/testkey_rsa4096.pem`
- mkbootimg: `kernel_gki_6.12/tools/mkbootimg/mkbootimg.py`
- unpack_bootimg: `kernel_gki_6.12/tools/mkbootimg/unpack_bootimg.py`
- rebuild/splice: `device/lenovo/tb351fu/`
  (`rebuild_vendor_boot.sh`, `splice_header.py`, `stock_header.bin`, `prebuilt/dtb.dtb`)
- pmOS device tree: `mt8781_pmos.dts` (mali node at ~line 4513)
- MFGCFG clk driver: `common/drivers/clk/mediatek/clk-mt6789-mfgcfg.c`
