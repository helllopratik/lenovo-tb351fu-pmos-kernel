#!/bin/sh
# pmosfb keeps the last scanout frame visible when the CRTC is disabled:
# pmosfb_crtc_atomic_disable() only stops vblank, and pmosfb_bl_update_status()
# ignores bl_power.  The panel therefore stays lit (often on the last frame)
# after an idle/lock DPMS off, which looks like a hung device.
#
# Workaround: force the backlight PWM to 0 while DRM DPMS is off.  Keep forcing
# it if userspace writes a new brightness while the panel is off (save that as
# the level to restore).  On DPMS on, restore the saved level only if nothing
# else (gsd-power) restored brightness first.

BL="/sys/class/backlight/lcd-backlight"
DPMS="/sys/class/drm/card0-DSI-1/dpms"
SAVED="/run/pmos-display-guard.brightness"
last=""

while :; do
    d=$(cat "$DPMS" 2>/dev/null || echo unknown)

    case "$d" in
        Off)
            cur=$(cat "$BL/brightness" 2>/dev/null || echo 0)
            if [ "${cur:-0}" -gt 0 ]; then
                echo "$cur" > "$SAVED" 2>/dev/null
                echo 0 > "$BL/brightness" 2>/dev/null
                [ "$last" != "Off" ] && echo "DPMS off: backlight $cur -> 0"
            fi
            ;;
        On)
            if [ "$last" != "On" ] && [ -f "$SAVED" ]; then
                cur=$(cat "$BL/brightness" 2>/dev/null || echo 0)
                if [ "${cur:-0}" -eq 0 ]; then
                    b=$(cat "$SAVED" 2>/dev/null || echo 0)
                    if [ "${b:-0}" -gt 0 ]; then
                        echo "$b" > "$BL/brightness" 2>/dev/null
                        echo "DPMS on: backlight 0 -> $b"
                    fi
                fi
                rm -f "$SAVED"
            fi
            ;;
    esac

    last="$d"
    sleep 1
done
