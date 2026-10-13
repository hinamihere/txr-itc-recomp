// Compatibility shims so clang-targeted generated PPC code builds with GCC.
#pragma once
#include <stdint.h>
#ifndef __clang__
#define __builtin_assume(x) do { if (!(x)) __builtin_unreachable(); } while (0)
static inline uint8_t  __builtin_rotateleft8 (uint8_t  x, uint8_t n) { n &= 7;  return n ? (uint8_t)((x << n) | (x >> (8 - n)))   : x; }
static inline uint16_t __builtin_rotateleft16(uint16_t x, uint16_t n){ n &= 15; return n ? (uint16_t)((x << n) | (x >> (16 - n))) : x; }
static inline uint32_t __builtin_rotateleft32(uint32_t x, uint32_t n){ n &= 31; return n ? (x << n) | (x >> (32 - n)) : x; }
static inline uint64_t __builtin_rotateleft64(uint64_t x, uint64_t n){ n &= 63; return n ? (x << n) | (x >> (64 - n)) : x; }
#define __builtin_debugtrap() __builtin_trap()
#endif

#if !defined(__clang__)
#include <x86intrin.h>
#endif

// Optional guest call tracing (bring-up aid): every generated function records its name in a per-thread ring
// kept in that thread's PCR block (guest memory at r13 + 0x800, 64 entries; counter at r13 + 0x7F8).
#ifdef ITC_TRACE
#define PPC_FUNC_PROLOGUE() do { uint64_t* t__ = (uint64_t*)(base + ctx.r13.u32 + 0x800); uint64_t i__ = t__[-1]++; t__[i__ & 63] = (uint64_t)__func__; } while (0)
#endif
