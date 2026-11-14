// Surface Duo 2 touch: heat frames to contacts, as Microsoft's heat processor
// (libsurfacetouch_c1.so, the Duo 2 "Zeta" build) computes them.
//
// See docs/touch.md for the stock functions each step reproduces.

#ifndef DUO2_HEAT_H
#define DUO2_HEAT_H

#include <stdint.h>
#include <stddef.h>

#define HEAT_MAX_ROWS 32          // antennas along a panel's long axis (30 here)
#define HEAT_MAX_COLS 32          // antennas along its short axis (21 here)
#define HEAT_MAX_BLOBS 48         // TouchDetection keeps 48 blob records
#define HEAT_MAX_TRACKS 10
#define HEAT_THR_TABLE 20

// The tuning this code needs from PS_CFG_DATA_Zeta, the Duo 2's PSDB blob in
// libsurfacetouch_c1.so. Offsets are from the blob's start.
struct heat_cfg {
    // Panel geometry (region record at 0x3c; both panels share it).
    int cols, rows;               // 21 along x (short axis), 30 along y
    float active_mm[2];           // antenna span: 81.15 x 117.67 mm
    float size_mm[2];             // panel: 85.18 x 120.20 mm
    int logical_max[2];           // report range: 0..8517, 0..12019
    float pitch_mm[2];            // active span / (antennas - 1)
    float offset_mm[2];           // (size - active) / 2: the antennas are centred

    // TouchDetectionCfg (0xef4).
    float thr_min, thr_max;       // [0], [1]: the scan threshold's clamp
    float strong;                 // [2]: a junction that may seed or join a blob
    float peak_valid;             // [3]: a small blob's peak must exceed this
    int thr_count;                // [4]: noise table entries
    float thr_table[HEAT_THR_TABLE];
    unsigned noise_limit[HEAT_THR_TABLE];
    float radius_factor;          // +0xeac+4

    // StretchingConfig (TouchPostProcessingCfg 0x870 + 0x22c).
    int stretch_on;
    float stretch[2][4];          // per axis: low zone, low param, high zone, high param
};

int heat_cfg_load(struct heat_cfg *cfg, const char *library, char *err, size_t errlen);

// One heat frame from a panel, as the digitizer sends it (report 0x0A or 0x3C).
struct heat_frame {
    uint16_t seq;
    int have_image;
    uint8_t image[HEAT_MAX_ROWS * HEAT_MAX_COLS];  // [y][x], rows x cols
    unsigned noise;               // max of the frequency-noise section's values
};

int heat_parse_report(const struct heat_cfg *cfg, const uint8_t *r, size_t n, struct heat_frame *f);

// A detected touch, in the panel's millimetres (x along the short axis).
struct heat_blob {
    float x, y;                   // centroid, mm, after offset and stretching
    float ax, ay;                 // centroid in antenna units
    float radius;                 // mm (SetBlobRadius)
    float peak;                   // strongest junction's magnitude
    int count;                    // junctions
};

int heat_detect(const struct heat_cfg *cfg, const struct heat_frame *f, struct heat_blob *out, int max);

// Contacts as the panel reports them, in its logical units.
struct heat_contact {
    int id;                       // tracking id, stable while touching
    int x, y;                     // 0..logical_max
    int major, minor;             // logical units
};

struct heat_track {
    int active, id;
    float x, y, vx, vy, radius;
    unsigned frames;
};

struct heat_tracker {
    struct heat_track t[HEAT_MAX_TRACKS];
    int next_id;
};

int heat_track(const struct heat_cfg *cfg, struct heat_tracker *tr, const struct heat_blob *b, int nb,
               struct heat_contact *out, int max);
void heat_track_reset(struct heat_tracker *tr);

#endif
