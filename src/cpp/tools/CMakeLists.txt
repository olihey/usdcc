# usdcc.tools — SelectTool/MoveTool/RotateTool/ScaleTool (milestone M5).
# Placeholder for M0; grows as later milestones need it.
add_library(usdcc_tools INTERFACE)
target_include_directories(usdcc_tools INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_sources(usdcc_tools INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include/usdcc/tools/tools.h")
target_link_libraries(usdcc_tools INTERFACE usdcc_core)
