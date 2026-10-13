import re
p = 'C:/itc-work/runtime/src/gpu.cpp'
s = open(p, encoding='utf-8').read()

old = s[s.index("    if (h & 1)\n    {\n        bool skip = (g_binSelect & g_binMask) == 0 || opcode == 0x64;"):s.index("    switch (opcode)\n    {\n    case 0x60:")]
new = '''    {
        // Predicated tiling (the game renders 3D in 3 horizontal strips). Every draw is submitted once and replayed per strip; which strips a
        // draw touches comes from per-draw screen extents the GPU is supposed to write (EVENT_WRITE_EXT, not emulated), so the draw masks are
        // useless here. Render targets in this renderer are whole frames, so the first strip pass draws everything full frame and the later
        // passes only keep their (predicated) resolves, which copy their slice out of the finished frame.
        const bool isDraw = opcode == 0x22 || opcode == 0x36;
        const bool tiled = g_binSelect != 0xFFFFFFFFull;
        const bool firstPass = tiled && (g_binSelect & 0x80000000ull) != 0;
        const bool laterPass = tiled && !firstPass && g_binSelect != 0;
        static const bool exactMasks = getenv("ITC_EXACT_BIN_MASKS") != nullptr;
        g_tilingActive = tiled && !exactMasks; g_tilingFirstPass = firstPass && !exactMasks;
        bool skip = (h & 1) && ((g_binSelect & g_binMask) == 0 || opcode == 0x64);
        if (!exactMasks && isDraw && (XReg(xr::RB_MODECONTROL) & 7) != 6)
        {
            if (laterPass) skip = true;                                   // everything was drawn in the first pass
            else if (firstPass && (h & 1) && g_binMask != 0) skip = false;   // draw all geometry once, full frame
        }
        if (skip) { g_predSkipped++; return 1 + count; }
    }
'''
s = s.replace(old, new, 1)

# globals + exports
old = "static uint64_t g_binMask = 0xFFFFFFFFull, g_binSelect = 0xFFFFFFFFull;"
new = "static uint64_t g_binMask = 0xFFFFFFFFull, g_binSelect = 0xFFFFFFFFull;\nstatic bool g_tilingActive = false, g_tilingFirstPass = false;\nbool XGpuTilingActive() { return g_tilingActive; }\nbool XGpuTilingFirstPass() { return g_tilingFirstPass; }"
assert old in s
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8').write(s)

p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()
old = "        D3D11_RECT sr = { l, t, r, b };\n        g_ctx->RSSetScissorRects(1, &sr);"
assert old in s
new = "        if (XGpuTilingFirstPass()) { l = 0; t = 0; r = (int32_t)actualW; b = (int32_t)actualH; }   // first strip pass draws the whole frame\n        D3D11_RECT sr = { l, t, r, b };\n        g_ctx->RSSetScissorRects(1, &sr);"
s = s.replace(old, new, 1)
old = "    if (clearColor && rt)\n    {"
assert old in s
new = "    // While a tiled frame is in progress the clears that follow each strip's resolve are skipped: the frame was drawn once, the later\n    // strips still have to read it, and the next frame starts with its own clear draws.\n    const bool tilingNow = XGpuTilingActive();\n    if (clearColor && rt && !tilingNow)\n    {"
s = s.replace(old, new, 1)
old = "    if (clearDepth)\n    {"
assert old in s
s = s.replace(old, "    if (clearDepth && !tilingNow)\n    {", 1)
# declarations near the top of the file (before first use)
old = "uint32_t g_frame = 0, g_dumped = 0;"
assert old in s
s = s.replace(old, "bool XGpuTilingActive();\nbool XGpuTilingFirstPass();\nuint32_t g_frame = 0, g_dumped = 0;", 1)
open(p, 'w', encoding='utf-8').write(s)
print('ok')
