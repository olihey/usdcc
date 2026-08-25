# usdcc.ui Python bindings (Shiboken6) — milestone M1.
#
# PySide6/Shiboken6's own Qt binaries are a *different compiled build* of Qt
# than vcpkg's (different vendor, different build flags) even at the same
# version number. Loading both in one process makes Qt/QObject-derived
# symbols resolve against whichever copy happened to load first, which
# produces "DLL load failed: the specified procedure could not be found" at
# random points — hit and confirmed while building this. There is no
# reliable way to bind vcpkg-Qt-derived classes (SidePanel, ViewPanel) for
# use from a pip-installed PySide6 as a result.
#
# The fix is to build PySide6/Shiboken6 from source against usdcc's own
# vcpkg Qt, so there is exactly one Qt build in the process. That custom
# build's CMake package config lives under USDCC_PYSIDE_INSTALL_PREFIX
# (see tools/build-pyside.ps1, which produces
# external/pyside-setup/build/<...>/install). Falls back to discovering a
# pip-installed PySide6/shiboken6-generator on PATH if that variable isn't
# set, for anyone who wants to experiment before running the from-source
# build — but expect the ABI mismatch above until they do.
if(USDCC_PYSIDE_INSTALL_PREFIX)
    list(APPEND CMAKE_PREFIX_PATH "${USDCC_PYSIDE_INSTALL_PREFIX}/lib/cmake/Shiboken6Tools")
    list(APPEND CMAKE_PREFIX_PATH "${USDCC_PYSIDE_INSTALL_PREFIX}/lib/cmake/Shiboken6")
    list(APPEND CMAKE_PREFIX_PATH "${USDCC_PYSIDE_INSTALL_PREFIX}/lib/cmake/PySide6")
    set(_pyside6_dir "${USDCC_PYSIDE_INSTALL_PREFIX}/PySide6")
    set(_shiboken6_dir "${USDCC_PYSIDE_INSTALL_PREFIX}/shiboken6")
else()
    # Deliberately does NOT reuse the top-level Python_EXECUTABLE: vcpkg's
    # toolchain redirects find_package(Python) to its own bundled python3
    # tool (for pybind11's build-time codegen), which won't have
    # PySide6/shiboken6 installed. Bindings need whatever interpreter the
    # developer actually installed PySide6 into, hence the independent,
    # PATH-only lookup below.
    find_program(
        USDCC_BINDINGS_PYTHON
        NAMES python3 python
        PATHS ENV PATH
        NO_DEFAULT_PATH)

    if(NOT USDCC_BINDINGS_PYTHON)
        message(STATUS "usdcc.ui Python bindings: no python interpreter found on PATH; skipping.")
        return()
    endif()

    execute_process(
        COMMAND
            ${USDCC_BINDINGS_PYTHON} -c
            "import shiboken6_generator, shiboken6, PySide6, os, sys
print(os.path.dirname(shiboken6_generator.__file__))
print(os.path.dirname(shiboken6.__file__))
print(os.path.dirname(PySide6.__file__))"
        OUTPUT_VARIABLE _usdcc_bindings_probe
        RESULT_VARIABLE _usdcc_bindings_probe_result
        ERROR_QUIET)

    if(NOT _usdcc_bindings_probe_result EQUAL 0)
        message(STATUS "usdcc.ui Python bindings: PySide6 / shiboken6-generator not importable by "
                        "${USDCC_BINDINGS_PYTHON}; skipping (pip install pyside6 shiboken6-generator, "
                        "or set USDCC_PYSIDE_INSTALL_PREFIX to a from-source build, to enable).")
        return()
    endif()

    string(STRIP "${_usdcc_bindings_probe}" _usdcc_bindings_probe)
    string(REPLACE "\n" ";" _usdcc_bindings_probe "${_usdcc_bindings_probe}")
    list(GET _usdcc_bindings_probe 0 _shiboken6_generator_dir)
    list(GET _usdcc_bindings_probe 1 _shiboken6_dir)
    list(GET _usdcc_bindings_probe 2 _pyside6_dir)

    list(APPEND CMAKE_PREFIX_PATH "${_shiboken6_generator_dir}/lib/cmake/Shiboken6Tools")
    list(APPEND CMAKE_PREFIX_PATH "${_shiboken6_dir}/lib/cmake/Shiboken6")
    list(APPEND CMAKE_PREFIX_PATH "${_pyside6_dir}/lib/cmake/PySide6")
endif()

find_package(Shiboken6Tools REQUIRED)
find_package(Shiboken6 REQUIRED)
find_package(PySide6 REQUIRED)

if(MSVC)
    set(_shiboken_compiler msvc)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_shiboken_compiler clang)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_shiboken_compiler g++)
else()
    message(STATUS "usdcc.ui Python bindings: unrecognized compiler '${CMAKE_CXX_COMPILER_ID}'; skipping.")
    return()
endif()

# Bindings only cover SidePanel/ViewPanel today (see typesystem.xml), so the
# include list only needs to satisfy those two headers. Extend this if
# bindings.h grows to include headers from other usdcc_* libraries.
get_target_property(_qt_core_includes Qt6::Core INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(_qt_gui_includes Qt6::Gui INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(_qt_widgets_includes Qt6::Widgets INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(_ads_includes ads::qtadvanceddocking-qt6 INTERFACE_INCLUDE_DIRECTORIES)

set(_shiboken_include_dirs
    "${CMAKE_SOURCE_DIR}/src/cpp/ui/include"
    ${_qt_core_includes}
    ${_qt_gui_includes}
    ${_qt_widgets_includes}
    ${_ads_includes}
    ${Python_INCLUDE_DIRS}
    "${_pyside6_dir}/include"
    "${_pyside6_dir}/include/QtCore"
    "${_pyside6_dir}/include/QtGui"
    "${_pyside6_dir}/include/QtWidgets"
    "${_shiboken6_dir}/include")
list(REMOVE_DUPLICATES _shiboken_include_dirs)

set(_shiboken_include_flags ${_shiboken_include_dirs})
list(TRANSFORM _shiboken_include_flags PREPEND "-I")

set(_bindings_header "${CMAKE_CURRENT_SOURCE_DIR}/bindings.h")
set(_typesystem_file "${CMAKE_CURRENT_SOURCE_DIR}/typesystem.xml")
set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Filenames are shiboken6's own naming convention for a `package="usdcc.ui"`
# typesystem with `usdcc::ui::{SidePanel,ViewPanel}` object-types: verified
# empirically by running shiboken6 once and inspecting its output, not
# documented anywhere authoritative. Re-run by hand if typesystem.xml grows
# new classes and the build starts complaining about missing generated files.
set(_generated_sources
    "${_gen_dir}/usdcc/ui/usdcc_wrapper.cpp"
    "${_gen_dir}/usdcc/ui/usdcc_ui_wrapper.cpp"
    "${_gen_dir}/usdcc/ui/usdcc_ui_sidepanel_wrapper.cpp"
    "${_gen_dir}/usdcc/ui/usdcc_ui_viewpanel_wrapper.cpp"
    "${_gen_dir}/usdcc/ui/ui_module_wrapper.cpp")

add_custom_command(
    OUTPUT ${_generated_sources}
    COMMAND
        Shiboken6::shiboken6 --generator-set=shiboken --enable-parent-ctor-heuristic
        --enable-return-value-heuristic --avoid-protected-hack -std=c++17
        --compiler=${_shiboken_compiler} ${_shiboken_include_flags} -T${PYSIDE_TYPESYSTEMS}
        --output-directory=${_gen_dir} ${_bindings_header} ${_typesystem_file}
    DEPENDS
        ${_bindings_header} ${_typesystem_file}
        "${CMAKE_SOURCE_DIR}/src/cpp/ui/include/usdcc/ui/side_panel.h"
        "${CMAKE_SOURCE_DIR}/src/cpp/ui/include/usdcc/ui/view_panel.h"
    COMMENT "Generating usdcc.ui Python bindings (Shiboken6)"
    VERBATIM)

# Output layout: build/python/usdcc/ui.pyd (+ a hand-rolled __init__.py) so
# `import usdcc.ui` works with build/python on PYTHONPATH.
set(_python_package_dir "${CMAKE_BINARY_DIR}/python/usdcc")

add_library(usdcc_ui_python MODULE ${_generated_sources})
target_include_directories(usdcc_ui_python PRIVATE ${_shiboken_include_dirs} "${_gen_dir}/usdcc/ui")
# Shiboken's generated wrappers use the standard "#define protected public"
# trick to reach protected members of the wrapped classes. MSVC's STL treats
# redefining access-specifier keywords as a hard error unless this is set.
target_compile_definitions(usdcc_ui_python PRIVATE _ALLOW_KEYWORD_MACROS)
target_link_libraries(usdcc_ui_python PRIVATE usdcc_ui Shiboken6::libshiboken PySide6::pyside6)
set_target_properties(
    usdcc_ui_python
    PROPERTIES
        OUTPUT_NAME "ui"
        PREFIX ""
        SUFFIX ".pyd"
        LIBRARY_OUTPUT_DIRECTORY "${_python_package_dir}"
        RUNTIME_OUTPUT_DIRECTORY "${_python_package_dir}")
foreach(_config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${_config}" _config_upper)
    set_target_properties(
        usdcc_ui_python
        PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${_config_upper} "${_python_package_dir}"
                   RUNTIME_OUTPUT_DIRECTORY_${_config_upper} "${_python_package_dir}")
endforeach()

# Shared across every usdcc.* submodule — see that file for why.
add_custom_command(
    TARGET usdcc_ui_python
    POST_BUILD
    COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/python/usdcc/__init__.py" "${_python_package_dir}/__init__.py")
