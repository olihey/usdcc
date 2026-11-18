# Velopack (https://velopack.io), which installs usdcc on Windows and updates it
# from the GitHub releases (docs/UPDATES.md). Its C/C++ library, pinned:
# downloaded once at configure time from Velopack's GitHub release, checked
# against its SHA-256, and the header and the Windows x64 libraries unpacked
# into build/velopack, with Velopack's MIT LICENSE beside them; tools/deploy.py
# copies the licence into the deploy's licenses/Velopack.
#
# Velopack is Rust, built with the static C runtime (libcmt). So the root
# launcher (src/cpp/launcher), a small Win32 program, links the static library
# and is itself built with the static runtime; the app, on the runtime DLL as
# all of usdcc is, links the DLL, which keeps its runtime to itself. The zip's
# velopack_libc_win_x64_msvc.dll is velopack_libc.dll to its import library, so
# it is unpacked under that name and copied beside usdcc.exe as it.
#
# Windows only: on Linux the release stays a tar.xz (docs/UPDATES.md §2).
# Offline, or with USDCC_VELOPACK off, usdcc builds without it: no launcher,
# and Help > Check for Updates only opens the releases page.
#
# To upgrade: pick a release at least two weeks old, take
# velopack_libc_<version>.zip's sha256 from it (gh release view <version>
# --repo velopack/velopack --json assets) and the LICENSE's at that tag, and use
# the same version of vpk in .github/workflows/release.yml.
#
# Sets:
#   USDCC_VELOPACK_FOUND   TRUE when the library is there
#   usdcc::velopack        the header and the DLL (USDCC_VELOPACK_DLL)
#   usdcc::velopack_static the header and the static library, with the Windows
#                          libraries it needs, for a program on the static runtime
#   USDCC_VELOPACK_DLL     the DLL, to copy beside the program that links it

option(USDCC_VELOPACK "Download Velopack's library, for the installer and updates (Windows)" ON)

set(USDCC_VELOPACK_VERSION "1.2.161")
set(USDCC_VELOPACK_URL
    "https://github.com/velopack/velopack/releases/download/${USDCC_VELOPACK_VERSION}/velopack_libc_${USDCC_VELOPACK_VERSION}.zip")
set(USDCC_VELOPACK_SHA256 "31de8f2cf05a47bd5328a9f98b0c41aec1582156f6075f1058b26941f7f17c20")
set(USDCC_VELOPACK_LICENSE_URL
    "https://raw.githubusercontent.com/velopack/velopack/${USDCC_VELOPACK_VERSION}/LICENSE")
set(USDCC_VELOPACK_LICENSE_SHA256 "91845db83551c877ebbb1118e0fb92e4e527290d23b995c55dcd438b3293943f")

set(_usdcc_velopack_dir "${CMAKE_BINARY_DIR}/velopack")
set(_usdcc_velopack_stamp "${_usdcc_velopack_dir}/VERSION")
set(_usdcc_velopack_lib "${_usdcc_velopack_dir}/lib-static/velopack_libc_win_x64_msvc.lib")
set(_usdcc_velopack_dll "${_usdcc_velopack_dir}/lib/velopack_libc.dll")
set(_usdcc_velopack_implib "${_usdcc_velopack_dir}/lib/velopack_libc_win_x64_msvc.dll.lib")
set(USDCC_VELOPACK_FOUND FALSE)

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND USDCC_VELOPACK)
    set(_usdcc_velopack_have "")
    if(EXISTS "${_usdcc_velopack_stamp}")
        file(READ "${_usdcc_velopack_stamp}" _usdcc_velopack_have)
        string(STRIP "${_usdcc_velopack_have}" _usdcc_velopack_have)
    endif()
    if(NOT _usdcc_velopack_have STREQUAL USDCC_VELOPACK_VERSION OR NOT EXISTS "${_usdcc_velopack_lib}"
       OR NOT EXISTS "${_usdcc_velopack_dll}")
        message(STATUS "Downloading Velopack ${USDCC_VELOPACK_VERSION}")
        file(REMOVE_RECURSE "${_usdcc_velopack_dir}")
        set(_usdcc_velopack_archive "${CMAKE_BINARY_DIR}/velopack_libc_${USDCC_VELOPACK_VERSION}.zip")
        file(DOWNLOAD "${USDCC_VELOPACK_URL}" "${_usdcc_velopack_archive}"
             EXPECTED_HASH SHA256=${USDCC_VELOPACK_SHA256}
             STATUS _usdcc_velopack_status TLS_VERIFY ON)
        list(GET _usdcc_velopack_status 0 _usdcc_velopack_code)
        if(_usdcc_velopack_code EQUAL 0)
            file(DOWNLOAD "${USDCC_VELOPACK_LICENSE_URL}" "${_usdcc_velopack_dir}/LICENSE"
                 EXPECTED_HASH SHA256=${USDCC_VELOPACK_LICENSE_SHA256}
                 STATUS _usdcc_velopack_status TLS_VERIFY ON)
            list(GET _usdcc_velopack_status 0 _usdcc_velopack_code)
        endif()
        if(_usdcc_velopack_code EQUAL 0)
            set(_usdcc_velopack_unpacked "${CMAKE_BINARY_DIR}/velopack-unpacked")
            file(REMOVE_RECURSE "${_usdcc_velopack_unpacked}")
            file(ARCHIVE_EXTRACT INPUT "${_usdcc_velopack_archive}" DESTINATION "${_usdcc_velopack_unpacked}"
                 PATTERNS "include/*" "lib-static/velopack_libc_win_x64_msvc.lib"
                          "lib/velopack_libc_win_x64_msvc.dll" "lib/velopack_libc_win_x64_msvc.dll.lib")
            foreach(_usdcc_velopack_part include lib lib-static)
                file(COPY "${_usdcc_velopack_unpacked}/${_usdcc_velopack_part}" DESTINATION "${_usdcc_velopack_dir}")
            endforeach()
            file(RENAME "${_usdcc_velopack_dir}/lib/velopack_libc_win_x64_msvc.dll" "${_usdcc_velopack_dll}")
            file(WRITE "${_usdcc_velopack_stamp}" "${USDCC_VELOPACK_VERSION}\n")
            file(REMOVE_RECURSE "${_usdcc_velopack_unpacked}")
        else()
            list(GET _usdcc_velopack_status 1 _usdcc_velopack_error)
            message(WARNING "Couldn't download Velopack ${USDCC_VELOPACK_VERSION} (${_usdcc_velopack_error}); "
                            "building without the launcher and updates.")
            file(REMOVE_RECURSE "${_usdcc_velopack_dir}")
        endif()
        file(REMOVE "${_usdcc_velopack_archive}")
    endif()
    if(EXISTS "${_usdcc_velopack_lib}" AND EXISTS "${_usdcc_velopack_dll}"
       AND EXISTS "${_usdcc_velopack_dir}/include/Velopack.hpp")
        set(USDCC_VELOPACK_FOUND TRUE)
        set(USDCC_VELOPACK_DLL "${_usdcc_velopack_dll}")
        add_library(usdcc::velopack SHARED IMPORTED GLOBAL)
        set_target_properties(usdcc::velopack PROPERTIES
            IMPORTED_LOCATION "${_usdcc_velopack_dll}"
            IMPORTED_IMPLIB "${_usdcc_velopack_implib}"
            INTERFACE_INCLUDE_DIRECTORIES "${_usdcc_velopack_dir}/include"
            INTERFACE_COMPILE_DEFINITIONS "USDCC_HAS_VELOPACK=1")
        add_library(usdcc::velopack_static STATIC IMPORTED GLOBAL)
        set_target_properties(usdcc::velopack_static PROPERTIES
            IMPORTED_LOCATION "${_usdcc_velopack_lib}"
            INTERFACE_INCLUDE_DIRECTORIES "${_usdcc_velopack_dir}/include"
            INTERFACE_COMPILE_DEFINITIONS "USDCC_HAS_VELOPACK=1"
            # What Rust's standard library and Velopack's own Windows calls link
            # against; a static Rust library leaves them to the program.
            INTERFACE_LINK_LIBRARIES
                "kernel32;advapi32;bcrypt;ntdll;userenv;ws2_32;ole32;oleaut32;shell32;user32;crypt32;secur32;ncrypt;propsys;shlwapi;version;rpcrt4;uuid;iphlpapi;dbghelp;winhttp;gdi32;comctl32")
        message(STATUS "Velopack for the installer and updates: ${_usdcc_velopack_dir} (${USDCC_VELOPACK_VERSION})")
    endif()
endif()

# For tools/deploy.py, which reads the cache.
if(USDCC_VELOPACK_FOUND)
    set(USDCC_VELOPACK_LICENSE_PATH "${_usdcc_velopack_dir}/LICENSE" CACHE INTERNAL "Velopack's licence")
else()
    set(USDCC_VELOPACK_LICENSE_PATH "" CACHE INTERNAL "Velopack's licence")
endif()
