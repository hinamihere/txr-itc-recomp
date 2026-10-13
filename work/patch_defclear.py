p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()

def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)

# 1. deferred clear state + applier, placed right after the depth RT map
rep('DXGI_FORMAT ColorRtFormat(uint32_t f)',
    '''// Clears requested by the resolves of a predicated-tiling frame are applied lazily before the next draw: the frame is drawn once (full
// frame) in the first strip pass and the later strips still read it, so the hardware's "clear after resolve" has to wait until the
// whole frame has been resolved - and it has to cover the whole frame, not just the last strip.
struct PendingClear
{
    bool color = false, depth = false;
    ColorRT* rt = nullptr; float col[4] = {};
    uint32_t dbase = 0; float dz = 1.0f; uint8_t dstencil = 0;
} g_pendClear;
void ApplyPendingClears()
{
    if (g_pendClear.color && g_pendClear.rt && g_pendClear.rt->rtv) g_ctx->ClearRenderTargetView(g_pendClear.rt->rtv.get(), g_pendClear.col);
    if (g_pendClear.depth)
    {
        auto it = g_depthRTs.find(g_pendClear.dbase);
        if (it != g_depthRTs.end() && it->second.dsv) g_ctx->ClearDepthStencilView(it->second.dsv.get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, g_pendClear.dz, g_pendClear.dstencil);
    }
    g_pendClear.color = g_pendClear.depth = false;
}

DXGI_FORMAT ColorRtFormat(uint32_t f)''', 'pending clear')

# 2. resolve: record instead of clearing while tiling
rep('''    if (clearColor && rt && !tilingNow)
    {
        const uint32_t cv = XReg(xr::RB_COLOR_CLEAR);
        float col[4] = { (cv & 0xFF) / 255.0f, ((cv >> 8) & 0xFF) / 255.0f, ((cv >> 16) & 0xFF) / 255.0f, ((cv >> 24) & 0xFF) / 255.0f };
        g_ctx->ClearRenderTargetView(rt->rtv.get(), col);
    }
    if (clearDepth && !tilingNow)
    {
        const uint32_t dv = XReg(xr::RB_DEPTH_CLEAR);
        const uint32_t dbase = XReg(xr::RB_DEPTH_INFO) & 0xFFF;
        auto it = g_depthRTs.find(dbase);
        if (it != g_depthRTs.end() && it->second.dsv)
            g_ctx->ClearDepthStencilView(it->second.dsv.get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, (float)(dv >> 8) / 16777215.0f, (UINT8)(dv & 0xFF));
    }''',
    '''    if (clearColor && rt)
    {
        const uint32_t cv = XReg(xr::RB_COLOR_CLEAR);
        float col[4] = { (cv & 0xFF) / 255.0f, ((cv >> 8) & 0xFF) / 255.0f, ((cv >> 16) & 0xFF) / 255.0f, ((cv >> 24) & 0xFF) / 255.0f };
        if (!tilingNow) g_ctx->ClearRenderTargetView(rt->rtv.get(), col);
        else { g_pendClear.color = true; g_pendClear.rt = rt; memcpy(g_pendClear.col, col, sizeof col); }
    }
    if (clearDepth)
    {
        const uint32_t dv = XReg(xr::RB_DEPTH_CLEAR);
        const uint32_t dbase = XReg(xr::RB_DEPTH_INFO) & 0xFFF;
        if (!tilingNow)
        {
            auto it = g_depthRTs.find(dbase);
            if (it != g_depthRTs.end() && it->second.dsv)
                g_ctx->ClearDepthStencilView(it->second.dsv.get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, (float)(dv >> 8) / 16777215.0f, (UINT8)(dv & 0xFF));
        }
        else { g_pendClear.depth = true; g_pendClear.dbase = dbase; g_pendClear.dz = (float)(dv >> 8) / 16777215.0f; g_pendClear.dstencil = (UINT8)(dv & 0xFF); }
    }''', 'resolve clears')

# 3. apply before the next draw
rep('    if (mode != 4 && mode != 5) return;\n    if (g_skipDraws) return;',
    '    if (mode != 4 && mode != 5) return;\n    if (g_skipDraws) return;\n    if (g_pendClear.color || g_pendClear.depth) ApplyPendingClears();', 'apply')
open(p, 'w', encoding='utf-8').write(s)
print('ok')
