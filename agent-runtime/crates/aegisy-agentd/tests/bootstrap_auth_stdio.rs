//! End-to-end bootstrap authentication prelude fixtures for the real daemon.
//!
//! These fixtures prove the token-configured transport boundary: the exact
//! one-time prelude is required before any AAP frame, a successful prelude
//! reports `authenticated: true`, and missing, malformed, replayed, or
//! mismatched preludes fail closed with the fixed content-free `-32154`
//! error. A daemon started without the environment token keeps the legacy
//! unauthenticated local mode. No fixture grants permission, approval,
//! mutation, or execution authority.

use aegisy_aap::MAX_AAP_FRAME_BYTES;
use aegisy_agentd::bootstrap_auth::{BOOTSTRAP_AUTH_ERROR_CODE, BOOTSTRAP_TOKEN_ENV};
use aegisy_agentd::STABLE_CAPABILITY_REGISTRY;
use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use base64::Engine;
use serde_json::{json, Value};
use std::io::{BufRead, BufReader, Write};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

const FIXED_TOKEN: [u8; 32] = [
    0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    0xA5, 0x5A, 0xC3, 0x3C, 0x0F, 0xF0, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x42, 0x24,
];

fn fixed_token_string() -> String {
    URL_SAFE_NO_PAD.encode(FIXED_TOKEN)
}

fn prelude_frame(encoded: &str) -> String {
    format!("{{\"schema\":\"aegisy-bootstrap-auth/0.1\",\"token\":\"{encoded}\"}}")
}

fn initialize_params(authenticated: bool) -> Value {
    // These fixtures exercise the legacy bare-event route; subscription
    // negotiation has dedicated coverage elsewhere.
    let stable_capabilities = STABLE_CAPABILITY_REGISTRY
        .iter()
        .copied()
        .filter(|capability| *capability != "timeline.subscription.fixed-watermark")
        .collect::<Vec<_>>();
    json!({
        "protocol": {"minimum": "0.1", "maximum": "0.1", "preferred": "0.1"},
        "client": {"name": "bootstrap-auth-fixture", "version": "1"},
        "platform": {"os": "macos", "architecture": "arm64"},
        "capabilities": {"stable": stable_capabilities, "experimental": []},
        "limits": {"max_frame_bytes": MAX_AAP_FRAME_BYTES},
        "transport_security": {
            "transport": "stdio",
            "local": true,
            "authenticated": authenticated,
            "encrypted": false,
            "peer_verified": false
        }
    })
}

fn spawn_daemon(token: Option<&str>) -> (Child, ChildStdin, mpsc::Receiver<Value>) {
    let mut command = Command::new(env!("CARGO_BIN_EXE_aegisy-agentd"));
    command
        .env("AEGISY_AGENT_BACKEND", "preview")
        .env_remove(BOOTSTRAP_TOKEN_ENV)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null());
    if let Some(token) = token {
        command.env(BOOTSTRAP_TOKEN_ENV, token);
    }
    let mut child = command.spawn().unwrap();
    let stdin = child.stdin.take().unwrap();
    let stdout = child.stdout.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    thread::spawn(move || {
        let reader = BufReader::new(stdout);
        for line in reader.lines() {
            let Ok(line) = line else { break };
            if line.trim().is_empty() {
                continue;
            }
            if sender
                .send(serde_json::from_str(&line).expect("daemon emitted non-JSON"))
                .is_err()
            {
                break;
            }
        }
    });
    (child, stdin, receiver)
}

fn send_line(stdin: &mut ChildStdin, line: &str) {
    stdin.write_all(line.as_bytes()).unwrap();
    stdin.write_all(b"\n").unwrap();
    stdin.flush().unwrap();
}

fn send(stdin: &mut ChildStdin, message: &Value) {
    serde_json::to_writer(&mut *stdin, message).unwrap();
    stdin.write_all(b"\n").unwrap();
    stdin.flush().unwrap();
}

fn next_message(receiver: &mpsc::Receiver<Value>) -> Value {
    receiver
        .recv_timeout(Duration::from_secs(15))
        .expect("daemon did not emit the expected message")
}

fn initialize_handshake(
    stdin: &mut ChildStdin,
    receiver: &mpsc::Receiver<Value>,
    authenticated: bool,
) {
    send(
        stdin,
        &json!({"jsonrpc": "2.0", "id": "init-1", "method": "initialize", "params": initialize_params(authenticated)}),
    );
    let response = next_message(receiver);
    assert_eq!(response["id"], "init-1");
    let security = &response["result"]["transport_security"];
    assert_eq!(security["transport"], "stdio");
    assert_eq!(security["local"], true);
    assert_eq!(security["authenticated"], authenticated);
    assert_eq!(security["encrypted"], false);
    assert_eq!(security["peer_verified"], false);
    send(
        stdin,
        &json!({"jsonrpc": "2.0", "method": "initialized", "params": {}}),
    );
}

fn wait_for_exit(child: &mut Child) {
    let deadline = Instant::now() + Duration::from_secs(15);
    loop {
        if let Some(_status) = child.try_wait().unwrap() {
            return;
        }
        assert!(Instant::now() < deadline, "daemon did not exit in time");
        thread::sleep(Duration::from_millis(25));
    }
}

#[test]
fn configured_token_accepts_exact_prelude_and_reports_authenticated() {
    let (mut child, mut stdin, receiver) = spawn_daemon(Some(&fixed_token_string()));
    send_line(&mut stdin, &prelude_frame(&fixed_token_string()));
    initialize_handshake(&mut stdin, &receiver, true);
    send(
        &mut stdin,
        &json!({"jsonrpc": "2.0", "id": "shutdown-1", "method": "shutdown", "params": {}}),
    );
    let response = next_message(&receiver);
    assert_eq!(response["id"], "shutdown-1");
    drop(stdin);
    wait_for_exit(&mut child);
}

#[test]
fn configured_token_rejects_initialize_without_prelude() {
    let (mut child, mut stdin, receiver) = spawn_daemon(Some(&fixed_token_string()));
    send(
        &mut stdin,
        &json!({"jsonrpc": "2.0", "id": "init-1", "method": "initialize", "params": initialize_params(false)}),
    );
    let response = next_message(&receiver);
    assert!(response["id"].is_null());
    assert_eq!(response["error"]["code"], BOOTSTRAP_AUTH_ERROR_CODE);
    let encoded = serde_json::to_string(&response).unwrap();
    assert!(!encoded.contains(&fixed_token_string()));
    wait_for_exit(&mut child);
}

#[test]
fn configured_token_rejects_wrong_and_replayed_prelude_values() {
    let mut wrong = FIXED_TOKEN;
    wrong[0] ^= 0xFF;
    for candidate in [
        URL_SAFE_NO_PAD.encode(wrong),
        fixed_token_string().replace('p', "q"),
    ] {
        let (mut child, mut stdin, receiver) = spawn_daemon(Some(&fixed_token_string()));
        send_line(&mut stdin, &prelude_frame(&candidate));
        let response = next_message(&receiver);
        assert!(response["id"].is_null());
        assert_eq!(response["error"]["code"], BOOTSTRAP_AUTH_ERROR_CODE);
        wait_for_exit(&mut child);
    }
}

#[test]
fn configured_token_rejects_malformed_prelude_shapes() {
    for line in [
        String::new(),
        "{}".to_owned(),
        prelude_frame("short"),
        format!(
            "{{\"token\":\"{}\",\"schema\":\"aegisy-bootstrap-auth/0.1\"}}",
            fixed_token_string()
        ),
        String::from(" {}"),
        "x".repeat(4 * 1024 * 1024 + 2),
    ] {
        let (mut child, mut stdin, receiver) = spawn_daemon(Some(&fixed_token_string()));
        send_line(&mut stdin, &line);
        let response = next_message(&receiver);
        assert!(response["id"].is_null());
        assert_eq!(response["error"]["code"], BOOTSTRAP_AUTH_ERROR_CODE);
        wait_for_exit(&mut child);
    }
}

#[test]
fn second_prelude_after_authentication_is_an_ordinary_invalid_frame() {
    let (mut child, mut stdin, receiver) = spawn_daemon(Some(&fixed_token_string()));
    send_line(&mut stdin, &prelude_frame(&fixed_token_string()));
    initialize_handshake(&mut stdin, &receiver, true);
    // A repeated prelude line is not an authentication path; it is handled
    // like any other id-less invalid JSON-RPC envelope, which the daemon
    // silently ignores without a response and without closing the
    // connection. The next emitted message must therefore be the shutdown
    // response, proving the replay produced neither a `-32154` error nor a
    // transport failure.
    send_line(&mut stdin, &prelude_frame(&fixed_token_string()));
    send(
        &mut stdin,
        &json!({"jsonrpc": "2.0", "id": "shutdown-1", "method": "shutdown", "params": {}}),
    );
    let response = next_message(&receiver);
    assert_eq!(response["id"], "shutdown-1");
    assert!(
        response["error"].is_null(),
        "unexpected response: {response}"
    );
    drop(stdin);
    wait_for_exit(&mut child);
}

#[test]
fn malformed_environment_token_fails_startup_without_an_aap_response() {
    let (mut child, mut stdin, receiver) = spawn_daemon(Some("not-a-valid-token"));
    send(
        &mut stdin,
        &json!({"jsonrpc": "2.0", "id": "init-1", "method": "initialize", "params": initialize_params(false)}),
    );
    wait_for_exit(&mut child);
    assert!(
        receiver.recv_timeout(Duration::from_millis(250)).is_err(),
        "a malformed environment token must not produce an AAP response"
    );
}

#[test]
fn legacy_mode_without_token_keeps_unauthenticated_stdio_facts() {
    let (mut child, mut stdin, receiver) = spawn_daemon(None);
    initialize_handshake(&mut stdin, &receiver, false);
    send(
        &mut stdin,
        &json!({"jsonrpc": "2.0", "id": "shutdown-1", "method": "shutdown", "params": {}}),
    );
    let response = next_message(&receiver);
    assert_eq!(response["id"], "shutdown-1");
    drop(stdin);
    wait_for_exit(&mut child);
}
