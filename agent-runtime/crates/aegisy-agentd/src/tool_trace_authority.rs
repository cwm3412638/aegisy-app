//! Reconstructs content-free Tool lifecycle authority from Codex command Items.
//!
//! This module may inspect the persisted command payload to bind authority, but
//! it never returns command text, paths, output, provider bodies, or process IDs.

use aegisy_aap::stable::v0_1::TimelineItem;
use serde_json::{Map, Value};
use sha2::{Digest, Sha256};

use crate::turn_trace::{
    AuthorityLabel, EvidenceRef, EvidenceSource, RedactionSummary, ToolProviderStatus, ToolSource,
    ToolState, ToolTimelineBinding, TracePayload,
};
use crate::workbench_store::StoredItem;
use crate::workspace_edit::ContentHash;

pub const COMMAND_TIMELINE_SCHEMA_VERSION: &str = "command-timeline/0.1";

const MAX_BINDING_ID_BYTES: usize = 128;
const MAX_COMMAND_BYTES: usize = 32 * 1024;
const MAX_CWD_BYTES: usize = 4 * 1024;
const MAX_PROCESS_ID_BYTES: usize = 512;
const MAX_COMMAND_ACTIONS: usize = 128;
const MAX_STRUCTURED_FIELD_BYTES: usize = 64 * 1024;

const COMMAND_DATA_KEYS: &[&str] = &[
    "command",
    "command_actions",
    "completed_at_ms",
    "cwd",
    "duration_ms",
    "environment",
    "exit_code",
    "input_identity",
    "output",
    "process_id",
    "risk",
    "schema_version",
    "session_id",
    "source",
    "started_at_ms",
    "status",
];
const COMMAND_PAYLOAD_KEYS: &[&str] = &["content", "data"];

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ToolTraceAuthorityError {
    pub code: &'static str,
    pub message: &'static str,
}

impl ToolTraceAuthorityError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ToolTraceObservation {
    pub at_ms: u64,
    pub payload: TracePayload,
}

/// Reconstructs the provider-observed start. Started command Items are emitted
/// live and are deliberately not accepted as persisted Timeline authority.
pub fn started_observation(
    session_id: &str,
    turn_id: &str,
    item: &TimelineItem,
) -> Result<ToolTraceObservation, ToolTraceAuthorityError> {
    validate_binding_id(session_id, "tool-authority-session-invalid")?;
    validate_binding_id(turn_id, "tool-authority-turn-invalid")?;
    validate_common_item_fields(&item.id, &item.kind, &item.role)?;
    if item.state != "started" {
        return Err(error(
            "tool-authority-started-state-invalid",
            "a started Tool observation requires a started Timeline Item",
        ));
    }
    let data = item.data.as_ref().ok_or_else(|| {
        error(
            "tool-authority-data-missing",
            "the command Timeline Item is missing structured data",
        )
    })?;
    let command = parse_command_data(data, session_id)?;
    if command.status != ToolProviderStatus::InProgress
        || command.completed_at_ms.is_some()
        || command.duration_ms.is_some()
        || command.exit_code.is_some()
    {
        return Err(error(
            "tool-authority-started-lifecycle-invalid",
            "a started command must be in progress and cannot carry terminal metadata",
        ));
    }

    build_observation(
        session_id,
        turn_id,
        &item.id,
        &command,
        ToolTimelineBinding::NotPersisted,
        None,
    )
}

/// Reconstructs terminal authority only from the exact successfully persisted
/// command Item. The complete payload hash is rechecked before any identities
/// are returned.
pub fn terminal_observation(
    session_id: &str,
    turn_id: &str,
    item: &StoredItem,
) -> Result<ToolTraceObservation, ToolTraceAuthorityError> {
    validate_binding_id(session_id, "tool-authority-session-invalid")?;
    validate_binding_id(turn_id, "tool-authority-turn-invalid")?;
    if item.session_id != session_id {
        return Err(error(
            "tool-authority-session-binding-mismatch",
            "the persisted command Item belongs to a different Session",
        ));
    }
    if item.turn_id.as_deref() != Some(turn_id) {
        return Err(error(
            "tool-authority-turn-binding-mismatch",
            "the persisted command Item belongs to a different Turn",
        ));
    }
    if item.sequence == 0 {
        return Err(error(
            "tool-authority-item-sequence-invalid",
            "the persisted command Item sequence must be positive",
        ));
    }
    validate_common_item_fields(&item.item_id, &item.item_kind, &item.role)?;
    if item.state != "completed" {
        return Err(error(
            "tool-authority-terminal-state-invalid",
            "terminal command authority requires a completed outer Timeline Item",
        ));
    }
    validate_payload_hash(&item.payload, &item.payload_hash)?;

    let payload = exact_object(
        &item.payload,
        COMMAND_PAYLOAD_KEYS,
        "tool-authority-payload-schema-invalid",
        "the persisted command Item payload shape is invalid",
    )?;
    if !payload.get("content").is_some_and(Value::is_string) {
        return Err(error(
            "tool-authority-content-schema-invalid",
            "the persisted command Item content field is invalid",
        ));
    }
    let data = payload.get("data").ok_or_else(|| {
        error(
            "tool-authority-data-missing",
            "the command Timeline Item is missing structured data",
        )
    })?;
    let command = parse_command_data(data, session_id)?;
    if command.status == ToolProviderStatus::InProgress {
        return Err(error(
            "tool-authority-terminal-status-invalid",
            "terminal command authority cannot retain an in-progress status",
        ));
    }
    let completed_at_ms = command.completed_at_ms.ok_or_else(|| {
        error(
            "tool-authority-completed-time-missing",
            "terminal command authority requires a completion timestamp",
        )
    })?;
    if completed_at_ms < command.started_at_ms || item.created_at_ms < completed_at_ms {
        return Err(error(
            "tool-authority-time-order-invalid",
            "command lifecycle and persistence timestamps are not monotonic",
        ));
    }
    let elapsed_ms = completed_at_ms - command.started_at_ms;
    if command
        .duration_ms
        .is_some_and(|duration| duration > elapsed_ms)
    {
        return Err(error(
            "tool-authority-duration-invalid",
            "command duration exceeds its provider lifecycle interval",
        ));
    }
    validate_terminal_semantics(command.status, command.duration_ms, command.exit_code)?;

    build_observation(
        session_id,
        turn_id,
        &item.item_id,
        &command,
        ToolTimelineBinding::Persisted {
            item_identity: persisted_item_identity(session_id, turn_id, &item.item_id),
            payload_identity: format!("sha256:{}", item.payload_hash.sha256),
        },
        Some(completed_at_ms),
    )
}

fn persisted_item_identity(session_id: &str, turn_id: &str, item_id: &str) -> String {
    IdentityBuilder::new("codex-command-persisted-item")
        .component(session_id.as_bytes())
        .component(turn_id.as_bytes())
        .component(item_id.as_bytes())
        .finish()
}

struct CommandData<'a> {
    environment: &'a Value,
    risk: &'a Value,
    output: &'a Value,
    status: ToolProviderStatus,
    source: ToolSource,
    started_at_ms: u64,
    completed_at_ms: Option<u64>,
    duration_ms: Option<u64>,
    exit_code: Option<i64>,
    input_identity: &'a str,
}

fn parse_command_data<'a>(
    value: &'a Value,
    session_id: &str,
) -> Result<CommandData<'a>, ToolTraceAuthorityError> {
    let data = exact_object(
        value,
        COMMAND_DATA_KEYS,
        "tool-authority-data-schema-invalid",
        "the command Timeline data shape is invalid",
    )?;
    if data.get("schema_version").and_then(Value::as_str) != Some(COMMAND_TIMELINE_SCHEMA_VERSION) {
        return Err(error(
            "tool-authority-schema-version-invalid",
            "the command Timeline schema version is missing or unsupported",
        ));
    }
    if data.get("session_id").and_then(Value::as_str) != Some(session_id) {
        return Err(error(
            "tool-authority-session-binding-mismatch",
            "the command Timeline data belongs to a different Session",
        ));
    }

    let command = bounded_string(
        data.get("command"),
        MAX_COMMAND_BYTES,
        "tool-authority-command-schema-invalid",
    )?;
    if command.is_empty() {
        return Err(error(
            "tool-authority-command-schema-invalid",
            "the command Timeline input is missing",
        ));
    }
    let cwd = bounded_string(
        data.get("cwd"),
        MAX_CWD_BYTES,
        "tool-authority-cwd-schema-invalid",
    )?;
    if cwd.is_empty() {
        return Err(error(
            "tool-authority-cwd-schema-invalid",
            "the command Timeline working directory is missing",
        ));
    }

    let command_actions = data.get("command_actions").ok_or_else(|| {
        error(
            "tool-authority-command-actions-schema-invalid",
            "the command action metadata is missing",
        )
    })?;
    if !command_actions
        .as_array()
        .is_some_and(|actions| actions.len() <= MAX_COMMAND_ACTIONS)
    {
        return Err(error(
            "tool-authority-command-actions-schema-invalid",
            "the command action metadata is invalid or exceeds its bound",
        ));
    }
    validate_structured_bound(
        command_actions,
        "tool-authority-command-actions-schema-invalid",
    )?;

    let environment = required_object_field(
        data,
        "environment",
        "tool-authority-environment-schema-invalid",
    )?;
    validate_structured_bound(environment, "tool-authority-environment-schema-invalid")?;
    let risk = required_object_field(data, "risk", "tool-authority-risk-schema-invalid")?;
    validate_structured_bound(risk, "tool-authority-risk-schema-invalid")?;
    let output = required_object_field(data, "output", "tool-authority-output-schema-invalid")?;
    validate_structured_bound(output, "tool-authority-output-schema-invalid")?;

    match data.get("process_id") {
        Some(Value::Null) => {}
        Some(Value::String(value)) if !value.is_empty() && value.len() <= MAX_PROCESS_ID_BYTES => {}
        _ => {
            return Err(error(
                "tool-authority-process-id-schema-invalid",
                "the provider process identity shape is invalid",
            ));
        }
    }

    let status = match data.get("status").and_then(Value::as_str) {
        Some("inProgress") => ToolProviderStatus::InProgress,
        Some("completed") => ToolProviderStatus::Completed,
        Some("failed") => ToolProviderStatus::Failed,
        Some("declined") => ToolProviderStatus::Declined,
        _ => {
            return Err(error(
                "tool-authority-provider-status-invalid",
                "the provider command status is missing or unsupported",
            ));
        }
    };
    let source = match data.get("source").and_then(Value::as_str) {
        Some("agent") => ToolSource::Agent,
        Some("userShell") => ToolSource::UserShell,
        Some("unifiedExecStartup") => ToolSource::UnifiedExecStartup,
        Some("unifiedExecInteraction") => ToolSource::UnifiedExecInteraction,
        _ => {
            return Err(error(
                "tool-authority-provider-source-invalid",
                "the provider command source is missing or unsupported",
            ));
        }
    };
    let started_at_ms = required_u64(
        data.get("started_at_ms"),
        "tool-authority-started-time-invalid",
    )?;
    let completed_at_ms = optional_u64(
        data.get("completed_at_ms"),
        "tool-authority-completed-time-invalid",
    )?;
    let duration_ms = optional_u64(data.get("duration_ms"), "tool-authority-duration-invalid")?;
    if duration_ms.is_some_and(|value| value > i64::MAX as u64) {
        return Err(error(
            "tool-authority-duration-invalid",
            "the command duration exceeds the provider schema bound",
        ));
    }
    let exit_code = optional_i64(data.get("exit_code"), "tool-authority-exit-code-invalid")?;
    if exit_code.is_some_and(|value| i32::try_from(value).is_err()) {
        return Err(error(
            "tool-authority-exit-code-invalid",
            "the command exit code exceeds the provider schema bound",
        ));
    }
    let input_identity = bounded_string(
        data.get("input_identity"),
        71,
        "tool-authority-input-identity-invalid",
    )?;
    validate_sha256_identity(input_identity)?;

    Ok(CommandData {
        environment,
        risk,
        output,
        status,
        source,
        started_at_ms,
        completed_at_ms,
        duration_ms,
        exit_code,
        input_identity,
    })
}

fn build_observation(
    session_id: &str,
    turn_id: &str,
    item_id: &str,
    command: &CommandData<'_>,
    item_binding: ToolTimelineBinding,
    terminal_at_ms: Option<u64>,
) -> Result<ToolTraceObservation, ToolTraceAuthorityError> {
    let source = tool_source_name(command.source);
    let status = provider_status_name(command.status);

    let tool_identity = IdentityBuilder::new("codex-command-tool")
        .component(COMMAND_TIMELINE_SCHEMA_VERSION.as_bytes())
        .component(b"codex-app-server")
        .component(b"command-execution")
        .finish();
    let action_identity = IdentityBuilder::new("codex-command-action")
        .component(session_id.as_bytes())
        .component(turn_id.as_bytes())
        .component(item_id.as_bytes())
        .component(source.as_bytes())
        .u64(command.started_at_ms)
        .finish();
    let input_identity = IdentityBuilder::new("codex-command-input")
        .component(action_identity.as_bytes())
        .component(command.input_identity.as_bytes())
        .json(command.environment)?
        .json(command.risk)?
        .finish();

    let (state, output_identity, at_ms) = match command.status {
        ToolProviderStatus::InProgress => (ToolState::Started, None, command.started_at_ms),
        ToolProviderStatus::Completed
        | ToolProviderStatus::Failed
        | ToolProviderStatus::Declined => {
            let at_ms = terminal_at_ms.ok_or_else(|| {
                error(
                    "tool-authority-completed-time-missing",
                    "terminal command authority requires a completion timestamp",
                )
            })?;
            let identity = IdentityBuilder::new("codex-command-output")
                .component(action_identity.as_bytes())
                .component(input_identity.as_bytes())
                .component(status.as_bytes())
                .json(command.output)?
                .u64(command.started_at_ms)
                .u64(at_ms)
                .optional_u64(command.duration_ms)
                .optional_i64(command.exit_code)
                .finish();
            let state = match command.status {
                ToolProviderStatus::Completed => ToolState::Completed,
                ToolProviderStatus::Failed => ToolState::Failed,
                ToolProviderStatus::Declined => ToolState::Declined,
                ToolProviderStatus::InProgress => unreachable!("matched terminal status"),
            };
            (state, Some(identity), at_ms)
        }
    };

    let payload_binding_identity = match &item_binding {
        ToolTimelineBinding::NotPersisted => b"not-persisted".as_slice(),
        ToolTimelineBinding::Persisted {
            payload_identity, ..
        } => payload_identity.as_bytes(),
    };
    let evidence_identity = IdentityBuilder::new("codex-command-evidence")
        .component(tool_identity.as_bytes())
        .component(action_identity.as_bytes())
        .component(input_identity.as_bytes())
        .component(output_identity.as_deref().unwrap_or("none").as_bytes())
        .component(status.as_bytes())
        .component(source.as_bytes())
        .component(payload_binding_identity)
        .u64(at_ms)
        .finish();

    Ok(ToolTraceObservation {
        at_ms,
        payload: TracePayload::Tool {
            tool_identity,
            action_identity,
            state,
            provider_status: Some(command.status),
            source: Some(command.source),
            input_identity: Some(input_identity),
            output_identity,
            item_binding: Some(item_binding),
            duration_ms: command.duration_ms,
            exit_code: command.exit_code,
            evidence: EvidenceRef {
                authority: AuthorityLabel::Observed,
                source: EvidenceSource::ToolRuntime,
                identity: Some(evidence_identity),
                observed_at_ms: Some(at_ms),
            },
            redaction: RedactionSummary::metadata_only(),
        },
    })
}

fn validate_terminal_semantics(
    status: ToolProviderStatus,
    duration_ms: Option<u64>,
    exit_code: Option<i64>,
) -> Result<(), ToolTraceAuthorityError> {
    match status {
        ToolProviderStatus::InProgress => Err(error(
            "tool-authority-terminal-status-invalid",
            "terminal command authority cannot retain an in-progress status",
        )),
        ToolProviderStatus::Completed if exit_code.is_some_and(|code| code != 0) => Err(error(
            "tool-authority-exit-code-status-mismatch",
            "a completed provider command cannot carry a nonzero exit code",
        )),
        ToolProviderStatus::Failed if exit_code == Some(0) => Err(error(
            "tool-authority-exit-code-status-mismatch",
            "a failed provider command cannot carry a successful exit code",
        )),
        ToolProviderStatus::Declined if duration_ms.is_some() || exit_code.is_some() => Err(error(
            "tool-authority-declined-execution-invalid",
            "a declined provider command cannot claim execution duration or exit status",
        )),
        ToolProviderStatus::Completed
        | ToolProviderStatus::Failed
        | ToolProviderStatus::Declined => Ok(()),
    }
}

fn validate_common_item_fields(
    item_id: &str,
    kind: &str,
    role: &str,
) -> Result<(), ToolTraceAuthorityError> {
    validate_binding_id(item_id, "tool-authority-provider-command-id-invalid")?;
    if kind != "command" {
        return Err(error(
            "tool-authority-item-kind-invalid",
            "Tool authority requires a command Timeline Item",
        ));
    }
    if role != "tool" {
        return Err(error(
            "tool-authority-item-role-invalid",
            "Tool authority requires a tool-role Timeline Item",
        ));
    }
    Ok(())
}

fn validate_payload_hash(
    payload: &Value,
    expected: &ContentHash,
) -> Result<(), ToolTraceAuthorityError> {
    let bytes = serde_json::to_vec(payload).map_err(|_| {
        error(
            "tool-authority-payload-encoding-invalid",
            "the persisted command Item payload cannot be encoded",
        )
    })?;
    let actual = ContentHash::for_bytes(&bytes);
    if actual != *expected {
        return Err(error(
            "tool-authority-payload-hash-mismatch",
            "the persisted command Item payload does not match its exact hash",
        ));
    }
    Ok(())
}

fn exact_object<'a>(
    value: &'a Value,
    expected_keys: &[&str],
    code: &'static str,
    message: &'static str,
) -> Result<&'a Map<String, Value>, ToolTraceAuthorityError> {
    let object = value.as_object().ok_or_else(|| error(code, message))?;
    if object.len() != expected_keys.len()
        || !expected_keys.iter().all(|key| object.contains_key(*key))
    {
        return Err(error(code, message));
    }
    Ok(object)
}

fn required_object_field<'a>(
    object: &'a Map<String, Value>,
    field: &str,
    code: &'static str,
) -> Result<&'a Value, ToolTraceAuthorityError> {
    let value = object.get(field).ok_or_else(|| {
        error(
            code,
            "a required structured command metadata field is missing",
        )
    })?;
    if !value.is_object() {
        return Err(error(
            code,
            "a structured command metadata field is invalid",
        ));
    }
    Ok(value)
}

fn validate_structured_bound(
    value: &Value,
    code: &'static str,
) -> Result<(), ToolTraceAuthorityError> {
    let bytes = serde_json::to_vec(value)
        .map_err(|_| error(code, "structured command metadata cannot be encoded"))?;
    if bytes.len() > MAX_STRUCTURED_FIELD_BYTES {
        return Err(error(
            code,
            "structured command metadata exceeds its authority bound",
        ));
    }
    Ok(())
}

fn bounded_string<'a>(
    value: Option<&'a Value>,
    maximum: usize,
    code: &'static str,
) -> Result<&'a str, ToolTraceAuthorityError> {
    value
        .and_then(Value::as_str)
        .filter(|value| value.len() <= maximum)
        .ok_or_else(|| {
            error(
                code,
                "a command Timeline string is invalid or exceeds its bound",
            )
        })
}

fn required_u64(value: Option<&Value>, code: &'static str) -> Result<u64, ToolTraceAuthorityError> {
    value
        .and_then(Value::as_u64)
        .ok_or_else(|| error(code, "a provider lifecycle timestamp is missing or invalid"))
}

fn optional_u64(
    value: Option<&Value>,
    code: &'static str,
) -> Result<Option<u64>, ToolTraceAuthorityError> {
    match value {
        Some(Value::Null) => Ok(None),
        Some(value) => value
            .as_u64()
            .map(Some)
            .ok_or_else(|| error(code, "an optional command integer is invalid")),
        None => Err(error(code, "an optional command integer field is missing")),
    }
}

fn optional_i64(
    value: Option<&Value>,
    code: &'static str,
) -> Result<Option<i64>, ToolTraceAuthorityError> {
    match value {
        Some(Value::Null) => Ok(None),
        Some(value) => value
            .as_i64()
            .map(Some)
            .ok_or_else(|| error(code, "an optional command integer is invalid")),
        None => Err(error(code, "an optional command integer field is missing")),
    }
}

fn validate_binding_id(value: &str, code: &'static str) -> Result<(), ToolTraceAuthorityError> {
    if value.is_empty()
        || value.len() > MAX_BINDING_ID_BYTES
        || !value.is_ascii()
        || value.contains('/')
        || value.contains('\\')
        || value.contains("..")
        || value.chars().any(|character| {
            character.is_ascii_whitespace()
                || character.is_ascii_control()
                || !(character.is_ascii_alphanumeric() || ".:_@+-".contains(character))
        })
    {
        return Err(error(code, "a Tool authority binding identity is invalid"));
    }
    let lower = value.to_ascii_lowercase();
    if [
        "sk-",
        "ghp_",
        "github_pat_",
        "jwt:",
        "bearer:",
        "api_key:",
        "apikey:",
        "password:",
        "secret:",
        "authorization:",
        "private-key:",
        "-----begin",
    ]
    .iter()
    .any(|marker| lower.contains(marker))
    {
        return Err(error(
            code,
            "a Tool authority binding identity resembles a credential",
        ));
    }
    Ok(())
}

fn validate_sha256_identity(value: &str) -> Result<(), ToolTraceAuthorityError> {
    let Some(digest) = value.strip_prefix("sha256:") else {
        return Err(error(
            "tool-authority-input-identity-invalid",
            "the command input identity is invalid",
        ));
    };
    if digest.len() != 64
        || !digest
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return Err(error(
            "tool-authority-input-identity-invalid",
            "the command input identity is invalid",
        ));
    }
    Ok(())
}

fn provider_status_name(status: ToolProviderStatus) -> &'static str {
    match status {
        ToolProviderStatus::InProgress => "in-progress",
        ToolProviderStatus::Completed => "completed",
        ToolProviderStatus::Failed => "failed",
        ToolProviderStatus::Declined => "declined",
    }
}

fn tool_source_name(source: ToolSource) -> &'static str {
    match source {
        ToolSource::Agent => "agent",
        ToolSource::UserShell => "user-shell",
        ToolSource::UnifiedExecStartup => "unified-exec-startup",
        ToolSource::UnifiedExecInteraction => "unified-exec-interaction",
    }
}

fn error(code: &'static str, message: &'static str) -> ToolTraceAuthorityError {
    ToolTraceAuthorityError::new(code, message)
}

struct IdentityBuilder(Sha256);

impl IdentityBuilder {
    fn new(domain: &str) -> Self {
        let mut digest = Sha256::new();
        digest.update(b"aegisy-tool-trace-authority\0");
        update_identity_component(&mut digest, domain.as_bytes());
        Self(digest)
    }

    fn component(mut self, value: &[u8]) -> Self {
        update_identity_component(&mut self.0, value);
        self
    }

    fn json(self, value: &Value) -> Result<Self, ToolTraceAuthorityError> {
        let encoded = serde_json::to_vec(value).map_err(|_| {
            error(
                "tool-authority-identity-input-invalid",
                "structured authority input cannot be encoded",
            )
        })?;
        Ok(self.component(&encoded))
    }

    fn u64(self, value: u64) -> Self {
        self.component(&value.to_be_bytes())
    }

    fn optional_u64(self, value: Option<u64>) -> Self {
        match value {
            Some(value) => self.component(b"some").u64(value),
            None => self.component(b"none"),
        }
    }

    fn optional_i64(self, value: Option<i64>) -> Self {
        match value {
            Some(value) => self.component(b"some").component(&value.to_be_bytes()),
            None => self.component(b"none"),
        }
    }

    fn finish(self) -> String {
        format!("sha256:{:x}", self.0.finalize())
    }
}

fn update_identity_component(digest: &mut Sha256, value: &[u8]) {
    digest.update((value.len() as u64).to_be_bytes());
    digest.update(value);
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn command_data(status: &str) -> Value {
        let terminal = status != "inProgress";
        json!({
            "schema_version": COMMAND_TIMELINE_SCHEMA_VERSION,
            "command": "printf private-command",
            "command_actions": [{"type": "unknown", "command": "printf private-command"}],
            "input_identity": format!("sha256:{}", "d".repeat(64)),
            "cwd": "/private/workspace",
            "environment": {
                "environment_id": format!("environment:sha256:{}", "a".repeat(64)),
                "values_exposed": false
            },
            "risk": {"level": "medium"},
            "status": status,
            "duration_ms": if terminal && status != "declined" { json!(40) } else { Value::Null },
            "exit_code": match status {
                "completed" => json!(0),
                "failed" => json!(1),
                _ => Value::Null,
            },
            "process_id": "private-pid-1",
            "source": "agent",
            "session_id": "session-1",
            "started_at_ms": 10,
            "completed_at_ms": if terminal { json!(50) } else { Value::Null },
            "output": {
                "head": "private-output",
                "tail": "private-output",
                "total_bytes": 14,
                "source_bytes": 14,
                "retained_bytes": 14,
                "omitted_bytes": 0,
                "truncated": false,
                "redacted_count": 0,
                "redacted": false,
                "head_limit": 65536,
                "tail_limit": 196608,
                "artifact": null
            }
        })
    }

    fn timeline_item(status: &str) -> TimelineItem {
        TimelineItem {
            id: "command-1".into(),
            kind: "command".into(),
            role: "tool".into(),
            state: if status == "inProgress" {
                "started".into()
            } else {
                "completed".into()
            },
            content: "private rendered content".into(),
            data: Some(command_data(status)),
        }
    }

    fn stored_item(status: &str) -> StoredItem {
        let item = timeline_item(status);
        let payload = json!({"content": item.content, "data": item.data});
        let payload_hash = ContentHash::for_bytes(&serde_json::to_vec(&payload).unwrap());
        StoredItem {
            session_id: "session-1".into(),
            sequence: 1,
            item_id: item.id,
            turn_id: Some("turn-1".into()),
            item_kind: item.kind,
            role: item.role,
            state: item.state,
            payload,
            payload_hash,
            created_at_ms: 51,
        }
    }

    fn rehash(item: &mut StoredItem) {
        item.payload_hash = ContentHash::for_bytes(&serde_json::to_vec(&item.payload).unwrap());
    }

    #[test]
    fn valid_lifecycle_is_stable_content_free_and_exactly_item_bound() {
        let started =
            started_observation("session-1", "turn-1", &timeline_item("inProgress")).unwrap();
        let stored = stored_item("completed");
        let terminal = terminal_observation("session-1", "turn-1", &stored).unwrap();
        let repeated = terminal_observation("session-1", "turn-1", &stored).unwrap();
        assert_eq!(terminal, repeated);
        assert_eq!(started.at_ms, 10);
        assert_eq!(terminal.at_ms, 50);

        let TracePayload::Tool {
            tool_identity: started_tool,
            action_identity: started_action,
            input_identity: started_input,
            output_identity: started_output,
            state: started_state,
            item_binding: started_binding,
            ..
        } = started.payload
        else {
            panic!("expected started Tool payload")
        };
        let TracePayload::Tool {
            tool_identity: terminal_tool,
            action_identity: terminal_action,
            input_identity: terminal_input,
            output_identity: terminal_output,
            state: terminal_state,
            item_binding: terminal_binding,
            ..
        } = &terminal.payload
        else {
            panic!("expected terminal Tool payload")
        };
        assert_eq!(&started_tool, terminal_tool);
        assert_eq!(&started_action, terminal_action);
        assert_eq!(started_input.as_ref(), terminal_input.as_ref());
        assert!(started_output.is_none());
        assert!(terminal_output.is_some());
        assert_eq!(started_state, ToolState::Started);
        assert_eq!(*terminal_state, ToolState::Completed);
        assert_eq!(started_binding, Some(ToolTimelineBinding::NotPersisted));
        assert_eq!(
            terminal_binding,
            &Some(ToolTimelineBinding::Persisted {
                item_identity: persisted_item_identity("session-1", "turn-1", "command-1"),
                payload_identity: format!("sha256:{}", stored.payload_hash.sha256)
            })
        );

        let trace_json = serde_json::to_string(&terminal.payload).unwrap();
        for forbidden in [
            "private-command",
            "/private/workspace",
            "private-output",
            "private-pid-1",
            "command_actions",
            "process_id",
        ] {
            assert!(!trace_json.contains(forbidden));
        }
    }

    #[test]
    fn provider_item_id_is_hashed_before_it_enters_the_trace() {
        let provider_item_id = "Users-alice-private-project-command";
        let mut started_item = timeline_item("inProgress");
        started_item.id = provider_item_id.into();
        let started = started_observation("session-1", "turn-1", &started_item).unwrap();
        let mut stored = stored_item("completed");
        stored.item_id = provider_item_id.into();
        let terminal = terminal_observation("session-1", "turn-1", &stored).unwrap();
        let TracePayload::Tool {
            action_identity: started_action,
            ..
        } = &started.payload
        else {
            panic!("expected started Tool")
        };
        let TracePayload::Tool {
            action_identity: terminal_action,
            item_binding: Some(ToolTimelineBinding::Persisted { item_identity, .. }),
            ..
        } = &terminal.payload
        else {
            panic!("expected terminal Tool")
        };
        assert_eq!(started_action, terminal_action);
        assert_eq!(
            item_identity,
            &persisted_item_identity("session-1", "turn-1", provider_item_id)
        );
        let serialized = serde_json::to_string(&terminal.payload).unwrap();
        assert!(!serialized.contains(provider_item_id));
        assert!(!serialized.contains("alice"));
        assert!(!serialized.contains("project-command"));
    }

    #[test]
    fn missing_and_extra_schema_fields_fail_closed() {
        let mut missing = timeline_item("inProgress");
        missing
            .data
            .as_mut()
            .unwrap()
            .as_object_mut()
            .unwrap()
            .remove("started_at_ms");
        assert_eq!(
            started_observation("session-1", "turn-1", &missing)
                .unwrap_err()
                .code,
            "tool-authority-data-schema-invalid"
        );

        let mut wrong_version = timeline_item("inProgress");
        wrong_version.data.as_mut().unwrap()["schema_version"] = json!("command-timeline/9.9");
        assert_eq!(
            started_observation("session-1", "turn-1", &wrong_version)
                .unwrap_err()
                .code,
            "tool-authority-schema-version-invalid"
        );

        let mut extra = timeline_item("inProgress");
        extra
            .data
            .as_mut()
            .unwrap()
            .as_object_mut()
            .unwrap()
            .insert("provider_body".into(), json!("private"));
        assert_eq!(
            started_observation("session-1", "turn-1", &extra)
                .unwrap_err()
                .code,
            "tool-authority-data-schema-invalid"
        );
    }

    #[test]
    fn cross_session_turn_and_item_bindings_are_rejected() {
        let mut started = timeline_item("inProgress");
        started.data.as_mut().unwrap()["session_id"] = json!("session-2");
        assert_eq!(
            started_observation("session-1", "turn-1", &started)
                .unwrap_err()
                .code,
            "tool-authority-session-binding-mismatch"
        );

        let mut stored = stored_item("completed");
        stored.session_id = "session-2".into();
        assert_eq!(
            terminal_observation("session-1", "turn-1", &stored)
                .unwrap_err()
                .code,
            "tool-authority-session-binding-mismatch"
        );

        let mut stored = stored_item("completed");
        stored.turn_id = Some("turn-2".into());
        assert_eq!(
            terminal_observation("session-1", "turn-1", &stored)
                .unwrap_err()
                .code,
            "tool-authority-turn-binding-mismatch"
        );

        let mut stored = stored_item("completed");
        stored.item_id = "../../command".into();
        assert_eq!(
            terminal_observation("session-1", "turn-1", &stored)
                .unwrap_err()
                .code,
            "tool-authority-provider-command-id-invalid"
        );
    }

    #[test]
    fn exact_payload_hash_is_required() {
        let mut stored = stored_item("completed");
        stored.payload.as_object_mut().unwrap()["content"] = json!("changed");
        assert_eq!(
            terminal_observation("session-1", "turn-1", &stored)
                .unwrap_err()
                .code,
            "tool-authority-payload-hash-mismatch"
        );
    }

    #[test]
    fn hash_consistent_semantic_tampering_fails_closed() {
        let mut status = stored_item("completed");
        status.payload["data"]["status"] = json!("inProgress");
        rehash(&mut status);
        assert_eq!(
            terminal_observation("session-1", "turn-1", &status)
                .unwrap_err()
                .code,
            "tool-authority-terminal-status-invalid"
        );

        let mut time = stored_item("completed");
        time.payload["data"]["completed_at_ms"] = json!(9);
        rehash(&mut time);
        assert_eq!(
            terminal_observation("session-1", "turn-1", &time)
                .unwrap_err()
                .code,
            "tool-authority-time-order-invalid"
        );

        let mut session = stored_item("completed");
        session.payload["data"]["session_id"] = json!("session-2");
        rehash(&mut session);
        assert_eq!(
            terminal_observation("session-1", "turn-1", &session)
                .unwrap_err()
                .code,
            "tool-authority-session-binding-mismatch"
        );
    }

    #[test]
    fn provider_status_source_and_time_enums_are_closed() {
        let mut status = timeline_item("inProgress");
        status.data.as_mut().unwrap()["status"] = json!("cancelled");
        assert_eq!(
            started_observation("session-1", "turn-1", &status)
                .unwrap_err()
                .code,
            "tool-authority-provider-status-invalid"
        );

        let mut source = timeline_item("inProgress");
        source.data.as_mut().unwrap()["source"] = json!("model");
        assert_eq!(
            started_observation("session-1", "turn-1", &source)
                .unwrap_err()
                .code,
            "tool-authority-provider-source-invalid"
        );

        let mut zero = timeline_item("inProgress");
        zero.data.as_mut().unwrap()["started_at_ms"] = json!(0);
        let observation = started_observation("session-1", "turn-1", &zero).unwrap();
        assert_eq!(observation.at_ms, 0);

        let mut negative = timeline_item("inProgress");
        negative.data.as_mut().unwrap()["started_at_ms"] = json!(-1);
        assert_eq!(
            started_observation("session-1", "turn-1", &negative)
                .unwrap_err()
                .code,
            "tool-authority-started-time-invalid"
        );
    }

    #[test]
    fn duration_exit_code_and_declined_semantics_are_strict() {
        let mut long_duration = stored_item("completed");
        long_duration.payload["data"]["duration_ms"] = json!(41);
        rehash(&mut long_duration);
        assert_eq!(
            terminal_observation("session-1", "turn-1", &long_duration)
                .unwrap_err()
                .code,
            "tool-authority-duration-invalid"
        );

        let mut schema_overflow = stored_item("completed");
        schema_overflow.payload["data"]["duration_ms"] = json!((i64::MAX as u64) + 1);
        rehash(&mut schema_overflow);
        assert_eq!(
            terminal_observation("session-1", "turn-1", &schema_overflow)
                .unwrap_err()
                .code,
            "tool-authority-duration-invalid"
        );

        let mut completed_nonzero = stored_item("completed");
        completed_nonzero.payload["data"]["exit_code"] = json!(2);
        rehash(&mut completed_nonzero);
        assert_eq!(
            terminal_observation("session-1", "turn-1", &completed_nonzero)
                .unwrap_err()
                .code,
            "tool-authority-exit-code-status-mismatch"
        );

        let mut failed_zero = stored_item("failed");
        failed_zero.payload["data"]["exit_code"] = json!(0);
        rehash(&mut failed_zero);
        assert_eq!(
            terminal_observation("session-1", "turn-1", &failed_zero)
                .unwrap_err()
                .code,
            "tool-authority-exit-code-status-mismatch"
        );

        let mut declined = stored_item("declined");
        declined.payload["data"]["duration_ms"] = json!(1);
        rehash(&mut declined);
        assert_eq!(
            terminal_observation("session-1", "turn-1", &declined)
                .unwrap_err()
                .code,
            "tool-authority-declined-execution-invalid"
        );
    }

    #[test]
    fn all_closed_provider_sources_and_terminal_states_map_exactly() {
        for (source, expected) in [
            ("agent", ToolSource::Agent),
            ("userShell", ToolSource::UserShell),
            ("unifiedExecStartup", ToolSource::UnifiedExecStartup),
            ("unifiedExecInteraction", ToolSource::UnifiedExecInteraction),
        ] {
            let mut item = timeline_item("inProgress");
            item.data.as_mut().unwrap()["source"] = json!(source);
            let observation = started_observation("session-1", "turn-1", &item).unwrap();
            let TracePayload::Tool { source: actual, .. } = observation.payload else {
                panic!("expected Tool payload")
            };
            assert_eq!(actual, Some(expected));
        }

        for (status, state) in [
            ("completed", ToolState::Completed),
            ("failed", ToolState::Failed),
            ("declined", ToolState::Declined),
        ] {
            let stored = stored_item(status);
            let observation = terminal_observation("session-1", "turn-1", &stored).unwrap();
            let TracePayload::Tool { state: actual, .. } = observation.payload else {
                panic!("expected Tool payload")
            };
            assert_eq!(actual, state);
        }
    }
}
