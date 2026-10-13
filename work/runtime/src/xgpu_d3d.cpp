// Xenos GPU -> Direct3D 11 backend (ITC_RENDERER=d3d11, the default; xgpu_common.cpp dispatches).
// Draws are replayed with HLSL translated from the guest's shader microcode (xgpu_shader.cpp). Render targets live in D3D
// textures; a "resolve" copies them back into guest memory (tiled, endian-swapped) exactly like the real GPU does, and
// textures / the swap frontbuffer are always decoded from guest memory. That keeps the whole path faithful and simple.
#include "xgpu.h"
#include "xgpu_shader.h"
#include "xenos_regs.h"
#include "guest.h"
#include <windows.h>
#include <nmmintrin.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include "renderdoc_app.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

bool XGpuTilingActive();      // gpu.cpp: a predicated-tiling frame is in progress
bool XGpuTilingFirstPass();   // gpu.cpp: ... and this is its first strip pass

namespace {

template <class T> struct CP
{
    T* p = nullptr;
    CP() {}
    CP(const CP& o) : p(o.p) { if (p) p->AddRef(); }
    CP& operator=(const CP& o) { if (o.p) o.p->AddRef(); if (p) p->Release(); p = o.p; return *this; }
    ~CP() { if (p) p->Release(); }
    T* operator->() const { return p; }
    T* get() const { return p; }
    T** put() { reset(); return &p; }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    explicit operator bool() const { return p != nullptr; }
};

#include "xgpu_shared.inl"   // API-independent parts shared with the Vulkan backend (xgpu_vk.cpp)


// ---- Device state ---------------------------------------------------------------------------------------
HWND g_hwnd = nullptr;
bool g_initTried = false, g_initOk = false;
CP<ID3D11Device> g_dev;
CP<ID3D11InfoQueue> g_infoQueue;
CP<ID3D11DeviceContext> g_ctx;   // (declared extern above for MapTimed)
CP<IDXGISwapChain> g_swap;
CP<ID3D11RenderTargetView> g_backRTV;
CP<ID3D11Texture2D> g_backTex;
CP<ID3D11Texture2D> g_lastGoodTex;   // copy of the last frame that was drawn completely (shown while shaders compile)
bool g_haveGood = false;
uint64_t g_pendingMark = 0;
uint32_t g_heldFrames = 0;
uint64_t g_mapStall = 0, g_mapStallCount = 0;   // Map calls on the dynamic rings that took longer than 1 ms (the driver waited for the GPU / allocated a new instance)
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
}


// FXC's optimiser used to miscompile the big branchy lighting shaders (a 'continue' of a forward jump was hoisted above the register stores of the previous block ->
// stale registers leaked into the colour). Forward jumps are now emitted without 'continue', which avoids it; ITC_SHADER_FLAGS=4 compiles without optimisation for comparisons.
UINT ShaderCompileFlags()
{
    static const UINT f = getenv("ITC_SHADER_FLAGS") ? (UINT)strtoul(getenv("ITC_SHADER_FLAGS"), nullptr, 0) : (UINT)D3DCOMPILE_OPTIMIZATION_LEVEL1;
    return f;
}

bool CompileHlsl(const std::string& src, const char* target, const char* tag, ID3DBlob** blob, bool quick = false)
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
        const UINT fl = (shaderDebug ? kDebugLadder : kFlagLadder)[li];
        err.reset(); if (*blob) { (*blob)->Release(); *blob = nullptr; }
        hr = D3DCompile(src.data(), src.size(), tag, nullptr, nullptr, "main", target, fl, 0, blob, err.put());
        if (SUCCEEDED(hr)) break;
        if (err && strstr((const char*)err->GetBufferPointer(), "X4000") == nullptr) break;   // only this error is worth retrying
    }
    if (g_dumpShaders || FAILED(hr))
    {
        CreateDirectoryA("C:/itc-work/shaders", nullptr);
        char path[256]; snprintf(path, sizeof path, "C:/itc-work/shaders/%s%s.hlsl", tag, FAILED(hr) ? ".fail" : "");
        if (FILE* f = fopen(path, "wb")) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
    }
    if (FAILED(hr))
    {
        std::string errs = err ? (const char*)err->GetBufferPointer() : "?", only;
        size_t pos = 0;
        while (pos < errs.size())
        {
            size_t e = errs.find('\n', pos);
            if (e == std::string::npos) e = errs.size();
            const std::string ln = errs.substr(pos, e - pos);
            if (ln.find("error") != std::string::npos) only += ln + "\n";
            pos = e + 1;
        }
        Log("[gpu] HLSL compile failed (%s) hr=%08X: %.1500s", tag, (unsigned)hr, only.c_str());
        return false;
    }
    return true;
}

// ---- Shaders --------------------------------------------------------------------------------------------

// Compiling translated HLSL costs 50-300 ms per shader and a level has hundreds of them, so shaders are compiled on worker threads and
// their bytecode is cached on disk (shader_cache/): a draw whose shader is still compiling is skipped for a few frames instead of
// stalling the command processor. ITC_SYNC_SHADERS=1 waits for the compile instead (deterministic captures).
struct CompileJob
{
    std::string hlsl, target, tag;
    std::string cachePath;            // empty: the result is not persisted (quick first-stage build)
    bool quick = false;               // unoptimised build (D3DCOMPILE_SKIP_OPTIMIZATION): used until the optimised build of the same shader is ready
    std::atomic<int> state{ 0 };      // 0 = compiling, 1 = done, 2 = failed
    std::vector<uint8_t> bytecode;
};
struct ShaderEntry
{
    CP<ID3D11VertexShader> vs;
    CP<ID3D11PixelShader> ps;
    XShaderInfo info;
    bool failed = false;
    std::string name;                  // "ps_<hash>_<interpolators>" - shown as the object name in RenderDoc
    std::shared_ptr<CompileJob> job;   // set while the compile is in flight
    std::shared_ptr<CompileJob> jobOpt;   // the optimised build that follows a quick one (swapped in when done)
};
std::unordered_map<uint64_t, ShaderEntry> g_shaders;
CP<ID3D11GeometryShader> g_rectGS;


std::mutex g_jobMutex;
std::condition_variable g_jobCv;
std::deque<std::shared_ptr<CompileJob>> g_jobQueue;
bool g_workersStarted = false;

void CompileWorker()
{
    for (;;)
    {
        std::shared_ptr<CompileJob> job;
        {
            std::unique_lock<std::mutex> l(g_jobMutex);
            g_jobCv.wait(l, [] { return !g_jobQueue.empty(); });
            job = g_jobQueue.front();
            g_jobQueue.pop_front();
        }
        CP<ID3DBlob> blob;
        if (CompileHlsl(job->hlsl, job->target.c_str(), job->tag.c_str(), blob.put(), job->quick))
        {
            const uint8_t* b = (const uint8_t*)blob->GetBufferPointer();
            job->bytecode.assign(b, b + blob->GetBufferSize());
            // Persist the bytecode (write to a temporary name first so a killed process never leaves a truncated cache file).
            const std::string tmp = job->cachePath + ".tmp";
            if (!job->cachePath.empty()) if (FILE* f = fopen(tmp.c_str(), "wb"))
            {
                const bool ok = fwrite(job->bytecode.data(), 1, job->bytecode.size(), f) == job->bytecode.size();
                fclose(f);
                if (ok) MoveFileExA(tmp.c_str(), job->cachePath.c_str(), MOVEFILE_REPLACE_EXISTING); else DeleteFileA(tmp.c_str());
            }
            job->state = 1;
        }
        else job->state = 2;
        job->hlsl.clear(); job->hlsl.shrink_to_fit();
    }
}

void QueueCompile(const std::shared_ptr<CompileJob>& job)
{
    {
        std::lock_guard<std::mutex> l(g_jobMutex);
        if (!g_workersStarted)
        {
            g_workersStarted = true;
            const unsigned hw = std::thread::hardware_concurrency();
            const unsigned n = std::max(2u, std::min(4u, hw / 2));
            for (unsigned i = 0; i < n; i++) std::thread(CompileWorker).detach();
        }
        g_jobQueue.push_back(job);
    }
    g_jobCv.notify_one();
}

bool CreateShaderObject(ShaderEntry& e, bool isPixel, const void* code, size_t size)
{
    HRESULT hr;
    if (isPixel) hr = g_dev->CreatePixelShader(code, size, nullptr, e.ps.put());
    else hr = g_dev->CreateVertexShader(code, size, nullptr, e.vs.put());
    if (SUCCEEDED(hr) && !e.name.empty())
    {
        static const GUID kDebugObjectName = { 0x429b8c22, 0x9188, 0x4b0c, { 0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00 } };   // WKPDID_D3DDebugObjectName
        ID3D11DeviceChild* obj = isPixel ? (ID3D11DeviceChild*)e.ps.get() : (ID3D11DeviceChild*)e.vs.get();
        obj->SetPrivateData(kDebugObjectName, (UINT)e.name.size(), e.name.c_str());
    }
    return SUCCEEDED(hr);
}

ShaderEntry* GetShader(const ShaderCode& sc, bool isPixel)
{
    static const bool syncShaders = getenv("ITC_SYNC_SHADERS") != nullptr;
    uint32_t interp = 0; int pgen = -1;
    if (isPixel) GetPsParams(interp, pgen);
    uint64_t key = sc.hash ^ (isPixel ? 0x5555000000000000ull : 0) ^ ((uint64_t)interp << 40) ^ ((uint64_t)(pgen + 1) << 48);
    auto it = g_shaders.find(key);
    if (it != g_shaders.end())
    {
        ShaderEntry& e = it->second;
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
    }

    ShaderEntry& e = g_shaders[key];
    XTranslateParams tp;
    tp.isPixel = isPixel; tp.psInterpolatorCount = interp; tp.psParamGenReg = pgen;
    std::string hlsl = TranslateXenosShader(sc.code.data(), (uint32_t)sc.code.size(), tp, e.info);
    {
        // Debug: ITC_DEBUG_PS_UV=<hex prefix of a pixel shader hash> makes that shader output its first interpolators (colour = frac(t0.xy), frac(t1.x)).
        // ITC_DEBUG_PS_COLORS="<hash prefix>=RRGGBB,..." paints the output of the listed pixel shaders in a solid colour (to see which draws cover which pixels).
        static const char* dbg = getenv("ITC_DEBUG_PS_UV");
        static const char* dbgCol = getenv("ITC_DEBUG_PS_COLORS");
        char hs[24]; snprintf(hs, sizeof hs, "%016llx", (unsigned long long)sc.hash);
        const std::string marker = "\t{\n\t\tuint af = (uint)sys2.x;";   // the alpha test block that follows the body of every pixel shader
        if (dbg && isPixel && strncmp(hs, dbg, strlen(dbg)) == 0)
        {
            const size_t pos = hlsl.find(marker);
            if (pos != std::string::npos) hlsl.insert(pos, "\toC0 = float4(frac(iT0.xy), frac(iT1.x), 1.0);\n");
        }
        static const bool dbgRainbow = getenv("ITC_DEBUG_PS_RAINBOW") != nullptr;   // per-draw solid colour in the main HDR pass, set through sys2.z (see XGpuDraw)
        if (dbgRainbow && isPixel)
        {
            const size_t pos = hlsl.find(marker);
            if (pos != std::string::npos)
                hlsl.insert(pos, "\tif (sys2.z != 0.0) { uint xe_dc = (uint)sys2.z; oC0 = float4(((xe_dc >> 16) & 255) / 255.0, ((xe_dc >> 8) & 255) / 255.0, (xe_dc & 255) / 255.0, 1.0); }\n");
        }
        if (dbgCol && isPixel)
        {
            const char* q = dbgCol;
            while (*q)
            {
                const char* eq = strchr(q, '=');
                if (!eq) break;
                const size_t n = (size_t)(eq - q);
                unsigned rgb = (unsigned)strtoul(eq + 1, nullptr, 16);
                if (n > 0 && strncmp(hs, q, n) == 0)
                {
                    const size_t pos = hlsl.find(marker);
                    if (pos != std::string::npos)
                    {
                        char ins[96]; snprintf(ins, sizeof ins, "\toC0 = float4(%g, %g, %g, 1.0);\n", ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
                        hlsl.insert(pos, ins);
                    }
                }
                const char* comma = strchr(eq, ',');
                if (!comma) break;
                q = comma + 1;
            }
        }
    }
    char tag[64]; snprintf(tag, sizeof tag, "%s_%016llx_%u", isPixel ? "ps" : "vs", (unsigned long long)sc.hash, interp);
    const char* target = isPixel ? "ps_5_0" : "vs_5_0";
    e.name = tag;

    // Disk cache keyed by the generated HLSL (so translator changes invalidate it automatically).
    uint64_t srcHash = 0xCBF29CE484222325ull ^ (isPixel ? 0x77ull : 0x33ull);
    for (unsigned char ch : hlsl) { srcHash ^= ch; srcHash *= 0x100000001B3ull; }   // FNV-1a (a cache key must not collide)
    static char cacheDirBuf[64] = "";
    if (!cacheDirBuf[0])
    {
        if (getenv("ITC_SHADER_DEBUG")) snprintf(cacheDirBuf, sizeof cacheDirBuf, "shader_cache_dbg");
        else snprintf(cacheDirBuf, sizeof cacheDirBuf, "shader_cache_f%u", (unsigned)ShaderCompileFlags());
    }
    const char* cacheDir = cacheDirBuf;   // differently compiled bytecode must not mix with the normal cache
    char cacheFile[160]; snprintf(cacheFile, sizeof cacheFile, "%s/%s_%016llx.bin", cacheDir, target, (unsigned long long)srcHash);
    if (FILE* f = fopen(cacheFile, "rb"))
    {
        fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> bytes(n > 0 ? (size_t)n : 0);
        const bool ok = n > 0 && fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
        fclose(f);
        if (ok && CreateShaderObject(e, isPixel, bytes.data(), bytes.size())) { g_shaderOk++; g_shaderCacheHits++; return &e; }
    }
    CreateDirectoryA(cacheDir, nullptr);
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
    QueueCompile(job);
    if (g_log) Log("[gpu] compiling %s (%zu dwords, %zu textures, %zu vfetch) in the background", tag, sc.code.size(), e.info.textureSlots.size(), e.info.vfetchSlots.size());
    if (syncShaders) return GetShader(sc, isPixel);
    g_shaderPending++;
    return nullptr;
}

CP<ID3D11Buffer> g_cbVS, g_cbPS;
// Per-draw constants (b1: the first index of the draw in the index buffer): a 4 MB dynamic ring addressed with Direct3D 11.1 constant-buffer offsets
// (one NO_OVERWRITE map per draw, DISCARD at the wrap), or without that support a 16-byte buffer rewritten with DISCARD for every draw.
CP<ID3D11Buffer> g_cbDraw;
CP<ID3D11DeviceContext1> g_ctx1;
bool g_cbOffsets = false;
uint32_t g_cbDrawPos = 0;
const uint32_t kCbDrawEntries = 16384;
uint64_t g_cbUploadsV = 0, g_cbUploadsP = 0;
// The big constant buffers (b0, 5968 bytes) go through one 48 MB dynamic ring as well (NO_OVERWRITE maps, bound with offsets) when the device supports it.
CP<ID3D11Buffer> g_cbRing;
const uint32_t kCbRingStride = 6144, kCbRingEntries = 8192;   // 384 constants per entry
uint32_t g_cbRingPos = 0;
UINT g_cbRingFirstV = 0, g_cbRingFirstP = 0;


struct CachedTex
{
    CP<ID3D11Texture2D> tex;
    CP<ID3D11ShaderResourceView> srv;
    uint64_t dataHash = 0;
    uint32_t checkedFrame = 0;
    uint64_t checkedSeq = 0;
    uint32_t base = 0;
    size_t footprint = 0;
    uint32_t w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    size_t bytes = 0;
    uint32_t comps = 4;
    uint32_t levels = 1;
    uint8_t stable = 0;   // consecutive hash comparisons that found the data unchanged (saturating)
    bool mlValid = false;
    MipLayout ml;         // guest mip layout (depends on the fetch constant only; computed once)
};
std::unordered_map<uint64_t, CachedTex> g_textures;
size_t g_texBytes = 0;
CP<ID3D11ShaderResourceView> g_dummy2D, g_dummy3D, g_dummyCube;

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
    uint32_t sigFrame = 0xFFFFFFFFu;   // frame in which memSig was last compared with guest memory
    bool isDepth = false;   // depth/stencil resolve kept as R32_FLOAT (never written back to guest memory)
    bool velocity = false;  // produced by the motion-vector pass (see g_velocityNeeded)
};
std::map<uint32_t, Resolved> g_resolved;
uint32_t g_resolvedGen = 1, g_texGen = 1;   // bumped when the resolved-texture table / the texture cache changes (invalidates the per-slot texture memos)
void DumpTextureResource(ID3D11Texture2D* tex, const char* name);


CP<ID3D11Texture2D> g_flushStaging;
uint32_t g_flushW = 0, g_flushH = 0;
DXGI_FORMAT g_flushFmt = DXGI_FORMAT_UNKNOWN;

// Writes the resolved texture back into guest memory (tiled, endian-swapped) and forgets it.
const char* g_flushReason = "?";
std::map<std::string, std::pair<uint64_t, uint64_t>> g_flushByReason;   // reason -> (count, bytes) since the last stats line
void FlushResolved(uint32_t base)
{
    auto it = g_resolved.find(base);
    if (it == g_resolved.end()) return;
    ProfScope profFlush(g_profFlush);
    Resolved& r = it->second;
    if (r.isDepth) { g_resolved.erase(it); g_resolvedGen++; return; }
    if (r.velocity) NoteVelocityUse("flush to guest memory");
    g_flushCount++; g_flushBytes += r.bytes; g_ev.flushes++;
    { auto& e = g_flushByReason[g_flushReason]; e.first++; e.second += r.bytes; }
    { static int logged = 0; if (g_log && logged++ < 60) Log("[gpu] flush (%s) E[%08X] %ux%u fmt=%u bytes=%zu frame %u", g_flushReason, base, r.w, r.h, r.fmt, r.bytes, g_frame); }
    XTRACE("[trace] FLUSH E[%08X] tex=%p %ux%u", base, (void*)r.tex.get(), r.w, r.h);
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
                if (bpp == 1) { dest[off] = row[x]; continue; }
                if (bpp == 2)
                {
                    uint16_t v; memcpy(&v, row + x * 2, 2);
                    if (r.endian != 0) v = __builtin_bswap16(v);
                    memcpy(dest + off, &v, 2);
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
        NoteMemWrite(base, r.bytes);
    }
    g_resolved.erase(it);
    g_resolvedGen++;
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


CP<ID3D11ShaderResourceView> MakeDummy(D3D11_SRV_DIMENSION dim)
{
    CP<ID3D11ShaderResourceView> srv;
    const uint32_t px[6] = { 0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u, 0xFF808080u };
    CP<ID3D11Texture2D> t2;
    if (dim == D3D11_SRV_DIMENSION_TEXTURE3D)
    {
        D3D11_TEXTURE3D_DESC d = {}; d.Width = d.Height = d.Depth = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = { px, 4, 4 };
        CP<ID3D11Texture3D> t3;
        if (SUCCEEDED(g_dev->CreateTexture3D(&d, &sd, t3.put()))) g_dev->CreateShaderResourceView(t3.get(), nullptr, srv.put());
        return srv;
    }
    D3D11_TEXTURE2D_DESC d = {}; d.Width = d.Height = 1; d.MipLevels = 1; d.ArraySize = dim == D3D11_SRV_DIMENSION_TEXTURECUBE ? 6 : 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = dim == D3D11_SRV_DIMENSION_TEXTURECUBE ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    D3D11_SUBRESOURCE_DATA sd[6]; for (int i = 0; i < 6; i++) sd[i] = { &px[i], 4, 4 };
    if (SUCCEEDED(g_dev->CreateTexture2D(&d, sd, t2.put()))) g_dev->CreateShaderResourceView(t2.get(), nullptr, srv.put());
    return srv;
}

// Cube texture: six consecutive 4 KB-aligned faces (+X,-X,+Y,-Y,+Z,-Z), each decoded like a 2D base level (mips are not used yet: the game's
// cube fetch constants select the base map only).
ID3D11ShaderResourceView* GetCubeSRV(uint32_t slot, const TexFetch& tf)
{
    const size_t faceBytes = TextureFootprint(tf);
    const uint32_t faceStride = (uint32_t)((faceBytes + 4095) & ~(size_t)4095);
    const uint64_t key = Hash64(tf.d, sizeof tf.d) ^ 0x7C0BE5;
    CachedTex& ct = g_textures[key];
    const size_t footprint = (size_t)faceStride * 6;
    if (ct.srv && ct.checkedFrame == g_frame && !WrittenSince(ct.checkedSeq, ct.base, footprint)) { g_slotComps[slot & 31] = ct.comps; return ct.srv.get(); }
    Decoded faces[6];
    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint32_t f = 0; f < 6; f++)
    {
        TexFetch tf2 = tf;
        tf2.d[1] = (tf.d[1] & 0xFFFu) | ((tf.base() + f * faceStride) & 0xFFFFF000u);
        tf2.d[5] = (tf.d[5] & ~(3u << 9)) | (1u << 9);   // make it look like a 2D texture
        uint64_t h = 0;
        { ProfScope ps(g_profDecode); DecodeTexture(tf2, faces[f], false, &h); }
        hash = (hash ^ h) * 0x100000001B3ull;
        if (faces[f].data.empty() || faces[f].fmt == DXGI_FORMAT_UNKNOWN || faces[f].w != faces[0].w || faces[f].h != faces[0].h) return nullptr;
    }
    ct.checkedFrame = g_frame; ct.checkedSeq = g_memSeq;
    if (ct.srv && hash == ct.dataHash) { g_slotComps[slot & 31] = ct.comps; return ct.srv.get(); }
    g_decodeCount++; g_decodeBytes += faces[0].data.size() * 6;
    if (ct.tex && ct.w == faces[0].w && ct.h == faces[0].h && ct.fmt == faces[0].fmt)
    {
        for (uint32_t f = 0; f < 6; f++) g_ctx->UpdateSubresource(ct.tex.get(), f, nullptr, faces[f].data.data(), faces[f].rowPitch, 0);
        ct.dataHash = hash; ct.comps = faces[0].comps; g_slotComps[slot & 31] = ct.comps;
        return ct.srv.get();
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = faces[0].w; d.Height = faces[0].h; d.MipLevels = 1; d.ArraySize = 6; d.Format = faces[0].fmt; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    D3D11_SUBRESOURCE_DATA sd[6];
    for (int f = 0; f < 6; f++) sd[f] = { faces[f].data.data(), faces[f].rowPitch, 0 };
    CP<ID3D11Texture2D> t;
    if (FAILED(g_dev->CreateTexture2D(&d, sd, t.put()))) { Log("[gpu] cube texture create failed %ux%u fmt %u", faces[0].w, faces[0].h, (unsigned)faces[0].fmt); return nullptr; }
    CP<ID3D11ShaderResourceView> srv;
    g_dev->CreateShaderResourceView(t.get(), nullptr, srv.put());
    g_texBytes += faces[0].data.size() * 6; g_texBytes -= ct.bytes;
    ct.base = tf.base(); ct.footprint = footprint; ct.tex = t; ct.srv = srv; ct.dataHash = hash; ct.w = faces[0].w; ct.h = faces[0].h; ct.fmt = faces[0].fmt;
    ct.bytes = faces[0].data.size() * 6; ct.comps = faces[0].comps; g_slotComps[slot & 31] = ct.comps;
    if (g_log) Log("[gpu] cube texture slot %u %ux%u fmt=%u base=%08X face stride %X", slot, ct.w, ct.h, tf.format(), tf.base(), faceStride);
    return srv.get();
}

// Returns an SRV for the texture fetch constant (dimension must match what the shader declared).
// Draws mostly repeat the textures of the previous draw: the result of the last lookup per fetch slot is reused while the fetch constant is unchanged, the frame is the
// same and no guest memory range was written by the GPU emulation since (g_resolvedGen counts changes of the resolved-texture table, g_texGen clears of the cache).
struct TexMemo { uint32_t d[6]; uint32_t dim; uint32_t frame; uint64_t seq; uint32_t resolvedGen, texGen, comps; ID3D11ShaderResourceView* srv; };
TexMemo g_texMemo[32];
ID3D11ShaderResourceView* GetTextureSRVSlow(uint32_t slot, uint32_t shaderDim, const TexFetch& tf);
ID3D11ShaderResourceView* GetTextureSRV(uint32_t slot, uint32_t shaderDim)
{
    const TexFetch tf = GetTexFetch(slot);
    TexMemo& m = g_texMemo[slot & 31];
    if (!g_noTexMemo && m.srv && m.frame == g_frame && m.seq == g_memSeq && m.dim == shaderDim && m.resolvedGen == g_resolvedGen && m.texGen == g_texGen && memcmp(m.d, tf.d, sizeof m.d) == 0 &&
        !(g_log && g_frame == g_captureFrame))
    {
        g_slotComps[slot & 31] = m.comps;
        return m.srv;
    }
    g_ev.texMemoMiss++;
    ID3D11ShaderResourceView* r = GetTextureSRVSlow(slot, shaderDim, tf);
    const uint32_t gen = g_resolvedGen;   // a flush inside the lookup bumps the generation: remember the value the result was computed with
    memcpy(m.d, tf.d, sizeof m.d); m.dim = shaderDim; m.frame = g_frame; m.seq = g_memSeq; m.resolvedGen = gen; m.texGen = g_texGen; m.comps = g_slotComps[slot & 31]; m.srv = r;
    return r;
}
ID3D11ShaderResourceView* GetTextureSRVSlow(uint32_t slot, uint32_t shaderDim, const TexFetch& tf)
{
    {
        const uint32_t logFrame = g_captureFrame;
        if (g_log && g_frame == logFrame)
        {
            const uint8_t* b = PhysPtr(tf.base());
            Log("[gpu]   tex slot %u dim=%u type=%u fmt=%u %ux%u tiled=%u pitch=%u base=%08X endian=%u swz=%03X expAdj=%d numFmt=%u mips=%u..%u raw=%08X %08X %08X %08X %08X %08X data=%02X%02X%02X%02X %02X%02X%02X%02X", slot, tf.dimension(), tf.type(), tf.format(),
                (tf.d[2] & 0x1FFF) + 1, ((tf.d[2] >> 13) & 0x1FFF) + 1, tf.tiled(), tf.pitchTexels(), tf.base(), tf.endian(), tf.swizzle(), ((int32_t)(tf.d[3] << 13)) >> 26, tf.d[3] & 1, (tf.d[4] >> 2) & 0xF, (tf.d[4] >> 6) & 0xF, tf.d[0], tf.d[1], tf.d[2], tf.d[3], tf.d[4], tf.d[5], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        }
    }
    // shaderDim: xenos TextureDimension 0=1D 1=2D 2=3D 3=cube
    g_slotComps[slot & 31] = 4;
    if (tf.type() == 2 && shaderDim != 2 && shaderDim != 3)
    {
        const uint32_t base = tf.base() & 0x1FFFFFFF;
        uint32_t fw, fh;
        if (tf.dimension() == 0) { fw = (tf.d[2] & 0xFFFFFF) + 1; fh = 1; } else { fw = (tf.d[2] & 0x1FFF) + 1; fh = ((tf.d[2] >> 13) & 0x1FFF) + 1; }
        auto it = g_resolved.find(base);
        if (it != g_resolved.end())
        {
            Resolved& r = it->second;
            const uint32_t ff = tf.format();
            auto canon = [](uint32_t f) { return f == 14 || f == 50 ? 6u : f == 54 ? 7u : f == 55 ? 16u : f == 56 ? 17u : f == 23 ? 22u : f; };
            const bool fmtOk = canon(ff) == canon(r.fmt);
            static const bool noSample = getenv("ITC_NO_RESOLVED_SAMPLE") != nullptr;
            // The guest-memory signature (a sample of the target region) is compared once per frame and entry, not at every fetch.
            const bool sigOk = !noSample && (r.sigFrame == g_frame || (MemSig(base, r.bytes) == r.memSig && (r.sigFrame = g_frame, true)));
            if (fmtOk && fw == r.w && fh == r.h && tf.tiled() && sigOk)
            {
                g_slotComps[slot & 31] = r.comps;
                if (r.velocity) NoteVelocityUse("texture fetch");
                return r.srv.get();
            }
            {
                const uint32_t lf = g_captureFrame;
                if (g_log && g_frame == lf) Log("[gpu]   resolved texture %08X NOT used: fmtOk=%d dims %ux%u vs %ux%u tiled=%d sigOk=%d -> flushing to memory", base, (int)fmtOk, fw, fh, r.w, r.h, (int)tf.tiled(), (int)sigOk);
            }
            g_flushReason = "fetch-mismatch"; FlushResolved(base);   // incompatible use: make guest memory authoritative again
        }
        // Any other pending resolved texture overlapping this one must be written back before decoding from memory.
        if (!g_resolved.empty()) { g_flushReason = "fetch-overlap"; FlushOverlapping(base, TextureFootprint(tf), 0xFFFFFFFF); }
    }
    if (shaderDim == 2) return g_dummy3D.get();
    if (shaderDim == 3)
    {
        if (tf.type() == 2 && tf.dimension() == 3) if (ID3D11ShaderResourceView* cube = GetCubeSRV(slot, tf)) return cube;
        return g_dummyCube.get();
    }
    if (tf.type() != 2) return g_dummy2D.get();
    const uint64_t key = Hash64(tf.d, sizeof tf.d) ^ 0x1234;
    CachedTex& ct = g_textures[key];
    if (!ct.mlValid) { ct.ml = ComputeMips(tf); ct.mlValid = true; }
    const MipLayout& ml = ct.ml;
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
        static uint32_t budgetFrame = 0xFFFFFFFFu, rehashed = 0;
        if (budgetFrame != g_frame) { budgetFrame = g_frame; rehashed = 0; }
        const bool untouched = !WrittenSince(ct.checkedSeq, ct.base, ct.footprint);
        if (ct.stable >= kStableAfter && ct.levels == ml.levels && untouched && (((g_frame + (uint32_t)(key & 31)) & (kRecheckPeriod - 1)) != 0 || rehashed >= kRehashBudget))
        {
            ct.checkedFrame = g_frame;   // static texture: its data is not compared this frame (checkedSeq stays: a later GPU write to its memory still triggers the comparison)
            g_slotComps[slot & 31] = ct.comps;
            return ct.srv.get();
        }
        if (untouched && ct.stable >= kStableAfter) rehashed++;
        Decoded unused;
        g_ev.texHashed++;
        { ProfScope ps(g_profHash); DecodeTexture(tf, unused, true, &hash); if (ml.levels > 1) hash = (hash ^ HashMipRegion(ml)) * 0x100000001B3ull; }
        ct.checkedFrame = g_frame; ct.checkedSeq = g_memSeq;
        if (hash == ct.dataHash && ct.levels == ml.levels) { if (ct.stable < 255) ct.stable++; g_slotComps[slot & 31] = ct.comps; return ct.srv.get(); }
        ct.stable = 0;
    }
    static Decoded dec;
    const uint64_t tDec0 = QpcNow();
    { ProfScope ps(g_profDecode); DecodeTexture(tf, dec, false, &hash); }
    const uint64_t tDec1 = QpcNow();
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
    static int mipLogged = 0;
    if (g_log && (g_decodeCount < 60 || (nLevels > 1 && mipLogged++ < 40))) Log("[gpu] decode tex slot %u fmt=%u %ux%u tiled=%u endian=%u mips=%u -> %u bytes", slot, tf.format(), dec.w, dec.h, tf.tiled(), tf.endian(), nLevels, (unsigned)dec.data.size());
    g_ev.texBytes += dec.data.size();
    {
        // Slow decodes are logged (ITC_SLOW_MS applies): what the texture is and where the time went.
        static LARGE_INTEGER qf = {}; if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
        const double decMs = (double)(tDec1 - tDec0) * 1000.0 / (double)qf.QuadPart, mipMs = (double)(QpcNow() - tDec1) * 1000.0 / (double)qf.QuadPart;
        static int logged = 0;
        if (decMs + mipMs > 3.0 && logged++ < 60)
            Log("[slowtex] frame %u slot %u fmt=%u %ux%u tiled=%u endian=%u mips=%u/%u base=%08X: decode %.1f ms (+%.1f ms mips) -> %zu KB", g_frame, slot, tf.format(), dec.w, dec.h, tf.tiled(), tf.endian(), nLevels, ml.levels, tf.base(), decMs, mipMs, dec.data.size() / 1024);
    }
    if (ct.tex && ct.w == dec.w && ct.h == dec.h && ct.fmt == dec.fmt && ct.levels == nLevels)
    {
        g_ev.texUpdated++;
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
    g_ev.texCreated++;
    HRESULT chr = g_dev->CreateTexture2D(&d, sd, t.put());
    if (FAILED(chr) && d.MipLevels > 1) { d.MipLevels = 1; t.reset(); chr = g_dev->CreateTexture2D(&d, sd, t.put()); }   // fall back to the base level only
    if (FAILED(chr)) { Log("[gpu] texture create failed %ux%u fmt %u mips %u", dec.w, dec.h, (unsigned)dec.fmt, nLevels); return g_dummy2D.get(); }
    CP<ID3D11ShaderResourceView> srv;
    g_dev->CreateShaderResourceView(t.get(), nullptr, srv.put());
    g_texBytes += totalBytes;
    g_texBytes -= ct.bytes;
    ct.base = tf.base(); ct.footprint = footprintOf(TextureFootprint(tf)); ct.tex = t; ct.srv = srv; ct.dataHash = hash; ct.checkedFrame = g_frame; ct.checkedSeq = g_memSeq; ct.w = dec.w; ct.h = dec.h; ct.fmt = dec.fmt;
    ct.bytes = totalBytes; ct.comps = dec.comps; ct.levels = d.MipLevels; g_slotComps[slot & 31] = dec.comps;
    g_texGen++;   // a (re)created view: memoised lookups of this texture may hold the old one
    if (g_texBytes > (1ull << 30)) { Log("[gpu] texture cache cleared"); g_textures.clear(); g_texBytes = 0; g_texGen++; return g_dummy2D.get(); }
    return srv.get();
}

// ---- Sampler / state caches --------------------------------------------------------------------------------
std::unordered_map<uint64_t, CP<ID3D11SamplerState>> g_samplers;
D3D11_TEXTURE_ADDRESS_MODE ClampToAddr(uint32_t c)
{
    switch (c)
    {
    case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
    case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    case 6: case 7: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}
ID3D11SamplerState* GetSampler(const TexFetch& tf)
{
    const uint32_t cx = (tf.d[0] >> 10) & 7, cy = (tf.d[0] >> 13) & 7, cz = (tf.d[0] >> 16) & 7;
    const uint32_t mag = (tf.d[3] >> 19) & 3, mn = (tf.d[3] >> 21) & 3, mip = (tf.d[3] >> 23) & 3;
    const uint32_t aniso = (tf.d[3] >> 26) & 7;   // 0/1 = off, 2 = 2:1, 3 = 4:1, 4 = 8:1, 5+ = 16:1
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
        }
        g_dev->CreateSamplerState(&d, s.put());
    }
    return s.get();
}

// Sampler lookups are remembered per fetch slot (keyed by the two fetch-constant dwords that define the sampler).
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

// ---- Vertex buffer cache ------------------------------------------------------------------------------------
struct VBEntry
{
    CP<ID3D11Buffer> buf;
    CP<ID3D11ShaderResourceView> srv;
    std::vector<uint8_t> shadow;
    std::vector<uint32_t> pageStamp;   // frame in which each 4 KB page was last compared with guest memory
    std::vector<uint8_t> pageStable;   // consecutive comparisons that found the page unchanged (saturating)
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
const uint32_t kVbCreateBudget = 12;
const uint32_t kVbRingMaxBuffer = 64u << 10;   // only guest buffers up to this size go through the ring, copied whole (shaders may fetch with computed indices anywhere in the buffer)
ID3D11ShaderResourceView* VbRingCopy(const uint8_t* src, uint32_t rangeStart, uint32_t rangeEnd, uint32_t avail, uint32_t* baseDw)
{
    (void)rangeStart; (void)rangeEnd;
    if (!g_vbRing || avail > kVbRingMaxBuffer) return nullptr;
    const uint32_t rs = 0, re = avail;
    const uint32_t n = re - rs;
    bool discard = g_vbRingFirst;
    if (g_vbRingPos + n > kVbRing) { discard = true; }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(MapTimed(g_vbRing.get(), discard ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, &m, "vertex ring"))) return nullptr;
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

// Returns an SRV over the guest vertex buffer [physBase, physBase + sizeBytes). Only the byte range [rangeStart, rangeEnd) that the
// draw can read is compared against the shadow copy (and re-uploaded when it changed); pages of large buffers are compared at most
// once per frame, small buffers (typically dynamic / inline vertex data) every time.
ID3D11ShaderResourceView* GetVertexBuffer(uint32_t physBase, uint32_t sizeBytes, uint32_t rangeStart, uint32_t rangeEnd, uint32_t* baseDw)
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
    // Small ranges are cheap to copy and are mostly short-lived dynamic data: they get a persistent buffer only after many frames; big ones after a few.
    const uint32_t promoteAfter = avail > 16384 ? kVbPromoteAfter : 30;
    // At most kVbCreateBudget persistent buffers are created per frame (a burst of promotions, e.g. 124 in one frame at a race start, costs ~10 ms); the others wait in the ring.
    static uint32_t budgetFrame = 0xFFFFFFFFu, created = 0;
    if (budgetFrame != g_frame) { budgetFrame = g_frame; created = 0; }
    const bool budgetLeft = created < kVbCreateBudget;
    if (!g_noVbRing && ((!e.buf && (e.frames < promoteAfter || !budgetLeft)) || (e.buf && e.hot)))
    {
        ID3D11ShaderResourceView* r = VbRingCopy(src, std::min(rangeStart, avail), std::min(std::max(rangeEnd, rangeStart), avail), avail, baseDw);
        if (r) return r;
    }
    if (!e.buf)
    {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = avail; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_SUBRESOURCE_DATA sd = { src, 0, 0 };
        g_ev.vbCreated++; g_ev.vbBytesUp += avail; created++;
        if (FAILED(g_dev->CreateBuffer(&d, &sd, e.buf.put()))) { g_vbs.erase(key); return nullptr; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R32_TYPELESS; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX; sv.BufferEx.FirstElement = 0; sv.BufferEx.NumElements = avail / 4; sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        g_dev->CreateShaderResourceView(e.buf.get(), &sv, e.srv.put());
        e.shadow.assign(src, src + avail);
        e.pageStamp.assign((avail + 4095) / 4096, g_frame);
        e.pageStable.assign((avail + 4095) / 4096, 0);
        g_vbBytes += avail;
        return e.srv.get();
    }
    rangeStart = std::min(rangeStart, avail); rangeEnd = std::min(std::max(rangeEnd, rangeStart), avail);
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
            e.pageStable[pg] = 0;
            if (e.lastChangeFrame != g_frame)
            {
                // Data that changes in three consecutive frames is dynamic: the used range goes through the ring for a while instead of page compares + uploads.
                e.changeStreak = (e.lastChangeFrame == g_frame - 1) ? e.changeStreak + 1 : 1;
                e.lastChangeFrame = g_frame;
                if (!g_noVbRing && e.changeStreak >= 3 && avail <= kVbRingMaxBuffer) e.hot = 180;
            }
        }
        else { flushRun(); if (e.pageStable[pg] < 255) e.pageStable[pg]++; }
    }
    flushRun();
    return e.srv.get();
}

// Index ring (StructuredBuffer<uint>)
CP<ID3D11Buffer> g_idxBuf;
CP<ID3D11ShaderResourceView> g_idxSRV;
const uint32_t kIdxRing = 1u << 23;   // index arena (see GetIndexList); startVertex travels through a float constant, exact below 2^24
uint32_t g_idxPos = 0;
bool g_idxFirst = true;

// ---- Render targets ------------------------------------------------------------------------------------------
struct ColorRT
{
    CP<ID3D11Texture2D> tex;
    CP<ID3D11RenderTargetView> rtv;
    CP<ID3D11ShaderResourceView> srv;
    uint32_t w = 0, h = 0, fmt = 0;
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
};
struct DepthRT
{
    CP<ID3D11Texture2D> tex;
    CP<ID3D11DepthStencilView> dsv;
    CP<ID3D11ShaderResourceView> srv;   // depth as R24_UNORM_X8_TYPELESS (for depth resolves)
    uint32_t w = 0, h = 0;
};
std::map<uint32_t, ColorRT> g_colorRTs;   // key = edram base | format << 16
std::map<uint32_t, DepthRT> g_depthRTs;   // key = edram base

// Clears requested by the resolves of a predicated-tiling frame are applied lazily before the next draw: the frame is drawn once (full
// frame) in the first strip pass and the later strips still read it, so the hardware's "clear after resolve" has to wait until the
// whole frame has been resolved - and it has to cover the whole frame, not just the last strip.
struct PendingClear
{
    bool color = false, depth = false;
    ColorRT* rt = nullptr; float col[4] = {};
    uint32_t dbase = 0; float dz = 1.0f; uint8_t dstencil = 0;
} g_pendClear;
void ApplyPendingClears()
{
    if (g_pendClear.color && g_pendClear.rt && g_pendClear.rt->rtv) g_ctx->ClearRenderTargetView(g_pendClear.rt->rtv.get(), g_pendClear.col);
    if (g_pendClear.depth)
    {
        auto it = g_depthRTs.find(g_pendClear.dbase);
        if (it != g_depthRTs.end() && it->second.dsv) g_ctx->ClearDepthStencilView(it->second.dsv.get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, g_pendClear.dz, g_pendClear.dstencil);
    }
    g_pendClear.color = g_pendClear.depth = false;
}


ColorRT* GetColorRT(uint32_t base, uint32_t fmt, uint32_t w, uint32_t h, bool create)
{
    // k_2_10_10_10_FLOAT_AS_16_16_16_16 (12) is the same surface as k_2_10_10_10_FLOAT (3) with a 64-bit EDRAM tile layout; the game clears with
    // one and renders/resolves with the other, so both must reach the same render target texture.
    if (fmt == 12) fmt = 3;
    const uint32_t key = base | (fmt << 16);
    auto it = g_colorRTs.find(key);
    if (it == g_colorRTs.end() && !create) return nullptr;
    ColorRT& rt = g_colorRTs[key];
    if (rt.tex && rt.w >= w && rt.h >= h) return &rt;
    const uint32_t nw = std::max(w, rt.w), nh = std::max(h, rt.h);
    D3D11_TEXTURE2D_DESC d = {};
    const DXGI_FORMAT rtFmt = ColorRtFormat(fmt);
    const DXGI_FORMAT texFmt = (rtFmt == DXGI_FORMAT_R8G8B8A8_UNORM || rtFmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ? DXGI_FORMAT_R8G8B8A8_TYPELESS : rtFmt;
    d.Width = nw; d.Height = nh; d.MipLevels = 1; d.ArraySize = 1; d.Format = texFmt; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    CP<ID3D11Texture2D> t;
    g_ev.rtCreated++;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, t.put()))) { Log("[gpu] RT create failed %ux%u fmt %u", nw, nh, fmt); return nullptr; }
    CP<ID3D11RenderTargetView> v;
    { D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = rtFmt; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; g_dev->CreateRenderTargetView(t.get(), &rd, v.put()); }
    CP<ID3D11ShaderResourceView> sv;
    { D3D11_SHADER_RESOURCE_VIEW_DESC sd = {}; sd.Format = (rtFmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ? DXGI_FORMAT_R8G8B8A8_UNORM : rtFmt; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1; g_dev->CreateShaderResourceView(t.get(), &sd, sv.put()); }
    const float clear[4] = { 0, 0, 0, 0 };
    g_ctx->ClearRenderTargetView(v.get(), clear);
    if (rt.tex)   // keep old content when growing
    {
        D3D11_BOX box = { 0, 0, 0, rt.w, rt.h, 1 };
        g_ctx->CopySubresourceRegion(t.get(), 0, 0, 0, 0, rt.tex.get(), 0, &box);
    }
    rt.tex = t; rt.rtv = v; rt.srv = sv; rt.w = nw; rt.h = nh; rt.fmt = fmt; rt.dxgi = rtFmt;
    if (g_log) Log("[gpu] color RT base=%u fmt=%u %ux%u", base, fmt, nw, nh);
    return &rt;
}

DepthRT* GetDepthRT(uint32_t base, uint32_t w, uint32_t h)
{
    DepthRT& rt = g_depthRTs[base];
    if (rt.tex && rt.w >= w && rt.h >= h) return &rt;
    const uint32_t nw = std::max(w, rt.w), nh = std::max(h, rt.h);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = nw; d.Height = nh; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R24G8_TYPELESS; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    CP<ID3D11Texture2D> t;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, t.put()))) { Log("[gpu] depth RT create failed %ux%u", nw, nh); return nullptr; }
    CP<ID3D11DepthStencilView> v;
    { D3D11_DEPTH_STENCIL_VIEW_DESC dd = {}; dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D; g_dev->CreateDepthStencilView(t.get(), &dd, v.put()); }
    CP<ID3D11ShaderResourceView> sv;
    { D3D11_SHADER_RESOURCE_VIEW_DESC sd = {}; sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1; g_dev->CreateShaderResourceView(t.get(), &sd, sv.put()); }
    g_ctx->ClearDepthStencilView(v.get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    rt.tex = t; rt.dsv = v; rt.srv = sv; rt.w = nw; rt.h = nh;
    return &rt;
}

// ---- Pipeline state caches -------------------------------------------------------------------------------------
std::unordered_map<std::string, CP<ID3D11BlendState>> g_blendStates;
std::unordered_map<std::string, CP<ID3D11RasterizerState>> g_rasterStates;
std::unordered_map<std::string, CP<ID3D11DepthStencilState>> g_depthStates;

D3D11_BLEND BlendFactorToD3D(uint32_t f, bool alpha)
{
    switch (f)
    {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 4: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 5: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 6: return D3D11_BLEND_SRC_ALPHA;
    case 7: return D3D11_BLEND_INV_SRC_ALPHA;
    case 8: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 9: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 10: return D3D11_BLEND_DEST_ALPHA;
    case 11: return D3D11_BLEND_INV_DEST_ALPHA;
    case 12: return alpha ? D3D11_BLEND_BLEND_FACTOR : D3D11_BLEND_BLEND_FACTOR;
    case 13: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 14: return D3D11_BLEND_BLEND_FACTOR;
    case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D11_BLEND_SRC_ALPHA_SAT;
    default: return D3D11_BLEND_ONE;
    }
}
D3D11_BLEND_OP BlendOpToD3D(uint32_t o)
{
    switch (o)
    {
    case 0: return D3D11_BLEND_OP_ADD;
    case 1: return D3D11_BLEND_OP_SUBTRACT;
    case 2: return D3D11_BLEND_OP_MIN;
    case 3: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_REV_SUBTRACT;
    }
}

ID3D11BlendState* GetBlendState(const uint32_t blendRegs[4], uint32_t colorMask, uint32_t numRTs)
{
    D3D11_BLEND_DESC d = {};
    d.IndependentBlendEnable = TRUE;
    for (uint32_t i = 0; i < 4; i++)
    {
        auto& rt = d.RenderTarget[i];
        const uint32_t b = blendRegs[i];
        const uint32_t sc = b & 31, oc = (b >> 5) & 7, dc = (b >> 8) & 31, sa = (b >> 16) & 31, oa = (b >> 21) & 7, da = (b >> 24) & 31;
        const bool passthrough = sc == 1 && dc == 0 && oc == 0 && sa == 1 && da == 0 && oa == 0;
        rt.BlendEnable = !passthrough && i < numRTs;
        rt.SrcBlend = BlendFactorToD3D(sc, false); rt.DestBlend = BlendFactorToD3D(dc, false); rt.BlendOp = BlendOpToD3D(oc);
        rt.SrcBlendAlpha = BlendFactorToD3D(sa, true); rt.DestBlendAlpha = BlendFactorToD3D(da, true); rt.BlendOpAlpha = BlendOpToD3D(oa);
        rt.RenderTargetWriteMask = (UINT8)((colorMask >> (i * 4)) & 0xF);
    }
    std::string key((const char*)&d, sizeof d);
    auto& s = g_blendStates[key];
    if (!s) g_dev->CreateBlendState(&d, s.put());
    return s.get();
}

ID3D11RasterizerState* GetRasterState(bool cullFront, bool cullBack, bool frontCCW, bool depthClip, int depthBias = 0)
{
    D3D11_RASTERIZER_DESC d = {};
    d.DepthBias = depthBias;
    d.FillMode = D3D11_FILL_SOLID;
    d.CullMode = (g_noCull || (!cullFront && !cullBack)) ? D3D11_CULL_NONE : (cullBack && cullFront) ? D3D11_CULL_BACK : cullBack ? D3D11_CULL_BACK : D3D11_CULL_FRONT;
    d.FrontCounterClockwise = frontCCW; d.DepthClipEnable = depthClip; d.ScissorEnable = TRUE;
    std::string key((const char*)&d, sizeof d);
    auto& s = g_rasterStates[key];
    if (!s) g_dev->CreateRasterizerState(&d, s.put());
    return s.get();
}

ID3D11DepthStencilState* GetDepthState(uint32_t dc, uint32_t refMask)
{
    D3D11_DEPTH_STENCIL_DESC d = {};
    const bool stencil = dc & 1, zEnable = (dc >> 1) & 1, zWrite = (dc >> 2) & 1;
    d.DepthEnable = zEnable; d.DepthWriteMask = zWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = (D3D11_COMPARISON_FUNC)(((dc >> 4) & 7) + 1);
    d.StencilEnable = stencil; d.StencilReadMask = (UINT8)((refMask >> 8) & 0xFF); d.StencilWriteMask = (UINT8)((refMask >> 16) & 0xFF);
    d.FrontFace.StencilFunc = (D3D11_COMPARISON_FUNC)(((dc >> 8) & 7) + 1);
    d.FrontFace.StencilFailOp = (D3D11_STENCIL_OP)(((dc >> 11) & 7) + 1);
    d.FrontFace.StencilPassOp = (D3D11_STENCIL_OP)(((dc >> 14) & 7) + 1);
    d.FrontFace.StencilDepthFailOp = (D3D11_STENCIL_OP)(((dc >> 17) & 7) + 1);
    if ((dc >> 7) & 1)
    {
        d.BackFace.StencilFunc = (D3D11_COMPARISON_FUNC)(((dc >> 20) & 7) + 1);
        d.BackFace.StencilFailOp = (D3D11_STENCIL_OP)(((dc >> 23) & 7) + 1);
        d.BackFace.StencilPassOp = (D3D11_STENCIL_OP)(((dc >> 26) & 7) + 1);
        d.BackFace.StencilDepthFailOp = (D3D11_STENCIL_OP)(((dc >> 29) & 7) + 1);
    }
    else d.BackFace = d.FrontFace;
    std::string key((const char*)&d, sizeof d);
    auto& s = g_depthStates[key];
    if (!s) g_dev->CreateDepthStencilState(&d, s.put());
    return s.get();
}

// ---- Present --------------------------------------------------------------------------------------------------
CP<ID3D11VertexShader> g_presentVS;
CP<ID3D11PixelShader> g_presentPS;
CP<ID3D11Buffer> g_presentCB;
CP<ID3D11SamplerState> g_presentSampler;
CP<ID3D11RasterizerState> g_presentRaster;
CP<ID3D11Buffer> g_idxDummy;
CP<ID3D11PixelShader> g_copyPS, g_copyDepthPS;
CP<ID3D11Buffer> g_copyCB;
const char* kCopyDepthHlsl = R"HLSL(
cbuffer P : register(b0) { uint4 rect; uint4 cfg; };
Texture2D<float> srcD : register(t0);
float4 main(float4 pos : SV_Position) : SV_Target
{
    uint2 p = uint2(pos.xy);
    float d = srcD.Load(int3(rect.xy + p, 0));
    return float4(d, d, d, d);
}
)HLSL";
const char* kCopyHlsl = R"HLSL(
cbuffer P : register(b0) { uint4 rect; uint4 cfg; };
Texture2D<float4> src : register(t0);
float4 main(float4 pos : SV_Position) : SV_Target
{
    uint2 p = uint2(pos.xy);
    float4 c = src.Load(int3(rect.xy + p, 0));
    if (cfg.x != 0) c = c.bgra;
    c.rgb *= exp2((float)(int)cfg.y);
    return c;
}
)HLSL";

const char* kPresentHlsl = R"HLSL(
cbuffer C : register(b0) { uint4 swz; };
Texture2D<float4> t : register(t0);
SamplerState s : register(s0);
struct VO { float4 p : SV_Position; float2 uv : TEXCOORD0; };
VO vsmain(uint id : SV_VertexID)
{
    VO o; float2 uv = float2((id << 1) & 2, id & 2);
    o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o;
}
float4 psmain(VO i) : SV_Target
{
    float4 c = t.Sample(s, i.uv);
    float v[6] = { c.x, c.y, c.z, c.w, 0.0, 1.0 };
    uint sw = swz.x;
    return float4(v[min(sw & 7, 5u)], v[min((sw >> 3) & 7, 5u)], v[min((sw >> 6) & 7, 5u)], 1.0);
}
)HLSL";

bool CompileEntry(const char* src, const char* entry, const char* target, ID3DBlob** blob)
{
    CP<ID3DBlob> err;
    HRESULT hr = D3DCompile(src, strlen(src), "present", nullptr, nullptr, entry, target, 0, 0, blob, err.put());
    if (FAILED(hr)) { Log("[gpu] present shader compile failed: %.500s", err ? (const char*)err->GetBufferPointer() : "?"); return false; }
    return true;
}

// ---- GPU timing (timestamp queries) -----------------------------------------------------------------------------------------
// Every frame is bracketed by a disjoint timestamp query (span from the end of the previous Present to the start of this one: includes time the GPU
// waits for the CPU). ITC_GPU_TIMING=1 also stamps every render-target change (time per pass), =2 brackets every draw with two stamps (GPU busy time per
// draw, summed per pass and per pixel shader). ITC_NO_GPU_TIMING=1 turns it all off. Results are read back a few frames late, without stalling.
const int g_gpuMode = getenv("ITC_GPU_TIMING") ? atoi(getenv("ITC_GPU_TIMING")) : 0;
bool g_gpuOff = getenv("ITC_NO_GPU_TIMING") != nullptr;
struct GpuMark { uint64_t key, ps; uint32_t qb, qe; };   // qe == ~0u: no end stamp (the mark spans to the next one)
struct GpuRec
{
    CP<ID3D11Query> disjoint, begin, end;
    std::vector<CP<ID3D11Query>> stamps;
    std::vector<GpuMark> marks;
    uint32_t nStamps = 0;
    bool inFlight = false, recording = false;
};
const int kGpuRing = 8;
GpuRec g_gpuRing[kGpuRing];
int g_gpuCur = 0;
struct GpuAcc
{
    double spanMs = 0, busyMs = 0, minSpan = 1e9, maxSpan = 0;
    uint32_t frames = 0;
    std::map<uint64_t, std::pair<double, uint32_t>> pass;                  // pass key -> (ms, draws)
    std::unordered_map<uint64_t, std::pair<double, uint32_t>> ps;          // pixel shader hash -> (ms, draws)
    std::unordered_map<uint64_t, uint64_t> psPass;
};
GpuAcc g_gpuAcc;
double g_gpuSpanEma = 0;   // overlay value

constexpr uint64_t kPassResolve = 1ull << 50;
constexpr uint64_t kPassOther = 1ull << 51;
uint64_t PassKey(uint32_t mode, uint32_t colorInfo, uint32_t w, uint32_t h, bool depthOnly, uint32_t msaa)
{
    return (uint64_t)(colorInfo & 0xFFF) | ((uint64_t)((colorInfo >> 16) & 0xF) << 12) | ((uint64_t)w << 16) | ((uint64_t)h << 32) | ((uint64_t)depthOnly << 48) | ((uint64_t)(msaa > 1) << 49) | ((uint64_t)(mode == 5) << 52);
}
std::string PassLabel(uint64_t k)
{
    char b[96];
    if (k & kPassResolve) return "resolves";
    if (k & kPassOther) return "other";
    snprintf(b, sizeof b, "%ux%u fmt%u base%u%s%s%s", (unsigned)((k >> 16) & 0xFFFF), (unsigned)((k >> 32) & 0xFFFF), (unsigned)((k >> 12) & 0xF), (unsigned)(k & 0xFFF),
             (k >> 48) & 1 ? " depth-only" : "", (k >> 49) & 1 ? " msaa" : "", (k >> 52) & 1 ? " (mode5)" : "");
    return b;
}
CP<ID3D11Query> MakeQuery(D3D11_QUERY type)
{
    CP<ID3D11Query> q; D3D11_QUERY_DESC d = { type, 0 };
    if (FAILED(g_dev->CreateQuery(&d, q.put()))) q.reset();
    return q;
}
void GpuTimingBeginFrame()
{
    if (g_gpuOff) return;
    GpuRec& r = g_gpuRing[g_gpuCur];
    if (r.inFlight) return;   // the frame recorded in this slot was not read back yet: skip this frame instead of stalling
    if (!r.disjoint)
    {
        r.disjoint = MakeQuery(D3D11_QUERY_TIMESTAMP_DISJOINT); r.begin = MakeQuery(D3D11_QUERY_TIMESTAMP); r.end = MakeQuery(D3D11_QUERY_TIMESTAMP);
        if (!r.disjoint || !r.begin || !r.end) { g_gpuOff = true; Log("[gpu] timestamp queries unavailable: GPU timing is off"); return; }
    }
    g_ctx->Begin(r.disjoint.get()); g_ctx->End(r.begin.get());
    r.marks.clear(); r.nStamps = 0; r.recording = true;
}
uint32_t GpuStamp(GpuRec& r)
{
    if (r.nStamps >= r.stamps.size()) r.stamps.push_back(MakeQuery(D3D11_QUERY_TIMESTAMP));
    CP<ID3D11Query>& q = r.stamps[r.nStamps];
    if (!q) return ~0u;
    g_ctx->End(q.get());
    return r.nStamps++;
}
// Before a draw (mode 2: begin stamp of the draw; mode 1: only when the pass changes).
void GpuTimingBeforeDraw(uint64_t passKey, uint64_t ps)
{
    if (g_gpuMode < 1 || g_gpuOff) return;
    GpuRec& r = g_gpuRing[g_gpuCur];
    if (!r.recording) return;
    if (g_gpuMode == 1 && !r.marks.empty() && r.marks.back().key == passKey) return;
    const uint32_t q = GpuStamp(r);
    if (q != ~0u) r.marks.push_back({ passKey, ps, q, ~0u });
}
void GpuTimingAfterDraw()
{
    if (g_gpuMode < 2 || g_gpuOff) return;
    GpuRec& r = g_gpuRing[g_gpuCur];
    if (!r.recording || r.marks.empty() || r.marks.back().qe != ~0u) return;
    r.marks.back().qe = GpuStamp(r);
}
void GpuTimingEndFrame()   // right before Present
{
    if (g_gpuOff) return;
    GpuRec& r = g_gpuRing[g_gpuCur];
    if (!r.recording) return;
    g_ctx->End(r.end.get()); g_ctx->End(r.disjoint.get());
    r.recording = false; r.inFlight = true;
    g_gpuCur = (g_gpuCur + 1) % kGpuRing;
}
void GpuTimingCollect()   // reads back the finished frames, oldest first
{
    if (g_gpuOff) return;
    for (int n = 0; n < kGpuRing; n++)
    {
        GpuRec& r = g_gpuRing[(g_gpuCur + n) % kGpuRing];
        if (!r.inFlight) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        if (g_ctx->GetData(r.disjoint.get(), &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;   // not finished: the later frames are not either
        r.inFlight = false;
        if (dj.Disjoint || dj.Frequency == 0) continue;
        UINT64 tb = 0, te = 0;
        if (g_ctx->GetData(r.begin.get(), &tb, 8, 0) != S_OK || g_ctx->GetData(r.end.get(), &te, 8, 0) != S_OK || te < tb) continue;
        const double toMs = 1000.0 / (double)dj.Frequency;
        const double span = (double)(te - tb) * toMs;
        std::vector<UINT64> tq;
        if (g_gpuMode >= 1)
        {
            tq.assign(r.nStamps, 0);
            bool ok = true;
            for (uint32_t i = 0; i < r.nStamps && ok; i++) ok = g_ctx->GetData(r.stamps[i].get(), &tq[i], 8, 0) == S_OK;
            if (!ok) continue;
        }
        GpuAcc& a = g_gpuAcc;
        a.spanMs += span; a.frames++; a.minSpan = std::min(a.minSpan, span); a.maxSpan = std::max(a.maxSpan, span);
        g_gpuSpanEma = g_gpuSpanEma == 0 ? span : g_gpuSpanEma * 0.9 + span * 0.1;
        for (size_t i = 0; i < r.marks.size() && g_gpuMode >= 1; i++)
        {
            const GpuMark& m = r.marks[i];
            const UINT64 t0 = tq[m.qb];
            const UINT64 t1 = m.qe != ~0u ? tq[m.qe] : (i + 1 < r.marks.size() ? tq[r.marks[i + 1].qb] : te);
            const double ms = t1 > t0 ? (double)(t1 - t0) * toMs : 0.0;
            if (m.qe != ~0u) a.busyMs += ms;
            auto& pp = a.pass[m.key]; pp.first += ms; pp.second++;
            if (g_gpuMode >= 2 && m.ps) { auto& s = a.ps[m.ps]; s.first += ms; s.second++; a.psPass.emplace(m.ps, m.key); }
        }
    }
}
void GpuTimingPrint()
{
    GpuAcc& a = g_gpuAcc;
    if (!a.frames) return;
    Log("[stat]   gpu: %u frames, span avg %.1f ms (min %.1f, max %.1f)%s", a.frames, a.spanMs / a.frames, a.minSpan, a.maxSpan, "");
    if (g_gpuMode >= 2) Log("[stat]   gpu: busy (sum of the draws' own time) avg %.1f ms/frame", a.busyMs / a.frames);
    if (g_gpuMode >= 1)
    {
        std::vector<std::pair<double, uint64_t>> v;
        for (auto& kv : a.pass) v.push_back({ kv.second.first, kv.first });
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 12; i++)
            Log("[stat]     gpu pass %-40s %7.2f ms/frame  %6.1f %s/frame", PassLabel(v[i].second).c_str(), v[i].first / a.frames, a.pass[v[i].second].second / (double)a.frames, g_gpuMode >= 2 ? "draws" : "marks");
    }
    if (g_gpuMode >= 2)
    {
        std::vector<std::pair<double, uint64_t>> v;
        for (auto& kv : a.ps) v.push_back({ kv.second.first, kv.first });
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 14; i++)
        {
            const auto& s = a.ps[v[i].second];
            Log("[stat]     gpu ps %016llx %-30s %7.2f ms/frame %6.1f draws/frame %7.1f us/draw", (unsigned long long)v[i].second, PassLabel(a.psPass[v[i].second]).c_str(), v[i].first / a.frames, s.second / (double)a.frames, v[i].first * 1000.0 / s.second);
        }
    }
    a = GpuAcc();
}

// ---- On-screen overlay (FPS counter) ------------------------------------------------------------------------------------------
// F3 toggles it (ITC_NO_OVERLAY=1 starts hidden). Glyphs come from an atlas rendered once with GDI (Consolas), drawn as one batch of quads.
const char* kOverlayHlsl = R"HLSL(
cbuffer C : register(b0) { float4 view; float4 org; uint4 ch[64]; };
Texture2D<float> atlas : register(t0);
SamplerState smp : register(s0);
struct VO { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; };
VO vsmain(uint id : SV_VertexID)
{
    uint cid = id / 6, k = id % 6;
    uint v = ch[cid >> 2][cid & 3];
    uint code = v & 0xFFu, cx = (v >> 8) & 0xFFu, cy = (v >> 16) & 0xFFu, ci = v >> 24;
    float2 q = float2((k == 1 || k == 3 || k == 4) ? 1.0 : 0.0, (k == 2 || k == 4 || k == 5) ? 1.0 : 0.0);
    float2 px = org.xy + (float2(cx, cy) + q) * view.zw;
    VO o;
    o.p = float4(px.x * view.x - 1.0, 1.0 - px.y * view.y, 0.0, 1.0);
    float g = (float)code - 32.0;
    o.uv = (float2(fmod(g, 16.0), floor(g / 16.0)) + q) / float2(16.0, 6.0);
    float4 cols[6] = { float4(1, 1, 1, 1), float4(0, 0, 0, 0.62), float4(0.45, 1, 0.45, 1), float4(1, 0.9, 0.3, 1), float4(1, 0.4, 0.35, 1), float4(0.65, 0.82, 1, 1) };
    o.col = cols[min(ci, 5u)];
    return o;
}
float4 psmain(VO i) : SV_Target { float a = atlas.SampleLevel(smp, i.uv, 0); return float4(i.col.rgb, i.col.a * a); }
)HLSL";
struct Overlay
{
    bool tried = false, ok = false, visible = getenv("ITC_NO_OVERLAY") == nullptr;
    CP<ID3D11VertexShader> vs; CP<ID3D11PixelShader> ps; CP<ID3D11Buffer> cb;
    CP<ID3D11ShaderResourceView> atlas; CP<ID3D11SamplerState> smp; CP<ID3D11BlendState> blend;
    static constexpr int kCols = 16, kRows = 6, kCellW = 9, kCellH = 18;
} g_ov;

bool OverlayInit()
{
    g_ov.tried = true;
    CP<ID3DBlob> b;
    if (!CompileEntry(kOverlayHlsl, "vsmain", "vs_5_0", b.put()) || FAILED(g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, g_ov.vs.put()))) return false;
    b.reset();
    if (!CompileEntry(kOverlayHlsl, "psmain", "ps_5_0", b.put()) || FAILED(g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, g_ov.ps.put()))) return false;
    const int cw = Overlay::kCellW, chh = Overlay::kCellH, cols = Overlay::kCols, rows = Overlay::kRows, W = cw * cols, H = chh * rows;
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bmp || !bits) { if (bmp) DeleteObject(bmp); if (dc) DeleteDC(dc); return false; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)W * H * 4);
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    for (int g = 0; g < 95; g++) { const char c = (char)(32 + g); TextOutA(dc, (g % cols) * cw, (g / cols) * chh + 1, &c, 1); }
    GdiFlush();
    std::vector<uint8_t> a8((size_t)W * H);
    for (int i = 0; i < W * H; i++) a8[i] = ((const uint8_t*)bits)[i * 4 + 1];
    for (int y = 0; y < chh; y++) for (int x = 0; x < cw; x++) a8[(size_t)(5 * chh + y) * W + 15 * cw + x] = 255;   // code 127 = solid block (text background)
    SelectObject(dc, oldFont); DeleteObject(font); SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { a8.data(), (UINT)W, 0 };
    CP<ID3D11Texture2D> tex;
    if (FAILED(g_dev->CreateTexture2D(&td, &sd, tex.put())) || FAILED(g_dev->CreateShaderResourceView(tex.get(), nullptr, g_ov.atlas.put()))) return false;
    D3D11_SAMPLER_DESC smd = {}; smd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; smd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&smd, g_ov.smp.put());
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE; bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&bd, g_ov.blend.put());
    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = 32 + 64 * 16; cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&cb, nullptr, g_ov.cb.put()))) return false;
    return true;
}

void InvalidateBound();   // redundant-state filter, defined with the draw code below

// Draws up to three lines of text (colour index per line: 0 white, 2 green, 3 yellow, 4 red, 5 blue) in the top-left corner of the back buffer.
void OverlayDraw(const char* const* lines, const int* colors, int nLines)
{
    if (!g_ov.tried) g_ov.ok = OverlayInit();
    if (!g_ov.ok || !g_presentRaster) return;
    uint32_t cells[256];
    int n = 0;
    for (int l = 0; l < nLines; l++)   // background cells first, then the glyphs
    {
        const int len = (int)strlen(lines[l]);
        for (int c = 0; c < len + 2 && n < 256; c++) cells[n++] = 127u | ((uint32_t)c << 8) | ((uint32_t)l << 16) | (1u << 24);
    }
    for (int l = 0; l < nLines; l++)
        for (int c = 0; lines[l][c] && n < 256; c++)
            if (lines[l][c] != ' ' && (uint8_t)lines[l][c] >= 32 && (uint8_t)lines[l][c] < 127) cells[n++] = (uint32_t)(uint8_t)lines[l][c] | ((uint32_t)(c + 1) << 8) | ((uint32_t)l << 16) | ((uint32_t)colors[l] << 24);
    if (n == 0) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(g_ov.cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    float* f = (float*)m.pData;
    f[0] = 2.0f / (float)g_backW; f[1] = 2.0f / (float)g_backH; f[2] = (float)Overlay::kCellW; f[3] = (float)Overlay::kCellH;
    f[4] = 6.0f; f[5] = 6.0f; f[6] = 1.0f; f[7] = 0.0f;
    uint32_t* u = (uint32_t*)(f + 8);
    memset(u, 0, 64 * 16);
    memcpy(u, cells, (size_t)n * 4);
    g_ctx->Unmap(g_ov.cb.get(), 0);
    InvalidateBound();
    ID3D11RenderTargetView* rtv = g_backRTV.get();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->OMSetBlendState(g_ov.blend.get(), nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(nullptr, 0);
    g_ctx->RSSetState(g_presentRaster.get());
    D3D11_VIEWPORT vp = { 0, 0, (float)g_backW, (float)g_backH, 0, 1 };
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_ov.vs.get(), nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->PSSetShader(g_ov.ps.get(), nullptr, 0);
    ID3D11Buffer* cbp = g_ov.cb.get(); g_ctx->VSSetConstantBuffers(0, 1, &cbp);
    ID3D11ShaderResourceView* sv = g_ov.atlas.get(); g_ctx->PSSetShaderResources(0, 1, &sv);
    ID3D11SamplerState* ss = g_ov.smp.get(); g_ctx->PSSetSamplers(0, 1, &ss);
    g_ctx->Draw((UINT)n * 6, 0);
    ID3D11ShaderResourceView* none = nullptr; g_ctx->PSSetShaderResources(0, 1, &none);
}

// ---- Init ---------------------------------------------------------------------------------------------------------
bool EnsureInit()
{
    if (g_initTried) return g_initOk;
    g_initTried = true;
    g_hwnd = (HWND)XGpuCreateWindow(g_backW, g_backH);
    if (!g_hwnd) { Log("[gpu] window creation failed"); return false; }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1; sd.BufferDesc.Width = g_backW; sd.BufferDesc.Height = g_backH; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = { 60, 1 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = g_hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    UINT flags = 0;
    if (getenv("ITC_D3D_DEBUG")) flags |= D3D11_CREATE_DEVICE_DEBUG;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 1, D3D11_SDK_VERSION, &sd, g_swap.put(), g_dev.put(), &got, g_ctx.put());
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG))
    {
        Log("[gpu] D3D debug layer unavailable (hr=%08X), retrying without", (unsigned)hr);
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 1, D3D11_SDK_VERSION, &sd, g_swap.put(), g_dev.put(), &got, g_ctx.put());
    }
    if (FAILED(hr)) { Log("[gpu] D3D11CreateDeviceAndSwapChain failed hr=%08X", (unsigned)hr); return false; }
    if (flags & D3D11_CREATE_DEVICE_DEBUG)
    {
        if (SUCCEEDED(g_dev->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)g_infoQueue.put()))) Log("[gpu] D3D debug layer active");
    }
    IDXGIFactory* fac = nullptr;
    { IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr; if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd))) { if (SUCCEEDED(dd->GetAdapter(&ad))) { if (SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory), (void**)&fac))) { fac->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER); fac->Release(); } ad->Release(); } dd->Release(); } }
    {
        // Maximum number of frames queued before Present blocks: 1 (lowest input latency; costs no fps here, the GPU keeps up with the 30 fps game), ITC_FRAME_LATENCY overrides.
        static const GUID kIID_DXGIDevice1 = { 0x77db970f, 0x6276, 0x48ba, { 0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39, 0x2c } };
        const int lat = getenv("ITC_FRAME_LATENCY") ? std::max(1, std::min(16, atoi(getenv("ITC_FRAME_LATENCY")))) : 1;
        IDXGIDevice1* dd1 = nullptr;
        if (SUCCEEDED(g_dev->QueryInterface(kIID_DXGIDevice1, (void**)&dd1))) { dd1->SetMaximumFrameLatency((UINT)lat); dd1->Release(); Log("[gpu] maximum frame latency %d", lat); }
    }
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)g_backTex.put());
    g_dev->CreateRenderTargetView(g_backTex.get(), nullptr, g_backRTV.put());

    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = sizeof(XeCB); cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&cb, nullptr, g_cbVS.put());
    g_dev->CreateBuffer(&cb, nullptr, g_cbPS.put());
    cb.ByteWidth = 16;
    g_dev->CreateBuffer(&cb, nullptr, g_presentCB.put());
    cb.ByteWidth = 32;
    g_dev->CreateBuffer(&cb, nullptr, g_copyCB.put());
    {
        static const GUID kIID_DeviceContext1 = { 0xbb2c6faa, 0xb5fb, 0x4082, { 0x8e, 0x6b, 0x38, 0x8b, 0x8c, 0xfa, 0x90, 0xe1 } };   // ID3D11DeviceContext1
        D3D11_FEATURE_DATA_D3D11_OPTIONS opt = {};
        const bool haveOpt = SUCCEEDED(g_dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opt, sizeof opt));
        if (!getenv("ITC_NO_CB_OFFSETS") && haveOpt && opt.ConstantBufferOffsetting && opt.MapNoOverwriteOnDynamicConstantBuffer && SUCCEEDED(g_ctx->QueryInterface(kIID_DeviceContext1, (void**)g_ctx1.put())))
            g_cbOffsets = true;
        cb.ByteWidth = g_cbOffsets ? kCbDrawEntries * 256 : 16;
        g_dev->CreateBuffer(&cb, nullptr, g_cbDraw.put());
        Log("[gpu] per-draw constants: %s (ConstantBufferOffsetting=%d MapNoOverwriteOnDynamicConstantBuffer=%d MapNoOverwriteOnDynamicBufferSRV=%d)", g_cbOffsets ? "offset ring" : "small buffer per draw",
            haveOpt ? (int)opt.ConstantBufferOffsetting : -1, haveOpt ? (int)opt.MapNoOverwriteOnDynamicConstantBuffer : -1, haveOpt ? (int)opt.MapNoOverwriteOnDynamicBufferSRV : -1);
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
        if (!g_vbRing) Log("[gpu] vertex ring creation failed: persistent buffers only");
    }

    D3D11_BUFFER_DESC ib = {};
    ib.ByteWidth = kIdxRing * 4; ib.Usage = D3D11_USAGE_DYNAMIC; ib.BindFlags = D3D11_BIND_SHADER_RESOURCE; ib.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (getenv("ITC_IDX_UPDATE")) { ib.Usage = D3D11_USAGE_DEFAULT; ib.CPUAccessFlags = 0; }   // UpdateSubresource uploads (RenderDoc cannot cope with the big mapped ring)
    ib.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; ib.StructureByteStride = 4;
    g_dev->CreateBuffer(&ib, nullptr, g_idxBuf.put());
    D3D11_SHADER_RESOURCE_VIEW_DESC iv = {};
    iv.Format = DXGI_FORMAT_UNKNOWN; iv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; iv.Buffer.FirstElement = 0; iv.Buffer.NumElements = kIdxRing;
    g_dev->CreateShaderResourceView(g_idxBuf.get(), &iv, g_idxSRV.put());

    CP<ID3DBlob> b;
    if (CompileEntry(kPresentHlsl, "vsmain", "vs_5_0", b.put())) g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, g_presentVS.put());
    b.reset();
    if (CompileEntry(kPresentHlsl, "psmain", "ps_5_0", b.put())) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, g_presentPS.put());
    b.reset();
    if (CompileEntry(kCopyHlsl, "main", "ps_5_0", b.put())) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, g_copyPS.put());
    b.reset();
    if (CompileEntry(kCopyDepthHlsl, "main", "ps_5_0", b.put())) g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, g_copyDepthPS.put());
    D3D11_SAMPLER_DESC smd = {}; smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; smd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&smd, g_presentSampler.put());
    D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, g_presentRaster.put());

    {
        CP<ID3DBlob> gb;
        if (CompileHlsl(BuildRectListGeometryShader(), "gs_5_0", "rectlist_gs", gb.put()))
            g_dev->CreateGeometryShader(gb->GetBufferPointer(), gb->GetBufferSize(), nullptr, g_rectGS.put());
    }
    g_dummy2D = MakeDummy(D3D11_SRV_DIMENSION_TEXTURE2D);
    g_dummy3D = MakeDummy(D3D11_SRV_DIMENSION_TEXTURE3D);
    g_dummyCube = MakeDummy(D3D11_SRV_DIMENSION_TEXTURECUBE);
    InvalidateBound();
    Log("[gpu] D3D11 initialized (feature level %x)", (unsigned)got);
    g_initOk = true;
    return true;
}

// ---- Shader loading API ---------------------------------------------------------------------------------------------
}  // namespace

uint32_t D3dFrame() { return g_frame; }
uint32_t D3dCaptureFrame() { return g_captureFrame; }

void D3dLoadShader(bool isPixel, const uint32_t* words, uint32_t dwordCount)
{
    ShaderCode& sc = isPixel ? g_curPS : g_curVS;
    sc.code.assign(words, words + dwordCount);
    sc.hash = Hash64(words, dwordCount * 4ull) ^ (isPixel ? 0xA5A5A5A5ull : 0);
    sc.valid = true;
    if (g_log && g_frame == g_captureFrame) Log("[gpu] IM_LOAD %s hash=%016llx dwords=%u (draw #%llu)", isPixel ? "ps" : "vs", (unsigned long long)sc.hash, dwordCount, (unsigned long long)g_drawCount);
}

namespace {

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
inline void BindVSSrv(uint32_t slot, ID3D11ShaderResourceView* v) { if (!g_noFilter && g_bound.vsSrv[slot] == v) return; g_bound.vsSrv[slot] = v; g_ctx->VSSetShaderResources(slot, 1, &v); }
inline void BindPSSrv(uint32_t slot, ID3D11ShaderResourceView* v) { if (!g_noFilter && g_bound.psSrv[slot] == v) return; g_bound.psSrv[slot] = v; g_ctx->PSSetShaderResources(slot, 1, &v); }
inline void BindVSSmp(uint32_t slot, ID3D11SamplerState* v) { if (!g_noFilter && g_bound.vsSmp[slot] == v) return; g_bound.vsSmp[slot] = v; g_ctx->VSSetSamplers(slot, 1, &v); }
inline void BindPSSmp(uint32_t slot, ID3D11SamplerState* v) { if (!g_noFilter && g_bound.psSmp[slot] == v) return; g_bound.psSmp[slot] = v; g_ctx->PSSetSamplers(slot, 1, &v); }

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

// ---- Draw -----------------------------------------------------------------------------------------------------------

void UploadCB(ID3D11Buffer* buf, const XeCB& cb)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &cb, sizeof cb); g_ctx->Unmap(buf, 0); }
}

// Uploads a constant block into the ring and binds it to b0 of the stage; returns false when the ring is not available.
bool UploadCBRing(bool pixel, const XeCB& cb)
{
    if (!g_cbRing) return false;
    D3D11_MAPPED_SUBRESOURCE m;
    if (g_cbRingPos >= kCbRingEntries) g_cbRingPos = 0;
    if (FAILED(MapTimed(g_cbRing.get(), g_cbRingPos == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, &m, "constant ring"))) return false;
    memcpy((uint8_t*)m.pData + (size_t)g_cbRingPos * kCbRingStride, &cb, sizeof cb);
    g_ctx->Unmap(g_cbRing.get(), 0);
    ID3D11Buffer* b = g_cbRing.get();
    const UINT first = g_cbRingPos * (kCbRingStride / 16), num = kCbRingStride / 16;
    if (pixel) { g_ctx1->PSSetConstantBuffers1(0, 1, &b, &first, &num); g_cbRingFirstP = first; g_bound.psCb = b; }
    else { g_ctx1->VSSetConstantBuffers1(0, 1, &b, &first, &num); g_cbRingFirstV = first; g_bound.vsCb = b; }
    g_cbRingPos++;   // (the wrap to 0 is handled by the caller before the uploads of a draw)
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
{
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Buffer* b = g_cbDraw.get();
    if (g_cbOffsets)
    {
        if (FAILED(MapTimed(b, g_cbDrawPos == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, &m, "draw constants"))) return;
        uint32_t* d = (uint32_t*)((uint8_t*)m.pData + (size_t)g_cbDrawPos * 256);
        d[0] = startVertex; d[1] = 0; d[2] = 0; d[3] = 0;
        g_ctx->Unmap(b, 0);
        const UINT first = g_cbDrawPos * 16, num = 16;
        g_ctx1->VSSetConstantBuffers1(1, 1, &b, &first, &num);
        g_cbDrawPos = (g_cbDrawPos + 1) % kCbDrawEntries;
    }
    else
    {
        if (SUCCEEDED(g_ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { const uint32_t v[4] = { startVertex, 0, 0, 0 }; memcpy(m.pData, v, 16); g_ctx->Unmap(b, 0); }
        g_ctx->VSSetConstantBuffers(1, 1, &b);
    }
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
            hr = MapTimed(g_idxBuf.get(), D3D11_MAP_WRITE_DISCARD, &m, "index buffer");
            g_idxPos = 0; g_idxCache.clear(); g_idxFirst = false;
        }
        else
        {
            hr = MapTimed(g_idxBuf.get(), D3D11_MAP_WRITE_NO_OVERWRITE, &m, "index buffer");
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
    g_idxMisses++; g_forceVbCheck = true;
    std::vector<uint32_t> idx; D3D_PRIMITIVE_TOPOLOGY topo; bool rectList; uint32_t minIdx, maxIdx;
    if (!BuildIndices(dp, idx, topo, rectList, minIdx, maxIdx)) return nullptr;
    const uint32_t start = AppendIndices(idx.data(), (uint32_t)idx.size());
    if (start == ~0u) return nullptr;
    IdxEntry& e = g_idxCache[key];   // looked up after the append: it may have cleared the cache
    e = { k0, k1, k2, content, start, (uint32_t)idx.size(), minIdx, maxIdx, (uint32_t)topo, rectList };
    return &e;
}


}  // namespace

void D3dDraw(const XDrawParams& dp)
{
    if (!EnsureInit()) return;
    ProfScope profScope(g_profDraw);
    g_drawCount++;
    const uint32_t mode = XReg(xr::RB_MODECONTROL) & 7;
    extern void D3dDoResolve();
    if (mode == 6) { D3dDoResolve(); return; }
    if (mode != 4 && mode != 5) return;
    if (g_skipDrawsFrom >= 0 && (int)g_frame >= g_skipDrawsFrom) return;
    if (g_pendClear.color || g_pendClear.depth) ApplyPendingClears();
    {
        // Debug bisection: from frame ITC_LIMIT_BASE on, only the first (frame - base) * ITC_LIMIT_STEP draws of each frame are executed.
        static const int limBase = getenv("ITC_LIMIT_BASE") ? atoi(getenv("ITC_LIMIT_BASE")) : -1;
        static const int limStep = getenv("ITC_LIMIT_STEP") ? atoi(getenv("ITC_LIMIT_STEP")) : 1;
        static uint32_t limFrame = 0xFFFFFFFF, limCount = 0;
        if (limBase >= 0 && (int)g_frame >= limBase)
        {
            if (limFrame != g_frame) { limFrame = g_frame; limCount = 0; }
            const uint32_t dc0 = XReg(xr::RB_DEPTHCONTROL);
            const bool probe = (XReg(xr::RB_COLOR_MASK) & 0xFFFF) == 0 && !(((dc0 >> 1) & 1) && ((dc0 >> 2) & 1)) && !(dc0 & 1);
            if (!probe)
            {
                if (limCount >= (uint32_t)((int)g_frame - limBase) * (uint32_t)limStep) return;
                limCount++;
            }
        }
    }
    {
        // Draws that write no color and touch no depth/stencil (visibility-query probes) have no visible effect.
        const uint32_t dc = XReg(xr::RB_DEPTHCONTROL);
        const bool depthWrite = ((dc >> 1) & 1) && ((dc >> 2) & 1);
        if (mode == 4 && (XReg(xr::RB_COLOR_MASK) & 0xFFFF) == 0 && !depthWrite && !(dc & 1)) return;
    }
    if (!g_curVS.valid || !g_curPS.valid) { g_skippedDraws++; return; }
    if (g_envEvery > 1 && IsEnvPassState() && EnvPassSkipped()) { g_envDrawsSkipped++; return; }
    if (mode == 4 && !g_velocityNeeded && g_curPS.hash == kVelocityPsHash && ((XReg(xr::RB_COLOR_INFO) >> 16) & 0xF) == 4)
    {
        // Motion-vector pass: skipped (see g_velocityNeeded). The render target is still created so that the resolve at the end of the pass produces a (cleared) entry whose use is detected.
        static bool made = false;
        if (!made) { const Surface sf = CurrentSurface(); const uint32_t info = XReg(xr::RB_COLOR_INFO); made = GetColorRT(info & 0xFFF, 4, sf.w, sf.h, true) != nullptr; }
        g_velocitySkipped++; g_velSkippedThisPass++;
        return;
    }

    uint64_t T0 = QpcNow();
    ShaderEntry* vs = GetShader(g_curVS, false);
    ShaderEntry* ps = GetShader(g_curPS, true);
    if (!vs || !ps) { g_skippedDraws++; return; }
    g_dp[0] += QpcNow() - T0; T0 = QpcNow();

    std::vector<uint32_t> idx; D3D_PRIMITIVE_TOPOLOGY topo; bool rectList; uint32_t minIdx = 0, maxIdx = 0;
    uint32_t idxCount = 0, startVertex = 0;
    g_forceVbCheck = false;
    {
        // Frames that log index values need the converted list on the CPU; everything else uses the index cache.
        const bool detailed = g_frame == g_captureFrame || g_frame == g_traceFrame || g_log || g_noIdxCache;
        if (!detailed)
        {
            const IdxEntry* ie = GetIndexList(dp);
            if (!ie) { g_skippedDraws++; return; }
            topo = (D3D_PRIMITIVE_TOPOLOGY)ie->topo; rectList = ie->rectList; minIdx = ie->minIdx; maxIdx = ie->maxIdx; idxCount = ie->count; startVertex = ie->start;
        }
        else
        {
            if (!BuildIndices(dp, idx, topo, rectList, minIdx, maxIdx)) { g_skippedDraws++; return; }
            g_forceVbCheck = true;
            idxCount = (uint32_t)idx.size();
            startVertex = AppendIndices(idx.data(), idxCount);
            if (startVertex == ~0u) { g_skippedDraws++; return; }
        }
    }
    if (mode == 4 && idxCount >= 300) g_bigDraws++;
    g_dp[1] += QpcNow() - T0; T0 = QpcNow();

    // Render targets
    const Surface surf = CurrentSurface();
    ID3D11RenderTargetView* rtvs[4] = {};
    uint32_t numRTs = 0;
    uint32_t colorMask = XReg(xr::RB_COLOR_MASK);
    const uint32_t colorRegs[4] = { xr::RB_COLOR_INFO, xr::RB_COLOR1_INFO, xr::RB_COLOR2_INFO, xr::RB_COLOR3_INFO };
    float rtW = (float)surf.w, rtH = (float)surf.h;
    uint32_t actualW = surf.w, actualH = surf.h;
    if (mode == 4)
    {
        for (uint32_t i = 0; i < 4; i++)
        {
            const uint32_t info = XReg(colorRegs[i]);
            // Extra targets are only bound when the pixel shader writes them (the registers of unused slots hold stale values).
            if (i > 0 && (((colorMask >> (i * 4)) & 0xF) == 0 || !((ps->info.colorOutputsWritten >> i) & 1))) continue;
            ColorRT* rt = GetColorRT(info & 0xFFF, (info >> 16) & 0xF, surf.w, surf.h, true);
            if (!rt) continue;
            // All bound render targets must have identical dimensions or Direct3D silently ignores the whole binding.
            if (i > 0 && rtvs[0] && (rt->w != actualW || rt->h != actualH)) continue;
            rtvs[i] = rt->rtv.get();
            numRTs = i + 1;
            if (i == 0) { actualW = rt->w; actualH = rt->h; }
        }
        rtW = (float)actualW; rtH = (float)actualH;
    }
    const uint32_t depthCtl = XReg(xr::RB_DEPTHCONTROL);
    ID3D11DepthStencilView* dsv = nullptr;
    if ((depthCtl & 3) || mode == 5)
    {
        DepthRT* dr = GetDepthRT(XReg(xr::RB_DEPTH_INFO) & 0xFFF, actualW, actualH);
        if (dr) dsv = dr->dsv.get();
    }
    if (!rtvs[0] && !dsv) { g_skippedDraws++; return; }
    if (g_noFilter || memcmp(g_bound.rtv, rtvs, sizeof rtvs) != 0 || g_bound.dsv != dsv)
    {
        g_ctx->OMSetRenderTargets(4, rtvs, dsv);
        memcpy(g_bound.rtv, rtvs, sizeof rtvs); g_bound.dsv = dsv;
        InvalidateSrvs();   // D3D11 unbinds views of resources that are now render targets
        static int bad = 0;
        ID3D11RenderTargetView* chk = nullptr; g_ctx->OMGetRenderTargets(1, &chk, nullptr);
        if (chk) chk->Release();
        if (chk != rtvs[0] && bad++ < 20) Log("[gpu] WARNING: OMSetRenderTargets was rejected by Direct3D (rt0 %p, bound %p, dsv %p)", (void*)rtvs[0], (void*)chk, (void*)dsv);
    }

    g_dp[2] += QpcNow() - T0; T0 = QpcNow();
    g_dp[3] += QpcNow() - T0; T0 = QpcNow();
    // Resources
    int vbLo = 128, vbHi = -1;
    auto wantVS = [&](uint32_t t, ID3D11ShaderResourceView* v) { if (g_noFilter || g_bound.vsSrv[t] != v) { g_bound.vsSrv[t] = v; vbLo = std::min<int>(vbLo, (int)t); vbHi = std::max<int>(vbHi, (int)t); } };
    wantVS(31, g_idxSRV.get());
    for (uint32_t slot : vs->info.vfetchSlots)
    {
        const uint32_t reg = xr::SHADER_CONSTANT_FETCH_00_0 + (slot / 3) * 6 + (slot % 3) * 2;
        const uint32_t d0 = g_xregs[reg], d1 = g_xregs[reg + 1];
        ID3D11ShaderResourceView* srv = nullptr;
        g_vfBaseDw[slot] = 0;
        if ((d0 & 3) == 3)
        {
            const uint64_t tv = QpcNow();
            // Byte range this draw can read: vertex index range * stride (+ per-fetch offsets), from the translated shader.
            const uint32_t stride = vs->info.vfetchStride.count(slot) ? vs->info.vfetchStride.at(slot) : 0;
            const int64_t minOff = vs->info.vfetchMinOff.count(slot) ? vs->info.vfetchMinOff.at(slot) : 0, maxOff = vs->info.vfetchMaxOff.count(slot) ? vs->info.vfetchMaxOff.at(slot) : 4;
            int64_t r0 = ((int64_t)minIdx * stride + minOff) * 4, r1 = ((int64_t)maxIdx * stride + maxOff) * 4 + 16;
            if (r0 < 0) r0 = 0;
            if (r1 > 0x7FFFFFFF) r1 = 0x7FFFFFFF;
            srv = GetVertexBuffer(d0 & ~3u, ((d1 >> 2) & 0xFFFFFF) * 4, (uint32_t)r0, (uint32_t)r1, &g_vfBaseDw[slot]);
            g_dp[4] += QpcNow() - tv;
        }
        wantVS(32 + slot, srv);
    }
    if (vbHi >= 0)
    {
        for (int i = vbLo; i <= vbHi; i++) if (g_bound.vsSrv[i] == (ID3D11ShaderResourceView*)kUnk) g_bound.vsSrv[i] = nullptr;
        g_ctx->VSSetShaderResources(vbLo, vbHi - vbLo + 1, g_bound.vsSrv + vbLo);
    }
    static const char* dumpPs = getenv("ITC_DUMP_PS");   // hex prefix of a pixel shader hash: dump its bound textures and its render target (capture frame)
    char psHashStr[24]; snprintf(psHashStr, sizeof psHashStr, "%016llx", (unsigned long long)g_curPS.hash);
    const bool dumpThis = dumpPs && g_frame == g_captureFrame && strncmp(psHashStr, dumpPs, strlen(dumpPs)) == 0;
    static const bool dumpBound = getenv("ITC_DUMP_BOUND") != nullptr;
    const uint32_t dumpBoundFrame = g_captureFrame;
    for (int stage = 0; stage < 2; stage++)
    {
        ShaderEntry* se = stage ? ps : vs;
        // The views and samplers of all slots of the stage are compared with what is bound and sent in one call per kind covering the changed range.
        ID3D11ShaderResourceView** boundSrv = stage ? g_bound.psSrv : g_bound.vsSrv;
        ID3D11SamplerState** boundSmp = stage ? g_bound.psSmp : g_bound.vsSmp;
        int srvLo = 64, srvHi = -1, smpLo = 64, smpHi = -1;
        for (uint32_t slot : se->info.textureSlots)
        {
            const uint64_t tt = QpcNow();
            ID3D11ShaderResourceView* srv = GetTextureSRV(slot, se->info.dimOfSlot[slot]);
            g_dp[5] += QpcNow() - tt;
            if ((dumpThis || (dumpBound && g_frame == dumpBoundFrame)) && srv && stage == 1)
            {
                CP<ID3D11Resource> res; srv->GetResource(res.put());
                CP<ID3D11Texture2D> t2;
                if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)t2.put())))
                {
                    char nm[96]; snprintf(nm, sizeof nm, "bound%s_f%u_draw%llu_slot%u", getenv("ITC_DUMP_TAG") ? getenv("ITC_DUMP_TAG") : "", g_frame, (unsigned long long)g_drawCount, slot);
                    const TexFetch tfb = GetTexFetch(slot);
                    if (dumpThis || tfb.base() == 0x00C70000u || getenv("ITC_DUMP_BOUND_ALL")) DumpTextureResource(t2.get(), nm);
                }
            }
            ID3D11SamplerState* smp = GetSamplerFast(slot);
            if (g_noFilter || boundSrv[slot & 31] != srv) { boundSrv[slot & 31] = srv; srvLo = std::min<int>(srvLo, slot & 31); srvHi = std::max<int>(srvHi, slot & 31); }
            if (g_noFilter || boundSmp[slot & 15] != smp) { boundSmp[slot & 15] = smp; smpLo = std::min<int>(smpLo, slot & 15); smpHi = std::max<int>(smpHi, slot & 15); }
        }
        if (srvHi >= 0)
        {
            for (int i = srvLo; i <= srvHi; i++) if (boundSrv[i] == (ID3D11ShaderResourceView*)kUnk) boundSrv[i] = nullptr;   // unknown slots inside the range are simply cleared
            if (stage) g_ctx->PSSetShaderResources(srvLo, srvHi - srvLo + 1, boundSrv + srvLo); else g_ctx->VSSetShaderResources(srvLo, srvHi - srvLo + 1, boundSrv + srvLo);
        }
        if (smpHi >= 0)
        {
            for (int i = smpLo; i <= smpHi; i++) if (boundSmp[i] == (ID3D11SamplerState*)kUnk) boundSmp[i] = nullptr;
            if (stage) g_ctx->PSSetSamplers(smpLo, smpHi - smpLo + 1, boundSmp + smpLo); else g_ctx->VSSetSamplers(smpLo, smpHi - smpLo + 1, boundSmp + smpLo);
        }
    }

    g_dp[6] += QpcNow() - T0; T0 = QpcNow();
    // Constants: the two big buffers are rebuilt and uploaded only when a register they depend on changed since the last fill (XGpuWriteRegister keeps g_regDirty and
    // the fetch-entry versions), the vertex shader or the render target size changed, or a texture's component count changed (PatchSwizzles). The index offset of
    // the draw travels in the small b1 buffer.
    static XeCB cbv, cbp;
    static bool haveV = false, haveP = false; static uint64_t lastVsHash = 0; static float lastRtW = 0, lastRtH = 0;
    static const bool dbgRainbow = getenv("ITC_DEBUG_PS_RAINBOW") != nullptr;
    // Per-slot values as the shaders read them: the fetch constant's swizzle (patched for the texture's component count) and the vertex fetch metadata.
    auto patchedSwz = [](uint32_t slot) -> uint32_t
    {
        const uint32_t d3 = g_xregs[xr::SHADER_CONSTANT_FETCH_00_3 + slot * 6];
        const uint32_t sw = ((d3 >> 1) & 0xFFF) | (((d3 >> 13) & 0x3F) << 12);
        const uint32_t comps = g_slotComps[slot & 31];
        if (comps >= 3) return sw;
        uint32_t out = 0;
        for (uint32_t ch = 0; ch < 4; ch++)
        {
            uint32_t v = (sw >> (ch * 3)) & 7;
            if (comps == 1 && v >= 1 && v <= 3) v = 0;
            else if (comps == 2 && v >= 2 && v <= 3) v = 1;
            out |= v << (ch * 3);
        }
        return out | (sw & ~0xFFFu);
    };
    auto texSwzChanged = [&](const XeCB& cb, const ShaderEntry* se) { for (uint32_t slot : se->info.textureSlots) if (cb.texswz[slot & 31] != patchedSwz(slot & 31)) return true; return false; };
    auto vfMetaChanged = [&]()
    {
        for (uint32_t slot : vs->info.vfetchSlots)
        {
            const uint32_t d1 = g_xregs[xr::SHADER_CONSTANT_FETCH_00_0 + (slot / 3) * 6 + (slot % 3) * 2 + 1];
            if (cbv.vfmeta[slot][0] != g_vfBaseDw[slot] || cbv.vfmeta[slot][1] != ((d1 >> 2) & 0xFFFFFF) || cbv.vfmeta[slot][2] != (d1 & 3)) return true;
        }
        return false;
    };
    // When the constant ring cannot take this draw's two blocks it restarts at slot 0 with a WRITE_DISCARD: the old contents are gone, so the blocks that stay bound
    // from earlier draws (the stage whose constants did not change) must be uploaded again - otherwise they read undefined memory (black / garbage triangles).
    bool cbWrap = false;
    if (g_cbRing && g_cbRingPos + 2 > kCbRingEntries) { cbWrap = true; g_cbRingPos = 0; }
    const bool refillV = cbWrap || g_noCbDedupe || !haveV || (g_regDirty & (kDirtyVsFloat | kDirtyBoolLoop | kDirtyMisc)) || lastVsHash != g_curVS.hash || lastRtW != rtW || lastRtH != rtH || vfMetaChanged() || texSwzChanged(cbv, vs);
    const bool refillP = cbWrap || g_noCbDedupe || !haveP || dbgRainbow || (g_regDirty & (kDirtyPsFloat | kDirtyBoolLoop | kDirtyMisc)) || texSwzChanged(cbp, ps);
    if (refillV) { FillConstants(false, vs->info, cbv, rtW, rtH); for (uint32_t slot : vs->info.vfetchSlots) cbv.vfmeta[slot][0] = g_vfBaseDw[slot]; }
    if (refillP) FillConstants(true, ps->info, cbp, rtW, rtH);
    {
        // Debug: ITC_DUMP_CONSTS=<hex prefix of a pixel shader hash> logs every non-zero float constant (vertex + pixel) of the first such draw of the capture frame.
        static const char* dumpConsts = getenv("ITC_DUMP_CONSTS");
        static uint32_t dumpedFrame = 0xFFFFFFFFu;
        if (dumpConsts && g_frame == g_captureFrame && dumpedFrame != g_frame)
        {
            char hs[24]; snprintf(hs, sizeof hs, "%016llx", (unsigned long long)g_curPS.hash);
            if (strncmp(hs, dumpConsts, strlen(dumpConsts)) == 0)
            {
                dumpedFrame = g_frame;
                Log("[consts] draw #%llu ps=%s vs=%016llx", (unsigned long long)g_drawCount, hs, (unsigned long long)g_curVS.hash);
                for (int i = 0; i < 256; i++)
                    if (cbp.c[i][0] != 0 || cbp.c[i][1] != 0 || cbp.c[i][2] != 0 || cbp.c[i][3] != 0) Log("[consts]   ps c%d = %.6g %.6g %.6g %.6g", i, cbp.c[i][0], cbp.c[i][1], cbp.c[i][2], cbp.c[i][3]);
                for (int i = 0; i < 256; i++)
                    if (cbv.c[i][0] != 0 || cbv.c[i][1] != 0 || cbv.c[i][2] != 0 || cbv.c[i][3] != 0) Log("[consts]   vs c%d = %.6g %.6g %.6g %.6g", i, cbv.c[i][0], cbv.c[i][1], cbv.c[i][2], cbv.c[i][3]);
                Log("[consts]   ps bool consts: %08X %08X %08X %08X %08X %08X %08X %08X", cbp.bconst[0], cbp.bconst[1], cbp.bconst[2], cbp.bconst[3], cbp.bconst[4], cbp.bconst[5], cbp.bconst[6], cbp.bconst[7]);
                for (int i = 0; i < 32; i++) if (cbp.lconst[i]) Log("[consts]   loop const %d = %08X", i, cbp.lconst[i]);
            }
        }
    }
    {
        if (dbgRainbow && mode == 4 && !rectList && surf.w == 1040 && ((XReg(xr::RB_SURFACE_INFO) >> 16) & 3) == 1 && (XReg(xr::RB_COLOR_INFO) & 0xFFF) == 0 && ((XReg(xr::RB_COLOR_INFO) >> 16) & 0xF) == 3)
        {
            const unsigned long long hv = (unsigned long long)g_curPS.hash * 0x9E3779B97F4A7C15ull;
            const unsigned rgb = ((unsigned)(hv >> 40) & 0xFFFFFF) | 0x202020u;
            cbp.sys2[2] = (float)rgb;
            static std::set<unsigned long long> seen;
            if (seen.insert((unsigned long long)g_curPS.hash).second) Log("[dbg] rainbow ps=%016llx color=%06X", (unsigned long long)g_curPS.hash, rgb);
        }
    }
    if (refillV) { PatchSwizzles(cbv); if (!UploadCBRing(false, cbv)) UploadCB(g_cbVS.get(), cbv); haveV = true; lastVsHash = g_curVS.hash; lastRtW = rtW; lastRtH = rtH; g_cbUploadsV++; }
    if (refillP) { PatchSwizzles(cbp); if (!UploadCBRing(true, cbp)) UploadCB(g_cbPS.get(), cbp); haveP = true; g_cbUploadsP++; }
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
    }
    BindDrawConstants(startVertex);

    g_dp[7] += QpcNow() - T0; T0 = QpcNow();
    // State
    uint32_t blendRegs[4] = { XReg(xr::RB_BLENDCONTROL0), XReg(xr::RB_BLENDCONTROL1), XReg(xr::RB_BLENDCONTROL2), XReg(xr::RB_BLENDCONTROL3) };
    const float bf[4] = { F(XReg(xr::RB_BLEND_RED)), F(XReg(xr::RB_BLEND_GREEN)), F(XReg(xr::RB_BLEND_BLUE)), F(XReg(xr::RB_BLEND_ALPHA)) };
    {
        ID3D11BlendState* bs = GetBlendStateFast(blendRegs, colorMask, numRTs);
        uint32_t bfBits[4]; memcpy(bfBits, bf, sizeof bfBits);
        if (g_noFilter || g_bound.blend != bs || memcmp(g_bound.bf, bfBits, sizeof bfBits) != 0) { g_ctx->OMSetBlendState(bs, bf, 0xFFFFFFFF); g_bound.blend = bs; memcpy(g_bound.bf, bfBits, sizeof bfBits); }
        const uint32_t stRef = XReg(xr::RB_STENCILREFMASK) & 0xFF;
        ID3D11DepthStencilState* dss = GetDepthStateFast(depthCtl, XReg(xr::RB_STENCILREFMASK));
        if (g_noFilter || g_bound.ds != dss || g_bound.stRef != stRef) { g_ctx->OMSetDepthStencilState(dss, stRef); g_bound.ds = dss; g_bound.stRef = stRef; }
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
        if (g_noFilter || g_bound.rs != rss) { g_ctx->RSSetState(rss); g_bound.rs = rss; }
    }
    D3D11_VIEWPORT vp = { 0, 0, rtW, rtH, 0.0f, 1.0f };
    if (g_noFilter || memcmp(&g_bound.vp, &vp, sizeof vp) != 0) { g_ctx->RSSetViewports(1, &vp); g_bound.vp = vp; }
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
        if (g_noFilter || memcmp(&g_bound.sc, &sr, sizeof sr) != 0) { g_ctx->RSSetScissorRects(1, &sr); g_bound.sc = sr; }
    }
    if (g_noFilter || g_bound.topo != (int)topo) { g_ctx->IASetPrimitiveTopology(topo); g_bound.topo = (int)topo; }
    {
        ID3D11GeometryShader* gs = rectList ? g_rectGS.get() : nullptr;
        if (g_noFilter || g_bound.vs != vs->vs.get()) { g_ctx->VSSetShader(vs->vs.get(), nullptr, 0); g_bound.vs = vs->vs.get(); }
        if (g_noFilter || g_bound.gs != gs) { g_ctx->GSSetShader(gs, nullptr, 0); g_bound.gs = gs; }
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
        if (g_noFilter || g_bound.ps != psObj) { g_ctx->PSSetShader(psObj, nullptr, 0); g_bound.ps = psObj; }
    }
    if (g_frame == g_traceFrame)
    {
        ID3D11RenderTargetView* r0 = nullptr; g_ctx->OMGetRenderTargets(1, &r0, nullptr);
        CP<ID3D11Resource> rr; if (r0) { r0->GetResource(rr.put()); r0->Release(); }
        ID3D11ShaderResourceView* s0 = nullptr; g_ctx->PSGetShaderResources(0, 1, &s0);
        CP<ID3D11Resource> sr; if (s0) { s0->GetResource(sr.put()); s0->Release(); }
        Log("[trace] DRAW #%llu prim=%u n=%u rt0=%p srv0=%p ps=%016llx", (unsigned long long)g_drawCount, dp.primType, idxCount, (void*)rr.get(), (void*)sr.get(), (unsigned long long)g_curPS.hash);
    }
    g_dp[8] += QpcNow() - T0; T0 = QpcNow();
    if (g_gpuMode >= 1) GpuTimingBeforeDraw(PassKey(mode, XReg(xr::RB_COLOR_INFO), actualW, actualH, (colorMask & 0xFFFF) == 0, (XReg(xr::RB_SURFACE_INFO) >> 16) & 3), g_curPS.hash);
    g_ctx->Draw(idxCount, 0);
    if (g_gpuMode >= 2) GpuTimingAfterDraw();
    g_dp[9] += QpcNow() - T0;
    if (dumpThis && rtvs[0])
    {
        CP<ID3D11Resource> rr; rtvs[0]->GetResource(rr.put());
        CP<ID3D11Texture2D> t2;
        if (rr && SUCCEEDED(rr->QueryInterface(__uuidof(ID3D11Texture2D), (void**)t2.put())))
        {
            char nm[96]; snprintf(nm, sizeof nm, "rtafter_f%u_draw%llu", g_frame, (unsigned long long)g_drawCount);
            DumpTextureResource(t2.get(), nm);
        }
    }

    {
        const uint32_t logFrame = g_captureFrame;
        if (g_logVerbose && g_frame == logFrame && dp.primType != 1)
        {
            Log("[gpu]   sys0=(%g %g %g %g) sys1=(%g %g %g) rt=%gx%g idx0..=%u %u %u", cbv.sys0[0], cbv.sys0[1], cbv.sys0[2], cbv.sys0[3], cbv.sys1[0], cbv.sys1[1], cbv.sys1[2], rtW, rtH,
                idx.size() > 0 ? idx[0] : 0, idx.size() > 1 ? idx[1] : 0, idx.size() > 2 ? idx[2] : 0);
            Log("[gpu]   colorcontrol=%08X alpharef=%g sys2=(%g %g) polyoff front scale=%g offset=%g back scale=%g offset=%g depthinfo=%08X stencilrefmask=%08X", XReg(xr::RB_COLORCONTROL), F(XReg(xr::RB_ALPHA_REF)), cbp.sys2[0], cbp.sys2[1],
                F(XReg(xr::PA_SU_POLY_OFFSET_FRONT_SCALE)), F(XReg(xr::PA_SU_POLY_OFFSET_FRONT_OFFSET)), F(XReg(xr::PA_SU_POLY_OFFSET_BACK_SCALE)), F(XReg(xr::PA_SU_POLY_OFFSET_BACK_OFFSET)), XReg(xr::RB_DEPTH_INFO), XReg(xr::RB_STENCILREFMASK));
            Log("[gpu]   ps c254=(%g %g %g %g) c255=(%g %g %g %g) c0=(%g %g %g %g) c33=(%g %g %g %g)", cbp.c[254][0], cbp.c[254][1], cbp.c[254][2], cbp.c[254][3], cbp.c[255][0], cbp.c[255][1], cbp.c[255][2], cbp.c[255][3],
                cbp.c[0][0], cbp.c[0][1], cbp.c[0][2], cbp.c[0][3], cbp.c[33][0], cbp.c[33][1], cbp.c[33][2], cbp.c[33][3]);
            Log("[gpu]   vs c0=(%g %g %g %g) c1=(%g %g %g %g)", cbv.c[0][0], cbv.c[0][1], cbv.c[0][2], cbv.c[0][3], cbv.c[1][0], cbv.c[1][1], cbv.c[1][2], cbv.c[1][3]);
            for (uint32_t slot : vs->info.vfetchSlots)
            {
                const uint32_t reg = xr::SHADER_CONSTANT_FETCH_00_0 + (slot / 3) * 6 + (slot % 3) * 2;
                const uint32_t d0 = g_xregs[reg], d1 = g_xregs[reg + 1];
                const uint8_t* v = PhysPtr(d0 & ~3u);
                float f[10];
                for (int i = 0; i < 10; i++) { uint32_t raw; memcpy(&raw, v + i * 4, 4); f[i] = F(ApplyEndian(raw, d1 & 3)); }
                Log("[gpu]   vslot %u d0=%08X d1=%08X floats: %g %g %g %g %g | %g %g %g %g %g", slot, d0, d1, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]);
                if (rectList)
                {
                    // the three corners of a rect list, as floats at the vertex stride of the translated shader
                    const uint32_t strideB = (vs->info.vfetchStride.count(slot) ? vs->info.vfetchStride.at(slot) : 10) * 4;   // stride is in dwords
                    for (int vi = 0; vi < 3 && vi < (int)idx.size(); vi++)
                    {
                        const uint8_t* vp = v + (size_t)idx[vi] * strideB;
                        float g[10]; for (int i = 0; i < 10; i++) { uint32_t raw; memcpy(&raw, vp + i * 4, 4); g[i] = F(ApplyEndian(raw, d1 & 3)); }
                        Log("[gpu]   rect vertex %d (index %u stride %u bytes): %g %g %g %g | %g %g %g %g | %g %g", vi, idx[vi], strideB, g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9]);
                    }
                }
            }
            const uint32_t tl = XReg(xr::PA_SC_WINDOW_SCISSOR_TL), br = XReg(xr::PA_SC_WINDOW_SCISSOR_BR);
            Log("[gpu]   wscissor tl=%08X br=%08X screen tl=%08X br=%08X winoff=%08X modeCntl=%08X clip=%08X", tl, br, XReg(xr::PA_SC_SCREEN_SCISSOR_TL), XReg(xr::PA_SC_SCREEN_SCISSOR_BR), XReg(xr::PA_SC_WINDOW_OFFSET), sc, clipCntl);
        }
    }

    if (g_log)
    {
        static int n = 0;
        const uint32_t logFrame = g_captureFrame;
        if (n++ < 60 || g_frame == logFrame)
            Log("[gpu] draw #%llu prim=%u count=%u (%zu idx) vs=%016llx ps=%016llx rt0=%08X surf=%ux%u pitch=%u msaa=%u depthctl=%08X blend0=%08X mask=%X tex(vs/ps)=%zu/%zu vf=%zu vte=%X mode=%u first idx=%u modecntl=%X reset=%X po=(%g %g) stref=%08X clip=%08X sqpc=%08X",
                (unsigned long long)g_drawCount, dp.primType, dp.indexCount, (size_t)idxCount, (unsigned long long)g_curVS.hash, (unsigned long long)g_curPS.hash,
                XReg(xr::RB_COLOR_INFO), surf.w, surf.h, surf.pitch, (XReg(xr::RB_SURFACE_INFO) >> 16) & 3, depthCtl, blendRegs[0], colorMask,
                vs->info.textureSlots.size(), ps->info.textureSlots.size(), vs->info.vfetchSlots.size(), XReg(xr::PA_CL_VTE_CNTL), mode, idx.empty() ? 0u : idx[0], XReg(xr::PA_SU_SC_MODE_CNTL), XReg(xr::VGT_MULTI_PRIM_IB_RESET_INDX), F(XReg(xr::PA_SU_POLY_OFFSET_FRONT_SCALE)), F(XReg(xr::PA_SU_POLY_OFFSET_FRONT_OFFSET)), XReg(xr::RB_STENCILREFMASK), XReg(xr::PA_CL_CLIP_CNTL), XReg(xr::SQ_PROGRAM_CNTL));
    }
}

namespace { void WritePng(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t pitch); }
// ---- Resolve (EDRAM -> guest memory) ------------------------------------------------------------------------------------
namespace {


void DumpResolvedTexture(const Resolved& r, uint32_t base);
void DumpTextureResource(ID3D11Texture2D* tex, const char* name);
// Copies a rectangle of a render target (colour SRV, or depth SRV when isDepth) into the GPU-side resolve cache entry for `destBase`.
bool GpuResolve(ID3D11ShaderResourceView* srcSrv, bool isDepth, int x0, int y0, uint32_t rw, uint32_t rh, uint32_t destBase, uint32_t destPitch, uint32_t destHeight, uint32_t destEndian, uint32_t destFmt, bool swapRB, int expBias)
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
    g_resolvedGen++;     // memoised texture lookups may point at the entry this resolve replaces or grows
    InvalidateBound();   // the copy pass below sets its own pipeline state

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
            else if (!(sameLayout && rw >= old.w && rh >= old.h)) { g_flushReason = "resolve-neither"; FlushResolved(destBase); }   // neither contains the other: make memory authoritative
            // else: the new resolve fully covers the old one; it simply replaces it.
        }
        // Older pending resolves at other addresses that overlap this destination must reach memory before this one.
        g_flushReason = "resolve-overlap"; FlushOverlapping(destBase, newBytes, destBase);
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
        uint32_t cbd[8] = { (uint32_t)x0, (uint32_t)(y0 - (int)yOff), rw, rh, swapRB ? 1u : 0u, (uint32_t)expBias, 0, 0 };
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



void DumpResolvedTexture(const Resolved& r, uint32_t base)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = r.w; d.Height = r.h; d.MipLevels = 1; d.ArraySize = 1; d.Format = r.dxgi; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CP<ID3D11Texture2D> st;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, st.put()))) return;
    g_ctx->CopyResource(st.get(), r.tex.get());
    D3D11_MAPPED_SUBRESOURCE mp;
    if (FAILED(g_ctx->Map(st.get(), 0, D3D11_MAP_READ, 0, &mp))) return;
    uint32_t bpp = 4;
    switch (r.dxgi) { case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: bpp = 8; break; default: bpp = 4; }
    std::vector<uint8_t> rgba((size_t)r.w * r.h * 4);
    for (uint32_t y = 0; y < r.h; y++)
        for (uint32_t x = 0; x < r.w; x++)
        {
            const F4 c = ReadPixel((const uint8_t*)mp.pData + (size_t)y * mp.RowPitch + (size_t)x * bpp, r.dxgi);
            uint8_t* o = &rgba[((size_t)y * r.w + x) * 4];
            for (int i = 0; i < 3; i++) o[i] = (uint8_t)Q8(c.v[i]);
            o[3] = 255;
        }
    g_ctx->Unmap(st.get(), 0);
    static int n = 0;
    CreateDirectoryA("C:/itc-work/dumps", nullptr);
    char path[256]; snprintf(path, sizeof path, "C:/itc-work/dumps/res_f%u_%02d_%08X_fmt%u.png", g_frame, n++, base, r.fmt);
    WritePng(path, rgba.data(), r.w, r.h, r.w * 4);
    Log("[gpu] dumped resolved texture %s (%ux%u)", path, r.w, r.h);
}


void DumpTextureResource(ID3D11Texture2D* tex, const char* name)
{
    D3D11_TEXTURE2D_DESC sd;
    tex->GetDesc(&sd);
    D3D11_TEXTURE2D_DESC d = sd;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    CP<ID3D11Texture2D> st;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, st.put()))) return;
    g_ctx->CopyResource(st.get(), tex);
    D3D11_MAPPED_SUBRESOURCE mp;
    if (FAILED(g_ctx->Map(st.get(), 0, D3D11_MAP_READ, 0, &mp))) return;
    DXGI_FORMAT f = sd.Format;
    if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS) f = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t bpp = 4;
    switch (f) { case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R32G32_FLOAT: bpp = 8; break; case DXGI_FORMAT_R8_UNORM: bpp = 1; break; case DXGI_FORMAT_R8G8_UNORM: bpp = 2; break; default: bpp = 4; }
    std::vector<uint8_t> rgba((size_t)sd.Width * sd.Height * 4);
    for (uint32_t y = 0; y < sd.Height; y++)
        for (uint32_t x = 0; x < sd.Width; x++)
        {
            const uint8_t* src = (const uint8_t*)mp.pData + (size_t)y * mp.RowPitch + (size_t)x * bpp;
            uint8_t* o = &rgba[((size_t)y * sd.Width + x) * 4];
            if (bpp == 1) { o[0] = o[1] = o[2] = src[0]; o[3] = 255; continue; }
            if (bpp == 2) { o[0] = src[0]; o[1] = src[1]; o[2] = 0; o[3] = 255; continue; }
            const F4 c = ReadPixel(src, f);
            for (int i = 0; i < 3; i++) o[i] = (uint8_t)Q8(c.v[i]);
            o[3] = 255;
        }
    g_ctx->Unmap(st.get(), 0);
    CreateDirectoryA("C:/itc-work/dumps", nullptr);
    char path[256]; snprintf(path, sizeof path, "C:/itc-work/dumps/%s.png", name);
    WritePng(path, rgba.data(), sd.Width, sd.Height, sd.Width * 4);
    Log("[gpu] dumped %s (%ux%u fmt %u)", path, sd.Width, sd.Height, (unsigned)sd.Format);
}

CP<ID3D11Texture2D> g_staging;
uint32_t g_stagingW = 0, g_stagingH = 0;
DXGI_FORMAT g_stagingFmt = DXGI_FORMAT_UNKNOWN;

}  // namespace

void D3dDoResolve()
{
    ProfScope profScope(g_profResolve);
    g_resolveCount++;
    if (g_envEvery > 1 && IsEnvPassState() && EnvPassSkipped()) { g_envResolvesSkipped++; return; }   // the environment pass of this frame is not rendered: keep the old maps
    if (g_gpuMode >= 1) GpuTimingBeforeDraw(kPassResolve, 0);
    const uint32_t cc = XReg(xr::RB_COPY_CONTROL);
    const uint32_t src = cc & 7;
    const bool clearColor = (cc >> 8) & 1, clearDepth = (cc >> 9) & 1;
    const uint32_t destBase = XReg(xr::RB_COPY_DEST_BASE) & 0x1FFFFFFF;
    const uint32_t destPitchReg = XReg(xr::RB_COPY_DEST_PITCH);
    const uint32_t di = XReg(xr::RB_COPY_DEST_INFO);
    const uint32_t destEndian = di & 7, destFmt = (di >> 7) & 0x3F;
    const bool swapRB = (di >> 24) & 1;
    const uint32_t destPitch = destPitchReg & 0x3FFF, destHeight = (destPitchReg >> 16) & 0x3FFF;

    const Surface surf = CurrentSurface();
    const uint32_t colorRegs[4] = { xr::RB_COLOR_INFO, xr::RB_COLOR1_INFO, xr::RB_COLOR2_INFO, xr::RB_COLOR3_INFO };
    ColorRT* rt = nullptr;
    if (src < 4)
    {
        const uint32_t info = XReg(colorRegs[src]);
        rt = GetColorRT(info & 0xFFF, (info >> 16) & 0xF, surf.w, surf.h, false);
    }

    // Resolve rectangle: the three vertices of the RectangleList (vertex fetch constant 0).
    int32_t x0 = 0, y0 = 0, x1 = rt ? (int32_t)surf.w : 0, y1 = rt ? (int32_t)surf.h : 0;
    {
        const uint32_t d0 = XReg(xr::SHADER_CONSTANT_FETCH_00_0), d1 = XReg(xr::SHADER_CONSTANT_FETCH_00_1);
        if ((d0 & 3) == 3 && ((d1 >> 2) & 0xFFFFFF) >= 6)
        {
            const uint8_t* vp = PhysPtr(d0 & ~3u);
            if (CommittedBytes(vp, 24) == 24)
            {
                float f[6];
                for (int i = 0; i < 6; i++) { uint32_t raw; memcpy(&raw, vp + i * 4, 4); f[i] = F(ApplyEndian(raw, d1 & 3)); }
                if (g_log && g_frame == g_captureFrame)
                    Log("[gpu]   resolve verts: (%g,%g) (%g,%g) (%g,%g) winoff=%08X wscissor=%08X/%08X", f[0], f[1], f[2], f[3], f[4], f[5], XReg(xr::PA_SC_WINDOW_OFFSET), XReg(xr::PA_SC_WINDOW_SCISSOR_TL), XReg(xr::PA_SC_WINDOW_SCISSOR_BR));
                const float minx = std::min(f[0], std::min(f[2], f[4])), maxx = std::max(f[0], std::max(f[2], f[4]));
                const float miny = std::min(f[1], std::min(f[3], f[5])), maxy = std::max(f[1], std::max(f[3], f[5]));
                if (maxx > minx && maxy > miny && maxx < 16384 && maxy < 16384) { x0 = (int32_t)ceilf(minx); y0 = (int32_t)ceilf(miny); x1 = (int32_t)ceilf(maxx); y1 = (int32_t)ceilf(maxy); }
            }
        }
    }
    if (rt) { x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min<int32_t>(x1, (int32_t)rt->w); y1 = std::min<int32_t>(y1, (int32_t)rt->h); }

    if (g_log)
    {
        static int n = 0;
        const uint32_t logFrameR = g_captureFrame;
        if (n++ < 40 || g_frame == logFrameR) Log("[gpu] resolve #%llu src=%u rect=(%d,%d)-(%d,%d) dest=%08X pitch=%u h=%u fmt=%u endian=%u swap=%u clear c=%u d=%u rt=%s destNumber=%u destExpBias=%d srcExpBias=%d",
                          (unsigned long long)g_resolveCount, src, x0, y0, x1, y1, destBase, destPitch, destHeight, destFmt, destEndian, swapRB, clearColor, clearDepth, rt ? "yes" : "none",
                          (di >> 13) & 7, ((int32_t)(di << 10)) >> 26, src < 4 ? ((int32_t)(XReg(colorRegs[src]) << 6)) >> 26 : 0);
    }

    static const bool cpuResolveAll = getenv("ITC_CPU_RESOLVE") != nullptr;
    static const uint32_t cpuResolveMask = getenv("ITC_CPU_RESOLVE_MASK") ? (uint32_t)strtoul(getenv("ITC_CPU_RESOLVE_MASK"), nullptr, 0) : 0;
    static uint32_t resFrame = 0xFFFFFFFF, resIdx = 0;
    if (resFrame != g_frame) { resFrame = g_frame; resIdx = 0; }
    const bool cpuResolve = cpuResolveAll || ((cpuResolveMask >> std::min(resIdx, 31u)) & 1);
    resIdx++;
    bool gpuDone = false;
    if (!cpuResolve && rt && x1 > x0 && y1 > y0 && destPitch)
        gpuDone = GpuResolve(rt->srv.get(), false, x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), destBase, destPitch, destHeight, destEndian, destFmt, swapRB, ((int32_t)(di << 10)) >> 26);
    if (gpuDone)
    {
        auto rit = g_resolved.find(destBase);
        if (rit != g_resolved.end()) rit->second.velocity = rt && src == 0 && rt->fmt == 4 && g_velSkippedThisPass > 0;   // the (empty) result of the skipped motion-vector pass
    }
    g_velSkippedThisPass = 0;
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
                GpuResolve(dr.srv.get(), true, dx0, dy0, (uint32_t)(dx1 - dx0), (uint32_t)(dy1 - dy0), destBase, destPitch, destHeight, destEndian, destFmt, false, 0);
        }
    }
    if (!gpuDone && rt && x1 > x0 && y1 > y0 && destPitch)
    {
        g_ev.cpuResolves++;
        static int cpuLogged = 0;
        if (cpuLogged++ < 12) Log("[gpu] CPU resolve fallback: src=%u rect=(%d,%d)-(%d,%d) dest=%08X pitch=%u h=%u destFmt=%u endian=%u rt fmt=%u (dxgi %u)", src, x0, y0, x1, y1, destBase, destPitch, destHeight, destFmt, destEndian, rt->fmt, (unsigned)rt->dxgi);
        const uint32_t rw = x1 - x0, rh = y1 - y0;
        {
            uint32_t qb, qc; ResolvedFormat(destFmt, qb, qc);
            const uint32_t qbpp = qb ? qb : 4;
            g_flushReason = "cpu-resolve-overlap"; FlushOverlapping(destBase, (size_t)((destPitch + 31) & ~31u) * ((std::max(destHeight, rh) + 31) & ~31u) * qbpp);
            g_resolved.erase(destBase); g_resolvedGen++;
        }
        if (!g_staging || g_stagingW < rw || g_stagingH < rh || g_stagingFmt != rt->dxgi)
        {
            D3D11_TEXTURE2D_DESC d = {};
            g_stagingW = std::max(rw, g_stagingW); g_stagingH = std::max(rh, g_stagingH);
            d.Width = g_stagingW; d.Height = g_stagingH; d.MipLevels = 1; d.ArraySize = 1; d.Format = rt->dxgi; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            g_stagingFmt = rt->dxgi;
            g_dev->CreateTexture2D(&d, nullptr, g_staging.put());
        }
        D3D11_BOX box = { (UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1 };
        g_ctx->CopySubresourceRegion(g_staging.get(), 0, 0, 0, 0, rt->tex.get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mp;
        HRESULT mapHr;
        { ProfScope pm(g_profMap); mapHr = g_ctx->Map(g_staging.get(), 0, D3D11_MAP_READ, 0, &mp); }
        if (SUCCEEDED(mapHr))
        {
            static const bool dumpRes = getenv("ITC_DUMP_RESOLVE") != nullptr;
            static int dumpedRes = 0;
            const uint32_t dumpFrame = g_captureFrame;
            if (dumpRes && (dumpedRes < 12 || g_frame == dumpFrame) && dumpedRes < 400 && (rt->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM || rt->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
            {
                CreateDirectoryA("C:/itc-work/dumps", nullptr);
                char path[256]; snprintf(path, sizeof path, "C:/itc-work/dumps/resolve_%02d_f%u.png", dumpedRes++, g_frame);
                WritePng(path, (const uint8_t*)mp.pData, rw, rh, mp.RowPitch);
                Log("[gpu] dumped %s", path);
            }
            uint32_t srcBpp = 4;
            switch (rt->dxgi) { case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT: srcBpp = 8; break; default: break; }
            uint32_t dummy[2];
            const uint32_t bpp = PackPixel(F4{ { 0, 0, 0, 0 } }, destFmt, false, dummy);
            uint32_t log2bpp = 0; while ((1u << log2bpp) < bpp) log2bpp++;
            const uint32_t alignedPitch = (destPitch + 31) & ~31u;
            uint8_t* dest = PhysPtrW(destBase);
            const size_t tiledBytes = (size_t)alignedPitch * (((destHeight ? destHeight : rh) + 31) & ~31u) * bpp;
            const size_t writable = CommittedBytes(dest, tiledBytes);
            const bool fastRgba8 = (rt->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM || rt->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) && bpp == 4 && destEndian != 4 && (destFmt == 6 || destFmt == 14);
            for (uint32_t y = 0; y < rh; y++)
            {
                const uint8_t* row = (const uint8_t*)mp.pData + (size_t)y * mp.RowPitch;
                if (fastRgba8)
                {
                    const uint32_t outer = TiledOuter(y, alignedPitch, 2);
                    const uint32_t* srcRow = (const uint32_t*)row;
                    const uint32_t lim = std::min(rw, alignedPitch);
                    for (uint32_t x = 0; x < lim; x++)
                    {
                        uint32_t v = srcRow[x];
                        if (swapRB) v = (v & 0xFF00FF00u) | ((v & 0xFF) << 16) | ((v >> 16) & 0xFF);
                        v = ApplyEndian(v, destEndian);
                        const uint32_t off = TiledInner(x, y, 2, outer);
                        if (off + 4 <= writable) memcpy(dest + off, &v, 4);
                    }
                    continue;
                }
                for (uint32_t x = 0; x < rw && x < alignedPitch; x++)
                {
                    uint32_t px[2];
                    PackPixel(ReadPixel(row + (size_t)x * srcBpp, rt->dxgi), destFmt, swapRB, px);
                    const uint32_t off = TiledByteOffset(x, y, alignedPitch, log2bpp);
                    if (off + bpp > writable) continue;
                    if (bpp == 1) { dest[off] = (uint8_t)px[0]; continue; }
                    for (uint32_t i = 0; i < bpp / 4; i++)
                    {
                        uint32_t v = px[i];
                        if (destEndian == 4) { /* 8in64 handled below */ }
                        else v = ApplyEndian(v, destEndian);
                        memcpy(dest + off + i * 4, &v, 4);
                    }
                    if (destEndian == 4 && bpp == 8)
                    {
                        const uint64_t q = px[0] | ((uint64_t)px[1] << 32);
                        const uint64_t sw = __builtin_bswap64(q);
                        memcpy(dest + off, &sw, 8);
                    }
                }
            }
            g_ctx->Unmap(g_staging.get(), 0);
            NoteMemWrite(destBase, tiledBytes);
        }
    }

    // While a tiled frame is in progress the clears that follow each strip's resolve are skipped: the frame was drawn once, the later
    // strips still have to read it, and the next frame starts with its own clear draws.
    const bool tilingNow = XGpuTilingActive();
    if (clearColor && rt)
    {
        const uint32_t cv = XReg(xr::RB_COLOR_CLEAR);
        float col[4] = { (cv & 0xFF) / 255.0f, ((cv >> 8) & 0xFF) / 255.0f, ((cv >> 16) & 0xFF) / 255.0f, ((cv >> 24) & 0xFF) / 255.0f };
        if (!tilingNow) g_ctx->ClearRenderTargetView(rt->rtv.get(), col);
        else { g_pendClear.color = true; g_pendClear.rt = rt; memcpy(g_pendClear.col, col, sizeof col); }
    }
    if (clearDepth)
    {
        const uint32_t dv = XReg(xr::RB_DEPTH_CLEAR);
        const uint32_t dbase = XReg(xr::RB_DEPTH_INFO) & 0xFFF;
        if (!tilingNow)
        {
            auto it = g_depthRTs.find(dbase);
            if (it != g_depthRTs.end() && it->second.dsv)
                g_ctx->ClearDepthStencilView(it->second.dsv.get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, (float)(dv >> 8) / 16777215.0f, (UINT8)(dv & 0xFF));
        }
        else { g_pendClear.depth = true; g_pendClear.dbase = dbase; g_pendClear.dz = (float)(dv >> 8) / 16777215.0f; g_pendClear.dstencil = (UINT8)(dv & 0xFF); }
    }
}

// ---- PNG dump (debug) ------------------------------------------------------------------------------------------------------
namespace {

CP<ID3D11Texture2D> g_dumpStaging;
void DumpBackbuffer(const char* tag)
{
    if (!g_dumpStaging)
    {
        D3D11_TEXTURE2D_DESC d = {}; d.Width = g_backW; d.Height = g_backH; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        g_dev->CreateTexture2D(&d, nullptr, g_dumpStaging.put());
    }
    g_ctx->CopyResource(g_dumpStaging.get(), g_backTex.get());
    D3D11_MAPPED_SUBRESOURCE mp;
    if (SUCCEEDED(g_ctx->Map(g_dumpStaging.get(), 0, D3D11_MAP_READ, 0, &mp)))
    {
        CreateDirectoryA("C:/itc-work/dumps", nullptr);
        char path[256]; snprintf(path, sizeof path, "C:/itc-work/dumps/%s%s_%05u.png", tag, getenv("ITC_DUMP_TAG") ? getenv("ITC_DUMP_TAG") : "", g_frame);
        WritePng(path, (const uint8_t*)mp.pData, g_backW, g_backH, mp.RowPitch);
        g_ctx->Unmap(g_dumpStaging.get(), 0);
        Log("[gpu] dumped %s", path);
    }
}
}  // namespace

// ---- Swap ------------------------------------------------------------------------------------------------------------------
// RenderDoc in-application API: ITC_RDOC_FRAME=<n> captures the draws of presented-frame number n (run the game through renderdoccmd / RenderDoc so renderdoc.dll is loaded).
static RENDERDOC_API_1_1_2* g_rdoc = nullptr;
static bool g_rdocCapturing = false;
static uint32_t RdocFrame()
{
    static const uint32_t f = getenv("ITC_RDOC_FRAME") ? (uint32_t)atoi(getenv("ITC_RDOC_FRAME")) : 0xFFFFFFFFu;
    return f;
}

void D3dSwap(uint32_t frontbufferPhys, uint32_t width, uint32_t height)
{
    if (!EnsureInit()) return;
    ProfScope profScope(g_profSwap);
    (void)frontbufferPhys; (void)width; (void)height;
    if (RdocFrame() != 0xFFFFFFFFu)
    {
        if (!g_rdoc)
        {
            if (HMODULE m = GetModuleHandleA("renderdoc.dll"))
                if (pRENDERDOC_GetAPI get = (pRENDERDOC_GetAPI)GetProcAddress(m, "RENDERDOC_GetAPI")) get(eRENDERDOC_API_Version_1_1_2, (void**)&g_rdoc);
            if (g_rdoc) { CreateDirectoryA("C:/itc-work/rdoc", nullptr); g_rdoc->SetCaptureFilePathTemplate("C:/itc-work/rdoc/itc"); Log("[rdoc] RenderDoc API available"); }
            else { static bool warned = false; if (!warned) { warned = true; Log("[rdoc] ITC_RDOC_FRAME set but renderdoc.dll is not loaded (start the game with renderdoccmd capture)"); } }
        }
        if (g_rdoc && g_rdocCapturing && g_frame == RdocFrame())
        {
            g_rdoc->EndFrameCapture(nullptr, nullptr); g_rdocCapturing = false; Log("[rdoc] ended capture of frame %u", g_frame);
            if (getenv("ITC_RDOC_QUIT")) { Log("[rdoc] ITC_RDOC_QUIT: exiting"); Sleep(1500); TerminateProcess(GetCurrentProcess(), 0); }
        }
    }
    g_lastFrameDraws = g_drawCount - g_frameDrawsStart; g_frameDrawsStart = g_drawCount;
    if (g_envFrameDecided == g_frame && !g_envFrameSkip && g_lastFrameDraws > 1200) g_envRenderedOnce = true;   // a race-sized frame went through the environment pass
    g_frame++;
    if (g_rdoc && g_frame == RdocFrame()) { g_rdoc->StartFrameCapture(nullptr, nullptr); g_rdocCapturing = true; Log("[rdoc] started capture of frame %u", g_frame); }
    if (g_capture3dAfter >= 0 && g_captureFrame == 0xFFFFFFFFu)
    {
        static const uint64_t t0 = QpcNow();
        LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
        if ((double)(QpcNow() - t0) / (double)qf.QuadPart >= g_capture3dAfter && g_bigDraws >= 3)
        {
            g_captureFrame = g_frame;
            Log("[capture] logging frame %u (%u large draws in the previous frame)", g_frame, g_bigDraws);
        }
    }
    g_bigDraws = 0;
    {
        static const bool fps = getenv("ITC_FPS") != nullptr;
        static LARGE_INTEGER freq = {}, last = {};
        if (fps)
        {
            if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&last); }
            if ((g_frame % 60) == 0)
            {
                LARGE_INTEGER now; QueryPerformanceCounter(&now);
                const double ms = 1000.0 / freq.QuadPart;
                Log("[fps] frame %u: %.1f fps  draws=%llu resolves=%llu texbytes=%zuKB | per-60-frames ms: map=%.0f decode=%.0f hash=%.0f resolve=%.0f swap(incl.present)=%.0f draw=%.0f shader=%.0f", g_frame, 60.0 * freq.QuadPart / (double)(now.QuadPart - last.QuadPart), (unsigned long long)g_drawCount, (unsigned long long)g_resolveCount, g_texBytes / 1024,
                    g_profMap * ms, g_profDecode * ms, g_profHash * ms, g_profResolve * ms, g_profSwap * ms, g_profDraw * ms, g_profShader * ms);
                Log("[fps]   decodes=%llu bytes=%llu commit=%.0f hashIn=%.0f upload=%.0f", (unsigned long long)g_decodeCount, (unsigned long long)g_decodeBytes, g_profCommit * ms, g_profHashIn * ms, g_profUpload * ms); g_profCommit = g_profHashIn = g_profUpload = 0; g_decodeCount = g_decodeBytes = 0;
                g_profMap = g_profDecode = g_profHash = g_profResolve = g_profSwap = g_profDraw = g_profShader = 0;
                last = now;
            }
        }
    }
    const TexFetch tf = GetTexFetch(0);
    ID3D11ShaderResourceView* srv = nullptr;
    if (tf.type() == 2) srv = GetTextureSRV(0, tf.dimension() == 0 ? 0 : 1);
    if (g_log && g_frame <= 60)
    {
        static uint64_t lastDraws = 0, lastSkipped = 0, lastRes = 0;
        Log("[gpu] frame %u: draws=%llu skipped=%llu resolves=%llu (shaders ok/fail %llu/%llu)", g_frame, (unsigned long long)(g_drawCount - lastDraws), (unsigned long long)(g_skippedDraws - lastSkipped),
            (unsigned long long)(g_resolveCount - lastRes), (unsigned long long)g_shaderOk, (unsigned long long)g_shaderFail);
        lastDraws = g_drawCount; lastSkipped = g_skippedDraws; lastRes = g_resolveCount;
    }
    if (g_log && g_frame <= 5)
        Log("[gpu] swap frame %u: front fetch %08X %08X %08X %08X %08X %08X (format %u tiled %u pitch %u)", g_frame, tf.d[0], tf.d[1], tf.d[2], tf.d[3], tf.d[4], tf.d[5], tf.format(), tf.tiled(), tf.pitchTexels());
    if (srv && g_presentVS && g_presentPS)
    {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(g_presentCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { uint32_t sw[4] = { tf.swizzle(), 0, 0, 0 }; memcpy(m.pData, sw, 16); g_ctx->Unmap(g_presentCB.get(), 0); }
        InvalidateBound();
        ID3D11RenderTargetView* rtv = g_backRTV.get();
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        g_ctx->OMSetDepthStencilState(nullptr, 0);
        g_ctx->RSSetState(g_presentRaster.get());
        D3D11_VIEWPORT vp = { 0, 0, (float)g_backW, (float)g_backH, 0, 1 };
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_presentVS.get(), nullptr, 0);
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->PSSetShader(g_presentPS.get(), nullptr, 0);
        ID3D11Buffer* cb = g_presentCB.get(); g_ctx->PSSetConstantBuffers(0, 1, &cb);
        g_ctx->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState* ss = g_presentSampler.get(); g_ctx->PSSetSamplers(0, 1, &ss);
        g_ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr; g_ctx->PSSetShaderResources(0, 1, &none);
    }
    else
    {
        const float c[4] = { 0.1f, 0.0f, 0.2f, 1.0f };
        g_ctx->ClearRenderTargetView(g_backRTV.get(), c);
    }
    static const int dumpFrom = getenv("ITC_DUMP_FROM") ? atoi(getenv("ITC_DUMP_FROM")) : -1;
    if (((g_dumpEvery && dumpFrom < 0 && (g_frame % g_dumpEvery) == 0) || (dumpFrom >= 0 && (int)g_frame >= dumpFrom && (g_dumpEvery == 0 || ((int)g_frame - dumpFrom) % (int)g_dumpEvery == 0))) && g_dumped < g_dumpMax) { g_dumped++; DumpBackbuffer("frame"); }
    if (g_capture3dAfter >= 0 && g_captureFrame != 0xFFFFFFFFu && g_frame == g_captureFrame + 1) DumpBackbuffer("cap");
    if (g_thumbWanted.load() && (g_frame % 6) == 0)
    {
        static CP<ID3D11Texture2D> thumbStaging;
        if (!thumbStaging)
        {
            D3D11_TEXTURE2D_DESC d = {}; d.Width = g_backW; d.Height = g_backH; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            g_dev->CreateTexture2D(&d, nullptr, thumbStaging.put());
        }
        D3D11_MAPPED_SUBRESOURCE tm;
        if (thumbStaging && SUCCEEDED((g_ctx->CopyResource(thumbStaging.get(), g_backTex.get()), g_ctx->Map(thumbStaging.get(), 0, D3D11_MAP_READ, 0, &tm))))
        {
            uint8_t t[64 * 36];
            for (int by = 0; by < 36; by++)
                for (int bx = 0; bx < 64; bx++)
                {
                    uint32_t sum = 0;
                    for (int y = 0; y < 20; y++)
                    {
                        const uint8_t* row = (const uint8_t*)tm.pData + (size_t)(by * 20 + y) * tm.RowPitch + bx * 20 * 4;
                        for (int x = 0; x < 20; x++) sum += (row[x * 4] * 77 + row[x * 4 + 1] * 150 + row[x * 4 + 2] * 29) >> 8;
                    }
                    t[by * 64 + bx] = (uint8_t)(sum / 400);
                }
            g_ctx->Unmap(thumbStaging.get(), 0);
            std::lock_guard<std::mutex> l(g_thumbMutex);
            memcpy(g_thumbData, t, sizeof t); g_thumbFrame = g_frame;
        }
    }
    {
        // A frame with draws skipped because their shaders are still compiling in the background would flicker, so the last complete frame
        // stays on screen until the compile finishes (at most ~6 s).
        const bool incomplete = g_shaderPending != g_pendingMark;
        g_pendingMark = g_shaderPending;
        if (!g_lastGoodTex && g_backTex)
        {
            D3D11_TEXTURE2D_DESC bd; g_backTex->GetDesc(&bd);
            bd.BindFlags = 0; bd.MiscFlags = 0; bd.CPUAccessFlags = 0; bd.Usage = D3D11_USAGE_DEFAULT;
            g_dev->CreateTexture2D(&bd, nullptr, g_lastGoodTex.put());
        }
        if (g_lastGoodTex)
        {
            if (incomplete && g_haveGood && g_heldFrames < 180) { g_ctx->CopyResource(g_backTex.get(), g_lastGoodTex.get()); g_heldFrames++; }
            else { g_ctx->CopyResource(g_lastGoodTex.get(), g_backTex.get()); g_haveGood = true; g_heldFrames = 0; }
        }
    }
    {
        // FPS counter: presented frames per second over 0.5 s windows, shown in the title bar and in the overlay (F3 toggles the overlay).
        static LARGE_INTEGER qf = {};
        static uint64_t winStart = 0, lastSwapT = 0, drawTicksPrev = 0, drawsPrev = 0, cpuPrev = 0;
        static uint32_t winFrames = 0;
        static double worstMs = 0, fps = 0, avgMs = 0, worst = 0, cpuPct = 0, drawPct = 0, drawsPerFrame = 0;
        static bool f3Prev = false;
        if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
        auto threadCpu = []() -> uint64_t { FILETIME c, e, k, u; GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u); return (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime); };
        const uint64_t now = QpcNow();
        if (lastSwapT) worstMs = std::max(worstMs, (double)(now - lastSwapT) * 1000.0 / (double)qf.QuadPart);
        {
            // Hitch diagnostics: a frame slower than ITC_SLOW_MS (default 45) is logged together with what happened in it (at most 20 per 2 s window).
            static const double slowMs = getenv("ITC_SLOW_MS") ? atof(getenv("ITC_SLOW_MS")) : 45.0;
            static FrameEvents prev = {}; static uint64_t prevDraws = 0, prevDecodes = 0, prevSkipped = 0, prevDrawTicks = 0, prevIdxMiss = 0, prevResolves = 0, prevPresent = 0, prevStall = 0, prevDumped = 0, prevIdle = 0, prevFence = 0;
            extern uint64_t g_cpIdleTicks, g_cpFenceWaitTicks;
            static int logged = 0; static uint32_t logWin = 0;
            const double ms = lastSwapT ? (double)(now - lastSwapT) * 1000.0 / (double)qf.QuadPart : 0.0;
            if (g_frame / 60 != logWin) { logWin = g_frame / 60; logged = 0; }
            if (ms > slowMs && logged++ < 20)
                Log("[slow] frame %u: %.0f ms (draw thread %.0f ms, present %.1f ms, map stalls %.1f ms, ring empty %.1f ms, fence waits %.1f ms%s): draws %llu skipped +%llu resolves +%llu | decodes +%llu texCreated +%llu texUpdated +%llu texHashed +%llu (%llu KB) | vb created +%llu pagesCmp +%llu changed +%llu up %llu KB | rtCreated +%llu cpuResolves +%llu flushes +%llu shaderCompiles +%llu idxMiss +%llu pendingShaders %llu",
                    g_frame, ms, (double)(g_profDraw - prevDrawTicks) * 1000.0 / (double)qf.QuadPart, (double)(g_profPresent - prevPresent) * 1000.0 / (double)qf.QuadPart, (double)(g_mapStall - prevStall) * 1000.0 / (double)qf.QuadPart, (double)(g_cpIdleTicks - prevIdle) * 1000.0 / (double)qf.QuadPart, (double)(g_cpFenceWaitTicks - prevFence) * 1000.0 / (double)qf.QuadPart, g_dumped != prevDumped ? ", PNG dump" : "",
                    (unsigned long long)(g_drawCount - prevDraws), (unsigned long long)(g_skippedDraws - prevSkipped), (unsigned long long)(g_resolveCount - prevResolves),
                    (unsigned long long)(g_decodeCount - prevDecodes), (unsigned long long)(g_ev.texCreated - prev.texCreated), (unsigned long long)(g_ev.texUpdated - prev.texUpdated), (unsigned long long)(g_ev.texHashed - prev.texHashed), (unsigned long long)((g_ev.texBytes - prev.texBytes) / 1024),
                    (unsigned long long)(g_ev.vbCreated - prev.vbCreated), (unsigned long long)(g_ev.vbPagesCmp - prev.vbPagesCmp), (unsigned long long)(g_ev.vbPagesChanged - prev.vbPagesChanged), (unsigned long long)((g_ev.vbBytesUp - prev.vbBytesUp) / 1024),
                    (unsigned long long)(g_ev.rtCreated - prev.rtCreated), (unsigned long long)(g_ev.cpuResolves - prev.cpuResolves), (unsigned long long)(g_ev.flushes - prev.flushes), (unsigned long long)(g_ev.shaderCompiles - prev.shaderCompiles), (unsigned long long)(g_idxMisses - prevIdxMiss), (unsigned long long)g_shaderPending);
            prev = g_ev; prevDraws = g_drawCount; prevDecodes = g_decodeCount; prevSkipped = g_skippedDraws; prevDrawTicks = g_profDraw; prevIdxMiss = g_idxMisses; prevResolves = g_resolveCount; prevPresent = g_profPresent; prevStall = g_mapStall; prevDumped = g_dumped; prevIdle = g_cpIdleTicks; prevFence = g_cpFenceWaitTicks;
        }
        lastSwapT = now; winFrames++;
        if (!winStart) { winStart = now; drawTicksPrev = g_profDraw; drawsPrev = g_drawCount; cpuPrev = threadCpu(); winFrames = 0; }
        const double winSec = (double)(now - winStart) / (double)qf.QuadPart;
        if (winSec >= 0.5 && winFrames > 0)
        {
            const uint64_t cpuNow = threadCpu();
            fps = winFrames / winSec; avgMs = 1000.0 / fps; worst = worstMs;
            cpuPct = 100.0 * (double)(cpuNow - cpuPrev) * 1e-7 / winSec;
            drawPct = g_profDraw >= drawTicksPrev ? 100.0 * (double)(g_profDraw - drawTicksPrev) / (double)qf.QuadPart / winSec : 0.0;
            drawsPerFrame = (double)(g_drawCount - drawsPrev) / winFrames;
            char title[160];
            snprintf(title, sizeof title, "Tokyo Xtreme Racer (static recompilation) - %.1f FPS  (%.1f ms)", fps, avgMs);
            SetWindowTextA(g_hwnd, title);
            winStart = now; winFrames = 0; worstMs = 0; drawTicksPrev = g_profDraw; drawsPrev = g_drawCount; cpuPrev = cpuNow;
        }
        if (XGpuWindowFocused())
        {
            const bool f3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
            if (f3 && !f3Prev) g_ov.visible = !g_ov.visible;
            f3Prev = f3;
        }
        if (g_ov.visible && fps > 0)
        {
            char l0[80], l1[80], l2[80];
            snprintf(l0, sizeof l0, "FPS %5.1f   %5.1f ms  (worst %5.1f)", fps, avgMs, worst);
            snprintf(l1, sizeof l1, "draws/frame %4.0f   render thread %3.0f%% cpu (%3.0f%% in draws)", drawsPerFrame, cpuPct, drawPct);
            snprintf(l2, sizeof l2, "gpu frame span %5.1f ms        F3 = hide", g_gpuSpanEma);
            const char* lines[3] = { l0, l1, l2 };
            const int colors[3] = { fps >= 55.0 ? 2 : fps >= 28.0 ? 3 : 4, 0, 5 };
            OverlayDraw(lines, colors, g_gpuSpanEma > 0 ? 3 : 2);
            static const bool dumpOv = getenv("ITC_DUMP_OVERLAY") != nullptr;   // test: PNG of the back buffer including the overlay
            if (dumpOv && (g_frame % 200) == 0 && g_dumped < g_dumpMax) { g_dumped++; DumpBackbuffer("ovl"); }
        }
    }
    GpuTimingEndFrame();
    {
        // Diagnostic (ITC_BLACK_DETECT=1): once per second the back buffer is sampled; a black picture is logged together with what the frame did.
        static const bool detect = getenv("ITC_BLACK_DETECT") != nullptr;
        static CP<ID3D11Texture2D> stg;
        if (detect && (g_frame % 30) == 0 && g_backTex)
        {
            if (!stg)
            {
                D3D11_TEXTURE2D_DESC d = {}; d.Width = g_backW; d.Height = g_backH; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                g_dev->CreateTexture2D(&d, nullptr, stg.put());
            }
            D3D11_MAPPED_SUBRESOURCE bm;
            if (stg && SUCCEEDED((g_ctx->CopyResource(stg.get(), g_backTex.get()), g_ctx->Map(stg.get(), 0, D3D11_MAP_READ, 0, &bm))))
            {
                uint32_t lit = 0, total = 0;
                for (uint32_t y = 40; y < g_backH; y += 80)
                    for (uint32_t x = 40; x < g_backW; x += 80)
                    {
                        const uint8_t* px = (const uint8_t*)bm.pData + (size_t)y * bm.RowPitch + x * 4;
                        total++; if (px[0] + px[1] + px[2] > 12) lit++;
                    }
                g_ctx->Unmap(stg.get(), 0);
                static bool wasBlack = false;
                const bool black = lit == 0;
                if (black != wasBlack || (black && (g_frame % 300) == 0))
                    Log("[black] frame %u: picture is %s (%u of %u samples lit); draws in the last frame %llu, skipped draws total %llu, pending shaders %llu, velocity pass %s, env pass every %u",
                        g_frame, black ? "BLACK" : "visible again", lit, total, (unsigned long long)g_lastFrameDraws, (unsigned long long)g_skippedDraws, (unsigned long long)g_shaderPending, g_velocityNeeded ? "on" : "skipped", g_envEvery);
                wasBlack = black;
            }
        }
    }
    {
        const uint64_t tp = QpcNow();
        const HRESULT phr = g_swap->Present(1, 0);
        g_profPresent += QpcNow() - tp;
        if (phr != S_OK && phr != DXGI_STATUS_OCCLUDED)
        {
            static int n = 0;
            if (n++ < 20) Log("[gpu] Present returned %08X (device removed reason %08X)", (unsigned)phr, (unsigned)g_dev->GetDeviceRemovedReason());
        }
    }
    GpuTimingBeginFrame();
    GpuTimingCollect();
    if (g_infoQueue)
    {
        static int printed = 0;
        const UINT64 n = g_infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < n && printed < 80; i++)
        {
            SIZE_T len = 0;
            g_infoQueue->GetMessage(i, nullptr, &len);
            std::vector<uint8_t> buf(len);
            auto* msg = (D3D11_MESSAGE*)buf.data();
            if (SUCCEEDED(g_infoQueue->GetMessage(i, msg, &len))) { Log("[d3d] %s", msg->pDescription); printed++; }
        }
        g_infoQueue->ClearStoredMessages();
    }

    // periodic cleanup of unused vertex buffers
    if ((g_frame & 255) == 0)
        for (auto it = g_vbs.begin(); it != g_vbs.end();)
            if (g_frame - it->second.lastFrame > 600) { g_vbBytes -= it->second.shadow.size(); it = g_vbs.erase(it); } else ++it;
}

void D3dIntervalStats(double seconds)
{
    static uint64_t lastDraws = 0, lastRes = 0, lastDecode = 0, lastSkipped = 0, lastShaders = 0;
    static uint64_t pDec = 0, pHash = 0, pRes = 0, pSwap = 0, pDraw = 0, pMap = 0;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const double ms = 1000.0 / f.QuadPart;
    {
        static uint64_t lastFlush = 0, lastFlushBytes = 0, lastFlushProf = 0;
        Log("[stat]   resolved-texture flushes: +%llu (%llu KB), %.0f ms", (unsigned long long)(g_flushCount - lastFlush), (unsigned long long)((g_flushBytes - lastFlushBytes) / 1024), (g_profFlush - lastFlushProf) * ms);
        for (auto& kv : g_flushByReason) Log("[stat]     flush reason %-20s count=%llu bytes=%llu KB", kv.first.c_str(), (unsigned long long)kv.second.first, (unsigned long long)(kv.second.second / 1024));
        g_flushByReason.clear();
        lastFlush = g_flushCount; lastFlushBytes = g_flushBytes; lastFlushProf = g_profFlush;
    }
    {
        // The command-processor thread: its CPU time, the time it was blocked in Present and the time in draws, per presented frame of this window.
        static uint64_t lastCpu = 0, lastFrames = 0, lastPresent = 0, lastDrawT = 0;
        FILETIME c, e, k, u; GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
        const uint64_t cpu = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
        const uint64_t nf = g_frame > lastFrames ? g_frame - lastFrames : 1;
        static uint64_t lastStall = 0, lastStallN = 0;
        Log("[stat]   cp thread: cpu %.1f ms/frame, blocked in Present %.1f ms/frame, draws %.1f ms/frame, map stalls %.1f ms/frame (%llu) (%llu frames, %.0f%% of wall time busy)", (double)(cpu - lastCpu) * 1e-4 / nf, (g_profPresent - lastPresent) * ms / nf, (g_profDraw - lastDrawT) * ms / nf,
            (g_mapStall - lastStall) * ms / nf, (unsigned long long)(g_mapStallCount - lastStallN), (unsigned long long)nf, 100.0 * (double)(cpu - lastCpu) * 1e-7 / seconds);
        lastCpu = cpu; lastFrames = g_frame; lastPresent = g_profPresent; lastDrawT = g_profDraw; lastStall = g_mapStall; lastStallN = g_mapStallCount;
    }
    Log("[stat] %.1fs: frames=%u draws+%llu (skipped +%llu) resolves+%llu decodes+%llu shaders+%llu | ms: draw=%.0f decode=%.0f hash=%.0f resolve=%.0f swap=%.0f",
        seconds, g_frame, (unsigned long long)(g_drawCount - lastDraws), (unsigned long long)(g_skippedDraws - lastSkipped), (unsigned long long)(g_resolveCount - lastRes),
        (unsigned long long)(g_decodeCount - lastDecode), (unsigned long long)(g_shaderOk - lastShaders),
        (g_profDraw - pDraw) * ms, (g_profDecode - pDec) * ms, (g_profHash - pHash) * ms, (g_profResolve - pRes) * ms, (g_profSwap - pSwap) * ms);
    {
        static uint64_t last[12] = {};
        Log("[stat]   draw breakdown ms: shader=%.0f idxbuild=%.0f rt=%.0f idxupload=%.0f vb=%.0f tex=%.0f(+binds) consts=%.0f state=%.0f drawcall=%.0f (resources total=%.0f)",
            (g_dp[0] - last[0]) * ms, (g_dp[1] - last[1]) * ms, (g_dp[2] - last[2]) * ms, (g_dp[3] - last[3]) * ms, (g_dp[4] - last[4]) * ms, (g_dp[5] - last[5]) * ms,
            (g_dp[7] - last[7]) * ms, (g_dp[8] - last[8]) * ms, (g_dp[9] - last[9]) * ms, (g_dp[6] - last[6]) * ms);
        memcpy(last, g_dp, sizeof last);
    }
    {
        static uint64_t lh = 0, lm = 0, lr = 0, lv = 0;
        {
            static FrameEvents pv = {};
            Log("[stat]   events: tex hashed +%llu created +%llu updated +%llu (%llu KB decoded) memo misses +%llu | vb created +%llu pages compared +%llu changed +%llu uploaded %llu KB ring copies +%llu (%llu KB) | rt created +%llu cpu resolves +%llu flushes +%llu shader compiles +%llu",
                (unsigned long long)(g_ev.texHashed - pv.texHashed), (unsigned long long)(g_ev.texCreated - pv.texCreated), (unsigned long long)(g_ev.texUpdated - pv.texUpdated), (unsigned long long)((g_ev.texBytes - pv.texBytes) / 1024), (unsigned long long)(g_ev.texMemoMiss - pv.texMemoMiss),
                (unsigned long long)(g_ev.vbCreated - pv.vbCreated), (unsigned long long)(g_ev.vbPagesCmp - pv.vbPagesCmp), (unsigned long long)(g_ev.vbPagesChanged - pv.vbPagesChanged), (unsigned long long)((g_ev.vbBytesUp - pv.vbBytesUp) / 1024),
                (unsigned long long)(g_ev.vbRingCopies - pv.vbRingCopies), (unsigned long long)((g_ev.vbRingBytes - pv.vbRingBytes) / 1024),
                (unsigned long long)(g_ev.rtCreated - pv.rtCreated), (unsigned long long)(g_ev.cpuResolves - pv.cpuResolves), (unsigned long long)(g_ev.flushes - pv.flushes), (unsigned long long)(g_ev.shaderCompiles - pv.shaderCompiles));
            pv = g_ev;
        }
        { static uint64_t le = 0, lre = 0; Log("[stat]   environment pass: draws skipped +%llu, resolves skipped +%llu (every %u frames)", (unsigned long long)(g_envDrawsSkipped - le), (unsigned long long)(g_envResolvesSkipped - lre), g_envEvery); le = g_envDrawsSkipped; lre = g_envResolvesSkipped; }
        static uint64_t lcv = 0, lcp = 0;
        static uint64_t lup = 0;
        Log("[stat]   index cache: hits +%llu misses +%llu resets +%llu (list=%zu), motion-vector pass draws skipped +%llu%s, constant buffer uploads vs +%llu ps +%llu, shader upgrades +%llu", (unsigned long long)(g_idxHits - lh), (unsigned long long)(g_idxMisses - lm), (unsigned long long)(g_idxResets - lr),
            g_idxCache.size(), (unsigned long long)(g_velocitySkipped - lv), g_velocityNeeded ? " (pass active)" : "", (unsigned long long)(g_cbUploadsV - lcv), (unsigned long long)(g_cbUploadsP - lcp), (unsigned long long)(g_shaderUpgrades - lup));
        lh = g_idxHits; lm = g_idxMisses; lr = g_idxResets; lv = g_velocitySkipped; lcv = g_cbUploadsV; lcp = g_cbUploadsP; lup = g_shaderUpgrades;
    }
    GpuTimingPrint();
    lastDraws = g_drawCount; lastRes = g_resolveCount; lastDecode = g_decodeCount; lastSkipped = g_skippedDraws; lastShaders = g_shaderOk;
    pDec = g_profDecode; pHash = g_profHash; pRes = g_profResolve; pSwap = g_profSwap; pDraw = g_profDraw; pMap = g_profMap; (void)pMap;
}

void D3dStats(char* out, size_t outSize)
{
    snprintf(out, outSize, "frames=%u draws=%llu resolves=%llu shaders ok/fail=%llu/%llu skipped=%llu textures=%zu (%zu KB) vbs=%zu",
             g_frame, (unsigned long long)g_drawCount, (unsigned long long)g_resolveCount, (unsigned long long)g_shaderOk, (unsigned long long)g_shaderFail,
             (unsigned long long)g_skippedDraws, g_textures.size(), g_texBytes / 1024, g_vbs.size());
}
