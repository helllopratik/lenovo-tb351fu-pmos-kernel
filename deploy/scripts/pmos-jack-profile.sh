#!/bin/sh
# Follow the ACCDET headphone jack and keep the PipeWire-Pulse card profile in
# sync.  This runs as a systemd *user* service so XDG_RUNTIME_DIR is the
# logged-in user's runtime directory; running it as root cannot reach the
# session sound server.
# Manual profile choices are preserved until the next physical plug/unplug.
#
# Why the extra suspend/resume: the MT8186 AFE driver clears the DAPM-owned
# ADDA DL enable bit (AFE_ADDA_DL_SRC2_CON0 bit 0) in
# mtk_dai_adda_hw_params() whenever it rewrites that register.  If a PCM is
# configured while the ADDA backend is still powered (exactly what a profile
# switch during playback does), DAPM never re-runs the supply enable and the
# codec stays silent.  Closing and reopening the new Headphones PCM after the
# profile switch forces DAPM to power the ADDA path up again, restoring audio.
#
# Profile names are discovered from the running card instead of being
# hard-coded: the mt8186-mt6366 machine driver exposes only combined profiles
# ("HiFi (Mic, Speaker)", "HiFi (Mic, Headphones)", ...), so the old
# "HiFi (Speaker)"/"HiFi (Headphones)" names never matched and the card was
# left on a profile whose port was unavailable.  That is what produced the
# "Dummy Output" + no-sound state at boot.

export XDG_RUNTIME_DIR="/run/user/$(id -u)"
CARD="alsa_card.platform-sound"
HP_SINK="alsa_output.platform-sound.HiFi__Headphones__sink"
SPK_SINK="alsa_output.platform-sound.HiFi__Speaker__sink"

card_profile() {
    pactl list cards 2>/dev/null |
        sed -n 's/.*Active Profile: //p' | head -n 1
}

# module-alsa-card appends ".N" to a node name when the previous node with
# that name is still being torn down (profile switches, WirePlumber restarts).
# Treat "<expected>.2" as the same sink; otherwise this watcher concludes the
# sink is missing and resets the card in a loop, which kills playback.
sink_present() {
    pactl list sinks short 2>/dev/null |
        awk -v n="$1" '
            $2 == n { found = 1 }
            index($2, n ".") == 1 {
                suffix = substr($2, length(n) + 2)
                if (suffix ~ /^[0-9]+$/) found = 1
            }
            END { exit !found }'
}

# Print the live sink name for $1, including any numeric suffix.  Prefer the
# current default sink when it matches, otherwise the newest (highest id).
sink_name() {
    default=$(pactl get-default-sink 2>/dev/null)
    pactl list sinks short 2>/dev/null |
        awk -v n="$1" -v d="$default" '
            function matches(x) {
                if (x == n) return 1
                if (index(x, n ".") == 1) {
                    s = substr(x, length(n) + 2)
                    if (s ~ /^[0-9]+$/) return 1
                }
                return 0
            }
            matches($2) {
                if ($2 == d) { best = $2; found = 1; exit }
                if (!found || ($1 + 0) > bestid) {
                    best = $2; bestid = $1 + 0; found = 1
                }
            }
            END { if (found) print best; else exit 1 }'
}

sink_id() {
    name=$(sink_name "$1") || return 1
    [ -n "$name" ] || return 1
    pactl list sinks short 2>/dev/null |
        awk -v n="$name" '$2 == n { print $1; exit }'
}

# Follow a profile switch only when the default would otherwise stay on the
# old speaker/headphone sink (or the dummy fallback).  Leave a deliberate
# Bluetooth/HDMI default alone.
ensure_default() {
    actual=$(sink_name "$1")
    [ -n "$actual" ] || return 0
    cur=$(pactl get-default-sink 2>/dev/null)
    case "$cur" in
        "$actual") return 0 ;;
        ""|auto_null*) ;;
        "$HP_SINK"|"$HP_SINK".[0-9]*|"$SPK_SINK"|"$SPK_SINK".[0-9]*) ;;
        *) return 0 ;;
    esac
    pactl set-default-sink "$actual" 2>/dev/null
}

# Highest-priority *available* profile whose name contains $1 (Speaker or
# Headphones).  Empty when no such profile exists.
pick_profile() {
    kw="$1"
    pactl list cards 2>/dev/null | awk -v kw="$kw" '
        /priority:/ && /available: yes/ {
            line=$0
            sub(/^[ \t]+/, "", line)
            name=line; sub(/:.*/, "", name)
            if (index(name, kw) == 0) next
            p=line; sub(/.*priority: /, "", p); sub(/,.*/, "", p)
            if (p+0 > best) { best=p+0; bestn=name }
        }
        END { if (bestn != "") print bestn }
    '
}

# Ordering against pipewire-pulse.service only orders the fork, not socket
# readiness.  Wait for both the daemon and the card before touching it.
wait_card() {
    i=0
    while [ "$i" -lt 150 ]; do
        if pactl info >/dev/null 2>&1 &&
            pactl list cards short 2>/dev/null | grep -q "$CARD"; then
            return 0
        fi
        sleep 0.2
        i=$((i + 1))
    done
    return 1
}

# If the expected sink is missing after a profile change, bounce the profile
# so module-alsa-card recreates the PCM/sink.
ensure_sink() {
    sink_present "$2" && return 0
    pactl set-card-profile "$CARD" off 2>/dev/null
    sleep 0.3
    pactl set-card-profile "$CARD" "$1" 2>/dev/null

    i=0
    while [ "$i" -lt 25 ]; do
        sink_present "$2" && return 0
        sleep 0.2
        i=$((i + 1))
    done
    return 1
}

find_hp_sink() {
    i=0
    while [ "$i" -lt 10 ]; do
        id=$(sink_id "$HP_SINK")
        if [ -n "$id" ]; then
            echo "$id"
            return 0
        fi
        sleep 0.2
        i=$((i + 1))
    done
    return 1
}

reopen_hp_pcm() {
    id=$(find_hp_sink) || return 0
    pactl suspend-sink "$id" 1 2>/dev/null
    sleep 0.3
    pactl suspend-sink "$id" 0 2>/dev/null
}

# Both profiles are re-discovered on every pass: if PulseAudio reloads the
# card the profile names can change under us.
HP_PROFILE=""
SPK_PROFILE=""
last=""

while :; do
    if ! wait_card; then
        # PulseAudio is down/restarting; re-evaluate the jack state when it
        # is back even if the physical state has not changed.
        last=""
        HP_PROFILE=""
        SPK_PROFILE=""
        sleep 2
        continue
    fi

    numid=$(amixer -c 0 controls 2>/dev/null |
        sed -n "s/^numid=\([0-9]*\),iface=CARD,name='Headset Jack'$/\1/p" |
        head -n 1)

    val=""
    if [ -n "$numid" ]; then
        val=$(amixer -c 0 cget numid="$numid" 2>/dev/null |
            sed -n 's/.*values=\(on\|off\).*/\1/p' |
            tail -n 1)
    fi
    [ -n "$val" ] || val="off"

    case "$val" in
        on)  want="$HP_PROFILE";  sink="$HP_SINK";  kw="Headphones" ;;
        *)   want="$SPK_PROFILE"; sink="$SPK_SINK"; kw="Speaker" ;;
    esac

    if [ -z "$want" ]; then
        want=$(pick_profile "$kw")
        case "$val" in
            on)  HP_PROFILE="$want" ;;
            *)   SPK_PROFILE="$want" ;;
        esac
    fi

    if [ -n "$want" ] &&
        { [ "$last" != "$val" ] || ! sink_present "$sink"; }; then
        if [ "$(card_profile)" != "$want" ]; then
            pactl set-card-profile "$CARD" "$want" 2>/dev/null
        fi
        if ensure_sink "$want" "$sink"; then
            ensure_default "$sink"
            actual=$(sink_name "$sink")
            last="$val"
            echo "jack=$val -> $want (${actual:-$sink})"
        else
            echo "jack=$val -> $want: sink $sink still missing" >&2
        fi
        case "$val" in
            on) sleep 0.5; reopen_hp_pcm ;;
        esac
    fi

    sleep 1
done
