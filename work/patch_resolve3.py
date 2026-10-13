p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()
start = s.index("bool GpuResolve(ColorRT* rt, int x0, int y0")
end = s.index("struct F4 { float v[4]; };")
new = r'''// Copies a rectangle of a render target (colour SRV, or depth SRV when isDepth) into the GPU-side resolve cache entry for `destBase`.
bool GpuResolve(ID3D11ShaderResourceView* srcSrv, bool isDepth, int x0, int y0, uint32_t rw, uint32_t rh, uint32_t destBase, uint32_t destPitch, uint32_t destHeight, uint32_t destEndian, uint32_t destFmt, bool swapRB)
{
    uint32_t bpp, comps;
    DXGI_FORMAT df;
    if (isDepth) { df = DXGI_FORMAT_R32_FLOAT; bpp = 4; comps = 1; destFmt = 22; }   // depth is kept as float and fetched as k_24_8
    else df = ResolvedFormat(destFmt, bpp, comps);
    ID3D11PixelShader* copyPs = isDepth ? g_copyDepthPS.get() : g_copyPS.get();
    if (df == DXGI_FORMAT_UNKNOWN || !copyPs || !g_presentVS || !srcSrv || rw == 0 || rh == 0) return false;
    const uint32_t alignedPitch = (destPitch + 31) & ~31u;
    const uint32_t hAligned = (std::max(destHeight, rh) + 31) & ~31u;
    const size_t newBytes = (size_t)alignedPitch * hAligned * bpp;

    Resolved* tgt = nullptr;     // entry that receives the rectangle
    uint32_t tgtBase = destBase;
    uint32_t yOff = 0;           // row inside the entry where the rectangle lands

    // Predicated tiling: tile k of a frame buffer is resolved to (base + k * tileBytes) with the whole surface's height as dest height.
    // Such a tile continues the texture of the tiles before it, so it is appended to that entry (on the GPU) instead of becoming a
    // separate small texture that would have to be flushed through guest memory before the final image can be sampled.
    if (g_resolved.find(destBase) == g_resolved.end())
    {
        const size_t rowBlockBytes = (size_t)alignedPitch * 32 * bpp;   // bytes of one row of 32x32 tiles
        for (auto& kv : g_resolved)
        {
            Resolved& e = kv.second;
            if (kv.first >= destBase || e.isDepth != isDepth || e.dxgi != df || e.pitch != destPitch || e.endian != destEndian || e.w != rw) continue;
            const size_t delta = destBase - kv.first;
            if (delta % rowBlockBytes != 0) continue;
            const uint32_t y = (uint32_t)(delta / rowBlockBytes) * 32;
            const uint32_t surfaceRows = std::max(e.height, e.h);
            // Either it lands inside the rows the entry already has, or it appends to a surface that is declared taller than it is so far.
            const bool inside = y + rh <= e.h;
            const bool appends = y <= e.h && y + rh <= surfaceRows && e.height > e.h;
            if (!inside && !appends) continue;
            tgt = &e; tgtBase = kv.first; yOff = y;
            break;
        }
    }
    if (!tgt)
    {
        auto it = g_resolved.find(destBase);
        if (it != g_resolved.end())
        {
            Resolved& old = it->second;
            const bool sameLayout = old.isDepth == isDepth && old.dxgi == df && old.pitch == destPitch && old.endian == destEndian;
            if (sameLayout && rw <= old.w && rh <= old.h) { /* sub-rectangle of the existing texture: merge on the GPU */ }
            else if (!(sameLayout && rw >= old.w && rh >= old.h)) FlushResolved(destBase);   // neither contains the other: make memory authoritative
            // else: the new resolve fully covers the old one; it simply replaces it.
        }
        // Older pending resolves at other addresses that overlap this destination must reach memory before this one.
        FlushOverlapping(destBase, newBytes, destBase);
        tgt = &g_resolved[destBase];
    }
    Resolved& r = *tgt;

    // (Re)allocate the texture when it does not exist yet, has another layout, or has to grow downwards to take the new rows.
    const bool sameLayoutNow = r.tex && r.dxgi == df && r.isDepth == isDepth && r.pitch == destPitch && r.endian == destEndian;
    if (!r.tex || !sameLayoutNow || r.w < rw || r.h < yOff + rh)
    {
        const bool keep = r.tex && sameLayoutNow;   // growing: keep what is already in it
        CP<ID3D11Texture2D> nt;
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = keep ? std::max(r.w, rw) : rw; d.Height = keep ? std::max(r.h, yOff + rh) : yOff + rh;
        d.MipLevels = 1; d.ArraySize = 1; d.Format = df; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, nt.put()))) { if (!keep) g_resolved.erase(tgtBase); return false; }
        CP<ID3D11RenderTargetView> nr; CP<ID3D11ShaderResourceView> ns;
        g_dev->CreateRenderTargetView(nt.get(), nullptr, nr.put());
        g_dev->CreateShaderResourceView(nt.get(), nullptr, ns.put());
        if (keep)
        {
            D3D11_BOX box = { 0, 0, 0, r.w, r.h, 1 };
            g_ctx->CopySubresourceRegion(nt.get(), 0, 0, 0, 0, r.tex.get(), 0, &box);
        }
        r.tex = nt; r.rtv = nr; r.srv = ns; r.w = d.Width; r.h = d.Height;
    }
    r.dxgi = df; r.fmt = destFmt; r.pitch = destPitch; r.endian = destEndian; r.comps = comps; r.isDepth = isDepth;
    r.height = std::max(destHeight, yOff + rh);
    r.bytes = std::max(r.bytes, (size_t)alignedPitch * ((std::max(r.height, r.h) + 31) & ~31u) * bpp);
    r.memSig = MemSig(tgtBase, r.bytes);

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_copyCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        // SV_Position carries framebuffer coordinates (the viewport offset included), so the source rectangle origin is shifted back.
        uint32_t cbd[8] = { (uint32_t)x0, (uint32_t)(y0 - (int)yOff), rw, rh, swapRB ? 1u : 0u, 0, 0, 0 };
        memcpy(m.pData, cbd, sizeof cbd);
        g_ctx->Unmap(g_copyCB.get(), 0);
    }
    XTRACE("[trace] GPU-RESOLVE -> E[%08X] tex=%p %ux%u at row %u rect=(%d,%d,%u,%u) fmt=%u swap=%d depth=%d", tgtBase, (void*)r.tex.get(), r.w, r.h, yOff, x0, y0, rw, rh, destFmt, (int)swapRB, (int)isDepth);
    ID3D11RenderTargetView* rtv = r.rtv.get();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(nullptr, 0);
    g_ctx->RSSetState(g_presentRaster.get());
    D3D11_VIEWPORT vp = { 0, (float)yOff, (float)rw, (float)rh, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    D3D11_RECT sc = { 0, (LONG)yOff, (LONG)rw, (LONG)(yOff + rh) };
    g_ctx->RSSetScissorRects(1, &sc);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_presentVS.get(), nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->PSSetShader(copyPs, nullptr, 0);
    ID3D11Buffer* cb = g_copyCB.get(); g_ctx->PSSetConstantBuffers(0, 1, &cb);
    g_ctx->PSSetShaderResources(0, 1, &srcSrv);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr; g_ctx->PSSetShaderResources(0, 1, &none);
    {
        static const bool dumpRes = getenv("ITC_DUMP_RESOLVE") != nullptr;
        const uint32_t dumpFrame = g_captureFrame;
        if (dumpRes && g_frame == dumpFrame) DumpResolvedTexture(r, tgtBase);
    }
    return true;
}

'''
s = s[:start] + new + s[end:]

old = """    if (!cpuResolve && rt && x1 > x0 && y1 > y0 && destPitch)
        gpuDone = GpuResolve(rt, x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), destBase, destPitch, destHeight, destEndian, destFmt, swapRB);"""
new_call = """    if (!cpuResolve && rt && x1 > x0 && y1 > y0 && destPitch)
        gpuDone = GpuResolve(rt->srv.get(), false, x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), destBase, destPitch, destHeight, destEndian, destFmt, swapRB);
    if (!cpuResolve && src == 4 && !rt && destPitch)
    {
        // Depth/stencil resolve (shadow maps, depth textures): kept on the GPU as a float texture.
        auto dit = g_depthRTs.find(XReg(xr::RB_DEPTH_INFO) & 0xFFF);
        if (dit != g_depthRTs.end() && dit->second.srv)
        {
            DepthRT& dr = dit->second;
            const int32_t dx1 = std::min<int32_t>(x1 > 0 ? x1 : (int32_t)dr.w, (int32_t)dr.w), dy1 = std::min<int32_t>(y1 > 0 ? y1 : (int32_t)dr.h, (int32_t)dr.h);
            const int32_t dx0 = std::max(x0, 0), dy0 = std::max(y0, 0);
            if (dx1 > dx0 && dy1 > dy0)
                GpuResolve(dr.srv.get(), true, dx0, dy0, (uint32_t)(dx1 - dx0), (uint32_t)(dy1 - dy0), destBase, destPitch, destHeight, destEndian, destFmt, false);
        }
    }"""
assert old in s
s = s.replace(old, new_call, 1)

# depth copy shader: compile next to the colour copy shader
old = """    if (CompileEntry(kCopyHlsl, "main", "ps_5_0", b.put())) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, g_copyPS.put());"""
new = old + """
    b.reset();
    if (CompileEntry(kCopyDepthHlsl, "main", "ps_5_0", b.put())) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, g_copyDepthPS.put());"""
assert old in s
s = s.replace(old, new, 1)
old = "const char* kCopyHlsl = R\"HLSL("
idx = s.index(old)
depth_src = '''const char* kCopyDepthHlsl = R"HLSL(
cbuffer P : register(b0) { uint4 rect; uint4 cfg; };
Texture2D<float> srcD : register(t0);
float4 main(float4 pos : SV_Position) : SV_Target
{
    uint2 p = uint2(pos.xy);
    float d = srcD.Load(int3(rect.xy + p, 0));
    return float4(d, d, d, d);
}
)HLSL";
'''
s = s[:idx] + depth_src + s[idx:]
open(p, 'w', encoding='utf-8').write(s)
print('ok')
