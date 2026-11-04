# What a C++ plugin builds against (docs/PLUGIN_SDK.md): the header, and through it
# the app's libraries, which are DLLs so that a plugin shares them (docs/PLUGINS.md §3.2).
add_library(usdcc_plugin_sdk INTERFACE)
add_library(usdcc::plugin_sdk ALIAS usdcc_plugin_sdk)
target_include_directories(usdcc_plugin_sdk INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(usdcc_plugin_sdk INTERFACE usdcc_core usdcc_usd usdcc_tools usdcc_ui usdcc_usd_ui)

# For find_package(UsdccPlugin) outside this build; tools/deploy.ps1 puts it in sdk/.
configure_file("${CMAKE_SOURCE_DIR}/cmake/sdk/UsdccPluginConfig.cmake.in" "${CMAKE_BINARY_DIR}/sdk/UsdccPluginConfig.cmake" @ONLY)
