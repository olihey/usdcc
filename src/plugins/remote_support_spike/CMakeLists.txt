# Remote Support phase 0 spike (docs/REMOTE_SUPPORT.md §7, §10): every usdcc window
# composited into one picture, sent through Viewport Broadcast's room and page, with
# the page's mouse and keyboard replayed into the app. A developer's tool, built only
# with USDCC_BUILD_REMOTE_SUPPORT_SPIKE and never deployed.
usdcc_add_plugin(remote_support_spike
    ID org.usdcc.remote-support-spike
    NAME "Remote Support Spike"
    VERSION 0.0.1
    DESCRIPTION "Phase 0 spike of Remote Support: usdcc's windows on a web page, controlled from it."
    SOURCES plugin.cpp)
# QWindowSystemInterface, the platform plugin's way in: a private Qt API (§10).
find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
target_link_libraries(remote_support_spike PRIVATE usdcc_viewport_broadcast_core Qt6::GuiPrivate)

set(_folder "${CMAKE_BINARY_DIR}/plugins/remote_support_spike")
add_custom_command(TARGET remote_support_spike POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/web" "${_folder}/web"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/src/cpp/ui/resources/theme/usdcc.css"
            "${_folder}/web/usdcc.css"
    VERBATIM)
if(TARGET usdcc)
    # Where PluginManager::shippedFolder() looks, so a build loads it.
    add_custom_target(remote_support_spike_app_plugin
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_folder}" "$<TARGET_FILE_DIR:usdcc>/../plugins/remote_support_spike"
        DEPENDS remote_support_spike VERBATIM)
    add_dependencies(usdcc remote_support_spike_app_plugin)
endif()
