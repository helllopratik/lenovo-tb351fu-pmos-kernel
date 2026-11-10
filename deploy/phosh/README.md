# Patched phosh for the Lenovo TB351FU

This directory carries the two userspace patches needed for the TB351FU
postmarketOS/Phosh port. They are applied to an unmodified phosh 0.55.0
release tarball and built on-device; the system phosh package is left alone.

## Patches

| File | Purpose |
| --- | --- |
| `lockscreen-im-module-crash.patch` | Avoids a stack overflow when the lock-screen OSK switches the PIN entry's IM module. |
| `phosh-0.55.0-prelock-rotation.patch` | Keeps the compositor's 180° portrait transform when the shell starts before a monitor is known, instead of resetting to normal at unlock. |

The rotation patch changes `src/rotation-manager.c`:

* `phosh_rotation_manager_init()` initializes `prelock_transform` to `-1`
  (`0` is a valid `PHOSH_MONITOR_TRANSFORM_NORMAL`, so it cannot be used as
  the "nothing saved" sentinel).
* On unlock, when `prelock_transform` is still `-1`, phosh logs
  `No prelock transform saved, keeping current transform` and leaves the
  compositor transform untouched.

## Build

On the tablet (Alpine/pmOS; see the package list in `build.sh`):

```sh
# Place phosh-0.55.0.tar.xz next to build.sh, or let the script use the
# directory already present.
./build.sh
```

The resulting binary is `phosh-0.55.0/build/src/phosh`.

## Install

Install the patched shell outside `/usr` so package upgrades cannot overwrite
it:

```sh
sudo install -m 0755 phosh-0.55.0/build/src/phosh /usr/local/libexec/phosh-patched
```

Select it with a user systemd drop-in at
`~/.config/systemd/user/mobi.phosh.Shell.service.d/40-patched-binary.conf`
(or the equivalent `ExecStart=` override used on the device), then:

```sh
systemctl --user daemon-reload
systemctl --user restart mobi.phosh.Shell.service
```

`/etc/phosh/phoc.ini` must keep the portrait output configuration:

```ini
[output:DSI-1]
scale = 2
rotate = 180
```

The tracked copy is `../configs/phoc.ini`.

## Verification

```sh
export XDG_RUNTIME_DIR=/run/user/10000
export WAYLAND_DISPLAY=wayland-0
wlr-randr | grep -A2 DSI-1     # Transform: 180 after login/unlock
pgrep -a phosh-patched
```

A real reboot must show the lock screen at 180° and keep `Transform: 180`
after the PIN unlock. On 2026-11-10 this passed on slot B with boot ID
`553c04ad-e265-4658-a0e0-3018951a5293`; the previously observed reset
modeset after `gkr-pam: unlocked login keyring` no longer occurs.

## Rollback

Restore the stock shell by removing the drop-in and restarting
`mobi.phosh.Shell.service`. The pre-rotation patched binary used during
development is kept on the device at
`/root/phosh-patched.pre-rotation-20261110`.

The 180° volume-rocker swap is a per-user `gsd-media-keys` dconf setting, not
part of this build; see the porting guide (Issue 59).
