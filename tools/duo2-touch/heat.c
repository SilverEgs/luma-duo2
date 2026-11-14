// Surface Duo 2 touch: heat frames to contacts, following Microsoft's heat
// processor (libsurfacetouch_c1.so, Duo 2 "Zeta" build) operation for
// operation. Each step names the stock function it reproduces; docs/touch.md
// has the overview. The tuning is read at run time from the stock library's
// own PS_CFG_DATA_Zeta blob.
//
// The stock code does its arithmetic in single precision, without fused
// multiply-adds, in the order kept here; build with -ffp-contract=off so the
// results stay bit-identical (tools/duo2-touch/stock-ref checks them).

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
// 1 - (b * K + 0.6), floored at 0: 0xB4 (180), the idle level, is ~0;
// smaller is stronger.
#define HEAT_K 0.0022203543f
#define HEAT_BASE 0.6f
// FeatureExtraction: a blob whose strongest junction is below this is noise.
#define PEAK_MIN 0.04f
// The radius given to blobs whose shape says nothing (corners, palms).
#define RADIUS_DEFAULT 4.5f
// ReportInformation::RunStretching: the stretch knee is r * A + B + knee
// (double precision).
#define STRETCH_A 0.4247
#define STRETCH_B (-0.0316)
// ProjectTuningMngr::GetRegionIdx for the Duo 2's topology (1): the region
// whose stretching configuration DetectedArray uses.
#define STRETCH_REGION 1

float heat_lut[256];
#define lut heat_lut

static void lut_init(void) {
    for (int b = 0; b < 256; b++) {
        float m = 1.0f - ((float)b * HEAT_K + HEAT_BASE);
        lut[b] = m < 0.0f ? 0.0f : m;
    }
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

// Offsets in PS_CFG_DATA_Zeta. The stock objects point straight into the
// blob (TouchDetection +0x16608 is the blob + 0xef4, and so on).
#define CFG_SIZE_MIN 0x2000
#define CFG_ALIGN_LIMIT 0x104       // ConfigurationDataBase: alignment clamp (mm)
#define CFG_ALIGN_ENABLE 0x10b      //   and whether alignment applies
#define CFG_REGION0 0x3c            // the panel region record
#define CFG_POSTPROC 0x870          // TouchPostProcessingCfg
#define CFG_STRETCH (CFG_POSTPROC + 0x22c)
#define CFG_BORDER 0xc94            // TouchDetection +0x16618
#define CFG_STATIC 0xcd0            // StaticConfig
#define CFG_DETECT 0xef4            // TouchDetectionCfg (TouchDetection +0x16608)
#define CFG_RADIUS 0x1da0           // TouchDetection +0x19600

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

    // The region record, in 0.01 mm, short axis first.
    const uint8_t *r = b + CFG_REGION0;
    c->rows = rd16(r);                    // 21: y, the short axis
    c->cols = rd16(r + 2);                // 30: x, the long axis
    c->active_mm[1] = (float)rd32(r + 0x18) * 0.01f;
    c->active_mm[0] = (float)rd32(r + 0x1c) * 0.01f;
    c->size_mm[1] = (float)rd32(r + 0x20) * 0.01f;
    c->size_mm[0] = (float)rd32(r + 0x24) * 0.01f;
    c->logical_max[1] = (int)rd32(r + 0x2c);
    c->logical_max[0] = (int)rd32(r + 0x30);
    if (c->cols < 2 || c->cols > HEAT_MAX_X || c->rows < 2 || c->rows > HEAT_MAX_Y || c->active_mm[0] <= 0 ||
        c->active_mm[1] <= 0 || c->size_mm[0] < c->active_mm[0] || c->size_mm[1] < c->active_mm[1]) {
        snprintf(err, errlen, "PS_CFG_DATA_Zeta: unexpected panel geometry");
        goto out;
    }
    // ProjectTuningMngr::InitCommonTrackingCfg (no edge antennas here) and
    // DetectedArray::Init's centring offset.
    for (int a = 0; a < 2; a++) {
        int count = a == 0 ? c->cols : c->rows;
        c->pitch_mm[a] = c->active_mm[a] / (float)(count - 1);
        c->offset_mm[a] = (c->active_mm[a] - c->size_mm[a]) * 0.5f;
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
    c->peak_eps = rdf(t + 0xcc);
    c->saddle_diag = rdf(t + 0xd0);
    c->saddle_2 = rdf(t + 0xd4);
    c->saddle_knight = rdf(t + 0xd8);
    c->saddle_long_d2 = (int)rd32(t + 0xdc);
    c->saddle_long = rdf(t + 0xe0);
    c->max_junctions = rd16(t + 0xb4);
    c->topo_floor = rdf(t + 0xbc);
    for (int i = 0; i < 3; i++) c->topo_level[i] = rdf(t + 0xe8 + 4 * i);
    c->topo_base = rdf(t + 0xf4);
    c->halo_fraction = rdf(t + 0xf8);
    c->halo_ring[0] = rdf(t + 0xfc);
    c->halo_ring[1] = rdf(t + 0x100);
    c->big_blob = rd16(t + 0xe80);
    c->big_blob_palm = rd16(t + 0xe82);
    for (int i = 0; i < 10; i++) c->palm_features[i] = rdf(t + 0x49c + 4 * i);
    memcpy(c->td, t, sizeof c->td);
    c->match_d2 = rdf(b + CFG_POSTPROC);
    for (int i = 0; i < 3; i++) c->palm_radius[i] = rdf(b + CFG_POSTPROC + 0x17c + 4 * i);
    c->radius_iir = rdf(b + CFG_RADIUS);
    c->close_d2[0] = rdf(b + CFG_BORDER + 0xc);
    c->close_d2[1] = rdf(b + CFG_BORDER + 0x20);
    c->base_byte[0] = rd16(b + CFG_BORDER + 0xc + 8);
    c->base_byte[1] = rd16(b + CFG_BORDER + 0x20 + 8);
    c->pad_fraction = (float)rd16(b + CFG_BORDER + 8) * 0.01f;
    // TouchDetection::InitScanWindowLUT: each row takes the first scan
    // window that lists it. Layout by StaticConfig's type: 0 and 2 list rows
    // per window and group, the others per window only.
    {
        const uint8_t *sc = b + CFG_STATIC;
        memset(c->scan_row, 0xff, sizeof c->scan_row);
        if ((sc[0] | 2) == 2) {
            uint32_t nw = rd32(sc + 4), ng = rd32(sc + 8), ne = rd32(sc + 0xc);
            for (uint32_t w = 0; w < nw && w < 8; w++)
                for (uint32_t g = 0; g < ng && g < 8; g++)
                    for (uint32_t k = 0; k < ne && k < 4; k++) {
                        uint8_t row = sc[0x10 + w * 0x20 + g * 4 + k];
                        if (row != 0xff && row < sizeof c->scan_row && w < c->scan_row[row]) c->scan_row[row] = (uint8_t)w;
                    }
        } else {
            uint32_t nw = rd32(sc + 0x138), ne = rd32(sc + 0x13c);
            for (uint32_t w = 0; w < nw && w < 16; w++)
                for (uint32_t k = 0; k < ne && k < 16; k++) {
                    uint8_t row = sc[0x140 + w * 0x10 + k];
                    if (row != 0xff && row < sizeof c->scan_row && w < c->scan_row[row]) c->scan_row[row] = (uint8_t)w;
                }
        }
    }
    // RunCclForSpecificJunction lets a junction below the strong level join
    // or seed a blob only beside a strong one. Every junction the scan takes
    // is at least the threshold, which is never below the strong level in
    // this tuning, so that case cannot arise and is not reproduced.
    if (c->thr_min < c->strong) { snprintf(err, errlen, "PS_CFG_DATA_Zeta: threshold below the strong level"); goto out; }
    c->border_margin = rdf(b + CFG_BORDER + 4);
    c->radius_factor = rdf(b + CFG_RADIUS + 4);

    const uint8_t *s = b + CFG_STRETCH;
    c->stretch_on = s[0x4c] == 1;
    for (int a = 0; a < 2; a++)
        for (int k = 0; k < 4; k++) c->stretch[a][k] = rdf(s + 0xc + STRETCH_REGION * 0x20 + 16 * a + 4 * k);

    c->align_limit = rdf(b + CFG_ALIGN_LIMIT);
    c->align_enabled = b[CFG_ALIGN_ENABLE] != 0;
    rc = 0;
out:
    munmap((void *)d, (size_t)st.st_size);
    return rc;
}

// PanelAlignmentReaderPersistentStorageFullUefiStructure::ReadPanelAlignmentOffsets:
// the 24-byte UEFI record holds two offsets in micrometres as floats, at 16
// for the reader's own region (1 here: the left or top screen's index) and
// at 20 for the other. SetCurrentPanelAlignmentOffset keeps one within
// [-limit, 2 * limit], clamped to the limit. DetectedArray adds the offset of
// region GetRegionIdx(topology 1) = 1, which is the one at 16.
int heat_cfg_load_alignment(struct heat_cfg *c, const char *file, char *err, size_t errlen) {
    c->align_mm = 0.0f;
    if (!c->align_enabled) return 0;
    uint8_t rec[24];
    FILE *f = fopen(file, "rb");
    if (!f) { snprintf(err, errlen, "%s: %s", file, strerror(errno)); return -1; }
    size_t got = fread(rec, 1, sizeof rec, f);
    fclose(f);
    if (got != sizeof rec) { snprintf(err, errlen, "%s: not a 24-byte alignment record", file); return -1; }
    float v = rdf(rec + 16) * 0.001f, lim = c->align_limit;
    if (!(-lim <= v && v <= lim + lim)) { snprintf(err, errlen, "%s: offset %.3f mm out of range", file, (double)v); return -1; }
    c->align_mm = v <= lim ? v : lim;
    return 0;
}

// ---------------------------------------------------------------------------
// Heat reports (SurfaceHeatProcessorLogic::ParseFullFrame, Parser::RunParser).
//
//   [0] report id   [1..2] sequence   [3..6] payload length   [7..8] 0
//   from 10: sections [u32 size][u16 type]...
//     0x0100 heat image: [6] format (nonzero: 8-bit), [8] u32 start,
//            [12] u32 count, [16] count bytes, row by row ([y][x])
//     0xFF00 [7..]: sub-sections [u8 type][u8 flags][u16 size] payload;
//            type 0 is the cycle header, with the scan counter at 2 (the
//            frame time TouchDetection keeps, DataProtectedGeneral +4);
//            type 4 holds the frequency noise levels, type 50 the
//            scan windows' values ([4] count, from [5])

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
                for (int y = 0; y < c->rows; y++) memcpy(f->image[y], sec + 16 + y * c->cols, (size_t)c->cols);
                f->have_image = 1;
            }
        } else if (type == 0xFF00) {
            size_t p = 7;
            while (p + 4 <= size) {
                uint8_t st = sec[p];
                uint16_t ss = rd16(sec + p + 2);
                if ((st == 0 && ss == 0) || p + 4 + ss > size) break;
                if (st == 0 && ss >= 4) f->ts = rd16(sec + p + 4 + 2);
                if (st == 50 && ss >= 5 && sec[p + 4 + 4] < 0xb)
                    memcpy(f->scan, sec + p + 4 + 5,
                           sec[p + 4 + 4] <= ss - 5 ? sec[p + 4 + 4] : (size_t)(ss - 5));
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
// Blobs (TouchDetection::CclLogic).
//
// The stock code runs one connected-component pass (RunCcl,
// RunCclForSpecificJunction, CreateNewBlob, AddNewJunction,
// MergeSameBlobsData) over a window of the image described by a
// CclAlgorithmInput: the whole image for blobs, and a blob's bounding box at
// three levels for its topography.

struct ccl {
    const struct heat_frame *f;
    float thr;                    // +0x28: junctions at or above this
    int x0, y0, x1, y1;           // +0x34..+0x3a: the window
    int conn8;                    // +0x4c: 8-connected (else 4)
    int only_label;               // +0x4a: only junctions of this blob (0: all)
    const struct heat_detector *main;  // the blob labels only_label refers to
    uint8_t *labels;              // +0x08: labels for the window
    int stride;                   // +0x46
    struct heat_blob *rec;        // +0x20: records
    int maxrec;                   // +0x4b
    int nrec;                     // +0x3c, plus one
    uint8_t *eq;                  // +0x18: equivalences
    uint8_t (*pairs)[2];          // +0x10
    int maxpairs, npairs;         // +0x3e, +0x40
    int nvalid;                   // +0x49: blobs that pass
    int maxcount;                 // +0x44: junctions in the largest of them
};

#define LABEL(k, x, y) (k)->labels[((x) - (k)->x0) + ((y) - (k)->y0) * (k)->stride]

// CreateNewBlob: a new record for a junction no earlier neighbour claims.
static int create_blob(struct ccl *k, int x, int y, uint8_t byte) {
    if (k->nrec >= k->maxrec) return -1;
    int i = k->nrec++;
    struct heat_blob *b = &k->rec[i];
    float m = lut[byte];
    float w = m - (k->thr + -HEAT_K);   // not floored here, unlike AddNewJunction
    LABEL(k, x, y) = (uint8_t)(i + 1);
    b->peak = byte;
    b->label = b->src_label = (uint8_t)(i + 1);
    b->minx = b->maxx = (uint16_t)x;
    b->miny = b->maxy = (uint16_t)y;
    b->status = 4;
    b->big = 0;
    b->near_border = 0;
    b->index = (uint16_t)i;
    b->smx = m * (float)x;
    b->smy = m * (float)y;
    b->sm = m;
    b->sm2 = m * m;
    b->count = 1;
    b->swx = w * (float)x;
    b->swy = w * (float)y;
    b->sw = w;
    b->sw2 = w * w;
    b->max_count = 1;
    return 0;
}

// AddNewJunction: a junction joins blob LABEL (its own record, before the
// equivalences are merged).
static void add_junction(struct ccl *k, int label, int x, int y, uint8_t byte) {
    struct heat_blob *b = &k->rec[label - 1];
    float m = lut[byte];
    float w = m - (k->thr + -HEAT_K);
    if (w <= 0.0001f) w = 0.0001f;
    if (x < b->minx) b->minx = (uint16_t)x;
    if (y < b->miny) b->miny = (uint16_t)y;
    if (x > b->maxx) b->maxx = (uint16_t)x;
    if (y > b->maxy) b->maxy = (uint16_t)y;
    b->swx = b->swx + w * (float)x;
    b->swy = b->swy + w * (float)y;
    b->sw = b->sw + w;
    b->sw2 = b->sw2 + w * w;
    b->smx = m * (float)x + b->smx;
    b->smy = m * (float)y + b->smy;
    b->sm = m + b->sm;
    b->sm2 = m * m + b->sm2;
    b->count++;
    if (b->max_count < b->count) b->max_count = b->count;
    if (byte < b->peak) b->peak = byte;
}

// RunCcl + RunCclForSpecificJunction: one raster pass, y outer. A junction
// at or above the threshold joins the first labelled earlier neighbour (left,
// up-left, up, up-right, or only left and up when 4-connected; fewer on the
// window's edges); further labels it touches are recorded as equivalent.
// With none, it starts a blob.
static int run_ccl(struct ccl *k) {
    const struct heat_frame *f = k->f;
    int cand = (int)(((1.0f - k->thr) + -HEAT_BASE) / HEAT_K + 0.5f);
    for (int y = k->y0; y <= k->y1; y++) {
        for (int x = k->x0; x <= k->x1; x++) {
            LABEL(k, x, y) = 0;
            if (k->only_label && k->main->eq[k->main->label[y][x]] != k->only_label) continue;
            uint8_t byte = f->image[y][x];
            if ((int)byte > cand) continue;
            int nx[4], ny[4], nn;
            if (x == k->x0) {
                if (y == k->y0) nn = 0;
                else { nx[0] = 0; ny[0] = -1; nx[1] = 1; ny[1] = -1; nn = 2; }
            } else {
                nx[0] = -1; ny[0] = 0;
                if (y == k->y0) nn = 1;
                else {
                    nx[1] = -1; ny[1] = -1; nx[2] = 0; ny[2] = -1; nx[3] = 1; ny[3] = -1;
                    nn = x == k->x1 ? 3 : 4;
                }
            }
            uint8_t seen[4] = { 0, 0, 0, 0 };
            int joined = 0;
            for (int j = 0; j < nn; j++) {
                if (!k->conn8 && ny[j] == -1 && nx[j] != 0) continue;   // diagonals
                int l = LABEL(k, x + nx[j], y + ny[j]);
                if (!l) continue;
                if (!joined) {
                    add_junction(k, l, x, y, byte);
                    LABEL(k, x, y) = (uint8_t)l;
                    joined = l;
                    continue;
                }
                if (l == joined) continue;
                int dup = 0;
                for (int q = 0; q < nn; q++) if (seen[q] == l) dup = 1;
                if (dup) continue;
                seen[j] = (uint8_t)l;
                k->pairs[k->npairs][0] = (uint8_t)(joined < l ? joined : l);
                k->pairs[k->npairs][1] = (uint8_t)(joined < l ? l : joined);
                if (++k->npairs >= k->maxpairs) { k->npairs = k->maxpairs - 1; return -1; }
            }
            if (!joined && create_blob(k, x, y, byte) < 0) return -1;
        }
    }
    return 0;
}

// MergeSameBlobsData: resolve the equivalences to the smallest label, then
// fold each record into the one its label resolves to
// (CalcNewParamsForMergedBlobs), and decide which records are blobs.
static int merge_blobs(const struct heat_cfg *c, struct ccl *k) {
    uint8_t *eq = k->eq;
    int last = k->nrec - 1;
    for (int p = 0; p < k->npairs; p++) {
        uint8_t a = k->pairs[p][0], b = k->pairs[p][1];
        uint8_t ea = eq[a] ? eq[a] : 0xff, eb = eq[b] ? eq[b] : 0xff;
        uint8_t m = ea <= a ? ea : a;
        uint8_t mb = eb <= b ? eb : b;
        if (mb <= m) m = mb;
        eq[a] = m;
        eq[b] = m;
        if ((ea & eb) == 0xff) continue;
        if (m < ea && m < eb) {
            for (int i = 0; i <= last; i++) if (eq[i] == ea || eq[i] == eb) eq[i] = m;
        } else if (m < ea) {
            for (int i = 0; i <= last; i++) if (eq[i] == ea) eq[i] = m;
        } else if (m < eb) {
            for (int i = 0; i <= last; i++) if (eq[i] == eb) eq[i] = m;
        }
    }
    for (int l = 0; l <= k->nrec; l++) {
        if (eq[l] == 0 || eq[l] == l) { eq[l] = (uint8_t)l; continue; }
        int to = eq[l];
        if (l - 1 >= k->maxrec || to > k->maxrec) return -1;
        struct heat_blob *s = &k->rec[l - 1], *t = &k->rec[to - 1];
        t->sw = s->sw + t->sw; t->sw2 = s->sw2 + t->sw2;
        t->swx = s->swx + t->swx; t->swy = s->swy + t->swy;
        t->sm = s->sm + t->sm; t->sm2 = s->sm2 + t->sm2;
        t->smx = s->smx + t->smx; t->smy = s->smy + t->smy;
        if (s->minx < t->minx) t->minx = s->minx;
        if (s->miny < t->miny) t->miny = s->miny;
        if (s->maxx > t->maxx) t->maxx = s->maxx;
        if (s->maxy > t->maxy) t->maxy = s->maxy;
        if (s->peak < t->peak) t->peak = s->peak;
        s->status = 4;
        s->label = 0;
        t->count = (uint16_t)(t->count + s->count);
    }
    k->npairs = 0;
    k->nvalid = 0;
    for (int i = 0; i <= last; i++) {
        struct heat_blob *b = &k->rec[i];
        if (!b->label) b->status = 4;
        else if (b->count > 2 || (c->peak_valid < lut[b->peak] && b->status != 5)) {
            b->status = 1;
            k->nvalid++;
            if (k->maxcount <= b->count) k->maxcount = b->count;
        } else {
            b->status = 4;
            b->label = 0;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Blob features (TouchDetection::FeatureExtraction).

// SetBlobOnBorderFlags.
static void set_border_flags(const struct heat_cfg *c, struct heat_blob *b, int palm) {
    int xedge = 0;
    b->border = 0;
    if (b->minx == 0) { b->border = 1; xedge = 1; }
    else if (b->maxx == c->cols - 1) { b->border = 3; xedge = 1; }
    int yedge = b->miny == 0 ? 2 : b->maxy == c->rows - 1 ? 4 : 0;
    if (yedge) {
        b->border = (uint32_t)yedge;
        b->on_border = 1;
        if (xedge) { b->on_corner = 1; b->border = 5; }
    } else if (xedge) {
        b->on_border = 1;               // (the hinge case needs the stitched topology)
    }
    // Half a junction wider while a palm was seen lately.
    float margin = palm == 1 ? c->border_margin + 0.5f : c->border_margin;
    if (b->x <= margin || b->y <= margin || (float)(c->cols - 1) - margin <= b->x ||
        (float)(c->rows - 1) - margin <= b->y)
        b->near_border = 1;
}

// SetBlobRadius: the blob's widest line (the sum of a line's magnitudes over
// its largest) across its bounding box, times half the pitch and the radius
// factor; across the other axis when the blob touches an x edge.
static float widest(const struct heat_frame *f, const struct heat_blob *b, int along_x) {
    float best = 0.0f;
    int a0 = along_x ? b->miny : b->minx, a1 = along_x ? b->maxy : b->maxx;
    int s0 = along_x ? b->minx : b->miny, s1 = along_x ? b->maxx : b->maxy;
    for (int a = a0; a <= a1; a++) {
        float sum = 0.0f, mx = 0.0f;
        for (int s = s0; s <= s1; s++) {
            float m = along_x ? lut[f->image[a][s]] : lut[f->image[s][a]];
            sum = sum + m;
            if (m > mx) mx = m;
        }
        float q = sum / mx;
        best = q <= best ? best : q;
    }
    return best;
}

static void set_radius(const struct heat_cfg *c, const struct heat_frame *f, struct heat_blob *b) {
    switch (b->border) {
    case 1:
    case 3:
        b->radius = c->pitch_mm[1] * 0.5f * widest(f, b, 0) * c->radius_factor;
        break;
    case 5:
        b->radius = RADIUS_DEFAULT;
        return;
    default:
        b->radius = c->pitch_mm[0] * 0.5f * widest(f, b, 1) * c->radius_factor;
        break;
    }
    if (b->on_hinge) b->radius = RADIUS_DEFAULT;
}

// GetNeighborsDataForPeak: of a junction's four direct neighbours (0 off
// the image), how many are lower, how many equal (within eps), and the last
// equal one.
static void neighbours(const struct heat_cfg *c, const struct heat_frame *f, int x, int y, int *lower, int *equal,
                       int *ex, int *ey) {
    static const int dx[4] = { -1, 1, 0, 0 }, dy[4] = { 0, 0, -1, 1 };
    float v = lut[f->image[y][x]];
    for (int k = 0; k < 4; k++) {
        int nx = x + dx[k], ny = y + dy[k];
        float m = 0.0f;
        if (nx >= 0 && nx < c->cols && ny >= 0 && ny < c->rows) m = lut[f->image[ny][nx]];
        if (!(v <= m + c->peak_eps)) (*lower)++;
        else if (fabsf(v - m) < c->peak_eps) { (*equal)++; *ex = nx; *ey = ny; }
    }
}

// DetectPeakPerJunction: a junction above all four neighbours is a peak; so
// is one of a pair of equal junctions both above their other three
// neighbours (the stronger, or the first in raster order).
static void detect_peak(const struct heat_cfg *c, const struct heat_frame *f, struct heat_blob *b, int x, int y) {
    int lower = 0, equal = 0, ex = 0, ey = 0;
    neighbours(c, f, x, y, &lower, &equal, &ex, &ey);
    if (lower < 3) return;
    if (lower != 4) {
        if (lower != 3 || equal != 1) return;
        int l2 = 0, e2 = 0, ex2 = 0, ey2 = 0;
        neighbours(c, f, ex, ey, &l2, &e2, &ex2, &ey2);
        if (l2 != 3 || e2 != 1) return;
        float own = lut[f->image[y][x]], other = lut[f->image[ey][ex]];
        if (own <= other && (own != other || ex + c->cols * ey <= x + y * c->cols)) return;
    }
    if (b->npeaks < HEAT_MAX_PEAKS) {
        b->peaks[b->npeaks].x = (uint16_t)x;
        b->peaks[b->npeaks].y = (uint16_t)y;
        b->peaks[b->npeaks].valid = 1;
        b->npeaks++;
    }
}

#define M(x, y) lut[f->image[(y)][(x)]]

// MergePeaksByShortSaddle: of two peaks a knight's move, two or one diagonal
// apart, the weaker goes when the junctions between them are nearly as
// strong as the peaks.
static void merge_short_saddle(const struct heat_cfg *c, const struct heat_frame *f, struct heat_blob *b) {
    for (int i = 0; i < b->npeaks; i++)
        for (int j = i + 1; j < b->npeaks; j++) {
            int xi = b->peaks[i].x, yi = b->peaks[i].y, xj = b->peaks[j].x, yj = b->peaks[j].y;
            int ddx = xi - xj, ddy = yi - yj, d2 = ddx * ddx + ddy * ddy;
            float sad, factor;
            if (d2 == 5) {
                if (abs(ddy) == 2) { int my = (yj + yi) >> 1; sad = M(xi, my) + M(xj, my); }
                else { int mx = (xj + xi) >> 1; sad = M(mx, yi) + M(mx, yj); }
                factor = c->saddle_knight;
            } else if (d2 == 4) {
                int mx = (xj + xi) >> 1, my = (yj + yi) >> 1;
                sad = M(mx, my) + M(mx, my);
                factor = c->saddle_2;
            } else if (d2 == 2) {
                sad = M(xi, yj) + M(xj, yi);
                factor = c->saddle_diag;
            } else continue;
            if (!(0.0f < sad)) continue;
            float pi = M(xi, yi), pj = M(xj, yj);
            if (pi + pj < sad * factor) {
                if (pj < pi || (pj <= pi && xi + yi * 0x120 < xj + yj * 0x120)) b->peaks[j].valid = 0;
                else b->peaks[i].valid = 0;
            }
        }
}

// MergePeaksByLongSaddle: for valid peaks further apart, the weakest junction
// on a path between them (stepping towards the stronger neighbour) decides.
static void merge_long_saddle(const struct heat_cfg *c, const struct heat_frame *f, struct heat_blob *b) {
    for (int i = 0; i < b->npeaks; i++)
        for (int j = i + 1; j < b->npeaks; j++) {
            if (b->peaks[i].valid != 1 || b->peaks[j].valid != 1) continue;
            int xi = b->peaks[i].x, yi = b->peaks[i].y, xj = b->peaks[j].x, yj = b->peaks[j].y;
            int ddy = yj - yi, ddx = xj - xi;
            unsigned d2 = (unsigned)(ddy * ddy + ddx * ddx);
            if (!(5 < d2 && (int)d2 < c->saddle_long_d2)) continue;
            int sx = ddx >= 0 ? 1 : -1, sy = ddy >= 0 ? 1 : -1;
            float pi = M(xi, yi), pj = M(xj, yj), low = 1.0f;
            if (yi != yj || xi != xj) {
                int ay = abs(ddy), ax = abs(ddx), x = xi, y = yi;
                do {
                    if ((ay & 0xff) == 0 || (ax & 0xff) == 0) {
                        if ((ax & 0xff) == 0) { ax = 0; y += sy; ay--; }
                        else { x += sx; ax--; }
                    } else {
                        int ny = sy + y, nx = sx + x;
                        if (M(x, ny) <= M(nx, y)) { ax--; x = nx; }
                        else { ay--; y = ny; }
                    }
                    float v = M(x, y);
                    if (low <= v) v = low;
                    low = v;
                } while (xj != x || yj != y);
            }
            float k = c->saddle_long;
            if (pi + pj < low * (k + k)) {
                if (pj < pi || (pj <= pi && xi + yi * 0x120 < xj + yj * 0x120)) b->peaks[j].valid = 0;
                else b->peaks[i].valid = 0;
            }
        }
}

// atanf as bionic computes it (FreeBSD msun s_atanf.c, no fused operations):
// the orientation is truncated to whole centi-degrees, so an ulp matters.
static float msun_atanf(float x) {
    static const float atanhi[] = { 4.6364760399e-01f, 7.8539812565e-01f, 9.8279368877e-01f, 1.5707962513e+00f };
    static const float atanlo[] = { 5.0121582440e-09f, 3.7748947079e-08f, 3.4473217170e-08f, 7.5497894159e-08f };
    static const float aT[] = { 3.3333328366e-01f, -1.9999158382e-01f, 1.4253635705e-01f, -1.0648017377e-01f,
                                6.1687607318e-02f };
    uint32_t u;
    memcpy(&u, &x, 4);
    int32_t hx = (int32_t)u, ix = hx & 0x7fffffff, id;
    if (ix >= 0x4c800000) {
        if (ix > 0x7f800000) return x + x;
        return hx > 0 ? atanhi[3] + atanlo[3] : -atanhi[3] - atanlo[3];
    }
    if (ix < 0x3ee00000) {
        if (ix < 0x39800000) return x;
        id = -1;
    } else {
        x = fabsf(x);
        if (ix < 0x3f980000) {
            if (ix < 0x3f300000) { id = 0; x = (2.0f * x - 1.0f) / (2.0f + x); }
            else { id = 1; x = (x - 1.0f) / (x + 1.0f); }
        } else {
            if (ix < 0x401c0000) { id = 2; x = (x - 1.5f) / (1.0f + 1.5f * x); }
            else { id = 3; x = -1.0f / x; }
        }
    }
    float z = x * x, w = z * z;
    float s1 = z * (aT[0] + w * (aT[2] + w * aT[4]));
    float s2 = w * (aT[1] + w * aT[3]);
    if (id < 0) return x - x * (s1 + s2);
    z = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
    return hx < 0 ? -z : z;
}

// The blob's second moments as an ellipse: orientation (centi-degrees),
// axis ratio and spread (end of FeatureExtraction).
static void shape(struct heat_blob *b, float sxx, float syy, float sxy) {
    float inv = 1.0f / b->sm;
    float a = sxy * inv, xx = sxx * inv, yy = syy * inv;   // (+0x16604 set: x stays x)
    float tr = yy + xx;
    float disc = sqrtf(tr * tr + (yy * xx - a * a) * -4.0f);
    float l1 = (tr - disc) * 0.5f, l2 = (tr + disc) * 0.5f;
    if (l1 <= 0.0f) l1 = 0.0f;
    float big = (l2 == l1 || l1 > l2) ? l1 : l2;
    float small = l1 <= l2 ? l1 : l2;
    float minor, major;
    int axes = 1;
    if (yy == xx) {
        b->orientation = 0;
        if (yy == 0.0f) { minor = major = 1.15f; axes = 0; }
        else minor = sqrtf(small) * 4.6188f;
    } else if (yy == 0.0f || xx == 0.0f) {
        b->orientation = yy == 0.0f ? 9000 : 0;
        minor = 1.15f;
    } else {
        float base, ang;
        if (yy <= xx) {
            ang = msun_atanf((a + a) / (yy - xx)) * 0.5f * 18000.0f / 3.1415927f;
            base = 9000.0f;
            ang = ang + base;           // fVar21 + fVar25
        } else {
            float b0 = (a == 0.0f || a < 0.0f) ? 18000.0f : 0.0f;
            base = a != 0.0f ? b0 : 9000.0f;
            ang = base + msun_atanf((a + a) / (yy - xx)) * 0.5f * 18000.0f / 3.1415927f;
        }
        int16_t o = (int16_t)(int)ang;
        unsigned u = (unsigned)((int)ang + 9000);
        b->orientation = (uint16_t)(int16_t)u;
        if (((u >> 4) & 0xfff) > 0x464) b->orientation = (uint16_t)(int16_t)(o - 9000);
        minor = sqrtf(small) * 4.6188f;
    }
    if (axes) {
        if (minor < 1.0f) minor = 1.15f;
        major = sqrtf(big) * 4.6188f;
        if (major < 1.0f) major = 1.15f;
    }
    b->ratio = major / minor;
    b->spread = b->count > 1 ? (tr * 6.2831855f) / (float)(int)(b->count - 1) : 1.0f;
    b->count9e = b->count;
}

// RunTopographySegmentationFeature: the blob's own junctions scanned again
// at three levels between the threshold and its peak, 4-connected: how many
// sub-blobs each level splits it into, and the largest one's size.
static int topography(const struct heat_cfg *c, struct heat_detector *d, const struct heat_frame *f,
                      struct heat_blob *b) {
    float peak = lut[b->peak];
    if (!(c->topo_base + c->topo_floor < peak) || b->big == 1) return 0;
    float diff = (peak - c->topo_base) - c->topo_floor;
    static struct heat_blob rec[16];
    static uint8_t labels[HEAT_MAX_X * HEAT_MAX_Y], eq[0xf0 + 1], pairs[0x2d0][2];
    for (int lv = 0; lv < 3; lv++) {
        memset(rec, 0, sizeof rec);
        memset(eq, 0, sizeof eq);
        struct ccl k = {
            .f = f, .thr = c->topo_floor + diff * c->topo_level[lv], .x0 = b->minx, .y0 = b->miny,
            .x1 = b->maxx, .y1 = b->maxy, .conn8 = 0, .only_label = b->label, .main = d, .labels = labels,
            .stride = b->maxx - b->minx + 1, .rec = rec, .maxrec = 16, .eq = eq, .pairs = pairs, .maxpairs = 0xf0,
        };
        if (run_ccl(&k) < 0) return 0;
        if (merge_blobs(c, &k) < 0) return -1;
        b->topo[2 * lv] = (uint8_t)k.nvalid;
        b->topo[2 * lv + 1] = (uint8_t)k.maxcount;
    }
    return 0;
}

// HaloFeatureCalculation: the magnitude in the ring of weaker junctions
// around the blob (up to two antennas out, each at least a fraction of its
// inner neighbour), relative to the peak. Large blobs get 100.
#define H(xo, yo) map[(xo) * 16 + (yo)]
static void halo_ring(const struct heat_cfg *c, const struct heat_frame *f, struct heat_blob *b, uint8_t *map,
                      float lim, float frac, int level) {
    int y0 = b->miny - level, y1 = b->maxy + level, x0 = b->minx - level, x1 = b->maxx + level;
    if (y0 < 0) y0 = 0;
    if (y1 >= c->rows) y1 = c->rows - 1;
    if (x0 < 0) x0 = 0;
    if (x1 >= c->cols) x1 = c->cols - 1;
    float eps = frac + -0.0001f;
    for (int y = y0; y <= y1; y++) {
        int yo = y - b->miny + 3;
        for (int x = x0; x <= x1; x++) {
            int xo = 3 - b->minx + x;
            if (!(H(xo, yo) <= level)) continue;
            float v = M(x, y);
            static const int ddx[4] = { -1, 0, 1, 0 }, ddy[4] = { 0, -1, 0, 1 };
            for (int q = 0; q < 4; q++) {
                if (H(xo + ddx[q], yo + ddy[q]) != 4) continue;
                float n = M(x + ddx[q], y + ddy[q]);
                if (n < lim && n < v && eps < n / v) {
                    H(xo + ddx[q], yo + ddy[q]) = (uint8_t)(level + 1);
                    b->halo_sum = n + b->halo_sum;
                }
            }
        }
    }
}

static void halo(const struct heat_cfg *c, const struct heat_detector *d, const struct heat_frame *f,
                 struct heat_blob *b) {
    b->halo_sum = 0.0f;
    if (!((b->maxx - b->minx) + 1 < 11 && (b->maxy - b->miny) + 1 < 11)) { b->halo = 100.0f; return; }
    float lim = c->halo_fraction * lut[b->peak];
    // InitHaloMap: 0 the blob's own junctions, 4 free ones, 5 anything else
    // (another label, or outside the image), within three of the box.
    uint8_t map[256];
    memset(map, 5, sizeof map);
    int x0 = b->minx < 4 ? 0 : b->minx - 3, y0 = b->miny < 4 ? 0 : b->miny - 3;
    int x1 = b->maxx + 3 >= c->cols ? c->cols - 1 : b->maxx + 3;
    int y1 = b->maxy + 3 >= c->rows ? c->rows - 1 : b->maxy + 3;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int l = d->label[y][x];
            if (l == b->label) H(x - b->minx + 3, y - b->miny + 3) = 0;
            else if (l == 0) H(x - b->minx + 3, y - b->miny + 3) = 4;
        }
    // HaloFeatureRunMeatDetectionPhase: when that limit is under the scan
    // threshold, free junctions above it touching the blob are its own.
    if (lim < d->thr) {
        for (int y = b->miny; y <= b->maxy; y++) {
            int yo = y - b->miny + 3;
            for (int xo = 3; b->minx + xo - 3 <= b->maxx; xo++) {
                if (H(xo, yo) != 0) continue;
                int x = b->minx + xo - 3;
                if (H(xo - 1, yo) == 4 && lim < M(x - 1, y)) H(xo - 1, yo) = 1;
                if (H(xo, yo - 1) == 4 && lim < M(x, y - 1)) H(xo, yo - 1) = 1;
                if (H(xo + 1, yo) == 4 && lim < M(x + 1, y)) H(xo + 1, yo) = 1;
                if (H(xo, yo + 1) == 4 && lim < M(x, y + 1)) H(xo, yo + 1) = 1;
            }
        }
    }
    halo_ring(c, f, b, map, lim, c->halo_ring[0], 1);
    halo_ring(c, f, b, map, lim, c->halo_ring[1], 2);
    float h = b->halo_sum / lut[b->peak];
    b->halo = b->near_border == 1 ? h + h : h;
}
#undef H

static int features(const struct heat_cfg *c, struct heat_detector *d, const struct heat_frame *f) {
    int n = d->nblobs, skipped = 0;
    for (int i = 0; i < n + skipped; i++) {
        if (i >= HEAT_MAX_BLOBS) return -1;
        struct heat_blob *b = &d->blob[i];
        if (!b->label || (b->status & 0xfe) == 4) {
            if (i > 0x2e) return -1;
            skipped++;
            continue;
        }
        float inv = 1.0f / b->sw;
        b->x = b->swx * inv;
        b->y = b->swy * inv;
        // (A pen nearby would be checked here: none on the Duo 2's panels.)
        if (!(PEAK_MIN <= lut[b->peak])) {
            d->nblobs--;
            b->status = 5;
            b->label = 0;
            continue;
        }
        if (b->count >= c->max_junctions) return -1;
        if (b->count >= c->big_blob || (b->count >= c->big_blob_palm && d->palm)) {
            // A palm: no shape features of its own but the palm class's
            // means, and a fixed radius.
            const float *m = c->palm_features;
            b->radius = RADIUS_DEFAULT;
            b->x = b->swx / b->sw;
            b->y = b->swy / b->sw;
            b->count9e = (uint16_t)(int16_t)(int)m[0];
            b->topo[1] = (uint8_t)(int)m[1];
            b->topo[3] = (uint8_t)(int)m[2];
            b->topo[5] = (uint8_t)(int)m[3];
            b->topo[0] = (uint8_t)(int)m[4];
            b->topo[2] = (uint8_t)(int)m[5];
            b->topo[4] = (uint8_t)(int)m[6];
            b->ratio = m[7];
            b->spread = m[8];
            b->halo = 2.0f;
            b->npeaks = b->nvalid_peaks = 1;
            b->big = 1;
            continue;
        }
        set_border_flags(c, b, d->palm);
        set_radius(c, f, b);
        // Second moments about the magnitude centroid, and the peaks.
        float sxx = 0.0f, syy = 0.0f, sxy = 0.0f;
        for (int y = b->miny; y <= b->maxy; y++) {
            float dy = (float)y - (1.0f / b->sm) * b->smy;
            for (int x = b->minx; x <= b->maxx; x++) {
                if (d->eq[d->label[y][x]] != b->label) continue;
                float dx = (float)x - b->smx * (1.0f / b->sm);
                float m = M(x, y);
                float t = dx * m;
                sxx = sxx + dx * t;
                syy = syy + dy * (dy * m);
                sxy = sxy + dy * t;
                detect_peak(c, f, b, x, y);
            }
        }
        merge_short_saddle(c, f, b);
        merge_long_saddle(c, f, b);
        for (int k = 0; k < b->npeaks; k++) {
            if (b->peaks[k].valid != 1) continue;
            if (M(b->peaks[k].x, b->peaks[k].y) <= PEAK_MIN) b->peaks[k].valid = 0;
            else b->nvalid_peaks++;
        }
        b->topo[0] = b->topo[2] = b->topo[4] = 1;
        b->topo[1] = b->topo[3] = b->topo[5] = (uint8_t)b->count;
        if (topography(c, d, f, b) < 0) return -1;
        halo(c, d, f, b);
        shape(b, sxx, syy, sxy);
    }
    return 0;
}

int heat_detect(const struct heat_cfg *c, struct heat_detector *d, const struct heat_frame *f) {
    // CclLogic: the scan threshold for this frame's noise level.
    int e = 0;
    while (e < c->thr_count - 1 && !(f->noise < c->noise_limit[e])) e++;
    float thr = c->thr_table[e];
    if (thr <= c->thr_min) thr = c->thr_min;
    if (c->thr_max <= thr) thr = c->thr_max;
    d->thr = thr;
    memset(d->eq, 0, sizeof d->eq);
    memset(d->blob, 0, sizeof d->blob);
    d->nrecords = 0;
    d->nblobs = 0;
    static uint8_t pairs[0x2d0][2];
    struct ccl k = {
        .f = f, .thr = thr, .x0 = 0, .y0 = 0, .x1 = c->cols - 1, .y1 = c->rows - 1, .conn8 = 1,
        .labels = &d->label[0][0], .stride = HEAT_MAX_X, .rec = d->blob, .maxrec = HEAT_MAX_BLOBS,
        .eq = d->eq, .pairs = pairs, .maxpairs = 0x2d0,
    };
    int rc = run_ccl(&k);
    d->nrecords = k.nrec;
    if (rc < 0 || merge_blobs(c, &k) < 0) return -1;
    d->nblobs = k.nvalid;
    if (features(c, d, f) < 0) return -1;
    return d->nblobs;
}

void heat_dump_blobs(FILE *out, const struct heat_detector *d, const struct heat_frame *f) {
    fprintf(out, "P blobs n=%u noise=%u\n", (unsigned)d->nblobs, f->noise);
    for (int i = 0; i < HEAT_MAX_BLOBS; i++) {
        const struct heat_blob *b = &d->blob[i];
        if (!b->label) continue;
        fprintf(out, "P blob %d st=%u lab=%u x=%.6f y=%.6f r=%.6f n=%u peak=%u box=%u,%u..%u,%u border=%u big=%u peaks=%u/%u sw=%.6f\n",
                i, b->status, b->label, (double)b->x, (double)b->y, (double)b->radius, b->count, b->peak, b->minx,
                b->miny, b->maxx, b->maxy, b->border, b->big, b->nvalid_peaks, b->npeaks, (double)b->sw);
        fprintf(out, "P blobx %d o=%u ratio=%.6f spread=%.6f halo=%.6f topo=%u,%u,%u,%u,%u,%u n9e=%u flags=%u,%u,%u,%u pk=", i,
                b->orientation, (double)b->ratio, (double)b->spread, (double)b->halo, b->topo[0], b->topo[1], b->topo[2],
                b->topo[3], b->topo[4], b->topo[5], b->count9e, b->near_border, b->on_border, b->on_corner, b->on_hinge);
        for (int k = 0; k < b->npeaks; k++)
            fprintf(out, "%s%u,%u,%u", k ? ";" : "", b->peaks[k].x, b->peaks[k].y, b->peaks[k].valid);
        fprintf(out, "\n");
    }
}

// ---------------------------------------------------------------------------
// Antennas to the panel's millimetres (DetectedArray::ProcessDetectedBlobs).

// ReportInformation::RunStretching, one axis: near an edge, [knee, zone] is
// spread over [0, zone], so a finger whose centre cannot reach the antennas'
// edge can still reach the screen's.
static float stretch(const struct heat_cfg *c, float p, float r, int a) {
    const float *s = c->stretch[a];
    float size = c->size_mm[a];
    float z = s[0] + r;
    if (!(z <= p)) {
        float k = (float)((double)r * STRETCH_A + STRETCH_B + (double)s[1]);
        return (z * p - z * k) / (z - k);
    }
    float q = size - p;
    z = r + s[2];
    if (q < z) {
        float k = (float)((double)r * STRETCH_A + STRETCH_B + (double)s[3]);
        return size - (q * z - z * k) / (z - k);
    }
    return p;
}

int heat_to_mm(const struct heat_cfg *c, float ax, float ay, float radius, int valid, struct heat_finger *o) {
    o->ax = ax;
    o->ay = ay;
    o->radius = radius;
    // ConvertPositionFromAntennaToMmAndFlip (no edge antennas, no flip).
    float x = ax * c->pitch_mm[0], y = ay * c->pitch_mm[1];
    // FixOffsetRotation and the stretching apply to valid records only.
    // FixOffsetRotation: the centring offset, plus the unit's alignment on x.
    if (valid) {
        x = x - (c->offset_mm[0] + c->align_mm);
        y = y - c->offset_mm[1];
    }
    if (valid && c->stretch_on) {
        // x first; y's stretch uses the same radius.
        float sx = stretch(c, x, radius, 0);
        float sy = stretch(c, y, radius, 1);
        x = sx;
        y = sy;
    }
    // FixCropDrop (dropping by distance outside is off for the Duo 2).
    if (!(0.0f <= x)) x = 0.0f;
    else if (c->size_mm[0] < x) x = c->size_mm[0];
    if (!(0.0f <= y)) y = 0.0f;
    else if (!(y <= c->size_mm[1])) y = c->size_mm[1];
    o->x = x;
    o->y = y;
    return 1;
}

// ---------------------------------------------------------------------------
// Tracking: each finger joins the track whose predicted position (last
// position + last velocity) is nearest, closest pairs first; unmatched
// fingers open tracks. (To be replaced by the stock tracker.)

#define TRACK_GATE_MM 25.0f

void heat_track_reset(struct heat_tracker *tr) {
    for (int i = 0; i < HEAT_MAX_TRACKS; i++) tr->t[i].active = 0;
}

static int clampi(float v, int max) {
    int i = (int)lrintf(v);
    return i < 0 ? 0 : i > max ? max : i;
}

int heat_track(const struct heat_cfg *c, struct heat_tracker *tr, const struct heat_finger *b, int nb,
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
    // TracksArray::FillOsReport: millimetres to the panel's logical units,
    // axes swapped so the report's x runs across the short side.
    float sx = (float)c->logical_max[0] / c->size_mm[0], sy = (float)c->logical_max[1] / c->size_mm[1];
    int n = 0;
    for (int t = 0; t < HEAT_MAX_TRACKS && n < max; t++) {
        const struct heat_track *k = &tr->t[t];
        if (!k->active) continue;
        struct heat_contact *o = &out[n++];
        o->id = k->id;
        o->x = clampi(k->y * sy, c->logical_max[1]);
        o->y = clampi(k->x * sx, c->logical_max[0]);
        o->major = o->minor = clampi(2.0f * k->radius * sy, c->logical_max[1]);
    }
    return n;
}
