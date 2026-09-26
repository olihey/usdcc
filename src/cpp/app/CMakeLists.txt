# usdcc — the application executable.
add_executable(usdcc main.cpp)
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
            "$<TARGET_FILE_DIR:Qt6::Core>/../Qt6/plugins/platforms/$<IF:$<CONFIG:Debug>,qwindowsd.dll,qwindows.dll>"
            "$<TARGET_FILE_DIR:usdcc>/platforms/"
        COMMENT "Deploying Qt platform plugin")
endif()
