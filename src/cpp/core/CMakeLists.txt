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

# Windows has no rpath: copy usdcc_core.dll, and the other usdcc libraries named,
# next to every binary that loads them (the exe, the test executables, the .pyd
# modules). Elsewhere the build-tree rpath CMake sets by default finds them. A
# second copy in another folder is harmless: Windows uses the DLL of that name
# already loaded, so a process has one instance.
function(usdcc_deploy_core_library target)
    if(WIN32)
        # On every build, not after linking `target`: changing a DLL often doesn't
        # relink what uses it, which would leave a stale copy beside it.
        set(_commands COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>")
        foreach(_library usdcc_core ${ARGN})
            list(APPEND _commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${_library}>"
                 "$<TARGET_FILE_DIR:${target}>")
        endforeach()
        add_custom_target(${target}_copy_libraries ${_commands} VERBATIM)
        add_dependencies(${target}_copy_libraries usdcc_core ${ARGN})
        add_dependencies(${target} ${target}_copy_libraries)
        # One copy step after another: several targets share an output folder
        # (the test programs, the Python modules), and in a parallel build two
        # steps copying the same DLL there at once fail on the locked file.
        get_property(_previous GLOBAL PROPERTY USDCC_LAST_COPY_LIBRARIES_TARGET)
        if(_previous)
            add_dependencies(${target}_copy_libraries ${_previous})
        endif()
        set_property(GLOBAL PROPERTY USDCC_LAST_COPY_LIBRARIES_TARGET ${target}_copy_libraries)
    endif()
endfunction()

# A static library of Qt resources (.qrc), for a DLL to link. Something in the DLL
# has to call Q_INIT_RESOURCE, which is also what pulls them in from the library.
function(usdcc_add_resources target)
    add_library(${target} STATIC ${ARGN})
    set_target_properties(${target} PROPERTIES AUTORCC ON POSITION_INDEPENDENT_CODE ON LINKER_LANGUAGE CXX)
    target_link_libraries(${target} PRIVATE Qt6::Core)
endfunction()

# A usdcc library that plugins and the Python modules link: a DLL with its
# functions exported (docs/PLUGINS.md §3.2). Data has to be marked (core/export.h).
function(usdcc_export_library target)
    set_target_properties(${target} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    if(MSVC)
        # A class marked for export (a Q_OBJECT class, for its staticMetaObject,
        # which the generated list leaves out) has its vtables exported twice:
        # by the class's dllexport and by WINDOWS_EXPORT_ALL_SYMBOLS's list, which
        # includes them. The linker warns (LNK4197, "export specified multiple
        # times") and uses the first; both name the same symbol, so nothing is
        # wrong. Neither can go (the list can't be filtered, and the marking is
        # what exports the meta-objects), so the warning is turned off here.
        target_link_options(${target} PRIVATE /IGNORE:4197)
    endif()
    # USD's headers ask for the release Python import library in every
    # configuration, as for the app (src/cpp/app/CMakeLists.txt).
    if(Python3_LIBRARY AND EXISTS "${Python3_LIBRARY}")
        target_link_libraries(${target} PRIVATE "${Python3_LIBRARY}")
    endif()
endfunction()
