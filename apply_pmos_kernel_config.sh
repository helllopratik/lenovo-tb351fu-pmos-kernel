#!/bin/bash
# Apply the TB351FU kernel feature fragment to out/.config and resolve
# dependencies.  Safe to run before every build; it only changes the
# options listed in arch/arm64/configs/pmos_tb351fu.fragment.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMMON_DIR="${COMMON_DIR:-$SCRIPT_DIR}"
OUT_DIR="${OUT_DIR:-$COMMON_DIR/out}"
FRAGMENT="$COMMON_DIR/arch/arm64/configs/pmos_tb351fu.fragment"

if [ ! -f "$OUT_DIR/.config" ]; then
    echo "ERROR: $OUT_DIR/.config not found" >&2
    exit 1
fi

echo "Merging $FRAGMENT into $OUT_DIR/.config"
(cd "$COMMON_DIR" && ./scripts/kconfig/merge_config.sh -m -y -O "$OUT_DIR" \
    "$OUT_DIR/.config" "$FRAGMENT")

echo "Resolving kernel config dependencies (olddefconfig)"
make -C "$COMMON_DIR" O="$OUT_DIR" ARCH=arm64 LLVM=1 olddefconfig
