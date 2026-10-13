p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)


# ---- 1. mip layout helper, placed before CachedTex ------------------------------------------------------------------
rep('struct CachedTex\n{', r'''// Guest layout of the mip levels of a tiled 2D texture (rules from Xenia's GetGuestTextureLayout): level 0 sits at the base address with the pitch of the fetch
// constant; levels >= 1 start at mip_address, one after the other, each 4 KB aligned; their pitch/rows come from the next power of two of the base size >> level,
// aligned to 32 blocks. Levels from the "packed tail" on (minimum dimension <= 16 texels, fetch constant packed_mips bit) share one 32x32-block tile and are not
// used here: the chain simply ends before them.
struct MipLayout
{
    uint32_t levels = 1;                 // number of D3D mip levels (>= 1)
    struct Level { uint32_t w, h, addr, pitchBlocks, strideBytes; } lv[16];
    uint32_t mipAddr = 0;
    size_t regionBytes = 0;              // bytes from mip_address to the end of the last stored level
};
inline uint32_t Log2Ceil(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) r++; return r; }
inline uint32_t Log2Floor(uint32_t v) { uint32_t r = 0; while ((2u << r) <= v && r < 31) r++; return r; }
inline uint32_t NextPow2(uint32_t v) { return 1u << Log2Ceil(v ? v : 1); }
MipLayout ComputeMips(const TexFetch& tf)
{
    MipLayout m;
    if (tf.dimension() != 1 || !tf.tiled() || tf.type() != 2) return m;
    static const bool noMips = getenv("ITC_NO_MIPS") != nullptr;
    const uint32_t mipFilter = (tf.d[3] >> 23) & 3;
    const uint32_t mipAddr = tf.d[5] & 0xFFFFF000u;
    const uint32_t maxLevel = (tf.d[4] >> 6) & 0xF;
    if (noMips || mipFilter == 2 || mipAddr == 0 || maxLevel == 0) return m;
    const uint32_t w = (tf.d[2] & 0x1FFF) + 1, h = ((tf.d[2] >> 13) & 0x1FFF) + 1;
    if (w < 2 && h < 2) return m;
    const FmtDesc fd = DescribeFormat(tf.format());
    if (fd.kind == K_UNKNOWN) return m;
    const uint32_t bw = fd.bdim, bpb = fd.bpb;
    const bool hasPacked = (tf.d[5] >> 11) & 1;
    const uint32_t l2min = std::min(Log2Ceil(w), Log2Ceil(h));
    const uint32_t packedLevel = hasPacked ? (l2min > 4 ? l2min - 4 : 0) : 0xFFFFFFFFu;
    uint32_t maxL = std::min(maxLevel, Log2Floor(std::max(w, h)));
    if (packedLevel != 0xFFFFFFFFu) maxL = std::min(maxL, packedLevel == 0 ? 0u : packedLevel - 1);
    if (maxL == 0) return m;
    const uint32_t npw = NextPow2(w), nph = NextPow2(h);
    uint32_t offset = 0;
    for (uint32_t L = 1; L <= maxL; L++)
    {
        MipLayout::Level& lv = m.lv[L];
        lv.w = std::max(w >> L, 1u); lv.h = std::max(h >> L, 1u);
        const uint32_t pw = std::max(npw >> L, 1u), ph = std::max(nph >> L, 1u);
        lv.pitchBlocks = (((pw + bw - 1) / bw) + 31) & ~31u;
        const uint32_t rows = (((ph + bw - 1) / bw) + 31) & ~31u;
        lv.strideBytes = (uint32_t)(((size_t)lv.pitchBlocks * bpb * rows + 4095) & ~(size_t)4095);
        lv.addr = mipAddr + offset;
        offset += lv.strideBytes;
        // every level must be backed by committed guest memory, otherwise decoding it would produce black mips
        if (CommittedBytes(PhysPtr(lv.addr), lv.strideBytes) < lv.strideBytes) { maxL = L - 1; offset -= lv.strideBytes; break; }
    }
    if (maxL == 0) return m;
    m.levels = maxL + 1; m.mipAddr = mipAddr; m.regionBytes = offset;
    return m;
}
// Fetch constant that describes mip level L as an ordinary 2D base level.
TexFetch MipLevelFetch(const TexFetch& tf, const MipLayout& m, uint32_t L)
{
    TexFetch t = tf;
    const MipLayout::Level& lv = m.lv[L];
    const FmtDesc fd = DescribeFormat(tf.format());
    t.d[0] = (tf.d[0] & ~(0x1FFu << 22)) | ((((lv.pitchBlocks * fd.bdim) >> 5) & 0x1FFu) << 22);
    t.d[1] = (tf.d[1] & 0xFFFu) | (lv.addr & 0xFFFFF000u);
    t.d[2] = (tf.d[2] & ~0x03FFFFFFu) | ((lv.w - 1) & 0x1FFFu) | (((lv.h - 1) & 0x1FFFu) << 13);
    t.d[4] = tf.d[4] & ~0x3FFu;
    t.d[5] = (tf.d[5] & ~(0xFFFFF000u | (3u << 9) | (1u << 11))) | (1u << 9);
    return t;
}
uint64_t HashMipRegion(const MipLayout& m)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t L = 1; L < m.levels; L++)
    {
        const uint8_t* p = PhysPtr(m.lv[L].addr);
        h = (h ^ Hash64(p, CommittedBytes(p, m.lv[L].strideBytes))) * 0x100000001B3ull;
    }
    return h;
}

struct CachedTex
{''', 'mip helpers')

rep('    uint32_t comps = 4;\n};\nstd::unordered_map<uint64_t, CachedTex> g_textures;',
    '    uint32_t comps = 4;\n    uint32_t levels = 1;\n};\nstd::unordered_map<uint64_t, CachedTex> g_textures;', 'levels field')

# ---- 2. GetTextureSRV: decode / validate / create with mips ---------------------------------------------------------
old_start = s.index('    const uint64_t key = Hash64(tf.d, sizeof tf.d) ^ 0x1234;\n    CachedTex& ct = g_textures[key];')
old_end = s.index('    if (g_texBytes > (1ull << 30)) { Log("[gpu] texture cache cleared");')
new_block = r'''    const uint64_t key = Hash64(tf.d, sizeof tf.d) ^ 0x1234;
    CachedTex& ct = g_textures[key];
    const MipLayout ml = ComputeMips(tf);
    // Footprint for write tracking: base level, and the mip region when it follows (or precedes) it.
    auto footprintOf = [&](size_t baseBytes) -> size_t
    {
        if (ml.levels <= 1) return baseBytes;
        const uint32_t b = tf.base();
        if (ml.mipAddr >= b) return std::max<size_t>(baseBytes, (size_t)(ml.mipAddr - b) + ml.regionBytes);
        return baseBytes;
    };
    if (ct.srv && ct.checkedFrame == g_frame && !WrittenSince(ct.checkedSeq, ct.base, ct.footprint)) { g_slotComps[slot & 31] = ct.comps; return ct.srv.get(); }
    uint64_t hash = 0;
    if (ct.srv)
    {
        Decoded unused;
        { ProfScope ps(g_profHash); DecodeTexture(tf, unused, true, &hash); if (ml.levels > 1) hash = (hash ^ HashMipRegion(ml)) * 0x100000001B3ull; }
        ct.checkedFrame = g_frame; ct.checkedSeq = g_memSeq;
        if (hash == ct.dataHash && ct.levels == ml.levels) { g_slotComps[slot & 31] = ct.comps; return ct.srv.get(); }
    }
    static Decoded dec;
    { ProfScope ps(g_profDecode); DecodeTexture(tf, dec, false, &hash); }
    std::vector<Decoded> mipDecs;
    if (ml.levels > 1)
    {
        ProfScope ps(g_profDecode);
        mipDecs.resize(ml.levels);
        for (uint32_t L = 1; L < ml.levels; L++)
        {
            const TexFetch tl = MipLevelFetch(tf, ml, L);
            uint64_t hl = 0;
            DecodeTexture(tl, mipDecs[L], false, &hl);
            if (mipDecs[L].fmt != dec.fmt || mipDecs[L].data.empty()) { mipDecs.clear(); break; }   // inconsistent chain: fall back to the base level only
        }
        if (!mipDecs.empty()) hash = (hash ^ HashMipRegion(ml)) * 0x100000001B3ull;
    }
    const uint32_t nLevels = mipDecs.empty() ? 1 : ml.levels;
    g_decodeCount++; g_decodeBytes += dec.data.size();
    if (g_log && g_decodeCount < 60) Log("[gpu] decode tex slot %u fmt=%u %ux%u tiled=%u endian=%u mips=%u -> %u bytes", slot, tf.format(), dec.w, dec.h, tf.tiled(), tf.endian(), nLevels, (unsigned)dec.data.size());
    if (ct.tex && ct.w == dec.w && ct.h == dec.h && ct.fmt == dec.fmt && ct.levels == nLevels)
    {
        { ProfScope pu(g_profUpload); g_ctx->UpdateSubresource(ct.tex.get(), 0, nullptr, dec.data.data(), dec.rowPitch, 0);
          for (uint32_t L = 1; L < nLevels; L++) g_ctx->UpdateSubresource(ct.tex.get(), L, nullptr, mipDecs[L].data.data(), mipDecs[L].rowPitch, 0); }
        ct.dataHash = hash; ct.checkedFrame = g_frame; ct.checkedSeq = g_memSeq; ct.comps = dec.comps; g_slotComps[slot & 31] = dec.comps;
        return ct.srv.get();
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = dec.w; d.Height = dec.h; d.MipLevels = nLevels; d.ArraySize = 1; d.Format = dec.fmt; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd[16] = {};
    sd[0] = { dec.data.data(), dec.rowPitch, 0 };
    size_t totalBytes = dec.data.size();
    for (uint32_t L = 1; L < nLevels; L++) { sd[L] = { mipDecs[L].data.data(), mipDecs[L].rowPitch, 0 }; totalBytes += mipDecs[L].data.size(); }
    CP<ID3D11Texture2D> t;
    HRESULT chr = g_dev->CreateTexture2D(&d, sd, t.put());
    if (FAILED(chr) && d.MipLevels > 1) { d.MipLevels = 1; t.reset(); chr = g_dev->CreateTexture2D(&d, sd, t.put()); }   // fall back to the base level only
    if (FAILED(chr)) { Log("[gpu] texture create failed %ux%u fmt %u mips %u", dec.w, dec.h, (unsigned)dec.fmt, nLevels); return g_dummy2D.get(); }
    CP<ID3D11ShaderResourceView> srv;
    g_dev->CreateShaderResourceView(t.get(), nullptr, srv.put());
    g_texBytes += totalBytes;
    g_texBytes -= ct.bytes;
    ct.base = tf.base(); ct.footprint = footprintOf(TextureFootprint(tf)); ct.tex = t; ct.srv = srv; ct.dataHash = hash; ct.checkedFrame = g_frame; ct.checkedSeq = g_memSeq; ct.w = dec.w; ct.h = dec.h; ct.fmt = dec.fmt;
    ct.bytes = totalBytes; ct.comps = dec.comps; ct.levels = d.MipLevels; g_slotComps[slot & 31] = dec.comps;
'''
s = s[:old_start] + new_block + s[old_end:]

# ---- 3. sampler: base-map-only filter limits the LOD; anisotropy from the fetch constant -----------------------------
rep('''    const uint64_t key = cx | (cy << 3) | (cz << 6) | (mag << 9) | (mn << 11) | (mip << 13);
    auto& s = g_samplers[key];
    if (!s)
    {
        D3D11_SAMPLER_DESC d = {};
        const bool linMin = mn == 1 || mn == 3, linMag = mag == 1 || mag == 3, linMip = mip == 1;
        d.Filter = (D3D11_FILTER)((linMin ? 0x10 : 0) | (linMag ? 0x4 : 0) | (linMip ? 1 : 0));
        d.AddressU = ClampToAddr(cx); d.AddressV = ClampToAddr(cy); d.AddressW = ClampToAddr(cz);
        d.MaxAnisotropy = 1; d.ComparisonFunc = D3D11_COMPARISON_NEVER; d.MaxLOD = D3D11_FLOAT32_MAX;''',
    '''    const uint32_t aniso = (tf.d[3] >> 26) & 7;   // 0/1 = off, 2 = 2:1, 3 = 4:1, 4 = 8:1, 5+ = 16:1
    const uint64_t key = cx | (cy << 3) | (cz << 6) | (mag << 9) | (mn << 11) | (mip << 13) | ((uint64_t)aniso << 15);
    auto& s = g_samplers[key];
    if (!s)
    {
        D3D11_SAMPLER_DESC d = {};
        const bool linMin = mn == 1 || mn == 3, linMag = mag == 1 || mag == 3, linMip = mip == 1;
        d.Filter = (D3D11_FILTER)((linMin ? 0x10 : 0) | (linMag ? 0x4 : 0) | (linMip ? 1 : 0));
        d.AddressU = ClampToAddr(cx); d.AddressV = ClampToAddr(cy); d.AddressW = ClampToAddr(cz);
        d.MaxAnisotropy = 1; d.ComparisonFunc = D3D11_COMPARISON_NEVER; d.MaxLOD = (mip == 2) ? 0.0f : D3D11_FLOAT32_MAX;
        static const bool noAniso = getenv("ITC_NO_ANISO") != nullptr;
        if (!noAniso && linMin && linMag && linMip && aniso >= 2)
        {
            d.Filter = D3D11_FILTER_ANISOTROPIC;
            d.MaxAnisotropy = aniso == 2 ? 2 : aniso == 3 ? 4 : aniso == 4 ? 8 : 16;
        }''', 'sampler')
open(p, 'w', encoding='utf-8').write(s)
print('ok')
