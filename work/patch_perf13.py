p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what, count=1):
    global s
    n = s.count(old)
    if n != count:
        raise SystemExit('count %d != %d: %s' % (n, count, what))
    s = s.replace(old, new)


# Periodic re-validation of static textures is budgeted per frame (82 big-texture hashes landed in one frame = a 25 ms hitch): beyond the budget the check is postponed.
rep('''bool g_forceVbCheck = false;   // set for the draw whose index list was not cached''', '''bool g_forceVbCheck = false;   // set for the draw whose index list was not cached
const uint32_t kRehashBudget = 12;   // periodic texture re-hashes per frame; new textures and textures whose memory was written by the GPU are always checked''', 'budget const')
rep('''        if (ct.stable >= kStableAfter && ct.levels == ml.levels && !WrittenSince(ct.checkedSeq, ct.base, ct.footprint) && ((g_frame + (uint32_t)(key & 31)) & (kRecheckPeriod - 1)) != 0)
        {
            ct.checkedFrame = g_frame;   // static texture: its data is not compared this frame (checkedSeq stays: a later GPU write to its memory still triggers the comparison)
            g_slotComps[slot & 31] = ct.comps;
            return ct.srv.get();
        }''', '''        static uint32_t budgetFrame = 0xFFFFFFFFu, rehashed = 0;
        if (budgetFrame != g_frame) { budgetFrame = g_frame; rehashed = 0; }
        const bool untouched = !WrittenSince(ct.checkedSeq, ct.base, ct.footprint);
        if (ct.stable >= kStableAfter && ct.levels == ml.levels && untouched && (((g_frame + (uint32_t)(key & 31)) & (kRecheckPeriod - 1)) != 0 || rehashed >= kRehashBudget))
        {
            ct.checkedFrame = g_frame;   // static texture: its data is not compared this frame (checkedSeq stays: a later GPU write to its memory still triggers the comparison)
            g_slotComps[slot & 31] = ct.comps;
            return ct.srv.get();
        }
        if (untouched && ct.stable >= kStableAfter) rehashed++;''', 'budget use')
open(p, 'w', encoding='utf-8').write(s)
print('patched part 13')
