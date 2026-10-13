# ReXGlue SDK vs. our own runtime - research notes for TXR Import Tuner Challenge

Read-only research (web + local files). Nothing was installed, built, run or modified.
Written 2026-10-05. Everything is plain ASCII.

Legend:
- **[WEB]**   read on a GitHub / web page (URLs in section 8)
- **[LOCAL]** checked on this machine with read-only commands
- **[INFER]** my inference from the evidence, not stated by a source
- **[EST]**   my estimate, no hard data

`<root>` = `C:\Users\<user>\Documents\txr-itc-recomp`; `C:\itc-work` = `<root>\work`, `C:\itc-xr` = `<root>\tools\XenonRecomp`.

---------------------------------------------------------------------------------------------------------------------------

## 0. TL;DR (read this first)

1. **What it is.** ReXGlue SDK (github.com/rexglue/rexglue-sdk) = "Xbox 360 Recompilation Runtime and Toolkit", BSD-3-Clause,
   created 2026-01-22, ~1000 stars, stable v0.10.0 (2026-08-21), nightlies almost daily (0.10.0.24-dev on 2026-10-02).
   Codegen is a descendant of XenonRecomp (+ rexdex's recompiler) with its own analysis pipeline; the runtime is a port of
   Xenia's kernel/HLE, memory, VFS, **Xenos GPU (D3D12 on Windows, Vulkan on Linux/macOS, shipped as a plugin DLL)**,
   XMA decoding through an FFmpeg fork, SDL3 audio/input (+XInput), ImGui overlays, XContent saves, achievements. [WEB]
2. **It is realistic, but it is a replacement of the whole runtime layer, not a drop-in.** Your 95 MB of XenonRecomp output
   cannot be reused (macros / context / function-table plumbing differ; you re-run `rexglue codegen`), and your hand-written
   `itc_host.dll` (kernel, PM4 + D3D11 renderer, audio) would be replaced by ReXGlue's. About 25 community games already use
   it, including racing games (Project Gotham Racing 4, Forza Motorsport 2, Midnight Club LA, Skate 3). [WEB]
3. **Toolchain blocker on THIS machine.** ReXGlue's CMake aborts unless the compiler is **Clang >= 18** (C++23); GCC / MinGW /
   MSVC `cl` are rejected. On Windows it is built with clang targeting the **MSVC ABI** (MSVC STL + Windows SDK): CI = LLVM from
   Chocolatey on a runner with Visual Studio; wiki = "Visual Studio 2022 + Desktop C++ workload + C++ Clang Compiler 20.x+". [WEB]
   This machine has VS Build Tools 2026 (18.10.3) with **no C++ workload at all** (no MSVC, no Windows SDK, no VS clang) and
   only a Cygwin-target clang and a MinGW-target clang (both unusable for ReXGlue). cmake 4.4.2, ninja 1.13.2, git, python are
   fine. First step would be a ~6-10 GB Visual Studio C++ install that needs UAC elevation. [LOCAL]
4. **Evidence the GPU side would work for this game:** Xenia (same GPU/kernel code base) lists Import Tuner Challenge as
   "gameplay"; a Xenia Canary reporter says "720p/1440p - no issues" with vsync on, another played 5 in-game days flawlessly
   with default settings. The one well-known game-specific problem is a timing-dependent black screen after the first Quest-mode
   race, fixed by a one-instruction XEX patch for **the same module hash as your default.xex (AAD3CAA44B5F7D92)** - section 5. [WEB][LOCAL]
5. **Recommendation:** do not switch blindly, do not dismiss it either. Two cheap checks decide it: (i) run the Xenia Canary that
   is already in `tools/` on your ISO as a rendering oracle (30 min, section 4/6); (ii) a time-boxed 1-day ReXGlue spike (install
   toolchain, prebuilt SDK, `rexglue init`, `codegen`, build, boot to the logo). If both pass, porting is likely the faster path
   to "everything rendered correctly + music + saves" (**[EST] 3-5 working days to parity, then GPU/XMA/saves come mostly free**),
   versus **[EST] 2-4+ weeks** of long-tail work (cube maps, mips, sign/gamma, EDRAM/MSAA, XMA, saves, CP performance) in the
   own runtime. Main risks: toolchain change, API churn, D3D12-only (Intel Iris Xe performance unknown), and the possibility that
   the wrong-lighting bug is in the recompiled CPU code (then a port would not fix it). Whatever you decide, the Xenia reference
   you already have locally contains the exact semantics for the pieces you are missing (section 6.4).

---------------------------------------------------------------------------------------------------------------------------

## 1. What ReXGlue provides

### 1.1 Identity, license, activity [WEB]
- Repo `rexglue/rexglue-sdk`, description "Xbox 360 Recompilation Runtime and Toolkit". Author/maintainer Tom Clay ("crack");
  contributors incl. Loreaxe, mystixor, Graine25 (Windows/Linux/ARM64/CI). Discord linked from README.
- **License: BSD 3-Clause** ("Copyright (c) 2026 Tom Clay; portions derived from Xenia, Copyright (c) 2022 Ben Vanik and Xenia
  contributors"). GitHub shows "NOASSERTION" only because the text is wrapped in quotes. Redistribution and commercial use are
  allowed if the notices are kept. README adds a no-piracy statement and "not affiliated with Microsoft/Xbox".
- Status text: "early development ... expect things not to work quite right and breaking public API updates".
  The wiki Home still says v0.3.x - the wiki lags the code (code is 0.10.0.x).
- Activity (fetched 2026-10-05): created 2026-01-22; 645 commits; 982 stars; 148 forks; 48 open (about 31 issues + 17 PRs);
  last push 2026-10-02. Release cadence: v0.8.0 (multi-binary / DLL manifests), v0.9.0 (achievements, SDL3 windowing, GPU plugin
  framework), v0.10.0 (2026-08-21: macOS via MoltenVK, incremental codegen, audio fixes), then nightlies.
- Release assets: `rexglue-sdk-0.10.0-win-amd64.zip` **44.75 MB** (3,303 downloads), linux-amd64 121.6 MB, linux-arm64
  116.9 MB, mac-amd64 71.2 MB, mac-arm64 69.1 MB. Nightlies have the same five assets (e.g. nightly-20261002-bd833a2a).
- Platforms: Windows x64 (Windows ARM64 is an open PR), Linux x64 + ARM64, macOS x64 + ARM64.

### 1.2 The recompiler: `rexglue codegen` [WEB]
- Heritage: "foundations from Xenia, XenonRecomp and rexdex's recompiler". Early versions called XenonRecomp; since v0.8 it has
  its own analysis + emitter (`src/codegen`, ~350-entry instruction dispatch table, C++23 output, `PPC_UNIMPLEMENTED` traps).
  Third-party kit 360tools says "XenonRecomp is no longer required" (old pipeline moved to `legacy/`).
- CLI: `rexglue init --app_name X --app_root DIR` (scaffold), `rexglue codegen X_config.toml` (XEX/ELF -> C++), `rexglue migrate`
  (upgrade a project to a newer SDK), `rexglue recompile-tests`. Global flags: `--force` (generate despite validation errors),
  `--enable_exception_handlers` (SEH wrappers from PDATA), `--log_level`, `--log_file`.
- Input config = TOML. Required: `project_name`, `file_path` (XEX or ELF; retail encrypted/compressed XEX is handled by its own
  XEX loader), `out_directory_path`. Optional: `patch_file_path` / `patched_file_path` (XEXP title update).
  Codegen switches (all default false): `skip_lr`, `skip_msr`, `ctr_as_local`, `xer_as_local`, `cr_as_local`, `reserved_as_local`,
  `non_argument_as_local`, `non_volatile_as_local`, `enable_exception_handlers`; `setjmp_address` / `longjmp_address`;
  `[analysis]` tuning (`max_jump_extension`, `data_region_threshold`, `large_function_threshold`, `exception_handler_funcs`);
  manual overrides `[functions]` (name / size / end / parent), `[[switch_tables]]` (address = the `bctr`, register, labels),
  `[[invalid_instructions]]`, `[[midasm_hook]]`, `[rexcrt]` (optional native replacements for RtlAllocateHeap, memcpy, ...).
- **Automatic discovery (7 stages):** register entries (import thunks, save/restore helper signatures such as `__savegprlr_14`,
  **.pdata exception tables**, TOML entries, XEX entry point) -> scan code/data regions -> iterative discovery (linear sweep, branch
  following, `bl` targets, **jump-table detection by backward scan for `mtctr`/load patterns** incl. absolute / byte / half-word /
  computed tables, MSVC-RTTI vtable scanning) -> gap fill -> second discovery -> merge/seal -> validate (every `bl`/`b` must
  resolve; `--force` emits a fatal abort at unresolved sites).
- **Manual part:** the wiki says outright that unresolved functions are expected: "you must manually define function boundaries in
  the TOML, implement missing kernel imports, and iteratively refine". Recent history shows a long tail of codegen fixes
  (conditional bcctr tails #365, switch tables #359/#370/#430/#435/#461, indirect-call discovery on Midnight Club LA #363,
  Plants vs Zombies #355, invalid switch tables on DJ Hero 2 #171) - it is improving fast but not "push button".
- Other features: MMIO-aware loads/stores emitted for GPU (0x7FC8-0x7FCF) and XMA (0x7FEA-0x7FEF) register pages; CSR flush-mode
  tracking (denormals scalar vs VMX); mid-ASM hooks (void or bool C++ hook with PPCRegister& args, conditional return/jump);
  function overrides (`REX_HOOK`, `REX_HOOK_RAW`, `REX_IMPORT`, `REX_STUB*`, `REX_EXPORT`).
  **Mid-ASM hooks cannot patch instruction immediates** (documented capabilities are register edits and control flow only).

### 1.3 Shape of the generated code vs what you have (can XenonRecomp output be reused?) [WEB][LOCAL]
ReXGlue (wiki "Generated Code Structure", `include/rex/ppc/func.h`):
```
using PPCFunc = void(PPCContext& ctx, uint8_t* base);       // same signature family as XenonRecomp
DEFINE_REX_FUNC(sub_82003A40) { REX_FUNC_PROLOGUE(); ...
    PPC_STORE_U32(base, ctx.r1.u32 + 4294967264, ctx.r1.u32);   // explicit base argument
    ctx.lr = 0x82003A50;  sub_82010000(ctx, base);               // direct call
    __imp__DbgPrint(ctx, base);                                  // kernel import = host fn with the same signature
```
Guest->host table `PPCFuncMappings[]` = `{ guest_addr, host_fn }`, copied into a dispatch table in guest memory by
`Runtime::Setup()`; indirect calls go through `rex::runtime::ResolveIndirectFunction(addr)`. Output files:
`<proj>_init.h/.cpp`, `<proj>_recomp.0..N.cpp` (split for parallel compile), `sources.cmake`.

Yours (`tools/XenonRecomp/XenonUtils/ppc_context.h`, 103 files / 95.4 MB in `work/out`): `PPC_FUNC_IMPL(x)`, `PPC_LOAD_U32(x)` with an
**implicit** `base`, `PPC_CALL_INDIRECT_FUNC(x)` -> `PPC_LOOKUP_FUNC(base, x)`, `__imp__<Name>` overrides in `itc_host.dll`.

Verdict: **same family, not source compatible** (macro arity, `PPCContext` layout incl. `msr`/`vscr_sat`/`last_indirect_target`/
`PPCFPSCRRegister`, headers/namespaces, function-table setup, ReXGlue's MMIO macros and `rexcrt_` natives). A compatibility shim is
possible but pointless: regenerate from `default.xex` (minutes). Config migration notes:
- `itc.toml` bits: helper addresses (savegprlr/restgprlr/...) are auto-detected by ReXGlue's signature scan; your
  `[[midasm_hook]] HookReturn0 @0x8214FA78 (r3, return=true)` maps 1:1 to ReXGlue's `[[midasm_hook]]`.
- `jump_tables.toml`: 331 XenonAnalyse `[[switch]]` entries (`base`, `r`, `default`, `labels`). ReXGlue wants `[[switch_tables]]` with
  `address` = **the `bctr`**, `register`, `labels`. I checked the first entry in `work/image.bin`: XenonAnalyse's `base` (0x82469710)
  is the `lis` at the start of the sequence and the `bctr` is at base+0x14 (0x82469724) for the absolute table type, so conversion
  needs a small script that scans forward from `base` for the first `0x4E800420`. **Try codegen WITHOUT any switch tables first**
  (auto-detection) and only add the ones that fail.
- The game has `.pdata` (it imports `__C_specific_handler`, `RtlUnwind`, `RtlCaptureContext`), which gives ReXGlue accurate function
  boundaries; `enable_exception_handlers` exists for SEH.

### 1.4 The runtime [WEB]
- **Core:** `rex::Runtime` owns Memory (4 GB guest space, 512 MB physical at host offset 0x200000000, same 0xA0000000/0xC0000000/
  0xE0000000 physical views as yours), ExportResolver, FunctionDispatcher, VirtualFileSystem, KernelState; graphics/audio/input are
  injected via `RuntimeConfig`. Kernel = port of Xenia: `src/kernel/{xboxkrnl,xam,xbdm,crt}`, `src/system/*` (kernel_state, xthread,
  xmemory 97 KB, mmio_handler, function_dispatcher, xex_module ...). XAM export table is 181 KB.
  Your game's imports (from the Xenia log): 63 xam + 135 xboxkrnl, of which Xenia lacks only XamShowFriendsUI,
  XamShowGamerCardUIForXUID, XamShowPlayerReviewUI (UI only) and `__C_specific_handler`, `RtlUnwind`, `RtlCaptureContext` (SEH -
  irrelevant to a JIT, handled by ReXGlue's SEH wrappers if needed). [LOCAL]
- **GPU:** the Xenos emulator is a **runtime-loaded plugin** `rexgpu-xenos` (C ABI: `rex_gpu_abi_version()`, `rex_gpu_create()`; ABI v1;
  CLI `--gpu_plugin="xenos"`; backend `d3d12 | vulkan | any`). Source is a port of Xenia canary:
  `src/graphics/d3d12/` (command_processor 234 KB, render_target_cache 301 KB, pipeline_cache 158 KB, texture_cache 112 KB,
  deferred_command_list ...), `src/graphics/vulkan/` (same set, bigger), shared `src/graphics/pipeline/{shader,texture,render_target}`
  (DXBC translator ~490 KB across 5 files, SPIR-V translator ~640 KB, ucode interpreter, texture cache/conversion/extent/formats),
  `util/draw.cpp` (draw_util) + `util/draw_extent_estimator.cpp` (the Xenia code that emulates EVENT_WRITE_EXT for predicated tiling).
  D3D12 shader bytecode is checked in (`src/graphics/shaders/bytecode/d3d12_5_1`) - no shader-compile step in the build.
  PR #162 ported Xenia-Edge/Canary features (FSR, resolution scaling, async shader compilation, readback modes).
  CVars inherited from Xenia (`render_target_path_d3d12` rtv/rov, `readback_resolve`, `draw_resolution_scale`, `occlusion_query`,
  `clear_memory_page_state`, `texture_cache_limit` default 384 MB, ...). **DX11 backend request (#460) was declined**
  ("DirectX12 provides the features needed to run the Xenos-GPU implementation"); custom plugins (e.g. a native renderer) are allowed
  - Skate 3's project replaced Xenos emulation with its own renderer this way.
- **Audio:** `src/audio`: `xma_context.cpp` (28 KB), `xma_decoder.cpp` (12 KB), `xma_register_file.cpp`; 320 XMA contexts of 64 bytes
  in physical memory, MMIO page 0x7FEA0000, one decoder worker thread; decodes with an **FFmpeg fork** (`wmarti/FFmpeg`, branch
  `xenia-ffmpeg-canary-full`) built through `thirdparty/ffmpeg-overlay`; enabled decoders: mp3, mp3float, wmapro, wmav2 and the custom
  **`xmaframes`** (= Xenia's XMA frame decoder). Output: SDL3 audio driver (default), opt-in XAudio2 backend (SDL had a
  time-dilated-audio bug in Forza 2, fixed in #403), `nop`. v0.10.0 fixed XMA loop wraps and MDCT tail on context recycle.
- **Input:** SDL3 gamepads, XInput, keyboard+mouse (`mnk`), `nop`; `xam_input.cpp`. (Open: a race in InputSystem::RefreshDevices #475.)
- **File system / how the game is provided:** host-folder VFS, **not ISO/STFS**. `\Device\Harddisk0\Partition1` = game root
  (symlinks `game:` and `d:`), `PartitionUpdate` = `update:`; `cache:` deliberately not registered. Game root = first CLI argument
  (`--game_data_root`) or `<exe_dir>/assets`; extract the ISO first (PGR4's setup guide: extract-xiso; 360tools has STFS/ISO helpers).
  Your `work/extracted/` folder is exactly what it wants.
- **Saves / profile:** XAM XContent (`xam_content*.cpp`, `xcontent.h`) under `user_data_root` (CVar; default = platform user dir),
  profile via `xam_user.cpp` (one profile; multi-profile request #187 closed "not planned"), achievement manager with persistence
  (PR #361). Fixes for save integrity landed (#428, #395); open: XContentQueryVolumeDeviceType returns success with type 0 (#452).
  Your game imports XamContentCreate/Delete/Close/CreateEnumerator/GetCreator/SetThumbnail/GetDeviceData, XamShowDeviceSelectorUI. [LOCAL]
- **UI/dev tooling:** SDL3 window (default 1280x720), ImGui overlays (F3 debug: host/guest FPS, threads, memory; backtick console with
  CVar commands; F4 settings editor), Tracy profiler on by default, RenderDoc API header vendored, texture dump/replace (#333).

---------------------------------------------------------------------------------------------------------------------------

## 2. Build requirements, and what THIS machine has

### 2.1 What ReXGlue needs [WEB]
| Item | Requirement |
|---|---|
| Compiler | **Clang >= 18**, C++23, 64-bit only. `CMakeLists.txt`: `if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$") FATAL_ERROR`; AppleClang >= 16. MSVC `cl`, GCC and MinGW-GCC are not supported. Wiki says Clang 20+ |
| Windows ABI | Presets use plain `clang`/`clang++` + **Ninja Multi-Config**, `x86-64-v2`. No clang-cl / MinGW handling in CMake. CI: `choco install llvm cmake ninja` on `windows-latest` (the workflow does not install Visual Studio; the runner image already ships it), then `cmake --preset win-amd64 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ...`. So: MSVC STL + Windows SDK headers/libs are required (D3D12/DXGI) |
| Wiki (Getting Started) | "Visual Studio 2022 Community, Desktop development with C++ workload, incl. C++ Clang Compiler for Windows (20.x or newer) and MSBuild support for LLVM (clang-cl)" - the MSBuild/clang-cl part is only needed for VS projects, not for CMake+Ninja [INFER] |
| CMake | >= 3.25 (`cmake_minimum_required(VERSION 3.25)`) |
| Ninja | yes (Multi-Config generator) |
| Dependencies | All vendored: `thirdparty/` + 22 git submodules (FFmpeg fork, SDL3 3.4.x, imgui, spdlog, fmt, glslang, SPIRV-Tools/Headers, Vulkan headers/loader/VMA, MoltenVK, Tracy, xxHash, simde, tomlplusplus, CLI11, inja, utfcpp, libmspack, o1heap, Catch2). **No vcpkg for the SDK** (some game projects add vcpkg for their own deps). No Python needed to build |
| Options | `REXGLUE_USE_D3D12` (ON on Windows), `REXGLUE_USE_VULKAN` (OFF on Windows), `REXGLUE_ENABLE_TRACY` ON, `REXGLUE_ENABLE_PERF_COUNTERS` ON, `REXGLUE_BUILD_TESTS` OFF, FIDELITYFX off/experimental |
| Windows gotcha | libmspack is a git symlink: clone needs **Developer Mode + `git config core.symlinks true`** (issue #227) |
| Consuming the SDK | Three modes: binary install (registers in CMake user package registry), git submodule, FetchContent. A game project does `find_package(rexglue CONFIG REQUIRED)` with `-DREXSDK=<prefix>` or env `REXSDK`. Prebuilt zip works for the "binary install" mode if you point REXSDK at the unzipped folder |
| Build time / size | CI nightly (5 platforms in parallel, sccache): **11-13 min** wall clock [WEB]. Cold local build on this 4C/8T laptop [EST] 20-45 min. Tracked repo 28.7 MB + submodules [EST] 1-2 GB clone. **Prebuilt Windows SDK zip = 44.75 MB** (skip the SDK build entirely) |
| Game project | Your generated code is 95.4 MB in 103 files [LOCAL]; [EST] full cold clang compile 20-60 min on 8 threads; ~1-2 GB RAM per 1 MB generated TU at -O1/-O2, so use `-j4`..`-j6` on 15.7 GB. v0.10.0 added incremental codegen |

### 2.2 What this machine has [LOCAL] (all commands read-only)
| Check | Result |
|---|---|
| `where cl` | **not found** |
| `where clang` / `clang-cl` | `C:\msys64\usr\bin\clang.exe` and `clang-cl.exe`: clang 21.1.8, **Target x86_64-pc-windows-cygnus** (MSYS2/Cygwin runtime) - unusable. It is also what a bare `clang` on PATH resolves to |
| other clang | `C:\msys64\mingw64\bin\clang.exe` 22.1.8, **Target x86_64-w64-windows-gnu** (MinGW ABI, not on PATH). Not the MSVC ABI that ReXGlue's CI/docs use; no evidence anybody builds ReXGlue with it [INFER: expect pain with D3D12 headers/SEH/ABI; not recommended]. Its `clang-cl` says target pc-windows-msvc but there is no MSVC/Windows SDK to link against |
| GCC | `C:\mingw64\bin\gcc.exe` 16.2.0 (MinGW-W64 ucrt) - rejected by ReXGlue's CMake |
| `where cmake` | `C:\mingw64\bin\cmake.exe` **4.4.2** (>= 3.25 OK) |
| `where ninja` | `C:\mingw64\bin\ninja.exe` **1.13.2** OK |
| `where git` | Git 2.55.0.windows.5; system `core.symlinks=false`; Developer Mode **off** (matters only if building the SDK from source) |
| `where python` | Python 3.14.8 (not needed for ReXGlue) |
| Visual Studio | `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools` = **Build Tools 2026 18.10.3**, installed 2026-10-04. `C:\Program Files\Microsoft Visual Studio` does not exist. vswhere: only workload `Microsoft.VisualStudio.Workload.MSBuildTools`; **not installed:** `...Component.VC.Tools.x86.x64`, `...Workload.VCTools`, `...Component.VC.Llvm.Clang`, `...VC.Llvm.ClangToolset`, `...Windows11SDK.26100/22621`, `...Windows10SDK.19041`, `...VC.CMake.Project`. No `VC\Tools\MSVC`, no `C:\Program Files (x86)\Windows Kits\10`, no vcvars bat |
| Hardware | i5-1145G7 (4C/8T), 15.7 GB RAM, **Intel Iris Xe** (driver 30.0.100.9864), C: has 48.1 GB free |
| Rights / installers | session NOT elevated; account `...\<user>` is in the local Administrators group (UAC split token) so a Visual Studio Installer "Modify" is possible after a UAC prompt. `winget` present; no choco / scoop |

### 2.3 Exact commands (copy/paste; the first block is read-only, the second block was NOT run)
Check what is installed on any Windows machine:
```powershell
$vw = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
& $vw -all -products * -prerelease -format json                                   # every VS / Build Tools instance
& $vw -all -products * -prerelease -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath   # has MSVC?
& $vw -all -products * -prerelease -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath      # has VS clang?
Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Include"                  # Windows SDK versions
where.exe cl clang clang-cl cmake ninja git python
clang --version      # need >= 18 (20+ recommended) AND "Target: x86_64-pc-windows-msvc"
```
What to install (needs elevation; component IDs are the VS 2022 ones - confirm them in the installer UI under Modify ->
"Desktop development with C++" -> optional "C++ Clang Compiler for Windows" + "Windows 11 SDK"; **[UNVERIFIED for VS 2026]**):
```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe" modify `
  --installPath "${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\BuildTools" `
  --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Llvm.Clang `
  --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --includeRecommended --passive --norestart
# alternative clang source: winget install --id LLVM.LLVM -e   (still needs the MSVC toolset + Windows SDK from above)
```
Then build from a shell where the compiler is unambiguous: open "x64 Native Tools Command Prompt" (or run `vcvars64.bat`), put
`...\VC\Tools\Llvm\x64\bin` (or `C:\Program Files\LLVM\bin`) FIRST on PATH so `C:\msys64\usr\bin\clang.exe` (Cygwin target) does not
win, and/or pass `-DCMAKE_C_COMPILER=<full path>\clang.exe -DCMAKE_CXX_COMPILER=<full path>\clang++.exe`.
Non-ASCII user path (`...\<user>\Documents`): your GCC chokes on it; clang+MSVC normally copes [INFER], but keep the ASCII junction trick
(`C:\itc-work`) as a fallback and set the `user_data_root` CVar to an ASCII folder. Prebuilt SDK + project folder on an ASCII path
also avoids the issue. Disk: [EST] VS C++ workload 6-10 GB + build trees 3-6 GB; you have 48 GB free.

---------------------------------------------------------------------------------------------------------------------------

## 3. Workflow for a new game, effort, limitations, maturity

### 3.1 Steps from XEX to a running exe (and what you already have)
| # | Step | Manual? | You already have | [EST] time |
|---|---|---|---|---|
| 0 | Toolchain: VS C++ workload + Clang, prebuilt SDK zip, `REXSDK` | manual, one-off | no (see 2.2) | 0.5 day (mostly downloads) |
| 1 | Extract ISO to a folder | manual | yes (`work/extracted`) | 0 |
| 2 | `rexglue init --app_name itc --app_root C:\itc-rex` (CMake, presets, `main.cpp`, `ReXApp` subclass, `itc_config.toml`) | auto | - | minutes |
| 3 | Edit `itc_config.toml` (`file_path`, flags, hooks, optional switch tables) | manual | partly (`itc.toml`, 331 tables, hook) | 1-2 h |
| 4 | `cmake --build --preset win-amd64-debug --target itc_codegen` | auto | - | minutes |
| 5 | Fix validation errors: unresolved calls / bad boundaries / jump tables via `[functions]`, `[[switch_tables]]`, `[[invalid_instructions]]`, or `--force` | **manual, iterative** | experience from XenonRecomp | 0.5-1 day [EST] |
| 6 | Pre-patch the image if the game needs it (see section 5) | manual | `patch_*.py` habit | 1 h |
| 7 | Full build of generated code | auto | - | 20-60 min wall clock [EST] |
| 8 | Run `itc.exe --game_data_root <extracted>`; add missing kernel imports / stubs (`REX_EXPORT`, `REX_STUB`), mid-ASM hooks, CVars | **manual** | - | 0.5-2 days [EST] |
| 9 | Per-game GPU/audio/input quirks (CVars, hooks) | **manual** | - | 0.5-2 days [EST] |
Maintainer on GPU work: "for the foreseeable future each individual port project will be figuring out how that game did things".

### 3.2 How long do ports take? (evidence)
- No official timeline ("I honestly don't have a timeline"); 360tools says "runtime integration (stubs, per-title quirks, rendering/audio)
  is the genuinely demanding work"; press says alpha builds appeared "within weeks of the SDK's release"; PGR4's author says development is
  deliberately slow. I found **no published hours/days figures**. My estimate for parity with your current state is in section 6.2.

### 3.3 Known limitations / open problems [WEB]
- Early-stage, breaking API changes; wiki partly outdated; "not a turnkey solution, requires advanced knowledge of Xbox 360 internals".
- **D3D12 required on Windows** (DX11 declined #460). Xenia's own config comment says the auto render-target path is RTV except on Intel
  GPUs (stencil-test bug breaks D3D9-style clears), which implies the slower, accurate **ROV path on Intel Iris Xe** [INFER] - performance
  there is unknown (measure with Xenia Canary, same backend design).
- Open GPU/graphics issues: #480 Dead or Alive Xtreme 2 black window in one mode (render-target content fine, final blit black, both
  rtv and rov); readback trade-offs (TTT2 #447); Ninja Gaiden 2 needed `clear_memory_page_state` (#341); macOS/MoltenVK glitches (#446).
  Fixed bug classes that mirror your symptoms: #456 (SPIR-V tfetch used exp_adjust from the wrong fetch-constant dword -> dark/black/green
  textures), #283 (Vulkan format map), #280 (ZPD occlusion queries).
- Open runtime issues: #404 (Windows relwithdebinfo build: guest thread gets invalid base pointer, 0xC0000005), #476/#459/#382 (host FPSCR
  init on host threads -> SIGFPE on x86_64), #409 (guest C++ exceptions escaping XThread), #407 (REX_EXPORT_STUB leaves stale r3),
  #412 (guest sockets blocking), #453 (XeCryptBnQwNeRsaPubCrypt no-op on non-Windows), #452 (XContent device type).
- Closed but telling: #290 (InitLoggingEarly crash on Windows v0.7.5), #372 (MSVC C++20 u8 literal), #92 (AllocFixed assertion).
- Maintainers respond fast (many fixes within days) and nightlies carry them; **pin a version** (prebuilt 0.10.0 or a specific nightly).

### 3.4 Known games [WEB]
ReXGlue-based (awesome-x360-recomps list, ~25): Skate 3, Naughty Bear, Lollipop Chainsaw, Banjo-Kazooie Nuts & Bolts, Viva Pinata TiP,
GoldenEye 007, Blue Dragon (re:Blue), Lost Odyssey, Ninja Gaiden II, Ace Combat 6, Crackdown 1+2, Dante's Inferno, Castlevania SotN XBLA,
Fable II (two projects), **Project Gotham Racing 4** (boots, menus + races work; "FAR from polished"), DoDonPachi, Silent Hill Downpour,
Daytona USA XBLA, Condemned 2, DBZ Budokai HD, DB Raging Blast 2, Halo 3 (delta build), LEGO Dimensions, Deadly Premonition.
Issue tracker also mentions Forza Motorsport 2, Midnight Club LA, Kameo, Halo Reach, DJ Hero 2, Dead or Alive Xtreme 2, Rock Band 3.
XenonRecomp-based racing projects for comparison: Test Drive Unlimited (early), Sonic Free Riders (menus + race). No Tokyo Xtreme Racer
recomp exists as far as I could find (web search found nothing).
Skate 3 (v2.0+) is an example of the end state: replaced Xenos emulation by a native D3D12/Vulkan renderer for "more than 2x the frame rate".

---------------------------------------------------------------------------------------------------------------------------

## 4. The Xenia Canary build already on this machine [LOCAL]
Folder `<root>\tools\xenia_canary_windows` (listed, NOT run):
- `xenia_canary.exe` 17,360,384 bytes, 2026-10-03 20:11 - **runnable** (log header: `canary_experimental@7347cd12e on Oct 3 2026`).
- `xenia-canary.config.toml` (64 KB), `xconfig.settings`, `recent.toml`, `xenia.log` (63 KB, 1048 lines), `LICENSE`; folders `cache`,
  `cache0`, `cache1`, `cache_host`, `content`. **No README** in the folder.
- `recent.toml`: last title = `tools\iso\Import Tuner Challenge (USA, Europe).iso` (IMPORT TUNER CHALLENGE).
- **GPU settings** in the config: `gpu = "any"` (D3D12 on Windows, Vulkan fallback), `render_target_path_d3d12 = ""` (auto, see 3.3),
  `render_target_path_vulkan = ""`, `draw_resolution_scale_x/y = 1`, `readback_resolve = "none"`, `async_shader_compilation = true`,
  `vsync = true`, `gpu_3d_to_2d_texture = true`, `window_offset_in_edram = true` (predicated tiling fix), `occlusion_query = "fast"`;
  audio `apu = "any"`, `xma_decoder = "new"`, `use_dedicated_xma_thread = true`; `[Content]/[Storage]` roots default to the folder.
- **What the log shows (run 2026-10-04 20:13):** DXGI adapter Intel(R) Iris(R) Xe Graphics (0x8086:0x9A49); D3D12 features: ROV yes,
  resource binding tier 3, tiled resources tier 3, pixel-shader stencil ref yes, programmable sample positions tier 1, unaligned BC
  textures no. Disc image mounted, `\default.xex` loaded: **Module Hash AAD3CAA44B5F7D92**, Title ID 555307EE, image 0x82000000..0x829B0000.
  Import report: xam 63 imports (60 implemented), xboxkrnl 135 (132 implemented). It then opened `\DATA\TXR\MOVIE`, `\DATA\AUDIO`, and ended
  with "Cheap-skate exit!" (window closed) - **"Loaded 0 shaders from storage"**, i.e. the run never reached 3D, so it tells nothing yet
  about rendering quality or speed on this Iris Xe. A local gamer profile was auto-created under `content`.
- Suggested use: it is the **reference implementation of the same GPU design ReXGlue ships**. Run the ISO to the race view (Free Run),
  compare with your renderer's frame, try `render_target_path_d3d12 = rtv` vs `rov`, note FPS. If you need to get past the post-race black
  screen in Quest Mode, enable the patch in section 5 (Xenia reads patches from a `patches/` folder; check the Xenia Canary wiki).

---------------------------------------------------------------------------------------------------------------------------

## 5. Game-specific facts found along the way (independent of which runtime you use)
Xenia compatibility trackers [WEB]: xenia-project/game-compatibility #651 (since 2016, 25 comments, label state-gameplay + vsync-off-speedup)
and xenia-canary/game-compatibility #411 (2025: "720p/1440p - no issues. The game goes double speed if vsync is off"; a later comment:
5 in-game days flawless, default settings, Arch Linux / RX 580). Problems reported over the years: crash after "start new game" (old
builds), **black screen after finishing the first Quest-mode race** (dialogue never loads), speed tied to vsync.

Fix that the community uses - `xenia-canary/game-patches/patches/555307EE - Import Tuner Challange.patch.toml` (sic), author Gliniak:
```
title_id = "555307EE"   hash = "AAD3CAA44B5F7D92"  # default.xex  <- identical to the Module Hash in YOUR xenia.log
[[patch]] name = "Fix blackscreen after returning to menu"
  desc = "Change required loading check cycles from 0x3C to 0x1A. For faster CPUs use lower value like 0x10."
  [[patch.be32]] address = 0x8218e4ec  value = 0x2b0b001a      # disabled by default in the repo
```
My decode [INFER]: 0x2B0B001A = `cmplwi cr6, r11, 26` (0x2B0B003C would be the original `cmplwi cr6, r11, 60`). Other commenters used
0x2B0B000F (15) or 0x2B0B0001. So the game has a **frame/vsync-count based "loading check"**, and game speed is tied to the swap rate.
Consequences for the recompiled build: (1) Quest Mode may black-screen after the first race for the same reason; (2) the constant must
be patched **in the image before codegen** (a mid-ASM hook cannot change an immediate; ReXGlue's `OnPostLoadXexImage` patches memory
after load, which does not change already-recompiled code); your `patch_*.py` / `default_clean.xex` flow can do it; (3) keep it in mind
when judging frame pacing: your in-game 12-15 fps vs a vsync-locked game loop.

---------------------------------------------------------------------------------------------------------------------------

## 6. Recommendation

### 6.1 The options
- **(a) Keep fixing the own runtime.** Everything stays as is (MinGW, D3D11, your tooling). Each missing GPU feature is a few days; the
  list is long and new scenes keep exposing new Xenos behaviour.
- **(b) Port to ReXGlue.** New toolchain, new runtime; get Xenia-grade GPU (EDRAM/ROV, full texture cache: cube, 3D, mips, packed mips,
  CTX1/DXN/..., sign/gamma/exp-adjust, MSAA, resolve semantics, predicated-tiling extent estimator, async pipelines + disk cache), real XMA
  decoding, XContent saves, rumble, ImGui/RenderDoc/Tracy tooling - all in one move, if the bug is in the GPU emulation.
- **(c) Hybrid: stay on the own runtime, borrow Xenia/ReXGlue pieces** (XMA, texture sign/cube/mips, extent estimator) - see 6.4.
  (A fourth variant - ReXGlue runtime + your own D3D11 renderer as a GPU plugin - is technically allowed by the plugin ABI but throws away
  the reason to switch: the Xenos plugin is what fixes your renderer-accuracy problems.)

### 6.2 Candid estimate for (b)  [EST - no published numbers exist]
| Phase | Working time | Notes |
|---|---|---|
| Toolchain (VS C++ + Clang, unzip SDK, scaffold project) | 0.5 day | mostly downloading; UAC prompt; PATH shadowing by msys clang |
| `init`, config, codegen, fix unresolved calls/jump tables | 0.5-1 day | PDATA exists -> good boundaries; 331 known switch tables to compare against |
| First full build (95 MB generated C++, clang) | 0.5 day wall clock, little attention | use -j4..-j6; RAM 15.7 GB |
| Boot to logo -> menus (imports, SEH, `HookReturn0`, XEX pre-patch, profile prompt) | 0.5-2 days | Xenia implements all but 6 of the 198 imports |
| GPU/audio/saves validation + CVar tuning on Iris Xe | 0.5-1 day | compare with Xenia Canary result first |
| **Total to reach today's state** | **about 3-5 days** (up to ~1 week if codegen needs hand-holding) | |
After parity the remaining GPU/XMA/saves work is mostly CVars and game quirks rather than new emulation code.

(a) for comparison [EST]: cube maps 1-1.5 d (notes_cube.md already derived the maths), mips + packed mips + 3D 1-2 d, texture sign/gamma/exp-adjust
+ CTX1/DXN formats 1-2 d, finding the lighting/black-region cause 1-3 d, XMA 2-3 d (borrowing Xenia's), saves 1-2 d, CP performance 3-5 d,
EDRAM/MSAA/float formats/depth precision 2-4 d => **2-4+ weeks**, high variance, but visible progress every day and no toolchain risk.

### 6.3 Risks of (b)
1. **The bug might not be in the GPU emulation.** Saturated blue/magenta/red noise and a black region can also come from wrong CPU-side data
   (a recompiled VMX/`vpkd3d`/rounding bug, bad vertex or constant data). ReXGlue shares the XenonRecomp-style PPC semantics, so a CPU-side bug
   would survive the port. -> Run Xenia (it uses a JIT, independent CPU path) as an oracle first.
2. **Performance on Intel Iris Xe is unknown.** D3D12 only; auto render-target path is probably ROV on Intel (accurate, heavy); RTV is faster
   but less accurate. Your D3D11 path gets ~12-15 fps in-game and is CP-thread bound; Xenia's CP is far leaner per draw but its pixel path
   on an iGPU may or may not beat that. Measure with Xenia Canary (same design) before committing.
3. **Toolchain/admin:** needs a multi-GB Visual Studio C++ install via UAC; Clang version inside VS 2026 unknown (must be >= 18); VS component IDs unverified.
4. **API churn / bugs:** pre-1.0, breaking changes, several open Windows/x86_64 runtime issues (#404, #476), codegen edge cases; pin a version.
5. **Loss of your debugging stack:** the `ITC_*` env vars, PM4 dumps, shader dumps and `sheet.py` flow do not exist there (you gain RenderDoc,
   Tracy, ImGui overlays, `dump_shaders`, texture dump/replace, Xenia GPU traces).
6. **Game quirks come back in a new form:** post-race black screen (section 5), vsync-locked game speed, networking hook, SEH.
7. Licensing is fine (BSD-3, keep notices); the README forbids piracy use - you use your own ISO.
8. Nothing is destroyed: develop the port in a sibling folder (e.g. `C:\itc-rex`) and keep `work/` as the fallback.

### 6.4 What to borrow quickly (works for (c), and is the reference for (b)'s debugging)
Local Xenia checkout = `<root>\reference\xenia-gpu\src\xenia\gpu` (canary_experimental 0b0d57a1); ReXGlue equivalents under `src/graphics/pipeline/...`.
| What | Where | [EST] effort | Targets which symptom |
|---|---|---|---|
| **Per-component texture sign / gamma / exp-adjust** | `xenos.h` L119-127 (`TextureSign`: unsigned / signed two's complement / unsigned-biased 2*c-1 / gamma), L1263-1266 (`sign_x..w` = fetch dword0 bits 2..9 - the "ignored bits" in NOTES.md), L1316 (`exp_adjust`); applied in `dxbc_shader_translator_fetch.cc` L1576, 1934, 2347, 2386, 2464 | 0.5-1 d | **prime suspect for noisy blue/magenta/red road and rails** (normal maps with signed/biased components) and dark/green textures (ReXGlue #456 was the same bug class) [INFER] |
| Cube maps | your `notes_cube.md`; `dxbc_shader_translator_alu.cc` L247-354 (cube op), `..._fetch.cc` L1453-1533 (tfetchCube), `d3d12_texture_cache.cc` ~L1994-1998 (6-layer upload) | 1-1.5 d | black reflections / sky |
| Mip chains, packed mips, guest layout, tiling | `texture_util.cc` `GetPackedMipOffset` L124, `GetGuestTextureLayout` L216, `texture_info.cc` `GetMipExtent` L123, `texture_address.h` (Tiled2D/3D) | 1-2 d | single-mip textures, 3D |
| Format conversions (CTX1, DXN, DXT3A, DXT5A, 16-bit snorm/unorm, 10/11/11, 8_8_8_8_GAMMA) | `shaders/texture_load_*.cs.xesl` (161 shader files in that folder; HLSL compute templates), `texture_info_formats.inl` | 0.25 d per format | 2-channel normal maps, odd formats |
| 3D texture sampled as 2D | CVAR `gpu_3d_to_2d_texture` in `d3d12_texture_cache.cc` | 0.5 d | black 3D fetches |
| Predicated tiling + EVENT_WRITE_EXT | `draw_extent_estimator.cc` (15.7 KB; ReXGlue `util/draw_extent_estimator.cpp`), CVAR `window_offset_in_edram` | 1-2 d | your "all masks = tile 0" hack |
| EDRAM formats and resolves (7e3 float10, 16_16_16_16 float, float24 depth, MSAA resolve) | `render_target_cache.cc`, `shaders/pixel_formats.xesli` (49 KB), `edram.xesli`, `resolve.xesli`, `draw_util.cc` (`GetResolveInfo`) | 2-4 d (partial) | format 12/3 aliasing hack, exp bias -3, MSAA edges |
| Depth bias for decals; occlusion query modes | CVARs `depth_bias_shader_offset`, `occlusion_query` | 0.5 d | z-fighting, flares |
| **XMA decoder** | ReXGlue `src/audio/xma_context.cpp` + `xma_decoder.cpp` + `xma_register_file.cpp` + `include/rex/audio/xma/*`; FFmpeg fork `wmarti/FFmpeg` branch `xenia-ffmpeg-canary-full` (custom decoder `xmaframes`; minimal decoder list in `thirdparty/ffmpeg-overlay/codec_list.c`). Your local Xenia checkout has NO `apu` folder - take it from ReXGlue (BSD-3). Game imports: XMACreateContext / XMAReleaseContext + XAudio render-driver client/frame submit | 2-3 d; main risk = building the FFmpeg fork with MinGW | silent music |
| Saves / profile | `src/kernel/xam/xam_content*.cpp`, `xam_user.cpp`, `include/rex/system/xcontent.h` | 1-2 d | saves, profile prompt |
| Game patch | section 5 (address 0x8218E4EC) | 15 min | Quest-mode black screen |

### 6.5 Suggested decision path
1. **Today, 30 min:** run `xenia_canary.exe` on the ISO to the Free Run race view (try `render_target_path_d3d12` = rtv and rov; enable the
   section-5 patch if needed). Compare with your renderer's frame and note FPS on this Iris Xe. If Xenia renders correctly -> the GPU emulation
   is the culprit and (b) is attractive; if Xenia shows the same black region/noise -> the cause is CPU-side or game-specific and a port would not
   help (then continue (a)/(c)).
2. **Then a 1-day spike:** VS C++ workload + Clang, unzip `rexglue-sdk-0.10.0-win-amd64.zip` (or the newest nightly), `rexglue init`,
   `codegen` on `work/extracted/default.xex`, build, boot to the Ubisoft logo. Abort if codegen/build/boot is not clearly converging.
3. If the spike converges, port (phases in 6.2) in a sibling folder; keep `work/` as the fallback. If you decide against it, do the (c) quick
   wins in this order: texture sign/gamma bits -> cube maps -> mips -> XMA -> saves.
4. If the plan is (b), do not spend time on XMA/saves/cube maps in the old runtime.

---------------------------------------------------------------------------------------------------------------------------

## 7. Unverified / open questions
- Which clang version VS 2026 (18.10) bundles (docs say 20.x+ for VS 2022) and whether the VS 2022 component IDs above exist unchanged in VS 18.
- Whether clang finds a VS 2026 install on its own (should, via the setup-configuration API) - not tested.
- Whether `rexglue codegen` auto-detects all 331 jump tables / needs `[functions]` entries for this XEX - only a spike answers it.
- Real cold build times (SDK and 95 MB generated code) - my numbers are guesses.
- Behaviour with the non-ASCII user path in clang/ReXGlue (CVar `user_data_root` and ASCII junctions are the workaround).
- Xenia Canary's behaviour on this Iris Xe in-game (the log ends at the movie loader); performance of ROV vs RTV there.
- Whether ReXGlue's Xenia snapshot has every canary fix that this game needs (e.g. `window_offset_in_edram`, `depth_bias_shader_offset`).
- Whether mainline FFmpeg's `xma1/xma2` decoders could serve as a shortcut for XMA music in the own runtime (not checked; Xenia/ReXGlue use the
  custom `xmaframes` decoder because the hardware context semantics need frame-level control).

---------------------------------------------------------------------------------------------------------------------------

## 8. Sources (all fetched 2026-10-05)
ReXGlue: https://github.com/rexglue/rexglue-sdk (README, `CMakeLists.txt`, `CMakePresets.json`, `.gitmodules`, `LICENSE`,
`.github/workflows/_build-platform.yaml`, `include/rex/ppc/func.h`, `include/rex/system/gpu_plugin.h`, `src/graphics/plugin_main.cpp`,
`src/audio/xma_*.cpp`, `thirdparty/ffmpeg-overlay/codec_list.c`); GitHub API for repo/releases/contents/issues/search.
Wiki: https://github.com/rexglue/rexglue-sdk/wiki (Getting-Started, rexglue-CLI-Commands, rexglue-CLI-Configuration-File,
Codegen-Pipeline-Overview, Generated-Code-Structure, Runtime-Architecture-Overview, ReXApp, Virtual-File-System, Function-Overrides,
SDK-Install-Modes, Mid-ASM-Hooks, CVar-System, Memory).
Issues/PRs cited: #92 #162 #171 #187 #227 #280 #283 #290 #333 #341 #355 #359 #361 #363 #365 #370 #372 #375 #382 #395 #402 #403 #404 #407 #409
#412 #414 #428 #430 #435 #437 #441 #446 #447 #452 #453 #456 #459 #460 #461 #475 #476 #480 (all in rexglue/rexglue-sdk).
Third party: https://readonlymemo.com/rexglue-xbox-360-recompilation-interview/ ; https://github.com/abduznik/awesome-x360-recomps ;
https://github.com/sp00nznet/360tools ; https://github.com/beatrixzy/PGR4-Recomp (+ Setup.md) ; https://github.com/mchughalex/skate3recomp ;
https://github.com/zolaware/reblue ; https://github.com/Fable2Recomp/Fable2Recomp ; https://heldgames.com/guides/xbox-360-recompilation-rexglue ;
https://abit.ee/en/games/rexglue-xbox-360-static-recompilation-pc-port-powerpc-c-xenia-blue-dragon-en
Xenia: https://github.com/xenia-project/game-compatibility/issues/651 ; https://github.com/xenia-canary/game-compatibility/issues/411 ;
https://github.com/xenia-canary/game-patches/blob/main/patches/555307EE%20-%20Import%20Tuner%20Challange.patch.toml
Local: `<root>\tools\xenia_canary_windows\{xenia-canary.config.toml,xenia.log,recent.toml}`, `<root>\reference\xenia-gpu\...`,
`work\NOTES.md`, `work\notes_cube.md`, `work\itc.toml`, `work\jump_tables.toml`, `work\image.bin`, `tools\XenonRecomp\XenonUtils\ppc_context.h`,
`tools\XenonRecomp\XenonAnalyse\main.cpp`, vswhere / where.exe / WMI queries (read-only).
