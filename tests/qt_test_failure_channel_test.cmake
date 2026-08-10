if(NOT DEFINED AEGISY_TEST_EXECUTABLE OR NOT EXISTS "${AEGISY_TEST_EXECUTABLE}")
    message(FATAL_ERROR "Qt failure-channel executable is missing")
endif()
if(NOT DEFINED AEGISY_TEST_LABEL OR AEGISY_TEST_LABEL STREQUAL "")
    message(FATAL_ERROR "Qt failure-channel label is missing")
endif()

execute_process(
    COMMAND "${AEGISY_TEST_EXECUTABLE}" --failure-channel-self-test
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_stdout
    ERROR_VARIABLE probe_stderr
    TIMEOUT 10)

if(NOT "${probe_result}" STREQUAL "86")
    message(FATAL_ERROR
        "${AEGISY_TEST_LABEL} failure-channel probe returned '${probe_result}', expected 86")
endif()
if(NOT probe_stdout STREQUAL "")
    message(FATAL_ERROR
        "${AEGISY_TEST_LABEL} failure-channel probe wrote to stdout")
endif()

string(REPLACE "\r\n" "\n" normalized_stderr "${probe_stderr}")
string(REPLACE "\r" "\n" normalized_stderr "${normalized_stderr}")
set(expected_stderr "AEGISY_TEST_FAILURE: QT_STDERR_CHANNEL_PROBE\n")
if(NOT normalized_stderr STREQUAL expected_stderr)
    message(FATAL_ERROR
        "${AEGISY_TEST_LABEL} failure-channel probe did not emit the exact bounded stderr marker")
endif()
