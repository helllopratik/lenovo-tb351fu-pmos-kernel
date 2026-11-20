#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Build the charge-cap helper against the prepared kernel tree.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COMMON_DIR=${COMMON_DIR:-$(CDPATH= cd -- "$HERE/../../.." && pwd)}
OUT_DIR=${OUT_DIR:-$COMMON_DIR/out}

[ -f "$OUT_DIR/include/config/kernel.release" ] || {
    echo "ERROR: kernel output tree not prepared: $OUT_DIR" >&2
    exit 1
}

# charger_dev_* are exported by the vendor charger_class.ko at insmod time,
# so modpost cannot resolve them from the kernel's Module.symvers.
make -C "$COMMON_DIR" O="$OUT_DIR" ARCH=arm64 LLVM=1 M="$HERE" \
    KBUILD_MODPOST_WARN=1 modules

[ -f "$HERE/pmos_charge_cap.ko" ] || {
    echo "ERROR: pmos_charge_cap.ko was not built" >&2
    exit 1
}

echo "Charge-cap module built for $(cat "$OUT_DIR/include/config/kernel.release")"
