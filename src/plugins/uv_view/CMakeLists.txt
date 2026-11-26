# UV View (docs/PRIMVAR_VIEWER.md §18): a bundled plugin, built and enabled in every build,
# that adds a UV tab to the Primvars panel.
#
#   usdcc_uv_view_core  the layout and its problems (USD only, no Qt), a static library the
#                       plugin and the tests share
#   usdcc_uv_view_ui    the view (usdcc_usd_ui), the same
#   uv_view             the plugin: registers the view
add_library(usdcc_uv_view_core STATIC uv_layout.cpp uv_layout.h)
target_include_directories(usdcc_uv_view_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(usdcc_uv_view_core PUBLIC usdcc_usd)
set_target_properties(usdcc_uv_view_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(usdcc_uv_view_ui STATIC uv_view.cpp uv_view.h)
target_link_libraries(usdcc_uv_view_ui PUBLIC usdcc_uv_view_core usdcc_usd_ui Qt6::Widgets)
set_target_properties(usdcc_uv_view_ui PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)

usdcc_add_plugin(uv_view
    ID org.usdcc.uv-view
    NAME "UV View"
    VERSION 0.1.0
    DESCRIPTION "A UV tab in the Primvars panel: a mesh's UV set laid out flat, its UDIM tiles, a checker or its texture, flipped and degenerate faces and seams, linked to the viewport."
    SOURCES plugin.cpp)
target_link_libraries(uv_view PRIVATE usdcc_uv_view_ui)

if(TARGET usdcc)
    # Where PluginManager::shippedFolder() looks, so a build loads it as a deployed app does.
    add_custom_target(uv_view_app_plugin
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/plugins/uv_view" "${USDCC_APP_INSTALL_ROOT}/plugins/uv_view"
        DEPENDS uv_view VERBATIM)
    add_dependencies(usdcc uv_view_app_plugin)
endif()
