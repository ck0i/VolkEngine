const float PI = 3.14159265359;

float distributionGGX(vec3 n, vec3 h, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float ndoth = max(dot(n, h), 0.0);
    float ndoth2 = ndoth * ndoth;
    float denom = ndoth2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 0.0001);
}

float geometrySchlickGGX(float ndotv, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return ndotv / max(ndotv * (1.0 - k) + k, 0.0001);
}

float geometrySmith(float ndotv, float ndotl, float roughness) {
    return geometrySchlickGGX(ndotv, roughness) * geometrySchlickGGX(ndotl, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    float f = clamp(1.0 - cosTheta, 0.0, 1.0);
    float f2 = f * f;
    float f5 = f2 * f2 * f;
    return f0 + (1.0 - f0) * f5;
}

float gridMask(vec2 uv) {
    vec2 width = max(fwidth(uv), vec2(0.0001));
    vec2 cell = abs(fract(uv - 0.5) - 0.5) / width;
    return 1.0 - clamp(min(cell.x, cell.y), 0.0, 1.0);
}

vec3 environmentDiffuse(vec3 normal) {
    float skyWeight = clamp(normal.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 ground = scene.ambientGroundColor.rgb * scene.ambientGroundColor.a;
    vec3 sky = scene.ambientSkyColor.rgb * scene.ambientSkyColor.a;
    return mix(ground, sky, skyWeight);
}
