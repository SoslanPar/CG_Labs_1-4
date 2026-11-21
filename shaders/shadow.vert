#version 450

// Для карты теней нужна только позиция вершины; цвет и нормаль не записываются.
layout(location = 0) in vec3 inPosition;

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

// Данные модели лежат в динамическом наборе set = 1.
layout(set = 1, binding = 0) uniform ModelData {
    mat4 model;
    mat4 normalMatrix;
    vec4 albedo;
    vec4 specularShininess;
    vec4 emission;
} modelData;

#if LAB_NUMBER == 4
// Отдельный диапазон push constants: offset 0..15 занят эффектом материала во fragment stage.
layout(push_constant) uniform ShadowProjection {
    layout(offset = 16) mat4 viewProjection;
} shadowProjection;
#endif
void main() {
	// Результат попадает в depth attachment shadow pass; fragment shader здесь не нужен.
#if LAB_NUMBER == 4
    gl_Position = shadowProjection.viewProjection * modelData.model * vec4(inPosition, 1.0);
#else
    gl_Position = sceneData.lightViewProjection * modelData.model * vec4(inPosition, 1.0);
#endif
}
