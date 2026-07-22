#![cfg(unix)]

use aegisy_agentd::turn_trace::{
    ErrorClass as TraceErrorClass, EvidenceSource as TraceEvidenceSource,
    TerminalState as TraceTerminalState, TracePayload,
};
use aegisy_agentd::workbench_store::WorkbenchStore;
#[cfg(target_os = "macos")]
use base64::engine::general_purpose::STANDARD as BASE64_STANDARD;
#[cfg(target_os = "macos")]
use base64::Engine;
use serde_json::{json, Value};
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
printf 'warning: fixture stderr Authorization: Bearer ghp_123456789012345678901234567890\n' >&2
while IFS= read -r line; do
  id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p')
  case "$line" in
    *'"method":"initialize"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      ;;
    *'"method":"thread/start"'*)
      printf '{"id":%s,"result":{"thread":{"id":"thread-fixture"},"modelProvider":"fixture","model":"fixture"}}\n' "$id"
      ;;
    *'"method":"thread/archive"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      ;;
    *'"method":"thread/unarchive"'*)
      printf '{"id":%s,"result":{"thread":{"id":"thread-fixture"},"modelProvider":"fixture","model":"fixture"}}\n' "$id"
      ;;
    *'"method":"thread/list"'*)
      printf '{"id":%s,"result":{"data":[{"id":"thread-fixture","sessionId":"session-fixture","name":"Fixture session","preview":"Read-only fixture preview","cwd":"/tmp/provider-fixture","modelProvider":"fixture","source":"appServer","status":{"type":"idle"},"createdAt":10,"updatedAt":20,"recencyAt":20,"ephemeral":false,"forkedFromId":null,"turns":[]}],"nextCursor":null,"backwardsCursor":null}}\n' "$id"
      ;;
    *'"method":"thread/read"'*)
      printf '{"id":%s,"result":{"thread":{"id":"thread-fixture","sessionId":"session-fixture","name":"Fixture session","preview":"Read-only fixture preview","cwd":"/tmp/provider-fixture","modelProvider":"fixture","source":"appServer","status":{"type":"idle"},"createdAt":10,"updatedAt":20,"recencyAt":20,"ephemeral":false,"forkedFromId":null,"turns":[{"id":"turn-fixture","items":[],"status":"completed","startedAt":10,"completedAt":20,"durationMs":10}]}}}\n' "$id"
      ;;
    *'"method":"turn/start"'*)
      printf '{"id":%s,"result":{"turn":{"id":"turn-fixture"}}}\n' "$id"
      case "$line" in
        *'emit metadata'*)
          printf '{"method":"thread/tokenUsage/updated","params":{"threadId":"thread-fixture","turnId":"turn-fixture","tokenUsage":{"last":{"cachedInputTokens":1,"inputTokens":2,"outputTokens":3,"reasoningOutputTokens":0,"totalTokens":5},"total":{"cachedInputTokens":1,"inputTokens":2,"outputTokens":3,"reasoningOutputTokens":0,"totalTokens":5},"modelContextWindow":128000}}}\n'
          printf '{"method":"turn/plan/updated","params":{"threadId":"thread-fixture","turnId":"turn-fixture","explanation":"Inspect then verify","plan":[{"status":"inProgress","step":"Inspect"},{"status":"pending","step":"Verify"}]}}\n'
          printf '{"method":"turn/diff/updated","params":{"threadId":"thread-fixture","turnId":"turn-fixture","diff":"@@ -1 +1 @@\\n-old\\n+new\\n"}}\n'
          printf '{"method":"turn/completed","params":{"threadId":"thread-fixture","turn":{"id":"turn-fixture","status":"completed"}}}\n'
          ;;
        *'emit diagnostics'*)
          printf '{"method":"item/started","params":{"threadId":"thread-fixture","turnId":"turn-fixture","item":{"id":"command-fixture","type":"commandExecution","command":"cargo check","commandActions":[{"type":"unknown"}],"cwd":"%s","status":"inProgress","source":"agent"}}}\n' "$AEGISY_FIXTURE_ROOT"
          printf '{"method":"item/commandExecution/outputDelta","params":{"threadId":"thread-fixture","turnId":"turn-fixture","itemId":"command-fixture","delta":"error[E0425]: cannot find function missing\\n --> src/main.rs:2:5\\n"}}\n'
          printf '{"method":"item/completed","params":{"threadId":"thread-fixture","turnId":"turn-fixture","item":{"id":"command-fixture","type":"commandExecution","command":"cargo check","commandActions":[{"type":"unknown"}],"cwd":"%s","status":"failed","aggregatedOutput":"error[E0425]: cannot find function missing\\n --> src/main.rs:2:5\\n","durationMs":12,"exitCode":101,"source":"agent"}}}\n' "$AEGISY_FIXTURE_ROOT"
          printf '{"method":"turn/completed","params":{"threadId":"thread-fixture","turn":{"id":"turn-fixture","status":"completed"}}}\n'
          ;;
      esac
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
      printf '{"id":%s,"result":{"thread":{"id":"thread-reconnect"},"modelProvider":"fixture","model":"fixture"}}\n' "$id"
      ;;
    *'"method":"turn/start"'*)
      printf '{"id":%s,"result":{"turn":{"id":"turn-reconnect-%s"}}}\n' "$id" "$count"
      if [ "$count" -eq 1 ]; then
        printf '{"method":"item/agentMessage/delta","params":{"threadId":"thread-reconnect","turnId":"turn-reconnect-1","itemId":"item-reconnect","delta":"partial\\n"}}\n'
        exit 23
      fi
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
      printf '{"id":%s,"result":{"thread":{"id":"thread-provider-failure"},"modelProvider":"fixture","model":"fixture"}}\n' "$id"
      ;;
    *'"method":"turn/start"'*)
      printf '{"id":%s,"result":{"turn":{"id":"turn-provider-failure"}}}\n' "$id"
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
      printf '{"id":%s,"result":{"thread":{"id":"thread-approval"},"modelProvider":"fixture","model":"fixture"}}\n' "$id"
      ;;
    *'"method":"turn/start"'*)
      printf '{"id":%s,"result":{"turn":{"id":"turn-approval"}}}\n' "$id"
      printf '{"id":99,"method":"item/commandExecution/requestApproval","params":{"threadId":"thread-approval","turnId":"turn-approval","itemId":"command-approval","command":"rm -rf project-data","risk":"high"}}\n'
      ;;
    *'"id":99,"result":{"decision":"decline"}'*)
      printf '%s' "decline" > "$decision_file"
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
            json!({
                "protocol_version": "0.1",
                "client": { "name": "test", "version": "1" }
            }),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "metadata-initialize");
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized" }),
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
                "idempotency_key": "metadata-fixture-turn"
            }),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "metadata-turn");
    let deadline = std::time::Instant::now() + Duration::from_secs(5);
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
    let usage_event = events
        .iter()
        .find(|event| event["params"]["event"] == "usage.updated")
        .expect("metadata fixture did not emit usage");
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
            json!({
                "protocol_version": "0.1",
                "client": { "name": "test", "version": "1" }
            }),
        ),
    );
    receive_until(&restarted_receiver, |message| {
        message["id"] == "metadata-reinitialize"
    });
    send(
        &mut restarted_stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized" }),
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
            json!({
                "protocol_version": "0.1",
                "client": { "name": "test", "version": "1" }
            }),
        ),
    );
    let initialized = receive_until(&receiver, |message| message["id"] == "startup-initialize");
    assert_eq!(initialized["result"]["backend"]["status"], "unavailable");
    assert_eq!(
        initialized["result"]["capabilities"][0],
        "runtime.unavailable"
    );
    let attempts_text = fs::read_to_string(&attempts).unwrap();
    assert_eq!(attempts_text.len(), 3);

    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized" }),
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
            json!({
                "protocol_version": "0.1",
                "client": { "name": "test", "version": "1" }
            }),
        ),
    );
    let initialized = receive_until(&receiver, |message| message["id"] == "restart-initialize");
    assert!(initialized["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "runtime.restart"));
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized" }),
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
        &request("restart", "runtime/restart", json!({})),
    );
    let restarted = receive_until(&receiver, |message| message["id"] == "restart");
    assert_eq!(restarted["result"]["status"], "restarted");
    assert_eq!(restarted["result"]["health"]["state"], "running");

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
            json!({
                "protocol_version": "0.1",
                "client": { "name": "test", "version": "1" }
            }),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "reconnect-initialize");
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized" }),
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
                "idempotency_key": "reconnect-first"
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
                "idempotency_key": "reconnect-second"
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
    assert!(store
        .read_turn_trace(&session_id, "turn-reconnect-2")
        .unwrap()
        .is_none());
    drop(store);
    let reopened = WorkbenchStore::open(&data_root).unwrap();
    assert_eq!(
        reopened
            .read_turn_trace(&session_id, "turn-reconnect-1")
            .unwrap()
            .unwrap(),
        failed_trace
    );
    assert!(reopened
        .read_turn_trace(&session_id, "turn-reconnect-2")
        .unwrap()
        .is_none());
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
            json!({
                "protocol_version": "0.1",
                "client": { "name": "test", "version": "1" }
            }),
        ),
    );
    receive_until(&receiver, |message| {
        message["id"] == "provider-failure-initialize"
    });
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized" }),
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
            "approval-initialize",
            "initialize",
            json!({
                "protocol_version": "0.1",
                "client": { "name": "test", "version": "1" }
            }),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "approval-initialize");
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized" }),
    );
    send(
        &mut stdin,
        &request(
            "approval-session",
            "session/start",
            json!({ "mode": "chat" }),
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
                "idempotency_key": "approval-turn"
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
    assert_eq!(fs::read_to_string(&decision).unwrap(), "decline");
    assert!(!failed.to_string().contains("rm -rf"));

    send(
        &mut stdin,
        &request("approval-shutdown", "shutdown", json!({})),
    );
    receive_until(&receiver, |message| message["id"] == "approval-shutdown");
    drop(stdin);
    assert!(child.wait().unwrap().success());
    reader.join().unwrap();
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[test]
fn stdio_command_output_produces_scoped_observed_diagnostics_and_raw_authority() {
    let codex = fake_codex();
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
        .env("AEGISY_FIXTURE_ROOT", &project_root)
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
            json!({
                "protocol_version": "0.1",
                "client": { "name": "test", "version": "1" }
            }),
        ),
    );
    receive_until(&receiver, |message| {
        message["id"] == "diagnostic-initialize"
    });
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized" }),
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
                "idempotency_key": "diagnostic-fixture-turn"
            }),
        ),
    );

    let deadline = std::time::Instant::now() + Duration::from_secs(5);
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
    assert_eq!(command_completed_data["status"], "failed");
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
    let _ = fs::remove_dir_all(codex.parent().unwrap());
}

#[test]
fn stdio_control_steers_and_cancels_a_turn_while_normal_dispatch_is_blocked() {
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
        &request(
            "1",
            "initialize",
            json!({
                "protocol_version": "0.1",
                "client": { "name": "test", "version": "1" }
            }),
        ),
    );
    let initialized = receive_until(&receiver, |message| message["id"] == "1");
    assert!(initialized["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "turn.cancel.interrupt"));
    assert!(initialized["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "turn.steer.same-turn"));
    assert!(initialized["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "session.provider.lifecycle.archive"));
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized" }),
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
                "idempotency_key": "cancel-fixture-turn"
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
            json!({
                "protocol_version": "0.1",
                "client": { "name": "test", "version": "1" }
            }),
        ),
    );
    receive_until(&receiver, |message| message["id"] == "lifecycle-initialize");
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized" }),
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
            json!({
                "protocol_version": "0.1",
                "client": { "name": "test", "version": "1" }
            }),
        ),
    );
    let initialized = receive_until(&receiver, |message| {
        message["id"] == "provider-read-initialize"
    });
    assert!(initialized["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "session.provider.lifecycle.list-read"));
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized" }),
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
    assert_eq!(listed["result"]["content_projection"], "metadata-only");
    assert_eq!(listed["result"]["provider_state_only"], true);
    assert!(listed["result"]["threads"][0].get("path").is_none());

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
    assert_eq!(read["result"]["provider_items_omitted"], true);

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
            json!({
                "protocol_version": "0.1",
                "client": { "name": "test", "version": "1" }
            }),
        ),
    );
    let initialized = receive_until(&receiver, |message| message["id"] == "terminal-initialize");
    assert!(initialized["result"]["capabilities"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "terminal.stop.out-of-band"));
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized" }),
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
                "idempotency_key": "terminal-stop-fixture-turn"
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
