# usdcc.usd — StageManager and OpenUSD integration (milestone M2).
add_library(usdcc_usd SHARED
    include/usdcc/usd/orbit_camera.h
    src/orbit_camera.cpp
    include/usdcc/usd/camera_framing.h
    src/camera_framing.cpp
    include/usdcc/usd/stage_camera.h
    src/stage_camera.cpp
    include/usdcc/usd/viewport_grid.h
    src/viewport_grid.cpp
    include/usdcc/usd/applied_schemas.h
    include/usdcc/usd/schema_info.h
    src/schema_info.cpp
    include/usdcc/usd/schema_library.h
    src/schema_library.cpp
    include/usdcc/usd/schema_libraries.h
    src/schema_libraries.cpp
    include/usdcc/usd/prim_selection.h
    include/usdcc/usd/composition_arcs.h
    include/usdcc/usd/dependencies.h
    include/usdcc/usd/layer_stack.h
    include/usdcc/usd/stage.h
    include/usdcc/usd/prim_creation_catalog.h
    include/usdcc/usd/property_categorizer.h
    include/usdcc/usd/relationship_targets.h
    include/usdcc/usd/scene_bounds.h
    include/usdcc/usd/scene_counts.h
    include/usdcc/usd/shader_node_catalog.h
    src/shader_node_catalog.cpp
    include/usdcc/usd/renderer_settings.h
    src/renderer_settings.cpp
    include/usdcc/usd/layered_layout.h
    src/layered_layout.cpp
    include/usdcc/usd/shade_graph.h
    src/shade_graph.cpp
    include/usdcc/usd/shading_preview.h
    src/shading_preview.cpp
    include/usdcc/usd/shade_connections.h
    include/usdcc/usd/stage_manager.h
    include/usdcc/usd/asset_servers.h
    include/usdcc/usd/asset_sources.h
    include/usdcc/usd/stage_names.h
    include/usdcc/usd/tf_diagnostic_bridge.h
    include/usdcc/usd/time_range.h
    src/time_range.cpp
    include/usdcc/usd/anim_edit.h
    src/anim_edit.cpp
    include/usdcc/usd/playback.h
    src/playback.cpp
    include/usdcc/usd/physics_bake.h
    src/physics_bake.cpp
    include/usdcc/usd/physics_check.h
    src/physics_check.cpp
    include/usdcc/usd/validation_run.h
    src/validation_run.cpp
    include/usdcc/usd/validation_rules.h
    src/validation_rules.cpp
    include/usdcc/usd/validation_finding.h
    include/usdcc/usd/validation_templates.h
    src/validation_templates.cpp
    include/usdcc/usd/validation_profile.h
    src/validation_profile.cpp
    include/usdcc/usd/physics_backend.h
    src/physics_backend.cpp
    include/usdcc/usd/physics_desc.h
    src/physics_desc.cpp
    include/usdcc/usd/physics_mass.h
    src/physics_mass.cpp
    include/usdcc/usd/physics_session.h
    src/physics_session.cpp
    include/usdcc/usd/sim_cache.h
    src/sim_cache.cpp
    include/usdcc/usd/usd_environment.h
    src/usd_environment.cpp
    include/usdcc/usd/arnold_runtime.h
    src/arnold_runtime.cpp
    include/usdcc/usd/color_management.h
    src/color_management.cpp
    include/usdcc/usd/linear_exr.h
    src/linear_exr.cpp
    include/usdcc/usd/usd_runtime_info.h
    src/usd_runtime_info.cpp
    include/usdcc/usd/value_type_info.h
    include/usdcc/usd/xform_math.h
    include/usdcc/usd/xform_stack.h
    src/applied_schemas.cpp
    src/composition_arcs.cpp
    src/dependencies.cpp
    src/layer_stack.cpp
    src/prim_creation_catalog.cpp
    src/prim_selection.cpp
    src/property_categorizer.cpp
    src/relationship_targets.cpp
    src/scene_bounds.cpp
    src/scene_counts.cpp
    src/shade_connections.cpp
    src/stage.cpp
    src/stage_manager.cpp
    src/asset_servers.cpp
    src/asset_sources.cpp
    src/stage_names.cpp
    src/tf_diagnostic_bridge.cpp
    src/value_type_info.cpp
    src/xform_math.cpp
    src/xform_stack.cpp
)
target_include_directories(usdcc_usd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" ${PXR_INCLUDE_DIRS})
# Deliberately no Qt here: StageManager (this target's only class) is
# non-UI logic and stays Qt-free — see docs/PLAN.md's "Qt only in UI
# classes" rule and stage_manager.h's own comment. usdcc_usd_ui below is
# where Qt enters this directory's build.
target_link_libraries(usdcc_usd PUBLIC usdcc_core ${PXR_LIBRARIES})
# Remote assets (docs/REMOTE_ASSETS.md §10): each stage's resolver context, and
# what the UI shows of a path on a server.
target_link_libraries(usdcc_usd PUBLIC usdcc_assets)
usdcc_export_library(usdcc_usd)
# OpenColorIO, from the OpenUSD install, which built it for Hydra's color correction
# (docs/COLOR_MANAGEMENT.md): the same library, so the config usdcc makes current is
# the one Hydra uses. Without it, color management is off and viewports show sRGB.
find_library(USDCC_OCIO_LIBRARY NAMES OpenColorIO HINTS "${USD_INSTALL}/lib" "${pxr_DIR}/lib" NO_DEFAULT_PATH)
find_path(USDCC_OCIO_INCLUDE_DIR OpenColorIO/OpenColorIO.h HINTS "${USD_INSTALL}/include" "${pxr_DIR}/include"
          NO_DEFAULT_PATH)
if(USDCC_OCIO_LIBRARY AND USDCC_OCIO_INCLUDE_DIR)
    message(STATUS "OpenColorIO for color management: ${USDCC_OCIO_LIBRARY}")
    target_link_libraries(usdcc_usd PRIVATE "${USDCC_OCIO_LIBRARY}")
    target_include_directories(usdcc_usd PRIVATE "${USDCC_OCIO_INCLUDE_DIR}")
    target_compile_definitions(usdcc_usd PRIVATE USDCC_HAS_OCIO)
else()
    message(STATUS "OpenColorIO for color management: none in the OpenUSD install; viewports show sRGB")
endif()
# OpenEXR, also from the OpenUSD install: Save Image's linear EXR (docs/COLOR_MANAGEMENT.md §6).
find_library(USDCC_OPENEXR_LIBRARY NAMES OpenEXR-3_1 OpenEXR HINTS "${USD_INSTALL}/lib" "${pxr_DIR}/lib" NO_DEFAULT_PATH)
find_library(USDCC_IEX_LIBRARY NAMES Iex-3_1 Iex HINTS "${USD_INSTALL}/lib" "${pxr_DIR}/lib" NO_DEFAULT_PATH)
find_library(USDCC_IMATH_LIBRARY NAMES Imath-3_1 Imath HINTS "${USD_INSTALL}/lib" "${pxr_DIR}/lib" NO_DEFAULT_PATH)
find_path(USDCC_OPENEXR_INCLUDE_DIR OpenEXR/ImfOutputFile.h HINTS "${USD_INSTALL}/include" "${pxr_DIR}/include"
          NO_DEFAULT_PATH)
if(USDCC_OPENEXR_LIBRARY AND USDCC_IEX_LIBRARY AND USDCC_IMATH_LIBRARY AND USDCC_OPENEXR_INCLUDE_DIR)
    message(STATUS "OpenEXR for linear EXR images: ${USDCC_OPENEXR_LIBRARY}")
    target_link_libraries(usdcc_usd PRIVATE "${USDCC_OPENEXR_LIBRARY}" "${USDCC_IEX_LIBRARY}" "${USDCC_IMATH_LIBRARY}")
    # Its headers include <ImathBox.h> and friends without the folder.
    target_include_directories(usdcc_usd PRIVATE "${USDCC_OPENEXR_INCLUDE_DIR}" "${USDCC_OPENEXR_INCLUDE_DIR}/Imath"
                                                 "${USDCC_OPENEXR_INCLUDE_DIR}/OpenEXR")
    # DLL builds, as OpenEXR's and Imath's own CMake targets say.
    target_compile_definitions(usdcc_usd PRIVATE USDCC_HAS_OPENEXR $<$<PLATFORM_ID:Windows>:OPENEXR_DLL> $<$<PLATFORM_ID:Windows>:IMATH_DLL>)
else()
    message(STATUS "OpenEXR for linear EXR images: none in the OpenUSD install; Save Image offers no EXR")
endif()
usdcc_precompile_headers(usdcc_usd usd_pch.h)

add_subdirectory(python)

# usdcc_render — final renders without widgets (docs/FINAL_RENDER.md, docs/RENDER_NODE.md §3.2):
# renderers, jobs, the scheduler and rendering with peers. Its own library so that a
# program without windows (usdcc_node) renders as the app does. Qt Gui only for Storm's
# offscreen OpenGL context. Its headers stay in usdcc/usd/render/.
add_library(usdcc_render SHARED
    include/usdcc/usd/render_camera.h
    src/render_camera.cpp
    include/usdcc/usd/render/final_renderer.h
    src/render/final_renderer.cpp
    include/usdcc/usd/render/render_spec.h
    src/render/render_spec.cpp
    include/usdcc/usd/render/render_job.h
    src/render/render_job.cpp
    include/usdcc/usd/render/hydra_final_renderer.h
    src/render/hydra_final_renderer.cpp
    include/usdcc/usd/render/render_scheduler.h
    src/render/render_scheduler.cpp
    include/usdcc/usd/render/scene_file_renderer.h
    src/render/scene_file_renderer.cpp
    include/usdcc/usd/render/render_bundle.h
    src/render/render_bundle.cpp
    include/usdcc/usd/render/render_network.h
    src/render/render_network.cpp
    include/usdcc/usd/render/render_queue.h
    src/render/render_queue.cpp
)
target_link_libraries(usdcc_render PUBLIC usdcc_usd usdcc_peers Qt6::Core Qt6::Gui)
target_compile_definitions(usdcc_render PRIVATE USDCC_RENDER_BUILD)
usdcc_export_library(usdcc_render)
usdcc_precompile_headers(usdcc_render usd_pch.h)

# usdcc_usd_ui — the USD-aware ViewPanel subclasses (ViewportViewPanel,
# OutlinerViewPanel, AttributesViewPanel; M3/M4). Kept as a separate
# target from usdcc_usd (rather than folded into it) so usdcc.usd's
# pybind11 module doesn't pull in Qt Widgets/ADS as a transitive runtime
# dependency it doesn't otherwise need — usdcc.usd's Python bindings only
# expose StageManager's plain data surface.
add_library(usdcc_usd_ui SHARED
    include/usdcc/usd/asset_picker.h
    src/asset_picker.cpp
    include/usdcc/usd/asset_servers_editor.h
    src/asset_servers_editor.cpp
    include/usdcc/usd/asset_settings.h
    src/asset_settings.cpp
    include/usdcc/usd/attributes_view_panel.h
    include/usdcc/usd/property_sheet.h
    src/property_sheet.cpp
    include/usdcc/usd/shading_preview_widget.h
    src/shading_preview_widget.cpp
    include/usdcc/usd/main_menu_actions.h
    src/main_menu_actions.cpp
    include/usdcc/usd/usd_shade_graph_model.h
    src/usd_shade_graph_model.cpp
    include/usdcc/usd/shader_node_graphics.h
    src/shader_node_graphics.cpp
    include/usdcc/usd/shade_actions.h
    src/shade_actions.cpp
    include/usdcc/usd/shader_node_palette.h
    src/shader_node_palette.cpp
    include/usdcc/usd/material_editor_view_panel.h
    src/material_editor_view_panel.cpp
    include/usdcc/usd/shader_nodes_dialog.h
    src/shader_nodes_dialog.cpp
    include/usdcc/usd/usd_runtime_dialog.h
    src/usd_runtime_dialog.cpp
    include/usdcc/usd/recent_files.h
    src/recent_files.cpp
    include/usdcc/usd/dependencies_view_panel.h
    include/usdcc/usd/schema_explorer_view_panel.h
    include/usdcc/usd/apply_schema_ui.h
    include/usdcc/usd/schema_generator.h
    include/usdcc/usd/hydra_viewport_window.h
    include/usdcc/usd/layer_actions.h
    include/usdcc/usd/layer_editor_view_panel.h
    include/usdcc/usd/log_test_messages.h
    include/usdcc/usd/outliner_item_delegate.h
    include/usdcc/usd/outliner_view_panel.h
    include/usdcc/usd/prim_actions.h
    include/usdcc/usd/prim_context_menu.h
    include/usdcc/usd/prim_icons.h
    include/usdcc/usd/preview_stage.h
    include/usdcc/usd/prim_picker_dialog.h
    include/usdcc/usd/prim_tree_dialog.h
    include/usdcc/usd/prim_tree_filter.h
    src/prim_tree_filter.cpp
    include/usdcc/usd/prim_tree_widget.h
    include/usdcc/usd/relationship_target_dialog.h
    include/usdcc/usd/shader_connection_dialog.h
    include/usdcc/usd/add_shade_input_dialog.h
    include/usdcc/usd/layer_properties_dialog.h
    include/usdcc/usd/properties/array_editor_dialog.h
    include/usdcc/usd/properties/attribute_binding.h
    include/usdcc/usd/properties/category_section.h
    include/usdcc/usd/properties/color_swatch.h
    include/usdcc/usd/properties/connected_inputs_editor.h
    include/usdcc/usd/properties/data_type_gallery_dialog.h
    include/usdcc/usd/properties/number_field.h
    include/usdcc/usd/properties/property_editor.h
    include/usdcc/usd/properties/property_row.h
    include/usdcc/usd/properties/property_style.h
    include/usdcc/usd/properties/relationship_targets_editor.h
    include/usdcc/usd/properties/schema_pills.h
    include/usdcc/usd/properties/transform_stack_editor.h
    include/usdcc/usd/close_stage_dialog.h
    include/usdcc/usd/save_stage_as.h
    include/usdcc/usd/create_menu.h
    include/usdcc/usd/physics_menu.h
    include/usdcc/usd/physics_overlay.h
    include/usdcc/usd/select_by_name_dialog.h
    include/usdcc/usd/selection_menu.h
    include/usdcc/usd/current_stage_ui.h
    include/usdcc/usd/session_stages.h
    include/usdcc/usd/open_stage_options_dialog.h
    include/usdcc/usd/stage_opener.h
    include/usdcc/usd/window_preview.h
    include/usdcc/usd/stages_menu.h
    include/usdcc/usd/render_view_panel.h
    include/usdcc/usd/tool_options_bar.h
    include/usdcc/usd/tool_rail.h
    include/usdcc/usd/usd_prim_tree_model.h
    include/usdcc/usd/variant_actions.h
    include/usdcc/usd/variant_editor_dialog.h
    include/usdcc/usd/usda_preview_dialog.h
    include/usdcc/usd/usda_syntax.h
    include/usdcc/usd/usda_view.h
    include/usdcc/usd/usda_view_panel.h
    include/usdcc/usd/viewport_tool_bars.h
    include/usdcc/usd/viewport_view_panel.h
    include/usdcc/usd/viewport_display.h
    include/usdcc/usd/viewport_hud.h
    include/usdcc/usd/viewport_render_params.h
    include/usdcc/usd/undo_manager.h
    include/usdcc/usd/time_axis.h
    src/time_axis.cpp
    include/usdcc/usd/time_ruler.h
    src/time_ruler.cpp
    include/usdcc/usd/time_range_slider.h
    src/time_range_slider.cpp
    include/usdcc/usd/playback_controller.h
    src/playback_controller.cpp
    include/usdcc/usd/timeline_bar.h
    src/timeline_bar.cpp
    src/attributes_view_panel.cpp
    src/dependencies_view_panel.cpp
    include/usdcc/usd/dope_sheet_view_panel.h
    src/dope_sheet_view_panel.cpp
    include/usdcc/usd/playblast.h
    src/playblast.cpp
    include/usdcc/usd/curve_editor_view_panel.h
    src/curve_editor_view_panel.cpp
    include/usdcc/usd/validation_view_panel.h
    src/validation_view_panel.cpp
    include/usdcc/usd/validation_profile_dialog.h
    src/validation_profile_dialog.cpp
    include/usdcc/usd/validation_fix.h
    src/validation_fix.cpp
    src/schema_explorer_view_panel.cpp
    src/apply_schema_ui.cpp
    src/schema_generator.cpp
    include/usdcc/usd/schema_library_editor.h
    src/schema_library_editor.cpp
    include/usdcc/usd/promote_schema_dialog.h
    src/promote_schema_dialog.cpp
    src/hydra_viewport_window.cpp
    src/selection_overlay.cpp
    src/selection_overlay.h
    src/viewport_overlay_renderer.cpp
    src/viewport_overlay_renderer.h
    src/viewport_display.cpp
    src/viewport_hud.cpp
    src/viewport_render_params.cpp
    src/layer_actions.cpp
    src/layer_editor_view_panel.cpp
    src/log_test_messages.cpp
    src/outliner_item_delegate.cpp
    src/outliner_view_panel.cpp
    src/prim_actions.cpp
    src/prim_context_menu.cpp
    src/prim_icons.cpp
    src/preview_stage.cpp
    src/prim_picker_dialog.cpp
    src/prim_tree_dialog.cpp
    src/prim_tree_widget.cpp
    src/relationship_target_dialog.cpp
    src/render_view_panel.cpp
    src/shader_connection_dialog.cpp
    src/add_shade_input_dialog.cpp
    src/layer_properties_dialog.cpp
    src/properties/array_editor_dialog.cpp
    src/properties/attribute_binding.cpp
    src/properties/color_swatch.cpp
    src/properties/connected_inputs_editor.cpp
    src/properties/data_type_gallery_dialog.cpp
    src/properties/number_field.cpp
    src/properties/property_editors.cpp
    src/properties/property_row.cpp
    src/properties/relationship_targets_editor.cpp
    src/properties/schema_pills.cpp
    src/properties/transform_stack_editor.cpp
    src/close_stage_dialog.cpp
    src/save_stage_as.cpp
    src/create_menu.cpp
    src/physics_menu.cpp
    src/physics_overlay.cpp
    src/select_by_name_dialog.cpp
    src/selection_menu.cpp
    src/current_stage_ui.cpp
    src/session_stages.cpp
    src/open_stage_options_dialog.cpp
    src/stage_opener.cpp
    src/window_preview.cpp
    src/stages_menu.cpp
    src/tool_options_bar.cpp
    src/tool_rail.cpp
    src/usd_prim_tree_model.cpp
    src/variant_actions.cpp
    src/variant_editor_dialog.cpp
    src/usda_preview_dialog.cpp
    src/usda_syntax.cpp
    src/usda_view.cpp
    src/usda_view_panel.cpp
    src/viewport_tool_bars.cpp
    src/viewport_view_panel.cpp
    src/undo_manager.cpp
    include/usdcc/usd/share/layer_ops.h
    src/share/layer_ops.cpp
    include/usdcc/usd/share/stage_sync.h
    src/share/stage_sync.cpp
    include/usdcc/usd/share/live_layers.h
    src/share/live_layers.cpp
    include/usdcc/usd/share/shared_stages.h
    src/share/shared_stages.cpp
    include/usdcc/usd/share/shared_stages_section.h
    src/share/shared_stages_section.cpp
    include/usdcc/usd/share/presence.h
    src/share/presence.cpp
    include/usdcc/usd/render/render_menu.h
    src/render/render_menu.cpp
    include/usdcc/usd/render/render_settings_view_panel.h
    src/render/render_settings_view_panel.cpp
    include/usdcc/usd/render/render_queue_panel.h
    src/render/render_queue_panel.cpp
    include/usdcc/usd/render/swarm_panel.h
    src/render/swarm_panel.cpp
)
# The Outliner's prim icons (see prim_icons.cpp), as a library for the reason in
# src/cpp/ui/CMakeLists.txt.
usdcc_add_resources(usdcc_usd_ui_resources resources/prim_icons.qrc)
target_link_libraries(usdcc_usd_ui PUBLIC usdcc_usd usdcc_render usdcc_ui usdcc_tools Qt6::Widgets Qt6::OpenGL PRIVATE Qt6::Svg usdcc_usd_ui_resources)
# Only the Material Editor sees it, so plugins built against the SDK don't need it.
target_link_libraries(usdcc_usd_ui PRIVATE QtNodes::QtNodes)
target_compile_definitions(usdcc_usd_ui PRIVATE USDCC_USD_UI_BUILD
    # The Material Editor preview's shapes and HDRIs, for a build that isn't deployed
    # (deployed, they're in resources/preview next to bin/; tools/deploy.py).
    USDCC_PREVIEW_ASSETS_DIR="${CMAKE_SOURCE_DIR}/resources/preview")
usdcc_export_library(usdcc_usd_ui)
usdcc_precompile_headers(usdcc_usd_ui ui_pch.h)
