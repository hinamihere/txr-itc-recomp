// Xenos shader microcode -> HLSL SM5 translator.
// Instruction semantics follow hedge-dev/XenosRecomp (MIT) and Xenia's ucode definitions (BSD-3).
// Bit-fields are decoded with explicit shifts (portable across compilers).
#include "xgpu_shader.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <map>

namespace {

// ---- control flow opcodes ------------------------------------------------------------------------------
enum CfOp { CF_NOP = 0, CF_EXEC = 1, CF_EXEC_END = 2, CF_COND_EXEC = 3, CF_COND_EXEC_END = 4, CF_COND_EXEC_PRED = 5,
            CF_COND_EXEC_PRED_END = 6, CF_LOOP_START = 7, CF_LOOP_END = 8, CF_COND_CALL = 9, CF_RETURN = 10,
            CF_COND_JMP = 11, CF_ALLOC = 12, CF_COND_EXEC_PRED_CLEAN = 13, CF_COND_EXEC_PRED_CLEAN_END = 14, CF_MARK_VS_FETCH_DONE = 15 };

enum ScalarOp { S_ADDS = 0, S_ADDS_PREV, S_MULS, S_MULS_PREV, S_MULS_PREV2, S_MAXS, S_MINS, S_SEQS, S_SGTS, S_SGES, S_SNES, S_FRCS, S_TRUNCS,
                S_FLOORS, S_EXP, S_LOGC, S_LOG, S_RCPC, S_RCPF, S_RCP, S_RSQC, S_RSQF, S_RSQ, S_MAXAS, S_MAXASF, S_SUBS, S_SUBS_PREV,
                S_SETP_EQ, S_SETP_NE, S_SETP_GT, S_SETP_GE, S_SETP_INV, S_SETP_POP, S_SETP_CLR, S_SETP_RSTR, S_KILLS_EQ, S_KILLS_GT,
                S_KILLS_GE, S_KILLS_NE, S_KILLS_ONE, S_SQRT, S_MULSC0 = 42, S_MULSC1, S_ADDSC0, S_ADDSC1, S_SUBSC0, S_SUBSC1, S_SIN, S_COS,
                S_RETAIN_PREV };

enum VectorOp { V_ADD = 0, V_MUL, V_MAX, V_MIN, V_SEQ, V_SGT, V_SGE, V_SNE, V_FRC, V_TRUNC, V_FLOOR, V_MAD, V_CNDEQ, V_CNDGE, V_CNDGT,
                V_DP4, V_DP3, V_DP2ADD, V_CUBE, V_MAX4, V_SETP_EQ_PUSH, V_SETP_NE_PUSH, V_SETP_GT_PUSH, V_SETP_GE_PUSH, V_KILL_EQ,
                V_KILL_GT, V_KILL_GE, V_KILL_NE, V_DST, V_MAXA };

constexpr char SW[] = { 'x', 'y', 'z', 'w', '0', '1', '_', '_' };

inline uint32_t bits(uint32_t v, int lo, int n) { return (v >> lo) & ((n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1)); }
inline int32_t sbits(uint32_t v, int lo, int n)
{
    uint32_t x = bits(v, lo, n);
    const uint32_t sign = 1u << (n - 1);
    return (int32_t)((x ^ sign) - sign);
}

struct Translator
{
    const uint32_t* code;
    uint32_t dwordCount;
    XTranslateParams p;
    XShaderInfo& info;
    std::string out;
    int indentation = 1;

    Translator(const uint32_t* c, uint32_t n, const XTranslateParams& pp, XShaderInfo& i) : code(c), dwordCount(n), p(pp), info(i) {}

    void fm(const char* f, ...)
    {
        char buf[4096];
        va_list ap; va_start(ap, f); vsnprintf(buf, sizeof buf, f, ap); va_end(ap);
        out += buf;
    }
    void ind() { for (int i = 0; i < indentation; i++) out += '\t'; }
    void line(const char* f, ...)
    {
        char buf[4096];
        va_list ap; va_start(ap, f); vsnprintf(buf, sizeof buf, f, ap); va_end(ap);
        ind(); out += buf; out += '\n';
    }

    // ------------------------------------------------------------------------------------------------------
    // Fetch destination swizzles
    // ------------------------------------------------------------------------------------------------------
    static uint32_t dstSwz(uint32_t dstSwizzle, uint32_t i) { return (dstSwizzle >> (i * 3)) & 7; }

    std::string dstComponents(uint32_t dstSwizzle, bool operand)
    {
        std::string s;
        for (uint32_t i = 0; i < 4; i++)
        {
            const uint32_t sw = dstSwz(dstSwizzle, i);
            if (sw <= 3) s += SW[operand ? sw : i];
        }
        return s;
    }

    void dstConst01(uint32_t dstReg, uint32_t dstSwizzle)
    {
        for (uint32_t i = 0; i < 4; i++)
        {
            const uint32_t sw = dstSwz(dstSwizzle, i);
            if (sw == 4) line("r[%u].%c = 0.0;", dstReg, SW[i]);
            else if (sw == 5) line("r[%u].%c = 1.0;", dstReg, SW[i]);
        }
    }

    // ------------------------------------------------------------------------------------------------------
    // Vertex fetch
    // ------------------------------------------------------------------------------------------------------
    void vertexFetch(uint32_t w0, uint32_t w1, uint32_t w2)
    {
        const uint32_t srcReg = bits(w0, 5, 6), dstReg = bits(w0, 12, 6);
        const uint32_t constIndex = bits(w0, 20, 5), constSel = bits(w0, 25, 2), srcSwizzle = bits(w0, 30, 2);
        const uint32_t dstSwizzle = bits(w1, 0, 12);
        const bool isSigned = bits(w1, 12, 1), isInteger = bits(w1, 13, 1), srfMode = bits(w1, 14, 1), rounded = bits(w1, 15, 1);
        const uint32_t format = bits(w1, 16, 6);
        const int32_t expAdjust = sbits(w1, 24, 6);
        const bool isMini = bits(w1, 30, 1), isPred = bits(w1, 31, 1);
        const uint32_t stride = bits(w2, 0, 8);
        const int32_t offset = sbits(w2, 8, 23);
        const bool predCond = bits(w2, 31, 1);
        const uint32_t slot = constIndex * 3 + constSel;

        if (isPred) { line("if (%sp0)", predCond ? "" : "!"); line("{"); indentation++; }

        info.vfetchSlots.insert(slot);
        {
            auto& st = info.vfetchStride[slot];
            if (!isMini && stride > st) st = stride;
            auto itMin = info.vfetchMinOff.find(slot);
            if (itMin == info.vfetchMinOff.end()) { info.vfetchMinOff[slot] = offset; info.vfetchMaxOff[slot] = offset + 4; }
            else { itMin->second = std::min(itMin->second, offset); info.vfetchMaxOff[slot] = std::max(info.vfetchMaxOff[slot], offset + 4); }
        }
        // Mini fetches reuse the address of the previous full fetch (vfetchAddr persists in the shader).
        if (!isMini)
        {
            // Address in dwords of the first element: base + index * stride
            line("{");
            indentation++;
            line("float xe_idxf = r[%u].%c;", srcReg, SW[srcSwizzle]);
            line("int xe_idx = (int)floor(xe_idxf%s);", rounded ? " + 0.5" : "");
            line("vfAddr = vfBase%u + (uint)xe_idx * %uu;", slot, stride);   // vfBase: dword offset (may wrap: the sum is exact inside the buffer)
            indentation--;
            line("}");
        }
        line("{");
        indentation++;
        // Load the words needed (up to 4) honoring the bounds of the vertex buffer and the endian swap of the constant.
        line("uint4 w = vfLoad4(vb%u, vfAddr + (uint)(%d), vfSize%u, vfEndian%u);", slot, offset, slot, slot);
        std::string res;
        const char* comp = "xyzw";
        switch (format)
        {
        case 6:   // 8_8_8_8
            if (isInteger) res = isSigned ? "float4(xe_sbfe(w.x,0,8), xe_sbfe(w.x,8,8), xe_sbfe(w.x,16,8), xe_sbfe(w.x,24,8))"
                                          : "float4(xe_ubfe(w.x,0,8), xe_ubfe(w.x,8,8), xe_ubfe(w.x,16,8), xe_ubfe(w.x,24,8))";
            else if (isSigned) res = srfMode ? "xe_snorm4(float4(xe_sbfe(w.x,0,8), xe_sbfe(w.x,8,8), xe_sbfe(w.x,16,8), xe_sbfe(w.x,24,8)), 127.0)"
                                             : "max(float4(xe_sbfe(w.x,0,8), xe_sbfe(w.x,8,8), xe_sbfe(w.x,16,8), xe_sbfe(w.x,24,8)) / 127.0, -1.0)";
            else res = "float4(xe_ubfe(w.x,0,8), xe_ubfe(w.x,8,8), xe_ubfe(w.x,16,8), xe_ubfe(w.x,24,8)) / 255.0";
            break;
        case 7:   // 2_10_10_10
            if (isInteger) res = isSigned ? "float4(xe_sbfe(w.x,0,10), xe_sbfe(w.x,10,10), xe_sbfe(w.x,20,10), xe_sbfe(w.x,30,2))"
                                          : "float4(xe_ubfe(w.x,0,10), xe_ubfe(w.x,10,10), xe_ubfe(w.x,20,10), xe_ubfe(w.x,30,2))";
            else if (isSigned) res = "max(float4(xe_sbfe(w.x,0,10) / 511.0, xe_sbfe(w.x,10,10) / 511.0, xe_sbfe(w.x,20,10) / 511.0, xe_sbfe(w.x,30,2)), -1.0)";
            else res = "float4(xe_ubfe(w.x,0,10) / 1023.0, xe_ubfe(w.x,10,10) / 1023.0, xe_ubfe(w.x,20,10) / 1023.0, xe_ubfe(w.x,30,2) / 3.0)";
            break;
        case 16:  // 10_11_11  (x: 11 bits, y: 11, z: 10)
            res = isSigned ? "float4(max(xe_sbfe(w.x,0,11)/1023.0,-1.0), max(xe_sbfe(w.x,11,11)/1023.0,-1.0), max(xe_sbfe(w.x,22,10)/511.0,-1.0), 0.0)"
                           : "float4(xe_ubfe(w.x,0,11)/2047.0, xe_ubfe(w.x,11,11)/2047.0, xe_ubfe(w.x,22,10)/1023.0, 0.0)";
            break;
        case 17:  // 11_11_10  (x: 10 bits, y: 11, z: 11)
            res = isSigned ? "float4(max(xe_sbfe(w.x,0,10)/511.0,-1.0), max(xe_sbfe(w.x,10,11)/1023.0,-1.0), max(xe_sbfe(w.x,21,11)/1023.0,-1.0), 0.0)"
                           : "float4(xe_ubfe(w.x,0,10)/1023.0, xe_ubfe(w.x,10,11)/2047.0, xe_ubfe(w.x,21,11)/2047.0, 0.0)";
            break;
        case 25:  // 16_16
            if (isInteger) res = isSigned ? "float4(xe_sbfe(w.x,0,16), xe_sbfe(w.x,16,16), 0, 0)" : "float4(xe_ubfe(w.x,0,16), xe_ubfe(w.x,16,16), 0, 0)";
            else if (isSigned) res = "float4(max(xe_sbfe(w.x,0,16)/32767.0,-1.0), max(xe_sbfe(w.x,16,16)/32767.0,-1.0), 0, 0)";
            else res = "float4(xe_ubfe(w.x,0,16)/65535.0, xe_ubfe(w.x,16,16)/65535.0, 0, 0)";
            break;
        case 26:  // 16_16_16_16
            if (isInteger) res = isSigned ? "float4(xe_sbfe(w.x,0,16), xe_sbfe(w.x,16,16), xe_sbfe(w.y,0,16), xe_sbfe(w.y,16,16))"
                                          : "float4(xe_ubfe(w.x,0,16), xe_ubfe(w.x,16,16), xe_ubfe(w.y,0,16), xe_ubfe(w.y,16,16))";
            else if (isSigned) res = "max(float4(xe_sbfe(w.x,0,16), xe_sbfe(w.x,16,16), xe_sbfe(w.y,0,16), xe_sbfe(w.y,16,16))/32767.0, -1.0)";
            else res = "float4(xe_ubfe(w.x,0,16), xe_ubfe(w.x,16,16), xe_ubfe(w.y,0,16), xe_ubfe(w.y,16,16))/65535.0";
            break;
        case 31:  // 16_16_FLOAT
            res = "float4(f16tof32(w.x & 0xFFFF), f16tof32(w.x >> 16), 0, 0)";
            break;
        case 32:  // 16_16_16_16_FLOAT
            res = "float4(f16tof32(w.x & 0xFFFF), f16tof32(w.x >> 16), f16tof32(w.y & 0xFFFF), f16tof32(w.y >> 16))";
            break;
        case 33: case 34: case 35:  // 32 / 32_32 / 32_32_32_32 integer
            res = isSigned ? "float4((int)w.x, (int)w.y, (int)w.z, (int)w.w)" : "float4(w.x, w.y, w.z, w.w)";
            if (!isInteger) res += isSigned ? " / 2147483647.0" : " / 4294967295.0";
            break;
        case 36: case 37: case 38: case 57:  // 32 float variants
            res = "asfloat(w)";
            break;
        default:
            res = "float4(0,0,0,0)";
            break;
        }
        // Components not provided by the format read as 0.
        int comps = 4;
        switch (format) { case 36: case 33: comps = 1; break; case 25: case 31: case 34: case 37: comps = 2; break; case 16: case 17: case 57: comps = 3; break; default: comps = 4; }
        line("float4 xe_v = %s;", res.c_str());
        if (comps < 4) for (int c = comps; c < 4; c++) line("xe_v.%c = 0.0;", comp[c]);
        if (expAdjust) line("xe_v *= %.9g;", ldexp(1.0, expAdjust));
        {
            // A fetch whose destination swizzle only holds constants / masked components writes nothing from the fetched data.
            const std::string dm = dstComponents(dstSwizzle, false);
            if (!dm.empty()) line("r[%u].%s = xe_v.%s;", dstReg, dm.c_str(), dstComponents(dstSwizzle, true).c_str());
        }
        indentation--;
        line("}");
        dstConst01(dstReg, dstSwizzle);
        if (isPred) { indentation--; line("}"); }
    }

    // ------------------------------------------------------------------------------------------------------
    // Texture fetch
    // ------------------------------------------------------------------------------------------------------
    void textureFetch(uint32_t w0, uint32_t w1, uint32_t w2)
    {
        const uint32_t opcode = bits(w0, 0, 5);
        const uint32_t srcReg = bits(w0, 5, 6), dstReg = bits(w0, 12, 6), constIndex = bits(w0, 20, 5);
        const bool denorm = bits(w0, 25, 1);
        const uint32_t srcSwizzle = bits(w0, 26, 6);
        const uint32_t dstSwizzle = bits(w1, 0, 12);
        const bool isPred = bits(w1, 31, 1);
        const uint32_t dimension = bits(w2, 14, 2);
        const int32_t offX = sbits(w2, 16, 5), offY = sbits(w2, 21, 5);
        const bool predCond = bits(w2, 31, 1);
        (void)denorm;

        if (opcode == 24)  // setTextureLod: lod register
        {
            line("xe_lod = r[%u].%c;", srcReg, SW[srcSwizzle & 3]);
            return;
        }
        if (opcode != 1) return;   // only plain texture fetch is supported (gradients/weights are ignored)

        if (isPred) { line("if (%sp0)", predCond ? "" : "!"); line("{"); indentation++; }
        info.textureSlots.insert(constIndex);
        info.dimOfSlot[constIndex] = dimension;

        std::string coords;
        const int cc = (dimension == 0) ? 1 : (dimension == 1) ? 2 : 3;   // 1D, 2D, 3D/cube
        for (int i = 0; i < cc; i++) coords += SW[(srcSwizzle >> (i * 2)) & 3];
        std::string src = "r[" + std::to_string(srcReg) + "]." + coords;
        std::string call;
        // dimension: 0 = 1D, 1 = 2D, 2 = 3D, 3 = cube
        if (dimension == 0) call = "xe_fetch1D(" + std::to_string(constIndex) + ", " + src + ")";
        else if (dimension == 1) {
            char o[96]; snprintf(o, sizeof o, ", float2(%g, %g)", offX * 0.5f, offY * 0.5f);
            call = "xe_fetch2D(" + std::to_string(constIndex) + ", " + src + o + ")";
        }
        else if (dimension == 2) call = "xe_fetch3D(" + std::to_string(constIndex) + ", " + src + ")";
        else call = "xe_fetchCube(" + std::to_string(constIndex) + ", " + src + ")";
        if (!dstComponents(dstSwizzle, false).empty()) line("r[%u].%s = %s.%s;", dstReg, dstComponents(dstSwizzle, false).c_str(), call.c_str(), dstComponents(dstSwizzle, true).c_str());
        dstConst01(dstReg, dstSwizzle);
        if (isPred) { indentation--; line("}"); }
    }

    // ------------------------------------------------------------------------------------------------------
    // ALU
    // ------------------------------------------------------------------------------------------------------
    struct Alu { uint32_t w0, w1, w2; };

    std::string aluOperand(const Alu& a, int operand)
    {
        enum { VECTOR_0, VECTOR_1, VECTOR_2, SCALAR_0, SCALAR_1, SCALAR_CONSTANT_0, SCALAR_CONSTANT_1 };
        const uint32_t vectorDest = bits(a.w0, 0, 6);
        (void)vectorDest;
        const bool absConstants = bits(a.w0, 7, 1);
        const uint32_t scalarOpcode = bits(a.w0, 26, 6);
        const uint32_t vectorOpcode = bits(a.w2, 24, 5);
        const uint32_t vectorWriteMask = bits(a.w0, 16, 4);
        const uint32_t src3Swz = bits(a.w1, 0, 8), src2Swz = bits(a.w1, 8, 8), src1Swz = bits(a.w1, 16, 8);
        const bool src3Neg = bits(a.w1, 24, 1), src2Neg = bits(a.w1, 25, 1), src1Neg = bits(a.w1, 26, 1);
        const bool constAddrRegRel = bits(a.w1, 29, 1), const0Rel = bits(a.w1, 31, 1);
        const uint32_t src3Reg = bits(a.w2, 0, 8), src2Reg = bits(a.w2, 8, 8), src1Reg = bits(a.w2, 16, 8);
        const bool src3Sel = bits(a.w2, 29, 1), src2Sel = bits(a.w2, 30, 1), src1Sel = bits(a.w2, 31, 1);

        uint32_t reg = 0, swizzle = 0; bool select = true, negate = false, abs = false;
        switch (operand)
        {
        case SCALAR_CONSTANT_0:
            reg = src3Reg; swizzle = src3Swz; select = false; negate = src3Neg; abs = absConstants; break;
        case SCALAR_CONSTANT_1:
            reg = (scalarOpcode & 1) | ((src3Sel ? 1u : 0u) << 1) | (src3Swz & 0x3C); swizzle = src3Swz; select = true; negate = src3Neg; abs = absConstants; break;
        default:
            switch (operand)
            {
            case VECTOR_0: reg = src1Reg; swizzle = src1Swz; select = src1Sel; negate = src1Neg; break;
            case VECTOR_1: reg = src2Reg; swizzle = src2Swz; select = src2Sel; negate = src2Neg; break;
            default: reg = src3Reg; swizzle = src3Swz; select = src3Sel; negate = src3Neg; break;   // VECTOR_2, SCALAR_0, SCALAR_1
            }
            if (select) { abs = (reg & 0x80) != 0; reg &= 0x3F; }
            else abs = absConstants;
            break;
        }

        char regs[96];
        if (select) snprintf(regs, sizeof regs, "r[%u]", reg);
        else if (const0Rel) snprintf(regs, sizeof regs, "c[min(%u + %s, 255)]", reg, constAddrRegRel ? "a0" : "aL");
        else snprintf(regs, sizeof regs, "c[%u]", reg);

        std::string result;
        if (negate) result += '-';
        if (abs) result += "abs(";
        result += regs;
        result += '.';
        switch (operand)
        {
        case VECTOR_0: case VECTOR_1: case VECTOR_2:
        {
            uint32_t mask;
            switch (vectorOpcode)
            {
            case V_DP2ADD: mask = (operand == VECTOR_2) ? 1 : 3; break;
            case V_DP3: mask = 7; break;
            case V_DP4: case V_MAX4: case V_CUBE: mask = 15; break;
            default: mask = vectorWriteMask != 0 ? vectorWriteMask : 1; break;
            }
            for (int i = 0; i < 4; i++)
                if ((mask >> i) & 1) result += SW[((swizzle >> (i * 2)) + i) & 3];
            break;
        }
        case SCALAR_0: case SCALAR_CONSTANT_0: result += SW[((swizzle >> 6) + 3) & 3]; break;
        case SCALAR_1: case SCALAR_CONSTANT_1: result += SW[swizzle & 3]; break;
        }
        if (abs) result += ")";
        return result;
    }

    void alu(const Alu& a)
    {
        enum { VECTOR_0, VECTOR_1, VECTOR_2, SCALAR_0, SCALAR_1, SCALAR_CONSTANT_0, SCALAR_CONSTANT_1 };
        const uint32_t vectorDest = bits(a.w0, 0, 6);
        const bool exportData = bits(a.w0, 15, 1);
        const uint32_t scalarDest = bits(a.w0, 8, 6);
        const bool scalarDestRel = bits(a.w0, 14, 1);
        uint32_t vectorWriteMask = bits(a.w0, 16, 4), scalarWriteMask = bits(a.w0, 20, 4);
        const bool vectorSat = bits(a.w0, 24, 1), scalarSat = bits(a.w0, 25, 1);
        const uint32_t scalarOpcode = bits(a.w0, 26, 6), vectorOpcode = bits(a.w2, 24, 5);
        const bool predCond = bits(a.w1, 27, 1), isPred = bits(a.w1, 28, 1);
        const uint32_t src1Reg = bits(a.w2, 16, 8);
        auto op = [&](int o) { return aluOperand(a, o); };

        if (isPred) { line("if (%sp0)", predCond ? "" : "!"); line("{"); indentation++; }

        // kill (vector)
        switch (vectorOpcode)
        {
        case V_KILL_EQ: line("if (any(%s == %s)) discard;", op(VECTOR_0).c_str(), op(VECTOR_1).c_str()); info.usesKill = true; break;
        case V_KILL_GT: line("if (any(%s > %s)) discard;", op(VECTOR_0).c_str(), op(VECTOR_1).c_str()); info.usesKill = true; break;
        case V_KILL_GE: line("if (any(%s >= %s)) discard;", op(VECTOR_0).c_str(), op(VECTOR_1).c_str()); info.usesKill = true; break;
        case V_KILL_NE: line("if (any(%s != %s)) discard;", op(VECTOR_0).c_str(), op(VECTOR_1).c_str()); info.usesKill = true; break;
        }

        std::string exportReg;
        if (exportData)
        {
            if (p.isPixel)
            {
                switch (vectorDest)
                {
                case 0: exportReg = "oC0"; info.colorOutputsWritten |= 1; break;
                case 1: exportReg = "oC1"; info.colorOutputsWritten |= 2; break;
                case 2: exportReg = "oC2"; info.colorOutputsWritten |= 4; break;
                case 3: exportReg = "oC3"; info.colorOutputsWritten |= 8; break;
                case 61: exportReg = "oDepth"; info.writesDepth = true; break;
                }
            }
            else
            {
                if (vectorDest == 62) exportReg = "oPos";
                else if (vectorDest == 63) exportReg = "oMisc";
                else if (vectorDest < 16) { char b[32]; snprintf(b, sizeof b, "oT[%u]", vectorDest); exportReg = b; info.interpolatorsWritten |= 1u << vectorDest; }
                else exportReg = "oMisc";   // memexport etc. are not supported
            }
        }

        if (vectorOpcode >= V_SETP_EQ_PUSH && vectorOpcode <= V_SETP_GE_PUSH)
        {
            const char* cmp = vectorOpcode == V_SETP_EQ_PUSH ? "==" : vectorOpcode == V_SETP_NE_PUSH ? "!=" : vectorOpcode == V_SETP_GT_PUSH ? ">" : ">=";
            line("p0 = %s == 0.0 && %s %s 0.0;", op(VECTOR_0).c_str(), op(VECTOR_1).c_str(), cmp);
        }
        else if (vectorOpcode == V_MAXA)
        {
            line("a0 = (int)clamp(floor((%s).w + 0.5), -256.0, 255.0);", op(VECTOR_0).c_str());
        }

        if (exportData) vectorWriteMask &= ~scalarWriteMask;
        if (vectorWriteMask != 0)
        {
            std::string dest = !exportReg.empty() ? exportReg : "r[" + std::to_string(vectorDest) + "]";
            dest += '.';
            for (int i = 0; i < 4; i++) if ((vectorWriteMask >> i) & 1) dest += SW[i];
            std::string expr;
            char t[1024];
            auto a0s = op(VECTOR_0), a1s = op(VECTOR_1), a2s = op(VECTOR_2);
            switch (vectorOpcode)
            {
            case V_ADD: expr = a0s + " + " + a1s; break;
            case V_MUL: expr = a0s + " * " + a1s; break;
            case V_MAX: case V_MAXA: expr = "max(" + a0s + ", " + a1s + ")"; break;
            case V_MIN: expr = "min(" + a0s + ", " + a1s + ")"; break;
            case V_SEQ: expr = "(" + a0s + " == " + a1s + ")"; break;
            case V_SGT: expr = "(" + a0s + " > " + a1s + ")"; break;
            case V_SGE: expr = "(" + a0s + " >= " + a1s + ")"; break;
            case V_SNE: expr = "(" + a0s + " != " + a1s + ")"; break;
            case V_FRC: expr = "frac(" + a0s + ")"; break;
            case V_TRUNC: expr = "trunc(" + a0s + ")"; break;
            case V_FLOOR: expr = "floor(" + a0s + ")"; break;
            case V_MAD: expr = a0s + " * " + a1s + " + " + a2s; break;
            case V_CNDEQ: expr = "((" + a0s + " == 0.0) ? " + a1s + " : " + a2s + ")"; break;
            case V_CNDGE: expr = "((" + a0s + " >= 0.0) ? " + a1s + " : " + a2s + ")"; break;
            case V_CNDGT: expr = "((" + a0s + " > 0.0) ? " + a1s + " : " + a2s + ")"; break;
            case V_DP4: case V_DP3: expr = "dot(" + a0s + ", " + a1s + ")"; break;
            case V_DP2ADD: expr = "dot(" + a0s + ", " + a1s + ") + " + a2s; break;
            case V_CUBE: expr = "xe_cube(" + a0s + ")"; break;
            case V_MAX4: expr = "xe_max4(" + a0s + ")"; break;
            case V_SETP_EQ_PUSH: case V_SETP_NE_PUSH: case V_SETP_GT_PUSH: case V_SETP_GE_PUSH: expr = "(p0 ? 0.0 : " + a0s + " + 1.0)"; break;
            case V_KILL_EQ: expr = "(any(" + a0s + " == " + a1s + ") ? 1.0 : 0.0)"; break;
            case V_KILL_GT: expr = "(any(" + a0s + " > " + a1s + ") ? 1.0 : 0.0)"; break;
            case V_KILL_GE: expr = "(any(" + a0s + " >= " + a1s + ") ? 1.0 : 0.0)"; break;
            case V_KILL_NE: expr = "(any(" + a0s + " != " + a1s + ") ? 1.0 : 0.0)"; break;
            case V_DST: expr = "xe_dst(" + a0s + ", " + a1s + ")"; break;
            default: expr = "0.0"; break;
            }
            if (vectorSat) expr = "saturate(" + expr + ")";
            line("%s = %s;", dest.c_str(), expr.c_str());
        }

        if (scalarOpcode != S_RETAIN_PREV)
        {
            if (scalarOpcode >= S_SETP_EQ && scalarOpcode <= S_SETP_RSTR)
            {
                std::string s0 = op(SCALAR_0), e;
                switch (scalarOpcode)
                {
                case S_SETP_EQ: e = s0 + " == 0.0"; break;
                case S_SETP_NE: e = s0 + " != 0.0"; break;
                case S_SETP_GT: e = s0 + " > 0.0"; break;
                case S_SETP_GE: e = s0 + " >= 0.0"; break;
                case S_SETP_INV: e = s0 + " == 1.0"; break;
                case S_SETP_POP: e = s0 + " - 1.0 <= 0.0"; break;
                case S_SETP_CLR: e = "false"; break;
                case S_SETP_RSTR: e = s0 + " == 0.0"; break;
                }
                line("p0 = %s;", e.c_str());
            }
            std::string s0 = op(SCALAR_0), s1 = op(SCALAR_1), c0 = op(SCALAR_CONSTANT_0), c1 = op(SCALAR_CONSTANT_1), e;
            switch (scalarOpcode)
            {
            case S_ADDS: e = s0 + " + " + s1; break;
            case S_ADDS_PREV: e = s0 + " + ps"; break;
            case S_MULS: e = s0 + " * " + s1; break;
            case S_MULS_PREV: case S_MULS_PREV2: e = s0 + " * ps"; break;
            case S_MAXS: case S_MAXAS: case S_MAXASF: e = "max(" + s0 + ", " + s1 + ")"; break;
            case S_MINS: e = "min(" + s0 + ", " + s1 + ")"; break;
            case S_SEQS: e = "(" + s0 + " == 0.0 ? 1.0 : 0.0)"; break;
            case S_SGTS: e = "(" + s0 + " > 0.0 ? 1.0 : 0.0)"; break;
            case S_SGES: e = "(" + s0 + " >= 0.0 ? 1.0 : 0.0)"; break;
            case S_SNES: e = "(" + s0 + " != 0.0 ? 1.0 : 0.0)"; break;
            case S_FRCS: e = "frac(" + s0 + ")"; break;
            case S_TRUNCS: e = "trunc(" + s0 + ")"; break;
            case S_FLOORS: e = "floor(" + s0 + ")"; break;
            case S_EXP: e = "exp2(" + s0 + ")"; break;
            case S_LOGC: case S_LOG: e = "clamp(log2(" + s0 + "), -3.402823466e+38, 3.402823466e+38)"; break;
            case S_RCPC: case S_RCPF: case S_RCP: e = "clamp(rcp(" + s0 + "), -3.402823466e+38, 3.402823466e+38)"; break;
            case S_RSQC: case S_RSQF: case S_RSQ: e = "clamp(rsqrt(" + s0 + "), -3.402823466e+38, 3.402823466e+38)"; break;
            case S_SUBS: e = s0 + " - " + s1; break;
            case S_SUBS_PREV: e = s0 + " - ps"; break;
            case S_SETP_EQ: case S_SETP_NE: case S_SETP_GT: case S_SETP_GE: e = "(p0 ? 0.0 : 1.0)"; break;
            case S_SETP_INV: e = "(" + s0 + " == 0.0 ? 1.0 : " + s0 + ")"; break;
            case S_SETP_POP: e = "(p0 ? 0.0 : (" + s0 + " - 1.0))"; break;
            case S_SETP_CLR: e = "3.402823466e+38"; break;
            case S_SETP_RSTR: e = "(p0 ? 0.0 : " + s0 + ")"; break;
            case S_KILLS_EQ: e = "(" + s0 + " == 0.0 ? 1.0 : 0.0)"; break;
            case S_KILLS_GT: e = "(" + s0 + " > 0.0 ? 1.0 : 0.0)"; break;
            case S_KILLS_GE: e = "(" + s0 + " >= 0.0 ? 1.0 : 0.0)"; break;
            case S_KILLS_NE: e = "(" + s0 + " != 0.0 ? 1.0 : 0.0)"; break;
            case S_KILLS_ONE: e = "(" + s0 + " == 1.0 ? 1.0 : 0.0)"; break;
            case S_SQRT: e = "sqrt(" + s0 + ")"; break;
            case S_MULSC0: case S_MULSC1: e = c0 + " * " + c1; break;
            case S_ADDSC0: case S_ADDSC1: e = c0 + " + " + c1; break;
            case S_SUBSC0: case S_SUBSC1: e = c0 + " - " + c1; break;
            case S_SIN: e = "sin(" + s0 + ")"; break;
            case S_COS: e = "cos(" + s0 + ")"; break;
            default: e = "0.0"; break;
            }
            if (scalarSat) e = "saturate(" + e + ")";
            line("ps = %s;", e.c_str());
            if (scalarOpcode == S_MAXAS) line("a0 = (int)clamp(floor(%s + 0.5), -256.0, 255.0);", s0.c_str());
            else if (scalarOpcode == S_MAXASF) line("a0 = (int)clamp(floor(%s), -256.0, 255.0);", s0.c_str());
        }

        if (exportData) scalarWriteMask &= ~bits(a.w0, 16, 4);
        if (scalarWriteMask != 0)
        {
            std::string dest = !exportReg.empty() ? exportReg : "r[" + std::to_string(scalarDest) + "]";
            dest += '.';
            for (int i = 0; i < 4; i++) if ((scalarWriteMask >> i) & 1) dest += SW[i];
            line("%s = ps;", dest.c_str());
        }

        if (exportData && !exportReg.empty())
        {
            const uint32_t vMask = bits(a.w0, 16, 4), sMask = bits(a.w0, 20, 4);
            const uint32_t zeroMask = scalarDestRel ? (0xF & ~(vMask | sMask)) : 0;
            const uint32_t oneMask = vMask & sMask;
            for (int i = 0; i < 4; i++)
            {
                if ((zeroMask >> i) & 1) line("%s.%c = 0.0;", exportReg.c_str(), SW[i]);
                else if ((oneMask >> i) & 1) line("%s.%c = 1.0;", exportReg.c_str(), SW[i]);
            }
        }
        if (scalarOpcode >= S_KILLS_EQ && scalarOpcode <= S_KILLS_ONE) { line("if (ps != 0.0) discard;"); info.usesKill = true; }

        if (isPred) { indentation--; line("}"); }
    }

    // ------------------------------------------------------------------------------------------------------
    // Control flow
    // ------------------------------------------------------------------------------------------------------
    struct CfInstr { uint64_t v; };
    static uint32_t cfOpcode(uint64_t v) { return (uint32_t)((v >> 44) & 0xF); }

    void execBlock(uint32_t address, uint32_t count, uint32_t sequence)
    {
        const uint32_t* ic = code + address * 3;
        for (uint32_t i = 0; i < count; i++, ic += 3)
        {
            if ((address + i) * 3 + 2 >= dwordCount) break;
            const uint32_t w0 = ic[0], w1 = ic[1], w2 = ic[2];
            if (sequence & 1)
            {
                if (bits(w0, 0, 5) == 0) vertexFetch(w0, w1, w2);
                else textureFetch(w0, w1, w2);
            }
            else alu({ w0, w1, w2 });
            sequence >>= 2;
        }
    }

    // Decode the exec header: returns address/count/sequence; kind: 0 plain, 1 bool cond, 2 pred cond
    struct ExecInfo { uint32_t address, count, sequence; int kind; bool cond; uint32_t boolAddr; bool isEnd; bool valid; };
    ExecInfo decodeExec(uint64_t v)
    {
        ExecInfo e{}; e.valid = true;
        const uint32_t op = cfOpcode(v);
        e.address = (uint32_t)(v & 0xFFF);
        e.count = (uint32_t)((v >> 12) & 7);
        e.sequence = (uint32_t)((v >> 16) & 0xFFF);
        switch (op)
        {
        case CF_EXEC: e.kind = 0; break;
        case CF_EXEC_END: e.kind = 0; e.isEnd = true; break;
        case CF_COND_EXEC: case CF_COND_EXEC_END: case CF_COND_EXEC_PRED_CLEAN: case CF_COND_EXEC_PRED_CLEAN_END:
            e.kind = 1; e.boolAddr = (uint32_t)((v >> 34) & 0xFF); e.cond = (v >> 42) & 1; e.isEnd = (op == CF_COND_EXEC_END || op == CF_COND_EXEC_PRED_CLEAN_END); break;
        case CF_COND_EXEC_PRED: case CF_COND_EXEC_PRED_END:
            e.kind = 2; e.cond = (v >> 42) & 1; e.isEnd = (op == CF_COND_EXEC_PRED_END); break;
        default: e.valid = false; break;
        }
        return e;
    }

    std::string boolTest(uint32_t addr)
    {
        char b[96];
        snprintf(b, sizeof b, "xe_bool(%u)", addr);   // pixel shaders encode absolute addresses (128..255)
        return b;
    }

    void emitExec(const ExecInfo& e, bool machine)
    {
        std::string cond;
        if (e.kind == 1) cond = boolTest(e.boolAddr) + (e.cond ? "" : " == false");
        else if (e.kind == 2) cond = std::string(e.cond ? "" : "!") + "p0";
        if (!cond.empty()) { line("if (%s)", cond.c_str()); line("{"); indentation++; }
        execBlock(e.address, e.count, e.sequence);
        if (e.isEnd) line(machine ? "break;" : "break;");
        if (!cond.empty()) { indentation--; line("}"); }
    }

    void controlFlow()
    {
        // Control-flow instructions are packed two per 3 dwords at the start of the program, followed by the
        // exec data. Exec addresses are in units of 3 dwords, so the cf area ends at the lowest exec address.
        std::vector<uint64_t> cf;
        uint32_t minAddr = 0xFFFFFFFF;
        bool simple = true;
        for (uint32_t k = 0; k < minAddr && k * 3 + 2 < dwordCount; k++)
        {
            const uint32_t* c = code + k * 3;
            const uint64_t a = (uint64_t)c[0] | ((uint64_t)(c[1] & 0xFFFF) << 32);
            const uint64_t b = ((uint64_t)(c[1] >> 16) | ((uint64_t)c[2] << 16)) & 0xFFFFFFFFFFFFull;
            for (uint64_t v : { a, b })
            {
                cf.push_back(v);
                const uint32_t op = cfOpcode(v);
                if (op == CF_EXEC || op == CF_EXEC_END || op == CF_COND_EXEC || op == CF_COND_EXEC_END || op == CF_COND_EXEC_PRED ||
                    op == CF_COND_EXEC_PRED_END || op == CF_COND_EXEC_PRED_CLEAN || op == CF_COND_EXEC_PRED_CLEAN_END)
                    minAddr = std::min<uint32_t>(minAddr, (uint32_t)(v & 0xFFF));
                else if (op == CF_LOOP_START || op == CF_LOOP_END || op == CF_COND_CALL || op == CF_RETURN || op == CF_COND_JMP)
                    simple = false;
            }
        }

        // Generated body
        if (simple)
        {
            line("do");
            line("{");
            indentation++;
            for (uint64_t v : cf)
            {
                const ExecInfo e = decodeExec(v);
                if (e.valid) emitExec(e, false);
            }
            indentation--;
            line("} while (false);");
            return;
        }

        line("uint pc = 0; uint xe_lit[32];");
        line("[unroll] for (int xl = 0; xl < 32; xl++) xe_lit[xl] = 0;");
        line("uint xe_cs[8]; uint xe_sp = 0;");
        line("[loop] while (true)");
        line("{");
        indentation++;
        for (uint32_t i = 0; i < cf.size(); i++)
        {
            const uint64_t v = cf[i];
            const uint32_t op = cfOpcode(v);
            line("if (pc == %u)", i);
            line("{");
            indentation++;
            line("pc = %u;", i + 1);
            switch (op)
            {
            case CF_EXEC: case CF_EXEC_END: case CF_COND_EXEC: case CF_COND_EXEC_END: case CF_COND_EXEC_PRED: case CF_COND_EXEC_PRED_END:
            case CF_COND_EXEC_PRED_CLEAN: case CF_COND_EXEC_PRED_CLEAN_END:
                emitExec(decodeExec(v), true);
                break;
            case CF_LOOP_START:
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
            }
            case CF_COND_CALL:
            {
                const uint32_t addr = (uint32_t)(v & 0x1FFF);
                const bool uncond = (v >> 13) & 1, predicated = (v >> 14) & 1, cond = (v >> 42) & 1;
                const uint32_t boolAddr = (uint32_t)((v >> 34) & 0xFF);
                std::string c = uncond ? "true" : predicated ? std::string(cond ? "" : "!") + "p0" : boolTest(boolAddr) + (cond ? "" : " == false");
                line("if (%s) { xe_cs[xe_sp++ & 7] = pc; pc = %u; continue; }", c.c_str(), addr);
                break;
            }
            case CF_RETURN:
                line("if (xe_sp > 0) { pc = xe_cs[--xe_sp & 7]; continue; }");
                break;
            case CF_COND_JMP:
            {
                const uint32_t addr = (uint32_t)(v & 0x1FFF);
                const bool uncond = (v >> 13) & 1, predicated = (v >> 14) & 1, cond = (v >> 42) & 1;
                const uint32_t boolAddr = (uint32_t)((v >> 34) & 0xFF);
                const char* cont = addr > i ? "" : " continue;";   // forward jump: later blocks just do not match pc
                if (uncond) line("{ pc = %u;%s }", addr, cont);
                else
                {
                    std::string c = predicated ? std::string(cond ? "" : "!") + "p0" : boolTest(boolAddr) + (cond ? "" : " == false");
                    line("if (%s) { pc = %u;%s }", c.c_str(), addr, cont);
                }
                break;
            }
            default: break;   // nop / alloc / mark
            }
            indentation--;
            line("}");
        }
        line("break;");
        indentation--;
        line("}");
    }
};

const char* kHlslPrelude = R"HLSL(
// ---- Xenos shader support code ----
float xe_ubfe(uint v, uint o, uint w) { return (float)((v >> o) & ((1u << w) - 1u)); }
float xe_sbfe(uint v, uint o, uint w) { return (float)((int)(v << (32u - o - w)) >> (32u - w)); }
float4 xe_snorm4(float4 v, float s) { return max(v / s, -1.0); }
float xe_max4(float4 v) { return max(max(v.x, v.y), max(v.z, v.w)); }
float4 xe_dst(float4 a, float4 b) { return float4(1.0, a.y * b.y, a.z, b.w); }
// Xenos CUBEv: s0 = operand 0 after its 4-lane swizzle; (x, y, z) = (s0.z, s0.w, s0.x). Result = (tc, sc, 2*ma, faceId) with faceId in D3D order
// (+X, -X, +Y, -Y, +Z, -Z).
float4 xe_cube(float4 s0)
{
    float cx = s0.z, cy = s0.w, cz = s0.x;
    float ax = abs(cx), ay = abs(cy), az = abs(cz);
    float4 r;
    if (az >= ax && az >= ay) { bool neg = cz < 0.0; r = float4(-cy, neg ? -cx : cx, 2.0 * cz, neg ? 5.0 : 4.0); }
    else if (ay >= ax)        { bool neg = cy < 0.0; r = float4(neg ? -cz : cz, cx, 2.0 * cy, neg ? 3.0 : 2.0); }
    else                      { bool neg = cx < 0.0; r = float4(-cy, neg ? cz : -cz, 2.0 * cx, neg ? 1.0 : 0.0); }
    return r;
}
// tfetchCube takes (S', T', face) with S' = sc/(2|ma|) + 1.5; turn it back into a direction for a D3D11 TextureCube.
static const float3 kCubeN[6] = { float3( 1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0,-1, 0), float3(0, 0, 1), float3(0, 0,-1) };
static const float3 kCubeU[6] = { float3( 0, 0,-1), float3( 0, 0, 1), float3(1, 0, 0), float3(1, 0, 0), float3(1, 0, 0), float3(-1,0, 0) };
static const float3 kCubeV[6] = { float3( 0,-1, 0), float3( 0,-1, 0), float3(0, 0, 1), float3(0, 0,-1), float3(0,-1, 0), float3(0,-1, 0) };
float3 xe_cubeDir(float3 stf)
{
    uint f = (uint)clamp(stf.z, 0.0, 5.0);
    return kCubeN[f] + (stf.x * 2.0 - 3.0) * kCubeU[f] + (stf.y * 2.0 - 3.0) * kCubeV[f];
}
)HLSL";

}  // namespace

std::string TranslateXenosShader(const uint32_t* code, uint32_t dwordCount, const XTranslateParams& params, XShaderInfo& info)
{
    info = XShaderInfo{};
    info.isPixel = params.isPixel;
    Translator t(code, dwordCount, params, info);

    // First pass: translate the body (this records the used resources).
    t.out.clear();
    t.indentation = 1;
    t.controlFlow();
    const std::string body = t.out;

    std::string s;
    s.reserve(body.size() + 8192);
    s += "// Xenos ";
    s += params.isPixel ? "pixel" : "vertex";
    s += " shader\n";
    s += kHlslPrelude;
    if (params.vulkan) s += params.isPixel ? "[[vk::binding(1, 1)]] " : "[[vk::binding(0, 1)]] ";
    s += "cbuffer XeConstants : register(b0)\n{\n";
    s += "\tfloat4 c[256];\n\tuint4 bconst[2];\n\tuint4 lconst[8];\n\tuint4 texswz[8];\n\tfloat4 sys0;   // vertex: xy = position scale, zw = position bias (x/y)\n";
    s += "\tfloat4 sys1;   // vertex: x = z scale, y = z bias, z = flags (1 = vertex already divided by w); pixel: alpha test\n";
    s += "\tfloat4 sys2;   // pixel: x = alpha func, y = alpha ref\n\tuint4 vfmeta[96];  // per vertex fetch slot: x = byte base address, y = size in dwords, z = endian\n";
    s += "};\n";
    if (!params.isPixel && !params.vulkan) s += "cbuffer XeDraw : register(b1) { uint4 xe_draw; };   // x = first index of this draw in xe_indices\n";
    s += "bool xe_bool(uint i) { return ((bconst[i >> 7][(i >> 5) & 3] >> (i & 31)) & 1u) != 0u; }\n";
    s += "uint xe_loopCount(uint i) { return lconst[(i >> 2) & 7][i & 3] & 0xFFu; }\n";
    s += "uint xe_loopStart(uint i) { return (lconst[(i >> 2) & 7][i & 3] >> 8) & 0xFFu; }\n";
    s += "uint xe_loopStep(uint i) { return (lconst[(i >> 2) & 7][i & 3] >> 16) & 0xFFu; }\n";
    s += "int xe_loopStepS(uint i) { return ((int)(lconst[(i >> 2) & 7][i & 3] << 8)) >> 24; }\n";
    s += "float4 xe_applyswz(float4 t, uint slot)\n{\n\tuint swz = texswz[slot >> 2][slot & 3];\n\tint xe_e = (int)((swz >> 12) & 63u); if (xe_e >= 32) xe_e -= 64;\n\tif (xe_e != 0) t.xyz *= exp2((float)xe_e);\n\tfloat v[6] = { t.x, t.y, t.z, t.w, 0.0, 1.0 };\n";
    s += "\treturn float4(v[min(swz & 7, 5u)], v[min((swz >> 3) & 7, 5u)], v[min((swz >> 6) & 7, 5u)], v[min((swz >> 9) & 7, 5u)]);\n}\n";

    // Textures + samplers (Vulkan: one combined image sampler per used slot)
    uint32_t texIndex = 0;
    for (uint32_t slot : info.textureSlots)
    {
        const uint32_t d = info.dimOfSlot[slot];
        const char* type = d == 2 ? "Texture3D<float4>" : d == 3 ? "TextureCube<float4>" : "Texture2D<float4>";
        char b[384];
        if (params.vulkan)
        {
            const uint32_t binding = (params.isPixel ? kVkPsTexBase : kVkVsTexBase) + texIndex++;
            snprintf(b, sizeof b, "[[vk::combinedImageSampler]] [[vk::binding(%u, 0)]] %s tex%u;\n[[vk::combinedImageSampler]] [[vk::binding(%u, 0)]] SamplerState smp%u;\n", binding, type, slot, binding, slot);
        }
        else snprintf(b, sizeof b, "%s tex%u : register(t%u);\nSamplerState smp%u : register(s%u);\n", type, slot, slot, slot, slot & 15);
        s += b;
    }
    s += "static float xe_lod = 0.0;\n";
    // 1D textures are bound as 2D (height 1); sample with y = 0.5.
    bool has1D = false, has2D = false, has3D = false, hasCube = false;
    for (uint32_t slot : info.textureSlots)
    {
        const uint32_t d = info.dimOfSlot[slot];
        if (d == 0) has1D = true; else if (d == 1) has2D = true; else if (d == 2) has3D = true; else hasCube = true;
    }
    if (has1D)
    {
        s += "float4 xe_fetch1D(uint slot, float u)\n{\n\tswitch (slot)\n\t{\n";
        for (uint32_t slot : info.textureSlots)
            if (info.dimOfSlot[slot] == 0)
            {
                char b[512];
                snprintf(b, sizeof b, "\tcase %u: return xe_applyswz(tex%u.%s(smp%u, float2(u, 0.5)%s), %u);\n", slot, slot, params.isPixel ? "Sample" : "SampleLevel", slot & 15, params.isPixel ? "" : ", 0", slot);
                s += b;
            }
        s += "\t}\n\treturn float4(0,0,0,0);\n}\n";
    }
    if (has2D)
    {
        s += "float4 xe_fetch2D(uint slot, float2 uv, float2 off)\n{\n\tswitch (slot)\n\t{\n";
        for (uint32_t slot : info.textureSlots)
            if (info.dimOfSlot[slot] == 1)
            {
                char b[512];
                snprintf(b, sizeof b, "\tcase %u: return xe_applyswz(tex%u.%s(smp%u, uv%s), %u);\n", slot, slot, params.isPixel ? "Sample" : "SampleLevel", slot & 15, params.isPixel ? "" : ", 0", slot);
                s += b;
            }
        s += "\t}\n\treturn float4(0,0,0,0);\n}\n";
    }
    if (has3D)
    {
        s += "float4 xe_fetch3D(uint slot, float3 uv)\n{\n\tswitch (slot)\n\t{\n";
        for (uint32_t slot : info.textureSlots)
            if (info.dimOfSlot[slot] == 2)
            {
                char b[512];
                snprintf(b, sizeof b, "\tcase %u: return xe_applyswz(tex%u.%s(smp%u, uv%s), %u);\n", slot, slot, params.isPixel ? "Sample" : "SampleLevel", slot & 15, params.isPixel ? "" : ", 0", slot);
                s += b;
            }
        s += "\t}\n\treturn float4(0,0,0,0);\n}\n";
    }
    if (hasCube)
    {
        s += "float4 xe_fetchCube(uint slot, float3 uv)\n{\n\tswitch (slot)\n\t{\n";
        for (uint32_t slot : info.textureSlots)
            if (info.dimOfSlot[slot] == 3)
            {
                char b[512];
                snprintf(b, sizeof b, "\tcase %u: return xe_applyswz(tex%u.%s(smp%u, xe_cubeDir(uv)%s), %u);\n", slot, slot, params.isPixel ? "Sample" : "SampleLevel", slot & 15, params.isPixel ? "" : ", 0", slot);
                s += b;
            }
        s += "\t}\n\treturn float4(0,0,0,0);\n}\n";
    }

    // Vertex buffers
    if (!params.isPixel)
    {
        s += params.vulkan ? "[[vk::binding(2, 1)]] StructuredBuffer<uint> xe_indices;\n" : "StructuredBuffer<uint> xe_indices : register(t31);\n";
        uint32_t vbIndex = 0;
        for (uint32_t slot : info.vfetchSlots)
        {
            char b[256];
            if (params.vulkan) snprintf(b, sizeof b, "[[vk::binding(%u, 0)]] ByteAddressBuffer vb%u;\n", kVkVbBase + vbIndex++, slot);
            else snprintf(b, sizeof b, "ByteAddressBuffer vb%u : register(t%u);\n", slot, 32 + slot);
            s += b;
            snprintf(b, sizeof b, "#define vfBase%u vfmeta[%u].x\n#define vfSize%u vfmeta[%u].y\n#define vfEndian%u vfmeta[%u].z\n", slot, slot, slot, slot, slot, slot);
            s += b;
        }
        s += "static uint vfAddr = 0;\n";
        s += "uint xe_swap(uint v, uint e)\n{\n\tif (e == 1 || e == 2) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);\n\tif (e == 2 || e == 3) v = (v >> 16) | (v << 16);\n\treturn v;\n}\n";
        s += "uint4 vfLoad4(ByteAddressBuffer b, uint dwordAddr, uint size, uint endian)\n{\n\tuint4 r = 0;\n";
        s += "\tuint byteAddr = dwordAddr * 4;\n\tuint bufSize; b.GetDimensions(bufSize);\n";
        s += "\t[unroll] for (uint i = 0; i < 4; i++) { uint a = byteAddr + i * 4; r[i] = (a + 4 <= bufSize) ? xe_swap(b.Load(a), endian) : 0u; }\n\treturn r;\n}\n";
    }

    // main
    if (params.isPixel)
    {
        s += "void main(float4 iPos : SV_Position";
        for (uint32_t i = 0; i < 16; i++) { char b[64]; snprintf(b, sizeof b, ",\n\tfloat4 iT%u : TEXCOORD%u", i, i); s += b; }
        s += ",\n\tbool iFace : SV_IsFrontFace";
        for (int i = 0; i < 4; i++) { char b[96]; snprintf(b, sizeof b, ",\n\tout float4 oC%d : SV_Target%d", i, i); s += b; }
        if (info.writesDepth) s += ",\n\tout float oDepth : SV_Depth";
        s += ")\n{\n";
        s += "\tfloat4 r[64];\n\t[unroll] for (int xi = 0; xi < 64; xi++) r[xi] = float4(0,0,0,0);\n";
        s += "\toC0 = float4(0,0,0,0); oC1 = float4(0,0,0,0); oC2 = float4(0,0,0,0); oC3 = float4(0,0,0,0);\n";
        if (info.writesDepth) s += "\toDepth = iPos.z;\n";
        s += "\tfloat4 iT[16] = { iT0, iT1, iT2, iT3, iT4, iT5, iT6, iT7, iT8, iT9, iT10, iT11, iT12, iT13, iT14, iT15 };\n";
        char b[160];
        snprintf(b, sizeof b, "\t[unroll] for (int ii = 0; ii < %u; ii++) r[ii] = iT[ii];\n", std::min<uint32_t>(params.psInterpolatorCount, 16));
        s += b;
        if (params.psParamGenReg >= 0)
        {
            snprintf(b, sizeof b, "\tr[%d] = float4(floor(iPos.xy), iFace ? 1.0 : -1.0, 0.0);\n", params.psParamGenReg);
            s += b;
        }
    }
    else
    {
        // Vulkan: the body is xe_main, wrapped by a main() that returns a struct (DXC accepts the PointSize built-in only on a struct member).
        s += params.vulkan ? "void xe_main(uint vid : SV_VertexID,\n\tout float4 oPosOut : SV_Position" : "void main(uint vid : SV_VertexID,\n\tout float4 oPosOut : SV_Position";
        for (uint32_t i = 0; i < 16; i++) { char b[64]; snprintf(b, sizeof b, ",\n\tout float4 oT%u : TEXCOORD%u", i, i); s += b; }
        s += ")\n{\n";
        s += "\tfloat4 r[64];\n\t[unroll] for (int xi = 0; xi < 64; xi++) r[xi] = float4(0,0,0,0);\n";
        s += "\tfloat4 oPos = float4(0,0,0,1);\n\tfloat4 oMisc = float4(0,0,0,0);\n\tfloat4 oT[16];\n\t[unroll] for (int xj = 0; xj < 16; xj++) oT[xj] = float4(0,0,0,0);\n";
        s += params.vulkan ? "\tuint xe_vidx = xe_indices[vid];\n" : "\tuint xe_vidx = xe_indices[vid + xe_draw.x];\n";
        s += "\tr[0] = float4((float)xe_vidx, 0, 0, 0);\n";
    }
    s += "\tint a0 = 0; int aL = 0; bool p0 = false; float ps = 0.0;\n";
    s += body;

    if (params.isPixel)
    {
        // Alpha test (RB_COLORCONTROL) emulated here: sys2.x = function (0 never .. 7 always, 8 = disabled), sys2.y = reference.
        s += "\t{\n\t\tuint af = (uint)sys2.x;\n";
        s += "\t\tif (af < 8) {\n\t\t\tfloat a = oC0.w, ref = sys2.y; bool apass = true;\n";
        s += "\t\t\tif (af == 0) apass = false; else if (af == 1) apass = a < ref; else if (af == 2) apass = a == ref; else if (af == 3) apass = a <= ref;\n";
        s += "\t\t\telse if (af == 4) apass = a > ref; else if (af == 5) apass = a != ref; else if (af == 6) apass = a >= ref;\n";
        s += "\t\t\tif (!apass) discard;\n\t\t}\n\t}\n";
    }
    else
    {
        // Map guest clip space to D3D clip space (see the host viewport setup).
        s += "\tif (sys1.z != 0.0) { oPos.w = 1.0; }\n";
        s += "\toPosOut = float4(oPos.x * sys0.x + oPos.w * sys0.z, oPos.y * sys0.y + oPos.w * sys0.w, oPos.z * sys1.x + oPos.w * sys1.y, oPos.w);\n";
        for (uint32_t i = 0; i < 16; i++) { char b[64]; snprintf(b, sizeof b, "\toT%u = oT[%u];\n", i, i); s += b; }
    }
    s += "}\n";
    if (params.vulkan && !params.isPixel)
    {
        // Point lists need a point size in Vulkan (1, like Direct3D 11).
        s += "struct XeVSOut\n{\n\tfloat4 pos : SV_Position;\n";
        for (uint32_t i = 0; i < 16; i++) { char b[64]; snprintf(b, sizeof b, "\tfloat4 t%u : TEXCOORD%u;\n", i, i); s += b; }
        s += "\t[[vk::builtin(\"PointSize\")]] float psize : PSIZE;\n};\n";
        s += "XeVSOut main(uint vid : SV_VertexID)\n{\n\tXeVSOut o;\n\txe_main(vid, o.pos";
        for (uint32_t i = 0; i < 16; i++) { char b[16]; snprintf(b, sizeof b, ", o.t%u", i); s += b; }
        s += ");\n\to.psize = 1.0;\n\treturn o;\n}\n";
    }
    return s;
}

std::string BuildRectListGeometryShader()
{
    std::string s;
    s += "struct VSOut\n{\n\tfloat4 pos : SV_Position;\n";
    for (int i = 0; i < 16; i++) { char b[64]; snprintf(b, sizeof b, "\tfloat4 t%d : TEXCOORD%d;\n", i, i); s += b; }
    s += "};\n";
    s += "VSOut xe_combine(VSOut a, VSOut b, VSOut c) // a + b - c ... (v1 + v2 - v0)\n{\n\tVSOut o;\n\to.pos = a.pos + b.pos - c.pos;\n";
    for (int i = 0; i < 16; i++) { char b[96]; snprintf(b, sizeof b, "\to.t%d = a.t%d + b.t%d - c.t%d;\n", i, i, i, i); s += b; }
    s += "\treturn o;\n}\n";
    // The game passes the three corners of a rectangle in different orders (TL,TR,BL for blits but TL,TR,BR for clears and resolves), so the corner
    // with the right angle is the vertex opposite the longest edge; the fourth corner is the sum of the other two minus that corner.
    s += "[maxvertexcount(4)]\nvoid main(triangle VSOut v[3], inout TriangleStream<VSOut> stream)\n{\n";
    s += "\tfloat2 p0 = v[0].pos.xy / (v[0].pos.w != 0.0 ? v[0].pos.w : 1.0);\n";
    s += "\tfloat2 p1 = v[1].pos.xy / (v[1].pos.w != 0.0 ? v[1].pos.w : 1.0);\n";
    s += "\tfloat2 p2 = v[2].pos.xy / (v[2].pos.w != 0.0 ? v[2].pos.w : 1.0);\n";
    s += "\tfloat l01 = dot(p1 - p0, p1 - p0), l12 = dot(p2 - p1, p2 - p1), l20 = dot(p0 - p2, p0 - p2);\n";
    s += "\tuint c = (l12 >= l01 && l12 >= l20) ? 0u : (l20 >= l01 ? 1u : 2u);\n";
    s += "\tVSOut vc = v[c], va = v[(c + 1u) % 3u], vb = v[(c + 2u) % 3u];\n";
    s += "\tVSOut vd = xe_combine(va, vb, vc);\n";
    s += "\tstream.Append(vc);\n\tstream.Append(va);\n\tstream.Append(vb);\n\tstream.Append(vd);\n}\n";
    return s;
}
