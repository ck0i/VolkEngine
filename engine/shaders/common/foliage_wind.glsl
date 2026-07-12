vec3 applyFoliageWind(vec3 localPosition, SceneInstance instance) {
    uint materialClass = (instance.textureIndices.w >> 3U) & 15U;
    if (materialClass != 3U) return localPosition;

    float weight = smoothstep(0.15, 4.5, localPosition.y);
    vec2 anchor = instance.model[3].xz;
    float time = scene.cameraPositionTime.w;
    float primary = sin(time * 1.35 + anchor.x * 0.017 + anchor.y * 0.013);
    float gust = sin(time * 0.47 + anchor.x * 0.004 - anchor.y * 0.006);
    localPosition.x += (primary * 0.22 + gust * 0.11) * weight;
    localPosition.z += (primary * 0.08 - gust * 0.06) * weight;
    return localPosition;
}
