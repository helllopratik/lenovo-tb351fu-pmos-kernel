#!/bin/sh
# Run inside `unshare -m -p -f --mount-proc`.  Build a private Bionic root
# from the payload and exec the vendor GPS daemon.
set -eu
g=/opt/tb351fu-gps
root=/run/tb351fu-gps/root
log=${GPS_LOG:-/run/tb351fu-gps/mnld.log}
persist=/var/lib/tb351fu-gps/data

mkdir -p "$root/system" "$root/vendor" "$root/apex/com.android.runtime" \
  "$root/proc" "$root/dev" "$root/data" "$root/linkerconfig" \
  "$root/mnt/vendor/nvdata" "$root/mnt/vendor/nvcfg" "$root/sys" "$root/tmp" \
  "$root/gps-shim"
mkdir -p "$persist/vendor/gps" "$persist/misc/gps"

bind_ro() { mount --bind "$1" "$2"; mount -o remount,bind,ro "$2"; }
bind_ro "$g/system" "$root/system"
bind_ro "$g/vendor" "$root/vendor"
bind_ro "$g/runtime-apex" "$root/apex/com.android.runtime"
: > "$root/linkerconfig/ld.config.txt"
bind_ro "$g/ld.config.txt" "$root/linkerconfig/ld.config.txt"
bind_ro "$g/shim" "$root/gps-shim"
[ -d /mnt/vendor/nvdata ] && bind_ro /mnt/vendor/nvdata "$root/mnt/vendor/nvdata"
[ -d /mnt/vendor/nvcfg ] && bind_ro /mnt/vendor/nvcfg "$root/mnt/vendor/nvcfg"
mount --bind "$persist" "$root/data"
mount -t proc -o nosuid,nodev proc "$root/proc"
mount -t sysfs -o ro,nosuid,nodev,noexec sysfs "$root/sys"
mount -t tmpfs -o nosuid,mode=0755 tmpfs "$root/dev"
mount -t tmpfs -o nosuid,nodev tmpfs "$root/tmp"
mkdir -p "$root/dev/socket"
for d in stpgps stpgps2 gps_pwr gps_emi fw_log_gps null zero urandom random; do
  if [ -c "/dev/$d" ]; then
    : > "$root/dev/$d"; mount --bind "/dev/$d" "$root/dev/$d"
  else
    echo "WARN missing /dev/$d" >&2
  fi
done
"$g/logd-sink.py" "$root/dev/socket/logdw" "$log" &
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -S "$root/dev/socket/logdw" ] && break; sleep 0.1; done
chroot_bin=$(command -v chroot)
exec env -i LD_PRELOAD=/gps-shim/lib64/libgpsprops.so ANDROID_ROOT=/system \
  ANDROID_DATA=/data PATH=/system/bin:/vendor/bin \
  LD_CONFIG_FILE=/linkerconfig/ld.config.txt \
  "$chroot_bin" "$root" /vendor/bin/mnld 2>>"$log"
