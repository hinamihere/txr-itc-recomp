p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()

def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)

rep("CP<ID3D11Texture2D> g_backTex;",
    "CP<ID3D11Texture2D> g_backTex;\nCP<ID3D11Texture2D> g_lastGoodTex;   // copy of the last frame that was drawn completely (shown while shaders compile)\nbool g_haveGood = false;\nuint64_t g_pendingMark = 0;\nuint32_t g_heldFrames = 0;", 'globals')

rep("    g_swap->Present(1, 0);\n    if (g_infoQueue)",
    '''    {
        // A frame with draws skipped because their shaders are still compiling in the background would flicker, so the last complete frame
        // stays on screen until the compile finishes (at most ~6 s).
        const bool incomplete = g_shaderPending != g_pendingMark;
        g_pendingMark = g_shaderPending;
        if (!g_lastGoodTex && g_backTex)
        {
            D3D11_TEXTURE2D_DESC bd; g_backTex->GetDesc(&bd);
            bd.BindFlags = 0; bd.MiscFlags = 0; bd.CPUAccessFlags = 0; bd.Usage = D3D11_USAGE_DEFAULT;
            g_dev->CreateTexture2D(&bd, nullptr, g_lastGoodTex.put());
        }
        if (g_lastGoodTex)
        {
            if (incomplete && g_haveGood && g_heldFrames < 180) { g_ctx->CopyResource(g_backTex.get(), g_lastGoodTex.get()); g_heldFrames++; }
            else { g_ctx->CopyResource(g_lastGoodTex.get(), g_backTex.get()); g_haveGood = true; g_heldFrames = 0; }
        }
    }
    g_swap->Present(1, 0);
    if (g_infoQueue)''', 'present')
open(p, 'w', encoding='utf-8').write(s)
print('ok')
