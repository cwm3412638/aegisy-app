foreach(required_value AEGISY_SOURCE_DIR AEGISY_BINARY_DIR AEGISY_QT6_DIR)
    if(NOT DEFINED ${required_value} OR "${${required_value}}" STREQUAL "")
        message(FATAL_ERROR "Missing required test value: ${required_value}")
    endif()
endforeach()

set(fixture_source "${AEGISY_SOURCE_DIR}/tests/qt6_release_gate_fixture")
set(fixture_build "${AEGISY_BINARY_DIR}/qt6-release-policy-missing-webengine")
file(REMOVE_RECURSE "${fixture_build}")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -S "${fixture_source}"
            -B "${fixture_build}"
            "-DQt6_DIR=${AEGISY_QT6_DIR}"
            -DCMAKE_DISABLE_FIND_PACKAGE_Qt6WebEngineWidgets=TRUE
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
    TIMEOUT 60)

if(configure_result EQUAL 0)
    message(FATAL_ERROR
        "Qt 6 release fixture configured without required WebEngineWidgets")
endif()

set(configure_output "${configure_stdout}\n${configure_stderr}")
string(FIND "${configure_output}" "Qt6WebEngineWidgets" missing_component_diagnostic)
if(missing_component_diagnostic EQUAL -1)
    message(FATAL_ERROR
        "Qt 6 release fixture failed without the missing WebEngineWidgets diagnostic")
endif()

message(STATUS "Qt 6 release gate rejects a missing WebEngineWidgets component")
