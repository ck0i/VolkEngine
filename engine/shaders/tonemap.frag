#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrColor;

layout(push_constant) uniform TonemapData {
    float exposure;
} tonemap;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

vec3 acesApprox(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(hdrColor, vUv).rgb * tonemap.exposure;
    vec3 mapped = acesApprox(hdr);
    mapped = pow(mapped, vec3(1.0 / 2.2));
    outColor = vec4(mapped, 1.0);
}
