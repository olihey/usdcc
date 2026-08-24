# usdcc.usd — StageManager and OpenUSD integration (milestone M2).
# Placeholder for M0: builds as an empty header-only target regardless of
# whether OpenUSD was found, so the app shell doesn't depend on it yet.
add_library(usdcc_usd INTERFACE)
target_include_directories(usdcc_usd INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_sources(usdcc_usd INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include/usdcc/usd/usd.h")
target_link_libraries(usdcc_usd INTERFACE usdcc_core)

if(USD_FOUND)
    target_include_directories(usdcc_usd INTERFACE ${PXR_INCLUDE_DIRS})
    target_link_libraries(usdcc_usd INTERFACE ${PXR_LIBRARIES})
endif()
