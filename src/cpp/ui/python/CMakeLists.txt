# usdcc.ui Python bindings (Shiboken6) — milestone M1, used by M7's
# Python-made side panels (docs/SCRIPTING.md §8).
#
# PySide6/Shiboken6's own Qt binaries are a *different compiled build* of Qt
# than vcpkg's (different vendor, different build flags) even at the same
# version number. Loading both in one process makes Qt/QObject-derived
# symbols resolve against whichever copy happened to load first, which
# produces "DLL load failed: the specified procedure could not be found" at
# random points — hit and confirmed while building this. A pip-installed
# PySide6 can't be used for that reason.
#
# So PySide6/Shiboken6 are built from source against usdcc's own vcpkg Qt,
# for OpenUSD's Python (the one the app embeds): tools/build-pyside.ps1,
# which leaves them in USDCC_PYSIDE_DIR (see the top-level CMakeLists.txt).
# Without that build this module is skipped.
if(NOT USDCC_PYSIDE_FOUND OR NOT USDCC_PYTHON_FOUND)
    message(STATUS "usdcc.ui Python bindings: skipped (needs OpenUSD's Python and tools/build-pyside.ps1)")
    return()
endif()

set(_shiboken6_generator_dir "${USDCC_PYSIDE_DIR}/shiboken6_generator")
set(_shiboken6_dir "${USDCC_PYSIDE_DIR}/shiboken6")
set(_pyside6_dir "${USDCC_PYSIDE_DIR}/PySide6")
list(APPEND CMAKE_PREFIX_PATH "${_shiboken6_generator_dir}/lib/cmake/Shiboken6Tools")
list(APPEND CMAKE_PREFIX_PATH "${_shiboken6_dir}/lib/cmake/Shiboken6")
list(APPEND CMAKE_PREFIX_PATH "${_pyside6_dir}/lib/cmake/PySide6")
string(REPLACE "." "" _usdcc_ui_python_version_nodot "${USDCC_PYTHON_VERSION}")

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

# Bindings only cover SidePanel today (see typesystem.xml) — ViewPanel is
# deliberately excluded (see bindings.h) — so the include list only needs to
# satisfy that one header. Extend this if bindings.h grows to include headers
# from other usdcc_* libraries.
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
    "${USDCC_PYTHON_INCLUDE_DIR}"
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

# Filenames are shiboken6's own naming convention for a `package="usdcc._ui"`
# typesystem with a `usdcc::ui::SidePanel` object-type: verified empirically
# by running shiboken6 once and inspecting its output, not documented
# anywhere authoritative. Re-run by hand if typesystem.xml grows new classes
# and the build starts complaining about missing generated files.
set(_generated_sources
    "${_gen_dir}/usdcc/_ui/usdcc_wrapper.cpp"
    "${_gen_dir}/usdcc/_ui/usdcc_ui_wrapper.cpp"
    "${_gen_dir}/usdcc/_ui/usdcc_ui_sidepanel_wrapper.cpp"
    "${_gen_dir}/usdcc/_ui/_ui_module_wrapper.cpp")

# libclang's own headers, which shiboken6 otherwise can't find (it warns that
# parse errors may follow); tools/build-pyside.ps1 leaves them here.
set(_shiboken_env "")
if(EXISTS "${CMAKE_SOURCE_DIR}/external/libclang/lib/clang")
    set(_shiboken_env ${CMAKE_COMMAND} -E env "LLVM_INSTALL_DIR=${CMAKE_SOURCE_DIR}/external/libclang")
endif()

add_custom_command(
    OUTPUT ${_generated_sources}
    COMMAND
        ${_shiboken_env} $<TARGET_FILE:Shiboken6::shiboken6> --generator-set=shiboken --enable-parent-ctor-heuristic
        --enable-return-value-heuristic --avoid-protected-hack -std=c++17
        --compiler=${_shiboken_compiler} ${_shiboken_include_flags} -T${PYSIDE_TYPESYSTEMS}
        --output-directory=${_gen_dir} ${_bindings_header} ${_typesystem_file}
    DEPENDS
        ${_bindings_header} ${_typesystem_file}
        "${CMAKE_SOURCE_DIR}/src/cpp/ui/include/usdcc/ui/side_panel.h"
    COMMENT "Generating usdcc.ui Python bindings (Shiboken6)"
    VERBATIM)

# Output layout: build/python/usdcc/_ui.pyd, behind src/python/usdcc/ui.py
# (which sets up the DLL folders it needs first), so `import usdcc.ui` works
# with build/python on PYTHONPATH.
set(_python_package_dir "${CMAKE_BINARY_DIR}/python/usdcc")

add_library(usdcc_ui_python MODULE ${_generated_sources})
target_include_directories(usdcc_ui_python PRIVATE ${_shiboken_include_dirs} "${_gen_dir}/usdcc/_ui")
# Shiboken's generated wrappers use the standard "#define protected public"
# trick to reach protected members of the wrapped classes. MSVC's STL treats
# redefining access-specifier keywords as a hard error unless this is set.
target_compile_definitions(usdcc_ui_python PRIVATE _ALLOW_KEYWORD_MACROS)
target_link_libraries(usdcc_ui_python PRIVATE usdcc_ui Shiboken6::libshiboken PySide6::pyside6)
usdcc_deploy_core_library(usdcc_ui_python)
# Debug-config-only fixup: the generated wrapper .cpp files include
# shiboken6's sbkpython.h, which #includes <Python.h> with no protection
# against MSVC's pyconfig.h auto-embedding
# `#pragma comment(lib,"python312_d.lib")` whenever `_DEBUG` is defined (i.e.
# every Debug-config translation unit) — unlike pybind11's own Python.h
# wrapper (see usdcc_usd_python), which explicitly undefs _DEBUG around its
# include specifically to avoid this. A pip-installed PySide6/shiboken6 only
# ships release-mode Python ABI stub libraries (pyside6.abi3.lib/
# shiboken6.abi3.lib, already linked above, which resolve dynamically against
# whatever python3*.dll is loaded at runtime and don't themselves need a
# debug-suffixed import lib), so linking still fails looking for a
# python3xy_d.lib file that plain `pip install`/python.org installs don't
# provide (building CPython itself in debug mode is a separate, rarely-taken
# step). /NODEFAULTLIB suppresses that specific embedded request; it doesn't
# remove any of the explicitly-linked libraries above, which is all this
# target's Stable-ABI-only code actually needs.
if(MSVC AND _usdcc_ui_python_version_nodot)
    target_link_options(usdcc_ui_python PRIVATE
        $<$<CONFIG:Debug>:/NODEFAULTLIB:python${_usdcc_ui_python_version_nodot}_d.lib>)
endif()
# usdcc_ui now transitively links usdcc_usd (see src/cpp/ui/CMakeLists.txt's
# comment on ViewPanel's mandatory Stage association), which pulls in USD's
# own headers — those embed a plain `#pragma comment(lib,"python312.lib")`
# regardless of build config (see src/cpp/app/CMakeLists.txt's comment on the
# exact same issue for the usdcc executable). Same fix here.
target_link_libraries(usdcc_ui_python PRIVATE "${USDCC_PYTHON_LIBRARY}")
set_target_properties(
    usdcc_ui_python
    PROPERTIES
        OUTPUT_NAME "_ui"
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
