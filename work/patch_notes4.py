p = 'C:/itc-work/NOTES.md'
s = open(p, encoding='utf-8').read()


def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)


rep('''- **60 fps**: `ITC_VBLANK_HZ=120` gives 45-58 fps but the game runs at double speed (per-frame fixed timestep; the intro movie too). A research note on the game's pacing
  (vblank handler, presentation interval, timestep constants, the 0x8218E4EC patch) is being written to `notes_60fps.md`; the thread-split / write-tracking design (not
  implemented, CP is already at 10 ms/frame) is in `notes_renderer_design.md`.''',
    '''- **60 fps verdict** (`notes_60fps.md`, research agent): the game waits for 2 vblank callbacks per frame (`li r6,2` at 0x820923D4 -> [gfx+304]; its own vblank callback
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
- Game code at **-O2** (runtime/CMakeLists.txt; was -O1 from bring-up; the -O1 exe is kept as `rt_build/itc_O1_backup.exe`): built 2026-10-05 ~19:45 to cut guest-side frame time
  (the remaining 50 ms frames show the CP idle for ~15 ms = the guest submitted late).''', '60 fps verdict')
open(p, 'w', encoding='utf-8').write(s)
print('ok')
