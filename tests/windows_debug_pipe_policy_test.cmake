set(workflow_path
    "${AEGISY_SOURCE_DIR}/.github/workflows/windows-debug-pipe.yml")
if(NOT EXISTS "${workflow_path}")
    message(FATAL_ERROR "Windows named-pipe focused workflow is missing")
endif()

function(validate_monaco_probe workflow_text out_errors)
    set(errors)
    string(REPLACE "\r\n" "\n" workflow_text "${workflow_text}")
    string(REPLACE "\r" "\n" workflow_text "${workflow_text}")

    set(step_marker "      - name: Probe monaco render test with ctest environment\n")
    string(FIND "${workflow_text}" "${step_marker}" step_offset)
    if(step_offset EQUAL -1)
        list(APPEND errors "missing Monaco render probe step")
    else()
        string(SUBSTRING "${workflow_text}" ${step_offset} -1 probe_step)
        foreach(required_fragment
                "$ErrorActionPreference = 'Stop'"
                "$PSNativeCommandUseErrorActionPreference = $true"
                "Start-Process -FilePath $exe -Wait -PassThru"
                "if ($proc.ExitCode -ne 0) {"
                "throw \"Monaco render probe failed with exit code $($proc.ExitCode)\"")
            string(FIND "${probe_step}" "${required_fragment}" fragment_offset)
            if(fragment_offset EQUAL -1)
                list(APPEND errors
                    "Monaco render probe is missing fail-closed fragment: ${required_fragment}")
            endif()
        endforeach()
    endif()

    set(${out_errors} "${errors}" PARENT_SCOPE)
endfunction()

function(expect_probe_rejection case_name workflow_text expected_error)
    validate_monaco_probe("${workflow_text}" actual_errors)
    string(JOIN "\n" actual_error_text ${actual_errors})
    string(FIND "${actual_error_text}" "${expected_error}" expected_error_offset)
    if(expected_error_offset EQUAL -1)
        message(FATAL_ERROR
            "Windows debug workflow negative case '${case_name}' was not rejected. "
            "Expected '${expected_error}', got '${actual_error_text}'")
    endif()
endfunction()

file(READ "${workflow_path}" workflow)
validate_monaco_probe("${workflow}" workflow_errors)
if(workflow_errors)
    string(JOIN "\n" workflow_error_text ${workflow_errors})
    message(FATAL_ERROR
        "Windows debug workflow policy failed:\n${workflow_error_text}")
endif()

set(non_failing_exit_workflow "${workflow}")
string(REPLACE
    "if ($proc.ExitCode -ne 0) {"
    "if ($proc.ExitCode -eq 0) {"
    non_failing_exit_workflow
    "${non_failing_exit_workflow}")
expect_probe_rejection(
    "non-failing-exit-check"
    "${non_failing_exit_workflow}"
    "missing fail-closed fragment: if ($proc.ExitCode -ne 0) {")

set(native_failure_ignored_workflow "${workflow}")
string(REPLACE
    "$PSNativeCommandUseErrorActionPreference = $true\n"
    ""
    native_failure_ignored_workflow
    "${native_failure_ignored_workflow}")
expect_probe_rejection(
    "native-command-failure-ignored"
    "${native_failure_ignored_workflow}"
    "missing fail-closed fragment: $PSNativeCommandUseErrorActionPreference = $true")
