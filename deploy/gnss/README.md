# TB351FU GNSS deployment (vendor `mnld` + GeoClue)

**Status:** working on slot B as of 2026-11-15. Real NMEA fixes have been
received and GeoClue reports GPS-level accuracy. GPS is **not** started at
boot: systemd socket activation starts it only when a location client (GeoClue
NMEA source) connects, and it powers down again 60 s after the last client
disconnects.

This directory contains the complete userspace deployment: the vendor-daemon
compatibility chroot, the HAL/NMEA bridge, the systemd units, the GeoClue
configuration, and the stock kernel modules.

## How it works

```
 location app (GNOME Maps, Firefox, ...)
        |
        v
 GeoClue 2.8  [network-nmea]  nmea-socket=/run/tablet-gps/nmea.sock
        |  (only when a client asks for accuracy 8 / GPS)
        v
 tb351fu-gps.socket  --accept-->  tb351fu-gps.service
                                        |
                              run-gps.sh (wrapper)
                                 |            |
                     tablet-gnss-bridge.py   run-mnld.sh
                                 |            |
                     abstract AF_UNIX      unshare -m -p -f
                     mtk_hal2mnl /         gps-runtime-inner.sh
                     mtk_mnl2hal                |
                                 |         chroot /run/tb351fu-gps/root
                                 |         /vendor/bin/mnld
                                 v
                     /run/tablet-gps/nmea      (pty, all sentences)
                     /run/tablet-gps/nmea.sock (GeoClue, valid fixes only)
```

`mnld` is the stock MediaTek Android GNSS daemon
(`MTK_MNLD_14_1.00`, `MNL_VER_26010602ALPS05_8.20_00`). It is run inside a
private mount and PID namespace with a minimal Bionic root assembled from the
slot A Android 17 partitions. It talks the vendor HAL protocol over two
abstract `AF_UNIX` datagram sockets:

| Socket | Direction | Purpose |
|---|---|---|
| `mtk_hal2mnl` | bridge -> `mnld` | commands: int32 version `1`, command, arguments |
| `mtk_mnl2hal` | `mnld` -> bridge | replies and NMEA; message type `104` carries NMEA text |

Known commands: `101` init, `102` start (float `1.4` argument), `103` stop,
`105` time injection (wall clock ms, boot-time ms, interval ms). Type `308` is
the `mnld` version reply.

The bridge writes every standard sentence to the raw pty
`/run/tablet-gps/nmea` for diagnostics. GeoClue gets a filtered stream on
`/run/tablet-gps/nmea.sock`: GGA/RMC/GLL/GSA are forwarded only when they
describe a real fix, and the receiver's no-fix placeholder
`8960.0000,N,00000.0000,E` can never reach GeoClue. VTG is dropped; auxiliary
standard sentences (GSV, ZDA, TXT, GBS, GNS) are passed through.

## On-demand lifecycle

1. `tb351fu-gps.socket` is enabled at `sockets.target` and owns
   `/run/tablet-gps/nmea.sock` (`0666`). It is active from boot, but the
   service is **not** running.
2. GeoClue connects its NMEA source to the socket. systemd accepts the
   connection and starts `tb351fu-gps.service`.
3. The wrapper starts the bridge first (so it owns `mtk_mnl2hal` before
   `mnld` starts), then starts `run-mnld.sh`.
4. The bridge sends init/start. `mnld` powers the receiver and emits NMEA.
5. When the last client disconnects, the bridge waits 60 s, sends the HAL
   stop command, and exits `0`.
6. The wrapper waits up to 5 s for `mnld` to react, then exits `0`.
   `KillMode=control-group` makes systemd remove anything left in the
   service cgroup (`mnld`, `unshare`, the log sink). The socket stays active
   for the next request.

Do not use `pkill -x mnld` or a bare `wait` in the wrapper. `mnld` is PID 1 in
its private PID namespace, so host `SIGTERM` is ignored and the host cannot
reap it normally. cgroup teardown by systemd is the reliable path. To stop
everything manually, stop the socket first:

```sh
sudo systemctl stop tb351fu-gps.socket tb351fu-gps.service
```

## Files and installation

| File | Installed as |
|---|---|
| `tablet-gnss-bridge.py` | `/opt/tb351fu-gps/tablet-gnss-bridge.py` |
| `run-gps.sh` | `/opt/tb351fu-gps/run-gps.sh` |
| `run-mnld.sh` | `/opt/tb351fu-gps/run-mnld.sh` |
| `gps-runtime-inner.sh` | `/opt/tb351fu-gps/gps-runtime-inner.sh` |
| `logd-sink.py` | `/opt/tb351fu-gps/logd-sink.py` |
| `mnl-probe.py` | `/opt/tb351fu-gps/mnl-probe.py` (diagnostic only) |
| `ld.config.txt` | `/opt/tb351fu-gps/ld.config.txt` |
| `shim/props.txt` | `/opt/tb351fu-gps/shim/props.txt` |
| `shim/lib64/libgpsprops.so` | `/opt/tb351fu-gps/shim/lib64/libgpsprops.so` |
| `modules/gps_pwr.ko` | `/lib/modules/extra/gps_pwr.ko` |
| `modules/gps_drv_stp.ko` | `/lib/modules/extra/gps_drv_stp.ko` |
| `tb351fu-gps.service` | `/etc/systemd/system/tb351fu-gps.service` |
| `tb351fu-gps.socket` | `/etc/systemd/system/tb351fu-gps.socket` |
| `/etc/ssl/certs/*.0` (host) | `/opt/tb351fu-gps/system/etc/security/cacerts/*.0` |
| `geoclue.conf` | `/etc/geoclue/geoclue.conf` |
| `libgeoclue-simple-task-fix.patch` | not installed; applied to the geoclue 2.8.1 source used to build the patched `libgeoclue`, see below |
| `geoclue-probe.py` | not installed; run from a checkout or `/tmp` |

## Payload provenance

The large Android binaries are not duplicated in this directory. They are
read-only copies from the **stock slot A** partitions (slot A is never
written by this work):

- `/vendor/bin/mnld` (md5 `675e8542c71a259602d3240282f8fe52`)
- `/vendor/lib64/*` (`libmnl.so`, `libDR.so`, `libcurl.so`, `libssl.so`,
  `libcrypto.so`, `libnvram*.so`, `libmtkproperty.so`, ...)
- `/vendor/etc/MNL_Config.xml`, `/vendor/etc/mpe.conf`
- `/system/lib64/*` (`libbase.so`, `libbinder*.so`, `libc++.so`, `liblog.so`,
  `libcutils.so`, `libutils.so`, `libvndksupport.so`, ...)
- `/apex/com.android.runtime` (Bionic `linker64`, `libc.so`, ...)

The runtime APEX is packed into `runtime-apex.img`, a 12,845,056-byte ext2
image that `run-mnld.sh` loop-mounts read-only at
`/opt/tb351fu-gps/runtime-apex`. `/opt/tb351fu-gps/system/bin/linker64` is a
symlink to `/apex/com.android.runtime/bin/linker64`; inside the chroot that
path is the mounted APEX.

`shim/lib64/libgpsprops.so` is a small `LD_PRELOAD` library that supplies the
Bionic `__system_property_*` ABI and returns the hard-coded properties listed
in `shim/props.txt` (`persist.vendor.connsys.chipid=0x6789`,
`vendor.connsys.adie.chipid=0x6631`, `vendor.connsys.driver.ready=yes`,
`vendor.service.nvram_init=Ready`, `ro.build.type=user`, `ro.debuggable=0`,
`ro.vendor.mtk_log_hide_gps=0`). Unknown properties read as empty. It is a
prebuilt binary; its source is not in this repository.

`ld.config.txt` is a hand-written Android linker namespace configuration for
the chroot: `/vendor/lib64` plus the GPS shim for `mnld`, and the Bionic
runtime plus `/system/lib64` as the `system` namespace.

The two kernel modules are stock vendor modules from slot A
`/vendor_dlkm/lib/modules`:

- `gps_pwr.ko` (md5 `b882ab3c74059bb4af30d07bedee087c`, `depends: connadp`)
- `gps_drv_stp.ko` (md5 `ea280b6b8503e1ab03410168861d7c3c`,
  `depends: wmt_drv`)

`gps_drv_stp.ko` in this directory is the deployed copy: its 31-byte
`vermagic` string was rewritten from the stock `gaaaa486553c8` to the running
pmOS build `gd2c91f0c3b11` (the same relaxation used for the other vendor
modules). The unmodified stock pull is kept at the repository root as
`gps_drv_stp.ko`.

Persistent receiver state is stored outside the chroot at
`/var/lib/tb351fu-gps/data` and bind-mounted as `/data`:

```
/var/lib/tb351fu-gps/data/vendor/gps/
    mtkgps.dat          almanac / receiver state
    cache.dat
    MNL_Config.xml
    EPO.DAT             validated MediaTek EPO predicted orbits (persistent)
    QEPO_BD.DAT         BeiDou QEPO, downloaded when online
    QEPO_GA.DAT         Galileo QEPO, downloaded when online
    MTKNAV.DAT          MTKNav assistance file, downloaded when online
```

`EPO_GR_3_1.DAT`/`.MD5` and `EPOHAL.DAT` are transient download/merge files;
`mnld` deletes the merge file and keeps the result in `EPO.DAT`. `EPO.DAT`
covers roughly three days; at each GPS start `mnld` checks it and logs either
`EPO file is valid, no need update` or re-downloads it when expired.

## Assistance data (EPO/QEPO)

MediaTek EPO is **predicted satellite-orbit data**, not a location source. It
does not change the NMEA-only GeoClue policy; it only shortens time-to-first-fix
by letting the receiver know where to search before broadcast ephemeris arrives.
`mnld` downloads it over HTTPS from `vepodownload.mediatek.com` through the
vendor `libcurl.so`, whose compiled CA path is `/system/etc/security/cacerts`.
That directory must exist inside the payload or every download fails with curl
error 26/60 and EPO stays empty.

With current EPO, measured TTFF after a GPS start was 13–29 s (later runs:
17 s and 14 s); without EPO the previous boot took 6 min 32 s. When the certs
and network are available, `mnld` also downloads `QEPO_BD.DAT`,
`QEPO_GA.DAT`, and `MTKNAV.DAT`.

Refresh is automatic: at each start `mnld` logs
`EPO file is valid, no need update` while the file is current, and re-downloads
when it has expired (EPO covers roughly three days). To force a refresh, stop
the units, move `EPO.DAT` aside, then start a GPS client; `mnld` downloads,
MD5-checks, and merges a new file. Re-copy the host CA bundle after a host
`ca-certificates` update.

## Deploy

Copy the payload, modules, units, and GeoClue configuration. `mnld` needs
root for `mount`, `unshare`, and `chroot`, so the service runs as root.

```sh
cd common/deploy/gnss

sudo install -d -m 755 /opt/tb351fu-gps
sudo install -m 755 tablet-gnss-bridge.py run-gps.sh run-mnld.sh \
    gps-runtime-inner.sh logd-sink.py mnl-probe.py /opt/tb351fu-gps/
sudo install -m 644 ld.config.txt /opt/tb351fu-gps/
sudo install -d -m 755 /opt/tb351fu-gps/shim/lib64
sudo install -m 644 shim/props.txt /opt/tb351fu-gps/shim/
sudo install -m 755 shim/lib64/libgpsprops.so /opt/tb351fu-gps/shim/lib64/

# CA bundle for the vendor libcurl used by mnld's EPO/QEPO downloads.
sudo install -d -m 755 /opt/tb351fu-gps/system/etc/security/cacerts
sudo cp -L /etc/ssl/certs/*.0 /opt/tb351fu-gps/system/etc/security/cacerts/
sudo chmod 644 /opt/tb351fu-gps/system/etc/security/cacerts/*.0

sudo install -m 644 modules/gps_pwr.ko modules/gps_drv_stp.ko \
    /lib/modules/extra/
sudo depmod -a

sudo install -m 644 tb351fu-gps.service tb351fu-gps.socket \
    /etc/systemd/system/
sudo cp -a /etc/geoclue/geoclue.conf /etc/geoclue/geoclue.conf.bak-tb351fu
sudo install -m 644 geoclue.conf /etc/geoclue/geoclue.conf

sudo systemctl daemon-reload
sudo systemctl enable --now tb351fu-gps.socket
sudo systemctl restart geoclue
```

Do **not** enable `tb351fu-gps.service`; it is intentionally static and has
no `[Install]` section. The socket is the only boot-time unit.

The service's `ExecStartPre` loads `gps_pwr.ko` and `gps_drv_stp.ko` if they
are not already loaded, and read-only mounts
`/dev/disk/by-partlabel/nvdata` at `/mnt/vendor/nvdata` when it is not
already mounted. It is ordered after `pmos-hardware-init.service` and
`pmos-wifi-supervisor.service`, which load `connadp`/`wmt_drv`.

## GeoClue configuration

`geoclue.conf` is NMEA-only:

- `[network-nmea] enable=true`, `nmea-socket=/run/tablet-gps/nmea.sock`
- `[ip]`, `[wifi]`, `[static-source]`, `[3g]`, `[cdma]`, `[modem-gps]` are
  all disabled. The static source is explicitly off, so there is no
  fallback coordinate anywhere.
- `sm.puri.Phosh` is in the agent whitelist, and `firefox` is an allowed
  application.

GeoClue agents are **per-UID**. Phosh registers `sm.puri.Phosh` for the
session user (uid 10000). A root D-Bus client has no agent and its `Start`
call waits forever. Run probes as the session user, and request accuracy
`EXACT` = `8`; the NMEA source advertises accuracy 8.

GeoClue is D-Bus activated and exits after 60 s with no clients. Phosh
re-registers its agent when GeoClue comes back.

**Final configuration (2026-11-15).** This NMEA-only GeoClue setup is the
accepted final configuration: real GNSS fixes, on-demand GPS power, and no
network, WiFi, cell, static, or modem location source. With those sources
disabled, an indoor Maps blue dot is still a real receiver fix, not a
fallback coordinate. Do not enable a fallback source without explicit
approval.

## libgeoclue `GClue.Simple` fix (GNOME Maps)

Unpatched libgeoclue 2.8.1 delivers only the **first** location update to
clients using `GClue.Simple`, which GNOME Maps does. Maps draws the first blue
dot and then never moves it; after a while it logs
`g_task_return_boolean: assertion '!task->ever_returned' failed` and repeated
`g_atomic_ref_count_dec: assertion 'old_value > 0' failed`, and it can die with
`SIGSEGV`. Firefox uses the raw GeoClue D-Bus API and is not affected.

Cause: `on_client_created()` stores the async-init `GTask` in `priv->task` and
holds a reference. The task completes on the first location, but that
reference is never released, so `priv->task` stays non-NULL. Every later
`on_location_proxy_ready()` calls `g_task_return_boolean()` again instead of
`g_object_notify("location")`, so the `location` property never changes again.
`libgeoclue-simple-task-fix.patch` clears `priv->task` after the first
completion in both the non-portal and portal paths. It deliberately leaves the
existing task reference alone; releasing it early causes a GJS
`GTask has been finalized while it was still owned by gjs` critical and a
GObject signal crash.

Installed 2026-11-15 from geoclue 2.8.1 (tag `2.8.1`, commit
`81ddcb11208a59328a04bf022542504ac80602f5`):

- `/usr/lib/libgeoclue-2.so.0.0.0` SHA256
  `23e3b69abb6e81156f5206f916c44688bc5fae9f0d447c5d75fdb3192ad58696`
- original package library backup:
  `/var/lib/tb351fu-gps/backup/libgeoclue-2.so.0.0.0.pre-taskfix-20261115-160855`
  (SHA256 `32559f50360ada16ce12a22722d7e4adb5e4509eb52298da79dc09f92c1003bd`)
- abandoned aggressive variant, do not use:
  `/var/lib/tb351fu-gps/backup/libgeoclue-2.so.0.0.0.taskfix-v1-20261115-161326`

The file is owned by the `geoclue-2.8.1-r0` package, so an `apk upgrade` or
`apk fix geoclue` that reinstalls the package overwrites the patched library.
After a geoclue upgrade, check the hash and rebuild/reapply if it changed.

Build (on the tablet or another aarch64 pmOS rootfs with `build-base`,
`meson`, `ninja`, `glib-dev`, `gobject-introspection-dev`, and
`json-glib-dev`):

```sh
git clone --depth 1 --branch 2.8.1 \
    https://gitlab.freedesktop.org/geoclue/geoclue.git
cd geoclue
patch -p1 < /path/to/libgeoclue-simple-task-fix.patch
meson setup build \
    -Denable-backend=false -Dlibgeoclue=true -Dintrospection=false \
    -Dvapi=false -Dgtk-doc=false -Ddemo-agent=false \
    -Dnmea-source=false -Dwifi-source=false -D3g-source=false \
    -Dcdma-source=false -Dmodem-gps-source=false -Dstatic-source=false \
    -Dip-source=false -Dcompass=false --prefix=/usr
ninja -C build

sudo install -d -m 755 /var/lib/tb351fu-gps/backup
sudo cp -a /usr/lib/libgeoclue-2.so.0.0.0 \
    /var/lib/tb351fu-gps/backup/libgeoclue-2.so.0.0.0.before-$(date +%Y%m%d-%H%M%S)
sudo install -m 755 -o root -g root \
    build/libgeoclue/libgeoclue-2.so.0.0.0 /usr/lib/libgeoclue-2.so.0.0.0
sha256sum /usr/lib/libgeoclue-2.so.0.0.0
```

The build is unstripped (about 395 KiB versus 130 KiB for the package
library) and has the same `libgeoclue-2.so.0` SONAME, so no symlink changes
are needed. The library is loaded by client processes, not the GeoClue daemon;
stop any running Maps and relaunch it to pick up the patch.

Verify continuous updates as the session user, with Maps open:

```sh
MAPS_DEBUG=1 gnome-maps 2>&1 | grep "Updated location"
```

New lines should keep appearing at every GGA/RMC fix. The occasional
`GPS GGA+RMC` line is the two sentences merged in one read; that is normal.

Revert to the package library:

```sh
sudo install -m 755 -o root -g root \
    /var/lib/tb351fu-gps/backup/libgeoclue-2.so.0.0.0.pre-taskfix-20261115-160855 \
    /usr/lib/libgeoclue-2.so.0.0.0
```

or reinstall the package with `sudo apk fix geoclue`; relaunch Maps afterward.
Upstream 2.8.2 and current master still contain the same task logic, so a
newer package is not a fix by itself.

## Verify

Check that the socket is idle and GPS is off:

```sh
systemctl is-active tb351fu-gps.socket      # active
systemctl is-active tb351fu-gps.service     # inactive when idle
pgrep -af '^/vendor/bin/mnld'               # no output
systemctl show tb351fu-gps.socket -p NConnections -p NAccepted
```

Watch an on-demand cycle. Any client that connects to the **socket** starts
the service. The raw pty `/run/tablet-gps/nmea` only exists while the bridge
is running, so a plain `cat` on it cannot be the first connection:

```sh
timeout 8 python3 - <<'PY'
import socket, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("/run/tablet-gps/nmea.sock")
s.settimeout(5)
end = time.time() + 5
while time.time() < end:
    try:
        data = s.recv(4096)
    except socket.timeout:
        break
    if not data:
        break
    print(data.decode(errors="replace"), end="")
s.close()
PY
```

Once the service is active, the raw pty carries every sentence:

```sh
sudo timeout 5 cat /run/tablet-gps/nmea
```

After the client disconnects, expect this sequence in the journal within ~60 s:

```
NMEA client connected (1)
GPS start requested
no NMEA clients for 60 s, stopping
GPS stop sent
tb351fu-gps.service: Deactivated successfully.
```

Run the GeoClue probe as the session user (not root):

```sh
python3 common/deploy/gnss/geoclue-probe.py
```

A real fix looks like this (overnight capture, 2026-11-14):

```
$GNGGA,154725.317,2105.4722,N,07959.4748,E,1,5,1.98,268.2,M,-65.9,M,,*63
```

That is 21°05.4722′N, 079°59.4748′E, quality 1, 5 satellites.

A live outdoor fix on 2026-11-15 12:03:33 IST confirmed the whole chain:

```
$GNGGA,063336.000,2105.4725,N,07959.4677,E,1,5,1.17,249.7,M,-65.9,M,,*6E
```

GeoClue reported `lat=21.09119, lon=79.99114, acc=1.0` from `GPS RMC`, and
GNOME Maps showed the blue dot. If the dot appears but never moves, the
patched libgeoclue is missing; see "libgeoclue `GClue.Simple` fix" above. If
the map is panned elsewhere, the dot can be off-screen: press **Current
Location**, or run `/usr/local/bin/pmos-maps-current-location` as the session
user (uid 10000, not root). The receiver briefly emitted a quality-0 position at
33°36′N 070°53′E before the real fix; the bridge filtered it and GeoClue
never saw it. `mnld` also logs `MTK_GPS_MSG_FIX_READY,GET_RTC_FAIL` at start
when it has no satellite time; this warning did not prevent either fix and
is not considered fatal.

### Diagnostics

```sh
sudo journalctl -u tb351fu-gps.service -b --no-pager
sudo tail -50 /run/tb351fu-gps/mnld.log
sudo systemctl status tb351fu-gps.socket tb351fu-gps.service

# Assistance-data state: downloads, validity, and the persistent file.
sudo grep -E 'EPO|QEPO' /run/tb351fu-gps/mnld.log | tail -20
sudo ls -l /var/lib/tb351fu-gps/data/vendor/gps/EPO.DAT
```

Use `pgrep -f '^/vendor/bin/mnld'`, not `pgrep -x mnld`; BusyBox/PID-namespace
behavior means the short name is not matched.

### Remote access while testing outdoors

`sshd` listens on all interfaces and is enabled at boot, so the tablet can be
driven over WiFi as well as the USB gadget link (`172.16.42.1`). Find the
current WiFi address with `nmcli -t -f DEVICE,STATE,CONNECTION device status`
and `ip route`, then connect as the session user. The router's DNS name works
too; on the reference network it is
`lenovo-tab-plus-tb351fu.bbrouter`. The USB link can be unplugged without
stopping sshd, GeoClue, Maps, or the GPS service. WiFi power saving can make
the first packet of a new connection slow; that is normal.

`mnl-probe.py` is the original HAL protocol probe from bring-up. It binds
`mtk_mnl2hal`, sends init/start, prints message types and NMEA, then sends
stop. It requires a running `mnld` with **no bridge** owning the HAL sockets,
so it is a manual-bring-up tool only; the socket-activated production path
does not use it.

## Known issues and non-issues

- **No fix indoors is expected.** The receiver is proven by the overnight fix
  above. At a desk with no sky view it can report 0 satellites and only the
  `8960.0000,N,00000.0000,E` placeholder. The stock Android slot A GNSS also
  failed at this location, so this is a site/antenna-visibility issue, not a
  chroot regression. Verify near a window or outside for 5–10 minutes.
- **A retained fix disappears on reboot.** While indoors, Maps can keep drawing
  the last outdoor fix. A reboot clears it because the NMEA-only setup has no
  fallback source and no persisted position; the dot then requires a new real
  fix. This is expected, not a regression.
- **GNOME Maps needs the patched libgeoclue.** With the stock 2.8.1 library,
  `GClue.Simple` delivers only the first fix, so the dot freezes and Maps can
  crash; Firefox is unaffected. The patch is overwritten by a geoclue package
  upgrade. See "libgeoclue `GClue.Simple` fix" for the hash, rebuild steps, and
  the backup used to revert.
- **EPO assistance is downloaded, not shipped.** The payload must contain host
  CA certificates in `system/etc/security/cacerts/` or `mnld` cannot download
  EPO/QEPO (curl errors 26/60) and cold starts take minutes. EPO expires after
  roughly three days; `mnld` re-downloads it automatically when online. Offline
  with expired EPO still works, just with a slower first fix.
- **GPS state vs. battery:** the socket itself is always present, but no
  receiver, `mnld`, or bridge process runs until a client connects. After the
  last client leaves, the receiver is stopped and the service is gone.
- **GeoClue without clients:** Phosh/GNOME may keep a background Maps
  service running; that counts as a location client and keeps GPS on. Close
  the app or its background service to let the idle timer expire.
- **The chroot is a compatibility environment, not a security boundary.**
  Vendor Android binaries run as root with private namespaces. Treat the
  payload as trusted stock firmware.
- **Slot A is read-only.** Nothing here writes to slot A; blobs are copied
  from it. Do not flash or modify slot A while servicing this deployment.
- **`/run` is volatile.** The payload in `/opt` and the state in
  `/var/lib` survive reboots; `/run/tablet-gps` is recreated by the units.
- **`geoclue.conf.bak-tb351fu`** is the pre-change backup. Restore it to
  return GeoClue to its stock source configuration.
