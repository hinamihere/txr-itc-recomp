p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()

def rep(old, new, what, count=1):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, count)

# 1. stencil read/write masks
rep('ID3D11DepthStencilState* GetDepthState(uint32_t dc)\n{', 'ID3D11DepthStencilState* GetDepthState(uint32_t dc, uint32_t refMask)\n{', 'GetDepthState sig')
rep('d.StencilEnable = stencil; d.StencilReadMask = 0xFF; d.StencilWriteMask = 0xFF;',
    'd.StencilEnable = stencil; d.StencilReadMask = (UINT8)((refMask >> 8) & 0xFF); d.StencilWriteMask = (UINT8)((refMask >> 16) & 0xFF);', 'stencil masks')
rep('g_ctx->OMSetDepthStencilState(GetDepthState(depthCtl), XReg(xr::RB_STENCILREFMASK) & 0xFF);',
    'g_ctx->OMSetDepthStencilState(GetDepthState(depthCtl, XReg(xr::RB_STENCILREFMASK)), XReg(xr::RB_STENCILREFMASK) & 0xFF);', 'GetDepthState call')

# 2. texture exp_adjust packed into texswz bits 12..17
rep('for (uint32_t s = 0; s < 32; s++) cb.texswz[s] = (g_xregs[xr::SHADER_CONSTANT_FETCH_00_3 + s * 6] >> 1) & 0xFFF;',
    'for (uint32_t s = 0; s < 32; s++)\n    {\n        const uint32_t d3 = g_xregs[xr::SHADER_CONSTANT_FETCH_00_3 + s * 6];\n        cb.texswz[s] = ((d3 >> 1) & 0xFFF) | (((d3 >> 13) & 0x3F) << 12);   // swizzle | exponent adjust\n    }',
    'texswz fill')

# PatchSwizzles must keep the exp bits
rep('        cb.texswz[slot] = out;\n', '        cb.texswz[slot] = out | (sw & ~0xFFFu);\n', 'PatchSwizzles keep exp')

# 3. copy shader: exp bias (cfg.y) on RGB
rep('    if (cfg.x != 0) c = c.bgra;\n    return c;', '    if (cfg.x != 0) c = c.bgra;\n    c.rgb *= exp2((float)(int)cfg.y);\n    return c;', 'copy ps bias')
rep('        uint32_t cbd[8] = { (uint32_t)x0, (uint32_t)(y0 - (int)yOff), rw, rh, swapRB ? 1u : 0u, 0, 0, 0 };',
    '        uint32_t cbd[8] = { (uint32_t)x0, (uint32_t)(y0 - (int)yOff), rw, rh, swapRB ? 1u : 0u, (uint32_t)expBias, 0, 0 };', 'copy cb')
rep('bool GpuResolve(ID3D11ShaderResourceView* srcSrv, bool isDepth, int x0, int y0, uint32_t rw, uint32_t rh, uint32_t destBase, uint32_t destPitch, uint32_t destHeight, uint32_t destEndian, uint32_t destFmt, bool swapRB)',
    'bool GpuResolve(ID3D11ShaderResourceView* srcSrv, bool isDepth, int x0, int y0, uint32_t rw, uint32_t rh, uint32_t destBase, uint32_t destPitch, uint32_t destHeight, uint32_t destEndian, uint32_t destFmt, bool swapRB, int expBias)', 'GpuResolve sig')
rep('destEndian, destFmt, swapRB);\n    if (!cpuResolve && src == 4 && !rt && destPitch)',
    'destEndian, destFmt, swapRB, ((int32_t)(di << 10)) >> 26);\n    if (!cpuResolve && src == 4 && !rt && destPitch)', 'call 1')
rep('destPitch, destHeight, destEndian, destFmt, false);\n        }\n    }', 'destPitch, destHeight, destEndian, destFmt, false, 0);\n        }\n    }', 'call 2')
open(p, 'w', encoding='utf-8').write(s)

# 4. shader side: apply the exponent adjust in xe_applyswz
p = 'C:/itc-work/runtime/src/xgpu_shader.cpp'
s = open(p, encoding='utf-8').read()
rep('''    s += "float4 xe_applyswz(float4 t, uint slot)\\n{\\n\\tuint swz = texswz[slot >> 2][slot & 3];\\n\\tfloat v[6] = { t.x, t.y, t.z, t.w, 0.0, 1.0 };\\n";''',
    '''    s += "float4 xe_applyswz(float4 t, uint slot)\\n{\\n\\tuint swz = texswz[slot >> 2][slot & 3];\\n\\tint xe_e = (int)((swz >> 12) & 63u); if (xe_e >= 32) xe_e -= 64;\\n\\tif (xe_e != 0) t.xyz *= exp2((float)xe_e);\\n\\tfloat v[6] = { t.x, t.y, t.z, t.w, 0.0, 1.0 };\\n";''', 'shader exp adjust')
open(p, 'w', encoding='utf-8').write(s)
print('ok')
