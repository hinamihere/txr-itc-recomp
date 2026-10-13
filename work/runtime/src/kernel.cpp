// Host implementations of the Xbox 360 kernel imports (xboxkrnl / xam) and guest memory/thread plumbing.
// Every HOST_FUNC(Name) here replaces the recompiled import thunk __imp__<Name>.
// Imports without an implementation get a logging stub from stubs.cpp (see gen_stubs.py).
#include "guest.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <map>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Guest memory
// ---------------------------------------------------------------------------------------------
static std::mutex g_memMutex;
static constexpr uint32_t kVirtBase = 0x10000000, kVirtEnd = 0x40000000;
static constexpr uint32_t kPhysBase = 0xA0000000, kPhysEnd = 0xC0000000;

// Address-space heap with a free list (first fit by address, coalescing). The guest frees and re-allocates a lot (streaming
// buffers, textures, temporary pools); a bump allocator ran the 512 MB physical heap dry during the first level load.
struct Heap
{
    std::map<uint32_t, uint32_t> freeList;   // start -> size
    std::map<uint32_t, uint32_t> used;       // start -> size
    uint64_t usedBytes = 0, peakBytes = 0;
    uint32_t highWater = 0;                  // end of the highest block ever handed out: memory below it may hold stale data

    void Init(uint32_t lo, uint32_t hi) { freeList.clear(); used.clear(); freeList[lo] = hi - lo; highWater = lo; usedBytes = peakBytes = 0; }
    uint32_t Alloc(uint32_t size, uint32_t align)
    {
        for (auto it = freeList.begin(); it != freeList.end(); ++it)
        {
            const uint32_t start = it->first, len = it->second;
            const uint64_t a = ((uint64_t)start + align - 1) & ~(uint64_t)(align - 1);
            const uint64_t end = a + size;
            if (end > (uint64_t)start + len) continue;
            freeList.erase(it);
            if (a > start) freeList[start] = (uint32_t)(a - start);
            if (end < (uint64_t)start + len) freeList[(uint32_t)end] = (uint32_t)((uint64_t)start + len - end);
            used[(uint32_t)a] = size;
            usedBytes += size; peakBytes = std::max(peakBytes, usedBytes);
            return (uint32_t)a;
        }
        return 0;
    }
    uint32_t Free(uint32_t addr)   // returns the size released, 0 if addr is not the base of a live block
    {
        auto it = used.find(addr);
        if (it == used.end()) return 0;
        const uint32_t size = it->second;
        used.erase(it);
        usedBytes -= size;
        uint32_t start = addr, len = size;
        auto next = freeList.lower_bound(addr);
        if (next != freeList.end() && start + len == next->first) { len += next->second; next = freeList.erase(next); }
        if (next != freeList.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second == start) { start = prev->first; len += prev->second; freeList.erase(prev); }
        }
        freeList[start] = len;
        return size;
    }
    // Marks [addr, addr + size) as used if it is entirely free (a fixed-address reservation by the guest).
    bool Reserve(uint32_t addr, uint32_t size)
    {
        auto it = freeList.upper_bound(addr);
        if (it == freeList.begin()) return false;
        --it;
        const uint64_t start = it->first, end = start + it->second, e = (uint64_t)addr + size;
        if (addr < start || e > end) return false;
        freeList.erase(it);
        if (addr > start) freeList[(uint32_t)start] = (uint32_t)(addr - start);
        if (e < end) freeList[(uint32_t)e] = (uint32_t)(end - e);
        used[addr] = size;
        usedBytes += size; peakBytes = std::max(peakBytes, usedBytes);
        return true;
    }
};
static Heap g_virtHeap, g_physHeap;
static std::once_flag g_heapInit;
static void EnsureHeaps() { std::call_once(g_heapInit, [] { g_virtHeap.Init(kVirtBase, kVirtEnd); g_physHeap.Init(kPhysBase, kPhysEnd); }); }

static bool g_physAliased = false;
static void MarkPages(uint32_t addr, uint32_t size, uint8_t v);

// The console's 512 MB of RAM is visible at three virtual windows (0xA0000000 cached, 0xC0000000 uncached, 0xE0000000 4K pages);
// physical address = low 29 bits. The title uses all of them (e.g. D3D reads command buffers back through 0xC0000000), so the
// RAM is a shared section mapped three times inside the 4 GB guest reservation.
bool GuestMemInit()
{
    const uint64_t kTotal = 0x100000000ull + 0x10000;
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 0x20000000, nullptr);
    if (section)
    {
        for (int attempt = 0; attempt < 32 && !g_base; attempt++)
        {
            uint8_t* probe = (uint8_t*)VirtualAlloc(nullptr, kTotal, MEM_RESERVE, PAGE_NOACCESS);
            if (!probe) break;
            VirtualFree(probe, 0, MEM_RELEASE);
            uint8_t* low = (uint8_t*)VirtualAlloc(probe, 0xA0000000ull, MEM_RESERVE, PAGE_READWRITE);
            if (!low) continue;
            uint8_t* views[3] = {};
            bool ok = true;
            for (int i = 0; i < 3 && ok; i++)
            {
                views[i] = (uint8_t*)MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, 0, 0x20000000, probe + 0xA0000000ull + (uint64_t)i * 0x20000000);
                ok = views[i] != nullptr;
            }
            uint8_t* guard = ok ? (uint8_t*)VirtualAlloc(probe + 0x100000000ull, 0x10000, MEM_RESERVE, PAGE_READWRITE) : nullptr;
            if (ok && guard) { g_base = probe; g_physAliased = true; break; }
            for (auto* v : views) if (v) UnmapViewOfFile(v);
            VirtualFree(low, 0, MEM_RELEASE);
        }
    }
    if (!g_base)
        g_base = (uint8_t*)VirtualAlloc(nullptr, kTotal, MEM_RESERVE, PAGE_READWRITE);   // fallback: no aliases
    Log("[mem] guest base = %p (physical RAM aliases %s)", g_base, g_physAliased ? "mapped" : "NOT mapped");
    if (g_physAliased) MarkPages(0xA0000000u, 0x60000000u, 1);   // the three RAM windows are always committed
    return g_base != nullptr;
}

void WatchOnCommit(uint32_t addr, uint32_t size);   // main.cpp (debug write watch)

// One byte per 4 KB guest page: 1 = committed. VirtualQuery on the huge guest reservation costs microseconds per call and the GPU
// emulation asks "is this range mapped?" for every resolve / texture / vertex buffer, so it keeps its own table.
static uint8_t g_pageMap[1u << 20];
static void MarkPages(uint32_t addr, uint32_t size, uint8_t v)
{
    if (!size) return;
    const uint64_t end = (uint64_t)addr + size;
    for (uint64_t p = addr >> 12; p < ((end + 0xFFF) >> 12) && p < (1u << 20); p++) g_pageMap[p] = v;
}

size_t GuestCommittedBytes(uint32_t addr, size_t want)
{
    size_t got = 0;
    uint64_t page = addr >> 12;
    while (got < want && page < (1u << 20) && g_pageMap[page])
    {
        got += got == 0 ? 0x1000 - (addr & 0xFFF) : 0x1000;
        page++;
    }
    return std::min(got, want);
}

bool GuestCommit(uint32_t addr, uint32_t size)
{
    if (g_physAliased && addr >= 0xA0000000u) return true;   // the RAM section is committed up front
    const bool ok = VirtualAlloc(g_base + addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    if (ok) MarkPages(addr, size, 1);
    WatchOnCommit(addr, size);
    return ok;
}

void GuestDecommit(uint32_t addr, uint32_t size)
{
    if (g_physAliased && addr >= 0xA0000000u) return;
    VirtualFree(g_base + addr, size, MEM_DECOMMIT);
    MarkPages(addr, size, 0);
}

static uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

uint32_t GuestAlloc(uint32_t size, uint32_t align)
{
    std::lock_guard<std::mutex> lock(g_memMutex);
    EnsureHeaps();
    size = AlignUp(size, 0x1000);
    if (align < 0x1000) align = 0x1000;
    const uint32_t addr = g_virtHeap.Alloc(size, align);
    if (!addr) { Log("[mem] virtual heap exhausted (asked %X, used %llu MB)", size, (unsigned long long)(g_virtHeap.usedBytes >> 20)); return 0; }
    GuestCommit(addr, size);   // freed blocks are decommitted, so recommitting hands out zeroed pages
    return addr;
}

uint32_t GuestFree(uint32_t addr)
{
    std::lock_guard<std::mutex> lock(g_memMutex);
    EnsureHeaps();
    const uint32_t size = g_virtHeap.Free(addr);
    if (size) GuestDecommit(addr, size);
    return size;
}

uint32_t GuestAllocPhysical(uint32_t size, uint32_t align)
{
    std::lock_guard<std::mutex> lock(g_memMutex);
    EnsureHeaps();
    size = AlignUp(size, 0x1000);
    if (align < 0x1000) align = 0x1000;
    const uint32_t addr = g_physHeap.Alloc(size, align);
    if (!addr) { Log("[mem] physical heap exhausted (asked %X, used %llu MB)", size, (unsigned long long)(g_physHeap.usedBytes >> 20)); return 0; }
    // Reused memory still holds the previous owner's data; the kernel hands out zeroed pages.
    if (addr < g_physHeap.highWater) memset(GP(addr), 0, std::min<uint64_t>(size, g_physHeap.highWater - addr));
    g_physHeap.highWater = std::max<uint32_t>(g_physHeap.highWater, addr + size);
    static const bool memLog = getenv("ITC_MEM_LOG") != nullptr;
    if (memLog) Log("[mem] physical alloc %08X size=%X align=%X (in use %llu MB)", addr, size, align, (unsigned long long)(g_physHeap.usedBytes >> 20));
    return addr;
}

uint32_t GuestFreePhysical(uint32_t addr)
{
    std::lock_guard<std::mutex> lock(g_memMutex);
    EnsureHeaps();
    return g_physHeap.Free(addr);
}

uint32_t GuestPhysicalBlockSize(uint32_t addr)
{
    std::lock_guard<std::mutex> lock(g_memMutex);
    EnsureHeaps();
    auto it = g_physHeap.used.find(addr);
    return it == g_physHeap.used.end() ? 0 : it->second;
}

void GuestMemStats(char* out, size_t n)
{
    std::lock_guard<std::mutex> lock(g_memMutex);
    EnsureHeaps();
    snprintf(out, n, "virtual heap %llu MB in use (peak %llu, %zu blocks), physical heap %llu MB in use (peak %llu, %zu blocks)",
             (unsigned long long)(g_virtHeap.usedBytes >> 20), (unsigned long long)(g_virtHeap.peakBytes >> 20), g_virtHeap.used.size(),
             (unsigned long long)(g_physHeap.usedBytes >> 20), (unsigned long long)(g_physHeap.peakBytes >> 20), g_physHeap.used.size());
}

// ---------------------------------------------------------------------------------------------
// Guest threads
// ---------------------------------------------------------------------------------------------
extern PPCContext* g_dbgCtx;
static std::atomic<uint32_t> g_nextThreadId{1};
thread_local PPCContext* t_ctx = nullptr;
thread_local uint32_t t_threadId = 0;
thread_local uint32_t t_tls[64];

std::mutex g_threadRegMutex;
struct ThreadRegEntry { uint32_t id; HANDLE h; PPCContext* ctx; };
std::vector<ThreadRegEntry> g_threadReg;   // guest thread id -> host thread handle (for the watchdog)

void InitGuestThreadContext(PPCContext& ctx, uint32_t stackSize, int cpu)
{
    memset(&ctx, 0, sizeof(ctx));
    const uint32_t stack = GuestAlloc(stackSize, 0x1000);
    const uint32_t pcr = GuestAlloc(0x1000, 0x1000);
    const uint32_t tls = GuestAlloc(0x10000, 0x1000);
    const uint32_t kthread = GuestAlloc(0x1000, 0x1000);
    t_threadId = g_nextThreadId++;

    wr32(pcr + 0x00, tls);                 // TLS static block
    wr32(pcr + 0x70, stack + stackSize);   // stack base
    wr32(pcr + 0x74, stack);               // stack limit
    wr32(pcr + 0x100, kthread);            // current thread
    wr8 (pcr + 0x10C, (uint8_t)(cpu >= 0 ? cpu : (t_threadId % 6)));   // hardware thread (0-5); games index per-CPU arrays with it
    wr32(kthread + 0x14C, t_threadId);
    wr32(kthread + 0x58, kthread);          // per-thread identity read by the D3D device lock (r13+256 -> +88)

    ctx.r1.u64 = stack + stackSize - 0x100;
    ctx.r13.u64 = pcr;
    ctx.fpscr.csr = simde_mm_getcsr();
    t_ctx = &ctx;
    {
        HANDLE self = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, THREAD_ALL_ACCESS, FALSE, 0);
        std::lock_guard<std::mutex> l(g_threadRegMutex);
        g_threadReg.push_back({ t_threadId, self, &ctx });
    }
    if (!g_dbgCtx) g_dbgCtx = &ctx;   // first guest thread = the one we debug
}

struct ThreadStart { uint32_t entry, arg, startup, stackSize; HANDLE resume; int cpu; };

static PPCFunc* LookupGuestFunc(uint32_t addr)
{
    return *(PPCFunc**)GP(PPC_IMAGE_BASE + PPC_IMAGE_SIZE + uint32_t((addr - PPC_CODE_BASE) * 2));
}

static DWORD WINAPI GuestThreadProc(LPVOID p)
{
    std::unique_ptr<ThreadStart> ts((ThreadStart*)p);
    if (ts->resume) WaitForSingleObject(ts->resume, INFINITE);
    ProfNameThread("guest main");
    auto* ctx = new PPCContext;
    InitGuestThreadContext(*ctx, ts->stackSize ? ts->stackSize : 0x100000, ts->cpu);
    PPCFunc* fn = LookupGuestFunc(ts->entry);
    if (ts->startup)
    {
        ctx->r3.u64 = ts->entry; ctx->r4.u64 = ts->arg;
        fn = LookupGuestFunc(ts->startup);
    }
    else
        ctx->r3.u64 = ts->arg;
    Log("[thread %u] start entry=%08X arg=%08X", t_threadId, ts->entry, ts->arg);
    fn(*ctx, g_base);
    Log("[thread %u] returned %08X", t_threadId, ctx->r3.u32);
    return 0;
}

void RunGuestMain(uint32_t entry)
{
    auto* ts = new ThreadStart{ entry, 0, 0, 0x400000, nullptr, 0 };
    HANDLE h = CreateThread(nullptr, 256ull << 20, GuestThreadProc, ts, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    WaitForSingleObject(h, INFINITE);
}

// ---------------------------------------------------------------------------------------------
// Imports
// ---------------------------------------------------------------------------------------------
enum : uint32_t { NTS_SUCCESS = 0, NTS_NO_MEMORY = 0xC0000017 };

HOST_FUNC(NtAllocateVirtualMemory)
{
    // (PVOID* baseAddr, PSIZE_T regionSize, ULONG allocType, ULONG protect, ULONG debugMemory)
    const uint32_t pBase = ctx.r3.u32, pSize = ctx.r4.u32, type = ctx.r5.u32;
    uint32_t addr = rd32(pBase), size = rd32(pSize);
    static const bool vlog = getenv("ITC_MEM_LOG") != nullptr;
    // Page granularity depends on the allocation flags; the kernel rounds the range outward and reports it back.
    uint32_t pageSize = 0x1000;
    if (type & 0x80000000) pageSize = 0x400000;        // 4 MB pages
    else if (type & 0x20000000) pageSize = 0x10000;    // 64 KB "large" pages
    else if (type & 0x10000000) pageSize = 0x1000000;  // 16 MB pages
    if (addr == 0)
    {
        size = AlignUp(size, pageSize);
        addr = GuestAlloc(size, pageSize > 0x10000 ? pageSize : 0x10000);
        if (!addr) { if (vlog) Log("[mem] NtAllocateVirtualMemory size=%X type=%X FAILED", size, type); ctx.r3.u64 = NTS_NO_MEMORY; return; }
    }
    else
    {
        const uint32_t base = addr & ~(pageSize - 1);
        const uint32_t end = AlignUp(addr + size, pageSize);
        addr = base; size = end - base;
        if (addr >= kVirtBase && addr + size <= kVirtEnd)
        {
            // Keep the heap from handing this range out again.
            std::lock_guard<std::mutex> lock(g_memMutex);
            EnsureHeaps();
            g_virtHeap.Reserve(addr, size);
        }
        if (type & 0x1000) GuestCommit(addr, size);   // MEM_COMMIT; reserving needs no action (everything is reserved up front)
    }
    if (vlog) Log("[mem] NtAllocateVirtualMemory -> %08X size=%X type=%X protect=%X", addr, size, type, ctx.r6.u32);
    wr32(pBase, addr);
    wr32(pSize, size);
    ctx.r3.u64 = NTS_SUCCESS;
}

HOST_FUNC(NtFreeVirtualMemory)
{
    // (PVOID* baseAddr, PSIZE_T regionSize, ULONG freeType, ULONG debugMemory)
    // Pages must come back zero-filled when recommitted, so decommit for real.
    const uint32_t pBase = ctx.r3.u32, pSize = ctx.r4.u32, type = ctx.r5.u32;
    const uint32_t addr = rd32(pBase);
    uint32_t size = pSize ? rd32(pSize) : 0;
    static const bool vlog = getenv("ITC_MEM_LOG") != nullptr;
    if (vlog) Log("[mem] NtFreeVirtualMemory %08X size=%X type=%X", addr, size, type);
    if (type & 0x8000)   // MEM_RELEASE: the whole allocation goes back to the heap
    {
        const uint32_t released = GuestFree(addr);
        if (released) size = released;
        else if (size) GuestDecommit(addr & ~0xFFFu, AlignUp(size, 0x1000));
    }
    else if (size && (type & 0x4000)) GuestDecommit(addr & ~0xFFFu, AlignUp(size, 0x1000));   // MEM_DECOMMIT
    size = AlignUp(size, 0x1000);
    if (pSize) wr32(pSize, size);
    ctx.r3.u64 = NTS_SUCCESS;
}

HOST_FUNC(MmAllocatePhysicalMemoryEx)
{
    // (type, size, protect, minAddr, maxAddr, alignment) -> guest address
    uint32_t align = ctx.r8.u32 ? ctx.r8.u32 : 0x1000;
    ctx.r3.u64 = GuestAllocPhysical(ctx.r4.u32, align);
}

HOST_FUNC(MmFreePhysicalMemory)
{
    // (type, baseAddress)
    GuestFreePhysical(ctx.r4.u32);
    ctx.r3.u64 = 0;
}
HOST_FUNC(MmGetPhysicalAddress) { ctx.r3.u64 = ctx.r3.u32 & 0x1FFFFFFF; }
HOST_FUNC(MmQueryAllocationSize) { const uint32_t n = GuestPhysicalBlockSize(ctx.r3.u32); ctx.r3.u64 = n ? n : 0x1000; }

// --- Critical sections: host recursive mutexes keyed by guest address -------------------------
static std::mutex g_csMutex;
static std::unordered_map<uint32_t, std::unique_ptr<std::recursive_mutex>> g_cs;
static std::recursive_mutex& CS(uint32_t addr)
{
    std::lock_guard<std::mutex> lock(g_csMutex);
    auto& p = g_cs[addr];
    if (!p) p = std::make_unique<std::recursive_mutex>();
    return *p;
}
HOST_FUNC(RtlInitializeCriticalSection) { std::lock_guard<std::mutex> l(g_csMutex); g_cs[ctx.r3.u32] = std::make_unique<std::recursive_mutex>(); }
HOST_FUNC(RtlEnterCriticalSection)
{
    auto& m = CS(ctx.r3.u32);
    if (!m.try_lock())
    {
        if (getenv("ITC_SYNC_LOG")) Log("[cs] T%u blocks entering CS %08X lr=%08llX", t_threadId, ctx.r3.u32, (unsigned long long)ctx.lr);
        m.lock();
    }
}
HOST_FUNC(RtlLeaveCriticalSection) { CS(ctx.r3.u32).unlock(); }
HOST_FUNC(RtlTryEnterCriticalSection) { ctx.r3.u64 = CS(ctx.r3.u32).try_lock() ? 1 : 0; }

HOST_FUNC(KeEnterCriticalRegion) {}
HOST_FUNC(KeLeaveCriticalRegion) {}
HOST_FUNC(KeRaiseIrqlToDpcLevel) { ctx.r3.u64 = 0; }
HOST_FUNC(KfLowerIrql) {}
// Spin locks live in guest memory; the owner is the acquiring thread's PCR address (r13).
static void SpinAcquire(PPCContext& ctx, uint32_t addr)
{
    volatile LONG* lock = (volatile LONG*)GP(addr);
    while (InterlockedCompareExchange(lock, (LONG)ctx.r13.u32, 0) != 0) SwitchToThread();
}
static void SpinRelease(uint32_t addr) { InterlockedExchange((volatile LONG*)GP(addr), 0); }

HOST_FUNC(KfAcquireSpinLock) { SpinAcquire(ctx, ctx.r3.u32); ctx.r3.u64 = 0; }   // returns old IRQL
HOST_FUNC(KfReleaseSpinLock) { SpinRelease(ctx.r3.u32); }
HOST_FUNC(KeAcquireSpinLockAtRaisedIrql) { SpinAcquire(ctx, ctx.r3.u32); }
HOST_FUNC(KeReleaseSpinLockFromRaisedIrql) { SpinRelease(ctx.r3.u32); }
HOST_FUNC(KeTryToAcquireSpinLockAtRaisedIrql)
{
    ctx.r3.u64 = InterlockedCompareExchange((volatile LONG*)GP(ctx.r3.u32), (LONG)ctx.r13.u32, 0) == 0 ? 1 : 0;
}

// --- TLS ----------------------------------------------------------------------------------------
static std::atomic<uint32_t> g_tlsNext{0};
HOST_FUNC(KeTlsAlloc) { uint32_t i = g_tlsNext++; ctx.r3.u64 = i < 64 ? i : 0xFFFFFFFF; }
HOST_FUNC(KeTlsFree) { ctx.r3.u64 = 1; }
HOST_FUNC(KeTlsGetValue) { ctx.r3.u64 = ctx.r3.u32 < 64 ? t_tls[ctx.r3.u32] : 0; }
HOST_FUNC(KeTlsSetValue) { if (ctx.r3.u32 < 64) t_tls[ctx.r3.u32] = ctx.r4.u32; ctx.r3.u64 = 1; }

// --- Time -----------------------------------------------------------------------------------------
HOST_FUNC(KeQuerySystemTime)
{
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    wr64(ctx.r3.u32, ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
}
HOST_FUNC(KeQueryPerformanceFrequency) { ctx.r3.u64 = 50000000; }
HOST_FUNC(KeDelayExecutionThread)
{
    // (mode, alertable, PLARGE_INTEGER interval)
    if (ctx.r4.u32 && RunPendingApcs(ctx)) { ctx.r3.u64 = 0xC0; return; }
    int64_t iv = (int64_t)rd64(ctx.r5.u32);
    Sleep(iv < 0 ? (DWORD)(-iv / 10000) : 0);
    ctx.r3.u64 = NTS_SUCCESS;
}

// --- Strings ----------------------------------------------------------------------------------------
HOST_FUNC(RtlInitAnsiString)
{
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    uint32_t len = src ? (uint32_t)strlen((const char*)GP(src)) : 0;
    wr16(dst, (uint16_t)len); wr16(dst + 2, (uint16_t)(src ? len + 1 : 0)); wr32(dst + 4, src);
}

HOST_FUNC(DbgPrint)
{
    Log("[DbgPrint] %s", (const char*)GP(ctx.r3.u32));
    ctx.r3.u64 = 0;
}

// --- Misc system info ---------------------------------------------------------------------------------
HOST_FUNC(XexCheckExecutablePrivilege) { ctx.r3.u64 = 0; }
HOST_FUNC(XexGetModuleHandle)
{
    // (name, PHANDLE out)
    if (ctx.r4.u32) wr32(ctx.r4.u32, 0x82000000);
    ctx.r3.u64 = NTS_SUCCESS;
}
HOST_FUNC(XGetLanguage) { ctx.r3.u64 = 1; }   // English
HOST_FUNC(XGetAVPack) { ctx.r3.u64 = 6; }     // HDMI
HOST_FUNC(KeGetCurrentProcessType) { ctx.r3.u64 = 1; }   // user process

HOST_FUNC(KeEnableFpuExceptions) {}
HOST_FUNC(KeBugCheck) { Log("*** KeBugCheck(%08X)", ctx.r3.u32); ExitProcess(5); }
HOST_FUNC(KeBugCheckEx) { Log("*** KeBugCheckEx(%08X %08X %08X %08X %08X)", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); ExitProcess(5); }
HOST_FUNC(HalReturnToFirmware) { Log("[kernel] HalReturnToFirmware(%u)", ctx.r3.u32); ExitProcess(0); }
