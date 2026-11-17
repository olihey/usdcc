# Viewport Broadcast (docs/VIEWPORT_BROADCAST.md): a bundled plugin, built and enabled in
# every build, that streams a viewport to web pages through a room of usdcc_peers.
#
#   usdcc_viewport_broadcast_core  the broadcast (Qt and usdcc_peers only), a static
#                                  library the plugin and the tests share
#   viewport_broadcast             the plugin: the Broadcast entry, the viewport as its
#                                  picture, toasts; and the page, in web/
add_library(usdcc_viewport_broadcast_core STATIC broadcast.cpp broadcast.h)
target_include_directories(usdcc_viewport_broadcast_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(usdcc_viewport_broadcast_core PUBLIC usdcc_peers Qt6::Gui)
set_target_properties(usdcc_viewport_broadcast_core PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)

usdcc_add_plugin(viewport_broadcast
    ID org.usdcc.viewport-broadcast
    NAME "Viewport Broadcast"
    VERSION 0.1.0
    DESCRIPTION "Broadcast in a viewport's menu: a web link to watch that viewport live, with its watchers' chat."
    SOURCES plugin.cpp)
target_link_libraries(viewport_broadcast PRIVATE usdcc_viewport_broadcast_core)

# The page, which the room serves at / (PEERS_PROTOCOL.md §1).
set(_folder "${CMAKE_BINARY_DIR}/plugins/viewport_broadcast")
add_custom_command(TARGET viewport_broadcast POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/web" "${_folder}/web"
    VERBATIM)
if(TARGET usdcc)
    # Where PluginManager::shippedFolder() looks, so a build loads it as a deployed app does.
    add_custom_target(viewport_broadcast_app_plugin
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_folder}" "$<TARGET_FILE_DIR:usdcc>/../plugins/viewport_broadcast"
        DEPENDS viewport_broadcast VERBATIM)
    add_dependencies(usdcc viewport_broadcast_app_plugin)
endif()
