p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what, count=1):
    global s
    n = s.count(old)
    if n != count:
        raise SystemExit('count %d != %d: %s' % (n, count, what))
    s = s.replace(old, new)


# ---- environment-map pass throttle ----------------------------------------------------------------------------------------------
rep('''uint64_t g_velocitySkipped = 0;''', '''uint64_t g_velocitySkipped = 0;
// The 256x256 environment-map pass (2x MSAA, ~465 draws: a third of a race frame) is re-rendered every kEnvEvery-th frame only; in between its draws, clears and resolves are
// dropped, so the GPU textures resolved by the last real pass keep serving the reflections. Only race-sized frames are throttled. ITC_ENV_EVERY=1 renders it every frame.
const uint32_t g_envEvery = getenv("ITC_ENV_EVERY") ? (uint32_t)std::max(1, atoi(getenv("ITC_ENV_EVERY"))) : (g_noPerf ? 1u : 2u);
bool g_envRenderedOnce = false;
uint32_t g_envFrameDecided = 0xFFFFFFFFu; bool g_envFrameSkip = false;
uint64_t g_envDrawsSkipped = 0, g_envResolvesSkipped = 0;
uint64_t g_frameDrawsStart = 0, g_lastFrameDraws = 0;   // draws executed per frame (g_drawCount at the start of the frame / total of the previous one)
bool IsEnvPassState()
{
    return XReg(xr::PA_SC_WINDOW_SCISSOR_BR) == 0x01000100u && ((XReg(xr::RB_SURFACE_INFO) >> 16) & 3) == 2;
}
// True when the environment pass of this frame is dropped (decided once per frame, at its first draw / resolve).
bool EnvPassSkipped()
{
    if (g_envEvery <= 1) return false;
    if (g_envFrameDecided != g_frame)
    {
        g_envFrameDecided = g_frame;
        g_envFrameSkip = g_envRenderedOnce && g_lastFrameDraws > 1200 && (g_frame % g_envEvery) != 0;
    }
    return g_envFrameSkip;
}''', 'env decl')

rep('''    if (!g_curVS.valid || !g_curPS.valid) { g_skippedDraws++; return; }
    if (mode == 4 && !g_velocityNeeded''', '''    if (!g_curVS.valid || !g_curPS.valid) { g_skippedDraws++; return; }
    if (g_envEvery > 1 && IsEnvPassState() && EnvPassSkipped()) { g_envDrawsSkipped++; return; }
    if (mode == 4 && !g_velocityNeeded''', 'env draw skip')

rep('''    ProfScope profScope(g_profResolve);
    g_resolveCount++;
    if (g_gpuMode >= 1) GpuTimingBeforeDraw(kPassResolve, 0);''', '''    ProfScope profScope(g_profResolve);
    g_resolveCount++;
    if (g_envEvery > 1 && IsEnvPassState() && EnvPassSkipped()) { g_envResolvesSkipped++; return; }   // the environment pass of this frame is not rendered: keep the old maps
    if (g_gpuMode >= 1) GpuTimingBeforeDraw(kPassResolve, 0);''', 'env resolve skip')

# the pass counts as rendered once a frame went through it completely; frame draw counts
rep('''    g_frame++;
    if (g_rdoc && g_frame == RdocFrame())''', '''    g_lastFrameDraws = g_drawCount - g_frameDrawsStart; g_frameDrawsStart = g_drawCount;
    if (g_envFrameDecided == g_frame && !g_envFrameSkip && g_lastFrameDraws > 1200) g_envRenderedOnce = true;   // a race-sized frame went through the environment pass
    g_frame++;
    if (g_rdoc && g_frame == RdocFrame())''', 'swap hook')

rep('''        Log("[stat]   index cache: hits +%llu misses +%llu resets +%llu (list=%zu), motion-vector pass draws skipped +%llu%s",''', '''        { static uint64_t le = 0, lre = 0; Log("[stat]   environment pass: draws skipped +%llu, resolves skipped +%llu (every %u frames)", (unsigned long long)(g_envDrawsSkipped - le), (unsigned long long)(g_envResolvesSkipped - lre), g_envEvery); le = g_envDrawsSkipped; lre = g_envResolvesSkipped; }
        Log("[stat]   index cache: hits +%llu misses +%llu resets +%llu (list=%zu), motion-vector pass draws skipped +%llu%s",''', 'stat')
open(p, 'w', encoding='utf-8').write(s)
print('patched part 4')
