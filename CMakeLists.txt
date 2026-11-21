cmake_minimum_required(VERSION 3.24)
project(cg-labs LANGUAGES C CXX)
include(FetchContent)
find_package(Vulkan REQUIRED COMPONENTS glslc)

set(LAB_NUMBER 3 CACHE STRING "Номер лабораторной: 1, 2, 3 или 4")
if(NOT LAB_NUMBER MATCHES "^[1234]$")
    message(FATAL_ERROR "LAB_NUMBER must be 1, 2, 3 or 4")
endif()
set(LAB_SPHERE_LATITUDE_SEGMENTS 9 CACHE STRING "Сегменты широты")
set(LAB_SPHERE_LONGITUDE_SEGMENTS 9 CACHE STRING "Сегменты долготы")
option(LAB_ENABLE_SHADOWS "Направленные тени из ЛР2" ON)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_LIBRARY_TYPE STATIC)
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.5.1 SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cache/glfw")
FetchContent_Declare(vk-bootstrap GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git GIT_TAG v1.4.321 SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cache/vk-bootstrap")
FetchContent_Declare(VulkanMemoryAllocator GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git GIT_TAG v3.4.0 SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cache/vulkanmemoryallocator")
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.92.9 SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cache/imgui")
# Один общий кэш исходников библиотек; объектные файлы у каждой лабы свои.
foreach(dependency glfw vk-bootstrap vulkanmemoryallocator imgui)
    string(TOUPPER "${dependency}" upper)
    set(cached_source "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cache/${dependency}")
    if(EXISTS "${cached_source}/.git" OR EXISTS "${cached_source}/CMakeLists.txt" OR EXISTS "${cached_source}/imgui.cpp")
        set(FETCHCONTENT_SOURCE_DIR_${upper} "${cached_source}" CACHE PATH "Shared dependency source" FORCE)
    endif()
endforeach()
FetchContent_MakeAvailable(glfw vk-bootstrap VulkanMemoryAllocator imgui)

add_executable(${PROJECT_NAME}
    source/main.cpp source/graphics_internal.cpp source/application.cpp source/gpu_resources.cpp source/frame_capture.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "lab${LAB_NUMBER}")
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)
target_include_directories(${PROJECT_NAME} PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE third_party)
target_link_libraries(${PROJECT_NAME} PRIVATE glfw Vulkan::Vulkan vk-bootstrap::vk-bootstrap GPUOpen::VulkanMemoryAllocator)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    LAB_NUMBER=${LAB_NUMBER}
    LAB_SPHERE_LATITUDE_SEGMENTS=${LAB_SPHERE_LATITUDE_SEGMENTS}
    LAB_SPHERE_LONGITUDE_SEGMENTS=${LAB_SPHERE_LONGITUDE_SEGMENTS}
    LAB_SHADOWS_ENABLED=$<BOOL:${LAB_ENABLE_SHADOWS}>)
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /utf-8)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()
# Обычный console subsystem оставляет видимыми ошибки загрузки и Vulkan validation.
set(shader_outputs)
foreach(shader sphere.vert sphere.frag shadow.vert)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND Vulkan::glslc -DLAB_NUMBER=${LAB_NUMBER} "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}" -o "${output}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}" VERBATIM)
    list(APPEND shader_outputs "${output}")
endforeach()
add_custom_target(lab-shaders DEPENDS ${shader_outputs})
add_custom_target(lab-assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${CMAKE_CURRENT_BINARY_DIR}/assets"
    VERBATIM)
add_dependencies(${PROJECT_NAME} lab-shaders lab-assets)
