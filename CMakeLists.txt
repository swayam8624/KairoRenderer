cmake_minimum_required(VERSION 3.28)
project(KairoRenderer LANGUAGES C CXX)

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
find_package(Vulkan REQUIRED)
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Git REQUIRED)

# OpenGL symbols above the Windows 1.1 import library must be loaded from the
# active context. Generate one pinned 4.1-core loader for every host so the
# backend source and validation behavior are identical on macOS/Linux/Windows.
# glad 0.1.x carries a dependency-free generator, avoiding pip/Jinja setup on
# clean engine and CI machines. Its packaged registry may contain a UTF-8 BOM;
# the pinned upstream generator opens the XML in text mode, so apply the same
# utf-8-sig compatibility fix used by downstream package managers before the
# reproducible generator is configured.
if(WIN32)
    find_package(Python 3.8...<3.14 COMPONENTS Interpreter REQUIRED)
endif()
include(FetchContent)
block()
    set(GLAD_PROFILE core CACHE STRING "" FORCE)
    set(GLAD_API "gl=4.1" CACHE STRING "" FORCE)
    set(GLAD_REPRODUCIBLE ON CACHE BOOL "" FORCE)
    set(GLAD_INSTALL OFF CACHE BOOL "" FORCE)
    # CMake 4 removed implicit compatibility below 3.5; glad's small legacy
    # project is policy-clean at 3.5 and needs this explicit floor.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_Declare(KairoGlad
        GIT_REPOSITORY https://github.com/Dav1dde/glad.git
        GIT_TAG v0.1.36
        GIT_SHALLOW TRUE
        PATCH_COMMAND "${GIT_EXECUTABLE}" -C <SOURCE_DIR> apply --whitespace=nowarn
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/glad-0.1.36-utf8-sig.patch")
    FetchContent_MakeAvailable(KairoGlad)
endblock()
if(APPLE)
    enable_language(OBJCXX)
    find_program(KAIRO_RENDERER_BREW_EXECUTABLE NAMES brew)
    if(KAIRO_RENDERER_BREW_EXECUTABLE)
        execute_process(
            COMMAND "${KAIRO_RENDERER_BREW_EXECUTABLE}" --prefix molten-vk
            OUTPUT_VARIABLE KAIRO_RENDERER_MOLTENVK_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE KAIRO_RENDERER_MOLTENVK_RESULT
            ERROR_QUIET)
    endif()
    find_file(KAIRO_MOLTENVK_ICD_FILE MoltenVK_icd.json
        HINTS
            "${KAIRO_RENDERER_MOLTENVK_PREFIX}/etc/vulkan/icd.d"
            "$ENV{VULKAN_SDK}/etc/vulkan/icd.d"
        REQUIRED)
endif()

# Standalone builds use immutable fallbacks. The umbrella build always supplies
# these targets locally, while a consumer outside the workspace receives the
# exact revisions validated by this renderer rather than whichever branch head
# happens to exist when CMake configures.
set(KAIRO_RENDERER_MATH_REVISION e12778734a8e8ffe060255a9c03683ad6c4b1a92)
set(KAIRO_RENDERER_GEOMETRY_REVISION d570c91eaddf5c40e05b5a22cea6db78177e7fd0)
set(KAIRO_RENDERER_ASSETS_REVISION 9e63b89520a053a1144a0350415a9f107b66eafa)
set(KAIRO_RENDERER_PHYSICS_REVISION 180016b1c91e7b34731491f2616e64e450ea24e5)

if(NOT TARGET KairoMath)
    set(KAIRO_MATH_SOURCE_DIR "" CACHE PATH "Path to a local KairoMath checkout")
    if(KAIRO_MATH_SOURCE_DIR AND EXISTS "${KAIRO_MATH_SOURCE_DIR}/CMakeLists.txt")
        add_subdirectory("${KAIRO_MATH_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/KairoMath")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../Foundation/KairoMath/CMakeLists.txt")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../Foundation/KairoMath" "${CMAKE_CURRENT_BINARY_DIR}/_deps/KairoMath")
    else()
        include(FetchContent)
        FetchContent_Declare(KairoMath
            GIT_REPOSITORY https://github.com/swayam8624/KairoMath.git
            GIT_TAG ${KAIRO_RENDERER_MATH_REVISION})
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
        FetchContent_Declare(KairoGeometry
            GIT_REPOSITORY https://github.com/swayam8624/KairoGeometry.git
            GIT_TAG ${KAIRO_RENDERER_GEOMETRY_REVISION})
        FetchContent_MakeAvailable(KairoGeometry)
    endif()
endif()

if(NOT TARGET KairoAssets)
    set(KAIRO_ASSETS_SOURCE_DIR "" CACHE PATH "Path to a local KairoAssets checkout")
    if(KAIRO_ASSETS_SOURCE_DIR AND EXISTS "${KAIRO_ASSETS_SOURCE_DIR}/CMakeLists.txt")
        add_subdirectory("${KAIRO_ASSETS_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/KairoAssets")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../KairoAssets/CMakeLists.txt")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../KairoAssets" "${CMAKE_CURRENT_BINARY_DIR}/_deps/KairoAssets")
    else()
        include(FetchContent)
        FetchContent_Declare(KairoAssets
            GIT_REPOSITORY https://github.com/swayam8624/KairoAssets.git
            GIT_TAG ${KAIRO_RENDERER_ASSETS_REVISION})
        FetchContent_MakeAvailable(KairoAssets)
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

set(KAIRO_RENDERER_MODULES
    GraphicsBackend.cppm
    RendererTypes.cppm
    RenderGraph.cppm
    Camera.cppm
    Mesh.cppm
    Skinning.cppm
    Texture.cppm
    AssetAdapters.cppm
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
    VulkanTexture.cppm
    VulkanDescriptor.cppm
    VulkanDepth.cppm
    VulkanViewportTarget.cppm
    VulkanShadowMap.cppm
    VulkanBackendContext.cppm
    VulkanTriangle.cppm
    VulkanRenderer.cppm
    Renderer.cppm
    KairoRenderer.cppm)
list(INSERT KAIRO_RENDERER_MODULES -2
    OpenGLShaders.cppm
    OpenGLRenderer.cppm)
if(APPLE)
    list(INSERT KAIRO_RENDERER_MODULES -2 MetalRenderer.cppm)
endif()
if(WIN32)
    list(INSERT KAIRO_RENDERER_MODULES -2 Direct3D12Renderer.cppm)
endif()

add_library(KairoRenderer)
target_sources(KairoRenderer PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES
    ${KAIRO_RENDERER_MODULES})
add_dependencies(KairoRenderer KairoRendererShaders)
target_compile_definitions(KairoRenderer PRIVATE KAIRO_RENDERER_SHADER_DIR="${KAIRO_RENDERER_SHADER_OUTPUT_DIR}")
target_compile_definitions(KairoRenderer PUBLIC
    KAIRO_RENDERER_HAS_VULKAN_BACKEND=1
    KAIRO_RENDERER_HAS_OPENGL_BACKEND=1)
if(APPLE)
    target_sources(KairoRenderer PRIVATE detail/MetalBackend.mm)
    set_source_files_properties(detail/MetalBackend.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    target_compile_definitions(KairoRenderer PUBLIC
        KAIRO_RENDERER_HAS_METAL_BACKEND=1)
    target_compile_definitions(KairoRenderer PRIVATE KAIRO_RENDERER_MOLTENVK_ICD_FILE="${KAIRO_MOLTENVK_ICD_FILE}")
endif()
if(WIN32)
    target_sources(KairoRenderer PRIVATE detail/Direct3D12Backend.cpp)
    target_compile_definitions(KairoRenderer PUBLIC
        KAIRO_RENDERER_HAS_D3D12_BACKEND=1)
endif()
target_compile_definitions(KairoRenderer PUBLIC $<$<BOOL:${KAIRO_RENDERER_ENABLE_VALIDATION}>:KAIRO_RENDERER_ENABLE_VALIDATION=1>)
target_link_libraries(KairoRenderer PUBLIC KairoMath KairoGeometry KairoAssets
    glfw Vulkan::Vulkan OpenGL::GL glad)
if(APPLE)
    target_link_libraries(KairoRenderer PUBLIC
        "-framework Metal"
        "-framework AppKit"
        "-framework QuartzCore"
        "-framework CoreFoundation"
        "-framework IOKit"
        "-framework IOSurface")
endif()
if(WIN32)
    target_link_libraries(KairoRenderer PUBLIC d3d12 dxgi d3dcompiler)
endif()

if(KAIRO_RENDERER_BUILD_PHYSICS_BRIDGE)
    if(NOT TARGET KairoPhysicsEngine)
        set(KAIRO_PHYSICS_ENGINE_SOURCE_DIR "" CACHE PATH "Path to a local KairoPhysicsEngine checkout")
        if(KAIRO_PHYSICS_ENGINE_SOURCE_DIR AND EXISTS "${KAIRO_PHYSICS_ENGINE_SOURCE_DIR}/CMakeLists.txt")
            add_subdirectory("${KAIRO_PHYSICS_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/KairoPhysicsEngine")
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../Foundation/KairoPhysicsEngine/CMakeLists.txt")
            add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../Foundation/KairoPhysicsEngine" "${CMAKE_CURRENT_BINARY_DIR}/_deps/KairoPhysicsEngine")
        else()
            include(FetchContent)
            FetchContent_Declare(KairoPhysicsEngine
                GIT_REPOSITORY https://github.com/swayam8624/KairoPhysicsEngine.git
                GIT_TAG ${KAIRO_RENDERER_PHYSICS_REVISION}
                GIT_SHALLOW TRUE)
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
    add_executable(KairoRendererTests
        tests/RendererTests.cpp
        tests/RenderGraphExecutionPlanTests.cpp
        tests/SkinningContractTests.cpp)
    target_link_libraries(KairoRendererTests PRIVATE KairoRenderer Catch2::Catch2WithMain)
    enable_testing()
    add_test(NAME KairoRendererTests COMMAND KairoRendererTests)
    if(TARGET KairoRendererPhysicsDebug)
        add_executable(KairoRendererPhysicsDebugTests tests/PhysicsDebugBridgeTests.cpp)
        target_link_libraries(KairoRendererPhysicsDebugTests PRIVATE KairoRendererPhysicsDebug Catch2::Catch2WithMain)
        add_test(NAME KairoRendererPhysicsDebugTests COMMAND KairoRendererPhysicsDebugTests)
    endif()
endif()