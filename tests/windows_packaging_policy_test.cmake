foreach(required_file
        "${AEGISY_SOURCE_DIR}/installer.iss"
        "${AEGISY_SOURCE_DIR}/.github/workflows/windows-package.yml"
        "${AEGISY_SOURCE_DIR}/package-windows.bat"
        "${AEGISY_SOURCE_DIR}/include/canonical_path_policy.h"
        "${AEGISY_SOURCE_DIR}/src/artifact_manifest.cpp"
        "${AEGISY_SOURCE_DIR}/src/update_artifact_set.cpp"
        "${AEGISY_SOURCE_DIR}/cmake/windows/AegisyClient.manifest.in"
        "${AEGISY_SOURCE_DIR}/cmake/windows/AegisyClient.rc.in")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Windows packaging policy file is missing: ${required_file}")
    endif()
endforeach()

file(READ "${AEGISY_SOURCE_DIR}/src/artifact_manifest.cpp" artifact_manifest_source)
file(READ "${AEGISY_SOURCE_DIR}/src/update_artifact_set.cpp" artifact_set_source)
foreach(path_consumer artifact_manifest_source artifact_set_source)
    if(NOT "${${path_consumer}}" MATCHES
            "CanonicalPathPolicy::isStrictDescendant")
        message(FATAL_ERROR
            "Windows canonical path consumer bypasses the shared policy: ${path_consumer}")
    endif()
    if("${${path_consumer}}" MATCHES "QDir::separator\\(\\)")
        message(FATAL_ERROR
            "Windows canonical containment reintroduced a native separator: ${path_consumer}")
    endif()
endforeach()

file(READ "${AEGISY_SOURCE_DIR}/installer.iss" installer)
if(NOT installer MATCHES "MinVersion=10\\.0\\.17763")
    message(FATAL_ERROR "Inno Setup script does not enforce Windows 10 1809 minimum")
endif()

file(READ "${AEGISY_SOURCE_DIR}/cmake/windows/AegisyClient.manifest.in" manifest)
foreach(required_fragment
        "requestedExecutionLevel level=\"asInvoker\""
        "longPathAware"
        ">true</longPathAware>")
    if(NOT manifest MATCHES "${required_fragment}")
        message(FATAL_ERROR "Windows application manifest is missing: ${required_fragment}")
    endif()
endforeach()

file(READ "${AEGISY_SOURCE_DIR}/cmake/windows/AegisyClient.rc.in" resource)
if(NOT resource MATCHES "RT_MANIFEST")
    message(FATAL_ERROR "Windows resource script does not embed the application manifest")
endif()

set(unicode_checkout "windows-验证-源码")
set(required_ctest_command
    "ctest --test-dir build -C Release --no-tests=error --output-on-failure")
set(required_ctest_rerun_line
    "$rerunOutput = @(ctest --test-dir build -C Release --rerun-failed --output-on-failure 2>&1)")
set(required_rust_test_line
    "$testOutput = @(cargo test --locked --workspace --manifest-path agent-runtime\\Cargo.toml 2>&1)")
string(CONCAT required_artifact_path
    "path: $" "{{ github.workspace }}/" "${unicode_checkout}"
    "/dist/AegisyClientSetup-*.exe")

function(validate_windows_workflow workflow_text out_errors)
    set(errors)
    string(REPLACE "\r\n" "\n" workflow_text "${workflow_text}")
    string(REPLACE "\r" "\n" workflow_text "${workflow_text}")

    string(FIND "${workflow_text}"
        "  validate:\n    runs-on: windows-2022\n    timeout-minutes: 150\n"
        validate_timeout_offset)
    if(validate_timeout_offset EQUAL -1)
        list(APPEND errors
            "Windows validation job must retain the 150-minute timeout budget")
    endif()

    # Qt 6.8.3 bundles qtdeclarative inside the base win64_msvc2022_64 package;
    # it is not a separate online-installer module and breaks aqtinstall when
    # listed, so it must stay out of the modules list while the four real
    # add-on modules remain required.
    foreach(required_qt_module
            qtpositioning
            qtwebchannel
            qtwebengine
            qtwebsockets)
        if(NOT workflow_text MATCHES "modules:[^\n]*${required_qt_module}")
            list(APPEND errors
                "missing required Qt module: ${required_qt_module}")
        endif()
    endforeach()
    if(workflow_text MATCHES "modules:[^\n]*qtdeclarative")
        list(APPEND errors
            "qtdeclarative must not be listed as a module: it ships inside the Qt 6.8.3 base package")
    endif()

    foreach(required_unicode_fragment
            "working-directory: ${unicode_checkout}"
            "path: ${unicode_checkout}"
            "Verify clean Unicode checkout"
            "Validated clean Unicode checkout")
        string(FIND "${workflow_text}" "${required_unicode_fragment}" fragment_offset)
        if(fragment_offset EQUAL -1)
            list(APPEND errors
                "missing Unicode-checkout gate: ${required_unicode_fragment}")
        endif()
    endforeach()

    foreach(required_trigger
            ".gitattributes"
            "CMakeLists.txt"
            "deny.toml"
            "cmake/**"
            "include/**"
            "src/**"
            "tests/**"
            "workbench-web/**"
            "resources.qrc"
            "assets/**"
            "installer.iss"
            "package-windows.bat"
            "agent-runtime/**"
            ".github/workflows/windows-package.yml"
            "release/generate-windows-appcast.ps1"
            "release/smoke-test-windows-runtime.ps1"
            "release/verify-windows-tls-runtime.ps1"
            "release/notes/*-windows.md")
        string(FIND "${workflow_text}" "      - ${required_trigger}\n" trigger_offset)
        if(trigger_offset EQUAL -1)
            list(APPEND errors
                "missing validation trigger: ${required_trigger}")
        endif()
    endforeach()

    foreach(required_complete_gate
            "cmake --build build --config Release"
            "cargo fmt --all --manifest-path agent-runtime\\Cargo.toml -- --check"
            "cargo test --locked --workspace --manifest-path agent-runtime\\Cargo.toml"
            "cargo clippy --locked --workspace --all-targets --manifest-path agent-runtime\\Cargo.toml -- -D warnings"
            "cargo build --locked --workspace --manifest-path agent-runtime\\Cargo.toml --release"
            "cargo package --locked --offline --manifest-path agent-runtime\\Cargo.toml -p aegisy-aap --allow-dirty")
        string(FIND "${workflow_text}" "${required_complete_gate}" gate_offset)
        if(gate_offset EQUAL -1)
            list(APPEND errors
            "missing complete desktop gate: ${required_complete_gate}")
        endif()
    endforeach()

    foreach(required_rust_step
            "Setup Windows agent runtime toolchain"
            "Format Windows agent runtime"
            "Test Windows agent runtime"
            "Lint Windows agent runtime"
            "Build Windows agent runtime"
            "Package Windows AAP schema"
            "Audit Windows agent runtime dependencies"
            "Windows Rust test failure"
            "no bounded failure summary was found")
        string(FIND "${workflow_text}" "${required_rust_step}" rust_step_offset)
        if(rust_step_offset EQUAL -1)
            list(APPEND errors
                "missing Windows Rust diagnostic gate: ${required_rust_step}")
        endif()
    endforeach()

    string(REGEX MATCHALL
        "[^\r\n]*\\$testOutput = @\\(cargo test[^\r\n]*"
        rust_test_lines
        "${workflow_text}")
    list(LENGTH rust_test_lines rust_test_line_count)
    if(NOT rust_test_line_count EQUAL 1)
        list(APPEND errors
            "expected exactly one complete Windows Rust test command, found ${rust_test_line_count}")
    else()
        list(GET rust_test_lines 0 rust_test_line)
        string(STRIP "${rust_test_line}" rust_test_line)
        if(NOT rust_test_line STREQUAL required_rust_test_line)
            list(APPEND errors
                "Windows Rust test command must be exactly: ${required_rust_test_line}")
        endif()
    endif()

    string(REGEX MATCHALL "[^\r\n]*ctest[^\r\n]*" ctest_lines "${workflow_text}")
    list(LENGTH ctest_lines ctest_line_count)
    if(NOT ctest_line_count EQUAL 2)
        list(APPEND errors
            "expected the complete CTest command and one failed-set rerun, found ${ctest_line_count}")
    else()
        list(GET ctest_lines 0 first_ctest_line)
        list(GET ctest_lines 1 second_ctest_line)
        string(STRIP "${first_ctest_line}" first_ctest_line)
        string(STRIP "${second_ctest_line}" second_ctest_line)
        if(NOT first_ctest_line STREQUAL required_ctest_command)
            list(APPEND errors
                "CTest command must be exactly: ${required_ctest_command}")
        endif()
        if(NOT second_ctest_line STREQUAL required_ctest_rerun_line)
            list(APPEND errors
                "CTest failed-set rerun must be exactly: ${required_ctest_rerun_line}")
        endif()
    endif()

    foreach(required_artifact_fragment
            "uses: actions/upload-artifact@v4"
            "${required_artifact_path}"
            "if-no-files-found: error")
        string(FIND "${workflow_text}" "${required_artifact_fragment}" artifact_offset)
        if(artifact_offset EQUAL -1)
            list(APPEND errors
                "missing Unicode-checkout artifact gate: ${required_artifact_fragment}")
        endif()
    endforeach()

    set(${out_errors} "${errors}" PARENT_SCOPE)
endfunction()

function(expect_workflow_rejection case_name workflow_text expected_error)
    validate_windows_workflow("${workflow_text}" actual_errors)
    string(JOIN "\n" actual_error_text ${actual_errors})
    string(FIND "${actual_error_text}" "${expected_error}" expected_error_offset)
    if(expected_error_offset EQUAL -1)
        message(FATAL_ERROR
            "Windows workflow negative case '${case_name}' was not rejected as expected. "
            "Expected '${expected_error}', got '${actual_error_text}'")
    endif()
endfunction()

file(READ "${AEGISY_SOURCE_DIR}/.github/workflows/windows-package.yml" workflow)
validate_windows_workflow("${workflow}" workflow_errors)
if(workflow_errors)
    string(JOIN "\n" workflow_error_text ${workflow_errors})
    message(FATAL_ERROR "Windows workflow policy failed:\n${workflow_error_text}")
endif()

set(crlf_workflow "${workflow}")
string(REPLACE "\r\n" "\n" crlf_workflow "${crlf_workflow}")
string(REPLACE "\r" "\n" crlf_workflow "${crlf_workflow}")
string(REPLACE "\n" "\r\n" crlf_workflow "${crlf_workflow}")
validate_windows_workflow("${crlf_workflow}" crlf_workflow_errors)
if(crlf_workflow_errors)
    string(JOIN "\n" crlf_workflow_error_text ${crlf_workflow_errors})
    message(FATAL_ERROR
        "Windows workflow policy is not CRLF-safe:\n${crlf_workflow_error_text}")
endif()

set(missing_trigger_workflow "${workflow}")
string(REPLACE "      - agent-runtime/**\n" ""
    missing_trigger_workflow "${missing_trigger_workflow}")
expect_workflow_rejection(
    "missing-agent-runtime-trigger"
    "${missing_trigger_workflow}"
    "missing validation trigger: agent-runtime/**")

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

set(filtered_ctest_rerun_workflow "${workflow}")
string(REPLACE
    "${required_ctest_rerun_line}"
    "$rerunOutput = @(ctest --test-dir build -C Release --rerun-failed --output-on-failure -R agent_workbench_render 2>&1)"
    filtered_ctest_rerun_workflow
    "${filtered_ctest_rerun_workflow}")
expect_workflow_rejection(
    "filtered-ctest-rerun"
    "${filtered_ctest_rerun_workflow}"
    "CTest failed-set rerun must be exactly: ${required_ctest_rerun_line}")

set(filtered_rust_test_workflow "${workflow}")
string(REPLACE
    "${required_rust_test_line}"
    "$testOutput = @(cargo test --locked --workspace --manifest-path agent-runtime\\Cargo.toml -p aegisy-aap 2>&1)"
    filtered_rust_test_workflow
    "${filtered_rust_test_workflow}")
expect_workflow_rejection(
    "filtered-rust-test"
    "${filtered_rust_test_workflow}"
    "Windows Rust test command must be exactly: ${required_rust_test_line}")

set(merged_rust_test_step_workflow "${workflow}")
string(REPLACE
    "Test Windows agent runtime"
    "Verify Windows agent runtime"
    merged_rust_test_step_workflow
    "${merged_rust_test_step_workflow}")
expect_workflow_rejection(
    "merged-rust-test-step"
    "${merged_rust_test_step_workflow}"
    "missing Windows Rust diagnostic gate: Test Windows agent runtime")

set(short_timeout_workflow "${workflow}")
string(REPLACE
    "    timeout-minutes: 150\n"
    "    timeout-minutes: 90\n"
    short_timeout_workflow
    "${short_timeout_workflow}")
expect_workflow_rejection(
    "short-validation-timeout"
    "${short_timeout_workflow}"
    "Windows validation job must retain the 150-minute timeout budget")

set(relative_artifact_workflow "${workflow}")
string(REPLACE
    "${required_artifact_path}"
    "path: dist/AegisyClientSetup-*.exe"
    relative_artifact_workflow
    "${relative_artifact_workflow}")
expect_workflow_rejection(
    "relative-artifact-path"
    "${relative_artifact_workflow}"
    "missing Unicode-checkout artifact gate: ${required_artifact_path}")

file(READ "${AEGISY_SOURCE_DIR}/CMakeLists.txt" cmake_source)
string(FIND "${cmake_source}" "option(AEGISY_REQUIRE_QT6" require_qt6_option)
if(require_qt6_option EQUAL -1)
    message(FATAL_ERROR "CMake does not expose the explicit Qt 6 release gate")
endif()
string(FIND "${cmake_source}" "llvm-rc" llvm_rc_fallback)
if(llvm_rc_fallback EQUAL -1)
    message(FATAL_ERROR
        "CMake does not provide the llvm-rc fallback for non-ASCII Windows checkouts")
endif()
foreach(required_qt_component
        Core
        Widgets
        Network
        Sql
        WebSockets
        WebChannel
        WebEngineWidgets)
    if(NOT cmake_source MATCHES "${required_qt_component}")
        message(FATAL_ERROR
            "Qt 6 release gate does not require component: ${required_qt_component}")
    endif()
endforeach()
if(NOT cmake_source MATCHES "Qt6 REQUIRED COMPONENTS")
    message(FATAL_ERROR "Qt 6 release gate does not fail closed on missing components")
endif()
foreach(required_release_test
        artifact_manifest_runtime_startup
        update_signing_key_ring_contract
        update_signing_key_ring_cache_integrity
        update_artifact_set_compatibility
        update_progress_record_integrity)
    string(FIND "${cmake_source}" "add_test(NAME ${required_release_test}"
        required_release_test_offset)
    if(required_release_test_offset EQUAL -1)
        message(FATAL_ERROR
            "CMake does not register required release test: ${required_release_test}")
    endif()
endforeach()

file(READ "${AEGISY_SOURCE_DIR}/package-windows.bat" package_script)
foreach(release_source workflow package_script)
    string(FIND "${${release_source}}" "AEGISY_REQUIRE_QT6=ON" require_qt6_enabled)
    if(require_qt6_enabled EQUAL -1)
        message(FATAL_ERROR
            "Windows release path does not enable the complete Qt 6 gate: ${release_source}")
    endif()
endforeach()
message(STATUS
    "Windows packaging policy includes OS, manifest, Unicode, complete Qt SDK, full CTest, artifact-set, key-ring continuity, update-record, trigger, and artifact gates")
