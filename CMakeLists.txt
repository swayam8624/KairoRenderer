cmake_minimum_required(VERSION 3.28)
project(KairoRenderer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KAIRO_RENDERER_BUILD_TESTS "Build KairoRenderer tests" ${PROJECT_IS_TOP_LEVEL})
option(KAIRO_RENDERER_ENABLE_VALIDATION "Enable Vulkan validation layers in debug configurations" ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
elseif(MSVC)
    add_compile_options(/W4)
endif()

find_package(glfw3 CONFIG REQUIRED)
find_path(KAIRO_VULKAN_INCLUDE_DIR vulkan/vulkan.h REQUIRED)
set(KAIRO_MOLTENVK_LIBRARY "/opt/homebrew/lib/libMoltenVK.dylib" CACHE FILEPATH "MoltenVK dynamic library")
if(NOT EXISTS "${KAIRO_MOLTENVK_LIBRARY}")
    find_library(KAIRO_MOLTENVK_LIBRARY NAMES MoltenVK REQUIRED)
endif()

if(NOT TARGET KairoMath)
    set(KAIRO_MATH_SOURCE_DIR "" CACHE PATH "Path to a local KairoMath checkout")
    if(KAIRO_MATH_SOURCE_DIR AND EXISTS "${KAIRO_MATH_SOURCE_DIR}/CMakeLists.txt")
        add_subdirectory("${KAIRO_MATH_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/KairoMath")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../Foundation/KairoMath/CMakeLists.txt")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../Foundation/KairoMath" "${CMAKE_CURRENT_BINARY_DIR}/_deps/KairoMath")
    else()
        include(FetchContent)
        FetchContent_Declare(KairoMath GIT_REPOSITORY https://github.com/swayam8624/KairoMath.git GIT_TAG main)
        FetchContent_MakeAvailable(KairoMath)
    endif()
endif()

if(NOT TARGET KairoGeometry)
    set(KAIRO_GEOMETRY_SOURCE_DIR "" CACHE PATH "Path to a local KairoGeometry checkout")
    if(KAIRO_GEOMETRY_SOURCE_DIR AND EXISTS "${KAIRO_GEOMETRY_SOURCE_DIR}/CMakeLists.txt")
        add_subdirectory("${KAIRO_GEOMETRY_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/KairoGeometry")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../Foundation/KairoGeometry/CMakeLists.txt")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../Foundation/KairoGeometry" "${CMAKE_CURRENT_BINARY_DIR}/_deps/KairoGeometry")
    else()
        include(FetchContent)
        FetchContent_Declare(KairoGeometry GIT_REPOSITORY https://github.com/swayam8624/KairoGeometry.git GIT_TAG main)
        FetchContent_MakeAvailable(KairoGeometry)
    endif()
endif()

add_library(KairoRenderer)
target_sources(KairoRenderer PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES
    RendererTypes.cppm
    Window.cppm
    VulkanInstance.cppm
    VulkanSurface.cppm
    Renderer.cppm
    KairoRenderer.cppm)
target_include_directories(KairoRenderer PUBLIC ${KAIRO_VULKAN_INCLUDE_DIR})
target_compile_definitions(KairoRenderer PUBLIC $<$<BOOL:${KAIRO_RENDERER_ENABLE_VALIDATION}>:KAIRO_RENDERER_ENABLE_VALIDATION=1>)
target_link_libraries(KairoRenderer PUBLIC KairoMath KairoGeometry glfw ${KAIRO_MOLTENVK_LIBRARY}
    "-framework Metal" "-framework QuartzCore" "-framework CoreFoundation" "-framework IOKit" "-framework IOSurface")

add_executable(KairoRendererClear examples/clear_window.cpp)
target_link_libraries(KairoRendererClear PRIVATE KairoRenderer)

if(KAIRO_RENDERER_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(Catch2 GIT_REPOSITORY https://github.com/catchorg/Catch2.git GIT_TAG v3.5.4)
    FetchContent_MakeAvailable(Catch2)
    add_executable(KairoRendererTests tests/RendererTests.cpp)
    target_link_libraries(KairoRendererTests PRIVATE KairoRenderer Catch2::Catch2WithMain)
    enable_testing()
    add_test(NAME KairoRendererTests COMMAND KairoRendererTests)
endif()
