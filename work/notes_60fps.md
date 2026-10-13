# TXR: ITC - frame pacing, simulation time step and the 60 fps question

Research notes (read-only analysis of `out/ppc_recomp.*.cpp`, the runtime in `runtime/src`, and the decrypted XEX image).
Date: 2026-10-05. Everything below was read from the recompiled code; guest addresses are `sub_8XXXXXXX`, file:line refer to `C:\itc-work\out\`.
To read `.rdata`/`.data` constants and vtables the XEX was decrypted with a throw-away pure-Python AES script in the scratchpad
(`xexdec.py` -> `image.bin`; the XEX is "basic"-compressed + encrypted, `extracted/default.xex` is NOT plaintext, so `grep` on it finds nothing).

## TL;DR

* The game presents every **2nd vblank by its own choice**, not because of D3D: the D3D device is created with
  `PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE (0x80000000)` (D3D's own throttle never asks for more than 1 vblank; the
  loading thread sets interval 1) and the game registers its own vertical-blank callback
  (`sub_8258D4D8`) that counts vblanks and signals an event. Before every swap the game waits until **`[gfx+304]` (= 2) vblanks**
  have been counted since the previous frame started (`sub_8258DAA8`). The value 2 is a literal argument in the application
  constructor: `li r6,2` at guest **0x820923D4** (`sub_82092358`, ppc_recomp.0.cpp:5989).
* The simulation is stepped **once per presented frame with constants baked for 30 Hz**: `30.0f` at 0x82000D44 (seconds->frames in ~25
  compilation units), `1/30` at 0x820029EC (physics/animation), integer frame timers everywhere (e.g. the Xenia-patch site
  `cmplwi r11,60` = "wait 60 frames = 2 s" in `sub_8218E420`). No elapsed-time/vblank-count catch-up logic exists; the total vblank
  counter the callback maintains (0x8296BD2C) is never read. That is why `ITC_VBLANK_HZ=120` doubled the game speed.
* Correct-speed 60 fps therefore cannot come from the runtime alone. The least invasive *experiment* is a one-instruction patch (interval
  2 -> 1), but it just reproduces the double-speed result. A real 60 fps mode needs a game-logic patch set (halve the dt constants, double
  the frame-count timers), which is the classic "60 fps mod" job: days of iteration, high risk of subtle gameplay changes. Recommendation:
  keep 30 fps pacing and spend the effort on holding a solid 30 (the vblank alignment turns a late frame into 20/15 fps).

## 1. How the game decides when to swap

### 1.1 D3D side: interrupt handler, vblank counter, swap-interval machinery (all in the Xbox 360 D3D runtime linked into the XEX)

* `sub_822AD190` (ppc_recomp.48.cpp:7429-7515) is D3D's engine init: `VdInitializeEngines`, then
  `VdSetGraphicsInterruptCallback(r3 = 0x822A0000 - 8544 = 0x8229DEA0, r4 = device)` (48.cpp:7494-7502).
  So the kernel interrupt callback our `IrqThread` calls is **`sub_8229DEA0`**, with r3 = source, r4 = D3D device.
* `sub_8229DEA0` (45.cpp:24473-24630), the graphics interrupt handler:
  * source 1 (command-processor interrupt): reads the scratch-register mirror block `[dev+10772]` (`SCRATCH_ADDR` block, physical
    0xA0113000 in our runtime), takes `callback = [[dev+10772]+16]`, asserts it is not the 0x0BADF00D sentinel (45.cpp:24493-24510),
    calls `callback([[dev+10772]+20])`, then clears this CPU's bit in `[[dev+10772]+0]` under a spin lock (45.cpp:24525-24553).
  * source 0 (vblank): only if bit 0 of the display interrupt status register `0x7FC80000+25924 = 0x7FC86544` is set
    (45.cpp:24561-24569; this is why `IrqThread` sets that bit, gpu.cpp:131). Then:
    ```
    45.cpp:24570-24579   lwz r10,15148(r31) ; addi r10,r10,1 ; stw r10,15148(r31)      -> [dev+15148]++   (vblank counter)
    45.cpp:24572-24591   lwz r11,15156(r31) ; ble ; addic. r11,r11,-1 ; stw ; bne       -> if ([dev+15156] > 0 && --[dev+15156] == 0)
    45.cpp:24592-24599       stw r10(=0),4(r11=[dev+10772]) ; [dev+15152] = [dev+15148]  ->   clear "swap pending" flag, remember vblank of swap
    45.cpp:24601-24623   lwz r11,15144(r31) ; beq ; ... mtctr r11 ; bctrl               -> if ([dev+15144]) callback(&{vblank=[15148], swap=[15160], 0})
    ```
    `[dev+15144]` is set by `sub_8229DA20` (45.cpp:23729-23735, `stw r4,15144(r3)`) = `D3DDevice_SetVerticalBlankCallback`.
    `[dev+15148]` = vblank count, `[dev+15152]` = vblank count at the last swap, `[dev+15156]` = countdown, `[dev+15160]` = swap count.
* The presentation interval lives in **`[dev+13240]`**: setter `sub_822A2A40` (47.cpp:169-175, `stw r4,13240(r3)`), getter
  `sub_822A2A48`. The setter is the only writer of that field. It is entry 94 of the render-state accessor table at 0x82638D60
  (`{getter 0x822A2A48, setter 0x822A2A40, default 0}`); the device reset `sub_822ACB90` (48.cpp:6527-6701) saves the field
  (6549 `lwz r25,13240(r31)`), runs every setter with its table default through the per-device table `[dev+56+4*state]`
  (6551-6590, indirect calls) and restores it (6695-6701 `sub_822A2A40(dev, r25)`). The only direct caller with a literal value is the
  loading-screen thread (`sub_822A2A40(dev, 1)`, 1.cpp:15620), which never restores it. Whether CreateDevice pushes the game's
  `pp.PresentationInterval` (= IMMEDIATE, see 1.2) through that table was not traced; every possible value (0, 1 or 0x80000000) makes
  D3D's throttle demand at most one vblank per swap, so it is never what limits the game to 30 fps.
  `D3DRS_PRESENTIMMEDIATETHRESHOLD` is bits 16..22 of `[dev+11596]` (setter `sub_822A2A50`, 47.cpp:187-197).
* `sub_822A0718` (45.cpp:30667-30909) is the D3D-level throttle, called right before Swap. Quoted logic:
  ```
  45.cpp:30681-30688  lis r11,-32768 ; lwz r10,13240(r31) ; cmplw ; beq -> loc_822A088C   // interval == 0x80000000 (IMMEDIATE): do nothing
  45.cpp:30717-30783  type-0 write reg 1401 (0x579 = SCRATCH_REG1) := 1                      // GPU mirror -> [[dev+10772]+4] = 1 ("swap pending")
                      WAIT_REG_MEM(0xC0043C00) info=19 (==, memory) addr=phys([dev+10772]+4)|2 ref=1 mask=-1 poll=256
  45.cpp:30785-30817  interval code: [13240] <=1 -> 1, 2 -> 2, 4 -> 3 (D3DPRESENT_INTERVAL_ONE/TWO/.. as on PC D3D9: 1,2,4)
  45.cpp:30819-30839  r7 = (interval << 16) | threshold ; sub_8229E9D0(dev, ring, 0, &sub_822A0688 (addi r6,r10,1672), r7)
                                                                     // CP interrupt packet: callback = sub_822A0688, context = r7
  45.cpp:30850-30893  WAIT_REG_MEM info=19 addr=phys([dev+10772]+4)|2 ref=0                  // GPU stalls until the CPU clears the flag
  ```
* `sub_822A0688` (45.cpp:30587-30664) is that CP-interrupt callback (runs inside `sub_8229DEA0`, source 1):
  ```
  30591-30610  r10 = arg>>16 (interval) ; dev = **0x820107B0 ; r8 = (interval - [dev+15148]) + [dev+15152] ; [dev+15160]++
  30614        if (r8 > 0) -> [dev+15156] = r8            // arm the vblank countdown: flag cleared by the vblank ISR r8 vblanks later
  30616-30643  else percent = reg[0x7FC86530]*100 / (reg[0x7FC86584] & 0xFFF) + 1, clamp 100   // scanline position
               if (percent > threshold) -> [dev+15156] = 1 // too late in the frame: wait for the next vblank
  30644-30653  else [[dev+10772]+4] = 0 ; [dev+15152] = [dev+15148]   // present immediately
  ```
  So on real hardware the GPU itself waits (`WAIT_REG_MEM`) for the CPU/vblank decision, and the CPU gets blocked indirectly when the
  ring fills: `sub_8229EF48` (45.cpp:26979, ring reserve) -> `sub_8229D8A0` (45.cpp:23491-23605) spins on the rptr write-back
  `[dev+10768]` with a 5000 ms timeout (`sub_825B9A60` = tick count) before declaring a GPU hang (`sub_822ACAA8`). That is the
  "ring-full spin" the profiler sees.
* `sub_822A08A0` (45.cpp:30912-31747) is `D3DDevice_Swap`: reserves 64 dwords (`sub_8229F148(dev,64)`), calls
  `VdSwap(ring+4, frontbuffer_fetch+28, [dev+10768]+8, &outstruct, sysbuf_id, &fb_addr, &format, &colorspace([dev+13256]), &w, &h)`
  (45.cpp:31089-31129), advances the write pointer by 64 dwords (31130-31133), inserts the swap fence ring (`sub_822B1F68`, 8 slots,
  `[dev+20076] += 2`), kicks the ring (`sub_8229EC00`), updates frame statistics (`sub_822B20D8`, mftb at 48.cpp:19597) and resets
  state (`sub_822A3B90`). **Swap itself never blocks on vblank**; in our runtime the XE_SWAP packet presents immediately
  (gpu.cpp:256-260 -> `XGpuSwap`).

### 1.2 Game side: this is where the "every 2nd vblank" comes from

* Game graphics object (`gfx`, embedded at app+64), created in the app constructor `sub_82092358` (0.cpp:5916-6250):
  ```
  0.cpp:5972  sub_8258DB18(gfx)              // ctor: zeroes fields, g_8296BD30 = 0 (85.cpp:12948)
  0.cpp:5987-6003  li r24,1 ; li r6,2 ; li r5,0 ; li r4,1 ; mr r3,r27 ; ... ; bl sub_8258D568   (guest 0x820923D0..0x820923EC)
  ```
  `li r6,2` is at guest **0x820923D4** (encoding 0x38C00002).
* `sub_8258D568(gfx, 1, 0, interval=2)` (85.cpp:12021-12530) = device creation:
  * `XGetVideoMode(&gfx+152)` (85.cpp:12060) - the refresh-rate field (+0x14) is never read; only width/height/widescreen are used
    (here and in `sub_82482Bxx`, 74.cpp:2717-2750).
  * Presentation parameters at gfx+16: 1280x720, format 0x18280186, SwapEffect 1, AutoDepthStencil 1 (0x1A220197),
    **PresentationInterval `[gfx+68] = 0x80000000` = IMMEDIATE** (85.cpp:12092-12107 `lis r10,-32768 ; stw r10,68(r31)`),
    DisableAutoFrontBuffer = 1 (`[gfx+76]`), i.e. the game manages its own two front buffers (`sub_822A5D08` x2 -> gfx+272/276).
  * `[gfx+304]` = vsync interval derived from the ctor argument r6 (85.cpp:12096-12145): 0/1 -> 1, **2 -> 2**, 4 -> 3,
    0x80000000 -> 0. With the literal 2 above, **`[gfx+304] = 2`**.
  * `sub_8229DB38(...)` = D3D CreateDevice (85.cpp:12213), then
    `sub_8229DA20(dev, 0x82590000 - 11048 = 0x8258D4D8)` (85.cpp:12372-12380) = **SetVerticalBlankCallback(sub_8258D4D8)**.
* `sub_8258D4D8` (85.cpp:11928-11953), the game's vblank callback, runs inside `sub_8229DEA0` on our IrqThread:
  ```
  g_8296BD2C++        (total vblanks; NEVER read anywhere else - grep of -17108(r) finds only this function)
  g_8296BD30++        (vblanks since frame start)
  sub_8258DD78(&g_8296BD34)  -> NtSetEvent(handle)   (85.cpp:13259-13270 -> sub_825BA1E0, 91.cpp:17450)
  ```
* Frame start: `sub_8258D958` (85.cpp:12602-12638) does `sub_822A8A60(dev,0,32,96)` and **`g_8296BD30 = 0`** (85.cpp:12628).
  It is called once per frame from `sub_82098940` (1.cpp:10043-10105: `scene->vtbl[12]()`, `sub_8258D958`, `gfx->vtbl[20](...)`).
* Present: `sub_82091A60` (0.cpp:4489-4524) -> `sub_8258DC58(gfx)` (85.cpp:13072-13136):
  `sub_822A8A60(dev,0,0,0)`, **`sub_8258DAA8(gfx)`**, `sub_822A08A0(dev, frontbuffer[[gfx+268]], 0)` (= D3D Swap), flip `[gfx+268]`.
  `sub_82091A60` is registered as task id 5 in the app ctor (0.cpp:6167-6201, `addi r11,r11,6752` -> 0x82091A60).
* **`sub_8258DAA8`** (85.cpp:12813-12881) is the actual pacing decision:
  ```
  12831  r27 = g_8296BD30                      // snapshot
  12835  sub_822A0718(dev)                     // D3D throttle: no-op for IMMEDIATE, otherwise interval 1 (>= 1 vblank) - see 1.1
  12836-12841  if ([gfx+304] == 0) return      // IMMEDIATE: never wait
  loc_8258DAE0:
  12849-12856  if ([gfx+304] > g_8296BD30) goto wait          // fewer than 2 vblanks since frame start
  12857-12862  if (g_8296BD30 != r27) return                   // and at least one vblank since we entered -> go
  wait: 12864-12874  sub_8258DD90(&g_8296BD34, INFINITE) ; goto loc_8258DAE0
  ```
  `sub_8258DD90` (85.cpp:13286-13326) -> `sub_822970A0` (45.cpp:4108) -> `sub_82297D58` (45.cpp:6130-6193, alertable
  `NtWaitForSingleObjectEx`, re-waits on STATUS_ALERTED) - exactly the profiler stack of the main thread.
  Consequence: a frame shorter than 2 vblanks presents on the 2nd vblank (30 fps); a frame of 2..3 vblanks presents on the 3rd
  (20 fps), etc. There is no `KeQuerySystemTime`/timebase involvement at all.
* The loading/"now loading" thread `sub_8209ACB8` (1.cpp:15572-15679) is the only code that uses the D3D throttle: it sets
  `sub_822A2A40(dev, 1)` (1.cpp:15614-15620) and loops `Clear -> sub_822A0718 -> Swap`, i.e. the loading animation runs at one swap
  per vblank (60 Hz) through the `sub_822A0688` countdown path - that is why the intro/loading also sped up with `ITC_VBLANK_HZ=120`.

### 1.3 Runtime side (for reference)
`gpu.cpp:104-138` IrqThread (60 Hz, `ITC_VBLANK_HZ`), sets bit 0 of 0x7FC86544 then `DispatchIrq(source 0)`; CP interrupts are
raised by the INTERRUPT packet (opcode 0x54, gpu.cpp:251-255) and dispatched with per-CPU bits; `WAIT_REG_MEM` is polled
(gpu.cpp:261-295, 25 s cap); scratch registers 0x578.. are mirrored to memory (gpu.cpp:63-73) which is what makes
`[[dev+10772]+4/+16/+20]` work; `VdSwap` (gpu.cpp:600-615) builds the XE_SWAP packet; `FillVideoMode` reports 60 Hz (unused by the game).

## 2. Simulation time step

Evidence that the game advances a fixed amount per presented frame and never measures time:

1. No catch-up logic: the vblank total `g_8296BD2C` is written by the callback and never read; `g_8296BD30` is read only by the pacing
   wait. `KeQuerySystemTime` is called only from xapi helpers (45.cpp:2614, 6288 - time-zone/file-time code, `mulli ...,60` there is
   minutes). The seven `mftb` sites are: D3D block-time statistics `sub_8229D6F0`/`sub_8229D720` (45.cpp:23241, 23268), D3D frame
   statistics `sub_822B20D8` (48.cpp:19597), xapi `QueryPerformanceCounter` = `sub_825BA0C8` (91.cpp:17240/17247) and an XMA/audio
   timeout loop `sub_8245D288` (71.cpp:8121/8131). The QPC users are a stopwatch class (`sub_8258CFA0/sub_8258D030/sub_8258D090`,
   85.cpp:11125-11270; used at 1.cpp:3703 in an async-job loop), ms conversions in `sub_824CF448` (79.cpp:25415) / `sub_82487458`
   (74.cpp:16950), network/audio code (`sub_8257D518`, `sub_82580028`, `sub_82461F20`) and the CRT clock (`sub_825C09A8`). None of
   them feed the game update.
   Side note: the recompiler maps `mftb` to `__rdtsc()` (host TSC, GHz) while `KeQueryPerformanceFrequency` returns 50,000,000
   (kernel.cpp:442). Any code that divides TSC deltas by that frequency sees wrong seconds; the XMA wait in `sub_8245D288`
   (timeout = freq/8 ticks, 71.cpp:8133-8143) therefore times out after ~2 ms of TSC instead of 125 ms. Worth checking for the music
   problem, irrelevant for pacing.
2. The frame-rate constants are compile-time: the decrypted `.rdata` holds `30.0f` at **0x82000D44** (loaded as `lfs fX,3396(r11)` after
   `lis r11,-32256` in 3.cpp:8420/8722, 4.cpp:12051, 5.cpp, 6.cpp:20510, 8.cpp:12540/13794/14519/15306, 13.cpp:14194/17563,
   15.cpp:12288, 18.cpp:15640/16153, 20.cpp:3993, 21.cpp:16835, 34.cpp:27456, 72.cpp:18336, 74.cpp:3716/3855/15613, 77.cpp:11165,
   84.cpp:16246/23391/28814 - the usual "seconds * 30 = frames" / "frames / 30" conversions), **`1/30 = 0x3D088889` at 0x820029EC**
   (`lfs ...,10732(r11)`: `sub_8211F530` 15.cpp:6667, `sub_820C23E8` 6.cpp:9972, `sub_820B1790` 4.cpp:12102, `sub_8210D890`
   13.cpp:5665, 13.cpp:9142, 16.cpp:8858, 5.cpp:5060, 6.cpp:3966) and **`1/60 = 0x3C888889` at 0x820029E8** (`lfs ...,10728(r11)`:
   `sub_820B0EE8` 4.cpp:10889 stores it into an object field `[obj+2352]`, `sub_820D0D60` 7.cpp:8106, `sub_8224AB50` 37.cpp:7639,
   `sub_82155E58` 20.cpp:10470). 1/60 and 1/30 sit next to each other in the same constant pool (0x820029E4 = 1/600, 1/60, 1/30,
   then 1.3, 3.6 (km/h), ...), i.e. the physics unit uses both (sub-step or half-frame terms), it is not a selectable mode.
   There is no float 1/30 or 30.0 selected through a global; the `.data` section additionally contains ~60 static objects of the form
   `{ptr, 0.0333333, 0.0166667}` (0x826002EC/0x826002FC/0x82600300, 0x82600B74/78, 0x82601834.., 0x82602544.. - offsets
   748/764/768, 2932/2936, 6196.., 9540.. from 0x82600000; one code reader found: 6.cpp:3892 `lfs f1,6200(r11)` after
   `lis r11,-32160`). They look like per-object rate pairs (30 Hz and 60 Hz values) - worth a look if a 60 fps patch is attempted,
   but nothing switches between them at runtime as far as the code shows.
3. Integer frame timers are everywhere. The community Xenia patch site is one: **`sub_8218E420`** (23.cpp:31736-31886):
   ```
   31753-31811  if ([this+9608] == 0) { if (sub_8211BE98(0) && (9) && (10) && sub_8218E2D8(g) && sub_8218E320(g)) [this+9608] = 1 }
                // "all resources/streams ready" latch
   31813-31838  counter g_826D841C: if ((counter / 15) & 1) == 0 -> sub_82106540(100, 30, "...", 0)   // debug/overlay text blink, 15-frame period
   31841-31849  counter++ ; store                                                                   // one increment per call = per frame
   31852-31855  cmplwi cr6,r11,60 ; blt -> return          <- guest 0x8218E4EC, the Xenia patch (60 -> 26)
   31856-31872  counter = 0 ; [this+9604] = ([this+9612] != 0) ? 9 : 4   // advance the state machine after 60 frames (2 s at 30 fps)
   ```
   It is a post-race/transition state handler: wait 60 frames after everything is ready, then go to state 9 or 4. Shortening to 26 frames
   just makes the transition faster (the Xenia black screen is presumably a hang of that state; it is not frame-rate logic). Other
   examples: `li r11,30` reload of a per-object frame counter `[obj+2392]` in `sub_820B1790` (4.cpp:12078-12081), `li r29,30` in
   `sub_8211F530` (15.cpp), the 15-frame blink above. Doubling the frame rate doubles all of these.
4. The empirical result (`ITC_VBLANK_HZ=120` -> ~2x speed, 45-58 fps) matches: one tick per presented frame, frames paced by vblank count.

## 3. Options for 60 fps at correct speed

(a) Report vblanks differently from the runtime: **not possible**. The only quantity the game uses is "how many vblank callbacks since
    frame start"; any faster cadence runs the game faster. (`ITC_VBLANK_HZ` already demonstrates this.) Effort 0, result = wrong speed.

(b) Present every vblank by patching the game's interval (interval 2 -> 1): one instruction, `li r6,2` -> `li r6,1` at 0x820923D4
    (0.cpp:5989), or equivalently a mid-asm hook in `itc.toml`:
    ```
    [[midasm_hook]]            # runtime/src/hooks.cpp: void SetVsyncInterval1(PPCRegister& r6) { r6.u32 = 1; }
    name = "SetVsyncInterval1"
    address = 0x820923D4
    registers = ["r6"]
    after_instruction = true
    ```
    (or hook `sub_8258D568` entry 0x8258D568 with r6). Then `[gfx+304] = 1` and `sub_8258DAA8` waits for a single vblank.
    Effort: 15 minutes + recompile/relink. Risk: none for stability, but the game runs 2x fast - same as the experiment, only cleaner
    (the loading thread, D3D statistics and audio are untouched). Also note the renderer currently needs ~18.6 ms of CP-thread time per
    race frame (NOTES.md), so even with correct speed 60 fps needs ~2x more renderer throughput first.

(c) Real 60 fps = (b) + make one frame advance half as much. There is no dt variable to halve; it would be a patch set:
    * physics/animation float constants: 0x820029EC (1/30 -> 1/60), 0x82000D44 (30.0 -> 60.0; check each of the ~25 users, some may use
      30.0 as a speed/scale rather than fps), the 1/60 users (which then mean "1/120"), the `.data` rate pairs above;
    * every integer frame counter/threshold (`60`, `30`, `15`, ... in dozens of functions - e.g. 0x8218E4EC, `sub_820B1790`), AI
      state timers, replay/ghost data stored in frames, music/SFX cue timing, per-frame damping factors (x *= 0.9 per frame does not
      scale with dt and needs sqrt-style retuning), menu/animation scripts in frames (likely data-driven in DATA/ files).
    Memory patches can be applied by the runtime after `Image::ParseImage` maps the image (main.cpp:227) - `.rdata` is just guest memory
    for the recompiled loads (`PPC_LOAD_U32(0x82000000 + 10732)`), so no recompile is needed for constants; code patches need
    mid-asm hooks or an edited TOML `functions`/patch. Effort: days to weeks of trial and error (this is a classic console 60 fps mod);
    risk: high (physics/AI behaviour drift, desyncs, half-speed UI timers you forgot). Not recommended unless 60 fps is a hard goal.

(d) Interpolated or "render twice" 60 fps: the engine has no interpolation; presenting the same simulation state twice gives nothing.
    Impractical.

Recommendation: stay with 2 vblanks/frame. If smoother gameplay is the real goal, the actionable items are performance (keep every frame
under 2 vblanks so the pacing never drops to 3 = 20 fps) and, if desired, an opt-in env var that installs the (b) hook for experiments.
The one timing bug found on the way - `mftb`/`KeQueryPerformanceFrequency` unit mismatch - is worth fixing independently (make
`KeQueryPerformanceFrequency` return the host TSC rate or translate `__rdtsc` to a 50 MHz timebase), it affects audio timeouts, not fps.

## 4. Reference list (guest address -> file:line, role)

| Address / symbol | Where | Role |
|---|---|---|
| `sub_822AD190` | 48.cpp:7429 | D3D engine init; registers `sub_8229DEA0` with `VdSetGraphicsInterruptCallback` (48.cpp:7494-7502) |
| `sub_8229DEA0` | 45.cpp:24473 | D3D graphics interrupt handler (CP interrupt source 1 / vblank source 0); vblank counter `[dev+15148]`, countdown `[dev+15156]`, user callback `[dev+15144]` |
| `sub_8229DA20` | 45.cpp:23729 | `D3DDevice_SetVerticalBlankCallback` |
| `sub_822A0688` | 45.cpp:30587 | swap CP-interrupt callback: interval/threshold decision; uses scanline regs 0x7FC86530/0x7FC86584 |
| `sub_822A0718` | 45.cpp:30667 | D3D swap throttle packets (skipped for `[dev+13240] == 0x80000000`); packs `(interval<<16)|threshold` |
| `sub_822A2A40` / `sub_822A2A48` / `sub_822A2A50` | 47.cpp:169 / 178 / 187 | set/get PresentationInterval `[dev+13240]`, set PresentImmediateThreshold (`[dev+11596]` bits 16..22) |
| `sub_822A08A0` | 45.cpp:30912 | `D3DDevice_Swap`; `VdSwap` call at 45.cpp:31129 |
| `sub_8229E9D0` | 45.cpp:26124 | writes the CP-interrupt packet (SCRATCH_REG4/5 + INTERRUPT 0x54), called from `sub_822A0718` |
| `sub_822ACB90` / table 0x82638D60 | 48.cpp:6527 / .data | device reset: applies render-state defaults via `[dev+56+4*state]`, saves/restores `[dev+13240]`; entry 94 = `{0x822A2A48, 0x822A2A40, 0}` |
| `sub_8229EF48` -> `sub_8229D8A0` | 45.cpp:26979 / 23491 | ring-buffer reserve / spin until the GPU frees space (5 s hang timeout -> `sub_822ACAA8`) |
| `sub_8229D6F0`, `sub_8229D720`, `sub_822B20D8` | 45.cpp:23221, 23250, 48.cpp:19529 | D3D statistics (mftb sites 45.cpp:23241/23268, 48.cpp:19597) |
| `sub_825BA0C8` | 91.cpp:17236 | xapi `QueryPerformanceCounter` (mftb -> `__rdtsc`) ; `sub_825B9A70` 91.cpp:16221 = `QueryPerformanceFrequency` |
| `sub_8245D288` | 71.cpp:8107 | XMA/audio wait with freq/8 timeout (mftb 71.cpp:8121/8131) |
| `sub_82092358` | 0.cpp:5916 | application constructor; `li r6,2` at **0x820923D4** (0.cpp:5989) -> `sub_8258D568(gfx,1,0,2)`; registers Present task `sub_82091A60` (0.cpp:6175) |
| `sub_8258D568` | 85.cpp:12021 | game device creation: XGetVideoMode, pp with INTERVAL_IMMEDIATE (`[gfx+68]`), `[gfx+304]` = vsync interval (85.cpp:12096-12145), SetVerticalBlankCallback(`sub_8258D4D8`) at 85.cpp:12380 |
| `sub_8258D4D8` | 85.cpp:11928 | game vblank callback: `g_8296BD2C++`, `g_8296BD30++`, `NtSetEvent(g_8296BD34)` |
| `sub_8258D958` | 85.cpp:12602 | frame start: `g_8296BD30 = 0` (85.cpp:12628); called from `sub_82098940` 1.cpp:10074 |
| `sub_8258DAA8` | 85.cpp:12813 | **vsync wait**: loop until `g_8296BD30 >= [gfx+304]` and changed since entry |
| `sub_8258DC58` | 85.cpp:13072 | game Present (`sub_822A8A60`, `sub_8258DAA8`, D3D Swap, front-buffer flip `[gfx+268]`) |
| `sub_82091A60` | 0.cpp:4489 | Present task (`sub_8258DC58` + `sub_8258D9F8`) |
| `sub_8258DD90` / `sub_8258DD78` | 85.cpp:13286 / 13259 | Event wait (-> `sub_822970A0` 45.cpp:4108 -> `sub_82297D58` 45.cpp:6130 -> `NtWaitForSingleObjectEx`) / Event set (-> `sub_825BA1E0` 91.cpp:17450 -> `NtSetEvent`) |
| `sub_8209ACB8` | 1.cpp:15572 | loading-screen loop thread: D3D interval 1 (`sub_822A2A40(dev,1)` 1.cpp:15620), `sub_822A0718` + Swap per vblank |
| `sub_8218E420` | 23.cpp:31736 | state handler with the 60-frame timer; `cmplwi cr6,r11,60` at 0x8218E4EC (23.cpp:31852) on global counter 0x826D841C |
| `sub_82106540` | 12.cpp:4965 | text draw helper `(x, y, string, ...)` ("Now Loading" at (100,30) in `sub_8218E420`, "ctrlWait" in `sub_82282138`), not timing |
| 0x82000D44 / 0x820029EC / 0x820029E8 | .rdata | `30.0f` / `1/30` / `1/60` constant pool entries (loads `lfs ..,3396/10732/10728(r11)` after `lis r11,-32256`) |
| 0x8296BD2C / 0x8296BD30 / 0x8296BD34 | .bss | total vblanks (unused) / vblanks since frame start / event handle (`lis -32105` + -17108/-17104/-17100) |
| 0x7FC86544, 0x7FC86530, 0x7FC86584 | GPU regs | display interrupt status (bit 0 = vblank), current scanline, total scanlines (& 0xFFF) |
| runtime | gpu.cpp:104-138, 251-295, 534-539, 600-615; kernel.cpp:437-450 | IrqThread, INTERRUPT/WAIT_REG_MEM/XE_SWAP handling, Vd imports, time imports |

## Summary (10 lines)

1. Swap pacing is game-side: the D3D device uses `D3DPRESENT_INTERVAL_IMMEDIATE`; the game's vblank callback `sub_8258D4D8` counts vblanks into `g_8296BD30` and signals event `g_8296BD34`.
2. Before each swap `sub_8258DAA8` waits until `g_8296BD30 >= [gfx+304]` (= 2, from `li r6,2` at 0x820923D4 in the app ctor) and a new vblank arrived; `sub_8258D958` resets the counter at frame start.
3. D3D's own interval machinery (`sub_822A0718`/`sub_822A0688`, countdown `[dev+15156]`, scratch flag `[[dev+10772]+4]`, `WAIT_REG_MEM`) exists and is correctly emulated, but `[dev+13240]` is never set above 1 (pp asks for IMMEDIATE, the loading thread sets 1), so it never limits the frame rate.
4. The interrupt callback given to `VdSetGraphicsInterruptCallback` is `sub_8229DEA0`; on vblank it increments `[dev+15148]` and calls `[dev+15144]` (the game callback).
5. The simulation is stepped once per presented frame with 30 Hz baked in: `30.0f` @0x82000D44 (~25 users), `1/30` @0x820029EC, `1/60` @0x820029E8 (physics), plus integer frame timers everywhere.
6. No elapsed-time/vblank catch-up exists; `KeQuerySystemTime`/`mftb`/QPC are only used for D3D stats, a stopwatch class, audio timeouts and xapi helpers.
7. The Xenia patch site 0x8218E4EC (`sub_8218E420`) is a "wait 60 frames then change state" timer on global 0x826D841C, another per-frame timer, not pacing logic.
8. Runtime-only fixes cannot give correct-speed 60 fps; the one-instruction interval patch (mid-asm hook at 0x820923D4, r6 = 1) only reproduces the 2x-speed experiment cleanly.
9. Real 60 fps needs a game-logic patch set (halve dt constants, double all frame-count timers, retune per-frame damping): days-weeks, high risk; recommended to stay at 30 fps and improve frame-time consistency.
10. Side finding: `mftb` is translated to `__rdtsc()` while `KeQueryPerformanceFrequency` returns 50 MHz, so timebase-based timeouts (XMA wait `sub_8245D288`) are wrong by the TSC/50 MHz ratio.
