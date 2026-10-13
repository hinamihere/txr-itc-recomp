import ctypes, sys, glob, os
from ctypes import c_void_p, c_char_p, c_size_t, c_uint, POINTER, byref, string_at, WINFUNCTYPE
# usage: python hlslcheck.py <file.hlsl> [...]   -- compiles with D3DCompile (ps_5_0 / vs_5_0 by filename prefix), prints errors with the source lines
d3d = ctypes.WinDLL('d3dcompiler_47.dll')
d3d.D3DCompile.argtypes = [c_char_p, c_size_t, c_char_p, c_void_p, c_void_p, c_char_p, c_char_p, c_uint, c_uint, POINTER(c_void_p), POINTER(c_void_p)]
d3d.D3DCompile.restype = ctypes.c_long

class IUnknown(ctypes.Structure):
    pass

def blob_bytes(p):
    # ID3DBlob vtable: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize
    vtbl = ctypes.cast(p, POINTER(c_void_p))[0]
    vt = ctypes.cast(vtbl, POINTER(c_void_p))
    GetPtr = ctypes.WINFUNCTYPE(c_void_p, c_void_p)(vt[3])
    GetSize = ctypes.WINFUNCTYPE(c_size_t, c_void_p)(vt[4])
    return string_at(GetPtr(p), GetSize(p))

for path in sys.argv[1:]:
    src = open(path, 'rb').read()
    name = os.path.basename(path)
    target = b'ps_5_0' if name.startswith('ps_') else b'vs_5_0'
    code = c_void_p(); err = c_void_p()
    hr = d3d.D3DCompile(src, len(src), name.encode(), None, None, b'main', target, 1 << 12, 0, byref(code), byref(err))
    print('==', name, 'hr=%08X' % (hr & 0xFFFFFFFF))
    if err.value:
        text = blob_bytes(err).decode('latin1')
        lines = src.decode('latin1').split('\n')
        for ln in text.split('\n'):
            if 'error' in ln:
                print('  ', ln[:300])
                import re
                m = re.search(r'\((\d+),', ln)
                if m:
                    i = int(m.group(1))
                    if 0 < i <= len(lines):
                        print('      src:', lines[i - 1][:240])
