import re
p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()

# ---- 1. color RT: typeless + SRV
s = s.replace("""struct ColorRT
{
    CP<ID3D11Texture2D> tex;
    CP<ID3D11RenderTargetView> rtv;""", """struct ColorRT
{
    CP<ID3D11Texture2D> tex;
    CP<ID3D11RenderTargetView> rtv;
    CP<ID3D11ShaderResourceView> srv;""")
old = """    d.Width = nw; d.Height = nh; d.MipLevels = 1; d.ArraySize = 1; d.Format = ColorRtFormat(fmt); d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
    CP<ID3D11Texture2D> t;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, t.put()))) { Log("[gpu] RT create failed %ux%u fmt %u", nw, nh, fmt); return nullptr; }
    CP<ID3D11RenderTargetView> v;
    g_dev->CreateRenderTargetView(t.get(), nullptr, v.put());"""
new = """    const DXGI_FORMAT rtFmt = ColorRtFormat(fmt);
    const DXGI_FORMAT texFmt = (rtFmt == DXGI_FORMAT_R8G8B8A8_UNORM || rtFmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ? DXGI_FORMAT_R8G8B8A8_TYPELESS : rtFmt;
    d.Width = nw; d.Height = nh; d.MipLevels = 1; d.ArraySize = 1; d.Format = texFmt; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    CP<ID3D11Texture2D> t;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, t.put()))) { Log("[gpu] RT create failed %ux%u fmt %u", nw, nh, fmt); return nullptr; }
    CP<ID3D11RenderTargetView> v;
    { D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = rtFmt; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; g_dev->CreateRenderTargetView(t.get(), &rd, v.put()); }
    CP<ID3D11ShaderResourceView> sv;
    { D3D11_SHADER_RESOURCE_VIEW_DESC sd = {}; sd.Format = (rtFmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ? DXGI_FORMAT_R8G8B8A8_UNORM : rtFmt; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1; g_dev->CreateShaderResourceView(t.get(), &sd, sv.put()); }"""
assert old in s
s = s.replace(old, new)
old = "rt.tex = t; rt.rtv = v; rt.w = nw; rt.h = nh; rt.fmt = fmt; rt.dxgi = d.Format;"
assert old in s
s = s.replace(old, "rt.tex = t; rt.rtv = v; rt.srv = sv; rt.w = nw; rt.h = nh; rt.fmt = fmt; rt.dxgi = rtFmt;")

# ---- 2. resolved cache
cache_code = open('C:/itc-work/resolved_cache.inc', encoding='utf-8').read()
old = "uint32_t g_slotComps[32];\n"
assert old in s
s = s.replace(old, old + cache_code, 1)

# ---- 3. use resolved in GetTextureSRV
old = """    g_slotComps[slot & 31] = 4;
    if (shaderDim == 2) return g_dummy3D.get();"""
new = """    g_slotComps[slot & 31] = 4;
    if (tf.type() == 2 && shaderDim != 2 && shaderDim != 3)
    {
        const uint32_t base = tf.base() & 0x1FFFFFFF;
        uint32_t fw, fh;
        if (tf.dimension() == 0) { fw = (tf.d[2] & 0xFFFFFF) + 1; fh = 1; } else { fw = (tf.d[2] & 0x1FFF) + 1; fh = ((tf.d[2] >> 13) & 0x1FFF) + 1; }
        auto it = g_resolved.find(base);
        if (it != g_resolved.end())
        {
            Resolved& r = it->second;
            const uint32_t ff = tf.format();
            const bool fmtOk = (ff == r.fmt) || (ff == 6 && r.fmt == 14) || (ff == 14 && r.fmt == 6);
            if (fmtOk && fw == r.w && fh == r.h && tf.tiled() && MemSig(base, r.bytes) == r.memSig)
            {
                g_slotComps[slot & 31] = r.comps;
                return r.srv.get();
            }
            FlushResolved(base);   // incompatible use: make guest memory authoritative again
        }
        // Any other pending resolved texture overlapping this one must be written back before decoding from memory.
        if (!g_resolved.empty()) FlushOverlapping(base, (size_t)fw * fh * 16, 0xFFFFFFFF);
    }
    if (shaderDim == 2) return g_dummy3D.get();"""
assert old in s
s = s.replace(old, new)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
