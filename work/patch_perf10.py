p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what, count=1):
    global s
    n = s.count(old)
    if n != count:
        raise SystemExit('count %d != %d: %s' % (n, count, what))
    s = s.replace(old, new)


# ---- (A) two-stage shader compiles: a quick unoptimised build first, the optimised one replaces it when ready ---------------------------------
rep('''bool CompileHlsl(const std::string& src, const char* target, const char* tag, ID3DBlob** blob)
{
    CP<ID3DBlob> err;
    // The optimizer rejects some of the big branchy state-machine shaders with X4000 ("variable 'r' used without having been completely
    // initialized"); the same source compiles with a different flow-control strategy, so retry down a ladder of flag sets before giving up.
    static const bool shaderDebug = getenv("ITC_SHADER_DEBUG") != nullptr;
    const UINT firstFlags = ShaderCompileFlags();
    const UINT kFlagLadder[] = { firstFlags, D3DCOMPILE_PREFER_FLOW_CONTROL, D3DCOMPILE_SKIP_OPTIMIZATION };
    static const UINT kDebugLadder[] = { D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION };
    HRESULT hr = E_FAIL;
    for (int li = 0, nLadder = shaderDebug ? 1 : 3; li < nLadder; li++)
    {
        const UINT fl = (shaderDebug ? kDebugLadder : kFlagLadder)[li];''',
    '''bool CompileHlsl(const std::string& src, const char* target, const char* tag, ID3DBlob** blob, bool quick = false)
{
    CP<ID3DBlob> err;
    // The optimizer rejects some of the big branchy state-machine shaders with X4000 ("variable 'r' used without having been completely
    // initialized"); the same source compiles with a different flow-control strategy, so retry down a ladder of flag sets before giving up.
    // quick = the unoptimised first stage of a two-stage compile (about half the time of the optimised build).
    static const bool shaderDebug = getenv("ITC_SHADER_DEBUG") != nullptr;
    const UINT firstFlags = quick ? (UINT)D3DCOMPILE_SKIP_OPTIMIZATION : ShaderCompileFlags();
    const UINT kFlagLadder[] = { firstFlags, D3DCOMPILE_PREFER_FLOW_CONTROL, D3DCOMPILE_SKIP_OPTIMIZATION };
    static const UINT kDebugLadder[] = { D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION };
    HRESULT hr = E_FAIL;
    for (int li = 0, nLadder = shaderDebug || quick ? 1 : 3; li < nLadder; li++)
    {
        const UINT fl = (shaderDebug ? kDebugLadder : kFlagLadder)[li];''', 'compile quick')

rep('''struct CompileJob
{
    std::string hlsl, target, tag;
    std::string cachePath;
    std::atomic<int> state{ 0 };      // 0 = compiling, 1 = done, 2 = failed
    std::vector<uint8_t> bytecode;
};''', '''struct CompileJob
{
    std::string hlsl, target, tag;
    std::string cachePath;            // empty: the result is not persisted (quick first-stage build)
    bool quick = false;               // unoptimised build (D3DCOMPILE_SKIP_OPTIMIZATION): used until the optimised build of the same shader is ready
    std::atomic<int> state{ 0 };      // 0 = compiling, 1 = done, 2 = failed
    std::vector<uint8_t> bytecode;
};''', 'job struct')
rep('''    std::string name;                  // "ps_<hash>_<interpolators>" - shown as the object name in RenderDoc
    std::shared_ptr<CompileJob> job;   // set while the compile is in flight
};''', '''    std::string name;                  // "ps_<hash>_<interpolators>" - shown as the object name in RenderDoc
    std::shared_ptr<CompileJob> job;   // set while the compile is in flight
    std::shared_ptr<CompileJob> jobOpt;   // the optimised build that follows a quick one (swapped in when done)
};''', 'entry struct')
rep('''        CP<ID3DBlob> blob;
        if (CompileHlsl(job->hlsl, job->target.c_str(), job->tag.c_str(), blob.put()))
        {
            const uint8_t* b = (const uint8_t*)blob->GetBufferPointer();
            job->bytecode.assign(b, b + blob->GetBufferSize());
            // Persist the bytecode (write to a temporary name first so a killed process never leaves a truncated cache file).
            const std::string tmp = job->cachePath + ".tmp";
            if (FILE* f = fopen(tmp.c_str(), "wb"))''', '''        CP<ID3DBlob> blob;
        if (CompileHlsl(job->hlsl, job->target.c_str(), job->tag.c_str(), blob.put(), job->quick))
        {
            const uint8_t* b = (const uint8_t*)blob->GetBufferPointer();
            job->bytecode.assign(b, b + blob->GetBufferSize());
            // Persist the bytecode (write to a temporary name first so a killed process never leaves a truncated cache file).
            const std::string tmp = job->cachePath + ".tmp";
            if (!job->cachePath.empty()) if (FILE* f = fopen(tmp.c_str(), "wb"))''', 'worker persist')

rep('''        ShaderEntry& e = it->second;
        if (e.failed) return nullptr;
        if (!e.job) return &e;
        if (syncShaders) while (e.job->state.load() == 0) Sleep(1);
        const int st = e.job->state.load();
        if (st == 0) { g_shaderPending++; return nullptr; }
        std::shared_ptr<CompileJob> job = e.job;
        e.job.reset();
        if (st != 1 || !CreateShaderObject(e, isPixel, job->bytecode.data(), job->bytecode.size())) { e.failed = true; g_shaderFail++; return nullptr; }
        g_shaderOk++;
        return &e;
    }''', '''        ShaderEntry& e = it->second;
        if (e.failed) return nullptr;
        if (e.jobOpt && !e.job && e.jobOpt->state.load() != 0)
        {
            // The optimised build of a shader that runs as its quick build: swap it in (a failed optimised build keeps the quick one).
            std::shared_ptr<CompileJob> jo = e.jobOpt;
            e.jobOpt.reset();
            if (jo->state.load() == 1)
            {
                ShaderEntry fresh; fresh.name = e.name;
                if (CreateShaderObject(fresh, isPixel, jo->bytecode.data(), jo->bytecode.size())) { if (isPixel) e.ps = fresh.ps; else e.vs = fresh.vs; g_shaderUpgrades++; }
            }
        }
        if (!e.job) return &e;
        if (syncShaders) while (e.job->state.load() == 0) Sleep(1);
        const int st = e.job->state.load();
        if (st == 0) { g_shaderPending++; return nullptr; }
        std::shared_ptr<CompileJob> job = e.job;
        e.job.reset();
        if (st != 1 || !CreateShaderObject(e, isPixel, job->bytecode.data(), job->bytecode.size()))
        {
            if (e.jobOpt) { e.job = e.jobOpt; e.jobOpt.reset(); return GetShader(sc, isPixel); }   // the quick build failed: wait for the optimised one (it has the retry ladder)
            e.failed = true; g_shaderFail++; return nullptr;
        }
        g_shaderOk++;
        return &e;
    }''', 'getshader swap')

rep('''    CreateDirectoryA(cacheDir, nullptr);
    g_ev.shaderCompiles++;
    auto job = std::make_shared<CompileJob>();
    job->hlsl = std::move(hlsl); job->target = target; job->tag = tag; job->cachePath = cacheFile;
    e.job = job;
    QueueCompile(job);''', '''    CreateDirectoryA(cacheDir, nullptr);
    g_ev.shaderCompiles++;
    // Two-stage compile: an unoptimised build is ready in about half the time and is used first; the optimised build (the one that is cached on disk)
    // replaces it when it arrives. ITC_NO_QUICK_SHADERS=1 compiles the optimised build only.
    static const bool noQuick = getenv("ITC_NO_QUICK_SHADERS") != nullptr || ShaderCompileFlags() == (UINT)D3DCOMPILE_SKIP_OPTIMIZATION;
    auto job = std::make_shared<CompileJob>();
    job->hlsl = hlsl; job->target = target; job->tag = tag; job->cachePath = cacheFile;
    if (!noQuick)
    {
        auto quick = std::make_shared<CompileJob>();
        quick->hlsl = std::move(hlsl); quick->target = target; quick->tag = tag; quick->quick = true;
        e.job = quick; e.jobOpt = job;
        QueueCompile(quick);
    }
    else e.job = job;
    QueueCompile(job);''', 'queue two jobs')
rep('''uint64_t g_drawCount = 0, g_resolveCount = 0, g_shaderFail = 0, g_shaderOk = 0, g_skippedDraws = 0, g_shaderPending = 0, g_shaderCacheHits = 0;''',
    '''uint64_t g_drawCount = 0, g_resolveCount = 0, g_shaderFail = 0, g_shaderOk = 0, g_skippedDraws = 0, g_shaderPending = 0, g_shaderCacheHits = 0, g_shaderUpgrades = 0;''', 'upgrade counter')

# ---- (B) coalesced vertex page uploads --------------------------------------------------------------------------------------------------------------
rep('''    rangeStart = std::min(rangeStart, avail); rangeEnd = std::min(std::max(rangeEnd, rangeStart), avail);
    const bool small = avail <= 32768;
    for (uint32_t pg = rangeStart / 4096, pgEnd = (rangeEnd + 4095) / 4096; pg < pgEnd && pg < e.pageStamp.size(); pg++)
    {
        if (!small && e.pageStamp[pg] == g_frame) continue;
        if (!small && !g_forceVbCheck && e.pageStable[pg] >= kStableAfter && ((g_frame + pg * 5) & (kRecheckPeriod - 1)) != 0) continue;   // static page: not this frame
        e.pageStamp[pg] = g_frame;
        const uint32_t o0 = pg * 4096, o1 = std::min(o0 + 4096, avail);
        g_ev.vbPagesCmp++;
        if (memcmp(e.shadow.data() + o0, src + o0, o1 - o0) != 0)
        {
            g_ev.vbPagesChanged++; g_ev.vbBytesUp += o1 - o0;
            D3D11_BOX box = { o0, 0, 0, o1, 1, 1 };
            g_ctx->UpdateSubresource(e.buf.get(), 0, &box, src + o0, 0, 0);
            memcpy(e.shadow.data() + o0, src + o0, o1 - o0);
            e.pageStable[pg] = 0;''', '''    rangeStart = std::min(rangeStart, avail); rangeEnd = std::min(std::max(rangeEnd, rangeStart), avail);
    const bool small = avail <= 32768;
    // Changed pages that follow each other are uploaded with one call.
    uint32_t run0 = 0, run1 = 0;
    auto flushRun = [&]() { if (run1 > run0) { D3D11_BOX box = { run0, 0, 0, run1, 1, 1 }; g_ctx->UpdateSubresource(e.buf.get(), 0, &box, src + run0, 0, 0); memcpy(e.shadow.data() + run0, src + run0, run1 - run0); run0 = run1 = 0; } };
    for (uint32_t pg = rangeStart / 4096, pgEnd = (rangeEnd + 4095) / 4096; pg < pgEnd && pg < e.pageStamp.size(); pg++)
    {
        if (!small && e.pageStamp[pg] == g_frame) { flushRun(); continue; }
        if (!small && !g_forceVbCheck && e.pageStable[pg] >= kStableAfter && ((g_frame + pg * 5) & (kRecheckPeriod - 1)) != 0) { flushRun(); continue; }   // static page: not this frame
        e.pageStamp[pg] = g_frame;
        const uint32_t o0 = pg * 4096, o1 = std::min(o0 + 4096, avail);
        g_ev.vbPagesCmp++;
        if (memcmp(e.shadow.data() + o0, src + o0, o1 - o0) != 0)
        {
            g_ev.vbPagesChanged++; g_ev.vbBytesUp += o1 - o0;
            if (run1 == o0) run1 = o1; else { flushRun(); run0 = o0; run1 = o1; }
            e.pageStable[pg] = 0;''', 'coalesce head')
rep('''        else if (e.pageStable[pg] < 255) e.pageStable[pg]++;
    }
    return e.srv.get();
}''', '''        else { flushRun(); if (e.pageStable[pg] < 255) e.pageStable[pg]++; }
    }
    flushRun();
    return e.srv.get();
}''', 'coalesce tail')

# ---- (C) big tiled textures are untiled on several threads ------------------------------------------------------------------------------------------
rep('''    for (uint32_t by = 0; by < outBh; by++)
    {
        for (uint32_t bx = 0; bx < outBw; bx++)
        {
            const uint32_t off = tf.tiled() ? TiledByteOffset(bx, by, alignedPitch, log2bpb) : (by * pitchBlocks + bx) * fd.bpb;''', '''    auto decodeRows = [&](uint32_t byStart, uint32_t byEnd)
    {
    for (uint32_t by = byStart; by < byEnd; by++)
    {
        for (uint32_t bx = 0; bx < outBw; bx++)
        {
            const uint32_t off = tf.tiled() ? TiledByteOffset(bx, by, alignedPitch, log2bpb) : (by * pitchBlocks + bx) * fd.bpb;''', 'decode rows head')
rep('''            (void)v;
            px[0] = (uint8_t)r8; px[1] = (uint8_t)g8; px[2] = (uint8_t)b8; px[3] = (uint8_t)a8;
        }
    }
    return true;
}''', '''            (void)v;
            px[0] = (uint8_t)r8; px[1] = (uint8_t)g8; px[2] = (uint8_t)b8; px[3] = (uint8_t)a8;
        }
    }
    };
    // Big textures (>= 256 KB of output) are split over helper threads: a 1 MB texture takes ~2.5 ms single-threaded.
    static const bool noParallel = getenv("ITC_NO_PARALLEL_DECODE") != nullptr;
    const size_t outBytes = (size_t)out.rowPitch * outBh;
    if (!noParallel && outBytes >= 262144 && outBh >= 32)
    {
        const uint32_t parts = 4, rows = (outBh + parts - 1) / parts;
        std::thread helpers[parts - 1];
        for (uint32_t i = 0; i < parts - 1; i++) helpers[i] = std::thread([&, i]() { decodeRows(i * rows, std::min(outBh, (i + 1) * rows)); });
        decodeRows((parts - 1) * rows, outBh);
        for (auto& h : helpers) h.join();
    }
    else decodeRows(0, outBh);
    return true;
}''', 'decode rows tail')

# ---- (D) frame latency knob -------------------------------------------------------------------------------------------------------------------------
rep('''    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)g_backTex.put());''', '''    if (getenv("ITC_FRAME_LATENCY"))
    {
        // Maximum number of frames queued before Present blocks (default 3): a lower value reduces input latency at the price of less buffering.
        static const GUID kIID_DXGIDevice1 = { 0x77db970f, 0x6276, 0x48ba, { 0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39, 0x2c } };
        IDXGIDevice1* dd1 = nullptr;
        if (SUCCEEDED(g_dev->QueryInterface(kIID_DXGIDevice1, (void**)&dd1))) { dd1->SetMaximumFrameLatency((UINT)std::max(1, std::min(16, atoi(getenv("ITC_FRAME_LATENCY"))))); dd1->Release(); Log("[gpu] maximum frame latency set to %s", getenv("ITC_FRAME_LATENCY")); }
    }
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)g_backTex.put());''', 'latency')

# stats: shader upgrades
rep('''        Log("[stat]   index cache: hits +%llu misses +%llu resets +%llu (list=%zu), motion-vector pass draws skipped +%llu%s, constant buffer uploads vs +%llu ps +%llu", (unsigned long long)(g_idxHits - lh), (unsigned long long)(g_idxMisses - lm), (unsigned long long)(g_idxResets - lr),
            g_idxCache.size(), (unsigned long long)(g_velocitySkipped - lv), g_velocityNeeded ? " (pass active)" : "", (unsigned long long)(g_cbUploadsV - lcv), (unsigned long long)(g_cbUploadsP - lcp));
        lh = g_idxHits; lm = g_idxMisses; lr = g_idxResets; lv = g_velocitySkipped; lcv = g_cbUploadsV; lcp = g_cbUploadsP;''',
    '''        static uint64_t lup = 0;
        Log("[stat]   index cache: hits +%llu misses +%llu resets +%llu (list=%zu), motion-vector pass draws skipped +%llu%s, constant buffer uploads vs +%llu ps +%llu, shader upgrades +%llu", (unsigned long long)(g_idxHits - lh), (unsigned long long)(g_idxMisses - lm), (unsigned long long)(g_idxResets - lr),
            g_idxCache.size(), (unsigned long long)(g_velocitySkipped - lv), g_velocityNeeded ? " (pass active)" : "", (unsigned long long)(g_cbUploadsV - lcv), (unsigned long long)(g_cbUploadsP - lcp), (unsigned long long)(g_shaderUpgrades - lup));
        lh = g_idxHits; lm = g_idxMisses; lr = g_idxResets; lv = g_velocitySkipped; lcv = g_cbUploadsV; lcp = g_cbUploadsP; lup = g_shaderUpgrades;''', 'stats')
open(p, 'w', encoding='utf-8').write(s)
print('patched part 10')
