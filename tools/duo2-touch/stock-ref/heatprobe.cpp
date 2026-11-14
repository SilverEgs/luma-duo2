// libheatprobe.so: preloaded under duo2-heatref, prints the stock heat
// processor's intermediate results so duo2-touch can be checked against it
// stage by stage. libsurfacetouch_c1.so calls its own exported functions
// through its PLT, so a preloaded definition wraps them: each probe calls the
// library's own function (dlsym on the library) and then reads the object it
// worked on, at the offsets docs/touch.md gives.
//
// Also answers IStats::tryGetService with no service (see statsshim.cpp).
//
// HEATPROBE_IMAGE=1 also prints every heat image as the library holds it;
// HEATPROBE_TRACKS=1 the track table and the tracker's output every frame.

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT extern "C" __attribute__((visibility("default")))

struct NullSp { void *ptr = nullptr; ~NullSp() {} };
EXPORT NullSp probe_try_get_service(const void *, bool) __asm__(
    "_ZN7android10frameworks5stats4V1_06IStats13tryGetServiceERKNSt3__112basic_"
    "stringIcNS4_11char_traitsIcEENS4_9allocatorIcEEEEb");
EXPORT NullSp probe_try_get_service(const void *, bool) { return NullSp(); }

template <typename T> static T rd(const void *p, size_t off) { T v; memcpy(&v, (const char *)p + off, sizeof v); return v; }

// The library itself, already loaded by duo2-heatref (it is dlopen()ed after
// this preload, so RTLD_NEXT does not reach it).
template <typename Fn> static Fn next(const char *name) {
    static void *lib = dlopen("libsurfacetouch_c1.so", RTLD_NOW | RTLD_NOLOAD);
    void *f = lib ? dlsym(lib, name) : nullptr;
    if (!f) { fprintf(stderr, "heatprobe: no %s\n", name); abort(); }
    return reinterpret_cast<Fn>(f);
}

#define SYM_PROCESS_TOUCH "_ZN14TouchDetection12ProcessTouchEP20TouchProcessingInput"
#define SYM_FEATURES "_ZN14TouchDetection17FeatureExtractionEP20TouchProcessingInput"
#define SYM_DETECTED "_ZN13DetectedArray20ProcessDetectedBlobsEP25TracklibPrepareTrackingIn"

// TouchDetection::ProcessTouch: one call per panel per frame.
EXPORT long probe_process_touch(void *td, uint8_t *in) __asm__(SYM_PROCESS_TOUCH);
EXPORT long probe_process_touch(void *td, uint8_t *in) {
    static auto real = next<long (*)(void *, uint8_t *)>(SYM_PROCESS_TOUCH);
    unsigned cols = rd<uint16_t>(td, 0x16526), rows = rd<uint16_t>(td, 0x16524);
    printf("P frame td=%p cols=%u rows=%u ts=%u,%u nz=%u pen=%u post=%u,%u\n", td, cols, rows, rd<uint16_t>(in, 0xb6a8),
           rd<uint16_t>(in, 0xb6a6), in[0xb6c3], in[0xb7d8], rd<uint32_t>(in, 0xb7cc), ((const uint8_t *)td)[0x19639]);
    static void *seen[4];
    int known = 0;
    for (void *&k : seen) { if (k == td) known = 1; if (!known && !k) { k = td; break; } }
    if (!known) {
        // CommonTrackingCfg as InitCommonTrackingCfg built it (+0x16590), and
        // the per-antenna magnitude table (+0xc590) endpoints.
        const uint8_t *c = (const uint8_t *)td + 0x16590;
        printf("P cfg");
        for (int o = 0; o < 0x78; o += 4)
            printf(" %02x:%08x/%.9g", o, rd<uint32_t>(c, o), (double)rd<float>(c, o));
        printf("\nP lut 0=%.9g 180=%.9g 255=%.9g\n", (double)rd<float>(td, 0xc590),
               (double)rd<float>(td, 0xc590 + 180 * 4), (double)rd<float>(td, 0xc590 + 255 * 4));
        // TouchDetection's own settings (+0x16520..+0x16660), and where its
        // configuration pointers lead: TouchDetectionCfg (+0x16608, as an
        // offset into PS_CFG_DATA_Zeta when it points there), +0x16618, +0x19600.
        printf("P tdcfg");
        for (int o = 0x16520; o < 0x16660; o += 4)
            if (o < 0x16590 || o >= 0x16608) printf(" %x:%08x/%.9g", o, rd<uint32_t>(td, o), (double)rd<float>(td, o));
        printf("\n");
        printf("P tdcfg2");
        for (int o = 0x19600; o < 0x19640; o += 4) printf(" %x:%08x/%.9g", o, rd<uint32_t>(td, o), (double)rd<float>(td, o));
        printf("\n");
        static const uint8_t *ps = (const uint8_t *)dlsym(dlopen("libsurfacetouch_c1.so", RTLD_NOW | RTLD_NOLOAD), "PS_CFG_DATA_Zeta");
        static const int ptrs[] = { 0x16608, 0x16610, 0x16618, 0x19600 };
        for (int o : ptrs) {
            const uint8_t *q = rd<const uint8_t *>(td, o);
            printf("P tdptr %x=%p ps+%ld:", o, q, ps && q ? (long)(q - ps) : -1L);
            for (int k = 0; q && k < 0x20; k += 4) printf(" %08x/%.9g", rd<uint32_t>(q, k), (double)rd<float>(q, k));
            printf("\n");
        }
    }
    static int image = getenv("HEATPROBE_IMAGE") != nullptr;
    if (image) {
        for (unsigned y = 0; y < rows && y < 64; y++) {
            printf("P img %2u", y);
            for (unsigned x = 0; x < cols && x < 64; x++) printf(" %02x", in[y * 0x120 + x]);
            printf("\n");
        }
    }
    long rc = real(td, in);
    static int tracks = getenv("HEATPROBE_TRACKS") != nullptr;
    if (tracks) {
        // The track table (48 x 0x25c from +0xf3e0) after
        // TouchDetectionPostProcess: every track not free.
        for (int i = 0; i < 48; i++) {
            const uint8_t *t = (const uint8_t *)td + 0xf3e0 + i * 0x25c;
            unsigned st = rd<uint32_t>(t, 0x3c);
            if (!st) continue;
            unsigned ring = t[0x259];
            const uint8_t *h = t + 0x60 + ring * 0x30;
            printf("P trk %d st=%u ty=%u x=%.6f y=%.6f r=%.6f v=%.6f,%.6f fr=%u n=%u blob=%u lab=%u hn=%u f=",
                   i, st, rd<uint32_t>(h, 0x18), (double)rd<float>(t, 0), (double)rd<float>(t, 4),
                   (double)rd<float>(t, 0x18), (double)rd<float>(t, 0x1c), (double)rd<float>(t, 0x20),
                   rd<uint16_t>(t, 0x2c), rd<uint16_t>(t, 0x2e), rd<uint16_t>(t, 0x30), t[0x37], rd<uint16_t>(h, 0x1c));
            for (int o = 0x40; o < 0x48; o++) printf("%02x", t[o]);
            printf(" c=%u,%u,%u g=", rd<uint16_t>(t, 0x250), rd<uint16_t>(t, 0x252), rd<uint16_t>(t, 0x254));
            for (int o = 0x256; o < 0x25c; o++) printf("%02x", t[o]);
            printf(" llr=%.6f,%.6f,%.6f,%.6f\n", (double)rd<float>(t, 8), (double)rd<float>(t, 0xc),
                   (double)rd<float>(t, 0x10), (double)rd<float>(t, 0x14));
        }
        // FillTrackLibInput's output (ProtectedBlobData at input +0xaf84):
        // ReportInformation records of 0x34 bytes from +4.
        const uint8_t *pb = in + 0xaf84;
        printf("P tl n=%u b2=%u\n", pb[0], pb[2]);
        for (unsigned i = 0; i < pb[0] && i < 0x22; i++) {
            const uint8_t *a = pb + 4 + i * 0x34;
            printf("P tli %u x=%.6f y=%.6f r=%.6f h=", i, (double)rd<float>(a, 0), (double)rd<float>(a, 4),
                   (double)rd<float>(a, 0x10));
            for (int o = 0x8; o < 0x34; o++) printf("%02x", a[o]);
            printf("\n");
        }
    }
    return rc;
}

// TouchDetection::FeatureExtraction: the blob records (0xb0 bytes from +0xca58).
EXPORT long probe_features(void *td, void *in) __asm__(SYM_FEATURES);
EXPORT long probe_features(void *td, void *in) {
    static auto real = next<long (*)(void *, void *)>(SYM_FEATURES);
    long rc = real(td, in);
    const uint8_t *b = (const uint8_t *)td + 0xca58;
    printf("P blobs n=%u noise=%u\n", rd<uint8_t>(td, 0xf3de), rd<uint16_t>(td, 0x16532));
    for (int i = 0; i < 48; i++, b += 0xb0) {
        if (!b[0x41]) continue;
        printf("P blob %d st=%u lab=%u x=%.6f y=%.6f r=%.6f n=%u peak=%u box=%u,%u..%u,%u border=%u big=%u peaks=%u/%u sw=%.6f\n",
               i, b[0x40], b[0x41], rd<float>(b, 0x24), rd<float>(b, 0x28), rd<float>(b, 0x2c), rd<uint16_t>(b, 0x38),
               b[0x4c], rd<uint16_t>(b, 0x30), rd<uint16_t>(b, 0x32), rd<uint16_t>(b, 0x34), rd<uint16_t>(b, 0x36),
               rd<uint32_t>(b, 0x44), b[0x43], b[0x4e], b[0x4d], rd<float>(b, 0));
        // The shape features classification uses.
        printf("P blobx %d o=%u ratio=%.6f spread=%.6f halo=%.6f topo=%u,%u,%u,%u,%u,%u n9e=%u flags=%u,%u,%u,%u pk=", i,
               rd<uint16_t>(b, 0x3e), rd<float>(b, 0x8c), rd<float>(b, 0x90), rd<float>(b, 0x94), b[0x98], b[0x99],
               b[0x9a], b[0x9b], b[0x9c], b[0x9d], rd<uint16_t>(b, 0x9e), b[0x48], b[0x49], b[0x4a], b[0x4b]);
        for (int k = 0; k < b[0x4d] && k < 10; k++)
            printf("%s%u,%u,%u", k ? ";" : "", rd<uint16_t>(b, 0x50 + 6 * k), rd<uint16_t>(b, 0x52 + 6 * k), b[0x54 + 6 * k]);
        printf("\n");
    }
    return rc;
}

// DetectedArray::ProcessDetectedBlobs: the fingers handed to the tracker
// (ReportInformation, 0x34 bytes each), in mm after offset and stretching.
EXPORT void probe_detected(void *da, void *in) __asm__(SYM_DETECTED);
EXPORT void probe_detected(void *da, void *in) {
    static auto real = next<void (*)(void *, void *)>(SYM_DETECTED);
    real(da, in);
    static int once;
    if (!once++) {
        // AspectRatio's state (set by the host over CNM) and DetectedArray's
        // offset/rotation configuration (+0x18..+0x30, centring at +0x1c).
        static void *ar = dlsym(dlopen("libsurfacetouch_c1.so", RTLD_NOW | RTLD_NOLOAD),
                                "_ZZN11AspectRatio8InstanceEvE9mInstance");
        if (ar) {
            printf("P aspect");
            for (int o = 0; o < 0x24; o += 4) printf(" %02x:%08x/%.9g", o, rd<uint32_t>(ar, o), (double)rd<float>(ar, o));
            printf("\n");
        }
        // The alignment reader (offsets at reader+8, so *(da+0xa0) - 8) and the
        // limits it clamps them to (ConfigurationDataBase+0x104: max, enable at +7).
        if (const uint8_t *al = rd<const uint8_t *>(da, 0xa0)) {
            const uint8_t *lim = rd<const uint8_t *>(al, 0x10);
            printf("P alignlimit max=%.9g enable=%u valid=%u,%u\n", (double)rd<float>(lim, 0), lim[7], al[8], al[9]);
            Dl_info info{};
            const void *vt = rd<const void *>(al, -8);
            if (dladdr(vt, &info) && info.dli_sname)
                printf("P alignreader %s+%ld mode=%u own_region=%u\n", info.dli_sname, (long)((const char *)vt - (const char *)info.dli_saddr), lim[4], rd<uint32_t>(al, 0x28));
        }
        printf("P darray");
        for (int o = 0x18; o < 0xb0; o += 4) printf(" %02x:%08x/%.9g", o, rd<uint32_t>(da, o), (double)rd<float>(da, o));
        printf("\n");
    }
    const uint8_t *a = rd<const uint8_t *>(da, 0);
    unsigned n = rd<uint8_t>(da, 0x3a);
    // The panel alignment offsets FixOffsetRotation adds (per region, from
    // PanelAlignmentReaderBase at *(da+0xa0)).
    const float *al = rd<const float *>(da, 0xa0);
    printf("P fingers n=%u da=%p align=%.6f,%.6f\n", n, da, al ? (double)al[0] : 0.0, al ? (double)al[1] : 0.0);
    for (unsigned i = 0; i < n && a; i++, a += 0x34)
        printf("P finger %u x=%.6f y=%.6f ax=%.6f ay=%.6f r=%.6f type=%u valid=%u sep=%u td=%u nsr=%u\n", i,
               rd<float>(a, 0), rd<float>(a, 4), rd<float>(a, 8), rd<float>(a, 0xc), rd<float>(a, 0x10), a[0x26],
               a[0x2d], a[0x2e], a[0x2f], rd<uint16_t>(a, 0x22));
}

// TrackLib::RunTrackingProcess (per panel per frame, after DetectedArray's
// input is ready): its input, then the tracks (TrackedFinger, 20 x 0x1f0 at
// TrackLib +0x748 +0x20) and the OS report it filled, and once the tracker's
// configuration (where each TrackedFinger::mp* points in PS_CFG_DATA_Zeta,
// and CommonTrackingCfg). HEATPROBE_TRACKLIB=1.
#define SYM_TRACKLIB "_ZN8TrackLib18RunTrackingProcessEP12TuProcessOutP14TuProcessInput"
static void hex(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", p[i]); }
EXPORT void probe_tracklib(uint8_t *tl, uint8_t *out, uint8_t *in) __asm__(SYM_TRACKLIB);
EXPORT void probe_tracklib(uint8_t *tl, uint8_t *out, uint8_t *in) {
    static auto real = next<void (*)(uint8_t *, uint8_t *, uint8_t *)>(SYM_TRACKLIB);
    static int on = getenv("HEATPROBE_TRACKLIB") != nullptr;
    if (!on) { real(tl, out, in); return; }
    static void *lib = dlopen("libsurfacetouch_c1.so", RTLD_NOW | RTLD_NOLOAD);
    static const uint8_t *ps = (const uint8_t *)dlsym(lib, "PS_CFG_DATA_Zeta");
    static int once;
    if (!once++) {
        static const char *names[] = { "mpSmallFingerCfg", "mpMatching", "mpJitterRestrictorConfig", "mpKalmanConfig",
            "mpVelocitiesConfig", "mpPixel0Cfg", "mpPrediction", "mpProcessStd2", "mpBlobSizeSmoothConfig",
            "mpNoiseManageConfig", "mpFastMovement", "mpIirCfg" };
        for (const char *nm : names) {
            char sym[128];
            snprintf(sym, sizeof sym, "_ZN13TrackedFinger%zu%sE", strlen(nm), nm);
            const uint8_t **v = (const uint8_t **)dlsym(lib, sym);
            const uint8_t *q = v ? *v : nullptr;
            printf("P tfcfg %s=%p ps+%#lx\n", nm, q, ps && q ? (long)(q - ps) : -1L);
        }
        const uint8_t **ct = (const uint8_t **)dlsym(lib, "_ZN8TrackLib19mpCommonTrackingCfgE");
        if (ct && *ct) { printf("P ctc "); hex(*ct, 0x78); printf("\n"); }
        printf("P tlstate "); hex(tl + 0x2e68, 0x50); printf("\n");
        {
            const uint8_t *t0 = tl + 0x748 + 0x20;
            printf("P tfptr proj=%u", rd<uint32_t>(t0, 0x198));
            for (int o = 0x1a0; o <= 0x1e8; o += 8) {
                const uint8_t *q = rd<const uint8_t *>(t0, o);
                Dl_info di{};
                if (q && dladdr(q, &di) && di.dli_sname) printf(" %x=%s+%ld", o, di.dli_sname, (long)(q - (const uint8_t *)di.dli_saddr));
                else printf(" %x=%p", o, q);
            }
            const uint8_t *fl = rd<const uint8_t *>(t0, 0x1b0);
            if (fl) { printf(" flags1b0="); hex(fl, 8); }
            const uint8_t *f8 = rd<const uint8_t *>(t0, 0x1b8);
            if (f8) { printf(" flags1b8="); hex(f8, 8); }
            printf(" raw="); hex(t0 + 0x190, 0x60); printf("\n");
        }
        const uint8_t *ta = tl + 0x748;
        printf("P tastate "); hex(ta + 0x26f0, 0x30); printf(" sep="); hex(tl + 0xb8, 0x40); printf("\n");
    }
    const uint8_t *pi = *(const uint8_t **)in;
    printf("P tlin f=%02x%02x%02x pi=", in[0x10], in[0x11], in[0x12]); hex(pi, 0xc0); printf("\n");
    real(tl, out, in);
    const uint8_t *ta = tl + 0x748;
    const uint8_t *head = *(const uint8_t *const *)ta;
    printf("P tastat n=%u head=%d", ta[0x10], head ? (int)((head - (ta + 0x20)) / 0x1f0) : -1);
    printf(" x="); hex(ta + 0x18, 4); printf(" st="); hex(ta + 0x2718, 4); printf("\n");
    for (int i = 0; i < 20; i++) {
        const uint8_t *t = ta + 0x20 + i * 0x1f0;
        unsigned st = rd<uint32_t>(t, 0xd4);
        const uint8_t *nx = rd<const uint8_t *>(t, 400);
        if (!st && !nx && head != t) continue;
        {   // each panel's CommonTrackingCfg as its fingers hold it (+0x1a0), once per panel
            static const uint8_t *seen[2];
            const uint8_t *ct = rd<const uint8_t *>(t, 0x1a0);
            if (ct && seen[0] != ta && seen[1] != ta) {
                seen[seen[0] ? 1 : 0] = ta;
                printf("P tfctc ta=%p ", (const void *)ta); hex(ct, 0x78); printf("\n");
            }
        }
        printf("P tf %d st=%u next=%d pos=%.6f,%.6f rep=%.6f,%.6f id=%u h=", i, st, nx ? (int)((nx - (ta + 0x20)) / 0x1f0) : -1,
               (double)rd<float>(t, 0), (double)rd<float>(t, 4), (double)rd<float>(t, 8), (double)rd<float>(t, 0xc), rd<uint16_t>(t, 0x158));
        // all of it but the deque (+0x110..+0x140) and the pointers (+0x190.., +0x198 index, +0x1a0.., +0x1e8)
        hex(t, 0x110); printf("|"); hex(t + 0x140, 0x50); printf("|"); hex(t + 0x1b0, 0x38); printf("\n");
    }
    const uint8_t *rep = *(const uint8_t *const *)out;
    printf("P osrep n=%u ", rep[0]);
    for (unsigned i = 0; i < rep[0] && i < 10; i++) {
        const uint8_t *e = rep + 4 + i * 0x18;
        printf(" [x=%.6f y=%.6f w=%.6f h=%.6f id=%u o=%u f=%u l=%u]", (double)rd<float>(e, 0), (double)rd<float>(e, 4),
               (double)rd<float>(e, 8), (double)rd<float>(e, 0xc), rd<uint16_t>(e, 0x10), rd<uint16_t>(e, 0x12), e[0x14], e[0x15]);
    }
    printf("\n");
}

// The matching and preparation inputs TrackLib builds for each frame.
#define SYM_RUNMATCH "_ZN12MatchFingers11RunMatchingEPK18TracklibMatchingInP14TuProcessInput"
#define SYM_PREPTRACK "_ZN12MatchFingers15PrepareTrackingEfP25TracklibPrepareTrackingIn"
EXPORT void probe_runmatch(void *mf, uint8_t *mi, void *in) __asm__(SYM_RUNMATCH);
EXPORT void probe_runmatch(void *mf, uint8_t *mi, void *in) {
    static auto real = next<void (*)(void *, uint8_t *, void *)>(SYM_RUNMATCH);
    static int on = getenv("HEATPROBE_TRACKLIB") != nullptr;
    if (on) { printf("P mi "); hex(mi, 8); printf(" "); hex(mi + 0x20, 0x10); printf(" dt=%.9g\n", (double)rd<float>(mi, 0x24)); }
    real(mf, mi, in);
}
EXPORT void probe_preptrack(void *mf, float dt, uint8_t *pt) __asm__(SYM_PREPTRACK);
EXPORT void probe_preptrack(void *mf, float dt, uint8_t *pt) {
    static auto real = next<void (*)(void *, float, uint8_t *)>(SYM_PREPTRACK);
    static int on = getenv("HEATPROBE_TRACKLIB") != nullptr;
    if (on) { printf("P pt dt=%.9g ", (double)dt); hex(pt + 0x10, 0x10); printf("\n"); }
    real(mf, dt, pt);
}
