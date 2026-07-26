use aegisy_aap::MAX_AAP_FRAME_BYTES;
use aegisy_agentd::{Runtime, STABLE_CAPABILITY_REGISTRY};
use serde_json::{json, Value};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

const TIMELINE_SUBSCRIPTION_CAPABILITY: &str = "timeline.subscription.fixed-watermark";
const TIMELINE_SUBSCRIPTION_REQUEST_ROUTES: [(&str, &str); 4] = [
    ("subscribe", "timeline/subscribe"),
    ("subscription-sync", "timeline/subscription-sync"),
    ("subscription-snapshot", "timeline/subscription-snapshot"),
    ("subscription-activate", "timeline/subscription-activate"),
];

fn request(id: &str, method: &str, params: Value) -> String {
    json!({"jsonrpc": "2.0", "id": id, "method": method, "params": params}).to_string()
}

fn notification(method: &str, params: Value) -> String {
    json!({"jsonrpc": "2.0", "method": method, "params": params}).to_string()
}

fn initialize_params(capabilities: &[&str]) -> Value {
    json!({
        "protocol": {"minimum": "0.1", "maximum": "0.1", "preferred": "0.1"},
        "client": {"name": "runtime-test", "version": "1.0"},
        "platform": {"os": "macos", "architecture": "arm64"},
        "capabilities": {"stable": capabilities, "experimental": []},
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

fn initialize(runtime: &mut Runtime, capabilities: &[&str]) -> Value {
    initialize_with_id(runtime, "initialize", capabilities)
}

fn initialize_with_id(runtime: &mut Runtime, id: &str, capabilities: &[&str]) -> Value {
    runtime.handle_line(&request(id, "initialize", initialize_params(capabilities)))[0].clone()
}

fn ready(runtime: &mut Runtime, capabilities: &[&str]) -> Value {
    let initialized = initialize(runtime, capabilities);
    assert!(initialized.get("result").is_some(), "{initialized}");
    assert!(runtime
        .handle_line(&notification("initialized", json!({})))
        .is_empty());
    initialized
}

fn terminal_platform_capability() -> &'static str {
    #[cfg(target_os = "macos")]
    {
        "terminal.pty.macos.user-initiated"
    }
    #[cfg(target_os = "windows")]
    {
        "terminal.conpty.windows.user-initiated"
    }
    #[cfg(not(any(target_os = "macos", target_os = "windows")))]
    {
        "terminal.pty.unsupported"
    }
}

fn timeline_subscription_test_root(label: &str) -> PathBuf {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    std::env::temp_dir().join(format!("aegisy-handshake-{label}-{unique}"))
}

fn ready_timeline_subscription_runtime(root: &Path) -> Runtime {
    fs::create_dir_all(root).unwrap();
    let mut runtime = Runtime::with_store(root).unwrap();
    let initialized = ready(
        &mut runtime,
        &[
            "runtime.preview",
            "permission.read-only",
            "session.chat",
            "timeline.streaming",
            TIMELINE_SUBSCRIPTION_CAPABILITY,
            "timeline.replay.fixed-watermark",
            "timeline.snapshot.current",
        ],
    );
    assert!(initialized["result"]["capabilities"]["stable"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == TIMELINE_SUBSCRIPTION_CAPABILITY));
    runtime
}

fn start_chat_session(runtime: &mut Runtime, request_id: &str) -> String {
    let messages = runtime.handle_line(&request(
        request_id,
        "session/start",
        json!({"mode": "chat"}),
    ));
    assert_eq!(messages.len(), 1, "{messages:?}");
    messages[0]["result"]["session"]["id"]
        .as_str()
        .unwrap()
        .to_owned()
}

fn start_preview_turn(runtime: &mut Runtime, request_id: &str, session_id: &str) -> Vec<Value> {
    runtime.handle_line(&request(
        request_id,
        "turn/start",
        json!({
            "session_id": session_id,
            "input": request_id,
            "idempotency_key": request_id
        }),
    ))
}

fn subscribe(
    runtime: &mut Runtime,
    request_id: &str,
    session_id: &str,
    subscription_id: &str,
    connection_generation: u64,
) -> Value {
    let messages = runtime.handle_line(&request(
        request_id,
        "timeline/subscribe",
        json!({
            "schema_version": "timeline-subscribe-request/0.1",
            "connection_generation": connection_generation,
            "session_id": session_id,
            "subscription_id": subscription_id,
            "cursor": {"sequence": 0, "event_id": null},
            "watermark": null
        }),
    ));
    assert_eq!(messages.len(), 1, "{messages:?}");
    assert!(messages[0].get("result").is_some(), "{messages:?}");
    messages[0]["result"].clone()
}

fn subscription_sync(runtime: &mut Runtime, request_id: &str, subscription: &Value) -> Vec<Value> {
    runtime.handle_line(&request(
        request_id,
        "timeline/subscription-sync",
        json!({
            "schema_version": "timeline-subscription-sync-request/0.1",
            "connection_generation": subscription["connection_generation"],
            "session_id": subscription["session_id"],
            "subscription_id": subscription["subscription_id"],
            "request": {
                "session_id": subscription["session_id"],
                "after": subscription["cursor"],
                "watermark": subscription["watermark"],
                "limit": 200
            }
        }),
    ))
}

fn activate_subscription(
    runtime: &mut Runtime,
    request_id: &str,
    subscription: &Value,
) -> Vec<Value> {
    runtime.handle_line(&request(
        request_id,
        "timeline/subscription-activate",
        json!({
            "schema_version": "timeline-subscription-activate-request/0.1",
            "connection_generation": subscription["connection_generation"],
            "session_id": subscription["session_id"],
            "subscription_id": subscription["subscription_id"],
            "source": "sync",
            "cursor": subscription["watermark"],
            "watermark": subscription["watermark"],
            "snapshot_identity": null
        }),
    ))
}

#[test]
fn initialize_negotiates_only_the_declared_stable_intersection() {
    let mut runtime = Runtime::default();
    let response = ready(
        &mut runtime,
        &[
            "runtime.preview",
            "permission.read-only",
            "runtime.health",
            "future.valid-capability",
        ],
    );
    assert_eq!(response["result"]["protocol"]["minimum"], "0.1");
    assert_eq!(response["result"]["protocol"]["maximum"], "0.1");
    assert_eq!(response["result"]["protocol"]["selected"], "0.1");
    assert_eq!(response["result"]["protocol"]["upgrade_direction"], "none");
    assert_eq!(response["result"]["platform"]["architecture"], "arm64");
    assert_eq!(
        response["result"]["limits"]["max_frame_bytes"],
        MAX_AAP_FRAME_BYTES
    );
    assert_eq!(
        response["result"]["transport_security"]["transport"],
        "stdio"
    );
    assert_eq!(response["result"]["transport_security"]["local"], true);
    assert_eq!(
        response["result"]["transport_security"]["authenticated"],
        false
    );
    assert_eq!(response["result"]["transport_security"]["encrypted"], false);
    assert_eq!(
        response["result"]["transport_security"]["peer_verified"],
        false
    );
    assert_eq!(
        response["result"]["capabilities"]["stable"],
        json!(["runtime.preview", "runtime.health", "permission.read-only"])
    );
    assert_eq!(
        response["result"]["capabilities"]["experimental"],
        json!([])
    );

    assert!(
        runtime.handle_line(&request("health", "runtime/health", json!({})))[0]
            .get("result")
            .is_some()
    );
    let denied = runtime.handle_line(&request(
        "workspace-list",
        "workspace/list",
        json!({"project_id": "missing", "path": ""}),
    ));
    assert_eq!(denied[0]["error"]["code"], -32006);
}

#[test]
fn handshake_state_rejects_out_of_order_and_repeated_messages() {
    let mut runtime = Runtime::default();
    assert!(runtime
        .handle_line(&notification("initialized", json!({})))
        .is_empty());
    assert_eq!(
        runtime.handle_line(&request("before", "runtime/health", json!({})))[0]["error"]["code"],
        -32002
    );
    assert_eq!(
        runtime.handle_line(&request("shutdown", "shutdown", json!({})))[0]["error"]["code"],
        -32002
    );

    let initialized = initialize(
        &mut runtime,
        &["runtime.preview", "permission.read-only", "runtime.health"],
    );
    assert!(initialized.get("result").is_some());
    assert_eq!(
        runtime.handle_line(&request("early", "runtime/health", json!({})))[0]["error"]["code"],
        -32002
    );
    assert_eq!(
        runtime.handle_line(&request("wrong-kind", "initialized", json!({})))[0]["error"]["code"],
        -32600
    );
    for (index, malformed) in [
        r#"{"jsonrpc":"2.0","method":"initialized"}"#,
        r#"{"jsonrpc":"2.0","method":"initialized","params":null}"#,
        r#"{"jsonrpc":"2.0","method":"initialized","params":{"extra":true}}"#,
    ]
    .into_iter()
    .enumerate()
    {
        assert!(runtime.handle_line(malformed).is_empty());
        assert_eq!(
            runtime.handle_line(&request(
                &format!("still-early-{index}"),
                "runtime/health",
                json!({})
            ))[0]["error"]["code"],
            -32002
        );
    }

    assert!(runtime
        .handle_line(&notification("initialized", json!({})))
        .is_empty());
    assert!(runtime
        .handle_line(&notification("initialized", json!({})))
        .is_empty());
    assert!(
        runtime.handle_line(&request("health", "runtime/health", json!({})))[0]
            .get("result")
            .is_some()
    );
    assert_eq!(
        initialize_with_id(
            &mut runtime,
            "initialize-repeat",
            &["runtime.preview", "permission.read-only", "runtime.health"]
        )["error"]["code"],
        -32007
    );
}

#[test]
fn incompatible_ranges_report_the_exact_upgrade_direction_without_changing_state() {
    for (minimum, maximum, preferred, direction) in [
        ("0.0", "0.0", "0.0", "client"),
        ("0.2", "0.2", "0.2", "runtime"),
        ("65536.0", "65536.0", "65536.0", "runtime"),
    ] {
        let mut runtime = Runtime::default();
        let mut params = initialize_params(&["runtime.preview", "permission.read-only"]);
        params["protocol"] = json!({
            "minimum": minimum,
            "maximum": maximum,
            "preferred": preferred
        });
        let response = runtime.handle_line(&request("incompatible", "initialize", params));
        let data = &response[0]["error"]["data"];
        assert_eq!(response[0]["error"]["code"], -32003);
        assert_eq!(data["schema_version"], "initialize-error/0.1");
        assert_eq!(data["reason"], "protocol-range-not-overlapping");
        assert_eq!(data["client"]["minimum"], minimum);
        assert_eq!(data["client"]["maximum"], maximum);
        assert_eq!(data["runtime"]["minimum"], "0.1");
        assert_eq!(data["runtime"]["maximum"], "0.1");
        assert_eq!(data["upgrade_direction"], direction);

        let retry = initialize_with_id(
            &mut runtime,
            "retry",
            &["runtime.preview", "permission.read-only", "runtime.health"],
        );
        assert!(retry.get("result").is_some(), "{retry}");
    }
}

#[test]
fn initialize_fails_closed_without_the_backend_marker_and_read_only_permission() {
    let mut runtime = Runtime::default();
    let unknown_only = initialize(&mut runtime, &["future.valid-capability"]);
    assert_eq!(unknown_only["error"]["code"], -32006);

    let missing_permission = initialize_with_id(
        &mut runtime,
        "missing-permission",
        &["runtime.preview", "runtime.health"],
    );
    assert_eq!(missing_permission["error"]["code"], -32006);

    let accepted = initialize_with_id(
        &mut runtime,
        "accepted",
        &[
            "future.valid-capability",
            "runtime.preview",
            "permission.read-only",
        ],
    );
    assert!(accepted.get("result").is_some(), "{accepted}");
}

#[test]
fn initialize_validation_is_semantic_bounded_and_content_free() {
    let invalid_cases = [
        ("client", json!({"name": "Bad Client", "version": "1"})),
        ("platform", json!({"os": "other", "architecture": "arm64"})),
        (
            "limits",
            json!({"max_frame_bytes": MAX_AAP_FRAME_BYTES + 1}),
        ),
        (
            "transport_security",
            json!({
                "transport": "stdio", "local": false, "authenticated": false,
                "encrypted": false, "peer_verified": false
            }),
        ),
    ];
    for (field, invalid) in invalid_cases {
        let mut runtime = Runtime::default();
        let mut params = initialize_params(&["runtime.preview", "permission.read-only"]);
        params[field] = invalid;
        let response = runtime.handle_line(&request(field, "initialize", params));
        assert_eq!(response[0]["error"]["code"], -32602, "{response:?}");
    }
    let mut runtime = Runtime::default();
    let mut smaller_limit = initialize_params(&["runtime.preview", "permission.read-only"]);
    smaller_limit["limits"]["max_frame_bytes"] = json!(512);
    assert_eq!(
        runtime.handle_line(&request("small-limit", "initialize", smaller_limit))[0]["error"]
            ["code"],
        -32602
    );

    let mut runtime = Runtime::default();
    let mut experimental = initialize_params(&["runtime.preview", "permission.read-only"]);
    experimental["capabilities"]["experimental"] = json!(["future.experimental"]);
    assert_eq!(
        runtime.handle_line(&request("experimental", "initialize", experimental))[0]["error"]
            ["code"],
        -32602
    );

    let mut runtime = Runtime::default();
    let mut leading_zero = initialize_params(&["runtime.preview", "permission.read-only"]);
    leading_zero["protocol"]["minimum"] = json!("00.1");
    assert_eq!(
        runtime.handle_line(&request("leading-zero", "initialize", leading_zero))[0]["error"]
            ["code"],
        -32602
    );

    let mut runtime = Runtime::default();
    let mut secret = initialize_params(&["runtime.preview", "permission.read-only"]);
    secret["Authorization-Bearer-secret-value"] = json!(true);
    let response = runtime.handle_line(&request("secret", "initialize", secret));
    let encoded = response[0].to_string();
    assert_eq!(
        response[0]["error"]["message"],
        "initialize params are invalid"
    );
    assert!(!encoded.contains("Authorization"));
    assert!(!encoded.contains("secret-value"));
}

#[test]
fn strict_request_envelopes_do_not_treat_invalid_ids_as_notifications() {
    let mut runtime = Runtime::default();
    for invalid in [
        json!(null),
        json!(7),
        json!("contains space"),
        json!("unicode-"),
        json!("x".repeat(129)),
    ] {
        let envelope = json!({
            "jsonrpc": "2.0", "id": invalid, "method": "initialize",
            "params": initialize_params(&["runtime.preview", "permission.read-only"])
        });
        let response = runtime.handle_line(&envelope.to_string());
        assert_eq!(response[0]["id"], Value::Null);
        assert_eq!(response[0]["error"]["code"], -32600);
    }
    let wrong_envelope = json!({
        "jsonrpc": "2.0", "id": "wrong", "method": "initialize", "params": {},
        "result": {"forged": true}
    });
    assert_eq!(
        runtime.handle_line(&wrong_envelope.to_string())[0]["error"]["code"],
        -32600
    );
    let valid = initialize(&mut runtime, &["runtime.preview", "permission.read-only"]);
    assert!(valid.get("result").is_some(), "{valid}");
}

#[test]
fn method_gates_require_search_structured_and_pinned_context_capabilities() {
    let mut runtime = Runtime::default();
    ready(
        &mut runtime,
        &[
            "runtime.preview",
            "permission.read-only",
            "session.list",
            "timeline.streaming",
            "turn.context.inspect",
        ],
    );
    assert!(
        runtime.handle_line(&request("list", "session/list", json!({"limit": 10})))[0]
            .get("result")
            .is_some()
    );
    assert_eq!(
        runtime.handle_line(&request("search", "session/search", json!({})))[0]["error"]["code"],
        -32006
    );
    for method in ["turn/start", "turn/context/inspect"] {
        let structured = runtime.handle_line(&request(
            &format!("{method}-structured"),
            method,
            json!({"session_id": "missing", "context": [{"kind": "selection"}]}),
        ));
        assert_eq!(structured[0]["error"]["code"], -32006);
        let pinned = runtime.handle_line(&request(
            &format!("{method}-pinned"),
            method,
            json!({
                "session_id": "missing",
                "pinned_context_set_identity": "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "pinned_context_ids": ["pin-1"]
            }),
        ));
        assert_eq!(pinned[0]["error"]["code"], -32006);
    }
}

#[test]
fn notifications_have_no_response_and_cannot_mutate_business_state() {
    let mut runtime = Runtime::default();
    ready(
        &mut runtime,
        &[
            "runtime.preview",
            "permission.read-only",
            "session.chat",
            "session.list",
        ],
    );
    assert!(runtime
        .handle_line(&notification("session/start", json!({"mode": "chat"})))
        .is_empty());
    let sessions = runtime.handle_line(&request("list", "session/list", json!({"limit": 10})));
    assert_eq!(sessions[0]["result"]["sessions"], json!([]));

    let invalid_notification = json!({
        "jsonrpc": "1.0", "method": "session/start", "params": {"mode": "chat"}
    });
    assert!(runtime
        .handle_line(&invalid_notification.to_string())
        .is_empty());
}

#[test]
fn out_of_band_requests_obey_handshake_capabilities_and_the_fixed_frame_limit() {
    let mut runtime = Runtime::default();
    let control = runtime.control();
    let stop = request(
        "stop",
        "terminal/stop-user",
        json!({"session_id": "missing", "terminal_id": "missing"}),
    );
    assert!(control.handle_out_of_band_line(&stop).is_none());

    let params = initialize_params(&[
        "runtime.preview",
        "permission.read-only",
        "terminal.lifecycle.named",
        terminal_platform_capability(),
        "terminal.stop.out-of-band",
    ]);
    let initialized = runtime.handle_line(&request("initialize", "initialize", params));
    assert_eq!(
        initialized[0]["result"]["limits"]["max_frame_bytes"],
        MAX_AAP_FRAME_BYTES
    );
    runtime.handle_line(&notification("initialized", json!({})));

    let retried = control.handle_out_of_band_line(&stop).unwrap();
    assert_ne!(retried[0]["error"]["code"], -32001);
    assert_ne!(retried[0]["error"]["code"], -32002);
    assert_ne!(retried[0]["error"]["code"], -32006);

    let oversized_request = request(
        "oversized",
        "terminal/stop-user",
        json!({
            "session_id": "missing",
            "terminal_id": "missing",
            "padding": "x".repeat(usize::try_from(MAX_AAP_FRAME_BYTES).unwrap())
        }),
    );
    let oversized_response = control.handle_out_of_band_line(&oversized_request).unwrap();
    assert_eq!(oversized_response[0]["error"]["code"], -32005);
    assert!(oversized_response[0]["id"].is_null());
    let oversized_notification = notification(
        "terminal/stop-user",
        json!({
            "session_id": "missing",
            "terminal_id": "missing",
            "padding": "x".repeat(usize::try_from(MAX_AAP_FRAME_BYTES).unwrap())
        }),
    );
    let oversized_response = control
        .handle_out_of_band_line(&oversized_notification)
        .unwrap();
    assert_eq!(oversized_response[0]["error"]["code"], -32005);
    assert!(oversized_response[0]["id"].is_null());
}

#[test]
fn heartbeat_is_strict_negotiated_and_reachable_only_on_the_ready_control_path() {
    let heartbeat = request(
        "heartbeat-1",
        "runtime/heartbeat",
        json!({
            "schema_version": "runtime-heartbeat-request/0.1",
            "nonce": "client-nonce-1"
        }),
    );

    let mut runtime = Runtime::default();
    let control = runtime.control();
    assert!(control.handle_out_of_band_line(&heartbeat).is_none());

    ready(&mut runtime, &["runtime.preview", "permission.read-only"]);
    assert_eq!(
        control.handle_out_of_band_line(&heartbeat).unwrap()[0]["error"]["code"],
        -32006
    );

    let mut runtime = Runtime::unavailable("fixture unavailable");
    let initialized = ready(
        &mut runtime,
        &["runtime.unavailable", "runtime.heartbeat.out-of-band"],
    );
    assert!(initialized["result"]["capabilities"]["stable"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "runtime.heartbeat.out-of-band"));
    assert!(!initialized["result"]["capabilities"]["stable"]
        .as_array()
        .unwrap()
        .iter()
        .any(|capability| capability == "permission.read-only"));
    let control = runtime.control();
    let response = control.handle_out_of_band_line(&heartbeat).unwrap();
    assert_eq!(
        response[0],
        json!({
            "jsonrpc": "2.0",
            "id": "heartbeat-1",
            "result": {
                "schema_version": "runtime-heartbeat/0.1",
                "nonce": "client-nonce-1",
                "state": "alive"
            }
        })
    );
    assert_eq!(
        control.handle_out_of_band_line(&heartbeat).unwrap()[0]["error"]["code"],
        -32001
    );

    let normal_path = runtime.handle_line(&request(
        "heartbeat-normal-path",
        "runtime/heartbeat",
        json!({
            "schema_version": "runtime-heartbeat-request/0.1",
            "nonce": "normal-path"
        }),
    ));
    assert_eq!(normal_path[0]["error"]["code"], -32601);

    for (id, params) in [
        (
            "wrong-schema",
            json!({"schema_version": "runtime-heartbeat-request/0.2", "nonce": "nonce"}),
        ),
        (
            "empty-nonce",
            json!({"schema_version": "runtime-heartbeat-request/0.1", "nonce": ""}),
        ),
        (
            "long-nonce",
            json!({
                "schema_version": "runtime-heartbeat-request/0.1",
                "nonce": "x".repeat(129)
            }),
        ),
        (
            "extra-field",
            json!({
                "schema_version": "runtime-heartbeat-request/0.1",
                "nonce": "nonce",
                "store": "must-not-be-read"
            }),
        ),
    ] {
        let response = control
            .handle_out_of_band_line(&request(id, "runtime/heartbeat", params))
            .unwrap();
        assert_eq!(response[0]["error"]["code"], -32602, "{response:?}");
    }

    let boundary = control
        .handle_out_of_band_line(&request(
            "boundary-nonce",
            "runtime/heartbeat",
            json!({
                "schema_version": "runtime-heartbeat-request/0.1",
                "nonce": "x".repeat(128)
            }),
        ))
        .unwrap();
    assert_eq!(boundary[0]["result"]["nonce"], "x".repeat(128));

    assert_eq!(
        control
            .handle_out_of_band_line(&notification(
                "runtime/heartbeat",
                json!({
                    "schema_version": "runtime-heartbeat-request/0.1",
                    "nonce": "notification"
                }),
            ))
            .unwrap()[0]["error"]["code"],
        -32600
    );
    let null_id = json!({
        "jsonrpc": "2.0",
        "id": null,
        "method": "runtime/heartbeat",
        "params": {
            "schema_version": "runtime-heartbeat-request/0.1",
            "nonce": "null-id"
        }
    })
    .to_string();
    assert_eq!(
        control.handle_out_of_band_line(&null_id).unwrap()[0]["error"]["code"],
        -32600
    );
}

#[test]
fn oversized_library_frames_are_rejected_without_parsing_or_echoing_the_body() {
    let secret = "oversized-private-body-sentinel";
    let oversized = json!({
        "jsonrpc": "2.0",
        "id": "must-not-be-recovered",
        "method": "runtime/health",
        "params": {
            "secret": secret,
            "padding": "x".repeat(usize::try_from(MAX_AAP_FRAME_BYTES).unwrap())
        }
    })
    .to_string();
    let mut runtime = Runtime::default();
    let response = runtime.handle_line(&oversized);
    assert_eq!(response.len(), 1);
    assert!(response[0]["id"].is_null());
    assert_eq!(response[0]["error"]["code"], -32005);
    assert!(!response[0].to_string().contains(secret));

    let control_response = runtime.control().reject_oversized_line(&oversized).unwrap();
    assert_eq!(control_response.len(), 1);
    assert!(control_response[0]["id"].is_null());
    assert_eq!(control_response[0]["error"]["code"], -32005);
    assert!(!control_response[0].to_string().contains(secret));
}

#[test]
fn terminal_stop_requires_lifecycle_platform_and_out_of_band_capabilities() {
    let platform = terminal_platform_capability();
    let required = [
        "terminal.lifecycle.named",
        platform,
        "terminal.stop.out-of-band",
    ];
    for missing in required {
        let capabilities = [
            "runtime.preview",
            "permission.read-only",
            "terminal.lifecycle.named",
            platform,
            "terminal.stop.out-of-band",
        ]
        .into_iter()
        .filter(|capability| *capability != missing)
        .collect::<Vec<_>>();
        let mut runtime = Runtime::default();
        ready(&mut runtime, &capabilities);
        let stop = request(
            "stop",
            "terminal/stop-user",
            json!({"session_id": "missing", "terminal_id": "missing"}),
        );
        assert_eq!(runtime.handle_line(&stop)[0]["error"]["code"], -32006);
        assert_eq!(
            runtime.control().handle_out_of_band_line(&stop).unwrap()[0]["error"]["code"],
            -32006
        );
    }

    let mut runtime = Runtime::default();
    ready(
        &mut runtime,
        &[
            "runtime.preview",
            "permission.read-only",
            "terminal.lifecycle.named",
            platform,
            "terminal.stop.out-of-band",
        ],
    );
    let stop = request(
        "stop-complete",
        "terminal/stop-user",
        json!({"session_id": "missing", "terminal_id": "missing"}),
    );
    assert_ne!(runtime.handle_line(&stop)[0]["error"]["code"], -32006);
}

#[test]
fn stable_capability_registry_remains_within_initialize_bounds() {
    assert!(!STABLE_CAPABILITY_REGISTRY.is_empty());
    assert!(STABLE_CAPABILITY_REGISTRY.len() <= 128);
    assert_eq!(
        STABLE_CAPABILITY_REGISTRY
            .iter()
            .filter(|capability| **capability == TIMELINE_SUBSCRIPTION_CAPABILITY)
            .count(),
        1
    );
}

#[test]
fn timeline_subscription_routes_require_the_negotiated_capability() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!(
        "aegisy-handshake-timeline-subscription-gate-{unique}"
    ));
    fs::create_dir_all(&root).unwrap();

    let mut runtime = Runtime::with_store(&root).unwrap();
    let initialized = ready(
        &mut runtime,
        &[
            "runtime.preview",
            "permission.read-only",
            "timeline.replay.fixed-watermark",
            "timeline.snapshot.current",
        ],
    );
    let capabilities = initialized["result"]["capabilities"]["stable"]
        .as_array()
        .unwrap();
    assert!(!capabilities
        .iter()
        .any(|capability| capability == TIMELINE_SUBSCRIPTION_CAPABILITY));

    for (id, method) in TIMELINE_SUBSCRIPTION_REQUEST_ROUTES {
        let response = runtime.handle_line(&request(id, method, json!({})));
        assert_eq!(response.len(), 1);
        assert_eq!(response[0]["error"]["code"], -32006, "{response:?}");
    }

    fs::remove_dir_all(root).unwrap();
}

#[test]
fn healthy_store_advertises_subscription_routes_and_preserves_legacy_recovery_routes() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!(
        "aegisy-handshake-timeline-subscription-healthy-{unique}"
    ));
    fs::create_dir_all(&root).unwrap();

    let mut runtime = Runtime::with_store(&root).unwrap();
    let initialized = ready(
        &mut runtime,
        &[
            "runtime.preview",
            "permission.read-only",
            TIMELINE_SUBSCRIPTION_CAPABILITY,
            "timeline.replay.fixed-watermark",
            "timeline.snapshot.current",
        ],
    );
    let capabilities = initialized["result"]["capabilities"]["stable"]
        .as_array()
        .unwrap();
    for capability in [
        TIMELINE_SUBSCRIPTION_CAPABILITY,
        "timeline.replay.fixed-watermark",
        "timeline.snapshot.current",
    ] {
        assert!(
            capabilities.iter().any(|candidate| candidate == capability),
            "missing negotiated capability {capability}: {initialized}"
        );
    }

    for (id, method) in TIMELINE_SUBSCRIPTION_REQUEST_ROUTES {
        let response = runtime.handle_line(&request(id, method, json!({})));
        assert_eq!(response.len(), 1);
        assert_ne!(response[0]["error"]["code"], -32006, "{response:?}");
        assert_ne!(response[0]["error"]["code"], -32601, "{response:?}");
    }
    for (id, method) in [
        ("legacy-sync", "timeline/sync"),
        ("legacy-snapshot", "timeline/snapshot"),
    ] {
        let response = runtime.handle_line(&request(id, method, json!({})));
        assert_eq!(response.len(), 1);
        assert_ne!(response[0]["error"]["code"], -32006, "{response:?}");
        assert_ne!(response[0]["error"]["code"], -32601, "{response:?}");
    }

    fs::remove_dir_all(root).unwrap();
}

#[test]
fn store_recovery_mode_never_advertises_timeline_subscription() {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!(
        "aegisy-handshake-timeline-subscription-recovery-{unique}"
    ));
    fs::create_dir_all(&root).unwrap();
    fs::write(
        root.join("aegisy-workbench.sqlite3"),
        b"not-a-sqlite-database",
    )
    .unwrap();

    let mut runtime = Runtime::with_store(&root).unwrap();
    let initialized = ready(
        &mut runtime,
        &[
            "runtime.recovery.read-only",
            "permission.read-only",
            TIMELINE_SUBSCRIPTION_CAPABILITY,
            "timeline.replay.fixed-watermark",
            "timeline.snapshot.current",
        ],
    );
    assert_eq!(
        initialized["result"]["backend"]["status"],
        "read-only-recovery"
    );
    let capabilities = initialized["result"]["capabilities"]["stable"]
        .as_array()
        .unwrap();
    assert!(!capabilities
        .iter()
        .any(|capability| capability == TIMELINE_SUBSCRIPTION_CAPABILITY));

    for (id, method) in TIMELINE_SUBSCRIPTION_REQUEST_ROUTES {
        let response = runtime.handle_line(&request(id, method, json!({})));
        assert_eq!(response.len(), 1);
        assert_eq!(response[0]["error"]["code"], -32006, "{response:?}");
    }

    fs::remove_dir_all(root).unwrap();
}

#[test]
fn timeline_subscription_sync_keeps_its_fixed_head_and_activates_before_drain() {
    let root = timeline_subscription_test_root("timeline-subscription-fixed-head");
    let mut runtime = ready_timeline_subscription_runtime(&root);
    let session_id = start_chat_session(&mut runtime, "session-fixed-head");

    let subscription = subscribe(
        &mut runtime,
        "subscribe-fixed-head",
        &session_id,
        "subscription-fixed-head",
        1,
    );
    assert_eq!(subscription["state"], "sync-required");
    assert_eq!(subscription["next_method"], "timeline/subscription-sync");
    assert_eq!(subscription["watermark"]["sequence"], 0);
    let fixed_head = subscription["watermark"].clone();

    let buffered_turn = start_preview_turn(&mut runtime, "turn-buffered", &session_id);
    assert_eq!(buffered_turn.len(), 1, "{buffered_turn:?}");
    assert!(buffered_turn
        .iter()
        .all(|message| message["method"] != "event"));

    let sync = subscription_sync(&mut runtime, "sync-fixed-head", &subscription);
    assert_eq!(sync.len(), 1, "{sync:?}");
    assert_eq!(sync[0]["result"]["watermark"], fixed_head);
    assert!(sync[0]["result"]["events"].as_array().unwrap().is_empty());
    assert_eq!(sync[0]["result"]["complete"], true);
    assert!(sync[0]["result"]["events"]
        .as_array()
        .unwrap()
        .iter()
        .all(|event| event["sequence"].as_u64().unwrap() == 0));

    let activation = activate_subscription(&mut runtime, "activate-fixed-head", &subscription);
    assert_eq!(activation.len(), 7, "{activation:?}");
    assert_eq!(activation[0]["id"], "activate-fixed-head");
    assert_eq!(activation[0]["result"]["state"], "active");
    assert_eq!(activation[0]["result"]["cursor"], fixed_head);
    for (index, message) in activation[1..].iter().enumerate() {
        assert_eq!(message["method"], "timeline/subscription-event");
        assert_eq!(
            message["params"]["schema_version"],
            "timeline-subscription-event/0.1"
        );
        assert_eq!(message["params"]["session_id"], session_id);
        assert_eq!(
            message["params"]["subscription_id"],
            "subscription-fixed-head"
        );
        assert_eq!(message["params"]["event"]["sequence"], 1 + index as u64);
    }
    assert!(activation
        .iter()
        .all(|message| message["method"] != "event"));

    let live_turn = start_preview_turn(&mut runtime, "turn-live", &session_id);
    assert_eq!(live_turn.len(), 7, "{live_turn:?}");
    assert!(live_turn[1..]
        .iter()
        .all(|message| message["method"] == "timeline/subscription-event"));
    assert!(live_turn.iter().all(|message| message["method"] != "event"));

    drop(runtime);
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn timeline_subscription_buffers_and_drains_each_session_in_isolation() {
    let root = timeline_subscription_test_root("timeline-subscription-isolation");
    let mut runtime = ready_timeline_subscription_runtime(&root);
    let session_a = start_chat_session(&mut runtime, "session-a");
    let session_b = start_chat_session(&mut runtime, "session-b");

    let subscription_a = subscribe(&mut runtime, "subscribe-a", &session_a, "subscription-a", 9);
    assert_eq!(subscription_a["watermark"]["sequence"], 0);
    let subscription_b = subscribe(&mut runtime, "subscribe-b", &session_b, "subscription-b", 9);

    assert_eq!(
        start_preview_turn(&mut runtime, "turn-a-buffered", &session_a).len(),
        1
    );
    assert_eq!(
        subscription_sync(&mut runtime, "sync-a", &subscription_a).len(),
        1
    );
    assert_eq!(
        subscription_sync(&mut runtime, "sync-b", &subscription_b).len(),
        1
    );

    let activation_b = activate_subscription(&mut runtime, "activate-b", &subscription_b);
    assert_eq!(activation_b.len(), 1, "{activation_b:?}");
    assert_eq!(activation_b[0]["result"]["session_id"], session_b);

    let activation_a = activate_subscription(&mut runtime, "activate-a", &subscription_a);
    assert_eq!(activation_a.len(), 7, "{activation_a:?}");
    assert!(activation_a[1..].iter().all(|message| {
        message["method"] == "timeline/subscription-event"
            && message["params"]["session_id"] == session_a
            && message["params"]["subscription_id"] == "subscription-a"
    }));
    assert!(activation_a[1..]
        .iter()
        .all(|message| message["params"]["session_id"] != session_b));

    let live_b = start_preview_turn(&mut runtime, "turn-b-live", &session_b);
    assert_eq!(live_b.len(), 7, "{live_b:?}");
    assert!(live_b[1..].iter().all(|message| {
        message["method"] == "timeline/subscription-event"
            && message["params"]["session_id"] == session_b
            && message["params"]["subscription_id"] == "subscription-b"
    }));
    assert!(live_b[1..]
        .iter()
        .all(|message| message["params"]["session_id"] != session_a));

    drop(runtime);
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn timeline_subscription_rejects_cross_bound_requests_without_retiring_the_owner() {
    let root = timeline_subscription_test_root("timeline-subscription-failure-cleanup");
    let mut runtime = ready_timeline_subscription_runtime(&root);
    let session_a = start_chat_session(&mut runtime, "failure-session-a");
    let session_b = start_chat_session(&mut runtime, "failure-session-b");
    let session_c = start_chat_session(&mut runtime, "failure-session-c");
    let subscription = subscribe(
        &mut runtime,
        "failure-subscribe-a",
        &session_a,
        "failure-subscription",
        17,
    );
    assert_eq!(subscription["watermark"]["sequence"], 0);
    assert_eq!(
        start_preview_turn(&mut runtime, "failure-turn-a", &session_a).len(),
        1
    );

    let stale = runtime.handle_line(&request(
        "stale-generation",
        "timeline/subscribe",
        json!({
            "schema_version": "timeline-subscribe-request/0.1",
            "connection_generation": 18,
            "session_id": session_b,
            "subscription_id": "stale-subscription",
            "cursor": {"sequence": 0, "event_id": null},
            "watermark": null
        }),
    ));
    assert_eq!(stale[0]["error"]["code"], -32151);
    assert_eq!(stale[0]["error"]["data"]["stage"], "subscribe");
    assert_eq!(
        stale[0]["error"]["data"]["reason"],
        "connection-generation-invalid"
    );
    assert_eq!(stale[0]["error"]["data"]["connection_generation"], 18);
    assert_eq!(stale[0]["error"]["data"]["session_id"], session_b);
    assert_eq!(
        stale[0]["error"]["data"]["subscription_id"],
        "stale-subscription"
    );
    assert_eq!(stale[0]["error"]["data"]["cleanup_required"], true);
    assert!(stale[0]["error"]["data"]["request_identity"]
        .as_str()
        .is_some_and(|identity| identity.starts_with("timeline-subscription-request:sha256:")));

    let forged_binding = runtime.handle_line(&request(
        "forged-binding",
        "timeline/subscription-sync",
        json!({
            "schema_version": "timeline-subscription-sync-request/0.1",
            "connection_generation": 17,
            "session_id": session_b,
            "subscription_id": "failure-subscription",
            "request": {
                "session_id": session_b,
                "after": subscription["cursor"],
                "watermark": subscription["watermark"],
                "limit": 200
            }
        }),
    ));
    assert_eq!(forged_binding[0]["error"]["code"], -32151);
    assert_eq!(forged_binding[0]["error"]["data"]["stage"], "sync");
    assert_eq!(
        forged_binding[0]["error"]["data"]["reason"],
        "subscription-context-drift"
    );
    assert_eq!(forged_binding[0]["error"]["data"]["session_id"], session_b);
    assert_eq!(
        forged_binding[0]["error"]["data"]["subscription_id"],
        "failure-subscription"
    );
    assert_eq!(forged_binding[0]["error"]["data"]["cleanup_required"], true);
    assert_eq!(
        forged_binding[0]["error"]["data"]["cursor"],
        subscription["cursor"]
    );
    assert_eq!(
        forged_binding[0]["error"]["data"]["watermark"],
        subscription["watermark"]
    );
    assert_eq!(forged_binding[0]["error"]["data"]["retryable"], false);
    assert!(forged_binding[0]["error"]["data"]["request_identity"]
        .as_str()
        .is_some_and(|identity| identity.starts_with("timeline-subscription-request:sha256:")));

    let forged_snapshot = runtime.handle_line(&request(
        "forged-snapshot-binding",
        "timeline/subscription-snapshot",
        json!({
            "schema_version": "timeline-subscription-snapshot-request/0.1",
            "connection_generation": 17,
            "session_id": session_b,
            "subscription_id": "failure-subscription",
            "request": {
                "session_id": session_b,
                "snapshot_identity": null,
                "watermark": null,
                "after": null,
                "limit": 200
            }
        }),
    ));
    assert_eq!(forged_snapshot[0]["error"]["code"], -32151);
    assert_eq!(
        forged_snapshot[0]["error"]["data"]["reason"],
        "subscription-context-drift"
    );
    assert_eq!(forged_snapshot[0]["error"]["data"]["session_id"], session_b);

    let stale_activation = runtime.handle_line(&request(
        "stale-activation-generation",
        "timeline/subscription-activate",
        json!({
            "schema_version": "timeline-subscription-activate-request/0.1",
            "connection_generation": 18,
            "session_id": session_a,
            "subscription_id": "failure-subscription",
            "source": "sync",
            "cursor": subscription["watermark"],
            "watermark": subscription["watermark"],
            "snapshot_identity": null
        }),
    ));
    assert_eq!(stale_activation[0]["error"]["code"], -32151);
    assert_eq!(
        stale_activation[0]["error"]["data"]["reason"],
        "subscription-context-drift"
    );
    assert_eq!(
        stale_activation[0]["error"]["data"]["connection_generation"],
        18
    );

    let owner_sync = subscription_sync(&mut runtime, "owner-sync-after-forgery", &subscription);
    assert_eq!(owner_sync.len(), 1, "{owner_sync:?}");
    assert_eq!(owner_sync[0]["result"]["complete"], true);
    let owner_activation =
        activate_subscription(&mut runtime, "owner-activate-after-forgery", &subscription);
    assert_eq!(owner_activation.len(), 7, "{owner_activation:?}");
    assert_eq!(owner_activation[0]["result"]["session_id"], session_a);
    assert!(owner_activation[1..].iter().all(|message| {
        message["method"] == "timeline/subscription-event"
            && message["params"]["session_id"] == session_a
            && message["params"]["subscription_id"] == "failure-subscription"
    }));

    let reused = runtime.handle_line(&request(
        "reuse-forged-id",
        "timeline/subscribe",
        json!({
            "schema_version": "timeline-subscribe-request/0.1",
            "connection_generation": 17,
            "session_id": session_a,
            "subscription_id": "failure-subscription",
            "cursor": {"sequence": 0, "event_id": null},
            "watermark": null
        }),
    ));
    assert_eq!(reused[0]["error"]["code"], -32151);
    assert_eq!(
        reused[0]["error"]["data"]["reason"],
        "subscription-id-reused"
    );

    let bootstrap_b = subscribe(
        &mut runtime,
        "bootstrap-subscribe-b",
        &session_b,
        "bootstrap-subscription-b",
        17,
    );
    assert_eq!(
        start_preview_turn(&mut runtime, "bootstrap-turn-b", &session_b).len(),
        1
    );
    let bound_route_mismatch = runtime.handle_line(&request(
        "bound-route-mismatch-b",
        "timeline/subscription-snapshot",
        json!({
            "schema_version": "timeline-subscription-snapshot-request/0.1",
            "connection_generation": 17,
            "session_id": session_b,
            "subscription_id": bootstrap_b["subscription_id"],
            "request": {
                "session_id": session_b,
                "snapshot_identity": null,
                "watermark": null,
                "after": null,
                "limit": 200
            }
        }),
    ));
    assert_eq!(bound_route_mismatch[0]["error"]["code"], -32151);
    assert_eq!(
        bound_route_mismatch[0]["error"]["data"]["reason"],
        "subscription-context-drift"
    );

    let replacement = subscribe(
        &mut runtime,
        "replacement-subscribe",
        &session_b,
        "replacement-subscription",
        17,
    );

    let failed_sync = runtime.handle_line(&request(
        "failed-sync",
        "timeline/subscription-sync",
        json!({
            "schema_version": "timeline-subscription-sync-request/0.1",
            "connection_generation": 17,
            "session_id": session_b,
            "subscription_id": "replacement-subscription",
            "request": {
                "session_id": session_b,
                "after": {
                    "sequence": 1,
                    "event_id": "event:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                },
                "watermark": replacement["watermark"],
                "limit": 200
            }
        }),
    ));
    assert_eq!(failed_sync[0]["error"]["code"], -32151);
    let failure = &failed_sync[0]["error"]["data"];
    assert_eq!(
        failure["schema_version"],
        "timeline-subscription-failure/0.1"
    );
    assert_eq!(failure["connection_generation"], 17);
    assert_eq!(failure["session_id"], session_b);
    assert_eq!(failure["subscription_id"], "replacement-subscription");
    assert_eq!(failure["state"], "failed");
    assert_eq!(failure["stage"], "sync");
    assert_eq!(failure["cursor"]["sequence"], 1);
    assert_eq!(failure["watermark"], replacement["watermark"]);
    assert_eq!(failure["reason"], "sync-page-unavailable");
    assert_eq!(failure["retryable"], true);
    assert_eq!(failure["cleanup_required"], true);
    assert!(failure["request_identity"]
        .as_str()
        .is_some_and(|identity| identity.starts_with("timeline-subscription-request:sha256:")));

    let post_failure = subscribe(
        &mut runtime,
        "post-failure-subscribe",
        &session_b,
        "post-failure-subscription",
        17,
    );
    assert_eq!(post_failure["state"], "sync-required");
    let session_c_subscription = subscribe(
        &mut runtime,
        "current-generation-subscribe",
        &session_c,
        "current-generation-subscription",
        17,
    );
    assert_eq!(session_c_subscription["session_id"], session_c);

    drop(runtime);
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn timeline_subscription_disconnect_drops_connection_owned_id_history() {
    let root = timeline_subscription_test_root("timeline-subscription-disconnect");
    let session_id = {
        let mut runtime = ready_timeline_subscription_runtime(&root);
        let session_id = start_chat_session(&mut runtime, "disconnect-session");
        let subscription = subscribe(
            &mut runtime,
            "disconnect-subscribe",
            &session_id,
            "connection-local-subscription",
            31,
        );
        assert_eq!(subscription["state"], "sync-required");
        session_id
    };

    let mut reconnected = ready_timeline_subscription_runtime(&root);
    let subscription = subscribe(
        &mut reconnected,
        "reconnect-subscribe",
        &session_id,
        "connection-local-subscription",
        32,
    );
    assert_eq!(subscription["connection_generation"], 32);
    assert_eq!(subscription["session_id"], session_id);

    drop(reconnected);
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn saturated_queue_classification_preserves_strict_envelope_semantics() {
    let runtime = Runtime::default();
    let control = runtime.control();
    let null_id = json!({
        "jsonrpc": "2.0", "id": null, "method": "runtime/health", "params": {}
    });
    let response = control.overload_response(&null_id.to_string()).unwrap();
    assert_eq!(response["id"], Value::Null);
    assert_eq!(response["error"]["code"], -32600);

    let wrong_version = json!({
        "jsonrpc": "1.0", "id": "wrong-version", "method": "runtime/health", "params": {}
    });
    assert_eq!(
        control
            .overload_response(&wrong_version.to_string())
            .unwrap()["error"]["code"],
        -32600
    );
    let valid = request("overload", "runtime/health", json!({}));
    assert_eq!(
        control.overload_response(&valid).unwrap()["error"]["code"],
        -32004
    );
    assert!(control
        .overload_response(&notification("runtime/health", json!({})))
        .is_none());
}
