#!/bin/sh
# Follow the ACCDET headphone jack and keep the PulseAudio card profile in sync.
# Manual profile choices are preserved until the next physical plug/unplug.
#
# Why the extra suspend/resume: the MT8186 AFE driver clears the DAPM-owned
# ADDA DL enable bit (AFE_ADDA_DL_SRC2_CON0 bit 0) in
# mtk_dai_adda_hw_params() whenever it rewrites that register.  If a PCM is
# configured while the ADDA backend is still powered (exactly what a profile
# switch during playback does), DAPM never re-runs the supply enable and the
# codec stays silent.  Closing and reopening the new Headphones PCM after the
# profile switch forces DAPM to power the ADDA path up again, restoring audio.

export XDG_RUNTIME_DIR="/run/user/$(id -u)"
CARD="alsa_card.platform-sound"
HP_PROFILE="HiFi (Headphones)"
SPK_PROFILE="HiFi (Speaker)"
HP_SINK="alsa_output.platform-sound.HiFi__Headphones__sink"
SPK_SINK="alsa_output.platform-sound.HiFi__Speaker__sink"

card_profile() {
    pactl list cards 2>/dev/null |
        sed -n 's/.*Active Profile: //p' | head -n 1
}

sink_present() {
    pactl list sinks short 2>/dev/null |
        awk -v n="$1" '$2 == n { found = 1 } END { exit !found }'
}

# `After=pulseaudio.service` only orders the fork, not socket readiness.
# Switching the card profile while PulseAudio is still adding the card can
# leave the profile active with no sink; wait for both before touching it.
wait_card() {
    i=0
    while [ "$i" -lt 50 ]; do
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
# once so module-alsa-card recreates it.
ensure_sink() {
    i=0
    while [ "$i" -lt 25 ]; do
        sink_present "$2" && return 0
        sleep 0.2
        i=$((i + 1))
    done
    pactl set-card-profile "$CARD" off 2>/dev/null
    sleep 0.3
    pactl set-card-profile "$CARD" "$1" 2>/dev/null
}

find_hp_sink() {
    i=0
    while [ "$i" -lt 10 ]; do
        id=$(pactl list sinks short 2>/dev/null |
            awk -v n="$HP_SINK" '$2 == n { print $1; exit }')
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

last=""

while :; do
    if ! wait_card; then
        # PulseAudio is down/restarting; re-evaluate the jack state when it
        # is back even if the physical state has not changed.
        last=""
        sleep 2
        continue
    fi

    numid=$(amixer -c 0 controls 2>/dev/null |
        sed -n "s/^numid=\([0-9]*\),iface=CARD,name='Headphone Jack'$/\1/p" |
        head -n 1)

    if [ -n "$numid" ]; then
        val=$(amixer -c 0 cget numid="$numid" 2>/dev/null |
            sed -n 's/.*values=\(on\|off\).*/\1/p' |
            tail -n 1)

        if [ -n "$val" ] && [ "$val" != "$last" ]; then
            case "$val" in
                on)
                    if [ "$(card_profile)" != "$HP_PROFILE" ] ||
                        ! sink_present "$HP_SINK"; then
                        pactl set-card-profile "$CARD" "$HP_PROFILE" 2>/dev/null
                        ensure_sink "$HP_PROFILE" "$HP_SINK"
                    fi
                    sleep 0.5
                    reopen_hp_pcm
                    ;;
                off)
                    if [ "$(card_profile)" != "$SPK_PROFILE" ] ||
                        ! sink_present "$SPK_SINK"; then
                        pactl set-card-profile "$CARD" "$SPK_PROFILE" 2>/dev/null
                        ensure_sink "$SPK_PROFILE" "$SPK_SINK"
                    fi
                    ;;
            esac
            last="$val"
            echo "jack=$val -> $(pactl list cards 2>/dev/null |
                sed -n 's/.*Active Profile: //p' | head -n 1)"
        fi
    fi

    sleep 1
done
