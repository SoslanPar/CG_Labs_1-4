#pragma once
#include "graphics_internal.hpp"
// Необязательное сохранение кадра для проверки рендера, без автоматизации рабочего стола.
namespace capture {
void record(VkCommandBuffer command, VkImage image, VkFormat format, VkExtent2D extent);
bool pending();
void save(); // Вызывать только после fence отправленного кадра.
}
