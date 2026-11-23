# Features a build may leave out (docs/BUILD_OPTIONS.md). One usdcc_feature() call each, in
# the top-level CMakeLists.txt before usdcc_core, then usdcc_write_features() once:
#
#   usdcc_feature(VIEWPORT_BROADCAST
#       LABEL "Viewport Broadcast"
#       DEFAULT ON
#       DOC "A viewport on a web page (docs/VIEWPORT_BROADCAST.md)"
#       PLUGINS org.usdcc.viewport-broadcast
#       FOLDERS viewport_broadcast
#       NEEDS SOME_OTHER_FEATURE)
#
# It defines the option USDCC_BUILD_<NAME>. Off means not there, not hidden: the caller
# doesn't add its directory, deploy.py doesn't copy its FOLDERS (from build/features.json),
# the plugin manager refuses its PLUGINS ids from any folder, and About usdcc lists it as
# left out (usdcc::core::buildFeatures(), from a source generated here into usdcc_core).
#
# NEEDS: a feature that rests on another defaults to that one's value, and asking for it
# with the other off fails the configure.

set_property(GLOBAL PROPERTY USDCC_FEATURES "")

function(usdcc_feature name)
    cmake_parse_arguments(PARSE_ARGV 1 F "" "LABEL;DEFAULT;DOC" "PLUGINS;FOLDERS;NEEDS")
    if(NOT F_LABEL)
        message(FATAL_ERROR "usdcc_feature(${name}): LABEL is required")
    endif()
    if(NOT DEFINED F_DEFAULT)
        set(F_DEFAULT ON)
    endif()
    set(_option USDCC_BUILD_${name})
    # A feature resting on one that's off defaults to off.
    set(_default ${F_DEFAULT})
    foreach(_base IN LISTS F_NEEDS)
        if(NOT USDCC_BUILD_${_base})
            set(_default OFF)
        endif()
    endforeach()
    option(${_option} "${F_DOC}" ${_default})
    foreach(_base IN LISTS F_NEEDS)
        if(${_option} AND NOT USDCC_BUILD_${_base})
            message(FATAL_ERROR "${_option}=ON needs USDCC_BUILD_${_base}=ON (docs/BUILD_OPTIONS.md)")
        endif()
    endforeach()
    set_property(GLOBAL APPEND PROPERTY USDCC_FEATURES ${name})
    set_property(GLOBAL PROPERTY USDCC_FEATURE_${name}_LABEL "${F_LABEL}")
    set_property(GLOBAL PROPERTY USDCC_FEATURE_${name}_PLUGINS "${F_PLUGINS}")
    set_property(GLOBAL PROPERTY USDCC_FEATURE_${name}_FOLDERS "${F_FOLDERS}")
endfunction()

# Writes the source usdcc_core compiles (build_features.cpp) and build/features.json, and
# sets USDCC_FEATURE_DEFINITIONS: USDCC_FEATURE_<NAME>=0/1 for every feature, which
# usdcc_core passes on to everything that links it.
function(usdcc_write_features)
    get_property(_features GLOBAL PROPERTY USDCC_FEATURES)
    set(_entries "")
    set(_json "")
    set(_definitions "")
    foreach(_name IN LISTS _features)
        get_property(_label GLOBAL PROPERTY USDCC_FEATURE_${_name}_LABEL)
        get_property(_plugins GLOBAL PROPERTY USDCC_FEATURE_${_name}_PLUGINS)
        get_property(_folders GLOBAL PROPERTY USDCC_FEATURE_${_name}_FOLDERS)
        if(USDCC_BUILD_${_name})
            set(_on true)
            list(APPEND _definitions USDCC_FEATURE_${_name}=1)
        else()
            set(_on false)
            list(APPEND _definitions USDCC_FEATURE_${_name}=0)
        endif()
        set(_ids "")
        set(_ids_json "")
        foreach(_id IN LISTS _plugins)
            string(APPEND _ids "\"${_id}\", ")
            list(APPEND _ids_json "\"${_id}\"")
        endforeach()
        set(_folders_json "")
        foreach(_folder IN LISTS _folders)
            list(APPEND _folders_json "\"${_folder}\"")
        endforeach()
        list(JOIN _ids_json ", " _ids_json)
        list(JOIN _folders_json ", " _folders_json)
        string(APPEND _entries "        {\"${_name}\", \"${_label}\", ${_on}, {${_ids}}},\n")
        list(APPEND _json "    {\"name\": \"${_name}\", \"label\": \"${_label}\", \"included\": ${_on}, \"plugins\": [${_ids_json}], \"folders\": [${_folders_json}]}")
    endforeach()
    list(JOIN _json ",\n" _json)
    set(USDCC_FEATURE_ENTRIES "${_entries}")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/build_features.cpp.in"
                   "${CMAKE_BINARY_DIR}/generated/build_features.cpp" @ONLY)
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/features.json"
         CONTENT "{\n  \"features\": [\n${_json}\n  ]\n}\n" @ONLY)
    set(USDCC_FEATURE_DEFINITIONS ${_definitions} PARENT_SCOPE)
endfunction()
