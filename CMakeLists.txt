cmake_minimum_required(VERSION 3.28)
project(KairoRenderer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KAIRO_RENDERER_BUILD_TESTS "Build KairoRenderer tests" ${PROJECT_IS_TOP_LEVEL})
option(KAIRO_RENDERER_BUILD_PHYSICS_BRIDGE "Build the optional PhysicsEngine debug adapter" ${PROJECT_IS_TOP_LEVEL})
option(KAIRO_RENDERER_ENABLE_VALIDATION "Enable Vulkan validation layers in debug configurations" ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
elseif(MSVC)
    add_compile_options(/W4)
endif()

find_package(glfw3 CONFIG REQUIRED)
find_path(KAIRO_VULKAN_INCLUDE_DIR vulkan/vulkan.h REQUIRED)
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
find_library(KAIRO_VULKAN_LOADER_LIBRARY NAMES vulkan REQUIRED PATHS /opt/homebrew/lib)
if(APPLE)
    find_file(KAIRO_MOLTENVK_ICD_FILE MoltenVK_icd.json
        PATHS /opt/homebrew/opt/molten-vk/etc/vulkan/icd.d
        REQUIRED)
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

set(KAIRO_RENDERER_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
add_custom_command(
    OUTPUT "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}/triangle.vert.spv"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}"
    COMMAND "${GLSLANG_VALIDATOR}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.vert" -o "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}/triangle.vert.spv"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.vert"
    VERBATIM)
add_custom_command(
    OUTPUT "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}/triangle.frag.spv"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}"
    COMMAND "${GLSLANG_VALIDATOR}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.frag" -o "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}/triangle.frag.spv"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.frag"
    VERBATIM)
add_custom_command(
    OUTPUT "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}/debug_line.vert.spv"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}"
    COMMAND "${GLSLANG_VALIDATOR}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/debug_line.vert" -o "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}/debug_line.vert.spv"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/debug_line.vert"
    VERBATIM)
add_custom_command(
    OUTPUT "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}/debug_line.frag.spv"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}"
    COMMAND "${GLSLANG_VALIDATOR}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/debug_line.frag" -o "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}/debug_line.frag.spv"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/debug_line.frag"
    VERBATIM)
add_custom_command(
    OUTPUT "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}/shadow.vert.spv"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}"
    COMMAND "${GLSLANG_VALIDATOR}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/shadow.vert" -o "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}/shadow.vert.spv"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/shadow.vert"
    VERBATIM)
add_custom_target(KairoRendererShaders DEPENDS
    "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}/triangle.vert.spv"
    "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}/triangle.frag.spv"
    "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}/debug_line.vert.spv"
    "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}/debug_line.frag.spv"
    "${KAIRO_RENDERER_SHADER_OUTPUT_DIR}/shadow.vert.spv")

add_library(KairoRenderer)
target_sources(KairoRenderer PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES
    RendererTypes.cppm
    Camera.cppm
    Mesh.cppm
    Material.cppm
    ShadowSettings.cppm
    RenderScene.cppm
    DebugDraw.cppm
    Window.cppm
    VulkanInstance.cppm
    VulkanSurface.cppm
    VulkanDevice.cppm
    VulkanSwapchain.cppm
    VulkanCommand.cppm
    VulkanSync.cppm
    VulkanBuffer.cppm
    VulkanDescriptor.cppm
    VulkanDepth.cppm
    VulkanShadowMap.cppm
    VulkanBackendContext.cppm
    VulkanTriangle.cppm
    Renderer.cppm
    KairoRenderer.cppm)
add_dependencies(KairoRenderer KairoRendererShaders)
target_include_directories(KairoRenderer PUBLIC ${KAIRO_VULKAN_INCLUDE_DIR})
target_compile_definitions(KairoRenderer PRIVATE KAIRO_RENDERER_SHADER_DIR="${KAIRO_RENDERER_SHADER_OUTPUT_DIR}")
if(APPLE)
    target_compile_definitions(KairoRenderer PRIVATE KAIRO_RENDERER_MOLTENVK_ICD_FILE="${KAIRO_MOLTENVK_ICD_FILE}")
endif()
target_compile_definitions(KairoRenderer PUBLIC $<$<BOOL:${KAIRO_RENDERER_ENABLE_VALIDATION}>:KAIRO_RENDERER_ENABLE_VALIDATION=1>)
target_link_libraries(KairoRenderer PUBLIC KairoMath KairoGeometry glfw ${KAIRO_VULKAN_LOADER_LIBRARY}
    "-framework Metal" "-framework QuartzCore" "-framework CoreFoundation" "-framework IOKit" "-framework IOSurface")

if(KAIRO_RENDERER_BUILD_PHYSICS_BRIDGE)
    if(NOT TARGET KairoPhysicsEngine)
        set(KAIRO_PHYSICS_ENGINE_SOURCE_DIR "" CACHE PATH "Path to a local KairoPhysicsEngine checkout")
        if(KAIRO_PHYSICS_ENGINE_SOURCE_DIR AND EXISTS "${KAIRO_PHYSICS_ENGINE_SOURCE_DIR}/CMakeLists.txt")
            add_subdirectory("${KAIRO_PHYSICS_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/KairoPhysicsEngine")
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../Foundation/KairoPhysicsEngine/CMakeLists.txt")
            add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../Foundation/KairoPhysicsEngine" "${CMAKE_CURRENT_BINARY_DIR}/_deps/KairoPhysicsEngine")
        else()
            include(FetchContent)
            FetchContent_Declare(KairoPhysicsEngine GIT_REPOSITORY https://github.com/swayam8624/KairoPhysicsEngine.git GIT_TAG main)
            FetchContent_MakeAvailable(KairoPhysicsEngine)
        endif()
    endif()

    add_library(KairoRendererPhysicsDebug)
    target_sources(KairoRendererPhysicsDebug PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES
        PhysicsDebugBridge.cppm)
    target_link_libraries(KairoRendererPhysicsDebug PUBLIC KairoRenderer KairoPhysicsEngine)
endif()

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
    if(TARGET KairoRendererPhysicsDebug)
        add_executable(KairoRendererPhysicsDebugTests tests/PhysicsDebugBridgeTests.cpp)
        target_link_libraries(KairoRendererPhysicsDebugTests PRIVATE KairoRendererPhysicsDebug Catch2::Catch2WithMain)
        add_test(NAME KairoRendererPhysicsDebugTests COMMAND KairoRendererPhysicsDebugTests)
    endif()
endif()
