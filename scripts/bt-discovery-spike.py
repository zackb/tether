#!/usr/bin/env python3
"""Throwaway #287 Phase 0 spike: registers the LAN-info GATT service with BlueZ.

Serves the real JSON value from docs/BT_DISCOVERY_PLAN.md and logs every read with the
bearer BlueZ reports ("link": LE or BR/EDR). Ctrl-C unregisters. Delete with the plan file.
"""
import json
import socket
import subprocess
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

SERVICE_UUID = "ba7a69f7-bf0f-4c09-8c73-cf4ee9afb5b5"
CHAR_UUID = "ba7a69f7-bf0f-4c09-8c73-cf4ee9afb5b6"
ROOT = "/org/tether/spike"
SVC = ROOT + "/service0"
CHR = SVC + "/char0"
ADAPTER = sys.argv[1] if len(sys.argv) > 1 else "/org/bluez/hci0"

XML = f"""
<node>
  <interface name='org.freedesktop.DBus.ObjectManager'>
    <method name='GetManagedObjects'><arg type='a{{oa{{sa{{sv}}}}}}' direction='out'/></method>
  </interface>
  <interface name='org.bluez.GattCharacteristic1'>
    <method name='ReadValue'>
      <arg type='a{{sv}}' direction='in'/><arg type='ay' direction='out'/>
    </method>
  </interface>
</node>"""


def addrs():
    out = json.loads(subprocess.run(["ip", "-j", "addr"], capture_output=True, text=True).stdout)
    v4, v6 = [], []
    for iface in out:
        if "UP" not in iface.get("flags", []) or "LOOPBACK" in iface.get("flags", []):
            continue
        for a in iface.get("addr_info", []):
            if a.get("scope") != "global":
                continue
            (v4 if a["family"] == "inet" else v6).append(f"{a['local']}/{a['prefixlen']}")
    return v4 + v6


def value():
    doc = {"v": 1, "name": socket.gethostname(), "port": 5134, "fp": "0" * 64, "addrs": addrs()}
    while True:
        raw = json.dumps(doc, separators=(",", ":")).encode()
        if len(raw) <= 512 or not doc["addrs"]:
            return raw
        doc["addrs"].pop()


def managed_objects():
    return {
        SVC: {"org.bluez.GattService1": {
            "UUID": GLib.Variant("s", SERVICE_UUID), "Primary": GLib.Variant("b", True)}},
        CHR: {"org.bluez.GattCharacteristic1": {
            "UUID": GLib.Variant("s", CHAR_UUID), "Service": GLib.Variant("o", SVC),
            "Flags": GLib.Variant("as", ["encrypt-read"])}},
    }


def on_call(conn, sender, path, iface, method, params, inv):
    if method == "GetManagedObjects":
        inv.return_value(GLib.Variant("(a{oa{sa{sv}}})", (managed_objects(),)))
        return
    opts = params.unpack()[0]
    offset = opts.get("offset", 0)
    raw = value()
    print(f"ReadValue device={opts.get('device')} link={opts.get('link')} mtu={opts.get('mtu')} "
          f"offset={offset} len={len(raw)}", flush=True)
    if offset == 0:
        print(f"  value: {raw.decode()}", flush=True)
    inv.return_value(GLib.Variant("(ay)", (raw[offset:],)))


def main():
    bus = Gio.bus_get_sync(Gio.BusType.SYSTEM)
    node = Gio.DBusNodeInfo.new_for_xml(XML)
    bus.register_object(ROOT, node.interfaces[0], on_call)
    bus.register_object(CHR, node.interfaces[1], on_call)
    # GattService1 properties travel only via GetManagedObjects; BlueZ never reads them directly.

    loop = GLib.MainLoop()

    def registered(b, res):
        try:
            b.call_finish(res)
            print(f"registered on {ADAPTER}; value is {len(value())} bytes", flush=True)
        except GLib.Error as e:
            print(f"RegisterApplication failed: {e.message}", flush=True)
            loop.quit()

    bus.call("org.bluez", ADAPTER, "org.bluez.GattManager1", "RegisterApplication",
             GLib.Variant("(oa{sv})", (ROOT, {})), None, Gio.DBusCallFlags.NONE, 10000, None, registered)
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    try:
        bus.call_sync("org.bluez", ADAPTER, "org.bluez.GattManager1", "UnregisterApplication",
                      GLib.Variant("(o)", (ROOT,)), None, Gio.DBusCallFlags.NONE, 5000, None)
    except GLib.Error:
        pass


if __name__ == "__main__":
    main()
