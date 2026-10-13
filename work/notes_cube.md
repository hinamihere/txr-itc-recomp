# Xenos (Xbox 360 GPU) cube maps - research notes for the D3D11 renderer

Read-only research: no existing file was edited, nothing was built, the game was not run.
Written 2026-10-05. Everything is plain ASCII.

Legend used below:
- **[SRC]** verified by reading source code (Xenia Canary local checkout / XenosRecomp / this project's code)
- **[DATA]** verified against this project's own game data (shader files in `work/extracted/DATA/SHADERS`, capture logs `work/*.err`)
- **[TEST]** verified by a numeric or compile test I ran (scratch scripts, see section 7)
- **[?]** not verified / unsure

Paths: `XENIA/` = `reference/xenia-gpu/src/xenia/gpu/` (sparse clone of xenia-canary, branch canary_experimental, commit
`0b0d57a1bc55f62e2bd43516bea96c848a9a6f96`; project root = `C:\Users\<user>\Documents\txr-itc-recomp`, `C:\itc-work` = `work/`).

---------------------------------------------------------------------------------------------------------------------------

## 0. Summary (read this first)

1. **`cube` (vector opcode 18)** computes `dest = (tc, sc, 2*ma, faceId)` from a cube vector (x, y, z). The vector is taken from
   lanes **[2], [3], [0] of the swizzled operand 0** (x = s0.z, y = s0.w, z = s0.x). The canonical encoding is
   `cube d, v.zzxy, v.yxzz`; operand 1 carries no extra information. faceId is the D3D order 0..5 = +X, -X, +Y, -Y, +Z, -Z.
2. The shader then does `rcp |dest.z|`, `mad ST, dest.xy, rcp, 1.5` giving S', T' in [1.0, 2.0), and calls
   `tfetchCube dst, (S', T', faceId)`. To sample a D3D11 `TextureCube` you **invert** that: `sc = 2*S'-3`, `tc = 2*T'-3`,
   `face = trunc(faceId)` clamped to 0..5, `dir = N[face] + sc*U[face] + tc*V[face]` (table in 2.2). Xenos face i == memory face i ==
   D3D11 array slice i. No face remap, no flips.
3. **Game-specific trap**: this game's shader compiler emits **two different operand swizzles** for `cube`: the canonical
   `.zzxy/.yxzz` (5 of the 15 cube shaders in `DATA/SHADERS`) and a rotated `.xxyz/.zyxx` (the other 10). The current translator
   (`xe_cube(r[src1Reg])`) and XenosRecomp both ignore the operand swizzle, so they feed the rotated vector into the formula. The
   translator must print operand 0 with its real 4-lane swizzle (and negate/abs/constant-register) and feed that to `xe_cube`.
4. **Guest layout**: faces are consecutive, each aligned to 4 KB: `face_k = base_address + k * faceStride`,
   `faceStride = align_up(alignedPitchBytes * alignedRows, 4096)` (= **0x20000** for the game's 512x512 BC1 cubes). Mips live in a
   separate region at `mip_address`, stored level-major (all 6 faces of level 1, then all 6 of level 2, ...). Verified on the game's
   real fetch constants: `mip_address - base_address = 0xC0000 = 6 * 0x20000`.
5. **Both cube fetch constants found in the logs are base-level-only** (fetch-constant mip filter = BaseMap), so a first version needs
   just six untiled 512x512 BC1 images per cube (decode exactly like a 2D BC1 texture, six times).
6. **Rendering into a cube face** is a normal 2D resolve whose destination base is the face address; there is no cube-specific
   hardware path (`copy_dest_array/slice` are for 3D volumes only). **In the captured frames no resolve ever targets a cube**: the
   256x256 pass is later fetched as a 2D texture (see section 4), so the "env cube-map face" interpretation in `NOTES.md` is not
   supported by the logs, and that pass is not safe to skip.
7. D3D11 samples cube maps seamlessly across face edges (cannot be disabled); Xenos/D3D9 does not [?]. Only affects the outer
   half-texel ring of each face; irrelevant for 512x512 sky maps.

---------------------------------------------------------------------------------------------------------------------------

## 1. The `cube` ALU instruction (CUBEv, vector opcode 18)

### 1.1 Encoding facts [SRC][DATA]
- 3-dword ALU instruction; vector opcode = bits 24..28 of dword 2 (`AluVectorOpcode::Cube = 18`, `shader_code.h` line 371,
  `V_CUBE` in `xgpu_shader.cpp`).
- Operand 0 = the "src1" fields: register byte = dword2 bits 16..23, select bit (1 = temp register r#, 0 = constant c#) = dword2 bit 31,
  swizzle = dword1 bits 16..23, negate = dword1 bit 26. Swizzle bytes are stored **relative to identity**: lane i selects
  `((swz >> 2*i) + i) & 3`. (`.zzxy` = 0xA6, `.yxzz` = 0xCD, `.xxyz` = 0xFC, `.zyxx` = 0x62.)
- The vector write mask is 0xF in every cube instruction I found (all 15 shader files).

### 1.2 Exact semantics [SRC: three independent Xenia implementations agree]
`XENIA/ucode.h` lines 1506-1547 (documentation), `dxbc_shader_translator_alu.cc` lines 247-354, `spirv_shader_translator_alu.cc` line
521+, `shader_interpreter.cc` lines 500-527 (CPU reference).

Let `s0` be operand 0 after swizzle/negate/abs. The cube vector is

```
x = s0[2] (= s0.z),   y = s0[3] (= s0.w),   z = s0[0] (= s0.x)
```

(for the canonical `v.zzxy`, `s0 = (v.z, v.z, v.x, v.y)`, so (x, y, z) = (v.x, v.y, v.z); lane 1 is a duplicate of lane 0).

```
if (|z| >= |x| && |z| >= |y|) { tc = -y;               sc = (z<0) ? -x :  x;  ma = 2*z;  id = (z<0) ? 5 : 4; }  // Z major
else if (|y| >= |x|)          { tc = (y<0) ? -z : z;   sc = x;                ma = 2*y;  id = (y<0) ? 3 : 2; }  // Y major
else                          { tc = -y;               sc = (x<0) ?  z : -z;  ma = 2*x;  id = (x<0) ? 1 : 0; }  // X major
dest = (tc, sc, ma, id)   // dest.x = T numerator, dest.y = S numerator, dest.z = 2*major axis (signed), dest.w = face id
```

Tie-break: `>=` with priority Z, then Y, then X. (Xenia notes the original R400 sq_alu used `>` and GCN uses `>=`; only exact
ties are affected [?].)

### 1.3 What the shader does with the result (canonical sequence) [SRC][DATA]
`XENIA/ucode.h` 1532-1536, and my disassembly of the real game shader `XBM_SH_T0.xpu` (instructions @16..@19):

```
cube       r3.xyzw, r3.xxyz, r3.zyxx        ; (tc, sc, 2*ma, id) in r3
max        r0.z, r3.w, r3.w   +  rcp r0.x, |r3.z|   ; co-issued: id -> r0.z, r0.x = 1/(2*|ma|)
mad        r0.xy, r3.yx, r0.xx, c255.zz     ; r0.x = sc/(2|ma|) + 1.5 = S',  r0.y = tc/(2|ma|) + 1.5 = T'   (c255.z = 1.5)
tfetchCube r3.xyz_, r0.xyz, tf6             ; coordinates = (S', T', face id)
```

Consequences: S', T' are in **[1.0, 2.0)** (the 1.5 bias is a mantissa-encoding trick, "range 1.0 <= x < 2.0" per the GPUOpen
article that Xenia cites; they are NOT 0..1 coordinates). The face id passes through untouched (exact floats 0.0 .. 5.0).
`c255 = (0, 1, 1.5, 0)` was confirmed in a verbose capture log immediately before a draw with the cube-using pixel shader
188fcacc (`work/c4.err`: "ps c254=(-0.5 4 1 0.666667) c255=(0 1 1.5 0)").

### 1.4 Operand swizzles seen in THIS game [DATA]
I scanned all 78 shader files in `work/extracted/DATA/SHADERS` (parsing the CF program, listing every ALU opcode 18 and every
`tfetch` whose instruction dimension is 3). 15 pixel shaders contain exactly one `cube` followed 3 instructions later by one
`tfetchCube` from fetch constant **tf6**; no vertex shader uses `cube`. All have write mask 0xF, no negate, and `r<n>` register sources.

| shader files                                                         | cube src0 / src1   | meaning                                           |
|----------------------------------------------------------------------|--------------------|---------------------------------------------------|
| XBM_SH_T1_B1, T1_B2, T2_B1, T2_B2, T3_B1                             | `.zzxy` / `.yxzz`  | canonical: register holds (x, y, z)               |
| XBM_SH_T0, T1, T2, T3, T4, and T0_L, T1_L, T2_L, T3_L, T4_L          | `.xxyz` / `.zyxx`  | rotated: register holds x in .y, y in .z, z in .x |

(`_L` variants have the cube instruction predicated: `(p0) cube ...`.) For the rotated variant the logical vector is
`(x, y, z) = (r.y, r.z, r.x)`: src0 = (z,z,x,y) = (r.x, r.x, r.y, r.z) = `.xxyz` and src1 = (y,x,z,z) = (r.z, r.y, r.x, r.x) = `.zyxx`
- exactly what is encoded. That also confirms that the hardware pairing is always (z,z,x,y)/(y,x,z,z) of the *logical* vector and
that reading only operand 0 is sufficient.

The two race-scene shaders in the project's dumps (`ps_188fcacc02901114_5.hlsl`, `ps_83699d3d5ea0613e_4.hlsl`) are **not** among
those 15 files: I matched the loaded-microcode hashes in the logs against all 78 files with a re-implementation of `Hash64`; only
`ParticleVPSC_PS.xpu` matched (which also validates the method), so the race shaders come from the compressed `BUILD*.DAT` archives.
Their swizzle is therefore unknown [?]; their HLSL shows the same `cube / max / rcp / mad(+1.5) / tfetchCube tf6` pattern. Suggestion:
make the translator emit a comment with the decoded cube operand swizzle so it can be read from a shader dump.

### 1.5 HLSL [TEST: compiles with D3DCompile ps_5_0 (flags 0 / strict / avoid-flow-control); numeric twin verified, section 7]

```hlsl
// Xenos CUBEv. s0 = vector operand 0 AFTER its 4-lane swizzle / negate / abs (e.g. "r[3].xxyz", "-r[7].zzxy", "c[9].zzxy").
// Result = (tc, sc, 2*ma, faceId).
float4 xe_cube(float4 s0)
{
    float cx = s0.z, cy = s0.w, cz = s0.x;
    float ax = abs(cx), ay = abs(cy), az = abs(cz);
    float4 r;
    if (az >= ax && az >= ay)       // Z major
    {
        bool neg = cz < 0.0;
        r = float4(-cy, neg ? -cx : cx, 2.0 * cz, neg ? 5.0 : 4.0);
    }
    else if (ay >= ax)              // Y major
    {
        bool neg = cy < 0.0;
        r = float4(neg ? -cz : cz, cx, 2.0 * cy, neg ? 3.0 : 2.0);
    }
    else                            // X major
    {
        bool neg = cx < 0.0;
        r = float4(-cy, neg ? cz : -cz, 2.0 * cx, neg ? 1.0 : 0.0);
    }
    return r;
}
```

Translator integration (`work/runtime/src/xgpu_shader.cpp`):
- `aluOperand()` (lines 297-311): the default branch builds the lane list from `vectorWriteMask`; add `case V_CUBE: mask = 15; break;`
  next to `V_DP4/V_MAX4` so operand 0 prints as a full 4-lane swizzle.
- line 406: replace `xe_cube(r[%u])` by `expr = "xe_cube(" + a0s + ")"` (this uses operand 0 with its swizzle, negate, abs and
  constant-register form). The result lanes map positionally to the write mask; with a partial mask append `.` + the dest mask letters to
  the call, because HLSL would otherwise truncate the float4 from the left (all 15 game shaders use 0xF, so this is only defensive).
- optionally warn if src1 is not the canonical complement of src0 (`(y,x,z,z)` of the logical vector).

---------------------------------------------------------------------------------------------------------------------------

## 2. `tfetch` with dimension = cube

### 2.1 Semantics [SRC]
`XENIA/dxbc_shader_translator_fetch.cc` lines 1453-1533 and `spirv_shader_translator_fetch.cc` lines 1528-1656 implement the same inverse.
The instruction takes 3 source lanes (after the instruction's own 6-bit source swizzle, absolute 2 bits per lane):

```
(S', T', F) = (src.x, src.y, src.z)          // S', T' in [1,2) from the sequence above, F = face id as float

sc   = S' * 2 - 3                             // -1 .. 1   (= sc/|ma|)
tc   = T' * 2 - 3                             // -1 .. 1   (= tc/|ma|)
face = min((uint)F, 5)                        // float->uint conversion (negative/NaN -> 0), clamp to 5
axis = face >> 1 (0=X, 1=Y, 2=Z),  neg = face & 1
// "remap the axes in a way opposite to the ALU cube instruction":
axis X: dir = (neg ? -1 : 1,   -tc,            neg ? sc : -sc)
axis Y: dir = (sc,             neg ? -1 : 1,   neg ? -tc : tc)
axis Z: dir = (neg ? -sc : sc, -tc,            neg ? -1 : 1)
```

then the normal cube-texture sample with `dir` (D3D12: SRV TEXTURECUBE; Vulkan: VK_IMAGE_VIEW_TYPE_CUBE). Offsets (`OffsetX/Y`, half-texel
units) are added in ST space before the transform; `OffsetZ` offsets the face index. The game's fetches use offsets 0 [DATA].

### 2.2 HLSL for the fetch [TEST]
Branch-free table form (identical to the switch form in the semantics above; both are in my scratch test):

```hlsl
// faces in D3D / D3DCUBEMAP_FACES order: 0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z
static const float3 kCubeN[6] = { float3( 1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0,-1, 0), float3(0, 0, 1), float3(0, 0,-1) };
static const float3 kCubeU[6] = { float3( 0, 0,-1), float3( 0, 0, 1), float3(1, 0, 0), float3(1, 0, 0), float3(1, 0, 0), float3(-1,0, 0) };
static const float3 kCubeV[6] = { float3( 0,-1, 0), float3( 0,-1, 0), float3(0, 0, 1), float3(0, 0,-1), float3(0,-1, 0), float3(0,-1, 0) };
float3 xe_cubeDir(float3 stf)        // stf = (S', T', face) = the three source lanes of tfetchCube
{
    uint f = (uint)clamp(stf.z, 0.0, 5.0);
    return kCubeN[f] + (stf.x * 2.0 - 3.0) * kCubeU[f] + (stf.y * 2.0 - 3.0) * kCubeV[f];
}
// per face this is: +X (1,-tc,-sc)  -X (-1,-tc,sc)  +Y (sc,1,tc)  -Y (sc,-1,-tc)  +Z (sc,-tc,1)  -Z (-sc,-tc,-1)
```

In the generated per-slot fetch helper (`xgpu_shader.cpp` lines 785-796): `tex6.Sample(smp6, uv)` becomes
`tex6.Sample(smp6, xe_cubeDir(uv))` (`SampleLevel(smp6, xe_cubeDir(uv), 0)` in vertex shaders). The existing `xe_applyswz()` (texture swizzle /
exp adjust) is applied to the result exactly as for 2D. The per-slot declaration `TextureCube<float4> texN : register(tN)` is already emitted.

### 2.3 Face order / orientation: why no remap or flip is needed [SRC][TEST]
- Face id 0..5 is `D3DCUBEMAP_FACES` (+X, -X, +Y, -Y, +Z, -Z) ([MSDN](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dcubemap-faces));
  this is also D3D11's TextureCube array-slice order and the DDS cube layout.
- The ALU rules in 1.2 are exactly the standard OpenGL/D3D cube-map face table: +X: sc=-z, tc=-y; -X: sc=+z, tc=-y; +Y: sc=x, tc=+z;
  -Y: sc=x, tc=-z; +Z: sc=x, tc=-y; -Z: sc=-x, tc=-y, with face texture coordinate s = 0.5*(sc/|ma|+1), t = 0.5*(tc/|ma|+1), i.e.
  **S'-1 and T'-1 are the ordinary 0..1 texture coordinates of that face's 2D image (row 0 at the top)**.
- Xenia uploads memory slice i into array layer i of a 6-layer image and views it as a cube (D3D12: `d3d12_texture_cache.cc` 1994-1998,
  Vulkan: `vulkan_texture_cache.cc` 1134-1135 and 2012-2013) with no per-face flips, using the same inverse as above.
- Numeric proof [TEST]: 200,000 random vectors -> `cube` -> `rcp`/`mad` -> inverse gives `v / max(|v|)` (to 2e-5), and
  `(face, S'-1, T'-1)` equals the GL/D3D table result every time; canonical and rotated register layouts give identical output.

### 2.4 Filtering, LOD, clamping, "1/2 texel" [SRC][?]
- No half-texel adjustment exists for cubes. Because of the +1.5 encoding the face coordinate is simply `S'-1`; the ordinary texel-center
  convention applies, which `TextureCube.Sample` already implements. Do not add 0.5/N offsets and do not use `2*S'-1` (the common
  mistake) - it is `2*S'-3` because `ma` carries a factor 2 (`rcp(2|ma|)`).
- The only rounding fudge in Xenia's fetch path is `kTextureCoordEpsilon = 1.5/1024` added to offsets for point-sampled fetches
  (`shader.h` 568; texel-center snapping applies to 2D only); irrelevant for the bilinear cube fetches in this game.
- Edge filtering: Xenia's `non_seamless_cube_map` option (`gpu_flags.cc` 41-46) says Direct3D 9 does not filter across cube faces and tries
  to reproduce that with VK_EXT_non_seamless_cube_map; `ucode.h` 587-589 says the Xenos "likely doesn't support seamless sampling" (stated as
  likely). D3D11 has no switch for it: D3D10+ cube sampling is seamless (secondary source: web search summary, not checked against the
  D3D11 spec [?]). Expect tiny differences confined to the outer ~half texel of each face.
- Address modes: `GetClampModesForDimension` (`texture_util.cc` 586-607) treats cube clamp modes as not applicable (always clamp to edge); D3D11
  ignores them for cube sampling as well [?].
- LOD: the game's fetch has `useCompLod=1` (automatic LOD), filters "use fetch constant" (3,3,3), aniso "use fetch constant" (7), lod bias 0,
  no register gradients [DATA]. Reconstructed directions are continuous across face edges (`v/|ma|` is the same from either face), so
  D3D11's implicit derivatives give a sane LOD. Register gradients (not used here) would be in cube space (`ucode.h` 575-578).
- Mip filter: fetch-constant dword3 bits 23..24 = 2 ("BaseMap": no mip filtering) in both cube constants [DATA]. If you ever upload the mip
  chain, restrict the sampler (MaxLOD = 0 for BaseMap) - the project's `GetSampler` currently sets `MaxLOD = D3D11_FLOAT32_MAX` and treats
  mip filter 2 as point-mip, which would pick lower mips on minification.
- Predicated/looped fetches: `Sample` needs uniform control flow for derivatives. The `_L` variants have `(p0) cube` / `(p0) tfetchCube`
  inside predication; same issue as for 2D fetches in the translator, not cube-specific.

---------------------------------------------------------------------------------------------------------------------------

## 3. Guest-memory layout of a cube texture

### 3.1 Fetch constant (6 dwords, `XENIA/xenos.h` 1245-1346) [SRC] and the game's real values [DATA]

| field                     | dword / bits                   | cube notes                                                                          |
|---------------------------|--------------------------------|-------------------------------------------------------------------------------------|
| type                      | d0 [1:0]                       | 2 = texture                                                                         |
| sign x/y/z/w              | d0 [9:2]                       | 2 bits each                                                                         |
| clamp x/y/z               | d0 [18:10], 3 bits each        | not applicable to cube                                                              |
| pitch                     | d0 [30:22] (9 bits)            | base row pitch in **texels** >> 5 (`pitchTexels = field << 5`)                       |
| tiled                     | d0 [31]                        | 1 = tiled (cubes are tiled in practice)                                             |
| format                    | d1 [5:0]                       | e.g. 18 = k_DXT1                                                                    |
| endianness                | d1 [7:6]                       | same for all faces                                                                  |
| stacked                   | d1 [10]                        | 0 for cube                                                                          |
| base_address              | d1 [31:12]                     | >> 12 (4 KB units): physical address of face 0, base level                          |
| width-1 / height-1        | d2 [12:0] / [25:13]            | 13 bits each; cube is square; the `size_2d` layout is used (not size_3d)            |
| stack_depth               | d2 [31:26]                     | **5 for cube** (6 faces - 1); informational, ignored                                |
| num_format, swizzle, exp  | d3 [0], [12:1], [18:13]        | as for 2D                                                                           |
| mag / min / mip filter    | d3 [20:19] / [22:21] / [24:23] | 0 point, 1 linear, **2 base-map-only (mip filter)**, 3 use fetch const              |
| mip_min / mip_max level   | d4 [5:2] / [9:6]               | highest level index, not a count                                                    |
| dimension                 | d5 [10:9]                      | **3 = cube** (fetch constant: 0=1D, 1=2D/stacked, 2=3D, 3=cube; the `tfetch` instruction dimension also uses 3 for cube) |
| packed_mips               | d5 [11]                        | mip tail packing for levels <= 16 texels                                            |
| mip_address               | d5 [31:12]                     | >> 12; start of the mip region (0 = no mips)                                        |

Real constants from the project's logs (`work/c10.err`, slot 6, bound by the car/reflection pixel shaders):

```
raw = 84004802 0FCE7052 143FE1FF 01280D10 00000243 0FDA7E00
d0 type=2 clampXYZ=2,2,0 pitch=16<<5=512 tiled=1 | d1 fmt=18 (DXT1) endian=1 (8in16) base=0x0FCE7000
d2 width=512 height=512 stack_depth=5 | d3 swz=0x688 (identity) expAdj=0 mag=lin min=lin mip=2 (base map)
d4 mip_min=0 mip_max=9 | d5 dimension=3 (cube) packed_mips=1 mip_address=0x0FDA7000
second cube: raw = 84004802 0F73B052 143FE1FF 01280D10 00000243 0F7FBE00   (base 0x0F73B000, mips 0x0F7FB000)
```

Both are 512x512 BC1 cubes with a full chain declared (mips 0..9) but a base-map-only filter.

### 3.2 Face stride, face addresses and mip layout [SRC: `XENIA/texture_util.cc` `GetGuestTextureLayout`, lines 216-470]
Notation: `bpb` = bytes per block, `bw,bh` = block width/height in texels (BC1/2/3: 4x4 with 8 or 16 B; most others 1x1), `align(a,n)`
rounds up to a multiple of n. A cube is an array of 6 slices; all faces have the same size (square, power of two in practice).

```
// BASE LEVEL (level 0) - the pitch comes from the fetch constant
pitchBlocks0 = align( ceil(pitchTexels / bw), 32 )                    // tiled textures: 32-block alignment
rows0        = align( ceil(height / bh),      32 )                    // (if the whole texture lies in the packed tail, packedLevel == 0, i.e. <= 16 texels,
                                                                      //  Xenia uses next_pow2(height) here)
faceStride0  = align( pitchBlocks0 * bpb * rows0, 4096 )              // 4 KB aligned ("each mip and stack slice / cube face", xenos.h 1185-1190)
face k, level 0 at   base_address + k * faceStride0                   // k = 0..5 = +X,-X,+Y,-Y,+Z,-Z

// MIP LEVELS L >= 1 live at mip_address (only if mip_address != 0 and mip_max_level > 0); NOT derived from the pitch field
pitchBlocksL = align( ceil( max(next_pow2(width)  >> L, 1) / bw ), 32 )
rowsL        = align( ceil( max(next_pow2(height) >> L, 1) / bh ), 32 )
faceStrideL  = align( pitchBlocksL * bpb * rowsL, 4096 )
levelOffsetL = sum_{j=1}^{L-1} faceStrideJ * 6                        // each level holds all 6 faces ("array slices are stored
                                                                      //  within levels", texture_util.h 134-137)
face k, level L at   mip_address + levelOffsetL + k * faceStrideL

// PACKED TAIL: packedLevel = (log2_ceil(min(w,h)) > 4) ? log2_ceil(min(w,h)) - 4 : 0        (512 -> 5)
// levels >= packedLevel share one 32x32-block tile per face, stored where level 'packedLevel' would be;
// sub-offsets inside it come from texture_util::GetPackedMipOffset (texture_util.cc 124-214).
// Cube mips use the exact 6-face count (the "align array size to 4" rule applies to stacked 2D only).
```

Worked example (the game's cube, 512x512 BC1, bpb=8, bw=bh=4, pitch 512): pitchBlocks0 = 128 -> 1024 B/row, rows0 = 128 ->
faceStride0 = **0x20000**.

```
face 0 (+X) 0x0FCE7000   face 1 (-X) 0x0FD07000   face 2 (+Y) 0x0FD27000
face 3 (-Y) 0x0FD47000   face 4 (+Z) 0x0FD67000   face 5 (-Z) 0x0FD87000   end = 0x0FDA7000 == mip_address   [DATA: matches d5]
mip region from 0x0FDA7000 (packedLevel = 5): per-face strides L1 0x8000, L2..L5 0x2000 each
  L1 at +0x00000 (6 x 0x8000), L2 at +0x30000, L3 at +0x3C000, L4 at +0x48000, packed tail (levels 5..9) at +0x54000, end +0x60000
```

The base-to-mip distance (6 x 0x20000) is verified against real data; the per-level numbers above are derived from Xenia's model and were
not compared against a memory dump [?] (not needed while the filter is base-map-only).

### 3.3 What changes compared with your 2D untile of a base level
1. Detect cube: `tf.dimension() == 3` (d5 bits 9..10) *and* `type == 2`; the shader-side dimension must also be 3 (otherwise keep binding a dummy, as now).
2. Size: width/height = d2 [12:0], [25:13] (+1) - already what `DecodeTexture` reads; `stack_depth` (d2 [31:26] = 5) is above those fields, so it
   does not disturb the existing parse.
3. Run the existing 2D decode **six times**, with `src = PhysPtr(base + k * faceStride0)`. Tiling (`TiledByteOffset`), pitch handling, endian
   swap, format conversion and BC handling are identical for every face; there is no face-dependent swizzle or tiling.
   Practical trick that needs no change to `DecodeTexture`: make a copy of the fetch constant per face,
   `tf2 = tf; tf2.d[1] = (tf.d[1] & 0xFFF) | faceAddr; tf2.d[5] = (tf.d[5] & ~(3u << 9)) | (1u << 9);` (faceAddr is 4 KB aligned; the last line
   makes it look like a 2D texture) and call `DecodeTexture(tf2, ...)` per face.
4. Footprint (`TextureFootprint`, used by `FlushOverlapping`, write tracking and cache validation): `6 * faceStride0` for the base (plus the mip
   region if used), not one face. Hash all six faces (or one hash per face) so a face updated by a resolve invalidates only its own slice.
5. Create `Texture2D` with `Width = Height = N`, `ArraySize = 6`, `MipLevels = 1` (or M), `MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE` (as
   `MakeDummy` already does for the dummy cube); initial data is face-major: `subresource(face f, mip m) = m + f * MipLevels`, i.e.
   `sd[f] = { faceData[f], rowPitch, 0 }` for one mip. A null SRV desc gives a TextureCube view. Mind the order mismatch if mips are added
   later: D3D11 is face-major, Xenos memory is level-major.
6. BC1 faces are 512 (a multiple of 4) so no padding issue; for smaller mips use block-rounded sizes (1x1 and 2x2 mips occupy one 4x4 block).

---------------------------------------------------------------------------------------------------------------------------

## 4. Rendering into a cube face (EDRAM resolve)

- The hardware has no cube-specific resolve. The D3D9 runtime resolves a render target into a texture with the generic resolve packets
  (`RB_COPY_CONTROL`, `RB_COPY_DEST_BASE/PITCH/INFO`, a RECTLIST draw of 3 vertices - `XENIA/xenos.h` 965-1026). To target face k it
  simply writes `RB_COPY_DEST_BASE = faceAddress_k` (byte address = cube base + k * faceStride, plus the tiled offset of the destination
  point if any; D3D may even point it before the texture for negative offsets), `RB_COPY_DEST_PITCH.copy_dest_pitch` = the texture's base pitch
  (32-aligned), and the destination format/endian/swap/number/exp-bias fields in `RB_COPY_DEST_INFO`.
- `copy_dest_array` / `copy_dest_slice` select 3D-volume addressing (Z/8 slices, `RB_COPY_SURFACE_SLICE`); they are **not** used for cube
  faces. Xenia's `draw_util.cc` 1414-1530 has no cube path (the only "cube" hits in its resolve/render-target code are the 8192 size limit);
  Xenia emulates cube render targets purely by (1) writing the face into guest memory like any resolve and (2) later loading six slices
  from memory when the texture is fetched, so the face-address model above is what makes it work there [SRC; real D3D9 behaviour is inferred
  from that, not observed].
- A face of mip level > 0 would target `mip_address + levelOffsetL + k * faceStrideL`.
- Orientation: the game must render each face with the view matrix that produces the standard cube-face image (same orientation as in
  2.3). The resolved image is a normal top-row-first 2D image, so nothing is flipped when it becomes slice k.

What the project's logs show [DATA]:
- Every **square** resolve rect in the logs (a cube face must be square): 512x512 -> 0x0146E000 (HUD), 1024x1024 depth -> 0x0106E000 and
  0x01CAE000 (shadow maps), 1024x1024 -> 0x0156E000, and **256x256 -> 0x0102E000 (color fmt 7 = RGBA16F) and 0x01C6E000 (fmt 6)**. None of
  them lands in a cube texture (cube bases are 0x0FCE7000..+0xC0000 + mips ..+0x60000, and 0x0F73B000..+0xC0000 + mips ..+0x60000).
- `0x0102E000` is later fetched as a **2D** texture: `tex slot 6 dim=1 fmt=54 256x256 base=0102E000` (bound by 40 draws in `c10.err`, e.g. by
  pixel shader 9e3540ff94103948, 291 dwords). So the 256x256 / 465-draw pass is consumed by 2D fetches; it looks like a planar or
  paraboloid-style reflection target, not "one face of a cube per frame", and skipping it ("Known problems" item 3 in `NOTES.md`) would
  blank that texture. All cube fetch constants that appear in the logs point to static BC1 assets, not to resolve targets. Other
  tracks / times of day / cars might differ [?].
- Architecture note, in case dynamic cubes ever appear: `g_resolved` is keyed by destination address, so a face resolve is an entry at
  `faceAddr_k`. Simplest correct path: when binding a cube, `FlushResolved(faceAddr_k)` for every face that has a pending entry and decode
  the six faces from memory. Faster path: `CopySubresourceRegion` each compatible resolved 2D texture into array slice k of a cached
  6-layer cube texture, so only the face(s) updated this frame change.

---------------------------------------------------------------------------------------------------------------------------

## 5. Things commonly gotten wrong (checklist)

1. **Ignoring the cube operand swizzle** (XenosRecomp's `cube(r{src1Register}, ...)`, the project's `xe_cube(r[src1Reg & 0x3F])`): breaks the
   rotated `.xxyz/.zyxx` variant used by 10 of this game's 15 cube shaders; also wrong for negated or constant-register operands.
2. **Treating `cube`'s result as the final coordinates and the fetch coordinates as a direction.** The project currently returns unsigned
   lane picks such as `(s.y, s.x, |z|, id)` for Z-major (wrong lane roles, no signs, `ma` not doubled, so S',T' end up in ~[0.5, 2.5]) and then
   passes `(S', T', face)` **as the direction** to `TextureCube.Sample` (no inverse at all). Both functions must be replaced together.
3. **Wrong inverse constants**: `sc = 2*S'-3` (not -1, not `S'-0.5`); `ma` is *twice* the major axis.
4. **Lane order**: `dest.x = T`, `dest.y = S` (T first). The `mad` swaps them (`r.yx`), and `tfetchCube` takes (S', T', face).
5. **Face-ID handling**: it is a float 0.0..5.0 produced by `cube`; convert with truncation + clamp(0,5) (NaN/negative -> 0).
6. **Flips**: none are needed in D3D11. Do not mirror faces, do not swap +Y/-Y, do not v-flip: image row 0 is the top and t grows
   downward in both worlds (checked against the face table). Typical flip bugs come from OpenGL image-origin conventions.
7. **1/2 texel**: none for cubes; S'-1 is the plain 0..1 coordinate. (The point-sampling epsilon in Xenia applies only to 2D snapping.)
8. **Layout**: faces are 4 KB aligned slices, the mip region is level-major at `mip_address`, the packed-mip tail exists per face, and the fetch
   constant's pitch applies to the base level only. Do not compute the face stride as `w*h*bpp`; use aligned pitch x aligned rows rounded to
   4 KB (the two agree for this game's 512x512 BC1, 0x20000, but not in general).
9. **Mip filter**: BaseMap (2) means "no mips"; the sampler's `MaxLOD` must be 0 if you upload more than the base level. The declared level
   count in the fetch constant (`mip_max=9`) is there even when the filter says base-only.
10. **Dimension mismatches**: a cube fetch against a non-cube fetch constant (or vice versa) cannot be bound in D3D11; keep the dummy.
    UnleashedRecomp's comment says real hardware shows a 2D texture on all six faces in that case ("duplicates the first face",
    `UnleashedRecomp/gpu/video.cpp` 5865-5867) - not verified by me [?].
11. **Seamless filtering** differs (see 2.4) - cosmetic.
12. **Derivatives in divergent flow**: `(p0)`-predicated cube fetches (the `_L` variants) must not break `Sample` derivatives.

---------------------------------------------------------------------------------------------------------------------------

## 6. What the local code does today and what to change (file/line map)

`work/runtime/src/xgpu_shader.cpp`
- 689-698: prelude `xe_cube` (simplified guess; replace with 1.5 and add `xe_cubeDir` from 2.2).
- 406: `case V_CUBE` emits `xe_cube(r[src1Reg & 0x3F])` (no swizzle, no neg/abs, assumes a temp register).
- 297-311: operand lane/mask selection (`vectorWriteMask`); add `V_CUBE -> mask 15`.
- 230-243: `textureFetch()` already passes `r[src].<3 lanes>` using the instruction's absolute source swizzle - correct for cube
  (lanes = S', T', face). `dimension == 3` already selects `xe_fetchCube`.
- 785-796: `xe_fetchCube`: wrap the coordinate in `xe_cubeDir()`.
- 737: the `TextureCube<float4>` declaration is already generated.

`work/runtime/src/xgpu_d3d.cpp`
- 466-477 `TexFetch` (has `dimension()`, `base()`, `pitchTexels()`, `tiled()`, `endian()`, `format()`).
- 521-536 `TextureFootprint`: needs the 6-face variant.
- 728-837 `DecodeTexture`: reuse per face (see 3.3 item 3).
- 902-903: `if (shaderDim == 3) return g_dummyCube.get();` - the place to build/cache a real cube (the resolved-texture shortcut above it is
  already skipped for cube dims).
- 563 / 839-857 / 1376: dummy cube creation (shows the correct `ArraySize=6` + `MISC_TEXTURECUBE` pattern).
- 953-969 `GetSampler`: cube address modes are irrelevant; a base-map mip filter should set `MaxLOD = 0`.

Suggested validation (cheap, visible on screen): bind a test cube whose 6 faces are solid, different colors (face-order check) and one with a
per-face gradient/letter (orientation check) at slot 6; reflections must be continuous across face edges and follow the order +X,-X,+Y,-Y,+Z,-Z.
Also, in a debug shader build, output `normalize(xe_cubeDir(...))` against the original vector.

Alternative to inverting (what XenosRecomp does): record the true direction at `cube` time (`float3 cubeDir[2]`), return an index in `.w`, and let
`tfetchCube` pick it. It avoids the round trip, but (a) breaks if the shader modifies S'/T'/face between the two instructions, (b) caps the number
of cube ops per shader (their array has 2 entries), and (c) still needs the operand swizzle handled. The exact inverse above is cheap (a few ALU
ops per fetch) and general; use that.

---------------------------------------------------------------------------------------------------------------------------

## 7. Verification log and source index

Tests I ran (scratch files in `C:\Users\<user>\AppData\Local\Temp\claude\...\scratchpad`, nothing in the project):
- `verify_cube.py` - Python twin of 1.5 + 2.2 vs the GL/D3D face table over 200,000 random vectors; canonical and rotated register layouts; table
  form vs switch form. Result: **0 mismatches** (appendix A).
- `cube_test.hlsl` + `hlsl_syntax.py` - the HLSL of 1.5 and 2.2 (switch form and table form, `Sample` call) compiled with `d3dcompiler_47.dll`
  `D3DCompile` as `ps_5_0` with flags 0, strict, avoid-flow-control: all OK. (The code blocks in this file were also extracted and compiled.)
- `scan_cube.py`, `scan_blob.py`, `dis_around.py`, `list_fetch.py` - parse the CF program of the 78 shader files, list `cube`/`tfetchCube`
  (section 1.4), disassemble `XBM_SH_T0.xpu`, list the texture fetches per shader. Cross-checked with an independent brute-force scan of every
  12-byte triple (ALU opcode 18 followed within 6 triples by a dimension-3 `tfetch` of tf6): the same 15 files, no others.
- `match_hash.py` - re-implements the project's `Hash64` (CRC32C chains) and matched `IM_LOAD` hashes in the logs against the shader files
  (only `ParticleVPSC_PS.xpu` matched, so the race shaders are not in `DATA/SHADERS`).
- Log mining (`grep` over `work/*.err`): cube fetch constants (only two distinct, both 512x512 BC1), all square resolves, fetch of 0x0102E000 as 2D,
  `c255 = (0 1 1.5 0)` before a draw with shader 188fcacc.
- Web (secondary): GPUOpen "Fetching from cubes and octahedrons" (1.5 constant, range [1,2), V_CUBETC/SC/MA/ID sequence); MSDN
  `D3DCUBEMAP_FACES`; D3D10+ seamless cube filtering (search summary only).

Source index (all `[SRC]`):
- `XENIA/ucode.h` 1506-1547 (`kCube` doc), 575-589 (cube gradients, seamless note), 1760 (needed components)
- `XENIA/dxbc_shader_translator_alu.cc` 57-73, 247-354; `spirv_shader_translator_alu.cc` 521-640; `shader_interpreter.cc` 500-527
- `XENIA/dxbc_shader_translator_fetch.cc` 805-836 (offsets), 905-911 (size), 1453-1533 (inverse); `spirv_shader_translator_fetch.cc` 1528-1656
- `XENIA/texture_util.cc` 23-122 (`GetSubresourcesFromFetchConstant`: 6 faces / stack_depth 5), 216-470 (`GetGuestTextureLayout`), 586-607;
  `texture_util.h` 134-189; `texture_address.h` 21-43; `xenos.h` 95-100, 129-135, 161-168, 965-1026, 1185-1190, 1245-1346
- `XENIA/d3d12/d3d12_texture_cache.cc` 1455-1730 (per-slice / per-level load loops), 1994-1998; `vulkan/vulkan_texture_cache.cc` 1134-1135, 2012-2013
- `XENIA/draw_util.cc` 1414-1530 (resolve destination); `gpu_flags.cc` 41-46
- XenosRecomp: `tools/ps3/UnleashedRecomp/tools/XenosRecomp/XenosRecomp/shader_recompiler.cpp` 321-324, 342-344, 766-768, 1491;
  `shader_common.h` 151-160, 183-190; `README.md` 79-81 (their rationale for the direction side-channel)
- UnleashedRecomp: `tools/UnleashedRecomp/UnleashedRecomp/gpu/video.cpp` (DDS cube loading, 2D-as-cube hack at 5865)

---------------------------------------------------------------------------------------------------------------------------

## Appendix A - Python oracle (the verified twin of the HLSL)

```python
import random
def cube_alu(s0):                      # s0 = swizzled operand 0 (4 floats)
    x, y, z = s0[2], s0[3], s0[0]
    ax, ay, az = abs(x), abs(y), abs(z)
    if az >= ax and az >= ay:
        n = z < 0; return (-y, -x if n else x, 2*z, 5.0 if n else 4.0)
    elif ay >= ax:
        n = y < 0; return (-z if n else z, x, 2*y, 3.0 if n else 2.0)
    else:
        n = x < 0; return (-y, z if n else -z, 2*x, 1.0 if n else 0.0)
def cube_dir(S, T, F):                 # tfetchCube inverse
    sc, tc = S*2-3, T*2-3
    f = int(min(max(F, 0.0), 5.0)); neg = f & 1; ax = f >> 1
    if ax == 0: return (-1.0 if neg else 1.0, -tc, sc if neg else -sc)
    if ax == 1: return (sc, -1.0 if neg else 1.0, -tc if neg else tc)
    return (-sc if neg else sc, -tc, -1.0 if neg else 1.0)
def gl_face(d):                        # standard GL/D3D table
    x, y, z = d; ax, ay, az = abs(x), abs(y), abs(z)
    if ax >= ay and ax >= az: f, sc, tc, ma = (0, -z, -y, ax) if x > 0 else (1, z, -y, ax)
    elif ay >= az:            f, sc, tc, ma = (2, x, z, ay) if y > 0 else (3, x, -z, ay)
    else:                     f, sc, tc, ma = (4, x, -y, az) if z > 0 else (5, -x, -y, az)
    return f, 0.5*(sc/ma+1), 0.5*(tc/ma+1)
for _ in range(200000):
    v = [random.uniform(-1, 1) for _ in range(3)]
    tc, sc, ma2, fid = cube_alu((v[2], v[2], v[0], v[1]))      # canonical .zzxy
    reg = (v[2], v[0], v[1], 0.0)                              # rotated register layout, operand .xxyz
    assert cube_alu((reg[0], reg[0], reg[1], reg[2])) == (tc, sc, ma2, fid)
    S, T = sc/abs(ma2) + 1.5, tc/abs(ma2) + 1.5                # rcp + mad(1.5)
    d = cube_dir(S, T, fid); m = max(abs(c) for c in v)
    assert all(abs(a - b/m) < 2e-5 for a, b in zip(d, v))      # direction recovered up to scale 1/|major|
    f, s, t = gl_face(v)
    assert f == int(fid) and abs(s-(S-1)) < 2e-5 and abs(t-(T-1)) < 2e-5
```
