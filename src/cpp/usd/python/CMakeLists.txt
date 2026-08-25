# usdcc.usd Python bindings (pybind11) — milestone M2.
#
# Deliberately does NOT use the top-level pybind11_add_module()/pybind11
# target: those are tied to whichever Python find_package(pybind11) resolved
# at the top level, which is vcpkg's bundled python3 (see
# src/cpp/ui/python/CMakeLists.txt for why that happens and why it's wrong
# here too) — this module must load into the SAME Python that OpenUSD's own
# pxr_boost::python bindings were built against, i.e. whatever system
# interpreter USD_INSTALL was built with. Mirrors usdcc.ui's independent,
# PATH-only interpreter lookup for the same reason.
find_program(
    USDCC_USD_BINDINGS_PYTHON
    NAMES python3 python
    PATHS ENV PATH
    NO_DEFAULT_PATH)

if(NOT USDCC_USD_BINDINGS_PYTHON)
    message(STATUS "usdcc.usd Python bindings: no python interpreter found on PATH; skipping.")
    return()
endif()

execute_process(
    COMMAND
        ${USDCC_USD_BINDINGS_PYTHON} -c
        "import sysconfig
print(sysconfig.get_path('include'))
print(sysconfig.get_config_var('LIBDIR') or '')
print(sysconfig.get_config_var('LDLIBRARY') or sysconfig.get_config_var('LIBRARY') or '')
print(sysconfig.get_config_var('VERSION') or '')"
    OUTPUT_VARIABLE _usdcc_usd_python_probe
    RESULT_VARIABLE _usdcc_usd_python_probe_result
    ERROR_QUIET)

if(NOT _usdcc_usd_python_probe_result EQUAL 0)
    message(STATUS "usdcc.usd Python bindings: could not query ${USDCC_USD_BINDINGS_PYTHON}; skipping.")
    return()
endif()

string(STRIP "${_usdcc_usd_python_probe}" _usdcc_usd_python_probe)
string(REPLACE "\n" ";" _usdcc_usd_python_probe "${_usdcc_usd_python_probe}")
list(GET _usdcc_usd_python_probe 0 _usdcc_usd_python_include_dir)
list(GET _usdcc_usd_python_probe 3 _usdcc_usd_python_version)

# sysconfig doesn't report the .lib import library location on Windows
# (LIBDIR/LDLIBRARY are POSIX-oriented); it lives at <prefix>/libs/pythonXY.lib.
get_filename_component(_usdcc_usd_python_prefix "${_usdcc_usd_python_include_dir}" DIRECTORY)
string(REPLACE "." "" _usdcc_usd_python_version_nodot "${_usdcc_usd_python_version}")
set(_usdcc_usd_python_library
    "${_usdcc_usd_python_prefix}/libs/python${_usdcc_usd_python_version_nodot}.lib")

if(NOT EXISTS "${_usdcc_usd_python_library}")
    message(STATUS "usdcc.usd Python bindings: expected import library not found at "
                    "${_usdcc_usd_python_library}; skipping.")
    return()
endif()

find_package(pybind11 REQUIRED)

set(_python_package_dir "${CMAKE_BINARY_DIR}/python/usdcc")

add_library(usdcc_usd_python MODULE bindings.cpp)
target_include_directories(usdcc_usd_python PRIVATE "${_usdcc_usd_python_include_dir}")
target_link_libraries(usdcc_usd_python PRIVATE usdcc_usd pybind11::headers "${_usdcc_usd_python_library}")
# vcpkg's applocal-deps step (VCPKG_APPLOCAL_DEPS, on by default in this
# toolchain-based integration with no documented per-target override) copies
# its own bundled python312.dll next to usd.pyd, even though this target
# links the system Python instead. Harmless: Windows resolves an
# already-loaded DLL by name first, and the system interpreter that imports
# this module has already loaded its own python312.dll by the time it does
# so — but worth knowing about if usd.pyd is ever loaded some other way.
set_target_properties(
    usdcc_usd_python
    PROPERTIES
        OUTPUT_NAME "usd"
        PREFIX ""
        SUFFIX ".pyd"
        LIBRARY_OUTPUT_DIRECTORY "${_python_package_dir}"
        RUNTIME_OUTPUT_DIRECTORY "${_python_package_dir}")
foreach(_config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${_config}" _config_upper)
    set_target_properties(
        usdcc_usd_python
        PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${_config_upper} "${_python_package_dir}"
                   RUNTIME_OUTPUT_DIRECTORY_${_config_upper} "${_python_package_dir}")
endforeach()

# Shared across every usdcc.* submodule — see that file for why.
add_custom_command(
    TARGET usdcc_usd_python
    POST_BUILD
    COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/python/usdcc/__init__.py" "${_python_package_dir}/__init__.py")
