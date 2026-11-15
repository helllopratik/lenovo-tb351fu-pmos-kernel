#!/usr/bin/python3
"""Query GeoClue from one persistent D-Bus connection and print the fix."""
import sys
import time

from gi.repository import Gio, GLib

BUS = "org.freedesktop.GeoClue2"
ROOT = "/org/freedesktop/GeoClue2"


def call(conn, path, iface, method, args=None, ret=None):
    return conn.call_sync(BUS, path, iface, method, args, ret,
                          Gio.DBusCallFlags.NONE, -1, None)


bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
try:
    res = call(bus, ROOT + "/Manager", BUS + ".Manager", "GetClient",
               None, GLib.VariantType.new("(o)"))
except Exception as e:
    print("GetClient failed:", e)
    sys.exit(1)
client = res.unpack()[0]
print("client:", client)


def setprop(name, value):
    call(bus, client, "org.freedesktop.DBus.Properties", "Set",
         GLib.Variant("(ssv)", (BUS + ".Client", name, value)), None)


setprop("DesktopId", GLib.Variant("s", "tb351fu-probe"))
setprop("RequestedAccuracyLevel", GLib.Variant("u", 8))
try:
    call(bus, client, BUS + ".Client", "Start", None, None)
except Exception as e:
    print("Start failed:", e)
    sys.exit(2)
print("started; waiting for a fix")

last = None
for i in range(30):
    time.sleep(1)
    try:
        v = call(bus, client, "org.freedesktop.DBus.Properties", "Get",
                 GLib.Variant("(ss)", (BUS + ".Client", "Location")),
                 GLib.VariantType.new("(v)")).unpack()[0]
    except Exception as e:
        print("Location read failed:", e)
        continue
    lp = v if isinstance(v, str) else v.unpack()
    if not lp or lp == "/":
        continue
    try:
        props = call(bus, lp, "org.freedesktop.DBus.Properties", "GetAll",
                     GLib.Variant("(s)", (BUS + ".Location",)),
                     GLib.VariantType.new("(a{sv})")).unpack()[0]
    except Exception as e:
        print("Location properties failed:", e)
        continue
    def val(x):
        if x is None:
            return None
        return x.unpack() if hasattr(x, "unpack") else x

    lat = props.get("Latitude")
    lon = props.get("Longitude")
    acc = props.get("Accuracy")
    desc = props.get("Description")
    ts = props.get("Timestamp")
    cur = (val(lat), val(lon), val(acc))
    if cur != last:
        print("FIX lat=%s lon=%s acc=%s desc=%s ts=%s" %
              (cur[0], cur[1], cur[2], val(desc), val(ts)), flush=True)
        last = cur
    if cur[0] is not None and cur[2] is not None and cur[2] < 1000:
        break

call(bus, client, BUS + ".Client", "Stop", None, None)
print("stopped")
