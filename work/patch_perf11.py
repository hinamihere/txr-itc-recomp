p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what, count=1):
    global s
    n = s.count(old)
    if n != count:
        raise SystemExit('count %d != %d: %s' % (n, count, what))
    s = s.replace(old, new)


# Command-processor thread accounting per stats window: CPU time of the thread, time blocked in Present, draw time (all per frame).
rep('''uint64_t g_profMap = 0, g_profDecode = 0, g_profResolve = 0, g_profSwap = 0, g_profDraw = 0, g_profHash = 0, g_profShader = 0;''',
    '''uint64_t g_profMap = 0, g_profDecode = 0, g_profResolve = 0, g_profSwap = 0, g_profDraw = 0, g_profHash = 0, g_profShader = 0, g_profPresent = 0;''', 'present prof')
rep('''    {
        const HRESULT phr = g_swap->Present(1, 0);''', '''    {
        const uint64_t tp = QpcNow();
        const HRESULT phr = g_swap->Present(1, 0);
        g_profPresent += QpcNow() - tp;''', 'present timing')
rep('''    Log("[stat] %.1fs: frames=%u draws+%llu (skipped +%llu) resolves+%llu decodes+%llu shaders+%llu | ms: draw=%.0f decode=%.0f hash=%.0f resolve=%.0f swap=%.0f",''',
    '''    {
        // The command-processor thread: its CPU time, the time it was blocked in Present and the time in draws, per presented frame of this window.
        static uint64_t lastCpu = 0, lastFrames = 0, lastPresent = 0, lastDrawT = 0;
        FILETIME c, e, k, u; GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
        const uint64_t cpu = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
        const uint64_t nf = g_frame > lastFrames ? g_frame - lastFrames : 1;
        Log("[stat]   cp thread: cpu %.1f ms/frame, blocked in Present %.1f ms/frame, draws %.1f ms/frame (%llu frames, %.0f%% of wall time busy)", (double)(cpu - lastCpu) * 1e-4 / nf, (g_profPresent - lastPresent) * ms / nf, (g_profDraw - lastDrawT) * ms / nf,
            (unsigned long long)nf, 100.0 * (double)(cpu - lastCpu) * 1e-7 / seconds);
        lastCpu = cpu; lastFrames = g_frame; lastPresent = g_profPresent; lastDrawT = g_profDraw;
    }
    Log("[stat] %.1fs: frames=%u draws+%llu (skipped +%llu) resolves+%llu decodes+%llu shaders+%llu | ms: draw=%.0f decode=%.0f hash=%.0f resolve=%.0f swap=%.0f",''', 'cp stat line')
open(p, 'w', encoding='utf-8').write(s)
print('patched part 11')
