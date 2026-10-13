import re

# ---------------- xgpu_d3d.cpp: thumbnail of the presented frame for screen-aware scripted input ----------------
p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()

def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)

rep("bool XGpuTilingActive();      // gpu.cpp: a predicated-tiling frame is in progress",
    "bool XGpuTilingActive();      // gpu.cpp: a predicated-tiling frame is in progress\n"
    "// Screen-aware scripted input: while a script waits for a screen, every 6th frame is reduced to a 64x36 grey thumbnail (mean of 20x20 blocks).\n"
    "std::atomic<bool> g_thumbWanted{ false };\n"
    "std::mutex g_thumbMutex;\nuint8_t g_thumbData[64 * 36];\nuint32_t g_thumbFrame = 0xFFFFFFFFu;\n"
    "void XGpuWantThumb(bool want) { g_thumbWanted = want; }\n"
    "bool XGpuGetThumb(uint8_t* out, uint32_t* frame) { std::lock_guard<std::mutex> l(g_thumbMutex); if (g_thumbFrame == 0xFFFFFFFFu) return false; memcpy(out, g_thumbData, sizeof g_thumbData); if (frame) *frame = g_thumbFrame; return true; }",
    'thumb globals')

rep("    {\n        // A frame with draws skipped because their shaders are still compiling in the background would flicker",
    '''    if (g_thumbWanted.load() && (g_frame % 6) == 0)
    {
        static CP<ID3D11Texture2D> thumbStaging;
        if (!thumbStaging)
        {
            D3D11_TEXTURE2D_DESC d = {}; d.Width = g_backW; d.Height = g_backH; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            g_dev->CreateTexture2D(&d, nullptr, thumbStaging.put());
        }
        D3D11_MAPPED_SUBRESOURCE tm;
        if (thumbStaging && SUCCEEDED((g_ctx->CopyResource(thumbStaging.get(), g_backTex.get()), g_ctx->Map(thumbStaging.get(), 0, D3D11_MAP_READ, 0, &tm))))
        {
            uint8_t t[64 * 36];
            for (int by = 0; by < 36; by++)
                for (int bx = 0; bx < 64; bx++)
                {
                    uint32_t sum = 0;
                    for (int y = 0; y < 20; y++)
                    {
                        const uint8_t* row = (const uint8_t*)tm.pData + (size_t)(by * 20 + y) * tm.RowPitch + bx * 20 * 4;
                        for (int x = 0; x < 20; x++) sum += (row[x * 4] * 77 + row[x * 4 + 1] * 150 + row[x * 4 + 2] * 29) >> 8;
                    }
                    t[by * 64 + bx] = (uint8_t)(sum / 400);
                }
            g_ctx->Unmap(thumbStaging.get(), 0);
            std::lock_guard<std::mutex> l(g_thumbMutex);
            memcpy(g_thumbData, t, sizeof t); g_thumbFrame = g_frame;
        }
    }
    {
        // A frame with draws skipped because their shaders are still compiling in the background would flicker''',
    'thumb capture')
open(p, 'w', encoding='utf-8').write(s)

# ---------------- kernel_xam.cpp: WAIT:<screen> script items ----------------
p = 'C:/itc-work/runtime/src/kernel_xam.cpp'
s = open(p, encoding='utf-8').read()
old_start = s.index("        // Debug: ITC_PAD_SCRIPT=")
old_end = s.index("    if (XGpuWindowFocused())\n    {\n        auto down")
new_block = r'''        // Debug: ITC_PAD_SCRIPT="frame:BUTTON:frames,..." presses buttons at given presented frames (START BACK A B X Y UP DOWN LEFT RIGHT LB RB).
        // "WAIT:<name>" waits until the presented frame looks like refs/<name>.thumb (64x36 grey thumbnail); the items after it use frame
        // numbers relative to that moment. Example: "WAIT:title,10:START:15,WAIT:prompt,30:DOWN:8,90:A:8".
        struct Item { uint32_t start; uint16_t mask; uint32_t len; };
        struct Segment { std::string wait; std::vector<Item> items; std::vector<uint8_t> ref; bool started = false; uint32_t base = 0; };
        static std::vector<Segment> segs = []() {
            std::vector<Segment> v(1);
            const char* e = getenv("ITC_PAD_SCRIPT");
            if (!e) return v;
            std::string str = e;
            size_t pos = 0;
            while (pos < str.size())
            {
                size_t end = str.find(',', pos); if (end == std::string::npos) end = str.size();
                std::string item = str.substr(pos, end - pos);
                pos = end + 1;
                if (item.compare(0, 5, "WAIT:") == 0)
                {
                    Segment sg; sg.wait = item.substr(5);
                    std::string path = "refs/" + sg.wait + ".thumb";
                    if (FILE* f = fopen(path.c_str(), "rb")) { sg.ref.resize(64 * 36); if (fread(sg.ref.data(), 1, sg.ref.size(), f) != sg.ref.size()) sg.ref.clear(); fclose(f); }
                    if (sg.ref.empty()) Log("[pad] reference screen '%s' not found (%s)", sg.wait.c_str(), path.c_str());
                    v.push_back(std::move(sg));
                    continue;
                }
                const size_t c1 = item.find(':'), c2 = item.find(':', c1 + 1);
                if (c1 == std::string::npos || c2 == std::string::npos) continue;
                const std::string name = item.substr(c1 + 1, c2 - c1 - 1);
                static const std::pair<const char*, uint16_t> names[] = { {"START", 0x10}, {"BACK", 0x20}, {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000},
                                                                           {"UP", 1}, {"DOWN", 2}, {"LEFT", 4}, {"RIGHT", 8}, {"LB", 0x100}, {"RB", 0x200} };
                uint16_t mask = 0;
                for (auto& n : names) if (name == n.first) mask = n.second;
                v.back().items.push_back({ (uint32_t)atoi(item.c_str()), mask, (uint32_t)atoi(item.c_str() + c2 + 1) });
            }
            return v;
        }();
        static size_t cur = 0;
        const uint32_t f = XGpuFrame();
        if (segs.size() > 1) XGpuWantThumb(cur + 1 < segs.size());
        if (cur + 1 < segs.size())
        {
            Segment& nx = segs[cur + 1];
            uint8_t th[64 * 36]; uint32_t tf = 0;
            if (!nx.ref.empty() && XGpuGetThumb(th, &tf))
            {
                uint64_t diff = 0;
                for (int i = 0; i < 64 * 36; i++) diff += (uint64_t)abs((int)th[i] - (int)nx.ref[i]);
                static uint32_t lastLogged = 0;
                if (f - lastLogged >= 120) { lastLogged = f; Log("[pad] waiting for '%s': frame %u mean diff %.1f", nx.wait.c_str(), f, diff / (64.0 * 36.0)); }
                if (diff / (64.0 * 36.0) < 6.0) { cur++; nx.started = true; nx.base = f; Log("[pad] screen '%s' seen at frame %u", nx.wait.c_str(), f); }
            }
        }
        {
            const Segment& sg = segs[cur];
            const uint32_t rel = (cur == 0) ? f : f - sg.base;
            for (const Item& it : sg.items) if (rel >= it.start && rel < it.start + it.len) pad.buttons |= it.mask;
        }
    }
'''
s = s[:old_start] + new_block + s[old_end:]
s = s.replace("bool XGpuWindowFocused();\nuint32_t XGpuFrame();", "bool XGpuWindowFocused();\nuint32_t XGpuFrame();\nvoid XGpuWantThumb(bool want);\nbool XGpuGetThumb(uint8_t* out, uint32_t* frame);", 1)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
