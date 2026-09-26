# usdcc.usd — StageManager and OpenUSD integration (milestone M2).
add_library(usdcc_usd STATIC
    include/usdcc/usd/stage.h
    include/usdcc/usd/stage_manager.h
    src/stage.cpp
    src/stage_manager.cpp
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
    include/usdcc/usd/usd_prim_tree_model.h
    include/usdcc/usd/viewport_view_panel.h
    src/attributes_view_panel.cpp
    src/hydra_viewport_window.cpp
    src/outliner_view_panel.cpp
    src/usd_prim_tree_model.cpp
    src/viewport_view_panel.cpp
)
target_link_libraries(usdcc_usd_ui PUBLIC usdcc_usd usdcc_ui usdcc_tools Qt6::Widgets Qt6::OpenGL)
