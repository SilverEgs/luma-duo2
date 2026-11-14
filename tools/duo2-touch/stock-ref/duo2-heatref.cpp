// duo2-heatref: run a duo2-heatcap recording through Microsoft's own heat
// processor (libsurfacetouch_c1.so from the user's stock vendor image) and
// print the contacts it reports -- the reference duo2-touch is measured
// against (docs/touch.md, Debugging).
//
// An Android (bionic) program: run it with the stock linker64 and runtime
// libraries (scripts/duo2-touch-bundle.sh), with libstatsshim.so preloaded so
// the library's telemetry finds no stats service instead of waiting for one.
//
// The library is initialised as vendor.surface.touchpen@1.3-service does it
// (docs/touch.md), and the reports are fed at their recorded pace, since the
// library keeps its own clock.
//
// Usage: duo2-heatref --lib LIB --desc DESCRIPTOR.bin [--state DIR] [--fast] RECORDING

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <set>
#include <vector>

namespace {

struct EosHeatDevice {
    uint8_t region;
    uint8_t pad[3];
    uint32_t kind;
    const void *collection;
};

struct __attribute__((packed)) TouchReport {
    uint16_t unknown0;
    uint16_t contact_id;
    uint8_t state;
    int32_t x, y;
    int32_t width, height;
    uint16_t orientation;
    uint8_t rest[8];
};
static_assert(sizeof(TouchReport) == 31, "TouchReport layout");

enum ProcessorSlot {
    kInitPenTouchMw = 2, kProcessIncomingReport = 3, kPostDispatchProcessing = 4,
    kOnDeviceAttached = 5, kOnStitchingModeChange = 9, kOnDisplayChange = 10,
};
struct Processor { void **vtable; };
template <typename Fn> Fn slot(Processor *p, int index) { return reinterpret_cast<Fn>(p->vtable[index]); }

double g_now;   // the recording time of the report being processed

void svc_noop(void *) {}
void svc_set_angle_sensitivity(void *, float) {}
bool svc_send_message_to_touch_fw(void *, uint32_t length, const uint8_t *data, const EosHeatDevice *) {
    Dl_info di = {};
    void *ra = __builtin_return_address(0);
    dladdr(ra, &di);
    fprintf(stderr, "to touch fw: %u bytes, report 0x%02x at %.6f from", length, length ? data[0] : 0, g_now);
    void **fp = (void **)__builtin_frame_address(0);
    for (int lvl = 0; lvl < 6 && fp; lvl++) {
        ra = fp[1];
        if (!dladdr(ra, &di)) break;
        fprintf(stderr, " %s+0x%lx", di.dli_sname ? di.dli_sname : "?",
                (long)((char *)ra - (char *)(di.dli_sname ? di.dli_saddr : di.dli_fbase)));
        fp = (void **)fp[0];
    }
    fprintf(stderr, ":");
    for (uint32_t i = 0; i < length; i++) fprintf(stderr, "%02x", data[i]);
    fprintf(stderr, "\n");
    return false;
}
void svc_send_pen_reports(void *, const void *, uint32_t) {}
void svc_send_touch_reports(void *, const TouchReport *r, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        printf("%.6f contact id=%u state=%u x=%d y=%d w=%d h=%d o=%u\n", g_now, r[i].contact_id, r[i].state,
               r[i].x, r[i].y, r[i].width, r[i].height, r[i].orientation);
}
long svc_unused(void *) { return 0; }
void *g_service_vtable[16] = {
    (void *)svc_noop, (void *)svc_noop, (void *)svc_set_angle_sensitivity,
    (void *)svc_send_message_to_touch_fw, (void *)svc_send_pen_reports, (void *)svc_send_touch_reports,
    (void *)svc_unused, (void *)svc_unused, (void *)svc_unused, (void *)svc_unused,
    (void *)svc_unused, (void *)svc_unused, (void *)svc_unused, (void *)svc_unused,
    (void *)svc_unused, (void *)svc_unused,
};
struct Service { void **vtable; } g_service = { g_service_vtable };

// The report IDs inside each of the two heat-map application collections.
struct Collections { std::set<int> panel[2]; };
Collections parse_descriptor(const uint8_t *d, int n) {
    Collections c;
    int usage_page = 0, depth = 0, heat = -1, heat_seen = 0, last_usage = -1;
    for (int i = 0; i < n;) {
        uint8_t b = d[i];
        int size = b & 3; if (size == 3) size = 4;
        uint32_t v = 0;
        for (int k = 0; k < size && i + 1 + k < n; k++) v |= (uint32_t)d[i + 1 + k] << (8 * k);
        switch (b & 0xfc) {
        case 0x04: usage_page = v; break;
        case 0x08: last_usage = v; break;
        case 0xa0: if (depth == 0 && usage_page == 0x0d && last_usage == 0x0f) heat = heat_seen++; depth++; break;
        case 0xc0: if (--depth == 0) heat = -1; break;
        case 0x84: if (heat >= 0 && heat < 2) c.panel[heat].insert(v); break;
        }
        i += 1 + size;
    }
    return c;
}

std::vector<uint8_t> slurp(const char *path) {
    std::vector<uint8_t> d;
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);
    return d;
}

double mono() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

}  // namespace

int main(int argc, char **argv) {
    const char *lib = "/vendor/lib64/c1/libsurfacetouch_c1.so", *desc_path = nullptr, *rec = nullptr;
    const char *state = "/tmp/duo2-heatref";
    bool fast = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lib") && i + 1 < argc) lib = argv[++i];
        else if (!strcmp(argv[i], "--desc") && i + 1 < argc) desc_path = argv[++i];
        else if (!strcmp(argv[i], "--state") && i + 1 < argc) state = argv[++i];
        else if (!strcmp(argv[i], "--fast")) fast = true;
        else rec = argv[i];
    }
    if (!desc_path || !rec) { fprintf(stderr, "usage: %s --lib LIB --desc DESC [--state DIR] [--fast] REC\n", argv[0]); return 2; }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    mkdir(state, 0700);
    std::vector<uint8_t> desc = slurp(desc_path);
    static Collections col = parse_descriptor(desc.data(), (int)desc.size());
    if (!col.panel[0].count(0x0a) || !col.panel[1].count(0x3c)) { fprintf(stderr, "unexpected descriptor\n"); return 1; }

    void *h = dlopen(lib, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    auto create = reinterpret_cast<Processor *(*)()>(dlsym(h, "CreateHeatProcessorInstance"));
    Processor *proc = create ? create() : nullptr;
    if (!proc) { fprintf(stderr, "no heat processor\n"); return 1; }
    static EosHeatDevice dev_a = { 1, {}, 2, &col.panel[0] };
    static EosHeatDevice dev_b = { 2, {}, 1, &col.panel[1] };
    slot<void (*)(Processor *, const EosHeatDevice *, const char *, Service *, uint16_t)>(proc, kInitPenTouchMw)(
        proc, &dev_a, state, &g_service, 0x0c1d);
    slot<void (*)(Processor *, const EosHeatDevice *)>(proc, kOnDeviceAttached)(proc, &dev_b);
    slot<void (*)(Processor *, bool)>(proc, kOnStitchingModeChange)(proc, false);
    slot<void (*)(Processor *, const EosHeatDevice *, bool)>(proc, kOnDisplayChange)(proc, &dev_a, true);
    slot<void (*)(Processor *, const EosHeatDevice *, bool)>(proc, kOnDisplayChange)(proc, &dev_b, true);
    fprintf(stderr, "heat processor ready\n");

    std::vector<uint8_t> d = slurp(rec);
    size_t o = 0;
    double t0 = -1, w0 = mono();
    unsigned long n = 0;
    while (o + 12 <= d.size()) {
        uint64_t t; uint32_t len;
        memcpy(&t, &d[o], 8); memcpy(&len, &d[o + 8], 4);
        if (o + 12 + len > d.size()) break;
        const uint8_t *r = &d[o + 12];
        o += 12 + len;
        if (!len) continue;
        double tr = t / 1e9;
        if (t0 < 0) t0 = tr;
        if (!fast) {
            double wait = (tr - t0) - (mono() - w0);
            if (wait > 0) usleep((useconds_t)(wait * 1e6));
        }
        const EosHeatDevice *dev = nullptr;
        if (r[0] == 0xfe && len > 1) dev = r[1] == 1 ? &dev_a : r[1] == 2 ? &dev_b : nullptr;
        else if (col.panel[0].count(r[0])) dev = &dev_a;
        else if (col.panel[1].count(r[0])) dev = &dev_b;
        if (!dev) continue;
        g_now = tr;
        slot<void (*)(Processor *, const EosHeatDevice *, uint32_t, const uint8_t *)>(proc, kProcessIncomingReport)(
            proc, dev, len, r);
        if (r[0] != 0xfe) slot<void (*)(Processor *, const EosHeatDevice *)>(proc, kPostDispatchProcessing)(proc, dev);
        n++;
    }
    fprintf(stderr, "%lu reports\n", n);
    fflush(stdout);
    _exit(0);   // the library's threads and statics need not wind down
}
