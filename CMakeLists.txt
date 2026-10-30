cmake_minimum_required(VERSION 3.20)
project(ASTRO_CTR VERSION 1.0.0 LANGUAGES CXX)
add_compile_definitions(ASTRO_VERSION="${PROJECT_VERSION}" ASTRO_VERSION_W=L"${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "ASTRO CTR targets Windows x64 (the sacredfire.exe host).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The host process is x64; build with a 64-bit toolchain.")
endif()

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra)
endif()

# Vulkan headers (vulkan.h, vk_layer.h). MSYS2: mingw-w64-ucrt-x86_64-vulkan-headers; or the Vulkan SDK.
find_path(VULKAN_INCLUDE_DIR vulkan/vk_layer.h HINTS ENV VULKAN_SDK PATH_SUFFIXES Include include)
if(NOT VULKAN_INCLUDE_DIR)
  message(FATAL_ERROR "Vulkan headers not found (install the Vulkan SDK or MSYS2 vulkan-headers)")
endif()

# ---- shared core (config, colours, controller, log; unit-tested) ----
add_library(astro_core STATIC
  src/common/color.cpp
  src/config/json.cpp
  src/config/config.cpp
  src/diagnostics/log.cpp
  src/game/backend.cpp
  src/core/controller.cpp)
target_include_directories(astro_core PUBLIC src)

# ---- in-game module: Vulkan layer VK_LAYER_ASTRO_ctr with the ImGui menu ----
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(astro_ctr SHARED
  src/layer/layer.cpp src/layer/overlay.cpp src/layer/menu.cpp src/layer/nitro_backend.cpp
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(astro_ctr PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends ${VULKAN_INCLUDE_DIR})
target_compile_definitions(astro_ctr PRIVATE VK_NO_PROTOTYPES IMGUI_IMPL_VULKAN_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
target_link_libraries(astro_ctr PRIVATE astro_core dwmapi gdi32)
set_target_properties(astro_ctr PROPERTIES OUTPUT_NAME "astro_ctr" PREFIX "")
if(NOT MSVC)
  target_link_options(astro_ctr PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_custom_command(TARGET astro_ctr POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/layer/VkLayer_ASTRO_ctr_implicit.json $<TARGET_FILE_DIR:astro_ctr>)

# ---- launcher: registers the layer while its window is open ----
add_executable(astro_launcher launcher/main.cpp)
set_target_properties(astro_launcher PROPERTIES OUTPUT_NAME "ASTRO_CTR_Launcher")
if(NOT MSVC)
  target_link_options(astro_launcher PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

# ---- tests ----
enable_testing()
add_executable(astro_tests tests/test_core.cpp)
target_link_libraries(astro_tests PRIVATE astro_core)
add_test(NAME core COMMAND astro_tests)

# ---- developer tools (optional) ----
option(ASTRO_DEV_TOOLS "Build the stand-in host and the pointer scanner" OFF)
if(ASTRO_DEV_TOOLS)
  find_library(VULKAN_LIB NAMES vulkan-1 vulkan HINTS ENV VULKAN_SDK PATH_SUFFIXES Lib lib)
  add_executable(present_test tests/present_test.cpp)   # copy as sacredfire.exe to test the overlay without the game
  target_include_directories(present_test PRIVATE ${VULKAN_INCLUDE_DIR})
  target_link_libraries(present_test PRIVATE ${VULKAN_LIB})
  add_executable(ptrscan tools/ptrscan.cpp)             # pointer scanner for the running game
endif()
