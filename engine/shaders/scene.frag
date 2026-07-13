#version 450
#extension GL_GOOGLE_include_directive : require
#if VE_BINDLESS
#extension GL_EXT_nonuniform_qualifier : require
#endif

#include "common/scene_uniforms.glsl"
#include "common/pbr.glsl"
#include "common/lighting.glsl"

#if VE_BINDLESS
layout(set = 0, binding = 3) uniform sampler2D materialTextures[];
#else
layout(set = 0, binding = 3) uniform sampler2D materialTextures[3];
#endif


layout(location = 0) in vec3 vWorldPosition;
layout(location = 1) in vec3 vWorldNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) flat in vec4 vAlbedoRoughness;
layout(location = 4) flat in vec4 vEmissiveMetallic;
layout(location = 5) flat in vec4 vMaterialFlags;
layout(location = 6) in vec4 vWorldTangent;
layout(location = 7) flat in uvec4 vTextureIndices;

layout(location = 0) out vec4 outColor;


vec4 sampleMaterialTexture(uint role) {
#if VE_BINDLESS
    return texture(materialTextures[nonuniformEXT(vTextureIndices[role])], vUv);
#else
    if (role == 0U) return texture(materialTextures[0], vUv);
    if (role == 1U) return texture(materialTextures[1], vUv);
    return texture(materialTextures[2], vUv);
#endif
}
bool hasMaterialTexture(uint bit) {
    return (vTextureIndices.w & bit) != 0U;
}
float sampleShadowView(uint viewIndex, vec3 worldPosition, vec3 n, vec3 l,
                       bool orthographic) {
    vec4 clip =
        lighting.shadowViewProjection[viewIndex] * vec4(worldPosition, 1.0);
    if (!orthographic && clip.w <= 0.0) return 1.0;
    vec3 projected = orthographic ? clip.xyz : clip.xyz / clip.w;
    vec2 tileUv = projected.xy * 0.5 + 0.5;
    if (any(lessThan(tileUv, vec2(0.0))) ||
        any(greaterThan(tileUv, vec2(1.0))) ||
        projected.z <= 0.0 || projected.z >= 1.0) {
        return 1.0;
    }
    vec4 atlasRect = lighting.shadowUvScaleBias[viewIndex];
    vec2 atlasUv = tileUv * atlasRect.xy + atlasRect.zw;
    float slopeBias =
        mix(0.00008, 0.0008, 1.0 - clamp(dot(n, l), 0.0, 1.0));
    return texture(shadowAtlas,
                   vec3(atlasUv, min(projected.z + slopeBias, 1.0)));
}

float directionalShadowVisibility(vec3 worldPosition, vec3 n, vec3 l) {
    if (lighting.directionalParameters.x == 0U) return 1.0;
    float viewDepth = 1.0 / gl_FragCoord.w;
    if (viewDepth <= 0.0 || viewDepth > lighting.cascadeSplits.w)
        return 1.0;
    uint cascade = viewDepth <= lighting.cascadeSplits.x ? 0U
        : viewDepth <= lighting.cascadeSplits.y ? 1U
        : viewDepth <= lighting.cascadeSplits.z ? 2U
                                                : 3U;
    float visibility =
        sampleShadowView(cascade, worldPosition, n, l, true);
    float split = lighting.cascadeSplits[cascade];
    float blendStart = uintBitsToFloat(
        lighting.directionalParameters[cascade + 1U]);
    float blend = smoothstep(blendStart, split, viewDepth);
    if (blend > 0.0) {
        visibility = cascade < 2U
            ? mix(visibility,
                  sampleShadowView(
                      cascade + 1U, worldPosition, n, l, true),
                  blend)
            : mix(visibility, 1.0, blend);
    }
    return visibility;
}

float localShadowVisibility(LocalLight light, vec3 worldPosition,
                            vec3 n, vec3 l) {
    if (light.parameters.w == 0U) return 1.0;
    uint viewIndex = light.parameters.w - 1U;
    if (viewIndex >= lighting.counts.w) return 1.0;
    return sampleShadowView(viewIndex, worldPosition, n, l, false);
}



const uint MATERIAL_STANDARD = 0U;
const uint MATERIAL_MASKED = 1U;
const uint MATERIAL_CLEAR_COAT = 2U;
const uint MATERIAL_FOLIAGE = 3U;
const uint MATERIAL_SKIN = 4U;
const uint MATERIAL_HAIR = 5U;
const uint MATERIAL_CLOTH = 6U;
const uint MATERIAL_EMISSIVE = 7U;
const uint MATERIAL_LANDSCAPE = 8U;
const uint MATERIAL_WATER = 9U;
const uint MATERIAL_FEATURE_ALPHA_MASK = 1U;
const uint MATERIAL_FEATURE_GROUND_GRID = 4U;

uint materialClass() {
    return (vTextureIndices.w >> 3U) & 15U;
}
uint packedMaterialFeatures() {
    return floatBitsToUint(vMaterialFlags.x);
}
bool hasMaterialFeature(uint feature) {
    return (packedMaterialFeatures() & feature) != 0U;
}
float materialAlphaCutoff() {
    return float(packedMaterialFeatures() >> 8U) / 32767.5;
}
float landscapeVariation(vec2 coordinate) {
    vec2 cell = floor(coordinate);
    vec2 local = fract(coordinate);
    local = local * local * (3.0 - 2.0 * local);
    float a = fract(sin(dot(cell, vec2(127.1, 311.7))) * 43758.5453);
    float b = fract(sin(dot(cell + vec2(1.0, 0.0),
                            vec2(127.1, 311.7))) * 43758.5453);
    float c = fract(sin(dot(cell + vec2(0.0, 1.0),
                            vec2(127.1, 311.7))) * 43758.5453);
    float d = fract(sin(dot(cell + vec2(1.0),
                            vec2(127.1, 311.7))) * 43758.5453);
    return mix(mix(a, b, local.x), mix(c, d, local.x), local.y);
}
vec3 landscapeSurface(vec3 normal, vec2 coordinate, float height) {
    float variation = landscapeVariation(coordinate * 0.37);
    float slope = smoothstep(0.18, 0.62, 1.0 - normal.y);
    vec3 meadow = mix(vec3(0.075, 0.19, 0.055),
                      vec3(0.19, 0.31, 0.08), variation);
    vec3 rock = mix(vec3(0.19, 0.17, 0.145),
                    vec3(0.36, 0.32, 0.27), variation);
    float snow = smoothstep(125.0, 210.0, height) *
                 smoothstep(0.65, 0.92, normal.y);
    return mix(mix(meadow, rock, slope), vec3(0.72, 0.77, 0.80), snow);
}
vec2 environmentUv(vec3 direction) {
    float rotationCosine = lighting.environmentSky.a;
    float rotationSine = lighting.environmentGround.a;
    direction = vec3(
        rotationCosine * direction.x - rotationSine * direction.z,
        direction.y,
        rotationSine * direction.x + rotationCosine * direction.z);
    direction /= abs(direction.x) + abs(direction.y) + abs(direction.z);
    vec2 encoded = direction.xy;
    vec2 signNotZero = mix(
        vec2(-1.0), vec2(1.0),
        greaterThanEqual(encoded, vec2(0.0)));
    vec2 folded = (1.0 - abs(encoded.yx)) * signNotZero;
    encoded = mix(encoded, folded, bvec2(direction.z < 0.0));
    return encoded * 0.5 + 0.5;
}

vec4 environmentProbeBlend(vec3 worldPosition) {
    vec3 weightedTint = vec3(0.0);
    float totalWeight = 1.0;
    uint probeCount = floatBitsToUint(
        lighting.environmentParameters.z);
    for (uint probeIndex = 0U; probeIndex < probeCount;
         ++probeIndex) {
        ReflectionProbe probe =
            lighting.reflectionProbes[probeIndex];
        vec3 probeOffset =
            worldPosition - probe.positionRadius.xyz;
        float distanceSquared = dot(probeOffset, probeOffset);
        float radius = probe.positionRadius.w;
        if (distanceSquared >= radius * radius) continue;
        float normalizedDistance = sqrt(distanceSquared) / radius;
        float weight =
            (1.0 - smoothstep(0.55, 1.0, normalizedDistance)) *
            probe.tintIntensity.w;
        weightedTint += probe.tintIntensity.rgb * weight;
        totalWeight += weight;
    }
    float normalization = 1.0 / totalWeight;
    return vec4(weightedTint * normalization, normalization);
}

vec3 environmentRadiance(vec3 direction, vec3 radiance,
                         vec4 probeBlend) {
    float skyWeight =
        clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 globalTint = mix(
        lighting.environmentGround.rgb,
        lighting.environmentSky.rgb,
        skyWeight);
    return radiance *
        (globalTint * probeBlend.a + probeBlend.rgb);
}


vec3 evaluateDirectLight(vec3 n, vec3 v, vec3 l, float rawNdotL,
                         float ndotv, vec3 radiance, vec3 albedo,
                         vec2 brdfParameters, float metallic, vec3 f0,
                         uint model, float strength) {
    float ndotl = max(rawNdotL, 0.0);
    vec3 h = normalize(v + l);
    vec3 result = vec3(0.0);
    if (ndotl > 0.0) {
        float d = distributionGGX(n, h, brdfParameters.x);
        float g = geometrySmith(ndotv, ndotl, brdfParameters.y);
        vec3 f = fresnelSchlick(max(dot(h, v), 0.0), f0);
        vec3 specular =
            (d * g * f) / max(4.0 * ndotv * ndotl, 0.0001);
        vec3 kd = (1.0 - f) * (1.0 - metallic);
        result = (kd * albedo / PI + specular) * radiance * ndotl;
    }
    if (model == MATERIAL_CLEAR_COAT && ndotl > 0.0) {
        float coatRoughness = mix(0.18, 0.04, strength);
        float coatAlpha = coatRoughness * coatRoughness;
        float coatR = coatRoughness + 1.0;
        vec3 coatF = fresnelSchlick(max(dot(h, v), 0.0), vec3(0.04));
        float coatD = distributionGGX(
            n, h, coatAlpha * coatAlpha);
        float coatG = geometrySmith(
            ndotv, ndotl, coatR * coatR * 0.125);
        vec3 coat = coatD * coatG * coatF /
                    max(4.0 * ndotv * ndotl, 0.0001);
        result = result * (1.0 - 0.25 * strength) +
                 coat * radiance * ndotl * strength;
    } else if (model == MATERIAL_CLOTH && ndotl > 0.0) {
        float sheen = pow5(1.0 - max(dot(h, v), 0.0));
        result += albedo * radiance * ndotl * sheen *
                  (0.35 * strength);
    } else if (model == MATERIAL_HAIR && ndotl > 0.0) {
        vec3 tangent = normalize(vWorldTangent.xyz);
        float tangentDotH = dot(tangent, h);
        float sinTheta = sqrt(max(
            1.0 - tangentDotH * tangentDotH, 0.0));
        float longitudinal = pow(sinTheta, mix(24.0, 96.0, strength));
        result += radiance * ndotl * longitudinal *
                  mix(vec3(0.04), albedo, 0.35) * strength;
    } else if (model == MATERIAL_FOLIAGE ||
               model == MATERIAL_SKIN) {
        float wrap = (model == MATERIAL_FOLIAGE ? 0.5 : 0.25) *
                     strength;
        float wrapped = max((rawNdotL + wrap) / (1.0 + wrap), 0.0);
        result += albedo * radiance * max(wrapped - ndotl, 0.0) *
                  (1.0 - metallic) / PI;
    }
    return result;
}

vec3 evaluateLocalLight(LocalLight light, vec3 worldPosition, vec3 n, vec3 v,
                        float ndotv, vec3 albedo, vec2 brdfParameters,
                        float metallic, vec3 f0, uint model, float strength) {
    vec3 toLight = light.positionRange.xyz - worldPosition;
    float distanceSquared = dot(toLight, toLight);
    float range = light.positionRange.w;
    float rangeSquared = range * range;
    float inverseRangeSquared = uintBitsToFloat(light.parameters.y);
    if (distanceSquared >= rangeSquared ||
        distanceSquared <= 0.000001)
        return vec3(0.0);
    float wrap = model == MATERIAL_FOLIAGE
        ? 0.5 * strength
        : model == MATERIAL_SKIN ? 0.25 * strength : 0.0;
    float unnormalizedNdotL = dot(n, toLight);
    if (wrap == 0.0 && unnormalizedNdotL <= 0.0)
        return vec3(0.0);
    float inverseDistance = inversesqrt(distanceSquared);
    float rawNdotL = unnormalizedNdotL * inverseDistance;
    if (rawNdotL <= -wrap) return vec3(0.0);
    vec3 l = toLight * inverseDistance;
    float normalizedDistanceSquared =
        distanceSquared * inverseRangeSquared;
    float rangeWindow = max(
        1.0 - normalizedDistanceSquared *
              normalizedDistanceSquared,
        0.0);
    float inverseFalloffDistance = min(
        inverseDistance * inverseDistance, 100.0);
    float attenuation =
        rangeWindow * rangeWindow * inverseFalloffDistance;
    if (light.parameters.x == 1U) {
        float coneCosine = dot(-l, light.directionOuterCone.xyz);
        float coneWeight = clamp(
            (coneCosine - light.directionOuterCone.w) *
                uintBitsToFloat(light.parameters.z),
            0.0, 1.0);
        attenuation *=
            coneWeight * coneWeight * (3.0 - 2.0 * coneWeight);
    }
    if (attenuation <= 0.0) return vec3(0.0);
    vec3 radiance =
        light.colorIntensity.rgb * attenuation;
    float visibility =
        localShadowVisibility(light, worldPosition, n, l);
    return evaluateDirectLight(n, v, l, rawNdotL, ndotv, radiance, albedo,
                               brdfParameters, metallic, f0, model,
                               strength) * visibility;
}

void main() {
    vec3 n = normalize(vWorldNormal);
    if (hasMaterialTexture(2U)) {
        vec3 t = normalize(vWorldTangent.xyz - n * dot(n, vWorldTangent.xyz));
        vec3 b = cross(n, t) * vWorldTangent.w;
        vec3 tangentNormal = sampleMaterialTexture(1U).xyz * 2.0 - 1.0;
        tangentNormal.xy *= clamp(vMaterialFlags.w, 0.0, 4.0);
        n = normalize(mat3(t, b, n) * tangentNormal);
    }
    vec3 v = normalize(scene.cameraPositionTime.xyz - vWorldPosition);
    vec3 albedo = max(vAlbedoRoughness.rgb, vec3(0.0));
    float roughness = clamp(vAlbedoRoughness.a, 0.04, 1.0);
    float metallic = clamp(vEmissiveMetallic.a, 0.0, 1.0);
    float ambientOcclusion = 1.0;
    vec4 baseSample = hasMaterialTexture(1U)
        ? sampleMaterialTexture(0U)
        : vec4(1.0);
    if (hasMaterialFeature(MATERIAL_FEATURE_ALPHA_MASK) &&
        baseSample.a < materialAlphaCutoff()) {
        discard;
    }
    albedo *= baseSample.rgb;
    if (hasMaterialTexture(4U)) {
        vec3 orm = sampleMaterialTexture(2U).rgb;
        ambientOcclusion = clamp(orm.r, 0.0, 1.0);
        roughness = clamp(roughness * orm.g, 0.04, 1.0);
        metallic = clamp(metallic * orm.b, 0.0, 1.0);
    }
    uint model = materialClass();
    if (model == MATERIAL_LANDSCAPE) {
        albedo *= landscapeSurface(n, vUv, vWorldPosition.y);
        roughness = max(roughness, 0.72);
        metallic = 0.0;
    } else if (model == MATERIAL_WATER) {
        float time = scene.cameraPositionTime.w;
        vec2 phase = vUv * 9.0;
        float waveX = sin(phase.x + time * 0.73) * 0.11 +
                      sin(phase.y * 0.61 + time * 1.07) * 0.07;
        float waveZ = cos(phase.y - time * 0.67) * 0.11 +
                      cos(phase.x * 0.53 - time * 0.91) * 0.07;
        n = normalize(n + vec3(waveX, 0.0, waveZ));
        float variation = landscapeVariation(vUv * 0.8);
        albedo = mix(vec3(0.012, 0.075, 0.105),
                     vec3(0.025, 0.18, 0.21), variation);
        roughness = 0.12;
        metallic = 0.06;
    }

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    float alpha = roughness * roughness;
    float geometryR = roughness + 1.0;
    vec2 brdfParameters = vec2(
        alpha * alpha, geometryR * geometryR * 0.125);
    float materialStrength = clamp(vMaterialFlags.z, 0.0, 1.0);
    float ndotv = max(dot(n, v), 0.0);
    vec3 directionalL =
        -lighting.directionalDirectionIntensity.xyz;
    float directionalNdotL = dot(n, directionalL);
    float directionalWrap = model == MATERIAL_FOLIAGE
        ? 0.5 * materialStrength
        : model == MATERIAL_SKIN ? 0.25 * materialStrength : 0.0;
    float directionalVisibility =
        directionalNdotL > -directionalWrap
            ? directionalShadowVisibility(
                  vWorldPosition, n, directionalL)
            : 1.0;
    vec3 directional = evaluateDirectLight(
        n, v, directionalL, directionalNdotL, ndotv,
        lighting.directionalColor.rgb, albedo, brdfParameters,
        metallic, f0, model, materialStrength) *
        directionalVisibility;
    uvec2 tile = uvec2(gl_FragCoord.xy) / LIGHT_TILE_SIZE;
    LightTileHeader header =
        lightTileHeaders[tile.y * lighting.counts.y + tile.x];
    vec3 local = vec3(0.0);
    for (uint index = 0U; index < header.count; ++index) {
        uint lightIndex = lightTileIndices[header.offset + index];
        local += evaluateLocalLight(
            localLights[lightIndex], vWorldPosition, n, v, ndotv, albedo,
            brdfParameters, metallic, f0, model, materialStrength);
    }

    vec3 ambientF = fresnelSchlickRoughness(ndotv, f0, roughness);
    vec3 ambientKd = (1.0 - ambientF) * (1.0 - metallic);
    float maximumEnvironmentLod =
        lighting.environmentParameters.w;
    vec4 probeBlend =
        environmentProbeBlend(vWorldPosition);
    vec3 diffuseEnvironment = environmentRadiance(
        n, lighting.environmentDiffuseRadiance.rgb, probeBlend);
    vec3 reflectionDirection = reflect(-v, n);
    vec3 specularRadiance = textureLod(
        environmentMap, environmentUv(reflectionDirection),
        roughness * maximumEnvironmentLod).rgb;
    vec3 specularEnvironment = environmentRadiance(
        reflectionDirection, specularRadiance, probeBlend);
    vec3 ambientDiffuse =
        ambientKd * albedo * diffuseEnvironment;
    vec3 ambientSpecular = ambientF * specularEnvironment;
    vec3 ambient = (ambientDiffuse + ambientSpecular) *
                   ambientOcclusion;
    float groundGrid =
        hasMaterialFeature(MATERIAL_FEATURE_GROUND_GRID) ? gridMask(vUv) : 0.0;
    vec3 gridTint = vec3(0.08, 0.09, 0.10) * groundGrid;
    vec3 color = directional + local + ambient +
                 vEmissiveMetallic.rgb + gridTint;
    outColor = vec4(color, 1.0);
}
