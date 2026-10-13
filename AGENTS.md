# AGENTS.md - Tokyo Xtreme Racer: Import Tuner Challenge PC port (static recompilation)

Read this first. It tells a new agent what the project is, how to build/run/test it, where things are, and what is going on right now.
Deeper history and findings are in `work/NOTES.md` (long, detailed; read the sections you need).

## What this is
- A native Windows port of the Xbox 360 game **Tokyo Xtreme Racer: Import Tuner Challenge** (Genki/Ubisoft, 2006), made by **static recompilation**:
  XenonRecomp turns the game's PowerPC code (`default.xex`) into C++ (`work/out/ppc_recomp.*.cpp`), and a hand-written runtime emulates everything
  around it: Xbox kernel/XAM imports, threads/sync, file I/O, the Xenos GPU (PM4 command processor + renderer), XMA audio, input.
- The game files come from the user's own ISO (`work/extracted/`). **Never distribute or upload game data** (ISO, xex, extracted files, dumps of game assets).
- The project was built with heavy AI assistance. The user owns it, tests it themselves, and gives agents a lot of autonomy.

## The user and how to work with them
- They test the game themselves and report what they see ("black triangles", "plays way better"). Treat those reports as the source of truth.
- They want honest status: say what works, what doesn't, what was verified and what wasn't. Don't overstate.
- Downloads need their explicit OK (state file, source, size). FFmpeg, Vulkan-Headers and DXC were approved and are already in place.
- Popup questions (AskUserQuestion) are welcome for real decisions. Short, plain answers; no jargon walls.
- Test runs of the game are allowed whenever needed, but the test harness kills **every** running `itc.exe`, including a window the user is
  playing in. Check `Get-Process itc` first if they might be playing.
- Long-term wishes (not started): mod/plugin support, custom multiplayer, a touge (mountain pass) mode, maybe Assetto Corsa track/car content.

## Paths (important)
- Project root: `C:\Users\日南\Documents\txr-itc-recomp`. The user name contains non-ASCII characters and **GCC/CMake cannot handle that path**.
- Always build and run through the ASCII junctions:
  - `C:\itc-work` -> `<root>\work` (everything active lives here)
  - `C:\itc-xr`  -> `<root>\tools\XenonRecomp`
- Root-level `build/`, `runtime/`, `src/` are empty leftovers; `recomp/` and `docs/` are early recompiler experiments; `reference/` holds Xenia GPU sources
  for reference; `tools/` has XenonRecomp, XenonAnalyse, UnleashedRecomp (XenosRecomp shader reference), Xenia Canary (reference emulator), Ghidra, XexTool.

## Layout of `C:\itc-work`
| Path | What |
|---|---|
| `itc.toml`, `jump_tables.toml` | XenonRecomp config (function boundaries, jump tables, midasm hooks) |
| `out/ppc_recomp.*.cpp` | generated game code (do not edit by hand; regenerate) |
| `runtime/CMakeLists.txt` | builds `itc.exe` (generated code) and `itc_host.dll` (all hand-written code) |
| `runtime/src/main.cpp` | loader (xex parse, imports, function table), crash handler, watchdog, debug probes |
| `runtime/src/kernel*.cpp` | kernel/XAM HLE: memory, threads, sync, I/O, input (`kernel_xam.cpp`: keyboard/XInput/pad scripts), audio |
| `runtime/src/stubs.cpp` | generated logging stubs for unimplemented imports (`python runtime/gen_stubs.py`) |
| `runtime/src/gpu.cpp` | PM4 command processor thread, vblank/interrupt thread, VdSwap |
| `runtime/src/xgpu_shader.cpp` | Xenos shader microcode -> HLSL (D3D11 SM5, or Vulkan mode for DXC -> SPIR-V) |
| `runtime/src/xgpu_common.cpp` | register file, game window, scripted-input thumbnails, **backend dispatch** |
| `runtime/src/xgpu_shared.inl` | API-independent GPU code shared by both backends (texture decode, tiling, index conversion, constants) |
| `runtime/src/xgpu_d3d.cpp` | Direct3D 11 renderer (default, most tested) |
| `runtime/src/xgpu_vk.cpp` | Vulkan renderer (`ITC_RENDERER=vulkan`, new) |
| `runtime/src/xma.cpp`, `kernel_audio.cpp` | XMA2 decode through FFmpeg (dynamically loaded), XAudio render driver -> waveOut |
| `rt_build/` | build dir: `itc.exe`, `itc_host.dll`, FFmpeg DLLs |
| `deps/` | Vulkan-Headers 1.3.290, DXC (dxcompiler.dll + dxil.dll) |
| `extracted/` | game files from the ISO |
| `shader_cache*/`, `vk_pipeline_cache.bin` | compiled shader caches (safe to delete; rebuilt on demand) |
| `dumps/`, `*.err`, `*.out` | frame dumps and logs from test runs (scratch) |
| `NOTES.md` + `notes_*.md` | detailed engineering notes and research |

## Build
```bash
cd /c/itc-work && ./build.sh          # host DLL (+ exe only if generated code changed); seconds for the DLL
```
- Toolchain: MinGW-w64 GCC 16 (`C:\mingw64\bin`), CMake + Ninja, build dir `rt_build`.
- A running `itc.exe` locks `rt_build/itc_host.dll`: close/kill it first (PowerShell `(Get-Process itc).Kill()`), or rename the locked DLL.
- Regenerating the game code (after changing `itc.toml`/`jump_tables.toml`): `bin/XenonRecomp.exe itc.toml ../tools/XenonRecomp/XenonUtils/ppc_context.h`,
  then **`python patch_mftb.py`** (fixes the time base, see NOTES "Audio"), then `./build.sh` (several minutes).
- After adding kernel import implementations: `python runtime/gen_stubs.py`.

## Run
- User launchers in `C:\itc-work`: `play.bat` (D3D11, log `play.err`), `play_vk.bat` (Vulkan), `play_vk_safe.bat` (Vulkan, all perf shortcuts off),
  `play_prof.bat` (sampling profiler).
- Controls: arrows = d-pad, Enter = Start, Space/Z = A, X = B, W/S = throttle/brake, A/D = steer, Q/E = LB/RB; XInput pads work. F3 toggles the FPS overlay.
- Scripted test runs (agents):
  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File C:\itc-work\run_test.ps1 -Tag t1 -Seconds 100 -Pad "<script>" -EnvList 'ITC_NO_XINPUT=1;ITC_RENDERER=vulkan'
  ```
  It kills every `itc.exe`, runs the game, kills it after `-Seconds`, logs to `t1.err`. **Always pass `ITC_NO_XINPUT=1`** for scripted runs, otherwise the user's pad
  can interfere. The keyboard is only read while the window is focused.
  Pad script syntax: `frame:BUTTON:frames,...`, `WAIT:title` waits for `refs/title.thumb`. The full Free Run flow is in `padscript4.txt`
  (append `,1800:RT:900` to drive in the race; ~2000 frames to reach the race).
- Frame dumps: `ITC_DUMP_FROM=<frame>;ITC_DUMP_EVERY=<n>;ITC_DUMP_MAX=<n>` -> `dumps/frame<tag>_NNNNN.png`; `python sheet.py <tag> 6 320` makes a contact sheet.
- `runab.ps1 -Tag x -Env '...'` = race run with dumps + `blackpix.py` detector for the flat-dark-surface glitch.

## Useful env vars
`ITC_RENDERER=vulkan`, `ITC_STATUS=1` ([stat] lines every 2 s), `ITC_PROF=1`, `ITC_WATCHDOG=<s>` (+`ITC_PROBE=1`), `ITC_GPU_LOG=1|2`, `ITC_IO_LOG=1`,
`ITC_AUDIO_LOG=1`, `ITC_NO_PERF=1` (disable all draw-path optimisations), `ITC_SYNC_SHADERS=1`, `ITC_VK_SYNC=1` (Vulkan: no CPU/GPU overlap),
`ITC_VK_VALIDATION=1` (needs the Vulkan SDK layer, not installed), `ITC_LOG_FRAME=N`, `ITC_DUMP_SHADERS=1`. Full list: NOTES.md "Debug env vars".

## How the runtime works (short)
- The game thinks it runs on an Xbox 360: 4 GB guest address space, 512 MB physical memory mirrored at 0xA0000000/0xC0000000/0xE0000000, big-endian data.
- GPU: the game writes PM4 packets to a ring; `gpu.cpp` parses them on a command-processor thread, keeps all registers in `g_xregs`, and calls the backend
  for draws, resolves and swaps. Shaders are translated from Xenos microcode to HLSL at first use, compiled on worker threads and cached on disk.
  Render targets are host textures keyed by EDRAM address; resolves stay on the GPU and are written back to guest memory only when the CPU needs them.
- The game logic runs a fixed 30 Hz tick per frame: **30 fps is the cap** (60 fps would run the game at double speed; see `notes_60fps.md`).

## Current status (2026-10-13)
- Free Run is playable start to finish with D3D11: correct rendering, steady 30 fps in races, sound effects and XMA music, keyboard + pad.
- **Vulkan renderer** (`xgpu_vk.cpp`, started 2026-10-08): menus and races render like D3D11 at ~30 fps on the dev machine (Intel Iris Xe, Vulkan 1.2).
  The user says the normal Vulkan build plays "way better" than the safe one. D3D11 stays the default until Vulkan is proven.
- **Quest Mode**: permanent black screen right after buying the first car (game keeps running at 30 fps, draws ~59 draws/frame of black, repeated BUILD.DAT reads).
  Unsolved; investigation was paused for the Vulkan work. Probe code: `main.cpp` `ProbeLoading()` (`ITC_PROBE=1` + `ITC_WATCHDOG`).

## Open work, in priority order
1. **Vulkan: black triangles / surfaces flashing in races** (user report). One capture showed a surface flat dark grey (13,13,13) for a single frame; rare in
   scripted runs, not reproduced with `ITC_VK_SYNC=1`. Ask the user which of `play_vk.bat` / `play_vk_safe.bat` / `play.bat` shows it (bisects shared code vs
   Vulkan vs the perf shortcuts). Suspects: stale vertex pages (validation every 8 frames), frame-overlap synchronisation, shadow-map resolve.
2. Vulkan performance: replace the conservative ALL_COMMANDS barriers, background pipeline compilation, then make Vulkan the default.
3. Quest Mode black screen after the car purchase (see NOTES.md and the probe above). Candidates: loading state machine `sub_8218E420`, save/profile/content
   stubs (XamContent*), movie playback (WMV/XMV is not emulated).
4. Late-session slowdown to 8-14 fps (guest-side, cause unknown); mip chains / texture sign bits; saves; rumble; movies.

## Gotchas
- Bash tool: heredocs with many quotes/backticks can break, and `\t` inside heredocs turns into real tabs. Write patch scripts with the Write tool and run them with python.
- PowerShell here is 5.1: no `&&`; use `;` or `if ($?) {}`.
- A killed game can leave a stuck `itc.exe` holding the DLL; kill with `(Get-Process itc).Kill()`.
- Long commands (>2 min) belong in the background; builds after a regeneration take minutes.
- Frame numbers are not stable between runs (loading times vary); check what a dump shows before trusting a frame number.
- `__imp__<Name>` in the runtime overrides kernel import `<Name>`; game functions cannot be overridden from the DLL (use a `[[midasm_hook]]` in itc.toml).
- D3D11 ignores invalid calls silently (no debug layer). Vulkan has no validation layer installed either.
- `XenonRecomp` emitted `mftb` as `__rdtsc()`; `patch_mftb.py` must be re-run after every regeneration or timeouts in the game break.
- DXC from MinGW: `__declspec(uuid)` is ignored, so `xgpu_vk.cpp` declares the interface IDs with `__CRT_UUID_DECL`. DXC needs `-HV 2018` for the translated HLSL.

## Conventions
- Match the surrounding code: long explanatory comments on *why*, compact C++, env-var switches for experiments (`ITC_*`), logs with a `[tag]` prefix.
- Keep D3D11 working while changing shared code (`xgpu_shared.inl`, `xgpu_shader.cpp`); test both backends.
- Record non-obvious findings in `work/NOTES.md` (append a dated section) so the next agent does not re-investigate them.
- Version control: a **git repository at the project root**, remote `origin` = the **public** GitHub repo https://github.com/hinamihere/txr-itc-recomp
  (branch `master`). Push only when the user asks; never commit game data (anything is public once pushed). It tracks code, configs,
  scripts and notes only; `.gitignore` keeps out game data, generated code (`work/out/`), builds, caches, dumps and logs. Commit finished, working steps
  with a clear message (what changed and why). Local changes to XenonRecomp live in `patches/xenonrecomp-local.patch` (see `patches/README.md`).
