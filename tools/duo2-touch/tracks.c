// Surface Duo 2 touch: TouchDetection's tracker, as libsurfacetouch_c1.so
// runs it after CclLogic and FeatureExtraction (heat.c) in
// TouchDetection::ProcessTouch:
//
//   InitTouchDetection        palm state, palm map decay, per-track resets
//   Classification            each blob's log-likelihood of each class
//   TouchDetectionTracking    blobs to tracks (Hungarian), lost tracks, track classes
//   PostTouchDetectionProcess closing, palm bookkeeping
//   TouchDetectionPostProcess palm by proximity, suspension, padded centroid,
//                             the records handed to DetectedArray
//
// Each function below is named after the stock one it reproduces, and keeps
// its order of operations, its single-precision arithmetic and its quirks.
// Classes (eTRACK_TYPE): 0 and 2 fingers, 1 palm, 3 several fingers in one
// blob, 4 undecided, 5 palm by proximity.
//
// Not reproduced, because nothing on the Duo 2 reaches them: the pen paths
// (no pen reports here; their inputs read as "no pen"), the virtual touch
// pad area (no region configured), grip rejection (off in this tuning), and
// on-the-fly calibration bookkeeping, which changes no track or record.

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "heat.h"

_Static_assert(sizeof(struct heat_hist) == 0x30, "history entry layout");
_Static_assert(sizeof(struct heat_trk) == 0x25c, "TrackDataStruct layout");
_Static_assert(offsetof(struct heat_trk, status) == 0x3c, "TrackDataStruct layout");
_Static_assert(offsetof(struct heat_trk, h) == 0x60, "TrackDataStruct layout");
_Static_assert(offsetof(struct heat_trk, run_undecided) == 0x250, "TrackDataStruct layout");
_Static_assert(sizeof(struct heat_report) == 0x34, "ReportInformation layout");

#define LUT heat_lut

// TouchDetectionCfg fields by offset.
static float cf(const struct heat_cfg *c, int o) { float v; memcpy(&v, c->td + o, 4); return v; }
static uint8_t cb(const struct heat_cfg *c, int o) { return c->td[o]; }
static int8_t cc(const struct heat_cfg *c, int o) { return (int8_t)c->td[o]; }
static uint16_t ch(const struct heat_cfg *c, int o) { uint16_t v; memcpy(&v, c->td + o, 2); return v; }
static int16_t cs(const struct heat_cfg *c, int o) { int16_t v; memcpy(&v, c->td + o, 2); return v; }
static int32_t ci(const struct heat_cfg *c, int o) { int32_t v; memcpy(&v, c->td + o, 4); return v; }
static uint32_t cu(const struct heat_cfg *c, int o) { uint32_t v; memcpy(&v, c->td + o, 4); return v; }

// TouchDetection::Init's derived values.
#define PALM_MAP_SHIFT(c) palm_shift(c)
static int palm_shift(const struct heat_cfg *c) {
    int s = 0;
    for (unsigned v = cb(c, 0xe92); v > 1; v >>= 1) s++;   // (int)log2(cell)
    return s;
}
static int palm_map_on(const struct heat_cfg *c) { return cb(c, 0xe93) != 0 && palm_shift(c) > 1; }
static float llr_offset(const struct heat_cfg *c) { return (float)ch(c, 0xe76) * -0.9189385f + -1.3862944f; }
static uint16_t max_suspended(const struct heat_cfg *c) {
    if (cb(c, 0xe68) != 1) return 100;
    uint16_t v = ch(c, 0xe64);
    return v < 0x65 ? 100 : v;
}

// The u16 frame-time difference as the stock computes it.
static unsigned elapsed(uint16_t then, uint16_t now) {
    unsigned d = now < then ? ~(unsigned)then : -(unsigned)then;
    return (d + now) & 0xffff;
}

static uint32_t *type_of(struct heat_trk *t) { return &t->h[t->ring].type; }
static uint32_t prev_type(const struct heat_trk *t) { return t->h[(t->ring + 9) % 10].type; }

void heat_tracks_init(const struct heat_cfg *c, struct heat_tracks *t) {
    (void)c;
    memset(t, 0, sizeof *t);
}

// ---------------------------------------------------------------------------
// SetTrackStatus: keeps the counts of tracks not free and active.

static void set_status(struct heat_tracks *s, struct heat_trk *t, unsigned st) {
    if (st >= 5) return;
    uint32_t old = t->status;
    unsigned act = old - 1u;          // < 2: active or suspended
    switch (st) {
    case 0:
        if (old != 0) {
            if (s->nlive) s->nlive--;
            if (act < 2 && s->nactive) s->nactive--;
            t->status = 0;
        }
        break;
    case 1:
    case 2:
        if (old == 0) s->nlive++;
        if (act > 1) s->nactive++;
        t->status = st;
        break;
    default:
        if (act < 2 && s->nactive) s->nactive--;
        t->status = st;
        break;
    }
}

// ---------------------------------------------------------------------------
// InitTouchDetection, UpdatePalmHeuristicsState, ReducePalmMap.

static void init_frame(const struct heat_cfg *c, struct heat_tracks *s, const struct heat_frame *f) {
    int seen = 0, skipped = 0;
    for (int i = 0; seen + skipped < s->nlive + skipped && i < HEAT_TRACKS; i++) {
        struct heat_trk *t = &s->t[i];
        if (t->status == 0) { skipped++; continue; }
        seen++;
        t->suspended = 0;
        uint32_t ty = *type_of(t);
        if (ty == 5 || (ty != 1 && prev_type(t) == 5 && t->frames > 1)) {
            t->palm_hold--;
            if (t->palm_hold == 0) *type_of(t) = 4;
        } else {
            t->palm_hold = 0;
        }
    }
    s->noisy = f->noise > ch(c, 0xe78);

    // UpdatePalmHeuristicsState (no pen: only a recent palm sets the state).
    s->now = f->ts;
    uint16_t left = 0;
    if (s->palm_left) {
        uint16_t e = (uint16_t)elapsed(s->palm_ts, s->now);
        uint16_t lim = ch(c, 0xe7a);
        left = (uint16_t)((lim <= e ? e : lim) - e);
        s->palm_left = left;
    }
    s->palm = left != 0;
    s->npalms = 0;
    s->nlist = 0;

    // ReducePalmMap: palm cells fade one level per 10 frames.
    if (palm_map_on(c) && s->palm_level) {
        unsigned e = elapsed(s->palm_map_ts, f->ts);
        unsigned steps = (e / 10) & 0xff;
        if (ch(c, 0xe84) < e || s->palm_level <= steps) {
            s->palm_map_ts = f->ts;
            s->palm_level = 0;
            memset(s->palm_map, 0, sizeof s->palm_map);
            memset(s->palm_pen, 0, sizeof s->palm_pen);
        } else if (e > 9) {
            uint16_t r = (uint16_t)(e % 10);
            s->palm_map_ts = r <= f->ts ? (uint16_t)(f->ts - r) : (uint16_t)(r + ~f->ts);
            s->palm_level = (uint8_t)(s->palm_level - e / 10);
            int rows = c->rows / cb(c, 0xe92), cols = c->cols / cb(c, 0xe92);
            for (int y = 0; y < rows; y++)
                for (int x = 0; x < cols; x++) {
                    uint8_t v = s->palm_map[y][x];
                    if (!v) continue;
                    if (steps < v) s->palm_map[y][x] = (uint8_t)(v - e / 10);
                    else { s->palm_map[y][x] = 0; s->palm_pen[y][x] = 0; }
                }
        }
    }
}

static int mapped_to_palm(const struct heat_cfg *c, const struct heat_tracks *s, const struct heat_trk *t) {
    if (!palm_map_on(c)) return 0;
    if (cb(c, 0xe94) == 1 || s->tap) return 0;     // (pen state: none)
    const struct heat_hist *h = &t->h[t->ring];
    unsigned hx = (unsigned)(int)h->x, hy = (unsigned)(int)h->y;
    if (!(hx < (unsigned)c->cols && hy < (unsigned)c->rows)) return 0;
    unsigned half = cb(c, 0xe92) >> 1, sh = (unsigned)palm_shift(c);
    unsigned iy = ((hy + half) >> sh) & 0xffff, ix = ((half + hx) >> sh) & 0xffff;
    if (s->palm_map[iy][ix] && (cb(c, 0xe94) == 0 || s->palm_pen[iy][ix])) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Classification: ClassifierInit, CalculateLlr, TreeClassifierHeuristics.
// Each class is a Gaussian over ten features; the stock code keeps the
// inverse covariance as an upper-triangular factor (10 x 10 at class + 4),
// then the log determinant, the means and the priors.

static void classifier_init(const struct heat_cfg *c, const struct heat_tracks *s, const struct heat_blob *b,
                            float *f, uint8_t *mask) {
    f[0] = (float)b->count9e;
    f[1] = (float)b->topo[1];
    f[2] = (float)b->topo[3];
    f[3] = (float)b->topo[5];
    f[4] = (float)b->topo[0];
    f[5] = (float)b->topo[2];
    f[6] = (float)b->topo[4];
    f[7] = b->ratio;
    f[8] = b->spread;
    f[9] = b->halo;
    for (int j = 0; j < 10; j++) mask[j] = (uint8_t)((cb(c, 0x11c + j) & s->noisy) | cb(c, 0x10c + j));
    if (b->halo == 100.0f) mask[9] = 1;
}

static float class_llr(const struct heat_cfg *c, const struct heat_tracks *s, const float *f, const uint8_t *mask,
                       int k) {
    int base = 0x130 + k * 0x1d0;
    float d[10];
    for (int j = 0; j < 10; j++) d[j] = mask[j] ? 0.0f : f[j] - cf(c, base + 0x19c + 4 * j);
    float q[10];
    for (int r = 0; r < 10; r++) {
        float v = cf(c, base + 4 + r * 0x28 + r * 4) * d[r] + 0.0f;
        for (int j = r + 1; j < 10; j++) v = v + cf(c, base + 4 + r * 0x28 + j * 4) * d[j];
        q[r] = v;
    }
    float sum = q[0] * q[0] + 0.0f;
    for (int r = 1; r < 10; r++) sum = sum + q[r] * q[r];
    float prior = cf(c, base + (s->palm == 1 ? 0x1c8 : 0x1c4));   // (by the palm state)
    return (llr_offset(c) + (prior - cf(c, base + 0x194) * 0.5f)) - sum * 0.5f;
}

static int thumbs(const struct heat_cfg *c, struct heat_blob *b, int cfgo) {
    float extra = 0.0f;
    if (b->near_border == 1) extra = (float)cb(c, 0xc86);
    if (cb(c, cfgo + 0x14) <= b->count9e && b->count9e <= cb(c, cfgo + 0x15) && cf(c, cfgo) <= b->spread &&
        b->spread <= cf(c, cfgo + 4) && cf(c, cfgo + 8) <= b->ratio && b->ratio <= cf(c, cfgo + 0xc) &&
        cf(c, cfgo + 0x10) <= LUT[b->peak] && b->nvalid_peaks < 2) {
        b->llr[0] = (-0.5f - (float)ci(c, 0xc60)) - extra;
        b->llr[1] = (-0.5f - (float)ci(c, 0xc64)) - extra;
        b->llr[2] = (float)-ci(c, 0xc68) - extra;
        b->llr[3] = (-0.5f - (float)ci(c, 0xc6c)) - extra;
        return 1;
    }
    return 0;
}

static void tree_heuristics(const struct heat_cfg *c, const struct heat_tracks *s, struct heat_blob *b) {
    if (b->npeaks == 1) b->llr[3] = b->llr[3] - cf(c, 0x8d0);
    else if (b->nvalid_peaks == 1) b->llr[3] = b->llr[3] - cf(c, 0x8d4);
    if (s->palm) return;
    if ((int)((unsigned)b->maxx - b->minx) < (int)cb(c, 0x884) &&
        (int)((unsigned)b->maxy - b->miny) < (int)cb(c, 0x885) && b->count < 7 && cf(c, 0x880) < LUT[b->peak]) {
        b->llr[0] = 10.0f;
        b->llr[1] = -9999.0f;
        b->llr[2] = -9999.0f;
        b->llr[3] = -9999.0f;
    }
    int done = 0;
    if (b->on_corner == 1) done = thumbs(c, b, 0x8b8);
    if (!done && b->on_border == 1) done = thumbs(c, b, 0x8a0);
    if (!done) thumbs(c, b, 0x888);
    // (The virtual touch pad's bonus: no such area here.)
}

static void classification(const struct heat_cfg *c, const struct heat_tracks *s, struct heat_detector *d) {
    int n = d->nblobs, skipped = 0;
    for (int i = 0; i < n + skipped && i < HEAT_MAX_BLOBS; i++) {
        struct heat_blob *b = &d->blob[i];
        if (!b->label || (b->status & 0xfe) == 4) { skipped++; continue; }
        float f[10];
        uint8_t mask[10];
        classifier_init(c, s, b, f, mask);
        for (int k = 0; k < 4; k++)
            b->llr[k] = b->count > cu(c, 0x870 + 4 * k) ? -9999.0f : class_llr(c, s, f, mask, k);
        tree_heuristics(c, s, b);
        n = d->nblobs;
    }
}

// ---------------------------------------------------------------------------
// MatchBlobsToTracks: Hungarian::HungarianInitBeforeRun and RunHungarian
// (Knuth's assignment algorithm on whole-millimetre costs), then
// UpdateTrackListAccordingToHungarian.

#define HN HEAT_HN

void hungarian_init(struct hungarian *h, const uint16_t (*tp)[2], int nt, const uint16_t (*bp)[2], int nb,
                           uint16_t maxd) {
    h->n = nt > nb ? nt : nb;
    for (int r = 0; r < h->n; r++)
        for (int k = 0; k < h->n; k++) {
            int16_t v;
            if (r < nt && k < nb) {
                float dx = (float)(int)((unsigned)bp[k][0] - (unsigned)tp[r][0]);
                float dy = (float)(int)((unsigned)bp[k][1] - (unsigned)tp[r][1]);
                float d2 = dx * dx + dy * dy;
                v = d2 <= (float)maxd ? (int16_t)(int)sqrtf(d2) : 100;
            } else {
                v = 1000;
            }
            h->cost[r * h->n + k] = v;
            h->assign[r * h->n + k] = 0;
        }
}

void hungarian_run(struct hungarian *h) {
    int n = h->n;
    if (n == 0) return;
    int16_t *cost = h->cost;
#define C(k, l) cost[(k) * n + (l)]
    int8_t col_mate[HN], row_mate[HN], parent_row[HN], unchosen_row[HN], slack_row[HN];
    int16_t row_dec[HN], col_inc[HN], slack[HN];
    for (int i = 0; i < n; i++) { col_mate[i] = 0; unchosen_row[i] = 0; row_dec[i] = 0; slack_row[i] = 0; }
    for (int i = 0; i < n; i++) { row_mate[i] = 0; parent_row[i] = 0; col_inc[i] = 0; slack[i] = 0; }
    // Column minima.
    if (n == 1) {
        int16_t s = C(0, 0);
        if (s != 0) C(0, 0) = (int16_t)(C(0, 0) - s);
    } else {
        for (int l = 0; l < n; l++) {
            int16_t s = C(0, l);
            for (int k = 1; k < n; k++) if (C(k, l) < s) s = C(k, l);
            if (s != 0) for (int k = 0; k < n; k++) C(k, l) = (int16_t)(C(k, l) - s);
        }
    }
    for (int l = 0; l < n; l++) { row_mate[l] = -1; parent_row[l] = -1; col_inc[l] = 0; slack[l] = 0x7fff; }
    int t = 0;
    if (n == 1) {
        row_dec[0] = C(0, 0);
        if (row_mate[0] < 0) { col_mate[0] = 0; row_mate[0] = 0; }
        else { t = 1; col_mate[0] = -1; unchosen_row[0] = 0; }
    } else {
        for (int k = 0; k < n; k++) {
            int16_t s = C(k, 0);
            for (int l = 1; l < n; l++) if (C(k, l) < s) s = C(k, l);
            row_dec[k] = s;
            int l;
            for (l = 0; l < n; l++)
                if (s == C(k, l) && row_mate[l] < 0) { col_mate[k] = (int8_t)l; row_mate[l] = (int8_t)k; break; }
            if (l == n) { col_mate[k] = -1; unchosen_row[t++] = (int8_t)k; }
        }
    }
    if (t != 0) {
        int unmatched = t;
        for (;;) {
            int q = 0, k = 0, l = 0;
            for (;;) {
                while (q < t) {
                    k = unchosen_row[q];
                    int16_t s = row_dec[k];
                    for (l = 0; l < n; l++) {
                        if (!slack[l]) continue;
                        int16_t del = (int16_t)((int16_t)(C(k, l) - s) + col_inc[l]);
                        if (del < slack[l]) {
                            if (del == 0) {
                                if (row_mate[l] < 0) goto breakthru;
                                slack[l] = 0;
                                parent_row[l] = (int8_t)k;
                                unchosen_row[t++] = row_mate[l];
                            } else {
                                slack[l] = del;
                                slack_row[l] = (int8_t)k;
                            }
                        }
                    }
                    q++;
                }
                int16_t s = 0x7fff;
                for (l = 0; l < n; l++) if (slack[l] && slack[l] < s) s = slack[l];
                for (q = 0; q < t; q++) row_dec[(int)unchosen_row[q]] = (int16_t)(row_dec[(int)unchosen_row[q]] + s);
                if (t < 1) q = 0;
                for (l = 0; l < n; l++) {
                    if (!slack[l]) { col_inc[l] = (int16_t)(col_inc[l] + s); continue; }
                    slack[l] = (int16_t)(slack[l] - s);
                    if (slack[l]) continue;
                    k = slack_row[l];
                    if (row_mate[l] >= 0) {
                        parent_row[l] = (int8_t)k;
                        unchosen_row[t++] = row_mate[l];
                        continue;
                    }
                    for (int j = l + 1; j < n; j++) if (!slack[j]) col_inc[j] = (int16_t)(col_inc[j] + s);
                    goto breakthru;
                }
            }
        breakthru:
            for (;;) {
                int j = col_mate[k];
                col_mate[k] = (int8_t)l;
                row_mate[l] = (int8_t)k;
                if (j < 0) break;
                k = parent_row[j];
                l = j;
            }
            if (--unmatched == 0) break;
            for (l = 0; l < n; l++) { parent_row[l] = -1; slack[l] = 0x7fff; }
            t = 0;
            for (k = 0; k < n; k++) if (col_mate[k] < 0) unchosen_row[t++] = (int8_t)k;
        }
    }
    // The stock check of the solution: when it fails, nothing is assigned.
    for (int k = 0; k < n; k++)
        for (int l = 0; l < n; l++)
            if ((int16_t)((int16_t)(C(k, l) - row_dec[k]) + col_inc[l]) < 0) return;
    for (int k = 0; k < n; k++) {
        int l = col_mate[k];
        int v = (int)col_inc[l < 0 ? 0 : l] - (int)row_dec[k] + (int)(uint16_t)C(k, l < 0 ? 0 : l);
        if (l < 0) return;
        if (v != 0) return;
    }
    int nz = 0;
    for (int l = 0; l < n; l++) if (col_inc[l]) nz++;
    if (nz > n) return;
    for (int k = 0; k < n; k++) h->assign[k * n + col_mate[k]] = 1;
    for (int k = 0; k < n; k++)
        for (int l = 0; l < n; l++) C(k, l) = (int16_t)((int16_t)(C(k, l) - row_dec[k]) + col_inc[l]);
#undef C
}

static void open_track(const struct heat_cfg *c, struct heat_tracks *s, const struct heat_detector *d,
                       const struct heat_blob *b);
static void update_track(const struct heat_cfg *c, struct heat_tracks *s, const struct heat_detector *d,
                         struct heat_trk *t, const struct heat_blob *b);

static float dist2(float ax, float ay, float bx, float by) {
    float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

static void match_blobs(const struct heat_cfg *c, struct heat_tracks *s, struct heat_detector *d) {
    uint8_t blist[HN], tlist[HN];
    memset(blist, 0xff, sizeof blist);
    memset(tlist, 0xff, sizeof tlist);
    uint16_t bp[HN][2], tp[HN][2];
    int nb = 0, nt = 0;
    {
        int n = d->nblobs, skipped = 0;
        for (int i = 0; i < n + skipped && i < HEAT_MAX_BLOBS; i++) {
            const struct heat_blob *b = &d->blob[i];
            if (!b->label || (b->status & 0xfe) == 4) { skipped++; continue; }
            bp[nb][0] = (uint16_t)(int16_t)(int)(b->x * c->pitch_mm[0] + 0.5f);
            bp[nb][1] = (uint16_t)(int16_t)(int)(b->y * c->pitch_mm[1] + 0.5f);
            blist[nb++] = (uint8_t)i;
            n = d->nblobs;
        }
    }
    {
        int n = s->nactive, skipped = 0;
        for (int i = 0; i < n + skipped && i < HEAT_TRACKS; i++) {
            const struct heat_trk *t = &s->t[i];
            if (t->status - 1u < 2) {
                tp[nt][0] = (uint16_t)(int16_t)(int)((t->x + t->vx) * c->pitch_mm[0] + 0.5f);
                tp[nt][1] = (uint16_t)(int16_t)(int)((t->y + t->vy) * c->pitch_mm[1] + 0.5f);
                tlist[nt++] = (uint8_t)i;
                n = s->nactive;
            } else {
                skipped++;
            }
        }
    }
    static struct hungarian h;
    hungarian_init(&h, (const uint16_t (*)[2])tp, nt, (const uint16_t (*)[2])bp, nb,
                   (uint16_t)(int16_t)(int)c->match_d2);
    hungarian_run(&h);

    // UpdateTrackListAccordingToHungarian.
    int n = h.n, cols = nb;
    float gate;
    if (n == 1 && nb == 1) {
        gate = s->nlive == 1 && s->prev_nblobs == 1 ? cf(c, 0x8e0) : cf(c, 0x8dc);
        gate = gate * gate;
    } else {
        if (n == 0) return;
        if (nb == 0) {
            for (int r = 0; r < n; r++)
                if (tlist[r] != 0xff) s->t[tlist[r]].matched = 0;
            return;
        }
        gate = cf(c, 0x8dc) * cf(c, 0x8dc);
    }
    for (int r = 0; r < n; r++) {
        int k;
        for (k = 0; k < cols; k++) {
            if (h.assign[r * n + k] != 1) continue;
            const struct heat_blob *b = &d->blob[blist[k]];
            if (tlist[r] == 0xff) {
                open_track(c, s, d, b);
            } else {
                struct heat_trk *t = &s->t[tlist[r]];
                if (gate <= dist2(t->x, t->y, b->x, b->y)) {
                    open_track(c, s, d, b);
                    t->matched = 0;
                } else {
                    update_track(c, s, d, t, b);
                }
            }
            break;
        }
        if (k == cols && tlist[r] != 0xff) s->t[tlist[r]].matched = 0;
    }
}

// ---------------------------------------------------------------------------
// OpenNewTrack, UpdateTrackData (with SetTrackRadius).

static void open_track(const struct heat_cfg *c, struct heat_tracks *s, const struct heat_detector *d,
                       const struct heat_blob *b) {
    for (int i = 0; i < HEAT_TRACKS; i++) {
        struct heat_trk *t = &s->t[i];
        if (t->status != 0) continue;
        if (!(cf(c, 0xc) < LUT[b->peak] && b->status != 5)) return;
        set_status(s, t, 1);
        t->run_decided = 0;
        t->max_count = b->count;
        t->matched = 1;
        t->run_undecided = 1;
        t->reported = 0;
        t->ring = 0;
        t->close = 0;
        t->hold = 0;
        t->blob = b->index;
        t->frames = 1;
        t->vy = 0.0f;
        t->vx = 0.0f;
        t->suspended = 0;
        t->pen = 0;
        t->min_x = b->x;
        t->min_y = b->y;
        t->max_x = b->x;
        t->max_y = b->y;
        t->x = b->x;
        t->y = b->y;
        t->minx = b->minx;
        t->miny = b->miny;
        t->maxx = b->maxx;
        t->maxy = b->maxy;
        t->count = b->count;
        t->orientation = b->orientation;
        t->label = b->label;
        t->near_border = b->near_border;
        t->otfc = 0;
        t->otfc_count = 0;
        t->peaks_max = (float)b->nvalid_peaks;
        t->peaks_sum = (float)b->nvalid_peaks;
        t->one_junction = b->count == 1;
        t->single = d->nblobs < 2;
        t->on_border = b->on_border;
        t->on_corner = b->on_corner;
        t->off_border = b->on_border != 1;
        struct heat_hist *h = &t->h[t->ring];
        for (int k = 0; k < 4; k++) { h->llr[k] = b->llr[k]; t->llr[k] = b->llr[k]; }
        h->x = b->x;
        h->y = b->y;
        memset(h->box, 0, sizeof h->box);
        h->count = b->count;
        h->type = 4;
        t->peak = b->peak;
        t->radius = b->radius;
        t->grip = 0;
        t->grip_count = 0;
        return;
    }
}

static void update_track(const struct heat_cfg *c, struct heat_tracks *s, const struct heat_detector *d,
                         struct heat_trk *t, const struct heat_blob *b) {
    (void)s;
    uint16_t fr = t->frames;
    t->frames = (uint16_t)(fr + 1);
    t->matched = 1;
    t->blob = b->index;
    if (b->x <= t->min_x) t->min_x = b->x;
    if (b->y <= t->min_y) t->min_y = b->y;
    if (t->max_x <= b->x) t->max_x = b->x;
    if (t->max_y <= b->y) t->max_y = b->y;
    t->vx = b->x - t->x;
    t->vy = b->y - t->y;
    t->x = b->x;
    t->y = b->y;
    t->minx = b->minx;
    t->miny = b->miny;
    t->maxx = b->maxx;
    t->maxy = b->maxy;
    t->count = b->count;
    if (t->max_count <= b->count) t->max_count = b->count;
    t->label = b->label;
    t->near_border &= b->near_border;
    t->single &= d->nblobs < 2;
    t->one_junction &= b->count == 1;
    float nv = (float)b->nvalid_peaks;
    if (t->peaks_max <= nv) t->peaks_max = nv;
    t->peaks_sum = t->peaks_sum + (float)b->nvalid_peaks;
    if ((uint16_t)(fr + 1) == 2) {
        t->on_border &= b->on_border;
        t->on_corner &= b->on_corner;
    }
    // SetTrackRadius.
    if (b->on_border == 0 || t->off_border == 0) {
        if (t->run_decided == 0) t->radius = b->radius;
        else t->radius = c->radius_iir * b->radius + (1.0f - c->radius_iir) * t->radius;
    }
    if (t->off_border == 0) t->off_border = b->on_border != 1;
    uint8_t old = t->ring;
    t->ring = (uint8_t)((old + 1) % 10);
    struct heat_hist *h = &t->h[t->ring];
    h->type = t->h[old].type;
    for (int k = 0; k < 4; k++) { h->llr[k] = b->llr[k]; t->llr[k] = b->llr[k]; }
    h->x = b->x;
    h->y = b->y;
    h->count = b->count;
    // (t->pen |= pen in range: never here.)
    if (b->peak <= t->peak) t->peak = b->peak;
    t->orientation = b->orientation;
}

// ---------------------------------------------------------------------------
// AnalyzeHistoryToDetectTap: a short touch that lifted is held as a tap.

static void detect_tap(const struct heat_cfg *c, struct heat_tracks *s, const struct heat_detector *d,
                       struct heat_trk *t) {
    int strict = !(cb(c, 0x8d8) == 0 || s->nactive > 1 || t->single == 0 || d->nblobs != 0) ? 0 : 1;
    int p = strict ? 0xe44 : 0xe30;
    uint8_t minf = cb(c, p + 10);      // (+11 with a pen)
    s->tap = 0;
    uint8_t lim = s->palm == 1 ? (uint8_t)(cb(c, 0xe60) + 5) : cb(c, 0xe60);
    uint8_t maxf = cb(c, p + 0xc) <= lim ? lim : cb(c, p + 0xc);
    if (t->frames < minf || maxf < t->frames) return;
    if (t->reported > 2) return;
    uint32_t ty = *type_of(t);
    if (ty != 4 && (strict || ty != 1)) {
        if ((ty & ~2u) != 0) return;
        if (prev_type(t) != 4) return;
    }
    if (ch(c, p + 8) < t->max_count) return;
    if (t->max_count < ch(c, p + (s->palm == 1 ? 6 : 4))) return;   // (inside the touch pad area: none)
    if (t->one_junction == 1) return;                              // (likewise)
    int mr = 0, mc = 0;
    if (t->near_border == 1) { mr = cc(c, p + 0x10); mc = cc(c, p + 0x11); }
    if (4.0f < t->peaks_max) return;
    static const double minpeak[2] = { 0.0949999988079071, 0.10999999940395355 };
    if ((double)LUT[t->peak] < minpeak[strict]) return;
    int m0 = mc + 3, m2 = mr + 3;
    if (t->peaks_sum / (float)t->frames <= 2.4f) { m0 = mc; m2 = mr; }
    unsigned n = t->frames > 9 ? 10 : t->frames;
    float minx = 3.4028235e+38f, maxx = 0.0f, miny = 3.4028235e+38f, maxy = 0.0f;
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f;
    for (unsigned i = 0; i < n; i++) {
        const struct heat_hist *h = &t->h[(t->ring + 10 - i) % 10];
        if (h->x <= minx) minx = h->x;
        if (maxx <= h->x) maxx = h->x;
        if (h->y <= miny) miny = h->y;
        if (maxy <= h->y) maxy = h->y;
        if (i != 0 && h->type != 4 && (strict || h->type != 1)) return;
        s2 = s2 + h->llr[2];
        s0 = s0 + h->llr[0];
        s1 = s1 + h->llr[1];
    }
    float span = (float)cb(c, p + 0x12);
    if (span < maxy - miny) return;
    if (span < maxx - minx) return;
    float inv = 1.0f / (float)n;
    float a0 = inv * s0, a2 = inv * s2;
    float t0 = (float)(m2 + cs(c, p + 2)), t2 = (float)(m0 + cs(c, p));
    if (a0 < t0 && a2 < t2) return;
    float a1 = inv * s1;
    if ((float)cc(c, p + 0xd) < a1) return;
    float best;
    int8_t need;
    if (!(t2 <= a2) && !(a0 < t0)) {
        need = cc(c, p + 0xf);
        best = a0;
    } else {
        if (t0 <= a0 || a2 < t2) {
            if (a0 < t0 || a2 < t2) return;
            if ((float)cc(c, p + 0xf) <= a0 - a1) goto held;
        }
        need = cc(c, p + 0xe);
        best = a2;
    }
    if (best - a1 < (float)need) return;
held:
    if (strict && mapped_to_palm(c, s, t)) return;
    set_status(s, t, 4);
    *type_of(t) = 0;
    s->tap = (uint8_t)(strict ^ 1);
    uint8_t hold = cb(c, p + 0x13);
    t->hold = (uint8_t)(n < hold ? n : hold);
}

static void tracks_without_match(const struct heat_cfg *c, struct heat_tracks *s, const struct heat_detector *d) {
    int n = s->nlive, skipped = 0;
    for (int i = 0; i < n + skipped && i < HEAT_TRACKS; i++) {
        struct heat_trk *t = &s->t[i];
        if (t->status == 0) { skipped++; continue; }
        if (t->matched) continue;
        if (t->status == 1) {
            detect_tap(c, s, d, t);
            if (t->status != 4) set_status(s, t, 3);
        } else if ((t->status == 4 || t->status == 2) && t->hold == 0) {
            set_status(s, t, 3);
        }
        n = s->nlive;
    }
}

// ---------------------------------------------------------------------------
// ClassifyTracks and the functions it decides with.

static int changed_direct(const struct heat_cfg *c, const struct heat_tracks *s, const struct heat_trk *t,
                          const struct heat_blob *b, unsigned to) {
    uint32_t from = t->h[t->ring].type;
    int o = (int)(from * 0xc0 + to * 0x30);
    int16_t minv = cs(c, o + 0x924);
    if (s->palm == 1 && cb(c, o + 0x929) == 0) return 0;
    float tp = 0.0f, extra = 0.0f;           // (touch pad area: none)
    if (b->near_border == 1) {
        extra = (float)(int)cc(c, o + 0x926);
        if (s->palm == 1) extra = extra + (float)(int)cc(c, 0x8f2);
    }
    float v = t->llr[to];
    for (unsigned j = 0; j < 4; j++) {
        if (j == to) continue;
        if (v - t->llr[j] < tp + extra + (float)ci(c, o + 0x914 + 4 * (int)j)) return 0;
    }
    return extra + (float)(int)minv <= v;
}

static int changed_by_history(const struct heat_cfg *c, const struct heat_tracks *s, const struct heat_trk *t,
                              const struct heat_blob *b, unsigned to) {
    uint32_t from = t->h[t->ring].type;
    if (from == to) return 1;
    int o = (int)(from * 0xc0 + to * 0x30);
    uint8_t need = cb(c, o + 0x927);
    unsigned have = t->frames > 9 ? 10 : (t->frames & 0xff);
    if (s->palm == 1) need = (uint8_t)(cb(c, o + 0x92a) + need);
    float tp = 0.0f, extra = 0.0f;
    if (!(need <= have)) return 0;
    if (b->near_border == 1) extra = (float)(int)(s->palm == 1 ? cc(c, 0x8f2) : cc(c, o + 0x926));
    if (need == 0) return 1;
    float minv = extra + (float)(int)cs(c, o + 0x924);
    float thr[4];
    for (int j = 0; j < 4; j++) thr[j] = tp + extra + (float)ci(c, o + 0x900 + 4 * j);
    int idx = t->ring + 10;
    for (unsigned k = 0; k < need; k++, idx--) {
        const float *l = t->h[idx % 10].llr;
        float v = l[to];
        for (unsigned j = 0; j < 4; j++) {
            if (j == to) continue;
            if (v - l[j] < thr[j]) return 0;
        }
        if (v < minv) return 0;
    }
    return 1;
}

static void uncertain_cleaner(const struct heat_cfg *c, const struct heat_tracks *s, const struct heat_detector *d,
                              struct heat_trk *t) {
    uint8_t ring = t->ring;
    uint32_t ty = t->h[ring].type;
    unsigned lo = cb(c, 0xe0d), hi = cb(c, 0xe0e);
    if (s->palm == 1) { lo += cb(c, 0xe0c); hi += cb(c, 0xe0c); }
    uint16_t fr = t->frames;
    if (ty == 4 && fr <= (hi & 0xff) && (lo & 0xff) <= fr) {
        unsigned n = fr > 9 ? 10 : fr, skip = 0;
        float s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
        for (unsigned k = 1; k < n; k++) {
            const struct heat_hist *h = &t->h[(ring + 10 - k) % 10];
            if (h->llr[2] == -9999.0f && h->llr[1] == -9999.0f && h->llr[3] == -9999.0f) skip++;
            else { s2 = s2 + h->llr[2]; s1 = s1 + h->llr[1]; s3 = s3 + h->llr[3]; }
        }
        float m = (float)(int)(n - skip - 1);
        float a2 = s2 / m;
        if (cf(c, 0xe00) <= a2 && !(a2 <= s3 / m)) {
            int8_t marg = d->blob[t->blob].on_border == 1 ? cc(c, 0xe11)
                          : s->palm == 1 ? cc(c, 0xe13) : cc(c, 0xe12);
            int8_t need = t->peaks_sum / (float)fr <= 2.4f ? marg : (int8_t)(marg + 3);
            if ((float)(int)need < a2 - s1 / m && cb(c, 0xe15) <= t->max_count && !mapped_to_palm(c, s, t) &&
                t->peaks_max <= 4.0f && 0.12f <= LUT[t->peak])
                t->h[ring].type = 2;
        }
    }
    if (ty == 4 && fr > 2) {
        const struct heat_hist *h = &t->h[ring];
        int now = !(-100.0f < h->llr[1] || -100.0f < h->llr[0] || -100.0f < h->llr[2]) &&
                  h->llr[3] <= (float)(int)cs(c, 0xcb4);
        const struct heat_hist *p = &t->h[(ring + 9) % 10];
        if (p->llr[1] <= -100.0f && p->llr[0] <= -100.0f && p->llr[2] <= -100.0f && now &&
            !((float)(int)cs(c, 0xcb4) < p->llr[3]))
            t->h[t->ring].type = 1;
    }
    if (cb(c, 0xe10) <= t->run_undecided && cb(c, 0xe0f) < fr) t->h[t->ring].type = 1;
}

// UpdateLlrArrayAccordingToReliefZone: a young blob inside an older track's
// grown box takes that track's class's relief off its likelihoods.
static void relief_zone(const struct heat_cfg *c, const struct heat_tracks *s, struct heat_trk *t,
                        const struct heat_blob *b) {
    uint8_t ring = t->ring;
    if (t->frames <= cb(c, (int)t->h[ring].type * 0x10 + 0xd8d) && s->nlive) {
        int n = s->nlive, skipped = 0;
        for (int i = 0; i < n + skipped && i < HEAT_TRACKS; i++) {
            const struct heat_trk *o = &s->t[i];
            if (o->status == 0) { skipped++; continue; }
            if (o->status != 3 && o->frames <= 1) continue;
            const struct heat_hist *oh = &o->h[o->status == 3 ? o->ring : (o->ring + 9) % 10];
            int k = (int)oh->type * 0x10;
            if (cb(c, k + 0xd8c) == 1 && oh->box[0] < (float)b->minx && (float)b->maxx < oh->box[2] &&
                oh->box[1] < (float)b->miny && (float)b->maxy < oh->box[3]) {
                int mul = cb(c, k + 0xd8e) < o->frames ? 2 : 1;
                for (int j = 0; j < 4; j++)
                    t->llr[j] = t->h[ring].llr[j] - (float)(mul * cs(c, k + 0xd84 + 2 * j));
                return;
            }
        }
    }
    for (int j = 0; j < 4; j++) t->llr[j] = t->h[ring].llr[j];
}

static void grow_box(const struct heat_cfg *c, struct heat_trk *t, const struct heat_blob *b) {
    struct heat_hist *h = &t->h[t->ring];
    float pad = (float)cb(c, (int)h->type * 0x10 + 0xd8f);
    h->box[0] = t->vx + ((float)b->minx - pad);
    h->box[2] = t->vx + pad + (float)b->maxx;
    h->box[1] = t->vy + ((float)b->miny - pad);
    h->box[3] = t->vy + pad + (float)b->maxy;
}

// GetBestLlrType, as the code compares (csel gt): a later class wins only when
// strictly greater, so a NaN likelihood keeps the earlier pick.
static unsigned best_llr(const float *l) {
    unsigned a = l[1] > l[0];
    unsigned b = l[2] > l[a] ? 2 : a;
    return l[3] > l[b] ? 3 : b;
}

static void classify_tracks(const struct heat_cfg *c, struct heat_tracks *s, struct heat_detector *d) {
    unsigned best = 4;
    int n = s->nlive, skipped = 0;
    for (int i = 0; i < n + skipped && i < HEAT_TRACKS; i++) {
        struct heat_trk *t = &s->t[i];
        if (t->status == 0) { skipped++; continue; }
        n = s->nlive;
        struct heat_blob *b = &d->blob[t->blob];
        uint32_t ty = t->h[t->ring].type;
        grow_box(c, t, b);
        if (!(t->status == 1 && (t->one_junction != 1 || (5 < t->frames && 0.15f <= LUT[t->peak])))) continue;
        relief_zone(c, s, t, b);
        if (!(t->near_border != 1 || cb(c, 0x8f3) < t->frames || 10.0f <= t->llr[0])) continue;
        // (A pen nearby would lower the likelihoods here.)
        int change;
        if (t->frames == 1) {
            if (t->count <= ch(c, 0x8ee) || LUT[t->peak] <= cf(c, 0x8e4)) {
                change = 0;
            } else {
                best = best_llr(t->llr);
                change = changed_direct(c, s, t, b, best);
            }
        } else {
            if (t->close == 1 && ty < 4 && ty != 1)
                for (int j = 0; j < 4; j++)
                    t->h[t->ring].llr[j] = t->h[t->ring].llr[j] - (float)(int)cs(c, 0xe1c + 2 * j);
            best = best_llr(t->h[t->ring].llr);
            if (cb(c, (int)(ty * 0xc0 + best * 0x30 + 0x928)) < t->frames || !changed_direct(c, s, t, b, best))
                change = changed_by_history(c, s, t, b, best);
            else
                change = 1;
        }
        uint16_t cnt = t->count;
        if (cnt < ch(c, (int)best * 2 + 0xde4) || ch(c, (int)best * 2 + 0xdf4) < cnt) change = 0;
        int strict = !(cb(c, 0x8d8) == 0 || s->nactive > 1 || t->single == 0 || d->nblobs != 0) ? 0 : 1;
        int undecided = ty == 4;
        if (change == 1) {
            if (best == 1 && ty == 4 && t->frames > 1) {
                static const uint16_t palm_count[2] = { 10, 4 };   // (0x2a36e4)
                uint16_t lim = palm_count[strict];
                change = lim <= cnt && cnt != lim;
            } else if (best == 1 && ty == 0 && t->frames > 1) {
                change = cnt > 0xc && cnt != 0xd;
            } else if (ty == 4 && best == 3) {
                change = cf(c, 0x8e8) <= LUT[t->peak] ? 1 : ch(c, 0x8ec) <= t->frames;
            }
        }
        if (s->palm == 1 && best == 0 && undecided && change == 1) {
            if (t->max_count < 3) goto keep;
            t->h[t->ring].type = best;
        } else if (change == 1) {
            t->h[t->ring].type = best;
        } else {
        keep:
            if (t->frames > 1) t->h[t->ring].type = ty;
            uncertain_cleaner(c, s, d, t);
        }
        // (A pen nearby forces a palm here.)
        if (b->big == 1) t->h[t->ring].type = 1;
        if (best != ty) grow_box(c, t, b);
        uint32_t now = t->h[t->ring].type;
        if (now == 3) {
            if (s->noisy != 1) continue;   // (or the posture flag with a palm about)
            t->h[t->ring].type = 4;
            now = 4;
        }
        if (now == 1 && palm_map_on(c)) {
            uint8_t lvl = (uint8_t)(ch(c, 0xe84) / 10);
            s->palm_level = lvl;
            int sh = palm_shift(c);
            for (unsigned y = b->miny; y <= b->maxy; y++)
                for (unsigned x = b->minx; x <= b->maxx; x++)
                    if (d->eq[d->label[y][x]] == b->label) {
                        s->palm_map[y >> sh][x >> sh] = lvl;
                        s->palm_pen[y >> sh][x >> sh] = t->pen;
                    }
        }
    }
}

// ---------------------------------------------------------------------------
// PostTouchDetectionProcess.

static void post_detection(const struct heat_cfg *c, struct heat_tracks *s) {
    int n = s->nlive, skipped = 0;
    for (int i = 0; i < n + skipped && i < HEAT_TRACKS; i++) {
        struct heat_trk *t = &s->t[i];
        if (t->status == 3) set_status(s, t, 0);
        n = s->nlive;
        if (t->status == 0) { skipped++; continue; }
        t->close = 0;
        if (t->status == 4) continue;
        uint32_t ty = t->h[t->ring].type;
        if (ty == 5) {
            s->nlist++;
            s->list[s->nlist] = (uint8_t)i;
        } else if (ty == 1) {
            s->list[s->nlist] = (uint8_t)i;
            s->nlist++;
            s->npalms++;
            s->palm_ts = s->now;
            s->palm_left = ch(c, 0xe7a);
        }
    }
}

// ---------------------------------------------------------------------------
// TouchDetectionPostProcess.

// A float to unsigned conversion as arm64's fcvtzu does it: negative and NaN
// to 0, saturating at the top.
static unsigned f2u(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 4294967296.0f) return 0xffffffffu;
    return (unsigned)v;
}

// UpdatePalmByProximityAccordingToDistanceFromPalmCriteria: a track near a
// palm's junctions becomes a palm by proximity.
static void near_palm(struct heat_tracks *s, const struct heat_detector *d, struct heat_trk *t, float r,
                      uint8_t *found) {
    if (!(s->npalms || s->nlist) || !s->nactive) return;
    float px = t->x, py = t->y;
    int x0 = (int)f2u(px - r), x1 = (int)f2u(px + r);
    int y0 = (int)f2u(py - r), y1 = (int)f2u(py + r);
    int self5 = t->h[t->ring].type == 5;
    int done = 0;
    int n = s->nactive, skipped = 0;
    for (int i = 0; i < n + skipped && i < HEAT_TRACKS; i++) {
        struct heat_trk *o = &s->t[i];
        if (!(o->status - 1u < 2)) { skipped++; continue; }
        if (o == t || (o->h[o->ring].type | 4) != 5) continue;
        if (done) return;
        int xa = x0 <= (int)o->minx ? o->minx : x0;
        int xb = (unsigned)o->maxx <= (unsigned)x1 ? o->maxx : x1;
        int ya = y0 <= (int)o->miny ? o->miny : y0;
        int yb = (unsigned)o->maxy <= (unsigned)y1 ? o->maxy : y1;
        if (!(ya <= yb) || xb < xa) continue;
        uint8_t ol = d->blob[o->blob].label;
        for (int y = ya; y <= yb; y++) {
            float dy = (float)y - py;
            for (int x = xa; x <= xb; x++) {
                float dx = (float)x - px;
                if (d->eq[d->label[y][x]] == ol && dy * dy + dx * dx < r * r) {
                    uint8_t v;
                    if (o->h[o->ring].type == 1) { done = 1; v = 10; }
                    else v = t->palm_hold <= o->palm_hold ? o->palm_hold : t->palm_hold;
                    t->palm_hold = v;
                    if (!self5) { *found = 1; t->h[t->ring].type = 5; }
                    n = s->nactive;
                    goto next;
                }
            }
        }
    next:;
    }
}

static void palm_by_proximity(const struct heat_cfg *c, struct heat_tracks *s, const struct heat_detector *d) {
    uint16_t young = ch(c, 0x8f0);
    // The radius: [0] with a pen; else [2] while TouchDetection +0x19639 > 1
    // (2 in this tuning) and the input's panel arrangement (+0xb7cc) is not
    // 2 (stitched), which per-panel processing never is; [1] otherwise.
    float r = c->palm_radius[2];
    uint8_t again;
    do {
        uint8_t found = 0;
        again = 0;
        if (!s->nactive) break;
        int n = s->nactive, skipped = 0;
        for (int i = 0; i < n + skipped && i < HEAT_TRACKS; i++) {
            struct heat_trk *t = &s->t[i];
            if (!(t->status - 1u < 2)) { skipped++; n = s->nactive; continue; }
            if (t->h[t->ring].type != 1 && (t->frames < young || (prev_type(t) | 4) == 5) && t->reported < 10) {
                near_palm(s, d, t, r, &found);
                again = found;
                if (!found) {
                    uint32_t ty = t->h[t->ring].type;
                    again = 0;
                    if (ty != 1 && ty != 4 && mapped_to_palm(c, s, t)) {
                        t->palm_hold = 10;
                        t->h[t->ring].type = 5;
                    }
                }
            }
            n = s->nactive;
        }
    } while (again == 1);
}

static void suspension(const struct heat_cfg *c, struct heat_tracks *s, const struct heat_detector *d) {
    if (!s->nactive) return;
    int n = s->nactive, skipped = 0;
    for (int i = 0; i < n + skipped && i < HEAT_TRACKS; i++) {
        struct heat_trk *t = &s->t[i];
        if (!(t->status - 1u < 2)) { skipped++; n = s->nactive; continue; }
        n = s->nactive;
        if (t->status != 1 || t->run_decided != 0) continue;
        uint32_t *tp = &t->h[t->ring].type, ty = *tp;
        uint16_t fr = t->frames;
        if (!(ty < 5 && ((1u << ty) & 0x15) && fr <= cb(c, 0xe5e) && cb(c, 0xe5d) <= fr && t->on_border == 1))
            continue;
        int k = fr < 10 ? (fr > 1 ? fr - 1 : fr) : 9;
        float inv = 1.0f / (float)k;
        float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        for (int j = 0, idx = t->ring + 10; j < k; j++, idx--) {
            const struct heat_hist *h = &t->h[idx % 10];
            a2 = a2 + h->llr[2];
            a0 = a0 + h->llr[0];
            a1 = a1 + h->llr[1];
        }
        int16_t l0 = cs(c, 0xe5a), l2 = cs(c, 0xe58);
        a0 = inv * a0;
        if (s->palm == 1) { l0 = (int16_t)(l0 + cb(c, 0xe62)); l2 = (int16_t)(l2 + cb(c, 0xe62)); }
        a2 = inv * a2;
        float gap = (float)(int)cc(c, 0xe61);
        if (!(((float)(int)l0 <= a0 && gap <= a0 - inv * a1) || ((float)(int)l2 <= a2 && gap <= a2 - inv * a1)))
            continue;
        if (mapped_to_palm(c, s, t)) continue;
        uint8_t lim = s->palm == 1 ? (uint8_t)(cb(c, 0xe60) + 5) : cb(c, 0xe60);
        const struct heat_blob *b = &d->blob[t->blob];
        unsigned f8 = fr;
        if (b->on_border == 0 || (b->on_corner == 0 && t->on_corner == 1)) {
            if (ty == 4) *tp = (uint32_t)(a0 <= a2) << 1;
            if (f8 > 9) f8 = 10;
            t->hold = (uint8_t)f8;
            set_status(s, t, 2);
        } else if (f8 < lim) {
            if (t->reported == 0) *tp = 4;
        } else if (ty != 4) {
            t->hold = f8 < 0xc ? (uint8_t)((fr >> 1) + 1) : 6;
            set_status(s, t, 2);
        }
    }
}

// RecalculateComWithPadding: the centroid again, over the blob's box grown
// by a junction, with the junctions beside the blob's strong ones (above a
// part of its peak) taken in too, all weighted above the padding floor. The
// label map is marked (0xff) and restored as it goes, as the stock code does.
static void padded_centroid(const struct heat_cfg *c, struct heat_tracks *s, struct heat_detector *d,
                            const struct heat_blob *b, struct heat_trk *t, const struct heat_frame *f) {
    int x0 = b->minx - 1, y0 = b->miny - 1, x1 = b->maxx + 1, y1 = b->maxy + 1;
    if (b->on_border == 1) {
        x0 = b->minx ? b->minx - 1 : 0;
        y0 = b->miny ? b->miny - 1 : 0;
        x1 = c->cols - 1 == b->maxx ? c->cols - 1 : b->maxx + 1;
        y1 = c->rows - 1 == b->maxy ? c->rows - 1 : b->maxy + 1;
    }
    float strong = LUT[b->peak];
    float base = LUT[c->base_byte[s->noisy]];
    strong = strong * c->pad_fraction;
    uint8_t (*L)[HEAT_MAX_X] = d->label;
    const uint8_t *eq = d->eq;
    uint8_t lab = b->label;
    float sx = 0.0f, sy = 0.0f, sw = 0.0f, w;
    float fx1 = (float)x1;
#define V(x, y) LUT[f->image[y][x]]
    if (y0 < y1) {
        if (x0 < x1) {
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    uint8_t l = L[y][x];
                    if (lab == eq[l]) {
                        w = V(x, y) - base;
                        sx = sx + w * (float)x;
                        sy = sy + w * (float)y;
                        sw = sw + w;
                        if (strong < V(x, y)) {
                            if (L[y][x + 1] == 0) L[y][x + 1] = 0xff;
                            if (L[y + 1][x] == 0) L[y + 1][x] = 0xff;
                        }
                        if (l != 0xff) continue;
                    } else if (l == 0) {
                        if (!((lab == eq[L[y][x + 1]] && strong < V(x + 1, y)) ||
                              (lab == eq[L[y + 1][x]] && strong < V(x, y + 1))))
                            continue;
                        L[y][x] = 0xff;
                    } else if (l != 0xff) {
                        continue;
                    }
                    if (base < V(x, y)) {
                        w = V(x, y) - base;
                        sx = sx + w * (float)x;
                        sy = sy + w * (float)y;
                        sw = sw + w;
                    }
                    L[y][x] = 0;
                }
        }
        for (int y = y0; y < y1; y++) {
            int x = x1;
            uint8_t l = L[y][x];
            if (lab == eq[l]) {
                w = V(x, y) - base;
                sx = sx + w * fx1;
                sy = sy + w * (float)y;
                sw = sw + w;
                if (strong < V(x, y) && L[y + 1][x] == 0) L[y + 1][x] = 0xff;
                if (l != 0xff) continue;
            } else if (l == 0) {
                if (!(lab == eq[L[y + 1][x]] && strong < V(x, y + 1))) continue;
                L[y][x] = 0xff;
            } else if (l != 0xff) {
                continue;
            }
            if (base < V(x, y)) {
                w = V(x, y) - base;
                sx = sx + w * fx1;
                sy = sy + w * (float)y;
                sw = sw + w;
            }
            L[y][x] = 0;
        }
    }
    float fy1 = (float)y1;
    if (x0 < x1) {
        for (int x = x0; x < x1; x++) {
            int y = y1;
            uint8_t l = L[y][x];
            if (lab == eq[l]) {
                w = V(x, y) - base;
                sx = sx + w * (float)x;
                sy = sy + w * fy1;
                sw = sw + w;
                if (strong < V(x, y) && L[y][x + 1] == 0) L[y][x + 1] = 0xff;
                if (l != 0xff) continue;
            } else if (l == 0) {
                if (!(lab == eq[L[y][x + 1]] && strong < V(x + 1, y))) continue;
                L[y][x] = 0xff;
            } else if (l != 0xff) {
                continue;
            }
            if (base < V(x, y)) {
                w = V(x, y) - base;
                sx = sx + w * (float)x;
                sy = sy + w * fy1;
                sw = sw + w;
            }
            L[y][x] = 0;
        }
    }
    uint8_t l = L[y1][x1];
    if (l == 0xff) {
        if (base < V(x1, y1)) {
            w = V(x1, y1) - base;
            sx = sx + w * fx1;
            sy = sy + w * fy1;
            sw = sw + w;
        }
        l = 0;
        L[y1][x1] = 0;
    }
    if (lab == eq[l]) {
        w = V(x1, y1) - base;
        sx = sx + w * fx1;
        sy = sy + w * fy1;
        sw = sw + w;
    }
#undef V
    t->x = sx * (1.0f / sw);
    t->y = sy * (1.0f / sw);
}

// CalculatePeaksCom: each junction of the blob goes to its nearest valid
// peak (all of them on a tie); a centroid per peak.
static void peaks_centroid(const struct heat_detector *d, const struct heat_trk *t, const struct heat_frame *f,
                           float (*out)[2]) {
    const struct heat_blob *b = &d->blob[t->blob];
    uint8_t nv = b->nvalid_peaks;
    float ax[12] = { 0 }, ay[12] = { 0 }, aw[12] = { 0 };
    for (unsigned y = t->miny; y <= t->maxy; y++) {
        if (!(t->minx <= t->maxx) || nv == 0) continue;
        for (unsigned x = t->minx; x <= t->maxx; x++) {
            if (b->label != d->eq[d->label[y][x]]) continue;
            float w = LUT[f->image[y][x]];
            uint8_t ties[12] = { 0 };
            int nt = 0, best = 0x7fffffff, skipped = 0;
            for (unsigned k = 0; k < (unsigned)nv + (unsigned)skipped; k++) {
                if (!b->peaks[k].valid) skipped++;
                else {
                    int dy = (int)y - b->peaks[k].y, dx = (int)x - b->peaks[k].x;
                    int dd = dx * dx + dy * dy;
                    if (dd < best) { ties[0] = (uint8_t)k; nt = 1; best = dd; }
                    else if (dd == best) ties[nt++] = (uint8_t)k;
                }
                if (k > 7) break;
            }
            for (int j = 0; j < nt; j++) {
                ax[ties[j]] = w * (float)x + ax[ties[j]];
                ay[ties[j]] = w * (float)y + ay[ties[j]];
                aw[ties[j]] = w + aw[ties[j]];
            }
        }
    }
    int skipped = 0;
    for (unsigned k = 0; nv && k < (unsigned)nv + (unsigned)skipped; k++) {
        if (!b->peaks[k].valid) skipped++;
        else {
            out[k][0] = ax[k] * (1.0f / aw[k]);
            out[k][1] = (1.0f / aw[k]) * ay[k];
        }
        if (k > 7) break;
    }
}

// CopyBlobToFingerInput.
static void copy_out(const struct heat_cfg *c, struct heat_tracks *s, const struct heat_detector *d, int ti,
                     uint16_t count, const float *pos, uint8_t peak, int split, uint8_t live, uint8_t back,
                     uint8_t peaks) {
    struct heat_trk *t = &s->t[ti];
    struct heat_report *r = &s->out[s->nout];
    if (back != 0xff) {
        const struct heat_hist *h = &t->h[(t->ring - back + 10) % 10];
        count = h->count;
        pos = &h->x;
    }
    if (!split) {
        r->maxx = t->maxx;
        r->maxy = t->maxy;
        r->minx = t->minx;
        r->miny = t->miny;
        r->radius = t->radius;
        r->type = 1;
    } else {
        int ix = (int)pos[0], iy = (int)pos[1];
        r->maxx = (uint16_t)((unsigned)ix + 1 != (unsigned)c->cols ? ix + 1 : ix);
        r->maxy = (uint16_t)((unsigned)iy + 1 != (unsigned)c->rows ? iy + 1 : iy);
        r->minx = (uint16_t)((int)pos[0] != 0 ? (int16_t)(int)pos[0] - 1 : 0);
        r->radius = 4.5f;
        r->miny = (uint16_t)((int)pos[1] != 0 ? (int16_t)(int)pos[1] - 1 : 0);
        r->type = 3;
    }
    r->x = pos[0];
    r->y = pos[1];
    r->peak = peak;
    r->track_count = t->count;
    {
        unsigned row = (unsigned)(int)(pos[1] + 0.5f);
        r->window = row < (unsigned)c->rows && c->scan_row[row] != 0xff ? s->scan[c->scan_row[row]] : 0;
    }
    r->split = (uint8_t)split;
    r->peaks = peaks;
    r->label = r->label2 = t->label;
    r->count = count;
    r->b2c = 0;
    r->valid = t->reported < 4 ? 1 : cf(c, 0xc) < LUT[peak];
    // The strong-touch level: the one-blob level when one blob and the
    // noise is low (the touch pad's level never applies here).
    float thr = cf(c, 0xe6c);
    if (!(ch(c, 0xe74) < s->noise_level) && d->nblobs == 1) thr = cf(c, 0xe70);
    r->strong = thr < LUT[peak];
    r->track = (uint8_t)ti;
    r->live = live;
    r->orientation = t->orientation;
    r->near_border = t->near_border;
    s->nout++;
}

static void fill_out(const struct heat_cfg *c, struct heat_tracks *s, const struct heat_detector *d,
                     const struct heat_frame *f) {
    s->nout = 0;
    s->nheld = 0;
    int n = s->nlive, skipped = 0;
    for (int i = 0; i < n + skipped; i++) {
        if (i == 0x2f || s->nout > 0x21) break;
        struct heat_trk *t = &s->t[i];
        int sent = 0;
        uint32_t st = t->status;
        if (st > 4) goto counters;
        uint32_t ty = t->h[t->ring].type;
        const struct heat_blob *b = &d->blob[t->blob];
        switch (st) {
        case 0:
            skipped++;
            continue;
        case 1:
            if ((ty & ~2u) == 0 || (ty == 3 && b->nvalid_peaks < 2)) {
                copy_out(c, s, d, i, b->count, &t->x, b->peak, 0, 1, 0xff, b->nvalid_peaks);
                sent = 1;
            } else if (ty == 3) {
                float pc[12][2] = { { 0 } };
                peaks_centroid(d, t, f, pc);
                int sk = 0;
                for (unsigned k = 0; k < (unsigned)b->nvalid_peaks + (unsigned)sk; k++) {
                    if (!b->peaks[k].valid) sk++;
                    else copy_out(c, s, d, i, b->count, pc[k], b->peak, 1, 1, 0xff, b->nvalid_peaks);
                    if (k > 7) break;
                }
                sent = 1;
            } else if (t->suspended == 1 && t->frames < max_suspended(c)) {
                copy_out(c, s, d, i, b->count, &t->x, b->peak, 0, 0, 0xff, b->nvalid_peaks);
                sent = 1;
            }
            break;
        case 2:
            if (t->suspended == 1 && t->frames < max_suspended(c)) {
                set_status(s, t, 0);
                float z[2] = { 0, 0 };
                copy_out(c, s, d, i, 0, z, t->peak, 0, 0, (uint8_t)(t->hold - 1), 1);
                t->hold = 0;
                sent = 1;
            } else {
                const struct heat_hist *h = &t->h[t->ring];
                if (-200.0f <= h->llr[0] || -200.0f <= h->llr[2] || -200.0f <= h->llr[3]) {
                    if (ty < 4 && ty != 1) {
                        uint8_t step;
                        if (t->reported < 3) step = 0;
                        else { s->nheld++; step = (uint8_t)(cb(c, 0xe5f) - 1); }
                        float pos[2] = { 0, 0 };
                        uint8_t back;
                        if ((int)step < (int)t->hold - 1) {
                            t->hold = (uint8_t)(t->hold - step);
                            back = t->hold;
                        } else {
                            t->hold = 0;
                            set_status(s, t, 1);
                            pos[0] = h->x;
                            pos[1] = h->y;
                            back = t->hold;
                        }
                        copy_out(c, s, d, i, 0, pos, t->peak, 0, 1, (uint8_t)(back - 1), 1);
                        sent = 1;
                    } else {
                        t->hold = 0;
                        set_status(s, t, 1);
                    }
                } else {
                    t->hold = 0;
                    set_status(s, t, 0);
                }
            }
            break;
        case 4: {
            uint8_t back = t->hold ? --t->hold : 0;
            const struct heat_hist *h = &t->h[(t->ring - back + 10) % 10];
            float pos[2] = { h->x, h->y };
            int ix = (int)(pos[0] + 0.5f), iy = (int)(pos[1] + 0.5f);
            unsigned thr = (unsigned)(int)(((1.0f - cf(c, 0xc)) + -0.6f) / 0.0022203543f + 0.5f);
            int touched = 0;
            for (int y = iy - 1; y <= iy + 1 && !touched; y++) {
                if (!(y >= 0 && y < c->rows)) continue;
                for (int x = ix - 1; x <= ix + 1; x++)
                    if (x >= 0 && x < c->cols && f->image[y][x] < thr) { touched = 1; break; }
            }
            if (!touched && !mapped_to_palm(c, s, t)) {
                copy_out(c, s, d, i, 0, pos, t->peak, 0, 1, back, 1);
                sent = 1;
            } else {
                t->hold = 0;
                set_status(s, t, 0);
            }
            break;
        }
        default:
            break;
        }
    counters: {
        uint32_t ty2 = t->h[t->ring].type;
        if (ty2 < 6) {
            unsigned bit = 1u << ty2;
            if ((bit & 0xd) == 0 && (bit & 0x22) == 0) { t->run_decided = 0; t->run_undecided++; }
            else { t->run_undecided = 0; t->run_decided++; }
        }
        t->reported = sent ? (uint16_t)(t->reported + 1) : 0;
    }
    }
}

// MarkCloseBlobs: records of whole blobs nearer than the close distance
// mark their tracks (ClassifyTracks lowers their likelihoods next frame).
static void mark_close(const struct heat_cfg *c, struct heat_tracks *s) {
    int n = s->nout, np = 0;
    float lim = c->close_d2[s->noisy];
    for (int a = 0; a < n; a++) {
        struct heat_report *ra = &s->out[a];
        if ((ra->type | 2) != 3 || a + 1 >= n) continue;
        for (int b = a + 1; b < n; b++) {
            struct heat_report *rb = &s->out[b];
            if (ra->label == rb->label || (rb->type | 2) != 3) continue;
            float dx = ra->x - rb->x, dy = ra->y - rb->y;
            if (!(dx * dx + dy * dy < lim)) continue;
            s->closepairs[np][0] = (uint8_t)a;
            s->closepairs[np][1] = (uint8_t)b;
            if (np + 1 == 0x2d0) { np = 0x2d0; break; }
            s->t[ra->track].close = 1;
            s->t[rb->track].close = 1;
            np++;
        }
    }
    // Each close pair joins one label, and whole blobs among them become
    // type 7 (blobs close to others).
    for (int p = 0; p < np; p++) {
        uint8_t a = s->closepairs[p][0], b = s->closepairs[p][1];
        uint8_t g = s->out[b].label;
        for (int k = 0; k < n; k++)
            if (s->out[k].label == g) s->out[k].label = s->out[a].label;
        if (s->out[a].type == 1) s->out[a].type = 7;
        if (s->out[b].type == 1) s->out[b].type = 7;
    }
}

// ---------------------------------------------------------------------------

int heat_tracks_frame(const struct heat_cfg *c, struct heat_tracks *s, struct heat_detector *d,
                      const struct heat_frame *f) {
    init_frame(c, s, f);
    memcpy(s->scan, f->scan, sizeof s->scan);
    s->scan_us = f->scan_us;
    if (f->have_nsr) memcpy(s->nsr, f->nsr, sizeof s->nsr);
    s->noise_level = (uint16_t)f->noise;
    d->palm = s->palm;
    s->prev_nblobs = s->cur_nblobs;
    int nd = heat_detect(c, d, f);
    s->cur_nblobs = nd < 0 ? 0 : (uint8_t)d->nblobs;
    s->nout = 0;
    s->failed = nd < 0;
    if (nd < 0) return 0;
    classification(c, s, d);

    // TouchDetectionTracking.
    unsigned e = elapsed(s->last_ts, f->ts);
    if (e > 5) {
        for (int i = 0; i < HEAT_TRACKS; i++) set_status(s, &s->t[i], 0);
        if (s->nlive || s->nactive) { s->nlive = 0; s->nactive = 0; }
    }
    s->last_ts = f->ts;
    match_blobs(c, s, d);
    tracks_without_match(c, s, d);
    classify_tracks(c, s, d);
    post_detection(c, s);

    // TouchDetectionPostProcess. (No pen: RunPenDrivenPalmRejection only
    // clears its state. Grip rejection is off in this tuning.)
    palm_by_proximity(c, s, d);
    {
        int n = s->nlive, skipped = 0;
        for (int i = 0; i < n + skipped && i < HEAT_TRACKS; i++) {
            struct heat_trk *t = &s->t[i];
            if (t->status == 0) { skipped++; continue; }
            if (t->frames > 1 && (t->h[t->ring].type | 4) == 5 && (prev_type(t) | 2) == 2) t->suspended = 1;
        }
    }
    suspension(c, s, d);
    {
        int n = s->nactive, skipped = 0;
        for (int i = 0; i < n + skipped && i < HEAT_TRACKS; i++) {
            struct heat_trk *t = &s->t[i];
            if (!(t->status - 1u < 2)) { skipped++; n = s->nactive; continue; }
            uint32_t ty = t->h[t->ring].type;
            if (t->matched && ty < 5 && ((1u << ty) & 0x15) && t->count < 0x1e)
                padded_centroid(c, s, d, &d->blob[t->blob], t, f);
            n = s->nactive;
        }
    }
    fill_out(c, s, d, f);
    mark_close(c, s);
    return s->nout;
}

void heat_dump_tracks(FILE *out, const struct heat_tracks *s) {
    for (int i = 0; i < HEAT_TRACKS; i++) {
        const struct heat_trk *t = &s->t[i];
        if (!t->status) continue;
        const struct heat_hist *h = &t->h[t->ring];
        fprintf(out, "P trk %d st=%u ty=%u x=%.6f y=%.6f r=%.6f v=%.6f,%.6f fr=%u n=%u blob=%u lab=%u hn=%u f=", i,
                t->status, h->type, (double)t->x, (double)t->y, (double)t->radius, (double)t->vx, (double)t->vy,
                t->frames, t->count, t->blob, t->label, h->count);
        const uint8_t *p = (const uint8_t *)t;
        for (int o = 0x40; o < 0x48; o++) fprintf(out, "%02x", p[o]);
        fprintf(out, " c=%u,%u,%u g=", t->run_undecided, t->run_decided, t->max_count);
        for (int o = 0x256; o < 0x25c; o++) fprintf(out, "%02x", p[o]);
        fprintf(out, " llr=%.6f,%.6f,%.6f,%.6f\n", (double)t->llr[0], (double)t->llr[1], (double)t->llr[2],
                (double)t->llr[3]);
    }
    fprintf(out, "P tl n=%d b2=%d\n", s->nout, s->nheld);
    for (int i = 0; i < s->nout; i++) {
        const struct heat_report *r = &s->out[i];
        fprintf(out, "P tli %d x=%.6f y=%.6f r=%.6f h=", i, (double)r->x, (double)r->y, (double)r->radius);
        const uint8_t *p = (const uint8_t *)r;
        for (int o = 8; o < 0x34; o++) fprintf(out, "%02x", o < 0x10 ? 0 : p[o]);
        fprintf(out, "\n");
    }
}
