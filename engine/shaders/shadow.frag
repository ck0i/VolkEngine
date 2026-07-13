#version 450
#if VE_BINDLESS
#extension GL_EXT_nonuniform_qualifier : require
#endif

#if VE_BINDLESS
layout(set = 0, binding = 3) uniform sampler2D materialTextures[];
#else
layout(set = 0, binding = 3) uniform sampler2D materialTextures[3];
#endif

layout(location = 0) in vec2 vUv;
layout(location = 1) flat in vec4 vMaterialFlags;
layout(location = 2) flat in uvec4 vTextureIndices;

void main() {
    uint packedFeatures = floatBitsToUint(vMaterialFlags.x);
    if ((packedFeatures & 1U) == 0U) return;
    float alpha = 1.0;
    if ((vTextureIndices.w & 1U) != 0U) {
#if VE_BINDLESS
        alpha = texture(materialTextures[
            nonuniformEXT(vTextureIndices.x)], vUv).a;
#else
        alpha = texture(materialTextures[0], vUv).a;
#endif
    }
    float alphaCutoff = float(packedFeatures >> 8U) / 32767.5;
    if (alpha < alphaCutoff) discard;
}
