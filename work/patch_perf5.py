p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what, count=1):
    global s
    n = s.count(old)
    if n != count:
        raise SystemExit('count %d != %d: %s' % (n, count, what))
    s = s.replace(old, new)


# ---- hitch diagnostics: per-frame event counters, slow frames are logged with what happened in them ----------------------------------------
rep('''uint64_t g_drawCount = 0, g_resolveCount = 0, g_shaderFail = 0, g_shaderOk = 0, g_skippedDraws = 0, g_shaderPending = 0, g_shaderCacheHits = 0;''',
    '''uint64_t g_drawCount = 0, g_resolveCount = 0, g_shaderFail = 0, g_shaderOk = 0, g_skippedDraws = 0, g_shaderPending = 0, g_shaderCacheHits = 0;
// Event counters for the hitch diagnostics (see XGpuSwap: a frame slower than ITC_SLOW_MS, default 45 ms, is logged with the deltas of these).
struct FrameEvents { uint64_t texHashed, texCreated, texUpdated, texBytes, vbPagesCmp, vbPagesChanged, vbBytesUp, vbCreated, rtCreated, cpuResolves, flushes, shaderCompiles; };
FrameEvents g_ev = {};''', 'counters')

rep('''    if (ct.tex && ct.w == dec.w && ct.h == dec.h && ct.fmt == dec.fmt && ct.levels == nLevels)
    {
        { ProfScope pu(g_profUpload); g_ctx->UpdateSubresource(ct.tex.get(), 0, nullptr, dec.data.data(), dec.rowPitch, 0);''',
    '''    g_ev.texBytes += dec.data.size();
    if (ct.tex && ct.w == dec.w && ct.h == dec.h && ct.fmt == dec.fmt && ct.levels == nLevels)
    {
        g_ev.texUpdated++;
        { ProfScope pu(g_profUpload); g_ctx->UpdateSubresource(ct.tex.get(), 0, nullptr, dec.data.data(), dec.rowPitch, 0);''', 'tex update')
rep('''    CP<ID3D11Texture2D> t;
    HRESULT chr = g_dev->CreateTexture2D(&d, sd, t.put());''', '''    CP<ID3D11Texture2D> t;
    g_ev.texCreated++;
    HRESULT chr = g_dev->CreateTexture2D(&d, sd, t.put());''', 'tex create')
rep('''        Decoded unused;
        { ProfScope ps(g_profHash); DecodeTexture(tf, unused, true, &hash); if (ml.levels > 1) hash = (hash ^ HashMipRegion(ml)) * 0x100000001B3ull; }
        ct.checkedFrame = g_frame; ct.checkedSeq = g_memSeq;
        if (hash == ct.dataHash && ct.levels == ml.levels) { if (ct.stable < 255) ct.stable++;''', '''        Decoded unused;
        g_ev.texHashed++;
        { ProfScope ps(g_profHash); DecodeTexture(tf, unused, true, &hash); if (ml.levels > 1) hash = (hash ^ HashMipRegion(ml)) * 0x100000001B3ull; }
        ct.checkedFrame = g_frame; ct.checkedSeq = g_memSeq;
        if (hash == ct.dataHash && ct.levels == ml.levels) { if (ct.stable < 255) ct.stable++;''', 'tex hash')
rep('''        D3D11_SUBRESOURCE_DATA sd = { src, 0, 0 };
        if (FAILED(g_dev->CreateBuffer(&d, &sd, e.buf.put()))) { g_vbs.erase(key); return nullptr; }''', '''        D3D11_SUBRESOURCE_DATA sd = { src, 0, 0 };
        g_ev.vbCreated++; g_ev.vbBytesUp += avail;
        if (FAILED(g_dev->CreateBuffer(&d, &sd, e.buf.put()))) { g_vbs.erase(key); return nullptr; }''', 'vb create')
rep('''        e.pageStamp[pg] = g_frame;
        const uint32_t o0 = pg * 4096, o1 = std::min(o0 + 4096, avail);
        if (memcmp(e.shadow.data() + o0, src + o0, o1 - o0) != 0)
        {
            D3D11_BOX box = { o0, 0, 0, o1, 1, 1 };''', '''        e.pageStamp[pg] = g_frame;
        const uint32_t o0 = pg * 4096, o1 = std::min(o0 + 4096, avail);
        g_ev.vbPagesCmp++;
        if (memcmp(e.shadow.data() + o0, src + o0, o1 - o0) != 0)
        {
            g_ev.vbPagesChanged++; g_ev.vbBytesUp += o1 - o0;
            D3D11_BOX box = { o0, 0, 0, o1, 1, 1 };''', 'vb page')
rep('''    CP<ID3D11Texture2D> t;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, t.put()))) { Log("[gpu] RT create failed %ux%u fmt %u", nw, nh, fmt); return nullptr; }''', '''    CP<ID3D11Texture2D> t;
    g_ev.rtCreated++;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, t.put()))) { Log("[gpu] RT create failed %ux%u fmt %u", nw, nh, fmt); return nullptr; }''', 'rt create')
rep('''    if (!gpuDone && rt && x1 > x0 && y1 > y0 && destPitch)
    {
        static int cpuLogged = 0;''', '''    if (!gpuDone && rt && x1 > x0 && y1 > y0 && destPitch)
    {
        g_ev.cpuResolves++;
        static int cpuLogged = 0;''', 'cpu resolve')
rep('''    g_flushCount++; g_flushBytes += r.bytes;''', '''    g_flushCount++; g_flushBytes += r.bytes; g_ev.flushes++;''', 'flush')
rep('''    CreateDirectoryA(cacheDir, nullptr);
    auto job = std::make_shared<CompileJob>();''', '''    CreateDirectoryA(cacheDir, nullptr);
    g_ev.shaderCompiles++;
    auto job = std::make_shared<CompileJob>();''', 'compile')

# the slow-frame log in XGpuSwap (right where the frame time is measured)
rep('''        const uint64_t now = QpcNow();
        if (lastSwapT) worstMs = std::max(worstMs, (double)(now - lastSwapT) * 1000.0 / (double)qf.QuadPart);
        lastSwapT = now; winFrames++;''', '''        const uint64_t now = QpcNow();
        if (lastSwapT) worstMs = std::max(worstMs, (double)(now - lastSwapT) * 1000.0 / (double)qf.QuadPart);
        {
            // Hitch diagnostics: a frame slower than ITC_SLOW_MS (default 45) is logged together with what happened in it (at most 20 per 2 s window).
            static const double slowMs = getenv("ITC_SLOW_MS") ? atof(getenv("ITC_SLOW_MS")) : 45.0;
            static FrameEvents prev = {}; static uint64_t prevDraws = 0, prevDecodes = 0, prevSkipped = 0, prevDrawTicks = 0, prevIdxMiss = 0, prevResolves = 0;
            static int logged = 0; static uint32_t logWin = 0;
            const double ms = lastSwapT ? (double)(now - lastSwapT) * 1000.0 / (double)qf.QuadPart : 0.0;
            if (g_frame / 60 != logWin) { logWin = g_frame / 60; logged = 0; }
            if (ms > slowMs && logged++ < 20)
                Log("[slow] frame %u: %.0f ms (draw thread %.0f ms): draws %llu skipped +%llu resolves +%llu | decodes +%llu texCreated +%llu texUpdated +%llu texHashed +%llu (%llu KB) | vb created +%llu pagesCmp +%llu changed +%llu up %llu KB | rtCreated +%llu cpuResolves +%llu flushes +%llu shaderCompiles +%llu idxMiss +%llu pendingShaders %llu",
                    g_frame, ms, (double)(g_profDraw - prevDrawTicks) * 1000.0 / (double)qf.QuadPart, (unsigned long long)(g_drawCount - prevDraws), (unsigned long long)(g_skippedDraws - prevSkipped), (unsigned long long)(g_resolveCount - prevResolves),
                    (unsigned long long)(g_decodeCount - prevDecodes), (unsigned long long)(g_ev.texCreated - prev.texCreated), (unsigned long long)(g_ev.texUpdated - prev.texUpdated), (unsigned long long)(g_ev.texHashed - prev.texHashed), (unsigned long long)((g_ev.texBytes - prev.texBytes) / 1024),
                    (unsigned long long)(g_ev.vbCreated - prev.vbCreated), (unsigned long long)(g_ev.vbPagesCmp - prev.vbPagesCmp), (unsigned long long)(g_ev.vbPagesChanged - prev.vbPagesChanged), (unsigned long long)((g_ev.vbBytesUp - prev.vbBytesUp) / 1024),
                    (unsigned long long)(g_ev.rtCreated - prev.rtCreated), (unsigned long long)(g_ev.cpuResolves - prev.cpuResolves), (unsigned long long)(g_ev.flushes - prev.flushes), (unsigned long long)(g_ev.shaderCompiles - prev.shaderCompiles), (unsigned long long)(g_idxMisses - prevIdxMiss), (unsigned long long)g_shaderPending);
            prev = g_ev; prevDraws = g_drawCount; prevDecodes = g_decodeCount; prevSkipped = g_skippedDraws; prevDrawTicks = g_profDraw; prevIdxMiss = g_idxMisses; prevResolves = g_resolveCount;
        }
        lastSwapT = now; winFrames++;''', 'slow log')

# per-window summary of the same counters
rep('''        { static uint64_t le = 0, lre = 0; Log("[stat]   environment pass:''', '''        {
            static FrameEvents pv = {};
            Log("[stat]   events: tex hashed +%llu created +%llu updated +%llu (%llu KB decoded) | vb created +%llu pages compared +%llu changed +%llu uploaded %llu KB | rt created +%llu cpu resolves +%llu flushes +%llu shader compiles +%llu",
                (unsigned long long)(g_ev.texHashed - pv.texHashed), (unsigned long long)(g_ev.texCreated - pv.texCreated), (unsigned long long)(g_ev.texUpdated - pv.texUpdated), (unsigned long long)((g_ev.texBytes - pv.texBytes) / 1024),
                (unsigned long long)(g_ev.vbCreated - pv.vbCreated), (unsigned long long)(g_ev.vbPagesCmp - pv.vbPagesCmp), (unsigned long long)(g_ev.vbPagesChanged - pv.vbPagesChanged), (unsigned long long)((g_ev.vbBytesUp - pv.vbBytesUp) / 1024),
                (unsigned long long)(g_ev.rtCreated - pv.rtCreated), (unsigned long long)(g_ev.cpuResolves - pv.cpuResolves), (unsigned long long)(g_ev.flushes - pv.flushes), (unsigned long long)(g_ev.shaderCompiles - pv.shaderCompiles));
            pv = g_ev;
        }
        { static uint64_t le = 0, lre = 0; Log("[stat]   environment pass:''', 'window stats')
open(p, 'w', encoding='utf-8').write(s)
print('patched part 5 (diagnostics)')
