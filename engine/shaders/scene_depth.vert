#version 450
#extension GL_GOOGLE_include_directive : require

#include "common/scene_uniforms.glsl"
#include "common/scene_instances.glsl"
#include "common/foliage_wind.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec2 vUv;
layout(location = 1) flat out vec4 vMaterialFlags;
layout(location = 2) flat out uvec4 vTextureIndices;

void main() {
    SceneInstance instance = sceneInstance();
    vec3 localPosition = applyFoliageWind(inPosition, instance);
    vec4 world = instance.model * vec4(localPosition, 1.0);
    gl_Position = scene.viewProjection * world;
    vUv = inUv;
    vMaterialFlags = instance.materialFlags;
    vTextureIndices = instance.textureIndices;
}
