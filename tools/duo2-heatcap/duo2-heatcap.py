#!/usr/bin/env python3
"""duo2-heatcap: record the Duo 2 digitizer's raw HID reports.

Every report read from hidraw is written as one record:

    u64 CLOCK_MONOTONIC nanoseconds | u32 length | report bytes

hidraw gives each reader its own copy of every input report, so this runs
next to the touch daemon without disturbing it. The recordings are the
reference input for duo2-touchd's heat parser and contact tracker.

Usage: duo2-heatcap.py OUTPUT [--hidraw /dev/hidrawN] [--seconds N]
"""
import argparse
import os
import select
import struct
import sys
import time


def find_hidraw():
    for name in sorted(os.listdir("/sys/class/hidraw")):
        with open(f"/sys/class/hidraw/{name}/device/uevent") as f:
            if ":0000045E:00000C1D" in f.read():
                return f"/dev/{name}"
    sys.exit("no digitizer hidraw device")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("--hidraw")
    ap.add_argument("--seconds", type=float, default=0)
    a = ap.parse_args()
    dev = a.hidraw or find_hidraw()
    fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
    end = time.monotonic() + a.seconds if a.seconds else None
    n = heat = 0
    with open(a.output, "wb") as out:
        while end is None or time.monotonic() < end:
            r, _, _ = select.select([fd], [], [], 0.5)
            if not r:
                continue
            try:
                data = os.read(fd, 16384)
            except BlockingIOError:
                continue
            out.write(struct.pack("<QI", time.monotonic_ns(), len(data)) + data)
            n += 1
            heat += data[0] != 0xFE
    print(f"{dev}: {n} reports ({heat} not heartbeats) -> {a.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
