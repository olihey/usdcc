# usdcc.tools — SelectTool/MoveTool/RotateTool/ScaleTool (milestone M5).
# Deliberately kept free of Qt Widgets/GL (see tool.h) — the viewport turns a
# Tool's GizmoGeometry output into actual draw calls, and only usdcc_usd_ui
# (not this target) needs Qt.
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
target_include_directories(usdcc_tools PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" ${PXR_INCLUDE_DIRS})
target_link_libraries(usdcc_tools PUBLIC usdcc_core ${PXR_LIBRARIES})
