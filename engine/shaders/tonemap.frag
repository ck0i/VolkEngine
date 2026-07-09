#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrColor;

layout(push_constant) uniform TonemapData {
    float exposure;
    uint applySrgbOetf;
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

vec3 srgbEncode(vec3 linearRgb) {
    linearRgb = clamp(linearRgb, vec3(0.0), vec3(1.0));
    const vec3 cutoff = vec3(0.0031308);
    vec3 linearSegment = linearRgb * 12.92;
    vec3 gammaSegment = 1.055 * pow(linearRgb, vec3(1.0 / 2.4)) - 0.055;
    return mix(linearSegment, gammaSegment, step(cutoff, linearRgb));
}

void main() {
    vec3 hdr = texture(hdrColor, vUv).rgb * tonemap.exposure;
    vec3 mapped = acesApprox(hdr);
    if (tonemap.applySrgbOetf != 0U) {
        mapped = srgbEncode(mapped);
    }
    outColor = vec4(mapped, 1.0);
}
