#version 450

// Вершинные атрибуты совпадают с VkVertexInputAttributeDescription.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
#if LAB_NUMBER >= 3
layout(location = 2) in vec2 inUV;
#endif

// Набор сцены (set 0): общие для всех моделей параметры камеры и света.
layout(set = 0, binding = 0) uniform SceneData {
    mat4 viewProjection;
    mat4 lightViewProjection;
    vec4 cameraPosition;
    vec4 ambientColorIntensity;
    vec4 directionalDirection;
    vec4 directionalColorIntensity;
    ivec4 lightCounts;
    vec4 shadowParameters;
#if LAB_NUMBER == 4
    mat4 spotViewProjection;
    vec4 spotShadowParameters;
#endif
} sceneData;

// Набор модели (set 1): динамический UBO с параметрами текущего draw call.
layout(set = 1, binding = 0) uniform ModelData {
    mat4 model;
    mat4 normalMatrix;
    vec4 albedo;
    vec4 specularShininess;
    vec4 emission;
} modelData;

#if LAB_NUMBER >= 3
layout(location = 0) out vec2 textureUV;
#else
layout(location = 0) out vec3 vertexColor;
#endif
layout(location = 1) out vec3 worldPosition;
layout(location = 2) out vec3 surfaceNormal;
layout(location = 3) out vec4 shadowPosition;

void main() {
    vec4 localPosition = vec4(inPosition, 1.0);
    gl_Position = sceneData.viewProjection * modelData.model * localPosition;
    worldPosition = (modelData.model * localPosition).xyz;
    surfaceNormal = mat3(modelData.normalMatrix) * inNormal;
#if LAB_NUMBER >= 3
    textureUV = inUV;
#else
    // Процедурный цвет вершины: тот же градиент, что в ЛР1–2, и серый пол.
    vertexColor = length(inPosition) > 1.01 ? vec3(0.52, 0.56, 0.64)
        : 0.28 + 0.72 * (inPosition * 0.5 + 0.5);
#endif
    shadowPosition = sceneData.lightViewProjection * modelData.model * localPosition;
}
