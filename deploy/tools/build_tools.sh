#!/usr/bin/env bash
# Build the freestanding aarch64 helpers used by the encrypted-userdata
# installer.  These helpers never link libc, so they run in Android's
# toybox shell and in the initramfs.
#
# Usage: build_tools.sh [OUTPUT_DIR]
#
# Set CC to override the compiler, e.g.:
#   CC="aarch64-linux-gnu-gcc" ./build_tools.sh /tmp/pmos-tools
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${1:-$SCRIPT_DIR/bin}"
mkdir -p "$OUT_DIR"

if [ -n "${CC:-}" ]; then
  read -r -a CC_CMD <<< "$CC"
elif command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
  CC_CMD=(aarch64-linux-gnu-gcc)
elif [ -x /mnt/Nvme/evolutionx/sync/prebuilts/clang/host/linux-x86/clang-r547379/bin/clang ]; then
  CC_CMD=(/mnt/Nvme/evolutionx/sync/prebuilts/clang/host/linux-x86/clang-r547379/bin/clang --target=aarch64-linux-gnu)
elif command -v clang >/dev/null 2>&1; then
  CC_CMD=(clang --target=aarch64-linux-gnu)
else
  echo "ERROR: no aarch64 cross compiler found." >&2
  echo "Install aarch64-linux-gnu-gcc, or set CC to a clang that supports --target=aarch64-linux-gnu." >&2
  exit 1
fi

FLAGS=(
  -static -nostdlib -fno-builtin -O2 -fno-stack-protector
  -fno-asynchronous-unwind-tables -fno-ident -Wl,-e,_start
)

for tool in fscrypt_helper sparse_copy sparse_pack; do
  "${CC_CMD[@]}" "${FLAGS[@]}" "$SCRIPT_DIR/$tool.c" -o "$OUT_DIR/$tool"
  chmod 755 "$OUT_DIR/$tool"
done

echo "Built aarch64 device tools in $OUT_DIR"
