foreach(required IN ITEMS AEGISY_SOURCE_DIR AEGISY_BINARY_DIR NODE_EXECUTABLE CARGO_EXECUTABLE CPP_EXECUTABLE)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

set(schema_root "${AEGISY_SOURCE_DIR}/agent-runtime/aap-schema")
set(fixture "${schema_root}/fixtures/aap-core-domains.json")
set(corpus "${schema_root}/fixtures/aap-core-generated-corpus.json")
set(materialized_corpus "${AEGISY_BINARY_DIR}/aap-core-generated-corpus.materialized.json")

execute_process(
    COMMAND "${NODE_EXECUTABLE}" "${schema_root}/scripts/test-core-generator-inputs.mjs"
    RESULT_VARIABLE generator_test_result
    OUTPUT_VARIABLE generator_test_output
    ERROR_VARIABLE generator_test_error)
if(NOT generator_test_result EQUAL 0)
    message(FATAL_ERROR "AAP generator input tests failed: ${generator_test_output}${generator_test_error}")
endif()

execute_process(
    COMMAND "${NODE_EXECUTABLE}" "${schema_root}/scripts/generate-core-types.mjs" --check
    RESULT_VARIABLE generation_result
    OUTPUT_VARIABLE generation_output
    ERROR_VARIABLE generation_error)
if(NOT generation_result EQUAL 0)
    message(FATAL_ERROR "AAP generated outputs are stale: ${generation_output}${generation_error}")
endif()

execute_process(
    COMMAND "${NODE_EXECUTABLE}" "${schema_root}/scripts/emit-core-fixture.mjs" "${fixture}"
    RESULT_VARIABLE typescript_result
    OUTPUT_VARIABLE typescript_identity
    ERROR_VARIABLE typescript_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT typescript_result EQUAL 0)
    message(FATAL_ERROR "TypeScript fixture serialization failed: ${typescript_error}")
endif()

execute_process(
    COMMAND "${CPP_EXECUTABLE}" "${fixture}"
    RESULT_VARIABLE cpp_result
    OUTPUT_VARIABLE cpp_identity
    ERROR_VARIABLE cpp_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT cpp_result EQUAL 0)
    message(FATAL_ERROR "C++ fixture serialization failed: ${cpp_error}")
endif()

execute_process(
    COMMAND "${CARGO_EXECUTABLE}" run --quiet
        --manifest-path "${AEGISY_SOURCE_DIR}/agent-runtime/Cargo.toml"
        -p aegisy-aap --example aap_core_fixture -- "${fixture}"
    RESULT_VARIABLE rust_result
    OUTPUT_VARIABLE rust_identity
    ERROR_VARIABLE rust_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT rust_result EQUAL 0)
    message(FATAL_ERROR "Rust fixture serialization failed: ${rust_error}")
endif()

foreach(identity IN ITEMS typescript_identity cpp_identity rust_identity)
    if(NOT "${${identity}}" MATCHES "^(0|[1-9][0-9]*) [0-9a-f]+$")
        message(FATAL_ERROR "${identity} is not a bounded canonical fixture identity")
    endif()
    string(REGEX REPLACE "^[0-9]+ " "" identity_digest "${${identity}}")
    string(LENGTH "${identity_digest}" identity_digest_length)
    if(NOT identity_digest_length EQUAL 64)
        message(FATAL_ERROR "${identity} does not contain a SHA-256 digest")
    endif()
endforeach()

if(NOT typescript_identity STREQUAL cpp_identity OR NOT typescript_identity STREQUAL rust_identity)
    message(FATAL_ERROR
        "AAP fixture bytes differ: TypeScript=${typescript_identity}, C++=${cpp_identity}, Rust=${rust_identity}")
endif()

message(STATUS "AAP generated fixture identity: ${typescript_identity}")

execute_process(
    COMMAND "${NODE_EXECUTABLE}" "${schema_root}/scripts/materialize-core-corpus.mjs"
        "${corpus}" "${fixture}" "${materialized_corpus}"
    RESULT_VARIABLE corpus_materialization_result
    OUTPUT_VARIABLE corpus_materialization_output
    ERROR_VARIABLE corpus_materialization_error)
if(NOT corpus_materialization_result EQUAL 0)
    message(FATAL_ERROR
        "AAP core corpus materialization failed: ${corpus_materialization_output}${corpus_materialization_error}")
endif()

execute_process(
    COMMAND "${NODE_EXECUTABLE}" "${schema_root}/scripts/emit-core-fixture.mjs"
        --corpus "${materialized_corpus}"
    RESULT_VARIABLE typescript_corpus_result
    OUTPUT_VARIABLE typescript_corpus_identity
    ERROR_VARIABLE typescript_corpus_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT typescript_corpus_result EQUAL 0)
    message(FATAL_ERROR "TypeScript core corpus failed: ${typescript_corpus_error}")
endif()

execute_process(
    COMMAND "${CPP_EXECUTABLE}" --corpus "${materialized_corpus}"
    RESULT_VARIABLE cpp_corpus_result
    OUTPUT_VARIABLE cpp_corpus_identity
    ERROR_VARIABLE cpp_corpus_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT cpp_corpus_result EQUAL 0)
    message(FATAL_ERROR "C++ core corpus failed: ${cpp_corpus_error}")
endif()

execute_process(
    COMMAND "${CARGO_EXECUTABLE}" run --quiet
        --manifest-path "${AEGISY_SOURCE_DIR}/agent-runtime/Cargo.toml"
        -p aegisy-aap --example aap_core_fixture -- --corpus "${materialized_corpus}"
    RESULT_VARIABLE rust_corpus_result
    OUTPUT_VARIABLE rust_corpus_identity
    ERROR_VARIABLE rust_corpus_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT rust_corpus_result EQUAL 0)
    message(FATAL_ERROR "Rust core corpus failed: ${rust_corpus_error}")
endif()

foreach(identity IN ITEMS typescript_corpus_identity cpp_corpus_identity rust_corpus_identity)
    if(NOT "${${identity}}" MATCHES "^[1-9][0-9]* [0-9a-f]+$")
        message(FATAL_ERROR "${identity} is not a bounded core corpus identity")
    endif()
    string(REGEX REPLACE "^[0-9]+ " "" identity_digest "${${identity}}")
    string(LENGTH "${identity_digest}" identity_digest_length)
    if(NOT identity_digest_length EQUAL 64)
        message(FATAL_ERROR "${identity} does not contain a SHA-256 digest")
    endif()
endforeach()

if(NOT typescript_corpus_identity STREQUAL cpp_corpus_identity
        OR NOT typescript_corpus_identity STREQUAL rust_corpus_identity)
    message(FATAL_ERROR
        "AAP corpus decisions differ: TypeScript=${typescript_corpus_identity}, "
        "C++=${cpp_corpus_identity}, Rust=${rust_corpus_identity}")
endif()

message(STATUS "AAP generated corpus identity: ${typescript_corpus_identity}")
