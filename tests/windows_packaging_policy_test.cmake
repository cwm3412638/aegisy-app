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
string(CONCAT required_artifact_path
    "path: $" "{{ github.workspace }}/" "${unicode_checkout}"
    "/dist/AegisyClientSetup-*.exe")

function(validate_windows_workflow workflow_text out_errors)
    set(errors)
    string(REPLACE "\r\n" "\n" workflow_text "${workflow_text}")
    string(REPLACE "\r" "\n" workflow_text "${workflow_text}")

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
            "cargo package --locked --offline --manifest-path agent-runtime\\Cargo.toml -p aegisy-aap --allow-dirty")
        string(FIND "${workflow_text}" "${required_complete_gate}" gate_offset)
        if(gate_offset EQUAL -1)
            list(APPEND errors
                "missing complete desktop gate: ${required_complete_gate}")
        endif()
    endforeach()

    string(REGEX MATCHALL "[^\r\n]*ctest[^\r\n]*" ctest_lines "${workflow_text}")
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
