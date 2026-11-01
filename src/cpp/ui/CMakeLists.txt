# usdcc.ui — QMainWindow shell, SidePanel/ViewPanel base classes (milestone M1).
add_library(usdcc_ui STATIC
    include/usdcc/ui/main_window.h
    include/usdcc/ui/scale_controller.h
    include/usdcc/ui/scaled_metrics.h
    include/usdcc/ui/settings.h
    include/usdcc/ui/side_panel.h
    include/usdcc/ui/theme.h
    include/usdcc/ui/title_bar.h
    include/usdcc/ui/view_panel.h
    include/usdcc/ui/view_panel_registry.h
    src/main_window.cpp
    src/scale_controller.cpp
    src/settings.cpp
    src/side_panel.cpp
    src/theme.cpp
    src/title_bar.cpp
    src/view_panel.cpp
    src/view_panel_registry.cpp
    # Theme stylesheets + icons (AUTORCC compiles this in; see theme.cpp).
    resources/usdcc_ui.qrc
)
target_include_directories(usdcc_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# usdcc_usd: ViewPanel (below) requires usdcc::usd::Stage/StageManager
# unconditionally — see docs/PLAN.md §6 "ViewPanel: mandatory Stage
# association" for why usdcc_ui is no longer USD-agnostic.
target_link_libraries(usdcc_ui PUBLIC usdcc_core usdcc_usd Qt6::Widgets ads::qtadvanceddocking-qt6
                                      QWindowKit::Widgets)

add_subdirectory(python)
