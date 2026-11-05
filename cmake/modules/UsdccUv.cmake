# uv (https://docs.astral.sh/uv/), which installs the Python packages plugins and
# scripts ask for (docs/PYTHON_PACKAGES.md), bundled at a pinned version.
# Downloaded once at configure time from PyPI's Windows wheel, checked against
# its SHA-256, and unpacked into build/uv (uv.exe and its licenses);
# tools/deploy.ps1 copies that into the deploy's tools/uv. Offline, or with
# USDCC_BUNDLE_UV off, the app falls back to a uv on PATH.
#
# To upgrade: pick a release at least two weeks old, and take the
# py3-none-win_amd64 wheel's URL and sha256 from
# https://pypi.org/pypi/uv/<version>/json.
#
# Sets:
#   USDCC_UV_EXE   build/uv/uv.exe, or empty when it isn't there

option(USDCC_BUNDLE_UV "Download the pinned uv, which installs Python packages" ON)

set(USDCC_UV_VERSION "0.13.5")
set(USDCC_UV_URL
    "https://files.pythonhosted.org/packages/32/ea/771744a784b16aa21d195cb5d5d5a2878d72fa00fc26f11ea3f9646994fe/uv-0.13.5-py3-none-win_amd64.whl")
set(USDCC_UV_SHA256 "1fb29fc46ba12033e2ef95a95ac79d649e71bbb1358419bbbcbd19d86026ef2f")

set(_usdcc_uv_dir "${CMAKE_BINARY_DIR}/uv")
set(_usdcc_uv_stamp "${_usdcc_uv_dir}/VERSION")
set(USDCC_UV_EXE "")

if(WIN32 AND USDCC_BUNDLE_UV)
    set(_usdcc_uv_have "")
    if(EXISTS "${_usdcc_uv_stamp}")
        file(READ "${_usdcc_uv_stamp}" _usdcc_uv_have)
        string(STRIP "${_usdcc_uv_have}" _usdcc_uv_have)
    endif()
    if(NOT _usdcc_uv_have STREQUAL USDCC_UV_VERSION OR NOT EXISTS "${_usdcc_uv_dir}/uv.exe")
        message(STATUS "Downloading uv ${USDCC_UV_VERSION}")
        file(REMOVE_RECURSE "${_usdcc_uv_dir}")
        # A wheel is a zip; the .zip name makes that plain to the extractor.
        set(_usdcc_uv_archive "${CMAKE_BINARY_DIR}/uv-${USDCC_UV_VERSION}.zip")
        file(DOWNLOAD "${USDCC_UV_URL}" "${_usdcc_uv_archive}"
             EXPECTED_HASH SHA256=${USDCC_UV_SHA256}
             STATUS _usdcc_uv_status TLS_VERIFY ON)
        list(GET _usdcc_uv_status 0 _usdcc_uv_code)
        if(_usdcc_uv_code EQUAL 0)
            set(_usdcc_uv_unpacked "${CMAKE_BINARY_DIR}/uv-unpacked")
            file(REMOVE_RECURSE "${_usdcc_uv_unpacked}")
            file(ARCHIVE_EXTRACT INPUT "${_usdcc_uv_archive}" DESTINATION "${_usdcc_uv_unpacked}"
                 PATTERNS "*/scripts/uv.exe" "*/licenses/*")
            set(_usdcc_uv_data "${_usdcc_uv_unpacked}/uv-${USDCC_UV_VERSION}")
            file(COPY "${_usdcc_uv_data}.data/scripts/uv.exe" DESTINATION "${_usdcc_uv_dir}")
            file(GLOB _usdcc_uv_licenses "${_usdcc_uv_data}.dist-info/licenses/*")
            file(COPY ${_usdcc_uv_licenses} DESTINATION "${_usdcc_uv_dir}")
            file(WRITE "${_usdcc_uv_stamp}" "${USDCC_UV_VERSION}\n")
            file(REMOVE_RECURSE "${_usdcc_uv_unpacked}")
        else()
            list(GET _usdcc_uv_status 1 _usdcc_uv_error)
            message(WARNING "Couldn't download uv ${USDCC_UV_VERSION} (${_usdcc_uv_error}); "
                            "installing Python packages will use a uv on PATH, if there is one.")
        endif()
        file(REMOVE "${_usdcc_uv_archive}")
    endif()
    if(EXISTS "${_usdcc_uv_dir}/uv.exe")
        set(USDCC_UV_EXE "${_usdcc_uv_dir}/uv.exe")
        message(STATUS "uv for Python packages: ${USDCC_UV_EXE} (${USDCC_UV_VERSION})")
    endif()
endif()

# For tools/deploy.ps1, which reads the cache.
set(USDCC_UV_EXE_PATH "${USDCC_UV_EXE}" CACHE INTERNAL "The bundled uv.exe, if downloaded")
