#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Build the OTG VBUS helper against the prepared kernel tree.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COMMON_DIR=${COMMON_DIR:-$(CDPATH= cd -- "$HERE/../../.." && pwd)}
OUT_DIR=${OUT_DIR:-$COMMON_DIR/out}

[ -f "$OUT_DIR/include/config/kernel.release" ] || {
    echo "ERROR: kernel output tree not prepared: $OUT_DIR" >&2
    exit 1
}

# get_charger_by_name()/charger_dev_enable_otg() come from charger_class.ko.
make -C "$COMMON_DIR" O="$OUT_DIR" ARCH=arm64 LLVM=1 M="$HERE" \
    KBUILD_MODPOST_WARN=1 modules

[ -f "$HERE/pmos_otg_vbus.ko" ] || {
    echo "ERROR: pmos_otg_vbus.ko was not built" >&2
    exit 1
}

echo "OTG VBUS module built for $(cat "$OUT_DIR/include/config/kernel.release")"
