# Makes AUTOMOC run again when what the build is made of changes.
#
# A target's moc step (cmake -E cmake_autogen) runs again only when one of its
# recorded inputs changes: moc, uic, the headers it parsed and the CMake files
# read in the target's own folder. Whether OpenUSD's Python was found, or an
# option such as USDCC_BUILD_EDITOR_LINK, is a cache value: it changes which
# headers a target has (src/cpp/scripting/CMakeLists.txt adds the Python view
# panels only with Python) without changing any of those. moc then keeps its
# old output, and the link fails on the new classes ("undefined
# staticMetaObject"), as it did when Python was turned on in a build configured
# without it. AUTOGEN_TARGET_DEPENDS doesn't help: Ninja gets a file there as an
# order-only dependency, which never makes a step run again.
#
# So the build's options and what was found are kept in a file, and when a
# configure finds them changed it deletes the timestamp of each AUTOMOC target
# of usdcc's own (<target>_autogen/timestamp*), the output the build tool checks:
# its moc step runs again at the next build, for those targets and only then.
# Call last, once every target exists.

function(_usdcc_autogen_targets directory out)
    set(found "")
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(automoc ${target} AUTOMOC)
        if(automoc)
            list(APPEND found ${target})
        endif()
    endforeach()
    get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(subdirectory IN LISTS subdirectories)
        # Not the external projects' own (QWindowKit): their sources don't vary with usdcc's options.
        if(subdirectory MATCHES "^${CMAKE_SOURCE_DIR}/external/")
            continue()
        endif()
        _usdcc_autogen_targets("${subdirectory}" more)
        list(APPEND found ${more})
    endforeach()
    set(${out} ${found} PARENT_SCOPE)
endfunction()

function(usdcc_autogen_state)
    set(state "# What AUTOMOC's output depends on beyond the sources (cmake/modules/UsdccAutogenState.cmake).\n")
    string(APPEND state "USDCC_PYTHON_FOUND=${USDCC_PYTHON_FOUND}\n")
    string(APPEND state "USDCC_PYSIDE_FOUND=${USDCC_PYSIDE_FOUND}\n")
    get_cmake_property(variables CACHE_VARIABLES)
    list(SORT variables)
    foreach(variable IN LISTS variables)
        if(variable MATCHES "^USDCC_BUILD_")
            string(APPEND state "${variable}=${${variable}}\n")
        endif()
    endforeach()

    set(file "${CMAKE_BINARY_DIR}/usdcc_autogen_state.txt")
    set(before "")
    if(EXISTS "${file}")
        file(READ "${file}" before)
    endif()
    if(state STREQUAL before)
        return()
    endif()
    file(WRITE "${file}" "${state}")
    if(before STREQUAL "")
        return()  # a new build folder (or one from before this file): nothing to redo
    endif()

    _usdcc_autogen_targets("${CMAKE_SOURCE_DIR}" targets)
    set(redone "")
    foreach(target IN LISTS targets)
        get_target_property(autogen_dir ${target} AUTOGEN_BUILD_DIR)
        if(NOT autogen_dir)
            get_target_property(binary_dir ${target} BINARY_DIR)
            set(autogen_dir "${binary_dir}/${target}_autogen")
        endif()
        file(GLOB stamps "${autogen_dir}/timestamp*")
        if(stamps)
            file(REMOVE ${stamps})
            list(APPEND redone ${target})
        endif()
    endforeach()
    list(LENGTH redone count)
    if(count GREATER 0)
        message(STATUS "AUTOMOC: the build's options or Python changed; moc runs again for ${count} targets")
    endif()
endfunction()
