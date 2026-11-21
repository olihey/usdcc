# usdcc_node — usdcc on a machine with nobody at it (docs/RENDER_NODE.md): one render from
# the command line, or a render node for peers' rooms. A console program beside usdcc.exe,
# with its DLLs, plugins and Python.
#
#   usdcc_node_core  what its commands decide, without a room or a window: the options, the
#                    render arguments and their overrides, the trust list, the key file (the tests')
#   usdcc_node       the program
add_library(usdcc_node_core STATIC node.h options.cpp render_options.cpp render_options.h serve_support.cpp serve_support.h)
target_include_directories(usdcc_node_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(usdcc_node_core PUBLIC usdcc_render usdcc_peers Qt6::Core)
if(Python3_LIBRARY AND EXISTS "${Python3_LIBRARY}")
    target_link_libraries(usdcc_node_core PUBLIC "${Python3_LIBRARY}")
endif()

add_executable(usdcc_node main.cpp node.h render_command.cpp serve_command.cpp)
target_link_libraries(usdcc_node PRIVATE usdcc_node_core usdcc_app_core usdcc_scripting usdcc_scripting_app Qt6::Gui)
# Beside usdcc.exe, so it finds the same DLLs, Qt plugins, OpenUSD plugins and Python.
set_target_properties(usdcc_node PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:usdcc>")
add_dependencies(usdcc_node usdcc)
if(WIN32)
    # Visual Studio's debugger, as for the app (src/cpp/app/CMakeLists.txt).
    set_target_properties(
        usdcc_node
        PROPERTIES
            VS_DEBUGGER_ENVIRONMENT
            "PATH=${USD_INSTALL}/bin;${USD_INSTALL}/lib;%PATH%\nPXR_PLUGINPATH_NAME=${USD_INSTALL}/plugin/usd")
endif()
