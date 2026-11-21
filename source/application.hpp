#pragma once

#include "graphics_internal.hpp"

// Контракт между главным циклом (main.cpp) и реализацией сцены (application.cpp).
namespace application {

// Создаёт геометрию и все Vulkan-ресурсы сцены. false означает, что запуск нужно прервать.
bool initialize();
// Уничтожает ресурсы сцены до уничтожения общего VkDevice в graphics::internal::shutdown.
void shutdown();

// Обновляет состояние по абсолютному времени и строит интерфейс текущего кадра.
void update(double time);
// Записывает команды отрисовки сцены в command buffer, подготовленный renderer-ом.
void render(const graphics::internal::FrameData& fd);

} // namespace application
