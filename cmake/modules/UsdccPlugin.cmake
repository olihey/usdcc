#[[
UsdccPlugin
-----------

usdcc_add_plugin() builds a C++ plugin (docs/PLUGIN_SDK.md): a module library
in a plugin folder, with a manifest saying what it was built for, which the app
compares before loading it (docs/PLUGINS.md section 3.2).

  usdcc_add_plugin(my_target
      ID com.example.notes
      NAME "Notes"
      VERSION 1.0.0
      SOURCES notes.cpp
      [DESCRIPTION "..."]
      [OUTPUT_DIR folder]        # default: <build>/plugins/<target>
      [PYTHON_MODULE notes]      # copied from python/ by you; named in the manifest
      [USD_FOLDERS usd]          # likewise
      [REQUIRES other.plugin.id ...])

Needs the variables USDCC_BUILT_FOR_{PLATFORM,COMPILER,QT,USD,USDCC} and the
target usdcc::plugin_sdk: the app's build sets them, and so does
find_package(UsdccPlugin) outside it.
]]

function(usdcc_add_plugin target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "ID;NAME;VERSION;DESCRIPTION;OUTPUT_DIR;PYTHON_MODULE"
                          "SOURCES;USD_FOLDERS;REQUIRES")
    foreach(_required ID NAME VERSION SOURCES)
        if(NOT ARG_${_required})
            message(FATAL_ERROR "usdcc_add_plugin(${target}) needs ${_required}")
        endif()
    endforeach()
    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR "${CMAKE_BINARY_DIR}/plugins/${target}")
    endif()

    # The app has Qt and the rest: vcpkg needn't copy them next to the plugin.
    set(VCPKG_APPLOCAL_DEPS OFF)
    add_library(${target} MODULE ${ARG_SOURCES})
    target_link_libraries(${target} PRIVATE usdcc::plugin_sdk)
    # USD's headers ask for the release Python import library in every
    # configuration, as for the app (src/cpp/app/CMakeLists.txt).
    if(Python3_LIBRARY AND EXISTS "${Python3_LIBRARY}")
        target_link_libraries(${target} PRIVATE "${Python3_LIBRARY}")
    endif()
    # A generator expression, so that multi-config generators don't add a folder.
    set(_bin "${ARG_OUTPUT_DIR}/bin/${USDCC_BUILT_FOR_PLATFORM}")
    set_target_properties(
        ${target}
        PROPERTIES PREFIX ""
                   AUTOMOC ON
                   LIBRARY_OUTPUT_DIRECTORY "$<1:${_bin}>"
                   RUNTIME_OUTPUT_DIRECTORY "$<1:${_bin}>")

    set(_json "{\n  \"id\": \"${ARG_ID}\",\n  \"name\": \"${ARG_NAME}\",\n  \"version\": \"${ARG_VERSION}\",\n")
    if(ARG_DESCRIPTION)
        string(APPEND _json "  \"description\": \"${ARG_DESCRIPTION}\",\n")
    endif()
    string(APPEND _json "  \"api\": 1,\n")
    if(ARG_PYTHON_MODULE)
        string(APPEND _json "  \"python\": { \"module\": \"${ARG_PYTHON_MODULE}\" },\n")
    endif()
    if(ARG_USD_FOLDERS)
        list(JOIN ARG_USD_FOLDERS "\", \"" _folders)
        string(APPEND _json "  \"usd\": [\"${_folders}\"],\n")
    endif()
    if(ARG_REQUIRES)
        list(JOIN ARG_REQUIRES "\", \"" _requires)
        string(APPEND _json "  \"requires\": [\"${_requires}\"],\n")
    endif()
    string(APPEND _json
           "  \"native\": {\n    \"library\": \"${target}\",\n    \"built_for\": {\n"
           "      \"platform\": \"${USDCC_BUILT_FOR_PLATFORM}\",\n"
           "      \"compiler\": \"${USDCC_BUILT_FOR_COMPILER}\",\n"
           "      \"qt\": \"${USDCC_BUILT_FOR_QT}\",\n"
           "      \"usd\": \"${USDCC_BUILT_FOR_USD}\",\n"
           "      \"usdcc\": \"${USDCC_BUILT_FOR_USDCC}\"\n"
           "    }\n  }\n}\n")
    file(WRITE "${ARG_OUTPUT_DIR}/usdcc-plugin.json" "${_json}")
endfunction()
