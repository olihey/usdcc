# usdcc.scripting — the embedded Python interpreter (milestone M7,
# docs/SCRIPTING.md). Uses OpenUSD's own Python (USDCC_PYTHON_*,
# cmake/modules/UsdccPython.cmake); without it both libraries still build and
# scripting reports itself unavailable.
#
#   usdcc_scripting      PythonRuntime and the log bridge; Qt-free
#   usdcc_scripting_app  the running app for scripts: the embedded _usdcc_app
#                        module (usdcc.app/.usd/.tools), ScriptRunner (undo)
#                        and the startup scripts
#   usdcc_scripting_ui   the Script Editor side panel: CodeEditor,
#                        PythonHighlighter, ScriptEditorSidePanel
add_library(usdcc_scripting STATIC
    include/usdcc/scripting/python_runtime.h
    src/python_runtime.cpp
)
target_include_directories(usdcc_scripting PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(usdcc_scripting PUBLIC usdcc_core)

add_library(usdcc_scripting_app STATIC
    include/usdcc/scripting/app_module.h
    include/usdcc/scripting/package_environment.h
    include/usdcc/scripting/plugin_manager.h
    include/usdcc/scripting/python_settings.h
    include/usdcc/scripting/script_runner.h
    include/usdcc/scripting/startup_scripts.h
    src/app_module.cpp
    src/package_environment.cpp
    src/plugin_manager.cpp
    src/python_settings.cpp
    src/script_runner.cpp
    src/startup_scripts.cpp
)
target_include_directories(usdcc_scripting_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(usdcc_scripting_app PUBLIC usdcc_scripting usdcc_usd_ui usdcc_tools Qt6::Core PRIVATE usdcc_plugin_sdk)
# What a C++ plugin has to match (cmake/modules/UsdccPlugin.cmake).
target_compile_definitions(
    usdcc_scripting_app
    PRIVATE USDCC_BUILT_FOR_PLATFORM="${USDCC_BUILT_FOR_PLATFORM}"
            USDCC_BUILT_FOR_COMPILER="${USDCC_BUILT_FOR_COMPILER}"
            USDCC_BUILT_FOR_QT="${USDCC_BUILT_FOR_QT}"
            USDCC_BUILT_FOR_USD="${USDCC_BUILT_FOR_USD}"
            USDCC_BUILT_FOR_USDCC="${USDCC_BUILT_FOR_USDCC}")
# The build's pinned uv (UsdccUv.cmake), used unless the app finds one bundled
# next to it (tools/deploy.ps1).
if(USDCC_UV_EXE)
    target_compile_definitions(usdcc_scripting_app PRIVATE USDCC_UV_EXE="${USDCC_UV_EXE}")
endif()

add_library(usdcc_scripting_ui STATIC
    include/usdcc/scripting/code_editor.h
    include/usdcc/scripting/package_install_dialog.h
    include/usdcc/scripting/plugin_manager_side_panel.h
    include/usdcc/scripting/python_highlighter.h
    include/usdcc/scripting/script_editor_side_panel.h
    src/code_editor.cpp
    src/package_install_dialog.cpp
    src/plugin_manager_side_panel.cpp
    src/python_highlighter.cpp
    src/script_editor_side_panel.cpp
)
target_include_directories(usdcc_scripting_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(usdcc_scripting_ui PUBLIC usdcc_scripting_app usdcc_ui Qt6::Widgets)
# The build's pinned ruff (UsdccRuff.cmake), used unless the app finds one
# bundled next to it (tools/deploy.ps1).
if(USDCC_RUFF_EXE)
    target_compile_definitions(usdcc_scripting_ui PRIVATE USDCC_RUFF_EXE="${USDCC_RUFF_EXE}")
endif()

if(USDCC_PYTHON_FOUND)
    find_package(pybind11 REQUIRED)
    target_include_directories(usdcc_scripting PRIVATE "${USDCC_PYTHON_INCLUDE_DIR}")
    # pybind11::headers only: pybind11::embed would link the Python
    # find_package(pybind11) found, which is vcpkg's (see UsdccPython.cmake).
    target_link_libraries(usdcc_scripting PRIVATE pybind11::headers PUBLIC "${USDCC_PYTHON_LIBRARY}")
    # Build-time defaults for PythonSettings::defaults(), used unless the app
    # finds a Python bundled next to it (tools/deploy.ps1).
    target_compile_definitions(
        usdcc_scripting
        PRIVATE USDCC_HAS_PYTHON=1
                USDCC_PYTHON_HOME="${USDCC_PYTHON_HOME}"
                USDCC_PYTHON_PACKAGE_ROOT="${USDCC_PYTHON_PACKAGE_ROOT}"
                USDCC_USD_PYTHON_DIR="${USD_INSTALL}/lib/site-packages")
    if(USDCC_PYSIDE_FOUND)
        target_compile_definitions(usdcc_scripting PRIVATE USDCC_PYSIDE_DIR="${USDCC_PYSIDE_DIR}")
    endif()
    # The usdcc package (usdcc/log.py, ...) must be in build/python before
    # anything embeds the interpreter.
    add_dependencies(usdcc_scripting usdcc_python_package)

    target_link_libraries(usdcc_scripting_app PRIVATE usdcc_usd_bindings)
    # Python view panels and tools; they need pybind11, so only with Python.
    target_sources(usdcc_scripting_app PRIVATE
        include/usdcc/scripting/scripted_tool.h
        include/usdcc/scripting/scripted_view_panel.h
        src/python_object.h
        src/scripted_tool.cpp
        src/scripted_view_panel.cpp)
    target_compile_definitions(usdcc_scripting_app PRIVATE USDCC_HAS_PYTHON=1)
else()
    target_compile_definitions(usdcc_scripting PRIVATE USDCC_HAS_PYTHON=0)
    target_compile_definitions(usdcc_scripting_app PRIVATE USDCC_HAS_PYTHON=0)
endif()
