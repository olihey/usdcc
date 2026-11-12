# Ruff (https://docs.astral.sh/ruff/), which the Script Editor's Ruff Format
# and Ruff Check run, bundled at a pinned version so formatting is the same
# for everyone (docs/SCRIPTING.md §7). Downloaded once at configure time from
# PyPI's Windows wheel, checked against its SHA-256, and unpacked into
# build/ruff (ruff.exe and its MIT LICENSE); tools/deploy.py copies that into
# the deploy's tools/ruff. Offline, or with USDCC_BUNDLE_RUFF off, the app
# falls back to a ruff on PATH.
#
# To upgrade: pick a release at least two weeks old, and take the
# py3-none-win_amd64 wheel's URL and sha256 from
# https://pypi.org/pypi/ruff/<version>/json.
#
# Sets:
#   USDCC_RUFF_EXE   build/ruff/ruff.exe, or empty when it isn't there

option(USDCC_BUNDLE_RUFF "Download the pinned ruff for the Script Editor" ON)

set(USDCC_RUFF_VERSION "0.17.1")
set(USDCC_RUFF_URL
    "https://files.pythonhosted.org/packages/6a/2e/da3d9589bae8e8b58f3a48bd5bc0b980232fb14a83bf02c3be7660f87c1b/ruff-0.17.1-py3-none-win_amd64.whl")
set(USDCC_RUFF_SHA256 "5df76f16580fab58d0f6c98a3ac254a4abceb7b2489b8b678335869cff77b3d7")

set(_usdcc_ruff_dir "${CMAKE_BINARY_DIR}/ruff")
set(_usdcc_ruff_stamp "${_usdcc_ruff_dir}/VERSION")
set(USDCC_RUFF_EXE "")

if(WIN32 AND USDCC_BUNDLE_RUFF)
    set(_usdcc_ruff_have "")
    if(EXISTS "${_usdcc_ruff_stamp}")
        file(READ "${_usdcc_ruff_stamp}" _usdcc_ruff_have)
        string(STRIP "${_usdcc_ruff_have}" _usdcc_ruff_have)
    endif()
    if(NOT _usdcc_ruff_have STREQUAL USDCC_RUFF_VERSION OR NOT EXISTS "${_usdcc_ruff_dir}/ruff.exe")
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
                 PATTERNS "*/scripts/ruff.exe" "*/licenses/LICENSE")
            set(_usdcc_ruff_data "${_usdcc_ruff_unpacked}/ruff-${USDCC_RUFF_VERSION}")
            file(COPY "${_usdcc_ruff_data}.data/scripts/ruff.exe"
                      "${_usdcc_ruff_data}.dist-info/licenses/LICENSE"
                 DESTINATION "${_usdcc_ruff_dir}")
            file(WRITE "${_usdcc_ruff_stamp}" "${USDCC_RUFF_VERSION}\n")
            file(REMOVE_RECURSE "${_usdcc_ruff_unpacked}")
        else()
            list(GET _usdcc_ruff_status 1 _usdcc_ruff_error)
            message(WARNING "Couldn't download ruff ${USDCC_RUFF_VERSION} (${_usdcc_ruff_error}); "
                            "the Script Editor will use a ruff on PATH, if there is one.")
        endif()
        file(REMOVE "${_usdcc_ruff_archive}")
    endif()
    if(EXISTS "${_usdcc_ruff_dir}/ruff.exe")
        set(USDCC_RUFF_EXE "${_usdcc_ruff_dir}/ruff.exe")
        message(STATUS "ruff for the Script Editor: ${USDCC_RUFF_EXE} (${USDCC_RUFF_VERSION})")
    endif()
endif()

# For tools/deploy.py, which reads the cache.
set(USDCC_RUFF_EXE_PATH "${USDCC_RUFF_EXE}" CACHE INTERNAL "The bundled ruff.exe, if downloaded")
