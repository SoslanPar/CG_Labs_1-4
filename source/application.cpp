#include "application.hpp"
#include "lab_config.hpp"
#include "gpu_resources.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

// Реализация геометрии, UI, преобразований и Vulkan-команд конкретной сцены.
namespace application {

namespace {

// CMake передаёт эти значения выбранным preset-ом; fallback позволяет компилировать вручную.
#ifndef LAB_SPHERE_LONGITUDE_SEGMENTS
#define LAB_SPHERE_LONGITUDE_SEGMENTS 22
#endif
#ifndef LAB_SPHERE_LATITUDE_SEGMENTS
#define LAB_SPHERE_LATITUDE_SEGMENTS 21
#endif
#ifndef LAB_SHADOWS_ENABLED
#define LAB_SHADOWS_ENABLED 1
#endif

// Сфера строится прямоугольной UV-сеткой: число вершин = (широта + 1) * (долгота + 1).
constexpr uint32_t longitude_segments = LAB_SPHERE_LONGITUDE_SEGMENTS;
constexpr uint32_t latitude_segments = LAB_SPHERE_LATITUDE_SEGMENTS;
constexpr uint32_t sphere_vertex_count = (longitude_segments + 1) * (latitude_segments + 1);
constexpr uint32_t maximum_objects = 3;
constexpr uint32_t shadow_map_size = 2048;
constexpr uint32_t cone_segments = 32;
constexpr float pi = 3.14159265358979323846f;

struct Vertex {
	// Положение в локальной системе радиуса 1.
	float position[3];
	// Единичное направление наружу; используется для диффузного освещения.
	float normal[3];
	// UV-координаты: положение на двумерной текстуре.
	float uv[2];
};

// Mat4 хранит 4x4 матрицу в column-major порядке, как ожидает GLSL.
struct Mat4 {
	float value[16]{};
};

// Параметры сцены передаются во set = 0 и общие для всех рисуемых объектов.
struct alignas(16) SceneUniform {
	float view_projection[16];
	float light_view_projection[16];
	float camera_position[4];
	float ambient_color_intensity[4];
	float directional_direction[4];
	float directional_color_intensity[4];
	int32_t light_counts[4]; // точечные, прожекторные и резервные счётчики.
	float shadow_parameters[4];
#if LAB_NUMBER == 4
	float spot_view_projection[16];
	float spot_shadow_parameters[4]; // включение, bias, индекс прожектора, резерв.
#endif
};

#if LAB_NUMBER == 4
static_assert(sizeof(SceneUniform) == 304);
static_assert(offsetof(SceneUniform, spot_view_projection) == 224);
static_assert(offsetof(SceneUniform, spot_shadow_parameters) == 288);
#endif
// Модель передаётся через динамический UBO в set = 1.
struct alignas(16) ModelUniform {
	float model[16];
	float normal_matrix[16];
	float albedo[4];
	float specular_shininess[4];
	float emission[4]; // w=1 включает постоянный RGB маркера, независимо от освещения.
};

// Элементы storage-буферов используют vec4-совместимое выравнивание std430.
struct alignas(16) PointLightGpu {
	float position[4];
	float color_intensity[4];
};

struct alignas(16) SpotLightGpu {
	float position[4];
	float direction[4];
	float color_intensity[4];
	float cone_cosines[4]; // cos(inner), cos(outer), остальные поля зарезервированы.
};

struct DynamicBuffer {
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
	void* mapped = nullptr;
	VkDeviceSize capacity = 0;
};

struct ObjectSettings {
	// Настройки отдельного экземпляра сферы и его материала.
	float position[3]{};
	float rotation[3]{};
	float scale[3]{1.0f, 1.0f, 1.0f};
	float albedo[4]{1.0f, 1.0f, 1.0f, 1.0f};
	float specular[3]{0.8f, 0.8f, 0.8f};
	float shininess = 48.0f;
    int material = 0;             // CPU выбирает VkDescriptorSet материала.
    bool effect = false;          // Эти параметры НЕ попадают в ModelUniform.
    float effect_strength = 0.65f; // В shader передаются через push constants.
    float emission_strength = 1.0f;
};

struct EffectPush {
    float time;
    int32_t enabled;
    float strength;
    float emission_strength;
};
static_assert(sizeof(EffectPush) == 16);
constexpr size_t material_count = 4;
[[maybe_unused]] const char* material_names[] = {"Ocean planet", "Neon circuitry", "Ceramic tiles", "Matte fabric (black fallback)"};
struct MaterialTextures { gpu::Texture albedo, specular, emissive; };
std::array<MaterialTextures, material_count> materials{};
gpu::Texture black_texture;
std::array<VkDescriptorSet, material_count> material_descriptor_sets{};
int floor_material = 2;
bool floor_effect = false;
bool effect_playing = true;
float effect_time = 0.0f;
float effect_speed = 1.0f;

bool loadMaterials() {
    if (!gpu::loadTexture("assets/black.png", false, black_texture)) return false;
#if LAB_NUMBER >= 3
    const char* albedo[] = {"planet", "circuit", "tiles", "fabric"};
    for (size_t i = 0; i < material_count; ++i) {
        const std::string base = std::string("assets/") + albedo[i];
        if (!gpu::loadTexture((base + "_albedo.png").c_str(), true, materials[i].albedo)) return false;
        if (i != 3 && !gpu::loadTexture((base + "_specular.png").c_str(), false, materials[i].specular)) return false;
        if (i == 1 && !gpu::loadTexture((base + "_emissive.png").c_str(), true, materials[i].emissive)) return false;
    }
#endif
    return true;
}

struct PointLightSettings {
	float position[3]{-1.6f, 1.5f, 1.2f};
	float color[3]{1.0f, 0.36f, 0.12f};
	float intensity = 5.0f;
};

struct SpotLightSettings {
	float position[3]{1.5f, 2.8f, 1.4f};
	float direction[3]{-0.35f, -0.85f, -0.4f};
	float color[3]{0.3f, 0.65f, 1.0f};
	float intensity = 12.0f;
	float inner_degrees = 18.0f;
	float outer_degrees = 30.0f;
};

VkBuffer vertex_buffer = VK_NULL_HANDLE;
VmaAllocation vertex_allocation = VK_NULL_HANDLE;
VkBuffer index_buffer = VK_NULL_HANDLE;
VmaAllocation index_allocation = VK_NULL_HANDLE;
uint32_t index_count = 0;
VkBuffer plane_vertex_buffer = VK_NULL_HANDLE;
VmaAllocation plane_vertex_allocation = VK_NULL_HANDLE;
VkBuffer plane_index_buffer = VK_NULL_HANDLE;
VmaAllocation plane_index_allocation = VK_NULL_HANDLE;

VkImage shadow_image = VK_NULL_HANDLE;
VmaAllocation shadow_image_allocation = VK_NULL_HANDLE;
VkImageView shadow_image_view = VK_NULL_HANDLE;
VkSampler shadow_sampler = VK_NULL_HANDLE;
VkRenderPass shadow_render_pass = VK_NULL_HANDLE;
VkFramebuffer shadow_framebuffer = VK_NULL_HANDLE;
VkFormat shadow_depth_format = VK_FORMAT_UNDEFINED;
#if LAB_NUMBER == 4
VkImage spot_shadow_image = VK_NULL_HANDLE;
VmaAllocation spot_shadow_allocation = VK_NULL_HANDLE;
VkImageView spot_shadow_view = VK_NULL_HANDLE;
bool shadow_initialized[2]{};
bool spot_shadows_enabled = true;
int shadow_spot_index = 0;
float shadow_coverage = 9.0f;
float shadow_constant_bias = 1.25f;
float shadow_slope_bias = 1.75f;
float spot_shadow_bias = 0.0015f;
#endif

std::array<ObjectSettings, maximum_objects> objects{};
std::vector<PointLightSettings> point_lights;
std::vector<SpotLightSettings> spot_lights;
// Настройки, которыми управляет ImGui; сами GPU-буферы геометрии общие для сфер.
int visible_object_count = maximum_objects;
int selected_object = 0;
bool perspective_projection = true;
bool animation_playing = true;
#if LAB_SHADOWS_ENABLED
bool shadows_enabled = LAB_SHADOWS_ENABLED != 0;
#endif
bool show_point_visualizers = false;
bool show_spot_visualizers = false;
float field_of_view_degrees = 55.0f;
float orthographic_height = 3.6f;
float near_plane = 0.1f;
float far_plane = 100.0f;
float camera_position[3]{0.0f, 0.0f, 4.2f};
float camera_yaw = 0.0f;
float camera_pitch = 0.0f;
float ambient_color[3]{0.22f, 0.24f, 0.30f};
float ambient_intensity = 0.24f;
float directional_direction[3]{0.45f, 0.8f, 0.4f};
float directional_color[3]{1.0f, 0.92f, 0.78f};
float directional_intensity = 0.75f;
float trajectory_radius = 0.22f;
float animation_speed = 1.0f;
#if LAB_SHADOWS_ENABLED
float shadow_bias = 0.0015f;
#endif
float animation_time = 0.0f;
double previous_update_time = 0.0;
bool has_previous_update_time = false;

VkDescriptorSetLayout scene_descriptor_set_layout = VK_NULL_HANDLE;
VkDescriptorSetLayout model_descriptor_set_layout = VK_NULL_HANDLE;
VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
VkDescriptorSet scene_descriptor_set = VK_NULL_HANDLE;
VkBuffer scene_uniform_buffer = VK_NULL_HANDLE;
VmaAllocation scene_uniform_allocation = VK_NULL_HANDLE;
void* scene_uniform_mapped = nullptr;
DynamicBuffer point_light_buffer;
DynamicBuffer spot_light_buffer;
DynamicBuffer model_uniform_buffer;
VkDeviceSize model_uniform_stride = 0;
VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
VkPipeline graphics_pipeline = VK_NULL_HANDLE;
VkPipeline shadow_pipeline = VK_NULL_HANDLE;
VkPipeline line_pipeline = VK_NULL_HANDLE;
VkBuffer cone_line_buffer = VK_NULL_HANDLE;
VmaAllocation cone_line_allocation = VK_NULL_HANDLE;
uint32_t cone_line_vertex_count = 0;

// Создаёт единичную матрицу: диагональ равна 1, остальные элементы — 0.
Mat4 identityMatrix() {
	Mat4 result{};
	result.value[0] = result.value[5] = result.value[10] = result.value[15] = 1.0f;
	return result;
}

// Перемножает матрицы column-major: результат применяется как left * right.
Mat4 multiply(const Mat4& left, const Mat4& right) {
	Mat4 result{};
	for (int column = 0; column < 4; ++column) {
		for (int row = 0; row < 4; ++row) {
			for (int k = 0; k < 4; ++k) {
				result.value[column * 4 + row] +=
					left.value[k * 4 + row] * right.value[column * 4 + k];
			}
		}
	}
	return result;
}

// Перенос хранится в последнем столбце матрицы в column-major массиве.
Mat4 translation(float x, float y, float z) {
	Mat4 result = identityMatrix();
	result.value[12] = x;
	result.value[13] = y;
	result.value[14] = z;
	return result;
}

// Масштаб по каждой локальной оси; обратные коэффициенты используются normal matrix.
Mat4 scaling(float x, float y, float z) {
	Mat4 result{};
	result.value[0] = x;
	result.value[5] = y;
	result.value[10] = z;
	result.value[15] = 1.0f;
	return result;
}

// Поворот вокруг X; angle задаётся в радианах.
Mat4 rotationX(float angle) {
	Mat4 result = identityMatrix();
	const float c = std::cos(angle), s = std::sin(angle);
	result.value[5] = c;
	result.value[6] = s;
	result.value[9] = -s;
	result.value[10] = c;
	return result;
}

// Поворот вокруг Y; angle задаётся в радианах.
Mat4 rotationY(float angle) {
	Mat4 result = identityMatrix();
	const float c = std::cos(angle), s = std::sin(angle);
	result.value[0] = c;
	result.value[2] = -s;
	result.value[8] = s;
	result.value[10] = c;
	return result;
}

// Поворот вокруг Z; angle задаётся в радианах.
Mat4 rotationZ(float angle) {
	Mat4 result = identityMatrix();
	const float c = std::cos(angle), s = std::sin(angle);
	result.value[0] = c;
	result.value[1] = s;
	result.value[4] = -s;
	result.value[5] = c;
	return result;
}

// Перспективная проекция: удалённые объекты выглядят меньше.
Mat4 perspective(float fov_degrees, float aspect, float near_value, float far_value) {
	const float f = 1.0f / std::tan(fov_degrees * pi / 360.0f);
	Mat4 result{};
	result.value[0] = f / aspect;
	// Минус по Y адаптирует направление экранной оси Vulkan.
	result.value[5] = -f;
	result.value[10] = far_value / (near_value - far_value);
	result.value[11] = -1.0f;
	result.value[14] = (far_value * near_value) / (near_value - far_value);
	return result;
}

// Ортографическая проекция сохраняет размер объекта независимо от расстояния.
Mat4 orthographic(float height, float aspect, float near_value, float far_value) {
	Mat4 result = identityMatrix();
	result.value[0] = 2.0f / (height * aspect);
	result.value[5] = -2.0f / height;
	result.value[10] = 1.0f / (near_value - far_value);
	result.value[14] = near_value / (near_value - far_value);
	return result;
}

// Приводит 3D-вектор к длине 1; нулевой вектор оставляет без изменений.
void normalize3(float value[3]) {
	const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
	if (length > 0.0f) {
		value[0] /= length;
		value[1] /= length;
		value[2] /= length;
	}
}

// Строит view matrix из положения глаза, точки взгляда и приблизительного направления вверх.
Mat4 lookAt(const float eye[3], const float target[3], const float up[3]) {
	// Forward направлен от камеры к цели; side и camera_up образуют ортонормированный базис.
	float forward[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
	normalize3(forward);
	float side[3] = {
		forward[1] * up[2] - forward[2] * up[1],
		forward[2] * up[0] - forward[0] * up[2],
		forward[0] * up[1] - forward[1] * up[0],
	};
	normalize3(side);
	const float camera_up[3] = {
		side[1] * forward[2] - side[2] * forward[1],
		side[2] * forward[0] - side[0] * forward[2],
		side[0] * forward[1] - side[1] * forward[0],
	};
	const float dot_side = side[0] * eye[0] + side[1] * eye[1] + side[2] * eye[2];
	const float dot_up = camera_up[0] * eye[0] + camera_up[1] * eye[1] + camera_up[2] * eye[2];
	const float dot_forward = forward[0] * eye[0] + forward[1] * eye[1] + forward[2] * eye[2];

	Mat4 result = identityMatrix();
	result.value[0] = side[0]; result.value[4] = side[1]; result.value[8] = side[2];
	result.value[1] = camera_up[0]; result.value[5] = camera_up[1]; result.value[9] = camera_up[2];
	result.value[2] = -forward[0]; result.value[6] = -forward[1]; result.value[10] = -forward[2];
	result.value[12] = -dot_side;
	result.value[13] = -dot_up;
	result.value[14] = dot_forward;
	return result;
}

// Нормализует вектор и подставляет безопасное направление, если длина близка к нулю.
void normalized3(const float input[3], float output[3], const float fallback[3] = nullptr) {
	const float length = std::sqrt(input[0] * input[0] + input[1] * input[1] + input[2] * input[2]);
	if (length > 1e-6f) {
		output[0] = input[0] / length;
		output[1] = input[1] / length;
		output[2] = input[2] / length;
	} else if (fallback) {
		std::memcpy(output, fallback, sizeof(float) * 3);
        normalize3(output);
	} else {
		output[0] = 0.0f; output[1] = 0.0f; output[2] = -1.0f;
	}
}

// Направление взгляда камеры из yaw/pitch: нулевые углы смотрят вдоль -Z.
void cameraForward(float output[3]) {
	output[0] = std::sin(camera_yaw) * std::cos(camera_pitch);
	output[1] = std::sin(camera_pitch);
	output[2] = -std::cos(camera_yaw) * std::cos(camera_pitch);
}

// Строит вращение, переводящее локальную ось -Z в заданное направление прожектора.
Mat4 orientationFromDirection(const float direction[3]) {
	float forward[3];
	normalized3(direction, forward);
	const float local_z[3] = {-forward[0], -forward[1], -forward[2]};
	const float reference_up[3] = {std::fabs(local_z[1]) > 0.98f ? 1.0f : 0.0f,
	                              std::fabs(local_z[1]) > 0.98f ? 0.0f : 1.0f, 0.0f};
	float local_x[3] = {
		reference_up[1] * local_z[2] - reference_up[2] * local_z[1],
		reference_up[2] * local_z[0] - reference_up[0] * local_z[2],
		reference_up[0] * local_z[1] - reference_up[1] * local_z[0],
	};
	normalized3(local_x, local_x);
	const float local_y[3] = {
		local_z[1] * local_x[2] - local_z[2] * local_x[1],
		local_z[2] * local_x[0] - local_z[0] * local_x[2],
		local_z[0] * local_x[1] - local_z[1] * local_x[0],
	};
	Mat4 result = identityMatrix();
	result.value[0] = local_x[0]; result.value[1] = local_x[1]; result.value[2] = local_x[2];
	result.value[4] = local_y[0]; result.value[5] = local_y[1]; result.value[6] = local_y[2];
	result.value[8] = local_z[0]; result.value[9] = local_z[1]; result.value[10] = local_z[2];
	return result;
}

// Создаёт точки UV-сферы равномерной сеткой по широте и долготе.
std::vector<Vertex> createSphereVertices() {
	std::vector<Vertex> vertices;
	vertices.reserve(sphere_vertex_count);
	for (uint32_t latitude = 0; latitude <= latitude_segments; ++latitude) {
		const float v = static_cast<float>(latitude) / latitude_segments;
		// phi идёт от северного полюса до южного; theta обходит полный круг вокруг оси Y.
		const float phi = v * pi;
		for (uint32_t longitude = 0; longitude <= longitude_segments; ++longitude) {
			const float u = static_cast<float>(longitude) / longitude_segments;
			const float theta = u * 2.0f * pi;
			const float x = std::sin(phi) * std::cos(theta);
			const float y = std::cos(phi);
			const float z = std::sin(phi) * std::sin(theta);
			// Для единичной сферы нормаль радиальна и численно равна позиции.
			// Шов дублируется: одинаковой позиции соответствуют u=0 и u=1.
			vertices.push_back({
				.position = {x, y, z},
				.normal = {x, y, z},
				.uv = {u, v},
			});
		}
	}
	return vertices;
}

// Каждую ячейку сетки делит диагональ на два треугольника; вершины соседних ячеек общие.
std::vector<uint32_t> createSphereIndices() {
	std::vector<uint32_t> indices;
	indices.reserve(latitude_segments * longitude_segments * 6);
	for (uint32_t latitude = 0; latitude < latitude_segments; ++latitude) {
		for (uint32_t longitude = 0; longitude < longitude_segments; ++longitude) {
			const uint32_t top_left = latitude * (longitude_segments + 1) + longitude;
			const uint32_t top_right = top_left + 1;
			const uint32_t bottom_left = top_left + longitude_segments + 1;
			const uint32_t bottom_right = bottom_left + 1;
			// Шесть индексов описывают два triangle-list треугольника одной ячейки.
			indices.insert(indices.end(), {top_left, bottom_left, top_right,
			                               top_right, bottom_left, bottom_right});
		}
	}
	return indices;
}

// Создаёт GPU-буфер с памятью, доступной CPU; mapped != nullptr запрашивает готовый указатель.
bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
	              VkBuffer& buffer, VmaAllocation& allocation, void** mapped = nullptr) {
	const VkBufferCreateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VmaAllocationCreateInfo allocation_info = {
		.flags = static_cast<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
		         (mapped ? VMA_ALLOCATION_CREATE_MAPPED_BIT : 0)),
		.usage = VMA_MEMORY_USAGE_AUTO,
		.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
	};
	VmaAllocationInfo result_info{};
	if (vmaCreateBuffer(graphics::internal::context.allocator, &buffer_info, &allocation_info,
	                    &buffer, &allocation, &result_info) != VK_SUCCESS) {
		std::cerr << "Failed to create application buffer\n";
		return false;
	}
	// Удобно для статических данных сцены: записываем их в память напрямую через memcpy.
	if (mapped) {
		*mapped = result_info.pMappedData;
	}
	return true;
}

// VMA уничтожает одновременно VkBuffer и связанное с ним выделение памяти.
void destroyBuffer(VkBuffer& buffer, VmaAllocation& allocation) {
	if (buffer != VK_NULL_HANDLE) {
		vmaDestroyBuffer(graphics::internal::context.allocator, buffer, allocation);
		buffer = VK_NULL_HANDLE;
		allocation = VK_NULL_HANDLE;
	}
}

// Буфер меняющегося размера остаётся отображённым для записи CPU перед текущим draw call.
bool ensureDynamicBuffer(DynamicBuffer& target, VkDeviceSize required_size,
	                     VkBufferUsageFlags usage) {
	const VkDeviceSize minimum_size = std::max<VkDeviceSize>(required_size, 16);
	if (target.buffer != VK_NULL_HANDLE && target.capacity >= minimum_size) return true;
	VkDeviceSize new_capacity = std::max<VkDeviceSize>(target.capacity, 256);
	while (new_capacity < minimum_size) new_capacity *= 2;
	if (target.buffer != VK_NULL_HANDLE) {
		// prepare() ждёт завершения предыдущего кадра до входа в render().
		if (vkDeviceWaitIdle(graphics::internal::context.device) != VK_SUCCESS) return false;
		vmaDestroyBuffer(graphics::internal::context.allocator, target.buffer, target.allocation);
		target = {};
	}
	const VkBufferCreateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = new_capacity,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	const VmaAllocationCreateInfo allocation_info = {
		.flags = static_cast<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_MAPPED_BIT |
		         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT),
		.usage = VMA_MEMORY_USAGE_AUTO,
		.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
	};
	VmaAllocationInfo result_info{};
	if (vmaCreateBuffer(graphics::internal::context.allocator, &buffer_info, &allocation_info,
	                    &target.buffer, &target.allocation, &result_info) != VK_SUCCESS) {
		std::cerr << "Failed to create resizable lighting/model buffer\n";
		target = {};
		return false;
	}
	target.mapped = result_info.pMappedData;
	target.capacity = new_capacity;
	return true;
}

void destroyDynamicBuffer(DynamicBuffer& target) {
	if (target.buffer != VK_NULL_HANDLE) {
		vmaDestroyBuffer(graphics::internal::context.allocator, target.buffer, target.allocation);
	}
	target = {};
}

// Читает SPIR-V бинарник с диска и создаёт VkShaderModule для Vulkan pipeline.
VkShaderModule loadShader(const char* path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		std::cerr << "Could not open shader: " << path << '\n';
		return VK_NULL_HANDLE;
	}
	const std::streamsize size = file.tellg();
	// SPIR-V состоит из 32-битных слов, поэтому размер должен делиться на четыре.
	if (size <= 0 || size % 4 != 0) {
		std::cerr << "Invalid SPIR-V shader size: " << path << '\n';
		return VK_NULL_HANDLE;
	}
	std::vector<uint32_t> code(static_cast<size_t>(size) / sizeof(uint32_t));
	file.seekg(0);
	if (!file.read(reinterpret_cast<char*>(code.data()), size)) {
		std::cerr << "Could not read shader: " << path << '\n';
		return VK_NULL_HANDLE;
	}
	const VkShaderModuleCreateInfo module_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = static_cast<size_t>(size),
		.pCode = code.data(),
	};
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(graphics::internal::context.device, &module_info, nullptr, &module) != VK_SUCCESS) {
		std::cerr << "Could not create shader module: " << path << '\n';
		return VK_NULL_HANDLE;
	}
	return module;
}

// Формирует CPU-массивы сферы, затем загружает вершины и индексы в отдельные Vulkan buffers.
bool createSphereBuffers() {
    const auto vertices = createSphereVertices();
    const auto indices = createSphereIndices();
    index_count = static_cast<uint32_t>(indices.size());
    return gpu::uploadBuffer(vertices.data(), vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             vertex_buffer, vertex_allocation) &&
           gpu::uploadBuffer(indices.data(), indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                             index_buffer, index_allocation);
}

// Создаёт большую горизонтальную плоскость под сферами; её нормали направлены вверх.
bool createPlaneBuffers() {
	constexpr float floor_y = -0.82f;
	const Vertex vertices[] = {
		{.position = {-5.0f, floor_y, -5.0f}, .normal = {0.0f, 1.0f, 0.0f}, .uv = {0.0f, 0.0f}},
		{.position = { 5.0f, floor_y, -5.0f}, .normal = {0.0f, 1.0f, 0.0f}, .uv = {4.0f, 0.0f}},
		{.position = { 5.0f, floor_y,  5.0f}, .normal = {0.0f, 1.0f, 0.0f}, .uv = {4.0f, 4.0f}},
		{.position = {-5.0f, floor_y,  5.0f}, .normal = {0.0f, 1.0f, 0.0f}, .uv = {0.0f, 4.0f}},
	};
	const uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    return gpu::uploadBuffer(vertices, sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             plane_vertex_buffer, plane_vertex_allocation) &&
           gpu::uploadBuffer(indices, sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                             plane_index_buffer, plane_index_allocation);
}

// Создаёт проволочный усечённый конус: основание и расходящиеся от вершины рёбра.
bool createConeLineBuffer() {
	std::vector<Vertex> vertices;
	vertices.reserve(cone_segments * 4);
	for (uint32_t i = 0; i < cone_segments; ++i) {
		const float a0 = 2.0f * pi * static_cast<float>(i) / cone_segments;
		const float a1 = 2.0f * pi * static_cast<float>(i + 1) / cone_segments;
		const float p0[3] = {std::cos(a0), std::sin(a0), -1.0f};
		const float p1[3] = {std::cos(a1), std::sin(a1), -1.0f};
		vertices.push_back({.position = {p0[0], p0[1], p0[2]}, .normal = {0, 0, 1}, .uv = {0, 0}});
		vertices.push_back({.position = {p1[0], p1[1], p1[2]}, .normal = {0, 0, 1}, .uv = {0, 0}});
		if (i % 4 == 0) {
			vertices.push_back({.position = {0, 0, 0}, .normal = {0, 0, 1}, .uv = {0, 0}});
			vertices.push_back({.position = {p0[0], p0[1], p0[2]}, .normal = {0, 0, 1}, .uv = {0, 0}});
		}
	}
    cone_line_vertex_count = static_cast<uint32_t>(vertices.size());
    return gpu::uploadBuffer(vertices.data(), vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             cone_line_buffer, cone_line_allocation);
}

// Создаёт карту глубины и sampler. В ЛР4 render pass/framebuffer для теней не создаются.
bool createShadowResources(bool spotlight = false) {
#if LAB_NUMBER == 4
    // Две независимые карты; один depth-only pipeline подходит обеим.
    VkImage& shadow_image = spotlight ? spot_shadow_image : application::shadow_image;
    VmaAllocation& shadow_image_allocation = spotlight ? spot_shadow_allocation : application::shadow_image_allocation;
    VkImageView& shadow_image_view = spotlight ? spot_shadow_view : application::shadow_image_view;
#else
    (void)spotlight;
#endif
	VkPhysicalDevice physical_device = graphics::internal::context.physical_device;
	const VkFormat candidates[] = {
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_D16_UNORM,
		VK_FORMAT_D24_UNORM_S8_UINT,
	};
	// Выбираем формат, который GPU разрешает и записывать как depth, и читать как texture.
	for (VkFormat format : candidates) {
		VkFormatProperties properties{};
		vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);
		const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
		                                     VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		if ((properties.optimalTilingFeatures & required) == required) {
			shadow_depth_format = format;
			break;
		}
	}
	if (shadow_depth_format == VK_FORMAT_UNDEFINED) {
		std::cerr << "No depth format supports both shadow rendering and sampling\n";
		return false;
	}

	// Shadow map — изображение глубины фиксированного разрешения, доступное attachment и sampler.
	const VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = shadow_depth_format,
		.extent = {shadow_map_size, shadow_map_size, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	const VmaAllocationCreateInfo image_allocation_info = {
		.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
	};
	if (vmaCreateImage(graphics::internal::context.allocator, &image_info,
	                   &image_allocation_info, &shadow_image, &shadow_image_allocation,
	                   nullptr) != VK_SUCCESS) {
		std::cerr << "Failed to create shadow depth image\n";
		return false;
	}

	const VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = shadow_image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = shadow_depth_format,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};
	if (vkCreateImageView(graphics::internal::context.device, &view_info, nullptr,
	                      &shadow_image_view) != VK_SUCCESS) {
		std::cerr << "Failed to create shadow depth image view\n";
		return false;
	}

#if LAB_NUMBER != 4
	const VkAttachmentDescription depth_attachment = {
		.format = shadow_depth_format,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
	};
	const VkAttachmentReference depth_reference = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	};
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 0,
		.pDepthStencilAttachment = &depth_reference,
	};
	// Барьеры описывают переход shadow image: чтение прошлой карты → запись → чтение шейдером.
	const VkSubpassDependency dependencies[] = {
		{
			.srcSubpass = VK_SUBPASS_EXTERNAL,
			.dstSubpass = 0,
			.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
			                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
		},
		{
			.srcSubpass = 0,
			.dstSubpass = VK_SUBPASS_EXTERNAL,
			.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
			                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
		},
	};
	const VkRenderPassCreateInfo render_pass_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &depth_attachment,
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = 2,
		.pDependencies = dependencies,
	};
	if (vkCreateRenderPass(graphics::internal::context.device, &render_pass_info, nullptr,
	                       &shadow_render_pass) != VK_SUCCESS) {
		std::cerr << "Failed to create shadow depth render pass\n";
		return false;
	}

	const VkFramebufferCreateInfo framebuffer_info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = shadow_render_pass,
		.attachmentCount = 1,
		.pAttachments = &shadow_image_view,
		.width = shadow_map_size,
		.height = shadow_map_size,
		.layers = 1,
	};
	if (vkCreateFramebuffer(graphics::internal::context.device, &framebuffer_info, nullptr,
	                        &shadow_framebuffer) != VK_SUCCESS) {
		std::cerr << "Failed to create shadow depth framebuffer\n";
		return false;
	}

#endif
	// Nearest выборка подходит для сравнения глубины; 3x3 PCF смягчает границы в shader-е.
	if (shadow_sampler != VK_NULL_HANDLE) return true;
	const VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.mipLodBias = 0.0f,
		.anisotropyEnable = VK_FALSE,
		.compareEnable = LAB_NUMBER == 4 ? VK_TRUE : VK_FALSE,
		.compareOp = VK_COMPARE_OP_LESS,
		.minLod = 0.0f,
		.maxLod = 0.0f,
		.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
		.unnormalizedCoordinates = VK_FALSE,
	};
	if (vkCreateSampler(graphics::internal::context.device, &sampler_info, nullptr,
	                    &shadow_sampler) != VK_SUCCESS) {
		std::cerr << "Failed to create shadow sampler\n";
		return false;
	}
	return true;
}

// Обновляет ссылки сцены на UBO, оба динамических массива света и карту теней.
void writeSceneDescriptors() {
	const VkDescriptorBufferInfo scene_info = {
		.buffer = scene_uniform_buffer, .offset = 0, .range = sizeof(SceneUniform),
	};
	const VkDescriptorBufferInfo point_info = {
		.buffer = point_light_buffer.buffer, .offset = 0,
		.range = std::max<VkDeviceSize>(point_light_buffer.capacity, sizeof(PointLightGpu)),
	};
	const VkDescriptorBufferInfo spot_info = {
		.buffer = spot_light_buffer.buffer, .offset = 0,
		.range = std::max<VkDeviceSize>(spot_light_buffer.capacity, sizeof(SpotLightGpu)),
	};
	const VkDescriptorImageInfo shadow_info = {
		.sampler = shadow_sampler, .imageView = shadow_image_view,
		.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
	};
#if LAB_NUMBER == 4
    const VkDescriptorImageInfo spot_shadow_info{shadow_sampler, spot_shadow_view,
                                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
#endif
	const VkWriteDescriptorSet writes[] = {
		{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = scene_descriptor_set,
		 .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		 .pBufferInfo = &scene_info},
		{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = scene_descriptor_set,
		 .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		 .pBufferInfo = &point_info},
		{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = scene_descriptor_set,
		 .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		 .pBufferInfo = &spot_info},
		{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = scene_descriptor_set,
		 .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		 .pImageInfo = &shadow_info},
#if LAB_NUMBER == 4
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = scene_descriptor_set,
         .dstBinding = 4, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &spot_shadow_info},
#endif
	};
	vkUpdateDescriptorSets(graphics::internal::context.device, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
}

// У каждого материала свой set, но binding 0 у всех указывает на один dynamic UBO.
void writeModelDescriptor() {
    const VkDescriptorBufferInfo model_info{model_uniform_buffer.buffer, 0, sizeof(ModelUniform)};
    for (size_t i = 0; i < material_count; ++i) {
        auto& m = materials[i];
        const gpu::Texture* textures[] = {m.albedo.image ? &m.albedo : &black_texture,
            m.specular.image ? &m.specular : &black_texture,
            m.emissive.image ? &m.emissive : &black_texture};
        VkDescriptorImageInfo images[3]{};
        VkWriteDescriptorSet writes[4]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = material_descriptor_sets[i];
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[0].pBufferInfo = &model_info;
        for (uint32_t j = 0; j < 3; ++j) {
            images[j] = {textures[j]->sampler, textures[j]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[j+1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[j+1].dstSet = material_descriptor_sets[i];
            writes[j+1].dstBinding = j+1;
            writes[j+1].descriptorCount = 1;
            writes[j+1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[j+1].pImageInfo = &images[j];
        }
        vkUpdateDescriptorSets(graphics::internal::context.device, 4, writes, 0, nullptr);
    }
}

// Копирует актуальные списки света в SSBO. Размеры меняются по UI и не зашиты в шейдере.
bool uploadLightBuffers() {
	const VkBuffer previous_point_buffer = point_light_buffer.buffer;
	const VkBuffer previous_spot_buffer = spot_light_buffer.buffer;
	const VkDeviceSize point_bytes = point_lights.size() * sizeof(PointLightGpu);
	const VkDeviceSize spot_bytes = spot_lights.size() * sizeof(SpotLightGpu);
	if (!ensureDynamicBuffer(point_light_buffer, point_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
	    !ensureDynamicBuffer(spot_light_buffer, spot_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return false;
	if (previous_point_buffer != point_light_buffer.buffer || previous_spot_buffer != spot_light_buffer.buffer) {
		writeSceneDescriptors();
	}
	if (!point_lights.empty()) {
		auto* gpu = static_cast<PointLightGpu*>(point_light_buffer.mapped);
		for (size_t i = 0; i < point_lights.size(); ++i) {
			const PointLightSettings& source = point_lights[i];
			gpu[i] = {};
			for (int axis = 0; axis < 3; ++axis) gpu[i].position[axis] = source.position[axis];
			gpu[i].position[3] = 1.0f;
			for (int channel = 0; channel < 3; ++channel) gpu[i].color_intensity[channel] = source.color[channel];
			gpu[i].color_intensity[3] = source.intensity;
		}
		vmaFlushAllocation(graphics::internal::context.allocator, point_light_buffer.allocation, 0, VK_WHOLE_SIZE);
	}
	if (!spot_lights.empty()) {
		auto* gpu = static_cast<SpotLightGpu*>(spot_light_buffer.mapped);
		for (size_t i = 0; i < spot_lights.size(); ++i) {
			const SpotLightSettings& source = spot_lights[i];
			gpu[i] = {};
			for (int axis = 0; axis < 3; ++axis) gpu[i].position[axis] = source.position[axis];
			gpu[i].position[3] = 1.0f;
			float direction[3];
			normalized3(source.direction, direction);
			for (int axis = 0; axis < 3; ++axis) gpu[i].direction[axis] = direction[axis];
			for (int channel = 0; channel < 3; ++channel) gpu[i].color_intensity[channel] = source.color[channel];
			gpu[i].color_intensity[3] = source.intensity;
			gpu[i].cone_cosines[0] = std::cos(source.inner_degrees * pi / 180.0f);
			gpu[i].cone_cosines[1] = std::cos(source.outer_degrees * pi / 180.0f);
		}
		vmaFlushAllocation(graphics::internal::context.allocator, spot_light_buffer.allocation, 0, VK_WHOLE_SIZE);
	}
	return true;
}

// Набор сцены содержит общие параметры и runtime-массивы света; набор модели — динамический UBO.
bool createDescriptors() {
	const VkDescriptorSetLayoutBinding scene_bindings[] = {
		{.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1,
		 .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
		{.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
		 .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
		{.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
		 .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
		{.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
		 .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
#if LAB_NUMBER == 4
        {.binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
#endif
	};
	const VkDescriptorSetLayoutCreateInfo scene_layout_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = static_cast<uint32_t>(std::size(scene_bindings)), .pBindings = scene_bindings,
	};
	if (vkCreateDescriptorSetLayout(graphics::internal::context.device, &scene_layout_info, nullptr,
	                                &scene_descriptor_set_layout) != VK_SUCCESS) return false;

    const VkDescriptorSetLayoutBinding model_bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo model_layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    model_layout_info.bindingCount = 4;
    model_layout_info.pBindings = model_bindings;
    if (vkCreateDescriptorSetLayout(graphics::internal::context.device, &model_layout_info, nullptr,
                                    &model_descriptor_set_layout) != VK_SUCCESS) return false;
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, static_cast<uint32_t>(material_count)},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (LAB_NUMBER == 4 ? 2u : 1u) + 3 * static_cast<uint32_t>(material_count)},
    };
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1 + static_cast<uint32_t>(material_count);
    pool_info.poolSizeCount = 4;
    pool_info.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(graphics::internal::context.device, &pool_info, nullptr, &descriptor_pool) != VK_SUCCESS) return false;
    std::array<VkDescriptorSetLayout, 1 + material_count> layouts{};
    layouts.fill(model_descriptor_set_layout);
    layouts[0] = scene_descriptor_set_layout;
    std::array<VkDescriptorSet, 1 + material_count> sets{};
    VkDescriptorSetAllocateInfo allocation_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation_info.descriptorPool = descriptor_pool;
    allocation_info.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocation_info.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(graphics::internal::context.device, &allocation_info, sets.data()) != VK_SUCCESS) return false;
    scene_descriptor_set = sets[0];
    for (size_t i = 0; i < material_count; ++i) material_descriptor_sets[i] = sets[i+1];

	if (!createBuffer(sizeof(SceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
	                  scene_uniform_buffer, scene_uniform_allocation, &scene_uniform_mapped)) return false;
	const VkPhysicalDeviceProperties properties = [&] {
		VkPhysicalDeviceProperties value{};
		vkGetPhysicalDeviceProperties(graphics::internal::context.physical_device, &value);
		return value;
	}();
	const VkDeviceSize alignment = std::max<VkDeviceSize>(1, properties.limits.minUniformBufferOffsetAlignment);
	model_uniform_stride = (sizeof(ModelUniform) + alignment - 1) / alignment * alignment;
	if (!ensureDynamicBuffer(point_light_buffer, sizeof(PointLightGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
	    !ensureDynamicBuffer(spot_light_buffer, sizeof(SpotLightGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
	    !ensureDynamicBuffer(model_uniform_buffer, model_uniform_stride * 16,
	                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
		return false;
	}
	writeSceneDescriptors();
	writeModelDescriptor();
	return true;
}

// Создаёт основной цветовой pipeline и отдельный depth-only pipeline теневого прохода.
bool createPipeline() {
	VkShaderModule vertex_shader = loadShader("shaders/sphere.vert.spv");
	VkShaderModule fragment_shader = loadShader("shaders/sphere.frag.spv");
	VkShaderModule shadow_vertex_shader = loadShader("shaders/shadow.vert.spv");
	if (vertex_shader == VK_NULL_HANDLE || fragment_shader == VK_NULL_HANDLE ||
	    shadow_vertex_shader == VK_NULL_HANDLE) {
		if (vertex_shader) vkDestroyShaderModule(graphics::internal::context.device, vertex_shader, nullptr);
		if (fragment_shader) vkDestroyShaderModule(graphics::internal::context.device, fragment_shader, nullptr);
		if (shadow_vertex_shader) vkDestroyShaderModule(graphics::internal::context.device, shadow_vertex_shader, nullptr);
		return false;
	}

	const VkDescriptorSetLayout set_layouts[] = {scene_descriptor_set_layout, model_descriptor_set_layout};
	const VkPushConstantRange push_ranges[] = {
        {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(EffectPush)},
#if LAB_NUMBER == 4
        {VK_SHADER_STAGE_VERTEX_BIT, 16, sizeof(Mat4)},
#endif
    };
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 2,
		.pSetLayouts = set_layouts,
        .pushConstantRangeCount = static_cast<uint32_t>(std::size(push_ranges)),
        .pPushConstantRanges = push_ranges,
	};
	if (vkCreatePipelineLayout(graphics::internal::context.device, &layout_info, nullptr,
	                           &pipeline_layout) != VK_SUCCESS) {
		std::cerr << "Failed to create graphics pipeline layout\n";
		vkDestroyShaderModule(graphics::internal::context.device, vertex_shader, nullptr);
		vkDestroyShaderModule(graphics::internal::context.device, fragment_shader, nullptr);
		vkDestroyShaderModule(graphics::internal::context.device, shadow_vertex_shader, nullptr);
		return false;
	}

	const VkPipelineShaderStageCreateInfo shader_stages[] = {
		{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		 .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex_shader, .pName = "main"},
		{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		 .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment_shader, .pName = "main"},
	};
	const VkVertexInputBindingDescription vertex_binding = {
		.binding = 0, .stride = sizeof(Vertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
	};
	// location в этих атрибутах должен совпадать с layout(location=...) в sphere.vert.
	const VkVertexInputAttributeDescription vertex_attributes[] = {
		{.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, position)},
		{.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, normal)},
		{.location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex, uv)},
	};
	const VkPipelineVertexInputStateCreateInfo vertex_input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vertex_binding,
		.vertexAttributeDescriptionCount = LAB_NUMBER >= 3 ? 3u : 2u, .pVertexAttributeDescriptions = vertex_attributes,
	};
	const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	const VkPipelineViewportStateCreateInfo viewport_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .scissorCount = 1,
	};
	const VkPipelineRasterizationStateCreateInfo rasterization = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	const VkPipelineMultisampleStateCreateInfo multisampling = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	// LESS + запись глубины обеспечивает правильное перекрытие поверхностей сцены.
	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
	};
	const VkPipelineColorBlendAttachmentState blend_attachment = {
		.blendEnable = VK_FALSE,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	const VkPipelineColorBlendStateCreateInfo color_blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &blend_attachment,
	};
	const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	const VkPipelineDynamicStateCreateInfo dynamic_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2, .pDynamicStates = dynamic_states,
	};
	const VkGraphicsPipelineCreateInfo pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2, .pStages = shader_stages,
		.pVertexInputState = &vertex_input,
		.pInputAssemblyState = &input_assembly,
		.pViewportState = &viewport_state,
		.pRasterizationState = &rasterization,
		.pMultisampleState = &multisampling,
		.pDepthStencilState = &depth_stencil,
		.pColorBlendState = &color_blend,
		.pDynamicState = &dynamic_state,
		.layout = pipeline_layout,
		.renderPass = graphics::internal::context.render_pass,
		.subpass = 0,
	};
	const VkResult result = vkCreateGraphicsPipelines(graphics::internal::context.device,
	                                                  VK_NULL_HANDLE, 1, &pipeline_info,
	                                                  nullptr, &graphics_pipeline);
	if (result != VK_SUCCESS) {
		std::cerr << "Failed to create sphere graphics pipeline\n";
		vkDestroyShaderModule(graphics::internal::context.device, shadow_vertex_shader, nullptr);
		return false;
	}
	// Тот же шейдерный расчёт в режиме линий используется только для белого конуса прожектора.
	const VkPipelineInputAssemblyStateCreateInfo line_input_assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
	};
	VkGraphicsPipelineCreateInfo line_pipeline_info = pipeline_info;
	line_pipeline_info.pInputAssemblyState = &line_input_assembly;
	const VkResult line_result = vkCreateGraphicsPipelines(graphics::internal::context.device,
	                                                     VK_NULL_HANDLE, 1, &line_pipeline_info,
	                                                     nullptr, &line_pipeline);
	vkDestroyShaderModule(graphics::internal::context.device, vertex_shader, nullptr);
	vkDestroyShaderModule(graphics::internal::context.device, fragment_shader, nullptr);
	if (line_result != VK_SUCCESS) {
		std::cerr << "Failed to create spotlight cone line pipeline\n";
		vkDestroyShaderModule(graphics::internal::context.device, shadow_vertex_shader, nullptr);
		return false;
	}

	const VkPipelineShaderStageCreateInfo shadow_stage = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_VERTEX_BIT,
		.module = shadow_vertex_shader,
		.pName = "main",
	};
	const VkPipelineRasterizationStateCreateInfo shadow_rasterization = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.depthBiasEnable = VK_TRUE,
		.depthBiasConstantFactor = 1.25f,
		.depthBiasClamp = 0.0f,
		.depthBiasSlopeFactor = 1.75f,
		.lineWidth = 1.0f,
	};
	const VkPipelineColorBlendStateCreateInfo shadow_color_blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 0,
		.pAttachments = nullptr,
	};
	// У shadow pipeline нет цветового attachment: он пишет только глубину в shadow map.
    VkPipelineVertexInputStateCreateInfo shadow_vertex_input = vertex_input;
    shadow_vertex_input.vertexAttributeDescriptionCount = 1; // Shadow shader читает только position.
#if LAB_NUMBER == 4
    // Формат attachment задаётся через pNext, VkRenderPass и VkFramebuffer не создаются.
    const VkPipelineRenderingCreateInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .depthAttachmentFormat = shadow_depth_format,
    };
    const VkDynamicState shadow_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
    const VkPipelineDynamicStateCreateInfo shadow_dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 3, .pDynamicStates = shadow_states,
    };
#endif
	const VkGraphicsPipelineCreateInfo shadow_pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
#if LAB_NUMBER == 4
        .pNext = &rendering_info,
#endif
		.stageCount = 1,
		.pStages = &shadow_stage,
		.pVertexInputState = &shadow_vertex_input,
		.pInputAssemblyState = &input_assembly,
		.pViewportState = &viewport_state,
		.pRasterizationState = &shadow_rasterization,
		.pMultisampleState = &multisampling,
		.pDepthStencilState = &depth_stencil,
		.pColorBlendState = &shadow_color_blend,
#if LAB_NUMBER == 4
        .pDynamicState = &shadow_dynamic,
#else
        .pDynamicState = &dynamic_state,
#endif
        .layout = pipeline_layout,
        .renderPass = LAB_NUMBER == 4 ? VK_NULL_HANDLE : shadow_render_pass,
		.subpass = 0,
	};
	const VkResult shadow_result = vkCreateGraphicsPipelines(graphics::internal::context.device,
	                                                         VK_NULL_HANDLE, 1, &shadow_pipeline_info,
	                                                         nullptr, &shadow_pipeline);
	vkDestroyShaderModule(graphics::internal::context.device, shadow_vertex_shader, nullptr);
	if (shadow_result != VK_SUCCESS) {
		std::cerr << "Failed to create shadow graphics pipeline\n";
		return false;
	}
	return true;
}

// Освобождает ресурсы сцены в обратном порядке зависимостей: pipeline → descriptors → buffers/images.
void destroyApplicationResources() {
	VkDevice device = graphics::internal::context.device;
	if (device == VK_NULL_HANDLE) return;
	vkDeviceWaitIdle(device);
	if (graphics_pipeline) vkDestroyPipeline(device, graphics_pipeline, nullptr);
	graphics_pipeline = VK_NULL_HANDLE;
	if (line_pipeline) vkDestroyPipeline(device, line_pipeline, nullptr);
	line_pipeline = VK_NULL_HANDLE;
	if (shadow_pipeline) vkDestroyPipeline(device, shadow_pipeline, nullptr);
	shadow_pipeline = VK_NULL_HANDLE;
	if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
	pipeline_layout = VK_NULL_HANDLE;
	if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
	descriptor_pool = VK_NULL_HANDLE;
	scene_descriptor_set = VK_NULL_HANDLE;
    material_descriptor_sets.fill(VK_NULL_HANDLE);
    for (auto& m : materials) {
        gpu::destroyTexture(m.albedo); gpu::destroyTexture(m.specular); gpu::destroyTexture(m.emissive);
    }
    gpu::destroyTexture(black_texture);
	destroyBuffer(scene_uniform_buffer, scene_uniform_allocation);
	scene_uniform_mapped = nullptr;
	destroyDynamicBuffer(point_light_buffer);
	destroyDynamicBuffer(spot_light_buffer);
	destroyDynamicBuffer(model_uniform_buffer);
	if (scene_descriptor_set_layout) vkDestroyDescriptorSetLayout(device, scene_descriptor_set_layout, nullptr);
	scene_descriptor_set_layout = VK_NULL_HANDLE;
	if (model_descriptor_set_layout) vkDestroyDescriptorSetLayout(device, model_descriptor_set_layout, nullptr);
	model_descriptor_set_layout = VK_NULL_HANDLE;
	destroyBuffer(cone_line_buffer, cone_line_allocation);
	cone_line_vertex_count = 0;
	destroyBuffer(plane_index_buffer, plane_index_allocation);
	destroyBuffer(plane_vertex_buffer, plane_vertex_allocation);
	destroyBuffer(index_buffer, index_allocation);
	destroyBuffer(vertex_buffer, vertex_allocation);
#if LAB_NUMBER == 4
    if (spot_shadow_view) vkDestroyImageView(device, spot_shadow_view, nullptr);
    if (spot_shadow_image) vmaDestroyImage(graphics::internal::context.allocator, spot_shadow_image, spot_shadow_allocation);
    spot_shadow_view = VK_NULL_HANDLE;
    spot_shadow_image = VK_NULL_HANDLE;
    spot_shadow_allocation = VK_NULL_HANDLE;
    shadow_initialized[0] = shadow_initialized[1] = false;
#endif
	if (shadow_sampler) vkDestroySampler(device, shadow_sampler, nullptr);
	shadow_sampler = VK_NULL_HANDLE;
	if (shadow_framebuffer) vkDestroyFramebuffer(device, shadow_framebuffer, nullptr);
	shadow_framebuffer = VK_NULL_HANDLE;
	if (shadow_render_pass) vkDestroyRenderPass(device, shadow_render_pass, nullptr);
	shadow_render_pass = VK_NULL_HANDLE;
	if (shadow_image_view) vkDestroyImageView(device, shadow_image_view, nullptr);
	shadow_image_view = VK_NULL_HANDLE;
	if (shadow_image) vmaDestroyImage(graphics::internal::context.allocator, shadow_image, shadow_image_allocation);
	shadow_image = VK_NULL_HANDLE;
	shadow_image_allocation = VK_NULL_HANDLE;
}

} // namespace

bool initialize() {
	// Начальные параметры трёх экземпляров одной общей геометрии.
	objects[0] = {{-0.95f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.72f, 0.72f, 0.72f},
	               {1.0f, 1.0f, 1.0f, 1.0f}, {0.8f, 0.8f, 0.8f}, 48.0f};
	objects[1] = {{0.95f, 0.0f, 0.0f}, {0.4f, 0.7f, 0.0f}, {0.58f, 0.58f, 0.58f},
	               {0.7f, 0.9f, 1.0f, 1.0f}, {0.9f, 0.9f, 0.9f}, 72.0f};
	objects[2] = {{0.0f, 0.85f, 0.0f}, {0.8f, 0.0f, 0.5f}, {0.48f, 0.48f, 0.48f},
	               {1.0f, 0.7f, 0.5f, 1.0f}, {0.7f, 0.7f, 0.7f}, 32.0f};
#if LAB_NUMBER >= 3
    objects[0].material = 0;
    objects[1].material = 1;
    objects[1].effect = true;
    objects[2].material = 3;
    for (auto& o : objects) { o.albedo[0] = o.albedo[1] = o.albedo[2] = 1.0f; }
#endif
    point_lights = {PointLightSettings{}};
	spot_lights = {SpotLightSettings{}};
    // Воспроизводимые интеграционные сценарии для проверки реально отрисованных кадров.
    // В обычном запуске переменная отсутствует, все настройки доступны через ImGui.
    if (const char* test_case = std::getenv("LAB3_TEST_CASE")) {
        animation_playing = effect_playing = false;
        trajectory_radius = 0.0f;
        const std::string test(test_case);
        if (test == "dark" || test == "black") {
            ambient_intensity = directional_intensity = 0.0f;
            for (auto& light : point_lights) light.intensity = 0.0f;
            for (auto& light : spot_lights) light.intensity = 0.0f;
            if (test == "black") for (auto& object : objects) object.material = 3;
        } else if (test == "effect0" || test == "effect2") {
            objects[0].material = 1;
            objects[0].effect = true;
            effect_time = test == "effect0" ? 0.0f : 2.0f;
        } else if (test == "specular") {
            ambient_intensity = 0.0f;
            directional_intensity = 1.0f;
            directional_direction[0] = directional_direction[1] = 0.0f;
            directional_direction[2] = 1.0f;
            for (auto& light : point_lights) light.intensity = 0.0f;
            for (auto& light : spot_lights) light.intensity = 0.0f;
            objects[0].material = 0;
            objects[1].material = 3;
            objects[2].material = 2;
            for (auto& object : objects) {
                object.effect = false;
                object.albedo[0] = object.albedo[1] = object.albedo[2] = 0.0f;
                object.specular[0] = object.specular[1] = object.specular[2] = 1.0f;
            }
        } else if (test == "stress") {
            point_lights.resize(12);
            spot_lights.resize(6);
            show_point_visualizers = show_spot_visualizers = true;
        }
    }
#if LAB_NUMBER == 4
    // Детерминированные сценарии проверяют вклад каждой карты отдельно от прочих ламп.
    if (const char* name = std::getenv("LAB4_TEST_CASE")) {
        const std::string test(name);
        animation_playing = effect_playing = false;
        trajectory_radius = 0.0f;
        for (auto& object : objects) { object.effect = false; object.material = 2; }
        if (test.starts_with("sun")) {
            point_lights.clear(); spot_lights.clear();
            ambient_intensity = 0.1f;
            directional_intensity = 1.0f;
            shadows_enabled = test != "sun-off";
            if (test == "sun-wide") shadow_coverage = 16.0f;
            if (test == "sun-direction") { directional_direction[0] = -0.7f; directional_direction[2] = 0.8f; }
            if (test == "sun-bias") shadow_bias = 0.02f;
        } else if (test.starts_with("spot")) {
            point_lights.clear(); directional_intensity = 0;
            ambient_intensity = 0.1f;
            spot_shadows_enabled = test != "spot-off";
            spot_lights[0].intensity = 28.0f;
            if (test == "spot-move") spot_lights[0].position[0] = -1.5f;
            if (test == "spot-angle") spot_lights[0].outer_degrees = 45.0f;
            if (test == "spot-selected") { spot_lights.push_back(spot_lights[0]); spot_lights[0].intensity = 0; shadow_spot_index = 1; }
        } else if (test == "empty") {
            point_lights.clear(); spot_lights.clear(); shadow_spot_index = 99;
        } else if (test == "vertical") {
            directional_direction[0] = directional_direction[2] = 0;
            spot_lights[0].direction[0] = spot_lights[0].direction[2] = 0;
        } else if (test == "stress") {
            point_lights.resize(12); spot_lights.resize(6);
            shadow_spot_index = 5;
            show_point_visualizers = show_spot_visualizers = true;
        }
    }
#endif
	// Порядок важен: сначала изображения/буферы, затем descriptor sets, затем pipeline их использует.
	if (!createSphereBuffers() || !createPlaneBuffers() || !createConeLineBuffer() || !createShadowResources() ||
	    (LAB_NUMBER == 4 && !createShadowResources(true)) || !loadMaterials() || !createDescriptors() || !createPipeline()) {
		destroyApplicationResources();
		return false;
	}
	return true;
}

void shutdown() {
	// Вызывается до остановки общего VkDevice в graphics_internal::shutdown.
	destroyApplicationResources();
}

void update(double time) {
	// Переводим абсолютное GLFW-время в накопленное время анимации с учётом скорости и паузы.
	if (has_previous_update_time && animation_playing) {
		animation_time += static_cast<float>(time - previous_update_time) * animation_speed;
	}
	if (has_previous_update_time && effect_playing)
        effect_time += static_cast<float>(time - previous_update_time) * effect_speed;
    previous_update_time = time;
    has_previous_update_time = true;

	ImGui::SetNextWindowSize(ImVec2(425, 650), ImGuiCond_FirstUseEver);
    const auto ui_extent = graphics::internal::context.swapchain_extent;
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(ui_extent.width > 450 ? ui_extent.width - 435 : 10), 10), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(425, static_cast<float>(ui_extent.height) - 20), ImGuiCond_Always);
    ImGui::Begin(LAB_WINDOW_TITLE, nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    ImGui::PushItemWidth(200.0f);
	ImGui::Text("Variant 5: UV sphere (%u vertices)", sphere_vertex_count);
#if LAB_NUMBER == 4
#if LAB_SHADOWS_ENABLED
	ImGui::SeparatorText("Directional-light shadows");
	ImGui::Checkbox("Enable shadows", &shadows_enabled);
	ImGui::SliderFloat("Shadow bias", &shadow_bias, 0.0f, 0.02f, "%.5f");
#if LAB_NUMBER == 4
    ImGui::SliderFloat("Sun coverage (width = height)", &shadow_coverage, 3.0f, 20.0f, "%.1f");
    ImGui::SliderFloat("Raster constant bias", &shadow_constant_bias, 0.0f, 4.0f);
    ImGui::SliderFloat("Raster slope bias", &shadow_slope_bias, 0.0f, 4.0f);
    ImGui::Checkbox("Spotlight shadows", &spot_shadows_enabled);
    if (!spot_lights.empty()) {
        ImGui::SliderInt("Shadow spotlight (0-based)", &shadow_spot_index, 0, static_cast<int>(spot_lights.size()) - 1);
    } else ImGui::TextUnformatted("Add a spotlight to enable its shadow.");
    ImGui::SliderFloat("Spot shadow bias", &spot_shadow_bias, 0.0f, 0.02f, "%.5f");
#endif
#endif
#endif

#if LAB_NUMBER >= 3
    ImGui::SeparatorText("Textures and sampling (from Lab 3)");
    const char* texture_objects[] = {"Sphere 1", "Sphere 2", "Sphere 3"};
    ImGui::Combo("Texture target", &selected_object, texture_objects, maximum_objects);
    ObjectSettings& texture_object = objects[selected_object];
    ImGui::Combo("Material", &texture_object.material, material_names, static_cast<int>(material_count));
    const auto& selected_material = materials[static_cast<size_t>(texture_object.material)];
    ImGui::Text("Specular: %s | Emissive: %s", selected_material.specular.image ? "map" : "black 1x1",
                selected_material.emissive.image ? "map" : "black 1x1");
    ImGui::Checkbox("Animated multi-sampling", &texture_object.effect);
    ImGui::SliderFloat("Effect strength", &texture_object.effect_strength, 0.0f, 1.0f);
    ImGui::SliderFloat("Emission strength", &texture_object.emission_strength, 0.0f, 4.0f);
    ImGui::Checkbox("Play texture effect", &effect_playing);
    ImGui::SliderFloat("Texture speed", &effect_speed, 0.0f, 3.0f);
    ImGui::Combo("Floor material", &floor_material, material_names, static_cast<int>(material_count));
    ImGui::Checkbox("Animate floor UV", &floor_effect);
    if (ImGui::Button("All lights off (emission demo)")) {
        ambient_intensity = directional_intensity = 0.0f;
        for (auto& light : point_lights) light.intensity = 0.0f;
        for (auto& light : spot_lights) light.intensity = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Restore light")) {
        ambient_intensity = 0.24f; directional_intensity = 0.75f;
        for (auto& light : point_lights) light.intensity = 5.0f;
        for (auto& light : spot_lights) light.intensity = 12.0f;
    }
#endif
#if LAB_NUMBER >= 2
	// Самые нужные настройки света ставим вверху окна, чтобы их не скрывала прокрутка.
	ImGui::SeparatorText("Ambient + directional light (sun)");
	ImGui::ColorEdit3("Ambient color", ambient_color);
	ImGui::SliderFloat("Ambient intensity", &ambient_intensity, 0.0f, 1.5f, "%.2f");
	ImGui::DragFloat3("Direction toward light", directional_direction, 0.01f, -1.0f, 1.0f);
	ImGui::ColorEdit3("Directional color", directional_color);
	ImGui::SliderFloat("Directional intensity", &directional_intensity, 0.0f, 3.0f, "%.2f");
	ImGui::TextWrapped("Direction, color and intensity control the sunlight-like source.");

#endif
	ImGui::SeparatorText("Camera");
	ImGui::DragFloat3("Position##camera", camera_position, 0.03f, -12.0f, 12.0f);
	float yaw_degrees = camera_yaw * 180.0f / pi;
	float pitch_degrees = camera_pitch * 180.0f / pi;
	if (ImGui::SliderFloat("Yaw", &yaw_degrees, -180.0f, 180.0f, "%.1f deg")) camera_yaw = yaw_degrees * pi / 180.0f;
	if (ImGui::SliderFloat("Pitch", &pitch_degrees, -89.0f, 89.0f, "%.1f deg")) camera_pitch = pitch_degrees * pi / 180.0f;
	if (ImGui::RadioButton("Perspective", perspective_projection)) perspective_projection = true;
	ImGui::SameLine();
	if (ImGui::RadioButton("Orthographic", !perspective_projection)) perspective_projection = false;
	if (perspective_projection) ImGui::SliderFloat("Field of view", &field_of_view_degrees, 25.0f, 100.0f, "%.0f deg");
	else ImGui::SliderFloat("View height", &orthographic_height, 2.0f, 12.0f, "%.2f");
	ImGui::SliderFloat("Near plane", &near_plane, 0.05f, 1.0f, "%.2f");
	ImGui::SliderFloat("Far plane", &far_plane, 10.0f, 150.0f, "%.1f");

#if LAB_NUMBER >= 2
	ImGui::SeparatorText("Point lights (inverse-square falloff)");
	if (ImGui::Button("Add point light")) point_lights.push_back(PointLightSettings{});
	int remove_point = -1;
	for (int i = 0; i < static_cast<int>(point_lights.size()); ++i) {
		ImGui::PushID(i);
		if (ImGui::TreeNode("point_light", "Point light %d", i + 1)) {
			auto& light = point_lights[static_cast<size_t>(i)];
			ImGui::DragFloat3("Position", light.position, 0.03f, -8.0f, 8.0f);
			ImGui::ColorEdit3("Color", light.color);
			ImGui::SliderFloat("Intensity", &light.intensity, 0.0f, 30.0f, "%.2f");
			if (ImGui::Button("Remove point light")) remove_point = i;
			ImGui::TreePop();
		}
		ImGui::PopID();
		if (remove_point >= 0) break;
	}
	if (remove_point >= 0) point_lights.erase(point_lights.begin() + remove_point);

	ImGui::SeparatorText("Spotlights (soft cone and inverse-square falloff)");
	if (ImGui::Button("Add spotlight")) spot_lights.push_back(SpotLightSettings{});
	int remove_spot = -1;
	for (int i = 0; i < static_cast<int>(spot_lights.size()); ++i) {
		ImGui::PushID(1000 + i);
		if (ImGui::TreeNode("spot_light", "Spotlight %d", i + 1)) {
			auto& light = spot_lights[static_cast<size_t>(i)];
			ImGui::DragFloat3("Position", light.position, 0.03f, -8.0f, 8.0f);
			ImGui::DragFloat3("Direction", light.direction, 0.01f, -1.0f, 1.0f);
			ImGui::ColorEdit3("Color", light.color);
			ImGui::SliderFloat("Intensity", &light.intensity, 0.0f, 40.0f, "%.2f");
			ImGui::SliderFloat("Inner angle", &light.inner_degrees, 1.0f, 79.0f, "%.1f deg");
			ImGui::SliderFloat("Outer angle", &light.outer_degrees, 2.0f, 80.0f, "%.1f deg");
			if (light.inner_degrees >= light.outer_degrees) light.inner_degrees = light.outer_degrees - 0.1f;
			if (ImGui::Button("Remove spotlight")) remove_spot = i;
			ImGui::TreePop();
		}
		ImGui::PopID();
		if (remove_spot >= 0) break;
	}
	if (remove_spot >= 0) spot_lights.erase(spot_lights.begin() + remove_spot);

#endif
	ImGui::SeparatorText("Model and material");
	ImGui::SliderInt("Visible spheres", &visible_object_count, 1, maximum_objects);
	const char* object_names[] = {"Sphere 1", "Sphere 2", "Sphere 3"};
	ImGui::Combo("Selected sphere", &selected_object, object_names, maximum_objects);
	ObjectSettings& object = objects[selected_object];
	ImGui::DragFloat3("Position##model", object.position, 0.02f, -3.0f, 3.0f);
	ImGui::SliderFloat3("Rotation", object.rotation, -pi, pi, "%.2f rad");
	ImGui::SliderFloat3("Scale", object.scale, 0.15f, 1.5f, "%.2f");
	ImGui::ColorEdit4("Albedo", object.albedo);
#if LAB_NUMBER >= 2
	ImGui::ColorEdit3("Specular color", object.specular);
	ImGui::SliderFloat("Shininess", &object.shininess, 1.0f, 128.0f, "%.1f");

#endif
	ImGui::SeparatorText("Animation and visualizers");
	ImGui::Checkbox("Play sphere animation", &animation_playing);
	ImGui::SliderFloat("Animation speed", &animation_speed, 0.0f, 3.0f, "%.2f x");
	ImGui::SliderFloat("Trajectory radius", &trajectory_radius, 0.0f, 1.5f, "%.2f");
#if LAB_NUMBER >= 2
	ImGui::Checkbox("Show point-light spheres", &show_point_visualizers);
	ImGui::Checkbox("Show spotlight markers and cones", &show_spot_visualizers);
	if (show_spot_visualizers) {
		ImGui::TextColored(ImVec4(0.2f, 0.85f, 1.0f, 1.0f), "Cyan: inner cone (full angular intensity)");
		ImGui::TextUnformatted("White: outer cone (light fades to zero)");
	}
#endif
#if LAB_SHADOWS_ENABLED && LAB_NUMBER != 4
    ImGui::SeparatorText("Directional-light shadows");
    ImGui::Checkbox("Enable shadows", &shadows_enabled);
    ImGui::SliderFloat("Shadow bias", &shadow_bias, 0.0f, 0.02f, "%.5f");
#endif
    ImGui::PopItemWidth();
	ImGui::End();
}



void render(const graphics::internal::FrameData& fd) {
	if (!graphics_pipeline || !scene_descriptor_set || !material_descriptor_sets[0] ||
	    fd.command_buffer == VK_NULL_HANDLE || fd.framebuffer == VK_NULL_HANDLE) return;
	auto& context = graphics::internal::context;
	if (!uploadLightBuffers()) return;

    const uint32_t scene_width = context.swapchain_extent.width > 450 ? context.swapchain_extent.width - 450 : context.swapchain_extent.width;
	const float aspect = static_cast<float>(scene_width) /
	                     static_cast<float>(std::max(context.swapchain_extent.height, 1u));
	const Mat4 projection = perspective_projection
		? perspective(field_of_view_degrees, aspect, near_plane, far_plane)
		: orthographic(orthographic_height, aspect, near_plane, far_plane);
	float forward[3];
	cameraForward(forward);
	const float target[3] = {camera_position[0] + forward[0],
	                         camera_position[1] + forward[1],
	                         camera_position[2] + forward[2]};
	const float up[3] = {0.0f, 1.0f, 0.0f};
	const Mat4 view = lookAt(camera_position, target, up);
	const Mat4 view_projection = multiply(projection, view);

	float light_direction[3];
	const float default_direction[3] = {0.45f, 0.8f, 0.4f};
	normalized3(directional_direction, light_direction, default_direction);
	const float light_eye[3] = {light_direction[0] * 7.0f,
	                             light_direction[1] * 7.0f,
	                             light_direction[2] * 7.0f};
	const float light_target[3] = {0.0f, 0.0f, 0.0f};
	const float light_up[3] = {std::fabs(light_direction[1]) > 0.98f ? 1.0f : 0.0f, std::fabs(light_direction[1]) > 0.98f ? 0.0f : 1.0f, 0.0f};
    const Mat4 light_view = lookAt(light_eye, light_target, light_up);
	const Mat4 light_projection = orthographic(
#if LAB_NUMBER == 4
        shadow_coverage,
#else
        9.0f,
#endif
        1.0f, 0.1f, 20.0f);
	const Mat4 light_view_projection = multiply(light_projection, light_view);

#if LAB_NUMBER == 4
    Mat4 spot_view_projection = identityMatrix();
    shadow_spot_index = std::clamp(shadow_spot_index, 0, std::max(0, static_cast<int>(spot_lights.size()) - 1));
    if (!spot_lights.empty()) {
        const auto& spot = spot_lights[static_cast<size_t>(shadow_spot_index)];
        float direction[3];
        normalized3(spot.direction, direction);
        const float target[3] = {spot.position[0]+direction[0], spot.position[1]+direction[1], spot.position[2]+direction[2]};
        const float up[3] = {std::fabs(direction[1]) > 0.98f ? 1.0f : 0.0f,
                            std::fabs(direction[1]) > 0.98f ? 0.0f : 1.0f, 0};
        // Outer angle — половина угла раскрытия. Для прожектора нужна перспектива.
        spot_view_projection = multiply(perspective(2.0f * spot.outer_degrees, 1, 0.05f, 25.0f),
                                        lookAt(spot.position, target, up));
    }
#endif
	SceneUniform scene{};
	std::memcpy(scene.view_projection, view_projection.value, sizeof(scene.view_projection));
	std::memcpy(scene.light_view_projection, light_view_projection.value, sizeof(scene.light_view_projection));
	for (int axis = 0; axis < 3; ++axis) {
		scene.camera_position[axis] = camera_position[axis];
		scene.ambient_color_intensity[axis] = ambient_color[axis];
		scene.directional_direction[axis] = light_direction[axis];
		scene.directional_color_intensity[axis] = directional_color[axis];
	}
	scene.ambient_color_intensity[3] = ambient_intensity;
	scene.directional_color_intensity[3] = directional_intensity;
	scene.light_counts[0] = static_cast<int32_t>(point_lights.size());
	scene.light_counts[1] = static_cast<int32_t>(spot_lights.size());
#if LAB_SHADOWS_ENABLED
	scene.shadow_parameters[0] = shadows_enabled ? 1.0f : 0.0f;
	scene.shadow_parameters[1] = shadow_bias;
#else
	scene.shadow_parameters[0] = 0.0f;
#endif
#if LAB_NUMBER == 4
    std::memcpy(scene.spot_view_projection, spot_view_projection.value, sizeof(scene.spot_view_projection));
    scene.spot_shadow_parameters[0] = spot_shadows_enabled && !spot_lights.empty() ? 1.0f : 0.0f;
    scene.spot_shadow_parameters[1] = spot_shadow_bias;
    scene.spot_shadow_parameters[2] = static_cast<float>(shadow_spot_index);
#endif
	std::memcpy(scene_uniform_mapped, &scene, sizeof(scene));
	vmaFlushAllocation(context.allocator, scene_uniform_allocation, 0, VK_WHOLE_SIZE);

	std::vector<ModelUniform> models;
	const auto make_model_uniform = [&](const Mat4& model, const Mat4& normal_matrix,
	                                    const float albedo[4], const float specular[3],
	                                    float shininess, const float emission[4] = nullptr) {
		ModelUniform uniform{};
		std::memcpy(uniform.model, model.value, sizeof(uniform.model));
		std::memcpy(uniform.normal_matrix, normal_matrix.value, sizeof(uniform.normal_matrix));
		std::memcpy(uniform.albedo, albedo, sizeof(uniform.albedo));
		std::memcpy(uniform.specular_shininess, specular, sizeof(float) * 3);
		uniform.specular_shininess[3] = shininess;
		if (emission) std::memcpy(uniform.emission, emission, sizeof(uniform.emission));
		return uniform;
	};

	const int sphere_count = std::clamp(visible_object_count, 1, static_cast<int>(maximum_objects));
	std::array<size_t, maximum_objects> sphere_slots{};
	for (int i = 0; i < sphere_count; ++i) {
		const float phase = animation_time + static_cast<float>(i) * 2.0f * pi / maximum_objects;
		const float orbit_x = trajectory_radius * std::cos(phase);
		const float orbit_y = trajectory_radius * 0.55f * std::sin(phase * 2.0f);
		const float orbit_z = trajectory_radius * 0.35f * std::sin(phase);
		const ObjectSettings& object = objects[static_cast<size_t>(i)];
		const Mat4 position = translation(object.position[0] + orbit_x,
		                                  object.position[1] + orbit_y,
		                                  object.position[2] + orbit_z);
		const Mat4 rotation = multiply(rotationZ(object.rotation[2] + animation_time * 0.7f),
		                      multiply(rotationY(object.rotation[1] + animation_time),
		                               rotationX(object.rotation[0] + animation_time * 0.45f)));
		const Mat4 scale = scaling(object.scale[0], object.scale[1], object.scale[2]);
		const Mat4 model = multiply(position, multiply(rotation, scale));
		const Mat4 normal_matrix = multiply(rotation, scaling(1.0f / object.scale[0],
		                                                      1.0f / object.scale[1],
		                                                      1.0f / object.scale[2]));
		sphere_slots[static_cast<size_t>(i)] = models.size();
		models.push_back(make_model_uniform(model, normal_matrix, object.albedo,
		                                   object.specular, object.shininess));
	}

	const float white_albedo[4] = {1, 1, 1, 1};
	const float floor_specular[3] = {0.12f, 0.12f, 0.12f};
	const size_t floor_slot = models.size();
	models.push_back(make_model_uniform(identityMatrix(), identityMatrix(), white_albedo,
	                                   floor_specular, 12.0f));

	std::vector<size_t> point_slots;
	if (show_point_visualizers) {
		for (const PointLightSettings& light : point_lights) {
			const Mat4 model = multiply(translation(light.position[0], light.position[1], light.position[2]),
			                            scaling(0.075f, 0.075f, 0.075f));
			float color[4] = {std::min(light.color[0] * 1.8f, 1.0f),
			                  std::min(light.color[1] * 1.8f, 1.0f),
			                  std::min(light.color[2] * 1.8f, 1.0f), 1.0f};
			point_slots.push_back(models.size());
			models.push_back(make_model_uniform(model, identityMatrix(), white_albedo,
			                                   floor_specular, 1.0f, color));
		}
	}

	std::vector<size_t> spot_marker_slots;
	std::vector<size_t> spot_cone_slots;
	if (show_spot_visualizers) {
		for (const SpotLightSettings& light : spot_lights) {
			const Mat4 marker = multiply(translation(light.position[0], light.position[1], light.position[2]),
			                             scaling(0.075f, 0.075f, 0.075f));
			float color[4] = {std::min(light.color[0] * 1.8f, 1.0f),
			                  std::min(light.color[1] * 1.8f, 1.0f),
			                  std::min(light.color[2] * 1.8f, 1.0f), 1.0f};
			spot_marker_slots.push_back(models.size());
			models.push_back(make_model_uniform(marker, identityMatrix(), white_albedo,
			                                   floor_specular, 1.0f, color));

			float direction[3];
			normalized3(light.direction, direction);
			const float length = 1.35f;
			const Mat4 cone_transform = multiply(
				translation(light.position[0], light.position[1], light.position[2]),
				orientationFromDirection(direction));
			auto append_cone = [&](float degrees, const float (&cone_color)[4]) {
				const float radians = std::clamp(degrees, 1.0f, 80.0f) * pi / 180.0f;
				const float radius = length * std::tan(radians);
				const Mat4 model = multiply(cone_transform, scaling(radius, radius, length));
				spot_cone_slots.push_back(models.size());
				models.push_back(make_model_uniform(model, identityMatrix(), white_albedo,
				                                   floor_specular, 1.0f, cone_color));
			};
			const float cone_white[4] = {1, 1, 1, 1};
			const float cone_cyan[4] = {0.2f, 0.85f, 1.0f, 1.0f};
			append_cone(light.outer_degrees, cone_white);
			append_cone(light.inner_degrees, cone_cyan);
		}
	}

	const VkBuffer previous_model_buffer = model_uniform_buffer.buffer;
	if (!ensureDynamicBuffer(model_uniform_buffer, models.size() * model_uniform_stride,
	                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return;
	if (previous_model_buffer != model_uniform_buffer.buffer) writeModelDescriptor();
	for (size_t i = 0; i < models.size(); ++i) {
		std::memcpy(static_cast<uint8_t*>(model_uniform_buffer.mapped) + i * model_uniform_stride,
		            &models[i], sizeof(ModelUniform));
	}
	vmaFlushAllocation(context.allocator, model_uniform_buffer.allocation, 0, VK_WHOLE_SIZE);

	const VkCommandBufferBeginInfo command_begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkResetCommandBuffer(fd.command_buffer, 0);
	if (vkBeginCommandBuffer(fd.command_buffer, &command_begin) != VK_SUCCESS) return;
	const VkDeviceSize vertex_offset = 0;
	const VkDeviceSize plane_offset = 0;
    const auto bind_model = [&](size_t slot) {
        int material = slot == floor_slot ? floor_material : 0;
        EffectPush effect{effect_time, slot == floor_slot && floor_effect ? 1 : 0, 0.65f, 1.0f};
        for (int i = 0; i < sphere_count; ++i) {
            if (slot == sphere_slots[static_cast<size_t>(i)]) {
                const auto& object = objects[static_cast<size_t>(i)];
                material = object.material;
                effect.enabled = object.effect ? 1 : 0;
                effect.strength = object.effect_strength;
                effect.emission_strength = object.emission_strength;
            }
        }
        const VkDescriptorSet sets[] = {scene_descriptor_set, material_descriptor_sets[static_cast<size_t>(material)]};
        const uint32_t dynamic_offset = static_cast<uint32_t>(slot * model_uniform_stride);
        vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               pipeline_layout, 0, 2, sets, 1, &dynamic_offset);
        vkCmdPushConstants(fd.command_buffer, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(effect), &effect);
    };

#if LAB_NUMBER == 4
    // Каждая карта: прошлое чтение -> depth attachment -> новое чтение в fragment shader.
    // Для первого кадра layout UNDEFINED; далее отслеживаем READ_ONLY отдельно для каждой карты.
    for (int pass = 0; pass < 2; ++pass) {
        const VkImage image = pass == 0 ? shadow_image : spot_shadow_image;
        VkImageMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = shadow_initialized[pass] ? VK_ACCESS_SHADER_READ_BIT : 0u,
            .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout = shadow_initialized[pass] ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(fd.command_buffer,
            shadow_initialized[pass] ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        const VkRenderingAttachmentInfo depth{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = pass == 0 ? shadow_image_view : spot_shadow_view,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = {.depthStencil = {1.0f, 0}},
        };
        const VkRenderingInfo rendering{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {{0, 0}, {shadow_map_size, shadow_map_size}}, .layerCount = 1,
            .pDepthAttachment = &depth,
        };
        vkCmdBeginRendering(fd.command_buffer, &rendering);
        vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline);
        const Mat4& matrix = pass == 0 ? light_view_projection : spot_view_projection;
        vkCmdPushConstants(fd.command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 16, sizeof(Mat4), matrix.value);
        const VkViewport viewport{0, 0, float(shadow_map_size), float(shadow_map_size), 0, 1};
        const VkRect2D scissor{{0, 0}, {shadow_map_size, shadow_map_size}};
        vkCmdSetViewport(fd.command_buffer, 0, 1, &viewport);
        vkCmdSetScissor(fd.command_buffer, 0, 1, &scissor);
        vkCmdSetDepthBias(fd.command_buffer, shadow_constant_bias, 0, shadow_slope_bias);
        vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &vertex_buffer, &vertex_offset);
        vkCmdBindIndexBuffer(fd.command_buffer, index_buffer, 0, VK_INDEX_TYPE_UINT32);
        for (int i = 0; i < sphere_count; ++i) {
            bind_model(sphere_slots[static_cast<size_t>(i)]);
            vkCmdDrawIndexed(fd.command_buffer, index_count, 1, 0, 0, 0);
        }
        vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &plane_vertex_buffer, &plane_offset);
        vkCmdBindIndexBuffer(fd.command_buffer, plane_index_buffer, 0, VK_INDEX_TYPE_UINT32);
        bind_model(floor_slot);
        vkCmdDrawIndexed(fd.command_buffer, 6, 1, 0, 0, 0);
        vkCmdEndRendering(fd.command_buffer);
        barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(fd.command_buffer,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        shadow_initialized[pass] = true;
    }
#else
	const VkClearValue shadow_clear = {.depthStencil = {1.0f, 0}};
	const VkRenderPassBeginInfo shadow_begin = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = shadow_render_pass, .framebuffer = shadow_framebuffer,
		.renderArea = {.offset = {0, 0}, .extent = {shadow_map_size, shadow_map_size}},
		.clearValueCount = 1, .pClearValues = &shadow_clear,
	};
	vkCmdBeginRenderPass(fd.command_buffer, &shadow_begin, VK_SUBPASS_CONTENTS_INLINE);
#if LAB_SHADOWS_ENABLED
	vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline);
	const VkViewport shadow_viewport = {0.0f, 0.0f, static_cast<float>(shadow_map_size),
	                                    static_cast<float>(shadow_map_size), 0.0f, 1.0f};
	const VkRect2D shadow_scissor = {.offset = {0, 0}, .extent = {shadow_map_size, shadow_map_size}};
	vkCmdSetViewport(fd.command_buffer, 0, 1, &shadow_viewport);
	vkCmdSetScissor(fd.command_buffer, 0, 1, &shadow_scissor);
	vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &vertex_buffer, &vertex_offset);
	vkCmdBindIndexBuffer(fd.command_buffer, index_buffer, 0, VK_INDEX_TYPE_UINT32);
	for (int i = 0; i < sphere_count; ++i) {
		bind_model(sphere_slots[static_cast<size_t>(i)]);
		vkCmdDrawIndexed(fd.command_buffer, index_count, 1, 0, 0, 0);
	}
	vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &plane_vertex_buffer, &plane_offset);
	vkCmdBindIndexBuffer(fd.command_buffer, plane_index_buffer, 0, VK_INDEX_TYPE_UINT32);
	bind_model(floor_slot);
	vkCmdDrawIndexed(fd.command_buffer, 6, 1, 0, 0, 0);
#endif
	// Даже в сборке без теней проход нужен, чтобы перевести depth image в layout sampler-а.
	vkCmdEndRenderPass(fd.command_buffer);

#endif

	const VkClearValue clear_values[] = {
		{.color = {{0.035f, 0.045f, 0.075f, 1.0f}}}, {.depthStencil = {1.0f, 0}},
	};
	const VkRenderPassBeginInfo render_begin = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = context.render_pass, .framebuffer = fd.framebuffer,
		.renderArea = {.offset = {0, 0}, .extent = context.swapchain_extent},
		.clearValueCount = 2, .pClearValues = clear_values,
	};
	vkCmdBeginRenderPass(fd.command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
	const VkViewport viewport = {0.0f, 0.0f, static_cast<float>(scene_width),
	                             static_cast<float>(context.swapchain_extent.height), 0.0f, 1.0f};
	const VkRect2D scissor = {.offset = {0, 0}, .extent = context.swapchain_extent};
	vkCmdSetViewport(fd.command_buffer, 0, 1, &viewport);
	vkCmdSetScissor(fd.command_buffer, 0, 1, &scissor);
	vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &plane_vertex_buffer, &plane_offset);
	vkCmdBindIndexBuffer(fd.command_buffer, plane_index_buffer, 0, VK_INDEX_TYPE_UINT32);
	bind_model(floor_slot);
	vkCmdDrawIndexed(fd.command_buffer, 6, 1, 0, 0, 0);
	vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &vertex_buffer, &vertex_offset);
	vkCmdBindIndexBuffer(fd.command_buffer, index_buffer, 0, VK_INDEX_TYPE_UINT32);
	for (int i = 0; i < sphere_count; ++i) {
		bind_model(sphere_slots[static_cast<size_t>(i)]);
		vkCmdDrawIndexed(fd.command_buffer, index_count, 1, 0, 0, 0);
	}
	for (size_t slot : point_slots) {
		bind_model(slot);
		vkCmdDrawIndexed(fd.command_buffer, index_count, 1, 0, 0, 0);
	}
	for (size_t slot : spot_marker_slots) {
		bind_model(slot);
		vkCmdDrawIndexed(fd.command_buffer, index_count, 1, 0, 0, 0);
	}
	if (!spot_cone_slots.empty() && line_pipeline != VK_NULL_HANDLE) {
		const VkDeviceSize cone_offset = 0;
		vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, line_pipeline);
		vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &cone_line_buffer, &cone_offset);
		for (size_t slot : spot_cone_slots) {
			bind_model(slot);
			vkCmdDraw(fd.command_buffer, cone_line_vertex_count, 1, 0, 0);
		}
	}
	vkCmdEndRenderPass(fd.command_buffer);
	vkEndCommandBuffer(fd.command_buffer);
}

} // namespace application
