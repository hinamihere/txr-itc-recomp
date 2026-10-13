import re, sys
# usage: python capsum.py <log> [skip_first_n_draws=60]
# Summarises the logged capture frame pass by pass: consecutive draws with the same render state (rt0, surface, msaa, window scissor/offset) are merged.
path = sys.argv[1]
skip = int(sys.argv[2]) if len(sys.argv) > 2 else 60
rows = []
cur = None
nd = 0
wsc = None
for l in open(path, 'r', errors='replace'):
    if l.startswith('[gpu]   wscissor'):
        m = re.search(r'wscissor tl=(\w+) br=(\w+) screen tl=(\w+) br=(\w+) winoff=(\w+) modeCntl=(\w+) clip=(\w+)', l)
        if m: wsc = m.groups()
    elif l.startswith('[gpu] draw #'):
        nd += 1
        if nd <= skip:
            continue
        m = re.search(r'draw #(\d+) prim=(\d+) count=(\d+) \((\d+) idx\) vs=(\w+) ps=(\w+) rt0=(\w+) surf=(\d+x\d+) pitch=(\d+) msaa=(\d) depthctl=(\w+) blend0=(\w+) mask=(\w+) tex\(vs/ps\)=(\d+)/(\d+)', l)
        if not m: continue
        g = m.groups()
        key = (g[6], g[7], g[9], wsc[0] + '-' + wsc[1] if wsc else '?', wsc[4] if wsc else '?')
        rows.append(('D', key, g, wsc))
    elif l.startswith('[gpu] resolve #'):
        if nd <= skip: continue
        m = re.search(r'resolve #(\d+) src=(\d) rect=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\) dest=(\w+) pitch=(\d+) h=(\d+) fmt=(\d+) endian=(\d) swap=(\d) clear c=(\d) d=(\d) rt=(\w+)', l)
        if m: rows.append(('R', None, m.groups(), None))
out = []
for r in rows:
    if r[0] == 'D':
        g = r[2]
        if out and out[-1][0] == 'D' and out[-1][1] == r[1]:
            o = out[-1]; o[2] += 1; o[3].add((g[4][:6], g[5][:6], g[14])); o[4] += int(g[3]); o[6] = g[0]
            o[7].add(g[10])
        else:
            out.append(['D', r[1], 1, {(g[4][:6], g[5][:6], g[14])}, int(g[3]), g[0], g[0], {g[10]}])
    else:
        out.append(['R', r[2]])
for o in out:
    if o[0] == 'D':
        print('PASS x%-4d #%s-#%s rt0=%s surf=%s msaa=%s wscissor=%s winoff=%s idx=%d nshaders=%d depth=%s' % (o[2], o[5], o[6], *o[1], o[4], len(o[3]), ','.join(sorted(o[7]))))
    else:
        g = o[1]
        print('    RESOLVE #%s src=%s rect=(%s,%s)-(%s,%s) dest=%s pitch=%s h=%s fmt=%s endian=%s swap=%s clear c=%s d=%s' % g[:14])
