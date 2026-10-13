# Xenos shader translator vs XenosRecomp / Xenia - instruction-by-instruction diff

Read-only analysis, written 2026-10-05 (about 17:30). Nothing in the project was modified or built, the game was not run.
Only scratch scripts (in the session scratchpad) were used: they parse the shipped microcode, scan the dumped HLSL, and call
`D3DCompile`/`D3DDisassemble` (d3dcompiler_47.dll) on the dumped HLSL files (read only; results only in the scratchpad). The only file I created in the project
is this note. Plain ASCII.

## 0. Scope, sources, snapshot

Analysed versions (the files were edited by someone while I worked; line numbers below refer to these):
- `C:\itc-work\runtime\src\xgpu_shader.cpp` - 903 lines, mtime 17:07:53. The cube path (`xe_cube`, `xe_cubeDir`, `V_CUBE` operand mask) had already been
  rewritten per `work/notes_cube.md` (my first read, 892 lines, still had the old guess). ALU/CF code (lines 253-680) is unchanged by that edit.
- `C:\itc-work\runtime\src\xgpu_d3d.cpp` - mtime 17:17:46. It now compiles with `D3DCOMPILE_SKIP_OPTIMIZATION` first (lines 217-223, see 1.1).
- Reference A (as requested): `...\UnleashedRecomp\tools\XenosRecomp\XenosRecomp\shader_recompiler.cpp` (+ `shader_common.h`, `shader_code.h`).
  Abbreviated `XR:<line>`.
- Reference B (ground truth, found in the repo): Xenia canary GPU sources `C:\Users\...\txr-itc-recomp\reference\xenia-gpu\src\xenia\gpu\`:
  `ucode.h`, `shader_interpreter.cc` (CPU interpreter, `XI:`), `dxbc_shader_translator_alu.cc` (`XA:`), `dxbc_shader_translator_fetch.cc` (`XF:`),
  `dxbc_shader_translator.cc`, `registers.h`, `xenos.h`. I use B whenever XenosRecomp and hardware disagree, because XenosRecomp is a lossy
  translator (e.g. it never emits the condition of any `cexec`, see 2.4).
- Data used: the 78 shipped shaders `work/extracted/DATA/SHADERS/*.xpu|xvu` (parsed with my own decoder: about 2.8k non-export ALU instructions, 371 tfetch,
  96 vfetch), the 44 dumped race-pass pixel shaders `work/shaders/ps_*.hlsl`, RenderDoc pixel trace `work/rdoc/dbg3/{trace.txt,disasm_DXBC.txt}`,
  logs `work/k1.err` etc.

Verdict labels: **MY BUG** (translator differs from hardware and it matters/can matter), **EQUIV** (same semantics), **DEVIATES** (mine AND XenosRecomp differ
from Xenia/hardware), **UNCERTAIN**. "Seen?" = whether the construct occurs in the shipped shaders / dumped shaders.

## 1. TL;DR - ranked by likelihood of causing wrong LIGHTING colours

| # | Finding | Where | Verdict | Evidence |
|---|---------|-------|---------|----------|
| 1 | The pc-dispatch state machine (`while(true){ if (pc==N){...} ... continue; }`) is MISCOMPILED by FXC's optimiser: the `continue` of an unconditional jump block is hoisted above the register stores of the preceding block, so those stores are lost and stale registers leak into the colour. | `controlFlow()` xgpu_shader.cpp:612-678 (all `continue`s 643,652,656,663,667) | MY BUG (structure) - already worked around in xgpu_d3d.cpp:217-223 | Section 1.1: RenderDoc pixel trace + fresh `D3DCompile` of the dumped shader |
| 2 | `COND_EXEC_PRED_CLEAN(_END)` (CF opcodes 13/14) is decoded as "predicate p0" exec; per Xenia it is a BOOL-CONSTANT exec (same layout as `cexec`). Blocks are therefore skipped/mis-routed. | `decodeExec()` xgpu_shader.cpp:547-548 | MY BUG (high) | Section 1.2: 158/158 in shipped shaders carry bool addresses and no instruction predicates; 3 dumped race shaders read `p0` before it is ever set |
| 3 | `LOOP_END` increments `aL` twice (`aL++;` AND `aL += step`) and `LOOP_START` never skips a zero-count loop; step is read unsigned; `is_repeat` / predicated break ignored. | xgpu_shader.cpp:632-645 | MY BUG (high for the 8 looped dumped shaders) | Section 1.3: real loop constants have step=1 -> aL = 0,2,4.. (should be 0,1,2..) |
| 4 | Texture fetch fidelity: exp-adjust applied to xyz only; `getWeights`/other fetch opcodes silently DROPPED (destination register left stale); `off` argument of `xe_fetch2D` ignored; unnormalised coords flag ignored; sign/gamma/num_format ignored. | xgpu_shader.cpp:206-246, 741-742, 772-783 | MY BUG (low today: none of these occur in the shaders I can see) | Section 2.6 |
| 5 | ALU edge cases (MULS_PREV2 lumped with MULS_PREV, SETP_INV result, RCPF/RSQF/RCP/RSQ clamps, `0*x=0`, scalar 2nd lane `.z`, co-issue read-before-write, saturate on `ps`). | xgpu_shader.cpp:390,400,444,455-461,479 | DEVIATES (same as XenosRecomp) | Sections 2.1-2.3 and 3; none of them occur in the 78 shipped shaders |
| 6 | PS param-gen register layout (faceness in Z magnitude instead of sign of X). | xgpu_shader.cpp:846 | MY BUG (low; no dumped shader uses it) | Section 2.5 |

Everything else I compared (all 30 vector opcodes, all scalar opcodes except the few above, operand/swizzle/negate/abs decoding, export masks,
saturate, SETP/KILL/MAXA, exec sequencing, bool/loop constant addressing, cube after the 17:07 edit) is EQUIV to XenosRecomp, and EQUIV to Xenia except for
the listed DEVIATES rows.

### 1.1 Finding 1 in detail (stale-register leak = the visible symptom) - already worked around, mechanism confirmed

Symptom reproduced in the user's own RenderDoc history (`rdoc/dbg3/trace.txt`): pixel (240,480), event 30954, `ps_9473127d9a6bf8cf_12`,
pre `[0,0,0,1]`, post `[0.048, 0.000, 1824.000, 1.000]`. Pixel debug trace: `o0 = [0.049161, 0, 1824.6, 1]`. 1824.6 has the magnitude of the (light/camera - position)
vectors the shader computes at its start (first use `-r[0].xyz + c[4].xyz`), i.e. it is a scratch value, not a lighting term. One huge channel -> saturated
blue/cyan/white after the composite; other pixels/shaders leak other scratch values -> noisy red road.

Mechanism (HLSL is correct, FXC output is not):
```
HLSL (ps_9473127d..._12.hlsl, CF blocks 77/78):                DXBC (RenderDoc disasm_DXBC.txt, inst 2007-2024; same with a fresh D3DCompile, flags=0)
 if (pc == 77) { pc = 78; ...; r[11].x = max(r[0].z,r[0].z);     movc r69.xyz, r14.zzzz, r57.zxyz, r16.wxyw   ; next pc
                 r[11].y = ...; r[0].xw = max(r[11].xy,..); }     ieq  r16.x, r69.x, l(78)
 if (pc == 78) { pc = 79; { pc = 80; continue; } }                 if_nz r16.x
                                                                     mov r66.y, l(80)
                                                                     continue                 <-- jump taken BEFORE the stores
                                                                   endif
                                                                   ...max/movc...
                                                                   mov x0[0].xw, r67.zzzy     <-- block 77's store of r[0].xw (never reached)
```
Trace confirmation: at inst 2006 `r67.z = x0[0].x = -1824.6` (block 77 should be about to overwrite r0.x); in the last loop iteration the same -1824.6 is read from
x0[0].x again (inst 2006 of the final pass), block 91 computes `r1.x = -r0.x + r11.x = 1824.6`, then `r11.x = r1.x + r3.x`, `r3.x = max(r11.x,0)` and
`oC0.z = r3.x` -> the blue channel. So the r0.xw write of block 77 never happened. Expected result with the store applied: r0.x=r0.w=r0.z=0
-> output about (0.049, 0.037, 0.034), a dim grey-brown, which is what the road/walls should look like.
`D3DCompile(ps_9473127d..., flags 0 / 1<<14 / 1<<15)` reproduces the pattern; `SKIP_OPTIMIZATION (4)` keeps `mov x0[0].xw, r7.xxxy` BEFORE the `continue`
(correct). `PREFER_FLOW_CONTROL (1024)` and `AVOID_FLOW_CONTROL (512)` fail to compile this shader (E_FAIL). The road shader `ps_0bbc553289131016_13` shows
the identical hoisted `continue` (`mov r67.y, l(81); continue` before `mov x0[2].xz, r72.zzyz`). All 13 state-machine pixel shaders in the dumps use this construct; the 3 big lit ones
(9473127d, d96c4660, 0bbc5532) have 9 jumps each: 7 x `if (!p0) { pc = N; continue; }` and 2 x unconditional `{ pc = N; continue; }` (the else-skips). Conditional jump sites
keep their stores in front of the `continue` in the DXBC (checked on 9473127d); the unconditional ones are where the hoisting hits.
Simple shaders (no CF jump/loop) never hit this -> matches "sky, car, UI fine, lit world wrong".

`xgpu_d3d.cpp:217-223` (comment: "FXC's optimiser miscompiles the big branchy lighting shaders") already switches the default to `SKIP_OPTIMIZATION`; that
is the right workaround for the optimiser. Hardening on the translator side (cheap, removes the dependency on compile flags):
- forward jumps do not need `continue`: after `pc = target;` all blocks `if (pc == k)` with k < target are skipped anyway. Only LOOP_END / backward
  jumps need `continue`. (Lines 652,656,663,667: drop `continue` when `addr > i`.)
- or emit structured `if` nesting for forward-only `cond_jmp` like XenosRecomp's `simpleControlFlow` path (XR:1513-1560 pre-pass, XR:1596-1606 closing braces,
  XR:1680-1719 jump emission), which also avoids the `while(true)` loop completely.
- the compiler also warns `X3595: gradient instruction used in a loop with varying iteration` for every `Sample` inside the state machine; textures are
  single-mip, so `SampleLevel(smp, uv, 0)` in pixel shaders would remove the warning and any flattening FXC does for it.

### 1.2 Finding 2 in detail (bool exec decoded as predicate)

Xenia `ucode.h:142-157`: "kCondExecPredClean: Conditionally executes based on a bool const. PredicateClean = true. This is cexec with a bool constant
(kCondExec ...), not a kCondExecPred." The struct is `ControlFlowCondExecInstruction` (`ucode.h:258-292`): bool address = bits 34..41, condition = bit 42.
`XI:48-86` handles ops 13/14 together with 3/4 (bool test), ops 5/6 with the predicate. XenosRecomp also groups 13/14 with 3/4 (XR:1626-1634).
Mine (xgpu_shader.cpp:545-548): ops 3/4 -> bool, ops 5/6 AND 13/14 -> `if (p0)` / `if (!p0)`.

Evidence from the data:
- 78 shipped shaders: 158 CF instructions of opcode 13, 0 of opcode 3/4. Their bool addresses: 137,140,144,149,156,163,175,182,194,201,208,209 (PS bool bank);
  ALL 158 contain zero individually-predicated instructions, whereas in opcode-5 blocks every effective instruction is predicated (38 of 53 blocks have all ALU instructions predicated; the other 15 hold predicated fetches plus one empty-mask `max/retain_prev` nop).
  E.g. `XBM_SH_T1`: `cexec b149` followed by `cexec !b149` (an if/else pair).
- 3 of the 44 dumped race pixel shaders (`ps_c2b5e8b8bf7e8b0d_5`, `ps_a08c03fc49b71985_5`, `ps_3775d3584d0cda37_5`; the 20-fetch, 16-tap-shadow ones; c2b5e8b8 lines
  131-235) start with nine `if (p0) { ...several unpredicated instructions... }` blocks BEFORE any `p0 =` assignment exists in the shader (first one at line 315).
  p0 is still its initial `false`, so the whole shadow section never runs, no matter what the bool constant says (and `cond=0` blocks of if/else pairs would run
  unconditionally).
- The only logged draw (k1.err:452 area) has PS bools `b128` only, so those draws happen to match; draws with shadows/lights enabled will not.
Fix: `case CF_COND_EXEC_PRED_CLEAN(_END)` -> `e.kind = 1; e.boolAddr = (v>>34)&0xFF; e.cond = (v>>42)&1` (same as the COND_EXEC case).
Note XenosRecomp is NOT a usable reference here: it never emits any condition for `CondExec*`/`CondExecPred*` (XR:1616-1643 only collect address/count/
sequence), it relies on per-instruction predicates.

### 1.3 Finding 3 in detail (loops)

Xenia `xenos.h:1141-1155`: `LoopConstant { count:8, start:8, step:int8 }`, "the resulting aL is `iterator * step + start`". `XI:126-179`:
LOOP_START: if iteration >= count jump to `cf.address()` (skip the loop); `is_repeat` keeps aL; LOOP_END: ++iterator, loop while iterator < count and
not (predicated_break && condition == p0).
Mine (xgpu_shader.cpp:632-645):
```
LOOP_START:  xe_lit[id] = 0; aL = start;                        // no zero-count skip, address (skip target) and is_repeat ignored
LOOP_END:    aL++;  xe_lit[id]++; aL += (int)xe_loopStep(id);   // aL advances by 1 + step  <-- wrong
             if (xe_lit[id] < xe_loopCount(id)) { pc = addr; continue; }
```
Real constants from the capture (k1.err:452-453): `loop const 30 = 00010004`, `31 = 00010003` -> count 4/3, start 0, **step 1**. Hence aL = 0,2,4(,6)
instead of 0,1,2(,3). The dumped loop shaders index light arrays with aL (e.g. `ps_0d8b4d20b4f64aef_4.hlsl` lines 99-114 (3-iteration loop) and 124-233 (4-iteration loop): `c[min(41+aL,255)]`,
`c[min(65+aL,255)].www`, `c[min(5+aL,255)]` for 3 directional lights; `c[8+aL] c[16+aL] c[24+aL] c[32+aL] c[47+aL]` for 4 point lights). Captured constants
(k1.err:349-): c5..c7 directions, c8..c11 positions (c9 = (-1264,29,620,1)), c12..c15 zero, c41..c46 colours. With aL=4 the "direction" is c9 = a world POSITION and the
point-light loop reads c12/c14 (zero lights) while skipping c9/c11 -> wrong/huge dot products. Affected dumped shaders: 0d8b4d20, 648b0952, 67f07499, 83699d3d, 846547db,
9e3540ff, c370cbc4, d6f4f468 (all 8 have two loops, ids 30/31).
Other loop gaps: step is read as unsigned (xgpu_shader.cpp:740) - negative steps would add 255; nested loops share one `aL` (hardware restores the outer aL);
`is_predicated_break` (bit 21) ignored (0 of the 30 shipped loops use it).

### 1.4 Who is affected (draw counts = lines `ps=ps_<hash>` in `rdoc/out2/analysis.txt`, one captured frame; approximate)

| Finding | Dumped pixel shaders (draws in that frame) |
|---|---|
| 1 FXC miscompile of the pc state machine (jump with stores before an unconditional `continue`) | 9473127d (89), d96c4660 (82), 0bbc5532 (34, the road draw `e32259`), plus the other state-machine shaders 142d3a7c (37), aaad76a4 (189; only conditional jumps, stores precede them, likely unaffected), and the 8 looped ones |
| 2 PredClean decoded as p0 | c2b5e8b8 (9), a08c03fc (11), 3775d358 (3) |
| 3 loop aL double increment | 0d8b4d20 (31), 9e3540ff (16), c370cbc4 (8), 846547db (4), d6f4f468 (2), 67f07499 (1); 648b0952 and 83699d3d (cube) not in that frame |
| (cube, fixed 17:07) | 188fcacc (48) |

## 2. Compare tables

### 2.1 Vector ALU (xgpu_shader.cpp:386-418 vs XR:694-800)

| Opcode | Mine (line) | XenosRecomp (line) | Xenia / hardware | Verdict |
|---|---|---|---|---|
| ADD | `a0 + a1` (389) | `{} + {}` (696) | XI:359 | EQUIV |
| MUL | `a0 * a1` (390) | (700) | SM3 rule 0*x=+0 (XI:364-371, XA:92-122) | DEVIATES (low, only inf/NaN) |
| MAX / MAXA | `max(a0,a1)` (391) | (704-707) | `a>=b ? a : b` (NaN order) | EQUIV (NaN edge differs) |
| MIN | `min` (392) | (709) | `a<b ? a : b` | EQUIV |
| SEQ SGT SGE SNE | `(a OP b)` bool->float (393-396) | (713-727) | XA:148-167 (AND 1.0) | EQUIV |
| FRC TRUNC FLOOR | `frac trunc floor` (397-399) | (729-739) | XI:412-427 | EQUIV |
| MAD | `a0*a1 + a2` (400) | (741-743) | mul+add, 0*x rule (XA:93-121) | EQUIV (DEVIATES only for 0*inf) |
| CNDEQ CNDGE CNDGT | `((a0 == 0.0) ? a1 : a2)` etc (401-403) | `select(a0 == 0.0, a1, a2)` (745-755) | `src0 cond ? src1 : src2` (XI:440-460) | EQUIV. Operand order identical. FXC compiles a vector-condition `?:` to a per-component `movc` (verified with d3dcompiler_47: `eq r0.xyz..; movc o0.xyz`); it would NOT be equivalent under DXC -HV 2021 (needs `select`) |
| DP4 / DP3 | `dot(a0,a1)`, lane mask 15 / 7 (404, 305-306) | (757-760, 519-522, 515-517) | XI:461-486 | EQUIV |
| DP2ADD | `dot(a0,a1) + a2`, masks 3/3/1 (405, 304) | (762-763, 511-513) | XI:487-499 | EQUIV |
| CUBE | `xe_cube(a0s)` full 4-lane swizzled operand (406, 306), prelude 689-709, `xe_cubeDir` in `xe_fetchCube` (803) | `cube(rN, cubeMapData)` direction side-channel (766-767; common.h 157-160,183-190) | XA:247-354, `ucode.h:1506-1547` | EQUIV to Xenia after the 17:07 edit (checked all three major-axis branches, signs, `2*ma`, face ids). Before the edit: MY BUG (see notes_cube.md). 15 cube shaders ship, 10 with the rotated `.xxyz/.zyxx` swizzle |
| MAX4 | `xe_max4(a0)` (407, 687) | (770-772; common 202-205) | XI:528-545 | EQUIV |
| SETP_EQ/NE/GT/GE_PUSH | `p0 = a0 == 0.0 && a1 OP 0.0` (368-372); value `(p0 ? 0.0 : a0 + 1.0)` (408) | (637-659, 774-779) | predicate = src0.w/src1.w (lane 3), value = src0.x/src1.x (lane 0) (`ucode.h:1574-1628`, XI:546-581) | EQUIV to XR; vs Xenia only equal when swizzle lanes 0 and 3 agree and the write-mask lane equals them. Checked: all 19 SETP_PUSH in the shipped shaders have broadcast swizzles -> equal in practice. Write mask 0 falls back to lane x (line 307) instead of lane w: UNCERTAIN (not seen) |
| KILL_EQ/GT/GE/NE | `if (any(a0 OP a1)) discard;` (339-342); value `any(..) ? 1 : 0` (409-412) | `clip(any(..) ? -1 : 1)` (557-575) + (781-795) | all 4 lanes compared (XA:508-575) | EQUIV to XR. Operands only print the write-mask lanes (mask 0 -> lane x) so with mask 0 only 1 lane is compared: DEVIATES. Not seen (0 kill ops shipped) |
| DST | `xe_dst` (413, 688) | `dst` (797; common 192-200) | XI:616-624 | EQUIV |
| MAXA | `a0 = (int)clamp(floor(a0s.w + 0.5), -256, 255)` (373-376) + max (391) | (660-664) | XI:625-634 | EQUIV (vector MAXA: 0 uses; scalar MAXAS/MAXASF: 50 uses incl. vertex shaders) |

### 2.2 Scalar ALU (xgpu_shader.cpp:420-482 vs XR:853-1031)

| Opcode | Mine (line) | XenosRecomp (line) | Xenia / hardware | Verdict |
|---|---|---|---|---|
| ADDS, ADDS_PREV, SUBS, SUBS_PREV | `s0 + s1`, `s0 + ps`, `s0 - s1`, `s0 - ps` (441,442,458,459) | (860-866, 936-942) | XI:715-721, 876-883 | EQUIV |
| MULS | `s0 * s1` (443) | (868) | 0*x rule (XI:723-728) | DEVIATES (low) |
| MULS_PREV | `s0 * ps` (444) | (872-875) | XI:746-751 | EQUIV |
| MULS_PREV2 | same `s0 * ps` (444) | same (872-875) | `ps == -FLT_MAX \|\| !isfinite(ps) \|\| !isfinite(s1) \|\| s1 <= 0 ? -FLT_MAX : s0*ps` (`ucode.h:1025-1030`, XI:752-765) | DEVIATES (the "lit"/pow idiom: spec term would leak on N.L<=0). 0 uses in the 78 shipped shaders |
| MAXS MINS | `max/min(s0,s1)` (445,446) | (877-885) | XI:766-777 | EQUIV |
| MAXAS / MAXASF | `max(s0,s1)` + `a0 = (int)clamp(floor(s0 [+0.5]),-256,255)` (445,480-481) | (877-880, 1020-1030) | XI:860-875 | EQUIV |
| SEQS SGTS SGES SNES | `(s0 OP 0.0 ? 1.0 : 0.0)` (447-450) | (887-901) | XI:778-790 | EQUIV |
| FRCS TRUNCS FLOORS | (451-453) | (903-913) | XI:791-800 | EQUIV |
| EXP | `exp2(s0)` (454) | (915-917) | `exp2`, 21-bit precision (XI:801-804) | EQUIV |
| LOGC / LOG | `clamp(log2(s0), -FLT_MAX, FLT_MAX)` (455) | `clamp(log2(), FLT_MIN, FLT_MAX)`; XR defines `FLT_MIN` as `asfloat(0xff7fffff)` = -FLT_MAX (common.h:15-16) (919-922) | LOGC clamps -inf only; LOG (IEEE) does not clamp (XI:805-815) | EQUIV for LOGC; DEVIATES for LOG (log2(0) = -inf vs -FLT_MAX) |
| RCPC / RCPF / RCP | all `clamp(rcp(s0), -FLT_MAX, FLT_MAX)` (456) | (924-928) | RCPC clamps both inf; RCPF -> +-0; RCP (IEEE) unclamped (XI:816-837) | EQUIV for RCPC; DEVIATES for RCPF (should give +-0) and RCP (should give +-inf; the clamped value avoids inf*0=NaN so it is usually benign). Shipped: 119 IEEE rcp/rsq, 0 RCPC/RCPF/RSQC/RSQF |
| RSQC / RSQF / RSQ | `clamp(rsqrt(s0), ..)` (457) | (930-934) | XI:838-859 | same as above |
| SETP_EQ NE GT GE | `p0 = s0 OP 0.0` (427-430) ; ps `(p0 ? 0.0 : 1.0)` (460) | (817-831, 944-949) | XI:884-899 | EQUIV |
| SETP_INV | `p0 = s0 == 1.0` (431) ; ps `(s0 == 0.0 ? 1.0 : s0)` (461) | (833-835, 951-953) | p0 ? 0.0 : (s0==0 ? 1 : s0) (`ucode.h:1204-1217`, XI:900-906) | DEVIATES: result should be 0.0 when s0 == 1.0. Harmless in the observed counter idiom (setp_inv ... setp_pop) because pop gives p0=true/counter 0 either way; 20 shipped uses (DofLv2-6) |
| SETP_POP | `p0 = s0 - 1.0 <= 0.0`; ps `(p0 ? 0.0 : s0 - 1.0)` (432,462) | (837-839, 955-957) | XI:907-911 | EQUIV |
| SETP_CLR / SETP_RSTR | `false` / FLT_MAX ; `s0 == 0.0` / `(p0 ? 0 : s0)` (433,434,463,464) | (841-846, 959-964) | XI:912-919 | EQUIV |
| KILLS_* | `(s0 OP 0.0 ? 1:0)` + `if (ps != 0.0) discard;` (465-469,504) | (967-985, 1080-1084) | XI:922-936 | EQUIV |
| SQRT | `sqrt(s0)` (470) | (987-989) | XI:938-941 | EQUIV |
| MULSC0/1 ADDSC0/1 SUBSC0/1 | `c0 op c1` with c0 = constant `.w` lane of src3, c1 = TEMP register `((opc&1)\|(src3sel<<1)\|(swz&0x3C))` `.x` lane (471-473, 271-274) | (991-1004, 396-410) | XI:683-700 | EQUIV (both treat the 2nd operand as a register, which is what Xenia does) |
| SIN COS | (474,475) | (1006-1011) | XI:942-947 | EQUIV |
| RETAIN_PREV | skipped, dest still gets `ps` (420, 485-491) | (808) | XI:948 | EQUIV |
| saturate | `ps` is assigned the SATURATED value (478-479) | (855-856, 1015-1016) | only the written copy is clamped; `ps` keeps the raw value (XI:955-962) | DEVIATES (matters only if a `*_PREV` op follows a saturated scalar op) |
| 2nd operand lane of 2-component scalar ops | `s1` = lane 0 always (314) | (543-545) | lane 0, but lane 2 (`.wz`) when the paired vector op has 3 operands (`ucode.h:1336-1342`, XI:676-681) | DEVIATES (ADDS/MULS/MAXS/MINS/SUBS/MAXAS(F) co-issued with MAD/CND*/DP2ADD). 0 occurrences in the 78 shipped shaders |

### 2.3 Operand fetch, destinations, exports (aluOperand 253-318, alu 320-507 vs XR:386-553, 580-635, 666-684, 1033-1078)

| Item | Mine | XenosRecomp | Xenia / hardware | Verdict |
|---|---|---|---|---|
| Bit-field decode of the 3-dword ALU instruction (opcodes, masks, sat, swizzles, negates, pred bits, select bits, src regs) | 256-266, 323-331 | `AluInstruction` shader_code.h:418-458 | `ucode.h:2038-2091` | EQUIV (checked every field) |
| src select, abs | `select ? r[reg&0x3F], abs = reg&0x80 : c[reg], abs = absConstants` (282-283) | (437-445) | XI:329-343 | EQUIV |
| negate | `-` prefix (293) | (492) | XOR sign bit | EQUIV |
| vector lane swizzle | `SW[((swz>>2i)+i)&3]` per write-mask lane (310) | (532) | `ucode.h:2000-2003` | EQUIV (DP*/MAX4/CUBE force all lanes; others use mask or `.x` when mask==0) |
| scalar operand lanes | SCALAR_0 lane `.w` (313), SCALAR_1 lane `.x` (314) | (538-546) | see 2.2 last row | EQUIV to XR |
| constant addressing | `c[min(reg + a0 or aL, 255)]` when const0Rel (289) | `Name(reg - base + a0/aL)` with bounds select (473-474) | `GetFloatConstant`: index<0 -> 0 (XI:255-280) | EQUIV |
| which constant operand is relative | const0Rel bit applied to EVERY constant operand (289); const1Rel ignored | same (asserts !const1Relative for non-arrays) | 1st constant uses const_0_rel_abs, 2nd/3rd use const_1_rel_abs (`ucode.h:1944-1963`) | DEVIATES, not seen: shipped data has 140 relative-const instructions, all const0-only, none with >= 2 constant operands (95 PS + 45 VS, all a0-based; the dumped race shaders use aL) |
| a0 vs aL selector | `constAddrRegRel ? a0 : aL` (289) | (474) | XI:258-262 | EQUIV |
| temp register relative (`reg & 0x40`, r[aL+n]) / dest relative (bits 6, 14) | ignored (282, 323-326) | ignored | `ucode.h:1886-1888`, XI:994-1009 | DEVIATES, not seen (0 in shipped data) |
| `abs_constants` | applied to constants and to SCALAR_CONSTANT operands (258, 272, 274) | (401, 409, 444) | XI:685-704 | EQUIV |
| vector write mask on export | `vectorWriteMask &= ~scalarWriteMask` (378) | (667-668) | `ucode.h:2010-2016` | EQUIV |
| scalar write mask on export | `&= ~original vector mask` (484) | (1033-1035) | `ucode.h:2017-2023` | EQUIV |
| export constants 0 / 1 | zero = relBit ? ~(v\|s) : 0 ; one = v & s (493-503) | (1059-1078) | `ucode.h:2024-2035` | EQUIV |
| export targets | PS 0-3 -> oC0..3, 61 -> oDepth; VS 62 oPos, 63 oMisc, <16 oT[n] (348-365) | (580-635) | `ucode.h:1792-1851` | EQUIV (memexport 32+ unsupported in both) |
| saturate (vector) | `saturate(expr)` (416) | (691-692, 802-803) | XI:955-958 | EQUIV |
| vector/scalar co-issue | vector result written to its register first, THEN the scalar operands are read (379-418, then 420-491) | same order | both operations read the pre-instruction registers; results stored after (XA:1181-1191) | DEVIATES (both). Measured on the 2819 shipped non-export ALU instructions that write a vector result: 0 cases where the scalar op reads a register component written by the vector op of the same instruction; not an issue in practice |
| predicated instruction | whole instruction (incl. `ps`) inside `if (p0)` (334, 506) | (365-373, 1093-1098) | XA:1174-1175 | EQUIV |
| `ps`, `p0`, `a0`, `aL` | function-scope, initial 0/false (859) | (1482-1485) | XI Reset | EQUIV |

### 2.4 Control flow (decodeExec 534-552, emitExec 561-570, controlFlow 572-679 vs XR:1494-1888)

| CF op | Mine | XenosRecomp | Xenia | Verdict |
|---|---|---|---|---|
| bit layout of the 2 x 48-bit CF words per 3 dwords | 582-583 | 1515-1518 | `ucode.h:515-524` | EQUIV |
| CF area end = lowest exec address | 576-594 | 1513-1560 | - | EQUIV |
| EXEC / EXEC_END | unconditional block, `break;` on END (543-544, 568) | (1618-1623, 1795-1857) | XI:48-124 | EQUIV |
| COND_EXEC / _END (3/4, bool) | `if (xe_bool(addr))` / `== false`, `break` inside the if (545-546, 564, 568) | **no condition emitted** (1626-1634) | bool test, END only if the condition holds (`ucode.h:99-108`) | EQUIV to Xenia (XR emits no condition: not a usable reference for exec conditions) |
| COND_EXEC_PRED / _END (5/6) | `if (p0)` / `if (!p0)` (547-548, 565) | no condition emitted (1636-1642; relies on per-instruction predicates) | predicate test (`ucode.h:109-125`) | EQUIV to Xenia |
| **COND_EXEC_PRED_CLEAN / _END (13/14)** | **`if (p0)` (547-548)** | grouped with 3/4, no condition (1626-1634) | **bool constant test (`ucode.h:142-157`, XI:60-74)** | **MY BUG** (Finding 2) |
| LOOP_START | `xe_lit[id]=0; aL=start` (632-637) | `for (aL = 0; aL < i.x; aL++)` (simple) or `aL = 0` (1644-1660) | zero-count skip to `address`, `is_repeat` (XI:126-148) | MY BUG (no skip) ; XR ignores start/step entirely |
| LOOP_END | `aL++; iter++; aL += step; if (iter<count) {pc=addr; continue;}` (638-645) | `++aL; if (aL < i.x) {pc=addr; continue;}` (1662-1678) | `aL = start + iter*step`, predicated break (XI:150-179, xenos.h:1141) | **MY BUG** (Finding 3: double increment) |
| COND_CALL / RETURN | return stack of 8, uncond / pred / bool (646-657) | not implemented | XI:181-214 | EQUIV to Xenia (0 calls in shipped shaders) |
| COND_JMP | uncond / pred / bool -> `pc = addr; continue;` (658-670) | structured `if` for forward jumps, `pc` machine otherwise (1680-1719) | XI:216-235 | EQUIV semantically; `continue` on forward jumps is what triggers the FXC miscompile (Finding 1) |
| ALLOC / NOP / MARK_VS_FETCH_DONE | ignored (671) | ignored | no-op without export sink | EQUIV |
| exec sequence bits | bit0 = fetch, `>>= 2` (522-528) | (1743-1792) | `ucode.h:225-227` | EQUIV (serialize bit ignored in all) |
| bool constant addressing | `bconst[i>>7][(i>>5)&3] >> (i&31)` absolute 0..255, PS 128..255 (737) ; fed from regs 0x4900.. (xgpu_d3d.cpp:1478) | `g_Booleans` mask | XI:68-71 | EQUIV (data: PS bools 128, 137-209 absolute) |
| loop constant addressing | `lconst[(i>>2)&7][i&3]` absolute 0..31, PS 16..31 (738-740; xgpu_d3d.cpp:1479) | `int4 i{id}` | `LOOP_00 + loop_id` (XI:135-137) | EQUIV |
| shader without END / predicate p0 persistence across blocks | `p0` is one variable for the whole shader | same | same | EQUIV |
| overall structure | any CF jump/loop/call => `while(true)` pc dispatch (612-678) | structured ifs when jumps are forward-only (1513-1606) | - | see Finding 1 |

### 2.5 Pixel-shader inputs and system values

| Item | Mine | XenosRecomp | Xenia | Verdict |
|---|---|---|---|---|
| interpolators -> GPRs | `r[ii] = iT[ii]` for ii < psInterpolatorCount (842-843); count = `((SQ_PROGRAM_CNTL>>20)&0xF)+1` (xgpu_d3d.cpp:297) | table driven `float4 r{interp.reg} = i{usage}{idx}` (1421-1445) | all GPRs < max(static bound, ps_num_reg) from interpolants that the VS writes, others 0 (`shader_translator.cc:329-347`); `vs_export_count` is "count minus 1" (`registers.h:142-143`) | EQUIV (identity mapping) |
| param gen register (vPos / vFace) | `float4(floor(iPos.xy), iFace ? 1 : -1, 0)` (846) | `float4((iPos.xy-0.5) * float2(iFace ? 1 : -1, 1), 0, 0)` (1467) | |x| = pixel x, SIGN of x = back face; |y| = pixel y, sign = point; sign of z = line; w = 0 (dxbc_shader_translator.cc:725-818) | MY BUG (low): faceness belongs in the sign of X, Z must stay 0. No dumped shader uses a param-gen register (grep for `iPos.xy`: 0 hits) |
| GPR zero-init | `float4 r[64]` zeroed (837) | zeroed | zeroed | EQUIV |

### 2.6 Texture fetch (textureFetch 206-246, prelude emission 741-807 vs XR:226-361, common.h:62-160; Xenia XF:623-2552)

| Item | Mine | XenosRecomp | Xenia | Verdict / seen? |
|---|---|---|---|---|
| decode: src swizzle (6 bits, absolute), dst swizzle, dimension (w2 14-15), offsets (w2 16-25), predicate | 208-216 | 241-247, 337-345 | `ucode.h` | EQUIV |
| src coordinates by dimension 1D/2D/3D/cube = 1/2/3/3 lanes | 231-233 | 307-325 | XF:1124-1141 | EQUIV |
| dst write: only lanes with swizzle 0-3 get fetched data, 4/5 -> 0.0/1.0 | 243-244, 69-88 | 281-283, 143-159 | `StoreFetchResult` XI:1013-1045 | EQUIV |
| opcodes other than tfetch | 224: `return` for everything except 1 and 24 -> destination register left STALE | only 1 (tfetch) and 19 (getWeights -> `frac(coord*dims + off - 0.5)`) (228-229, common.h:68-72) | 16 getBCF, 17 compLod, 18 gradients, 19 weights, 24-26 set lod/gradients (XF:632-720, 1038-1090) | MY BUG (low): getWeights silently dropped. 0 occurrences in the 78 shipped shaders; cannot be seen in the dumped HLSL because nothing is emitted |
| setTextureLod (24) | stores `xe_lod` (219-223), never used (all `Sample` use hardware LOD) | ignored (228) | register LOD used for the next fetch with use_register_lod | DEVIATES, 0 uses shipped |
| offsets (w2 16-25, 0.5 texel units) | passed as `float2(off*0.5)` (238) but `xe_fetch2D` ignores its `off` parameter (774-780) | `texCoord + offset / textureDimensions` (common.h:62-66) | XF:773-837 | MY BUG (low). All 207 `xe_fetch2D` calls in the dumps use offset (0,0); 0 shipped fetches use offsets |
| unnormalised coordinates (w0 bit 25) | read and dropped (210, 217) | ignored | XF:1155-1200 | DEVIATES, 0 shipped |
| use_computed_lod / lod_bias / use_register_lod / aniso / filter overrides | ignored | ignored | XF:748-760, 575-621 | DEVIATES, 0 non-default shipped (lod bias 0, use_comp_lod 1) |
| exponent adjust (fetch const dword3 bits 13-18) | `t.xyz *= exp2(e)` (741) : alpha NOT scaled | none (UnleashedRecomp handles it elsewhere) | all used result components `* 2^e` (XF:2529-2543) | MY BUG (low): include `.w`. texswz is packed at xgpu_d3d.cpp:1483 |
| signs (dword0 bits 2-9: unsigned / signed / biased / gamma) and num_format | ignored (NOTES.md says so) | ignored | converted in the shader (XF:2380-2515) | UNCERTAIN: every fetch constant that appears in the logs has sign bits 0 (raw d0 = 0x8x0000x2), numFmt=1 only on the two k_24_8 shadow maps (their shaders compare normalised [0,1] depths) |
| cube | `xe_fetchCube(slot, r.xyz)` -> `xe_cubeDir((S',T',face))` (242, 803) | `cubeMapData.cubeMapDirections[texCoord.z]` (common.h:157-160) | XF:1453-1533 inverse | EQUIV to Xenia after the 17:07 edit |
| `Sample` inside non-uniform flow | pixel: `Sample` (779); VS: `SampleLevel(.., 0)` | `Sample` | n/a | FXC warns X3595 ("gradient instruction used in a loop with varying iteration") for the `Sample` calls inside the state machine (seen on ps_9473127d); harmless with 1 mip, `SampleLevel(smp, uv, 0)` would silence it |
| sampler per slot | `smp<slot> : register(s<slot & 15>)` (749) | descriptor heap | per fetch-constant sampler | UNCERTAIN: slots >= 16 alias samplers 0..15 (PS only uses 0..15) |

### 2.7 Vertex fetch (vertexFetch 93-201; no equivalent in XR because it uses the shader's vertex-element table)

Compared against `ExecuteVertexFetchInstruction` (XI:1047-1283) / XF:1-510: format enum values (6,7,16,17,25,26,31,32,33-38,57) EQUIV; bit widths/offsets of
10_11_11 and 11_11_10 EQUIV; signed-normalised `max(x/(2^(w-1)-1), -1)` EQUIV; 32-bit int/float variants EQUIV; index rounding `+0.5` EQUIV; exp adjust on all
components EQUIV; mini fetch reuses `vfAddr` EQUIV; predicated vfetch EQUIV. DEVIATES (low): `signed_rf_mode` (kNoZero) is read (line 98) but both branches of the 8_8_8_8 case
(140-141) emit the same expression and the other formats never look at it (Xenia: `(n+0.5)*2/(2^w-1)`).

## 3. What the data say is NOT exercised (so cannot explain the colours today)

Measured on the 78 shipped shaders (scratch scripts `ucode_stats*.py`, `fetch_stats.py`, `coissue_chk.py`, `setp_chk.py` in the session scratchpad):
- CF ops present: EXEC 739, EXEC_END 57, ALLOC 57, NOP 30, COND_EXEC_PRED 53, **COND_EXEC_PRED_CLEAN 158**, COND_JMP 634, LOOP_START/END 30 each; no COND_EXEC(3/4), CALL, RETURN.
- Vector ops used: MAX 823, MUL 673, ADD 548, MAD 524, DP3 137, SGT 87, CNDEQ 41, MIN 32, DP4 23, SGE 15, SETP_NE_PUSH 15, CUBE 15, SETP_GT_PUSH 4, CNDGE 3, SNE 1. No DST/MAX4/KILL/MAXA/FRC/FLOOR.
- Scalar ops used: MAXS 302, SUBSC 152, ADDS_PREV 51, RCP 51, RSQ 43, MAXAS 40, ADDSC 69, LOG 37, SGTS 35, SETP_NE 32, EXP 25, SETP_INV 20, SETP_POP 19, MULS_PREV 17, SETP_EQ 11, SGES 10, MULSC 11, SQRT 6, ...
  No MULS_PREV2, RCPF/RSQF, KILLS, SIN/COS.
- Fetch ops: tfetch 371 (356 2D, 15 cube), vfetch 96; no getWeights / setTexLOD / gradients / BCF; no unnormalised coords, no offsets, no LOD bias.
- No temp-register-relative or dest-relative addressing, no const1-relative, one `abs_constants`, no 2-component-scalar + 3-operand-vector pairs, 0 same-instruction co-issue hazards.
So for this game the ISA-level deviations in section 2.1-2.3 are latent; Findings 1-3 are what actually bites.

## 4. Suggested minimal changes (not applied - read-only task)

1. `decodeExec`: ops 13/14 -> `kind = 1` with `boolAddr=(v>>34)&0xFF`, `cond=(v>>42)&1`, `isEnd` for 14.
2. `CF_LOOP_END`: delete the `aL++;` line; read the step as `int8`: `((int)(lconst << 8) >> 24)`; `CF_LOOP_START`: if count == 0 jump to `v & 0x1FFF`; honour `is_repeat` (bit 13) by skipping `aL = start`.
3. Forward jumps: drop `continue` (or emit structured ifs); keep SKIP_OPTIMIZATION as the default (already done).
4. Optional hardening: exp-adjust on `.w` too; texture `off` argument; emit a visible stub (comment or `// UNSUPPORTED getWeights`) for dropped fetch opcodes so they show up in dumps;
   param-gen register layout; `ps` unsaturated.
5. Verify with a draw that has shadows on: dump `ps bool consts` for the draws of `c2b5e8b8/a08c03fc/3775d358` (only one draw was ever logged, b128).

## 5. Reproduction notes

- Trace evidence: `C:\itc-work\rdoc\dbg3\trace.txt` (history lines: `eid 30954 ps=ps_9473127d9a6bf8cf_12 ... post=['0.048','0.000','1824.000','1.000']`; step 3504-3509 `o0 = [0.049161, 0, 1824.6, 1]`),
  `disasm_DXBC.txt` instructions 2002-2024.
- FXC experiment (analysis only): `D3DCompile` + `D3DDisassemble` via ctypes on a copy of `work/shaders/ps_9473127d9a6bf8cf_12.hlsl` with flags 0, 4, 512, 1024, 16384, 32768
  (`fxc_dis.py` in the scratchpad). Flags 0/16384/32768 -> hoisted `continue`; 4 -> stores first; 512/1024 -> compile error. Same for `ps_0bbc553289131016_13.hlsl`.
- Dumped HLSL are from an older translator build (14:23); the structure of the state machine, loops, and `cexec` handling is identical in the 17:07 source.
