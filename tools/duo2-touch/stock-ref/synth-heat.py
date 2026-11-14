"""Build a synthetic heat recording that exercises the tracker paths a plain
single-finger capture does not: several fingers crossing, taps, merging
fingers (multi-peak blobs), a palm with a finger near it, edges and corners,
a frame-time gap, noisy frames, weak touches, fast swipes.

usage: synth-heat.py TEMPLATE.bin OUT.bin  (TEMPLATE: a duo2-heatcap recording)
Frames reuse the template's real idle frames as background (so the noise is
real) and its headers; only the image, the scan counter and (for noisy
segments) the frequency-noise levels change."""
import itertools, math, struct, sys

src, out = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
recs = []
o = 0
while o < len(d):
    ns, ln = struct.unpack_from('<QI', d, o)
    recs.append((ns, bytearray(d[o + 12:o + 12 + ln])))
    o += 12 + ln

COLS, ROWS = 30, 21
K, BASE = 0.0022203543, 0.6

def sections(r):
    end = struct.unpack_from('<I', r, 3)[0] + 3
    p = 10
    res = {}
    while p + 6 <= min(end, len(r)):
        sz, ty = struct.unpack_from('<IH', r, p)
        if sz < 6: break
        res.setdefault(ty, p)
        if ty == 0xff00:
            q = 7
            while q + 4 <= sz:
                st, ss = r[p + q], struct.unpack_from('<H', r, p + q + 2)[0]
                if st == 0 and ss == 0: break
                res[('sub', st)] = p + q + 4
                q += 4 + ss
        p += sz
    return res

def image(r):
    s = sections(r)[0x100]
    return r[s + 16:s + 16 + COLS * ROWS]

# Idle frames per panel: no junction stronger than the weakest touch.
idle = {0x3c: [], 0x0a: []}
template = {}
for ns, r in recs:
    if r[0] in idle and len(r) == 1000 and 0x100 in sections(r) and ('sub', 4) in sections(r):
        template.setdefault(r[0], r)
        if min(image(r)) > 165: idle[r[0]].append(bytes(image(r)))
print('idle frames', {k: len(v) for k, v in idle.items()})

def compose(bg, shapes):
    img = bytearray(bg)
    for y in range(ROWS):
        for x in range(COLS):
            m = 0.0
            for (cx, cy, sx, sy, a) in shapes:
                m += a * math.exp(-((x - cx) ** 2 / (2 * sx * sx) + (y - cy) ** 2 / (2 * sy * sy)))
            if m > 0.004:
                b = int((1.0 - BASE - m) / K + 0.5)
                b = max(0, min(255, b))
                if b < img[y * COLS + x]: img[y * COLS + x] = b
    return bytes(img)

frames = []   # (panel, shapes, ts_step, noise)

def seg(n, fn, panel=0x3c, noise=None):
    for t in range(n): frames.append((panel, fn(t, n), 1, noise))

def lerp(a, b, t, n): return a + (b - a) * t / max(1, n - 1)
F = lambda x, y, a=0.2, s=1.1: (x, y, s, s, a)
idle_seg = lambda n, panel=0x3c: seg(n, lambda t, n: [], panel)

idle_seg(12)
# Two fingers crossing.
seg(60, lambda t, n: [F(lerp(4, 25, t, n), lerp(4, 16, t, n)), F(lerp(25, 4, t, n), lerp(4, 16, t, n))])
idle_seg(12)
# Taps: short touches in the open, a longer one, one at the edge, a weak one.
for (x, y, k, a) in [(15, 10, 4, 0.2), (8, 12, 6, 0.18), (22, 6, 3, 0.22), (0.4, 10, 4, 0.2), (12, 15, 5, 0.12)]:
    seg(k, lambda t, n, x=x, y=y, a=a: [F(x, y, a)])
    idle_seg(14)
# Two fingers closing in until they read as one blob with two peaks, then apart.
seg(25, lambda t, n: [F(lerp(9, 12.6, t, n), 10), F(lerp(18, 14.4, t, n), 10.5)])
seg(30, lambda t, n: [F(12.6 + 0.1 * math.sin(t / 3), 10), F(14.4, 10.5)])
seg(20, lambda t, n: [F(lerp(12.6, 8, t, n), 10), F(lerp(14.4, 19, t, n), 10.5)])
idle_seg(12)
# A palm, a finger touching down beside it, the palm leaving.
palm = lambda t: (20 + 0.05 * t, 12, 3.0, 2.4, 0.17)
seg(15, lambda t, n: [palm(t)])
seg(30, lambda t, n: [palm(t + 15), F(lerp(11, 13, t, n), 7)])
seg(25, lambda t, n: [F(13, 7 + 0.05 * t)])
idle_seg(8)
seg(20, lambda t, n: [F(21, 12)])           # a finger where the palm was
idle_seg(12)
# Four fingers, lifting one by one.
pts = [(5, 5), (12, 14), (20, 6), (26, 15)]
seg(40, lambda t, n: [F(x + 0.08 * t, y - 0.05 * t) for (x, y) in pts])
seg(15, lambda t, n: [F(x + 3.2, y - 2) for (x, y) in pts[1:]])
seg(15, lambda t, n: [F(x + 3.2, y - 2) for (x, y) in pts[2:]])
seg(15, lambda t, n: [F(pts[3][0] + 3.2, pts[3][1] - 2)])
idle_seg(12)
# Along the edge from the corner, then in from the far edge.
seg(50, lambda t, n: [F(lerp(0.2, 28, t, n), 0.3)])
seg(30, lambda t, n: [F(29.6, lerp(20.5, 4, t, n))])
idle_seg(12)
# A held finger, then the scan counter jumping.
seg(20, lambda t, n: [F(10, 10)])
frames.append((0x3c, [F(10.2, 10)], 9, None))
seg(20, lambda t, n: [F(10.4, 10)])
idle_seg(12)
# Noisy frames with two fingers.
seg(40, lambda t, n: [F(7, 6 + 0.1 * t), F(20, 14 - 0.1 * t)], noise=420)
idle_seg(12)
# Weak, near-threshold touches; a fast swipe.
seg(30, lambda t, n: [F(15, 10, 0.075, 1.0), F(25, 5, 0.09, 0.9)])
idle_seg(12)
seg(10, lambda t, n: [F(lerp(2, 28, t, n), lerp(18, 3, t, n))])
idle_seg(12)
# A thin, long contact (a thumb's side) and a small, sharp one.
seg(30, lambda t, n: [(14, 10, 3.2, 0.8, 0.16), (25, 16, 0.6, 0.6, 0.25)])
idle_seg(12)
# Fingers side by side at several spacings, sizes and strengths (blobs with
# one or two peaks), and three in a row.
for sep, amp, sg in itertools.product([1.6, 2.0, 2.4, 2.8, 3.2], [0.16, 0.22], [0.9, 1.2]):
    seg(25, lambda t, n, sep=sep, amp=amp, sg=sg: [F(14, 10, amp, sg), F(14 + sep, 10 + 0.4 * sep, amp, sg)])
    idle_seg(8)
for sep in [1.8, 2.4, 3.0]:
    seg(25, lambda t, n, sep=sep: [F(8, 6, 0.2), F(8 + sep, 6, 0.2), F(8 + 2 * sep, 6, 0.2)])
    idle_seg(8)
# The right panel: two fingers and a tap.
seg(40, lambda t, n: [F(lerp(6, 20, t, n), 8), F(22, lerp(16, 6, t, n))], 0x0a)
idle_seg(10, 0x0a)
seg(4, lambda t, n: [F(15, 10)], 0x0a)
idle_seg(14, 0x0a)

ts = {0x3c: 1000, 0x0a: 2000}
seq = {0x3c: 0, 0x0a: 0}
ns = 10 ** 12
outb = bytearray()
# The template's leading non-image records (configuration) first.
for n0, r in recs:
    if len(r) == 1000: break
    outb += struct.pack('<QI', n0, len(r)) + r
for i, (panel, shapes, step, noise) in enumerate(frames):
    r = bytearray(template[panel])
    sc = sections(r)
    bgs = idle[panel]
    img = compose(bgs[i % len(bgs)], shapes)
    s = sc[0x100]
    r[s + 16:s + 16 + COLS * ROWS] = img
    ts[panel] = (ts[panel] + step) & 0xffff
    struct.pack_into('<H', r, sc[('sub', 0)] + 2, ts[panel])
    seq[panel] = (seq[panel] + 1) & 0xffff
    struct.pack_into('<H', r, 1, seq[panel])
    q = sc[('sub', 4)]
    if noise is not None: struct.pack_into('<I', r, q + 4, noise)
    ns += 10_000_000 if step == 1 else 10_000_000 * step
    outb += struct.pack('<QI', ns, len(r)) + r
open(out, 'wb').write(outb)
print('frames', len(frames), 'bytes', len(outb))
