# usdcc.core — cross-cutting types shared by the rest of the C++ tree.
# Placeholder for M0; grows as later milestones need it.
add_library(usdcc_core INTERFACE)
target_include_directories(usdcc_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_sources(usdcc_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include/usdcc/core/core.h"
                                     "${CMAKE_CURRENT_SOURCE_DIR}/include/usdcc/core/signal.h")
