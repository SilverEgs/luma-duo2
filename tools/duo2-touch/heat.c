// Surface Duo 2 touch: heat frames to contacts, following Microsoft's heat
// processor (libsurfacetouch_c1.so, Duo 2 "Zeta" build). Each step names the
// stock function it reproduces; docs/touch.md has the overview. The tuning is
// read at run time from the stock library's own PS_CFG_DATA_Zeta blob.

#include "heat.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Constants compiled into the stock code (not part of the tuning blob).

// TouchDetection::Init builds its byte -> magnitude table as
// 1 - (b * K + 0.6): 0xB4 (180), the idle level, is ~0; smaller is stronger.
#define HEAT_K 0.0022203543f
#define HEAT_BASE 0.6f
// ReportInformation::RunStretching: the stretch knee is r * A + B + param.
#define STRETCH_A 0.4247
#define STRETCH_B (-0.0316)

static float lut[256];

static void lut_init(void) {
    for (int b = 0; b < 256; b++) lut[b] = 1.0f - ((float)b * HEAT_K + HEAT_BASE);
}

// ---------------------------------------------------------------------------
// The tuning blob.

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static float rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }

// The bytes of symbol NAME in an ELF64 little-endian file, by its section.
static const uint8_t *elf_symbol(const uint8_t *d, size_t n, const char *name, size_t *size) {
    if (n < 64 || memcmp(d, "\177ELF", 4) || d[4] != 2 || d[5] != 1) return NULL;
    uint64_t shoff = (uint64_t)rd32(d + 0x28) | (uint64_t)rd32(d + 0x2c) << 32;
    unsigned shentsize = rd16(d + 0x3a), shnum = rd16(d + 0x3c);
    if (shentsize < 64 || shoff + (uint64_t)shnum * shentsize > n) return NULL;
#define SH(i, off) (d + shoff + (uint64_t)(i) * shentsize + (off))
#define U64(p) ((uint64_t)rd32(p) | (uint64_t)rd32((p) + 4) << 32)
    for (unsigned s = 0; s < shnum; s++) {
        uint32_t type = rd32(SH(s, 4));
        if (type != 2 && type != 11) continue;  // SHT_SYMTAB, SHT_DYNSYM
        uint64_t off = U64(SH(s, 0x18)), sz = U64(SH(s, 0x20));
        uint32_t link = rd32(SH(s, 0x28));
        if (link >= shnum || off + sz > n) continue;
        uint64_t stroff = U64(SH(link, 0x18)), strsz = U64(SH(link, 0x20));
        if (stroff + strsz > n) continue;
        for (uint64_t e = off; e + 24 <= off + sz; e += 24) {
            uint32_t nm = rd32(d + e);
            if (nm >= strsz || strncmp((const char *)d + stroff + nm, name, strsz - nm)) continue;
            unsigned shndx = rd16(d + e + 6);
            uint64_t value = U64(d + e + 8), ssize = U64(d + e + 16);
            if (shndx == 0 || shndx >= shnum) continue;
            uint64_t addr = U64(SH(shndx, 0x10)), soff = U64(SH(shndx, 0x18));
            uint64_t fo = value - addr + soff;
            if (fo + ssize > n) return NULL;
            *size = ssize;
            return d + fo;
        }
    }
#undef SH
#undef U64
    return NULL;
}

#define CFG_SIZE_MIN 0x2000
// Offsets in PS_CFG_DATA_Zeta (SurfaceHeatProcessorLogic's ConfigurationDataBase
// hands these to TouchProcessingManager::Init).
#define CFG_REGION0 0x3c            // the panel region record (TouchProcessingMngrInitStruct source)
#define CFG_POSTPROC 0x870          // TouchPostProcessingCfg
#define CFG_STRETCH (CFG_POSTPROC + 0x22c)
#define CFG_DETECT 0xef4            // TouchDetectionCfg

int heat_cfg_load(struct heat_cfg *c, const char *library, char *err, size_t errlen) {
    lut_init();
    memset(c, 0, sizeof *c);
    int fd = open(library, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { snprintf(err, errlen, "%s: %s", library, strerror(errno)); return -1; }
    struct stat st;
    if (fstat(fd, &st) < 0) { snprintf(err, errlen, "%s: %s", library, strerror(errno)); close(fd); return -1; }
    const uint8_t *d = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (d == MAP_FAILED) { snprintf(err, errlen, "%s: %s", library, strerror(errno)); return -1; }
    size_t n = 0;
    const uint8_t *b = elf_symbol(d, (size_t)st.st_size, "PS_CFG_DATA_Zeta", &n);
    int rc = -1;
    if (!b) { snprintf(err, errlen, "%s: no PS_CFG_DATA_Zeta", library); goto out; }
    if (n < CFG_SIZE_MIN || memcmp(b, "PSDB", 4) || rd16(b + 6) != 0x0c1d) {
        snprintf(err, errlen, "%s: PS_CFG_DATA_Zeta is not the Duo 2 (0C1D) tuning", library);
        goto out;
    }

    const uint8_t *r = b + CFG_REGION0;
    c->cols = rd16(r);                    // 21: along x, the panel's short axis
    c->rows = rd16(r + 2);                // 30: along y
    c->active_mm[0] = rd32(r + 0x18) * 0.01f;
    c->active_mm[1] = rd32(r + 0x1c) * 0.01f;
    c->size_mm[0] = rd32(r + 0x20) * 0.01f;
    c->size_mm[1] = rd32(r + 0x24) * 0.01f;
    c->logical_max[0] = (int)rd32(r + 0x2c);
    c->logical_max[1] = (int)rd32(r + 0x30);
    if (c->cols < 2 || c->cols > HEAT_MAX_COLS || c->rows < 2 || c->rows > HEAT_MAX_ROWS ||
        c->active_mm[0] <= 0 || c->size_mm[0] < c->active_mm[0] || c->size_mm[1] < c->active_mm[1]) {
        snprintf(err, errlen, "PS_CFG_DATA_Zeta: unexpected panel geometry");
        goto out;
    }
    // ProjectTuningMngr::InitCommonTrackingCfg (no edge antennas here) and
    // DetectedArray::Init's centring offset.
    for (int a = 0; a < 2; a++) {
        int count = a == 0 ? c->cols : c->rows;
        c->pitch_mm[a] = c->active_mm[a] / (float)(count - 1);
        c->offset_mm[a] = (c->size_mm[a] - c->active_mm[a]) * 0.5f;
    }

    const uint8_t *t = b + CFG_DETECT;
    c->thr_min = rdf(t);
    c->thr_max = rdf(t + 4);
    c->strong = rdf(t + 8);
    c->peak_valid = rdf(t + 0xc);
    c->thr_count = (int)rd32(t + 0x10);
    if (c->thr_count < 1 || c->thr_count > HEAT_THR_TABLE) { snprintf(err, errlen, "PS_CFG_DATA_Zeta: bad threshold table"); goto out; }
    for (int i = 0; i < c->thr_count; i++) {
        c->thr_table[i] = rdf(t + 0x14 + 8 * i);
        c->noise_limit[i] = rd32(t + 0x18 + 8 * i);
    }
    c->radius_factor = rdf(t + 0xeac + 4);

    const uint8_t *s = b + CFG_STRETCH;
    c->stretch_on = s[0x4c] == 1;
    for (int a = 0; a < 2; a++)
        for (int k = 0; k < 4; k++) c->stretch[a][k] = rdf(s + 0xc + 16 * a + 4 * k);
    rc = 0;
out:
    munmap((void *)d, (size_t)st.st_size);
    return rc;
}

// ---------------------------------------------------------------------------
// Heat reports (SurfaceHeatProcessorLogic::ParseFullFrame, Parser::RunParser).
//
//   [0] report id   [1..2] sequence   [3..6] payload length   [7..8] 0
//   from 10: sections [u32 size][u16 type]...
//     0x0100 heat image: [6] format (nonzero: 8-bit), [8] u32 start,
//            [12] u32 count, [16] count bytes, x-major (x * rows + y)
//     0xFF00 [7..]: sub-sections [u8 type][u8 flags][u16 size] payload;
//            type 4 holds the frequency noise levels

int heat_parse_report(const struct heat_cfg *c, const uint8_t *r, size_t n, struct heat_frame *f) {
    memset(f, 0, sizeof *f);
    if (n < 16) return -1;
    f->seq = rd16(r + 1);
    size_t end = (size_t)rd32(r + 3) + 3;
    if (end > n) end = n;
    size_t o = 10;
    for (int k = 0; k < 4 && o + 6 <= end; k++) {
        uint32_t size = rd32(r + o);
        uint16_t type = rd16(r + o + 4);
        if (size < 6 || o + size > end) break;
        const uint8_t *sec = r + o;
        if (type == 0x0100 && size >= 16 && sec[6]) {
            uint32_t start = rd32(sec + 8), count = rd32(sec + 12);
            if (start == 0 && count == (uint32_t)(c->cols * c->rows) && 16 + count <= size) {
                for (int x = 0; x < c->cols; x++)
                    for (int y = 0; y < c->rows; y++)
                        f->image[y * c->cols + x] = sec[16 + x * c->rows + y];
                f->have_image = 1;
            }
        } else if (type == 0xFF00) {
            size_t p = 7;
            while (p + 4 <= size) {
                uint8_t st = sec[p];
                uint16_t ss = rd16(sec + p + 2);
                if ((st == 0 && ss == 0) || p + 4 + ss > size) break;
                if (st == 4)
                    for (size_t q = 2; q + 2 <= ss; q += 2) {
                        unsigned v = rd16(sec + p + 4 + q);
                        if (v > f->noise) f->noise = v;
                    }
                p += 4 + ss;
            }
        }
        o += size;
    }
    return f->have_image ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Detection (TouchDetection::ProcessTouch).

struct blobacc {
    float sw, swx, swy, sm;
    int minx, miny, maxx, maxy, count;
    uint8_t peak;                 // smallest byte: the strongest junction
    int parent;
};

static int find(struct blobacc *a, int l) {
    while (a[l].parent != l) l = a[l].parent = a[a[l].parent].parent;
    return l;
}

static float mag_at(const struct heat_cfg *c, const struct heat_frame *f, int x, int y) {
    if (x < 0 || y < 0 || x >= c->cols || y >= c->rows) return 0.0f;
    return lut[f->image[y * c->cols + x]];
}

// SetBlobOnBorderFlags + SetBlobRadius: the blob's widest run of junctions
// (sum / max of a line through it) across one axis, in mm.
static float blob_radius(const struct heat_cfg *c, const struct heat_frame *f, const struct blobacc *b) {
    int xb = b->minx == 0 || b->maxx == c->cols - 1;
    int yb = b->miny == 0 || b->maxy == c->rows - 1;
    if (xb && yb) return 4.5f;
    float best = 0.0f;
    if (!xb) {
        // Along x, for each line of the blob's y span.
        for (int y = b->miny; y <= b->maxy; y++) {
            float sum = 0.0f, mx = 0.0f;
            for (int x = b->minx; x <= b->maxx; x++) {
                float m = mag_at(c, f, x, y);
                sum += m;
                if (m > mx) mx = m;
            }
            if (mx > 0.0f && sum / mx > best) best = sum / mx;
        }
        return c->pitch_mm[0] * 0.5f * best * c->radius_factor;
    }
    // On a short-side border: along y instead.
    for (int x = b->minx; x <= b->maxx; x++) {
        float sum = 0.0f, mx = 0.0f;
        for (int y = b->miny; y <= b->maxy; y++) {
            float m = mag_at(c, f, x, y);
            sum += m;
            if (m > mx) mx = m;
        }
        if (mx > 0.0f && sum / mx > best) best = sum / mx;
    }
    return c->pitch_mm[1] * 0.5f * best * c->radius_factor;
}

// ReportInformation::RunStretching: near an edge, map [knee, zone] onto
// [0, zone] so a finger centred off the antennas can still reach the edge.
static float stretch(const struct heat_cfg *c, float p, float r, int a) {
    const float *s = c->stretch[a];
    float z = s[0] + r;
    if (p < z) {
        float k = (float)((double)r * STRETCH_A + STRETCH_B + (double)s[1]);
        return (z * p - z * k) / (z - k);
    }
    float q = c->size_mm[a] - p;
    z = r + s[2];
    if (q < z) {
        float k = (float)((double)r * STRETCH_A + STRETCH_B + (double)s[3]);
        return c->size_mm[a] - (q * z - z * k) / (z - k);
    }
    return p;
}

int heat_detect(const struct heat_cfg *c, const struct heat_frame *f, struct heat_blob *out, int max) {
    // CclLogic: the scan threshold for this frame's noise level.
    float thr = c->thr_table[c->thr_count - 1];
    for (int i = 0; i < c->thr_count; i++)
        if (f->noise < c->noise_limit[i]) { thr = c->thr_table[i]; break; }
    if (thr < c->thr_min) thr = c->thr_min;
    if (thr > c->thr_max) thr = c->thr_max;
    int cand = (int)(((1.0f - thr) - HEAT_BASE) / HEAT_K + 0.5f);

    // RunCcl: one raster pass in y-major order with 8-connectivity, joining
    // only through strong junctions, then label equivalences merged
    // (MergeSameBlobsData).
    static int label[HEAT_MAX_ROWS * HEAT_MAX_COLS];
    struct blobacc acc[HEAT_MAX_ROWS * HEAT_MAX_COLS / 2 + 2];
    int nl = 0, cap = (int)(sizeof acc / sizeof acc[0]) - 1;
    for (int y = 0; y < c->rows; y++) {
        for (int x = 0; x < c->cols; x++) {
            int i = y * c->cols + x;
            label[i] = 0;
            uint8_t byte = f->image[i];
            if (byte > cand) continue;
            float own = lut[byte];
            static const int prev[4][2] = { { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 } };
            int l = 0;
            for (int k = 0; k < 4; k++) {
                int nx = x + prev[k][0], ny = y + prev[k][1];
                if (nx < 0 || ny < 0 || nx >= c->cols) continue;
                int nlab = label[ny * c->cols + nx];
                if (!nlab) continue;
                if (own < c->strong && mag_at(c, f, nx, ny) < c->strong) continue;
                if (!l) l = nlab;
                else {
                    int ra = find(acc, l), rb = find(acc, nlab);
                    if (ra != rb) acc[ra > rb ? ra : rb].parent = ra < rb ? ra : rb;
                }
            }
            if (!l) {
                static const int next[4][2] = { { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 } };
                int seed = own >= c->strong;
                for (int k = 0; k < 4 && !seed; k++) seed = mag_at(c, f, x + next[k][0], y + next[k][1]) >= c->strong;
                if (!seed || nl >= cap) continue;
                l = ++nl;
                memset(&acc[l], 0, sizeof acc[l]);
                acc[l].parent = l;
            }
            label[i] = l;
        }
    }
    for (int l = 1; l <= nl; l++) {
        acc[l].minx = acc[l].miny = 1 << 20;
        acc[l].maxx = acc[l].maxy = -1;
        acc[l].peak = 0xff;
        acc[l].sw = acc[l].swx = acc[l].swy = acc[l].sm = 0.0f;
        acc[l].count = 0;
    }
    // AddNewJunction: weights are the magnitude above the threshold.
    for (int y = 0; y < c->rows; y++)
        for (int x = 0; x < c->cols; x++) {
            int i = y * c->cols + x;
            if (!label[i]) continue;
            struct blobacc *b = &acc[find(acc, label[i])];
            float m = lut[f->image[i]];
            float w = m - (thr - HEAT_K);
            if (w < 0.0001f) w = 0.0001f;
            b->sw += w; b->swx += w * (float)x; b->swy += w * (float)y; b->sm += m;
            if (x < b->minx) b->minx = x;
            if (x > b->maxx) b->maxx = x;
            if (y < b->miny) b->miny = y;
            if (y > b->maxy) b->maxy = y;
            if (f->image[i] < b->peak) b->peak = f->image[i];
            b->count++;
        }

    int nb = 0;
    for (int l = 1; l <= nl && nb < max; l++) {
        struct blobacc *b = &acc[l];
        if (b->parent != l || !b->count) continue;
        // CalcNewParamsForMergedBlobs: small blobs need a real peak.
        if (!(b->count > 2 || lut[b->peak] > c->peak_valid)) continue;
        // FeatureExtraction: a blob whose peak is under 0.04 is dropped; one of
        // TouchDetectionCfg+0xe80 (50) junctions or more is a palm.
        if (lut[b->peak] < 0.04f || b->count >= 50) continue;
        struct heat_blob *o = &out[nb++];
        o->ax = b->swx / b->sw;
        o->ay = b->swy / b->sw;
        o->radius = blob_radius(c, f, b);
        o->peak = lut[b->peak];
        o->count = b->count;
        // DetectedArray::ConvertPositionFromAntennaToMmAndFlip, FixOffsetRotation.
        o->x = o->ax * c->pitch_mm[0] + c->offset_mm[0];
        o->y = o->ay * c->pitch_mm[1] + c->offset_mm[1];
        if (c->stretch_on) {
            o->x = stretch(c, o->x, o->radius, 0);
            o->y = stretch(c, o->y, o->radius, 1);
        }
    }
    return nb;
}

// ---------------------------------------------------------------------------
// Tracking: each blob joins the track whose predicted position (last
// position + last velocity, as TouchDetection::MatchBlobsToTracks predicts)
// is nearest, closest pairs first; unmatched blobs open tracks.

#define TRACK_GATE_MM 25.0f

void heat_track_reset(struct heat_tracker *tr) {
    for (int i = 0; i < HEAT_MAX_TRACKS; i++) tr->t[i].active = 0;
}

static int clampi(float v, int max) {
    int i = (int)lrintf(v);
    return i < 0 ? 0 : i > max ? max : i;
}

int heat_track(const struct heat_cfg *c, struct heat_tracker *tr, const struct heat_blob *b, int nb,
               struct heat_contact *out, int max) {
    int blob_track[HEAT_MAX_BLOBS], track_used[HEAT_MAX_TRACKS] = { 0 };
    if (nb > HEAT_MAX_BLOBS) nb = HEAT_MAX_BLOBS;
    for (int i = 0; i < nb; i++) blob_track[i] = -1;
    for (;;) {
        float best = TRACK_GATE_MM * TRACK_GATE_MM;
        int bi = -1, bt = -1;
        for (int i = 0; i < nb; i++) {
            if (blob_track[i] >= 0) continue;
            for (int t = 0; t < HEAT_MAX_TRACKS; t++) {
                const struct heat_track *k = &tr->t[t];
                if (!k->active || track_used[t]) continue;
                float dx = b[i].x - (k->x + k->vx), dy = b[i].y - (k->y + k->vy);
                float d2 = dx * dx + dy * dy;
                if (d2 < best) { best = d2; bi = i; bt = t; }
            }
        }
        if (bi < 0) break;
        blob_track[bi] = bt;
        track_used[bt] = 1;
    }
    for (int t = 0; t < HEAT_MAX_TRACKS; t++)
        if (tr->t[t].active && !track_used[t]) tr->t[t].active = 0;
    for (int i = 0; i < nb; i++) {
        if (blob_track[i] >= 0) {
            struct heat_track *k = &tr->t[blob_track[i]];
            k->vx = b[i].x - k->x; k->vy = b[i].y - k->y;
            k->x = b[i].x; k->y = b[i].y; k->radius = b[i].radius; k->frames++;
            continue;
        }
        for (int t = 0; t < HEAT_MAX_TRACKS; t++) {
            struct heat_track *k = &tr->t[t];
            if (k->active) continue;
            k->active = 1; k->id = tr->next_id; tr->next_id = (tr->next_id + 1) & 0xffff;
            k->x = b[i].x; k->y = b[i].y; k->vx = k->vy = 0.0f; k->radius = b[i].radius; k->frames = 1;
            blob_track[i] = t;
            break;
        }
    }
    // TracksArray::FillOsReport: millimetres to the panel's logical units.
    float sx = (float)c->logical_max[0] / c->size_mm[0], sy = (float)c->logical_max[1] / c->size_mm[1];
    int n = 0;
    for (int t = 0; t < HEAT_MAX_TRACKS && n < max; t++) {
        const struct heat_track *k = &tr->t[t];
        if (!k->active) continue;
        struct heat_contact *o = &out[n++];
        o->id = k->id;
        o->x = clampi(k->x * sx, c->logical_max[0]);
        o->y = clampi(k->y * sy, c->logical_max[1]);
        o->major = o->minor = clampi(2.0f * k->radius * sx, c->logical_max[1]);
    }
    return n;
}
