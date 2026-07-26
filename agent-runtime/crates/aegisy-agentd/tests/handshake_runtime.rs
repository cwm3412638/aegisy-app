use aegisy_aap::MAX_AAP_FRAME_BYTES;
use aegisy_agentd::{Runtime, STABLE_CAPABILITY_REGISTRY};
use serde_json::{json, Value};

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
