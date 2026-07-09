cmake_minimum_required(VERSION 3.28)

project(VolkEngine
    VERSION 0.1.0
    DESCRIPTION "High-fidelity Vulkan renderer foundation"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(VOLKENGINE_ENABLE_VALIDATION "Request Vulkan validation layers in debug builds" ON)
option(VOLKENGINE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(VOLKENGINE_ENABLE_IMGUI "Enable Dear ImGui debug overlay" ON)

find_package(Vulkan REQUIRED)
find_program(GLSLC_EXECUTABLE
    NAMES glslc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    REQUIRED)

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

find_package(glfw3 CONFIG QUIET)
if(TARGET glfw)
    set(VOLKENGINE_GLFW_TARGET glfw)
elseif(TARGET glfw3)
    set(VOLKENGINE_GLFW_TARGET glfw3)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GLFW3 QUIET IMPORTED_TARGET glfw3)
    endif()
    if(TARGET PkgConfig::GLFW3)
        set(VOLKENGINE_GLFW_TARGET PkgConfig::GLFW3)
    else()
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG 3.4
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(glfw)
        set(VOLKENGINE_GLFW_TARGET glfw)
    endif()
endif()

find_package(spdlog CONFIG QUIET)
if(TARGET spdlog::spdlog)
    set(VOLKENGINE_SPDLOG_TARGET spdlog::spdlog)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SPDLOG QUIET IMPORTED_TARGET spdlog)
    endif()
    if(TARGET PkgConfig::SPDLOG)
        set(VOLKENGINE_SPDLOG_TARGET PkgConfig::SPDLOG)
    else()
        set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG v1.15.3
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(spdlog)
        set(VOLKENGINE_SPDLOG_TARGET spdlog::spdlog)
    endif()
endif()

FetchContent_Declare(VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

if(VOLKENGINE_ENABLE_IMGUI)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.8
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(imgui)
    add_library(VolkEngineImGui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
    target_include_directories(VolkEngineImGui
        PUBLIC
            "${imgui_SOURCE_DIR}"
            "${imgui_SOURCE_DIR}/backends")
    target_link_libraries(VolkEngineImGui
        PUBLIC
            Vulkan::Vulkan
            ${VOLKENGINE_GLFW_TARGET})
    target_compile_definitions(VolkEngineImGui
        PUBLIC
            GLFW_INCLUDE_NONE)
    if(MSVC)
        target_compile_options(VolkEngineImGui PRIVATE /w)
    else()
        target_compile_options(VolkEngineImGui PRIVATE -w)
    endif()
endif()

function(volkengine_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(VOLKENGINE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wno-missing-field-initializers)
        if(VOLKENGINE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

set(VOLKENGINE_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders")
set(VOLKENGINE_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(VOLKENGINE_ASSET_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
set(VOLKENGINE_ASSET_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets")
file(GLOB VOLKENGINE_SHADER_SOURCES CONFIGURE_DEPENDS
    "${VOLKENGINE_SHADER_SOURCE_DIR}/*.vert"
    "${VOLKENGINE_SHADER_SOURCE_DIR}/*.frag")

set(VOLKENGINE_SHADER_OUTPUTS)
foreach(shader ${VOLKENGINE_SHADER_SOURCES})
    get_filename_component(shader_name "${shader}" NAME)
    set(shader_output "${VOLKENGINE_SHADER_OUTPUT_DIR}/${shader_name}.spv")
    set(shader_depfile "${shader_output}.d")
    add_custom_command(
        OUTPUT "${shader_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${VOLKENGINE_SHADER_OUTPUT_DIR}"
        COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.3 -I "${VOLKENGINE_SHADER_SOURCE_DIR}" -MD -MF "${shader_depfile}" -MT "${shader_output}" "${shader}" -o "${shader_output}"
        DEPENDS "${shader}"
        DEPFILE "${shader_depfile}"
        COMMENT "Compiling shader ${shader_name}"
        VERBATIM)
    list(APPEND VOLKENGINE_SHADER_OUTPUTS "${shader_output}")
endforeach()
add_custom_target(VolkEngineShaders DEPENDS ${VOLKENGINE_SHADER_OUTPUTS})
add_custom_target(VolkEngineAssets
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${VOLKENGINE_ASSET_SOURCE_DIR}"
            "${VOLKENGINE_ASSET_OUTPUT_DIR}"
    COMMENT "Copying runtime assets")


add_library(VolkEngine STATIC
    engine/core/Application.cpp
    engine/core/Camera.cpp
    engine/core/FileSystem.cpp
    engine/core/Log.cpp
    engine/core/Time.cpp
    engine/platform/Window.cpp
    engine/renderer/Geometry.cpp
    engine/renderer/ImageLoader.cpp
    engine/renderer/vulkan/VmaUsage.cpp
    engine/renderer/vulkan/VulkanRenderer.cpp)

if(MSVC)
    set_source_files_properties(engine/renderer/vulkan/VmaUsage.cpp PROPERTIES COMPILE_OPTIONS /w)
else()
    set_source_files_properties(engine/renderer/vulkan/VmaUsage.cpp PROPERTIES COMPILE_OPTIONS -w)
endif()

add_dependencies(VolkEngine VolkEngineShaders VolkEngineAssets)

target_include_directories(VolkEngine
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/engine")

target_link_libraries(VolkEngine
    PUBLIC
        Vulkan::Vulkan
        GPUOpen::VulkanMemoryAllocator
        ${VOLKENGINE_GLFW_TARGET}
        ${VOLKENGINE_SPDLOG_TARGET}
        $<$<BOOL:${VOLKENGINE_ENABLE_IMGUI}>:VolkEngineImGui>)

target_compile_definitions(VolkEngine
    PUBLIC
        GLFW_INCLUDE_NONE
        VOLKENGINE_SHADER_DIR="${VOLKENGINE_SHADER_OUTPUT_DIR}"
        VOLKENGINE_ASSET_DIR="${VOLKENGINE_ASSET_OUTPUT_DIR}"
        VOLKENGINE_VALIDATION=$<BOOL:${VOLKENGINE_ENABLE_VALIDATION}>
        $<$<CONFIG:Debug>:VOLKENGINE_DEBUG_BUILD=1>
        $<$<NOT:$<CONFIG:Debug>>:VOLKENGINE_DEBUG_BUILD=0>
        $<$<BOOL:${VOLKENGINE_ENABLE_IMGUI}>:VOLKENGINE_ENABLE_IMGUI=1>
        $<$<NOT:$<BOOL:${VOLKENGINE_ENABLE_IMGUI}>>:VOLKENGINE_ENABLE_IMGUI=0>)

volkengine_target_warnings(VolkEngine)

add_executable(VolkEngineSandbox
    samples/sandbox/main.cpp)

target_link_libraries(VolkEngineSandbox PRIVATE VolkEngine)
add_custom_target(VolkEngineRuntimeShaders
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${VOLKENGINE_SHADER_OUTPUT_DIR}"
            "$<TARGET_FILE_DIR:VolkEngineSandbox>/shaders"
    DEPENDS ${VOLKENGINE_SHADER_OUTPUTS}
    COMMENT "Copying compiled shaders next to sandbox executable")
add_dependencies(VolkEngineRuntimeShaders VolkEngineShaders)
add_dependencies(VolkEngineRuntimeShaders VolkEngineAssets)
add_dependencies(VolkEngineSandbox VolkEngineRuntimeShaders)
volkengine_target_warnings(VolkEngineSandbox)

enable_testing()

add_executable(VolkEngineSceneRendererTest
    tests/SceneRendererCpuTests.cpp)
target_include_directories(VolkEngineSceneRendererTest
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/engine")
volkengine_target_warnings(VolkEngineSceneRendererTest)
add_test(NAME SceneRendererCpu
    COMMAND VolkEngineSceneRendererTest)
add_executable(VolkEngineFrameGraphCpuTest
    tests/FrameGraphCpuTests.cpp)
target_include_directories(VolkEngineFrameGraphCpuTest
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/engine")
volkengine_target_warnings(VolkEngineFrameGraphCpuTest)
add_test(NAME FrameGraphCpu
    COMMAND VolkEngineFrameGraphCpuTest)
add_executable(VolkEngineImageLoaderCpuTest
    tests/ImageLoaderCpuTests.cpp
    engine/renderer/ImageLoader.cpp)
target_include_directories(VolkEngineImageLoaderCpuTest
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/engine")
volkengine_target_warnings(VolkEngineImageLoaderCpuTest)
add_test(NAME ImageLoaderCpu
    COMMAND VolkEngineImageLoaderCpuTest)
