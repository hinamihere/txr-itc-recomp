p = 'C:/itc-work/NOTES.md'
s = open(p, encoding='utf-8').read()


def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)


rep('Last updated: 2026-10-05 (night, race view renders correctly).', 'Last updated: 2026-10-05 (late evening: performance work, race ~14 fps -> ~28 fps in the scripted test).', 'date')

perf = r'''## Performance (2026-10-05 evening) - the race was CPU-bound on the command-processor (CP) thread
Measured facts: a race frame is ~2000 draws (shadow ~80, 256x256 env pass ~465, 512x288 motion-vector pass ~423, main scene ~950, bloom/HUD); the CP thread spent 35 us per draw
(index conversion, vertex-buffer / texture re-validation against guest memory, ~30 D3D calls, constant uploads). GPU time (timestamp queries) was only ~20 ms/frame on the
Iris Xe, so the GPU was NOT the limit. The game is vblank-paced: the guest waits for 2 simulated vblanks (60 Hz timer thread in gpu.cpp) per frame => 30 fps cap, 20/15 fps when a frame is late.
Done (all in xgpu_d3d.cpp; `ITC_NO_PERF=1` turns them all off, individual switches ITC_NO_STATEFILTER / ITC_NO_IDXCACHE / ITC_NO_CBDEDUP / ITC_NO_TEXMEMO / ITC_RECHECK_PERIOD=1 / ITC_ENV_EVERY=1 / ITC_KEEP_VELOCITY=1):
- **FPS overlay** (top-left, F3 toggles, ITC_NO_OVERLAY=1 hides) + fps in the title bar: fps, frame ms, worst frame, draws/frame, render-thread cpu%. GDI font atlas, one batched draw (`OverlayDraw`).
- GPU timing: always-on frame span (disjoint+timestamp queries); `ITC_GPU_TIMING=1` per render pass, `=2` per draw (busy time per pixel shader), printed in the [stat] lines.
- **Index cache** (`GetIndexList`): converted index lists live in one 8M-entry structured buffer, keyed by (guest address, count, format, prim type, base index, reset index) + hash of the raw index
  data; a hit costs one hash. **Bound-state filter** (`g_bound`, `InvalidateBound()` after anything that touches the pipeline outside XGpuDraw): unchanged Set* calls are skipped; state objects found by the
  last key. **Texture memo** per fetch slot (fetch constants + frame + guest-write sequence + generation counters `g_resolvedGen`/`g_texGen`). Constant buffers are only rewritten when the content changed.
- **Staggered re-validation**: vertex-buffer pages and textures that were unchanged for 3 consecutive checks are only re-compared every 8th frame (`kRecheckPeriod`); any change makes them dynamic again,
  a new index list forces a check of the draw's vertex ranges. (Proper fix = guest write tracking, see below.)
- **Motion-vector pass** (PS 1ba23ef83ff60169, colour format 4): its draws are skipped until a texture fetch / flush uses a resolve that follows skipped draws (`g_velocityNeeded`). In the scripted race
  it gets used ~8 s after the start (motion blur at speed), after which it renders for good. **Environment pass** (window scissor 256x256, 2x MSAA): rendered every 2nd frame only in race-sized frames
  (draws, clears and resolves are dropped in between, the previous resolved maps keep serving the reflections).
- Results (scripted Free Run, run_test.ps1 + padscript4.txt, helper `python fpsum.py <tag>`): baseline 14.0 fps (57 ms CP/frame) -> index cache/state filter/memo 22.5 -> staggered validation 25.1 -> env throttle 28.2 (18.6 ms CP/frame).
- Experiments: `ITC_SKIP_DRAWS=<frame>` drops every draw from that frame on (race with no rendering = 30 fps: the guest is not the limit); `ITC_VBLANK_HZ=<hz>` changes the simulated vblank rate (game pacing).
- Shader compile times (python hlsltime.py): the three big lit-surface pixel shaders (9473127d, d96c4660, 0bbc5532) take ~7 s each with the default flags (4 s skip-opt), most others 0.05-0.8 s; a cold
  `shader_cache_f0/` makes a new area stutter / skip draws until compiled. Ideas: a shader manifest pre-compiled at startup; skip-opt first, optimised swap-in later.
- Next ideas, in order: (1) split the CP thread into a prepare thread and a D3D-submit thread (prep ~12 ms, submit ~10 ms per frame today), (2) guest write tracking (version counter per 4 KB page bumped by the
  generated store macros PPC_STORE_* + the recompiler's stvx/dcbz emission + host writes such as NtReadFile) to drop all hashing/memcmp, (3) smaller constant buffers (translator reports the used float
  constant range; HLSL changes invalidate shader_cache_f0), (4) 60 fps: the game swaps every 2nd vblank - see ITC_VBLANK_HZ experiments, depends on whether game logic is time-based.
- Keyboard (kernel_xam.cpp): W = accelerator (right trigger, also left stick up), S = brake/reverse (left trigger, also stick down), A/D = steer, V = Y (camera), Space/Z = A, Q/E bumpers, Enter = START;
  the pad script also knows RT, LT, LSL, LSR.

'''
rep('## Known problems / next steps (in order)', perf + '## Known problems / next steps (in order)', 'perf section')
rep('4. Per-draw CPU cost: cache converted index buffers per (address,count,prim), skip texture/VB re-validation when no guest write touched them, fewer constant uploads.',
    '4. Per-draw CPU cost: index cache / state filter / memo / staggered validation are done (see Performance); next: prepare/submit thread split, guest write tracking, smaller constant buffers.', 'next steps 4')
open(p, 'w', encoding='utf-8').write(s)
print('ok')
