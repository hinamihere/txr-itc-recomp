// "Null GPU": Xenos command processor that consumes the primary ring buffer and executes only the
// packets with side effects the game waits on (fences, memory writes, interrupts, swap). Draws are ignored.
// Also provides the Vd* kernel imports and a 60 Hz vblank interrupt.
#include "guest.h"
#include "xgpu.h"
#include <windows.h>
#include <atomic>
#include <vector>
#include <algorithm>
#include "xenos_regs.h"

extern PPCContext* g_dbgCtx;

static inline uint32_t PhysToVirt(uint32_t phys) { return 0xA0000000u + (phys & 0x1FFFFFFF); }

static constexpr uint32_t kRegWptr = 0x7FC80714;   // CP_RB_WPTR (dword index written by the game)
static constexpr uint32_t kRegRptr = 0x7FC80710;   // mirror of the read pointer

struct Gpu
{
    uint32_t ringBase = 0;            // guest virtual
    uint32_t ringSizeDwords = 0;
    uint32_t rptrWriteback = 0;       // guest virtual, 0 = disabled
    uint32_t irqCallback = 0, irqContext = 0;
    uint32_t rptr = 0;
    std::atomic<bool> started{false};
    uint64_t packets = 0, swaps = 0, fences = 0, draws = 0;
} g_gpu;

static bool g_gpuLog = getenv("ITC_GPU_LOG") != nullptr;

// Xenos stores to memory apply a byte-swap mode selected by the low two address bits; the value is then
// written in host (little-endian) order, so with mode 2 (full swap) the big-endian guest reads the original value.
static void GpuStore32(uint32_t guestAddr, uint32_t v, uint32_t endian)
{
    switch (endian)
    {
    case 1: v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); break;   // 8in16
    case 2: v = __builtin_bswap32(v); break;                                  // 8in32
    case 3: v = (v << 16) | (v >> 16); break;                                 // 16in32
    default: break;
    }
    *(uint32_t*)GP(guestAddr) = v;
}

// GPU register file lives in the MMIO window (plain guest memory). Writes coming from the command stream go through here.
static constexpr uint32_t kRegScratchUmsk = 0x01DC, kRegScratchAddr = 0x01DD, kRegScratch0 = 0x0578;
static uint32_t g_curPkt = 0, g_curHdr = 0;
// Predicated tiling: type-3 packets with bit 0 set only execute when (bin_select & bin_mask) != 0.
static uint64_t g_binMask = 0xFFFFFFFFull, g_binSelect = 0xFFFFFFFFull;
static bool g_tilingActive = false, g_tilingFirstPass = false;
bool XGpuTilingActive() { return g_tilingActive; }
bool XGpuTilingFirstPass() { return g_tilingFirstPass; }
static uint64_t g_predSkipped = 0;
static volatile uint32_t g_waitAddr = 0, g_waitRef = 0, g_waitCur = 0, g_waitMask = 0, g_waitMode = 0, g_inWait = 0;
static uint32_t g_ibHist[16][3]; static int g_ibHistPos = 0;
static void WriteRegister(uint32_t index, uint32_t value)
{
    if (index == 0x57C && g_gpuLog) { static int n = 0; if (n++ < 60) Log("[gpu] SCRATCH4 <- %08X by packet @%08X hdr=%08X", value, g_curPkt, g_curHdr); }
    if (index >= 0x20000) return;
    XGpuWriteRegister(index, value);
    wr32(0x7FC80000 + index * 4, value);
    if (index >= kRegScratch0 && index < kRegScratch0 + 8)
    {
        // Scratch registers are mirrored to memory so the CPU (and WAIT_REG_MEM) can observe them.
        const uint32_t reg = index - kRegScratch0;
        if ((1u << reg) & rd32(0x7FC80000 + kRegScratchUmsk * 4))
        {
            const uint32_t phys = rd32(0x7FC80000 + kRegScratchAddr * 4) + reg * 4;
            if (g_gpuLog) { static int n = 0; if (n++ < 40) Log("[gpu] scratch%u = %08X -> mem %08X", reg, value, phys); }
            if (GuestCommittedBytes(PhysToVirt(phys & ~3u), 4) == 4)
                wr32(PhysToVirt(phys & ~3u), value);
            else { static int bad = 0; if (bad++ < 5) Log("[gpu] scratch%u writeback to unmapped phys %08X ignored", reg, phys); }
        }
    }
}

static PPCFunc* GuestFunc(uint32_t addr)
{
    return *(PPCFunc**)GP(PPC_IMAGE_BASE + PPC_IMAGE_SIZE + uint32_t((addr - PPC_CODE_BASE) * 2));
}

// --- Guest interrupt dispatch (own thread + context so the callback runs like a kernel DPC) ---------
static std::atomic<uint64_t> g_irqCount{0};
static std::atomic<uint32_t> g_pendingIrq{0};   // bit n = source n pending (source 0 = vblank, 1 = CP interrupt)
static CRITICAL_SECTION g_irqLock;

static void DispatchIrq(PPCContext& ctx, uint32_t source, uint32_t cpu)
{
    if (!g_gpu.irqCallback) return;
    g_irqCount++;
    static std::atomic<int> logged{0};
    if (g_gpuLog && logged++ < 12) Log("[gpu] irq dispatch source=%u cpu=%u", source, cpu);
    if (source == 1 && g_gpuLog) { static int n = 0; if (n++ < 40 || rd32(0xA0113010) != 0x82481B68) Log("[gpu] CP irq: slot A0113010=%08X [A0113014]=%08X", rd32(0xA0113010), rd32(0xA0113014)); }
    // An interrupt is delivered on a specific hardware thread; the handler derives its per-CPU bit from the PCR.
    wr8(ctx.r13.u32 + 0x10C, (uint8_t)cpu);
    ctx.r3.u64 = source;
    ctx.r4.u64 = g_gpu.irqContext;
    GuestFunc(g_gpu.irqCallback)(ctx, g_base);
}

static HANDLE g_irqEvent = nullptr;   // signaled when the command processor raises an interrupt

static DWORD WINAPI IrqThread(LPVOID)
{
    ProfNameThread("irq");
    auto* ctx = new PPCContext;
    InitGuestThreadContext(*ctx, 0x40000);
    // InitGuestThreadContext registers the first context as the debug context; this is never the first.
    LARGE_INTEGER freq, nextVb, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&nextVb);
    static const double vbHz = getenv("ITC_VBLANK_HZ") ? atof(getenv("ITC_VBLANK_HZ")) : 60.0;   // experiment: the game paces itself with this simulated vblank
    const LONGLONG period = (LONGLONG)((double)freq.QuadPart / (vbHz > 1.0 ? vbHz : 60.0));
    nextVb.QuadPart += period;
    for (;;)
    {
        QueryPerformanceCounter(&now);
        const LONGLONG remain = nextVb.QuadPart - now.QuadPart;
        if (remain > 0)
        {
            const DWORD ms = (DWORD)(remain * 1000 / freq.QuadPart);
            WaitForSingleObject(g_irqEvent, ms ? ms : 1);   // wakes immediately when the CP raises an interrupt
        }
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= nextVb.QuadPart)
        {
            nextVb.QuadPart += period;
            if (now.QuadPart - nextVb.QuadPart > period * 4) nextVb.QuadPart = now.QuadPart + period;
            // The D3D interrupt handler only processes a vblank if the display interrupt status register says so.
            wr32(0x7FC80000 + 0x6544, rd32(0x7FC80000 + 0x6544) | 1);
            DispatchIrq(*ctx, 0, 0);   // vblank
        }
        uint32_t mask = g_pendingIrq.exchange(0);
        for (uint32_t cpu = 0; cpu < 6; cpu++)
            if (mask & (1u << cpu)) DispatchIrq(*ctx, 1, cpu);
    }
}

// --- PM4 interpreter ------------------------------------------------------------------------------------

static void ExecuteRange(uint32_t addr, uint32_t dwords, int depth);
uint32_t XGpuFrame();
uint32_t XGpuCaptureFrame();

static uint32_t ExecuteOne(uint32_t addr, uint32_t avail, int depth)
{
    // Returns number of dwords consumed (>= 1).
    const uint32_t h = rd32(addr);
    g_curPkt = addr; g_curHdr = h;
    const uint32_t type = h >> 30;
    g_gpu.packets++;
    if (g_gpuLog && g_gpu.packets <= 120)
    {
        if (type == 3) Log("[gpu] pkt @%u (depth %d): type3 op=%02X count=%u  [%08X %08X %08X %08X]", (addr - g_gpu.ringBase) / 4, depth, (h >> 8) & 0x7F, ((h >> 16) & 0x3FFF) + 1, h, rd32(addr + 4), rd32(addr + 8), rd32(addr + 12));
        else Log("[gpu] pkt @%u (depth %d): type%u hdr=%08X", (addr - g_gpu.ringBase) / 4, depth, type, h);
    }
    if (g_gpuLog && g_gpu.packets > 120 && type == 3)
    {
        const uint32_t op = (h >> 8) & 0x7F;
        if (op == 0x54 || op == 0x3C || op == 0x58 || op == 0x64 || op == 0x3D || op == 0x3F)
        {
            static int n = 0;
            if (n++ < 400) Log("[gpu] #%llu @%08X depth %d op=%02X count=%u  [%08X %08X %08X %08X %08X]", (unsigned long long)g_gpu.packets, addr, depth, op, ((h >> 16) & 0x3FFF) + 1,
                                 rd32(addr + 4), rd32(addr + 8), rd32(addr + 12), rd32(addr + 16), rd32(addr + 20));
        }
    }
    if (type == 0)
    {
        const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
        const uint32_t base = h & 0x7FFF;
        const bool single = (h >> 15) & 1;   // all values go to the same register
        for (uint32_t i = 0; i < count; i++)
            WriteRegister(single ? base : base + i, rd32(addr + 4 + i * 4));
        return 1 + count;
    }
    if (type == 1)
    {
        static int n = 0;
        if (n++ < 5) { for (int k = 0; k < 16; k++) { auto& e = g_ibHist[(g_ibHistPos + k) & 15]; Log("[gpu]   IB hist: from pkt@%08X -> phys %08X size %u dwords", e[0], e[1], e[2]); }
            Log("[gpu] BAD type-1 packet @%08X depth %d hdr=%08X (ring base %08X rptr=%u)", addr, depth, h, g_gpu.ringBase, g_gpu.rptr);
            for (int k = -16; k < 8; k += 4) Log("[gpu]   %08X: %08X %08X %08X %08X", addr + k * 4, rd32(addr + k * 4), rd32(addr + k * 4 + 4), rd32(addr + k * 4 + 8), rd32(addr + k * 4 + 12)); }
        return 3;
    }
    if (type == 2) return 1;

    const uint32_t opcode = (h >> 8) & 0x7F;
    const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
    const uint32_t p = addr + 4;
    {
        // Debug: packet trace of the capture frame (ITC_PKT=1 with ITC_LOG_FRAME / ITC_CAPTURE_3D), without the constant/shader upload noise.
        static const bool pktOn = getenv("ITC_PKT") != nullptr;
        static int logged = 0;
        if (pktOn && XGpuFrame() == XGpuCaptureFrame() && logged < 30000 && opcode != 0x2D && opcode != 0x4A && opcode != 0x2B && opcode != 0x27 && opcode != 0x25)
        {
            logged++;
            Log("[pkt] d%d op=%02X n=%u%s mask=%llX sel=%llX [%08X %08X %08X %08X]", depth, opcode, count, (h & 1) ? " PRED" : "", (unsigned long long)g_binMask, (unsigned long long)g_binSelect,
                rd32(p), count > 1 ? rd32(p + 4) : 0, count > 2 ? rd32(p + 8) : 0, count > 3 ? rd32(p + 12) : 0);
        }
    }
    {
        // Predicated tiling (the game renders 3D in 3 horizontal strips). Every draw is submitted once and replayed per strip; which strips a
        // draw touches comes from per-draw screen extents the GPU is supposed to write (EVENT_WRITE_EXT, not emulated), so the draw masks are
        // useless here. Render targets in this renderer are whole frames, so the first strip pass draws everything full frame and the later
        // passes only keep their (predicated) resolves, which copy their slice out of the finished frame.
        const bool isDraw = opcode == 0x22 || opcode == 0x36;
        const bool tiled = g_binSelect != 0xFFFFFFFFull;
        const bool firstPass = tiled && (g_binSelect & 0x80000000ull) != 0;
        const bool laterPass = tiled && !firstPass && g_binSelect != 0;
        static const bool exactMasks = getenv("ITC_EXACT_BIN_MASKS") != nullptr;
        g_tilingActive = tiled && !exactMasks; g_tilingFirstPass = firstPass && !exactMasks;
        bool skip = (h & 1) && ((g_binSelect & g_binMask) == 0 || opcode == 0x64);
        if (!exactMasks && isDraw && (XReg(xr::RB_MODECONTROL) & 7) != 6)
        {
            if (laterPass) skip = true;                                   // everything was drawn in the first pass
            else if (firstPass && (h & 1) && g_binMask != 0) skip = false;   // draw all geometry once, full frame
        }
        if (skip) { g_predSkipped++; return 1 + count; }
    }
    switch (opcode)
    {
    case 0x60: g_binMask = (g_binMask & 0xFFFFFFFF00000000ull) | rd32(p); break;           // SET_BIN_MASK_LO
    case 0x61: g_binMask = (g_binMask & 0xFFFFFFFFull) | ((uint64_t)rd32(p) << 32); break;  // SET_BIN_MASK_HI
    case 0x62: g_binSelect = (g_binSelect & 0xFFFFFFFF00000000ull) | rd32(p); break;        // SET_BIN_SELECT_LO
    case 0x63: g_binSelect = (g_binSelect & 0xFFFFFFFFull) | ((uint64_t)rd32(p) << 32); break;  // SET_BIN_SELECT_HI
    case 0x50: g_binMask = ((uint64_t)rd32(p) << 32) | rd32(p + 4); break;                  // SET_BIN_MASK (hi, lo)
    case 0x51: g_binSelect = ((uint64_t)rd32(p) << 32) | rd32(p + 4); break;                // SET_BIN_SELECT (hi, lo)
    case 0x3F:    // INDIRECT_BUFFER (addr, size)
    case 0x37:    // INDIRECT_BUFFER_PFD
    {
        const uint32_t ibAddr = rd32(p) & 0x1FFFFFFC, ibSize = rd32(p + 4) & 0xFFFFF;
        g_ibHist[g_ibHistPos & 15][0] = addr; g_ibHist[g_ibHistPos & 15][1] = ibAddr; g_ibHist[g_ibHistPos & 15][2] = ibSize; g_ibHistPos++;
        if (depth < 8) ExecuteRange(PhysToVirt(ibAddr), ibSize, depth + 1);
        break;
    }
    case 0x3D:    // MEM_WRITE (addr, data...)
    {
        const uint32_t a = rd32(p);
        for (uint32_t i = 1; i < count; i++) GpuStore32(PhysToVirt(a & ~3u) + (i - 1) * 4, rd32(p + i * 4), a & 3);
        break;
    }
    case 0x58:    // EVENT_WRITE_SHD (initiator, addr, value): fence/timestamp write
    {
        const uint32_t initiator = rd32(p), a = rd32(p + 4), v = rd32(p + 8);
        const uint32_t data = (initiator >> 31) ? (uint32_t)GetTickCount() : v;
        if (g_gpuLog) { static int n = 0; if (n++ < 40) Log("[gpu] EVENT_WRITE_SHD init=%08X addr=%08X value=%08X", initiator, a, v); }
        GpuStore32(PhysToVirt(a & ~3u), data, a & 3);
        g_gpu.fences++;
        break;
    }
    case 0x54:    // INTERRUPT (cpu_mask)
        if (g_gpuLog) Log("[gpu] INTERRUPT packet cpu_mask=%X", rd32(p));
        g_pendingIrq |= rd32(p);
        if (g_irqEvent) SetEvent(g_irqEvent);
        break;
    case 0x64:    // XE_SWAP: present
        g_gpu.swaps++;
        if (g_gpuLog && g_gpu.swaps <= 5) Log("[gpu] swap #%llu", (unsigned long long)g_gpu.swaps);
        XGpuSwap(rd32(p + 4), rd32(p + 8), rd32(p + 12));
        break;
    case 0x3C:    // WAIT_REG_MEM (wait_info, poll_addr, ref, mask, wait)
    {
        const uint32_t info = rd32(p), pollAddr = rd32(p + 4), ref = rd32(p + 8), mask = rd32(p + 12);
        const bool mem = (info & 0x10) != 0;
        int waited = 0;
        g_waitAddr = pollAddr; g_waitRef = ref; g_waitMask = mask; g_waitMode = info; g_inWait = 1;
        LARGE_INTEGER w0; QueryPerformanceCounter(&w0);
        for (int i = 0; i < 25000; i++)
        {
            waited = i;
            const uint32_t cur = (mem ? rd32(PhysToVirt(pollAddr & ~3u)) : rd32(0x7FC80000 + pollAddr * 4)) & mask;
            g_waitCur = cur;
            bool ok = false;
            switch (info & 7) { case 0: ok = false; break; case 1: ok = cur < ref; break; case 2: ok = cur <= ref; break;
                                case 3: ok = cur == ref; break; case 4: ok = cur != ref; break; case 5: ok = cur >= ref; break;
                                case 6: ok = cur > ref; break; default: ok = true; }
            if (ok) break;
            if (i == 0 && g_gpuLog)
            {
                static int dumps = 0;
                if (dumps++ < 3)
                {
                    Log("[gpu] --- 48 dwords before this WAIT_REG_MEM (at %08X) ---", addr);
                    for (uint32_t k = 48; k >= 4; k -= 4)
                        Log("[gpu]   %08X: %08X %08X %08X %08X", addr - k * 4, rd32(addr - k * 4), rd32(addr - k * 4 + 4), rd32(addr - k * 4 + 8), rd32(addr - k * 4 + 12));
                }
            }
            if (i == 0 && g_gpuLog) { static int n = 0; if (n++ < 30) Log("[gpu] WAIT_REG_MEM waits: %s addr=%08X func=%u ref=%08X mask=%08X cur=%08X", mem ? "mem" : "reg", pollAddr, info & 7, ref, mask, cur); }
            if (i < 300) { for (int k = 0; k < 40; k++) YieldProcessor(); }
            else if (i < 3000) SwitchToThread();
            else Sleep(1);   // give up after a very long wait rather than hang the ring forever
        }
        g_inWait = 0;
        { LARGE_INTEGER w1; QueryPerformanceCounter(&w1); extern uint64_t g_cpFenceWaitTicks; g_cpFenceWaitTicks += (uint64_t)(w1.QuadPart - w0.QuadPart); }
        if (waited > 30 && g_gpuLog) { static int n = 0; if (n++ < 40) Log("[gpu] WAIT_REG_MEM waited %d ms (addr=%08X ref=%08X)%s", waited, pollAddr, ref, waited >= 24999 ? " TIMEOUT" : ""); }
        break;
    }
    case 0x22: case 0x36:   // DRAW_INDX (viz query dword first) / DRAW_INDX_2
    {
        uint32_t q = p, remaining = count;
        if (opcode == 0x22) { q += 4; remaining--; }
        const uint32_t init = rd32(q);
        q += 4; remaining--;
        WriteRegister(xr::VGT_DRAW_INITIATOR, init);
        XDrawParams dp;
        dp.primType = init & 0x3F;
        dp.sourceSelect = (init >> 6) & 3;
        dp.index32 = (init >> 11) & 1;
        dp.indexCount = init >> 16;
        if (dp.sourceSelect == 0 && remaining >= 2)
        {
            dp.dmaPhysBase = rd32(q) & 0x1FFFFFFC;
            const uint32_t sz = rd32(q + 4);
            dp.dmaSizeWords = sz & 0xFFFFFF;
            dp.dmaEndian = sz >> 30;
            WriteRegister(xr::VGT_DMA_BASE, rd32(q)); WriteRegister(xr::VGT_DMA_SIZE, sz);
        }
        else if (dp.sourceSelect == 1)
        {
            dp.sourceSelect = 0;   // indices follow in the packet: treat the packet payload as a memory index buffer
            dp.dmaPhysBase = q & 0x1FFFFFFF;
            dp.dmaEndian = (XReg(xr::VGT_DMA_SIZE) >> 30);
        }
        g_gpu.draws++;
        XGpuDraw(dp);
        break;
    }
    case 0x34: case 0x35:   // DRAW_INDX_BIN variants (unused by retail titles)
        g_gpu.draws++;
        break;
    case 0x2D:    // SET_CONSTANT (offset_type, values...)
    {
        const uint32_t ot = rd32(p), index = ot & 0x7FF, type = (ot >> 16) & 0xFF;
        static const uint32_t base[5] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
        if (type < 5)
            for (uint32_t i = 1; i < count; i++) WriteRegister(base[type] + index + i - 1, rd32(p + i * 4));
        break;
    }
    case 0x55: case 0x56:   // SET_CONSTANT2 / SET_SHADER_CONSTANTS (register index, values...)
    {
        const uint32_t index = rd32(p) & 0xFFFF;
        for (uint32_t i = 1; i < count; i++) WriteRegister(index + i - 1, rd32(p + i * 4));
        break;
    }
    case 0x2F:    // LOAD_ALU_CONSTANT (address, offset_type, size)
    {
        const uint32_t a = rd32(p) & 0x1FFFFFFC, ot = rd32(p + 4), size = rd32(p + 8) & 0xFFF;
        const uint32_t index = ot & 0x7FF, type = (ot >> 16) & 0xFF;
        static const uint32_t base[5] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
        if (type < 5)
            for (uint32_t i = 0; i < size; i++) WriteRegister(base[type] + index + i, rd32(PhysToVirt(a) + i * 4));
        break;
    }
    case 0x27:    // IM_LOAD (address|type, start|size): shader microcode from memory
    {
        const uint32_t at = rd32(p), ss = rd32(p + 4);
        const uint32_t size = ss & 0xFFFF;
        std::vector<uint32_t> words(size);
        for (uint32_t i = 0; i < size; i++) words[i] = rd32(PhysToVirt(at & ~3u) + i * 4);
        XGpuLoadShader((at & 3) == 1, words.data(), size);
        break;
    }
    case 0x2B:    // IM_LOAD_IMMEDIATE (type, start|size, code...)
    {
        const uint32_t type = rd32(p), ss = rd32(p + 4);
        const uint32_t size = std::min<uint32_t>(ss & 0xFFFF, count - 2);
        std::vector<uint32_t> words(size);
        for (uint32_t i = 0; i < size; i++) words[i] = rd32(p + 8 + i * 4);
        XGpuLoadShader((type & 3) == 1, words.data(), size);
        break;
    }
    case 0x21:    // REG_RMW (rmw_info, and_mask, or_mask)
    {
        const uint32_t info = rd32(p), reg = info & 0x1FFF;
        uint32_t andm = rd32(p + 4), orm = rd32(p + 8);
        if (info & 0x80000000u) andm = XReg(andm & 0x1FFF);
        if (info & 0x40000000u) orm = XReg(orm & 0x1FFF);
        WriteRegister(reg, (XReg(reg) & andm) | orm);
        break;
    }
    case 0x45:    // COND_WRITE (wait_info, poll_addr, ref, mask, write_addr, write_data)
    {
        const uint32_t info = rd32(p), pollAddr = rd32(p + 4), ref = rd32(p + 8), mask = rd32(p + 12), wAddr = rd32(p + 16), wData = rd32(p + 20);
        const uint32_t cur = ((info & 0x10) ? rd32(PhysToVirt(pollAddr & ~3u)) : XReg(pollAddr & 0x7FFF)) & mask;
        bool ok = false;
        switch (info & 7) { case 0: ok = false; break; case 1: ok = cur < ref; break; case 2: ok = cur <= ref; break; case 3: ok = cur == ref; break;
                            case 4: ok = cur != ref; break; case 5: ok = cur >= ref; break; case 6: ok = cur > ref; break; default: ok = true; }
        if (ok) { if (info & 0x100) wr32(PhysToVirt(wAddr & ~3u), wData); else WriteRegister(wAddr & 0x7FFF, wData); }
        break;
    }
    default:
    {
        static int n = 0;
        static const uint32_t known[] = { 0x50, 0x51, 0x60, 0x61, 0x62, 0x63, 0x10, 0x48, 0x2E, 0x23, 0x25, 0x3B, 0x46, 0x5A, 0x5B, 0x44, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x3E, 0x62, 0x63, 0x60, 0x61 };
        bool k = false; for (uint32_t o : known) if (o == opcode) k = true;
        if (!k && n++ < 10) Log("[gpu] unknown opcode %02X count=%u @%08X depth %d (ring rptr=%u)", opcode, count, addr, depth, g_gpu.rptr);
        break;   // state setup, constants, shaders, events: nothing to do without a renderer
    }
    }
    (void)avail;
    return 1 + count;
}

static void ExecuteRange(uint32_t addr, uint32_t dwords, int depth)
{
    uint32_t i = 0;
    while (i < dwords)
        i += ExecuteOne(addr + i * 4, dwords - i, depth);
}

void XGpuIntervalStats(double seconds);
uint64_t g_cpIdleTicks = 0;   // command processor: time spent with an empty ring (waiting for the guest), QPC ticks
uint64_t g_cpFenceWaitTicks = 0;   // time spent inside WAIT_REG_MEM (waiting for a guest-written value)
static DWORD WINAPI CpThread(LPVOID)
{
    ProfNameThread("gpu command processor");
    Log("[gpu] command processor started: ring=%08X size=%u dwords rptr-writeback=%08X", g_gpu.ringBase, g_gpu.ringSizeDwords, g_gpu.rptrWriteback);
    uint32_t lastLogged = 0;
    uint32_t idle = 0;
    static const bool statusLog = getenv("ITC_STATUS") != nullptr;
    LARGE_INTEGER qf, qlast, qnow; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qlast);
    uint64_t lastPackets = 0;
    for (;;)
    {
        const uint32_t wptrRaw = rd32(kRegWptr);
        const uint32_t wptr = wptrRaw & (g_gpu.ringSizeDwords - 1);
        if (statusLog)
        {
            QueryPerformanceCounter(&qnow);
            if (qnow.QuadPart - qlast.QuadPart >= qf.QuadPart * 2)
            {
                const double secs = (double)(qnow.QuadPart - qlast.QuadPart) / qf.QuadPart;
                Log("[stat] cp: packets/s=%.0f backlog=%u dwords irqs=%llu predicated-skips=%llu", (g_gpu.packets - lastPackets) / secs, (wptr - g_gpu.rptr) & (g_gpu.ringSizeDwords - 1), (unsigned long long)g_irqCount.load(), (unsigned long long)g_predSkipped);
                XGpuIntervalStats(secs);
                { char ms[256]; GuestMemStats(ms, sizeof ms); Log("[stat] mem: %s", ms); }
                lastPackets = g_gpu.packets; qlast = qnow;
            }
        }
        {
            // History of distinct WPTR values (debug): the register must only move forward.
            static uint32_t hist[32][2]; static int hn = 0; static uint32_t lastW = 0xFFFFFFFF;
            if (wptrRaw != lastW)
            {
                hist[hn & 31][0] = wptrRaw; hist[hn & 31][1] = g_gpu.rptr; hn++;
                const uint32_t fwd = (wptr - (lastW == 0xFFFFFFFF ? wptr : (lastW & (g_gpu.ringSizeDwords - 1)))) & (g_gpu.ringSizeDwords - 1);
                if (lastW != 0xFFFFFFFF && fwd > g_gpu.ringSizeDwords / 2)
                {
                    static int bad = 0;
                    if (bad++ < 5)
                    {
                        Log("[gpu] WARNING: WPTR moved backwards: %08X -> %08X (cp rptr=%u)", lastW, wptrRaw, g_gpu.rptr);
                        for (int k = 0; k < 16 && k < hn; k++) { auto& e = hist[(hn - 1 - k) & 31]; Log("[gpu]    wptr history[-%d]: wptr=%08X (cp rptr was %u)", k, e[0], e[1]); }
                    }
                }
                lastW = wptrRaw;
            }
        }
        static uint32_t lastRaw = 0xFFFFFFFF;
        if (g_gpuLog && wptrRaw != lastRaw) { lastRaw = wptrRaw; static int n = 0; if (n++ < 20) Log("[gpu] WPTR=%08X rptr=%u", wptrRaw, g_gpu.rptr); }
        {
            const uint32_t pending = (wptr - g_gpu.rptr) & (g_gpu.ringSizeDwords - 1);
            static int bigLogged = 0;
            if (pending > 8000 && bigLogged < 4)
            {
                bigLogged++;
                // Histogram of the pending packets (top level only).
                uint32_t hist[3][128] = {};
                uint32_t pos = g_gpu.rptr, left = pending, npk = 0;
                while (left > 0 && npk < 20000)
                {
                    const uint32_t h = rd32(g_gpu.ringBase + pos * 4), ty = h >> 30;
                    uint32_t len = ty == 1 ? 3 : ty == 2 ? 1 : (((h >> 16) & 0x3FFF) + 2);
                    if (ty == 3) hist[2][(h >> 8) & 0x7F]++; else hist[ty][ty == 0 ? 0 : 0]++;
                    if (len > left) break;
                    pos = (pos + len) & (g_gpu.ringSizeDwords - 1); left -= len; npk++;
                }
                Log("[gpu] BIG BACKLOG: %u dwords pending (rptr=%u wptr=%u), %u packets parsed: type0=%u type1=%u type2=%u", pending, g_gpu.rptr, wptr, npk, hist[0][0], hist[1][0], hist[2][0] ? 0 : 0);
                for (int o = 0; o < 128; o++) if (hist[2][o]) Log("[gpu]    type3 opcode %02X x %u", o, hist[2][o]);
            }
        }
        if (g_gpu.rptr == wptr)
        {
            // Idle: spin briefly (latency matters: the CPU waits on fences), then yield, then sleep. The time is accounted (g_cpIdleTicks) for the hitch diagnostics.
            idle++;
            LARGE_INTEGER i0; QueryPerformanceCounter(&i0);
            if (idle < 200) { for (int k = 0; k < 40; k++) YieldProcessor(); }
            else if (idle < 4000) SwitchToThread();
            else Sleep(1);
            LARGE_INTEGER i1; QueryPerformanceCounter(&i1);
            g_cpIdleTicks += (uint64_t)(i1.QuadPart - i0.QuadPart);
            continue;
        }
        idle = 0;
        while (g_gpu.rptr != wptr)
        {
            const uint32_t avail = (wptr - g_gpu.rptr) & (g_gpu.ringSizeDwords - 1);
            uint32_t pktAddr = g_gpu.ringBase + g_gpu.rptr * 4;
            {
                // A packet may straddle the end of the ring: execute it from a contiguous copy.
                const uint32_t h = rd32(pktAddr), ty = h >> 30;
                const uint32_t len = ty == 1 ? 3 : ty == 2 ? 1 : (((h >> 16) & 0x3FFF) + 2);
                if (g_gpu.rptr + len > g_gpu.ringSizeDwords)
                {
                    static uint32_t tmp = GuestAlloc(0x20000, 0x1000);
                    for (uint32_t i = 0; i < len; i++) wr32(tmp + i * 4, rd32(g_gpu.ringBase + ((g_gpu.rptr + i) & (g_gpu.ringSizeDwords - 1)) * 4));
                    pktAddr = tmp;
                }
            }
            const uint32_t n = ExecuteOne(pktAddr, avail, 0);
            if (n > avail)
            {
                static int over = 0;
                if (over++ < 10) Log("[gpu] WARNING: packet at ring[%u] (hdr %08X, %u dwords) extends past WPTR (%u dwords available)", g_gpu.rptr, rd32(g_gpu.ringBase + g_gpu.rptr * 4), n, avail);
            }
            g_gpu.rptr = (g_gpu.rptr + n) & (g_gpu.ringSizeDwords - 1);
            if (g_gpu.rptrWriteback) wr32(g_gpu.rptrWriteback, g_gpu.rptr);
            wr32(kRegRptr, g_gpu.rptr);
        }
        if (g_gpuLog && g_gpu.packets - lastLogged > 2000)
        {
            lastLogged = (uint32_t)g_gpu.packets;
            Log("[gpu] packets=%llu fences=%llu swaps=%llu draws=%llu", (unsigned long long)g_gpu.packets,
                (unsigned long long)g_gpu.fences, (unsigned long long)g_gpu.swaps, (unsigned long long)g_gpu.draws);
        }
    }
}

static void StartGpuIfReady()
{
    if (!g_gpu.ringBase || g_gpu.started.exchange(true)) return;
    InitializeCriticalSection(&g_irqLock);
    g_irqEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    CreateThread(nullptr, 0, CpThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 64ull << 20, IrqThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
}

// --- Vd* kernel imports --------------------------------------------------------------------------------
HOST_FUNC(VdInitializeEngines) { ctx.r3.u64 = 1; }
HOST_FUNC(VdShutdownEngines) {}

HOST_FUNC(VdSetGraphicsInterruptCallback)
{
    g_gpu.irqCallback = ctx.r3.u32;
    g_gpu.irqContext = ctx.r4.u32;
    Log("[gpu] interrupt callback %08X ctx=%08X", ctx.r3.u32, ctx.r4.u32);
}

HOST_FUNC(VdInitializeRingBuffer)
{
    // (physical base, log2 size) ; D3D passes 8-byte-entry log2 size
    g_gpu.ringBase = PhysToVirt(ctx.r3.u32);
    g_gpu.ringSizeDwords = 1u << (ctx.r4.u32 + 1);   // log2 is in qwords (matches the 64 KB allocation for 13)
    g_gpu.rptr = 0;
    Log("[gpu] ring buffer base=%08X (phys %08X) log2=%u -> %u dwords", g_gpu.ringBase, ctx.r3.u32, ctx.r4.u32, g_gpu.ringSizeDwords);
    StartGpuIfReady();
}

HOST_FUNC(VdEnableRingBufferRPtrWriteBack)
{
    g_gpu.rptrWriteback = PhysToVirt(ctx.r3.u32);
    Log("[gpu] rptr writeback at %08X", g_gpu.rptrWriteback);
}

HOST_FUNC(VdSetSystemCommandBufferGpuIdentifierAddress) {}

HOST_FUNC(VdGetSystemCommandBuffer)
{
    // (void** outBuffer, void** outIdentifier)
    if (ctx.r3.u32) wr32(ctx.r3.u32, 0xBEEF0000);
    if (ctx.r4.u32) wr32(ctx.r4.u32, 0xBEEF0001);
}

HOST_FUNC(VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 1; }
HOST_FUNC(VdRetrainEDRAM) { ctx.r3.u64 = 0; }
HOST_FUNC(VdRetrainEDRAMWorker) { ctx.r3.u64 = 0; }
HOST_FUNC(VdEnableDisableClockGating) {}
HOST_FUNC(VdPersistDisplay) { ctx.r3.u64 = 0; }
HOST_FUNC(VdSetDisplayMode) { ctx.r3.u64 = 0; }
HOST_FUNC(VdCallGraphicsNotificationRoutines) {}
HOST_FUNC(VdInitializeScalerCommandBuffer) { ctx.r3.u64 = 0; }
HOST_FUNC(VdQueryVideoFlags) { ctx.r3.u64 = 0x1; }   // widescreen/hi-def flags

static void FillVideoMode(uint32_t p)
{
    memset(GP(p), 0, 0x30);
    wr32(p + 0x00, 1280);      // display width
    wr32(p + 0x04, 720);       // display height
    wr32(p + 0x08, 0);         // interlaced
    wr32(p + 0x0C, 1);         // widescreen
    wr32(p + 0x10, 1);         // hi-def
    const float hz = 60.0f; uint32_t bits; memcpy(&bits, &hz, 4);
    wr32(p + 0x14, bits);      // refresh rate
    wr32(p + 0x18, 1);         // video standard (NTSC)
    wr32(p + 0x1C, 0);
}
HOST_FUNC(VdQueryVideoMode) { FillVideoMode(ctx.r3.u32); }
HOST_FUNC(XGetVideoMode) { FillVideoMode(ctx.r3.u32); ctx.r3.u64 = 0; }

HOST_FUNC(VdGetCurrentDisplayInformation)
{
    const uint32_t p = ctx.r3.u32;
    memset(GP(p), 0, 0x58);
    wr16(p + 0, 1280); wr16(p + 2, 720);
}
HOST_FUNC(VdGetCurrentDisplayGamma) { if (ctx.r3.u32) wr32(ctx.r3.u32, 0); if (ctx.r4.u32) wr32(ctx.r4.u32, 0); }

HOST_FUNC(VdSwap)
{
    // (buffer_ptr, fetch_ptr, ..., frontbuffer_ptr ...): the caller reserved 64 dwords in the ring.
    // Layout: type-0 packet writing the 6-dword texture fetch constant of the frontbuffer to fetch slot 0, the swap packet, then NOP padding.
    const uint32_t buf = ctx.r3.u32, fetch = ctx.r4.u32;
    uint32_t i = 0;
    wr32(buf + 4 * i++, (5u << 16) | 0x4800u);                       // type 0, 6 values -> registers 0x4800..0x4805
    for (int k = 0; k < 6; k++) { const uint32_t v = rd32(fetch + 4 * k); wr32(buf + 4 * i++, v); }
    wr32(buf + 4 * i++, 0xC0000000u | (3u << 16) | (0x64u << 8));    // XE_SWAP, 4 payload dwords
    wr32(buf + 4 * i++, 0x504157u);                                  // signature
    wr32(buf + 4 * i++, ctx.r8.u32);                                 // frontbuffer
    wr32(buf + 4 * i++, 1280);
    wr32(buf + 4 * i++, 720);
    while (i < 64) wr32(buf + 4 * i++, 0x80000000u);                 // type-2 NOPs
    ctx.r3.u64 = 0;
}

// Debug: dump command-processor and D3D device ring state (called by the watchdog).
void GpuDebugDump()
{
    {
        const uint64_t p0 = g_gpu.packets; const uint32_t r0 = g_gpu.rptr;
        Sleep(700);
        Log("[gpu] CP progress over 700 ms: packets %llu -> %llu, rptr %u -> %u; current packet @%08X hdr=%08X; in WAIT_REG_MEM=%u (addr=%08X mode=%08X ref=%08X mask=%08X cur=%08X)",
            (unsigned long long)p0, (unsigned long long)g_gpu.packets, r0, g_gpu.rptr, g_curPkt, g_curHdr, g_inWait, g_waitAddr, g_waitMode, g_waitRef, g_waitMask, g_waitCur);
    }
    const uint32_t dev = g_gpu.irqContext;
    const uint32_t p10768 = rd32(dev + 10768), v10780 = rd32(dev + 10780);
    Log("[gpu] dump: ring=%08X sizeDwords=%u cp.rptr=%u WPTR-reg=%u writebackAddr=%08X writebackVal=%u",
        g_gpu.ringBase, g_gpu.ringSizeDwords, g_gpu.rptr, rd32(kRegWptr), g_gpu.rptrWriteback,
        g_gpu.rptrWriteback ? rd32(g_gpu.rptrWriteback) : 0);
    Log("[gpu] dump: dev=%08X [dev+10768]=%08X -> *=%08X  [dev+10780]=%08X  packets=%llu fences=%llu swaps=%llu draws=%llu",
        dev, p10768, p10768 ? rd32(p10768) : 0, v10780, (unsigned long long)g_gpu.packets,
        (unsigned long long)g_gpu.fences, (unsigned long long)g_gpu.swaps, (unsigned long long)g_gpu.draws);
    const uint32_t from = g_gpu.rptr >= 16 ? g_gpu.rptr - 16 : 0, to = (rd32(kRegWptr) + 4);
    for (uint32_t i = from; i < to && i < g_gpu.ringSizeDwords; i += 8)
        Log("[gpu] ring[%u]: %08X %08X %08X %08X %08X %08X %08X %08X", i, rd32(g_gpu.ringBase + i * 4), rd32(g_gpu.ringBase + i * 4 + 4),
            rd32(g_gpu.ringBase + i * 4 + 8), rd32(g_gpu.ringBase + i * 4 + 12), rd32(g_gpu.ringBase + i * 4 + 16), rd32(g_gpu.ringBase + i * 4 + 20),
            rd32(g_gpu.ringBase + i * 4 + 24), rd32(g_gpu.ringBase + i * 4 + 28));
    for (uint32_t i = 0; i < g_gpu.ringSizeDwords; i++)
        if (rd32(g_gpu.ringBase + i * 4) == v10780) Log("[gpu] ring[%u] contains pending fence value %08X (ring dwords %08X %08X %08X %08X)", i, v10780, rd32(g_gpu.ringBase + i * 4 - 8), rd32(g_gpu.ringBase + i * 4 - 4), rd32(g_gpu.ringBase + i * 4), rd32(g_gpu.ringBase + i * 4 + 4));
    Log("[gpu] dev+12960=%08X dev+10776=%08X dev+10784=%08X dev+10788=%08X", rd32(dev + 12960), rd32(dev + 10776), rd32(dev + 10784), rd32(dev + 10788));
    Log("[gpu] vblank state: [15144]=%08X [15148]=%08X [15152]=%08X [15156]=%08X [15160]=%08X  [10772]=%08X  irqs=%llu",
        rd32(dev + 15144), rd32(dev + 15148), rd32(dev + 15152), rd32(dev + 15156), rd32(dev + 15160), rd32(dev + 10772), (unsigned long long)g_irqCount.load());
    Log("[gpu] mem[0x113000..]: %08X %08X %08X %08X   mem[0x112000..]: %08X %08X %08X %08X", rd32(0xA0113000), rd32(0xA0113004), rd32(0xA0113008), rd32(0xA011300C),
        rd32(0xA0112000), rd32(0xA0112004), rd32(0xA0112008), rd32(0xA011200C));
}
