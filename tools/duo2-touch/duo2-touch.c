// duo2-touch: the Surface Duo 2's touchscreens, natively.
//
// The digitizer (HID 045E:0C1D on spi0.0) does no contact detection of its
// own: it streams raw capacitive heat frames, one report per panel per scan
// (0x3C left, 0x0A right). On Android, Microsoft's heat processor turns them
// into contacts. This daemon does the same in plain C (heat.c), with the
// tuning read from the stock library's PS_CFG_DATA_Zeta blob, and presents
// each panel as its own Linux multitouch screen through uinput.
//
// Usage: duo2-touch [--lib LIB] [--hidraw /dev/hidrawN] [--test-input] [-v]
//        duo2-touch [--lib LIB] --replay FILE [--probe]  (a duo2-heatcap recording;
//                   --probe prints each stage as stock-ref/heatprobe.cpp does)
//
// --align FILE: the unit's two-panel alignment record (default
// /var/lib/luma-duo2/touch/TwoPanelAlignment.bin, copied from sfpd).

#include "heat.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_LIB "/usr/lib/luma-duo2/touch/lib64/libsurfacetouch_c1.so"
#define DEFAULT_ALIGN "/var/lib/luma-duo2/touch/TwoPanelAlignment.bin"
#define SPI_READY "/sys/bus/spi/devices/spi0.0/ready"
#define REPORT_LEFT 0x3c
#define REPORT_RIGHT 0x0a
// The digitizer stops sending frames when nothing touches a panel; a panel
// silent this long has been released.
#define LIFT_TIMEOUT_MS 40
#define TEST_SOCKET "/run/duo2-touch-test.sock"

static volatile sig_atomic_t g_stop;
static int g_verbose;

static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void on_signal(int s) { (void)s; g_stop = 1; }

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---------------------------------------------------------------------------
// One touchscreen per panel. Each has its own product id so GNOME's per-device
// touchscreen settings (keyed by vendor:product) can tie it to its own output.

struct panel {
    const char *name;
    uint16_t product;
    int fd;
    struct heat_tracker tracker;
    struct heat_detector detector;
    struct heat_tracks tracks;        // TouchDetection's tracker (tracks.c)
    int slot_id[HEAT_MAX_TRACKS];     // tracking id per slot, -1 free
    long long last_frame;
    int touching;
};

static struct panel g_panels[2] = {
    { .name = "Surface Duo 2 Touchscreen Left", .product = 0x0c1d, .fd = -1 },
    { .name = "Surface Duo 2 Touchscreen Right", .product = 0x0c1e, .fd = -1 },
};

static void emit(struct panel *p, int type, int code, int value) {
    if (p->fd < 0) return;
    struct input_event ev = { 0 };
    ev.type = (uint16_t)type; ev.code = (uint16_t)code; ev.value = value;
    if (write(p->fd, &ev, sizeof ev) != (ssize_t)sizeof ev) log_msg("uinput write: %s", strerror(errno));
}

static void abs_setup(int fd, int code, int min, int max, int res) {
    struct uinput_abs_setup a = { 0 };
    a.code = (uint16_t)code; a.absinfo.minimum = min; a.absinfo.maximum = max; a.absinfo.resolution = res;
    if (ioctl(fd, UI_ABS_SETUP, &a) < 0) log_msg("UI_ABS_SETUP %d: %s", code, strerror(errno));
}

static int panel_create(struct panel *p, const struct heat_cfg *c) {
    p->fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (p->fd < 0) { log_msg("open /dev/uinput: %s", strerror(errno)); return -1; }
    for (int s = 0; s < HEAT_MAX_TRACKS; s++) p->slot_id[s] = -1;
    ioctl(p->fd, UI_SET_EVBIT, EV_KEY);
    ioctl(p->fd, UI_SET_EVBIT, EV_ABS);
    ioctl(p->fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(p->fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    int w = c->logical_max[0], h = c->logical_max[1];
    // Logical units per mm, as the stock report scales them.
    int rx = (int)((float)w / c->size_mm[0] + 0.5f), ry = (int)((float)h / c->size_mm[1] + 0.5f);
    abs_setup(p->fd, ABS_X, 0, w, rx);
    abs_setup(p->fd, ABS_Y, 0, h, ry);
    abs_setup(p->fd, ABS_MT_SLOT, 0, HEAT_MAX_TRACKS - 1, 0);
    abs_setup(p->fd, ABS_MT_TRACKING_ID, 0, 0xffff, 0);
    abs_setup(p->fd, ABS_MT_POSITION_X, 0, w, rx);
    abs_setup(p->fd, ABS_MT_POSITION_Y, 0, h, ry);
    abs_setup(p->fd, ABS_MT_TOUCH_MAJOR, 0, h, ry);
    abs_setup(p->fd, ABS_MT_TOUCH_MINOR, 0, h, ry);
    struct uinput_setup s = { 0 };
    s.id.bustype = BUS_VIRTUAL; s.id.vendor = 0x045e; s.id.product = p->product; s.id.version = 1;
    snprintf(s.name, sizeof s.name, "%s", p->name);
    if (ioctl(p->fd, UI_DEV_SETUP, &s) < 0 || ioctl(p->fd, UI_DEV_CREATE) < 0) {
        log_msg("uinput create %s: %s", p->name, strerror(errno));
        return -1;
    }
    return 0;
}

static void panel_report(struct panel *p, const struct heat_contact *k, int n) {
    if (n == 0 && !p->touching) return;
    int seen[HEAT_MAX_TRACKS] = { 0 };
    for (int i = 0; i < n; i++) {
        int s = -1;
        for (int j = 0; j < HEAT_MAX_TRACKS; j++) if (p->slot_id[j] == k[i].id) { s = j; break; }
        if (s < 0) {
            for (int j = 0; j < HEAT_MAX_TRACKS; j++) if (p->slot_id[j] < 0) { s = j; break; }
            if (s < 0) continue;
            p->slot_id[s] = k[i].id;
            emit(p, EV_ABS, ABS_MT_SLOT, s);
            emit(p, EV_ABS, ABS_MT_TRACKING_ID, k[i].id);
        } else {
            emit(p, EV_ABS, ABS_MT_SLOT, s);
        }
        seen[s] = 1;
        emit(p, EV_ABS, ABS_MT_POSITION_X, k[i].x);
        emit(p, EV_ABS, ABS_MT_POSITION_Y, k[i].y);
        emit(p, EV_ABS, ABS_MT_TOUCH_MAJOR, k[i].major);
        emit(p, EV_ABS, ABS_MT_TOUCH_MINOR, k[i].minor);
        if (g_verbose) log_msg("%s: id %d at %d,%d", p->name, k[i].id, k[i].x, k[i].y);
    }
    for (int j = 0; j < HEAT_MAX_TRACKS; j++) {
        if (p->slot_id[j] < 0 || seen[j]) continue;
        emit(p, EV_ABS, ABS_MT_SLOT, j);
        emit(p, EV_ABS, ABS_MT_TRACKING_ID, -1);
        if (g_verbose) log_msg("%s: id %d lifted", p->name, p->slot_id[j]);
        p->slot_id[j] = -1;
    }
    int touching = n > 0;
    if (touching != p->touching) emit(p, EV_KEY, BTN_TOUCH, touching);
    if (n > 0) {
        emit(p, EV_ABS, ABS_X, k[0].x);
        emit(p, EV_ABS, ABS_Y, k[0].y);
    }
    p->touching = touching;
    emit(p, EV_SYN, SYN_REPORT, 0);
}

static void panel_release(struct panel *p) {
    heat_track_reset(&p->tracker);
    if (p->touching) panel_report(p, NULL, 0);
}

// ---------------------------------------------------------------------------

static int find_hidraw(char *path, size_t len) {
    DIR *d = opendir("/sys/class/hidraw");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) && found < 0) {
        if (e->d_name[0] == '.') continue;
        char p[512], buf[1024] = { 0 };
        snprintf(p, sizeof p, "/sys/class/hidraw/%s/device/uevent", e->d_name);
        FILE *f = fopen(p, "re");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        if (strstr(buf, ":0000045E:00000C1D")) { snprintf(path, len, "/dev/%s", e->d_name); found = 0; }
    }
    closedir(d);
    return found;
}

static int wait_ready(int timeout_ms) {
    for (int waited = 0; waited <= timeout_ms; waited += 50) {
        char s[16] = { 0 };
        int fd = open(SPI_READY, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t n = read(fd, s, sizeof s - 1);
            close(fd);
            if (n > 0 && !strncmp(s, "ready", 5)) return 0;
        }
        usleep(50 * 1000);
    }
    return -1;
}

// Bring-up only (--test-input): a datagram socket taking heat reports in the
// digitizer's own format, processed exactly as if hidraw had delivered them,
// so the whole path to the compositor can be exercised without a finger.
static int test_socket(void) {
    int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) { log_msg("test socket: %s", strerror(errno)); return -1; }
    struct sockaddr_un a = { 0 };
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", TEST_SOCKET);
    unlink(TEST_SOCKET);
    mode_t old = umask(077);
    int rc = bind(s, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (rc < 0) { log_msg("bind %s: %s", TEST_SOCKET, strerror(errno)); close(s); return -1; }
    log_msg("test input on %s", TEST_SOCKET);
    return s;
}

static struct panel *panel_for(uint8_t id) {
    return id == REPORT_LEFT ? &g_panels[0] : id == REPORT_RIGHT ? &g_panels[1] : NULL;
}

static FILE *g_probe;    // --probe: per-stage output in stock-ref/heatprobe.cpp's format

static int process(const struct heat_cfg *c, struct panel *p, const uint8_t *r, size_t n,
                   struct heat_contact *k, int maxk) {
    static struct heat_frame f;
    if (heat_parse_report(c, r, n, &f) < 0) return -1;
    struct heat_detector *d = &p->detector;
    int nr = heat_tracks_frame(c, &p->tracks, d, &f);
    if (g_probe) {
        fprintf(g_probe, "P frame panel=%s seq=%u ts=%u\n", p == &g_panels[0] ? "left" : "right", f.seq, f.ts);
        heat_dump_blobs(g_probe, d, &f);
        heat_dump_tracks(g_probe, &p->tracks);
    }
    struct heat_finger fg[HEAT_REPORTS];
    const struct heat_report *fr[HEAT_REPORTS];
    int nf = 0;
    for (int i = 0; i < nr; i++) {
        const struct heat_report *rep = &p->tracks.out[i];
        if (heat_to_mm(c, rep->x, rep->y, rep->radius, rep->valid, &fg[nf])) fr[nf++] = rep;
    }
    if (g_probe) {
        if (nr > 0) fprintf(g_probe, "P fingers n=%d\n", nf);
        for (int i = 0; i < nf; i++)
            fprintf(g_probe, "P finger %d x=%.6f y=%.6f ax=%.6f ay=%.6f r=%.6f type=%u valid=%u sep=%u td=%u nsr=%u\n",
                    i, (double)fg[i].x, (double)fg[i].y, (double)fg[i].ax, (double)fg[i].ay, (double)fg[i].radius,
                    fr[i]->type, fr[i]->valid, fr[i]->split, fr[i]->track, fr[i]->count);
    }
    // Until TrackLib and FillOsReport are reproduced: the valid records go to
    // the simple contact tracker (the stock stages after this drop the rest).
    int nv = 0;
    for (int i = 0; i < nf; i++)
        if (fr[i]->valid) fg[nv++] = fg[i];
    return heat_track(c, &p->tracker, fg, nv, k, maxk);
}

// Replay a duo2-heatcap recording: records of [u64 CLOCK_MONOTONIC ns][u32 len][bytes].
static int replay(const struct heat_cfg *c, const char *file) {
    FILE *f = fopen(file, "rb");
    if (!f) { log_msg("%s: %s", file, strerror(errno)); return 1; }
    static uint8_t buf[65536];
    uint8_t hdr[12];
    long long last[2] = { 0, 0 };
    while (fread(hdr, 1, 12, f) == 12) {
        unsigned long long t = 0;
        for (int i = 7; i >= 0; i--) t = t << 8 | hdr[i];
        uint32_t n = (uint32_t)hdr[8] | (uint32_t)hdr[9] << 8 | (uint32_t)hdr[10] << 16 | (uint32_t)hdr[11] << 24;
        if (n > sizeof buf || fread(buf, 1, n, f) != n) break;
        struct panel *p = n ? panel_for(buf[0]) : NULL;
        if (!p) continue;
        int pi = (int)(p - g_panels);
        long long ms = (long long)(t / 1000000ULL);
        if (last[pi] && ms - last[pi] > LIFT_TIMEOUT_MS) heat_track_reset(&p->tracker);
        last[pi] = ms;
        struct heat_contact k[HEAT_MAX_TRACKS];
        int nk = process(c, p, buf, n, k, HEAT_MAX_TRACKS);
        for (int i = 0; i < nk; i++)
            printf("%.6f %s id=%d x=%d y=%d major=%d\n", (double)t / 1e9, pi ? "right" : "left",
                   k[i].id, k[i].x, k[i].y, k[i].major);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    const char *lib = DEFAULT_LIB, *align = DEFAULT_ALIGN, *hidraw = NULL, *replay_file = NULL;
    int test_input = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lib") && i + 1 < argc) lib = argv[++i];
        else if (!strcmp(argv[i], "--hidraw") && i + 1 < argc) hidraw = argv[++i];
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay_file = argv[++i];
        else if (!strcmp(argv[i], "--align") && i + 1 < argc) align = argv[++i];
        else if (!strcmp(argv[i], "--probe")) g_probe = stdout;
        else if (!strcmp(argv[i], "--test-input")) test_input = 1;
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else { log_msg("usage: %s [--lib LIB] [--align FILE] [--hidraw DEV] [--replay FILE [--probe]] [--test-input] [-v]", argv[0]); return 2; }
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    struct heat_cfg cfg;
    char err[256];
    if (heat_cfg_load(&cfg, lib, err, sizeof err) < 0) { log_msg("tuning: %s", err); return 1; }
    if (heat_cfg_load_alignment(&cfg, align, err, sizeof err) < 0) log_msg("panel alignment: %s; none applied", err);
    else log_msg("panel alignment: %.3f mm", (double)cfg.align_mm);
    log_msg("tuning from %s: %d x %d antennas, %.2f x %.2f mm (pitch %.4f mm), threshold %.3f..%.3f%s",
         lib, cfg.cols, cfg.rows, cfg.size_mm[0], cfg.size_mm[1], cfg.pitch_mm[0], cfg.thr_min, cfg.thr_max,
         cfg.stretch_on ? ", edge stretching" : "");
    if (replay_file) return replay(&cfg, replay_file);

    char found[300];
    if (!hidraw) {
        if (find_hidraw(found, sizeof found) < 0) { log_msg("no digitizer (045E:0C1D) hidraw device"); return 1; }
        hidraw = found;
    }
    int fd = open(hidraw, O_RDWR | O_CLOEXEC);
    if (fd < 0) { log_msg("open %s: %s", hidraw, strerror(errno)); return 1; }
    struct hidraw_devinfo info = { 0 };
    if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0 || (uint16_t)info.vendor != 0x045e || (uint16_t)info.product != 0x0c1d) {
        log_msg("%s is not the Duo 2 digitizer (045e:0c1d)", hidraw);
        return 1;
    }
    // Opening hidraw wakes the digitizer; spi-hid says when it takes writes.
    if (wait_ready(10000) < 0) { log_msg("%s never became ready", SPI_READY); return 1; }
    // Heat mode (feature report 5, vendor usage 0xC8 = 1) and both panels on
    // (output report 0x38: bit 0 left, bit 1 right), as on Android.
    const uint8_t mode[2] = { 0x05, 0x01 }, on[2] = { 0x38, 0x03 };
    if (ioctl(fd, HIDIOCSFEATURE(sizeof mode), mode) < 0) log_msg("heat mode: %s", strerror(errno));
    if (write(fd, on, sizeof on) != (ssize_t)sizeof on) log_msg("display state: %s", strerror(errno));
    for (int i = 0; i < 2; i++) if (panel_create(&g_panels[i], &cfg) < 0) return 1;

    struct sigaction sa = { 0 };
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    log_msg("running on %s", hidraw);

    int test = test_input ? test_socket() : -1;
    static uint8_t buf[16384];
    unsigned long frames = 0, contacts = 0;
    long long stats = now_ms();
    while (!g_stop) {
        struct pollfd pfds[2] = { { fd, POLLIN, 0 }, { test, POLLIN, 0 } };
        int timeout = 1000;
        for (int i = 0; i < 2; i++) if (g_panels[i].touching) timeout = LIFT_TIMEOUT_MS / 2;
        int pr = poll(pfds, test >= 0 ? 2 : 1, timeout);
        struct pollfd pfd = pfds[0];
        int from = fd;
        if (pr > 0 && test >= 0 && (pfds[1].revents & POLLIN)) { pfd = pfds[1]; from = test; }
        long long t = now_ms();
        for (int i = 0; i < 2; i++)
            if (g_panels[i].touching && t - g_panels[i].last_frame > LIFT_TIMEOUT_MS) panel_release(&g_panels[i]);
        if (t - stats >= 30000) {
            if (frames) log_msg("last 30 s: %lu heat frames, %lu contact reports", frames, contacts);
            frames = contacts = 0;
            stats = t;
        }
        if (pr <= 0 || !(pfd.revents & POLLIN)) {
            if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP))) { log_msg("hidraw: hung up"); break; }
            continue;
        }
        ssize_t n = read(from, buf, sizeof buf);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; log_msg("hidraw read: %s", n < 0 ? strerror(errno) : "EOF"); break; }
        struct panel *p = panel_for(buf[0]);
        if (!p) continue;
        p->last_frame = t;
        struct heat_contact k[HEAT_MAX_TRACKS];
        int nk = process(&cfg, p, buf, (size_t)n, k, HEAT_MAX_TRACKS);
        if (nk < 0) continue;
        frames++;
        contacts += (unsigned long)nk;
        panel_report(p, k, nk);
    }
    for (int i = 0; i < 2; i++) {
        panel_release(&g_panels[i]);
        if (g_panels[i].fd >= 0) { ioctl(g_panels[i].fd, UI_DEV_DESTROY); close(g_panels[i].fd); }
    }
    if (test >= 0) { close(test); unlink(TEST_SOCKET); }
    log_msg("stopping");
    return 0;
}
