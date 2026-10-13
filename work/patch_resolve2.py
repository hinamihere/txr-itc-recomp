p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()

# copy PS + CB globals, created in EnsureInit
s = s.replace("CP<ID3D11Buffer> g_idxDummy;", """CP<ID3D11Buffer> g_idxDummy;
CP<ID3D11PixelShader> g_copyPS;
CP<ID3D11Buffer> g_copyCB;
const char* kCopyHlsl = R"HLSL(
cbuffer P : register(b0) { uint4 rect; uint4 cfg; };
Texture2D<float4> src : register(t0);
float4 main(float4 pos : SV_Position) : SV_Target
{
    uint2 p = uint2(pos.xy);
    float4 c = src.Load(int3(rect.xy + p, 0));
    if (cfg.x != 0) c = c.bgra;
    return c;
}
)HLSL";""")

old = "    g_dev->CreateBuffer(&cb, nullptr, g_presentCB.put());\n"
assert old in s
s = s.replace(old, old + "    cb.ByteWidth = 32;\n    g_dev->CreateBuffer(&cb, nullptr, g_copyCB.put());\n", 1)

old = "    D3D11_SAMPLER_DESC smd = {};"
assert old in s
s = s.replace(old, """    b.reset();
    if (CompileEntry(kCopyHlsl, "main", "ps_5_0", b.put())) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, g_copyPS.put());
""" + old, 1)

# GpuResolve function before "struct F4"
gpu_resolve = r'''
bool GpuResolve(ColorRT* rt, int x0, int y0, uint32_t rw, uint32_t rh, uint32_t destBase, uint32_t destPitch, uint32_t destHeight, uint32_t destEndian, uint32_t destFmt, bool swapRB)
{
    uint32_t bpp, comps;
    const DXGI_FORMAT df = ResolvedFormat(destFmt, bpp, comps);
    if (df == DXGI_FORMAT_UNKNOWN || !g_copyPS || !g_presentVS || !rt->srv || rw == 0 || rh == 0) return false;
    Resolved& r = g_resolved[destBase];
    if (!r.tex || r.w != rw || r.h != rh || r.dxgi != df)
    {
        r.tex.reset(); r.srv.reset(); r.rtv.reset();
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = rw; d.Height = rh; d.MipLevels = 1; d.ArraySize = 1; d.Format = df; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, r.tex.put()))) { g_resolved.erase(destBase); return false; }
        g_dev->CreateRenderTargetView(r.tex.get(), nullptr, r.rtv.put());
        g_dev->CreateShaderResourceView(r.tex.get(), nullptr, r.srv.put());
    }
    r.w = rw; r.h = rh; r.dxgi = df; r.fmt = destFmt; r.pitch = destPitch; r.height = destHeight; r.endian = destEndian; r.comps = comps;
    const uint32_t alignedPitch = (destPitch + 31) & ~31u;
    const uint32_t hAligned = (std::max(destHeight, rh) + 31) & ~31u;
    r.bytes = (size_t)alignedPitch * hAligned * bpp;
    r.memSig = MemSig(destBase, r.bytes);

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_copyCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        uint32_t cbd[8] = { (uint32_t)x0, (uint32_t)y0, rw, rh, swapRB ? 1u : 0u, 0, 0, 0 };
        memcpy(m.pData, cbd, sizeof cbd);
        g_ctx->Unmap(g_copyCB.get(), 0);
    }
    ID3D11RenderTargetView* rtv = r.rtv.get();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(nullptr, 0);
    g_ctx->RSSetState(g_presentRaster.get());
    D3D11_VIEWPORT vp = { 0, 0, (float)rw, (float)rh, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_presentVS.get(), nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->PSSetShader(g_copyPS.get(), nullptr, 0);
    ID3D11Buffer* cb = g_copyCB.get(); g_ctx->PSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* srv = rt->srv.get(); g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr; g_ctx->PSSetShaderResources(0, 1, &none);
    return true;
}

'''
old = "struct F4 { float v[4]; };"
assert old in s
s = s.replace(old, gpu_resolve + old, 1)

# integrate in XGpuDoResolve
old = "    if (rt && x1 > x0 && y1 > y0 && destPitch)\n    {\n        const uint32_t rw = x1 - x0, rh = y1 - y0;"
assert old in s
new = """    static const bool cpuResolve = getenv("ITC_CPU_RESOLVE") != nullptr;
    bool gpuDone = false;
    if (!cpuResolve && rt && x1 > x0 && y1 > y0 && destPitch)
        gpuDone = GpuResolve(rt, x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), destBase, destPitch, destHeight, destEndian, destFmt, swapRB);
    if (!gpuDone && rt && x1 > x0 && y1 > y0 && destPitch)
    {
        const uint32_t rw = x1 - x0, rh = y1 - y0;
        {
            uint32_t qb, qc; ResolvedFormat(destFmt, qb, qc);
            const uint32_t qbpp = qb ? qb : 4;
            FlushOverlapping(destBase, (size_t)((destPitch + 31) & ~31u) * ((std::max(destHeight, rh) + 31) & ~31u) * qbpp);
            g_resolved.erase(destBase);
        }"""
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
