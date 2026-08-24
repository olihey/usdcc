# usdcc — the application executable.
add_executable(usdcc main.cpp)
target_link_libraries(usdcc PRIVATE usdcc_ui usdcc_usd usdcc_tools)

set_target_properties(usdcc PROPERTIES WIN32_EXECUTABLE ON)

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
