p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what, count=1):
    global s
    n = s.count(old)
    if n != count:
        raise SystemExit('count %d != %d: %s' % (n, count, what))
    s = s.replace(old, new)


# ---- staggered re-validation of static data ---------------------------------------------------------------------------------
rep('''const bool g_noTexMemo = g_noPerf || getenv("ITC_NO_TEXMEMO") != nullptr;      // look every texture up again''',
    '''const bool g_noTexMemo = g_noPerf || getenv("ITC_NO_TEXMEMO") != nullptr;      // look every texture up again
// Guest memory is compared with the GPU copies (vertex buffers per 4 KB page, textures by hash) to notice CPU writes. Data that has been unchanged for kStableAfter
// consecutive comparisons is "static" and only re-compared every kRecheckPeriod frames (staggered over the pages / textures); anything that changes becomes "dynamic"
// again and is compared every frame. A mesh whose index list is new (index cache miss) is always compared at once. ITC_RECHECK_PERIOD=1 compares everything every frame.
const uint32_t kRecheckPeriod = []() { uint32_t v = getenv("ITC_RECHECK_PERIOD") ? (uint32_t)std::max(1, std::min(64, atoi(getenv("ITC_RECHECK_PERIOD")))) : (g_noPerf ? 1u : 8u); uint32_t pw = 1; while (pw * 2 <= v) pw *= 2; return pw; }();   // a power of two
const uint32_t kStableAfter = 3;
bool g_forceVbCheck = false;   // set for the draw whose index list was not cached''', 'consts')

rep('''    std::vector<uint32_t> pageStamp;   // frame in which each 4 KB page was last compared with guest memory
    uint32_t lastFrame = 0;''', '''    std::vector<uint32_t> pageStamp;   // frame in which each 4 KB page was last compared with guest memory
    std::vector<uint8_t> pageStable;   // consecutive comparisons that found the page unchanged (saturating)
    uint32_t lastFrame = 0;''', 'vb entry')
rep('''        e.shadow.assign(src, src + avail);
        e.pageStamp.assign((avail + 4095) / 4096, g_frame);''', '''        e.shadow.assign(src, src + avail);
        e.pageStamp.assign((avail + 4095) / 4096, g_frame);
        e.pageStable.assign((avail + 4095) / 4096, 0);''', 'vb create')
rep('''        if (!small && e.pageStamp[pg] == g_frame) continue;
        e.pageStamp[pg] = g_frame;
        const uint32_t o0 = pg * 4096, o1 = std::min(o0 + 4096, avail);
        if (memcmp(e.shadow.data() + o0, src + o0, o1 - o0) != 0)
        {
            D3D11_BOX box = { o0, 0, 0, o1, 1, 1 };
            g_ctx->UpdateSubresource(e.buf.get(), 0, &box, src + o0, 0, 0);
            memcpy(e.shadow.data() + o0, src + o0, o1 - o0);
        }''', '''        if (!small && e.pageStamp[pg] == g_frame) continue;
        if (!small && !g_forceVbCheck && e.pageStable[pg] >= kStableAfter && ((g_frame + pg * 5) & (kRecheckPeriod - 1)) != 0) continue;   // static page: not this frame
        e.pageStamp[pg] = g_frame;
        const uint32_t o0 = pg * 4096, o1 = std::min(o0 + 4096, avail);
        if (memcmp(e.shadow.data() + o0, src + o0, o1 - o0) != 0)
        {
            D3D11_BOX box = { o0, 0, 0, o1, 1, 1 };
            g_ctx->UpdateSubresource(e.buf.get(), 0, &box, src + o0, 0, 0);
            memcpy(e.shadow.data() + o0, src + o0, o1 - o0);
            e.pageStable[pg] = 0;
        }
        else if (e.pageStable[pg] < 255) e.pageStable[pg]++;''', 'vb check')

# the index cache tells whether this draw's list is new
rep('''    g_idxMisses++;
    std::vector<uint32_t> idx; D3D_PRIMITIVE_TOPOLOGY topo; bool rectList; uint32_t minIdx, maxIdx;
    if (!BuildIndices(dp, idx, topo, rectList, minIdx, maxIdx)) return nullptr;''', '''    g_idxMisses++; g_forceVbCheck = true;
    std::vector<uint32_t> idx; D3D_PRIMITIVE_TOPOLOGY topo; bool rectList; uint32_t minIdx, maxIdx;
    if (!BuildIndices(dp, idx, topo, rectList, minIdx, maxIdx)) return nullptr;''', 'idx miss flag')
rep('''    uint32_t idxCount = 0, startVertex = 0;
    {
        // Frames that log index values''', '''    uint32_t idxCount = 0, startVertex = 0;
    g_forceVbCheck = false;
    {
        // Frames that log index values''', 'flag reset')
rep('''            if (!BuildIndices(dp, idx, topo, rectList, minIdx, maxIdx)) { g_skippedDraws++; return; }
            idxCount = (uint32_t)idx.size();''', '''            if (!BuildIndices(dp, idx, topo, rectList, minIdx, maxIdx)) { g_skippedDraws++; return; }
            g_forceVbCheck = true;
            idxCount = (uint32_t)idx.size();''', 'detailed force')

# textures: static ones are hashed only every kRecheckPeriod frames
rep('''    uint32_t levels = 1;
};
std::unordered_map<uint64_t, CachedTex> g_textures;''', '''    uint32_t levels = 1;
    uint8_t stable = 0;   // consecutive hash comparisons that found the data unchanged (saturating)
};
std::unordered_map<uint64_t, CachedTex> g_textures;''', 'cachedtex stable')
rep('''    uint64_t hash = 0;
    if (ct.srv)
    {
        Decoded unused;
        { ProfScope ps(g_profHash); DecodeTexture(tf, unused, true, &hash); if (ml.levels > 1) hash = (hash ^ HashMipRegion(ml)) * 0x100000001B3ull; }
        ct.checkedFrame = g_frame; ct.checkedSeq = g_memSeq;
        if (hash == ct.dataHash && ct.levels == ml.levels) { g_slotComps[slot & 31] = ct.comps; return ct.srv.get(); }
    }''', '''    uint64_t hash = 0;
    if (ct.srv)
    {
        if (ct.stable >= kStableAfter && ct.levels == ml.levels && !WrittenSince(ct.checkedSeq, ct.base, ct.footprint) && ((g_frame + (uint32_t)(key & 31)) & (kRecheckPeriod - 1)) != 0)
        {
            ct.checkedFrame = g_frame;   // static texture: its data is not compared this frame (checkedSeq stays: a later GPU write to its memory still triggers the comparison)
            g_slotComps[slot & 31] = ct.comps;
            return ct.srv.get();
        }
        Decoded unused;
        { ProfScope ps(g_profHash); DecodeTexture(tf, unused, true, &hash); if (ml.levels > 1) hash = (hash ^ HashMipRegion(ml)) * 0x100000001B3ull; }
        ct.checkedFrame = g_frame; ct.checkedSeq = g_memSeq;
        if (hash == ct.dataHash && ct.levels == ml.levels) { if (ct.stable < 255) ct.stable++; g_slotComps[slot & 31] = ct.comps; return ct.srv.get(); }
        ct.stable = 0;
    }''', 'tex stagger')

# sampler lookups per slot are remembered as well
rep('''            ID3D11SamplerState* smp = GetSampler(GetTexFetch(slot));''', '''            ID3D11SamplerState* smp = GetSamplerFast(slot);''', 'sampler fast call')
rep('''// ---- Vertex buffer cache ------------------------------------------------------------------------------------''', '''// Sampler lookups are remembered per fetch slot (keyed by the two fetch-constant dwords that define the sampler).
ID3D11SamplerState* GetSamplerFast(uint32_t slot)
{
    struct Memo { uint32_t d0, d3; ID3D11SamplerState* s; };
    static Memo memo[32] = {};
    const uint32_t d0 = XReg(xr::SHADER_CONSTANT_FETCH_00_0 + slot * 6), d3 = XReg(xr::SHADER_CONSTANT_FETCH_00_3 + slot * 6);
    Memo& m = memo[slot & 31];
    if (m.s && m.d0 == d0 && m.d3 == d3) return m.s;
    m.s = GetSampler(GetTexFetch(slot)); m.d0 = d0; m.d3 = d3;
    return m.s;
}

// ---- Vertex buffer cache ------------------------------------------------------------------------------------''', 'sampler fast def')
open(p, 'w', encoding='utf-8').write(s)
print('patched part 3')
