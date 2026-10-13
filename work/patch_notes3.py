p = 'C:/itc-work/NOTES.md'
s = open(p, encoding='utf-8').read()


def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)


rep('Last updated: 2026-10-05 (late evening: performance work, race ~14 fps -> ~28 fps in the scripted test).',
    'Last updated: 2026-10-05 (night: performance work, race ~14 fps -> steady 30 fps in the scripted test; 60 fps blocked by the game logic).', 'date')

rep('''- Keyboard (kernel_xam.cpp): W = accelerator''', '''- **Second round (19:00-19:15)**, all with env switches for A/B: a 32 MB dynamic **vertex ring** (`VbRingCopy`: data drawn in fewer than 3 frames (big ranges) / 30 frames
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
- **60 fps**: `ITC_VBLANK_HZ=120` gives 45-58 fps but the game runs at double speed (per-frame fixed timestep; the intro movie too). A research note on the game's pacing
  (vblank handler, presentation interval, timestep constants, the 0x8218E4EC patch) is being written to `notes_60fps.md`; the thread-split / write-tracking design (not
  implemented, CP is already at 10 ms/frame) is in `notes_renderer_design.md`.
- Helpers: `fpsum.py <tag> [from_frame]` (race fps per 2 s window + mean from a run log), `blackcheck.py <tag>` (dark frames in dumps), `hlsltime.py` (FXC compile times per dumped
  shader and flag set), `play.bat` (starts the game for the user with logging + black-screen detector; the scripted harness kills every itc.exe, including the user's).
- Keyboard (kernel_xam.cpp): W = accelerator''', 'second round')
open(p, 'w', encoding='utf-8').write(s)
print('ok')
