# usdcc.ui — QMainWindow shell, SidePanel/ViewPanel base classes (milestone M1).
add_library(usdcc_ui STATIC
    include/usdcc/ui/main_window.h
    include/usdcc/ui/side_panel.h
    include/usdcc/ui/view_panel.h
    src/main_window.cpp
    src/side_panel.cpp
    src/view_panel.cpp
)
target_include_directories(usdcc_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# usdcc_usd: ViewPanel (below) requires usdcc::usd::Stage/StageManager
# unconditionally — see docs/PLAN.md §6 "ViewPanel: mandatory Stage
# association" for why usdcc_ui is no longer USD-agnostic.
target_link_libraries(usdcc_ui PUBLIC usdcc_core usdcc_usd Qt6::Widgets ads::qtadvanceddocking-qt6)

add_subdirectory(python)
