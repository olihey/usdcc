# usdcc — the application executable.
add_executable(usdcc main.cpp)
if(WIN32)
    # The .exe's own icon (Explorer, taskbar pins); see resources/usdcc.rc.
    target_sources(usdcc PRIVATE resources/usdcc.rc)
endif()

# All three icon formats are generated from one source by tools/gen_app_icon.py.
if(APPLE)
    # macOS shows an app's icon from its bundle, so usdcc is built as one with
    # usdcc.icns in Contents/Resources. NOTE: not yet built or run on macOS.
    set(USDCC_MAC_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/usdcc.icns")
    target_sources(usdcc PRIVATE "${USDCC_MAC_ICON}")
    set_source_files_properties("${USDCC_MAC_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(
        usdcc
        PROPERTIES MACOSX_BUNDLE ON
                   MACOSX_BUNDLE_ICON_FILE usdcc.icns
                   MACOSX_BUNDLE_BUNDLE_NAME usdcc)
endif()

if(UNIX AND NOT APPLE)
    # Linux desktop integration: the freedesktop icon theme tree and the
    # launcher entry that refers to it. The project has no other install rules
    # yet (none for the executable itself), so this only covers these files.
    # NOTE: not yet built or run on Linux.
    install(DIRECTORY resources/linux/icons/ DESTINATION share/icons)
    install(FILES resources/linux/usdcc.desktop DESTINATION share/applications)
endif()
target_link_libraries(usdcc PRIVATE usdcc_ui usdcc_usd usdcc_tools usdcc_usd_ui)

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
    # directly, without USD's bin/lib on PATH — unlike tools/run.ps1, which
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
# plugin could be initialized." Copy it by hand alongside the executable.
if(WIN32)
    add_custom_command(
        TARGET usdcc
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:usdcc>/platforms"
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:Qt6::Core>/../Qt6/plugins/platforms/qwindows.dll"
            "$<TARGET_FILE_DIR:usdcc>/platforms/"
        COMMENT "Deploying Qt platform plugin")
endif()
