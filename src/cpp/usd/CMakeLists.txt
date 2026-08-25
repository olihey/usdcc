# usdcc.usd — StageManager and OpenUSD integration (milestone M2).
if(USD_FOUND)
    add_library(usdcc_usd STATIC
        include/usdcc/usd/stage_manager.h
        src/stage_manager.cpp
    )
    target_include_directories(usdcc_usd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" ${PXR_INCLUDE_DIRS})
    target_link_libraries(usdcc_usd PUBLIC usdcc_core Qt6::Core ${PXR_LIBRARIES})
    # Downstream code (the app's composition root, main.cpp) uses this to
    # conditionally compile the USD-dependent UI wiring, since usdcc_usd
    # falls back to an empty placeholder without USD_INSTALL (see the else()
    # branch below).
    target_compile_definitions(usdcc_usd PUBLIC USDCC_USD_AVAILABLE)

    add_subdirectory(python)

    # ViewportViewPanel/HydraViewportWindow — the Hydra-rendering ViewPanel
    # from milestone M3. Kept as a separate target from usdcc_usd (rather
    # than folded into it) so usdcc.usd's pybind11 module doesn't pull in Qt
    # Widgets/ADS as a transitive runtime dependency it doesn't otherwise
    # need — usdcc.usd's Python bindings only expose StageManager's plain
    # data surface. Only exists when USD_FOUND, same as usdcc_usd itself,
    # since there is nothing to render without USD.
    add_library(usdcc_usd_viewport STATIC
        include/usdcc/usd/hydra_viewport_window.h
        include/usdcc/usd/viewport_view_panel.h
        src/hydra_viewport_window.cpp
        src/viewport_view_panel.cpp
    )
    target_link_libraries(usdcc_usd_viewport PUBLIC usdcc_usd usdcc_ui Qt6::Widgets)
else()
    # Placeholder so the rest of the app builds without USD_INSTALL set (see
    # docs/PLAN.md milestone M0/M2): an empty header-only target.
    add_library(usdcc_usd INTERFACE)
    target_include_directories(usdcc_usd INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_sources(usdcc_usd INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include/usdcc/usd/usd.h")
    target_link_libraries(usdcc_usd INTERFACE usdcc_core)
endif()
