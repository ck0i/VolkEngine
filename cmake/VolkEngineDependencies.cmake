include_guard(GLOBAL)

include(FetchContent)
find_package(PkgConfig QUIET)
include("${CMAKE_CURRENT_LIST_DIR}/VolkEngineBootstrap.cmake")

set(FETCHCONTENT_QUIET FALSE)
set(CMAKE_TLS_VERIFY ON)

option(VOLKENGINE_FETCH_MISSING_DEPS "Fetch source dependencies when package discovery fails" ON)

set(VOLKENGINE_GLFW_VERSION "3.4" CACHE STRING "Pinned GLFW version")
set(VOLKENGINE_SPDLOG_VERSION "1.15.3" CACHE STRING "Pinned spdlog version")
set(VOLKENGINE_VMA_VERSION "3.4.0" CACHE STRING "Pinned VulkanMemoryAllocator version")
set(VOLKENGINE_IMGUI_VERSION "1.92.8" CACHE STRING "Pinned Dear ImGui version")
set(VOLKENGINE_STB_COMMIT "31c1ad37456438565541f4919958214b6e762fb4" CACHE STRING "Pinned stb commit")

function(volkengine_parse_os_release out_id out_like out_name)
    set(_id "unknown")
    set(_id_like "")
    set(_name "Linux")

    if(EXISTS "/etc/os-release")
        file(STRINGS "/etc/os-release" _os_release_lines REGEX "^(ID|ID_LIKE|NAME)=")
        foreach(_line IN LISTS _os_release_lines)
            if(_line MATCHES "^ID=(.*)$")
                set(_id "${CMAKE_MATCH_1}")
            elseif(_line MATCHES "^ID_LIKE=(.*)$")
                set(_id_like "${CMAKE_MATCH_1}")
            elseif(_line MATCHES "^NAME=(.*)$")
                set(_name "${CMAKE_MATCH_1}")
            endif()
        endforeach()
        foreach(_field _id _id_like _name)
            string(REGEX REPLACE "^\"|\"$" "" ${_field} "${${_field}}")
        endforeach()
    endif()

    set(${out_id} "${_id}" PARENT_SCOPE)
    set(${out_like} "${_id_like}" PARENT_SCOPE)
    set(${out_name} "${_name}" PARENT_SCOPE)
endfunction()

function(volkengine_linux_window_dependency_help out_var)
    volkengine_parse_os_release(_id _id_like _name)
    set(_family "generic")
    set(_tokens "${_id};${_id_like}")
    if(_tokens MATCHES "debian|ubuntu|linuxmint|pop")
        set(_family "debian")
    elseif(_tokens MATCHES "fedora|rhel|centos|rocky|almalinux")
        set(_family "fedora")
    elseif(_tokens MATCHES "arch|manjaro|cachyos|endeavouros")
        set(_family "arch")
    elseif(_tokens MATCHES "suse|opensuse")
        set(_family "suse")
    elseif(_tokens MATCHES "alpine")
        set(_family "alpine")
    endif()

    if(_family STREQUAL "debian")
        set(_command "sudo apt update && sudo apt install -y pkg-config xorg-dev libwayland-dev libxkbcommon-dev wayland-protocols extra-cmake-modules")
    elseif(_family STREQUAL "fedora")
        set(_command "sudo dnf install -y pkgconf-pkg-config libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel wayland-devel libxkbcommon-devel wayland-protocols-devel extra-cmake-modules")
    elseif(_family STREQUAL "arch")
        set(_command "sudo pacman -S --needed pkgconf libx11 libxrandr libxinerama libxcursor libxi wayland libxkbcommon wayland-protocols extra-cmake-modules")
    elseif(_family STREQUAL "suse")
        set(_command "sudo zypper install -y pkgconf-pkg-config libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel wayland-devel libxkbcommon-devel wayland-protocols-devel extra-cmake-modules")
    elseif(_family STREQUAL "alpine")
        set(_command "sudo apk add pkgconf libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev wayland-dev libxkbcommon-dev wayland-protocols extra-cmake-modules")
    else()
        set(_command "Install pkg-config plus X11 development packages (X11, Xrandr, Xinerama, Xcursor, Xi) or Wayland development packages (wayland-client, wayland-cursor, wayland-egl, wayland-scanner, xkbcommon, wayland-protocols).")
    endif()

    set(${out_var}
        "Detected Linux distro: ${_name} (ID=${_id}, ID_LIKE=${_id_like})\n"
        "Fetched GLFW still needs one native Linux window backend at compile time. CMake will not run sudo/package managers from configure.\n"
        "Suggested command: ${_command}"
        PARENT_SCOPE)
endfunction()

function(volkengine_configure_fetched_glfw_linux_backends)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif()

    set(_x11_ok FALSE)
    find_package(X11 QUIET)
    if(X11_FOUND AND X11_Xrandr_FOUND AND X11_Xinerama_FOUND AND X11_Xcursor_FOUND AND X11_Xi_FOUND)
        set(_x11_ok TRUE)
    endif()

    set(_wayland_ok FALSE)
    if(PkgConfig_FOUND)
        pkg_check_modules(VOLKENGINE_WAYLAND_CLIENT QUIET wayland-client)
        pkg_check_modules(VOLKENGINE_WAYLAND_CURSOR QUIET wayland-cursor)
        pkg_check_modules(VOLKENGINE_WAYLAND_EGL QUIET wayland-egl)
        pkg_check_modules(VOLKENGINE_XKBCOMMON QUIET xkbcommon)
        pkg_check_modules(VOLKENGINE_WAYLAND_PROTOCOLS QUIET wayland-protocols)
        find_program(VOLKENGINE_WAYLAND_SCANNER NAMES wayland-scanner)
        if(VOLKENGINE_WAYLAND_CLIENT_FOUND AND VOLKENGINE_WAYLAND_CURSOR_FOUND AND VOLKENGINE_WAYLAND_EGL_FOUND AND VOLKENGINE_XKBCOMMON_FOUND AND VOLKENGINE_WAYLAND_PROTOCOLS_FOUND AND VOLKENGINE_WAYLAND_SCANNER)
            set(_wayland_ok TRUE)
        endif()
    endif()

    if(NOT _x11_ok AND NOT _wayland_ok)
        volkengine_linux_window_dependency_help(_help)
        message(FATAL_ERROR "GLFW was not found as an installed package, and the repo-local GLFW fallback cannot compile without Linux window-system development files.\n${_help}")
    endif()

    set(GLFW_BUILD_X11 ${_x11_ok} CACHE BOOL "Build GLFW X11 backend" FORCE)
    set(GLFW_BUILD_WAYLAND ${_wayland_ok} CACHE BOOL "Build GLFW Wayland backend" FORCE)
    message(STATUS "Fetched GLFW Linux backends: X11=${_x11_ok}, Wayland=${_wayland_ok}")
endfunction()

function(volkengine_find_glslc out_var)
    if(DEFINED GLSLC_EXECUTABLE AND EXISTS "${GLSLC_EXECUTABLE}")
        set(${out_var} "${GLSLC_EXECUTABLE}" PARENT_SCOPE)
        return()
    endif()

    set(_hints "")
    if(DEFINED ENV{VULKAN_SDK})
        list(APPEND _hints "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    endif()
    if(DEFINED VOLKENGINE_BOOTSTRAPPED_VULKAN_SDK_ROOT)
        list(APPEND _hints "${VOLKENGINE_BOOTSTRAPPED_VULKAN_SDK_ROOT}/Bin" "${VOLKENGINE_BOOTSTRAPPED_VULKAN_SDK_ROOT}/bin")
    endif()

    find_program(_glslc NAMES glslc HINTS ${_hints} NO_CACHE)
    set(${out_var} "${_glslc}" PARENT_SCOPE)
endfunction()

function(volkengine_locate_vulkan_sdk search_root out_sdk_root out_bin_dir out_include_dir out_library out_glslc)
    set(_sdk_root "")
    set(_bin_dir "")
    set(_include_dir "")
    set(_library "")
    set(_glslc "")

    find_path(_candidate_include_dir
        NAMES vulkan/vulkan.h
        HINTS "${search_root}"
        PATH_SUFFIXES
            include Include
            x86_64/include x86_64/Include
            ${VOLKENGINE_VULKAN_SDK_LINUX_VERSION}/x86_64/include
            ${VOLKENGINE_VULKAN_SDK_LINUX_VERSION}/x86_64/Include
            ${VOLKENGINE_VULKAN_SDK_WINDOWS_VERSION}/Include
        NO_DEFAULT_PATH)

    find_program(_candidate_glslc
        NAMES glslc glslc.exe
        HINTS "${search_root}"
        PATH_SUFFIXES
            bin Bin
            x86_64/bin x86_64/Bin
            ${VOLKENGINE_VULKAN_SDK_LINUX_VERSION}/x86_64/bin
            ${VOLKENGINE_VULKAN_SDK_LINUX_VERSION}/x86_64/Bin
            ${VOLKENGINE_VULKAN_SDK_WINDOWS_VERSION}/Bin
        NO_DEFAULT_PATH)

    if(WIN32)
        set(_vulkan_library_names vulkan-1)
    else()
        set(_vulkan_library_names vulkan)
    endif()
    find_library(_candidate_vulkan_library
        NAMES ${_vulkan_library_names}
        HINTS "${search_root}"
        PATH_SUFFIXES
            lib Lib lib64
            x86_64/lib x86_64/Lib x86_64/lib64
            ${VOLKENGINE_VULKAN_SDK_LINUX_VERSION}/x86_64/lib
            ${VOLKENGINE_VULKAN_SDK_LINUX_VERSION}/x86_64/lib64
            x86_64/lib/VulkanLoader/lib
            ${VOLKENGINE_VULKAN_SDK_LINUX_VERSION}/x86_64/lib/VulkanLoader/lib
            ${VOLKENGINE_VULKAN_SDK_WINDOWS_VERSION}/Lib
        NO_DEFAULT_PATH)

    if(_candidate_include_dir)
        get_filename_component(_sdk_root "${_candidate_include_dir}" DIRECTORY)
        set(_include_dir "${_candidate_include_dir}")
    endif()
    if(_candidate_glslc)
        set(_glslc "${_candidate_glslc}")
        get_filename_component(_bin_dir "${_candidate_glslc}" DIRECTORY)
    endif()
    if(_candidate_vulkan_library)
        set(_library "${_candidate_vulkan_library}")
    endif()

    set(${out_sdk_root} "${_sdk_root}" PARENT_SCOPE)
    set(${out_bin_dir} "${_bin_dir}" PARENT_SCOPE)
    set(${out_include_dir} "${_include_dir}" PARENT_SCOPE)
    set(${out_library} "${_library}" PARENT_SCOPE)
    set(${out_glslc} "${_glslc}" PARENT_SCOPE)
endfunction()

function(volkengine_register_vulkan_sdk sdk_root bin_dir include_dir library glslc)
    if(sdk_root)
        set(ENV{VULKAN_SDK} "${sdk_root}")
        set(VULKAN_SDK "${sdk_root}" CACHE PATH "Resolved Vulkan SDK root" FORCE)
        list(PREPEND CMAKE_PREFIX_PATH "${sdk_root}")
        set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" PARENT_SCOPE)
    endif()
    if(bin_dir)
        volkengine_register_bootstrap_bin("${bin_dir}")
    endif()
    if(include_dir)
        set(VOLKENGINE_BOOTSTRAPPED_VULKAN_INCLUDE_DIR "${include_dir}" CACHE PATH "Bootstrapped Vulkan include directory" FORCE)
    endif()
    if(library)
        set(VOLKENGINE_BOOTSTRAPPED_VULKAN_LIBRARY "${library}" CACHE FILEPATH "Bootstrapped Vulkan loader/import library" FORCE)
    endif()
    if(glslc)
        set(GLSLC_EXECUTABLE "${glslc}" CACHE FILEPATH "glslc shader compiler" FORCE)
    endif()
endfunction()

function(volkengine_bootstrap_vulkan_sdk)
    if(NOT VOLKENGINE_BOOTSTRAP_TOOLS OR NOT VOLKENGINE_BOOTSTRAP_VULKAN_SDK)
        return()
    endif()

    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        set(_sdk_version "${VOLKENGINE_VULKAN_SDK_LINUX_VERSION}")
        set(_sdk_url "https://sdk.lunarg.com/sdk/download/${_sdk_version}/linux/vulkan_sdk.tar.xz")
        set(_sdk_sha256 "6cce33c7e5383814150c5041820769d93c65a1fd883002e5949b067045a07daa")
        set(_sdk_download "${VOLKENGINE_BOOTSTRAP_DIR}/downloads/vulkan-sdk-linux-${_sdk_version}.tar.xz")
        set(_sdk_install_dir "${VOLKENGINE_BOOTSTRAP_DIR}/vulkan-sdk/linux-${_sdk_version}")
        set(_sdk_marker "${_sdk_install_dir}/.${_sdk_version}.complete")
        volkengine_download_file("LunarG Vulkan SDK ${_sdk_version}" "${_sdk_url}" "${_sdk_sha256}" "${_sdk_download}")
        volkengine_extract_archive("LunarG Vulkan SDK ${_sdk_version}" "${_sdk_download}" "${_sdk_install_dir}" "${_sdk_marker}")
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(_sdk_version "${VOLKENGINE_VULKAN_SDK_WINDOWS_VERSION}")
        set(_sdk_url "https://sdk.lunarg.com/sdk/download/${_sdk_version}/windows/vulkan_sdk.exe")
        set(_sdk_sha256 "855b27ba05d2d8119c5114c5d4ff870ca38f2c632b11e1bb9923b9b7e6ecfe7b")
        set(_sdk_download "${VOLKENGINE_BOOTSTRAP_DIR}/downloads/vulkan-sdk-windows-${_sdk_version}.exe")
        set(_sdk_install_dir "${VOLKENGINE_BOOTSTRAP_DIR}/vulkan-sdk/windows-${_sdk_version}")
        set(_sdk_marker "${_sdk_install_dir}/.${_sdk_version}.complete")
        volkengine_download_file("LunarG Vulkan SDK ${_sdk_version}" "${_sdk_url}" "${_sdk_sha256}" "${_sdk_download}")
        if(NOT EXISTS "${_sdk_marker}")
            file(MAKE_DIRECTORY "${_sdk_install_dir}")
            message(STATUS "LunarG Vulkan SDK ${_sdk_version}: installing repo-local copy")
            execute_process(
                COMMAND "${_sdk_download}" --root "${_sdk_install_dir}" --accept-licenses --default-answer --confirm-command install copy_only=1
                RESULT_VARIABLE _sdk_install_result
                OUTPUT_VARIABLE _sdk_install_stdout
                ERROR_VARIABLE _sdk_install_stderr)
            if(NOT _sdk_install_result EQUAL 0)
                message(FATAL_ERROR "LunarG Vulkan SDK ${_sdk_version}: installer failed with ${_sdk_install_result}\n${_sdk_install_stdout}\n${_sdk_install_stderr}")
            endif()
            file(WRITE "${_sdk_marker}" "LunarG Vulkan SDK ${_sdk_version}\n${_sdk_download}\n")
        endif()
    else()
        message(STATUS "Vulkan SDK bootstrap is only implemented for Linux and Windows hosts; skipping ${CMAKE_HOST_SYSTEM_NAME}")
        return()
    endif()

    volkengine_locate_vulkan_sdk("${_sdk_install_dir}" _sdk_root _sdk_bin_dir _sdk_include_dir _sdk_library _sdk_glslc)
    if(NOT _sdk_include_dir OR NOT _sdk_library OR NOT _sdk_glslc)
        message(FATAL_ERROR "LunarG Vulkan SDK ${_sdk_version}: install completed, but expected include/library/glslc paths were not found under ${_sdk_install_dir}")
    endif()

    volkengine_register_vulkan_sdk("${_sdk_root}" "${_sdk_bin_dir}" "${_sdk_include_dir}" "${_sdk_library}" "${_sdk_glslc}")
    message(STATUS "Using bootstrapped Vulkan SDK: ${_sdk_root}")
endfunction()

function(volkengine_create_bootstrapped_vulkan_target)
    if(TARGET Vulkan::Vulkan)
        return()
    endif()
    if(VOLKENGINE_BOOTSTRAPPED_VULKAN_INCLUDE_DIR AND VOLKENGINE_BOOTSTRAPPED_VULKAN_LIBRARY)
        add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
        set_target_properties(Vulkan::Vulkan PROPERTIES
            IMPORTED_LOCATION "${VOLKENGINE_BOOTSTRAPPED_VULKAN_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${VOLKENGINE_BOOTSTRAPPED_VULKAN_INCLUDE_DIR}")
        set(Vulkan_FOUND TRUE PARENT_SCOPE)
        set(Vulkan_INCLUDE_DIRS "${VOLKENGINE_BOOTSTRAPPED_VULKAN_INCLUDE_DIR}" PARENT_SCOPE)
        set(Vulkan_LIBRARIES "${VOLKENGINE_BOOTSTRAPPED_VULKAN_LIBRARY}" PARENT_SCOPE)
    endif()
endfunction()

function(volkengine_vulkan_failure message_prefix)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        set(_runtime_note "Linux runtime note: a Vulkan-capable GPU driver/ICD is still required to run the sandbox. CMake can provide headers, import libraries, and glslc, but it cannot safely install kernel/display drivers.")
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(_runtime_note "Windows runtime note: a Vulkan-capable GPU driver is still required to run the sandbox. CMake can provide SDK files, but it cannot install or update GPU drivers.")
    else()
        set(_runtime_note "Install a Vulkan SDK/runtime for ${CMAKE_HOST_SYSTEM_NAME}.")
    endif()
    message(FATAL_ERROR "${message_prefix}\n${_runtime_note}")
endfunction()

function(volkengine_resolve_vulkan)
    find_package(Vulkan QUIET)
    volkengine_find_glslc(_glslc)

    if(NOT Vulkan_FOUND OR NOT TARGET Vulkan::Vulkan OR NOT _glslc)
        volkengine_bootstrap_vulkan_sdk()
        unset(Vulkan_INCLUDE_DIR CACHE)
        unset(Vulkan_LIBRARY CACHE)
        unset(Vulkan_GLSLC_EXECUTABLE CACHE)
        find_package(Vulkan QUIET)
        volkengine_find_glslc(_glslc)
    endif()

    if(NOT Vulkan_FOUND OR NOT TARGET Vulkan::Vulkan)
        volkengine_create_bootstrapped_vulkan_target()
    endif()

    if(NOT Vulkan_FOUND OR NOT TARGET Vulkan::Vulkan)
        volkengine_vulkan_failure("Vulkan headers/loader were not found, and repo-local Vulkan SDK bootstrap did not produce a usable Vulkan::Vulkan target.")
    endif()
    if(NOT _glslc)
        volkengine_vulkan_failure("glslc was not found, and repo-local Vulkan SDK bootstrap did not produce a shader compiler.")
    endif()

    set(GLSLC_EXECUTABLE "${_glslc}" CACHE FILEPATH "glslc shader compiler" FORCE)
    get_filename_component(_glslc_bin_dir "${_glslc}" DIRECTORY)
    volkengine_register_bootstrap_bin_if_repo_local("${_glslc_bin_dir}")
    message(STATUS "Using glslc: ${GLSLC_EXECUTABLE}")
endfunction()

function(volkengine_resolve_glfw)
    find_package(glfw3 CONFIG QUIET)
    if(TARGET glfw)
        set(VOLKENGINE_GLFW_TARGET glfw PARENT_SCOPE)
        message(STATUS "Using installed GLFW target: glfw")
        return()
    elseif(TARGET glfw3)
        set(VOLKENGINE_GLFW_TARGET glfw3 PARENT_SCOPE)
        message(STATUS "Using installed GLFW target: glfw3")
        return()
    endif()

    if(PkgConfig_FOUND)
        pkg_check_modules(GLFW3 QUIET IMPORTED_TARGET glfw3)
    endif()
    if(TARGET PkgConfig::GLFW3)
        set(VOLKENGINE_GLFW_TARGET PkgConfig::GLFW3 PARENT_SCOPE)
        message(STATUS "Using pkg-config GLFW target: PkgConfig::GLFW3")
        return()
    endif()

    if(NOT VOLKENGINE_FETCH_MISSING_DEPS)
        message(FATAL_ERROR "GLFW was not found and VOLKENGINE_FETCH_MISSING_DEPS=OFF")
    endif()

    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    volkengine_configure_fetched_glfw_linux_backends()
    FetchContent_Declare(glfw
        URL "https://github.com/glfw/glfw/releases/download/${VOLKENGINE_GLFW_VERSION}/glfw-${VOLKENGINE_GLFW_VERSION}.zip"
        URL_HASH "SHA256=b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(glfw)
    set(VOLKENGINE_GLFW_TARGET glfw PARENT_SCOPE)
    message(STATUS "Using fetched GLFW ${VOLKENGINE_GLFW_VERSION}")
endfunction()

function(volkengine_resolve_spdlog)
    find_package(spdlog CONFIG QUIET)
    if(TARGET spdlog::spdlog)
        set(VOLKENGINE_SPDLOG_TARGET spdlog::spdlog PARENT_SCOPE)
        message(STATUS "Using installed spdlog target: spdlog::spdlog")
        return()
    endif()

    if(PkgConfig_FOUND)
        pkg_check_modules(SPDLOG QUIET IMPORTED_TARGET spdlog)
    endif()
    if(TARGET PkgConfig::SPDLOG)
        set(VOLKENGINE_SPDLOG_TARGET PkgConfig::SPDLOG PARENT_SCOPE)
        message(STATUS "Using pkg-config spdlog target: PkgConfig::SPDLOG")
        return()
    endif()

    if(NOT VOLKENGINE_FETCH_MISSING_DEPS)
        message(FATAL_ERROR "spdlog was not found and VOLKENGINE_FETCH_MISSING_DEPS=OFF")
    endif()

    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(spdlog
        URL "https://github.com/gabime/spdlog/archive/refs/tags/v${VOLKENGINE_SPDLOG_VERSION}.zip"
        URL_HASH "SHA256=b74274c32c8be5dba70b7006c1d41b7d3e5ff0dff8390c8b6390c1189424e094"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(spdlog)
    set(VOLKENGINE_SPDLOG_TARGET spdlog::spdlog PARENT_SCOPE)
    message(STATUS "Using fetched spdlog ${VOLKENGINE_SPDLOG_VERSION}")
endfunction()

function(volkengine_resolve_vma)
    FetchContent_Declare(VulkanMemoryAllocator
        URL "https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v${VOLKENGINE_VMA_VERSION}.zip"
        URL_HASH "SHA256=ce9a61ea5e72284ac9ccc152c214eef381ab57bfd7383f20232e5a652e3afda6"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(VulkanMemoryAllocator)
    if(NOT TARGET GPUOpen::VulkanMemoryAllocator)
        message(FATAL_ERROR "Fetched VulkanMemoryAllocator ${VOLKENGINE_VMA_VERSION} did not define GPUOpen::VulkanMemoryAllocator")
    endif()
endfunction()

function(volkengine_resolve_stb)
    FetchContent_Declare(stb
        URL "https://github.com/nothings/stb/archive/${VOLKENGINE_STB_COMMIT}.zip"
        URL_HASH "SHA256=617266695cf191a45bec2405427207011a09b057133134594b0db6ccbf9ee0b2"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(stb)
    FetchContent_GetProperties(stb)
    if(NOT stb_SOURCE_DIR)
        set(stb_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/stb-src")
    endif()
    add_library(VolkEngineStb INTERFACE)
    target_include_directories(VolkEngineStb INTERFACE "${stb_SOURCE_DIR}")
    set(VOLKENGINE_STB_TARGET VolkEngineStb PARENT_SCOPE)
    message(STATUS "Using fetched stb ${VOLKENGINE_STB_COMMIT}")
endfunction()

function(volkengine_resolve_imgui)
    if(NOT VOLKENGINE_ENABLE_IMGUI)
        return()
    endif()

    FetchContent_Declare(imgui
        URL "https://github.com/ocornut/imgui/archive/refs/tags/v${VOLKENGINE_IMGUI_VERSION}.zip"
        URL_HASH "SHA256=27765c56ab27ce47472d0bea43cf1e3301c726362ce585e99a059e3b37616870"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(imgui)
    FetchContent_GetProperties(imgui)
    if(NOT imgui_SOURCE_DIR)
        if(FETCHCONTENT_SOURCE_DIR_IMGUI)
            set(imgui_SOURCE_DIR "${FETCHCONTENT_SOURCE_DIR_IMGUI}")
        else()
            set(imgui_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/imgui-src")
        endif()
    endif()
    set(imgui_SOURCE_DIR "${imgui_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(volkengine_resolve_dependencies)
    volkengine_resolve_vulkan()
    volkengine_resolve_glfw()
    volkengine_resolve_spdlog()
    volkengine_resolve_vma()
    volkengine_resolve_stb()
    volkengine_resolve_imgui()
    set(VOLKENGINE_GLFW_TARGET "${VOLKENGINE_GLFW_TARGET}" PARENT_SCOPE)
    set(VOLKENGINE_SPDLOG_TARGET "${VOLKENGINE_SPDLOG_TARGET}" PARENT_SCOPE)
    set(VOLKENGINE_STB_TARGET "${VOLKENGINE_STB_TARGET}" PARENT_SCOPE)
    if(DEFINED imgui_SOURCE_DIR)
        set(imgui_SOURCE_DIR "${imgui_SOURCE_DIR}" PARENT_SCOPE)
    endif()
endfunction()
