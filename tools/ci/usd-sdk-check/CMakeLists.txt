# Checks that a prebuilt OpenUSD install works as an SDK for plugin authors
# (docs/CI.md section 2): from wherever it was unzipped, CMake finds it, a small
# OpenUSD plugin (a file format) compiles and links against it, and OpenUSD loads
# that plugin and reads a file through it.
#
#     cmake -S tools/ci/usd-sdk-check -B <build> -DUSD_INSTALL=<install>
#     cmake --build <build> --config Release
#     ctest --test-dir <build> -C Release --output-on-failure
#
# Standalone on purpose: it isn't part of usdcc's build and uses nothing of it.
cmake_minimum_required(VERSION 3.21)
project(usd_sdk_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # The prebuilt OpenUSD is Release-only, so plugins use the release CRT with
    # iterator debugging off in every configuration, as usdcc does (its top-level
    # CMakeLists.txt).
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    add_compile_definitions(_ITERATOR_DEBUG_LEVEL=0)
endif()
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(USD_INSTALL "" CACHE PATH "The OpenUSD install to check (holding pxrConfig.cmake)")
if(NOT USD_INSTALL)
    message(FATAL_ERROR "Pass -DUSD_INSTALL=<OpenUSD install>")
endif()
# On the prefix path, not just a hint for pxr: pxrConfig.cmake finds some of its
# own dependencies (OpenSubdiv, TBB) with plain find_dependency() calls, and they
# live in the same install. usdcc's FindUSD.cmake does the same.
list(PREPEND CMAKE_PREFIX_PATH "${USD_INSTALL}")
find_package(pxr CONFIG REQUIRED)
message(STATUS "OpenUSD ${PXR_VERSION} from ${PXR_CMAKE_DIR}")

# Both land in one folder, with the plugin's plugInfo.json next to them.
set(_out "${CMAKE_BINARY_DIR}/out/$<CONFIG>")

add_library(usdccSdkCheck SHARED sdk_check_format.cpp)
target_link_libraries(usdccSdkCheck PRIVATE sdf tf)
# TF_REGISTRY_FUNCTION names the library it's in through these.
target_compile_definitions(usdccSdkCheck PRIVATE
    MFB_PACKAGE_NAME=usdccSdkCheck MFB_ALT_PACKAGE_NAME=usdccSdkCheck)
set_target_properties(usdccSdkCheck PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${_out}")
file(GENERATE OUTPUT "${_out}/plugInfo.json" CONTENT [[{
    "Plugins": [{
        "Type": "library",
        "Name": "usdccSdkCheck",
        "Root": ".",
        "LibraryPath": "usdccSdkCheck.dll",
        "ResourcePath": ".",
        "Info": {
            "Types": {
                "UsdccSdkCheckFileFormat": {
                    "bases": ["SdfFileFormat"],
                    "displayName": "usdcc SDK check",
                    "extensions": ["usdccsdkcheck"],
                    "formatId": "usdccsdkcheck",
                    "primary": true,
                    "target": "usd"
                }
            }
        }
    }]
}
]])

add_executable(usd_sdk_check sdk_check.cpp)
target_link_libraries(usd_sdk_check PRIVATE usd usdGeom)
set_target_properties(usd_sdk_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${_out}")
add_dependencies(usd_sdk_check usdccSdkCheck)

enable_testing()
add_test(NAME usd_sdk_check COMMAND usd_sdk_check)
if(WIN32)
    # OpenUSD's DLLs are in its bin\ and lib\, and it links the Python it was
    # built with, whose DLL sits next to python.exe.
    get_filename_component(_python_dir "${Python3_EXECUTABLE}" DIRECTORY)
    set_tests_properties(usd_sdk_check PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${PXR_CMAKE_DIR}/bin;PATH=path_list_prepend:${PXR_CMAKE_DIR}/lib;PATH=path_list_prepend:${_python_dir}")
endif()
