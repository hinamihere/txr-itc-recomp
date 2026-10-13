p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what, count=1):
    global s
    n = s.count(old)
    if n != count:
        raise SystemExit('count %d != %d: %s' % (n, count, what))
    s = s.replace(old, new)


# A/B switches for the draw-path optimisations (to bisect visual problems): ITC_NO_PERF=1 turns them all off
rep('''const bool g_skipDraws = getenv("ITC_SKIP_DRAWS") != nullptr;''', '''const bool g_skipDraws = getenv("ITC_SKIP_DRAWS") != nullptr;
const bool g_noPerf = getenv("ITC_NO_PERF") != nullptr;   // A/B switches for the draw-path optimisations (to bisect visual problems): ITC_NO_PERF=1 turns them all off
const bool g_noFilter = g_noPerf || getenv("ITC_NO_STATEFILTER") != nullptr;   // send every state call
const bool g_noIdxCache = g_noPerf || getenv("ITC_NO_IDXCACHE") != nullptr;    // convert and upload the index list of every draw
const bool g_noCbDedupe = g_noPerf || getenv("ITC_NO_CBDEDUP") != nullptr;     // rewrite the constant buffers for every draw
const bool g_noTexMemo = g_noPerf || getenv("ITC_NO_TEXMEMO") != nullptr;      // look every texture up again''', 'switches')

rep('''inline void BindVSSrv(uint32_t slot, ID3D11ShaderResourceView* v) { if (g_bound.vsSrv[slot] == v) return;''', '''inline void BindVSSrv(uint32_t slot, ID3D11ShaderResourceView* v) { if (!g_noFilter && g_bound.vsSrv[slot] == v) return;''', 'b1')
rep('''inline void BindPSSrv(uint32_t slot, ID3D11ShaderResourceView* v) { if (g_bound.psSrv[slot] == v) return;''', '''inline void BindPSSrv(uint32_t slot, ID3D11ShaderResourceView* v) { if (!g_noFilter && g_bound.psSrv[slot] == v) return;''', 'b2')
rep('''inline void BindVSSmp(uint32_t slot, ID3D11SamplerState* v) { if (g_bound.vsSmp[slot] == v) return;''', '''inline void BindVSSmp(uint32_t slot, ID3D11SamplerState* v) { if (!g_noFilter && g_bound.vsSmp[slot] == v) return;''', 'b3')
rep('''inline void BindPSSmp(uint32_t slot, ID3D11SamplerState* v) { if (g_bound.psSmp[slot] == v) return;''', '''inline void BindPSSmp(uint32_t slot, ID3D11SamplerState* v) { if (!g_noFilter && g_bound.psSmp[slot] == v) return;''', 'b4')

rep('''    if (memcmp(g_bound.rtv, rtvs, sizeof rtvs) != 0 || g_bound.dsv != dsv)
    {''', '''    if (g_noFilter || memcmp(g_bound.rtv, rtvs, sizeof rtvs) != 0 || g_bound.dsv != dsv)
    {''', 'rt')
rep('''        if (g_bound.blend != bs || memcmp(g_bound.bf, bfBits, sizeof bfBits) != 0) {''', '''        if (g_noFilter || g_bound.blend != bs || memcmp(g_bound.bf, bfBits, sizeof bfBits) != 0) {''', 'blend')
rep('''        if (g_bound.ds != dss || g_bound.stRef != stRef) {''', '''        if (g_noFilter || g_bound.ds != dss || g_bound.stRef != stRef) {''', 'ds')
rep('''        if (g_bound.rs != rss) {''', '''        if (g_noFilter || g_bound.rs != rss) {''', 'rs')
rep('''    if (memcmp(&g_bound.vp, &vp, sizeof vp) != 0) {''', '''    if (g_noFilter || memcmp(&g_bound.vp, &vp, sizeof vp) != 0) {''', 'vp')
rep('''        if (memcmp(&g_bound.sc, &sr, sizeof sr) != 0) {''', '''        if (g_noFilter || memcmp(&g_bound.sc, &sr, sizeof sr) != 0) {''', 'sc')
rep('''    if (g_bound.topo != (int)topo) {''', '''    if (g_noFilter || g_bound.topo != (int)topo) {''', 'topo')
rep('''        if (g_bound.vs != vs->vs.get()) {''', '''        if (g_noFilter || g_bound.vs != vs->vs.get()) {''', 'vs')
rep('''        if (g_bound.gs != gs) {''', '''        if (g_noFilter || g_bound.gs != gs) {''', 'gs')
rep('''        if (g_bound.ps != psObj) {''', '''        if (g_noFilter || g_bound.ps != psObj) {''', 'ps')
rep('''        if (!haveV || memcmp(&cbv, &lastV, sizeof cbv) != 0) {''', '''        if (g_noCbDedupe || !haveV || memcmp(&cbv, &lastV, sizeof cbv) != 0) {''', 'cbv')
rep('''        if (!haveP || memcmp(&cbp, &lastP, sizeof cbp) != 0) {''', '''        if (g_noCbDedupe || !haveP || memcmp(&cbp, &lastP, sizeof cbp) != 0) {''', 'cbp')
rep('''        if (g_bound.vsCb != g_cbVS.get()) {''', '''        if (g_noFilter || g_bound.vsCb != g_cbVS.get()) {''', 'vscb')
rep('''        if (g_bound.psCb != g_cbPS.get()) {''', '''        if (g_noFilter || g_bound.psCb != g_cbPS.get()) {''', 'pscb')
rep('''        const bool detailed = g_frame == g_captureFrame || g_frame == g_traceFrame || g_log;''', '''        const bool detailed = g_frame == g_captureFrame || g_frame == g_traceFrame || g_log || g_noIdxCache;''', 'idx')
rep('''    if (m.srv && m.frame == g_frame && m.seq == g_memSeq''', '''    if (!g_noTexMemo && m.srv && m.frame == g_frame && m.seq == g_memSeq''', 'memo')
open(p, 'w', encoding='utf-8').write(s)
print('patched part 2')
