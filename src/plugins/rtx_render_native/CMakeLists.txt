cmake_minimum_required(VERSION 3.21)
if(NOT TARGET usdcc::plugin_sdk)
    project(usdcc_rtx_render_native LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    find_package(UsdccPlugin REQUIRED)
endif()
if(NOT WIN32)
    message(FATAL_ERROR "Native RTX Render currently supports Windows only")
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
        message(FATAL_ERROR "Native RTX requires ${_package} 0.${_expected_minor}.0")
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
string(JSON _rtx_pin_build GET "${_pins}" ovrtx build)
string(JSON _rtx_pin_sha GET "${_pins}" ovrtx loader_sha256)
string(JSON _stage_pin_build GET "${_pins}" ovstage build)
string(JSON _stage_pin_sha GET "${_pins}" ovstage loader_sha256)
configure_file(runtime_pins.h.in "${CMAKE_CURRENT_BINARY_DIR}/runtime_pins.h" @ONLY)

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
    ID org.usdcc.rtx-render NAME "RTX Render" VERSION 0.4.0
    DESCRIPTION "Native C++ RTX rendering with navigation and selection highlighting"
    SOURCES plugin.cpp rtx_engine.cpp rtx_engine.h stage_bridge.cpp stage_bridge.h
        render_types.h gpu_bridge.h gpu_frame.h cuda_driver.h vulkan_window.cpp vulkan_window.h navigation.cpp navigation.h
        rtx_panel.cpp rtx_panel.h presentation.cpp runtime_identity.cpp runtime_identity.h)
target_include_directories(rtx_render_native PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}" "${usdcc_vulkan_headers_SOURCE_DIR}/include" "${usdcc_volk_SOURCE_DIR}" "${CUDAToolkit_INCLUDE_DIRS}")
target_compile_definitions(rtx_render_native PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(rtx_render_native PRIVATE ovrtx::ovrtx ovstage::ovstage delayimp)
target_link_options(rtx_render_native PRIVATE /DELAYLOAD:ovrtx-dynamic.dll /DELAYLOAD:ovstage-dynamic.dll)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../tests/rtx_native_sdk_probe.cpp")
    add_executable(rtx_native_sdk_probe EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_LIST_DIR}/../../../tests/rtx_native_sdk_probe.cpp" rtx_engine.cpp vulkan_window.cpp runtime_identity.cpp)
    target_include_directories(rtx_native_sdk_probe PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
        "${usdcc_vulkan_headers_SOURCE_DIR}/include" "${usdcc_volk_SOURCE_DIR}" "${CUDAToolkit_INCLUDE_DIRS}")
    target_compile_definitions(rtx_native_sdk_probe PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(rtx_native_sdk_probe PRIVATE usdcc_core Qt6::Gui ovrtx::ovrtx ovstage::ovstage delayimp)
    target_link_options(rtx_native_sdk_probe PRIVATE /DELAYLOAD:ovrtx-dynamic.dll /DELAYLOAD:ovstage-dynamic.dll)
endif()

if(USDCC_RTX_GPU_TESTS)
    enable_testing()
    add_custom_target(rtx_gpu_test_bins DEPENDS rtx_native_sdk_probe)
    file(READ "${CMAKE_SOURCE_DIR}/test_data/rtx_native_m1.usda" _fixture)
    foreach(_mode realtime pathtracing minimal)
        set(_token "RealTimePathTracing")
        set(_label "Real-Time")
        if(_mode STREQUAL "pathtracing")
            set(_token "PathTracing")
            set(_label "Path Tracing")
        elseif(_mode STREQUAL "minimal")
            set(_token "MinimalRendering")
            set(_label "Minimal")
        endif()
        string(REPLACE "RealTimePathTracing" "${_token}" _scene "${_fixture}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${_mode}.usda" "${_scene}")
        add_test(NAME rtx_gpu_${_mode} COMMAND rtx_native_sdk_probe
            "${USDCC_RTX_OVRTX_ROOT}" "${USDCC_RTX_OVSTAGE_ROOT}"
            "${CMAKE_CURRENT_BINARY_DIR}/${_mode}.usda" /Render/Product "${CMAKE_CURRENT_BINARY_DIR}/${_mode}.png")
        set_tests_properties(rtx_gpu_${_mode} PROPERTIES LABELS "rtx;gpu" RESOURCE_LOCK usdcc_rtx_gpu TIMEOUT 660
            ENVIRONMENT "USDCC_RTX_PROBE_SELECTION=/World/Sphere;USDCC_RTX_PROBE_MODE=${_label};USDCC_RTX_PROBE_TIMEOUT_SECONDS=600;USDCC_RTX_VULKAN_VALIDATION=1;OMNICLIENT_HUB_MODE=disabled"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:usdcc_core>;PATH=path_list_prepend:${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin;QT_QPA_PLATFORM_PLUGIN_PATH=set:${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins/platforms")
    endforeach()
    add_test(NAME rtx_gpu_interop COMMAND rtx_native_sdk_probe --interop)
    set_tests_properties(rtx_gpu_interop PROPERTIES LABELS "rtx;gpu" RESOURCE_LOCK usdcc_rtx_gpu TIMEOUT 60
        ENVIRONMENT "USDCC_RTX_VULKAN_VALIDATION=1"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:usdcc_core>;PATH=path_list_prepend:${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin;QT_QPA_PLATFORM_PLUGIN_PATH=set:${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins/platforms")
endif()

# Development runtime locator, deliberately separate from the manifest. Copying
# the plugin without these SDKs must still allow activation and an actionable UI.
file(TO_CMAKE_PATH "${USDCC_RTX_OVRTX_ROOT}" _ovrtx_root)
file(TO_CMAKE_PATH "${USDCC_RTX_OVSTAGE_ROOT}" _ovstage_root)
file(WRITE "${CMAKE_BINARY_DIR}/plugins/rtx_render_native/runtime.json"
    "{\n  \"ovrtx\": \"${_ovrtx_root}\",\n  \"ovstage\": \"${_ovstage_root}\"\n}\n")
file(WRITE "${CMAKE_BINARY_DIR}/plugins/rtx_render_native/sdk-build-info.txt" "${_identities}")
