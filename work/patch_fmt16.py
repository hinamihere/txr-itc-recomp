p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()

def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)

rep('''    case 6: case 14: bpp = 4; return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 7: bpp = 4; return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 32: bpp = 8; return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 31: bpp = 4; comps = 2; return DXGI_FORMAT_R16G16_FLOAT;
    case 36: bpp = 4; comps = 1; return DXGI_FORMAT_R32_FLOAT;''',
'''    case 6: case 14: bpp = 4; return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 7: bpp = 4; return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 2: case 8: case 9: bpp = 1; comps = 1; return DXGI_FORMAT_R8_UNORM;
    case 10: bpp = 2; comps = 2; return DXGI_FORMAT_R8G8_UNORM;
    case 24: bpp = 2; comps = 1; return DXGI_FORMAT_R16_UNORM;
    case 25: bpp = 4; comps = 2; return DXGI_FORMAT_R16G16_UNORM;
    case 26: bpp = 8; return DXGI_FORMAT_R16G16B16A16_UNORM;
    case 30: bpp = 2; comps = 1; return DXGI_FORMAT_R16_FLOAT;
    case 32: bpp = 8; return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 31: bpp = 4; comps = 2; return DXGI_FORMAT_R16G16_FLOAT;
    case 36: bpp = 4; comps = 1; return DXGI_FORMAT_R32_FLOAT;''', 'ResolvedFormat')

rep('''                if (bpp == 8 && r.endian == 4)
                {
                    uint64_t q; memcpy(&q, row + x * 8, 8); q = __builtin_bswap64(q); memcpy(dest + off, &q, 8);
                    continue;
                }
                for (uint32_t i = 0; i < bpp / 4; i++)''',
'''                if (bpp == 8 && r.endian == 4)
                {
                    uint64_t q; memcpy(&q, row + x * 8, 8); q = __builtin_bswap64(q); memcpy(dest + off, &q, 8);
                    continue;
                }
                if (bpp == 1) { dest[off] = row[x]; continue; }
                if (bpp == 2)
                {
                    uint16_t v; memcpy(&v, row + x * 2, 2);
                    if (r.endian != 0) v = __builtin_bswap16(v);
                    memcpy(dest + off, &v, 2);
                    continue;
                }
                for (uint32_t i = 0; i < bpp / 4; i++)''', 'FlushResolved small formats')
open(p, 'w', encoding='utf-8').write(s)
print('ok')
