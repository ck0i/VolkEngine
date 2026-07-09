#version 450
#extension GL_GOOGLE_include_directive : require

#include "common/scene_uniforms.glsl"
#include "common/scene_instances.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;


layout(location = 0) out vec3 vWorldPosition;
layout(location = 1) out vec3 vWorldNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec4 vAlbedoRoughness;
layout(location = 4) out vec4 vEmissiveMetallic;
layout(location = 5) flat out vec4 vMaterialFlags;

void main() {
    SceneInstance instance = instanceData.instances[gl_InstanceIndex];
    vec4 world = instance.model * vec4(inPosition, 1.0);
    vWorldPosition = world.xyz;
    vWorldNormal = normalize(mat3(instance.model) * inNormal);
    vUv = inUv;
    vAlbedoRoughness = instance.albedoRoughness;
    vEmissiveMetallic = instance.emissiveMetallic;
    vMaterialFlags = instance.materialFlags;
    gl_Position = scene.viewProjection * world;
}
