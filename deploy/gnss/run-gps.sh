#!/bin/sh
# Run the GNSS HAL bridge and vendor mnld as one systemd unit.
# The bridge must own mtk_mnl2hal before mnld starts; if either dies, tear
# down both (mnld treats a missing HAL socket as a stop request).
#
# With systemd socket activation, fd 3 is the listening NMEA socket and the
# service only starts when a consumer (GeoClue) connects.  The bridge exits
# after 60 s without clients; this wrapper then exits and systemd's
# KillMode=control-group stops the remaining mnld/unshare processes.
set -u
g=/opt/tb351fu-gps

if [ "${LISTEN_FDS:-0}" -ge 1 ] 2>/dev/null; then
  GNSS_SOCKET_ACTIVATED=1
  export GNSS_SOCKET_ACTIVATED
fi

# kill -0 also succeeds for unreaped zombies; use /proc state instead.
alive() {
  [ -r "/proc/$1/status" ] || return 1
  ! grep -q '^State:[[:space:]]*Z' "/proc/$1/status" 2>/dev/null
}

python3 -u "$g/tablet-gnss-bridge.py" &
bridge=$!

# Wait for the abstract HAL socket to appear (mnld connects at startup).
i=0
while [ "$i" -lt 50 ]; do
  grep -q "@mtk_mnl2hal" /proc/net/unix 2>/dev/null && break
  alive "$bridge" || { echo "bridge exited early" >&2; exit 1; }
  i=$((i + 1))
  sleep 0.1
done

"$g/run-mnld.sh" &
mnld=$!

# Never use a bare `wait` here: mnld is PID 1 in a private PID namespace and
# may not be reapable from this shell.  systemd handles the whole cgroup.
trap 'kill -TERM "$bridge" 2>/dev/null; exit 0' TERM INT

while alive "$bridge" && alive "$mnld"; do
  sleep 1
done

# On the idle path the bridge has already sent the HAL stop request.  Give
# mnld a moment to act on it, then let systemd kill whatever remains.
kill -TERM "$bridge" 2>/dev/null
n=0
while alive "$mnld" && [ "$n" -lt 5 ]; do
  n=$((n + 1))
  sleep 1
done
exit 0
