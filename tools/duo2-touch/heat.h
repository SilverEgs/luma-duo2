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
    unsigned big_blob_palm;       // +0xe82: ...or this big while a palm is about
    float palm_features[10];      // +0x49c: what a palm-sized blob's features are taken to be
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
    // Both offsets as the feedback to the digitizer carries them
    // (GetPanelAlignmentOffsetsPtr: record +20, then +16, mm, clamped).
    float align_pair[2];
    int align_loaded;

    // The rest of the tuning the tracker reads (tracks.c), kept whole:
    // TouchDetectionCfg (PSDB 0xef4) and the pieces TouchDetection::Init
    // points into.
    uint8_t td[0xec0];
    float match_d2;               // PSDB 0x870: Hungarian cost cut-off (squared antennas... as the stock compares it)
    float palm_radius[3];         // PSDB 0x9ec: palm-by-proximity radius (pen, normal, posture)
    float radius_iir;             // PSDB 0x1da0: track radius smoothing weight
    float close_d2[2];            // PSDB 0xc94 + 0xc / + 0x20: blobs closer than this are "close"
    uint16_t base_byte[2];        // PSDB 0xc94 + 0x14 / + 0x28: padding floor byte
    float pad_fraction;           // PSDB 0xc94 + 8, percent: padding joins junctions above this part of the peak
    uint8_t scan_row[0x9c];       // InitScanWindowLUT (StaticConfig, PSDB 0xcd0): each row's scan window, 0xff none

    // TrackLib (tracklib.c): TouchPostProcessingCfg (PSDB 0x870) whole, the
    // StaticConfig pieces it reads, and CommonTrackingCfg's derived values.
    uint8_t tpp[0x320];
    uint8_t static_type;          // StaticConfig +0: 1 here (row LUT, not edges)
    uint8_t nwin;                 // StaticConfig +0x13c: scan windows (12)
    uint8_t row_lut[0x40];        // StaticConfig +0x1e4: antenna row -> scan index
    float ctc_w, ctc_h;           // CommonTrackingCfg +0x18, +0x1c: the panel's bounds tracks are clamped to (mm)
    float ctc_scale[2];           // +0x38, +0x3c: logical units per mm, long and short axis
};

int heat_cfg_load(struct heat_cfg *cfg, const char *library, char *err, size_t errlen);
// The unit's two-panel alignment (sfpd display/PixelAlignmentData.bin, kept by
// the stock service as TwoPanelAlignment.bin). Missing or unusable: 0.
int heat_cfg_load_alignment(struct heat_cfg *cfg, const char *file, char *err, size_t errlen);

// One heat frame from a panel, as the digitizer sends it (report 0x0A or 0x3C).
struct heat_frame {
    uint16_t seq;
    uint16_t ts;                  // the digitizer's scan counter: one per 10 ms
    int have_image;
    uint8_t image[HEAT_MAX_Y][HEAT_MAX_X];  // [y][x]
    unsigned noise;               // the frequency-noise level CclLogic picks a threshold by
    uint8_t scan[16];             // each scan window's value this frame (sub-section 50; Parser::UpdateScanTimeData)
    uint32_t scan_us;             // the frame period, us (sub-section 50; 10000 without one)
    int have_nsr;
    uint16_t nsr[16];             // each scan window's noise (sub-section 4; Parser::UpdateFrequencyNoiseData)
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
    float llr[4];                 // 0xa0: log-likelihood of each class (Classification)
};

// One panel's detector state (TouchDetection).
struct heat_detector {
    uint8_t label[HEAT_MAX_Y][HEAT_MAX_X];  // +0: CCL labels
    uint8_t eq[0x2d1];            // +0xf0fa: label equivalences
    struct heat_blob blob[HEAT_MAX_BLOBS];  // +0xca58
    int nrecords;                 // records in use (CclAlgorithmInput +0x3c, plus one)
    int nblobs;                   // +0xf3de: valid blobs
    float thr;                    // +0xc578: this frame's threshold
    int palm;                     // +0x16580: a palm was seen lately (set by the tracker)
};

// CclLogic + FeatureExtraction: the blob records for one frame. Returns the
// number of valid blobs (status 1), or -1 when the stock code gives up on the
// frame.
int heat_detect(const struct heat_cfg *cfg, struct heat_detector *d, const struct heat_frame *f);
// The records in the probe format (stock-ref/heatprobe.cpp "P blob").
void heat_dump_blobs(FILE *out, const struct heat_detector *d, const struct heat_frame *f);

// The magnitude of each heat byte (TouchDetection::Init's table).
extern float heat_lut[256];

// ---------------------------------------------------------------------------
// TouchDetection's tracker (tracks.c): blobs to tracks, the classifier, and
// the records handed on to DetectedArray. The track record keeps the stock
// layout (TrackDataStruct, 0x25c bytes), because the stock code reads parts
// of a record left from its previous use, and so does this.

struct heat_hist {                // one frame of a track's history (0x30 bytes)
    float x, y;                   // 0x00
    float llr[4];                 // 0x08
    uint32_t type;                // 0x18: the class this frame
    uint16_t count;               // 0x1c: junctions
    uint16_t pad1e;
    float box[4];                 // 0x20: the blob's box grown by the class margin, moved by the motion
};

struct heat_trk {                 // TrackDataStruct
    float x, y;                   // 0x00: antennas
    float llr[4];                 // 0x08
    float radius;                 // 0x18: mm, smoothed
    float vx, vy;                 // 0x1c: motion over the last frame
    uint16_t minx, miny, maxx, maxy;  // 0x24
    uint16_t frames;              // 0x2c
    uint16_t count;               // 0x2e
    uint16_t blob;                // 0x30: record index of this frame's blob
    uint16_t orientation;         // 0x32
    uint16_t reported;            // 0x34: frames in a row handed on
    uint8_t peak;                 // 0x36: strongest byte seen
    uint8_t label;                // 0x37
    uint8_t pad38[4];
    uint32_t status;              // 0x3c: 0 free, 1 active, 2 suspended, 3 closing, 4 held
    uint8_t close;                // 0x40: near another blob
    uint8_t hold;                 // 0x41: frames left to hold or replay
    uint8_t single;               // 0x42: alone since it started
    uint8_t near_border;          // 0x43
    uint8_t one_junction;         // 0x44
    uint8_t palm_hold;            // 0x45
    uint8_t suspended;            // 0x46
    uint8_t matched;              // 0x47
    int32_t otfc;                 // 0x48
    uint8_t otfc_count;           // 0x4c
    uint8_t pad4d[3];
    float peaks_sum;              // 0x50
    float peaks_max;              // 0x54
    uint8_t grip;                 // 0x58
    uint8_t pad59[3];
    int32_t grip_count;           // 0x5c
    struct heat_hist h[10];       // 0x60: ring, current at ring
    float min_x, min_y, max_x, max_y;  // 0x240: extent travelled
    uint16_t run_undecided;       // 0x250
    uint16_t run_decided;         // 0x252
    uint16_t max_count;           // 0x254
    uint8_t on_border;            // 0x256
    uint8_t on_corner;            // 0x257
    uint8_t off_border;           // 0x258
    uint8_t ring;                 // 0x259
    uint8_t pen;                  // 0x25a
    uint8_t pad25b;
};

#define HEAT_TRACKS 48
#define HEAT_REPORTS 0x22
#define HEAT_PALM_MAP 0x48

// ReportInformation (0x34 bytes): what TouchDetection hands to DetectedArray.
struct heat_report {
    float x, y;                   // 0x00: antennas
    float ax, ay;                 // 0x08: (filled by DetectedArray)
    float radius;                 // 0x10: mm
    uint32_t peak;                // 0x14: byte
    uint16_t minx, miny, maxx, maxy;  // 0x18
    uint16_t track_count;         // 0x20
    uint16_t count;               // 0x22
    uint16_t orientation;         // 0x24
    uint8_t type;                 // 0x26: 1 a blob, 3 one of a blob's peaks
    uint8_t label, label2;        // 0x27
    uint8_t strong;               // 0x29
    uint8_t window;               // 0x2a
    uint8_t r2b;                  // 0x2b
    uint8_t b2c;                  // 0x2c
    uint8_t valid;                // 0x2d
    uint8_t split;                // 0x2e
    uint8_t track;                // 0x2f
    uint8_t live;                 // 0x30
    uint8_t peaks;                // 0x31
    uint8_t near_border;          // 0x32
    uint8_t pad33;
};

struct heat_tracks {
    struct heat_trk t[HEAT_TRACKS];   // +0xf3e0
    uint8_t nactive;              // +0x16520: tracks active or suspended
    uint8_t nlive;                // +0x16521: tracks not free
    uint16_t last_ts;             // +0x1662e
    uint8_t prev_nblobs;          // +0xf3df
    uint8_t noisy;                // +0x16534
    uint16_t palm_left;           // +0x1657a
    uint16_t now;                 // +0x1657c
    uint16_t palm_ts;             // +0x1657e
    uint8_t palm;                 // +0x16580
    uint8_t npalms;               // +0x16581
    uint8_t nlist;                // +0x16582
    uint8_t list[HEAT_TRACKS + 2];    // +0x1654a
    uint8_t palm_level;           // +0x1652f
    uint16_t palm_map_ts;         // +0x16630
    uint8_t palm_map[39][HEAT_PALM_MAP];   // +0xaf80
    uint8_t palm_pen[39][HEAT_PALM_MAP];   // +0xba78
    uint8_t tap;                  // +0x19638
    uint8_t closepairs[0x2d0][2]; // +0xeb58
    struct heat_report out[HEAT_REPORTS];
    int nout;
    int nheld;                    // ProtectedBlobData +2
    uint16_t noise_level;         // this frame's noise (CclLogic's)
    uint8_t cur_nblobs;           // this frame's valid blobs
    uint8_t scan[16];             // this frame's scan window values (TouchDetection +0xca28)
    int failed;                   // detection gave up on this frame (ProcessTouch failure)
    uint32_t scan_us;             // this frame's period (TouchProcessingInput +0xd818)
    uint16_t nsr[16];             // the last scan windows' noise (TouchProcessingInput +0xd828, kept until replaced)
};

// Hungarian::HungarianInitBeforeRun and RunHungarian (tracks.c), shared by
// TouchDetection's tracker and TrackLib's separation groups: rows are
// tracks, columns blobs, both whole millimetres; a pair further apart than
// sqrt(maxd) costs 100, padding 1000. assign[row * n + col] is 1 for the
// chosen pairs (none when the stock check of the solution fails).
#define HEAT_HN 48
struct hungarian {
    int n;
    int16_t cost[HEAT_HN * HEAT_HN];
    uint8_t assign[HEAT_HN * HEAT_HN];
};
void hungarian_init(struct hungarian *h, const uint16_t (*tp)[2], int nt, const uint16_t (*bp)[2], int nb, uint16_t maxd);
void hungarian_run(struct hungarian *h);

void heat_tracks_init(const struct heat_cfg *cfg, struct heat_tracks *t);
// One frame, as TouchDetection::ProcessTouch: detection (heat_detect),
// classification, tracking and the records for DetectedArray (t->out).
// Returns the number of records.
int heat_tracks_frame(const struct heat_cfg *cfg, struct heat_tracks *t, struct heat_detector *d,
                      const struct heat_frame *f);
void heat_dump_tracks(FILE *out, const struct heat_tracks *t);

// ---------------------------------------------------------------------------
// TrackLib (tracklib.c): DetectedArray, MatchFingers/TracksArray and the
// TrackedFinger filters, as the stock library runs them on each panel, up to
// TracksArray::FillOsReport. A TrackedFinger keeps the stock 0x1f0-byte
// layout (the code reads parts of a record left from its previous use).

#define TL_FINGERS 20
#define TL_REPORTS 10

struct tl_finger { uint8_t b[0x1f0]; };

struct tl_entry {                 // one OS report entry (0x18 bytes in the stock report)
    float x, y, w, h;             // logical units; x across the panel's short side
    uint16_t id, orientation;
    uint8_t lift;                 // the contact ends with this entry
    uint8_t lost;                 // closed because its blob lost its live flag
};

struct tracklib {
    struct tl_finger f[TL_FINGERS];
    int head;                     // TracksArray list, -1 empty
    int next[TL_FINGERS];         // TrackedFinger +0x190 as indexes
    int deque_n[TL_FINGERS];      // backward-prediction queue (TrackedFinger +0x110)
    float deque[TL_FINGERS][8][2];
    uint8_t nactive, nmatched;    // TracksArray +0x10, +0x11
    float dt;                     // +0x14
    uint8_t fast, fast_half;      // +0x18, +0x19
    uint16_t next_id;             // +0x1a
    uint8_t edge_lo, edge_hi;     // +0x2708, +0x2709
    uint8_t mode, prev_mode;      // +0x2718, +0x2719
    // TrackLib
    uint16_t last_counter;        // +0x2e68 (CloseOldTracks)
    uint8_t irr_count, irr_prev;  // +0x2eae, +0x2eaf
    uint32_t irr_last;            // +0x2eb0 (PrepareIrregularEventForTrackLib)
    // TuProcessInput's frame fields the stock code keeps between frames.
    uint32_t pi_dt;               // +8: microseconds since the last frame, as tracking takes it
    uint8_t pi_code;              // +0xc
    // SepFingerArray (TrackLib +0xb8): this frame's separation groups, each
    // the records of one split blob: [0] its label, [1] records, [2] tracks
    // matched to it.
    uint8_t sep[15][3];
    uint8_t nsep;                 // +0x2d
    float sep_dt;                 // +0x58
    // This frame's report.
    struct heat_report det[HEAT_REPORTS];
    int ndet;
    struct tl_entry out[TL_REPORTS];
    int nout;
};

void tracklib_init(struct tracklib *tl);
// One frame, after heat_tracks_frame: TouchProcessingManager's irregular
// event handling and TrackLib::RunTrackingProcess. Fills tl->out.
int tracklib_frame(const struct heat_cfg *c, struct tracklib *tl, const struct heat_tracks *s, uint16_t counter);
// The state in stock-ref/heatprobe.cpp's format ("P tf", "P osrep").
void tracklib_dump(FILE *out, const struct tracklib *tl);

// A detected touch handed to tracking, in the panel's millimetres.
struct heat_finger {
    float x, y;                   // mm after offset, alignment and stretching
    float ax, ay;                 // centroid, antennas
    float radius;                 // mm
};

// DetectedArray::ProcessDetectedBlobs for one blob: antennas to millimetres,
// offset and alignment and edge stretching (valid records only), crop.
// Returns 0 if dropped.
int heat_to_mm(const struct heat_cfg *cfg, float ax, float ay, float radius, int valid, struct heat_finger *out);

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
