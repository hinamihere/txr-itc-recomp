p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what, count=1):
    global s
    n = s.count(old)
    if n != count:
        raise SystemExit('count %d != %d: %s' % (n, count, what))
    s = s.replace(old, new)


# ---- (a) vertex buffers: small ranges stay in the ring much longer before a persistent buffer is made for them ----------------------------------------
rep('''    if (!g_noVbRing && ((!e.buf && e.frames < kVbPromoteAfter) || (e.buf && e.hot)))
    {''', '''    // Small ranges are cheap to copy and are mostly short-lived dynamic data: they get a persistent buffer only after many frames; big ones after a few.
    const uint32_t promoteAfter = (std::max(rangeEnd, rangeStart) - std::min(rangeStart, avail)) > 16384 ? kVbPromoteAfter : 30;
    if (!g_noVbRing && ((!e.buf && e.frames < promoteAfter) || (e.buf && e.hot)))
    {''', 'promotion')

# ---- (b) batched shader-resource / sampler binds -------------------------------------------------------------------------------------------------
rep('''    for (int stage = 0; stage < 2; stage++)
    {
        ShaderEntry* se = stage ? ps : vs;
        for (uint32_t slot : se->info.textureSlots)
        {
            const uint64_t tt = QpcNow();
            ID3D11ShaderResourceView* srv = GetTextureSRV(slot, se->info.dimOfSlot[slot]);
            g_dp[5] += QpcNow() - tt;''', '''    for (int stage = 0; stage < 2; stage++)
    {
        ShaderEntry* se = stage ? ps : vs;
        // The views and samplers of all slots of the stage are compared with what is bound and sent in one call per kind covering the changed range.
        ID3D11ShaderResourceView** boundSrv = stage ? g_bound.psSrv : g_bound.vsSrv;
        ID3D11SamplerState** boundSmp = stage ? g_bound.psSmp : g_bound.vsSmp;
        int srvLo = 64, srvHi = -1, smpLo = 64, smpHi = -1;
        for (uint32_t slot : se->info.textureSlots)
        {
            const uint64_t tt = QpcNow();
            ID3D11ShaderResourceView* srv = GetTextureSRV(slot, se->info.dimOfSlot[slot]);
            g_dp[5] += QpcNow() - tt;''', 'bind loop head')
rep('''            ID3D11SamplerState* smp = GetSamplerFast(slot);
            if (stage) { BindPSSrv(slot, srv); BindPSSmp(slot & 15, smp); }
            else { BindVSSrv(slot, srv); BindVSSmp(slot & 15, smp); }
        }
    }
''', '''            ID3D11SamplerState* smp = GetSamplerFast(slot);
            if (g_noFilter || boundSrv[slot & 31] != srv) { boundSrv[slot & 31] = srv; srvLo = std::min<int>(srvLo, slot & 31); srvHi = std::max<int>(srvHi, slot & 31); }
            if (g_noFilter || boundSmp[slot & 15] != smp) { boundSmp[slot & 15] = smp; smpLo = std::min<int>(smpLo, slot & 15); smpHi = std::max<int>(smpHi, slot & 15); }
        }
        if (srvHi >= 0)
        {
            for (int i = srvLo; i <= srvHi; i++) if (boundSrv[i] == (ID3D11ShaderResourceView*)kUnk) boundSrv[i] = nullptr;   // unknown slots inside the range are simply cleared
            if (stage) g_ctx->PSSetShaderResources(srvLo, srvHi - srvLo + 1, boundSrv + srvLo); else g_ctx->VSSetShaderResources(srvLo, srvHi - srvLo + 1, boundSrv + srvLo);
        }
        if (smpHi >= 0)
        {
            for (int i = smpLo; i <= smpHi; i++) if (boundSmp[i] == (ID3D11SamplerState*)kUnk) boundSmp[i] = nullptr;
            if (stage) g_ctx->PSSetSamplers(smpLo, smpHi - smpLo + 1, boundSmp + smpLo); else g_ctx->VSSetSamplers(smpLo, smpHi - smpLo + 1, boundSmp + smpLo);
        }
    }
''', 'bind loop tail')
# vertex buffer views (t32..t127) and the index view: one range call as well
rep('''    BindVSSrv(31, g_idxSRV.get());
    for (uint32_t slot : vs->info.vfetchSlots)
    {''', '''    int vbLo = 128, vbHi = -1;
    auto wantVS = [&](uint32_t t, ID3D11ShaderResourceView* v) { if (g_noFilter || g_bound.vsSrv[t] != v) { g_bound.vsSrv[t] = v; vbLo = std::min<int>(vbLo, (int)t); vbHi = std::max<int>(vbHi, (int)t); } };
    wantVS(31, g_idxSRV.get());
    for (uint32_t slot : vs->info.vfetchSlots)
    {''', 'vb bind head')
rep('''        BindVSSrv(32 + slot, srv);
    }
    static const char* dumpPs''', '''        wantVS(32 + slot, srv);
    }
    if (vbHi >= 0)
    {
        for (int i = vbLo; i <= vbHi; i++) if (g_bound.vsSrv[i] == (ID3D11ShaderResourceView*)kUnk) g_bound.vsSrv[i] = nullptr;
        g_ctx->VSSetShaderResources(vbLo, vbHi - vbLo + 1, g_bound.vsSrv + vbLo);
    }
    static const char* dumpPs''', 'vb bind tail')

# ---- (c) refill decisions compare the values the shader reads (vfmeta / patched swizzles), not fetch-constant versions ------------------------------------
rep('''    static uint32_t lastCompsV[32], lastCompsP[32], lastFetchV[32], lastFetchP[32];
    static const bool dbgRainbow = getenv("ITC_DEBUG_PS_RAINBOW") != nullptr;
    auto fetchChanged = [](const uint32_t* last, const ShaderEntry* se, bool withVfetch)
    {
        for (uint32_t slot : se->info.textureSlots) if (last[slot & 31] != g_fetchVer[slot & 31]) return true;
        if (withVfetch) for (uint32_t slot : se->info.vfetchSlots) if (last[(slot / 3) & 31] != g_fetchVer[(slot / 3) & 31]) return true;
        return false;
    };
    static uint32_t lastVfBase[96];
    auto vfBaseChanged = [&]() { for (uint32_t slot : vs->info.vfetchSlots) if (lastVfBase[slot] != g_vfBaseDw[slot]) return true; return false; };
    const bool refillV = g_noCbDedupe || !haveV || (g_regDirty & (kDirtyVsFloat | kDirtyBoolLoop | kDirtyMisc)) || lastVsHash != g_curVS.hash || lastRtW != rtW || lastRtH != rtH
                         || memcmp(lastCompsV, g_slotComps, sizeof lastCompsV) != 0 || fetchChanged(lastFetchV, vs, true) || vfBaseChanged();
    const bool refillP = g_noCbDedupe || !haveP || dbgRainbow || (g_regDirty & (kDirtyPsFloat | kDirtyBoolLoop | kDirtyMisc)) || memcmp(lastCompsP, g_slotComps, sizeof lastCompsP) != 0 || fetchChanged(lastFetchP, ps, false);''',
    '''    static const bool dbgRainbow = getenv("ITC_DEBUG_PS_RAINBOW") != nullptr;
    // Per-slot values as the shaders read them: the fetch constant's swizzle (patched for the texture's component count) and the vertex fetch metadata.
    auto patchedSwz = [](uint32_t slot) -> uint32_t
    {
        const uint32_t d3 = g_xregs[xr::SHADER_CONSTANT_FETCH_00_3 + slot * 6];
        const uint32_t sw = ((d3 >> 1) & 0xFFF) | (((d3 >> 13) & 0x3F) << 12);
        const uint32_t comps = g_slotComps[slot & 31];
        if (comps >= 3) return sw;
        uint32_t out = 0;
        for (uint32_t ch = 0; ch < 4; ch++)
        {
            uint32_t v = (sw >> (ch * 3)) & 7;
            if (comps == 1 && v >= 1 && v <= 3) v = 0;
            else if (comps == 2 && v >= 2 && v <= 3) v = 1;
            out |= v << (ch * 3);
        }
        return out | (sw & ~0xFFFu);
    };
    auto texSwzChanged = [&](const XeCB& cb, const ShaderEntry* se) { for (uint32_t slot : se->info.textureSlots) if (cb.texswz[slot & 31] != patchedSwz(slot & 31)) return true; return false; };
    auto vfMetaChanged = [&]()
    {
        for (uint32_t slot : vs->info.vfetchSlots)
        {
            const uint32_t d1 = g_xregs[xr::SHADER_CONSTANT_FETCH_00_0 + (slot / 3) * 6 + (slot % 3) * 2 + 1];
            if (cbv.vfmeta[slot][0] != g_vfBaseDw[slot] || cbv.vfmeta[slot][1] != ((d1 >> 2) & 0xFFFFFF) || cbv.vfmeta[slot][2] != (d1 & 3)) return true;
        }
        return false;
    };
    const bool refillV = g_noCbDedupe || !haveV || (g_regDirty & (kDirtyVsFloat | kDirtyBoolLoop | kDirtyMisc)) || lastVsHash != g_curVS.hash || lastRtW != rtW || lastRtH != rtH || vfMetaChanged() || texSwzChanged(cbv, vs);
    const bool refillP = g_noCbDedupe || !haveP || dbgRainbow || (g_regDirty & (kDirtyPsFloat | kDirtyBoolLoop | kDirtyMisc)) || texSwzChanged(cbp, ps);''', 'refill rules')
rep('''    if (refillV) { PatchSwizzles(cbv); if (!UploadCBRing(false, cbv)) UploadCB(g_cbVS.get(), cbv); haveV = true; lastVsHash = g_curVS.hash; lastRtW = rtW; lastRtH = rtH; memcpy(lastCompsV, g_slotComps, sizeof lastCompsV); memcpy(lastFetchV, g_fetchVer, sizeof lastFetchV); memcpy(lastVfBase, g_vfBaseDw, sizeof lastVfBase); g_cbUploadsV++; }
    if (refillP) { PatchSwizzles(cbp); if (!UploadCBRing(true, cbp)) UploadCB(g_cbPS.get(), cbp); haveP = true; memcpy(lastCompsP, g_slotComps, sizeof lastCompsP); memcpy(lastFetchP, g_fetchVer, sizeof lastFetchP); g_cbUploadsP++; }''',
    '''    if (refillV) { PatchSwizzles(cbv); if (!UploadCBRing(false, cbv)) UploadCB(g_cbVS.get(), cbv); haveV = true; lastVsHash = g_curVS.hash; lastRtW = rtW; lastRtH = rtH; g_cbUploadsV++; }
    if (refillP) { PatchSwizzles(cbp); if (!UploadCBRing(true, cbp)) UploadCB(g_cbPS.get(), cbp); haveP = true; g_cbUploadsP++; }''', 'refill upload')
open(p, 'w', encoding='utf-8').write(s)
print('patched part 9')
