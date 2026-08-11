foreach(required_file
        "${AEGISY_SOURCE_DIR}/installer.iss"
        "${AEGISY_SOURCE_DIR}/.github/workflows/windows-package.yml"
        "${AEGISY_SOURCE_DIR}/package-windows.bat"
        "${AEGISY_SOURCE_DIR}/include/canonical_path_policy.h"
        "${AEGISY_SOURCE_DIR}/src/artifact_manifest.cpp"
        "${AEGISY_SOURCE_DIR}/src/update_artifact_set.cpp"
        "${AEGISY_SOURCE_DIR}/src/tool_manager.cpp"
        "${AEGISY_SOURCE_DIR}/tests/tool_manager_runtime_test.cpp"
        "${AEGISY_SOURCE_DIR}/tests/agent_workbench_render_test.cpp"
        "${AEGISY_SOURCE_DIR}/tests/monaco_editor_render_test.cpp"
        "${AEGISY_SOURCE_DIR}/tests/qt_test_failure_sink.h"
        "${AEGISY_SOURCE_DIR}/tests/qt_test_failure_channel_test.cmake"
        "${AEGISY_SOURCE_DIR}/cmake/windows/AegisyClient.manifest.in"
        "${AEGISY_SOURCE_DIR}/cmake/windows/AegisyClient.rc.in")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Windows packaging policy file is missing: ${required_file}")
    endif()
endforeach()

file(READ "${AEGISY_SOURCE_DIR}/CMakeLists.txt" cmake_source)
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

file(READ "${AEGISY_SOURCE_DIR}/tests/qt_test_failure_sink.h" failure_sink_source)
file(READ "${AEGISY_SOURCE_DIR}/tests/qt_test_failure_channel_test.cmake"
    failure_channel_test_source)
file(READ "${AEGISY_SOURCE_DIR}/tests/tool_manager_runtime_test.cpp"
    tool_runtime_source)
file(READ "${AEGISY_SOURCE_DIR}/src/tool_manager.cpp" tool_manager_source)
file(READ "${AEGISY_SOURCE_DIR}/tests/agent_workbench_render_test.cpp"
    agent_render_source)
file(READ "${AEGISY_SOURCE_DIR}/tests/monaco_editor_render_test.cpp"
    monaco_render_source)

set(required_sink_only_failure_codes
    QT_STDERR_CHANNEL_PROBE)
set(required_webengine_failure_codes
    WEBENGINE_GLES2_CONTEXT_CREATE
    WEBENGINE_GLES3_CONTEXT_CREATE
    WEBENGINE_CONTEXT_FATAL)
set(required_d3d11_failure_codes
    QT_D3D11_INITIALIZATION)
set(required_tool_failure_codes
    TOOL_COMMAND_SHAPE
    TOOL_BATCH_EXECUTION
    TOOL_SHIM_RESOLUTION
    TOOL_RUNTIME_REGISTRY
    TOOL_NPM_RESIDUE_FIXTURE
    TOOL_NPM_RESIDUE_SYNC
    TOOL_NPM_RESIDUE_TIMEOUT
    TOOL_NPM_RESIDUE_ASYNC)
set(required_agent_failure_codes
    AWB_DATA_ROOT
    AWB_AAP_HANDSHAKE
    AWB_DURABLE_STORE
    AWB_COMPOSER_READY
    AWB_TIMELINE_TURN
    AWB_MUTATION_ACK
    AWB_RUNTIME_DEGRADATION
    AWB_TIMELINE_VALIDATION
    AWB_TIMELINE_GAP
    AWB_TIMELINE_SUBSCRIPTION
    AWB_TIMELINE_SNAPSHOT
    AWB_PROPOSAL_ARTIFACT
    AWB_PROPOSAL_PROJECTION
    AWB_UI_BASELINE
    AWB_MODEL_RECOVERY
    AWB_TURN_LIFECYCLE
    AWB_PROJECT_SESSION
    AWB_GIT_COMPACTION
    AWB_WORKSPACE_EDIT
    AWB_TERMINAL_CONTEXT
    AWB_CONTEXT_ARTIFACT
    AWB_EDITOR_LSP
    AWB_RESPONSIVE_STATE
    AWB_VISIBLE_STATE
    AWB_SNAPSHOT_SAVE)
set(required_monaco_failure_codes
    MONACO_DATA_ROOT
    MONACO_HOST_CONTROL
    MONACO_RUNTIME_READY
    MONACO_WORKSPACE_FIXTURE
    MONACO_TERMINAL_BRIDGE
    MONACO_EDITOR_LIFECYCLE
    MONACO_D3D11_PRESENTATION
    MONACO_SPLIT_LIFECYCLE
    MONACO_SPLIT_BLANK
    MONACO_SNAPSHOT_SAVE
    MONACO_SPLIT_RESTORE
    MONACO_SECURITY_BOUNDARY)
set(required_qt_failure_codes
    ${required_sink_only_failure_codes}
    ${required_tool_failure_codes}
    ${required_agent_failure_codes}
    ${required_monaco_failure_codes}
    ${required_d3d11_failure_codes}
    ${required_webengine_failure_codes})

function(extract_failure_codes source_variable out_codes)
    set(source_text "${${source_variable}}")
    string(REGEX MATCHALL "FailureCode::[A-Z][A-Z0-9_]*"
        code_literals "${source_text}")
    set(codes)
    foreach(code_literal IN LISTS code_literals)
        string(REPLACE "FailureCode::" "" code "${code_literal}")
        list(APPEND codes "${code}")
    endforeach()
    list(REMOVE_DUPLICATES codes)
    list(SORT codes)
    set(${out_codes} "${codes}" PARENT_SCOPE)
endfunction()

function(validate_qt_failure_channel_policy sink_variable agent_variable monaco_variable
        channel_test_variable cmake_variable out_errors)
    set(errors)
    set(sink_text "${${sink_variable}}")
    set(channel_test_text "${${channel_test_variable}}")
    set(cmake_text "${${cmake_variable}}")

    string(REGEX MATCH
        "enum class FailureCode[ \t\r\n]*\\{[^}]*\\}"
        sink_enum_block "${sink_text}")
    string(REGEX MATCHALL "[A-Z][A-Z0-9_]*[ \t\r\n]*,"
        sink_enum_literals "${sink_enum_block}")
    set(sink_enum_codes)
    foreach(sink_enum_literal IN LISTS sink_enum_literals)
        string(REGEX REPLACE "[ \t\r\n,]" "" sink_enum_code
            "${sink_enum_literal}")
        list(APPEND sink_enum_codes "${sink_enum_code}")
    endforeach()
    list(LENGTH sink_enum_codes sink_enum_code_count)
    set(sorted_sink_enum_codes ${sink_enum_codes})
    list(SORT sorted_sink_enum_codes)
    set(expected_sink_enum_codes ${required_qt_failure_codes})
    list(SORT expected_sink_enum_codes)
    list(LENGTH expected_sink_enum_codes expected_sink_enum_code_count)
    if(NOT sink_enum_code_count EQUAL expected_sink_enum_code_count
            OR NOT "${sorted_sink_enum_codes}" STREQUAL "${expected_sink_enum_codes}")
        list(APPEND errors
            "Qt stderr failure sink enum set drifted: expected '${expected_sink_enum_codes}', got '${sorted_sink_enum_codes}'")
    endif()

    string(FIND "${sink_text}" "failureCodeText(FailureCode code)"
        sink_code_block_start)
    string(FIND "${sink_text}" "inline bool localDiagnosticsEnabled"
        sink_code_block_end)
    set(sink_code_block "")
    if(sink_code_block_start EQUAL -1 OR sink_code_block_end EQUAL -1
            OR sink_code_block_end LESS_EQUAL sink_code_block_start)
        list(APPEND errors "Qt stderr failure sink has no bounded fixed-code mapping")
    else()
        math(EXPR sink_code_block_length
            "${sink_code_block_end} - ${sink_code_block_start}")
        string(SUBSTRING "${sink_text}" ${sink_code_block_start}
            ${sink_code_block_length} sink_code_block)
    endif()
    string(REGEX MATCHALL "\"[A-Z][A-Z0-9_]*\""
        sink_code_literals "${sink_code_block}")
    set(sink_codes)
    foreach(sink_code_literal IN LISTS sink_code_literals)
        string(REPLACE "\"" "" sink_code "${sink_code_literal}")
        list(APPEND sink_codes "${sink_code}")
    endforeach()
    list(REMOVE_DUPLICATES sink_codes)
    list(SORT sink_codes)
    set(expected_sink_codes ${required_qt_failure_codes})
    list(SORT expected_sink_codes)
    if(NOT "${sink_codes}" STREQUAL "${expected_sink_codes}")
        list(APPEND errors
            "Qt stderr failure sink code set drifted: expected '${expected_sink_codes}', got '${sink_codes}'")
    endif()

    string(REGEX MATCHALL "case FailureCode::[A-Z][A-Z0-9_]*"
        sink_case_literals "${sink_code_block}")
    set(sink_case_codes)
    foreach(sink_case_literal IN LISTS sink_case_literals)
        string(REPLACE "case FailureCode::" "" sink_case_code
            "${sink_case_literal}")
        list(APPEND sink_case_codes "${sink_case_code}")
    endforeach()
    list(LENGTH sink_case_codes sink_case_code_count)
    set(sorted_sink_case_codes ${sink_case_codes})
    list(SORT sorted_sink_case_codes)
    if(NOT sink_case_code_count EQUAL expected_sink_enum_code_count
            OR NOT "${sorted_sink_case_codes}" STREQUAL "${expected_sink_enum_codes}")
        list(APPEND errors
            "Qt stderr failure sink case set drifted: expected '${expected_sink_enum_codes}', got '${sorted_sink_case_codes}'")
    endif()

    foreach(expected_failure_code IN LISTS required_qt_failure_codes)
        set(case_marker "case FailureCode::${expected_failure_code}")
        string(FIND "${sink_code_block}" "${case_marker}" case_offset)
        if(case_offset EQUAL -1)
            list(APPEND errors
                "Qt stderr failure sink has no mapping for ${expected_failure_code}")
            continue()
        endif()
        string(SUBSTRING "${sink_code_block}" ${case_offset} -1 case_tail)
        string(LENGTH "${case_marker}" case_marker_length)
        string(SUBSTRING "${case_tail}" ${case_marker_length} -1 case_body_tail)
        string(FIND "${case_body_tail}" "case FailureCode::" next_case_offset)
        if(next_case_offset EQUAL -1)
            set(case_body "${case_body_tail}")
        else()
            string(SUBSTRING "${case_body_tail}" 0 ${next_case_offset} case_body)
        endif()
        string(REGEX MATCHALL "\"[A-Z][A-Z0-9_]*\""
            case_code_literals "${case_body}")
        list(LENGTH case_code_literals case_code_literal_count)
        set(expected_case_literal "\"${expected_failure_code}\"")
        if(NOT case_code_literal_count EQUAL 1)
            list(APPEND errors
                "Qt stderr failure sink mapping for ${expected_failure_code} is not one-to-one")
        else()
            list(GET case_code_literals 0 case_code_literal)
            if(NOT case_code_literal STREQUAL expected_case_literal)
                list(APPEND errors
                    "Qt stderr failure sink mapping aliases ${expected_failure_code} to ${case_code_literal}")
            endif()
        endif()
    endforeach()

    string(FIND "${sink_text}" "constexpr CodeExpectation codeExpectations[]"
        self_test_block_start)
    string(FIND "${sink_text}" "for (const CodeExpectation &expectation"
        self_test_block_end)
    set(self_test_block "")
    if(self_test_block_start EQUAL -1 OR self_test_block_end EQUAL -1
            OR self_test_block_end LESS_EQUAL self_test_block_start)
        list(APPEND errors "Qt stderr failure sink has no fixed-code self-test table")
    else()
        math(EXPR self_test_block_length
            "${self_test_block_end} - ${self_test_block_start}")
        string(SUBSTRING "${sink_text}" ${self_test_block_start}
            ${self_test_block_length} self_test_block)
    endif()
    string(REGEX MATCHALL "FailureCode::[A-Z][A-Z0-9_]*"
        self_test_code_literals "${self_test_block}")
    list(LENGTH self_test_code_literals self_test_code_count)
    extract_failure_codes(self_test_block self_test_codes)
    if(NOT self_test_code_count EQUAL expected_sink_enum_code_count
            OR NOT "${self_test_codes}" STREQUAL "${expected_sink_enum_codes}")
        list(APPEND errors
            "Qt stderr failure sink self-test set drifted: expected '${expected_sink_enum_codes}', got '${self_test_codes}'")
    endif()
    foreach(expected_failure_code IN LISTS required_qt_failure_codes)
        string(REGEX MATCH
            "\\{FailureCode::${expected_failure_code},[ \\t\\r\\n]*\"${expected_failure_code}\"\\}"
            self_test_pair "${self_test_block}")
        if(self_test_pair STREQUAL "")
            list(APPEND errors
                "Qt stderr failure sink self-test mapping is missing: ${expected_failure_code}")
        endif()
    endforeach()

    set(agent_text "${${agent_variable}}")
    set(monaco_text "${${monaco_variable}}")
    extract_failure_codes(agent_text agent_failure_codes)
    extract_failure_codes(monaco_text monaco_failure_codes)
    set(expected_agent_failure_codes ${required_agent_failure_codes})
    set(expected_monaco_failure_codes ${required_monaco_failure_codes})
    list(SORT expected_agent_failure_codes)
    list(SORT expected_monaco_failure_codes)
    if(NOT "${agent_failure_codes}" STREQUAL "${expected_agent_failure_codes}")
        list(APPEND errors
            "Agent Workbench fixed-code set drifted: expected '${expected_agent_failure_codes}', got '${agent_failure_codes}'")
    endif()
    if(NOT "${monaco_failure_codes}" STREQUAL "${expected_monaco_failure_codes}")
        list(APPEND errors
            "Monaco fixed-code set drifted: expected '${expected_monaco_failure_codes}', got '${monaco_failure_codes}'")
    endif()

    foreach(forbidden_generic_code AWB_ASSERTION MONACO_ASSERTION)
        foreach(source_variable sink_text agent_text monaco_text)
            string(FIND "${${source_variable}}" "${forbidden_generic_code}"
                forbidden_generic_offset)
            if(NOT forbidden_generic_offset EQUAL -1)
                list(APPEND errors
                    "Qt failure channel reintroduced generic code: ${forbidden_generic_code}")
            endif()
        endforeach()
    endforeach()

    set(agent_helper_stage_bindings
        "verifyBoundedContextThresholdCache=AWB_MODEL_RECOVERY"
        "verifyRuntimeDegradationFailures=AWB_RUNTIME_DEGRADATION"
        "verifyRuntimeHealthDegradationRefresh=AWB_RUNTIME_DEGRADATION"
        "verifyStrictTimelineValidation=AWB_TIMELINE_VALIDATION"
        "verifySessionScopedTimelineSequences=AWB_TIMELINE_VALIDATION"
        "verifyTimelineGapRecovery=AWB_TIMELINE_GAP"
        "verifyTimelineSubscriptionRecovery=AWB_TIMELINE_SUBSCRIPTION"
        "verifyTimelineSnapshotRecovery=AWB_TIMELINE_SNAPSHOT"
        "verifyDurableProposalUtf8Paging=AWB_PROPOSAL_ARTIFACT"
        "verifyStaleProposalArtifactResponseDiscarded=AWB_PROPOSAL_ARTIFACT"
        "verifyProposalSchemaVariants=AWB_PROPOSAL_PROJECTION"
        "verifyDurableProposalProjection=AWB_PROPOSAL_PROJECTION"
        "verifyTimelineProposalReference=AWB_PROPOSAL_PROJECTION")
    foreach(binding IN LISTS agent_helper_stage_bindings)
        string(REPLACE "=" ";" binding_parts "${binding}")
        list(GET binding_parts 0 function_name)
        list(GET binding_parts 1 failure_code)
        string(FIND "${agent_text}" "bool ${function_name}(" function_start)
        if(function_start EQUAL -1)
            list(APPEND errors
                "Agent Workbench diagnostic helper is missing: ${function_name}")
            continue()
        endif()
        string(SUBSTRING "${agent_text}" ${function_start} -1 function_tail)
        string(FIND "${function_tail}" "\nbool " next_function_offset)
        if(next_function_offset EQUAL -1)
            set(function_body "${function_tail}")
        else()
            string(SUBSTRING "${function_tail}" 0 ${next_function_offset}
                function_body)
        endif()
        string(FIND "${function_body}"
            "const ScopedFailureStage stage(FailureCode::${failure_code});"
            stage_offset)
        if(stage_offset EQUAL -1)
            list(APPEND errors
                "Agent Workbench diagnostic helper stage drifted: ${function_name}=${failure_code}")
            continue()
        endif()
        string(REGEX MATCHALL
            "ScopedFailureStage"
            helper_scoped_stage_calls "${function_body}")
        string(REGEX MATCHALL "setFailureStage[ \\t]*\\("
            helper_global_stage_calls "${function_body}")
        string(REGEX MATCHALL "failureStage[ \\t]*="
            helper_direct_stage_assignments "${function_body}")
        list(LENGTH helper_scoped_stage_calls helper_scoped_stage_call_count)
        list(LENGTH helper_global_stage_calls helper_global_stage_call_count)
        list(LENGTH helper_direct_stage_assignments
            helper_direct_stage_assignment_count)
        string(FIND "${function_body}" "expect(" first_expect_offset)
        if(NOT helper_scoped_stage_call_count EQUAL 1)
            list(APPEND errors
                "Agent Workbench diagnostic helper must declare exactly one scoped stage: ${function_name}")
        endif()
        if(NOT helper_global_stage_call_count EQUAL 0
                OR NOT helper_direct_stage_assignment_count EQUAL 0)
            list(APPEND errors
                "Agent Workbench diagnostic helper must not override its scoped stage: ${function_name}")
        endif()
        if(first_expect_offset EQUAL -1
                OR stage_offset GREATER first_expect_offset)
            list(APPEND errors
                "Agent Workbench diagnostic helper stage must precede its first assertion: ${function_name}=${failure_code}")
        endif()
    endforeach()

    string(FIND "${agent_text}" "int main(int argc, char *argv[])" agent_main_offset)
    if(agent_main_offset EQUAL -1)
        list(APPEND errors "Agent Workbench diagnostic main is missing")
        set(agent_main "")
    else()
        string(SUBSTRING "${agent_text}" ${agent_main_offset} -1 agent_main)
    endif()
    string(REGEX MATCHALL
        "setFailureStage[ \\t]*\\([ \\t]*FailureCode::[A-Z][A-Z0-9_]*[ \\t]*\\)"
        agent_main_stage_calls "${agent_main}")
    set(agent_main_stages)
    foreach(stage_call IN LISTS agent_main_stage_calls)
        string(REGEX REPLACE
            "setFailureStage[ \\t]*\\([ \\t]*FailureCode::([A-Z][A-Z0-9_]*)[ \\t]*\\)" "\\1"
            stage_code "${stage_call}")
        list(APPEND agent_main_stages "${stage_code}")
    endforeach()
    set(expected_agent_main_stages
        AWB_UI_BASELINE
        AWB_MODEL_RECOVERY
        AWB_TIMELINE_SNAPSHOT
        AWB_TIMELINE_SUBSCRIPTION
        AWB_RUNTIME_DEGRADATION
        AWB_UI_BASELINE
        AWB_MODEL_RECOVERY
        AWB_TURN_LIFECYCLE
        AWB_PROJECT_SESSION
        AWB_GIT_COMPACTION
        AWB_WORKSPACE_EDIT
        AWB_PROJECT_SESSION
        AWB_TERMINAL_CONTEXT
        AWB_CONTEXT_ARTIFACT
        AWB_EDITOR_LSP
        AWB_RESPONSIVE_STATE
        AWB_VISIBLE_STATE)
    if(NOT "${agent_main_stages}" STREQUAL "${expected_agent_main_stages}")
        list(APPEND errors
            "Agent Workbench main diagnostic stage sequence drifted: expected '${expected_agent_main_stages}', got '${agent_main_stages}'")
    endif()
    set(agent_main_stage_anchors
        "AWB_UI_BASELINE|QTemporaryDir workbenchData"
        "AWB_MODEL_RECOVERY|const QString protectedSessionId"
        "AWB_TIMELINE_SNAPSHOT|AgentRuntimeClient *runtimeClient"
        "AWB_TIMELINE_SUBSCRIPTION|AgentRuntimeClient *runtimeClient"
        "AWB_RUNTIME_DEGRADATION|AgentRuntimeClient *runtimeClient"
        "AWB_UI_BASELINE|QPushButton *runtimeRestart"
        "AWB_MODEL_RECOVERY|runtimeClient->runtimeInitialized"
        "AWB_TURN_LIFECYCLE|const QJsonObject commandBase"
        "AWB_PROJECT_SESSION|auto *sourceItem"
        "AWB_GIT_COMPACTION|int gitTabIndex"
        "AWB_WORKSPACE_EDIT|auto sha256"
        "AWB_PROJECT_SESSION|runtime->renameSession"
        "AWB_TERMINAL_CONTEXT|#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)"
        "AWB_CONTEXT_ARTIFACT|int structureTab"
        "AWB_EDITOR_LSP|editor->selectAll"
        "AWB_RESPONSIVE_STATE|QPushButton *compactChat"
        "AWB_VISIBLE_STATE|const int failureNoticeCountBeforeOffline")
    set(agent_stage_cursor 0)
    foreach(stage_anchor IN LISTS agent_main_stage_anchors)
        string(REPLACE "|" ";" stage_anchor_parts "${stage_anchor}")
        list(GET stage_anchor_parts 0 stage_code)
        list(GET stage_anchor_parts 1 stage_anchor_text)
        string(SUBSTRING "${agent_main}" ${agent_stage_cursor} -1 agent_stage_tail)
        set(stage_line_pattern
            "\n[ \\t]*setFailureStage[ \\t]*\\([ \\t]*FailureCode::${stage_code}[ \\t]*\\)[ \\t]*;[ \\t]*\n")
        string(REGEX MATCH "${stage_line_pattern}" stage_line_match
            "${agent_stage_tail}")
        if(stage_line_match STREQUAL "")
            list(APPEND errors
                "Agent Workbench diagnostic stage anchor is missing: ${stage_code} before ${stage_anchor_text}")
            break()
        endif()
        string(FIND "${agent_stage_tail}" "${stage_line_match}"
            stage_relative_offset)
        string(LENGTH "${stage_line_match}" stage_line_length)
        math(EXPR anchor_search_start
            "${stage_relative_offset} + ${stage_line_length}")
        string(SUBSTRING "${agent_stage_tail}" ${anchor_search_start} -1
            agent_anchor_tail)
        string(FIND "${agent_anchor_tail}" "${stage_anchor_text}"
            anchor_relative_offset)
        if(anchor_relative_offset EQUAL -1)
            list(APPEND errors
                "Agent Workbench diagnostic stage anchor is missing: ${stage_code} before ${stage_anchor_text}")
            break()
        endif()
        string(SUBSTRING "${agent_anchor_tail}" 0 ${anchor_relative_offset}
            agent_stage_anchor_gap)
        string(STRIP "${agent_stage_anchor_gap}"
            agent_stage_anchor_gap_content)
        if(NOT agent_stage_anchor_gap_content STREQUAL "")
            list(APPEND errors
                "Agent Workbench diagnostic stage is not immediately bound: ${stage_code} before ${stage_anchor_text}")
            break()
        endif()
        string(LENGTH "${stage_anchor_text}" anchor_length)
        math(EXPR agent_stage_cursor
            "${agent_stage_cursor} + ${anchor_search_start} + ${anchor_relative_offset} + ${anchor_length}")
    endforeach()

    string(FIND "${monaco_text}" "int main(int argc, char *argv[])" monaco_main_offset)
    if(monaco_main_offset EQUAL -1)
        list(APPEND errors "Monaco diagnostic main is missing")
        set(monaco_main "")
    else()
        string(SUBSTRING "${monaco_text}" ${monaco_main_offset} -1 monaco_main)
    endif()
    string(REGEX MATCHALL
        "setFailureStage[ \\t]*\\([ \\t]*FailureCode::[A-Z][A-Z0-9_]*[ \\t]*\\)"
        monaco_main_stage_calls "${monaco_main}")
    set(monaco_main_stages)
    foreach(stage_call IN LISTS monaco_main_stage_calls)
        string(REGEX REPLACE
            "setFailureStage[ \\t]*\\([ \\t]*FailureCode::([A-Z][A-Z0-9_]*)[ \\t]*\\)" "\\1"
            stage_code "${stage_call}")
        list(APPEND monaco_main_stages "${stage_code}")
    endforeach()
    set(expected_monaco_main_stages
        MONACO_RUNTIME_READY
        MONACO_WORKSPACE_FIXTURE
        MONACO_TERMINAL_BRIDGE
        MONACO_EDITOR_LIFECYCLE
        MONACO_SPLIT_LIFECYCLE
        MONACO_SPLIT_RESTORE
        MONACO_SECURITY_BOUNDARY)
    if(NOT "${monaco_main_stages}" STREQUAL "${expected_monaco_main_stages}")
        list(APPEND errors
            "Monaco main diagnostic stage sequence drifted: expected '${expected_monaco_main_stages}', got '${monaco_main_stages}'")
    endif()
    set(monaco_main_stage_anchors
        "MONACO_RUNTIME_READY|if (!expect(waitUntil(application, [runtimeStatus]"
        "MONACO_WORKSPACE_FIXTURE|QTemporaryDir project"
        "MONACO_TERMINAL_BRIDGE|int terminalTab"
        "MONACO_EDITOR_LIFECYCLE|QTreeWidgetItem *item"
        "MONACO_SPLIT_LIFECYCLE|split->click"
        "MONACO_SPLIT_RESTORE|{\n        AgentWorkbenchWidget restoredWorkbench"
        "MONACO_SECURITY_BOUNDARY|const QUrl trustedUrl")
    set(monaco_stage_cursor 0)
    foreach(stage_anchor IN LISTS monaco_main_stage_anchors)
        string(REPLACE "|" ";" stage_anchor_parts "${stage_anchor}")
        list(GET stage_anchor_parts 0 stage_code)
        list(GET stage_anchor_parts 1 stage_anchor_text)
        string(SUBSTRING "${monaco_main}" ${monaco_stage_cursor} -1 monaco_stage_tail)
        set(stage_line_pattern
            "\n[ \\t]*setFailureStage[ \\t]*\\([ \\t]*FailureCode::${stage_code}[ \\t]*\\)[ \\t]*;[ \\t]*\n")
        string(REGEX MATCH "${stage_line_pattern}" stage_line_match
            "${monaco_stage_tail}")
        if(stage_line_match STREQUAL "")
            list(APPEND errors
                "Monaco diagnostic stage anchor is missing: ${stage_code} before ${stage_anchor_text}")
            break()
        endif()
        string(FIND "${monaco_stage_tail}" "${stage_line_match}"
            stage_relative_offset)
        string(LENGTH "${stage_line_match}" stage_line_length)
        math(EXPR anchor_search_start
            "${stage_relative_offset} + ${stage_line_length}")
        string(SUBSTRING "${monaco_stage_tail}" ${anchor_search_start} -1
            monaco_anchor_tail)
        string(FIND "${monaco_anchor_tail}" "${stage_anchor_text}"
            anchor_relative_offset)
        if(anchor_relative_offset EQUAL -1)
            list(APPEND errors
                "Monaco diagnostic stage anchor is missing: ${stage_code} before ${stage_anchor_text}")
            break()
        endif()
        string(SUBSTRING "${monaco_anchor_tail}" 0 ${anchor_relative_offset}
            monaco_stage_anchor_gap)
        string(STRIP "${monaco_stage_anchor_gap}"
            monaco_stage_anchor_gap_content)
        if(NOT monaco_stage_anchor_gap_content STREQUAL "")
            list(APPEND errors
                "Monaco diagnostic stage is not immediately bound: ${stage_code} before ${stage_anchor_text}")
            break()
        endif()
        string(LENGTH "${stage_anchor_text}" anchor_length)
        math(EXPR monaco_stage_cursor
            "${monaco_stage_cursor} + ${anchor_search_start} + ${anchor_relative_offset} + ${anchor_length}")
    endforeach()

    string(FIND "${monaco_text}" "bool verifyWindowsWebEngineRenderer("
        monaco_d3d11_start)
    string(FIND "${monaco_text}" "\n#endif\n\n} // namespace"
        monaco_d3d11_end)
    if(monaco_d3d11_start EQUAL -1 OR monaco_d3d11_end EQUAL -1
            OR monaco_d3d11_end LESS_EQUAL monaco_d3d11_start)
        list(APPEND errors "Monaco D3D11 fixture body is missing")
        set(monaco_d3d11_body "")
    else()
        math(EXPR monaco_d3d11_length
            "${monaco_d3d11_end} - ${monaco_d3d11_start}")
        string(SUBSTRING "${monaco_text}" ${monaco_d3d11_start}
            ${monaco_d3d11_length} monaco_d3d11_body)
    endif()
    string(REGEX MATCHALL "expect\\(" monaco_d3d11_expect_calls
        "${monaco_d3d11_body}")
    string(REGEX MATCHALL "FailureCode::MONACO_D3D11_PRESENTATION"
        monaco_d3d11_code_calls "${monaco_d3d11_body}")
    list(LENGTH monaco_d3d11_expect_calls monaco_d3d11_expect_count)
    list(LENGTH monaco_d3d11_code_calls monaco_d3d11_code_count)
    if(NOT monaco_d3d11_expect_count EQUAL 5
            OR NOT monaco_d3d11_code_count EQUAL 5)
        list(APPEND errors
            "Monaco D3D11 fixture assertions do not each use their dedicated fixed code")
    endif()
    foreach(forbidden_d3d11_control "//" "/*" "*/" "#if" "#else" "#endif")
        string(FIND "${monaco_d3d11_body}" "${forbidden_d3d11_control}"
            forbidden_d3d11_control_offset)
        if(NOT forbidden_d3d11_control_offset EQUAL -1)
            list(APPEND errors
                "Monaco D3D11 fixture contains unreviewed control text: ${forbidden_d3d11_control}")
        endif()
    endforeach()
    foreach(d3d11_message
            "Qt Quick did not select the requested D3D11 graphics API"
            "WebEngine did not expose its internal QQuickWidget renderer"
            "WebEngine QQuickWidget did not expose a QQuickWindow"
            "WebEngine QQuickWindow scene graph did not initialize"
            "WebEngine scene graph did not initialize with D3D11")
        set(d3d11_message_literal "\"${d3d11_message}\",")
        string(FIND "${monaco_d3d11_body}" "${d3d11_message_literal}"
            d3d11_message_offset)
        if(d3d11_message_offset EQUAL -1)
            list(APPEND errors
                "Monaco D3D11 assertion is missing: ${d3d11_message}")
            continue()
        endif()
        string(LENGTH "${d3d11_message_literal}" d3d11_message_length)
        math(EXPR d3d11_after_message
            "${d3d11_message_offset} + ${d3d11_message_length}")
        string(SUBSTRING "${monaco_d3d11_body}" ${d3d11_after_message} -1
            d3d11_after_message_tail)
        string(FIND "${d3d11_after_message_tail}" "${d3d11_message_literal}"
            duplicate_d3d11_message_offset)
        if(NOT duplicate_d3d11_message_offset EQUAL -1)
            list(APPEND errors
                "Monaco D3D11 assertion is not uniquely bound: ${d3d11_message}")
            continue()
        endif()
        string(SUBSTRING "${monaco_d3d11_body}" 0 ${d3d11_message_offset}
            d3d11_before_message)
        string(FIND "${d3d11_before_message}" "\n"
            d3d11_message_line_break REVERSE)
        if(d3d11_message_line_break EQUAL -1)
            set(d3d11_message_line_start 0)
        else()
            math(EXPR d3d11_message_line_start
                "${d3d11_message_line_break} + 1")
        endif()
        math(EXPR d3d11_message_line_prefix_length
            "${d3d11_message_offset} - ${d3d11_message_line_start}")
        string(SUBSTRING "${monaco_d3d11_body}" ${d3d11_message_line_start}
            ${d3d11_message_line_prefix_length} d3d11_message_line_prefix)
        string(STRIP "${d3d11_message_line_prefix}"
            d3d11_message_line_prefix_content)
        if(NOT d3d11_message_line_prefix_content STREQUAL "")
            list(APPEND errors
                "Monaco D3D11 assertion is not on a real argument line: ${d3d11_message}")
            continue()
        endif()
        math(EXPR d3d11_binding_start
            "${d3d11_message_offset} + ${d3d11_message_length}")
        string(SUBSTRING "${monaco_d3d11_body}" ${d3d11_binding_start} -1
            d3d11_binding_tail)
        string(FIND "${d3d11_binding_tail}"
            "FailureCode::MONACO_D3D11_PRESENTATION" d3d11_code_offset)
        if(d3d11_code_offset EQUAL -1)
            list(APPEND errors
                "Monaco D3D11 assertion uses the wrong fixed code: ${d3d11_message}")
            continue()
        endif()
        string(SUBSTRING "${d3d11_binding_tail}" 0 ${d3d11_code_offset}
            d3d11_binding_gap)
        string(STRIP "${d3d11_binding_gap}" d3d11_binding_gap_content)
        if(NOT d3d11_binding_gap_content STREQUAL "")
            list(APPEND errors
                "Monaco D3D11 assertion uses the wrong fixed code: ${d3d11_message}")
        endif()
    endforeach()

    foreach(renderer_variable ${agent_variable} ${monaco_variable})
        set(renderer_text "${${renderer_variable}}")
        foreach(required_renderer_fragment
                "#include \"qt_test_failure_sink.h\""
                "aegisy::test::reportFailure("
                "aegisy::test::isFailureChannelSelfTest(argc, argv)"
                "aegisy::test::runFailureChannelSelfTest()")
            string(FIND "${renderer_text}" "${required_renderer_fragment}"
                required_renderer_offset)
            if(required_renderer_offset EQUAL -1)
                list(APPEND errors
                    "Qt renderer does not use the shared stderr failure channel: ${renderer_variable}: ${required_renderer_fragment}")
            endif()
        endforeach()
    endforeach()

    foreach(required_sink_fragment
            "enum class FailureCode"
            "failureCodeText(FailureCode code)"
            "kMaxFailureCodeBytes = 32"
            "buildFailureLine(FailureCode code"
            "writeWithFallback(const char *line"
            "GetStdHandle(STD_ERROR_HANDLE)"
            "WriteFile(handle"
            "if (offset == length) return true"
            "std::fwrite(bytes, 1, length, stderr)"
            "std::fflush(stderr)"
            "reportFailure(FailureCode code)")
        string(FIND "${sink_text}" "${required_sink_fragment}" required_sink_offset)
        if(required_sink_offset EQUAL -1)
            list(APPEND errors
                "Qt stderr failure sink is missing: ${required_sink_fragment}")
        endif()
    endforeach()

    foreach(forbidden_fragment
            "STD_OUTPUT_HANDLE"
            "AttachConsole"
            "AllocConsole"
            "CONERR$"
            "CONOUT$"
            "freopen"
            "OutputDebugString")
        foreach(source_variable ${sink_variable} ${agent_variable} ${monaco_variable})
            string(FIND "${${source_variable}}" "${forbidden_fragment}" forbidden_offset)
            if(NOT forbidden_offset EQUAL -1)
                list(APPEND errors
                    "Qt failure channel reintroduced forbidden console handling: ${source_variable}: ${forbidden_fragment}")
            endif()
        endforeach()
    endforeach()

    foreach(required_channel_test_fragment
            "COMMAND \"\${AEGISY_TEST_EXECUTABLE}\" --failure-channel-self-test"
            "RESULT_VARIABLE probe_result"
            "OUTPUT_VARIABLE probe_stdout"
            "ERROR_VARIABLE probe_stderr"
            "if(NOT \"\${probe_result}\" STREQUAL \"86\")"
            "if(NOT probe_stdout STREQUAL \"\")"
            "AEGISY_TEST_FAILURE: QT_STDERR_CHANNEL_PROBE\\n"
            "if(NOT normalized_stderr STREQUAL expected_stderr)")
        string(FIND "${channel_test_text}" "${required_channel_test_fragment}"
            required_channel_test_offset)
        if(required_channel_test_offset EQUAL -1)
            list(APPEND errors
                "Qt failure-channel CTest is missing: ${required_channel_test_fragment}")
        endif()
    endforeach()

    foreach(required_cmake_fragment
            "add_test(NAME agent_workbench_failure_channel"
            "TARGET_FILE:AegisyAgentWorkbenchRenderTest"
            "add_test(NAME monaco_editor_failure_channel"
            "TARGET_FILE:AegisyMonacoEditorRenderTest"
            "tests/qt_test_failure_channel_test.cmake")
        string(FIND "${cmake_text}" "${required_cmake_fragment}" required_cmake_offset)
        if(required_cmake_offset EQUAL -1)
            list(APPEND errors
                "CMake does not register the real Qt failure-channel probe: ${required_cmake_fragment}")
        endif()
    endforeach()

    set(${out_errors} "${errors}" PARENT_SCOPE)
endfunction()

function(validate_tool_failure_channel_policy tool_variable manager_variable cmake_variable
        out_errors)
    set(errors)
    set(tool_text "${${tool_variable}}")
    set(manager_text "${${manager_variable}}")
    set(cmake_text "${${cmake_variable}}")
    extract_failure_codes(tool_text tool_failure_codes)
    set(expected_tool_failure_codes ${required_tool_failure_codes})
    list(SORT expected_tool_failure_codes)
    if(NOT "${tool_failure_codes}" STREQUAL "${expected_tool_failure_codes}")
        list(APPEND errors
            "ToolManager fixed-code set drifted: expected '${expected_tool_failure_codes}', got '${tool_failure_codes}'")
    endif()
    foreach(required_fragment
            "#include \"qt_test_failure_sink.h\""
            "aegisy::test::reportFailure(code);"
            "aegisy::test::reportLocalDiagnostic(message);"
            "class ScopedEnvironmentVariable"
            "_wputenv_s("
            "previousEnvironmentValueIsRestorable("
            "return !value.isEmpty();"
            "bool restore() noexcept"
            "AEGISY_TOOL_MANAGER_RESTORE_TEST"
            "!restoreOverride.restore()"
            "qEnvironmentVariableIsSet(restoreVariableName)"
            "inherited environment state did not restore exactly"
            "QTemporaryDir residueFixture("
            "AEGISY_TOOL_MANAGER_TEST_PATH"
            "ScopedEnvironmentVariable isolatedPath("
            [=[if /I \"%~1\"==\"list\" if /I \"%~2\"==\"-g\" if /I \"%~3\"==\"opencode-ai\" if /I \"%~4\"==\"--depth=0\" if /I \"%~5\"==\"--json\" if \"%~6\"==\"\" (]=]
            [=[if [ \"$#\" -eq 5 ] && [ \"$1\" = \"list\" ] && [ \"$2\" = \"-g\" ] && [ \"$3\" = \"opencode-ai\" ] && [ \"$4\" = \"--depth=0\" ] && [ \"$5\" = \"--json\" ]; then]=]
            "bool asyncObserved = false;"
            "if (!asyncObserved) {"
            "FailureCode::TOOL_NPM_RESIDUE_TIMEOUT"
            "FailureCode::TOOL_NPM_RESIDUE_ASYNC"
            "const bool configHomeRestored = isolatedConfigHome.restore();"
            "const bool pathRestored = isolatedPath.restore();")
        string(FIND "${tool_text}" "${required_fragment}" required_offset)
        if(required_offset EQUAL -1)
            list(APPEND errors
                "ToolManager failure channel is missing: ${required_fragment}")
        endif()
    endforeach()
    foreach(forbidden_fragment
            "if (!manager.resolvedRuntimeCommand("
            "npm_config_prefix"
            "SetEnvironmentVariableW(")
        string(FIND "${tool_text}" "${forbidden_fragment}" forbidden_offset)
        if(NOT forbidden_offset EQUAL -1)
            list(APPEND errors
                "ToolManager npm residue fixture can be skipped or leak environment state: ${forbidden_fragment}")
        endif()
    endforeach()
    foreach(required_manager_fragment
            "#ifdef AEGISY_TOOL_MANAGER_RUNTIME_TEST"
            "qEnvironmentVariable("
            "\"AEGISY_TOOL_MANAGER_TEST_PATH\""
            "return isolatedPaths;"
            "#endif")
        string(FIND "${manager_text}" "${required_manager_fragment}" manager_offset)
        if(manager_offset EQUAL -1)
            list(APPEND errors
                "ToolManager isolated command-search seam is missing: ${required_manager_fragment}")
        endif()
    endforeach()
    foreach(forbidden_fragment
            "std::cerr"
            "std::cout"
            "AEGISY_TEST_FAILURE:")
        string(FIND "${tool_text}" "${forbidden_fragment}" forbidden_offset)
        if(NOT forbidden_offset EQUAL -1)
            list(APPEND errors
                "ToolManager failure channel bypasses the shared fixed-code sink: ${forbidden_fragment}")
        endif()
    endforeach()
    string(FIND "${cmake_text}"
        "tests/tool_manager_runtime_test.cpp\n        tests/qt_test_failure_sink.h"
        cmake_sink_offset)
    if(cmake_sink_offset EQUAL -1)
        list(APPEND errors
            "ToolManager runtime target does not list the shared failure sink header")
    endif()
    string(REGEX MATCHALL
        "(^|\n)[ \t]*target_compile_definitions\\([ \t\r\n]*AegisyToolManagerRuntimeTest[ \t\r\n]+PRIVATE[ \t\r\n]+AEGISY_TOOL_MANAGER_RUNTIME_TEST=1[ \t\r\n]*\\)"
        cmake_test_seam_definitions "${cmake_text}")
    list(LENGTH cmake_test_seam_definitions cmake_test_seam_definition_count)
    if(NOT cmake_test_seam_definition_count EQUAL 1)
        list(APPEND errors
            "ToolManager runtime target must privately own its isolated command-search seam exactly once")
    endif()
    string(REGEX MATCHALL "AEGISY_TOOL_MANAGER_RUNTIME_TEST=1"
        cmake_test_seam_tokens "${cmake_text}")
    list(LENGTH cmake_test_seam_tokens cmake_test_seam_token_count)
    if(NOT cmake_test_seam_token_count EQUAL 1)
        list(APPEND errors
            "ToolManager isolated command-search seam must not leak to another target or global scope")
    endif()
    set(${out_errors} "${errors}" PARENT_SCOPE)
endfunction()

function(expect_tool_failure_channel_rejection case_name tool_variable manager_variable
        cmake_variable expected_error)
    validate_tool_failure_channel_policy(${tool_variable} ${manager_variable}
        ${cmake_variable}
        actual_errors)
    string(JOIN "\n" actual_error_text ${actual_errors})
    string(FIND "${actual_error_text}" "${expected_error}" expected_error_offset)
    if(expected_error_offset EQUAL -1)
        message(FATAL_ERROR
            "ToolManager failure-channel negative case '${case_name}' was not rejected as expected. "
            "Expected '${expected_error}', got '${actual_error_text}'")
    endif()
endfunction()

function(expect_qt_failure_channel_rejection case_name sink_variable agent_variable
        monaco_variable channel_test_variable cmake_variable expected_error)
    validate_qt_failure_channel_policy(${sink_variable} ${agent_variable}
        ${monaco_variable} ${channel_test_variable} ${cmake_variable} actual_errors)
    string(JOIN "\n" actual_error_text ${actual_errors})
    string(FIND "${actual_error_text}" "${expected_error}" expected_error_offset)
    if(expected_error_offset EQUAL -1)
        message(FATAL_ERROR
            "Qt failure-channel negative case '${case_name}' was not rejected as expected. "
            "Expected '${expected_error}', got '${actual_error_text}'")
    endif()
endfunction()

validate_qt_failure_channel_policy(failure_sink_source agent_render_source
    monaco_render_source failure_channel_test_source cmake_source
    failure_channel_errors)
if(failure_channel_errors)
    string(JOIN "\n" failure_channel_error_text ${failure_channel_errors})
    message(FATAL_ERROR "Qt failure-channel policy failed:\n${failure_channel_error_text}")
endif()

validate_tool_failure_channel_policy(tool_runtime_source tool_manager_source cmake_source
    tool_failure_channel_errors)
if(tool_failure_channel_errors)
    string(JOIN "\n" tool_failure_channel_error_text ${tool_failure_channel_errors})
    message(FATAL_ERROR
        "ToolManager failure-channel policy failed:\n${tool_failure_channel_error_text}")
endif()

set(missing_tool_sink_include "${tool_runtime_source}")
string(REPLACE "#include \"qt_test_failure_sink.h\"" ""
    missing_tool_sink_include "${missing_tool_sink_include}")
expect_tool_failure_channel_rejection(
    "missing-tool-sink-include" missing_tool_sink_include tool_manager_source cmake_source
    "ToolManager failure channel is missing: #include \"qt_test_failure_sink.h\"")

set(missing_tool_timeout_code "${tool_runtime_source}")
string(REPLACE "FailureCode::TOOL_NPM_RESIDUE_TIMEOUT"
    "FailureCode::TOOL_NPM_RESIDUE_ASYNC"
    missing_tool_timeout_code "${missing_tool_timeout_code}")
expect_tool_failure_channel_rejection(
    "missing-tool-timeout-code" missing_tool_timeout_code tool_manager_source cmake_source
    "ToolManager fixed-code set drifted")

set(raw_tool_failure_output "${tool_runtime_source}\nstd::cerr << \"failure\";\n")
expect_tool_failure_channel_rejection(
    "raw-tool-failure-output" raw_tool_failure_output tool_manager_source cmake_source
    "ToolManager failure channel bypasses the shared fixed-code sink: std::cerr")

set(win32_only_tool_environment "${tool_runtime_source}")
string(REPLACE "_wputenv_s(" "SetEnvironmentVariableW("
    win32_only_tool_environment "${win32_only_tool_environment}")
expect_tool_failure_channel_rejection(
    "win32-only-tool-environment" win32_only_tool_environment
    tool_manager_source cmake_source
    "ToolManager npm residue fixture can be skipped or leak environment state: SetEnvironmentVariableW(")

set(unpreserved_empty_tool_environment "${tool_runtime_source}")
string(REPLACE "return !value.isEmpty();" "return true;"
    unpreserved_empty_tool_environment "${unpreserved_empty_tool_environment}")
expect_tool_failure_channel_rejection(
    "unpreserved-empty-tool-environment" unpreserved_empty_tool_environment
    tool_manager_source cmake_source
    "ToolManager failure channel is missing: return !value.isEmpty()")

set(missing_tool_sink_source "${cmake_source}")
string(REPLACE "        tests/qt_test_failure_sink.h\n" ""
    missing_tool_sink_source "${missing_tool_sink_source}")
expect_tool_failure_channel_rejection(
    "missing-tool-sink-source" tool_runtime_source tool_manager_source missing_tool_sink_source
    "ToolManager runtime target does not list the shared failure sink header")

set(public_tool_search_seam "${cmake_source}")
string(REPLACE
    "target_compile_definitions(AegisyToolManagerRuntimeTest PRIVATE\n        AEGISY_TOOL_MANAGER_RUNTIME_TEST=1)"
    "target_compile_definitions(AegisyToolManagerRuntimeTest PUBLIC\n        AEGISY_TOOL_MANAGER_RUNTIME_TEST=1)"
    public_tool_search_seam "${public_tool_search_seam}")
expect_tool_failure_channel_rejection(
    "public-tool-search-seam" tool_runtime_source tool_manager_source
    public_tool_search_seam
    "ToolManager runtime target must privately own its isolated command-search seam exactly once")

set(wrong_target_tool_search_seam "${cmake_source}")
string(REPLACE
    "target_compile_definitions(AegisyToolManagerRuntimeTest PRIVATE\n        AEGISY_TOOL_MANAGER_RUNTIME_TEST=1)"
    "target_compile_definitions(AegisyToolManagerGatewayConfigTest PRIVATE\n        AEGISY_TOOL_MANAGER_RUNTIME_TEST=1)"
    wrong_target_tool_search_seam "${wrong_target_tool_search_seam}")
expect_tool_failure_channel_rejection(
    "wrong-target-tool-search-seam" tool_runtime_source tool_manager_source
    wrong_target_tool_search_seam
    "ToolManager runtime target must privately own its isolated command-search seam exactly once")

set(global_tool_search_seam "${cmake_source}\nadd_compile_definitions(AEGISY_TOOL_MANAGER_RUNTIME_TEST=1)\n")
expect_tool_failure_channel_rejection(
    "global-tool-search-seam" tool_runtime_source tool_manager_source
    global_tool_search_seam
    "ToolManager isolated command-search seam must not leak to another target or global scope")

set(comment_spoofed_tool_search_seam "${cmake_source}")
string(REPLACE
    "    target_compile_definitions(AegisyToolManagerRuntimeTest PRIVATE\n        AEGISY_TOOL_MANAGER_RUNTIME_TEST=1)"
    "    # target_compile_definitions(AegisyToolManagerRuntimeTest PRIVATE\n    #     AEGISY_TOOL_MANAGER_RUNTIME_TEST=1)"
    comment_spoofed_tool_search_seam "${comment_spoofed_tool_search_seam}")
expect_tool_failure_channel_rejection(
    "comment-spoofed-tool-search-seam" tool_runtime_source tool_manager_source
    comment_spoofed_tool_search_seam
    "ToolManager runtime target must privately own its isolated command-search seam exactly once")

set(skippable_tool_residue "${tool_runtime_source}")
string(REPLACE
    "    QTemporaryDir residueFixture("
    "    if (!manager.resolvedRuntimeCommand(QStringLiteral(\"npm\"), 500).isEmpty()) {\n    QTemporaryDir residueFixture("
    skippable_tool_residue "${skippable_tool_residue}")
expect_tool_failure_channel_rejection(
    "skippable-tool-residue" skippable_tool_residue tool_manager_source cmake_source
    "ToolManager npm residue fixture can be skipped or leak environment state")

set(missing_tool_search_seam "${tool_manager_source}")
string(REPLACE "#ifdef AEGISY_TOOL_MANAGER_RUNTIME_TEST"
    "#ifdef REMOVED_TOOL_MANAGER_RUNTIME_TEST"
    missing_tool_search_seam "${missing_tool_search_seam}")
expect_tool_failure_channel_rejection(
    "missing-tool-search-seam" tool_runtime_source missing_tool_search_seam cmake_source
    "ToolManager isolated command-search seam is missing: #ifdef AEGISY_TOOL_MANAGER_RUNTIME_TEST")

set(permissive_windows_npm_fixture "${tool_runtime_source}")
string(REPLACE
    [=[if /I \"%~1\"==\"list\" if /I \"%~2\"==\"-g\" if /I \"%~3\"==\"opencode-ai\" if /I \"%~4\"==\"--depth=0\" if /I \"%~5\"==\"--json\" if \"%~6\"==\"\" (]=]
    [=[if /I \"%~1\"==\"list\" (]=]
    permissive_windows_npm_fixture "${permissive_windows_npm_fixture}")
expect_tool_failure_channel_rejection(
    "permissive-windows-npm-fixture" permissive_windows_npm_fixture
    tool_manager_source cmake_source
    [=[ToolManager failure channel is missing: if /I \"%~1\"==\"list\" if /I \"%~2\"==\"-g\"]=])

set(permissive_posix_npm_fixture "${tool_runtime_source}")
string(REPLACE
    [=[if [ \"$#\" -eq 5 ] && [ \"$1\" = \"list\" ] && [ \"$2\" = \"-g\" ] && [ \"$3\" = \"opencode-ai\" ] && [ \"$4\" = \"--depth=0\" ] && [ \"$5\" = \"--json\" ]; then]=]
    [=[if [ \"$1\" = \"list\" ]; then]=]
    permissive_posix_npm_fixture "${permissive_posix_npm_fixture}")
expect_tool_failure_channel_rejection(
    "permissive-posix-npm-fixture" permissive_posix_npm_fixture
    tool_manager_source cmake_source
    [=[ToolManager failure channel is missing: if [ \"$#\" -eq 5 ] && [ \"$1\" = \"list\" ]]=])

set(missing_sink_include_agent "${agent_render_source}")
string(REPLACE "#include \"qt_test_failure_sink.h\"" ""
    missing_sink_include_agent "${missing_sink_include_agent}")
expect_qt_failure_channel_rejection(
    "missing-sink-include" failure_sink_source missing_sink_include_agent
    monaco_render_source failure_channel_test_source cmake_source
    "Qt renderer does not use the shared stderr failure channel: missing_sink_include_agent")

set(stdout_failure_sink "${failure_sink_source}")
string(REPLACE "STD_ERROR_HANDLE" "STD_OUTPUT_HANDLE"
    stdout_failure_sink "${stdout_failure_sink}")
expect_qt_failure_channel_rejection(
    "stdout-handle" stdout_failure_sink agent_render_source monaco_render_source
    failure_channel_test_source cmake_source
    "Qt stderr failure sink is missing: GetStdHandle(STD_ERROR_HANDLE)")

set(console_rebound_monaco "${monaco_render_source}\nAttachConsole(ATTACH_PARENT_PROCESS);\n")
expect_qt_failure_channel_rejection(
    "console-rebind" failure_sink_source agent_render_source console_rebound_monaco
    failure_channel_test_source cmake_source
    "Qt failure channel reintroduced forbidden console handling: console_rebound_monaco: AttachConsole")

set(unbounded_failure_sink "${failure_sink_source}")
string(REPLACE "kMaxFailureCodeBytes = 32" "kMaxFailureCodeBytes = 4096"
    unbounded_failure_sink "${unbounded_failure_sink}")
expect_qt_failure_channel_rejection(
    "removed-message-bound" unbounded_failure_sink agent_render_source
    monaco_render_source failure_channel_test_source cmake_source
    "Qt stderr failure sink is missing: kMaxFailureCodeBytes = 32")

set(drifted_failure_code_sink "${failure_sink_source}")
string(REPLACE "\"AWB_UI_BASELINE\"" "\"AWB_UNREVIEWED\""
    drifted_failure_code_sink "${drifted_failure_code_sink}")
expect_qt_failure_channel_rejection(
    "drifted-fixed-code-set" drifted_failure_code_sink agent_render_source
    monaco_render_source failure_channel_test_source cmake_source
    "Qt stderr failure sink code set drifted")

set(extra_failure_code_enum "${failure_sink_source}")
string(REPLACE "    AWB_UI_BASELINE,\n"
    "    AWB_UI_BASELINE,\n    AWB_UNREVIEWED_ALIAS,\n"
    extra_failure_code_enum "${extra_failure_code_enum}")
expect_qt_failure_channel_rejection(
    "extra-enum-alias" extra_failure_code_enum agent_render_source
    monaco_render_source failure_channel_test_source cmake_source
    "Qt stderr failure sink enum set drifted")

set(swapped_failure_code_mapping "${failure_sink_source}")
string(REPLACE "return \"AWB_UI_BASELINE\"" "return \"AWB_MAPPING_SWAP\""
    swapped_failure_code_mapping "${swapped_failure_code_mapping}")
string(REPLACE "return \"AWB_DATA_ROOT\"" "return \"AWB_UI_BASELINE\""
    swapped_failure_code_mapping "${swapped_failure_code_mapping}")
string(REPLACE "return \"AWB_MAPPING_SWAP\"" "return \"AWB_DATA_ROOT\""
    swapped_failure_code_mapping "${swapped_failure_code_mapping}")
expect_qt_failure_channel_rejection(
    "swapped-code-mapping" swapped_failure_code_mapping agent_render_source
    monaco_render_source failure_channel_test_source cmake_source
    "Qt stderr failure sink mapping aliases AWB_DATA_ROOT")

set(missing_self_test_code "${failure_sink_source}")
string(REPLACE
    "        {FailureCode::AWB_TIMELINE_GAP, \"AWB_TIMELINE_GAP\"},\n"
    "" missing_self_test_code "${missing_self_test_code}")
expect_qt_failure_channel_rejection(
    "missing-self-test-code" missing_self_test_code agent_render_source
    monaco_render_source failure_channel_test_source cmake_source
    "Qt stderr failure sink self-test set drifted")

set(swapped_agent_helper_stage "${agent_render_source}")
string(REPLACE
    "const ScopedFailureStage stage(FailureCode::AWB_TIMELINE_GAP);"
    "const ScopedFailureStage stage(FailureCode::AWB_STAGE_SWAP);"
    swapped_agent_helper_stage "${swapped_agent_helper_stage}")
string(REPLACE
    "const ScopedFailureStage stage(FailureCode::AWB_TIMELINE_SNAPSHOT);"
    "const ScopedFailureStage stage(FailureCode::AWB_TIMELINE_GAP);"
    swapped_agent_helper_stage "${swapped_agent_helper_stage}")
string(REPLACE
    "const ScopedFailureStage stage(FailureCode::AWB_STAGE_SWAP);"
    "const ScopedFailureStage stage(FailureCode::AWB_TIMELINE_SNAPSHOT);"
    swapped_agent_helper_stage "${swapped_agent_helper_stage}")
expect_qt_failure_channel_rejection(
    "swapped-agent-helper-stage" failure_sink_source swapped_agent_helper_stage
    monaco_render_source failure_channel_test_source cmake_source
    "Agent Workbench diagnostic helper stage drifted: verifyTimelineGapRecovery=AWB_TIMELINE_GAP")

set(overridden_agent_helper_stage "${agent_render_source}")
string(REPLACE
    "    const ScopedFailureStage stage(FailureCode::AWB_TIMELINE_GAP);\n"
    "    const ScopedFailureStage stage(FailureCode::AWB_TIMELINE_GAP);\n    const ScopedFailureStage overrideStage(FailureCode::AWB_UI_BASELINE);\n"
    overridden_agent_helper_stage "${overridden_agent_helper_stage}")
expect_qt_failure_channel_rejection(
    "overridden-agent-helper-stage" failure_sink_source
    overridden_agent_helper_stage monaco_render_source
    failure_channel_test_source cmake_source
    "Agent Workbench diagnostic helper must declare exactly one scoped stage: verifyTimelineGapRecovery")

set(braced_agent_helper_stage "${agent_render_source}")
string(REPLACE
    "    const ScopedFailureStage stage(FailureCode::AWB_TIMELINE_GAP);\n"
    "    const ScopedFailureStage stage(FailureCode::AWB_TIMELINE_GAP);\n    const ScopedFailureStage overrideStage{FailureCode::AWB_UI_BASELINE};\n"
    braced_agent_helper_stage "${braced_agent_helper_stage}")
expect_qt_failure_channel_rejection(
    "braced-agent-helper-stage" failure_sink_source
    braced_agent_helper_stage monaco_render_source
    failure_channel_test_source cmake_source
    "Agent Workbench diagnostic helper must declare exactly one scoped stage: verifyTimelineGapRecovery")

set(mutated_agent_helper_stage "${agent_render_source}")
string(REPLACE
    "    const ScopedFailureStage stage(FailureCode::AWB_TIMELINE_GAP);\n"
    "    const ScopedFailureStage stage(FailureCode::AWB_TIMELINE_GAP);\n    setFailureStage(FailureCode::AWB_UI_BASELINE);\n"
    mutated_agent_helper_stage "${mutated_agent_helper_stage}")
expect_qt_failure_channel_rejection(
    "mutated-agent-helper-stage" failure_sink_source
    mutated_agent_helper_stage monaco_render_source
    failure_channel_test_source cmake_source
    "Agent Workbench diagnostic helper must not override its scoped stage: verifyTimelineGapRecovery")

set(duplicated_agent_main_stage "${agent_render_source}")
string(REPLACE
    "    setFailureStage(FailureCode::AWB_RESPONSIVE_STATE);\n"
    "    setFailureStage(FailureCode::AWB_RESPONSIVE_STATE);\n    setFailureStage(FailureCode::AWB_RESPONSIVE_STATE);\n"
    duplicated_agent_main_stage "${duplicated_agent_main_stage}")
expect_qt_failure_channel_rejection(
    "duplicated-agent-main-stage" failure_sink_source duplicated_agent_main_stage
    monaco_render_source failure_channel_test_source cmake_source
    "Agent Workbench main diagnostic stage sequence drifted")

set(swapped_agent_main_stage "${agent_render_source}")
string(REPLACE
    "setFailureStage(FailureCode::AWB_CONTEXT_ARTIFACT);"
    "setFailureStage(FailureCode::AWB_STAGE_SWAP);"
    swapped_agent_main_stage "${swapped_agent_main_stage}")
string(REPLACE
    "setFailureStage(FailureCode::AWB_EDITOR_LSP);"
    "setFailureStage(FailureCode::AWB_CONTEXT_ARTIFACT);"
    swapped_agent_main_stage "${swapped_agent_main_stage}")
string(REPLACE
    "setFailureStage(FailureCode::AWB_STAGE_SWAP);"
    "setFailureStage(FailureCode::AWB_EDITOR_LSP);"
    swapped_agent_main_stage "${swapped_agent_main_stage}")
expect_qt_failure_channel_rejection(
    "swapped-agent-main-stage" failure_sink_source swapped_agent_main_stage
    monaco_render_source failure_channel_test_source cmake_source
    "Agent Workbench main diagnostic stage sequence drifted")

set(late_agent_main_stage "${agent_render_source}")
string(REPLACE
    "    setFailureStage(FailureCode::AWB_RESPONSIVE_STATE);\n"
    ""
    late_agent_main_stage "${late_agent_main_stage}")
string(REPLACE
    "    QWidget *compactBar = workbench.findChild<QWidget *>(QStringLiteral(\"agentCompactPaneBar\"));\n"
    "    QWidget *compactBar = workbench.findChild<QWidget *>(QStringLiteral(\"agentCompactPaneBar\"));\n    setFailureStage(FailureCode::AWB_RESPONSIVE_STATE);\n"
    late_agent_main_stage "${late_agent_main_stage}")
expect_qt_failure_channel_rejection(
    "late-agent-main-stage" failure_sink_source late_agent_main_stage
    monaco_render_source failure_channel_test_source cmake_source
    "Agent Workbench diagnostic stage anchor is missing: AWB_RESPONSIVE_STATE before QPushButton *compactChat")

set(early_agent_visible_stage "${agent_render_source}")
string(REPLACE
    "    setFailureStage(FailureCode::AWB_VISIBLE_STATE);\n"
    ""
    early_agent_visible_stage "${early_agent_visible_stage}")
string(REPLACE
    "    QPushButton *compactChat = workbench.findChild<QPushButton *>(\n        QStringLiteral(\"agentCompactPaneChatButton\"));\n"
    "    QPushButton *compactChat = workbench.findChild<QPushButton *>(\n        QStringLiteral(\"agentCompactPaneChatButton\"));\n    setFailureStage(FailureCode::AWB_VISIBLE_STATE);\n"
    early_agent_visible_stage "${early_agent_visible_stage}")
expect_qt_failure_channel_rejection(
    "early-agent-visible-stage" failure_sink_source early_agent_visible_stage
    monaco_render_source failure_channel_test_source cmake_source
    "Agent Workbench diagnostic stage is not immediately bound: AWB_VISIBLE_STATE before const int failureNoticeCountBeforeOffline")

set(comment_spoofed_agent_visible_stage "${agent_render_source}")
string(REPLACE
    "    setFailureStage(FailureCode::AWB_VISIBLE_STATE);\n"
    "    // setFailureStage(FailureCode::AWB_VISIBLE_STATE);\n"
    comment_spoofed_agent_visible_stage
    "${comment_spoofed_agent_visible_stage}")
string(REPLACE
    "    QPushButton *compactChat = workbench.findChild<QPushButton *>(\n        QStringLiteral(\"agentCompactPaneChatButton\"));\n"
    "    QPushButton *compactChat = workbench.findChild<QPushButton *>(\n        QStringLiteral(\"agentCompactPaneChatButton\"));\n    setFailureStage (FailureCode::AWB_VISIBLE_STATE);\n"
    comment_spoofed_agent_visible_stage
    "${comment_spoofed_agent_visible_stage}")
expect_qt_failure_channel_rejection(
    "comment-spoofed-agent-visible-stage" failure_sink_source
    comment_spoofed_agent_visible_stage monaco_render_source
    failure_channel_test_source cmake_source
    "Agent Workbench main diagnostic stage sequence drifted")

set(swapped_monaco_main_stage "${monaco_render_source}")
string(REPLACE
    "setFailureStage(FailureCode::MONACO_WORKSPACE_FIXTURE);"
    "setFailureStage(FailureCode::MONACO_STAGE_SWAP);"
    swapped_monaco_main_stage "${swapped_monaco_main_stage}")
string(REPLACE
    "setFailureStage(FailureCode::MONACO_TERMINAL_BRIDGE);"
    "setFailureStage(FailureCode::MONACO_WORKSPACE_FIXTURE);"
    swapped_monaco_main_stage "${swapped_monaco_main_stage}")
string(REPLACE
    "setFailureStage(FailureCode::MONACO_STAGE_SWAP);"
    "setFailureStage(FailureCode::MONACO_TERMINAL_BRIDGE);"
    swapped_monaco_main_stage "${swapped_monaco_main_stage}")
expect_qt_failure_channel_rejection(
    "swapped-monaco-main-stage" failure_sink_source agent_render_source
    swapped_monaco_main_stage failure_channel_test_source cmake_source
    "Monaco main diagnostic stage sequence drifted")

set(late_monaco_restore_stage "${monaco_render_source}")
string(REPLACE
    "    setFailureStage(FailureCode::MONACO_SPLIT_RESTORE);\n"
    ""
    late_monaco_restore_stage "${late_monaco_restore_stage}")
string(REPLACE
    "        AgentWorkbenchWidget restoredWorkbench;\n"
    "        AgentWorkbenchWidget restoredWorkbench;\n        setFailureStage(FailureCode::MONACO_SPLIT_RESTORE);\n"
    late_monaco_restore_stage "${late_monaco_restore_stage}")
expect_qt_failure_channel_rejection(
    "late-monaco-restore-stage" failure_sink_source agent_render_source
    late_monaco_restore_stage failure_channel_test_source cmake_source
    "Monaco diagnostic stage anchor is missing: MONACO_SPLIT_RESTORE before {\n        AgentWorkbenchWidget restoredWorkbench")

set(early_monaco_restore_stage "${monaco_render_source}")
string(REPLACE
    "    setFailureStage(FailureCode::MONACO_SPLIT_RESTORE);\n"
    ""
    early_monaco_restore_stage "${early_monaco_restore_stage}")
string(REPLACE
    "    split->click();\n"
    "    split->click();\n    setFailureStage(FailureCode::MONACO_SPLIT_RESTORE);\n"
    early_monaco_restore_stage "${early_monaco_restore_stage}")
expect_qt_failure_channel_rejection(
    "early-monaco-restore-stage" failure_sink_source agent_render_source
    early_monaco_restore_stage failure_channel_test_source cmake_source
    "Monaco diagnostic stage is not immediately bound: MONACO_SPLIT_RESTORE before {\n        AgentWorkbenchWidget restoredWorkbench")

set(drifted_monaco_d3d11_assertion "${monaco_render_source}")
string(REPLACE
    "\"WebEngine QQuickWidget did not expose a QQuickWindow\",\n                FailureCode::MONACO_D3D11_PRESENTATION"
    "\"WebEngine QQuickWidget did not expose a QQuickWindow\",\n                FailureCode::MONACO_EDITOR_LIFECYCLE"
    drifted_monaco_d3d11_assertion "${drifted_monaco_d3d11_assertion}")
expect_qt_failure_channel_rejection(
    "drifted-monaco-d3d11-assertion" failure_sink_source agent_render_source
    drifted_monaco_d3d11_assertion failure_channel_test_source cmake_source
    "Monaco D3D11 fixture assertions do not each use their dedicated fixed code")

set(spoofed_monaco_d3d11_assertion "${monaco_render_source}")
string(REPLACE
    "\"WebEngine QQuickWidget did not expose a QQuickWindow\",\n                FailureCode::MONACO_D3D11_PRESENTATION"
    "\"WebEngine QQuickWidget did not expose a QQuickWindow\",\n                // FailureCode::MONACO_D3D11_PRESENTATION\n                FailureCode::MONACO_EDITOR_LIFECYCLE"
    spoofed_monaco_d3d11_assertion "${spoofed_monaco_d3d11_assertion}")
expect_qt_failure_channel_rejection(
    "spoofed-monaco-d3d11-assertion" failure_sink_source agent_render_source
    spoofed_monaco_d3d11_assertion failure_channel_test_source cmake_source
    "Monaco D3D11 assertion uses the wrong fixed code: WebEngine QQuickWidget did not expose a QQuickWindow")

set(comment_bound_monaco_d3d11_assertion "${monaco_render_source}")
string(REPLACE
    "                \"WebEngine QQuickWidget did not expose a QQuickWindow\",\n                FailureCode::MONACO_D3D11_PRESENTATION"
    "                // \"WebEngine QQuickWidget did not expose a QQuickWindow\", FailureCode::MONACO_D3D11_PRESENTATION\n                \"WebEngine QQuickWidget did not expose a QQuickWindow\",\n                FailureCode::MONACO_EDITOR_LIFECYCLE"
    comment_bound_monaco_d3d11_assertion
    "${comment_bound_monaco_d3d11_assertion}")
expect_qt_failure_channel_rejection(
    "comment-bound-monaco-d3d11-assertion" failure_sink_source
    agent_render_source comment_bound_monaco_d3d11_assertion
    failure_channel_test_source cmake_source
    "Monaco D3D11 fixture contains unreviewed control text: //")

set(generic_agent_default "${agent_render_source}")
string(REPLACE "FailureCode code)"
    "FailureCode code = FailureCode::AWB_ASSERTION)"
    generic_agent_default "${generic_agent_default}")
expect_qt_failure_channel_rejection(
    "generic-agent-default" failure_sink_source generic_agent_default
    monaco_render_source failure_channel_test_source cmake_source
    "Qt failure channel reintroduced generic code: AWB_ASSERTION")

set(merged_failure_channel_test "${failure_channel_test_source}")
string(REPLACE "ERROR_VARIABLE probe_stderr" "OUTPUT_VARIABLE probe_stderr"
    merged_failure_channel_test "${merged_failure_channel_test}")
expect_qt_failure_channel_rejection(
    "merged-output-channel" failure_sink_source agent_render_source
    monaco_render_source merged_failure_channel_test cmake_source
    "Qt failure-channel CTest is missing: ERROR_VARIABLE probe_stderr")

set(missing_monaco_failure_probe "${cmake_source}")
string(REPLACE "add_test(NAME monaco_editor_failure_channel"
    "add_test(NAME removed_monaco_editor_failure_channel"
    missing_monaco_failure_probe "${missing_monaco_failure_probe}")
expect_qt_failure_channel_rejection(
    "missing-monaco-probe" failure_sink_source agent_render_source
    monaco_render_source failure_channel_test_source missing_monaco_failure_probe
    "CMake does not register the real Qt failure-channel probe: add_test(NAME monaco_editor_failure_channel")

set(unicode_checkout "windows-验证-源码")
set(required_ctest_command
    "ctest --test-dir build -C Release --no-tests=error --output-on-failure *> $null")
set(required_ctest_rerun_line
    "ctest --test-dir build -C Release --rerun-failed --output-on-failure 2>&1 |")
set(required_rust_test_line
    "$testOutput = @(cargo test --locked --workspace --manifest-path agent-runtime\\Cargo.toml 2>&1)")
string(JOIN "|" required_qt_failure_code_alternation
    ${required_qt_failure_codes})
string(CONCAT required_qt_marker_pattern
    [=[\A(?:[1-9][0-9]*:[ \t]*)?(?<marker>AEGISY_TEST_FAILURE: (?:]=]
    "${required_qt_failure_code_alternation}"
    [=[))\z]=])
set(required_marker_assignment
    [=[$fixedDiagnostic = $markerMatch.Groups['marker'].Value
                          } elseif]=])
set(required_d3d11_factory_assignment
    [=[$line -match '^(?:[1-9][0-9]*:[ \t]*)?CreateDXGIFactory2\(\) failed to create DXGI factory:') {
                              $fixedDiagnostic = 'AEGISY_TEST_FAILURE: QT_D3D11_INITIALIZATION'
                          } elseif]=])
set(required_d3d11_device_assignment
    [=[$line -match '^(?:[1-9][0-9]*:[ \t]*)?Failed to create D3D11 device and context:') {
                              $fixedDiagnostic = 'AEGISY_TEST_FAILURE: QT_D3D11_INITIALIZATION'
                          } elseif]=])
set(required_d3d11_context_assignment
    [=[$line -match '^(?:[1-9][0-9]*:[ \t]*)?ID3D11DeviceContext1 not supported') {
                              $fixedDiagnostic = 'AEGISY_TEST_FAILURE: QT_D3D11_INITIALIZATION'
                          } elseif]=])
set(required_gles2_assignment
    [=[$fixedDiagnostic = 'AEGISY_TEST_FAILURE: WEBENGINE_GLES2_CONTEXT_CREATE'
                          } elseif]=])
set(required_gles3_assignment
    [=[$fixedDiagnostic = 'AEGISY_TEST_FAILURE: WEBENGINE_GLES3_CONTEXT_CREATE'
                          } elseif]=])
set(required_context_fatal_assignment
    [=[$fixedDiagnostic = 'AEGISY_TEST_FAILURE: WEBENGINE_CONTEXT_FATAL'
                          }
                          if ($null -ne $fixedDiagnostic) {]=])
set(required_qt_test_step_sha256
    "ce0990fd4e6b0c8b1b22611b275ddd45ad41a573b8f75888f81e43a5714c7f8e")
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

    set(qt_test_step_marker "      - name: Test Windows Qt agent runtime\n")
    string(REGEX MATCHALL
        "[-] name: Test Windows Qt agent runtime"
        qt_test_step_markers "${workflow_text}")
    list(LENGTH qt_test_step_markers qt_test_step_marker_count)
    if(NOT qt_test_step_marker_count EQUAL 1)
        list(APPEND errors
            "expected exactly one Windows Qt test step, found ${qt_test_step_marker_count}")
    endif()
    string(FIND "${workflow_text}" "${qt_test_step_marker}" qt_test_step_offset)
    set(qt_test_step "")
    if(qt_test_step_offset EQUAL -1)
        list(APPEND errors "missing Windows Qt test step")
    else()
        string(SUBSTRING "${workflow_text}" ${qt_test_step_offset} -1 qt_test_tail)
        string(FIND "${qt_test_tail}" "\n      - name: " next_qt_step_offset)
        if(next_qt_step_offset EQUAL -1)
            set(qt_test_step "${qt_test_tail}")
        else()
            string(SUBSTRING "${qt_test_tail}" 0 ${next_qt_step_offset} qt_test_step)
        endif()
    endif()
    string(SHA256 qt_test_step_sha256 "${qt_test_step}")
    if(NOT qt_test_step_sha256 STREQUAL required_qt_test_step_sha256)
        list(APPEND errors
            "Windows Qt test step content identity drifted: expected ${required_qt_test_step_sha256}, got ${qt_test_step_sha256}")
    endif()

    set(workflow_without_qt_test_step "${workflow_text}")
    string(REPLACE "${qt_test_step}" "" workflow_without_qt_test_step
        "${workflow_without_qt_test_step}")
    string(REPLACE "\\" "/" workflow_without_qt_test_step
        "${workflow_without_qt_test_step}")
    string(TOLOWER "${workflow_without_qt_test_step}"
        workflow_without_qt_test_step_lower)
    foreach(qt_step_scoped_token
            "testing/temporary"
            "lasttest.log"
            "lasttestsfailed.log"
            "windows qt test failure::"
            "windows qt test diagnostic::"
            "aegisy_test_failure:"
            "$failurelog"
            "$failedtests"
            "$failuremessage"
            "$fallbacklines"
            "$markerpattern"
            "$markermatch"
            "$fixeddiagnostic"
            "$fixedfallback"
            "$diagnosticmessage")
        string(FIND "${workflow_without_qt_test_step_lower}"
            "${qt_step_scoped_token}" external_qt_step_token_offset)
        if(NOT external_qt_step_token_offset EQUAL -1)
            list(APPEND errors
                "Windows Qt diagnostic state must be confined to the protected test step: ${qt_step_scoped_token}")
        endif()
    endforeach()

    foreach(required_qt_diagnostic_fragment
            "$ErrorActionPreference = 'Stop'"
            "$PSNativeCommandUseErrorActionPreference = $false"
            "[System.Collections.Generic.List[string]]::new()"
            "[System.Collections.Generic.Queue[string]]::new()"
            "$failureLog = Join-Path $PWD 'build\\Testing\\Temporary\\LastTestsFailed.log'"
            "Get-Content -LiteralPath $failureLog"
            "$_ -match '\\A[1-9][0-9]{0,5}:[A-Za-z0-9_.-]{1,128}\\z'"
            "Select-Object -First 50"
            "${required_qt_marker_pattern}"
            "[System.Text.RegularExpressions.Regex]::Match("
            "[System.Text.RegularExpressions.RegexOptions]::CultureInvariant"
            "${required_marker_assignment}"
            "${required_d3d11_factory_assignment}"
            "${required_d3d11_device_assignment}"
            "${required_d3d11_context_assignment}"
            "Failed to create GLES2 context"
            "${required_gles2_assignment}"
            "Failed to create GLES3 context"
            "${required_gles3_assignment}"
            "ContextResult::kFatalFailure"
            "${required_context_fatal_assignment}"
            "if ($line.Length -le 4096 -and $diagnosticLines.Count -lt 50)"
            "[void]$diagnosticLines.Add($fixedDiagnostic)"
            "$fixedFallback = $null"
            "if ($line.Length -le 4096)"
            "$fixedFallback = 'CTEST_RERUN_DIRECTORY'"
            "$fixedFallback = 'CTEST_RERUN_PROJECT'"
            "$fixedFallback = 'CTEST_RERUN_START'"
            "$fixedFallback = 'CTEST_RERUN_RESULT'"
            "$fixedFallback = 'CTEST_RERUN_FAILED_SET'"
            "$fixedFallback = 'CTEST_RERUN_ERROR'"
            "if ($null -ne $fixedFallback)"
            "if ($fallbackLines.Count -ge 20)"
            "[void]$fallbackLines.Dequeue()"
            "$fallbackLines.Enqueue($fixedFallback)"
            "Out-Null"
            "$diagnosticLines = @($fallbackLines)"
            "Internal ctest changing into directory:"
            "Test project "
            "The following tests FAILED:"
            "Errors while running CTest"
            "$failureMessage = ($failedTests | ForEach-Object { $_.Trim() }) -join \"`n\""
            "$failureMessage = $failureMessage.Replace('%', '%25').Replace(\"`r\", '%0D').Replace(\"`n\", '%0A')"
            "if ($failureMessage.Length -gt 2000)"
            "$failureMessage = $failureMessage.Substring(0, 1997)"
            "$failureMessage = $failureMessage -replace '%(?:0|2)?$', ''"
            "$failureMessage += '...'"
            "Windows Qt test failure::$failureMessage"
            "$diagnosticMessage = ($diagnosticLines | ForEach-Object { $_.Trim() }) -join \"`n\""
            "$rootVariants = @("
            "$sensitiveRoot.Replace('\\', '/')"
            "$sensitiveRoot.Replace('/', '\\')"
            "$diagnosticMessage = [System.Text.RegularExpressions.Regex]::Replace("
            "[System.Text.RegularExpressions.Regex]::Escape($rootVariant)"
            "[System.Text.RegularExpressions.RegexOptions]::IgnoreCase"
            "$diagnosticMessage = $diagnosticMessage.Replace('%', '%25').Replace(\"`r\", '%0D').Replace(\"`n\", '%0A')"
            "if ($diagnosticMessage.Length -gt 2000)"
            "$diagnosticMessage = $diagnosticMessage.Substring(0, 1997)"
            "$diagnosticMessage = $diagnosticMessage -replace '%(?:0|2)?$', ''"
            "$diagnosticMessage += '...'"
            "Windows Qt test diagnostic::$diagnosticMessage"
            "exit $exitCode")
        string(FIND "${qt_test_step}" "${required_qt_diagnostic_fragment}"
            qt_diagnostic_offset)
        if(qt_diagnostic_offset EQUAL -1)
            list(APPEND errors
                "missing bounded Windows Qt diagnostic gate: ${required_qt_diagnostic_fragment}")
        endif()
    endforeach()

    set(ordered_qt_diagnostic_fragments
        "$ErrorActionPreference = 'Stop'"
        "$PSNativeCommandUseErrorActionPreference = $false"
        "${required_ctest_command}"
        "$exitCode = $LASTEXITCODE"
        "if ($exitCode -ne 0)"
        "$failureLog = Join-Path $PWD 'build\\Testing\\Temporary\\LastTestsFailed.log'"
        "Get-Content -LiteralPath $failureLog"
        "$_ -match '\\A[1-9][0-9]{0,5}:[A-Za-z0-9_.-]{1,128}\\z'"
        "Select-Object -First 50"
        "$failureMessage = ($failedTests | ForEach-Object { $_.Trim() }) -join \"`n\""
        "$failureMessage = $failureMessage.Replace('%', '%25').Replace(\"`r\", '%0D').Replace(\"`n\", '%0A')"
        "if ($failureMessage.Length -gt 2000)"
        "$failureMessage = $failureMessage.Substring(0, 1997)"
        "$failureMessage = $failureMessage -replace '%(?:0|2)?$', ''"
        "$failureMessage += '...'"
        "Windows Qt test failure::$failureMessage"
        "[System.Collections.Generic.List[string]]::new()"
        "[System.Collections.Generic.Queue[string]]::new()"
        "${required_qt_marker_pattern}"
        "${required_ctest_rerun_line}"
        "if ($line.Length -le 4096 -and $diagnosticLines.Count -lt 50)"
        "[System.Text.RegularExpressions.Regex]::Match("
        "${required_marker_assignment}"
        "${required_d3d11_factory_assignment}"
        "${required_d3d11_device_assignment}"
        "${required_d3d11_context_assignment}"
        "Failed to create GLES2 context"
        "${required_gles2_assignment}"
        "Failed to create GLES3 context"
        "${required_gles3_assignment}"
        "ContextResult::kFatalFailure"
        "${required_context_fatal_assignment}"
        "[void]$diagnosticLines.Add($fixedDiagnostic)"
        "$fixedFallback = $null"
        "if ($line.Length -le 4096)"
        "Internal ctest changing into directory:"
        "$fixedFallback = 'CTEST_RERUN_DIRECTORY'"
        "$fixedFallback = 'CTEST_RERUN_PROJECT'"
        "$fixedFallback = 'CTEST_RERUN_START'"
        "$fixedFallback = 'CTEST_RERUN_RESULT'"
        "$fixedFallback = 'CTEST_RERUN_FAILED_SET'"
        "Errors while running CTest"
        "$fixedFallback = 'CTEST_RERUN_ERROR'"
        "if ($null -ne $fixedFallback)"
        "if ($fallbackLines.Count -ge 20)"
        "[void]$fallbackLines.Dequeue()"
        "$fallbackLines.Enqueue($fixedFallback)"
        "Out-Null"
        "$diagnosticLines = @($fallbackLines)"
        "$diagnosticMessage = ($diagnosticLines | ForEach-Object { $_.Trim() }) -join \"`n\""
        "$rootVariants = @("
        "$diagnosticMessage = [System.Text.RegularExpressions.Regex]::Replace("
        "[System.Text.RegularExpressions.Regex]::Escape($rootVariant)"
        "[System.Text.RegularExpressions.RegexOptions]::IgnoreCase"
        "$diagnosticMessage = $diagnosticMessage.Replace('%', '%25').Replace(\"`r\", '%0D').Replace(\"`n\", '%0A')"
        "if ($diagnosticMessage.Length -gt 2000)"
        "$diagnosticMessage = $diagnosticMessage.Substring(0, 1997)"
        "$diagnosticMessage = $diagnosticMessage -replace '%(?:0|2)?$', ''"
        "$diagnosticMessage += '...'"
        "Windows Qt test diagnostic::$diagnosticMessage"
        "exit $exitCode")
    set(qt_diagnostic_order_cursor 0)
    foreach(ordered_qt_diagnostic_fragment IN LISTS ordered_qt_diagnostic_fragments)
        string(SUBSTRING "${qt_test_step}" ${qt_diagnostic_order_cursor} -1
            qt_diagnostic_order_tail)
        string(FIND "${qt_diagnostic_order_tail}"
            "${ordered_qt_diagnostic_fragment}" qt_diagnostic_order_offset)
        if(qt_diagnostic_order_offset EQUAL -1)
            list(APPEND errors
                "Windows Qt diagnostic control flow is out of order at: ${ordered_qt_diagnostic_fragment}")
            break()
        endif()
        string(LENGTH "${ordered_qt_diagnostic_fragment}"
            qt_diagnostic_order_fragment_length)
        math(EXPR qt_diagnostic_order_cursor
            "${qt_diagnostic_order_cursor} + ${qt_diagnostic_order_offset} + ${qt_diagnostic_order_fragment_length}")
    endforeach()

    string(REGEX MATCHALL "exit [$]exitCode" qt_original_exits "${qt_test_step}")
    list(LENGTH qt_original_exits qt_original_exit_count)
    if(NOT qt_original_exit_count EQUAL 1)
        list(APPEND errors
            "expected exactly one original Windows Qt CTest exit, found ${qt_original_exit_count}")
    endif()
    string(REGEX MATCHALL "[$]exitCode = [$]LASTEXITCODE"
        qt_original_exit_captures "${qt_test_step}")
    list(LENGTH qt_original_exit_captures qt_original_exit_capture_count)
    if(NOT qt_original_exit_capture_count EQUAL 1)
        list(APPEND errors
            "expected exactly one original Windows Qt CTest exit capture, found ${qt_original_exit_capture_count}")
    endif()
    string(REGEX MATCHALL "[$]exitCode[ \t]*="
        qt_exit_code_assignments "${qt_test_step}")
    list(LENGTH qt_exit_code_assignments qt_exit_code_assignment_count)
    if(NOT qt_exit_code_assignment_count EQUAL 1)
        list(APPEND errors
            "expected exactly one Windows Qt CTest exit-code assignment, found ${qt_exit_code_assignment_count}")
    endif()

    string(REGEX MATCHALL "[$]markerPattern[ \t]*="
        qt_marker_assignments "${qt_test_step}")
    list(LENGTH qt_marker_assignments qt_marker_assignment_count)
    if(NOT qt_marker_assignment_count EQUAL 1)
        list(APPEND errors
            "expected exactly one Windows Qt fixed-marker assignment, found ${qt_marker_assignment_count}")
    endif()
    string(REGEX MATCHALL "[$]diagnosticLines[.]Add\\("
        qt_diagnostic_adds "${qt_test_step}")
    list(LENGTH qt_diagnostic_adds qt_diagnostic_add_count)
    if(NOT qt_diagnostic_add_count EQUAL 1)
        list(APPEND errors
            "Windows Qt diagnostic list must have exactly one fixed-code add path")
    endif()
    string(REGEX MATCHALL "[$]fallbackLines[.]Enqueue\\("
        qt_fallback_enqueues "${qt_test_step}")
    list(LENGTH qt_fallback_enqueues qt_fallback_enqueue_count)
    if(NOT qt_fallback_enqueue_count EQUAL 1)
        list(APPEND errors
            "Windows Qt fallback list must have exactly one fixed-code enqueue path")
    endif()
    string(REGEX MATCHALL "[$]fixedFallback[ \t]*="
        qt_fixed_fallback_assignments "${qt_test_step}")
    list(LENGTH qt_fixed_fallback_assignments qt_fixed_fallback_assignment_count)
    if(NOT qt_fixed_fallback_assignment_count EQUAL 7)
        list(APPEND errors
            "Windows Qt fallback classifier must have exactly seven closed assignments")
    endif()
    string(REGEX MATCHALL "Get-Content -LiteralPath [$]failureLog"
        qt_failure_log_reads "${qt_test_step}")
    string(REGEX MATCHALL "Select-Object -First 50"
        qt_failure_name_bounds "${qt_test_step}")
    list(LENGTH qt_failure_log_reads qt_failure_log_read_count)
    list(LENGTH qt_failure_name_bounds qt_failure_name_bound_count)
    if(NOT qt_failure_log_read_count EQUAL 1
            OR NOT qt_failure_name_bound_count EQUAL 1)
        list(APPEND errors
            "Windows Qt failed-test names must come once from the bounded LastTestsFailed log")
    endif()

    string(REGEX MATCHALL "[$](ctestOutput|rerunOutput)"
        qt_unbounded_output_variables "${qt_test_step}")
    list(LENGTH qt_unbounded_output_variables qt_unbounded_output_variable_count)
    if(NOT qt_unbounded_output_variable_count EQUAL 0)
        list(APPEND errors
            "Windows CTest output must be classified as a bounded stream")
    endif()

    string(TOLOWER "${qt_test_step}" qt_test_step_lower)
    string(REGEX MATCHALL "write-output" qt_public_output_commands
        "${qt_test_step_lower}")
    list(LENGTH qt_public_output_commands qt_public_output_command_count)
    if(NOT qt_public_output_command_count EQUAL 3)
        list(APPEND errors
            "Windows Qt test step must expose exactly three bounded annotations")
    endif()
    foreach(forbidden_output_command
            "Write-Host"
            "Write-Error"
            "Write-Warning"
            "Write-Information"
            "Write-Debug"
            "Write-Verbose"
            "Write-Progress"
            "Out-Host"
            "Out-Default"
            "Tee-Object"
            "[Console]::"
            "[System.Console]::"
            "$host.UI"
            "echo "
            "throw ")
        string(TOLOWER "${forbidden_output_command}"
            forbidden_output_command_lower)
        string(FIND "${qt_test_step_lower}"
            "${forbidden_output_command_lower}"
            forbidden_output_offset)
        if(NOT forbidden_output_offset EQUAL -1)
            list(APPEND errors
                "Windows Qt test step exposes an unreviewed output command: ${forbidden_output_command}")
        endif()
    endforeach()

    string(REGEX MATCHALL
        "AEGISY_TEST_FAILURE: WEBENGINE_[A-Z0-9_]*"
        workflow_webengine_markers "${qt_test_step}")
    list(LENGTH workflow_webengine_markers workflow_webengine_marker_count)
    if(NOT workflow_webengine_marker_count EQUAL 3)
        list(APPEND errors
            "expected exactly three fixed Windows Qt WebEngine diagnostics, found ${workflow_webengine_marker_count}")
    endif()
    set(workflow_webengine_codes)
    foreach(workflow_webengine_marker IN LISTS workflow_webengine_markers)
        string(REPLACE "AEGISY_TEST_FAILURE: " "" workflow_webengine_code
            "${workflow_webengine_marker}")
        list(APPEND workflow_webengine_codes "${workflow_webengine_code}")
    endforeach()
    list(REMOVE_DUPLICATES workflow_webengine_codes)
    list(SORT workflow_webengine_codes)
    set(expected_webengine_codes ${required_webengine_failure_codes})
    list(SORT expected_webengine_codes)
    if(NOT "${workflow_webengine_codes}" STREQUAL "${expected_webengine_codes}")
        list(APPEND errors
            "Windows Qt WebEngine diagnostic code set drifted: expected '${expected_webengine_codes}', got '${workflow_webengine_codes}'")
    endif()

    string(REGEX MATCHALL
        "AEGISY_TEST_FAILURE: QT_D3D11_[A-Z0-9_]*"
        workflow_d3d11_markers "${qt_test_step}")
    list(LENGTH workflow_d3d11_markers workflow_d3d11_marker_count)
    if(NOT workflow_d3d11_marker_count EQUAL 3)
        list(APPEND errors
            "expected exactly three fixed Windows Qt D3D11 source mappings, found ${workflow_d3d11_marker_count}")
    endif()
    set(workflow_d3d11_codes)
    foreach(workflow_d3d11_marker IN LISTS workflow_d3d11_markers)
        string(REPLACE "AEGISY_TEST_FAILURE: " "" workflow_d3d11_code
            "${workflow_d3d11_marker}")
        list(APPEND workflow_d3d11_codes "${workflow_d3d11_code}")
    endforeach()
    list(REMOVE_DUPLICATES workflow_d3d11_codes)
    list(SORT workflow_d3d11_codes)
    set(expected_d3d11_codes ${required_d3d11_failure_codes})
    list(SORT expected_d3d11_codes)
    if(NOT "${workflow_d3d11_codes}" STREQUAL "${expected_d3d11_codes}")
        list(APPEND errors
            "Windows Qt D3D11 diagnostic code set drifted: expected '${expected_d3d11_codes}', got '${workflow_d3d11_codes}'")
    endif()

    string(REGEX MATCHALL
        "Windows Qt test (failure::[$]failureMessage|diagnostic::[$]diagnosticMessage)"
        merged_qt_annotations
        "${qt_test_step}")
    list(LENGTH merged_qt_annotations merged_qt_annotation_count)
    if(NOT merged_qt_annotation_count EQUAL 2)
        list(APPEND errors
            "expected exactly two merged Windows Qt test annotations, found ${merged_qt_annotation_count}")
    endif()

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

    string(REGEX MATCHALL
        "[^\r\n]*ctest --test-dir build[^\r\n]*"
        ctest_lines
        "${workflow_text}")
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
    "ctest --test-dir build -C Release --rerun-failed --output-on-failure -R agent_workbench_render 2>&1 |"
    filtered_ctest_rerun_workflow
    "${filtered_ctest_rerun_workflow}")
expect_workflow_rejection(
    "filtered-ctest-rerun"
    "${filtered_ctest_rerun_workflow}"
    "CTest failed-set rerun must be exactly: ${required_ctest_rerun_line}")

set(non_prefix_marker_workflow "${workflow}")
set(permissive_qt_marker_pattern
    [=[.*(?<marker>AEGISY_TEST_FAILURE: [\x20-\x7E]{1,768})]=])
string(REPLACE
    "${required_qt_marker_pattern}"
    "${permissive_qt_marker_pattern}"
    non_prefix_marker_workflow
    "${non_prefix_marker_workflow}")
expect_workflow_rejection(
    "non-prefix-qt-diagnostic-marker"
    "${non_prefix_marker_workflow}"
    "missing bounded Windows Qt diagnostic gate: ${required_qt_marker_pattern}")

set(drifted_workflow_marker_code "${workflow}")
string(REPLACE "TOOL_NPM_RESIDUE_TIMEOUT" "TOOL_UNREVIEWED"
    drifted_workflow_marker_code "${drifted_workflow_marker_code}")
expect_workflow_rejection(
    "drifted-workflow-marker-code"
    "${drifted_workflow_marker_code}"
    "missing bounded Windows Qt diagnostic gate: ${required_qt_marker_pattern}")

set(missing_workflow_marker_pattern "${required_qt_marker_pattern}")
string(REPLACE "TOOL_COMMAND_SHAPE|" ""
    missing_workflow_marker_pattern "${missing_workflow_marker_pattern}")
set(missing_workflow_marker_code "${workflow}")
string(REPLACE "${required_qt_marker_pattern}"
    "${missing_workflow_marker_pattern}"
    missing_workflow_marker_code "${missing_workflow_marker_code}")
expect_workflow_rejection(
    "missing-workflow-marker-code"
    "${missing_workflow_marker_code}"
    "missing bounded Windows Qt diagnostic gate: ${required_qt_marker_pattern}")

set(extra_workflow_marker_pattern "${required_qt_marker_pattern}")
string(REPLACE [=[))\z]=] [=[|UNREVIEWED_CODE))\z]=]
    extra_workflow_marker_pattern "${extra_workflow_marker_pattern}")
set(extra_workflow_marker_code "${workflow}")
string(REPLACE "${required_qt_marker_pattern}"
    "${extra_workflow_marker_pattern}"
    extra_workflow_marker_code "${extra_workflow_marker_code}")
expect_workflow_rejection(
    "extra-workflow-marker-code"
    "${extra_workflow_marker_code}"
    "missing bounded Windows Qt diagnostic gate: ${required_qt_marker_pattern}")

set(unstripped_marker_workflow "${workflow}")
string(REPLACE
    "$markerMatch.Groups['marker'].Value"
    "$line"
    unstripped_marker_workflow
    "${unstripped_marker_workflow}")
expect_workflow_rejection(
    "unstripped-qt-diagnostic-marker"
    "${unstripped_marker_workflow}"
    "missing bounded Windows Qt diagnostic gate: ${required_marker_assignment}")

set(d3d11_secret_suffix_workflow "${workflow}")
string(REPLACE
    "'AEGISY_TEST_FAILURE: QT_D3D11_INITIALIZATION'"
    "'AEGISY_TEST_FAILURE: QT_D3D11_INITIALIZATION SECRET_SENTINEL'"
    d3d11_secret_suffix_workflow
    "${d3d11_secret_suffix_workflow}")
expect_workflow_rejection(
    "d3d11-secret-suffix"
    "${d3d11_secret_suffix_workflow}"
    "missing bounded Windows Qt diagnostic gate: ${required_d3d11_factory_assignment}")

set(d3d11_dynamic_suffix_workflow "${workflow}")
string(REPLACE
    "'AEGISY_TEST_FAILURE: QT_D3D11_INITIALIZATION'"
    "'AEGISY_TEST_FAILURE: QT_D3D11_INITIALIZATION' + $line"
    d3d11_dynamic_suffix_workflow
    "${d3d11_dynamic_suffix_workflow}")
expect_workflow_rejection(
    "d3d11-dynamic-suffix"
    "${d3d11_dynamic_suffix_workflow}"
    "missing bounded Windows Qt diagnostic gate: ${required_d3d11_factory_assignment}")

set(d3d11_permissive_prefix_workflow "${workflow}")
string(REPLACE
    "CreateDXGIFactory2\\(\\) failed to create DXGI factory:"
    ".*CreateDXGIFactory2\\(\\).*"
    d3d11_permissive_prefix_workflow
    "${d3d11_permissive_prefix_workflow}")
expect_workflow_rejection(
    "d3d11-permissive-prefix"
    "${d3d11_permissive_prefix_workflow}"
    "missing bounded Windows Qt diagnostic gate: ${required_d3d11_factory_assignment}")

set(graphics_secret_suffix_workflow "${workflow}")
string(REPLACE
    "'AEGISY_TEST_FAILURE: WEBENGINE_GLES3_CONTEXT_CREATE'"
    "'AEGISY_TEST_FAILURE: WEBENGINE_GLES3_CONTEXT_CREATE SECRET_SENTINEL'"
    graphics_secret_suffix_workflow
    "${graphics_secret_suffix_workflow}")
expect_workflow_rejection(
    "graphics-secret-suffix"
    "${graphics_secret_suffix_workflow}"
    "missing bounded Windows Qt diagnostic gate: ${required_gles3_assignment}")

set(graphics_dynamic_suffix_workflow "${workflow}")
string(REPLACE
    "'AEGISY_TEST_FAILURE: WEBENGINE_CONTEXT_FATAL'"
    "'AEGISY_TEST_FAILURE: WEBENGINE_CONTEXT_FATAL' + $line"
    graphics_dynamic_suffix_workflow
    "${graphics_dynamic_suffix_workflow}")
expect_workflow_rejection(
    "graphics-dynamic-suffix"
    "${graphics_dynamic_suffix_workflow}"
    "missing bounded Windows Qt diagnostic gate: ${required_context_fatal_assignment}")

set(graphics_pipeline_suffix_workflow "${workflow}")
string(REPLACE
    "'AEGISY_TEST_FAILURE: WEBENGINE_GLES2_CONTEXT_CREATE'"
    "'AEGISY_TEST_FAILURE: WEBENGINE_GLES2_CONTEXT_CREATE' + $_"
    graphics_pipeline_suffix_workflow
    "${graphics_pipeline_suffix_workflow}")
expect_workflow_rejection(
    "graphics-pipeline-suffix"
    "${graphics_pipeline_suffix_workflow}"
    "missing bounded Windows Qt diagnostic gate: ${required_gles2_assignment}")

set(unmerged_qt_failure_workflow "${workflow}")
string(REPLACE
    "Windows Qt test failure::$failureMessage"
    "Windows Qt test failure::$failedTests"
    unmerged_qt_failure_workflow
    "${unmerged_qt_failure_workflow}")
expect_workflow_rejection(
    "unmerged-qt-failure-annotation"
    "${unmerged_qt_failure_workflow}"
    "missing bounded Windows Qt diagnostic gate: Windows Qt test failure::$failureMessage")

set(unmerged_qt_diagnostic_workflow "${workflow}")
string(REPLACE
    "Windows Qt test diagnostic::$diagnosticMessage"
    "Windows Qt test diagnostic::$diagnosticLines"
    unmerged_qt_diagnostic_workflow
    "${unmerged_qt_diagnostic_workflow}")
expect_workflow_rejection(
    "unmerged-qt-diagnostic-annotation"
    "${unmerged_qt_diagnostic_workflow}"
    "missing bounded Windows Qt diagnostic gate: Windows Qt test diagnostic::$diagnosticMessage")

set(oversized_qt_failure_annotation_workflow "${workflow}")
string(REPLACE
    "if ($failureMessage.Length -gt 2000)"
    "if ($failureMessage.Length -gt 2001)"
    oversized_qt_failure_annotation_workflow
    "${oversized_qt_failure_annotation_workflow}")
string(REPLACE
    "$failureMessage = $failureMessage.Substring(0, 1997)"
    "$failureMessage = $failureMessage.Substring(0, 1998)"
    oversized_qt_failure_annotation_workflow
    "${oversized_qt_failure_annotation_workflow}")
expect_workflow_rejection(
    "oversized-qt-failure-annotation"
    "${oversized_qt_failure_annotation_workflow}"
    "missing bounded Windows Qt diagnostic gate: if ($failureMessage.Length -gt 2000)")

set(oversized_qt_diagnostic_annotation_workflow "${workflow}")
string(REPLACE
    "if ($diagnosticMessage.Length -gt 2000)"
    "if ($diagnosticMessage.Length -gt 2001)"
    oversized_qt_diagnostic_annotation_workflow
    "${oversized_qt_diagnostic_annotation_workflow}")
string(REPLACE
    "$diagnosticMessage = $diagnosticMessage.Substring(0, 1997)"
    "$diagnosticMessage = $diagnosticMessage.Substring(0, 1998)"
    oversized_qt_diagnostic_annotation_workflow
    "${oversized_qt_diagnostic_annotation_workflow}")
expect_workflow_rejection(
    "oversized-qt-diagnostic-annotation"
    "${oversized_qt_diagnostic_annotation_workflow}"
    "missing bounded Windows Qt diagnostic gate: if ($diagnosticMessage.Length -gt 2000)")

set(case_sensitive_root_redaction_workflow "${workflow}")
string(REPLACE
    "[System.Text.RegularExpressions.RegexOptions]::IgnoreCase"
    "[System.Text.RegularExpressions.RegexOptions]::None"
    case_sensitive_root_redaction_workflow
    "${case_sensitive_root_redaction_workflow}")
expect_workflow_rejection(
    "case-sensitive-qt-root-redaction"
    "${case_sensitive_root_redaction_workflow}"
    "missing bounded Windows Qt diagnostic gate: [System.Text.RegularExpressions.RegexOptions]::IgnoreCase")

set(discarded_root_redaction_workflow "${workflow}")
string(REPLACE
    "$diagnosticMessage = [System.Text.RegularExpressions.Regex]::Replace("
    "$discardedMessage = [System.Text.RegularExpressions.Regex]::Replace("
    discarded_root_redaction_workflow
    "${discarded_root_redaction_workflow}")
expect_workflow_rejection(
    "discarded-qt-root-redaction"
    "${discarded_root_redaction_workflow}"
    "missing bounded Windows Qt diagnostic gate: $diagnosticMessage = [System.Text.RegularExpressions.Regex]::Replace(")

set(late_failure_length_guard_workflow "${workflow}")
string(REPLACE
    "                  if ($failureMessage.Length -gt 2000) {\n                      $failureMessage = $failureMessage.Substring(0, 1997)"
    "                  $failureMessage = $failureMessage.Substring(0, 1997)\n                  if ($failureMessage.Length -gt 2000) {"
    late_failure_length_guard_workflow
    "${late_failure_length_guard_workflow}")
expect_workflow_rejection(
    "late-failure-length-guard"
    "${late_failure_length_guard_workflow}"
    "Windows Qt diagnostic control flow is out of order at: $failureMessage = $failureMessage.Substring(0, 1997)")

set(late_diagnostic_length_guard_workflow "${workflow}")
string(REPLACE
    "              if ($diagnosticMessage.Length -gt 2000) {\n                  $diagnosticMessage = $diagnosticMessage.Substring(0, 1997)"
    "              $diagnosticMessage = $diagnosticMessage.Substring(0, 1997)\n              if ($diagnosticMessage.Length -gt 2000) {"
    late_diagnostic_length_guard_workflow
    "${late_diagnostic_length_guard_workflow}")
expect_workflow_rejection(
    "late-diagnostic-length-guard"
    "${late_diagnostic_length_guard_workflow}"
    "Windows Qt diagnostic control flow is out of order at: $diagnosticMessage = $diagnosticMessage.Substring(0, 1997)")

set(unbounded_qt_fallback_workflow "${workflow}")
string(REPLACE
    "$line -match '^Internal ctest changing into directory:')"
    "$line -match '.+')"
    unbounded_qt_fallback_workflow
    "${unbounded_qt_fallback_workflow}")
expect_workflow_rejection(
    "unbounded-qt-fallback"
    "${unbounded_qt_fallback_workflow}"
    "missing bounded Windows Qt diagnostic gate: Internal ctest changing into directory:")

set(drifted_failed_test_source_workflow "${workflow}")
string(REPLACE
    "Get-Content -LiteralPath $failureLog"
    "Get-Content -LiteralPath $env:RUNNER_TEMP"
    drifted_failed_test_source_workflow
    "${drifted_failed_test_source_workflow}")
expect_workflow_rejection(
    "drifted-failed-test-source"
    "${drifted_failed_test_source_workflow}"
    "missing bounded Windows Qt diagnostic gate: Get-Content -LiteralPath $failureLog")

set(unbounded_failed_test_names_workflow "${workflow}")
string(REPLACE
    "Select-Object -First 50"
    "Select-Object"
    unbounded_failed_test_names_workflow
    "${unbounded_failed_test_names_workflow}")
expect_workflow_rejection(
    "unbounded-failed-test-names"
    "${unbounded_failed_test_names_workflow}"
    "missing bounded Windows Qt diagnostic gate: Select-Object -First 50")

set(raw_fallback_enqueue_workflow "${workflow}")
string(REPLACE
    "$fallbackLines.Enqueue($fixedFallback)"
    "$fallbackLines.Enqueue($fixedFallback)\n                          $fallbackLines.Enqueue($line)"
    raw_fallback_enqueue_workflow
    "${raw_fallback_enqueue_workflow}")
expect_workflow_rejection(
    "raw-fallback-enqueue"
    "${raw_fallback_enqueue_workflow}"
    "Windows Qt fallback list must have exactly one fixed-code enqueue path")

set(raw_diagnostic_add_workflow "${workflow}")
string(REPLACE
    "[void]$diagnosticLines.Add($fixedDiagnostic)"
    "[void]$diagnosticLines.Add($fixedDiagnostic)\n                              [void]$diagnosticLines.Add($line)"
    raw_diagnostic_add_workflow
    "${raw_diagnostic_add_workflow}")
expect_workflow_rejection(
    "raw-diagnostic-add"
    "${raw_diagnostic_add_workflow}"
    "Windows Qt diagnostic list must have exactly one fixed-code add path")

set(duplicate_marker_assignment_workflow "${workflow}")
string(REPLACE
    "$markerPattern = '${required_qt_marker_pattern}'"
    "$markerPattern = '${required_qt_marker_pattern}'\n              $markerPattern = '.*'"
    duplicate_marker_assignment_workflow
    "${duplicate_marker_assignment_workflow}")
expect_workflow_rejection(
    "duplicate-marker-assignment"
    "${duplicate_marker_assignment_workflow}"
    "expected exactly one Windows Qt fixed-marker assignment")

set(duplicate_qt_test_step_workflow "${workflow}")
string(REPLACE
    "      - name: Install Inno Setup\n"
    "      - name: Test Windows Qt agent runtime\n        shell: pwsh\n        run: Write-Output 'second publishing step'\n\n      - name: Install Inno Setup\n"
    duplicate_qt_test_step_workflow
    "${duplicate_qt_test_step_workflow}")
expect_workflow_rejection(
    "duplicate-windows-qt-test-step"
    "${duplicate_qt_test_step_workflow}"
    "expected exactly one Windows Qt test step, found 2")

function(expect_external_qt_token_rejection case_name token_literal token_pattern)
    set(external_token_workflow "${workflow}")
    string(REPLACE
        "      - name: Install Inno Setup\n"
        "      - name: Publish unexpected Windows Qt diagnostic\n        shell: pwsh\n        run: Write-Output '${token_literal}'\n\n      - name: Install Inno Setup\n"
        external_token_workflow
        "${external_token_workflow}")
    expect_workflow_rejection(
        "${case_name}"
        "${external_token_workflow}"
        "Windows Qt diagnostic state must be confined to the protected test step: ${token_pattern}")
endfunction()

expect_external_qt_token_rejection(
    "external-last-tests-failed-token"
    "LastTestsFailed.log"
    "lasttestsfailed.log")
expect_external_qt_token_rejection(
    "external-qt-failure-token"
    "Windows Qt test failure::"
    "windows qt test failure::")
expect_external_qt_token_rejection(
    "external-qt-diagnostic-token"
    "Windows Qt test diagnostic::"
    "windows qt test diagnostic::")
expect_external_qt_token_rejection(
    "external-fixed-failure-token"
    "AEGISY_TEST_FAILURE:"
    "aegisy_test_failure:")
expect_external_qt_token_rejection(
    "external-lowercase-last-tests-failed-token"
    "lasttestsfailed.log"
    "lasttestsfailed.log")

set(external_last_test_log_workflow "${workflow}")
string(REPLACE
    "      - name: Install Inno Setup\n"
    "      - name: Publish raw CTest log\n        shell: pwsh\n        run: Get-Content build\\Testing\\Temporary\\LastTest.log | Write-Output\n\n      - name: Install Inno Setup\n"
    external_last_test_log_workflow "${external_last_test_log_workflow}")
expect_workflow_rejection(
    "external-last-test-log"
    "${external_last_test_log_workflow}"
    "Windows Qt diagnostic state must be confined to the protected test step: testing/temporary")

set(external_wildcard_ctest_log_workflow "${workflow}")
string(REPLACE
    "      - name: Install Inno Setup\n"
    "      - name: Publish wildcard CTest log\n        shell: pwsh\n        run: Get-Content build\\Testing\\Temporary\\*.log | ForEach-Object { \"$_\" }\n\n      - name: Install Inno Setup\n"
    external_wildcard_ctest_log_workflow "${external_wildcard_ctest_log_workflow}")
expect_workflow_rejection(
    "external-wildcard-ctest-log"
    "${external_wildcard_ctest_log_workflow}"
    "Windows Qt diagnostic state must be confined to the protected test step: testing/temporary")

set(external_diagnostic_state_workflow "${workflow}")
string(REPLACE
    "      - name: Install Inno Setup\n"
    "      - name: Publish borrowed Qt diagnostic state\n        shell: pwsh\n        run: Write-Output $diagnosticMessage\n\n      - name: Install Inno Setup\n"
    external_diagnostic_state_workflow "${external_diagnostic_state_workflow}")
expect_workflow_rejection(
    "external-diagnostic-state"
    "${external_diagnostic_state_workflow}"
    "Windows Qt diagnostic state must be confined to the protected test step: $diagnosticmessage")

set(console_output_bypass_workflow "${workflow}")
string(REPLACE
    "              exit $exitCode\n          }\n\n      - name: Install Inno Setup"
    "              [console]::WriteLine($line)\n              exit $exitCode\n          }\n\n      - name: Install Inno Setup"
    console_output_bypass_workflow
    "${console_output_bypass_workflow}")
expect_workflow_rejection(
    "console-output-bypass"
    "${console_output_bypass_workflow}"
    "Windows Qt test step exposes an unreviewed output command: [Console]::")

set(lowercase_write_host_bypass_workflow "${workflow}")
string(REPLACE
    "              exit $exitCode\n          }\n\n      - name: Install Inno Setup"
    "              write-host $line\n              exit $exitCode\n          }\n\n      - name: Install Inno Setup"
    lowercase_write_host_bypass_workflow
    "${lowercase_write_host_bypass_workflow}")
expect_workflow_rejection(
    "lowercase-write-host-output-bypass"
    "${lowercase_write_host_bypass_workflow}"
    "Windows Qt test step exposes an unreviewed output command: Write-Host")

set(uppercase_echo_bypass_workflow "${workflow}")
string(REPLACE
    "              exit $exitCode\n          }\n\n      - name: Install Inno Setup"
    "              ECHO $line\n              exit $exitCode\n          }\n\n      - name: Install Inno Setup"
    uppercase_echo_bypass_workflow
    "${uppercase_echo_bypass_workflow}")
expect_workflow_rejection(
    "uppercase-echo-output-bypass"
    "${uppercase_echo_bypass_workflow}"
    "Windows Qt test step exposes an unreviewed output command: echo ")

set(uppercase_host_ui_bypass_workflow "${workflow}")
string(REPLACE
    "              exit $exitCode\n          }\n\n      - name: Install Inno Setup"
    "              $HOST.UI.WriteLine($line)\n              exit $exitCode\n          }\n\n      - name: Install Inno Setup"
    uppercase_host_ui_bypass_workflow
    "${uppercase_host_ui_bypass_workflow}")
expect_workflow_rejection(
    "uppercase-host-ui-output-bypass"
    "${uppercase_host_ui_bypass_workflow}"
    "Windows Qt test step exposes an unreviewed output command: $host.UI")

set(mixed_case_write_output_bypass_workflow "${workflow}")
string(REPLACE
    "              exit $exitCode\n          }\n\n      - name: Install Inno Setup"
    "              wRiTe-OuTpUt $line\n              exit $exitCode\n          }\n\n      - name: Install Inno Setup"
    mixed_case_write_output_bypass_workflow
    "${mixed_case_write_output_bypass_workflow}")
expect_workflow_rejection(
    "mixed-case-write-output-bypass"
    "${mixed_case_write_output_bypass_workflow}"
    "Windows Qt test step must expose exactly three bounded annotations")

set(appended_qt_step_workflow "${workflow}")
string(REPLACE
    "              exit $exitCode\n          }\n\n      - name: Install Inno Setup"
    "              [void]0\n              exit $exitCode\n          }\n\n      - name: Install Inno Setup"
    appended_qt_step_workflow
    "${appended_qt_step_workflow}")
expect_workflow_rejection(
    "appended-qt-step"
    "${appended_qt_step_workflow}"
    "Windows Qt test step content identity drifted")

set(unbounded_qt_diagnostic_list_workflow "${workflow}")
string(REPLACE
    "if ($line.Length -le 4096 -and $diagnosticLines.Count -lt 50)"
    "if ($line.Length -le 4096 -and $diagnosticLines.Count -lt 5000)"
    unbounded_qt_diagnostic_list_workflow
    "${unbounded_qt_diagnostic_list_workflow}")
expect_workflow_rejection(
    "unbounded-qt-diagnostic-list"
    "${unbounded_qt_diagnostic_list_workflow}"
    "missing bounded Windows Qt diagnostic gate: if ($line.Length -le 4096 -and $diagnosticLines.Count -lt 50)")

set(rerun_exit_code_workflow "${workflow}")
string(REPLACE
    "              exit $exitCode\n          }\n\n      - name: Install Inno Setup"
    "              exit $LASTEXITCODE\n          }\n\n      - name: Install Inno Setup"
    rerun_exit_code_workflow
    "${rerun_exit_code_workflow}")
expect_workflow_rejection(
    "rerun-exit-code-substitution"
    "${rerun_exit_code_workflow}"
    "missing bounded Windows Qt diagnostic gate: exit $exitCode")

set(early_qt_exit_workflow "${workflow}")
string(REPLACE
    "          $exitCode = $LASTEXITCODE\n"
    "          $exitCode = $LASTEXITCODE\n          exit $exitCode\n"
    early_qt_exit_workflow
    "${early_qt_exit_workflow}")
expect_workflow_rejection(
    "early-qt-test-exit"
    "${early_qt_exit_workflow}"
    "expected exactly one original Windows Qt CTest exit")

set(overwritten_qt_exit_code_workflow "${workflow}")
string(REPLACE
    "              $markerPattern = '${required_qt_marker_pattern}'\n"
    "              $exitCode = 0\n              $markerPattern = '${required_qt_marker_pattern}'\n"
    overwritten_qt_exit_code_workflow
    "${overwritten_qt_exit_code_workflow}")
expect_workflow_rejection(
    "overwritten-qt-test-exit-code"
    "${overwritten_qt_exit_code_workflow}"
    "expected exactly one Windows Qt CTest exit-code assignment, found 2")

set(published_streamed_ctest_output_workflow "${workflow}")
string(REPLACE
    "                  Out-Null\n"
    "                  Out-Null\n              Write-Host $diagnosticLines\n"
    published_streamed_ctest_output_workflow
    "${published_streamed_ctest_output_workflow}")
expect_workflow_rejection(
    "published-streamed-ctest-output"
    "${published_streamed_ctest_output_workflow}"
    "Windows Qt test step exposes an unreviewed output command: Write-Host")

set(captured_unbounded_ctest_output_workflow "${workflow}")
string(REPLACE
    "${required_ctest_command}"
    "$ctestOutput = @(ctest --test-dir build -C Release --no-tests=error --output-on-failure 2>&1)"
    captured_unbounded_ctest_output_workflow
    "${captured_unbounded_ctest_output_workflow}")
expect_workflow_rejection(
    "captured-unbounded-ctest-output"
    "${captured_unbounded_ctest_output_workflow}"
    "Windows CTest output must be classified as a bounded stream")

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

function(validate_monaco_windows_test_policy cmake_variable monaco_variable out_errors)
    set(errors)
    set(cmake_text "${${cmake_variable}}")
    set(monaco_text "${${monaco_variable}}")
    string(FIND "${cmake_text}"
        "        add_test(NAME monaco_editor_render COMMAND AegisyMonacoEditorRenderTest)"
        monaco_test_start)
    string(FIND "${cmake_text}"
        "# Isolated Qt WebEngine experiments" monaco_test_end)
    set(monaco_test_block "")
    if(monaco_test_start EQUAL -1 OR monaco_test_end EQUAL -1
            OR monaco_test_end LESS_EQUAL monaco_test_start)
        list(APPEND errors "missing bounded Monaco CTest block")
    else()
        math(EXPR monaco_test_length "${monaco_test_end} - ${monaco_test_start}")
        string(SUBSTRING "${cmake_text}" ${monaco_test_start}
            ${monaco_test_length} monaco_test_block)
    endif()

    string(FIND "${monaco_test_block}" "        if(WIN32)" windows_branch_start)
    string(FIND "${monaco_test_block}" "        else()" non_windows_branch_start)
    string(FIND "${monaco_test_block}" "        endif()" branch_end)
    set(monaco_environment_prefix "")
    set(monaco_windows_branch "")
    set(monaco_non_windows_branch "")
    if(windows_branch_start EQUAL -1 OR non_windows_branch_start EQUAL -1
            OR branch_end EQUAL -1
            OR non_windows_branch_start LESS_EQUAL windows_branch_start
            OR branch_end LESS_EQUAL non_windows_branch_start)
        list(APPEND errors "missing bounded Windows/non-Windows Monaco environment branches")
    else()
        string(SUBSTRING "${monaco_test_block}" 0 ${windows_branch_start}
            monaco_environment_prefix)
        math(EXPR windows_branch_length
            "${non_windows_branch_start} - ${windows_branch_start}")
        string(SUBSTRING "${monaco_test_block}" ${windows_branch_start}
            ${windows_branch_length} monaco_windows_branch)
        math(EXPR non_windows_branch_length
            "${branch_end} - ${non_windows_branch_start}")
        string(SUBSTRING "${monaco_test_block}" ${non_windows_branch_start}
            ${non_windows_branch_length} monaco_non_windows_branch)
    endif()

    set(ordered_monaco_windows_test_fragments
        "set(monaco_editor_render_environment"
        "if(WIN32)"
        "list(APPEND monaco_editor_render_environment"
        "QT_QPA_PLATFORM=windows"
        "QT_QUICK_BACKEND=rhi"
        "QSG_RHI_BACKEND=d3d11"
        "QSG_RHI_PREFER_SOFTWARE_RENDERER=1"
        "QT_FORCE_STDERR_LOGGING=1"
        "QTWEBENGINE_DISABLE_SANDBOX=1"
        "QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --enable-logging=stderr"
        "qt.webenginecontext.debug=true"
        "QSG_INFO=1"
        "else()"
        "list(APPEND monaco_editor_render_environment"
        "QT_QPA_PLATFORM=offscreen"
        "QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
        "endif()"
        "set_tests_properties(monaco_editor_render PROPERTIES"
        "ENVIRONMENT")

    set(controlled_windows_environment_keys
        QT_QPA_PLATFORM
        QT_QUICK_BACKEND
        QSG_RHI_BACKEND
        QSG_RHI_PREFER_SOFTWARE_RENDERER
        QT_FORCE_STDERR_LOGGING
        QTWEBENGINE_DISABLE_SANDBOX
        QTWEBENGINE_CHROMIUM_FLAGS
        QT_LOGGING_RULES
        QSG_INFO)
    foreach(controlled_environment_key IN LISTS controlled_windows_environment_keys)
        string(REGEX MATCHALL "${controlled_environment_key}=" controlled_matches
            "${monaco_windows_branch}")
        list(LENGTH controlled_matches controlled_match_count)
        if(NOT controlled_match_count EQUAL 1)
            list(APPEND errors
                "Windows Monaco environment must define ${controlled_environment_key} exactly once, found ${controlled_match_count}")
        endif()
        string(FIND "${monaco_environment_prefix}" "${controlled_environment_key}="
            common_environment_offset)
        if(NOT common_environment_offset EQUAL -1)
            list(APPEND errors
                "Windows Monaco environment leaks a controlled key from the common prefix: ${controlled_environment_key}")
        endif()
    endforeach()

    foreach(forbidden_windows_render_fragment
            "QT_QPA_PLATFORM=offscreen"
            "QT_QUICK_BACKEND=software"
            "QSG_RHI_BACKEND=software"
            "QT_OPENGL=software"
            "LIBGL_ALWAYS_SOFTWARE"
            "--disable-gpu"
            "--disable-gpu-compositing")
        string(FIND "${monaco_windows_branch}" "${forbidden_windows_render_fragment}"
            forbidden_windows_render_offset)
        if(NOT forbidden_windows_render_offset EQUAL -1)
            list(APPEND errors
                "Windows Monaco CTest reintroduced an unsupported renderer path: ${forbidden_windows_render_fragment}")
        endif()
    endforeach()

    foreach(required_non_windows_fragment
            "QT_QPA_PLATFORM=offscreen"
            "QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu")
        string(REGEX MATCHALL "${required_non_windows_fragment}"
            non_windows_matches "${monaco_non_windows_branch}")
        list(LENGTH non_windows_matches non_windows_match_count)
        if(NOT non_windows_match_count EQUAL 1)
            list(APPEND errors
                "Non-Windows Monaco fallback must define ${required_non_windows_fragment} exactly once, found ${non_windows_match_count}")
        endif()
    endforeach()
    foreach(forbidden_non_windows_fragment
            "QT_QPA_PLATFORM=windows"
            "QT_QUICK_BACKEND=rhi"
            "QSG_RHI_BACKEND=d3d11"
            "QSG_RHI_PREFER_SOFTWARE_RENDERER=1"
            "QSG_INFO=1")
        string(FIND "${monaco_non_windows_branch}"
            "${forbidden_non_windows_fragment}" forbidden_non_windows_offset)
        if(NOT forbidden_non_windows_offset EQUAL -1)
            list(APPEND errors
                "Non-Windows Monaco fallback contains a Windows renderer control: ${forbidden_non_windows_fragment}")
        endif()
    endforeach()

    set(monaco_test_cursor 0)
    foreach(monaco_test_fragment IN LISTS ordered_monaco_windows_test_fragments)
        string(SUBSTRING "${monaco_test_block}" ${monaco_test_cursor} -1
            monaco_test_tail)
        string(FIND "${monaco_test_tail}" "${monaco_test_fragment}"
            monaco_test_offset)
        if(monaco_test_offset EQUAL -1)
            list(APPEND errors
                "Windows Monaco CTest control flow is out of order at: ${monaco_test_fragment}")
            break()
        endif()
        string(LENGTH "${monaco_test_fragment}" monaco_test_fragment_length)
        math(EXPR monaco_test_cursor
            "${monaco_test_cursor} + ${monaco_test_offset} + ${monaco_test_fragment_length}")
    endforeach()

    foreach(required_quick_renderer_fragment
            "set(AEGISY_QT_WORKBENCH_COMPONENTS WebChannel WebEngineWidgets QuickWidgets)"
            "AND TARGET Qt6::QuickWidgets"
            "            Qt6::QuickWidgets\n            Qt6::WebChannel")
        string(FIND "${cmake_text}" "${required_quick_renderer_fragment}"
            quick_renderer_cmake_offset)
        if(quick_renderer_cmake_offset EQUAL -1)
            list(APPEND errors
                "Windows Monaco renderer is missing its Qt QuickWidgets contract: ${required_quick_renderer_fragment}")
        endif()
    endforeach()
    foreach(required_quick_assertion_fragment
            "#include <QQuickWidget>"
            "QQuickWindow::graphicsApi() == QSGRendererInterface::Direct3D11"
            "findChild<QQuickWidget *>()"
            "quickWidget->quickWindow()"
            "quickWindow->isSceneGraphInitialized()"
            "renderer->graphicsApi() == QSGRendererInterface::Direct3D11"
            "verifyWindowsWebEngineRenderer(application, monaco)")
        string(FIND "${monaco_text}" "${required_quick_assertion_fragment}"
            quick_assertion_offset)
        if(quick_assertion_offset EQUAL -1)
            list(APPEND errors
                "Windows Monaco renderer lacks a real QQuickWidget assertion: ${required_quick_assertion_fragment}")
        endif()
    endforeach()
    set(${out_errors} "${errors}" PARENT_SCOPE)
endfunction()

function(expect_monaco_windows_test_rejection case_name cmake_variable monaco_variable expected_error)
    validate_monaco_windows_test_policy(${cmake_variable} ${monaco_variable} actual_errors)
    string(JOIN "\n" actual_error_text ${actual_errors})
    string(FIND "${actual_error_text}" "${expected_error}" expected_error_offset)
    if(expected_error_offset EQUAL -1)
        message(FATAL_ERROR
            "Windows Monaco negative case '${case_name}' was not rejected as expected. "
            "Expected '${expected_error}', got '${actual_error_text}'")
    endif()
endfunction()

validate_monaco_windows_test_policy(cmake_source monaco_render_source
    monaco_windows_test_errors)
if(monaco_windows_test_errors)
    string(JOIN "\n" monaco_windows_test_error_text ${monaco_windows_test_errors})
    message(FATAL_ERROR
        "Windows Monaco CTest policy failed:\n${monaco_windows_test_error_text}")
endif()

set(inverted_monaco_windows_branch "${cmake_source}")
string(REPLACE
    "        if(WIN32)\n            # The GitHub Windows runner"
    "        if(NOT WIN32)\n            # The GitHub Windows runner"
    inverted_monaco_windows_branch "${inverted_monaco_windows_branch}")
expect_monaco_windows_test_rejection(
    "inverted-windows-branch" inverted_monaco_windows_branch monaco_render_source
    "Windows Monaco CTest control flow is out of order at: if(WIN32)")

set(missing_monaco_software_flag "${cmake_source}")
string(REPLACE "                \"QSG_RHI_BACKEND=d3d11\"\n" ""
    missing_monaco_software_flag "${missing_monaco_software_flag}")
expect_monaco_windows_test_rejection(
    "missing-d3d11-rhi" missing_monaco_software_flag monaco_render_source
    "Windows Monaco CTest control flow is out of order at: QSG_RHI_BACKEND=d3d11")

set(disabled_monaco_gpu "${cmake_source}")
string(REPLACE
    "QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --enable-logging=stderr"
    "QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --no-sandbox --enable-logging=stderr"
    disabled_monaco_gpu "${disabled_monaco_gpu}")
expect_monaco_windows_test_rejection(
    "disabled-chromium-gpu" disabled_monaco_gpu monaco_render_source
    "Windows Monaco CTest reintroduced an unsupported renderer path: --disable-gpu")

set(windows_offscreen_monaco "${cmake_source}")
string(REPLACE
    "                \"QT_QPA_PLATFORM=windows\"\n"
    "                \"QT_QPA_PLATFORM=windows\"\n                \"QT_QPA_PLATFORM=offscreen\"\n"
    windows_offscreen_monaco "${windows_offscreen_monaco}")
expect_monaco_windows_test_rejection(
    "windows-offscreen-qpa" windows_offscreen_monaco monaco_render_source
    "Windows Monaco CTest reintroduced an unsupported renderer path: QT_QPA_PLATFORM=offscreen")

set(windows_software_quick_backend "${cmake_source}")
string(REPLACE
    "                \"QT_QUICK_BACKEND=rhi\"\n"
    "                \"QT_QUICK_BACKEND=rhi\"\n                \"QT_QUICK_BACKEND=software\"\n"
    windows_software_quick_backend "${windows_software_quick_backend}")
expect_monaco_windows_test_rejection(
    "windows-software-quick-backend" windows_software_quick_backend
    monaco_render_source
    "Windows Monaco CTest reintroduced an unsupported renderer path: QT_QUICK_BACKEND=software")

set(windows_software_rhi_backend "${cmake_source}")
string(REPLACE
    "                \"QSG_RHI_BACKEND=d3d11\"\n"
    "                \"QSG_RHI_BACKEND=d3d11\"\n                \"QSG_RHI_BACKEND=software\"\n"
    windows_software_rhi_backend "${windows_software_rhi_backend}")
expect_monaco_windows_test_rejection(
    "windows-software-rhi-backend" windows_software_rhi_backend
    monaco_render_source
    "Windows Monaco CTest reintroduced an unsupported renderer path: QSG_RHI_BACKEND=software")

set(windows_software_opengl "${cmake_source}")
string(REPLACE
    "                \"QSG_RHI_BACKEND=d3d11\"\n"
    "                \"QSG_RHI_BACKEND=d3d11\"\n                \"QT_OPENGL=software\"\n"
    windows_software_opengl "${windows_software_opengl}")
expect_monaco_windows_test_rejection(
    "windows-software-opengl" windows_software_opengl monaco_render_source
    "Windows Monaco CTest reintroduced an unsupported renderer path: QT_OPENGL=software")

set(reversed_gpu_disable_flags "${cmake_source}")
string(REPLACE
    "QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --enable-logging=stderr"
    "QTWEBENGINE_CHROMIUM_FLAGS=--enable-logging=stderr --disable-gpu-compositing --no-sandbox"
    reversed_gpu_disable_flags "${reversed_gpu_disable_flags}")
expect_monaco_windows_test_rejection(
    "reversed-gpu-disable-flags" reversed_gpu_disable_flags monaco_render_source
    "Windows Monaco CTest reintroduced an unsupported renderer path: --disable-gpu")

set(embedded_gpu_disable_flag "${cmake_source}")
string(REPLACE
    "QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --enable-logging=stderr"
    "QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --feature=--disable-gpu --enable-logging=stderr"
    embedded_gpu_disable_flag "${embedded_gpu_disable_flag}")
expect_monaco_windows_test_rejection(
    "embedded-gpu-disable-flag" embedded_gpu_disable_flag monaco_render_source
    "Windows Monaco CTest reintroduced an unsupported renderer path: --disable-gpu")

set(duplicated_windows_renderer_env "${cmake_source}")
string(REPLACE
    "                \"QSG_INFO=1\")"
    "                \"QSG_INFO=1\"\n                \"QSG_INFO=1\")"
    duplicated_windows_renderer_env "${duplicated_windows_renderer_env}")
expect_monaco_windows_test_rejection(
    "duplicated-windows-renderer-env" duplicated_windows_renderer_env
    monaco_render_source
    "Windows Monaco environment must define QSG_INFO exactly once, found 2")

set(common_offscreen_renderer_env "${cmake_source}")
string(REPLACE
    "            \"AEGISY_WORKBENCH_WEB_ROOT=\${AEGISY_WORKBENCH_WEB_BUILD_DIR}\")"
    "            \"AEGISY_WORKBENCH_WEB_ROOT=\${AEGISY_WORKBENCH_WEB_BUILD_DIR}\"\n            \"QT_QPA_PLATFORM=offscreen\")"
    common_offscreen_renderer_env "${common_offscreen_renderer_env}")
expect_monaco_windows_test_rejection(
    "common-offscreen-renderer-env" common_offscreen_renderer_env
    monaco_render_source
    "Windows Monaco environment leaks a controlled key from the common prefix: QT_QPA_PLATFORM")

set(reordered_windows_renderer_env "${cmake_source}")
string(REPLACE
    "                \"QT_QPA_PLATFORM=windows\"\n                \"QT_QUICK_BACKEND=rhi\""
    "                \"QT_QUICK_BACKEND=rhi\"\n                \"QT_QPA_PLATFORM=windows\""
    reordered_windows_renderer_env "${reordered_windows_renderer_env}")
expect_monaco_windows_test_rejection(
    "reordered-windows-renderer-env" reordered_windows_renderer_env
    monaco_render_source
    "Windows Monaco CTest control flow is out of order at: QT_QUICK_BACKEND=rhi")

set(missing_quickwidgets_component "${cmake_source}")
string(REPLACE
    "set(AEGISY_QT_WORKBENCH_COMPONENTS WebChannel WebEngineWidgets QuickWidgets)"
    "set(AEGISY_QT_WORKBENCH_COMPONENTS WebChannel WebEngineWidgets)"
    missing_quickwidgets_component "${missing_quickwidgets_component}")
expect_monaco_windows_test_rejection(
    "missing-quickwidgets-component" missing_quickwidgets_component
    monaco_render_source
    "Windows Monaco renderer is missing its Qt QuickWidgets contract: set(AEGISY_QT_WORKBENCH_COMPONENTS WebChannel WebEngineWidgets QuickWidgets)")

set(missing_quickwidgets_enable_target "${cmake_source}")
string(REPLACE "        AND TARGET Qt6::QuickWidgets\n" ""
    missing_quickwidgets_enable_target "${missing_quickwidgets_enable_target}")
expect_monaco_windows_test_rejection(
    "missing-quickwidgets-enable-target" missing_quickwidgets_enable_target
    monaco_render_source
    "Windows Monaco renderer is missing its Qt QuickWidgets contract: AND TARGET Qt6::QuickWidgets")

set(missing_quickwidgets_link "${cmake_source}")
string(REPLACE "            Qt6::QuickWidgets\n" ""
    missing_quickwidgets_link "${missing_quickwidgets_link}")
expect_monaco_windows_test_rejection(
    "missing-quickwidgets-link" missing_quickwidgets_link monaco_render_source
    "Windows Monaco renderer is missing its Qt QuickWidgets contract:             Qt6::QuickWidgets\n            Qt6::WebChannel")

set(fake_quickwidget_assertion "${monaco_render_source}")
string(REPLACE "findChild<QQuickWidget *>()" "findChild<QObject *>()"
    fake_quickwidget_assertion "${fake_quickwidget_assertion}")
expect_monaco_windows_test_rejection(
    "fake-quickwidget-assertion" cmake_source fake_quickwidget_assertion
    "Windows Monaco renderer lacks a real QQuickWidget assertion: findChild<QQuickWidget *>()")
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
        WebEngineWidgets
        QuickWidgets)
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
