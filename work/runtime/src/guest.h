#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "ppc_config.h"
#include "ppc_context.h"
#include "prof.h"

extern uint8_t* g_base;   // host pointer to guest address 0

inline uint8_t* GP(uint32_t a) { return g_base + a; }
inline uint8_t  rd8 (uint32_t a) { return *(uint8_t*)(g_base + a); }
inline uint16_t rd16(uint32_t a) { return __builtin_bswap16(*(uint16_t*)(g_base + a)); }
inline uint32_t rd32(uint32_t a) { return __builtin_bswap32(*(uint32_t*)(g_base + a)); }
inline uint64_t rd64(uint32_t a) { return __builtin_bswap64(*(uint64_t*)(g_base + a)); }
inline void wr8 (uint32_t a, uint8_t  v) { *(uint8_t*)(g_base + a) = v; }
inline void wr16(uint32_t a, uint16_t v) { *(uint16_t*)(g_base + a) = __builtin_bswap16(v); }
inline void wr32(uint32_t a, uint32_t v) { *(uint32_t*)(g_base + a) = __builtin_bswap32(v); }
inline void wr64(uint32_t a, uint64_t v) { *(uint64_t*)(g_base + a) = __builtin_bswap64(v); }

#define HOST_FUNC(name) PPC_FUNC(__imp__##name)

// Logging
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void DumpTraceShort(int n);

// Guest memory management (memory.cpp-ish, lives in kernel.cpp)
bool     GuestMemInit();
uint32_t GuestAlloc(uint32_t size, uint32_t align = 0x10000);   // virtual heap, committed
uint32_t GuestFree(uint32_t addr);                              // releases a GuestAlloc block (decommits it); returns its size
uint32_t GuestAllocPhysical(uint32_t size, uint32_t align);     // 0xA0000000 region, committed
uint32_t GuestFreePhysical(uint32_t addr);
uint32_t GuestPhysicalBlockSize(uint32_t addr);
void     GuestMemStats(char* out, size_t n);
bool     GuestCommit(uint32_t addr, uint32_t size);
size_t   GuestCommittedBytes(uint32_t addr, size_t want);       // bytes from addr that are committed, up to want (O(pages), no syscall)
void     GuestDecommit(uint32_t addr, uint32_t size);

// Threads
struct GuestThreadParams { uint32_t entry, arg, startup; };
void RunGuestMain(uint32_t entry);
void InitGuestThreadContext(PPCContext& ctx, uint32_t stackSize, int cpu = -1);   // cpu < 0: assign automatically

// APC queue (per guest thread): I/O completion routines run when the thread enters an alertable wait.
void QueueApc(uint32_t routine, uint32_t arg1, uint32_t arg2);
bool RunPendingApcs(PPCContext& ctx);
