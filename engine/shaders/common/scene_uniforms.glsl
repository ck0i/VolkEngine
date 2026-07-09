layout(set = 0, binding = 0) uniform SceneData {
    mat4 viewProjection;
    vec4 cameraPositionTime;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 ambientSkyColor;
    vec4 ambientGroundColor;
} scene;
