const uint LIGHT_TILE_SIZE = 16U;
const uint MAXIMUM_LOCAL_LIGHTS = 256U;
const uint MAXIMUM_LIGHTS_PER_TILE = 64U;

struct LocalLight {
    vec4 positionRange;
    vec4 colorIntensity;
    vec4 directionOuterCone;
    uvec4 parameters;
};

struct LightTileHeader {
    uint offset;
    uint count;
};
struct ReflectionProbe {
    vec4 positionRadius;
    vec4 tintIntensity;
};


layout(std430, set = 1, binding = 0) readonly buffer LocalLightData {
    LocalLight localLights[];
};
#ifdef VE_LIGHT_ASSIGNMENT
layout(std430, set = 1, binding = 1) writeonly buffer LightTileHeaderData {
    LightTileHeader lightTileHeaders[];
};
layout(std430, set = 1, binding = 2) writeonly buffer LightTileIndexData {
    uint lightTileIndices[];
};
layout(std430, set = 1, binding = 4) buffer LightListCounterData {
    uint overflowCount;
    uvec3 reserved;
} lightListCounters;
#else
layout(std430, set = 1, binding = 1) readonly buffer LightTileHeaderData {
    LightTileHeader lightTileHeaders[];
};
layout(std430, set = 1, binding = 2) readonly buffer LightTileIndexData {
    uint lightTileIndices[];
};
#endif
layout(set = 1, binding = 5) uniform sampler2DShadow shadowAtlas;
layout(std430, set = 1, binding = 6) readonly buffer ShadowInstanceIndexData {
    uint shadowInstanceIndices[];
};
layout(set = 1, binding = 7) uniform sampler2D environmentMap;

layout(set = 1, binding = 3) uniform LightingData {
    mat4 viewProjection;
    vec4 directionalDirectionIntensity;
    vec4 directionalColor;
    uvec4 directionalParameters;
    vec4 environmentSky;
    vec4 environmentGround;
    vec4 environmentParameters;
    uvec4 counts;
    vec4 viewport;
    mat4 shadowViewProjection[16];
    vec4 shadowUvScaleBias[16];
    vec4 cascadeSplits;
    ReflectionProbe reflectionProbes[4];
} lighting;
