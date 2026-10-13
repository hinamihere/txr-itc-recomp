p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what, count=1):
    global s
    n = s.count(old)
    if n != count:
        raise SystemExit('count %d != %d: %s' % (n, count, what))
    s = s.replace(old, new)


# ---- Map stall accounting -------------------------------------------------------------------------------------------------------------------
rep('''uint64_t g_profMap = 0, g_profDecode = 0, g_profResolve = 0, g_profSwap = 0, g_profDraw = 0, g_profHash = 0, g_profShader = 0, g_profPresent = 0;''',
    '''uint64_t g_profMap = 0, g_profDecode = 0, g_profResolve = 0, g_profSwap = 0, g_profDraw = 0, g_profHash = 0, g_profShader = 0, g_profPresent = 0;
uint64_t g_mapStall = 0, g_mapStallCount = 0;   // Map calls on the dynamic rings that took longer than 1 ms (the driver waited for the GPU / allocated a new instance)''', 'stall decl')
rep('''inline uint64_t QpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }''',
    '''inline uint64_t QpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }
extern CP<ID3D11DeviceContext> g_ctx;
// Map of a dynamic ring buffer with stall accounting (see g_mapStall).
inline HRESULT MapTimed(ID3D11Buffer* b, D3D11_MAP mode, D3D11_MAPPED_SUBRESOURCE* m, const char* what)
{
    const uint64_t t0 = QpcNow();
    const HRESULT hr = g_ctx->Map(b, 0, mode, 0, m);
    const uint64_t dt = QpcNow() - t0;
    static uint64_t oneMs = 0; if (!oneMs) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); oneMs = (uint64_t)f.QuadPart / 1000; }
    if (dt > oneMs)
    {
        g_mapStall += dt; g_mapStallCount++;
        static int logged = 0;
        if (dt > 5 * oneMs && logged++ < 30) Log("[gpu] Map(%s, %s) stalled %.1f ms", what, mode == D3D11_MAP_WRITE_DISCARD ? "DISCARD" : "NO_OVERWRITE", (double)dt / oneMs);
    }
    return hr;
}''', 'maptimed')
# g_ctx is declared later in the file (CP<ID3D11DeviceContext> g_ctx;) inside the same anonymous namespace: the extern declaration above must match
rep('''CP<ID3D11DeviceContext> g_ctx;
CP<IDXGISwapChain> g_swap;''', '''CP<ID3D11DeviceContext> g_ctx;   // (declared extern above for MapTimed)
CP<IDXGISwapChain> g_swap;''', 'ctx decl')

rep('''    if (FAILED(g_ctx->Map(g_cbRing.get(), 0, g_cbRingPos == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m))) return false;''',
    '''    if (FAILED(MapTimed(g_cbRing.get(), g_cbRingPos == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, &m, "constant ring"))) return false;''', 'cb ring map')
rep('''        if (FAILED(g_ctx->Map(b, 0, g_cbDrawPos == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m))) return;''',
    '''        if (FAILED(MapTimed(b, g_cbDrawPos == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, &m, "draw constants"))) return;''', 'b1 map')
rep('''    if (FAILED(g_ctx->Map(g_vbRing.get(), 0, discard ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m))) return nullptr;''',
    '''    if (FAILED(MapTimed(g_vbRing.get(), discard ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, &m, "vertex ring"))) return nullptr;''', 'vb ring map')
rep('''            hr = g_ctx->Map(g_idxBuf.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
            g_idxPos = 0; g_idxCache.clear(); g_idxFirst = false;''', '''            hr = MapTimed(g_idxBuf.get(), D3D11_MAP_WRITE_DISCARD, &m, "index buffer");
            g_idxPos = 0; g_idxCache.clear(); g_idxFirst = false;''', 'idx discard map')
rep('''            hr = g_ctx->Map(g_idxBuf.get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m);
            if (FAILED(hr) || !m.pData)''', '''            hr = MapTimed(g_idxBuf.get(), D3D11_MAP_WRITE_NO_OVERWRITE, &m, "index buffer");
            if (FAILED(hr) || !m.pData)''', 'idx nooverwrite map')

# ---- the slow-frame log also shows the Present block and the map stalls of that frame -----------------------------------------------------------
rep('''            static FrameEvents prev = {}; static uint64_t prevDraws = 0, prevDecodes = 0, prevSkipped = 0, prevDrawTicks = 0, prevIdxMiss = 0, prevResolves = 0;''',
    '''            static FrameEvents prev = {}; static uint64_t prevDraws = 0, prevDecodes = 0, prevSkipped = 0, prevDrawTicks = 0, prevIdxMiss = 0, prevResolves = 0, prevPresent = 0, prevStall = 0, prevDumped = 0;''', 'slow statics')
rep('''            if (ms > slowMs && logged++ < 20)
                Log("[slow] frame %u: %.0f ms (draw thread %.0f ms): draws %llu skipped +%llu resolves +%llu | decodes +%llu texCreated +%llu texUpdated +%llu texHashed +%llu (%llu KB) | vb created +%llu pagesCmp +%llu changed +%llu up %llu KB | rtCreated +%llu cpuResolves +%llu flushes +%llu shaderCompiles +%llu idxMiss +%llu pendingShaders %llu",
                    g_frame, ms, (double)(g_profDraw - prevDrawTicks) * 1000.0 / (double)qf.QuadPart, (unsigned long long)(g_drawCount - prevDraws), (unsigned long long)(g_skippedDraws - prevSkipped), (unsigned long long)(g_resolveCount - prevResolves),''',
    '''            if (ms > slowMs && logged++ < 20)
                Log("[slow] frame %u: %.0f ms (draw thread %.0f ms, present %.1f ms, map stalls %.1f ms%s): draws %llu skipped +%llu resolves +%llu | decodes +%llu texCreated +%llu texUpdated +%llu texHashed +%llu (%llu KB) | vb created +%llu pagesCmp +%llu changed +%llu up %llu KB | rtCreated +%llu cpuResolves +%llu flushes +%llu shaderCompiles +%llu idxMiss +%llu pendingShaders %llu",
                    g_frame, ms, (double)(g_profDraw - prevDrawTicks) * 1000.0 / (double)qf.QuadPart, (double)(g_profPresent - prevPresent) * 1000.0 / (double)qf.QuadPart, (double)(g_mapStall - prevStall) * 1000.0 / (double)qf.QuadPart, g_dumped != prevDumped ? ", PNG dump" : "",
                    (unsigned long long)(g_drawCount - prevDraws), (unsigned long long)(g_skippedDraws - prevSkipped), (unsigned long long)(g_resolveCount - prevResolves),''', 'slow log')
rep('''            prev = g_ev; prevDraws = g_drawCount; prevDecodes = g_decodeCount; prevSkipped = g_skippedDraws; prevDrawTicks = g_profDraw; prevIdxMiss = g_idxMisses; prevResolves = g_resolveCount;''',
    '''            prev = g_ev; prevDraws = g_drawCount; prevDecodes = g_decodeCount; prevSkipped = g_skippedDraws; prevDrawTicks = g_profDraw; prevIdxMiss = g_idxMisses; prevResolves = g_resolveCount; prevPresent = g_profPresent; prevStall = g_mapStall; prevDumped = g_dumped;''', 'slow prev')
rep('''        Log("[stat]   cp thread: cpu %.1f ms/frame, blocked in Present %.1f ms/frame, draws %.1f ms/frame (%llu frames, %.0f%% of wall time busy)", (double)(cpu - lastCpu) * 1e-4 / nf, (g_profPresent - lastPresent) * ms / nf, (g_profDraw - lastDrawT) * ms / nf,
            (unsigned long long)nf, 100.0 * (double)(cpu - lastCpu) * 1e-7 / seconds);
        lastCpu = cpu; lastFrames = g_frame; lastPresent = g_profPresent; lastDrawT = g_profDraw;''',
    '''        static uint64_t lastStall = 0, lastStallN = 0;
        Log("[stat]   cp thread: cpu %.1f ms/frame, blocked in Present %.1f ms/frame, draws %.1f ms/frame, map stalls %.1f ms/frame (%llu) (%llu frames, %.0f%% of wall time busy)", (double)(cpu - lastCpu) * 1e-4 / nf, (g_profPresent - lastPresent) * ms / nf, (g_profDraw - lastDrawT) * ms / nf,
            (g_mapStall - lastStall) * ms / nf, (unsigned long long)(g_mapStallCount - lastStallN), (unsigned long long)nf, 100.0 * (double)(cpu - lastCpu) * 1e-7 / seconds);
        lastCpu = cpu; lastFrames = g_frame; lastPresent = g_profPresent; lastDrawT = g_profDraw; lastStall = g_mapStall; lastStallN = g_mapStallCount;''', 'cp stat')

# ---- vertex buffer creations are spread over frames ------------------------------------------------------------------------------------------
rep('''    const uint32_t promoteAfter = (std::max(rangeEnd, rangeStart) - std::min(rangeStart, avail)) > 16384 ? kVbPromoteAfter : 30;
    if (!g_noVbRing && ((!e.buf && e.frames < promoteAfter) || (e.buf && e.hot)))
    {''', '''    const uint32_t promoteAfter = (std::max(rangeEnd, rangeStart) - std::min(rangeStart, avail)) > 16384 ? kVbPromoteAfter : 30;
    // At most kVbCreateBudget persistent buffers are created per frame (a burst of promotions, e.g. 124 in one frame at a race start, costs ~10 ms); the others wait in the ring.
    static uint32_t budgetFrame = 0xFFFFFFFFu, created = 0;
    if (budgetFrame != g_frame) { budgetFrame = g_frame; created = 0; }
    const bool budgetLeft = created < kVbCreateBudget;
    if (!g_noVbRing && ((!e.buf && (e.frames < promoteAfter || !budgetLeft)) || (e.buf && e.hot)))
    {''', 'budget')
rep('''const uint32_t kVbPromoteAfter = 3;''', '''const uint32_t kVbPromoteAfter = 3;
const uint32_t kVbCreateBudget = 12;''', 'budget const')
rep('''        D3D11_SUBRESOURCE_DATA sd = { src, 0, 0 };
        g_ev.vbCreated++; g_ev.vbBytesUp += avail;''', '''        D3D11_SUBRESOURCE_DATA sd = { src, 0, 0 };
        g_ev.vbCreated++; g_ev.vbBytesUp += avail; created++;''', 'budget count')
open(p, 'w', encoding='utf-8').write(s)
print('patched part 12')
