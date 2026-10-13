p = 'C:/itc-work/NOTES.md'
s = open(p, encoding='utf-8').read()


def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)


rep('''## Known problems / next steps (in order)''', '''## Audio (2026-10-05 night)
- SFX path: `XAudioRegisterRenderDriverClient` callback every 256 samples; `XAudioSubmitRenderDriverFrame` gets 6 PLANAR channels of 256 big-endian floats (as in
  UnleashedRecomp's audio.cpp; it was read as interleaved until 19:50 - SFX were scrambled), downmixed to stereo 16-bit into waveOut (kernel_audio.cpp).
- Music = XMA2 streams through hardware decoder contexts (`XMACreateContext`, 64-byte XMA_CONTEXT_DATA blocks, layout from Xenia's xma_context.h; the game kicks via the
  0x7FEA0000 MMIO window which is plain memory here, so `xma.cpp` polls the contexts every 2 ms). `xma.cpp` implements the decoder with FFmpeg's libavcodec XMA2 decoder
  loaded at runtime from `avcodec-*.dll` / `avutil-*.dll` placed next to `rt_build/itc.exe` (headers expected in `work/ffmpeg/include`, see runtime/CMakeLists.txt; without
  them the file compiles to the old silent stub). Whole 2 KB packets are fed in order (libavcodec keeps frames straddling packets), decoded 512-sample frames are written as
  big-endian int16 (interleaved for stereo) into the 256-byte-block output ring (write/read offsets in blocks; full = write catches read -> output_buffer_valid cleared);
  input buffers are consumed at packet granularity (valid bit cleared, current_buffer toggled, read offset = 32 bits), loops at packet granularity. Untested until the FFmpeg
  DLLs are present (download needs the user's OK). `ITC_NO_XMA=1` disables decoding, `ITC_AUDIO_LOG=1` logs context activity.
- Known timing issue: `mftb` -> `__rdtsc()` vs KeQueryPerformanceFrequency = 50 MHz (see the 60 fps note): the game's XMA wait loop (sub_8245D288) times out ~60x too early -
  check this first if music starts but stutters/stops.

## Known problems / next steps (in order)''', 'audio section')
open(p, 'w', encoding='utf-8').write(s)
print('ok')
