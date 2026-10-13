p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()

def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)

rep('void FlushResolved(uint32_t base)\n{',
    '''const char* g_flushReason = "?";
std::map<std::string, std::pair<uint64_t, uint64_t>> g_flushByReason;   // reason -> (count, bytes) since the last stats line
void FlushResolved(uint32_t base)
{''', 'FlushResolved head')
rep('    g_flushCount++; g_flushBytes += r.bytes;',
    '''    g_flushCount++; g_flushBytes += r.bytes;
    { auto& e = g_flushByReason[g_flushReason]; e.first++; e.second += r.bytes; }
    { static int logged = 0; if (g_log && logged++ < 60) Log("[gpu] flush (%s) E[%08X] %ux%u fmt=%u bytes=%zu frame %u", g_flushReason, base, r.w, r.h, r.fmt, r.bytes, g_frame); }''', 'flush count')
rep('            FlushResolved(base);   // incompatible use: make guest memory authoritative again',
    '            g_flushReason = "fetch-mismatch"; FlushResolved(base);   // incompatible use: make guest memory authoritative again', 'site fetch mismatch')
rep('        if (!g_resolved.empty()) FlushOverlapping(base, TextureFootprint(tf), 0xFFFFFFFF);',
    '        if (!g_resolved.empty()) { g_flushReason = "fetch-overlap"; FlushOverlapping(base, TextureFootprint(tf), 0xFFFFFFFF); }', 'site fetch overlap')
rep('            else if (!(sameLayout && rw >= old.w && rh >= old.h)) FlushResolved(destBase);',
    '            else if (!(sameLayout && rw >= old.w && rh >= old.h)) { g_flushReason = "resolve-neither"; FlushResolved(destBase); }', 'site resolve neither')
rep('        FlushOverlapping(destBase, newBytes, destBase);\n        tgt = &g_resolved[destBase];',
    '        g_flushReason = "resolve-overlap"; FlushOverlapping(destBase, newBytes, destBase);\n        tgt = &g_resolved[destBase];', 'site resolve overlap')
rep('            FlushOverlapping(destBase, (size_t)((destPitch + 31) & ~31u) * ((std::max(destHeight, rh) + 31) & ~31u) * qbpp);',
    '            g_flushReason = "cpu-resolve-overlap"; FlushOverlapping(destBase, (size_t)((destPitch + 31) & ~31u) * ((std::max(destHeight, rh) + 31) & ~31u) * qbpp);', 'site cpu resolve')

# print per-reason counters in the interval stats (after the flush line)
idx = s.index('resolved-texture flushes')
line_start = s.rfind('\n', 0, idx)
print_stmt_pos = s.index('\n', s.index(');', idx)) + 1
s = s[:print_stmt_pos] + '''        for (auto& kv : g_flushByReason) Log("[stat]     flush reason %-20s count=%llu bytes=%llu KB", kv.first.c_str(), (unsigned long long)kv.second.first, (unsigned long long)(kv.second.second / 1024));
        g_flushByReason.clear();
''' + s[print_stmt_pos:]
open(p, 'w', encoding='utf-8').write(s)
print('ok')
