# Three test executables on a tiny in-repo harness (test_harness.h/.cpp) rather
# than GoogleTest, so they need no extra dependency. Run them through CTest
# (`ctest -C RelWithDebInfo`) or directly, optionally with a name filter as the
# first argument.
#   usdcc_tests    — headless tests for the non-UI logic (tools, undo, stage
#                    change notices, value-type classification, ...).
#   usdcc_ui_tests — widget tests (UI scale, log panel, collapsing side
#                    panels), which need a full QApplication;
#                    CTest runs them on Qt's offscreen platform.
add_executable(usdcc_tests
    test_harness.h
    test_harness.cpp
    test_main.cpp
    applied_schemas_test.cpp
    categorizer_spike_test.cpp
    composition_arcs_test.cpp
    create_prim_test.cpp
    dependencies_test.cpp
    dependency_files.h
    layer_stack_test.cpp
    log_bridges_test.cpp
    log_capture_test.cpp
    log_test.cpp
    move_tool_test.cpp
    prim_move_test.cpp
    property_categorizer_test.cpp
    relationship_targets_test.cpp
    rotate_tool_test.cpp
    settings_test.cpp
    stage_changes_test.cpp
    stage_names_test.cpp
    undo_manager_test.cpp
    usda_syntax_test.cpp
    value_type_info_test.cpp
    variant_actions_test.cpp
    view_panel_registry_test.cpp
    xform_math_test.cpp
    xform_stack_test.cpp
    xform_target_test.cpp
)
target_link_libraries(usdcc_tests PRIVATE usdcc_tools usdcc_usd usdcc_usd_ui Qt6::Core)
usdcc_precompile_headers(usdcc_tests ui_pch.h)

add_executable(usdcc_ui_tests
    test_harness.h
    test_settings.h
    test_harness.cpp
    ui_test_main.cpp
    attributes_primvar_groups_test.cpp
    attributes_rename_test.cpp
    composition_edit_test.cpp
    create_menu_test.cpp
    current_stage_test.cpp
    dependencies_panel_test.cpp
    dependency_files.h
    layer_editor_test.cpp
    log_panel_test.cpp
    outliner_icons_test.cpp
    outliner_context_menu_test.cpp
    outliner_drag_test.cpp
    outliner_names_test.cpp
    prim_tree_dialog_test.cpp
    property_row_test.cpp
    relationship_editor_test.cpp
    scale_controller_test.cpp
    schema_pills_test.cpp
    settings_ui_test.cpp
    side_panel_test.cpp
    toast_test.cpp
    usda_view_test.cpp
    variant_editor_test.cpp
    view_panel_layout_test.cpp
)
target_link_libraries(usdcc_ui_tests PRIVATE usdcc_usd_ui Qt6::Test)
usdcc_precompile_headers(usdcc_ui_tests ui_pch.h)

#   usdcc_python_tests — the embedded Python (docs/SCRIPTING.md). Its own
#                    executable because Python can only be started once per
#                    process; python_test_main.cpp starts it for every test.
add_executable(usdcc_python_tests
    test_harness.h
    test_harness.cpp
    python_test_main.cpp
    plugin_manager_test.cpp
    plugin_native_test.cpp
    python_app_test.cpp
    python_runtime_test.cpp
    script_editor_panel_test.cpp
    startup_scripts_test.cpp
    python_test_app.h
)
target_link_libraries(usdcc_python_tests PRIVATE usdcc_scripting usdcc_scripting_app usdcc_scripting_ui)

# A C++ plugin for plugin_native_test.cpp, with a DLL of its own beside it.
set(_usdcc_test_plugins "${CMAKE_CURRENT_BINARY_DIR}/plugins")
usdcc_add_plugin(usdcc_test_plugin
    ID test.native
    NAME "Native Test"
    VERSION 1.0.0
    SOURCES test_plugin/test_plugin.cpp
    OUTPUT_DIR "${_usdcc_test_plugins}/test_native")
add_library(usdcc_test_plugin_helper SHARED test_plugin/helper.cpp)
set_target_properties(
    usdcc_test_plugin_helper
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${_usdcc_test_plugins}/test_native/bin/${USDCC_BUILT_FOR_PLATFORM}>"
               LIBRARY_OUTPUT_DIRECTORY "$<1:${_usdcc_test_plugins}/test_native/bin/${USDCC_BUILT_FOR_PLATFORM}>")
target_link_libraries(usdcc_test_plugin PRIVATE usdcc_test_plugin_helper)
add_dependencies(usdcc_python_tests usdcc_test_plugin)
target_compile_definitions(usdcc_python_tests PRIVATE USDCC_TEST_PLUGINS="${_usdcc_test_plugins}")
target_link_libraries(usdcc_python_tests PRIVATE usdcc_plugin_sdk)
# Python-made panels need the usdcc.ui module, which needs PySide6.
if(TARGET usdcc_ui_python)
    target_sources(usdcc_python_tests PRIVATE plugin_extensions_test.cpp python_panels_test.cpp)
    target_compile_definitions(usdcc_python_tests PRIVATE USDCC_EXAMPLE_PLUGINS="${CMAKE_SOURCE_DIR}/src/plugins")
    add_dependencies(usdcc_python_tests usdcc_ui_python)
endif()

foreach(_usdcc_test usdcc_tests usdcc_ui_tests usdcc_python_tests)
    # Same reason as the app (see src/cpp/app/CMakeLists.txt): USD's headers ask
    # for the release Python import library in every configuration.
    if(Python3_LIBRARY AND EXISTS "${Python3_LIBRARY}")
        target_link_libraries(${_usdcc_test} PRIVATE "${Python3_LIBRARY}")
    endif()
    usdcc_deploy_core_library(${_usdcc_test} usdcc_usd usdcc_tools usdcc_ui usdcc_usd_ui)
    # OpenUSD's Python DLL rather than vcpkg's (see UsdccPython.cmake).
    usdcc_deploy_python(${_usdcc_test})
    add_test(NAME ${_usdcc_test} COMMAND ${_usdcc_test})
endforeach()

# No window ever appears; the widgets still lay out and paint.
set_tests_properties(usdcc_ui_tests usdcc_python_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(WIN32)
    # USD's DLLs and plugins aren't next to the executables; Qt's are (vcpkg's
    # applocal step), except for Debug, which links the release Qt (see the
    # top-level CMakeLists.txt) — so its bin dir goes on PATH too, along with
    # its platform plugins for the offscreen UI tests.
    set(_usdcc_test_env
        "PATH=path_list_prepend:${USD_INSTALL}/bin"
        "PATH=path_list_prepend:${USD_INSTALL}/lib"
        "PXR_PLUGINPATH_NAME=set:${USD_INSTALL}/plugin/usd")
    set(_usdcc_ui_test_env ${_usdcc_test_env})
    if(_VCPKG_INSTALLED_DIR)
        set(_usdcc_vcpkg_dir "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
        list(APPEND _usdcc_test_env "PATH=path_list_prepend:${_usdcc_vcpkg_dir}/bin")
        list(APPEND _usdcc_ui_test_env "PATH=path_list_prepend:${_usdcc_vcpkg_dir}/bin"
             "QT_QPA_PLATFORM_PLUGIN_PATH=set:${_usdcc_vcpkg_dir}/Qt6/plugins/platforms")
    endif()
    set_tests_properties(usdcc_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_usdcc_test_env}")
    set_tests_properties(usdcc_ui_tests usdcc_python_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_usdcc_ui_test_env}")
    set_target_properties(
        usdcc_tests usdcc_ui_tests usdcc_python_tests
        PROPERTIES
            VS_DEBUGGER_ENVIRONMENT
            "PATH=${USD_INSTALL}/bin;${USD_INSTALL}/lib;%PATH%\nPXR_PLUGINPATH_NAME=${USD_INSTALL}/plugin/usd")
endif()
