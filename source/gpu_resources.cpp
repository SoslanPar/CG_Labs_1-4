#include "gpu_resources.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <cstring>
#include <functional>
#include <iostream>

namespace gpu {
namespace {
// Одноразовая передача при загрузке. Ожидание fence позволяет сразу освободить staging.
bool transfer(const std::function<void(VkCommandBuffer)>& record) {
    auto& c = graphics::internal::context;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = c.graphics_queue_index;
    if (vkCreateCommandPool(c.device, &poolInfo, nullptr, &pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    bool success = false;
    if (vkAllocateCommandBuffers(c.device, &alloc, &command) == VK_SUCCESS &&
        vkCreateFence(c.device, &fenceInfo, nullptr, &fence) == VK_SUCCESS) {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(command, &begin) == VK_SUCCESS) {
            record(command);
            if (vkEndCommandBuffer(command) == VK_SUCCESS) {
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.commandBufferCount = 1;
                submit.pCommandBuffers = &command;
                if (vkQueueSubmit(c.graphics_queue, 1, &submit, fence) == VK_SUCCESS)
                    success = vkWaitForFences(c.device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
            }
        }
    }
    if (fence) vkDestroyFence(c.device, fence, nullptr);
    vkDestroyCommandPool(c.device, pool, nullptr);
    return success;
}

bool stagingBuffer(const void* data, VkDeviceSize size, VkBuffer& buffer, VmaAllocation& allocation) {
    auto allocator = graphics::internal::context.allocator;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo memory{};
    memory.usage = VMA_MEMORY_USAGE_AUTO;
    memory.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    memory.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    VmaAllocationInfo mapped{};
    if (vmaCreateBuffer(allocator, &info, &memory, &buffer, &allocation, &mapped) != VK_SUCCESS) return false;
    std::memcpy(mapped.pMappedData, data, static_cast<size_t>(size));
    vmaFlushAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
    return true;
}
}

bool uploadBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkBuffer& buffer, VmaAllocation& allocation) {
    auto& c = graphics::internal::context;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingMemory = VK_NULL_HANDLE;
    if (!stagingBuffer(data, size, staging, stagingMemory)) return false;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo memory{};
    memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    memory.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    bool ok = vmaCreateBuffer(c.allocator, &info, &memory, &buffer, &allocation, nullptr) == VK_SUCCESS;
    if (ok) ok = transfer([&](VkCommandBuffer cmd) {
        VkBufferCopy copy{0, 0, size};
        vkCmdCopyBuffer(cmd, staging, buffer, 1, &copy);
        // Перед чтением вершин/индексов GPU обязан увидеть результат transfer-записи.
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ? VK_ACCESS_INDEX_READ_BIT : VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer;
        barrier.size = size;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                             0, 0, nullptr, 1, &barrier, 0, nullptr);
    });
    vmaDestroyBuffer(c.allocator, staging, stagingMemory);
    if (!ok && buffer) {
        vmaDestroyBuffer(c.allocator, buffer, allocation);
        buffer = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
    }
    return ok;
}

bool loadTexture(const char* path, bool srgb, Texture& t) {
    int channels = 0;
    // stb_image декодирует настоящий файл с диска и приводит пиксели к RGBA8.
    stbi_uc* pixels = stbi_load(path, &t.width, &t.height, &channels, STBI_rgb_alpha);
    if (!pixels) {
        std::cerr << "Cannot decode texture " << path << ": " << stbi_failure_reason() << '\n';
        return false;
    }
    auto& c = graphics::internal::context;
    const VkDeviceSize size = static_cast<VkDeviceSize>(t.width) * t.height * 4;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingMemory = VK_NULL_HANDLE;
    bool ok = stagingBuffer(pixels, size, staging, stagingMemory);
    stbi_image_free(pixels);
    if (!ok) return false;
    // Цветовые карты — sRGB; коэффициенты specular — линейные данные UNORM.
    const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {static_cast<uint32_t>(t.width), static_cast<uint32_t>(t.height), 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo memory{};
    memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    memory.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    ok = vmaCreateImage(c.allocator, &info, &memory, &t.image, &t.allocation, nullptr) == VK_SUCCESS;
    if (ok) ok = transfer([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = t.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = info.extent;
        vkCmdCopyBufferToImage(cmd, staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    });
    vmaDestroyBuffer(c.allocator, staging, stagingMemory);
    if (ok) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = t.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ok = vkCreateImageView(c.device, &view, nullptr, &t.view) == VK_SUCCESS;
    }
    if (ok) {
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.maxLod = 0.0f; // ЛР3 не требует mipmaps: загружен один уровень.
        ok = vkCreateSampler(c.device, &sampler, nullptr, &t.sampler) == VK_SUCCESS;
    }
    if (!ok) destroyTexture(t);
    else std::cout << "Texture loaded: " << path << " (" << t.width << 'x' << t.height << ")\n";
    return ok;
}

void destroyTexture(Texture& t) {
    auto& c = graphics::internal::context;
    if (t.sampler) vkDestroySampler(c.device, t.sampler, nullptr);
    if (t.view) vkDestroyImageView(c.device, t.view, nullptr);
    if (t.image) vmaDestroyImage(c.allocator, t.image, t.allocation);
    t = {};
}
}
