use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::HashSet;
use std::fmt;

pub const JSONRPC_VERSION: &str = "2.0";
pub const PROTOCOL_VERSION: &str = "0.1";
pub const RUNTIME_PROTOCOL_MINIMUM: &str = PROTOCOL_VERSION;
pub const RUNTIME_PROTOCOL_MAXIMUM: &str = PROTOCOL_VERSION;
pub const MAX_AAP_FRAME_BYTES: u64 = 4 * 1024 * 1024;
pub const MAX_INITIALIZE_CAPABILITIES: usize = 128;
pub const MAX_INITIALIZE_CAPABILITY_BYTES: usize = 128;

fn valid_initialize_capability(value: &str) -> bool {
    let bytes = value.as_bytes();
    if bytes.is_empty() {
        return false;
    }

    let mut needs_segment = true;
    for byte in bytes {
        if byte.is_ascii_lowercase() || byte.is_ascii_digit() {
            needs_segment = false;
        } else if (*byte == b'.' || *byte == b'-') && !needs_segment {
            needs_segment = true;
        } else {
            return false;
        }
    }
    !needs_segment
}

fn deserialize_capability_list<'de, D>(deserializer: D) -> Result<Vec<String>, D::Error>
where
    D: serde::Deserializer<'de>,
{
    struct CapabilityVisitor;

    impl<'de> serde::de::Visitor<'de> for CapabilityVisitor {
        type Value = Vec<String>;

        fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
            formatter.write_str("a bounded array of unique capability strings")
        }

        fn visit_seq<A>(self, mut sequence: A) -> Result<Self::Value, A::Error>
        where
            A: serde::de::SeqAccess<'de>,
        {
            let mut capabilities = Vec::with_capacity(
                sequence
                    .size_hint()
                    .unwrap_or_default()
                    .min(MAX_INITIALIZE_CAPABILITIES),
            );
            let mut unique = HashSet::with_capacity(capabilities.capacity());
            while let Some(capability) = sequence.next_element::<String>()? {
                if capabilities.len() == MAX_INITIALIZE_CAPABILITIES {
                    return Err(serde::de::Error::custom(
                        "initialize capability count exceeds the limit",
                    ));
                }
                if capability.len() > MAX_INITIALIZE_CAPABILITY_BYTES {
                    return Err(serde::de::Error::custom(
                        "initialize capability exceeds the UTF-8 byte limit",
                    ));
                }
                if !valid_initialize_capability(&capability) {
                    return Err(serde::de::Error::custom(
                        "initialize capability format is invalid",
                    ));
                }
                if !unique.insert(capability.clone()) {
                    return Err(serde::de::Error::custom(
                        "initialize capabilities contain a duplicate",
                    ));
                }
                capabilities.push(capability);
            }
            Ok(capabilities)
        }
    }

    deserializer.deserialize_seq(CapabilityVisitor)
}

fn deserialize_non_empty_capability_list<'de, D>(deserializer: D) -> Result<Vec<String>, D::Error>
where
    D: serde::Deserializer<'de>,
{
    let capabilities = deserialize_capability_list(deserializer)?;
    if capabilities.is_empty() {
        return Err(serde::de::Error::custom(
            "stable initialize capabilities must not be empty",
        ));
    }
    Ok(capabilities)
}

pub mod stable {
    pub mod v0_1 {
        use super::super::*;

        #[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
        #[serde(deny_unknown_fields)]
        pub struct Identity {
            pub name: String,
            pub version: String,
        }

        #[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
        #[serde(deny_unknown_fields)]
        pub struct ProtocolRange {
            pub minimum: String,
            pub maximum: String,
            pub preferred: String,
        }

        #[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
        #[serde(deny_unknown_fields)]
        pub struct NegotiatedProtocol {
            pub minimum: String,
            pub maximum: String,
            pub selected: String,
            pub upgrade_direction: String,
        }

        #[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
        #[serde(deny_unknown_fields)]
        pub struct Platform {
            pub os: String,
            pub architecture: String,
        }

        #[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
        #[serde(deny_unknown_fields)]
        pub struct CapabilityDeclaration {
            #[serde(deserialize_with = "deserialize_non_empty_capability_list")]
            pub stable: Vec<String>,
            #[serde(deserialize_with = "deserialize_capability_list")]
            pub experimental: Vec<String>,
        }

        #[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
        #[serde(deny_unknown_fields)]
        pub struct NegotiatedCapabilities {
            pub stable: Vec<String>,
            pub experimental: Vec<String>,
        }

        #[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
        #[serde(deny_unknown_fields)]
        pub struct ProtocolLimits {
            pub max_frame_bytes: u64,
        }

        #[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
        #[serde(deny_unknown_fields)]
        pub struct TransportSecurity {
            pub transport: String,
            pub local: bool,
            pub authenticated: bool,
            pub encrypted: bool,
            pub peer_verified: bool,
        }

        #[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
        #[serde(deny_unknown_fields)]
        pub struct InitializeParams {
            pub protocol: ProtocolRange,
            pub client: Identity,
            pub platform: Platform,
            pub capabilities: CapabilityDeclaration,
            pub limits: ProtocolLimits,
            pub transport_security: TransportSecurity,
        }

        #[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
        #[serde(deny_unknown_fields)]
        pub struct InitializeResult {
            pub protocol: NegotiatedProtocol,
            pub runtime: Identity,
            pub platform: Platform,
            pub backend: BackendDescriptor,
            pub capabilities: NegotiatedCapabilities,
            pub limits: ProtocolLimits,
            pub transport_security: TransportSecurity,
        }

        #[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
        #[serde(deny_unknown_fields)]
        pub struct BackendDescriptor {
            pub adapter: String,
            pub status: String,
            pub version: String,
        }

        #[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
        #[serde(rename_all = "lowercase")]
        pub enum SessionMode {
            Chat,
            Work,
        }

        #[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
        pub struct Project {
            pub id: String,
            pub root: String,
            pub name: String,
        }

        #[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
        pub struct Session {
            pub id: String,
            pub mode: SessionMode,
            pub project_id: Option<String>,
            pub title: String,
        }

        #[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
        pub struct TimelineItem {
            pub id: String,
            pub kind: String,
            pub role: String,
            pub state: String,
            pub content: String,
            #[serde(default, skip_serializing_if = "Option::is_none")]
            pub data: Option<Value>,
        }

        #[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
        pub struct EventEnvelope {
            pub sequence: u64,
            pub timestamp_ms: u64,
            pub session_id: String,
            pub turn_id: Option<String>,
            pub event: String,
            pub item: Option<TimelineItem>,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::stable::v0_1::InitializeParams;
    use super::{MAX_INITIALIZE_CAPABILITIES, MAX_INITIALIZE_CAPABILITY_BYTES};
    use serde_json::{json, Value};

    fn base_params() -> Value {
        json!({
            "protocol": {"minimum": "0.1", "maximum": "0.1", "preferred": "0.1"},
            "client": {"name": "aegisy-client", "version": "1"},
            "platform": {"os": "macos", "architecture": "aarch64"},
            "capabilities": {"stable": ["runtime.health"], "experimental": []},
            "limits": {"max_frame_bytes": 4194304},
            "transport_security": {
                "transport": "stdio",
                "local": true,
                "authenticated": false,
                "encrypted": false,
                "peer_verified": false
            }
        })
    }

    fn with_stable_capabilities(capabilities: Value) -> Value {
        let mut params = base_params();
        params["capabilities"]["stable"] = capabilities;
        params
    }

    fn reject(capabilities: Value) -> String {
        serde_json::from_value::<InitializeParams>(with_stable_capabilities(capabilities))
            .expect_err("invalid capability declaration must be rejected")
            .to_string()
    }

    #[test]
    fn complete_initialize_contract_round_trips() {
        let params: InitializeParams = serde_json::from_value(base_params()).unwrap();
        assert_eq!(params.protocol.minimum, "0.1");
        assert_eq!(params.capabilities.stable, ["runtime.health"]);
        assert!(params.capabilities.experimental.is_empty());
        assert_eq!(params.limits.max_frame_bytes, 4 * 1024 * 1024);
        assert_eq!(serde_json::to_value(params).unwrap(), base_params());
    }

    #[test]
    fn declared_capabilities_preserve_order_and_unknown_names() {
        let declared = vec![
            "runtime.health".to_owned(),
            "future.valid-capability".to_owned(),
            "workspace.read-text".to_owned(),
        ];
        let params: InitializeParams =
            serde_json::from_value(with_stable_capabilities(json!(declared)))
                .expect("valid capability declaration");
        assert_eq!(params.capabilities.stable, declared);
        assert_eq!(
            serde_json::to_value(params).unwrap()["capabilities"]["stable"],
            json!(declared)
        );
    }

    #[test]
    fn capability_bounds_accept_exact_limits() {
        let capabilities = (0..MAX_INITIALIZE_CAPABILITIES)
            .map(|index| format!("capability.{index}"))
            .collect::<Vec<_>>();
        let params: InitializeParams =
            serde_json::from_value(with_stable_capabilities(json!(capabilities))).unwrap();
        assert_eq!(
            params.capabilities.stable.len(),
            MAX_INITIALIZE_CAPABILITIES
        );

        let longest = format!("a{}", "b".repeat(MAX_INITIALIZE_CAPABILITY_BYTES - 1));
        let params: InitializeParams =
            serde_json::from_value(with_stable_capabilities(json!([longest.clone()]))).unwrap();
        assert_eq!(params.capabilities.stable, [longest]);
    }

    #[test]
    fn missing_null_non_array_and_empty_declarations_are_rejected() {
        let mut missing = base_params();
        missing.as_object_mut().unwrap().remove("capabilities");
        assert!(serde_json::from_value::<InitializeParams>(missing).is_err());
        assert!(reject(Value::Null).contains("bounded array"));
        assert!(reject(json!("runtime.health")).contains("bounded array"));
        assert!(reject(json!({"runtime.health": true})).contains("bounded array"));
        assert!(reject(json!([])).contains("must not be empty"));
    }

    #[test]
    fn duplicate_and_over_limit_declarations_are_rejected() {
        assert!(reject(json!(["runtime.health", "runtime.health"])).contains("duplicate"));
        let too_many = (0..=MAX_INITIALIZE_CAPABILITIES)
            .map(|index| format!("capability.{index}"))
            .collect::<Vec<_>>();
        assert!(reject(json!(too_many)).contains("count exceeds"));
        let too_long = format!("a{}", "b".repeat(MAX_INITIALIZE_CAPABILITY_BYTES));
        assert!(reject(json!([too_long])).contains("UTF-8 byte limit"));
    }

    #[test]
    fn invalid_capability_formats_and_element_types_are_rejected_without_echo() {
        for invalid in [
            "Runtime.health",
            ".runtime.health",
            "runtime.health.",
            "runtime..health",
            "runtime.-health",
            "runtime--health",
            "runtime_health",
            "runtime/health",
            "能力.runtime",
        ] {
            let error = reject(json!([invalid]));
            assert!(error.contains("format is invalid"));
            assert!(!error.contains(invalid));
        }
        let error = reject(json!(["runtime.health", 7]));
        assert!(error.contains("expected a string"));
    }

    #[test]
    fn capability_length_is_measured_in_utf8_bytes() {
        let multibyte = format!("a{}", "é".repeat(MAX_INITIALIZE_CAPABILITY_BYTES / 2));
        assert!(multibyte.len() > MAX_INITIALIZE_CAPABILITY_BYTES);
        assert!(reject(json!([multibyte])).contains("UTF-8 byte limit"));
    }

    #[test]
    fn unknown_initialize_fields_are_rejected() {
        let mut params = base_params();
        params["legacy"] = json!(true);
        assert!(serde_json::from_value::<InitializeParams>(params).is_err());
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Request {
    pub jsonrpc: String,
    pub id: Option<Value>,
    pub method: String,
    #[serde(default)]
    pub params: Value,
}

#[derive(Debug, Clone, Serialize)]
pub struct Response {
    pub jsonrpc: &'static str,
    pub id: Value,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub result: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<RpcError>,
}

#[derive(Debug, Clone, Serialize)]
pub struct Notification<T: Serialize> {
    pub jsonrpc: &'static str,
    pub method: &'static str,
    pub params: T,
}

#[derive(Debug, Clone, Serialize)]
pub struct RpcError {
    pub code: i64,
    pub message: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub data: Option<Value>,
}

impl Response {
    pub fn success(id: Value, result: Value) -> Self {
        Self {
            jsonrpc: JSONRPC_VERSION,
            id,
            result: Some(result),
            error: None,
        }
    }

    pub fn error(id: Value, code: i64, message: impl Into<String>) -> Self {
        Self::error_with_data(id, code, message, None)
    }

    pub fn error_with_data(
        id: Value,
        code: i64,
        message: impl Into<String>,
        data: Option<Value>,
    ) -> Self {
        Self {
            jsonrpc: JSONRPC_VERSION,
            id,
            result: None,
            error: Some(RpcError {
                code,
                message: message.into(),
                data,
            }),
        }
    }
}
