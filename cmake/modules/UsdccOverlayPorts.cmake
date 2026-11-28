# A check that vcpkg's installed packages were built with the patches of an overlay port
# of this checkout: on macOS cmake/ports-macos, a patched Qt (docs/MACOS.md §2).
#
# vcpkg builds a package again when its port changed, but only where it runs. A build
# with VCPKG_MANIFEST_INSTALL off and VCPKG_INSTALLED_DIR at another checkout's
# build/vcpkg_installed, as a worktree's, never runs it and links whatever is installed
# there. With a Qt from before cgimage-colorspace-lifetime.patch that configured, built
# and ran, and crashed whenever a wait cursor was set.
#
# vcpkg lists every file of the port a package was built from, with its SHA-256, in
# <installed>/<triplet>/share/<port>/vcpkg_abi_info.txt. Each patch (*.patch, *.diff) in
# the overlay port's folder has to be listed there, with the hash of this checkout's file:
# the folder is the list, so a patch added to a port, dropped from it or changed needs
# nothing here. A port that isn't installed yet, as before a first configure's vcpkg has
# run, isn't checked, and neither is a build without vcpkg's toolchain.
#
# A missing or different patch stops the configure rather than warns: the build would
# succeed, and what it lacks shows only as a crash later, which a warning scrolled past
# in a configure's output doesn't explain. USDCC_ALLOW_UNPATCHED_PORTS makes it a warning,
# for a build that is known not to need the patch.

option(USDCC_ALLOW_UNPATCHED_PORTS
       "Only warn when an installed vcpkg package lacks a patch of this checkout's overlay ports" OFF)

# usdcc_check_overlay_ports(<folder of overlay ports>)
function(usdcc_check_overlay_ports overlay_dir)
    if(NOT VCPKG_INSTALLED_DIR OR NOT VCPKG_TARGET_TRIPLET)
        return()
    endif()
    set(_installed "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    file(GLOB _ports RELATIVE "${overlay_dir}" "${overlay_dir}/*")
    foreach(_port IN LISTS _ports)
        set(_port_dir "${overlay_dir}/${_port}")
        set(_abi_file "${_installed}/share/${_port}/vcpkg_abi_info.txt")
        if(NOT IS_DIRECTORY "${_port_dir}" OR NOT EXISTS "${_abi_file}")
            continue()
        endif()
        # "<file> <sha256>" a line, between newlines so that a whole line is searched for.
        file(STRINGS "${_abi_file}" _abi)
        list(JOIN _abi "\n" _abi)
        set(_abi "\n${_abi}\n")

        file(GLOB_RECURSE _patches RELATIVE "${_port_dir}" "${_port_dir}/*.patch" "${_port_dir}/*.diff")
        set(_problems "")
        foreach(_patch IN LISTS _patches)
            file(SHA256 "${_port_dir}/${_patch}" _hash)
            string(FIND "${_abi}" "\n${_patch} ${_hash}\n" _same)
            string(FIND "${_abi}" "\n${_patch} " _listed)
            if(_same EQUAL -1 AND _listed EQUAL -1)
                string(APPEND _problems "  ${_patch}: not in it\n")
            elseif(_same EQUAL -1)
                string(APPEND _problems "  ${_patch}: another version of it\n")
            endif()
        endforeach()
        if(NOT _problems)
            continue()
        endif()

        file(RELATIVE_PATH _port_name "${CMAKE_SOURCE_DIR}" "${_port_dir}")
        if(VCPKG_MANIFEST_INSTALL)
            # Not seen: vcpkg has just installed it, from this port unless something
            # else changed its overlay ports.
            string(CONCAT _advice "vcpkg installed it for this build, so it didn't take the port "
                          "from there: see what VCPKG_OVERLAY_PORTS is.")
        else()
            string(CONCAT _advice
                          "This build doesn't run vcpkg (VCPKG_MANIFEST_INSTALL is OFF) and links "
                          "what is installed there. Configure the checkout that folder belongs to, "
                          "at a commit with these patches and with VCPKG_MANIFEST_INSTALL on, its "
                          "default (tools/build.py there does), so that vcpkg builds ${_port} "
                          "again. Then configure this build again.")
        endif()
        if(USDCC_ALLOW_UNPATCHED_PORTS)
            set(_kind WARNING)
            set(_opt_out "")
        else()
            set(_kind FATAL_ERROR)
            set(_opt_out "\nConfigure with -DUSDCC_ALLOW_UNPATCHED_PORTS=ON to make this a warning.")
        endif()
        message(
            ${_kind}
            "The ${_port} installed in\n"
            "  ${_installed}\n"
            "wasn't built with the patches of ${_port_name} (docs/MACOS.md §2), going by its "
            "share/${_port}/vcpkg_abi_info.txt:\n"
            "${_problems}"
            "What links it lacks those fixes. ${_advice}${_opt_out}")
    endforeach()
endfunction()
