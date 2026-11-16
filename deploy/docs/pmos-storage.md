# Internal storage and Android partitions

`pmos-storage.service` exposes the tablet's internal UFS storage to the pmOS
desktop without ever writing to Android's system/vendor/boot partitions.

| Path | Mode | Source | Purpose |
|------|------|--------|---------|
| `/media/internal` | rw | bind of `/data_host/media/0` | Shared storage (Downloads, Pictures, ...) |
| `/media/android-data` | ro | bind of `/data_host` | Browse Android `/data` |
| `/media/android-parts/<name>` | ro | loop over super extents | `system_a`, `vendor_a`, `product_a`, ... |
| `/media/android-parts/<label>` | ro | raw UFS partitions | `metadata`, `persist`, `nvdata`, ... |

SD cards and USB storage are **not** handled here. udisks2/Nautilus mount them
under `/run/media/user/` as usual.

## Rootfs protection

pmOS boots from `/data_host/pmos/rootfs.img`, which lives inside Android's
userdata filesystem.  Before any part of `/data_host` is exposed, the service
does a read-only self-bind of `/data_host/pmos`:

```
mount --bind /data_host/pmos /data_host/pmos
mount -o remount,bind,ro /data_host/pmos
```

This is deliberate and is not undone by `pmos-storage-umount.sh` while pmOS is
running: making that path writable again would let a desktop process corrupt
the live root filesystem image.  systemd removes the bind during shutdown.

Android `/data` itself is only ever reachable through a read-only bind.
`userdata` and `super` are never mounted raw (the super partitions are mapped
individually through liblp and loop devices with `--offset/--sizelimit`).

## Group IDs

Android stores files with fixed numeric GIDs.  `pmos-storage-groups.sh` adds
the relevant ones to `/etc/group` and makes `user` a member:

| GID | Name |
|-----|------|
| 1000 | `system` |
| 1015 | `sdcard_rw` |
| 1023 | `media_rw` |
| 1028 | `sdcard_r` |
| 1078 | `ext_data_rw` |
| 1079 | `ext_obb_rw` |

The service runs the group update before mounting.  A user session that was
already running when the groups were first added may need one logout/login
(`id` must list the new GIDs).

## Android logical partitions

`pmos-supermap.py` parses Android's liblp metadata read-only and prints:

```
name|offset_bytes|size_bytes|group
```

It verifies both the geometry and metadata checksums, chooses the populated
A/B metadata slot, and refuses multi-extent or non-linear partitions (none
exist on this device).  Validate it against Android's own view with:

```
python3 /usr/local/bin/pmos-supermap.py --print-device
python3 /usr/local/bin/pmos-supermap.py --verbose
```

On slot A's super partition this yields the same extents as Android's
`lpdump`:

```
system_a        offset 1048576     size 2147483648
system_ext_a    offset 2148532224  size 1073741824
product_a       offset 3222274048  size 3221225472
vendor_a        offset 6443499520  size 1073741824
vendor_dlkm_a   offset 7517241344  size 104857600
system_dlkm_a   offset 7622098944  size 104857600
odm_dlkm_a      offset 7726956544  size 104857600
```

## Troubleshooting

```
systemctl status pmos-storage
journalctl -u pmos-storage -b
cat /run/pmos-storage.log
findmnt -R /media
pmos-storage-umount.sh && pmos-storage-mount.sh     # as root, manual retry
```

If a partition is missing, check that udev created its block node:

```
ls -l /dev/disk/by-partlabel/ | head -40
blkid -p /dev/disk/by-partlabel/super
```

The udev rule `99-pmos-android-ufs.rules` sets `UDISKS_IGNORE=1` for every
LUN of the UFS host controller (`platform-11270000.ufshci*`), with an `sdc`
fallback when `ID_PATH` is unavailable.  udisks2 therefore cannot race the
service or auto-mount userdata.  SD cards (`mmcblk0`) and USB disks keep normal
auto-mount behaviour.
