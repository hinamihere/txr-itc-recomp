import re
p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what, count=1):
    global s
    n = s.count(old)
    if n != count:
        raise SystemExit('count %d != %d: %s' % (n, count, what))
    s = s.replace(old, new)


# ---- 1. register writes keep dirty bits / fetch-entry versions ------------------------------------------------------------------------------
rep('''void XGpuWriteRegister(uint32_t index, uint32_t value) { if (index < 0x6000) g_xregs[index] = value; }''',
    '''// Register ranges written since the constant buffers were last filled (XGpuDraw clears it) and a version per texture-fetch entry (6 dwords each; vertex fetch
// slot s lives in entry s / 3): the big constant buffers are only rebuilt when something they depend on changed.
enum : uint32_t { kDirtyVsFloat = 1, kDirtyPsFloat = 2, kDirtyBoolLoop = 4, kDirtyMisc = 16 };
uint32_t g_regDirty = 0xFFu;
uint32_t g_fetchVer[32] = {};
void XGpuWriteRegister(uint32_t index, uint32_t value)
{
    if (index >= 0x6000 || g_xregs[index] == value) return;   // writing the value a register already holds changes nothing
    g_xregs[index] = value;
    if (index >= 0x4000)
    {
        if (index < 0x4400) g_regDirty |= kDirtyVsFloat;
        else if (index < 0x4800) g_regDirty |= kDirtyPsFloat;
        else if (index < 0x48C0) g_fetchVer[(index - 0x4800) / 6]++;
        else if (index >= 0x4900 && index < 0x4928) g_regDirty |= kDirtyBoolLoop;
    }
    else if (index == xr::PA_CL_VTE_CNTL || index == xr::PA_SU_SC_MODE_CNTL || index == xr::PA_SC_WINDOW_OFFSET || index == xr::RB_COLORCONTROL || (index >= xr::RB_ALPHA_REF && index <= xr::PA_CL_VPORT_ZOFFSET))
        g_regDirty |= kDirtyMisc;
}''', 'write register')

# ---- 2. per-draw constant buffer (b1) --------------------------------------------------------------------------------------------------------
rep('''#include <d3d11.h>
#include <dxgi.h>''', '''#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>''', 'include')
rep('''CP<ID3D11Buffer> g_cbVS, g_cbPS;''', '''CP<ID3D11Buffer> g_cbVS, g_cbPS;
// Per-draw constants (b1: the first index of the draw in the index buffer): a 4 MB dynamic ring addressed with Direct3D 11.1 constant-buffer offsets
// (one NO_OVERWRITE map per draw, DISCARD at the wrap), or without that support a 16-byte buffer rewritten with DISCARD for every draw.
CP<ID3D11Buffer> g_cbDraw;
CP<ID3D11DeviceContext1> g_ctx1;
bool g_cbOffsets = false;
uint32_t g_cbDrawPos = 0;
const uint32_t kCbDrawEntries = 16384;
uint64_t g_cbUploadsV = 0, g_cbUploadsP = 0;''', 'decls')
rep('''    cb.ByteWidth = 32;
    g_dev->CreateBuffer(&cb, nullptr, g_copyCB.put());''', '''    cb.ByteWidth = 32;
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
    }''', 'init')
rep('''bool BuildIndices(const XDrawParams& dp, std::vector<uint32_t>& out, D3D_PRIMITIVE_TOPOLOGY& topo, bool& rectList, uint32_t& minIdx, uint32_t& maxIdx)
{''', '''void BindDrawConstants(uint32_t startVertex)
{
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Buffer* b = g_cbDraw.get();
    if (g_cbOffsets)
    {
        if (FAILED(g_ctx->Map(b, 0, g_cbDrawPos == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m))) return;
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

bool BuildIndices(const XDrawParams& dp, std::vector<uint32_t>& out, D3D_PRIMITIVE_TOPOLOGY& topo, bool& rectList, uint32_t& minIdx, uint32_t& maxIdx)
{''', 'bind draw consts')

# ---- 3. the constants block of XGpuDraw ------------------------------------------------------------------------------------------------------
rep('''    // Constants
    static XeCB cbv, cbp;
    FillConstants(false, vs->info, cbv, rtW, rtH);
    cbv.sys1[3] = (float)startVertex;
    FillConstants(true, ps->info, cbp, rtW, rtH);
''', '''    // Constants: the two big buffers are rebuilt and uploaded only when a register they depend on changed since the last fill (XGpuWriteRegister keeps g_regDirty and
    // the fetch-entry versions), the vertex shader or the render target size changed, or a texture's component count changed (PatchSwizzles). The index offset of
    // the draw travels in the small b1 buffer.
    static XeCB cbv, cbp;
    static bool haveV = false, haveP = false; static uint64_t lastVsHash = 0; static float lastRtW = 0, lastRtH = 0;
    static uint32_t lastCompsV[32], lastCompsP[32], lastFetchV[32], lastFetchP[32];
    static const bool dbgRainbow = getenv("ITC_DEBUG_PS_RAINBOW") != nullptr;
    auto fetchChanged = [](const uint32_t* last, const ShaderEntry* se, bool withVfetch)
    {
        for (uint32_t slot : se->info.textureSlots) if (last[slot & 31] != g_fetchVer[slot & 31]) return true;
        if (withVfetch) for (uint32_t slot : se->info.vfetchSlots) if (last[(slot / 3) & 31] != g_fetchVer[(slot / 3) & 31]) return true;
        return false;
    };
    const bool refillV = g_noCbDedupe || !haveV || (g_regDirty & (kDirtyVsFloat | kDirtyBoolLoop | kDirtyMisc)) || lastVsHash != g_curVS.hash || lastRtW != rtW || lastRtH != rtH
                         || memcmp(lastCompsV, g_slotComps, sizeof lastCompsV) != 0 || fetchChanged(lastFetchV, vs, true);
    const bool refillP = g_noCbDedupe || !haveP || dbgRainbow || (g_regDirty & (kDirtyPsFloat | kDirtyBoolLoop | kDirtyMisc)) || memcmp(lastCompsP, g_slotComps, sizeof lastCompsP) != 0 || fetchChanged(lastFetchP, ps, false);
    if (refillV) FillConstants(false, vs->info, cbv, rtW, rtH);
    if (refillP) FillConstants(true, ps->info, cbp, rtW, rtH);
''', 'fill')
rep('''        static const bool dbgRainbow = getenv("ITC_DEBUG_PS_RAINBOW") != nullptr;
        if (dbgRainbow && mode == 4 && !rectList''', '''        if (dbgRainbow && mode == 4 && !rectList''', 'rainbow')
rep('''    PatchSwizzles(cbv); PatchSwizzles(cbp);
    {
        // The buffers are only rewritten when the contents differ from what they hold (pixel constants repeat a lot between draws).
        static XeCB lastV, lastP; static bool haveV = false, haveP = false;
        if (g_noCbDedupe || !haveV || memcmp(&cbv, &lastV, sizeof cbv) != 0) { UploadCB(g_cbVS.get(), cbv); memcpy(&lastV, &cbv, sizeof cbv); haveV = true; }
        if (g_noCbDedupe || !haveP || memcmp(&cbp, &lastP, sizeof cbp) != 0) { UploadCB(g_cbPS.get(), cbp); memcpy(&lastP, &cbp, sizeof cbp); haveP = true; }
        if (g_noFilter || g_bound.vsCb != g_cbVS.get()) { ID3D11Buffer* b = g_cbVS.get(); g_ctx->VSSetConstantBuffers(0, 1, &b); g_bound.vsCb = b; }
        if (g_noFilter || g_bound.psCb != g_cbPS.get()) { ID3D11Buffer* b = g_cbPS.get(); g_ctx->PSSetConstantBuffers(0, 1, &b); g_bound.psCb = b; }
    }
''', '''    if (refillV) { PatchSwizzles(cbv); UploadCB(g_cbVS.get(), cbv); haveV = true; lastVsHash = g_curVS.hash; lastRtW = rtW; lastRtH = rtH; memcpy(lastCompsV, g_slotComps, sizeof lastCompsV); memcpy(lastFetchV, g_fetchVer, sizeof lastFetchV); g_cbUploadsV++; }
    if (refillP) { PatchSwizzles(cbp); UploadCB(g_cbPS.get(), cbp); haveP = true; memcpy(lastCompsP, g_slotComps, sizeof lastCompsP); memcpy(lastFetchP, g_fetchVer, sizeof lastFetchP); g_cbUploadsP++; }
    g_regDirty = 0;
    if (g_noFilter || g_bound.vsCb != g_cbVS.get()) { ID3D11Buffer* b = g_cbVS.get(); g_ctx->VSSetConstantBuffers(0, 1, &b); g_bound.vsCb = b; }
    if (g_noFilter || g_bound.psCb != g_cbPS.get()) { ID3D11Buffer* b = g_cbPS.get(); g_ctx->PSSetConstantBuffers(0, 1, &b); g_bound.psCb = b; }
    BindDrawConstants(startVertex);
''', 'upload')

# ---- 4. stats ------------------------------------------------------------------------------------------------------------------------------
rep('''        Log("[stat]   index cache: hits +%llu misses +%llu resets +%llu (list=%zu), motion-vector pass draws skipped +%llu%s", (unsigned long long)(g_idxHits - lh), (unsigned long long)(g_idxMisses - lm), (unsigned long long)(g_idxResets - lr),
            g_idxCache.size(), (unsigned long long)(g_velocitySkipped - lv), g_velocityNeeded ? " (pass active)" : "");
        lh = g_idxHits; lm = g_idxMisses; lr = g_idxResets; lv = g_velocitySkipped;''',
    '''        static uint64_t lcv = 0, lcp = 0;
        Log("[stat]   index cache: hits +%llu misses +%llu resets +%llu (list=%zu), motion-vector pass draws skipped +%llu%s, constant buffer uploads vs +%llu ps +%llu", (unsigned long long)(g_idxHits - lh), (unsigned long long)(g_idxMisses - lm), (unsigned long long)(g_idxResets - lr),
            g_idxCache.size(), (unsigned long long)(g_velocitySkipped - lv), g_velocityNeeded ? " (pass active)" : "", (unsigned long long)(g_cbUploadsV - lcv), (unsigned long long)(g_cbUploadsP - lcp));
        lh = g_idxHits; lm = g_idxMisses; lr = g_idxResets; lv = g_velocitySkipped; lcv = g_cbUploadsV; lcp = g_cbUploadsP;''', 'stats')
open(p, 'w', encoding='utf-8').write(s)

# ---- 5. the vertex shader reads the index offset from b1 -------------------------------------------------------------------------------------
p2 = 'C:/itc-work/runtime/src/xgpu_shader.cpp'
t = open(p2, encoding='utf-8').read()
old = '''    s += "\\tfloat4 sys2;   // pixel: x = alpha func, y = alpha ref\\n\\tuint4 vfmeta[96];  // per vertex fetch slot: x = byte base address, y = size in dwords, z = endian\\n";
    s += "};\\n";
'''
new = '''    s += "\\tfloat4 sys2;   // pixel: x = alpha func, y = alpha ref\\n\\tuint4 vfmeta[96];  // per vertex fetch slot: x = byte base address, y = size in dwords, z = endian\\n";
    s += "};\\n";
    if (!params.isPixel) s += "cbuffer XeDraw : register(b1) { uint4 xe_draw; };   // x = first index of this draw in xe_indices\\n";
'''
if t.count(old) != 1:
    raise SystemExit('shader prelude anchor')
t = t.replace(old, new)
old2 = '''"\\tuint xe_vidx = xe_indices[vid + (uint)sys1.w];\\n'''
new2 = '''"\\tuint xe_vidx = xe_indices[vid + xe_draw.x];\\n'''
if t.count(old2) != 1:
    raise SystemExit('shader vidx anchor')
t = t.replace(old2, new2)
open(p2, 'w', encoding='utf-8').write(t)
print('patched part 6')
