import re, sys
# usage: python pktsum.py <log>   -- summarises the [pkt] trace of the capture frame (bin select/mask changes, draws, resolves)
path = sys.argv[1]
rx = re.compile(r'\[pkt\] d(\d+) op=(\w+) n=(\d+)( PRED)? mask=(\w+) sel=(\w+) \[(\w+) (\w+) (\w+) (\w+)\]')
events = []
for l in open(path, 'r', errors='replace'):
    m = rx.match(l)
    if not m:
        continue
    d, op, n, pred, mask, sel, a, b, c, e = m.groups()
    events.append((int(d), op, int(n), bool(pred), int(mask, 16), int(sel, 16), a, b, c, e))
print(len(events), 'packets')
# collapse: runs of draws with the same (mask, sel, pred) become one line
out = []
def flush(run):
    if run:
        d, kind, pred, mask, sel, cnt, execd = run
        print('  %4dx %-5s %s mask=%X sel=%X  (executed %d)' % (cnt, kind, 'PRED' if pred else '    ', mask, sel, execd))
run = None
for d, op, n, pred, mask, sel, a, b, c, e in events:
    if op in ('22', '36'):
        key = ('draw', pred, mask, sel)
        ex = (not pred) or ((mask & sel) != 0)
        if run and run[1] == 'draw' and run[2] == pred and run[3] == mask and run[4] == sel:
            run[5] += 1; run[6] += 1 if ex else 0
        else:
            flush(run); run = [d, 'draw', pred, mask, sel, 1, 1 if ex else 0]
        continue
    flush(run); run = None
    if op in ('60', '61', '62', '63', '50', '51'):
        print('d%d SET_BIN op=%s %s val=%s' % (d, op, 'PRED' if pred else '', a))
    elif op == '3F':
        print('d%d IB addr=%s size=%d mask=%X sel=%X' % (d, a, int(b, 16) & 0xFFFFF, mask, sel))
    elif op in ('46', '5A'):
        pass
    else:
        print('d%d op=%s n=%d %s [%s %s %s %s] mask=%X sel=%X' % (d, op, n, 'PRED' if pred else '', a, b, c, e, mask, sel))
flush(run)
