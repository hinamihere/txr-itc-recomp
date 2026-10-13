// XMA2 hardware decoder emulation: the title fills 64-byte context blocks (XMA_CONTEXT_DATA, layout from Xenia) with two input buffers of 2 KB XMA packets
// and an output ring of 256-byte blocks of 16-bit big-endian PCM; a thread decodes with FFmpeg's XMA2 decoder (libavcodec, loaded dynamically from avcodec-*.dll
// next to the executable; without it the inputs are consumed silently as before). Whole packets are fed to the decoder in order (it keeps frames that straddle
// packets); the bit read offset / loop points are honoured at packet granularity.
#include "guest.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if __has_include(<libavcodec/avcodec.h>)
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#define ITC_HAVE_FFMPEG 1
#else
#define ITC_HAVE_FFMPEG 0
#endif

uint32_t PhysToVirtAddr(uint32_t phys);

namespace {
constexpr uint32_t kBytesPerPacket = 2048, kBitsPerPacket = 16384, kBitsPerPacketHeader = 32, kSamplesPerFrame = 512, kOutputBlockBytes = 256;
const bool g_xmaLog = getenv("ITC_AUDIO_LOG") != nullptr;
const bool g_noXma = getenv("ITC_NO_XMA") != nullptr;

// Host-order view of a context (the guest block is big-endian dwords).
struct Ctx
{
    uint32_t d[16];
    uint32_t in0PacketCount() const { return d[0] & 0xFFF; }
    uint32_t loopCount() const { return (d[0] >> 12) & 0xFF; }
    bool in0Valid() const { return (d[0] >> 20) & 1; }
    bool in1Valid() const { return (d[0] >> 21) & 1; }
    uint32_t outBlockCount() const { return (d[0] >> 22) & 31; }
    uint32_t outWrite() const { return (d[0] >> 27) & 31; }
    uint32_t in1PacketCount() const { return d[1] & 0xFFF; }
    // DWORD 1 (LSB first): in1 packet count 12 | loop_subframe_start 2 | end 3 | skip 3 | subframe_decode_count 4 | subframe_skip_count 3 | sample_rate 2 (bits 27-28) | is_stereo 1 (bit 29) | unk 1 | output_buffer_valid 1 (bit 31)
    uint32_t sampleRateId() const { return (d[1] >> 27) & 3; }
    bool stereo() const { return (d[1] >> 29) & 1; }
    bool outValid() const { return (d[1] >> 31) & 1; }
    uint32_t readOffsetBits() const { return d[2] & 0x3FFFFFF; }
    uint32_t loopStart() const { return d[3] & 0x3FFFFFF; }
    uint32_t loopEnd() const { return d[4] & 0x3FFFFFF; }
    uint32_t currentBuffer() const { return (d[4] >> 31) & 1; }
    uint32_t in0Ptr() const { return d[5]; }
    uint32_t in1Ptr() const { return d[6]; }
    uint32_t outPtr() const { return d[7]; }
    uint32_t outRead() const { return d[9] & 31; }

    void setIn0Valid(bool v) { d[0] = (d[0] & ~(1u << 20)) | ((uint32_t)v << 20); }
    void setIn1Valid(bool v) { d[0] = (d[0] & ~(1u << 21)) | ((uint32_t)v << 21); }
    void setOutWrite(uint32_t v) { d[0] = (d[0] & ~(31u << 27)) | ((v & 31) << 27); }
    void setOutValid(bool v) { d[1] = (d[1] & ~(1u << 31)) | ((uint32_t)v << 31); }
    void setReadOffsetBits(uint32_t v) { d[2] = (d[2] & ~0x3FFFFFFu) | (v & 0x3FFFFFF); }
    void setLoopCount(uint32_t v) { d[0] = (d[0] & ~(0xFFu << 12)) | ((v & 0xFF) << 12); }
    void setCurrentBuffer(uint32_t v) { d[4] = (d[4] & ~(1u << 31)) | ((v & 1) << 31); }
};
Ctx LoadCtx(uint32_t p) { Ctx c; for (int i = 0; i < 16; i++) c.d[i] = rd32(p + i * 4); return c; }
void StoreCtx(uint32_t p, const Ctx& c, const Ctx& orig) { for (int i = 0; i < 16; i++) if (c.d[i] != orig.d[i]) wr32(p + i * 4, c.d[i]); }

#if ITC_HAVE_FFMPEG
// libavcodec / libavutil entry points, resolved at runtime.
struct FF
{
    HMODULE codec = nullptr, util = nullptr;
    decltype(&avcodec_find_decoder) find_decoder = nullptr;
    decltype(&avcodec_alloc_context3) alloc_context3 = nullptr;
    decltype(&avcodec_open2) open2 = nullptr;
    decltype(&avcodec_free_context) free_context = nullptr;
    decltype(&avcodec_send_packet) send_packet = nullptr;
    decltype(&avcodec_receive_frame) receive_frame = nullptr;
    decltype(&avcodec_flush_buffers) flush_buffers = nullptr;
    decltype(&av_packet_alloc) packet_alloc = nullptr;
    decltype(&av_packet_free) packet_free = nullptr;
    decltype(&av_frame_alloc) frame_alloc = nullptr;
    decltype(&av_frame_free) frame_free = nullptr;
    decltype(&av_frame_unref) frame_unref = nullptr;
    decltype(&av_malloc) malloc_ = nullptr;
    decltype(&av_channel_layout_default) channel_layout_default = nullptr;
    decltype(&av_log_set_level) log_set_level = nullptr;
    bool ok = false;
};
FF g_ff;

HMODULE LoadDllByPattern(const char* pattern)
{
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, sizeof exe);
    std::string dir = exe; dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    HMODULE m = nullptr;
    do { if (!m) m = LoadLibraryA((dir + fd.cFileName).c_str()); } while (!m && FindNextFileA(h, &fd));
    FindClose(h);
    return m;
}

bool LoadFFmpeg()
{
    static bool tried = false;
    if (tried) return g_ff.ok;
    tried = true;
    g_ff.util = LoadDllByPattern("avutil-*.dll");
    g_ff.codec = LoadDllByPattern("avcodec-*.dll");
    if (!g_ff.util || !g_ff.codec) { Log("[xma] avcodec-*.dll / avutil-*.dll not found next to the executable: XMA music stays silent"); return false; }
    auto get = [](HMODULE m, const char* n) { return GetProcAddress(m, n); };
#define FFGET(mod, field, name) g_ff.field = (decltype(g_ff.field))get(mod, name); if (!g_ff.field) { Log("[xma] missing %s", name); return false; }
    FFGET(g_ff.codec, find_decoder, "avcodec_find_decoder");
    FFGET(g_ff.codec, alloc_context3, "avcodec_alloc_context3");
    FFGET(g_ff.codec, open2, "avcodec_open2");
    FFGET(g_ff.codec, free_context, "avcodec_free_context");
    FFGET(g_ff.codec, send_packet, "avcodec_send_packet");
    FFGET(g_ff.codec, receive_frame, "avcodec_receive_frame");
    FFGET(g_ff.codec, flush_buffers, "avcodec_flush_buffers");
    FFGET(g_ff.codec, packet_alloc, "av_packet_alloc");
    FFGET(g_ff.codec, packet_free, "av_packet_free");
    FFGET(g_ff.util, frame_alloc, "av_frame_alloc");
    FFGET(g_ff.util, frame_free, "av_frame_free");
    FFGET(g_ff.util, frame_unref, "av_frame_unref");
    FFGET(g_ff.util, malloc_, "av_malloc");
    FFGET(g_ff.util, channel_layout_default, "av_channel_layout_default");
    FFGET(g_ff.util, log_set_level, "av_log_set_level");
#undef FFGET
    g_ff.log_set_level(g_xmaLog ? 32 : 8);   // AV_LOG_INFO : AV_LOG_FATAL
    g_ff.ok = true;
    Log("[xma] FFmpeg loaded: XMA2 decoding enabled");
    return true;
}

struct Decoder
{
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    uint32_t sampleRate = 0, channels = 0;
    std::vector<uint8_t> pending;   // decoded big-endian int16 interleaved samples not yet written to the output ring
    size_t pendingPos = 0;
    uint32_t expectedBuffer = 0xFFFFFFFFu, expectedPacket = 0;   // where the next packet is expected (a jump = new stream: flush the decoder)
    uint64_t packets = 0, frames = 0;

    bool Open(uint32_t rate, uint32_t nch)
    {
        if (ctx && sampleRate == rate && channels == nch) return true;
        Close();
        const AVCodec* codec = g_ff.find_decoder(AV_CODEC_ID_XMA2);
        if (!codec) { Log("[xma] libavcodec has no XMA2 decoder"); return false; }
        ctx = g_ff.alloc_context3(codec);
        if (!ctx) return false;
        ctx->sample_rate = (int)rate;
        g_ff.channel_layout_default(&ctx->ch_layout, (int)nch);
        ctx->block_align = kBytesPerPacket;
        // XMA2WAVEFORMATEX extradata (34 bytes): one stream, channel mask
        ctx->extradata = (uint8_t*)g_ff.malloc_(34 + AV_INPUT_BUFFER_PADDING_SIZE);
        memset(ctx->extradata, 0, 34 + AV_INPUT_BUFFER_PADDING_SIZE);
        ctx->extradata[0] = 1; ctx->extradata[2] = nch == 2 ? 3 : 4;
        ctx->extradata_size = 34;
        if (g_ff.open2(ctx, codec, nullptr) < 0) { Log("[xma] avcodec_open2 failed (%u Hz, %u ch)", rate, nch); Close(); return false; }
        pkt = g_ff.packet_alloc(); frame = g_ff.frame_alloc();
        sampleRate = rate; channels = nch;
        return pkt && frame;
    }
    void Close()
    {
        if (pkt) g_ff.packet_free(&pkt);
        if (frame) g_ff.frame_free(&frame);
        if (ctx) g_ff.free_context(&ctx);
        pkt = nullptr; frame = nullptr; ctx = nullptr; sampleRate = channels = 0;
        pending.clear(); pendingPos = 0;
    }
    void Flush() { if (ctx) g_ff.flush_buffers(ctx); pending.clear(); pendingPos = 0; }
    // Feeds one 2 KB packet; decoded samples are appended to `pending` as big-endian int16, interleaved.
    void Feed(const uint8_t* packet)
    {
        std::vector<uint8_t> buf(kBytesPerPacket + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        memcpy(buf.data(), packet, kBytesPerPacket);
        pkt->data = buf.data(); pkt->size = (int)kBytesPerPacket;
        const int sent = g_ff.send_packet(ctx, pkt);
        packets++;
        if (sent < 0) { if (g_xmaLog) { static int n = 0; if (n++ < 20) Log("[xma] send_packet error %d", sent); } return; }
        for (;;)
        {
            const int r = g_ff.receive_frame(ctx, frame);
            if (r < 0) break;
            frames++;
            const int n = frame->nb_samples;
            const size_t base = pending.size();
            pending.resize(base + (size_t)n * channels * 2);
            uint8_t* out = pending.data() + base;
            for (uint32_t c = 0; c < channels; c++)
            {
                const float* in = (const float*)frame->data[c < (uint32_t)frame->ch_layout.nb_channels ? c : 0];
                for (int i = 0; i < n; i++)
                {
                    float v = in[i]; v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
                    const int16_t s = (int16_t)(v * 32767.0f);
                    out[(i * channels + c) * 2] = (uint8_t)((uint16_t)s >> 8); out[(i * channels + c) * 2 + 1] = (uint8_t)s;
                }
            }
            g_ff.frame_unref(frame);
        }
    }
};
#endif

constexpr uint32_t kMaxContexts = 320;
uint32_t g_ctxBase = 0;
std::atomic<uint32_t> g_count{ 0 };
std::atomic<bool> g_threadStarted{ false };
#if ITC_HAVE_FFMPEG
Decoder* g_dec[kMaxContexts] = {};
#endif
uint64_t g_blocksWritten = 0, g_packetsFed = 0;

#if ITC_HAVE_FFMPEG
// One pass over a context: fill the output ring with decoded data, feeding packets as needed.
void WorkContext(uint32_t index, uint32_t p)
{
    const Ctx orig = LoadCtx(p);
    Ctx c = orig;
    if (!c.outValid() || c.outBlockCount() == 0 || c.outPtr() == 0) return;
    if (!c.in0Valid() && !c.in1Valid()) { Decoder* d = g_dec[index]; if (!d || d->pending.size() <= d->pendingPos) return; }
    if (!g_dec[index]) g_dec[index] = new Decoder();
    Decoder& d = *g_dec[index];
    const uint32_t rate = c.sampleRateId() == 0 ? 24000 : c.sampleRateId() == 1 ? 32000 : c.sampleRateId() == 2 ? 44100 : 48000;
    if (!d.Open(rate, c.stereo() ? 2 : 1)) return;

    const uint32_t count = c.outBlockCount();
    uint32_t write = c.outWrite() % count, read = c.outRead() % count;
    uint32_t freeBlocks = (read + count - write) % count;
    if (freeBlocks == 0) freeBlocks = count;   // valid and write == read: empty
    const uint32_t frameBytes = kSamplesPerFrame * 2 * (c.stereo() ? 2 : 1);
    for (int guard = 0; guard < 64 && freeBlocks > 0; guard++)
    {
        // write what is pending
        while (freeBlocks > 0 && d.pending.size() - d.pendingPos >= kOutputBlockBytes)
        {
            memcpy(GP(PhysToVirtAddr(c.outPtr()) + write * kOutputBlockBytes), d.pending.data() + d.pendingPos, kOutputBlockBytes);
            d.pendingPos += kOutputBlockBytes;
            write = (write + 1) % count; freeBlocks--; g_blocksWritten++;
        }
        if (d.pendingPos >= d.pending.size()) { d.pending.clear(); d.pendingPos = 0; }
        if (freeBlocks == 0 || d.pending.size() - d.pendingPos >= kOutputBlockBytes) break;
        // decode another packet
        const uint32_t cur = c.currentBuffer();
        const bool valid = cur ? c.in1Valid() : c.in0Valid();
        if (!valid) break;
        const uint32_t ptr = cur ? c.in1Ptr() : c.in0Ptr(), npk = cur ? c.in1PacketCount() : c.in0PacketCount();
        uint32_t pi = c.readOffsetBits() / kBitsPerPacket;
        if (npk == 0 || ptr == 0) { if (cur) c.setIn1Valid(false); else c.setIn0Valid(false); c.setCurrentBuffer(cur ^ 1); c.setReadOffsetBits(kBitsPerPacketHeader); break; }
        if (pi >= npk) pi = 0;
        if (d.expectedBuffer != ptr || d.expectedPacket != pi) { d.Flush(); }   // a different place in the stream: start clean
        const uint8_t* packet = GP(PhysToVirtAddr(ptr) + pi * kBytesPerPacket);
        d.Feed(packet); g_packetsFed++;
        // advance: next packet, loop, or buffer swap
        uint32_t next = pi + 1;
        const uint32_t nextBits = next * kBitsPerPacket + kBitsPerPacketHeader;
        if (c.loopCount() > 0 && c.loopEnd() > c.loopStart() && nextBits >= c.loopEnd() && c.loopEnd() <= npk * kBitsPerPacket)
        {
            next = c.loopStart() / kBitsPerPacket;
            if (c.loopCount() != 255) c.setLoopCount(c.loopCount() - 1);
            c.setReadOffsetBits(next * kBitsPerPacket + kBitsPerPacketHeader);
            d.expectedBuffer = ptr; d.expectedPacket = next;
            d.Flush();
        }
        else if (next >= npk)
        {
            if (cur) c.setIn1Valid(false); else c.setIn0Valid(false);
            c.setCurrentBuffer(cur ^ 1);
            c.setReadOffsetBits(kBitsPerPacketHeader);
            d.expectedBuffer = cur ? c.in0Ptr() : c.in1Ptr(); d.expectedPacket = 0;
        }
        else
        {
            c.setReadOffsetBits(nextBits);
            d.expectedBuffer = ptr; d.expectedPacket = next;
        }
    }
    c.setOutWrite(write);
    if (write == read && freeBlocks == 0) c.setOutValid(false);   // full: the title reads it out and re-arms the output buffer
    StoreCtx(p, c, orig);
    if (g_xmaLog) { static int n = 0; if (n++ < 40) Log("[xma] ctx %u: write=%u read=%u count=%u free=%u pending=%zu packets=%llu frames=%llu", index, write, read, count, freeBlocks, d.pending.size() - d.pendingPos, (unsigned long long)d.packets, (unsigned long long)d.frames); }
}
#endif

DWORD WINAPI XmaThread(LPVOID)
{
    ProfNameThread("xma");
#if ITC_HAVE_FFMPEG
    const bool decode = !g_noXma && LoadFFmpeg();
#else
    const bool decode = false;
    Log("[xma] built without FFmpeg headers: XMA music stays silent");
#endif
    for (;;)
    {
        Sleep(decode ? 2 : 4);
        const uint32_t n = g_count.load();
        for (uint32_t i = 0; i < n; i++)
        {
            const uint32_t p = g_ctxBase + i * 64;
#if ITC_HAVE_FFMPEG
            if (decode) { WorkContext(i, p); continue; }
#endif
            const uint32_t d0 = rd32(p);
            if (d0 & ((1u << 20) | (1u << 21)))
            {
                // no decoder: consume the input silently (the first contexts are logged in full so the decoder can be checked against real parameters)
                static int logged = 0;
                if (logged++ < 12)
                {
                    const Ctx c = LoadCtx(p);
                    Log("[xma] kick ctx %u: in0 valid=%d packets=%u ptr=%08X | in1 valid=%d packets=%u ptr=%08X | cur=%u readOffset=%u bits (packet %u) | out ptr=%08X blocks=%u write=%u read=%u valid=%d | rate=%u stereo=%d loop count=%u start=%u end=%u | d1=%08X d9=%08X",
                        i, c.in0Valid(), c.in0PacketCount(), c.in0Ptr(), c.in1Valid(), c.in1PacketCount(), c.in1Ptr(), c.currentBuffer(), c.readOffsetBits(), c.readOffsetBits() / kBitsPerPacket,
                        c.outPtr(), c.outBlockCount(), c.outWrite(), c.outRead(), c.outValid(), c.sampleRateId(), c.stereo(), c.loopCount(), c.loopStart(), c.loopEnd(), c.d[1], c.d[9]);
                    const uint8_t* pk = GP(PhysToVirtAddr(c.currentBuffer() ? c.in1Ptr() : c.in0Ptr()));
                    Log("[xma]   first packet header bytes: %02X %02X %02X %02X %02X %02X %02X %02X", pk[0], pk[1], pk[2], pk[3], pk[4], pk[5], pk[6], pk[7]);
                }
                wr32(p, d0 & ~((1u << 20) | (1u << 21)));
            }
        }
    }
}
}  // namespace

uint32_t PhysToVirtAddr(uint32_t phys) { return 0xA0000000u + (phys & 0x1FFFFFFF); }

HOST_FUNC(XMACreateContext)
{
    // (DWORD* outContext)
    if (!g_ctxBase) g_ctxBase = GuestAllocPhysical(kMaxContexts * 64, 256);
    const uint32_t i = g_count.fetch_add(1);
    if (i >= kMaxContexts) { g_count--; ctx.r3.u64 = 0x80070057; return; }
    const uint32_t p = g_ctxBase + i * 64;
    memset(GP(p), 0, 64);
    wr32(ctx.r3.u32, p);
    Log("[xma] XMACreateContext -> %08X", p);
    if (!g_threadStarted.exchange(true)) CreateThread(nullptr, 0, XmaThread, nullptr, 0, nullptr);
    ctx.r3.u64 = 0;
}

HOST_FUNC(XMAReleaseContext)
{
#if ITC_HAVE_FFMPEG
    const uint32_t p = ctx.r3.u32;
    if (g_ctxBase && p >= g_ctxBase && p < g_ctxBase + kMaxContexts * 64) { const uint32_t i = (p - g_ctxBase) / 64; if (g_dec[i]) g_dec[i]->Flush(); }
#endif
    ctx.r3.u64 = 0;
}

void XmaStats(char* out, size_t n) { snprintf(out, n, "xma contexts=%u packets=%llu blocks=%llu", g_count.load(), (unsigned long long)g_packetsFed, (unsigned long long)g_blocksWritten); }
