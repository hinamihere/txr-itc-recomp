p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what, count=1):
    global s
    n = s.count(old)
    if n != count:
        raise SystemExit('count %d != %d: %s' % (n, count, what))
    s = s.replace(old, new)


# ---- switches / counters ------------------------------------------------------------------------------------------------------------------
rep('''const bool g_noTexMemo = g_noPerf || getenv("ITC_NO_TEXMEMO") != nullptr;      // look every texture up again''',
    '''const bool g_noTexMemo = g_noPerf || getenv("ITC_NO_TEXMEMO") != nullptr;      // look every texture up again
const bool g_noVbRing = g_noPerf || getenv("ITC_NO_VBRING") != nullptr;        // a Direct3D buffer per guest vertex buffer, even for dynamic data
const bool g_noCbRing = g_noPerf || getenv("ITC_NO_CBRING") != nullptr;        // constant buffers: Map DISCARD per upload instead of the offset ring''', 'switches')
rep('''struct FrameEvents { uint64_t texHashed, texCreated, texUpdated, texBytes, vbPagesCmp, vbPagesChanged, vbBytesUp, vbCreated, rtCreated, cpuResolves, flushes, shaderCompiles; };''',
    '''struct FrameEvents { uint64_t texHashed, texCreated, texUpdated, texBytes, vbPagesCmp, vbPagesChanged, vbBytesUp, vbCreated, rtCreated, cpuResolves, flushes, shaderCompiles, vbRingCopies, vbRingBytes, texMemoMiss; };''', 'events')

# ---- vertex buffers: ring for dynamic data ------------------------------------------------------------------------------------------------
rep('''    std::vector<uint8_t> pageStable;   // consecutive comparisons that found the page unchanged (saturating)
    uint32_t lastFrame = 0;
};
std::unordered_map<uint64_t, VBEntry> g_vbs;
size_t g_vbBytes = 0;
''', '''    std::vector<uint8_t> pageStable;   // consecutive comparisons that found the page unchanged (saturating)
    uint32_t lastFrame = 0;
    uint32_t frames = 0;               // distinct frames this guest buffer was drawn in
    uint32_t hot = 0;                  // frames left in "hot" mode (data changes every frame: the used range is copied through the ring instead)
    uint32_t changeStreak = 0, lastChangeFrame = 0xFFFFFFFFu;
};
std::unordered_map<uint64_t, VBEntry> g_vbs;
size_t g_vbBytes = 0;
uint32_t g_vfBaseDw[96];               // per vertex fetch slot of the current draw: dword offset to add to the fetch addresses (ring copies), 0 for persistent buffers

// Dynamic vertex data: the game places short-lived geometry (HUD, particles, per-frame meshes) at ever new addresses, and a persistent Direct3D buffer per
// (address, size) meant ~30 buffer creations per frame. A guest buffer is only given a persistent buffer when it has been drawn in kVbPromoteAfter different
// frames; before that, and while its data changes every frame ("hot"), the byte range the draw reads is copied into this 32 MB dynamic ring (one NO_OVERWRITE map
// per copy, DISCARD at the wrap) and the vertex shader gets the ring position as a dword offset (vfmeta.x). The offset may wrap below zero: the shader adds it
// to index * stride in dword arithmetic, so the sum is exact for every byte inside the copied range.
CP<ID3D11Buffer> g_vbRing;
CP<ID3D11ShaderResourceView> g_vbRingSRV;
const uint32_t kVbRing = 32u << 20;
uint32_t g_vbRingPos = 0;
bool g_vbRingFirst = true;
const uint32_t kVbPromoteAfter = 3;
ID3D11ShaderResourceView* VbRingCopy(const uint8_t* src, uint32_t rangeStart, uint32_t rangeEnd, uint32_t avail, uint32_t* baseDw)
{
    if (!g_vbRing) return nullptr;
    const uint32_t rs = std::min(rangeStart & ~15u, avail), re = std::min((rangeEnd + 15) & ~15u, avail);
    if (re <= rs) return nullptr;
    const uint32_t n = re - rs;
    if (n > kVbRing / 8) return nullptr;   // huge range: a persistent buffer is the better deal
    bool discard = g_vbRingFirst;
    if (g_vbRingPos + n > kVbRing) { discard = true; }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(g_vbRing.get(), 0, discard ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m))) return nullptr;
    if (discard) { g_vbRingPos = 0; g_vbRingFirst = false; }
    uint8_t* dst = (uint8_t*)m.pData + g_vbRingPos;
    const size_t have = CommittedBytes(src + rs, n);
    memcpy(dst, src + rs, have);
    if (have < n) memset(dst + have, 0, n - have);
    g_ctx->Unmap(g_vbRing.get(), 0);
    *baseDw = (uint32_t)((int32_t)(g_vbRingPos / 4) - (int32_t)(rs / 4));
    g_vbRingPos += n;
    g_ev.vbRingCopies++; g_ev.vbRingBytes += n;
    return g_vbRingSRV.get();
}
''', 'vb ring')

rep('''ID3D11ShaderResourceView* GetVertexBuffer(uint32_t physBase, uint32_t sizeBytes, uint32_t rangeStart, uint32_t rangeEnd)
{
    if (sizeBytes == 0) return nullptr;
    sizeBytes = std::min<uint32_t>(sizeBytes, 128u << 20);
    const uint8_t* src = PhysPtr(physBase);
    const uint64_t key = ((uint64_t)physBase << 32) | sizeBytes;
    VBEntry& e = g_vbs[key];
    e.lastFrame = g_frame;
    // Round UP to 16 bytes (shaders load float4s): rounding down cut the last vertex of small buffers (a 3-vertex rect list of 40-byte vertices
    // is 120 bytes) and made its trailing attributes read as zero, which collapsed the texture coordinates of every full-screen post-process quad.
    const uint32_t avail = (sizeBytes + 15) & ~15u;
    if (!e.buf)
    {
        if (avail == 0) { g_vbs.erase(key); return nullptr; }''',
    '''ID3D11ShaderResourceView* GetVertexBuffer(uint32_t physBase, uint32_t sizeBytes, uint32_t rangeStart, uint32_t rangeEnd, uint32_t* baseDw)
{
    *baseDw = 0;
    if (sizeBytes == 0) return nullptr;
    sizeBytes = std::min<uint32_t>(sizeBytes, 128u << 20);
    const uint8_t* src = PhysPtr(physBase);
    const uint64_t key = ((uint64_t)physBase << 32) | sizeBytes;
    VBEntry& e = g_vbs[key];
    if (e.lastFrame != g_frame || e.frames == 0) { e.frames++; if (e.hot) e.hot--; }
    e.lastFrame = g_frame;
    // Round UP to 16 bytes (shaders load float4s): rounding down cut the last vertex of small buffers (a 3-vertex rect list of 40-byte vertices
    // is 120 bytes) and made its trailing attributes read as zero, which collapsed the texture coordinates of every full-screen post-process quad.
    const uint32_t avail = (sizeBytes + 15) & ~15u;
    if (avail == 0) { g_vbs.erase(key); return nullptr; }
    if (!g_noVbRing && ((!e.buf && e.frames < kVbPromoteAfter) || (e.buf && e.hot)))
    {
        ID3D11ShaderResourceView* r = VbRingCopy(src, std::min(rangeStart, avail), std::min(std::max(rangeEnd, rangeStart), avail), avail, baseDw);
        if (r) return r;
    }
    if (!e.buf)
    {''', 'vb lookup')

rep('''        g_ev.vbPagesCmp++;
        if (memcmp(e.shadow.data() + o0, src + o0, o1 - o0) != 0)
        {
            g_ev.vbPagesChanged++; g_ev.vbBytesUp += o1 - o0;
            D3D11_BOX box = { o0, 0, 0, o1, 1, 1 };
            g_ctx->UpdateSubresource(e.buf.get(), 0, &box, src + o0, 0, 0);
            memcpy(e.shadow.data() + o0, src + o0, o1 - o0);
            e.pageStable[pg] = 0;
        }
        else if (e.pageStable[pg] < 255) e.pageStable[pg]++;
    }
    return e.srv.get();
}''', '''        g_ev.vbPagesCmp++;
        if (memcmp(e.shadow.data() + o0, src + o0, o1 - o0) != 0)
        {
            g_ev.vbPagesChanged++; g_ev.vbBytesUp += o1 - o0;
            D3D11_BOX box = { o0, 0, 0, o1, 1, 1 };
            g_ctx->UpdateSubresource(e.buf.get(), 0, &box, src + o0, 0, 0);
            memcpy(e.shadow.data() + o0, src + o0, o1 - o0);
            e.pageStable[pg] = 0;
            if (e.lastChangeFrame != g_frame)
            {
                // Data that changes in three consecutive frames is dynamic: the used range goes through the ring for a while instead of page compares + uploads.
                e.changeStreak = (e.lastChangeFrame == g_frame - 1) ? e.changeStreak + 1 : 1;
                e.lastChangeFrame = g_frame;
                if (!g_noVbRing && e.changeStreak >= 3 && rangeEnd - rangeStart <= (kVbRing / 8)) e.hot = 180;
            }
        }
        else if (e.pageStable[pg] < 255) e.pageStable[pg]++;
    }
    return e.srv.get();
}''', 'vb hot')

rep('''            srv = GetVertexBuffer(d0 & ~3u, ((d1 >> 2) & 0xFFFFFF) * 4, (uint32_t)r0, (uint32_t)r1);''',
    '''            srv = GetVertexBuffer(d0 & ~3u, ((d1 >> 2) & 0xFFFFFF) * 4, (uint32_t)r0, (uint32_t)r1, &g_vfBaseDw[slot]);''', 'call site')
rep('''        ID3D11ShaderResourceView* srv = nullptr;
        if ((d0 & 3) == 3)
        {
            const uint64_t tv = QpcNow();''', '''        ID3D11ShaderResourceView* srv = nullptr;
        g_vfBaseDw[slot] = 0;
        if ((d0 & 3) == 3)
        {
            const uint64_t tv = QpcNow();''', 'call site reset')

# the ring buffer and the constant-buffer ring are created at init
rep('''        cb.ByteWidth = g_cbOffsets ? kCbDrawEntries * 256 : 16;
        g_dev->CreateBuffer(&cb, nullptr, g_cbDraw.put());''', '''        cb.ByteWidth = g_cbOffsets ? kCbDrawEntries * 256 : 16;
        g_dev->CreateBuffer(&cb, nullptr, g_cbDraw.put());
        if (g_cbOffsets && !g_noCbRing)
        {
            cb.ByteWidth = kCbRingEntries * kCbRingStride;
            if (FAILED(g_dev->CreateBuffer(&cb, nullptr, g_cbRing.put()))) { g_cbRing.reset(); Log("[gpu] constant buffer ring creation failed: per-upload DISCARD instead"); }
        }
    }
    {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = kVbRing; d.Usage = D3D11_USAGE_DYNAMIC; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (SUCCEEDED(g_dev->CreateBuffer(&d, nullptr, g_vbRing.put())))
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format = DXGI_FORMAT_R32_TYPELESS; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX; sv.BufferEx.FirstElement = 0; sv.BufferEx.NumElements = kVbRing / 4; sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            if (FAILED(g_dev->CreateShaderResourceView(g_vbRing.get(), &sv, g_vbRingSRV.put()))) g_vbRing.reset();
        }
        if (!g_vbRing) Log("[gpu] vertex ring creation failed: persistent buffers only");''', 'init rings')

# ---- constant buffer ring (b0) ------------------------------------------------------------------------------------------------------------------
rep('''uint64_t g_cbUploadsV = 0, g_cbUploadsP = 0;''', '''uint64_t g_cbUploadsV = 0, g_cbUploadsP = 0;
// The big constant buffers (b0, 5968 bytes) go through one 48 MB dynamic ring as well (NO_OVERWRITE maps, bound with offsets) when the device supports it.
CP<ID3D11Buffer> g_cbRing;
const uint32_t kCbRingStride = 6144, kCbRingEntries = 8192;   // 384 constants per entry
uint32_t g_cbRingPos = 0;
UINT g_cbRingFirstV = 0, g_cbRingFirstP = 0;''', 'cb ring decl')

rep('''void BindDrawConstants(uint32_t startVertex)
{''', '''// Uploads a constant block into the ring and binds it to b0 of the stage; returns false when the ring is not available.
bool UploadCBRing(bool pixel, const XeCB& cb)
{
    if (!g_cbRing) return false;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(g_cbRing.get(), 0, g_cbRingPos == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m))) return false;
    memcpy((uint8_t*)m.pData + (size_t)g_cbRingPos * kCbRingStride, &cb, sizeof cb);
    g_ctx->Unmap(g_cbRing.get(), 0);
    ID3D11Buffer* b = g_cbRing.get();
    const UINT first = g_cbRingPos * (kCbRingStride / 16), num = kCbRingStride / 16;
    if (pixel) { g_ctx1->PSSetConstantBuffers1(0, 1, &b, &first, &num); g_cbRingFirstP = first; g_bound.psCb = b; }
    else { g_ctx1->VSSetConstantBuffers1(0, 1, &b, &first, &num); g_cbRingFirstV = first; g_bound.vsCb = b; }
    g_cbRingPos = (g_cbRingPos + 1) % kCbRingEntries;
    return true;
}
// Re-binds the last uploaded ring blocks after something else touched the pipeline (InvalidateBound).
void RebindCBRing(bool pixel)
{
    ID3D11Buffer* b = g_cbRing.get();
    const UINT num = kCbRingStride / 16;
    if (pixel) { g_ctx1->PSSetConstantBuffers1(0, 1, &b, &g_cbRingFirstP, &num); g_bound.psCb = b; }
    else { g_ctx1->VSSetConstantBuffers1(0, 1, &b, &g_cbRingFirstV, &num); g_bound.vsCb = b; }
}

void BindDrawConstants(uint32_t startVertex)
{''', 'cb ring funcs')

# g_bound is declared after the helpers? check order at patch time: BoundState is defined in the "Redundant-state filter" section which precedes
# the index-list section and BuildIndices; UploadCBRing sits just before BindDrawConstants (defined before BuildIndices) - verify below.

rep('''    if (refillV) FillConstants(false, vs->info, cbv, rtW, rtH);
    if (refillP) FillConstants(true, ps->info, cbp, rtW, rtH);
''', '''    if (refillV) { FillConstants(false, vs->info, cbv, rtW, rtH); for (uint32_t slot : vs->info.vfetchSlots) cbv.vfmeta[slot][0] = g_vfBaseDw[slot]; }
    if (refillP) FillConstants(true, ps->info, cbp, rtW, rtH);
''', 'vfmeta override')
rep('''    const bool refillV = g_noCbDedupe || !haveV || (g_regDirty & (kDirtyVsFloat | kDirtyBoolLoop | kDirtyMisc)) || lastVsHash != g_curVS.hash || lastRtW != rtW || lastRtH != rtH
                         || memcmp(lastCompsV, g_slotComps, sizeof lastCompsV) != 0 || fetchChanged(lastFetchV, vs, true);''',
    '''    static uint32_t lastVfBase[96];
    auto vfBaseChanged = [&]() { for (uint32_t slot : vs->info.vfetchSlots) if (lastVfBase[slot] != g_vfBaseDw[slot]) return true; return false; };
    const bool refillV = g_noCbDedupe || !haveV || (g_regDirty & (kDirtyVsFloat | kDirtyBoolLoop | kDirtyMisc)) || lastVsHash != g_curVS.hash || lastRtW != rtW || lastRtH != rtH
                         || memcmp(lastCompsV, g_slotComps, sizeof lastCompsV) != 0 || fetchChanged(lastFetchV, vs, true) || vfBaseChanged();''', 'refill vf')
rep('''    if (refillV) { PatchSwizzles(cbv); UploadCB(g_cbVS.get(), cbv); haveV = true; lastVsHash = g_curVS.hash; lastRtW = rtW; lastRtH = rtH; memcpy(lastCompsV, g_slotComps, sizeof lastCompsV); memcpy(lastFetchV, g_fetchVer, sizeof lastFetchV); g_cbUploadsV++; }
    if (refillP) { PatchSwizzles(cbp); UploadCB(g_cbPS.get(), cbp); haveP = true; memcpy(lastCompsP, g_slotComps, sizeof lastCompsP); memcpy(lastFetchP, g_fetchVer, sizeof lastFetchP); g_cbUploadsP++; }
    g_regDirty = 0;
    if (g_noFilter || g_bound.vsCb != g_cbVS.get()) { ID3D11Buffer* b = g_cbVS.get(); g_ctx->VSSetConstantBuffers(0, 1, &b); g_bound.vsCb = b; }
    if (g_noFilter || g_bound.psCb != g_cbPS.get()) { ID3D11Buffer* b = g_cbPS.get(); g_ctx->PSSetConstantBuffers(0, 1, &b); g_bound.psCb = b; }''',
    '''    if (refillV) { PatchSwizzles(cbv); if (!UploadCBRing(false, cbv)) UploadCB(g_cbVS.get(), cbv); haveV = true; lastVsHash = g_curVS.hash; lastRtW = rtW; lastRtH = rtH; memcpy(lastCompsV, g_slotComps, sizeof lastCompsV); memcpy(lastFetchV, g_fetchVer, sizeof lastFetchV); memcpy(lastVfBase, g_vfBaseDw, sizeof lastVfBase); g_cbUploadsV++; }
    if (refillP) { PatchSwizzles(cbp); if (!UploadCBRing(true, cbp)) UploadCB(g_cbPS.get(), cbp); haveP = true; memcpy(lastCompsP, g_slotComps, sizeof lastCompsP); memcpy(lastFetchP, g_fetchVer, sizeof lastFetchP); g_cbUploadsP++; }
    g_regDirty = 0;
    if (g_cbRing)
    {
        if (g_noFilter || g_bound.vsCb != g_cbRing.get()) RebindCBRing(false);
        if (g_noFilter || g_bound.psCb != g_cbRing.get()) RebindCBRing(true);
    }
    else
    {
        if (g_noFilter || g_bound.vsCb != g_cbVS.get()) { ID3D11Buffer* b = g_cbVS.get(); g_ctx->VSSetConstantBuffers(0, 1, &b); g_bound.vsCb = b; }
        if (g_noFilter || g_bound.psCb != g_cbPS.get()) { ID3D11Buffer* b = g_cbPS.get(); g_ctx->PSSetConstantBuffers(0, 1, &b); g_bound.psCb = b; }
    }''', 'upload ring')

# ---- stats ---------------------------------------------------------------------------------------------------------------------------------------
rep('''            Log("[stat]   events: tex hashed +%llu created +%llu updated +%llu (%llu KB decoded) | vb created +%llu pages compared +%llu changed +%llu uploaded %llu KB | rt created +%llu cpu resolves +%llu flushes +%llu shader compiles +%llu",
                (unsigned long long)(g_ev.texHashed - pv.texHashed), (unsigned long long)(g_ev.texCreated - pv.texCreated), (unsigned long long)(g_ev.texUpdated - pv.texUpdated), (unsigned long long)((g_ev.texBytes - pv.texBytes) / 1024),
                (unsigned long long)(g_ev.vbCreated - pv.vbCreated), (unsigned long long)(g_ev.vbPagesCmp - pv.vbPagesCmp), (unsigned long long)(g_ev.vbPagesChanged - pv.vbPagesChanged), (unsigned long long)((g_ev.vbBytesUp - pv.vbBytesUp) / 1024),
                (unsigned long long)(g_ev.rtCreated - pv.rtCreated), (unsigned long long)(g_ev.cpuResolves - pv.cpuResolves), (unsigned long long)(g_ev.flushes - pv.flushes), (unsigned long long)(g_ev.shaderCompiles - pv.shaderCompiles));''',
    '''            Log("[stat]   events: tex hashed +%llu created +%llu updated +%llu (%llu KB decoded) memo misses +%llu | vb created +%llu pages compared +%llu changed +%llu uploaded %llu KB ring copies +%llu (%llu KB) | rt created +%llu cpu resolves +%llu flushes +%llu shader compiles +%llu",
                (unsigned long long)(g_ev.texHashed - pv.texHashed), (unsigned long long)(g_ev.texCreated - pv.texCreated), (unsigned long long)(g_ev.texUpdated - pv.texUpdated), (unsigned long long)((g_ev.texBytes - pv.texBytes) / 1024), (unsigned long long)(g_ev.texMemoMiss - pv.texMemoMiss),
                (unsigned long long)(g_ev.vbCreated - pv.vbCreated), (unsigned long long)(g_ev.vbPagesCmp - pv.vbPagesCmp), (unsigned long long)(g_ev.vbPagesChanged - pv.vbPagesChanged), (unsigned long long)((g_ev.vbBytesUp - pv.vbBytesUp) / 1024),
                (unsigned long long)(g_ev.vbRingCopies - pv.vbRingCopies), (unsigned long long)((g_ev.vbRingBytes - pv.vbRingBytes) / 1024),
                (unsigned long long)(g_ev.rtCreated - pv.rtCreated), (unsigned long long)(g_ev.cpuResolves - pv.cpuResolves), (unsigned long long)(g_ev.flushes - pv.flushes), (unsigned long long)(g_ev.shaderCompiles - pv.shaderCompiles));''', 'stats')
rep('''    ID3D11ShaderResourceView* r = GetTextureSRVSlow(slot, shaderDim, tf);
    const uint32_t gen = g_resolvedGen;''', '''    g_ev.texMemoMiss++;
    ID3D11ShaderResourceView* r = GetTextureSRVSlow(slot, shaderDim, tf);
    const uint32_t gen = g_resolvedGen;''', 'memo miss')
open(p, 'w', encoding='utf-8').write(s)

# ---- shader: the fetch base is a dword offset added in dword arithmetic ---------------------------------------------------------------------------
p2 = 'C:/itc-work/runtime/src/xgpu_shader.cpp'
t = open(p2, encoding='utf-8').read()
old = '''            line("vfAddr = (vfBase%u >> 2) + (uint)xe_idx * %uu;", slot, stride);'''
new = '''            line("vfAddr = vfBase%u + (uint)xe_idx * %uu;", slot, stride);   // vfBase: dword offset (may wrap: the sum is exact inside the buffer)'''
if t.count(old) != 1:
    raise SystemExit('shader vfAddr anchor')
t = t.replace(old, new)
old2 = '''#define vfBase%u vfmeta[%u].x\\n'''
if t.count(old2) != 1:
    raise SystemExit('shader define anchor')
open(p2, 'w', encoding='utf-8').write(t)
print('patched part 7')
