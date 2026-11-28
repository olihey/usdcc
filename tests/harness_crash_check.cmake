# Runs usdcc_harness_crash_check (harness_crash_check.cpp), which crashes on purpose, and
# passes when the harness reported the crash: the test it was in, and the function it
# crashed in. CTest itself counts any crash as a failure, so the check is made here.
#   cmake -DPROGRAM=<usdcc_harness_crash_check> -P harness_crash_check.cmake
execute_process(COMMAND "${PROGRAM}" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(report "${out}${err}")
message("${report}")
if(result STREQUAL "0")
    message(FATAL_ERROR "usdcc_harness_crash_check didn't crash")
endif()
if(NOT report MATCHES "Crash in test \\[harness_reports_a_crash\\]")
    message(FATAL_ERROR "The crash report doesn't name the test (exit: ${result})")
endif()
if(NOT report MATCHES "usdccHarnessReadThrough")
    message(FATAL_ERROR "The crash report doesn't name the function it crashed in (exit: ${result})")
endif()
message(STATUS "The crash (${result}) was reported with its test and function")
