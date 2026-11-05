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

if(MSVC)
    # OpenUSD is built once, in Release (/MD, _ITERATOR_DEBUG_LEVEL=0), and used
    # by every configuration of usdcc. Its API passes std::vector/std::string
    # across the DLL boundary (e.g. UsdImagingGLEngine::SetSelected(const
    # SdfPathVector&)), so the app must share its STL layout and heap. A stock
    # Debug build (/MDd, _ITERATOR_DEBUG_LEVEL=2) does not: std::vector gains a
    # debug-proxy pointer, so USD reads our vectors with the wrong layout and
    # hits an access violation on *every* launch (usually swallowed, sometimes
    # fatal) — see docs/PLAN.md §6 item 28. So all configurations use the release
    # CRT with iterator debugging off. "Debug" still means no optimisation and
    # full symbols; it just doesn't get MSVC's debug-STL/heap checks.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    add_compile_definitions(_ITERATOR_DEBUG_LEVEL=0)
    # Unoptimised, a file instantiating templates for every USD value type
    # (value_type_info.cpp) has more sections than an object file allows by
    # default (C1128).
    add_compile_options(/bigobj)
    # C++ exceptions, without /EHsc's "extern C functions never throw": a C++
    # plugin's entry points are extern "C" and may throw, and the app catches
    # that and fails the plugin (docs/PLUGIN_SDK.md). Under /EHsc that's
    # undefined behaviour (warning C4297 in the plugin).
    string(REPLACE "/EHsc" "/EHs" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    # Build in parallel. /MP compiles a target's files at once; MSBuild's
    # multi-tool task does the same across projects, sharing one limit (the
    # core count) between them, so building several projects at a time
    # (`cmake --build --parallel`, as tools/build.ps1 does, or Visual Studio)
    # doesn't start cores x cores compilers.
    add_compile_options(/MP)
    set(CMAKE_VS_GLOBALS "UseMultiToolTask=true" "EnforceProcessCountAcrossBuilds=true")
    # The same goes for Qt and Qt Advanced Docking System: Qt hands ownership
    # of objects across its DLL boundary (e.g. setLayout(new QHBoxLayout) is
    # deleted from inside Qt), so they must be on the same CRT heap as the app.
    # vcpkg's *Debug* Qt/ADS DLLs use the debug CRT, so link their Release
    # builds in every configuration (done below, right after find_package()
    # has created those imported targets).
    set(USDCC_LINK_RELEASE_QT ON)
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
# already requires (see docs/PLAN.md milestone M5/M3). Svg: the Outliner's
# prim icons, rendered with QSvgRenderer (docs/PRIM_ICONS.md); no SVG
# plugins are needed for that. Test: only for the tests (key presses that go
# through the shortcut map, QTest::keyClick).
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL Svg Test)
find_package(qtadvanceddocking-qt6 CONFIG REQUIRED)

if(USDCC_LINK_RELEASE_QT)
    # Only Qt and ADS: other imported targets (e.g. USD's TBB) don't have a
    # Release/Debug pair to choose between.
    get_directory_property(_usdcc_imported IMPORTED_TARGETS)
    foreach(_target IN LISTS _usdcc_imported)
        if(_target MATCHES "^(Qt6::|ads::)")
            set_property(TARGET ${_target} PROPERTY MAP_IMPORTED_CONFIG_DEBUG "RELEASE;RELWITHDEBINFO;MINSIZEREL")
        endif()
    endforeach()
endif()

# QWindowKit: frameless window + custom title bar (usdcc::ui::MainWindow's
# title bar). Vendored as a git submodule because vcpkg has no port for it.
# Static and Widgets-only (no Quick, examples, tests or install), and without
# its system-theme "style agent" — usdcc has one fixed dark theme. Its own
# configure step also builds a small host tool (qmsetup); that is expected.
set(QWINDOWKIT_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(QWINDOWKIT_BUILD_WIDGETS ON CACHE BOOL "" FORCE)
set(QWINDOWKIT_BUILD_QUICK OFF CACHE BOOL "" FORCE)
set(QWINDOWKIT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QWINDOWKIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(QWINDOWKIT_INSTALL OFF CACHE BOOL "" FORCE)
set(QWINDOWKIT_ENABLE_STYLE_AGENT OFF CACHE BOOL "" FORCE)
# Every configure deletes and recreates QWindowKit's generated include folder
# (its CMakeLists pass FORCE to qm_sync_include), which gives qwkconfig.h a
# new timestamp even when its content is the same, and every QWindowKit
# source, and so everything linking it, would be rebuilt. The file is put
# back as it was (file(COPY) keeps the timestamp) when nothing in it changed.
set(_qwk_config "${CMAKE_BINARY_DIR}/external/qwindowkit/out/include/QWKCore/qwkconfig.h")
set(_qwk_config_saved "${CMAKE_BINARY_DIR}/CMakeFiles/usdcc_qwkconfig")
file(REMOVE_RECURSE "${_qwk_config_saved}")
if(EXISTS "${_qwk_config}")
    file(COPY "${_qwk_config}" DESTINATION "${_qwk_config_saved}")
endif()
add_subdirectory(external/qwindowkit EXCLUDE_FROM_ALL)
if(EXISTS "${_qwk_config_saved}/qwkconfig.h" AND EXISTS "${_qwk_config}")
    file(SHA256 "${_qwk_config_saved}/qwkconfig.h" _qwk_config_before)
    file(SHA256 "${_qwk_config}" _qwk_config_after)
    if(_qwk_config_before STREQUAL _qwk_config_after)
        get_filename_component(_qwk_config_dir "${_qwk_config}" DIRECTORY)
        file(COPY "${_qwk_config_saved}/qwkconfig.h" DESTINATION "${_qwk_config_dir}")
    endif()
endif()

set(PYBIND11_FINDPYTHON ON)
find_package(pybind11 REQUIRED)

# OpenUSD is provided externally (see docs/PLAN.md, "Technical Requirements"):
# pass -DUSD_INSTALL=<path-to-usd-install> to point at a custom OpenUSD build.
# REQUIRED: usdcc::ui::ViewPanel (usdcc_ui, needed by every panel and thus by
# the whole app) unconditionally requires usdcc::usd::Stage/StageManager as of
# docs/PLAN.md §6's "ViewPanel: mandatory Stage association" — there is no
# longer a meaningful "app shell builds without USD_INSTALL" mode (retiring
# that milestone-M0 property).
#
# USD's imaging targets (e.g. garch) reference OpenGL::GL, so it must already
# be findable before pxrConfig.cmake sets up USD's own imported targets.
find_package(OpenGL)
find_package(USD REQUIRED)
message(STATUS "OpenUSD ${PXR_VERSION} found at ${USD_INSTALL}")
if(USDCC_LINK_RELEASE_QT)
    # USD's targets link Python3::Python, which in Debug is vcpkg's python312_d.lib.
    # The interpreter is OpenUSD's release Python, so anything resolved from the
    # debug one (whatever a library later on the link line alone needs, such as
    # pybind11's list conversion) would run in a second Python that never started:
    # an assert, in a dialog, and a hang if nobody sees it. So release, always.
    foreach(_target Python3::Python Python3::Module)
        if(TARGET ${_target})
            set_property(TARGET ${_target} PROPERTY MAP_IMPORTED_CONFIG_DEBUG "RELEASE;RELWITHDEBINFO;MINSIZEREL;")
        endif()
    endforeach()
endif()
include(UsdccPython)
# PySide6 and shiboken6 as tools/build-pyside.ps1 builds them, against vcpkg's
# Qt for OpenUSD's Python (docs/SCRIPTING.md §8). Without them the usdcc.ui
# module isn't built and scripts can't make panels; everything else works.
set(USDCC_PYSIDE_DIR "${CMAKE_SOURCE_DIR}/external/pyside-install"
    CACHE PATH "PySide6/shiboken6 built against vcpkg's Qt (tools/build-pyside.ps1)")
if(EXISTS "${USDCC_PYSIDE_DIR}/PySide6/__init__.py" AND EXISTS "${USDCC_PYSIDE_DIR}/shiboken6/__init__.py")
    set(USDCC_PYSIDE_FOUND TRUE)
    message(STATUS "PySide6 for usdcc: ${USDCC_PYSIDE_DIR}")
else()
    set(USDCC_PYSIDE_FOUND FALSE)
    message(STATUS "PySide6 for usdcc: none in ${USDCC_PYSIDE_DIR} (run tools/build-pyside.ps1); "
                   "usdcc.ui and Python-made panels are disabled")
endif()
include(UsdccRuff)
include(UsdccUv)

# The usdcc Python package lives in build/python/usdcc: the compiled modules
# (usd.pyd, ...) are built there, and this copies the pure-Python files from
# src/python/usdcc next to them. The embedded interpreter puts build/python on
# sys.path (docs/SCRIPTING.md §3.1).
set(USDCC_PYTHON_PACKAGE_ROOT "${CMAKE_BINARY_DIR}/python")
file(GLOB _usdcc_python_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/python/usdcc/*.py")
add_custom_target(
    usdcc_python_package ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${USDCC_PYTHON_PACKAGE_ROOT}/usdcc"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_usdcc_python_sources} "${USDCC_PYTHON_PACKAGE_ROOT}/usdcc"
    # `packaging`, for the plugins' requirements (docs/PLUGINS.md §3.10).
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/src/python/usdcc/_vendor"
            "${USDCC_PYTHON_PACKAGE_ROOT}/usdcc/_vendor"
    SOURCES ${_usdcc_python_sources}
    VERBATIM)

# Qt's `emit`/`signals`/`slots` keyword macros collide with OpenUSD's own TBB
# headers (oneapi/tbb/profiling.h declares `event::emit()`), which every Qt
# translation unit that touches a pxr header ends up including. So our code
# spells them Q_EMIT/Q_SIGNALS/Q_SLOTS. Set here, after the vendored QWindowKit,
# so only usdcc's own targets get it.
add_compile_definitions(QT_NO_KEYWORDS)

# What a C++ plugin has to have been built with (docs/PLUGINS.md §3.2).
if(WIN32)
    set(USDCC_BUILT_FOR_PLATFORM "windows-x64")
elseif(APPLE)
    set(USDCC_BUILT_FOR_PLATFORM "macos-${CMAKE_SYSTEM_PROCESSOR}")
else()
    set(USDCC_BUILT_FOR_PLATFORM "linux-${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(USDCC_BUILT_FOR_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
set(USDCC_BUILT_FOR_QT "${Qt6_VERSION}")
set(USDCC_BUILT_FOR_USD "${PXR_VERSION}")
set(USDCC_BUILT_FOR_USDCC "${PROJECT_VERSION}")
include(UsdccPlugin)

# Precompiled headers for the libraries whose files almost all include OpenUSD
# and Qt: parsing those was ~90% of a file's compile time (cmake/pch/*.h say
# what may go in them). Off checks that every file still includes what it uses.
option(USDCC_USE_PCH "Precompile the OpenUSD/Qt headers of the main libraries and tests" ON)
function(usdcc_precompile_headers target header)
    if(USDCC_USE_PCH)
        target_precompile_headers(${target} PRIVATE "${CMAKE_SOURCE_DIR}/cmake/pch/${header}")
    endif()
endfunction()

add_subdirectory(src/cpp/core)
add_subdirectory(src/cpp/tools)
add_subdirectory(src/cpp/usd)
add_subdirectory(src/cpp/ui)
add_subdirectory(src/cpp/plugin_sdk)
add_subdirectory(src/cpp/scripting)
add_subdirectory(src/cpp/app)
add_subdirectory(src/plugins/cpp_example)

option(USDCC_BUILD_TESTS "Build the usdcc_tests executable and register it with CTest" ON)
if(USDCC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Set here (rather than left to whoever opens the generated .sln) so it
# survives every CMake reconfigure instead of reverting to CMakePredefinedTargets.
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT usdcc)
