#!/system/bin/sh
# Device-side half of the encrypted pmOS rootfs removal.
#
# This file is a template.  The host installer substitutes the @...@
# placeholders and pushes the result to /data/local/tmp/pmos_remove.sh.
# Run it as root (su -c 'sh /data/local/tmp/pmos_remove.sh').
#
# The per-install fscrypt key must be added temporarily, otherwise Android
# cannot traverse the encrypted directory and the ciphertext cannot be
# deleted.  The key is evicted again on both success and failure.
set -e

EXPECTED_IDENT="@FSCRYPT_IDENT@"
REMOVE_LEGACY="@REMOVE_LEGACY@"

DATA=/data
DIR="@INSTALL_DIR@"
MARKER="@INSTALL_MARKER@"
LEGACY=/data/pmos_legacy/rootfs.img
KEYFILE=/data/local/tmp/pmos_fscrypt.key
HELPER=/data/local/tmp/pmos_fscrypt_helper

KEY_ADDED=0
IDENT=""

log() { echo "PMOS: $*"; }
die() { echo "PMOS_ERROR: $*" >&2; exit 1; }

cleanup() {
  rc=$?
  if [ "$KEY_ADDED" = "1" ] && [ -n "$IDENT" ]; then
    "$HELPER" delkey2 "$DATA" "$IDENT" >/dev/null 2>&1 || true
    sync
  fi
  rm -f "$KEYFILE" "$HELPER" /data/local/tmp/pmos_remove.sh 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT

[ -x "$HELPER" ] || die "fscrypt helper missing"
[ -f "$KEYFILE" ] || die "fscrypt key file missing"
[ -n "$EXPECTED_IDENT" ] || die "expected fscrypt identifier is empty"
[ "$DIR" != "/data" ] && [ "$DIR" != "/" ] || die "invalid install directory"

log "adding the fscrypt key to the userdata keyring"
OUT=$("$HELPER" addkey2f "$DATA" "$KEYFILE" || true)
echo "$OUT"
IDENT=$(printf '%s\n' "$OUT" | sed -n 's/^identifier=\([0-9a-f][0-9a-f]*\)$/\1/p' | tail -n 1)
[ -n "$IDENT" ] || IDENT="$EXPECTED_IDENT"
[ "$IDENT" = "$EXPECTED_IDENT" ] || \
  die "fscrypt identifier mismatch (expected $EXPECTED_IDENT, got $IDENT)"
KEY_ADDED=1

if [ -d "$DIR" ]; then
  log "checking the encryption policy on $DIR"
  POLICY=$("$HELPER" getpolicy2ident "$DIR" 2>/dev/null || true)
  CURRENT=$(printf '%s\n' "$POLICY" | sed -n 's/^identifier=//p' | tail -n 1)
  if [ -n "$CURRENT" ] && [ "$CURRENT" != "$IDENT" ]; then
    die "$DIR is encrypted with a different key; cannot remove it safely"
  fi
  case "$POLICY" in
    *unsupported*) die "$DIR has a non-v2 policy; refusing to remove it" ;;
  esac

  log "removing $DIR"
  rm -rf "$DIR" || die "could not remove $DIR"
else
  log "no installation directory at $DIR"
fi

if [ -f "$MARKER" ]; then
  log "removing installation marker"
  rm -f "$MARKER"
fi

if [ "$REMOVE_LEGACY" = "1" ]; then
  if [ -f "$LEGACY" ]; then
    log "removing the legacy Android-readable image"
    rm -f "$LEGACY"
    rmdir /data/pmos_legacy 2>/dev/null || true
  fi
fi
sync

log "evicting the fscrypt key from Android"
i=0
while [ "$i" -lt 4 ]; do
  "$HELPER" delkey2 "$DATA" "$IDENT" >/dev/null 2>&1 || true
  sync
  echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
  if ! grep -q "$IDENT" /proc/keys 2>/dev/null; then
    KEY_ADDED=0
    break
  fi
  i=$((i + 1))
  sleep 1
done
if [ "$KEY_ADDED" = "1" ]; then
  die "fscrypt key could not be evicted; reboot before treating Android as clean"
fi

log "removal complete"
