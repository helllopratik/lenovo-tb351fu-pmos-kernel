#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# One-time migration for images built from the pmOS base rootfs that selects
# the PulseAudio audio backend on a Phosh/PipeWire desktop.
#
# The stock base image ships
#   postmarketos-base-ui-audio-backend-pulseaudio
# which depends on pulseaudio-wireplumber.  That package installs
#   /usr/share/wireplumber/wireplumber.conf.d/51-pulseaudio.conf
# disabling WirePlumber's hardware.audio and hardware.bluetooth profiles.
# Phosh runs PipeWire but never starts PulseAudio, so the result is a
# completely silent system.  Replacing the world entry with
#   postmarketos-base-ui-audio-backend-pipewire
# removes the override and installs pipewire-pulse, pipewire-alsa and the
# Bluetooth SPA plugin.
#
# The migration is intentionally guarded:
#   * it only acts when the exact PulseAudio backend entry is in /etc/apk/world
#   * it is idempotent and serialized with flock
#   * if the package transaction cannot reach the repositories it restores
#     the world file and lets the timer retry later
#   * it tolerates the known postmarketos-mkinitfs trigger failure on this
#     device (the custom boot chain has no kernel in /boot); success is based
#     on the installed package set, not solely on apk's exit status
set -u

WORLD=/etc/apk/world
PULSE_BACKEND=postmarketos-base-ui-audio-backend-pulseaudio
PIPEWIRE_BACKEND=postmarketos-base-ui-audio-backend-pipewire
STATE_DIR=/var/lib/pmos
MARKER=$STATE_DIR/audio-backend-migrated
LOG=/var/log/pmos-audio-backend-migrate.log

log() {
    echo "pmos-audio-migrate: $*"
}

# The old jack-profile unit ran as root, so it could never connect to a
# user session's sound server.  Replace it with the per-user unit from the
# installer payload.  This is independent of the package migration and also
# runs on systems whose world entry was already fixed by hand.
fix_jack_service() {
    if [ -f /etc/systemd/system/pmos-jack-profile.service ]; then
        log "disabling the old system-wide jack profile service"
        systemctl disable --now pmos-jack-profile.service >/dev/null 2>&1 || true
        rm -f /etc/systemd/system/pmos-jack-profile.service
        rm -f /etc/systemd/system/multi-user.target.wants/pmos-jack-profile.service
        systemctl daemon-reload >/dev/null 2>&1 || true
    fi

    if [ -f /usr/lib/systemd/user/pmos-jack-profile.service ]; then
        mkdir -p /etc/systemd/user/default.target.wants
        ln -sf /usr/lib/systemd/user/pmos-jack-profile.service \
            /etc/systemd/user/default.target.wants/pmos-jack-profile.service
    fi
}

mark_done() {
    fix_jack_service
    mkdir -p "$STATE_DIR"
    : > "$MARKER"
}

have_pipewire_packages() {
    apk info -e "$PIPEWIRE_BACKEND" >/dev/null 2>&1 &&
        apk info -e pipewire-pulse >/dev/null 2>&1
}

if [ -f "$MARKER" ]; then
    exit 0
fi

if [ ! -f "$WORLD" ]; then
    log "no $WORLD; nothing to migrate"
    mark_done
    exit 0
fi

if ! grep -qx "$PULSE_BACKEND" "$WORLD"; then
    log "PulseAudio backend is not selected; nothing to migrate"
    mark_done
    exit 0
fi

# Serialize timer and boot starts.
exec 9>/run/pmos-audio-backend-migrate.lock
if ! flock -n 9; then
    log "another migration run is active"
    exit 0
fi

if have_pipewire_packages; then
    log "packages are already migrated; repairing the world entry"
    sed -i "s/^$PULSE_BACKEND$/$PIPEWIRE_BACKEND/" "$WORLD"
    mark_done
    exit 0
fi

BACKUP=$(mktemp "${TMPDIR:-/tmp}/pmos-world.XXXXXX") || exit 1
trap 'rm -f "$BACKUP"' EXIT HUP INT TERM
cp "$WORLD" "$BACKUP"
sed -i "s/^$PULSE_BACKEND$/$PIPEWIRE_BACKEND/" "$WORLD"

log "installing the PipeWire audio backend (log: $LOG)"
if apk add --interactive=no --progress=no "$PIPEWIRE_BACKEND" \
        >>"$LOG" 2>&1; then
    rc=0
else
    rc=$?
fi

if ! have_pipewire_packages; then
    log "package transaction failed (apk rc=$rc); restoring $WORLD"
    cp "$BACKUP" "$WORLD"
    exit "$rc"
fi

# The mkinitfs trigger exits 1 on this device because the custom boot chain
# has no vmlinuz in /boot.  The package set is what matters here.
if [ "$rc" -ne 0 ]; then
    log "apk reported rc=$rc, but the PipeWire packages are installed; continuing"
fi
rm -f "$BACKUP"
trap - EXIT HUP INT TERM

# Defensive: remove the WirePlumber override if it survived the transaction.
if [ -f /usr/share/wireplumber/wireplumber.conf.d/51-pulseaudio.conf ]; then
    log "removing stale WirePlumber PulseAudio override"
    rm -f /usr/share/wireplumber/wireplumber.conf.d/51-pulseaudio.conf
fi

systemctl daemon-reload >/dev/null 2>&1 || true

# Refresh any logged-in user's audio stack so the migration takes effect
# without requiring a reboot.  A failure here is not fatal; the packages are
# already in place for the next login.
for runtime in /run/user/[0-9]*; do
    [ -S "$runtime/bus" ] || continue
    uid=${runtime##*/}
    user=$(getent passwd "$uid" 2>/dev/null | cut -d: -f1)
    [ -n "$user" ] || continue
    log "restarting audio services for $user"
    runuser -u "$user" -- env XDG_RUNTIME_DIR="$runtime" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus" \
        systemctl --user daemon-reload >/dev/null 2>&1 || true
    runuser -u "$user" -- env XDG_RUNTIME_DIR="$runtime" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus" \
        systemctl --user restart wireplumber.service pipewire.service \
        >/dev/null 2>&1 || true
    # pipewire-pulse unlinks its socket path on shutdown while systemd keeps
    # the listening fd.  Restarting only pipewire-pulse.service therefore
    # leaves $XDG_RUNTIME_DIR/pulse/native missing and every PulseAudio
    # client fails with "Connection refused".  Restart the socket (which
    # also stops the service), then start the service again.
    runuser -u "$user" -- env XDG_RUNTIME_DIR="$runtime" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus" \
        systemctl --user restart pipewire-pulse.socket >/dev/null 2>&1 || true
    runuser -u "$user" -- env XDG_RUNTIME_DIR="$runtime" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus" \
        systemctl --user start pipewire-pulse.service >/dev/null 2>&1 || true
    runuser -u "$user" -- env XDG_RUNTIME_DIR="$runtime" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus" \
        systemctl --user start pmos-jack-profile.service >/dev/null 2>&1 || true
done

mark_done
log "audio backend migration complete"
