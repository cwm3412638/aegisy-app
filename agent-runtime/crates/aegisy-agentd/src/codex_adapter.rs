use crate::codex_file_change::{validate_codex_file_changes, CodexFileUpdateChange};
use crate::command_output::CommandOutputCapture;
use crate::output_redaction::{redact_complete, OutputRedactor};
use crate::provider_error::{from_codex_error_info, ProviderError};
use crate::session_environment::{EnvironmentSummary, ProcessEnvironment, SessionEnvironment};
use crate::turn_trace::{
    configured_runtime_approval_policy_identity, effective_runtime_approval_policy_identity,
    provider_thread_identity, ApprovalPolicyPermissionProfile, ApprovalPolicyReviewer,
    ApprovalPolicySandbox, RuntimeApprovalPolicy, RuntimeDenialRequestKind, TurnTraceError,
};
use crate::{TurnCancellationHandle, TurnSteerRequest, TurnSteeringHandle};
use aegisy_aap::{MAX_TIMELINE_CONTENT_CHARACTERS, MAX_TIMELINE_IDENTIFIER_BYTES};
use serde::Serialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::env;
use std::fmt;
use std::io::{self, BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, SyncSender};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

const REQUEST_TIMEOUT: Duration = Duration::from_secs(30);
const STARTUP_TIMEOUT: Duration = Duration::from_secs(15);
const STARTUP_MAX_ATTEMPTS: usize = 3;
const STARTUP_RETRY_BACKOFF: Duration = Duration::from_millis(100);
const TURN_TIMEOUT: Duration = Duration::from_secs(10 * 60);
const CODEX_MESSAGE_QUEUE_CAPACITY: usize = 16;
const MAX_CODEX_MESSAGE_BYTES: usize = 4 * 1024 * 1024;
const MAX_AGENT_MESSAGE_CHARACTERS: usize = MAX_TIMELINE_CONTENT_CHARACTERS;
const MAX_PROVIDER_IDENTIFIER_BYTES: usize = MAX_TIMELINE_IDENTIFIER_BYTES;
const CODEX_ADAPTER_IDENTITY: &str = "codex-app-server";
const CODEX_TRACE_RUNTIME_VERSION: &str = "0.144.5";
const PINNED_CODEX_VERSION: &str = "codex-cli 0.144.5";
const MAX_SERVER_REQUEST_ID_BYTES: usize = 128;
const MAX_SERVER_REQUEST_ITEM_ID_BYTES: usize = 4 * 1024;
const MAX_UNKNOWN_NOTIFICATION_METHODS: usize = 16;
const MAX_FILE_CHANGE_ITEMS_PER_TURN: usize = 256;
const MAX_FILE_CHANGE_RETAINED_BYTES_PER_TURN: usize = 16 * 1024 * 1024;

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct BackendInfo {
    pub adapter: String,
    pub version: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub provider: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub model: Option<String>,
    pub permission_profile: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub environment: Option<EnvironmentSummary>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct AdapterHealth {
    pub state: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub process_id: Option<u32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub exit_code: Option<i32>,
    pub stderr: StderrDiagnostics,
    pub unknown_notifications: UnknownNotificationDiagnostics,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct UnknownNotificationMethodDiagnostic {
    pub method_sha256: String,
    pub count: u64,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct UnknownNotificationDiagnostics {
    pub schema_version: &'static str,
    pub total_count: u64,
    pub methods: Vec<UnknownNotificationMethodDiagnostic>,
    pub unretained_count: u64,
}

impl Default for UnknownNotificationDiagnostics {
    fn default() -> Self {
        Self {
            schema_version: "codex-unknown-notification-diagnostics/0.1",
            total_count: 0,
            methods: Vec::new(),
            unretained_count: 0,
        }
    }
}

impl UnknownNotificationDiagnostics {
    fn observe(&mut self, method: &str) {
        self.total_count = self.total_count.saturating_add(1);
        let method_sha256 = format!("sha256:{:x}", Sha256::digest(method.as_bytes()));
        if let Some(entry) = self
            .methods
            .iter_mut()
            .find(|entry| entry.method_sha256 == method_sha256)
        {
            entry.count = entry.count.saturating_add(1);
            return;
        }
        if self.methods.len() < MAX_UNKNOWN_NOTIFICATION_METHODS {
            self.methods.push(UnknownNotificationMethodDiagnostic {
                method_sha256,
                count: 1,
            });
        } else {
            self.unretained_count = self.unretained_count.saturating_add(1);
        }
    }
}

#[derive(Debug, Clone, Default, Serialize, PartialEq, Eq)]
pub struct StderrDiagnostics {
    pub bytes: u64,
    pub lines: u64,
    pub redacted_lines: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub last_class: Option<String>,
}

impl StderrDiagnostics {
    fn observe(&mut self, bytes: &[u8]) {
        if bytes.is_empty() {
            return;
        }
        self.bytes = self.bytes.saturating_add(bytes.len() as u64);
        self.lines = self
            .lines
            .saturating_add(bytes.iter().filter(|byte| **byte == b'\n').count() as u64);
        let text = String::from_utf8_lossy(bytes);
        let redacted = redact_complete(&text);
        if redacted != text {
            self.redacted_lines = self.redacted_lines.saturating_add(1);
        }
        self.last_class = Some(classify_stderr(&redacted).into());
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CodexSession {
    pub thread_id: String,
    pub provider: String,
    pub model: String,
    pub approval_policy: Option<CodexApprovalPolicyBinding>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct CodexApprovalPolicyBinding {
    pub adapter_identity: String,
    pub runtime_version: String,
    pub adapter_version: String,
    pub provider_thread_identity: String,
    pub policy: RuntimeApprovalPolicy,
    pub reviewer: ApprovalPolicyReviewer,
    pub sandbox: ApprovalPolicySandbox,
    pub permission_profile: ApprovalPolicyPermissionProfile,
    pub configured_policy_identity: String,
    pub effective_policy_identity: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum CommandStatus {
    InProgress,
    Completed,
    Failed,
    Declined,
}

impl Serialize for CommandStatus {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        serializer.serialize_str(self.as_str())
    }
}

impl CommandStatus {
    fn parse(value: &Value) -> Option<Self> {
        match value.as_str()? {
            "inProgress" => Some(Self::InProgress),
            "completed" => Some(Self::Completed),
            "failed" => Some(Self::Failed),
            "declined" => Some(Self::Declined),
            _ => None,
        }
    }

    pub(crate) const fn as_str(self) -> &'static str {
        match self {
            Self::InProgress => "inProgress",
            Self::Completed => "completed",
            Self::Failed => "failed",
            Self::Declined => "declined",
        }
    }

    const fn is_terminal(self) -> bool {
        matches!(self, Self::Completed | Self::Failed | Self::Declined)
    }
}

impl fmt::Display for CommandStatus {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum CommandSource {
    Agent,
    UserShell,
    UnifiedExecStartup,
    UnifiedExecInteraction,
}

impl Serialize for CommandSource {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        serializer.serialize_str(self.as_str())
    }
}

impl CommandSource {
    fn parse_optional(value: Option<&Value>) -> Option<Self> {
        match value {
            None => Some(Self::Agent),
            Some(value) => match value.as_str()? {
                "agent" => Some(Self::Agent),
                "userShell" => Some(Self::UserShell),
                "unifiedExecStartup" => Some(Self::UnifiedExecStartup),
                "unifiedExecInteraction" => Some(Self::UnifiedExecInteraction),
                _ => None,
            },
        }
    }

    pub(crate) const fn as_str(self) -> &'static str {
        match self {
            Self::Agent => "agent",
            Self::UserShell => "userShell",
            Self::UnifiedExecStartup => "unifiedExecStartup",
            Self::UnifiedExecInteraction => "unifiedExecInteraction",
        }
    }
}

#[derive(Clone, PartialEq, Eq)]
pub(crate) struct ProviderCommandInputFingerprint(pub(crate) [u8; 32]);

impl std::fmt::Debug for ProviderCommandInputFingerprint {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("<redacted-provider-command-input>")
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CommandItem {
    pub item_id: String,
    pub command: String,
    pub command_actions: Value,
    pub cwd: String,
    pub(crate) status: CommandStatus,
    pub output: CommandOutputCapture,
    pub redactor: OutputRedactor,
    pub duration_ms: Option<u64>,
    pub exit_code: Option<i64>,
    pub process_id: Option<String>,
    pub(crate) source: CommandSource,
    pub started_at_ms: u64,
    pub completed_at_ms: Option<u64>,
    pub(crate) provider_input_fingerprint: ProviderCommandInputFingerprint,
    pub(crate) trace_input_identity: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CodexEvent {
    TurnStarted {
        turn_id: String,
    },
    AgentStarted {
        turn_id: String,
        item_id: String,
        text: String,
    },
    AgentDelta {
        turn_id: String,
        item_id: String,
        text: String,
    },
    AgentCompleted {
        turn_id: String,
        item_id: String,
        text: String,
    },
    CommandUpdated {
        turn_id: String,
        command: Box<CommandItem>,
        lifecycle: String,
    },
    FileChangeProposalRequested {
        request: CodexFileChangeProposalRequest,
    },
    TokenUsage {
        turn_id: String,
        usage: Value,
    },
    TurnDiff {
        turn_id: String,
        diff: String,
    },
    TurnPlan {
        turn_id: String,
        explanation: Option<String>,
        steps: Vec<Value>,
    },
    TurnErrorObserved {
        turn_id: String,
        message: String,
        provider_error: Option<ProviderError>,
        will_retry: bool,
    },
    TurnCompleted {
        turn_id: String,
    },
    TurnSteeringRequested {
        turn_id: String,
        input: String,
    },
    TurnSteeringAcknowledged {
        turn_id: String,
    },
    TurnSteeringFailed {
        turn_id: String,
        message: String,
    },
    TurnCancellationAcknowledged {
        turn_id: String,
    },
    TurnCancellationFailed {
        turn_id: String,
        message: String,
    },
    TurnInterrupted {
        turn_id: String,
    },
    TurnFailed {
        turn_id: String,
        message: String,
        provider_error: Option<ProviderError>,
    },
}

#[derive(Debug)]
struct AgentMessageItem {
    text: String,
    character_count: usize,
    completed: bool,
}

pub struct CodexAdapter {
    child: Child,
    stdin: ChildStdin,
    messages: Receiver<Value>,
    next_request_id: i64,
    version: String,
    environment: EnvironmentSummary,
    stderr: Arc<Mutex<StderrDiagnostics>>,
    unknown_notifications: UnknownNotificationDiagnostics,
}

pub(crate) struct CodexTurnRequest<'a> {
    pub thread_id: &'a str,
    pub input: &'a str,
    pub local_images: &'a [PathBuf],
    pub idempotency_key: &'a str,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct CodexRuntimeDenialRequest {
    pub request_kind: RuntimeDenialRequestKind,
    pub provider_request_identity: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct CodexFileChangeProposalRequest {
    pub provider_thread_id: String,
    pub provider_turn_id: String,
    pub provider_item_id: String,
    pub approval_started_at_ms: u64,
    pub changes: Vec<CodexFileUpdateChange>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct CodexFileChangeItemState {
    changes: Vec<CodexFileUpdateChange>,
    started_at_ms: u64,
    approval_request_id: Option<Value>,
    approval_started_at_ms: Option<u64>,
    request_resolved: bool,
    completed: bool,
}

#[derive(Debug, Default)]
struct CodexFileChangeTurnState {
    pending_patches: HashMap<String, Vec<CodexFileUpdateChange>>,
    items: HashMap<String, CodexFileChangeItemState>,
    retained_bytes: usize,
}

impl CodexFileChangeTurnState {
    fn observe_patch_updated(
        &mut self,
        item_id: &str,
        changes: Vec<CodexFileUpdateChange>,
    ) -> Result<(), String> {
        if self.items.contains_key(item_id) {
            return Err(file_change_protocol_error(
                "patch update arrived after item start",
            ));
        }
        if changes.is_empty() {
            if let Some(previous) = self.pending_patches.remove(item_id) {
                self.retained_bytes = self
                    .retained_bytes
                    .checked_sub(file_change_retained_bytes(&previous)?)
                    .ok_or_else(|| {
                        file_change_protocol_error("patch byte accounting is inconsistent")
                    })?;
            }
            return Ok(());
        }
        if !self.pending_patches.contains_key(item_id)
            && self.pending_patches.len().saturating_add(self.items.len())
                >= MAX_FILE_CHANGE_ITEMS_PER_TURN
        {
            return Err(file_change_protocol_error(
                "too many patch-updated items are open",
            ));
        }
        let previous_bytes = self
            .pending_patches
            .get(item_id)
            .map(|changes| file_change_retained_bytes(changes))
            .transpose()?
            .unwrap_or_default();
        let next_bytes = file_change_retained_bytes(&changes)?;
        self.retained_bytes =
            checked_file_change_retained_total(self.retained_bytes, previous_bytes, next_bytes)?;
        self.pending_patches.insert(item_id.to_owned(), changes);
        Ok(())
    }

    fn start_item(
        &mut self,
        item_id: &str,
        changes: Vec<CodexFileUpdateChange>,
        started_at_ms: u64,
    ) -> Result<(), String> {
        let pending_item = self.pending_patches.contains_key(item_id);
        if self.items.contains_key(item_id)
            || (!pending_item
                && self.items.len().saturating_add(self.pending_patches.len())
                    >= MAX_FILE_CHANGE_ITEMS_PER_TURN)
        {
            return Err(file_change_protocol_error(
                "file change item started more than once or exceeded the limit",
            ));
        }
        let pending_bytes = self
            .pending_patches
            .get(item_id)
            .map(|changes| file_change_retained_bytes(changes))
            .transpose()?
            .unwrap_or_default();
        let started_bytes = file_change_retained_bytes(&changes)?;
        let retained_bytes =
            checked_file_change_retained_total(self.retained_bytes, pending_bytes, started_bytes)?;

        // patchUpdated is volatile progress in Codex 0.144.5. item/started is the
        // authoritative, normalized change set for proposal compilation.
        self.pending_patches.remove(item_id);
        self.items.insert(
            item_id.to_owned(),
            CodexFileChangeItemState {
                changes,
                started_at_ms,
                approval_request_id: None,
                approval_started_at_ms: None,
                request_resolved: false,
                completed: false,
            },
        );
        self.retained_bytes = retained_bytes;
        Ok(())
    }

    fn complete_item(
        &mut self,
        item_id: &str,
        completed_changes: Vec<CodexFileUpdateChange>,
        status: Option<&str>,
        completed_at_ms: u64,
    ) -> Result<(), String> {
        let state = self.items.get_mut(item_id).ok_or_else(|| {
            file_change_protocol_error("completed item has no started file change")
        })?;
        if state.completed || state.changes != completed_changes {
            return Err(file_change_protocol_error(
                "completed item is duplicated or changed identity",
            ));
        }
        if completed_at_ms < state.started_at_ms {
            return Err(file_change_protocol_error(
                "file change completion timestamp precedes its start",
            ));
        }
        match status {
            Some("declined")
                if state.approval_request_id.is_some()
                    && state.approval_started_at_ms.is_some()
                    && state.request_resolved => {}
            Some("failed") if state.approval_request_id.is_none() => {}
            Some("completed") => {
                return Err(file_change_protocol_error(
                    "provider reported a completed write in read-only mode",
                ));
            }
            _ => {
                return Err(file_change_protocol_error(
                    "file change terminal status is inconsistent",
                ));
            }
        }
        state.completed = true;
        Ok(())
    }

    fn has_open_items(&self) -> bool {
        !self.pending_patches.is_empty() || self.items.values().any(|item| !item.completed)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum CodexRuntimeDenialFailure {
    Preflight(String),
    ResponseWrite(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum CodexTurnFailure {
    Reusable(String),
    RestartRequired(String),
}

impl CodexTurnFailure {
    pub(crate) fn restart_required(&self) -> bool {
        matches!(self, Self::RestartRequired(_))
    }

    pub(crate) fn into_message(self) -> String {
        match self {
            Self::Reusable(message) | Self::RestartRequired(message) => message,
        }
    }
}

impl From<String> for CodexTurnFailure {
    fn from(message: String) -> Self {
        Self::Reusable(message)
    }
}

impl From<&str> for CodexTurnFailure {
    fn from(message: &str) -> Self {
        Self::Reusable(message.to_owned())
    }
}

impl CodexAdapter {
    pub fn start() -> Result<Self, String> {
        let mut attempts = 0;
        loop {
            attempts += 1;
            match Self::start_once() {
                Ok(adapter) => return Ok(adapter),
                Err(error)
                    if attempts < STARTUP_MAX_ATTEMPTS && is_retryable_startup_error(&error) =>
                {
                    thread::sleep(STARTUP_RETRY_BACKOFF);
                }
                Err(error) => {
                    let safe_error = bounded_string(&redact_complete(&error), 512);
                    return Err(format!(
                        "Codex App Server startup failed after {attempts} bounded attempt(s): {safe_error}"
                    ));
                }
            }
        }
    }

    fn start_once() -> Result<Self, String> {
        let executable = locate_codex();
        let process_environment = codex_process_environment()?;
        let version = codex_version(&executable, &process_environment);
        if !codex_version_is_pinned(&version) {
            return Err(format!(
                "unsupported Codex App Server version {version}; Aegisy requires {PINNED_CODEX_VERSION}"
            ));
        }
        let mut command = codex_command(&executable, &process_environment);
        command
            .args(["app-server", "--stdio"])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let mut child = command
            .spawn()
            .map_err(|error| format!("cannot start Codex App Server: {error}"))?;
        let stdin = child
            .stdin
            .take()
            .ok_or_else(|| "Codex App Server stdin is unavailable".to_owned())?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| "Codex App Server stdout is unavailable".to_owned())?;
        let stderr = child.stderr.take();
        let (sender, messages) = codex_message_channel();
        let stderr_diagnostics = Arc::new(Mutex::new(StderrDiagnostics::default()));

        thread::spawn(move || {
            let mut reader = BufReader::new(stdout);
            loop {
                let message = match read_bounded_line(&mut reader, MAX_CODEX_MESSAGE_BYTES) {
                    Ok(BoundedLine::Line(line)) if line.iter().all(u8::is_ascii_whitespace) => {
                        continue
                    }
                    Ok(BoundedLine::Line(line)) => match serde_json::from_slice::<Value>(&line) {
                        Ok(message) => message,
                        Err(_) => transport_error("Codex App Server emitted invalid JSON"),
                    },
                    Ok(BoundedLine::Oversized) => transport_error(format!(
                        "Codex App Server message exceeded the {MAX_CODEX_MESSAGE_BYTES}-byte limit"
                    )),
                    Ok(BoundedLine::Eof) => break,
                    Err(_) => transport_error("cannot read Codex App Server output"),
                };
                if sender.send(message).is_err() {
                    break;
                }
            }
        });
        if let Some(stderr) = stderr {
            let diagnostics = stderr_diagnostics.clone();
            thread::spawn(move || {
                let mut stderr = stderr;
                let mut buffer = [0_u8; 8 * 1024];
                loop {
                    match stderr.read(&mut buffer) {
                        Ok(0) | Err(_) => break,
                        Ok(bytes_read) => {
                            let mut diagnostics = diagnostics
                                .lock()
                                .unwrap_or_else(|poisoned| poisoned.into_inner());
                            diagnostics.observe(&buffer[..bytes_read]);
                        }
                    }
                }
            });
        }

        let mut adapter = Self {
            child,
            stdin,
            messages,
            next_request_id: 0,
            version,
            environment: process_environment.summary().clone(),
            stderr: stderr_diagnostics,
            unknown_notifications: UnknownNotificationDiagnostics::default(),
        };
        adapter.request_with_timeout(
            "initialize",
            json!({
                "clientInfo": {
                    "name": "aegisy-agentd",
                    "title": "Aegisy Coding",
                    "version": env!("CARGO_PKG_VERSION")
                },
                "capabilities": { "experimentalApi": false }
            }),
            STARTUP_TIMEOUT,
        )?;
        adapter.write_message(&json!({ "method": "initialized" }))?;
        Ok(adapter)
    }

    pub fn info(&self) -> BackendInfo {
        BackendInfo {
            adapter: CODEX_ADAPTER_IDENTITY.into(),
            version: self.version.clone(),
            provider: None,
            model: None,
            permission_profile: "read-only".into(),
            environment: Some(self.environment.clone()),
        }
    }

    pub fn health(&mut self) -> AdapterHealth {
        let process_id = Some(self.child.id());
        let stderr = self
            .stderr
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clone();
        match self.child.try_wait() {
            Ok(None) => AdapterHealth {
                state: "running".into(),
                process_id,
                exit_code: None,
                stderr,
                unknown_notifications: self.unknown_notifications.clone(),
            },
            Ok(Some(status)) => AdapterHealth {
                state: "exited".into(),
                process_id,
                exit_code: status.code(),
                stderr,
                unknown_notifications: self.unknown_notifications.clone(),
            },
            Err(_) => AdapterHealth {
                state: "unknown".into(),
                process_id,
                exit_code: None,
                stderr,
                unknown_notifications: self.unknown_notifications.clone(),
            },
        }
    }

    pub fn start_session(&mut self, cwd: &Path, chat: bool) -> Result<CodexSession, String> {
        let result = self.request("thread/start", thread_start_params(cwd, chat))?;
        parse_codex_session(result, "thread/start", true)
    }

    pub fn resume_session(
        &mut self,
        thread_id: &str,
        cwd: &Path,
        chat: bool,
    ) -> Result<CodexSession, String> {
        let result = self.request("thread/resume", thread_resume_params(thread_id, cwd, chat))?;
        parse_codex_session(result, "thread/resume", true)
    }

    pub fn fork_session(
        &mut self,
        thread_id: &str,
        last_turn_id: Option<&str>,
        cwd: &Path,
        chat: bool,
    ) -> Result<CodexSession, String> {
        let result = self.request(
            "thread/fork",
            thread_fork_params(thread_id, last_turn_id, cwd, chat),
        )?;
        parse_codex_session(result, "thread/fork", true)
    }

    pub fn archive_session(&mut self, thread_id: &str) -> Result<(), String> {
        self.request("thread/archive", json!({ "threadId": thread_id }))?;
        Ok(())
    }

    #[allow(dead_code)]
    pub fn unarchive_session(&mut self, thread_id: &str) -> Result<CodexSession, String> {
        let result = self.request("thread/unarchive", json!({ "threadId": thread_id }))?;
        parse_codex_session(result, "thread/unarchive", false)
    }

    #[allow(dead_code)]
    pub fn delete_session(&mut self, thread_id: &str) -> Result<(), String> {
        self.request("thread/delete", json!({ "threadId": thread_id }))?;
        Ok(())
    }

    #[allow(dead_code)]
    pub fn list_threads(
        &mut self,
        cursor: Option<&str>,
        limit: Option<u32>,
        cwd: Option<&Path>,
        archived: Option<bool>,
    ) -> Result<Value, String> {
        self.request(
            "thread/list",
            thread_list_params(cursor, limit, cwd, archived),
        )
    }

    #[allow(dead_code)]
    pub fn read_thread(&mut self, thread_id: &str, include_turns: bool) -> Result<Value, String> {
        self.request("thread/read", thread_read_params(thread_id, include_turns))
    }

    #[allow(dead_code)]
    pub fn compact_thread(&mut self, thread_id: &str) -> Result<(), String> {
        self.request(
            "thread/compact/start",
            thread_compact_start_params(thread_id),
        )?;
        Ok(())
    }
}

fn parse_codex_session(
    result: Value,
    method: &str,
    require_approval_policy: bool,
) -> Result<CodexSession, String> {
    let thread_id = result
        .pointer("/thread/id")
        .and_then(Value::as_str)
        .ok_or_else(|| format!("Codex {method} response is missing thread.id"))?;
    let approval_policy = require_approval_policy
        .then(|| parse_codex_approval_policy_binding(&result, thread_id, method))
        .transpose()?;
    let provider = result
        .get("modelProvider")
        .or_else(|| result.pointer("/thread/modelProvider"))
        .and_then(Value::as_str)
        .unwrap_or("unknown");
    let model = result
        .get("model")
        .or_else(|| result.pointer("/thread/model"))
        .and_then(Value::as_str)
        .unwrap_or("unknown");
    Ok(CodexSession {
        thread_id: thread_id.to_owned(),
        provider: provider.to_owned(),
        model: model.to_owned(),
        approval_policy,
    })
}

fn parse_codex_approval_policy_binding(
    result: &Value,
    thread_id: &str,
    method: &str,
) -> Result<CodexApprovalPolicyBinding, String> {
    if result.get("approvalPolicy").and_then(Value::as_str) != Some("never") {
        return Err(format!(
            "Codex {method} response did not preserve approvalPolicy=never"
        ));
    }

    let reviewer = match result.get("approvalsReviewer").and_then(Value::as_str) {
        Some("user") => ApprovalPolicyReviewer::User,
        Some("auto_review") => ApprovalPolicyReviewer::AutoReview,
        Some("guardian_subagent") => ApprovalPolicyReviewer::GuardianSubagent,
        _ => {
            return Err(format!(
                "Codex {method} response has a missing or unsupported approvalsReviewer"
            ));
        }
    };

    let sandbox = result
        .get("sandbox")
        .and_then(Value::as_object)
        .ok_or_else(|| format!("Codex {method} response is missing the read-only sandbox"))?;
    if sandbox.len() != 2
        || sandbox.get("type").and_then(Value::as_str) != Some("readOnly")
        || sandbox.get("networkAccess").and_then(Value::as_bool) != Some(false)
    {
        return Err(format!(
            "Codex {method} response did not preserve the closed read-only sandbox"
        ));
    }

    codex_approval_policy_binding(thread_id, reviewer).map_err(|error| {
        format!(
            "Codex {method} response has an invalid thread identity ({})",
            error.code
        )
    })
}

pub(crate) fn codex_approval_policy_binding(
    thread_id: &str,
    reviewer: ApprovalPolicyReviewer,
) -> Result<CodexApprovalPolicyBinding, TurnTraceError> {
    let policy = RuntimeApprovalPolicy::Never;
    let sandbox = ApprovalPolicySandbox::ReadOnly;
    let permission_profile = ApprovalPolicyPermissionProfile::ReadOnly;
    let provider_thread_identity = provider_thread_identity(thread_id)?;
    let configured_policy_identity =
        configured_runtime_approval_policy_identity(policy, sandbox, permission_profile);
    let effective_policy_identity = effective_runtime_approval_policy_identity(
        &provider_thread_identity,
        policy,
        reviewer,
        sandbox,
        permission_profile,
    );
    Ok(CodexApprovalPolicyBinding {
        adapter_identity: CODEX_ADAPTER_IDENTITY.into(),
        runtime_version: CODEX_TRACE_RUNTIME_VERSION.into(),
        adapter_version: PINNED_CODEX_VERSION.into(),
        provider_thread_identity,
        policy,
        reviewer,
        sandbox,
        permission_profile,
        configured_policy_identity,
        effective_policy_identity,
    })
}

impl CodexAdapter {
    pub(crate) fn run_turn<F, D>(
        &mut self,
        request: CodexTurnRequest<'_>,
        cancellation: &TurnCancellationHandle,
        steering: &TurnSteeringHandle,
        mut emit: F,
        mut deny_runtime_request: D,
    ) -> Result<(), CodexTurnFailure>
    where
        F: FnMut(CodexEvent) -> bool,
        D: FnMut(
            CodexRuntimeDenialRequest,
            &mut dyn FnMut() -> Result<(), String>,
        ) -> Result<(), CodexRuntimeDenialFailure>,
    {
        let request_id = self.write_request("turn/start", turn_start_params(&request))?;
        let response = self.wait_for_response(request_id, REQUEST_TIMEOUT)?;
        let turn_id = response
            .pointer("/turn/id")
            .and_then(Value::as_str)
            .filter(|turn_id| valid_provider_identifier(turn_id))
            .ok_or_else(|| "Codex turn/start response has no valid turn.id".to_owned())?
            .to_owned();
        emit(CodexEvent::TurnStarted {
            turn_id: turn_id.clone(),
        });

        let mut agent_messages = HashMap::<String, AgentMessageItem>::new();
        let mut commands = HashMap::<String, CommandItem>::new();
        let mut file_changes = CodexFileChangeTurnState::default();
        let deadline = Instant::now() + TURN_TIMEOUT;
        let mut interrupt_sent = false;
        let mut interrupt_request_id = None;
        let mut pending_steers = HashMap::<i64, TurnSteerRequest>::new();
        loop {
            if cancellation.is_requested() && !interrupt_sent {
                interrupt_request_id = Some(self.write_request(
                    "turn/interrupt",
                    turn_interrupt_params(request.thread_id, &turn_id),
                )?);
                interrupt_sent = true;
            }
            for steer in steering.drain() {
                let request_id = self.write_request(
                    "turn/steer",
                    turn_steer_params(request.thread_id, &turn_id, &steer),
                )?;
                emit(CodexEvent::TurnSteeringRequested {
                    turn_id: turn_id.clone(),
                    input: steer.input.clone(),
                });
                pending_steers.insert(request_id, steer);
            }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Err("Codex App Server timed out".into());
            }
            let Some(message) = self.receive_optional(remaining.min(Duration::from_millis(100)))?
            else {
                continue;
            };
            if let Some(request_kind) = runtime_denial_request_kind(&message) {
                if request_kind == RuntimeDenialRequestKind::FileChange {
                    let mut accepted = None;
                    self.handle_turn_runtime_denial_request(
                        &message,
                        request.thread_id,
                        &turn_id,
                        request_kind,
                        &mut |id, params| {
                            let item_id =
                                params
                                    .get("itemId")
                                    .and_then(Value::as_str)
                                    .ok_or_else(|| {
                                        "Codex file change approval item identity is missing"
                                            .to_owned()
                                    })?;
                            let started_at_ms = params
                                .get("startedAtMs")
                                .and_then(Value::as_i64)
                                .and_then(|value| u64::try_from(value).ok())
                                .ok_or_else(|| {
                                    "Codex file change approval timestamp is invalid".to_owned()
                                })?;
                            let state = file_changes.items.get(item_id).ok_or_else(|| {
                                "Codex file change approval has no started item".to_owned()
                            })?;
                            if state.completed || state.approval_request_id.is_some() {
                                return Err(
                                    "Codex file change approval was requested more than once"
                                        .to_owned(),
                                );
                            }
                            let proposal_request = CodexFileChangeProposalRequest {
                                provider_thread_id: request.thread_id.to_owned(),
                                provider_turn_id: turn_id.clone(),
                                provider_item_id: item_id.to_owned(),
                                approval_started_at_ms: started_at_ms,
                                changes: state.changes.clone(),
                            };
                            if !emit(CodexEvent::FileChangeProposalRequested {
                                request: proposal_request,
                            }) {
                                return Err(
                                    "Codex file change proposal could not be persisted".to_owned()
                                );
                            }
                            accepted = Some((item_id.to_owned(), id.clone(), started_at_ms));
                            Ok(())
                        },
                        &mut deny_runtime_request,
                    )?;
                    let Some((item_id, request_id, started_at_ms)) = accepted else {
                        return Err(CodexTurnFailure::Reusable(
                            "Codex file change approval persistence did not complete".to_owned(),
                        ));
                    };
                    let state = file_changes.items.get_mut(&item_id).ok_or_else(|| {
                        CodexTurnFailure::Reusable(
                            "Codex file change approval lost its started item".to_owned(),
                        )
                    })?;
                    state.approval_request_id = Some(request_id);
                    state.approval_started_at_ms = Some(started_at_ms);
                } else {
                    self.handle_turn_runtime_denial_request(
                        &message,
                        request.thread_id,
                        &turn_id,
                        request_kind,
                        &mut |_, _| Ok(()),
                        &mut deny_runtime_request,
                    )?;
                }
                continue;
            }
            if self.handle_server_request(&message)? {
                continue;
            }
            if interrupt_request_id.is_some()
                && message.get("id").and_then(Value::as_i64) == interrupt_request_id
            {
                interrupt_request_id = None;
                if let Some(error) = message.get("error") {
                    emit(CodexEvent::TurnCancellationFailed {
                        turn_id: turn_id.clone(),
                        message: error
                            .get("message")
                            .and_then(Value::as_str)
                            .unwrap_or("Codex rejected turn interruption")
                            .to_owned(),
                    });
                } else {
                    emit(CodexEvent::TurnCancellationAcknowledged {
                        turn_id: turn_id.clone(),
                    });
                }
                continue;
            }
            if let Some(_steer) = message
                .get("id")
                .and_then(Value::as_i64)
                .and_then(|id| pending_steers.remove(&id))
            {
                if let Some(error) = message.get("error") {
                    emit(CodexEvent::TurnSteeringFailed {
                        turn_id: turn_id.clone(),
                        message: error
                            .get("message")
                            .and_then(Value::as_str)
                            .unwrap_or("Codex rejected turn steering")
                            .to_owned(),
                    });
                } else if message.pointer("/result/turnId").and_then(Value::as_str)
                    != Some(turn_id.as_str())
                {
                    emit(CodexEvent::TurnSteeringFailed {
                        turn_id: turn_id.clone(),
                        message: "Codex turn/steer response did not match the active turn"
                            .to_owned(),
                    });
                } else {
                    emit(CodexEvent::TurnSteeringAcknowledged {
                        turn_id: turn_id.clone(),
                    });
                }
                continue;
            }
            self.observe_unknown_notification(&message);
            let method = message.get("method").and_then(Value::as_str).unwrap_or("");
            let params = message.get("params").cloned().unwrap_or(Value::Null);
            if params.get("threadId").and_then(Value::as_str) != Some(request.thread_id) {
                continue;
            }
            if method == "item/fileChange/patchUpdated" {
                if params.get("turnId").and_then(Value::as_str) != Some(turn_id.as_str()) {
                    return Err(file_change_protocol_error(
                        "patch update does not match the active turn",
                    )
                    .into());
                }
                let item_id = required_file_change_item_id_value(params.get("itemId"))?;
                let changes = parse_file_update_changes(params.get("changes"), true)?;
                file_changes.observe_patch_updated(item_id, changes)?;
                continue;
            }
            if method == "serverRequest/resolved" {
                let Some(request_id) = params.get("requestId") else {
                    return Err(file_change_protocol_error(
                        "resolved server request identity is missing",
                    )
                    .into());
                };
                if let Some(state) = file_changes
                    .items
                    .values_mut()
                    .find(|state| state.approval_request_id.as_ref() == Some(request_id))
                {
                    if state.request_resolved {
                        return Err(file_change_protocol_error(
                            "approval request resolved more than once",
                        )
                        .into());
                    }
                    state.request_resolved = true;
                }
                continue;
            }
            if matches!(
                method,
                "item/started" | "item/commandExecution/outputDelta" | "item/completed"
            ) {
                if let Some(event) =
                    translate_command_notification(method, &params, &turn_id, &mut commands)?
                {
                    emit(event);
                    continue;
                }
                if method == "item/commandExecution/outputDelta" {
                    continue;
                }
            }
            if method == "item/started" {
                let item = params
                    .get("item")
                    .ok_or_else(|| agent_message_protocol_error("started item is missing"))?;
                if item.get("type").and_then(Value::as_str) == Some("fileChange") {
                    if params.get("turnId").and_then(Value::as_str) != Some(turn_id.as_str()) {
                        return Err(file_change_protocol_error(
                            "started item does not match the active turn",
                        )
                        .into());
                    }
                    if params.get("completedAtMs").is_some() {
                        return Err(file_change_protocol_error(
                            "started notification carries a completion timestamp",
                        )
                        .into());
                    }
                    let started_at_ms = required_file_change_timestamp(&params, "startedAtMs")?;
                    let item_id = required_file_change_item_id(item)?;
                    if item.get("status").and_then(Value::as_str) != Some("inProgress") {
                        return Err(file_change_protocol_error(
                            "started item status is not in progress",
                        )
                        .into());
                    }
                    let changes = parse_file_update_changes(item.get("changes"), false)?;
                    file_changes.start_item(item_id, changes, started_at_ms)?;
                    continue;
                }
                if item.get("type").and_then(Value::as_str) == Some("agentMessage") {
                    let item_id = required_agent_message_item_id(item)?;
                    if agent_messages.contains_key(item_id) {
                        return Err(agent_message_protocol_error(
                            "agent message item started more than once",
                        )
                        .into());
                    }
                    let text = bounded_agent_message_text(item.get("text"))?;
                    agent_messages.insert(
                        item_id.to_owned(),
                        AgentMessageItem {
                            character_count: text.chars().count(),
                            text: text.clone(),
                            completed: false,
                        },
                    );
                    emit(CodexEvent::AgentStarted {
                        turn_id: turn_id.clone(),
                        item_id: item_id.to_owned(),
                        text,
                    });
                    continue;
                }
            }
            if let Some(event) = translate_turn_notification(method, &params, &turn_id) {
                emit(event);
                continue;
            }
            match method {
                "item/agentMessage/delta" => {
                    let item_id = required_agent_message_item_id_value(params.get("itemId"))?;
                    let delta = params
                        .get("delta")
                        .and_then(Value::as_str)
                        .ok_or_else(|| agent_message_protocol_error("delta text is missing"))?;
                    let message = agent_messages.get_mut(item_id).ok_or_else(|| {
                        agent_message_protocol_error("delta has no started agent message item")
                    })?;
                    if message.completed {
                        return Err(agent_message_protocol_error(
                            "delta arrived after agent message completion",
                        )
                        .into());
                    }
                    let next_character_count =
                        checked_agent_message_character_count(message.character_count, delta)?;
                    message.text.push_str(delta);
                    message.character_count = next_character_count;
                    emit(CodexEvent::AgentDelta {
                        turn_id: turn_id.clone(),
                        item_id: item_id.to_owned(),
                        text: message.text.clone(),
                    });
                }
                "item/completed" => {
                    let item = params.get("item").cloned().unwrap_or(Value::Null);
                    if item.get("type").and_then(Value::as_str) == Some("fileChange") {
                        if params.get("turnId").and_then(Value::as_str) != Some(turn_id.as_str()) {
                            return Err(file_change_protocol_error(
                                "completed item does not match the active turn",
                            )
                            .into());
                        }
                        let item_id = required_file_change_item_id(&item)?;
                        let completed_changes =
                            parse_file_update_changes(item.get("changes"), false)?;
                        let completed_at_ms =
                            required_file_change_timestamp(&params, "completedAtMs")?;
                        file_changes.complete_item(
                            item_id,
                            completed_changes,
                            item.get("status").and_then(Value::as_str),
                            completed_at_ms,
                        )?;
                        continue;
                    }
                    if item.get("type").and_then(Value::as_str) != Some("agentMessage") {
                        continue;
                    }
                    let item_id = required_agent_message_item_id(&item)?;
                    let message = agent_messages.get_mut(item_id).ok_or_else(|| {
                        agent_message_protocol_error(
                            "completed item has no started agent message item",
                        )
                    })?;
                    if message.completed {
                        return Err(agent_message_protocol_error(
                            "agent message item completed more than once",
                        )
                        .into());
                    }
                    let text = match item.get("text") {
                        Some(value) => bounded_agent_message_text(Some(value))?,
                        None => message.text.clone(),
                    };
                    message.completed = true;
                    message.character_count = text.chars().count();
                    message.text = text.clone();
                    emit(CodexEvent::AgentCompleted {
                        turn_id: turn_id.clone(),
                        item_id: item_id.to_owned(),
                        text,
                    });
                }
                "turn/completed" => {
                    if agent_messages.values().any(|message| !message.completed)
                        || commands
                            .values()
                            .any(|command| !command.status.is_terminal())
                        || file_changes.has_open_items()
                    {
                        return Err(agent_message_protocol_error(
                            "turn reached a terminal state with an open item",
                        )
                        .into());
                    }
                    emit(turn_terminal_event(&params, &turn_id)?);
                    return Ok(());
                }
                "error" => {
                    if let Some(event) = turn_error_observation(&params, &turn_id)? {
                        emit(event);
                    }
                }
                _ => {}
            }
        }
    }

    fn request(&mut self, method: &str, params: Value) -> Result<Value, String> {
        self.request_with_timeout(method, params, REQUEST_TIMEOUT)
    }

    fn request_with_timeout(
        &mut self,
        method: &str,
        params: Value,
        timeout: Duration,
    ) -> Result<Value, String> {
        let id = self.write_request(method, params)?;
        self.wait_for_response(id, timeout)
    }

    fn write_request(&mut self, method: &str, params: Value) -> Result<i64, String> {
        self.next_request_id += 1;
        let id = self.next_request_id;
        self.write_message(&json!({ "id": id, "method": method, "params": params }))?;
        Ok(id)
    }

    fn wait_for_response(&mut self, request_id: i64, timeout: Duration) -> Result<Value, String> {
        let deadline = Instant::now() + timeout;
        loop {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Err("Codex App Server timed out".into());
            }
            let message = self.receive(remaining)?;
            if self.handle_server_request(&message)? {
                continue;
            }
            if message.get("id").and_then(Value::as_i64) != Some(request_id) {
                self.observe_unknown_notification(&message);
                continue;
            }
            if let Some(error) = message.get("error") {
                let detail = error
                    .get("message")
                    .and_then(Value::as_str)
                    .unwrap_or("unknown Codex App Server error");
                return Err(detail.to_owned());
            }
            return message
                .get("result")
                .cloned()
                .ok_or_else(|| "Codex App Server response is missing result".to_owned());
        }
    }

    fn receive(&mut self, timeout: Duration) -> Result<Value, String> {
        self.receive_optional(timeout)?
            .ok_or_else(|| "Codex App Server timed out".into())
    }

    fn observe_unknown_notification(&mut self, message: &Value) {
        if message.get("id").is_some() {
            return;
        }
        let Some(method) = message.get("method").and_then(Value::as_str) else {
            return;
        };
        if !crate::codex_capability_matrix::is_known_server_notification(method) {
            self.unknown_notifications.observe(method);
        }
    }

    fn receive_optional(&mut self, timeout: Duration) -> Result<Option<Value>, String> {
        match self.messages.recv_timeout(timeout) {
            Ok(message)
                if message.get("method").and_then(Value::as_str)
                    == Some("aegisy/transportError") =>
            {
                Err(message
                    .pointer("/params/message")
                    .and_then(Value::as_str)
                    .unwrap_or("Codex App Server transport failed")
                    .to_owned())
            }
            Ok(message) => Ok(Some(message)),
            Err(RecvTimeoutError::Timeout) => Ok(None),
            Err(RecvTimeoutError::Disconnected) => {
                Err("Codex App Server closed its output channel".into())
            }
        }
    }

    fn handle_server_request(&mut self, message: &Value) -> Result<bool, String> {
        let Some(method) = message.get("method").and_then(Value::as_str) else {
            return Ok(false);
        };
        if matches!(
            method,
            "item/commandExecution/requestApproval"
                | "item/fileChange/requestApproval"
                | "item/permissions/requestApproval"
        ) {
            let id = message
                .get("id")
                .filter(|value| valid_server_request_id(value))
                .cloned()
                .unwrap_or(Value::Null);
            self.write_message(&json!({
                "id": id,
                "error": {
                    "code": -32602,
                    "message": "approval request requires an active bound turn"
                }
            }))?;
            return Err("Codex approval request arrived without an active bound turn".into());
        }
        if method == "item/tool/requestUserInput" {
            return match user_input_request_disposition(message) {
                UserInputRequestDisposition::Continue(response) => {
                    self.write_message(&response)?;
                    Ok(true)
                }
                UserInputRequestDisposition::FailClosed(response) => {
                    self.write_message(&response)?;
                    Err("Codex user input request has an invalid request ID".into())
                }
            };
        }
        let Some(id) = message.get("id").cloned() else {
            return Ok(false);
        };
        self.write_message(&json!({
            "id": id,
            "error": { "code": -32601, "message": "unsupported server request" }
        }))?;
        Ok(true)
    }

    fn handle_turn_runtime_denial_request<B, D>(
        &mut self,
        message: &Value,
        active_thread_id: &str,
        active_turn_id: &str,
        request_kind: RuntimeDenialRequestKind,
        before_denial: &mut B,
        deny_runtime_request: &mut D,
    ) -> Result<(), CodexTurnFailure>
    where
        B: FnMut(&Value, &serde_json::Map<String, Value>) -> Result<(), String>,
        D: FnMut(
            CodexRuntimeDenialRequest,
            &mut dyn FnMut() -> Result<(), String>,
        ) -> Result<(), CodexRuntimeDenialFailure>,
    {
        let Some(id) = message
            .get("id")
            .filter(|value| valid_server_request_id(value))
            .cloned()
        else {
            self.write_message(&invalid_runtime_denial_request_error())
                .map_err(CodexTurnFailure::RestartRequired)?;
            return Err(CodexTurnFailure::RestartRequired(
                "Codex approval request has an invalid request ID".to_owned(),
            ));
        };
        let Some(params) = message.get("params").and_then(Value::as_object) else {
            self.write_message(&invalid_runtime_denial_request_error())
                .map_err(CodexTurnFailure::RestartRequired)?;
            return Err(CodexTurnFailure::RestartRequired(
                "Codex approval request params are invalid".to_owned(),
            ));
        };
        let thread_matches =
            params.get("threadId").and_then(Value::as_str) == Some(active_thread_id);
        let turn_matches = params.get("turnId").and_then(Value::as_str) == Some(active_turn_id);
        let item_valid = params
            .get("itemId")
            .and_then(Value::as_str)
            .is_some_and(|item| !item.is_empty() && item.len() <= MAX_SERVER_REQUEST_ITEM_ID_BYTES);
        let started_at_valid = params
            .get("startedAtMs")
            .and_then(Value::as_i64)
            .is_some_and(|value| value >= 0);
        if !thread_matches || !turn_matches || !item_valid || !started_at_valid {
            self.write_message(&json!({
                "id": id,
                "error": {
                    "code": -32602,
                    "message": "approval request does not match the active turn"
                }
            }))
            .map_err(CodexTurnFailure::RestartRequired)?;
            return Err(CodexTurnFailure::Reusable(
                "Codex approval request does not match the active turn".to_owned(),
            ));
        }
        let provider_request_identity = match codex_runtime_denial_request_identity(message) {
            Ok(identity) => identity,
            Err(error) => {
                self.write_message(&runtime_denial_preflight_error(id))
                    .map_err(CodexTurnFailure::RestartRequired)?;
                return Err(CodexTurnFailure::Reusable(error));
            }
        };
        if let Err(error) = before_denial(&id, params) {
            self.write_message(&runtime_denial_preflight_error(id))
                .map_err(CodexTurnFailure::RestartRequired)?;
            return Err(CodexTurnFailure::Reusable(error));
        }
        let result = match request_kind {
            RuntimeDenialRequestKind::CommandExecution | RuntimeDenialRequestKind::FileChange => {
                json!({ "decision": "decline" })
            }
            RuntimeDenialRequestKind::Permissions => {
                json!({ "permissions": {}, "scope": "turn" })
            }
        };
        let response = json!({ "id": id, "result": result });
        let mut write_denial = || self.write_message(&response);
        match deny_runtime_request(
            CodexRuntimeDenialRequest {
                request_kind,
                provider_request_identity,
            },
            &mut write_denial,
        ) {
            Ok(()) => Ok(()),
            Err(CodexRuntimeDenialFailure::Preflight(error)) => {
                self.write_message(&runtime_denial_preflight_error(id))
                    .map_err(CodexTurnFailure::RestartRequired)?;
                Err(CodexTurnFailure::Reusable(error))
            }
            Err(CodexRuntimeDenialFailure::ResponseWrite(error)) => {
                Err(CodexTurnFailure::RestartRequired(error))
            }
        }
    }

    fn write_message(&mut self, message: &Value) -> Result<(), String> {
        serde_json::to_writer(&mut self.stdin, message)
            .map_err(|error| format!("cannot encode Codex request: {error}"))?;
        self.stdin
            .write_all(b"\n")
            .and_then(|_| self.stdin.flush())
            .map_err(|error| format!("cannot write to Codex App Server: {error}"))
    }
}

fn required_agent_message_item_id(item: &Value) -> Result<&str, String> {
    required_agent_message_item_id_value(item.get("id"))
}

fn required_file_change_item_id(item: &Value) -> Result<&str, String> {
    required_file_change_item_id_value(item.get("id"))
}

fn required_file_change_item_id_value(value: Option<&Value>) -> Result<&str, String> {
    let item_id = value
        .and_then(Value::as_str)
        .ok_or_else(|| file_change_protocol_error("item identity is missing"))?;
    if !valid_provider_identifier(item_id) {
        return Err(file_change_protocol_error("item identity is invalid"));
    }
    Ok(item_id)
}

fn parse_file_update_changes(
    value: Option<&Value>,
    allow_empty: bool,
) -> Result<Vec<CodexFileUpdateChange>, String> {
    let changes = value
        .cloned()
        .ok_or_else(|| file_change_protocol_error("changes are missing"))?;
    validate_file_update_change_wire(&changes)?;
    let changes: Vec<CodexFileUpdateChange> = serde_json::from_value(changes)
        .map_err(|_| file_change_protocol_error("changes do not match the pinned schema"))?;
    if allow_empty && changes.is_empty() {
        return Ok(changes);
    }
    validate_codex_file_changes(&changes)
        .map_err(|error| file_change_protocol_error(&error.message))?;
    Ok(changes)
}

fn validate_file_update_change_wire(value: &Value) -> Result<(), String> {
    let changes = value
        .as_array()
        .ok_or_else(|| file_change_protocol_error("changes are not an array"))?;
    for change in changes {
        let change = change
            .as_object()
            .filter(|object| {
                object.len() == 3
                    && object.contains_key("path")
                    && object.contains_key("kind")
                    && object.contains_key("diff")
            })
            .ok_or_else(|| {
                file_change_protocol_error("change fields do not match the pinned schema")
            })?;
        let kind = change
            .get("kind")
            .and_then(Value::as_object)
            .ok_or_else(|| file_change_protocol_error("change kind is invalid"))?;
        let kind_name = kind.get("type").and_then(Value::as_str);
        let valid = match kind_name {
            Some("add" | "delete") => kind.len() == 1,
            Some("update") => {
                (kind.len() == 1 || (kind.len() == 2 && kind.contains_key("move_path")))
                    && kind
                        .get("move_path")
                        .is_none_or(|value| value.is_null() || value.is_string())
            }
            _ => false,
        };
        if !valid {
            return Err(file_change_protocol_error(
                "change kind fields do not match the pinned schema",
            ));
        }
    }
    Ok(())
}

fn file_change_retained_bytes(changes: &[CodexFileUpdateChange]) -> Result<usize, String> {
    changes.iter().try_fold(0_usize, |total, change| {
        let move_path_bytes = match &change.kind {
            crate::codex_file_change::CodexPatchChangeKind::Update { move_path } => {
                move_path.as_deref().map_or(0, str::len)
            }
            _ => 0,
        };
        total
            .checked_add(change.path.len())
            .and_then(|bytes| bytes.checked_add(change.diff.len()))
            .and_then(|bytes| bytes.checked_add(move_path_bytes))
            .ok_or_else(|| file_change_protocol_error("file change byte accounting overflowed"))
    })
}

fn checked_file_change_retained_total(
    retained_bytes: usize,
    replaced_bytes: usize,
    next_bytes: usize,
) -> Result<usize, String> {
    retained_bytes
        .checked_sub(replaced_bytes)
        .and_then(|bytes| bytes.checked_add(next_bytes))
        .filter(|bytes| *bytes <= MAX_FILE_CHANGE_RETAINED_BYTES_PER_TURN)
        .ok_or_else(|| {
            file_change_protocol_error("single-turn retained file change budget is exceeded")
        })
}

fn required_file_change_timestamp(params: &Value, field: &str) -> Result<u64, String> {
    params
        .get(field)
        .and_then(Value::as_i64)
        .and_then(|value| u64::try_from(value).ok())
        .ok_or_else(|| file_change_protocol_error("lifecycle timestamp is invalid"))
}

fn file_change_protocol_error(reason: &str) -> String {
    format!("Codex file change lifecycle protocol error: {reason}")
}

fn required_agent_message_item_id_value(value: Option<&Value>) -> Result<&str, String> {
    let item_id = value
        .and_then(Value::as_str)
        .ok_or_else(|| agent_message_protocol_error("item identity is missing"))?;
    if !valid_provider_identifier(item_id) {
        return Err(agent_message_protocol_error("item identity is invalid"));
    }
    Ok(item_id)
}

fn valid_provider_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= MAX_PROVIDER_IDENTIFIER_BYTES
        && value.bytes().all(|byte| byte.is_ascii_graphic())
}

fn bounded_agent_message_text(value: Option<&Value>) -> Result<String, String> {
    let text = value.and_then(Value::as_str).unwrap_or("");
    if text.chars().count() > MAX_AGENT_MESSAGE_CHARACTERS {
        return Err(agent_message_protocol_error("agent message is too large"));
    }
    Ok(text.to_owned())
}

fn checked_agent_message_character_count(current: usize, delta: &str) -> Result<usize, String> {
    current
        .checked_add(delta.chars().count())
        .filter(|count| *count <= MAX_AGENT_MESSAGE_CHARACTERS)
        .ok_or_else(|| agent_message_protocol_error("agent message is too large"))
}

fn agent_message_protocol_error(reason: &str) -> String {
    format!("Codex agent message lifecycle protocol error: {reason}")
}

fn runtime_denial_request_kind(message: &Value) -> Option<RuntimeDenialRequestKind> {
    match message.get("method").and_then(Value::as_str) {
        Some("item/commandExecution/requestApproval") => {
            Some(RuntimeDenialRequestKind::CommandExecution)
        }
        Some("item/fileChange/requestApproval") => Some(RuntimeDenialRequestKind::FileChange),
        Some("item/permissions/requestApproval") => Some(RuntimeDenialRequestKind::Permissions),
        _ => None,
    }
}

fn valid_server_request_id(value: &Value) -> bool {
    value.as_i64().is_some()
        || value.as_str().is_some_and(|value| {
            !value.is_empty()
                && value.len() <= MAX_SERVER_REQUEST_ID_BYTES
                && !value.chars().any(char::is_control)
        })
}

#[derive(Debug, Clone, PartialEq)]
enum UserInputRequestDisposition {
    Continue(Value),
    FailClosed(Value),
}

fn user_input_request_disposition(message: &Value) -> UserInputRequestDisposition {
    let Some(id) = message
        .get("id")
        .filter(|value| valid_server_request_id(value))
        .cloned()
    else {
        return UserInputRequestDisposition::FailClosed(json!({
            "id": Value::Null,
            "error": {
                "code": -32602,
                "message": "user input request is invalid"
            }
        }));
    };
    UserInputRequestDisposition::Continue(json!({
        "id": id,
        "error": {
            "code": -32601,
            "message": "user input request is unsupported"
        }
    }))
}

fn invalid_runtime_denial_request_error() -> Value {
    json!({
        "id": Value::Null,
        "error": {
            "code": -32602,
            "message": "approval request is invalid"
        }
    })
}

fn runtime_denial_preflight_error(id: Value) -> Value {
    json!({
        "id": id,
        "error": {
            "code": -32000,
            "message": "runtime denial could not be recorded"
        }
    })
}

fn codex_runtime_denial_request_identity(message: &Value) -> Result<String, String> {
    let bytes = serde_json::to_vec(message)
        .map_err(|_| "Codex approval request cannot be fingerprinted".to_owned())?;
    let mut digest = Sha256::new();
    digest.update(b"aegisy-codex-runtime-denial-request/0.1\0");
    digest.update((bytes.len() as u64).to_be_bytes());
    digest.update(bytes);
    Ok(format!("sha256:{:x}", digest.finalize()))
}

fn translate_command_notification(
    method: &str,
    params: &Value,
    turn_id: &str,
    commands: &mut HashMap<String, CommandItem>,
) -> Result<Option<CodexEvent>, String> {
    if params.get("turnId").and_then(Value::as_str) != Some(turn_id) {
        return Ok(None);
    }
    match method {
        "item/started" => {
            let Some(item) = params.get("item") else {
                return Err(command_protocol_error("started item is missing"));
            };
            if item.get("type").and_then(Value::as_str) != Some("commandExecution") {
                return Ok(None);
            }
            if params.get("completedAtMs").is_some() {
                return Err(command_protocol_error(
                    "started notification carries a completion timestamp",
                ));
            }
            let started_at_ms = required_command_timestamp(params, "startedAtMs")?;
            let status = item
                .get("status")
                .and_then(CommandStatus::parse)
                .ok_or_else(|| command_protocol_error("started item status is invalid"))?;
            if status != CommandStatus::InProgress {
                return Err(command_protocol_error(
                    "started item status is not in progress",
                ));
            }
            let command = command_item(item, None, started_at_ms, None)
                .ok_or_else(|| command_protocol_error("started item is invalid"))?;
            if !valid_provider_identifier(&command.item_id) {
                return Err(command_protocol_error("started item identity is invalid"));
            }
            if commands.contains_key(&command.item_id) {
                return Err(command_protocol_error(
                    "command item started more than once",
                ));
            }
            commands.insert(command.item_id.clone(), command.clone());
            Ok(Some(CodexEvent::CommandUpdated {
                turn_id: turn_id.into(),
                command: Box::new(command),
                lifecycle: "started".into(),
            }))
        }
        "item/commandExecution/outputDelta" => {
            let item_id = params
                .get("itemId")
                .and_then(Value::as_str)
                .ok_or_else(|| command_protocol_error("output delta item identity is missing"))?;
            if !valid_provider_identifier(item_id) {
                return Err(command_protocol_error(
                    "output delta item identity is invalid",
                ));
            }
            let delta = params
                .get("delta")
                .and_then(Value::as_str)
                .ok_or_else(|| command_protocol_error("output delta is missing"))?;
            let command = commands
                .get_mut(item_id)
                .ok_or_else(|| command_protocol_error("output delta has no started command"))?;
            if command.status != CommandStatus::InProgress || command.completed_at_ms.is_some() {
                return Err(command_protocol_error(
                    "output delta arrived after command completion",
                ));
            }
            append_bounded_output(command, delta);
            Ok(Some(CodexEvent::CommandUpdated {
                turn_id: turn_id.into(),
                command: Box::new(command.clone()),
                lifecycle: "delta".into(),
            }))
        }
        "item/completed" => {
            let Some(item) = params.get("item") else {
                return Err(command_protocol_error("completed item is missing"));
            };
            if item.get("type").and_then(Value::as_str) != Some("commandExecution") {
                return Ok(None);
            }
            let item_id = item
                .get("id")
                .and_then(Value::as_str)
                .ok_or_else(|| command_protocol_error("completed item identity is missing"))?;
            if !valid_provider_identifier(item_id) {
                return Err(command_protocol_error("completed item identity is invalid"));
            }
            let previous = commands
                .get(item_id)
                .ok_or_else(|| command_protocol_error("completed item has no started command"))?;
            if previous.status != CommandStatus::InProgress || previous.completed_at_ms.is_some() {
                return Err(command_protocol_error(
                    "command item completed more than once",
                ));
            }
            let completed_at_ms = required_command_timestamp(params, "completedAtMs")?;
            if completed_at_ms < previous.started_at_ms {
                return Err(command_protocol_error(
                    "command completion timestamp precedes its start",
                ));
            }
            let status = item
                .get("status")
                .and_then(CommandStatus::parse)
                .ok_or_else(|| command_protocol_error("completed item status is invalid"))?;
            if !status.is_terminal() {
                return Err(command_protocol_error(
                    "completed item does not carry a terminal status",
                ));
            }
            let mut command = command_item(
                item,
                Some(previous),
                previous.started_at_ms,
                Some(completed_at_ms),
            )
            .ok_or_else(|| command_protocol_error("completed item is invalid"))?;
            if command.source != previous.source {
                return Err(command_protocol_error(
                    "command source changed during its lifecycle",
                ));
            }
            if command.provider_input_fingerprint != previous.provider_input_fingerprint
                || command.command != previous.command
                || command.command_actions != previous.command_actions
                || command.cwd != previous.cwd
            {
                return Err(command_protocol_error(
                    "command input changed during its lifecycle",
                ));
            }
            finish_bounded_output(&mut command);
            commands.insert(command.item_id.clone(), command.clone());
            Ok(Some(CodexEvent::CommandUpdated {
                turn_id: turn_id.into(),
                command: Box::new(command),
                lifecycle: "completed".into(),
            }))
        }
        _ => Ok(None),
    }
}

fn required_command_timestamp(params: &Value, field: &str) -> Result<u64, String> {
    let value = params
        .get(field)
        .and_then(Value::as_i64)
        .and_then(|value| u64::try_from(value).ok())
        .ok_or_else(|| command_protocol_error("lifecycle timestamp is invalid"))?;
    Ok(value)
}

fn command_protocol_error(reason: &str) -> String {
    format!("Codex command lifecycle protocol error: {reason}")
}

fn translate_turn_notification(method: &str, params: &Value, turn_id: &str) -> Option<CodexEvent> {
    if params.get("turnId").and_then(Value::as_str) != Some(turn_id) {
        return None;
    }
    match method {
        "thread/tokenUsage/updated" => {
            let usage = bounded_token_usage(params.get("tokenUsage")?)?;
            Some(CodexEvent::TokenUsage {
                turn_id: turn_id.into(),
                usage,
            })
        }
        "turn/diff/updated" => Some(CodexEvent::TurnDiff {
            turn_id: turn_id.into(),
            diff: bounded_string(
                &redact_complete(params.get("diff").and_then(Value::as_str).unwrap_or("")),
                64 * 1024,
            ),
        }),
        "turn/plan/updated" => {
            let steps = params
                .get("plan")
                .and_then(Value::as_array)?
                .iter()
                .take(64)
                .filter_map(|step| {
                    Some(json!({
                        "status": bounded_string(step.get("status")?.as_str()?, 32),
                        "step": bounded_string(
                            &redact_complete(step.get("step")?.as_str()?),
                            2 * 1024
                        )
                    }))
                })
                .collect::<Vec<_>>();
            Some(CodexEvent::TurnPlan {
                turn_id: turn_id.into(),
                explanation: params
                    .get("explanation")
                    .and_then(Value::as_str)
                    .map(|value| bounded_string(&redact_complete(value), 4 * 1024)),
                steps,
            })
        }
        _ => None,
    }
}

fn bounded_token_usage(value: &Value) -> Option<Value> {
    let breakdown = |value: &Value| {
        Some(json!({
            "cached_input_tokens": value.get("cachedInputTokens")?.as_i64()?.max(0),
            "input_tokens": value.get("inputTokens")?.as_i64()?.max(0),
            "output_tokens": value.get("outputTokens")?.as_i64()?.max(0),
            "reasoning_output_tokens": value.get("reasoningOutputTokens")?.as_i64()?.max(0),
            "total_tokens": value.get("totalTokens")?.as_i64()?.max(0)
        }))
    };
    Some(json!({
        "last": breakdown(value.get("last")?)?,
        "total": breakdown(value.get("total")?)?,
        "model_context_window": value
            .get("modelContextWindow")
            .and_then(Value::as_i64)
            .map(|value| value.max(0))
    }))
}

fn command_item(
    item: &Value,
    previous: Option<&CommandItem>,
    started_at_ms: u64,
    completed_at_ms: Option<u64>,
) -> Option<CommandItem> {
    if item.get("type").and_then(Value::as_str) != Some("commandExecution") {
        return None;
    }
    let item_id = item.get("id").and_then(Value::as_str)?;
    let command = item.get("command").and_then(Value::as_str)?;
    let command_actions = item.get("commandActions")?;
    command_actions.as_array()?;
    let cwd = item.get("cwd").and_then(Value::as_str)?;
    let status = CommandStatus::parse(item.get("status")?)?;
    let source = CommandSource::parse_optional(item.get("source"))?;
    if completed_at_ms.is_some() != status.is_terminal() {
        return None;
    }
    let duration_ms = match item.get("durationMs") {
        None | Some(Value::Null) => None,
        Some(value) => Some(u64::try_from(value.as_i64()?).ok()?),
    };
    let exit_code = match item.get("exitCode") {
        None | Some(Value::Null) => None,
        Some(value) => Some(i64::from(i32::try_from(value.as_i64()?).ok()?)),
    };
    let process_id = match item.get("processId") {
        None | Some(Value::Null) => None,
        Some(value) => Some(bounded_string(value.as_str()?, 512)),
    };
    if !command_terminal_metadata_is_valid(status, duration_ms, exit_code) {
        return None;
    }
    if let (Some(completed_at_ms), Some(duration_ms)) = (completed_at_ms, duration_ms) {
        if duration_ms > completed_at_ms.checked_sub(started_at_ms)? {
            return None;
        }
    }
    let provider_input_fingerprint = ProviderCommandInputFingerprint(
        raw_command_input_fingerprint(command, command_actions, cwd)?,
    );
    let trace_input_digest = trace_command_input_fingerprint(command, command_actions, cwd)?;
    let trace_input_identity = format!(
        "sha256:{}",
        trace_input_digest
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect::<String>()
    );
    let mut result = CommandItem {
        item_id: bounded_string(item_id, 512),
        command: bounded_string(&redact_complete(command), 32 * 1024),
        command_actions: bounded_command_actions(item.get("commandActions")),
        cwd: bounded_string(cwd, 4 * 1024),
        status,
        output: CommandOutputCapture::default(),
        redactor: OutputRedactor::default(),
        duration_ms,
        exit_code,
        process_id,
        source,
        started_at_ms,
        completed_at_ms,
        provider_input_fingerprint,
        trace_input_identity,
    };
    if let Some(output) = item.get("aggregatedOutput").and_then(Value::as_str) {
        append_bounded_output(&mut result, output);
    } else if let Some(previous) = previous {
        result.output = previous.output.clone();
        result.redactor = previous.redactor.clone();
    }
    Some(result)
}

fn raw_command_input_fingerprint(
    command: &str,
    command_actions: &Value,
    cwd: &str,
) -> Option<[u8; 32]> {
    let actions = serde_json::to_vec(command_actions).ok()?;

    let mut digest = Sha256::new();
    digest.update(b"aegisy-codex-command-raw-input/0.1\0");
    update_digest_component(&mut digest, command.as_bytes());
    update_digest_component(&mut digest, &actions);
    update_digest_component(&mut digest, cwd.as_bytes());
    Some(digest.finalize().into())
}

fn trace_command_input_fingerprint(
    command: &str,
    command_actions: &Value,
    cwd: &str,
) -> Option<[u8; 32]> {
    let projection = trace_command_actions_projection(command_actions)?;
    let actions = serde_json::to_vec(&projection).ok()?;
    let mut digest = Sha256::new();
    digest.update(b"aegisy-codex-command-trace-input/0.2\0");
    update_digest_component(&mut digest, redact_complete(command).as_bytes());
    update_digest_component(&mut digest, &actions);
    update_digest_component(&mut digest, redact_complete(cwd).as_bytes());
    Some(digest.finalize().into())
}

fn trace_command_actions_projection(value: &Value) -> Option<Value> {
    let actions = value.as_array()?;
    actions
        .iter()
        .map(trace_command_action_projection)
        .collect::<Option<Vec<_>>>()
        .map(Value::Array)
}

fn trace_command_action_projection(value: &Value) -> Option<Value> {
    let action = value.as_object()?;
    let action_type = action.get("type")?.as_str()?;
    let command = redact_complete(action.get("command")?.as_str()?);
    let mut projection = serde_json::Map::new();
    projection.insert("command".into(), Value::String(command));
    projection.insert("type".into(), Value::String(action_type.into()));
    match action_type {
        "read" => {
            projection.insert(
                "name".into(),
                Value::String(redact_complete(action.get("name")?.as_str()?)),
            );
            projection.insert(
                "path".into(),
                Value::String(redact_complete(action.get("path")?.as_str()?)),
            );
        }
        "listFiles" => {
            insert_optional_redacted_string(&mut projection, action, "path")?;
        }
        "search" => {
            insert_optional_redacted_string(&mut projection, action, "path")?;
            insert_optional_redacted_string(&mut projection, action, "query")?;
        }
        "unknown" => {}
        _ => return None,
    }
    Some(Value::Object(projection))
}

fn insert_optional_redacted_string(
    projection: &mut serde_json::Map<String, Value>,
    action: &serde_json::Map<String, Value>,
    field: &str,
) -> Option<()> {
    match action.get(field) {
        None | Some(Value::Null) => Some(()),
        Some(Value::String(value)) => {
            projection.insert(field.into(), Value::String(redact_complete(value)));
            Some(())
        }
        Some(_) => None,
    }
}

fn update_digest_component(digest: &mut Sha256, value: &[u8]) {
    digest.update((value.len() as u64).to_be_bytes());
    digest.update(value);
}

fn command_terminal_metadata_is_valid(
    status: CommandStatus,
    duration_ms: Option<u64>,
    exit_code: Option<i64>,
) -> bool {
    match status {
        CommandStatus::InProgress => duration_ms.is_none() && exit_code.is_none(),
        CommandStatus::Completed => exit_code.is_none_or(|code| code == 0),
        CommandStatus::Failed => exit_code != Some(0),
        CommandStatus::Declined => duration_ms.is_none() && exit_code.is_none(),
    }
}

pub(crate) fn append_bounded_output(command: &mut CommandItem, delta: &str) {
    let redacted = command.redactor.push(delta);
    command.output.append(&redacted);
}

pub(crate) fn finish_bounded_output(command: &mut CommandItem) {
    let redacted = command.redactor.finish();
    command.output.append(&redacted);
}

fn codex_message_channel() -> (SyncSender<Value>, Receiver<Value>) {
    mpsc::sync_channel(CODEX_MESSAGE_QUEUE_CAPACITY)
}

fn transport_error(message: impl Into<String>) -> Value {
    json!({
        "method": "aegisy/transportError",
        "params": { "message": message.into() }
    })
}

#[derive(Debug, PartialEq, Eq)]
enum BoundedLine {
    Line(Vec<u8>),
    Oversized,
    Eof,
}

fn read_bounded_line<R: BufRead>(reader: &mut R, limit: usize) -> io::Result<BoundedLine> {
    let mut line = Vec::with_capacity(limit.min(64 * 1024));
    let mut oversized = false;
    loop {
        let available = reader.fill_buf()?;
        if available.is_empty() {
            return if line.is_empty() && !oversized {
                Ok(BoundedLine::Eof)
            } else if oversized {
                Ok(BoundedLine::Oversized)
            } else {
                Ok(BoundedLine::Line(line))
            };
        }
        let consumed = available
            .iter()
            .position(|byte| *byte == b'\n')
            .map(|position| position + 1)
            .unwrap_or(available.len());
        let completed = available[..consumed].ends_with(b"\n");
        let payload_bytes = consumed.saturating_sub(usize::from(completed));
        if !oversized {
            let remaining = limit.saturating_sub(line.len());
            line.extend_from_slice(&available[..payload_bytes.min(remaining)]);
            oversized = payload_bytes > remaining;
        }
        reader.consume(consumed);
        if completed {
            if oversized {
                return Ok(BoundedLine::Oversized);
            }
            if line.ends_with(b"\r") {
                line.pop();
            }
            return Ok(BoundedLine::Line(line));
        }
    }
}

fn bounded_string(value: &str, byte_limit: usize) -> String {
    if value.len() <= byte_limit {
        return value.to_owned();
    }
    let mut end = byte_limit;
    while end > 0 && !value.is_char_boundary(end) {
        end -= 1;
    }
    value[..end].to_owned()
}

fn bounded_command_actions(actions: Option<&Value>) -> Value {
    let values = actions
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .take(32)
        .map(|action| {
            let mut bounded = serde_json::Map::new();
            for key in ["type", "command", "name", "path", "query"] {
                if let Some(value) = action.get(key).and_then(Value::as_str) {
                    bounded.insert(
                        key.into(),
                        Value::String(bounded_string(&redact_complete(value), 4 * 1024)),
                    );
                }
            }
            Value::Object(bounded)
        })
        .collect();
    Value::Array(values)
}

impl Drop for CodexAdapter {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn locate_codex() -> PathBuf {
    if let Some(path) = env::var_os("AEGISY_CODEX_PATH") {
        return PathBuf::from(path);
    }
    for path in ["/opt/homebrew/bin/codex", "/usr/local/bin/codex"] {
        if Path::new(path).is_file() {
            return PathBuf::from(path);
        }
    }
    PathBuf::from(if cfg!(windows) { "codex.exe" } else { "codex" })
}

fn codex_process_environment() -> Result<ProcessEnvironment, String> {
    let isolated_root = std::env::temp_dir().join("aegisy-codex-adapter");
    SessionEnvironment::build("codex-adapter", None, "read-only", &isolated_root)
        .for_tool("codex-app-server", Vec::new())
        .map_err(|error| {
            format!(
                "cannot construct Codex process environment: {}",
                error.message
            )
        })
}

fn codex_command(executable: &Path, environment: &ProcessEnvironment) -> Command {
    let mut command = Command::new(executable);
    command.env_clear();
    for (name, value) in environment.iter() {
        command.env(name, value);
    }
    command
}

fn codex_version(executable: &Path, environment: &ProcessEnvironment) -> String {
    let mut command = codex_command(executable, environment);
    command
        .arg("--version")
        .output()
        .ok()
        .filter(|output| output.status.success())
        .map(|output| String::from_utf8_lossy(&output.stdout).trim().to_owned())
        .filter(|version| !version.is_empty())
        .unwrap_or_else(|| "unknown".into())
}

fn codex_version_is_pinned(version: &str) -> bool {
    version.trim() == PINNED_CODEX_VERSION
}

fn classify_stderr(value: &str) -> &'static str {
    let lower = value.to_ascii_lowercase();
    if lower.contains("panic") || lower.contains("fatal") {
        "fatal"
    } else if lower.contains("timeout") || lower.contains("timed out") {
        "timeout"
    } else if lower.contains("error") || lower.contains("failed") {
        "error"
    } else if lower.contains("warn") {
        "warning"
    } else {
        "info"
    }
}

fn is_retryable_startup_error(value: &str) -> bool {
    let lower = value.to_ascii_lowercase();
    [
        "timed out",
        "closed its output channel",
        "transport",
        "cannot read codex app server output",
        "cannot write to codex app server",
    ]
    .iter()
    .any(|needle| lower.contains(needle))
}

fn thread_start_params(cwd: &Path, chat: bool) -> Value {
    let instructions = if chat {
        "You are running inside Aegisy Coding Chat. This mode is non-mutating. Do not modify files, execute mutating commands, or inspect paths outside explicitly provided context."
    } else {
        "You are running inside the Aegisy Coding Work preview. Inspect the bound project in read-only mode. Do not modify files or execute mutating commands. Explain proposed changes instead."
    };
    json!({
        "cwd": cwd,
        "sandbox": "read-only",
        "approvalPolicy": "never",
        "ephemeral": false,
        "developerInstructions": instructions,
        "threadSource": "aegisy-coding"
    })
}

fn thread_resume_params(thread_id: &str, cwd: &Path, chat: bool) -> Value {
    let mut params = thread_start_params(cwd, chat);
    let object = params.as_object_mut().expect("thread start params object");
    object.remove("ephemeral");
    object.insert("threadId".into(), thread_id.into());
    params
}

fn thread_fork_params(
    thread_id: &str,
    last_turn_id: Option<&str>,
    cwd: &Path,
    chat: bool,
) -> Value {
    let mut params = thread_resume_params(thread_id, cwd, chat);
    let object = params.as_object_mut().expect("thread fork params object");
    object.insert("ephemeral".into(), false.into());
    object.insert(
        "lastTurnId".into(),
        last_turn_id.map_or(Value::Null, |turn_id| turn_id.into()),
    );
    params
}

#[allow(dead_code)]
fn thread_list_params(
    cursor: Option<&str>,
    limit: Option<u32>,
    cwd: Option<&Path>,
    archived: Option<bool>,
) -> Value {
    let mut params = json!({});
    let object = params.as_object_mut().expect("thread list params object");
    if let Some(cursor) = cursor {
        object.insert("cursor".into(), cursor.into());
    }
    if let Some(limit) = limit {
        object.insert("limit".into(), limit.into());
    }
    if let Some(cwd) = cwd {
        object.insert("cwd".into(), cwd.to_string_lossy().into_owned().into());
    }
    if let Some(archived) = archived {
        object.insert("archived".into(), archived.into());
    }
    params
}

#[allow(dead_code)]
fn thread_read_params(thread_id: &str, include_turns: bool) -> Value {
    json!({
        "threadId": thread_id,
        "includeTurns": include_turns
    })
}

#[allow(dead_code)]
fn thread_compact_start_params(thread_id: &str) -> Value {
    json!({ "threadId": thread_id })
}

fn turn_start_params(request: &CodexTurnRequest<'_>) -> Value {
    let mut turn_input = vec![json!({ "type": "text", "text": request.input })];
    turn_input.extend(request.local_images.iter().map(|path| {
        json!({
            "type": "localImage",
            "path": path.to_string_lossy()
        })
    }));
    json!({
        "threadId": request.thread_id,
        "input": turn_input,
        "clientUserMessageId": request.idempotency_key
    })
}

fn turn_interrupt_params(thread_id: &str, turn_id: &str) -> Value {
    json!({
        "threadId": thread_id,
        "turnId": turn_id
    })
}

fn turn_steer_params(thread_id: &str, turn_id: &str, steer: &TurnSteerRequest) -> Value {
    json!({
        "threadId": thread_id,
        "expectedTurnId": turn_id,
        "input": [{ "type": "text", "text": steer.input }],
        "clientUserMessageId": steer.client_user_message_id
    })
}

fn turn_error_observation(params: &Value, turn_id: &str) -> Result<Option<CodexEvent>, String> {
    let notification_turn_id = params
        .get("turnId")
        .and_then(Value::as_str)
        .ok_or_else(|| {
            "Codex App Server protocol error: error notification is missing turnId".to_owned()
        })?;
    if notification_turn_id != turn_id {
        return Ok(None);
    }
    let will_retry = params
        .get("willRetry")
        .and_then(Value::as_bool)
        .ok_or_else(|| {
            "Codex App Server protocol error: error notification is missing willRetry".to_owned()
        })?;
    let error = params
        .get("error")
        .and_then(Value::as_object)
        .ok_or_else(|| {
            "Codex App Server protocol error: error notification is missing error metadata"
                .to_owned()
        })?;
    let message = error
        .get("message")
        .and_then(Value::as_str)
        .ok_or_else(|| {
            "Codex App Server protocol error: error notification is missing error.message"
                .to_owned()
        })?
        .to_owned();
    let provider_error = error
        .get("codexErrorInfo")
        .filter(|value| !value.is_null())
        .and_then(from_codex_error_info);
    Ok(Some(CodexEvent::TurnErrorObserved {
        turn_id: turn_id.to_owned(),
        message,
        provider_error,
        will_retry,
    }))
}

fn turn_terminal_event(params: &Value, turn_id: &str) -> Result<CodexEvent, String> {
    let notification_turn_id = params
        .pointer("/turn/id")
        .and_then(Value::as_str)
        .ok_or_else(|| {
            "Codex App Server protocol error: terminal notification is missing turn.id".to_owned()
        })?;
    if notification_turn_id != turn_id {
        return Err(
            "Codex App Server protocol error: terminal notification has a different turn identity"
                .into(),
        );
    }
    match params.pointer("/turn/status").and_then(Value::as_str) {
        Some("completed") => Ok(CodexEvent::TurnCompleted {
            turn_id: turn_id.into(),
        }),
        Some("interrupted") => Ok(CodexEvent::TurnInterrupted {
            turn_id: turn_id.into(),
        }),
        Some("failed") => Ok(CodexEvent::TurnFailed {
            turn_id: turn_id.into(),
            message: params
                .pointer("/turn/error/message")
                .and_then(Value::as_str)
                .unwrap_or("Codex turn failed")
                .to_owned(),
            provider_error: params
                .pointer("/turn/error/codexErrorInfo")
                .and_then(from_codex_error_info),
        }),
        Some("inProgress") => Err(
            "Codex App Server protocol error: terminal notification is still in progress".into(),
        ),
        _ => Err(
            "Codex App Server protocol error: terminal notification has an invalid status".into(),
        ),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn file_change(
        path: &str,
        kind: crate::codex_file_change::CodexPatchChangeKind,
        diff: impl Into<String>,
    ) -> CodexFileUpdateChange {
        CodexFileUpdateChange {
            path: path.to_owned(),
            diff: diff.into(),
            kind,
        }
    }

    #[test]
    fn pinned_file_change_shapes_parse_without_relabeling_diff_semantics() {
        let changes = json!([
            {
                "path": "/workspace/added.txt",
                "kind": { "type": "add" },
                "diff": "new content\n"
            },
            {
                "path": "/workspace/updated.txt",
                "kind": { "type": "update", "move_path": null },
                "diff": "@@ -1 +1 @@\n-old\n+new\n"
            },
            {
                "path": "/workspace/deleted.txt",
                "kind": { "type": "delete" },
                "diff": "old content\n"
            }
        ]);
        let parsed = parse_file_update_changes(Some(&changes), false).unwrap();
        assert_eq!(parsed.len(), 3);
        assert_eq!(parsed[0].diff, "new content\n");
        assert!(matches!(
            &parsed[0].kind,
            crate::codex_file_change::CodexPatchChangeKind::Add
        ));
        assert!(matches!(
            &parsed[1].kind,
            crate::codex_file_change::CodexPatchChangeKind::Update { move_path: None }
        ));
        assert!(matches!(
            &parsed[2].kind,
            crate::codex_file_change::CodexPatchChangeKind::Delete
        ));
    }

    #[test]
    fn patch_updates_may_be_empty_but_final_file_changes_may_not() {
        let empty = json!([]);
        assert!(parse_file_update_changes(Some(&empty), true)
            .unwrap()
            .is_empty());
        assert!(parse_file_update_changes(Some(&empty), false).is_err());
        let unknown = json!([{
            "path": "file.txt",
            "kind": { "type": "add", "unexpected": true },
            "diff": "content\n"
        }]);
        assert!(parse_file_update_changes(Some(&unknown), false).is_err());
    }

    #[test]
    fn volatile_patch_formats_do_not_override_authoritative_started_changes() {
        use crate::codex_file_change::CodexPatchChangeKind;

        let mut state = CodexFileChangeTurnState::default();
        let cases = [
            (
                "update-item",
                file_change(
                    "/workspace/update.txt",
                    CodexPatchChangeKind::Update { move_path: None },
                    "@@\n-old\n+new\n",
                ),
                file_change(
                    "/workspace/update.txt",
                    CodexPatchChangeKind::Update { move_path: None },
                    "--- a/update.txt\n+++ b/update.txt\n@@ -1 +1 @@\n-old\n+new\n",
                ),
            ),
            (
                "delete-item",
                file_change("/workspace/delete.txt", CodexPatchChangeKind::Delete, ""),
                file_change(
                    "/workspace/delete.txt",
                    CodexPatchChangeKind::Delete,
                    "old content\n",
                ),
            ),
            (
                "partial-add-item",
                file_change("/workspace/add.txt", CodexPatchChangeKind::Add, "partial"),
                file_change(
                    "/workspace/add.txt",
                    CodexPatchChangeKind::Add,
                    "partial\ncomplete\n",
                ),
            ),
            (
                "relative-add-item",
                file_change("relative.txt", CodexPatchChangeKind::Add, "content\n"),
                file_change(
                    "/workspace/relative.txt",
                    CodexPatchChangeKind::Add,
                    "content\n",
                ),
            ),
            (
                "rename-item",
                file_change(
                    "old.txt",
                    CodexPatchChangeKind::Update {
                        move_path: Some("new.txt".into()),
                    },
                    "",
                ),
                file_change(
                    "/workspace/old.txt",
                    CodexPatchChangeKind::Update {
                        move_path: Some("/workspace/new.txt".into()),
                    },
                    "\n\nMoved to: /workspace/new.txt",
                ),
            ),
        ];

        for (item_id, patch_updated, started) in cases {
            state
                .observe_patch_updated(item_id, vec![patch_updated])
                .unwrap();
            state
                .start_item(item_id, vec![started.clone()], 10)
                .unwrap();
            assert_eq!(state.items.get(item_id).unwrap().changes, vec![started]);
            assert!(!state.pending_patches.contains_key(item_id));
        }
    }

    #[test]
    fn volatile_patch_progress_cannot_cross_item_lifecycles() {
        use crate::codex_file_change::CodexPatchChangeKind;

        let mut state = CodexFileChangeTurnState::default();
        state
            .observe_patch_updated(
                "item-a",
                vec![file_change("a.txt", CodexPatchChangeKind::Add, "a")],
            )
            .unwrap();
        state
            .start_item(
                "item-b",
                vec![file_change("b.txt", CodexPatchChangeKind::Add, "b")],
                10,
            )
            .unwrap();

        assert!(state.pending_patches.contains_key("item-a"));
        assert!(state.items.contains_key("item-b"));
        assert!(state
            .observe_patch_updated(
                "item-b",
                vec![file_change("b.txt", CodexPatchChangeKind::Add, "changed")],
            )
            .is_err());
        assert!(state.has_open_items());
    }

    #[test]
    fn single_turn_file_change_budget_fails_before_retaining_started_item() {
        use crate::codex_file_change::CodexPatchChangeKind;

        let mut state = CodexFileChangeTurnState::default();
        let one_mebibyte = "x".repeat(1024 * 1024);
        for index in 0..15 {
            state
                .start_item(
                    &format!("item-{index}"),
                    vec![file_change(
                        &format!("file-{index}.txt"),
                        CodexPatchChangeKind::Add,
                        one_mebibyte.clone(),
                    )],
                    10,
                )
                .unwrap();
        }
        let retained_before = state.retained_bytes;
        let error = state
            .start_item(
                "item-over-budget",
                vec![file_change(
                    "over-budget.txt",
                    CodexPatchChangeKind::Add,
                    one_mebibyte,
                )],
                10,
            )
            .unwrap_err();

        assert!(error.contains("single-turn retained file change budget is exceeded"));
        assert_eq!(state.retained_bytes, retained_before);
        assert!(!state.items.contains_key("item-over-budget"));
    }

    #[test]
    fn file_change_lifecycle_timestamps_are_required_and_monotonic() {
        use crate::codex_file_change::CodexPatchChangeKind;

        assert_eq!(
            required_file_change_timestamp(&json!({"startedAtMs": 10}), "startedAtMs").unwrap(),
            10
        );
        for invalid in [
            json!({}),
            json!({"startedAtMs": null}),
            json!({"startedAtMs": "10"}),
            json!({"startedAtMs": -1}),
            json!({"startedAtMs": 1.5}),
        ] {
            assert!(required_file_change_timestamp(&invalid, "startedAtMs").is_err());
        }

        let mut state = CodexFileChangeTurnState::default();
        let change = file_change("file.txt", CodexPatchChangeKind::Add, "content\n");
        state.start_item("item", vec![change.clone()], 20).unwrap();
        assert!(state
            .complete_item("item", vec![change.clone()], Some("failed"), 19)
            .is_err());
        assert!(!state.items.get("item").unwrap().completed);
        state
            .complete_item("item", vec![change], Some("failed"), 20)
            .unwrap();
    }

    #[test]
    fn agent_message_limit_counts_unicode_characters_not_utf8_bytes() {
        let boundary = json!("界".repeat(MAX_AGENT_MESSAGE_CHARACTERS));
        assert!(bounded_agent_message_text(Some(&boundary)).is_ok());
        assert_eq!(
            checked_agent_message_character_count(MAX_AGENT_MESSAGE_CHARACTERS - 1, "界").unwrap(),
            MAX_AGENT_MESSAGE_CHARACTERS
        );

        let over_limit = json!("界".repeat(MAX_AGENT_MESSAGE_CHARACTERS + 1));
        assert!(bounded_agent_message_text(Some(&over_limit)).is_err());
        assert!(checked_agent_message_character_count(MAX_AGENT_MESSAGE_CHARACTERS, "界").is_err());
    }

    #[test]
    fn backend_info_uses_read_only_profile() {
        let info = BackendInfo {
            adapter: "codex-app-server".into(),
            version: "codex-cli test".into(),
            provider: Some("aegisy".into()),
            model: Some("test-model".into()),
            permission_profile: "read-only".into(),
            environment: None,
        };
        assert_eq!(info.permission_profile, "read-only");
        assert_eq!(serde_json::to_value(info).unwrap()["provider"], "aegisy");
    }

    #[test]
    fn codex_adapter_rejects_unpinned_versions() {
        assert!(codex_version_is_pinned(PINNED_CODEX_VERSION));
        assert!(!codex_version_is_pinned("codex-cli 0.144.4"));
        assert!(!codex_version_is_pinned("unknown"));
    }

    #[test]
    fn turn_terminal_event_preserves_provider_error_classification_only() {
        let event = turn_terminal_event(
            &json!({
                "turn": {
                    "id": "turn-provider-error",
                    "status": "failed",
                    "error": {
                        "message": "stream disconnected before completion: private body",
                        "codexErrorInfo": {
                            "responseStreamDisconnected": { "httpStatusCode": 502 }
                        }
                    }
                }
            }),
            "turn-provider-error",
        )
        .unwrap();
        let CodexEvent::TurnFailed {
            message,
            provider_error,
            ..
        } = event
        else {
            panic!("expected failed turn event");
        };
        assert!(message.contains("stream disconnected"));
        let provider_error = provider_error.expect("provider metadata");
        assert_eq!(provider_error.kind, "response-stream-disconnected");
        assert_eq!(provider_error.http_status, Some(502));
        assert!(provider_error.retryable);
        assert!(!serde_json::to_string(&provider_error)
            .unwrap()
            .contains("private body"));
    }

    #[test]
    fn startup_supervision_retries_only_transient_transport_failures() {
        assert!(is_retryable_startup_error("Codex App Server timed out"));
        assert!(is_retryable_startup_error(
            "Codex App Server closed its output channel"
        ));
        assert!(is_retryable_startup_error(
            "cannot write to Codex App Server"
        ));
        assert!(!is_retryable_startup_error(
            "unsupported Codex App Server version codex-cli 0.144.4"
        ));
        assert!(!is_retryable_startup_error(
            "Codex rejected initialize because the request is invalid"
        ));
        assert_eq!(STARTUP_MAX_ATTEMPTS, 3);
        assert_eq!(STARTUP_TIMEOUT, Duration::from_secs(15));
    }

    #[test]
    fn unknown_notification_diagnostics_are_bounded_hashed_and_content_free() {
        let private_method = "future/private-notification-sk-secret";
        let mut diagnostics = UnknownNotificationDiagnostics::default();
        diagnostics.observe(private_method);
        diagnostics.observe(private_method);
        for index in 1..20 {
            diagnostics.observe(&format!("future/private-notification-{index}"));
        }

        assert_eq!(diagnostics.total_count, 21);
        assert_eq!(diagnostics.methods.len(), MAX_UNKNOWN_NOTIFICATION_METHODS);
        assert_eq!(diagnostics.unretained_count, 4);
        assert_eq!(diagnostics.methods[0].count, 2);
        assert_eq!(
            diagnostics.methods[0].method_sha256,
            format!("sha256:{:x}", Sha256::digest(private_method.as_bytes()))
        );
        let encoded = serde_json::to_string(&diagnostics).unwrap();
        assert!(!encoded.contains(private_method));
        assert!(!encoded.contains("future/private-notification"));
        assert!(!encoded.contains("sk-secret"));
        assert!(!encoded.contains("params"));
        assert!(!encoded.contains("body"));
    }

    #[test]
    fn known_notification_methods_do_not_enter_unknown_diagnostics() {
        assert!(crate::codex_capability_matrix::is_known_server_notification("turn/completed"));
        assert!(
            !crate::codex_capability_matrix::is_known_server_notification(
                "future/private-notification"
            )
        );
    }

    #[test]
    fn user_input_server_request_returns_only_a_content_free_unsupported_error() {
        let secret = "sk-request-user-input-secret";
        let request = json!({
            "id": "user-input-1",
            "method": "item/tool/requestUserInput",
            "params": {
                "questions": [{
                    "header": "Private question",
                    "question": format!("Reveal {secret}"),
                    "options": [{"label": "Secret answer", "description": secret}]
                }]
            }
        });

        let UserInputRequestDisposition::Continue(response) =
            user_input_request_disposition(&request)
        else {
            panic!("valid request ID must receive a correlated unsupported error");
        };
        assert_eq!(response["id"], "user-input-1");
        assert_eq!(response["error"]["code"], -32601);
        assert_eq!(
            response["error"]["message"],
            "user input request is unsupported"
        );
        assert!(response.get("result").is_none());
        let encoded = serde_json::to_string(&response).unwrap();
        for forbidden in [
            "answers",
            "params",
            "questions",
            "Private question",
            "Secret answer",
            secret,
        ] {
            assert!(
                !encoded.contains(forbidden),
                "unsupported response leaked {forbidden}"
            );
        }

        assert!(matches!(
            user_input_request_disposition(&json!({
                "id": 7,
                "method": "item/tool/requestUserInput",
                "params": {"question": secret}
            })),
            UserInputRequestDisposition::Continue(response)
                if response["id"] == 7 && response.get("result").is_none()
        ));
    }

    #[test]
    fn invalid_user_input_request_ids_fail_closed_without_echoing_the_body() {
        let secret = "ghp_request_user_input_secret_value";
        let requests = [
            json!({
                "method": "item/tool/requestUserInput",
                "params": {"question": secret}
            }),
            json!({
                "id": {"untrusted": secret},
                "method": "item/tool/requestUserInput",
                "params": {"question": secret}
            }),
            json!({
                "id": "",
                "method": "item/tool/requestUserInput",
                "params": {"question": secret}
            }),
            json!({
                "id": "x".repeat(MAX_SERVER_REQUEST_ID_BYTES + 1),
                "method": "item/tool/requestUserInput",
                "params": {"question": secret}
            }),
            json!({
                "id": "invalid\nrequest",
                "method": "item/tool/requestUserInput",
                "params": {"question": secret}
            }),
        ];

        for request in requests {
            let UserInputRequestDisposition::FailClosed(response) =
                user_input_request_disposition(&request)
            else {
                panic!("invalid request ID must fail closed");
            };
            assert!(response["id"].is_null());
            assert_eq!(response["error"]["code"], -32602);
            assert_eq!(
                response["error"]["message"],
                "user input request is invalid"
            );
            assert!(response.get("result").is_none());
            let encoded = serde_json::to_string(&response).unwrap();
            assert!(!encoded.contains(secret));
            assert!(!encoded.contains("question"));
            assert!(!encoded.contains("answers"));
        }
    }

    #[test]
    fn stderr_diagnostics_are_bounded_redacted_and_content_free() {
        let secret = b"warning: Authorization: Bearer ghp_123456789012345678901234567890\n";
        let mut diagnostics = StderrDiagnostics::default();
        diagnostics.observe(secret);
        assert_eq!(diagnostics.bytes, secret.len() as u64);
        assert_eq!(diagnostics.lines, 1);
        assert_eq!(diagnostics.redacted_lines, 1);
        assert_eq!(diagnostics.last_class.as_deref(), Some("warning"));
        let encoded = serde_json::to_string(&diagnostics).unwrap();
        assert!(!encoded.contains("ghp_"));

        diagnostics.observe(b"fatal: Codex process panic\n");
        assert_eq!(diagnostics.last_class.as_deref(), Some("fatal"));
        assert_eq!(diagnostics.lines, 2);
    }

    #[test]
    fn agent_delta_fixture_accumulates_in_wire_order() {
        let fixture = ["AE", "GIS", "Y_CODEX_OK"];
        let mut accumulated = String::new();
        for delta in fixture {
            accumulated.push_str(delta);
        }
        assert_eq!(accumulated, "AEGISY_CODEX_OK");
    }

    #[test]
    fn thread_start_is_always_read_only_without_approval_escalation() {
        let params = thread_start_params(Path::new("/tmp/project"), false);
        assert_eq!(params["cwd"], "/tmp/project");
        assert_eq!(params["sandbox"], "read-only");
        assert_eq!(params["approvalPolicy"], "never");
        assert_eq!(params["ephemeral"], false);
        assert!(params["developerInstructions"]
            .as_str()
            .unwrap()
            .contains("Do not modify files"));
    }

    #[test]
    fn thread_resume_and_fork_match_generated_pinned_schema() {
        let resume = thread_resume_params("thread-1", Path::new("/tmp/project"), false);
        assert_eq!(resume["threadId"], "thread-1");
        assert_eq!(resume["cwd"], "/tmp/project");
        assert_eq!(resume["sandbox"], "read-only");
        assert_eq!(resume["approvalPolicy"], "never");
        assert!(resume.get("ephemeral").is_none());
        let fork = thread_fork_params("thread-1", Some("turn-2"), Path::new("/tmp/project"), false);
        assert_eq!(fork["threadId"], "thread-1");
        assert_eq!(fork["lastTurnId"], "turn-2");
        assert_eq!(fork["ephemeral"], false);
        assert_eq!(
            fork["developerInstructions"],
            resume["developerInstructions"]
        );
    }

    #[test]
    fn thread_lifecycle_params_match_generated_pinned_schema() {
        let list = thread_list_params(
            Some("cursor-1"),
            Some(50),
            Some(Path::new("/tmp/project")),
            Some(false),
        );
        assert_eq!(
            list,
            json!({
                "cursor": "cursor-1",
                "limit": 50,
                "cwd": "/tmp/project",
                "archived": false
            })
        );
        assert_eq!(
            thread_read_params("thread-1", true),
            json!({ "threadId": "thread-1", "includeTurns": true })
        );
        assert_eq!(
            thread_compact_start_params("thread-1"),
            json!({ "threadId": "thread-1" })
        );
    }

    #[test]
    fn thread_lifecycle_session_parser_accepts_thread_scoped_metadata_without_new_policy_binding() {
        let session = parse_codex_session(
            json!({
                "thread": {
                    "id": "thread-1",
                    "modelProvider": "aegisy",
                    "model": "model-1"
                }
            }),
            "thread/unarchive",
            false,
        )
        .unwrap();
        assert_eq!(session.thread_id, "thread-1");
        assert_eq!(session.provider, "aegisy");
        assert_eq!(session.model, "model-1");
        assert_eq!(session.approval_policy, None);
    }

    #[test]
    fn session_parser_binds_closed_effective_read_only_policy() {
        for (reviewer, expected) in [
            ("user", ApprovalPolicyReviewer::User),
            ("auto_review", ApprovalPolicyReviewer::AutoReview),
            (
                "guardian_subagent",
                ApprovalPolicyReviewer::GuardianSubagent,
            ),
        ] {
            let session = parse_codex_session(
                json!({
                    "thread": { "id": "thread-sensitive-1" },
                    "modelProvider": "aegisy",
                    "model": "model-1",
                    "approvalPolicy": "never",
                    "approvalsReviewer": reviewer,
                    "sandbox": {
                        "type": "readOnly",
                        "networkAccess": false
                    }
                }),
                "thread/start",
                true,
            )
            .unwrap();
            let binding = session.approval_policy.unwrap();
            assert_eq!(binding.adapter_identity, CODEX_ADAPTER_IDENTITY);
            assert_eq!(binding.runtime_version, CODEX_TRACE_RUNTIME_VERSION);
            assert_eq!(binding.adapter_version, PINNED_CODEX_VERSION);
            assert_eq!(binding.policy, RuntimeApprovalPolicy::Never);
            assert_eq!(binding.reviewer, expected);
            assert_eq!(binding.sandbox, ApprovalPolicySandbox::ReadOnly);
            assert_eq!(
                binding.permission_profile,
                ApprovalPolicyPermissionProfile::ReadOnly
            );
            for identity in [
                &binding.provider_thread_identity,
                &binding.configured_policy_identity,
                &binding.effective_policy_identity,
            ] {
                assert!(identity.starts_with("sha256:"));
                assert_eq!(identity.len(), 71);
                assert!(!identity.contains("thread-sensitive-1"));
                assert!(!identity.contains("never"));
                assert!(!identity.contains("readOnly"));
            }
            assert_ne!(
                binding.provider_thread_identity,
                binding.configured_policy_identity
            );
            assert_ne!(
                binding.configured_policy_identity,
                binding.effective_policy_identity
            );
        }
    }

    #[test]
    fn session_parser_rejects_unclosed_or_weakened_policy_response() {
        let valid = json!({
            "thread": { "id": "thread-1" },
            "modelProvider": "aegisy",
            "model": "model-1",
            "approvalPolicy": "never",
            "approvalsReviewer": "user",
            "sandbox": {
                "type": "readOnly",
                "networkAccess": false
            }
        });
        let invalid = [
            {
                let mut value = valid.clone();
                value.as_object_mut().unwrap().remove("approvalPolicy");
                value
            },
            {
                let mut value = valid.clone();
                value["approvalPolicy"] = json!("on-request");
                value
            },
            {
                let mut value = valid.clone();
                value["approvalPolicy"] = json!({
                    "granular": {
                        "mcp_elicitations": false,
                        "rules": false,
                        "sandbox_approval": false
                    }
                });
                value
            },
            {
                let mut value = valid.clone();
                value.as_object_mut().unwrap().remove("approvalsReviewer");
                value
            },
            {
                let mut value = valid.clone();
                value["approvalsReviewer"] = json!("future-reviewer");
                value
            },
            {
                let mut value = valid.clone();
                value["sandbox"] = json!("read-only");
                value
            },
            {
                let mut value = valid.clone();
                value["sandbox"]
                    .as_object_mut()
                    .unwrap()
                    .remove("networkAccess");
                value
            },
            {
                let mut value = valid.clone();
                value["sandbox"]["networkAccess"] = json!(true);
                value
            },
            {
                let mut value = valid.clone();
                value["sandbox"]["futureField"] = json!(false);
                value
            },
        ];
        for response in invalid {
            let error = parse_codex_session(response, "thread/resume", true).unwrap_err();
            assert!(error.contains("Codex thread/resume response"));
        }
    }

    #[test]
    fn approval_policy_identities_are_recomputable_and_domain_separated() {
        let first =
            codex_approval_policy_binding("thread-1", ApprovalPolicyReviewer::User).unwrap();
        let repeated =
            codex_approval_policy_binding("thread-1", ApprovalPolicyReviewer::User).unwrap();
        let other_thread =
            codex_approval_policy_binding("thread-2", ApprovalPolicyReviewer::User).unwrap();
        let other_reviewer =
            codex_approval_policy_binding("thread-1", ApprovalPolicyReviewer::AutoReview).unwrap();

        assert_eq!(first, repeated);
        assert_eq!(
            first.configured_policy_identity,
            other_thread.configured_policy_identity
        );
        assert_eq!(
            first.configured_policy_identity,
            other_reviewer.configured_policy_identity
        );
        assert_ne!(
            first.provider_thread_identity,
            other_thread.provider_thread_identity
        );
        assert_ne!(
            first.effective_policy_identity,
            other_thread.effective_policy_identity
        );
        assert_ne!(
            first.effective_policy_identity,
            other_reviewer.effective_policy_identity
        );
        assert!(codex_approval_policy_binding("", ApprovalPolicyReviewer::User).is_err());
    }

    #[test]
    fn turn_metadata_notifications_map_to_bounded_stable_events() {
        let usage = translate_turn_notification(
            "thread/tokenUsage/updated",
            &json!({
                "threadId": "thread-1",
                "turnId": "turn-1",
                "tokenUsage": {
                    "last": {
                        "cachedInputTokens": 1,
                        "inputTokens": 2,
                        "outputTokens": 3,
                        "reasoningOutputTokens": 4,
                        "totalTokens": 5
                    },
                    "total": {
                        "cachedInputTokens": 6,
                        "inputTokens": 7,
                        "outputTokens": 8,
                        "reasoningOutputTokens": 9,
                        "totalTokens": 10
                    },
                    "modelContextWindow": 100
                }
            }),
            "turn-1",
        )
        .unwrap();
        match usage {
            CodexEvent::TokenUsage { usage, .. } => {
                assert_eq!(usage["last"]["total_tokens"], 5);
                assert_eq!(usage["model_context_window"], 100);
            }
            _ => panic!("unexpected token usage event"),
        }

        let plan = translate_turn_notification(
            "turn/plan/updated",
            &json!({
                "threadId": "thread-1",
                "turnId": "turn-1",
                "explanation": "inspect then test",
                "plan": [{ "status": "inProgress", "step": "inspect" }]
            }),
            "turn-1",
        )
        .unwrap();
        match plan {
            CodexEvent::TurnPlan { steps, .. } => assert_eq!(steps[0]["step"], "inspect"),
            _ => panic!("unexpected turn plan event"),
        }

        let diff = translate_turn_notification(
            "turn/diff/updated",
            &json!({
                "threadId": "thread-1",
                "turnId": "turn-1",
                "diff": "Authorization: Bearer ghp_123456789012345678901234567890"
            }),
            "turn-1",
        )
        .unwrap();
        match diff {
            CodexEvent::TurnDiff { diff, .. } => assert!(!diff.contains("ghp_")),
            _ => panic!("unexpected turn diff event"),
        }
    }

    #[test]
    fn turn_start_preserves_client_idempotency_key() {
        let local_images = [PathBuf::from("/tmp/aegisy-image.png")];
        let params = turn_start_params(&CodexTurnRequest {
            thread_id: "thread-1",
            input: "hello",
            local_images: &local_images,
            idempotency_key: "client-turn-1",
        });
        assert_eq!(params["threadId"], "thread-1");
        assert_eq!(params["input"][0]["type"], "text");
        assert_eq!(params["input"][0]["text"], "hello");
        assert_eq!(params["input"][1]["type"], "localImage");
        assert_eq!(params["input"][1]["path"], "/tmp/aegisy-image.png");
        assert_eq!(params["clientUserMessageId"], "client-turn-1");
    }

    #[test]
    fn turn_interrupt_matches_pinned_app_server_schema() {
        assert_eq!(
            turn_interrupt_params("thread-1", "turn-1"),
            json!({ "threadId": "thread-1", "turnId": "turn-1" })
        );
    }

    #[test]
    fn turn_steer_matches_pinned_app_server_schema() {
        let params = turn_steer_params(
            "thread-1",
            "turn-1",
            &TurnSteerRequest {
                input: "focus on the failing test".into(),
                client_user_message_id: Some("steer-1".into()),
            },
        );
        assert_eq!(params["threadId"], "thread-1");
        assert_eq!(params["expectedTurnId"], "turn-1");
        assert_eq!(params["input"][0]["type"], "text");
        assert_eq!(params["input"][0]["text"], "focus on the failing test");
        assert_eq!(params["clientUserMessageId"], "steer-1");
        assert!(params.get("turnId").is_none());
    }

    #[test]
    fn interrupted_turn_is_not_misreported_as_failure() {
        assert_eq!(
            turn_terminal_event(
                &json!({ "turn": { "id": "turn-1", "status": "interrupted" } }),
                "turn-1"
            )
            .unwrap(),
            CodexEvent::TurnInterrupted {
                turn_id: "turn-1".into()
            }
        );
    }

    #[test]
    fn error_notification_is_non_terminal_and_preserves_retry_metadata() {
        let observed = turn_error_observation(
            &json!({
                "threadId": "thread-1",
                "turnId": "turn-1",
                "willRetry": true,
                "error": {
                    "message": "stream disconnected before completion: private body",
                    "codexErrorInfo": {
                        "responseStreamDisconnected": { "httpStatusCode": 502 }
                    }
                }
            }),
            "turn-1",
        )
        .unwrap()
        .unwrap();
        let CodexEvent::TurnErrorObserved {
            turn_id,
            provider_error,
            will_retry,
            ..
        } = observed
        else {
            panic!("expected a non-terminal error observation")
        };
        assert_eq!(turn_id, "turn-1");
        assert!(will_retry);
        assert_eq!(
            provider_error.expect("provider classification").kind,
            "response-stream-disconnected"
        );
        assert!(turn_error_observation(
            &json!({
                "turnId": "late-turn",
                "willRetry": false,
                "error": { "message": "late" }
            }),
            "turn-1"
        )
        .unwrap()
        .is_none());
    }

    #[test]
    fn terminal_notification_rejects_identity_and_non_terminal_status_drift() {
        let identity_error = turn_terminal_event(
            &json!({ "turn": { "id": "turn-2", "status": "completed" } }),
            "turn-1",
        )
        .unwrap_err();
        assert!(identity_error.contains("protocol error"));
        let status_error = turn_terminal_event(
            &json!({ "turn": { "id": "turn-1", "status": "inProgress" } }),
            "turn-1",
        )
        .unwrap_err();
        assert!(status_error.contains("protocol error"));
        let missing_status =
            turn_terminal_event(&json!({ "turn": { "id": "turn-1" } }), "turn-1").unwrap_err();
        assert!(missing_status.contains("protocol error"));
    }

    #[test]
    fn command_items_preserve_structured_metadata_and_bound_output() {
        let fixture = json!({
            "id": "command-1",
            "type": "commandExecution",
            "command": "git status --short",
            "commandActions": [{
                "type": "listFiles",
                "command": "git status --short",
                "path": null
            }],
            "cwd": "/tmp/project",
            "status": "inProgress",
            "aggregatedOutput": null,
            "durationMs": null,
            "exitCode": null,
            "processId": "pty-1",
            "source": "agent"
        });
        let mut command = command_item(&fixture, None, 10, None).unwrap();
        assert_eq!(command.status, CommandStatus::InProgress);
        assert_eq!(command.source, CommandSource::Agent);
        assert_eq!(command.started_at_ms, 10);
        assert_eq!(command.completed_at_ms, None);
        append_bounded_output(&mut command, "M src/main.rs\n");
        assert_eq!(command.command_actions[0]["type"], "listFiles");
        assert_eq!(command.output.snapshot().head, "M src/main.rs\n");
        append_bounded_output(&mut command, &"界".repeat(256 * 1024));
        let snapshot = command.output.snapshot();
        assert!(snapshot.retained_bytes <= 256 * 1024);
        assert!(snapshot.omitted_bytes > 0);
        assert!(snapshot.head.is_char_boundary(0));
    }

    #[test]
    fn command_notification_fixture_preserves_one_incremental_lifecycle() {
        let started = json!({
            "threadId": "thread-1",
            "turnId": "turn-1",
            "startedAtMs": 10,
            "item": {
                "id": "command-1",
                "type": "commandExecution",
                "command": "git status --short",
                "commandActions": [{
                    "type": "listFiles",
                    "command": "git status --short",
                    "path": null
                }],
                "cwd": "/tmp/project",
                "status": "inProgress"
            }
        });
        let delta = json!({
            "threadId": "thread-1",
            "turnId": "turn-1",
            "itemId": "command-1",
            "delta": "M src/main.rs\n"
        });
        let completed = json!({
            "threadId": "thread-1",
            "turnId": "turn-1",
            "completedAtMs": 52,
            "item": {
                "id": "command-1",
                "type": "commandExecution",
                "command": "git status --short",
                "commandActions": [{
                    "type": "listFiles",
                    "command": "git status --short",
                    "path": null
                }],
                "cwd": "/tmp/project",
                "status": "completed",
                "aggregatedOutput": "M src/main.rs\n",
                "durationMs": 42,
                "exitCode": 0,
                "processId": "pty-1",
                "source": "agent"
            }
        });
        let mut commands = HashMap::new();
        let events = [
            translate_command_notification("item/started", &started, "turn-1", &mut commands),
            translate_command_notification(
                "item/commandExecution/outputDelta",
                &delta,
                "turn-1",
                &mut commands,
            ),
            translate_command_notification("item/completed", &completed, "turn-1", &mut commands),
        ];
        let lifecycles = events
            .into_iter()
            .map(|event| match event.unwrap().unwrap() {
                CodexEvent::CommandUpdated {
                    command, lifecycle, ..
                } => {
                    if lifecycle == "completed" {
                        assert_eq!(command.status, CommandStatus::Completed);
                        assert_eq!(command.source, CommandSource::Agent);
                        assert_eq!(command.started_at_ms, 10);
                        assert_eq!(command.completed_at_ms, Some(52));
                        assert_eq!(command.duration_ms, Some(42));
                        assert_eq!(command.exit_code, Some(0));
                        assert_eq!(command.output.snapshot().head, "M src/main.rs\n");
                    }
                    lifecycle
                }
                _ => panic!("unexpected event"),
            })
            .collect::<Vec<_>>();
        assert_eq!(lifecycles, ["started", "delta", "completed"]);
        assert_eq!(commands.len(), 1);
    }

    fn command_fixture(status: &str, source: Option<&str>) -> Value {
        let mut item = json!({
            "id": "command-1",
            "type": "commandExecution",
            "command": "git status --short",
            "commandActions": [],
            "cwd": "/tmp/project",
            "status": status
        });
        if let Some(source) = source {
            item["source"] = json!(source);
        }
        item
    }

    fn started_command_notification(item: Value, started_at_ms: Value) -> Value {
        json!({
            "threadId": "thread-1",
            "turnId": "turn-1",
            "startedAtMs": started_at_ms,
            "item": item
        })
    }

    fn completed_command_notification(item: Value, completed_at_ms: Value) -> Value {
        json!({
            "threadId": "thread-1",
            "turnId": "turn-1",
            "completedAtMs": completed_at_ms,
            "item": item
        })
    }

    fn start_command(
        commands: &mut HashMap<String, CommandItem>,
        source: Option<&str>,
        started_at_ms: u64,
    ) -> CommandItem {
        let notification = started_command_notification(
            command_fixture("inProgress", source),
            json!(started_at_ms),
        );
        match translate_command_notification("item/started", &notification, "turn-1", commands)
            .unwrap()
            .unwrap()
        {
            CodexEvent::CommandUpdated { command, .. } => *command,
            _ => panic!("unexpected command event"),
        }
    }

    #[test]
    fn command_statuses_map_only_the_pinned_lifecycle_values() {
        let mut commands = HashMap::new();
        let started = start_command(&mut commands, None, 10);
        assert_eq!(started.status, CommandStatus::InProgress);
        assert_eq!(started.status.as_str(), "inProgress");
        assert_eq!(started.source, CommandSource::Agent);

        for (wire_status, expected) in [
            ("completed", CommandStatus::Completed),
            ("failed", CommandStatus::Failed),
            ("declined", CommandStatus::Declined),
        ] {
            let mut commands = HashMap::new();
            start_command(&mut commands, None, 10);
            let notification =
                completed_command_notification(command_fixture(wire_status, None), json!(11));
            let event = translate_command_notification(
                "item/completed",
                &notification,
                "turn-1",
                &mut commands,
            )
            .unwrap()
            .unwrap();
            let CodexEvent::CommandUpdated { command, .. } = event else {
                panic!("unexpected command event");
            };
            assert_eq!(command.status, expected);
            assert_eq!(command.status.as_str(), wire_status);
            assert_eq!(command.started_at_ms, 10);
            assert_eq!(command.completed_at_ms, Some(11));
        }
    }

    #[test]
    fn command_status_rejects_missing_unknown_and_wrong_lifecycle_values() {
        let mut missing = command_fixture("inProgress", None);
        missing.as_object_mut().unwrap().remove("status");
        for item in [
            missing,
            command_fixture("cancelled", None),
            {
                let mut item = command_fixture("inProgress", None);
                item["status"] = Value::Null;
                item
            },
            command_fixture("completed", None),
        ] {
            let mut commands = HashMap::new();
            let error = translate_command_notification(
                "item/started",
                &started_command_notification(item, json!(10)),
                "turn-1",
                &mut commands,
            )
            .unwrap_err();
            assert!(error.contains("command lifecycle protocol error"));
            assert!(commands.is_empty());
        }

        let mut commands = HashMap::new();
        start_command(&mut commands, None, 10);
        let error = translate_command_notification(
            "item/completed",
            &completed_command_notification(command_fixture("inProgress", None), json!(11)),
            "turn-1",
            &mut commands,
        )
        .unwrap_err();
        assert!(error.contains("terminal status"));
    }

    #[test]
    fn command_execution_metadata_is_type_and_status_consistent() {
        let mut started_with_duration = command_fixture("inProgress", None);
        started_with_duration["durationMs"] = json!(1);
        assert!(translate_command_notification(
            "item/started",
            &started_command_notification(started_with_duration, json!(10)),
            "turn-1",
            &mut HashMap::new(),
        )
        .unwrap_err()
        .contains("started item is invalid"));

        for terminal in [
            {
                let mut item = command_fixture("completed", None);
                item["exitCode"] = json!(1);
                item
            },
            {
                let mut item = command_fixture("failed", None);
                item["exitCode"] = json!(0);
                item
            },
            {
                let mut item = command_fixture("declined", None);
                item["durationMs"] = json!(1);
                item
            },
            {
                let mut item = command_fixture("completed", None);
                item["durationMs"] = json!(2);
                item
            },
            {
                let mut item = command_fixture("completed", None);
                item["exitCode"] = json!((i32::MAX as i64) + 1);
                item
            },
            {
                let mut item = command_fixture("completed", None);
                item["processId"] = json!({"unexpected": true});
                item
            },
        ] {
            let mut commands = HashMap::new();
            start_command(&mut commands, None, 10);
            let error = translate_command_notification(
                "item/completed",
                &completed_command_notification(terminal, json!(11)),
                "turn-1",
                &mut commands,
            )
            .unwrap_err();
            assert!(error.contains("completed item is invalid"));
        }
    }

    #[test]
    fn command_started_item_rejects_missing_pinned_required_fields() {
        for field in ["id", "command", "commandActions", "cwd", "status"] {
            let mut item = command_fixture("inProgress", None);
            item.as_object_mut().unwrap().remove(field);
            let mut commands = HashMap::new();
            let error = translate_command_notification(
                "item/started",
                &started_command_notification(item, json!(10)),
                "turn-1",
                &mut commands,
            )
            .unwrap_err();
            assert!(error.contains("command lifecycle protocol error"));
            assert!(commands.is_empty());
        }

        let mut item = command_fixture("inProgress", None);
        item["commandActions"] = json!({});
        assert!(translate_command_notification(
            "item/started",
            &started_command_notification(item, json!(10)),
            "turn-1",
            &mut HashMap::new(),
        )
        .unwrap_err()
        .contains("started item is invalid"));
    }

    #[test]
    fn command_sources_map_only_the_pinned_values_and_default_only_when_absent() {
        for (wire_source, expected) in [
            ("agent", CommandSource::Agent),
            ("userShell", CommandSource::UserShell),
            ("unifiedExecStartup", CommandSource::UnifiedExecStartup),
            (
                "unifiedExecInteraction",
                CommandSource::UnifiedExecInteraction,
            ),
        ] {
            let mut commands = HashMap::new();
            let command = start_command(&mut commands, Some(wire_source), 10);
            assert_eq!(command.source, expected);
            assert_eq!(command.source.as_str(), wire_source);
        }

        let mut commands = HashMap::new();
        assert_eq!(
            start_command(&mut commands, None, 10).source,
            CommandSource::Agent
        );

        for source in [json!("runtime"), Value::Null, json!(1)] {
            let mut item = command_fixture("inProgress", None);
            item["source"] = source;
            let mut commands = HashMap::new();
            let error = translate_command_notification(
                "item/started",
                &started_command_notification(item, json!(10)),
                "turn-1",
                &mut commands,
            )
            .unwrap_err();
            assert!(error.contains("started item is invalid"));
        }
    }

    #[test]
    fn command_source_cannot_drift_between_started_and_terminal_items() {
        let mut commands = HashMap::new();
        start_command(&mut commands, Some("userShell"), 10);
        let error = translate_command_notification(
            "item/completed",
            &completed_command_notification(command_fixture("completed", Some("agent")), json!(11)),
            "turn-1",
            &mut commands,
        )
        .unwrap_err();
        assert!(error.contains("source changed"));
    }

    #[test]
    fn command_input_cannot_drift_between_started_and_terminal_items() {
        for field in ["command", "commandActions", "cwd"] {
            let mut commands = HashMap::new();
            let started = start_command(&mut commands, None, 10);
            let mut terminal = command_fixture("completed", None);
            terminal[field] = match field {
                "command" => json!("git diff --stat"),
                "commandActions" => {
                    json!([{"type": "unknown", "command": "git diff --stat"}])
                }
                "cwd" => json!("/tmp/other-project"),
                _ => unreachable!(),
            };
            let error = translate_command_notification(
                "item/completed",
                &completed_command_notification(terminal, json!(11)),
                "turn-1",
                &mut commands,
            )
            .unwrap_err();
            assert!(error.contains("input changed"));
            assert_eq!(commands.get("command-1"), Some(&started));
        }
    }

    #[test]
    fn command_input_drift_beyond_display_bounds_is_rejected() {
        let long_command_prefix = "c".repeat(32 * 1024);
        let long_cwd_prefix = format!("/{}", "w".repeat((4 * 1024) - 1));
        let actions = (0..33)
            .map(|index| json!({"type": "unknown", "command": format!("action-{index}")}))
            .collect::<Vec<_>>();
        let cases = [
            (
                {
                    let mut item = command_fixture("inProgress", None);
                    item["command"] = json!(format!("{long_command_prefix}a"));
                    item
                },
                {
                    let mut item = command_fixture("completed", None);
                    item["command"] = json!(format!("{long_command_prefix}b"));
                    item
                },
            ),
            (
                {
                    let mut item = command_fixture("inProgress", None);
                    item["cwd"] = json!(format!("{long_cwd_prefix}a"));
                    item
                },
                {
                    let mut item = command_fixture("completed", None);
                    item["cwd"] = json!(format!("{long_cwd_prefix}b"));
                    item
                },
            ),
            (
                {
                    let mut item = command_fixture("inProgress", None);
                    item["commandActions"] = json!(actions.clone());
                    item
                },
                {
                    let mut changed = actions.clone();
                    changed[32]["command"] = json!("changed-after-visible-bound");
                    let mut item = command_fixture("completed", None);
                    item["commandActions"] = json!(changed);
                    item
                },
            ),
            (
                {
                    let mut item = command_fixture("inProgress", None);
                    item["commandActions"] = json!([{
                        "type": "unknown",
                        "command": "opaque-command",
                        "providerOpaqueField": "first"
                    }]);
                    item
                },
                {
                    let mut item = command_fixture("completed", None);
                    item["commandActions"] = json!([{
                        "type": "unknown",
                        "command": "opaque-command",
                        "providerOpaqueField": "second"
                    }]);
                    item
                },
            ),
        ];

        for (started_item, terminal_item) in cases {
            let mut commands = HashMap::new();
            translate_command_notification(
                "item/started",
                &started_command_notification(started_item, json!(10)),
                "turn-1",
                &mut commands,
            )
            .unwrap()
            .unwrap();
            let error = translate_command_notification(
                "item/completed",
                &completed_command_notification(terminal_item, json!(11)),
                "turn-1",
                &mut commands,
            )
            .unwrap_err();
            assert!(error.contains("input changed"));
            assert_eq!(commands.len(), 1);
        }
    }

    #[test]
    fn command_trace_input_identity_uses_closed_redacted_projection_without_content() {
        let secret = "ghp_123456789012345678901234567890";
        let mut item = command_fixture("inProgress", None);
        item["command"] = json!(format!("tool --token {secret}"));
        item["commandActions"] = json!([{
            "type": "unknown",
            "command": format!("tool --token {secret}"),
            "providerOpaqueField": secret,
            "fullField": "x".repeat(8 * 1024)
        }]);
        let mut commands = HashMap::new();
        let event = translate_command_notification(
            "item/started",
            &started_command_notification(item, json!(10)),
            "turn-1",
            &mut commands,
        )
        .unwrap()
        .unwrap();
        let CodexEvent::CommandUpdated { command, .. } = event else {
            panic!("unexpected command event");
        };
        assert!(command.trace_input_identity.starts_with("sha256:"));
        assert_eq!(command.trace_input_identity.len(), 71);
        assert!(!command.trace_input_identity.contains(secret));
        assert!(!format!("{:?}", command.provider_input_fingerprint).contains(secret));
        assert_eq!(
            format!("{:?}", command.provider_input_fingerprint),
            "<redacted-provider-command-input>"
        );
    }

    #[test]
    fn command_trace_projection_excludes_unknown_fields_but_raw_drift_guard_retains_them() {
        let mut first = command_fixture("inProgress", None);
        first["commandActions"] = json!([{
            "type": "unknown",
            "command": "printf stable",
            "authorization: ghp_123456789012345678901234567890": "first-private-value"
        }]);
        let mut second = first.clone();
        second["commandActions"] = json!([{
            "type": "unknown",
            "command": "printf stable",
            "another-private-provider-key": "second-private-value"
        }]);
        let without_extension = json!([{
            "type": "unknown",
            "command": "printf stable"
        }]);

        let first = command_item(&first, None, 10, None).unwrap();
        let second = command_item(&second, None, 10, None).unwrap();
        let mut baseline = command_fixture("inProgress", None);
        baseline["commandActions"] = without_extension;
        let baseline = command_item(&baseline, None, 10, None).unwrap();

        assert_eq!(first.trace_input_identity, second.trace_input_identity);
        assert_eq!(first.trace_input_identity, baseline.trace_input_identity);
        assert_ne!(
            first.provider_input_fingerprint,
            second.provider_input_fingerprint
        );
        assert_ne!(
            first.provider_input_fingerprint,
            baseline.provider_input_fingerprint
        );
        let debug = format!("{first:?}{second:?}");
        assert!(!debug.contains("authorization"));
        assert!(!debug.contains("first-private-value"));
        assert!(!debug.contains("second-private-value"));
    }

    #[test]
    fn command_trace_projection_is_closed_typed_and_canonical() {
        let mut absent = command_fixture("inProgress", None);
        absent["commandActions"] = json!([{
            "type": "search",
            "command": "rg symbol"
        }]);
        let mut null = absent.clone();
        null["commandActions"] = json!([{
            "type": "search",
            "command": "rg symbol",
            "path": null,
            "query": null
        }]);
        let absent = command_item(&absent, None, 10, None).unwrap();
        let null = command_item(&null, None, 10, None).unwrap();
        assert_eq!(absent.trace_input_identity, null.trace_input_identity);

        let mut changed = command_fixture("inProgress", None);
        changed["commandActions"] = json!([{
            "type": "search",
            "command": "rg different",
            "query": "different"
        }]);
        let changed = command_item(&changed, None, 10, None).unwrap();
        assert_ne!(absent.trace_input_identity, changed.trace_input_identity);

        let mut unsupported = command_fixture("inProgress", None);
        unsupported["commandActions"] = json!([{
            "type": "futureAction",
            "command": "printf future"
        }]);
        assert!(command_item(&unsupported, None, 10, None).is_none());
    }

    #[test]
    fn command_input_identity_ignores_object_key_order_but_preserves_action_order() {
        let actions_first: Value = serde_json::from_str(
            r#"[
                {"type":"unknown","command":"printf first","extension":{"z":1,"a":2}},
                {"type":"unknown","command":"printf second"}
            ]"#,
        )
        .unwrap();
        let actions_reordered_keys: Value = serde_json::from_str(
            r#"[
                {"extension":{"a":2,"z":1},"command":"printf first","type":"unknown"},
                {"command":"printf second","type":"unknown"}
            ]"#,
        )
        .unwrap();
        let actions_reordered_array: Value = serde_json::from_str(
            r#"[
                {"type":"unknown","command":"printf second"},
                {"type":"unknown","command":"printf first","extension":{"a":2,"z":1}}
            ]"#,
        )
        .unwrap();

        let mut first = command_fixture("inProgress", None);
        first["commandActions"] = actions_first;
        let mut reordered_keys = command_fixture("inProgress", None);
        reordered_keys["commandActions"] = actions_reordered_keys;
        let mut reordered_array = command_fixture("inProgress", None);
        reordered_array["commandActions"] = actions_reordered_array;

        let first = command_item(&first, None, 10, None).unwrap();
        let reordered_keys = command_item(&reordered_keys, None, 10, None).unwrap();
        let reordered_array = command_item(&reordered_array, None, 10, None).unwrap();

        assert_eq!(
            first.provider_input_fingerprint,
            reordered_keys.provider_input_fingerprint
        );
        assert_eq!(
            first.trace_input_identity,
            reordered_keys.trace_input_identity
        );
        assert_ne!(
            first.provider_input_fingerprint,
            reordered_array.provider_input_fingerprint
        );
        assert_ne!(
            first.trace_input_identity,
            reordered_array.trace_input_identity
        );
    }

    #[test]
    fn command_lifecycle_timestamps_accept_int64_boundaries_and_preserve_exact_values() {
        let mut zero_commands = HashMap::new();
        let zero = start_command(&mut zero_commands, None, 0);
        assert_eq!(zero.started_at_ms, 0);

        let boundary = i64::MAX as u64;
        let mut commands = HashMap::new();
        let started = start_command(&mut commands, None, boundary);
        assert_eq!(started.started_at_ms, boundary);
        let event = translate_command_notification(
            "item/completed",
            &completed_command_notification(command_fixture("declined", None), json!(boundary)),
            "turn-1",
            &mut commands,
        )
        .unwrap()
        .unwrap();
        let CodexEvent::CommandUpdated { command, .. } = event else {
            panic!("unexpected command event");
        };
        assert_eq!(command.started_at_ms, boundary);
        assert_eq!(command.completed_at_ms, Some(boundary));
    }

    #[test]
    fn command_lifecycle_timestamps_reject_missing_non_int64_and_reverse_time() {
        let mut missing_started =
            started_command_notification(command_fixture("inProgress", None), json!(10));
        missing_started
            .as_object_mut()
            .unwrap()
            .remove("startedAtMs");
        let too_large = (i64::MAX as u64) + 1;
        for notification in [
            missing_started,
            started_command_notification(command_fixture("inProgress", None), json!(-1)),
            started_command_notification(command_fixture("inProgress", None), json!(too_large)),
            started_command_notification(command_fixture("inProgress", None), json!(1.5)),
            started_command_notification(command_fixture("inProgress", None), json!("10")),
        ] {
            let mut commands = HashMap::new();
            let error = translate_command_notification(
                "item/started",
                &notification,
                "turn-1",
                &mut commands,
            )
            .unwrap_err();
            assert!(error.contains("timestamp is invalid"));
            assert!(commands.is_empty());
        }

        let mut started_with_completion =
            started_command_notification(command_fixture("inProgress", None), json!(10));
        started_with_completion["completedAtMs"] = json!(10);
        assert!(translate_command_notification(
            "item/started",
            &started_with_completion,
            "turn-1",
            &mut HashMap::new(),
        )
        .unwrap_err()
        .contains("completion timestamp"));

        for completed_at_ms in [
            Value::Null,
            json!(-1),
            json!(too_large),
            json!(1.5),
            json!("10"),
        ] {
            let mut commands = HashMap::new();
            start_command(&mut commands, None, 10);
            let error = translate_command_notification(
                "item/completed",
                &completed_command_notification(
                    command_fixture("completed", None),
                    completed_at_ms,
                ),
                "turn-1",
                &mut commands,
            )
            .unwrap_err();
            assert!(error.contains("timestamp is invalid"));
        }

        let mut missing_completed =
            completed_command_notification(command_fixture("completed", None), json!(11));
        missing_completed
            .as_object_mut()
            .unwrap()
            .remove("completedAtMs");
        let mut commands = HashMap::new();
        start_command(&mut commands, None, 10);
        assert!(translate_command_notification(
            "item/completed",
            &missing_completed,
            "turn-1",
            &mut commands,
        )
        .unwrap_err()
        .contains("timestamp is invalid"));

        let mut commands = HashMap::new();
        start_command(&mut commands, None, 10);
        assert!(translate_command_notification(
            "item/completed",
            &completed_command_notification(command_fixture("failed", None), json!(9)),
            "turn-1",
            &mut commands,
        )
        .unwrap_err()
        .contains("precedes its start"));
    }

    #[test]
    fn codex_parent_process_environment_is_value_free_and_secret_scrubbed() {
        let environment = codex_process_environment().unwrap();
        let names = environment
            .iter()
            .map(|(name, _)| name.to_owned())
            .collect::<Vec<_>>();
        assert!(names.contains(&"PATH".into()));
        assert!(names.contains(&"AEGISY_TOOL_NAME".into()));
        for denied in [
            "OPENAI_API_KEY",
            "ANTHROPIC_API_KEY",
            "AWS_SECRET_ACCESS_KEY",
            "HTTP_PROXY",
            "LD_PRELOAD",
            "DYLD_INSERT_LIBRARIES",
            "NODE_OPTIONS",
        ] {
            assert!(!names.contains(&denied.into()));
        }
        assert!(environment
            .summary()
            .environment_id
            .starts_with("environment:sha256:"));
        assert_eq!(environment.summary().variable_count, names.len());
    }

    #[test]
    fn bounded_line_reader_accepts_the_limit_and_drains_oversized_frames() {
        let input = b"1234\n12345\nok\r\nlast";
        let mut reader = BufReader::with_capacity(2, &input[..]);
        assert_eq!(
            read_bounded_line(&mut reader, 4).unwrap(),
            BoundedLine::Line(b"1234".to_vec())
        );
        assert_eq!(
            read_bounded_line(&mut reader, 4).unwrap(),
            BoundedLine::Oversized
        );
        assert_eq!(
            read_bounded_line(&mut reader, 4).unwrap(),
            BoundedLine::Line(b"ok".to_vec())
        );
        assert_eq!(
            read_bounded_line(&mut reader, 4).unwrap(),
            BoundedLine::Line(b"last".to_vec())
        );
        assert_eq!(read_bounded_line(&mut reader, 4).unwrap(), BoundedLine::Eof);
    }

    #[test]
    fn codex_message_queue_applies_backpressure_and_then_unblocks() {
        let (sender, receiver) = codex_message_channel();
        for index in 0..CODEX_MESSAGE_QUEUE_CAPACITY {
            sender.send(json!(index)).unwrap();
        }
        let (finished_sender, finished_receiver) = mpsc::channel();
        let producer = thread::spawn(move || {
            sender.send(json!("after-capacity")).unwrap();
            finished_sender.send(()).unwrap();
        });
        assert!(finished_receiver
            .recv_timeout(Duration::from_millis(50))
            .is_err());
        assert_eq!(receiver.recv().unwrap(), json!(0));
        finished_receiver
            .recv_timeout(Duration::from_secs(1))
            .unwrap();
        producer.join().unwrap();
    }

    #[test]
    fn command_output_redaction_precedes_all_capture_buffers() {
        let fixture = json!({
            "id": "command-secret",
            "type": "commandExecution",
            "command": "print diagnostics",
            "commandActions": [],
            "cwd": "/tmp/project",
            "status": "inProgress"
        });
        let secret = "ghp_123456789012345678901234567890";
        let mut command = command_item(&fixture, None, 10, None).unwrap();
        append_bounded_output(&mut command, "Authorization: Bearer ");
        append_bounded_output(&mut command, secret);
        finish_bounded_output(&mut command);
        let snapshot = command.output.snapshot();
        let artifact = command.output.artifact();
        assert!(!snapshot.head.contains(secret));
        assert!(!snapshot.tail.contains(secret));
        assert!(!artifact.content.contains(secret));
        assert_eq!(command.redactor.redacted_count(), 1);
        assert_eq!(
            command.redactor.source_bytes(),
            ("Authorization: Bearer ".len() + secret.len()) as u64
        );
    }

    #[cfg(target_os = "macos")]
    #[test]
    #[ignore = "requires an explicitly selected installed codex-cli 0.144.5 binary; run with AEGISY_CODEX_PATH=... --ignored"]
    fn live_pinned_binary_initializes_and_starts_read_only_thread() {
        let path = std::env::var_os("AEGISY_CODEX_PATH")
            .expect("AEGISY_CODEX_PATH must select the live pinned Codex binary");
        assert!(Path::new(&path).is_file());
        let mut adapter = CodexAdapter::start().expect("pinned Codex app-server must initialize");
        assert_eq!(adapter.info().version, PINNED_CODEX_VERSION);
        let session = adapter
            .start_session(Path::new("/tmp"), true)
            .expect("pinned Codex app-server must accept a read-only thread start");
        assert!(!session.thread_id.is_empty());
        assert_eq!(adapter.info().permission_profile, "read-only");
        assert_eq!(adapter.health().state, "running");
    }

    #[test]
    fn command_metadata_is_redacted_at_the_adapter_boundary() {
        let secret = "ghp_123456789012345678901234567890";
        let fixture = json!({
            "id": "command-secret-metadata",
            "type": "commandExecution",
            "command": format!("tool --token {secret}"),
            "commandActions": [{
                "type": "search",
                "command": format!("rg --api-key={secret}"),
                "query": format!("--api-key={secret}")
            }],
            "cwd": "/tmp/project",
            "status": "inProgress"
        });
        let command = command_item(&fixture, None, 10, None).unwrap();
        let serialized = format!("{}\n{}", command.command, command.command_actions);
        assert!(!serialized.contains(secret));
        assert!(serialized.contains("[REDACTED]"));
    }
}
