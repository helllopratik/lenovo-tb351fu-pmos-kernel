#!/system/bin/sh
# Device-side half of the encrypted pmOS rootfs installer.
#
# This file is a template.  The host installer substitutes the @...@
# placeholders and pushes the result to /data/local/tmp/pmos_install.sh.
# Run it as root (su -c 'sh /data/local/tmp/pmos_install.sh').
#
# Invariants:
#   * Only /data/pmos is re-keyed; the directory is never moved across
#     fscrypt policies.
#   * rootfs.img.new is created inside the encrypted directory, verified,
#     and renamed within that directory.
#   * The per-install fscrypt key is removed from the Android keyring at the
#     end, leaving the image locked (ENOKEY) to Android.
set -e

ROOTFS_SIZE_BYTES="@ROOTFS_SIZE_BYTES@"
ROOTFS_SIZE="@ROOTFS_SIZE@"
EXPECTED_IDENT="@FSCRYPT_IDENT@"
KEEP_OLD="@KEEP_OLD@"
ACCOUNT_ENABLED="@ACCOUNT_ENABLED@"
WHOLE_MODE="@WHOLE_MODE@"

DATA=/data
DIR="@INSTALL_DIR@"
IMG=$DIR/rootfs.img
NEW=$DIR/rootfs.img.new
OLD=$DIR/rootfs.img.old
KEYFILE=/data/local/tmp/pmos_fscrypt.key
HELPER=/data/local/tmp/pmos_fscrypt_helper
PACK=/data/local/tmp/pmos_sparse_pack
PACKFILE=/data/local/tmp/pmos_rootfs.sparsepack
PAYLOAD=/data/local/tmp/pmos_payload.tar.gz
ACCOUNT_ENV=/data/local/tmp/pmos_account.env
WHOLEPACK=/data/local/tmp/pmos_whole.sparsepack
MNT=/data/local/tmp/pmos_chk

KEY_ADDED=0
INSTALL_OK=0
KEEP_WHOLEPACK=0
LOOP=""
MOUNTED=0
IDENT=""

log() { echo "PMOS: $*"; }
die() { echo "PMOS_ERROR: $*" >&2; exit 1; }

cleanup() {
  rc=$?
  if [ "$MOUNTED" = "1" ]; then
    umount "$MNT" 2>/dev/null || true
  fi
  if [ -n "$LOOP" ]; then
    losetup -d "$LOOP" 2>/dev/null || true
  fi
  if [ "$INSTALL_OK" != "1" ] && [ -f "$NEW" ]; then
    rm -f "$NEW" 2>/dev/null || true
  fi
  if [ "$KEY_ADDED" = "1" ] && [ -n "$IDENT" ]; then
    "$HELPER" delkey2 "$DATA" "$IDENT" >/dev/null 2>&1 || true
    sync
  fi
  if [ "$KEEP_WHOLEPACK" != "1" ] && [ -f "$WHOLEPACK" ]; then
    rm -f "$WHOLEPACK" 2>/dev/null || true
  fi
  rm -f "$KEYFILE" "$PACKFILE" "$PAYLOAD" "$ACCOUNT_ENV" "$HELPER" "$PACK" \
        /data/local/tmp/pmos_install.sh 2>/dev/null || true
  rm -rf "$MNT" 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT

[ -x "$HELPER" ] || die "fscrypt helper missing"
[ -x "$PACK" ] || die "sparse pack tool missing"
[ -f "$PACKFILE" ] || die "rootfs pack missing"
[ -f "$KEYFILE" ] || die "fscrypt key file missing"
[ -f "$PAYLOAD" ] || die "customization payload missing"
[ -n "$EXPECTED_IDENT" ] || die "expected fscrypt identifier is empty"
[ "$DIR" != "/data" ] && [ "$DIR" != "/" ] || die "invalid install directory"

# Create or rename the desktop account inside the mounted rootfs.  Only a
# SHA-512 crypt hash is ever present on the device; the plaintext password is
# never written to disk or passed in a command line.
configure_account() {
  [ "$ACCOUNT_ENABLED" = "1" ] || return 0
  [ -f "$ACCOUNT_ENV" ] || die "account setup requested but $ACCOUNT_ENV is missing"
  # shellcheck disable=SC1090
  . "$ACCOUNT_ENV"

  case "${PMOS_USERNAME:-}" in
    [a-z_]*) ;;
    *) die "invalid account name" ;;
  esac
  case "$PMOS_USERNAME" in
    *[!a-z0-9_-]*) die "invalid account name" ;;
  esac
  [ "${#PMOS_USERNAME}" -le 32 ] || die "account name is too long"
  case "$PMOS_USERNAME" in
    root|daemon|bin|sys|sync|games|man|lp|mail|news|uucp|proxy|www-data|backup|list|irc|gnats|nobody)
      die "account name is reserved" ;;
  esac
  case "${PMOS_PASSWORD_HASH:-}" in
    '$6$'*) ;;
    *) die "account password hash is not SHA-512 crypt" ;;
  esac
  case "$PMOS_PASSWORD_HASH" in
    *[!A-Za-z0-9./=$]*) die "account password hash contains invalid characters" ;;
  esac

  OLD=""
  if grep -q "^$PMOS_USERNAME:" "$MNT/etc/passwd"; then
    # The name already exists.  Accept it only if it is the desktop account
    # (UID >= 1000 and home /home/<name>).  Otherwise the requested name
    # collides with a system account and must not be renamed or re-keyed.
    ACCT_LINE=$(grep "^$PMOS_USERNAME:" "$MNT/etc/passwd" | head -n 1)
    ACCT_UID=$(printf '%s\n' "$ACCT_LINE" | cut -d: -f3)
    ACCT_HOME=$(printf '%s\n' "$ACCT_LINE" | cut -d: -f6)
    case "$ACCT_UID" in
      ''|*[!0-9]*) die "could not parse the existing account entry" ;;
    esac
    if [ "$ACCT_UID" -lt 1000 ] || [ "$ACCT_HOME" != "/home/$PMOS_USERNAME" ]; then
      die "account name '$PMOS_USERNAME' already exists and is not the desktop account; choose another name"
    fi
    OLD=""
  elif grep -q '^user:' "$MNT/etc/passwd"; then
    OLD=user
  else
    OLD=$(grep -E '^[^:]+:[^:]*:[0-9]+:[0-9]+:[^:]*:/home/' "$MNT/etc/passwd" \
      | head -n 1 | sed 's/:.*//')
  fi
  if [ -z "$OLD" ] && ! grep -q "^$PMOS_USERNAME:" "$MNT/etc/passwd"; then
    die "no usable desktop account found; rerun with --no-account"
  fi

  if [ -x "$MNT/bin/busybox" ]; then
    BB=/bin/busybox
  elif [ -x "$MNT/usr/bin/busybox" ]; then
    BB=/usr/bin/busybox
  else
    die "busybox is missing from the rootfs"
  fi

  log "configuring desktop account $PMOS_USERNAME"
  mkdir -p "$MNT/tmp"
  ACCOUNT_SCRIPT="$MNT/tmp/.pmos_account_setup.sh"
  {
    printf '#!/bin/sh\nset -e\numask 077\n'
    printf "BB='%s'\nOLD='%s'\nNEW='%s'\nHASH='%s'\n" \
      "$BB" "$OLD" "$PMOS_USERNAME" "$PMOS_PASSWORD_HASH"
    cat <<'ACCOUNT_SETUP_EOF'
if [ -n "$OLD" ]; then
  "$BB" awk -F: -v old="$OLD" -v new="$NEW" 'BEGIN{OFS=":"} {
    if ($1 == old) {
      $1 = new;
      if ($6 == "/home/" old) $6 = "/home/" new;
    }
    print;
  }' /etc/passwd > /tmp/.pmos_passwd
  "$BB" cat /tmp/.pmos_passwd > /etc/passwd
  "$BB" rm -f /tmp/.pmos_passwd

  "$BB" awk -F: -v old="$OLD" -v new="$NEW" 'BEGIN{OFS=":"} {
    if ($1 == old) $1 = new;
    if (NF >= 4 && $4 != "") {
      n = split($4, a, ",");
      out = "";
      for (i = 1; i <= n; i++) {
        if (a[i] == old) a[i] = new;
        out = out (i > 1 ? "," : "") a[i];
      }
      $4 = out;
    }
    print;
  }' /etc/group > /tmp/.pmos_group
  "$BB" cat /tmp/.pmos_group > /etc/group
  "$BB" rm -f /tmp/.pmos_group

  if [ -f /etc/gshadow ]; then
    "$BB" awk -F: -v old="$OLD" -v new="$NEW" 'BEGIN{OFS=":"} {
      if ($1 == old) $1 = new;
      for (f = 3; f <= 4; f++) {
        if (NF >= f && $f != "") {
          n = split($f, a, ",");
          out = "";
          for (i = 1; i <= n; i++) {
            if (a[i] == old) a[i] = new;
            out = out (i > 1 ? "," : "") a[i];
          }
          $f = out;
        }
      }
      print;
    }' /etc/gshadow > /tmp/.pmos_gshadow
    "$BB" cat /tmp/.pmos_gshadow > /etc/gshadow
    "$BB" rm -f /tmp/.pmos_gshadow
  fi

  if [ -f /etc/shadow ]; then
    "$BB" awk -F: -v old="$OLD" -v new="$NEW" 'BEGIN{OFS=":"} {
      if ($1 == old) $1 = new;
      print;
    }' /etc/shadow > /tmp/.pmos_shadow_rename
    "$BB" cat /tmp/.pmos_shadow_rename > /etc/shadow
    "$BB" rm -f /tmp/.pmos_shadow_rename
  fi

  for f in /etc/subuid /etc/subgid; do
    if [ -f "$f" ]; then
      "$BB" awk -F: -v old="$OLD" -v new="$NEW" 'BEGIN{OFS=":"} {
        if ($1 == old) $1 = new;
        print;
      }' "$f" > /tmp/.pmos_subid
      "$BB" cat /tmp/.pmos_subid > "$f"
      "$BB" rm -f /tmp/.pmos_subid
    fi
  done

  if [ -d "/home/$OLD" ]; then
    [ ! -e "/home/$NEW" ] || { echo "home already exists: /home/$NEW" >&2; exit 1; }
    "$BB" mv "/home/$OLD" "/home/$NEW"
  fi
  for d in /var/mail /var/spool/mail; do
    if [ -e "$d/$OLD" ]; then
      "$BB" mv "$d/$OLD" "$d/$NEW"
    fi
  done
  if [ -e "/var/lib/AccountsService/users/$OLD" ]; then
    "$BB" mv "/var/lib/AccountsService/users/$OLD" \
      "/var/lib/AccountsService/users/$NEW"
  fi
fi

"$BB" awk -F: -v u="$NEW" -v h="$HASH" 'BEGIN{OFS=":"} {
  if ($1 == u) $2 = h;
  print;
}' /etc/shadow > /tmp/.pmos_shadow
"$BB" grep -q "^$NEW:" /tmp/.pmos_shadow || {
  "$BB" rm -f /tmp/.pmos_shadow
  echo "account is missing from /etc/shadow" >&2
  exit 1
}
"$BB" awk -F: -v u="$NEW" -v h="$HASH" '
  $1 == u && $2 == h { ok = 1 }
  END { exit(ok ? 0 : 1) }
' /tmp/.pmos_shadow || {
  "$BB" rm -f /tmp/.pmos_shadow
  echo "password hash was not applied to /etc/shadow" >&2
  exit 1
}
"$BB" cat /tmp/.pmos_shadow > /etc/shadow
"$BB" rm -f /tmp/.pmos_shadow

if [ ! -d "/home/$NEW" ]; then
  "$BB" mkdir -p "/home/$NEW"
  U=$("$BB" id -u "$NEW")
  G=$("$BB" id -g "$NEW")
  "$BB" chown -R "$U:$G" "/home/$NEW"
fi
"$BB" id "$NEW" >/dev/null
ACCOUNT_SETUP_EOF
  } > "$ACCOUNT_SCRIPT"
  chmod 700 "$ACCOUNT_SCRIPT"
  chroot "$MNT" /bin/sh /tmp/.pmos_account_setup.sh
  rm -f "$ACCOUNT_SCRIPT" "$ACCOUNT_ENV"
  log "desktop account $PMOS_USERNAME is ready (password hash only)"
}

log "creating $DIR if needed"
[ -d "$DIR" ] || mkdir -p "$DIR"
chown 0:0 "$DIR" 2>/dev/null || true
chmod 755 "$DIR" 2>/dev/null || true

log "adding fscrypt key to the userdata keyring"
OUT=$("$HELPER" addkey2f "$DATA" "$KEYFILE" || true)
echo "$OUT"
IDENT=$(printf '%s\n' "$OUT" | sed -n 's/^identifier=\([0-9a-f][0-9a-f]*\)$/\1/p' | tail -n 1)
# -EEXIST is fine: the key is already in the filesystem keyring.  The
# expected identifier was derived from the same raw key on the host.
[ -n "$IDENT" ] || IDENT="$EXPECTED_IDENT"
[ "$IDENT" = "$EXPECTED_IDENT" ] || die "fscrypt identifier mismatch (expected $EXPECTED_IDENT, got $IDENT)"
KEY_ADDED=1
echo "PMOS_IDENTIFIER=$IDENT"

log "checking the encryption policy on $DIR"
POLICY=$("$HELPER" getpolicy2ident "$DIR" 2>/dev/null || true)
CURRENT=$(printf '%s\n' "$POLICY" | sed -n 's/^identifier=//p' | tail -n 1)
if [ -n "$CURRENT" ]; then
  [ "$CURRENT" = "$IDENT" ] || die "$DIR is encrypted with a different key; refusing to re-key it"
else
  case "$POLICY" in
    *unsupported*) die "$DIR has a non-v2 policy; refusing to re-key it" ;;
  esac
  if [ -n "$(ls -A "$DIR" 2>/dev/null)" ]; then
    die "$DIR is not encrypted and is not empty; refusing to re-key existing data"
  fi
  log "setting the v2 policy on $DIR"
  "$HELPER" setpolicy2 "$DIR" "$IDENT"
fi

log "applying sparse rootfs to $NEW"
rm -f "$NEW"
"$PACK" apply "$PACKFILE" "$NEW"
"$PACK" verify "$PACKFILE" "$NEW"
rm -f "$PACKFILE"

CURRENT_SIZE=$(stat -c %s "$NEW")
[ "$CURRENT_SIZE" -eq "$ROOTFS_SIZE_BYTES" ] || \
  die "applied image is $CURRENT_SIZE bytes, expected $ROOTFS_SIZE_BYTES"

log "labeling the image for a read-write loop mount"
chcon u:object_r:media_rw_data_file:s0 "$NEW" 2>/dev/null || \
  log "WARNING: could not relabel rootfs.img.new; continuing"

mkdir -p "$MNT"
log "loop-mounting $NEW"
LOOP=$(losetup -f --show "$NEW")
[ -n "$LOOP" ] || die "losetup failed"
mount -t ext4 "$LOOP" "$MNT" || die "ext4 mount failed"
MOUNTED=1

log "extracting the customization payload"
tar -xzf "$PAYLOAD" -C "$MNT"
rm -f "$PAYLOAD"

log "normalizing system directory ownership"
chown 0:0 "$MNT" "$MNT/etc" "$MNT/usr" "$MNT/var" "$MNT/root" 2>/dev/null || true
chmod 755 "$MNT" "$MNT/etc" "$MNT/usr" 2>/dev/null || true

# sudo refuses files in sudoers.d that are not root-owned.  The payload tar
# is built by the host user, so normalize this directory explicitly.
if [ -d "$MNT/etc/sudoers.d" ]; then
  chown -R 0:0 "$MNT/etc/sudoers.d" 2>/dev/null || true
  chmod 750 "$MNT/etc/sudoers.d" 2>/dev/null || true
  chmod 440 "$MNT/etc/sudoers.d/"* 2>/dev/null || true
fi

log "enforcing merged /usr symlink invariants"
if [ ! -L "$MNT/lib" ]; then
  if [ -d "$MNT/lib" ]; then
    cp -a "$MNT/lib/"* "$MNT/usr/lib/" 2>/dev/null || true
    rm -rf "$MNT/lib"
  fi
  ln -s usr/lib "$MNT/lib"
fi
[ -L "$MNT/bin" ] || ln -sf usr/bin "$MNT/bin"
[ -L "$MNT/sbin" ] || ln -sf usr/sbin "$MNT/sbin"
[ -e "$MNT/bin/bash" ] || ln -sf /bin/sh "$MNT/bin/bash"

log "configuring the desktop account"
configure_account

# The stock base image still carries the PinePhone Pro hostname.  Use the
# actual device name so SSH prompts, logs and avahi do not identify it as a
# different phone.
log "setting the device hostname"
printf 'lenovo-tab-plus-tb351fu\n' > "$MNT/etc/hostname"
if [ -f "$MNT/etc/hosts" ]; then
  sed -i 's/pine64-pinephonepro/lenovo-tab-plus-tb351fu/g' "$MNT/etc/hosts"
  grep -q '^127\.0\.1\.1[[:space:]]' "$MNT/etc/hosts" || \
    printf '127.0.1.1\tlenovo-tab-plus-tb351fu\n' >> "$MNT/etc/hosts"
else
  printf '127.0.0.1\tlenovo-tab-plus-tb351fu localhost\n' > "$MNT/etc/hosts"
  printf '::1\tlocalhost\n' >> "$MNT/etc/hosts"
fi

if [ "$WHOLE_MODE" = "1" ]; then
  log "adding the whole-device root marker"
  : > "$MNT/pmos_whole_device"
  chmod 644 "$MNT/pmos_whole_device"
  [ -f "$MNT/pmos_whole_device" ] || die "whole-device marker was not created"
fi

if [ -f "$MNT/etc/fstab" ]; then
  sed -i "s|^UUID=B171-53C0|# UUID=B171-53C0|" "$MNT/etc/fstab" || true
  sed -i "s|/boot|# /boot|" "$MNT/etc/fstab" || true
fi

log "validating userspace binaries"
[ -f "$MNT/lib/ld-musl-aarch64.so.1" ] || die "dynamic linker /lib/ld-musl-aarch64.so.1 is missing"
chroot "$MNT" /usr/lib/systemd/systemd --version | head -n 1
chroot "$MNT" /bin/busybox --help | head -n 1

log "compiling GSettings schemas"
if [ -x "$MNT/usr/bin/glib-compile-schemas" ]; then
  chroot "$MNT" /usr/bin/glib-compile-schemas /usr/share/glib-2.0/schemas || \
    log "WARNING: glib-compile-schemas failed"
else
  log "WARNING: glib-compile-schemas missing; Display settings panel may not register"
fi

log "building the built-in module index"
if [ -f "$MNT/usr/lib/modules/.pmos_kernel_release" ]; then
  KREL=$(cat "$MNT/usr/lib/modules/.pmos_kernel_release")
  log "kernel release: $KREL"
  chroot "$MNT" /usr/sbin/depmod -a "$KREL" >/dev/null 2>&1 || \
    log "WARNING: depmod failed"
fi

log "configuring nftables and zram services"
chroot "$MNT" /usr/bin/systemctl enable nftables.service >/dev/null 2>&1 || true
chroot "$MNT" /usr/bin/systemctl disable postmarketos-zram-swap.service >/dev/null 2>&1 || true

log "unmounting the new rootfs"
sync
umount "$MNT"
MOUNTED=0
# Android may detach the loop device as soon as the last mount reference
# goes away; that is fine and must not fail the install.
losetup -d "$LOOP" 2>/dev/null || true
LOOP=""
sync

if [ "$WHOLE_MODE" = "1" ]; then
  log "building the offline whole-device pack"
  rm -f "$WHOLEPACK"
  "$PACK" pack "$NEW" "$WHOLEPACK" || die "could not create whole-device pack"
  WHOLEPACK_SIZE=$(stat -c %s "$WHOLEPACK")
  echo "PMOS_WHOLE_PACK=$WHOLEPACK"
  echo "PMOS_WHOLE_PACK_BYTES=$WHOLEPACK_SIZE"
fi

log "swapping images inside $DIR"
if [ -e "$IMG" ]; then
  rm -f "$OLD"
  mv "$IMG" "$OLD"
fi
mv "$NEW" "$IMG"
sync
chcon u:object_r:system_data_file:s0 "$IMG" 2>/dev/null || true
if [ "$KEEP_OLD" != "1" ]; then
  rm -f "$OLD"
fi
INSTALL_OK=1

log "evicting the per-install fscrypt key from Android"
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

if head -c 4 "$IMG" >/dev/null 2>&1; then
  die "rootfs is still readable after key removal"
fi
log "rootfs is locked with the custom key (ENOKEY as expected)"
if [ "$WHOLE_MODE" = "1" ]; then
  KEEP_WHOLEPACK=1
  log "whole-device pack retained for host export"
fi
log "installation complete"
