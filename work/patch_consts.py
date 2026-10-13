p = 'C:/itc-work/runtime/src/xgpu_shader.cpp'
s = open(p, encoding='utf-8').read()

def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)

rep('snprintf(b, sizeof b, "xe_bool(%u)", addr + (p.isPixel ? 128u : 0u));',
    'snprintf(b, sizeof b, "xe_bool(%u)", addr);   // pixel shaders encode absolute addresses (128..255)', 'boolTest')

rep('line("if (aL < (int)xe_loopCount(%u)) { pc = %u; continue; }", loopId + (p.isPixel ? 16u : 0u), addr);',
    'line("xe_lit[%u]++; aL += (int)xe_loopStep(%u);", loopId, loopId);\n'
    '                line("if (xe_lit[%u] < xe_loopCount(%u)) { pc = %u; continue; }", loopId, loopId, addr);', 'loop end')

rep('            case CF_LOOP_START:\n                line("aL = 0;");\n                break;',
    '            case CF_LOOP_START:\n            {\n'
    '                const uint32_t loopId = (uint32_t)((v >> 16) & 0x1F);   // absolute loop constant index (pixel shaders use 16..31)\n'
    '                line("xe_lit[%u] = 0; aL = (int)xe_loopStart(%u);", loopId, loopId);\n'
    '                break;\n            }', 'loop start')

rep('        line("uint pc = 0;");',
    '        line("uint pc = 0; uint xe_lit[32];");\n        line("[unroll] for (int xl = 0; xl < 32; xl++) xe_lit[xl] = 0;");', 'pc decl')

rep('    s += "uint xe_loopCount(uint i) { return lconst[i >> 2][i & 3] & 0xFFu; }\\n";',
    '    s += "uint xe_loopCount(uint i) { return lconst[(i >> 2) & 7][i & 3] & 0xFFu; }\\n";\n'
    '    s += "uint xe_loopStart(uint i) { return (lconst[(i >> 2) & 7][i & 3] >> 8) & 0xFFu; }\\n";\n'
    '    s += "uint xe_loopStep(uint i) { return (lconst[(i >> 2) & 7][i & 3] >> 16) & 0xFFu; }\\n";', 'loopCount decl')
open(p, 'w', encoding='utf-8').write(s)

p = 'C:/itc-work/runtime/src/xgpu_d3d.cpp'
s = open(p, encoding='utf-8').read()
rep('''    if (isPixel)
    {
        for (int i = 0; i < 4; i++) cb.bconst[i] = g_xregs[0x4904 + i];
        for (int i = 0; i < 16; i++) cb.lconst[i] = g_xregs[0x4908 + 16 + i];
    }
    else
    {
        for (int i = 0; i < 4; i++) cb.bconst[i] = g_xregs[0x4900 + i];
        for (int i = 0; i < 16; i++) cb.lconst[i] = g_xregs[0x4908 + i];
    }''',
    '''    // Both stages see the whole boolean (256 bits) and loop (32 dwords) constant files: shaders address them absolutely
    // (pixel shaders use bool 128..255 and loop 16..31).
    for (int i = 0; i < 8; i++) cb.bconst[i] = g_xregs[0x4900 + i];
    for (int i = 0; i < 32; i++) cb.lconst[i] = g_xregs[0x4908 + i];''', 'FillConstants')
open(p, 'w', encoding='utf-8').write(s)
print('ok')
