# Touch on the Surface Duo 2

The Duo 2's digitizer is a Microsoft "G6" capacitive sensor behind both
panels, on `spi0.0`, driven by Microsoft's `spi-hid` (HID `045E:0C1D`, product
`Zeta`). It does not report finger positions. It streams raw heat frames, one
report per panel per scan, and on Android a userspace library turns them into
contacts. On Luma, `tools/duo2-touch` does that natively: a plain C daemon
that follows the stock library's algorithm step by step and reads the stock
tuning from the library at run time.

Nothing here is guessed: every value below comes out of the stock firmware or
the digitizer itself.

## What the stock firmware contains

| Piece | Where | Role |
|---|---|---|
| `vendor.surface.touchpen@1.3-service` | `/vendor/bin/hw/` | the touch HAL: reads `hidraw`, drives the library, injects through uinput |
| `libsurfacetouch_c1.so` | `/vendor/lib64/c1/` | Microsoft's heat processor, Duo 2 build: contact detection, palm rejection, panel stitching, hinge handling, tuning (`PS_CFG_DATA_Zeta`) |
| `G6_FW_PKG_Zeta.cfu` | `/vendor/firmware/cfu/hid/touch/0C1D/` | the digitizer's own firmware (already on the device) |
| `PixelAlignmentData.bin` | `sfpd` partition, `display/` | per-unit two-panel alignment |

## The heat reports

Report `0x3C` is the left panel, `0x0A` the right one; `0xFE` is a heartbeat
naming its panel in its second byte. Frames arrive at 100 Hz while something
touches a panel and stop when nothing does. A frame
(`SurfaceHeatProcessorLogic::ParseFullFrame`, `Parser::RunParser`):

| Bytes | Content |
|---|---|
| 0 | report id |
| 1-2 | sequence number |
| 3-6 | payload length |
| 10- | sections: `u32 size, u16 type`, then the body |

Section `0x0100` is the heat image: byte 6 nonzero for 8-bit samples, a `u32`
start and `u32` count at 8 and 12, then 630 bytes: 21 rows (the panel's short
side) of 30 antennas (its long side). The stock code calls the long axis x and
the short one y, and so does `heat.c`; only the OS report swaps them. `0xB4` (180) is no signal;
smaller is stronger. Section `0xFF00` holds sub-sections (`u8 type, u8 flags,
u16 size`): the cycle header (0), topology (3: 21 x 30), frequency noise (4),
gain clipping (6), scan time (`0x32`). There is no blob section: the digitizer
does no detection.

The daemon puts the digitizer in heat mode (feature report 5 = 1) and reports
both panels on (output report `0x38`, bit 0 left, bit 1 right), after waiting
for `spi0.0/ready` to read `ready`, as the stock service does.

## The tuning: `PS_CFG_DATA_Zeta`

A 41088-byte `PSDB` blob (project `0x0C1D`) in `libsurfacetouch_c1.so`.
`duo2-touch` finds it through the library's symbol table and reads:

| Offset | Holds | Duo 2 values |
|---|---|---|
| `0x3c` | panel region record: antennas, active span, panel size, report range | 21 x 30; 81.15 x 117.67 mm; 85.18 x 120.20 mm; 0..8517 x 0..12019 |
| `0xef4` | `TouchDetectionCfg`: threshold clamp, strong-junction level, peak floor, noise table | 0.04..0.104, 0.04, 0.055, 20 entries |
| `0xef4 + 0xb4..0x100` | blob size limits, short and long saddle factors, topography levels, halo fractions | 300, 1.9 / 1.11 / 1.2, 1.27, 0.5 / 0.75 / 0.875 |
| `0xef4 + 0xe80` | palm-sized blob | 50 junctions |
| `0xc94 + 4` | "near the border" margin | 0.21 antennas |
| `0x1da0 + 4` | blob radius factor | 0.85 |
| `0x104`, `0x10b` | two-panel alignment clamp and switch | 0.14 mm, on |
| `0x870 + 0x22c` | `StretchingConfig`: edge zones and knees per axis | x: -0.11 / 0.6, y: 0.11 / 0.98 |

The antenna pitch is the active span over the antenna count less one (4.0576
mm both ways), and the antennas are centred on the panel (2.015 mm, 1.265 mm).

## The pipeline (`heat.c`)

Each step names the stock function it reproduces.

1. **Magnitudes** (`TouchDetection::Init`): byte `b` is `1 - (b * 0.0022203543
   + 0.6)`.
2. **Threshold** (`TouchDetection::CclLogic`): the first noise-table entry
   whose limit exceeds the frame's noise level, clamped to the configured
   range. Here that is 0.04, a byte of 162 or less.
3. **Blobs** (`RunCcl`, `RunCclForSpecificJunction`, `CreateNewBlob`,
   `MergeSameBlobsData`): one raster pass with 8-connectivity, joining only
   through strong junctions, then label equivalences merged (with the stock
   merge order, which decides each merged record's sums and index). A blob of
   two junctions or fewer needs a peak above 0.055; one of 300 or more ends
   the frame.
4. **Features** (`FeatureExtraction`): the position is the centroid weighted
   by each junction's magnitude above the threshold; border, near-border,
   corner and hinge flags (`SetBlobOnBorderFlags`); the radius, the blob's
   widest line (sum over maximum) times half the pitch times 0.85, across the
   axis away from any border it touches (`SetBlobRadius`); the peaks
   (`DetectPeak`), with neighbouring peaks joined across short and long
   saddles (`MergePeaksWithShortSaddle`, `...LongSaddle`); orientation, axis
   ratio and spread from the second moments (`CalculateShapeFeatures`, with
   bionic's `atanf`); the sub-blobs at three levels between floor and peak
   (`CalculateTopography`, a second CCL pass); and the halo
   (`CalculateHaloFeatures`). Blobs whose peak is under 0.04 are dropped, and
   blobs of 50 junctions or more marked as palms.

   All of this is single-precision arithmetic in the stock order, built
   without fused multiply-adds: over a recorded session (3220 frames) every record and
   feature is bit-identical to the stock library's (see Debugging).
5. **Millimetres** (`DetectedArray::ConvertPositionFromAntennaToMmAndFlip`,
   `FixOffsetRotation`): antenna position times the pitch, less the centring
   offset and the unit's two-panel alignment. The alignment is the float at
   byte 16 of the unit's `PixelAlignmentData.bin`, in micrometres, kept only
   between -0.14 and 0.28 mm and clamped to 0.14 mm
   (`PanelAlignmentReaderPersistentStorageFullUefiStructure`,
   `SetCurrentPanelAlignmentOffset`). Without the file it is 0.
6. **Edge stretching** (`ReportInformation::RunStretching`): within `r +
   zone` of an edge, `[r * 0.4247 - 0.0316 + knee, r + zone]` is spread over
   `[0, r + zone]`, so a finger whose centre cannot reach the antennas' edge
   can still reach the screen's.
7. **Tracking**: each blob joins the track whose predicted position (last
   position plus last motion, as `TouchDetection::MatchBlobsToTracks`
   predicts) is nearest, closest pairs first; a panel without frames for 40 ms
   has been released.
8. **Report** (`TracksArray::FillOsReport`): millimetres to the panel's
   logical range (8517 / 85.18 per mm across, 12019 / 120.20 along).

Replaying a recording of the stock service against its own output
(`--replay`) puts the median difference at 0.4 mm across and 0.5 mm along the
panel, most of it the stock tracker's smoothing lag. Still to port from the
stock tracker: its Kalman and IIR smoothing, jitter restriction, the
backward-prediction edge extrapolation (stock reaches the outermost 1-2 mm),
and its palm classification.

## On Luma

Each panel is its own uinput touchscreen: "Surface Duo 2 Touchscreen Left"
(`045E:0C1D`) and "... Right" (`045E:0C1E`), 0..8517 x 0..12019 at 100 units
per mm. Both panels report the same EDID, so Mutter cannot match a
touchscreen to its panel by itself; `luma-duo2-touch-outputs` (an autostart
entry) sets each touchscreen's `org.gnome.desktop.peripherals.touchscreen`
`output` to its panel, with the connector as the fourth field to tell the
twins apart. It has to be the user's own value: Mutter reads a dconf default
as "automatic".

`scripts/duo2-build-touch.sh` builds the daemon for the phone;
`duo2-compose-ostree.sh` installs it with the stock library (as data only, for
its tuning) and the `luma-duo2-touch` unit. Before each start,
`luma-duo2-touch-calibration` copies the unit's alignment from its `sfpd`
partition (read-only) to `/var/lib/luma-duo2/touch/TwoPanelAlignment.bin`, as
Android's `init.surface.touch.preloads.sh` does; the file is the unit's own
and never part of the image.

## Debugging

- `duo2-touch -v` logs every contact and lift.
- `duo2-touch --replay FILE` runs a `tools/duo2-heatcap` recording through the
  pipeline and prints the contacts.
- `duo2-touch --test-input` opens `/run/duo2-touch-test.sock`, which takes
  heat reports in the digitizer's own format; `duo2-touch-test.py tap left 0.5
  0.5` or `... swipe right 0.5 0.99 0.5 0.5` sends a synthetic finger through
  the whole path to the compositor, for testing without touching the phone.
- `duo2-touch --replay FILE --probe` prints every stage (blob records,
  features, fingers) in the format of `stock-ref/heatprobe.cpp`, a library
  preloaded under `stock-ref/duo2-heatref.cpp` that prints the same stages
  from inside the stock library as it runs the same recording (wrapping the
  functions it calls through its own PLT). Comparing the two line by line,
  frame by frame, is how each step is checked. The stock side needs the
  user's own vendor image and runs on the phone with the stock `linker64`
  and runtime (`scripts/duo2-touch-bundle.sh`).

## The earlier approach

`tools/duo2-touchd` hosted the stock library itself under the stock
`linker64` and bionic (`scripts/duo2-touch-bundle.sh`). The library reaches
for Android services (the stats HAL) that never answer here and stalls, which
is why touch is now native.
