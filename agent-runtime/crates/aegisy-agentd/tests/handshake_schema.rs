use aegisy_aap::stable::v0_1::{InitializeParams, InitializeResult};
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

fn fixture_messages(name: &str) -> Vec<Value> {
    fs::read_to_string(fixture_path(name))
        .unwrap()
        .lines()
        .map(|line| serde_json::from_str(line).unwrap())
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
        "method": "event",
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
