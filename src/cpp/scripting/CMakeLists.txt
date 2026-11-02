# usdcc.scripting — the embedded Python interpreter (milestone M7,
# docs/SCRIPTING.md). Qt-free. Uses OpenUSD's own Python (USDCC_PYTHON_*,
# cmake/modules/UsdccPython.cmake); without it the library still builds and
# PythonRuntime reports scripting as unavailable.
add_library(usdcc_scripting STATIC
    include/usdcc/scripting/python_runtime.h
    src/python_runtime.cpp
)
target_include_directories(usdcc_scripting PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(usdcc_scripting PUBLIC usdcc_core)

if(USDCC_PYTHON_FOUND)
    find_package(pybind11 REQUIRED)
    target_include_directories(usdcc_scripting PRIVATE "${USDCC_PYTHON_INCLUDE_DIR}")
    # pybind11::headers only: pybind11::embed would link the Python
    # find_package(pybind11) found, which is vcpkg's (see UsdccPython.cmake).
    target_link_libraries(usdcc_scripting PRIVATE pybind11::headers PUBLIC "${USDCC_PYTHON_LIBRARY}")
    # Build-time defaults for PythonSettings::defaults(); M11 packaging
    # replaces these absolute dev-build paths with ones next to the exe.
    target_compile_definitions(
        usdcc_scripting
        PRIVATE USDCC_HAS_PYTHON=1
                USDCC_PYTHON_HOME="${USDCC_PYTHON_HOME}"
                USDCC_PYTHON_PACKAGE_ROOT="${USDCC_PYTHON_PACKAGE_ROOT}"
                USDCC_USD_PYTHON_DIR="${USD_INSTALL}/lib/site-packages")
    # The usdcc package (usdcc/log.py, ...) must be in build/python before
    # anything embeds the interpreter.
    add_dependencies(usdcc_scripting usdcc_python_package)
else()
    target_compile_definitions(usdcc_scripting PRIVATE USDCC_HAS_PYTHON=0)
endif()
