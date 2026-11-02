# usdcc.usd Python bindings (pybind11) — milestone M2, reshaped in M7.
#
#   usdcc_usd_bindings  the binding code (bind_usd.h), shared by the two below
#   usdcc_usd_python    usdcc/_usd.pyd, usdcc.usd's native part in a plain python
#   (the app's own embedded module, _usdcc_app, is in src/cpp/scripting)
# usdcc/usd.py (src/python) picks whichever of the two is there
# (docs/SCRIPTING.md §4.1).
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

find_package(pybind11 REQUIRED)

add_library(usdcc_usd_bindings STATIC include/usdcc/usd/python/bind_usd.h bind_usd.cpp)
target_include_directories(usdcc_usd_bindings PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${USDCC_PYTHON_INCLUDE_DIR}")
target_link_libraries(usdcc_usd_bindings PUBLIC usdcc_usd pybind11::headers)

set(_python_package_dir "${CMAKE_BINARY_DIR}/python/usdcc")
# Before M7 this module was usdcc/usd.pyd; a stale copy would shadow usd.py.
file(REMOVE "${_python_package_dir}/usd.pyd" "${_python_package_dir}/usd.pdb")

add_library(usdcc_usd_python MODULE bindings.cpp)
target_link_libraries(usdcc_usd_python PRIVATE usdcc_usd_bindings "${USDCC_PYTHON_LIBRARY}")
usdcc_deploy_core_library(usdcc_usd_python)
# vcpkg's applocal-deps step (VCPKG_APPLOCAL_DEPS, on by default in this
# toolchain-based integration with no documented per-target override) copies
# its own bundled python312.dll next to _usd.pyd, even though this target
# links OpenUSD's Python instead. Harmless: Windows resolves an
# already-loaded DLL by name first, and the interpreter that imports this
# module has already loaded its own python312.dll by the time it does so.
set_target_properties(
    usdcc_usd_python
    PROPERTIES
        OUTPUT_NAME "_usd"
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

# The package's pure-Python files (__init__.py, usd.py, ...) — see the
# top-level CMakeLists.txt.
add_dependencies(usdcc_usd_python usdcc_python_package)
