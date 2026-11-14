# Ruff (https://docs.astral.sh/ruff/), which the Script Editor's Ruff Format
# and Ruff Check run, bundled at a pinned version so formatting is the same
# for everyone (docs/SCRIPTING.md §7). Downloaded once at configure time from
# PyPI's wheel for this platform (Windows x64, Linux x86_64 or aarch64), checked
# against its SHA-256, and unpacked into build/ruff (ruff[.exe] and its MIT
# LICENSE); tools/deploy.py copies that into the deploy's tools/ruff. Offline,
# elsewhere, or with USDCC_BUNDLE_RUFF off, the app falls back to a ruff on PATH.
#
# To upgrade: pick a release at least two weeks old, and take the
# py3-none-win_amd64 and manylinux x86_64/aarch64 wheels' URLs and sha256s from
# https://pypi.org/pypi/ruff/<version>/json.
#
# Sets:
#   USDCC_RUFF_EXE   build/ruff/ruff[.exe], or empty when it isn't there

option(USDCC_BUNDLE_RUFF "Download the pinned ruff for the Script Editor" ON)

set(USDCC_RUFF_VERSION "0.17.1")
set(USDCC_RUFF_URL "")
if(WIN32)
    set(USDCC_RUFF_URL
        "https://files.pythonhosted.org/packages/6a/2e/da3d9589bae8e8b58f3a48bd5bc0b980232fb14a83bf02c3be7660f87c1b/ruff-0.17.1-py3-none-win_amd64.whl")
    set(USDCC_RUFF_SHA256 "5df76f16580fab58d0f6c98a3ac254a4abceb7b2489b8b678335869cff77b3d7")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(USDCC_RUFF_URL
        "https://files.pythonhosted.org/packages/62/86/5566290219708ce2928f3a29c4f843a1185b341ec6111c4631a33c5ceced/ruff-0.17.1-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl")
    set(USDCC_RUFF_SHA256 "a6ef98e51de8ef38082f90653ceeb84944bd7673783eea13e1647c5b9b064189")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(USDCC_RUFF_URL
        "https://files.pythonhosted.org/packages/c0/8c/e075e9c917cb0575297880c96768304e881240171c870cbd8d9b7b54bec2/ruff-0.17.1-py3-none-manylinux_2_17_aarch64.manylinux2014_aarch64.whl")
    set(USDCC_RUFF_SHA256 "872e03dd231744d6c5ec5c8af6be52b71323fc6ee096e4c8b34d9f844ee3b978")
endif()

set(_usdcc_ruff_name "ruff${CMAKE_EXECUTABLE_SUFFIX}")
set(_usdcc_ruff_dir "${CMAKE_BINARY_DIR}/ruff")
set(_usdcc_ruff_stamp "${_usdcc_ruff_dir}/VERSION")
set(USDCC_RUFF_EXE "")

if(USDCC_RUFF_URL AND USDCC_BUNDLE_RUFF)
    set(_usdcc_ruff_have "")
    if(EXISTS "${_usdcc_ruff_stamp}")
        file(READ "${_usdcc_ruff_stamp}" _usdcc_ruff_have)
        string(STRIP "${_usdcc_ruff_have}" _usdcc_ruff_have)
    endif()
    if(NOT _usdcc_ruff_have STREQUAL USDCC_RUFF_VERSION OR NOT EXISTS "${_usdcc_ruff_dir}/${_usdcc_ruff_name}")
        message(STATUS "Downloading ruff ${USDCC_RUFF_VERSION}")
        file(REMOVE_RECURSE "${_usdcc_ruff_dir}")
        # A wheel is a zip; the .zip name makes that plain to the extractor.
        set(_usdcc_ruff_archive "${CMAKE_BINARY_DIR}/ruff-${USDCC_RUFF_VERSION}.zip")
        file(DOWNLOAD "${USDCC_RUFF_URL}" "${_usdcc_ruff_archive}"
             EXPECTED_HASH SHA256=${USDCC_RUFF_SHA256}
             STATUS _usdcc_ruff_status TLS_VERIFY ON)
        list(GET _usdcc_ruff_status 0 _usdcc_ruff_code)
        if(_usdcc_ruff_code EQUAL 0)
            set(_usdcc_ruff_unpacked "${CMAKE_BINARY_DIR}/ruff-unpacked")
            file(REMOVE_RECURSE "${_usdcc_ruff_unpacked}")
            file(ARCHIVE_EXTRACT INPUT "${_usdcc_ruff_archive}" DESTINATION "${_usdcc_ruff_unpacked}"
                 PATTERNS "*/scripts/${_usdcc_ruff_name}" "*/licenses/LICENSE")
            set(_usdcc_ruff_data "${_usdcc_ruff_unpacked}/ruff-${USDCC_RUFF_VERSION}")
            file(COPY "${_usdcc_ruff_data}.data/scripts/${_usdcc_ruff_name}" DESTINATION "${_usdcc_ruff_dir}"
                 FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
            file(COPY "${_usdcc_ruff_data}.dist-info/licenses/LICENSE" DESTINATION "${_usdcc_ruff_dir}")
            file(WRITE "${_usdcc_ruff_stamp}" "${USDCC_RUFF_VERSION}\n")
            file(REMOVE_RECURSE "${_usdcc_ruff_unpacked}")
        else()
            list(GET _usdcc_ruff_status 1 _usdcc_ruff_error)
            message(WARNING "Couldn't download ruff ${USDCC_RUFF_VERSION} (${_usdcc_ruff_error}); "
                            "the Script Editor will use a ruff on PATH, if there is one.")
        endif()
        file(REMOVE "${_usdcc_ruff_archive}")
    endif()
    if(EXISTS "${_usdcc_ruff_dir}/${_usdcc_ruff_name}")
        set(USDCC_RUFF_EXE "${_usdcc_ruff_dir}/${_usdcc_ruff_name}")
        message(STATUS "ruff for the Script Editor: ${USDCC_RUFF_EXE} (${USDCC_RUFF_VERSION})")
    endif()
endif()

# For tools/deploy.py, which reads the cache.
set(USDCC_RUFF_EXE_PATH "${USDCC_RUFF_EXE}" CACHE INTERNAL "The bundled ruff executable, if downloaded")
