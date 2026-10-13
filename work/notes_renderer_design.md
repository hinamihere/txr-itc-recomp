# Renderer design: prepare/submit thread split + guest-memory write tracking

Written 2026-10-05 (evening) by a read-only review agent. Everything below is a proposal; no code was changed.

Sources read: `NOTES.md` (Renderer design, Performance), `runtime/src/gpu.cpp` (646 lines), `runtime/src/xgpu_d3d.cpp`
(3471 lines at 18:00 - the file was being edited while this was written and had 3576 lines at 18:49; line numbers below
are from the 18:00 version unless marked *new*), `xgpu_shader.cpp/h`, `kernel.cpp`, `kernel_io.cpp`, `guest.h`, `main.cpp`,
`gcc_compat.h`, `runtime/CMakeLists.txt`, `C:\itc-xr\XenonUtils\ppc_context.h`, `C:\itc-xr\XenonRecomp\recompiler.cpp`,
`rt_build/.ninja_log`, `regen.log`, and the `[stat]` lines of `ab4.err`, `vb60.err`, `vb120.err`.

## 0. Numbers to design against (from the logs, race scene, scripted Free Run)

- `fpsum.py`: ab4 26.9 fps / 22.3 ms CP per frame, vb60 27.4 fps / 17.2 ms, dr1 27.0 fps / 21.8 ms (NOTES quotes 18.6 ms).
  Run-to-run spread is large (17-25 ms); use the `draw breakdown` ratios rather than absolute numbers.
- `[stat] draw breakdown` (ab4, 2 s windows of ~51 frames, converted to ms/frame): **vb 6.5-11.4**, tex 2.8-3.1,
  consts 4.1-4.5, idxbuild 1.0-1.1, state 0.5-0.6, drawcall 0.4, rt 0.2; `resolve` 0.4, `swap` 0.9-1.3 per frame.
  So guest-memory validation (`GetVertexBuffer` memcmp + `GetTextureSRV` hashing + the index hash inside idxbuild) is
  **10-14 ms/frame**; pure D3D submission (Map/memcpy of constants, state, Draw, UpdateSubresource, resolves, present) is
  roughly **6-8 ms/frame**. Part 2 (write tracking) therefore removes at least as much as Part 1 (the split) does.
- GPU span 11.8-19.2 ms/frame (this includes time the GPU waits for the CPU). `vb120.err` (ITC_VBLANK_HZ=120, i.e. a 60 fps
  pacing) still gave 27 fps with 25.6 ms CP/frame: the CP thread, not the GPU, is what blocks 60 fps.
- Driver facts logged by the new code (`ab4.err`): `ConstantBufferOffsetting=1 MapNoOverwriteOnDynamicConstantBuffer=1
  MapNoOverwriteOnDynamicBufferSRV=1` - the Iris Xe driver supports `VSSetConstantBuffers1` offsets and NO_OVERWRITE on
  constant and SRV buffers natively. One run logged `Map(NO_OVERWRITE) on the index ring failed (hr=8007000E)`
  (E_OUTOFMEMORY): keep rings modest (<= 32 MB each) and keep the DISCARD fallback.
- `resolved-texture flushes: +0` in every race window: `FlushResolved` (the only GPU->guest-memory round trip) does not
  happen in the race; it may happen in menus/loading (fetch-mismatch/overlap reasons).
- Index cache: hits +125k..137k / 2 s, misses +0 in steady state (`GetIndexList` only pays `Hash64` of the raw indices).
- Constant uploads (new counters): vs +67779 / ps +59836 per 2 s = ~1300 + ~1170 per frame even with the new
  `g_regDirty` tracking, i.e. 6 KB x ~2500 Map(DISCARD)+memcpy per frame = ~15 MB/frame of constant traffic.

## PART 1 - Prepare thread / submit thread

### 1.1 Thread roles and ownership

| Thread | Today | After the split |
|---|---|---|
| `CpThread` (gpu.cpp:410) | PM4 parse, registers, fences, draws **and** every D3D call | becomes the **PREPARE** thread: PM4 parse, `g_xregs`, fences/WAIT_REG_MEM/INTERRUPT exactly as now, shader lookup, index cache, VB/texture validation + decode, constant fill, state-object lookup; writes `DrawCmd`s and upload blobs into the queue |
| (none) | | **SUBMIT** thread: owns `g_ctx`/`g_ctx1` and the swap chain; executes ops in order; owns the `g_bound` filter, GPU timing queries, overlay, present, readbacks |
| `IrqThread` (gpu.cpp:104) | 60 Hz vblank + CP interrupts | unchanged (it never touches D3D) |
| compile workers (`CompileWorker`, 318) | HLSL -> DXBC | unchanged; `CreateVertexShader/PixelShader` stay on prepare (device calls are free-threaded) |
| window thread (210) | message loop | unchanged |

Rule: **the submit thread never reads guest memory and never reads `g_xregs`**; everything it needs is in the command.
The only guest-memory *write* it does is the flush/CPU-resolve untile (synchronous op, 1.6). ID3D11Device methods
(`Create*`, `CheckFeatureSupport`, `SetPrivateData`) are thread-safe (device not created with
`D3D11_CREATE_DEVICE_SINGLETHREADED`, see `EnsureInit` 1891), so prepare keeps creating buffers/textures/views/state
objects itself; only `ID3D11DeviceContext` calls move.

Why not D3D11 deferred contexts: the runtime would record and replay the same ~30 calls per draw on the submit thread
(no CPU saved there), `Map(NO_OVERWRITE)` on deferred contexts is restricted, and the `g_bound` filter would have to be
rebuilt per command list. A thin hand-rolled queue keeps the existing bind-filter logic and lets the submit side batch
uploads, which deferred contexts cannot do.

### 1.2 The queue

Single-producer/single-consumer byte ring (commands are variable-sized, written in place; no allocation per draw):

```cpp
struct CmdRing {                                  // 32 MB: ~2-3 frames of commands incl. inline blobs <= 64 KB
    static constexpr uint64_t kSize = 32u << 20;
    uint8_t* buf;                                 // VirtualAlloc'd, never freed
    alignas(64) std::atomic<uint64_t> head{0};    // bytes produced (prepare writes, submit reads)
    alignas(64) std::atomic<uint64_t> tail{0};    // bytes consumed (submit writes, prepare reads)
    uint64_t localHead = 0;                       // prepare-private copy; published by Commit()
};
struct CmdHdr { uint16_t op; uint16_t pad; uint32_t size; };   // size includes the header, multiple of 16
// producer: Alloc(size): while (localHead + size - tail.load(acquire) > kSize) { WaitOnAddress(&tail, ...); }
//           a command never straddles the end: emit OP_PAD to the wrap point first.
// Commit(): head.store(localHead, release); WakeByAddressSingle(&head) only if the consumer flagged itself sleeping.
// consumer: t = tail; while (t == head.load(acquire)) { spin 2000x YieldProcessor, then WaitOnAddress(&head) }
//           execute [t, t+size); tail.store(t + size, release); WakeByAddressSingle(&tail).
// (WaitOnAddress/WakeByAddressSingle: Windows 8+, link -lsynchronization in CMakeLists; or use a manual-reset event.)
```

Blobs: constants, index lists, VB page data and small texture levels are written **inline** after the command header
(prepare `memcpy`s from its working buffers; the submit thread reads them from the ring). Blobs > 64 KB (texture
uploads, initial VB data for very large buffers) go into a prepare-owned `std::vector<uint8_t>` whose pointer travels
in the command and which is released through the keep-alive list (1.7) - never freed by submit.

Ops (every immediate-context use found in the file, see 1.6 for where each comes from):

```
OP_PAD, OP_FRAME_BEGIN{frame}, OP_DRAW{DrawCmd}, OP_UPLOAD_CONSTANTS{ringOffset, bytes, data[]},
OP_IDX_APPEND{ringStart, count, data[]}, OP_IDX_DISCARD, OP_VB_UPDATE{buf, offset, bytes, data[]},
OP_TEX_UPDATE{tex, subresource, rowPitch, bytes, data[] | blobPtr}, OP_CLEAR_RTV{rtv, col[4]},
OP_CLEAR_DSV{dsv, z, stencil}, OP_COPY_REGION{dst, src, box} (RT growth), OP_RESOLVE{...} (GpuResolve pass),
OP_FLUSH{...; HANDLE done} (synchronous), OP_CPU_RESOLVE{...; HANDLE done} (synchronous),
OP_SWAP{frame, frontSrv, swizzle, overlay lines, flags: incomplete/thumb/dumpBackbuffer/rdocStart/rdocEnd},
OP_DUMP_TEX{tex, name} (debug), OP_GPU_MARK{passKey, ps} (ITC_GPU_TIMING)
```

### 1.3 What a DrawCmd carries (everything `XGpuDraw` 2315-2661 derives from registers or caches)

```cpp
struct DrawCmd {                                   // ~ 200 B fixed + 16 B per bound SRV/sampler
    // pipeline objects (raw COM pointers; kept alive by the caches + keep-alive lists, see 1.7)
    ID3D11VertexShader* vs; ID3D11PixelShader* ps; ID3D11GeometryShader* gs;      // gs = g_rectGS or null  (2576-2589)
    ID3D11BlendState* blend; float blendFactor[4];                                  // GetBlendStateFast + RB_BLEND_* (2536-2541)
    ID3D11DepthStencilState* dss; uint8_t stencilRef;                               // RB_DEPTHCONTROL / RB_STENCILREFMASK (2542-2544)
    ID3D11RasterizerState* rs;                                                      // GetRasterStateFast (2553-2555)
    ID3D11RenderTargetView* rtv[4]; uint8_t numRTs; ID3D11DepthStencilView* dsv;    // GetColorRT/GetDepthRT (2398-2421)
    D3D11_VIEWPORT vp; D3D11_RECT scissor; uint8_t topology;                        // 2557-2574
    // constants: byte offsets into the constant ring (1.4); 0xFFFFFFFF = "unchanged since the previous draw"
    uint32_t vsCbOffset, psCbOffset;                                                // 6144-byte slices (sizeof XeCB rounded to 256)
    uint32_t drawCbOffset;                                                          // 256-byte slice holding xe_draw.x = startVertex
    uint32_t indexCount;                                                            // g_ctx->Draw(indexCount, 0) (2601)
    // resources: slot lists (counts are small: 1-3 vfetch, <= 8 textures per stage in practice)
    uint8_t nVsSrv, nPsSrv;                                                         // entries: {uint8 slot; ID3D11ShaderResourceView* srv; ID3D11SamplerState* smp}
    // debug/timing
    uint64_t passKey, psHash; uint32_t frame, drawIndex; uint8_t flags;             // flags: GPU mark before/after, dump RT after draw
    Binding b[];   // nVsSrv + nPsSrv entries follow the fixed part (slot 31 = g_idxSRV, 32+s = vertex buffers, t = textures)
};
```

Not carried (prepare-only): the register file, `g_curVS/g_curPS` hashes (except for timing), fetch constants,
`vs->info` (used by prepare to compute vertex ranges and to pick SRV slots), the index-cache entry, `CurrentSurface`.

### 1.4 Uploads: constants, indices, vertex pages, textures

**Constants.** Replace the two per-draw `UploadCB` Map(DISCARD) calls (2158 new) and the per-draw b1 map in
`BindDrawConstants` (2164-2183 new, one `Map(NO_OVERWRITE)` + `VSSetConstantBuffers1` *per draw*) with one
**constant ring**: a single `D3D11_USAGE_DYNAMIC | D3D11_BIND_CONSTANT_BUFFER` buffer of 16-32 MB (it cannot carry other
bind flags; the index ring stays separate). Slice sizes: `XeCB` = 5968 B -> 6144 B (24 x 256 B); the b1 draw block 256 B.
Prepare lays the slices out in ring order inside a per-batch host blob; every ~64 draws (or 256 KB) it emits
`OP_UPLOAD_CONSTANTS{ringOffset, bytes, data}`; submit does **one** `Map(NO_OVERWRITE)` + memcpy + `Unmap` per batch
(DISCARD when the ring wraps, exactly like `AppendIndices` 2215-2258), then binds with
`g_ctx1->VSSetConstantBuffers1(0, 1, &ring, &first, &num)` where `first = offset/16`, `num = 384` (the shader declares
373 float4; NumConstants must be a multiple of 16 and >= the declared size, <= 4096). b1: `first = drawOff/16, num = 16`.
The driver supports this natively (ConstantBufferOffsetting=1, 0.). Fallback when `g_cbOffsets == false`: keep today's
three small buffers and do the Map(DISCARD) on submit (the existing `ITC_NO_CB_OFFSETS` path).
The `g_regDirty`/`g_fetchVer` refill decision (2559-2609 new) stays on prepare and decides whether a new slice is written
or `vsCbOffset = 0xFFFFFFFF` (reuse). Volume: today ~2500 x 6 KB = 15 MB/frame; the per-shader used-range reduction
(NOTES idea 3) shrinks each slice to `max constant index used` (needs the HLSL to declare `float4 c[N]` with N per
shader, which invalidates `shader_cache_f0/`), the struct should then be reordered so that `c[]` comes last.

**Indices.** `AppendIndices` keeps its ring arithmetic on prepare (`g_idxPos`, `g_idxFirst`, `g_idxResets`, the
`g_idxCache.clear()` on wrap - all prepare state); the Map/memcpy/Unmap moves to `OP_IDX_APPEND` (data inline) and the
wrap to `OP_IDX_DISCARD`. Batch consecutive appends into one map just like constants (misses are rare in steady state).

**Vertex pages.** `GetVertexBuffer` 1267-1312: `CreateBuffer` with initial data (1284) stays on prepare (device call,
reads guest memory at that moment - fine). The per-page `memcmp` + `UpdateSubresource(box)` (1302-1306) becomes:
compare on prepare (or version check, Part 2), copy the changed page into the ring (`OP_VB_UPDATE`, 4 KB inline), submit
calls `UpdateSubresource`. Coalesce adjacent changed pages into one op (today a 64 KB dynamic buffer costs 16 calls).

**Textures.** `GetTextureSRVSlow` 1076-1199 and `GetCubeSRV` 1010-1053: decode on prepare; new textures are created with
initial data on prepare (`CreateTexture2D(&d, sd)` 1187/1045 - no op needed); *updates* of an existing texture
(1175-1176, 1035) become `OP_TEX_UPDATE` with the decoded level in a heap blob. Important: `static Decoded dec` (1153)
is reused across calls and must become a per-upload allocation (the blob is read by submit later). Alternative that
avoids the op entirely: on content change re-create the texture (device call) and push the old one to keep-alive; good
for rarely changing textures, bad for a per-frame dynamic texture (use the op there).

### 1.5 Submit-side execution of a draw (the `g_bound` filter moves with it)

`SubmitDraw(const DrawCmd&)` = lines 2423-2432 (OMSetRenderTargets + `InvalidateSrvs` + the `OMGetRenderTargets`
warning), 2437-2485 (`BindVSSrv/BindPSSrv/BindVSSmp/BindPSSmp`), 2530-2531 + `BindDrawConstants` (now
`VSSetConstantBuffers1` with the slice offsets), 2541-2590 (blend/depth/raster/viewport/scissor/topology/shaders through
`g_bound`), `GpuTimingBeforeDraw`/`Draw`/`GpuTimingAfterDraw` (2600-2602). `g_bound` (1976-2003) is a model of the
immediate context, so it belongs to the submit thread; `InvalidateBound()` is called by the resolve/present/overlay ops at
the same points as today (2683, 1853, 3250). Later optimisation: prepare keeps a second copy of the model and omits
unchanged bindings from the command (smaller commands); not needed for stage 1.

### 1.6 Immediate-context operations hidden inside helpers, and ordering

| Helper (old line) | Context call | Becomes | Ordering note |
|---|---|---|---|
| `GetVertexBuffer` 1305 | `UpdateSubresource` page | `OP_VB_UPDATE` before the draw | in-stream order = today's order |
| `GetTextureSRVSlow` 1175-1176, `GetCubeSRV` 1035 | `UpdateSubresource` levels/faces | `OP_TEX_UPDATE` | same |
| `GetColorRT` 1402/1406, `GetDepthRT` 1426 | clear on create, `CopySubresourceRegion` on growth | `OP_CLEAR_RTV/DSV`, `OP_COPY_REGION` emitted at creation time, before the draw | the new texture is created on prepare; the old one goes to keep-alive because queued draws may still bind its RTV/SRV |
| `ApplyPendingClears` 1349-1358 (called at 2325) | clears | `OP_CLEAR_*` | prepare decides, same point in the stream |
| `XGpuDoResolve` 3091-3110 | immediate clears after a resolve | `OP_CLEAR_*` | same |
| `GpuResolve` 2671-2786 | `Map(g_copyCB)`, OM/RS/IA/VS/PS set, `Draw(3)`, `CopySubresourceRegion` on growth, `DumpResolvedTexture` | `OP_RESOLVE{srcSrv, dstRtv, x0, y0-yOff, rw, rh, yOff, swapRB, expBias, copyPs}` | the `Resolved` table bookkeeping (2685-2751: which base maps to which texture, `memSig`) stays on prepare |
| `FlushResolved` 770-834 | `CopySubresourceRegion` to staging, `Map(READ)`, untile into guest memory | `OP_FLUSH` **synchronous**: prepare enqueues, then waits on an event; submit executes everything queued before it (full drain + GPU stall on the Map), writes guest memory, bumps the write versions (Part 2) / `NoteMemWrite`, signals | prepare must not continue because the next step decodes from that memory. Cost = one pipeline drain (up to ~1-2 frames). Not hit in the race (0 flushes), can hit in menus; if it ever becomes frequent, the fix is a GPU-side format conversion instead of a flush |
| CPU resolve fallback 2998-3087 (`ITC_CPU_RESOLVE`) | same pattern | `OP_CPU_RESOLVE` synchronous | debug path only |
| `DumpTextureResource` 2875, `DumpResolvedTexture` 2846, `DumpBackbuffer` 3151, ITC_DUMP_PS/ITC_DUMP_BOUND (2470-2480, 2604-2613), `ITC_TRACE_FRAME` (2591-2598, uses `OMGetRenderTargets`), ITC_BLACK_DETECT (3369-3398) | readbacks | **do not port**: when any dump/trace/capture env var is set, run the executor inline (`ITC_SUBMIT_INLINE=1` auto-set), see 1.10 | keeps every debug feature working unchanged |
| `XGpuSwap` 3181-3428 | present pass (3246-3273), thumbnail readback (3277-3304), last-good copy (3305-3321), overlay (`OverlayDraw` 1829-1871), `GpuTimingEndFrame/BeginFrame/Collect`, `Present`, info-queue drain | `OP_SWAP` executed by submit; the bookkeeping (3201-3215: `g_frame++`, env-pass decision, capture-frame selection; 3234-3236 frontbuffer `GetTextureSRV`; 3425-3427 VB cleanup) stays on prepare | the frontbuffer SRV is a resolved-cache entry in practice; the thumbnail `Map(READ)` should map the staging copy one frame later instead of stalling |
| `OverlayDraw` 1845 `Map(g_ov.cb)` | | inside `OP_SWAP` (lines are formatted by prepare, copied into the op) | |
| GPU timing 1638-1722 (`Begin/End/GetData`) | | submit-owned; `OP_GPU_MARK` or the DrawCmd flag triggers `GpuStamp` | `GpuTimingPrint` (called from `XGpuIntervalStats` on prepare) reads `g_gpuAcc` -> protect with a mutex or snapshot |

Ordering is preserved because there is exactly one stream and prepare emits ops in the same program order in which the
code executes them today; the only reorder is that *decisions* (which texture, which RT, clear or not) are made earlier
than their *execution*, which is safe because those decisions only depend on prepare-side state.

### 1.7 Object lifetime

- Commands hold **raw** pointers. Owners are the prepare-side caches (`g_shaders`, `g_textures`, `g_vbs`, `g_resolved`,
  `g_colorRTs`, `g_depthRTs`, `g_samplers`, the state maps). Every place that today *releases* a D3D object must instead
  move the `CP<>` into `g_keepAlive[frame % 3]`:
  `g_textures.clear()` (1197, > 1 GB), the VB cleanup (3425-3427), RT growth (1403-1408, 1427), resolved-texture growth
  (2741-2746) and `g_resolved.erase` (776, 832, 2737, 3007), texture re-creation with a new size (1186-1194).
- Submit publishes `g_submitDoneFrame` (atomic) after executing `OP_SWAP{frame}`; prepare clears
  `g_keepAlive[(frame-2) % 3]` when `g_submitDoneFrame >= frame-2`. D3D itself keeps resources alive for GPU work that was
  already *submitted*, so the keep-alive only has to cover "enqueued but not yet executed". Three frames of lists are
  enough with a queue depth of two frames (1.9).
- Blobs > 64 KB (texture levels) use the same lists (`std::vector<uint8_t>` moved in).
- `g_texMemo` (1058-1059) holds raw `ID3D11ShaderResourceView*` and is invalidated by `g_texGen`/`g_resolvedGen`; both
  stay on prepare, nothing changes. `g_idxCache` only holds ring offsets; `OP_IDX_DISCARD` is emitted at the same time the
  cache is cleared, so a queued draw never references a discarded ring range (the GPU-side discard happens when submit
  reaches the op, after the draws that used the old contents).
- The `GetSamplerFast`/`GetBlendStateFast`/... static memos hold objects that are never destroyed: fine.
- Shader objects: `ShaderEntry` lives in `g_shaders` (unordered_map; rehash moves the entry but not the COM object);
  entries are never erased: fine.

### 1.8 Shared state and thread safety

- `g_xregs` + `XGpuWriteRegister` (36-49 new, with `g_regDirty`/`g_fetchVer`): prepare only. Never read by submit.
- `g_resolved`, `g_textures`, `g_vbs`, `g_colorRTs`, `g_depthRTs`, `g_shaders`, `g_idxCache`, `g_texMemo`, `g_bound`
  -> single-owner (prepare for all but `g_bound`); no locks needed. Add `static_assert`-style debug checks:
  `ITC_ASSERT_THREAD(prepare)` at the top of each cache function.
- Shader compile jobs: already thread-safe (`g_jobMutex`, atomic `state`, 313-363); `GetShader` runs on prepare.
- Stats counters (`g_dp[]`, `g_profDraw`, `g_drawCount`, `g_shaderPending`, ...) are read by `XGpuIntervalStats` on
  prepare; make the submit-side ones `std::atomic<uint64_t>` (relaxed) or accept torn reads (debug only).
- `g_frame`: prepare increments at swap; submit uses the frame number carried in `OP_SWAP`/`OP_FRAME_BEGIN` for dumps and
  timing. `XGpuFrame()` (used by gpu.cpp for the packet trace) stays the prepare value.
- The FPS/overlay block (3322-3366) uses `GetThreadTimes(GetCurrentThread())` - on submit that measures submit; pass the
  prepare thread's handle (from `StartGpuIfReady`) and show both percentages.
- Guest memory: only prepare reads it (decode/validate/`MemSig`/`CommittedBytes`); `g_pageMap` (kernel.cpp:129) is
  written by `GuestCommit`/`Decommit` from guest threads without a lock today - unchanged risk.
- `g_thumbData` already has `g_thumbMutex`; `XGpuWantThumb` is an atomic flag read by prepare when it builds `OP_SWAP`.
- RenderDoc (`g_rdoc` 3173-3204): `StartFrameCapture/EndFrameCapture` must be called from the thread that uses the
  context -> carry `rdocStart/rdocEnd` flags in `OP_SWAP`.

### 1.9 Frame pacing, back-pressure, deadlocks

- Depth limit: prepare counts `OP_SWAP`s enqueued minus `g_submitDoneFrame`; it blocks (WaitOnAddress) when **2** frames
  are in flight. Together with the 32 MB ring this bounds latency to <= 2 displayed frames behind the guest (one more
  than today). An env knob `ITC_SUBMIT_DEPTH=1|2`.
- The guest's own pacing does not change: the vblank interrupt (`IrqThread`) is a free-running 60 Hz timer and the CP
  interrupt / fence writes (`EVENT_WRITE_SHD` 242, `MEM_WRITE` 236, `INTERRUPT` 251, rptr writeback 509) are issued by
  prepare when it *parses* the packet, exactly as today (today they are also issued before the GPU has executed the work,
  because D3D is asynchronous). The one invariant that matters: a fence must only be written after prepare has
  **finished reading the guest data that precedes it** (index data inline in the ring for `sourceSelect == 1`, vertex
  pages, textures) - true by construction since all guest reads happen on prepare before the command is enqueued.
- The guest ring (`CP_RB_WPTR` polling, 421) drains faster, so the guest's `sub_8229D8A0` ring-full spins shrink; the
  30 fps cap (2 vblanks per swap) is then met every frame instead of 20/15 fps drops.
- `Present(1, 0)` (3400) with vsync blocks the submit thread up to one vblank; with depth 2 prepare is never starved.
- Deadlock analysis: submit waits only on the GPU (`Present`, `Map(READ)`) and on the queue being non-empty; prepare waits
  on queue space, depth, and `OP_FLUSH` completion; submit never waits on prepare -> no cycle. `WAIT_REG_MEM` (261-295)
  spins on guest memory written by prepare itself or by the CPU -> unchanged. `ITC_SYNC_SHADERS` sleeps on prepare only.
  Shutdown is `TerminateProcess` everywhere (WndProc 206, watchdog) -> no orderly join needed.
- Risk: `OP_FLUSH` inside a frame with a 2-deep queue costs up to ~2 frames of latency each time; log the count (the
  `flush reason` stat already exists) and fall back to depth 1 automatically if flushes exceed N per second.

### 1.10 Migration plan (each stage keeps the game working; verify with `run_test.ps1 -Tag x -Seconds 130 -Pad "$(cat padscript4.txt)" -EnvList 'ITC_NO_XINPUT=1'`, `python fpsum.py x`, `ITC_DUMP_EVERY=200` contact sheets via `sheet.py`, and the `[stat]` draw/resolve counts which must stay identical)

1. **Measure split costs** (no behaviour change): add `g_dp` buckets that separate "D3D call time" from "decision time" in
   `XGpuDraw`/`XGpuDoResolve`/`XGpuSwap` (today `consts` mixes fill+Map, `vb` mixes memcmp+UpdateSubresource). Gives the
   real prepare/submit split to compare against the estimate in 1.11.
2. **Refactor in place**: `XGpuDraw` -> `PrepareDraw(dp, DrawCmd&)` + `SubmitDraw(const DrawCmd&)`, called back to back
   on the CP thread; same for resolve (`PrepareResolve` + `SubmitResolve`), swap, clears, uploads (`OP_*` structs filled
   and immediately executed by `ExecuteOp`). All `g_ctx` uses end up in one `Submit*` family that takes explicit
   parameters and never reads `g_xregs`/guest memory. Expected perf-neutral; pixel-identical dumps.
3. **Queue, inline executor**: route every op through `CmdRing`; the CP thread drains the ring itself after each
   `XGpuSwap` and whenever it is full (`ITC_SUBMIT_INLINE=1` keeps this mode forever for debugging and for the dump/trace
   env vars). Validates command encoding, blob lifetime and keep-alive without threads. Add the thread-affinity asserts.
4. **Submit thread**: start it in `EnsureInit`; `OP_FLUSH` handshake; depth limit; keep-alive release on
   `g_submitDoneFrame`; stats/overlay changes. `ITC_SUBMIT_THREAD=0` falls back to stage 3. Verify fps and that
   `resolves`/`draws` per frame and the contact sheets match stage 2.
5. **Batched uploads**: constant ring with `VSSetConstantBuffers1` (both big CBs and the b1 block), batched
   `OP_IDX_APPEND`, coalesced `OP_VB_UPDATE`. Verify with `ITC_NO_CB_OFFSETS=1` fallback still working.
6. **Smaller constant slices** (per-shader used range; HLSL change -> new `shader_cache_f0` contents; use
   `ITC_SYNC_SHADERS=1` for the first verification run so no draws are skipped).
7. Optional: prepare-side `g_bound` model to shrink DrawCmds; thumbnail/black-detect readbacks deferred by one frame.

### 1.11 Expected gain and risks

- Today (ab4): CP 22 ms/frame = prepare-type work ~14-16 ms (validation 10-14, index/constant fill 2-3, lookups) +
  submit-type work ~6-8 ms. After stages 1-5: frame cost on the CP side = max(prepare, submit) + queue overhead
  (~0.1 ms) = **~14-16 ms** -> the 30 fps cap is met with margin (every frame < 33 ms), i.e. 27 -> 30 fps, and
  `vb120`-style 60 fps pacing becomes possible only once prepare also drops below 16 ms (Part 2 does that: validation
  10-14 ms -> ~1 ms, prepare ~5-6 ms). Submit alone (~6-8 ms, less after stage 5's batching: ~4-6 ms) is then the
  longest CPU stage, and the GPU (12-19 ms span) becomes the limit around 50-60 fps.
- Risks: (1) lifetime bugs (object released while queued) - mitigated by stage 3's inline executor and the keep-alive
  rule "never `Release` outside keep-alive"; (2) ordering bugs around clears/resolves - mitigated by emitting ops at the
  same program points; (3) driver behaviour when `Create*` runs concurrently with context calls (spec-legal, Intel OK in
  practice; RenderDoc serialises internally); (4) `E_OUTOFMEMORY` on big NO_OVERWRITE rings (seen once) - keep rings
  <= 32 MB and keep the DISCARD fallbacks; (5) the debug env-var paths silently assume the single-thread model - hence
  the auto-inline rule; (6) latency +1 frame (pad input feels later) - `ITC_SUBMIT_DEPTH=1` option.

### 1.12 Things in the current code that make the split harder than it looks

- `GetTextureSRV` is called from `XGpuSwap` for the frontbuffer (3234-3236) and may *flush* inside the lookup
  (1114, 1117 `FlushResolved/FlushOverlapping`): the prepare side of swap can block on `OP_FLUSH`. Acceptable, just note it.
- `GpuResolve` mutates `g_resolved` **and** issues context calls in one function (2682-2779); `g_resolvedGen++` (2682)
  must stay on prepare and the `Resolved&` reference must not be used after an emitted op that can erase entries.
- `XGpuDraw` interleaves decisions and context calls per slot (vfetch loop 2438-2456 binds while computing ranges);
  stage 2 has to collect bindings first, then emit.
- `static Decoded dec` (1153) and the `static XeCB cbv, cbp` + `lastV/lastP`-style memos (2562-2564 new) are
  single-thread assumptions: the constant memo stays on prepare (it is a decision), but the *bytes* must be copied into
  the ring per refill.
- `g_pendClear.rt` (1346) points into `g_colorRTs` (std::map, stable) - fine, but the pending clear must be emitted as an
  op referencing the RTV pointer, not the `ColorRT*`.
- The `OMGetRenderTargets` self-check (2429-2431) and the trace log (2593-2597) read context state: submit-only.
- `GpuTimingBeginFrame` is called *after* `Present` (3407) and `Collect` right after: both move into `OP_SWAP`.

## PART 2 - Guest-memory write tracking (per-4 KB-page version counters)

### 2.1 Who writes guest memory (complete inventory)

1. Generated code (`out/ppc_recomp.*.cpp`, 99 TUs, built into `itc.exe` with `-include gcc_compat.h`, `-O1`,
   `ITC_TRACE=1`; `CMakeLists.txt` 13-15, 30-34). Stores are emitted as:
   - `PPC_STORE_U8/U16/U32/U64` (recompiler.cpp 1570-1774: stb/sth/stw/std and all `*x`/`*u`/`brx` forms, stfs/stfd,
     and the vector element/partial stores stvebx/stvehx/stvewx/stvlx/stvrx at 1578/1689/1700/1713/1726). The macros are
     `#ifndef`-guarded in `ppc_context.h` 72-86 (`PPC_MM_STORE_*` 90-104 default to them), so they can be overridden from
     `gcc_compat.h`, which is force-included before `ppc_recomp_shared.h` -> `ppc_config.h` -> `ppc_context.h`.
   - `stvx/stvx128`: direct `simde_mm_store_si128((simde__m128i*)(base + ((ra +) rb & ~0xF)), ...)` (1729-1735).
   - `dcbz`/`dcbzl`: direct `memset(base + (ea & ~31), 0, 32)` / `& ~127, 128` (870-882).
   - `stwcx`/`stdcx`: `__sync_bool_compare_and_swap(reinterpret_cast<uint32_t*>(base + ea), ...)` (1751-1758, 1599).
   - `setjmp`/`longjmp` host jmp_buf into guest memory (431-436) and the `ITC_TRACE` prologue ring at `r13 + 0x800`
     (gcc_compat.h) - both virtual-range, never GPU data: **not tracked on purpose** (the prologue is hot).
2. Host code in `itc_host.dll`: `wr8/16/32/64` (guest.h 16-19; used by kernel*.cpp, gpu.cpp, main.cpp - 120 call sites),
   `GpuStore32` (gpu.cpp 34-44: MEM_WRITE, EVENT_WRITE_SHD fences, scratch writeback 72), `COND_WRITE` (386), the ring
   straddle copy (497-499, into a virtual temp), `VdSwap` (600-615, writes 64 dwords into the physical ring),
   `NtReadFile` (kernel_io.cpp 135: `ReadFile` straight into `GP(buffer)` - **the main loader of GPU data**),
   `memset(GP(...))` in `NtQueryInformationFile` 183 / `NtQueryFullAttributesFile` 215 / `XamUserGetName`-style stubs
   (kernel_xam.cpp 14, 190) / kernel_audio.cpp 178, `RtlFillMemoryUlong` 267-272 (via wr32), `RtlMultiByteToUnicodeN`
   (via wr16), `GuestAllocPhysical` zeroing of reused blocks (kernel.cpp 197 `memset`), `SpinAcquire/Release`
   `Interlocked*` on guest lock words (413-418, 426), the loader `memcpy` of the XEX image and the function table
   (main.cpp 242, 275 - before the GPU starts), `FlushResolved` (770-834) and the CPU resolve (3040-3083) in xgpu_d3d.cpp
   (`PhysPtrW` writes), and `NtWriteFile` reads only.
3. Address space (kernel.cpp 86-123): one 512 MB section mapped at `0xA0000000`, `0xC0000000`, `0xE0000000`
   (`MapViewOfFileEx` 109; `g_physAliased`); physical address = low 29 bits; the GPU side always goes through
   `PhysToVirt` = `0xA0000000 | (phys & 0x1FFFFFFF)` (gpu.cpp 14, xgpu_d3d.cpp 78). The CPU may write the same RAM through
   any of the three windows (D3D uses the uncached 0xC window for command/dynamic buffers). Virtual heap
   `0x10000000-0x40000000` and the image at `0x82000000` are not GPU-visible. `MEM_WRITE_WATCH` is impossible on a
   section view, and the `ITC_WATCH` style (`VirtualProtect` + vectored handler, main.cpp 140-176) would cost a kernel
   trap per page per frame: a counter table is the only cheap option.

### 2.2 The table

- `uint32_t ver[1 << 20]` = **4 MB**, one counter per 4 KB page of the whole 32-bit guest space (no range check in the
  store path, no branch). The three RAM windows get separate entries; consumers sum the three aliases.
- Placement: **directly below the guest base**, at `base - 0x400000`. `GuestMemInit` reserves `0x400000 + kTotal`,
  commits the first 4 MB, and sets `g_base = probe + 0x400000` (the three `MapViewOfFileEx` targets become
  `g_base + 0xA0000000 + i * 0x20000000`). Both the generated code (which already has `base` in a register in every
  function) and the DLL (`g_base`) address it with a negative displacement - no exported symbol, no `.def` change, no
  `dllimport` (the exe only imports `HostRun`, exe_main.cpp 9-14).
- 32-bit counters: 8/16-bit ones wrap too easily (a 4 KB page rewritten with 256 `stvx` wraps an 8-bit counter exactly
  back to the snapshot). A lost increment from two threads bumping the same page concurrently is harmless: the value
  still differs from every snapshot taken before.

### 2.3 Exact macro and emitter changes

`gcc_compat.h` (applies to all generated TUs and to the DLL; `#ifndef` guards in ppc_context.h pick these up):

```c
#define ITC_VERTAB_BYTES 0x400000u
#define PPC_VERTAB(base)   ((volatile uint32_t*)((base) - ITC_VERTAB_BYTES))
#define PPC_MARK_WRITE(x)  (PPC_VERTAB(base)[(uint32_t)(x) >> 12] += 1u)       /* volatile RMW: ordered after the volatile data store */
#define PPC_STORE_U8(x, y)   do { *(volatile uint8_t*) (base + (x)) = (y);                    PPC_MARK_WRITE(x); } while (0)
#define PPC_STORE_U16(x, y)  do { *(volatile uint16_t*)(base + (x)) = __builtin_bswap16(y);   PPC_MARK_WRITE(x); } while (0)
#define PPC_STORE_U32(x, y)  do { *(volatile uint32_t*)(base + (x)) = __builtin_bswap32(y);   PPC_MARK_WRITE(x); } while (0)
#define PPC_STORE_U64(x, y)  do { *(volatile uint64_t*)(base + (x)) = __builtin_bswap64(y);   PPC_MARK_WRITE(x); } while (0)
#define PPC_STORE_V128(x, v) do { simde_mm_store_si128((simde__m128i*)(base + ((x) & ~0xFu)), (v)); PPC_MARK_WRITE(x); } while (0)
#define PPC_DCBZ(x)          do { memset(base + ((x) & ~31u), 0, 32);   PPC_MARK_WRITE(x); } while (0)
#define PPC_DCBZL(x)         do { memset(base + ((x) & ~127u), 0, 128); PPC_MARK_WRITE(x); } while (0)
#define PPC_CAS32(x, old, nw) ({ bool ok__ = __sync_bool_compare_and_swap((uint32_t*)(base + (x)), (old), (nw)); PPC_MARK_WRITE(x); ok__; })
```

`(x)` is evaluated twice; the emitted operands are pure register expressions (`ctx.r11.u32 + 2608`, `ea`), so that is
safe. The macros are used only in statement position (loops at 1713/1726 use them as the loop body - `do/while` is fine).

`recompiler.cpp` (three emitters; everything else already goes through the macros):

```
870-882  PPC_INST_DCBZ/DCBZL : emit "\tPPC_DCBZ({ra +}rb.u32);" / "\tPPC_DCBZL(...)" instead of the memset
1729-1735 PPC_INST_STVX/128   : emit "\tPPC_STORE_V128({ra +}rb.u32, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)vS.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));"
1751-1758 STWCX (and 1599 STDCX): "\tcr0.eq = PPC_CAS32(ea, reserved.s32, __builtin_bswap32(rS.s32));" (64-bit twin for stdcx)
```

Quick-experiment shortcut: a python regex over `out/ppc_recomp.*.cpp` can rewrite those three patterns without touching
the recompiler (the lines are single-line and regular), but the durable change is the emitter patch (the work dir already
has `patch_recompiler.py` for the same kind of edit).

Host side (`guest.h`): `inline void GuestMarkWrite(uint32_t a, size_t n)` bumping every page of `[a, a+n)`;
`wr8/16/32/64` call it with n = 1/2/4/8; explicit calls after `ReadFile` (NtReadFile, n = `got`), the `memset`/`memcpy`
sites listed in 2.1, `GpuStore32`, `Interlocked*` in SpinAcquire/Release (lock words are never GPU data, but it costs
nothing), `FlushResolved` / CPU resolve (`PhysPtrW` ranges = `r.bytes` / `tiledBytes`), `GuestAllocPhysical` zeroing.
`NoteMemWrite`/`WrittenSince`/`g_memSeq` (173-190) become redundant once every consumer checks versions; keep them
during migration, delete afterwards.

### 2.4 Memory ordering and cost

- Writer order is **data store, then counter increment** (program order). x86-64 TSO makes stores visible in program
  order, and both accesses are `volatile`, so the compiler cannot reorder them either. Consequence for a reader that
  snapshots the counter *before* reading the data: if any part of a store becomes visible during/after the read, its
  increment becomes visible after the snapshot, so the next comparison differs and the data is re-read. The reverse
  order (counter first) would allow a reader to record the new version together with old data - never do that.
- Reader protocol (prepare thread): `v0 = RangeVer(range)`; read/hash/copy the data; store `v0` as the entry's version.
  A torn read (writer mid-flight) is uploaded for at most one validation period, exactly like today's `memcmp`.
- No atomics, no fences: a plain `shr` + `add [mem], 1` (GCC emits load/add/store for the volatile RMW: 4 instructions
  per store). Lost increments between threads are harmless (2.2). Cost per store ~1-2 cycles throughput plus one extra
  L1 line (16 pages per 64 B line, so the working set of counters is tiny); the only pathological case is a tight loop
  storing to one page (memcpy-like `stvx`/`stw` loops): the RMWs on the same counter serialize at store-forwarding latency
  (~5 cycles each), i.e. such loops run at ~1-3 GB/s instead of memory speed. The guest main thread currently spends most
  of its time spinning in the ring-wait (`sub_8229D8A0`), so this is affordable; measure with `ITC_PROF=1` before/after.
  If it matters, the branchy variant `if ((x) >= 0xA0000000u) PPC_MARK_WRITE(x)` skips the virtual heap (game state)
  at the price of a well-predicted compare per store.
- Multi-byte stores can straddle a page boundary (unaligned `stw`/`std`); only the first page is bumped. Consumers
  therefore always include the page **before** a range (`p0 - 1`) in their sum; for per-page vertex-buffer checks the
  stored version of page p is `ver(p-1) + ver(p)` (a write to page q re-uploads q and q+1: 8 KB instead of 4 KB for
  dynamic buffers, zero cost for static ones). `stvx`/`dcbz` are aligned and never straddle.

### 2.5 Consumers (xgpu_d3d.cpp)

```cpp
inline const volatile uint32_t* VerTab() { return (const volatile uint32_t*)(g_base - ITC_VERTAB_BYTES); }
inline uint32_t PhysPageVer(uint32_t page)                      // page = (phys & 0x1FFFFFFF) >> 12, three RAM windows summed
{ const volatile uint32_t* t = VerTab(); return t[0xA0000u + page] + t[0xC0000u + page] + t[0xE0000u + page]; }
inline uint64_t RangeVer(uint32_t phys, size_t bytes)           // monotonic sum => equal means unchanged (mod 2^64)
{
    phys &= 0x1FFFFFFF; uint32_t p0 = phys >> 12, p1 = (uint32_t)((phys + bytes - 1) >> 12);
    if (p0) p0--;                                                // the page before: stores spilling into the range
    uint64_t s = 0; for (uint32_t p = p0; p <= p1; p++) s += PhysPageVer(p); return s;
}
```

| Consumer | Today | With versions |
|---|---|---|
| `GetVertexBuffer` 1267-1312 | shadow copy + per-page `memcmp` (`pageStamp`, `pageStable`, `kRecheckPeriod`, `g_forceVbCheck`) | `VBEntry::pageVer[]` (`ver(p-1)+ver(p)` at last upload); per draw, for the pages of the range: 6 loads + compare; on mismatch copy the page into `OP_VB_UPDATE` and store the new version. No shadow (saves `g_vbBytes` of RAM and the memcpy), no staggering, no per-frame stamp needed (a 256-page range costs ~0.2 us). `small` buffers stop being compared on every draw |
| `GetTextureSRVSlow` 1137-1152, `GetCubeSRV` 1017-1031 | `DecodeTexture(hashOnly)` of the whole base level + `HashMipRegion` once per frame, staggered every 8th frame after 3 stable checks | `CachedTex::ver = RangeVer(base, footprint)` (+ mip region); check once per frame (`checkedFrame`) as now - 3 loads per page, a 4 MB texture = 1024 pages = ~1 us instead of ~0.4 ms of CRC; mismatch -> decode + `OP_TEX_UPDATE`. `g_hostWriteSeq` (bumped by flush/NtReadFile paths only) replaces `g_memSeq` in the per-frame fast path and in `TexMemo.seq` |
| `GetIndexList` 2272-2278 | `Hash64` of the raw index bytes on every draw (part of the 1 ms `idxbuild`) | `IdxEntry::ver = RangeVer(dmaPhysBase, need)` compared on hit; the hash is dropped from the hit path (key = k0/k1/k2; on a version mismatch, re-hash the raw indices and look the content hash up as a secondary key so an address that alternates between two meshes keeps both lists cached, as today). Inline-index draws (`sourceSelect == 1`, gpu.cpp 316-321) point into the ring and are rewritten every frame, so their version always differs: keep converting them (as today) |
| `Resolved::memSig` 750-761, 1103, 2751 | `MemSig` samples 1024 dwords of the destination region (misses writes between samples) | `RangeVer(base, r.bytes)` taken at resolve time, compared once per frame (`sigFrame`) - exact instead of sampled |
| `TexMemo` 1058-1075 | `m.seq == g_memSeq` | `m.seq == g_hostWriteSeq` (guest-CPU writes are caught by the per-frame check of the cached entry; a mid-frame CPU write to a texture already validated this frame is missed today as well - if that ever shows, re-check per render pass instead of per frame: it is only loads) |
| `GetVertexBuffer` CreateBuffer path 1278-1292, `XGpuDoResolve` resolve-rect vertex read 2946-2956 | read guest memory | unchanged (reads) |

### 2.6 Safety net and switches

- `ITC_VERTAB=0`: ignore the table, use today's hash/memcmp paths (keep both code paths until the audit has run clean
  for a while).
- `ITC_VERTAB_AUDIT=N` (default 4): every N-th frame, a round-robin 1/16 slice of `g_vbs`/`g_textures`/`g_idxCache`
  entries is re-validated the old way (hash/memcmp) *in addition*; "content changed but version unchanged" logs
  `[vertab] MISSED WRITE base=... bytes=... (entry kind)` and forces the upload. This catches any host write path not
  listed in 2.1 (future kernel imports, XMA decode output when implemented, save/profile loads). Cost: ~1/64 of today's
  validation per frame.
- Periodic full revalidation: on `ITC_VERTAB_FULL=F` (default 0 = off; suggest 900 = every 30 s during bring-up) all
  entries are re-hashed once; the stat line reports audit hits so a missed path is visible in `run_test` output.
- Overflow of the queue of page bumps is impossible (counters, not a log); the only failure mode is a missed bump, which
  the audit reports.

### 2.7 Build impact

- A macro change in `gcc_compat.h` touches **all 99 generated TUs** (`out/ppc_recomp.*.cpp`, 96 MB of source). From
  `rt_build/.ninja_log`: the last full batch compiled 99 objects in **175 s wall** (-j8, 1267 s CPU, mean 12.8 s, max
  39 s), and linking `itc.exe` takes **152 s**. So `./build.sh` after the header change is ~5.5 min; run it in the
  background (long foreground commands are moved to the background anyway). NOTES.md quotes "~2-5 min" for a partial
  regeneration (changed generated files + one relink); the relink alone is 2.5 min of that.
- Changing the recompiler (`stvx`/`dcbz`/`stwcx` emitters) additionally needs: rebuild XenonRecomp in
  `C:\itc-xr\build_ascii` (cmake, ~1 min for one file), then `bin/XenonRecomp.exe itc.toml ../tools/XenonRecomp/XenonUtils/ppc_context.h`
  (`regen.log`: **2 m 25 s**), then the full generated-code build above -> **~8-9 min** end to end. Kill any running
  `itc.exe` first (it locks `itc_host.dll`).
- The DLL side (`guest.h`, `kernel*.cpp`, `gpu.cpp`, `xgpu_d3d.cpp`) rebuilds in seconds (`xgpu_d3d.cpp` ~10 s). Every
  TU of the DLL also includes `gcc_compat.h` (same `-include`), so host code that uses the `PPC_STORE_*` names (none
  found; host code uses `wr32` etc.) would pick up the new macros automatically.
- Nothing else changes: `shader_cache_f0/` stays valid (no HLSL change in Part 2).

### 2.8 Risks / open points

- Stores by code paths that bypass the macros but target physical memory: the audit will tell; the known ones are
  listed in 2.1. XMA/audio output buffers and future save-game code must use `GuestMarkWrite`.
- `GuestMemInit` retry loop (98-116): moving `g_base` by 4 MB changes the alignment assumption
  `PPC_FUNC_PROLOGUE: ((size_t)base & 0x1F) == 0` - keep `g_base` 64 KB aligned (reserve granularity) by reserving
  `kTotal + 0x400000` in one call, which VirtualAlloc aligns to 64 KB.
- The 4 MB table's pages must be committed before any guest code runs (they are, in `GuestMemInit`).
- Guest-CPU write patterns that touch a page every frame but do not change the bytes (e.g. re-copying identical data)
  will re-upload every frame where today's memcmp would skip; the VB consumer can keep a cheap `memcmp` against the
  previous upload *only for pages whose version changed* if this shows up in `[stat]` (upload bytes counter to add).

## Other findings (noticed while reading; not part of the two designs)

1. **Inline-index draws after a ring straddle read garbage** (gpu.cpp 491-501 + 316-321): when the packet is copied to the
   `GuestAlloc`'d temp (`tmp` at 0x1xxxxxxx), `dp.dmaPhysBase = q & 0x1FFFFFFF` is taken from the *virtual* temp address
   and later read through `PhysPtr` (0xA0000000 window) -> wrong memory for that draw. Rare (only packets that wrap the
   64 KB ring), but a cheap fix: copy into a `GuestAllocPhysical` block.
2. `GetVertexBuffer` compares `small` (<= 32 KB) buffers on **every draw** (1295-1298 skip only big ones): N draws sharing
   one small dynamic buffer cost N x 32 KB of memcmp per frame. Part 2 fixes it; a one-line stamp per (buffer, frame,
   range) would already help.
3. `kIdxRing` comment (1355 new) still says "startVertex travels through a float constant, exact below 2^24"; it now
   travels as `uint` in `xe_draw.x`, so the 2^24 limit is gone.
4. `BindDrawConstants` (2164-2183 new) maps the 4 MB b1 ring **per draw** (~2000 Map/Unmap pairs per frame); batching
   per N draws (one map, N slices) is the cheap interim step before the full constant ring of 1.4.
5. `MemSig` (750-761) samples at most 1024 dwords of a resolved region, so a CPU write to a resolved buffer between the
   samples is not detected (e.g. the game writing into a resolve target before re-reading it as a texture).
6. `GetIndexList` returns `&it->second` / `&e` into `g_idxCache` (unordered_map): `AppendIndices` may `clear()` the map
   on wrap *before* the entry is inserted (handled, 2288), but any future second insertion while holding an `IdxEntry*`
   would dangle - worth a comment.
7. `XGpuWriteRegister` (36-49 new) ignores same-value writes; `WriteRegister` in gpu.cpp (57-76) still mirrors them to
   MMIO memory and runs the scratch writeback, which is right (the CPU can observe those).
8. `FlushOverlapping` (837-851) restarts the scan after every flush: O(n^2) in `g_resolved` size; harmless today (small
   n) but will matter if the resolved table ever holds hundreds of entries.
9. `ITC_TRACE` prologue (gcc_compat.h) writes 16 bytes into the PCR ring on every generated function call; NOTES item 6
   already suggests dropping it for the final build. It is also the reason not to route it through `PPC_MARK_WRITE`.
10. The `static Decoded dec` reuse in `GetTextureSRVSlow` (1153) and the per-slot `GetSamplerFast` memo keyed only by
    `d0/d3` (1240-1249) are fine today but are the two places that assume one thread (see 1.12).
