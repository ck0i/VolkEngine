#version 450
#extension GL_GOOGLE_include_directive : require

#include "common/scene_uniforms.glsl"
#include "common/scene_instances.glsl"

layout(location = 0) in vec3 inPosition;

void main() {
    SceneInstance instance = sceneInstance();
    vec4 world = instance.model * vec4(inPosition, 1.0);
    gl_Position = scene.viewProjection * world;
}
