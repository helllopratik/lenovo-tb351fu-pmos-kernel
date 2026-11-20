#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Rebuild the out-of-tree helpers and refresh deploy/modules.tar.xz.
#
# The payload keeps every existing vendor module and adds the MT6789 audio
# helpers, the Panfrost GPU stack, the MFG power helper, the SCP sensor
# bridge, and the vendor charger stack + charge-cap/OTG helpers.  It also
# replaces the stock vendor hf_manager.ko with the patched build: the stock
# binary uses Android task_struct offsets and NULL-derefs in
# hf_client_create() on this kernel, which turns the sensor bring-up into a
# boot loop.  The archive is written deterministically so a rebuild with an
# unchanged kernel/source does not churn the tracked payload.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COMMON_DIR=${COMMON_DIR:-$(CDPATH= cd -- "$HERE/.." && pwd)}
DEPLOY_DIR=${DEPLOY_DIR:-$COMMON_DIR/deploy}
OUT_DIR=${OUT_DIR:-$COMMON_DIR/out}
AUDIO_DIR="$DEPLOY_DIR/modules/audio"
MFG_DIR="$DEPLOY_DIR/modules/mt6789_mfg_pd"
SENSOR_DIR="$DEPLOY_DIR/modules/pmos_sensor_bridge"
CHARGER_DIR="$DEPLOY_DIR/modules/charger"
OTG_DIR="$DEPLOY_DIR/modules/pmos_otg_vbus"
PAYLOAD="$DEPLOY_DIR/modules.tar.xz"
HF_PATCHER="$DEPLOY_DIR/tools/patch_hf_manager.py"

HF_PRISTINE=36cf232726cbbb05cac8a9983fc5433e406f50d176a190ca7815d6762123efe9
HF_PATCHED=f2d8828a2ddedcbeb36960a1b0846a5134ac6d4ffab9e87342e9cc5ef0fbeed4

[ -f "$PAYLOAD" ] || {
    echo "ERROR: missing existing module payload: $PAYLOAD" >&2
    exit 1
}
[ -f "$HF_PATCHER" ] || {
    echo "ERROR: missing hf_manager patcher: $HF_PATCHER" >&2
    exit 1
}

COMMON_DIR="$COMMON_DIR" OUT_DIR="$OUT_DIR" "$AUDIO_DIR/build.sh"
COMMON_DIR="$COMMON_DIR" OUT_DIR="$OUT_DIR" "$MFG_DIR/build.sh"
COMMON_DIR="$COMMON_DIR" OUT_DIR="$OUT_DIR" "$SENSOR_DIR/build.sh"
COMMON_DIR="$COMMON_DIR" OUT_DIR="$OUT_DIR" "$CHARGER_DIR/build.sh"
COMMON_DIR="$COMMON_DIR" OUT_DIR="$OUT_DIR" "$OTG_DIR/build.sh"

for module in \
    "$OUT_DIR/drivers/gpu/drm/panfrost/panfrost.ko" \
    "$OUT_DIR/drivers/gpu/drm/scheduler/gpu-sched.ko"
do
    [ -f "$module" ] || {
        echo "ERROR: GPU module missing: $module" >&2
        echo "Build the kernel first (for example NO_FLASH=1 ./build_and_flash_pmos.sh)." >&2
        exit 1
    }
done

TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pmos-modules.XXXXXX")
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT HUP INT TERM

tar -xJf "$PAYLOAD" -C "$TMP_DIR"

copy_module() {
    src="$1"
    dest="${2:-$(basename "$src")}"
    [ -f "$src" ] || {
        echo "ERROR: module missing: $src" >&2
        exit 1
    }
    mkdir -p "$TMP_DIR/$(dirname "$dest")"
    cp "$src" "$TMP_DIR/$dest"
    chmod 0644 "$TMP_DIR/$dest"
}

# Patch the vendor hf_manager.ko exactly once.  Accept an already patched
# payload so rerunning this script is idempotent, but reject anything unknown.
HF="$TMP_DIR/hf_manager.ko"
[ -f "$HF" ] || {
    echo "ERROR: hf_manager.ko missing from $PAYLOAD" >&2
    exit 1
}
HF_HASH=$(sha256sum "$HF" | awk '{print $1}')
if [ "$HF_HASH" = "$HF_PRISTINE" ]; then
    python3 "$HF_PATCHER" "$HF" "$TMP_DIR/hf_manager.ko.patched"
    mv -f "$TMP_DIR/hf_manager.ko.patched" "$HF"
    HF_HASH=$(sha256sum "$HF" | awk '{print $1}')
fi
[ "$HF_HASH" = "$HF_PATCHED" ] || {
    echo "ERROR: unexpected hf_manager.ko sha256: $HF_HASH" >&2
    exit 1
}
chmod 0644 "$HF"

for module in \
    "$AUDIO_DIR/mt6789_audio_pd.ko" \
    "$AUDIO_DIR/mt6789_i2s3_mclk.ko" \
    "$OUT_DIR/drivers/gpu/drm/panfrost/panfrost.ko" \
    "$OUT_DIR/drivers/gpu/drm/scheduler/gpu-sched.ko" \
    "$MFG_DIR/mt6789_mfg_pd.ko" \
    "$SENSOR_DIR/pmos_sensor_bridge.ko"
do
    copy_module "$module"
done

# The charger stack lives in its own directory because init_charger.sh loads
# the vendor modules in a fixed order and keeps them separate from the rest of
# /lib/modules/extra.
for module in \
    charger_class.ko \
    mtk_charger_algorithm_class.ko \
    adapter_class.ko \
    dev-info.ko \
    mt6375.ko \
    sc8989x_charger.ko \
    pmos_chg_stub.ko
do
    copy_module "$CHARGER_DIR/$module" "charger/$module"
done
copy_module "$CHARGER_DIR/pmos_charge_cap.ko" "charger/pmos_charge_cap.ko"
copy_module "$OTG_DIR/pmos_otg_vbus.ko" "charger/pmos_otg_vbus.ko"

NEW_PAYLOAD="$PAYLOAD.new"
tar --sort=name --mtime='UTC 1970-01-01' --owner=0 --group=0 \
    --numeric-owner -cJf "$NEW_PAYLOAD" -C "$TMP_DIR" .
mv -f "$NEW_PAYLOAD" "$PAYLOAD"

for module in \
    mt6789_audio_pd.ko mt6789_i2s3_mclk.ko hf_manager.ko \
    panfrost.ko gpu-sched.ko mt6789_mfg_pd.ko pmos_sensor_bridge.ko \
    charger/charger_class.ko charger/sc8989x_charger.ko \
    charger/pmos_charge_cap.ko charger/pmos_otg_vbus.ko
do
    tar -tJf "$PAYLOAD" | grep -qx "./$module" || {
        echo "ERROR: $module is missing from $PAYLOAD" >&2
        exit 1
    }
done

ARCHIVED_HF=$(tar -xOJf "$PAYLOAD" ./hf_manager.ko | sha256sum | awk '{print $1}')
[ "$ARCHIVED_HF" = "$HF_PATCHED" ] || {
    echo "ERROR: $PAYLOAD contains an unpatched hf_manager.ko" >&2
    exit 1
}

echo "Updated $PAYLOAD ($(tar -tJf "$PAYLOAD" | grep -c '\.ko$') modules)"
