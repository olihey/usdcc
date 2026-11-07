cmake_minimum_required(VERSION 3.21)
if(NOT TARGET usdcc::plugin_sdk)
    project(usdcc_rtx_render_native LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    find_package(UsdccPlugin REQUIRED)
endif()
if(NOT WIN32)
    message(FATAL_ERROR "The native RTX M1 prototype currently supports Windows only")
endif()

# No implicit SDK download and no app-startup dependency on vendor DLLs. The
# DLL imports are delayed until an explicit Render request.
set(VCPKG_APPLOCAL_DEPS OFF)
set(USDCC_RTX_OVRTX_ROOT "" CACHE PATH "ovrtx 0.6.0 native package root")
set(USDCC_RTX_OVSTAGE_ROOT "" CACHE PATH "ovstage 0.3.0 native package root")
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../../deps/rtx-native.json" _pins)
foreach(_package ovrtx ovstage)
    string(TOUPPER "${_package}" _upper)
    set(_root "${USDCC_RTX_${_upper}_ROOT}")
    if(NOT EXISTS "${_root}/${_package}-build-info.json")
        message(FATAL_ERROR "Set USDCC_RTX_${_upper}_ROOT to the installed native package")
    endif()
    file(READ "${_root}/${_package}-build-info.json" _info)
    string(JSON _major GET "${_info}" version_major)
    string(JSON _minor GET "${_info}" version_minor)
    string(JSON _patch GET "${_info}" version_patch)
    if(_package STREQUAL "ovrtx")
        set(_expected_minor 6)
    else()
        set(_expected_minor 3)
    endif()
    if(NOT "${_major}.${_minor}.${_patch}" STREQUAL "0.${_expected_minor}.0")
        message(FATAL_ERROR "Native RTX M1 requires ${_package} 0.${_expected_minor}.0")
    endif()
    find_package(${_package} CONFIG REQUIRED PATHS "${_root}/lib/cmake/${_package}" NO_DEFAULT_PATH)
    file(SHA256 "${_root}/${_package}-build-info.json" _identity_hash)
    file(SHA256 "${_root}/bin/${_package}-dynamic.dll" _loader_hash)
    string(JSON _build GET "${_info}" build_string)
    string(JSON _expected_build GET "${_pins}" ${_package} build)
    string(JSON _expected_loader GET "${_pins}" ${_package} loader_sha256)
    if(NOT _build STREQUAL _expected_build OR NOT _loader_hash STREQUAL _expected_loader)
        message(FATAL_ERROR "${_package} differs from the tested native RTX SDK pin in deps/rtx-native.json")
    endif()
    # Record the exact locally supplied SDK identities for reproducible probes.
    string(APPEND _identities "${_package}: ${_info}\nidentity SHA256: ${_identity_hash}\nloader SHA256: ${_loader_hash}\n")
endforeach()

# Qt in usdcc is built without Vulkan. Use a native QWindow and a plugin-local
# loader; the app and other plugins acquire no Vulkan development dependency.
include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(usdcc_vulkan_headers
    SOURCE_SUBDIR usdcc-headers-only
    URL https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/refs/tags/v1.4.304
    URL_HASH SHA256=5e1a06b3f27e7581b55d1dea176fd97ee0a2f299406db2f437c1d2f297ceba5b)
FetchContent_Declare(usdcc_volk
    SOURCE_SUBDIR usdcc-headers-only
    URL https://codeload.github.com/zeux/volk/tar.gz/refs/tags/1.4.304
    URL_HASH SHA256=ab3d4a8ccaeb32652259cdd008399504a41792675b0421d90b67729ee274746f)
FetchContent_MakeAvailable(usdcc_vulkan_headers usdcc_volk)
find_package(CUDAToolkit 12 REQUIRED)

usdcc_add_plugin(rtx_render_native
    ID org.usdcc.rtx-render NAME "RTX Render" VERSION 0.3.0
    DESCRIPTION "Native C++ RTX Render (development)"
    SOURCES plugin.cpp rtx_engine.cpp rtx_engine.h stage_bridge.cpp stage_bridge.h
        render_types.h gpu_bridge.h cuda_driver.h vulkan_window.cpp vulkan_window.h)
target_include_directories(rtx_render_native PRIVATE
    "${usdcc_vulkan_headers_SOURCE_DIR}/include" "${usdcc_volk_SOURCE_DIR}" "${CUDAToolkit_INCLUDE_DIRS}")
target_compile_definitions(rtx_render_native PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(rtx_render_native PRIVATE ovrtx::ovrtx ovstage::ovstage delayimp)
target_link_options(rtx_render_native PRIVATE /DELAYLOAD:ovrtx-dynamic.dll /DELAYLOAD:ovstage-dynamic.dll)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../tests/rtx_native_sdk_probe.cpp")
    add_executable(rtx_native_sdk_probe EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_LIST_DIR}/../../../tests/rtx_native_sdk_probe.cpp" rtx_engine.cpp vulkan_window.cpp)
    target_include_directories(rtx_native_sdk_probe PRIVATE "${CMAKE_CURRENT_LIST_DIR}"
        "${usdcc_vulkan_headers_SOURCE_DIR}/include" "${usdcc_volk_SOURCE_DIR}" "${CUDAToolkit_INCLUDE_DIRS}")
    target_compile_definitions(rtx_native_sdk_probe PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(rtx_native_sdk_probe PRIVATE usdcc_core Qt6::Gui ovrtx::ovrtx ovstage::ovstage delayimp)
    target_link_options(rtx_native_sdk_probe PRIVATE /DELAYLOAD:ovrtx-dynamic.dll /DELAYLOAD:ovstage-dynamic.dll)
endif()

# Development runtime locator, deliberately separate from the manifest. Copying
# the plugin without these SDKs must still allow activation and an actionable UI.
file(TO_CMAKE_PATH "${USDCC_RTX_OVRTX_ROOT}" _ovrtx_root)
file(TO_CMAKE_PATH "${USDCC_RTX_OVSTAGE_ROOT}" _ovstage_root)
file(WRITE "${CMAKE_BINARY_DIR}/plugins/rtx_render_native/runtime.json"
    "{\n  \"ovrtx\": \"${_ovrtx_root}\",\n  \"ovstage\": \"${_ovstage_root}\"\n}\n")
file(WRITE "${CMAKE_BINARY_DIR}/plugins/rtx_render_native/sdk-build-info.txt" "${_identities}")
