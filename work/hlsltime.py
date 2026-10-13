import ctypes, sys, os, time, glob
from ctypes import c_void_p, c_char_p, c_size_t, c_uint, POINTER, byref
# usage: python hlsltime.py [dir]  -- compile every dumped .hlsl with several D3DCompile flag sets and print the compile times
d3d = ctypes.WinDLL('d3dcompiler_47.dll')
d3d.D3DCompile.argtypes = [c_char_p, c_size_t, c_char_p, c_void_p, c_void_p, c_char_p, c_char_p, c_uint, c_uint, POINTER(c_void_p), POINTER(c_void_p)]
d3d.D3DCompile.restype = ctypes.c_long
d = sys.argv[1] if len(sys.argv) > 1 else 'C:/itc-work/shaders'
flag_sets = [('opt1(0)', 0), ('skipopt(4)', 4), ('level0', 1 << 14)]
rows = []
for path in sorted(glob.glob(d + '/*.hlsl')):
    name = os.path.basename(path)
    if '.fail' in name:
        continue
    src = open(path, 'rb').read()
    target = b'ps_5_0' if name.startswith('ps_') else b'vs_5_0'
    t = []
    for fn, fl in flag_sets:
        code = c_void_p(); err = c_void_p()
        t0 = time.perf_counter()
        hr = d3d.D3DCompile(src, len(src), name.encode(), None, None, b'main', target, fl, 0, byref(code), byref(err))
        t.append((time.perf_counter() - t0) * 1000.0 if hr >= 0 else -1.0)
    rows.append((t[0], name, len(src), t))
rows.sort(reverse=True)
tot = [0.0, 0.0, 0.0]
for r in rows:
    for i in range(3):
        tot[i] += max(r[3][i], 0)
print('shaders: %d   total ms  opt1=%.0f  skipopt=%.0f  level0=%.0f' % (len(rows), tot[0], tot[1], tot[2]))
for r in rows[:25]:
    print('%8.0f %8.0f %8.0f  %7d  %s' % (r[3][0], r[3][1], r[3][2], r[2], r[1]))
