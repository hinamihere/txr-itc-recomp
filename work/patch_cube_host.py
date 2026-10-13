p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)


# cube texture builder, placed right before GetTextureSRV
marker = '// Returns an SRV for the texture fetch constant (dimension must match what the shader declared).'
cube_fn = r'''// Cube texture: six consecutive 4 KB-aligned faces (+X,-X,+Y,-Y,+Z,-Z), each decoded like a 2D base level (mips are not used yet: the game's
// cube fetch constants select the base map only).
ID3D11ShaderResourceView* GetCubeSRV(uint32_t slot, const TexFetch& tf)
{
    const size_t faceBytes = TextureFootprint(tf);
    const uint32_t faceStride = (uint32_t)((faceBytes + 4095) & ~(size_t)4095);
    const uint64_t key = Hash64(tf.d, sizeof tf.d) ^ 0x7C0BE5;
    CachedTex& ct = g_textures[key];
    const size_t footprint = (size_t)faceStride * 6;
    if (ct.srv && ct.checkedFrame == g_frame && !WrittenSince(ct.checkedSeq, ct.base, footprint)) { g_slotComps[slot & 31] = ct.comps; return ct.srv.get(); }
    Decoded faces[6];
    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint32_t f = 0; f < 6; f++)
    {
        TexFetch tf2 = tf;
        tf2.d[1] = (tf.d[1] & 0xFFFu) | ((tf.base() + f * faceStride) & 0xFFFFF000u);
        tf2.d[5] = (tf.d[5] & ~(3u << 9)) | (1u << 9);   // make it look like a 2D texture
        uint64_t h = 0;
        { ProfScope ps(g_profDecode); DecodeTexture(tf2, faces[f], false, &h); }
        hash = (hash ^ h) * 0x100000001B3ull;
        if (faces[f].data.empty() || faces[f].fmt == DXGI_FORMAT_UNKNOWN || faces[f].w != faces[0].w || faces[f].h != faces[0].h) return nullptr;
    }
    ct.checkedFrame = g_frame; ct.checkedSeq = g_memSeq;
    if (ct.srv && hash == ct.dataHash) { g_slotComps[slot & 31] = ct.comps; return ct.srv.get(); }
    g_decodeCount++; g_decodeBytes += faces[0].data.size() * 6;
    if (ct.tex && ct.w == faces[0].w && ct.h == faces[0].h && ct.fmt == faces[0].fmt)
    {
        for (uint32_t f = 0; f < 6; f++) g_ctx->UpdateSubresource(ct.tex.get(), f, nullptr, faces[f].data.data(), faces[f].rowPitch, 0);
        ct.dataHash = hash; ct.comps = faces[0].comps; g_slotComps[slot & 31] = ct.comps;
        return ct.srv.get();
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = faces[0].w; d.Height = faces[0].h; d.MipLevels = 1; d.ArraySize = 6; d.Format = faces[0].fmt; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    D3D11_SUBRESOURCE_DATA sd[6];
    for (int f = 0; f < 6; f++) sd[f] = { faces[f].data.data(), faces[f].rowPitch, 0 };
    CP<ID3D11Texture2D> t;
    if (FAILED(g_dev->CreateTexture2D(&d, sd, t.put()))) { Log("[gpu] cube texture create failed %ux%u fmt %u", faces[0].w, faces[0].h, (unsigned)faces[0].fmt); return nullptr; }
    CP<ID3D11ShaderResourceView> srv;
    g_dev->CreateShaderResourceView(t.get(), nullptr, srv.put());
    g_texBytes += faces[0].data.size() * 6; g_texBytes -= ct.bytes;
    ct.base = tf.base(); ct.footprint = footprint; ct.tex = t; ct.srv = srv; ct.dataHash = hash; ct.w = faces[0].w; ct.h = faces[0].h; ct.fmt = faces[0].fmt;
    ct.bytes = faces[0].data.size() * 6; ct.comps = faces[0].comps; g_slotComps[slot & 31] = ct.comps;
    if (g_log) Log("[gpu] cube texture slot %u %ux%u fmt=%u base=%08X face stride %X", slot, ct.w, ct.h, tf.format(), tf.base(), faceStride);
    return srv.get();
}

'''
rep(marker, cube_fn + marker, 'GetCubeSRV placement')

rep('    if (shaderDim == 3) return g_dummyCube.get();\n    if (tf.type() != 2) return g_dummy2D.get();',
    '''    if (shaderDim == 3)
    {
        if (tf.type() == 2 && tf.dimension() == 3) if (ID3D11ShaderResourceView* cube = GetCubeSRV(slot, tf)) return cube;
        return g_dummyCube.get();
    }
    if (tf.type() != 2) return g_dummy2D.get();''', 'use cube')
open(p, 'w', encoding='utf-8').write(s)
print('ok')
