p = r'C:\Users\日南\Documents\txr-itc-recomp\tools\XenonRecomp\XenonRecomp\recompiler.cpp'
s = open(p, encoding='utf-8').read()


def ins_before(marker, text):
    global s
    assert s.count(marker) == 1, marker
    s = s.replace(marker, text + marker)


# ---- scalar: mulhd/mulhdu + CR logic ----
ins_before("    case PPC_INST_MULHW:\n", r'''    case PPC_INST_MULHD:
        println("\t{}.s64 = int64_t((__int128(int64_t({}.s64)) * __int128(int64_t({}.s64))) >> 64);", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        break;

    case PPC_INST_MULHDU:
        println("\t{}.u64 = uint64_t((unsigned __int128({}.u64) * unsigned __int128({}.u64)) >> 64);", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        break;

    case PPC_INST_CRAND:
    case PPC_INST_CRANDC:
    case PPC_INST_CROR:
    case PPC_INST_CRORC:
    case PPC_INST_CRXOR:
    case PPC_INST_CREQV:
    case PPC_INST_CRNAND:
    case PPC_INST_CRNOR:
    {
        static constexpr const char* crFields[] = { "lt", "gt", "eq", "so" };
        auto crBit = [&](uint32_t bit) { return fmt::format("{}.{}", cr(bit / 4), crFields[bit % 4]); };
        const auto d = crBit(insn.operands[0]);
        const auto a = crBit(insn.operands[1]);
        const auto b = crBit(insn.operands[2]);
        switch (insn.opcode->id)
        {
        case PPC_INST_CRAND:  println("\t{} = {} & {};", d, a, b); break;
        case PPC_INST_CRANDC: println("\t{} = {} & ({} ^ 1);", d, a, b); break;
        case PPC_INST_CROR:   println("\t{} = {} | {};", d, a, b); break;
        case PPC_INST_CRORC:  println("\t{} = {} | ({} ^ 1);", d, a, b); break;
        case PPC_INST_CRXOR:  println("\t{} = {} ^ {};", d, a, b); break;
        case PPC_INST_CREQV:  println("\t{} = ({} ^ {}) ^ 1;", d, a, b); break;
        case PPC_INST_CRNAND: println("\t{} = ({} & {}) ^ 1;", d, a, b); break;
        case PPC_INST_CRNOR:  println("\t{} = ({} | {}) ^ 1;", d, a, b); break;
        }
        break;
    }

''')


def simple(case, simd, ty, rec=False, swap=False, outty=None):
    outty = outty or ty
    a, b = ('2', '1') if swap else ('1', '2')
    t = (
        '    case PPC_INST_%s:\n'
        '        println("\\tsimde_mm_store_si128((simde__m128i*){}.%s, %s(simde_mm_load_si128((simde__m128i*){}.%s), simde_mm_load_si128((simde__m128i*){}.%s)));", v(insn.operands[0]), v(insn.operands[%s]), v(insn.operands[%s]));\n'
    ) % (case, outty, simd, ty, ty, a, b)
    if rec:
        t += (
            "        if (strchr(insn.opcode->name, '.'))\n"
            '            println("\\t{}.setFromMask(simde_mm_load_si128((simde__m128i*){}.u8), 0xFFFF);", cr(6), v(insn.operands[0]));\n'
        )
    t += '        break;\n\n'
    return t


vec = ''
vec += simple('VADDSBS', 'simde_mm_adds_epi8', 's8')
vec += simple('VSUBSHS', 'simde_mm_subs_epi16', 's16')
vec += simple('VSUBUBM', 'simde_mm_sub_epi8', 'u8')
vec += simple('VMAXSH', 'simde_mm_max_epi16', 's16')
vec += simple('VMINSH', 'simde_mm_min_epi16', 's16')
vec += simple('VAVGUH', 'simde_mm_avg_epu16', 'u16')
vec += simple('VCMPEQUH', 'simde_mm_cmpeq_epi16', 'u16', rec=True, outty='u8')
vec += simple('VCMPGTSH', 'simde_mm_cmpgt_epi16', 's16', rec=True, outty='u8')
vec += simple('VCMPGTSW', 'simde_mm_cmpgt_epi32', 's32', rec=True, outty='u8')
vec += simple('VPKSWSS', 'simde_mm_packs_epi32', 's32', swap=True, outty='s16')
vec += simple('VPKSHSS', 'simde_mm_packs_epi16', 's16', swap=True, outty='s8')
vec += simple('VPKSWUS', 'simde_mm_packus_epi32', 's32', swap=True, outty='u16')

vec += r'''    case PPC_INST_VADDSWS:
        for (size_t i = 0; i < 4; i++)
        {
            println("\t{}.s64 = int64_t({}.s32[{}]) + int64_t({}.s32[{}]);", temp(), v(insn.operands[1]), i, v(insn.operands[2]), i);
            println("\t{}.s32[{}] = {}.s64 > INT_MAX ? INT_MAX : {}.s64 < INT_MIN ? INT_MIN : {}.s64;", v(insn.operands[0]), i, temp(), temp(), temp());
        }
        break;

    case PPC_INST_VPKUHUS:
    case PPC_INST_VPKUHUS128:
        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_packus_epi16(simde_mm_min_epu16(simde_mm_load_si128((simde__m128i*){}.u16), simde_mm_set1_epi16(255)), simde_mm_min_epu16(simde_mm_load_si128((simde__m128i*){}.u16), simde_mm_set1_epi16(255))));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;

    case PPC_INST_VSPLTISH:
        println("\tsimde_mm_store_si128((simde__m128i*){}.u16, simde_mm_set1_epi16(short(0x{:X})));", v(insn.operands[0]), insn.operands[1] & 0xFFFF);
        break;

    case PPC_INST_VSLH:
        for (size_t i = 0; i < 8; i++)
            println("\t{}.u16[{}] = {}.u16[{}] << ({}.u16[{}] & 0xF);", v(insn.operands[0]), i, v(insn.operands[1]), i, v(insn.operands[2]), i);
        break;

    case PPC_INST_VSRH:
        for (size_t i = 0; i < 8; i++)
            println("\t{}.u16[{}] = {}.u16[{}] >> ({}.u16[{}] & 0xF);", v(insn.operands[0]), i, v(insn.operands[1]), i, v(insn.operands[2]), i);
        break;

    case PPC_INST_VSRAH:
        for (size_t i = 0; i < 8; i++)
            println("\t{}.s16[{}] = {}.s16[{}] >> ({}.u16[{}] & 0xF);", v(insn.operands[0]), i, v(insn.operands[1]), i, v(insn.operands[2]), i);
        break;

    case PPC_INST_VSRAB:
        for (size_t i = 0; i < 16; i++)
            println("\t{}.s8[{}] = {}.s8[{}] >> ({}.u8[{}] & 0x7);", v(insn.operands[0]), i, v(insn.operands[1]), i, v(insn.operands[2]), i);
        break;

    case PPC_INST_VRLH:
        for (size_t i = 0; i < 8; i++)
            println("\t{0}.u16[{1}] = uint16_t(({2}.u16[{1}] << ({3}.u16[{1}] & 0xF)) | ({2}.u16[{1}] >> ((16 - ({3}.u16[{1}] & 0xF)) & 0xF)));", v(insn.operands[0]), i, v(insn.operands[1]), v(insn.operands[2]));
        break;

    case PPC_INST_VCTUXS:
    case PPC_INST_VCFPUXWS128:
        printSetFlushMode(true);
        for (size_t i = 0; i < 4; i++)
        {
            println("\t{}.f64 = double({}.f32[{}]) * {};", temp(), v(insn.operands[1]), i, double(uint64_t(1) << insn.operands[2]));
            println("\t{}.u32[{}] = !({}.f64 > 0.0) ? 0u : {}.f64 >= 4294967296.0 ? 0xFFFFFFFFu : uint32_t({}.f64);", v(insn.operands[0]), i, temp(), temp(), temp());
        }
        break;

'''

for old, new in (
    ("    case PPC_INST_VANDC128:\n", "    case PPC_INST_VANDC:\n    case PPC_INST_VANDC128:\n"),
    ("    case PPC_INST_VSEL:\n", "    case PPC_INST_VSEL:\n    case PPC_INST_VSEL128:\n"),
):
    assert s.count(old) == 1, old
    s = s.replace(old, new)

ins_before("    case PPC_INST_VSLB:\n", vec)
open(p, 'w', encoding='utf-8').write(s)
print("patched")
