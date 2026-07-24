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
                if self.event_id.len() != 77
                    || !self.event_id.starts_with("event:sha256:")
                    || !self.event_id[13..]
                        .bytes()
                        .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
                {
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
    }
}

#[cfg(test)]
mod tests {
    use super::stable::v0_1::{
        timeline_event_id, EventEnvelope, InitializeParams, ItemUpdate, TimelineItem, TurnState,
    };
    use super::{
        MAX_INITIALIZE_CAPABILITIES, MAX_INITIALIZE_CAPABILITY_BYTES, MAX_SAFE_JSON_INTEGER,
        MAX_TIMELINE_CONTENT_CHARACTERS, MAX_TIMELINE_DATA_DEPTH,
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
