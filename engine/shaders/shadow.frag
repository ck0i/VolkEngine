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
    const uint maskedMaterial = 1U;
    uint materialClass = uint(round(clamp(vMaterialFlags.y, 0.0, 7.0)));
    if (materialClass != maskedMaterial) return;
    float alpha = 1.0;
    if ((vTextureIndices.w & 1U) != 0U) {
#if VE_BINDLESS
        alpha = texture(materialTextures[
            nonuniformEXT(vTextureIndices.x)], vUv).a;
#else
        alpha = texture(materialTextures[0], vUv).a;
#endif
    }
    if (alpha < vMaterialFlags.z) discard;
}
