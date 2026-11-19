# MT6789 audio helper modules

These two out-of-tree modules are required for the TB351FU speaker path and
must be present in `/usr/lib/modules/extra` on the installed rootfs.  They are
loaded, in this order, by `deploy/scripts/init_wifi_sensors.sh`:

1. `mt6789_audio_pd.ko` powers the MT6789 SPM audio domain (`0xB`).  The
   mainline kernel has no MT6789 SCPSYS driver, so without this module the
   whole AFE register block reads zero.
2. `mt6789_i2s3_mclk.ko` enables the MT6789 I2S3 MCLK tree.  Mainline
   `mt8186-afe-clk` wires DAI_I2S3 to the I2S4 tree, and leaves
   `aud_2_sel` on the 26 MHz parent.  The module selects APLL2
   (196.608 MHz) for `aud_2_sel`, points `top_i2s3_m_sel` at it, and
   divides by 32 to get 6.144 MHz.  Without the APLL2 selection the
   divider produces only 5.2 MHz and the AW88395 amplifiers report
   `NOCLKS`.

Both modules intentionally leave their power domain and clock tree enabled on
unload: the AFE has no genpd owner and unloading during playback would kill
audio.  Do not change the load order or power/clock sequence without hardware
testing.

## Build

Build against the same kernel output tree that produced the boot image:

```sh
PATH=/path/to/clang/bin:$PATH \
    ./deploy/modules/audio/build.sh
```

By default the script uses `common/out`; override with `OUT_DIR` and
`COMMON_DIR` when needed.  The resulting `.ko` files are written next to the
sources.

## Installer payload

`deploy/update_modules_payload.sh` rebuilds these modules and refreshes
`deploy/modules.tar.xz` without dropping any of the existing vendor modules.
`build_and_flash_pmos.sh` calls it after the kernel/module build, so the
installer payload always matches the kernel in the boot image.
