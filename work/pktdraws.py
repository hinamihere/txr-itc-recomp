import re, sys, collections
# usage: python pktdraws.py <log> [min_index_count]
# Lists, per bin-select pass, the draws (from the [pkt] trace) with their predicate masks, grouped by (mask, executed).
path = sys.argv[1]
minc = int(sys.argv[2]) if len(sys.argv) > 2 else 300
rx = re.compile(r'\[pkt\] d(\d+) op=(\w+) n=(\d+)( PRED)? mask=(\w+) sel=(\w+) \[(\w+) (\w+) (\w+) (\w+)\]')
passes = collections.OrderedDict()
for l in open(path, 'r', errors='replace'):
    m = rx.match(l)
    if not m:
        continue
    d, op, n, pred, mask, sel, a, b, c, e = m.groups()
    if op not in ('22', '36'):
        continue
    init = int(b, 16) if op == '22' else int(a, 16)
    cnt = init >> 16
    prim = init & 0x3F
    mask_i, sel_i = int(mask, 16), int(sel, 16)
    execd = (not pred) or ((mask_i & sel_i) != 0)
    key = sel
    passes.setdefault(key, collections.Counter())[(mask, 'PRED' if pred else 'plain', 'big' if cnt >= minc else 'small', execd)] += 1
for sel, c in passes.items():
    print('sel=%s' % sel)
    for (mask, pred, size, execd), n in sorted(c.items(), key=lambda kv: -kv[1]):
        print('   %4d draws  mask=%-9s %-5s %-5s executed=%s' % (n, mask, pred, size, execd))
