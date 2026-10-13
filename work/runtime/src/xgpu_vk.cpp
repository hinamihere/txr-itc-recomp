// Xenos GPU -> Vulkan backend (ITC_RENDERER=vulkan; xgpu_common.cpp dispatches).
// Same model as the Direct3D 11 backend (xgpu_d3d.cpp), whose API-independent parts it shares (xgpu_shared.inl): draws are replayed with HLSL translated from
// the guest's shader microcode, compiled to SPIR-V by the DirectX Shader Compiler (dxcompiler.dll, loaded at runtime); render targets are Vulkan images;
// resolves copy them into GPU-side textures keyed by their guest address, which are written back to guest memory only when the CPU needs them.
//
// Recording: one command buffer per frame (more when a readback forces an early submit) plus a "pre-upload" command buffer submitted in front of it, which
// takes the uploads into resources the frame has not used yet - a texture or vertex buffer upload then does not interrupt a render pass. Descriptors: set 0
// is pushed per draw (textures as combined image samplers, vertex fetch buffers), set 1 is static (the vertex and pixel constant blocks as dynamic uniform
// buffers in a per-frame ring, the converted index lists). Y is flipped with a negative viewport height, so the translated shaders keep the Direct3D
// conventions and the front-face winding maps 1:1 (as in DXVK). The frame is rendered into an offscreen 1280x720 image that is blitted to the swapchain.
#include "xgpu.h"
#include "xgpu_shader.h"
#include "xenos_regs.h"
#include "guest.h"
#include "prof.h"
#include <windows.h>
#include <nmmintrin.h>
#include <dxgiformat.h>
#include <d3dcommon.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <dxcapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// MinGW ignores __declspec(uuid): the interface IDs of the DXC API used here.
__CRT_UUID_DECL(IDxcBlob, 0x8BA5FB08, 0x5195, 0x40e2, 0xAC, 0x58, 0x0D, 0x98, 0x9C, 0x3A, 0x01, 0x02)
__CRT_UUID_DECL(IDxcBlobEncoding, 0x7241d424, 0x2646, 0x4191, 0x97, 0xc0, 0x98, 0xe9, 0x6e, 0x42, 0xfc, 0x68)
__CRT_UUID_DECL(IDxcBlobUtf8, 0x3DA636C9, 0xBA71, 0x4024, 0xA3, 0x01, 0x30, 0xCB, 0xF1, 0x25, 0x30, 0x5B)
__CRT_UUID_DECL(IDxcOperationResult, 0xCEDB484A, 0xD4E9, 0x445A, 0xB9, 0x91, 0xCA, 0x21, 0xCA, 0x15, 0x7D, 0xC2)
__CRT_UUID_DECL(IDxcResult, 0x58346CDA, 0xDDE7, 0x4497, 0x94, 0x61, 0x6F, 0x87, 0xAF, 0x5E, 0x06, 0x59)
__CRT_UUID_DECL(IDxcCompiler3, 0x228B4687, 0x5A6A, 0x4730, 0x90, 0x0C, 0x97, 0x02, 0xB2, 0x20, 0x3F, 0x54)

bool XGpuTilingActive();      // gpu.cpp: a predicated-tiling frame is in progress
bool XGpuTilingFirstPass();   // gpu.cpp: ... and this is its first strip pass
extern uint64_t g_cpIdleTicks;  // gpu.cpp: time the command processor waited for work

namespace {

#include "xgpu_shared.inl"

// ---- Vulkan entry points (loaded from vulkan-1.dll at runtime) ------------------------------------------------------------------------
#define VK_GLOBAL_FUNCS(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define VK_INSTANCE_FUNCS(X) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
    X(vkCreateWin32SurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)
#define VK_DEVICE_FUNCS(X) \
    X(vkGetDeviceQueue) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateSampler) X(vkCreateShaderModule) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkCreateGraphicsPipelines) X(vkCreatePipelineLayout) \
    X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateRenderPass) X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) X(vkCreateCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdPushDescriptorSetKHR) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants) \
    X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetDepthBias) X(vkCmdDraw) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage)
#define VK_DECLARE(n) PFN_##n n = nullptr;
VK_GLOBAL_FUNCS(VK_DECLARE)
VK_INSTANCE_FUNCS(VK_DECLARE)
VK_DEVICE_FUNCS(VK_DECLARE)
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

bool VkOk(VkResult r, const char* what)
{
    if (r == VK_SUCCESS) return true;
    static int n = 0;
    if (n++ < 40) Log("[vk] %s failed: VkResult %d", what, (int)r);
    return false;
}

// ---- Device state -----------------------------------------------------------------------------------------------------------------------
HWND g_hwnd = nullptr;
bool g_initTried = false, g_initOk = false;
VkInstance g_inst = VK_NULL_HANDLE;
VkPhysicalDevice g_pd = VK_NULL_HANDLE;
VkDevice g_dev = VK_NULL_HANDLE;
VkQueue g_queue = VK_NULL_HANDLE;
uint32_t g_qfam = 0;
VkPhysicalDeviceProperties g_props = {};
VkPhysicalDeviceMemoryProperties g_memProps = {};
bool g_haveDepthClamp = false, g_haveAniso = false, g_haveMirrorOnce = false;
VkFormat g_depthFmt = VK_FORMAT_D24_UNORM_S8_UINT;
VkSurfaceKHR g_surface = VK_NULL_HANDLE;
VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
std::vector<VkImage> g_swapImages;
VkFormat g_swapFmt = VK_FORMAT_UNDEFINED;
VkExtent2D g_swapExtent = {};
bool g_swapDirty = false;
uint64_t g_profAcquire = 0;

// ---- Memory: a first-fit sub-allocator over 64 MB blocks (Vulkan limits the number of allocations; the game has thousands of textures) ----
struct MemBlock
{
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0, used = 0;
    uint32_t type = 0;
    bool linear = false;               // buffers and optimally tiled images never share a block (bufferImageGranularity)
    uint8_t* mapped = nullptr;
    std::map<VkDeviceSize, VkDeviceSize> freeList;   // offset -> size
};
std::vector<std::unique_ptr<MemBlock>> g_blocks;
uint64_t g_memDedicatedBytes = 0;
struct Alloc
{
    MemBlock* block = nullptr;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, size = 0;
    uint8_t* ptr = nullptr;
    bool dedicated = false;
};
const VkDeviceSize kBlockSize = 64ull << 20;

int FindMemType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0)
{
    for (uint32_t i = 0; i < g_memProps.memoryTypeCount; i++)
        if ((bits >> i) & 1 && (g_memProps.memoryTypes[i].propertyFlags & want) == want && !(g_memProps.memoryTypes[i].propertyFlags & avoid)) return (int)i;
    return -1;
}

// host: CPU-visible memory (rings, staging, readback); otherwise device-local memory.
bool AllocMem(const VkMemoryRequirements& req, bool host, bool linear, Alloc& out, bool cached = false)
{
    int type = -1;
    if (host)
    {
        if (cached) type = FindMemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (type < 0) type = FindMemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type < 0) type = FindMemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    else
    {
        type = FindMemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type < 0) type = FindMemType(req.memoryTypeBits, 0);
    }
    if (type < 0) { Log("[vk] no memory type for bits %08X host=%d", req.memoryTypeBits, (int)host); return false; }
    const bool mappable = (g_memProps.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    out = Alloc();
    if (req.size > kBlockSize / 2)
    {
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)type;
        if (!VkOk(vkAllocateMemory(g_dev, &ai, nullptr, &out.mem), "vkAllocateMemory (dedicated)")) return false;
        if (mappable && !VkOk(vkMapMemory(g_dev, out.mem, 0, VK_WHOLE_SIZE, 0, (void**)&out.ptr), "vkMapMemory")) return false;
        out.size = req.size; out.dedicated = true; g_memDedicatedBytes += req.size;
        return true;
    }
    for (int pass = 0; pass < 2; pass++)
    {
        for (auto& bp : g_blocks)
        {
            MemBlock& b = *bp;
            if (b.type != (uint32_t)type || b.linear != linear) continue;
            for (auto it = b.freeList.begin(); it != b.freeList.end(); ++it)
            {
                const VkDeviceSize off = it->first, sz = it->second;
                const VkDeviceSize al = (off + req.alignment - 1) / req.alignment * req.alignment;
                if (al + req.size > off + sz) continue;
                b.freeList.erase(it);
                if (al > off) b.freeList[off] = al - off;
                if (al + req.size < off + sz) b.freeList[al + req.size] = off + sz - (al + req.size);
                out.block = &b; out.mem = b.mem; out.offset = al; out.size = req.size; out.ptr = b.mapped ? b.mapped + al : nullptr;
                b.used += req.size;
                return true;
            }
        }
        if (pass == 1) break;
        auto nb = std::make_unique<MemBlock>();
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = kBlockSize; ai.memoryTypeIndex = (uint32_t)type;
        if (!VkOk(vkAllocateMemory(g_dev, &ai, nullptr, &nb->mem), "vkAllocateMemory (block)")) return false;
        if (mappable && !VkOk(vkMapMemory(g_dev, nb->mem, 0, VK_WHOLE_SIZE, 0, (void**)&nb->mapped), "vkMapMemory")) return false;
        nb->size = kBlockSize; nb->type = (uint32_t)type; nb->linear = linear; nb->freeList[0] = kBlockSize;
        g_blocks.push_back(std::move(nb));
        if (g_log) Log("[vk] memory block %zu: type %d %s", g_blocks.size(), type, linear ? "buffers" : "images");
    }
    return false;
}

void FreeMem(Alloc& a)
{
    if (a.dedicated) { if (a.mem) vkFreeMemory(g_dev, a.mem, nullptr); g_memDedicatedBytes -= a.size; a = Alloc(); return; }
    if (!a.block) return;
    MemBlock& b = *a.block;
    VkDeviceSize off = a.offset, sz = a.size;
    b.used -= sz;
    auto next = b.freeList.lower_bound(off);
    if (next != b.freeList.end() && off + sz == next->first) { sz += next->second; next = b.freeList.erase(next); }
    if (next != b.freeList.begin())
    {
        auto prev = std::prev(next);
        if (prev->first + prev->second == off) { prev->second += sz; a = Alloc(); return; }
    }
    b.freeList[off] = sz;
    a = Alloc();
}

// ---- Frames, command buffers, deferred destruction -------------------------------------------------------------------------------------
constexpr uint32_t kFrames = 2;
struct FrameCtx
{
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmds;
    uint32_t nextCmd = 0;
    VkFence fence = VK_NULL_HANDLE;
    bool inFlight = false;
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    std::vector<std::function<void()>> garbage;   // destroyed once this frame's work has completed
};
FrameCtx g_fr[kFrames];
uint32_t g_fi = 0;
VkCommandBuffer g_cmd = VK_NULL_HANDLE;   // main command buffer of the current submission
VkCommandBuffer g_pre = VK_NULL_HANDLE;   // pre-upload command buffer (executes before g_cmd)
uint64_t g_cmdSerial = 1;                 // identifies the current submission (resources remember the serial of their last use)
VkFence g_flushFence = VK_NULL_HANDLE;
uint64_t g_flushes = 0, g_profFlushWait = 0;
void OnNewCmd();
void EndPass();

void Defer(std::function<void()> fn) { g_fr[g_fi].garbage.push_back(std::move(fn)); }

VkCommandBuffer NewCmd()
{
    FrameCtx& f = g_fr[g_fi];
    if (f.nextCmd == f.cmds.size())
    {
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = f.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VkCommandBuffer cb;
        if (!VkOk(vkAllocateCommandBuffers(g_dev, &ai, &cb), "vkAllocateCommandBuffers")) return VK_NULL_HANDLE;
        f.cmds.push_back(cb);
    }
    VkCommandBuffer cb = f.cmds[f.nextCmd++];
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}
void FullBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst, VkAccessFlags dstAccess)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = srcAccess; mb.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
}
VkCommandBuffer Cmd()
{
    if (!g_cmd) { g_cmd = NewCmd(); OnNewCmd(); }
    return g_cmd;
}
VkCommandBuffer Pre()
{
    if (!g_pre)
    {
        g_pre = NewCmd();
        // the uploads must wait for every earlier read of the resources they overwrite (frames still in flight)
        FullBarrier(g_pre, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    }
    return g_pre;
}
// Ends the recording and submits [pre-upload, main]. fence/semaphores are optional.
void SubmitWork(VkFence fence, VkSemaphore wait = VK_NULL_HANDLE, VkPipelineStageFlags waitStage = 0, VkSemaphore signal = VK_NULL_HANDLE)
{
    EndPass();
    VkCommandBuffer list[2]; uint32_t n = 0;
    if (g_pre)
    {
        FullBarrier(g_pre, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        vkEndCommandBuffer(g_pre); list[n++] = g_pre;
    }
    if (g_cmd) { vkEndCommandBuffer(g_cmd); list[n++] = g_cmd; }
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = n; si.pCommandBuffers = list;
    if (wait) { si.waitSemaphoreCount = 1; si.pWaitSemaphores = &wait; si.pWaitDstStageMask = &waitStage; }
    if (signal) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &signal; }
    if (n || fence || wait || signal) VkOk(vkQueueSubmit(g_queue, 1, &si, fence), "vkQueueSubmit");
    g_pre = g_cmd = VK_NULL_HANDLE;
    g_cmdSerial++;
}
void ResetRings();
// Submits everything recorded so far and waits until the GPU has finished it (and every earlier submission): readbacks, full rings.
void FlushAndWait()
{
    const uint64_t t0 = QpcNow();
    SubmitWork(g_flushFence);
    vkWaitForFences(g_dev, 1, &g_flushFence, VK_TRUE, UINT64_MAX);
    vkResetFences(g_dev, 1, &g_flushFence);
    g_flushes++; g_profFlushWait += QpcNow() - t0;
    ResetRings();   // the GPU is idle: the ring space of this frame can be reused
}

// ---- Buffers and images ------------------------------------------------------------------------------------------------------------------
struct Buf
{
    VkBuffer buf = VK_NULL_HANDLE;
    Alloc mem;
    VkDeviceSize size = 0;
    uint8_t* ptr = nullptr;   // host-visible buffers: persistently mapped
    uint64_t lastUse = 0;
};
bool CreateBuf(Buf& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host, bool cached = false)
{
    b = Buf();
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = size; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!VkOk(vkCreateBuffer(g_dev, &ci, nullptr, &b.buf), "vkCreateBuffer")) return false;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(g_dev, b.buf, &req);
    if (!AllocMem(req, host, true, b.mem, cached)) { vkDestroyBuffer(g_dev, b.buf, nullptr); b.buf = VK_NULL_HANDLE; return false; }
    vkBindBufferMemory(g_dev, b.buf, b.mem.mem, b.mem.offset);
    b.size = size; b.ptr = b.mem.ptr;
    return true;
}
void RetireBuf(Buf& b)
{
    if (!b.buf) return;
    VkBuffer h = b.buf; Alloc a = b.mem;
    Defer([h, a]() mutable { vkDestroyBuffer(g_dev, h, nullptr); FreeMem(a); });
    b = Buf();
}

struct Img
{
    VkImage img = VK_NULL_HANDLE;
    Alloc mem;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    VkFormat rtFmt = VK_FORMAT_UNDEFINED;  // format of the attachment view (render pass compatibility)
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t w = 0, h = 0, levels = 1, layers = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageView view = VK_NULL_HANDLE;     // sampled view (2D / cube / 3D; depth images: depth aspect)
    VkImageView rtView = VK_NULL_HANDLE;   // attachment view (sRGB render targets use another format than the sampled view)
    uint64_t lastUse = 0;
};
struct ImgDesc
{
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    uint32_t w = 1, h = 1, levels = 1, layers = 1;
    VkImageUsageFlags usage = 0;
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
    VkFormat viewFmt = VK_FORMAT_UNDEFINED, rtFmt = VK_FORMAT_UNDEFINED;   // when they differ from fmt (mutable-format image)
    bool depth = false, is3D = false;
};
std::unique_ptr<Img> CreateImg(const ImgDesc& d)
{
    auto im = std::make_unique<Img>();
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = d.is3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = d.fmt; ci.extent = { d.w, d.h, 1 }; ci.mipLevels = d.levels; ci.arrayLayers = d.layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = d.usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d.viewType == VK_IMAGE_VIEW_TYPE_CUBE) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if ((d.viewFmt && d.viewFmt != d.fmt) || (d.rtFmt && d.rtFmt != d.fmt)) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (!VkOk(vkCreateImage(g_dev, &ci, nullptr, &im->img), "vkCreateImage")) return nullptr;
    VkMemoryRequirements req; vkGetImageMemoryRequirements(g_dev, im->img, &req);
    if (!AllocMem(req, false, false, im->mem)) { vkDestroyImage(g_dev, im->img, nullptr); return nullptr; }
    vkBindImageMemory(g_dev, im->img, im->mem.mem, im->mem.offset);
    im->fmt = d.fmt; im->rtFmt = d.rtFmt ? d.rtFmt : d.fmt; im->w = d.w; im->h = d.h; im->levels = d.levels; im->layers = d.layers;
    im->aspect = d.depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = im->img; vi.viewType = d.viewType; vi.format = d.viewFmt ? d.viewFmt : d.fmt;
    vi.subresourceRange = { d.depth ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT, 0, d.levels, 0, d.layers };
    if (d.usage & VK_IMAGE_USAGE_SAMPLED_BIT) VkOk(vkCreateImageView(g_dev, &vi, nullptr, &im->view), "vkCreateImageView");
    if (d.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
    {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = d.rtFmt ? d.rtFmt : d.fmt;
        vi.subresourceRange = { im->aspect, 0, 1, 0, 1 };
        VkOk(vkCreateImageView(g_dev, &vi, nullptr, &im->rtView), "vkCreateImageView (attachment)");
    }
    return im;
}
void ForgetFramebuffers(VkImageView v);
void RetireImg(std::unique_ptr<Img>& im)
{
    if (!im) return;
    if (im->rtView) ForgetFramebuffers(im->rtView);
    Img* p = im.release();
    Defer([p]() { if (p->view) vkDestroyImageView(g_dev, p->view, nullptr); if (p->rtView) vkDestroyImageView(g_dev, p->rtView, nullptr); vkDestroyImage(g_dev, p->img, nullptr); FreeMem(p->mem); delete p; });
}

// Layout transition with a full (conservative) dependency; outside a render pass.
void ImgBarrier(VkCommandBuffer cb, Img& im, VkImageLayout to)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = im.layout; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.img; b.subresourceRange = { im.aspect, 0, im.levels, 0, im.layers };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    im.layout = to;
}
// Puts an image into `layout` for use by the main command buffer (ends the render pass when a transition is needed).
void UseImg(Img& im, VkImageLayout layout)
{
    if (im.layout != layout) { EndPass(); ImgBarrier(Cmd(), im, layout); }
    im.lastUse = g_cmdSerial;
}
VkImageLayout SampledLayout(const Img& im) { return (im.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
VkImageLayout AttachmentLayout(const Img& im) { return (im.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; }

// ---- Per-frame rings (host-visible): constants, dynamic vertex data, upload staging --------------------------------------------------------
// Each ring is split into kFrames parts; frame i allocates linearly from part i, which the GPU has finished with when the frame starts. When a part is full
// the frame's work so far is submitted and waited for (FlushAndWait), which frees the whole part again.
struct Ring
{
    Buf buf;
    VkDeviceSize part = 0, pos = 0;
    const char* name = "";
    uint64_t overflows = 0, bytes = 0;
};
Ring g_cbRing, g_vbRing, g_stRing;
uint32_t g_cbEpoch = 1;   // bumped whenever the constant ring restarts (cached constant offsets are void)
void ResetRings() { g_cbRing.pos = g_vbRing.pos = g_stRing.pos = 0; g_cbEpoch++; }
// Returns the byte offset in r.buf, or ~0 when n does not fit into a part at all.
VkDeviceSize RingAlloc(Ring& r, VkDeviceSize n, VkDeviceSize align)
{
    if (n > r.part) return ~(VkDeviceSize)0;
    VkDeviceSize p = (r.pos + align - 1) / align * align;
    if (p + n > r.part)
    {
        r.overflows++;
        static int logged = 0;
        if (logged++ < 10) Log("[vk] %s ring full (frame %u): flushing", r.name, g_frame);
        FlushAndWait();
        p = 0;
    }
    r.pos = p + n; r.bytes += n;
    return (VkDeviceSize)g_fi * r.part + p;
}
bool CreateRing(Ring& r, const char* name, VkDeviceSize partSize, VkBufferUsageFlags usage)
{
    r.name = name; r.part = partSize;
    return CreateBuf(r.buf, partSize * kFrames, usage, true);
}

// Copies bytes into a buffer through the staging ring: recorded into the pre-upload command buffer when the buffer has not been used by the commands
// recorded so far, otherwise inline (which interrupts the render pass).
Buf g_bigStage;   // temporary staging for uploads larger than a ring part
VkDeviceSize StageBytes(const void* const* parts, const size_t* sizes, uint32_t n, VkBuffer& srcBuf)
{
    size_t total = 0; for (uint32_t i = 0; i < n; i++) total += (sizes[i] + 15) & ~(size_t)15;
    VkDeviceSize off = RingAlloc(g_stRing, total, 16);
    uint8_t* dst;
    if (off == ~(VkDeviceSize)0)
    {
        Buf tmp;
        if (!CreateBuf(tmp, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return ~(VkDeviceSize)0;
        dst = tmp.ptr; srcBuf = tmp.buf; off = 0;
        RetireBuf(tmp);   // destroyed once this frame has completed
    }
    else { dst = g_stRing.buf.ptr + off; srcBuf = g_stRing.buf.buf; }
    size_t o = 0;
    for (uint32_t i = 0; i < n; i++) { memcpy(dst + o, parts[i], sizes[i]); o += (sizes[i] + 15) & ~(size_t)15; }
    return off;
}
VkCommandBuffer UploadCmd(uint64_t lastUse)
{
    if (lastUse == g_cmdSerial)
    {
        EndPass();
        VkCommandBuffer cb = Cmd();
        FullBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        return cb;
    }
    return Pre();
}
void UploadDone(VkCommandBuffer cb)
{
    if (cb == g_cmd) FullBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
}
void UploadToBuffer(Buf& dst, VkDeviceSize dstOff, const void* src, VkDeviceSize n)
{
    VkBuffer sb; const void* parts[1] = { src }; const size_t sizes[1] = { (size_t)n };
    const VkDeviceSize so = StageBytes(parts, sizes, 1, sb);
    if (so == ~(VkDeviceSize)0) return;
    VkCommandBuffer cb = UploadCmd(dst.lastUse);
    VkBufferCopy c = { so, dstOff, n };
    vkCmdCopyBuffer(cb, sb, dst.buf, 1, &c);
    UploadDone(cb);
}
// Uploads mip levels / array layers (tightly packed rows) into an image and leaves it in the sampled layout.
struct Subres { const uint8_t* data; size_t bytes; uint32_t level, layer, w, h; };
void UploadToImage(Img& im, const Subres* subs, uint32_t n)
{
    std::vector<const void*> parts(n); std::vector<size_t> sizes(n);
    for (uint32_t i = 0; i < n; i++) { parts[i] = subs[i].data; sizes[i] = subs[i].bytes; }
    VkBuffer sb;
    const VkDeviceSize so = StageBytes(parts.data(), sizes.data(), n, sb);
    if (so == ~(VkDeviceSize)0) return;
    VkCommandBuffer cb = UploadCmd(im.lastUse);
    ImgBarrier(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    std::vector<VkBufferImageCopy> regions(n);
    VkDeviceSize o = so;
    for (uint32_t i = 0; i < n; i++)
    {
        VkBufferImageCopy& r = regions[i];
        r = {};
        r.bufferOffset = o; r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, subs[i].level, subs[i].layer, 1 };
        r.imageExtent = { subs[i].w, subs[i].h, 1 };
        o += (subs[i].bytes + 15) & ~(size_t)15;
    }
    vkCmdCopyBufferToImage(cb, sb, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, regions.data());
    ImgBarrier(cb, im, SampledLayout(im));
}

// ---- Render passes and framebuffers ----------------------------------------------------------------------------------------------------
// Every pass loads and stores all attachments (no clears: those are explicit), so a render pass only depends on the attachment formats.
struct RpKey { VkFormat color[4]; VkFormat depth; uint32_t numColor; bool operator<(const RpKey& o) const { return memcmp(this, &o, sizeof *this) < 0; } };
std::map<RpKey, VkRenderPass> g_renderPasses;
VkRenderPass GetRenderPass(const RpKey& k)
{
    auto it = g_renderPasses.find(k);
    if (it != g_renderPasses.end()) return it->second;
    VkAttachmentDescription att[5]; VkAttachmentReference refs[4]; VkAttachmentReference dref;
    uint32_t na = 0;
    for (uint32_t i = 0; i < k.numColor; i++)
    {
        if (!k.color[i]) { refs[i] = { VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED }; continue; }
        VkAttachmentDescription& a = att[na];
        a = {}; a.format = k.color[i]; a.samples = VK_SAMPLE_COUNT_1_BIT; a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        refs[i] = { na++, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    }
    if (k.depth)
    {
        VkAttachmentDescription& a = att[na];
        a = {}; a.format = k.depth; a.samples = VK_SAMPLE_COUNT_1_BIT; a.loadOp = a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        dref = { na++, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    }
    VkSubpassDescription sp = {};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = k.numColor; sp.pColorAttachments = refs;
    sp.pDepthStencilAttachment = k.depth ? &dref : nullptr;
    // Passes on the same attachments follow each other without barriers: these dependencies order them (and everything else) conservatively.
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; deps[0].dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; deps[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT; deps[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    deps[1].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; deps[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = na; ci.pAttachments = att; ci.subpassCount = 1; ci.pSubpasses = &sp; ci.dependencyCount = 2; ci.pDependencies = deps;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkOk(vkCreateRenderPass(g_dev, &ci, nullptr, &rp), "vkCreateRenderPass");
    g_renderPasses[k] = rp;
    return rp;
}
struct FbKey { VkRenderPass rp; VkImageView views[5]; uint32_t n, w, h; bool operator<(const FbKey& o) const { return memcmp(this, &o, sizeof *this) < 0; } };
std::map<FbKey, VkFramebuffer> g_framebuffers;
VkFramebuffer GetFramebuffer(const FbKey& k)
{
    auto it = g_framebuffers.find(k);
    if (it != g_framebuffers.end()) return it->second;
    VkFramebufferCreateInfo ci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    ci.renderPass = k.rp; ci.attachmentCount = k.n; ci.pAttachments = k.views; ci.width = k.w; ci.height = k.h; ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkOk(vkCreateFramebuffer(g_dev, &ci, nullptr, &fb), "vkCreateFramebuffer");
    g_framebuffers[k] = fb;
    return fb;
}
void ForgetFramebuffers(VkImageView v)
{
    for (auto it = g_framebuffers.begin(); it != g_framebuffers.end();)
    {
        bool uses = false;
        for (uint32_t i = 0; i < it->first.n; i++) uses |= it->first.views[i] == v;
        if (uses) { VkFramebuffer fb = it->second; Defer([fb]() { vkDestroyFramebuffer(g_dev, fb, nullptr); }); it = g_framebuffers.erase(it); }
        else ++it;
    }
}

// The active render pass. Attachments: up to 4 colour slots (gaps allowed) and a depth/stencil image.
struct PassState
{
    bool active = false;
    Img* color[4] = {};
    Img* depth = nullptr;
    uint32_t numColor = 0, w = 0, h = 0;
    RpKey rpKey = {};
    VkRenderPass rp = VK_NULL_HANDLE;
};
PassState g_pass;
uint64_t g_passCount = 0;
void EndPass()
{
    if (!g_pass.active) return;
    vkCmdEndRenderPass(g_cmd);
    g_pass.active = false;
}
// Makes sure a render pass on exactly these attachments is active (w x h = render area).
void EnsurePass(Img* const color[4], uint32_t numColor, Img* depth, uint32_t w, uint32_t h)
{
    if (g_pass.active && g_pass.numColor == numColor && g_pass.depth == depth && g_pass.w == w && g_pass.h == h && memcmp(g_pass.color, color, sizeof g_pass.color) == 0) return;
    EndPass();
    VkCommandBuffer cb = Cmd();
    RpKey k = {};
    FbKey fk = {};
    for (uint32_t i = 0; i < numColor; i++)
        if (color[i])
        {
            if (color[i]->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) ImgBarrier(cb, *color[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            color[i]->lastUse = g_cmdSerial;
            k.color[i] = color[i]->rtFmt;
            fk.views[fk.n++] = color[i]->rtView;
        }
    if (depth)
    {
        if (depth->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) ImgBarrier(cb, *depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        depth->lastUse = g_cmdSerial;
        k.depth = depth->fmt;
        fk.views[fk.n++] = depth->rtView;
    }
    k.numColor = numColor;
    g_pass.rpKey = k;
    g_pass.rp = GetRenderPass(k);
    fk.rp = g_pass.rp; fk.w = w; fk.h = h;
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = g_pass.rp; bi.framebuffer = GetFramebuffer(fk); bi.renderArea = { { 0, 0 }, { w, h } };
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    g_pass.active = true; memcpy(g_pass.color, color, sizeof g_pass.color); g_pass.depth = depth; g_pass.numColor = numColor; g_pass.w = w; g_pass.h = h;
    g_passCount++;
}

// ---- Formats ---------------------------------------------------------------------------------------------------------------------------------
VkFormat VkFormatOf(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case DXGI_FORMAT_R16G16B16A16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case DXGI_FORMAT_R16G16_UNORM: return VK_FORMAT_R16G16_UNORM;
    case DXGI_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case DXGI_FORMAT_R16_UNORM: return VK_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
    case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_BC1_UNORM: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case DXGI_FORMAT_BC2_UNORM: return VK_FORMAT_BC2_UNORM_BLOCK;
    case DXGI_FORMAT_BC3_UNORM: return VK_FORMAT_BC3_UNORM_BLOCK;
    case DXGI_FORMAT_BC5_UNORM: return VK_FORMAT_BC5_UNORM_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
    }
}
uint32_t BytesPerPixel(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SFLOAT: return 2;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 4;
    }
}

// ---- Samplers ---------------------------------------------------------------------------------------------------------------------------------
std::unordered_map<uint64_t, VkSampler> g_samplers;
VkSamplerAddressMode ClampToAddr(uint32_t c)
{
    switch (c)
    {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return g_haveMirrorOnce ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 6: case 7: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}
VkSampler GetSampler(const TexFetch& tf)
{
    const uint32_t cx = (tf.d[0] >> 10) & 7, cy = (tf.d[0] >> 13) & 7, cz = (tf.d[0] >> 16) & 7;
    const uint32_t mag = (tf.d[3] >> 19) & 3, mn = (tf.d[3] >> 21) & 3, mip = (tf.d[3] >> 23) & 3;
    const uint32_t aniso = (tf.d[3] >> 26) & 7;   // 0/1 = off, 2 = 2:1, 3 = 4:1, 4 = 8:1, 5+ = 16:1
    const uint64_t key = cx | (cy << 3) | (cz << 6) | (mag << 9) | (mn << 11) | (mip << 13) | ((uint64_t)aniso << 15);
    VkSampler& s = g_samplers[key];
    if (!s)
    {
        VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        const bool linMin = mn == 1 || mn == 3, linMag = mag == 1 || mag == 3, linMip = mip == 1;
        ci.magFilter = linMag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; ci.minFilter = linMin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        ci.mipmapMode = linMip ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = ClampToAddr(cx); ci.addressModeV = ClampToAddr(cy); ci.addressModeW = ClampToAddr(cz);
        ci.maxLod = mip == 2 ? 0.0f : VK_LOD_CLAMP_NONE;
        ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        static const bool noAniso = getenv("ITC_NO_ANISO") != nullptr;
        if (!noAniso && g_haveAniso && linMin && linMag && linMip && aniso >= 2)
        {
            ci.anisotropyEnable = VK_TRUE;
            ci.maxAnisotropy = std::min(g_props.limits.maxSamplerAnisotropy, aniso == 2 ? 2.0f : aniso == 3 ? 4.0f : aniso == 4 ? 8.0f : 16.0f);
        }
        VkOk(vkCreateSampler(g_dev, &ci, nullptr, &s), "vkCreateSampler");
    }
    return s;
}
VkSampler GetSamplerFast(uint32_t slot)
{
    struct Memo { uint32_t d0, d3; VkSampler s; };
    static Memo memo[32] = {};
    const uint32_t d0 = XReg(xr::SHADER_CONSTANT_FETCH_00_0 + slot * 6), d3 = XReg(xr::SHADER_CONSTANT_FETCH_00_3 + slot * 6);
    Memo& m = memo[slot & 31];
    if (m.s && m.d0 == d0 && m.d3 == d3) return m.s;
    m.s = GetSampler(GetTexFetch(slot)); m.d0 = d0; m.d3 = d3;
    return m.s;
}
VkSampler g_linearClamp = VK_NULL_HANDLE, g_pointClamp = VK_NULL_HANDLE;

// ---- HLSL -> SPIR-V (DirectX Shader Compiler) -------------------------------------------------------------------------------------------------
DxcCreateInstanceProc g_dxcCreate = nullptr;
bool LoadDxc()
{
    if (g_dxcCreate) return true;
    // dxcompiler.dll (+ dxil.dll) next to the executable, else from the build tree's deps folder.
    HMODULE m = LoadLibraryA("dxcompiler.dll");
    if (!m) m = LoadLibraryA("C:\\itc-work\\deps\\dxc\\bin\\x64\\dxcompiler.dll");
    if (!m) { Log("[vk] dxcompiler.dll not found: the Vulkan renderer cannot compile shaders"); return false; }
    g_dxcCreate = (DxcCreateInstanceProc)GetProcAddress(m, "DxcCreateInstance");
    return g_dxcCreate != nullptr;
}
// One compiler instance per thread (IDxcCompiler3 is not thread-safe).
bool CompileSpirv(const std::string& src, const char* profile, const char* entry, const char* tag, std::vector<uint32_t>& out, std::string* errors = nullptr)
{
    thread_local IDxcCompiler3* comp = nullptr;
    if (!comp && (!g_dxcCreate || FAILED(g_dxcCreate(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void**)&comp)))) { Log("[vk] DxcCreateInstance failed"); return false; }
    std::wstring wp(profile, profile + strlen(profile)), we(entry, entry + strlen(entry));
    static const bool noOpt = getenv("ITC_VK_SHADER_O0") != nullptr;
    const wchar_t* args[] = { L"-E", we.c_str(), L"-T", wp.c_str(), L"-spirv", L"-HV", L"2018", L"-fvk-use-dx-layout", noOpt ? L"-O0" : L"-O3" };
    DxcBuffer buf = { src.data(), src.size(), DXC_CP_UTF8 };
    IDxcResult* res = nullptr;
    HRESULT hr = comp->Compile(&buf, args, sizeof args / sizeof args[0], nullptr, __uuidof(IDxcResult), (void**)&res);
    HRESULT st = E_FAIL;
    if (SUCCEEDED(hr) && res) res->GetStatus(&st);
    bool ok = false;
    if (SUCCEEDED(hr) && SUCCEEDED(st))
    {
        IDxcBlob* obj = nullptr;
        if (SUCCEEDED(res->GetResult(&obj)) && obj && obj->GetBufferSize() >= 4)
        {
            out.assign((const uint32_t*)obj->GetBufferPointer(), (const uint32_t*)obj->GetBufferPointer() + obj->GetBufferSize() / 4);
            ok = true;
        }
        if (obj) obj->Release();
    }
    if (!ok)
    {
        std::string msg;
        IDxcBlobEncoding* eb = nullptr;
        if (res && SUCCEEDED(res->GetErrorBuffer(&eb)) && eb && eb->GetBufferSize()) msg.assign((const char*)eb->GetBufferPointer(), eb->GetBufferSize());
        if (eb) eb->Release();
        if (errors) *errors = msg;
        Log("[vk] HLSL -> SPIR-V failed (%s) hr=%08X: %.1500s", tag, (unsigned)(SUCCEEDED(hr) ? st : hr), msg.c_str());
        CreateDirectoryA("C:/itc-work/shaders", nullptr);
        char path[256]; snprintf(path, sizeof path, "C:/itc-work/shaders/vk_%s.fail.hlsl", tag);
        if (FILE* f = fopen(path, "wb")) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
    }
    else if (g_dumpShaders)
    {
        CreateDirectoryA("C:/itc-work/shaders", nullptr);
        char path[256]; snprintf(path, sizeof path, "C:/itc-work/shaders/vk_%s.hlsl", tag);
        if (FILE* f = fopen(path, "wb")) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
    }
    if (res) res->Release();
    return ok;
}
VkShaderModule MakeModule(const std::vector<uint32_t>& spv)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = spv.size() * 4; ci.pCode = spv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    VkOk(vkCreateShaderModule(g_dev, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

// ---- Guest shaders: translation, background compilation, SPIR-V disk cache -------------------------------------------------------------------
struct CompileJob
{
    std::string hlsl, profile, tag, cachePath;
    std::atomic<int> state{ 0 };   // 0 = compiling, 1 = done, 2 = failed
    std::vector<uint32_t> spirv;
};
struct ShaderEntry
{
    VkShaderModule module = VK_NULL_HANDLE;
    XShaderInfo info;
    bool failed = false;
    std::string name;
    uint64_t key = 0;
    std::shared_ptr<CompileJob> job;
};
std::unordered_map<uint64_t, ShaderEntry> g_shaders;
std::mutex g_jobMutex;
std::condition_variable g_jobCv;
std::deque<std::shared_ptr<CompileJob>> g_jobQueue;
bool g_workersStarted = false;
void CompileWorker()
{
    ProfNameThread("vk-shader");
    for (;;)
    {
        std::shared_ptr<CompileJob> job;
        {
            std::unique_lock<std::mutex> l(g_jobMutex);
            g_jobCv.wait(l, [] { return !g_jobQueue.empty(); });
            job = g_jobQueue.front();
            g_jobQueue.pop_front();
        }
        if (CompileSpirv(job->hlsl, job->profile.c_str(), "main", job->tag.c_str(), job->spirv))
        {
            const std::string tmp = job->cachePath + ".tmp";
            if (FILE* f = fopen(tmp.c_str(), "wb"))
            {
                const bool ok = fwrite(job->spirv.data(), 4, job->spirv.size(), f) == job->spirv.size();
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

ShaderEntry* GetShader(const ShaderCode& sc, bool isPixel)
{
    static const bool syncShaders = getenv("ITC_SYNC_SHADERS") != nullptr;
    uint32_t interp = 0; int pgen = -1;
    if (isPixel) GetPsParams(interp, pgen);
    const uint64_t key = sc.hash ^ (isPixel ? 0x5555000000000000ull : 0) ^ ((uint64_t)interp << 40) ^ ((uint64_t)(pgen + 1) << 48);
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
        if (st != 1 || !(e.module = MakeModule(job->spirv))) { e.failed = true; g_shaderFail++; return nullptr; }
        g_shaderOk++;
        return &e;
    }
    ShaderEntry& e = g_shaders[key];
    e.key = key;
    XTranslateParams tp;
    tp.isPixel = isPixel; tp.psInterpolatorCount = interp; tp.psParamGenReg = pgen; tp.vulkan = true;
    std::string hlsl = TranslateXenosShader(sc.code.data(), (uint32_t)sc.code.size(), tp, e.info);
    char tag[64]; snprintf(tag, sizeof tag, "%s_%016llx_%u", isPixel ? "ps" : "vs", (unsigned long long)sc.hash, interp);
    e.name = tag;
    // The descriptor layout has room for 4 vertex shader textures, 12 vertex fetch buffers and 16 pixel shader textures.
    const size_t nTex = e.info.textureSlots.size();
    if ((isPixel && nTex > kVkPsTexMax) || (!isPixel && (nTex > kVkVsTexMax || e.info.vfetchSlots.size() > kVkVbMax)))
    {
        Log("[vk] shader %s uses %zu textures / %zu vertex fetch slots: more than the descriptor layout holds, its draws are skipped", tag, nTex, e.info.vfetchSlots.size());
        e.failed = true; g_shaderFail++;
        return nullptr;
    }
    uint64_t srcHash = 0xCBF29CE484222325ull ^ (isPixel ? 0x77ull : 0x33ull);
    for (unsigned char ch : hlsl) { srcHash ^= ch; srcHash *= 0x100000001B3ull; }
    static const char* cacheDir = getenv("ITC_VK_SHADER_O0") ? "shader_cache_vk0" : "shader_cache_vk";
    char cacheFile[160]; snprintf(cacheFile, sizeof cacheFile, "%s/%s_%016llx.spv", cacheDir, isPixel ? "ps" : "vs", (unsigned long long)srcHash);
    if (FILE* f = fopen(cacheFile, "rb"))
    {
        fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
        std::vector<uint32_t> spv(n > 0 ? (size_t)n / 4 : 0);
        const bool ok = n > 0 && fread(spv.data(), 4, spv.size(), f) == spv.size();
        fclose(f);
        if (ok && (e.module = MakeModule(spv))) { g_shaderOk++; g_shaderCacheHits++; return &e; }
    }
    CreateDirectoryA(cacheDir, nullptr);
    g_ev.shaderCompiles++;
    auto job = std::make_shared<CompileJob>();
    job->hlsl = std::move(hlsl); job->profile = isPixel ? "ps_6_0" : "vs_6_0"; job->tag = tag; job->cachePath = cacheFile;
    e.job = job;
    QueueCompile(job);
    if (g_log) Log("[vk] compiling %s (%zu dwords, %zu textures, %zu vfetch) in the background", tag, sc.code.size(), e.info.textureSlots.size(), e.info.vfetchSlots.size());
    if (syncShaders) return GetShader(sc, isPixel);
    g_shaderPending++;
    return nullptr;
}

// ---- Pipelines -------------------------------------------------------------------------------------------------------------------------------
VkDescriptorSetLayout g_set0Layout = VK_NULL_HANDLE, g_set1Layout = VK_NULL_HANDLE, g_utilSetLayout = VK_NULL_HANDLE;
VkPipelineLayout g_drawLayout = VK_NULL_HANDLE, g_utilLayout = VK_NULL_HANDLE;
VkDescriptorPool g_descPool = VK_NULL_HANDLE;
VkDescriptorSet g_set1 = VK_NULL_HANDLE;
VkPipelineCache g_pipeCache = VK_NULL_HANDLE;
VkShaderModule g_rectGS = VK_NULL_HANDLE;
size_t g_pipeCacheSavedSize = 0;
uint64_t g_pipelinesCreated = 0, g_profPipeline = 0;

struct PipeKey
{
    uint64_t vs, ps;
    uint32_t blend[4];
    uint32_t colorMask, depthCtl;
    VkFormat color[4];
    VkFormat depth;
    uint8_t numColor, topo, rect, cullFront, cullBack, frontCCW, depthClamp, pad;
    uint32_t pad2;
};
static_assert(sizeof(PipeKey) % 8 == 0, "PipeKey has no tail padding");
struct PipeHash { size_t operator()(const PipeKey& k) const { return (size_t)Hash64(&k, sizeof k); } };
struct PipeEq { bool operator()(const PipeKey& a, const PipeKey& b) const { return memcmp(&a, &b, sizeof a) == 0; } };
std::unordered_map<PipeKey, VkPipeline, PipeHash, PipeEq> g_pipelines;

VkBlendFactor BlendFactorOf(uint32_t f)
{
    switch (f)
    {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp BlendOpOf(uint32_t o)
{
    switch (o)
    {
    case 0: return VK_BLEND_OP_ADD;
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_REVERSE_SUBTRACT;
    }
}

VkPipeline CreateGraphicsPipeline(const VkPipelineShaderStageCreateInfo* stages, uint32_t nStages, VkPrimitiveTopology topo, const VkPipelineRasterizationStateCreateInfo& rs,
                                  const VkPipelineDepthStencilStateCreateInfo& ds, const VkPipelineColorBlendAttachmentState* blends, uint32_t numColor, VkPipelineLayout layout, VkRenderPass rp, bool util = false)
{
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = topo;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = numColor; cb.pAttachments = blends;
    const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                   VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_DEPTH_BIAS };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = util ? 2 : sizeof dyn / sizeof dyn[0]; dy.pDynamicStates = dyn;   // utility passes: viewport + scissor only
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount = nStages; ci.pStages = stages; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp; ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms; ci.pDepthStencilState = &ds; ci.pColorBlendState = &cb; ci.pDynamicState = &dy; ci.layout = layout; ci.renderPass = rp; ci.subpass = 0;
    VkPipeline p = VK_NULL_HANDLE;
    const uint64_t t0 = QpcNow();
    VkOk(vkCreateGraphicsPipelines(g_dev, g_pipeCache, 1, &ci, nullptr, &p), "vkCreateGraphicsPipelines");
    g_profPipeline += QpcNow() - t0;
    g_pipelinesCreated++;
    return p;
}

VkPipeline GetDrawPipeline(const PipeKey& k, const ShaderEntry& vs, const ShaderEntry& ps)
{
    auto it = g_pipelines.find(k);
    if (it != g_pipelines.end()) return it->second;
    VkPipelineShaderStageCreateInfo st[3] = {};
    uint32_t n = 0;
    st[n] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }; st[n].stage = VK_SHADER_STAGE_VERTEX_BIT; st[n].module = vs.module; st[n].pName = "main"; n++;
    if (k.rect && g_rectGS) { st[n] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }; st[n].stage = VK_SHADER_STAGE_GEOMETRY_BIT; st[n].module = g_rectGS; st[n].pName = "main"; n++; }
    st[n] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }; st[n].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[n].module = ps.module; st[n].pName = "main"; n++;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.depthClampEnable = k.depthClamp && g_haveDepthClamp; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f; rs.depthBiasEnable = VK_TRUE;
    rs.cullMode = (g_noCull || (!k.cullFront && !k.cullBack)) ? VK_CULL_MODE_NONE : k.cullBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT;
    rs.frontFace = k.frontCCW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    const uint32_t dc = k.depthCtl;
    if (k.depth)
    {
        ds.depthTestEnable = (dc >> 1) & 1; ds.depthWriteEnable = ((dc >> 1) & 1) && ((dc >> 2) & 1); ds.depthCompareOp = (VkCompareOp)((dc >> 4) & 7);
        ds.stencilTestEnable = dc & 1;
        ds.front.compareOp = (VkCompareOp)((dc >> 8) & 7); ds.front.failOp = (VkStencilOp)((dc >> 11) & 7); ds.front.passOp = (VkStencilOp)((dc >> 14) & 7); ds.front.depthFailOp = (VkStencilOp)((dc >> 17) & 7);
        if ((dc >> 7) & 1) { ds.back.compareOp = (VkCompareOp)((dc >> 20) & 7); ds.back.failOp = (VkStencilOp)((dc >> 23) & 7); ds.back.passOp = (VkStencilOp)((dc >> 26) & 7); ds.back.depthFailOp = (VkStencilOp)((dc >> 29) & 7); }
        else ds.back = ds.front;
    }
    VkPipelineColorBlendAttachmentState bl[4] = {};
    for (uint32_t i = 0; i < k.numColor; i++)
    {
        const uint32_t b = k.blend[i];
        const uint32_t sc = b & 31, oc = (b >> 5) & 7, dcf = (b >> 8) & 31, sa = (b >> 16) & 31, oa = (b >> 21) & 7, da = (b >> 24) & 31;
        const bool passthrough = sc == 1 && dcf == 0 && oc == 0 && sa == 1 && da == 0 && oa == 0;
        bl[i].blendEnable = !passthrough;
        bl[i].srcColorBlendFactor = BlendFactorOf(sc); bl[i].dstColorBlendFactor = BlendFactorOf(dcf); bl[i].colorBlendOp = BlendOpOf(oc);
        bl[i].srcAlphaBlendFactor = BlendFactorOf(sa); bl[i].dstAlphaBlendFactor = BlendFactorOf(da); bl[i].alphaBlendOp = BlendOpOf(oa);
        bl[i].colorWriteMask = (k.colorMask >> (i * 4)) & 0xF;
    }
    RpKey rk = {};
    memcpy(rk.color, k.color, sizeof rk.color); rk.depth = k.depth; rk.numColor = k.numColor;
    const VkPipeline p = CreateGraphicsPipeline(st, n, (VkPrimitiveTopology)k.topo, rs, ds, bl, k.numColor, g_drawLayout, GetRenderPass(rk));
    if (g_log || g_pipelinesCreated <= 3) Log("[vk] pipeline #%llu: vs %s ps %s rect=%u colors=%u depth=%d", (unsigned long long)g_pipelinesCreated, vs.name.c_str(), ps.name.c_str(), k.rect, k.numColor, (int)(k.depth != 0));
    g_pipelines[k] = p;
    return p;
}
void SavePipelineCache()
{
    size_t n = 0;
    if (!g_pipeCache || vkGetPipelineCacheData(g_dev, g_pipeCache, &n, nullptr) != VK_SUCCESS || n == g_pipeCacheSavedSize) return;
    std::vector<uint8_t> data(n);
    if (vkGetPipelineCacheData(g_dev, g_pipeCache, &n, data.data()) != VK_SUCCESS) return;
    if (FILE* f = fopen("vk_pipeline_cache.bin.tmp", "wb"))
    {
        const bool ok = fwrite(data.data(), 1, n, f) == n;
        fclose(f);
        if (ok) MoveFileExA("vk_pipeline_cache.bin.tmp", "vk_pipeline_cache.bin", MOVEFILE_REPLACE_EXISTING);
    }
    g_pipeCacheSavedSize = n;
}

// ---- Draw-state cache (what the current command buffer has bound) ----------------------------------------------------------------------------
VkPipeline g_boundPipe = VK_NULL_HANDLE;
bool g_set1Bound = false, g_pushValid = false, g_dynValid = false;
uint32_t g_boundCbV = 0, g_boundCbP = 0;
uint64_t g_cbUploadsV = 0, g_cbUploadsP = 0;
uint64_t g_pushed[kVkSet0Bindings][3];   // last pushed descriptors (binding -> handle, sampler/offset, range)
uint32_t g_pushedMask = 0;
VkViewport g_boundVp = {}; VkRect2D g_boundSc = {}; float g_boundBf[4] = {}; uint32_t g_boundStencil = 0xFFFFFFFFu;
void InvalidateDrawState() { g_boundPipe = VK_NULL_HANDLE; g_set1Bound = false; g_pushValid = false; g_dynValid = false; }
void OnNewCmd() { InvalidateDrawState(); }

// ---- Readback (GPU -> CPU, waits for the GPU) ---------------------------------------------------------------------------------------------------
Buf g_readback;
// Copies a rectangle of an image into host memory; returns tightly packed rows (w * bpp bytes), valid until the next readback.
const uint8_t* ReadbackImage(Img& im, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t bpp, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
{
    const VkDeviceSize need = (VkDeviceSize)w * h * bpp;
    if (g_readback.size < need)
    {
        RetireBuf(g_readback);
        if (!CreateBuf(g_readback, std::max<VkDeviceSize>(need, 16ull << 20), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true)) return nullptr;
    }
    EndPass();
    VkCommandBuffer cb = Cmd();
    ImgBarrier(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r = {};
    r.imageSubresource = { aspect, 0, 0, 1 }; r.imageOffset = { (int32_t)x, (int32_t)y, 0 }; r.imageExtent = { w, h, 1 };
    vkCmdCopyImageToBuffer(cb, im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_readback.buf, 1, &r);
    im.lastUse = g_cmdSerial;
    FlushAndWait();
    return g_readback.ptr;
}

// ---- GPU-side resolve targets (see the Direct3D 11 backend for the model) ------------------------------------------------------------------------
struct Resolved
{
    std::unique_ptr<Img> im;
    uint32_t w = 0, h = 0, fmt = 0, pitch = 0, height = 0, endian = 0, comps = 4;
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    size_t bytes = 0;
    uint64_t memSig = 0;
    uint32_t sigFrame = 0xFFFFFFFFu;
    bool isDepth = false;
    bool velocity = false;
};
std::map<uint32_t, Resolved> g_resolved;
uint32_t g_resolvedGen = 1, g_texGen = 1;
void EraseResolved(uint32_t base)
{
    auto it = g_resolved.find(base);
    if (it == g_resolved.end()) return;
    RetireImg(it->second.im);
    g_resolved.erase(it);
    g_resolvedGen++;
}

const char* g_flushReason = "?";
std::map<std::string, std::pair<uint64_t, uint64_t>> g_flushByReason;
// Writes the resolved texture back into guest memory (tiled, endian-swapped) and forgets it.
void FlushResolved(uint32_t base)
{
    auto it = g_resolved.find(base);
    if (it == g_resolved.end()) return;
    ProfScope profFlush(g_profFlush);
    Resolved& r = it->second;
    if (r.isDepth) { EraseResolved(base); return; }
    if (r.velocity) NoteVelocityUse("flush to guest memory");
    g_flushCount++; g_flushBytes += r.bytes; g_ev.flushes++;
    { auto& e = g_flushByReason[g_flushReason]; e.first++; e.second += r.bytes; }
    { static int logged = 0; if (g_log && logged++ < 60) Log("[vk] flush (%s) E[%08X] %ux%u fmt=%u bytes=%zu frame %u", g_flushReason, base, r.w, r.h, r.fmt, r.bytes, g_frame); }
    uint32_t bpp, comps;
    ResolvedFormat(r.fmt, bpp, comps);
    const uint8_t* px = ReadbackImage(*r.im, 0, 0, r.w, r.h, bpp);
    if (px)
    {
        uint32_t log2bpp = 0; while ((1u << log2bpp) < bpp) log2bpp++;
        const uint32_t alignedPitch = (r.pitch + 31) & ~31u;
        uint8_t* dest = PhysPtrW(base);
        const size_t writable = CommittedBytes(dest, r.bytes);
        for (uint32_t y = 0; y < r.h; y++)
        {
            const uint8_t* row = px + (size_t)y * r.w * bpp;
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
        NoteMemWrite(base, r.bytes);
    }
    EraseResolved(base);
}
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

// ---- Textures -----------------------------------------------------------------------------------------------------------------------------------
struct CachedTex
{
    std::unique_ptr<Img> im;
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
    uint8_t stable = 0;
    bool mlValid = false;
    MipLayout ml;
};
std::unordered_map<uint64_t, CachedTex> g_textures;
size_t g_texBytes = 0;
std::unique_ptr<Img> g_dummy2D, g_dummy3D, g_dummyCube;
const VkImageUsageFlags kTexUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

std::unique_ptr<Img> MakeDummy(VkImageViewType type)
{
    ImgDesc d; d.fmt = VK_FORMAT_R8G8B8A8_UNORM; d.usage = kTexUsage; d.viewType = type;
    d.layers = type == VK_IMAGE_VIEW_TYPE_CUBE ? 6 : 1; d.is3D = type == VK_IMAGE_VIEW_TYPE_3D;
    auto im = CreateImg(d);
    if (!im) return nullptr;
    static const uint32_t gray = 0xFF808080u;
    Subres s[6];
    for (uint32_t i = 0; i < d.layers; i++) s[i] = { (const uint8_t*)&gray, 4, 0, i, 1, 1 };
    UploadToImage(*im, s, d.layers);
    return im;
}

// Creates (or reuses, when the size/format/levels match) the image of a cache entry and uploads the decoded levels / faces.
bool StoreTexture(CachedTex& ct, const Decoded* const* levels, uint32_t nLevels, uint32_t layers, VkImageViewType viewType)
{
    const Decoded& d0 = *levels[0];
    const VkFormat vf = VkFormatOf(d0.fmt);
    if (!vf) return false;
    const bool reuse = ct.im && ct.w == d0.w && ct.h == d0.h && ct.fmt == d0.fmt && ct.levels == nLevels && ct.im->layers == layers;
    if (!reuse)
    {
        ImgDesc d; d.fmt = vf; d.w = d0.w; d.h = d0.h; d.levels = nLevels; d.layers = layers; d.usage = kTexUsage; d.viewType = viewType;
        auto im = CreateImg(d);
        if (!im) return false;
        RetireImg(ct.im);
        ct.im = std::move(im);
        g_ev.texCreated++;
        g_texGen++;
    }
    else g_ev.texUpdated++;
    std::vector<Subres> subs;
    size_t total = 0;
    for (uint32_t l = 0; l < layers; l++)
        for (uint32_t L = 0; L < nLevels; L++)
        {
            const Decoded& dl = *levels[layers > 1 ? l : L];
            subs.push_back({ dl.data.data(), dl.data.size(), layers > 1 ? 0u : L, l, dl.w, dl.h });
            total += dl.data.size();
            if (layers > 1) break;
        }
    { ProfScope pu(g_profUpload); UploadToImage(*ct.im, subs.data(), (uint32_t)subs.size()); }
    g_texBytes += total; g_texBytes -= std::min(g_texBytes, ct.bytes);
    ct.w = d0.w; ct.h = d0.h; ct.fmt = d0.fmt; ct.levels = nLevels; ct.bytes = total; ct.comps = d0.comps;
    return true;
}

Img* GetCubeImg(uint32_t slot, const TexFetch& tf)
{
    const size_t faceBytes = TextureFootprint(tf);
    const uint32_t faceStride = (uint32_t)((faceBytes + 4095) & ~(size_t)4095);
    const uint64_t key = Hash64(tf.d, sizeof tf.d) ^ 0x7C0BE5;
    CachedTex& ct = g_textures[key];
    const size_t footprint = (size_t)faceStride * 6;
    if (ct.im && ct.checkedFrame == g_frame && !WrittenSince(ct.checkedSeq, ct.base, footprint)) { g_slotComps[slot & 31] = ct.comps; return ct.im.get(); }
    Decoded faces[6];
    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint32_t f = 0; f < 6; f++)
    {
        TexFetch tf2 = tf;
        tf2.d[1] = (tf.d[1] & 0xFFFu) | ((tf.base() + f * faceStride) & 0xFFFFF000u);
        tf2.d[5] = (tf.d[5] & ~(3u << 9)) | (1u << 9);
        uint64_t h = 0;
        { ProfScope ps(g_profDecode); DecodeTexture(tf2, faces[f], false, &h); }
        hash = (hash ^ h) * 0x100000001B3ull;
        if (faces[f].data.empty() || faces[f].fmt == DXGI_FORMAT_UNKNOWN || faces[f].w != faces[0].w || faces[f].h != faces[0].h) return nullptr;
    }
    ct.checkedFrame = g_frame; ct.checkedSeq = g_memSeq; ct.base = tf.base(); ct.footprint = footprint;
    if (ct.im && hash == ct.dataHash) { g_slotComps[slot & 31] = ct.comps; return ct.im.get(); }
    g_decodeCount++; g_decodeBytes += faces[0].data.size() * 6;
    const Decoded* lv[6] = { &faces[0], &faces[1], &faces[2], &faces[3], &faces[4], &faces[5] };
    if (!StoreTexture(ct, lv, 1, 6, VK_IMAGE_VIEW_TYPE_CUBE)) return nullptr;
    ct.dataHash = hash;
    g_slotComps[slot & 31] = ct.comps;
    if (g_log) Log("[vk] cube texture slot %u %ux%u fmt=%u base=%08X face stride %X", slot, ct.w, ct.h, tf.format(), tf.base(), faceStride);
    return ct.im.get();
}

struct TexMemo { uint32_t d[6]; uint32_t dim; uint32_t frame; uint64_t seq; uint32_t resolvedGen, texGen, comps; Img* im; };
TexMemo g_texMemo[32];
Img* GetTextureImgSlow(uint32_t slot, uint32_t shaderDim, const TexFetch& tf);
// Returns the image for a texture fetch constant (dimension as declared by the shader: 0 = 1D, 1 = 2D, 2 = 3D, 3 = cube).
Img* GetTextureImg(uint32_t slot, uint32_t shaderDim)
{
    const TexFetch tf = GetTexFetch(slot);
    TexMemo& m = g_texMemo[slot & 31];
    if (!g_noTexMemo && m.im && m.frame == g_frame && m.seq == g_memSeq && m.dim == shaderDim && m.resolvedGen == g_resolvedGen && m.texGen == g_texGen && memcmp(m.d, tf.d, sizeof m.d) == 0)
    {
        g_slotComps[slot & 31] = m.comps;
        return m.im;
    }
    g_ev.texMemoMiss++;
    Img* r = GetTextureImgSlow(slot, shaderDim, tf);
    const uint32_t gen = g_resolvedGen;
    memcpy(m.d, tf.d, sizeof m.d); m.dim = shaderDim; m.frame = g_frame; m.seq = g_memSeq; m.resolvedGen = gen; m.texGen = g_texGen; m.comps = g_slotComps[slot & 31]; m.im = r;
    return r;
}
Img* GetTextureImgSlow(uint32_t slot, uint32_t shaderDim, const TexFetch& tf)
{
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
            const bool sigOk = !noSample && (r.sigFrame == g_frame || (MemSig(base, r.bytes) == r.memSig && (r.sigFrame = g_frame, true)));
            if (fmtOk && fw == r.w && fh == r.h && tf.tiled() && sigOk)
            {
                g_slotComps[slot & 31] = r.comps;
                if (r.velocity) NoteVelocityUse("texture fetch");
                return r.im.get();
            }
            g_flushReason = "fetch-mismatch"; FlushResolved(base);
        }
        if (!g_resolved.empty()) { g_flushReason = "fetch-overlap"; FlushOverlapping(base, TextureFootprint(tf), 0xFFFFFFFF); }
    }
    if (shaderDim == 2) return g_dummy3D.get();
    if (shaderDim == 3)
    {
        if (tf.type() == 2 && tf.dimension() == 3) if (Img* cube = GetCubeImg(slot, tf)) return cube;
        return g_dummyCube.get();
    }
    if (tf.type() != 2) return g_dummy2D.get();
    const uint64_t key = Hash64(tf.d, sizeof tf.d) ^ 0x1234;
    CachedTex& ct = g_textures[key];
    if (!ct.mlValid) { ct.ml = ComputeMips(tf); ct.mlValid = true; }
    const MipLayout& ml = ct.ml;
    auto footprintOf = [&](size_t baseBytes) -> size_t
    {
        if (ml.levels <= 1) return baseBytes;
        const uint32_t b = tf.base();
        if (ml.mipAddr >= b) return std::max<size_t>(baseBytes, (size_t)(ml.mipAddr - b) + ml.regionBytes);
        return baseBytes;
    };
    if (ct.im && ct.checkedFrame == g_frame && !WrittenSince(ct.checkedSeq, ct.base, ct.footprint)) { g_slotComps[slot & 31] = ct.comps; return ct.im.get(); }
    uint64_t hash = 0;
    if (ct.im)
    {
        static uint32_t budgetFrame = 0xFFFFFFFFu, rehashed = 0;
        if (budgetFrame != g_frame) { budgetFrame = g_frame; rehashed = 0; }
        const bool untouched = !WrittenSince(ct.checkedSeq, ct.base, ct.footprint);
        if (ct.stable >= kStableAfter && ct.levels == ml.levels && untouched && (((g_frame + (uint32_t)(key & 31)) & (kRecheckPeriod - 1)) != 0 || rehashed >= kRehashBudget))
        {
            ct.checkedFrame = g_frame;
            g_slotComps[slot & 31] = ct.comps;
            return ct.im.get();
        }
        if (untouched && ct.stable >= kStableAfter) rehashed++;
        Decoded unused;
        g_ev.texHashed++;
        { ProfScope ps(g_profHash); DecodeTexture(tf, unused, true, &hash); if (ml.levels > 1) hash = (hash ^ HashMipRegion(ml)) * 0x100000001B3ull; }
        ct.checkedFrame = g_frame; ct.checkedSeq = g_memSeq;
        if (hash == ct.dataHash && ct.levels == ml.levels) { if (ct.stable < 255) ct.stable++; g_slotComps[slot & 31] = ct.comps; return ct.im.get(); }
        ct.stable = 0;
    }
    static Decoded dec;
    const uint64_t tDec0 = QpcNow();
    { ProfScope ps(g_profDecode); DecodeTexture(tf, dec, false, &hash); }
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
            if (mipDecs[L].fmt != dec.fmt || mipDecs[L].data.empty()) { mipDecs.clear(); break; }
        }
        if (!mipDecs.empty()) hash = (hash ^ HashMipRegion(ml)) * 0x100000001B3ull;
    }
    const uint32_t nLevels = mipDecs.empty() ? 1 : ml.levels;
    g_decodeCount++; g_decodeBytes += dec.data.size();
    g_ev.texBytes += dec.data.size();
    {
        static LARGE_INTEGER qf = {}; if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
        const double ms = (double)(QpcNow() - tDec0) * 1000.0 / (double)qf.QuadPart;
        static int logged = 0;
        if (ms > 3.0 && logged++ < 60) Log("[slowtex] frame %u slot %u fmt=%u %ux%u tiled=%u endian=%u mips=%u base=%08X: decode %.1f ms -> %zu KB", g_frame, slot, tf.format(), dec.w, dec.h, tf.tiled(), tf.endian(), nLevels, tf.base(), ms, dec.data.size() / 1024);
    }
    std::vector<const Decoded*> lv(nLevels);
    lv[0] = &dec;
    for (uint32_t L = 1; L < nLevels; L++) lv[L] = &mipDecs[L];
    if (!StoreTexture(ct, lv.data(), nLevels, 1, VK_IMAGE_VIEW_TYPE_2D))
    {
        static int logged = 0;
        if (logged++ < 20) Log("[vk] texture create failed %ux%u fmt %u mips %u", dec.w, dec.h, (unsigned)dec.fmt, nLevels);
        return g_dummy2D.get();
    }
    ct.base = tf.base(); ct.footprint = footprintOf(TextureFootprint(tf)); ct.dataHash = hash; ct.checkedFrame = g_frame; ct.checkedSeq = g_memSeq;
    g_slotComps[slot & 31] = ct.comps;
    if (g_texBytes > (1ull << 30))
    {
        Log("[vk] texture cache cleared");
        for (auto& kv : g_textures) RetireImg(kv.second.im);
        g_textures.clear(); g_texBytes = 0; g_texGen++;
        return g_dummy2D.get();
    }
    return ct.im.get();
}

// ---- Render targets ------------------------------------------------------------------------------------------------------------------------------
struct ColorRT
{
    std::unique_ptr<Img> im;
    uint32_t w = 0, h = 0, fmt = 0;
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
};
struct DepthRT
{
    std::unique_ptr<Img> im;
    uint32_t w = 0, h = 0;
};
std::map<uint32_t, ColorRT> g_colorRTs;   // key = edram base | format << 16
std::map<uint32_t, DepthRT> g_depthRTs;   // key = edram base

void ClearColorImg(Img& im, const float col[4])
{
    EndPass();
    VkCommandBuffer cb = Cmd();
    ImgBarrier(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);   // (also orders it after earlier transfers to the image)
    im.lastUse = g_cmdSerial;
    VkClearColorValue v; memcpy(v.float32, col, 16);
    const VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
}
void ClearDepthImg(Img& im, float z, uint8_t stencil)
{
    EndPass();
    VkCommandBuffer cb = Cmd();
    ImgBarrier(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    im.lastUse = g_cmdSerial;
    VkClearDepthStencilValue v = { z, stencil };
    const VkImageSubresourceRange r = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
    vkCmdClearDepthStencilImage(cb, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
}

ColorRT* GetColorRT(uint32_t base, uint32_t fmt, uint32_t w, uint32_t h, bool create)
{
    if (fmt == 12) fmt = 3;   // k_2_10_10_10_FLOAT_AS_16_16_16_16 is the same surface as k_2_10_10_10_FLOAT (see the Direct3D 11 backend)
    const uint32_t key = base | (fmt << 16);
    auto it = g_colorRTs.find(key);
    if (it == g_colorRTs.end() && !create) return nullptr;
    ColorRT& rt = g_colorRTs[key];
    if (rt.im && rt.w >= w && rt.h >= h) return &rt;
    const uint32_t nw = std::max(w, rt.w), nh = std::max(h, rt.h);
    const DXGI_FORMAT dxgi = ColorRtFormat(fmt);
    ImgDesc d;
    const bool rgba8 = dxgi == DXGI_FORMAT_R8G8B8A8_UNORM || dxgi == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    d.fmt = rgba8 ? VK_FORMAT_R8G8B8A8_UNORM : VkFormatOf(dxgi);
    d.rtFmt = dxgi == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? VK_FORMAT_R8G8B8A8_SRGB : d.fmt;
    d.w = nw; d.h = nh;
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    g_ev.rtCreated++;
    auto im = CreateImg(d);
    if (!im) { Log("[vk] RT create failed %ux%u fmt %u", nw, nh, fmt); return nullptr; }
    const float zero[4] = { 0, 0, 0, 0 };
    ClearColorImg(*im, zero);
    if (rt.im)   // keep the old content when growing
    {
        VkCommandBuffer cb = Cmd();
        ImgBarrier(cb, *rt.im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        ImgBarrier(cb, *im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy c = {};
        c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        c.extent = { rt.w, rt.h, 1 };
        vkCmdCopyImage(cb, rt.im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        RetireImg(rt.im);
    }
    rt.im = std::move(im); rt.w = nw; rt.h = nh; rt.fmt = fmt; rt.dxgi = dxgi;
    if (g_log) Log("[vk] color RT base=%u fmt=%u %ux%u", base, fmt, nw, nh);
    return &rt;
}
DepthRT* GetDepthRT(uint32_t base, uint32_t w, uint32_t h)
{
    DepthRT& rt = g_depthRTs[base];
    if (rt.im && rt.w >= w && rt.h >= h) return &rt;
    const uint32_t nw = std::max(w, rt.w), nh = std::max(h, rt.h);
    ImgDesc d;
    d.fmt = g_depthFmt; d.w = nw; d.h = nh; d.depth = true;
    d.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    auto im = CreateImg(d);
    if (!im) { Log("[vk] depth RT create failed %ux%u", nw, nh); return nullptr; }
    ClearDepthImg(*im, 1.0f, 0);
    if (rt.im) { EndPass(); RetireImg(rt.im); }
    rt.im = std::move(im); rt.w = nw; rt.h = nh;
    return &rt;
}

struct PendingClear
{
    bool color = false, depth = false;
    ColorRT* rt = nullptr; float col[4] = {};
    uint32_t dbase = 0; float dz = 1.0f; uint8_t dstencil = 0;
} g_pendClear;
void ApplyPendingClears()
{
    if (g_pendClear.color && g_pendClear.rt && g_pendClear.rt->im) ClearColorImg(*g_pendClear.rt->im, g_pendClear.col);
    if (g_pendClear.depth)
    {
        auto it = g_depthRTs.find(g_pendClear.dbase);
        if (it != g_depthRTs.end() && it->second.im) ClearDepthImg(*it->second.im, g_pendClear.dz, g_pendClear.dstencil);
    }
    g_pendClear.color = g_pendClear.depth = false;
}

// ---- Vertex buffers ---------------------------------------------------------------------------------------------------------------------------------
// Same policy as the Direct3D 11 backend: dynamic data (new or changing every frame) is copied into the per-frame vertex ring, data that stays gets a
// persistent buffer whose 4 KB pages are compared with guest memory (static pages only every kRecheckPeriod frames) and re-uploaded when they change.
struct VBEntry
{
    Buf buf;
    std::vector<uint8_t> shadow;
    std::vector<uint32_t> pageStamp;
    std::vector<uint8_t> pageStable;
    uint32_t lastFrame = 0, frames = 0, hot = 0;
    uint32_t changeStreak = 0, lastChangeFrame = 0xFFFFFFFFu;
};
std::unordered_map<uint64_t, VBEntry> g_vbs;
size_t g_vbBytes = 0;
const uint32_t kVbPromoteAfter = 3, kVbCreateBudget = 12, kVbRingMaxBuffer = 64u << 10;
Buf g_nullBuf;   // bound for vertex fetch slots without a buffer (reads return 0)
VkDeviceSize g_ssboAlign = 64;

bool VbRingCopy(const uint8_t* src, uint32_t avail, VkDescriptorBufferInfo& out)
{
    if (avail > kVbRingMaxBuffer) return false;
    const VkDeviceSize off = RingAlloc(g_vbRing, avail, g_ssboAlign);
    if (off == ~(VkDeviceSize)0) return false;
    uint8_t* dst = g_vbRing.buf.ptr + off;
    const size_t have = CommittedBytes(src, avail);
    memcpy(dst, src, have);
    if (have < avail) memset(dst + have, 0, avail - have);
    out = { g_vbRing.buf.buf, off, avail };
    g_ev.vbRingCopies++; g_ev.vbRingBytes += avail;
    return true;
}
bool GetVertexBuffer(uint32_t physBase, uint32_t sizeBytes, uint32_t rangeStart, uint32_t rangeEnd, VkDescriptorBufferInfo& out)
{
    if (sizeBytes == 0) return false;
    sizeBytes = std::min<uint32_t>(sizeBytes, 128u << 20);
    const uint8_t* src = PhysPtr(physBase);
    const uint64_t key = ((uint64_t)physBase << 32) | sizeBytes;
    VBEntry& e = g_vbs[key];
    if (e.lastFrame != g_frame || e.frames == 0) { e.frames++; if (e.hot) e.hot--; }
    e.lastFrame = g_frame;
    const uint32_t avail = (sizeBytes + 15) & ~15u;
    const uint32_t promoteAfter = avail > 16384 ? kVbPromoteAfter : 30;
    static uint32_t budgetFrame = 0xFFFFFFFFu, created = 0;
    if (budgetFrame != g_frame) { budgetFrame = g_frame; created = 0; }
    const bool budgetLeft = created < kVbCreateBudget;
    if (!g_noVbRing && ((!e.buf.buf && (e.frames < promoteAfter || !budgetLeft)) || (e.buf.buf && e.hot)))
        if (VbRingCopy(src, avail, out)) return true;
    if (!e.buf.buf)
    {
        g_ev.vbCreated++; g_ev.vbBytesUp += avail; created++;
        if (!CreateBuf(e.buf, avail, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false)) { g_vbs.erase(key); return false; }
        e.shadow.assign(avail, 0);
        memcpy(e.shadow.data(), src, CommittedBytes(src, avail));
        UploadToBuffer(e.buf, 0, e.shadow.data(), avail);
        e.pageStamp.assign((avail + 4095) / 4096, g_frame);
        e.pageStable.assign((avail + 4095) / 4096, 0);
        g_vbBytes += avail;
        e.buf.lastUse = g_cmdSerial;
        out = { e.buf.buf, 0, avail };
        return true;
    }
    rangeStart = std::min(rangeStart, avail); rangeEnd = std::min(std::max(rangeEnd, rangeStart), avail);
    const bool small = avail <= 32768;
    uint32_t run0 = 0, run1 = 0;
    auto flushRun = [&]() { if (run1 > run0) { memcpy(e.shadow.data() + run0, src + run0, run1 - run0); UploadToBuffer(e.buf, run0, src + run0, run1 - run0); run0 = run1 = 0; } };
    for (uint32_t pg = rangeStart / 4096, pgEnd = (rangeEnd + 4095) / 4096; pg < pgEnd && pg < e.pageStamp.size(); pg++)
    {
        if (!small && e.pageStamp[pg] == g_frame) { flushRun(); continue; }
        if (!small && !g_forceVbCheck && e.pageStable[pg] >= kStableAfter && ((g_frame + pg * 5) & (kRecheckPeriod - 1)) != 0) { flushRun(); continue; }
        e.pageStamp[pg] = g_frame;
        const uint32_t o0 = pg * 4096, o1 = std::min(o0 + 4096, avail);
        g_ev.vbPagesCmp++;
        if (memcmp(e.shadow.data() + o0, src + o0, std::min<size_t>(o1 - o0, CommittedBytes(src + o0, o1 - o0))) != 0)
        {
            g_ev.vbPagesChanged++; g_ev.vbBytesUp += o1 - o0;
            if (run1 == o0) run1 = o1; else { flushRun(); run0 = o0; run1 = o1; }
            e.pageStable[pg] = 0;
            if (e.lastChangeFrame != g_frame)
            {
                e.changeStreak = (e.lastChangeFrame == g_frame - 1) ? e.changeStreak + 1 : 1;
                e.lastChangeFrame = g_frame;
                if (!g_noVbRing && e.changeStreak >= 3 && avail <= kVbRingMaxBuffer) e.hot = 180;
            }
        }
        else { flushRun(); if (e.pageStable[pg] < 255) e.pageStable[pg]++; }
    }
    flushRun();
    e.buf.lastUse = g_cmdSerial;
    out = { e.buf.buf, 0, avail };
    return true;
}

// ---- Converted index lists (one host-visible arena, cached by guest address + content hash; see the Direct3D 11 backend) --------------------------
Buf g_idxBuf;
const uint32_t kIdxRing = 1u << 23;
uint32_t g_idxPos = 0;
struct IdxEntry { uint64_t k0, k1, k2, content; uint32_t start, count, minIdx, maxIdx, topo; bool rectList; };
std::unordered_map<uint64_t, IdxEntry> g_idxCache;
uint64_t g_idxHits = 0, g_idxMisses = 0, g_idxResets = 0;
uint32_t AppendIndices(const uint32_t* data, uint32_t n)
{
    if (n == 0 || n > kIdxRing || !g_idxBuf.ptr) return ~0u;
    if (g_idxPos + n > kIdxRing)
    {
        // The arena is full: everything recorded so far must have executed before it is reused.
        g_idxResets++;
        FlushAndWait();
        g_idxPos = 0; g_idxCache.clear();
    }
    memcpy((uint32_t*)g_idxBuf.ptr + g_idxPos, data, (size_t)n * 4);
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
    if (!g_noIdxCache && it != g_idxCache.end() && it->second.k0 == k0 && it->second.k1 == k1 && it->second.k2 == k2 && it->second.content == content) { g_idxHits++; return &it->second; }
    g_idxMisses++; g_forceVbCheck = true;
    std::vector<uint32_t> idx; D3D_PRIMITIVE_TOPOLOGY topo; bool rectList; uint32_t minIdx, maxIdx;
    if (!BuildIndices(dp, idx, topo, rectList, minIdx, maxIdx)) return nullptr;
    const uint32_t start = AppendIndices(idx.data(), (uint32_t)idx.size());
    if (start == ~0u) return nullptr;
    IdxEntry& e = g_idxCache[key];
    e = { k0, k1, k2, content, start, (uint32_t)idx.size(), minIdx, maxIdx, (uint32_t)topo, rectList };
    return &e;
}
VkPrimitiveTopology TopologyOf(uint32_t d3dTopo)
{
    switch (d3dTopo)
    {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

bool EnsureInit();
void DoResolve();

// ---- Draw -----------------------------------------------------------------------------------------------------------------------------------------
void Draw(const XDrawParams& dp)
{
    if (!EnsureInit()) return;
    ProfScope profScope(g_profDraw);
    g_drawCount++;
    const uint32_t mode = XReg(xr::RB_MODECONTROL) & 7;
    if (mode == 6) { DoResolve(); return; }
    if (mode != 4 && mode != 5) return;
    if (g_skipDrawsFrom >= 0 && (int)g_frame >= g_skipDrawsFrom) return;
    if (g_pendClear.color || g_pendClear.depth) ApplyPendingClears();
    {
        // Draws that write no colour and touch no depth/stencil (visibility-query probes) have no visible effect.
        const uint32_t dc = XReg(xr::RB_DEPTHCONTROL);
        const bool depthWrite = ((dc >> 1) & 1) && ((dc >> 2) & 1);
        if (mode == 4 && (XReg(xr::RB_COLOR_MASK) & 0xFFFF) == 0 && !depthWrite && !(dc & 1)) return;
    }
    if (!g_curVS.valid || !g_curPS.valid) { g_skippedDraws++; return; }
    if (g_envEvery > 1 && IsEnvPassState() && EnvPassSkipped()) { g_envDrawsSkipped++; return; }
    if (mode == 4 && !g_velocityNeeded && g_curPS.hash == kVelocityPsHash && ((XReg(xr::RB_COLOR_INFO) >> 16) & 0xF) == 4)
    {
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

    g_forceVbCheck = false;
    const IdxEntry* ie = GetIndexList(dp);
    if (!ie) { g_skippedDraws++; return; }
    const IdxEntry idx = *ie;   // (copied: a later index-arena reset clears the cache)
    if (mode == 4 && idx.count >= 300) g_bigDraws++;
    g_dp[1] += QpcNow() - T0; T0 = QpcNow();

    // Render targets
    const Surface surf = CurrentSurface();
    Img* colors[4] = {};
    uint32_t numRTs = 0;
    const uint32_t colorMask = XReg(xr::RB_COLOR_MASK);
    const uint32_t colorRegs[4] = { xr::RB_COLOR_INFO, xr::RB_COLOR1_INFO, xr::RB_COLOR2_INFO, xr::RB_COLOR3_INFO };
    float rtW = (float)surf.w, rtH = (float)surf.h;
    uint32_t actualW = surf.w, actualH = surf.h;
    if (mode == 4)
    {
        for (uint32_t i = 0; i < 4; i++)
        {
            const uint32_t info = XReg(colorRegs[i]);
            if (i > 0 && (((colorMask >> (i * 4)) & 0xF) == 0 || !((ps->info.colorOutputsWritten >> i) & 1))) continue;
            ColorRT* rt = GetColorRT(info & 0xFFF, (info >> 16) & 0xF, surf.w, surf.h, true);
            if (!rt || !rt->im) continue;
            if (i > 0 && colors[0] && (rt->w != actualW || rt->h != actualH)) continue;
            bool dup = false; for (uint32_t j = 0; j < i; j++) dup |= colors[j] == rt->im.get();
            if (dup) continue;
            colors[i] = rt->im.get();
            numRTs = i + 1;
            if (i == 0) { actualW = rt->w; actualH = rt->h; }
        }
        rtW = (float)actualW; rtH = (float)actualH;
    }
    const uint32_t depthCtl = XReg(xr::RB_DEPTHCONTROL);
    Img* depthImg = nullptr;
    uint32_t fbW = actualW, fbH = actualH;
    if ((depthCtl & 3) || mode == 5)
    {
        DepthRT* dr = GetDepthRT(XReg(xr::RB_DEPTH_INFO) & 0xFFF, actualW, actualH);
        if (dr && dr->im) { depthImg = dr->im.get(); if (!colors[0]) { fbW = dr->w; fbH = dr->h; } }
    }
    if (!colors[0] && !depthImg) { g_skippedDraws++; return; }
    if (!colors[0]) numRTs = 0;
    g_dp[2] += QpcNow() - T0; T0 = QpcNow();

    // Resources and constants. Anything here may upload (and interrupt the render pass) or, when a per-frame ring is full, submit and wait; the ring
    // offsets taken before such a reset are void, so the whole step is repeated if one happened.
    VkDescriptorBufferInfo vbInfo[kVkVbMax];
    Img* texImg[2][16];
    VkSampler texSmp[2][16];
    uint32_t cbOffV = 0, cbOffP = 0;
    static XeCB cbv, cbp;
    static bool haveV = false, haveP = false; static uint64_t lastVsHash = 0; static float lastRtW = 0, lastRtH = 0; static uint32_t cbEpoch = 0, offV = 0, offP = 0;
    for (int attempt = 0; attempt < 3; attempt++)
    {
        const uint32_t epoch0 = g_cbEpoch;
        uint32_t vi = 0;
        for (uint32_t slot : vs->info.vfetchSlots)
        {
            const uint32_t reg = xr::SHADER_CONSTANT_FETCH_00_0 + (slot / 3) * 6 + (slot % 3) * 2;
            const uint32_t d0 = g_xregs[reg], d1 = g_xregs[reg + 1];
            VkDescriptorBufferInfo& bi = vbInfo[vi++];
            bi = { g_nullBuf.buf, 0, 16 };
            if ((d0 & 3) == 3)
            {
                const uint64_t tv = QpcNow();
                const uint32_t stride = vs->info.vfetchStride.count(slot) ? vs->info.vfetchStride.at(slot) : 0;
                const int64_t minOff = vs->info.vfetchMinOff.count(slot) ? vs->info.vfetchMinOff.at(slot) : 0, maxOff = vs->info.vfetchMaxOff.count(slot) ? vs->info.vfetchMaxOff.at(slot) : 4;
                int64_t r0 = ((int64_t)idx.minIdx * stride + minOff) * 4, r1 = ((int64_t)idx.maxIdx * stride + maxOff) * 4 + 16;
                if (r0 < 0) r0 = 0;
                if (r1 > 0x7FFFFFFF) r1 = 0x7FFFFFFF;
                if (!GetVertexBuffer(d0 & ~3u, ((d1 >> 2) & 0xFFFFFF) * 4, (uint32_t)r0, (uint32_t)r1, bi)) bi = { g_nullBuf.buf, 0, 16 };
                g_dp[4] += QpcNow() - tv;
            }
        }
        for (int stage = 0; stage < 2; stage++)
        {
            const ShaderEntry* se = stage ? ps : vs;
            uint32_t ti = 0;
            for (uint32_t slot : se->info.textureSlots)
            {
                const uint64_t tt = QpcNow();
                const uint32_t dim = se->info.dimOfSlot[slot];
                Img* im = GetTextureImg(slot, dim);
                if (!im) im = dim == 2 ? g_dummy3D.get() : dim == 3 ? g_dummyCube.get() : g_dummy2D.get();
                g_dp[5] += QpcNow() - tt;
                texImg[stage][ti] = im;
                texSmp[stage][ti] = GetSamplerFast(slot);
                ti++;
            }
        }
        // Sampled images must be in the shader-read layout before the pass (a resolve target was last written as an attachment).
        for (int stage = 0; stage < 2; stage++)
            for (uint32_t i = 0; i < (stage ? ps : vs)->info.textureSlots.size(); i++) UseImg(*texImg[stage][i], SampledLayout(*texImg[stage][i]));
        g_dp[6] += QpcNow() - T0; T0 = QpcNow();

        // Constants: rebuilt only when a register they depend on changed (XGpuWriteRegister keeps g_regDirty), the vertex shader / render target size changed,
        // or a texture's component count changed (PatchSwizzles); vertex fetch slots never carry a base offset here (the descriptor carries it).
        static const bool dbgRainbow = false;
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
                if (cbv.vfmeta[slot][0] != 0 || cbv.vfmeta[slot][1] != ((d1 >> 2) & 0xFFFFFF) || cbv.vfmeta[slot][2] != (d1 & 3)) return true;
            }
            return false;
        };
        const bool epochChanged = cbEpoch != g_cbEpoch;
        const bool refillV = epochChanged || g_noCbDedupe || !haveV || (g_regDirty & (kDirtyVsFloat | kDirtyBoolLoop | kDirtyMisc)) || lastVsHash != g_curVS.hash || lastRtW != rtW || lastRtH != rtH || vfMetaChanged() || texSwzChanged(cbv, vs);
        const bool refillP = epochChanged || g_noCbDedupe || !haveP || dbgRainbow || (g_regDirty & (kDirtyPsFloat | kDirtyBoolLoop | kDirtyMisc)) || texSwzChanged(cbp, ps);
        if (refillV) { FillConstants(false, vs->info, cbv, rtW, rtH); PatchSwizzles(cbv); }
        if (refillP) { FillConstants(true, ps->info, cbp, rtW, rtH); PatchSwizzles(cbp); }
        const VkDeviceSize cbAlign = std::max<VkDeviceSize>(64, g_props.limits.minUniformBufferOffsetAlignment);
        if (refillV)
        {
            const VkDeviceSize o = RingAlloc(g_cbRing, sizeof(XeCB), cbAlign);
            memcpy(g_cbRing.buf.ptr + o, &cbv, sizeof cbv);
            offV = (uint32_t)o; haveV = true; lastVsHash = g_curVS.hash; lastRtW = rtW; lastRtH = rtH; g_cbUploadsV++;
        }
        if (refillP)
        {
            const VkDeviceSize o = RingAlloc(g_cbRing, sizeof(XeCB), cbAlign);
            memcpy(g_cbRing.buf.ptr + o, &cbp, sizeof cbp);
            offP = (uint32_t)o; haveP = true; g_cbUploadsP++;
        }
        cbEpoch = g_cbEpoch;
        cbOffV = offV; cbOffP = offP;
        if (epoch0 == g_cbEpoch) break;
        haveV = haveP = false;   // a ring was reset during this step: start over
    }
    g_regDirty = 0;
    g_dp[7] += QpcNow() - T0; T0 = QpcNow();

    // Pipeline
    const uint32_t sc = XReg(xr::PA_SU_SC_MODE_CNTL), clipCntl = XReg(xr::PA_CL_CLIP_CNTL);
    PipeKey pk;
    memset(&pk, 0, sizeof pk);
    pk.vs = vs->key; pk.ps = ps->key;
    pk.blend[0] = XReg(xr::RB_BLENDCONTROL0); pk.blend[1] = XReg(xr::RB_BLENDCONTROL1); pk.blend[2] = XReg(xr::RB_BLENDCONTROL2); pk.blend[3] = XReg(xr::RB_BLENDCONTROL3);
    pk.colorMask = numRTs ? colorMask : 0;
    for (uint32_t i = 0; i < numRTs; i++) pk.color[i] = colors[i] ? colors[i]->rtFmt : VK_FORMAT_UNDEFINED;
    for (uint32_t i = numRTs; i < 4; i++) pk.blend[i] = 0;
    pk.numColor = (uint8_t)numRTs;
    pk.depth = depthImg ? depthImg->fmt : VK_FORMAT_UNDEFINED;
    pk.depthCtl = depthImg ? depthCtl : 0;
    pk.topo = (uint8_t)TopologyOf(idx.topo);
    pk.rect = idx.rectList ? 1 : 0;
    if (idx.rectList) { pk.cullFront = 0; pk.cullBack = 0; pk.frontCCW = 1; }
    else { pk.cullFront = sc & 1; pk.cullBack = (sc >> 1) & 1; pk.frontCCW = ((sc >> 2) & 1) == 0; }
    pk.depthClamp = (clipCntl >> 16) & 1;
    VkPipeline pipe = GetDrawPipeline(pk, *vs, *ps);
    if (!pipe) { g_skippedDraws++; return; }
    g_dp[8] += QpcNow() - T0; T0 = QpcNow();

    // Record
    EnsurePass(colors, numRTs, depthImg, fbW, fbH);
    VkCommandBuffer cb = Cmd();
    if (g_boundPipe != pipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); g_boundPipe = pipe; }
    if (!g_set1Bound || g_boundCbV != cbOffV || g_boundCbP != cbOffP)
    {
        const uint32_t dyn[2] = { cbOffV, cbOffP };
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_drawLayout, 1, 1, &g_set1, 2, dyn);
        g_set1Bound = true; g_boundCbV = cbOffV; g_boundCbP = cbOffP;
    }
    {
        // Push descriptors: only when something differs from the last push.
        VkWriteDescriptorSet w[kVkSet0Bindings]; VkDescriptorImageInfo ii[kVkSet0Bindings]; VkDescriptorBufferInfo bi[kVkVbMax];
        uint64_t cur[kVkSet0Bindings][3];
        uint32_t nw = 0, mask = 0;
        auto addImg = [&](uint32_t binding, Img* im, VkSampler s)
        {
            ii[nw] = { s, im->view, SampledLayout(*im) };
            w[nw] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[nw].dstBinding = binding; w[nw].descriptorCount = 1; w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &ii[nw];
            cur[binding][0] = (uint64_t)(uintptr_t)im->view; cur[binding][1] = (uint64_t)(uintptr_t)s; cur[binding][2] = ii[nw].imageLayout;
            mask |= 1u << binding; nw++;
            im->lastUse = g_cmdSerial;
        };
        uint32_t ti = 0;
        for (size_t i = 0; i < vs->info.textureSlots.size(); i++) addImg(kVkVsTexBase + ti++, texImg[0][i], texSmp[0][i]);
        for (uint32_t i = 0; i < vs->info.vfetchSlots.size(); i++)
        {
            const uint32_t binding = kVkVbBase + i;
            bi[i] = vbInfo[i];
            w[nw] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[nw].dstBinding = binding; w[nw].descriptorCount = 1; w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[i];
            cur[binding][0] = (uint64_t)(uintptr_t)bi[i].buffer; cur[binding][1] = bi[i].offset; cur[binding][2] = bi[i].range;
            mask |= 1u << binding; nw++;
        }
        for (size_t i = 0; i < ps->info.textureSlots.size(); i++) addImg(kVkPsTexBase + (uint32_t)i, texImg[1][i], texSmp[1][i]);
        bool same = g_pushValid && mask == g_pushedMask;
        for (uint32_t b = 0; same && b < kVkSet0Bindings; b++) if ((mask >> b) & 1) same = memcmp(cur[b], g_pushed[b], sizeof cur[b]) == 0;
        if (!same && nw)
        {
            vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_drawLayout, 0, nw, w);
            for (uint32_t b = 0; b < kVkSet0Bindings; b++) if ((mask >> b) & 1) memcpy(g_pushed[b], cur[b], sizeof cur[b]);
            g_pushedMask = mask; g_pushValid = true;
        }
    }
    {
        // Dynamic state: viewport (Y flipped), scissor, blend constants, stencil reference / masks.
        const VkViewport vp = { 0.0f, rtH, rtW, -rtH, 0.0f, 1.0f };
        const uint32_t tl = XReg(xr::PA_SC_WINDOW_SCISSOR_TL), br = XReg(xr::PA_SC_WINDOW_SCISSOR_BR);
        int32_t l = tl & 0x3FFF, t = (tl >> 16) & 0x3FFF, r = br & 0x3FFF, b = (br >> 16) & 0x3FFF;
        const uint32_t stl = XReg(xr::PA_SC_SCREEN_SCISSOR_TL), sbr = XReg(xr::PA_SC_SCREEN_SCISSOR_BR);
        l = std::max<int32_t>(l, (int32_t)((int32_t)(stl << 17) >> 17)); t = std::max<int32_t>(t, (int32_t)(((int32_t)((stl >> 16) << 17)) >> 17));
        r = std::min<int32_t>(r, sbr & 0x7FFF); b = std::min<int32_t>(b, (sbr >> 16) & 0x7FFF);
        l = std::max(l, 0); t = std::max(t, 0); r = std::min<int32_t>(r, (int32_t)fbW); b = std::min<int32_t>(b, (int32_t)fbH);
        if (r <= l || b <= t) { l = 0; t = 0; r = (int32_t)fbW; b = (int32_t)fbH; }
        if (XGpuTilingFirstPass()) { l = 0; t = 0; r = (int32_t)fbW; b = (int32_t)fbH; }
        const VkRect2D scr = { { l, t }, { (uint32_t)(r - l), (uint32_t)(b - t) } };
        const float bf[4] = { F(XReg(xr::RB_BLEND_RED)), F(XReg(xr::RB_BLEND_GREEN)), F(XReg(xr::RB_BLEND_BLUE)), F(XReg(xr::RB_BLEND_ALPHA)) };
        const uint32_t refMask = XReg(xr::RB_STENCILREFMASK);
        if (!g_dynValid || memcmp(&g_boundVp, &vp, sizeof vp) != 0) { vkCmdSetViewport(cb, 0, 1, &vp); g_boundVp = vp; }
        if (!g_dynValid || memcmp(&g_boundSc, &scr, sizeof scr) != 0) { vkCmdSetScissor(cb, 0, 1, &scr); g_boundSc = scr; }
        if (!g_dynValid || memcmp(g_boundBf, bf, sizeof bf) != 0) { vkCmdSetBlendConstants(cb, bf); memcpy(g_boundBf, bf, sizeof bf); }
        if (!g_dynValid || g_boundStencil != refMask)
        {
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, refMask & 0xFF);
            vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (refMask >> 8) & 0xFF);
            vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (refMask >> 16) & 0xFF);
            g_boundStencil = refMask;
        }
        if (!g_dynValid) vkCmdSetDepthBias(cb, 0.0f, 0.0f, 0.0f);
        g_dynValid = true;
    }
    vkCmdDraw(cb, idx.count, 1, idx.start, 0);
    g_dp[9] += QpcNow() - T0;
    if (g_log)
    {
        static int n = 0;
        if (n++ < 60 || g_frame == g_captureFrame)
            Log("[vk] draw #%llu prim=%u count=%u vs=%016llx ps=%016llx rt0=%08X surf=%ux%u depthctl=%08X blend0=%08X mask=%X tex(vs/ps)=%zu/%zu vf=%zu mode=%u",
                (unsigned long long)g_drawCount, dp.primType, idx.count, (unsigned long long)g_curVS.hash, (unsigned long long)g_curPS.hash, XReg(xr::RB_COLOR_INFO), surf.w, surf.h,
                depthCtl, pk.blend[0], colorMask, vs->info.textureSlots.size(), ps->info.textureSlots.size(), vs->info.vfetchSlots.size(), mode);
    }
}

// ---- Utility passes (present, resolve copies, overlay) ------------------------------------------------------------------------------------------
// Fixed shaders with their own pipeline layout: set 0 pushed (binding 0 = texture + sampler, 1 = uniform buffer, 2 = texture for Load), 32 bytes of
// push constants. Positions use Direct3D conventions (the viewport flips Y like for the game's draws).
const char* kVkUtilVS = R"HLSL(
struct VO { float4 p : SV_Position; float2 uv : TEXCOORD0; };
VO main(uint id : SV_VertexID)
{
    VO o; float2 uv = float2((id << 1) & 2, id & 2);
    o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o;
}
)HLSL";
const char* kVkPresentPS = R"HLSL(
struct PC { uint4 swz; uint4 pad; };
[[vk::push_constant]] PC pc;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D<float4> t;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState s;
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 c = t.Sample(s, uv);
    float v[6] = { c.x, c.y, c.z, c.w, 0.0, 1.0 };
    uint sw = pc.swz.x;
    return float4(v[min(sw & 7, 5u)], v[min((sw >> 3) & 7, 5u)], v[min((sw >> 6) & 7, 5u)], 1.0);
}
)HLSL";
const char* kVkCopyPS = R"HLSL(
struct PC { uint4 rect; uint4 cfg; };
[[vk::push_constant]] PC pc;
[[vk::binding(2, 0)]] Texture2D<float4> src;
float4 main(float4 pos : SV_Position) : SV_Target
{
    uint2 p = uint2(pos.xy);
    float4 c = src.Load(int3(pc.rect.xy + p, 0));
    if (pc.cfg.x != 0) c = c.bgra;
    c.rgb *= exp2((float)(int)pc.cfg.y);
    return c;
}
)HLSL";
const char* kVkCopyDepthPS = R"HLSL(
struct PC { uint4 rect; uint4 cfg; };
[[vk::push_constant]] PC pc;
[[vk::binding(2, 0)]] Texture2D<float> srcD;
float4 main(float4 pos : SV_Position) : SV_Target
{
    uint2 p = uint2(pos.xy);
    float d = srcD.Load(int3(pc.rect.xy + p, 0));
    return float4(d, d, d, d);
}
)HLSL";
const char* kVkOverlayHlsl = R"HLSL(
[[vk::binding(1, 0)]] cbuffer C { float4 view; float4 org; uint4 ch[64]; };
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D<float> atlas;
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState smp;
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
enum UtilKind { kUtilPresent = 0, kUtilCopy, kUtilCopyDepth, kUtilOverlay, kUtilCount };
VkShaderModule g_utilVS = VK_NULL_HANDLE, g_utilPS[kUtilCount] = {}, g_overlayVS = VK_NULL_HANDLE;
std::map<uint64_t, VkPipeline> g_utilPipes;
VkPipeline GetUtilPipeline(UtilKind kind, VkFormat fmt)
{
    const uint64_t key = (uint64_t)kind | ((uint64_t)fmt << 8);
    auto it = g_utilPipes.find(key);
    if (it != g_utilPipes.end()) return it->second;
    VkPipelineShaderStageCreateInfo st[2] = {};
    st[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }; st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = kind == kUtilOverlay ? g_overlayVS : g_utilVS; st[0].pName = "main";
    st[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = g_utilPS[kind]; st[1].pName = "main";
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState bl = {};
    bl.colorWriteMask = 0xF;
    if (kind == kUtilOverlay)
    {
        bl.blendEnable = VK_TRUE;
        bl.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; bl.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; bl.colorBlendOp = VK_BLEND_OP_ADD;
        bl.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; bl.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; bl.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    RpKey rk = {}; rk.color[0] = fmt; rk.numColor = 1;
    const VkPipeline p = CreateGraphicsPipeline(st, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, rs, ds, &bl, 1, g_utilLayout, GetRenderPass(rk), true);
    g_utilPipes[key] = p;
    return p;
}
// Draws a utility pass into `target` (viewport in Direct3D convention: x, y, w, h). src: sampled image (binding 0 or 2), ubo: binding 1.
void UtilDraw(UtilKind kind, Img& target, float vx, float vy, float vw, float vh, const VkRect2D& scissor, Img* src, VkSampler smp,
              const void* pc, uint32_t pcSize, const VkDescriptorBufferInfo* ubo, uint32_t vertexCount)
{
    if (src) UseImg(*src, SampledLayout(*src));
    Img* colors[4] = { &target, nullptr, nullptr, nullptr };
    EnsurePass(colors, 1, nullptr, target.w, target.h);
    VkCommandBuffer cb = Cmd();
    const VkPipeline pipe = GetUtilPipeline(kind, target.rtFmt);
    if (!pipe) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkWriteDescriptorSet w[2]; VkDescriptorImageInfo ii; uint32_t nw = 0;
    if (src)
    {
        ii = { smp, src->view, SampledLayout(*src) };
        w[nw] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        const bool load = kind == kUtilCopy || kind == kUtilCopyDepth;
        w[nw].dstBinding = load ? 2 : 0; w[nw].descriptorCount = 1; w[nw].descriptorType = load ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &ii;
        nw++;
        src->lastUse = g_cmdSerial;
    }
    if (ubo)
    {
        w[nw] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[nw].dstBinding = 1; w[nw].descriptorCount = 1; w[nw].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[nw].pBufferInfo = ubo;
        nw++;
    }
    if (nw) vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_utilLayout, 0, nw, w);
    if (pc && pcSize) vkCmdPushConstants(cb, g_utilLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcSize, pc);
    const VkViewport vp = { vx, vy + vh, vw, -vh, 0.0f, 1.0f };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdDraw(cb, vertexCount, 1, 0, 0);
    InvalidateDrawState();   // another pipeline layout disturbed the game's descriptor bindings
}
void CopyImg(Img& src, Img& dst, uint32_t w, uint32_t h)
{
    EndPass();
    VkCommandBuffer cb = Cmd();
    ImgBarrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ImgBarrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c = {};
    c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.extent = { w, h, 1 };
    vkCmdCopyImage(cb, src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    src.lastUse = dst.lastUse = g_cmdSerial;
}

// ---- Resolve (EDRAM -> guest memory) ---------------------------------------------------------------------------------------------------------
// Copies a rectangle of a render target (colour, or depth when isDepth) into the GPU-side resolve entry for `destBase` (same model as the Direct3D 11 backend).
bool GpuResolve(Img* src, bool isDepth, int x0, int y0, uint32_t rw, uint32_t rh, uint32_t destBase, uint32_t destPitch, uint32_t destHeight, uint32_t destEndian, uint32_t destFmt, bool swapRB, int expBias)
{
    uint32_t bpp, comps;
    DXGI_FORMAT df;
    if (isDepth) { df = DXGI_FORMAT_R32_FLOAT; bpp = 4; comps = 1; destFmt = 22; }
    else df = ResolvedFormat(destFmt, bpp, comps);
    const VkFormat vf = VkFormatOf(df);
    if (df == DXGI_FORMAT_UNKNOWN || !vf || !src || rw == 0 || rh == 0) return false;
    const uint32_t alignedPitch = (destPitch + 31) & ~31u;
    const uint32_t hAligned = (std::max(destHeight, rh) + 31) & ~31u;
    const size_t newBytes = (size_t)alignedPitch * hAligned * bpp;
    g_resolvedGen++;

    Resolved* tgt = nullptr;
    uint32_t tgtBase = destBase;
    uint32_t yOff = 0;
    // Predicated tiling: tile k of a frame buffer is resolved to (base + k * tileBytes); it continues the texture of the tiles before it.
    if (g_resolved.find(destBase) == g_resolved.end())
    {
        const size_t rowBlockBytes = (size_t)alignedPitch * 32 * bpp;
        for (auto& kv : g_resolved)
        {
            Resolved& e = kv.second;
            if (kv.first >= destBase || e.isDepth != isDepth || e.dxgi != df || e.pitch != destPitch || e.endian != destEndian || e.w != rw) continue;
            const size_t delta = destBase - kv.first;
            if (delta % rowBlockBytes != 0) continue;
            const uint32_t y = (uint32_t)(delta / rowBlockBytes) * 32;
            const uint32_t surfaceRows = std::max(e.height, e.h);
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
            else if (!(sameLayout && rw >= old.w && rh >= old.h)) { g_flushReason = "resolve-neither"; FlushResolved(destBase); }
        }
        g_flushReason = "resolve-overlap"; FlushOverlapping(destBase, newBytes, destBase);
        tgt = &g_resolved[destBase];
    }
    Resolved& r = *tgt;
    const bool sameLayoutNow = r.im && r.dxgi == df && r.isDepth == isDepth && r.pitch == destPitch && r.endian == destEndian;
    if (!r.im || !sameLayoutNow || r.w < rw || r.h < yOff + rh)
    {
        const bool keep = r.im && sameLayoutNow;
        ImgDesc d;
        d.fmt = vf; d.w = keep ? std::max(r.w, rw) : rw; d.h = keep ? std::max(r.h, yOff + rh) : yOff + rh;
        d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        auto nt = CreateImg(d);
        if (!nt) { if (!keep) EraseResolved(tgtBase); return false; }
        if (keep) CopyImg(*r.im, *nt, r.w, r.h);
        RetireImg(r.im);
        r.im = std::move(nt); r.w = d.w; r.h = d.h;
    }
    r.dxgi = df; r.fmt = destFmt; r.pitch = destPitch; r.endian = destEndian; r.comps = comps; r.isDepth = isDepth;
    r.height = std::max(destHeight, yOff + rh);
    r.bytes = std::max(r.bytes, (size_t)alignedPitch * ((std::max(r.height, r.h) + 31) & ~31u) * bpp);
    r.memSig = MemSig(tgtBase, r.bytes);
    // SV_Position carries framebuffer coordinates (the viewport offset included), so the source rectangle origin is shifted back.
    const uint32_t pc[8] = { (uint32_t)x0, (uint32_t)(y0 - (int)yOff), rw, rh, swapRB ? 1u : 0u, (uint32_t)expBias, 0, 0 };
    const VkRect2D sc = { { 0, (int32_t)yOff }, { rw, rh } };
    UtilDraw(isDepth ? kUtilCopyDepth : kUtilCopy, *r.im, 0.0f, (float)yOff, (float)rw, (float)rh, sc, src, VK_NULL_HANDLE, pc, sizeof pc, nullptr, 3);
    return true;
}

void DoResolve()
{
    ProfScope profScope(g_profResolve);
    g_resolveCount++;
    if (g_envEvery > 1 && IsEnvPassState() && EnvPassSkipped()) { g_envResolvesSkipped++; return; }
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
        if (rt && !rt->im) rt = nullptr;
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
        if (n++ < 40 || g_frame == g_captureFrame) Log("[vk] resolve #%llu src=%u rect=(%d,%d)-(%d,%d) dest=%08X pitch=%u h=%u fmt=%u endian=%u swap=%u clear c=%u d=%u rt=%s",
                          (unsigned long long)g_resolveCount, src, x0, y0, x1, y1, destBase, destPitch, destHeight, destFmt, destEndian, swapRB, clearColor, clearDepth, rt ? "yes" : "none");
    }
    static const bool cpuResolveAll = getenv("ITC_CPU_RESOLVE") != nullptr;
    bool gpuDone = false;
    if (!cpuResolveAll && rt && x1 > x0 && y1 > y0 && destPitch)
        gpuDone = GpuResolve(rt->im.get(), false, x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), destBase, destPitch, destHeight, destEndian, destFmt, swapRB, ((int32_t)(di << 10)) >> 26);
    if (gpuDone)
    {
        auto rit = g_resolved.find(destBase);
        if (rit != g_resolved.end()) rit->second.velocity = rt && src == 0 && rt->fmt == 4 && g_velSkippedThisPass > 0;
    }
    g_velSkippedThisPass = 0;
    if (!cpuResolveAll && src == 4 && !rt && destPitch)
    {
        // Depth/stencil resolve (shadow maps, depth textures): kept on the GPU as a float texture.
        auto dit = g_depthRTs.find(XReg(xr::RB_DEPTH_INFO) & 0xFFF);
        if (dit != g_depthRTs.end() && dit->second.im)
        {
            DepthRT& dr = dit->second;
            const int32_t dx1 = std::min<int32_t>(x1 > 0 ? x1 : (int32_t)dr.w, (int32_t)dr.w), dy1 = std::min<int32_t>(y1 > 0 ? y1 : (int32_t)dr.h, (int32_t)dr.h);
            const int32_t dx0 = std::max(x0, 0), dy0 = std::max(y0, 0);
            if (dx1 > dx0 && dy1 > dy0)
                GpuResolve(dr.im.get(), true, dx0, dy0, (uint32_t)(dx1 - dx0), (uint32_t)(dy1 - dy0), destBase, destPitch, destHeight, destEndian, destFmt, false, 0);
        }
    }
    if (!gpuDone && rt && x1 > x0 && y1 > y0 && destPitch)
    {
        // CPU fallback: read the rectangle back and write it into guest memory in the destination format.
        g_ev.cpuResolves++;
        static int cpuLogged = 0;
        if (cpuLogged++ < 12) Log("[vk] CPU resolve fallback: src=%u rect=(%d,%d)-(%d,%d) dest=%08X pitch=%u h=%u destFmt=%u endian=%u rt fmt=%u", src, x0, y0, x1, y1, destBase, destPitch, destHeight, destFmt, destEndian, rt->fmt);
        const uint32_t rw = x1 - x0, rh = y1 - y0;
        {
            uint32_t qb, qc; ResolvedFormat(destFmt, qb, qc);
            const uint32_t qbpp = qb ? qb : 4;
            g_flushReason = "cpu-resolve-overlap"; FlushOverlapping(destBase, (size_t)((destPitch + 31) & ~31u) * ((std::max(destHeight, rh) + 31) & ~31u) * qbpp);
            EraseResolved(destBase);
        }
        const uint32_t srcBpp = BytesPerPixel(VkFormatOf(rt->dxgi));
        const uint8_t* px = ReadbackImage(*rt->im, (uint32_t)x0, (uint32_t)y0, rw, rh, srcBpp);
        if (px)
        {
            uint32_t dummy[2];
            const uint32_t bpp = PackPixel(F4{ { 0, 0, 0, 0 } }, destFmt, false, dummy);
            uint32_t log2bpp = 0; while ((1u << log2bpp) < bpp) log2bpp++;
            const uint32_t alignedPitch = (destPitch + 31) & ~31u;
            uint8_t* dest = PhysPtrW(destBase);
            const size_t tiledBytes = (size_t)alignedPitch * (((destHeight ? destHeight : rh) + 31) & ~31u) * bpp;
            const size_t writable = CommittedBytes(dest, tiledBytes);
            const DXGI_FORMAT rf = rt->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM : rt->dxgi;
            for (uint32_t y = 0; y < rh; y++)
            {
                const uint8_t* row = px + (size_t)y * rw * srcBpp;
                for (uint32_t x = 0; x < rw && x < alignedPitch; x++)
                {
                    uint32_t pxv[2];
                    PackPixel(ReadPixel(row + (size_t)x * srcBpp, rf), destFmt, swapRB, pxv);
                    const uint32_t off = TiledByteOffset(x, y, alignedPitch, log2bpp);
                    if (off + bpp > writable) continue;
                    if (bpp == 1) { dest[off] = (uint8_t)pxv[0]; continue; }
                    if (destEndian == 4 && bpp == 8)
                    {
                        const uint64_t q = pxv[0] | ((uint64_t)pxv[1] << 32);
                        const uint64_t sw = __builtin_bswap64(q);
                        memcpy(dest + off, &sw, 8);
                        continue;
                    }
                    for (uint32_t i = 0; i < bpp / 4; i++) { const uint32_t v = ApplyEndian(pxv[i], destEndian); memcpy(dest + off + i * 4, &v, 4); }
                }
            }
            NoteMemWrite(destBase, tiledBytes);
        }
    }
    // While a tiled frame is in progress the clears that follow each strip's resolve are deferred (see the Direct3D 11 backend).
    const bool tilingNow = XGpuTilingActive();
    if (clearColor && rt)
    {
        const uint32_t cv = XReg(xr::RB_COLOR_CLEAR);
        float col[4] = { (cv & 0xFF) / 255.0f, ((cv >> 8) & 0xFF) / 255.0f, ((cv >> 16) & 0xFF) / 255.0f, ((cv >> 24) & 0xFF) / 255.0f };
        if (!tilingNow) ClearColorImg(*rt->im, col);
        else { g_pendClear.color = true; g_pendClear.rt = rt; memcpy(g_pendClear.col, col, sizeof col); }
    }
    if (clearDepth)
    {
        const uint32_t dv = XReg(xr::RB_DEPTH_CLEAR);
        const uint32_t dbase = XReg(xr::RB_DEPTH_INFO) & 0xFFF;
        if (!tilingNow)
        {
            auto it = g_depthRTs.find(dbase);
            if (it != g_depthRTs.end() && it->second.im) ClearDepthImg(*it->second.im, (float)(dv >> 8) / 16777215.0f, (uint8_t)(dv & 0xFF));
        }
        else { g_pendClear.depth = true; g_pendClear.dbase = dbase; g_pendClear.dz = (float)(dv >> 8) / 16777215.0f; g_pendClear.dstencil = (uint8_t)(dv & 0xFF); }
    }
}

// ---- Overlay (FPS counter, F3) ------------------------------------------------------------------------------------------------------------------------
struct Overlay
{
    bool tried = false, ok = false, visible = getenv("ITC_NO_OVERLAY") == nullptr;
    std::unique_ptr<Img> atlas;
    VkSampler smp = VK_NULL_HANDLE;
    static constexpr int kCols = 16, kRows = 6, kCellW = 9, kCellH = 18;
} g_ov;
bool OverlayInit()
{
    g_ov.tried = true;
    if (!g_overlayVS || !g_utilPS[kUtilOverlay]) return false;
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
    ImgDesc d; d.fmt = VK_FORMAT_R8_UNORM; d.w = W; d.h = H; d.usage = kTexUsage;
    g_ov.atlas = CreateImg(d);
    if (!g_ov.atlas) return false;
    const Subres s = { a8.data(), a8.size(), 0, 0, (uint32_t)W, (uint32_t)H };
    UploadToImage(*g_ov.atlas, &s, 1);
    g_ov.smp = g_pointClamp;
    return true;
}
Img* g_backImg = nullptr;
std::unique_ptr<Img> g_back, g_lastGood;
void OverlayDraw(const char* const* lines, const int* colors, int nLines)
{
    if (!g_ov.tried) g_ov.ok = OverlayInit();
    if (!g_ov.ok) return;
    uint32_t cells[256];
    int n = 0;
    for (int l = 0; l < nLines; l++)
    {
        const int len = (int)strlen(lines[l]);
        for (int c = 0; c < len + 2 && n < 256; c++) cells[n++] = 127u | ((uint32_t)c << 8) | ((uint32_t)l << 16) | (1u << 24);
    }
    for (int l = 0; l < nLines; l++)
        for (int c = 0; lines[l][c] && n < 256; c++)
            if (lines[l][c] != ' ' && (uint8_t)lines[l][c] >= 32 && (uint8_t)lines[l][c] < 127) cells[n++] = (uint32_t)(uint8_t)lines[l][c] | ((uint32_t)(c + 1) << 8) | ((uint32_t)l << 16) | ((uint32_t)colors[l] << 24);
    if (n == 0) return;
    const VkDeviceSize off = RingAlloc(g_cbRing, 32 + 64 * 16, std::max<VkDeviceSize>(64, g_props.limits.minUniformBufferOffsetAlignment));
    if (off == ~(VkDeviceSize)0) return;
    float* f = (float*)(g_cbRing.buf.ptr + off);
    f[0] = 2.0f / (float)g_backW; f[1] = 2.0f / (float)g_backH; f[2] = (float)Overlay::kCellW; f[3] = (float)Overlay::kCellH;
    f[4] = 6.0f; f[5] = 6.0f; f[6] = 1.0f; f[7] = 0.0f;
    uint32_t* u = (uint32_t*)(f + 8);
    memset(u, 0, 64 * 16);
    memcpy(u, cells, (size_t)n * 4);
    const VkDescriptorBufferInfo ubo = { g_cbRing.buf.buf, off, 32 + 64 * 16 };
    const VkRect2D sc = { { 0, 0 }, { g_backW, g_backH } };
    UtilDraw(kUtilOverlay, *g_back, 0.0f, 0.0f, (float)g_backW, (float)g_backH, sc, g_ov.atlas.get(), g_ov.smp, nullptr, 0, &ubo, (uint32_t)n * 6);
}

// ---- Swapchain -------------------------------------------------------------------------------------------------------------------------------------
bool CreateSwapchain()
{
    VkSurfaceCapabilitiesKHR caps;
    if (!VkOk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, g_surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) return false;
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;   // minimised
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surface, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nf); vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surface, &nf, fmts.data());
    VkSurfaceFormatKHR pick = fmts.empty() ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } : fmts[0];
    for (auto& f : fmts) if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { pick = f; break; }
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = g_surface;
    ci.minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = pick.format; ci.imageColorSpace = pick.colorSpace;
    ci.imageExtent = caps.currentExtent; ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // vsync, like Present(1, 0) in the Direct3D 11 backend
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g_swapchain;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    if (!VkOk(vkCreateSwapchainKHR(g_dev, &ci, nullptr, &sc), "vkCreateSwapchainKHR")) return false;
    if (g_swapchain) vkDestroySwapchainKHR(g_dev, g_swapchain, nullptr);
    g_swapchain = sc; g_swapFmt = pick.format; g_swapExtent = caps.currentExtent;
    uint32_t ni = 0; vkGetSwapchainImagesKHR(g_dev, g_swapchain, &ni, nullptr);
    g_swapImages.resize(ni); vkGetSwapchainImagesKHR(g_dev, g_swapchain, &ni, g_swapImages.data());
    Log("[vk] swapchain %ux%u format %d, %u images", g_swapExtent.width, g_swapExtent.height, (int)g_swapFmt, ni);
    return true;
}
void RecreateSwapchain()
{
    FlushAndWait();
    vkQueueWaitIdle(g_queue);
    g_swapDirty = false;
    if (!CreateSwapchain() && g_swapchain) { vkDestroySwapchainKHR(g_dev, g_swapchain, nullptr); g_swapchain = VK_NULL_HANDLE; }
}

// ---- Init ----------------------------------------------------------------------------------------------------------------------------------------------
bool CreateDeviceObjects()
{
    // Frames
    for (FrameCtx& f : g_fr)
    {
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; pci.queueFamilyIndex = g_qfam;
        if (!VkOk(vkCreateCommandPool(g_dev, &pci, nullptr, &f.pool), "vkCreateCommandPool")) return false;
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (!VkOk(vkCreateFence(g_dev, &fci, nullptr, &f.fence), "vkCreateFence")) return false;
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(g_dev, &sci, nullptr, &f.acquired);
        vkCreateSemaphore(g_dev, &sci, nullptr, &f.rendered);
    }
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (!VkOk(vkCreateFence(g_dev, &fci, nullptr, &g_flushFence), "vkCreateFence")) return false;

    // Rings and fixed buffers
    if (!CreateRing(g_cbRing, "constant", 24ull << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return false;
    if (!CreateRing(g_vbRing, "vertex", 32ull << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return false;
    if (!CreateRing(g_stRing, "staging", 48ull << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return false;
    if (!CreateBuf(g_idxBuf, (VkDeviceSize)kIdxRing * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return false;
    if (!CreateBuf(g_nullBuf, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return false;
    memset(g_nullBuf.ptr, 0, 256);
    g_ssboAlign = std::max<VkDeviceSize>(64, g_props.limits.minStorageBufferOffsetAlignment);

    // Descriptor set layouts / pipeline layouts
    {
        VkDescriptorSetLayoutBinding b[kVkSet0Bindings] = {};
        for (uint32_t i = 0; i < kVkSet0Bindings; i++)
        {
            b[i].binding = i; b[i].descriptorCount = 1;
            if (i >= kVkVbBase && i < kVkVbBase + kVkVbMax) { b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; }
            else { b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[i].stageFlags = i < kVkVbBase ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT; }
        }
        VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; ci.bindingCount = kVkSet0Bindings; ci.pBindings = b;
        if (!VkOk(vkCreateDescriptorSetLayout(g_dev, &ci, nullptr, &g_set0Layout), "set 0 layout")) return false;
    }
    {
        VkDescriptorSetLayoutBinding b[3] = {};
        b[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr };
        b[1] = { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        b[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.bindingCount = 3; ci.pBindings = b;
        if (!VkOk(vkCreateDescriptorSetLayout(g_dev, &ci, nullptr, &g_set1Layout), "set 1 layout")) return false;
        const VkDescriptorSetLayout sets[2] = { g_set0Layout, g_set1Layout };
        VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pl.setLayoutCount = 2; pl.pSetLayouts = sets;
        if (!VkOk(vkCreatePipelineLayout(g_dev, &pl, nullptr, &g_drawLayout), "draw pipeline layout")) return false;
        VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
        VkDescriptorPoolCreateInfo dp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dp.maxSets = 1; dp.poolSizeCount = 2; dp.pPoolSizes = ps;
        if (!VkOk(vkCreateDescriptorPool(g_dev, &dp, nullptr, &g_descPool), "descriptor pool")) return false;
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g_descPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &g_set1Layout;
        if (!VkOk(vkAllocateDescriptorSets(g_dev, &ai, &g_set1), "set 1")) return false;
        const VkDescriptorBufferInfo cbi = { g_cbRing.buf.buf, 0, sizeof(XeCB) }, ibi = { g_idxBuf.buf, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w[3] = {};
        for (int i = 0; i < 3; i++) { w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; w[i].dstSet = g_set1; w[i].dstBinding = (uint32_t)i; w[i].descriptorCount = 1; }
        w[0].descriptorType = w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[0].pBufferInfo = w[1].pBufferInfo = &cbi;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &ibi;
        vkUpdateDescriptorSets(g_dev, 3, w, 0, nullptr);
    }
    {
        VkDescriptorSetLayoutBinding b[3] = {};
        b[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        b[1] = { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr };
        b[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; ci.bindingCount = 3; ci.pBindings = b;
        if (!VkOk(vkCreateDescriptorSetLayout(g_dev, &ci, nullptr, &g_utilSetLayout), "utility set layout")) return false;
        VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
        VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pl.setLayoutCount = 1; pl.pSetLayouts = &g_utilSetLayout; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
        if (!VkOk(vkCreatePipelineLayout(g_dev, &pl, nullptr, &g_utilLayout), "utility pipeline layout")) return false;
    }
    // Pipeline cache (disk)
    {
        std::vector<uint8_t> data;
        if (FILE* f = fopen("vk_pipeline_cache.bin", "rb"))
        {
            fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
            if (n > 0) { data.resize((size_t)n); if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear(); }
            fclose(f);
        }
        VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        ci.initialDataSize = data.size(); ci.pInitialData = data.empty() ? nullptr : data.data();
        if (vkCreatePipelineCache(g_dev, &ci, nullptr, &g_pipeCache) != VK_SUCCESS)
        {
            ci.initialDataSize = 0; ci.pInitialData = nullptr;
            vkCreatePipelineCache(g_dev, &ci, nullptr, &g_pipeCache);
        }
        g_pipeCacheSavedSize = data.size();
        Log("[vk] pipeline cache: %zu bytes loaded", data.size());
    }
    // Samplers
    {
        VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        ci.magFilter = ci.minFilter = VK_FILTER_LINEAR; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; ci.maxLod = VK_LOD_CLAMP_NONE;
        vkCreateSampler(g_dev, &ci, nullptr, &g_linearClamp);
        ci.magFilter = ci.minFilter = VK_FILTER_NEAREST; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(g_dev, &ci, nullptr, &g_pointClamp);
    }
    // Fixed shaders
    {
        std::vector<uint32_t> spv;
        auto mk = [&](const std::string& src, const char* prof, const char* entry, const char* tag) -> VkShaderModule
        {
            spv.clear();
            return CompileSpirv(src, prof, entry, tag, spv) ? MakeModule(spv) : VK_NULL_HANDLE;
        };
        g_rectGS = mk(BuildRectListGeometryShader(), "gs_6_0", "main", "rectlist_gs");
        g_utilVS = mk(kVkUtilVS, "vs_6_0", "main", "util_vs");
        g_utilPS[kUtilPresent] = mk(kVkPresentPS, "ps_6_0", "main", "present_ps");
        g_utilPS[kUtilCopy] = mk(kVkCopyPS, "ps_6_0", "main", "copy_ps");
        g_utilPS[kUtilCopyDepth] = mk(kVkCopyDepthPS, "ps_6_0", "main", "copydepth_ps");
        g_overlayVS = mk(kVkOverlayHlsl, "vs_6_0", "vsmain", "overlay_vs");
        g_utilPS[kUtilOverlay] = mk(kVkOverlayHlsl, "ps_6_0", "psmain", "overlay_ps");
        if (!g_utilVS || !g_utilPS[kUtilPresent] || !g_utilPS[kUtilCopy] || !g_utilPS[kUtilCopyDepth]) { Log("[vk] the fixed shaders did not compile"); return false; }
        if (!g_rectGS) Log("[vk] WARNING: rectangle-list geometry shader unavailable");
    }
    // Images
    g_dummy2D = MakeDummy(VK_IMAGE_VIEW_TYPE_2D);
    g_dummy3D = MakeDummy(VK_IMAGE_VIEW_TYPE_3D);
    g_dummyCube = MakeDummy(VK_IMAGE_VIEW_TYPE_CUBE);
    {
        ImgDesc d; d.fmt = VK_FORMAT_R8G8B8A8_UNORM; d.w = g_backW; d.h = g_backH;
        d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        g_back = CreateImg(d);
        g_lastGood = CreateImg(d);
        if (!g_back || !g_lastGood || !g_dummy2D || !g_dummy3D || !g_dummyCube) return false;
        g_backImg = g_back.get();
        const float black[4] = { 0, 0, 0, 1 };
        ClearColorImg(*g_back, black);
        ClearColorImg(*g_lastGood, black);
    }
    return true;
}

bool EnsureInit()
{
    if (g_initTried) return g_initOk;
    g_initTried = true;
    g_hwnd = (HWND)XGpuCreateWindow(g_backW, g_backH);
    if (!g_hwnd) { Log("[vk] window creation failed"); return false; }
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) { Log("[vk] vulkan-1.dll not found (no Vulkan driver installed)"); return false; }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr) return false;
#define VK_LOAD_GLOBAL(n) n = (PFN_##n)vkGetInstanceProcAddr(nullptr, #n);
    VK_GLOBAL_FUNCS(VK_LOAD_GLOBAL)
    if (!LoadDxc()) return false;

    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "itc"; ai.pEngineName = "itc-recomp"; ai.apiVersion = VK_API_VERSION_1_2;
    const char* instExt[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai; ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = instExt;
    const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
    if (getenv("ITC_VK_VALIDATION")) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers; }
    VkResult r = vkCreateInstance(&ici, nullptr, &g_inst);
    if (r != VK_SUCCESS && ici.enabledLayerCount) { Log("[vk] validation layer unavailable"); ici.enabledLayerCount = 0; r = vkCreateInstance(&ici, nullptr, &g_inst); }
    if (!VkOk(r, "vkCreateInstance")) return false;
#define VK_LOAD_INSTANCE(n) n = (PFN_##n)vkGetInstanceProcAddr(g_inst, #n);
    VK_INSTANCE_FUNCS(VK_LOAD_INSTANCE)

    VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = GetModuleHandleW(nullptr); sci.hwnd = g_hwnd;
    if (!VkOk(vkCreateWin32SurfaceKHR(g_inst, &sci, nullptr, &g_surface), "vkCreateWin32SurfaceKHR")) return false;

    uint32_t n = 0; vkEnumeratePhysicalDevices(g_inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(g_inst, &n, pds.data());
    if (pds.empty()) { Log("[vk] no Vulkan device"); return false; }
    // ITC_VK_DEVICE=<index> picks a device; otherwise the first discrete GPU, else the first device.
    int pick = -1;
    if (getenv("ITC_VK_DEVICE")) pick = std::min<int>(atoi(getenv("ITC_VK_DEVICE")), (int)pds.size() - 1);
    for (size_t i = 0; i < pds.size() && pick < 0; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pick = (int)i; }
    if (pick < 0) pick = 0;
    g_pd = pds[pick];
    vkGetPhysicalDeviceProperties(g_pd, &g_props);
    vkGetPhysicalDeviceMemoryProperties(g_pd, &g_memProps);
    Log("[vk] device %s (Vulkan %u.%u.%u, driver %08X)", g_props.deviceName, VK_API_VERSION_MAJOR(g_props.apiVersion), VK_API_VERSION_MINOR(g_props.apiVersion), VK_API_VERSION_PATCH(g_props.apiVersion), g_props.driverVersion);
    if (g_props.apiVersion < VK_API_VERSION_1_1) { Log("[vk] Vulkan 1.1 is required"); return false; }

    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq); vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, qf.data());
    g_qfam = ~0u;
    for (uint32_t i = 0; i < nq && g_qfam == ~0u; i++)
    {
        VkBool32 present = VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(g_pd, i, g_surface, &present);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) g_qfam = i;
    }
    if (g_qfam == ~0u) { Log("[vk] no queue family with graphics + present"); return false; }

    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    const bool vk12 = g_props.apiVersion >= VK_API_VERSION_1_2;
    if (vk12) f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(g_pd, &f2);
    if (!f2.features.geometryShader) Log("[vk] WARNING: no geometry shaders: rectangle lists will be drawn as triangles");
    VkPhysicalDeviceFeatures en = {};
    en.geometryShader = f2.features.geometryShader; en.depthClamp = f2.features.depthClamp; en.samplerAnisotropy = f2.features.samplerAnisotropy;
    en.textureCompressionBC = f2.features.textureCompressionBC; en.independentBlend = f2.features.independentBlend;
    g_haveDepthClamp = en.depthClamp; g_haveAniso = en.samplerAnisotropy;
    VkPhysicalDeviceVulkan12Features en12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    if (vk12) { en12.samplerMirrorClampToEdge = f12.samplerMirrorClampToEdge; g_haveMirrorOnce = f12.samplerMirrorClampToEdge; }
    {
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(g_pd, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ex(ne); vkEnumerateDeviceExtensionProperties(g_pd, nullptr, &ne, ex.data());
        auto has = [&](const char* name) { for (auto& e : ex) if (!strcmp(e.extensionName, name)) return true; return false; };
        if (!has(VK_KHR_SWAPCHAIN_EXTENSION_NAME) || !has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) { Log("[vk] the device lacks VK_KHR_swapchain or VK_KHR_push_descriptor"); return false; }
    }
    const char* devExt[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = g_qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = vk12 ? &en12 : nullptr;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; dci.enabledExtensionCount = 2; dci.ppEnabledExtensionNames = devExt; dci.pEnabledFeatures = &en;
    if (!VkOk(vkCreateDevice(g_pd, &dci, nullptr, &g_dev), "vkCreateDevice")) return false;
#define VK_LOAD_DEVICE(n) n = (PFN_##n)vkGetDeviceProcAddr(g_dev, #n); if (!n) { Log("[vk] missing device function " #n); return false; }
    VK_DEVICE_FUNCS(VK_LOAD_DEVICE)
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
    {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_pd, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        g_depthFmt = (fp.optimalTilingFeatures & need) == need ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    if (!CreateSwapchain()) Log("[vk] no swapchain yet (window minimised?)");
    if (!CreateDeviceObjects()) { Log("[vk] device object creation failed"); return false; }
    Log("[vk] Vulkan renderer initialized (depth format %s, geometry shaders %d, depth clamp %d)", g_depthFmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8", (int)en.geometryShader, (int)en.depthClamp);
    g_initOk = true;
    return true;
}

// ---- Swap -------------------------------------------------------------------------------------------------------------------------------------------
void DumpImagePng(Img& im, const char* tag)
{
    const uint8_t* px = ReadbackImage(im, 0, 0, im.w, im.h, 4);
    if (!px) return;
    CreateDirectoryA("C:/itc-work/dumps", nullptr);
    char path[256]; snprintf(path, sizeof path, "C:/itc-work/dumps/%s%s_%05u.png", tag, getenv("ITC_DUMP_TAG") ? getenv("ITC_DUMP_TAG") : "", g_frame);
    WritePng(path, px, im.w, im.h, im.w * 4);
    Log("[vk] dumped %s", path);
}
void PresentFrame()
{
    FrameCtx& fr = g_fr[g_fi];
    bool presented = false;
    if (g_swapDirty || !g_swapchain) RecreateSwapchain();
    if (g_swapchain)
    {
        uint32_t ii = 0;
        const uint64_t ta = QpcNow();
        const VkResult ar = vkAcquireNextImageKHR(g_dev, g_swapchain, UINT64_MAX, fr.acquired, VK_NULL_HANDLE, &ii);
        g_profAcquire += QpcNow() - ta;
        if (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR)
        {
            EndPass();
            VkCommandBuffer cb = Cmd();
            ImgBarrier(cb, *g_back, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = g_swapImages[ii]; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            VkImageBlit bl = {};
            bl.srcSubresource = bl.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            bl.srcOffsets[1] = { (int32_t)g_backW, (int32_t)g_backH, 1 };
            bl.dstOffsets[1] = { (int32_t)g_swapExtent.width, (int32_t)g_swapExtent.height, 1 };
            vkCmdBlitImage(cb, g_back->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_swapImages[ii], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            g_back->lastUse = g_cmdSerial;
            SubmitWork(fr.fence, fr.acquired, VK_PIPELINE_STAGE_TRANSFER_BIT, fr.rendered);
            VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
            pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &fr.rendered; pi.swapchainCount = 1; pi.pSwapchains = &g_swapchain; pi.pImageIndices = &ii;
            const uint64_t tp = QpcNow();
            const VkResult pr = vkQueuePresentKHR(g_queue, &pi);
            g_profPresent += QpcNow() - tp;
            if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) g_swapDirty = true;
            else if (pr != VK_SUCCESS) { static int nn = 0; if (nn++ < 20) Log("[vk] vkQueuePresentKHR returned %d", (int)pr); }
            presented = true;
        }
        else if (ar == VK_ERROR_OUT_OF_DATE_KHR) g_swapDirty = true;
        else { static int nn = 0; if (nn++ < 20) Log("[vk] vkAcquireNextImageKHR returned %d", (int)ar); }
    }
    if (!presented) SubmitWork(fr.fence);
    fr.inFlight = true;
    static const bool syncFrames = getenv("ITC_VK_SYNC") != nullptr;   // debug: wait for every frame (no CPU/GPU overlap)
    if (syncFrames) { vkWaitForFences(g_dev, 1, &fr.fence, VK_TRUE, UINT64_MAX); vkResetFences(g_dev, 1, &fr.fence); fr.inFlight = false; }
    // Next frame slot: wait until the GPU is done with it, then recycle its command buffers, ring parts and garbage.
    g_fi = (g_fi + 1) % kFrames;
    FrameCtx& nf = g_fr[g_fi];
    if (nf.inFlight)
    {
        const uint64_t tw = QpcNow();
        vkWaitForFences(g_dev, 1, &nf.fence, VK_TRUE, UINT64_MAX);
        g_profPresent += QpcNow() - tw;
        vkResetFences(g_dev, 1, &nf.fence);
        nf.inFlight = false;
    }
    for (auto& fn : nf.garbage) fn();
    nf.garbage.clear();
    vkResetCommandPool(g_dev, nf.pool, 0);
    nf.nextCmd = 0;
    ResetRings();
}

uint64_t g_pendingMark = 0;
uint32_t g_heldFrames = 0;
bool g_haveGood = false;
void Swap()
{
    if (!EnsureInit()) return;
    ProfScope profScope(g_profSwap);
    g_lastFrameDraws = g_drawCount - g_frameDrawsStart; g_frameDrawsStart = g_drawCount;
    if (g_envFrameDecided == g_frame && !g_envFrameSkip && g_lastFrameDraws > 1200) g_envRenderedOnce = true;
    g_frame++;
    if (g_capture3dAfter >= 0 && g_captureFrame == 0xFFFFFFFFu)
    {
        static const uint64_t t0 = QpcNow();
        LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
        if ((double)(QpcNow() - t0) / (double)qf.QuadPart >= g_capture3dAfter && g_bigDraws >= 3) { g_captureFrame = g_frame; Log("[capture] logging frame %u (%u large draws in the previous frame)", g_frame, g_bigDraws); }
    }
    g_bigDraws = 0;
    const TexFetch tf = GetTexFetch(0);
    Img* front = nullptr;
    if (tf.type() == 2) front = GetTextureImg(0, tf.dimension() == 0 ? 0 : 1);
    if (g_log && g_frame <= 60)
    {
        static uint64_t lastDraws = 0, lastSkipped = 0, lastRes = 0;
        Log("[vk] frame %u: draws=%llu skipped=%llu resolves=%llu (shaders ok/fail %llu/%llu)", g_frame, (unsigned long long)(g_drawCount - lastDraws), (unsigned long long)(g_skippedDraws - lastSkipped),
            (unsigned long long)(g_resolveCount - lastRes), (unsigned long long)g_shaderOk, (unsigned long long)g_shaderFail);
        lastDraws = g_drawCount; lastSkipped = g_skippedDraws; lastRes = g_resolveCount;
    }
    if (front)
    {
        const uint32_t pc[8] = { tf.swizzle(), 0, 0, 0, 0, 0, 0, 0 };
        const VkRect2D sc = { { 0, 0 }, { g_backW, g_backH } };
        UtilDraw(kUtilPresent, *g_back, 0.0f, 0.0f, (float)g_backW, (float)g_backH, sc, front, g_linearClamp, pc, sizeof pc, nullptr, 3);
    }
    else
    {
        const float c[4] = { 0.1f, 0.0f, 0.2f, 1.0f };
        ClearColorImg(*g_back, c);
    }
    static const int dumpFrom = getenv("ITC_DUMP_FROM") ? atoi(getenv("ITC_DUMP_FROM")) : -1;
    if (((g_dumpEvery && dumpFrom < 0 && (g_frame % g_dumpEvery) == 0) || (dumpFrom >= 0 && (int)g_frame >= dumpFrom && (g_dumpEvery == 0 || ((int)g_frame - dumpFrom) % (int)g_dumpEvery == 0))) && g_dumped < g_dumpMax) { g_dumped++; DumpImagePng(*g_back, "frame"); }
    if (g_thumbWanted.load() && (g_frame % 6) == 0)
    {
        if (const uint8_t* px = ReadbackImage(*g_back, 0, 0, g_backW, g_backH, 4))
        {
            uint8_t t[64 * 36];
            for (int by = 0; by < 36; by++)
                for (int bx = 0; bx < 64; bx++)
                {
                    uint32_t sum = 0;
                    for (int y = 0; y < 20; y++)
                    {
                        const uint8_t* row = px + (size_t)(by * 20 + y) * g_backW * 4 + bx * 20 * 4;
                        for (int x = 0; x < 20; x++) sum += (row[x * 4] * 77 + row[x * 4 + 1] * 150 + row[x * 4 + 2] * 29) >> 8;
                    }
                    t[by * 64 + bx] = (uint8_t)(sum / 400);
                }
            std::lock_guard<std::mutex> l(g_thumbMutex);
            memcpy(g_thumbData, t, sizeof t); g_thumbFrame = g_frame;
        }
    }
    {
        // A frame with draws skipped because their shaders are still compiling would flicker: the last complete frame stays on screen meanwhile (at most ~6 s).
        const bool incomplete = g_shaderPending != g_pendingMark;
        g_pendingMark = g_shaderPending;
        if (incomplete && g_haveGood && g_heldFrames < 180) { CopyImg(*g_lastGood, *g_back, g_backW, g_backH); g_heldFrames++; }
        else { CopyImg(*g_back, *g_lastGood, g_backW, g_backH); g_haveGood = true; g_heldFrames = 0; }
    }
    {
        // FPS counter: presented frames per second over 0.5 s windows (title bar and overlay; F3 toggles the overlay).
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
            static const double slowMs = getenv("ITC_SLOW_MS") ? atof(getenv("ITC_SLOW_MS")) : 45.0;
            static FrameEvents prev = {}; static uint64_t prevDraws = 0, prevDecodes = 0, prevSkipped = 0, prevDrawTicks = 0, prevResolves = 0, prevPresent = 0, prevFlushWait = 0, prevPipe = 0, prevPipes = 0, prevFlushes = 0, prevIdle = 0;
            static int logged = 0; static uint32_t logWin = 0;
            const double ms = lastSwapT ? (double)(now - lastSwapT) * 1000.0 / (double)qf.QuadPart : 0.0;
            const double toMs = 1000.0 / (double)qf.QuadPart;
            if (g_frame / 60 != logWin) { logWin = g_frame / 60; logged = 0; }
            if (ms > slowMs && logged++ < 20)
                Log("[slow] frame %u: %.0f ms (draw thread %.0f ms, present/wait %.1f ms, flush waits %.1f ms (%llu), pipelines %.1f ms (+%llu), ring empty %.1f ms): draws %llu skipped +%llu resolves +%llu | decodes +%llu texCreated +%llu texUpdated +%llu | vb created +%llu changed +%llu | rtCreated +%llu cpuResolves +%llu flushes +%llu shaderCompiles +%llu pendingShaders %llu",
                    g_frame, ms, (double)(g_profDraw - prevDrawTicks) * toMs, (double)(g_profPresent - prevPresent) * toMs, (double)(g_profFlushWait - prevFlushWait) * toMs, (unsigned long long)(g_flushes - prevFlushes),
                    (double)(g_profPipeline - prevPipe) * toMs, (unsigned long long)(g_pipelinesCreated - prevPipes), (double)(g_cpIdleTicks - prevIdle) * toMs,
                    (unsigned long long)(g_drawCount - prevDraws), (unsigned long long)(g_skippedDraws - prevSkipped), (unsigned long long)(g_resolveCount - prevResolves),
                    (unsigned long long)(g_decodeCount - prevDecodes), (unsigned long long)(g_ev.texCreated - prev.texCreated), (unsigned long long)(g_ev.texUpdated - prev.texUpdated),
                    (unsigned long long)(g_ev.vbCreated - prev.vbCreated), (unsigned long long)(g_ev.vbPagesChanged - prev.vbPagesChanged),
                    (unsigned long long)(g_ev.rtCreated - prev.rtCreated), (unsigned long long)(g_ev.cpuResolves - prev.cpuResolves), (unsigned long long)(g_ev.flushes - prev.flushes), (unsigned long long)(g_ev.shaderCompiles - prev.shaderCompiles), (unsigned long long)g_shaderPending);
            prev = g_ev; prevDraws = g_drawCount; prevDecodes = g_decodeCount; prevSkipped = g_skippedDraws; prevDrawTicks = g_profDraw; prevResolves = g_resolveCount; prevPresent = g_profPresent;
            prevFlushWait = g_profFlushWait; prevPipe = g_profPipeline; prevPipes = g_pipelinesCreated; prevFlushes = g_flushes; prevIdle = g_cpIdleTicks;
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
            snprintf(title, sizeof title, "Tokyo Xtreme Racer (static recompilation) - Vulkan - %.1f FPS  (%.1f ms)", fps, avgMs);
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
            char l0[80], l1[80];
            snprintf(l0, sizeof l0, "FPS %5.1f   %5.1f ms  (worst %5.1f)   Vulkan", fps, avgMs, worst);
            snprintf(l1, sizeof l1, "draws/frame %4.0f   render thread %3.0f%% cpu (%3.0f%% in draws)", drawsPerFrame, cpuPct, drawPct);
            const char* lines[2] = { l0, l1 };
            const int colors[2] = { fps >= 55.0 ? 2 : fps >= 28.0 ? 3 : 4, 0 };
            OverlayDraw(lines, colors, 2);
        }
    }
    PresentFrame();
    if ((g_frame & 255) == 0)
        for (auto it = g_vbs.begin(); it != g_vbs.end();)
            if (g_frame - it->second.lastFrame > 600) { g_vbBytes -= it->second.shadow.size(); RetireBuf(it->second.buf); it = g_vbs.erase(it); } else ++it;
    if ((g_frame % 600) == 0) SavePipelineCache();
}

}  // namespace

// ---- Entry points (dispatched from xgpu_common.cpp) -------------------------------------------------------------------------------------------------------
uint32_t VkFrame() { return g_frame; }
uint32_t VkCaptureFrame() { return g_captureFrame; }
void VkLoadShader(bool isPixel, const uint32_t* words, uint32_t dwordCount)
{
    ShaderCode& sc = isPixel ? g_curPS : g_curVS;
    sc.code.assign(words, words + dwordCount);
    sc.hash = Hash64(words, dwordCount * 4ull) ^ (isPixel ? 0xA5A5A5A5ull : 0);
    sc.valid = true;
}
void VkDraw(const XDrawParams& dp) { Draw(dp); }
void VkSwap(uint32_t frontbufferPhys, uint32_t width, uint32_t height) { (void)frontbufferPhys; (void)width; (void)height; Swap(); }
void VkIntervalStats(double seconds)
{
    static uint64_t lastDraws = 0, lastRes = 0, lastDecode = 0, lastSkipped = 0, lastShaders = 0, lastPasses = 0, lastFlushes = 0, lastPipes = 0;
    static uint64_t pDec = 0, pHash = 0, pRes = 0, pSwap = 0, pDraw = 0, pFlushWait = 0, pPipe = 0, pPresent = 0;
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
        static uint64_t lastCpu = 0, lastFrames = 0;
        FILETIME c, e, k, u; GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
        const uint64_t cpu = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
        const uint64_t nf = g_frame > lastFrames ? g_frame - lastFrames : 1;
        Log("[stat]   cp thread: cpu %.1f ms/frame, present+frame waits %.1f ms/frame, draws %.1f ms/frame, early submits %.1f ms/frame (%llu), pipeline creation %.1f ms/frame (+%llu) (%llu frames, %.0f%% of wall time busy)",
            (double)(cpu - lastCpu) * 1e-4 / nf, (g_profPresent - pPresent) * ms / nf, (g_profDraw - pDraw) * ms / nf, (g_profFlushWait - pFlushWait) * ms / nf, (unsigned long long)(g_flushes - lastFlushes),
            (g_profPipeline - pPipe) * ms / nf, (unsigned long long)(g_pipelinesCreated - lastPipes), (unsigned long long)nf, 100.0 * (double)(cpu - lastCpu) * 1e-7 / seconds);
        lastCpu = cpu; lastFrames = g_frame;
    }
    Log("[stat] %.1fs: frames=%u draws+%llu (skipped +%llu) resolves+%llu decodes+%llu shaders+%llu render passes+%llu | ms: draw=%.0f decode=%.0f hash=%.0f resolve=%.0f swap=%.0f",
        seconds, g_frame, (unsigned long long)(g_drawCount - lastDraws), (unsigned long long)(g_skippedDraws - lastSkipped), (unsigned long long)(g_resolveCount - lastRes),
        (unsigned long long)(g_decodeCount - lastDecode), (unsigned long long)(g_shaderOk - lastShaders), (unsigned long long)(g_passCount - lastPasses),
        (g_profDraw - pDraw) * ms, (g_profDecode - pDec) * ms, (g_profHash - pHash) * ms, (g_profResolve - pRes) * ms, (g_profSwap - pSwap) * ms);
    {
        static uint64_t last[12] = {};
        Log("[stat]   draw breakdown ms: shader=%.0f idx=%.0f rt=%.0f vb=%.0f tex=%.0f resources=%.0f consts=%.0f pipeline=%.0f record=%.0f",
            (g_dp[0] - last[0]) * ms, (g_dp[1] - last[1]) * ms, (g_dp[2] - last[2]) * ms, (g_dp[4] - last[4]) * ms, (g_dp[5] - last[5]) * ms, (g_dp[6] - last[6]) * ms,
            (g_dp[7] - last[7]) * ms, (g_dp[8] - last[8]) * ms, (g_dp[9] - last[9]) * ms);
        memcpy(last, g_dp, sizeof last);
    }
    {
        static FrameEvents pv = {};
        Log("[stat]   events: tex hashed +%llu created +%llu updated +%llu (%llu KB decoded) memo misses +%llu | vb created +%llu pages compared +%llu changed +%llu ring copies +%llu (%llu KB) | rt created +%llu cpu resolves +%llu flushes +%llu shader compiles +%llu",
            (unsigned long long)(g_ev.texHashed - pv.texHashed), (unsigned long long)(g_ev.texCreated - pv.texCreated), (unsigned long long)(g_ev.texUpdated - pv.texUpdated), (unsigned long long)((g_ev.texBytes - pv.texBytes) / 1024), (unsigned long long)(g_ev.texMemoMiss - pv.texMemoMiss),
            (unsigned long long)(g_ev.vbCreated - pv.vbCreated), (unsigned long long)(g_ev.vbPagesCmp - pv.vbPagesCmp), (unsigned long long)(g_ev.vbPagesChanged - pv.vbPagesChanged),
            (unsigned long long)(g_ev.vbRingCopies - pv.vbRingCopies), (unsigned long long)((g_ev.vbRingBytes - pv.vbRingBytes) / 1024),
            (unsigned long long)(g_ev.rtCreated - pv.rtCreated), (unsigned long long)(g_ev.cpuResolves - pv.cpuResolves), (unsigned long long)(g_ev.flushes - pv.flushes), (unsigned long long)(g_ev.shaderCompiles - pv.shaderCompiles));
        pv = g_ev;
    }
    {
        static uint64_t lh = 0, lm = 0, lr = 0, lv = 0, lcv = 0, lcp = 0;
        size_t blocksUsed = 0; for (auto& b : g_blocks) blocksUsed += b->used;
        Log("[stat]   index cache: hits +%llu misses +%llu resets +%llu (list=%zu), motion-vector draws skipped +%llu%s, constant uploads vs +%llu ps +%llu | memory: %zu blocks (%zu MB used) + %llu MB dedicated, ring overflows cb %llu vb %llu staging %llu, pipelines %zu",
            (unsigned long long)(g_idxHits - lh), (unsigned long long)(g_idxMisses - lm), (unsigned long long)(g_idxResets - lr), g_idxCache.size(), (unsigned long long)(g_velocitySkipped - lv), g_velocityNeeded ? " (pass active)" : "",
            (unsigned long long)(g_cbUploadsV - lcv), (unsigned long long)(g_cbUploadsP - lcp), g_blocks.size(), blocksUsed >> 20, (unsigned long long)(g_memDedicatedBytes >> 20),
            (unsigned long long)g_cbRing.overflows, (unsigned long long)g_vbRing.overflows, (unsigned long long)g_stRing.overflows, g_pipelines.size());
        lh = g_idxHits; lm = g_idxMisses; lr = g_idxResets; lv = g_velocitySkipped; lcv = g_cbUploadsV; lcp = g_cbUploadsP;
    }
    lastDraws = g_drawCount; lastRes = g_resolveCount; lastDecode = g_decodeCount; lastSkipped = g_skippedDraws; lastShaders = g_shaderOk; lastPasses = g_passCount; lastFlushes = g_flushes; lastPipes = g_pipelinesCreated;
    pDec = g_profDecode; pHash = g_profHash; pRes = g_profResolve; pSwap = g_profSwap; pDraw = g_profDraw; pFlushWait = g_profFlushWait; pPipe = g_profPipeline; pPresent = g_profPresent;
}
void VkStats(char* out, size_t outSize)
{
    snprintf(out, outSize, "[vulkan] frames=%u draws=%llu resolves=%llu shaders ok/fail=%llu/%llu skipped=%llu textures=%zu (%zu KB) vbs=%zu pipelines=%zu",
             g_frame, (unsigned long long)g_drawCount, (unsigned long long)g_resolveCount, (unsigned long long)g_shaderOk, (unsigned long long)g_shaderFail,
             (unsigned long long)g_skippedDraws, g_textures.size(), g_texBytes / 1024, g_vbs.size(), g_pipelines.size());
}
