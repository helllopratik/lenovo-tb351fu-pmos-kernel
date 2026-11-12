#!/bin/sh
# Auto-recover the Phosh session from accidental VT escapes and the phoc
# layer-transaction assertion.
#
# ttyescape (hkdm VolumeDown+Power) can leave the machine on a text console:
# phoc pauses, hkdm starts buffyboard which grabs the touchscreen, and the
# desktop appears frozen.  This guard switches back to the Wayland VT after a
# grace period.  If phoc logged its layer assertion during the resume, restart
# only the phosh shell (apps and phoc survive).
#
# Hold: touch /run/pmos-session-guard.hold to stay on the console.

HOLD="/run/pmos-session-guard.hold"
STATE="/run/pmos-session-guard.state"
COOLDOWN="/run/pmos-session-guard.phosh-restart"
USER_NAME="user"
USER_ID="10000"
USER_RUNTIME="/run/user/$USER_ID"
GRACE=10
PHOSH_UNIT="mobi.phosh.Shell.service"

wayland_session() {
    for s in $(loginctl list-sessions --no-legend 2>/dev/null |
                   awk -v u="$USER_NAME" '$3 == u { print $1 }'); do
        [ "$(loginctl show-session "$s" -p Type --value 2>/dev/null)" = "wayland" ] && {
            echo "$s"
            return 0
        }
    done
    return 1
}

user_systemctl() {
    runuser -u "$USER_NAME" -- env XDG_RUNTIME_DIR="$USER_RUNTIME" \
        systemctl --user "$@" 2>/dev/null
}

restart_phosh() {
    now=$(date +%s)
    if [ -f "$COOLDOWN" ]; then
        last=$(cat "$COOLDOWN" 2>/dev/null || echo 0)
        [ $((now - last)) -lt 60 ] && return 0
    fi
    echo "$now" > "$COOLDOWN"
    echo "restarting $PHOSH_UNIT to recover layer surfaces"
    user_systemctl restart "$PHOSH_UNIT"
}

while :; do
    sid=$(wayland_session)
    if [ -z "$sid" ]; then
        sleep 2
        continue
    fi

    vt=$(loginctl show-session "$sid" -p VTNr --value 2>/dev/null)
    active=$(cat /sys/class/tty/tty0/active 2>/dev/null)

    if [ -n "$vt" ] && [ "$active" != "tty$vt" ]; then
        if [ -f "$HOLD" ]; then
            rm -f "$STATE"
        else
            now=$(date +%s)
            [ -f "$STATE" ] || echo "$now" > "$STATE"
            start=$(cat "$STATE" 2>/dev/null || echo "$now")
            if [ $((now - start)) -ge $GRACE ]; then
                echo "VT escape detected ($active), restoring tty$vt"
                loginctl activate "$sid" 2>/dev/null || chvt "$vt" 2>/dev/null
                rm -f "$STATE"
                sleep 3
                if runuser -u "$USER_NAME" -- env \
                       XDG_RUNTIME_DIR="$USER_RUNTIME" \
                       journalctl --user --since "-10s" --no-pager 2>/dev/null |
                       grep -q "phoc_layout_transaction_notify_layer_configured"; then
                    restart_phosh
                fi
            fi
        fi
    else
        rm -f "$STATE"
        if pgrep buffyboard >/dev/null 2>&1; then
            echo "killing buffyboard on the graphical VT"
            pkill buffyboard 2>/dev/null
        fi
        # The deploy installs the shell as /usr/local/libexec/phosh-patched, so
        # comm is "phosh-patched".  A bare `pidof phosh` never matches, and
        # BusyBox `pgrep -x` compares against argv[0], not comm -- both made
        # this guard re-run `systemctl start` every 2 s forever.
        if ! pidof phosh >/dev/null 2>&1 &&
           ! pidof phosh-patched >/dev/null 2>&1; then
            echo "$PHOSH_UNIT not running, starting it"
            user_systemctl start "$PHOSH_UNIT"
        fi
    fi

    sleep 2
done
