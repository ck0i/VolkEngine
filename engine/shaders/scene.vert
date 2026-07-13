#version 450
#extension GL_GOOGLE_include_directive : require

#include "common/scene_uniforms.glsl"
#include "common/scene_instances.glsl"
#include "common/foliage_wind.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inTangent;
const uint MATERIAL_TEXTURE_NORMAL = 2U;
const uint MATERIAL_HAIR = 5U;


layout(location = 0) out vec3 vWorldPosition;
layout(location = 1) out vec3 vWorldNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) flat out vec4 vAlbedoRoughness;
layout(location = 4) flat out vec4 vEmissiveMetallic;
layout(location = 5) flat out vec4 vMaterialFlags;
layout(location = 6) out vec4 vWorldTangent;
layout(location = 7) flat out uvec4 vTextureIndices;

void main() {
    uint instanceIndex = sceneInstanceIndex();
    mat4 model = instanceData.instances[instanceIndex].model;
    uvec4 textureIndices =
        instanceData.instances[instanceIndex].textureIndices;
    uint materialBits = textureIndices.w;
    vec3 localPosition =
        applyFoliageWind(inPosition, model, materialBits);
    vec4 world = model * vec4(localPosition, 1.0);
    vWorldPosition = world.xyz;
    if ((materialBits & (1U << 31U)) != 0U) {
        vWorldNormal = normalize(mat3(model) * inNormal);
    } else {
        mat3 normalMatrix = mat3(
            instanceData.instances[instanceIndex].normalMatrix0.xyz,
            instanceData.instances[instanceIndex].normalMatrix1.xyz,
            instanceData.instances[instanceIndex].normalMatrix2.xyz);
        vWorldNormal = normalize(normalMatrix * inNormal);
    }
    bool needsTangent =
        (materialBits & MATERIAL_TEXTURE_NORMAL) != 0U ||
        ((materialBits >> 3U) & 15U) == MATERIAL_HAIR;
    vWorldTangent = needsTangent
        ? vec4(
              normalize(mat3(model) * inTangent.xyz),
              inTangent.w *
                  instanceData.instances[instanceIndex].normalMatrix0.w)
        : vec4(0.0);
    vUv = inUv;
    vAlbedoRoughness =
        instanceData.instances[instanceIndex].albedoRoughness;
    vEmissiveMetallic =
        instanceData.instances[instanceIndex].emissiveMetallic;
    vMaterialFlags =
        instanceData.instances[instanceIndex].materialFlags;
    vTextureIndices = textureIndices;
    gl_Position = scene.viewProjection * world;
}
