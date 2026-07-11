#version 450
#extension GL_GOOGLE_include_directive : require

#include "common/scene_uniforms.glsl"

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

float hash21(vec2 value) {
    return fract(sin(dot(value, vec2(127.1, 311.7))) * 43758.5453);
}

float noise2(vec2 coordinate) {
    vec2 cell = floor(coordinate);
    vec2 local = fract(coordinate);
    local = local * local * (3.0 - 2.0 * local);
    float a = hash21(cell);
    float b = hash21(cell + vec2(1.0, 0.0));
    float c = hash21(cell + vec2(0.0, 1.0));
    float d = hash21(cell + vec2(1.0));
    return mix(mix(a, b, local.x), mix(c, d, local.x), local.y);
}

float cloudNoise(vec2 coordinate) {
    float result = 0.0;
    float amplitude = 0.5333333;
    mat2 rotation = mat2(0.80, -0.60, 0.60, 0.80);
    for (int octave = 0; octave < 4; ++octave) {
        result += noise2(coordinate) * amplitude;
        coordinate = rotation * coordinate * 2.03 + vec2(13.7, -9.2);
        amplitude *= 0.5;
    }
    return result;
}

void main() {
    vec2 ndc = vUv * 2.0 - 1.0;
    float tanHalfFov = scene.cameraForwardTanHalfFov.w;
    vec3 ray = normalize(
        scene.cameraForwardTanHalfFov.xyz +
        scene.cameraRightAspect.xyz * ndc.x * scene.cameraRightAspect.w *
            tanHalfFov +
        scene.cameraUpAtmosphere.xyz * ndc.y * tanHalfFov);

    float elevation = clamp(ray.y * 0.5 + 0.5, 0.0, 1.0);
    float horizon = exp(-abs(ray.y) * 7.5);
    vec3 horizonColor = vec3(0.40, 0.57, 0.76) * 1.35;
    vec3 zenithColor = vec3(0.035, 0.13, 0.31) * 1.7;
    vec3 groundHaze = vec3(0.20, 0.24, 0.27);
    vec3 sky = ray.y >= 0.0
        ? mix(horizonColor, zenithColor, pow(elevation, 0.65))
        : mix(groundHaze, horizonColor * 0.72, horizon);

    vec3 sunDirection = normalize(-scene.lightDirection.xyz);
    float sunAlignment = max(dot(ray, sunDirection), 0.0);
    float sunDisk = smoothstep(0.99945, 0.99986, sunAlignment);
    float sunGlow = pow(sunAlignment, 96.0);
    sky += scene.lightColor.rgb * (sunDisk * 14.0 + sunGlow * 0.65);

    if (ray.y > 0.025) {
        float cloudDistance = 1500.0 / ray.y;
        vec2 cloudPosition =
            (scene.cameraPositionTime.xz + ray.xz * cloudDistance) / 4200.0;
        cloudPosition += vec2(scene.cameraPositionTime.w * 0.0035,
                              scene.cameraPositionTime.w * 0.0012);
        float coverage = cloudNoise(cloudPosition * 3.2);
        float cloud = smoothstep(0.53, 0.76, coverage) *
                      smoothstep(0.02, 0.16, ray.y);
        float light = mix(0.55, 1.25,
                          clamp(dot(sunDirection, vec3(0.0, 1.0, 0.0)) *
                                    0.5 + 0.5,
                                0.0, 1.0));
        vec3 cloudColor = mix(vec3(0.38, 0.43, 0.49),
                              vec3(1.0, 0.96, 0.88) * light,
                              smoothstep(0.55, 0.85, coverage));
        sky = mix(sky, cloudColor, cloud * 0.72);
    }

    sky = mix(sky, horizonColor, horizon * 0.16);
    outColor = vec4(max(sky, vec3(0.0)), 1.0);
}
