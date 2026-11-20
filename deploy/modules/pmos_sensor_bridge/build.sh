#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Build the SCP sensorhub -> IIO bridge against the prepared kernel tree.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COMMON_DIR=${COMMON_DIR:-$(CDPATH= cd -- "$HERE/../../.." && pwd)}
OUT_DIR=${OUT_DIR:-$COMMON_DIR/out}

[ -f "$OUT_DIR/include/config/kernel.release" ] || {
    echo "ERROR: kernel output tree not prepared: $OUT_DIR" >&2
    exit 1
}

# hf_client_* live in the vendor hf_manager.ko, so modpost cannot resolve
# them from the kernel's Module.symvers.  They are resolved at insmod time.
make -C "$COMMON_DIR" O="$OUT_DIR" ARCH=arm64 LLVM=1 M="$HERE" \
    KBUILD_MODPOST_WARN=1 modules

[ -f "$HERE/pmos_sensor_bridge.ko" ] || {
    echo "ERROR: pmos_sensor_bridge.ko was not built" >&2
    exit 1
}

echo "Sensor bridge built for $(cat "$OUT_DIR/include/config/kernel.release")"
