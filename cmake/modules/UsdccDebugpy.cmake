# debugpy (https://github.com/microsoft/debugpy), which lets VS Code's debugger attach to the
# Python in usdcc (docs/VSCODE.md §5), bundled at a pinned version so debugging works with
# nothing installed. Downloaded once at configure time from PyPI's wheel for this platform,
# checked against its SHA-256, and unpacked into build/debugpy (the debugpy package, and its
# MIT LICENSE and ThirdPartyNotices.txt); tools/deploy.py copies that into the deploy's
# tools/debugpy. Offline, or with USDCC_BUNDLE_DEBUGPY off, usdcc.debug needs a debugpy among
# the Python packages.
#
# The wheels with compiled parts are for Python 3.13 on x64; elsewhere it's the pure Python
# one, which is slower at breakpoints.
#
# To upgrade: pick a release at least two weeks old, and take the cp313 win_amd64 and
# manylinux x86_64 wheels' and the py2.py3-none-any one's URLs and sha256s from
# https://pypi.org/pypi/debugpy/<version>/json.
#
# Sets:
#   USDCC_DEBUGPY_DIR   build/debugpy, or empty when it isn't there

option(USDCC_BUNDLE_DEBUGPY "Download the pinned debugpy, for debugging Python in usdcc" ON)

set(USDCC_DEBUGPY_VERSION "1.8.22")
set(USDCC_DEBUGPY_URL
    "https://files.pythonhosted.org/packages/56/3d/c7dc9f35bc9e22cdb53679f844bee40fd5cff871862f60595869a433400d/debugpy-1.8.22-py2.py3-none-any.whl")
set(USDCC_DEBUGPY_SHA256 "a9e9d3550e15ca479c59333e90845029190531f0cacfedab3b815a57bd913947")
if(USDCC_PYTHON_VERSION STREQUAL "3.13")
    if(WIN32 AND NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
        set(USDCC_DEBUGPY_URL
            "https://files.pythonhosted.org/packages/d0/f5/c2b3e8260388d7b76bc4e5a3a29648a0ca47dca6a2721d6549ff2d944eee/debugpy-1.8.22-cp313-cp313-win_amd64.whl")
        set(USDCC_DEBUGPY_SHA256 "f49b1d6cecf326b63c06d7f517e4a6642777f758c58cb799e7bc5a6dd74721c2")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        set(USDCC_DEBUGPY_URL
            "https://files.pythonhosted.org/packages/bc/4f/8df91270afbf61fc5ab202951f28451194a7eb093704b26da11cb0ecd308/debugpy-1.8.22-cp313-cp313-manylinux_2_34_x86_64.whl")
        set(USDCC_DEBUGPY_SHA256 "c1ffb9953708b648ecf6acd2e5ad2c002bf68785ed618197dd4463a2a7226f39")
    endif()
endif()

set(_usdcc_debugpy_dir "${CMAKE_BINARY_DIR}/debugpy")
set(_usdcc_debugpy_stamp "${_usdcc_debugpy_dir}/VERSION")
set(USDCC_DEBUGPY_DIR "")

if(USDCC_BUNDLE_DEBUGPY)
    # The hash too, as it's another wheel for another Python.
    set(_usdcc_debugpy_want "${USDCC_DEBUGPY_VERSION} ${USDCC_DEBUGPY_SHA256}")
    set(_usdcc_debugpy_have "")
    if(EXISTS "${_usdcc_debugpy_stamp}")
        file(READ "${_usdcc_debugpy_stamp}" _usdcc_debugpy_have)
        string(STRIP "${_usdcc_debugpy_have}" _usdcc_debugpy_have)
    endif()
    if(NOT _usdcc_debugpy_have STREQUAL _usdcc_debugpy_want OR NOT EXISTS "${_usdcc_debugpy_dir}/debugpy/__init__.py")
        message(STATUS "Downloading debugpy ${USDCC_DEBUGPY_VERSION}")
        file(REMOVE_RECURSE "${_usdcc_debugpy_dir}")
        # A wheel is a zip; the .zip name makes that plain to the extractor.
        set(_usdcc_debugpy_archive "${CMAKE_BINARY_DIR}/debugpy-${USDCC_DEBUGPY_VERSION}.zip")
        file(DOWNLOAD "${USDCC_DEBUGPY_URL}" "${_usdcc_debugpy_archive}"
             EXPECTED_HASH SHA256=${USDCC_DEBUGPY_SHA256}
             STATUS _usdcc_debugpy_status TLS_VERIFY ON)
        list(GET _usdcc_debugpy_status 0 _usdcc_debugpy_code)
        if(_usdcc_debugpy_code EQUAL 0)
            file(ARCHIVE_EXTRACT INPUT "${_usdcc_debugpy_archive}" DESTINATION "${_usdcc_debugpy_dir}")
            # What's only for building it, and for attaching to a process from outside (other
            # programs' processes): 20 of its 28 MB. The attach library of this platform stays,
            # as pydevd loads it to trace threads that are there already.
            file(GLOB_RECURSE _usdcc_debugpy_extra "${_usdcc_debugpy_dir}/*.pdb" "${_usdcc_debugpy_dir}/*.c"
                 "${_usdcc_debugpy_dir}/*.cpp" "${_usdcc_debugpy_dir}/*.h" "${_usdcc_debugpy_dir}/*.hpp"
                 "${_usdcc_debugpy_dir}/*.pyx" "${_usdcc_debugpy_dir}/*.pxd" "${_usdcc_debugpy_dir}/*.exe")
            file(GLOB _usdcc_debugpy_attach "${_usdcc_debugpy_dir}/debugpy/_vendored/pydevd/pydevd_attach_to_process/*.dll"
                 "${_usdcc_debugpy_dir}/debugpy/_vendored/pydevd/pydevd_attach_to_process/*.so"
                 "${_usdcc_debugpy_dir}/debugpy/_vendored/pydevd/pydevd_attach_to_process/*.dylib")
            set(_usdcc_debugpy_keep "attach[.]dylib$")
            if(WIN32)
                set(_usdcc_debugpy_keep "/attach_amd64[.]dll$")
            elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                set(_usdcc_debugpy_keep "/attach_linux_amd64[.]so$")
            endif()
            list(FILTER _usdcc_debugpy_attach EXCLUDE REGEX "${_usdcc_debugpy_keep}")
            foreach(_file IN LISTS _usdcc_debugpy_extra _usdcc_debugpy_attach)
                file(REMOVE "${_file}")
            endforeach()
            set(_usdcc_debugpy_info "${_usdcc_debugpy_dir}/debugpy-${USDCC_DEBUGPY_VERSION}.dist-info")
            file(COPY "${_usdcc_debugpy_info}/licenses/LICENSE" DESTINATION "${_usdcc_debugpy_dir}")
            file(COPY "${_usdcc_debugpy_dir}/debugpy/ThirdPartyNotices.txt" DESTINATION "${_usdcc_debugpy_dir}")
            file(REMOVE_RECURSE "${_usdcc_debugpy_info}")
            file(WRITE "${_usdcc_debugpy_stamp}" "${_usdcc_debugpy_want}\n")
        else()
            list(GET _usdcc_debugpy_status 1 _usdcc_debugpy_error)
            message(WARNING "Couldn't download debugpy ${USDCC_DEBUGPY_VERSION} (${_usdcc_debugpy_error}); "
                            "debugging Python in usdcc will need one among the Python packages.")
        endif()
        file(REMOVE "${_usdcc_debugpy_archive}")
    endif()
    if(EXISTS "${_usdcc_debugpy_dir}/debugpy/__init__.py")
        set(USDCC_DEBUGPY_DIR "${_usdcc_debugpy_dir}")
        message(STATUS "debugpy for usdcc: ${USDCC_DEBUGPY_DIR} (${USDCC_DEBUGPY_VERSION})")
    endif()
endif()

# For tools/deploy.py, which reads the cache.
set(USDCC_DEBUGPY_PATH "${USDCC_DEBUGPY_DIR}" CACHE INTERNAL "The bundled debugpy's folder, if downloaded")
