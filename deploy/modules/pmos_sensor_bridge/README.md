# pmos_sensor_bridge

Out-of-tree kernel module that exposes the MediaTek SCP sensorhub
(accelerometer, gyroscope, ambient light) as standard IIO devices on the
Lenovo TB351FU running postmarketOS.

## Why this exists

The vendor sensor stack (`scp.ko -> hf_manager.ko -> sensorhub.ko`) exposes
`/dev/hf_manager`, but nothing in pmOS consumes it.  This bridge creates one
`hf_client` through the exported `hf_client_*` API, enables the three sensors
and republishes the samples as cached IIO channels.  `iio-sensor-proxy` then
picks them up with its `iio-poll-accel` / `iio-poll-als` drivers, which gives
Phosh screen rotation and auto-brightness.

Channels:

| IIO device   | Channels                                  | Units / scale            |
|--------------|-------------------------------------------|--------------------------|
| `pmos-accel` | `in_accel_{x,y,z}_raw`, `in_accel_scale`   | milli-m/s², scale 0.001  |
| `pmos-gyro`  | `in_anglvel_{x,y,z}_raw`, `in_anglvel_scale` | µrad/s, scale 0.000001 |
| `pmos-als`   | `in_illuminance_raw`, `in_illuminance_scale` | lux, scale 1           |

Sensor rates: accel 50 Hz, gyro 50 Hz, ALS 5 Hz.

## Build

```sh
./deploy/modules/pmos_sensor_bridge/build.sh
```

The script uses the prepared kernel tree in `out/`.  A manual equivalent is:

```sh
export PATH="/mnt/Nvme/evolutionx/sync/prebuilts/kernel-build-tools/linux-x86/bin:/mnt/Nvme/evolutionx/sync/prebuilts/clang/host/linux-x86/clang-r547379/bin:$PATH"
make -C . O=out ARCH=arm64 LLVM=1 M="$PWD/deploy/modules/pmos_sensor_bridge" \
     KBUILD_MODPOST_WARN=1 modules
```

`KBUILD_MODPOST_WARN=1` is required because `hf_client_*` live in the vendor
`hf_manager.ko`, not in the kernel tree's `Module.symvers`.  The symbols are
resolved at `insmod` time (CONFIG_MODVERSIONS is off).

The headers in this directory are the hf_manager userspace ABI headers
(`hf_manager_cmd` = 52 bytes, `hf_manager_event` = 76 bytes) taken from the
Motorola 6.12 vendor source.  They must stay in sync with the vendor
`hf_manager.ko`/`sensorhub.ko` pair.

## Load order

`init_sensors.sh` loads:

```
mtk-mbox -> mtk_rpmsg_mbox -> mtk_tinysys_ipi -> scp -> hf_manager -> sensorhub -> pmos_sensor_bridge
```

The bridge must come after `sensorhub` (it needs `/dev/hf_manager` and the
exported `hf_client_*` symbols).  udev tags the resulting IIO devices
(`IIO_SENSOR_PROXY_TYPE=iio-poll-accel` / `iio-poll-als`) and starts
`iio-sensor-proxy.service` automatically.

## Critical dependency: patched hf_manager.ko

The stock vendor `hf_manager.ko` was built against a different `task_struct`
layout.  It reads `comm`/`group_leader`/`pid` at `0x910/0x748/0x708`, while
this kernel has them at `0x850/0x688/0x648`.  Opening `/dev/hf_manager` with
the stock module NULL-derefs in `hf_client_create` at `+0x84` (instruction
`b9470908`).  If the bridge loads automatically, that turns the boot into a
loop.  The deployed module (`/lib/modules/extra/hf_manager.ko`, sha256
`f2d8828a…beed4`) is a 6-word binary patch of the pristine vendor module
(sha256 `36cf2327…`).  See `deploy/tools/patch_hf_manager.py`;
`deploy/update_modules_payload.sh` applies and verifies it.

Do **not** replace `hf_manager.ko` with a fresh copy from a stock image
without re-applying the patch.  Also do not deploy an `hf_manager.ko` rebuilt
from the Motorola source: that source's `struct hf_device` is 8 bytes shorter
(`list` at `0x78`) than the vendor ABI (`0x80`) that `sensorhub.ko` expects.
