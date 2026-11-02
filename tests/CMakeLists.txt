# Two test executables on a tiny in-repo harness (test_harness.h/.cpp) rather
# than GoogleTest, so they need no extra dependency. Run them through CTest
# (`ctest -C RelWithDebInfo`) or directly, optionally with a name filter as the
# first argument.
#   usdcc_tests    — headless tests for the non-UI logic (tools, undo, stage
#                    change notices, value-type classification, ...).
#   usdcc_ui_tests — widget tests (UI scale), which need a full QApplication;
#                    CTest runs them on Qt's offscreen platform.
add_executable(usdcc_tests
    test_harness.h
    test_harness.cpp
    test_main.cpp
    categorizer_spike_test.cpp
    log_bridges_test.cpp
    log_capture_test.cpp
    log_test.cpp
    move_tool_test.cpp
    property_categorizer_test.cpp
    rotate_tool_test.cpp
    stage_changes_test.cpp
    stage_names_test.cpp
    undo_manager_test.cpp
    value_type_info_test.cpp
    view_panel_registry_test.cpp
    xform_math_test.cpp
    xform_stack_test.cpp
    xform_target_test.cpp
)
target_link_libraries(usdcc_tests PRIVATE usdcc_tools usdcc_usd usdcc_usd_ui Qt6::Core)

add_executable(usdcc_ui_tests
    test_harness.h
    test_harness.cpp
    ui_test_main.cpp
    log_panel_test.cpp
    scale_controller_test.cpp
)
target_link_libraries(usdcc_ui_tests PRIVATE usdcc_usd_ui)

foreach(_usdcc_test usdcc_tests usdcc_ui_tests)
    # Same reason as the app (see src/cpp/app/CMakeLists.txt): USD's headers ask
    # for the release Python import library in every configuration.
    if(Python3_LIBRARY AND EXISTS "${Python3_LIBRARY}")
        target_link_libraries(${_usdcc_test} PRIVATE "${Python3_LIBRARY}")
    endif()
    usdcc_deploy_core_library(${_usdcc_test})
    add_test(NAME ${_usdcc_test} COMMAND ${_usdcc_test})
endforeach()

# No window ever appears; the widgets still lay out and paint.
set_tests_properties(usdcc_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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
    set_tests_properties(usdcc_ui_tests PROPERTIES ENVIRONMENT_MODIFICATION "${_usdcc_ui_test_env}")
    set_target_properties(
        usdcc_tests usdcc_ui_tests
        PROPERTIES
            VS_DEBUGGER_ENVIRONMENT
            "PATH=${USD_INSTALL}/bin;${USD_INSTALL}/lib;%PATH%\nPXR_PLUGINPATH_NAME=${USD_INSTALL}/plugin/usd")
endif()
