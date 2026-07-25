use serde::{Deserialize, Serialize};
use serde_json::{Number, Value};
use sha2::{Digest, Sha256};
use std::collections::HashSet;
use std::fmt;

pub const JSONRPC_VERSION: &str = "2.0";
pub const PROTOCOL_VERSION: &str = "0.1";
pub const RUNTIME_PROTOCOL_MINIMUM: &str = PROTOCOL_VERSION;
pub const RUNTIME_PROTOCOL_MAXIMUM: &str = PROTOCOL_VERSION;
pub const MAX_AAP_FRAME_BYTES: u64 = 4 * 1024 * 1024;
pub const MAX_INITIALIZE_CAPABILITIES: usize = 128;
pub const MAX_INITIALIZE_CAPABILITY_BYTES: usize = 128;
pub const MAX_SAFE_JSON_INTEGER: u64 = 9_007_199_254_740_991;
pub const MAX_TIMELINE_IDENTIFIER_BYTES: usize = 128;
pub const MAX_TIMELINE_KIND_BYTES: usize = 64;
pub const MAX_TIMELINE_CONTENT_CHARACTERS: usize = 64 * 1024;
pub const MAX_TIMELINE_DATA_DEPTH: usize = 16;
pub const MAX_TIMELINE_DATA_NODES: usize = 4096;
pub const MAX_TIMELINE_SYNC_EVENTS: usize = 200;
pub const MAX_TIMELINE_SNAPSHOT_ITEMS: usize = 200;
pub const MAX_TIMELINE_SNAPSHOT_TOTAL_ITEMS: usize = 10_000;
pub const MAX_TIMELINE_SNAPSHOT_TOTAL_BYTES: u64 = 64 * 1024 * 1024;
pub const MAX_TIMELINE_SNAPSHOT_PAGE_BYTES: usize = MAX_AAP_FRAME_BYTES as usize - 512;

fn valid_ascii_graphical(value: &str, maximum_bytes: usize) -> bool {
    !value.is_empty()
        && value.len() <= maximum_bytes
        && value.bytes().all(|byte| byte.is_ascii_graphic())
}

fn valid_timeline_name(value: &str, maximum_bytes: usize) -> bool {
    let bytes = value.as_bytes();
    if bytes.is_empty() || bytes.len() > maximum_bytes || !bytes[0].is_ascii_lowercase() {
        return false;
    }

    let mut needs_segment = false;
    for byte in &bytes[1..] {
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

fn canonical_safe_json_integer(number: &Number) -> Option<i64> {
    let representation = number.to_string();
    let (negative, unsigned) = representation
        .strip_prefix('-')
        .map_or((false, representation.as_str()), |value| (true, value));
    let (mantissa, exponent) = unsigned
        .split_once(['e', 'E'])
        .map_or((unsigned, "0"), |parts| parts);
    let (integer, fraction) = mantissa
        .split_once('.')
        .map_or((mantissa, ""), |parts| parts);
    let mut digits = String::with_capacity(integer.len() + fraction.len());
    digits.push_str(integer);
    digits.push_str(fraction);
    let digits = digits.trim_start_matches('0');
    if digits.is_empty() {
        return Some(0);
    }

    let exponent = exponent.parse::<i64>().ok()?;
    let fraction_len = i64::try_from(fraction.len()).ok()?;
    let scale = exponent.checked_sub(fraction_len)?;
    let canonical_digits = if scale >= 0 {
        let zero_count = usize::try_from(scale).ok()?;
        if digits.len().checked_add(zero_count)? > 16 {
            return None;
        }
        let mut canonical = String::with_capacity(digits.len() + zero_count);
        canonical.push_str(digits);
        canonical.extend(std::iter::repeat_n('0', zero_count));
        canonical
    } else {
        let fractional_digits = usize::try_from(scale.checked_neg()?).ok()?;
        if fractional_digits > digits.len()
            || !digits[digits.len() - fractional_digits..]
                .bytes()
                .all(|byte| byte == b'0')
        {
            return None;
        }
        digits[..digits.len() - fractional_digits].to_owned()
    };
    let canonical_digits = canonical_digits.trim_start_matches('0');
    if canonical_digits.is_empty() {
        return Some(0);
    }
    const MAXIMUM: &str = "9007199254740991";
    if canonical_digits.len() > MAXIMUM.len()
        || (canonical_digits.len() == MAXIMUM.len() && canonical_digits > MAXIMUM)
    {
        return None;
    }
    let magnitude = canonical_digits.parse::<i64>().ok()?;
    Some(if negative { -magnitude } else { magnitude })
}

fn normalize_timeline_data(value: &mut Value, depth: usize, nodes: &mut usize) -> bool {
    *nodes = nodes.saturating_add(1);
    if *nodes > MAX_TIMELINE_DATA_NODES || depth > MAX_TIMELINE_DATA_DEPTH {
        return false;
    }
    match value {
        Value::Null | Value::Bool(_) | Value::String(_) => true,
        Value::Number(number) => canonical_safe_json_integer(number).is_some_and(|integer| {
            *number = Number::from(integer);
            true
        }),
        Value::Array(values) => values
            .iter_mut()
            .all(|value| normalize_timeline_data(value, depth + 1, nodes)),
        Value::Object(values) => {
            values.len() <= 128
                && values
                    .keys()
                    .all(|key| valid_ascii_graphical(key, MAX_TIMELINE_IDENTIFIER_BYTES))
                && values
                    .values_mut()
                    .all(|value| normalize_timeline_data(value, depth + 1, nodes))
        }
    }
}

fn canonical_timeline_data(value: &Value) -> Option<Value> {
    let mut canonical = value.clone();
    normalize_timeline_data(&mut canonical, 1, &mut 0).then_some(canonical)
}

fn deserialize_positive_safe_json_integer<'de, D>(deserializer: D) -> Result<u64, D::Error>
where
    D: serde::Deserializer<'de>,
{
    let number = Number::deserialize(deserializer)?;
    let value = canonical_safe_json_integer(&number)
        .filter(|value| *value > 0)
        .ok_or_else(|| {
            serde::de::Error::custom("value must be a positive JSON-safe mathematical integer")
        })?;
    u64::try_from(value).map_err(serde::de::Error::custom)
}

fn deserialize_nonnegative_safe_json_integer<'de, D>(deserializer: D) -> Result<u64, D::Error>
where
    D: serde::Deserializer<'de>,
{
    let number = Number::deserialize(deserializer)?;
    let value = canonical_safe_json_integer(&number)
        .filter(|value| *value >= 0)
        .ok_or_else(|| {
            serde::de::Error::custom("value must be a non-negative JSON-safe mathematical integer")
        })?;
    u64::try_from(value).map_err(serde::de::Error::custom)
}

fn deserialize_timeline_sync_limit<'de, D>(deserializer: D) -> Result<u64, D::Error>
where
    D: serde::Deserializer<'de>,
{
    let limit = deserialize_positive_safe_json_integer(deserializer)?;
    if limit > MAX_TIMELINE_SYNC_EVENTS as u64 {
        return Err(serde::de::Error::custom(
            "timeline sync limit exceeds the maximum",
        ));
    }
    Ok(limit)
}

fn valid_timeline_event_id(value: &str) -> bool {
    value.len() == 77
        && value.starts_with("event:sha256:")
        && value[13..]
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn valid_sha256_identity(value: &str, prefix: &str) -> bool {
    value.len() == prefix.len() + 64
        && value.starts_with(prefix)
        && value[prefix.len()..]
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn valid_timeline_snapshot_id(value: &str) -> bool {
    valid_sha256_identity(value, "timeline-session-snapshot:sha256:")
}

fn valid_timeline_snapshot_item_id(value: &str) -> bool {
    valid_sha256_identity(value, "timeline-session-snapshot-item:sha256:")
}

fn valid_timeline_snapshot_page_id(value: &str) -> bool {
    valid_sha256_identity(value, "timeline-session-snapshot-page:sha256:")
}

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

        #[derive(Debug, Clone, PartialEq, Eq)]
        pub struct TimelineItem {
            pub id: String,
            pub kind: String,
            pub role: String,
            pub state: String,
            pub content: String,
            pub data: Option<Value>,
        }

        #[derive(Debug, Deserialize)]
        #[serde(deny_unknown_fields)]
        struct TimelineItemWire {
            id: String,
            kind: String,
            role: String,
            state: String,
            content: String,
            #[serde(default, deserialize_with = "deserialize_optional_object")]
            data: Option<Value>,
        }

        fn deserialize_optional_object<'de, D>(deserializer: D) -> Result<Option<Value>, D::Error>
        where
            D: serde::Deserializer<'de>,
        {
            let mut value = Value::deserialize(deserializer)?;
            if !value.is_object() {
                return Err(serde::de::Error::custom(
                    "timeline item data must be an object",
                ));
            }
            if !normalize_timeline_data(&mut value, 1, &mut 0) {
                return Err(serde::de::Error::custom(
                    "timeline item data is outside canonical JSON limits",
                ));
            }
            Ok(Some(value))
        }

        #[derive(Serialize)]
        struct TimelineItemRef<'a> {
            id: &'a str,
            kind: &'a str,
            role: &'a str,
            state: &'a str,
            content: &'a str,
            #[serde(skip_serializing_if = "Option::is_none")]
            data: Option<&'a Value>,
        }

        impl TimelineItem {
            pub fn validate(&self) -> Result<(), &'static str> {
                if !valid_ascii_graphical(&self.id, MAX_TIMELINE_IDENTIFIER_BYTES) {
                    return Err("timeline item identity is invalid");
                }
                if !valid_timeline_name(&self.kind, MAX_TIMELINE_KIND_BYTES) {
                    return Err("timeline item kind is invalid");
                }
                if !matches!(self.role.as_str(), "user" | "agent" | "system" | "tool") {
                    return Err("timeline item role is invalid");
                }
                if !matches!(
                    self.state.as_str(),
                    "started"
                        | "running"
                        | "delta"
                        | "updated"
                        | "completed"
                        | "failed"
                        | "interrupted"
                        | "truncated"
                        | "unavailable"
                ) {
                    return Err("timeline item state is invalid");
                }
                if self.content.chars().count() > MAX_TIMELINE_CONTENT_CHARACTERS {
                    return Err("timeline item content exceeds the character limit");
                }
                if let Some(data) = &self.data {
                    let object = data
                        .as_object()
                        .ok_or("timeline item data must be an object")?;
                    if object.len() > 128 {
                        return Err("timeline item data property count exceeds the limit");
                    }
                    if canonical_timeline_data(data).is_none() {
                        return Err("timeline item data is outside canonical JSON limits");
                    }
                }
                Ok(())
            }
        }

        impl TryFrom<TimelineItemWire> for TimelineItem {
            type Error = &'static str;

            fn try_from(value: TimelineItemWire) -> Result<Self, Self::Error> {
                let item = Self {
                    id: value.id,
                    kind: value.kind,
                    role: value.role,
                    state: value.state,
                    content: value.content,
                    data: value.data,
                };
                item.validate()?;
                Ok(item)
            }
        }

        impl<'de> Deserialize<'de> for TimelineItem {
            fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
            where
                D: serde::Deserializer<'de>,
            {
                TimelineItemWire::deserialize(deserializer)?
                    .try_into()
                    .map_err(serde::de::Error::custom)
            }
        }

        impl Serialize for TimelineItem {
            fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
            where
                S: serde::Serializer,
            {
                self.validate().map_err(serde::ser::Error::custom)?;
                let data = self
                    .data
                    .as_ref()
                    .map(|value| {
                        canonical_timeline_data(value)
                            .ok_or("timeline item data is outside canonical JSON limits")
                    })
                    .transpose()
                    .map_err(serde::ser::Error::custom)?;
                TimelineItemRef {
                    id: &self.id,
                    kind: &self.kind,
                    role: &self.role,
                    state: &self.state,
                    content: &self.content,
                    data: data.as_ref(),
                }
                .serialize(serializer)
            }
        }

        #[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
        #[serde(rename_all = "lowercase")]
        pub enum TurnState {
            Running,
            Completed,
            Failed,
            Interrupted,
        }

        #[derive(Debug, Clone, PartialEq, Eq)]
        pub struct ItemUpdate {
            pub revision: u64,
            pub content_mode: String,
        }

        #[derive(Debug, Deserialize)]
        #[serde(deny_unknown_fields)]
        struct ItemUpdateWire {
            #[serde(deserialize_with = "deserialize_positive_safe_json_integer")]
            revision: u64,
            content_mode: String,
        }

        impl ItemUpdate {
            pub fn validate(&self) -> Result<(), &'static str> {
                if self.revision == 0 || self.revision > MAX_SAFE_JSON_INTEGER {
                    return Err("timeline item revision is outside the safe integer range");
                }
                if self.content_mode != "snapshot-replacement" {
                    return Err("timeline item content mode is invalid");
                }
                Ok(())
            }
        }

        impl TryFrom<ItemUpdateWire> for ItemUpdate {
            type Error = &'static str;

            fn try_from(value: ItemUpdateWire) -> Result<Self, Self::Error> {
                let update = Self {
                    revision: value.revision,
                    content_mode: value.content_mode,
                };
                update.validate()?;
                Ok(update)
            }
        }

        impl<'de> Deserialize<'de> for ItemUpdate {
            fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
            where
                D: serde::Deserializer<'de>,
            {
                ItemUpdateWire::deserialize(deserializer)?
                    .try_into()
                    .map_err(serde::de::Error::custom)
            }
        }

        impl Serialize for ItemUpdate {
            fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
            where
                S: serde::Serializer,
            {
                self.validate().map_err(serde::ser::Error::custom)?;
                #[derive(Serialize)]
                struct UpdateRef<'a> {
                    revision: u64,
                    content_mode: &'a str,
                }
                UpdateRef {
                    revision: self.revision,
                    content_mode: &self.content_mode,
                }
                .serialize(serializer)
            }
        }

        #[derive(Debug, Clone, PartialEq, Eq)]
        pub struct EventEnvelope {
            pub schema_version: String,
            pub event_id: String,
            pub sequence: u64,
            pub timestamp_ms: u64,
            pub correlation_id: String,
            pub session_id: String,
            pub turn_id: String,
            pub turn_state: TurnState,
            pub event: String,
            pub item: Option<TimelineItem>,
            pub item_update: Option<ItemUpdate>,
        }

        #[allow(clippy::too_many_arguments)]
        pub fn timeline_event_id(
            schema_version: &str,
            sequence: u64,
            timestamp_ms: u64,
            correlation_id: &str,
            session_id: &str,
            turn_id: &str,
            turn_state: TurnState,
            event: &str,
            item: &Option<TimelineItem>,
            item_update: &Option<ItemUpdate>,
        ) -> Result<String, &'static str> {
            #[derive(Serialize)]
            struct IdentityMaterial<'a> {
                schema_version: &'a str,
                sequence: u64,
                timestamp_ms: u64,
                correlation_id: &'a str,
                session_id: &'a str,
                turn_id: &'a str,
                turn_state: TurnState,
                event: &'a str,
                item: &'a Option<TimelineItem>,
                item_update: &'a Option<ItemUpdate>,
            }

            let encoded = serde_json::to_vec(&IdentityMaterial {
                schema_version,
                sequence,
                timestamp_ms,
                correlation_id,
                session_id,
                turn_id,
                turn_state,
                event,
                item,
                item_update,
            })
            .map_err(|_| "timeline event identity material is invalid")?;
            let encoded_len = u64::try_from(encoded.len())
                .map_err(|_| "timeline event identity material is too large")?;
            let mut digest = Sha256::new();
            digest.update(b"aegisy-timeline-event/0.1\0");
            digest.update(encoded_len.to_be_bytes());
            digest.update(encoded);
            Ok(format!("event:sha256:{:x}", digest.finalize()))
        }

        #[derive(Debug, Deserialize)]
        #[serde(deny_unknown_fields)]
        struct EventEnvelopeWire {
            schema_version: String,
            event_id: String,
            #[serde(deserialize_with = "deserialize_positive_safe_json_integer")]
            sequence: u64,
            #[serde(deserialize_with = "deserialize_positive_safe_json_integer")]
            timestamp_ms: u64,
            correlation_id: String,
            session_id: String,
            turn_id: String,
            turn_state: TurnState,
            event: String,
            item: Value,
            item_update: Value,
        }

        impl EventEnvelope {
            pub fn validate(&self) -> Result<(), &'static str> {
                if self.schema_version != "timeline-event/0.1" {
                    return Err("timeline event schema version is invalid");
                }
                if !valid_timeline_event_id(&self.event_id) {
                    return Err("timeline event identity is invalid");
                }
                if self.sequence == 0 || self.sequence > MAX_SAFE_JSON_INTEGER {
                    return Err("timeline event sequence is outside the safe integer range");
                }
                if self.timestamp_ms == 0 || self.timestamp_ms > MAX_SAFE_JSON_INTEGER {
                    return Err("timeline event timestamp is outside the safe integer range");
                }
                for value in [&self.correlation_id, &self.session_id, &self.turn_id] {
                    if !valid_ascii_graphical(value, MAX_TIMELINE_IDENTIFIER_BYTES) {
                        return Err("timeline event binding identity is invalid");
                    }
                }
                if self.correlation_id != self.turn_id {
                    return Err("timeline event correlation does not match its turn");
                }
                if !valid_timeline_name(&self.event, MAX_TIMELINE_IDENTIFIER_BYTES)
                    || self.event == "turn.persistence-failed"
                {
                    return Err("timeline event name is invalid");
                }
                if self.item.is_some() != self.item_update.is_some() {
                    return Err("timeline item and update metadata must be present together");
                }
                if let Some(item) = &self.item {
                    item.validate()?;
                }
                if let Some(update) = &self.item_update {
                    update.validate()?;
                }
                self.validate_event_shape()?;
                if timeline_event_id(
                    &self.schema_version,
                    self.sequence,
                    self.timestamp_ms,
                    &self.correlation_id,
                    &self.session_id,
                    &self.turn_id,
                    self.turn_state,
                    &self.event,
                    &self.item,
                    &self.item_update,
                )? != self.event_id
                {
                    return Err("timeline event identity does not match its content");
                }
                Ok(())
            }

            fn validate_event_shape(&self) -> Result<(), &'static str> {
                let item_shape = |kind: Option<&str>, role: Option<&str>, state: &str| {
                    let item = self
                        .item
                        .as_ref()
                        .ok_or("timeline event item is required")?;
                    if kind.is_some_and(|kind| item.kind != kind)
                        || role.is_some_and(|role| item.role != role)
                        || item.state != state
                    {
                        return Err("timeline event item shape does not match the event");
                    }
                    Ok(())
                };
                let no_item = || {
                    if self.item.is_some() {
                        Err("timeline event must not contain an item")
                    } else {
                        Ok(())
                    }
                };
                let running = || {
                    if self.turn_state == TurnState::Running {
                        Ok(())
                    } else {
                        Err("non-terminal timeline event must keep the turn running")
                    }
                };

                match self.event.as_str() {
                    "turn.started" => {
                        running()?;
                        no_item()
                    }
                    "turn.completed" => {
                        if self.turn_state != TurnState::Completed {
                            return Err("completed timeline event has the wrong turn state");
                        }
                        no_item()
                    }
                    "turn.failed" => {
                        if self.turn_state != TurnState::Failed {
                            return Err("failed timeline event has the wrong turn state");
                        }
                        item_shape(Some("error"), Some("system"), "completed")
                    }
                    "turn.interrupted" => {
                        if self.turn_state != TurnState::Interrupted {
                            return Err("interrupted timeline event has the wrong turn state");
                        }
                        no_item()
                    }
                    "item.started" => {
                        running()?;
                        item_shape(None, None, "started")
                    }
                    "item.delta" => {
                        running()?;
                        item_shape(None, None, "delta")
                    }
                    "item.completed" => {
                        running()?;
                        item_shape(None, None, "completed")
                    }
                    "diagnostics.observed" => {
                        running()?;
                        item_shape(Some("diagnostic"), Some("tool"), "completed")
                    }
                    "usage.updated" => {
                        running()?;
                        item_shape(Some("usage"), Some("system"), "updated")
                    }
                    "usage.truncated" => {
                        running()?;
                        item_shape(Some("usage"), Some("system"), "truncated")
                    }
                    "turn.diff.updated" => {
                        running()?;
                        item_shape(Some("diff"), Some("tool"), "updated")
                    }
                    "turn.diff.truncated" => {
                        running()?;
                        item_shape(Some("diff"), Some("tool"), "truncated")
                    }
                    "turn.plan.updated" => {
                        running()?;
                        item_shape(Some("plan"), Some("agent"), "updated")
                    }
                    "turn.plan.truncated" => {
                        running()?;
                        item_shape(Some("plan"), Some("agent"), "truncated")
                    }
                    "turn.error-observed" => {
                        running()?;
                        item_shape(Some("error"), Some("system"), "updated")
                    }
                    "turn.steering-requested" => {
                        running()?;
                        item_shape(Some("message"), Some("user"), "completed")
                    }
                    "turn.steering-failed" | "turn.cancellation-failed" => {
                        running()?;
                        item_shape(Some("error"), Some("system"), "completed")
                    }
                    "turn.error-observed.truncated"
                    | "turn.steering-acknowledged"
                    | "turn.cancellation-acknowledged" => {
                        running()?;
                        no_item()
                    }
                    _ => {
                        running()?;
                        no_item()
                    }
                }
            }
        }

        impl TryFrom<EventEnvelopeWire> for EventEnvelope {
            type Error = &'static str;

            fn try_from(value: EventEnvelopeWire) -> Result<Self, Self::Error> {
                let item = if value.item.is_null() {
                    None
                } else {
                    Some(
                        serde_json::from_value(value.item)
                            .map_err(|_| "timeline event item is invalid")?,
                    )
                };
                let item_update = if value.item_update.is_null() {
                    None
                } else {
                    Some(
                        serde_json::from_value(value.item_update)
                            .map_err(|_| "timeline event item update is invalid")?,
                    )
                };
                let event = Self {
                    schema_version: value.schema_version,
                    event_id: value.event_id,
                    sequence: value.sequence,
                    timestamp_ms: value.timestamp_ms,
                    correlation_id: value.correlation_id,
                    session_id: value.session_id,
                    turn_id: value.turn_id,
                    turn_state: value.turn_state,
                    event: value.event,
                    item,
                    item_update,
                };
                event.validate()?;
                Ok(event)
            }
        }

        impl<'de> Deserialize<'de> for EventEnvelope {
            fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
            where
                D: serde::Deserializer<'de>,
            {
                EventEnvelopeWire::deserialize(deserializer)?
                    .try_into()
                    .map_err(serde::de::Error::custom)
            }
        }

        impl Serialize for EventEnvelope {
            fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
            where
                S: serde::Serializer,
            {
                self.validate().map_err(serde::ser::Error::custom)?;
                #[derive(Serialize)]
                struct EventRef<'a> {
                    schema_version: &'a str,
                    event_id: &'a str,
                    sequence: u64,
                    timestamp_ms: u64,
                    correlation_id: &'a str,
                    session_id: &'a str,
                    turn_id: &'a str,
                    turn_state: TurnState,
                    event: &'a str,
                    item: Option<&'a TimelineItem>,
                    item_update: Option<&'a ItemUpdate>,
                }
                EventRef {
                    schema_version: &self.schema_version,
                    event_id: &self.event_id,
                    sequence: self.sequence,
                    timestamp_ms: self.timestamp_ms,
                    correlation_id: &self.correlation_id,
                    session_id: &self.session_id,
                    turn_id: &self.turn_id,
                    turn_state: self.turn_state,
                    event: &self.event,
                    item: self.item.as_ref(),
                    item_update: self.item_update.as_ref(),
                }
                .serialize(serializer)
            }
        }

        #[derive(Debug, Clone, PartialEq, Eq)]
        pub struct TimelineAnchor {
            pub sequence: u64,
            pub event_id: Option<String>,
        }

        #[derive(Debug, Deserialize)]
        #[serde(deny_unknown_fields)]
        struct TimelineAnchorWire {
            #[serde(deserialize_with = "deserialize_nonnegative_safe_json_integer")]
            sequence: u64,
            event_id: Value,
        }

        impl TimelineAnchor {
            pub fn validate(&self) -> Result<(), &'static str> {
                if self.sequence > MAX_SAFE_JSON_INTEGER {
                    return Err("timeline anchor sequence is outside the safe integer range");
                }
                match (self.sequence, self.event_id.as_deref()) {
                    (0, None) => Ok(()),
                    (0, Some(_)) => Err("timeline anchor zero sequence must not have an event ID"),
                    (_, Some(event_id)) if valid_timeline_event_id(event_id) => Ok(()),
                    (_, Some(_)) => Err("timeline anchor event identity is invalid"),
                    (_, None) => Err("positive timeline anchor must have an event ID"),
                }
            }

            pub fn initial() -> Self {
                Self {
                    sequence: 0,
                    event_id: None,
                }
            }
        }

        impl TryFrom<TimelineAnchorWire> for TimelineAnchor {
            type Error = &'static str;

            fn try_from(value: TimelineAnchorWire) -> Result<Self, Self::Error> {
                let event_id = match value.event_id {
                    Value::Null => None,
                    Value::String(event_id) => Some(event_id),
                    _ => return Err("timeline anchor event identity must be a string or null"),
                };
                let anchor = Self {
                    sequence: value.sequence,
                    event_id,
                };
                anchor.validate()?;
                Ok(anchor)
            }
        }

        impl<'de> Deserialize<'de> for TimelineAnchor {
            fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
            where
                D: serde::Deserializer<'de>,
            {
                TimelineAnchorWire::deserialize(deserializer)?
                    .try_into()
                    .map_err(serde::de::Error::custom)
            }
        }

        impl Serialize for TimelineAnchor {
            fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
            where
                S: serde::Serializer,
            {
                self.validate().map_err(serde::ser::Error::custom)?;
                #[derive(Serialize)]
                struct AnchorRef<'a> {
                    sequence: u64,
                    event_id: Option<&'a str>,
                }
                AnchorRef {
                    sequence: self.sequence,
                    event_id: self.event_id.as_deref(),
                }
                .serialize(serializer)
            }
        }

        fn parse_nullable_timeline_anchor(
            value: Value,
        ) -> Result<Option<TimelineAnchor>, &'static str> {
            if value.is_null() {
                Ok(None)
            } else {
                serde_json::from_value(value)
                    .map(Some)
                    .map_err(|_| "nullable timeline anchor is invalid")
            }
        }

        fn validate_timeline_window(
            after: &TimelineAnchor,
            watermark: &TimelineAnchor,
        ) -> Result<(), &'static str> {
            after.validate()?;
            watermark.validate()?;
            if after.sequence > watermark.sequence {
                return Err("timeline sync after anchor exceeds the fixed watermark");
            }
            if after.sequence == watermark.sequence && after != watermark {
                return Err("timeline sync anchors disagree at the same sequence");
            }
            Ok(())
        }

        #[derive(Debug, Clone, PartialEq, Eq)]
        pub struct TimelineSyncParams {
            pub session_id: String,
            pub after: TimelineAnchor,
            pub watermark: Option<TimelineAnchor>,
            pub limit: u64,
        }

        #[derive(Debug, Deserialize)]
        #[serde(deny_unknown_fields)]
        struct TimelineSyncParamsWire {
            session_id: String,
            after: TimelineAnchor,
            watermark: Value,
            #[serde(deserialize_with = "deserialize_timeline_sync_limit")]
            limit: u64,
        }

        impl TimelineSyncParams {
            pub fn validate(&self) -> Result<(), &'static str> {
                if !valid_ascii_graphical(&self.session_id, MAX_TIMELINE_IDENTIFIER_BYTES) {
                    return Err("timeline sync session identity is invalid");
                }
                self.after.validate()?;
                if let Some(watermark) = &self.watermark {
                    validate_timeline_window(&self.after, watermark)?;
                }
                if self.limit == 0 || self.limit > MAX_TIMELINE_SYNC_EVENTS as u64 {
                    return Err("timeline sync limit is outside the supported range");
                }
                Ok(())
            }
        }

        impl TryFrom<TimelineSyncParamsWire> for TimelineSyncParams {
            type Error = &'static str;

            fn try_from(value: TimelineSyncParamsWire) -> Result<Self, Self::Error> {
                let params = Self {
                    session_id: value.session_id,
                    after: value.after,
                    watermark: parse_nullable_timeline_anchor(value.watermark)?,
                    limit: value.limit,
                };
                params.validate()?;
                Ok(params)
            }
        }

        impl<'de> Deserialize<'de> for TimelineSyncParams {
            fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
            where
                D: serde::Deserializer<'de>,
            {
                TimelineSyncParamsWire::deserialize(deserializer)?
                    .try_into()
                    .map_err(serde::de::Error::custom)
            }
        }

        impl Serialize for TimelineSyncParams {
            fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
            where
                S: serde::Serializer,
            {
                self.validate().map_err(serde::ser::Error::custom)?;
                #[derive(Serialize)]
                struct ParamsRef<'a> {
                    session_id: &'a str,
                    after: &'a TimelineAnchor,
                    watermark: Option<&'a TimelineAnchor>,
                    limit: u64,
                }
                ParamsRef {
                    session_id: &self.session_id,
                    after: &self.after,
                    watermark: self.watermark.as_ref(),
                    limit: self.limit,
                }
                .serialize(serializer)
            }
        }

        #[derive(Debug, Clone, PartialEq, Eq)]
        pub struct TimelineRetentionGapData {
            pub schema_version: String,
            pub reason: String,
            pub session_id: String,
            pub requested_after: TimelineAnchor,
            pub requested_watermark: Option<TimelineAnchor>,
            pub retained_floor: TimelineAnchor,
            pub head: TimelineAnchor,
            pub snapshot_required: bool,
            pub snapshot_available: bool,
            pub snapshot_capability: String,
            pub snapshot_method: String,
            pub event_history_complete: bool,
            pub replay_from_floor_allowed: bool,
        }

        #[derive(Debug, Deserialize)]
        #[serde(deny_unknown_fields)]
        struct TimelineRetentionGapDataWire {
            schema_version: String,
            reason: String,
            session_id: String,
            requested_after: TimelineAnchor,
            requested_watermark: Value,
            retained_floor: TimelineAnchor,
            head: TimelineAnchor,
            snapshot_required: bool,
            snapshot_available: bool,
            snapshot_capability: String,
            snapshot_method: String,
            event_history_complete: bool,
            replay_from_floor_allowed: bool,
        }

        impl TimelineRetentionGapData {
            pub fn validate(&self) -> Result<(), &'static str> {
                if self.schema_version != "timeline-retention-gap/0.1" {
                    return Err("timeline retention gap schema version is invalid");
                }
                if self.reason != "requested-anchor-not-retained" {
                    return Err("timeline retention gap reason is invalid");
                }
                if !valid_ascii_graphical(&self.session_id, MAX_TIMELINE_IDENTIFIER_BYTES) {
                    return Err("timeline retention gap session identity is invalid");
                }
                self.requested_after.validate()?;
                if let Some(watermark) = &self.requested_watermark {
                    validate_timeline_window(&self.requested_after, watermark)?;
                }
                validate_timeline_window(&self.retained_floor, &self.head)?;
                if self.requested_after.sequence >= self.retained_floor.sequence {
                    return Err("timeline retention gap request is not before the retained floor");
                }
                if !self.snapshot_required {
                    return Err("timeline retention gap must require snapshot recovery");
                }
                if self.snapshot_capability != "timeline.snapshot.current"
                    || self.snapshot_method != "timeline/snapshot"
                {
                    return Err("timeline retention gap snapshot recovery route is invalid");
                }
                if self.event_history_complete || self.replay_from_floor_allowed {
                    return Err("timeline retention gap cannot claim complete replay history");
                }
                Ok(())
            }

            pub fn validate_for_request(
                &self,
                request: &TimelineSyncParams,
            ) -> Result<(), &'static str> {
                self.validate()?;
                request.validate()?;
                if self.session_id != request.session_id
                    || self.requested_after != request.after
                    || self.requested_watermark != request.watermark
                {
                    return Err("timeline retention gap does not match its request");
                }
                Ok(())
            }
        }

        impl TryFrom<TimelineRetentionGapDataWire> for TimelineRetentionGapData {
            type Error = &'static str;

            fn try_from(value: TimelineRetentionGapDataWire) -> Result<Self, Self::Error> {
                let data = Self {
                    schema_version: value.schema_version,
                    reason: value.reason,
                    session_id: value.session_id,
                    requested_after: value.requested_after,
                    requested_watermark: parse_nullable_timeline_anchor(value.requested_watermark)?,
                    retained_floor: value.retained_floor,
                    head: value.head,
                    snapshot_required: value.snapshot_required,
                    snapshot_available: value.snapshot_available,
                    snapshot_capability: value.snapshot_capability,
                    snapshot_method: value.snapshot_method,
                    event_history_complete: value.event_history_complete,
                    replay_from_floor_allowed: value.replay_from_floor_allowed,
                };
                data.validate()?;
                Ok(data)
            }
        }

        impl<'de> Deserialize<'de> for TimelineRetentionGapData {
            fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
            where
                D: serde::Deserializer<'de>,
            {
                TimelineRetentionGapDataWire::deserialize(deserializer)?
                    .try_into()
                    .map_err(serde::de::Error::custom)
            }
        }

        impl Serialize for TimelineRetentionGapData {
            fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
            where
                S: serde::Serializer,
            {
                self.validate().map_err(serde::ser::Error::custom)?;
                #[derive(Serialize)]
                struct DataRef<'a> {
                    schema_version: &'a str,
                    reason: &'a str,
                    session_id: &'a str,
                    requested_after: &'a TimelineAnchor,
                    requested_watermark: Option<&'a TimelineAnchor>,
                    retained_floor: &'a TimelineAnchor,
                    head: &'a TimelineAnchor,
                    snapshot_required: bool,
                    snapshot_available: bool,
                    snapshot_capability: &'a str,
                    snapshot_method: &'a str,
                    event_history_complete: bool,
                    replay_from_floor_allowed: bool,
                }
                DataRef {
                    schema_version: &self.schema_version,
                    reason: &self.reason,
                    session_id: &self.session_id,
                    requested_after: &self.requested_after,
                    requested_watermark: self.requested_watermark.as_ref(),
                    retained_floor: &self.retained_floor,
                    head: &self.head,
                    snapshot_required: self.snapshot_required,
                    snapshot_available: self.snapshot_available,
                    snapshot_capability: &self.snapshot_capability,
                    snapshot_method: &self.snapshot_method,
                    event_history_complete: self.event_history_complete,
                    replay_from_floor_allowed: self.replay_from_floor_allowed,
                }
                .serialize(serializer)
            }
        }

        fn deserialize_timeline_sync_events<'de, D>(
            deserializer: D,
        ) -> Result<Vec<EventEnvelope>, D::Error>
        where
            D: serde::Deserializer<'de>,
        {
            struct TimelineEventsVisitor;

            impl<'de> serde::de::Visitor<'de> for TimelineEventsVisitor {
                type Value = Vec<EventEnvelope>;

                fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                    formatter.write_str("a bounded array of timeline events")
                }

                fn visit_seq<A>(self, mut sequence: A) -> Result<Self::Value, A::Error>
                where
                    A: serde::de::SeqAccess<'de>,
                {
                    let mut events = Vec::with_capacity(
                        sequence
                            .size_hint()
                            .unwrap_or_default()
                            .min(MAX_TIMELINE_SYNC_EVENTS),
                    );
                    while let Some(event) = sequence.next_element()? {
                        if events.len() == MAX_TIMELINE_SYNC_EVENTS {
                            return Err(serde::de::Error::custom(
                                "timeline sync event count exceeds the limit",
                            ));
                        }
                        events.push(event);
                    }
                    Ok(events)
                }
            }

            deserializer.deserialize_seq(TimelineEventsVisitor)
        }

        #[derive(Debug, Clone, PartialEq, Eq)]
        pub struct TimelineSyncPage {
            pub schema_version: String,
            pub session_id: String,
            pub after: TimelineAnchor,
            pub watermark: TimelineAnchor,
            pub events: Vec<EventEnvelope>,
            pub next_after: Option<TimelineAnchor>,
            pub complete: bool,
        }

        #[derive(Debug, Deserialize)]
        #[serde(deny_unknown_fields)]
        struct TimelineSyncPageWire {
            schema_version: String,
            session_id: String,
            after: TimelineAnchor,
            watermark: TimelineAnchor,
            #[serde(deserialize_with = "deserialize_timeline_sync_events")]
            events: Vec<EventEnvelope>,
            next_after: Value,
            complete: bool,
        }

        impl TimelineSyncPage {
            pub fn validate(&self) -> Result<(), &'static str> {
                if self.schema_version != "timeline-sync-page/0.1" {
                    return Err("timeline sync page schema version is invalid");
                }
                if !valid_ascii_graphical(&self.session_id, MAX_TIMELINE_IDENTIFIER_BYTES) {
                    return Err("timeline sync page session identity is invalid");
                }
                validate_timeline_window(&self.after, &self.watermark)?;
                if self.events.len() > MAX_TIMELINE_SYNC_EVENTS {
                    return Err("timeline sync event count exceeds the limit");
                }

                let mut expected_sequence = self.after.sequence;
                for event in &self.events {
                    event.validate()?;
                    expected_sequence = expected_sequence
                        .checked_add(1)
                        .ok_or("timeline sync event sequence overflowed")?;
                    if event.session_id != self.session_id {
                        return Err("timeline sync event belongs to a different session");
                    }
                    if event.sequence != expected_sequence {
                        return Err("timeline sync events are not contiguous after the anchor");
                    }
                    if event.sequence > self.watermark.sequence {
                        return Err("timeline sync event exceeds the fixed watermark");
                    }
                    if event.sequence == self.watermark.sequence
                        && self.watermark.event_id.as_deref() != Some(event.event_id.as_str())
                    {
                        return Err("timeline sync watermark identity does not match its event");
                    }
                }

                let final_anchor = self.events.last().map(|event| TimelineAnchor {
                    sequence: event.sequence,
                    event_id: Some(event.event_id.clone()),
                });
                if self.complete {
                    if self.next_after.is_some() {
                        return Err("complete timeline sync page must not have a next anchor");
                    }
                    let reached = final_anchor.as_ref().unwrap_or(&self.after);
                    if reached != &self.watermark {
                        return Err("complete timeline sync page did not reach its watermark");
                    }
                } else {
                    let final_anchor = final_anchor
                        .as_ref()
                        .ok_or("incomplete timeline sync page must contain an event")?;
                    if self.next_after.as_ref() != Some(final_anchor) {
                        return Err("timeline sync next anchor does not match the final event");
                    }
                    if final_anchor.sequence >= self.watermark.sequence {
                        return Err("incomplete timeline sync page reached its watermark");
                    }
                }
                if let Some(next_after) = &self.next_after {
                    next_after.validate()?;
                }
                Ok(())
            }

            pub fn validate_for_request(
                &self,
                request: &TimelineSyncParams,
            ) -> Result<(), &'static str> {
                request.validate()?;
                self.validate()?;
                if self.session_id != request.session_id {
                    return Err("timeline sync response session does not match the request");
                }
                if self.after != request.after {
                    return Err("timeline sync response after anchor does not match the request");
                }
                if request
                    .watermark
                    .as_ref()
                    .is_some_and(|watermark| watermark != &self.watermark)
                {
                    return Err("timeline sync response changed the requested fixed watermark");
                }
                if self.events.len() as u64 > request.limit {
                    return Err("timeline sync response exceeds the requested limit");
                }
                Ok(())
            }
        }

        impl TryFrom<TimelineSyncPageWire> for TimelineSyncPage {
            type Error = &'static str;

            fn try_from(value: TimelineSyncPageWire) -> Result<Self, Self::Error> {
                let page = Self {
                    schema_version: value.schema_version,
                    session_id: value.session_id,
                    after: value.after,
                    watermark: value.watermark,
                    events: value.events,
                    next_after: parse_nullable_timeline_anchor(value.next_after)?,
                    complete: value.complete,
                };
                page.validate()?;
                Ok(page)
            }
        }

        impl<'de> Deserialize<'de> for TimelineSyncPage {
            fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
            where
                D: serde::Deserializer<'de>,
            {
                TimelineSyncPageWire::deserialize(deserializer)?
                    .try_into()
                    .map_err(serde::de::Error::custom)
            }
        }

        impl Serialize for TimelineSyncPage {
            fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
            where
                S: serde::Serializer,
            {
                self.validate().map_err(serde::ser::Error::custom)?;
                #[derive(Serialize)]
                struct PageRef<'a> {
                    schema_version: &'a str,
                    session_id: &'a str,
                    after: &'a TimelineAnchor,
                    watermark: &'a TimelineAnchor,
                    events: &'a [EventEnvelope],
                    next_after: Option<&'a TimelineAnchor>,
                    complete: bool,
                }
                PageRef {
                    schema_version: &self.schema_version,
                    session_id: &self.session_id,
                    after: &self.after,
                    watermark: &self.watermark,
                    events: &self.events,
                    next_after: self.next_after.as_ref(),
                    complete: self.complete,
                }
                .serialize(serializer)
            }
        }

        fn parse_nullable_timeline_snapshot_id(
            value: Value,
        ) -> Result<Option<String>, &'static str> {
            match value {
                Value::Null => Ok(None),
                Value::String(snapshot_id) if valid_timeline_snapshot_id(&snapshot_id) => {
                    Ok(Some(snapshot_id))
                }
                _ => Err("timeline snapshot identity is invalid"),
            }
        }

        #[derive(Debug, Clone, PartialEq, Eq)]
        pub struct TimelineSnapshotCursor {
            pub ordinal: u64,
            pub item_id: String,
            pub item_identity: String,
        }

        #[derive(Deserialize)]
        #[serde(deny_unknown_fields)]
        struct TimelineSnapshotCursorWire {
            #[serde(deserialize_with = "deserialize_positive_safe_json_integer")]
            ordinal: u64,
            item_id: String,
            item_identity: String,
        }

        impl TimelineSnapshotCursor {
            pub fn validate(&self) -> Result<(), &'static str> {
                if self.ordinal == 0 || self.ordinal > MAX_SAFE_JSON_INTEGER {
                    return Err("timeline snapshot cursor ordinal is invalid");
                }
                if !valid_ascii_graphical(&self.item_id, MAX_TIMELINE_IDENTIFIER_BYTES) {
                    return Err("timeline snapshot cursor Item identity is invalid");
                }
                if !valid_timeline_snapshot_item_id(&self.item_identity) {
                    return Err("timeline snapshot cursor material identity is invalid");
                }
                Ok(())
            }
        }

        impl TryFrom<TimelineSnapshotCursorWire> for TimelineSnapshotCursor {
            type Error = &'static str;

            fn try_from(value: TimelineSnapshotCursorWire) -> Result<Self, Self::Error> {
                let cursor = Self {
                    ordinal: value.ordinal,
                    item_id: value.item_id,
                    item_identity: value.item_identity,
                };
                cursor.validate()?;
                Ok(cursor)
            }
        }

        impl<'de> Deserialize<'de> for TimelineSnapshotCursor {
            fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
            where
                D: serde::Deserializer<'de>,
            {
                TimelineSnapshotCursorWire::deserialize(deserializer)?
                    .try_into()
                    .map_err(serde::de::Error::custom)
            }
        }

        impl Serialize for TimelineSnapshotCursor {
            fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
            where
                S: serde::Serializer,
            {
                self.validate().map_err(serde::ser::Error::custom)?;
                #[derive(Serialize)]
                struct CursorRef<'a> {
                    ordinal: u64,
                    item_id: &'a str,
                    item_identity: &'a str,
                }
                CursorRef {
                    ordinal: self.ordinal,
                    item_id: &self.item_id,
                    item_identity: &self.item_identity,
                }
                .serialize(serializer)
            }
        }

        fn parse_nullable_snapshot_cursor(
            value: Value,
        ) -> Result<Option<TimelineSnapshotCursor>, &'static str> {
            if value.is_null() {
                return Ok(None);
            }
            let cursor: TimelineSnapshotCursor =
                serde_json::from_value(value).map_err(|_| "timeline snapshot cursor is invalid")?;
            cursor.validate()?;
            Ok(Some(cursor))
        }

        #[derive(Debug, Clone, PartialEq, Eq)]
        pub struct TimelineSnapshotParams {
            pub session_id: String,
            pub snapshot_identity: Option<String>,
            pub watermark: Option<TimelineAnchor>,
            pub after: Option<TimelineSnapshotCursor>,
            pub limit: u64,
        }

        #[derive(Debug, Deserialize)]
        #[serde(deny_unknown_fields)]
        struct TimelineSnapshotParamsWire {
            session_id: String,
            snapshot_identity: Value,
            watermark: Value,
            after: Value,
            #[serde(deserialize_with = "deserialize_timeline_sync_limit")]
            limit: u64,
        }

        impl TimelineSnapshotParams {
            pub fn validate(&self) -> Result<(), &'static str> {
                if !valid_ascii_graphical(&self.session_id, MAX_TIMELINE_IDENTIFIER_BYTES) {
                    return Err("timeline snapshot session identity is invalid");
                }
                if self.limit == 0 || self.limit > MAX_TIMELINE_SNAPSHOT_ITEMS as u64 {
                    return Err("timeline snapshot limit is outside the supported range");
                }
                if let Some(watermark) = &self.watermark {
                    watermark.validate()?;
                }
                if let Some(after) = &self.after {
                    after.validate()?;
                }
                if self
                    .watermark
                    .as_ref()
                    .is_some_and(|watermark| watermark.sequence == 0 && self.after.is_some())
                {
                    return Err("timeline snapshot continuation cannot follow an empty watermark");
                }
                match (
                    self.snapshot_identity.as_deref(),
                    self.watermark.as_ref(),
                    self.after.as_ref(),
                ) {
                    (None, None, None) => Ok(()),
                    (Some(identity), Some(_), Some(_)) if valid_timeline_snapshot_id(identity) => {
                        Ok(())
                    }
                    (Some(_), Some(_), Some(_)) => Err("timeline snapshot identity is invalid"),
                    _ => Err("timeline snapshot continuation fields must be all null or all set"),
                }
            }
        }

        impl TryFrom<TimelineSnapshotParamsWire> for TimelineSnapshotParams {
            type Error = &'static str;

            fn try_from(value: TimelineSnapshotParamsWire) -> Result<Self, Self::Error> {
                let params = Self {
                    session_id: value.session_id,
                    snapshot_identity: parse_nullable_timeline_snapshot_id(
                        value.snapshot_identity,
                    )?,
                    watermark: parse_nullable_timeline_anchor(value.watermark)?,
                    after: parse_nullable_snapshot_cursor(value.after)?,
                    limit: value.limit,
                };
                params.validate()?;
                Ok(params)
            }
        }

        impl<'de> Deserialize<'de> for TimelineSnapshotParams {
            fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
            where
                D: serde::Deserializer<'de>,
            {
                TimelineSnapshotParamsWire::deserialize(deserializer)?
                    .try_into()
                    .map_err(serde::de::Error::custom)
            }
        }

        impl Serialize for TimelineSnapshotParams {
            fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
            where
                S: serde::Serializer,
            {
                self.validate().map_err(serde::ser::Error::custom)?;
                #[derive(Serialize)]
                struct ParamsRef<'a> {
                    session_id: &'a str,
                    snapshot_identity: Option<&'a str>,
                    watermark: Option<&'a TimelineAnchor>,
                    after: Option<&'a TimelineSnapshotCursor>,
                    limit: u64,
                }
                ParamsRef {
                    session_id: &self.session_id,
                    snapshot_identity: self.snapshot_identity.as_deref(),
                    watermark: self.watermark.as_ref(),
                    after: self.after.as_ref(),
                    limit: self.limit,
                }
                .serialize(serializer)
            }
        }

        #[derive(Debug, Clone, PartialEq, Eq)]
        pub struct TimelineSnapshotItem {
            pub ordinal: u64,
            pub item_identity: String,
            pub turn_id: String,
            pub correlation_id: String,
            pub turn_state: TurnState,
            pub first_event: TimelineAnchor,
            pub latest_event: TimelineAnchor,
            pub item: TimelineItem,
            pub item_update: ItemUpdate,
        }

        #[derive(Debug, Deserialize)]
        #[serde(deny_unknown_fields)]
        struct TimelineSnapshotItemWire {
            #[serde(deserialize_with = "deserialize_positive_safe_json_integer")]
            ordinal: u64,
            item_identity: String,
            turn_id: String,
            correlation_id: String,
            turn_state: TurnState,
            first_event: TimelineAnchor,
            latest_event: TimelineAnchor,
            item: TimelineItem,
            item_update: ItemUpdate,
        }

        impl TimelineSnapshotItem {
            fn validate_material(&self) -> Result<(), &'static str> {
                if self.ordinal == 0 || self.ordinal > MAX_SAFE_JSON_INTEGER {
                    return Err("timeline snapshot item ordinal is outside the safe integer range");
                }
                if !valid_ascii_graphical(&self.turn_id, MAX_TIMELINE_IDENTIFIER_BYTES)
                    || self.correlation_id != self.turn_id
                {
                    return Err("timeline snapshot Item Turn binding is invalid");
                }
                validate_timeline_window(&self.first_event, &self.latest_event)?;
                if self.first_event.sequence == 0 {
                    return Err("timeline snapshot Item must have a positive first event");
                }
                self.item.validate()?;
                self.item_update.validate()
            }

            pub fn validate(&self) -> Result<(), &'static str> {
                self.validate_material()?;
                if !valid_timeline_snapshot_item_id(&self.item_identity) {
                    return Err("timeline snapshot Item material identity is invalid");
                }
                Ok(())
            }
        }

        #[derive(Serialize)]
        struct TimelineSnapshotItemIdentityMaterial<'a> {
            schema_version: &'static str,
            session_id: &'a str,
            ordinal: u64,
            turn_id: &'a str,
            correlation_id: &'a str,
            turn_state: TurnState,
            first_event: &'a TimelineAnchor,
            latest_event: &'a TimelineAnchor,
            item: &'a TimelineItem,
            item_update: &'a ItemUpdate,
        }

        fn timeline_snapshot_item_identity_bytes(
            session_id: &str,
            item: &TimelineSnapshotItem,
        ) -> Result<Vec<u8>, &'static str> {
            if !valid_ascii_graphical(session_id, MAX_TIMELINE_IDENTIFIER_BYTES) {
                return Err("timeline snapshot session identity is invalid");
            }
            item.validate_material()?;
            serde_json::to_vec(&TimelineSnapshotItemIdentityMaterial {
                schema_version: "timeline-session-snapshot-item/0.1",
                session_id,
                ordinal: item.ordinal,
                turn_id: &item.turn_id,
                correlation_id: &item.correlation_id,
                turn_state: item.turn_state,
                first_event: &item.first_event,
                latest_event: &item.latest_event,
                item: &item.item,
                item_update: &item.item_update,
            })
            .map_err(|_| "timeline snapshot Item identity material is invalid")
        }

        pub fn timeline_snapshot_item_identity(
            session_id: &str,
            item: &TimelineSnapshotItem,
        ) -> Result<String, &'static str> {
            let encoded = timeline_snapshot_item_identity_bytes(session_id, item)?;
            let encoded_len = u64::try_from(encoded.len())
                .map_err(|_| "timeline snapshot Item identity material is too large")?;
            let mut digest = Sha256::new();
            digest.update(b"aegisy-timeline-session-snapshot-item/0.1\0");
            digest.update(encoded_len.to_be_bytes());
            digest.update(encoded);
            Ok(format!(
                "timeline-session-snapshot-item:sha256:{:x}",
                digest.finalize()
            ))
        }

        pub fn timeline_snapshot_item_canonical_bytes(
            session_id: &str,
            item: &TimelineSnapshotItem,
        ) -> Result<u64, &'static str> {
            u64::try_from(timeline_snapshot_item_identity_bytes(session_id, item)?.len())
                .map_err(|_| "timeline snapshot Item identity material is too large")
        }

        impl TryFrom<TimelineSnapshotItemWire> for TimelineSnapshotItem {
            type Error = &'static str;

            fn try_from(value: TimelineSnapshotItemWire) -> Result<Self, Self::Error> {
                let item = Self {
                    ordinal: value.ordinal,
                    item_identity: value.item_identity,
                    turn_id: value.turn_id,
                    correlation_id: value.correlation_id,
                    turn_state: value.turn_state,
                    first_event: value.first_event,
                    latest_event: value.latest_event,
                    item: value.item,
                    item_update: value.item_update,
                };
                item.validate()?;
                Ok(item)
            }
        }

        impl<'de> Deserialize<'de> for TimelineSnapshotItem {
            fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
            where
                D: serde::Deserializer<'de>,
            {
                TimelineSnapshotItemWire::deserialize(deserializer)?
                    .try_into()
                    .map_err(serde::de::Error::custom)
            }
        }

        impl Serialize for TimelineSnapshotItem {
            fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
            where
                S: serde::Serializer,
            {
                self.validate().map_err(serde::ser::Error::custom)?;
                #[derive(Serialize)]
                struct ItemRef<'a> {
                    ordinal: u64,
                    item_identity: &'a str,
                    turn_id: &'a str,
                    correlation_id: &'a str,
                    turn_state: TurnState,
                    first_event: &'a TimelineAnchor,
                    latest_event: &'a TimelineAnchor,
                    item: &'a TimelineItem,
                    item_update: &'a ItemUpdate,
                }
                ItemRef {
                    ordinal: self.ordinal,
                    item_identity: &self.item_identity,
                    turn_id: &self.turn_id,
                    correlation_id: &self.correlation_id,
                    turn_state: self.turn_state,
                    first_event: &self.first_event,
                    latest_event: &self.latest_event,
                    item: &self.item,
                    item_update: &self.item_update,
                }
                .serialize(serializer)
            }
        }

        #[derive(Debug, Clone, PartialEq, Eq)]
        pub struct TimelineSnapshotActiveTurn {
            pub turn_id: String,
            pub correlation_id: String,
            pub state: TurnState,
            pub started_event: TimelineAnchor,
            pub latest_event: TimelineAnchor,
            pub open_item_ids: Vec<String>,
        }

        fn deserialize_snapshot_open_item_ids<'de, D>(
            deserializer: D,
        ) -> Result<Vec<String>, D::Error>
        where
            D: serde::Deserializer<'de>,
        {
            struct OpenItemIdsVisitor;

            impl<'de> serde::de::Visitor<'de> for OpenItemIdsVisitor {
                type Value = Vec<String>;

                fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                    formatter.write_str("a bounded ordered array of open Item IDs")
                }

                fn visit_seq<A>(self, mut sequence: A) -> Result<Self::Value, A::Error>
                where
                    A: serde::de::SeqAccess<'de>,
                {
                    let mut values = Vec::with_capacity(
                        sequence
                            .size_hint()
                            .unwrap_or_default()
                            .min(MAX_TIMELINE_SNAPSHOT_TOTAL_ITEMS),
                    );
                    while let Some(value) = sequence.next_element::<String>()? {
                        if values.len() == MAX_TIMELINE_SNAPSHOT_TOTAL_ITEMS {
                            return Err(serde::de::Error::custom(
                                "timeline snapshot open Item count exceeds the limit",
                            ));
                        }
                        values.push(value);
                    }
                    Ok(values)
                }
            }

            deserializer.deserialize_seq(OpenItemIdsVisitor)
        }

        #[derive(Debug, Deserialize)]
        #[serde(deny_unknown_fields)]
        struct TimelineSnapshotActiveTurnWire {
            turn_id: String,
            correlation_id: String,
            state: TurnState,
            started_event: TimelineAnchor,
            latest_event: TimelineAnchor,
            #[serde(deserialize_with = "deserialize_snapshot_open_item_ids")]
            open_item_ids: Vec<String>,
        }

        impl TimelineSnapshotActiveTurn {
            pub fn validate(&self) -> Result<(), &'static str> {
                if !valid_ascii_graphical(&self.turn_id, MAX_TIMELINE_IDENTIFIER_BYTES)
                    || self.correlation_id != self.turn_id
                {
                    return Err("timeline snapshot active Turn binding is invalid");
                }
                if self.state != TurnState::Running {
                    return Err("timeline snapshot active Turn must be running");
                }
                validate_timeline_window(&self.started_event, &self.latest_event)?;
                if self.started_event.sequence == 0 {
                    return Err("timeline snapshot active Turn must have a positive start event");
                }
                if self.open_item_ids.len() > MAX_TIMELINE_SNAPSHOT_TOTAL_ITEMS {
                    return Err("timeline snapshot active Turn open Item count exceeds the limit");
                }
                let mut unique = HashSet::with_capacity(self.open_item_ids.len());
                for item_id in &self.open_item_ids {
                    if !valid_ascii_graphical(item_id, MAX_TIMELINE_IDENTIFIER_BYTES)
                        || !unique.insert(item_id.as_str())
                    {
                        return Err("timeline snapshot active Turn open Item order is invalid");
                    }
                }
                Ok(())
            }
        }

        impl TryFrom<TimelineSnapshotActiveTurnWire> for TimelineSnapshotActiveTurn {
            type Error = &'static str;

            fn try_from(value: TimelineSnapshotActiveTurnWire) -> Result<Self, Self::Error> {
                let active_turn = Self {
                    turn_id: value.turn_id,
                    correlation_id: value.correlation_id,
                    state: value.state,
                    started_event: value.started_event,
                    latest_event: value.latest_event,
                    open_item_ids: value.open_item_ids,
                };
                active_turn.validate()?;
                Ok(active_turn)
            }
        }

        impl<'de> Deserialize<'de> for TimelineSnapshotActiveTurn {
            fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
            where
                D: serde::Deserializer<'de>,
            {
                TimelineSnapshotActiveTurnWire::deserialize(deserializer)?
                    .try_into()
                    .map_err(serde::de::Error::custom)
            }
        }

        impl Serialize for TimelineSnapshotActiveTurn {
            fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
            where
                S: serde::Serializer,
            {
                self.validate().map_err(serde::ser::Error::custom)?;
                #[derive(Serialize)]
                struct ActiveTurnRef<'a> {
                    turn_id: &'a str,
                    correlation_id: &'a str,
                    state: TurnState,
                    started_event: &'a TimelineAnchor,
                    latest_event: &'a TimelineAnchor,
                    open_item_ids: &'a [String],
                }
                ActiveTurnRef {
                    turn_id: &self.turn_id,
                    correlation_id: &self.correlation_id,
                    state: self.state,
                    started_event: &self.started_event,
                    latest_event: &self.latest_event,
                    open_item_ids: &self.open_item_ids,
                }
                .serialize(serializer)
            }
        }

        fn parse_nullable_snapshot_active_turn(
            value: Value,
        ) -> Result<Option<TimelineSnapshotActiveTurn>, &'static str> {
            if value.is_null() {
                Ok(None)
            } else {
                serde_json::from_value(value)
                    .map(Some)
                    .map_err(|_| "timeline snapshot active Turn is invalid")
            }
        }

        fn deserialize_timeline_snapshot_items<'de, D>(
            deserializer: D,
        ) -> Result<Vec<TimelineSnapshotItem>, D::Error>
        where
            D: serde::Deserializer<'de>,
        {
            struct SnapshotItemsVisitor;

            impl<'de> serde::de::Visitor<'de> for SnapshotItemsVisitor {
                type Value = Vec<TimelineSnapshotItem>;

                fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                    formatter.write_str("a bounded array of timeline snapshot items")
                }

                fn visit_seq<A>(self, mut sequence: A) -> Result<Self::Value, A::Error>
                where
                    A: serde::de::SeqAccess<'de>,
                {
                    let mut items = Vec::with_capacity(
                        sequence
                            .size_hint()
                            .unwrap_or_default()
                            .min(MAX_TIMELINE_SNAPSHOT_ITEMS),
                    );
                    while let Some(item) = sequence.next_element()? {
                        if items.len() == MAX_TIMELINE_SNAPSHOT_ITEMS {
                            return Err(serde::de::Error::custom(
                                "timeline snapshot item count exceeds the limit",
                            ));
                        }
                        items.push(item);
                    }
                    Ok(items)
                }
            }

            deserializer.deserialize_seq(SnapshotItemsVisitor)
        }

        #[derive(Debug, Clone, PartialEq, Eq)]
        pub struct TimelineSessionSnapshotPage {
            pub schema_version: String,
            pub session_id: String,
            pub snapshot_identity: String,
            pub floor: TimelineAnchor,
            pub watermark: TimelineAnchor,
            pub active_turn: Option<TimelineSnapshotActiveTurn>,
            pub total_items: u64,
            pub total_canonical_bytes: u64,
            pub after: Option<TimelineSnapshotCursor>,
            pub items: Vec<TimelineSnapshotItem>,
            pub next_after: Option<TimelineSnapshotCursor>,
            pub complete: bool,
            pub page_identity: String,
        }

        #[derive(Debug, Deserialize)]
        #[serde(deny_unknown_fields)]
        struct TimelineSessionSnapshotPageWire {
            schema_version: String,
            session_id: String,
            snapshot_identity: String,
            floor: TimelineAnchor,
            watermark: TimelineAnchor,
            active_turn: Value,
            #[serde(deserialize_with = "deserialize_nonnegative_safe_json_integer")]
            total_items: u64,
            #[serde(deserialize_with = "deserialize_nonnegative_safe_json_integer")]
            total_canonical_bytes: u64,
            after: Value,
            #[serde(deserialize_with = "deserialize_timeline_snapshot_items")]
            items: Vec<TimelineSnapshotItem>,
            next_after: Value,
            complete: bool,
            page_identity: String,
        }

        #[derive(Serialize)]
        struct TimelineSessionSnapshotPageRef<'a> {
            schema_version: &'a str,
            session_id: &'a str,
            snapshot_identity: &'a str,
            floor: &'a TimelineAnchor,
            watermark: &'a TimelineAnchor,
            active_turn: Option<&'a TimelineSnapshotActiveTurn>,
            total_items: u64,
            total_canonical_bytes: u64,
            after: Option<&'a TimelineSnapshotCursor>,
            items: &'a [TimelineSnapshotItem],
            next_after: Option<&'a TimelineSnapshotCursor>,
            complete: bool,
            page_identity: &'a str,
        }

        fn domain_separated_sha256(prefix: &str, material: &[u8]) -> Result<String, &'static str> {
            let material_len = u64::try_from(material.len())
                .map_err(|_| "timeline snapshot identity material is too large")?;
            let mut digest = Sha256::new();
            digest.update(prefix.as_bytes());
            digest.update([0]);
            digest.update(material_len.to_be_bytes());
            digest.update(material);
            Ok(format!("{:x}", digest.finalize()))
        }

        pub fn timeline_snapshot_identity(
            session_id: &str,
            floor: &TimelineAnchor,
            watermark: &TimelineAnchor,
            active_turn: Option<&TimelineSnapshotActiveTurn>,
            total_items: u64,
            total_canonical_bytes: u64,
            ordered_item_identities: &[String],
        ) -> Result<String, &'static str> {
            if !valid_ascii_graphical(session_id, MAX_TIMELINE_IDENTIFIER_BYTES) {
                return Err("timeline snapshot session identity is invalid");
            }
            validate_timeline_window(floor, watermark)?;
            if total_items > MAX_TIMELINE_SNAPSHOT_TOTAL_ITEMS as u64
                || total_items != ordered_item_identities.len() as u64
                || total_canonical_bytes > MAX_TIMELINE_SNAPSHOT_TOTAL_BYTES
                || (total_items == 0) != (total_canonical_bytes == 0)
            {
                return Err("timeline snapshot complete identity totals are invalid");
            }
            if let Some(active_turn) = active_turn {
                active_turn.validate()?;
                validate_timeline_window(&active_turn.latest_event, watermark)?;
                if active_turn.open_item_ids.len() as u64 > total_items {
                    return Err("timeline snapshot open Item count exceeds total Items");
                }
            }
            if ordered_item_identities
                .iter()
                .any(|identity| !valid_timeline_snapshot_item_id(identity))
            {
                return Err("timeline snapshot ordered Item identity is invalid");
            }
            #[derive(Serialize)]
            struct SnapshotIdentityMaterial<'a> {
                schema_version: &'static str,
                session_id: &'a str,
                floor: &'a TimelineAnchor,
                watermark: &'a TimelineAnchor,
                active_turn: Option<&'a TimelineSnapshotActiveTurn>,
                total_items: u64,
                total_canonical_bytes: u64,
                ordered_item_identities: &'a [String],
            }
            let encoded = serde_json::to_vec(&SnapshotIdentityMaterial {
                schema_version: "timeline-session-snapshot-page/0.1",
                session_id,
                floor,
                watermark,
                active_turn,
                total_items,
                total_canonical_bytes,
                ordered_item_identities,
            })
            .map_err(|_| "timeline snapshot complete identity material is invalid")?;
            Ok(format!(
                "timeline-session-snapshot:sha256:{}",
                domain_separated_sha256("aegisy-timeline-session-snapshot/0.1", &encoded)?
            ))
        }

        pub fn timeline_snapshot_page_identity(
            page: &TimelineSessionSnapshotPage,
        ) -> Result<String, &'static str> {
            if !valid_timeline_snapshot_id(&page.snapshot_identity) {
                return Err("timeline snapshot identity is invalid");
            }
            if let Some(after) = &page.after {
                after.validate()?;
            }
            if let Some(next_after) = &page.next_after {
                next_after.validate()?;
            }
            if page
                .items
                .iter()
                .any(|item| !valid_timeline_snapshot_item_id(&item.item_identity))
            {
                return Err("timeline snapshot page Item identity is invalid");
            }
            #[derive(Serialize)]
            struct PageIdentityMaterial<'a> {
                schema_version: &'static str,
                snapshot_identity: &'a str,
                after: Option<&'a TimelineSnapshotCursor>,
                ordered_item_identities: Vec<&'a str>,
                next_after: Option<&'a TimelineSnapshotCursor>,
                complete: bool,
            }
            let encoded = serde_json::to_vec(&PageIdentityMaterial {
                schema_version: "timeline-session-snapshot-page/0.1",
                snapshot_identity: &page.snapshot_identity,
                after: page.after.as_ref(),
                ordered_item_identities: page
                    .items
                    .iter()
                    .map(|item| item.item_identity.as_str())
                    .collect(),
                next_after: page.next_after.as_ref(),
                complete: page.complete,
            })
            .map_err(|_| "timeline snapshot page identity material is invalid")?;
            Ok(format!(
                "timeline-session-snapshot-page:sha256:{}",
                domain_separated_sha256("aegisy-timeline-session-snapshot-page/0.1", &encoded)?
            ))
        }

        impl TimelineSessionSnapshotPage {
            fn as_ref(&self) -> TimelineSessionSnapshotPageRef<'_> {
                TimelineSessionSnapshotPageRef {
                    schema_version: &self.schema_version,
                    session_id: &self.session_id,
                    snapshot_identity: &self.snapshot_identity,
                    floor: &self.floor,
                    watermark: &self.watermark,
                    active_turn: self.active_turn.as_ref(),
                    total_items: self.total_items,
                    total_canonical_bytes: self.total_canonical_bytes,
                    after: self.after.as_ref(),
                    items: &self.items,
                    next_after: self.next_after.as_ref(),
                    complete: self.complete,
                    page_identity: &self.page_identity,
                }
            }

            pub fn validate(&self) -> Result<(), &'static str> {
                if self.schema_version != "timeline-session-snapshot-page/0.1" {
                    return Err("timeline snapshot page schema version is invalid");
                }
                if !valid_ascii_graphical(&self.session_id, MAX_TIMELINE_IDENTIFIER_BYTES) {
                    return Err("timeline snapshot page session identity is invalid");
                }
                if !valid_timeline_snapshot_id(&self.snapshot_identity) {
                    return Err("timeline snapshot page identity is invalid");
                }
                if !valid_timeline_snapshot_page_id(&self.page_identity) {
                    return Err("timeline snapshot page material identity is invalid");
                }
                validate_timeline_window(&self.floor, &self.watermark)?;
                if self.items.len() > MAX_TIMELINE_SNAPSHOT_ITEMS {
                    return Err("timeline snapshot item count exceeds the limit");
                }
                if self.total_items > MAX_TIMELINE_SNAPSHOT_TOTAL_ITEMS as u64
                    || self.total_canonical_bytes > MAX_TIMELINE_SNAPSHOT_TOTAL_BYTES
                    || (self.total_items == 0) != (self.total_canonical_bytes == 0)
                {
                    return Err("timeline snapshot complete state exceeds its bound");
                }
                if let Some(after) = &self.after {
                    after.validate()?;
                    if after.ordinal >= self.total_items {
                        return Err("timeline snapshot request cursor exceeds total Items");
                    }
                }
                if let Some(active_turn) = &self.active_turn {
                    active_turn.validate()?;
                    validate_timeline_window(&active_turn.latest_event, &self.watermark)?;
                    if active_turn.open_item_ids.len() as u64 > self.total_items {
                        return Err("timeline snapshot open Item count exceeds total Items");
                    }
                }

                let mut expected_ordinal = self.after.as_ref().map_or(0, |cursor| cursor.ordinal);
                let mut item_ids = HashSet::with_capacity(self.items.len());
                let mut item_identities = HashSet::with_capacity(self.items.len());
                let mut page_canonical_bytes = 0_u64;
                for snapshot_item in &self.items {
                    snapshot_item.validate()?;
                    expected_ordinal = expected_ordinal
                        .checked_add(1)
                        .ok_or("timeline snapshot Item ordinal overflowed")?;
                    if snapshot_item.ordinal != expected_ordinal {
                        return Err("timeline snapshot items are not contiguous after the cursor");
                    }
                    if !item_ids.insert(snapshot_item.item.id.as_str()) {
                        return Err("timeline snapshot page contains duplicate item identities");
                    }
                    if !item_identities.insert(snapshot_item.item_identity.as_str()) {
                        return Err(
                            "timeline snapshot page contains duplicate material identities",
                        );
                    }
                    validate_timeline_window(&snapshot_item.latest_event, &self.watermark)?;
                    if timeline_snapshot_item_identity(&self.session_id, snapshot_item)?
                        != snapshot_item.item_identity
                    {
                        return Err("timeline snapshot Item identity does not match its material");
                    }
                    let item_is_open =
                        matches!(snapshot_item.item.state.as_str(), "started" | "delta");
                    match &self.active_turn {
                        Some(active_turn)
                            if active_turn.open_item_ids.contains(&snapshot_item.item.id) =>
                        {
                            if snapshot_item.turn_id != active_turn.turn_id || !item_is_open {
                                return Err("timeline snapshot open Item binding is invalid");
                            }
                        }
                        Some(active_turn)
                            if snapshot_item.turn_id == active_turn.turn_id && item_is_open =>
                        {
                            return Err(
                                "timeline snapshot active Item is missing from open Item order",
                            );
                        }
                        None if item_is_open => {
                            return Err("timeline snapshot open Item requires an active Turn");
                        }
                        _ => {}
                    }
                    page_canonical_bytes = page_canonical_bytes
                        .checked_add(timeline_snapshot_item_canonical_bytes(
                            &self.session_id,
                            snapshot_item,
                        )?)
                        .ok_or("timeline snapshot Item byte total overflowed")?;
                }
                if page_canonical_bytes > self.total_canonical_bytes {
                    return Err("timeline snapshot page bytes exceed the complete byte total");
                }

                if self.complete {
                    if self.next_after.is_some() {
                        return Err("complete timeline snapshot page must not have a next cursor");
                    }
                    if expected_ordinal != self.total_items {
                        return Err("complete timeline snapshot page did not reach total Items");
                    }
                } else {
                    let final_item = self
                        .items
                        .last()
                        .ok_or("incomplete timeline snapshot page must contain an item")?;
                    let expected_cursor = TimelineSnapshotCursor {
                        ordinal: final_item.ordinal,
                        item_id: final_item.item.id.clone(),
                        item_identity: final_item.item_identity.clone(),
                    };
                    if self.next_after.as_ref() != Some(&expected_cursor) {
                        return Err("timeline snapshot next cursor does not match the final item");
                    }
                    if final_item.ordinal >= self.total_items {
                        return Err("incomplete timeline snapshot page reached total Items");
                    }
                }
                if self.watermark.sequence == 0
                    && (self.floor.sequence != 0
                        || self.total_items != 0
                        || !self.items.is_empty()
                        || self.active_turn.is_some())
                {
                    return Err("empty timeline watermark cannot contain visible snapshot state");
                }
                if timeline_snapshot_page_identity(self)? != self.page_identity {
                    return Err("timeline snapshot page identity does not match its material");
                }

                let encoded = serde_json::to_vec(&self.as_ref())
                    .map_err(|_| "timeline snapshot page cannot be encoded")?;
                if encoded.len() > MAX_TIMELINE_SNAPSHOT_PAGE_BYTES {
                    return Err("timeline snapshot page exceeds the safe AAP frame budget");
                }
                Ok(())
            }

            pub fn validate_for_request(
                &self,
                request: &TimelineSnapshotParams,
            ) -> Result<(), &'static str> {
                request.validate()?;
                self.validate()?;
                if self.session_id != request.session_id {
                    return Err("timeline snapshot response session does not match the request");
                }
                if self.after != request.after {
                    return Err("timeline snapshot response cursor does not match the request");
                }
                if request
                    .snapshot_identity
                    .as_ref()
                    .is_some_and(|identity| identity != &self.snapshot_identity)
                {
                    return Err("timeline snapshot response identity does not match the request");
                }
                if request
                    .watermark
                    .as_ref()
                    .is_some_and(|watermark| watermark != &self.watermark)
                {
                    return Err("timeline snapshot response watermark does not match the request");
                }
                if self.items.len() as u64 > request.limit {
                    return Err("timeline snapshot response exceeds the requested limit");
                }
                Ok(())
            }

            pub fn validate_continuation(
                &self,
                request: &TimelineSnapshotParams,
                next_page: &TimelineSessionSnapshotPage,
            ) -> Result<(), &'static str> {
                self.validate()?;
                if self.complete {
                    return Err("complete timeline snapshot page cannot be continued");
                }
                request.validate()?;
                if request.session_id != self.session_id
                    || request.watermark.as_ref() != Some(&self.watermark)
                    || request.snapshot_identity.as_deref() != Some(self.snapshot_identity.as_str())
                    || request.after.as_ref() != self.next_after.as_ref()
                {
                    return Err("timeline snapshot continuation request changed snapshot state");
                }
                next_page.validate_for_request(request)?;
                if next_page.active_turn != self.active_turn {
                    return Err("timeline snapshot continuation changed the active Turn");
                }
                if next_page.floor != self.floor
                    || next_page.total_items != self.total_items
                    || next_page.total_canonical_bytes != self.total_canonical_bytes
                {
                    return Err("timeline snapshot continuation changed fixed snapshot metadata");
                }
                Ok(())
            }

            pub fn validate_complete_identity(
                &self,
                ordered_item_identities: &[String],
            ) -> Result<(), &'static str> {
                self.validate()?;
                if !self.complete || ordered_item_identities.len() as u64 != self.total_items {
                    return Err(
                        "timeline snapshot identity requires the complete ordered Item set",
                    );
                }
                let expected = timeline_snapshot_identity(
                    &self.session_id,
                    &self.floor,
                    &self.watermark,
                    self.active_turn.as_ref(),
                    self.total_items,
                    self.total_canonical_bytes,
                    ordered_item_identities,
                )?;
                if expected != self.snapshot_identity {
                    return Err("timeline snapshot identity does not match complete material");
                }
                Ok(())
            }

            pub fn validate_complete_items(
                &self,
                ordered_items: &[TimelineSnapshotItem],
            ) -> Result<(), &'static str> {
                self.validate()?;
                if !self.complete || ordered_items.len() as u64 != self.total_items {
                    return Err("timeline snapshot requires the complete ordered Item set");
                }
                let mut item_ids = HashSet::with_capacity(ordered_items.len());
                let mut identities = Vec::with_capacity(ordered_items.len());
                let mut canonical_bytes = 0_u64;
                let mut open_item_ids = Vec::new();
                for (index, item) in ordered_items.iter().enumerate() {
                    item.validate()?;
                    if item.ordinal != index as u64 + 1
                        || !item_ids.insert(item.item.id.as_str())
                        || timeline_snapshot_item_identity(&self.session_id, item)?
                            != item.item_identity
                    {
                        return Err("timeline snapshot complete Item order is invalid");
                    }
                    validate_timeline_window(&item.latest_event, &self.watermark)?;
                    canonical_bytes = canonical_bytes
                        .checked_add(timeline_snapshot_item_canonical_bytes(
                            &self.session_id,
                            item,
                        )?)
                        .ok_or("timeline snapshot complete Item byte total overflowed")?;
                    if matches!(item.item.state.as_str(), "started" | "delta") {
                        let active_turn = self
                            .active_turn
                            .as_ref()
                            .ok_or("timeline snapshot open Item requires an active Turn")?;
                        if item.turn_id != active_turn.turn_id {
                            return Err("timeline snapshot open Item belongs to another Turn");
                        }
                        open_item_ids.push(item.item.id.clone());
                    }
                    identities.push(item.item_identity.clone());
                }
                if canonical_bytes != self.total_canonical_bytes
                    || self
                        .active_turn
                        .as_ref()
                        .map_or(!open_item_ids.is_empty(), |turn| {
                            turn.open_item_ids != open_item_ids
                        })
                {
                    return Err("timeline snapshot complete visible state totals are invalid");
                }
                self.validate_complete_identity(&identities)
            }
        }

        impl TryFrom<TimelineSessionSnapshotPageWire> for TimelineSessionSnapshotPage {
            type Error = &'static str;

            fn try_from(value: TimelineSessionSnapshotPageWire) -> Result<Self, Self::Error> {
                let page = Self {
                    schema_version: value.schema_version,
                    session_id: value.session_id,
                    snapshot_identity: value.snapshot_identity,
                    floor: value.floor,
                    watermark: value.watermark,
                    active_turn: parse_nullable_snapshot_active_turn(value.active_turn)?,
                    total_items: value.total_items,
                    total_canonical_bytes: value.total_canonical_bytes,
                    after: parse_nullable_snapshot_cursor(value.after)?,
                    items: value.items,
                    next_after: parse_nullable_snapshot_cursor(value.next_after)?,
                    complete: value.complete,
                    page_identity: value.page_identity,
                };
                page.validate()?;
                Ok(page)
            }
        }

        impl<'de> Deserialize<'de> for TimelineSessionSnapshotPage {
            fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
            where
                D: serde::Deserializer<'de>,
            {
                TimelineSessionSnapshotPageWire::deserialize(deserializer)?
                    .try_into()
                    .map_err(serde::de::Error::custom)
            }
        }

        impl Serialize for TimelineSessionSnapshotPage {
            fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
            where
                S: serde::Serializer,
            {
                self.validate().map_err(serde::ser::Error::custom)?;
                self.as_ref().serialize(serializer)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::stable::v0_1::{
        timeline_event_id, timeline_snapshot_identity, timeline_snapshot_item_canonical_bytes,
        timeline_snapshot_item_identity, timeline_snapshot_page_identity, EventEnvelope,
        InitializeParams, ItemUpdate, TimelineAnchor, TimelineItem, TimelineRetentionGapData,
        TimelineSessionSnapshotPage, TimelineSnapshotActiveTurn, TimelineSnapshotCursor,
        TimelineSnapshotItem, TimelineSnapshotParams, TimelineSyncPage, TimelineSyncParams,
        TurnState,
    };
    use super::{
        MAX_INITIALIZE_CAPABILITIES, MAX_INITIALIZE_CAPABILITY_BYTES, MAX_SAFE_JSON_INTEGER,
        MAX_TIMELINE_CONTENT_CHARACTERS, MAX_TIMELINE_DATA_DEPTH, MAX_TIMELINE_SNAPSHOT_ITEMS,
        MAX_TIMELINE_SNAPSHOT_PAGE_BYTES, MAX_TIMELINE_SNAPSHOT_TOTAL_BYTES,
        MAX_TIMELINE_SNAPSHOT_TOTAL_ITEMS, MAX_TIMELINE_SYNC_EVENTS,
    };
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

    fn base_timeline_event(event: &str) -> Value {
        let mut value = json!({
            "schema_version": "timeline-event/0.1",
            "event_id": format!("event:sha256:{}", "a".repeat(64)),
            "sequence": 1,
            "timestamp_ms": 1_784_851_200_001_u64,
            "correlation_id": "turn-1",
            "session_id": "session-1",
            "turn_id": "turn-1",
            "turn_state": "running",
            "event": event,
            "item": null,
            "item_update": null
        });
        seal_timeline_event(&mut value);
        value
    }

    fn seal_timeline_event(value: &mut Value) {
        let item = (!value["item"].is_null())
            .then(|| serde_json::from_value::<TimelineItem>(value["item"].clone()).unwrap());
        let item_update = (!value["item_update"].is_null())
            .then(|| serde_json::from_value::<ItemUpdate>(value["item_update"].clone()).unwrap());
        let turn_state = serde_json::from_value::<TurnState>(value["turn_state"].clone()).unwrap();
        let event_id = timeline_event_id(
            value["schema_version"].as_str().unwrap(),
            value["sequence"].as_u64().unwrap(),
            value["timestamp_ms"].as_u64().unwrap(),
            value["correlation_id"].as_str().unwrap(),
            value["session_id"].as_str().unwrap(),
            value["turn_id"].as_str().unwrap(),
            turn_state,
            value["event"].as_str().unwrap(),
            &item,
            &item_update,
        )
        .unwrap();
        value["event_id"] = json!(event_id);
    }

    fn message_item(state: &str) -> Value {
        json!({
            "id": "item-1",
            "kind": "message",
            "role": "agent",
            "state": state,
            "content": "bounded snapshot",
            "data": {"source": "fixture"}
        })
    }

    #[test]
    fn timeline_item_data_uses_cross_language_canonical_json_bounds() {
        let item = |data| TimelineItem {
            id: "item-1".into(),
            kind: "message".into(),
            role: "agent".into(),
            state: "completed".into(),
            content: "bounded".into(),
            data: Some(data),
        };
        assert!(item(json!({
            "nested": {
                "array": [0, -1, MAX_SAFE_JSON_INTEGER, "quote\" slash\\ newline\n 界"]
            }
        }))
        .validate()
        .is_ok());
        assert!(item(json!({"unsafe": MAX_SAFE_JSON_INTEGER + 1}))
            .validate()
            .is_err());
        assert!(item(json!({"unsafe": -(MAX_SAFE_JSON_INTEGER as i64) - 1}))
            .validate()
            .is_err());
        assert!(item(json!({"unsafe": 1.5})).validate().is_err());
        assert!(item(json!({"非ASCII": true})).validate().is_err());
        assert!(item(json!({"": true})).validate().is_err());

        let nested_properties = (0..129)
            .map(|index| (format!("key-{index}"), json!(true)))
            .collect::<serde_json::Map<_, _>>();
        assert!(item(json!({"nested": nested_properties}))
            .validate()
            .is_err());

        let mut nested = json!(true);
        for _ in 0..MAX_TIMELINE_DATA_DEPTH {
            nested = json!({"next": nested});
        }
        assert!(item(json!({"nested": nested})).validate().is_err());
    }

    #[test]
    fn timeline_wire_accepts_mathematical_integers_and_serializes_canonical_decimals() {
        let mut canonical = base_timeline_event("item.completed");
        canonical["sequence"] = json!(MAX_SAFE_JSON_INTEGER);
        canonical["timestamp_ms"] = json!(MAX_SAFE_JSON_INTEGER);
        canonical["item"] = message_item("completed");
        canonical["item"]["data"] = json!({
            "maximum": MAX_SAFE_JSON_INTEGER,
            "negative_maximum": -(MAX_SAFE_JSON_INTEGER as i64),
            "negative_zero": 0,
            "one": 1,
            "thousand": 1000
        });
        canonical["item_update"] = json!({
            "revision": MAX_SAFE_JSON_INTEGER,
            "content_mode": "snapshot-replacement"
        });
        seal_timeline_event(&mut canonical);

        let wire = serde_json::to_string(&canonical)
            .unwrap()
            .replace(
                "\"sequence\":9007199254740991",
                "\"sequence\":9007199254740991.0",
            )
            .replace(
                "\"timestamp_ms\":9007199254740991",
                "\"timestamp_ms\":9007199254740991e0",
            )
            .replace(
                "\"revision\":9007199254740991",
                "\"revision\":9007199254740991.000",
            )
            .replace(
                "\"maximum\":9007199254740991",
                "\"maximum\":9007199254740991.0",
            )
            .replace(
                "\"negative_maximum\":-9007199254740991",
                "\"negative_maximum\":-9007199254740991.0",
            )
            .replace("\"negative_zero\":0", "\"negative_zero\":-0.0")
            .replace("\"one\":1", "\"one\":1.0")
            .replace("\"thousand\":1000", "\"thousand\":1e3");
        let parsed: EventEnvelope = serde_json::from_str(&wire).unwrap();
        assert_eq!(serde_json::to_value(parsed).unwrap(), canonical);

        let constructed = TimelineItem {
            id: "item-programmatic".into(),
            kind: "message".into(),
            role: "agent".into(),
            state: "completed".into(),
            content: String::new(),
            data: Some(json!({"one": 1.0, "negative_zero": -0.0})),
        };
        assert_eq!(
            serde_json::to_value(constructed).unwrap()["data"],
            json!({"one": 1, "negative_zero": 0})
        );
    }

    #[test]
    fn timeline_wire_rejects_fractional_and_out_of_range_numbers() {
        for sequence in ["1.5", "9007199254740992.0", "-0.0"] {
            let canonical = serde_json::to_string(&base_timeline_event("turn.started")).unwrap();
            let wire = canonical.replace("\"sequence\":1", &format!("\"sequence\":{sequence}"));
            assert!(serde_json::from_str::<EventEnvelope>(&wire).is_err());
        }

        for data in ["1.5", "9007199254740992.0", "-9007199254740992.0"] {
            let wire = format!(
                r#"{{"id":"item","kind":"message","role":"agent","state":"completed","content":"","data":{{"value":{data}}}}}"#
            );
            assert!(serde_json::from_str::<TimelineItem>(&wire).is_err());
        }
    }

    #[test]
    fn timeline_event_round_trips_known_and_unknown_events() {
        let mut delta = base_timeline_event("item.delta");
        delta["item"] = message_item("delta");
        delta["item_update"] = json!({"revision": 2, "content_mode": "snapshot-replacement"});
        seal_timeline_event(&mut delta);
        let parsed: EventEnvelope = serde_json::from_value(delta.clone()).unwrap();
        assert_eq!(parsed.turn_state, TurnState::Running);
        assert_eq!(parsed.item.as_ref().unwrap().id, "item-1");
        assert_eq!(serde_json::to_value(parsed).unwrap(), delta);

        let unknown = base_timeline_event("future.adapter-state");
        let parsed: EventEnvelope = serde_json::from_value(unknown.clone()).unwrap();
        assert_eq!(parsed.event, "future.adapter-state");
        assert!(parsed.item.is_none());
        assert_eq!(serde_json::to_value(parsed).unwrap(), unknown);
    }

    #[test]
    fn timeline_event_requires_exact_fields_and_matching_item_update() {
        for missing in ["item", "item_update"] {
            let mut value = base_timeline_event("turn.started");
            value.as_object_mut().unwrap().remove(missing);
            assert!(serde_json::from_value::<EventEnvelope>(value).is_err());
        }

        let mut extra = base_timeline_event("turn.started");
        extra["legacy"] = json!(true);
        assert!(serde_json::from_value::<EventEnvelope>(extra).is_err());

        let mut item_without_update = base_timeline_event("item.completed");
        item_without_update["item"] = message_item("completed");
        assert!(serde_json::from_value::<EventEnvelope>(item_without_update).is_err());

        let mut update_without_item = base_timeline_event("turn.started");
        update_without_item["item_update"] =
            json!({"revision": 1, "content_mode": "snapshot-replacement"});
        assert!(serde_json::from_value::<EventEnvelope>(update_without_item).is_err());

        let mut extra_item = base_timeline_event("item.completed");
        extra_item["item"] = message_item("completed");
        extra_item["item"]["legacy"] = json!(true);
        extra_item["item_update"] = json!({"revision": 1, "content_mode": "snapshot-replacement"});
        assert!(serde_json::from_value::<EventEnvelope>(extra_item).is_err());

        let mut extra_update = base_timeline_event("item.completed");
        extra_update["item"] = message_item("completed");
        extra_update["item_update"] = json!({
            "revision": 1,
            "content_mode": "snapshot-replacement",
            "append": true
        });
        assert!(serde_json::from_value::<EventEnvelope>(extra_update).is_err());

        let mut null_data = base_timeline_event("item.completed");
        null_data["item"] = message_item("completed");
        null_data["item"]["data"] = Value::Null;
        null_data["item_update"] = json!({"revision": 1, "content_mode": "snapshot-replacement"});
        assert!(serde_json::from_value::<EventEnvelope>(null_data).is_err());
    }

    #[test]
    fn timeline_event_rejects_invalid_bounds_and_identity_drift() {
        for field in ["sequence", "timestamp_ms"] {
            let mut zero = base_timeline_event("turn.started");
            zero[field] = json!(0);
            assert!(serde_json::from_value::<EventEnvelope>(zero).is_err());

            let mut above_safe = base_timeline_event("turn.started");
            above_safe[field] = json!(MAX_SAFE_JSON_INTEGER + 1);
            assert!(serde_json::from_value::<EventEnvelope>(above_safe).is_err());
        }

        let mut wrong_correlation = base_timeline_event("turn.started");
        wrong_correlation["correlation_id"] = json!("turn-2");
        assert!(serde_json::from_value::<EventEnvelope>(wrong_correlation).is_err());

        let mut long_session = base_timeline_event("turn.started");
        long_session["session_id"] = json!("s".repeat(129));
        assert!(serde_json::from_value::<EventEnvelope>(long_session).is_err());

        let mut invalid_event_id = base_timeline_event("turn.started");
        invalid_event_id["event_id"] = json!(format!("event:sha256:{}", "A".repeat(64)));
        assert!(serde_json::from_value::<EventEnvelope>(invalid_event_id).is_err());

        let mut content_tamper = base_timeline_event("turn.started");
        content_tamper["timestamp_ms"] = json!(1_784_851_200_002_u64);
        let error = serde_json::from_value::<EventEnvelope>(content_tamper)
            .unwrap_err()
            .to_string();
        assert!(error.contains("identity does not match its content"));

        let mut long_content = base_timeline_event("item.completed");
        long_content["item"] = message_item("completed");
        long_content["item"]["content"] = json!("x".repeat(MAX_TIMELINE_CONTENT_CHARACTERS + 1));
        long_content["item_update"] =
            json!({"revision": 1, "content_mode": "snapshot-replacement"});
        assert!(serde_json::from_value::<EventEnvelope>(long_content).is_err());

        let mut zero_revision = base_timeline_event("item.completed");
        zero_revision["item"] = message_item("completed");
        zero_revision["item_update"] =
            json!({"revision": 0, "content_mode": "snapshot-replacement"});
        assert!(serde_json::from_value::<EventEnvelope>(zero_revision).is_err());
    }

    #[test]
    fn timeline_event_accepts_exact_safe_and_content_boundaries() {
        let mut boundary = base_timeline_event("item.completed");
        boundary["sequence"] = json!(MAX_SAFE_JSON_INTEGER);
        boundary["timestamp_ms"] = json!(MAX_SAFE_JSON_INTEGER);
        boundary["session_id"] = json!("s".repeat(128));
        boundary["item"] = message_item("completed");
        boundary["item"]["id"] = json!("i".repeat(128));
        boundary["item"]["kind"] = json!("k".repeat(64));
        boundary["item"]["content"] = json!("界".repeat(MAX_TIMELINE_CONTENT_CHARACTERS));
        boundary["item_update"] = json!({
            "revision": MAX_SAFE_JSON_INTEGER,
            "content_mode": "snapshot-replacement"
        });
        seal_timeline_event(&mut boundary);
        let event: EventEnvelope = serde_json::from_value(boundary.clone()).unwrap();
        assert_eq!(serde_json::to_value(event).unwrap(), boundary);
    }

    #[test]
    fn invalid_constructed_timeline_event_cannot_be_serialized() {
        let mut event: EventEnvelope =
            serde_json::from_value(base_timeline_event("turn.started")).unwrap();
        event.correlation_id = "different-turn".into();
        assert!(serde_json::to_value(event).is_err());
    }

    #[test]
    fn timeline_event_enforces_terminal_and_unknown_event_shapes() {
        let mut failed = base_timeline_event("turn.failed");
        failed["turn_state"] = json!("failed");
        failed["item"] = json!({
            "id": "error-1",
            "kind": "error",
            "role": "system",
            "state": "completed",
            "content": "Turn failed"
        });
        failed["item_update"] = json!({"revision": 1, "content_mode": "snapshot-replacement"});
        seal_timeline_event(&mut failed);
        assert!(serde_json::from_value::<EventEnvelope>(failed).is_ok());

        let mut failed_without_item = base_timeline_event("turn.failed");
        failed_without_item["turn_state"] = json!("failed");
        assert!(serde_json::from_value::<EventEnvelope>(failed_without_item).is_err());

        let mut completed_running = base_timeline_event("turn.completed");
        completed_running["turn_state"] = json!("running");
        assert!(serde_json::from_value::<EventEnvelope>(completed_running).is_err());

        let mut unknown_with_item = base_timeline_event("future.adapter-state");
        unknown_with_item["item"] = message_item("completed");
        unknown_with_item["item_update"] =
            json!({"revision": 1, "content_mode": "snapshot-replacement"});
        assert!(serde_json::from_value::<EventEnvelope>(unknown_with_item).is_err());

        let persistence_failed = base_timeline_event("turn.persistence-failed");
        assert!(serde_json::from_value::<EventEnvelope>(persistence_failed).is_err());
    }

    fn sync_event(sequence: u64) -> EventEnvelope {
        let mut value = base_timeline_event("turn.started");
        value["sequence"] = json!(sequence);
        value["timestamp_ms"] = json!(1_784_851_200_000_u64 + sequence);
        seal_timeline_event(&mut value);
        serde_json::from_value(value).unwrap()
    }

    fn anchor_for(event: &EventEnvelope) -> Value {
        json!({"sequence": event.sequence, "event_id": event.event_id})
    }

    #[test]
    fn timeline_sync_request_round_trips_initial_and_fixed_watermark_forms() {
        let initial = json!({
            "session_id": "session-1",
            "after": {"sequence": 0, "event_id": null},
            "watermark": null,
            "limit": 200
        });
        let params: TimelineSyncParams = serde_json::from_value(initial.clone()).unwrap();
        assert_eq!(params.after, TimelineAnchor::initial());
        assert!(params.watermark.is_none());
        assert_eq!(serde_json::to_value(params).unwrap(), initial);

        let event = sync_event(1);
        let fixed = json!({
            "session_id": "session-1",
            "after": anchor_for(&event),
            "watermark": anchor_for(&event),
            "limit": 1.0
        });
        let params: TimelineSyncParams = serde_json::from_value(fixed).unwrap();
        assert_eq!(params.limit, 1);
        assert_eq!(params.after, params.watermark.unwrap());
    }

    #[test]
    fn timeline_anchor_and_sync_request_reject_noncanonical_pairs_bounds_and_fields() {
        for invalid in [
            json!({"sequence": 0, "event_id": format!("event:sha256:{}", "a".repeat(64))}),
            json!({"sequence": 1, "event_id": null}),
            json!({"sequence": 1, "event_id": format!("event:sha256:{}", "A".repeat(64))}),
            json!({"sequence": MAX_SAFE_JSON_INTEGER + 1, "event_id": null}),
            json!({"sequence": -1, "event_id": null}),
            json!({"sequence": 0, "event_id": null, "legacy": true}),
        ] {
            assert!(serde_json::from_value::<TimelineAnchor>(invalid).is_err());
        }

        for limit in [json!(0), json!(201), json!(1.5)] {
            let invalid = json!({
                "session_id": "session-1",
                "after": {"sequence": 0, "event_id": null},
                "watermark": null,
                "limit": limit
            });
            assert!(serde_json::from_value::<TimelineSyncParams>(invalid).is_err());
        }

        let event = sync_event(1);
        let mut missing_watermark = json!({
            "session_id": "session-1",
            "after": {"sequence": 0, "event_id": null},
            "watermark": null,
            "limit": 1
        });
        missing_watermark
            .as_object_mut()
            .unwrap()
            .remove("watermark");
        assert!(serde_json::from_value::<TimelineSyncParams>(missing_watermark).is_err());

        let after_watermark = json!({
            "session_id": "session-1",
            "after": anchor_for(&event),
            "watermark": {"sequence": 0, "event_id": null},
            "limit": 1
        });
        assert!(serde_json::from_value::<TimelineSyncParams>(after_watermark).is_err());

        let different_same_sequence = json!({
            "session_id": "session-1",
            "after": anchor_for(&event),
            "watermark": {
                "sequence": 1,
                "event_id": format!("event:sha256:{}", "f".repeat(64))
            },
            "limit": 1
        });
        assert!(serde_json::from_value::<TimelineSyncParams>(different_same_sequence).is_err());

        let invalid = TimelineSyncParams {
            session_id: "session-1".into(),
            after: TimelineAnchor::initial(),
            watermark: None,
            limit: 201,
        };
        assert!(serde_json::to_value(invalid).is_err());
    }

    #[test]
    fn timeline_retention_gap_data_is_closed_bounded_and_requires_snapshot_recovery() {
        let value = json!({
            "schema_version": "timeline-retention-gap/0.1",
            "reason": "requested-anchor-not-retained",
            "session_id": "session-1",
            "requested_after": {"sequence": 0, "event_id": null},
            "requested_watermark": null,
            "retained_floor": {
                "sequence": 2,
                "event_id": format!("event:sha256:{}", "a".repeat(64))
            },
            "head": {
                "sequence": 3,
                "event_id": format!("event:sha256:{}", "b".repeat(64))
            },
            "snapshot_required": true,
            "snapshot_available": false,
            "snapshot_capability": "timeline.snapshot.current",
            "snapshot_method": "timeline/snapshot",
            "event_history_complete": false,
            "replay_from_floor_allowed": false
        });
        let data: TimelineRetentionGapData = serde_json::from_value(value.clone()).unwrap();
        let request: TimelineSyncParams = serde_json::from_value(json!({
            "session_id": "session-1",
            "after": {"sequence": 0, "event_id": null},
            "watermark": null,
            "limit": 200
        }))
        .unwrap();
        data.validate_for_request(&request).unwrap();
        assert_eq!(serde_json::to_value(&data).unwrap(), value);

        for (key, invalid) in [
            ("snapshot_required", json!(false)),
            ("event_history_complete", json!(true)),
            ("replay_from_floor_allowed", json!(true)),
            ("snapshot_method", json!("session/read")),
        ] {
            let mut candidate = value.clone();
            candidate[key] = invalid;
            assert!(serde_json::from_value::<TimelineRetentionGapData>(candidate).is_err());
        }

        let mut available = value.clone();
        available["snapshot_available"] = json!(true);
        let available: TimelineRetentionGapData = serde_json::from_value(available).unwrap();
        available.validate_for_request(&request).unwrap();

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
            let mut candidate = value.clone();
            candidate[key] = mismatch;
            let candidate: TimelineRetentionGapData = serde_json::from_value(candidate).unwrap();
            assert!(candidate.validate_for_request(&request).is_err());
        }

        let mut not_a_gap = value.clone();
        not_a_gap["requested_after"] = not_a_gap["retained_floor"].clone();
        assert!(serde_json::from_value::<TimelineRetentionGapData>(not_a_gap).is_err());

        let mut extra = value;
        extra["checkpoint_identity"] = json!("must-not-be-public");
        assert!(serde_json::from_value::<TimelineRetentionGapData>(extra).is_err());
    }

    #[test]
    fn timeline_sync_pages_bind_fixed_watermark_contiguous_events_and_completion() {
        let first = sync_event(1);
        let second = sync_event(2);
        let watermark = TimelineAnchor {
            sequence: second.sequence,
            event_id: Some(second.event_id.clone()),
        };
        let request: TimelineSyncParams = serde_json::from_value(json!({
            "session_id": "session-1",
            "after": {"sequence": 0, "event_id": null},
            "watermark": anchor_for(&second),
            "limit": 1
        }))
        .unwrap();
        let first_page = TimelineSyncPage {
            schema_version: "timeline-sync-page/0.1".into(),
            session_id: "session-1".into(),
            after: TimelineAnchor::initial(),
            watermark: watermark.clone(),
            events: vec![first.clone()],
            next_after: Some(TimelineAnchor {
                sequence: first.sequence,
                event_id: Some(first.event_id.clone()),
            }),
            complete: false,
        };
        first_page.validate_for_request(&request).unwrap();
        let encoded = serde_json::to_value(&first_page).unwrap();
        assert_eq!(encoded["schema_version"], "timeline-sync-page/0.1");
        assert_eq!(encoded["watermark"], anchor_for(&second));
        assert_eq!(
            serde_json::from_value::<TimelineSyncPage>(encoded).unwrap(),
            first_page
        );

        let next_request: TimelineSyncParams = serde_json::from_value(json!({
            "session_id": "session-1",
            "after": anchor_for(&first),
            "watermark": anchor_for(&second),
            "limit": 200
        }))
        .unwrap();
        let final_page: TimelineSyncPage = serde_json::from_value(json!({
            "schema_version": "timeline-sync-page/0.1",
            "session_id": "session-1",
            "after": anchor_for(&first),
            "watermark": anchor_for(&second),
            "events": [serde_json::to_value(&second).unwrap()],
            "next_after": null,
            "complete": true
        }))
        .unwrap();
        final_page.validate_for_request(&next_request).unwrap();

        let empty: TimelineSyncPage = serde_json::from_value(json!({
            "schema_version": "timeline-sync-page/0.1",
            "session_id": "session-empty",
            "after": {"sequence": 0, "event_id": null},
            "watermark": {"sequence": 0, "event_id": null},
            "events": [],
            "next_after": null,
            "complete": true
        }))
        .unwrap();
        assert!(empty.events.is_empty());
    }

    #[test]
    fn timeline_sync_pages_reject_gap_drift_forged_continuation_and_request_mismatch() {
        let first = sync_event(1);
        let second = sync_event(2);
        let valid = json!({
            "schema_version": "timeline-sync-page/0.1",
            "session_id": "session-1",
            "after": {"sequence": 0, "event_id": null},
            "watermark": anchor_for(&second),
            "events": [serde_json::to_value(&first).unwrap()],
            "next_after": anchor_for(&first),
            "complete": false
        });

        let mut extra = valid.clone();
        extra["legacy"] = json!(true);
        assert!(serde_json::from_value::<TimelineSyncPage>(extra).is_err());

        let mut no_events = valid.clone();
        no_events["events"] = json!([]);
        no_events["next_after"] = Value::Null;
        assert!(serde_json::from_value::<TimelineSyncPage>(no_events).is_err());

        let mut forged_next = valid.clone();
        forged_next["next_after"] = anchor_for(&second);
        assert!(serde_json::from_value::<TimelineSyncPage>(forged_next).is_err());

        let mut premature_complete = valid.clone();
        premature_complete["complete"] = json!(true);
        premature_complete["next_after"] = Value::Null;
        assert!(serde_json::from_value::<TimelineSyncPage>(premature_complete).is_err());

        let mut gap = valid.clone();
        gap["events"] = json!([serde_json::to_value(&second).unwrap()]);
        gap["next_after"] = anchor_for(&second);
        assert!(serde_json::from_value::<TimelineSyncPage>(gap).is_err());

        let page: TimelineSyncPage = serde_json::from_value(valid).unwrap();
        let wrong_session: TimelineSyncParams = serde_json::from_value(json!({
            "session_id": "session-2",
            "after": {"sequence": 0, "event_id": null},
            "watermark": anchor_for(&second),
            "limit": 1
        }))
        .unwrap();
        assert!(page.validate_for_request(&wrong_session).is_err());

        let wrong_watermark: TimelineSyncParams = serde_json::from_value(json!({
            "session_id": "session-1",
            "after": {"sequence": 0, "event_id": null},
            "watermark": anchor_for(&first),
            "limit": 1
        }))
        .unwrap();
        assert!(page.validate_for_request(&wrong_watermark).is_err());
    }

    #[test]
    fn timeline_sync_page_deserializer_caps_events_before_projection() {
        let first = sync_event(1);
        let events = vec![serde_json::to_value(first).unwrap(); MAX_TIMELINE_SYNC_EVENTS + 1];
        let page = json!({
            "schema_version": "timeline-sync-page/0.1",
            "session_id": "session-1",
            "after": {"sequence": 0, "event_id": null},
            "watermark": {"sequence": 0, "event_id": null},
            "events": events,
            "next_after": null,
            "complete": true
        });
        assert!(serde_json::from_value::<TimelineSyncPage>(page).is_err());
    }

    fn snapshot_anchor(sequence: u64, byte: char) -> TimelineAnchor {
        TimelineAnchor {
            sequence,
            event_id: (sequence > 0)
                .then(|| format!("event:sha256:{}", byte.to_string().repeat(64))),
        }
    }

    fn materialized_snapshot_item(
        ordinal: u64,
        id: &str,
        event_sequence: u64,
    ) -> TimelineSnapshotItem {
        let anchor = snapshot_anchor(event_sequence, if event_sequence == 3 { 'b' } else { '2' });
        let mut item = TimelineSnapshotItem {
            ordinal,
            item_identity: format!("timeline-session-snapshot-item:sha256:{}", "0".repeat(64)),
            turn_id: "turn-1".into(),
            correlation_id: "turn-1".into(),
            turn_state: TurnState::Running,
            first_event: anchor.clone(),
            latest_event: anchor,
            item: TimelineItem {
                id: id.into(),
                kind: "message".into(),
                role: if ordinal.is_multiple_of(2) {
                    "agent"
                } else {
                    "user"
                }
                .into(),
                state: "completed".into(),
                content: format!("snapshot item {ordinal}"),
                data: None,
            },
            item_update: ItemUpdate {
                revision: 1,
                content_mode: "snapshot-replacement".into(),
            },
        };
        item.item_identity = timeline_snapshot_item_identity("session-1", &item).unwrap();
        item
    }

    fn snapshot_active_turn() -> TimelineSnapshotActiveTurn {
        TimelineSnapshotActiveTurn {
            turn_id: "turn-1".into(),
            correlation_id: "turn-1".into(),
            state: TurnState::Running,
            started_event: snapshot_anchor(1, '1'),
            latest_event: snapshot_anchor(3, 'b'),
            open_item_ids: Vec::new(),
        }
    }

    fn snapshot_pages() -> (
        TimelineSnapshotParams,
        TimelineSessionSnapshotPage,
        TimelineSnapshotParams,
        TimelineSessionSnapshotPage,
    ) {
        let first_item = materialized_snapshot_item(1, "item-1", 2);
        let second_item = materialized_snapshot_item(2, "item-2", 3);
        let floor = TimelineAnchor::initial();
        let watermark = snapshot_anchor(3, 'b');
        let active_turn = snapshot_active_turn();
        let total_canonical_bytes = [&first_item, &second_item]
            .into_iter()
            .map(|item| timeline_snapshot_item_canonical_bytes("session-1", item).unwrap())
            .sum();
        let ordered_item_identities = vec![
            first_item.item_identity.clone(),
            second_item.item_identity.clone(),
        ];
        let snapshot_identity = timeline_snapshot_identity(
            "session-1",
            &floor,
            &watermark,
            Some(&active_turn),
            2,
            total_canonical_bytes,
            &ordered_item_identities,
        )
        .unwrap();
        let cursor = TimelineSnapshotCursor {
            ordinal: 1,
            item_id: first_item.item.id.clone(),
            item_identity: first_item.item_identity.clone(),
        };
        let initial = TimelineSnapshotParams {
            session_id: "session-1".into(),
            snapshot_identity: None,
            watermark: None,
            after: None,
            limit: 1,
        };
        let mut first = TimelineSessionSnapshotPage {
            schema_version: "timeline-session-snapshot-page/0.1".into(),
            session_id: "session-1".into(),
            snapshot_identity: snapshot_identity.clone(),
            floor: floor.clone(),
            watermark: watermark.clone(),
            active_turn: Some(active_turn.clone()),
            total_items: 2,
            total_canonical_bytes,
            after: None,
            items: vec![first_item],
            next_after: Some(cursor.clone()),
            complete: false,
            page_identity: format!("timeline-session-snapshot-page:sha256:{}", "0".repeat(64)),
        };
        first.page_identity = timeline_snapshot_page_identity(&first).unwrap();
        let continuation = TimelineSnapshotParams {
            session_id: "session-1".into(),
            snapshot_identity: Some(snapshot_identity.clone()),
            watermark: Some(watermark.clone()),
            after: Some(cursor.clone()),
            limit: 1,
        };
        let mut final_page = TimelineSessionSnapshotPage {
            schema_version: "timeline-session-snapshot-page/0.1".into(),
            session_id: "session-1".into(),
            snapshot_identity,
            floor,
            watermark,
            active_turn: Some(active_turn),
            total_items: 2,
            total_canonical_bytes,
            after: Some(cursor),
            items: vec![second_item],
            next_after: None,
            complete: true,
            page_identity: format!("timeline-session-snapshot-page:sha256:{}", "0".repeat(64)),
        };
        final_page.page_identity = timeline_snapshot_page_identity(&final_page).unwrap();
        (initial, first, continuation, final_page)
    }

    #[test]
    fn timeline_snapshot_contract_binds_items_pages_and_complete_identity() {
        let (initial, first, continuation, final_page) = snapshot_pages();
        first.validate_for_request(&initial).unwrap();
        first
            .validate_continuation(&continuation, &final_page)
            .unwrap();
        let identities = first
            .items
            .iter()
            .chain(final_page.items.iter())
            .map(|item| item.item_identity.clone())
            .collect::<Vec<_>>();
        final_page.validate_complete_identity(&identities).unwrap();
        let ordered_items = first
            .items
            .iter()
            .chain(final_page.items.iter())
            .cloned()
            .collect::<Vec<_>>();
        final_page.validate_complete_items(&ordered_items).unwrap();
        for value in [
            serde_json::to_value(&initial).unwrap(),
            serde_json::to_value(&continuation).unwrap(),
        ] {
            let decoded: TimelineSnapshotParams = serde_json::from_value(value.clone()).unwrap();
            assert_eq!(serde_json::to_value(decoded).unwrap(), value);
        }
        for value in [
            serde_json::to_value(&first).unwrap(),
            serde_json::to_value(&final_page).unwrap(),
        ] {
            let decoded: TimelineSessionSnapshotPage =
                serde_json::from_value(value.clone()).unwrap();
            assert_eq!(serde_json::to_value(decoded).unwrap(), value);
        }
    }

    #[test]
    fn timeline_snapshot_request_requires_atomic_continuation_binding() {
        let (_, first, continuation, _) = snapshot_pages();
        let mut missing_cursor = continuation.clone();
        missing_cursor.after = None;
        assert!(missing_cursor.validate().is_err());
        assert!(serde_json::to_value(missing_cursor).is_err());

        let mut empty_watermark = continuation.clone();
        empty_watermark.watermark = Some(TimelineAnchor::initial());
        assert!(empty_watermark.validate().is_err());

        let mut invalid_limit = continuation;
        invalid_limit.limit = MAX_TIMELINE_SNAPSHOT_ITEMS as u64 + 1;
        assert!(invalid_limit.validate().is_err());

        let mut cursor = serde_json::to_value(first.next_after.unwrap()).unwrap();
        cursor["legacy"] = json!(true);
        assert!(serde_json::from_value::<TimelineSnapshotCursor>(cursor).is_err());
        let invalid_cursor = TimelineSnapshotCursor {
            ordinal: 1,
            item_id: "contains space".into(),
            item_identity: format!("timeline-session-snapshot-item:sha256:{}", "a".repeat(64)),
        };
        assert!(serde_json::to_value(invalid_cursor).is_err());
    }

    #[test]
    fn timeline_snapshot_contract_rejects_drift_and_unknown_state() {
        let (_, first, continuation, final_page) = snapshot_pages();
        for mutate in ["item_identity", "revision", "latest_event", "page_identity"] {
            let mut value = serde_json::to_value(&first).unwrap();
            match mutate {
                "item_identity" => {
                    value["items"][0]["item_identity"] = json!(format!(
                        "timeline-session-snapshot-item:sha256:{}",
                        "f".repeat(64)
                    ))
                }
                "revision" => value["items"][0]["item_update"]["revision"] = json!(2),
                "latest_event" => {
                    value["items"][0]["latest_event"] =
                        serde_json::to_value(snapshot_anchor(3, 'b')).unwrap()
                }
                "page_identity" => {
                    value["page_identity"] = json!(format!(
                        "timeline-session-snapshot-page:sha256:{}",
                        "f".repeat(64)
                    ))
                }
                _ => unreachable!(),
            }
            assert!(serde_json::from_value::<TimelineSessionSnapshotPage>(value).is_err());
        }

        let mut active = serde_json::to_value(&first).unwrap();
        active["active_turn"]["open_item_ids"] = json!(["item-1", "item-1"]);
        assert!(serde_json::from_value::<TimelineSessionSnapshotPage>(active).is_err());

        let mut extra = serde_json::to_value(&first).unwrap();
        extra["legacy"] = json!(true);
        assert!(serde_json::from_value::<TimelineSessionSnapshotPage>(extra).is_err());

        let mut wrong_request = continuation.clone();
        wrong_request.after.as_mut().unwrap().item_id = "item-other".into();
        assert!(first
            .validate_continuation(&wrong_request, &final_page)
            .is_err());

        let mut changed_page = final_page;
        changed_page.active_turn.as_mut().unwrap().latest_event = snapshot_anchor(2, '3');
        changed_page.page_identity = timeline_snapshot_page_identity(&changed_page).unwrap();
        assert!(first
            .validate_continuation(&continuation, &changed_page)
            .is_err());

        let (_, _, continuation, mut changed_totals) = snapshot_pages();
        changed_totals.total_canonical_bytes += 1;
        changed_totals.page_identity = timeline_snapshot_page_identity(&changed_totals).unwrap();
        assert!(first
            .validate_continuation(&continuation, &changed_totals)
            .is_err());
    }

    #[test]
    fn timeline_snapshot_contract_enforces_complete_and_frame_bounds() {
        assert_eq!(MAX_TIMELINE_SNAPSHOT_TOTAL_ITEMS, 10_000);
        assert_eq!(MAX_TIMELINE_SNAPSHOT_TOTAL_BYTES, 64 * 1024 * 1024);
        const { assert!(MAX_TIMELINE_SNAPSHOT_PAGE_BYTES < 4 * 1024 * 1024) };

        let mut items = (1..=64)
            .map(|ordinal| {
                let mut item = materialized_snapshot_item(ordinal, &format!("item-{ordinal}"), 3);
                item.item.content = "x".repeat(MAX_TIMELINE_CONTENT_CHARACTERS);
                item.item_identity = timeline_snapshot_item_identity("session-1", &item).unwrap();
                item
            })
            .collect::<Vec<_>>();
        let total_canonical_bytes = items
            .iter()
            .map(|item| timeline_snapshot_item_canonical_bytes("session-1", item).unwrap())
            .sum();
        let identities = items
            .iter()
            .map(|item| item.item_identity.clone())
            .collect::<Vec<_>>();
        let floor = TimelineAnchor::initial();
        let watermark = snapshot_anchor(3, 'b');
        let snapshot_identity = timeline_snapshot_identity(
            "session-1",
            &floor,
            &watermark,
            None,
            items.len() as u64,
            total_canonical_bytes,
            &identities,
        )
        .unwrap();
        let mut page = TimelineSessionSnapshotPage {
            schema_version: "timeline-session-snapshot-page/0.1".into(),
            session_id: "session-1".into(),
            snapshot_identity,
            floor,
            watermark,
            active_turn: None,
            total_items: items.len() as u64,
            total_canonical_bytes,
            after: None,
            items: std::mem::take(&mut items),
            next_after: None,
            complete: true,
            page_identity: format!("timeline-session-snapshot-page:sha256:{}", "0".repeat(64)),
        };
        page.page_identity = timeline_snapshot_page_identity(&page).unwrap();
        assert!(page.validate().is_err());
        assert!(serde_json::to_value(page).is_err());
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
