import sys, glob, os
from PIL import Image, ImageDraw
# usage: python sheet.py <tag> [cols] [tile_width]   -> dumps/sheet_<tag>.png from dumps/frame_<tag>_*.png
tag = sys.argv[1] if len(sys.argv) > 1 else ''
cols = int(sys.argv[2]) if len(sys.argv) > 2 else 6
w = int(sys.argv[3]) if len(sys.argv) > 3 else 320
h = w * 9 // 16
files = sorted(glob.glob('C:/itc-work/dumps/frame%s_*.png' % tag))
rows = (len(files) + cols - 1) // cols
sheet = Image.new('RGB', (cols * w, rows * h))
d = ImageDraw.Draw(sheet)
for i, f in enumerate(files):
    im = Image.open(f).convert('RGB').resize((w, h))
    x, y = (i % cols) * w, (i // cols) * h
    sheet.paste(im, (x, y))
    d.text((x + 4, y + 4), os.path.basename(f)[:-4].split('_')[-1], fill=(255, 255, 0))
out = 'C:/itc-work/dumps/sheet_%s.png' % (tag or 'all')
sheet.save(out)
print(len(files), out)
