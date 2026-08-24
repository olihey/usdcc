#[[
FindUSD
-------

Locates an OpenUSD build via its own CMake package config (pxrConfig.cmake),
so usdcc can be pointed at any custom OpenUSD build rather than a version
pinned by vcpkg.

Hints:
  USD_INSTALL   - path to the OpenUSD install/build directory that contains
                  pxrConfig.cmake (CMake cache variable or environment
                  variable).

Result variables (as produced by OpenUSD's own pxrConfig.cmake):
  PXR_FOUND
  PXR_INCLUDE_DIRS
  PXR_LIBRARIES
  PXR_VERSION
]]

if(NOT USD_INSTALL AND DEFINED ENV{USD_INSTALL})
    set(USD_INSTALL "$ENV{USD_INSTALL}")
endif()

set(USD_INSTALL
    "${USD_INSTALL}"
    CACHE PATH "Path to an OpenUSD install/build directory (containing pxrConfig.cmake)")

if(USD_INSTALL)
    list(APPEND CMAKE_PREFIX_PATH "${USD_INSTALL}")
endif()

find_package(pxr CONFIG QUIET)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    USD
    REQUIRED_VARS PXR_INCLUDE_DIRS PXR_LIBRARIES
    VERSION_VAR PXR_VERSION
    REASON_FAILURE_MESSAGE
        "OpenUSD was not found. Pass -DUSD_INSTALL=<path> pointing at an OpenUSD install/build directory that contains pxrConfig.cmake."
)

mark_as_advanced(USD_INSTALL)
