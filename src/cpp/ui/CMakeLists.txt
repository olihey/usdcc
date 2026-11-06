# usdcc.ui — QMainWindow shell, SidePanel/ViewPanel base classes (milestone M1).
add_library(usdcc_ui SHARED
    include/usdcc/ui/app_settings.h
    include/usdcc/ui/current_stage_tracker.h
    include/usdcc/ui/flow_layout.h
    include/usdcc/ui/line_view.h
    include/usdcc/ui/log_model.h
    include/usdcc/ui/log_side_panel.h
    include/usdcc/ui/main_window.h
    include/usdcc/ui/panel_icon_button.h
    include/usdcc/ui/panel_menu_button.h
    include/usdcc/ui/qt_message_bridge.h
    include/usdcc/ui/scale_controller.h
    include/usdcc/ui/scaled_metrics.h
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
    include/usdcc/ui/view_panel.h
    include/usdcc/ui/view_panel_registry.h
    src/app_settings.cpp
    src/current_stage_tracker.cpp
    src/flow_layout.cpp
    src/line_view.cpp
    src/log_model.cpp
    src/log_side_panel.cpp
    src/main_window.cpp
    src/panel_icon_button.cpp
    src/panel_menu_button.cpp
    src/qt_message_bridge.cpp
    src/scale_controller.cpp
    src/settings.cpp
    src/settings_dialog.cpp
    src/side_panel.cpp
    src/text_view.cpp
    src/theme.cpp
    src/title_bar.cpp
    src/toast.cpp
    src/toast_log_sink.cpp
    src/toast_manager.cpp
    src/panel_maximizer.cpp
    src/view_panel.cpp
    src/view_panel_registry.cpp
)
# Theme stylesheets + icons (see theme.cpp). A library of their own, as CMake's
# export step can't find an AUTORCC object in a target that uses it.
usdcc_add_resources(usdcc_ui_resources resources/usdcc_ui.qrc)
target_include_directories(usdcc_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# usdcc_usd: ViewPanel (below) requires usdcc::usd::Stage/StageManager
# unconditionally — see docs/PLAN.md §6 "ViewPanel: mandatory Stage
# association" for why usdcc_ui is no longer USD-agnostic.
# QWindowKit is static, and private so that this DLL holds the only copy.
target_link_libraries(usdcc_ui PUBLIC usdcc_core usdcc_usd Qt6::Widgets ads::qtadvanceddocking-qt6
                               PRIVATE QWindowKit::Widgets usdcc_ui_resources)
target_compile_definitions(usdcc_ui PRIVATE USDCC_UI_BUILD)
usdcc_export_library(usdcc_ui)
usdcc_precompile_headers(usdcc_ui ui_pch.h)

add_subdirectory(python)
