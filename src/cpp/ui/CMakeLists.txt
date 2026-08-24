# usdcc.ui — QMainWindow shell, SidePanel/ViewPanel base classes (milestone M1).
# M0 scope: a bare MainWindow so the app shell builds and launches.
add_library(usdcc_ui STATIC
    include/usdcc/ui/main_window.h
    src/main_window.cpp
)
target_include_directories(usdcc_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(usdcc_ui PUBLIC usdcc_core Qt6::Widgets)
