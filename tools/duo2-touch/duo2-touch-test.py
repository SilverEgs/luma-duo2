#!/usr/bin/env python3
"""Synthetic touches for duo2-touch --test-input (bring-up self-tests).

Builds heat reports in the digitizer's own format -- an idle image with one
finger-shaped bump per contact -- and sends them to duo2-touch's test socket
at the digitizer's 100 Hz, so a tap or swipe travels the whole real path:
detection, tracking, uinput, libinput, the compositor.

  duo2-touch-test.py tap   left|right FX FY [--hold MS]
  duo2-touch-test.py swipe left|right FX0 FY0 FX1 FY1 [--ms MS]

FX/FY are fractions of the panel (0..1, x across the short side, y down).
"""
import argparse, math, socket, struct, time

SOCK = "/run/duo2-touch-test.sock"
COLS, ROWS = 21, 30                  # antennas along x and y (PS_CFG_DATA_Zeta)
SIZE = (85.18, 120.20)               # panel, mm
ACTIVE = (81.15, 117.67)             # antenna span, mm
K = 0.0022203543                     # byte -> magnitude step (TouchDetection::Init)
IDLE = 180                           # the byte for no signal
REPORT = {"left": 0x3C, "right": 0x0A}


def antenna(f, axis):
    pitch = ACTIVE[axis] / ((COLS if axis == 0 else ROWS) - 1)
    mm = f * SIZE[axis]
    return (mm - (SIZE[axis] - ACTIVE[axis]) / 2) / pitch


def report(panel, seq, contacts, peak=0.2, sigma=1.0):
    img = bytearray([IDLE]) * (COLS * ROWS)
    for fx, fy in contacts:
        ax, ay = antenna(fx, 0), antenna(fy, 1)
        for x in range(COLS):
            for y in range(ROWS):
                m = peak * math.exp(-((x - ax) ** 2 + (y - ay) ** 2) / (2 * sigma ** 2))
                b = max(0, int(round(img[x * ROWS + y] - m / K)))
                img[x * ROWS + y] = b
    sec = struct.pack("<IHBBII", 16 + len(img), 0x0100, 1, 8, 0, len(img)) + bytes(img)
    body = b"\0" + sec                      # byte 9, then sections from 10
    head = struct.pack("<BHIH", REPORT[panel], seq & 0xFFFF, 0, 0)
    r = bytearray(head + body)
    struct.pack_into("<I", r, 3, len(r) - 3)
    return bytes(r)


def run(panel, path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    t = time.monotonic()
    for seq, contacts in enumerate(path):
        s.sendto(report(panel, seq, contacts), SOCK)
        t += 0.010
        time.sleep(max(0.0, t - time.monotonic()))
    s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("kind", choices=["tap", "swipe"])
    ap.add_argument("panel", choices=["left", "right"])
    ap.add_argument("coords", nargs="+", type=float)
    ap.add_argument("--hold", type=int, default=80, help="tap length, ms")
    ap.add_argument("--ms", type=int, default=300, help="swipe length, ms")
    a = ap.parse_args()
    if a.kind == "tap":
        fx, fy = a.coords
        run(a.panel, [[(fx, fy)]] * max(1, a.hold // 10))
    else:
        x0, y0, x1, y1 = a.coords
        n = max(2, a.ms // 10)
        run(a.panel, [[(x0 + (x1 - x0) * i / (n - 1), y0 + (y1 - y0) * i / (n - 1))] for i in range(n)])


if __name__ == "__main__":
    main()
