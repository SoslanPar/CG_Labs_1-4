# Сторонние библиотеки (third_party)

Название означает «код сторонних разработчиков». Наш собственный код находится в source/ и shaders/.

| Файл/папка | Для чего нужна |
|---|---|
| stb_image.h | Декодирование PNG и других картинок с диска в массив пикселей |
| cache/glfw | Окно, события мыши/клавиатуры, связь с оконной системой |
| cache/imgui | Панель управления сценой |
| cache/vulkanmemoryallocator | VMA: выделение памяти Vulkan для буферов и изображений |
| cache/vk-bootstrap | Создание instance/device/swapchain с меньшим количеством служебного кода |

cache — общий кэш скачанных исходников для трёх лабораторных, он исключён из Git. При его удалении чистая конфигурация может скачать библиотеки заново; потребуется интернет. stb_image.h хранится вместе с проектом и нужен для сборки. Обычно вручную менять эти библиотеки не требуется. Vulkan SDK устанавливается отдельно и в эту папку не входит.

## Происхождение stb_image

`stb_image.h` взят из репозитория https://github.com/nothings/stb,
commit `003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5`.

Исходный URL:
https://raw.githubusercontent.com/nothings/stb/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5/stb_image.h

Лицензия MIT / Public Domain находится в конце самого заголовка; используется вариант MIT.
`STB_IMAGE_IMPLEMENTATION` определён ровно в одном translation unit: `source/gpu_resources.cpp`.
Остальные зависимости перечислены и зафиксированы в CMakeLists.txt.

Все PNG в assets созданы скриптом tools/generate_assets.py специально для этой работы.
