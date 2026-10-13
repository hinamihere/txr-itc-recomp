import sys, re
# usage: python fpsum.py <tag> [from_frame]  -- race fps per 2 s window from the [stat] lines of <tag>.err, plus the mean
tag = sys.argv[1]; start = int(sys.argv[2]) if len(sys.argv) > 2 else 1040
prev = None; fps = []; dr = []
for ln in open('C:/itc-work/%s.err' % tag, errors='replace'):
    m = re.search(r'\[stat\] ([0-9.]+)s: frames=(\d+).*? draw=(\d+)', ln)
    if m:
        t = float(m.group(1)); f = int(m.group(2)); d = int(m.group(3))
        if prev is not None and f > start:
            fps.append((f - prev) / t); dr.append(d / max(1, (f - prev)))
        prev = f
if fps:
    print('%s: %d windows, mean %.1f fps, min %.1f, max %.1f, draw-thread ms/frame mean %.1f' % (tag, len(fps), sum(fps) / len(fps), min(fps), max(fps), sum(dr) / len(dr)))
    print(' '.join('%.0f' % x for x in fps))
