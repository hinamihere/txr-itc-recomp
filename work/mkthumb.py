import sys, os
from PIL import Image
# usage: python mkthumb.py <frame.png> <name>   -> refs/<name>.thumb (64x36 grey thumbnail, same filter as the game: mean of 20x20 blocks)
src, name = sys.argv[1], sys.argv[2]
im = Image.open(src).convert('RGB')
assert im.size == (1280, 720), im.size
px = im.load()
out = bytearray()
for by in range(36):
    for bx in range(64):
        s = 0
        for y in range(20):
            for x in range(20):
                r, g, b = px[bx * 20 + x, by * 20 + y]
                s += (r * 77 + g * 150 + b * 29) >> 8
        out.append(s // 400)
os.makedirs('C:/itc-work/refs', exist_ok=True)
open('C:/itc-work/refs/%s.thumb' % name, 'wb').write(out)
print('wrote', name, len(out))
