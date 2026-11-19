#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Rebuild the out-of-tree audio helpers and refresh deploy/modules.tar.xz.
#
# The payload keeps every existing vendor module and adds the two MT6789
# audio helpers that init_wifi_sensors.sh loads at boot.  The archive is
# written deterministically so a rebuild with an unchanged kernel/source does
# not churn the tracked payload.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COMMON_DIR=${COMMON_DIR:-$(CDPATH= cd -- "$HERE/.." && pwd)}
DEPLOY_DIR=${DEPLOY_DIR:-$COMMON_DIR/deploy}
OUT_DIR=${OUT_DIR:-$COMMON_DIR/out}
AUDIO_DIR="$DEPLOY_DIR/modules/audio"
PAYLOAD="$DEPLOY_DIR/modules.tar.xz"

[ -f "$PAYLOAD" ] || {
    echo "ERROR: missing existing module payload: $PAYLOAD" >&2
    exit 1
}

COMMON_DIR="$COMMON_DIR" OUT_DIR="$OUT_DIR" "$AUDIO_DIR/build.sh"

TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pmos-modules.XXXXXX")
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT HUP INT TERM

tar -xJf "$PAYLOAD" -C "$TMP_DIR"
for module in mt6789_audio_pd.ko mt6789_i2s3_mclk.ko; do
    [ -f "$AUDIO_DIR/$module" ] || {
        echo "ERROR: built module is missing: $AUDIO_DIR/$module" >&2
        exit 1
    }
    cp "$AUDIO_DIR/$module" "$TMP_DIR/$module"
    chmod 0644 "$TMP_DIR/$module"
done

NEW_PAYLOAD="$PAYLOAD.new"
tar --sort=name --mtime='UTC 1970-01-01' --owner=0 --group=0 \
    --numeric-owner -cJf "$NEW_PAYLOAD" -C "$TMP_DIR" .
mv -f "$NEW_PAYLOAD" "$PAYLOAD"

for module in mt6789_audio_pd.ko mt6789_i2s3_mclk.ko; do
    tar -tJf "$PAYLOAD" | grep -qx "./$module" || {
        echo "ERROR: $module is missing from $PAYLOAD" >&2
        exit 1
    }
done

echo "Updated $PAYLOAD ($(tar -tJf "$PAYLOAD" | grep -c '\.ko$') modules)"
