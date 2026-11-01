# usdcc.usd — StageManager and OpenUSD integration (milestone M2).
add_library(usdcc_usd STATIC
    include/usdcc/usd/stage.h
    include/usdcc/usd/property_categorizer.h
    include/usdcc/usd/stage_manager.h
    include/usdcc/usd/value_type_info.h
    src/property_categorizer.cpp
    src/stage.cpp
    src/stage_manager.cpp
    src/value_type_info.cpp
)
target_include_directories(usdcc_usd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" ${PXR_INCLUDE_DIRS})
# Deliberately no Qt here: StageManager (this target's only class) is
# non-UI logic and stays Qt-free — see docs/PLAN.md's "Qt only in UI
# classes" rule and stage_manager.h's own comment. usdcc_usd_ui below is
# where Qt enters this directory's build.
target_link_libraries(usdcc_usd PUBLIC usdcc_core ${PXR_LIBRARIES})

add_subdirectory(python)

# usdcc_usd_ui — the USD-aware ViewPanel subclasses (ViewportViewPanel,
# OutlinerViewPanel, AttributesViewPanel; M3/M4). Kept as a separate
# target from usdcc_usd (rather than folded into it) so usdcc.usd's
# pybind11 module doesn't pull in Qt Widgets/ADS as a transitive runtime
# dependency it doesn't otherwise need — usdcc.usd's Python bindings only
# expose StageManager's plain data surface.
add_library(usdcc_usd_ui STATIC
    include/usdcc/usd/attributes_view_panel.h
    include/usdcc/usd/hydra_viewport_window.h
    include/usdcc/usd/outliner_view_panel.h
    include/usdcc/usd/properties/array_editor_dialog.h
    include/usdcc/usd/properties/attribute_binding.h
    include/usdcc/usd/properties/category_section.h
    include/usdcc/usd/properties/color_swatch.h
    include/usdcc/usd/properties/data_type_gallery_dialog.h
    include/usdcc/usd/properties/number_field.h
    include/usdcc/usd/properties/property_editor.h
    include/usdcc/usd/properties/property_row.h
    include/usdcc/usd/properties/property_style.h
    include/usdcc/usd/stage_opener.h
    include/usdcc/usd/tool_options_bar.h
    include/usdcc/usd/tool_rail.h
    include/usdcc/usd/usd_prim_tree_model.h
    include/usdcc/usd/viewport_tool_bars.h
    include/usdcc/usd/viewport_view_panel.h
    include/usdcc/usd/undo_manager.h
    src/attributes_view_panel.cpp
    src/hydra_viewport_window.cpp
    src/outliner_view_panel.cpp
    src/properties/array_editor_dialog.cpp
    src/properties/attribute_binding.cpp
    src/properties/category_section.cpp
    src/properties/color_swatch.cpp
    src/properties/data_type_gallery_dialog.cpp
    src/properties/number_field.cpp
    src/properties/property_editors.cpp
    src/properties/property_row.cpp
    src/stage_opener.cpp
    src/tool_options_bar.cpp
    src/tool_rail.cpp
    src/usd_prim_tree_model.cpp
    src/viewport_tool_bars.cpp
    src/viewport_view_panel.cpp
    src/undo_manager.cpp
)
target_link_libraries(usdcc_usd_ui PUBLIC usdcc_usd usdcc_ui usdcc_tools Qt6::Widgets Qt6::OpenGL)
