p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()

def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)

rep('#include <algorithm>\n#include <cmath>\n#include <map>\n#include <string>\n#include <unordered_map>\n#include <vector>\n',
    '#include <algorithm>\n#include <atomic>\n#include <cmath>\n#include <condition_variable>\n#include <deque>\n#include <map>\n#include <memory>\n#include <mutex>\n#include <string>\n#include <thread>\n#include <unordered_map>\n#include <vector>\n',
    'includes')

rep('uint64_t g_drawCount = 0, g_resolveCount = 0, g_shaderFail = 0, g_shaderOk = 0, g_skippedDraws = 0;',
    'uint64_t g_drawCount = 0, g_resolveCount = 0, g_shaderFail = 0, g_shaderOk = 0, g_skippedDraws = 0, g_shaderPending = 0, g_shaderCacheHits = 0;', 'counters')

# ShaderEntry gets a pending compile job
rep('''struct ShaderEntry
{
    CP<ID3D11VertexShader> vs;
    CP<ID3D11PixelShader> ps;
    XShaderInfo info;
    bool failed = false;
};''', '''// Compiling translated HLSL costs 50-300 ms per shader and a level has hundreds of them, so shaders are compiled on worker threads and
// their bytecode is cached on disk (shader_cache/): a draw whose shader is still compiling is skipped for a few frames instead of
// stalling the command processor. ITC_SYNC_SHADERS=1 waits for the compile instead (deterministic captures).
struct CompileJob
{
    std::string hlsl, target, tag;
    std::string cachePath;
    std::atomic<int> state{ 0 };      // 0 = compiling, 1 = done, 2 = failed
    std::vector<uint8_t> bytecode;
};
struct ShaderEntry
{
    CP<ID3D11VertexShader> vs;
    CP<ID3D11PixelShader> ps;
    XShaderInfo info;
    bool failed = false;
    std::shared_ptr<CompileJob> job;   // set while the compile is in flight
};''', 'ShaderEntry')

old_start = s.index('ShaderEntry* GetShader(const ShaderCode& sc, bool isPixel)')
old_end = s.index('// ---- Constant buffers')
new_get = r'''std::mutex g_jobMutex;
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
        if (CompileHlsl(job->hlsl, job->target.c_str(), job->tag.c_str(), blob.put()))
        {
            const uint8_t* b = (const uint8_t*)blob->GetBufferPointer();
            job->bytecode.assign(b, b + blob->GetBufferSize());
            // Persist the bytecode (write to a temporary name first so a killed process never leaves a truncated cache file).
            const std::string tmp = job->cachePath + ".tmp";
            if (FILE* f = fopen(tmp.c_str(), "wb"))
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
        if (!e.job) return &e;
        if (syncShaders) while (e.job->state.load() == 0) Sleep(1);
        const int st = e.job->state.load();
        if (st == 0) { g_shaderPending++; return nullptr; }
        std::shared_ptr<CompileJob> job = e.job;
        e.job.reset();
        if (st != 1 || !CreateShaderObject(e, isPixel, job->bytecode.data(), job->bytecode.size())) { e.failed = true; g_shaderFail++; return nullptr; }
        g_shaderOk++;
        return &e;
    }

    ShaderEntry& e = g_shaders[key];
    XTranslateParams tp;
    tp.isPixel = isPixel; tp.psInterpolatorCount = interp; tp.psParamGenReg = pgen;
    std::string hlsl = TranslateXenosShader(sc.code.data(), (uint32_t)sc.code.size(), tp, e.info);
    char tag[64]; snprintf(tag, sizeof tag, "%s_%016llx_%u", isPixel ? "ps" : "vs", (unsigned long long)sc.hash, interp);
    const char* target = isPixel ? "ps_5_0" : "vs_5_0";

    // Disk cache keyed by the generated HLSL (so translator changes invalidate it automatically).
    uint64_t srcHash = 0xCBF29CE484222325ull ^ (isPixel ? 0x77ull : 0x33ull);
    for (unsigned char ch : hlsl) { srcHash ^= ch; srcHash *= 0x100000001B3ull; }   // FNV-1a (a cache key must not collide)
    char cacheFile[160]; snprintf(cacheFile, sizeof cacheFile, "shader_cache/%s_%016llx.bin", target, (unsigned long long)srcHash);
    if (FILE* f = fopen(cacheFile, "rb"))
    {
        fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> bytes(n > 0 ? (size_t)n : 0);
        const bool ok = n > 0 && fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
        fclose(f);
        if (ok && CreateShaderObject(e, isPixel, bytes.data(), bytes.size())) { g_shaderOk++; g_shaderCacheHits++; return &e; }
    }
    CreateDirectoryA("shader_cache", nullptr);
    auto job = std::make_shared<CompileJob>();
    job->hlsl = std::move(hlsl); job->target = target; job->tag = tag; job->cachePath = cacheFile;
    e.job = job;
    QueueCompile(job);
    if (g_log) Log("[gpu] compiling %s (%zu dwords, %zu textures, %zu vfetch) in the background", tag, sc.code.size(), e.info.textureSlots.size(), e.info.vfetchSlots.size());
    if (syncShaders) return GetShader(sc, isPixel);
    g_shaderPending++;
    return nullptr;
}

'''
s = s[:old_start] + new_get + s[old_end:]
open(p, 'w', encoding='utf-8').write(s)
print('ok')
