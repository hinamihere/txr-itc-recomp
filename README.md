# Tokyo Xtreme Racer: Import Tuner Challenge — PC port (static recompilation)

An experimental native Windows port of the Xbox 360 racing game **Tokyo Xtreme Racer: Import Tuner Challenge** (Genki / Ubisoft, 2006).
The game's PowerPC code is translated to C++ ahead of time with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), and a hand-written runtime
emulates the parts of the Xbox 360 the game talks to: the kernel and XAM system calls, threads, file I/O, the Xenos GPU (with a Direct3D 11 and a new
Vulkan renderer), XMA audio and controller input.

> **Honest disclaimer.** This was built largely with AI assistance (Claude), directed and tested by me. It is **not** polished and **not** close to
> a finished port: expect bugs, missing features and rough code. I think it is a good start, and it shows that this game can run natively on PC.

**No game files are included.** You need your own legally obtained copy of the game (Xbox 360 disc image) to use this.

## What works
- Boots to the title screen and menus; **Free Run mode is playable** from car select to driving on the Shuto Expressway.
- Rendering through Direct3D 11 (default) at a steady 30 fps on a laptop iGPU (Intel Iris Xe), or through Vulkan (`ITC_RENDERER=vulkan`, newer, less tested).
- Sound effects and music (XMA2 decoded through FFmpeg).
- Keyboard and XInput controllers.

## What doesn't (yet)
- **Quest Mode** goes to a black screen after buying the first car.
- Vulkan renderer: occasional black triangles / surfaces flashing in races.
- No saves/profiles, no video playback (intro movies), no rumble. 30 fps is the game's own cap (its logic runs one tick per frame).
- Plenty of smaller rendering inaccuracies; see `work/NOTES.md` for the full list.

## Building (Windows)
Requirements: MinGW-w64 GCC (tested with GCC 16), CMake + Ninja, Python 3, and:
- **XenonRecomp** built from the commit named in `patches/README.md` with `patches/xenonrecomp-local.patch` applied.
- Your game files extracted to `work/extracted/` (`default.xex` + `DATA/`; `work/xdvdfs_extract.py` can extract an ISO).
- Optional: FFmpeg shared DLLs (`avcodec`/`avutil`) next to `itc.exe` for music; [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) and the
  [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler/releases) in `work/deps/` for the Vulkan renderer.

GCC cannot handle non-ASCII characters in paths, so put (or junction) the project on an ASCII path. Then roughly:
```bash
cd work
bin/XenonRecomp.exe itc.toml <XenonRecomp>/XenonUtils/ppc_context.h   # generates out/ppc_recomp.*.cpp
python patch_mftb.py                                                   # time-base fix for the generated code
cmake -S runtime -B rt_build -G Ninja && ./build.sh                    # itc.exe + itc_host.dll
```
The scripts currently assume the paths of the original machine (`C:\itc-work`, `C:\itc-xr`); `AGENTS.md` describes the setup in detail.

## Running
`work/play.bat` (Direct3D 11) or `work/play_vk.bat` (Vulkan). Controls: arrows = D-pad, Enter = Start, Space/Z = A, X = B, W/S = throttle/brake,
A/D = steer, Q/E = LB/RB, F3 = FPS overlay. Many `ITC_*` environment variables enable logs and debug features (see `work/NOTES.md`).

## Plans
- Fix Quest Mode and the remaining rendering bugs; make Vulkan the default renderer.
- Saves, movies, a resizable / fullscreen window.
- **Mod support and plugins** (hooking game functions is straightforward in a recompilation).
- **Custom multiplayer.**
- Longer term: new content such as a touge (mountain pass) mode, maybe custom tracks and cars.

## Repository layout
- `work/runtime/` — the hand-written runtime (`itc_host.dll`) and build files.
- `work/itc.toml`, `work/jump_tables.toml` — recompiler configuration.
- `work/*.py`, `*.ps1`, `*.bat` — build, test and debugging tools.
- `work/NOTES.md`, `work/notes_*.md` — detailed engineering notes.
- `AGENTS.md` / `CLAUDE.md` — orientation for AI coding agents working on the project.
- `patches/` — local changes to XenonRecomp.

## Credits
XenonRecomp and XenosRecomp (hedge-dev), Xenia (GPU register and microcode documentation), FFmpeg (XMA2 decoding), RenderDoc.
See `THIRD_PARTY_NOTICES.md`. This project is not affiliated with Genki, Ubisoft or Microsoft.
