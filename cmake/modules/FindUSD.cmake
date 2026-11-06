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

# pxrConfig.cmake names the Python OpenUSD was built with. In the prebuilt
# OpenUSD from CI (docs/CI.md) it only uses that one if it exists, which on
# another machine it doesn't, and then its find_package(Python3) would get
# vcpkg's python3: vcpkg's find_package wrapper insists on it whenever
# Development is asked for, and it can't be embedded, so scripting would be
# off. So find this machine's own Python of the same version first, where
# Python's installer registers it or on PATH, and set it as pxrConfig would.
if(WIN32 AND NOT DEFINED Python3_EXECUTABLE AND EXISTS "${USD_INSTALL}/pxrConfig.cmake")
    file(READ "${USD_INSTALL}/pxrConfig.cmake" _usd_config)
    set(_usd_python_built "")
    if(_usd_config MATCHES "set\\(Python3_EXECUTABLE \\[\\[([^]]*)\\]\\]\\)")
        set(_usd_python_built "${CMAKE_MATCH_1}")
    endif()
    if(NOT EXISTS "${_usd_python_built}"
       AND _usd_config MATCHES "find_dependency\\(Python3 \"([0-9]+)\\.([0-9]+)\"")
        set(_usd_python_version "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
        set(_usd_python_candidates "")
        foreach(_root HKEY_CURRENT_USER HKEY_LOCAL_MACHINE)
            get_filename_component(_dir "[${_root}\\SOFTWARE\\Python\\PythonCore\\${_usd_python_version}\\InstallPath]"
                                   ABSOLUTE)
            list(APPEND _usd_python_candidates "${_dir}/python.exe")
        endforeach()
        string(REPLACE "\\" "/" _usd_path "$ENV{PATH}")
        foreach(_dir IN LISTS _usd_path)
            list(APPEND _usd_python_candidates "${_dir}/python.exe")
        endforeach()
        foreach(_python IN LISTS _usd_python_candidates)
            if(NOT EXISTS "${_python}" OR _python MATCHES "/vcpkg_installed/|/WindowsApps/")
                continue()
            endif()
            execute_process(
                COMMAND "${_python}" -c "import sys; print('%d.%d' % sys.version_info[:2]); print(sys.base_prefix)"
                OUTPUT_VARIABLE _probe RESULT_VARIABLE _result ERROR_QUIET)
            string(STRIP "${_probe}" _probe)
            string(REPLACE "\n" ";" _probe "${_probe}")
            list(LENGTH _probe _probe_length)
            if(NOT _result EQUAL 0 OR NOT _probe_length EQUAL 2)
                continue()
            endif()
            list(GET _probe 0 _version)
            list(GET _probe 1 _home)
            file(TO_CMAKE_PATH "${_home}" _home)
            string(REPLACE "." "" _nodot "${_version}")
            if(_version STREQUAL _usd_python_version AND EXISTS "${_home}/libs/python${_nodot}.lib")
                set(Python3_EXECUTABLE "${_home}/python.exe" CACHE FILEPATH "OpenUSD's Python")
                set(Python3_LIBRARY "${_home}/libs/python${_nodot}.lib" CACHE FILEPATH "OpenUSD's Python")
                set(Python3_INCLUDE_DIR "${_home}/Include" CACHE PATH "OpenUSD's Python")
                message(STATUS "OpenUSD's Python ${_usd_python_version}: ${_home} (built with ${_usd_python_built})")
                break()
            endif()
        endforeach()
        if(NOT DEFINED Python3_EXECUTABLE)
            message(WARNING "OpenUSD was built with Python ${_usd_python_version} (${_usd_python_built}), which isn't "
                            "on this machine, and no other Python ${_usd_python_version} was found. Install it, or "
                            "pass -DPython3_EXECUTABLE=<its python.exe>; without it scripting is off.")
        endif()
    endif()
    unset(_usd_config)
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
