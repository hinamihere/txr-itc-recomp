p = 'C:/itc-work/runtime/src/xgpu_shader.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)


# 1. cond_exec_pred_clean / _end are bool-constant execs (Xenia ucode.h: "cexec with a bool constant, not a kCondExecPred")
rep(r'''        case CF_COND_EXEC: case CF_COND_EXEC_END:
            e.kind = 1; e.boolAddr = (uint32_t)((v >> 34) & 0xFF); e.cond = (v >> 42) & 1; e.isEnd = (op == CF_COND_EXEC_END); break;
        case CF_COND_EXEC_PRED: case CF_COND_EXEC_PRED_END: case CF_COND_EXEC_PRED_CLEAN: case CF_COND_EXEC_PRED_CLEAN_END:
            e.kind = 2; e.cond = (v >> 42) & 1; e.isEnd = (op == CF_COND_EXEC_PRED_END || op == CF_COND_EXEC_PRED_CLEAN_END); break;''',
    r'''        case CF_COND_EXEC: case CF_COND_EXEC_END: case CF_COND_EXEC_PRED_CLEAN: case CF_COND_EXEC_PRED_CLEAN_END:
            e.kind = 1; e.boolAddr = (uint32_t)((v >> 34) & 0xFF); e.cond = (v >> 42) & 1; e.isEnd = (op == CF_COND_EXEC_END || op == CF_COND_EXEC_PRED_CLEAN_END); break;
        case CF_COND_EXEC_PRED: case CF_COND_EXEC_PRED_END:
            e.kind = 2; e.cond = (v >> 42) & 1; e.isEnd = (op == CF_COND_EXEC_PRED_END); break;''', 'decodeExec')

# 2. loops: skip when the count is zero, aL advances by the signed step once per iteration, predicated break
rep(r'''            case CF_LOOP_START:
            {
                const uint32_t loopId = (uint32_t)((v >> 16) & 0x1F);   // absolute loop constant index (pixel shaders use 16..31)
                line("xe_lit[%u] = 0; aL = (int)xe_loopStart(%u);", loopId, loopId);
                break;
            }
            case CF_LOOP_END:
            {
                const uint32_t loopId = (uint32_t)((v >> 16) & 0x1F), addr = (uint32_t)(v & 0x1FFF);
                line("aL++;");
                line("xe_lit[%u]++; aL += (int)xe_loopStep(%u);", loopId, loopId);
                line("if (xe_lit[%u] < xe_loopCount(%u)) { pc = %u; continue; }", loopId, loopId, addr);
                break;
            }''',
    r'''            case CF_LOOP_START:
            {
                const uint32_t loopId = (uint32_t)((v >> 16) & 0x1F);   // absolute loop constant index (pixel shaders use 16..31)
                const uint32_t skipAddr = (uint32_t)(v & 0x1FFF);
                const bool isRepeat = (v >> 13) & 1;
                line("if (xe_loopCount(%u) == 0) { pc = %u;%s }", loopId, skipAddr, skipAddr > i ? "" : " continue;");   // a zero-count loop is skipped
                line("xe_lit[%u] = 0;%s", loopId, isRepeat ? "" : (" aL = (int)xe_loopStart(" + std::to_string(loopId) + ");").c_str());
                break;
            }
            case CF_LOOP_END:
            {
                const uint32_t loopId = (uint32_t)((v >> 16) & 0x1F), addr = (uint32_t)(v & 0x1FFF);
                const bool predBreak = (v >> 13) & 1, brkCond = (v >> 42) & 1;
                line("xe_lit[%u]++;", loopId);
                std::string again = "xe_lit[" + std::to_string(loopId) + "] < xe_loopCount(" + std::to_string(loopId) + ")";
                if (predBreak) again += std::string(" && ") + (brkCond ? "!p0" : "p0");   // break when the predicate equals the condition
                line("if (%s) { aL += xe_loopStepS(%u); pc = %u; continue; }", again.c_str(), loopId, addr);
                break;
            }''', 'loops')

# 3. forward jumps need no 'continue' (the skipped blocks simply do not match pc); avoids a construct that FXC's optimiser mishandled
rep(r'''                if (uncond) line("{ pc = %u; continue; }", addr);
                else
                {
                    std::string c = predicated ? std::string(cond ? "" : "!") + "p0" : boolTest(boolAddr) + (cond ? "" : " == false");
                    line("if (%s) { pc = %u; continue; }", c.c_str(), addr);
                }''',
    r'''                const char* cont = addr > i ? "" : " continue;";   // forward jump: later blocks just do not match pc
                if (uncond) line("{ pc = %u;%s }", addr, cont);
                else
                {
                    std::string c = predicated ? std::string(cond ? "" : "!") + "p0" : boolTest(boolAddr) + (cond ? "" : " == false");
                    line("if (%s) { pc = %u;%s }", c.c_str(), addr, cont);
                }''', 'jumps')

# signed loop step accessor
rep(r'''    s += "uint xe_loopStep(uint i) { return (lconst[(i >> 2) & 7][i & 3] >> 16) & 0xFFu; }\n";''',
    r'''    s += "uint xe_loopStep(uint i) { return (lconst[(i >> 2) & 7][i & 3] >> 16) & 0xFFu; }\n";
    s += "int xe_loopStepS(uint i) { return ((int)(lconst[(i >> 2) & 7][i & 3] << 8)) >> 24; }\n";''', 'step accessor')
open(p, 'w', encoding='utf-8').write(s)
print('ok')
