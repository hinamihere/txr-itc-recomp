// API-independent parts of the Xenos GPU emulation, shared by the Direct3D 11 (xgpu_d3d.cpp) and Vulkan (xgpu_vk.cpp) backends: guest memory helpers,
// tiling, texture decoding into DXGI-format images (the Vulkan backend maps the DXGI formats), index-list conversion, constant-buffer filling, statistics
// counters and the PNG writer. Included inside the anonymous namespace of each backend, so every backend has its own copy of the state declared here.
const bool g_log = getenv("ITC_GPU_LOG") != nullptr;
const bool g_logVerbose = getenv("ITC_GPU_LOG") && atoi(getenv("ITC_GPU_LOG")) >= 2;   // per-draw register/constant dumps (very slow: the guest times out)
const bool g_dumpShaders = getenv("ITC_DUMP_SHADERS") != nullptr;
const bool g_noCull = getenv("ITC_NOCULL") != nullptr;
const uint32_t g_traceFrame = getenv("ITC_TRACE_FRAME") ? (uint32_t)atoi(getenv("ITC_TRACE_FRAME")) : 0xFFFFFFFF;
#define XTRACE(...) do { if (g_frame == g_traceFrame) Log(__VA_ARGS__); } while (0)
const int g_skipDrawsFrom = getenv("ITC_SKIP_DRAWS") ? atoi(getenv("ITC_SKIP_DRAWS")) : -1;   // experiment: from this frame on every draw is dropped (guest + command-processor limit)
const bool g_noPerf = getenv("ITC_NO_PERF") != nullptr;   // A/B switches for the draw-path optimisations (to bisect visual problems): ITC_NO_PERF=1 turns them all off
const bool g_noFilter = g_noPerf || getenv("ITC_NO_STATEFILTER") != nullptr;   // send every state call
const bool g_noIdxCache = g_noPerf || getenv("ITC_NO_IDXCACHE") != nullptr;    // convert and upload the index list of every draw
const bool g_noCbDedupe = g_noPerf || getenv("ITC_NO_CBDEDUP") != nullptr;     // rewrite the constant buffers for every draw
const bool g_noTexMemo = g_noPerf || getenv("ITC_NO_TEXMEMO") != nullptr;      // look every texture up again
const bool g_noVbRing = g_noPerf || getenv("ITC_NO_VBRING") != nullptr;        // a Direct3D buffer per guest vertex buffer, even for dynamic data
const bool g_noCbRing = g_noPerf || getenv("ITC_NO_CBRING") != nullptr;        // constant buffers: Map DISCARD per upload instead of the offset ring
// Guest memory is compared with the GPU copies (vertex buffers per 4 KB page, textures by hash) to notice CPU writes. Data that has been unchanged for kStableAfter
// consecutive comparisons is "static" and only re-compared every kRecheckPeriod frames (staggered over the pages / textures); anything that changes becomes "dynamic"
// again and is compared every frame. A mesh whose index list is new (index cache miss) is always compared at once. ITC_RECHECK_PERIOD=1 compares everything every frame.
const uint32_t kRecheckPeriod = []() { uint32_t v = getenv("ITC_RECHECK_PERIOD") ? (uint32_t)std::max(1, std::min(64, atoi(getenv("ITC_RECHECK_PERIOD")))) : (g_noPerf ? 1u : 8u); uint32_t pw = 1; while (pw * 2 <= v) pw *= 2; return pw; }();   // a power of two
const uint32_t kStableAfter = 3;
bool g_forceVbCheck = false;   // set for the draw whose index list was not cached
const uint32_t kRehashBudget = 12;   // periodic texture re-hashes per frame; new textures and textures whose memory was written by the GPU are always checked
const uint32_t g_dumpEvery = getenv("ITC_DUMP_EVERY") ? (uint32_t)atoi(getenv("ITC_DUMP_EVERY")) : 0;
const uint32_t g_dumpMax = getenv("ITC_DUMP_MAX") ? (uint32_t)atoi(getenv("ITC_DUMP_MAX")) : 20;

inline uint32_t PhysToVirt(uint32_t phys) { return 0xA0000000u + (phys & 0x1FFFFFFF); }
inline const uint8_t* PhysPtr(uint32_t phys) { return GP(PhysToVirt(phys)); }
inline uint8_t* PhysPtrW(uint32_t phys) { return GP(PhysToVirt(phys)); }

inline float F(uint32_t bitsv) { float f; memcpy(&f, &bitsv, 4); return f; }
inline uint64_t Hash64(const void* data, size_t n, uint64_t h = 0x9E3779B97F4A7C15ull)
{
    // Four independent CRC32 chains (SSE4.2) over 32-byte blocks: fast enough to validate megabytes of texture data every frame.
    const uint8_t* b = (const uint8_t*)data;
    uint64_t c0 = h, c1 = h ^ 0x1111111111111111ull, c2 = h ^ 0x2222222222222222ull, c3 = h ^ 0x3333333333333333ull;
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
    {
        uint64_t w0, w1, w2, w3;
        memcpy(&w0, b + i, 8); memcpy(&w1, b + i + 8, 8); memcpy(&w2, b + i + 16, 8); memcpy(&w3, b + i + 24, 8);
        c0 = _mm_crc32_u64(c0, w0); c1 = _mm_crc32_u64(c1, w1); c2 = _mm_crc32_u64(c2, w2); c3 = _mm_crc32_u64(c3, w3);
    }
    uint64_t r = c0 | (c1 << 32);
    r ^= (c2 | (c3 << 32)) * 0x9E3779B97F4A7C15ull;
    for (; i < n; i++) { r ^= b[i]; r *= 0x100000001b3ull; }
    return r ^ n;
}

// Number of bytes starting at p that are committed (accessible), up to `want`.
size_t CommittedBytes(const void* p, size_t want)
{
    const uint8_t* cur = (const uint8_t*)p;
    if (cur >= g_base && cur < g_base + 0x100000000ull) return GuestCommittedBytes((uint32_t)(cur - g_base), want);
    size_t got = 0;
    while (got < want)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(cur, &mbi, sizeof mbi)) break;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) break;
        const size_t regionLeft = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize - cur;
        got += regionLeft; cur += regionLeft;
    }
    return std::min(got, want);
}

inline uint32_t ApplyEndian(uint32_t v, uint32_t e)
{
    switch (e)
    {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    case 2: return __builtin_bswap32(v);
    case 3: return (v << 16) | (v >> 16);
    default: return v;
    }
}

// ---- Xenos tiling (32x32 macro tiles); returns a byte offset ------------------------------------------
inline uint32_t TiledOuter(uint32_t y, uint32_t width, uint32_t log2bpp)
{
    uint32_t macro = ((y >> 5) * (width >> 5)) << (log2bpp + 7);
    uint32_t micro = ((y & 6) << 2) << log2bpp;
    return macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2bpp)) + ((y & 1) << 4);
}
inline uint32_t TiledInner(uint32_t x, uint32_t y, uint32_t log2bpp, uint32_t baseOffset)
{
    uint32_t macro = (x >> 5) << (log2bpp + 7);
    uint32_t micro = (x & 7) << log2bpp;
    uint32_t offset = baseOffset + (macro + ((micro & ~0xFu) << 1) + (micro & 0xF));
    return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}
inline uint32_t TiledByteOffset(uint32_t x, uint32_t y, uint32_t alignedWidth, uint32_t log2bpp)
{
    return TiledInner(x, y, log2bpp, TiledOuter(y, alignedWidth, log2bpp));
}

const uint32_t g_backW = 1280, g_backH = 720;
uint32_t g_frame = 0, g_dumped = 0;
// Frame selected for detailed logging: ITC_LOG_FRAME=N, or (ITC_CAPTURE_3D=<seconds>) the frame after the first one with several large
// geometry draws once the process has run that long. Read by the draw/resolve logs here and the packet trace in gpu.cpp.
uint32_t g_captureFrame = getenv("ITC_LOG_FRAME") ? (uint32_t)atoi(getenv("ITC_LOG_FRAME")) : 0xFFFFFFFFu;
uint32_t g_bigDraws = 0;
const double g_capture3dAfter = getenv("ITC_CAPTURE_3D") ? atof(getenv("ITC_CAPTURE_3D")) : -1.0;

// Log of guest-memory ranges written by the GPU (flushes / CPU resolves): cached textures re-check their data only if overlapped.
struct MemWrite { uint64_t seq; uint32_t base; uint32_t bytes; };
std::vector<MemWrite> g_memWrites;
uint64_t g_memSeq = 1;
void NoteMemWrite(uint32_t base, size_t bytes)
{
    g_memWrites.push_back({ ++g_memSeq, base & 0x1FFFFFFF, (uint32_t)std::min<size_t>(bytes, 0x1FFFFFFF) });
    if (g_memWrites.size() > 1024) g_memWrites.erase(g_memWrites.begin(), g_memWrites.begin() + 512);
}
bool WrittenSince(uint64_t seq, uint32_t base, size_t bytes)
{
    if (seq >= g_memSeq) return false;
    if (g_memWrites.empty() || g_memWrites.front().seq > seq + 1) return true;   // history truncated: assume overlap
    base &= 0x1FFFFFFF;
    for (size_t i = g_memWrites.size(); i-- > 0;)
    {
        const MemWrite& w = g_memWrites[i];
        if (w.seq <= seq) break;
        if (base < (uint64_t)w.base + w.bytes && w.base < (uint64_t)base + bytes) return true;
    }
    return false;
}
struct ProfScope
{
    uint64_t& acc; LARGE_INTEGER t0;
    ProfScope(uint64_t& a) : acc(a) { QueryPerformanceCounter(&t0); }
    ~ProfScope() { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); acc += t1.QuadPart - t0.QuadPart; }
};
uint64_t g_dp[12] = {};
inline uint64_t QpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }

uint64_t g_flushCount = 0, g_flushBytes = 0, g_profFlush = 0;
uint64_t g_decodeCount = 0, g_decodeBytes = 0, g_profCommit = 0, g_profHashIn = 0, g_profCopyIn = 0, g_profUpload = 0;
uint64_t g_profMap = 0, g_profDecode = 0, g_profResolve = 0, g_profSwap = 0, g_profDraw = 0, g_profHash = 0, g_profShader = 0, g_profPresent = 0;
uint64_t g_drawCount = 0, g_resolveCount = 0, g_shaderFail = 0, g_shaderOk = 0, g_skippedDraws = 0, g_shaderPending = 0, g_shaderCacheHits = 0, g_shaderUpgrades = 0;
// Event counters for the hitch diagnostics (see XGpuSwap: a frame slower than ITC_SLOW_MS, default 45 ms, is logged with the deltas of these).
struct FrameEvents { uint64_t texHashed, texCreated, texUpdated, texBytes, vbPagesCmp, vbPagesChanged, vbBytesUp, vbCreated, rtCreated, cpuResolves, flushes, shaderCompiles, vbRingCopies, vbRingBytes, texMemoMiss; };
FrameEvents g_ev = {};

struct ShaderCode { std::vector<uint32_t> code; uint64_t hash = 0; bool valid = false; };
ShaderCode g_curVS, g_curPS;

// Interpolator / param-gen parameters come from SQ_PROGRAM_CNTL at draw time.
void GetPsParams(uint32_t& interpCount, int& paramGenReg)
{
    const uint32_t pc = XReg(xr::SQ_PROGRAM_CNTL);
    interpCount = ((pc >> 20) & 0xF) + 1;
    paramGenReg = -1;
    if ((pc >> 18) & 1) paramGenReg = (XReg(xr::SQ_CONTEXT_MISC) >> 8) & 0xFF;
}

// ---- Constant buffers ------------------------------------------------------------------------------------
struct XeCB
{
    float c[256][4];
    uint32_t bconst[8];
    uint32_t lconst[32];
    uint32_t texswz[32];
    float sys0[4], sys1[4], sys2[4];
    uint32_t vfmeta[96][4];
};
static_assert(sizeof(XeCB) == 5968, "XeConstants layout");

// ---- Fetch constants -------------------------------------------------------------------------------------
struct TexFetch
{
    uint32_t d[6];
    uint32_t type() const { return d[0] & 3; }
    uint32_t format() const { return d[1] & 0x3F; }
    uint32_t endian() const { return (d[1] >> 6) & 3; }
    uint32_t base() const { return (d[1] >> 12) << 12; }
    bool tiled() const { return d[0] >> 31; }
    uint32_t pitchTexels() const { return ((d[0] >> 22) & 0x1FF) << 5; }
    uint32_t dimension() const { return (d[5] >> 9) & 3; }
    uint32_t swizzle() const { return (d[3] >> 1) & 0xFFF; }
};
TexFetch GetTexFetch(uint32_t slot)
{
    TexFetch t;
    for (int i = 0; i < 6; i++) t.d[i] = XReg(xr::SHADER_CONSTANT_FETCH_00_0 + slot * 6 + i);
    return t;
}

// ---- Texture decoding ------------------------------------------------------------------------------------
struct FormatInfo { uint32_t bytesPerBlock; uint32_t blockDim; uint32_t comps; DXGI_FORMAT out; uint32_t outBytes; };
enum Kind { K_UNKNOWN, K_8, K_1555, K_565, K_4444, K_88, K_8888, K_2101010, K_1011_11, K_11_11_10, K_24_8, K_16, K_16F, K_32F, K_BC1, K_BC2, K_BC3, K_BC5 };

struct FmtDesc { Kind kind; uint32_t bpb; uint32_t bdim; uint32_t comps; };
FmtDesc DescribeFormat(uint32_t f)
{
    switch (f)
    {
    case 2: case 8: case 9: return { K_8, 1, 1, 1 };
    case 3: return { K_1555, 2, 1, 4 };
    case 4: return { K_565, 2, 1, 3 };
    case 15: return { K_4444, 2, 1, 4 };
    case 10: return { K_88, 2, 1, 2 };
    case 6: case 14: case 50: return { K_8888, 4, 1, 4 };
    case 7: case 54: return { K_2101010, 4, 1, 4 };
    case 16: case 55: return { K_1011_11, 4, 1, 3 };
    case 17: case 56: return { K_11_11_10, 4, 1, 3 };
    case 22: case 23: return { K_24_8, 4, 1, 1 };
    case 24: case 27: return { K_16, 2, 1, 1 };
    case 25: case 28: return { K_16, 4, 1, 2 };
    case 26: case 29: return { K_16, 8, 1, 4 };
    case 30: return { K_16F, 2, 1, 1 };
    case 31: return { K_16F, 4, 1, 2 };
    case 32: return { K_16F, 8, 1, 4 };
    case 33: case 36: return { K_32F, 4, 1, 1 };
    case 34: case 37: return { K_32F, 8, 1, 2 };
    case 35: case 38: return { K_32F, 16, 1, 4 };
    case 18: return { K_BC1, 8, 4, 4 };
    case 19: return { K_BC2, 16, 4, 4 };
    case 20: return { K_BC3, 16, 4, 4 };
    case 49: return { K_BC5, 16, 4, 2 };
    default: return { K_UNKNOWN, 4, 1, 4 };
    }
}

// Exact number of guest bytes the base level of a texture occupies (as read by DecodeTexture).
size_t TextureFootprint(const TexFetch& tf)
{
    const uint32_t dim = tf.dimension();
    uint32_t w, h;
    if (dim == 0) { w = (tf.d[2] & 0xFFFFFF) + 1; h = 1; }
    else { w = (tf.d[2] & 0x1FFF) + 1; h = ((tf.d[2] >> 13) & 0x1FFF) + 1; }
    if (w > 8192) w = 8192;
    const FmtDesc fd = DescribeFormat(tf.format());
    const uint32_t bw = fd.bdim;
    const uint32_t wBlocks = (w + bw - 1) / bw, hBlocks = (h + bw - 1) / bw;
    uint32_t pitchBlocks = tf.pitchTexels() ? tf.pitchTexels() / bw : wBlocks;
    if (pitchBlocks < wBlocks) pitchBlocks = wBlocks;
    const uint32_t alignedPitch = (pitchBlocks + 31) & ~31u;
    return tf.tiled() ? (size_t)alignedPitch * ((hBlocks + 31) & ~31u) * fd.bpb : (size_t)pitchBlocks * hBlocks * fd.bpb;
}

inline uint16_t F32toF16(float f)
{
    uint32_t x; memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000; int32_t e = (int32_t)((x >> 23) & 0xFF) - 127 + 15; uint32_t m = x & 0x7FFFFF;
    if (e <= 0) return (uint16_t)sign;
    if (e >= 31) return (uint16_t)(sign | 0x7C00);
    return (uint16_t)(sign | (e << 10) | (m >> 13));
}

// Guest layout of the mip levels of a tiled 2D texture (rules from Xenia's GetGuestTextureLayout): level 0 sits at the base address with the pitch of the fetch
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

uint32_t g_slotComps[32];

// The game renders a 512x288 motion-vector pass (~420 draws, pixel shader 1ba23ef83ff60169, colour format 4 = 16_16) every frame, but nothing seen so far ever samples
// its result. Its draws are skipped until a texture fetch (or a write-back to guest memory) actually uses a resolve of that pass: then it is switched on for good.
// ITC_KEEP_VELOCITY=1 renders it always.
constexpr uint64_t kVelocityPsHash = 0x1ba23ef83ff60169ull;
bool g_velocityNeeded = getenv("ITC_KEEP_VELOCITY") != nullptr;
uint64_t g_velocitySkipped = 0;
// The 256x256 environment-map pass (2x MSAA, ~465 draws: a third of a race frame) is re-rendered every kEnvEvery-th frame only; in between its draws, clears and resolves are
// dropped, so the GPU textures resolved by the last real pass keep serving the reflections. Only race-sized frames are throttled. ITC_ENV_EVERY=1 renders it every frame.
const uint32_t g_envEvery = getenv("ITC_ENV_EVERY") ? (uint32_t)std::max(1, atoi(getenv("ITC_ENV_EVERY"))) : (g_noPerf ? 1u : 2u);
bool g_envRenderedOnce = false;
uint32_t g_envFrameDecided = 0xFFFFFFFFu; bool g_envFrameSkip = false;
uint64_t g_envDrawsSkipped = 0, g_envResolvesSkipped = 0;
uint64_t g_frameDrawsStart = 0, g_lastFrameDraws = 0;   // draws executed per frame (g_drawCount at the start of the frame / total of the previous one)
bool IsEnvPassState()
{
    return XReg(xr::PA_SC_WINDOW_SCISSOR_BR) == 0x01000100u && ((XReg(xr::RB_SURFACE_INFO) >> 16) & 3) == 2;
}
// True when the environment pass of this frame is dropped (decided once per frame, at its first draw / resolve).
bool EnvPassSkipped()
{
    if (g_envEvery <= 1) return false;
    if (g_envFrameDecided != g_frame)
    {
        g_envFrameDecided = g_frame;
        g_envFrameSkip = g_envRenderedOnce && g_lastFrameDraws > 1200 && (g_frame % g_envEvery) != 0;
    }
    return g_envFrameSkip;
}
uint32_t g_velSkippedThisPass = 0;   // skipped motion-vector draws since the last resolve: a resolve that follows them is the (empty) result of that pass
void NoteVelocityUse(const char* why)
{
    if (g_velocityNeeded) return;
    g_velocityNeeded = true;
    Log("[gpu] the motion-vector pass result is used (%s, frame %u, draw #%llu, ps %016llx): rendering that pass from now on", why, g_frame, (unsigned long long)g_drawCount, (unsigned long long)g_curPS.hash);
}

DXGI_FORMAT ResolvedFormat(uint32_t destFmt, uint32_t& bpp, uint32_t& comps)
{
    comps = 4;
    switch (destFmt)
    {
    case 6: case 14: bpp = 4; return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 7: bpp = 4; return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 2: case 8: case 9: bpp = 1; comps = 1; return DXGI_FORMAT_R8_UNORM;
    case 10: bpp = 2; comps = 2; return DXGI_FORMAT_R8G8_UNORM;
    case 24: bpp = 2; comps = 1; return DXGI_FORMAT_R16_UNORM;
    case 25: bpp = 4; comps = 2; return DXGI_FORMAT_R16G16_UNORM;
    case 26: bpp = 8; return DXGI_FORMAT_R16G16B16A16_UNORM;
    case 30: bpp = 2; comps = 1; return DXGI_FORMAT_R16_FLOAT;
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

// Reads one element (1..16 bytes) at byte offset `off` of `src`, applying the fetch-constant endian mode; result as numeric dwords.
inline void LoadElement(const uint8_t* src, size_t avail, uint32_t off, uint32_t bpb, uint32_t endian, uint32_t out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    if (off + bpb > avail) return;
    if (bpb == 1)
    {
        uint32_t raw; memcpy(&raw, src + (off & ~3u), 4);
        out[0] = (ApplyEndian(raw, endian) >> ((off & 3) * 8)) & 0xFF;
    }
    else if (bpb == 2)
    {
        uint32_t raw; memcpy(&raw, src + (off & ~3u), 4);
        out[0] = (ApplyEndian(raw, endian) >> ((off & 2) * 8)) & 0xFFFF;
    }
    else
    {
        for (uint32_t i = 0; i < bpb / 4; i++) { uint32_t raw; memcpy(&raw, src + off + i * 4, 4); out[i] = ApplyEndian(raw, endian); }
    }
}

// Decodes the base level of a 2D (or 1D) texture into a D3D-uploadable image.
struct Decoded { std::vector<uint8_t> data; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; uint32_t w = 0, h = 0, rowPitch = 0, comps = 4; uint64_t srcHash = 0; };

bool DecodeTexture(const TexFetch& tf, Decoded& out, bool hashOnly, uint64_t* hashOut)
{
    const uint32_t dim = tf.dimension();
    uint32_t w, h;
    if (dim == 0) { w = (tf.d[2] & 0xFFFFFF) + 1; h = 1; }
    else { w = (tf.d[2] & 0x1FFF) + 1; h = ((tf.d[2] >> 13) & 0x1FFF) + 1; }
    if (w > 8192) w = 8192;
    const FmtDesc fd = DescribeFormat(tf.format());
    const uint32_t bw = fd.bdim;
    const uint32_t wBlocks = (w + bw - 1) / bw, hBlocks = (h + bw - 1) / bw;
    uint32_t pitchBlocks = tf.pitchTexels() ? tf.pitchTexels() / bw : wBlocks;
    if (pitchBlocks < wBlocks) pitchBlocks = wBlocks;
    const uint32_t alignedPitch = (pitchBlocks + 31) & ~31u;
    uint32_t log2bpb = 0; while ((1u << log2bpb) < fd.bpb) log2bpb++;
    const uint8_t* src = PhysPtr(tf.base());
    const size_t fullBytes = tf.tiled() ? (size_t)alignedPitch * ((hBlocks + 31) & ~31u) * fd.bpb : (size_t)pitchBlocks * hBlocks * fd.bpb;
    size_t avail;
    { ProfScope pc(g_profCommit); avail = CommittedBytes(src, fullBytes); }
    if (hashOut) { ProfScope ph(g_profHashIn); *hashOut = Hash64(src, avail); }
    if (hashOnly) return true;

    const uint32_t endian = tf.endian();
    // Output format selection: single/dual component formats stay native (the shader swizzle is patched to replicate components).
    uint32_t outBpp; DXGI_FORMAT ofmt; uint32_t comps = 4;
    switch (fd.kind)
    {
    case K_8: ofmt = DXGI_FORMAT_R8_UNORM; outBpp = 1; comps = 1; break;
    case K_88: ofmt = DXGI_FORMAT_R8G8_UNORM; outBpp = 2; comps = 2; break;
    case K_1555: case K_565: case K_4444: case K_1011_11: case K_11_11_10: ofmt = DXGI_FORMAT_R8G8B8A8_UNORM; outBpp = 4; break;
    case K_8888: ofmt = DXGI_FORMAT_R8G8B8A8_UNORM; outBpp = 4; break;
    case K_2101010: ofmt = DXGI_FORMAT_R10G10B10A2_UNORM; outBpp = 4; break;
    case K_24_8: ofmt = DXGI_FORMAT_R32_FLOAT; outBpp = 4; comps = 1; break;
    case K_16: comps = fd.comps == 3 ? 4 : fd.comps; ofmt = comps == 1 ? DXGI_FORMAT_R16_UNORM : comps == 2 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16G16B16A16_UNORM; outBpp = comps * 2; break;
    case K_16F: comps = fd.comps == 3 ? 4 : fd.comps; ofmt = comps == 1 ? DXGI_FORMAT_R16_FLOAT : comps == 2 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT; outBpp = comps * 2; break;
    case K_32F: comps = fd.comps == 3 ? 4 : fd.comps; ofmt = comps == 1 ? DXGI_FORMAT_R32_FLOAT : comps == 2 ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT; outBpp = comps * 4; break;
    case K_BC1: ofmt = DXGI_FORMAT_BC1_UNORM; outBpp = 8; break;
    case K_BC2: ofmt = DXGI_FORMAT_BC2_UNORM; outBpp = 16; break;
    case K_BC3: ofmt = DXGI_FORMAT_BC3_UNORM; outBpp = 16; break;
    case K_BC5: ofmt = DXGI_FORMAT_BC5_UNORM; outBpp = 16; comps = 2; break;
    default: ofmt = DXGI_FORMAT_R8G8B8A8_UNORM; outBpp = 4; break;
    }
    const bool bc = fd.bdim == 4;
    const uint32_t ow = bc ? ((w + 3) & ~3u) : w, oh = bc ? ((h + 3) & ~3u) : h;
    const uint32_t outBw = bc ? ow / 4 : ow, outBh = bc ? oh / 4 : oh;
    out.fmt = ofmt; out.w = ow; out.h = oh; out.comps = comps;
    out.rowPitch = outBw * outBpp;
    out.data.assign((size_t)out.rowPitch * outBh, 0);
    uint8_t* dst = out.data.data();

    const bool directKind = fd.kind == K_8 || fd.kind == K_88 || fd.kind == K_8888 || fd.kind == K_2101010 || fd.kind == K_16 || fd.kind == K_16F || fd.kind == K_32F ||
                            fd.kind == K_BC1 || fd.kind == K_BC2 || fd.kind == K_BC3 || fd.kind == K_BC5;
    if (!tf.tiled() && endian == 0 && directKind && outBpp == fd.bpb)
    {
        for (uint32_t by = 0; by < outBh; by++)
        {
            const size_t rowOff = (size_t)by * pitchBlocks * fd.bpb;
            if (rowOff >= avail) break;
            memcpy(dst + (size_t)by * out.rowPitch, src + rowOff, std::min<size_t>(out.rowPitch, avail - rowOff));
        }
        return true;
    }

    auto decodeRows = [&](uint32_t byStart, uint32_t byEnd)
    {
    for (uint32_t by = byStart; by < byEnd; by++)
    {
        for (uint32_t bx = 0; bx < outBw; bx++)
        {
            const uint32_t off = tf.tiled() ? TiledByteOffset(bx, by, alignedPitch, log2bpb) : (by * pitchBlocks + bx) * fd.bpb;
            uint32_t e[4];
            LoadElement(src, avail, off, fd.bpb, endian, e);
            uint8_t* px = dst + (size_t)by * out.rowPitch + (size_t)bx * outBpp;
            float v[4] = { 0, 0, 0, 0 };   // normalized components for the float paths
            uint32_t r8 = 0, g8 = 0, b8 = 0, a8 = 255;
            switch (fd.kind)
            {
            case K_8: px[0] = (uint8_t)e[0]; continue;
            case K_88: memcpy(px, &e[0], 2); continue;
            case K_8888: memcpy(px, &e[0], 4); continue;
            case K_2101010: memcpy(px, &e[0], 4); continue;
            case K_565: r8 = ((e[0] >> 11) & 31) * 255 / 31; g8 = ((e[0] >> 5) & 63) * 255 / 63; b8 = (e[0] & 31) * 255 / 31; break;
            case K_1555: a8 = (e[0] >> 15) ? 255 : 0; r8 = ((e[0] >> 10) & 31) * 255 / 31; g8 = ((e[0] >> 5) & 31) * 255 / 31; b8 = (e[0] & 31) * 255 / 31; break;
            case K_4444: a8 = ((e[0] >> 12) & 15) * 17; r8 = ((e[0] >> 8) & 15) * 17; g8 = ((e[0] >> 4) & 15) * 17; b8 = (e[0] & 15) * 17; break;
            case K_1011_11: r8 = (e[0] & 1023) * 255 / 1023; g8 = ((e[0] >> 10) & 2047) * 255 / 2047; b8 = ((e[0] >> 21) & 2047) * 255 / 2047; break;
            case K_11_11_10: r8 = (e[0] & 2047) * 255 / 2047; g8 = ((e[0] >> 11) & 2047) * 255 / 2047; b8 = ((e[0] >> 22) & 1023) * 255 / 1023; break;
            case K_24_8: { float d = (float)(e[0] >> 8) / 16777215.0f; memcpy(px, &d, 4); continue; }
            case K_16: case K_16F:
            {
                uint16_t c[4];
                const uint32_t n = fd.comps;
                for (uint32_t i = 0; i < n; i++) c[i] = (uint16_t)((e[i / 2] >> ((i & 1) * 16)) & 0xFFFF);
                for (uint32_t i = n; i < comps; i++) c[i] = c[n - 1];
                memcpy(px, c, comps * 2); continue;
            }
            case K_32F:
            {
                uint32_t c[4];
                const uint32_t n = fd.comps;
                for (uint32_t i = 0; i < n; i++) c[i] = e[i];
                for (uint32_t i = n; i < comps; i++) c[i] = c[n - 1];
                memcpy(px, c, comps * 4); continue;
            }
            case K_BC1: case K_BC2: case K_BC3: case K_BC5:
                memcpy(px, e, outBpp); continue;
            default: r8 = 255; g8 = 0; b8 = 255; break;
            }
            (void)v;
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
}

DXGI_FORMAT ColorRtFormat(uint32_t f)
{
    switch (f)
    {
    case 0: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 1: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case 2: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 3: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 4: return DXGI_FORMAT_R16G16_UNORM;
    case 5: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case 6: return DXGI_FORMAT_R16G16_FLOAT;
    case 7: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 10: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case 12: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 14: return DXGI_FORMAT_R32_FLOAT;
    case 15: return DXGI_FORMAT_R32G32_FLOAT;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

void FillConstants(bool isPixel, const XShaderInfo& info, XeCB& cb, float rtW, float rtH)
{
    memset(&cb, 0, sizeof cb);
    const uint32_t floatBase = xr::SHADER_CONSTANT_000_X + (isPixel ? 256 * 4 : 0);
    memcpy(cb.c, &g_xregs[floatBase], sizeof cb.c);
    // Both stages see the whole boolean (256 bits) and loop (32 dwords) constant files: shaders address them absolutely
    // (pixel shaders use bool 128..255 and loop 16..31).
    for (int i = 0; i < 8; i++) cb.bconst[i] = g_xregs[0x4900 + i];
    for (int i = 0; i < 32; i++) cb.lconst[i] = g_xregs[0x4908 + i];
    for (uint32_t s = 0; s < 32; s++)
    {
        const uint32_t d3 = g_xregs[xr::SHADER_CONSTANT_FETCH_00_3 + s * 6];
        cb.texswz[s] = ((d3 >> 1) & 0xFFF) | (((d3 >> 13) & 0x3F) << 12);   // swizzle | exponent adjust
    }
    if (!isPixel)
    {
        const uint32_t vte = XReg(xr::PA_CL_VTE_CNTL);
        const uint32_t modeCntl = XReg(xr::PA_SU_SC_MODE_CNTL);
        float xs = 1, xo = 0, ys = 1, yo = 0, zs = 1, zo = 0;
        if (vte & 1) xs = F(XReg(xr::PA_CL_VPORT_XSCALE));
        if (vte & 2) xo = F(XReg(xr::PA_CL_VPORT_XOFFSET));
        if (vte & 4) ys = F(XReg(xr::PA_CL_VPORT_YSCALE));
        if (vte & 8) yo = F(XReg(xr::PA_CL_VPORT_YOFFSET));
        if (vte & 16) zs = F(XReg(xr::PA_CL_VPORT_ZSCALE));
        if (vte & 32) zo = F(XReg(xr::PA_CL_VPORT_ZOFFSET));
        // The window offset only exists to shift each predicated-tiling pass into the tile held in EDRAM (e.g. y -256 for the second
        // 1280x256 tile). Render targets here are whole frames, so passes are drawn in full-frame coordinates (offset ignored here, in
        // the scissor and in the resolve rectangle); ITC_WINDOW_OFFSET=1 restores the literal behaviour for experiments.
        static const bool useWindowOffset = getenv("ITC_WINDOW_OFFSET") != nullptr;
        if (useWindowOffset && ((modeCntl >> 16) & 1))
        {
            const uint32_t wo = XReg(xr::PA_SC_WINDOW_OFFSET);
            xo += (float)((int32_t)(wo << 17) >> 17);
            yo += (float)((int32_t)((wo >> 16) << 17) >> 17);
        }
        cb.sys0[0] = 2.0f * xs / rtW; cb.sys0[1] = -2.0f * ys / rtH; cb.sys0[2] = 2.0f * xo / rtW - 1.0f; cb.sys0[3] = 1.0f - 2.0f * yo / rtH;
        cb.sys1[0] = zs; cb.sys1[1] = zo; cb.sys1[2] = ((vte >> 8) & 1) ? 1.0f : 0.0f;
        for (uint32_t slot : info.vfetchSlots)
        {
            const uint32_t reg = xr::SHADER_CONSTANT_FETCH_00_0 + (slot / 3) * 6 + (slot % 3) * 2;
            const uint32_t d1 = g_xregs[reg + 1];
            cb.vfmeta[slot][0] = 0;
            cb.vfmeta[slot][1] = (d1 >> 2) & 0xFFFFFF;
            cb.vfmeta[slot][2] = d1 & 3;
        }
    }
    else
    {
        const uint32_t cc = XReg(xr::RB_COLORCONTROL);
        static const bool noAlphaTest = getenv("ITC_NO_ALPHATEST") != nullptr;
        cb.sys2[0] = (((cc >> 3) & 1) && !noAlphaTest) ? (float)(cc & 7) : 8.0f;
        cb.sys2[1] = F(XReg(xr::RB_ALPHA_REF));
    }
}

void PatchSwizzles(XeCB& cb)
{
    for (uint32_t slot = 0; slot < 32; slot++)
    {
        const uint32_t comps = g_slotComps[slot];
        if (comps >= 3) continue;
        uint32_t sw = cb.texswz[slot], out = 0;
        for (uint32_t ch = 0; ch < 4; ch++)
        {
            uint32_t v = (sw >> (ch * 3)) & 7;
            if (comps == 1 && v >= 1 && v <= 3) v = 0;
            else if (comps == 2 && v >= 2 && v <= 3) v = 1;
            out |= v << (ch * 3);
        }
        cb.texswz[slot] = out | (sw & ~0xFFFu);
    }
}

bool BuildIndices(const XDrawParams& dp, std::vector<uint32_t>& out, D3D_PRIMITIVE_TOPOLOGY& topo, bool& rectList, uint32_t& minIdx, uint32_t& maxIdx)
{
    const uint32_t n = dp.indexCount;
    if (n == 0 || n > (1u << 21)) return false;
    std::vector<uint32_t> src(n);
    const uint32_t off = XReg(xr::VGT_INDX_OFFSET);
    // Primitive restart ("multi-prim IB"): an index equal to VGT_MULTI_PRIM_IB_RESET_INDX ends the current strip/fan. Direct3D 11 draws
    // here have no index buffer (the vertex shader pulls the index), so resets are removed on the CPU.
    const bool resetOn = ((XReg(xr::PA_SU_SC_MODE_CNTL) >> 21) & 1) != 0;
    const uint32_t resetIdx = XReg(xr::VGT_MULTI_PRIM_IB_RESET_INDX) & (dp.index32 ? 0xFFFFFFFFu : 0xFFFFu);
    constexpr uint32_t kReset = 0xFFFFFFFFu;
    bool anyReset = false;
    if (dp.sourceSelect == 0)
    {
        const uint8_t* mem = PhysPtr(dp.dmaPhysBase);
        const size_t need = dp.index32 ? (size_t)n * 4 : (size_t)((n + 1) / 2) * 4;
        if (CommittedBytes(mem, need) < need) return false;
        for (uint32_t i = 0; i < n; i++)
        {
            uint32_t v;
            if (dp.index32) { uint32_t raw; memcpy(&raw, mem + i * 4, 4); v = ApplyEndian(raw, dp.dmaEndian); }
            else { uint32_t raw; memcpy(&raw, mem + (i >> 1) * 4, 4); const uint32_t w = ApplyEndian(raw, dp.dmaEndian); v = (i & 1) ? (w >> 16) : (w & 0xFFFF); }
            if (resetOn && v == resetIdx) { src[i] = kReset; anyReset = true; }
            else src[i] = v + off;
        }
    }
    else for (uint32_t i = 0; i < n; i++) src[i] = i + off;
    minIdx = 0xFFFFFFFFu; maxIdx = 0;
    for (uint32_t v : src) { if (v == kReset) continue; if (v < minIdx) minIdx = v; if (v > maxIdx) maxIdx = v; }
    if (minIdx == 0xFFFFFFFFu) return false;

    rectList = false;
    out.clear();
    // Calls fn(first, count) for every run of indices between resets.
    auto forSegments = [&](auto&& fn)
    {
        size_t i = 0;
        while (i < n)
        {
            while (i < n && src[i] == kReset) i++;
            size_t j = i;
            while (j < n && src[j] != kReset) j++;
            if (j > i) fn(&src[i], j - i);
            i = j;
        }
    };
    auto stripToList = [&](const uint32_t* v, size_t c)
    {
        for (size_t i = 0; i + 2 < c; i++)
        {
            if (i & 1) { out.push_back(v[i + 1]); out.push_back(v[i]); out.push_back(v[i + 2]); }
            else { out.push_back(v[i]); out.push_back(v[i + 1]); out.push_back(v[i + 2]); }
        }
    };
    auto fanToList = [&](const uint32_t* v, size_t c) { for (size_t i = 1; i + 1 < c; i++) { out.push_back(v[0]); out.push_back(v[i]); out.push_back(v[i + 1]); } };
    if (anyReset && (dp.primType == 4 || dp.primType == 7 || dp.primType == 8 || dp.primType == 0x0D || dp.primType == 1 || dp.primType == 2))
    {
        src.erase(std::remove(src.begin(), src.end(), kReset), src.end());   // lists have no strips to cut
        anyReset = false;
    }
    const uint32_t m = (uint32_t)src.size();
    switch (dp.primType)
    {
    case 1: topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; out = src; break;
    case 2: topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST; out = src; break;
    case 3:
        if (!anyReset) { topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; out = src; }
        else { topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST; forSegments([&](const uint32_t* v, size_t c) { for (size_t i = 0; i + 1 < c; i++) { out.push_back(v[i]); out.push_back(v[i + 1]); } }); }
        break;
    case 0x0C: topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; out = src; out.push_back(src[0]); break;
    case 4: case 7: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; out = src; out.resize(m - m % 3); break;
    case 6:
        if (!anyReset) { topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; out = src; }
        else { topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; forSegments(stripToList); }
        break;
    case 5: case 0x0F:
        topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        if (!anyReset) fanToList(src.data(), src.size()); else forSegments(fanToList);
        break;
    case 8: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; out = src; out.resize(m - m % 3); rectList = true; break;
    case 0x0D:
        topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        for (uint32_t i = 0; i + 3 < m; i += 4) { out.push_back(src[i]); out.push_back(src[i + 1]); out.push_back(src[i + 2]); out.push_back(src[i]); out.push_back(src[i + 2]); out.push_back(src[i + 3]); }
        break;
    case 0x0E:
        topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        for (uint32_t i = 0; i + 3 < m; i += 2) { out.push_back(src[i]); out.push_back(src[i + 1]); out.push_back(src[i + 3]); out.push_back(src[i]); out.push_back(src[i + 3]); out.push_back(src[i + 2]); }
        break;
    default: return false;
    }
    return !out.empty();
}

struct Surface { uint32_t w, h, pitch; };

// Logical size of the render surface for this draw.
Surface CurrentSurface()
{
    const uint32_t pitch = XReg(xr::RB_SURFACE_INFO) & 0x3FFF;
    const uint32_t sbr = XReg(xr::PA_SC_SCREEN_SCISSOR_BR), wbr = XReg(xr::PA_SC_WINDOW_SCISSOR_BR);
    uint32_t w = pitch, h = 0;
    uint32_t bx = std::max<uint32_t>(sbr & 0x7FFF, wbr & 0x3FFF), by = std::max<uint32_t>((sbr >> 16) & 0x7FFF, (wbr >> 16) & 0x3FFF);
    if (bx && bx < w) w = bx;
    h = by;
    // The screen scissor is usually left at its "unlimited" value (8192); the window scissor then holds the real size of the pass
    // (a 1024x1024 shadow map must not be cut to the 720 rows of the frame buffer).
    if (h > 4096) h = std::max<uint32_t>(720, (wbr >> 16) & 0x3FFF);
    if (h == 0 || h > 4096) h = 720;
    if (w == 0) w = 1280;
    if (w > 4096) w = 4096;
    return { w, h, pitch };
}

struct F4 { float v[4]; };

inline F4 ReadPixel(const uint8_t* p, DXGI_FORMAT f)
{
    F4 o = { { 0, 0, 0, 1 } };
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: for (int i = 0; i < 4; i++) o.v[i] = p[i] / 255.0f; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: { uint32_t v; memcpy(&v, p, 4); o.v[0] = (v & 1023) / 1023.0f; o.v[1] = ((v >> 10) & 1023) / 1023.0f; o.v[2] = ((v >> 20) & 1023) / 1023.0f; o.v[3] = (v >> 30) / 3.0f; break; }
    case DXGI_FORMAT_R16G16B16A16_UNORM: { uint16_t v[4]; memcpy(v, p, 8); for (int i = 0; i < 4; i++) o.v[i] = v[i] / 65535.0f; break; }
    case DXGI_FORMAT_R16G16_UNORM: { uint16_t v[2]; memcpy(v, p, 4); o.v[0] = v[0] / 65535.0f; o.v[1] = v[1] / 65535.0f; break; }
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16_FLOAT:
    {
        const int n = f == DXGI_FORMAT_R16G16_FLOAT ? 2 : 4;
        static float* lut = nullptr;
        if (!lut)
        {
            lut = new float[65536];
            for (uint32_t h = 0; h < 65536; h++)
            {
                const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
                float v;
                if (e == 0) v = ldexpf((float)m, -24); else if (e == 31) v = m ? NAN : INFINITY; else v = ldexpf((float)(m | 1024), (int)e - 25);
                lut[h] = s ? -v : v;
            }
        }
        for (int i = 0; i < n; i++) { uint16_t h; memcpy(&h, p + i * 2, 2); o.v[i] = lut[h]; }
        break;
    }
    case DXGI_FORMAT_R32_FLOAT: memcpy(&o.v[0], p, 4); break;
    case DXGI_FORMAT_R32G32_FLOAT: memcpy(o.v, p, 8); break;
    default: break;
    }
    return o;
}

inline uint32_t Q8(float v) { return (uint32_t)(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f); }

// Packs one pixel into `numeric` dwords (up to 2) in the destination format. Returns bytes per pixel.
uint32_t PackPixel(const F4& c, uint32_t destFmt, bool swapRB, uint32_t out[2])
{
    out[0] = out[1] = 0;
    float r = swapRB ? c.v[2] : c.v[0], g = c.v[1], b = swapRB ? c.v[0] : c.v[2], a = c.v[3];
    switch (destFmt)
    {
    case 2: out[0] = Q8(r); return 1;
    case 7: { auto q = [](float v) { return (uint32_t)(std::min(std::max(v, 0.0f), 1.0f) * 1023.0f + 0.5f); }; out[0] = q(r) | (q(g) << 10) | (q(b) << 20) | ((uint32_t)(std::min(std::max(a, 0.0f), 1.0f) * 3.0f + 0.5f) << 30); return 4; }
    case 25: { auto q = [](float v) { return (uint32_t)(std::min(std::max(v, 0.0f), 1.0f) * 65535.0f + 0.5f); }; out[0] = q(r) | (q(g) << 16); return 4; }
    case 26: { auto q = [](float v) { return (uint32_t)(std::min(std::max(v, 0.0f), 1.0f) * 65535.0f + 0.5f); }; out[0] = q(r) | (q(g) << 16); out[1] = q(b) | (q(a) << 16); return 8; }
    case 31: out[0] = F32toF16(r) | ((uint32_t)F32toF16(g) << 16); return 4;
    case 32: out[0] = F32toF16(r) | ((uint32_t)F32toF16(g) << 16); out[1] = F32toF16(b) | ((uint32_t)F32toF16(a) << 16); return 8;
    case 36: memcpy(&out[0], &r, 4); return 4;
    case 37: memcpy(&out[0], &r, 4); memcpy(&out[1], &g, 4); return 8;
    default: out[0] = Q8(r) | (Q8(g) << 8) | (Q8(b) << 16) | (Q8(a) << 24); return 4;   // 8_8_8_8 and everything unsupported
    }
}

uint32_t Crc32(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu)
{
    static uint32_t table[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1; table[i] = v; } init = true; }
    for (size_t i = 0; i < n; i++) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}
void PutBE(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x); }
void PngChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
{
    PutBE(out, (uint32_t)data.size());
    std::vector<uint8_t> td(type, type + 4); td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    PutBE(out, ~Crc32(td.data(), td.size()));
}
void WritePng(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t pitch)
{
    std::vector<uint8_t> raw; raw.reserve((size_t)(w * 3 + 1) * h);
    for (uint32_t y = 0; y < h; y++) { raw.push_back(0); for (uint32_t x = 0; x < w; x++) { const uint8_t* p = rgba + (size_t)y * pitch + x * 4; raw.push_back(p[0]); raw.push_back(p[1]); raw.push_back(p[2]); } }
    std::vector<uint8_t> z = { 0x78, 0x01 };
    size_t pos = 0;
    while (pos < raw.size())
    {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n >= raw.size() ? 1 : 0); z.push_back(n & 0xFF); z.push_back(n >> 8); z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n); pos += n;
    }
    uint32_t a = 1, b = 0; for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    PutBE(z, (b << 16) | a);
    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    std::vector<uint8_t> ihdr; PutBE(ihdr, w); PutBE(ihdr, h); ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    PngChunk(png, "IHDR", ihdr); PngChunk(png, "IDAT", z); PngChunk(png, "IEND", {});
    if (FILE* f = fopen(path, "wb")) { fwrite(png.data(), 1, png.size(), f); fclose(f); }
}
