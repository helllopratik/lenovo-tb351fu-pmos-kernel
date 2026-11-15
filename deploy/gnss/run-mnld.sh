#!/bin/sh
# Host-side entry point: mount the runtime APEX, then run mnld in private
# mount/PID namespaces so none of the chroot mounts leak to the host.
set -eu
g=/opt/tb351fu-gps
mkdir -p /run/tb351fu-gps
if ! mountpoint -q "$g/runtime-apex"; then
  mkdir -p "$g/runtime-apex"
  mountpoint -q "$g/runtime-apex" || mount -o ro,loop "$g/runtime-apex.img" "$g/runtime-apex"
fi
exec unshare -m -p -f --mount-proc "$g/gps-runtime-inner.sh"
