foreach(required IN ITEMS AEGISY_SOURCE_DIR AEGISY_BINARY_DIR NODE_EXECUTABLE CARGO_EXECUTABLE CPP_EXECUTABLE)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

set(schema_root "${AEGISY_SOURCE_DIR}/agent-runtime/aap-schema")
set(schema "${schema_root}/stable/v0.1/aap.schema.json")
set(method_registry "${schema_root}/fixtures/aap-transport-methods.json")
set(fixture "${schema_root}/fixtures/aap-transport-definitions.fixture-map.json")
set(corpus "${schema_root}/fixtures/aap-transport-validation-corpus.json")
set(expected_fixture_identity
    "30050 af1c71890f5eaf43914915ab88adb10186e15765ba830f1efdafee2efa04e20d")
set(expected_corpus_identity
    "72 f0ce6bdc14c815b2b80b273126da8b20a80ec47371d39128c7e2155246f60404")

execute_process(
    COMMAND "${NODE_EXECUTABLE}" "${schema_root}/scripts/test-transport-generator-inputs.mjs"
    RESULT_VARIABLE generator_test_result
    OUTPUT_VARIABLE generator_test_output
    ERROR_VARIABLE generator_test_error)
if(NOT generator_test_result EQUAL 0)
    message(FATAL_ERROR
        "AAP Transport generator input tests failed: ${generator_test_output}${generator_test_error}")
endif()

execute_process(
    COMMAND "${NODE_EXECUTABLE}" "${schema_root}/scripts/generate-transport-types.mjs" --check
    RESULT_VARIABLE generation_result
    OUTPUT_VARIABLE generation_output
    ERROR_VARIABLE generation_error)
if(NOT generation_result EQUAL 0)
    message(FATAL_ERROR
        "AAP Transport generated outputs are stale: ${generation_output}${generation_error}")
endif()

execute_process(
    COMMAND "${NODE_EXECUTABLE}" "${schema_root}/scripts/verify-package-inventory.mjs"
    WORKING_DIRECTORY "${schema_root}"
    RESULT_VARIABLE package_inventory_result
    OUTPUT_VARIABLE package_inventory_output
    ERROR_VARIABLE package_inventory_error)
if(NOT package_inventory_result EQUAL 0)
    message(FATAL_ERROR
        "AAP Schema npm package inventory failed: ${package_inventory_output}${package_inventory_error}")
endif()

execute_process(
    COMMAND "${NODE_EXECUTABLE}" "${schema_root}/scripts/run-transport-corpus.mjs"
        "${schema}" "${corpus}"
    RESULT_VARIABLE oracle_corpus_result
    OUTPUT_VARIABLE oracle_corpus_identity
    ERROR_VARIABLE oracle_corpus_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT oracle_corpus_result EQUAL 0)
    message(FATAL_ERROR "AAP Transport oracle corpus failed: ${oracle_corpus_error}")
endif()

execute_process(
    COMMAND "${NODE_EXECUTABLE}" "${schema_root}/scripts/materialize-transport-fixtures.mjs"
        "${schema}" "${method_registry}" "${fixture}"
    RESULT_VARIABLE materialization_result
    OUTPUT_VARIABLE materialized_fixture_identity
    ERROR_VARIABLE materialization_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT materialization_result EQUAL 0)
    message(FATAL_ERROR "AAP Transport fixture materialization failed: ${materialization_error}")
endif()

execute_process(
    COMMAND "${NODE_EXECUTABLE}" "${schema_root}/scripts/run-generated-transport-types.mjs"
        "${fixture}"
    RESULT_VARIABLE typescript_result
    OUTPUT_VARIABLE typescript_identity
    ERROR_VARIABLE typescript_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT typescript_result EQUAL 0)
    message(FATAL_ERROR "TypeScript Transport fixture validation failed: ${typescript_error}")
endif()

execute_process(
    COMMAND "${NODE_EXECUTABLE}" "${schema_root}/scripts/run-generated-transport-types.mjs"
        --corpus "${corpus}"
    RESULT_VARIABLE typescript_corpus_result
    OUTPUT_VARIABLE typescript_corpus_identity
    ERROR_VARIABLE typescript_corpus_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT typescript_corpus_result EQUAL 0)
    message(FATAL_ERROR "TypeScript Transport corpus failed: ${typescript_corpus_error}")
endif()

execute_process(
    COMMAND "${CARGO_EXECUTABLE}" run --quiet
        --manifest-path "${AEGISY_SOURCE_DIR}/agent-runtime/Cargo.toml"
        -p aegisy-aap --example aap_transport_fixture -- "${fixture}"
    RESULT_VARIABLE rust_result
    OUTPUT_VARIABLE rust_identity
    ERROR_VARIABLE rust_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT rust_result EQUAL 0)
    message(FATAL_ERROR "Rust Transport fixture validation failed: ${rust_error}")
endif()

execute_process(
    COMMAND "${CARGO_EXECUTABLE}" run --quiet
        --manifest-path "${AEGISY_SOURCE_DIR}/agent-runtime/Cargo.toml"
        -p aegisy-aap --example aap_transport_fixture -- --corpus "${corpus}"
    RESULT_VARIABLE rust_corpus_result
    OUTPUT_VARIABLE rust_corpus_identity
    ERROR_VARIABLE rust_corpus_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT rust_corpus_result EQUAL 0)
    message(FATAL_ERROR "Rust Transport corpus failed: ${rust_corpus_error}")
endif()

execute_process(
    COMMAND "${CPP_EXECUTABLE}" "${fixture}"
    RESULT_VARIABLE cpp_result
    OUTPUT_VARIABLE cpp_identity
    ERROR_VARIABLE cpp_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT cpp_result EQUAL 0)
    message(FATAL_ERROR "C++ Transport fixture validation failed: ${cpp_error}")
endif()

execute_process(
    COMMAND "${CPP_EXECUTABLE}" --corpus "${corpus}"
    RESULT_VARIABLE cpp_corpus_result
    OUTPUT_VARIABLE cpp_corpus_identity
    ERROR_VARIABLE cpp_corpus_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT cpp_corpus_result EQUAL 0)
    message(FATAL_ERROR "C++ Transport corpus failed: ${cpp_corpus_error}")
endif()

foreach(identity IN ITEMS
        materialized_fixture_identity typescript_identity rust_identity cpp_identity
        oracle_corpus_identity typescript_corpus_identity rust_corpus_identity cpp_corpus_identity)
    if(NOT "${${identity}}" MATCHES "^[1-9][0-9]* [0-9a-f]+$")
        message(FATAL_ERROR "${identity} is not a bounded Transport identity")
    endif()
    string(REGEX REPLACE "^[0-9]+ " "" identity_digest "${${identity}}")
    string(LENGTH "${identity_digest}" identity_digest_length)
    if(NOT identity_digest_length EQUAL 64)
        message(FATAL_ERROR "${identity} does not contain a SHA-256 digest")
    endif()
endforeach()

foreach(identity IN ITEMS materialized_fixture_identity typescript_identity rust_identity cpp_identity)
    if(NOT "${${identity}}" STREQUAL "${expected_fixture_identity}")
        message(FATAL_ERROR
            "AAP Transport fixture identity differs: ${identity}=${${identity}}, expected=${expected_fixture_identity}")
    endif()
endforeach()

foreach(identity IN ITEMS oracle_corpus_identity typescript_corpus_identity rust_corpus_identity cpp_corpus_identity)
    if(NOT "${${identity}}" STREQUAL "${expected_corpus_identity}")
        message(FATAL_ERROR
            "AAP Transport corpus identity differs: ${identity}=${${identity}}, expected=${expected_corpus_identity}")
    endif()
endforeach()

message(STATUS "AAP Transport generated fixture identity: ${expected_fixture_identity}")
message(STATUS "AAP Transport generated corpus identity: ${expected_corpus_identity}")
