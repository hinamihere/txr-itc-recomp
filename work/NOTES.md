# TXR Import Tuner Challenge (Xbox 360) static recompilation - status notes

Last updated: 2026-10-05 (night: performance work, race ~14 fps -> steady 30 fps in the scripted test; 60 fps blocked by the game logic).  Everything lives in `work/` (also reachable as the ASCII junction `C:\itc-work`;
`C:\itc-xr` is a junction to `tools/XenonRecomp`). GCC cannot handle the non-ASCII `日南` in the user path,
so ALWAYS build/run via the junctions.

## STATUS
**(Older snapshot. The current status and open work list are in `../AGENTS.md`; the newest dated sections are at the end of this file.)**
`cd C:\itc-work; rt_build\itc.exe extracted\default.xex` opens a 1280x720 Direct3D 11 window: Ubisoft logo movie -> title -> profile prompt
-> main menu -> Free Run: car select (3D car renders), color select, course/time-of-day/route select, NOW LOADING, in-game (HUD + world).
Menus ~30 fps, in-game ~12-15 fps on the dev machine (the user says it plays fine for them). Keyboard + XInput pad input, waveOut audio (SFX; music is silent: XMA decode is a fake).
Goal set by the user: **Free Run mode playable, everything rendered correctly, good performance.**
**The race view now renders correctly** (RX-7/RX-8 from behind, sodium-lit asphalt and walls, lit buildings, signs, sky, headlights, cube-map reflections).
The last big fixes were: rect-list quads (the GS finds the right-angle corner: clears were half-drawn), vertex buffer size rounded UP, colour RT format 12 aliased to 3,
three shaders that failed to translate/compile, and **FXC's optimiser miscompiling the big lighting shaders** (now compiled with SKIP_OPTIMIZATION, see Findings).
Open items: mip chains (the far road shimmers), XMA audio (music), saves, per-draw CPU cost (see the profile below), TextureSign bits.

## Workflow
- Build (host DLL only, seconds): `cd /c/itc-work && ./build.sh`. Host code is -O2 (xgpu_d3d.cpp -O3); the game code (itc.exe) is -O1 + ITC_TRACE prologues.
- Changing `itc.toml` / `jump_tables.toml` / recompiler => `bin/XenonRecomp.exe itc.toml ../tools/XenonRecomp/XenonUtils/ppc_context.h`
  then `./build.sh` (changed generated files + one exe relink, ~2-5 min).
- Kill any running `itc.exe` before building (it locks `rt_build/itc_host.dll`): PowerShell `(Get-Process itc).Kill()`.
- Test runs: `powershell -NoProfile -ExecutionPolicy Bypass -File C:\itc-work\run_test.ps1 -Tag x -Seconds 90 -Pad '<script>' -EnvList 'A=1;B=2'` (run it with run_in_background; it kills
  ANY running itc.exe first (including the user's own game window - tell them), runs the game minimized, kills it, prints [stat] lines, logs to `x.err`;
  without `-ExecutionPolicy Bypass` PowerShell refuses the unsigned script). Pad script = `frame:BUTTON:frames,...`; the reliable full Free Run flow is in `padscript4.txt`
  (`PAD="$(cat padscript4.txt)"`), a Free Run test needs ~125 s to be in the race view.
  The physical pad/keyboard are only read while the window has focus; `ITC_NO_XINPUT=1` disables the pad for deterministic scripted runs.
  Scripted flow that works: `450:START:15,520:DOWN:8,560:A:8` (profile prompt -> No), then the main menu (cursor starts on Quest Mode):
  DOWN, DOWN, A = Free Run, then A for each following screen (car, color, base, course in, time zone, start, route).
- Frame dumps: ITC_DUMP_EVERY/ITC_DUMP_FROM/ITC_DUMP_MAX/ITC_DUMP_TAG -> `dumps/frame<tag>_NNNNN.png`; `python sheet.py <tag> [cols] [tile_w]` makes a contact sheet.
- Profiler: ITC_PROF=1 prints per-thread CPU and call-stack samples (function names for guest `sub_XXXXXXXX`, host, system DLLs) every 2 s into the log.
- Capture a 3D frame: ITC_CAPTURE_3D=<seconds> logs/dumps the first frame (after that time) with >= 3 large draws (with ITC_GPU_LOG=1, ITC_PKT=1 PM4 trace,
  ITC_DUMP_RESOLVE=1 intermediate render targets); or ITC_LOG_FRAME=N for a fixed frame. `drawsum.py`, `pktsum.py`, `pktdraws.py` summarise those logs;
  `hlslcheck.py` compiles dumped `.fail.hlsl` shaders and prints the errors with source lines.
- **Capturing an in-game frame (all of this is fast now; `Log` writes one line per call)**: `ITC_LOG_FRAME=1720;ITC_GPU_LOG=1` (2 = per-draw constants/vertex dump)
  `;ITC_PKT=1;ITC_DUMP_RESOLVE=1` with the deterministic Free Run script (`padscript4.txt`, `ITC_NO_XINPUT=1`). The frame number is NOT stable between runs
  (loading time varies; frame 1720 was the race intro in some runs and a menu in others) - check `python capsum.py <log> 60` first: it prints the capture frame
  pass by pass (render target, surface, MSAA, window scissor, resolves). `ITC_DUMP_PS=<hex prefix of a PS hash>` dumps every texture bound to those draws and the
  render target right after the draw (`dumps/bound<tag>_f<frame>_draw<n>_slot<k>.png`, `rtafter_f..png`). `ITC_DEBUG_PS_UV=<prefix>` makes a pixel shader output frac(t0.xy);
  `ITC_DEBUG_PS_COLORS="9473127d=ff0000,d96c4660=00ff00"` paints the listed pixel shaders in solid colours (see which draws cover which pixels; use ITC_SYNC_SHADERS=1).
  Dump all shader HLSL with `ITC_DUMP_SHADERS=1` and an EMPTY `shader_cache/` (cache hits are not dumped) -> `shaders/<vs|ps>_<hash>_<interpolators>.hlsl`.
  Resolve dumps (`res_f<frame>_NN_<dest>_fmt<N>.png`) have R/B swapped for the 16-bit formats (cosmetic, dump only).
- Architecture: `itc.exe` = generated game code + `src/exe_main.cpp`; `itc_host.dll` = everything hand-written (`main.cpp` loader+debug, `kernel*.cpp` kernel
  imports, `gpu.cpp` PM4 command processor + Vd*, `xgpu_d3d.cpp` D3D11 renderer, `xgpu_shader.cpp` Xenos microcode -> HLSL, `kernel_audio.cpp`, `prof.cpp`).

## Renderer design (xgpu_d3d.cpp)
- PM4 (gpu.cpp): all register writes go to `g_xregs`; IM_LOAD sets current shaders; DRAW_INDX/_2 -> `XGpuDraw`; XE_SWAP -> `XGpuSwap`.
- Shaders: microcode -> HLSL SM5 (cached by content hash + interpolator count). VS has no input layout: reads `StructuredBuffer<uint> xe_indices` (t31, offset in
  `sys1.w`) and raw `ByteAddressBuffer vb<slot>` (t32+slot). Rect lists use a geometry shader. Quad lists/fans/polygons/strips-with-restart become triangle lists on the CPU
  (primitive restart: PA_SU_SC_MODE_CNTL bit 21 + VGT_MULTI_PRIM_IB_RESET_INDX). PS bool constants are absolute (128..255), loop constants absolute (pixel 16..31).
- Shader compile is asynchronous (worker threads) with an on-disk DXBC cache `shader_cache/` (key = FNV of the generated HLSL). A draw whose shader is still
  compiling is skipped and the last complete frame is shown instead of the partial one (ITC_SYNC_SHADERS=1 waits instead). Ship/keep `shader_cache/`.
- Render targets: real D3D textures keyed by EDRAM base+format (format 12 = 2_10_10_10_FLOAT_AS_16_16_16_16 is ALIASED to 3: the game clears with one and
  renders/resolves with the other). 4x MSAA is ignored. Depth is D24S8 typeless with an SRV (for depth resolves). `CurrentSurface()` takes the height from the window
  scissor when the screen scissor is the unset 8192 (1024x1024 shadow maps), else 720; targets only ever grow.
- Vertex buffers: whole fetch-constant range uploaded as a raw buffer, size rounded UP to 16 bytes (shaders load float4s; rounding down once cut the last vertex of
  small buffers).
- Resolves (copy mode): rect from the 3 vertices of vfetch slot 0 (vertices are at -0.5: rect = ceil(min)..ceil(max)); result stays on the GPU in the "Resolved" cache
  keyed by destination address (formats 8888/2_10_10_10/16-bit/FP/depth), texture fetches of that address use it directly; flushed to guest memory (tiled, endian
  swapped) only when needed. Depth resolves (src=4) are kept as R32_FLOAT and fetched as k_24_8; they are never written back. Tile resolves of one frame
  buffer (dest = base + k * tileBytes) are appended to the first tile's entry on the GPU. `ITC_CPU_RESOLVE=1` forces the slow readback path.
- **Predicated tiling**: the game renders 3D in 3 horizontal 1280x256 strips (select 0x80000002, 8, 0x20). Draw masks come from a CPU pass that reads per-draw screen
  extents the GPU writes with EVENT_WRITE_EXT (NOT emulated -> masks are all "tile 0"). So: first strip pass draws everything full frame (window offset and tile
  scissor ignored), later passes skip all draws and only run their predicated resolves; clears after a tile resolve are skipped while tiling.
  ITC_EXACT_BIN_MASKS=1 / ITC_WINDOW_OFFSET=1 restore the literal behaviour. State packets with predicate bits keep exact hardware semantics.
- Textures: decoded from guest memory (untile, endian, formats -> RGBA8/R8/R16/FP/BC1-5/24_8), one mip only, cached by fetch constant + content hash
  (validated once per frame unless a GPU flush/CPU resolve touched the range). No cube/3D textures yet (dummy textures = black reflections).
- Present: frontbuffer fetch constant (slot 0) -> texture -> fullscreen triangle -> swapchain (vsync).

## Kernel / memory notes
- Guest memory: 4 GB reservation; physical RAM = 512 MB shared section mapped at 0xA0000000/0xC0000000/0xE0000000. `GuestAlloc`/`GuestAllocPhysical` use free-list
  heaps (the game frees a lot); committed pages are tracked in a table (`GuestCommittedBytes`), NEVER call VirtualQuery on the guest range in hot paths (it cost 77% CPU).
- I/O completion APCs, XMA contexts, GPU interrupts delivered by `IrqThread`, timer resolution 1 ms. See older notes in git history of this file for details.

## Debug env vars (all optional)
ITC_WATCHDOG=<sec>, ITC_STATUS=1 ([stat] every 2 s), ITC_PROF=1, ITC_GPU_LOG, ITC_SYNC_LOG, ITC_IO_LOG, ITC_AUDIO_LOG, ITC_MEM_LOG, ITC_FPS, ITC_DUMP_*, ITC_LOG_FRAME,
ITC_CAPTURE_3D, ITC_PKT, ITC_DUMP_RESOLVE, ITC_DUMP_BOUND(+_ALL), ITC_TRACE_FRAME, ITC_LIMIT_BASE/STEP, ITC_DUMP_SHADERS, ITC_FORCE_RED, ITC_NOCULL,
ITC_NO_ALPHATEST, ITC_SKIP_DRAWS, ITC_NO_RESOLVED_SAMPLE, ITC_CPU_RESOLVE(_MASK), ITC_SYNC_SHADERS, ITC_NO_XINPUT, ITC_PAD_SCRIPT, ITC_WATCH=<hex addr>.

## In-game frame structure (Free Run, night, captured 2026-10-05)
One frame is ~1900 draws in these passes (use `capsum.py` to list them):
1. HUD minimap 560x720 -> 512x512 resolves; 2. shadow maps: two depth-only 1024x1024 passes (78 + 3 draws, depth resolves src=4 to two textures);
3. environment cube-map face: 256x256 2xMSAA HDR, 465 draws, resolved to a 256x256 texture (one face per frame, cube maps are NOT supported yet -> black reflections;
   the cube research found NO resolve into a cube address: that pass is fetched later as a 2D texture, so it must NOT be skipped); 4. 512x288 format 4 (16_16) pass with 380-420 draws (resolved as fmt 25: velocity/depth-like);
5. **main scene**: 1024x576 region of a 1040x720 HDR (format 3) surface, ~730 draws: clear quad (format 12), depth pre-pass draws (mode=5 = depth only, shadow VS),
   colour draws, then a second phase (flares etc.), resolved to a 16_16_16_16 texture with **exp bias -3** (the scene is stored /8);
6. bloom chain: 256x144 downsample/blur passes (rect lists, 3 vertices of 40 bytes: position, tint (8,8,8,1), uv) with resolves;
7. composite: one full-screen rect list, PS 804c33c8: out = bloom + 8 * scene (the tint (8,8,8) undoes the exp bias; the tonemap branch is off when c33.w == 0),
   resolved to 16_16_16_16 and to a 2_10_10_10 texture; 8. HUD pass at 1280x720 (blit of that texture + gauges) -> front buffer resolve.
Rect lists: v0 top-left, v1 top-right, v2 bottom-left, v3 = v1 + v2 - v0 (geometry shader). Resolve draws use v0 top-left, v1 top-right, v2 bottom-right.

## Findings (what was wrong, so it is not re-investigated)
- Dark world = (a) `GetVertexBuffer` rounded the size DOWN to 16 bytes, so the last vertex of a 120-byte quad lost its uv.y -> composite/bloom sampled only the top row;
  (b) colour RT key included the raw format, so the format-12 clear/flare draws/resolve hit a different texture than the format-3 scene draws.
- Shadow maps are 1024x1024 but the RT height used to be forced to 720 (fetch mismatch -> a 4 MB readback per frame).
- PS interpolator count = SQ_PROGRAM_CNTL bits 20..23 + 1 is right. Draws in mode 5 (RB_MODECONTROL) are depth-only: the PS pairing in the log (shadow VS + colour PS) is harmless.
- The game's `SQ_PROGRAM_CNTL`/shader loads come from indirect buffers; IM_LOAD "last load of each type wins" works (see `[gpu] IM_LOAD` lines in capture logs).
- stderr logging was the reason verbose captures took minutes: MinGW printf calls WriteFile per character; `Log` now formats into a buffer and writes once.
- Performance: the CP thread (XGpuDraw ~25 us/draw: idxbuild, vb, tex hash/bind, constants) is the bottleneck at ~1900 draws/frame; the guest main thread just spins in the
  D3D ring-buffer-wait function sub_8229D8A0 when the ring is full. The env cube pass (465 draws) is the single biggest removable chunk.

- **FXC miscompile**: with the default optimiser the big pixel shaders (9473127d, 0bbc5532, d96c4660: pc-state-machine + 16 shadow taps) returned stale registers (the world
  position leaked into the colour: B = 1824.6) -> saturated blue/red/cyan surfaces. Compiling without optimisation (D3DCOMPILE_SKIP_OPTIMIZATION, `ShaderCompileFlags()`; cache dir
  `shader_cache_f4`) renders them correctly. `ITC_SHADER_FLAGS=<n>` overrides (0 = the old behaviour). Cause not understood (candidate: my HLSL relies on something FXC treats as undefined).
- **Rect lists**: the three corners come in different orders (blits TL,TR,BL; clears/resolves TL,TR,BR). The GS now finds the corner opposite the longest edge.
- RenderDoc (installed in `C:\Program Files\RenderDoc`) works headlessly, see "RenderDoc workflow". The game crashes under RenderDoc unless `ITC_IDX_UPDATE=1` (Map of the 16 MB index ring fails).
- Performance profile of the race scene (render thread ~81% of one core): my `BuildIndices` ~21% inclusive, `GetTextureSRV`/`DecodeTexture` ~21%, `memcmp` (VB shadow) ~4%,
  Intel D3D11 driver (igd10iumd64) ~15-20%, sleeping ~18%. So API overhead is NOT the main cost; caching index conversion / texture validation would help on any API.
- A Vulkan port was considered (user question): not needed for any bug found so far; revisit only for portability or ROV-style EDRAM emulation. ReXGlue (Xenia-based recomp SDK) was
  researched in `notes_rexglue.md`: needs clang 18+/MSVC ABI (the VS C++ workload is not installed), regenerates the code, ~3-5 days to reach today's state; not pursued for now.
- Community note (from that research): Xenia compat pages mention a black screen after the first Quest-mode race, fixed by patching the XEX at 0x8218E4EC (`cmplwi r11,60` -> `cmplwi r11,26`,
  word 0x2B0B001A). Our recompiled code has the same instruction: try it if the same symptom appears after a race.

## RenderDoc workflow
- Capture: `powershell -NoProfile -ExecutionPolicy Bypass -File C:\itc-work\run_rdoc.ps1 -Tag rdN -Seconds 600 -Frame 1720 -Pad "$(cat padscript4.txt)" [-EnvList 'ITC_SHADER_DEBUG=1;ITC_SYNC_SHADERS=1']`
  (sets ITC_RDOC_FRAME/ITC_RDOC_QUIT/ITC_IDX_UPDATE; the game exits right after the capture; output `rdoc\itc_capture.rdc`, ~75 MB). F12 overlay captures are named `itc_frameN.rdc`.
- Analysis scripts run in qrenderdoc's embedded Python: `run_rdoc_script.ps1 -Script C:\itc-work\rdoc_<x>.py -Capture <rdc> -OutDir <dir> -Extra 'NAME=value|NAME2=value2'`:
  `rdoc_pixel.py` (pixel history incl. depth + shader names of the largest float RT), `rdoc_textures.py` (export every texture bound to given events as PNG),
  `rdoc_debug.py` (RenderDoc shader debugger on a pixel: register trace + DXBC disassembly; with `ITC_SHADER_DEBUG=1` the shaders carry debug info), `rdoc_vertex.py` (vertex shader outputs).
- Shader objects are named `ps_<hash>_<interp>` / `vs_<hash>_0` (D3D debug names) so events are readable.
- Other debug env vars added: ITC_DUMP_CONSTS=<ps prefix>, ITC_DEBUG_PS_RAINBOW, ITC_DEBUG_PS_COLORS, ITC_DEBUG_PS_UV, ITC_DUMP_PS, ITC_DEPTH_BIAS (test only), ITC_SHADER_DEBUG, ITC_SHADER_FLAGS.

## Performance (2026-10-05 evening) - the race was CPU-bound on the command-processor (CP) thread
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
- **Second round (19:00-19:15)**, all with env switches for A/B: a 32 MB dynamic **vertex ring** (`VbRingCopy`: data drawn in fewer than 3 frames (big ranges) / 30 frames
  (<= 16 KB ranges) and "hot" buffers whose pages change 3 frames in a row are copied per draw instead of getting a persistent buffer; the shader gets the ring position as a
  dword offset in `vfmeta.x`, added in dword arithmetic) - this killed ~30 buffer creations per frame; the **constant ring** (48 MB dynamic CB, NO_OVERWRITE maps, bound with
  `VSSetConstantBuffers1` offsets; the per-draw index offset moved to a tiny b1 ring, `xe_draw.x`); **register dirty tracking** in `XGpuWriteRegister` (same-value writes ignored;
  the big CBs are refilled only when the float/bool/loop/misc registers, the shader, the RT size, a used vfmeta entry or a used (patched) texture swizzle changed); **batched binds**
  (one Set*ShaderResources / Set*Samplers call per stage covering the changed slot range); mip layouts cached per texture; coalesced vertex page uploads; big textures untiled on 4
  threads; **two-stage shader compiles** (quick unoptimised build used first, the optimised one swapped in when ready, `g_shaderUpgrades`; `ITC_NO_QUICK_SHADERS=1`).
  Switches: ITC_NO_VBRING, ITC_NO_CBRING, ITC_NO_CB_OFFSETS, ITC_NO_PARALLEL_DECODE, ITC_FRAME_LATENCY=<n> (DXGI max frame latency), ITC_SLOW_MS=<ms> (hitch log threshold),
  ITC_BLACK_DETECT=1 (logs when the back buffer is black), ITC_SKIP_DRAWS=<frame>, ITC_VBLANK_HZ=<hz>.
  Results (`python fpsum.py <tag>`): ab5 28.9 fps / 16.1 ms CP, **ab6 29.7 fps (min 27.5) / 10.0 ms CP per frame**; 31 frames > 45 ms in 120 s (texture streaming bursts at the race start).
  Per 2 s window at the end of ab6: draw breakdown vb=70 tex=190 consts=220 idxbuild=50 state=31 drawcall=23 ms; CB uploads vs ~1600 ps ~1000 per frame.
- CP-thread accounting (`[stat]   cp thread:` line): ab9 = cpu 16-19 ms/frame (draws 12-14, the rest is PM4 parsing/resolves/swap), blocked in Present 0.4-3 ms, ~50% of wall
  time busy at 30 fps. GPU pass timings with ITC_GPU_TIMING=1 are wall spans between marks (idle gaps while the GPU waits for the CPU land in the current pass: "resolves 33 ms"
  is such an artefact); use `=2` for busy time per draw/shader. Minimap HUD: the 512x512 minimap RT (first 3 passes of the frame, lines drawn with colour mask R / G only) is
  composited with a transparent background (a yellow billboard behind it shows through, frameab6_02900.png) - probably a clear-colour/alpha detail, not investigated.
- **60 fps verdict** (`notes_60fps.md`, research agent): the game waits for 2 vblank callbacks per frame (`li r6,2` at 0x820923D4 -> [gfx+304]; its own vblank callback
  sub_8258D4D8 counts them) and runs ONE fixed simulation tick per presented frame with 30 Hz baked in (30.0f @0x82000D44, 1/30 @0x820029EC, 1/60 @0x820029E8, ~60 static
  {ptr,1/30,1/60} rate tables, integer frame timers everywhere; no elapsed-time logic). Presenting every vblank is a one-instruction patch (or a mid-asm hook setting r6=1) but
  gives double speed; correct-speed 60 fps = a full "60fps mod" (halve the constants, double every frame timer, retune damping/AI/replay/music cues): days-weeks, high drift risk.
  Decision: stay at 30 and make it solid. Side finding: `mftb` is translated to `__rdtsc()` while KeQueryPerformanceFrequency returns 50 MHz -> timebase timeouts are ~60x too
  short (the XMA wait loop in sub_8245D288 times out after ~2 ms instead of 125 ms): fix when working on the silent music. The thread-split / write-tracking design is in
  `notes_renderer_design.md` (not implemented; CP is at ~11 ms draws + ~6 ms parsing/resolves per frame).
- **Hitch hunt** (`[slow]` lines carry draw-thread time, Present block, Map stalls, "ring empty" = CP idle waiting for the guest, fence waits, PNG-dump marker): with the harness
  logging through a PowerShell pipe, a 50 ms frame appeared every 60 frames (the [stat] block's Log() calls blocked on the pipe) - `run_test.ps1` now redirects stderr to the file
  via cmd.exe (ns1: 4 slow frames in 130 s, all texture-decode bursts at the race start). The user's `play.bat` logs to a file directly. Map stalls are rare (DISCARD at a ring
  wrap: 5-9 ms, a few per run). DXGI maximum frame latency is now 1 by default (no fps cost; lower input lag), ITC_FRAME_LATENCY overrides. Texture re-hashes are budgeted
  (12 per frame, `kRehashBudget`), vertex-buffer creations too (12 per frame, `kVbCreateBudget`).
- `play.bat` (user launcher, logs to play.err) no longer enables ITC_BLACK_DETECT: that detector reads the back buffer once per second (a GPU sync) and caused a 50 ms frame every
  30 frames in the user's session (all `[slow]` frames at multiples of 30). `play_prof.bat` adds ITC_PROF=1 (guest/host call-stack samples) for diagnosing slowdowns in real play.
  A real session ended with 8-14 fps from frame ~4330 with "ring empty 40-68 ms" per frame = the GUEST thread was the bottleneck there (draw thread 15-22 ms), cause unknown
  (area change / opponents?) - profile it with play_prof.bat next time.
- Game code at **-O2** (runtime/CMakeLists.txt; was -O1 from bring-up; the -O1 exe is kept as `rt_build/itc_O1_backup.exe`): built 2026-10-05 ~19:45 to cut guest-side frame time
  (the remaining 50 ms frames show the CP idle for ~15 ms = the guest submitted late).
- Helpers: `fpsum.py <tag> [from_frame]` (race fps per 2 s window + mean from a run log), `blackcheck.py <tag>` (dark frames in dumps), `hlsltime.py` (FXC compile times per dumped
  shader and flag set), `play.bat` (starts the game for the user with logging + black-screen detector; the scripted harness kills every itc.exe, including the user's).
- Keyboard (kernel_xam.cpp): W = accelerator (right trigger, also left stick up), S = brake/reverse (left trigger, also stick down), A/D = steer, V = Y (camera), Space/Z = A, Q/E bumpers, Enter = START;
  the pad script also knows RT, LT, LSL, LSR.

## Audio (2026-10-05 night)
- SFX path: `XAudioRegisterRenderDriverClient` callback every 256 samples; `XAudioSubmitRenderDriverFrame` gets 6 PLANAR channels of 256 big-endian floats (as in
  UnleashedRecomp's audio.cpp; it was read as interleaved until 19:50 - SFX were scrambled), downmixed to stereo 16-bit into waveOut (kernel_audio.cpp).
- Music = XMA2 streams through hardware decoder contexts (`XMACreateContext`, 64-byte XMA_CONTEXT_DATA blocks, layout from Xenia's xma_context.h; the game kicks via the
  0x7FEA0000 MMIO window which is plain memory here, so `xma.cpp` polls the contexts every 2 ms). `xma.cpp` implements the decoder with FFmpeg's libavcodec XMA2 decoder
  loaded at runtime from `avcodec-*.dll` / `avutil-*.dll` placed next to `rt_build/itc.exe` (headers expected in `work/ffmpeg/include`, see runtime/CMakeLists.txt; without
  them the file compiles to the old silent stub). Whole 2 KB packets are fed in order (libavcodec keeps frames straddling packets), decoded 512-sample frames are written as
  big-endian int16 (interleaved for stereo) into the 256-byte-block output ring (write/read offsets in blocks; full = write catches read -> output_buffer_valid cleared);
  input buffers are consumed at packet granularity (valid bit cleared, current_buffer toggled, read offset = 32 bits), loops at packet granularity. **Working** (FFmpeg LGPL shared build, avcodec-63/avutil-61/swresample-7 DLLs in rt_build/, headers in work/ffmpeg/include, ffmpeg.exe in work/ffmpeg; verified offline with
  `xma_test.exe` (decodes a stream extracted by `xwb_info.py` from bgm_tmp_01.xwb: 1068 packets -> 68.5 s stereo 44.1 kHz, sane levels) and in the game: contexts decode, no clicks
  in `ITC_AUDIO_DUMP=file.wav` recordings of the final mix). Music = `bgm_tmp_XX.xwb` (XACT wave banks, big-endian `DNBW` version 39: header = sig, version, 5 (offset,length) segments;
  entry = 24 bytes, format bitfield tag:2 ch:3 rate:18 blockAlign:8 bps:1), stereo 44.1 kHz XMA2 with loop regions; sound effects are mono. Context field positions (host order,
  LSB first): d0 = in0 packets 12 | loop_count 8 | in0 valid | in1 valid | out blocks 5 | out write 5; d1 = in1 packets 12 | ... | sample_rate bits 27-28 | is_stereo bit 29 | out valid bit 31.
  Loops are packet-granular (loop points inside a packet restart at the packet start: up to ~0.17 s early). Mixed output goes through a soft limiter (kernel_audio.cpp).
  `ITC_NO_XMA=1` disables decoding, `ITC_AUDIO_LOG=1` logs context activity, `ITC_AUDIO_DUMP=<wav>` records the final mix.
- Time base: `mftb` was emitted as `__rdtsc()` (GHz) while KeQueryPerformanceFrequency reports 50 MHz -> game timeouts ~60x too short (e.g. the XMA wait loop sub_8245D288).
  `patch_mftb.py` rewrites the 7 call sites in out/ppc_recomp.{45,48,71,91}.cpp to `ItcTimebase()` (50 MHz from QueryPerformanceCounter, defined inline in those files);
  **re-run it after every regeneration of the recompiled code** (then rebuild the exe).

## Known problems / next steps (in order)
1. Mip chains (textures are decoded base level only; the far road shimmers). Cube map mips are not needed (base-map filter), see `notes_cube.md`.
2. Texture fetch-constant sign bits (unsigned/signed/biased/gamma) are ignored (all textures seen so far are unsigned); exp_adjust is applied; MSAA edges; depth bias for decals.
3. XMA audio decode (music is silent), saves/profile, rumble. Check the post-race black screen (see the community patch above).
4. Per-draw CPU cost: index cache / state filter / memo / staggered validation are done (see Performance); next: prepare/submit thread split, guest write tracking, smaller constant buffers.
5. `notes_shader_diff.md` (translator vs XenosRecomp comparison by a research agent, if present) may list more instruction-semantics differences (legacy 0*x=0 multiply, etc.).
6. EVENT_WRITE_EXT extents; the generated code is -O1 with trace prologues (rebuild -O2 without ITC_TRACE if guest CPU matters).

## Gotchas
- `__imp__<Name>` = host override for kernel import `<Name>`; overriding a game function from the DLL does not work (use `[[midasm_hook]]`).
- D3D11: invalid calls are ignored silently (no debug layer); `SV_VertexID` excludes the start vertex on this driver; mismatched MRT sizes silently drop OMSetRenderTargets.
- Heredocs with many quotes break the bash tool: write patch scripts with the Write tool and run them with python3. PowerShell here is 5.1 (no `&&`).
- Reference: `tools/UnleashedRecomp` (XenosRecomp shader recompiler: loop/bool constant addressing, cube-map handling) is useful for shader semantics.

## Vulkan renderer (2026-10-08, in progress)
- `ITC_RENDERER=vulkan` selects `runtime/src/xgpu_vk.cpp`; default is still D3D11 (`xgpu_d3d.cpp`). `xgpu_common.cpp` holds the register file, window,
  thumbnails and the dispatch; `xgpu_shared.inl` holds the API-independent code (decode, tiling, index conversion, constants) included by both backends.
- Deps (downloaded with the user's OK): `deps/vkheaders` (Vulkan-Headers 1.3.290), `deps/dxc` (dxcompiler.dll 2024-07-31, signed by Microsoft;
  must ship next to itc.exe or is loaded from deps/dxc/bin/x64). Translator has a Vulkan mode (`XTranslateParams::vulkan`, bindings in xgpu_shader.h).
- Design: push descriptors (set 0: VS tex 0-3, VB 4-15, PS tex 16-31), static set 1 (2 dynamic UBOs + index arena), per-frame host-visible rings
  (constants / vertex / staging), pre-upload command buffer, render passes with LOAD/STORE + conservative external dependencies, offscreen 1280x720
  back image blitted to a FIFO swapchain, SPIR-V cache in shader_cache_vk/, pipeline cache vk_pipeline_cache.bin. ITC_VK_SYNC=1 = no CPU/GPU overlap.
- Status: menus + races render like D3D11 at ~30 fps on the Iris Xe. Open: user sees "black triangles flying around" in races; one capture
  (vk4 frame 2332) showed a surface flat dark grey (13,13,13) for one frame, not reproducible in A/B runs (runab.ps1 + blackpix.py detector).
  play_vk.bat / play_vk_safe.bat (ITC_NO_PERF=1) handed to the user to bisect.
