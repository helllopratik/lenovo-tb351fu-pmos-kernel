#!/usr/bin/env bash
# ==============================================================================
# Master postmarketOS RootFS Installer & Provisioner for Lenovo Tab Plus (TB351FU)
# Author: helllopratik <pratikgondane07@gmail.com>
#
# Provisions a complete, clean 64GB postmarketOS (Phosh) rootfs on UFS userdata
# (/data/pmos/rootfs.img) for dual-booting alongside Android.
#
# Preserves merged /usr symlinks, deploys out-of-tree hardware modules, installs
# Wi-Fi / Bluetooth bring-up services, installs the 60/90/adaptive display mode
# panel, and verifies systemd executable integrity inside chroot before completion.
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPLOY_DIR="$SCRIPT_DIR/deploy"
TMP_DIR="${TMPDIR:-/var/tmp}/pmos_install_tmp"
DEFAULT_IMAGE_URL_FILE="$DEPLOY_DIR/configs/pmos_image_url"

DEFAULT_IMAGE_URL=""
if [ -f "$DEFAULT_IMAGE_URL_FILE" ]; then
  DEFAULT_IMAGE_URL=$(sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' \
    "$DEFAULT_IMAGE_URL_FILE" | grep -v '^$' | head -n 1 || true)
fi

cleanup_host_tmp() {
  rm -rf "$TMP_DIR" 2>/dev/null || true
  # The device-side script removes these on both success and failure.  This
  # covers a host failure between pushing them and starting that script.
  if command -v adb >/dev/null 2>&1; then
    if command -v timeout >/dev/null 2>&1; then
      timeout 10 adb shell "su -c 'rm -f /data/local/tmp/pmos_fscrypt.key /data/local/tmp/pmos_account.env'" \
        >/dev/null 2>&1 || true
    else
      adb shell "su -c 'rm -f /data/local/tmp/pmos_fscrypt.key /data/local/tmp/pmos_account.env'" \
        >/dev/null 2>&1 || true
    fi
  fi
}

usage() {
  cat <<EOF
Usage: $0 [OPTIONS] [IMAGE]

Install a postmarketOS rootfs into /data/pmos/rootfs.img on the
Lenovo Tab Plus (TB351FU) while Android is running.

Image source:
  -i, --image PATH      Use a local rootfs image (.img, .img.zst, .img.xz, .img.gz).
      --source MODE     auto (download from server) or manual (local image).
                        If omitted, the installer asks.
  -u, --url URL         Server image URL for --source auto.
                        Default: deploy/configs/pmos_image_url
      --sha256 HEX      Optional expected SHA-256 of the downloaded file.

Installation:
  -s, --size SIZE       Loopback image size (default: 64G).  K/M/G/T suffixes.
                        In whole-device mode the default is the real userdata size.
  -m, --mode MODE       dual (default) or whole.
                        "dual" keeps Android and installs into /data/pmos.
                        "whole" prepares an offline package in a temporary
                        staging directory; it never erases or reformats userdata
                        while Android is running.
      --whole-output DIR
                        Output directory for the whole-device package.
      --keep-old        Keep the previous rootfs.img as rootfs.img.old.
      --replace-existing
                        Replace an existing /data/pmos installation.
      --remove-existing
                        Remove an existing /data/pmos installation and exit.
      --remove-legacy   With --remove-existing, also remove the old
                        Android-readable /data/pmos_legacy/rootfs.img.
      --yes             Non-interactive; never prompts.  Supply --source or
                        --image, --mode if needed, and --username plus
                        --password-file (or --no-account).

Account:
  -n, --username NAME   pmOS login name (default: prompt; "user" defaults).
      --password-file F Read the password from F instead of prompting.
      --password-hash-file F
                        Read a precomputed SHA-512 crypt hash from F.
                        Use this instead of --password-file; only the hash
                        is sent to the tablet.
      --no-account      Do not create/change a pmOS login account.

Other:
  -h, --help            Show this help.

Environment:
  PMOS_IMG, PMOS_IMAGE_URL, PMOS_IMAGE_SHA256, PMOS_SOURCE, PMOS_MODE
  PMOS_USERNAME, PMOS_PASSWORD_FILE, PMOS_PASSWORD_HASH_FILE,
  PMOS_PASSWORD (discouraged), PMOS_NO_ACCOUNT
  ROOTFS_SIZE, ANDROID_SERIAL, PMOS_STORAGE_KEYS, PMOS_INSTALL_DIR
  PMOS_KEEP_OLD, PMOS_REPLACE_EXISTING, PMOS_REMOVE_EXISTING, PMOS_REMOVE_LEGACY
  PMOS_WHOLE_OUTPUT   Output directory for --mode whole (prep only).

The dual-boot rootfs is stored in an fscrypt v2-encrypted /data/pmos.  The key
is removed from Android's keyring after installation, so Android sees ENOKEY
inside that directory while the pmOS initramfs can unlock it at boot.
EOF
}

step() { printf '%%%%PMOS_STEP:%s:%s\n' "$1" "$2"; }
progress() { printf '%%%%PMOS_PROGRESS:%s:%s\n' "$1" "$2"; }
trap 'rc=$?; echo "ERROR: rootfs installer failed at line $LINENO (exit $rc)" >&2; printf "%%%%PMOS_ERROR:%s\n" "$rc"' ERR

# Machine-readable protocol check used by the installer self-test.
if [ "${PMOS_MARKER_TEST:-}" = "1" ]; then
  step selftest ok
  progress 50 selftest
  printf '%%%%PMOS_DONE\n'
  exit 0
elif [ "${PMOS_MARKER_TEST:-}" = "error" ]; then
  false
fi

trap cleanup_host_tmp EXIT

ROOTFS_SIZE_EXPLICIT=0
if [ -n "${ROOTFS_SIZE:-}" ]; then
  ROOTFS_SIZE_EXPLICIT=1
else
  ROOTFS_SIZE="64G"
fi
PMOS_IMG="${PMOS_IMG:-}"
KEEP_OLD="${PMOS_KEEP_OLD:-0}"
INSTALL_DIR="${PMOS_INSTALL_DIR:-/data/pmos}"
INSTALL_DIR="${INSTALL_DIR%/}"
INSTALL_MARKER="${INSTALL_DIR}_install.env"
WHOLE_WORK_DIR="${INSTALL_DIR}_wholeprep"
WHOLE_PACK_DEVICE=/data/local/tmp/pmos_whole.sparsepack
STORAGE_KEYS_FILE="${PMOS_STORAGE_KEYS:-$HOME/.config/pmos/storage-keys.env}"
PMOS_MODE="${PMOS_MODE:-}"
PMOS_SOURCE="${PMOS_SOURCE:-}"
PMOS_IMAGE_URL="${PMOS_IMAGE_URL:-$DEFAULT_IMAGE_URL}"
PMOS_IMAGE_SHA256="${PMOS_IMAGE_SHA256:-}"
PMOS_USERNAME="${PMOS_USERNAME:-}"
PMOS_PASSWORD_FILE="${PMOS_PASSWORD_FILE:-}"
PMOS_PASSWORD_HASH_FILE="${PMOS_PASSWORD_HASH_FILE:-}"
PMOS_PASSWORD="${PMOS_PASSWORD:-}"
PMOS_NO_ACCOUNT="${PMOS_NO_ACCOUNT:-0}"
ASSUME_YES=0
REPLACE_EXISTING="${PMOS_REPLACE_EXISTING:-0}"
REMOVE_EXISTING="${PMOS_REMOVE_EXISTING:-0}"
REMOVE_LEGACY="${PMOS_REMOVE_LEGACY:-0}"
WHOLE_OUTPUT="${PMOS_WHOLE_OUTPUT:-$SCRIPT_DIR/../artifacts/whole_device_prep}"
EXISTING_DIR=0
EXISTING_LEGACY=0
EXISTING_MARKER=0
EXISTING_ACTION=""
ACCOUNT_ENABLED=0
PMOS_PASSWORD_HASH=""
ACCOUNT_ENV_HOST=""
USERDATA_BYTES=""
WHOLE_PACKAGE_DIR=""

die() {
  echo "ERROR: $*" >&2
  exit 2
}

is_tty() {
  [ -t 0 ] && [ -t 1 ]
}

ask_line() {
  # ask_line "prompt" "default"
  local prompt="$1" def="${2:-}" ans=""
  if ! is_tty; then
    printf '%s' "$def"
    return 0
  fi
  IFS= read -r -p "$prompt" ans || ans=""
  [ -n "$ans" ] || ans="$def"
  printf '%s' "$ans"
}

ask_password() {
  local p1 p2
  if ! is_tty; then
    die "password prompt requires a terminal; use --password-file or PMOS_PASSWORD_FILE"
  fi
  IFS= read -r -s -p "Password for ${PMOS_USERNAME}: " p1 || true
  echo
  IFS= read -r -s -p "Confirm password: " p2 || true
  echo
  [ "$p1" = "$p2" ] || die "passwords do not match"
  [ -n "$p1" ] || die "password must not be empty"
  printf '%s' "$p1"
}

validate_username() {
  local name="$1"
  [[ "$name" =~ ^[a-z_][a-z0-9_-]{0,31}$ ]] || \
    die "invalid username '$name' (lowercase letters, digits, _ and -; must start with a letter or _)"
  case "$name" in
    root|daemon|bin|sys|sync|games|man|lp|mail|news|uucp|proxy|www-data|backup|list|irc|gnats|nobody|systemd-*)
      die "username '$name' is reserved" ;;
  esac
}

hash_password() {
  local pw="$1" salt setting hash=""
  command -v openssl >/dev/null 2>&1 || \
    die "openssl is required to hash the account password"
  salt=$(python3 -c '
import secrets, string
alphabet = string.ascii_letters + string.digits + "./"
print("".join(secrets.choice(alphabet) for _ in range(16)))
')
  setting="\$6\$rounds=120000\$$salt\$"
  # GNU libcrypt through perl supports a higher round count than
  # OpenSSL's fixed 5000-round SHA-512 crypt.  Fall back to openssl.
  if command -v perl >/dev/null 2>&1; then
    hash=$(printf '%s\n' "$pw" | \
      perl -e 'my $p=<STDIN>; chomp $p; print crypt($p, $ARGV[0]);' \
      "$setting" 2>/dev/null || true)
  fi
  case "$hash" in
    '$6$'*) ;;
    *) hash=$(printf '%s' "$pw" | openssl passwd -6 -stdin 2>/dev/null || true) ;;
  esac
  case "$hash" in
    '$6$'*) printf '%s' "$hash" ;;
    *) die "could not hash the password" ;;
  esac
}

require_hex() {
  local name="$1" value="$2" min="$3" max="$4"
  if [ -n "$value" ]; then
    case "$value" in
      *[!0-9a-fA-F]*)
        echo "ERROR: $name must contain only hexadecimal characters." >&2
        exit 1
        ;;
    esac
    if [ "${#value}" -lt "$min" ] || [ "${#value}" -gt "$max" ] || \
       [ $(( ${#value} % 2 )) -ne 0 ]; then
      echo "ERROR: $name has an unexpected length." >&2
      exit 1
    fi
  fi
}

expand_path() {
  case "$1" in
    "~") printf '%s' "$HOME" ;;
    "~/"*) printf '%s/%s' "$HOME" "${1#\~/}" ;;
    *) printf '%s' "$1" ;;
  esac
}

download_image() {
  local url="$1" path name actual expected

  case "$url" in
    http://*|https://*) ;;
    *) die "server image URL must start with http:// or https://" ;;
  esac

  mkdir -p "$TMP_DIR"
  path="${url%%\?*}"
  name="${path##*/}"
  [ -n "$name" ] || name="rootfs.img"
  case "$name" in
    *.img|*.img.zst|*.img.xz|*.img.gz|*.zst|*.xz|*.gz) ;;
    *) name="$name.img" ;;
  esac
  DOWNLOAD_PATH="$TMP_DIR/download-$name"

  echo "Downloading postmarketOS image:"
  echo "  $url"
  if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 --progress-bar -o "$DOWNLOAD_PATH" "$url" || \
      die "image download failed"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "$DOWNLOAD_PATH" "$url" || die "image download failed"
  else
    die "curl or wget is required for --source auto"
  fi
  [ -s "$DOWNLOAD_PATH" ] || die "downloaded image is empty"

  if [ -n "$PMOS_IMAGE_SHA256" ]; then
    case "$PMOS_IMAGE_SHA256" in
      *[!0-9a-fA-F]*) die "--sha256 must be hexadecimal" ;;
    esac
    [ "${#PMOS_IMAGE_SHA256}" -eq 64 ] || die "--sha256 must be 64 hex characters"
    expected=$(printf '%s' "$PMOS_IMAGE_SHA256" | tr 'A-F' 'a-f')
    actual=$(sha256sum "$DOWNLOAD_PATH" | awk '{print $1}')
    [ "$actual" = "$expected" ] || die "downloaded image checksum mismatch"
    echo "Download checksum verified: $actual"
  else
    echo "WARNING: no --sha256 supplied for the downloaded image." >&2
  fi

  PMOS_IMG="$DOWNLOAD_PATH"
  echo "Downloaded image: $(ls -lh "$PMOS_IMG" | awk '{print $5}')"
}

select_image_source() {
  mkdir -p "$TMP_DIR"

  if [ -n "$PMOS_IMG" ]; then
    PMOS_SOURCE="manual"
  fi

  if [ -z "$PMOS_SOURCE" ]; then
    if [ "$ASSUME_YES" = "1" ] || ! is_tty; then
      if [ -n "$PMOS_IMAGE_URL" ]; then
        PMOS_SOURCE="auto"
      else
        die "non-interactive install needs --source auto --url URL, or --source manual --image PATH"
      fi
    else
      echo "postmarketOS image source:"
      echo "  1) Download from the server (default)"
      [ -n "$PMOS_IMAGE_URL" ] && echo "     $PMOS_IMAGE_URL"
      echo "  2) Use a local image file"
      SOURCE_CHOICE=$(ask_line "Select [1-2] (default 1): " "1")
      case "$SOURCE_CHOICE" in
        1|auto|a|download|d) PMOS_SOURCE="auto" ;;
        2|manual|m|local|l) PMOS_SOURCE="manual" ;;
        *) die "invalid source selection '$SOURCE_CHOICE'" ;;
      esac
    fi
  fi

  case "$PMOS_SOURCE" in
    auto)
      if [ -z "$PMOS_IMAGE_URL" ]; then
        is_tty || die "no server image URL configured"
        PMOS_IMAGE_URL=$(ask_line "Server image URL: " "")
        [ -n "$PMOS_IMAGE_URL" ] || die "server image URL must not be empty"
      fi
      download_image "$PMOS_IMAGE_URL"
      ;;
    manual)
      if [ -z "$PMOS_IMG" ]; then
        is_tty || die "no local image supplied"
        PMOS_IMG=$(ask_line "Local rootfs image path: " "")
      fi
      PMOS_IMG=$(expand_path "$PMOS_IMG")
      [ -f "$PMOS_IMG" ] || die "local rootfs image not found: $PMOS_IMG"
      echo "Local image: $PMOS_IMG ($(ls -lh "$PMOS_IMG" | awk '{print $5}'))"
      ;;
    *)
      die "unknown image source '$PMOS_SOURCE'"
      ;;
  esac
}

prepare_account() {
  local pw=""

  if [ "$PMOS_NO_ACCOUNT" = "1" ]; then
    ACCOUNT_ENABLED=0
    echo "Account setup: disabled (--no-account); the image default account is kept."
    return 0
  fi

  if [ -z "$PMOS_USERNAME" ]; then
    if [ "$ASSUME_YES" = "1" ] || ! is_tty; then
      die "non-interactive install needs --username, or --no-account"
    fi
    PMOS_USERNAME=$(ask_line "pmOS username [user]: " "user")
  fi
  validate_username "$PMOS_USERNAME"

  if [ -n "$PMOS_PASSWORD_HASH_FILE" ] && \
     { [ -n "$PMOS_PASSWORD_FILE" ] || [ -n "$PMOS_PASSWORD" ]; }; then
    die "use only one of --password-file, --password-hash-file and PMOS_PASSWORD"
  fi

  if [ -n "$PMOS_PASSWORD_HASH_FILE" ]; then
    PMOS_PASSWORD_HASH_FILE=$(expand_path "$PMOS_PASSWORD_HASH_FILE")
    [ -f "$PMOS_PASSWORD_HASH_FILE" ] || \
      die "password hash file not found: $PMOS_PASSWORD_HASH_FILE"
    IFS= read -r PMOS_PASSWORD_HASH < "$PMOS_PASSWORD_HASH_FILE" || true
    PMOS_PASSWORD_HASH=${PMOS_PASSWORD_HASH%$'\r'}
    case "$PMOS_PASSWORD_HASH" in
      '$6$'*) ;;
      *) die "password hash file does not contain a SHA-512 crypt hash" ;;
    esac
    [ "${#PMOS_PASSWORD_HASH}" -le 256 ] || \
      die "password hash is too long"
    case "$PMOS_PASSWORD_HASH" in
      *[!A-Za-z0-9./=$]*) die "password hash contains invalid characters" ;;
    esac
  else
    if [ -n "$PMOS_PASSWORD_FILE" ]; then
      PMOS_PASSWORD_FILE=$(expand_path "$PMOS_PASSWORD_FILE")
      [ -f "$PMOS_PASSWORD_FILE" ] || \
        die "password file not found: $PMOS_PASSWORD_FILE"
      IFS= read -r pw < "$PMOS_PASSWORD_FILE" || true
      pw=${pw%$'\r'}
    elif [ -n "$PMOS_PASSWORD" ]; then
      pw="$PMOS_PASSWORD"
      echo "WARNING: PMOS_PASSWORD is visible in the environment; prefer --password-file." >&2
    else
      pw=$(ask_password)
    fi
    [ -n "$pw" ] || die "password must not be empty"

    PMOS_PASSWORD_HASH=$(hash_password "$pw")
    unset pw
  fi

  ACCOUNT_ENABLED=1
  ACCOUNT_ENV_HOST="$TMP_DIR/pmos_account.env"
  (
    umask 077
    {
      printf "PMOS_USERNAME='%s'\n" "$PMOS_USERNAME"
      printf "PMOS_PASSWORD_HASH='%s'\n" "$PMOS_PASSWORD_HASH"
    } > "$ACCOUNT_ENV_HOST"
  )
  chmod 600 "$ACCOUNT_ENV_HOST"
  echo "Account: $PMOS_USERNAME (only a SHA-512 crypt hash leaves the host)"
}

remove_device_dir() {
  local dir="$1" marker="$2" remove_legacy="$3"
  local ident key_tmp device_script out rc

  [ -f "$STORAGE_KEYS_FILE" ] || \
    die "cannot remove $dir: storage key file not found at $STORAGE_KEYS_FILE"
  set -a
  # shellcheck disable=SC1090
  . "$STORAGE_KEYS_FILE"
  set +a
  [ -n "${PMOS_FSCRYPT_MASTER_KEY:-}" ] || \
    die "cannot remove $dir: PMOS_FSCRYPT_MASTER_KEY is empty"
  require_hex PMOS_FSCRYPT_MASTER_KEY "$PMOS_FSCRYPT_MASTER_KEY" 32 128

  mkdir -p "$TMP_DIR"
  key_tmp="$TMP_DIR/remove_fscrypt.key"
  ( umask 077; printf '%s\n' "$PMOS_FSCRYPT_MASTER_KEY" > "$key_tmp" )
  ident=$(python3 "$SCRIPT_DIR/deploy/tools/fscrypt_identifier.py" "$key_tmp")
  if [ -n "${PMOS_FSCRYPT_IDENTIFIER:-}" ] && \
     [ "$PMOS_FSCRYPT_IDENTIFIER" != "$ident" ]; then
    rm -f "$key_tmp"
    die "stored fscrypt identifier does not match the stored key"
  fi

  echo "Building the device helpers..."
  bash "$SCRIPT_DIR/deploy/tools/build_tools.sh" "$TMP_DIR/tools" >/dev/null
  device_script="$TMP_DIR/device_remove.sh"
  sed -e "s|@INSTALL_DIR@|$dir|g" \
      -e "s|@INSTALL_MARKER@|$marker|g" \
      -e "s|@FSCRYPT_IDENT@|$ident|g" \
      -e "s|@REMOVE_LEGACY@|$remove_legacy|g" \
      "$SCRIPT_DIR/deploy/installer/device_remove.sh" > "$device_script"

  adb push "$TMP_DIR/tools/fscrypt_helper" /data/local/tmp/pmos_fscrypt_helper >/dev/null
  adb push "$device_script" /data/local/tmp/pmos_remove.sh >/dev/null
  # Push the key last, after the helper and script, so a transfer failure
  # cannot leave a key without a cleanup trap.
  adb push "$key_tmp" /data/local/tmp/pmos_fscrypt.key >/dev/null
  rm -f "$key_tmp"
  adb shell "su -c 'chmod 755 /data/local/tmp/pmos_fscrypt_helper; chmod 600 /data/local/tmp/pmos_fscrypt.key'" >/dev/null

  set +e
  out=$(adb shell "su -c 'sh /data/local/tmp/pmos_remove.sh'" 2>&1)
  rc=$?
  set -e
  printf '%s\n' "$out"
  return "$rc"
}

write_install_marker() {
  local marker_tmp="$TMP_DIR/pmos_install.env"
  (
    umask 077
    {
      echo "# postmarketOS installation marker"
      echo "PMOS_INSTALL_FORMAT=1"
      echo "PMOS_INSTALL_MODE=$PMOS_MODE"
      echo "PMOS_INSTALL_DIR=$INSTALL_DIR"
      echo "PMOS_INSTALL_SIZE_BYTES=$ROOTFS_SIZE_BYTES"
      echo "PMOS_INSTALL_FSCRYPT_IDENTIFIER=$PMOS_FSCRYPT_IDENTIFIER"
      if [ "$ACCOUNT_ENABLED" = "1" ]; then
        echo "PMOS_INSTALL_USER=$PMOS_USERNAME"
      else
        echo "PMOS_INSTALL_USER=none"
      fi
      echo "PMOS_INSTALL_SOURCE=${PMOS_SOURCE:-unknown}"
      echo "PMOS_INSTALL_DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$marker_tmp"
  )
  if adb push "$marker_tmp" /data/local/tmp/pmos_install.env >/dev/null 2>&1 && \
     adb shell "su -c 'mv /data/local/tmp/pmos_install.env $INSTALL_MARKER; chmod 644 $INSTALL_MARKER'" \
       >/dev/null 2>&1; then
    echo "Installation marker written: $INSTALL_MARKER"
  else
    echo "WARNING: could not write the installation marker at $INSTALL_MARKER." >&2
  fi
}

build_whole_package() {
  local pack="$1"
  local out="$WHOLE_OUTPUT"
  local pack_sha pack_size account_line account_note

  [ -s "$pack" ] || die "whole-device pack is empty"
  if [ -e "$out" ] && [ ! -d "$out" ]; then
    die "whole-device output path is not a directory: $out"
  fi
  if [ -d "$out" ] && [ -n "$(ls -A "$out" 2>/dev/null)" ]; then
    die "whole-device output directory is not empty: $out (move it away or set PMOS_WHOLE_OUTPUT)"
  fi

  pack_sha=$(sha256sum "$pack" | awk '{print $1}')
  pack_size=$(stat -c %s "$pack")
  if [ "$ACCOUNT_ENABLED" = "1" ]; then
    account_line="Account:  $PMOS_USERNAME"
    account_note="the requested account (password stored only as a SHA-512 crypt hash)"
  else
    account_line="Account:  image default (account setup disabled)"
    account_note="the image's default account (account setup was disabled)"
  fi

  echo "Building the offline whole-device package in $out"
  mkdir -p "$out"
  chmod 700 "$out"
  cp "$pack" "$out/rootfs.sparsepack"
  cp "$TMP_DIR/tools/sparse_pack" "$out/sparse_pack"
  chmod 600 "$out/rootfs.sparsepack"
  chmod 700 "$out/sparse_pack"

  sed -e "s|@ROOTFS_SIZE_BYTES@|$ROOTFS_SIZE_BYTES|g" \
      -e "s|@ROOTFS_PACK_SHA256@|$pack_sha|g" \
      "$SCRIPT_DIR/deploy/installer/whole_device_install.sh" \
      > "$out/install_whole_device.sh"
  chmod 700 "$out/install_whole_device.sh"

  cat > "$out/README.txt" <<EOF
postmarketOS offline whole-device package
=========================================

Prepared: $(date -u +%Y-%m-%dT%H:%M:%SZ)
$account_line
Source:   ${PMOS_SOURCE:-unknown}
Image:    $ROOTFS_SIZE_BYTES bytes logical, $pack_size bytes packed
Checksum: $pack_sha

WARNING
-------
Applying this package ERASES the Android userdata partition.  It is intended
to be run from a recovery or initramfs shell with /data unmounted, never from
running Android.  Back up anything you need first.  It is not a secure erase:
old blocks outside the new filesystem may remain recoverable.

The package contains:
  rootfs.sparsepack          sparse ext4 rootfs (allocated extents only)
  sparse_pack                aarch64 pack apply/verify tool
  install_whole_device.sh    recovery-side installer
  SHA256SUMS                 checksums for these files

Verify the package before copying it to the device:

  sha256sum -c SHA256SUMS

Usage
-----
Copy this directory to the device/recovery, then run as root:

  PMOS_WHOLE_CONFIRM=ERASE-ANDROID sh install_whole_device.sh

The default destination is /dev/block/by-name/userdata.  A different block
device can be passed as the first argument.

The rootfs was prepared in a temporary encrypted staging install on the
tablet, so it includes the hardware modules, display/audio/camera services,
and $account_note.  It also has a
/pmos_whole_device marker at the filesystem root.

This package does NOT include a boot image.  The existing encrypted-dual-boot
initramfs expects /data/pmos/rootfs.img and will not boot this layout.  A boot
image that mounts userdata directly is required before restarting.

Android was not modified while this package was prepared.  Temporary staging
space under $WHOLE_WORK_DIR is removed afterwards.
EOF
  chmod 600 "$out/README.txt"
  (
    cd "$out"
    sha256sum rootfs.sparsepack sparse_pack install_whole_device.sh README.txt \
      > SHA256SUMS
    chmod 600 SHA256SUMS
  )
  WHOLE_PACKAGE_DIR="$out"
}
case "$INSTALL_DIR" in
  /data/?*) ;;
  *)
    echo "ERROR: PMOS_INSTALL_DIR must be a directory under /data." >&2
    exit 2
    ;;
esac
case "$INSTALL_DIR" in
  *[!A-Za-z0-9_/.+-]*)
    echo "ERROR: PMOS_INSTALL_DIR contains unsupported characters." >&2
    exit 2
    ;;
esac

while [ "$#" -gt 0 ]; do
  case "$1" in
    -i|--image)
      [ "$#" -ge 2 ] || { echo "ERROR: --image needs a path" >&2; exit 2; }
      PMOS_IMG="$2"; PMOS_SOURCE="manual"; shift 2 ;;
    -u|--url)
      [ "$#" -ge 2 ] || { echo "ERROR: --url needs a value" >&2; exit 2; }
      PMOS_IMAGE_URL="$2"; shift 2 ;;
    --source)
      [ "$#" -ge 2 ] || { echo "ERROR: --source needs auto or manual" >&2; exit 2; }
      PMOS_SOURCE="$2"; shift 2 ;;
    --sha256)
      [ "$#" -ge 2 ] || { echo "ERROR: --sha256 needs a value" >&2; exit 2; }
      PMOS_IMAGE_SHA256="$2"; shift 2 ;;
    -s|--size)
      [ "$#" -ge 2 ] || { echo "ERROR: --size needs a value" >&2; exit 2; }
      ROOTFS_SIZE="$2"; ROOTFS_SIZE_EXPLICIT=1; shift 2 ;;
    -m|--mode)
      [ "$#" -ge 2 ] || { echo "ERROR: --mode needs dual or whole" >&2; exit 2; }
      PMOS_MODE="$2"; shift 2 ;;
    --whole-output)
      [ "$#" -ge 2 ] || { echo "ERROR: --whole-output needs a path" >&2; exit 2; }
      WHOLE_OUTPUT="$2"; shift 2 ;;
    -n|--username)
      [ "$#" -ge 2 ] || { echo "ERROR: --username needs a value" >&2; exit 2; }
      PMOS_USERNAME="$2"; shift 2 ;;
    --password-file)
      [ "$#" -ge 2 ] || { echo "ERROR: --password-file needs a path" >&2; exit 2; }
      PMOS_PASSWORD_FILE="$2"; shift 2 ;;
    --password-hash-file)
      [ "$#" -ge 2 ] || { echo "ERROR: --password-hash-file needs a path" >&2; exit 2; }
      PMOS_PASSWORD_HASH_FILE="$2"; shift 2 ;;
    --no-account) PMOS_NO_ACCOUNT=1; shift ;;
    --keep-old) KEEP_OLD=1; shift ;;
    --replace-existing) REPLACE_EXISTING=1; shift ;;
    --remove-existing) REMOVE_EXISTING=1; shift ;;
    --remove-legacy) REMOVE_LEGACY=1; shift ;;
    --yes) ASSUME_YES=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --) shift; break ;;
    -*) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
    *) PMOS_IMG="$1"; PMOS_SOURCE="manual"; shift ;;
  esac
done

case "$PMOS_MODE" in
  ""|dual|whole) ;;
  *) die "--mode must be 'dual' or 'whole'" ;;
esac
case "$PMOS_SOURCE" in
  ""|auto|manual) ;;
  *) die "--source must be 'auto' or 'manual'" ;;
esac
if [ -n "$PMOS_IMG" ] && [ "$PMOS_SOURCE" != "manual" ]; then
  PMOS_SOURCE="manual"
fi
if [ "$ASSUME_YES" = "1" ] && [ -z "$PMOS_MODE" ]; then
  PMOS_MODE="dual"
fi
if [ "$REPLACE_EXISTING" = "1" ] && [ "$REMOVE_EXISTING" = "1" ]; then
  die "--replace-existing and --remove-existing are mutually exclusive"
fi
if [ "$REMOVE_LEGACY" = "1" ] && [ "$REMOVE_EXISTING" != "1" ]; then
  die "--remove-legacy requires --remove-existing"
fi
if [ "$REMOVE_EXISTING" = "1" ] && [ "$PMOS_MODE" = "whole" ]; then
  die "--remove-existing applies to the dual-boot install; use --mode dual"
fi

if ! [[ "$ROOTFS_SIZE" =~ ^[0-9]+[KkMmGgTt]?$ ]]; then
  echo "ERROR: invalid --size '$ROOTFS_SIZE' (use e.g. 32G or 64G)" >&2
  exit 2
fi

ROOTFS_SIZE_BYTES="$(python3 -c '
import sys
s = sys.argv[1].upper()
mult = {"K": 1024, "M": 1024**2, "G": 1024**3, "T": 1024**4}
print(int(s[:-1]) * mult[s[-1]] if s[-1] in mult else int(s))
' "$ROOTFS_SIZE")"

if [ "$ROOTFS_SIZE_BYTES" -lt 2147483648 ]; then
  echo "ERROR: --size must be at least 2G" >&2
  exit 2
fi

# Installation mode.  The whole-device path only prepares an offline package;
# it never writes userdata while Android is running.
if [ -z "$PMOS_MODE" ]; then
  if [ "$ASSUME_YES" = "1" ] || ! is_tty; then
    PMOS_MODE="dual"
  else
    echo "Installation mode:"
    echo "  1) Dual boot - keep Android, install pmOS in $INSTALL_DIR (default)"
    echo "  2) Whole device - prepare an offline package; Android userdata is not touched"
    MODE_CHOICE=$(ask_line "Select [1-2] (default 1): " "1")
    case "$MODE_CHOICE" in
      1|dual|d) PMOS_MODE="dual" ;;
      2|whole|w) PMOS_MODE="whole" ;;
      *) die "invalid mode selection '$MODE_CHOICE'" ;;
    esac
  fi
fi

echo "========================================================================"
echo " postmarketOS UFS Loopback RootFS Installer (Lenovo Tab Plus TB351FU)"
echo "========================================================================"
echo "Mode: ${PMOS_MODE}"
echo "RootFS size: $ROOTFS_SIZE ($ROOTFS_SIZE_BYTES bytes)"

step check "Checking host tools and deploy payload"
progress 2 "Checking host tools"

# 1. Check prerequisites
command -v adb >/dev/null 2>&1 || { echo "ERROR: adb is required but not installed."; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "ERROR: python3 is required but not installed."; exit 1; }

if [ ! -d "$DEPLOY_DIR" ]; then
  echo "ERROR: Deploy directory missing at $DEPLOY_DIR"
  exit 1
fi

# 2. Check device connectivity via ADB
step device "Waiting for an Android device over ADB"
progress 5 "Checking ADB connection"
echo "Checking ADB device connection..."
if ! adb get-state 1>/dev/null 2>&1; then
  echo "ERROR: Device not found. Ensure tablet is booted into Android with USB debugging enabled."
  exit 1
fi

if [ -n "${ANDROID_SERIAL:-}" ]; then
  DEVICE_ID="$ANDROID_SERIAL"
else
  DEVICE_ID=$(adb devices | grep -w "device" | head -n 1 | awk '{print $1}')
  export ANDROID_SERIAL="$DEVICE_ID"
fi
if [ -z "${DEVICE_ID:-}" ]; then
  echo "ERROR: no adb device in 'device' state."
  exit 1
fi
echo "Found device: $DEVICE_ID"

# Verify root ADB access
adb shell "su -c 'id'" | grep -q "uid=0" || {
  echo "ERROR: Root ADB access required. Grant su permissions in Magisk/KernelSU."
  exit 1
}
echo "Root ADB access confirmed."
progress 10 "Root access confirmed"

# 2b. Userdata geometry.  The whole-device package is sized to the real
# userdata partition unless the user passed --size explicitly.
USERDATA_BYTES=$(adb shell "su -c 'blockdev --getsize64 /dev/block/by-name/userdata'" \
  2>/dev/null | tr -d '\r' || true)
if [[ "$USERDATA_BYTES" =~ ^[0-9]+$ ]] && [ "$USERDATA_BYTES" -ge 2147483648 ]; then
  echo "Android userdata partition: $USERDATA_BYTES bytes"
else
  USERDATA_BYTES=""
  echo "WARNING: could not determine the userdata partition size." >&2
fi

if [ "$PMOS_MODE" = "whole" ] && [ "$ROOTFS_SIZE_EXPLICIT" = "0" ] && \
   [ -n "$USERDATA_BYTES" ]; then
  ROOTFS_SIZE_BYTES="$USERDATA_BYTES"
  ROOTFS_SIZE=$(python3 -c '
import sys
b = int(sys.argv[1])
print("%.1fG" % (b / (1024 ** 3)))
' "$ROOTFS_SIZE_BYTES")
  echo "Whole-device image size matched to userdata: $ROOTFS_SIZE ($ROOTFS_SIZE_BYTES bytes)"
fi
if [ "$PMOS_MODE" = "whole" ] && [ -n "$USERDATA_BYTES" ] && \
   [ "$ROOTFS_SIZE_BYTES" -gt "$USERDATA_BYTES" ]; then
  die "the whole-device image ($ROOTFS_SIZE_BYTES bytes) is larger than userdata ($USERDATA_BYTES bytes)"
fi

# 2c. Detect an existing pmOS installation before touching anything.  The
# encrypted directory is still visible as a directory even when its contents
# are name-encrypted and the key is evicted.
step existing "Checking for an existing pmOS installation"
if adb shell "su -c '[ -d $INSTALL_DIR ]'" >/dev/null 2>&1; then
  EXISTING_DIR=1
fi
if adb shell "su -c '[ -f /data/pmos_legacy/rootfs.img ]'" >/dev/null 2>&1; then
  EXISTING_LEGACY=1
fi
if adb shell "su -c '[ -f $INSTALL_MARKER ]'" >/dev/null 2>&1; then
  EXISTING_MARKER=1
  echo "Installation marker:"
  adb shell "su -c 'cat $INSTALL_MARKER'" 2>/dev/null | tr -d '\r' | sed 's/^/  /' || true
fi

if [ "$EXISTING_DIR" = "1" ]; then
  echo "Existing pmOS installation found: $INSTALL_DIR"
  if [ "$EXISTING_LEGACY" = "1" ]; then
    echo "Legacy Android-readable image also present: /data/pmos_legacy/rootfs.img"
  fi
  if [ "$PMOS_MODE" = "whole" ]; then
    echo "Whole-device preparation uses a temporary staging directory and"
    echo "does not modify the existing dual-boot installation."
  elif [ "$REMOVE_EXISTING" = "1" ]; then
    EXISTING_ACTION="remove"
  elif [ "$REPLACE_EXISTING" = "1" ]; then
    EXISTING_ACTION="replace"
  elif [ "$ASSUME_YES" = "1" ] || ! is_tty; then
    die "an installation already exists; pass --replace-existing or --remove-existing"
  else
    echo "  1) Replace the pmOS rootfs (Android is kept)"
    echo "  2) Remove the pmOS installation and exit"
    echo "  3) Cancel"
    EXISTING_CHOICE=$(ask_line "Select [1-3] (default 1): " "1")
    case "$EXISTING_CHOICE" in
      1|replace|r) EXISTING_ACTION="replace" ;;
      2|remove|rm) EXISTING_ACTION="remove" ;;
      3|cancel|c) echo "Cancelled."; exit 0 ;;
      *) die "invalid selection '$EXISTING_CHOICE'" ;;
    esac
  fi
else
  echo "No existing pmOS installation at $INSTALL_DIR."
  if [ "$REMOVE_EXISTING" = "1" ]; then
    EXISTING_ACTION="remove"
  fi
fi

if [ "$EXISTING_ACTION" = "remove" ]; then
  if [ "$EXISTING_DIR" = "0" ] && [ "$EXISTING_LEGACY" = "0" ] && \
     [ "$EXISTING_MARKER" = "0" ]; then
    echo "Nothing to remove."
    printf '%%%%PMOS_DONE\n'
    exit 0
  fi
  # The legacy Android-readable image is never removed implicitly: it may be
  # the only bootable copy left.  Deleting it requires --remove-legacy.
  if [ "$EXISTING_LEGACY" = "1" ] && [ "$REMOVE_LEGACY" != "1" ] && \
     [ "$EXISTING_DIR" = "0" ] && [ "$EXISTING_MARKER" = "0" ]; then
    echo "Only the legacy Android-readable image is present:"
    echo "  /data/pmos_legacy/rootfs.img"
    echo "It was left untouched.  Re-run with --remove-existing --remove-legacy"
    echo "to delete it as well."
    printf '%%%%PMOS_DONE\n'
    exit 0
  fi
  step remove "Removing the existing pmOS installation"
  progress 15 "Removing the existing installation"
  if remove_device_dir "$INSTALL_DIR" "$INSTALL_MARKER" "$REMOVE_LEGACY"; then
    echo "Existing pmOS installation removed."
    if [ "$EXISTING_LEGACY" = "1" ] && [ "$REMOVE_LEGACY" != "1" ]; then
      echo "The legacy image /data/pmos_legacy/rootfs.img was kept"
      echo "(use --remove-legacy to delete it)."
    fi
    echo "Android and slot A were not modified."
    progress 100 "Existing installation removed"
    printf '%%%%PMOS_DONE\n'
    exit 0
  fi
  die "removal failed; the installation was left as intact as possible"
fi

[ -f "$DEPLOY_DIR/modules.tar.xz" ] || {
  echo "ERROR: customization archive missing: $DEPLOY_DIR/modules.tar.xz" >&2
  echo "Run this from a full checkout of the pmOS deployment tree." >&2
  exit 1
}

# 3. Select and download the source image, then collect the account details.
step image "Preparing the postmarketOS rootfs image"
select_image_source
prepare_account

# Record free space now, but check it after the sparse pack is built: the
# device only receives allocated extents, not the logical image size.
# Android toybox does not support `df -B1`; use -k (1 KiB blocks) and let a
# failed probe fall through instead of tripping `set -e`.
DATA_AVAIL=""
DATA_AVAIL_KB=$(adb shell 'df -k /data' 2>/dev/null | awk 'NR==2 {print $4}' | tr -d '\r' || true)
if ! [[ "$DATA_AVAIL_KB" =~ ^[0-9]+$ ]]; then
  DATA_AVAIL_KB=$(adb shell 'df /data' 2>/dev/null | awk 'NR==2 {print $4}' | tr -d '\r' || true)
fi
if [[ "$DATA_AVAIL_KB" =~ ^[0-9]+$ ]]; then
  DATA_AVAIL=$((DATA_AVAIL_KB * 1024))
  echo "Free space on /data: $DATA_AVAIL bytes"
else
  echo "WARNING: could not read free space on /data; continuing." >&2
fi
progress 12 "Rootfs image selected"
mkdir -p "$TMP_DIR"

# 4. Prepare base rootfs image
# Android has no zstd/xz binary, so decompress/extract on the host and push a
# raw ext4 image.  The image is only moved into the install directory after
# transfer.
ROOTFS_RAW="$TMP_DIR/rootfs_base.img"
rm -f "$ROOTFS_RAW"
case "$PMOS_IMG" in
  *.zst)
    command -v zstd >/dev/null 2>&1 || die "zstd is required for .zst images"
    echo "Decompressing zstd image on the host: $PMOS_IMG"
    zstd -d -f "$PMOS_IMG" -o "$ROOTFS_RAW"
    ;;
  *.xz)
    command -v xz >/dev/null 2>&1 || die "xz is required for .xz images"
    echo "Decompressing xz image on the host: $PMOS_IMG"
    xz -dc "$PMOS_IMG" > "$ROOTFS_RAW"
    ;;
  *.gz)
    command -v gzip >/dev/null 2>&1 || die "gzip is required for .gz images"
    echo "Decompressing gzip image on the host: $PMOS_IMG"
    gzip -dc "$PMOS_IMG" > "$ROOTFS_RAW"
    ;;
  *.img)
    # Check if GPT disk image or a raw ext4 partition.
    if python3 - "$PMOS_IMG" <<'PY'
import sys
with open(sys.argv[1], "rb") as f:
    f.seek(512)
    sys.exit(0 if f.read(8) == b"EFI PART" else 1)
PY
    then
      echo "Detected GPT disk image. Extracting partition 2 (rootfs)..."
      # Parse the GPT directly instead of relying on fdisk, whose output format
      # and availability differ between hosts.  The rootfs is partition 2.
      python3 - "$PMOS_IMG" "$ROOTFS_RAW" <<'PY'
import struct
import subprocess
import sys

image, output = sys.argv[1], sys.argv[2]
with open(image, "rb") as fh:
    fh.seek(512)
    header = fh.read(92)
    if len(header) < 92 or header[:8] != b"EFI PART":
        sys.exit("not a GPT image")
    entries_lba, count, entry_size = struct.unpack_from("<QII", header, 72)
    if entry_size < 128 or count > 256:
        sys.exit("unsupported GPT layout")
    fh.seek(entries_lba * 512)
    entries = fh.read(count * entry_size)

partitions = []
for index in range(count):
    entry = entries[index * entry_size:(index + 1) * entry_size]
    if len(entry) < 128 or entry[:16] == b"\x00" * 16:
        continue
    first, last = struct.unpack_from("<QQ", entry, 32)
    partitions.append((index + 1, first, last))

if len(partitions) < 2:
    sys.exit("expected at least two GPT partitions")

_, first, sectors = partitions[1]
count_sectors = sectors - first + 1
print(f"Extracting partition 2: skip={first} count={count_sectors}")
with open(output, "wb") as out:
    dd = subprocess.Popen(
        ["dd", f"if={image}", "bs=512", f"skip={first}",
         f"count={count_sectors}"],
        stdout=out,
    )
    rc = dd.wait()
sys.exit(rc)
PY
    else
      echo "Detected raw ext4 partition. Copying to a temporary image..."
      cp -f "$PMOS_IMG" "$ROOTFS_RAW"
    fi
    ;;
  *)
    die "unsupported rootfs image type: $PMOS_IMG"
    ;;
esac

if [ ! -f "$ROOTFS_RAW" ]; then
  echo "ERROR: prepared rootfs image not found: $ROOTFS_RAW" >&2
  exit 1
fi
if ! python3 - "$ROOTFS_RAW" <<'PY'
import sys
# The ext4 superblock starts at byte 1024; s_magic is at offset 0x38
# (56) inside it, so the 0x53ef signature is at byte 1080.
with open(sys.argv[1], "rb") as f:
    f.seek(1024 + 56)
    sys.exit(0 if f.read(2) == b"\x53\xef" else 1)
PY
then
  die "the prepared image is not an ext4 filesystem (no 0x53ef superblock magic)"
fi

# Android 13/14 ship e2fsprogs 1.46, which refuses filesystems with the newer
# orphan_file feature and therefore cannot resize the image.  Clear it on the
# host; the filesystem remains fully compatible with the pmOS kernel.
if ! command -v tune2fs >/dev/null 2>&1; then
  echo "WARNING: tune2fs not found; Android may not be able to resize this image." >&2
elif tune2fs -l "$ROOTFS_RAW" 2>/dev/null | grep -qw orphan_file; then
  echo "Clearing ext4 orphan_file for Android e2fsprogs compatibility..."
  tune2fs -O ^orphan_file "$ROOTFS_RAW" || {
    echo "ERROR: could not clear orphan_file from $ROOTFS_RAW" >&2
    exit 1
  }
fi

# 4b. Resize on the host.  Growing or shrinking a loopback ext4 image on
# Android is fragile; doing it here also means the device only receives the
# final image.  e2fsck exit status 1 means "corrected", which is success.
CURRENT_IMG_BYTES=$(stat -c %s "$ROOTFS_RAW")
if [ "$CURRENT_IMG_BYTES" -ne "$ROOTFS_SIZE_BYTES" ]; then
  command -v e2fsck >/dev/null 2>&1 || {
    echo "ERROR: e2fsck is required to resize the rootfs image." >&2
    exit 1
  }
  command -v resize2fs >/dev/null 2>&1 || {
    echo "ERROR: resize2fs is required to resize the rootfs image." >&2
    exit 1
  }
  echo "Resizing rootfs on the host: $CURRENT_IMG_BYTES -> $ROOTFS_SIZE_BYTES bytes"
  [ $((ROOTFS_SIZE_BYTES % 512)) -eq 0 ] || \
    die "the requested rootfs size must be a multiple of 512 bytes"
  if [ "$CURRENT_IMG_BYTES" -gt "$ROOTFS_SIZE_BYTES" ]; then
    set +e
    e2fsck -f -y "$ROOTFS_RAW"
    fsck_rc=$?
    set -e
    [ "$fsck_rc" -le 1 ] || {
      echo "ERROR: e2fsck failed before shrinking ($fsck_rc)." >&2
      exit 1
    }
    resize2fs "$ROOTFS_RAW" "$((ROOTFS_SIZE_BYTES / 512))s" || {
      echo "ERROR: could not shrink the filesystem; choose a size at least as large as the image." >&2
      exit 1
    }
    truncate -s "$ROOTFS_SIZE_BYTES" "$ROOTFS_RAW"
  else
    truncate -s "$ROOTFS_SIZE_BYTES" "$ROOTFS_RAW"
    set +e
    e2fsck -f -y "$ROOTFS_RAW"
    fsck_rc=$?
    set -e
    [ "$fsck_rc" -le 1 ] || {
      echo "ERROR: e2fsck failed before growing ($fsck_rc)." >&2
      exit 1
    }
    resize2fs "$ROOTFS_RAW" || {
      echo "ERROR: could not grow the filesystem." >&2
      exit 1
    }
  fi
fi

# 4c. Storage keys.  The metadata key belongs to Android's metadata
# encryption; the fscrypt key belongs to /data/pmos only.  The fscrypt key is
# created here on first use and kept in a 0600 file outside this repository.
# A whole-device preparation still uses fscrypt for its temporary staging
# directory, but it does not need Android's metadata key.
PMOS_DM_METADATA_KEY="${PMOS_DM_METADATA_KEY:-}"
PMOS_FSCRYPT_MASTER_KEY="${PMOS_FSCRYPT_MASTER_KEY:-}"
PMOS_FSCRYPT_IDENTIFIER="${PMOS_FSCRYPT_IDENTIFIER:-}"
PMOS_DM_CIPHER="${PMOS_DM_CIPHER:-}"
PMOS_DM_SECTOR_SIZE="${PMOS_DM_SECTOR_SIZE:-}"
PMOS_DM_WRAPPEDKEY="${PMOS_DM_WRAPPEDKEY:-}"
if [ -f "$STORAGE_KEYS_FILE" ]; then
  set -a
  # shellcheck disable=SC1090
  . "$STORAGE_KEYS_FILE"
  set +a
fi

save_storage_keys() {
  local dir tmp
  dir="$(dirname "$STORAGE_KEYS_FILE")"
  mkdir -p "$dir"
  chmod 700 "$dir" 2>/dev/null || true
  tmp="$STORAGE_KEYS_FILE.tmp.$$"
  (
    umask 077
    {
      printf 'PMOS_DM_METADATA_KEY=%s\n' "$PMOS_DM_METADATA_KEY"
      printf 'PMOS_FSCRYPT_MASTER_KEY=%s\n' "$PMOS_FSCRYPT_MASTER_KEY"
      printf 'PMOS_FSCRYPT_IDENTIFIER=%s\n' "$PMOS_FSCRYPT_IDENTIFIER"
      [ -n "$PMOS_DM_CIPHER" ] && printf 'PMOS_DM_CIPHER=%s\n' "$PMOS_DM_CIPHER"
      [ -n "$PMOS_DM_SECTOR_SIZE" ] && printf 'PMOS_DM_SECTOR_SIZE=%s\n' "$PMOS_DM_SECTOR_SIZE"
      [ -n "$PMOS_DM_WRAPPEDKEY" ] && printf 'PMOS_DM_WRAPPEDKEY=%s\n' "$PMOS_DM_WRAPPEDKEY"
    } > "$tmp"
    chmod 600 "$tmp"
  )
  mv -f "$tmp" "$STORAGE_KEYS_FILE"
}

if [ -z "$PMOS_FSCRYPT_MASTER_KEY" ]; then
  echo "Generating a new 32-byte fscrypt key (value not shown)."
  PMOS_FSCRYPT_MASTER_KEY="$(python3 -c 'import secrets; print(secrets.token_hex(32))')"
  PMOS_FSCRYPT_IDENTIFIER=""
fi
require_hex PMOS_FSCRYPT_MASTER_KEY "$PMOS_FSCRYPT_MASTER_KEY" 32 128
require_hex PMOS_DM_METADATA_KEY "$PMOS_DM_METADATA_KEY" 64 128
require_hex PMOS_FSCRYPT_IDENTIFIER "$PMOS_FSCRYPT_IDENTIFIER" 32 32
if [ -z "$PMOS_DM_METADATA_KEY" ] && [ "$PMOS_MODE" = "dual" ]; then
  echo "WARNING: no Android metadata key in $STORAGE_KEYS_FILE." >&2
  echo "         Installation can proceed, but the boot image needs that key." >&2
fi

KEY_TMP="$TMP_DIR/fscrypt.key"
( umask 077; printf '%s\n' "$PMOS_FSCRYPT_MASTER_KEY" > "$KEY_TMP" )
chmod 600 "$KEY_TMP"
DERIVED_IDENT=$(python3 "$SCRIPT_DIR/deploy/tools/fscrypt_identifier.py" "$KEY_TMP")
if [ -n "$PMOS_FSCRYPT_IDENTIFIER" ] && [ "$PMOS_FSCRYPT_IDENTIFIER" != "$DERIVED_IDENT" ]; then
  echo "ERROR: stored fscrypt identifier does not match the stored key." >&2
  rm -f "$KEY_TMP"
  exit 1
fi
PMOS_FSCRYPT_IDENTIFIER="$DERIVED_IDENT"
save_storage_keys
echo "fscrypt identifier: $PMOS_FSCRYPT_IDENTIFIER (public; key value not shown)"

if [ "$PMOS_MODE" = "whole" ] && \
   adb shell "su -c '[ -d $WHOLE_WORK_DIR ]'" >/dev/null 2>&1; then
  echo "Removing a leftover whole-device staging directory..."
  remove_device_dir "$WHOLE_WORK_DIR" "${WHOLE_WORK_DIR}_install.env" 0 || \
    die "could not remove the leftover staging directory $WHOLE_WORK_DIR"
fi

# 4d. Pack only allocated extents.  adb push expands holes, so pushing the
# logical image directly would consume the whole image size on the tablet.
PACK_HOST="$TMP_DIR/rootfs.sparsepack"
echo "Packing allocated extents (holes are not transferred)..."
python3 "$SCRIPT_DIR/deploy/tools/make_sparse_pack.py" "$ROOTFS_RAW" "$PACK_HOST"
PACK_BYTES=$(stat -c %s "$PACK_HOST")
if [ "$PMOS_MODE" = "whole" ]; then
  NEED_BYTES=$((PACK_BYTES * 2 + 2147483648))
else
  NEED_BYTES=$((PACK_BYTES + 1073741824))
fi
if [ -n "$DATA_AVAIL" ] && [ "$DATA_AVAIL" -lt "$NEED_BYTES" ]; then
  echo "ERROR: need at least $NEED_BYTES bytes free on /data." >&2
  if [ "$PMOS_MODE" = "whole" ]; then
    echo "       Whole-device preparation temporarily stores the image and its offline pack." >&2
  fi
  exit 1
fi

echo "Base rootfs image ready: $(ls -lh "$ROOTFS_RAW" | awk '{print $5}')"
echo "Sparse transfer pack: $(ls -lh "$PACK_HOST" | awk '{print $5}')"

# 5. Build customization payload package
step payload "Building the device customization payload"
progress 25 "Packaging modules, services and configs"
echo "Packaging system customizations (modules, services, scripts, configs)..."
PAYLOAD_DIR="$TMP_DIR/payload"
rm -rf "$PAYLOAD_DIR"
mkdir -p "$PAYLOAD_DIR/usr/lib/modules" \
         "$PAYLOAD_DIR/usr/local/bin" \
         "$PAYLOAD_DIR/usr/bin" \
         "$PAYLOAD_DIR/root" \
         "$PAYLOAD_DIR/etc/systemd/system/multi-user.target.wants" \
         "$PAYLOAD_DIR/etc/systemd/system/timers.target.wants" \
         "$PAYLOAD_DIR/etc/systemd/user/default.target.wants" \
         "$PAYLOAD_DIR/usr/lib/systemd/user" \
         "$PAYLOAD_DIR/etc/udev/rules.d" \
         "$PAYLOAD_DIR/etc/phosh" \
         "$PAYLOAD_DIR/etc/sysctl.d" \
         "$PAYLOAD_DIR/etc/modprobe.d" \
         "$PAYLOAD_DIR/etc/systemd/system.conf.d" \
         "$PAYLOAD_DIR/etc/nftables.d" \
         "$PAYLOAD_DIR/usr/share/glib-2.0/schemas" \
         "$PAYLOAD_DIR/usr/share/postmarketos-tweaks" \
         "$PAYLOAD_DIR/data_host"

# Extract out-of-tree hardware modules into usr/lib/modules/extra
mkdir -p "$PAYLOAD_DIR/usr/lib/modules/extra"
tar -xf "$DEPLOY_DIR/modules.tar.xz" -C "$PAYLOAD_DIR/usr/lib/modules/extra"

# Built-in module index for the target kernel.  zram is built in, but
# zramstart calls modprobe; without modules.builtin + depmod, kmod
# reports "Module zram not found" and the script aborts under set -e.
BUILTIN_STAGE="$SCRIPT_DIR/../artifacts/modules_builtin"
if [ -f "$BUILTIN_STAGE/kernel.release" ]; then
  KREL="$(cat "$BUILTIN_STAGE/kernel.release")"
  mkdir -p "$PAYLOAD_DIR/usr/lib/modules/$KREL"
  cp "$BUILTIN_STAGE/$KREL/modules.builtin" \
     "$BUILTIN_STAGE/$KREL/modules.builtin.modinfo" \
     "$PAYLOAD_DIR/usr/lib/modules/$KREL/"
  echo "$KREL" > "$PAYLOAD_DIR/usr/lib/modules/.pmos_kernel_release"
fi

# Extract proprietary vendor firmware into usr/lib/firmware
if [ -f "$DEPLOY_DIR/firmware.tar.xz" ]; then
  mkdir -p "$PAYLOAD_DIR/usr/lib/firmware"
  tar -xf "$DEPLOY_DIR/firmware.tar.xz" -C "$PAYLOAD_DIR/usr/lib/firmware"
  # Also create convenience symlink for aw88395_acf.bin
  if [ -f "$PAYLOAD_DIR/usr/lib/firmware/aw883xx_acf.bin" ]; then
    ln -sf aw883xx_acf.bin "$PAYLOAD_DIR/usr/lib/firmware/aw88395_acf.bin"
  fi
  # Link /vendor/firmware and /etc/firmware
  mkdir -p "$PAYLOAD_DIR/vendor" "$PAYLOAD_DIR/etc"
  ln -sf /usr/lib/firmware "$PAYLOAD_DIR/vendor/firmware"
  ln -sf /usr/lib/firmware "$PAYLOAD_DIR/etc/firmware"
fi

# Copy ALSA UCM2 configurations for MT8186 audio
if [ -d "$DEPLOY_DIR/configs/ucm2" ]; then
  mkdir -p "$PAYLOAD_DIR/usr/share/alsa/ucm2"
  cp -a "$DEPLOY_DIR/configs/ucm2/"* "$PAYLOAD_DIR/usr/share/alsa/ucm2/"
fi

# Copy local bin scripts
cp -a "$DEPLOY_DIR/scripts/"* "$PAYLOAD_DIR/usr/local/bin/"
chmod 755 "$PAYLOAD_DIR/usr/local/bin/"*
if [ -f "$PAYLOAD_DIR/usr/local/bin/phosh-session" ]; then
  mv "$PAYLOAD_DIR/usr/local/bin/phosh-session" "$PAYLOAD_DIR/usr/bin/phosh-session"
fi

# Copy root scripts
cp -a "$DEPLOY_DIR/root_scripts/"* "$PAYLOAD_DIR/root/"

# Copy systemd services and wire symlinks
for s in "$DEPLOY_DIR/services/"*.service; do
  [ -f "$s" ] || continue
  sname=$(basename "$s")
  cp "$s" "$PAYLOAD_DIR/etc/systemd/system/$sname"
  ln -sf "/etc/systemd/system/$sname" "$PAYLOAD_DIR/etc/systemd/system/multi-user.target.wants/$sname"
done

# Timers are installed system-wide and started by timers.target.
for t in "$DEPLOY_DIR/services/"*.timer; do
  [ -f "$t" ] || continue
  tname=$(basename "$t")
  cp "$t" "$PAYLOAD_DIR/etc/systemd/system/$tname"
  ln -sf "/etc/systemd/system/$tname" "$PAYLOAD_DIR/etc/systemd/system/timers.target.wants/$tname"
done

# Per-user services (e.g. the jack-profile watcher) must run inside the
# graphical user session so XDG_RUNTIME_DIR points at the PipeWire-Pulse
# socket.  Install them as vendor user units and enable them for every user
# through /etc/systemd/user/default.target.wants.
for s in "$DEPLOY_DIR/services/user/"*.service; do
  [ -f "$s" ] || continue
  sname=$(basename "$s")
  cp "$s" "$PAYLOAD_DIR/usr/lib/systemd/user/$sname"
  ln -sf "/usr/lib/systemd/user/$sname" \
    "$PAYLOAD_DIR/etc/systemd/user/default.target.wants/$sname"
done

# Copy service drop-in overrides (e.g. upower.service.d)
if [ -d "$DEPLOY_DIR/services/upower.service.d" ]; then
  mkdir -p "$PAYLOAD_DIR/etc/systemd/system/upower.service.d"
  cp -a "$DEPLOY_DIR/services/upower.service.d/"* "$PAYLOAD_DIR/etc/systemd/system/upower.service.d/"
  mkdir -p "$PAYLOAD_DIR/etc/systemd/system/graphical.target.wants"
  ln -sf "/usr/lib/systemd/system/upower.service" "$PAYLOAD_DIR/etc/systemd/system/graphical.target.wants/upower.service"
fi

# Copy configurations
cp "$DEPLOY_DIR/configs/70-touchscreen-pen.rules" "$PAYLOAD_DIR/etc/udev/rules.d/"
cp "$DEPLOY_DIR/configs/phoc.ini" "$PAYLOAD_DIR/etc/phosh/phoc.ini"
cp "$DEPLOY_DIR/configs/99-panic.conf" "$PAYLOAD_DIR/etc/sysctl.d/99-panic.conf"
# Must sort after 99-panic.conf so kernel faults do not auto-reboot.
cp "$DEPLOY_DIR/configs/99-zz-no-auto-reboot.conf" "$PAYLOAD_DIR/etc/sysctl.d/99-zz-no-auto-reboot.conf"
# Camera modules stay manual-load only.
cp "$DEPLOY_DIR/configs/90-camera-manual.conf" "$PAYLOAD_DIR/etc/modprobe.d/90-camera-manual.conf"
cp "$DEPLOY_DIR/configs/watchdog.conf" "$PAYLOAD_DIR/etc/systemd/system.conf.d/watchdog.conf"

# Display mode: GSettings schema + Mobile Settings (phosh-mobile-settings)
# YAML panel.  The daemon applies 60/90/adaptive to pmosfb.refresh_hz.
cp "$DEPLOY_DIR/configs/org.postmarketos.Display.gschema.xml" \
   "$PAYLOAD_DIR/usr/share/glib-2.0/schemas/"
cp "$DEPLOY_DIR/configs/postmarketos-tweaks/display.yml" \
   "$PAYLOAD_DIR/usr/share/postmarketos-tweaks/"

# zram swap choice: GSettings schema + Mobile Settings panel.  The
# pmos-zram-mode daemon starts/stops postmarketos-zram-swap.service.
cp "$DEPLOY_DIR/configs/org.postmarketos.Zram.gschema.xml" \
   "$PAYLOAD_DIR/usr/share/glib-2.0/schemas/"
cp "$DEPLOY_DIR/configs/postmarketos-tweaks/zram.yml" \
   "$PAYLOAD_DIR/usr/share/postmarketos-tweaks/"

# nftables: allow DHCPv4 client replies so the default-drop ruleset
# cannot break DHCP.  SSH is already allowed by 50_sshd.nft.
cp "$DEPLOY_DIR/configs/nftables.d/10_dhcp-client.nft" \
   "$PAYLOAD_DIR/etc/nftables.d/"

# Copy ALSA UCM2 audio configurations
if [ -d "$DEPLOY_DIR/configs/ucm2" ]; then
  mkdir -p "$PAYLOAD_DIR/usr/share/alsa/ucm2"
  cp -a "$DEPLOY_DIR/configs/ucm2/"* "$PAYLOAD_DIR/usr/share/alsa/ucm2/"
fi

PAYLOAD_TAR="$TMP_DIR/pmos_payload.tar.gz"
tar -czf "$PAYLOAD_TAR" -C "$PAYLOAD_DIR" .
echo "Customization payload ready: $(ls -lh "$PAYLOAD_TAR" | awk '{print $5}')"
progress 35 "Customization payload ready"

# 6. Push archives and device helpers
step transfer "Pushing sparse rootfs and payload to the tablet"
progress 38 "Transferring allocated rootfs extents"
echo "Building the aarch64 device helpers..."
bash "$SCRIPT_DIR/deploy/tools/build_tools.sh" "$TMP_DIR/tools"

if [ "$PMOS_MODE" = "whole" ]; then
  DEVICE_INSTALL_DIR="$WHOLE_WORK_DIR"
  WHOLE_FLAG=1
else
  DEVICE_INSTALL_DIR="$INSTALL_DIR"
  WHOLE_FLAG=0
fi

echo "Pushing sparse pack, payload, helpers and key to device..."
adb push "$PACK_HOST" /data/local/tmp/pmos_rootfs.sparsepack
adb push "$PAYLOAD_TAR" /data/local/tmp/pmos_payload.tar.gz
adb push "$TMP_DIR/tools/fscrypt_helper" /data/local/tmp/pmos_fscrypt_helper
adb push "$TMP_DIR/tools/sparse_pack" /data/local/tmp/pmos_sparse_pack

DEVICE_SCRIPT="$TMP_DIR/device_install.sh"
sed -e "s|@ROOTFS_SIZE_BYTES@|$ROOTFS_SIZE_BYTES|g" \
    -e "s|@ROOTFS_SIZE@|$ROOTFS_SIZE|g" \
    -e "s|@FSCRYPT_IDENT@|$PMOS_FSCRYPT_IDENTIFIER|g" \
    -e "s|@KEEP_OLD@|$KEEP_OLD|g" \
    -e "s|@INSTALL_DIR@|$DEVICE_INSTALL_DIR|g" \
    -e "s|@ACCOUNT_ENABLED@|$ACCOUNT_ENABLED|g" \
    -e "s|@WHOLE_MODE@|$WHOLE_FLAG|g" \
    "$SCRIPT_DIR/deploy/installer/device_install.sh" > "$DEVICE_SCRIPT"
adb push "$DEVICE_SCRIPT" /data/local/tmp/pmos_install.sh

if [ "$ACCOUNT_ENABLED" = "1" ]; then
  adb push "$ACCOUNT_ENV_HOST" /data/local/tmp/pmos_account.env
fi

# Push the key last, so a failure while transferring the large files cannot
# leave a key on the device without a device script to clean it up.
adb push "$KEY_TMP" /data/local/tmp/pmos_fscrypt.key
adb shell "su -c 'chmod 755 /data/local/tmp/pmos_fscrypt_helper /data/local/tmp/pmos_sparse_pack; chmod 600 /data/local/tmp/pmos_fscrypt.key'"
if [ "$ACCOUNT_ENABLED" = "1" ]; then
  adb shell "su -c 'chmod 600 /data/local/tmp/pmos_account.env'"
fi
rm -f "$KEY_TMP"
progress 55 "Transfer complete"

# 7. Run the device-side installer
step unpack "Unpacking and configuring the encrypted rootfs"
progress 60 "Preparing the encrypted rootfs on the tablet"
echo "Running the device-side installer (key add, sparse apply, payload, account, key evict)..."
set +e
DEVICE_OUT=$(adb shell "su -c 'sh /data/local/tmp/pmos_install.sh'" 2>&1)
DEVICE_RC=$?
set -e
printf '%s\n' "$DEVICE_OUT"
if [ "$DEVICE_RC" -ne 0 ]; then
  echo "ERROR: device installer failed with exit code $DEVICE_RC." >&2
  echo "The previous rootfs.img (if any) remains in place. Fix the cause and rerun." >&2
  exit "$DEVICE_RC"
fi

DEVICE_IDENT=$(printf '%s\n' "$DEVICE_OUT" | sed -n 's/^PMOS_IDENTIFIER=\([0-9a-f][0-9a-f]*\)$/\1/p' | tail -n 1)
[ -n "$DEVICE_IDENT" ] || DEVICE_IDENT="$PMOS_FSCRYPT_IDENTIFIER"
if [ "${#DEVICE_IDENT}" -ne 32 ] || [ "$DEVICE_IDENT" != "$PMOS_FSCRYPT_IDENTIFIER" ]; then
  echo "ERROR: device reported fscrypt identifier '$DEVICE_IDENT', expected '$PMOS_FSCRYPT_IDENTIFIER'." >&2
  exit 1
fi

if [ "$PMOS_MODE" = "whole" ]; then
  WHOLE_PACK_BYTES=$(printf '%s\n' "$DEVICE_OUT" | \
    sed -n 's/^PMOS_WHOLE_PACK_BYTES=\([0-9][0-9]*\)$/\1/p' | tail -n 1)
  [ -n "$WHOLE_PACK_BYTES" ] || die "device did not report the whole-device pack size"

  WHOLE_PARENT=$(dirname "$WHOLE_OUTPUT")
  mkdir -p "$WHOLE_PARENT"
  HOST_FREE_KB=$(df -Pk "$WHOLE_PARENT" 2>/dev/null | awk 'NR==2 {print $4}' || true)
  if [[ "$HOST_FREE_KB" =~ ^[0-9]+$ ]]; then
    HOST_FREE_BYTES=$((HOST_FREE_KB * 1024))
    REQUIRED_BYTES=$((WHOLE_PACK_BYTES + 67108864))
    if [ "$HOST_FREE_BYTES" -lt "$REQUIRED_BYTES" ]; then
      adb shell "su -c 'rm -f $WHOLE_PACK_DEVICE'" >/dev/null 2>&1 || true
      die "not enough host space for the whole-device pack: need $REQUIRED_BYTES bytes in $WHOLE_PARENT"
    fi
  fi

  step export "Exporting the offline whole-device package"
  progress 75 "Pulling the whole-device pack"
  adb shell "su -c 'chmod 644 $WHOLE_PACK_DEVICE'"
  if ! adb pull "$WHOLE_PACK_DEVICE" "$TMP_DIR/whole.sparsepack"; then
    adb shell "su -c 'rm -f $WHOLE_PACK_DEVICE'" >/dev/null 2>&1 || true
    die "could not pull the whole-device pack; rerun --mode whole"
  fi
  LOCAL_WHOLE_PACK_BYTES=$(stat -c %s "$TMP_DIR/whole.sparsepack")
  if [ "$LOCAL_WHOLE_PACK_BYTES" -ne "$WHOLE_PACK_BYTES" ]; then
    adb shell "su -c 'rm -f $WHOLE_PACK_DEVICE'" >/dev/null 2>&1 || true
    die "whole-device pack transfer was incomplete ($LOCAL_WHOLE_PACK_BYTES of $WHOLE_PACK_BYTES bytes)"
  fi
  adb shell "su -c 'rm -f $WHOLE_PACK_DEVICE'" >/dev/null 2>&1 || true

  build_whole_package "$TMP_DIR/whole.sparsepack"
  progress 90 "Whole-device package ready"

  step cleanup "Removing the temporary whole-device staging install"
  if ! remove_device_dir "$WHOLE_WORK_DIR" "${WHOLE_WORK_DIR}_install.env" 0; then
    echo "WARNING: the staging directory $WHOLE_WORK_DIR could not be removed." >&2
    echo "         Rerun --mode whole (or remove it with the storage key) to reclaim space." >&2
  fi

  rm -rf "$TMP_DIR"
  echo "========================================================================"
  echo " SUCCESS: offline whole-device package prepared"
  echo ""
  echo " Package: $WHOLE_PACKAGE_DIR"
  if [ "$ACCOUNT_ENABLED" = "1" ]; then
    echo " Account: $PMOS_USERNAME"
    echo " Log in at the pmOS console as '$PMOS_USERNAME' with the password you entered."
  else
    echo " Account: image default"
  fi
  echo " Image:   $ROOTFS_SIZE_BYTES bytes logical"
  echo " Pack:    $WHOLE_PACK_BYTES bytes"
  echo ""
  echo " Android userdata was NOT erased or reformatted."
  echo " The package must be applied from recovery with /data unmounted."
  echo " It contains no boot image; userdata-direct boot support is required."
  echo " Read README.txt before using it."
  echo "========================================================================"
  progress 100 "Whole-device package ready"
  step done "Whole-device preparation complete"
  printf '%%%%PMOS_DONE\n'
  exit 0
fi

# Dual-boot path: record a small Android-readable marker for future runs.
write_install_marker

# Cleanup temporary host files
rm -rf "$TMP_DIR"

echo "========================================================================"
echo " SUCCESS: encrypted postmarketOS rootfs installed in $INSTALL_DIR"
echo ""
if [ "$ACCOUNT_ENABLED" = "1" ]; then
  echo " Account: $PMOS_USERNAME"
  echo " Log in at the pmOS console as '$PMOS_USERNAME' with the password you entered."
else
  echo " Account: image default"
fi
echo " Source:  ${PMOS_SOURCE:-unknown}"
echo ""
echo " Android now sees $INSTALL_DIR/rootfs.img as ENOKEY; the pmOS initramfs"
echo " unlocks it with the per-install key compiled into /init."
if [ "$EXISTING_LEGACY" = "1" ]; then
  echo " The old Android-readable image remains at /data/pmos_legacy."
fi
echo " Slot A and Android's own encryption were not modified."
echo ""
echo " Next steps (when you choose to test boot):"
echo "   1. Build /init with the key file:"
echo "        PMOS_STORAGE_KEYS=$STORAGE_KEYS_FILE $SCRIPT_DIR/build_and_flash_pmos.sh"
echo "   2. Flash slot B only, then select slot B from the boot menu."
echo "   3. If slot B does not reach userspace, return to slot A and capture"
echo "      /sys/fs/pstore before any further reboot."
echo "========================================================================"
progress 100 "RootFS installed and verified"
step done "RootFS installation complete"
printf '%%%%PMOS_DONE\n'
