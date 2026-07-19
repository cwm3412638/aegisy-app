use aegisy_agentd::workbench_store::{
    durable_blob_reference_id, DurableBlobKind, DurableBlobWrite, WorkbenchStore,
};
use aegisy_agentd::Runtime;
use base64::engine::general_purpose::STANDARD as BASE64_STANDARD;
use base64::Engine;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::fs;
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

fn request(id: &str, method: &str, params: Value) -> String {
    json!({ "jsonrpc": "2.0", "id": id, "method": method, "params": params }).to_string()
}

fn ready_runtime() -> Runtime {
    let mut runtime = Runtime::default();
    let messages = runtime.handle_line(&request(
        "1",
        "initialize",
        json!({
            "protocol_version": "0.1",
            "client": { "name": "test", "version": "1" }
        }),
    ));
    assert_eq!(messages[0]["result"]["protocol_version"], "0.1");
    assert_eq!(messages[0]["result"]["backend"]["adapter"], "preview");
    assert_eq!(messages[0]["result"]["backend"]["status"], "ready");
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "workspace.index.tree-sitter"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "project.trust-review"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "project.trust-acknowledge"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "project.relink.explicit"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "workspace.diagnostics.language-server"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "workspace.diagnostics.command-output"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "workspace.edit.preview.read-only"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "turn.context.structured"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "turn.context.manifest"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "turn.context.inspect"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "workspace.instructions.discovery"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "terminal.environment.session-scoped"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "terminal.lifecycle.named"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "terminal.stop.out-of-band"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "session.history.paginated"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "operation.reconciliation"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "operation.reconciliation.probe"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "operation.reconciliation.status"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "runtime.health"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "runtime.degradations"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "session.deletion.two-phase"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "session.portable.export"));
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "session.portable.import"));
    #[cfg(target_os = "macos")]
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "terminal.pty.macos.user-initiated"));
    #[cfg(target_os = "windows")]
    assert!(messages[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "terminal.conpty.windows.user-initiated"));
    assert!(runtime
        .handle_line(r#"{"jsonrpc":"2.0","method":"initialized"}"#)
        .is_empty());
    runtime
}

#[test]
fn requires_complete_handshake() {
    let mut runtime = Runtime::default();
    let messages = runtime.handle_line(&request("1", "session/start", json!({ "mode": "chat" })));
    assert_eq!(messages[0]["error"]["code"], -32002);
}

#[test]
fn runtime_health_reports_preview_readiness() {
    let mut runtime = ready_runtime();
    let messages = runtime.handle_line(&request("health", "runtime/health", json!({})));
    assert_eq!(
        messages[0]["result"]["schema_version"],
        "runtime-health/0.1"
    );
    assert_eq!(messages[0]["result"]["backend"], "preview");
    assert_eq!(messages[0]["result"]["state"], "ready");
    assert_eq!(messages[0]["result"]["restart_required"], false);
}

#[test]
fn runtime_degradations_are_explicit_for_preview() {
    let mut runtime = ready_runtime();
    let messages = runtime.handle_line(&request("degradations", "runtime/degradations", json!({})));
    assert_eq!(
        messages[0]["result"]["schema_version"],
        "runtime-degradations/0.1"
    );
    assert_eq!(
        messages[0]["result"]["degradations"][0]["feature"],
        "codex-provider"
    );
    assert_eq!(
        messages[0]["result"]["degradations"][0]["state"],
        "unavailable"
    );
}

#[test]
fn codex_thread_lifecycle_fixture_is_redacted_and_well_formed() {
    let path = format!(
        "{}/../../aap-schema/fixtures/codex-thread-lifecycle.jsonl",
        env!("CARGO_MANIFEST_DIR")
    );
    let fixture = fs::read_to_string(path).unwrap();
    let mut methods = Vec::new();
    for line in fixture.lines() {
        assert!(!line.contains("sk-"));
        assert!(!line.contains("ghp_"));
        assert!(!line.to_ascii_lowercase().contains("authorization"));
        let message: Value = serde_json::from_str(line).unwrap();
        methods.push(message["method"].as_str().unwrap().to_owned());
    }
    assert_eq!(
        methods,
        [
            "initialize",
            "initialized",
            "runtime/health",
            "runtime/degradations",
            "session/provider-list",
            "session/provider-read",
            "session/start",
            "session/archive",
            "session/unarchive",
            "shutdown"
        ]
    );
}

#[test]
fn codex_turn_metadata_fixture_matches_schema_methods_without_secrets() {
    let path = format!(
        "{}/../../aap-schema/fixtures/codex-turn-metadata.jsonl",
        env!("CARGO_MANIFEST_DIR")
    );
    let fixture = fs::read_to_string(path).unwrap();
    let methods = fixture
        .lines()
        .map(|line| {
            assert!(!line.contains("sk-"));
            assert!(!line.contains("ghp_"));
            let message: Value = serde_json::from_str(line).unwrap();
            message["method"].as_str().unwrap().to_owned()
        })
        .collect::<Vec<_>>();
    assert_eq!(
        methods,
        [
            "thread/tokenUsage/updated",
            "turn/plan/updated",
            "turn/diff/updated"
        ]
    );
}

#[test]
fn codex_recovery_fixture_covers_partial_transport_failure_and_reconnect() {
    let path = format!(
        "{}/../../aap-schema/fixtures/codex-recovery.jsonl",
        env!("CARGO_MANIFEST_DIR")
    );
    let fixture = fs::read_to_string(path).unwrap();
    let mut methods = Vec::new();
    let mut events = Vec::new();
    for line in fixture.lines() {
        let lower = line.to_ascii_lowercase();
        assert!(!lower.contains("sk-"));
        assert!(!lower.contains("ghp_"));
        assert!(!lower.contains("authorization"));
        let message: Value = serde_json::from_str(line).unwrap();
        if let Some(method) = message["method"].as_str() {
            methods.push(method.to_owned());
        }
        if let Some(event) = message["params"]["event"].as_str() {
            events.push(event.to_owned());
        }
    }
    assert!(methods.contains(&"runtime/restart".into()));
    assert!(methods.contains(&"session/provider-read".into()));
    assert!(methods.contains(&"turn/cancel".into()));
    assert!(methods.contains(&"thread/compact/start".into()));
    assert!(methods.contains(&"item/commandExecution/requestApproval".into()));
    assert!(fixture.contains("\"decision\":\"decline\""));
    assert!(events.iter().any(|event| event == "item.delta"));
    assert!(events.iter().any(|event| event == "turn.failed"));
    assert!(events
        .iter()
        .any(|event| event == "turn.cancellation-acknowledged"));
    assert!(events.iter().any(|event| event == "turn.interrupted"));
    let failed = fixture
        .lines()
        .map(|line| serde_json::from_str::<Value>(line).unwrap())
        .find(|message| message["params"]["event"] == "turn.failed")
        .unwrap();
    assert_eq!(
        failed["params"]["item"]["data"]["schema_version"],
        "runtime-error/0.1"
    );
    assert_eq!(failed["params"]["item"]["data"]["class"], "transport");
    assert_eq!(failed["params"]["item"]["data"]["retryable"], true);
    let provider_failed = fixture
        .lines()
        .map(|line| serde_json::from_str::<Value>(line).unwrap())
        .find(|message| {
            message["params"]["event"] == "turn.failed"
                && message["params"]["item"]["data"]["class"] == "provider"
        })
        .unwrap();
    assert_eq!(
        provider_failed["params"]["item"]["data"]["retryable"],
        false
    );
}

#[test]
fn codex_provider_lifecycle_failure_fixture_is_redacted_and_recoverable() {
    let path = format!(
        "{}/../../aap-schema/fixtures/codex-provider-lifecycle-failures.jsonl",
        env!("CARGO_MANIFEST_DIR")
    );
    let fixture = fs::read_to_string(path).unwrap();
    let messages = fixture
        .lines()
        .map(|line| {
            let lower = line.to_ascii_lowercase();
            assert!(!lower.contains("sk-"));
            assert!(!lower.contains("ghp_"));
            assert!(!lower.contains("authorization"));
            serde_json::from_str::<Value>(line).unwrap()
        })
        .collect::<Vec<_>>();

    let error_codes = messages
        .iter()
        .filter_map(|message| message["error"]["code"].as_i64())
        .collect::<Vec<_>>();
    assert!(error_codes.contains(&-32141));
    assert!(error_codes.contains(&-32143));
    assert!(error_codes.contains(&-32145));
    assert!(error_codes.contains(&-32110));

    let compact = messages
        .iter()
        .find(|message| message["result"]["degradations"].is_array())
        .unwrap();
    assert_eq!(
        compact["result"]["degradations"][0]["feature"],
        "provider-thread-compact"
    );
    assert_eq!(compact["result"]["degradations"][0]["state"], "blocked");

    let health_states = messages
        .iter()
        .filter_map(|message| message["result"]["state"].as_str())
        .collect::<Vec<_>>();
    assert_eq!(health_states, ["exited", "unavailable", "running"]);
}

fn stable_envelope_valid(message: &Value) -> bool {
    if message["jsonrpc"] != "2.0" {
        return false;
    }
    let has_method = message.get("method").is_some();
    let has_id = message.get("id").is_some();
    let has_result = message.get("result").is_some();
    let has_error = message.get("error").is_some();
    if has_method {
        return !has_result && !has_error && message.get("params").is_none_or(Value::is_object);
    }
    has_id
        && has_result != has_error
        && (!has_error
            || (message["error"]["code"].is_i64() && message["error"]["message"].is_string()))
}

#[test]
fn stable_aap_schema_accepts_checked_fixtures_and_rejects_envelope_drift() {
    let schema_path = format!(
        "{}/../../aap-schema/stable/v0.1/aap.schema.json",
        env!("CARGO_MANIFEST_DIR")
    );
    let schema: Value = serde_json::from_str(&fs::read_to_string(schema_path).unwrap()).unwrap();
    assert_eq!(
        schema["$id"],
        "https://aegisy.cc/schemas/aap/stable/v0.1/aap.schema.json"
    );
    assert_eq!(schema["properties"]["jsonrpc"]["const"], "2.0");
    assert!(schema["properties"]["error"]["required"]
        .as_array()
        .is_some_and(|required| required.iter().any(|field| field == "code")));
    assert_eq!(schema["oneOf"].as_array().map(Vec::len), Some(4));

    let fixtures = [
        "codex-thread-lifecycle.jsonl",
        "codex-turn-metadata.jsonl",
        "codex-recovery.jsonl",
        "codex-provider-lifecycle-failures.jsonl",
        "turn-lifecycle.jsonl",
    ];
    for fixture_name in fixtures {
        let path = format!(
            "{}/../../aap-schema/fixtures/{fixture_name}",
            env!("CARGO_MANIFEST_DIR")
        );
        for line in fs::read_to_string(path).unwrap().lines() {
            let message: Value = serde_json::from_str(line).unwrap();
            assert!(
                stable_envelope_valid(&message),
                "invalid fixture envelope: {line}"
            );
        }
    }

    let request_and_result = json!({
        "jsonrpc": "2.0",
        "id": "same-id",
        "method": "fixture",
        "result": {}
    });
    assert!(!stable_envelope_valid(&request_and_result));
}

#[test]
fn rejects_malformed_requests() {
    let mut runtime = Runtime::default();
    let messages = runtime.handle_line("{broken");
    assert_eq!(messages[0]["error"]["code"], -32700);
}

#[test]
fn rejects_duplicate_request_ids() {
    let mut runtime = ready_runtime();
    let first = runtime.handle_line(&request("2", "session/start", json!({ "mode": "chat" })));
    assert!(first[0].get("result").is_some());
    let second = runtime.handle_line(&request("2", "session/start", json!({ "mode": "chat" })));
    assert_eq!(second[0]["error"]["code"], -32001);
}

#[test]
fn lists_sessions_with_mode_project_and_limit_filters() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-session-list-{unique}"));
    fs::create_dir_all(&root).unwrap();
    let mut runtime = ready_runtime();
    let project = runtime.handle_line(&request("2", "project/open", json!({ "root": root })));
    let project_id = project[0]["result"]["project"]["id"].as_str().unwrap();
    runtime.handle_line(&request("3", "session/start", json!({ "mode": "chat" })));
    runtime.handle_line(&request(
        "4",
        "session/start",
        json!({ "mode": "work", "project_id": project_id }),
    ));

    let all = runtime.handle_line(&request("5", "session/list", json!({})));
    assert_eq!(all[0]["result"]["sessions"].as_array().unwrap().len(), 2);
    assert_eq!(all[0]["result"]["durable"], false);
    let work = runtime.handle_line(&request(
        "6",
        "session/list",
        json!({ "mode": "work", "project_id": project_id, "limit": 10 }),
    ));
    let sessions = work[0]["result"]["sessions"].as_array().unwrap();
    assert_eq!(sessions.len(), 1);
    assert_eq!(sessions[0]["mode"], "work");
    assert_eq!(sessions[0]["project_id"], project_id);
    let invalid = runtime.handle_line(&request("7", "session/list", json!({ "limit": 201 })));
    assert_eq!(invalid[0]["error"]["code"], -32602);
    let _ = fs::remove_dir_all(root);
}

#[test]
fn searches_sessions_by_title_runtime_model_and_approved_transcript_text() {
    let mut runtime = ready_runtime();
    let first = runtime.handle_line(&request(
        "search-start-a",
        "session/start",
        json!({ "mode": "chat", "title": "TLS repair" }),
    ));
    let session_id = first[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    runtime.handle_line(&request(
        "search-turn-a",
        "turn/start",
        json!({
            "session_id": session_id,
            "input": "investigate stream disconnect",
            "idempotency_key": "search-turn-a"
        }),
    ));
    runtime.handle_line(&request(
        "search-start-b",
        "session/start",
        json!({ "mode": "chat", "title": "Unrelated" }),
    ));

    let title = runtime.handle_line(&request(
        "search-title",
        "session/search",
        json!({ "title": "tls", "limit": 10 }),
    ));
    assert_eq!(title[0]["result"]["schema_version"], "session-search/0.1");
    assert_eq!(title[0]["result"]["sessions"].as_array().unwrap().len(), 1);
    assert_eq!(title[0]["result"]["sessions"][0]["session_id"], session_id);
    assert_eq!(
        title[0]["result"]["sessions"][0]["matched_fields"][0],
        "title"
    );

    let transcript = runtime.handle_line(&request(
        "search-text",
        "session/search",
        json!({
            "text": "disconnect",
            "runtime": "preview",
            "model": "deterministic-echo"
        }),
    ));
    assert_eq!(
        transcript[0]["result"]["sessions"]
            .as_array()
            .unwrap()
            .len(),
        1
    );
    assert_eq!(
        transcript[0]["result"]["sessions"][0]["session_id"],
        session_id
    );
    assert_eq!(
        transcript[0]["result"]["sessions"][0]["matched_fields"],
        json!(["text", "model", "runtime"])
    );
    assert!(transcript[0]["result"]["sessions"][0]["runtime"]["backend_session_id"].is_null());

    let branch = runtime.handle_line(&request(
        "search-branch",
        "session/search",
        json!({ "branch": "main" }),
    ));
    assert_eq!(branch[0]["error"]["code"], -32028);
    let invalid = runtime.handle_line(&request(
        "search-limit",
        "session/search",
        json!({ "limit": 101 }),
    ));
    assert_eq!(invalid[0]["error"]["code"], -32602);
}

#[test]
fn operation_reconcile_persists_state_and_blocks_then_unblocks_session_writes() {
    let mut runtime = ready_runtime();
    let started = runtime.handle_line(&request(
        "reconcile-session",
        "session/start",
        json!({ "mode": "chat", "title": "Operation recovery" }),
    ));
    let session_id = started[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let unknown = runtime.handle_line(&request(
        "reconcile-unknown",
        "operation/reconcile",
        json!({
            "operation_id": "operation-unknown",
            "session_id": session_id,
            "kind": "turn",
            "evidence": {
                "event": "none",
                "process": "not-observed",
                "workspace": {"state": "not-required"},
                "git": {"state": "not-required"}
            }
        }),
    ));
    assert_eq!(
        unknown[0]["result"]["schema_version"],
        "operation-reconciliation-result/0.1"
    );
    assert_eq!(unknown[0]["result"]["durable"], false);
    assert_eq!(unknown[0]["result"]["reconciliation"]["state"], "unknown");
    assert_eq!(
        unknown[0]["result"]["reconciliation"]["writes_blocked"],
        true
    );
    let status = runtime.handle_line(&request(
        "reconcile-status",
        "operation/status",
        json!({"session_id": session_id}),
    ));
    assert_eq!(
        status[0]["result"]["schema_version"],
        "operation-reconciliation-status/0.1"
    );
    assert_eq!(status[0]["result"]["blocked"], true);
    assert_eq!(status[0]["result"]["recovery_action_available"], false);

    let blocked = runtime.handle_line(&request(
        "blocked-turn",
        "turn/start",
        json!({
            "session_id": session_id,
            "input": "must wait for reconciliation",
            "idempotency_key": "blocked-turn"
        }),
    ));
    assert_eq!(blocked[0]["error"]["code"], -32132);

    let completed = runtime.handle_line(&request(
        "reconcile-completed",
        "operation/reconcile",
        json!({
            "operation_id": "operation-unknown",
            "session_id": session_id,
            "kind": "turn",
            "evidence": {
                "event": "completed",
                "process": "not-running",
                "workspace": {"state": "not-required"},
                "git": {"state": "not-required"}
            }
        }),
    ));
    assert_eq!(
        completed[0]["result"]["reconciliation"]["writes_blocked"],
        false
    );
    let cleared_status = runtime.handle_line(&request(
        "reconcile-status-cleared",
        "operation/status",
        json!({"session_id": session_id}),
    ));
    assert_eq!(cleared_status[0]["result"]["blocked"], false);
    let allowed = runtime.handle_line(&request(
        "allowed-turn",
        "turn/start",
        json!({
            "session_id": session_id,
            "input": "continue after review",
            "idempotency_key": "allowed-turn"
        }),
    ));
    assert_eq!(allowed[0]["result"]["turn"]["state"], "started");
}

#[test]
fn operation_probe_reads_registered_workspace_without_returning_content() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-operation-probe-{unique}"));
    fs::create_dir_all(&root).unwrap();
    fs::write(root.join("main.rs"), "fn main() {}\n").unwrap();
    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request(
        "probe-project",
        "project/open",
        json!({
            "root": root
        }),
    ));
    let project_id = opened[0]["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let started = runtime.handle_line(&request(
        "probe-session",
        "session/start",
        json!({ "mode": "work", "project_id": project_id }),
    ));
    let session_id = started[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let first = runtime.handle_line(&request(
        "probe-first",
        "operation/probe",
        json!({
            "operation_id": "operation-workspace-probe",
            "session_id": session_id,
            "kind": "workspace-edit",
            "event": "none",
            "root_id": "root-1"
        }),
    ));
    assert_eq!(
        first[0]["result"]["schema_version"],
        "operation-reconciliation-probe/0.1"
    );
    assert_eq!(
        first[0]["result"]["evidence"]["workspace"]["state"],
        "not-observed"
    );
    let workspace_hash = first[0]["result"]["workspace_snapshot_hash"]
        .as_str()
        .unwrap()
        .to_owned();
    assert!(first[0]["result"]["evidence"].get("content").is_none());
    let same = runtime.handle_line(&request(
        "probe-same",
        "operation/probe",
        json!({
            "operation_id": "operation-workspace-probe",
            "session_id": session_id,
            "kind": "workspace-edit",
            "event": "none",
            "root_id": "root-1",
            "workspace_snapshot_hash": workspace_hash
        }),
    ));
    assert_eq!(
        same[0]["result"]["evidence"]["workspace"]["state"],
        "unchanged"
    );
    fs::write(
        root.join("main.rs"),
        "fn main() { println!(\"changed\"); }\n",
    )
    .unwrap();
    let changed = runtime.handle_line(&request(
        "probe-changed",
        "operation/probe",
        json!({
            "operation_id": "operation-workspace-probe",
            "session_id": session_id,
            "kind": "workspace-edit",
            "event": "none",
            "root_id": "root-1",
            "workspace_snapshot_hash": workspace_hash
        }),
    ));
    assert_eq!(
        changed[0]["result"]["evidence"]["workspace"]["state"],
        "changed"
    );
    let _ = fs::remove_dir_all(root);
}

#[test]
fn durable_operation_reconciliation_survives_runtime_restart() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-operation-reconcile-{unique}"));
    let data_root = root.join("data");
    fs::create_dir_all(&data_root).unwrap();
    let mut runtime = Runtime::with_store(&data_root).unwrap();
    runtime.handle_line(&request(
        "initialize",
        "initialize",
        json!({
            "protocol_version": "0.1",
            "client": {"name": "operation-reconcile", "version": "1"}
        }),
    ));
    runtime.handle_line(&request("initialized", "initialized", json!({})));
    let started = runtime.handle_line(&request(
        "session-start",
        "session/start",
        json!({"mode": "chat", "title": "Durable operation"}),
    ));
    let session_id = started[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let reconciled = runtime.handle_line(&request(
        "operation",
        "operation/reconcile",
        json!({
            "operation_id": "operation-restart",
            "session_id": session_id,
            "kind": "turn",
            "evidence": {
                "event": "none",
                "process": "not-observed",
                "workspace": {"state": "not-required"},
                "git": {"state": "not-required"}
            }
        }),
    ));
    assert_eq!(reconciled[0]["result"]["durable"], true);
    drop(runtime);

    let mut restarted = Runtime::with_store(&data_root).unwrap();
    restarted.handle_line(&request(
        "initialize-2",
        "initialize",
        json!({
            "protocol_version": "0.1",
            "client": {"name": "operation-reconcile", "version": "1"}
        }),
    ));
    restarted.handle_line(&request("initialized-2", "initialized", json!({})));
    let blocked = restarted.handle_line(&request(
        "blocked-after-restart",
        "turn/start",
        json!({
            "session_id": session_id,
            "input": "must remain blocked",
            "idempotency_key": "blocked-after-restart"
        }),
    ));
    assert_eq!(blocked[0]["error"]["code"], -32132);
    let _ = fs::remove_dir_all(root);
}

#[test]
fn operation_probe_uses_durable_terminal_event_when_event_is_omitted() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-operation-event-probe-{unique}"));
    let data_root = root.join("data");
    let project_root = root.join("project");
    fs::create_dir_all(&data_root).unwrap();
    fs::create_dir_all(&project_root).unwrap();
    let mut runtime = Runtime::with_store(&data_root).unwrap();
    runtime.handle_line(&request(
        "initialize",
        "initialize",
        json!({
            "protocol_version": "0.1",
            "client": {"name": "operation-event-probe", "version": "1"}
        }),
    ));
    runtime.handle_line(&request("initialized", "initialized", json!({})));
    let opened = runtime.handle_line(&request(
        "project",
        "project/open",
        json!({"root": project_root}),
    ));
    let project_id = opened[0]["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let started = runtime.handle_line(&request(
        "session",
        "session/start",
        json!({"mode": "work", "project_id": project_id}),
    ));
    let session_id = started[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let turn = runtime.handle_line(&request(
        "turn",
        "turn/start",
        json!({
            "session_id": session_id,
            "input": "probe terminal event",
            "idempotency_key": "probe-terminal-event"
        }),
    ));
    let turn_id = turn[0]["result"]["turn"]["id"].as_str().unwrap().to_owned();
    let probed = runtime.handle_line(&request(
        "probe",
        "operation/probe",
        json!({
            "operation_id": turn_id,
            "session_id": session_id,
            "kind": "turn"
        }),
    ));
    assert_eq!(probed[0]["result"]["event_source"], "durable-event-stream");
    assert_eq!(probed[0]["result"]["evidence"]["event"], "completed");
    assert_eq!(probed[0]["result"]["evidence"]["process"], "not-running");
    let serialized = serde_json::to_string(&probed).unwrap();
    assert!(!serialized.contains("probe terminal event"));
    let _ = fs::remove_dir_all(root);
}

#[test]
fn project_trust_review_is_read_only_and_content_free() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-trust-review-{unique}"));
    fs::create_dir_all(root.join(".git/hooks")).unwrap();
    fs::write(
        root.join("AGENTS.md"),
        "do not persist this instruction body",
    )
    .unwrap();
    fs::write(root.join(".git/hooks/pre-commit"), "echo hook-body").unwrap();
    let mut runtime = ready_runtime();
    let reviewed = runtime.handle_line(&request(
        "2",
        "project/trust-review",
        json!({ "root": root }),
    ));
    assert_eq!(
        reviewed[0]["result"]["review"]["schema_version"],
        "project-trust-review/0.1"
    );
    assert_eq!(reviewed[0]["result"]["review"]["required"], true);
    assert_eq!(
        reviewed[0]["result"]["review"]["instructions"][0]["content"],
        "not-read"
    );
    assert_eq!(
        reviewed[0]["result"]["review"]["policy_impact"]["agent_execution"],
        "read-only"
    );
    let serialized = serde_json::to_string(&reviewed).unwrap();
    assert!(!serialized.contains("do not persist this instruction body"));
    assert!(!serialized.contains("echo hook-body"));
    let _ = fs::remove_dir_all(root);
}

#[test]
fn workspace_instructions_are_deterministic_untrusted_and_root_scoped() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-instructions-{unique}"));
    fs::create_dir_all(root.join("src")).unwrap();
    fs::write(root.join("AGENTS.md"), "project instruction\n").unwrap();
    fs::write(root.join("src/CLAUDE.md"), "nested instruction\n").unwrap();
    fs::write(
        root.join("src/CODEX.md"),
        "API_KEY=sk-12345678901234567890\n",
    )
    .unwrap();
    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request("open", "project/open", json!({ "root": root })));
    let project_id = opened[0]["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let discovered = runtime.handle_line(&request(
        "instructions",
        "workspace/instructions",
        json!({
            "project_id": project_id,
            "target_path": "src",
            "include_content": true
        }),
    ));
    assert_eq!(
        discovered[0]["result"]["schema_version"],
        "instruction-discovery/0.1"
    );
    assert_eq!(discovered[0]["result"]["merge_order"], "weakest-first");
    assert_eq!(discovered[0]["result"]["content_trust"], "untrusted-data");
    assert_eq!(
        discovered[0]["result"]["authority_effect"],
        "none; instructions cannot grant permissions, execute commands, enable hooks, or authorize network"
    );
    let entries = discovered[0]["result"]["entries"].as_array().unwrap();
    let included = entries
        .iter()
        .filter(|entry| entry["included"] == true)
        .collect::<Vec<_>>();
    assert_eq!(included.len(), 2);
    assert_eq!(included[0]["relative_path"], "project/AGENTS.md");
    assert_eq!(included[1]["relative_path"], "nested/src/CLAUDE.md");
    assert_eq!(included[0]["content"], "project instruction\n");
    assert_eq!(included[1]["content"], "nested instruction\n");
    assert!(entries.iter().any(|entry| {
        entry["rejection_reason"] == "secret-shaped-content" && entry["content"].is_null()
    }));
    assert!(entries
        .iter()
        .all(|entry| entry["trust"] == "untrusted-data"));
    let serialized = serde_json::to_string(&discovered).unwrap();
    assert!(!serialized.contains("API_KEY=sk-"));

    let metadata_only = runtime.handle_line(&request(
        "instructions-metadata",
        "workspace/instructions",
        json!({ "project_id": project_id, "target_path": "src" }),
    ));
    assert!(metadata_only[0]["result"]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|entry| entry["included"] == true)
        .all(|entry| entry.get("content").is_none()));
    let _ = fs::remove_dir_all(root);
}

#[test]
fn project_trust_acknowledgement_survives_restart_and_invalidates_on_content_change() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-trust-state-{unique}"));
    let data = root.join("data");
    let project = root.join("project");
    fs::create_dir_all(&data).unwrap();
    fs::create_dir_all(&project).unwrap();
    fs::write(project.join("AGENTS.md"), "review version one\n").unwrap();

    let mut runtime = Runtime::with_store(&data).unwrap();
    runtime.handle_line(&request(
        "1",
        "initialize",
        json!({ "protocol_version": "0.1", "client": { "name": "test", "version": "1" } }),
    ));
    runtime.handle_line(&request("initialized", "initialized", json!({})));
    let opened = runtime.handle_line(&request("2", "project/open", json!({ "root": project })));
    let project_id = opened[0]["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let root_identity = opened[0]["result"]["identity"]["root_identity"]
        .as_str()
        .unwrap()
        .to_owned();
    let first_review_id = opened[0]["result"]["trust_review"]["review_id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(
        opened[0]["result"]["trust_review"]["trust_state"],
        "unreviewed"
    );
    assert_eq!(opened[0]["result"]["trust_review"]["required"], true);

    let acknowledged = runtime.handle_line(&request(
        "3",
        "project/trust-acknowledge",
        json!({
            "project_id": project_id,
            "root_id": "root-1",
            "root_identity": root_identity,
            "review_id": first_review_id
        }),
    ));
    assert_eq!(acknowledged[0]["result"]["trust_state"], "acknowledged");
    assert_eq!(
        acknowledged[0]["result"]["permission_effect"],
        "none-read-only-boundary-unchanged"
    );
    assert_eq!(acknowledged[0]["result"]["review"]["required"], false);
    let acknowledged_at = acknowledged[0]["result"]["acknowledgement"]["acknowledged_at_ms"]
        .as_u64()
        .unwrap();
    let duplicate = runtime.handle_line(&request(
        "4",
        "project/trust-acknowledge",
        json!({
            "project_id": project_id,
            "root_id": "root-1",
            "root_identity": root_identity,
            "review_id": first_review_id
        }),
    ));
    assert_eq!(
        duplicate[0]["result"]["acknowledgement"]["acknowledged_at_ms"],
        acknowledged_at
    );
    drop(runtime);

    let mut restarted = Runtime::with_store(&data).unwrap();
    restarted.handle_line(&request(
        "5",
        "initialize",
        json!({ "protocol_version": "0.1", "client": { "name": "test", "version": "1" } }),
    ));
    restarted.handle_line(&request("initialized-2", "initialized", json!({})));
    let reopened = restarted.handle_line(&request("6", "project/open", json!({ "root": project })));
    assert_eq!(
        reopened[0]["result"]["trust_review"]["trust_state"],
        "acknowledged"
    );
    assert_eq!(reopened[0]["result"]["trust_review"]["required"], false);

    fs::write(project.join("AGENTS.md"), "review version two\n").unwrap();
    let changed = restarted.handle_line(&request("7", "project/open", json!({ "root": project })));
    assert_eq!(
        changed[0]["result"]["trust_review"]["trust_state"],
        "invalidated"
    );
    assert_eq!(
        changed[0]["result"]["trust_review"]["invalidation_reason"],
        "review-content-changed"
    );
    assert_eq!(changed[0]["result"]["trust_review"]["required"], true);
    let stale = restarted.handle_line(&request(
        "8",
        "project/trust-acknowledge",
        json!({
            "project_id": project_id,
            "root_id": "root-1",
            "root_identity": root_identity,
            "review_id": first_review_id
        }),
    ));
    assert_eq!(stale[0]["error"]["code"], -32042);

    let serialized = serde_json::to_string(&changed).unwrap();
    assert!(!serialized.contains("review version one"));
    assert!(!serialized.contains("review version two"));
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn project_roots_keep_independent_access_and_remove_only_secondary_roots() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-root-primary-{unique}"));
    let extra = std::env::temp_dir().join(format!("aegisy-aap-root-extra-{unique}"));
    let writable = std::env::temp_dir().join(format!("aegisy-aap-root-write-{unique}"));
    fs::create_dir_all(&root).unwrap();
    fs::create_dir_all(&extra).unwrap();
    fs::create_dir_all(&writable).unwrap();
    fs::write(extra.join("readonly.txt"), "read-only\n").unwrap();
    fs::write(extra.join("extra.rs"), "pub fn extra_root() {}\n").unwrap();
    fs::write(writable.join("writable.txt"), "before\n").unwrap();
    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request("2", "project/open", json!({ "root": root })));
    let project_id = opened[0]["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let initial = runtime.handle_line(&request(
        "3",
        "project/root-list",
        json!({ "project_id": project_id }),
    ));
    assert_eq!(initial[0]["result"]["roots"].as_array().unwrap().len(), 1);
    let added = runtime.handle_line(&request(
        "4",
        "project/root-add",
        json!({ "project_id": project_id, "root": extra, "access": "read" }),
    ));
    assert_eq!(added[0]["result"]["root"]["access"], "read");
    let root_id = added[0]["result"]["root"]["root_id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_ne!(root_id, "root-1");
    assert_eq!(added[0]["result"]["roots"].as_array().unwrap().len(), 2);
    let listed = runtime.handle_line(&request(
        "5",
        "workspace/list",
        json!({ "project_id": project_id, "root_id": root_id, "path": "" }),
    ));
    assert_eq!(listed[0]["result"]["root_id"], root_id);
    assert_eq!(listed[0]["result"]["root_access"], "read");
    assert!(listed[0]["result"]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .any(|entry| entry["path"] == "readonly.txt"));
    let read = runtime.handle_line(&request(
        "6",
        "workspace/read",
        json!({ "project_id": project_id, "root_id": root_id, "path": "readonly.txt" }),
    ));
    assert_eq!(read[0]["result"]["content"], "read-only\n");
    let denied = runtime.handle_line(&request(
        "7",
        "workspace/save-user-text",
        json!({
            "project_id": project_id,
            "root_id": root_id,
            "path": "readonly.txt",
            "content": "changed\n",
            "expected_revision": read[0]["result"]["revision"],
            "encoding": "utf-8",
            "newline": "lf",
            "origin": "user"
        }),
    ));
    assert_eq!(denied[0]["error"]["code"], -32046);
    assert_eq!(
        fs::read_to_string(extra.join("readonly.txt")).unwrap(),
        "read-only\n"
    );
    let extra_search = runtime.handle_line(&request(
        "root-search",
        "workspace/search",
        json!({
            "project_id": project_id,
            "root_id": root_id,
            "search_id": "secondary-root-search",
            "query": "read-only",
            "mode": "text",
            "limit": 10
        }),
    ));
    assert_eq!(extra_search[0]["result"]["root_id"], root_id);
    assert!(extra_search[0]["result"]["matches"]
        .as_array()
        .unwrap()
        .iter()
        .any(|entry| entry["path"] == "readonly.txt"));
    let extra_cancel = runtime.handle_line(&request(
        "root-search-cancel",
        "workspace/search/cancel",
        json!({
            "project_id": project_id,
            "root_id": root_id,
            "search_id": "secondary-root-search"
        }),
    ));
    assert_eq!(extra_cancel[0]["result"]["cancelled"], true);
    let cancelled_extra_search = runtime.handle_line(&request(
        "root-search-after-cancel",
        "workspace/search",
        json!({
            "project_id": project_id,
            "root_id": root_id,
            "search_id": "secondary-root-search",
            "query": "extra_root",
            "mode": "text"
        }),
    ));
    assert_eq!(cancelled_extra_search[0]["result"]["cancelled"], true);
    let extra_index = runtime.handle_line(&request(
        "root-index",
        "workspace/index",
        json!({ "project_id": project_id, "root_id": root_id, "index_id": "secondary-root-index" }),
    ));
    assert_eq!(extra_index[0]["result"]["root_id"], root_id);
    assert!(extra_index[0]["result"]["symbols"]
        .as_array()
        .unwrap()
        .iter()
        .any(|symbol| symbol["name"] == "extra_root"));
    let extra_map = runtime.handle_line(&request(
        "root-map",
        "workspace/repository-map",
        json!({ "project_id": project_id, "root_id": root_id, "token_budget": 256 }),
    ));
    assert_eq!(extra_map[0]["result"]["root_id"], root_id);
    assert!(extra_map[0]["result"]["included_files"]
        .as_array()
        .unwrap()
        .iter()
        .any(|path| path == "extra.rs"));
    #[cfg(unix)]
    {
        let linked = std::env::temp_dir().join(format!("aegisy-aap-root-link-{unique}"));
        std::os::unix::fs::symlink(&extra, &linked).unwrap();
        let rejected_link = runtime.handle_line(&request(
            "root-link",
            "project/root-add",
            json!({ "project_id": project_id, "root": linked, "access": "read" }),
        ));
        assert_eq!(rejected_link[0]["error"]["code"], -32020);
        fs::remove_file(linked).unwrap();
    }

    let write_added = runtime.handle_line(&request(
        "8",
        "project/root-add",
        json!({ "project_id": project_id, "root": writable, "access": "write" }),
    ));
    let write_root_id = write_added[0]["result"]["root"]["root_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let write_read = runtime.handle_line(&request(
        "9",
        "workspace/read",
        json!({ "project_id": project_id, "root_id": write_root_id, "path": "writable.txt" }),
    ));
    let saved = runtime.handle_line(&request(
        "10",
        "workspace/save-user-text",
        json!({
            "project_id": project_id,
            "root_id": write_root_id,
            "path": "writable.txt",
            "content": "after\n",
            "expected_revision": write_read[0]["result"]["revision"],
            "encoding": "utf-8",
            "newline": "lf",
            "origin": "user"
        }),
    ));
    assert_eq!(saved[0]["result"]["root_access"], "write");
    assert_eq!(
        fs::read_to_string(writable.join("writable.txt")).unwrap(),
        "after\n"
    );

    let replaced_extra = extra.with_extension("replaced");
    fs::rename(&extra, &replaced_extra).unwrap();
    fs::create_dir_all(&extra).unwrap();
    let identity_changed = runtime.handle_line(&request(
        "identity-changed",
        "workspace/read",
        json!({ "project_id": project_id, "root_id": root_id, "path": "readonly.txt" }),
    ));
    assert_eq!(identity_changed[0]["error"]["code"], -32020);

    let removed = runtime.handle_line(&request(
        "11",
        "project/root-remove",
        json!({ "project_id": project_id, "root_id": root_id }),
    ));
    assert_eq!(removed[0]["result"]["roots"].as_array().unwrap().len(), 2);
    let primary = runtime.handle_line(&request(
        "12",
        "project/root-remove",
        json!({ "project_id": project_id, "root_id": "root-1" }),
    ));
    assert_eq!(primary[0]["error"]["code"], -32027);
    let _ = fs::remove_dir_all(root);
    let _ = fs::remove_dir_all(extra);
    let _ = fs::remove_dir_all(replaced_extra);
    let _ = fs::remove_dir_all(writable);
}

#[test]
fn pinned_context_aap_persists_metadata_only_sets_and_reopens() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-pinned-context-{unique}"));
    let data_root = root.join("data");
    let project_root = root.join("project");
    fs::create_dir_all(&data_root).unwrap();
    fs::create_dir_all(&project_root).unwrap();
    let mut runtime = Runtime::with_store(&data_root).unwrap();
    let initialized = runtime.handle_line(&request(
        "initialize",
        "initialize",
        json!({
            "protocol_version": "0.1",
            "client": {"name": "pinned-context", "version": "1"}
        }),
    ));
    assert!(initialized[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "workspace.pinned-context.store"));
    assert!(initialized[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "turn.context.pinned-selected"));
    runtime.handle_line(&request("initialized", "initialized", json!({})));
    let opened = runtime.handle_line(&request(
        "project-open",
        "project/open",
        json!({"root": project_root}),
    ));
    let project_id = opened[0]["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let listed = runtime.handle_line(&request(
        "pins-empty",
        "workspace/pinned-context/list",
        json!({"project_id": project_id}),
    ));
    assert_eq!(listed[0]["result"]["persisted"], false);
    let set = json!({
        "schema_version": "pinned-context/0.1",
        "project_id": project_id.clone(),
        "items": [{
            "id": "pin-file",
            "project_id": project_id.clone(),
            "root_id": "root-1",
            "kind": "file",
            "source": "file-tree",
            "label": "src/main.rs",
            "reference": "src/main.rs",
            "content_hash": format!("sha256:{}", "a".repeat(64)),
            "bytes": 32,
            "freshness": "fresh",
            "priority": 850,
            "metadata": {}
        }]
    });
    let saved = runtime.handle_line(&request(
        "pins-save",
        "workspace/pinned-context/save",
        json!({"project_id": project_id.clone(), "set": set.clone()}),
    ));
    assert_eq!(saved[0]["result"]["persisted"], true);
    assert_eq!(saved[0]["result"]["content_bodies_included"], false);
    assert_eq!(saved[0]["result"]["event"]["persisted"], true);
    assert!(saved[0]["result"].get("content").is_none());
    let identity = saved[0]["result"]["set_identity"]
        .as_str()
        .unwrap()
        .to_owned();
    let event_sequence = saved[0]["result"]["event"]["sequence"].as_u64().unwrap();
    let duplicate = runtime.handle_line(&request(
        "pins-save-duplicate",
        "workspace/pinned-context/save",
        json!({"project_id": project_id.clone(), "set": set.clone()}),
    ));
    assert_eq!(duplicate[0]["result"]["persisted"], true);
    assert_eq!(duplicate[0]["result"]["event"]["sequence"], event_sequence);
    let stale = runtime.handle_line(&request(
        "pins-stale",
        "workspace/pinned-context/save",
        json!({"project_id": project_id.clone(), "set": set, "expected_set_identity": "stale"}),
    ));
    assert_eq!(stale[0]["error"]["code"], -32041);
    let removed = runtime.handle_line(&request(
        "pins-remove",
        "workspace/pinned-context/remove",
        json!({
            "project_id": project_id.clone(),
            "item_id": "pin-file",
            "expected_set_identity": identity
        }),
    ));
    assert_eq!(removed[0]["result"]["persisted"], true);
    assert_eq!(removed[0]["result"]["items"].as_array().unwrap().len(), 0);
    assert!(removed[0]["result"]["event"]["sequence"].as_u64().unwrap() > event_sequence);
    let removed_identity = removed[0]["result"]["set_identity"]
        .as_str()
        .unwrap()
        .to_owned();
    drop(runtime);

    let store = WorkbenchStore::open(&data_root).unwrap();
    let candidate = store
        .rebuild_project_projection_candidate(&project_id)
        .unwrap();
    assert!(candidate.source_complete);
    assert!(candidate.issues.is_empty());
    let events = store
        .read_session_events(&candidate.stream_id, 0, 64)
        .unwrap();
    let pinned_event = events
        .iter()
        .find(|event| event.event_kind == "project.pinned-context-updated")
        .expect("pinned context event");
    assert_eq!(pinned_event.payload["set_identity"], identity);
    assert_eq!(pinned_event.payload["content_bodies_persisted"], false);
    assert!(pinned_event.payload.get("items").is_none());
    drop(store);

    let mut restarted = Runtime::with_store(&data_root).unwrap();
    restarted.handle_line(&request(
        "initialize-2",
        "initialize",
        json!({
            "protocol_version": "0.1",
            "client": {"name": "pinned-context", "version": "1"}
        }),
    ));
    restarted.handle_line(&request("initialized-2", "initialized", json!({})));
    let reopened = restarted.handle_line(&request(
        "pins-reopen",
        "workspace/pinned-context/list",
        json!({"project_id": project_id}),
    ));
    assert_eq!(reopened[0]["result"]["persisted"], true);
    assert_eq!(reopened[0]["result"]["set_identity"], removed_identity);
    assert_eq!(reopened[0]["result"]["items"].as_array().unwrap().len(), 0);
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn pinned_context_aap_validates_project_blob_metadata_without_reading_body() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-pinned-context-blob-{unique}"));
    let data_root = root.join("data");
    let project_root = root.join("project");
    fs::create_dir_all(&data_root).unwrap();
    fs::create_dir_all(&project_root).unwrap();
    let mut runtime = Runtime::with_store(&data_root).unwrap();
    runtime.handle_line(&request(
        "initialize",
        "initialize",
        json!({
            "protocol_version": "0.1",
            "client": {"name": "pinned-context-blob", "version": "1"}
        }),
    ));
    runtime.handle_line(&request("initialized", "initialized", json!({})));
    let opened = runtime.handle_line(&request(
        "project-open",
        "project/open",
        json!({"root": project_root}),
    ));
    let project_id = opened[0]["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    drop(runtime);

    let content = b"pinned artifact metadata".to_vec();
    let content_hash = format!("{:x}", Sha256::digest(&content));
    let content_reference = format!("artifact:sha256:{content_hash}");
    let mut store = WorkbenchStore::open(&data_root).unwrap();
    store
        .put_durable_blob(DurableBlobWrite {
            reference_id: durable_blob_reference_id(
                None,
                Some(&project_id),
                "checkpoint",
                &project_id,
                &content_reference,
            ),
            content_reference: content_reference.clone(),
            session_id: None,
            project_id: Some(project_id.clone()),
            kind: DurableBlobKind::Artifact,
            media_type: "application/octet-stream".into(),
            owner_kind: "checkpoint".into(),
            owner_id: project_id.clone(),
            metadata: json!({"source": "pinned-context-test"}),
            content,
            created_at_ms: 2_000_000_000_000,
            retain_until_ms: 2_000_086_400_000,
        })
        .unwrap();
    drop(store);

    let mut runtime = Runtime::with_store(&data_root).unwrap();
    runtime.handle_line(&request(
        "initialize-2",
        "initialize",
        json!({
            "protocol_version": "0.1",
            "client": {"name": "pinned-context-blob", "version": "1"}
        }),
    ));
    runtime.handle_line(&request("initialized-2", "initialized", json!({})));
    let set = json!({
        "schema_version": "pinned-context/0.1",
        "project_id": project_id.clone(),
        "items": [{
            "id": "pin-artifact",
            "project_id": project_id.clone(),
            "kind": "artifact",
            "source": "artifact-store",
            "label": "artifact",
            "reference": content_reference,
            "content_hash": format!("sha256:{content_hash}"),
            "bytes": 24,
            "freshness": "fresh",
            "priority": 700,
            "metadata": {}
        }]
    });
    let saved = runtime.handle_line(&request(
        "blob-save",
        "workspace/pinned-context/save",
        json!({"project_id": project_id.clone(), "set": set.clone()}),
    ));
    assert_eq!(saved[0]["result"]["persisted"], true);
    assert_eq!(saved[0]["result"]["event"]["persisted"], true);

    let mismatched = json!({
        "schema_version": "pinned-context/0.1",
        "project_id": project_id.clone(),
        "items": [{
            "id": "pin-artifact",
            "project_id": project_id.clone(),
            "kind": "artifact",
            "source": "artifact-store",
            "label": "artifact",
            "reference": format!("artifact:sha256:{content_hash}"),
            "content_hash": format!("sha256:{}", "b".repeat(64)),
            "bytes": 24,
            "freshness": "fresh",
            "priority": 700,
            "metadata": {}
        }]
    });
    let rejected = runtime.handle_line(&request(
        "blob-mismatch",
        "workspace/pinned-context/save",
        json!({"project_id": project_id, "set": mismatched}),
    ));
    assert_eq!(rejected[0]["error"]["code"], -32044);
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn project_relink_requires_reviewed_identity_and_survives_restart() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-relink-{unique}"));
    let data = root.join("data");
    let original = root.join("original");
    let moved = root.join("moved");
    fs::create_dir_all(&data).unwrap();
    fs::create_dir_all(&original).unwrap();
    let original = original.canonicalize().unwrap();
    let mut runtime = Runtime::with_store(&data).unwrap();
    let initialized = runtime.handle_line(&request(
        "1",
        "initialize",
        json!({ "protocol_version": "0.1", "client": { "name": "test", "version": "1" } }),
    ));
    assert!(initialized[0].get("result").is_some());
    runtime.handle_line(&request("initialized", "initialized", json!({})));
    let opened = runtime.handle_line(&request("2", "project/open", json!({ "root": original })));
    let identity = opened[0]["result"]["identity"]["root_identity"]
        .as_str()
        .unwrap()
        .to_owned();
    fs::rename(&original, &moved).unwrap();
    let moved = moved.canonicalize().unwrap();
    let candidate = runtime.handle_line(&request("3", "project/open", json!({ "root": moved })));
    assert_eq!(candidate[0]["result"]["identity"]["relink_required"], true);
    let relinked = runtime.handle_line(&request(
        "4",
        "project/relink",
        json!({
            "project_id": "project-1",
            "root_id": "root-1",
            "root": moved,
            "expected_root_identity": identity
        }),
    ));
    assert_eq!(
        relinked[0]["result"]["identity"]["availability"],
        "available"
    );
    let stale = runtime.handle_line(&request(
        "5",
        "project/relink",
        json!({
            "project_id": "project-1",
            "root_id": "root-1",
            "root": moved,
            "expected_root_identity": "stale"
        }),
    ));
    assert_eq!(stale[0]["error"]["code"], -32042);
    let mut restarted = Runtime::with_store(&data).unwrap();
    let initialized = restarted.handle_line(&request(
        "6",
        "initialize",
        json!({ "protocol_version": "0.1", "client": { "name": "test", "version": "1" } }),
    ));
    assert!(initialized[0].get("result").is_some());
    restarted.handle_line(&request("initialized-2", "initialized", json!({})));
    let reopened = restarted.handle_line(&request("7", "project/open", json!({ "root": moved })));
    assert_eq!(reopened[0]["result"]["project"]["id"], "project-1");
    assert_eq!(
        reopened[0]["result"]["identity"]["availability"],
        "available"
    );
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn project_navigation_lists_pinned_state_and_unavailable_roots_after_restart() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-project-navigation-{unique}"));
    let data = root.join("data");
    let original = root.join("original");
    let moved = root.join("moved");
    fs::create_dir_all(&data).unwrap();
    fs::create_dir_all(&original).unwrap();
    let original = original.canonicalize().unwrap();
    let mut runtime = Runtime::with_store(&data).unwrap();
    runtime.handle_line(&request(
        "1",
        "initialize",
        json!({ "protocol_version": "0.1", "client": { "name": "test", "version": "1" } }),
    ));
    runtime.handle_line(&request("initialized", "initialized", json!({})));
    let opened = runtime.handle_line(&request("2", "project/open", json!({ "root": original })));
    let project_id = opened[0]["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let listed = runtime.handle_line(&request("3", "project/list", json!({})));
    assert_eq!(listed[0]["result"]["schema_version"], "project-list/0.1");
    assert_eq!(listed[0]["result"]["projects"].as_array().unwrap().len(), 1);
    assert_eq!(
        listed[0]["result"]["projects"][0]["availability"],
        "available"
    );
    assert_eq!(listed[0]["result"]["projects"][0]["pinned"], false);
    let pinned = runtime.handle_line(&request(
        "4",
        "project/navigation",
        json!({ "project_id": project_id, "pinned": true }),
    ));
    assert_eq!(pinned[0]["result"]["navigation"]["pinned"], true);
    let listed_pinned = runtime.handle_line(&request("5", "project/list", json!({})));
    assert_eq!(listed_pinned[0]["result"]["projects"][0]["pinned"], true);
    fs::rename(&original, &moved).unwrap();
    let moved = moved.canonicalize().unwrap();
    drop(runtime);

    let mut restarted = Runtime::with_store(&data).unwrap();
    restarted.handle_line(&request(
        "6",
        "initialize",
        json!({ "protocol_version": "0.1", "client": { "name": "test", "version": "1" } }),
    ));
    restarted.handle_line(&request("initialized-2", "initialized", json!({})));
    let unavailable = restarted.handle_line(&request("7", "project/list", json!({})));
    assert_eq!(unavailable[0]["result"]["projects"][0]["pinned"], true);
    assert_eq!(
        unavailable[0]["result"]["projects"][0]["availability"],
        "unavailable"
    );
    assert_eq!(
        unavailable[0]["result"]["projects"][0]["relink_required"],
        true
    );
    let candidate = restarted.handle_line(&request("8", "project/open", json!({ "root": moved })));
    let identity = candidate[0]["result"]["identity"]["stored_root_identity"]
        .as_str()
        .unwrap()
        .to_owned();
    let relinked = restarted.handle_line(&request(
        "9",
        "project/relink",
        json!({
            "project_id": project_id,
            "root_id": "root-1",
            "root": moved,
            "expected_root_identity": identity
        }),
    ));
    assert_eq!(
        relinked[0]["result"]["identity"]["availability"],
        "available"
    );
    let available = restarted.handle_line(&request("10", "project/list", json!({})));
    assert_eq!(available[0]["result"]["projects"][0]["pinned"], true);
    assert_eq!(
        available[0]["result"]["projects"][0]["availability"],
        "available"
    );
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn durable_preview_session_resumes_and_forks_at_a_completed_turn() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-session-resume-fork-{unique}"));
    let data = root.join("data");
    fs::create_dir_all(&data).unwrap();
    let mut runtime = Runtime::with_store(&data).unwrap();
    runtime.handle_line(&request(
        "1",
        "initialize",
        json!({ "protocol_version": "0.1", "client": { "name": "test", "version": "1" } }),
    ));
    runtime.handle_line(&request("initialized", "initialized", json!({})));
    let started = runtime.handle_line(&request(
        "2",
        "session/start",
        json!({ "mode": "chat", "title": "Resume source" }),
    ));
    let session_id = started[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let first_turn = runtime.handle_line(&request(
        "3",
        "turn/start",
        json!({ "session_id": session_id, "input": "first", "idempotency_key": "first" }),
    ));
    let first_turn_id = first_turn[0]["result"]["turn"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    drop(runtime);

    let mut restarted = Runtime::with_store(&data).unwrap();
    restarted.handle_line(&request(
        "4",
        "initialize",
        json!({ "protocol_version": "0.1", "client": { "name": "test", "version": "1" } }),
    ));
    restarted.handle_line(&request("initialized-2", "initialized", json!({})));
    let resumed = restarted.handle_line(&request(
        "5",
        "session/resume",
        json!({ "session_id": session_id }),
    ));
    assert_eq!(resumed[0]["result"]["resumed"], true);
    assert_eq!(
        resumed[0]["result"]["continuation"]["provider_state_available"],
        true
    );
    let second_turn = restarted.handle_line(&request(
        "6",
        "turn/start",
        json!({ "session_id": session_id, "input": "second", "idempotency_key": "second" }),
    ));
    assert_eq!(second_turn[0]["result"]["turn"]["state"], "started");
    let forked = restarted.handle_line(&request(
        "7",
        "session/fork",
        json!({ "session_id": session_id, "last_turn_id": first_turn_id, "title": "First turn fork" }),
    ));
    assert_eq!(forked[0]["result"]["schema_version"], "session-fork/0.1");
    assert_eq!(
        forked[0]["result"]["continuation"]["portable_history_items"],
        2
    );
    let fork_id = forked[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(
        forked[0]["result"]["receipt"]["session"]["lineage_kind"],
        "fork"
    );
    let fork_history = restarted.handle_line(&request(
        "8",
        "session/read",
        json!({ "session_id": fork_id, "limit": 10 }),
    ));
    assert_eq!(
        fork_history[0]["result"]["items"].as_array().unwrap().len(),
        2
    );
    drop(restarted);

    let mut reopened = Runtime::with_store(&data).unwrap();
    reopened.handle_line(&request(
        "9",
        "initialize",
        json!({ "protocol_version": "0.1", "client": { "name": "test", "version": "1" } }),
    ));
    reopened.handle_line(&request("initialized-3", "initialized", json!({})));
    let fork_resumed = reopened.handle_line(&request(
        "10",
        "session/resume",
        json!({ "session_id": fork_id }),
    ));
    assert_eq!(fork_resumed[0]["result"]["resumed"], true);
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn renames_archives_restores_and_blocks_archived_session_actions() {
    let mut runtime = ready_runtime();
    let started = runtime.handle_line(&request(
        "2",
        "session/start",
        json!({ "mode": "chat", "title": "Original" }),
    ));
    let session_id = started[0]["result"]["session"]["id"].as_str().unwrap();
    let renamed = runtime.handle_line(&request(
        "3",
        "session/title",
        json!({ "session_id": session_id, "title": "Renamed chat" }),
    ));
    assert_eq!(renamed[0]["result"]["title"], "Renamed chat");
    let archived = runtime.handle_line(&request(
        "4",
        "session/archive",
        json!({ "session_id": session_id }),
    ));
    assert_eq!(archived[0]["result"]["status"], "archived");
    let hidden = runtime.handle_line(&request("5", "session/list", json!({})));
    assert!(hidden[0]["result"]["sessions"]
        .as_array()
        .unwrap()
        .is_empty());
    let visible = runtime.handle_line(&request(
        "6",
        "session/list",
        json!({ "include_archived": true }),
    ));
    assert_eq!(visible[0]["result"]["sessions"][0]["status"], "archived");
    assert_eq!(visible[0]["result"]["sessions"][0]["title"], "Renamed chat");
    let blocked = runtime.handle_line(&request(
        "7",
        "turn/start",
        json!({
            "session_id": session_id,
            "input": "must not run",
            "idempotency_key": "archived-turn"
        }),
    ));
    assert_eq!(blocked[0]["error"]["code"], -32026);
    let restored = runtime.handle_line(&request(
        "8",
        "session/unarchive",
        json!({ "session_id": session_id }),
    ));
    assert_eq!(restored[0]["result"]["status"], "active");
    let turn = runtime.handle_line(&request(
        "9",
        "turn/start",
        json!({
            "session_id": session_id,
            "input": "available again",
            "idempotency_key": "restored-turn"
        }),
    ));
    assert!(turn[0].get("result").is_some());
    let invalid = runtime.handle_line(&request(
        "10",
        "session/title",
        json!({ "session_id": session_id, "title": "" }),
    ));
    assert_eq!(invalid[0]["error"]["code"], -32602);
}

#[test]
fn durable_session_deletion_protocol_requires_preview_and_supports_undo() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-session-delete-{unique}"));
    let data_root = root.join("data");
    fs::create_dir_all(&data_root).unwrap();
    let mut runtime = Runtime::with_store(&data_root).unwrap();
    let initialized = runtime.handle_line(&request(
        "initialize",
        "initialize",
        json!({
            "protocol_version": "0.1",
            "client": {"name": "deletion-protocol", "version": "1"}
        }),
    ));
    assert!(initialized[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "session.deletion.undo"));
    assert!(runtime
        .handle_line(r#"{"jsonrpc":"2.0","method":"initialized"}"#)
        .is_empty());

    let started = runtime.handle_line(&request(
        "start",
        "session/start",
        json!({"mode": "chat", "title": "Protocol deletion"}),
    ));
    let session_id = started[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let preview = runtime.handle_line(&request(
        "preview",
        "session/delete/preview",
        json!({"session_id": &session_id, "scope": "session-only"}),
    ));
    assert_eq!(
        preview[0]["result"]["schema_version"],
        "session-deletion-preview/0.1"
    );
    assert_eq!(preview[0]["result"]["session_count"], 1);
    let stale = runtime.handle_line(&request(
        "stale",
        "session/delete/schedule",
        json!({
            "session_id": &session_id,
            "scope": "session-only",
            "plan_hash": {"sha256": "0".repeat(64), "bytes": 1},
            "undo_window_ms": 7 * 24 * 60 * 60 * 1_000_u64
        }),
    ));
    assert_eq!(stale[0]["error"]["code"], -32130);

    let scheduled = runtime.handle_line(&request(
        "schedule",
        "session/delete/schedule",
        json!({
            "session_id": &session_id,
            "scope": "session-only",
            "plan_hash": preview[0]["result"]["plan_hash"].clone(),
            "undo_window_ms": 7 * 24 * 60 * 60 * 1_000_u64
        }),
    ));
    assert_eq!(scheduled[0]["result"]["state"], "pending");
    let deletion_id = scheduled[0]["result"]["deletion_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let searched_pending = runtime.handle_line(&request(
        "search-pending",
        "session/search",
        json!({"title": "Protocol deletion", "include_archived": true}),
    ));
    assert_eq!(
        searched_pending[0]["result"]["sessions"][0]["deletion_pending"],
        true
    );
    assert_eq!(
        searched_pending[0]["result"]["sessions"][0]["deletion"]["deletion_id"],
        deletion_id
    );
    assert_eq!(
        searched_pending[0]["result"]["sessions"][0]["recovery_required"],
        false
    );
    let frozen = runtime.handle_line(&request(
        "frozen",
        "turn/start",
        json!({
            "session_id": &session_id,
            "input": "must remain frozen",
            "idempotency_key": "pending-delete-turn"
        }),
    ));
    assert_eq!(frozen[0]["error"]["code"], -32131);
    let operation_status = runtime.handle_line(&request(
        "operation-status-pending",
        "operation/status",
        json!({"session_id": &session_id}),
    ));
    assert_eq!(
        operation_status[0]["result"]["schema_version"],
        "operation-reconciliation-status/0.1"
    );
    assert_eq!(operation_status[0]["result"]["blocked"], false);
    let status = runtime.handle_line(&request(
        "status",
        "session/deletion/status",
        json!({"session_id": &session_id}),
    ));
    assert_eq!(status[0]["result"]["deletion"]["deletion_id"], deletion_id);
    let undone = runtime.handle_line(&request(
        "undo",
        "session/delete/undo",
        json!({"deletion_id": &deletion_id}),
    ));
    assert_eq!(undone[0]["result"]["state"], "cancelled");

    let _ = fs::remove_dir_all(root);
}

#[test]
fn portable_session_protocol_previews_exports_validates_and_imports_a_copy() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-portable-session-{unique}"));
    let data_root = root.join("data");
    fs::create_dir_all(&data_root).unwrap();
    let mut runtime = Runtime::with_store(&data_root).unwrap();
    let initialized = runtime.handle_line(&request(
        "initialize",
        "initialize",
        json!({
            "protocol_version": "0.1",
            "client": {"name": "portable-session-protocol", "version": "1"}
        }),
    ));
    assert!(initialized[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "session.portable.import"));
    assert!(runtime
        .handle_line(r#"{"jsonrpc":"2.0","method":"initialized"}"#)
        .is_empty());

    let started = runtime.handle_line(&request(
        "start",
        "session/start",
        json!({"mode": "chat", "title": "Portable protocol"}),
    ));
    let source_session_id = started[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let turn = runtime.handle_line(&request(
        "turn",
        "turn/start",
        json!({
            "session_id": &source_session_id,
            "input": "portable conversation",
            "idempotency_key": "portable-protocol-turn"
        }),
    ));
    assert!(turn[0].get("result").is_some());

    let export_preview = runtime.handle_line(&request(
        "export-preview",
        "session/export/preview",
        json!({"session_id": &source_session_id}),
    ));
    assert_eq!(
        export_preview[0]["result"]["schema_version"],
        "portable-session-export-preview/0.1"
    );
    assert_eq!(export_preview[0]["result"]["item_count"], 2);
    assert!(export_preview[0]["result"]["content_categories"]
        .as_array()
        .unwrap()
        .iter()
        .any(|category| category == "conversation-transcript"));
    let exported = runtime.handle_line(&request(
        "export",
        "session/export",
        json!({
            "session_id": &source_session_id,
            "package_hash": export_preview[0]["result"]["package_hash"].clone()
        }),
    ));
    let package = exported[0]["result"]["package"].clone();
    let serialized = serde_json::to_string(&package).unwrap();
    assert!(!serialized.contains("environment_identity"));
    assert!(!serialized.contains("provider_response_id"));

    let reject_preview = runtime.handle_line(&request(
        "import-preview-reject",
        "session/import/preview",
        json!({"package": &package, "collision_strategy": "reject"}),
    ));
    assert_eq!(reject_preview[0]["result"]["collision_strategy"], "reject");
    assert_eq!(
        reject_preview[0]["result"]["source_session_collision"],
        true
    );
    assert_eq!(reject_preview[0]["result"]["source_item_id_collisions"], 2);
    assert!(reject_preview[0]["result"]["blocking_reasons"]
        .as_array()
        .unwrap()
        .iter()
        .any(|reason| reason == "portable-import-collision-rejected"));
    let rejected = runtime.handle_line(&request(
        "import-reject",
        "session/import",
        json!({"package": &package, "collision_strategy": "reject"}),
    ));
    assert_eq!(rejected[0]["error"]["code"], -32143);

    let copy_preview = runtime.handle_line(&request(
        "import-preview-copy",
        "session/import/preview",
        json!({"package": &package, "collision_strategy": "copy"}),
    ));
    assert_eq!(
        copy_preview[0]["result"]["copy_will_remap_identifiers"],
        true
    );
    assert!(copy_preview[0]["result"]["blocking_reasons"]
        .as_array()
        .unwrap()
        .is_empty());
    let imported = runtime.handle_line(&request(
        "import-copy",
        "session/import",
        json!({"package": &package, "collision_strategy": "copy"}),
    ));
    assert_eq!(
        imported[0]["result"]["schema_version"],
        "portable-session-import-result/0.1"
    );
    assert_eq!(
        imported[0]["result"]["receipt"]["linked_source_session"],
        true
    );
    assert_eq!(
        imported[0]["result"]["continuation"]["provider_state_available"],
        false
    );
    let imported_session_id = imported[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_ne!(imported_session_id, source_session_id);
    let replay = runtime.handle_line(&request(
        "import-read",
        "session/read",
        json!({"session_id": &imported_session_id}),
    ));
    assert_eq!(replay[0]["result"]["items"].as_array().unwrap().len(), 2);
    assert_eq!(replay[0]["result"]["consistency"]["consistent"], true);
    assert_ne!(
        replay[0]["result"]["items"][0]["id"],
        package["content"]["items"][0]["source_item_id"]
    );

    let mut tampered = package;
    tampered["content"]["title"] = Value::String("tampered package".into());
    let invalid = runtime.handle_line(&request(
        "import-tampered",
        "session/import/preview",
        json!({"package": tampered, "collision_strategy": "copy"}),
    ));
    assert_eq!(invalid[0]["error"]["code"], -32141);

    drop(runtime);
    let mut restarted = Runtime::with_store(&data_root).unwrap();
    let initialized = restarted.handle_line(&request(
        "restart-initialize",
        "initialize",
        json!({
            "protocol_version": "0.1",
            "client": {"name": "portable-session-restart", "version": "1"}
        }),
    ));
    assert_eq!(initialized[0]["result"]["backend"]["status"], "ready");
    restarted.handle_line(r#"{"jsonrpc":"2.0","method":"initialized"}"#);
    let replay = restarted.handle_line(&request(
        "restart-import-read",
        "session/read",
        json!({"session_id": &imported_session_id}),
    ));
    assert_eq!(replay[0]["result"]["items"].as_array().unwrap().len(), 2);
    assert_eq!(replay[0]["result"]["consistency"]["consistent"], true);

    let _ = fs::remove_dir_all(root);
}

#[test]
fn emits_ordered_turn_lifecycle() {
    let mut runtime = ready_runtime();
    let session = runtime.handle_line(&request("2", "session/start", json!({ "mode": "chat" })));
    let session_id = session[0]["result"]["session"]["id"].as_str().unwrap();
    let messages = runtime.handle_line(&request(
        "3",
        "turn/start",
        json!({
            "session_id": session_id,
            "input": "hello",
            "idempotency_key": "turn-client-1"
        }),
    ));
    assert_eq!(messages.len(), 6);
    assert_eq!(messages[1]["params"]["event"], "turn.started");
    assert_eq!(messages[2]["params"]["item"]["role"], "user");
    assert_eq!(messages[3]["params"]["event"], "item.delta");
    assert_eq!(messages[4]["params"]["item"]["state"], "completed");
    assert_eq!(messages[5]["params"]["event"], "turn.completed");
    for pair in messages[1..].windows(2) {
        assert!(
            pair[0]["params"]["sequence"].as_u64().unwrap()
                < pair[1]["params"]["sequence"].as_u64().unwrap()
        );
    }
}

#[test]
fn session_read_pages_latest_history_backwards_with_strict_cursors() {
    let mut runtime = ready_runtime();
    let session = runtime.handle_line(&request("2", "session/start", json!({ "mode": "chat" })));
    let session_id = session[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    for turn in 0..4 {
        let messages = runtime.handle_line(&request(
            &format!("turn-{turn}"),
            "turn/start",
            json!({
                "session_id": session_id,
                "input": format!("message {turn}"),
                "idempotency_key": format!("history-turn-{turn}")
            }),
        ));
        assert!(messages[0].get("result").is_some());
    }

    let latest = runtime.handle_line(&request(
        "history-latest",
        "session/read",
        json!({ "session_id": session_id, "limit": 3 }),
    ));
    let latest_items = latest[0]["result"]["items"].as_array().unwrap();
    assert_eq!(
        latest_items
            .iter()
            .map(|item| item["sequence"].as_u64().unwrap())
            .collect::<Vec<_>>(),
        vec![6, 7, 8]
    );
    assert_eq!(latest[0]["result"]["history_page"]["latest_sequence"], 8);
    assert_eq!(
        latest[0]["result"]["history_page"]["older_cursor"],
        "before:6"
    );

    let middle = runtime.handle_line(&request(
        "history-middle",
        "session/read",
        json!({ "session_id": session_id, "cursor": "before:6", "limit": 3 }),
    ));
    assert_eq!(
        middle[0]["result"]["items"]
            .as_array()
            .unwrap()
            .iter()
            .map(|item| item["sequence"].as_u64().unwrap())
            .collect::<Vec<_>>(),
        vec![3, 4, 5]
    );
    assert_eq!(
        middle[0]["result"]["history_page"]["older_cursor"],
        "before:3"
    );

    let oldest = runtime.handle_line(&request(
        "history-oldest",
        "session/read",
        json!({ "session_id": session_id, "cursor": "before:3", "limit": 3 }),
    ));
    assert_eq!(
        oldest[0]["result"]["items"]
            .as_array()
            .unwrap()
            .iter()
            .map(|item| item["sequence"].as_u64().unwrap())
            .collect::<Vec<_>>(),
        vec![1, 2]
    );
    assert_eq!(oldest[0]["result"]["history_page"]["has_older"], false);
    assert!(oldest[0]["result"]["history_page"]["older_cursor"].is_null());

    for (id, params) in [
        (
            "history-bad-cursor",
            json!({ "session_id": session_id, "cursor": "6", "limit": 3 }),
        ),
        (
            "history-leading-zero",
            json!({ "session_id": session_id, "cursor": "before:06", "limit": 3 }),
        ),
        (
            "history-out-of-range",
            json!({ "session_id": session_id, "cursor": "before:99", "limit": 3 }),
        ),
        (
            "history-bad-limit",
            json!({ "session_id": session_id, "limit": 201 }),
        ),
    ] {
        let rejected = runtime.handle_line(&request(id, "session/read", params));
        assert_eq!(rejected[0]["error"]["code"], -32602);
    }
}

#[test]
fn turn_context_is_bounded_project_scoped_and_authoritatively_read() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-context-{unique}"));
    fs::create_dir_all(&root).unwrap();
    let initialized = Command::new("git").arg("init").arg(&root).output().unwrap();
    assert!(initialized.status.success());
    fs::write(root.join(".gitignore"), "ignored.txt\n").unwrap();
    fs::write(root.join("visible.rs"), "fn authoritative() {}\n").unwrap();
    fs::write(root.join("ignored.txt"), "secret\n").unwrap();

    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request("2", "project/open", json!({ "root": root })));
    let project_id = opened[0]["result"]["project"]["id"].as_str().unwrap();
    let session = runtime.handle_line(&request(
        "3",
        "session/start",
        json!({ "mode": "work", "project_id": project_id }),
    ));
    let session_id = session[0]["result"]["session"]["id"].as_str().unwrap();
    let messages = runtime.handle_line(&request(
        "4",
        "turn/start",
        json!({
            "session_id": session_id,
            "input": "review this context",
            "idempotency_key": "turn-context-1",
            "context": [
                {
                    "id": "context-file",
                    "kind": "file",
                    "label": "visible.rs",
                    "origin": "file-tree",
                    "path": "visible.rs",
                    "revision": "content:stale"
                },
                {
                    "id": "context-selection",
                    "kind": "selection",
                    "label": "visible.rs:1",
                    "origin": "editor-selection",
                    "path": "visible.rs",
                    "content": "authoritative",
                    "line": 1,
                    "column": 4
                }
            ]
        }),
    ));
    assert_eq!(messages[0]["result"]["context"]["item_count"], 2);
    assert_eq!(messages[0]["result"]["context"]["truncated"], false);
    assert_eq!(
        messages[0]["result"]["context"]["manifest"]["schema_version"],
        "context-manifest/0.1"
    );
    assert_eq!(
        messages[0]["result"]["context"]["manifest"]["entries"]
            .as_array()
            .unwrap()
            .len(),
        2
    );
    assert_eq!(
        messages[0]["result"]["context"]["manifest"]["entries"][0]["freshness"],
        "stale"
    );
    assert!(
        messages[0]["result"]["context"]["manifest"]["entries"][0]["content_hash"]
            .as_str()
            .unwrap()
            .starts_with("sha256:")
    );
    assert!(messages[0]["result"]["context"]["manifest"]["entries"][0]
        .get("content")
        .is_none());
    let preview = messages[4]["params"]["item"]["content"].as_str().unwrap();
    assert!(preview.contains("fn authoritative()"));
    assert!(preview.contains("stale=true"));
    assert!(preview.contains("untrusted data"));

    let denied = runtime.handle_line(&request(
        "5",
        "turn/start",
        json!({
            "session_id": session_id,
            "input": "read ignored",
            "idempotency_key": "turn-context-2",
            "context": [{
                "id": "context-ignored",
                "kind": "file",
                "label": "ignored.txt",
                "origin": "file-tree",
                "path": "ignored.txt"
            }]
        }),
    ));
    assert_eq!(denied[0]["error"]["code"], -32035);
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn work_turn_auto_includes_bounded_project_instructions_after_user_context() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-auto-instructions-{unique}"));
    fs::create_dir_all(&root).unwrap();
    fs::write(root.join("AGENTS.md"), "project guidance stays untrusted\n").unwrap();
    fs::write(root.join("CODEX.md"), "API_KEY=sk-12345678901234567890\n").unwrap();
    fs::write(root.join("visible.rs"), "fn visible() {}\n").unwrap();

    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request("open", "project/open", json!({ "root": root })));
    let project_id = opened[0]["result"]["project"]["id"].as_str().unwrap();
    let session = runtime.handle_line(&request(
        "session",
        "session/start",
        json!({ "mode": "work", "project_id": project_id }),
    ));
    let session_id = session[0]["result"]["session"]["id"].as_str().unwrap();
    let messages = runtime.handle_line(&request(
        "turn",
        "turn/start",
        json!({
            "session_id": session_id,
            "input": "use project guidance",
            "idempotency_key": "auto-instruction-turn",
            "context": [{
                "id": "selected-file",
                "kind": "file",
                "label": "visible.rs",
                "origin": "file-tree",
                "path": "visible.rs"
            }]
        }),
    ));
    assert_eq!(messages[0]["result"]["context"]["item_count"], 2);
    assert_eq!(
        messages[0]["result"]["context"]["budget"]["schema_version"],
        "context-budget/0.1"
    );
    assert!(
        messages[0]["result"]["context"]["budget"]["allocated_bytes"]
            .as_u64()
            .unwrap()
            <= 64 * 1024
    );
    let manifest_entries = messages[0]["result"]["context"]["manifest"]["entries"]
        .as_array()
        .unwrap();
    assert_eq!(manifest_entries.len(), 3);
    assert_eq!(manifest_entries[1]["kind"], "instruction");
    assert_eq!(manifest_entries[1]["source"], "instruction-discovery");
    assert!(manifest_entries[1]["priority"]
        .as_str()
        .unwrap()
        .starts_with("instruction-rank-"));
    assert_eq!(
        manifest_entries[1]["inclusion_reason"],
        "instruction-project-root"
    );
    assert_eq!(manifest_entries[1]["trust"], "untrusted-data");
    assert!(manifest_entries[1].get("content").is_none());
    assert_eq!(manifest_entries[2]["included"], false);
    assert_eq!(
        manifest_entries[2]["inclusion_reason"],
        "instruction-excluded:secret-shaped-content"
    );
    assert!(manifest_entries[2].get("content").is_none());
    let serialized = serde_json::to_string(&messages).unwrap();
    assert!(serialized.contains("project guidance stays untrusted"));
    assert!(!serialized.contains("API_KEY=sk-"));
    let _ = fs::remove_dir_all(root);
}

#[test]
fn context_inspector_is_read_only_and_does_not_return_instruction_content() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-context-inspect-{unique}"));
    fs::create_dir_all(&root).unwrap();
    fs::write(
        root.join("AGENTS.md"),
        "do not expose this instruction body\n",
    )
    .unwrap();
    fs::write(root.join("visible.rs"), "fn inspectable() {}\n").unwrap();
    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request("open", "project/open", json!({ "root": root })));
    let project_id = opened[0]["result"]["project"]["id"].as_str().unwrap();
    let session = runtime.handle_line(&request(
        "session",
        "session/start",
        json!({ "mode": "work", "project_id": project_id }),
    ));
    let session_id = session[0]["result"]["session"]["id"].as_str().unwrap();
    let inspected = runtime.handle_line(&request(
        "inspect",
        "turn/context/inspect",
        json!({
            "session_id": session_id,
            "context": [{
                "id": "selected-file",
                "kind": "file",
                "label": "visible.rs",
                "origin": "file-tree",
                "path": "visible.rs"
            }]
        }),
    ));
    assert_eq!(
        inspected[0]["result"]["schema_version"],
        "context-inspector/0.1"
    );
    assert_eq!(inspected[0]["result"]["content_included"], false);
    assert_eq!(inspected[0]["result"]["model_started"], false);
    assert_eq!(inspected[0]["result"]["persisted"], false);
    assert_eq!(
        inspected[0]["result"]["context"]["manifest"]["entries"]
            .as_array()
            .unwrap()
            .len(),
        2
    );
    assert_eq!(
        inspected[0]["result"]["context"]["budget"]["schema_version"],
        "context-budget/0.1"
    );
    let serialized = serde_json::to_string(&inspected).unwrap();
    assert!(!serialized.contains("do not expose this instruction body"));
    assert!(!serialized.contains("fn inspectable"));
    let _ = fs::remove_dir_all(root);
}

#[test]
fn selected_file_pins_share_inspection_and_turn_assembly_with_stale_detection() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-pinned-turn-{unique}"));
    let data_root = root.join("data");
    let project_root = root.join("project");
    fs::create_dir_all(&data_root).unwrap();
    fs::create_dir_all(&project_root).unwrap();
    let original = "fn pinned_original() {}\n";
    fs::write(project_root.join("pinned.rs"), original).unwrap();

    let mut runtime = Runtime::with_store(&data_root).unwrap();
    runtime.handle_line(&request(
        "initialize",
        "initialize",
        json!({
            "protocol_version": "0.1",
            "client": {"name": "pinned-turn", "version": "1"}
        }),
    ));
    runtime.handle_line(&request("initialized", "initialized", json!({})));
    let opened = runtime.handle_line(&request(
        "project-open",
        "project/open",
        json!({"root": project_root}),
    ));
    let project_id = opened[0]["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let session = runtime.handle_line(&request(
        "session-start",
        "session/start",
        json!({"mode": "work", "project_id": project_id}),
    ));
    let session_id = session[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let second = runtime.handle_line(&request(
        "session-second",
        "session/start",
        json!({"mode": "work", "project_id": project_id}),
    ));
    let second_session_id = second[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let original_hash = format!("sha256:{:x}", Sha256::digest(original.as_bytes()));
    let set = json!({
        "schema_version": "pinned-context/0.1",
        "project_id": project_id,
        "items": [{
            "id": "pin-file",
            "project_id": project_id,
            "session_id": session_id,
            "root_id": "root-1",
            "kind": "file",
            "source": "file-tree",
            "label": "pinned.rs",
            "reference": "pinned.rs",
            "content_hash": original_hash,
            "bytes": original.len(),
            "freshness": "fresh",
            "priority": 900,
            "metadata": {}
        }, {
            "id": "pin-selection",
            "project_id": project_id,
            "session_id": session_id,
            "root_id": "root-1",
            "kind": "selection",
            "source": "editor-selection",
            "label": "selection",
            "reference": "pinned.rs",
            "content_hash": original_hash,
            "bytes": original.len(),
            "freshness": "fresh",
            "priority": 800,
            "metadata": {
                "line": "1",
                "column": "1",
                "end_line": "1",
                "end_column": "4"
            }
        }, {
            "id": "pin-image",
            "project_id": project_id,
            "session_id": session_id,
            "kind": "image",
            "source": "image-store",
            "label": "screenshot",
            "reference": "screenshots/one.png",
            "content_hash": format!("sha256:{}", "0".repeat(64)),
            "bytes": 4,
            "freshness": "fresh",
            "priority": 700,
            "metadata": {}
        }]
    });
    let saved = runtime.handle_line(&request(
        "pins-save",
        "workspace/pinned-context/save",
        json!({"project_id": project_id, "set": set}),
    ));
    assert_eq!(saved[0]["result"]["persisted"], true);
    let set_identity = saved[0]["result"]["set_identity"]
        .as_str()
        .unwrap()
        .to_owned();

    let inspected = runtime.handle_line(&request(
        "inspect-pin",
        "turn/context/inspect",
        json!({
            "session_id": session_id,
            "pinned_context_set_identity": set_identity,
            "pinned_context_ids": ["pin-file"]
        }),
    ));
    let inspected_entry = &inspected[0]["result"]["context"]["manifest"]["entries"][0];
    assert_eq!(inspected_entry["id"], "pin-file");
    assert_eq!(inspected_entry["source"], "pinned-context");
    assert_eq!(inspected_entry["priority"], "pinned-priority-900");
    assert_eq!(inspected_entry["inclusion_reason"], "pinned-context");
    assert_eq!(inspected_entry["freshness"], "fresh");
    assert_eq!(inspected_entry["content_hash"], original_hash);
    assert_eq!(
        inspected[0]["result"]["context"]["budget"]["entries"][0]["class"],
        "pinned"
    );
    assert_eq!(
        inspected[0]["result"]["context"]["budget"]["entries"][0]["priority_score"],
        900
    );
    assert!(!serde_json::to_string(&inspected)
        .unwrap()
        .contains("pinned_original"));

    let wrong_identity = runtime.handle_line(&request(
        "inspect-stale-set",
        "turn/context/inspect",
        json!({
            "session_id": session_id,
            "pinned_context_set_identity": format!("pinned-context:sha256:{}", "f".repeat(64)),
            "pinned_context_ids": ["pin-file"]
        }),
    ));
    assert_eq!(wrong_identity[0]["error"]["code"], -32041);
    let duplicate = runtime.handle_line(&request(
        "inspect-duplicate",
        "turn/context/inspect",
        json!({
            "session_id": session_id,
            "pinned_context_set_identity": set_identity,
            "pinned_context_ids": ["pin-file", "pin-file"]
        }),
    ));
    assert_eq!(duplicate[0]["error"]["code"], -32602);
    let missing = runtime.handle_line(&request(
        "inspect-missing",
        "turn/context/inspect",
        json!({
            "session_id": session_id,
            "pinned_context_set_identity": set_identity,
            "pinned_context_ids": ["pin-missing"]
        }),
    ));
    assert_eq!(missing[0]["error"]["code"], -32046);
    let selection = runtime.handle_line(&request(
        "inspect-selection-range",
        "turn/context/inspect",
        json!({
            "session_id": session_id,
            "pinned_context_set_identity": set_identity,
            "pinned_context_ids": ["pin-selection"]
        }),
    ));
    assert_eq!(selection[0]["result"]["context"]["item_count"], 1);
    assert_eq!(
        selection[0]["result"]["context"]["manifest"]["entries"][0]["kind"],
        "selection"
    );
    assert_eq!(
        selection[0]["result"]["context"]["manifest"]["entries"][0]["freshness"],
        "fresh"
    );
    let unsupported = runtime.handle_line(&request(
        "inspect-selection",
        "turn/context/inspect",
        json!({
            "session_id": session_id,
            "pinned_context_set_identity": set_identity,
            "pinned_context_ids": ["pin-image"]
        }),
    ));
    assert_eq!(unsupported[0]["error"]["code"], -32048);
    let cross_session = runtime.handle_line(&request(
        "inspect-cross-session",
        "turn/context/inspect",
        json!({
            "session_id": second_session_id,
            "pinned_context_set_identity": set_identity,
            "pinned_context_ids": ["pin-file"]
        }),
    ));
    assert_eq!(cross_session[0]["error"]["code"], -32095);

    let changed = "fn pinned_changed() {}\n";
    fs::write(project_root.join("pinned.rs"), changed).unwrap();
    let turn = runtime.handle_line(&request(
        "turn-pin",
        "turn/start",
        json!({
            "session_id": session_id,
            "input": "inspect the selected pin",
            "idempotency_key": "pinned-turn-1",
            "pinned_context_set_identity": set_identity,
            "pinned_context_ids": ["pin-file"]
        }),
    ));
    assert_eq!(turn[0]["result"]["context"]["item_count"], 1);
    assert_eq!(
        turn[0]["result"]["context"]["manifest"]["entries"][0]["freshness"],
        "stale"
    );
    let serialized = serde_json::to_string(&turn).unwrap();
    assert!(serialized.contains("pinned_changed"));
    assert!(!serialized.contains("pinned_original"));
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn session_runtime_state_isolation_keeps_environment_context_and_history_scoped() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-session-isolation-{unique}"));
    fs::create_dir_all(&root).unwrap();
    fs::write(root.join("visible.rs"), "fn isolated_context() {}\n").unwrap();

    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request("2", "project/open", json!({ "root": root })));
    let project_id = opened[0]["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let first = runtime.handle_line(&request(
        "3",
        "session/start",
        json!({ "mode": "work", "project_id": project_id, "title": "First" }),
    ));
    let first_id = first[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let second = runtime.handle_line(&request(
        "4",
        "session/start",
        json!({ "mode": "work", "project_id": project_id, "title": "Second" }),
    ));
    let second_id = second[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();

    assert_ne!(
        first[0]["result"]["environment"]["environment_id"],
        second[0]["result"]["environment"]["environment_id"]
    );
    assert_eq!(
        first[0]["result"]["runtime"]["permission_profile"],
        "read-only"
    );
    assert_eq!(
        second[0]["result"]["runtime"]["permission_profile"],
        "read-only"
    );

    let first_turn = runtime.handle_line(&request(
        "5",
        "turn/start",
        json!({
            "session_id": first_id,
            "input": "first session context",
            "idempotency_key": "isolation-first",
            "context": [{
                "id": "first-file",
                "kind": "file",
                "label": "visible.rs",
                "origin": "file-tree",
                "path": "visible.rs"
            }]
        }),
    ));
    assert_eq!(first_turn[0]["result"]["context"]["item_count"], 1);
    assert!(first_turn
        .iter()
        .any(|message| message["params"]["item"]["content"]
            .as_str()
            .is_some_and(|content| content.contains("isolated_context"))));

    let second_history = runtime.handle_line(&request(
        "6",
        "session/read",
        json!({ "session_id": second_id, "limit": 10 }),
    ));
    assert_eq!(second_history[0]["result"]["items"], json!([]));
    let second_turn = runtime.handle_line(&request(
        "7",
        "turn/start",
        json!({
            "session_id": second_id,
            "input": "second session only",
            "idempotency_key": "isolation-second"
        }),
    ));
    assert_eq!(second_turn[0]["result"]["context"]["item_count"], 0);
    let first_history = runtime.handle_line(&request(
        "8",
        "session/read",
        json!({ "session_id": first_id, "limit": 10 }),
    ));
    assert!(first_history[0]["result"]["items"]
        .as_array()
        .unwrap()
        .iter()
        .any(|item| item["content"] == "first session context"));
    assert!(!first_history[0]["result"]["items"]
        .as_array()
        .unwrap()
        .iter()
        .any(|item| item["content"] == "second session only"));
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn work_session_requires_project() {
    let mut runtime = ready_runtime();
    let messages = runtime.handle_line(&request("2", "session/start", json!({ "mode": "work" })));
    assert_eq!(messages[0]["error"]["code"], -32021);
}

#[test]
fn workspace_edit_preview_is_read_only_root_bound_and_session_paged() {
    fn digest(bytes: &[u8]) -> String {
        format!("{:x}", Sha256::digest(bytes))
    }

    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-edit-preview-{unique}"));
    fs::create_dir_all(root.join("src")).unwrap();
    fs::write(root.join("src/main.rs"), "fn before() {}\n").unwrap();
    fs::write(root.join(".env"), "OLD_SECRET=must-not-appear\n").unwrap();
    let root = root.canonicalize().unwrap();
    let root_string = root.to_string_lossy().into_owned();
    let root_identity = format!("workspace-root:sha256:{}", digest(root_string.as_bytes()));
    let replacement = "fn after() {}\n";
    let replacement_digest = digest(replacement.as_bytes());
    let replacement_reference = format!("workspace-edit-content:sha256:{replacement_digest}");

    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request(
        "edit-project",
        "project/open",
        json!({ "root": root }),
    ));
    let project_id = opened[0]["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let work = runtime.handle_line(&request(
        "edit-session",
        "session/start",
        json!({ "mode": "work", "project_id": project_id }),
    ));
    let session_id = work[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let other = runtime.handle_line(&request(
        "edit-other-session",
        "session/start",
        json!({ "mode": "work", "project_id": project_id }),
    ));
    let other_session_id = other[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let edit = json!({
        "schema_version": "workspace-edit/0.2",
        "edit_id": "edit-protocol",
        "project_id": project_id,
        "root": {
            "canonical_path": root_string,
            "identity": root_identity
        },
        "operations": [{
            "kind": "update",
            "path": "src/main.rs",
            "base": {
                "sha256": digest(b"fn before() {}\n"),
                "bytes": 15
            },
            "content": {
                "reference": replacement_reference,
                "hash": {
                    "sha256": replacement_digest,
                    "bytes": replacement.len()
                },
                "format": {
                    "encoding": "utf-8",
                    "newline": "lf",
                    "mode": "preserve"
                }
            }
        }]
    });
    let preview = runtime.handle_line(&request(
        "edit-preview",
        "workspace/edit/preview",
        json!({
            "session_id": session_id,
            "edit": edit,
            "contents": [{
                "reference": replacement_reference,
                "content": replacement
            }]
        }),
    ));
    assert_eq!(preview[0]["result"]["applicable"], true);
    assert_eq!(preview[0]["result"]["files"][0]["base_matches"], true);
    assert!(preview[0]["result"]["additions"].as_u64().unwrap() > 0);
    assert!(preview[0]["result"]["deletions"].as_u64().unwrap() > 0);
    let aggregate_reference = preview[0]["result"]["aggregate_diff"]["reference"]
        .as_str()
        .unwrap()
        .to_owned();
    let first_page = runtime.handle_line(&request(
        "edit-page",
        "workspace/edit/artifact/read",
        json!({
            "session_id": session_id,
            "project_id": project_id,
            "edit_id": "edit-protocol",
            "reference": aggregate_reference,
            "offset": 0,
            "limit": 32
        }),
    ));
    let page = BASE64_STANDARD
        .decode(first_page[0]["result"]["data_base64"].as_str().unwrap())
        .unwrap();
    assert!(String::from_utf8_lossy(&page).contains("--- a/src/main.rs"));
    let denied = runtime.handle_line(&request(
        "edit-page-denied",
        "workspace/edit/artifact/read",
        json!({
            "session_id": other_session_id,
            "project_id": project_id,
            "edit_id": "edit-protocol",
            "reference": aggregate_reference
        }),
    ));
    assert_eq!(denied[0]["error"]["code"], -32085);

    let mut wrong_root = edit.clone();
    wrong_root["root"]["canonical_path"] = Value::String("/tmp".into());
    let root_denied = runtime.handle_line(&request(
        "edit-root-denied",
        "workspace/edit/preview",
        json!({
            "session_id": session_id,
            "edit": wrong_root,
            "contents": [{
                "reference": replacement_reference,
                "content": replacement
            }]
        }),
    ));
    assert_eq!(root_denied[0]["error"]["code"], -32083);
    let apply_denied = runtime.handle_line(&request(
        "edit-apply-denied",
        "workspace/edit/apply",
        json!({ "session_id": session_id, "edit_id": "edit-protocol" }),
    ));
    assert_eq!(apply_denied[0]["error"]["code"], -32601);
    assert_eq!(
        fs::read_to_string(root.join("src/main.rs")).unwrap(),
        "fn before() {}\n"
    );
    assert_eq!(
        fs::read_to_string(root.join(".env")).unwrap(),
        "OLD_SECRET=must-not-appear\n"
    );
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn terminal_creation_remains_user_initiated_and_work_session_scoped() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-terminal-scope-{unique}"));
    fs::create_dir_all(&root).unwrap();
    let mut runtime = ready_runtime();
    let chat = runtime.handle_line(&request("2", "session/start", json!({ "mode": "chat" })));
    let chat_id = chat[0]["result"]["session"]["id"].as_str().unwrap();
    let denied = runtime.handle_line(&request(
        "3",
        "terminal/open-user",
        json!({ "session_id": chat_id }),
    ));
    assert_eq!(denied[0]["error"]["code"], -32095);
    let agent_method = runtime.handle_line(&request(
        "4",
        "terminal/open",
        json!({ "session_id": chat_id }),
    ));
    assert_eq!(agent_method[0]["error"]["code"], -32601);

    let opened = runtime.handle_line(&request("5", "project/open", json!({ "root": root })));
    let project_id = opened[0]["result"]["project"]["id"].as_str().unwrap();
    let work = runtime.handle_line(&request(
        "6",
        "session/start",
        json!({ "mode": "work", "project_id": project_id }),
    ));
    let work_id = work[0]["result"]["session"]["id"].as_str().unwrap();
    let invalid = runtime.handle_line(&request(
        "7",
        "terminal/open-user",
        json!({ "session_id": work_id, "rows": 0, "cols": 80 }),
    ));
    #[cfg(any(target_os = "macos", target_os = "windows"))]
    assert_eq!(invalid[0]["error"]["code"], -32602);
    #[cfg(not(any(target_os = "macos", target_os = "windows")))]
    assert_eq!(invalid[0]["error"]["code"], -32090);
    fs::remove_dir_all(root).unwrap();
}

#[cfg(any(target_os = "macos", target_os = "windows"))]
#[test]
fn platform_terminal_protocol_supports_interaction_resize_and_exit_status() {
    use std::thread;
    use std::time::{Duration, Instant};

    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-terminal-{unique}"));
    fs::create_dir_all(&root).unwrap();
    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request("2", "project/open", json!({ "root": root })));
    let project_id = opened[0]["result"]["project"]["id"].as_str().unwrap();
    let first = runtime.handle_line(&request(
        "3",
        "session/start",
        json!({ "mode": "work", "project_id": project_id }),
    ));
    let first_id = first[0]["result"]["session"]["id"].as_str().unwrap();
    let first_environment = first[0]["result"]["environment"].clone();
    assert!(first_environment["environment_id"]
        .as_str()
        .unwrap()
        .starts_with("environment:sha256:"));
    assert!(first_environment.get("variables").is_none());
    let second = runtime.handle_line(&request(
        "4",
        "session/start",
        json!({ "mode": "work", "project_id": project_id }),
    ));
    let second_id = second[0]["result"]["session"]["id"].as_str().unwrap();
    assert_ne!(
        first_environment["environment_id"],
        second[0]["result"]["environment"]["environment_id"]
    );
    let terminal = runtime.handle_line(&request(
        "5",
        "terminal/open-user",
        json!({ "session_id": first_id, "rows": 24, "cols": 80 }),
    ));
    let terminal_id = terminal[0]["result"]["terminal_id"].as_str().unwrap();
    assert_eq!(terminal[0]["result"]["project_id"], project_id);
    assert_eq!(terminal[0]["result"]["capture_limit"], 1024 * 1024);
    assert_eq!(terminal[0]["result"]["encoding"], "utf-8");
    assert_ne!(
        terminal[0]["result"]["environment"]["environment_id"],
        first_environment["environment_id"]
    );
    assert_eq!(
        terminal[0]["result"]["environment"]["masked_count"],
        first_environment["masked_count"]
    );
    assert!(
        terminal[0]["result"]["environment"]["explicit_variable_names"]
            .as_array()
            .unwrap()
            .iter()
            .any(|name| name == "TERM")
    );
    assert!(terminal[0]["result"]["environment"]
        .get("variable_values")
        .is_none());
    #[cfg(target_os = "macos")]
    assert_eq!(
        terminal[0]["result"]["process_tree_policy"],
        "unix-session-and-foreground-process-group"
    );
    #[cfg(target_os = "windows")]
    assert_eq!(
        terminal[0]["result"]["process_tree_policy"],
        "windows-job-object-kill-on-close"
    );

    let cross_session = runtime.handle_line(&request(
        "6",
        "terminal/read",
        json!({ "session_id": second_id, "terminal_id": terminal_id }),
    ));
    assert_eq!(cross_session[0]["error"]["code"], -32093);
    let resized = runtime.handle_line(&request(
        "7",
        "terminal/resize",
        json!({
            "session_id": first_id,
            "terminal_id": terminal_id,
            "rows": 42,
            "cols": 132
        }),
    ));
    assert_eq!(resized[0]["result"]["rows"], 42);
    #[cfg(target_os = "macos")]
    let input = BASE64_STANDARD.encode("printf '终端协议正常\\n'; exit 9\n");
    #[cfg(target_os = "windows")]
    let input = BASE64_STANDARD.encode("echo 终端协议正常\r\nexit 9\r\n");
    let written = runtime.handle_line(&request(
        "8",
        "terminal/input-user",
        json!({
            "session_id": first_id,
            "terminal_id": terminal_id,
            "data_base64": input
        }),
    ));
    assert!(written[0]["result"]["written"].as_u64().unwrap() > 0);

    let deadline = Instant::now() + Duration::from_secs(5);
    let mut request_id = 9_u32;
    let final_snapshot = loop {
        let messages = runtime.handle_line(&request(
            &request_id.to_string(),
            "terminal/read",
            json!({ "session_id": first_id, "terminal_id": terminal_id, "after": 0 }),
        ));
        request_id += 1;
        if !messages[0]["result"]["running"].as_bool().unwrap() {
            break messages[0]["result"].clone();
        }
        assert!(Instant::now() < deadline, "terminal did not exit");
        thread::sleep(Duration::from_millis(10));
    };
    assert_eq!(final_snapshot["exit_code"], 9);
    assert_eq!(final_snapshot["rows"], 42);
    assert_eq!(final_snapshot["cols"], 132);
    let output = BASE64_STANDARD
        .decode(final_snapshot["output_base64"].as_str().unwrap())
        .unwrap();
    assert!(String::from_utf8_lossy(&output).contains("终端协议正常"));
    let reread = runtime.handle_line(&request(
        "session-environment-read",
        "session/read",
        json!({ "session_id": first_id }),
    ));
    assert_eq!(
        reread[0]["result"]["environment"]["environment_id"],
        first_environment["environment_id"]
    );
    fs::remove_dir_all(root).unwrap();
}

#[cfg(any(target_os = "macos", target_os = "windows"))]
#[test]
fn terminal_lifecycle_supports_named_list_attach_stop_restart_and_remove() {
    use std::thread;
    use std::time::{Duration, Instant};

    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-terminal-lifecycle-{unique}"));
    fs::create_dir_all(&root).unwrap();
    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request(
        "lifecycle-project",
        "project/open",
        json!({ "root": root }),
    ));
    let project_id = opened[0]["result"]["project"]["id"].as_str().unwrap();
    let first = runtime.handle_line(&request(
        "lifecycle-session-1",
        "session/start",
        json!({ "mode": "work", "project_id": project_id }),
    ));
    let first_id = first[0]["result"]["session"]["id"].as_str().unwrap();
    let second = runtime.handle_line(&request(
        "lifecycle-session-2",
        "session/start",
        json!({ "mode": "work", "project_id": project_id }),
    ));
    let second_id = second[0]["result"]["session"]["id"].as_str().unwrap();

    let foreground = runtime.handle_line(&request(
        "lifecycle-open-foreground",
        "terminal/open-user",
        json!({
            "session_id": first_id,
            "kind": "foreground",
            "name": "Main",
            "rows": 24,
            "cols": 80
        }),
    ));
    let foreground_id = foreground[0]["result"]["terminal_id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(foreground[0]["result"]["kind"], "foreground");
    assert_eq!(foreground[0]["result"]["name"], "Main");
    assert_eq!(foreground[0]["result"]["generation"], 1);
    assert_eq!(foreground[0]["result"]["state"], "running");
    assert_eq!(foreground[0]["result"]["input_policy"], "user-only");

    let duplicate_foreground = runtime.handle_line(&request(
        "lifecycle-open-foreground-duplicate",
        "terminal/open-user",
        json!({ "session_id": first_id }),
    ));
    assert_eq!(duplicate_foreground[0]["error"]["code"], -32098);
    let unnamed_background = runtime.handle_line(&request(
        "lifecycle-open-background-unnamed",
        "terminal/open-user",
        json!({ "session_id": first_id, "kind": "background" }),
    ));
    assert_eq!(unnamed_background[0]["error"]["code"], -32602);

    let background = runtime.handle_line(&request(
        "lifecycle-open-background",
        "terminal/open-user",
        json!({
            "session_id": first_id,
            "kind": "background",
            "name": "Dev Server"
        }),
    ));
    let background_id = background[0]["result"]["terminal_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let duplicate_background = runtime.handle_line(&request(
        "lifecycle-open-background-duplicate",
        "terminal/open-user",
        json!({
            "session_id": first_id,
            "kind": "background",
            "name": "dev server"
        }),
    ));
    assert_eq!(duplicate_background[0]["error"]["code"], -32099);
    let archive_running = runtime.handle_line(&request(
        "lifecycle-archive-running",
        "session/archive",
        json!({ "session_id": first_id }),
    ));
    assert_eq!(archive_running[0]["error"]["code"], -32025);

    let invalid_restart = runtime.handle_line(&request(
        "lifecycle-restart-invalid",
        "terminal/restart-user",
        json!({
            "session_id": first_id,
            "terminal_id": background_id,
            "rows": 0,
            "cols": 80
        }),
    ));
    assert_eq!(invalid_restart[0]["error"]["code"], -32602);
    let attached_after_invalid_restart = runtime.handle_line(&request(
        "lifecycle-attach-after-invalid-restart",
        "terminal/attach",
        json!({ "session_id": first_id, "terminal_id": background_id }),
    ));
    assert_eq!(attached_after_invalid_restart[0]["result"]["generation"], 1);
    assert_eq!(attached_after_invalid_restart[0]["result"]["running"], true);

    let listed = runtime.handle_line(&request(
        "lifecycle-list",
        "terminal/list",
        json!({ "session_id": first_id }),
    ));
    assert_eq!(
        listed[0]["result"]["terminals"].as_array().unwrap().len(),
        2
    );
    assert!(listed[0]["result"]["terminals"]
        .as_array()
        .unwrap()
        .iter()
        .all(|terminal| terminal["output_base64"] == ""));
    let other_list = runtime.handle_line(&request(
        "lifecycle-list-other",
        "terminal/list",
        json!({ "session_id": second_id }),
    ));
    assert!(other_list[0]["result"]["terminals"]
        .as_array()
        .unwrap()
        .is_empty());
    let denied_attach = runtime.handle_line(&request(
        "lifecycle-attach-denied",
        "terminal/attach",
        json!({ "session_id": second_id, "terminal_id": foreground_id }),
    ));
    assert_eq!(denied_attach[0]["error"]["code"], -32093);

    let restarted = runtime.handle_line(&request(
        "lifecycle-restart",
        "terminal/restart-user",
        json!({
            "session_id": first_id,
            "terminal_id": background_id,
            "rows": 30,
            "cols": 100
        }),
    ));
    assert_eq!(restarted[0]["result"]["terminal_id"], background_id);
    assert_eq!(restarted[0]["result"]["generation"], 2);
    assert_eq!(restarted[0]["result"]["name"], "Dev Server");
    assert_eq!(restarted[0]["result"]["rows"], 30);
    assert_eq!(restarted[0]["result"]["cols"], 100);
    let remove_running = runtime.handle_line(&request(
        "lifecycle-remove-running",
        "terminal/remove-user",
        json!({ "session_id": first_id, "terminal_id": background_id }),
    ));
    assert_eq!(remove_running[0]["error"]["code"], -32097);

    {
        let mut stop_and_remove = |terminal_id: &str, label: &str| {
            let stopped = runtime.handle_line(&request(
                &format!("lifecycle-stop-{label}"),
                "terminal/stop-user",
                json!({ "session_id": first_id, "terminal_id": terminal_id }),
            ));
            assert!(
                stopped[0].get("result").is_some(),
                "terminal stop failed: {}",
                stopped[0]
            );
            let deadline = Instant::now() + Duration::from_secs(5);
            let mut poll = 0_u32;
            loop {
                let attached = runtime.handle_line(&request(
                    &format!("lifecycle-poll-{label}-{poll}"),
                    "terminal/attach",
                    json!({ "session_id": first_id, "terminal_id": terminal_id, "after": 0 }),
                ));
                if !attached[0]["result"]["running"].as_bool().unwrap() {
                    assert_eq!(attached[0]["result"]["state"], "exited");
                    break;
                }
                poll += 1;
                assert!(Instant::now() < deadline, "terminal did not stop");
                thread::sleep(Duration::from_millis(10));
            }
            let removed = runtime.handle_line(&request(
                &format!("lifecycle-remove-{label}"),
                "terminal/remove-user",
                json!({ "session_id": first_id, "terminal_id": terminal_id }),
            ));
            assert_eq!(removed[0]["result"]["removed"], true);
        };
        stop_and_remove(&background_id, "background");
        stop_and_remove(&foreground_id, "foreground");
    }

    let reopened = runtime.handle_line(&request(
        "lifecycle-reopen-foreground",
        "terminal/open-user",
        json!({ "session_id": first_id, "name": "Fresh Main" }),
    ));
    assert_eq!(reopened[0]["result"]["kind"], "foreground");
    assert_eq!(reopened[0]["result"]["generation"], 1);
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn workspace_methods_are_project_scoped() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-workspace-{unique}"));
    fs::create_dir_all(root.join("src")).unwrap();
    fs::write(root.join("src/main.rs"), "fn main() {}\n").unwrap();

    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request("2", "project/open", json!({ "root": root })));
    let project_id = opened[0]["result"]["project"]["id"].as_str().unwrap();
    let listing = runtime.handle_line(&request(
        "3",
        "workspace/list",
        json!({ "project_id": project_id, "path": "src" }),
    ));
    assert_eq!(listing[0]["result"]["entries"][0]["path"], "src/main.rs");
    let file = runtime.handle_line(&request(
        "4",
        "workspace/read",
        json!({ "project_id": project_id, "path": "src/main.rs" }),
    ));
    assert_eq!(file[0]["result"]["content"], "fn main() {}\n");
    assert_eq!(file[0]["result"]["newline"], "lf");
    assert_eq!(file[0]["result"]["save_supported"], true);
    let metadata = runtime.handle_line(&request(
        "5",
        "workspace/metadata",
        json!({ "project_id": project_id, "path": "src/main.rs" }),
    ));
    assert_eq!(metadata[0]["result"]["kind"], "file");
    assert_eq!(metadata[0]["result"]["path"], "src/main.rs");

    let self_watch = runtime.handle_line(&request(
        "self-save-watch",
        "workspace/watch",
        json!({ "project_id": project_id, "paths": ["src"] }),
    ));
    let self_watch_id = self_watch[0]["result"]["watch_id"]
        .as_str()
        .unwrap()
        .to_owned();

    let saved = runtime.handle_line(&request(
        "6",
        "workspace/save-user-text",
        json!({
            "project_id": project_id,
            "path": "src/main.rs",
            "content": "fn main() { println!(\"saved\"); }\n",
            "expected_revision": file[0]["result"]["revision"],
            "encoding": file[0]["result"]["encoding"],
            "newline": file[0]["result"]["newline"],
            "origin": "user"
        }),
    ));
    assert!(saved[0]["result"]["revision"]
        .as_str()
        .unwrap()
        .starts_with("content:"));
    assert_eq!(
        fs::read_to_string(root.join("src/main.rs")).unwrap(),
        "fn main() { println!(\"saved\"); }\n"
    );
    let self_changes = runtime.handle_line(&request(
        "self-save-poll",
        "workspace/watch/poll",
        json!({ "watch_id": self_watch_id }),
    ));
    assert!(self_changes[0]["result"]["changes"]
        .as_array()
        .unwrap()
        .is_empty());
    let stale = runtime.handle_line(&request(
        "7",
        "workspace/save-user-text",
        json!({
            "project_id": project_id,
            "path": "src/main.rs",
            "content": "stale\n",
            "expected_revision": file[0]["result"]["revision"],
            "encoding": "utf-8",
            "newline": "lf",
            "origin": "user"
        }),
    ));
    assert_eq!(stale[0]["error"]["code"], -32042);

    let watched = runtime.handle_line(&request(
        "8",
        "workspace/watch",
        json!({ "project_id": project_id, "paths": ["src"] }),
    ));
    let watch_id = watched[0]["result"]["watch_id"].as_str().unwrap();
    fs::write(
        root.join("src/main.rs"),
        "fn main() { println!(\"changed\"); }\n",
    )
    .unwrap();
    fs::write(root.join("src/new.rs"), "pub fn added() {}\n").unwrap();
    let changes = runtime.handle_line(&request(
        "9",
        "workspace/watch/poll",
        json!({ "watch_id": watch_id }),
    ));
    let changes = changes[0]["result"]["changes"].as_array().unwrap();
    assert!(changes
        .iter()
        .any(|change| { change["path"] == "src/main.rs" && change["kind"] == "modified" }));
    assert!(changes
        .iter()
        .any(|change| { change["path"] == "src/new.rs" && change["kind"] == "created" }));
    fs::remove_file(root.join("src/new.rs")).unwrap();
    let changes = runtime.handle_line(&request(
        "10",
        "workspace/watch/poll",
        json!({ "watch_id": watch_id }),
    ));
    assert!(changes[0]["result"]["changes"]
        .as_array()
        .unwrap()
        .iter()
        .any(|change| change["path"] == "src/new.rs" && change["kind"] == "deleted"));
    let escaped = runtime.handle_line(&request(
        "11",
        "workspace/read",
        json!({ "project_id": project_id, "path": "../outside" }),
    ));
    assert_eq!(escaped[0]["error"]["code"], -32030);
    let search = runtime.handle_line(&request(
        "12",
        "workspace/search",
        json!({
            "project_id": project_id,
            "search_id": "protocol-search-1",
            "query": "println",
            "mode": "text",
            "case_sensitive": false,
            "limit": 10
        }),
    ));
    assert_eq!(search[0]["result"]["search_id"], "protocol-search-1");
    assert_eq!(search[0]["result"]["matches"][0]["path"], "src/main.rs");
    assert_eq!(search[0]["result"]["matches"][0]["match_type"], "text");
    assert!(search[0]["result"]["snapshot"]
        .as_str()
        .unwrap()
        .starts_with("scan:"));
    let cancelled = runtime.handle_line(&request(
        "13",
        "workspace/search/cancel",
        json!({ "search_id": "protocol-search-cancelled" }),
    ));
    assert_eq!(cancelled[0]["result"]["cancelled"], true);
    let cancelled_search = runtime.handle_line(&request(
        "14",
        "workspace/search",
        json!({
            "project_id": project_id,
            "search_id": "protocol-search-cancelled",
            "query": "main",
            "mode": "all"
        }),
    ));
    assert_eq!(cancelled_search[0]["result"]["cancelled"], true);
    assert_eq!(cancelled_search[0]["result"]["matches"], json!([]));
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn workspace_tree_honors_gitignore_and_reports_git_decorations() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-git-tree-{unique}"));
    fs::create_dir_all(&root).unwrap();
    let initialized = Command::new("git").arg("init").arg(&root).output();
    if !initialized.is_ok_and(|output| output.status.success()) {
        let _ = fs::remove_dir_all(root);
        return;
    }
    fs::write(root.join(".gitignore"), "ignored.log\n").unwrap();
    fs::write(root.join("ignored.log"), "hidden\n").unwrap();
    fs::write(root.join("visible.txt"), "visible\n").unwrap();

    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request("2", "project/open", json!({ "root": root })));
    let project_id = opened[0]["result"]["project"]["id"].as_str().unwrap();
    let listing = runtime.handle_line(&request(
        "3",
        "workspace/list",
        json!({ "project_id": project_id, "path": "" }),
    ));
    let entries = listing[0]["result"]["entries"].as_array().unwrap();
    assert!(entries.iter().any(|entry| entry["path"] == "visible.txt"));
    assert!(!entries.iter().any(|entry| entry["path"] == "ignored.log"));

    let status = runtime.handle_line(&request(
        "4",
        "workspace/git-status",
        json!({ "project_id": project_id }),
    ));
    assert_eq!(status[0]["result"]["schema_version"], "git-status/0.2");
    assert_eq!(status[0]["result"]["repository"], true);
    assert_eq!(status[0]["result"]["worktree"], true);
    assert_eq!(
        status[0]["result"]["repository_root"],
        root.canonicalize().unwrap().to_string_lossy().as_ref()
    );
    assert_eq!(status[0]["result"]["unborn"], true);
    assert!(status[0]["result"]["branch"].is_string());
    assert!(status[0]["result"]["untracked_paths"]
        .as_array()
        .unwrap()
        .iter()
        .any(|path| path == "visible.txt"));
    assert!(status[0]["result"]["entries"]
        .as_array()
        .unwrap()
        .iter()
        .any(|entry| entry["path"] == "visible.txt"
            && entry["status"] == "??"
            && entry["kind"] == "untracked"
            && entry["unstaged"] == true));

    let ignored = runtime.handle_line(&request(
        "5",
        "workspace/read",
        json!({ "project_id": project_id, "path": "ignored.log" }),
    ));
    assert_eq!(ignored[0]["error"]["code"], -32035);
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn git_queries_are_read_only_project_scoped_and_content_filtered() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-git-query-{unique}"));
    let project = root.join("project");
    fs::create_dir_all(&project).unwrap();
    let git = |args: &[&str]| {
        Command::new("git")
            .arg("-C")
            .arg(&root)
            .args(args)
            .output()
            .unwrap()
    };
    if !git(&["init", "-q"]).status.success() {
        let _ = fs::remove_dir_all(root);
        return;
    }
    assert!(git(&["config", "user.name", "Aegisy Test"])
        .status
        .success());
    assert!(git(&["config", "user.email", "test@aegisy.local"])
        .status
        .success());
    fs::write(project.join("visible.txt"), "before\n").unwrap();
    fs::write(project.join(".env"), "TOKEN=before\n").unwrap();
    fs::write(root.join("outside.txt"), "outside-before\n").unwrap();
    assert!(git(&["add", "."]).status.success());
    assert!(git(&["commit", "-q", "-m", "initial"]).status.success());
    fs::write(project.join("visible.txt"), "after\n").unwrap();
    fs::write(project.join(".env"), "TOKEN=hidden-after\n").unwrap();
    fs::write(root.join("outside.txt"), "outside-after\n").unwrap();
    assert!(git(&["add", "."]).status.success());
    assert!(git(&["commit", "-q", "-m", "second"]).status.success());
    let oid = String::from_utf8(git(&["rev-parse", "HEAD"]).stdout)
        .unwrap()
        .trim()
        .to_owned();
    fs::write(project.join("visible.txt"), "worktree\n").unwrap();
    fs::write(project.join(".env"), "TOKEN=hidden-worktree\n").unwrap();

    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request("2", "project/open", json!({ "root": project })));
    let project_id = opened[0]["result"]["project"]["id"].as_str().unwrap();
    let overview = runtime.handle_line(&request(
        "3",
        "workspace/git/overview",
        json!({ "project_id": project_id }),
    ));
    assert_eq!(overview[0]["result"]["schema_version"], "git-query/0.1");
    assert!(overview[0]["result"]["branches"].is_array());
    assert!(overview[0]["result"]["worktrees"].is_array());

    let log = runtime.handle_line(&request(
        "4",
        "workspace/git/log",
        json!({ "project_id": project_id, "limit": 1 }),
    ));
    assert_eq!(log[0]["result"]["commits"][0]["oid"], oid);
    let commit = runtime.handle_line(&request(
        "5",
        "workspace/git/commit",
        json!({ "project_id": project_id, "oid": oid }),
    ));
    let encoded = serde_json::to_string(&commit[0]["result"]).unwrap();
    assert!(encoded.contains("visible.txt"));
    assert!(!encoded.contains(".env"));
    assert!(!encoded.contains("outside.txt"));

    let diff = runtime.handle_line(&request(
        "6",
        "workspace/git/diff",
        json!({ "project_id": project_id, "scope": "worktree" }),
    ));
    assert_eq!(diff[0]["result"]["paths"], json!(["visible.txt"]));
    assert!(diff[0]["result"]["patch"]
        .as_str()
        .unwrap()
        .contains("worktree"));
    let diff_json = serde_json::to_string(&diff[0]["result"]).unwrap();
    assert!(!diff_json.contains("hidden-worktree"));

    let invalid = runtime.handle_line(&request(
        "7",
        "workspace/git/commit",
        json!({ "project_id": project_id, "oid": "HEAD" }),
    ));
    assert_eq!(invalid[0]["error"]["code"], -32042);
    for (index, method) in [
        "workspace/git/stage",
        "workspace/git/branch/create",
        "workspace/git/commit/create",
        "workspace/git/push",
    ]
    .into_iter()
    .enumerate()
    {
        let request_id = format!("git-write-denied-{index}");
        let denied = runtime.handle_line(&request(
            &request_id,
            method,
            json!({ "project_id": project_id }),
        ));
        assert_eq!(denied[0]["error"]["code"], -32601);
    }
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn workspace_index_is_incremental_and_repository_map_is_budgeted() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-index-{unique}"));
    fs::create_dir_all(root.join("src")).unwrap();
    let initialized = Command::new("git").arg("init").arg(&root).output();
    if !initialized.is_ok_and(|output| output.status.success()) {
        let _ = fs::remove_dir_all(root);
        return;
    }
    fs::write(root.join(".gitignore"), "src/ignored.rs\n").unwrap();
    fs::write(
        root.join("src/main.rs"),
        "use crate::tools;\npub fn main() {}\n",
    )
    .unwrap();
    fs::write(
        root.join("src/tool.py"),
        "from app.core import Base\nclass Tool(Base):\n    pass\n",
    )
    .unwrap();
    fs::write(root.join("src/ignored.rs"), "pub fn secret() {}\n").unwrap();

    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request("2", "project/open", json!({ "root": root })));
    let project_id = opened[0]["result"]["project"]["id"].as_str().unwrap();
    let first = runtime.handle_line(&request(
        "3",
        "workspace/index",
        json!({ "project_id": project_id }),
    ));
    assert_eq!(first[0]["result"]["project_id"], project_id);
    assert_eq!(first[0]["result"]["indexed_files"], 2);
    assert_eq!(first[0]["result"]["parsed_files"], 2);
    assert!(first[0]["result"]["symbols"]
        .as_array()
        .unwrap()
        .iter()
        .any(|symbol| symbol["name"] == "main" && symbol["provenance"] == "tree-sitter"));
    assert!(!first[0]["result"]["symbols"]
        .as_array()
        .unwrap()
        .iter()
        .any(|symbol| symbol["name"] == "secret"));
    assert!(first[0]["result"]["dependencies"]
        .as_array()
        .unwrap()
        .iter()
        .any(|edge| edge["target"] == "app.core"));

    let second = runtime.handle_line(&request(
        "4",
        "workspace/index",
        json!({ "project_id": project_id }),
    ));
    assert_eq!(second[0]["result"]["parsed_files"], 0);
    assert_eq!(second[0]["result"]["reused_files"], 2);

    let opened_file = runtime.handle_line(&request(
        "5",
        "workspace/read",
        json!({ "project_id": project_id, "path": "src/main.rs" }),
    ));
    let saved = runtime.handle_line(&request(
        "6",
        "workspace/save-user-text",
        json!({
            "project_id": project_id,
            "path": "src/main.rs",
            "content": "use crate::tools;\npub fn changed() {}\n",
            "expected_revision": opened_file[0]["result"]["revision"],
            "encoding": opened_file[0]["result"]["encoding"],
            "newline": opened_file[0]["result"]["newline"],
            "origin": "user"
        }),
    ));
    assert!(saved[0].get("result").is_some());
    let changed = runtime.handle_line(&request(
        "7",
        "workspace/index",
        json!({ "project_id": project_id }),
    ));
    assert_eq!(changed[0]["result"]["parsed_files"], 1);
    assert_eq!(changed[0]["result"]["reused_files"], 1);
    assert!(changed[0]["result"]["symbols"]
        .as_array()
        .unwrap()
        .iter()
        .any(|symbol| symbol["name"] == "changed"));

    let repository_map = runtime.handle_line(&request(
        "8",
        "workspace/repository-map",
        json!({
            "project_id": project_id,
            "token_budget": 256,
            "focus_paths": ["src/main.rs"]
        }),
    ));
    assert_eq!(repository_map[0]["result"]["token_budget"], 256);
    assert!(
        repository_map[0]["result"]["estimated_tokens"]
            .as_u64()
            .unwrap()
            <= 256
    );
    assert_eq!(
        repository_map[0]["result"]["included_files"][0],
        "src/main.rs"
    );
    assert!(repository_map[0]["result"]["text"]
        .as_str()
        .unwrap()
        .contains("function changed"));
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn workspace_index_cancellation_keeps_the_previous_complete_snapshot() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-index-cancel-{unique}"));
    fs::create_dir_all(&root).unwrap();
    fs::write(root.join("main.rs"), "pub fn ready() {}\n").unwrap();
    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request("2", "project/open", json!({ "root": root })));
    let project_id = opened[0]["result"]["project"]["id"].as_str().unwrap();
    let cancelled = runtime.handle_line(&request(
        "3",
        "workspace/index/cancel",
        json!({ "project_id": project_id, "index_id": "index-cancelled" }),
    ));
    assert_eq!(cancelled[0]["result"]["cancelled"], true);
    let skipped = runtime.handle_line(&request(
        "4",
        "workspace/index",
        json!({ "project_id": project_id, "index_id": "index-cancelled" }),
    ));
    assert_eq!(skipped[0]["result"]["cancelled"], true);
    assert_eq!(skipped[0]["result"]["indexed_files"], 0);
    let completed = runtime.handle_line(&request(
        "5",
        "workspace/index",
        json!({ "project_id": project_id, "index_id": "index-completed" }),
    ));
    assert_eq!(completed[0]["result"]["cancelled"], false);
    assert_eq!(completed[0]["result"]["indexed_files"], 1);
    assert!(completed[0]["result"]["symbols"]
        .as_array()
        .unwrap()
        .iter()
        .any(|symbol| symbol["name"] == "ready"));
    let wrong_project = runtime.handle_line(&request(
        "6",
        "workspace/index/cancel",
        json!({ "project_id": "project-missing", "index_id": "index-completed" }),
    ));
    assert_eq!(wrong_project[0]["error"]["code"], -32022);
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn unicode_rename_updates_watch_and_incremental_index() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-rename-{unique}"));
    fs::create_dir_all(root.join("src")).unwrap();
    fs::write(root.join("src/旧文件.rs"), "pub fn old_name() {}\n").unwrap();
    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request("2", "project/open", json!({ "root": root })));
    let project_id = opened[0]["result"]["project"]["id"].as_str().unwrap();
    let initial = runtime.handle_line(&request(
        "3",
        "workspace/index",
        json!({ "project_id": project_id, "index_id": "rename-before" }),
    ));
    assert!(initial[0]["result"]["symbols"]
        .as_array()
        .unwrap()
        .iter()
        .any(|symbol| symbol["path"] == "src/旧文件.rs"));
    let watched = runtime.handle_line(&request(
        "4",
        "workspace/watch",
        json!({ "project_id": project_id, "paths": ["src"] }),
    ));
    let watch_id = watched[0]["result"]["watch_id"].as_str().unwrap();
    fs::rename(root.join("src/旧文件.rs"), root.join("src/新文件.rs")).unwrap();
    fs::write(root.join("src/新文件.rs"), "pub fn new_name() {}\n").unwrap();
    let changes = runtime.handle_line(&request(
        "5",
        "workspace/watch/poll",
        json!({ "watch_id": watch_id }),
    ));
    let changes = changes[0]["result"]["changes"].as_array().unwrap();
    assert!(changes
        .iter()
        .any(|change| change["path"] == "src/旧文件.rs" && change["kind"] == "deleted"));
    assert!(changes
        .iter()
        .any(|change| change["path"] == "src/新文件.rs" && change["kind"] == "created"));
    let refreshed = runtime.handle_line(&request(
        "6",
        "workspace/index",
        json!({ "project_id": project_id, "index_id": "rename-after" }),
    ));
    let symbols = refreshed[0]["result"]["symbols"].as_array().unwrap();
    assert!(symbols
        .iter()
        .any(|symbol| symbol["path"] == "src/新文件.rs" && symbol["name"] == "new_name"));
    assert!(!symbols
        .iter()
        .any(|symbol| symbol["path"] == "src/旧文件.rs"));
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn language_server_bridge_reports_availability_and_maps_clangd_results() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-lsp-{unique}"));
    fs::create_dir_all(&root).unwrap();
    let path = root.join("main.cpp");
    let content = "int add(int a, int b) { return a + b; }\nint main() { return add(1, 2); }\n";
    fs::write(&path, content).unwrap();

    let mut runtime = ready_runtime();
    let opened = runtime.handle_line(&request("2", "project/open", json!({ "root": root })));
    let project_id = opened[0]["result"]["project"]["id"].as_str().unwrap();
    let servers = runtime.handle_line(&request(
        "3",
        "workspace/language-servers",
        json!({ "project_id": project_id }),
    ));
    let clangd = servers[0]["result"]["servers"]
        .as_array()
        .unwrap()
        .iter()
        .find(|server| server["server_id"] == "clangd")
        .unwrap();
    assert_eq!(clangd["running"], false);
    let clangd_installed = clangd["installed"].as_bool().unwrap();

    let file = runtime.handle_line(&request(
        "4",
        "workspace/read",
        json!({ "project_id": project_id, "path": "main.cpp" }),
    ));
    let definition = runtime.handle_line(&request(
        "5",
        "workspace/definition",
        json!({
            "project_id": project_id,
            "path": "main.cpp",
            "content": content,
            "revision": file[0]["result"]["revision"],
            "line": 2,
            "column": 21
        }),
    ));
    if !clangd_installed {
        assert_eq!(definition[0]["error"]["code"], -32070);
        fs::remove_dir_all(root).unwrap();
        return;
    }
    assert_eq!(definition[0]["result"]["server_id"], "clangd");
    assert!(definition[0]["result"]["locations"]
        .as_array()
        .unwrap()
        .iter()
        .any(|location| location["path"] == "main.cpp" && location["line"] == 1));

    let references = runtime.handle_line(&request(
        "6",
        "workspace/references",
        json!({
            "project_id": project_id,
            "path": "main.cpp",
            "content": content,
            "revision": file[0]["result"]["revision"],
            "line": 1,
            "column": 5
        }),
    ));
    assert!(
        references[0]["result"]["locations"]
            .as_array()
            .unwrap()
            .len()
            >= 2
    );

    let broken = "int main() { return missing_name; }\n";
    fs::write(&path, broken).unwrap();
    let broken_file = runtime.handle_line(&request(
        "7",
        "workspace/read",
        json!({ "project_id": project_id, "path": "main.cpp" }),
    ));
    let diagnostics = runtime.handle_line(&request(
        "8",
        "workspace/diagnostics",
        json!({
            "project_id": project_id,
            "path": "main.cpp",
            "content": broken,
            "revision": broken_file[0]["result"]["revision"]
        }),
    ));
    assert_eq!(diagnostics[0]["result"]["pending"], false);
    assert!(diagnostics[0]["result"]["diagnostics"]
        .as_array()
        .unwrap()
        .iter()
        .any(|diagnostic| {
            diagnostic["path"] == "main.cpp"
                && diagnostic["severity"] == "error"
                && diagnostic["provenance"] == "language-server:clangd"
        }));
    let observed = diagnostics[0]["result"]["observed_diagnostics"]
        .as_array()
        .unwrap();
    assert!(observed.iter().any(|diagnostic| {
        diagnostic["source_kind"] == "language-server"
            && diagnostic["source_server"] == "clangd"
            && diagnostic["source_command"].is_null()
            && diagnostic["file_hash"] == broken_file[0]["result"]["revision"]
            && diagnostic["freshness"] == "fresh"
    }));
    let raw_reference = diagnostics[0]["result"]["raw_output_ref"].as_str().unwrap();
    assert!(raw_reference.starts_with("diagnostic-raw:sha256:"));

    let listed = runtime.handle_line(&request(
        "9",
        "workspace/observed-diagnostics",
        json!({ "project_id": project_id, "path": "main.cpp" }),
    ));
    assert_eq!(listed[0]["result"]["fresh_count"], 1);
    assert_eq!(listed[0]["result"]["stale_count"], 0);
    let raw = runtime.handle_line(&request(
        "10",
        "workspace/diagnostics/raw",
        json!({ "project_id": project_id, "reference": raw_reference }),
    ));
    assert_eq!(
        raw[0]["result"]["content_type"],
        "application/vnd.aegisy.diagnostics+json"
    );
    assert_eq!(raw[0]["result"]["sha256"].as_str().unwrap().len(), 64);
    assert!(raw[0]["result"]["content"]
        .as_str()
        .unwrap()
        .contains("missing_name"));

    let saved = runtime.handle_line(&request(
        "11",
        "workspace/save-user-text",
        json!({
            "project_id": project_id,
            "path": "main.cpp",
            "content": "int main() { return 0; }\n",
            "expected_revision": broken_file[0]["result"]["revision"],
            "encoding": broken_file[0]["result"]["encoding"],
            "newline": broken_file[0]["result"]["newline"],
            "origin": "user"
        }),
    ));
    assert!(saved[0].get("result").is_some());
    let stale = runtime.handle_line(&request(
        "12",
        "workspace/observed-diagnostics",
        json!({ "project_id": project_id, "path": "main.cpp" }),
    ));
    assert_eq!(stale[0]["result"]["fresh_count"], 0);
    assert_eq!(stale[0]["result"]["stale_count"], 1);
    assert_eq!(stale[0]["result"]["diagnostics"][0]["freshness"], "stale");

    let stopped = runtime.handle_line(&request(
        "13",
        "workspace/language-server/stop",
        json!({ "project_id": project_id, "path": "main.cpp" }),
    ));
    assert_eq!(stopped[0]["result"]["stopped"], true);
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn session_compaction_checkpoint_is_durable_review_only_and_idempotent() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-compaction-{unique}"));
    let data = root.join("data");
    fs::create_dir_all(&data).unwrap();

    let mut runtime = Runtime::with_store(&data).unwrap();
    let initialized = runtime.handle_line(&request(
        "1",
        "initialize",
        json!({ "protocol_version": "0.1", "client": { "name": "test", "version": "1" } }),
    ));
    assert!(initialized[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "session.compaction.checkpoint-review"));
    runtime.handle_line(&request("initialized", "initialized", json!({})));
    let session = runtime.handle_line(&request("2", "session/start", json!({ "mode": "chat" })));
    let session_id = session[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let params = json!({
        "session_id": session_id,
        "checkpoint_id": "checkpoint-1",
        "preservation_instructions": "Preserve all unresolved work",
        "summary": {
            "decisions": ["Keep original event history authoritative"],
            "unresolved_tasks": ["Add editable Qt review"],
            "changed_files": [],
            "commands": [],
            "tests": ["protocol"],
            "failures": [],
            "next_actions": ["Integrate model summary generation"]
        }
    });
    let created = runtime.handle_line(&request(
        "3",
        "session/compaction/checkpoint/create",
        params.clone(),
    ));
    assert_eq!(
        created[0]["result"]["schema_version"],
        "session-compaction-checkpoint-create-result/0.1"
    );
    assert_eq!(created[0]["result"]["idempotent_replay"], false);
    assert_eq!(created[0]["result"]["activation_available"], false);
    assert_eq!(created[0]["result"]["provider_compact_invoked"], false);
    assert_eq!(
        created[0]["result"]["original_event_history_authoritative"],
        true
    );
    let review_id = created[0]["result"]["review"]["review_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let event_sequence = created[0]["result"]["event_sequence"].as_u64().unwrap();

    let duplicate = runtime.handle_line(&request(
        "4",
        "session/compaction/checkpoint/create",
        params.clone(),
    ));
    assert_eq!(duplicate[0]["result"]["idempotent_replay"], true);
    assert_eq!(duplicate[0]["result"]["review"]["review_id"], review_id);
    assert_eq!(duplicate[0]["result"]["event_sequence"], event_sequence);
    let mut conflicting = params.clone();
    conflicting["summary"]["next_actions"] = json!(["Different content"]);
    let conflict = runtime.handle_line(&request(
        "5",
        "session/compaction/checkpoint/create",
        conflicting,
    ));
    assert_eq!(conflict[0]["error"]["code"], -32009);

    let revision_params = json!({
        "session_id": session_id,
        "source_checkpoint_id": "checkpoint-1",
        "source_review_id": review_id,
        "checkpoint_id": "checkpoint-2",
        "preservation_instructions": "Preserve unresolved work and reviewed decisions",
        "summary": {
            "decisions": ["Keep original event history authoritative"],
            "unresolved_tasks": ["Activate only after an explicit review"],
            "changed_files": [],
            "commands": [],
            "tests": ["protocol", "restart replay"],
            "failures": [],
            "next_actions": ["Integrate provider-safe activation"]
        }
    });
    let revised = runtime.handle_line(&request(
        "revision",
        "session/compaction/checkpoint/revise",
        revision_params.clone(),
    ));
    assert_eq!(
        revised[0]["result"]["schema_version"],
        "session-compaction-checkpoint-revise-result/0.1"
    );
    assert_eq!(revised[0]["result"]["idempotent_replay"], false);
    assert_eq!(
        revised[0]["result"]["supersedes"]["checkpoint_id"],
        "checkpoint-1"
    );
    assert_eq!(revised[0]["result"]["supersedes"]["review_id"], review_id);
    assert_eq!(revised[0]["result"]["activation_available"], false);
    assert_eq!(revised[0]["result"]["provider_compact_invoked"], false);
    let revised_review_id = revised[0]["result"]["review"]["review_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let revised_event_sequence = revised[0]["result"]["event_sequence"].as_u64().unwrap();

    let duplicate_revision = runtime.handle_line(&request(
        "revision-duplicate",
        "session/compaction/checkpoint/revise",
        revision_params.clone(),
    ));
    assert_eq!(duplicate_revision[0]["result"]["idempotent_replay"], true);
    assert_eq!(
        duplicate_revision[0]["result"]["event_sequence"],
        revised_event_sequence
    );
    let mut stale_source = revision_params.clone();
    stale_source["source_review_id"] = json!(revised_review_id);
    let stale_source = runtime.handle_line(&request(
        "revision-stale-source",
        "session/compaction/checkpoint/revise",
        stale_source,
    ));
    assert_eq!(stale_source[0]["error"]["code"], -32009);
    let mut same_id = revision_params.clone();
    same_id["checkpoint_id"] = json!("checkpoint-1");
    let same_id = runtime.handle_line(&request(
        "revision-same-id",
        "session/compaction/checkpoint/revise",
        same_id,
    ));
    assert_eq!(same_id[0]["error"]["code"], -32602);

    let read = runtime.handle_line(&request(
        "6",
        "session/compaction/checkpoint/read",
        json!({ "session_id": session_id, "checkpoint_id": "checkpoint-1" }),
    ));
    assert_eq!(read[0]["result"]["review"]["review_id"], review_id);
    assert_eq!(read[0]["result"]["provider_compact_invoked"], false);
    drop(runtime);

    let mut restarted = Runtime::with_store(&data).unwrap();
    restarted.handle_line(&request(
        "7",
        "initialize",
        json!({ "protocol_version": "0.1", "client": { "name": "test", "version": "1" } }),
    ));
    restarted.handle_line(&request("initialized-2", "initialized", json!({})));
    let replayed = restarted.handle_line(&request(
        "8",
        "session/compaction/checkpoint/read",
        json!({ "session_id": session_id, "checkpoint_id": "checkpoint-1" }),
    ));
    assert_eq!(replayed[0]["result"]["review"]["review_id"], review_id);
    assert_eq!(replayed[0]["result"]["event_sequence"], event_sequence);
    let replayed_revision = restarted.handle_line(&request(
        "9",
        "session/compaction/checkpoint/read",
        json!({ "session_id": session_id, "checkpoint_id": "checkpoint-2" }),
    ));
    assert_eq!(
        replayed_revision[0]["result"]["review"]["review_id"],
        revised_review_id
    );
    assert_eq!(
        replayed_revision[0]["result"]["event_sequence"],
        revised_event_sequence
    );
    assert_eq!(
        replayed_revision[0]["result"]["supersedes"]["review_id"],
        review_id
    );
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn unavailable_compaction_store_degrades_without_blocking_runtime() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("aegisy-aap-compaction-degraded-{unique}"));
    let data = root.join("data");
    fs::create_dir_all(&data).unwrap();
    fs::write(
        data.join("session-compaction-checkpoints-v1"),
        "unsafe-store-path",
    )
    .unwrap();

    let mut runtime = Runtime::with_store(&data).unwrap();
    let initialized = runtime.handle_line(&request(
        "1",
        "initialize",
        json!({ "protocol_version": "0.1", "client": { "name": "test", "version": "1" } }),
    ));
    assert!(!initialized[0]["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "session.compaction.checkpoint-review"));
    runtime.handle_line(&request("initialized", "initialized", json!({})));
    let session = runtime.handle_line(&request("2", "session/start", json!({ "mode": "chat" })));
    let session_id = session[0]["result"]["session"]["id"].as_str().unwrap();
    let unavailable = runtime.handle_line(&request(
        "3",
        "session/compaction/checkpoint/create",
        json!({
            "session_id": session_id,
            "checkpoint_id": "checkpoint-1",
            "summary": {
                "decisions": [],
                "unresolved_tasks": [],
                "changed_files": [],
                "commands": [],
                "tests": [],
                "failures": [],
                "next_actions": []
            }
        }),
    ));
    assert_eq!(unavailable[0]["error"]["code"], -32024);
    fs::remove_dir_all(root).unwrap();
}
