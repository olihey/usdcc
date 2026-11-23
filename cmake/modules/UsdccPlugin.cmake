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

#[[
usdcc_add_asset_protocol() builds a protocol of remote assets
(docs/REMOTE_ASSETS.md section 11): a usdcc plugin with only a usd/ part, holding
an OpenUSD resolver library built on usdcc_assets and its plugInfo.json.

  usdcc_add_asset_protocol(my_target
      ID org.usdcc.webdav
      NAME "WebDAV Assets"
      VERSION 1.0.0
      RESOLVER UsdccWebDavResolver   # the ArResolver class, as AR_DEFINE_RESOLVER names it
      SCHEMES webdav
      SOURCES webdav_protocol.cpp
      [DESCRIPTION "..."]
      [OUTPUT_DIR folder])           # default: <build>/plugins/<target>

Outside usdcc, PXR_PLUGINPATH_NAME=<folder>/usd makes the protocol known to any
OpenUSD program that finds usdcc_assets on its library path.
]]
function(usdcc_add_asset_protocol target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "ID;NAME;VERSION;DESCRIPTION;RESOLVER;OUTPUT_DIR" "SOURCES;SCHEMES")
    foreach(_required ID NAME VERSION RESOLVER SCHEMES SOURCES)
        if(NOT ARG_${_required})
            message(FATAL_ERROR "usdcc_add_asset_protocol(${target}) needs ${_required}")
        endif()
    endforeach()
    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR "${CMAKE_BINARY_DIR}/plugins/${target}")
    endif()

    add_library(${target} SHARED ${ARG_SOURCES})
    # In the app's build its target; against a deployed app's SDK, which has its import library.
    if(TARGET usdcc_assets)
        target_link_libraries(${target} PRIVATE usdcc_assets)
    else()
        target_link_libraries(${target} PRIVATE usdcc::plugin_sdk)
    endif()
    # TF_REGISTRY_FUNCTION names the library it's in through these.
    target_compile_definitions(${target} PRIVATE MFB_PACKAGE_NAME=${target} MFB_ALT_PACKAGE_NAME=${target})
    # USD's headers ask for the release Python import library in every configuration.
    if(Python3_LIBRARY AND EXISTS "${Python3_LIBRARY}")
        target_link_libraries(${target} PRIVATE "${Python3_LIBRARY}")
    endif()
    # A generator expression, so that multi-config generators don't add a folder.
    set_target_properties(
        ${target}
        PROPERTIES PREFIX ""
                   RUNTIME_OUTPUT_DIRECTORY "$<1:${ARG_OUTPUT_DIR}/usd>"
                   LIBRARY_OUTPUT_DIRECTORY "$<1:${ARG_OUTPUT_DIR}/usd>")

    list(JOIN ARG_SCHEMES "\", \"" _schemes)
    # implementsScopedCaches: without it OpenUSD never tells a URI resolver about a cache
    # scope, and every resolve of a composition asks the server again.
    file(GENERATE OUTPUT "${ARG_OUTPUT_DIR}/usd/plugInfo.json" CONTENT
"{
    \"Plugins\": [{
        \"Type\": \"library\",
        \"Name\": \"${target}\",
        \"Root\": \".\",
        \"LibraryPath\": \"$<TARGET_FILE_NAME:${target}>\",
        \"ResourcePath\": \".\",
        \"Info\": { \"Types\": { \"${ARG_RESOLVER}\": {
            \"bases\": [\"ArResolver\"], \"uriSchemes\": [\"${_schemes}\"],
            \"implementsContexts\": true, \"implementsScopedCaches\": true,
            \"usdccAssetsProtocol\": true
        } } }
    }]
}
")
    set(_json "{\n  \"id\": \"${ARG_ID}\",\n  \"name\": \"${ARG_NAME}\",\n  \"version\": \"${ARG_VERSION}\",\n")
    if(ARG_DESCRIPTION)
        string(APPEND _json "  \"description\": \"${ARG_DESCRIPTION}\",\n")
    endif()
    string(APPEND _json "  \"api\": 1,\n  \"usd\": [\"usd\"]\n}\n")
    file(WRITE "${ARG_OUTPUT_DIR}/usdcc-plugin.json" "${_json}")

    if(TARGET usdcc)
        # Where PluginManager::shippedFolder() looks, so a build loads it as a deployed
        # app does. The folder's name is the output folder's.
        get_filename_component(_name "${ARG_OUTPUT_DIR}" NAME)
        add_custom_target(${target}_app_plugin
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${ARG_OUTPUT_DIR}" "${USDCC_APP_INSTALL_ROOT}/plugins/${_name}"
            DEPENDS ${target} VERBATIM)
        add_dependencies(usdcc ${target}_app_plugin)
    endif()
endfunction()

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
    if(MSVC AND CMAKE_CXX_FLAGS MATCHES "/EHsc")
        # The entry points are extern "C", and may throw: the app catches what
        # usdcc_plugin_activate() throws and fails the plugin with its message
        # (docs/PLUGIN_SDK.md). MSVC's default /EHsc assumes an extern "C"
        # function never throws (warning C4297 where one does, and undefined
        # behaviour); /EHc- drops that assumption for the plugin's code. usdcc's
        # own build uses /EHs throughout; a project of its own can do the same
        # (replacing /EHsc in CMAKE_CXX_FLAGS) to avoid cl's D9025 note here.
        target_compile_options(${target} PRIVATE /EHc-)
    endif()
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
    if(NOT WIN32)
        # A plugin's own libraries are found next to it, as Windows does for DLLs
        # (docs/PLUGIN_SDK.md): $ORIGIN on the rpath, built and installed, which
        # macOS spells @loader_path.
        if(APPLE)
            set(_origin "@loader_path")
        else()
            set(_origin "$ORIGIN")
        endif()
        set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "${_origin}")
        set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "${_origin}")
    endif()

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
