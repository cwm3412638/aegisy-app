#![cfg(unix)]

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
    let deadline = std::time::Instant::now() + Duration::from_secs(5);
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
while IFS= read -r line; do
  id=$(printf '%s' "$line" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p')
  case "$line" in
    *'"method":"initialize"'*)
      printf '{"id":%s,"result":{}}\n' "$id"
      ;;
    *'"method":"thread/start"'*)
      printf '{"id":%s,"result":{"thread":{"id":"thread-fixture"},"modelProvider":"fixture","model":"fixture"}}\n' "$id"
      ;;
    *'"method":"turn/start"'*)
      printf '{"id":%s,"result":{"turn":{"id":"turn-fixture"}}}\n' "$id"
      case "$line" in
        *'emit diagnostics'*)
          printf '{"method":"item/started","params":{"threadId":"thread-fixture","turnId":"turn-fixture","item":{"id":"command-fixture","type":"commandExecution","command":"cargo check","commandActions":[{"type":"unknown"}],"cwd":"%s","status":"inProgress","source":"agent"}}}\n' "$AEGISY_FIXTURE_ROOT"
          printf '{"method":"item/commandExecution/outputDelta","params":{"threadId":"thread-fixture","turnId":"turn-fixture","itemId":"command-fixture","delta":"error[E0425]: cannot find function missing\\n --> src/main.rs:2:5\\n"}}\n'
          printf '{"method":"item/completed","params":{"threadId":"thread-fixture","turnId":"turn-fixture","item":{"id":"command-fixture","type":"commandExecution","command":"cargo check","commandActions":[{"type":"unknown"}],"cwd":"%s","status":"failed","aggregatedOutput":"error[E0425]: cannot find function missing\\n --> src/main.rs:2:5\\n","durationMs":12,"exitCode":101,"source":"agent"}}}\n' "$AEGISY_FIXTURE_ROOT"
          printf '{"method":"turn/completed","params":{"threadId":"thread-fixture","turn":{"id":"turn-fixture","status":"completed"}}}\n'
          ;;
      esac
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
    while diagnostic_data.is_none() {
        let message = receiver
            .recv_timeout(deadline.saturating_duration_since(std::time::Instant::now()))
            .expect("sidecar did not emit command diagnostics");
        let event = message["params"]["event"].as_str().unwrap_or("");
        if event == "item.completed" && message["params"]["item"]["id"] == "command-fixture" {
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
fn stdio_control_cancels_a_turn_while_normal_dispatch_is_blocked() {
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
    send(
        &mut stdin,
        &json!({ "jsonrpc": "2.0", "method": "initialized" }),
    );
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
            "turn/cancel",
            json!({ "session_id": session_id, "turn_id": "turn-fixture" }),
        ),
    );
    let accepted = receive_until(&receiver, |message| message["id"] == "4");
    assert_eq!(accepted["result"]["state"], "cancellation-requested");
    receive_until(&receiver, |message| {
        message["params"]["event"] == "turn.cancellation-acknowledged"
    });
    receive_until(&receiver, |message| {
        message["params"]["event"] == "turn.interrupted"
    });

    send(&mut stdin, &request("5", "shutdown", json!({})));
    receive_until(&receiver, |message| message["id"] == "5");
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
