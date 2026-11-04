# usdcc.core — cross-cutting types shared by the rest of the C++ tree.
#
# SHARED, unlike every other usdcc library: it holds process-wide state (the
# LogHub, docs/LOGGING.md §2.2) that the exe, the Python extension modules and
# later C++ plugins must all see as one instance. A static copy linked into
# each of those DLLs would give each its own hub.
add_library(usdcc_core SHARED
    include/usdcc/core/console_log_sink.h
    include/usdcc/core/core.h
    include/usdcc/core/export.h
    include/usdcc/core/file_log_sink.h
    include/usdcc/core/log.h
    include/usdcc/core/native_stream_capture.h
    include/usdcc/core/settings.h
    include/usdcc/core/signal.h
    include/usdcc/core/toast.h
    src/console_log_sink.cpp
    src/file_log_sink.cpp
    src/log.cpp
    src/native_stream_capture.cpp
    src/settings.cpp
)
target_include_directories(usdcc_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(usdcc_core PRIVATE USDCC_CORE_BUILD)
set_target_properties(usdcc_core PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

# Windows has no rpath: copy usdcc_core.dll next to every binary that loads it
# (the exe, the test executables, the .pyd modules). Elsewhere the build-tree
# rpath CMake sets by default finds it.
function(usdcc_deploy_core_library target)
    if(WIN32)
        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:usdcc_core>" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()
