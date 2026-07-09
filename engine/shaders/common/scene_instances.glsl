struct SceneInstance {
    mat4 model;
    vec4 normalMatrix0;
    vec4 normalMatrix1;
    vec4 normalMatrix2;
    vec4 albedoRoughness;
    vec4 emissiveMetallic;
    vec4 materialFlags;
};

layout(std430, set = 0, binding = 2) readonly buffer InstanceData {
    SceneInstance instances[];
} instanceData;
