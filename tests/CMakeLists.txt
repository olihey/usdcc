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
    prim_selection_test.cpp
    categorizer_spike_test.cpp
    composition_arcs_test.cpp
    create_prim_test.cpp
    create_stage_test.cpp
    dependencies_test.cpp
    dependency_files.h
    layer_stack_test.cpp
    shader_node_catalog_test.cpp
    shade_graph_test.cpp
    shade_actions_test.cpp
    shade_imaging_test.cpp
    shading_preview_test.cpp
    log_bridges_test.cpp
    log_capture_test.cpp
    log_test.cpp
    move_tool_test.cpp
    orbit_camera_test.cpp
    physics_spike_test.cpp
    physics_test.cpp
    physics_bake_joints_test.cpp
    viewport_grid_test.cpp
    prim_move_test.cpp
    prim_delete_test.cpp
    property_categorizer_test.cpp
    relationship_targets_test.cpp
    rotate_tool_test.cpp
    scene_bounds_test.cpp
    scene_counts_test.cpp
    settings_test.cpp
    usd_runtime_info_test.cpp
    usd_file_formats_test.cpp
    color_management_test.cpp
    shade_connections_test.cpp
    stage_changes_test.cpp
    stage_time_test.cpp
    time_imaging_test.cpp
    stage_names_test.cpp
    undo_manager_test.cpp
    usda_syntax_test.cpp
    value_type_info_test.cpp
    variant_actions_test.cpp
    version_test.cpp
    view_panel_registry_test.cpp
    xform_math_test.cpp
    xform_stack_test.cpp
    xform_target_test.cpp
)
target_link_libraries(usdcc_tests PRIVATE usdcc_tools usdcc_usd usdcc_usd_ui Qt6::Core)
# docs/PHYSICS.md: the phase 0 spike drives Jolt directly; the physics tests use the
# plugin's backend library (Jolt's DLL is copied beside them at the end of this file).
target_link_libraries(usdcc_tests PRIVATE Jolt::Jolt usdcc_physics_jolt_backend)
target_compile_definitions(usdcc_tests PRIVATE USDCC_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test_data")
usdcc_precompile_headers(usdcc_tests ui_pch.h)

add_executable(usdcc_ui_tests
    test_harness.h
    test_settings.h
    test_harness.cpp
    ui_test_main.cpp
    about_dialog_test.cpp
    usd_runtime_dialog_test.cpp
    attributes_primvar_groups_test.cpp
    attributes_rename_test.cpp
    attributes_tab_test.cpp
    attributes_right_edge_test.cpp
    attributes_transform_section_test.cpp
    close_stage_test.cpp
    composition_edit_test.cpp
    create_menu_test.cpp
    selection_menu_test.cpp
    current_stage_test.cpp
    dependencies_panel_test.cpp
    dependency_files.h
    layer_editor_test.cpp
    log_panel_test.cpp
    outliner_icons_test.cpp
    outliner_context_menu_test.cpp
    outliner_drag_test.cpp
    outliner_names_test.cpp
    outliner_single_root_test.cpp
    prim_tree_dialog_test.cpp
    property_row_test.cpp
    asset_path_test.cpp
    asset_picker_test.cpp
    prim_tree_filter_test.cpp
    recent_files_test.cpp
    relationship_editor_test.cpp
    render_view_panel_test.cpp
    scale_controller_test.cpp
    schema_pills_test.cpp
    material_editor_test.cpp
    settings_ui_test.cpp
    shader_connections_ui_test.cpp
    add_shade_input_ui_test.cpp
    session_store_test.cpp
    side_panel_test.cpp
    physics_timeline_test.cpp
    timeline_test.cpp
    toast_test.cpp
    usda_view_test.cpp
    variant_editor_test.cpp
    view_panel_layout_test.cpp
    view_panel_options_test.cpp
    viewport_display_test.cpp
    color_ui_test.cpp
    renderer_settings_test.cpp
    viewport_hud_test.cpp
    viewport_delete_test.cpp
    panel_maximizer_test.cpp
    peers_panel_test.cpp
    viewport_broadcast_test.cpp
    workspace_test.cpp
)
target_link_libraries(usdcc_ui_tests PRIVATE QtNodes::QtNodes)
target_compile_definitions(usdcc_ui_tests PRIVATE USDCC_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test_data")
target_link_libraries(usdcc_ui_tests PRIVATE usdcc_usd_ui Qt6::Test)
target_link_libraries(usdcc_ui_tests PRIVATE usdcc_physics_jolt_backend usdcc_viewport_broadcast_core)
usdcc_precompile_headers(usdcc_ui_tests ui_pch.h)

#   usdcc_python_tests — the embedded Python (docs/SCRIPTING.md). Its own
#                    executable because Python can only be started once per
#                    process; python_test_main.cpp starts it for every test.

add_executable(usdcc_python_tests
    test_harness.h
    test_harness.cpp
    python_test_main.cpp
    package_environment_test.cpp
    plugin_manager_test.cpp
    plugin_native_test.cpp
    plugin_tab_test.cpp
    python_app_test.cpp
    python_runtime_test.cpp
    script_editor_panel_test.cpp
    session_test.cpp
    startup_scripts_test.cpp
    viewport_camera_test.cpp
    viewport_renderer_python_test.cpp
    python_test_app.h
)
target_link_libraries(usdcc_python_tests PRIVATE usdcc_scripting usdcc_scripting_app usdcc_scripting_ui)
target_compile_definitions(usdcc_python_tests PRIVATE USDCC_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test_data")

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
    PROPERTIES PREFIX ""  # usdcc_test_plugin_helper.so, as the test names it (not lib...)
               RUNTIME_OUTPUT_DIRECTORY "$<1:${_usdcc_test_plugins}/test_native/bin/${USDCC_BUILT_FOR_PLATFORM}>"
               LIBRARY_OUTPUT_DIRECTORY "$<1:${_usdcc_test_plugins}/test_native/bin/${USDCC_BUILT_FOR_PLATFORM}>")
target_link_libraries(usdcc_test_plugin PRIVATE usdcc_test_plugin_helper)
add_dependencies(usdcc_python_tests usdcc_test_plugin)
target_compile_definitions(usdcc_python_tests PRIVATE USDCC_TEST_PLUGINS="${_usdcc_test_plugins}")
target_link_libraries(usdcc_python_tests PRIVATE usdcc_plugin_sdk)
target_link_libraries(usdcc_python_tests PRIVATE usdcc_physics_jolt_backend)
target_link_libraries(usdcc_python_tests PRIVATE Qt6::Test)
if(TARGET blender_mesh_tools)
    add_executable(usdcc_blender_mesh_benchmark blender_mesh_benchmark.cpp)
    target_link_libraries(usdcc_blender_mesh_benchmark PRIVATE usdcc_blender_mesh_conversion)
    if(WIN32)
        target_link_libraries(usdcc_blender_mesh_benchmark PRIVATE psapi)
    endif()
    if(Python3_LIBRARY AND EXISTS "${Python3_LIBRARY}")
        target_link_libraries(usdcc_blender_mesh_benchmark PRIVATE "${Python3_LIBRARY}")
    endif()
    usdcc_deploy_core_library(usdcc_blender_mesh_benchmark usdcc_usd usdcc_tools usdcc_ui usdcc_usd_ui)
    usdcc_deploy_python(usdcc_blender_mesh_benchmark)
    target_sources(usdcc_python_tests PRIVATE blender_mesh_plugin_test.cpp blender_mesh_conversion_test.cpp blender_mesh_operations_test.cpp)
    target_link_libraries(usdcc_python_tests PRIVATE usdcc_blender_mesh_conversion)
    target_compile_definitions(usdcc_python_tests PRIVATE
        USDCC_BLENDER_MESH_PLUGIN="${CMAKE_BINARY_DIR}/plugins/blender_mesh_tools")
    add_dependencies(usdcc_python_tests blender_mesh_tools)
endif()

target_sources(usdcc_python_tests PRIVATE rtx_native_stage_test.cpp
    rtx_native_navigation_test.cpp
    rtx_native_panel_test.cpp
    ../src/plugins/rtx_render_native/stage_bridge.cpp
    ../src/plugins/rtx_render_native/navigation.cpp
    ../src/plugins/rtx_render_native/runtime_identity.cpp
    ../src/plugins/rtx_render_native/rtx_panel.cpp)
target_include_directories(usdcc_python_tests PRIVATE ../src/plugins/rtx_render_native)
if(TARGET rtx_render_native)
    target_sources(usdcc_python_tests PRIVATE rtx_native_plugin_test.cpp)
    target_compile_definitions(usdcc_python_tests PRIVATE
        USDCC_NATIVE_RTX_PLUGIN="${CMAKE_BINARY_DIR}/plugins/rtx_render_native")
    add_dependencies(usdcc_python_tests rtx_render_native)
endif()
# Python-made panels need the usdcc.ui module, which needs PySide6.
if(TARGET usdcc_ui_python)
    target_sources(usdcc_python_tests PRIVATE plugin_extensions_test.cpp plugin_reload_test.cpp python_panels_test.cpp
                                              render_view_panel_python_test.cpp
                                              vray_render_plugin_test.cpp)
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
             "QT_QPA_PLATFORM_PLUGIN_PATH=set:${_usdcc_vcpkg_dir}/Qt6/plugins/platforms"
             # Qt's image plugins: the viewport broadcast writes JPEG (qjpeg.dll), which the
             # app has beside it and the tests don't.
             "QT_PLUGIN_PATH=set:${_usdcc_vcpkg_dir}/Qt6/plugins")
    endif()
    set_tests_properties(usdcc_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_usdcc_test_env}")
    set_tests_properties(usdcc_ui_tests usdcc_python_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_usdcc_ui_test_env}")
    set_target_properties(
        usdcc_tests usdcc_ui_tests usdcc_python_tests
        PROPERTIES
            VS_DEBUGGER_ENVIRONMENT
            "PATH=${USD_INSTALL}/bin;${USD_INSTALL}/lib;%PATH%\nPXR_PLUGINPATH_NAME=${USD_INSTALL}/plugin/usd")
else()
    # USD's plugins, and its libraries for whatever the executables' rpath
    # doesn't reach. Run on Linux (docs/LINUX_WSL.md), not yet on macOS.
    if(APPLE)
        set(_usdcc_library_path DYLD_LIBRARY_PATH)
    else()
        set(_usdcc_library_path LD_LIBRARY_PATH)
    endif()
    set_tests_properties(
        usdcc_tests usdcc_ui_tests usdcc_python_tests
        PROPERTIES
            ENVIRONMENT_MODIFICATION
            "${_usdcc_library_path}=path_list_prepend:${USD_INSTALL}/lib;PXR_PLUGINPATH_NAME=set:${USD_INSTALL}/plugin/usd")
endif()

# Qt's plugins beside the test programs, as the app has them (src/cpp/app/CMakeLists.txt):
# ctest points Qt at vcpkg's folder, but a test program started by hand, or from Visual
# Studio, has only its own folder, and stops at "no Qt platform plugin could be
# initialized". The windows and offscreen platforms; the JPEG plugin and libjpeg (the
# viewport broadcast writes JPEG); the Schannel TLS plugin. One step for the folder the
# programs share, so no two copy the same file at once.
if(WIN32)
    set(_usdcc_qt_bin "$<TARGET_FILE_DIR:Qt6::Core>")
    set(_usdcc_qt_plugins "${_usdcc_qt_bin}/../Qt6/plugins")
    set(_usdcc_tests_dir "$<TARGET_FILE_DIR:usdcc_ui_tests>")
    add_custom_target(usdcc_tests_qt_plugins
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_usdcc_tests_dir}/platforms"
                "${_usdcc_tests_dir}/imageformats" "${_usdcc_tests_dir}/tls"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_usdcc_qt_plugins}/platforms/qwindows.dll" "${_usdcc_qt_plugins}/platforms/qoffscreen.dll"
            "${_usdcc_tests_dir}/platforms/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_usdcc_qt_plugins}/imageformats/qjpeg.dll" "${_usdcc_tests_dir}/imageformats/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_usdcc_qt_bin}/jpeg62.dll" "${_usdcc_tests_dir}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_usdcc_qt_plugins}/tls/qschannelbackend.dll" "${_usdcc_tests_dir}/tls/"
        COMMENT "Deploying Qt's plugins beside the test programs"
        VERBATIM)
    foreach(_qt_test usdcc_tests usdcc_ui_tests usdcc_python_tests usdcc_blender_mesh_benchmark)
        if(TARGET ${_qt_test})
            add_dependencies(${_qt_test} usdcc_tests_qt_plugins)
        endif()
    endforeach()
endif()

# A focused entry with the same clean Qt/USD runtime environment as the app tests.
if(TARGET blender_mesh_tools)
    # Direct C ABI calls in conversion tests need the private backend at startup.
    add_custom_target(blender_mesh_test_runtime
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${USDCC_BLENDER_MESH_ROOT}/bin" "$<TARGET_FILE_DIR:usdcc_python_tests>"
        VERBATIM)
    add_dependencies(usdcc_python_tests blender_mesh_test_runtime)
    add_dependencies(usdcc_blender_mesh_benchmark blender_mesh_test_runtime)
    add_test(NAME usdcc_blender_mesh_plugin COMMAND usdcc_python_tests blender_mesh_native_plugin)
    add_test(NAME usdcc_blender_mesh_conversion COMMAND usdcc_python_tests blender_mesh_conversion)
    add_test(NAME usdcc_blender_mesh_operations COMMAND usdcc_python_tests blender_mesh_operations)
    get_test_property(usdcc_python_tests ENVIRONMENT_MODIFICATION _blender_test_environment)
    set_tests_properties(usdcc_blender_mesh_plugin usdcc_blender_mesh_conversion usdcc_blender_mesh_operations PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "${_blender_test_environment}")
    add_test(NAME usdcc_blender_mesh_build_tools
        COMMAND "${USDCC_PYTHON_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/blender_mesh_build_test.py")
    add_test(NAME usdcc_blender_mesh_package_tools
        COMMAND "${USDCC_PYTHON_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/blender_mesh_package_test.py")
endif()

# Jolt's DLL beside every test program that links the physics backend, wherever Jolt was
# found (docs/PHYSICS.md).
foreach(_physics_test usdcc_tests usdcc_ui_tests usdcc_python_tests)
    if(TARGET ${_physics_test})
        add_custom_command(TARGET ${_physics_test} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Jolt::Jolt>" "$<TARGET_FILE_DIR:${_physics_test}>"
            VERBATIM)
    endif()
endforeach()

# usdcc_assets (docs/REMOTE_ASSETS.md §14), in a test program of its own, without Qt:
# the fake:// protocol it resolves through has to be registered before anything asks
# OpenUSD for its resolver, which the other programs' tests do. The protocol is a
# plugin as a real one is: a resolver library and its plugInfo.json.
set(_usdcc_fake_protocol "${CMAKE_CURRENT_BINARY_DIR}/assets_fake_protocol")
add_library(usdccFakeProtocol SHARED assets_fake_protocol.cpp)
target_link_libraries(usdccFakeProtocol PRIVATE usdcc_assets)
# TF_REGISTRY_FUNCTION names the library it's in through these.
target_compile_definitions(usdccFakeProtocol PRIVATE MFB_PACKAGE_NAME=usdccFakeProtocol
                                                     MFB_ALT_PACKAGE_NAME=usdccFakeProtocol)
set_target_properties(
    usdccFakeProtocol
    PROPERTIES PREFIX ""
               RUNTIME_OUTPUT_DIRECTORY "$<1:${_usdcc_fake_protocol}>"
               LIBRARY_OUTPUT_DIRECTORY "$<1:${_usdcc_fake_protocol}>")
# file(GENERATE) evaluates $<TARGET_FILE_NAME>, the bracket argument notwithstanding.
file(GENERATE OUTPUT "${_usdcc_fake_protocol}/plugInfo.json" CONTENT [[{
    "Plugins": [{
        "Type": "library",
        "Name": "usdccFakeProtocol",
        "Root": ".",
        "LibraryPath": "$<TARGET_FILE_NAME:usdccFakeProtocol>",
        "ResourcePath": ".",
        "Info": { "Types": { "UsdccFakeResolver": {
            "bases": ["ArResolver"], "uriSchemes": ["fake"],
            "implementsContexts": true, "implementsScopedCaches": true,
            "usdccAssetsProtocol": true
        } } }
    }]
}
]])

add_executable(usdcc_assets_tests test_harness.h test_harness.cpp assets_test_main.cpp assets_test.cpp
                                  assets_net_test.cpp assets_test_server.h assets_test_server.cpp)
target_link_libraries(usdcc_assets_tests PRIVATE usdcc_assets usd sdf)
if(WIN32)
    # The test server's sockets.
    target_link_libraries(usdcc_assets_tests PRIVATE ws2_32)
endif()
# The http:// and webdav:// protocols, tested against a server inside the test program.
target_compile_definitions(
    usdcc_assets_tests
    PRIVATE USDCC_ASSETS_FAKE_PROTOCOL_DIR="${_usdcc_fake_protocol}"
            USDCC_ASSETS_HTTP_PROTOCOL_DIR="${CMAKE_BINARY_DIR}/plugins/http/usd"
            USDCC_ASSETS_WEBDAV_PROTOCOL_DIR="${CMAKE_BINARY_DIR}/plugins/webdav/usd")
add_dependencies(usdcc_assets_tests usdccFakeProtocol usdccHttpProtocol usdccWebDavProtocol)
foreach(_assets_target usdccFakeProtocol usdcc_assets_tests)
    # USD's headers ask for the release Python import library in every configuration.
    if(Python3_LIBRARY AND EXISTS "${Python3_LIBRARY}")
        target_link_libraries(${_assets_target} PRIVATE "${Python3_LIBRARY}")
    endif()
endforeach()
if(WIN32)
    # usdcc_assets.dll beside the test program, on every build, as
    # usdcc_deploy_core_library() does for the others. Not through it: that chains to
    # the other programs' copies, and so to the app's libraries, which this needs none of.
    add_custom_target(usdcc_assets_tests_copy_libraries
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:usdcc_assets_tests>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:usdcc_assets>" "$<TARGET_FILE_DIR:usdcc_assets_tests>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:CURL::libcurl>" "$<TARGET_FILE_DIR:usdcc_assets_tests>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ZLIB::ZLIB>" "$<TARGET_FILE_DIR:usdcc_assets_tests>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:pugixml::shared>" "$<TARGET_FILE_DIR:usdcc_assets_tests>"
        VERBATIM)
    add_dependencies(usdcc_assets_tests_copy_libraries usdcc_assets)
    add_dependencies(usdcc_assets_tests usdcc_assets_tests_copy_libraries)
endif()
usdcc_deploy_python(usdcc_assets_tests)
add_test(NAME usdcc_assets_tests COMMAND usdcc_assets_tests)
if(WIN32)
    set_tests_properties(usdcc_assets_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_usdcc_test_env}")
else()
    set_tests_properties(
        usdcc_assets_tests
        PROPERTIES
            ENVIRONMENT_MODIFICATION
            "${_usdcc_library_path}=path_list_prepend:${USD_INSTALL}/lib;PXR_PLUGINPATH_NAME=set:${USD_INSTALL}/plugin/usd")
endif()

# usdcc_peers (docs/PEERS.md §15), in a test program of its own: rooms over 127.0.0.1,
# with no USD and no widgets, as the library has none. libsodium's DLL is copied beside
# it by vcpkg's step, as a dependency of usdcc_peers.dll.
add_executable(usdcc_peers_tests test_harness.h test_harness.cpp test_main.cpp peers_test_support.h peers_test.cpp
                                 peers_rooms_test.cpp peers_discovery_test.cpp)
target_link_libraries(usdcc_peers_tests PRIVATE usdcc_peers Qt6::Network Qt6::Test)
if(WIN32)
    # usdcc_peers.dll beside the test program on every build, as for usdcc_assets_tests.
    add_custom_target(usdcc_peers_tests_copy_libraries
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:usdcc_peers_tests>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:usdcc_peers>" "$<TARGET_FILE_DIR:usdcc_peers_tests>"
        VERBATIM)
    add_dependencies(usdcc_peers_tests_copy_libraries usdcc_peers)
    add_dependencies(usdcc_peers_tests usdcc_peers_tests_copy_libraries)
endif()
add_test(NAME usdcc_peers_tests COMMAND usdcc_peers_tests)
if(WIN32 AND _VCPKG_INSTALLED_DIR)
    # Debug links the release Qt, which isn't beside a Debug build's programs.
    set_tests_properties(usdcc_peers_tests PROPERTIES ENVIRONMENT_MODIFICATION
                         "PATH=path_list_prepend:${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
endif()
