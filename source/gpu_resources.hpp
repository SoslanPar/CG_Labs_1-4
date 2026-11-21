#pragma once
#include "graphics_internal.hpp"

// Ресурсы ЛР3: CPU пишет staging, а геометрия и изображения живут в DEVICE_LOCAL.
namespace gpu {
struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    int width = 0;
    int height = 0;
};
bool uploadBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkBuffer& buffer, VmaAllocation& allocation);
bool loadTexture(const char* path, bool srgb, Texture& texture);
void destroyTexture(Texture& texture);
}
