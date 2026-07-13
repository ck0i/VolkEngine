struct SceneInstance {
    mat4 model;
    vec4 normalMatrix0;
    vec4 normalMatrix1;
    vec4 normalMatrix2;
    vec4 albedoRoughness;
    vec4 emissiveMetallic;
    vec4 materialFlags;
    uvec4 textureIndices;
};

layout(std430, set = 0, binding = 1) readonly buffer InstanceData {
    SceneInstance instances[];
} instanceData;

#if VE_GPU_VISIBILITY
layout(std430, set = 0, binding = 2) readonly buffer VisibleInstanceIndices {
    uint indices[];
} visibleInstanceIndices;
#endif

uint sceneInstanceIndex() {
#if VE_GPU_VISIBILITY
    return visibleInstanceIndices.indices[gl_InstanceIndex];
#else
    return gl_InstanceIndex;
#endif
}

SceneInstance sceneInstance() {
#if VE_GPU_VISIBILITY
    return instanceData.instances[
        visibleInstanceIndices.indices[gl_InstanceIndex]];
#else
    return instanceData.instances[gl_InstanceIndex];
#endif
}
