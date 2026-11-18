# MT6789 camera bring-up modules

Out-of-tree helpers for the Lenovo TB351FU camera path.  These are
diagnostics/bring-up modules only and are intentionally **not** loaded at
boot.  The V4L2 sensor drivers (`s5k4h7yx`, `sc820cs`), SENINF and CAMSV
drivers live in the kernel tree instead.

## Contents

| File | Purpose |
| --- | --- |
| `mt6789_cam_pd.c` | Powers the CAM/MFG power domains that mainline `mtk-scpsys` does not know about on MT6789. |
| `mt6789_cam_probe.c` | Early probe/power helper used during bring-up. |
| `mt6366_cam_ldo.c` | MT6366 camera LDO diagnostics. Defaults are deliberately conservative: `enable_vs1=0`, `enable_vrf18=0`. |
| `seninf_pd_fix.c` | Small SENINF power-domain glue. |
| `pmos_dtbo_loader.c` | Applies/removes `/lib/firmware/camera_ports.dtbo` at runtime. |
| `rt5133-regulator.c` | RT5133 regulator/GPIO driver built as an external module. |
| `mtk_reg_peek.c` | Bounded MMIO peek/poke helper. All accesses stay inside one `ioremap` window. |
| `mt6789_pstore_arm.c` | Arms pstore before a risky register experiment. |
| `camera_ports.dts` | Runtime DT overlay: CSI lanes, sensor ports and CAMSV clocks. |

## Build

Use the prepared kernel output tree (absolute `M=` is required):

```sh
PATH=/path/to/clang/bin:$PATH make -C /path/to/pmos/common \
    O=/path/to/pmos/common/out ARCH=arm64 LLVM=1 \
    M=/path/to/pmos/common/deploy/modules/camera modules

dtc -@ -I dts -O dtb camera_ports.dts -o camera_ports.dtbo
```

## Load order

1. Apply the overlay (`insmod pmos_dtbo_loader.ko`).
2. `insmod mt6789_cam_pd.ko`.
3. `insmod mtk-seninf.ko`.
4. `insmod mtk-camsv.ko`.
5. `insmod s5k4h7yx.ko` and/or `insmod sc820cs.ko`.

Unload in reverse order.  Remove `mtk-seninf` before `mtk-camsv` and the
sensors so the async references are dropped.

## Safety notes

- `mtk_reg_peek` must never use fixed offsets beyond its `ioremap` window;
  a previous unbounded read caused a level-3 translation fault and reboot.
- Do not enable the VIB rail: this tablet has no vibrator hardware and the
  camera AF supply was derived from it.
- Keep `enable_vs1=0` / `enable_vrf18=0` unless a rail test has been
  explicitly approved.
