cmake_minimum_required(VERSION 3.21)

# Toolchain must be set before project(); default to the vendored vcpkg
# submodule unless the caller already supplied one (e.g. a CI cache).
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_SOURCE_DIR}/external/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

project(usdcc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE
        "RelWithDebInfo"
        CACHE STRING "Build type" FORCE)
endif()

if(WIN32)
    # windows.h's min/max macros (pulled in transitively by Qt) corrupt any
    # std::min/std::max usage in headers compiled afterward — including
    # OpenUSD's — with cryptic syntax errors. NOMINMAX suppresses them.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")

# OpenGL component: only needed for QOpenGLFunctions_1_1 (legacy
# immediate-mode drawing), used by HydraViewportWindow to render M5's gizmo
# overlays directly into the same Compatibility-profile GL context Storm
# already requires (see docs/PLAN.md milestone M5/M3).
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL)
find_package(qtadvanceddocking-qt6 CONFIG REQUIRED)

set(PYBIND11_FINDPYTHON ON)
find_package(pybind11 REQUIRED)

# OpenUSD is provided externally (see docs/PLAN.md, "Technical Requirements"):
# pass -DUSD_INSTALL=<path-to-usd-install> to point at a custom OpenUSD build.
# Not REQUIRED yet — USD integration lands in milestone M2 — so the app shell
# can build standalone before then.
#
# USD's imaging targets (e.g. garch) reference OpenGL::GL, so it must already
# be findable before pxrConfig.cmake sets up USD's own imported targets.
find_package(OpenGL)
find_package(USD)
if(USD_FOUND)
    message(STATUS "OpenUSD ${PXR_VERSION} found at ${USD_INSTALL}")
else()
    message(STATUS "OpenUSD not found; pass -DUSD_INSTALL=<path> to enable USD-dependent "
                    "targets once they exist (see docs/PLAN.md, milestone M2).")
endif()

add_subdirectory(src/cpp/core)
add_subdirectory(src/cpp/tools)
add_subdirectory(src/cpp/usd)
add_subdirectory(src/cpp/ui)
add_subdirectory(src/cpp/app)
