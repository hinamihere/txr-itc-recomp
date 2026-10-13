import sys, os, struct
iso, out = sys.argv[1], sys.argv[2]
SEC = 2048
f = open(iso, 'rb')
base = None
for off in (0, 0x1FB020*0, 0xFD90000, 0x2080000, 0x89D80000, 0x18300000):
    f.seek(off + 32*SEC)
    if f.read(20) == b'MICROSOFT*XBOX*MEDIA':
        base = off; break
if base is None: sys.exit("no XDVDFS signature found")
print("XDVDFS base offset", hex(base))
f.seek(base + 32*SEC + 20)
root_sec, root_size = struct.unpack('<II', f.read(8))
def walk_dir(sector, size, path):
    f.seek(base + sector*SEC); data = f.read(size)
    def node(off):
        if off*4 >= len(data): return
        o = off*4
        l, r, sec, sz, attr, nl = struct.unpack_from('<HHIIBB', data, o)
        if l == 0xFFFF and r == 0xFFFF: return
        name = data[o+14:o+14+nl].decode('latin1')
        if l: node(l)
        full = os.path.join(path, name)
        if attr & 0x10:
            os.makedirs(full, exist_ok=True); walk_dir(sec, sz, full)
        else:
            print(full, sz)
            f.seek(base + sec*SEC)
            with open(full, 'wb') as w:
                rem = sz
                while rem:
                    c = f.read(min(rem, 1<<22)); w.write(c); rem -= len(c)
            f.seek(0)
        if r: node(r)
    node(0)
walk_dir(root_sec, root_size, out)
