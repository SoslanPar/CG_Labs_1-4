#version 450

#if LAB_NUMBER >= 3
layout(location = 0) in vec2 textureUV;
#else
layout(location = 0) in vec3 vertexColor;
#endif
layout(location = 1) in vec3 worldPosition;
layout(location = 2) in vec3 surfaceNormal;
layout(location = 3) in vec4 shadowPosition;
layout(location = 0) out vec4 outColor;

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

struct PointLight {
    vec4 position;
    vec4 colorIntensity;
};
layout(set = 0, binding = 1, std430) readonly buffer PointLightBuffer {
    PointLight lights[];
} pointLightBuffer;

struct SpotLight {
    vec4 position;
    vec4 direction;
    vec4 colorIntensity;
    vec4 coneCosines;
};
layout(set = 0, binding = 2, std430) readonly buffer SpotLightBuffer {
    SpotLight lights[];
} spotLightBuffer;

#if LAB_NUMBER == 4
layout(set = 0, binding = 3) uniform sampler2DShadow shadowMap;
layout(set = 0, binding = 4) uniform sampler2DShadow spotShadowMap;
#else
layout(set = 0, binding = 3) uniform sampler2D shadowMap;
#endif

layout(set = 1, binding = 0) uniform ModelData {
    mat4 model;
    mat4 normalMatrix;
    vec4 albedo;
    vec4 specularShininess;
    vec4 emission;
} modelData;

// Набор материала: общая модельная UBO + три image/sampler пары.
layout(set = 1, binding = 1) uniform sampler2D albedoMap;
layout(set = 1, binding = 2) uniform sampler2D specularMap;
layout(set = 1, binding = 3) uniform sampler2D emissiveMap;
layout(push_constant) uniform SamplingEffect {
    float time;
    int enabled;
    float strength;
    float emissionStrength;
} effect;

#if LAB_NUMBER == 4
float shadowVisibility(sampler2DShadow map, vec4 position, float bias) {
    if (position.w <= 0.0) return 1.0;
    vec3 projected = position.xyz / position.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0))) || any(greaterThan(uv, vec2(1))) ||
        projected.z < 0.0 || projected.z > 1.0) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(map, 0));
    float visibility = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            visibility += texture(map, vec3(uv + vec2(x,y) * texel, projected.z - bias));
    return visibility / 9.0;
}
#endif
float calculateShadow() {
#if LAB_NUMBER == 4
    if (sceneData.shadowParameters.x < 0.5) return 0.0;
    return 1.0 - shadowVisibility(shadowMap, shadowPosition, sceneData.shadowParameters.y);
#else
    if (sceneData.shadowParameters.x < 0.5 || shadowPosition.w <= 0.0) return 0.0;
    vec3 projected = shadowPosition.xyz / shadowPosition.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        projected.z < 0.0 || projected.z > 1.0) return 0.0;

    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float closestDepth = texture(shadowMap, uv + vec2(x, y) * texelSize).r;
            shadow += projected.z - sceneData.shadowParameters.y > closestDepth ? 1.0 : 0.0;
        }
    }
    return shadow / 9.0;
#endif
}

// Один источник использует модель Блинна-Фонга: диффузный + зеркальный компоненты.
vec3 evaluateLight(vec3 toLight, vec3 radiance, vec3 normal, vec3 viewDirection,
                   vec3 albedo, vec3 specularColor, float shininess) {
    vec3 L = normalize(toLight);
    float diffuse = max(dot(normal, L), 0.0);
    vec3 halfVector = normalize(L + viewDirection);
    float specular = diffuse > 0.0
        ? pow(max(dot(normal, halfVector), 0.0), shininess)
        : 0.0;
    return radiance * (albedo * diffuse + specularColor * specular);
}

void main() {
    // Маркеры ламп идут по отдельной ветке: их цвет не зависит от света и нормали.
    if (modelData.emission.w > 0.5) {
        outColor = vec4(modelData.emission.rgb, 1.0);
        return;
    }

#if LAB_NUMBER == 1
    outColor = vec4(vertexColor * modelData.albedo.rgb, 1.0);
    return;
#else
    vec3 N = normalize(surfaceNormal);
    vec3 V = normalize(sceneData.cameraPosition.xyz - worldPosition);
    // Две выборки одного изображения с разными UV: прокрутка и волновое искажение.
    // При выключенном эффекте читается исходная текстура без модуляции.
#if LAB_NUMBER >= 3
    vec2 uvA = textureUV;
    vec2 uvB = textureUV;
    float blendFactor = 0.0;
    vec3 modulation = vec3(1.0);
    if (effect.enabled != 0) {
        float t = effect.time;
        uvA += effect.strength * vec2(0.06 * t, 0.025 * sin(12.56637 * textureUV.x + t));
        uvB = textureUV * 2.0 + effect.strength * vec2(-0.04 * t, 0.07 * t);
        blendFactor = effect.strength * (0.25 + 0.15 * sin(t));
        modulation = mix(vec3(1.0), 0.65 + 0.35 * cos(vec3(0, 2, 4) + t), effect.strength);
    }
    vec3 sampledAlbedo = mix(texture(albedoMap, uvA).rgb, texture(albedoMap, uvB).rgb, blendFactor);
    vec3 sampledSpecular = mix(texture(specularMap, uvA).rgb, texture(specularMap, uvB).rgb, blendFactor);
    vec3 sampledEmission = mix(texture(emissiveMap, uvA).rgb, texture(emissiveMap, uvB).rgb, blendFactor);
    vec3 albedo = sampledAlbedo * modelData.albedo.rgb * modulation;
    vec3 specularColor = sampledSpecular * modelData.specularShininess.rgb;
#else
    vec3 albedo = modelData.albedo.rgb * vertexColor;
    vec3 specularColor = modelData.specularShininess.rgb;
#endif
    float shininess = max(modelData.specularShininess.w, 1.0);

    vec3 result = albedo * sceneData.ambientColorIntensity.rgb * sceneData.ambientColorIntensity.w;

    // Направленный источник задаёт постоянное направление; только его вклад затеняется картой.
    vec3 directionalRadiance = sceneData.directionalColorIntensity.rgb *
                               sceneData.directionalColorIntensity.w;
    float directionalVisibility = 1.0 - calculateShadow() * (LAB_NUMBER == 4 ? 1.0 : 0.65);
    result += evaluateLight(normalize(sceneData.directionalDirection.xyz), directionalRadiance,
                            N, V, albedo, specularColor, shininess) * directionalVisibility;

    // Точечные источники читаются из runtime-массива storage buffer.
    for (int i = 0; i < sceneData.lightCounts.x; ++i) {
        PointLight light = pointLightBuffer.lights[i];
        vec3 delta = light.position.xyz - worldPosition;
        float distanceSquared = max(dot(delta, delta), 0.01);
        vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.w / distanceSquared;
        result += evaluateLight(delta, radiance, N, V, albedo, specularColor, shininess);
    }

    // Прожектор добавляет обратное квадратичное затухание и плавный переход между конусами.
    for (int i = 0; i < sceneData.lightCounts.y; ++i) {
        SpotLight light = spotLightBuffer.lights[i];
        vec3 fromLight = worldPosition - light.position.xyz;
        float distanceSquared = max(dot(fromLight, fromLight), 0.01);
        float cone = dot(normalize(fromLight), normalize(light.direction.xyz));
        float edge = smoothstep(light.coneCosines.y, light.coneCosines.x, cone);
        vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * edge / distanceSquared;
#if LAB_NUMBER == 4
        if (sceneData.spotShadowParameters.x > 0.5 && i == int(sceneData.spotShadowParameters.z)) {
            vec4 position = sceneData.spotViewProjection * vec4(worldPosition, 1.0);
            radiance *= shadowVisibility(spotShadowMap, position, sceneData.spotShadowParameters.y);
        }
#endif
        result += evaluateLight(-fromLight, radiance, N, V, albedo, specularColor, shininess);
    }

    // Emissive прибавляется ПОСЛЕ освещения и не зависит от ламп или shadow map.
#if LAB_NUMBER >= 3
    result += sampledEmission * effect.emissionStrength * modulation;
#endif
    outColor = vec4(result, 1.0);
#endif
}
