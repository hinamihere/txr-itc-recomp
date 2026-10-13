import re, sys
# usage: python drawsum.py <log> [skip_first_n_draws=60]
# Summarises the draw/resolve lines of the logged frame as run-length-merged entries.
path = sys.argv[1]
skip = int(sys.argv[2]) if len(sys.argv) > 2 else 60
lines = [l for l in open(path, 'r', errors='replace') if l.startswith('[gpu] draw #') or l.startswith('[gpu] resolve #')]
nd = 0
rows = []
for l in lines:
    if l.startswith('[gpu] draw #'):
        nd += 1
        if nd <= skip:
            continue
        m = re.search(r'draw #(\d+) prim=(\d+) count=(\d+) \((\d+) idx\) vs=(\w+) ps=(\w+) rt0=(\w+) surf=(\d+x\d+) pitch=(\d+) msaa=(\d) depthctl=(\w+) blend0=(\w+) mask=(\w+) tex\(vs/ps\)=(\d+)/(\d+) vf=(\d+) vte=(\w+)', l)
        if not m:
            continue
        g = m.groups()
        key = ('D', g[1], g[3], g[4][:6], g[5][:6], g[6], g[7], g[9], g[10], g[11], g[12], g[13] + '/' + g[14])
        rows.append((key, g[0]))
    else:
        if nd <= skip:
            continue
        m = re.search(r'resolve #(\d+) src=(\d) rect=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\) dest=(\w+) pitch=(\d+) h=(\d+) fmt=(\d+) endian=(\d) swap=(\d) clear c=(\d) d=(\d) rt=(\w+)', l)
        if m:
            g = m.groups()
            rows.append((('R',) + g[1:], g[0]))
out = []
for key, num in rows:
    if out and out[-1][0] == key:
        out[-1][1] += 1
    else:
        out.append([key, 1, num])
for key, n, num in out:
    if key[0] == 'D':
        print('%4dx draw#%s prim=%s idx=%s vs=%s ps=%s rt0=%s surf=%s msaa=%s depth=%s blend=%s mask=%s tex=%s' % ((n, num) + key[1:]))
    else:
        print('     RESOLVE#%s src=%s rect=(%s,%s)-(%s,%s) dest=%s pitch=%s h=%s fmt=%s endian=%s swap=%s clear c=%s d=%s' % ((num,) + key[1:14]))
