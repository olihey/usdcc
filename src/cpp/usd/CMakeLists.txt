# usdcc.usd — StageManager and OpenUSD integration (milestone M2).
if(USD_FOUND)
    add_library(usdcc_usd STATIC
        include/usdcc/usd/stage_manager.h
        src/stage_manager.cpp
    )
    target_include_directories(usdcc_usd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" ${PXR_INCLUDE_DIRS})
    target_link_libraries(usdcc_usd PUBLIC usdcc_core Qt6::Core ${PXR_LIBRARIES})
else()
    # Placeholder so the rest of the app builds without USD_INSTALL set (see
    # docs/PLAN.md milestone M0/M2): an empty header-only target.
    add_library(usdcc_usd INTERFACE)
    target_include_directories(usdcc_usd INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_sources(usdcc_usd INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include/usdcc/usd/usd.h")
    target_link_libraries(usdcc_usd INTERFACE usdcc_core)
endif()
