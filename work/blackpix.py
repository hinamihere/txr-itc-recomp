import sys, glob
from PIL import Image
# usage: python blackpix.py <tag>  -> per dumped frame: pure-black pixels (sum of RGB <= 3) in the lower 2/3 of the picture (minus the HUD corners)
for f in sorted(glob.glob('C:/itc-work/dumps/frame%s_*.png' % sys.argv[1])):
    im = Image.open(f).convert('RGB')
    w, h = im.size
    px = im.load()
    n = 0
    for y in range(h // 3, h - 30, 4):
        for x in range(0, w - 300, 4):
            r, g, b = px[x, y]
            if r == g == b and r <= 20: n += 1
    print(f.split('_')[-1][:-4], n)
