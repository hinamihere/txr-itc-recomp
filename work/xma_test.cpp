// Offline check of the XMA2 decode path used by runtime/src/xma.cpp: reads a raw XMA2 packet stream (2 KB packets, e.g. extracted with xwb_info.py), decodes it with
// FFmpeg's XMA2 decoder (same decoder setup as the runtime) and writes a 16-bit WAV. Usage: xma_test.exe in.xma2 out.wav <channels> <rate> [max_packets]
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

int main(int argc, char** argv)
{
    if (argc < 5) { printf("usage: xma_test in.xma2 out.wav channels rate [max_packets]\n"); return 1; }
    const char* inPath = argv[1]; const char* outPath = argv[2];
    const int channels = atoi(argv[3]), rate = atoi(argv[4]);
    const int maxPackets = argc > 5 ? atoi(argv[5]) : 1 << 30;
    SetDllDirectoryA("C:\\itc-work\\ffmpeg");
    HMODULE util = LoadLibraryA("C:\\itc-work\\ffmpeg\\avutil-61.dll"), codec = LoadLibraryA("C:\\itc-work\\ffmpeg\\avcodec-63.dll");
    if (!util || !codec) { printf("cannot load FFmpeg DLLs (%p %p)\n", (void*)util, (void*)codec); return 2; }
#define GET(m, T, n) auto n##_f = (T)GetProcAddress(m, #n); if (!n##_f) { printf("missing " #n "\n"); return 3; }
    GET(codec, decltype(&avcodec_find_decoder), avcodec_find_decoder);
    GET(codec, decltype(&avcodec_alloc_context3), avcodec_alloc_context3);
    GET(codec, decltype(&avcodec_open2), avcodec_open2);
    GET(codec, decltype(&avcodec_send_packet), avcodec_send_packet);
    GET(codec, decltype(&avcodec_receive_frame), avcodec_receive_frame);
    GET(codec, decltype(&av_packet_alloc), av_packet_alloc);
    GET(util, decltype(&av_frame_alloc), av_frame_alloc);
    GET(util, decltype(&av_frame_unref), av_frame_unref);
    GET(util, decltype(&av_malloc), av_malloc);
    GET(util, decltype(&av_channel_layout_default), av_channel_layout_default);
    GET(util, decltype(&av_log_set_level), av_log_set_level);
    av_log_set_level_f(32);
    const AVCodec* c = avcodec_find_decoder_f(AV_CODEC_ID_XMA2);
    if (!c) { printf("no XMA2 decoder\n"); return 4; }
    AVCodecContext* ctx = avcodec_alloc_context3_f(c);
    ctx->sample_rate = rate;
    av_channel_layout_default_f(&ctx->ch_layout, channels);
    ctx->block_align = 2048;
    ctx->extradata = (uint8_t*)av_malloc_f(34 + AV_INPUT_BUFFER_PADDING_SIZE);
    memset(ctx->extradata, 0, 34 + AV_INPUT_BUFFER_PADDING_SIZE);
    ctx->extradata[0] = 1; ctx->extradata[2] = channels == 2 ? 3 : 4;
    ctx->extradata_size = 34;
    int r = avcodec_open2_f(ctx, c, nullptr);
    if (r < 0) { printf("avcodec_open2 failed: %d\n", r); return 5; }
    printf("decoder opened: %s, %d Hz, %d ch, frame_size %d\n", c->name, ctx->sample_rate, ctx->ch_layout.nb_channels, ctx->frame_size);

    FILE* f = fopen(inPath, "rb");
    if (!f) { printf("cannot open %s\n", inPath); return 6; }
    std::vector<uint8_t> data; { fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); data.resize(n); fread(data.data(), 1, n, f); fclose(f); }
    AVPacket* pkt = av_packet_alloc_f(); AVFrame* frame = av_frame_alloc_f();
    std::vector<int16_t> pcm; uint64_t frames = 0; int errors = 0; double peak = 0;
    const int nPackets = (int)(data.size() / 2048);
    for (int i = 0; i < nPackets && i < maxPackets; i++)
    {
        std::vector<uint8_t> buf(2048 + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        memcpy(buf.data(), data.data() + (size_t)i * 2048, 2048);
        pkt->data = buf.data(); pkt->size = 2048;
        const int sent = avcodec_send_packet_f(ctx, pkt);
        if (sent < 0) { if (errors++ < 5) printf("packet %d: send error %d\n", i, sent); continue; }
        for (;;)
        {
            const int rr = avcodec_receive_frame_f(ctx, frame);
            if (rr < 0) break;
            frames++;
            const int n = frame->nb_samples;
            for (int s = 0; s < n; s++)
                for (int ch = 0; ch < channels; ch++)
                {
                    const float* in = (const float*)frame->data[ch < frame->ch_layout.nb_channels ? ch : 0];
                    float v = in[s]; if (v > peak) peak = v; if (-v > peak) peak = -v;
                    v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
                    pcm.push_back((int16_t)(v * 32767.0f));
                }
            av_frame_unref_f(frame);
        }
    }
    printf("%d packets -> %llu frames, %zu samples per channel (%.2f s), peak %.3f, send errors %d\n", nPackets, (unsigned long long)frames, pcm.size() / channels, (double)(pcm.size() / channels) / rate, peak, errors);
    FILE* w = fopen(outPath, "wb");
    if (!w) return 7;
    const uint32_t dataBytes = (uint32_t)(pcm.size() * 2), riffSize = 36 + dataBytes;
    const uint16_t fmt = 1, ch16 = (uint16_t)channels, bits = 16, blockAlign = (uint16_t)(channels * 2);
    const uint32_t rate32 = (uint32_t)rate, byteRate = rate32 * blockAlign, fmtSize = 16;
    fwrite("RIFF", 1, 4, w); fwrite(&riffSize, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w); fwrite(&fmtSize, 4, 1, w); fwrite(&fmt, 2, 1, w); fwrite(&ch16, 2, 1, w);
    fwrite(&rate32, 4, 1, w); fwrite(&byteRate, 4, 1, w); fwrite(&blockAlign, 2, 1, w); fwrite(&bits, 2, 1, w); fwrite("data", 1, 4, w); fwrite(&dataBytes, 4, 1, w);
    fwrite(pcm.data(), 2, pcm.size(), w); fclose(w);
    printf("wrote %s\n", outPath);
    return 0;
}
