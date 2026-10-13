// XAudio render driver: the title registers a callback that the "audio system" must invoke once per audio
// frame (256 samples @ 48 kHz = 5.33 ms) to request more samples. For now the samples are discarded
// (no sound), but the callback must still fire or the game's audio threads - and everything waiting on them - stall.
#include "guest.h"
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <cmath>
#include <algorithm>


struct AudioClient { uint32_t callback = 0, arg = 0; std::atomic<bool> active{false}; };
static AudioClient g_clients[8];
static std::atomic<bool> g_audioThreadStarted{false};
static bool g_audioLog = getenv("ITC_AUDIO_LOG") != nullptr;

static DWORD WINAPI AudioThread(LPVOID)
{
    ProfNameThread("audio");
    auto* ctx = new PPCContext;
    InitGuestThreadContext(*ctx, 0x40000);
    LARGE_INTEGER freq, next, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    const LONGLONG period = freq.QuadPart * 256 / 48000;
    uint64_t frames = 0;
    for (;;)
    {
        QueryPerformanceCounter(&now);
        if (now.QuadPart < next.QuadPart) { Sleep(1); continue; }
        next.QuadPart += period;
        if (now.QuadPart - next.QuadPart > period * 8) next = now;   // don't try to catch up after a stall
        for (auto& c : g_clients)
        {
            if (!c.active) continue;
            if (g_audioLog && frames < 3) Log("[audio] calling frame callback %08X(%08X)", c.callback, c.arg);
            ctx->r3.u64 = c.arg;
            PPCFunc* fn = *(PPCFunc**)GP(PPC_IMAGE_BASE + PPC_IMAGE_SIZE + uint32_t((c.callback - PPC_CODE_BASE) * 2));
            fn(*ctx, g_base);
            if (g_audioLog && frames < 3) Log("[audio] frame callback %08X(%08X) returned", c.callback, c.arg);
        }
        frames++;
    }
}

HOST_FUNC(XAudioRegisterRenderDriverClient)
{
    // (struct { callback, arg }*, DWORD* outDriver)
    const uint32_t cb = rd32(ctx.r3.u32), arg = rd32(ctx.r3.u32 + 4);
    for (uint32_t i = 0; i < 8; i++)
    {
        if (g_clients[i].active) continue;
        g_clients[i].callback = cb; g_clients[i].arg = arg; g_clients[i].active = true;
        wr32(ctx.r4.u32, 0x41550000u | (i + 1));
        Log("[audio] render driver client %u: callback=%08X arg=%08X", i, cb, arg);
        if (!g_audioThreadStarted.exchange(true))
            CreateThread(nullptr, 64ull << 20, AudioThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        ctx.r3.u64 = 0;
        return;
    }
    ctx.r3.u64 = 0x80070057;
}

HOST_FUNC(XAudioUnregisterRenderDriverClient)
{
    const uint32_t i = (ctx.r3.u32 & 0xFFFF) - 1;
    if (i < 8) g_clients[i].active = false;
    ctx.r3.u64 = 0;
}

// --- PCM output (waveOut): the title mixes 256-sample frames of 6-channel big-endian floats (PLANAR: six planes of 256 samples, as in
// UnleashedRecomp's XAudioSubmitRenderDriverFrame); they are downmixed to stereo -----------------
namespace {
constexpr int kOutBuffers = 12, kOutFrames = 1024;   // 1024 frames = 21.3 ms per waveOut buffer
HWAVEOUT g_wo = nullptr;
WAVEHDR g_hdr[kOutBuffers];
int16_t g_pcm[kOutBuffers][kOutFrames * 2];
int g_curBuf = 0, g_fillFrames = 0;
bool g_outInit = false, g_outFailed = false;
uint64_t g_submitted = 0, g_dropped = 0;

void AudioOutInit()
{
    if (g_outInit || g_outFailed) return;
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = 48000; wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4; wf.nAvgBytesPerSec = 48000 * 4;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { g_outFailed = true; Log("[audio] waveOutOpen failed: no sound output"); return; }
    for (int i = 0; i < kOutBuffers; i++)
    {
        memset(&g_hdr[i], 0, sizeof g_hdr[i]);
        g_hdr[i].lpData = (LPSTR)g_pcm[i]; g_hdr[i].dwBufferLength = kOutFrames * 4;
        waveOutPrepareHeader(g_wo, &g_hdr[i], sizeof g_hdr[i]);
        g_hdr[i].dwFlags |= WHDR_DONE;   // free
    }
    g_outInit = true;
    Log("[audio] waveOut opened (48 kHz stereo)");
}

void AudioOutSubmitFrame(uint32_t samplesPtr)
{
    AudioOutInit();
    if (!g_outInit) return;
    WAVEHDR& h = g_hdr[g_curBuf];
    if (g_fillFrames == 0 && !(h.dwFlags & WHDR_DONE)) { g_dropped++; return; }   // device is behind: drop this frame instead of blocking the mixer
    int16_t* out = g_pcm[g_curBuf] + g_fillFrames * 2;
    double peak = 0;
    for (int i = 0; i < 256; i++)
    {
        float ch[6];
        for (int c = 0; c < 6; c++) { uint32_t raw = rd32(samplesPtr + (c * 256 + i) * 4); memcpy(&ch[c], &raw, 4); }
        float l = ch[0] + 0.7071f * ch[2] + 0.7071f * ch[4] + 0.5f * ch[3];
        float r = ch[1] + 0.7071f * ch[2] + 0.7071f * ch[5] + 0.5f * ch[3];
        if (!(l == l)) l = 0; if (!(r == r)) r = 0;
        peak = std::max(peak, (double)std::max(std::fabs(l), std::fabs(r)));
        // Soft limiter: the 5.1 -> stereo sum of music + engine + effects can exceed full scale (hard clipping was audible as distortion in the race): above 0.7 the level is compressed smoothly towards 1.0.
        auto soft = [](float x) { const float t = 0.7f, a = std::fabs(x); if (a <= t) return x; const float y = t + (1.0f - t) * std::tanh((a - t) / (1.0f - t)); return x < 0 ? -y : y; };
        l = soft(l); r = soft(r);
        out[i * 2] = (int16_t)(l * 32767.0f); out[i * 2 + 1] = (int16_t)(r * 32767.0f);
    }
    g_submitted++;
    if (g_audioLog && peak > 0.0001) { static int n = 0; if (n++ < 20) Log("[audio] frame %llu peak=%.4f", (unsigned long long)g_submitted, peak); }
    g_fillFrames += 256;
    if (g_fillFrames >= kOutFrames)
    {
        // Debug: ITC_AUDIO_DUMP=<file.wav> also writes everything that goes to the sound card (header sizes are refreshed after every buffer: the process is killed, not closed).
        static FILE* dump = nullptr; static bool dumpTried = false; static uint32_t dumpBytes = 0;
        if (!dumpTried) { dumpTried = true; if (const char* dp = getenv("ITC_AUDIO_DUMP")) dump = fopen(dp, "wb"); if (dump) { const uint8_t hdr[44] = { 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,0x80,0xBB,0,0,0,0xEE,2,0,4,0,16,0,'d','a','t','a',0,0,0,0 }; fwrite(hdr, 1, 44, dump); } }
        if (dump)
        {
            fwrite(g_pcm[g_curBuf], 1, kOutFrames * 4, dump); dumpBytes += kOutFrames * 4;
            const uint32_t riff = 36 + dumpBytes; fseek(dump, 4, SEEK_SET); fwrite(&riff, 4, 1, dump); fseek(dump, 40, SEEK_SET); fwrite(&dumpBytes, 4, 1, dump); fseek(dump, 0, SEEK_END); fflush(dump);
        }
        h.dwFlags &= ~WHDR_DONE;
        waveOutWrite(g_wo, &h, sizeof h);
        g_curBuf = (g_curBuf + 1) % kOutBuffers;
        g_fillFrames = 0;
    }
}
}  // namespace

HOST_FUNC(XAudioSubmitRenderDriverFrame)
{
    // (driver, float samples[256][6] big-endian)
    if (ctx.r4.u32) AudioOutSubmitFrame(ctx.r4.u32);
    ctx.r3.u64 = 0;
}

HOST_FUNC(XAudioGetVoiceCategoryVolume) { ctx.r3.u64 = 0; }
HOST_FUNC(XAudioGetVoiceCategoryVolumeChangeMask) { if (ctx.r4.u32) wr32(ctx.r4.u32, 0); ctx.r3.u64 = 0; }

// The XMA decoder lives in xma.cpp.
