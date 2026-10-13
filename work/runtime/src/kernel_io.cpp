// File I/O (game disc + save area mapped onto host directories) and string conversion helpers.
#include "guest.h"
#include <windows.h>
#include <algorithm>
#include <memory>
#include <string>

struct KObj
{
    enum Type { Event, Semaphore, Thread, Timer, File } type;
    HANDLE h = nullptr;
    HANDLE resume = nullptr;
    bool owned = true;
    uint32_t threadId = 0;
    uint32_t guestObject = 0;
};
uint32_t KNewHandle(std::shared_ptr<KObj> o);
std::shared_ptr<KObj> KFindHandle(uint32_t h);

enum : uint32_t {
    NTS_SUCCESS = 0, NTS_INVALID_HANDLE = 0xC0000008, NTS_END_OF_FILE = 0xC0000011,
    NTS_OBJECT_NAME_NOT_FOUND = 0xC0000034, NTS_UNSUCCESSFUL = 0xC0000001, NTS_NOT_IMPLEMENTED = 0xC0000002,
};

std::string g_gameDir = "C:/itc-work/extracted";   // contents of the game disc (default.xex, DATA/...)
std::string g_saveDir = "C:/itc-work/save";        // writable storage (HDD, cache, content)

static std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// Map a guest path ("game:\DATA\x", "\Device\Cdrom0\x", "d:\x", ...) to a host path. Returns "" if unknown.
static std::string ResolvePath(const std::string& guestPath)
{
    std::string p = guestPath;
    std::replace(p.begin(), p.end(), '\\', '/');
    const std::string low = Lower(p);
    auto strip = [&](const char* prefix) -> bool {
        const size_t n = strlen(prefix);
        if (low.compare(0, n, prefix) == 0) { p = p.substr(n); return true; }
        return false;
    };
    std::string root;
    if (strip("game:/") || strip("d:/") || strip("/device/cdrom0/") || strip("\?\?/game:/")) root = g_gameDir;
    else if (strip("/device/harddisk0/partition1/") || strip("hdd:/") || strip("cache:/") || strip("e:/") || strip("save:/") || strip("cache0:/") || strip("cache1:/")) root = g_saveDir;
    else return "";
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    return root + "/" + p;
}

static std::string ReadObjectName(uint32_t pObjAttr)
{
    // X_OBJECT_ATTRIBUTES { root, PANSI_STRING name, attributes }; X_ANSI_STRING { u16 len, u16 max, u32 buf }
    const uint32_t pName = rd32(pObjAttr + 4);
    if (!pName) return "";
    const uint16_t len = rd16(pName);
    const uint32_t buf = rd32(pName + 4);
    return std::string((const char*)GP(buf), len);
}

static bool g_ioLog = getenv("ITC_IO_LOG") != nullptr;

static void SetIosb(uint32_t pIosb, uint32_t status, uint32_t info)
{
    if (!pIosb) return;
    wr32(pIosb, status);
    wr32(pIosb + 4, info);
}

static uint32_t OpenFileCommon(PPCContext& ctx, uint32_t pHandle, uint32_t pObjAttr, uint32_t pIosb, uint32_t access, uint32_t disposition)
{
    const std::string name = ReadObjectName(pObjAttr);
    const std::string host = ResolvePath(name);
    // Video playback (software WMV decode + threads) is not supported yet: report movies as missing so the game skips them.
    static const bool skipMovies = getenv("ITC_SKIP_MOVIES") != nullptr;
    if (skipMovies && Lower(name).size() > 4 && Lower(name).compare(Lower(name).size() - 4, 4, ".wmv") == 0)
    {
        Log("[io] skipping movie '%s'", name.c_str());
        SetIosb(pIosb, NTS_OBJECT_NAME_NOT_FOUND, 0);
        return NTS_OBJECT_NAME_NOT_FOUND;
    }
    if (host.empty())
    {
        Log("[io] open: unmapped path '%s'", name.c_str());
        SetIosb(pIosb, NTS_OBJECT_NAME_NOT_FOUND, 0);
        return NTS_OBJECT_NAME_NOT_FOUND;
    }
    const bool write = (access & 0x40000000) != 0 || (access & 0x2) != 0;
    // disposition: 0 supersede, 1 open, 2 create, 3 open_if, 4 overwrite, 5 overwrite_if
    DWORD cd = OPEN_EXISTING;
    switch (disposition) { case 0: cd = CREATE_ALWAYS; break; case 1: cd = OPEN_EXISTING; break; case 2: cd = CREATE_NEW; break;
                           case 3: cd = OPEN_ALWAYS; break; case 4: cd = TRUNCATE_EXISTING; break; case 5: cd = CREATE_ALWAYS; break; }
    if (host.compare(0, g_gameDir.size(), g_gameDir) == 0) cd = OPEN_EXISTING;   // disc is read-only
    HANDLE h = CreateFileA(host.c_str(), write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, cd, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        Log("[io] open failed: '%s' -> '%s' (err %lu)", name.c_str(), host.c_str(), GetLastError());
        SetIosb(pIosb, NTS_OBJECT_NAME_NOT_FOUND, 0);
        return NTS_OBJECT_NAME_NOT_FOUND;
    }
    auto o = std::make_shared<KObj>();
    o->type = KObj::File;
    o->h = h;
    wr32(pHandle, KNewHandle(o));
    SetIosb(pIosb, NTS_SUCCESS, 1 /* FILE_OPENED */);
    Log("[io] open '%s' -> %08X", name.c_str(), rd32(pHandle));
    return NTS_SUCCESS;
}

HOST_FUNC(NtCreateFile)
{
    // (PHANDLE, access, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER alloc, fileAttrs, shareAccess, createDisposition, ...)
    ctx.r3.u64 = OpenFileCommon(ctx, ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, ctx.r4.u32, ctx.r10.u32);
}

HOST_FUNC(NtOpenFile)
{
    // (PHANDLE, access, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, shareAccess, openOptions)
    ctx.r3.u64 = OpenFileCommon(ctx, ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, ctx.r4.u32, 1);
}

HOST_FUNC(NtReadFile)
{
    // (HANDLE, event, apcRoutine, apcContext, PIO_STATUS_BLOCK, buffer, length, PLARGE_INTEGER byteOffset)
    auto o = KFindHandle(ctx.r3.u32);
    if (!o || o->type != KObj::File) { ctx.r3.u64 = NTS_INVALID_HANDLE; return; }
    const uint32_t pIosb = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, pOffset = ctx.r10.u32;
    OVERLAPPED ov{};
    OVERLAPPED* pov = nullptr;
    if (pOffset)
    {
        const int64_t off = (int64_t)rd64(pOffset);
        if (off >= 0) { ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32); pov = &ov; }
    }
    DWORD got = 0;
    BOOL ok = ReadFile(o->h, GP(buffer), length, &got, pov);
    if (g_ioLog) Log("[io] T read h=%08X len=%X off=%lld -> got=%lu ok=%d buf=%08X apc=%08X ev=%08X", ctx.r3.u32, length, pOffset ? (long long)rd64(pOffset) : -1LL, got, (int)ok, buffer, ctx.r5.u32, ctx.r4.u32);
    if (pov && ok) { LARGE_INTEGER pos; pos.QuadPart = ((int64_t)ov.OffsetHigh << 32 | ov.Offset) + got; SetFilePointerEx(o->h, pos, nullptr, FILE_BEGIN); }
    uint32_t status = !ok ? NTS_UNSUCCESSFUL : (got == 0 && length ? NTS_END_OF_FILE : NTS_SUCCESS);
    SetIosb(pIosb, status, got);
    if (ctx.r4.u32) { auto ev = KFindHandle(ctx.r4.u32); if (ev) SetEvent(ev->h); }
    if (ctx.r5.u32 & ~3u) QueueApc(ctx.r5.u32 & ~3u, ctx.r6.u32, pIosb);
    ctx.r3.u64 = status;
}

HOST_FUNC(NtWriteFile)
{
    auto o = KFindHandle(ctx.r3.u32);
    if (!o || o->type != KObj::File) { ctx.r3.u64 = NTS_INVALID_HANDLE; return; }
    const uint32_t pIosb = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, pOffset = ctx.r10.u32;
    if (pOffset)
    {
        const int64_t off = (int64_t)rd64(pOffset);
        if (off >= 0) { LARGE_INTEGER pos; pos.QuadPart = off; SetFilePointerEx(o->h, pos, nullptr, FILE_BEGIN); }
    }
    DWORD wrote = 0;
    BOOL ok = WriteFile(o->h, GP(buffer), length, &wrote, nullptr);
    SetIosb(pIosb, ok ? NTS_SUCCESS : NTS_UNSUCCESSFUL, wrote);
    if (ctx.r4.u32) { auto ev = KFindHandle(ctx.r4.u32); if (ev) SetEvent(ev->h); }
    if (ctx.r5.u32 & ~3u) QueueApc(ctx.r5.u32 & ~3u, ctx.r6.u32, pIosb);
    ctx.r3.u64 = ok ? NTS_SUCCESS : NTS_UNSUCCESSFUL;
}

HOST_FUNC(NtQueryInformationFile)
{
    // (HANDLE, PIO_STATUS_BLOCK, PVOID info, ULONG length, ULONG infoClass)
    auto o = KFindHandle(ctx.r3.u32);
    if (!o || o->type != KObj::File) { ctx.r3.u64 = NTS_INVALID_HANDLE; return; }
    const uint32_t pIosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
    LARGE_INTEGER size{}; GetFileSizeEx(o->h, &size);
    uint32_t written = 0;
    if (g_ioLog) Log("[io] NtQueryInformationFile h=%08X class=%u len=%u", ctx.r3.u32, cls, ctx.r6.u32);
    switch (cls)
    {
    case 5:   // FileStandardInformation
        wr64(info + 0, (uint64_t)size.QuadPart); wr64(info + 8, (uint64_t)size.QuadPart);
        wr32(info + 16, 1); wr8(info + 20, 0); wr8(info + 21, 0); written = 24; break;
    case 14:  // FilePositionInformation
    {
        LARGE_INTEGER zero{}, pos{}; SetFilePointerEx(o->h, zero, &pos, FILE_CURRENT);
        wr64(info, (uint64_t)pos.QuadPart); written = 8; break;
    }
    case 34:  // FileNetworkOpenInformation
        memset(GP(info), 0, 56);
        wr64(info + 32, (uint64_t)size.QuadPart); wr64(info + 40, (uint64_t)size.QuadPart); wr32(info + 48, 0x80 /* NORMAL */);
        written = 56; break;
    default:
        Log("[io] NtQueryInformationFile: unhandled class %u", cls);
        SetIosb(pIosb, NTS_NOT_IMPLEMENTED, 0); ctx.r3.u64 = NTS_NOT_IMPLEMENTED; return;
    }
    SetIosb(pIosb, NTS_SUCCESS, written);
    ctx.r3.u64 = NTS_SUCCESS;
}

HOST_FUNC(NtSetInformationFile)
{
    // (HANDLE, PIO_STATUS_BLOCK, PVOID info, ULONG length, ULONG infoClass)
    auto o = KFindHandle(ctx.r3.u32);
    if (!o || o->type != KObj::File) { ctx.r3.u64 = NTS_INVALID_HANDLE; return; }
    if (ctx.r7.u32 == 14)   // FilePositionInformation
    {
        LARGE_INTEGER pos; pos.QuadPart = (int64_t)rd64(ctx.r5.u32);
        SetFilePointerEx(o->h, pos, nullptr, FILE_BEGIN);
    }
    SetIosb(ctx.r4.u32, NTS_SUCCESS, 0);
    ctx.r3.u64 = NTS_SUCCESS;
}

HOST_FUNC(NtQueryFullAttributesFile)
{
    // (POBJECT_ATTRIBUTES, PFILE_NETWORK_OPEN_INFORMATION out)
    const std::string host = ResolvePath(ReadObjectName(ctx.r3.u32));
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (host.empty() || !GetFileAttributesExA(host.c_str(), GetFileExInfoStandard, &d)) { ctx.r3.u64 = NTS_OBJECT_NAME_NOT_FOUND; return; }
    const uint32_t out = ctx.r4.u32;
    memset(GP(out), 0, 56);
    const uint64_t size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    wr64(out + 32, size); wr64(out + 40, size);
    wr32(out + 48, (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x80);
    ctx.r3.u64 = NTS_SUCCESS;
}

HOST_FUNC(NtQueryVolumeInformationFile)
{
    SetIosb(ctx.r4.u32, NTS_NOT_IMPLEMENTED, 0);
    ctx.r3.u64 = NTS_NOT_IMPLEMENTED;
}

HOST_FUNC(NtFlushBuffersFile) { ctx.r3.u64 = NTS_SUCCESS; }

HOST_FUNC(RtlNtStatusToDosError)
{
    const uint32_t s = ctx.r3.u32;
    ctx.r3.u64 = s == 0 ? 0 : s == NTS_OBJECT_NAME_NOT_FOUND ? 2 : s == NTS_END_OF_FILE ? 38 : s == NTS_INVALID_HANDLE ? 6 : 31;
}

// --- Strings -------------------------------------------------------------------------------------------
HOST_FUNC(RtlMultiByteToUnicodeN)
{
    // (PWSTR dst, ULONG maxBytes, PULONG outBytes, PCSTR src, ULONG srcBytes)
    const uint32_t dst = ctx.r3.u32, maxBytes = ctx.r4.u32, pOut = ctx.r5.u32, src = ctx.r6.u32, srcBytes = ctx.r7.u32;
    const uint32_t n = std::min(srcBytes, maxBytes / 2);
    for (uint32_t i = 0; i < n; i++) wr16(dst + i * 2, rd8(src + i));
    if (pOut) wr32(pOut, n * 2);
    ctx.r3.u64 = NTS_SUCCESS;
}

HOST_FUNC(RtlUnicodeToMultiByteN)
{
    // (PCHAR dst, ULONG maxBytes, PULONG outBytes, PCWSTR src, ULONG srcBytes)
    const uint32_t dst = ctx.r3.u32, maxBytes = ctx.r4.u32, pOut = ctx.r5.u32, src = ctx.r6.u32, srcBytes = ctx.r7.u32;
    const uint32_t n = std::min(srcBytes / 2, maxBytes);
    for (uint32_t i = 0; i < n; i++) { const uint16_t c = rd16(src + i * 2); wr8(dst + i, c < 0x100 ? (uint8_t)c : '?'); }
    if (pOut) wr32(pOut, n);
    ctx.r3.u64 = NTS_SUCCESS;
}

HOST_FUNC(RtlInitUnicodeString)
{
    // (PUNICODE_STRING dst, PCWSTR src)
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    uint32_t len = 0;
    if (src) while (rd16(src + len * 2)) len++;
    wr16(dst, (uint16_t)(len * 2)); wr16(dst + 2, (uint16_t)(src ? len * 2 + 2 : 0)); wr32(dst + 4, src);
}

// --- Memory helpers ---------------------------------------------------------------------------------------
HOST_FUNC(RtlFillMemoryUlong)
{
    // (PVOID dest, SIZE_T length, ULONG pattern): fill with the 32-bit pattern (stored big-endian)
    const uint32_t dest = ctx.r3.u32, length = ctx.r4.u32, pattern = ctx.r5.u32;
    for (uint32_t i = 0; i + 4 <= length; i += 4) wr32(dest + i, pattern);
}

HOST_FUNC(RtlCompareMemoryUlong)
{
    // (PVOID src, SIZE_T length, ULONG pattern) -> number of leading bytes that match the pattern
    const uint32_t src = ctx.r3.u32, length = ctx.r4.u32, pattern = ctx.r5.u32;
    uint32_t i = 0;
    while (i + 4 <= length && rd32(src + i) == pattern) i += 4;
    ctx.r3.u64 = i;
}
