if(NOT DEFINED AEGISY_SOURCE_DIR)
    message(FATAL_ERROR "AEGISY_SOURCE_DIR must point to the repository root")
endif()

set(required_build_command "cmake --build build -j4")
set(required_ctest_command
    "ctest --test-dir build --no-tests=error --output-on-failure")

function(validate_macos_workflow workflow_text out_errors)
    set(errors)
    string(REPLACE "\r\n" "\n" workflow_text "${workflow_text}")
    string(REPLACE "\r" "\n" workflow_text "${workflow_text}")

    string(REGEX MATCHALL "[^\n]*cmake --build[^\n]*" build_lines
        "${workflow_text}")
    list(LENGTH build_lines build_line_count)
    if(NOT build_line_count EQUAL 1)
        list(APPEND errors
            "expected exactly one CMake build command, found ${build_line_count}")
    else()
        list(GET build_lines 0 build_line)
        string(STRIP "${build_line}" build_line)
        if(NOT build_line STREQUAL required_build_command)
            list(APPEND errors
                "build command must be exactly: ${required_build_command}")
        endif()
        if(build_line MATCHES "(^|[ \\t])--target([ \\t]|$)")
            list(APPEND errors
                "macOS CI must build the complete target graph without --target")
        endif()
    endif()

    string(REGEX MATCHALL "[^\n]*ctest[^\n]*" ctest_lines
        "${workflow_text}")
    list(LENGTH ctest_lines ctest_line_count)
    if(NOT ctest_line_count EQUAL 1)
        list(APPEND errors
            "expected exactly one CTest command, found ${ctest_line_count}")
    else()
        list(GET ctest_lines 0 ctest_line)
        string(STRIP "${ctest_line}" ctest_line)
        if(NOT ctest_line STREQUAL required_ctest_command)
            list(APPEND errors
                "CTest command must be exactly: ${required_ctest_command}")
        endif()
    endif()

    set(${out_errors} "${errors}" PARENT_SCOPE)
endfunction()

function(expect_workflow_rejection case_name workflow_text expected_error)
    validate_macos_workflow("${workflow_text}" actual_errors)
    string(JOIN "\n" actual_error_text ${actual_errors})
    string(FIND "${actual_error_text}" "${expected_error}" expected_error_offset)
    if(expected_error_offset EQUAL -1)
        message(FATAL_ERROR
            "macOS workflow negative case '${case_name}' was not rejected as expected. "
            "Expected '${expected_error}', got '${actual_error_text}'")
    endif()
endfunction()

set(workflow_path
    "${AEGISY_SOURCE_DIR}/.github/workflows/macos-build.yml")
if(NOT EXISTS "${workflow_path}")
    message(FATAL_ERROR "macOS workflow is missing: ${workflow_path}")
endif()

file(READ "${workflow_path}" workflow)
validate_macos_workflow("${workflow}" workflow_errors)
if(workflow_errors)
    string(JOIN "\n" workflow_error_text ${workflow_errors})
    message(FATAL_ERROR "macOS workflow policy failed:\n${workflow_error_text}")
endif()

set(crlf_workflow "${workflow}")
string(REPLACE "\r\n" "\n" crlf_workflow "${crlf_workflow}")
string(REPLACE "\r" "\n" crlf_workflow "${crlf_workflow}")
string(REPLACE "\n" "\r\n" crlf_workflow "${crlf_workflow}")
validate_macos_workflow("${crlf_workflow}" crlf_workflow_errors)
if(crlf_workflow_errors)
    string(JOIN "\n" crlf_workflow_error_text ${crlf_workflow_errors})
    message(FATAL_ERROR
        "macOS workflow policy is not CRLF-safe:\n${crlf_workflow_error_text}")
endif()

set(target_only_workflow "${workflow}")
string(REPLACE
    "${required_build_command}"
    "cmake --build build --target AegisyClient -j4"
    target_only_workflow
    "${target_only_workflow}")
expect_workflow_rejection(
    "target-only-build"
    "${target_only_workflow}"
    "macOS CI must build the complete target graph without --target")

set(filtered_ctest_workflow "${workflow}")
string(REPLACE
    "${required_ctest_command}"
    "${required_ctest_command} -R agent_runtime_environment"
    filtered_ctest_workflow
    "${filtered_ctest_workflow}")
expect_workflow_rejection(
    "filtered-ctest"
    "${filtered_ctest_workflow}"
    "CTest command must be exactly: ${required_ctest_command}")

message(STATUS
    "macOS workflow policy requires a complete build before unfiltered CTest")
