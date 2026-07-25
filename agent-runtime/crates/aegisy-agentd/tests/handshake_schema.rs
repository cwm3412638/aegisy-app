use aegisy_aap::stable::v0_1::{
    timeline_event_id, EventEnvelope, InitializeParams, InitializeResult, ItemUpdate,
    RuntimeHeartbeatParams, RuntimeHeartbeatResult, TimelineItem, TimelineRetentionGapData,
    TimelineSessionSnapshotPage, TimelineSnapshotParams, TimelineSyncPage, TimelineSyncParams,
    TurnState,
};
use serde_json::{json, Map, Value};
use std::collections::HashSet;
use std::fs;
use std::sync::OnceLock;

fn schema_path() -> String {
    format!(
        "{}/../../aap-schema/stable/v0.1/aap.schema.json",
        env!("CARGO_MANIFEST_DIR")
    )
}

fn fixture_path(name: &str) -> String {
    format!(
        "{}/../../aap-schema/fixtures/{name}",
        env!("CARGO_MANIFEST_DIR")
    )
}

fn guide_path() -> String {
    format!(
        "{}/../../../docs/AAP-PROTOCOL-GUIDE.md",
        env!("CARGO_MANIFEST_DIR")
    )
}

fn fixture_messages(name: &str) -> Vec<Value> {
    fs::read_to_string(fixture_path(name))
        .unwrap()
        .lines()
        .map(|line| serde_json::from_str(line).unwrap())
        .collect()
}

fn guide_timeline_messages() -> Vec<Value> {
    fs::read_to_string(guide_path())
        .unwrap()
        .lines()
        .filter_map(|line| serde_json::from_str::<Value>(line).ok())
        .filter(|message| {
            message["method"] == "event"
                && message["params"]["schema_version"] == "timeline-event/0.1"
        })
        .collect()
}

fn guide_timeline_sync_messages() -> Vec<Value> {
    fs::read_to_string(guide_path())
        .unwrap()
        .lines()
        .filter_map(|line| serde_json::from_str::<Value>(line).ok())
        .filter(|message| {
            message["method"] == "timeline/sync"
                || message["result"]["schema_version"] == "timeline-sync-page/0.1"
        })
        .collect()
}

fn has_exact_keys(object: &Map<String, Value>, expected: &[&str]) -> bool {
    object.len() == expected.len() && expected.iter().all(|key| object.contains_key(*key))
}

fn schema_validator() -> &'static jsonschema::Validator {
    static VALIDATOR: OnceLock<jsonschema::Validator> = OnceLock::new();
    VALIDATOR.get_or_init(|| {
        let schema: Value =
            serde_json::from_str(&fs::read_to_string(schema_path()).unwrap()).unwrap();
        jsonschema::validator_for(&schema).expect("AAP 0.1 schema must compile")
    })
}

fn strict_envelope_valid(message: &Value) -> bool {
    schema_validator().is_valid(message)
}

fn schema_definition_valid(name: &str, value: &Value) -> bool {
    let schema: Value = serde_json::from_str(&fs::read_to_string(schema_path()).unwrap()).unwrap();
    let document = json!({
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$ref": format!("#/$defs/{name}"),
        "$defs": schema["$defs"].clone()
    });
    jsonschema::validator_for(&document)
        .expect("AAP definition schema must compile")
        .is_valid(value)
}

fn valid_method_name(method: &str) -> bool {
    method.split('/').all(|segment| {
        let mut bytes = segment.bytes();
        bytes.next().is_some_and(|byte| byte.is_ascii_alphabetic())
            && bytes.all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-'))
    })
}

fn valid_protocol_version_shape(version: &str) -> bool {
    fn valid_segment(segment: &str) -> bool {
        !segment.is_empty()
            && segment.bytes().all(|byte| byte.is_ascii_digit())
            && (segment == "0" || !segment.starts_with('0'))
    }

    version.len() <= 16
        && version
            .split_once('.')
            .is_some_and(|(major, minor)| valid_segment(major) && valid_segment(minor))
}

fn strict_stdio_security(value: &Value) -> bool {
    let Some(object) = value.as_object() else {
        return false;
    };
    has_exact_keys(
        object,
        &[
            "transport",
            "local",
            "authenticated",
            "encrypted",
            "peer_verified",
        ],
    ) && object["transport"] == "stdio"
        && object["local"] == true
        && object["authenticated"] == false
        && object["encrypted"] == false
        && object["peer_verified"] == false
}

fn strict_capabilities(value: &Value) -> bool {
    let Some(object) = value.as_object() else {
        return false;
    };
    if !has_exact_keys(object, &["stable", "experimental"]) {
        return false;
    }
    let Some(stable) = object["stable"].as_array() else {
        return false;
    };
    let Some(experimental) = object["experimental"].as_array() else {
        return false;
    };
    let names = stable.iter().filter_map(Value::as_str).collect::<Vec<_>>();
    !names.is_empty()
        && names.len() == stable.len()
        && names.len() <= 128
        && names.iter().all(|name| valid_capability_name(name))
        && names.iter().copied().collect::<HashSet<_>>().len() == names.len()
        && experimental.is_empty()
}

fn valid_capability_name(name: &str) -> bool {
    let mut needs_segment = true;
    for byte in name.bytes() {
        if byte.is_ascii_lowercase() || byte.is_ascii_digit() {
            needs_segment = false;
        } else if (byte == b'.' || byte == b'-') && !needs_segment {
            needs_segment = true;
        } else {
            return false;
        }
    }
    !name.is_empty() && name.len() <= 128 && !needs_segment
}

fn assert_content_free(value: &Value) {
    match value {
        Value::Object(object) => {
            for (key, child) in object {
                let key = key.to_ascii_lowercase();
                assert!(![
                    "authorization",
                    "api_key",
                    "access_token",
                    "refresh_token",
                    "password",
                    "cookie",
                ]
                .iter()
                .any(|forbidden| key.contains(forbidden)));
                assert_content_free(child);
            }
        }
        Value::Array(array) => array.iter().for_each(assert_content_free),
        Value::String(string) => {
            let value = string.to_ascii_lowercase();
            assert!(!value.contains("bearer "));
            assert!(!value.contains("sk-"));
            assert!(!value.contains("ghp_"));
            assert!(!value.contains("github_pat_"));
        }
        _ => {}
    }
}

fn expected_timeline_event_id(event: &EventEnvelope) -> String {
    timeline_event_id(
        &event.schema_version,
        event.sequence,
        event.timestamp_ms,
        &event.correlation_id,
        &event.session_id,
        &event.turn_id,
        event.turn_state,
        &event.event,
        &event.item,
        &event.item_update,
    )
    .unwrap()
}

fn reseal_timeline_event(message: &mut Value) {
    let params = &message["params"];
    let item = (!params["item"].is_null())
        .then(|| serde_json::from_value::<TimelineItem>(params["item"].clone()).unwrap());
    let item_update = (!params["item_update"].is_null())
        .then(|| serde_json::from_value::<ItemUpdate>(params["item_update"].clone()).unwrap());
    let event_id = timeline_event_id(
        params["schema_version"].as_str().unwrap(),
        params["sequence"].as_u64().unwrap(),
        params["timestamp_ms"].as_u64().unwrap(),
        params["correlation_id"].as_str().unwrap(),
        params["session_id"].as_str().unwrap(),
        params["turn_id"].as_str().unwrap(),
        serde_json::from_value::<TurnState>(params["turn_state"].clone()).unwrap(),
        params["event"].as_str().unwrap(),
        &item,
        &item_update,
    )
    .unwrap();
    message["params"]["event_id"] = json!(event_id);
}

#[test]
fn stable_schema_defines_strict_handshake_and_json_rpc_envelopes() {
    let schema: Value = serde_json::from_str(&fs::read_to_string(schema_path()).unwrap()).unwrap();
    assert_eq!(schema["additionalProperties"], false);
    assert_eq!(schema["oneOf"].as_array().map(Vec::len), Some(4));
    assert_eq!(schema["properties"]["jsonrpc"]["const"], "2.0");

    let definitions = schema["$defs"].as_object().unwrap();
    for required in [
        "initializeParams",
        "initializeResult",
        "initializeRequest",
        "initializeSuccessResponse",
        "initializedNotification",
        "initializeIncompatibleData",
        "initializeIncompatibleErrorResponse",
        "runtimeHeartbeatParams",
        "runtimeHeartbeatResult",
        "runtimeHeartbeatRequest",
        "runtimeHeartbeatSuccessResponse",
        "safePositiveInteger",
        "safeNonNegativeInteger",
        "boundedGraphicalId",
        "timelineEventId",
        "timelineAnchor",
        "timelineEventName",
        "timelineItem",
        "timelineItemUpdate",
        "timelineEvent",
        "timelineSyncParams",
        "timelineSyncPage",
        "timelineSyncRequest",
        "timelineSyncSuccessResponse",
        "timelineRetentionGapData",
        "timelineSyncRetentionGapErrorResponse",
    ] {
        assert!(
            definitions.contains_key(required),
            "missing $defs.{required}"
        );
    }
    for strict_object in [
        "protocolRange",
        "protocolPreference",
        "negotiatedProtocol",
        "identity",
        "platform",
        "capabilities",
        "limits",
        "stdioTransportSecurity",
        "backend",
        "initializeParams",
        "initializeResult",
        "initializeRequest",
        "initializeSuccessResponse",
        "initializedNotification",
        "initializeIncompatibleData",
        "initializeIncompatibleErrorResponse",
        "runtimeHeartbeatParams",
        "runtimeHeartbeatResult",
        "runtimeHeartbeatRequest",
        "runtimeHeartbeatSuccessResponse",
        "timelineItem",
        "timelineItemUpdate",
        "timelineEvent",
        "timelineAnchor",
        "timelineSyncParams",
        "timelineSyncPage",
        "timelineSyncRequest",
        "timelineSyncSuccessResponse",
        "timelineRetentionGapData",
        "timelineSyncRetentionGapErrorResponse",
    ] {
        assert_eq!(
            definitions[strict_object]["additionalProperties"], false,
            "$defs.{strict_object} is not closed"
        );
    }
    assert_eq!(
        definitions["capabilities"]["properties"]["stable"]["minItems"],
        1
    );
    assert_eq!(
        definitions["capabilities"]["properties"]["experimental"]["maxItems"],
        0
    );
    assert_eq!(definitions["jsonRpcRequestId"]["type"], "string");
    assert_eq!(definitions["jsonRpcRequestId"]["pattern"], r"^[!-~]+$");
    assert_eq!(definitions["jsonRpcRequestId"]["maxLength"], 128);
    assert_eq!(schema["properties"]["method"]["maxLength"], 128);
    assert_eq!(
        schema["properties"]["method"]["pattern"],
        r"^[A-Za-z][A-Za-z0-9.-]*(?:/[A-Za-z][A-Za-z0-9.-]*)*$"
    );
    assert_eq!(
        definitions["identity"]["properties"]["name"]["maxLength"],
        64
    );
    assert_eq!(
        definitions["identity"]["properties"]["version"]["maxLength"],
        64
    );
    assert_eq!(
        definitions["identity"]["properties"]["name"]["pattern"],
        r"^[a-z0-9]+(?:[.-][a-z0-9]+)*$"
    );
    assert_eq!(
        definitions["identity"]["properties"]["version"]["pattern"],
        r"^[!-~]+$"
    );
    assert_eq!(
        definitions["backend"]["properties"]["adapter"]["pattern"],
        r"^[a-z0-9]+(?:[.-][a-z0-9]+)*$"
    );
    assert_eq!(
        definitions["backend"]["properties"]["version"]["pattern"],
        r"^[!-~]+$"
    );
    assert_eq!(definitions["capabilityName"]["maxLength"], 128);
    assert_eq!(
        definitions["limits"]["properties"]["max_frame_bytes"]["const"],
        4 * 1024 * 1024
    );
    assert_eq!(
        definitions["protocolVersion"]["pattern"],
        r"^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)$"
    );
    assert_eq!(definitions["protocolVersion"]["minLength"], 3);
    assert_eq!(definitions["protocolVersion"]["maxLength"], 16);
    assert_eq!(
        definitions["negotiatedProtocol"]["properties"]["upgrade_direction"]["const"],
        "none"
    );
    assert_eq!(
        definitions["stdioTransportSecurity"]["properties"]["transport"]["const"],
        "stdio"
    );
    for denied in ["authenticated", "encrypted", "peer_verified"] {
        assert_eq!(
            definitions["stdioTransportSecurity"]["properties"][denied]["const"],
            false
        );
    }
}

#[test]
fn protocol_version_schema_shape_allows_future_major_without_leading_zeroes() {
    let schema: Value = serde_json::from_str(&fs::read_to_string(schema_path()).unwrap()).unwrap();
    let protocol_version = &schema["$defs"]["protocolVersion"];

    assert_eq!(protocol_version["type"], "string");
    assert_eq!(
        protocol_version["pattern"],
        r"^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)$"
    );
    assert_eq!(protocol_version["minLength"], 3);
    assert_eq!(protocol_version["maxLength"], 16);
    assert!(valid_protocol_version_shape("0.1"));
    assert!(valid_protocol_version_shape("1.0"));
    assert!(valid_protocol_version_shape("65536.0"));
    assert!(!valid_protocol_version_shape("01.0"));
    assert!(!valid_protocol_version_shape("1.00"));
    assert!(!valid_protocol_version_shape("1"));
}

#[test]
fn compatible_handshake_fixture_matches_types_intersection_limits_and_security() {
    let messages = fixture_messages("aap-initialize-compatible.jsonl");
    assert_eq!(messages.len(), 3);
    messages.iter().for_each(|message| {
        assert!(strict_envelope_valid(message));
        assert_content_free(message);
    });

    let request = messages[0].as_object().unwrap();
    assert!(has_exact_keys(
        request,
        &["jsonrpc", "id", "method", "params"]
    ));
    assert_eq!(request["method"], "initialize");
    let params: InitializeParams = serde_json::from_value(request["params"].clone()).unwrap();
    assert_eq!(params.protocol.minimum, "0.1");
    assert_eq!(params.protocol.maximum, "0.1");
    assert_eq!(params.protocol.preferred, "0.1");
    assert!(strict_capabilities(&request["params"]["capabilities"]));
    assert!(strict_stdio_security(
        &request["params"]["transport_security"]
    ));

    let response = messages[1].as_object().unwrap();
    assert!(has_exact_keys(response, &["jsonrpc", "id", "result"]));
    assert_eq!(response["id"], request["id"]);
    let result: InitializeResult = serde_json::from_value(response["result"].clone()).unwrap();
    assert_eq!(result.protocol.minimum, "0.1");
    assert_eq!(result.protocol.maximum, "0.1");
    assert_eq!(result.protocol.selected, "0.1");
    assert_eq!(result.protocol.upgrade_direction, "none");
    assert!(strict_capabilities(&response["result"]["capabilities"]));
    assert!(strict_stdio_security(
        &response["result"]["transport_security"]
    ));

    let requested = request["params"]["capabilities"]["stable"]
        .as_array()
        .unwrap()
        .iter()
        .cloned()
        .collect::<HashSet<_>>();
    assert!(response["result"]["capabilities"]["stable"]
        .as_array()
        .unwrap()
        .iter()
        .all(|capability| requested.contains(capability)));
    let negotiated = response["result"]["capabilities"]["stable"]
        .as_array()
        .unwrap();
    assert!(negotiated
        .iter()
        .any(|capability| capability == "runtime.preview"));
    assert!(negotiated
        .iter()
        .any(|capability| capability == "permission.read-only"));
    assert_eq!(params.limits.max_frame_bytes, 4 * 1024 * 1024);
    assert_eq!(result.limits.max_frame_bytes, params.limits.max_frame_bytes);
    assert_eq!(
        messages[2],
        json!({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    );
}

#[test]
fn runtime_heartbeat_fixture_is_an_exact_content_free_nonce_round_trip() {
    let messages = fixture_messages("aap-runtime-heartbeat.jsonl");
    assert_eq!(messages.len(), 2);
    messages.iter().for_each(|message| {
        assert!(strict_envelope_valid(message));
        assert_content_free(message);
    });
    assert!(schema_definition_valid(
        "runtimeHeartbeatRequest",
        &messages[0]
    ));
    assert!(schema_definition_valid(
        "runtimeHeartbeatSuccessResponse",
        &messages[1]
    ));

    let request: RuntimeHeartbeatParams =
        serde_json::from_value(messages[0]["params"].clone()).unwrap();
    let result: RuntimeHeartbeatResult =
        serde_json::from_value(messages[1]["result"].clone()).unwrap();
    result.validate_for_request(&request).unwrap();
    assert_eq!(
        serde_json::to_value(&request).unwrap(),
        messages[0]["params"]
    );
    assert_eq!(
        serde_json::to_value(&result).unwrap(),
        messages[1]["result"]
    );

    let mut extra_request = messages[0].clone();
    extra_request["params"]["permission"] = json!("read-only");
    assert!(!strict_envelope_valid(&extra_request));

    let mut notification = messages[0].clone();
    notification.as_object_mut().unwrap().remove("id");
    assert!(!strict_envelope_valid(&notification));

    let mut extra_result = messages[1].clone();
    extra_result["result"]["timestamp_ms"] = json!(1);
    assert!(!schema_definition_valid(
        "runtimeHeartbeatSuccessResponse",
        &extra_result
    ));

    let mut wrong_state = messages[1].clone();
    wrong_state["result"]["state"] = json!("ready");
    assert!(!schema_definition_valid(
        "runtimeHeartbeatSuccessResponse",
        &wrong_state
    ));
}

#[test]
fn incompatible_fixture_carries_bounded_upgrade_direction_without_session_data() {
    let messages = fixture_messages("aap-initialize-incompatible.jsonl");
    assert_eq!(messages.len(), 2);
    messages.iter().for_each(|message| {
        assert!(strict_envelope_valid(message));
        assert_content_free(message);
    });

    let params = &messages[0]["params"];
    assert!(strict_capabilities(&params["capabilities"]));
    assert!(strict_stdio_security(&params["transport_security"]));
    let error = messages[1]["error"].as_object().unwrap();
    assert!(has_exact_keys(error, &["code", "message", "data"]));
    assert_eq!(error["code"], -32003);
    assert!(error["message"]
        .as_str()
        .is_some_and(|message| !message.is_empty() && message.len() <= 256));
    let data = error["data"].as_object().unwrap();
    assert!(has_exact_keys(
        data,
        &[
            "schema_version",
            "reason",
            "client",
            "runtime",
            "upgrade_direction",
        ]
    ));
    assert_eq!(data["schema_version"], "initialize-error/0.1");
    assert_eq!(data["reason"], "protocol-range-not-overlapping");
    assert_eq!(data["upgrade_direction"], "runtime");
    assert!(messages.iter().all(|message| {
        let rendered = message.to_string();
        !rendered.contains("session_id")
            && !rendered.contains("project_id")
            && !rendered.contains("root")
    }));
}

#[test]
fn strict_handshake_contract_rejects_legacy_empty_experimental_and_envelope_drift() {
    let compatible = fixture_messages("aap-initialize-compatible.jsonl");
    let mut request = compatible[0].clone();

    let mut legacy = request.clone();
    legacy["params"] = json!({
        "protocol_version": "0.1",
        "client": {"name": "aegisy-client", "version": "0.1.0"}
    });
    assert!(serde_json::from_value::<InitializeParams>(legacy["params"].clone()).is_err());

    request["params"]["capabilities"]["stable"] = json!([]);
    assert!(!strict_capabilities(&request["params"]["capabilities"]));
    request["params"]["capabilities"]["stable"] = json!(["runtime.health"]);
    request["params"]["capabilities"]["experimental"] = json!(["future.preview"]);
    assert!(!strict_capabilities(&request["params"]["capabilities"]));

    let mut extra_envelope = compatible[0].clone();
    extra_envelope["credential"] = json!("opaque");
    assert!(!strict_envelope_valid(&extra_envelope));

    let request_and_result = json!({
        "jsonrpc": "2.0",
        "id": "same-id",
        "method": "initialize",
        "params": compatible[0]["params"].clone(),
        "result": compatible[1]["result"].clone()
    });
    assert!(!strict_envelope_valid(&request_and_result));

    let mut null_success_id = compatible[1].clone();
    null_success_id["id"] = Value::Null;
    assert!(!strict_envelope_valid(&null_success_id));
    assert!(strict_envelope_valid(&json!({
        "jsonrpc": "2.0",
        "id": null,
        "error": {"code": -32700, "message": "parse error"}
    })));

    let mut ordinary_request = json!({
        "jsonrpc": "2.0",
        "id": "ordinary-request",
        "method": "runtime/health",
        "params": {}
    });
    assert!(strict_envelope_valid(&ordinary_request));
    ordinary_request.as_object_mut().unwrap().remove("params");
    assert!(!strict_envelope_valid(&ordinary_request));

    let mut ordinary_notification = json!({
        "jsonrpc": "2.0",
        "method": "runtime/ordinary-notification",
        "params": {}
    });
    assert!(strict_envelope_valid(&ordinary_notification));
    ordinary_notification
        .as_object_mut()
        .unwrap()
        .remove("params");
    assert!(!strict_envelope_valid(&ordinary_notification));

    let mut numeric_request_id = compatible[0].clone();
    numeric_request_id["id"] = json!(7);
    assert!(!strict_envelope_valid(&numeric_request_id));
    let mut long_request_id = compatible[0].clone();
    long_request_id["id"] = json!("x".repeat(129));
    assert!(!strict_envelope_valid(&long_request_id));
    let mut unicode_request_id = compatible[0].clone();
    unicode_request_id["id"] = json!("会话");
    assert!(!strict_envelope_valid(&unicode_request_id));
    let mut spaced_request_id = compatible[0].clone();
    spaced_request_id["id"] = json!("initialize request");
    assert!(!strict_envelope_valid(&spaced_request_id));

    let mut invalid_method = compatible[0].clone();
    invalid_method["method"] = json!("initialize//legacy");
    assert!(!strict_envelope_valid(&invalid_method));
    assert!(valid_method_name("thread/tokenUsage/updated"));

    let initialized = compatible[2].clone();
    let mut missing_initialized_params = initialized.clone();
    missing_initialized_params
        .as_object_mut()
        .unwrap()
        .remove("params");
    assert!(!strict_envelope_valid(&missing_initialized_params));
    let mut nonempty_initialized_params = initialized;
    nonempty_initialized_params["params"] = json!({"legacy": true});
    assert!(!strict_envelope_valid(&nonempty_initialized_params));

    assert!(valid_capability_name("runtime.health"));
    assert!(!valid_capability_name("runtime..health"));
    assert!(!valid_capability_name("runtime.health-"));

    let mut malformed_error = fixture_messages("aap-initialize-incompatible.jsonl")[1].clone();
    malformed_error["error"]["data"]
        .as_object_mut()
        .unwrap()
        .remove("upgrade_direction");
    assert!(!has_exact_keys(
        malformed_error["error"]["data"].as_object().unwrap(),
        &[
            "schema_version",
            "reason",
            "client",
            "runtime",
            "upgrade_direction",
        ]
    ));
}

#[test]
fn timeline_event_fixture_matches_schema_types_and_ordering_contract() {
    let messages = fixture_messages("aap-timeline-events.jsonl");
    assert_eq!(messages.len(), 7);
    let mut event_ids = HashSet::new();
    let mut last_sequence = 0;
    let mut last_timestamp = 0;
    let mut item_revisions = std::collections::HashMap::new();

    for message in &messages {
        assert!(strict_envelope_valid(message), "invalid fixture: {message}");
        assert_content_free(message);
        assert_eq!(message["method"], "event");
        let params = message["params"].as_object().unwrap();
        assert!(has_exact_keys(
            params,
            &[
                "schema_version",
                "event_id",
                "sequence",
                "timestamp_ms",
                "correlation_id",
                "session_id",
                "turn_id",
                "turn_state",
                "event",
                "item",
                "item_update",
            ]
        ));
        let event: EventEnvelope = serde_json::from_value(message["params"].clone()).unwrap();
        assert_eq!(serde_json::to_value(&event).unwrap(), message["params"]);
        assert_eq!(event.correlation_id, event.turn_id);
        assert_eq!(event.event_id, expected_timeline_event_id(&event));
        assert!(event_ids.insert(event.event_id.clone()));
        assert_eq!(event.sequence, last_sequence + 1);
        assert!(event.timestamp_ms > last_timestamp);
        last_sequence = event.sequence;
        last_timestamp = event.timestamp_ms;

        match (&event.item, &event.item_update) {
            (Some(item), Some(update)) => {
                let next = item_revisions.entry(item.id.clone()).or_insert(0_u64);
                assert_eq!(update.revision, *next + 1);
                *next = update.revision;
                assert_eq!(update.content_mode, "snapshot-replacement");
            }
            (None, None) => {}
            _ => panic!("item and item_update presence drifted"),
        }
    }

    assert_eq!(messages[0]["params"]["event"], "turn.started");
    assert_eq!(messages[5]["params"]["event"], "future.adapter-state");
    assert!(messages[5]["params"]["item"].is_null());
    assert_eq!(messages[6]["params"]["event"], "turn.completed");
    assert_eq!(messages[6]["params"]["turn_state"], "completed");
}

#[test]
fn timeline_event_schema_documents_typed_aggregate_data_bounds() {
    let schema: Value = serde_json::from_str(&fs::read_to_string(schema_path()).unwrap()).unwrap();
    let comment = schema["$defs"]["jsonSafeValue"]["$comment"]
        .as_str()
        .unwrap();
    assert!(comment.contains("maximum depth of 16"));
    assert!(comment.contains("4096 total values"));
    assert!(comment.contains("Rust and Qt typed validators"));
}

#[test]
fn timeline_event_guide_examples_have_reproducible_ids_and_complete_cancellation() {
    let messages = guide_timeline_messages();
    assert_eq!(messages.len(), 8);

    let events = messages
        .iter()
        .map(|message| {
            assert!(
                strict_envelope_valid(message),
                "invalid guide event: {message}"
            );
            let event: EventEnvelope = serde_json::from_value(message["params"].clone()).unwrap();
            assert_eq!(event.event_id, expected_timeline_event_id(&event));
            event
        })
        .collect::<Vec<_>>();

    let cancellation = events
        .iter()
        .filter(|event| event.session_id == "session-2")
        .collect::<Vec<_>>();
    assert_eq!(cancellation.len(), 3);
    assert_eq!(
        cancellation
            .iter()
            .map(|event| (event.sequence, event.event.as_str()))
            .collect::<Vec<_>>(),
        vec![
            (1, "turn.started"),
            (2, "turn.cancellation-acknowledged"),
            (3, "turn.interrupted"),
        ]
    );
    assert_eq!(
        cancellation
            .iter()
            .map(|event| event.event_id.as_str())
            .collect::<Vec<_>>(),
        vec![
            "event:sha256:72ea0537d20013e275ce8545ee08e65a783deeef0fec69ad011ff823679217e9",
            "event:sha256:fd7d6120bc7ba2ffff60562cd172a6a4d872781ad638953f89be27e7a5837e4f",
            "event:sha256:8b416a06c09ed140775e75d12d36a333a68e863e4565ffea33a25c681695eae2",
        ]
    );
    assert!(cancellation[0].turn_state == TurnState::Running);
    assert!(cancellation[1].turn_state == TurnState::Running);
    assert!(cancellation[2].turn_state == TurnState::Interrupted);
}

#[test]
fn timeline_event_schema_accepts_every_known_event_shape() {
    let started = fixture_messages("aap-timeline-events.jsonl")[0].clone();
    let cases = [
        ("turn.started", "running", None),
        ("turn.completed", "completed", None),
        (
            "turn.failed",
            "failed",
            Some(("error", "system", "completed")),
        ),
        ("turn.interrupted", "interrupted", None),
        (
            "item.started",
            "running",
            Some(("message", "agent", "started")),
        ),
        ("item.delta", "running", Some(("message", "agent", "delta"))),
        (
            "item.completed",
            "running",
            Some(("message", "agent", "completed")),
        ),
        (
            "diagnostics.observed",
            "running",
            Some(("diagnostic", "tool", "completed")),
        ),
        (
            "usage.updated",
            "running",
            Some(("usage", "system", "updated")),
        ),
        (
            "usage.truncated",
            "running",
            Some(("usage", "system", "truncated")),
        ),
        (
            "turn.diff.updated",
            "running",
            Some(("diff", "tool", "updated")),
        ),
        (
            "turn.diff.truncated",
            "running",
            Some(("diff", "tool", "truncated")),
        ),
        (
            "turn.plan.updated",
            "running",
            Some(("plan", "agent", "updated")),
        ),
        (
            "turn.plan.truncated",
            "running",
            Some(("plan", "agent", "truncated")),
        ),
        (
            "turn.error-observed",
            "running",
            Some(("error", "system", "updated")),
        ),
        ("turn.error-observed.truncated", "running", None),
        (
            "turn.steering-requested",
            "running",
            Some(("message", "user", "completed")),
        ),
        ("turn.steering-acknowledged", "running", None),
        (
            "turn.steering-failed",
            "running",
            Some(("error", "system", "completed")),
        ),
        ("turn.cancellation-acknowledged", "running", None),
        (
            "turn.cancellation-failed",
            "running",
            Some(("error", "system", "completed")),
        ),
    ];

    for (index, (event, turn_state, item_shape)) in cases.into_iter().enumerate() {
        let mut message = started.clone();
        message["params"]["event"] = json!(event);
        message["params"]["turn_state"] = json!(turn_state);
        message["params"]["sequence"] = json!(index + 1);
        if let Some((kind, role, state)) = item_shape {
            message["params"]["item"] = json!({
                "id": format!("item-{index}"),
                "kind": kind,
                "role": role,
                "state": state,
                "content": "bounded snapshot"
            });
            message["params"]["item_update"] =
                json!({"revision": 1, "content_mode": "snapshot-replacement"});
        }
        reseal_timeline_event(&mut message);
        assert!(strict_envelope_valid(&message), "rejected {event}");
        assert!(
            serde_json::from_value::<EventEnvelope>(message["params"].clone()).is_ok(),
            "Rust type rejected {event}"
        );
    }
}

#[test]
fn timeline_event_schema_rejects_missing_bounds_pairing_and_unknown_item_drift() {
    let messages = fixture_messages("aap-timeline-events.jsonl");
    let started = messages[0].clone();
    let completed_item = messages[1].clone();

    for missing in [
        "schema_version",
        "event_id",
        "sequence",
        "timestamp_ms",
        "correlation_id",
        "session_id",
        "turn_id",
        "turn_state",
        "event",
        "item",
        "item_update",
    ] {
        let mut invalid = started.clone();
        invalid["params"].as_object_mut().unwrap().remove(missing);
        assert!(
            !strict_envelope_valid(&invalid),
            "accepted missing {missing}"
        );
    }

    let mut extra = started.clone();
    extra["params"]["legacy"] = json!(true);
    assert!(!strict_envelope_valid(&extra));

    for invalid_number in [json!(0), json!(9_007_199_254_740_992_u64)] {
        for field in ["sequence", "timestamp_ms"] {
            let mut invalid = started.clone();
            invalid["params"][field] = invalid_number.clone();
            assert!(!strict_envelope_valid(&invalid));
        }
    }

    let mut invalid_event_id = started.clone();
    invalid_event_id["params"]["event_id"] = json!(format!("event:sha256:{}", "A".repeat(64)));
    assert!(!strict_envelope_valid(&invalid_event_id));

    let mut invalid_identifier = started.clone();
    invalid_identifier["params"]["session_id"] = json!("session id");
    assert!(!strict_envelope_valid(&invalid_identifier));

    let mut missing_update = completed_item.clone();
    missing_update["params"]["item_update"] = Value::Null;
    assert!(!strict_envelope_valid(&missing_update));

    let mut update_without_item = started.clone();
    update_without_item["params"]["item_update"] =
        json!({"revision": 1, "content_mode": "snapshot-replacement"});
    assert!(!strict_envelope_valid(&update_without_item));

    let mut invalid_revision = completed_item.clone();
    invalid_revision["params"]["item_update"]["revision"] = json!(0);
    assert!(!strict_envelope_valid(&invalid_revision));
    invalid_revision["params"]["item_update"]["revision"] = json!(1);
    invalid_revision["params"]["item_update"]["content_mode"] = json!("append");
    assert!(!strict_envelope_valid(&invalid_revision));

    let mut extra_update = completed_item.clone();
    extra_update["params"]["item_update"]["append"] = json!(true);
    assert!(!strict_envelope_valid(&extra_update));

    let mut extra_item = completed_item.clone();
    extra_item["params"]["item"]["legacy"] = json!(true);
    assert!(!strict_envelope_valid(&extra_item));

    let mut null_item_data = completed_item.clone();
    null_item_data["params"]["item"]["data"] = Value::Null;
    assert!(!strict_envelope_valid(&null_item_data));

    let mut wrong_item_state = completed_item.clone();
    wrong_item_state["params"]["item"]["state"] = json!("delta");
    assert!(!strict_envelope_valid(&wrong_item_state));

    let mut unknown_with_item = completed_item.clone();
    unknown_with_item["params"]["event"] = json!("future.adapter-state");
    assert!(!strict_envelope_valid(&unknown_with_item));

    let mut unknown_terminal = started.clone();
    unknown_terminal["params"]["event"] = json!("future.adapter-state");
    unknown_terminal["params"]["turn_state"] = json!("completed");
    assert!(!strict_envelope_valid(&unknown_terminal));

    let mut persistence_failed = started;
    persistence_failed["params"]["event"] = json!("turn.persistence-failed");
    assert!(!strict_envelope_valid(&persistence_failed));
}

#[test]
fn timeline_event_schema_accepts_exact_safe_and_text_boundaries() {
    let mut boundary = fixture_messages("aap-timeline-events.jsonl")[1].clone();
    boundary["params"]["sequence"] = json!(9_007_199_254_740_991_u64);
    boundary["params"]["timestamp_ms"] = json!(9_007_199_254_740_991_u64);
    boundary["params"]["session_id"] = json!("s".repeat(128));
    boundary["params"]["item"]["id"] = json!("i".repeat(128));
    boundary["params"]["item"]["kind"] = json!("k".repeat(64));
    boundary["params"]["item"]["content"] = json!("界".repeat(65_536));
    boundary["params"]["item_update"]["revision"] = json!(9_007_199_254_740_991_u64);
    reseal_timeline_event(&mut boundary);
    assert!(strict_envelope_valid(&boundary));
    let event: EventEnvelope = serde_json::from_value(boundary["params"].clone()).unwrap();
    assert_eq!(serde_json::to_value(event).unwrap(), boundary["params"]);
}

#[test]
fn timeline_event_schema_requires_structured_failed_terminal_item() {
    let mut failed = fixture_messages("aap-timeline-events.jsonl")[1].clone();
    failed["params"]["event"] = json!("turn.failed");
    failed["params"]["turn_state"] = json!("failed");
    failed["params"]["item"] = json!({
        "id": "error-1",
        "kind": "error",
        "role": "system",
        "state": "completed",
        "content": "Turn failed"
    });
    assert!(strict_envelope_valid(&failed));

    let mut missing_item = failed.clone();
    missing_item["params"]["item"] = Value::Null;
    missing_item["params"]["item_update"] = Value::Null;
    assert!(!strict_envelope_valid(&missing_item));

    let mut wrong_role = failed;
    wrong_role["params"]["item"]["role"] = json!("agent");
    assert!(!strict_envelope_valid(&wrong_role));
}

#[test]
fn timeline_sync_fixture_matches_fixed_watermark_schema_and_rust_contract() {
    let messages = fixture_messages("aap-timeline-sync.jsonl");
    assert_eq!(messages.len(), 4);
    messages.iter().for_each(assert_content_free);

    let mut requests = Vec::new();
    let mut pages = Vec::new();
    for (index, message) in messages.iter().enumerate() {
        assert!(
            strict_envelope_valid(message),
            "invalid sync fixture: {message}"
        );
        if index % 2 == 0 {
            assert!(schema_definition_valid("timelineSyncRequest", message));
            assert_eq!(message["method"], "timeline/sync");
            let params: TimelineSyncParams =
                serde_json::from_value(message["params"].clone()).unwrap();
            assert_eq!(serde_json::to_value(&params).unwrap(), message["params"]);
            requests.push(params);
        } else {
            assert!(schema_definition_valid(
                "timelineSyncSuccessResponse",
                message
            ));
            let page: TimelineSyncPage = serde_json::from_value(message["result"].clone()).unwrap();
            assert_eq!(serde_json::to_value(&page).unwrap(), message["result"]);
            pages.push(page);
        }
    }

    assert_eq!(requests.len(), 2);
    assert_eq!(pages.len(), 2);
    pages[0].validate_for_request(&requests[0]).unwrap();
    pages[1].validate_for_request(&requests[1]).unwrap();
    assert_eq!(pages[0].watermark, pages[1].watermark);
    assert_eq!(pages[0].next_after.as_ref(), Some(&requests[1].after));
    assert!(!pages[0].complete);
    assert!(pages[1].complete);
    assert!(pages[1].next_after.is_none());
    assert_eq!(
        pages
            .iter()
            .flat_map(|page| page.events.iter().map(|event| event.sequence))
            .collect::<Vec<_>>(),
        [1, 2]
    );
}

#[test]
fn timeline_sync_guide_example_is_an_empty_complete_fixed_watermark_page() {
    let messages = guide_timeline_sync_messages();
    assert_eq!(messages.len(), 2);
    assert!(schema_definition_valid("timelineSyncRequest", &messages[0]));
    assert!(schema_definition_valid(
        "timelineSyncSuccessResponse",
        &messages[1]
    ));

    let request: TimelineSyncParams =
        serde_json::from_value(messages[0]["params"].clone()).unwrap();
    let page: TimelineSyncPage = serde_json::from_value(messages[1]["result"].clone()).unwrap();
    page.validate_for_request(&request).unwrap();
    assert_eq!(page.after.sequence, 0);
    assert_eq!(page.watermark.sequence, 0);
    assert!(page.events.is_empty());
    assert!(page.complete);
}

#[test]
fn timeline_snapshot_fixture_binds_fixed_head_identity_cursor_and_active_turn() {
    let messages = fixture_messages("aap-timeline-snapshot.jsonl");
    assert_eq!(messages.len(), 4);
    messages.iter().for_each(assert_content_free);

    let mut requests = Vec::new();
    let mut pages = Vec::new();
    for (index, message) in messages.iter().enumerate() {
        assert!(
            strict_envelope_valid(message),
            "invalid snapshot fixture: {message}"
        );
        if index % 2 == 0 {
            assert!(schema_definition_valid("timelineSnapshotRequest", message));
            assert_eq!(message["method"], "timeline/snapshot");
            let request: TimelineSnapshotParams =
                serde_json::from_value(message["params"].clone()).unwrap();
            assert_eq!(serde_json::to_value(&request).unwrap(), message["params"]);
            requests.push(request);
        } else {
            assert!(schema_definition_valid(
                "timelineSnapshotSuccessResponse",
                message
            ));
            let page: TimelineSessionSnapshotPage =
                serde_json::from_value(message["result"].clone()).unwrap();
            assert_eq!(serde_json::to_value(&page).unwrap(), message["result"]);
            pages.push(page);
        }
    }

    assert_eq!(requests.len(), 2);
    assert_eq!(pages.len(), 2);
    pages[0].validate_for_request(&requests[0]).unwrap();
    pages[0]
        .validate_continuation(&requests[1], &pages[1])
        .unwrap();
    assert_eq!(pages[0].snapshot_identity, pages[1].snapshot_identity);
    assert_eq!(pages[0].floor, pages[1].floor);
    assert_eq!(pages[0].watermark, pages[1].watermark);
    assert_eq!(pages[0].active_turn, pages[1].active_turn);
    assert_eq!(pages[0].next_after, requests[1].after);
    assert!(!pages[0].complete);
    assert!(pages[1].complete);
    pages[1]
        .validate_complete_identity(
            &pages
                .iter()
                .flat_map(|page| page.items.iter().map(|item| item.item_identity.clone()))
                .collect::<Vec<_>>(),
        )
        .unwrap();
    pages[1]
        .validate_complete_items(
            &pages
                .iter()
                .flat_map(|page| page.items.iter().cloned())
                .collect::<Vec<_>>(),
        )
        .unwrap();

    for (field, mismatch) in [
        ("session_id", json!("session-2")),
        (
            "watermark",
            json!({
                "sequence": 3,
                "event_id": format!("event:sha256:{}", "d".repeat(64))
            }),
        ),
        (
            "snapshot_identity",
            json!(format!(
                "timeline-session-snapshot:sha256:{}",
                "e".repeat(64)
            )),
        ),
        (
            "after",
            json!({
                "ordinal": 2,
                "item_id": messages[2]["params"]["after"]["item_id"],
                "item_identity": messages[2]["params"]["after"]["item_identity"]
            }),
        ),
    ] {
        let mut candidate = messages[2].clone();
        candidate["params"][field] = mismatch;
        assert!(schema_definition_valid(
            "timelineSnapshotRequest",
            &candidate
        ));
        let request: TimelineSnapshotParams =
            serde_json::from_value(candidate["params"].clone()).unwrap();
        assert!(pages[0].validate_continuation(&request, &pages[1]).is_err());
    }

    let mut changed_turn = pages[1].clone();
    changed_turn.active_turn.as_mut().unwrap().turn_id = "turn-2".into();
    assert!(pages[0]
        .validate_continuation(&requests[1], &changed_turn)
        .is_err());

    let mut extra_request = messages[0].clone();
    extra_request["params"]["head"] = messages[1]["result"]["watermark"].clone();
    assert!(!strict_envelope_valid(&extra_request));
    assert!(!schema_definition_valid(
        "timelineSnapshotRequest",
        &extra_request
    ));

    let mut invalid_active_turn = messages[1].clone();
    invalid_active_turn["result"]["active_turn"]["state"] = json!("completed");
    assert!(!schema_definition_valid(
        "timelineSnapshotSuccessResponse",
        &invalid_active_turn
    ));
    assert!(serde_json::from_value::<TimelineSessionSnapshotPage>(
        invalid_active_turn["result"].clone()
    )
    .is_err());
}

#[test]
fn timeline_retention_gap_fixture_is_strict_content_free_and_requires_snapshot() {
    let messages = fixture_messages("aap-timeline-retention-gap.jsonl");
    assert_eq!(messages.len(), 2);
    assert!(schema_definition_valid("timelineSyncRequest", &messages[0]));
    assert!(schema_definition_valid(
        "timelineSyncRetentionGapErrorResponse",
        &messages[1]
    ));
    assert!(strict_envelope_valid(&messages[1]));
    assert_content_free(&messages[1]);

    let data: TimelineRetentionGapData =
        serde_json::from_value(messages[1]["error"]["data"].clone()).unwrap();
    let request: TimelineSyncParams =
        serde_json::from_value(messages[0]["params"].clone()).unwrap();
    data.validate_for_request(&request).unwrap();
    assert_eq!(
        serde_json::to_value(&data).unwrap(),
        messages[1]["error"]["data"]
    );
    assert!(data.snapshot_required);
    assert!(data.snapshot_available);
    assert!(!data.event_history_complete);
    assert!(!data.replay_from_floor_allowed);

    let mut unavailable = messages[1].clone();
    unavailable["error"]["data"]["snapshot_available"] = json!(false);
    assert!(schema_definition_valid(
        "timelineSyncRetentionGapErrorResponse",
        &unavailable
    ));
    let unavailable: TimelineRetentionGapData =
        serde_json::from_value(unavailable["error"]["data"].clone()).unwrap();
    unavailable.validate_for_request(&request).unwrap();
    assert!(!unavailable.snapshot_available);

    for (key, invalid) in [
        ("snapshot_required", json!(false)),
        ("snapshot_available", json!("true")),
        ("event_history_complete", json!(true)),
        ("replay_from_floor_allowed", json!(true)),
        ("snapshot_capability", json!("session.history")),
    ] {
        let mut candidate = messages[1].clone();
        candidate["error"]["data"][key] = invalid;
        assert!(!schema_definition_valid(
            "timelineSyncRetentionGapErrorResponse",
            &candidate
        ));
        assert!(serde_json::from_value::<TimelineRetentionGapData>(
            candidate["error"]["data"].clone()
        )
        .is_err());
    }

    for (key, mismatch) in [
        ("session_id", json!("session-2")),
        (
            "requested_after",
            json!({
                "sequence": 1,
                "event_id": format!("event:sha256:{}", "c".repeat(64))
            }),
        ),
        (
            "requested_watermark",
            json!({
                "sequence": 3,
                "event_id": format!("event:sha256:{}", "b".repeat(64))
            }),
        ),
    ] {
        let mut candidate = messages[1].clone();
        candidate["error"]["data"][key] = mismatch;
        assert!(schema_definition_valid(
            "timelineSyncRetentionGapErrorResponse",
            &candidate
        ));
        let candidate: TimelineRetentionGapData =
            serde_json::from_value(candidate["error"]["data"].clone()).unwrap();
        assert!(candidate.validate_for_request(&request).is_err());
    }

    let mut extra = messages[1].clone();
    extra["error"]["data"]["checkpoint_identity"] = json!("internal");
    assert!(!schema_definition_valid(
        "timelineSyncRetentionGapErrorResponse",
        &extra
    ));
}

#[test]
fn timeline_sync_schema_and_types_reject_invalid_anchors_pages_and_drift() {
    let messages = fixture_messages("aap-timeline-sync.jsonl");
    let initial_request = messages[0].clone();
    let first_response = messages[1].clone();
    let continued_request = messages[2].clone();
    let final_response = messages[3].clone();

    for invalid_anchor in [
        json!({
            "sequence": 0,
            "event_id": format!("event:sha256:{}", "a".repeat(64))
        }),
        json!({"sequence": 1, "event_id": null}),
        json!({
            "sequence": 1,
            "event_id": format!("event:sha256:{}", "A".repeat(64))
        }),
        json!({"sequence": 9_007_199_254_740_992_u64, "event_id": null}),
        json!({"sequence": 0, "event_id": null, "legacy": true}),
    ] {
        let mut invalid = initial_request.clone();
        invalid["params"]["after"] = invalid_anchor;
        assert!(!strict_envelope_valid(&invalid));
        assert!(serde_json::from_value::<TimelineSyncParams>(invalid["params"].clone()).is_err());
    }

    for invalid_limit in [json!(0), json!(201), json!(1.5)] {
        let mut invalid = initial_request.clone();
        invalid["params"]["limit"] = invalid_limit;
        assert!(!strict_envelope_valid(&invalid));
        assert!(serde_json::from_value::<TimelineSyncParams>(invalid["params"].clone()).is_err());
    }

    let mut extra_request = initial_request.clone();
    extra_request["params"]["legacy"] = json!(true);
    assert!(!strict_envelope_valid(&extra_request));

    let mut missing_watermark = initial_request.clone();
    missing_watermark["params"]
        .as_object_mut()
        .unwrap()
        .remove("watermark");
    assert!(!strict_envelope_valid(&missing_watermark));

    let mut incomplete_without_next = first_response.clone();
    incomplete_without_next["result"]["next_after"] = Value::Null;
    assert!(!schema_definition_valid(
        "timelineSyncSuccessResponse",
        &incomplete_without_next
    ));
    assert!(
        serde_json::from_value::<TimelineSyncPage>(incomplete_without_next["result"].clone())
            .is_err()
    );

    let mut complete_with_next = final_response.clone();
    complete_with_next["result"]["next_after"] = complete_with_next["result"]["watermark"].clone();
    assert!(!schema_definition_valid(
        "timelineSyncSuccessResponse",
        &complete_with_next
    ));

    let mut extra_response = final_response.clone();
    extra_response["result"]["legacy"] = json!(true);
    assert!(!schema_definition_valid(
        "timelineSyncSuccessResponse",
        &extra_response
    ));

    let request: TimelineSyncParams =
        serde_json::from_value(continued_request["params"].clone()).unwrap();
    let page: TimelineSyncPage = serde_json::from_value(final_response["result"].clone()).unwrap();

    let changed_after_request: TimelineSyncParams = serde_json::from_value(json!({
        "session_id": "session-1",
        "after": {"sequence": 0, "event_id": null},
        "watermark": final_response["result"]["watermark"].clone(),
        "limit": 1
    }))
    .unwrap();
    assert!(page.validate_for_request(&changed_after_request).is_err());

    let mut changed_watermark_request = request.clone();
    changed_watermark_request.watermark = Some(changed_watermark_request.after.clone());
    assert!(page
        .validate_for_request(&changed_watermark_request)
        .is_err());

    let mut sequence_gap = first_response["result"].clone();
    sequence_gap["events"] = final_response["result"]["events"].clone();
    sequence_gap["next_after"] = final_response["result"]["watermark"].clone();
    assert!(schema_definition_valid("timelineSyncPage", &sequence_gap));
    assert!(serde_json::from_value::<TimelineSyncPage>(sequence_gap).is_err());

    let mut forged_watermark = final_response["result"].clone();
    forged_watermark["watermark"]["event_id"] = json!(format!("event:sha256:{}", "f".repeat(64)));
    assert!(schema_definition_valid(
        "timelineSyncPage",
        &forged_watermark
    ));
    assert!(serde_json::from_value::<TimelineSyncPage>(forged_watermark).is_err());

    let too_many = vec![first_response["result"]["events"][0].clone(); 201];
    let mut oversized_page = first_response;
    oversized_page["result"]["events"] = json!(too_many);
    assert!(!schema_definition_valid(
        "timelineSyncSuccessResponse",
        &oversized_page
    ));
}
