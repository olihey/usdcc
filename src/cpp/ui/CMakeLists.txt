# usdcc.ui — QMainWindow shell, SidePanel/ViewPanel base classes (milestone M1).
add_library(usdcc_ui SHARED
    include/usdcc/ui/about_dialog.h
    include/usdcc/ui/app_settings.h
    include/usdcc/ui/category_section.h
    include/usdcc/ui/current_stage_tracker.h
    include/usdcc/ui/find_line_edit.h
    include/usdcc/ui/flow_layout.h
    include/usdcc/ui/line_view.h
    include/usdcc/ui/log_model.h
    include/usdcc/ui/log_panel.h
    include/usdcc/ui/main_window.h
    include/usdcc/ui/panel_icon_button.h
    include/usdcc/ui/options_menu_view.h
    include/usdcc/ui/panel_menu_button.h
    include/usdcc/ui/peer_rooms_debug_dialog.h
    include/usdcc/ui/peers_controller.h
    include/usdcc/ui/peers_side_panel.h
    include/usdcc/ui/qt_message_bridge.h
    include/usdcc/ui/scale_controller.h
    include/usdcc/ui/scaled_metrics.h
    include/usdcc/ui/session_store.h
    include/usdcc/ui/settings.h
    include/usdcc/ui/settings_dialog.h
    include/usdcc/ui/side_panel.h
    include/usdcc/ui/text_view.h
    include/usdcc/ui/theme.h
    include/usdcc/ui/title_bar.h
    include/usdcc/ui/toast.h
    include/usdcc/ui/toast_log_sink.h
    include/usdcc/ui/toast_manager.h
    include/usdcc/ui/panel_maximizer.h
    include/usdcc/ui/busy.h
    include/usdcc/ui/dock_panel.h
    include/usdcc/ui/view_panel.h
    include/usdcc/ui/view_panel_registry.h
    include/usdcc/ui/workspace.h
    include/usdcc/ui/workspace_applier.h
    include/usdcc/ui/workspace_controller.h
    include/usdcc/ui/workspace_library.h
    src/about_dialog.cpp
    src/app_settings.cpp
    src/category_section.cpp
    src/current_stage_tracker.cpp
    src/flow_layout.cpp
    src/line_view.cpp
    src/log_model.cpp
    src/log_panel.cpp
    src/main_window.cpp
    src/panel_icon_button.cpp
    src/options_menu_view.cpp
    src/panel_menu_button.cpp
    src/peer_rooms_debug_dialog.cpp
    src/peers_controller.cpp
    src/peers_side_panel.cpp
    src/qt_message_bridge.cpp
    src/scale_controller.cpp
    src/session_store.cpp
    src/settings.cpp
    src/settings_dialog.cpp
    src/side_panel.cpp
    src/dock_panel.cpp
    src/text_view.cpp
    src/theme.cpp
    src/title_bar.cpp
    src/toast.cpp
    src/toast_log_sink.cpp
    src/toast_manager.cpp
    src/panel_maximizer.cpp
    src/busy_overlay.cpp
    src/busy_overlay.h
    src/view_panel.cpp
    src/view_panel_registry.cpp
    src/workspace.cpp
    src/workspace_applier.cpp
    src/workspace_controller.cpp
    src/workspace_library.cpp
)
# Theme stylesheets + icons (see theme.cpp). A library of their own, as CMake's
# export step can't find an AUTORCC object in a target that uses it.
usdcc_add_resources(usdcc_ui_resources resources/usdcc_ui.qrc)
target_include_directories(usdcc_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# usdcc_usd: ViewPanel (below) requires usdcc::usd::Stage/StageManager
# unconditionally — see docs/PLAN.md §6 "ViewPanel: mandatory Stage
# association" for why usdcc_ui is no longer USD-agnostic.
# QWindowKit is static, and private so that this DLL holds the only copy.
# usdcc_peers: the Peers panel and its rooms (docs/PEERS.md §10.1).
target_link_libraries(usdcc_ui PUBLIC usdcc_core usdcc_usd usdcc_peers Qt6::Widgets ads::qtadvanceddocking-qt6
                               PRIVATE QWindowKit::Widgets usdcc_ui_resources)
target_compile_definitions(usdcc_ui PRIVATE USDCC_UI_BUILD
    # The shipped workspaces, for a build that isn't deployed (deployed, they're in
    # resources/workspaces next to bin/; tools/deploy.py). docs/WORKSPACES.md §5.
    USDCC_WORKSPACES_DIR="${CMAKE_SOURCE_DIR}/resources/workspaces")
usdcc_export_library(usdcc_ui)
usdcc_precompile_headers(usdcc_ui ui_pch.h)

add_subdirectory(python)
