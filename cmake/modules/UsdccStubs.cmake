# build/stubs: types of pxr, PySide6 and _usdcc_app for editors (docs/VSCODE.md §4), made by
# tools/build_stubs.py with OpenUSD's Python as usdcc is built. pxr's start from types-usd
# (https://github.com/LumaPictures/cg-stubs), pinned: downloaded once at configure time from
# PyPI, checked against its SHA-256, and unpacked into build/types-usd. Without it (offline, or
# with USDCC_BUNDLE_TYPES_USD off) pxr's names have no types. tools/deploy.py copies build/stubs.
#
# Sets:
#   USDCC_STUBS_DIR   build/stubs, or empty without Python

option(USDCC_BUNDLE_TYPES_USD "Download the pinned types-usd, for completion in editors" ON)

set(USDCC_TYPES_USD_VERSION "24.5.2")
set(_usdcc_types_usd_url
    "https://files.pythonhosted.org/packages/a9/38/77e716ff072350c3ae98da836e26080ffaf72e5da41c301dad55d23d8897/types_usd-24.5.2-py2.py3-none-any.whl")
set(_usdcc_types_usd_sha256 "e696c40f7aec7fa3f65f14d26345b9b747d9004616bcd9fb01c85d4bb52b3d68")
set(_usdcc_types_usd_dir "${CMAKE_BINARY_DIR}/types-usd")
set(USDCC_STUBS_DIR "")

if(USDCC_PYTHON_FOUND)
    if(USDCC_BUNDLE_TYPES_USD AND NOT EXISTS "${_usdcc_types_usd_dir}/types_usd-${USDCC_TYPES_USD_VERSION}.dist-info")
        message(STATUS "Downloading types-usd ${USDCC_TYPES_USD_VERSION}")
        file(REMOVE_RECURSE "${_usdcc_types_usd_dir}")
        set(_usdcc_types_usd_archive "${CMAKE_BINARY_DIR}/types-usd-${USDCC_TYPES_USD_VERSION}.zip")
        file(DOWNLOAD "${_usdcc_types_usd_url}" "${_usdcc_types_usd_archive}"
             EXPECTED_HASH SHA256=${_usdcc_types_usd_sha256}
             STATUS _usdcc_types_usd_status TLS_VERIFY ON)
        list(GET _usdcc_types_usd_status 0 _usdcc_types_usd_code)
        if(_usdcc_types_usd_code EQUAL 0)
            file(ARCHIVE_EXTRACT INPUT "${_usdcc_types_usd_archive}" DESTINATION "${_usdcc_types_usd_dir}")
        else()
            list(GET _usdcc_types_usd_status 1 _usdcc_types_usd_error)
            message(WARNING "Couldn't download types-usd ${USDCC_TYPES_USD_VERSION} (${_usdcc_types_usd_error}); "
                            "in editors, pxr will be completed without types.")
        endif()
        file(REMOVE "${_usdcc_types_usd_archive}")
    endif()
    set(USDCC_STUBS_DIR "${CMAKE_BINARY_DIR}/stubs")
    set(_usdcc_stubs_stamp "${CMAKE_BINARY_DIR}/stubs.stamp")
    set(_usdcc_stubs_arguments --out "${USDCC_STUBS_DIR}")
    if(USDCC_BUNDLE_TYPES_USD AND EXISTS "${_usdcc_types_usd_dir}/pxr-stubs")
        list(APPEND _usdcc_stubs_arguments --types-usd "${_usdcc_types_usd_dir}")
    endif()
    if(USDCC_PYSIDE_FOUND)
        list(APPEND _usdcc_stubs_arguments --pyside "${USDCC_PYSIDE_DIR}")
    endif()
    file(GLOB _usdcc_stubs_own CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/python/stubs/*.pyi")
    if(WIN32)
        list(APPEND _usdcc_stubs_arguments --dlls "${USD_INSTALL}/lib" "${USD_INSTALL}/bin"
             "${USD_INSTALL}/plugin/usd")  # what pxr.UsdAbc and pxr.UsdDraco link
        set(_usdcc_stubs_environment "PYTHONUTF8=1")
    elseif(APPLE)
        # The rpaths reach the libraries, and DYLD_LIBRARY_PATH would replace the system's
        # own by name (docs/MACOS.md).
        set(_usdcc_stubs_environment "PYTHONUTF8=1")
    else()
        set(_usdcc_stubs_environment "LD_LIBRARY_PATH=${USD_INSTALL}/lib:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
    endif()
    add_custom_command(
        OUTPUT "${_usdcc_stubs_stamp}"
        COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${USDCC_USD_PYTHON_DIR}" "${_usdcc_stubs_environment}"
                "${USDCC_PYTHON_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/build_stubs.py" ${_usdcc_stubs_arguments}
        COMMAND ${CMAKE_COMMAND} -E touch "${_usdcc_stubs_stamp}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tools/build_stubs.py" ${_usdcc_stubs_own} "${USD_INSTALL}/pxrConfig.cmake"
        COMMENT "Types for editors"
        VERBATIM)
    add_custom_target(usdcc_stubs ALL DEPENDS "${_usdcc_stubs_stamp}")
endif()

# For tools/deploy.py, which reads the cache.
set(USDCC_STUBS_PATH "${USDCC_STUBS_DIR}" CACHE INTERNAL "The stubs' folder, if they're built")
