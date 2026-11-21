#include <cstdint>
#include <climits>
#include <filesystem>
#include <cstdlib>

#include <iostream>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>

#include "graphics_internal.hpp"
#include "application.hpp"
#include "lab_config.hpp"

// Главный файл связывает между собой окно GLFW, Vulkan renderer и код сцены.
// Здесь задаётся порядок запуска/остановки приложения и цикл обработки кадров.
namespace {

// Начальный размер окна в пикселях; swapchain создаётся с таким же framebuffer-размером.
constexpr int32_t default_window_width = 1600;
constexpr int32_t default_window_height = 900;

// Подпись окна в рамке Windows.
constexpr char default_window_title[] = LAB_WINDOW_TITLE;

// GLFW владеет объектом окна; другие подсистемы получают его при инициализации.
GLFWwindow* glfw_window;

} // namespace

int main(int argc, char** argv) {
    // Ресурсы лежат рядом с exe: запуск из IDE/другого рабочего каталога тоже работает.
    if (argc > 0) {
        const auto executable = std::filesystem::absolute(argv[0]);
        if (std::filesystem::exists(executable.parent_path() / "assets"))
            std::filesystem::current_path(executable.parent_path());
    }
    // Ограничение кадров удобно для повторяемой проверки запуска без ручного закрытия.
    int frames_left = -1;
    if (const char* count = std::getenv("LAB3_SMOKE_FRAMES")) frames_left = std::atoi(count);

	// Сохраняем код завершения, чтобы ошибки инициализации корректно дошли до ОС.
	int status = EXIT_SUCCESS;

	// Сначала запускаем оконную библиотеку: без неё нельзя создать поверхность для Vulkan.
	if (!glfwInit()) {
		std::cerr << "Failed to initialize GLFW\n";
		return EXIT_FAILURE;
	}

	// Vulkan сам рисует кадр, поэтому запрещаем GLFW создавать OpenGL-контекст.
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	// Создаём обычное окно; Vulkan surface будет создан позднее из этого окна.
	glfw_window = glfwCreateWindow(default_window_width, default_window_height,
	                               default_window_title, nullptr, nullptr);
	if (glfw_window == nullptr) {
		status = EXIT_FAILURE;
		goto err_null_window;
	}

	// После изменения размера только помечаем swapchain на пересоздание.
	// Создавать Vulkan-ресурсы прямо внутри callback неудобно и небезопасно.
	glfwSetFramebufferSizeCallback(glfw_window, [](GLFWwindow*, int width, int height){
		if (width == 0 || height == 0) {
			return;
		}

		graphics::internal::resize(width, height);
	});

	// Контекст ImGui хранит состояние окон и виджетов интерфейса.
	if (ImGui::CreateContext() == nullptr) {
		std::cerr << "Failed to create ImGUI context\n";
		status = EXIT_FAILURE;
		goto err_imgui_init;
	}

	// Backend GLFW передаёт ImGui ввод мыши/клавиатуры; отрисовкой займётся backend Vulkan.
	if (!ImGui_ImplGlfw_InitForVulkan(glfw_window, true)) {
		std::cerr << "Failed to initialize ImGUI GLFW backend for Vulkan renderer\n";
		status = EXIT_FAILURE;
		goto err_imgui_glfw_init;
	}

	// Renderer создаёт Vulkan instance, device, swapchain, render pass и синхронизацию.
	if (!graphics::internal::initialize(glfw_window)) {
		std::cerr << "Failed to initialize graphics\n";
		status = EXIT_FAILURE;
		goto err_graphics_init;
	}

	// Сцена создаёт свои буферы геометрии, тени, дескрипторы и графические pipelines.
	if (!application::initialize()) {
		std::cerr << "Failed to initialize application\n";
		status = EXIT_FAILURE;
		goto err_application_init;
	}

	// Цикл: принять события → обновить UI/анимацию → записать GPU-команды → показать кадр.
	while (!glfwWindowShouldClose(glfw_window)) {
		// GLFW-время монотонное; разница между кадрами используется для плавной анимации.
		const double time = glfwGetTime();

		// Забираем события окна и начинаем новый кадр ImGui.
		glfwPollEvents();
		ImGui_ImplGlfw_NewFrame();

		ImGui::NewFrame();
		// Обновление меняет состояние сцены и создаёт виджеты интерфейса этого кадра.
		application::update(time);
		ImGui::Render();

		// prepare ждёт GPU и выдаёт framebuffer текущей swapchain-картинки.
		graphics::internal::FrameData fd = graphics::internal::prepare();
		// Сцена записывает команды рисования; renderer добавляет ImGui и отправляет команды в очередь.
		application::render(fd);
		graphics::internal::submitAndPresent();
        if (frames_left > 0 && --frames_left == 0) glfwSetWindowShouldClose(glfw_window, GLFW_TRUE);
	}

	// При обычном выходе сначала освобождаем ресурсы сцены, затем общие ресурсы Vulkan.
	application::shutdown();
err_application_init:
	graphics::internal::shutdown();
err_graphics_init:
	// Метки goto образуют обратный порядок очистки для каждого этапа успешного запуска.
	ImGui_ImplGlfw_Shutdown();
err_imgui_glfw_init:
	ImGui::DestroyContext();
err_imgui_init:
	glfwDestroyWindow(glfw_window);
err_null_window:
	glfwTerminate();

	return status;
}
