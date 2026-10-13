// Process-wide part of the Xenos GPU emulation: the register file, the game window, the scripted-input thumbnails, and the dispatch of the GPU entry points
// to the selected renderer backend (ITC_RENDERER=vulkan -> xgpu_vk.cpp, otherwise Direct3D 11 -> xgpu_d3d.cpp).
#include "xgpu.h"
#include "xenos_regs.h"
#include "guest.h"
#include "prof.h"
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

uint32_t g_xregs[0x6000];
// Register ranges written since the constant buffers were last filled (the backend's draw clears it) and a version per texture-fetch entry (6 dwords each;
// vertex fetch slot s lives in entry s / 3): the big constant buffers are only rebuilt when something they depend on changed.
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
}
// Screen-aware scripted input: while a script waits for a screen, every 6th frame is reduced to a 64x36 grey thumbnail (mean of 20x20 blocks).
std::atomic<bool> g_thumbWanted{ false };
std::mutex g_thumbMutex;
uint8_t g_thumbData[64 * 36];
uint32_t g_thumbFrame = 0xFFFFFFFFu;
void XGpuWantThumb(bool want) { g_thumbWanted = want; }
bool XGpuGetThumb(uint8_t* out, uint32_t* frame) { std::lock_guard<std::mutex> l(g_thumbMutex); if (g_thumbFrame == 0xFFFFFFFFu) return false; memcpy(out, g_thumbData, sizeof g_thumbData); if (frame) *frame = g_thumbFrame; return true; }

namespace {
HWND g_hwnd = nullptr;
HANDLE g_winReady = nullptr;
uint32_t g_winW = 1280, g_winH = 720;
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE || m == WM_DESTROY) { TerminateProcess(GetCurrentProcess(), 0); }
    return DefWindowProcW(h, m, w, l);
}

DWORD WINAPI WindowThread(LPVOID)
{
    ProfNameThread("window");
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"ITCWindow";
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r = { 0, 0, (LONG)g_winW, (LONG)g_winH };
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    g_hwnd = CreateWindowExW(0, L"ITCWindow", L"Tokyo Xtreme Racer (static recompilation)", style | WS_VISIBLE, 100, 100, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, wc.hInstance, nullptr);
    SetEvent(g_winReady);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
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
