#!/bin/sh
# Build a phosh 0.55.0 binary with the lock-screen IM-module crash fix and
# the portrait-180 prelock rotation fix.
# Prerequisites (Alpine/pmOS): meson, ninja, and the phosh makedepends
# (gtk+3.0-dev, libhandy1-dev, gcr-dev, gnome-desktop-dev, feedbackd-dev,
#  gnome-bluetooth-dev, libgudev-dev, libsecret-dev, libsoup3-dev,
#  polkit-elogind-dev, networkmanager-dev, modemmanager-dev, upower-dev,
#  evince-dev, appstream-dev, evolution-data-server-dev, qrcodegen-dev,
#  libadwaita-dev, callaudiod-dev, linux-pam-dev, pulseaudio-dev, etc.).
set -e

cd "$(dirname "$0")"
if [ ! -d phosh-0.55.0 ]; then
    tar -xf phosh-0.55.0.tar.xz
fi
if ! grep -q had_focus phosh-0.55.0/src/lockscreen.c; then
    patch -d phosh-0.55.0 -p1 < lockscreen-im-module-crash.patch
fi
if ! grep -q "No prelock transform saved" phosh-0.55.0/src/rotation-manager.c; then
    patch -d phosh-0.55.0 -p1 < phosh-0.55.0-prelock-rotation.patch
fi

cd phosh-0.55.0
meson setup build \
    --prefix=/usr \
    --libdir=lib \
    --buildtype=release \
    -Dphoc_tests=disabled \
    -Dman=false \
    -Dtests=false \
    -Dintrospection=false \
    -Dbindings-lib=false \
    -Dsearchd=false
ninja -C build

echo "Built: $(pwd)/build/src/phosh"
