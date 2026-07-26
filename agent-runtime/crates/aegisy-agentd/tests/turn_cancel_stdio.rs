#![cfg(unix)]

use aegisy_aap::MAX_AAP_FRAME_BYTES;
use aegisy_agentd::turn_trace::{
    CompletionDomain, ErrorClass as TraceErrorClass, EvidenceSource as TraceEvidenceSource,
    RuntimeDenialAttribution, RuntimeDenialRequestKind, RuntimeDenialResponseState,
    SessionMode as TraceSessionMode, TerminalState as TraceTerminalState, ToolProviderStatus,
    ToolSource, ToolState, ToolTimelineBinding, TracePayload, TurnAccess as TraceTurnAccess,
    TurnKind as TraceTurnKind, UsageAccounting, UsageAttribution, UsageReportScope,
};
use aegisy_agentd::workbench_store::WorkbenchStore;
use aegisy_agentd::STABLE_CAPABILITY_REGISTRY;
#[cfg(target_os = "macos")]
use base64::engine::general_purpose::STANDARD as BASE64_STANDARD;
#[cfg(target_os = "macos")]
use base64::Engine;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{BufRead, BufReader, Write};
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;
use std::process::{ChildStdin, Command, Stdio};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

fn request(id: &str, method: &str, params: Value) -> Value {
    json!({ "jsonrpc": "2.0", "id": id, "method": method, "params": params })
}

fn initialize_params(client_name: &str) -> Value {
    // These stdio lifecycle fixtures intentionally exercise the legacy bare-event
    // route. Subscription negotiation has dedicated subscribe/recovery/activate
    // integration coverage.
    let stable_capabilities = STABLE_CAPABILITY_REGISTRY
        .iter()
        .copied()
        .filter(|capability| *capability != "timeline.subscription.fixed-watermark")
        .collect::<Vec<_>>();
    json!({
        "protocol": {"minimum": "0.1", "maximum": "0.1", "preferred": "0.1"},
        "client": {"name": client_name, "version": "1"},
        "platform": {"os": "macos", "architecture": "arm64"},
        "capabilities": {"stable": stable_capabilities, "experimental": []},
        "limits": {"max_frame_bytes": MAX_AAP_FRAME_BYTES},
        "transport_security": {
            "transport": "stdio",
            "local": true,
            "authenticated": false,
            "encrypted": false,
            "peer_verified": false
        }
    })
}

fn send(stdin: &mut ChildStdin, message: &Value) {
    serde_json::to_writer(&mut *stdin, message).unwrap();
    stdin.write_all(b"\n").unwrap();
    stdin.flush().unwrap();
}

fn receive_until<F>(receiver: &mpsc::Receiver<Value>, predicate: F) -> Value
where
    F: Fn(&Value) -> bool,
{
    let deadline = std::time::Instant::now() + Duration::from_secs(15);
    loop {
        let remaining = deadline.saturating_duration_since(std::time::Instant::now());
        let message = receiver
            .recv_timeout(remaining)
            .expect("sidecar did not emit the expected cancellation message");
        if predicate(&message) {
            return message;
        }
    }
}

fn assert_atomic_failed_terminal(
    store: &WorkbenchStore,
    session_id: &str,
    turn_id: &str,
    expected_class: &str,
) {
    let error_items = store
        .read_session_items(session_id, 0, 200)
        .unwrap()
        .into_iter()
        .filter(|item| item.turn_id.as_deref() == Some(turn_id) && item.item_kind == "error")
        .collect::<Vec<_>>();
    assert_eq!(error_items.len(), 1);
    let error_item = &error_items[0];
    assert_eq!(error_item.payload["data"]["class"], expected_class);
    assert_eq!(error_item.payload["data"]["terminal_persisted"], true);

    let page = store
        .sync_public_timeline(session_id, 0, None, 200)
        .unwrap();
    let terminal_events = page
        .events
        .iter()
        .filter(|event| event.turn_id == turn_id && event.event == "turn.failed")
        .collect::<Vec<_>>();
    assert_eq!(terminal_events.len(), 1);
    let terminal = terminal_events[0];
    let public_item = terminal
        .item
        .as_ref()
        .expect("failed terminal must carry its durable Error Item");
    assert_eq!(public_item.id, error_item.item_id);
    assert_eq!(public_item.kind, error_item.item_kind);
    assert_eq!(public_item.role, error_item.role);
    assert_eq!(public_item.state, error_item.state);
    assert_eq!(
        public_item.content,
        error_item.payload["content"].as_str().unwrap()
    );
    assert_eq!(public_item.data.as_ref(), error_item.payload.get("data"));
    assert_eq!(terminal.timestamp_ms, error_item.created_at_ms);
}

fn start_codex_runtime(
    codex: &PathBuf,
    data_root: &PathBuf,
    prefix: &str,
) -> (
    std::process::Child,
    ChildStdin,
    mpsc::Receiver<Value>,
    thread::JoinHandle<()>,
    String,
) {
    fs::create_dir_all(data_root).unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", codex)
        .env("AEGISY_WORKBENCH_DATA_ROOT", data_root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });
    let initialize_id = format!("{prefix}-initialize");
    send(
        &mut stdin,
        &request(&initialize_id, "initialize", initialize_params("test")),
    );
    receive_until(&receiver, |message| message["id"] == initialize_id);
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );
    let session_id = format!("{prefix}-session");
    send(
        &mut stdin,
        &request(&session_id, "session/start", json!({ "mode": "chat" })),
    );
    let session = receive_until(&receiver, |message| message["id"] == session_id);
    let session_id = session["result"]["session"]["id"]
        .as_str()
        .unwrap_or_else(|| panic!("session did not start: {session}"))
        .to_owned();
    (child, stdin, receiver, reader, session_id)
}

#[test]
fn stdio_drains_physical_oversized_frame_reports_fixed_error_and_continues() {
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_AGENT_BACKEND", "preview")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });

    stdin
        .write_all(&vec![
            b'x';
            usize::try_from(MAX_AAP_FRAME_BYTES).unwrap() + 1
        ])
        .unwrap();
    stdin.write_all(b"\n").unwrap();
    send(
        &mut stdin,
        &request(
            "initialize-after-oversized",
            "initialize",
            initialize_params("oversized"),
        ),
    );

    let oversized = receive_until(&receiver, |message| message["error"]["code"] == -32005);
    assert_eq!(oversized["jsonrpc"], "2.0");
    assert!(oversized["id"].is_null());
    let initialized = receive_until(&receiver, |message| {
        message["id"] == "initialize-after-oversized"
    });
    assert_eq!(initialized["result"]["protocol"]["selected"], "0.1");

    send(
        &mut stdin,
        &json!({"jsonrpc": "2.0", "method": "initialized", "params": {}}),
    );
    send(&mut stdin, &request("shutdown", "shutdown", json!({})));
    receive_until(&receiver, |message| message["id"] == "shutdown");
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();
}

fn fake_codex() -> PathBuf {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let directory = std::env::temp_dir().join(format!("aegisy-cancel-fixture-{nonce}"));
    fs::create_dir_all(&directory).unwrap();
    let executable = directory.join("codex-fixture.sh");
    fs::write(
        &executable,
        r#"#!/bin/sh
if [ "$1" = "--version" ]; then
  echo "codex-cli 0.144.5"
  exit 0
fi
fixture_directory=$(CDPATH= cd -P "${0%/*}" && pwd -P)
printf 'warning: fixture stderr Authorization: Bearer ghp_123456789012345678901234567890\n' >&2
while IFS= read -r line; do
  id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p')
  case "$line" in
    *'"method":"initialize"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      ;;
    *'"method":"thread/start"'*)
      printf '{"id":%s,"result":{"thread":{"id":"thread-fixture"},"modelProvider":"fixture","model":"fixture","approvalPolicy":"never","approvalsReviewer":"user","sandbox":{"type":"readOnly","networkAccess":false}}}\n' "$id"
      ;;
    *'"method":"thread/archive"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      ;;
    *'"method":"thread/unarchive"'*)
      printf '{"id":%s,"result":{"thread":{"id":"thread-fixture"},"modelProvider":"fixture","model":"fixture"}}\n' "$id"
      ;;
    *'"method":"thread/list"'*)
      case "$line" in
        *'"cursor":"cursor-next-1"'*)
          printf '{"id":%s,"result":{"data":[],"nextCursor":null,"backwardsCursor":"cursor-next-1"}}\n' "$id"
          ;;
        *'"cursor":"fixture-sensitive-output"'*)
          printf '{"id":%s,"result":{"data":[],"nextCursor":"Authorization: Bearer ghp_123456789012345678901234567890","backwardsCursor":null}}\n' "$id"
          ;;
        *)
          printf '{"id":%s,"result":{"data":[{"id":"thread-fixture","sessionId":"provider-session-private-sentinel","name":"provider-name-private-sentinel","preview":"provider-preview-private-sentinel","cwd":"/private/provider-workspace-sentinel","path":"/private/provider-rollout-sentinel.jsonl","modelProvider":"provider-model-private-sentinel","source":"appServer","status":{"type":"idle"},"createdAt":10,"updatedAt":20,"recencyAt":20,"ephemeral":false,"forkedFromId":null,"turns":[]}],"nextCursor":"cursor-next-1","backwardsCursor":"cursor-previous-1"}}\n' "$id"
          ;;
      esac
      ;;
    *'"method":"thread/read"'*)
      case "$line" in
        *'"threadId":"thread-long-turn-ids"'*)
          turn_prefix=$(printf '%0256d' 0)
          printf '{"id":%s,"result":{"thread":{"id":"thread-long-turn-ids","sessionId":"provider-session","cwd":"/private/provider-workspace-sentinel","turns":[{"id":"%sa","items":[],"status":"completed"},{"id":"%sb","items":[],"status":"completed"}]}}}\n' "$id" "$turn_prefix" "$turn_prefix"
          ;;
        *)
          printf '{"id":%s,"result":{"thread":{"id":"thread-fixture","sessionId":"provider-session-private-sentinel","name":"provider-name-private-sentinel","preview":"provider-preview-private-sentinel","cwd":"/private/provider-workspace-sentinel","path":"/private/provider-rollout-sentinel.jsonl","modelProvider":"provider-model-private-sentinel","source":"appServer","status":{"type":"idle"},"createdAt":10,"updatedAt":20,"recencyAt":20,"ephemeral":false,"forkedFromId":null,"turns":[{"id":"turn-fixture","items":[{"type":"userMessage","content":"provider-item-private-sentinel"}],"status":"completed","startedAt":10,"completedAt":20,"durationMs":10}]}}}\n' "$id"
          ;;
      esac
      ;;
    *'"method":"turn/start"'*)
      printf '{"id":%s,"result":{"turn":{"id":"turn-fixture"}}}\n' "$id"
      case "$line" in
        *'emit agent lifecycle'*)
          printf '{"method":"item/started","params":{"threadId":"thread-fixture","turnId":"turn-fixture","item":{"id":"agent-message-fixture","type":"agentMessage","text":""}}}\n'
          printf '{"method":"item/agentMessage/delta","params":{"threadId":"thread-fixture","turnId":"turn-fixture","itemId":"agent-message-fixture","delta":"hello"}}\n'
          printf '{"method":"item/completed","params":{"threadId":"thread-fixture","turnId":"turn-fixture","item":{"id":"agent-message-fixture","type":"agentMessage","text":"hello"}}}\n'
          printf '{"method":"turn/completed","params":{"threadId":"thread-fixture","turn":{"id":"turn-fixture","status":"completed"}}}\n'
          ;;
        *'emit agent delta before start'*)
          printf '{"method":"item/agentMessage/delta","params":{"threadId":"thread-fixture","turnId":"turn-fixture","itemId":"agent-message-fixture","delta":"invalid"}}\n'
          ;;
        *'emit duplicate agent completion'*)
          printf '{"method":"item/started","params":{"threadId":"thread-fixture","turnId":"turn-fixture","item":{"id":"agent-message-fixture","type":"agentMessage","text":""}}}\n'
          printf '{"method":"item/completed","params":{"threadId":"thread-fixture","turnId":"turn-fixture","item":{"id":"agent-message-fixture","type":"agentMessage","text":"done"}}}\n'
          printf '{"method":"item/completed","params":{"threadId":"thread-fixture","turnId":"turn-fixture","item":{"id":"agent-message-fixture","type":"agentMessage","text":"done"}}}\n'
          ;;
        *'emit terminal with open agent'*)
          printf '{"method":"item/started","params":{"threadId":"thread-fixture","turnId":"turn-fixture","item":{"id":"agent-message-fixture","type":"agentMessage","text":"partial"}}}\n'
          printf '{"method":"turn/completed","params":{"threadId":"thread-fixture","turn":{"id":"turn-fixture","status":"completed"}}}\n'
          ;;
        *'emit metadata'*)
          printf '{"method":"thread/tokenUsage/updated","params":{"threadId":"thread-fixture","turnId":"turn-fixture","tokenUsage":{"last":{"cachedInputTokens":1,"inputTokens":2,"outputTokens":3,"reasoningOutputTokens":0,"totalTokens":5},"total":{"cachedInputTokens":1,"inputTokens":2,"outputTokens":3,"reasoningOutputTokens":0,"totalTokens":5},"modelContextWindow":128000}}}\n'
          printf '{"method":"thread/tokenUsage/updated","params":{"threadId":"thread-fixture","turnId":"turn-fixture","tokenUsage":{"last":{"cachedInputTokens":4,"inputTokens":20,"outputTokens":7,"reasoningOutputTokens":2,"totalTokens":27},"total":{"cachedInputTokens":10,"inputTokens":50,"outputTokens":15,"reasoningOutputTokens":4,"totalTokens":65},"modelContextWindow":128000}}}\n'
          printf '{"method":"turn/plan/updated","params":{"threadId":"thread-fixture","turnId":"turn-fixture","explanation":"Inspect then verify","plan":[{"status":"inProgress","step":"Inspect"},{"status":"pending","step":"Verify"}]}}\n'
          printf '{"method":"turn/diff/updated","params":{"threadId":"thread-fixture","turnId":"turn-fixture","diff":"@@ -1 +1 @@\\n-old\\n+new\\n"}}\n'
          printf '{"method":"turn/completed","params":{"threadId":"thread-fixture","turn":{"id":"turn-fixture","status":"completed"}}}\n'
          ;;
        *'emit unknown diagnostics'*)
          printf '{"method":"future/private-method-sk-never-log","params":{"threadId":"thread-fixture","turnId":"turn-fixture","body":"ghp_unknown_notification_private_body","path":"/private/unknown/source.rs","content":"private provider content"}}\n'
          printf '{"method":"turn/completed","params":{"threadId":"thread-fixture","turn":{"id":"turn-fixture","status":"completed"}}}\n'
          ;;
        *'request unsupported user input'*)
          printf '{"id":"user-input-request-sentinel","method":"item/tool/requestUserInput","params":{"threadId":"thread-fixture","turnId":"turn-fixture","itemId":"user-input-item-private-sentinel","questions":[{"header":"user-input-header-private-sentinel","question":"user-input-question-private-sentinel","options":[{"label":"user-input-option-private-sentinel","description":"ghp_user_input_private_sentinel"}]}]}}\n'
          ;;
        *'emit incomplete command'*)
          tool_started_at_ms="$(date +%s)999"
          sleep 1
          printf '{"method":"item/started","params":{"threadId":"thread-fixture","turnId":"turn-fixture","startedAtMs":%s,"item":{"id":"command-incomplete","type":"commandExecution","command":"printf pending","commandActions":[{"type":"unknown","command":"printf pending"}],"cwd":"%s/tool-project","status":"inProgress","source":"agent"}}}\n' "$tool_started_at_ms" "$fixture_directory"
          printf '{"method":"turn/completed","params":{"threadId":"thread-fixture","turn":{"id":"turn-fixture","status":"completed"}}}\n'
          ;;
        *'emit diagnostics'*)
          tool_started_at_ms="$(date +%s)999"
          tool_completed_at_ms=$((tool_started_at_ms + 12))
          sleep 1
          printf '{"method":"item/started","params":{"threadId":"thread-fixture","turnId":"turn-fixture","startedAtMs":%s,"item":{"id":"command-fixture","type":"commandExecution","command":"cargo check","commandActions":[{"type":"unknown","command":"cargo check"}],"cwd":"%s/project","status":"inProgress","source":"agent"}}}\n' "$tool_started_at_ms" "$fixture_directory"
          printf '{"method":"item/commandExecution/outputDelta","params":{"threadId":"thread-fixture","turnId":"turn-fixture","itemId":"command-fixture","delta":"error[E0425]: cannot find function missing\\n --> src/main.rs:2:5\\n"}}\n'
          sleep 1
          printf '{"method":"item/completed","params":{"threadId":"thread-fixture","turnId":"turn-fixture","completedAtMs":%s,"item":{"id":"command-fixture","type":"commandExecution","command":"cargo check","commandActions":[{"type":"unknown","command":"cargo check"}],"cwd":"%s/project","status":"failed","aggregatedOutput":"error[E0425]: cannot find function missing\\n --> src/main.rs:2:5\\n","durationMs":12,"exitCode":101,"source":"agent"}}}\n' "$tool_completed_at_ms" "$fixture_directory"
          printf '{"method":"turn/completed","params":{"threadId":"thread-fixture","turn":{"id":"turn-fixture","status":"completed"}}}\n'
          ;;
        *'emit completed command'*)
          tool_started_at_ms="$(date +%s)999"
          tool_completed_at_ms=$((tool_started_at_ms + 12))
          sleep 1
          printf '{"method":"item/started","params":{"threadId":"thread-fixture","turnId":"turn-fixture","startedAtMs":%s,"item":{"id":"command-completed","type":"commandExecution","command":"printf done","commandActions":[{"type":"unknown","command":"printf done"}],"cwd":"%s/tool-project","status":"inProgress","source":"agent"}}}\n' "$tool_started_at_ms" "$fixture_directory"
          sleep 1
          printf '{"method":"item/completed","params":{"threadId":"thread-fixture","turnId":"turn-fixture","completedAtMs":%s,"item":{"id":"command-completed","type":"commandExecution","command":"printf done","commandActions":[{"type":"unknown","command":"printf done"}],"cwd":"%s/tool-project","status":"completed","aggregatedOutput":"done\\n","durationMs":12,"exitCode":0,"source":"agent"}}}\n' "$tool_completed_at_ms" "$fixture_directory"
          printf '{"method":"turn/completed","params":{"threadId":"thread-fixture","turn":{"id":"turn-fixture","status":"completed"}}}\n'
          ;;
        *'emit declined command'*)
          tool_started_at_ms="$(date +%s)999"
          tool_completed_at_ms=$((tool_started_at_ms + 1))
          sleep 1
          printf '{"method":"item/started","params":{"threadId":"thread-fixture","turnId":"turn-fixture","startedAtMs":%s,"item":{"id":"command-declined","type":"commandExecution","command":"blocked-command","commandActions":[{"type":"unknown","command":"blocked-command"}],"cwd":"%s/tool-project","status":"inProgress","source":"agent"}}}\n' "$tool_started_at_ms" "$fixture_directory"
          sleep 1
          printf '{"method":"item/completed","params":{"threadId":"thread-fixture","turnId":"turn-fixture","completedAtMs":%s,"item":{"id":"command-declined","type":"commandExecution","command":"blocked-command","commandActions":[{"type":"unknown","command":"blocked-command"}],"cwd":"%s/tool-project","status":"declined","aggregatedOutput":null,"durationMs":null,"exitCode":null,"source":"agent"}}}\n' "$tool_completed_at_ms" "$fixture_directory"
          printf '{"method":"turn/completed","params":{"threadId":"thread-fixture","turn":{"id":"turn-fixture","status":"completed"}}}\n'
          ;;
        *'wait for cancellation'*)
          printf '{"method":"thread/tokenUsage/updated","params":{"threadId":"thread-fixture","turnId":"turn-fixture","tokenUsage":{"last":{"cachedInputTokens":1,"inputTokens":8,"outputTokens":2,"reasoningOutputTokens":1,"totalTokens":10},"total":{"cachedInputTokens":1,"inputTokens":8,"outputTokens":2,"reasoningOutputTokens":1,"totalTokens":10},"modelContextWindow":128000}}}\n'
          ;;
      esac
      ;;
    *'"id":"user-input-request-sentinel"'*)
      printf '%s\n' "$line" > "$fixture_directory/user-input-response.json"
      printf '{"method":"turn/completed","params":{"threadId":"thread-fixture","turn":{"id":"turn-fixture","status":"completed"}}}\n'
      ;;
    *'"method":"turn/steer"'*)
      printf '{"id":%s,"result":{"turnId":"turn-fixture"}}\n' "$id"
      ;;
    *'"method":"turn/interrupt"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      printf '{"method":"turn/completed","params":{"threadId":"thread-fixture","turn":{"id":"turn-fixture","status":"interrupted"}}}\n'
      ;;
  esac
done
"#,
    )
    .unwrap();
    let mut permissions = fs::metadata(&executable).unwrap().permissions();
    permissions.set_mode(0o755);
    fs::set_permissions(&executable, permissions).unwrap();
    executable
}

#[test]
fn stdio_agent_message_lifecycle_is_ordered_and_fails_closed() {
    for (prefix, case, terminal, expected_item_events) in [
        (
            "agent-valid",
            "emit agent lifecycle",
            "turn.completed",
            vec!["item.started", "item.delta", "item.completed"],
        ),
        (
            "agent-delta-before-start",
            "emit agent delta before start",
            "turn.failed",
            vec![],
        ),
        (
            "agent-duplicate-completed",
            "emit duplicate agent completion",
            "turn.failed",
            vec!["item.started", "item.completed"],
        ),
        (
            "agent-open-terminal",
            "emit terminal with open agent",
            "turn.failed",
            vec!["item.started"],
        ),
    ] {
        let codex = fake_codex();
        let fixture_root = codex.parent().unwrap().to_path_buf();
        let data_root = fixture_root.join("agent-lifecycle-workbench");
        let (mut child, mut stdin, receiver, reader, session_id) =
            start_codex_runtime(&codex, &data_root, prefix);
        let request_id = format!("{prefix}-turn");
        send(
            &mut stdin,
            &request(
                &request_id,
                "turn/start",
                json!({
                    "session_id": session_id,
                    "input": case,
                    "idempotency_key": request_id,
                    "generation": 1
                }),
            ),
        );

        let mut messages = Vec::new();
        loop {
            let message = receiver
                .recv_timeout(Duration::from_secs(15))
                .expect("sidecar did not finish the agent-message lifecycle fixture");
            let is_terminal = message["method"] == "event"
                && message["params"]["event"].as_str() == Some(terminal);
            messages.push(message);
            if is_terminal {
                break;
            }
        }
        assert!(messages.iter().any(|message| message["id"] == request_id));
        let item_events = messages
            .iter()
            .filter(|message| message["params"]["item"]["id"] == "agent-message-fixture")
            .map(|message| message["params"]["event"].as_str().unwrap())
            .collect::<Vec<_>>();
        assert_eq!(item_events, expected_item_events, "fixture {case}");

        let events = messages
            .iter()
            .filter(|message| message["method"] == "event")
            .collect::<Vec<_>>();
        assert_eq!(
            events
                .iter()
                .filter(|message| {
                    matches!(
                        message["params"]["event"].as_str(),
                        Some("turn.completed" | "turn.failed" | "turn.interrupted")
                    )
                })
                .count(),
            1,
            "fixture {case} emitted more than one terminal event"
        );
        for pair in events.windows(2) {
            assert_eq!(
                pair[1]["params"]["sequence"].as_u64().unwrap(),
                pair[0]["params"]["sequence"].as_u64().unwrap() + 1
            );
            assert!(
                pair[0]["params"]["timestamp_ms"].as_u64().unwrap()
                    <= pair[1]["params"]["timestamp_ms"].as_u64().unwrap()
            );
        }
        if terminal == "turn.failed" {
            let failed = events.last().unwrap();
            assert_eq!(failed["params"]["turn_state"], "failed");
            assert_eq!(failed["params"]["item"]["data"]["class"], "protocol");
            assert_eq!(failed["params"]["item"]["data"]["retryable"], false);
        }

        let shutdown_id = format!("{prefix}-shutdown");
        send(&mut stdin, &request(&shutdown_id, "shutdown", json!({})));
        receive_until(&receiver, |message| message["id"] == shutdown_id);
        drop(stdin);
        assert!(child.wait().unwrap().success());
        reader.join().unwrap();
        let _ = fs::remove_dir_all(fixture_root);
    }
}

fn trace_budget_codex() -> PathBuf {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let directory = std::env::temp_dir().join(format!("aegisy-trace-budget-fixture-{nonce}"));
    fs::create_dir_all(&directory).unwrap();
    let executable = directory.join("codex-trace-budget-fixture.sh");
    fs::write(
        &executable,
        r#"#!/bin/sh
if [ "$1" = "--version" ]; then
  echo "codex-cli 0.144.5"
  exit 0
fi
stop_file="$0.stop"
lock_directory="$0.stdout-lock"
generator_pid=""
rm -f "$stop_file"
rmdir "$lock_directory" 2>/dev/null || true

emit_budget_commands() {
  # The Trace binds provider timestamps after the Runtime-observed Turn start.
  # Cross a wall-clock second before sampling the fixture epoch so truncating
  # `date +%s` to milliseconds cannot make the first Tool precede the Turn.
  sleep 1
  base_timestamp_ms="$(date +%s)000"
  command_index=0
  while [ "$command_index" -lt 96 ] && [ ! -f "$stop_file" ]; do
    if [ "$command_index" -eq 0 ]; then
      provider_item_id="command-canary-alice-project-zero"
    else
      provider_item_id="command-$command_index"
    fi
    started_at_ms=$((base_timestamp_ms + command_index * 2))
    completed_at_ms=$((started_at_ms + 1))
    printf '{"method":"item/started","params":{"threadId":"thread-budget","turnId":"turn-budget","startedAtMs":%s,"item":{"id":"%s","type":"commandExecution","command":"printf done","commandActions":[{"type":"unknown","command":"printf done"}],"cwd":"/tmp/provider-workspace","status":"inProgress","source":"agent"}}}\n' "$started_at_ms" "$provider_item_id"
    while ! mkdir "$lock_directory" 2>/dev/null; do sleep 0.01; done
    printf '{"method":"item/completed","params":{"threadId":"thread-budget","turnId":"turn-budget","completedAtMs":%s,"item":{"id":"%s","type":"commandExecution","command":"printf done","commandActions":[{"type":"unknown","command":"printf done"}],"cwd":"/tmp/provider-workspace","status":"completed","aggregatedOutput":"done-%s\\n","durationMs":1,"exitCode":0,"source":"agent"}}}\n' "$completed_at_ms" "$provider_item_id" "$command_index"
    rmdir "$lock_directory"
    command_index=$((command_index + 1))
  done
  if [ ! -f "$stop_file" ]; then
    printf '{"method":"turn/completed","params":{"threadId":"thread-budget","turn":{"id":"turn-budget","status":"completed"}}}\n'
  fi
}

while IFS= read -r line; do
  id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p')
  case "$line" in
    *'"method":"initialize"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      ;;
    *'"method":"thread/start"'*)
      printf '{"id":%s,"result":{"thread":{"id":"thread-budget"},"modelProvider":"fixture","model":"fixture","approvalPolicy":"never","approvalsReviewer":"user","sandbox":{"type":"readOnly","networkAccess":false}}}\n' "$id"
      ;;
    *'"method":"turn/start"'*)
      printf '{"id":%s,"result":{"turn":{"id":"turn-budget"}}}\n' "$id"
      emit_budget_commands &
      generator_pid=$!
      ;;
    *'"method":"turn/interrupt"'*)
      touch "$stop_file"
      while ! mkdir "$lock_directory" 2>/dev/null; do sleep 0.01; done
      printf '{"id":%s,"result":{}}\n' "$id"
      printf '{"method":"turn/completed","params":{"threadId":"thread-budget","turn":{"id":"turn-budget","status":"interrupted"}}}\n'
      rmdir "$lock_directory"
      ;;
    *'"method":"shutdown"'*)
      touch "$stop_file"
      if [ -n "$generator_pid" ]; then wait "$generator_pid"; fi
      printf '{"id":%s,"result":{}}\n' "$id"
      exit 0
      ;;
  esac
done
"#,
    )
    .unwrap();
    let mut permissions = fs::metadata(&executable).unwrap().permissions();
    permissions.set_mode(0o755);
    fs::set_permissions(&executable, permissions).unwrap();
    executable
}

fn crashing_codex() -> PathBuf {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let directory = std::env::temp_dir().join(format!("aegisy-startup-crash-fixture-{nonce}"));
    fs::create_dir_all(&directory).unwrap();
    let executable = directory.join("codex-crash-fixture.sh");
    fs::write(
        &executable,
        r#"#!/bin/sh
if [ "$1" = "--version" ]; then
  echo "codex-cli 0.144.5"
  exit 0
fi
printf 'x' >> "$0.attempts"
exit 17
"#,
    )
    .unwrap();
    let mut permissions = fs::metadata(&executable).unwrap().permissions();
    permissions.set_mode(0o755);
    fs::set_permissions(&executable, permissions).unwrap();
    executable
}

fn restartable_codex() -> PathBuf {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let directory = std::env::temp_dir().join(format!("aegisy-restart-fixture-{nonce}"));
    fs::create_dir_all(&directory).unwrap();
    let executable = directory.join("codex-restart-fixture.sh");
    fs::write(
        &executable,
        r#"#!/bin/sh
if [ "$1" = "--version" ]; then
  echo "codex-cli 0.144.5"
  exit 0
fi
count_file="$0.instances"
count=$(cat "$count_file" 2>/dev/null || echo 0)
count=$((count + 1))
printf '%s' "$count" > "$count_file"
while IFS= read -r line; do
  id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p')
  case "$line" in
    *'"method":"initialize"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      ;;
    *'"method":"initialized"'*)
      if [ "$count" -eq 1 ]; then
        exit 0
      fi
      ;;
    *'"method":"shutdown"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      exit 0
      ;;
  esac
done
"#,
    )
    .unwrap();
    let mut permissions = fs::metadata(&executable).unwrap().permissions();
    permissions.set_mode(0o755);
    fs::set_permissions(&executable, permissions).unwrap();
    executable
}

fn reconnectable_codex() -> PathBuf {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let directory = std::env::temp_dir().join(format!("aegisy-reconnect-fixture-{nonce}"));
    fs::create_dir_all(&directory).unwrap();
    let executable = directory.join("codex-reconnect-fixture.sh");
    fs::write(
        &executable,
        r#"#!/bin/sh
if [ "$1" = "--version" ]; then
  echo "codex-cli 0.144.5"
  exit 0
fi
count_file="$0.instances"
count=$(cat "$count_file" 2>/dev/null || echo 0)
count=$((count + 1))
printf '%s' "$count" > "$count_file"
while IFS= read -r line; do
  id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p')
  case "$line" in
    *'"method":"initialize"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      ;;
    *'"method":"thread/start"'*)
      printf '{"id":%s,"result":{"thread":{"id":"thread-reconnect"},"modelProvider":"fixture","model":"fixture","approvalPolicy":"never","approvalsReviewer":"user","sandbox":{"type":"readOnly","networkAccess":false}}}\n' "$id"
      ;;
    *'"method":"turn/start"'*)
      printf '{"id":%s,"result":{"turn":{"id":"turn-reconnect-%s"}}}\n' "$id" "$count"
      if [ "$count" -eq 1 ]; then
        printf '{"method":"item/started","params":{"threadId":"thread-reconnect","turnId":"turn-reconnect-1","item":{"id":"item-reconnect","type":"agentMessage","text":""}}}\n'
        printf '{"method":"item/agentMessage/delta","params":{"threadId":"thread-reconnect","turnId":"turn-reconnect-1","itemId":"item-reconnect","delta":"partial\\n"}}\n'
        exit 23
      fi
      printf '{"method":"item/started","params":{"threadId":"thread-reconnect","turnId":"turn-reconnect-2","item":{"id":"item-reconnect","type":"agentMessage","text":""}}}\n'
      printf '{"method":"item/agentMessage/delta","params":{"threadId":"thread-reconnect","turnId":"turn-reconnect-2","itemId":"item-reconnect","delta":"recovered"}}\n'
      printf '{"method":"item/completed","params":{"threadId":"thread-reconnect","turnId":"turn-reconnect-2","item":{"id":"item-reconnect","type":"agentMessage","text":"recovered"}}}\n'
      printf '{"method":"turn/completed","params":{"threadId":"thread-reconnect","turn":{"id":"turn-reconnect-2","status":"completed"}}}\n'
      ;;
    *'"method":"shutdown"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      exit 0
      ;;
  esac
done
"#,
    )
    .unwrap();
    let mut permissions = fs::metadata(&executable).unwrap().permissions();
    permissions.set_mode(0o755);
    fs::set_permissions(&executable, permissions).unwrap();
    executable
}

fn provider_failure_codex() -> PathBuf {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let directory = std::env::temp_dir().join(format!("aegisy-provider-failure-fixture-{nonce}"));
    fs::create_dir_all(&directory).unwrap();
    let executable = directory.join("codex-provider-failure-fixture.sh");
    fs::write(
        &executable,
        r#"#!/bin/sh
if [ "$1" = "--version" ]; then
  echo "codex-cli 0.144.5"
  exit 0
fi
while IFS= read -r line; do
  id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p')
  case "$line" in
    *'"method":"initialize"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      ;;
    *'"method":"thread/start"'*)
      printf '{"id":%s,"result":{"thread":{"id":"thread-provider-failure"},"modelProvider":"fixture","model":"fixture","approvalPolicy":"never","approvalsReviewer":"user","sandbox":{"type":"readOnly","networkAccess":false}}}\n' "$id"
      ;;
    *'"method":"turn/start"'*)
      printf '{"id":%s,"result":{"turn":{"id":"turn-provider-failure"}}}\n' "$id"
      printf '{"method":"thread/tokenUsage/updated","params":{"threadId":"thread-provider-failure","turnId":"turn-provider-failure","tokenUsage":{"last":{"cachedInputTokens":1,"inputTokens":12,"outputTokens":3,"reasoningOutputTokens":1,"totalTokens":15},"total":{"cachedInputTokens":1,"inputTokens":12,"outputTokens":3,"reasoningOutputTokens":1,"totalTokens":15},"modelContextWindow":128000}}}\n'
      printf '{"method":"error","params":{"threadId":"thread-provider-failure","turnId":"turn-provider-failure","willRetry":true,"error":{"message":"stream retry includes private provider response body and authorization Bearer ghp_123456789012345678901234567890","codexErrorInfo":null}}}\n'
      printf '{"method":"turn/completed","params":{"threadId":"thread-provider-failure","turn":{"id":"turn-provider-failure","status":"failed","error":{"message":"stream disconnected before completion after authorization Bearer ghp_123456789012345678901234567890","additionalDetails":"private provider response body","codexErrorInfo":{"responseStreamDisconnected":{"httpStatusCode":502}}}}}}\n'
      ;;
    *'"method":"shutdown"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      exit 0
      ;;
  esac
done
"#,
    )
    .unwrap();
    let mut permissions = fs::metadata(&executable).unwrap().permissions();
    permissions.set_mode(0o755);
    fs::set_permissions(&executable, permissions).unwrap();
    executable
}

fn approval_denial_codex() -> PathBuf {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let directory = std::env::temp_dir().join(format!("aegisy-approval-fixture-{nonce}"));
    fs::create_dir_all(&directory).unwrap();
    let executable = directory.join("codex-approval-fixture.sh");
    fs::write(
        &executable,
        r#"#!/bin/sh
if [ "$1" = "--version" ]; then
  echo "codex-cli 0.144.5"
  exit 0
fi
decision_file="$0.decision"
while IFS= read -r line; do
  id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p')
  case "$line" in
    *'"method":"initialize"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      ;;
    *'"method":"thread/start"'*)
      printf '{"id":%s,"result":{"thread":{"id":"thread-approval"},"modelProvider":"fixture","model":"fixture","approvalPolicy":"never","approvalsReviewer":"user","sandbox":{"type":"readOnly","networkAccess":false}}}\n' "$id"
      ;;
    *'"method":"turn/start"'*)
      printf '{"id":%s,"result":{"turn":{"id":"turn-approval"}}}\n' "$id"
      printf '{"id":"runtime-denial-request-id-sentinel","method":"item/commandExecution/requestApproval","params":{"threadId":"thread-approval","turnId":"turn-approval","itemId":"runtime-denial-command-item-sentinel","startedAtMs":10,"command":"runtime-denial-command-sentinel","risk":"high","unknown":{"path":"/runtime-denial-command-path-sentinel","pid":4815162342,"credential":"runtime-denial-credential-sentinel","userId":"runtime-denial-user-id-sentinel"}}}\n'
      ;;
    *'"id":"runtime-denial-request-id-sentinel","result":{"decision":"decline"}'*)
      printf '%s' "command" > "$decision_file"
      printf '{"method":"item/fileChange/patchUpdated","params":{"threadId":"thread-approval","turnId":"turn-approval","itemId":"runtime-denial-file-item-sentinel","changes":[{"path":"proposal.txt","kind":{"type":"add"},"diff":"proposed\\n"}]}}\n'
      file_started_at_ms=$(($(date +%s) * 1000))
      printf '{"method":"item/started","params":{"threadId":"thread-approval","turnId":"turn-approval","startedAtMs":%s,"item":{"type":"fileChange","id":"runtime-denial-file-item-sentinel","status":"inProgress","changes":[{"path":"proposal.txt","kind":{"type":"add"},"diff":"proposed\\n"}]}}}\n' "$file_started_at_ms"
      approval_started_at_ms=$((file_started_at_ms + 1))
      printf '{"id":"runtime-denial-file-request-sentinel","method":"item/fileChange/requestApproval","params":{"threadId":"thread-approval","turnId":"turn-approval","itemId":"runtime-denial-file-item-sentinel","startedAtMs":%s,"reason":"runtime-denial-file-reason-sentinel","grantRoot":"/runtime-denial-grant-root-sentinel"}}\n' "$approval_started_at_ms"
      ;;
    *'"id":"runtime-denial-file-request-sentinel","result":{"decision":"decline"}'*)
      printf '%s' ",file" >> "$decision_file"
      printf '{"method":"serverRequest/resolved","params":{"threadId":"thread-approval","requestId":"runtime-denial-file-request-sentinel"}}\n'
      file_completed_at_ms=$((approval_started_at_ms + 1))
      printf '{"method":"item/completed","params":{"threadId":"thread-approval","turnId":"turn-approval","completedAtMs":%s,"item":{"type":"fileChange","id":"runtime-denial-file-item-sentinel","status":"declined","changes":[{"path":"proposal.txt","kind":{"type":"add"},"diff":"proposed\\n"}]}}}\n' "$file_completed_at_ms"
      printf '{"id":101,"method":"item/permissions/requestApproval","params":{"threadId":"thread-approval","turnId":"turn-approval","itemId":"runtime-denial-permissions-item-sentinel","startedAtMs":12,"cwd":"/runtime-denial-cwd-sentinel","permissions":{"write":["/runtime-denial-permission-path-sentinel"]},"environmentId":"runtime-denial-environment-sentinel","reason":"runtime-denial-permissions-reason-sentinel"}}\n'
      ;;
    *'"id":101,"result":{"permissions":{},"scope":"turn"}'*)
      printf '%s' ",permissions" >> "$decision_file"
      printf '{"method":"turn/completed","params":{"threadId":"thread-approval","turn":{"id":"turn-approval","status":"failed","error":{"message":"provider rejected after approval denial"}}}}\n'
      ;;
    *'"method":"shutdown"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      exit 0
      ;;
  esac
done
"#,
    )
    .unwrap();
    let mut permissions = fs::metadata(&executable).unwrap().permissions();
    permissions.set_mode(0o755);
    fs::set_permissions(&executable, permissions).unwrap();
    executable
}

fn mismatched_approval_codex() -> PathBuf {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let directory = std::env::temp_dir().join(format!("aegisy-mismatched-approval-{nonce}"));
    fs::create_dir_all(&directory).unwrap();
    let executable = directory.join("codex-mismatched-approval.sh");
    fs::write(
        &executable,
        r#"#!/bin/sh
if [ "$1" = "--version" ]; then
  echo "codex-cli 0.144.5"
  exit 0
fi
decision_file="$0.decision"
while IFS= read -r line; do
  id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p')
  case "$line" in
    *'"method":"initialize"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      ;;
    *'"method":"thread/start"'*)
      printf '{"id":%s,"result":{"thread":{"id":"thread-mismatch"},"modelProvider":"fixture","model":"fixture","approvalPolicy":"never","approvalsReviewer":"user","sandbox":{"type":"readOnly","networkAccess":false}}}\n' "$id"
      ;;
    *'"method":"turn/start"'*)
      printf '{"id":%s,"result":{"turn":{"id":"turn-active"}}}\n' "$id"
      printf '{"id":99,"method":"item/commandExecution/requestApproval","params":{"threadId":"thread-mismatch","turnId":"turn-stale","itemId":"command-stale","startedAtMs":10,"command":"private command"}}\n'
      ;;
    *'"id":99'*'"code":-32602'*|*'"code":-32602'*'"id":99'*)
      printf '%s' "rejected" > "$decision_file"
      ;;
    *'"method":"shutdown"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      exit 0
      ;;
  esac
done
"#,
    )
    .unwrap();
    let mut permissions = fs::metadata(&executable).unwrap().permissions();
    permissions.set_mode(0o755);
    fs::set_permissions(&executable, permissions).unwrap();
    executable
}

fn denial_budget_codex() -> PathBuf {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let directory = std::env::temp_dir().join(format!("aegisy-denial-budget-{nonce}"));
    fs::create_dir_all(&directory).unwrap();
    let executable = directory.join("codex-denial-budget.sh");
    fs::write(
        &executable,
        r#"#!/bin/sh
if [ "$1" = "--version" ]; then
  echo "codex-cli 0.144.5"
  exit 0
fi
request_index=0
turn_index=0
base_timestamp_ms="$(date +%s)000"
while IFS= read -r line; do
  id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p')
  case "$line" in
    *'"method":"initialize"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      ;;
    *'"method":"thread/start"'*)
      printf '{"id":%s,"result":{"thread":{"id":"thread-denial-budget"},"modelProvider":"fixture","model":"fixture","approvalPolicy":"never","approvalsReviewer":"user","sandbox":{"type":"readOnly","networkAccess":false}}}\n' "$id"
      ;;
    *'"method":"turn/start"'*)
      turn_index=$((turn_index + 1))
      printf '{"id":%s,"result":{"turn":{"id":"turn-denial-budget-%s"}}}\n' "$id" "$turn_index"
      if [ "$turn_index" -eq 1 ]; then
        printf '{"id":700,"method":"item/commandExecution/requestApproval","params":{"threadId":"thread-denial-budget","turnId":"turn-denial-budget-1","itemId":"approval-0","startedAtMs":%s,"command":"private-command-0"}}\n' "$base_timestamp_ms"
      else
        printf '{"method":"turn/completed","params":{"threadId":"thread-denial-budget","turn":{"id":"turn-denial-budget-%s","status":"completed"}}}\n' "$turn_index"
      fi
      ;;
    *'"result":{"decision":"decline"}'*)
      request_index=$((request_index + 1))
      request_id=$((700 + request_index))
      started_at_ms=$((base_timestamp_ms + request_index))
      printf '{"id":%s,"method":"item/commandExecution/requestApproval","params":{"threadId":"thread-denial-budget","turnId":"turn-denial-budget-1","itemId":"approval-%s","startedAtMs":%s,"command":"private-command-%s"}}\n' "$request_id" "$request_index" "$started_at_ms" "$request_index"
      ;;
    *'"code":-32000'*)
      printf '%s' "$request_index" > "$0.preflight-count"
      ;;
    *'"method":"shutdown"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      exit 0
      ;;
  esac
done
"#,
    )
    .unwrap();
    let mut permissions = fs::metadata(&executable).unwrap().permissions();
    permissions.set_mode(0o755);
    fs::set_permissions(&executable, permissions).unwrap();
    executable
}

fn invalid_approval_codex(case: &str) -> PathBuf {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let directory = std::env::temp_dir().join(format!("aegisy-invalid-approval-{nonce}"));
    fs::create_dir_all(&directory).unwrap();
    let executable = directory.join("codex-invalid-approval.sh");
    fs::write(executable.with_extension("sh.case"), case).unwrap();
    fs::write(
        &executable,
        r#"#!/bin/sh
if [ "$1" = "--version" ]; then
  echo "codex-cli 0.144.5"
  exit 0
fi
count_file="$0.instances"
count=$(cat "$count_file" 2>/dev/null || echo 0)
count=$((count + 1))
printf '%s' "$count" > "$count_file"
fixture_case=$(cat "$0.case")
while IFS= read -r line; do
  id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p')
  case "$line" in
    *'"method":"initialize"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      ;;
    *'"method":"thread/start"'*)
      printf '{"id":%s,"result":{"thread":{"id":"thread-invalid-approval"},"modelProvider":"fixture","model":"fixture","approvalPolicy":"never","approvalsReviewer":"user","sandbox":{"type":"readOnly","networkAccess":false}}}\n' "$id"
      ;;
    *'"method":"turn/start"'*)
      printf '{"id":%s,"result":{"turn":{"id":"turn-invalid-approval-%s"}}}\n' "$id" "$count"
      if [ "$count" -eq 1 ]; then
        if [ "$fixture_case" = "missing-id" ]; then
          printf '{"method":"item/commandExecution/requestApproval","params":{"threadId":"thread-invalid-approval","turnId":"turn-invalid-approval-1","itemId":"missing-id","startedAtMs":10}}\n'
        else
          printf '{"id":77,"method":"item/commandExecution/requestApproval","params":[]}\n'
        fi
      else
        printf '{"method":"turn/completed","params":{"threadId":"thread-invalid-approval","turn":{"id":"turn-invalid-approval-%s","status":"completed"}}}\n' "$count"
      fi
      ;;
    *'"method":"shutdown"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      exit 0
      ;;
  esac
done
"#,
    )
    .unwrap();
    let mut permissions = fs::metadata(&executable).unwrap().permissions();
    permissions.set_mode(0o755);
    fs::set_permissions(&executable, permissions).unwrap();
    executable
}

fn denial_write_failure_codex() -> PathBuf {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let directory = std::env::temp_dir().join(format!("aegisy-denial-write-failure-{nonce}"));
    fs::create_dir_all(&directory).unwrap();
    let executable = directory.join("codex-denial-write-failure.sh");
    fs::write(
        &executable,
        r#"#!/bin/sh
if [ "$1" = "--version" ]; then
  echo "codex-cli 0.144.5"
  exit 0
fi
count_file="$0.instances"
count=$(cat "$count_file" 2>/dev/null || echo 0)
count=$((count + 1))
printf '%s' "$count" > "$count_file"
while IFS= read -r line; do
  id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p')
  case "$line" in
    *'"method":"initialize"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      ;;
    *'"method":"thread/start"'*)
      printf '{"id":%s,"result":{"thread":{"id":"thread-denial-write"},"modelProvider":"fixture","model":"fixture","approvalPolicy":"never","approvalsReviewer":"user","sandbox":{"type":"readOnly","networkAccess":false}}}\n' "$id"
      ;;
    *'"method":"turn/start"'*)
      printf '{"id":%s,"result":{"turn":{"id":"turn-denial-write-%s"}}}\n' "$id" "$count"
      if [ "$count" -eq 1 ]; then
        printf '{"id":88,"method":"item/commandExecution/requestApproval","params":{"threadId":"thread-denial-write","turnId":"turn-denial-write-1","itemId":"write-failure","startedAtMs":10}}\n'
        exit 23
      else
        printf '{"method":"turn/completed","params":{"threadId":"thread-denial-write","turn":{"id":"turn-denial-write-%s","status":"completed"}}}\n' "$count"
      fi
      ;;
    *'"method":"shutdown"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      exit 0
      ;;
  esac
done
"#,
    )
    .unwrap();
    let mut permissions = fs::metadata(&executable).unwrap().permissions();
    permissions.set_mode(0o755);
    fs::set_permissions(&executable, permissions).unwrap();
    executable
}

#[test]
fn stdio_turn_metadata_items_survive_durable_restart_replay() {
    let codex = fake_codex();
    let data_root = codex.parent().unwrap().join("workbench-data");
    fs::create_dir_all(&data_root).unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .env("AEGISY_WORKBENCH_DATA_ROOT", &data_root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });

    send(
        &mut stdin,
        &request(
            "metadata-initialize",
            "initialize",
            initialize_params("test"),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "metadata-initialize");
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );
    send(
        &mut stdin,
        &request(
            "metadata-session",
            "session/start",
            json!({ "mode": "chat" }),
        ),
    );
    let session = receive_until(&receiver, |message| message["id"] == "metadata-session");
    let session_id = session["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    send(
        &mut stdin,
        &request(
            "metadata-turn",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "emit metadata",
                "idempotency_key": "metadata-fixture-turn",
                "generation": 1
            }),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "metadata-turn");
    let deadline = std::time::Instant::now() + Duration::from_secs(15);
    let mut events = Vec::new();
    while !events
        .iter()
        .any(|event: &Value| event["params"]["event"] == "turn.completed")
    {
        let message = receiver
            .recv_timeout(deadline.saturating_duration_since(std::time::Instant::now()))
            .expect("sidecar did not complete metadata fixture turn");
        if message["params"]["event"].is_string() {
            events.push(message);
        }
    }
    let usage_events = events
        .iter()
        .filter(|event| event["params"]["event"] == "usage.updated")
        .collect::<Vec<_>>();
    assert_eq!(usage_events.len(), 2);
    let usage_event = usage_events[0];
    assert_eq!(
        usage_event["params"]["item"]["data"]["authority"]["schema_version"],
        "usage-authority/0.1"
    );
    assert_eq!(
        usage_event["params"]["item"]["data"]["authority"]["entries"]
            .as_array()
            .map(Vec::len),
        Some(4)
    );
    assert_eq!(
        usage_event["params"]["item"]["data"]["authority"]["compaction_threshold"]
            ["schema_version"],
        "context-threshold/0.1"
    );
    assert_eq!(
        usage_event["params"]["item"]["data"]["authority"]["compaction_threshold"]["status"],
        "no_action"
    );
    assert_eq!(
        usage_event["params"]["item"]["data"]["authority"]["compaction_threshold"]
            ["automatic_compaction_authority"],
        false
    );
    assert!(events
        .iter()
        .any(|event| event["params"]["event"] == "turn.plan.updated"));
    assert!(events
        .iter()
        .any(|event| event["params"]["event"] == "turn.diff.updated"));

    send(
        &mut stdin,
        &request("metadata-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| message["id"] == "metadata-shutdown");
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();

    let store = WorkbenchStore::open(&data_root).unwrap();
    let completed_trace = store
        .read_turn_trace(&session_id, "turn-fixture")
        .unwrap()
        .expect("metadata completion must persist a terminal trace");
    assert_eq!(completed_trace.trace.schema_version, "turn-trace/0.6");
    let usage_trace_events = completed_trace
        .trace
        .events
        .iter()
        .filter(|event| matches!(event.payload, TracePayload::UsageReport { .. }))
        .collect::<Vec<_>>();
    assert_eq!(usage_trace_events.len(), 1);
    let usage_trace_event = usage_trace_events[0];
    let TracePayload::UsageReport {
        report_identity,
        persisted_item_id,
        scope,
        accounting,
        attempt_attribution,
        retry_attribution,
        report,
        evidence,
        ..
    } = &usage_trace_event.payload
    else {
        unreachable!("filtered UsageReport event")
    };
    assert_eq!(*scope, UsageReportScope::ProviderThread);
    assert_eq!(*accounting, UsageAccounting::AbsoluteSnapshot);
    assert_eq!(*attempt_attribution, UsageAttribution::Unavailable);
    assert_eq!(*retry_attribution, UsageAttribution::Unavailable);
    assert_eq!(usage_trace_event.at_ms, report.as_of_ms);
    assert_eq!(report.metadata_identity().unwrap(), *report_identity);
    assert_eq!(evidence.identity.as_deref(), Some(report_identity.as_str()));
    assert_eq!(evidence.observed_at_ms, Some(report.as_of_ms));
    assert_eq!(
        persisted_item_id,
        usage_events[1]["params"]["item"]["id"]
            .as_str()
            .expect("final live Usage Item ID")
    );
    let report_value = serde_json::to_value(report).unwrap();
    let token = report_value["entries"]
        .as_array()
        .unwrap()
        .iter()
        .find(|entry| entry["metric"] == "token")
        .expect("token Usage authority entry");
    assert_eq!(token["value"]["input_tokens"], 50);
    assert_eq!(token["value"]["output_tokens"], 15);
    assert_eq!(token["value"]["total_tokens"], 65);
    let context = report_value["entries"]
        .as_array()
        .unwrap()
        .iter()
        .find(|entry| entry["metric"] == "context")
        .expect("context Usage authority entry");
    assert_eq!(context["value"]["used_tokens"], 20);
    let persisted_items = store.read_session_items(&session_id, 0, 20).unwrap();
    let persisted_usage = persisted_items
        .iter()
        .find(|item| item.item_id == *persisted_item_id)
        .expect("UsageReport must reference a persisted Timeline Item");
    assert_eq!(persisted_usage.turn_id.as_deref(), Some("turn-fixture"));
    assert_eq!(persisted_usage.item_kind, "usage");
    assert_eq!(persisted_usage.role, "system");
    assert_eq!(persisted_usage.state, "updated");
    let mut persisted_report = persisted_usage.payload["data"]["authority"].clone();
    persisted_report
        .as_object_mut()
        .expect("persisted authority object")
        .remove("compaction_threshold");
    assert_eq!(persisted_report, report_value);
    let usage_position = completed_trace
        .trace
        .events
        .iter()
        .position(|event| matches!(event.payload, TracePayload::UsageReport { .. }))
        .unwrap();
    let terminal_position = completed_trace.trace.events.len() - 1;
    assert!(usage_position < terminal_position);
    drop(store);

    let mut restarted = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .env("AEGISY_WORKBENCH_DATA_ROOT", &data_root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut restarted_stdin = restarted.stdin.take().unwrap();
    let restarted_stdout = restarted.stdout.take().unwrap();
    let (restarted_sender, restarted_receiver) = mpsc::channel();
    let restarted_reader = thread::spawn(move || {
        for line in BufReader::new(restarted_stdout)
            .lines()
            .map_while(Result::ok)
        {
            if let Ok(message) = serde_json::from_str(&line) {
                if restarted_sender.send(message).is_err() {
                    return;
                }
            }
        }
    });
    send(
        &mut restarted_stdin,
        &request(
            "metadata-reinitialize",
            "initialize",
            initialize_params("test"),
        ),
    );
    receive_until(&restarted_receiver, |message| {
        message["id"] == "metadata-reinitialize"
    });
    send(
        &mut restarted_stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );
    send(
        &mut restarted_stdin,
        &request(
            "metadata-read",
            "session/read",
            json!({ "session_id": session_id }),
        ),
    );
    let replay = receive_until(&restarted_receiver, |message| {
        message["id"] == "metadata-read"
    });
    assert_eq!(replay["result"]["runtime"]["replayed"], true);
    let items = replay["result"]["items"].as_array().unwrap();
    assert!(items.iter().any(|item| item["kind"] == "usage"));
    assert!(items.iter().any(|item| item["kind"] == "plan"));
    assert!(items.iter().any(|item| item["kind"] == "diff"));

    send(
        &mut restarted_stdin,
        &request("metadata-final-shutdown", "shutdown", json!({})),
    );
    receive_until(&restarted_receiver, |message| {
        message["id"] == "metadata-final-shutdown"
    });
    drop(restarted_stdin);
    assert!(restarted.wait().unwrap().success());
    restarted_reader.join().unwrap();
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[test]
fn stdio_codex_startup_crash_loop_is_bounded_and_unavailable() {
    let codex = crashing_codex();
    let attempts = codex.with_extension("sh.attempts");
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });

    send(
        &mut stdin,
        &request(
            "startup-initialize",
            "initialize",
            initialize_params("test"),
        ),
    );
    let initialized = receive_until(&receiver, |message| message["id"] == "startup-initialize");
    assert_eq!(initialized["result"]["backend"]["status"], "unavailable");
    assert_eq!(
        initialized["result"]["capabilities"]["stable"][0],
        "runtime.unavailable"
    );
    let attempts_text = fs::read_to_string(&attempts).unwrap();
    assert_eq!(attempts_text.len(), 3);

    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );
    send(
        &mut stdin,
        &request("startup-health", "runtime/health", json!({})),
    );
    let health = receive_until(&receiver, |message| message["id"] == "startup-health");
    assert_eq!(health["result"]["state"], "unavailable");
    assert_eq!(health["result"]["restart_required"], true);
    assert!(!health.to_string().contains("attempts"));

    send(
        &mut stdin,
        &request("startup-degradations", "runtime/degradations", json!({})),
    );
    let degradations = receive_until(&receiver, |message| message["id"] == "startup-degradations");
    assert_eq!(degradations["result"]["backend"]["kind"], "unavailable");
    assert_eq!(degradations["result"]["backend"]["status"], "unavailable");
    assert!(degradations["result"]["degradations"]
        .as_array()
        .unwrap()
        .iter()
        .all(|entry| entry["scope"] != "provider"));

    send(
        &mut stdin,
        &request("startup-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| message["id"] == "startup-shutdown");
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[test]
fn stdio_codex_restart_recovers_after_later_process_exit() {
    let codex = restartable_codex();
    let instances = codex.with_extension("sh.instances");
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });

    send(
        &mut stdin,
        &request(
            "restart-initialize",
            "initialize",
            initialize_params("test"),
        ),
    );
    let initialized = receive_until(&receiver, |message| message["id"] == "restart-initialize");
    assert!(initialized["result"]["capabilities"]["stable"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "runtime.restart"));
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );

    let mut exited = false;
    for index in 0..20 {
        send(
            &mut stdin,
            &request(
                &format!("restart-health-{index}"),
                "runtime/health",
                json!({}),
            ),
        );
        let health = receive_until(&receiver, |message| {
            message["id"] == format!("restart-health-{index}")
        });
        if health["result"]["state"] == "exited" {
            assert_eq!(health["result"]["restart_required"], true);
            exited = true;
            break;
        }
        thread::sleep(Duration::from_millis(25));
    }
    assert!(
        exited,
        "Codex fixture did not exit after the initial handshake"
    );

    send(
        &mut stdin,
        &request(
            "restart-exited-degradations",
            "runtime/degradations",
            json!({}),
        ),
    );
    let exited_degradations = receive_until(&receiver, |message| {
        message["id"] == "restart-exited-degradations"
    });
    assert_eq!(
        exited_degradations["result"]["schema_version"],
        "runtime-degradations/0.2"
    );
    assert_eq!(
        exited_degradations["result"]["backend"]["kind"],
        "unavailable"
    );
    assert_eq!(
        exited_degradations["result"]["backend"]["status"],
        "unavailable"
    );
    assert!(exited_degradations["result"]["degradations"]
        .as_array()
        .unwrap()
        .iter()
        .all(|entry| entry["scope"] != "provider"));

    send(
        &mut stdin,
        &request("restart", "runtime/restart", json!({})),
    );
    let restarted = receive_until(&receiver, |message| message["id"] == "restart");
    assert_eq!(restarted["result"]["status"], "restarted");
    assert_eq!(restarted["result"]["health"]["state"], "running");

    send(
        &mut stdin,
        &request(
            "restart-running-degradations",
            "runtime/degradations",
            json!({}),
        ),
    );
    let running_degradations = receive_until(&receiver, |message| {
        message["id"] == "restart-running-degradations"
    });
    assert_eq!(running_degradations["result"]["backend"]["kind"], "codex");
    assert_eq!(running_degradations["result"]["backend"]["status"], "ready");

    send(
        &mut stdin,
        &request("restart-running", "runtime/restart", json!({})),
    );
    let rejected = receive_until(&receiver, |message| message["id"] == "restart-running");
    assert_eq!(rejected["error"]["code"], -32082);

    assert_eq!(fs::read_to_string(&instances).unwrap(), "2");
    send(
        &mut stdin,
        &request("restart-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| message["id"] == "restart-shutdown");
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[test]
fn stdio_codex_transport_failure_reconnects_and_preserves_session_binding() {
    let codex = reconnectable_codex();
    let instances = codex.with_extension("sh.instances");
    let data_root = codex.parent().expect("fixture directory").join("workbench");
    fs::create_dir_all(&data_root).unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .env("AEGISY_WORKBENCH_DATA_ROOT", &data_root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });

    send(
        &mut stdin,
        &request(
            "reconnect-initialize",
            "initialize",
            initialize_params("test"),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "reconnect-initialize");
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );
    send(
        &mut stdin,
        &request(
            "reconnect-session",
            "session/start",
            json!({ "mode": "chat" }),
        ),
    );
    let session = receive_until(&receiver, |message| message["id"] == "reconnect-session");
    let session_id = session["result"]["session"]["id"]
        .as_str()
        .unwrap_or_else(|| panic!("reconnect session did not start: {session}"))
        .to_owned();

    send(
        &mut stdin,
        &request(
            "reconnect-turn-fails",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "first attempt",
                "idempotency_key": "reconnect-first",
                "generation": 1
            }),
        ),
    );
    let failed = receive_until(&receiver, |message| {
        message["method"] == "event"
            && message["params"]["event"] == "turn.failed"
            && message["params"]["item"]["data"]["schema_version"] == "runtime-error/0.1"
    });
    assert_eq!(
        failed["params"]["item"]["data"]["class"], "transport",
        "failed={failed}"
    );
    assert_eq!(failed["params"]["item"]["data"]["retryable"], true);
    assert!(!failed.to_string().contains("Bearer"));

    send(
        &mut stdin,
        &request("reconnect-health", "runtime/health", json!({})),
    );
    let health = receive_until(&receiver, |message| message["id"] == "reconnect-health");
    assert_eq!(health["result"]["state"], "exited");
    assert_eq!(health["result"]["restart_required"], true);
    send(
        &mut stdin,
        &request("reconnect-restart", "runtime/restart", json!({})),
    );
    let restarted = receive_until(&receiver, |message| message["id"] == "reconnect-restart");
    assert_eq!(restarted["result"]["status"], "restarted");

    send(
        &mut stdin,
        &request(
            "reconnect-turn-recovers",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "retry after reconnect",
                "idempotency_key": "reconnect-second",
                "generation": 1
            }),
        ),
    );
    let completed = receive_until(&receiver, |message| {
        message["method"] == "event"
            && message["params"]["event"] == "turn.completed"
            && message["params"]["turn_id"] == "turn-reconnect-2"
    });
    assert_eq!(completed["params"]["turn_id"], "turn-reconnect-2");
    assert_eq!(fs::read_to_string(&instances).unwrap(), "2");

    send(
        &mut stdin,
        &request("reconnect-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| message["id"] == "reconnect-shutdown");
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();

    let store = WorkbenchStore::open(&data_root).unwrap();
    assert_eq!(store.load_turn("turn-reconnect-1").unwrap().state, "failed");
    assert_eq!(
        store.load_turn("turn-reconnect-2").unwrap().state,
        "completed"
    );
    let failed_trace = store
        .read_turn_trace(&session_id, "turn-reconnect-1")
        .unwrap()
        .expect("transport failure must persist a terminal trace");
    assert_eq!(failed_trace.state, "failed");
    assert_atomic_failed_terminal(&store, &session_id, "turn-reconnect-1", "transport");
    assert_eq!(failed_trace.trace.binding.session_id, session_id);
    assert_eq!(failed_trace.trace.binding.turn_id, "turn-reconnect-1");
    assert_eq!(failed_trace.trace.binding.project_id, None);
    assert!(failed_trace.trace.binding.environment_identity.is_some());
    assert!(matches!(
        failed_trace.trace.events.last().map(|event| &event.payload),
        Some(TracePayload::Terminal {
            state: TraceTerminalState::Failed,
            ..
        })
    ));
    assert!(failed_trace
        .trace
        .events
        .iter()
        .any(|event| matches!(event.payload, TracePayload::Runtime { .. })));
    assert!(failed_trace
        .trace
        .events
        .iter()
        .any(|event| matches!(event.payload, TracePayload::Model { .. })));
    assert!(failed_trace
        .trace
        .events
        .iter()
        .any(|event| matches!(event.payload, TracePayload::Context { .. })));
    assert!(failed_trace
        .trace
        .events
        .iter()
        .any(|event| matches!(event.payload, TracePayload::Error { .. })));
    assert!(failed_trace
        .trace
        .events
        .iter()
        .all(|event| !matches!(event.payload, TracePayload::UsageReport { .. })));
    let runtime_error = failed_trace
        .trace
        .events
        .iter()
        .find_map(|event| match &event.payload {
            TracePayload::Error {
                stable_class,
                source_class,
                retryable,
                evidence,
                ..
            } => Some((
                *stable_class,
                source_class.as_str(),
                *retryable,
                evidence.source,
            )),
            _ => None,
        })
        .expect("transport failure trace must contain error metadata");
    assert_eq!(runtime_error.0, TraceErrorClass::Transport);
    assert_eq!(runtime_error.1, "transport");
    assert!(runtime_error.2);
    assert_eq!(runtime_error.3, TraceEvidenceSource::Runtime);
    assert_eq!(
        failed_trace
            .trace
            .events
            .iter()
            .filter(|event| matches!(event.payload, TracePayload::Terminal { .. }))
            .count(),
        1
    );
    let serialized_trace = serde_json::to_string(&failed_trace).unwrap();
    for forbidden in [
        "first attempt",
        "partial",
        "Bearer",
        "codex-reconnect-fixture",
    ] {
        assert!(!serialized_trace.contains(forbidden), "found {forbidden}");
    }
    let completed_trace = store
        .read_turn_trace(&session_id, "turn-reconnect-2")
        .unwrap()
        .expect("recovered completion must persist a terminal trace");
    assert_eq!(completed_trace.state, "completed");
    assert_eq!(completed_trace.trace.schema_version, "turn-trace/0.6");
    assert_eq!(completed_trace.trace.binding.session_id, session_id);
    assert_eq!(completed_trace.trace.binding.turn_id, "turn-reconnect-2");
    assert!(completed_trace
        .trace
        .events
        .iter()
        .all(|event| !matches!(event.payload, TracePayload::UsageReport { .. })));
    let intent_identity = completed_trace
        .trace
        .events
        .iter()
        .find_map(|event| match &event.payload {
            TracePayload::Intent {
                session_mode: TraceSessionMode::Chat,
                turn_kind: TraceTurnKind::Conversation,
                access: TraceTurnAccess::NonMutating,
                intent_identity,
                ..
            } => Some(intent_identity.as_str()),
            _ => None,
        })
        .expect("completed Chat trace must contain its immutable intent");
    let TracePayload::Terminal {
        state: TraceTerminalState::Completed,
        evidence,
        ..
    } = &completed_trace
        .trace
        .events
        .last()
        .expect("completed trace must end with terminal evidence")
        .payload
    else {
        panic!("completed trace must end with a completed terminal event")
    };
    let completion = evidence
        .completion
        .as_ref()
        .expect("completed trace must classify every completion domain");
    assert_eq!(completion.intent_identity, intent_identity);
    assert!(matches!(
        completion.workspace_change,
        CompletionDomain::NotApplicable { .. }
    ));
    assert!(matches!(
        completion.git_change,
        CompletionDomain::NotApplicable { .. }
    ));
    assert!(matches!(
        completion.verification,
        CompletionDomain::NotApplicable { .. }
    ));
    assert!(completed_trace.trace.events.iter().all(|event| !matches!(
        event.payload,
        TracePayload::Tool { .. } | TracePayload::Change { .. } | TracePayload::Test { .. }
    )));
    drop(store);
    let reopened = WorkbenchStore::open(&data_root).unwrap();
    assert_eq!(
        reopened
            .read_turn_trace(&session_id, "turn-reconnect-1")
            .unwrap()
            .unwrap(),
        failed_trace
    );
    assert_eq!(
        reopened
            .read_turn_trace(&session_id, "turn-reconnect-2")
            .unwrap()
            .unwrap(),
        completed_trace
    );
    drop(reopened);
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[test]
fn stdio_codex_provider_failure_preserves_content_free_upstream_classification() {
    let codex = provider_failure_codex();
    let secret = "ghp_123456789012345678901234567890";
    let data_root = codex.parent().expect("fixture directory").join("workbench");
    fs::create_dir_all(&data_root).unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .env("AEGISY_WORKBENCH_DATA_ROOT", &data_root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });

    send(
        &mut stdin,
        &request(
            "provider-failure-initialize",
            "initialize",
            initialize_params("test"),
        ),
    );
    receive_until(&receiver, |message| {
        message["id"] == "provider-failure-initialize"
    });
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );
    send(
        &mut stdin,
        &request(
            "provider-failure-session",
            "session/start",
            json!({ "mode": "chat" }),
        ),
    );
    let session = receive_until(&receiver, |message| {
        message["id"] == "provider-failure-session"
    });
    let session_id = session["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    send(
        &mut stdin,
        &request(
            "provider-failure-turn",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "provider failure",
                "idempotency_key": "provider-failure-turn",
                "generation": 1,
                "context": [{
                    "id": "excluded-context",
                    "kind": "selection",
                    "label": "excluded",
                    "origin": "test",
                    "exclusion_reason": "user-excluded"
                }]
            }),
        ),
    );
    let observed = receive_until(&receiver, |message| {
        message["method"] == "event" && message["params"]["event"] == "turn.error-observed"
    });
    assert_eq!(
        observed["params"]["item"]["content"],
        "Codex reported a non-terminal turn error"
    );
    assert_eq!(observed["params"]["item"]["data"]["terminal"], false);
    assert_eq!(observed["params"]["item"]["data"]["will_retry"], true);
    assert_eq!(observed["params"]["item"]["data"]["class"], "transport");
    assert!(!observed.to_string().contains(secret));
    assert!(!observed
        .to_string()
        .contains("private provider response body"));
    let failed = receive_until(&receiver, |message| {
        message["method"] == "event"
            && message["params"]["event"] == "turn.failed"
            && message["params"]["item"]["data"]["schema_version"] == "runtime-error/0.1"
    });
    assert_eq!(failed["params"]["item"]["data"]["class"], "transport");
    assert_eq!(failed["params"]["item"]["data"]["retryable"], true);
    assert_eq!(
        failed["params"]["item"]["data"]["provider_error"]["schema_version"],
        "provider-error/0.1"
    );
    assert_eq!(
        failed["params"]["item"]["data"]["provider_error"]["kind"],
        "response-stream-disconnected"
    );
    assert_eq!(
        failed["params"]["item"]["data"]["provider_error"]["http_status"],
        502
    );
    assert_eq!(
        failed["params"]["item"]["data"]["provider_error"]["response_body_included"],
        false
    );
    assert!(!failed.to_string().contains(secret));
    assert!(!failed
        .to_string()
        .contains("private provider response body"));
    assert_eq!(
        failed["params"]["item"]["content"],
        "Upstream provider request failed"
    );

    send(
        &mut stdin,
        &request("provider-failure-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| {
        message["id"] == "provider-failure-shutdown"
    });
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();

    let store = WorkbenchStore::open(&data_root).unwrap();
    let failed_trace = store
        .read_turn_trace(&session_id, "turn-provider-failure")
        .unwrap()
        .expect("provider failure must persist a terminal trace");
    assert_eq!(
        store.load_turn("turn-provider-failure").unwrap().state,
        "failed"
    );
    assert_eq!(failed_trace.trace.binding.session_id, session_id);
    assert_eq!(failed_trace.trace.binding.turn_id, "turn-provider-failure");
    assert_eq!(failed_trace.trace.binding.project_id, None);
    assert!(failed_trace.trace.binding.environment_identity.is_some());
    let error = failed_trace
        .trace
        .events
        .iter()
        .find_map(|event| match &event.payload {
            TracePayload::Error {
                stable_class,
                source_class,
                retryable,
                ..
            } => Some((*stable_class, source_class.as_str(), *retryable)),
            _ => None,
        })
        .expect("provider failure trace must contain error metadata");
    assert_eq!(error.0, TraceErrorClass::Transport);
    assert_eq!(error.1, "response-stream-disconnected");
    assert!(error.2);
    let usage_position = failed_trace
        .trace
        .events
        .iter()
        .position(|event| matches!(event.payload, TracePayload::UsageReport { .. }))
        .expect("provider failure trace must retain observed Usage");
    let error_position = failed_trace
        .trace
        .events
        .iter()
        .position(|event| matches!(event.payload, TracePayload::Error { .. }))
        .expect("provider failure trace error position");
    assert!(usage_position < error_position);
    assert!(error_position < failed_trace.trace.events.len() - 1);
    let context = failed_trace
        .trace
        .events
        .iter()
        .find_map(|event| match &event.payload {
            TracePayload::Context {
                item_count,
                included_items,
                excluded_items,
                ..
            } => Some((*item_count, *included_items, *excluded_items)),
            _ => None,
        })
        .expect("provider failure trace must contain context metadata");
    assert_eq!(context, (1, 0, 1));
    let model_evidence_source = failed_trace
        .trace
        .events
        .iter()
        .find_map(|event| match &event.payload {
            TracePayload::Model { evidence, .. } => Some(evidence.source),
            _ => None,
        })
        .expect("provider-bound turn must contain model binding metadata");
    assert_eq!(model_evidence_source, TraceEvidenceSource::Runtime);
    assert!(matches!(
        failed_trace.trace.events.last().map(|event| &event.payload),
        Some(TracePayload::Terminal {
            state: TraceTerminalState::Failed,
            ..
        })
    ));
    assert_eq!(
        failed_trace
            .trace
            .events
            .iter()
            .filter(|event| matches!(event.payload, TracePayload::Terminal { .. }))
            .count(),
        1
    );
    let serialized_trace = serde_json::to_string(&failed_trace).unwrap();
    for forbidden in [
        secret,
        "private provider response body",
        "stream disconnected before completion",
        "provider failure",
        "codex-provider-failure-fixture",
    ] {
        assert!(!serialized_trace.contains(forbidden), "found {forbidden}");
    }
    drop(store);
    let reopened = WorkbenchStore::open(&data_root).unwrap();
    assert_eq!(
        reopened
            .read_turn_trace(&session_id, "turn-provider-failure")
            .unwrap()
            .unwrap(),
        failed_trace
    );
    drop(reopened);
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[test]
fn stdio_codex_approval_request_is_declined_without_execution_authority() {
    let codex = approval_denial_codex();
    let decision = codex.with_extension("sh.decision");
    let data_root = codex.parent().unwrap().join("approval-workbench");
    let project_root = codex.parent().unwrap().join("approval-project");
    fs::create_dir_all(&data_root).unwrap();
    fs::create_dir_all(&project_root).unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .env("AEGISY_WORKBENCH_DATA_ROOT", &data_root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });

    send(
        &mut stdin,
        &request(
            "approval-initialize",
            "initialize",
            initialize_params("test"),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "approval-initialize");
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );
    send(
        &mut stdin,
        &request(
            "approval-project",
            "project/open",
            json!({ "root": project_root }),
        ),
    );
    let project = receive_until(&receiver, |message| message["id"] == "approval-project");
    let project_id = project["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    send(
        &mut stdin,
        &request(
            "approval-session",
            "session/start",
            json!({ "mode": "work", "project_id": project_id }),
        ),
    );
    let session = receive_until(&receiver, |message| message["id"] == "approval-session");
    let session_id = session["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    send(
        &mut stdin,
        &request(
            "approval-turn",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "request a destructive command",
                "idempotency_key": "approval-turn",
                "generation": 1
            }),
        ),
    );
    let failed = receive_until(&receiver, |message| {
        message["method"] == "event"
            && message["params"]["event"] == "turn.failed"
            && message["params"]["item"]["data"]["schema_version"] == "runtime-error/0.1"
    });
    assert_eq!(failed["params"]["item"]["data"]["class"], "provider");
    assert_eq!(failed["params"]["item"]["data"]["retryable"], false);
    assert_eq!(
        fs::read_to_string(&decision).unwrap(),
        "command,file,permissions"
    );
    assert!(!failed
        .to_string()
        .contains("runtime-denial-command-sentinel"));

    send(
        &mut stdin,
        &request(
            "approval-session-read",
            "session/read",
            json!({"session_id": &session_id, "limit": 200}),
        ),
    );
    let replay = receive_until(&receiver, |message| {
        message["id"] == "approval-session-read"
    });
    let replay_change = replay["result"]["items"]
        .as_array()
        .unwrap()
        .iter()
        .find(|item| item["kind"] == "file-change")
        .expect("session/read must retain the durable file-change Item");
    assert_eq!(replay_change["turn_id"], "turn-approval");
    assert_eq!(replay_change["data"]["turn_id"], "turn-approval");

    send(
        &mut stdin,
        &request("approval-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| message["id"] == "approval-shutdown");
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();

    let store = WorkbenchStore::open(&data_root).unwrap();
    let proposal_event = store
        .read_session_events(&session_id, 0, 200)
        .unwrap()
        .into_iter()
        .find(|event| event.event_kind == "workspace-edit.proposal-recorded")
        .expect("file approval must persist an immutable Proposal before decline");
    let proposal_id = proposal_event.payload["proposal_id"]
        .as_str()
        .expect("proposal event must identify its immutable Proposal");
    let stored_proposal = store
        .read_workspace_edit_proposal(proposal_id)
        .expect("immutable Proposal must be readable after sidecar shutdown");
    let timeline_reference = stored_proposal
        .timeline_reference
        .as_ref()
        .expect("new Proposal must retain its durable Change Timeline reference");
    assert_eq!(
        timeline_reference.schema_version,
        "workspace-edit-proposal-reference/0.1"
    );
    assert_eq!(timeline_reference.proposal_id, proposal_id);
    assert!(!timeline_reference.file_mutation_authority);
    assert!(!timeline_reference.approval_recorded);
    assert!(!timeline_reference.apply_available);
    let timeline = store
        .sync_public_timeline(&session_id, 0, None, 20)
        .unwrap();
    let change = timeline
        .events
        .iter()
        .find(|event| {
            event.event == "item.completed"
                && event
                    .item
                    .as_ref()
                    .is_some_and(|item| item.kind == "file-change")
        })
        .expect("Proposal must be projected into the durable public Timeline");
    assert!(change.sequence > 0);
    assert_eq!(
        change.item.as_ref().unwrap().data.as_ref().unwrap()["reference_id"],
        timeline_reference.reference_id
    );
    assert_eq!(
        change.item.as_ref().unwrap().data.as_ref().unwrap()["proposal_id"],
        proposal_id
    );
    assert_eq!(stored_proposal.proposal.session_id, session_id);
    assert_eq!(stored_proposal.proposal.turn_id, "turn-approval");
    assert_eq!(
        stored_proposal.proposal.provider.as_deref(),
        Some("fixture")
    );
    assert_eq!(
        stored_proposal.proposal.provider_thread_id,
        "thread-approval"
    );
    assert_eq!(
        stored_proposal.proposal.provider_item_id,
        "runtime-denial-file-item-sentinel"
    );
    assert_eq!(stored_proposal.proposal.project_id, project_id);
    assert_eq!(stored_proposal.proposal.root_id, "root-1");
    assert_eq!(
        stored_proposal.proposal.created_at_ms,
        stored_proposal.proposal.approval_started_at_ms
    );
    assert!(!stored_proposal.proposal.file_mutation_authority);
    assert!(!stored_proposal.proposal.approval_recorded);
    assert!(!stored_proposal.proposal.apply_available);
    assert_eq!(stored_proposal.proposal.operations.len(), 1);
    assert!(!project_root.join("proposal.txt").exists());
    let runtime_binding = store.load_session_runtime_binding(&session_id).unwrap();
    assert_eq!(runtime_binding.adapter, "codex-app-server");
    assert_eq!(runtime_binding.adapter_version, "codex-cli 0.144.5");
    assert_eq!(
        runtime_binding.backend_session_id.as_deref(),
        Some("thread-approval")
    );
    assert_eq!(runtime_binding.provider.as_deref(), Some("fixture"));
    assert_eq!(runtime_binding.permission_profile, "read-only");
    let trace = store
        .read_turn_trace(&session_id, "turn-approval")
        .unwrap()
        .expect("approval-request failure must persist a terminal Trace");
    assert_eq!(trace.trace.schema_version, "turn-trace/0.6");
    let policy_events = trace
        .trace
        .events
        .iter()
        .filter(|event| matches!(event.payload, TracePayload::RuntimeApprovalPolicy { .. }))
        .collect::<Vec<_>>();
    assert_eq!(policy_events.len(), 1);
    let TracePayload::RuntimeApprovalPolicy {
        runtime_identity: policy_runtime_identity,
        adapter_identity: policy_adapter_identity,
        runtime_version: policy_runtime_version,
        provider_thread_identity: policy_provider_thread_identity,
        policy_authority_identity,
        user_decision_observed,
        execution_authority,
        evidence,
        ..
    } = &policy_events[0].payload
    else {
        unreachable!("policy event was selected above")
    };
    assert!(!user_decision_observed);
    assert!(!execution_authority);
    assert_eq!(evidence.source, TraceEvidenceSource::Runtime);
    let denial_events = trace
        .trace
        .events
        .iter()
        .filter(|event| matches!(event.payload, TracePayload::RuntimeDenial { .. }))
        .collect::<Vec<_>>();
    assert_eq!(denial_events.len(), 3);
    let TracePayload::RuntimeDenial {
        request_kind: RuntimeDenialRequestKind::CommandExecution,
        request_identity,
        denial_identity,
        runtime_identity,
        adapter_identity,
        runtime_version,
        provider_thread_identity,
        policy_authority_identity: denial_policy_authority_identity,
        response_state: RuntimeDenialResponseState::DeclineFlushed,
        attribution: RuntimeDenialAttribution::RuntimePolicy,
        user_decision_observed: false,
        approval_authority_observed: false,
        execution_authority: false,
        evidence: denial_evidence,
        ..
    } = &denial_events[0].payload
    else {
        panic!("approval request must produce one exact Runtime denial observation")
    };
    assert!(request_identity.starts_with("sha256:"));
    assert!(denial_identity.starts_with("sha256:"));
    assert_eq!(runtime_identity, policy_runtime_identity);
    assert_eq!(adapter_identity, policy_adapter_identity);
    assert_eq!(runtime_version, policy_runtime_version);
    assert_eq!(provider_thread_identity, policy_provider_thread_identity);
    assert_eq!(denial_policy_authority_identity, policy_authority_identity);
    assert_eq!(denial_evidence.source, TraceEvidenceSource::Runtime);
    assert_eq!(
        denial_evidence.identity.as_deref(),
        Some(denial_identity.as_str())
    );
    assert_eq!(denial_evidence.observed_at_ms, Some(denial_events[0].at_ms));
    assert!(matches!(
        &denial_events[1].payload,
        TracePayload::RuntimeDenial {
            request_kind: RuntimeDenialRequestKind::FileChange,
            response_state: RuntimeDenialResponseState::DeclineFlushed,
            ..
        }
    ));
    assert!(matches!(
        &denial_events[2].payload,
        TracePayload::RuntimeDenial {
            request_kind: RuntimeDenialRequestKind::Permissions,
            response_state: RuntimeDenialResponseState::DeclineFlushed,
            ..
        }
    ));
    assert!(trace
        .trace
        .events
        .iter()
        .all(|event| !matches!(event.payload, TracePayload::Approval { .. })));
    let request_sentinels = [
        "runtime-denial-request-id-sentinel",
        "runtime-denial-command-item-sentinel",
        "runtime-denial-command-sentinel",
        "runtime-denial-command-path-sentinel",
        "4815162342",
        "runtime-denial-credential-sentinel",
        "runtime-denial-user-id-sentinel",
        "runtime-denial-file-request-sentinel",
        "runtime-denial-file-item-sentinel",
        "runtime-denial-file-reason-sentinel",
        "runtime-denial-grant-root-sentinel",
        "runtime-denial-permissions-item-sentinel",
        "runtime-denial-cwd-sentinel",
        "runtime-denial-permission-path-sentinel",
        "runtime-denial-environment-sentinel",
        "runtime-denial-permissions-reason-sentinel",
    ];
    let serialized = serde_json::to_string(&trace).unwrap();
    for sentinel in request_sentinels {
        assert!(
            !serialized.contains(sentinel),
            "Runtime denial Trace persisted Provider request sentinel {sentinel}"
        );
    }
    assert!(!serialized.contains("request a destructive command"));
    drop(store);

    for entry in fs::read_dir(&data_root).unwrap() {
        let path = entry.unwrap().path();
        if !path.is_file() {
            continue;
        }
        let bytes = fs::read(&path).unwrap();
        for sentinel in request_sentinels {
            if sentinel == "runtime-denial-file-item-sentinel" {
                continue;
            }
            assert!(
                !bytes
                    .windows(sentinel.len())
                    .any(|window| window == sentinel.as_bytes()),
                "Workbench file persisted Provider request sentinel {sentinel}"
            );
        }
    }

    let reopened = WorkbenchStore::open(&data_root).unwrap();
    let replayed = reopened
        .read_turn_trace(&session_id, "turn-approval")
        .unwrap()
        .expect("Runtime denial Trace must replay after restart");
    let replayed = serde_json::to_string(&replayed).unwrap();
    for sentinel in request_sentinels {
        assert!(
            !replayed.contains(sentinel),
            "replayed Runtime denial Trace exposed Provider request sentinel {sentinel}"
        );
    }
    drop(reopened);
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[test]
fn stdio_codex_approval_request_must_match_the_active_turn() {
    let codex = mismatched_approval_codex();
    let decision = codex.with_extension("sh.decision");
    let data_root = codex
        .parent()
        .unwrap()
        .join("mismatched-approval-workbench");
    fs::create_dir_all(&data_root).unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .env("AEGISY_WORKBENCH_DATA_ROOT", &data_root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });

    send(
        &mut stdin,
        &request(
            "mismatch-initialize",
            "initialize",
            initialize_params("test"),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "mismatch-initialize");
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );
    send(
        &mut stdin,
        &request(
            "mismatch-session",
            "session/start",
            json!({ "mode": "chat" }),
        ),
    );
    let session = receive_until(&receiver, |message| message["id"] == "mismatch-session");
    let session_id = session["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    send(
        &mut stdin,
        &request(
            "mismatch-turn",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "inspect only",
                "idempotency_key": "mismatch-turn",
                "generation": 1
            }),
        ),
    );
    receive_until(&receiver, |message| {
        message["method"] == "event" && message["params"]["event"] == "turn.failed"
    });
    for _ in 0..100 {
        if decision.exists() {
            break;
        }
        thread::sleep(Duration::from_millis(10));
    }
    assert_eq!(fs::read_to_string(&decision).unwrap(), "rejected");

    send(
        &mut stdin,
        &request("mismatch-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| message["id"] == "mismatch-shutdown");
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();

    let store = WorkbenchStore::open(&data_root).unwrap();
    let trace = store
        .read_turn_trace(&session_id, "turn-active")
        .unwrap()
        .expect("mismatched approval request must fail with a durable Trace");
    assert_eq!(trace.trace.schema_version, "turn-trace/0.6");
    assert!(trace.trace.events.iter().all(|event| !matches!(
        event.payload,
        TracePayload::RuntimeDenial { .. } | TracePayload::Approval { .. }
    )));
    assert!(!serde_json::to_string(&trace)
        .unwrap()
        .contains("private command"));
    drop(store);
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[test]
fn stdio_runtime_denial_budget_error_resolves_request_and_reuses_backend() {
    let codex = denial_budget_codex();
    let preflight_count = codex.with_extension("sh.preflight-count");
    let data_root = codex.parent().unwrap().join("denial-budget-workbench");
    let (mut child, mut stdin, receiver, reader, session_id) =
        start_codex_runtime(&codex, &data_root, "denial-budget");

    send(
        &mut stdin,
        &request(
            "denial-budget-turn",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "exercise bounded denial metadata",
                "idempotency_key": "denial-budget-first",
                "generation": 1
            }),
        ),
    );
    let failed = receive_until(&receiver, |message| {
        message["method"] == "event"
            && message["params"]["event"] == "turn.failed"
            && message["params"]["turn_id"] == "turn-denial-budget-1"
    });
    assert_eq!(failed["params"]["item"]["data"]["class"], "budget");
    for _ in 0..100 {
        if preflight_count.exists() {
            break;
        }
        thread::sleep(Duration::from_millis(10));
    }
    let accepted_denials = fs::read_to_string(&preflight_count)
        .unwrap()
        .parse::<usize>()
        .unwrap();
    assert!(accepted_denials > 0);

    send(
        &mut stdin,
        &request("denial-budget-health", "runtime/health", json!({})),
    );
    let health = receive_until(&receiver, |message| message["id"] == "denial-budget-health");
    assert_eq!(health["result"]["state"], "running");
    assert_eq!(health["result"]["restart_required"], false);
    send(
        &mut stdin,
        &request(
            "denial-budget-next-turn",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "verify the same backend remains usable",
                "idempotency_key": "denial-budget-second",
                "generation": 1
            }),
        ),
    );
    receive_until(&receiver, |message| {
        message["method"] == "event"
            && message["params"]["event"] == "turn.completed"
            && message["params"]["turn_id"] == "turn-denial-budget-2"
    });

    send(
        &mut stdin,
        &request("denial-budget-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| {
        message["id"] == "denial-budget-shutdown"
    });
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();

    let store = WorkbenchStore::open(&data_root).unwrap();
    let trace = store
        .read_turn_trace(&session_id, "turn-denial-budget-1")
        .unwrap()
        .expect("budget failure must persist a terminal Trace");
    let denial_count = trace
        .trace
        .events
        .iter()
        .filter(|event| matches!(event.payload, TracePayload::RuntimeDenial { .. }))
        .count();
    assert_eq!(denial_count, accepted_denials);
    assert!(trace.trace.events.iter().any(|event| matches!(
        event.payload,
        TracePayload::Error {
            stable_class: TraceErrorClass::Budget,
            ..
        }
    )));
    drop(store);
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[test]
fn stdio_invalid_runtime_denial_requests_discard_backend_and_restart_cleanly() {
    for fixture_case in ["missing-id", "malformed-params"] {
        let codex = invalid_approval_codex(fixture_case);
        let instances = codex.with_extension("sh.instances");
        let data_root = codex.parent().unwrap().join("invalid-approval-workbench");
        let (mut child, mut stdin, receiver, reader, session_id) =
            start_codex_runtime(&codex, &data_root, fixture_case);

        send(
            &mut stdin,
            &request(
                &format!("{fixture_case}-turn"),
                "turn/start",
                json!({
                    "session_id": session_id,
                    "input": "emit malformed approval request",
                    "idempotency_key": format!("{fixture_case}-first"),
                    "generation": 1
                }),
            ),
        );
        receive_until(&receiver, |message| {
            message["method"] == "event"
                && message["params"]["event"] == "turn.failed"
                && message["params"]["turn_id"] == "turn-invalid-approval-1"
        });
        let health_id = format!("{fixture_case}-health");
        send(
            &mut stdin,
            &request(&health_id, "runtime/health", json!({})),
        );
        let health = receive_until(&receiver, |message| message["id"] == health_id);
        assert_eq!(health["result"]["state"], "unavailable");
        assert_eq!(health["result"]["restart_required"], true);
        let restart_id = format!("{fixture_case}-restart");
        send(
            &mut stdin,
            &request(&restart_id, "runtime/restart", json!({})),
        );
        let restarted = receive_until(&receiver, |message| message["id"] == restart_id);
        assert_eq!(restarted["result"]["status"], "restarted");
        send(
            &mut stdin,
            &request(
                &format!("{fixture_case}-next-turn"),
                "turn/start",
                json!({
                    "session_id": session_id,
                    "input": "verify restart",
                    "idempotency_key": format!("{fixture_case}-second"),
                    "generation": 1
                }),
            ),
        );
        receive_until(&receiver, |message| {
            message["method"] == "event"
                && message["params"]["event"] == "turn.completed"
                && message["params"]["turn_id"] == "turn-invalid-approval-2"
        });
        assert_eq!(fs::read_to_string(&instances).unwrap(), "2");

        let shutdown_id = format!("{fixture_case}-shutdown");
        send(&mut stdin, &request(&shutdown_id, "shutdown", json!({})));
        receive_until(&receiver, |message| message["id"] == shutdown_id);
        drop(stdin);
        assert!(child.wait().unwrap().success());
        reader.join().unwrap();

        let store = WorkbenchStore::open(&data_root).unwrap();
        let trace = store
            .read_turn_trace(&session_id, "turn-invalid-approval-1")
            .unwrap()
            .expect("invalid approval request must persist a failed Trace");
        assert!(trace.trace.events.iter().all(|event| !matches!(
            event.payload,
            TracePayload::RuntimeDenial { .. } | TracePayload::Approval { .. }
        )));
        drop(store);
        let _ = fs::remove_dir_all(codex.parent().unwrap());
    }
}

#[test]
fn stdio_runtime_denial_write_failure_discards_backend_until_restart() {
    let codex = denial_write_failure_codex();
    let instances = codex.with_extension("sh.instances");
    let data_root = codex.parent().unwrap().join("denial-write-workbench");
    let (mut child, mut stdin, receiver, reader, session_id) =
        start_codex_runtime(&codex, &data_root, "denial-write");

    send(
        &mut stdin,
        &request(
            "denial-write-turn",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "force denial response write failure",
                "idempotency_key": "denial-write-first",
                "generation": 1
            }),
        ),
    );
    receive_until(&receiver, |message| {
        message["method"] == "event"
            && message["params"]["event"] == "turn.failed"
            && message["params"]["turn_id"] == "turn-denial-write-1"
    });
    send(
        &mut stdin,
        &request("denial-write-health", "runtime/health", json!({})),
    );
    let health = receive_until(&receiver, |message| message["id"] == "denial-write-health");
    assert_eq!(health["result"]["state"], "unavailable");
    assert_eq!(health["result"]["restart_required"], true);
    send(
        &mut stdin,
        &request("denial-write-restart", "runtime/restart", json!({})),
    );
    let restarted = receive_until(&receiver, |message| message["id"] == "denial-write-restart");
    assert_eq!(restarted["result"]["status"], "restarted");
    send(
        &mut stdin,
        &request(
            "denial-write-next-turn",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "verify restarted backend",
                "idempotency_key": "denial-write-second",
                "generation": 1
            }),
        ),
    );
    receive_until(&receiver, |message| {
        message["method"] == "event"
            && message["params"]["event"] == "turn.completed"
            && message["params"]["turn_id"] == "turn-denial-write-2"
    });
    assert_eq!(fs::read_to_string(&instances).unwrap(), "2");

    send(
        &mut stdin,
        &request("denial-write-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| {
        message["id"] == "denial-write-shutdown"
    });
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();

    let store = WorkbenchStore::open(&data_root).unwrap();
    let trace = store
        .read_turn_trace(&session_id, "turn-denial-write-1")
        .unwrap()
        .expect("write failure must persist a failed Trace");
    assert!(trace.trace.events.iter().all(|event| !matches!(
        event.payload,
        TracePayload::RuntimeDenial { .. } | TracePayload::Approval { .. }
    )));
    drop(store);
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[test]
fn stdio_command_output_produces_scoped_observed_diagnostics_and_raw_authority() {
    let codex = fake_codex();
    let data_root = codex.parent().unwrap().join("diagnostic-workbench");
    fs::create_dir_all(&data_root).unwrap();
    let project_root = codex.parent().unwrap().join("project");
    fs::create_dir_all(project_root.join("src")).unwrap();
    fs::write(
        project_root.join("src/main.rs"),
        "fn main() {\n    missing();\n}\n",
    )
    .unwrap();
    let project_root = project_root.canonicalize().unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .env("AEGISY_WORKBENCH_DATA_ROOT", &data_root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });

    send(
        &mut stdin,
        &request(
            "diagnostic-initialize",
            "initialize",
            initialize_params("test"),
        ),
    );
    receive_until(&receiver, |message| {
        message["id"] == "diagnostic-initialize"
    });
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );
    send(
        &mut stdin,
        &request(
            "diagnostic-project",
            "project/open",
            json!({ "root": project_root }),
        ),
    );
    let project = receive_until(&receiver, |message| message["id"] == "diagnostic-project");
    let project_id = project["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    send(
        &mut stdin,
        &request(
            "diagnostic-session",
            "session/start",
            json!({ "mode": "work", "project_id": project_id }),
        ),
    );
    let session = receive_until(&receiver, |message| message["id"] == "diagnostic-session");
    let session_id = session["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    send(
        &mut stdin,
        &request(
            "diagnostic-turn",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "emit diagnostics",
                "idempotency_key": "diagnostic-fixture-turn",
                "generation": 1
            }),
        ),
    );

    let deadline = std::time::Instant::now() + Duration::from_secs(15);
    let mut command_output_ref = None;
    let mut diagnostic_data = None;
    let mut command_started_data = None;
    let mut command_completed_data = None;
    while diagnostic_data.is_none() {
        let message = receiver
            .recv_timeout(deadline.saturating_duration_since(std::time::Instant::now()))
            .expect("sidecar did not emit command diagnostics");
        let event = message["params"]["event"].as_str().unwrap_or("");
        if event == "item.started" && message["params"]["item"]["id"] == "command-fixture" {
            command_started_data = Some(message["params"]["item"]["data"].clone());
        }
        if event == "item.completed" && message["params"]["item"]["id"] == "command-fixture" {
            command_completed_data = Some(message["params"]["item"]["data"].clone());
            command_output_ref = message["params"]["item"]["data"]["output"]["artifact"]
                ["reference"]
                .as_str()
                .map(str::to_owned);
        }
        if event == "diagnostics.observed" {
            diagnostic_data = Some(message["params"]["item"]["data"].clone());
        }
    }
    let diagnostic_data = diagnostic_data.unwrap();
    let command_started_data =
        command_started_data.expect("command start item needs structured data");
    let command_completed_data =
        command_completed_data.expect("command completion needs structured data");
    assert!(command_started_data["cwd"].is_string());
    assert_eq!(
        command_started_data["environment"]["execution_binding"],
        "codex-adapter-launch-contract"
    );
    assert_eq!(command_started_data["environment"]["values_exposed"], false);
    assert_eq!(
        command_started_data["environment"]["contract"]["schema_version"],
        "codex-child-environment/0.1"
    );
    assert_eq!(
        command_started_data["environment"]["contract"]["launch"]["env_clear"],
        true
    );
    assert_eq!(
        command_started_data["environment"]["contract"]["child_observation"],
        "vendor-command-item-does-not-report-child-environment"
    );
    assert_eq!(command_started_data["risk"]["level"], "medium");
    assert_eq!(
        command_started_data["schema_version"],
        "command-timeline/0.1"
    );
    assert_eq!(command_started_data["status"], "inProgress");
    assert_eq!(command_started_data["source"], "agent");
    let command_started_at_ms = command_started_data["started_at_ms"]
        .as_u64()
        .expect("command start must retain the provider Unix timestamp");
    assert_eq!(command_started_data["completed_at_ms"], Value::Null);
    assert_eq!(
        command_completed_data["schema_version"],
        "command-timeline/0.1"
    );
    assert_eq!(command_completed_data["status"], "failed");
    assert_eq!(command_completed_data["source"], "agent");
    assert_eq!(
        command_completed_data["started_at_ms"],
        command_started_at_ms
    );
    let command_completed_at_ms = command_completed_data["completed_at_ms"]
        .as_u64()
        .expect("command completion must retain the provider Unix timestamp");
    assert_eq!(command_completed_at_ms - command_started_at_ms, 12);
    assert_eq!(command_completed_data["duration_ms"], 12);
    assert_eq!(command_completed_data["exit_code"], 101);
    assert_eq!(diagnostic_data["diagnostics"][0]["path"], "src/main.rs");
    assert_eq!(diagnostic_data["diagnostics"][0]["source_kind"], "command");
    assert_eq!(
        diagnostic_data["diagnostics"][0]["source_identity"],
        "command:rustc"
    );
    let command_output_ref = command_output_ref.expect("diagnostic command needs raw authority");
    let raw_output_ref = diagnostic_data["raw_output_ref"]
        .as_str()
        .unwrap()
        .to_owned();

    send(
        &mut stdin,
        &request(
            "diagnostic-command-output",
            "artifact/read-command-output",
            json!({ "session_id": session_id, "reference": command_output_ref }),
        ),
    );
    let command_output = receive_until(&receiver, |message| {
        message["id"] == "diagnostic-command-output"
    });
    assert_eq!(command_output["result"]["session_id"], session_id);
    assert!(command_output["result"]["content"]
        .as_str()
        .unwrap()
        .contains("src/main.rs:2:5"));
    send(
        &mut stdin,
        &request(
            "diagnostic-raw",
            "workspace/diagnostics/raw",
            json!({ "project_id": project_id, "reference": raw_output_ref }),
        ),
    );
    let raw = receive_until(&receiver, |message| message["id"] == "diagnostic-raw");
    let raw_content = raw["result"]["content"].as_str().unwrap();
    assert!(raw_content.contains("command_output_ref"));
    assert!(raw_content.contains("src/main.rs"));
    assert!(!raw_content.contains("output_excerpt"));

    send(
        &mut stdin,
        &request("diagnostic-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| message["id"] == "diagnostic-shutdown");
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();
    let store = WorkbenchStore::open(&data_root).unwrap();
    let completed_trace = store
        .read_turn_trace(&session_id, "turn-fixture")
        .unwrap()
        .expect("read-only Work completion must persist a terminal trace");
    assert_eq!(completed_trace.trace.schema_version, "turn-trace/0.6");
    let tool_events = completed_trace
        .trace
        .events
        .iter()
        .filter(|event| matches!(event.payload, TracePayload::Tool { .. }))
        .collect::<Vec<_>>();
    assert_eq!(tool_events.len(), 2);
    let TracePayload::Tool {
        tool_identity: started_tool_identity,
        action_identity: started_action_identity,
        state: ToolState::Started,
        provider_status: Some(ToolProviderStatus::InProgress),
        source: Some(ToolSource::Agent),
        input_identity: Some(started_input_identity),
        output_identity: None,
        item_binding: Some(ToolTimelineBinding::NotPersisted),
        duration_ms: None,
        exit_code: None,
        evidence: started_evidence,
        ..
    } = &tool_events[0].payload
    else {
        panic!("command start must produce one provider-observed Started Tool")
    };
    assert_eq!(tool_events[0].at_ms, command_started_at_ms);
    assert_eq!(started_evidence.source, TraceEvidenceSource::ToolRuntime);
    assert_eq!(started_evidence.observed_at_ms, Some(command_started_at_ms));
    let TracePayload::Tool {
        tool_identity,
        action_identity,
        state: ToolState::Failed,
        provider_status: Some(ToolProviderStatus::Failed),
        source: Some(ToolSource::Agent),
        input_identity: Some(input_identity),
        output_identity: Some(output_identity),
        item_binding:
            Some(ToolTimelineBinding::Persisted {
                item_identity,
                payload_identity,
            }),
        duration_ms: Some(12),
        exit_code: Some(101),
        evidence: terminal_evidence,
        ..
    } = &tool_events[1].payload
    else {
        panic!("failed command completion must produce one persisted Failed Tool")
    };
    assert_eq!(tool_events[1].at_ms, command_completed_at_ms);
    assert_eq!(tool_identity, started_tool_identity);
    assert_eq!(action_identity, started_action_identity);
    assert_eq!(input_identity, started_input_identity);
    assert!(output_identity.starts_with("sha256:"));
    assert!(item_identity.starts_with("sha256:"));
    assert_eq!(item_identity.len(), 71);
    assert_ne!(item_identity, "command-fixture");
    assert!(payload_identity.starts_with("sha256:"));
    assert_eq!(terminal_evidence.source, TraceEvidenceSource::ToolRuntime);
    assert_eq!(
        terminal_evidence.observed_at_ms,
        Some(command_completed_at_ms)
    );
    let intent_identity = completed_trace
        .trace
        .events
        .iter()
        .find_map(|event| match &event.payload {
            TracePayload::Intent {
                session_mode: TraceSessionMode::Work,
                turn_kind: TraceTurnKind::ReadOnlyInspection,
                access: TraceTurnAccess::ReadOnly,
                intent_identity,
                ..
            } => Some(intent_identity.as_str()),
            _ => None,
        })
        .expect("Work trace must retain its read-only intent");
    let TracePayload::Terminal {
        state: TraceTerminalState::Completed,
        evidence,
        ..
    } = &completed_trace.trace.events.last().unwrap().payload
    else {
        panic!("Work trace must end with completed terminal evidence")
    };
    let completion = evidence.completion.as_ref().unwrap();
    assert_eq!(completion.intent_identity, intent_identity);
    assert!(matches!(
        completion.workspace_change,
        CompletionDomain::NotApplicable { .. }
    ));
    assert!(matches!(
        completion.git_change,
        CompletionDomain::NotApplicable { .. }
    ));
    let CompletionDomain::Unknown { evidence } = &completion.verification else {
        panic!("unobserved Work verification must remain unknown")
    };
    assert_eq!(evidence.source, TraceEvidenceSource::Runtime);
    assert_eq!(evidence.identity, None);
    assert_eq!(evidence.observed_at_ms, None);
    assert!(completed_trace.trace.events.iter().all(|event| !matches!(
        event.payload,
        TracePayload::Change { .. } | TracePayload::Test { .. }
    )));
    let serialized_trace = serde_json::to_string(&completed_trace.trace).unwrap();
    for forbidden in [
        "cargo check",
        "cannot find function missing",
        "src/main.rs",
        "aegisy-cancel-fixture",
    ] {
        assert!(!serialized_trace.contains(forbidden), "found {forbidden}");
    }
    drop(store);
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[test]
fn stdio_command_completed_and_declined_preserve_exact_provider_tool_states() {
    for (
        input,
        item_id,
        expected_elapsed_ms,
        expected_state,
        expected_status,
        expected_duration_ms,
        expected_exit_code,
        forbidden_content,
    ) in [
        (
            "emit completed command",
            "command-completed",
            12,
            ToolState::Completed,
            ToolProviderStatus::Completed,
            Some(12),
            Some(0),
            "printf done",
        ),
        (
            "emit declined command",
            "command-declined",
            1,
            ToolState::Declined,
            ToolProviderStatus::Declined,
            None,
            None,
            "blocked-command",
        ),
    ] {
        let codex = fake_codex();
        let data_root = codex.parent().unwrap().join("tool-terminal-workbench");
        fs::create_dir_all(&data_root).unwrap();
        let project_root = codex.parent().unwrap().join("tool-project");
        fs::create_dir_all(&project_root).unwrap();
        let project_root = project_root.canonicalize().unwrap();
        let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
            .env("AEGISY_CODEX_PATH", &codex)
            .env("AEGISY_WORKBENCH_DATA_ROOT", &data_root)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let mut stdin = child.stdin.take().unwrap();
        let stdout = child.stdout.take().unwrap();
        let (sender, receiver) = mpsc::channel();
        let reader = thread::spawn(move || {
            for line in BufReader::new(stdout).lines().map_while(Result::ok) {
                if let Ok(message) = serde_json::from_str(&line) {
                    if sender.send(message).is_err() {
                        return;
                    }
                }
            }
        });

        send(
            &mut stdin,
            &request("tool-initialize", "initialize", initialize_params("test")),
        );
        receive_until(&receiver, |message| message["id"] == "tool-initialize");
        send(
            &mut stdin,
            &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
        );
        send(
            &mut stdin,
            &request(
                "tool-project",
                "project/open",
                json!({ "root": project_root }),
            ),
        );
        let project = receive_until(&receiver, |message| message["id"] == "tool-project");
        let project_id = project["result"]["project"]["id"]
            .as_str()
            .unwrap()
            .to_owned();
        send(
            &mut stdin,
            &request(
                "tool-session",
                "session/start",
                json!({ "mode": "work", "project_id": project_id }),
            ),
        );
        let session = receive_until(&receiver, |message| message["id"] == "tool-session");
        let session_id = session["result"]["session"]["id"]
            .as_str()
            .unwrap()
            .to_owned();
        send(
            &mut stdin,
            &request(
                "tool-turn",
                "turn/start",
                json!({
                    "session_id": session_id,
                    "input": input,
                    "idempotency_key": format!("{item_id}-turn"),
                    "generation": 1
                }),
            ),
        );
        receive_until(&receiver, |message| message["id"] == "tool-turn");
        send(&mut stdin, &request("tool-shutdown", "shutdown", json!({})));
        receive_until(&receiver, |message| message["id"] == "tool-shutdown");
        drop(stdin);
        assert!(child.wait().unwrap().success());
        reader.join().unwrap();

        let store = WorkbenchStore::open(&data_root).unwrap();
        let trace = store
            .read_turn_trace(&session_id, "turn-fixture")
            .unwrap()
            .expect("terminal command Turn must persist its Trace");
        assert_eq!(trace.trace.schema_version, "turn-trace/0.6");
        let tool_events = trace
            .trace
            .events
            .iter()
            .filter(|event| matches!(event.payload, TracePayload::Tool { .. }))
            .collect::<Vec<_>>();
        assert_eq!(tool_events.len(), 2);
        let TracePayload::Tool {
            tool_identity: started_tool_identity,
            action_identity: started_action_identity,
            state: ToolState::Started,
            provider_status: Some(ToolProviderStatus::InProgress),
            source: Some(ToolSource::Agent),
            input_identity: Some(started_input_identity),
            output_identity: None,
            item_binding: Some(ToolTimelineBinding::NotPersisted),
            duration_ms: None,
            exit_code: None,
            ..
        } = &tool_events[0].payload
        else {
            panic!("provider command must begin with exact Started authority")
        };
        let started_at_ms = tool_events[0].at_ms;
        let TracePayload::Tool {
            tool_identity,
            action_identity,
            state,
            provider_status,
            source: Some(ToolSource::Agent),
            input_identity: Some(input_identity),
            output_identity: Some(output_identity),
            item_binding:
                Some(ToolTimelineBinding::Persisted {
                    item_identity: persisted_item_identity,
                    payload_identity,
                }),
            duration_ms,
            exit_code,
            ..
        } = &tool_events[1].payload
        else {
            panic!("provider terminal command must bind one persisted Timeline Item")
        };
        let completed_at_ms = tool_events[1].at_ms;
        assert_eq!(completed_at_ms - started_at_ms, expected_elapsed_ms);
        assert_eq!(*state, expected_state);
        assert_eq!(*provider_status, Some(expected_status));
        assert_eq!(tool_identity, started_tool_identity);
        assert_eq!(action_identity, started_action_identity);
        assert_eq!(input_identity, started_input_identity);
        assert!(output_identity.starts_with("sha256:"));
        assert!(persisted_item_identity.starts_with("sha256:"));
        assert_eq!(persisted_item_identity.len(), 71);
        assert_ne!(persisted_item_identity, item_id);
        assert!(payload_identity.starts_with("sha256:"));
        assert_eq!(*duration_ms, expected_duration_ms);
        assert_eq!(*exit_code, expected_exit_code);
        assert!(trace.trace.events.iter().all(|event| !matches!(
            event.payload,
            TracePayload::Approval { .. }
                | TracePayload::Tool {
                    state: ToolState::Requested | ToolState::Cancelled,
                    ..
                }
        )));
        let serialized = serde_json::to_string(&trace.trace).unwrap();
        for forbidden in [
            forbidden_content,
            "done\\n",
            "user-denied",
            "policy-denied",
            "cancelled",
            "aegisy-cancel-fixture",
        ] {
            assert!(!serialized.contains(forbidden), "found {forbidden}");
        }
        drop(store);
        let _ = fs::remove_dir_all(codex.parent().unwrap());
    }
}

#[test]
fn stdio_completed_turn_with_incomplete_command_fails_durably_and_restarts_started_only() {
    let codex = fake_codex();
    let fixture_root = codex.parent().unwrap();
    let data_root = fixture_root.join("tool-incomplete-workbench");
    let project_root = fixture_root.join("tool-project");
    fs::create_dir_all(&data_root).unwrap();
    fs::create_dir_all(&project_root).unwrap();
    let project_root = project_root.canonicalize().unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .env("AEGISY_WORKBENCH_DATA_ROOT", &data_root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });

    send(
        &mut stdin,
        &request(
            "tool-incomplete-initialize",
            "initialize",
            initialize_params("test"),
        ),
    );
    receive_until(&receiver, |message| {
        message["id"] == "tool-incomplete-initialize"
    });
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );
    send(
        &mut stdin,
        &request(
            "tool-incomplete-project",
            "project/open",
            json!({ "root": project_root }),
        ),
    );
    let project = receive_until(&receiver, |message| {
        message["id"] == "tool-incomplete-project"
    });
    let project_id = project["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    send(
        &mut stdin,
        &request(
            "tool-incomplete-session",
            "session/start",
            json!({ "mode": "work", "project_id": project_id }),
        ),
    );
    let session = receive_until(&receiver, |message| {
        message["id"] == "tool-incomplete-session"
    });
    let session_id = session["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    send(
        &mut stdin,
        &request(
            "tool-incomplete-turn",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "emit incomplete command",
                "idempotency_key": "tool-incomplete-turn",
                "generation": 1
            }),
        ),
    );
    let started = receive_until(&receiver, |message| {
        message["params"]["event"] == "item.started"
            && message["params"]["item"]["id"] == "command-incomplete"
    });
    assert_eq!(started["params"]["item"]["state"], "started");
    let failed = receive_until(&receiver, |message| {
        message["params"]["event"] == "turn.failed"
    });
    assert_eq!(failed["params"]["item"]["data"]["terminal_persisted"], true);

    send(
        &mut stdin,
        &request("tool-incomplete-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| {
        message["id"] == "tool-incomplete-shutdown"
    });
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();

    let store = WorkbenchStore::open(&data_root).unwrap();
    let first_read = store
        .read_turn_trace(&session_id, "turn-fixture")
        .unwrap()
        .expect("incomplete command must produce an authoritative failed Trace");
    assert_eq!(first_read.state, "failed");
    assert_atomic_failed_terminal(&store, &session_id, "turn-fixture", "protocol");
    assert_eq!(first_read.trace.schema_version, "turn-trace/0.6");
    let tool_events = first_read
        .trace
        .events
        .iter()
        .filter(|event| matches!(event.payload, TracePayload::Tool { .. }))
        .collect::<Vec<_>>();
    assert_eq!(tool_events.len(), 1);
    assert!(matches!(
        tool_events[0].payload,
        TracePayload::Tool {
            state: ToolState::Started,
            item_binding: Some(ToolTimelineBinding::NotPersisted),
            output_identity: None,
            ..
        }
    ));
    assert!(matches!(
        first_read
            .trace
            .events
            .get(first_read.trace.events.len() - 2)
            .map(|event| &event.payload),
        Some(TracePayload::Error { .. })
    ));
    assert!(matches!(
        first_read.trace.events.last().map(|event| &event.payload),
        Some(TracePayload::Terminal {
            state: TraceTerminalState::Failed,
            ..
        })
    ));
    assert!(store
        .read_session_items(&session_id, 0, 100)
        .unwrap()
        .iter()
        .all(|item| item.item_kind != "command"));
    let blob_scan = store.scan_durable_blobs().unwrap();
    assert!(blob_scan.consistent);
    assert_eq!(blob_scan.checked_objects, 0);
    assert_eq!(blob_scan.checked_references, 0);
    assert_eq!(blob_scan.disk_objects, 0);
    let first_trace_json = serde_json::to_string(&first_read.trace).unwrap();
    for forbidden in [
        "printf pending",
        "tool-project",
        "command-incomplete",
        "aegisy-cancel-fixture",
    ] {
        assert!(!first_trace_json.contains(forbidden), "found {forbidden}");
    }
    drop(store);

    let restarted = WorkbenchStore::open(&data_root).unwrap();
    let restarted_read = restarted
        .read_turn_trace(&session_id, "turn-fixture")
        .unwrap()
        .expect("restart must retain the incomplete command failure Trace");
    assert_eq!(restarted_read.state, "failed");
    assert_eq!(restarted_read.trace, first_read.trace);
    assert!(restarted
        .read_session_items(&session_id, 0, 100)
        .unwrap()
        .iter()
        .all(|item| item.item_kind != "command"));
    drop(restarted);
    let _ = fs::remove_dir_all(fixture_root);
}

#[test]
fn stdio_command_persistence_failure_retains_started_without_terminal_tool_or_blob() {
    let codex = fake_codex();
    let fixture_root = codex.parent().unwrap();
    let data_root = fixture_root.join("tool-persistence-failure-workbench");
    fs::create_dir_all(&data_root).unwrap();
    let project_root = fixture_root.join("project");
    fs::create_dir_all(project_root.join("src")).unwrap();
    fs::write(
        project_root.join("src/main.rs"),
        "fn main() {\n    missing();\n}\n",
    )
    .unwrap();
    let project_root = project_root.canonicalize().unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .env("AEGISY_WORKBENCH_DATA_ROOT", &data_root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });

    send(
        &mut stdin,
        &request(
            "tool-failure-initialize",
            "initialize",
            initialize_params("test"),
        ),
    );
    receive_until(&receiver, |message| {
        message["id"] == "tool-failure-initialize"
    });
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );
    send(
        &mut stdin,
        &request(
            "tool-failure-project",
            "project/open",
            json!({ "root": project_root }),
        ),
    );
    let project = receive_until(&receiver, |message| message["id"] == "tool-failure-project");
    let project_id = project["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    send(
        &mut stdin,
        &request(
            "tool-failure-session",
            "session/start",
            json!({ "mode": "work", "project_id": project_id }),
        ),
    );
    let session = receive_until(&receiver, |message| message["id"] == "tool-failure-session");
    let session_id = session["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();

    let connection = rusqlite::Connection::open(data_root.join("aegisy-workbench.sqlite3"))
        .expect("open fixture Workbench database");
    connection
        .execute_batch(
            "CREATE TRIGGER fail_command_item_before_insert
             BEFORE INSERT ON items WHEN NEW.item_kind = 'command'
             BEGIN SELECT RAISE(ABORT, 'injected command Item persistence failure'); END;",
        )
        .unwrap();
    drop(connection);

    send(
        &mut stdin,
        &request(
            "tool-failure-turn",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "emit diagnostics",
                "idempotency_key": "tool-persistence-failure-turn",
                "generation": 1
            }),
        ),
    );
    let failed = receive_until(&receiver, |message| {
        message["params"]["event"] == "turn.failed"
    });
    assert_eq!(
        failed["params"]["item"]["data"]["class"], "storage",
        "unexpected budget failure: {failed:#}"
    );
    assert_eq!(failed["params"]["item"]["data"]["terminal_persisted"], true);

    send(
        &mut stdin,
        &request("tool-failure-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| {
        message["id"] == "tool-failure-shutdown"
    });
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();

    let store = WorkbenchStore::open(&data_root).unwrap();
    let trace = store
        .read_turn_trace(&session_id, "turn-fixture")
        .unwrap()
        .expect("persistence failure must retain an authoritative failed Trace");
    assert_eq!(trace.state, "failed");
    assert_atomic_failed_terminal(&store, &session_id, "turn-fixture", "storage");
    assert_eq!(trace.trace.schema_version, "turn-trace/0.6");
    let tool_events = trace
        .trace
        .events
        .iter()
        .filter(|event| matches!(event.payload, TracePayload::Tool { .. }))
        .collect::<Vec<_>>();
    assert_eq!(tool_events.len(), 1);
    assert!(matches!(
        tool_events[0].payload,
        TracePayload::Tool {
            state: ToolState::Started,
            item_binding: Some(ToolTimelineBinding::NotPersisted),
            output_identity: None,
            ..
        }
    ));
    assert!(trace.trace.events.iter().all(|event| !matches!(
        event.payload,
        TracePayload::Tool {
            state: ToolState::Completed | ToolState::Failed | ToolState::Declined,
            ..
        }
    )));
    assert!(matches!(
        trace.trace.events.last().map(|event| &event.payload),
        Some(TracePayload::Terminal {
            state: TraceTerminalState::Failed,
            ..
        })
    ));
    let items = store.read_session_items(&session_id, 0, 100).unwrap();
    assert!(items.iter().all(|item| item.item_kind != "command"));
    let blob_scan = store.scan_durable_blobs().unwrap();
    assert!(blob_scan.consistent);
    assert_eq!(blob_scan.checked_objects, 0);
    assert_eq!(blob_scan.checked_references, 0);
    assert_eq!(blob_scan.disk_objects, 0);
    let serialized = serde_json::to_string(&trace.trace).unwrap();
    for forbidden in [
        "cargo check",
        "cannot find function missing",
        "src/main.rs",
        "aegisy-cancel-fixture",
    ] {
        assert!(!serialized.contains(forbidden), "found {forbidden}");
    }
    drop(store);
    let _ = fs::remove_dir_all(fixture_root);
}

#[test]
fn stdio_trace_budget_exhaustion_fails_durably_before_command_terminal_persistence() {
    const MALICIOUS_PROVIDER_ITEM_ID: &str = "command-canary-alice-project-zero";

    let codex = trace_budget_codex();
    let fixture_root = codex.parent().unwrap();
    let data_root = fixture_root.join("trace-budget-workbench");
    let project_root = fixture_root.join("project");
    fs::create_dir_all(&data_root).unwrap();
    fs::create_dir_all(project_root.join("src")).unwrap();
    fs::write(project_root.join("src/main.rs"), "fn main() {}\n").unwrap();
    let project_root = project_root.canonicalize().unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .env("AEGISY_WORKBENCH_DATA_ROOT", &data_root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });

    send(
        &mut stdin,
        &request("budget-initialize", "initialize", initialize_params("test")),
    );
    receive_until(&receiver, |message| message["id"] == "budget-initialize");
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );
    send(
        &mut stdin,
        &request(
            "budget-project",
            "project/open",
            json!({ "root": project_root }),
        ),
    );
    let project = receive_until(&receiver, |message| message["id"] == "budget-project");
    let project_id = project["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    send(
        &mut stdin,
        &request(
            "budget-session",
            "session/start",
            json!({ "mode": "work", "project_id": project_id }),
        ),
    );
    let session = receive_until(&receiver, |message| message["id"] == "budget-session");
    let session_id = session["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    send(
        &mut stdin,
        &request(
            "budget-turn",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "exhaust durable trace budget",
                "idempotency_key": "trace-budget-fixture-turn",
                "generation": 1
            }),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "budget-turn");

    let deadline = std::time::Instant::now() + Duration::from_secs(90);
    let mut live_events = Vec::new();
    let failed = loop {
        let message = receiver
            .recv_timeout(deadline.saturating_duration_since(std::time::Instant::now()))
            .expect("sidecar did not exhaust the durable Trace budget");
        if message["params"]["event"].is_string() {
            live_events.push(message.clone());
        }
        if message["params"]["event"] == "turn.failed" {
            break message;
        }
    };
    assert_eq!(
        failed["params"]["item"]["data"]["class"], "storage",
        "unexpected budget failure: {failed:#}"
    );
    assert_eq!(failed["params"]["item"]["data"]["terminal_persisted"], true);
    let live_started = live_events
        .iter()
        .filter(|event| {
            event["params"]["event"] == "item.started"
                && event["params"]["item"]["kind"] == "command"
        })
        .count();
    let live_completed = live_events
        .iter()
        .filter(|event| {
            event["params"]["event"] == "item.completed"
                && event["params"]["item"]["kind"] == "command"
        })
        .count();
    assert!(
        live_completed >= 20,
        "only {live_completed} command terminals completed before failure: {failed:#}"
    );
    assert_eq!(live_started, live_completed + 1);

    send(
        &mut stdin,
        &request(
            "budget-live-read",
            "session/read",
            json!({ "session_id": session_id, "limit": 200 }),
        ),
    );
    let live_read = receive_until(&receiver, |message| message["id"] == "budget-live-read");
    assert!(live_read["result"].is_object());

    send(
        &mut stdin,
        &request("budget-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| message["id"] == "budget-shutdown");
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();

    let store = WorkbenchStore::open(&data_root).unwrap();
    let trace = store
        .read_turn_trace(&session_id, "turn-budget")
        .unwrap()
        .expect("Trace budget exhaustion must retain an authoritative failed Trace");
    assert_atomic_failed_terminal(&store, &session_id, "turn-budget", "storage");
    let durable_turn = store.load_turn("turn-budget").unwrap();
    assert_eq!(durable_turn.session_id, session_id);
    assert_eq!(durable_turn.state, "failed");
    assert_eq!(trace.state, "failed");
    assert!(matches!(
        trace.trace.events.last().map(|event| &event.payload),
        Some(TracePayload::Terminal {
            state: TraceTerminalState::Failed,
            ..
        })
    ));
    let started_actions = trace
        .trace
        .events
        .iter()
        .filter_map(|event| match &event.payload {
            TracePayload::Tool {
                action_identity,
                state: ToolState::Started,
                item_binding: Some(ToolTimelineBinding::NotPersisted),
                ..
            } => Some(action_identity),
            _ => None,
        })
        .collect::<Vec<_>>();
    let terminal_actions = trace
        .trace
        .events
        .iter()
        .filter_map(|event| match &event.payload {
            TracePayload::Tool {
                action_identity,
                state: ToolState::Completed | ToolState::Failed | ToolState::Declined,
                item_binding: Some(ToolTimelineBinding::Persisted { .. }),
                ..
            } => Some(action_identity),
            _ => None,
        })
        .collect::<Vec<_>>();
    assert_eq!(started_actions.len(), terminal_actions.len() + 1);
    let unmatched_started = started_actions
        .iter()
        .filter(|action| !terminal_actions.contains(action))
        .count();
    assert_eq!(unmatched_started, 1);

    let items = store.read_session_items(&session_id, 0, 200).unwrap();
    let command_items = items
        .iter()
        .filter(|item| item.item_kind == "command")
        .collect::<Vec<_>>();
    assert_eq!(command_items.len(), terminal_actions.len());
    let events = store.read_session_events(&session_id, 0, 200).unwrap();
    let command_item_events = events
        .iter()
        .filter(|event| {
            event.event_kind == "item.appended" && event.payload["item"]["item_kind"] == "command"
        })
        .count();
    assert_eq!(command_item_events, command_items.len());
    let trace_event = events
        .iter()
        .find(|event| event.event_kind == "turn.trace.recorded")
        .expect("the failed Turn must retain its durable Trace event");
    let blob_scan = store.scan_durable_blobs().unwrap();
    assert!(blob_scan.consistent);
    assert_eq!(blob_scan.checked_references, 0);
    assert_eq!(blob_scan.checked_objects, 0);
    assert_eq!(blob_scan.disk_objects, 0);
    let durable_json = serde_json::to_string(&json!({
        "trace": trace,
        "trace_event": trace_event,
        "blob_scan": blob_scan
    }))
    .unwrap();
    assert!(!durable_json.contains(MALICIOUS_PROVIDER_ITEM_ID));
    drop(store);

    let mut restarted = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .env("AEGISY_WORKBENCH_DATA_ROOT", &data_root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut restarted_stdin = restarted.stdin.take().unwrap();
    let restarted_stdout = restarted.stdout.take().unwrap();
    let (restarted_sender, restarted_receiver) = mpsc::channel();
    let restarted_reader = thread::spawn(move || {
        for line in BufReader::new(restarted_stdout)
            .lines()
            .map_while(Result::ok)
        {
            if let Ok(message) = serde_json::from_str(&line) {
                if restarted_sender.send(message).is_err() {
                    return;
                }
            }
        }
    });
    send(
        &mut restarted_stdin,
        &request(
            "budget-restart-initialize",
            "initialize",
            initialize_params("test"),
        ),
    );
    receive_until(&restarted_receiver, |message| {
        message["id"] == "budget-restart-initialize"
    });
    send(
        &mut restarted_stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );
    send(
        &mut restarted_stdin,
        &request(
            "budget-restart-read",
            "session/read",
            json!({ "session_id": session_id, "limit": 200 }),
        ),
    );
    let replay = receive_until(&restarted_receiver, |message| {
        message["id"] == "budget-restart-read"
    });
    assert!(replay["result"].is_object());
    assert_eq!(
        replay["result"]["session"]["id"].as_str(),
        Some(session_id.as_str())
    );
    send(
        &mut restarted_stdin,
        &request("budget-restart-shutdown", "shutdown", json!({})),
    );
    receive_until(&restarted_receiver, |message| {
        message["id"] == "budget-restart-shutdown"
    });
    drop(restarted_stdin);
    assert!(restarted.wait().unwrap().success());
    restarted_reader.join().unwrap();
    let _ = fs::remove_dir_all(fixture_root);
}

#[test]
fn stdio_control_heartbeats_steers_and_cancels_while_normal_dispatch_is_blocked() {
    let codex = fake_codex();
    let data_root = codex.parent().expect("fixture directory").join("workbench");
    fs::create_dir_all(&data_root).unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .env("AEGISY_WORKBENCH_DATA_ROOT", &data_root)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });

    send(
        &mut stdin,
        &request("1", "initialize", initialize_params("test")),
    );
    let initialized = receive_until(&receiver, |message| message["id"] == "1");
    assert!(initialized["result"]["capabilities"]["stable"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "turn.cancel.interrupt"));
    assert!(initialized["result"]["capabilities"]["stable"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "turn.steer.same-turn"));
    assert!(initialized["result"]["capabilities"]["stable"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "runtime.heartbeat.out-of-band"));
    assert!(initialized["result"]["capabilities"]["stable"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "session.provider.lifecycle.archive"));
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );
    let mut health = Value::Null;
    for index in 0..20 {
        send(
            &mut stdin,
            &request(&format!("health-{index}"), "runtime/health", json!({})),
        );
        health = receive_until(&receiver, |message| {
            message["id"] == format!("health-{index}")
        });
        if health["result"]["stderr"]["bytes"].as_u64().unwrap_or(0) > 0 {
            break;
        }
        thread::sleep(Duration::from_millis(10));
    }
    assert!(
        health["result"]["stderr"]["bytes"].as_u64().unwrap_or(0) > 0,
        "health response did not include stderr bytes: {health}"
    );
    assert!(health["result"]["stderr"]["lines"].as_u64().unwrap_or(0) > 0);
    assert!(
        health["result"]["stderr"]["redacted_lines"]
            .as_u64()
            .unwrap_or(0)
            > 0
    );
    assert_eq!(health["result"]["stderr"]["last_class"], "warning");
    assert!(!health.to_string().contains("ghp_"));
    send(
        &mut stdin,
        &request("2", "session/start", json!({ "mode": "chat" })),
    );
    let session = receive_until(&receiver, |message| message["id"] == "2");
    let session_id = session["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();

    send(
        &mut stdin,
        &request(
            "3",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "wait for cancellation",
                "idempotency_key": "cancel-fixture-turn",
                "generation": 1
            }),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "3");
    receive_until(&receiver, |message| {
        message["params"]["event"] == "turn.started"
    });
    for index in 0..40 {
        send(
            &mut stdin,
            &request(
                &format!("queued-{index}"),
                "session/read",
                json!({ "session_id": session_id }),
            ),
        );
    }
    assert!(
        receive_until(&receiver, |message| message["error"]["code"] == -32004)["error"]["message"]
            .as_str()
            .unwrap()
            .contains("queue")
    );
    send(
        &mut stdin,
        &request(
            "heartbeat-saturated",
            "runtime/heartbeat",
            json!({
                "schema_version": "runtime-heartbeat-request/0.1",
                "nonce": "saturated-turn-nonce"
            }),
        ),
    );
    let heartbeat = receive_until(&receiver, |message| message["id"] == "heartbeat-saturated");
    assert_eq!(
        heartbeat,
        json!({
            "jsonrpc": "2.0",
            "id": "heartbeat-saturated",
            "result": {
                "schema_version": "runtime-heartbeat/0.1",
                "nonce": "saturated-turn-nonce",
                "state": "alive"
            }
        })
    );
    send(
        &mut stdin,
        &request(
            "4",
            "turn/steer",
            json!({
                "session_id": session_id,
                "turn_id": "turn-fixture",
                "input": "focus on the latest instruction",
                "client_user_message_id": "steer-fixture-message"
            }),
        ),
    );
    let steer = receive_until(&receiver, |message| message["id"] == "4");
    assert_eq!(steer["result"]["state"], "steering-requested");
    let steering_item = receive_until(&receiver, |message| {
        message["params"]["event"] == "turn.steering-requested"
    });
    assert_eq!(
        steering_item["params"]["item"]["content"],
        "focus on the latest instruction"
    );
    receive_until(&receiver, |message| {
        message["params"]["event"] == "turn.steering-acknowledged"
    });

    send(
        &mut stdin,
        &request(
            "5",
            "turn/cancel",
            json!({ "session_id": session_id, "turn_id": "turn-fixture" }),
        ),
    );
    let accepted = receive_until(&receiver, |message| message["id"] == "5");
    assert_eq!(accepted["result"]["state"], "cancellation-requested");
    receive_until(&receiver, |message| {
        message["params"]["event"] == "turn.cancellation-acknowledged"
    });
    receive_until(&receiver, |message| {
        message["params"]["event"] == "turn.interrupted"
    });

    send(&mut stdin, &request("6", "shutdown", json!({})));
    receive_until(&receiver, |message| message["id"] == "6");
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();

    let store = WorkbenchStore::open(&data_root).unwrap();
    let interrupted_trace = store
        .read_turn_trace(&session_id, "turn-fixture")
        .unwrap()
        .expect("interrupted turn must persist a terminal trace");
    assert_eq!(
        store.load_turn("turn-fixture").unwrap().state,
        "interrupted"
    );
    assert_eq!(interrupted_trace.state, "interrupted");
    assert_eq!(interrupted_trace.trace.binding.session_id, session_id);
    assert_eq!(interrupted_trace.trace.binding.turn_id, "turn-fixture");
    assert_eq!(interrupted_trace.trace.binding.project_id, None);
    assert!(interrupted_trace
        .trace
        .binding
        .environment_identity
        .is_some());
    let TracePayload::Terminal {
        state, evidence, ..
    } = &interrupted_trace
        .trace
        .events
        .last()
        .expect("interrupted trace must have a terminal event")
        .payload
    else {
        panic!("interrupted trace must end with a terminal event")
    };
    assert_eq!(*state, TraceTerminalState::Interrupted);
    assert_eq!(evidence.workspace_identity, None);
    assert_eq!(evidence.git_state_identity, None);
    assert_eq!(evidence.verification_identity, None);
    assert_eq!(evidence.observed_verification_count, 0);
    assert_eq!(evidence.evidence.source, TraceEvidenceSource::Provider);
    let usage_position = interrupted_trace
        .trace
        .events
        .iter()
        .position(|event| matches!(event.payload, TracePayload::UsageReport { .. }))
        .expect("interrupted trace must retain observed Usage");
    assert!(usage_position < interrupted_trace.trace.events.len() - 1);
    assert_eq!(
        interrupted_trace
            .trace
            .events
            .iter()
            .filter(|event| matches!(event.payload, TracePayload::Terminal { .. }))
            .count(),
        1
    );
    assert!(interrupted_trace.trace.events.iter().all(|event| !matches!(
        event.payload,
        TracePayload::Tool { .. }
            | TracePayload::Change { .. }
            | TracePayload::Test { .. }
            | TracePayload::Error { .. }
    )));
    let serialized_trace = serde_json::to_string(&interrupted_trace).unwrap();
    for forbidden in [
        "wait for cancellation",
        "focus on the latest instruction",
        "ghp_",
        "aegisy-cancel-fixture",
    ] {
        assert!(!serialized_trace.contains(forbidden), "found {forbidden}");
    }
    drop(store);
    let reopened = WorkbenchStore::open(&data_root).unwrap();
    assert_eq!(
        reopened
            .read_turn_trace(&session_id, "turn-fixture")
            .unwrap()
            .unwrap(),
        interrupted_trace
    );
    drop(reopened);
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[test]
fn stdio_provider_archive_and_unarchive_follow_session_lifecycle() {
    let codex = fake_codex();
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });

    send(
        &mut stdin,
        &request(
            "lifecycle-initialize",
            "initialize",
            initialize_params("test"),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "lifecycle-initialize");
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );
    send(
        &mut stdin,
        &request(
            "lifecycle-session",
            "session/start",
            json!({ "mode": "chat" }),
        ),
    );
    let session = receive_until(&receiver, |message| message["id"] == "lifecycle-session");
    let session_id = session["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();

    send(
        &mut stdin,
        &request(
            "lifecycle-archive",
            "session/archive",
            json!({ "session_id": session_id }),
        ),
    );
    let archived = receive_until(&receiver, |message| message["id"] == "lifecycle-archive");
    assert_eq!(archived["result"]["status"], "archived");
    assert_eq!(archived["result"]["provider_state_updated"], true);

    send(
        &mut stdin,
        &request(
            "lifecycle-unarchive",
            "session/unarchive",
            json!({ "session_id": session_id }),
        ),
    );
    let unarchived = receive_until(&receiver, |message| message["id"] == "lifecycle-unarchive");
    assert_eq!(unarchived["result"]["status"], "active");
    assert_eq!(unarchived["result"]["provider_state_updated"], true);

    send(
        &mut stdin,
        &request("lifecycle-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| message["id"] == "lifecycle-shutdown");
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[test]
fn stdio_provider_thread_list_and_read_are_bounded_metadata_projections() {
    let codex = fake_codex();
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });

    send(
        &mut stdin,
        &request(
            "provider-read-initialize",
            "initialize",
            initialize_params("test"),
        ),
    );
    let initialized = receive_until(&receiver, |message| {
        message["id"] == "provider-read-initialize"
    });
    assert!(initialized["result"]["capabilities"]["stable"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "session.provider.lifecycle.list-read"));
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );

    send(
        &mut stdin,
        &request(
            "provider-list",
            "session/provider-list",
            json!({ "limit": 10 }),
        ),
    );
    let listed = receive_until(&receiver, |message| message["id"] == "provider-list");
    assert_eq!(
        listed["result"]["threads"][0]["thread_id"],
        "thread-fixture"
    );
    assert_eq!(
        listed["result"]["schema_version"],
        "provider-thread-list/0.2"
    );
    assert_eq!(
        listed["result"]["content_projection"],
        "content-free-metadata"
    );
    assert_eq!(listed["result"]["provider_state_only"], true);
    assert_eq!(listed["result"]["content_omitted"], true);
    assert_eq!(
        listed["result"]["cursor_projection"],
        "validated-lossless-opaque"
    );
    assert_eq!(listed["result"]["cursor_byte_limit"], 4 * 1024);
    assert_eq!(listed["result"]["next_cursor"], "cursor-next-1");
    assert_eq!(listed["result"]["backwards_cursor"], "cursor-previous-1");

    send(
        &mut stdin,
        &request(
            "provider-list-next-page",
            "session/provider-list",
            json!({ "limit": 10, "cursor": listed["result"]["next_cursor"] }),
        ),
    );
    let next_page = receive_until(&receiver, |message| {
        message["id"] == "provider-list-next-page"
    });
    assert_eq!(next_page["result"]["threads"], json!([]));
    assert_eq!(next_page["result"]["next_cursor"], Value::Null);
    assert_eq!(next_page["result"]["backwards_cursor"], "cursor-next-1");
    let listed_thread = listed["result"]["threads"][0].as_object().unwrap();
    for forbidden in [
        "title", "name", "preview", "cwd", "path", "source", "items", "content",
    ] {
        assert!(
            !listed_thread.contains_key(forbidden),
            "provider list leaked forbidden field {forbidden}"
        );
    }

    for (id, cursor) in [
        (
            "provider-list-sensitive-request",
            "Authorization: Bearer ghp_123456789012345678901234567890".to_owned(),
        ),
        ("provider-list-control-request", "cursor\nnext".to_owned()),
        ("provider-list-oversized-request", "x".repeat(4 * 1024 + 1)),
    ] {
        send(
            &mut stdin,
            &request(
                id,
                "session/provider-list",
                json!({ "limit": 10, "cursor": cursor }),
            ),
        );
        let rejected = receive_until(&receiver, |message| message["id"] == id);
        assert_eq!(rejected["error"]["code"], -32602);
    }

    send(
        &mut stdin,
        &request(
            "provider-list-sensitive-output",
            "session/provider-list",
            json!({ "limit": 10, "cursor": "fixture-sensitive-output" }),
        ),
    );
    let sensitive_output = receive_until(&receiver, |message| {
        message["id"] == "provider-list-sensitive-output"
    });
    assert_eq!(sensitive_output["error"]["code"], -32143);
    assert!(!serde_json::to_string(&sensitive_output)
        .unwrap()
        .contains("ghp_123456789012345678901234567890"));
    assert_eq!(listed_thread["name_present"], true);
    assert_eq!(listed_thread["preview_present"], true);
    assert_eq!(listed_thread["content_omitted"], true);
    assert!(listed_thread["workspace_identity"]
        .as_str()
        .unwrap()
        .starts_with("sha256:"));
    let listed_json = serde_json::to_string(&listed).unwrap();
    for forbidden in [
        "provider-session-private-sentinel",
        "provider-name-private-sentinel",
        "provider-preview-private-sentinel",
        "/private/provider-workspace-sentinel",
        "/private/provider-rollout-sentinel.jsonl",
        "provider-model-private-sentinel",
    ] {
        assert!(
            !listed_json.contains(forbidden),
            "provider list response leaked {forbidden}"
        );
    }

    send(
        &mut stdin,
        &request(
            "provider-read",
            "session/provider-read",
            json!({ "thread_id": "thread-fixture", "include_turns": true }),
        ),
    );
    let read = receive_until(&receiver, |message| message["id"] == "provider-read");
    assert_eq!(read["result"]["thread"]["thread_id"], "thread-fixture");
    assert_eq!(read["result"]["turns"][0]["turn_id"], "turn-fixture");
    assert_eq!(read["result"]["schema_version"], "provider-thread-read/0.2");
    assert_eq!(read["result"]["provider_items_omitted"], true);
    assert_eq!(read["result"]["content_omitted"], true);
    let read_thread = read["result"]["thread"].as_object().unwrap();
    for forbidden in [
        "title", "name", "preview", "cwd", "path", "source", "items", "content",
    ] {
        assert!(
            !read_thread.contains_key(forbidden),
            "provider read leaked forbidden field {forbidden}"
        );
    }
    let read_turn = read["result"]["turns"][0].as_object().unwrap();
    for forbidden in ["items", "content", "error"] {
        assert!(
            !read_turn.contains_key(forbidden),
            "provider turn summary leaked forbidden field {forbidden}"
        );
    }
    let read_json = serde_json::to_string(&read).unwrap();
    for forbidden in [
        "provider-session-private-sentinel",
        "provider-name-private-sentinel",
        "provider-preview-private-sentinel",
        "/private/provider-workspace-sentinel",
        "/private/provider-rollout-sentinel.jsonl",
        "provider-model-private-sentinel",
        "provider-item-private-sentinel",
        "userMessage",
    ] {
        assert!(
            !read_json.contains(forbidden),
            "provider read response leaked {forbidden}"
        );
    }

    send(
        &mut stdin,
        &request(
            "provider-read-colliding-turn-prefixes",
            "session/provider-read",
            json!({ "thread_id": "thread-long-turn-ids", "include_turns": true }),
        ),
    );
    let colliding_turns = receive_until(&receiver, |message| {
        message["id"] == "provider-read-colliding-turn-prefixes"
    });
    assert_eq!(colliding_turns["error"]["code"], -32143);
    assert!(colliding_turns.get("result").is_none());

    send(
        &mut stdin,
        &request("provider-read-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| {
        message["id"] == "provider-read-shutdown"
    });
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[test]
fn stdio_unknown_codex_notification_is_only_a_bounded_health_diagnostic() {
    let codex = fake_codex();
    let data_root = codex
        .parent()
        .unwrap()
        .join("unknown-notification-workbench");
    let (mut child, mut stdin, receiver, reader, session_id) =
        start_codex_runtime(&codex, &data_root, "unknown-notification");

    send(
        &mut stdin,
        &request(
            "unknown-notification-turn",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "emit unknown diagnostics",
                "idempotency_key": "unknown-notification-turn",
                "generation": 1
            }),
        ),
    );
    receive_until(&receiver, |message| {
        message["id"] == "unknown-notification-turn"
    });
    receive_until(&receiver, |message| {
        message["method"] == "event" && message["params"]["event"] == "turn.completed"
    });

    send(
        &mut stdin,
        &request("unknown-notification-health", "runtime/health", json!({})),
    );
    let health = receive_until(&receiver, |message| {
        message["id"] == "unknown-notification-health"
    });
    let diagnostics = &health["result"]["unknown_notifications"];
    assert_eq!(
        diagnostics["schema_version"],
        "codex-unknown-notification-diagnostics/0.1"
    );
    assert_eq!(diagnostics["total_count"], 1);
    assert_eq!(diagnostics["unretained_count"], 0);
    assert_eq!(diagnostics["methods"].as_array().map(Vec::len), Some(1));
    assert_eq!(diagnostics["methods"][0]["count"], 1);
    assert_eq!(
        diagnostics["methods"][0]["method_sha256"],
        format!(
            "sha256:{:x}",
            Sha256::digest(b"future/private-method-sk-never-log")
        )
    );

    send(
        &mut stdin,
        &request(
            "unknown-notification-read",
            "session/read",
            json!({ "session_id": session_id, "limit": 100 }),
        ),
    );
    let replay = receive_until(&receiver, |message| {
        message["id"] == "unknown-notification-read"
    });
    let combined = serde_json::to_string(&(&health, &replay)).unwrap();
    for forbidden in [
        "future/private-method-sk-never-log",
        "ghp_unknown_notification_private_body",
        "/private/unknown/source.rs",
        "private provider content",
    ] {
        assert!(
            !combined.contains(forbidden),
            "unknown notification diagnostic leaked {forbidden}"
        );
    }
    assert!(!combined.contains("\"kind\":\"unknown-notification\""));
    let replay_json = serde_json::to_string(&replay).unwrap();
    assert!(!replay_json.contains("method_sha256"));
    assert!(!replay_json.contains("codex-unknown-notification-diagnostics/0.1"));

    send(
        &mut stdin,
        &request("unknown-notification-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| {
        message["id"] == "unknown-notification-shutdown"
    });
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[test]
fn stdio_user_input_server_request_returns_unsupported_without_fake_answers() {
    let codex = fake_codex();
    let response_path = codex.parent().unwrap().join("user-input-response.json");
    let data_root = codex.parent().unwrap().join("user-input-workbench");
    let (mut child, mut stdin, receiver, reader, session_id) =
        start_codex_runtime(&codex, &data_root, "user-input");

    send(
        &mut stdin,
        &request(
            "user-input-turn",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "request unsupported user input",
                "idempotency_key": "user-input-turn",
                "generation": 1
            }),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "user-input-turn");
    receive_until(&receiver, |message| {
        message["method"] == "event" && message["params"]["event"] == "turn.completed"
    });

    let provider_response: Value =
        serde_json::from_str(&fs::read_to_string(&response_path).unwrap()).unwrap();
    assert_eq!(provider_response["id"], "user-input-request-sentinel");
    assert_eq!(provider_response["error"]["code"], -32601);
    assert_eq!(
        provider_response["error"]["message"],
        "user input request is unsupported"
    );
    assert!(provider_response.get("result").is_none());
    let response_json = serde_json::to_string(&provider_response).unwrap();
    for forbidden in [
        "answers",
        "questions",
        "user-input-item-private-sentinel",
        "user-input-header-private-sentinel",
        "user-input-question-private-sentinel",
        "user-input-option-private-sentinel",
        "ghp_user_input_private_sentinel",
    ] {
        assert!(
            !response_json.contains(forbidden),
            "user input response leaked or fabricated {forbidden}"
        );
    }

    send(
        &mut stdin,
        &request("user-input-health", "runtime/health", json!({})),
    );
    let health = receive_until(&receiver, |message| message["id"] == "user-input-health");
    assert_eq!(health["result"]["unknown_notifications"]["total_count"], 0);
    send(
        &mut stdin,
        &request(
            "user-input-read",
            "session/read",
            json!({ "session_id": session_id, "limit": 100 }),
        ),
    );
    let replay = receive_until(&receiver, |message| message["id"] == "user-input-read");
    let replay_json = serde_json::to_string(&replay).unwrap();
    for forbidden in [
        "answers",
        "questions",
        "user-input-item-private-sentinel",
        "user-input-header-private-sentinel",
        "user-input-question-private-sentinel",
        "user-input-option-private-sentinel",
        "ghp_user_input_private_sentinel",
    ] {
        assert!(
            !replay_json.contains(forbidden),
            "user input request entered the Timeline as {forbidden}"
        );
    }

    send(
        &mut stdin,
        &request("user-input-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| message["id"] == "user-input-shutdown");
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[cfg(target_os = "macos")]
#[test]
fn stdio_control_stops_user_terminal_while_model_dispatch_is_blocked() {
    let codex = fake_codex();
    let project_root = codex.parent().unwrap().join("terminal-project");
    fs::create_dir_all(&project_root).unwrap();
    let project_root = project_root.canonicalize().unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"))
        .env("AEGISY_CODEX_PATH", &codex)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Ok(message) = serde_json::from_str(&line) {
                if sender.send(message).is_err() {
                    return;
                }
            }
        }
    });

    send(
        &mut stdin,
        &request(
            "terminal-initialize",
            "initialize",
            initialize_params("test"),
        ),
    );
    let initialized = receive_until(&receiver, |message| message["id"] == "terminal-initialize");
    assert!(initialized["result"]["capabilities"]["stable"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "terminal.stop.out-of-band"));
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized", "params": {} }),
    );
    send(
        &mut stdin,
        &request(
            "terminal-project",
            "project/open",
            json!({ "root": project_root }),
        ),
    );
    let project = receive_until(&receiver, |message| message["id"] == "terminal-project");
    let project_id = project["result"]["project"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    send(
        &mut stdin,
        &request(
            "terminal-session",
            "session/start",
            json!({ "mode": "work", "project_id": project_id }),
        ),
    );
    let session = receive_until(&receiver, |message| message["id"] == "terminal-session");
    let session_id = session["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    send(
        &mut stdin,
        &request(
            "terminal-other-session",
            "session/start",
            json!({ "mode": "work", "project_id": project_id }),
        ),
    );
    let other_session = receive_until(&receiver, |message| {
        message["id"] == "terminal-other-session"
    });
    let other_session_id = other_session["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned();
    send(
        &mut stdin,
        &request(
            "terminal-open",
            "terminal/open-user",
            json!({
                "session_id": session_id,
                "kind": "background",
                "name": "Cancellation fixture"
            }),
        ),
    );
    let opened = receive_until(&receiver, |message| message["id"] == "terminal-open");
    let terminal_id = opened["result"]["terminal_id"].as_str().unwrap().to_owned();
    let command =
        BASE64_STANDARD.encode("sh -c 'printf \"AEGISY_CHILD:%s\\n\" \"$$\"; exec sleep 30'\n");
    send(
        &mut stdin,
        &request(
            "terminal-input",
            "terminal/input-user",
            json!({
                "session_id": session_id,
                "terminal_id": terminal_id,
                "data_base64": command
            }),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "terminal-input");

    let deadline = std::time::Instant::now() + Duration::from_secs(5);
    let mut read_index = 0_u32;
    let child_pid = loop {
        let read_id = format!("terminal-read-child-{read_index}");
        send(
            &mut stdin,
            &request(
                &read_id,
                "terminal/read",
                json!({ "session_id": session_id, "terminal_id": terminal_id }),
            ),
        );
        let snapshot = receive_until(&receiver, |message| message["id"] == read_id);
        let output = BASE64_STANDARD
            .decode(snapshot["result"]["output_base64"].as_str().unwrap())
            .unwrap();
        let output = String::from_utf8_lossy(&output);
        let parsed = output
            .match_indices("AEGISY_CHILD:")
            .find_map(|(offset, marker)| {
                let suffix = &output[offset + marker.len()..];
                let digits = suffix
                    .chars()
                    .take_while(|character| character.is_ascii_digit())
                    .collect::<String>();
                (!digits.is_empty()).then(|| digits.parse::<libc::pid_t>().unwrap())
            });
        if let Some(pid) = parsed {
            break pid;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "terminal child process did not start"
        );
        read_index += 1;
        thread::sleep(Duration::from_millis(10));
    };

    send(
        &mut stdin,
        &request(
            "terminal-turn",
            "turn/start",
            json!({
                "session_id": session_id,
                "input": "wait while the user stops a terminal",
                "idempotency_key": "terminal-stop-fixture-turn",
                "generation": 1
            }),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "terminal-turn");
    receive_until(&receiver, |message| {
        message["params"]["event"] == "turn.started"
    });
    for index in 0..40 {
        send(
            &mut stdin,
            &request(
                &format!("terminal-queued-{index}"),
                "session/read",
                json!({ "session_id": session_id }),
            ),
        );
    }
    receive_until(&receiver, |message| message["error"]["code"] == -32004);
    send(
        &mut stdin,
        &request(
            "terminal-stop-wrong-session",
            "terminal/stop-user",
            json!({ "session_id": other_session_id, "terminal_id": terminal_id }),
        ),
    );
    let denied = receive_until(&receiver, |message| {
        message["id"] == "terminal-stop-wrong-session"
    });
    assert_eq!(denied["error"]["code"], -32093);
    send(
        &mut stdin,
        &request(
            "terminal-stop",
            "terminal/stop-user",
            json!({ "session_id": session_id, "terminal_id": terminal_id }),
        ),
    );
    let stopped = receive_until(&receiver, |message| message["id"] == "terminal-stop");
    assert!(stopped["result"].is_object());
    assert!(matches!(
        stopped["result"]["state"].as_str(),
        Some("stopping" | "exited")
    ));
    send(
        &mut stdin,
        &request(
            "terminal-stop-repeat",
            "terminal/stop-user",
            json!({ "session_id": session_id, "terminal_id": terminal_id }),
        ),
    );
    let repeated = receive_until(&receiver, |message| message["id"] == "terminal-stop-repeat");
    assert!(repeated["result"].is_object());

    send(
        &mut stdin,
        &request(
            "terminal-cancel-turn",
            "turn/cancel",
            json!({ "session_id": session_id, "turn_id": "turn-fixture" }),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "terminal-cancel-turn");
    receive_until(&receiver, |message| {
        message["params"]["event"] == "turn.interrupted"
    });

    let deadline = std::time::Instant::now() + Duration::from_secs(5);
    loop {
        let alive = unsafe { libc::kill(child_pid, 0) == 0 };
        if !alive && std::io::Error::last_os_error().raw_os_error() == Some(libc::ESRCH) {
            break;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "stopped terminal left its foreground child alive"
        );
        thread::sleep(Duration::from_millis(10));
    }

    send(
        &mut stdin,
        &request("terminal-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| message["id"] == "terminal-shutdown");
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}
