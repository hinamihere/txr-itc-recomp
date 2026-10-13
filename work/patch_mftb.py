import glob, re
# mftb (time base register, 50 MHz on the Xbox 360) is emitted as __rdtsc() by XenonRecomp; the host TSC runs at GHz, so timebase-based timeouts in the game were
# ~60x too short (KeQueryPerformanceFrequency reports 50 MHz). The generated call sites are redirected to ItcTimebase() (defined inline in the same files, derived
# from QueryPerformanceCounter). Only the 4 affected translation units recompile. Re-run after every regeneration of out/ppc_recomp.*.cpp.
DEF = '''#include "ppc_recomp_shared.h"
#ifndef ITC_TIMEBASE_DEFINED
#define ITC_TIMEBASE_DEFINED
// Xbox 360 time base (mftb): 50 MHz, derived from the host performance counter (patch_mftb.py).
extern "C" __declspec(dllimport) int __stdcall QueryPerformanceCounter(long long* out);
extern "C" __declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long* out);
static inline uint64_t ItcTimebase(void)
{
    static double scale = 0.0;
    long long t = 0;
    if (scale == 0.0) { long long f = 1; QueryPerformanceFrequency(&f); scale = 50000000.0 / (double)(f > 0 ? f : 1); }
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t * scale);
}
#endif
'''
n = 0
for p in glob.glob('C:/itc-work/out/ppc_recomp.*.cpp'):
    s = open(p, encoding='utf-8').read()
    if '__rdtsc()' not in s:
        continue
    s, k = re.subn(r'\b__rdtsc\(\)', 'ItcTimebase()', s)
    if 'ITC_TIMEBASE_DEFINED' not in s:
        assert s.count('#include "ppc_recomp_shared.h"\n') == 1, p
        s = s.replace('#include "ppc_recomp_shared.h"\n', DEF, 1)
    open(p, 'w', encoding='utf-8').write(s)
    n += k
    print(p, k)
print('replaced', n)
