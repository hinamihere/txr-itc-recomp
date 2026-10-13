// Boot harness: loads default.xex, maps it into guest memory, wires up the recompiled
// function table and runs the guest entry point.
#include "guest.h"
#include <windows.h>
#include <timeapi.h>
#include <windows.h>
#include <cstdarg>
#include <fstream>
#include <vector>
#include <mutex>
#include <vector>
#include <map>
#include <string>
#include "image.h"

uint8_t* g_base = nullptr;
static const char** g_traceRing = nullptr;   // owned by itc.exe (generated code writes it)
static unsigned* g_tracePosPtr = nullptr;

PPCContext* g_dbgCtx = nullptr;   // context of the guest thread being debugged (set by the kernel layer)

static void DumpRegs()
{
    if (!g_dbgCtx) return;
    PPCContext& c = *g_dbgCtx;
    Log("regs: lr=%08llX ctr=%08X r1=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X r11=%08X r12=%08X",
        (unsigned long long)c.lr, c.ctr.u32, c.r1.u32, c.r3.u32, c.r4.u32, c.r5.u32, c.r6.u32, c.r7.u32, c.r8.u32, c.r9.u32, c.r10.u32, c.r11.u32, c.r12.u32);
    Log("      r22=%08X r23=%08X r24=%08X r25=%08X r26=%08X r27=%08X r28=%08X r29=%08X r30=%08X r31=%08X",
        c.r22.u32, c.r23.u32, c.r24.u32, c.r25.u32, c.r26.u32, c.r27.u32, c.r28.u32, c.r29.u32, c.r30.u32, c.r31.u32);
}

void DumpTraceShort(int n);
extern std::mutex g_threadRegMutex;
struct ThreadRegEntry { uint32_t id; HANDLE h; PPCContext* ctx; };
extern std::vector<ThreadRegEntry> g_threadReg;

// Print the host call stack of every guest thread as "module+offset" (resolve with symbolize.sh).
static void DumpThreadStacks()
{
    HMODULE exe = GetModuleHandleA(nullptr), dll = GetModuleHandleA("itc_host.dll");
    std::lock_guard<std::mutex> l(g_threadRegMutex);
    for (auto& [id, h, tctx] : g_threadReg)
    {
        if (GetThreadId(h) == GetCurrentThreadId()) continue;
        if (SuspendThread(h) == (DWORD)-1) continue;
        CONTEXT c{}; c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(h, &c))
        {
            char line[1024]; int n = snprintf(line, sizeof line, "T%u:", id);
            for (int f = 0; f < 14 && c.Rip; f++)
            {
                const uint8_t* ip = (const uint8_t*)c.Rip;
                const char* which = "?"; uint64_t off = 0;
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(ip, &mbi, sizeof mbi) && mbi.AllocationBase == (void*)exe) { which = "exe"; off = ip - (const uint8_t*)exe; }
                else if (mbi.AllocationBase == (void*)dll) { which = "dll"; off = ip - (const uint8_t*)dll; }
                n += snprintf(line + n, sizeof line - n, " %s+%llX", which, (unsigned long long)off);
                DWORD64 base = 0; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
                if (!rf) break;
                PVOID handlerData; DWORD64 frame;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &handlerData, &frame, nullptr);
            }
            Log("%s", line);
        }
        ResumeThread(h);
    }
}

static void DumpThreadTraces(int n)
{
    std::lock_guard<std::mutex> l(g_threadRegMutex);
    for (auto& [id, h, tctx] : g_threadReg)
    {
        const uint32_t pcr = tctx->r13.u32;
        const uint64_t* t = (const uint64_t*)(g_base + pcr + 0x800);
        const uint64_t pos = t[-1];
        char line[2048]; int len = snprintf(line, sizeof line, "TRACE T%u (r1=%08X):", id, tctx->r1.u32);
        for (int i = n; i >= 1; i--)
        {
            const char* f = (const char*)t[(pos - i) & 63];
            if (pos >= (uint64_t)i && f) len += snprintf(line + len, sizeof line - len, " %s", f + (strncmp(f, "__imp__", 7) == 0 ? 7 : 0));
        }
        Log("%s", line);
    }
}

static void DumpTrace(int n)
{
    DumpRegs();
    Log("--- last %d guest function entries (oldest first) ---", n);
    for (int i = n; i >= 1; i--)
    {
        const char* f = g_traceRing ? g_traceRing[(*g_tracePosPtr - i) & 255] : nullptr;
        if (f) Log("  %s", f);
    }
}

static std::mutex g_logMutex;
void Log(const char* fmt, ...)
{
    // stderr is unbuffered and the MinGW printf core emits one fputc (= one WriteFile) per character, so format into a local buffer first
    // and write the finished line with a single call; a verbose frame capture is otherwise limited by the system call rate.
    char buf[8192];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    std::lock_guard<std::mutex> lock(g_logMutex);
    fwrite(buf, 1, (size_t)n, stderr);
}

void DumpTraceShort(int n)
{
    for (int i = n; i >= 1; i--) { const char* f = g_traceRing ? g_traceRing[(*g_tracePosPtr - i) & 255] : nullptr; if (f) Log("    trace: %s", f); }
}

static void LogStackFrom(CONTEXT c, const char* label, int maxFrames)
{
    HMODULE exe = GetModuleHandleA(nullptr), dll = GetModuleHandleA("itc_host.dll");
    char line[2048]; int n = snprintf(line, sizeof line, "%s:", label);
    for (int f = 0; f < maxFrames && c.Rip; f++)
    {
        const uint8_t* ip = (const uint8_t*)c.Rip;
        const char* which = "?"; uint64_t off = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(ip, &mbi, sizeof mbi) && mbi.AllocationBase == (void*)exe) { which = "exe"; off = ip - (const uint8_t*)exe; }
        else if (mbi.AllocationBase == (void*)dll) { which = "dll"; off = ip - (const uint8_t*)dll; }
        n += snprintf(line + n, sizeof line - n, " %s+%llX", which, (unsigned long long)off);
        DWORD64 base = 0; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (!rf) break;
        PVOID handlerData; DWORD64 frame;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &handlerData, &frame, nullptr);
    }
    Log("%s", line);
}

void GpuDebugDump();
// Write watch (debug): ITC_WATCH=<hex guest address> write-protects that page and logs every writer.
static uint32_t g_watchPage = 0;
static thread_local uint8_t* t_watchReprotect = nullptr;
static void LogStackFrom(CONTEXT c, const char* label, int maxFrames);

void WatchOnCommit(uint32_t addr, uint32_t size)
{
    if (g_watchPage && g_watchPage >= (addr & ~0xFFFu) && g_watchPage < addr + size)
    {
        DWORD old; VirtualProtect(g_base + g_watchPage, 0x1000, PAGE_READONLY, &old);
        Log("[watch] page %08X protected (commit)", g_watchPage);
    }
}

static void DumpThreadTraces(int n);
static LONG CALLBACK CrashHandler(PEXCEPTION_POINTERS e)
{
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && t_watchReprotect)
    {
        DWORD old; VirtualProtect(t_watchReprotect, 0x1000, PAGE_READONLY, &old);
        t_watchReprotect = nullptr;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_watchPage && e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->ExceptionInformation[0] == 1)
    {
        uint8_t* fault = (uint8_t*)e->ExceptionRecord->ExceptionInformation[1];
        uint8_t* page = g_base + g_watchPage;
        if (fault >= page && fault < page + 0x1000)
        {
            static int n = 0;
            if (n++ < 60)
            {
                char label[96]; snprintf(label, sizeof label, "T0:WRITE-WATCH guest=%08X", (unsigned)(fault - g_base));
                LogStackFrom(*e->ContextRecord, label, 14);
            }
            DWORD old; VirtualProtect(page, 0x1000, PAGE_READWRITE, &old);
            t_watchReprotect = page;
            e->ContextRecord->EFlags |= 0x100;   // single-step the faulting store, then re-protect
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    const auto code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_BREAKPOINT)
        return EXCEPTION_CONTINUE_SEARCH;

    const uint64_t fault = e->ExceptionRecord->NumberParameters > 1 ? e->ExceptionRecord->ExceptionInformation[1] : 0;
    const bool inGuest = fault >= (uint64_t)g_base && fault < (uint64_t)g_base + 0x100000000ull;
    Log("*** CRASH: code=%08lX host-rip=%p (module+%llX) %s guest-addr=%08llX", code, e->ExceptionRecord->ExceptionAddress, (unsigned long long)((uint8_t*)e->ExceptionRecord->ExceptionAddress - (uint8_t*)GetModuleHandleA(nullptr)),
        e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
        inGuest ? (unsigned long long)(fault - (uint64_t)g_base) : (unsigned long long)fault);
    {
        std::lock_guard<std::mutex> l(g_threadRegMutex);
        for (auto& [id, h, tctx] : g_threadReg)
            if (GetThreadId(h) == GetCurrentThreadId())
            {
                PPCContext& c = *tctx;
                Log("crash thread T%u regs: lr=%08llX ctr=%08X r1=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X r11=%08X r12=%08X", id,
                    (unsigned long long)c.lr, c.ctr.u32, c.r1.u32, c.r3.u32, c.r4.u32, c.r5.u32, c.r6.u32, c.r7.u32, c.r8.u32, c.r9.u32, c.r10.u32, c.r11.u32, c.r12.u32);
                Log("      r23=%08X r24=%08X r25=%08X r26=%08X r27=%08X r28=%08X r29=%08X r30=%08X r31=%08X", c.r23.u32, c.r24.u32, c.r25.u32, c.r26.u32, c.r27.u32, c.r28.u32, c.r29.u32, c.r30.u32, c.r31.u32);
            }
    }
    LogStackFrom(*e->ContextRecord, "T0", 40);   // guest backtrace of the crashing thread (symbolize.sh)
    DumpThreadTraces(24);
    TerminateProcess(GetCurrentProcess(), 3);
}

static void MissingFunc(PPCContext& ctx, uint8_t*)
{
    Log("*** call to unmapped guest function: ctr=%08X lr=%08llX r3=%08X", ctx.ctr.u32, (unsigned long long)ctx.lr, ctx.r3.u32);
    DumpThreadTraces(24);
    TerminateProcess(GetCurrentProcess(), 4);
}

// ITC_PROBE: at the watchdog dump, evaluate the guest's "resources loaded?" checks of the loading transition state (sub_8218E420) to see which one blocks it.
static void ProbeLoading()
{
    auto* c = new PPCContext; InitGuestThreadContext(*c, 0x40000);
    auto fn = [](uint32_t addr) { return *(PPCFunc**)GP(PPC_IMAGE_BASE + PPC_IMAGE_SIZE + uint32_t((addr - PPC_CODE_BASE) * 2)); };
    for (uint32_t n : { 0u, 9u, 10u })
    {
        c->r3.u64 = n; fn(0x8211BE98)(*c, g_base);
        Log("[probe] sub_8211BE98(%u) = %u", n, (uint32_t)c->r3.u32);
    }
    const uint32_t obj = rd32(0x826D8418);
    Log("[probe] list object %08X: count1=%u count2=%u", obj, rd32(obj + 216), rd32(obj + 404));
    for (uint32_t i = 0; i < rd32(obj + 216) && i < 64; i++) { const uint32_t p = rd32(obj + 32 + i * 4); Log("[probe]   list1[%u] ptr=%08X value=%08X", i, p, rd32(p)); }
    for (uint32_t i = 0; i < rd32(obj + 404) && i < 64; i++) { const uint32_t p = rd32(obj + 220 + i * 4); Log("[probe]   list2[%u] ptr=%08X value=%08X", i, p, rd32(p)); }
    c->r3.u64 = obj; fn(0x8218E2D8)(*c, g_base); Log("[probe] sub_8218E2D8 = %u", (uint32_t)c->r3.u32);
    c->r3.u64 = obj; fn(0x8218E320)(*c, g_base); Log("[probe] sub_8218E320 = %u", (uint32_t)c->r3.u32);
    Log("[probe] frame counter 826D841C = %u", rd32(0x826D841C));
}

extern "C" __declspec(dllexport) int HostRun(int argc, char** argv, PPCFuncMapping* mappings, const char** traceRing, unsigned* tracePos)
{
    timeBeginPeriod(1);   // Sleep(1) must really be ~1 ms (the GPU/IRQ threads poll)
    g_traceRing = traceRing; g_tracePosPtr = tracePos;
    const char* xexPath = argc > 1 ? argv[1] : "../extracted/default.xex";
    std::ifstream f(xexPath, std::ios::binary);
    if (!f) { Log("cannot open %s", xexPath); return 1; }
    std::vector<uint8_t> xex((std::istreambuf_iterator<char>(f)), {});
    Log("[boot] loaded %s (%zu bytes)", xexPath, xex.size());

    AddVectoredExceptionHandler(1, CrashHandler);
    if (const char* w = getenv("ITC_WATCH")) g_watchPage = (uint32_t)strtoul(w, nullptr, 16) & ~0xFFFu;

    Image image = Image::ParseImage(xex.data(), xex.size());
    Log("[boot] image base=%08zX size=%08X entry=%08zX", image.base, image.size, image.entry_point);
    if (image.base != PPC_IMAGE_BASE || image.size != PPC_IMAGE_SIZE)
        Log("[boot] WARNING: image base/size differ from recompiled config");

    if (const char* dump = getenv("ITC_DUMP_IMAGE"))
    {
        std::ofstream o(dump, std::ios::binary);
        o.write((const char*)image.data.get(), image.size);
        Log("[boot] dumped parsed image to %s", dump);
        return 0;
    }

    if (!GuestMemInit()) { Log("failed to reserve guest memory"); return 1; }
    GuestCommit((uint32_t)image.base, image.size);
    memcpy(GP((uint32_t)image.base), image.data.get(), image.size);

    // GPU register window (MMIO). Plain memory for now; a GPU emulation layer will own it later.
    GuestCommit(0x7FC80000, 0x80000);
    GuestCommit(0x7FEA0000, 0x10000);   // XMA audio decoder registers

    // Variable imports (kernel data such as XexExecutableModuleHandle): the loader normally writes the
    // variable's address into the import record, so do the same with zero-initialised guest memory.
    size_t numVars = 0;
    std::map<std::pair<std::string, uint32_t>, uint32_t> thunks;   // (library, ordinal) -> thunk VA
    for (const auto& imp : image.imports)
        if (imp.isFunction) thunks[{ imp.library, imp.ordinal }] = imp.address;
    for (const auto& imp : image.imports)
    {
        if (imp.isFunction) continue;
        // Function imports also have a type-0 record that receives the function's address.
        auto thunk = thunks.find({ imp.library, imp.ordinal });
        if (thunk != thunks.end()) { wr32(imp.address, thunk->second); continue; }
        const uint32_t var = GuestAlloc(0x1000, 0x1000);
        if (imp.name == "__imp__XexExecutableModuleHandle")
        {
            // Variable holds a pointer to the module's loader entry; +88 is the xex header pointer.
            const uint32_t module = GuestAlloc(0x1000, 0x1000);
            wr32(module + 88, (uint32_t)image.base);
            wr32(var, module);
        }
        wr32(imp.address, var);
        numVars++;
        Log("[boot] variable import %s (ordinal %X, %s) @%08X -> %08X", imp.name.c_str(), imp.ordinal, imp.library.c_str(), imp.address, var);
    }
    Log("[boot] %zu variable imports resolved", numVars);

    // Function lookup table used for indirect calls.
    const uint32_t tableBase = PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
    GuestCommit(tableBase, PPC_CODE_SIZE * 2);
    auto* table = (PPCFunc**)GP(tableBase);
    for (size_t i = 0; i < PPC_CODE_SIZE / 4; i++) table[i] = MissingFunc;
    size_t mapped = 0;
    for (auto* m = mappings; m->host; m++, mapped++)
        *(PPCFunc**)GP(tableBase + uint32_t((m->guest - PPC_CODE_BASE) * 2)) = m->host;
    Log("[boot] %zu functions mapped", mapped);

    if (g_watchPage)
    {
        DWORD old; VirtualProtect(g_base + g_watchPage, 0x1000, PAGE_READONLY, &old);
        Log("[watch] write-watching guest page %08X", g_watchPage);
    }
    if (const char* t = getenv("ITC_WATCHDOG"))
    {
        const DWORD ms = (DWORD)atoi(t) * 1000;
        CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            Sleep((DWORD)(uintptr_t)p);
            Log("*** WATCHDOG: still running, dumping state");
            DumpThreadStacks();
            GpuDebugDump();
            DumpThreadTraces(14);
            if (getenv("ITC_PROBE")) ProbeLoading();
            TerminateProcess(GetCurrentProcess(), 6);
        }, (LPVOID)(uintptr_t)ms, 0, nullptr);
    }

    ProfStart();
    RunGuestMain((uint32_t)image.entry_point);
    return 0;
}
