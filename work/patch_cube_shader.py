p = 'C:/itc-work/runtime/src/xgpu_shader.cpp'
s = open(p, encoding='utf-8').read()


def rep(old, new, what):
    global s
    if old not in s:
        raise SystemExit('missing: ' + what)
    s = s.replace(old, new, 1)


# 1. operand 0 of CUBE is read with all four lanes
rep('            case V_DP4: case V_MAX4: mask = 15; break;', '            case V_DP4: case V_MAX4: case V_CUBE: mask = 15; break;', 'mask')

# 2. the instruction itself uses operand 0 with its real swizzle
rep('            case V_CUBE: snprintf(t, sizeof t, "xe_cube(r[%u])", src1Reg & 0x3F); expr = t; break;',
    '            case V_CUBE: expr = "xe_cube(" + a0s + ")"; break;', 'cube expr')

# 3. prelude: exact cube instruction + direction reconstruction for the cube fetch
start = s.index('float4 xe_cube(float4 s)')
end = s.index(')HLSL";', start)
new_cube = r'''// Xenos CUBEv: s0 = operand 0 after its 4-lane swizzle; (x, y, z) = (s0.z, s0.w, s0.x). Result = (tc, sc, 2*ma, faceId) with faceId in D3D order
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
'''
s = s[:start] + new_cube + s[end:]

# 4. cube fetch helper: sample with the reconstructed direction
old = r'''        s += "float4 xe_fetchCube(uint slot, float3 uv)\n{\n\tswitch (slot)\n\t{\n";
        for (uint32_t slot : info.textureSlots)
            if (info.dimOfSlot[slot] == 3)
            {
                char b[512];
                snprintf(b, sizeof b, "\tcase %u: return xe_applyswz(tex%u.%s(smp%u, uv%s), %u);\n", slot, slot, params.isPixel ? "Sample" : "SampleLevel", slot & 15, params.isPixel ? "" : ", 0", slot);'''
new = r'''        s += "float4 xe_fetchCube(uint slot, float3 uv)\n{\n\tswitch (slot)\n\t{\n";
        for (uint32_t slot : info.textureSlots)
            if (info.dimOfSlot[slot] == 3)
            {
                char b[512];
                snprintf(b, sizeof b, "\tcase %u: return xe_applyswz(tex%u.%s(smp%u, xe_cubeDir(uv)%s), %u);\n", slot, slot, params.isPixel ? "Sample" : "SampleLevel", slot & 15, params.isPixel ? "" : ", 0", slot);'''
rep(old, new, 'fetchCube block')
open(p, 'w', encoding='utf-8').write(s)
print('ok')
