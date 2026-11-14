// Surface Duo 2 touch: TrackLib, the stock heat processor's tracker, from the
// records TouchDetection hands on (heat_report) to the OS report.
//
// Reproduces, per panel and in the stock order:
//   ProjectTuningMngr::TrackingInit   TrackLib::CloseOldTracks,
//                                     PrepareIrregularEventForTrackLib,
//                                     IrregularEventHistoryHandling
//   TrackLib::RunTrackingProcess      InitTrackingProcess, the irregular event
//                                     handler, DetectedArray (heat_to_mm and
//                                     DeleteWeakBlobWithPoorNsr),
//                                     MatchFingers::PrepareTracking/RunMatching,
//                                     SepFingerArray (split blobs),
//                                     TracksArray and TrackedFinger, up to
//                                     TracksArray::FillOsReport.
//
// A TrackedFinger is kept as the stock 0x1f0-byte record and read by offset,
// because the stock code reads fields left over from a slot's previous track
// and the probe (stock-ref/heatprobe.cpp "P tf") compares the bytes. The
// offsets are the stock ones; each is named where it is used.
//
// Not reproduced: the pen ("touch source" 1), the virtual touchpad and
// split display mode (off in this tuning).

#include <float.h>
#include <math.h>
#include <string.h>

#include "heat.h"

// TouchPostProcessingCfg (PSDB 0x870) sub-configs the TrackedFinger statics
// point into (TrackedFinger::mp*).
#define K_   0x004    // mpKalmanConfig / mpVelocitiesConfig
#define PS2_ 0x0a8    // mpProcessStd2: process noise per velocity index (u32)
#define SF_  0x0d0    // mpSmallFingerCfg
#define J_   0x0dc    // mpJitterRestrictorConfig
#define M_   0x190    // mpMatching
#define P_   0x1ec    // mpPrediction
#define P0_  0x1fc    // mpPixel0Cfg
#define BSS_ 0x2bc    // mpBlobSizeSmoothConfig
#define FM_  0x2c4    // mpFastMovement
#define IIR_ 0x2d0    // mpIirCfg
#define NOISE_THR 0x1f0   // TrackLib +0x2eaa: any scan window at this is high noise
#define EDGE_THR 0x2b0    // TrackLib +0x2eac: the first/last scan window at this
#define WEAK_NSR 0x1c8    // DetectedArray +0x38


static float cf(const struct heat_cfg *c, int o) { float v; memcpy(&v, c->tpp + o, 4); return v; }
static uint8_t cb(const struct heat_cfg *c, int o) { return c->tpp[o]; }
static uint16_t ch(const struct heat_cfg *c, int o) { return (uint16_t)(c->tpp[o] | c->tpp[o + 1] << 8); }
static uint32_t cu(const struct heat_cfg *c, int o) { uint32_t v; memcpy(&v, c->tpp + o, 4); return v; }
static uint16_t tdc16(const struct heat_cfg *c, int o) { return (uint16_t)(c->td[o] | c->td[o + 1] << 8); }

typedef struct tl_finger TF;
static float F(const TF *t, int o) { float v; memcpy(&v, t->b + o, 4); return v; }
static void SF(TF *t, int o, float v) { memcpy(t->b + o, &v, 4); }
static uint32_t U32(const TF *t, int o) { uint32_t v; memcpy(&v, t->b + o, 4); return v; }
static void SU32(TF *t, int o, uint32_t v) { memcpy(t->b + o, &v, 4); }
static uint16_t U16(const TF *t, int o) { uint16_t v; memcpy(&v, t->b + o, 2); return v; }
static void SU16(TF *t, int o, uint16_t v) { memcpy(t->b + o, &v, 2); }
#define B(t, o) ((t)->b[o])
#define ST(t) U32(t, 0xd4)

// Record fields (ReportInformation, struct heat_report): x/y are mm once
// DetectedArray has run, ax/ay the antenna position.
typedef struct heat_report R;

// CommonUtilities.
static float dist2(const float *a, const float *b) {
    float dx = a[0] - b[0], dy = a[1] - b[1];
    return dx * dx + dy * dy;
}
static float sqrt2(float x) {   // CommonUtilities::Sqrt2: one Newton step on the fast inverse root
    uint32_t i; memcpy(&i, &x, 4);
    i = 0x5f3759dfu - (i >> 1);
    float y; memcpy(&y, &i, 4);
    return (y * x) * (((x * -0.5f) * y) * y + 1.5f);
}
static void P2(const TF *t, int o, float *v) { v[0] = F(t, o); v[1] = F(t, o + 4); }

// The per-frame inputs (TracklibMatchingIn, TracklibPrepareTrackingIn) the
// tracker reads.
struct mi {
    uint8_t pen;          // [0]: touch source 1
    uint8_t m1;           // [1]
    uint8_t src, prev_src;// [2], [3]
    float dt;             // +0x24, seconds
    uint8_t m28;          // +0x28 (TuProcessInput +0x40)
    uint8_t palm;         // +0x2b
    uint16_t counter;     // +0x2c
};
struct pt {
    uint8_t n;            // +0x10
    float dt;             // +0x14
    uint8_t noise;        // +0x1a (TuProcessInput +0x42)
    uint8_t edge_lo, edge_hi;  // +0x1b, +0x1c
    uint8_t irregular;    // +0x1d
    uint8_t mode;         // +0x1e (DetectedArray +0x98)
};

static uint8_t g_high_noise;       // TrackedFinger::mIsHighNoise (shared by both panels, as in the stock library)

// ---------------------------------------------------------------------------
// TrackedFinger

static void set_status(TF *t, uint32_t s) {   // SetTrackStatus
    if (s == 2 && B(t, 0x10d) == 2) return;
    SU32(t, 0xd4, s);
    B(t, 0xdb) = s == 3 ? (uint8_t)(B(t, 0xdb) + 1) : 0;
}

static void dq_push(struct tracklib *tl, int i, const float *p) {
    if (tl->deque_n[i] >= 8) return;
    tl->deque[i][tl->deque_n[i]][0] = p[0];
    tl->deque[i][tl->deque_n[i]][1] = p[1];
    tl->deque_n[i]++;
}
static void dq_pop(struct tracklib *tl, int i, float *p) {
    p[0] = tl->deque[i][0][0]; p[1] = tl->deque[i][0][1];
    memmove(tl->deque[i][0], tl->deque[i][1], sizeof(float) * 2 * (size_t)(tl->deque_n[i] - 1));
    tl->deque_n[i]--;
}

static void init_new_track(const struct heat_cfg *c, const struct tracklib *tl, TF *t, R *r, uint8_t p2, uint8_t p3,
                           uint16_t id, uint8_t p6) {
    B(t, 0x155) = 1;      // (p4 == 0)
    SU32(t, 0xd4, 1);
    B(t, 0xdb) = 0;
    SU16(t, 0x158, id);
    static const int xo[] = { 0x00, 0x38, 0x20, 0x18, 0x168 };
    for (int k = 0; k < 5; k++) { SF(t, xo[k], r->x); SF(t, xo[k] + 4, r->y); }
    SF(t, 0x40, r->ax); SF(t, 0x44, r->ay);
    SF(t, 0x164, 0); B(t, 0x15a) = 0;
    memset(t->b + 0x15c, 0, 8);
    B(t, 0xd0) = 0;
    memset(t->b + 0x58, 0, 8);
    SU16(t, 0xd8, 0);
    SU32(t, 0x150, 0); SU32(t, 0x14c, 0);
    memset(t->b + 0x70, 0, 0x18);
    B(t, 0x154) = 1;
    SU16(t, 0xf2, 0);
    memset(t->b + 0x48, 0, 8);
    SF(t, 0x144, 0);
    B(t, 0xda) = 0; B(t, 0x148) = 0;
    memset(t->b + 0x60, 0, 0x10);
    memset(t->b + 0xe4, 0, 0xd);
    SU16(t, 0x8a, r->orientation);
    SF(t, 0xf4, 0); B(t, 0x104) = 0;
    SF(t, 0xf8, (float)cb(c, M_ + 0x56));
    B(t, 0x10b) = 0;
    B(t, 0x107) = 1; B(t, 0x108) = 1;
    SU32(t, 0x1d0, 0);
    B(t, 0x10d) = 0;
    B(t, 0x141) = tl->irr_prev != 0;
    SF(t, 0x50, (float)(int)((unsigned)r->maxx - r->minx + 1));
    SF(t, 0x54, (float)(int)((unsigned)r->maxy - r->miny + 1));
    B(t, 0x143) = r->window;
    int sf = SF_ + (p2 ? 7 : 4);
    uint8_t small = 1;
    if (p2 == 0) {
        float m = 1.0f - ((float)r->peak * 0.0022203543f + 0.6f);
        small = (double)m >= 0.12 ? r->count <= cb(c, SF_ + 4) : 1;
    }
    B(t, 0x142) = small;
    B(t, 0x10a) = r->count <= cb(c, sf);
    SU16(t, 0xfc, r->track_count);
    B(t, 0x10c) = 0;
    SF(t, 0x100, (float)r->track_count);
    B(t, 0x157) = p3 == 5;
    B(t, 0x1c1) = p6;
    B(t, 0x156) = r->live;
    B(t, 0x1d4) = r->near_border != 0;
    SF(t, 0x1d8, r->x); SF(t, 0x1dc, r->y); SF(t, 0x1e0, r->x); SF(t, 0x1e4, r->y);
}

static void update_velocity_and_reported(const struct heat_cfg *c, TF *t, float dt) {
    float det[2], f18[2], pred[2];
    P2(t, 0x38, det); P2(t, 0x18, f18); P2(t, 0x20, pred);
    if (ST(t) == 1) {
        float d = sqrt2(dist2(det, f18));
        float lim = cf(c, M_ + 0x28);
        // (GetPrevTouchSource is never the pen here.)
        if (det[0] == 0.0f || det[1] == 0.0f || det[0] == c->ctc_w || det[1] == c->ctc_h) lim = lim * cf(c, M_ + 0x34);
        if (g_high_noise) lim = cf(c, M_ + 0x2c);
        float vx = 0, vy = 0;
        if (lim <= d) {
            float inv = 1.0f / dt;
            vx = (det[0] - f18[0]) * inv;
            vy = (det[1] - f18[1]) * inv;
        }
        SF(t, 0x18, det[0]); SF(t, 0x1c, det[1]); SF(t, 0x20, det[0]); SF(t, 0x24, det[1]);
        SF(t, 0x58, vx); SF(t, 0x5c, vy);
        B(t, 0x10c)++;
        B(t, 0xd8) = 1;
        return;
    }
    float vx = F(t, 0x58), vy = F(t, 0x5c), x, y;
    float p8 = cf(c, P_ + 8);
    if (ST(t) == 3 && p8 <= fabsf(vx) && p8 <= fabsf(vy)) {
        x = det[0]; y = det[1];
    } else {
        x = pred[0] + F(t, 0x60) * (det[0] - pred[0]);
        y = pred[1] + F(t, 0x64) * (det[1] - pred[1]);
    }
    SF(t, 0x18, x); SF(t, 0x1c, y);
    SF(t, 0x58, vx + F(t, 0x68) * (det[0] - pred[0]));
    SF(t, 0x5c, vy + F(t, 0x6c) * (det[1] - pred[1]));
    if (x < 0.0f) { x = 0.0f; SF(t, 0x18, 0.0f); }
    if (c->ctc_w < x) SF(t, 0x18, c->ctc_w);
    if (y < 0.0f) { y = 0.0f; SF(t, 0x1c, 0.0f); }
    if (c->ctc_h < y) SF(t, 0x1c, c->ctc_h);
    if (B(t, 0x10c) < 0xfe) B(t, 0x10c)++;
    B(t, 0xd8) = 0;
}

// LineHelper (TrackedFinger::PrepareBackwardPrediction's).
struct line {
    const struct heat_cfg *c;
    float slope, icpt;
    uint8_t vertical;
    float x0;
    float a[2], b[2];
};
static void line_set(struct line *l, const float *a, const float *b) {
    l->a[0] = a[0]; l->a[1] = a[1]; l->b[0] = b[0]; l->b[1] = b[1];
    l->slope = 0; l->x0 = b[0]; l->icpt = b[1];
    if (fabsf(b[0] - a[0]) <= cf(l->c, J_ + 0x1c)) { l->vertical = 1; return; }
    if (fabsf(b[1] - a[1]) > cf(l->c, J_ + 0x20)) {
        l->slope = (b[1] - a[1]) / (b[0] - a[0]);
        l->icpt = b[1] - l->slope * b[0];
    }
}
static float clampf0(float v, float hi) { float m = fmaxf(v, 0.0f); return hi < m ? hi : m; }
static void line_border(const struct line *l, unsigned dir, float *o) {
    const float W = l->c->ctc_w, H = l->c->ctc_h;
    if (dir == 1 || dir == 4) {
        float x = dir == 1 ? 0.0f : W;
        if (l->vertical) { o[0] = x; o[1] = l->icpt; return; }
        float y = x * l->slope + l->icpt;
        if (y < 0.0f || y > H) { y = clampf0(y, H); x = (y - l->icpt) / l->slope; }
        o[0] = x; o[1] = y;
        return;
    }
    float y = dir == 2 ? 0.0f : H;
    if (l->vertical || l->slope == 0.0f) { o[0] = l->x0; o[1] = y; return; }
    float x = (y - l->icpt) / l->slope;
    if (x < 0.0f || x > W) { x = clampf0(x, W); y = l->icpt + l->slope * x; }
    o[0] = x; o[1] = y;
}
static void line_points(const struct line *l, struct tracklib *tl, int i, int k) {
    float p[2] = { l->a[0], l->a[1] }, n = (float)(k + 1);
    for (int j = 0; j < k; j++) {
        p[0] = p[0] + (l->b[0] - l->a[0]) / n;
        p[1] = p[1] + (l->b[1] - l->a[1]) / n;
        dq_push(tl, i, p);
    }
}

static float border_threshold(const struct heat_cfg *c, const struct tracklib *tl, const TF *t, const struct mi *mi) {
    float base = (mi->pen == 1 || mi->prev_src == 1) ? cf(c, P0_ + 8) : cf(c, P0_ + 4);
    if (B(t, 0x157) == 1) base = cf(c, P0_ + 0xc);
    unsigned irr = tl->irr_prev;
    if (irr >= 2) base = base + cf(c, P0_ + 0x20) * (float)irr;
    else if (B(t, 0x141) == 1) base = base + cf(c, P0_ + 0x20);
    if (B(t, 0x1c1) == 1) {
        float ax = fabsf(F(t, 0x38) - F(t, 0x28)), ay = fabsf(F(t, 0x3c) - F(t, 0x2c));
        base = (ax > ay ? ax : ay) + base;
    }
    return base;
}

static unsigned backward_direction(const struct heat_cfg *c, const TF *t) {
    float a = cf(c, P0_ + 0x14), b = cf(c, P0_ + 0x18);
    uint8_t e = B(t, 0x10e), small = B(t, 0x142);
    for (int ax = 0; ax < 2; ax++) {
        int o = ax * 4;
        float d1 = F(t, 0x38 + o) - F(t, 0x28 + o), d2 = F(t, 0x28 + o) - F(t, 0x30 + o), d3 = F(t, 0x38 + o) - F(t, 0x30 + o);
        unsigned lo = ax ? 2 : 1, hi = ax ? 8 : 4;
        if (e & lo) {
            if (d1 >= a && d2 >= a) return lo;
            if (d3 >= b && !small) return lo;
        }
        if (e & hi) {
            if (-d1 >= a && -d2 >= a) return hi;
            if (-d3 >= b && !small) return hi;
        }
    }
    return 0;
}

static void prepare_backward(const struct heat_cfg *c, struct tracklib *tl, int i, const struct mi *mi) {
    TF *t = &tl->f[i];
    struct line l = { .c = c };
    const float W = c->ctc_w, H = c->ctc_h;
    if (B(t, 0x10d) != 0) {
        unsigned dir = backward_direction(c, t);
        if (!dir) { B(t, 0x10d) = 3; return; }
        float p1[2], p0[2], bp[2];
        P2(t, 0x28, p1); P2(t, 0x30, p0);
        B(t, 0x10d) = 1; B(t, 0x10f) = 1;
        line_set(&l, p0, p1);
        line_border(&l, dir, bp);
        dq_push(tl, i, bp);
        float s = sqrtf((p1[0] - p0[0]) * (p1[0] - p0[0]) + (p1[1] - p0[1]) * (p1[1] - p0[1]));
        float d = sqrtf((p0[0] - bp[0]) * (p0[0] - bp[0]) + (p0[1] - bp[1]) * (p0[1] - bp[1]));
        int n = (int)(d / s), k = n > 3 ? 2 : n - 1;
        if (k >= 1) {
            line_set(&l, bp, p0);
            line_points(&l, tl, i, k);
        }
        dq_push(tl, i, p0);
        dq_push(tl, i, p1);
        return;
    }
    float det[2], prev[2];
    P2(t, 0x38, det); P2(t, 0x28, prev);
    int border;
    if (mi->pen == 0 && (W == det[0] || det[1] == 0.0f || det[0] == 0.0f || H == det[1])) { B(t, 0x10d) = 3; return; }
    if (mi->prev_src == 1) border = 1;
    else border = prev[0] > 0.0f && W > prev[0] && prev[1] > 0.0f && H > prev[1];
    if (!border) {
        dq_push(tl, i, prev);
        B(t, 0x10d) = 1; B(t, 0x10f) = 0;
        return;
    }
    float th = border_threshold(c, tl, t, mi);
    float dx = det[0] - prev[0], dy = det[1] - prev[1];
    uint8_t bits = 0;
    if (prev[0] <= th + dx) bits |= 1;
    if (W - prev[0] <= th - dx) bits |= 4;
    if (prev[1] <= th + dy) bits |= 2;
    if (H - prev[1] <= th - dy) bits |= 8;
    B(t, 0x10e) = bits;
    B(t, 0x10d) = bits ? 2 : 3;
}

static void update_match_limit(const struct heat_cfg *c, TF *t, float dspeed, float speed, uint8_t src) {
    float b7c = F(t, 0x7c), L;
    if (b7c <= (float)cb(c, M_ + 8)) L = (float)cb(c, M_ + 9);
    else if (b7c <= (float)cb(c, M_ + 0xa)) L = (float)cb(c, M_ + 0xb);
    else if (b7c <= (float)cb(c, M_ + 0xc)) L = (float)cb(c, M_ + 0xd);
    else if ((float)cb(c, M_ + 0xe) < b7c) L = cf(c, M_ + 0x40);
    else L = (float)cb(c, M_ + 0xf);
    L = L + speed;
    if (B(t, 0x107) == 3) L = L + cf(c, M_ + 0x50);
    uint32_t st = ST(t);
    if (st == 1) {
        L = L + cf(c, M_ + 0x44);
        if (F(t, 0x58) == 0.0f && F(t, 0x5c) == 0.0f) L = L + cf(c, g_high_noise == 1 ? M_ + 0x2c : M_ + 0x28);
    }
    float ad = fabsf(dspeed);
    if (cf(c, M_ + 0x24) <= ad) L = ad * 3.0f + L;
    if (src == 0) {
        if (cf(c, M_ + 0x1c) < F(t, 0x4c)) L = L + cf(c, M_ + 0x20);
        if (g_high_noise == 1) L = L + cf(c, M_ + 0x30);
    }
    float px = F(t, 0x28), py = F(t, 0x2c);
    int prev_edge = px == 0.0f || py == 0.0f || px == c->ctc_w || py == c->ctc_h;
    if (B(t, 0x10d) != 3) B(t, 0x140) = cb(c, P0_ + 0x2c);
    if (st == 1 && prev_edge) B(t, 0x140) = cb(c, P0_ + 0x2c);
    if (B(t, 0x140) != 0) {
        L = L + cf(c, P0_ + 0x1c);
        if (st == 1 || !prev_edge) B(t, 0x140)--;
    }
    float mx = (float)ch(c, M_ + 0x54);
    if (mx < L) L = mx;
    float mn = (float)cb(c, M_ + 0x56);
    if (L < mn) {
        uint8_t n = F(t, 0x100) <= 9.0f ? cb(c, M_ + 0x5b) : (uint8_t)(cb(c, M_ + 0x5b) + 0xf);
        if (U32(t, 0x150) < n) L = mn;
    }
    SF(t, 0xf8, L);
}

static void set_dirs(TF *t, const struct heat_cfg *c, const float *p, float dx, float dy) {
    float j98 = cf(c, J_ + 0x98);
    float hx = p[4] * 0.5f, hy = p[5] * 0.5f;
    if (hx < j98) hx = j98;
    if (hy < j98) hy = j98;
    if (hx < fabsf(dx)) { if (dx > 0.0f) { B(t, 0xed) = 1; B(t, 0xef) = 0; } else { B(t, 0xef) = 1; B(t, 0xed) = 0; } }
    else { B(t, 0xef) = 0; B(t, 0xed) = 0; }
    if (hy < fabsf(dy)) { if (dy > 0.0f) { B(t, 0xee) = 1; B(t, 0xf0) = 0; } else { B(t, 0xf0) = 1; B(t, 0xee) = 0; } }
    else { B(t, 0xee) = 0; B(t, 0xf0) = 0; }
}

static void jitter_restrictor(const struct heat_cfg *c, TF *t, uint8_t src) {
    // The posture flags at GetRegionIdx(det) select the base config: the
    // Duo 2's region index is 1, whose flag is 0, so J+0xc.
    float p[6];
    for (int k = 0; k < 6; k++) p[k] = cf(c, J_ + 0xc + (src ? 0x20 : 0) + 4 * k);
    float nx = F(t, 0x160), ny = F(t, 0x164), ox = F(t, 0), oy = F(t, 4);
    float dx = nx - ox, dy = ny - oy, adx = fabsf(dx), ady = fabsf(dy);
    uint32_t s = U32(t, 0xe4);
    int w;                          // 2: moved (f2 counts), else f2 = 0
    if (s == 0) {
        uint32_t e8 = U32(t, 0xe8) + 1;
        SU32(t, 0xe8, e8);
        if (adx > p[0] || ady > p[1]) {
            SU32(t, 0xe4, 2); set_dirs(t, c, p, dx, dy);
            SF(t, 0, nx); SF(t, 4, ny);
            if (e8 >= 9) { SU32(t, 0xe4, 1); w = 0; } else w = 2;
        } else {
            if (e8 >= 9) SU32(t, 0xe4, 1);
            w = 0;
        }
    } else if (s == 1) {
        if (adx > p[2] || ady > p[3]) {
            SU32(t, 0xe4, 2); set_dirs(t, c, p, dx, dy);
            SF(t, 0, nx); SF(t, 4, ny);
            w = 2;
        } else w = 0;
    } else if (adx > p[4] || ady > p[5]) {
        if (s == 3) SU32(t, 0xe4, 2);
        set_dirs(t, c, p, dx, dy);
        SF(t, 0, nx); SF(t, 4, ny);
        w = 2;
    } else {
        if (s == 2) {
            SF(t, 0xdc, ox); SF(t, 0xe0, oy);
            SU32(t, 0xe4, 3); B(t, 0xec) = 1;
            w = 3;
        } else {
            float ddx = nx - F(t, 0xdc), ddy = ny - F(t, 0xe0);
            if (fabsf(ddx) > p[4] || fabsf(ddy) > p[5]) {
                B(t, 0xec) = 0; SU32(t, 0xe4, 2);
                set_dirs(t, c, p, ddx, ddy);
                w = 2;
            } else {
                B(t, 0xec)++;
                w = 3;
                if (cb(c, J_ + 0x9c) == B(t, 0xec)) { SU32(t, 0xe4, 1); w = 1; }
            }
        }
        if (!(dx > 0.0f ? B(t, 0xef) : B(t, 0xed))) SF(t, 0, nx);
        if (!(dy > 0.0f ? B(t, 0xf0) : B(t, 0xee))) SF(t, 4, ny);
    }
    SU16(t, 0xf2, w == 2 ? (uint16_t)(U16(t, 0xf2) + 1) : 0);
}

static void kalman_update(const struct heat_cfg *c, TF *t, const struct mi *mi, const uint16_t *scan) {
    float dt = mi->dt;
    // UpdateVelocityBucket
    uint16_t win = ch(c, K_ + 8);
    uint8_t n = (uint8_t)(B(t, 0xd9) + 1);
    B(t, 0xd9) = n;
    if ((uint8_t)(win >> 1) == n) SF(t, 0x74, 0);
    if (win == n) { n = 0; SF(t, 0x78, 0); B(t, 0xd9) = 0; }
    float v = F(t, 0x48), a = F(t, 0x74), b = F(t, 0x78);
    if (a < v) { SF(t, 0x74, v); a = v; }
    if (b < v) { SF(t, 0x78, v); b = v; }
    SF(t, 0x70, (uint8_t)(win >> 1) <= n ? b : a);
    // GetNsr
    uint16_t nsr = mi->pen == 1 ? ch(c, K_ + 0x3c) : scan[c->row_lut[(unsigned)(int)(F(t, 0x44) + 0.5f) & 0x3f]];
    SU16(t, 0x88, nsr);
    // FindVelocityAndSnrIndexes
    uint8_t vi = 9;
    float f70 = F(t, 0x70);
    for (int k = 0; k < 10; k++)
        if (f70 <= (float)ch(c, K_ + 0x10 + 2 * k)) { vi = (uint8_t)k; break; }
    uint8_t si = 9;
    if (mi->pen == 0)
        for (int k = 0; k < 10; k++)
            if (nsr <= ch(c, K_ + 0x28 + 2 * k)) { si = (uint8_t)k; break; }
    B(t, 0x8c) = vi; B(t, 0x8d) = si;
    float q = (float)cu(c, PS2_ + 4 * vi);
    if (B(t, 0xda) != 1) {
        float dt2 = dt * dt;
        float a0 = dt2 * dt2 * 0.25f * 10.0f * q;
        float a1 = dt * dt2 * 0.5f * 10.0f;
        for (int ax = 0; ax < 2; ax++) {
            SF(t, 0xb0 + ax * 0x10, a0);
            SF(t, 0xb4 + ax * 0x10, a1 * q);
            SF(t, 0xb8 + ax * 0x10, a1 * q);
            SF(t, 0xbc + ax * 0x10, dt2 * 10.0f * q);
        }
        B(t, 0xda) = 1;
    }
    for (int ax = 0; ax < 2; ax++) {
        int o = ax * 0x10;
        float c0 = F(t, 0xb0 + o), c1 = F(t, 0xb4 + o), c2 = F(t, 0xb8 + o), c3 = F(t, 0xbc + o);
        float dt2 = dt * dt;
        float p0 = (dt2 * c3 + (c0 + (c2 + c1) * dt)) + dt2 * dt2 * 0.25f * q;
        float h = dt2 * dt * 0.5f;
        float p2 = (c2 + c3 * dt) + h * q;
        float p1 = (c1 + c3 * dt) + h * q;
        float p3 = c3 + dt2 * q;
        SF(t, 0x90 + o, p0); SF(t, 0x94 + o, p1); SF(t, 0x98 + o, p2); SF(t, 0x9c + o, p3);
        float R;
        if (mi->pen == 1) R = cf(c, K_ + 0x98);
        else {
            R = cf(c, K_ + ax * 0x28 + si * 4 + 0x48);
            if (B(t, 0x107) == 3 && R < cf(c, K_ + 0x9c)) R = cf(c, K_ + 0x9c);
        }
        float ga = p0 / (p0 + R), gb = p2 / (R + p0);
        SF(t, 0x60 + 4 * ax, ga);
        SF(t, 0x68 + 4 * ax, gb);
        float g = 1.0f - ga;
        SF(t, 0xb0 + o, g * p0);
        SF(t, 0xb4 + o, g * p1);
        SF(t, 0xb8 + o, p2 - p0 * gb);
        SF(t, 0xbc + o, p3 - p1 * gb);
    }
}

static void drop_report_logic(const struct heat_cfg *c, TF *t, const struct mi *mi, const R *r) {
    B(t, 0x155) = 1;
    int ok;
    if ((g_high_noise == 1 && U32(t, 0x150) < cb(c, M_ + 0x5a)) ||
        (mi->palm == 1 && U32(t, 0x150) < tdc16(c, mi->src == 1 ? 0xe7e : 0xe7c))) ok = 0;
    else ok = B(t, 0x156) || U32(t, 0x14c);
    if (!ok) B(t, 0x155) = 0;
    if (!B(t, 0x154) || !ok) return;
    if (ST(t) == 1 && B(t, 0x10d) == 2) return;
    if (r->type == 8 && U32(t, 0x150) < 2) return;
    B(t, 0x154) = 0;      // (IsPositionInsideVirtualTouchPadArea: no touchpad area)
}

static void add_finger_to_track(const struct heat_cfg *c, struct tracklib *tl, int i, R *r, const struct mi *mi,
                                const uint16_t *scan) {
    TF *t = &tl->f[i];
    uint8_t src = mi->pen;
    memcpy(t->b + 0x30, t->b + 0x28, 8);
    memcpy(t->b + 0x28, t->b + 0x38, 8);
    SF(t, 0x38, r->x); SF(t, 0x3c, r->y);
    SF(t, 0x40, r->ax); SF(t, 0x44, r->ay);
    // CheckIfDetectedFingerIsSmall
    B(t, 0x109) = 0;
    B(t, 0x156) = r->live;
    int sf = SF_ + (src ? 7 : 4);
    if (cb(c, sf) < r->count) B(t, 0x10a) = 0;
    else {
        uint8_t o = B(t, 0x10a);
        if (o == 0xff) B(t, 0x109) = 1;
        else { B(t, 0x10a) = (uint8_t)(o + 1); if (cb(c, sf + 2) <= (uint8_t)(o + 1)) B(t, 0x109) = 1; }
    }
    r->b2c = 1;
    B(t, 0x108) = 1;
    B(t, 0x143) = r->window;
    // UpdateBlobSize
    if (r->live == 1) {
        float w = cf(c, BSS_);
        float sx = (float)(((unsigned)r->maxx - r->minx + 1) & 0xffff), sy = (float)(((unsigned)r->maxy - r->miny + 1) & 0xffff);
        if (cb(c, BSS_ + 4) == 0) { SF(t, 0x50, sx); SF(t, 0x54, sy); }
        else { SF(t, 0x50, sx * w + F(t, 0x50) * (1.0f - w)); SF(t, 0x54, sy * w + F(t, 0x54) * (1.0f - w)); }
    }
    // UpdateTrackMovementRectangle
    if (r->x <= F(t, 0x1d8)) SF(t, 0x1d8, r->x);
    if (r->y <= F(t, 0x1dc)) SF(t, 0x1dc, r->y);
    if (F(t, 0x1e0) <= r->x) SF(t, 0x1e0, r->x);
    if (F(t, 0x1e4) <= r->y) SF(t, 0x1e4, r->y);
    SU16(t, 0x8a, r->orientation);
    // UpdatePredictionError
    float det[2], v2[2];
    P2(t, 0x38, det);
    float speed = 0.0f;
    if (ST(t) != 1) { P2(t, 0x20, v2); speed = sqrt2(dist2(det, v2)); }
    float old = F(t, 0xf4);
    SF(t, 0xf4, speed);
    // UpdateVelocity
    float dt = mi->dt, ay = fabsf(F(t, 0x3c) - F(t, 0x2c));
    P2(t, 0x28, v2);
    float d = sqrt2(dist2(det, v2)), ct = ST(t) == 3 ? F(t, 0x144) : 0.0f;
    SF(t, 0x48, d / (dt + ct));
    SF(t, 0x4c, ay / (dt + ct));
    // UpdateDistanceBucket
    uint16_t win = ch(c, K_ + 8);
    uint8_t n = (uint8_t)(B(t, 0xd0) + 1);
    B(t, 0xd0) = n;
    if ((uint8_t)(win >> 1) == n) SF(t, 0x80, 0);
    if (win == n) { n = 0; SF(t, 0x84, 0); B(t, 0xd0) = 0; }
    float a = F(t, 0x80), b = F(t, 0x84);
    if (a < d) { SF(t, 0x80, d); a = d; }
    if (b < d) { SF(t, 0x84, d); b = d; }
    B(t, 0x104) = 0;
    SF(t, 0x7c, (uint8_t)(win >> 1) <= n ? b : a);
    SF(t, 0x144, 0);
    // UpdateSeparationStatus
    uint8_t s107 = B(t, 0x107), w8;
    if (r->type == 3 && r->split) {
        if (s107 == 3) { B(t, 0x10b) = 0; w8 = 3; }
        else if (s107 == 1) {
            B(t, 0x10b)++;
            if (cb(c, M_ + 0x58) <= B(t, 0x10b)) { B(t, 0x107) = 3; B(t, 0x10b) = 0; w8 = 3; }
            else w8 = 1;
        } else w8 = s107;
    } else {
        if (s107 == 3) {
            B(t, 0x10b)++;
            if (cb(c, M_ + 0x58) <= B(t, 0x10b)) { B(t, 0x107) = 1; B(t, 0x10b) = 0; w8 = 1; }
            else w8 = 3;
        } else if (s107 == 1) { B(t, 0x10b) = 0; w8 = 1; }
        else w8 = s107;
    }
    uint16_t cnt = r->count;
    float fn = (float)U32(t, 0x150);
    SU16(t, 0xfc, cnt);
    SF(t, 0x100, (F(t, 0x100) * (fn + 1.0f) + (float)cnt) / (fn + 2.0f));
    // UpdateIIRParameters
    if (cb(c, IIR_ + 0xc)) {
        float thr = cf(c, IIR_), vx = fabsf(F(t, 0x58)), vy = fabsf(F(t, 0x5c));
        if (vx <= thr && vy <= thr && (w8 == 3 || g_high_noise == 1)) {
            B(t, 0x15a) = 1;
            SF(t, 0x15c, cf(c, IIR_ + 4));
        } else if (B(t, 0x15a) == 1) {
            float w = F(t, 0x15c) - cf(c, IIR_ + 8);
            SF(t, 0x15c, w);
            if (!(w > 0.0f)) B(t, 0x15a) = 0;
        }
    } else B(t, 0x15a) = 0;
    update_velocity_and_reported(c, t, dt);
    if (ST(t) == 1 && B(t, 0x10d) != 3) prepare_backward(c, tl, i, mi);
    update_match_limit(c, t, speed - old, speed, src);
    // DoIIR
    float ox, oy;
    if (B(t, 0x15a) == 1) {
        float w = F(t, 0x15c);
        ox = F(t, 0x18) * (1.0f - w) + F(t, 0x168) * w;
        oy = F(t, 0x1c) * (1.0f - w) + F(t, 0x16c) * w;
    } else { ox = F(t, 0x18); oy = F(t, 0x1c); }
    SF(t, 0x160, ox); SF(t, 0x164, oy); SF(t, 0x168, ox); SF(t, 0x16c, oy);
    if (cb(c, J_ + 0x9d) == 1 && ST(t) != 1) jitter_restrictor(c, t, src);
    else { SF(t, 0, ox); SF(t, 4, oy); }
    kalman_update(c, t, mi, scan);
    if (B(t, 0x10d) != 2) { SU32(t, 0xd4, 2); B(t, 0xdb) = 0; }
    drop_report_logic(c, t, mi, r);
    SU32(t, 0x150, U32(t, 0x150) + 1);
}

// Returns 1 if the track closed (st 4 or 5), 0 if held (st 3).
static int is_closed(const struct heat_cfg *c, struct tracklib *tl, int i, const struct mi *mi) {
    TF *t = &tl->f[i];
    uint32_t st = ST(t);
    if (st == 1) { set_status(t, 5); return 1; }
    float px = F(t, 0x20), py = F(t, 0x24);
    int inside = px > 0.0f && px < c->ctc_w && py > 0.0f && py < c->ctc_h;
    B(t, 0x108) = 1;
    int sf = SF_ + (mi->pen ? 7 : 4);
    uint8_t h = B(t, 0x104);
    int hold = 0;
    if (B(t, 0x188) && inside && h < 10) hold = 1;
    else if (mi->pen && (g_high_noise == 1 ? h < 7 : h < 3)) hold = 1;
    else if (B(t, 0x109) && h < cb(c, sf + 1) && inside && (U16(t, 0xf2) > 5 || tl->nactive > 1)) hold = 1;
    else if (g_high_noise == 1 && inside && h < cb(c, P_ + 0xc)) hold = 1;
    else if (tl->nactive == 1 && !mi->pen && (g_high_noise == 1 || mi->m28 == 1) && !(F(t, 0x70) < cf(c, FM_ + 4)) &&
             inside && h < cb(c, FM_ + 8)) hold = 1;
    else if (h == 0 && B(t, 0x106) != 0xff) hold = 1;
    else if (B(t, 0x10d) == 1 && tl->deque_n[i]) hold = 1;
    if (hold) {
        SU32(t, 0xd4, 3);
        B(t, 0x104) = (uint8_t)(h + 1);
        B(t, 0xdb)++;
        SF(t, 0x144, mi->dt + F(t, 0x144));
        SF(t, 0xf8, cf(c, M_ + 0x48) + F(t, 0xf8));
        return 0;
    }
    B(t, 0x104) = 0;
    if (st != 3) { set_status(t, 5); return 1; }
    B(t, 0xdb) = 0;
    SU32(t, 0xd4, 4);
    return 1;
}

static int forward_prediction(const struct heat_cfg *c, const TF *t, float *out, uint8_t mode) {
    float px = F(t, 0x20), py = F(t, 0x24), x = F(t, 0), y = F(t, 4);
    float dx = px - x, dy = py - y, adx = fabsf(dx), ady = fabsf(dy);
    float m = mode == 1 ? cf(c, P0_ + 0x24) * 3.0f : cf(c, P0_ + 0x24);
    float n = mode == 1 ? cf(c, P0_ + 0x28) * 0.25f : cf(c, P0_ + 0x28);
    const float W = c->ctc_w, H = c->ctc_h;
    float off = ST(t) == 3 ? -2.0f : 0.0f;
    if (!((m + py >= H && y != H) || (py <= m && y != 0.0f) || (px <= m && x != 0.0f) || (m + px >= W && x != W))) return 0;
    if (!(F(t, 0x1e0) - F(t, 0x1d8) >= 8.0f) && !(F(t, 0x1e4) - F(t, 0x1dc) >= 8.0f)) return 0;
    float ddx = F(t, 0x38) - F(t, 0x28), ddy = F(t, 0x3c) - F(t, 0x2c);
    int xlo = 0;
    // x low (towards 0)
    if (px < off || !(n > -ddx || px > m)) {
        out[1] = y;
        if (adx != 0.0f) out[1] = y + dy / adx * x;
        out[0] = 0.0f;
        xlo = out[1] >= 0.0f && out[1] <= H;
    }
    // x high (towards W)
    if (px > W - off || !(ddx < n || px < W - m)) {
        out[1] = y;
        if (adx != 0.0f) out[1] = y + dy / adx * (W - x);
        out[0] = W;
        if (xlo) return 1;
        if (!(out[1] < 0.0f || out[1] > H)) return 1;
    } else if (xlo) return 1;
    // y low
    if (py < off || !(n > -ddy || py > m)) {
        out[0] = x;
        if (ady != 0.0f) out[0] = x + dx / ady * y;
        out[1] = 0.0f;
        return 1;
    }
    // y high
    if (py > H - off || !(ddy < n || py < H - m)) {
        out[0] = x;
        if (ady != 0.0f) out[0] = x + dx / ady * (H - y);
        out[1] = H;
        return 1;
    }
    return 0;
}

static void run_backward(struct tracklib *tl, int i) {
    TF *t = &tl->f[i];
    if (tl->deque_n[i] == 0) { B(t, 0x10d) = 3; return; }
    uint8_t old = B(t, 0x10f);
    B(t, 0x10f) = old ^ 1;
    if (old && ST(t) == 2) { float rep[2]; P2(t, 8, rep); dq_push(tl, i, rep); }
    float p[2];
    dq_pop(tl, i, p);
    SF(t, 8, p[0]); SF(t, 0xc, p[1]);
    if (tl->deque_n[i] == 0) B(t, 0x10d) = 3;
}

static int fix_position(const struct heat_cfg *c, struct tracklib *tl, int i) {
    TF *t = &tl->f[i];
    memcpy(t->b + 8, t->b, 8);
    uint32_t st = ST(t);
    if ((st & ~1u) == 2 && B(t, 0x10d) == 1 && B(t, 0x154) == 0) { run_backward(tl, i); return 0; }
    if (st != 5 && st != 3) return 0;
    if (B(t, 0x148) == 1) { B(t, 0x148) = 0; return 0; }
    float fp[2] = { 0, 0 };
    if (forward_prediction(c, t, fp, tl->mode)) {
        SF(t, 8, fp[0]); SF(t, 0, fp[0]); SF(t, 0xc, fp[1]); SF(t, 4, fp[1]);
        B(t, 0x148) = 1;
        if (st != 5) { set_status(t, 5); return 1; }
        return 0;
    }
    if (st == 5 && B(t, 0xd8) == 1) { B(t, 0x148) = 1; B(t, 0xd8) = 0; }
    return 0;
}

// ---------------------------------------------------------------------------
// TracksArray

#define FOR_LIST(tl, i) for (int i = (tl)->head; i >= 0; i = (tl)->next[i])

static void init_flags_and_prediction(const struct heat_cfg *c, struct tracklib *tl, const struct pt *pt) {
    tl->nmatched = 0;
    tl->dt = pt->dt;
    tl->edge_lo = pt->edge_hi;     // (+0x2708 takes +0x1c, +0x2709 +0x1b)
    tl->edge_hi = pt->edge_lo;
    tl->prev_mode = (tl->prev_mode == 1 && (pt->irregular == 1 || pt->n == 1)) ? 1 : tl->mode;
    tl->mode = pt->mode;
    FOR_LIST(tl, i) {
        TF *t = &tl->f[i];
        uint8_t s105 = B(t, 0x105);
        B(t, 0x108) = 0; B(t, 0x105) = 0xff; B(t, 0x106) = s105;
        int base = ST(t) == 3 ? 0x20 : ST(t) == 2 ? 0x18 : -1;
        if (base >= 0 && !pt->irregular) {
            SF(t, 0x20, F(t, base) + F(t, 0x58) * pt->dt);
            SF(t, 0x24, F(t, base + 4) + F(t, 0x5c) * pt->dt);
        }
    }
    tl->fast = 0;
    float slot0 = F(&tl->f[0], 0x70);   // (TracksArray +0x90: the first slot's, whichever track is there)
    tl->fast_half = tl->nactive == 1 ? cf(c, FM_ + 4) <= slot0 : 0;
    if (pt->noise == 1 && tl->nactive == 1 && cf(c, FM_ + 4) <= slot0) tl->fast = 1;
    g_high_noise = pt->noise;
}

static void open_new_track(const struct heat_cfg *c, struct tracklib *tl, R *r, uint8_t p2, uint8_t p3) {
    r->b2c = 1;
    // (TracksArray +0x26f0, the open-border margin, is 0: only strong blobs open tracks.)
    if (!(tl->nactive < TL_FINGERS && r->strong != 0)) return;
    int i = 0;
    while (i < TL_FINGERS - 1 && ST(&tl->f[i]) != 0) i++;
    init_new_track(c, tl, &tl->f[i], r, p2, p3, tl->next_id, tl->prev_mode);
    B(&tl->f[i], 0x1c0) = (uint8_t)i;     // (TrackedFinger::Init's index)
    tl->deque_n[i] = 0;
    tl->next[i] = -1;
    tl->next_id = tl->next_id == 0xfffd ? 0 : (uint16_t)(tl->next_id + 1);
    tl->nmatched++;
    tl->nactive++;
    if (tl->head < 0) tl->head = i;
    else {
        int j = tl->head;
        while (tl->next[j] >= 0) j = tl->next[j];
        tl->next[j] = i;
    }
}

static void match_single(const struct heat_cfg *c, struct tracklib *tl, int i, R *r, const struct mi *mi, int src,
                         float d2, const uint16_t *scan) {
    TF *t = &tl->f[i];
    float lim;
    int reject;
    if (src == 1) {
        lim = cf(c, M_ + 0x14);
        if (ST(t) == 3) lim = lim + cf(c, M_ + 0x18);
        goto fast;
    }
    lim = F(t, 0xf8);
    if (mi->pen) goto cmp;
    {
        float d = (float)(int)((int)r->window - (int)B(t, 0x143)) * 0.01f;
        if (d > 0.0f) lim = lim + fabsf(d * (F(t, 0x4c) * mi->dt));
    }
fast:
    if (tl->fast == 1) lim = lim + cf(c, FM_);
    else if (tl->fast_half == 1) lim = lim + cf(c, FM_) * 0.5f;
cmp:
    reject = sqrtf(d2) > lim;
    if (!reject && ST(t) == 3) {
        float f18[2], rp[2] = { r->x, r->y };
        P2(t, 0x18, f18);
        float l3 = B(t, 0xdb) > 1 ? 35.0f * (float)B(t, 0xdb) : 35.0f;   // (TracksArray +0x26f4)
        reject = !(sqrt2(dist2(rp, f18)) <= l3);
    }
    if (!reject) {
        if (src == 1) {                     // a track left over, near a split record: its group's
            if (r->r2b < 15) tl->sep[r->r2b][2]++;
            B(t, 0x105) = r->r2b;
            return;
        }
        if (src >= 2 && src <= 4) goto accept;
        if (src != 0) return;
    accept:
        if (B(t, 0x155) || r->strong) {
            B(t, 0x155) = 1;
            add_finger_to_track(c, tl, i, r, mi, scan);
            tl->nmatched++;
            return;
        }
        if (is_closed(c, tl, i, mi)) tl->nactive--;
        open_new_track(c, tl, r, mi->pen, mi->m1);
        return;
    }
    if (src >= 2 && src <= 4) {
        if (is_closed(c, tl, i, mi)) tl->nactive--;
        open_new_track(c, tl, r, mi->pen, mi->m1);
    } else if (src == 1) {
        if (is_closed(c, tl, i, mi)) tl->nactive--;
    } else if (src == 0) open_new_track(c, tl, r, mi->pen, mi->m1);
}

static void match_detected(const struct heat_cfg *c, struct tracklib *tl, R *r, const struct mi *mi, const uint16_t *scan) {
    float best = FLT_MAX, rp[2] = { r->x, r->y };
    int bi = -1;
    FOR_LIST(tl, i) {
        TF *t = &tl->f[i];
        uint32_t st = ST(t);
        if (B(t, 0x108) || st == 0 || st == 4 || st == 5) continue;
        float pr[2]; P2(t, 0x20, pr);
        float d = dist2(rp, pr);
        if (best > d) { best = d; bi = i; }
    }
    if (bi >= 0) match_single(c, tl, bi, r, mi, 0, best, scan);
    else open_new_track(c, tl, r, mi->pen, mi->m1);
}

static void try_closing_all(const struct heat_cfg *c, struct tracklib *tl, const struct mi *mi, int irr) {
    if (tl->head < 0) return;
    if (irr == 1 && tl->irr_prev < 10) {
        FOR_LIST(tl, i) {
            TF *t = &tl->f[i];
            uint32_t st = ST(t);
            if (st <= 5 && ((1u << st) & 0x33)) continue;
            set_status(t, 3);
            SF(t, 0x144, tl->dt + F(t, 0x144));
            SF(t, 0xf8, cf(c, M_ + 0x48) + F(t, 0xf8));
        }
    } else {
        FOR_LIST(tl, i) {
            uint32_t st = ST(&tl->f[i]);
            if (st <= 5 && ((1u << st) & 0x31)) continue;
            if (is_closed(c, tl, i, mi)) tl->nactive--;
        }
    }
}

static void handle_unmatched(const struct heat_cfg *c, struct tracklib *tl, const struct mi *mi, const uint16_t *scan) {
    if (!(tl->nmatched < tl->nactive)) return;
    FOR_LIST(tl, i) {
        TF *t = &tl->f[i];
        uint32_t st = ST(t);
        if (B(t, 0x108) || (st <= 5 && ((1u << st) & 0x31))) continue;
        // While a blob was split, a track left over takes the nearest
        // record not yet used, for its separation group.
        if (tl->nsep != 0 && tl->ndet != 0) {
            R *best = NULL;
            float bd = FLT_MAX, pr[2];
            P2(t, 0x20, pr);
            for (int k = 0; k < tl->ndet; k++) {
                R *r = &tl->det[k];
                if (r->b2c != 0 || r->valid != 1) continue;
                float rp[2] = { r->x, r->y }, d = dist2(rp, pr);
                if (d < bd) { bd = d; best = r; }
            }
            if (best) { match_single(c, tl, i, best, mi, 1, bd, scan); continue; }
        }
        if (is_closed(c, tl, i, mi)) tl->nactive--;
    }
}

// SepFingerArray.

static void sep_add(struct tracklib *tl, R *r) {     // AddFinger
    uint8_t n = tl->nsep;
    for (uint8_t g = 0; g < n; g++)
        if (tl->sep[g][0] == r->label) {
            if (tl->sep[g][1] > 0xe) { r->b2c = 1; return; }
            r->r2b = g;
            tl->sep[g][1]++;
            return;
        }
    if (n > 0xe) return;
    r->r2b = n;
    tl->sep[n][0] = r->label;
    tl->sep[n][1] = 1;
    tl->sep[n][2] = 0;
    tl->nsep++;
}

// IsHandledByFastSplittedFinger: one fast track and a blob split in two or
// three: the parts are matched to it as one record.
static int sep_fast_split(const struct heat_cfg *c, struct tracklib *tl, const uint8_t *g, const uint8_t *fi, int ti,
                          const struct mi *mi, const uint16_t *scan) {
    TF *t = &tl->f[ti];
    if ((g[1] & 0xfe) != 2 || g[2] != 1) return 0;
    if (!(F(t, 0x70) > cf(c, FM_ + 4))) return 0;
    if (B(t, 0x107) == 3) return 0;
    if (!(tl->det[fi[0]].count <= cb(c, FM_ + 9))) return 0;
    R m;
    memset(&m, 0, sizeof m);
    uint16_t minx = (uint16_t)c->cols, miny = (uint16_t)c->rows, maxx = 0, maxy = 0;   // (CommonTrackingCfg +0x54, +0x56)
    uint8_t strong = 0, live = 1;
    float sx = 0.0f, sy = 0.0f, sw = 0.0f;
    for (int k = 0; k < g[1]; k++) {
        const R *r = &tl->det[fi[k]];
        sx = sx + r->x; sy = sy + r->y;
        if (!(r->minx > minx)) minx = r->minx;
        if (!(r->miny > miny)) miny = r->miny;
        if (!(r->maxx < maxx)) maxx = r->maxx;
        if (!(r->maxy < maxy)) maxy = r->maxy;
        strong |= r->strong;
        live &= r->live;
        sw = sw + (float)r->window;
    }
    float inv = 1.0f / (float)g[1];
    m.x = sx * inv; m.y = sy * inv;
    m.minx = (uint16_t)(minx + 1); m.miny = (uint16_t)(miny + 1);   // (sic)
    m.maxx = maxx; m.maxy = maxy;
    m.type = 1;
    m.count = tl->det[fi[0]].count;
    m.strong = strong;
    m.window = (uint8_t)(int)(sw * inv);
    m.live = live;
    float rp[2] = { m.x, m.y }, pr[2];
    P2(t, 0x20, pr);
    match_single(c, tl, ti, &m, mi, 3, dist2(rp, pr), scan);
    return 1;
}

// IsSuspectedAsFalseSeparation: more or fewer parts than tracks, the size
// unchanged: hold the tracks a few frames instead.
static int sep_false(const struct heat_cfg *c, struct tracklib *tl, const uint8_t *g, uint16_t count, const uint8_t *ti) {
    uint8_t nt = g[2];
    if (g[1] == nt) return 0;
    TF *t0 = &tl->f[ti[0]];
    int d = (int)count - (int)U16(t0, 0xfc);
    if (d < 0) d = -d;
    if (!((float)(d & 0xff) < cf(c, M_ + 0x10))) return 0;
    uint8_t mx = 0;
    for (int k = 0; k < nt; k++) {
        TF *t = &tl->f[ti[k]];
        if (ST(t) == 1) return 0;
        if (B(t, 0x104) > mx) mx = B(t, 0x104);
    }
    if (mx < cb(c, M_ + 0x57)) {
        for (int k = 0; k < nt; k++) {
            TF *t = &tl->f[ti[k]];
            set_status(t, 3);
            SF(t, 0x144, tl->sep_dt + F(t, 0x144));
            B(t, 0x104)++;
            SF(t, 0xf8, cf(c, M_ + 0x4c) + F(t, 0xf8));
        }
        return 1;
    }
    for (int k = 0; k < nt; k++) {
        TF *t = &tl->f[ti[k]];
        SF(t, 0xf8, cf(c, M_ + 0x4c) + F(t, 0xf8));
    }
    return 0;
}

static void sep_hungarian(const struct heat_cfg *c, struct tracklib *tl, const uint8_t *g, const uint8_t *fi,
                          const uint8_t *ti, const struct mi *mi, const uint16_t *scan) {
    static struct hungarian h;
    uint16_t tp[TL_FINGERS][2], bp[HEAT_REPORTS][2];
    for (int k = 0; k < g[2]; k++) {
        const TF *t = &tl->f[ti[k]];
        tp[k][0] = (uint16_t)(int16_t)(int)(F(t, 0x20) + 0.5f);
        tp[k][1] = (uint16_t)(int16_t)(int)(F(t, 0x24) + 0.5f);
    }
    for (int k = 0; k < g[1]; k++) {
        const R *r = &tl->det[fi[k]];
        bp[k][0] = (uint16_t)(int16_t)(int)(r->x + 0.5f);
        bp[k][1] = (uint16_t)(int16_t)(int)(r->y + 0.5f);
    }
    float maxd;
    memcpy(&maxd, c->tpp, 4);          // (SepFingerArray +0x68c: TouchPostProcessingCfg +0)
    hungarian_init(&h, (const uint16_t (*)[2])tp, g[2], (const uint16_t (*)[2])bp, g[1], (uint16_t)(int)maxd);
    hungarian_run(&h);
    // ProcessHungarianResult.
    int n = h.n;
    for (int k = 0; k < n; k++)
        for (int l = 0; l < n; l++) {
            if (h.assign[k * n + l] != 1) continue;
            if (k < g[2]) {
                int i = ti[k];
                if (l < g[1]) {
                    R *r = &tl->det[fi[l]];
                    float rp[2] = { r->x, r->y }, pr[2];
                    P2(&tl->f[i], 0x20, pr);
                    match_single(c, tl, i, r, mi, 4, dist2(rp, pr), scan);
                } else if (is_closed(c, tl, i, mi)) tl->nactive--;
            } else open_new_track(c, tl, &tl->det[fi[l]], mi->pen, mi->m1);
            break;
        }
}

static void sep_process(const struct heat_cfg *c, struct tracklib *tl, const struct mi *mi, const uint16_t *scan) {
    for (uint8_t gi = 0; gi < tl->nsep; gi++) {
        uint8_t fi[20] = { 0 }, ti[20] = { 0 };
        int nf = 0, nt = 0;
        for (int k = 0; k < tl->ndet; k++) {
            const R *r = &tl->det[k];
            if (r->b2c == 0 && r->valid == 1 && r->r2b == gi && nf < 20) fi[nf++] = (uint8_t)k;
        }
        for (int i = 0; i < TL_FINGERS; i++) {
            const TF *t = &tl->f[i];
            uint32_t st = ST(t);
            if (!(st > 5 || !((1u << st) & 0x31))) continue;
            if (B(t, 0x108) == 0 && B(t, 0x105) == gi) ti[nt++] = (uint8_t)i;
        }
        const uint8_t *g = tl->sep[gi];
        if (g[2] == 1 && g[1] == 1) {
            R *r = &tl->det[fi[0]];
            float rp[2] = { r->x, r->y }, pr[2];
            P2(&tl->f[ti[0]], 0x20, pr);
            match_single(c, tl, ti[0], r, mi, 2, dist2(rp, pr), scan);
        } else if (g[2] == 0) {
            for (int k = 0; k < g[1]; k++) open_new_track(c, tl, &tl->det[fi[k]], mi->pen, mi->m1);
        } else {
            if (sep_fast_split(c, tl, g, fi, ti[0], mi, scan)) continue;
            if (sep_false(c, tl, g, tl->det[fi[0]].count, ti)) continue;
            sep_hungarian(c, tl, g, fi, ti, mi, scan);
        }
    }
}

static void fill_os_report(const struct heat_cfg *c, struct tracklib *tl, const struct mi *mi) {
    tl->nout = 0;
    FOR_LIST(tl, i) {
        TF *t = &tl->f[i];
        uint32_t st = ST(t);
        if (B(t, 0x154) != 0 || st - 2u >= 4) continue;
        int code;
        if (B(t, 0x156) == 0) {
            set_status(t, 5);
            code = 3;
            tl->nactive--;
            memcpy(t->b + 8, t->b + 0x10, 8);
        } else code = (st == 4 || (st == 5 && B(t, 0x148) == 0)) ? 2 : 1;
        st = ST(t);
        if (tl->nout >= TL_REPORTS) continue;
        struct tl_entry *e = &tl->out[tl->nout];
        // CommonTrackingCfg +0x74: the report swaps the axes.
        e->y = F(t, 8) * c->ctc_scale[0];
        e->x = F(t, 0xc) * c->ctc_scale[1];
        e->h = F(t, 0x50) * c->pitch_mm[0] * c->ctc_scale[0];
        e->w = F(t, 0x54) * c->pitch_mm[1] * c->ctc_scale[1];
        e->id = U16(t, 0x158);
        e->orientation = U16(t, 0x8a);
        e->lift = st == 4 || (st == 5 && B(t, 0x148) == 0);
        e->lost = code == 3;
        memcpy(t->b + 0x10, t->b + 8, 8);
        SU32(t, 0x1d0, mi->counter);
        tl->nout++;
        if (!e->lift) SU32(t, 0x14c, U32(t, 0x14c) + 1);
    }
}

static void post_processing(const struct heat_cfg *c, struct tracklib *tl, const struct mi *mi) {
    FOR_LIST(tl, i)
        if (fix_position(c, tl, i)) tl->nactive--;
    fill_os_report(c, tl, mi);
}

static void close_old_tracks(struct tracklib *tl, uint16_t counter) {
    // TracksArray::CloseOldTracks: drop closed tracks from the list.
    int prev = -1;
    for (int i = tl->head; i >= 0;) {
        int nx = tl->next[i];
        TF *t = &tl->f[i];
        if (ST(t) == 4 || (ST(t) == 5 && B(t, 0x148) == 0)) {
            set_status(t, 0);
            if (prev < 0) tl->head = nx; else tl->next[prev] = nx;
            // (The unlinked record keeps its next pointer, as in the stock code.)
        } else prev = i;
        i = nx;
    }
    uint16_t last = tl->last_counter;
    unsigned gap = last <= counter ? (unsigned)(counter - last) : (unsigned)(0xffffu - last) + counter;
    if (gap > 9)
        FOR_LIST(tl, i) {
            uint32_t st = ST(&tl->f[i]);
            if ((st | 1) != 5) { set_status(&tl->f[i], 5); tl->nactive--; }
        }
    tl->last_counter = counter;
}

// ---------------------------------------------------------------------------

void tracklib_init(struct tracklib *tl) {
    memset(tl, 0, sizeof *tl);
    tl->sep_dt = 0.1f;
    tl->head = -1;
    for (int i = 0; i < TL_FINGERS; i++) tl->next[i] = -1;
}

int tracklib_frame(const struct heat_cfg *c, struct tracklib *tl, const struct heat_tracks *s, uint16_t counter) {
    // TouchDetection's records, as TouchProcessingInput holds them.
    tl->ndet = s->nout < HEAT_REPORTS ? s->nout : HEAT_REPORTS;
    memcpy(tl->det, s->out, sizeof(R) * (size_t)tl->ndet);
    uint8_t n = (uint8_t)tl->ndet;

    // TouchDetection::GenerateIrregularEvent: a failed frame ('3'), or a
    // single held record replayed by the tap logic ('4').
    int pi_d = 0;
    if (s->failed) { tl->pi_code = '3'; pi_d = 1; tl->pi_dt = s->scan_us; }
    else if (n == 1 && s->nheld == 1) { tl->pi_code = '4'; pi_d = 1; tl->pi_dt = s->scan_us; }

    // ProjectTuningMngr::TrackingInit.
    close_old_tracks(tl, counter);
    int irr = 0;
    uint32_t it = s->scan_us;
    if (pi_d == 1 && tl->pi_dt != 0) { irr = 1; it += tl->pi_dt; }
    if ((uint32_t)counter - tl->irr_last == 2) { it += 10000; irr = 1; tl->pi_code = '4'; }
    tl->pi_dt = it;
    tl->irr_last = counter;
    // IrregularEventHistoryHandling: frames given up on in a row (detection
    // failed, nothing handed on).
    if (irr == 1 && s->failed && n == 0) { tl->irr_count++; tl->irr_prev = tl->irr_count; }
    else { tl->irr_prev = tl->irr_count; tl->irr_count = 0; }

    // TrackLib::InitTrackingProcess.
    struct mi mi = { .pen = 0, .m1 = 2, .src = 0, .prev_src = 0, .m28 = 0, .palm = s->palm, .counter = counter };
    mi.dt = (float)(irr == 1 ? tl->pi_dt : s->scan_us) * 1e-06f;
    struct pt pt = { .n = n, .dt = mi.dt, .noise = 0 };
    uint16_t thr = ch(c, EDGE_THR);
    pt.edge_lo = s->nsr[0] >= thr;
    pt.edge_hi = s->nsr[c->nwin - 1] >= thr;

    // TrackLib::RunTrackingProcess.
    if (irr == 1 && tl->pi_code != '3') {
        init_flags_and_prediction(c, tl, &pt);
        try_closing_all(c, tl, &mi, irr);
        post_processing(c, tl, &mi);
        pt.irregular = 1;
        irr = 0;
    }
    // DetectedArray::PrepareDetectedBlobs.
    for (int i = 0; i < n; i++) {
        R *r = &tl->det[i];
        struct heat_finger fg;
        heat_to_mm(c, r->x, r->y, r->radius, r->valid, &fg);
        r->ax = r->x; r->ay = r->y;
        r->x = fg.x; r->y = fg.y;
    }
    // DeleteWeakBlobWithPoorNsr.
    for (int i = 0; i < n; i++) {
        R *r = &tl->det[i];
        if (r->strong) continue;
        uint8_t w = c->row_lut[(unsigned)(int)(r->ay + 0.5f) & 0x3f];
        if (ch(c, WEAK_NSR) < s->nsr[w]) r->valid = 0;
    }
    // MatchFingers::PrepareTracking, RunMatching.
    tl->nsep = 0;
    init_flags_and_prediction(c, tl, &pt);
    tl->sep_dt = mi.dt;
    if (n == 0) try_closing_all(c, tl, &mi, irr);
    else {
        for (int i = 0; i < n; i++) {
            R *r = &tl->det[i];
            if (r->valid != 1) continue;
            if (r->split == 0 && r->type != 7) match_detected(c, tl, r, &mi, s->nsr);
            else sep_add(tl, r);
        }
        handle_unmatched(c, tl, &mi, s->nsr);
        sep_process(c, tl, &mi, s->nsr);
    }
    post_processing(c, tl, &mi);
    return tl->nout;
}

static void hex(FILE *out, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) fprintf(out, "%02x", p[i]);
}

void tracklib_dump(FILE *out, const struct tracklib *tl) {
    fprintf(out, "P tastat n=%u head=%d\n", tl->nactive, tl->head);
    for (int i = 0; i < TL_FINGERS; i++) {
        const TF *t = &tl->f[i];
        if (!ST(t) && tl->next[i] < 0 && tl->head != i) continue;
        fprintf(out, "P tf %d st=%u next=%d pos=%.6f,%.6f rep=%.6f,%.6f id=%u h=", i, ST(t), tl->next[i],
                (double)F(t, 0), (double)F(t, 4), (double)F(t, 8), (double)F(t, 0xc), U16(t, 0x158));
        hex(out, t->b, 0x110); fputc('|', out); hex(out, t->b + 0x140, 0x50); fputc('|', out);
        hex(out, t->b + 0x1b0, 0x38); fputc('\n', out);
    }
    fprintf(out, "P osrep n=%u ", (unsigned)tl->nout);
    for (int i = 0; i < tl->nout; i++) {
        const struct tl_entry *e = &tl->out[i];
        fprintf(out, " [x=%.6f y=%.6f w=%.6f h=%.6f id=%u o=%u f=%u l=%u]", (double)e->x, (double)e->y, (double)e->w,
                (double)e->h, e->id, e->orientation, e->lift, e->lost);
    }
    fputc('\n', out);
}
