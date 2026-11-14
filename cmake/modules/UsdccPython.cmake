# The one Python usdcc uses: the interpreter OpenUSD itself was built with
# (Python3_EXECUTABLE/_LIBRARY/_INCLUDE_DIR, which find_package(USD) takes
# from pxrConfig.cmake). The app embeds it, usdcc's Python modules are built
# for it, and OpenUSD's own pxr modules need it — see docs/SCRIPTING.md §3.1.
#
# Not vcpkg's python3: that one only exists as a build-time tool for pybind11,
# and vcpkg's deploy step would otherwise put its python3XY.dll next to the
# executables, pairing OpenUSD's Python with a different Python runtime.
#
# Include after find_package(USD). Sets:
#   USDCC_PYTHON_FOUND        TRUE if everything below was found
#   USDCC_PYTHON_EXECUTABLE   python.exe
#   USDCC_PYTHON_HOME         its prefix (holds Lib/, DLLs/, python3XY.dll)
#   USDCC_PYTHON_VERSION      "3.12"
#   USDCC_PYTHON_INCLUDE_DIR  Python.h
#   USDCC_PYTHON_LIBRARY      the import library to link
#   USDCC_PYTHON_RUNTIME_DLLS python3XY.dll and python3.dll (Windows)
#   USDCC_USD_PYTHON_DIR      the folder holding OpenUSD's pxr package
# and defines usdcc_deploy_python(<target>), which copies those DLLs next to
# the target after vcpkg's own deploy step has run.

set(USDCC_PYTHON_FOUND FALSE)
set(USDCC_PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")

if(USDCC_PYTHON_EXECUTABLE AND EXISTS "${USDCC_PYTHON_EXECUTABLE}")
    execute_process(
        COMMAND "${USDCC_PYTHON_EXECUTABLE}" -c
                "import sys, sysconfig; print(sys.base_prefix); print(sysconfig.get_config_var('py_version_short')); print(sysconfig.get_path('include')); print(sysconfig.get_config_var('LIBDIR') or ''); print(sysconfig.get_config_var('LDLIBRARY') or '')"
        OUTPUT_VARIABLE _usdcc_python_probe
        RESULT_VARIABLE _usdcc_python_probe_result
        ERROR_QUIET)
    if(_usdcc_python_probe_result EQUAL 0)
        string(STRIP "${_usdcc_python_probe}" _usdcc_python_probe)
        string(REPLACE "\n" ";" _usdcc_python_probe "${_usdcc_python_probe}")
        list(GET _usdcc_python_probe 0 USDCC_PYTHON_HOME)
        list(GET _usdcc_python_probe 1 USDCC_PYTHON_VERSION)
        list(GET _usdcc_python_probe 2 _usdcc_python_include)
        list(LENGTH _usdcc_python_probe _usdcc_python_probe_length)
        set(_usdcc_python_shared "")
        if(_usdcc_python_probe_length GREATER 4)
            list(GET _usdcc_python_probe 3 _usdcc_python_libdir)
            list(GET _usdcc_python_probe 4 _usdcc_python_ldlibrary)
            set(_usdcc_python_shared "${_usdcc_python_libdir}/${_usdcc_python_ldlibrary}")
        endif()
        file(TO_CMAKE_PATH "${USDCC_PYTHON_HOME}" USDCC_PYTHON_HOME)
        file(TO_CMAKE_PATH "${_usdcc_python_include}" _usdcc_python_include)
        string(REPLACE "." "" _usdcc_python_nodot "${USDCC_PYTHON_VERSION}")

        set(USDCC_PYTHON_INCLUDE_DIR "${Python3_INCLUDE_DIR}")
        if(NOT USDCC_PYTHON_INCLUDE_DIR)
            set(USDCC_PYTHON_INCLUDE_DIR "${_usdcc_python_include}")
        endif()
        set(USDCC_PYTHON_LIBRARY "${Python3_LIBRARY}")
        if(NOT USDCC_PYTHON_LIBRARY AND WIN32)
            set(USDCC_PYTHON_LIBRARY "${USDCC_PYTHON_HOME}/libs/python${_usdcc_python_nodot}.lib")
        elseif(NOT USDCC_PYTHON_LIBRARY AND _usdcc_python_shared MATCHES "[.](so|dylib)$" AND EXISTS "${_usdcc_python_shared}")
            # Linux/macOS: libpython3.XY.so from sysconfig, when pxrConfig.cmake
            # doesn't name it. The app embeds Python, so it needs the shared one.
            set(USDCC_PYTHON_LIBRARY "${_usdcc_python_shared}")
        endif()

        set(USDCC_PYTHON_RUNTIME_DLLS "")
        if(WIN32)
            foreach(_dll "python${_usdcc_python_nodot}.dll" "python3.dll")
                if(EXISTS "${USDCC_PYTHON_HOME}/${_dll}")
                    list(APPEND USDCC_PYTHON_RUNTIME_DLLS "${USDCC_PYTHON_HOME}/${_dll}")
                endif()
            endforeach()
        endif()

        if(EXISTS "${USDCC_PYTHON_INCLUDE_DIR}/Python.h" AND EXISTS "${USDCC_PYTHON_LIBRARY}")
            set(USDCC_PYTHON_FOUND TRUE)
        endif()
    endif()
endif()

# Where OpenUSD's own pxr package is: lib/site-packages on Windows,
# lib/python3.XY/site-packages from build_usd.py on Linux and macOS.
set(USDCC_USD_PYTHON_DIR "${USD_INSTALL}/lib/site-packages")
foreach(_dir "${USD_INSTALL}/lib/site-packages" "${USD_INSTALL}/lib/python${USDCC_PYTHON_VERSION}/site-packages"
             "${USD_INSTALL}/lib/python")
    if(EXISTS "${_dir}/pxr/__init__.py")
        set(USDCC_USD_PYTHON_DIR "${_dir}")
        break()
    endif()
endforeach()

if(USDCC_PYTHON_FOUND)
    message(STATUS "Python for usdcc (OpenUSD's): ${USDCC_PYTHON_VERSION} at ${USDCC_PYTHON_HOME}; pxr in ${USDCC_USD_PYTHON_DIR}")
else()
    message(STATUS "Python for usdcc: OpenUSD's Python (${Python3_EXECUTABLE}) not usable; "
                   "scripting and the usdcc.* Python modules are disabled")
endif()

function(usdcc_deploy_python target)
    if(WIN32 AND USDCC_PYTHON_RUNTIME_DLLS)
        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${USDCC_PYTHON_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()
