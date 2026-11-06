# usdcc's version (docs/RELEASE.md §2). Included before project(), which takes
# USDCC_VERSION_NUMBER.
#
# The release number, worked out here at configure time:
#   1. USDCC_VERSION (-DUSDCC_VERSION=0.2.0), which release.yml passes from the
#      release's tag: a release build.
#   2. Otherwise the last release tag before HEAD ("0.2.0" or "v0.2.0", git
#      describe), e.g. 0.2.0
#      for a development build after v0.2.0.
#   3. Otherwise 0.0.0.
# It's project(VERSION) and what C++ plugins are checked against
# (built_for.usdcc). A new tag takes effect at the next configure.
#
# The build's identity (commits since the tag, commit, changed files, date) is
# written at build time by usdcc_add_version_source(), so it's never stale.
#
# Sets:
#   USDCC_VERSION_NUMBER   0.2.0 (MAJOR.MINOR.PATCH)
#   USDCC_VERSION_RELEASE  0.2.0-beta.1 for a release build, empty otherwise

set(USDCC_VERSION "" CACHE STRING
    "The release's version (its tag without the v), set by release.yml; empty for a development build")

find_package(Git QUIET)

set(_usdcc_version_pattern "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z.-]+)?$")
if(USDCC_VERSION)
    if(NOT USDCC_VERSION MATCHES "${_usdcc_version_pattern}")
        message(FATAL_ERROR "USDCC_VERSION '${USDCC_VERSION}' isn't a version such as 0.2.0 or 0.2.0-beta.1.")
    endif()
    set(USDCC_VERSION_RELEASE "${USDCC_VERSION}")
    set(_usdcc_last_release "${USDCC_VERSION}")
else()
    set(USDCC_VERSION_RELEASE "")
    set(_usdcc_last_release "0.0.0")
    if(GIT_EXECUTABLE)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0 --match "v[0-9]*" --match "[0-9]*"
            WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../.."
            OUTPUT_VARIABLE _usdcc_tag
            RESULT_VARIABLE _usdcc_tag_result
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        string(REGEX REPLACE "^v" "" _usdcc_tag "${_usdcc_tag}")
        if(_usdcc_tag_result EQUAL 0 AND _usdcc_tag MATCHES "${_usdcc_version_pattern}")
            set(_usdcc_last_release "${_usdcc_tag}")
        endif()
    endif()
endif()
string(REGEX REPLACE "${_usdcc_version_pattern}" "\\1.\\2.\\3" USDCC_VERSION_NUMBER "${_usdcc_last_release}")

# Adds the generated source with usdcc::core's version functions
# (usdcc/core/version.h) to `target`, and rewrites it on every build when, and
# only when, something in it changed, so a new commit recompiles one file.
function(usdcc_add_version_source target)
    set(_source "${CMAKE_CURRENT_BINARY_DIR}/generated/version.cpp")
    set(_args
        -DOUTPUT=${_source}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -DRELEASE=${USDCC_VERSION_RELEASE}
        -DNUMBER=${USDCC_VERSION_NUMBER}
        -P "${CMAKE_SOURCE_DIR}/cmake/scripts/write_version.cmake")
    # Once now, so the file exists for the first build's dependency scan.
    execute_process(COMMAND "${CMAKE_COMMAND}" ${_args})
    add_custom_target(${target}_version
        COMMAND "${CMAKE_COMMAND}" ${_args}
        BYPRODUCTS "${_source}"
        COMMENT "Checking usdcc's version"
        VERBATIM)
    target_sources(${target} PRIVATE "${_source}")
    add_dependencies(${target} ${target}_version)
endfunction()
