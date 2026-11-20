#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Build the MT6789 MFG power-domain helper for Panfrost.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COMMON_DIR=${COMMON_DIR:-$(CDPATH= cd -- "$HERE/../../.." && pwd)}
OUT_DIR=${OUT_DIR:-$COMMON_DIR/out}

[ -f "$OUT_DIR/include/config/kernel.release" ] || {
    echo "ERROR: kernel output tree not prepared: $OUT_DIR" >&2
    exit 1
}

make -C "$COMMON_DIR" O="$OUT_DIR" ARCH=arm64 LLVM=1 M="$HERE" modules

[ -f "$HERE/mt6789_mfg_pd.ko" ] || {
    echo "ERROR: mt6789_mfg_pd.ko was not built" >&2
    exit 1
}

echo "MFG power module built for $(cat "$OUT_DIR/include/config/kernel.release")"
