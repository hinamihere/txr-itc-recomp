// Poor man's sampling profiler (ITC_PROF=1): per-thread CPU usage every 2 s plus call-stack samples of the busy threads.
// Functions are named from the COFF symbol tables of itc.exe / itc_host.dll (guest functions show up as sub_XXXXXXXX), the
// export tables of system DLLs, or module+offset. Stacks are unwound with the x64 unwind data while the thread is suspended
// (no allocation while a thread is suspended: the sampler must not touch locks the target may hold).
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include "guest.h"

namespace {
struct Sym { uint32_t rva; std::string name; };
struct Mod { std::string name; uintptr_t base = 0; size_t size = 0; std::vector<Sym> syms; bool loaded = false; };
std::vector<Mod> g_mods;

std::mutex g_nameLock;
std::map<DWORD, std::string> g_threadNames;

bool LoadCoffSymbols(Mod& m, const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    IMAGE_DOS_HEADER dos;
    if (fread(&dos, sizeof dos, 1, f) != 1 || dos.e_magic != IMAGE_DOS_SIGNATURE) { fclose(f); return false; }
    fseek(f, dos.e_lfanew, SEEK_SET);
    DWORD sig; IMAGE_FILE_HEADER fh;
    fread(&sig, 4, 1, f); fread(&fh, sizeof fh, 1, f);
    fseek(f, fh.SizeOfOptionalHeader, SEEK_CUR);
    std::vector<IMAGE_SECTION_HEADER> secs(fh.NumberOfSections);
    fread(secs.data(), sizeof(IMAGE_SECTION_HEADER), secs.size(), f);
    if (!fh.PointerToSymbolTable || !fh.NumberOfSymbols) { fclose(f); return false; }
    std::vector<uint8_t> symtab((size_t)fh.NumberOfSymbols * 18);
    fseek(f, fh.PointerToSymbolTable, SEEK_SET);
    fread(symtab.data(), 1, symtab.size(), f);
    uint32_t strSize = 0;
    fread(&strSize, 4, 1, f);
    std::vector<char> strtab(strSize > 4 ? strSize : 4, 0);
    if (strSize > 4) fread(strtab.data() + 4, 1, strSize - 4, f);
    fclose(f);
    for (uint32_t i = 0; i < fh.NumberOfSymbols; i++)
    {
        const uint8_t* s = &symtab[(size_t)i * 18];
        const uint32_t value = *(const uint32_t*)(s + 8);
        const int16_t section = *(const int16_t*)(s + 12);
        const uint16_t type = *(const uint16_t*)(s + 14);
        const uint8_t storage = s[16], aux = s[17];
        if (section > 0 && (type & 0xF0) == 0x20 && (storage == 2 || storage == 3) && section <= (int)secs.size())
        {
            std::string name;
            if (*(const uint32_t*)s == 0) { const uint32_t off = *(const uint32_t*)(s + 4); if (off < strtab.size()) name = &strtab[off]; }
            else name.assign((const char*)s, strnlen((const char*)s, 8));
            m.syms.push_back({ secs[section - 1].VirtualAddress + value, name });
        }
        i += aux;
    }
    std::sort(m.syms.begin(), m.syms.end(), [](const Sym& a, const Sym& b) { return a.rva < b.rva; });
    return !m.syms.empty();
}

// Export-table symbols for system DLLs (nearest preceding export).
void LoadExports(Mod& m)
{
    const uint8_t* b = (const uint8_t*)m.base;
    const auto* dos = (const IMAGE_DOS_HEADER*)b;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* nt = (const IMAGE_NT_HEADERS64*)(b + dos->e_lfanew);
    const auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.VirtualAddress || !dd.Size) return;
    const auto* ex = (const IMAGE_EXPORT_DIRECTORY*)(b + dd.VirtualAddress);
    const uint32_t* names = (const uint32_t*)(b + ex->AddressOfNames);
    const uint16_t* ords = (const uint16_t*)(b + ex->AddressOfNameOrdinals);
    const uint32_t* funcs = (const uint32_t*)(b + ex->AddressOfFunctions);
    for (uint32_t i = 0; i < ex->NumberOfNames; i++)
    {
        const uint32_t rva = funcs[ords[i]];
        if (rva >= dd.VirtualAddress && rva < dd.VirtualAddress + dd.Size) continue;   // forwarder
        m.syms.push_back({ rva, (const char*)(b + names[i]) });
    }
    std::sort(m.syms.begin(), m.syms.end(), [](const Sym& a, const Sym& b2) { return a.rva < b2.rva; });
}

Mod* FindMod(uintptr_t addr)
{
    for (auto& m : g_mods) if (addr >= m.base && addr < m.base + m.size) return &m;
    HMODULE h = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &h) || !h) return nullptr;
    char path[MAX_PATH] = {};
    GetModuleFileNameA(h, path, MAX_PATH);
    const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
    MODULEINFO mi = {};
    GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof mi);
    Mod m; m.name = base; m.base = (uintptr_t)h; m.size = mi.SizeOfImage;
    LoadExports(m);
    g_mods.push_back(std::move(m));
    return &g_mods.back();
}

void LoadModule(const char* modName, const char* label)
{
    HMODULE h = GetModuleHandleA(modName);
    if (!h) return;
    char path[MAX_PATH]; GetModuleFileNameA(h, path, MAX_PATH);
    MODULEINFO mi = {};
    GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof mi);
    Mod m; m.name = label; m.base = (uintptr_t)h; m.size = mi.SizeOfImage;
    if (LoadCoffSymbols(m, path)) Log("[prof] %s: %zu symbols", m.name.c_str(), m.syms.size());
    g_mods.push_back(std::move(m));
}

// Function-level name for an address (no offset), cached.
std::string FuncName(uintptr_t pc)
{
    static std::unordered_map<uintptr_t, std::string> cache;
    // Cache by 16-byte bucket of the function start when unwind info exists, else by exact pc.
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
    const uintptr_t key = rf ? (uintptr_t)(imageBase + rf->BeginAddress) : pc;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    char buf[300];
    std::string out;
    if (Mod* m = FindMod(key))
    {
        const uint32_t rva = (uint32_t)(key - m->base);
        auto s = std::upper_bound(m->syms.begin(), m->syms.end(), rva, [](uint32_t v, const Sym& y) { return v < y.rva; });
        if (s != m->syms.begin() && rva - (--s)->rva < 0x4000)
        {
            if (rva == s->rva) snprintf(buf, sizeof buf, "%s!%s", m->name.c_str(), s->name.c_str());
            else snprintf(buf, sizeof buf, "%s!%s+0x%X", m->name.c_str(), s->name.c_str(), rva - s->rva);
        }
        else snprintf(buf, sizeof buf, "%s+0x%X", m->name.c_str(), rva);
        out = buf;
    }
    else { snprintf(buf, sizeof buf, "?%llX", (unsigned long long)key); out = buf; }
    cache[key] = out;
    return out;
}

uint64_t FileTimeTo100ns(const FILETIME& ft) { return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }

constexpr int kMaxFrames = 14;
struct Sample { DWORD tid; int n; uintptr_t pc[kMaxFrames]; };

// Walks the stack of a suspended thread (no allocation).
int Unwind(CONTEXT c, uintptr_t* out, int maxFrames)
{
    int n = 0;
    while (n < maxFrames && c.Rip)
    {
        out[n++] = (uintptr_t)c.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (!rf)
        {
            if (!c.Rsp || (c.Rsp & 7)) break;
            c.Rip = *(const DWORD64*)c.Rsp; c.Rsp += 8;
        }
        else
        {
            PVOID handlerData = nullptr; DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, rf, &c, &handlerData, &establisher, nullptr);
        }
    }
    return n;
}

DWORD WINAPI ProfThread(LPVOID)
{
    LoadModule(nullptr, "exe");
    LoadModule("itc_host.dll", "host");
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    struct TState { uint64_t cpu = 0; double pct = 100.0; bool seen = false; };
    std::map<DWORD, TState> threads;
    std::vector<Sample> samples;
    samples.reserve(40000);
    LARGE_INTEGER qf, t0, now; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&t0);
    for (;;)
    {
        std::vector<DWORD> tids;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 te = { sizeof te };
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) tids.push_back(te.th32ThreadID);
            CloseHandle(snap);
        }
        std::map<DWORD, HANDLE> handles;
        for (DWORD t : tids)
        {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, t);
            if (h) handles[t] = h;
        }
        samples.clear();
        const int64_t winStart = t0.QuadPart;
        for (;;)
        {
            for (auto& kv : handles)
            {
                const TState& st = threads[kv.first];
                if (st.pct < 3.0) continue;   // only sample threads that were busy in the previous window
                if (samples.size() >= samples.capacity()) break;
                if (SuspendThread(kv.second) == (DWORD)-1) continue;
                CONTEXT c = {}; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (GetThreadContext(kv.second, &c))
                {
                    samples.push_back(Sample{ kv.first, 0, {} });
                    samples.back().n = Unwind(c, samples.back().pc, kMaxFrames);
                }
                ResumeThread(kv.second);
            }
            Sleep(1);
            QueryPerformanceCounter(&now);
            if (now.QuadPart - winStart >= qf.QuadPart * 2) break;
        }
        const double secs = (double)(now.QuadPart - winStart) / qf.QuadPart;
        t0 = now;
        // Per-thread CPU.
        std::vector<std::pair<double, DWORD>> order;
        for (auto& kv : handles)
        {
            FILETIME c, e, k, u;
            if (!GetThreadTimes(kv.second, &c, &e, &k, &u)) continue;
            const uint64_t total = FileTimeTo100ns(k) + FileTimeTo100ns(u);
            auto& st = threads[kv.first];
            st.pct = st.seen ? (double)(total - st.cpu) / (secs * 1e7) * 100.0 : 100.0;
            st.seen = true;
            st.cpu = total;
            order.push_back({ st.pct, kv.first });
        }
        std::sort(order.rbegin(), order.rend());
        Log("[prof] ---- window %.1fs, %zu samples ----", secs, samples.size());
        int shown = 0;
        for (auto& o : order)
        {
            if (o.first < 2.0 || shown++ >= 7) break;
            std::string nm;
            { std::lock_guard<std::mutex> l(g_nameLock); auto it = g_threadNames.find(o.second); if (it != g_threadNames.end()) nm = it->second; }
            uint32_t total = 0;
            std::map<std::string, uint32_t> self, incl, stacks;
            for (auto& s : samples)
            {
                if (s.tid != o.second || s.n == 0) continue;
                total++;
                self[FuncName(s.pc[0])]++;
                std::set<std::string> seen;
                std::string sig;
                for (int i = 0; i < s.n; i++)
                {
                    const std::string f = FuncName(s.pc[i]);
                    if (seen.insert(f).second) incl[f]++;
                    if (i < 6) { if (i) sig += " < "; sig += f; }
                }
                stacks[sig]++;
            }
            Log("[prof] thread %u (%s): cpu %.0f%%, %u samples", o.second, nm.c_str(), o.first, total);
            auto dump = [&](const char* title, std::map<std::string, uint32_t>& m, size_t lim)
            {
                std::vector<std::pair<uint32_t, std::string>> top;
                for (auto& h : m) top.push_back({ h.second, h.first });
                std::sort(top.rbegin(), top.rend());
                for (size_t i = 0; i < top.size() && i < lim; i++)
                    Log("[prof]   %s %5.1f%%  %s", title, 100.0 * top[i].first / std::max(1u, total), top[i].second.c_str());
            };
            dump("self ", self, 5);
            dump("incl ", incl, 14);
            dump("stack", stacks, 2);
        }
        for (auto& kv : handles) CloseHandle(kv.second);
    }
    return 0;
}
}

void ProfNameThread(const char* name)
{
    std::lock_guard<std::mutex> l(g_nameLock);
    g_threadNames[GetCurrentThreadId()] = name;
}

void ProfNameThreadId(unsigned tid, const char* name)
{
    std::lock_guard<std::mutex> l(g_nameLock);
    g_threadNames[tid] = name;
}

void ProfStart()
{
    if (!getenv("ITC_PROF")) return;
    CreateThread(nullptr, 0, ProfThread, nullptr, 0, nullptr);
}
