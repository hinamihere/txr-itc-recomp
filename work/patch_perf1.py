p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what, count=1):
    global s
    n = s.count(old)
    if n != count:
        raise SystemExit('count %d != %d: %s' % (n, count, what))
    s = s.replace(old, new)


# ---------------------------------------------------------------------------------------------------------------------------------
# 1. Resolved entries remember whether they come from the (unused) motion-vector pass; a fetch of such an entry re-enables the pass.
rep('''    bool isDepth = false;   // depth/stencil resolve kept as R32_FLOAT (never written back to guest memory)
};
std::map<uint32_t, Resolved> g_resolved;''',
    '''    bool isDepth = false;   // depth/stencil resolve kept as R32_FLOAT (never written back to guest memory)
    bool velocity = false;  // produced by the motion-vector pass (see g_velocityNeeded)
};
std::map<uint32_t, Resolved> g_resolved;
// The game renders a 512x288 motion-vector pass (~420 draws, pixel shader 1ba23ef83ff60169, colour format 4 = 16_16) every frame, but nothing seen so far ever samples
// its result. Its draws are skipped until a texture fetch (or a write-back to guest memory) actually uses a resolve of that pass: then it is switched on for good.
// ITC_KEEP_VELOCITY=1 renders it always.
constexpr uint64_t kVelocityPsHash = 0x1ba23ef83ff60169ull;
bool g_velocityNeeded = getenv("ITC_KEEP_VELOCITY") != nullptr;
uint64_t g_velocitySkipped = 0;
void NoteVelocityUse(const char* why)
{
    if (g_velocityNeeded) return;
    g_velocityNeeded = true;
    Log("[gpu] the motion-vector pass result is used (%s): rendering that pass from now on", why);
}''', 'resolved velocity flag')

rep('''    ProfScope profFlush(g_profFlush);
    Resolved& r = it->second;
    if (r.isDepth) { g_resolved.erase(it); return; }''',
    '''    ProfScope profFlush(g_profFlush);
    Resolved& r = it->second;
    if (r.isDepth) { g_resolved.erase(it); return; }
    if (r.velocity) NoteVelocityUse("flush to guest memory");''', 'flush velocity')

# ---------------------------------------------------------------------------------------------------------------------------------
# 2. Per-slot memo of the last texture lookup (the fetch constant, the frame and the guest-write sequence identify it).
rep('''ID3D11ShaderResourceView* GetTextureSRV(uint32_t slot, uint32_t shaderDim)
{
    const TexFetch tf = GetTexFetch(slot);
    {''',
    '''// Draws mostly repeat the textures of the previous draw: the result of the last lookup per fetch slot is reused while the fetch constant is unchanged, the frame is the
// same and no guest memory range was written by the GPU emulation since (g_resolvedGen counts changes of the resolved-texture table, g_texGen clears of the cache).
struct TexMemo { uint32_t d[6]; uint32_t dim; uint32_t frame; uint64_t seq; uint32_t resolvedGen, texGen, comps; ID3D11ShaderResourceView* srv; };
TexMemo g_texMemo[32];
uint32_t g_resolvedGen = 1, g_texGen = 1;
ID3D11ShaderResourceView* GetTextureSRVSlow(uint32_t slot, uint32_t shaderDim, const TexFetch& tf);
ID3D11ShaderResourceView* GetTextureSRV(uint32_t slot, uint32_t shaderDim)
{
    const TexFetch tf = GetTexFetch(slot);
    TexMemo& m = g_texMemo[slot & 31];
    if (m.srv && m.frame == g_frame && m.seq == g_memSeq && m.dim == shaderDim && m.resolvedGen == g_resolvedGen && m.texGen == g_texGen && memcmp(m.d, tf.d, sizeof m.d) == 0 &&
        !(g_log && g_frame == g_captureFrame))
    {
        g_slotComps[slot & 31] = m.comps;
        return m.srv;
    }
    ID3D11ShaderResourceView* r = GetTextureSRVSlow(slot, shaderDim, tf);
    const uint32_t gen = g_resolvedGen;   // a flush inside the lookup bumps the generation: remember the value the result was computed with
    memcpy(m.d, tf.d, sizeof m.d); m.dim = shaderDim; m.frame = g_frame; m.seq = g_memSeq; m.resolvedGen = gen; m.texGen = g_texGen; m.comps = g_slotComps[slot & 31]; m.srv = r;
    return r;
}
ID3D11ShaderResourceView* GetTextureSRVSlow(uint32_t slot, uint32_t shaderDim, const TexFetch& tf)
{
    {''', 'texture memo')

# the resolved-texture hit path: remember the use of velocity entries, and the per-frame signature check
rep('''            const bool fmtOk = canon(ff) == canon(r.fmt);
            static const bool noSample = getenv("ITC_NO_RESOLVED_SAMPLE") != nullptr;
            const bool sigOk = !noSample && MemSig(base, r.bytes) == r.memSig;
            if (fmtOk && fw == r.w && fh == r.h && tf.tiled() && sigOk)
            {
                g_slotComps[slot & 31] = r.comps;
                return r.srv.get();
            }''',
    '''            const bool fmtOk = canon(ff) == canon(r.fmt);
            static const bool noSample = getenv("ITC_NO_RESOLVED_SAMPLE") != nullptr;
            // The guest-memory signature (a sample of the target region) is compared once per frame and entry, not at every fetch.
            const bool sigOk = !noSample && (r.sigFrame == g_frame || (MemSig(base, r.bytes) == r.memSig && (r.sigFrame = g_frame, true)));
            if (fmtOk && fw == r.w && fh == r.h && tf.tiled() && sigOk)
            {
                g_slotComps[slot & 31] = r.comps;
                if (r.velocity) NoteVelocityUse("texture fetch");
                return r.srv.get();
            }''', 'resolved hit')
rep('''    uint64_t memSig = 0;
    bool isDepth = false;   // depth/stencil resolve kept as R32_FLOAT''', '''    uint64_t memSig = 0;
    uint32_t sigFrame = 0xFFFFFFFFu;   // frame in which memSig was last compared with guest memory
    bool isDepth = false;   // depth/stencil resolve kept as R32_FLOAT''', 'sigFrame field')

# the generation counters: any change of the resolved table or the texture cache invalidates the memos
rep('''    g_resolved.erase(it);
}

// Flushes every pending resolved texture overlapping''', '''    g_resolved.erase(it);
    g_resolvedGen++;
}

// Flushes every pending resolved texture overlapping''', 'flush gen')
rep('''    if (r.isDepth) { g_resolved.erase(it); return; }
    if (r.velocity)''', '''    if (r.isDepth) { g_resolved.erase(it); g_resolvedGen++; return; }
    if (r.velocity)''', 'flush depth gen')
rep('''    if (g_texBytes > (1ull << 30)) { Log("[gpu] texture cache cleared"); g_textures.clear(); g_texBytes = 0; return g_dummy2D.get(); }''',
    '''    if (g_texBytes > (1ull << 30)) { Log("[gpu] texture cache cleared"); g_textures.clear(); g_texBytes = 0; g_texGen++; return g_dummy2D.get(); }''', 'tex cache gen')

# ---------------------------------------------------------------------------------------------------------------------------------
# 3. Fast state-object lookups (last key per kind) and the redundant-state filter.

rep('''namespace {

// ---- Draw -----------------------------------------------------------------------------------------------------------
void FillConstants(''', '''namespace {

// ---- Redundant-state filter ---------------------------------------------------------------------------------------------------------
// Every Set* call costs the D3D11 runtime and the driver even when nothing changes, and the draws of one pass mostly share render targets, shaders, state objects and
// textures: the values last sent to the context are remembered and unchanged ones are not sent again. Code that sets pipeline state outside XGpuDraw (overlay, resolve,
// present) calls InvalidateBound(); binding render targets invalidates the remembered resource views (D3D11 silently unbinds views of the targets).
void* const kUnk = (void*)~(uintptr_t)0;
struct BoundState
{
    ID3D11RenderTargetView* rtv[4]; ID3D11DepthStencilView* dsv;
    ID3D11VertexShader* vs; ID3D11PixelShader* ps; ID3D11GeometryShader* gs;
    ID3D11BlendState* blend; uint32_t bf[4]; ID3D11DepthStencilState* ds; uint32_t stRef; ID3D11RasterizerState* rs;
    D3D11_VIEWPORT vp; D3D11_RECT sc; int topo;
    ID3D11ShaderResourceView* vsSrv[128]; ID3D11ShaderResourceView* psSrv[128];
    ID3D11SamplerState* vsSmp[16]; ID3D11SamplerState* psSmp[16];
    ID3D11Buffer* vsCb; ID3D11Buffer* psCb;
};
BoundState g_bound;
void InvalidateSrvs()
{
    for (auto& v : g_bound.vsSrv) v = (ID3D11ShaderResourceView*)kUnk;
    for (auto& v : g_bound.psSrv) v = (ID3D11ShaderResourceView*)kUnk;
}
void InvalidateBound()
{
    BoundState& b = g_bound;
    for (auto& v : b.rtv) v = (ID3D11RenderTargetView*)kUnk;
    b.dsv = (ID3D11DepthStencilView*)kUnk; b.vs = (ID3D11VertexShader*)kUnk; b.ps = (ID3D11PixelShader*)kUnk; b.gs = (ID3D11GeometryShader*)kUnk;
    b.blend = (ID3D11BlendState*)kUnk; memset(b.bf, 0xFF, sizeof b.bf); b.ds = (ID3D11DepthStencilState*)kUnk; b.stRef = 0xFFFFFFFFu; b.rs = (ID3D11RasterizerState*)kUnk;
    memset(&b.vp, 0xFF, sizeof b.vp); memset(&b.sc, 0xFF, sizeof b.sc); b.topo = -1;
    InvalidateSrvs();
    for (auto& v : b.vsSmp) v = (ID3D11SamplerState*)kUnk;
    for (auto& v : b.psSmp) v = (ID3D11SamplerState*)kUnk;
    b.vsCb = b.psCb = (ID3D11Buffer*)kUnk;
}
inline void BindVSSrv(uint32_t slot, ID3D11ShaderResourceView* v) { if (g_bound.vsSrv[slot] == v) return; g_bound.vsSrv[slot] = v; g_ctx->VSSetShaderResources(slot, 1, &v); }
inline void BindPSSrv(uint32_t slot, ID3D11ShaderResourceView* v) { if (g_bound.psSrv[slot] == v) return; g_bound.psSrv[slot] = v; g_ctx->PSSetShaderResources(slot, 1, &v); }
inline void BindVSSmp(uint32_t slot, ID3D11SamplerState* v) { if (g_bound.vsSmp[slot] == v) return; g_bound.vsSmp[slot] = v; g_ctx->VSSetSamplers(slot, 1, &v); }
inline void BindPSSmp(uint32_t slot, ID3D11SamplerState* v) { if (g_bound.psSmp[slot] == v) return; g_bound.psSmp[slot] = v; g_ctx->PSSetSamplers(slot, 1, &v); }

// State objects are looked up by the register values that define them; the last lookup of each kind is remembered (consecutive draws mostly repeat it).
ID3D11BlendState* GetBlendStateFast(const uint32_t blendRegs[4], uint32_t colorMask, uint32_t numRTs)
{
    static uint32_t lastKey[6]; static ID3D11BlendState* last = nullptr;
    const uint32_t key[6] = { blendRegs[0], blendRegs[1], blendRegs[2], blendRegs[3], colorMask, numRTs };
    if (last && memcmp(key, lastKey, sizeof key) == 0) return last;
    last = GetBlendState(blendRegs, colorMask, numRTs); memcpy(lastKey, key, sizeof key);
    return last;
}
ID3D11DepthStencilState* GetDepthStateFast(uint32_t dc, uint32_t refMask)
{
    static uint32_t lastDc = 0, lastRef = 0; static ID3D11DepthStencilState* last = nullptr;
    if (last && dc == lastDc && refMask == lastRef) return last;
    last = GetDepthState(dc, refMask); lastDc = dc; lastRef = refMask;
    return last;
}
ID3D11RasterizerState* GetRasterStateFast(bool cullFront, bool cullBack, bool frontCCW, bool depthClip, int depthBias)
{
    static uint32_t lastKey = 0xFFFFFFFFu; static ID3D11RasterizerState* last = nullptr;
    const uint32_t key = (uint32_t)cullFront | ((uint32_t)cullBack << 1) | ((uint32_t)frontCCW << 2) | ((uint32_t)depthClip << 3) | ((uint32_t)(depthBias + 0x8000) << 4);
    if (last && key == lastKey) return last;
    last = GetRasterState(cullFront, cullBack, frontCCW, depthClip, depthBias); lastKey = key;
    return last;
}

// ---- Converted index lists -----------------------------------------------------------------------------------------------------------
// The vertex shader pulls its indices from one big structured buffer. Converted lists (endian swap, strips/fans/quads -> triangle lists, primitive restart removed) are
// appended to it and cached by (guest address, count, format, primitive type, base index, reset index) plus the hash of the raw guest index data, so a repeated draw of
// the same mesh costs one hash of the raw indices instead of a conversion and an upload. When the buffer is full everything is dropped and refilled.
struct IdxEntry { uint64_t k0, k1, k2, content; uint32_t start, count, minIdx, maxIdx, topo; bool rectList; };
std::unordered_map<uint64_t, IdxEntry> g_idxCache;
uint64_t g_idxHits = 0, g_idxMisses = 0, g_idxResets = 0;

uint32_t AppendIndices(const uint32_t* data, uint32_t n)
{
    static const bool idxUpdateMode = getenv("ITC_IDX_UPDATE") != nullptr;
    if (n == 0 || n > kIdxRing) return ~0u;
    bool discard = g_idxFirst;
    if (g_idxPos + n > kIdxRing) { discard = true; g_idxResets++; }
    if (idxUpdateMode)
    {
        if (discard) { g_idxPos = 0; g_idxCache.clear(); }
        D3D11_BOX box = { g_idxPos * 4u, 0, 0, (UINT)((g_idxPos + n) * 4u), 1, 1 };
        g_ctx->UpdateSubresource(g_idxBuf.get(), 0, &box, data, 0, 0);
        g_idxFirst = false;
    }
    else
    {
        static bool noOverwriteBroken = false;
        D3D11_MAPPED_SUBRESOURCE m = {};
        HRESULT hr;
        if (discard || noOverwriteBroken)
        {
            // WRITE_DISCARD gives a fresh buffer instance: everything cached before is gone
            hr = g_ctx->Map(g_idxBuf.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
            g_idxPos = 0; g_idxCache.clear(); g_idxFirst = false;
        }
        else
        {
            hr = g_ctx->Map(g_idxBuf.get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m);
            if (FAILED(hr) || !m.pData)
            {
                // WRITE_NO_OVERWRITE on a shader-resource buffer needs D3D11.1-class driver support: fall back to a discard per list
                Log("[gpu] Map(NO_OVERWRITE) on the index buffer failed (hr=%08X): using WRITE_DISCARD for every list (no index cache)", (unsigned)hr);
                noOverwriteBroken = true;
                hr = g_ctx->Map(g_idxBuf.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
                g_idxPos = 0; g_idxCache.clear();
            }
        }
        if (FAILED(hr) || !m.pData) { Log("[gpu] Map of the index buffer failed (hr=%08X)", (unsigned)hr); return ~0u; }
        memcpy((uint32_t*)m.pData + g_idxPos, data, (size_t)n * 4);
        g_ctx->Unmap(g_idxBuf.get(), 0);
    }
    const uint32_t start = g_idxPos;
    g_idxPos += n;
    return start;
}

const IdxEntry* GetIndexList(const XDrawParams& dp)
{
    const uint32_t n = dp.indexCount;
    if (n == 0 || n > (1u << 21)) return nullptr;
    const uint32_t off = XReg(xr::VGT_INDX_OFFSET);
    const bool resetOn = ((XReg(xr::PA_SU_SC_MODE_CNTL) >> 21) & 1) != 0;
    const uint32_t resetIdx = resetOn ? (XReg(xr::VGT_MULTI_PRIM_IB_RESET_INDX) & (dp.index32 ? 0xFFFFFFFFu : 0xFFFFu)) : 0;
    const bool dma = dp.sourceSelect == 0;
    const uint64_t k0 = ((uint64_t)n << 32) | (dma ? dp.dmaPhysBase : 0);
    const uint64_t k1 = (uint64_t)dp.primType | ((uint64_t)dp.sourceSelect << 8) | ((uint64_t)dp.index32 << 12) | ((uint64_t)dp.dmaEndian << 13) | ((uint64_t)resetOn << 16) | ((uint64_t)off << 32);
    const uint64_t k2 = resetIdx;
    uint64_t content = 0;
    if (dma)
    {
        const uint8_t* mem = PhysPtr(dp.dmaPhysBase);
        const size_t need = dp.index32 ? (size_t)n * 4 : (size_t)((n + 1) / 2) * 4;
        if (CommittedBytes(mem, need) < need) return nullptr;
        content = Hash64(mem, need);
    }
    uint64_t key = (k0 * 0x9E3779B97F4A7C15ull) ^ (k1 * 0xC2B2AE3D27D4EB4Full) ^ (k2 * 0x165667B19E3779F9ull) ^ content;
    key ^= key >> 29;
    auto it = g_idxCache.find(key);
    if (it != g_idxCache.end() && it->second.k0 == k0 && it->second.k1 == k1 && it->second.k2 == k2 && it->second.content == content) { g_idxHits++; return &it->second; }
    g_idxMisses++;
    std::vector<uint32_t> idx; D3D_PRIMITIVE_TOPOLOGY topo; bool rectList; uint32_t minIdx, maxIdx;
    if (!BuildIndices(dp, idx, topo, rectList, minIdx, maxIdx)) return nullptr;
    const uint32_t start = AppendIndices(idx.data(), (uint32_t)idx.size());
    if (start == ~0u) return nullptr;
    IdxEntry& e = g_idxCache[key];   // looked up after the append: it may have cleared the cache
    e = { k0, k1, k2, content, start, (uint32_t)idx.size(), minIdx, maxIdx, (uint32_t)topo, rectList };
    return &e;
}

// ---- Draw -----------------------------------------------------------------------------------------------------------
void FillConstants(''', 'helper section')

# BuildIndices must be declared before GetIndexList: move the helper block after BuildIndices by splitting on a marker
# (BuildIndices is defined after FillConstants/PatchSwizzles/UploadCB, so the helper block is cut and re-inserted after it)
start = s.index('// ---- Converted index lists')
end = s.index('// ---- Draw -----------------------------------------------------------------------------------------------------------\nvoid FillConstants(')
block = s[start:end]
s = s[:start] + s[end:]
marker = '''struct Surface { uint32_t w, h, pitch; };'''
assert s.count(marker) == 1
s = s.replace(marker, block + marker)

# ---------------------------------------------------------------------------------------------------------------------------------
# 4. XGpuDraw: velocity skip, index cache, bound-state filter
rep('''    if (!g_curVS.valid || !g_curPS.valid) { g_skippedDraws++; return; }

    uint64_t T0 = QpcNow();''', '''    if (!g_curVS.valid || !g_curPS.valid) { g_skippedDraws++; return; }
    if (mode == 4 && !g_velocityNeeded && g_curPS.hash == kVelocityPsHash && ((XReg(xr::RB_COLOR_INFO) >> 16) & 0xF) == 4)
    {
        // Motion-vector pass: skipped (see g_velocityNeeded). The render target is still created so that the resolve at the end of the pass produces a (cleared) entry whose use is detected.
        static bool made = false;
        if (!made) { const Surface sf = CurrentSurface(); const uint32_t info = XReg(xr::RB_COLOR_INFO); made = GetColorRT(info & 0xFFF, 4, sf.w, sf.h, true) != nullptr; }
        g_velocitySkipped++;
        return;
    }

    uint64_t T0 = QpcNow();''', 'velocity skip')

rep('''    std::vector<uint32_t> idx; D3D_PRIMITIVE_TOPOLOGY topo; bool rectList; uint32_t minIdx = 0, maxIdx = 0;
    if (!BuildIndices(dp, idx, topo, rectList, minIdx, maxIdx)) { g_skippedDraws++; return; }
    if (mode == 4 && idx.size() >= 300) g_bigDraws++;''',
    '''    std::vector<uint32_t> idx; D3D_PRIMITIVE_TOPOLOGY topo; bool rectList; uint32_t minIdx = 0, maxIdx = 0;
    uint32_t idxCount = 0, startVertex = 0;
    {
        // Frames that log index values need the converted list on the CPU; everything else uses the index cache.
        const bool detailed = g_frame == g_captureFrame || g_frame == g_traceFrame || g_log;
        if (!detailed)
        {
            const IdxEntry* ie = GetIndexList(dp);
            if (!ie) { g_skippedDraws++; return; }
            topo = (D3D_PRIMITIVE_TOPOLOGY)ie->topo; rectList = ie->rectList; minIdx = ie->minIdx; maxIdx = ie->maxIdx; idxCount = ie->count; startVertex = ie->start;
        }
        else
        {
            if (!BuildIndices(dp, idx, topo, rectList, minIdx, maxIdx)) { g_skippedDraws++; return; }
            idxCount = (uint32_t)idx.size();
            startVertex = AppendIndices(idx.data(), idxCount);
            if (startVertex == ~0u) { g_skippedDraws++; return; }
        }
    }
    if (mode == 4 && idxCount >= 300) g_bigDraws++;''', 'index list')

# render targets: filtered
rep('''    if (!rtvs[0] && !dsv) { g_skippedDraws++; return; }
    g_ctx->OMSetRenderTargets(4, rtvs, dsv);
    {
        static int bad = 0;
        ID3D11RenderTargetView* chk = nullptr; g_ctx->OMGetRenderTargets(1, &chk, nullptr);
        if (chk) chk->Release();
        if (chk != rtvs[0] && bad++ < 20) Log("[gpu] WARNING: OMSetRenderTargets was rejected by Direct3D (rt0 %p, bound %p, dsv %p)", (void*)rtvs[0], (void*)chk, (void*)dsv);
    }
''', '''    if (!rtvs[0] && !dsv) { g_skippedDraws++; return; }
    if (memcmp(g_bound.rtv, rtvs, sizeof rtvs) != 0 || g_bound.dsv != dsv)
    {
        g_ctx->OMSetRenderTargets(4, rtvs, dsv);
        memcpy(g_bound.rtv, rtvs, sizeof rtvs); g_bound.dsv = dsv;
        InvalidateSrvs();   // D3D11 unbinds views of resources that are now render targets
        static int bad = 0;
        ID3D11RenderTargetView* chk = nullptr; g_ctx->OMGetRenderTargets(1, &chk, nullptr);
        if (chk) chk->Release();
        if (chk != rtvs[0] && bad++ < 20) Log("[gpu] WARNING: OMSetRenderTargets was rejected by Direct3D (rt0 %p, bound %p, dsv %p)", (void*)rtvs[0], (void*)chk, (void*)dsv);
    }
''', 'rt bind')

# the old "Index list" upload block is replaced by the filtered binding of the index SRV
a = s.index('''    // Index list
    static const bool idxUpdateMode = getenv("ITC_IDX_UPDATE") != nullptr;
    if (idx.size() > kIdxRing)''')
b = s.index('''    g_dp[3] += QpcNow() - T0; T0 = QpcNow();
    // Resources''')
s = s[:a] + s[b:]

rep('''    ID3D11ShaderResourceView* idxSrv = g_idxSRV.get();
    g_ctx->VSSetShaderResources(31, 1, &idxSrv);''', '''    BindVSSrv(31, g_idxSRV.get());''', 'idx srv')
rep('''        g_ctx->VSSetShaderResources(32 + slot, 1, &srv);
    }
    static const char* dumpPs''', '''        BindVSSrv(32 + slot, srv);
    }
    static const char* dumpPs''', 'vb srv')
rep('''            if (stage) { g_ctx->PSSetShaderResources(slot, 1, &srv); g_ctx->PSSetSamplers(slot & 15, 1, &smp); }
            else { g_ctx->VSSetShaderResources(slot, 1, &srv); g_ctx->VSSetSamplers(slot & 15, 1, &smp); }''',
    '''            if (stage) { BindPSSrv(slot, srv); BindPSSmp(slot & 15, smp); }
            else { BindVSSrv(slot, srv); BindVSSmp(slot & 15, smp); }''', 'tex bind')

# constants: upload only changed contents, bind once
rep('''    PatchSwizzles(cbv); PatchSwizzles(cbp);
    UploadCB(g_cbVS.get(), cbv);
    UploadCB(g_cbPS.get(), cbp);
    ID3D11Buffer* cbs[1];
    cbs[0] = g_cbVS.get(); g_ctx->VSSetConstantBuffers(0, 1, cbs);
    cbs[0] = g_cbPS.get(); g_ctx->PSSetConstantBuffers(0, 1, cbs);
''', '''    PatchSwizzles(cbv); PatchSwizzles(cbp);
    {
        // The buffers are only rewritten when the contents differ from what they hold (pixel constants repeat a lot between draws).
        static XeCB lastV, lastP; static bool haveV = false, haveP = false;
        if (!haveV || memcmp(&cbv, &lastV, sizeof cbv) != 0) { UploadCB(g_cbVS.get(), cbv); memcpy(&lastV, &cbv, sizeof cbv); haveV = true; }
        if (!haveP || memcmp(&cbp, &lastP, sizeof cbp) != 0) { UploadCB(g_cbPS.get(), cbp); memcpy(&lastP, &cbp, sizeof cbp); haveP = true; }
        if (g_bound.vsCb != g_cbVS.get()) { ID3D11Buffer* b = g_cbVS.get(); g_ctx->VSSetConstantBuffers(0, 1, &b); g_bound.vsCb = b; }
        if (g_bound.psCb != g_cbPS.get()) { ID3D11Buffer* b = g_cbPS.get(); g_ctx->PSSetConstantBuffers(0, 1, &b); g_bound.psCb = b; }
    }
''', 'consts upload')

# state: fast lookups + filter
a = s.index('''    // State
    uint32_t blendRegs[4]''')
b = s.index('''    if (g_frame == g_traceFrame)
    {
        ID3D11RenderTargetView* r0 = nullptr;''')
new_state = '''    // State
    uint32_t blendRegs[4] = { XReg(xr::RB_BLENDCONTROL0), XReg(xr::RB_BLENDCONTROL1), XReg(xr::RB_BLENDCONTROL2), XReg(xr::RB_BLENDCONTROL3) };
    const float bf[4] = { F(XReg(xr::RB_BLEND_RED)), F(XReg(xr::RB_BLEND_GREEN)), F(XReg(xr::RB_BLEND_BLUE)), F(XReg(xr::RB_BLEND_ALPHA)) };
    {
        ID3D11BlendState* bs = GetBlendStateFast(blendRegs, colorMask, numRTs);
        uint32_t bfBits[4]; memcpy(bfBits, bf, sizeof bfBits);
        if (g_bound.blend != bs || memcmp(g_bound.bf, bfBits, sizeof bfBits) != 0) { g_ctx->OMSetBlendState(bs, bf, 0xFFFFFFFF); g_bound.blend = bs; memcpy(g_bound.bf, bfBits, sizeof bfBits); }
        const uint32_t stRef = XReg(xr::RB_STENCILREFMASK) & 0xFF;
        ID3D11DepthStencilState* dss = GetDepthStateFast(depthCtl, XReg(xr::RB_STENCILREFMASK));
        if (g_bound.ds != dss || g_bound.stRef != stRef) { g_ctx->OMSetDepthStencilState(dss, stRef); g_bound.ds = dss; g_bound.stRef = stRef; }
    }
    const uint32_t sc = XReg(xr::PA_SU_SC_MODE_CNTL), clipCntl = XReg(xr::PA_CL_CLIP_CNTL);
    {
        // Debug/test: ITC_DEPTH_BIAS=N nudges colour draws towards the camera by N depth-buffer steps. The game draws a depth pre-pass and then the
        // colour pass with ZFUNC GREATER_EQUAL; two differently compiled vertex shaders may round the same position one ulp apart.
        static const int biasN = getenv("ITC_DEPTH_BIAS") ? atoi(getenv("ITC_DEPTH_BIAS")) : 0;
        int bias = 0;
        if (biasN && mode == 4 && ((depthCtl >> 1) & 1)) { const uint32_t zf = (depthCtl >> 4) & 7; bias = zf == 6 ? biasN : zf == 3 ? -biasN : 0; }
        ID3D11RasterizerState* rss = rectList ? GetRasterStateFast(false, false, true, !((clipCntl >> 16) & 1), 0)
                                              : GetRasterStateFast(sc & 1, (sc >> 1) & 1, ((sc >> 2) & 1) == 0, !((clipCntl >> 16) & 1), bias);
        if (g_bound.rs != rss) { g_ctx->RSSetState(rss); g_bound.rs = rss; }
    }
    D3D11_VIEWPORT vp = { 0, 0, rtW, rtH, 0.0f, 1.0f };
    if (memcmp(&g_bound.vp, &vp, sizeof vp) != 0) { g_ctx->RSSetViewports(1, &vp); g_bound.vp = vp; }
    {
        const uint32_t tl = XReg(xr::PA_SC_WINDOW_SCISSOR_TL), br = XReg(xr::PA_SC_WINDOW_SCISSOR_BR);
        const uint32_t wo = XReg(xr::PA_SC_WINDOW_OFFSET);
        int32_t l = tl & 0x3FFF, t = (tl >> 16) & 0x3FFF, r = br & 0x3FFF, b = (br >> 16) & 0x3FFF;
        static const bool useWindowOffset = getenv("ITC_WINDOW_OFFSET") != nullptr;
        if (useWindowOffset && !((tl >> 31) & 1)) { const int32_t ox = (int32_t)(wo << 17) >> 17, oy = (int32_t)((wo >> 16) << 17) >> 17; l += ox; r += ox; t += oy; b += oy; }
        const uint32_t stl = XReg(xr::PA_SC_SCREEN_SCISSOR_TL), sbr = XReg(xr::PA_SC_SCREEN_SCISSOR_BR);
        l = std::max<int32_t>(l, (int32_t)((int32_t)(stl << 17) >> 17)); t = std::max<int32_t>(t, (int32_t)(((int32_t)((stl >> 16) << 17)) >> 17));
        r = std::min<int32_t>(r, sbr & 0x7FFF); b = std::min<int32_t>(b, (sbr >> 16) & 0x7FFF);
        l = std::max(l, 0); t = std::max(t, 0); r = std::min<int32_t>(r, (int32_t)actualW); b = std::min<int32_t>(b, (int32_t)actualH);
        if (r <= l || b <= t) { l = 0; t = 0; r = (int32_t)actualW; b = (int32_t)actualH; }
        if (XGpuTilingFirstPass()) { l = 0; t = 0; r = (int32_t)actualW; b = (int32_t)actualH; }   // first strip pass draws the whole frame
        D3D11_RECT sr = { l, t, r, b };
        if (memcmp(&g_bound.sc, &sr, sizeof sr) != 0) { g_ctx->RSSetScissorRects(1, &sr); g_bound.sc = sr; }
    }
    if (g_bound.topo != (int)topo) { g_ctx->IASetPrimitiveTopology(topo); g_bound.topo = (int)topo; }
    {
        ID3D11GeometryShader* gs = rectList ? g_rectGS.get() : nullptr;
        if (g_bound.vs != vs->vs.get()) { g_ctx->VSSetShader(vs->vs.get(), nullptr, 0); g_bound.vs = vs->vs.get(); }
        if (g_bound.gs != gs) { g_ctx->GSSetShader(gs, nullptr, 0); g_bound.gs = gs; }
    }
    {
        static CP<ID3D11PixelShader> redPS;
        static const bool forceRed = getenv("ITC_FORCE_RED") != nullptr;
        if (forceRed && !redPS)
        {
            CP<ID3DBlob> b;
            if (CompileEntry("float4 main() : SV_Target { return float4(1, 0, 0, 1); }", "main", "ps_5_0", b.put())) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, redPS.put());
        }
        ID3D11PixelShader* psObj = (forceRed && redPS && dp.primType != 1) ? redPS.get() : ps->ps.get();
        if (g_bound.ps != psObj) { g_ctx->PSSetShader(psObj, nullptr, 0); g_bound.ps = psObj; }
    }
'''
s = s[:a] + new_state + s[b:]

# the draw call and the remaining uses of idx.size()
rep('''        Log("[trace] DRAW #%llu prim=%u n=%zu rt0=%p srv0=%p ps=%016llx", (unsigned long long)g_drawCount, dp.primType, idx.size(), (void*)rr.get(), (void*)sr.get(), (unsigned long long)g_curPS.hash);''',
    '''        Log("[trace] DRAW #%llu prim=%u n=%u rt0=%p srv0=%p ps=%016llx", (unsigned long long)g_drawCount, dp.primType, idxCount, (void*)rr.get(), (void*)sr.get(), (unsigned long long)g_curPS.hash);''', 'trace log')
rep('''    g_ctx->Draw((UINT)idx.size(), 0);
    if (g_gpuMode >= 2) GpuTimingAfterDraw();''', '''    g_ctx->Draw(idxCount, 0);
    if (g_gpuMode >= 2) GpuTimingAfterDraw();''', 'draw call')
rep('''(unsigned long long)g_drawCount, dp.primType, dp.indexCount, idx.size(), (unsigned long long)g_curVS.hash,''', '''(unsigned long long)g_drawCount, dp.primType, dp.indexCount, (size_t)idxCount, (unsigned long long)g_curVS.hash,''', 'draw log count')
rep('''vs->info.textureSlots.size(), ps->info.textureSlots.size(), vs->info.vfetchSlots.size(), XReg(xr::PA_CL_VTE_CNTL), mode, idx[0], XReg''', '''vs->info.textureSlots.size(), ps->info.textureSlots.size(), vs->info.vfetchSlots.size(), XReg(xr::PA_CL_VTE_CNTL), mode, idx.empty() ? 0u : idx[0], XReg''', 'draw log idx0')

# ---------------------------------------------------------------------------------------------------------------------------------
# 5. Invalidate the remembered state wherever the pipeline is touched outside XGpuDraw
rep('''    ID3D11RenderTargetView* rtv = g_backRTV.get();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->OMSetBlendState(g_ov.blend.get(), nullptr, 0xFFFFFFFF);''', '''    InvalidateBound();
    ID3D11RenderTargetView* rtv = g_backRTV.get();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->OMSetBlendState(g_ov.blend.get(), nullptr, 0xFFFFFFFF);''', 'overlay invalidate')
rep('''        ID3D11RenderTargetView* rtv = g_backRTV.get();
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);''', '''        InvalidateBound();
        ID3D11RenderTargetView* rtv = g_backRTV.get();
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);''', 'present invalidate')

# ---------------------------------------------------------------------------------------------------------------------------------
# 6. Extras: generation bumps, velocity tagging, invalidation in GpuResolve/EnsureInit, forward declaration, stats, arena size
rep('''    ct.bytes = totalBytes; ct.comps = dec.comps; ct.levels = d.MipLevels; g_slotComps[slot & 31] = dec.comps;''',
    '''    ct.bytes = totalBytes; ct.comps = dec.comps; ct.levels = d.MipLevels; g_slotComps[slot & 31] = dec.comps;
    g_texGen++;   // a (re)created view: memoised lookups of this texture may hold the old one''', 'texgen create')
rep('''    const size_t newBytes = (size_t)alignedPitch * hAligned * bpp;

    Resolved* tgt = nullptr;''', '''    const size_t newBytes = (size_t)alignedPitch * hAligned * bpp;
    g_resolvedGen++;     // memoised texture lookups may point at the entry this resolve replaces or grows
    InvalidateBound();   // the copy pass below sets its own pipeline state

    Resolved* tgt = nullptr;''', 'gpuresolve inval')
rep('''            g_resolved.erase(destBase);
        }
        if (!g_staging''', '''            g_resolved.erase(destBase); g_resolvedGen++;
        }
        if (!g_staging''', 'cpu resolve erase')
rep('''        gpuDone = GpuResolve(rt->srv.get(), false, x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), destBase, destPitch, destHeight, destEndian, destFmt, swapRB, ((int32_t)(di << 10)) >> 26);''',
    '''        gpuDone = GpuResolve(rt->srv.get(), false, x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), destBase, destPitch, destHeight, destEndian, destFmt, swapRB, ((int32_t)(di << 10)) >> 26);
    if (gpuDone && rt && src == 0 && rt->fmt == 4)
    {
        auto rit = g_resolved.find(destBase);
        if (rit != g_resolved.end()) rit->second.velocity = true;   // result of the motion-vector pass (colour format 16_16)
    }''', 'velocity tag')
rep('''// Draws up to three lines of text (colour index per line''', '''void InvalidateBound();   // redundant-state filter, defined with the draw code below

// Draws up to three lines of text (colour index per line''', 'fwd decl')
rep('''    Log("[gpu] D3D11 initialized (feature level %x)", (unsigned)got);
    g_initOk = true;''', '''    InvalidateBound();
    Log("[gpu] D3D11 initialized (feature level %x)", (unsigned)got);
    g_initOk = true;''', 'init inval')
rep('''const uint32_t kIdxRing = 1u << 22;''', '''const uint32_t kIdxRing = 1u << 23;   // index arena (see GetIndexList); startVertex travels through a float constant, exact below 2^24''', 'arena size')
rep('''    GpuTimingPrint();
    lastDraws = g_drawCount;''', '''    {
        static uint64_t lh = 0, lm = 0, lr = 0, lv = 0;
        Log("[stat]   index cache: hits +%llu misses +%llu resets +%llu (list=%zu), motion-vector pass draws skipped +%llu%s", (unsigned long long)(g_idxHits - lh), (unsigned long long)(g_idxMisses - lm), (unsigned long long)(g_idxResets - lr),
            g_idxCache.size(), (unsigned long long)(g_velocitySkipped - lv), g_velocityNeeded ? " (pass active)" : "");
        lh = g_idxHits; lm = g_idxMisses; lr = g_idxResets; lv = g_velocitySkipped;
    }
    GpuTimingPrint();
    lastDraws = g_drawCount;''', 'stats')

open(p, 'w', encoding='utf-8').write(s)
print('patched part 1')
