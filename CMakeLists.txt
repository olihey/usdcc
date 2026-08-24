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

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(qtadvanceddocking-qt6 CONFIG REQUIRED)

set(PYBIND11_FINDPYTHON ON)
find_package(pybind11 REQUIRED)

# OpenUSD is provided externally (see docs/PLAN.md, "Technical Requirements"):
# pass -DUSD_INSTALL=<path-to-usd-install> to point at a custom OpenUSD build.
# Not REQUIRED yet — USD integration lands in milestone M2 — so the app shell
# can build standalone before then.
find_package(USD)
if(USD_FOUND)
    message(STATUS "OpenUSD ${PXR_VERSION} found at ${USD_INSTALL}")
else()
    message(STATUS "OpenUSD not found; pass -DUSD_INSTALL=<path> to enable USD-dependent "
                    "targets once they exist (see docs/PLAN.md, milestone M2).")
endif()

add_subdirectory(src/cpp/core)
add_subdirectory(src/cpp/usd)
add_subdirectory(src/cpp/tools)
add_subdirectory(src/cpp/ui)
add_subdirectory(src/cpp/app)
