#include "frame_capture.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace capture {
namespace {
VkBuffer buffer = VK_NULL_HANDLE;
VmaAllocation allocation = VK_NULL_HANDLE;
void* mapped = nullptr;
VkExtent2D size{};
bool bgra = false;
bool requested = false;
std::string path;
}
bool pending() { return buffer != VK_NULL_HANDLE; }
void record(VkCommandBuffer cmd, VkImage image, VkFormat format, VkExtent2D extent) {
    const char* requested_path = std::getenv("LAB3_CAPTURE");
    if (!requested_path || requested) return;
    requested = true;
    path = requested_path;
    bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
    if (!bgra && format != VK_FORMAT_R8G8B8A8_SRGB && format != VK_FORMAT_R8G8B8A8_UNORM) {
        std::cerr << "Capture requires an RGBA8/BGRA8 swapchain\n";
        return;
    }
    size = extent;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = static_cast<VkDeviceSize>(size.width) * size.height * 4;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo memory{};
    memory.usage = VMA_MEMORY_USAGE_AUTO;
    memory.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    memory.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    VmaAllocationInfo result{};
    if (vmaCreateBuffer(graphics::internal::context.allocator, &info, &memory, &buffer, &allocation, &result) != VK_SUCCESS) return;
    mapped = result.pMappedData;
    VkImageMemoryBarrier imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    imageBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imageBarrier.srcQueueFamilyIndex = imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.image = image;
    imageBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {size.width, size.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    imageBarrier.dstAccessMask = 0;
    VkBufferMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostBarrier.srcQueueFamilyIndex = hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.buffer = buffer;
    hostBarrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 1, &hostBarrier, 1, &imageBarrier);
}
void save() {
    if (!pending()) return;
    auto allocator = graphics::internal::context.allocator;
    vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << size.width << ' ' << size.height << "\n255\n";
    const auto* bytes = static_cast<const unsigned char*>(mapped);
    for (size_t i = 0; i < static_cast<size_t>(size.width) * size.height; ++i) {
        const char rgb[] = {static_cast<char>(bytes[4*i+(bgra?2:0)]), static_cast<char>(bytes[4*i+1]), static_cast<char>(bytes[4*i+(bgra?0:2)])};
        out.write(rgb, 3);
    }
    if (out) std::cout << "Captured frame: " << path << '\n';
    else std::cerr << "Cannot write frame capture: " << path << '\n';
    vmaDestroyBuffer(allocator, buffer, allocation);
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    mapped = nullptr;
}
}
