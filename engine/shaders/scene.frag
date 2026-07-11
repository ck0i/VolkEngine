#version 450
#extension GL_GOOGLE_include_directive : require
#if VE_BINDLESS
#extension GL_EXT_nonuniform_qualifier : require
#endif

#include "common/scene_uniforms.glsl"
#include "common/pbr.glsl"

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


void main() {
    vec3 n = normalize(vWorldNormal);
    if (hasMaterialTexture(2U)) {
        vec3 t = normalize(vWorldTangent.xyz - n * dot(n, vWorldTangent.xyz));
        vec3 b = cross(n, t) * vWorldTangent.w;
        vec3 tangentNormal = sampleMaterialTexture(1U).xyz * 2.0 - 1.0;
        tangentNormal.xy *= clamp(vMaterialFlags.w, 0.0, 1.0);
        tangentNormal = normalize(tangentNormal);
        n = normalize(mat3(t, b, n) * tangentNormal);
    }
    vec3 v = normalize(scene.cameraPositionTime.xyz - vWorldPosition);
    vec3 l = -scene.lightDirection.xyz;
    vec3 h = normalize(v + l);

    vec3 albedo = max(vAlbedoRoughness.rgb, vec3(0.0));
    float roughness = clamp(vAlbedoRoughness.a, 0.04, 1.0);
    float metallic = clamp(vEmissiveMetallic.a, 0.0, 1.0);
    float ambientOcclusion = 1.0;
    if (hasMaterialTexture(1U)) {
        albedo *= sampleMaterialTexture(0U).rgb;
    }
    if (hasMaterialTexture(4U)) {
        vec3 orm = sampleMaterialTexture(2U).rgb;
        ambientOcclusion = clamp(orm.r, 0.0, 1.0);
        roughness = clamp(roughness * orm.g, 0.04, 1.0);
        metallic = clamp(metallic * orm.b, 0.0, 1.0);
    }
    vec3 f0 = mix(vec3(0.04), albedo, metallic);

    float ndotl = max(dot(n, l), 0.0);
    float ndotv = max(dot(n, v), 0.0);
    float d = distributionGGX(n, h, roughness);
    float g = geometrySmith(ndotv, ndotl, roughness);
    vec3 directF = fresnelSchlick(max(dot(h, v), 0.0), f0);
    vec3 specular = (d * g * directF) / max(4.0 * ndotv * ndotl, 0.0001);
    vec3 directKd = (1.0 - directF) * (1.0 - metallic);
    vec3 diffuse = directKd * albedo / PI;

    vec3 ambientF = fresnelSchlickRoughness(ndotv, f0, roughness);
    vec3 ambientKd = (1.0 - ambientF) * (1.0 - metallic);
    vec3 direct = (diffuse + specular) * scene.lightColor.rgb * scene.lightColor.a * ndotl;
    vec3 ambientDiffuse = ambientKd * albedo * environmentDiffuse(n);
    vec3 ambientSpecular = ambientF * scene.ambientSkyColor.rgb * scene.ambientSkyColor.a * (1.0 - roughness) * 0.12;
    vec3 ambient = (ambientDiffuse + ambientSpecular) * ambientOcclusion;
    float groundGrid = 0.0;
    if (vMaterialFlags.x > 0.5) {
        groundGrid = gridMask(vUv);
    }
    vec3 gridTint = vec3(0.08, 0.09, 0.10) * groundGrid;
    vec3 color = direct + ambient + vEmissiveMetallic.rgb + gridTint;
    outColor = vec4(color, 1.0);
}
