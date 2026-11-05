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

# OpenUSD's pxrConfig.cmake gives TBB::tbb TBB_USE_DEBUG=1 for Debug and
# RelWithDebInfo. But OpenUSD and its TBB are release builds, and on MSVC every
# usdcc configuration uses the release CRT too (top-level CMakeLists.txt,
# docs/PLAN.md §6 item 28): there it would only turn on TBB's debug assertions
# against a release TBB and make every TBB include warn "Recommend using /MDd".
# So it's dropped there (other compilers keep it for Debug), and TBB 2020's
# "tbb/task.h is deprecated" message is silenced too.
if(TARGET TBB::tbb)
    get_target_property(_usd_tbb_defs TBB::tbb INTERFACE_COMPILE_DEFINITIONS)
    if(NOT _usd_tbb_defs)
        set(_usd_tbb_defs "")
    endif()
    list(FILTER _usd_tbb_defs EXCLUDE REGEX "TBB_USE_DEBUG")
    list(APPEND _usd_tbb_defs
        "$<$<AND:$<CONFIG:Debug>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:TBB_USE_DEBUG=1>"
        TBB_SUPPRESS_DEPRECATED_MESSAGES=1)
    set_target_properties(TBB::tbb PROPERTIES INTERFACE_COMPILE_DEFINITIONS "${_usd_tbb_defs}")
    unset(_usd_tbb_defs)
endif()

# hd/dataSource.h uses the std::atomic_*() shared_ptr overloads that C++20
# deprecates (MSVC warning STL4029) in every file that includes it.
if(MSVC AND TARGET hd)
    set_property(TARGET hd APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
        _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    USD
    REQUIRED_VARS PXR_INCLUDE_DIRS PXR_LIBRARIES
    VERSION_VAR PXR_VERSION
    REASON_FAILURE_MESSAGE
        "OpenUSD was not found. Pass -DUSD_INSTALL=<path> pointing at an OpenUSD install/build directory that contains pxrConfig.cmake."
)

mark_as_advanced(USD_INSTALL)
