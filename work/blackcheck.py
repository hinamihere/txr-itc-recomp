import sys, glob, os
from PIL import Image
# usage: python blackcheck.py <tag> [threshold]  -- mean brightness (0..255) of every dumps/frame<tag>_N.png, flags frames whose 3D area (central 70%) is nearly black
tag = sys.argv[1]
thr = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
files = sorted(glob.glob('C:/itc-work/dumps/frame%s_*.png' % tag))
bad = 0
out = []
for f in files:
    im = Image.open(f).convert('L').resize((64, 36))
    w, h = im.size
    px = list(im.getdata())
    mean = sum(px) / len(px)
    # central region (skips the HUD at the corners / bottom)
    cen = [px[y * w + x] for y in range(6, 28) for x in range(10, 54)]
    cmean = sum(cen) / len(cen)
    n = int(os.path.basename(f).split('_')[1].split('.')[0])
    flag = 'BLACK' if cmean < thr else ''
    if flag:
        bad += 1
    out.append('%6d  mean %5.1f  center %5.1f  %s' % (n, mean, cmean, flag))
print('\n'.join(out))
print('%s: %d frames, %d dark (center < %.0f)' % (tag, len(files), bad, thr))
