# usdcc.usd — StageManager and OpenUSD integration (milestone M2).
if(USD_FOUND)
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
    # Downstream code (the app's composition root, main.cpp) uses this to
    # conditionally compile the USD-dependent UI wiring, since usdcc_usd
    # falls back to an empty placeholder without USD_INSTALL (see the else()
    # branch below).
    target_compile_definitions(usdcc_usd PUBLIC USDCC_USD_AVAILABLE)

    add_subdirectory(python)

    # usdcc_usd_ui — the USD-aware ViewPanel subclasses (ViewportViewPanel,
    # OutlinerViewPanel, AttributesViewPanel; M3/M4). Kept as a separate
    # target from usdcc_usd (rather than folded into it) so usdcc.usd's
    # pybind11 module doesn't pull in Qt Widgets/ADS as a transitive runtime
    # dependency it doesn't otherwise need — usdcc.usd's Python bindings only
    # expose StageManager's plain data surface. Only exists when USD_FOUND,
    # same as usdcc_usd itself, since these panels have nothing to show
    # without USD.
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
else()
    # Placeholder so the rest of the app builds without USD_INSTALL set (see
    # docs/PLAN.md milestone M0/M2): an empty header-only target.
    add_library(usdcc_usd INTERFACE)
    target_include_directories(usdcc_usd INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_sources(usdcc_usd INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include/usdcc/usd/usd.h")
    target_link_libraries(usdcc_usd INTERFACE usdcc_core)
endif()
