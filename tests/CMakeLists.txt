# usdcc_tests — headless tests for the non-UI logic (tools, undo, stage change
# notices, value-type classification). One executable on a tiny in-repo harness
# (test_harness.h) rather than GoogleTest, so it needs no extra dependency; run
# it through CTest (`ctest -C RelWithDebInfo`) or directly, optionally with a
# name filter as its first argument.
add_executable(usdcc_tests
    test_harness.h
    test_main.cpp
    categorizer_spike_test.cpp
    move_tool_test.cpp
    property_categorizer_test.cpp
    rotate_tool_test.cpp
    stage_changes_test.cpp
    undo_manager_test.cpp
    value_type_info_test.cpp
)
target_link_libraries(usdcc_tests PRIVATE usdcc_tools usdcc_usd usdcc_usd_ui Qt6::Core)

# Same reason as the app (see src/cpp/app/CMakeLists.txt): USD's headers ask
# for the release Python import library in every configuration.
if(Python3_LIBRARY AND EXISTS "${Python3_LIBRARY}")
    target_link_libraries(usdcc_tests PRIVATE "${Python3_LIBRARY}")
endif()

add_test(NAME usdcc_tests COMMAND usdcc_tests)

if(WIN32)
    # USD's DLLs and plugins aren't next to the executable; Qt's are (vcpkg's
    # applocal step), except for Debug, which links the release Qt (see the
    # top-level CMakeLists.txt) — so its bin dir goes on PATH too.
    set(_usdcc_test_path
        "PATH=path_list_prepend:${USD_INSTALL}/bin"
        "PATH=path_list_prepend:${USD_INSTALL}/lib")
    if(_VCPKG_INSTALLED_DIR)
        list(APPEND _usdcc_test_path "PATH=path_list_prepend:${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    endif()
    set_tests_properties(
        usdcc_tests PROPERTIES ENVIRONMENT_MODIFICATION
                               "${_usdcc_test_path};PXR_PLUGINPATH_NAME=set:${USD_INSTALL}/plugin/usd")
    set_target_properties(
        usdcc_tests
        PROPERTIES
            VS_DEBUGGER_ENVIRONMENT
            "PATH=${USD_INSTALL}/bin;${USD_INSTALL}/lib;%PATH%\nPXR_PLUGINPATH_NAME=${USD_INSTALL}/plugin/usd")
endif()
