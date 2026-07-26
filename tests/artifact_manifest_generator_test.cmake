set(fixture "${AEGISY_BINARY_DIR}/artifact-manifest-generator-fixture")
file(REMOVE_RECURSE "${fixture}")
file(MAKE_DIRECTORY "${fixture}")
file(WRITE "${fixture}/aegisy-agentd" "runtime fixture\n")
file(WRITE "${fixture}/codex" "adapter fixture\n")
set(output "${fixture}/aegisy-agentd.manifest.json")
set(generator "${AEGISY_SOURCE_DIR}/cmake/generate_artifact_manifest.cmake")
if(NOT EXISTS "${MANIFEST_VERIFIER}")
    message(FATAL_ERROR "MANIFEST_VERIFIER is unavailable")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DBASE_DIR=${fixture}
        -DOUTPUT=${output}
        -DRUNTIME_PATH=${fixture}/aegisy-agentd
        -DRUNTIME_ID=aegisy-agentd
        -DRUNTIME_VERSION=0.1.0
        -DADAPTER_PATH=${fixture}/codex
        -DADAPTER_ID=codex-app-server
        "-DADAPTER_VERSION=codex-cli 0.144.5"
        -P "${generator}"
    RESULT_VARIABLE first_result
)
if(NOT first_result EQUAL 0)
    message(FATAL_ERROR "manifest generator failed on valid fixture")
endif()
file(SHA256 "${output}" first_hash)
file(READ "${output}" first_content)
file(SHA256 "${fixture}/aegisy-agentd" runtime_hash)
file(SHA256 "${fixture}/codex" adapter_hash)
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DBASE_DIR=${fixture}
        -DOUTPUT=${output}
        -DRUNTIME_PATH=${fixture}/aegisy-agentd
        -DRUNTIME_ID=aegisy-agentd
        -DRUNTIME_VERSION=0.1.0
        -DADAPTER_PATH=${fixture}/codex
        -DADAPTER_ID=codex-app-server
        "-DADAPTER_VERSION=codex-cli 0.144.5"
        -P "${generator}"
    RESULT_VARIABLE second_result
)
if(NOT second_result EQUAL 0)
    message(FATAL_ERROR "manifest generator failed on repeat fixture")
endif()
file(SHA256 "${output}" second_hash)
file(READ "${output}" second_content)
if(NOT first_hash STREQUAL second_hash OR NOT first_content STREQUAL second_content)
    message(FATAL_ERROR "manifest generation is not deterministic")
endif()
if(NOT first_content MATCHES "aegisy-artifact-manifest/0\\.1")
    message(FATAL_ERROR "manifest schema is missing")
endif()
if(NOT first_content MATCHES "${runtime_hash}" OR NOT first_content MATCHES "${adapter_hash}")
    message(FATAL_ERROR "manifest does not hash both bundled artifacts")
endif()
if(NOT first_content MATCHES "\"version\": \"codex-cli 0\\.144\\.5\"")
    message(FATAL_ERROR "manifest adapter version is not exact")
endif()
execute_process(
    COMMAND "${MANIFEST_VERIFIER}" "${output}" "${fixture}/aegisy-agentd"
    RESULT_VARIABLE verification_result
)
if(NOT verification_result EQUAL 0)
    message(FATAL_ERROR "production verifier rejected the generated manifest")
endif()

file(WRITE "${AEGISY_BINARY_DIR}/artifact-manifest-outside" "outside fixture\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DBASE_DIR=${fixture}
        -DOUTPUT=${fixture}/invalid.json
        -DRUNTIME_PATH=${AEGISY_BINARY_DIR}/artifact-manifest-outside
        -DRUNTIME_ID=aegisy-agentd
        -DRUNTIME_VERSION=0.1.0
        -DADAPTER_PATH=${fixture}/codex
        -DADAPTER_ID=codex-app-server
        "-DADAPTER_VERSION=codex-cli 0.144.5"
        -P "${generator}"
    RESULT_VARIABLE invalid_result
)
if(invalid_result EQUAL 0)
    message(FATAL_ERROR "manifest generator accepted an artifact outside BASE_DIR")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DBASE_DIR=${fixture}
        -DOUTPUT=${AEGISY_BINARY_DIR}/artifact-manifest-outside.json
        -DRUNTIME_PATH=${fixture}/aegisy-agentd
        -DRUNTIME_ID=aegisy-agentd
        -DRUNTIME_VERSION=0.1.0
        -DADAPTER_PATH=${fixture}/codex
        -DADAPTER_ID=codex-app-server
        "-DADAPTER_VERSION=codex-cli 0.144.5"
        -P "${generator}"
    RESULT_VARIABLE outside_output_result
)
if(outside_output_result EQUAL 0)
    message(FATAL_ERROR "manifest generator accepted OUTPUT outside BASE_DIR")
endif()
