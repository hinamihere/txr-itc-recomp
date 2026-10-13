// Events, semaphores, waits, threads and timers.
// Two flavours of kernel objects exist on the 360:
//   * handle-based (NtCreateEvent, ExCreateThread, ...): guest holds an opaque handle
//   * dispatcher objects living in guest memory (KeInitializeSemaphore, KeSetEvent on a static KEVENT, ...):
//     we lazily associate a host object with the guest address.
#include "guest.h"
#include <vector>
#include <memory>
#include <windows.h>
#include <memory>
#include <mutex>
#include <unordered_map>

extern PPCContext* g_dbgCtx;
extern thread_local uint32_t t_threadId;
void InitGuestThreadContext(PPCContext& ctx, uint32_t stackSize, int cpu);

enum : uint32_t {
    NTS_SUCCESS = 0, NTS_TIMEOUT = 0x102, NTS_UNSUCCESSFUL = 0xC0000001, NTS_INVALID_HANDLE = 0xC0000008,
    NTS_INVALID_PARAMETER = 0xC000000D, NTS_NO_MEMORY = 0xC0000017, NTS_OBJECT_NAME_NOT_FOUND = 0xC0000034,
};

// ---------------------------------------------------------------------------------------------
// Object tables
// ---------------------------------------------------------------------------------------------
struct KObj
{
    enum Type { Event, Semaphore, Thread, Timer, File } type;
    HANDLE h = nullptr;          // host handle
    HANDLE resume = nullptr;     // Thread: event gating a thread created suspended
    bool owned = true;
    uint32_t threadId = 0;       // guest thread id (Thread)
    uint32_t guestObject = 0;    // pseudo kernel object handed out by ObReferenceObjectByHandle
};

static std::mutex g_mutex;
static std::unordered_map<uint32_t, std::shared_ptr<KObj>> g_handles;     // guest handle -> object
static std::unordered_map<uint32_t, std::shared_ptr<KObj>> g_guestObjs;   // guest dispatcher-object address -> object
static uint32_t g_nextHandle = 0x2000;

uint32_t KNewHandle(std::shared_ptr<KObj> o)
{
    std::lock_guard<std::mutex> l(g_mutex);
    const uint32_t h = g_nextHandle; g_nextHandle += 4;
    g_handles[h] = std::move(o);
    return h;
}

std::shared_ptr<KObj> KFindHandle(uint32_t h)
{
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second;
}

// Resolve a guest dispatcher-object pointer to a host object (creating it on first use).
static std::shared_ptr<KObj> KFindGuestObject(uint32_t addr)
{
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = g_guestObjs.find(addr);
    if (it != g_guestObjs.end()) return it->second;

    // DISPATCHER_HEADER: u8 type @0 (0 = notification event, 1 = synchronization event, 5 = semaphore)
    const uint8_t type = rd8(addr);
    auto o = std::make_shared<KObj>();
    if (type == 0 || type == 1)
    {
        o->type = KObj::Event;
        o->h = CreateEventA(nullptr, type == 0, rd32(addr + 4) != 0, nullptr);   // SignalState @4
    }
    else if (type == 5)
    {
        o->type = KObj::Semaphore;
        o->h = CreateSemaphoreA(nullptr, (LONG)rd32(addr + 4), 0x7FFFFFFF, nullptr);
    }
    else
    {
        Log("[sync] unknown dispatcher object type %u at %08X", type, addr);
        return nullptr;
    }
    g_guestObjs[addr] = o;
    return o;
}

static DWORD TimeoutMs(uint32_t pTimeout)
{
    if (!pTimeout) return INFINITE;
    const int64_t t = (int64_t)rd64(pTimeout);
    if (t < 0) return (DWORD)(-t / 10000);
    return 0;   // absolute times are not supported; treat as poll
}

static uint32_t WaitStatus(DWORD r)
{
    if (r == WAIT_OBJECT_0) return NTS_SUCCESS;
    if (r == WAIT_TIMEOUT) return NTS_TIMEOUT;
    return NTS_UNSUCCESSFUL;
}

// ---------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------
HOST_FUNC(NtCreateEvent)
{
    // (PHANDLE out, POBJECT_ATTRIBUTES, ULONG eventType /*0 = notification, 1 = synchronization*/, BOOLEAN initial)
    auto o = std::make_shared<KObj>();
    o->type = KObj::Event;
    o->h = CreateEventA(nullptr, ctx.r5.u32 == 0, ctx.r6.u32 & 0xFF, nullptr);
    const uint32_t handle = KNewHandle(o);
    wr32(ctx.r3.u32, handle);
    if (getenv("ITC_SYNC_LOG")) Log("[sync] NtCreateEvent -> %08X (%s) lr=%08llX", handle, ctx.r5.u32 == 0 ? "manual" : "auto", (unsigned long long)ctx.lr);
    ctx.r3.u64 = NTS_SUCCESS;
}

HOST_FUNC(NtSetEvent)
{
    auto o = KFindHandle(ctx.r3.u32);
    if (!o) { ctx.r3.u64 = NTS_INVALID_HANDLE; return; }
    SetEvent(o->h);
    if (getenv("ITC_SYNC_LOG")) { Log("[sync] T%u NtSetEvent %08X", t_threadId, ctx.r3.u32); if (ctx.r3.u32 == 0x2048) DumpTraceShort(8); }
    if (ctx.r4.u32) wr32(ctx.r4.u32, 0);
    ctx.r3.u64 = NTS_SUCCESS;
}

HOST_FUNC(NtClearEvent)
{
    auto o = KFindHandle(ctx.r3.u32);
    if (!o) { ctx.r3.u64 = NTS_INVALID_HANDLE; return; }
    ResetEvent(o->h);
    ctx.r3.u64 = NTS_SUCCESS;
}

HOST_FUNC(KeSetEvent)
{
    auto o = KFindGuestObject(ctx.r3.u32);
    if (getenv("ITC_SYNC_LOG")) Log("[sync] T%u KeSetEvent %08X (type %u, host %p)", t_threadId, ctx.r3.u32, rd8(ctx.r3.u32), o ? (void*)o->h : nullptr);
    if (!o) { ctx.r3.u64 = 0; return; }
    const bool was = WaitForSingleObject(o->h, 0) == WAIT_OBJECT_0;
    // A successful poll consumed an auto-reset event; SetEvent restores/sets it in either case.
    SetEvent(o->h);
    ctx.r3.u64 = was ? 1 : 0;
}

HOST_FUNC(KeResetEvent)
{
    auto o = KFindGuestObject(ctx.r3.u32);
    if (o) ResetEvent(o->h);
    ctx.r3.u64 = 0;
}

// ---------------------------------------------------------------------------------------------
// Semaphores
// ---------------------------------------------------------------------------------------------
HOST_FUNC(KeInitializeSemaphore)
{
    // (PKSEMAPHORE, count, limit)
    const uint32_t p = ctx.r3.u32;
    wr8(p, 5);                      // type
    wr32(p + 4, ctx.r4.u32);        // signal state
    wr32(p + 16, ctx.r5.u32);       // limit
    auto o = std::make_shared<KObj>();
    o->type = KObj::Semaphore;
    o->h = CreateSemaphoreA(nullptr, (LONG)ctx.r4.u32, (LONG)ctx.r5.u32, nullptr);
    std::lock_guard<std::mutex> l(g_mutex);
    g_guestObjs[p] = o;
}

HOST_FUNC(KeReleaseSemaphore)
{
    // (PKSEMAPHORE, increment, adjustment, wait) -> previous count
    auto o = KFindGuestObject(ctx.r3.u32);
    LONG prev = 0;
    if (o) ReleaseSemaphore(o->h, (LONG)ctx.r5.u32, &prev);
    ctx.r3.u64 = (uint32_t)prev;
}

// ---------------------------------------------------------------------------------------------
// Waits
// ---------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------
// APCs
// ---------------------------------------------------------------------------------------------
struct PendingApc { uint32_t routine, arg1, arg2; };
static thread_local std::vector<PendingApc> t_apcs;

void QueueApc(uint32_t routine, uint32_t arg1, uint32_t arg2)
{
    t_apcs.push_back({ routine, arg1, arg2 });
}

bool RunPendingApcs(PPCContext& ctx)
{
    if (t_apcs.empty()) return false;
    bool ran = false;
    while (!t_apcs.empty())
    {
        const PendingApc a = t_apcs.front();
        t_apcs.erase(t_apcs.begin());
        PPCFunc* fn = *(PPCFunc**)GP(PPC_IMAGE_BASE + PPC_IMAGE_SIZE + uint32_t((a.routine - PPC_CODE_BASE) * 2));
        if (!fn) continue;
        static int logged = 0;
        if (logged++ < 20 && getenv("ITC_IO_LOG")) Log("[io] T%u running APC %08X(%08X, %08X)", t_threadId, a.routine, a.arg1, a.arg2);
        auto saved = std::make_unique<PPCContext>(ctx);
        ctx.r3.u64 = a.arg1; ctx.r4.u64 = a.arg2; ctx.r5.u64 = 0;
        fn(ctx, g_base);
        ctx = *saved;
        ran = true;
    }
    return ran;
}

HOST_FUNC(NtWaitForSingleObjectEx)
{
    if (ctx.r5.u32 && RunPendingApcs(ctx)) { ctx.r3.u64 = 0xC0; return; }   // STATUS_USER_APC
    // (HANDLE, waitMode, alertable, PLARGE_INTEGER timeout)
    auto o = KFindHandle(ctx.r3.u32);
    if (!o) { ctx.r3.u64 = NTS_INVALID_HANDLE; return; }
    if (getenv("ITC_SYNC_LOG") && !ctx.r6.u32)
        Log("[sync] T%u wait(infinite) handle %08X lr=%08llX", t_threadId, ctx.r3.u32, (unsigned long long)ctx.lr);
    ctx.r3.u64 = WaitStatus(WaitForSingleObject(o->h, TimeoutMs(ctx.r6.u32)));
}

HOST_FUNC(KeWaitForSingleObject)
{
    if (ctx.r6.u32 && RunPendingApcs(ctx)) { ctx.r3.u64 = 0xC0; return; }
    // (PVOID object, reason, mode, alertable, PLARGE_INTEGER timeout)
    auto o = KFindGuestObject(ctx.r3.u32);
    if (!o) { ctx.r3.u64 = NTS_INVALID_PARAMETER; return; }
    if (getenv("ITC_SYNC_LOG") && !ctx.r7.u32)
        Log("[sync] T%u KeWaitForSingleObject(infinite) obj %08X type=%u lr=%08llX", t_threadId, ctx.r3.u32, rd8(ctx.r3.u32), (unsigned long long)ctx.lr);
    ctx.r3.u64 = WaitStatus(WaitForSingleObject(o->h, TimeoutMs(ctx.r7.u32)));
}

HOST_FUNC(KeWaitForMultipleObjects)
{
    if (ctx.r8.u32 && RunPendingApcs(ctx)) { ctx.r3.u64 = 0xC0; return; }
    // (count, objects**, waitType /*0 = all, 1 = any*/, reason, mode, alertable, timeout*, waitBlocks)
    const uint32_t count = ctx.r3.u32;
    HANDLE hs[64];
    if (count == 0 || count > 64) { ctx.r3.u64 = NTS_INVALID_PARAMETER; return; }
    for (uint32_t i = 0; i < count; i++)
    {
        auto o = KFindGuestObject(rd32(ctx.r4.u32 + i * 4));
        if (!o) { ctx.r3.u64 = NTS_INVALID_PARAMETER; return; }
        hs[i] = o->h;
    }
    if (getenv("ITC_SYNC_LOG") && !ctx.r9.u32)
        Log("[sync] T%u KeWaitForMultipleObjects(infinite) count=%u first=%08X type=%u lr=%08llX", t_threadId, count, rd32(ctx.r4.u32), rd8(rd32(ctx.r4.u32)), (unsigned long long)ctx.lr);
    const DWORD r = WaitForMultipleObjects(count, hs, ctx.r5.u32 == 0, TimeoutMs(ctx.r9.u32));
    ctx.r3.u64 = (r < WAIT_OBJECT_0 + count) ? (r - WAIT_OBJECT_0) : WaitStatus(r);
}

// ---------------------------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------------------------
struct ThreadStart { uint32_t entry, arg, startup, stackSize; HANDLE resume; int cpu; };

static PPCFunc* LookupGuestFunc(uint32_t addr)
{
    return *(PPCFunc**)GP(PPC_IMAGE_BASE + PPC_IMAGE_SIZE + uint32_t((addr - PPC_CODE_BASE) * 2));
}

static DWORD WINAPI GuestThreadProc2(LPVOID p)
{
    std::unique_ptr<ThreadStart> ts((ThreadStart*)p);
    if (ts->resume) { WaitForSingleObject(ts->resume, INFINITE); CloseHandle(ts->resume); }
    { char nm[64]; snprintf(nm, sizeof nm, "guest entry=%08X cpu=%d", ts->entry, ts->cpu); ProfNameThread(nm); }
    auto* ctx = new PPCContext;
    InitGuestThreadContext(*ctx, ts->stackSize >= 0x10000 ? ts->stackSize : 0x100000, ts->cpu);
    PPCFunc* fn = LookupGuestFunc(ts->entry);
    if (ts->startup)
    {
        ctx->r3.u64 = ts->entry; ctx->r4.u64 = ts->arg;
        fn = LookupGuestFunc(ts->startup);
    }
    else
        ctx->r3.u64 = ts->arg;
    Log("[thread] T%u start entry=%08X arg=%08X", t_threadId, ts->entry, ts->arg);
    fn(*ctx, g_base);
    Log("[thread] entry=%08X returned %08X", ts->entry, ctx->r3.u32);
    return ctx->r3.u32;
}

// Guest-visible pseudo kernel object for ObReferenceObjectByHandle (so KeWaitForSingleObject(thread) works).
static uint32_t PseudoObjectFor(const std::shared_ptr<KObj>& o)
{
    if (o->guestObject) return o->guestObject;
    o->guestObject = GuestAlloc(0x1000, 0x1000);
    std::lock_guard<std::mutex> l(g_mutex);
    g_guestObjs[o->guestObject] = o;
    return o->guestObject;
}

HOST_FUNC(ExCreateThread)
{
    // (PHANDLE out, stackSize, PDWORD threadId out, xapiStartup, startAddress, startContext, creationFlags)
    const uint32_t flags = ctx.r9.u32;
    // creation flags: bit 0 = suspended; bits 24..29 = bitmask of the hardware thread to run on
    const uint32_t cpuMask = (flags >> 24) & 0x3F;
    const int cpu = cpuMask ? (int)__builtin_ctz(cpuMask) : -1;
    auto* ts = new ThreadStart{ ctx.r7.u32, ctx.r8.u32, ctx.r6.u32, ctx.r4.u32, nullptr, cpu };
    auto o = std::make_shared<KObj>();
    o->type = KObj::Thread;
    const bool suspended = flags & 1;
    if (suspended) { o->resume = CreateEventA(nullptr, TRUE, FALSE, nullptr); ts->resume = o->resume; }
    DWORD tid = 0;
    o->h = CreateThread(nullptr, 64ull << 20, GuestThreadProc2, ts, STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
    o->threadId = tid;
    const uint32_t handle = KNewHandle(o);
    if (ctx.r3.u32) wr32(ctx.r3.u32, handle);
    if (ctx.r5.u32) wr32(ctx.r5.u32, tid);
    Log("[thread] ExCreateThread handle=%08X entry=%08X arg=%08X flags=%X cpu=%d", handle, ts->entry, ts->arg, flags, cpu);
    ctx.r3.u64 = NTS_SUCCESS;
}

static uint32_t ResumeThreadObj(const std::shared_ptr<KObj>& o)
{
    if (o && o->resume) { SetEvent(o->resume); o->resume = nullptr; }   // GuestThreadProc2 owns/closes the event
    return 0;
}

HOST_FUNC(NtResumeThread)
{
    auto o = KFindHandle(ctx.r3.u32);
    if (ctx.r4.u32) wr32(ctx.r4.u32, 0);
    if (!o) { ctx.r3.u64 = NTS_INVALID_HANDLE; return; }
    ResumeThreadObj(o);
    ctx.r3.u64 = NTS_SUCCESS;
}

HOST_FUNC(KeResumeThread)
{
    // (PKTHREAD) -> previous suspend count
    std::shared_ptr<KObj> o;
    { std::lock_guard<std::mutex> l(g_mutex); auto it = g_guestObjs.find(ctx.r3.u32); if (it != g_guestObjs.end()) o = it->second; }
    ResumeThreadObj(o);
    ctx.r3.u64 = 0;
}

HOST_FUNC(KeSetBasePriorityThread) { ctx.r3.u64 = 0; }
HOST_FUNC(KeSetAffinityThread) { ctx.r3.u64 = 1; }
HOST_FUNC(NtSuspendThread) { if (ctx.r4.u32) wr32(ctx.r4.u32, 0); ctx.r3.u64 = NTS_SUCCESS; }

HOST_FUNC(ObReferenceObjectByHandle)
{
    // (HANDLE, objectType, PVOID* out)
    auto o = KFindHandle(ctx.r3.u32);
    if (!o) { ctx.r3.u64 = NTS_INVALID_HANDLE; return; }
    wr32(ctx.r5.u32, PseudoObjectFor(o));
    ctx.r3.u64 = NTS_SUCCESS;
}

HOST_FUNC(ObDereferenceObject) { ctx.r3.u64 = 0; }

HOST_FUNC(NtDuplicateObject)
{
    // (HANDLE src, PHANDLE out, options)
    auto o = KFindHandle(ctx.r3.u32);
    if (!o) { ctx.r3.u64 = NTS_INVALID_HANDLE; return; }
    wr32(ctx.r4.u32, KNewHandle(o));
    ctx.r3.u64 = NTS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// Handle close
// ---------------------------------------------------------------------------------------------
HOST_FUNC(NtClose)
{
    std::shared_ptr<KObj> o;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        auto it = g_handles.find(ctx.r3.u32);
        if (it != g_handles.end()) { o = it->second; g_handles.erase(it); }
    }
    // Host handle stays alive while another guest handle / dispatcher object still references the KObj.
    if (o && o.use_count() == 1 && o->owned && o->h && o->type != KObj::Thread) CloseHandle(o->h);
    ctx.r3.u64 = NTS_SUCCESS;
}
