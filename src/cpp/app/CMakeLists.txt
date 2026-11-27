# What usdcc and usdcc_node both do to start (docs/RENDER_NODE.md §3.1): logging, the
# profile, settings, USD's environment, the plugins' manifests, the renderers, the peers.
add_library(usdcc_app_core STATIC startup.cpp startup.h)
target_include_directories(usdcc_app_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(usdcc_app_core PUBLIC usdcc_core usdcc_usd usdcc_render usdcc_ui usdcc_usd_ui usdcc_peers
                                            usdcc_scripting_app Qt6::Gui)
if(Python3_LIBRARY AND EXISTS "${Python3_LIBRARY}")
    target_link_libraries(usdcc_app_core PUBLIC "${Python3_LIBRARY}")
endif()

# Send to… (docs/PEERS.md §10.3): File > Send To and what each kind received does. A library of
# its own so that usdcc_python_tests runs the app's own receivers.
add_library(usdcc_send_to STATIC peers_send.cpp peers_send.h)
target_include_directories(usdcc_send_to PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(usdcc_send_to PUBLIC usdcc_ui usdcc_usd_ui usdcc_render usdcc_peers usdcc_scripting_app
                                           usdcc_scripting_ui)

# usdcc — the application executable.
add_executable(usdcc main.cpp file_open_events.h)
# Updates (docs/UPDATES.md): GitHub's releases with Qt Network (its DLL and TLS plugin are
# deployed below), and for an installed usdcc Velopack: on Windows its DLL, beside the exe. A build
# without the Updates feature (docs/BUILD_OPTIONS.md) has none of it.
if(USDCC_BUILD_UPDATES)
    target_sources(usdcc PRIVATE app_updates.cpp app_updates.h release_check.cpp release_check.h running_programs.h)
    target_link_libraries(usdcc PRIVATE Qt6::Network)
endif()
# Packs (docs/USD_PLUGINS.md section 10): downloaded with Qt Network too, from the releases.
if(USDCC_BUILD_PACKS)
    target_sources(usdcc PRIVATE app_packs.cpp app_packs.h pack_index.cpp pack_index.h pack_installer.cpp pack_installer.h
                                 system_proxy.h)
    target_link_libraries(usdcc PRIVATE Qt6::Network)
endif()
if(USDCC_BUILD_UPDATES AND USDCC_VELOPACK_FOUND)
    target_link_libraries(usdcc PRIVATE usdcc::velopack)
    if(WIN32)
        add_custom_command(
            TARGET usdcc
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${USDCC_VELOPACK_DLL}" "$<TARGET_FILE_DIR:usdcc>/"
            COMMENT "Deploying Velopack's DLL"
            VERBATIM)
    endif()
endif()
if(WIN32)
    # The .exe's own icon (Explorer, taskbar pins) and version (Properties >
    # Details); see resources/usdcc.rc. The version, from the configure-time
    # release number, in a header the resource script includes.
    string(REPLACE "." "," _usdcc_rc_version "${USDCC_VERSION_NUMBER},0")
    if(USDCC_VERSION_RELEASE)
        set(_usdcc_rc_string "${USDCC_VERSION_RELEASE}")
        set(_usdcc_rc_flags "0")
    else()
        set(_usdcc_rc_string "${USDCC_VERSION_NUMBER} (development build)")
        set(_usdcc_rc_flags "VS_FF_PRERELEASE")
    endif()
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/usdcc_version.h" CONTENT
         "#define USDCC_RC_VERSION ${_usdcc_rc_version}\n#define USDCC_RC_VERSION_STRING \"${_usdcc_rc_string}\"\n#define USDCC_RC_FLAGS ${_usdcc_rc_flags}\n")
    target_sources(usdcc PRIVATE resources/usdcc.rc)
    set_property(SOURCE resources/usdcc.rc APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/generated")
endif()

# All three icon formats are generated from one source by tools/gen_app_icon.py.
if(APPLE)
    # macOS shows an app's icon from its bundle, so usdcc is built as one with
    # usdcc.icns in Contents/Resources (docs/MACOS.md).
    set(USDCC_MAC_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/usdcc.icns")
    target_sources(usdcc PRIVATE "${USDCC_MAC_ICON}")
    set_source_files_properties("${USDCC_MAC_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(
        usdcc
        PROPERTIES MACOSX_BUNDLE ON
                   MACOSX_BUNDLE_ICON_FILE usdcc.icns
                   MACOSX_BUNDLE_BUNDLE_NAME usdcc
                   MACOSX_BUNDLE_GUI_IDENTIFIER org.usdcc.usdcc
                   MACOSX_BUNDLE_SHORT_VERSION_STRING "${USDCC_VERSION_NUMBER}"
                   MACOSX_BUNDLE_BUNDLE_VERSION "${USDCC_VERSION_NUMBER}"
                   MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/Info.plist.in")
    set(USDCC_MACOS_MINIMUM "${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

if(UNIX AND NOT APPLE)
    # Linux desktop integration: the freedesktop icon theme tree and the
    # launcher entry that refers to it. The project has no other install rules
    # yet (none for the executable itself), so this only covers these files.
    # NOTE: usdcc builds and runs on Linux (docs/LINUX_WSL.md); these install
    # rules haven't been tried there.
    install(DIRECTORY resources/linux/icons/ DESTINATION share/icons)
    install(FILES resources/linux/usdcc.desktop DESTINATION share/applications)
endif()
# For Help > About usdcc.
target_compile_definitions(usdcc PRIVATE USDCC_COMPILER="${USDCC_BUILT_FOR_COMPILER}")
target_link_libraries(usdcc PRIVATE usdcc_send_to usdcc_app_core usdcc_ui usdcc_usd usdcc_tools usdcc_render usdcc_usd_ui usdcc_scripting usdcc_scripting_app usdcc_scripting_ui)
# usdcc_peers for the plugins that use it (docs/PEERS.md); the app itself doesn't link it yet.
usdcc_deploy_core_library(usdcc usdcc_usd usdcc_tools usdcc_ui usdcc_render usdcc_usd_ui usdcc_peers)

# USD's own headers wrap their Python.h include the same way pybind11 does
# (see src/cpp/usd/python/CMakeLists.txt's comment on usdcc_usd_python) to
# avoid ever requiring a debug-suffixed Python import library — but they do
# it unconditionally, regardless of the *consumer's* own build config, so
# every USD-header-including object file (confirmed via `dumpbin /directives`
# across usdcc_tools/usdcc_usd/usdcc_usd_ui's Debug objects) embeds a plain
# `#pragma comment(lib,"python312.lib")`, requesting the system Python's
# release import library even when usdcc itself is built Debug. USD's own
# lib dirs (the only ones on this target's search path) don't contain that
# file, so a Debug build failed with "LNK1104: cannot open file
# 'python312.lib'". Explicitly linking the same Python3_LIBRARY that
# find_package(USD) already resolved (via pxrConfig.cmake, to whatever
# system interpreter USD itself was built against) satisfies that request
# directly, for every config — exactly what usdcc_usd_python already does
# for its own, separately-discovered system Python.
if(Python3_LIBRARY AND EXISTS "${Python3_LIBRARY}")
    target_link_libraries(usdcc PRIVATE "${Python3_LIBRARY}")
endif()

set_target_properties(usdcc PROPERTIES WIN32_EXECUTABLE ON)

if(WIN32)
    # Visual Studio's own debugger (Start Debugging / F5) launches usdcc.exe
    # directly, without USD's bin/lib on PATH — unlike tools/run.py, which
    # sets this by hand before launching — so Storm/Hydra fails to load with
    # "usd_usdImagingGL.dll was not found" the moment a stage is opened.
    # VS_DEBUGGER_ENVIRONMENT becomes <LocalDebuggerEnvironment> in the
    # generated .vcxproj, which Visual Studio's debugger reads directly, so
    # this only needs setting once here rather than by every developer by
    # hand in the IDE. Entries are newline-separated (not semicolon —
    # semicolons only separate multiple paths within PATH itself).
    set_target_properties(
        usdcc
        PROPERTIES
            VS_DEBUGGER_ENVIRONMENT
            "PATH=${USD_INSTALL}/bin;${USD_INSTALL}/lib;%PATH%\nPXR_PLUGINPATH_NAME=${USD_INSTALL}/plugin/usd")
endif()

# The Debug configuration links the *Release* Qt/ADS builds (see the top-level
# CMakeLists.txt: everything must share USD's release CRT), but vcpkg's
# applocal step deploys from debug/bin for Debug and so finds nothing to copy.
# Run the same step against the release bin for Debug. (Other configurations
# get a no-op; vcpkg's own step already does their job.)
if(WIN32 AND USDCC_LINK_RELEASE_QT AND Z_VCPKG_EXECUTABLE)
    set(_usdcc_release_bin "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    add_custom_command(
        TARGET usdcc
        POST_BUILD
        COMMAND
            $<IF:$<CONFIG:Debug>,${Z_VCPKG_EXECUTABLE},${CMAKE_COMMAND}>
            $<IF:$<CONFIG:Debug>,z-applocal$<SEMICOLON>--target-binary=$<TARGET_FILE:usdcc>$<SEMICOLON>--installed-bin-dir=${_usdcc_release_bin},-E$<SEMICOLON>true>
        COMMAND_EXPAND_LISTS
        VERBATIM)
endif()

# Qt is a shared (non-static) vcpkg build: vcpkg's applocal-deps step copies
# the DLLs usdcc links against directly, but not the platform plugin, which
# Qt loads dynamically at startup and which vcpkg doesn't ship a windeployqt
# binary to deploy. Without it the app fails to start with "no Qt platform
# plugin could be initialized." Copy it by hand alongside the executable, and
# the offscreen one with it: a scripted run with QT_QPA_PLATFORM=offscreen would
# otherwise stop at that same dialog.
if(WIN32)
    add_custom_command(
        TARGET usdcc
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:usdcc>/platforms"
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:Qt6::Core>/../Qt6/plugins/platforms/qwindows.dll"
            "$<TARGET_FILE_DIR:Qt6::Core>/../Qt6/plugins/platforms/qoffscreen.dll"
            "$<TARGET_FILE_DIR:usdcc>/platforms/"
        COMMENT "Deploying Qt platform plugins")
    # What plugins built on usdcc_peers (docs/PEERS.md §13) load, which usdcc itself
    # doesn't link, so vcpkg's step above doesn't know of it. Beside usdcc.exe, not a
    # plugin's folder: plugins are loaded with LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, which
    # doesn't search there for what they link, nor PATH (docs/VIEWPORT_BROADCAST.md §8):
    # - Qt Network and WebSockets;
    # - OpenSSL's libcrypto, which Qt Network as built here links at load time;
    # - Qt's JPEG plugin and libjpeg, without which QImageWriter writes no JPEG;
    # - Qt's TLS plugin, Schannel (Windows' own, so no second OpenSSL beside PySide6's):
    #   without one Qt Network warns "No functional TLS backend was found", and wss://
    #   to a relay (docs/PEERS.md §13) can't work.
    set(_usdcc_qt_bin "$<TARGET_FILE_DIR:Qt6::Core>")
    # OpenSSL's DLL names the architecture: libcrypto-3-x64.dll, libcrypto-3-arm64.dll.
    if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
        set(_usdcc_libcrypto "libcrypto-3-arm64.dll")
    else()
        set(_usdcc_libcrypto "libcrypto-3-x64.dll")
    endif()
    add_custom_command(
        TARGET usdcc
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::Network>" "$<TARGET_FILE:Qt6::WebSockets>"
            "${_usdcc_qt_bin}/${_usdcc_libcrypto}" "${_usdcc_qt_bin}/jpeg62.dll"
            "$<TARGET_FILE_DIR:usdcc>/"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:usdcc>/imageformats"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_usdcc_qt_bin}/../Qt6/plugins/imageformats/qjpeg.dll" "$<TARGET_FILE_DIR:usdcc>/imageformats/"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:usdcc>/tls"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_usdcc_qt_bin}/../Qt6/plugins/tls/qschannelbackend.dll" "$<TARGET_FILE_DIR:usdcc>/tls/"
        COMMENT "Deploying Qt Network, WebSockets, the JPEG plugin and the TLS plugin"
        VERBATIM)
endif()

# OpenUSD's Python DLL next to the exe, replacing the one vcpkg's deploy step
# copies there (docs/SCRIPTING.md §3.1). Added last, so it runs after vcpkg's
# own POST_BUILD step.
usdcc_deploy_python(usdcc)
