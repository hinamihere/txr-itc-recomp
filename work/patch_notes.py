p = 'C:/itc-work/NOTES.md'
s = open(p, encoding='utf-8').read()


def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)


rep('Last updated: 2026-10-05 (evening, after the "dark world" fixes).', 'Last updated: 2026-10-05 (night, race view renders correctly).', 'date')

# STATUS block
start = s.index('**The in-game world now renders**')
end = s.index('Other open items:', start)
end2 = s.index('\n', end) + 1
new_status = '''**The race view now renders correctly** (RX-7/RX-8 from behind, sodium-lit asphalt and walls, lit buildings, signs, sky, headlights, cube-map reflections).
The last big fixes were: rect-list quads (the GS finds the right-angle corner: clears were half-drawn), vertex buffer size rounded UP, colour RT format 12 aliased to 3,
three shaders that failed to translate/compile, and **FXC's optimiser miscompiling the big lighting shaders** (now compiled with SKIP_OPTIMIZATION, see Findings).
Open items: mip chains (the far road shimmers), XMA audio (music), saves, per-draw CPU cost (see the profile below), TextureSign bits.
'''
s = s[:start] + new_status + s[end2:]

# Findings + RenderDoc workflow, inserted before the next-steps list
findings = r'''- **FXC miscompile**: with the default optimiser the big pixel shaders (9473127d, 0bbc5532, d96c4660: pc-state-machine + 16 shadow taps) returned stale registers (the world
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

'''
rep('## Known problems / next steps (in order)', findings + '## Known problems / next steps (in order)', 'findings')

# replace the old numbered list
start = s.index('## Known problems / next steps (in order)')
end = s.index('## Gotchas', start)
new_list = '''## Known problems / next steps (in order)
1. Mip chains (textures are decoded base level only; the far road shimmers). Cube map mips are not needed (base-map filter), see `notes_cube.md`.
2. Texture fetch-constant sign bits (unsigned/signed/biased/gamma) are ignored (all textures seen so far are unsigned); exp_adjust is applied; MSAA edges; depth bias for decals.
3. XMA audio decode (music is silent), saves/profile, rumble. Check the post-race black screen (see the community patch above).
4. Per-draw CPU cost: cache converted index buffers per (address,count,prim), skip texture/VB re-validation when no guest write touched them, fewer constant uploads.
5. `notes_shader_diff.md` (translator vs XenosRecomp comparison by a research agent, if present) may list more instruction-semantics differences (legacy 0*x=0 multiply, etc.).
6. EVENT_WRITE_EXT extents; the generated code is -O1 with trace prologues (rebuild -O2 without ITC_TRACE if guest CPU matters).

'''
s = s[:start] + new_list + s[end:]

s = s.replace('this pass is pure overhead until they are); ', 'the cube research found NO resolve into a cube address: that pass is fetched later as a 2D texture, so it must NOT be skipped); ', 1)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
