# usdcc.tools — SelectTool/MoveTool/RotateTool/ScaleTool (milestone M5).
# Only exists when USD_FOUND, same as usdcc_usd (see src/cpp/usd/CMakeLists.txt):
# every Tool operates on UsdPrim/UsdGeomXformable. Deliberately kept free of
# Qt Widgets/GL (see tool.h) — the viewport turns a Tool's GizmoGeometry
# output into actual draw calls, and only usdcc_usd_ui (not this target)
# needs Qt.
if(USD_FOUND)
    add_library(usdcc_tools STATIC
        include/usdcc/tools/gizmo_shapes.h
        include/usdcc/tools/move_tool.h
        include/usdcc/tools/rotate_tool.h
        include/usdcc/tools/scale_tool.h
        include/usdcc/tools/select_tool.h
        include/usdcc/tools/tool.h
        include/usdcc/tools/transform_gizmo_tool.h
        src/gizmo_shapes.cpp
        src/move_tool.cpp
        src/rotate_tool.cpp
        src/scale_tool.cpp
        src/select_tool.cpp
        src/transform_gizmo_tool.cpp
    )
    target_include_directories(usdcc_tools PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(usdcc_tools PUBLIC usdcc_core ${PXR_LIBRARIES})
    target_include_directories(usdcc_tools PUBLIC ${PXR_INCLUDE_DIRS})
else()
    # Placeholder so the rest of the app builds without USD_INSTALL set (see
    # docs/PLAN.md milestone M0): an empty header-only target.
    add_library(usdcc_tools INTERFACE)
    target_include_directories(usdcc_tools INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_sources(usdcc_tools INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include/usdcc/tools/tools.h")
    target_link_libraries(usdcc_tools INTERFACE usdcc_core)
endif()
