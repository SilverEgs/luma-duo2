#!/usr/bin/python3
# SPDX-License-Identifier: Apache-2.0
"""Screenshot the GNOME Shell session on the Duo 2 (bring-up).

GNOME Shell serves its own screenshot interface only to a few system
callers, so this goes through the desktop portal like any application:
org.freedesktop.portal.Screenshot, non-interactive. The portal allows that
without a dialog once the permission store says so for host programs:

    flatpak permission-set screenshot screenshot "" yes

Run as the session's user with its DBUS_SESSION_BUS_ADDRESS. Writes the
whole stage (every panel side by side) to OUTPUT.png.

Usage: duo2-shot.py OUTPUT.png
"""
import shutil
import sys
from urllib.parse import unquote, urlparse

from gi.repository import Gio, GLib

out = sys.argv[1]
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
token = "duo2shot%d" % GLib.random_int_range(0, 1 << 30)
sender = bus.get_unique_name()[1:].replace(".", "_")
handle = f"/org/freedesktop/portal/desktop/request/{sender}/{token}"
loop = GLib.MainLoop()
result = {}


def on_response(_conn, _sender, _path, _iface, _signal, params):
    code, results = params.unpack()
    result["code"], result["uri"] = code, results.get("uri")
    loop.quit()


bus.signal_subscribe("org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request", "Response",
                     handle, None, Gio.DBusSignalFlags.NO_MATCH_RULE, on_response)
bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "AddMatch",
              GLib.Variant("(s)", (f"type='signal',interface='org.freedesktop.portal.Request',path='{handle}'",)),
              None, Gio.DBusCallFlags.NONE, -1)
bus.call_sync("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
              "org.freedesktop.portal.Screenshot", "Screenshot",
              GLib.Variant("(sa{sv})", ("", {"handle_token": GLib.Variant("s", token),
                                              "interactive": GLib.Variant("b", False)})),
              None, Gio.DBusCallFlags.NONE, 30000)
GLib.timeout_add_seconds(30, loop.quit)
loop.run()
if result.get("code") != 0 or not result.get("uri"):
    print(f"screenshot failed: {result or 'no response'}")
    sys.exit(1)
shutil.move(unquote(urlparse(result["uri"]).path), out)
print(out)
