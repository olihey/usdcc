# Keeps every build's PDBs and binaries for reading crash dumps later
# (tools/symbol_store.py, tools/analyze_crash.py).
#
# usdcc_archive_symbols(), called at the end of the top-level CMakeLists once
# every target exists, adds the usdcc_symbols target. It is part of ALL and
# depends on every executable and DLL the build makes, so it runs last in any
# build of everything: tools/build.py and run.py, or Build Solution in Visual
# Studio. (Building one project in Visual Studio skips it.) It hands the
# configuration's binaries to symbol_store.py in a list file, which copies the
# ones not yet stored.
#
# Off by default on CI (the CI environment variable is set there): a runner
# throws its disk away, and the release workflow zips the PDBs itself.

if(DEFINED ENV{CI})
    set(_usdcc_archive_default OFF)
else()
    set(_usdcc_archive_default ON)
endif()
option(USDCC_ARCHIVE_SYMBOLS "Copy each build's PDBs and binaries into ~/.usdcc-symbols (Windows)"
       ${_usdcc_archive_default})

function(_usdcc_binary_targets directory out)
    set(found "")
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        get_target_property(excluded ${target} EXCLUDE_FROM_ALL)
        if(type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$" AND NOT excluded)
            list(APPEND found ${target})
        endif()
    endforeach()
    get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(subdirectory IN LISTS subdirectories)
        _usdcc_binary_targets("${subdirectory}" more)
        list(APPEND found ${more})
    endforeach()
    set(${out} ${found} PARENT_SCOPE)
endfunction()

function(usdcc_archive_symbols)
    if(NOT WIN32 OR NOT USDCC_ARCHIVE_SYMBOLS)
        return()
    endif()
    set(python "${Python3_EXECUTABLE}")
    if(NOT python OR NOT EXISTS "${python}")
        find_package(Python3 COMPONENTS Interpreter QUIET)
        set(python "${Python3_EXECUTABLE}")
    endif()
    if(NOT python)
        message(STATUS "No Python found: build symbols are not archived")
        return()
    endif()

    _usdcc_binary_targets("${CMAKE_SOURCE_DIR}" targets)
    set(content "")
    foreach(target IN LISTS targets)
        string(APPEND content "$<TARGET_FILE:${target}>\n")
    endforeach()
    set(list_file "${CMAKE_BINARY_DIR}/symbols/$<CONFIG>.txt")
    file(GENERATE OUTPUT "${list_file}" CONTENT "${content}")

    add_custom_target(usdcc_symbols ALL
        COMMAND "${python}" "${CMAKE_SOURCE_DIR}/tools/symbol_store.py" archive "${list_file}"
        COMMENT "Archiving symbols"
        VERBATIM)
    add_dependencies(usdcc_symbols ${targets})
endfunction()
