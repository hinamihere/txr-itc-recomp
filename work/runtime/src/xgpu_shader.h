// Xenos (Xbox 360 GPU) shader microcode -> HLSL (Shader Model 5) translation.
// Instruction decoding and the translation strategy follow hedge-dev/XenosRecomp (MIT) and Xenia (BSD-3).
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

struct XShaderInfo
{
    bool isPixel = false;
    std::map<uint32_t, uint32_t> vfetchStride;          // per vertex fetch slot: stride in dwords (largest used)
    std::map<uint32_t, int32_t> vfetchMinOff, vfetchMaxOff;   // per slot: smallest offset / largest (offset + 4) in dwords used by the fetches
    std::set<uint32_t> vfetchSlots;                     // vertex fetch constant slots (0..95) used by a vertex shader
    std::set<uint32_t> textureSlots;                    // texture fetch constants (0..31) used
    std::vector<uint8_t> textureDims;                   // parallel to textureSlots iteration order: 0=1D/2D, 1=3D, 2=cube
    uint32_t dimOfSlot[32] = {};                        // dimension (xenos TextureDimension) per used slot
    uint32_t interpolatorsWritten = 0;                  // bitmask of VS interpolators exported
    uint32_t colorOutputsWritten = 0;                   // bitmask of PS color outputs written
    bool writesDepth = false;
    bool usesKill = false;
};

struct XTranslateParams
{
    bool isPixel = false;
    uint32_t psInterpolatorCount = 0;   // PS: number of interpolators loaded into r0.. (vs_export_count + 1)
    int psParamGenReg = -1;             // PS: register that receives screen position (-1 = none)
    bool vulkan = false;                // HLSL for DXC -> SPIR-V with the descriptor bindings below (instead of Direct3D 11 registers)
};

// Vulkan descriptor bindings of the translated shaders. Set 0 (push descriptors, rewritten per draw): the textures of the vertex shader, its vertex fetch buffers
// and the textures of the pixel shader, each numbered in ascending slot order (the i-th used texture slot of the pixel shader is binding kVkPsTexBase + i).
// Set 1 (static): binding 0 = vertex shader constants, 1 = pixel shader constants (dynamic uniform buffers), 2 = the converted index lists. The index of the
// first vertex of a draw is the firstVertex of vkCmdDraw (SV_VertexID includes it).
constexpr uint32_t kVkVsTexBase = 0, kVkVsTexMax = 4, kVkVbBase = 4, kVkVbMax = 12, kVkPsTexBase = 16, kVkPsTexMax = 16, kVkSet0Bindings = 32;

// code: microcode dwords already converted to host numeric order (rd32 of the big-endian guest memory).
std::string TranslateXenosShader(const uint32_t* code, uint32_t dwordCount, const XTranslateParams& params, XShaderInfo& info);

// Geometry shader that turns a 3-vertex rectangle (RectangleList primitive) into a 4-vertex triangle strip.
std::string BuildRectListGeometryShader();
