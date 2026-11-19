#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Build the MT6789 audio helper modules against the prepared kernel tree.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COMMON_DIR=${COMMON_DIR:-$(CDPATH= cd -- "$HERE/../../.." && pwd)}
OUT_DIR=${OUT_DIR:-$COMMON_DIR/out}

[ -f "$OUT_DIR/include/config/kernel.release" ] || {
    echo "ERROR: kernel output tree not prepared: $OUT_DIR" >&2
    exit 1
}

make -C "$COMMON_DIR" O="$OUT_DIR" ARCH=arm64 LLVM=1 M="$HERE" modules

for module in mt6789_audio_pd.ko mt6789_i2s3_mclk.ko; do
    [ -f "$HERE/$module" ] || {
        echo "ERROR: $module was not built" >&2
        exit 1
    }
done

echo "Audio helper modules built for $(cat "$OUT_DIR/include/config/kernel.release")"
