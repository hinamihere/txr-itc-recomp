
// ---- GPU-side resolve targets ----------------------------------------------------------------------------------
// A resolve normally copies EDRAM into guest memory. To avoid the readback, the result is kept as a GPU texture keyed by its
// destination address; texture fetches of that address use it directly, and it is written back to guest memory only on demand.
struct Resolved
{
    CP<ID3D11Texture2D> tex;
    CP<ID3D11ShaderResourceView> srv;
    CP<ID3D11RenderTargetView> rtv;
    uint32_t w = 0, h = 0, fmt = 0, pitch = 0, height = 0, endian = 0, comps = 4;
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    size_t bytes = 0;
    uint64_t memSig = 0;
};
std::map<uint32_t, Resolved> g_resolved;

DXGI_FORMAT ResolvedFormat(uint32_t destFmt, uint32_t& bpp, uint32_t& comps)
{
    comps = 4;
    switch (destFmt)
    {
    case 6: case 14: bpp = 4; return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 7: bpp = 4; return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 32: bpp = 8; return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 31: bpp = 4; comps = 2; return DXGI_FORMAT_R16G16_FLOAT;
    case 36: bpp = 4; comps = 1; return DXGI_FORMAT_R32_FLOAT;
    default: bpp = 0; return DXGI_FORMAT_UNKNOWN;
    }
}

uint64_t MemSig(uint32_t phys, size_t bytes)
{
    const uint8_t* p = PhysPtr(phys);
    const size_t avail = CommittedBytes(p, bytes);
    const size_t n = avail / 4;
    uint64_t h = 0x84222325CBF29CE4ull ^ n;
    if (!n) return h;
    const uint32_t* w = (const uint32_t*)p;
    const size_t step = std::max<size_t>(1, n / 1024);
    for (size_t i = 0; i < n; i += step) { h ^= w[i]; h *= 0x100000001b3ull; h ^= h >> 29; }
    return h;
}

CP<ID3D11Texture2D> g_flushStaging;
uint32_t g_flushW = 0, g_flushH = 0;
DXGI_FORMAT g_flushFmt = DXGI_FORMAT_UNKNOWN;

// Writes the resolved texture back into guest memory (tiled, endian-swapped) and forgets it.
void FlushResolved(uint32_t base)
{
    auto it = g_resolved.find(base);
    if (it == g_resolved.end()) return;
    Resolved& r = it->second;
    uint32_t bpp, comps;
    ResolvedFormat(r.fmt, bpp, comps);
    if (!g_flushStaging || g_flushW < r.w || g_flushH < r.h || g_flushFmt != r.dxgi)
    {
        g_flushW = std::max(g_flushW, r.w); g_flushH = std::max(g_flushH, r.h); g_flushFmt = r.dxgi;
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = g_flushW; d.Height = g_flushH; d.MipLevels = 1; d.ArraySize = 1; d.Format = r.dxgi; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        g_dev->CreateTexture2D(&d, nullptr, g_flushStaging.put());
    }
    D3D11_BOX box = { 0, 0, 0, r.w, r.h, 1 };
    g_ctx->CopySubresourceRegion(g_flushStaging.get(), 0, 0, 0, 0, r.tex.get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mp;
    if (SUCCEEDED(g_ctx->Map(g_flushStaging.get(), 0, D3D11_MAP_READ, 0, &mp)))
    {
        uint32_t log2bpp = 0; while ((1u << log2bpp) < bpp) log2bpp++;
        const uint32_t alignedPitch = (r.pitch + 31) & ~31u;
        uint8_t* dest = PhysPtrW(base);
        const size_t writable = CommittedBytes(dest, r.bytes);
        for (uint32_t y = 0; y < r.h; y++)
        {
            const uint8_t* row = (const uint8_t*)mp.pData + (size_t)y * mp.RowPitch;
            const uint32_t outer = TiledOuter(y, alignedPitch, log2bpp);
            for (uint32_t x = 0; x < r.w && x < alignedPitch; x++)
            {
                const uint32_t off = TiledInner(x, y, log2bpp, outer);
                if (off + bpp > writable) continue;
                if (bpp == 8 && r.endian == 4)
                {
                    uint64_t q; memcpy(&q, row + x * 8, 8); q = __builtin_bswap64(q); memcpy(dest + off, &q, 8);
                    continue;
                }
                for (uint32_t i = 0; i < bpp / 4; i++)
                {
                    uint32_t v; memcpy(&v, row + x * bpp + i * 4, 4);
                    v = ApplyEndian(v, r.endian == 4 ? 0 : r.endian);
                    memcpy(dest + off + i * 4, &v, 4);
                }
            }
        }
        g_ctx->Unmap(g_flushStaging.get(), 0);
        g_epoch++;
    }
    g_resolved.erase(it);
}

// Flushes every pending resolved texture overlapping [base, base + bytes).
void FlushOverlapping(uint32_t base, size_t bytes, uint32_t exceptBase = 0xFFFFFFFF)
{
    const uint32_t b0 = base & 0x1FFFFFFF;
    for (;;)
    {
        uint32_t victim = 0xFFFFFFFF;
        for (auto& kv : g_resolved)
        {
            if (kv.first == exceptBase) continue;
            if (b0 < kv.first + kv.second.bytes && kv.first < b0 + bytes) { victim = kv.first; break; }
        }
        if (victim == 0xFFFFFFFF) break;
        FlushResolved(victim);
    }
}
