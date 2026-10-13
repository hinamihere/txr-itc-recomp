import ctypes, sys, os
from ctypes import c_void_p, c_char_p, c_size_t, c_uint, POINTER, byref, string_at
# usage: python hlslflags.py <file.hlsl> [pre-edit: init=zero]  -- tries several D3DCompile flag sets on one shader and prints which ones compile
d3d = ctypes.WinDLL('d3dcompiler_47.dll')
d3d.D3DCompile.argtypes = [c_char_p, c_size_t, c_char_p, c_void_p, c_void_p, c_char_p, c_char_p, c_uint, c_uint, POINTER(c_void_p), POINTER(c_void_p)]
d3d.D3DCompile.restype = ctypes.c_long

def blob_bytes(p):
    vtbl = ctypes.cast(p, POINTER(c_void_p))[0]
    vt = ctypes.cast(vtbl, POINTER(c_void_p))
    GetPtr = ctypes.WINFUNCTYPE(c_void_p, c_void_p)(vt[3])
    GetSize = ctypes.WINFUNCTYPE(c_size_t, c_void_p)(vt[4])
    return string_at(GetPtr(p), GetSize(p))

path = sys.argv[1]
src0 = open(path, 'rb').read()
name = os.path.basename(path)
target = b'ps_5_0' if name.startswith('ps_') else b'vs_5_0'
variants = {
    'orig': src0,
    'zeroinit': src0.replace(b'float4 r[64];\n\t[unroll] for (int xi = 0; xi < 64; xi++) r[xi] = float4(0,0,0,0);', b'float4 r[64] = (float4[64])0;'),
    'zeroinit-nounroll': src0.replace(b'[unroll] for (int xi = 0; xi < 64; xi++) r[xi] = float4(0,0,0,0);', b'for (int xi = 0; xi < 64; xi++) r[xi] = float4(0,0,0,0);'),
}
flag_sets = {'default(0)': 0, 'skipopt(4)': 4, 'level0(1<<14)': 1 << 14, 'level3(1<<15)': 1 << 15, 'avoidflow(512)': 512, 'preferflow(1024)': 1024, 'skipvalidation(2)': 2}
for vn, src in variants.items():
    for fn, fl in flag_sets.items():
        code = c_void_p(); err = c_void_p()
        hr = d3d.D3DCompile(src, len(src), name.encode(), None, None, b'main', target, fl, 0, byref(code), byref(err))
        msg = ''
        if err.value:
            t = blob_bytes(err).decode('latin1')
            for ln in t.split('\n'):
                if 'error' in ln:
                    msg = ln[ln.find('error'):][:110]
                    break
        print('%-18s %-18s hr=%08X %s' % (vn, fn, hr & 0xFFFFFFFF, msg))
