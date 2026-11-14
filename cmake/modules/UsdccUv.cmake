# uv (https://docs.astral.sh/uv/), which installs the Python packages plugins and
# scripts ask for (docs/PYTHON_PACKAGES.md), bundled at a pinned version.
# Downloaded once at configure time from PyPI's wheel for this platform (Windows
# x64, Linux x86_64 or aarch64), checked against its SHA-256, and unpacked into
# build/uv (uv[.exe] and its licenses); tools/deploy.py copies that into the
# deploy's tools/uv. Offline, elsewhere, or with USDCC_BUNDLE_UV off, the app
# falls back to a uv on PATH.
#
# To upgrade: pick a release at least two weeks old, and take the
# py3-none-win_amd64 and manylinux x86_64/aarch64 wheels' URLs and sha256s from
# https://pypi.org/pypi/uv/<version>/json.
#
# Sets:
#   USDCC_UV_EXE   build/uv/uv[.exe], or empty when it isn't there

option(USDCC_BUNDLE_UV "Download the pinned uv, which installs Python packages" ON)

set(USDCC_UV_VERSION "0.13.5")
set(USDCC_UV_URL "")
if(WIN32)
    set(USDCC_UV_URL
        "https://files.pythonhosted.org/packages/32/ea/771744a784b16aa21d195cb5d5d5a2878d72fa00fc26f11ea3f9646994fe/uv-0.13.5-py3-none-win_amd64.whl")
    set(USDCC_UV_SHA256 "1fb29fc46ba12033e2ef95a95ac79d649e71bbb1358419bbbcbd19d86026ef2f")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(USDCC_UV_URL
        "https://files.pythonhosted.org/packages/3a/ad/3c2a81d66d1839063662e5b6c0002ec31f8ee987dbc5295747627ca6160a/uv-0.13.5-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl")
    set(USDCC_UV_SHA256 "fa8d26e08e34c29c0c4e820cf3181757086d547240b4f4d31ff1d8bd810ca946")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(USDCC_UV_URL
        "https://files.pythonhosted.org/packages/4c/92/d83d019c63c1ccc6ff17df2bfe76270412f9766e7e71aa4d4ccbf018683f/uv-0.13.5-py3-none-manylinux_2_28_aarch64.whl")
    set(USDCC_UV_SHA256 "91e58e304b63b6c91bac5561459ab0c3ea24f17bc01ea9359d69fed72142978f")
endif()

set(_usdcc_uv_name "uv${CMAKE_EXECUTABLE_SUFFIX}")
set(_usdcc_uv_dir "${CMAKE_BINARY_DIR}/uv")
set(_usdcc_uv_stamp "${_usdcc_uv_dir}/VERSION")
set(USDCC_UV_EXE "")

if(USDCC_UV_URL AND USDCC_BUNDLE_UV)
    set(_usdcc_uv_have "")
    if(EXISTS "${_usdcc_uv_stamp}")
        file(READ "${_usdcc_uv_stamp}" _usdcc_uv_have)
        string(STRIP "${_usdcc_uv_have}" _usdcc_uv_have)
    endif()
    if(NOT _usdcc_uv_have STREQUAL USDCC_UV_VERSION OR NOT EXISTS "${_usdcc_uv_dir}/${_usdcc_uv_name}")
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
                 PATTERNS "*/scripts/${_usdcc_uv_name}" "*/licenses/*")
            set(_usdcc_uv_data "${_usdcc_uv_unpacked}/uv-${USDCC_UV_VERSION}")
            file(COPY "${_usdcc_uv_data}.data/scripts/${_usdcc_uv_name}" DESTINATION "${_usdcc_uv_dir}"
                 FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
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
    if(EXISTS "${_usdcc_uv_dir}/${_usdcc_uv_name}")
        set(USDCC_UV_EXE "${_usdcc_uv_dir}/${_usdcc_uv_name}")
        message(STATUS "uv for Python packages: ${USDCC_UV_EXE} (${USDCC_UV_VERSION})")
    endif()
endif()

# For tools/deploy.py, which reads the cache.
set(USDCC_UV_EXE_PATH "${USDCC_UV_EXE}" CACHE INTERNAL "The bundled uv executable, if downloaded")
