"""Compare duo2-touch --probe output with the stock probe (heatprobe.cpp), frame
by frame per panel.
usage: cmpprobe.py STOCK OURS [prefix ...]  (prefixes of lines to compare, default 'P blob ')

Normalise both first: the first 8 bytes of a "P tli" record are left over
from the previous frame in the stock output, and "P fingers" carries
pointers:
  sed -E 's/(P tli .* h=)[0-9a-f]{16}/\10000000000000000/; s/^(P fingers n=[0-9]+).*/\1/'"""
import sys, re, collections
def frames(path):
    per = collections.OrderedDict(); cur = None
    for l in open(path, errors='replace'):
        if l.startswith('P frame'):
            key = l.split()[2]
            cur = []; per.setdefault(key, []).append(cur)
        elif cur is not None and l.startswith('P '):
            cur.append(l.rstrip('\n'))
    return per
st, ou = frames(sys.argv[1]), frames(sys.argv[2])
prefixes = sys.argv[3:] or ['P blob ']
print('stock panels', {k: len(v) for k, v in st.items()}, 'ours', {k: len(v) for k, v in ou.items()})
# pair panels by frame count
pairs = []
for ko, vo in ou.items():
    ks = min(st, key=lambda k: abs(len(st[k]) - len(vo)))
    pairs.append((ks, ko))
for ks, ko in pairs:
    a, b = st[ks], ou[ko]
    same = diff = 0; shown = 0
    fields = collections.Counter()
    for i, (fa, fb) in enumerate(zip(a, b)):
        la = [l for l in fa if any(l.startswith(p) for p in prefixes)]
        lb = [l for l in fb if any(l.startswith(p) for p in prefixes)]
        if la == lb: same += 1; continue
        diff += 1
        for x, y in zip(la, lb):
            for u, v in zip(x.split(), y.split()):
                if u != v: fields[u.split('=')[0]] += 1
        if len(la) != len(lb): fields['#lines'] += 1
        if shown < 6:
            shown += 1
            print(f'--- {ko} frame {i}'); print('  stock:', *la, sep='\n    '); print('  ours: ', *lb, sep='\n    ')
    print(f'{ko}: {same} same, {diff} differ; fields: {dict(fields)}')
