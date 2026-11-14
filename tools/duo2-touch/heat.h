// Surface Duo 2 touch: heat frames to contacts, as Microsoft's heat processor
// (libsurfacetouch_c1.so, the Duo 2 "Zeta" build) computes them.
//
// Conventions are the stock library's: a panel's heat image has 21 rows (y,
// the panel's short axis) of 30 antennas (x, its long axis); positions in
// antennas and millimetres keep x along the long axis. Only the OS report
// swaps them (TracksArray::FillOsReport), so that a report's x runs across
// the panel's short side.
//
// See docs/touch.md for the stock functions each step reproduces.

#ifndef DUO2_HEAT_H
#define DUO2_HEAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define HEAT_MAX_X 32             // antennas along a panel's long axis (30 here)
#define HEAT_MAX_Y 32             // along its short axis (21 here)
#define HEAT_MAX_BLOBS 48         // TouchDetection keeps 48 blob records
#define HEAT_MAX_PEAKS 10         // peaks recorded per blob
#define HEAT_MAX_TRACKS 10
#define HEAT_THR_TABLE 20

// The tuning this code needs, all read from PS_CFG_DATA_Zeta (the Duo 2's
// PSDB blob in libsurfacetouch_c1.so) and derived from it as the stock
// initialisation does. Index 0 is x (the long axis), 1 is y.
struct heat_cfg {
    int cols, rows;               // 30 x 21 antennas
    float active_mm[2];           // antenna span: 117.67 x 81.15
    float size_mm[2];             // panel: 120.20 x 85.18
    int logical_max[2];           // report range along each axis: 12019, 8517
    float pitch_mm[2];            // span / (antennas - 1)  (CommonTrackingCfg +0x40)
    float offset_mm[2];           // (span - size) / 2: DetectedArray's centring offset

    // TouchDetectionCfg (PSDB 0xef4).
    float thr_min, thr_max;       // +0, +4: the scan threshold's clamp
    float strong;                 // +8: a junction that may seed or join a blob
    float peak_valid;             // +0xc: a blob of 1-2 junctions needs a peak above this
    int thr_count;                // +0x10: noise table entries
    float thr_table[HEAT_THR_TABLE];
    unsigned noise_limit[HEAT_THR_TABLE];
    float peak_eps;               // +0xcc: neighbours this close are equal
    float saddle_diag, saddle_2, saddle_knight;  // +0xd0, +0xd4, +0xd8: short saddle factors
    int saddle_long_d2;           // +0xdc: long saddles up to this squared distance
    float saddle_long;            // +0xe0
    unsigned max_junctions;       // +0xb4: a blob this big is an error
    float topo_floor;             // +0xbc: topography levels start here...
    float topo_level[3];          // +0xe8: ...and go this far towards the peak
    float topo_base;              // +0xf4
    float halo_fraction;          // +0xf8: halo junctions are below this part of the peak
    float halo_ring[2];           // +0xfc, +0x100: and at least this part of their inner neighbour
    unsigned big_blob;            // +0xe80: a blob this big is a palm
    float border_margin;          // PSDB 0xc94 + 4: "near the border" in antennas
    float radius_factor;          // PSDB 0x1da0 + 4

    // StretchingConfig (TouchPostProcessingCfg 0x870 + 0x22c).
    int stretch_on;
    float stretch[2][4];          // per axis: low zone, low knee, high zone, high knee

    // Panel alignment: PanelAlignmentReaderBase's clamp (ConfigurationDataBase
    // +0x104, +0x10b) and the unit's own offset (heat_cfg_load_alignment).
    float align_limit;
    int align_enabled;
    float align_mm;               // added to DetectedArray's x offset
};

int heat_cfg_load(struct heat_cfg *cfg, const char *library, char *err, size_t errlen);
// The unit's two-panel alignment (sfpd display/PixelAlignmentData.bin, kept by
// the stock service as TwoPanelAlignment.bin). Missing or unusable: 0.
int heat_cfg_load_alignment(struct heat_cfg *cfg, const char *file, char *err, size_t errlen);

// One heat frame from a panel, as the digitizer sends it (report 0x0A or 0x3C).
struct heat_frame {
    uint16_t seq;
    int have_image;
    uint8_t image[HEAT_MAX_Y][HEAT_MAX_X];  // [y][x]
    unsigned noise;               // the frequency-noise level CclLogic picks a threshold by
};

int heat_parse_report(const struct heat_cfg *cfg, const uint8_t *r, size_t n, struct heat_frame *f);

// TouchDetection's blob record (BlobFeatureStruct, 0xb0 bytes in the stock
// library), the fields this code uses. Offsets are the stock ones.
struct heat_blob {
    float sw, sw2, swx, swy;      // 0x00: sums of the weight (magnitude above the threshold)
    float sm, sm2, smx, smy;      // 0x10: sums of the magnitude
    float x, y;                   // 0x24: centroid, antennas
    float radius;                 // 0x2c: mm
    uint16_t minx, miny, maxx, maxy;  // 0x30: bounding box
    uint16_t count, max_count;    // 0x38, 0x3a: junctions
    uint16_t index;               // 0x3c
    uint16_t orientation;         // 0x3e: centi-degrees
    uint8_t status;               // 0x40: 1 valid, 4 unused/merged, 5 dropped
    uint8_t label;                // 0x41
    uint8_t src_label;            // 0x42
    uint8_t big;                  // 0x43: a palm-sized blob
    uint32_t border;              // 0x44: 0 none, 1/3 x edges, 2/4 y edges, 5 corner
    uint8_t near_border;          // 0x48
    uint8_t on_border;            // 0x49
    uint8_t on_corner;            // 0x4a
    uint8_t on_hinge;             // 0x4b
    uint8_t peak;                 // 0x4c: smallest byte, the strongest junction
    uint8_t npeaks, nvalid_peaks; // 0x4d, 0x4e
    struct { uint16_t x, y; uint8_t valid; } peaks[HEAT_MAX_PEAKS];  // 0x50
    float halo_sum;               // 0x20
    float ratio, spread, halo;    // 0x8c, 0x90, 0x94
    uint8_t topo[6];              // 0x98: sub-blobs and junctions at three levels
    uint16_t count9e;             // 0x9e
};

// One panel's detector state (TouchDetection).
struct heat_detector {
    uint8_t label[HEAT_MAX_Y][HEAT_MAX_X];  // +0: CCL labels
    uint8_t eq[0x2d1];            // +0xf0fa: label equivalences
    struct heat_blob blob[HEAT_MAX_BLOBS];  // +0xca58
    int nrecords;                 // records in use (CclAlgorithmInput +0x3c, plus one)
    int nblobs;                   // +0xf3de: valid blobs
    float thr;                    // +0xc578: this frame's threshold
};

// CclLogic + FeatureExtraction: the blob records for one frame. Returns the
// number of valid blobs (status 1), or -1 when the stock code gives up on the
// frame.
int heat_detect(const struct heat_cfg *cfg, struct heat_detector *d, const struct heat_frame *f);
// The records in the probe format (stock-ref/heatprobe.cpp "P blob").
void heat_dump_blobs(FILE *out, const struct heat_detector *d, const struct heat_frame *f);

// A detected touch handed to tracking, in the panel's millimetres.
struct heat_finger {
    float x, y;                   // mm after offset, alignment and stretching
    float ax, ay;                 // centroid, antennas
    float radius;                 // mm
};

// DetectedArray::ProcessDetectedBlobs for one blob: antennas to millimetres,
// offset and alignment, edge stretching, crop. Returns 0 if dropped.
int heat_to_mm(const struct heat_cfg *cfg, float ax, float ay, float radius, struct heat_finger *out);

// Contacts as the panel reports them, in its logical units (x across the
// panel's short side, as the OS report has it).
struct heat_contact {
    int id;                       // tracking id, stable while touching
    int x, y;
    int major, minor;
};

struct heat_track {
    int active, id;
    float x, y, vx, vy, radius;   // mm, x along the long axis
    unsigned frames;
};

struct heat_tracker {
    struct heat_track t[HEAT_MAX_TRACKS];
    int next_id;
};

int heat_track(const struct heat_cfg *cfg, struct heat_tracker *tr, const struct heat_finger *b, int nb,
               struct heat_contact *out, int max);
void heat_track_reset(struct heat_tracker *tr);

#endif
