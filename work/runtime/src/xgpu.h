// Xenos GPU emulation interface (called from the command processor thread in gpu.cpp). xgpu_common.cpp dispatches to the renderer backend:
// Direct3D 11 (xgpu_d3d.cpp, default) or Vulkan (xgpu_vk.cpp, ITC_RENDERER=vulkan).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

// Register file shadow. All GPU register writes (ring type-0 packets, SET_CONSTANT, ...) must go through XGpuWriteRegister.
extern uint32_t g_xregs[0x6000];
inline uint32_t XReg(uint32_t index) { return g_xregs[index]; }
void XGpuWriteRegister(uint32_t index, uint32_t value);
// Register ranges written since a backend last filled its constant buffers (the backend clears it) and a version per texture-fetch entry.
enum : uint32_t { kDirtyVsFloat = 1, kDirtyPsFloat = 2, kDirtyBoolLoop = 4, kDirtyMisc = 16 };
extern uint32_t g_regDirty;
extern uint32_t g_fetchVer[32];

// Shared with the backends (xgpu_common.cpp): the game window, and the 64x36 grey thumbnails of the presented frames used by screen-aware scripted input.
void* XGpuCreateWindow(uint32_t width, uint32_t height);   // HWND; created once, later calls return the same window
bool XGpuWindowFocused();
bool XGpuIsVulkan();
extern std::atomic<bool> g_thumbWanted;
extern std::mutex g_thumbMutex;
extern uint8_t g_thumbData[64 * 36];
extern uint32_t g_thumbFrame;

// Shader upload (IM_LOAD / IM_LOAD_IMMEDIATE). `words` are host-order dwords of the microcode.
void XGpuLoadShader(bool isPixel, const uint32_t* words, uint32_t dwordCount);

struct XDrawParams
{
    uint32_t primType = 0;          // xenos::PrimitiveType
    uint32_t indexCount = 0;
    uint32_t sourceSelect = 2;      // 0 = DMA (index buffer in memory), 1 = immediate, 2 = auto index
    bool index32 = false;           // 32-bit indices (DMA)
    uint32_t dmaPhysBase = 0;       // physical byte address of the index buffer
    uint32_t dmaSizeWords = 0;
    uint32_t dmaEndian = 0;         // xenos::Endian of the index data
};
void XGpuDraw(const XDrawParams& dp);

// XE_SWAP: present the frontbuffer (physical address / size from the packet, fetch constant from slot 0).
void XGpuSwap(uint32_t frontbufferPhys, uint32_t width, uint32_t height);

// Called from the debug dump.
void XGpuStats(char* out, size_t outSize);
