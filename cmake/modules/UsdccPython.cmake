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
# and defines usdcc_deploy_python(<target>), which copies those DLLs next to
# the target after vcpkg's own deploy step has run.

set(USDCC_PYTHON_FOUND FALSE)
set(USDCC_PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")

if(USDCC_PYTHON_EXECUTABLE AND EXISTS "${USDCC_PYTHON_EXECUTABLE}")
    execute_process(
        COMMAND "${USDCC_PYTHON_EXECUTABLE}" -c
                "import sys, sysconfig; print(sys.base_prefix); print(sysconfig.get_config_var('py_version_short')); print(sysconfig.get_path('include'))"
        OUTPUT_VARIABLE _usdcc_python_probe
        RESULT_VARIABLE _usdcc_python_probe_result
        ERROR_QUIET)
    if(_usdcc_python_probe_result EQUAL 0)
        string(STRIP "${_usdcc_python_probe}" _usdcc_python_probe)
        string(REPLACE "\n" ";" _usdcc_python_probe "${_usdcc_python_probe}")
        list(GET _usdcc_python_probe 0 USDCC_PYTHON_HOME)
        list(GET _usdcc_python_probe 1 USDCC_PYTHON_VERSION)
        list(GET _usdcc_python_probe 2 _usdcc_python_include)
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

if(USDCC_PYTHON_FOUND)
    message(STATUS "Python for usdcc (OpenUSD's): ${USDCC_PYTHON_VERSION} at ${USDCC_PYTHON_HOME}")
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
