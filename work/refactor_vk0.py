"""Step 0 of the Vulkan renderer: split the API-independent parts of xgpu_d3d.cpp into xgpu_shared.inl (included by both backends) and the process-wide
state (register file, window, scripted-input thumbnails, backend dispatch) into xgpu_common.cpp. Code is moved verbatim; only the public entry points of
the Direct3D 11 backend are renamed (XGpuDraw -> D3dDraw, ...) so that xgpu_common.cpp can dispatch to the selected backend."""
import sys

SRC = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
lines = open(SRC, encoding='utf-8').read().split('\n')
L = lambda n: lines[n - 1]


def expect(n, prefix):
    if not L(n).startswith(prefix):
        sys.exit('line %d does not start with %r: %r' % (n, prefix, L(n)[:80]))


# (first, last, first-line prefix, last-line prefix)
SHARED = [
    (76, 98, 'const bool g_log', 'const uint32_t g_dumpMax'),
    (100, 168, 'inline uint32_t PhysToVirt', '}'),
    (184, 190, 'const uint32_t g_backW', 'const double g_capture3dAfter'),
    (191, 220, '// Log of guest-memory', 'inline uint64_t QpcNow'),
    (238, 244, 'uint64_t g_flushCount', 'FrameEvents g_ev'),
    (322, 323, 'struct ShaderCode', 'ShaderCode g_curVS'),
    (349, 356, '// Interpolator / param-gen', '}'),
    (553, 563, '// ---- Constant buffers', 'static_assert(sizeof(XeCB)'),
    (579, 736, '// ---- Fetch constants', '}'),
    (759, 759, 'uint32_t g_slotComps[32];', 'uint32_t g_slotComps[32];'),
    (779, 813, '// The game renders a 512x288', '}'),
    (816, 847, 'DXGI_FORMAT ResolvedFormat', '}'),
    (939, 1087, '// Reads one element', '}'),
    (1545, 1563, 'DXGI_FORMAT ColorRtFormat', '}'),
    (2255, 2326, 'void FillConstants', '}'),
    (2380, 2471, 'bool BuildIndices', '}'),
    (2559, 2577, 'struct Surface', '}'),
    (3116, 3171, 'struct F4', '}'),
    (3444, 3477, 'uint32_t Crc32', '}'),
]
COMMON = [
    (30, 49, 'uint32_t g_xregs[0x6000];', '}'),
    (51, 57, '// Screen-aware scripted input', 'bool XGpuGetThumb'),
    (246, 268, 'LRESULT CALLBACK WndProc', '}'),
]
REMOVE = [(172, 172, 'HANDLE g_winReady', 'HANDLE g_winReady')]
for a, b, pa, pb in SHARED + COMMON + REMOVE:
    expect(a, pa); expect(b, pb)

drop = set()
shared, common = [], []
for a, b, _, _ in SHARED:
    shared.append('\n'.join(lines[a - 1:b]))
    drop.update(range(a, b + 1))
for a, b, _, _ in COMMON:
    common.append('\n'.join(lines[a - 1:b]))
    drop.update(range(a, b + 1))
for a, b, _, _ in REMOVE:
    drop.update(range(a, b + 1))

out = []
for i, ln in enumerate(lines, 1):
    if i == 76:
        out.append('#include "xgpu_shared.inl"   // API-independent parts shared with the Vulkan backend (xgpu_vk.cpp)')
    if i in drop:
        continue
    out.append(ln)
d3d = '\n'.join(out)


def rep(old, new, count=1):
    global d3d
    n = d3d.count(old)
    if n != count:
        sys.exit('expected %d x %r, found %d' % (count, old, n))
    d3d = d3d.replace(old, new)


rep('''    g_winReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CreateThread(nullptr, 0, WindowThread, nullptr, 0, nullptr);
    WaitForSingleObject(g_winReady, 10000);
    if (!g_hwnd)''', '''    g_hwnd = (HWND)XGpuCreateWindow(g_backW, g_backH);
    if (!g_hwnd)''')
rep('bool XGpuWindowFocused() { return g_hwnd && GetForegroundWindow() == g_hwnd; }\n', '')
rep('uint32_t XGpuFrame() { return g_frame; }', 'uint32_t D3dFrame() { return g_frame; }')
rep('uint32_t XGpuCaptureFrame() { return g_captureFrame; }', 'uint32_t D3dCaptureFrame() { return g_captureFrame; }')
rep('void XGpuLoadShader(bool isPixel, const uint32_t* words, uint32_t dwordCount)\n{', 'void D3dLoadShader(bool isPixel, const uint32_t* words, uint32_t dwordCount)\n{')
rep('void XGpuDraw(const XDrawParams& dp)\n{', 'void D3dDraw(const XDrawParams& dp)\n{')
rep('    extern void XGpuDoResolve();\n    if (mode == 6) { XGpuDoResolve(); return; }', '    extern void D3dDoResolve();\n    if (mode == 6) { D3dDoResolve(); return; }')
rep('void XGpuDoResolve()\n{', 'void D3dDoResolve()\n{')
rep('void XGpuSwap(uint32_t frontbufferPhys, uint32_t width, uint32_t height)\n{', 'void D3dSwap(uint32_t frontbufferPhys, uint32_t width, uint32_t height)\n{')
rep('void XGpuIntervalStats(double seconds)\n{', 'void D3dIntervalStats(double seconds)\n{')
rep('void XGpuStats(char* out, size_t outSize)\n{', 'void D3dStats(char* out, size_t outSize)\n{')
rep('// Xenos GPU -> Direct3D 11 backend.\n', '// Xenos GPU -> Direct3D 11 backend (ITC_RENDERER=d3d11, the default; xgpu_common.cpp dispatches).\n')

inl = '''// API-independent parts of the Xenos GPU emulation, shared by the Direct3D 11 (xgpu_d3d.cpp) and Vulkan (xgpu_vk.cpp) backends: guest memory helpers,
// tiling, texture decoding into DXGI-format images (the Vulkan backend maps the DXGI formats), index-list conversion, constant-buffer filling, statistics
// counters and the PNG writer. Included inside the anonymous namespace of each backend, so every backend has its own copy of the state declared here.
''' + '\n\n'.join(shared) + '\n'
open('C:/itc-work/runtime/src/xgpu_shared.inl', 'w', encoding='utf-8').write(inl)

commonSrc = '''// Process-wide part of the Xenos GPU emulation: the register file, the game window, the scripted-input thumbnails, and the dispatch of the GPU entry points
// to the selected renderer backend (ITC_RENDERER=vulkan -> xgpu_vk.cpp, otherwise Direct3D 11 -> xgpu_d3d.cpp).
#include "xgpu.h"
#include "xenos_regs.h"
#include "guest.h"
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

''' + common[0] + '\n' + common[1] + '''

namespace {
HWND g_hwnd = nullptr;
HANDLE g_winReady = nullptr;
uint32_t g_winW = 1280, g_winH = 720;
''' + common[2].replace('RECT r = { 0, 0, (LONG)g_backW, (LONG)g_backH };', 'RECT r = { 0, 0, (LONG)g_winW, (LONG)g_winH };') + '''
}  // namespace

void* XGpuCreateWindow(uint32_t width, uint32_t height)
{
    if (g_hwnd) return g_hwnd;
    g_winW = width; g_winH = height;
    g_winReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CreateThread(nullptr, 0, WindowThread, nullptr, 0, nullptr);
    WaitForSingleObject(g_winReady, 10000);
    return g_hwnd;
}
bool XGpuWindowFocused() { return g_hwnd && GetForegroundWindow() == g_hwnd; }

// ---- Backend dispatch ------------------------------------------------------------------------------------------------------------------
void D3dLoadShader(bool isPixel, const uint32_t* words, uint32_t dwordCount);
void D3dDraw(const XDrawParams& dp);
void D3dSwap(uint32_t frontbufferPhys, uint32_t width, uint32_t height);
void D3dStats(char* out, size_t outSize);
void D3dIntervalStats(double seconds);
uint32_t D3dFrame();
uint32_t D3dCaptureFrame();
void VkLoadShader(bool isPixel, const uint32_t* words, uint32_t dwordCount);
void VkDraw(const XDrawParams& dp);
void VkSwap(uint32_t frontbufferPhys, uint32_t width, uint32_t height);
void VkStats(char* out, size_t outSize);
void VkIntervalStats(double seconds);
uint32_t VkFrame();
uint32_t VkCaptureFrame();

namespace {
const bool g_vk = []() { const char* r = getenv("ITC_RENDERER"); return r && (_stricmp(r, "vulkan") == 0 || _stricmp(r, "vk") == 0); }();
}
bool XGpuIsVulkan() { return g_vk; }
void XGpuLoadShader(bool isPixel, const uint32_t* words, uint32_t dwordCount) { if (g_vk) VkLoadShader(isPixel, words, dwordCount); else D3dLoadShader(isPixel, words, dwordCount); }
void XGpuDraw(const XDrawParams& dp) { if (g_vk) VkDraw(dp); else D3dDraw(dp); }
void XGpuSwap(uint32_t frontbufferPhys, uint32_t width, uint32_t height) { if (g_vk) VkSwap(frontbufferPhys, width, height); else D3dSwap(frontbufferPhys, width, height); }
void XGpuStats(char* out, size_t outSize) { if (g_vk) VkStats(out, outSize); else D3dStats(out, outSize); }
void XGpuIntervalStats(double seconds) { if (g_vk) VkIntervalStats(seconds); else D3dIntervalStats(seconds); }
uint32_t XGpuFrame() { return g_vk ? VkFrame() : D3dFrame(); }
uint32_t XGpuCaptureFrame() { return g_vk ? VkCaptureFrame() : D3dCaptureFrame(); }
'''
open('C:/itc-work/runtime/src/xgpu_common.cpp', 'w', encoding='utf-8').write(commonSrc)
open(SRC, 'w', encoding='utf-8').write(d3d)
print('shared: %d lines, common: %d lines, d3d: %d -> %d lines' % (inl.count('\n'), commonSrc.count('\n'), len(lines), d3d.count('\n') + 1))
