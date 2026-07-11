#version 450
#extension GL_GOOGLE_include_directive : require

#include "common/scene_uniforms.glsl"
#include "common/scene_instances.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inTangent;


layout(location = 0) out vec3 vWorldPosition;
layout(location = 1) out vec3 vWorldNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) flat out vec4 vAlbedoRoughness;
layout(location = 4) flat out vec4 vEmissiveMetallic;
layout(location = 5) flat out vec4 vMaterialFlags;
layout(location = 6) out vec4 vWorldTangent;
layout(location = 7) flat out uvec4 vTextureIndices;

void main() {
    SceneInstance instance = sceneInstance();
    vec4 world = instance.model * vec4(inPosition, 1.0);
    vWorldPosition = world.xyz;
    mat3 normalMatrix = mat3(instance.normalMatrix0.xyz, instance.normalMatrix1.xyz, instance.normalMatrix2.xyz);
    vWorldNormal = normalize(normalMatrix * inNormal);
    vWorldTangent = vec4(normalize(mat3(instance.model) * inTangent.xyz), inTangent.w * instance.normalMatrix0.w);
    vUv = inUv;
    vAlbedoRoughness = instance.albedoRoughness;
    vEmissiveMetallic = instance.emissiveMetallic;
    vMaterialFlags = instance.materialFlags;
    vTextureIndices = instance.textureIndices;
    gl_Position = scene.viewProjection * world;
}
