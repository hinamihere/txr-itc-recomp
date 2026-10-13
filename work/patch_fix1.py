p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what, count=1):
    global s
    n = s.count(old)
    if n != count:
        raise SystemExit('count %d != %d: %s' % (n, count, what))
    s = s.replace(old, new)


# ---- 1. constant ring: a DISCARD (wrap) loses every block that is still bound but not re-uploaded -> both stages are re-uploaded after a wrap ----------------------------
rep('''    const bool refillV = g_noCbDedupe || !haveV || (g_regDirty & (kDirtyVsFloat | kDirtyBoolLoop | kDirtyMisc)) || lastVsHash != g_curVS.hash || lastRtW != rtW || lastRtH != rtH || vfMetaChanged() || texSwzChanged(cbv, vs);
    const bool refillP = g_noCbDedupe || !haveP || dbgRainbow || (g_regDirty & (kDirtyPsFloat | kDirtyBoolLoop | kDirtyMisc)) || texSwzChanged(cbp, ps);''',
    '''    // When the constant ring cannot take this draw's two blocks it restarts at slot 0 with a WRITE_DISCARD: the old contents are gone, so the blocks that stay bound
    // from earlier draws (the stage whose constants did not change) must be uploaded again - otherwise they read undefined memory (black / garbage triangles).
    bool cbWrap = false;
    if (g_cbRing && g_cbRingPos + 2 > kCbRingEntries) { cbWrap = true; g_cbRingPos = 0; }
    const bool refillV = cbWrap || g_noCbDedupe || !haveV || (g_regDirty & (kDirtyVsFloat | kDirtyBoolLoop | kDirtyMisc)) || lastVsHash != g_curVS.hash || lastRtW != rtW || lastRtH != rtH || vfMetaChanged() || texSwzChanged(cbv, vs);
    const bool refillP = cbWrap || g_noCbDedupe || !haveP || dbgRainbow || (g_regDirty & (kDirtyPsFloat | kDirtyBoolLoop | kDirtyMisc)) || texSwzChanged(cbp, ps);''', 'cb wrap')
rep('''    if (FAILED(MapTimed(g_cbRing.get(), g_cbRingPos == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, &m, "constant ring"))) return false;
    memcpy((uint8_t*)m.pData + (size_t)g_cbRingPos * kCbRingStride, &cb, sizeof cb);''',
    '''    if (g_cbRingPos >= kCbRingEntries) g_cbRingPos = 0;
    if (FAILED(MapTimed(g_cbRing.get(), g_cbRingPos == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, &m, "constant ring"))) return false;
    memcpy((uint8_t*)m.pData + (size_t)g_cbRingPos * kCbRingStride, &cb, sizeof cb);''', 'cb ring bound')
rep('''    g_cbRingPos = (g_cbRingPos + 1) % kCbRingEntries;
    return true;''', '''    g_cbRingPos++;   // (the wrap to 0 is handled by the caller before the uploads of a draw)
    return true;''', 'cb ring advance')

# ---- 2. vertex ring: copy the WHOLE guest buffer (<= 64 KB) instead of the range the index list implies; bigger buffers get persistent Direct3D buffers ------------------
rep('''ID3D11ShaderResourceView* VbRingCopy(const uint8_t* src, uint32_t rangeStart, uint32_t rangeEnd, uint32_t avail, uint32_t* baseDw)
{
    if (!g_vbRing) return nullptr;
    const uint32_t rs = std::min(rangeStart & ~15u, avail), re = std::min((rangeEnd + 15) & ~15u, avail);
    if (re <= rs) return nullptr;
    const uint32_t n = re - rs;
    if (n > kVbRing / 8) return nullptr;   // huge range: a persistent buffer is the better deal''',
    '''const uint32_t kVbRingMaxBuffer = 64u << 10;   // only guest buffers up to this size go through the ring, copied whole (shaders may fetch with computed indices anywhere in the buffer)
ID3D11ShaderResourceView* VbRingCopy(const uint8_t* src, uint32_t rangeStart, uint32_t rangeEnd, uint32_t avail, uint32_t* baseDw)
{
    (void)rangeStart; (void)rangeEnd;
    if (!g_vbRing || avail > kVbRingMaxBuffer) return nullptr;
    const uint32_t rs = 0, re = avail;
    const uint32_t n = re - rs;''', 'ring whole')
rep('''    const uint32_t promoteAfter = (std::max(rangeEnd, rangeStart) - std::min(rangeStart, avail)) > 16384 ? kVbPromoteAfter : 30;''',
    '''    const uint32_t promoteAfter = avail > 16384 ? kVbPromoteAfter : 30;''', 'promote by size')
rep('''                if (!g_noVbRing && e.changeStreak >= 3 && rangeEnd - rangeStart <= (kVbRing / 8)) e.hot = 180;''',
    '''                if (!g_noVbRing && e.changeStreak >= 3 && avail <= kVbRingMaxBuffer) e.hot = 180;''', 'hot by size')
open(p, 'w', encoding='utf-8').write(s)
print('patched fix1')
