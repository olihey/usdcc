# usdcc.usd Python bindings (pybind11) — milestone M2.
#
# Deliberately does NOT use the top-level pybind11_add_module()/pybind11
# target: those are tied to whichever Python find_package(pybind11) resolved
# at the top level, which is vcpkg's bundled python3 — this module must load
# into the SAME Python that OpenUSD's own pxr_boost::python bindings were
# built against: USDCC_PYTHON_* (cmake/modules/UsdccPython.cmake). An earlier
# version found "the first python on PATH" instead, and so could end up built
# for a different Python than pxr (docs/SCRIPTING.md §1).
if(NOT USDCC_PYTHON_FOUND)
    message(STATUS "usdcc.usd Python bindings: OpenUSD's Python not found; skipping.")
    return()
endif()
set(_usdcc_usd_python_include_dir "${USDCC_PYTHON_INCLUDE_DIR}")
set(_usdcc_usd_python_library "${USDCC_PYTHON_LIBRARY}")

find_package(pybind11 REQUIRED)

set(_python_package_dir "${CMAKE_BINARY_DIR}/python/usdcc")

add_library(usdcc_usd_python MODULE bindings.cpp)
target_include_directories(usdcc_usd_python PRIVATE "${_usdcc_usd_python_include_dir}")
target_link_libraries(usdcc_usd_python PRIVATE usdcc_usd pybind11::headers "${_usdcc_usd_python_library}")
usdcc_deploy_core_library(usdcc_usd_python)
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

# The package's pure-Python files (__init__.py, ...) — see the top-level
# CMakeLists.txt.
add_dependencies(usdcc_usd_python usdcc_python_package)
