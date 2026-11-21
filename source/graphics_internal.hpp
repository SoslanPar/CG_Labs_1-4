#pragma once

#include <cstdint>

#include <vulkan/vulkan_core.h>

#include <vk_mem_alloc.h>

struct GLFWwindow;

namespace graphics::internal {

// Общие Vulkan-объекты, созданные renderer-ом и используемые кодом сцены.
struct Context {
	// GPU и логическое устройство, на котором создаются ресурсы и выполняются команды.
	VkPhysicalDevice physical_device;
	VkDevice device;

	// VMA упрощает выделение памяти для буферов и изображений Vulkan.
	VmaAllocator allocator;

	// Очередь графических команд и индекс семейства очередей для её создания.
	VkQueue graphics_queue;
	uint32_t graphics_queue_index;

	// Формат изображения swapchain и текущий размер framebuffer окна.
	VkFormat swapchain_format;
	VkExtent2D swapchain_extent;

	// Общий render pass основного изображения: цвет плюс depth buffer.
	VkRenderPass render_pass;
};

// Данные конкретного кадра, которые renderer передаёт application::render.
struct FrameData {
	// Framebuffer выбранного swapchain-изображения вместе с depth attachment.
	VkFramebuffer framebuffer;
	// Command buffer: сцена записывает в него vkCmd* команды отрисовки.
	VkCommandBuffer command_buffer;
};

// Единственный общий контекст renderer-а; объявлен здесь и определён в .cpp файле.
extern Context context;

// Создать все общие ресурсы Vulkan для окна.
bool initialize(GLFWwindow* const window);
// Дождаться завершения очереди и освободить renderer-ресурсы в обратном порядке.
void shutdown();

// Сообщить renderer-у новый размер framebuffer; фактический swapchain перестроится позже.
void resize(uint32_t width, uint32_t height);

// Дождаться свободного кадра и получить framebuffer/command buffer для рисования.
FrameData prepare();
// Добавить ImGui, отправить команды на GPU и предъявить готовое изображение окну.
void submitAndPresent();

} // namespace graphics::internal
