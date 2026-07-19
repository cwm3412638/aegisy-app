pub(crate) mod codex_adapter;
mod command_action;
mod command_artifact;
mod command_diagnostics;
mod command_output;
mod diagnostic_store;
mod durable_blob;
pub mod git_branch_transaction;
pub mod git_checkpoint;
pub mod git_commit_transaction;
mod git_query;
pub mod git_staging;
mod git_status;
pub mod git_workflow_authorization;
pub mod git_workflow_execution;
pub mod git_workflow_state;
pub mod git_worktree_lifecycle;
mod language_server;
pub mod non_git_checkpoint;
mod operation_probe;
pub mod operation_reconciliation;
mod output_redaction;
pub mod permission_issuer;
pub mod permission_profile;
mod repository_index;
pub mod session_compaction;
pub mod session_compaction_store;
mod session_environment;
#[cfg(target_os = "macos")]
mod terminal;
#[cfg(target_os = "windows")]
#[path = "terminal_windows.rs"]
mod terminal;
#[cfg(not(any(target_os = "macos", target_os = "windows")))]
#[path = "terminal_unsupported.rs"]
mod terminal;
mod turn_context;
mod workbench_migration;
pub mod workbench_store;
mod workspace;
pub mod workspace_edit;
pub mod workspace_edit_apply;
pub mod workspace_edit_overlap;
mod workspace_edit_preview;
pub mod workspace_edit_restore;

use aegisy_aap::stable::v0_1::{
    BackendDescriptor, EventEnvelope, Identity, InitializeParams, InitializeResult, Project,
    Session, SessionMode, TimelineItem,
};
use aegisy_aap::{Notification, Request, Response, JSONRPC_VERSION, PROTOCOL_VERSION};
use base64::engine::general_purpose::STANDARD as BASE64_STANDARD;
use base64::Engine;
use codex_adapter::{BackendInfo, CodexAdapter, CodexEvent, CodexSession, CommandItem};
use diagnostic_store::DiagnosticStore;
use git_query::{commit as git_commit, diff as git_diff, log as git_log, overview as git_overview};
use git_status::{ignored_paths, status as git_status};
use language_server::LanguageServerManager;
use operation_reconciliation::{
    reconcile as reconcile_operation, EventState, GitState, OperationKind, ProcessState,
    ReconciliationEvidence, ReconciliationInput, WorkspaceState,
};
use repository_index::WorkspaceIndex;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use session_environment::SessionEnvironment;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet, VecDeque};
use std::fs;
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{SystemTime, UNIX_EPOCH};
use terminal::{TerminalError, TerminalManager, TerminalOpenContext};
use turn_context::{prepare_turn_context_scoped, TurnContextItem};
use workbench_store::{
    durable_blob_reference_id, DurableBlobKind, DurableBlobWrite, PortableSessionImportCommand,
    PortableSessionPackage, RetentionPolicy, SessionDeletionScope, SessionProjectionConsistency,
    SessionSearchRequest, StoredItem, StoredItemAppend, StoredProjectCreate,
    StoredProjectNavigationEntry, StoredProjectTrustAcknowledge, StoredProjectTrustAcknowledgement,
    StoredSessionCreate, StoredSessionLineage, StoredSessionMode,
    StoredSessionRuntimeBindingCreate, StoredTurnCreate, WorkbenchRecoveryDiagnostic,
    WorkbenchStore, WorkbenchStoreOpen,
};
use workspace::{
    collect_search_candidates, list_directory, path_metadata, read_text_file, search_workspace,
    write_text_file,
};
use workspace_edit::{ContentHash, WorkspaceEdit, WorkspaceEditOperation};
use workspace_edit_preview::{ContentInput, PreviewArtifactSnapshot, WorkspaceEditPreviewStore};

const DURABLE_COMMAND_ARTIFACT_RETENTION_MS: u64 = 30 * 24 * 60 * 60 * 1_000;
const DURABLE_PREVIEW_RETENTION_MS: u64 = 7 * 24 * 60 * 60 * 1_000;
const MAX_TRUST_REVIEW_ENTRIES: usize = 512;
const MAX_TRUST_REVIEW_DEPTH: usize = 5;
const MAX_TRUST_REVIEW_HOOK_BYTES: usize = 256 * 1024;
const MAX_RUNTIME_REPLAY_ITEMS: usize = 2_000;
const RUNTIME_REPLAY_PAGE_SIZE: usize = 200;
const MAX_TURN_METADATA_UPDATES_PER_KIND: usize = 32;

const TRUST_INSTRUCTION_NAMES: &[&str] = &[
    "AGENTS.md",
    "CLAUDE.md",
    "CODEX.md",
    "GEMINI.md",
    "Aegisy.md",
    "copilot-instructions.md",
];

pub struct Runtime {
    initialized: bool,
    client_ready: bool,
    shutdown: bool,
    sequence: u64,
    next_id: u64,
    control: RuntimeControl,
    projects: HashMap<String, Project>,
    project_roots: HashMap<String, Vec<workbench_store::StoredProjectRoot>>,
    project_navigation: HashMap<String, workbench_store::StoredProjectNavigation>,
    project_trust_acknowledgements: HashMap<String, StoredProjectTrustAcknowledgement>,
    sessions: HashMap<String, SessionState>,
    workspace_watches: HashMap<String, WorkspaceWatch>,
    // Indexes and language/diagnostic state are scoped to one registered root.
    // A project may have several roots with different access and filesystem
    // identities, so project_id alone is not a sufficient cache key.
    workspace_indexes: HashMap<String, WorkspaceIndex>,
    language_servers: LanguageServerManager,
    diagnostic_store: DiagnosticStore,
    command_artifacts: command_artifact::CommandArtifactStore,
    workspace_edit_previews: WorkspaceEditPreviewStore,
    cancelled_workspace_searches: HashSet<String>,
    cancelled_workspace_indexes: HashSet<(String, String)>,
    archived_sessions: HashSet<String>,
    operation_reconciliations: HashMap<String, operation_reconciliation::ReconciliationResult>,
    workbench_store: Option<WorkbenchStore>,
    compaction_store: Option<session_compaction_store::CompactionCheckpointStore>,
    backend: Backend,
}

#[derive(Clone, Default)]
pub struct RuntimeControl {
    state: Arc<Mutex<RuntimeControlState>>,
    terminals: Arc<Mutex<TerminalState>>,
}

#[derive(Default)]
struct TerminalState {
    manager: TerminalManager,
    records: HashMap<String, TerminalRecord>,
}

#[derive(Default)]
struct RuntimeControlState {
    protocol_ready: bool,
    request_ids: HashSet<String>,
    active_turn: Option<ActiveTurnControl>,
}

struct ActiveTurnControl {
    session_id: String,
    turn_id: Option<String>,
    cancellation: TurnCancellationHandle,
    steering: TurnSteeringHandle,
}

#[derive(Clone, Debug, Default)]
pub(crate) struct TurnCancellationHandle {
    requested: Arc<AtomicBool>,
}

impl TurnCancellationHandle {
    pub(crate) fn is_requested(&self) -> bool {
        self.requested.load(Ordering::Acquire)
    }
}

const TURN_STEER_QUEUE_CAPACITY: usize = 8;
const MAX_TURN_STEER_INPUT_BYTES: usize = 64 * 1024;
const MAX_CLIENT_USER_MESSAGE_ID_BYTES: usize = 256;

#[derive(Debug, Clone)]
pub(crate) struct TurnSteerRequest {
    pub input: String,
    pub client_user_message_id: Option<String>,
}

#[derive(Clone, Debug, Default)]
pub(crate) struct TurnSteeringHandle {
    pending: Arc<Mutex<VecDeque<TurnSteerRequest>>>,
}

impl TurnSteeringHandle {
    pub(crate) fn drain(&self) -> Vec<TurnSteerRequest> {
        let mut pending = self
            .pending
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        pending.drain(..).collect()
    }

    fn enqueue(&self, request: TurnSteerRequest) -> Result<usize, ()> {
        let mut pending = self
            .pending
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        if pending.len() >= TURN_STEER_QUEUE_CAPACITY {
            return Err(());
        }
        pending.push_back(request);
        Ok(pending.len())
    }
}

#[derive(Debug, Deserialize)]
struct TurnCancelParams {
    session_id: String,
    turn_id: String,
}

#[derive(Debug, Deserialize)]
struct TurnSteerParams {
    session_id: String,
    turn_id: String,
    input: String,
    #[serde(default)]
    client_user_message_id: Option<String>,
}

impl RuntimeControl {
    fn lock(&self) -> MutexGuard<'_, RuntimeControlState> {
        self.state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    fn claim_request(&self, request: &Request) -> Option<Value> {
        let id = request.id.as_ref()?;
        let mut state = self.lock();
        if state.request_ids.insert(id.to_string()) {
            None
        } else {
            Some(
                serde_json::to_value(Response::error(id.clone(), -32001, "duplicate request id"))
                    .expect("duplicate request response serialization"),
            )
        }
    }

    fn set_protocol_ready(&self, ready: bool) {
        self.lock().protocol_ready = ready;
    }

    fn begin_turn(&self, session_id: &str) -> TurnCancellationHandle {
        let cancellation = TurnCancellationHandle::default();
        self.lock().active_turn = Some(ActiveTurnControl {
            session_id: session_id.into(),
            turn_id: None,
            cancellation: cancellation.clone(),
            steering: TurnSteeringHandle::default(),
        });
        cancellation
    }

    fn steering_handle(&self, session_id: &str) -> Option<TurnSteeringHandle> {
        self.lock()
            .active_turn
            .as_ref()
            .filter(|active| active.session_id == session_id)
            .map(|active| active.steering.clone())
    }

    fn identify_turn(&self, session_id: &str, turn_id: &str) {
        let mut state = self.lock();
        if let Some(active) = state.active_turn.as_mut() {
            if active.session_id == session_id {
                active.turn_id = Some(turn_id.into());
            }
        }
    }

    fn has_active_turn(&self, session_id: &str) -> bool {
        self.lock()
            .active_turn
            .as_ref()
            .is_some_and(|active| active.session_id == session_id)
    }

    fn protected_session_ids(&self) -> BTreeSet<String> {
        let mut protected = BTreeSet::new();
        if let Some(active) = self.lock().active_turn.as_ref() {
            protected.insert(active.session_id.clone());
        }
        let terminals = self
            .terminals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        protected.extend(
            terminals
                .records
                .values()
                .filter(|terminal| matches!(terminal.state.as_str(), "running" | "stopping"))
                .map(|terminal| terminal.session_id.clone()),
        );
        protected
    }

    fn has_active_work(&self) -> bool {
        let state = self.lock();
        if state.active_turn.is_some() {
            return true;
        }
        drop(state);
        self.terminals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .records
            .values()
            .any(|terminal| matches!(terminal.state.as_str(), "running" | "stopping"))
    }

    fn finish_turn(&self, session_id: &str) {
        let mut state = self.lock();
        if state
            .active_turn
            .as_ref()
            .is_some_and(|active| active.session_id == session_id)
        {
            state.active_turn = None;
        }
    }

    fn cancel_claimed(&self, request: Request) -> Vec<Value> {
        if !self.lock().protocol_ready {
            return vec![serde_json::to_value(Response::error(
                request.id.unwrap_or(Value::Null),
                -32002,
                "initialize handshake required",
            ))
            .expect("cancel readiness response serialization")];
        }
        let params: TurnCancelParams = match serde_json::from_value(request.params) {
            Ok(params) => params,
            Err(error) => {
                return vec![serde_json::to_value(Response::error(
                    request.id.unwrap_or(Value::Null),
                    -32602,
                    format!("invalid params: {error}"),
                ))
                .expect("cancel parameter response serialization")]
            }
        };
        if params.session_id.is_empty() || params.turn_id.is_empty() {
            return vec![serde_json::to_value(Response::error(
                request.id.unwrap_or(Value::Null),
                -32602,
                "session_id and turn_id must not be empty",
            ))
            .expect("cancel validation response serialization")];
        }
        let mut state = self.lock();
        let Some(active) = state.active_turn.as_mut() else {
            return vec![serde_json::to_value(Response::error(
                request.id.unwrap_or(Value::Null),
                -32080,
                "turn is not active",
            ))
            .expect("inactive turn response serialization")];
        };
        if active.session_id != params.session_id
            || active.turn_id.as_deref() != Some(params.turn_id.as_str())
        {
            return vec![serde_json::to_value(Response::error(
                request.id.unwrap_or(Value::Null),
                -32081,
                "turn identity does not match the active turn",
            ))
            .expect("turn identity response serialization")];
        }
        let already_requested = active.cancellation.requested.swap(true, Ordering::AcqRel);
        vec![serde_json::to_value(Response::success(
            request.id.unwrap_or(Value::Null),
            json!({
                "session_id": params.session_id,
                "turn_id": params.turn_id,
                "state": "cancellation-requested",
                "already_requested": already_requested
            }),
        ))
        .expect("cancel response serialization")]
    }

    fn steer_claimed(&self, request: Request) -> Vec<Value> {
        if !self.lock().protocol_ready {
            return vec![serde_json::to_value(Response::error(
                request.id.unwrap_or(Value::Null),
                -32002,
                "initialize handshake required",
            ))
            .expect("steer readiness response serialization")];
        }
        let params: TurnSteerParams = match serde_json::from_value(request.params) {
            Ok(params) => params,
            Err(error) => {
                return vec![serde_json::to_value(Response::error(
                    request.id.unwrap_or(Value::Null),
                    -32602,
                    format!("invalid params: {error}"),
                ))
                .expect("steer parameter response serialization")]
            }
        };
        if params.session_id.is_empty()
            || params.turn_id.is_empty()
            || params.input.trim().is_empty()
        {
            return vec![serde_json::to_value(Response::error(
                request.id.unwrap_or(Value::Null),
                -32602,
                "session_id, turn_id, and input must not be empty",
            ))
            .expect("steer validation response serialization")];
        }
        if params.input.len() > MAX_TURN_STEER_INPUT_BYTES {
            return vec![serde_json::to_value(Response::error(
                request.id.unwrap_or(Value::Null),
                -32602,
                "turn steering input exceeds the 64 KiB limit",
            ))
            .expect("steer input limit response serialization")];
        }
        if params.client_user_message_id.as_ref().is_some_and(|id| {
            id.is_empty()
                || id.len() > MAX_CLIENT_USER_MESSAGE_ID_BYTES
                || id.chars().any(char::is_control)
        }) {
            return vec![serde_json::to_value(Response::error(
                request.id.unwrap_or(Value::Null),
                -32602,
                "client_user_message_id is invalid",
            ))
            .expect("steer client message id response serialization")];
        }

        let mut state = self.lock();
        let Some(active) = state.active_turn.as_mut() else {
            return vec![serde_json::to_value(Response::error(
                request.id.unwrap_or(Value::Null),
                -32080,
                "turn is not active",
            ))
            .expect("inactive steer response serialization")];
        };
        if active.session_id != params.session_id
            || active.turn_id.as_deref() != Some(params.turn_id.as_str())
        {
            return vec![serde_json::to_value(Response::error(
                request.id.unwrap_or(Value::Null),
                -32081,
                "turn identity does not match the active turn",
            ))
            .expect("steer identity response serialization")];
        }
        let queued = match active.steering.enqueue(TurnSteerRequest {
            input: params.input,
            client_user_message_id: params.client_user_message_id,
        }) {
            Ok(queued) => queued,
            Err(()) => {
                return vec![serde_json::to_value(Response::error(
                    request.id.unwrap_or(Value::Null),
                    -32004,
                    "turn steering queue is full",
                ))
                .expect("steer queue response serialization")]
            }
        };
        vec![serde_json::to_value(Response::success(
            request.id.unwrap_or(Value::Null),
            json!({
                "session_id": params.session_id,
                "turn_id": params.turn_id,
                "state": "steering-requested",
                "queued": queued
            }),
        ))
        .expect("steer response serialization")]
    }

    fn terminal_stop_claimed(&self, request: Request) -> Vec<Value> {
        if !self.lock().protocol_ready {
            return vec![serde_json::to_value(Response::error(
                request.id.unwrap_or(Value::Null),
                -32002,
                "initialize handshake required",
            ))
            .expect("terminal stop readiness response serialization")];
        }
        let params: TerminalReadParams = match serde_json::from_value(request.params) {
            Ok(params) => params,
            Err(error) => {
                return vec![serde_json::to_value(Response::error(
                    request.id.unwrap_or(Value::Null),
                    -32602,
                    format!("invalid params: {error}"),
                ))
                .expect("terminal stop parameter response serialization")]
            }
        };
        let mut terminals = self
            .terminals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        match terminals.stop_user(&params.terminal_id, &params.session_id) {
            Ok(snapshot) => vec![serde_json::to_value(Response::success(
                request.id.unwrap_or(Value::Null),
                snapshot,
            ))
            .expect("terminal stop response serialization")],
            Err(error) => vec![serde_json::to_value(Response::error(
                request.id.unwrap_or(Value::Null),
                error.code,
                error.message,
            ))
            .expect("terminal stop error serialization")],
        }
    }

    pub fn handle_out_of_band_line(&self, line: &str) -> Option<Vec<Value>> {
        let request: Request = serde_json::from_str(line).ok()?;
        if !matches!(
            request.method.as_str(),
            "turn/cancel" | "turn/steer" | "terminal/stop-user"
        ) {
            return None;
        }
        if request.jsonrpc != JSONRPC_VERSION {
            return Some(vec![serde_json::to_value(Response::error(
                request.id.unwrap_or(Value::Null),
                -32600,
                "invalid JSON-RPC version",
            ))
            .expect("cancel protocol response serialization")]);
        }
        if let Some(duplicate) = self.claim_request(&request) {
            return Some(vec![duplicate]);
        }
        Some(match request.method.as_str() {
            "turn/cancel" => self.cancel_claimed(request),
            "turn/steer" => self.steer_claimed(request),
            "terminal/stop-user" => self.terminal_stop_claimed(request),
            _ => unreachable!("out-of-band method was checked above"),
        })
    }
}

enum Backend {
    Preview,
    Codex(CodexAdapter),
    Recovery(WorkbenchRecoveryDiagnostic),
    Unavailable(String),
}

#[derive(Debug)]
struct SessionState {
    session: Session,
    items: Vec<TimelineItem>,
    backend_session_id: Option<String>,
    backend_info: BackendInfo,
    environment: SessionEnvironment,
}

#[derive(Debug, Clone)]
struct WorkspaceWatch {
    project_id: String,
    root_id: String,
    paths: Vec<String>,
    snapshots: HashMap<String, BTreeMap<String, String>>,
}

#[derive(Debug, Deserialize)]
struct ProjectOpenParams {
    root: String,
}

#[derive(Debug, Deserialize)]
struct ProjectListParams {
    #[serde(default = "default_project_list_limit")]
    limit: usize,
}

#[derive(Debug, Deserialize)]
struct ProjectNavigationParams {
    project_id: String,
    pinned: bool,
}

#[derive(Debug, Deserialize)]
struct ProjectTrustReviewParams {
    root: String,
}

#[derive(Debug, Deserialize)]
struct ProjectTrustAcknowledgeParams {
    project_id: String,
    #[serde(default = "default_primary_root_id")]
    root_id: String,
    root_identity: String,
    review_id: String,
}

#[derive(Debug, Deserialize)]
struct ProjectRootListParams {
    project_id: String,
}

#[derive(Debug, Deserialize)]
struct ProjectRootAddParams {
    project_id: String,
    root: String,
    access: String,
}

#[derive(Debug, Deserialize)]
struct ProjectRootRemoveParams {
    project_id: String,
    root_id: String,
}

#[derive(Debug, Deserialize)]
struct ProjectRelinkParams {
    project_id: String,
    #[serde(default = "default_primary_root_id")]
    root_id: String,
    root: String,
    expected_root_identity: String,
}

#[derive(Debug, Deserialize)]
struct SessionStartParams {
    mode: SessionMode,
    #[serde(default)]
    project_id: Option<String>,
    #[serde(default)]
    title: Option<String>,
}

#[derive(Debug, Deserialize)]
struct SessionListParams {
    #[serde(default)]
    project_id: Option<String>,
    #[serde(default)]
    mode: Option<SessionMode>,
    #[serde(default)]
    include_archived: bool,
    #[serde(default = "default_session_list_limit")]
    limit: usize,
}

#[derive(Debug, Deserialize)]
struct SessionSearchParams {
    #[serde(default)]
    project_id: Option<String>,
    #[serde(default)]
    branch: Option<String>,
    #[serde(default)]
    model: Option<String>,
    #[serde(default)]
    runtime: Option<String>,
    #[serde(default)]
    status: Option<String>,
    #[serde(default)]
    title: Option<String>,
    #[serde(default)]
    text: Option<String>,
    #[serde(default)]
    include_archived: bool,
    #[serde(default)]
    cursor: Option<String>,
    #[serde(default = "default_session_search_limit")]
    limit: usize,
}

#[derive(Debug, Deserialize)]
struct SessionIdentityParams {
    session_id: String,
}

#[derive(Debug, Deserialize)]
struct ProviderThreadListParams {
    #[serde(default)]
    project_id: Option<String>,
    #[serde(default)]
    cursor: Option<String>,
    #[serde(default = "default_provider_thread_list_limit")]
    limit: usize,
    #[serde(default)]
    archived: Option<bool>,
}

#[derive(Debug, Deserialize)]
struct ProviderThreadReadParams {
    thread_id: String,
    #[serde(default)]
    include_turns: bool,
}

#[derive(Debug, Deserialize)]
struct SessionForkParams {
    session_id: String,
    #[serde(default)]
    last_turn_id: Option<String>,
    #[serde(default)]
    title: Option<String>,
}

#[derive(Debug, Deserialize)]
struct SessionTitleParams {
    session_id: String,
    title: String,
}

#[derive(Debug, Deserialize)]
struct SessionDeletePreviewParams {
    session_id: String,
    scope: SessionDeletionScope,
}

#[derive(Debug, Deserialize)]
struct SessionDeleteScheduleParams {
    session_id: String,
    scope: SessionDeletionScope,
    plan_hash: ContentHash,
    undo_window_ms: u64,
}

#[derive(Debug, Deserialize)]
struct SessionDeleteUndoParams {
    deletion_id: String,
}

#[derive(Debug, Deserialize)]
struct SessionCompactionCheckpointCreateParams {
    session_id: String,
    checkpoint_id: String,
    #[serde(default)]
    preservation_instructions: Option<String>,
    summary: session_compaction::CompactionSummary,
}

#[derive(Debug, Deserialize)]
struct SessionCompactionCheckpointReadParams {
    session_id: String,
    checkpoint_id: String,
}

#[derive(Debug, Clone, Copy, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
enum PortableSessionCollisionStrategy {
    Reject,
    Copy,
}

impl PortableSessionCollisionStrategy {
    fn as_str(self) -> &'static str {
        match self {
            Self::Reject => "reject",
            Self::Copy => "copy",
        }
    }
}

#[derive(Debug, Deserialize)]
struct PortableSessionExportPreviewParams {
    session_id: String,
}

#[derive(Debug, Deserialize)]
struct PortableSessionExportParams {
    session_id: String,
    package_hash: ContentHash,
}

#[derive(Debug, Deserialize)]
struct PortableSessionImportParams {
    package: PortableSessionPackage,
    #[serde(default)]
    target_project_id: Option<String>,
    collision_strategy: PortableSessionCollisionStrategy,
}

#[derive(Debug, Deserialize)]
struct RetentionPolicyIdentityParams {
    scope_kind: String,
    scope_id: String,
}

#[derive(Debug, Deserialize)]
struct RetentionPolicySetParams {
    scope_kind: String,
    scope_id: String,
    #[serde(default)]
    archive_after_ms: Option<u64>,
    #[serde(default)]
    delete_after_ms: Option<u64>,
    undo_window_ms: u64,
    delete_scope: SessionDeletionScope,
}

#[derive(Debug, Deserialize)]
struct TurnStartParams {
    session_id: String,
    input: String,
    idempotency_key: String,
    #[serde(default)]
    context: Vec<TurnContextItem>,
}

#[derive(Debug, Deserialize)]
struct OperationProbeParams {
    operation_id: String,
    session_id: String,
    kind: OperationKind,
    #[serde(default)]
    event: Option<EventState>,
    #[serde(default)]
    root_id: Option<String>,
    #[serde(default)]
    terminal_id: Option<String>,
    #[serde(default)]
    workspace_snapshot_hash: Option<String>,
    #[serde(default)]
    git_snapshot_hash: Option<String>,
}

#[derive(Debug, Deserialize)]
struct SessionReadParams {
    session_id: String,
    #[serde(default)]
    cursor: Option<String>,
    #[serde(default = "default_session_history_limit")]
    limit: usize,
}

#[derive(Debug, Deserialize)]
struct WorkspacePathParams {
    project_id: String,
    #[serde(default)]
    root_id: Option<String>,
    #[serde(default)]
    path: String,
}

#[derive(Debug, Deserialize)]
struct WorkspaceProjectParams {
    project_id: String,
    #[serde(default)]
    root_id: Option<String>,
}

#[derive(Debug, Deserialize)]
struct WorkspaceGitLogParams {
    project_id: String,
    #[serde(default = "default_git_log_limit")]
    limit: usize,
    #[serde(default)]
    cursor: Option<String>,
}

#[derive(Debug, Deserialize)]
struct WorkspaceGitCommitParams {
    project_id: String,
    oid: String,
}

#[derive(Debug, Deserialize)]
struct WorkspaceGitDiffParams {
    project_id: String,
    scope: String,
    #[serde(default)]
    oid: Option<String>,
    #[serde(default)]
    path: Option<String>,
}

#[derive(Debug, Deserialize)]
struct WorkspaceIndexParams {
    project_id: String,
    #[serde(default)]
    root_id: Option<String>,
    #[serde(default)]
    index_id: String,
}

#[derive(Debug, Deserialize)]
struct WorkspaceIndexCancelParams {
    project_id: String,
    #[serde(default)]
    root_id: Option<String>,
    index_id: String,
}

#[derive(Debug, Deserialize)]
struct WorkspaceSaveParams {
    project_id: String,
    #[serde(default)]
    root_id: Option<String>,
    path: String,
    content: String,
    expected_revision: String,
    encoding: String,
    newline: String,
    origin: String,
}

#[derive(Debug, Deserialize)]
struct WorkspaceWatchParams {
    project_id: String,
    #[serde(default)]
    root_id: Option<String>,
    #[serde(default)]
    paths: Vec<String>,
    #[serde(default)]
    watch_id: Option<String>,
}

#[derive(Debug, Deserialize)]
struct WorkspaceWatchPollParams {
    watch_id: String,
}

#[derive(Debug, Deserialize)]
struct WorkspaceSearchParams {
    project_id: String,
    #[serde(default)]
    root_id: Option<String>,
    search_id: String,
    query: String,
    #[serde(default = "default_search_mode")]
    mode: String,
    #[serde(default)]
    case_sensitive: bool,
    #[serde(default)]
    cursor: Option<String>,
    #[serde(default = "default_search_limit")]
    limit: usize,
}

#[derive(Debug, Deserialize)]
struct WorkspaceSearchCancelParams {
    search_id: String,
    #[serde(default)]
    project_id: Option<String>,
    #[serde(default)]
    root_id: Option<String>,
}

#[derive(Debug, Deserialize)]
struct WorkspaceRepositoryMapParams {
    project_id: String,
    #[serde(default)]
    root_id: Option<String>,
    #[serde(default = "default_repository_map_tokens")]
    token_budget: usize,
    #[serde(default)]
    focus_paths: Vec<String>,
}

#[derive(Debug, Deserialize)]
struct WorkspaceLanguageDocumentParams {
    project_id: String,
    #[serde(default)]
    root_id: Option<String>,
    path: String,
    content: String,
    revision: String,
    #[serde(default)]
    line: usize,
    #[serde(default)]
    column: usize,
}

#[derive(Debug, Deserialize)]
struct ObservedDiagnosticsParams {
    project_id: String,
    #[serde(default)]
    root_id: Option<String>,
    #[serde(default)]
    path: Option<String>,
    #[serde(default = "default_include_stale")]
    include_stale: bool,
}

#[derive(Debug, Deserialize)]
struct RawDiagnosticParams {
    project_id: String,
    #[serde(default)]
    root_id: Option<String>,
    reference: String,
}

#[derive(Debug, Deserialize)]
struct CommandArtifactParams {
    session_id: String,
    reference: String,
}

#[derive(Debug, Deserialize)]
struct WorkspaceEditPreviewParams {
    session_id: String,
    edit: Value,
    contents: Vec<ContentInput>,
}

#[derive(Debug, Deserialize)]
struct WorkspaceEditArtifactParams {
    session_id: String,
    project_id: String,
    edit_id: String,
    reference: String,
    #[serde(default)]
    offset: usize,
    #[serde(default = "default_workspace_edit_page_limit")]
    limit: usize,
}

#[derive(Debug, Deserialize)]
struct TerminalOpenParams {
    session_id: String,
    #[serde(default = "default_terminal_kind")]
    kind: String,
    #[serde(default)]
    name: Option<String>,
    #[serde(default = "default_terminal_rows")]
    rows: u16,
    #[serde(default = "default_terminal_cols")]
    cols: u16,
}

#[derive(Debug, Deserialize)]
struct TerminalReadParams {
    session_id: String,
    terminal_id: String,
    #[serde(default)]
    after: u64,
}

#[derive(Debug, Deserialize)]
struct TerminalInputParams {
    session_id: String,
    terminal_id: String,
    data_base64: String,
}

#[derive(Debug, Deserialize)]
struct TerminalResizeParams {
    session_id: String,
    terminal_id: String,
    rows: u16,
    cols: u16,
}

#[derive(Debug, Deserialize)]
struct TerminalSignalParams {
    session_id: String,
    terminal_id: String,
    signal: String,
}

#[derive(Debug, Deserialize)]
struct TerminalRestartParams {
    session_id: String,
    terminal_id: String,
    #[serde(default)]
    rows: Option<u16>,
    #[serde(default)]
    cols: Option<u16>,
}

#[derive(Debug, Clone, Serialize)]
struct TerminalRecord {
    terminal_id: String,
    session_id: String,
    project_id: String,
    kind: String,
    name: String,
    generation: u64,
    state: String,
    input_policy: String,
    created_at_ms: u64,
    updated_at_ms: u64,
}

impl TerminalState {
    fn stop_user(&mut self, terminal_id: &str, session_id: &str) -> Result<Value, TerminalError> {
        let snapshot = self.manager.close_user(terminal_id, session_id)?;
        if let Some(record) = self.records.get_mut(terminal_id) {
            record.state = "stopping".into();
            record.updated_at_ms = now_ms();
        }
        Ok(self.decorate_snapshot(terminal_id, snapshot))
    }

    fn purge_session(&mut self, session_id: &str) {
        let terminal_ids = self
            .records
            .values()
            .filter(|record| record.session_id == session_id)
            .map(|record| record.terminal_id.clone())
            .collect::<Vec<_>>();
        for terminal_id in terminal_ids {
            let _ = self.manager.remove_user(&terminal_id, session_id);
            self.records.remove(&terminal_id);
        }
    }

    fn validate_open_identity(
        &mut self,
        session_id: &str,
        kind: &str,
        name: &str,
    ) -> Result<(), (i64, String)> {
        let ids = self
            .records
            .values()
            .filter(|record| record.session_id == session_id)
            .map(|record| record.terminal_id.clone())
            .collect::<Vec<_>>();
        for terminal_id in ids {
            if let Err(error) = self.snapshot_value(&terminal_id, session_id, u64::MAX) {
                return Err((error.code, error.message));
            }
        }
        if kind == "foreground"
            && self.records.values().any(|record| {
                record.session_id == session_id
                    && record.kind == "foreground"
                    && matches!(record.state.as_str(), "running" | "stopping")
            })
        {
            return Err((
                -32098,
                "session already has a running foreground terminal".into(),
            ));
        }
        if kind == "background"
            && self.records.values().any(|record| {
                record.session_id == session_id
                    && record.kind == "background"
                    && record.name.to_lowercase() == name.to_lowercase()
            })
        {
            return Err((
                -32099,
                "background terminal name already exists in session".into(),
            ));
        }
        Ok(())
    }

    fn snapshot_value(
        &mut self,
        terminal_id: &str,
        session_id: &str,
        after: u64,
    ) -> Result<Value, TerminalError> {
        let snapshot = self.manager.snapshot(terminal_id, session_id, after)?;
        Ok(self.decorate_snapshot(terminal_id, snapshot))
    }

    fn decorate_snapshot<T: Serialize>(&mut self, terminal_id: &str, snapshot: T) -> Value {
        let mut value = serde_json::to_value(snapshot).expect("terminal snapshot serialization");
        let running = value
            .get("running")
            .and_then(Value::as_bool)
            .unwrap_or(false);
        if let Some(record) = self.records.get_mut(terminal_id) {
            let next_state = if running {
                if record.state == "stopping" {
                    "stopping"
                } else {
                    "running"
                }
            } else {
                "exited"
            };
            if record.state != next_state {
                record.state = next_state.into();
                record.updated_at_ms = now_ms();
            }
            if let (Some(object), Ok(metadata)) =
                (value.as_object_mut(), serde_json::to_value(record))
            {
                if let Some(metadata) = metadata.as_object() {
                    for (key, field) in metadata {
                        object.insert(key.clone(), field.clone());
                    }
                }
            }
        }
        value
    }
}

fn default_search_mode() -> String {
    "all".into()
}

fn default_git_log_limit() -> usize {
    50
}

const fn default_project_list_limit() -> usize {
    50
}

const fn default_session_list_limit() -> usize {
    50
}

const fn default_session_search_limit() -> usize {
    50
}

const fn default_provider_thread_list_limit() -> usize {
    50
}

const fn default_session_history_limit() -> usize {
    100
}

const fn default_search_limit() -> usize {
    50
}

fn contains_case_insensitive(haystack: &str, needle: &str) -> bool {
    haystack
        .to_ascii_lowercase()
        .contains(&needle.to_ascii_lowercase())
}

const fn default_repository_map_tokens() -> usize {
    2_048
}

const fn default_include_stale() -> bool {
    true
}

const fn default_terminal_rows() -> u16 {
    24
}

const fn default_terminal_cols() -> u16 {
    80
}

const fn default_workspace_edit_page_limit() -> usize {
    workspace_edit_preview::MAX_PAGE_BYTES
}

fn bounded_provider_text(value: &str, byte_limit: usize) -> String {
    let redacted = output_redaction::redact_complete(value);
    if redacted.len() <= byte_limit {
        return redacted;
    }
    let mut end = byte_limit;
    while end > 0 && !redacted.is_char_boundary(end) {
        end -= 1;
    }
    redacted[..end].to_owned()
}

fn provider_optional_text(value: Option<&Value>, byte_limit: usize) -> Option<String> {
    value
        .and_then(Value::as_str)
        .map(|value| bounded_provider_text(value, byte_limit))
}

fn provider_thread_source(value: Option<&Value>) -> String {
    match value {
        Some(Value::String(source)) => bounded_provider_text(source, 64),
        Some(Value::Object(source)) => source
            .get("custom")
            .and_then(Value::as_str)
            .map(|custom| bounded_provider_text(custom, 64))
            .unwrap_or_else(|| "sub-agent".into()),
        _ => "unknown".into(),
    }
}

fn provider_thread_status(value: Option<&Value>) -> Value {
    let state = value
        .and_then(|status| status.get("type"))
        .and_then(Value::as_str)
        .map(|state| bounded_provider_text(state, 32))
        .unwrap_or_else(|| "unknown".into());
    let active_flags = value
        .and_then(|status| status.get("activeFlags"))
        .and_then(Value::as_array)
        .map(|flags| {
            flags
                .iter()
                .filter_map(Value::as_str)
                .take(16)
                .map(|flag| bounded_provider_text(flag, 64))
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    json!({ "state": state, "active_flags": active_flags })
}

fn provider_thread_summary(thread: &Value) -> Result<Value, String> {
    let id = thread
        .get("id")
        .and_then(Value::as_str)
        .filter(|id| !id.is_empty())
        .map(|id| bounded_provider_text(id, 256))
        .ok_or_else(|| "Codex thread metadata is missing id".to_owned())?;
    let session_id = thread
        .get("sessionId")
        .and_then(Value::as_str)
        .filter(|id| !id.is_empty())
        .map(|id| bounded_provider_text(id, 256))
        .ok_or_else(|| "Codex thread metadata is missing sessionId".to_owned())?;
    let cwd = thread
        .get("cwd")
        .and_then(Value::as_str)
        .filter(|cwd| !cwd.is_empty())
        .map(|cwd| bounded_provider_text(cwd, 4 * 1024))
        .ok_or_else(|| "Codex thread metadata is missing cwd".to_owned())?;
    let model_provider = thread
        .get("modelProvider")
        .and_then(Value::as_str)
        .filter(|provider| !provider.is_empty())
        .map(|provider| bounded_provider_text(provider, 256))
        .ok_or_else(|| "Codex thread metadata is missing modelProvider".to_owned())?;
    let turns = thread
        .get("turns")
        .and_then(Value::as_array)
        .map_or(0, |turns| turns.len().min(2_000));
    Ok(json!({
        "thread_id": id,
        "provider_session_id": session_id,
        "title": provider_optional_text(thread.get("name"), 512),
        "preview": provider_optional_text(thread.get("preview"), 8 * 1024).unwrap_or_default(),
        "cwd": cwd,
        "model_provider": model_provider,
        "source": provider_thread_source(thread.get("source")),
        "status": provider_thread_status(thread.get("status")),
        "created_at_s": thread.get("createdAt").and_then(Value::as_i64),
        "updated_at_s": thread.get("updatedAt").and_then(Value::as_i64),
        "recency_at_s": thread.get("recencyAt").and_then(Value::as_i64),
        "ephemeral": thread.get("ephemeral").and_then(Value::as_bool).unwrap_or(false),
        "forked_from_thread_id": provider_optional_text(thread.get("forkedFromId"), 256),
        "turn_count": turns
    }))
}

fn provider_turn_summaries(thread: &Value) -> Result<Vec<Value>, String> {
    let turns = thread
        .get("turns")
        .and_then(Value::as_array)
        .ok_or_else(|| "Codex thread read response is missing turns".to_owned())?;
    turns
        .iter()
        .take(2_000)
        .map(|turn| {
            let id = turn
                .get("id")
                .and_then(Value::as_str)
                .filter(|id| !id.is_empty())
                .map(|id| bounded_provider_text(id, 256))
                .ok_or_else(|| "Codex turn metadata is missing id".to_owned())?;
            let status = turn
                .get("status")
                .and_then(Value::as_str)
                .map(|status| bounded_provider_text(status, 32))
                .unwrap_or_else(|| "unknown".into());
            let item_count = turn
                .get("items")
                .and_then(Value::as_array)
                .map_or(0, |items| items.len().min(10_000));
            Ok(json!({
                "turn_id": id,
                "status": status,
                "started_at_s": turn.get("startedAt").and_then(Value::as_i64),
                "completed_at_s": turn.get("completedAt").and_then(Value::as_i64),
                "duration_ms": turn.get("durationMs").and_then(Value::as_i64),
                "item_count": item_count,
                "error_present": turn.get("error").is_some_and(|error| !error.is_null())
            }))
        })
        .collect()
}

fn runtime_error_content(message: &str) -> String {
    bounded_provider_text(message, 8 * 1024)
}

fn runtime_error_classification(message: &str) -> (&'static str, bool) {
    let normalized = message.to_ascii_lowercase();
    if normalized.contains("timeout") || normalized.contains("timed out") {
        ("timeout", true)
    } else if normalized.contains("transport")
        || normalized.contains("network")
        || normalized.contains("connection")
        || normalized.contains("stream")
        || normalized.contains("closed its output channel")
        || normalized.contains("cannot read codex app server output")
        || normalized.contains("cannot write to codex app server")
    {
        ("transport", true)
    } else if normalized.contains("budget")
        || normalized.contains("context window")
        || normalized.contains("token limit")
        || normalized.contains("quota")
    {
        ("budget", false)
    } else if normalized.contains("protocol")
        || normalized.contains("json-rpc")
        || normalized.contains("jsonrpc")
        || normalized.contains("handshake")
        || normalized.contains("invalid params")
        || normalized.contains("unsupported aap")
    {
        ("protocol", false)
    } else if normalized.contains("sandbox")
        || normalized.contains("sandboxed")
        || normalized.contains("outside sandbox")
    {
        ("sandbox", false)
    } else if normalized.contains("policy")
        || normalized.contains("not allowed")
        || normalized.contains("permission denied")
        || normalized.contains("approval required")
        || normalized.contains("read-only")
    {
        ("policy", false)
    } else if normalized.contains("git")
        || normalized.contains("branch")
        || normalized.contains("merge")
        || normalized.contains("rebase")
        || normalized.contains("index.lock")
    {
        ("git", false)
    } else if normalized.contains("workspace")
        || normalized.contains("file path")
        || normalized.contains("root is")
        || normalized.contains("symlink")
        || normalized.contains("file revision")
    {
        ("workspace", false)
    } else if normalized.contains("tool")
        || normalized.contains("command execution")
        || normalized.contains("command failed")
        || normalized.contains("terminal")
    {
        ("tool", false)
    } else if normalized.contains("rate limit")
        || normalized.contains("429")
        || normalized.contains("503")
        || normalized.contains("temporarily")
    {
        ("provider", true)
    } else if normalized.contains("provider") || normalized.contains("model") {
        ("provider", false)
    } else if normalized.contains("persist")
        || normalized.contains("database")
        || normalized.contains("sqlite")
        || normalized.contains("storage")
        || normalized.contains("blob")
    {
        ("storage", false)
    } else {
        ("adapter", false)
    }
}

fn runtime_error_data(message: &str) -> Value {
    let (class, retryable) = runtime_error_classification(message);
    json!({
        "schema_version": "runtime-error/0.1",
        "class": class,
        "retryable": retryable
    })
}

fn default_terminal_kind() -> String {
    "foreground".into()
}

fn default_primary_root_id() -> String {
    "root-1".into()
}

fn parse_session_history_cursor(cursor: Option<&str>) -> Result<Option<u64>, String> {
    let Some(cursor) = cursor else {
        return Ok(None);
    };
    let Some(sequence) = cursor.strip_prefix("before:") else {
        return Err("session history cursor is invalid".into());
    };
    let parsed = sequence
        .parse::<u64>()
        .map_err(|_| "session history cursor is invalid".to_owned())?;
    if parsed == 0 || parsed.to_string() != sequence {
        return Err("session history cursor is invalid".into());
    }
    Ok(Some(parsed))
}

fn session_history_window(
    latest_sequence: u64,
    before_sequence: Option<u64>,
    limit: usize,
) -> Result<(u64, usize), String> {
    if limit == 0 || limit > 200 {
        return Err("session history limit must be between 1 and 200".into());
    }
    let latest_exclusive = latest_sequence
        .checked_add(1)
        .ok_or_else(|| "session history sequence is invalid".to_owned())?;
    let end_exclusive = before_sequence.unwrap_or(latest_exclusive);
    if end_exclusive == 0 || end_exclusive > latest_exclusive {
        return Err("session history cursor is stale or out of range".into());
    }
    let end_sequence = end_exclusive - 1;
    let page_len = usize::try_from(end_sequence.min(limit as u64))
        .map_err(|_| "session history page is invalid".to_owned())?;
    Ok((end_sequence.saturating_sub(page_len as u64), page_len))
}

fn timeline_item_value(item: &TimelineItem, sequence: u64) -> Value {
    let mut value = serde_json::to_value(item).expect("timeline item serialization");
    if let Some(object) = value.as_object_mut() {
        object.insert("sequence".into(), json!(sequence));
    }
    value
}

#[cfg(unix)]
fn filesystem_root_identity(path: &Path) -> Result<String, String> {
    use std::os::unix::fs::MetadataExt;
    let metadata = fs::metadata(path).map_err(|error| error.to_string())?;
    Ok(format!("fs:unix:{}:{}", metadata.dev(), metadata.ino()))
}

#[cfg(windows)]
fn filesystem_root_identity(path: &Path) -> Result<String, String> {
    use std::os::windows::fs::MetadataExt;
    let metadata = fs::metadata(path).map_err(|error| error.to_string())?;
    Ok(format!(
        "fs:windows:{}:{}",
        metadata.volume_serial_number(),
        metadata.file_index()
    ))
}

#[cfg(not(any(unix, windows)))]
fn filesystem_root_identity(path: &Path) -> Result<String, String> {
    fs::metadata(path).map_err(|error| error.to_string())?;
    Ok(format!(
        "path:sha256:{}",
        ContentHash::for_bytes(path.to_string_lossy().as_bytes()).sha256
    ))
}

#[derive(Default)]
struct TrustReviewScan {
    repositories: Vec<Value>,
    instructions: Vec<Value>,
    hooks: Vec<Value>,
    scanned_entries: usize,
    truncated: bool,
}

fn trust_relative_path(root: &Path, path: &Path) -> String {
    path.strip_prefix(root)
        .ok()
        .map(|relative| {
            let value = relative.to_string_lossy().replace('\\', "/");
            if value.is_empty() {
                ".".to_owned()
            } else {
                value
            }
        })
        .unwrap_or_else(|| ".".to_owned())
}

fn trust_is_executable(metadata: &fs::Metadata, path: &Path) -> bool {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = path;
        metadata.permissions().mode() & 0o111 != 0
    }
    #[cfg(not(unix))]
    {
        let extension = path
            .extension()
            .and_then(|value| value.to_str())
            .unwrap_or_default();
        matches!(
            extension.to_ascii_lowercase().as_str(),
            "bat" | "cmd" | "com" | "exe" | "ps1"
        )
    }
}

fn trust_file_hash(path: &Path, metadata: &fs::Metadata) -> Option<Value> {
    let size = metadata.len();
    if size > MAX_TRUST_REVIEW_HOOK_BYTES as u64 {
        return Some(json!({
            "bytes": size,
            "hash": Value::Null,
            "hash_state": "not-read-size-limit"
        }));
    }
    let bytes = fs::read(path).ok()?;
    let hash = ContentHash::for_bytes(&bytes);
    Some(json!({
        "bytes": size,
        "hash": format!("sha256:{}", hash.sha256),
        "hash_state": "observed"
    }))
}

fn trust_add_repository(root: &Path, directory: &Path, scan: &mut TrustReviewScan) {
    if scan.repositories.len() >= MAX_TRUST_REVIEW_ENTRIES {
        scan.truncated = true;
        return;
    }
    let git = directory.join(".git");
    let Ok(metadata) = fs::symlink_metadata(&git) else {
        return;
    };
    if metadata.file_type().is_symlink() || !(metadata.is_dir() || metadata.is_file()) {
        return;
    }
    let kind = if metadata.is_dir() {
        "directory"
    } else {
        "file"
    };
    scan.repositories.push(json!({
        "relative_path": trust_relative_path(root, directory),
        "metadata_path": trust_relative_path(root, &git),
        "kind": kind,
        "trust_state": "unreviewed"
    }));
}

fn trust_scan_hooks(root: &Path, repository: &Path, scan: &mut TrustReviewScan) {
    let git = repository.join(".git");
    let mut directories = Vec::new();
    if fs::symlink_metadata(&git).is_ok_and(|metadata| metadata.is_dir()) {
        directories.push((git.join("hooks"), "repository-hook"));
    }
    directories.push((repository.join(".aegisy").join("hooks"), "project-hook"));
    directories.push((repository.join(".husky"), "project-hook"));
    for (directory, scope) in directories {
        let Ok(entries) = fs::read_dir(&directory) else {
            continue;
        };
        for entry in entries.flatten() {
            if scan.hooks.len() >= MAX_TRUST_REVIEW_ENTRIES {
                scan.truncated = true;
                return;
            }
            let path = entry.path();
            let Ok(metadata) = fs::symlink_metadata(&path) else {
                continue;
            };
            if metadata.file_type().is_symlink() || !metadata.is_file() {
                continue;
            }
            let executable = trust_is_executable(&metadata, &path);
            if scope == "repository-hook"
                && !executable
                && path.extension().and_then(|value| value.to_str()) != Some("sample")
            {
                continue;
            }
            let mut hook = json!({
                "relative_path": trust_relative_path(root, &path),
                "scope": scope,
                "executable": executable,
                "trust_state": "untrusted",
                "execution": "disabled"
            });
            if let Some(metadata_value) = trust_file_hash(&path, &metadata) {
                hook.as_object_mut()
                    .expect("trust hook object")
                    .insert("content".into(), metadata_value);
            }
            scan.hooks.push(hook);
        }
    }
}

fn trust_scan_directory(root: &Path, directory: &Path, depth: usize, scan: &mut TrustReviewScan) {
    if depth > MAX_TRUST_REVIEW_DEPTH || scan.truncated {
        return;
    }
    trust_add_repository(root, directory, scan);
    trust_scan_hooks(root, directory, scan);
    let Ok(entries) = fs::read_dir(directory) else {
        return;
    };
    for entry in entries.flatten() {
        if scan.scanned_entries >= MAX_TRUST_REVIEW_ENTRIES {
            scan.truncated = true;
            return;
        }
        scan.scanned_entries += 1;
        let path = entry.path();
        let Ok(metadata) = fs::symlink_metadata(&path) else {
            continue;
        };
        if metadata.file_type().is_symlink() {
            continue;
        }
        if metadata.is_file() {
            let name = path
                .file_name()
                .and_then(|value| value.to_str())
                .unwrap_or_default();
            let relative = trust_relative_path(root, &path);
            let is_instruction = TRUST_INSTRUCTION_NAMES.iter().any(|candidate| {
                name.eq_ignore_ascii_case(candidate)
                    || (candidate == &"copilot-instructions.md"
                        && relative.ends_with(".github/copilot-instructions.md"))
            });
            if is_instruction {
                if scan.instructions.len() < MAX_TRUST_REVIEW_ENTRIES {
                    let mut instruction = json!({
                        "relative_path": relative,
                        "bytes": metadata.len(),
                        "trust_state": "untrusted",
                        "content": "not-read"
                    });
                    if let Some(identity) = trust_file_hash(&path, &metadata) {
                        instruction
                            .as_object_mut()
                            .expect("trust instruction object")
                            .insert("content_identity".into(), identity);
                    }
                    scan.instructions.push(instruction);
                } else {
                    scan.truncated = true;
                }
            }
        } else if metadata.is_dir() {
            let name = path
                .file_name()
                .and_then(|value| value.to_str())
                .unwrap_or_default();
            if matches!(name, ".git" | "node_modules" | "target" | ".venv" | "venv") {
                continue;
            }
            trust_scan_directory(root, &path, depth + 1, scan);
        }
    }
}

fn project_trust_review(root: &Path, root_identity: &str) -> Result<Value, String> {
    let canonical = root
        .canonicalize()
        .map_err(|error| format!("cannot resolve trust review root: {error}"))?;
    if !canonical.is_dir() {
        return Err("trust review root is not a directory".into());
    }
    let mut scan = TrustReviewScan::default();
    trust_scan_directory(&canonical, &canonical, 0, &mut scan);
    let mut review = json!({
        "schema_version": "project-trust-review/0.1",
        "resolved_root": canonical,
        "root_identity": root_identity,
        "roots": [{
            "relative_path": ".",
            "access": "write",
            "symlink_policy": "deny-components",
            "trust_state": "unreviewed"
        }],
        "repositories": scan.repositories,
        "instructions": scan.instructions,
        "executable_hooks": scan.hooks,
        "scan": {
            "scanned_entries": scan.scanned_entries,
            "truncated": scan.truncated
        },
        "policy_impact": {
            "agent_execution": "read-only",
            "workspace_write": "user-save-only",
            "executable_content": "blocked-until-trusted-and-policy-allowed",
            "hooks": "discovered-only-never-executed",
            "sensitive_paths": "filtered",
            "network": "not-granted"
        },
        "trust_state": "unreviewed",
        "invalidation_reason": null,
        "acknowledgement": null,
        "required": true
    });
    let hash_input = serde_json::to_vec(&review).map_err(|error| error.to_string())?;
    let hash = ContentHash::for_bytes(&hash_input);
    review.as_object_mut().expect("trust review object").insert(
        "review_id".into(),
        format!("project-trust-review:sha256:{}", hash.sha256).into(),
    );
    Ok(review)
}

fn canonical_project_root(path: &Path) -> Result<PathBuf, String> {
    if !path.is_dir() {
        return Err("project root is not a directory".into());
    }
    if fs::symlink_metadata(path)
        .map(|metadata| metadata.file_type().is_symlink())
        .unwrap_or(false)
    {
        return Err("project root is a symlink".into());
    }
    let canonical = path
        .canonicalize()
        .map_err(|error| format!("cannot canonicalize project root: {error}"))?;
    if !canonical.is_absolute() {
        return Err("project root canonical path is not absolute".into());
    }
    Ok(canonical)
}

fn project_root_value(root: &workbench_store::StoredProjectRoot) -> Value {
    let path = Path::new(&root.canonical_root);
    json!({
        "root_id": root.root_id,
        "canonical_path": root.canonical_root,
        "root_identity": root.root_identity,
        "access": root.access,
        "availability": if path.is_dir() { "available" } else { "unavailable" },
        "relink_required": !path.is_dir()
    })
}

fn project_navigation_value(entry: StoredProjectNavigationEntry) -> Value {
    json!({
        "project_id": entry.project.project_id,
        "id": entry.project.project_id,
        "root": entry.project.canonical_root,
        "name": entry.project.display_name,
        "state": entry.project.state,
        "root_identity": entry.project.root_identity,
        "created_at_ms": entry.project.created_at_ms,
        "updated_at_ms": entry.project.updated_at_ms,
        "pinned": entry.navigation.pinned,
        "last_opened_at_ms": entry.navigation.last_opened_at_ms,
        "availability": entry.availability,
        "relink_required": entry.relink_required,
        "session_count": entry.session_count,
        "active_session_count": entry.active_session_count,
        "live_session_count": entry.live_session_count
    })
}

fn stored_timeline_item(item: StoredItem) -> TimelineItem {
    let runtime_payload = item.payload.as_object().is_some_and(|payload| {
        payload.get("content").is_some_and(Value::is_string)
            && payload
                .keys()
                .all(|key| matches!(key.as_str(), "content" | "data"))
    });
    let content = item
        .payload
        .get("content")
        .or_else(|| item.payload.get("text"))
        .or_else(|| item.payload.get("output"))
        .or_else(|| item.payload.get("diff"))
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_owned();
    let data = if runtime_payload {
        item.payload
            .get("data")
            .cloned()
            .filter(|value| !value.is_null())
    } else if item.payload.is_null() {
        None
    } else {
        Some(item.payload)
    };
    TimelineItem {
        id: item.item_id,
        kind: item.item_kind,
        role: item.role,
        state: item.state,
        content,
        data,
    }
}

fn hydrate_runtime_session_items(
    store: &WorkbenchStore,
    session_id: &str,
) -> Result<Vec<TimelineItem>, String> {
    let mut items = Vec::new();
    let mut after_sequence = 0;
    while items.len() < MAX_RUNTIME_REPLAY_ITEMS {
        let limit = (MAX_RUNTIME_REPLAY_ITEMS - items.len()).min(RUNTIME_REPLAY_PAGE_SIZE);
        let page = store
            .read_session_items(session_id, after_sequence, limit)
            .map_err(|error| error.message)?;
        if page.is_empty() {
            break;
        }
        let next_sequence = page
            .last()
            .map(|item| item.sequence)
            .ok_or_else(|| "session replay page unexpectedly empty".to_owned())?;
        if next_sequence <= after_sequence {
            return Err("session replay did not advance its sequence".to_owned());
        }
        after_sequence = next_sequence;
        let page_len = page.len();
        items.extend(page.into_iter().map(stored_timeline_item));
        if page_len < limit {
            break;
        }
    }
    Ok(items)
}

fn session_history_page(
    first_sequence: Option<u64>,
    last_sequence: Option<u64>,
    latest_sequence: u64,
    limit: usize,
) -> Value {
    let has_older = first_sequence.is_some_and(|sequence| sequence > 1);
    json!({
        "limit": limit,
        "first_sequence": first_sequence,
        "last_sequence": last_sequence,
        "latest_sequence": latest_sequence,
        "has_older": has_older,
        "older_cursor": first_sequence
            .filter(|sequence| *sequence > 1)
            .map(|sequence| format!("before:{sequence}"))
    })
}

fn find_compaction_checkpoint_event(
    store: &WorkbenchStore,
    session_id: &str,
    descriptor: &session_compaction_store::CompactionCheckpointDescriptor,
    source_event_count: u64,
) -> Result<workbench_store::WorkbenchEvent, String> {
    let mut after_sequence = 0_u64;
    let mut inspected = 0_u64;
    while inspected < source_event_count {
        let remaining = usize::try_from((source_event_count - inspected).min(200))
            .map_err(|_| "compaction checkpoint event page is invalid".to_owned())?;
        let page = store
            .read_session_events(session_id, after_sequence, remaining)
            .map_err(|error| error.message)?;
        if page.is_empty() {
            break;
        }
        for event in &page {
            if event.event_kind == "session.compaction-checkpointed"
                && event.operation_id == descriptor.checkpoint_id
                && event.correlation_id == descriptor.review_id
                && event.payload["session_id"] == session_id
                && event.payload["checkpoint_id"] == descriptor.checkpoint_id
                && event.payload["review_id"] == descriptor.review_id
                && event.payload["object_reference"] == descriptor.object_reference
                && event.payload["state"] == "review-persisted"
                && event.payload["original_event_history_authoritative"] == true
            {
                return Ok(event.clone());
            }
        }
        inspected = inspected.saturating_add(page.len() as u64);
        after_sequence = page
            .last()
            .map(|event| event.sequence)
            .ok_or_else(|| "compaction checkpoint event page is empty".to_owned())?;
    }
    Err("compaction checkpoint event is missing".into())
}

fn normalize_terminal_kind(kind: &str) -> Result<String, (i64, String)> {
    match kind {
        "foreground" | "background" => Ok(kind.to_owned()),
        _ => Err((
            -32602,
            "terminal kind must be foreground or background".into(),
        )),
    }
}

fn normalize_terminal_name(kind: &str, name: Option<&str>) -> Result<String, (i64, String)> {
    let name = name.map(str::trim).filter(|name| !name.is_empty());
    if kind == "background" && name.is_none() {
        return Err((-32602, "background terminal requires a name".into()));
    }
    let name = name.unwrap_or("Terminal");
    if name.chars().count() > 64 || name.len() > 256 || name.chars().any(char::is_control) {
        return Err((
            -32602,
            "terminal name must contain 1 to 64 printable characters".into(),
        ));
    }
    Ok(name.to_owned())
}

impl Default for Runtime {
    fn default() -> Self {
        Self::with_backend(Backend::Preview)
    }
}

impl Runtime {
    pub fn with_codex() -> Result<Self, String> {
        CodexAdapter::start().map(|adapter| Self::with_backend(Backend::Codex(adapter)))
    }

    pub fn with_store(data_root: &Path) -> Result<Self, String> {
        match WorkbenchStore::open_or_recover(data_root).map_err(|cause| cause.message)? {
            WorkbenchStoreOpen::Writable(store) => {
                let compaction_store =
                    session_compaction_store::CompactionCheckpointStore::open(data_root).ok();
                Ok(Self::with_backend_and_store(
                    Backend::Preview,
                    Some(store),
                    compaction_store,
                ))
            }
            WorkbenchStoreOpen::ReadOnlyRecovery(diagnostic) => {
                Ok(Self::with_backend(Backend::Recovery(diagnostic)))
            }
        }
    }

    pub fn with_codex_and_store(data_root: &Path) -> Result<Self, String> {
        match WorkbenchStore::open_or_recover(data_root).map_err(|cause| cause.message)? {
            WorkbenchStoreOpen::Writable(store) => {
                let adapter = CodexAdapter::start()?;
                let compaction_store =
                    session_compaction_store::CompactionCheckpointStore::open(data_root).ok();
                Ok(Self::with_backend_and_store(
                    Backend::Codex(adapter),
                    Some(store),
                    compaction_store,
                ))
            }
            WorkbenchStoreOpen::ReadOnlyRecovery(diagnostic) => {
                Ok(Self::with_backend(Backend::Recovery(diagnostic)))
            }
        }
    }

    pub fn unavailable(error: impl Into<String>) -> Self {
        Self::with_backend(Backend::Unavailable(error.into()))
    }

    fn with_backend(backend: Backend) -> Self {
        Self::with_backend_and_store(backend, None, None)
    }

    fn with_backend_and_store(
        backend: Backend,
        workbench_store: Option<WorkbenchStore>,
        compaction_store: Option<session_compaction_store::CompactionCheckpointStore>,
    ) -> Self {
        let projects = workbench_store
            .as_ref()
            .and_then(|store| store.list_projects().ok())
            .unwrap_or_default()
            .into_iter()
            .filter(|stored| stored.state == "active")
            .map(|stored| {
                let project = Project {
                    id: stored.project_id,
                    root: stored.canonical_root,
                    name: stored.display_name,
                };
                (project.id.clone(), project)
            })
            .collect();
        let project_roots = workbench_store
            .as_ref()
            .and_then(|store| store.list_projects().ok())
            .unwrap_or_default()
            .into_iter()
            .filter(|stored| stored.state == "active")
            .filter_map(|stored| {
                workbench_store
                    .as_ref()
                    .and_then(|store| store.load_project_roots(&stored.project_id).ok())
                    .map(|roots| (stored.project_id, roots))
            })
            .collect();
        let project_navigation = workbench_store
            .as_ref()
            .and_then(|store| store.list_project_navigation(256).ok())
            .unwrap_or_default()
            .into_iter()
            .map(|entry| (entry.project.project_id.clone(), entry.navigation))
            .collect();
        let operation_reconciliations = workbench_store
            .as_ref()
            .and_then(|store| store.load_operation_reconciliations().ok())
            .unwrap_or_default()
            .into_iter()
            .map(|record| {
                (
                    Self::operation_reconciliation_key(
                        &record.input.session_id,
                        &record.input.operation_id,
                    ),
                    record.result,
                )
            })
            .collect();
        Self {
            initialized: false,
            client_ready: false,
            shutdown: false,
            sequence: 0,
            next_id: 0,
            control: RuntimeControl::default(),
            projects,
            project_roots,
            project_navigation,
            project_trust_acknowledgements: HashMap::new(),
            sessions: HashMap::new(),
            workspace_watches: HashMap::new(),
            workspace_indexes: HashMap::new(),
            language_servers: LanguageServerManager::default(),
            diagnostic_store: DiagnosticStore::default(),
            command_artifacts: command_artifact::CommandArtifactStore::default(),
            workspace_edit_previews: WorkspaceEditPreviewStore::default(),
            cancelled_workspace_searches: HashSet::new(),
            cancelled_workspace_indexes: HashSet::new(),
            archived_sessions: HashSet::new(),
            operation_reconciliations,
            workbench_store,
            compaction_store,
            backend,
        }
    }

    pub fn handle_line(&mut self, line: &str) -> Vec<Value> {
        let mut messages = Vec::new();
        self.handle_line_stream(line, |message| messages.push(message));
        messages
    }

    pub fn control(&self) -> RuntimeControl {
        self.control.clone()
    }

    pub fn handle_line_stream<F>(&mut self, line: &str, mut emit: F)
    where
        F: FnMut(Value),
    {
        let request: Request = match serde_json::from_str(line) {
            Ok(request) => request,
            Err(error) => {
                emit(
                    serde_json::to_value(Response::error(
                        Value::Null,
                        -32700,
                        format!("parse error: {error}"),
                    ))
                    .expect("response serialization"),
                );
                return;
            }
        };

        if request.jsonrpc != JSONRPC_VERSION {
            self.emit_all(
                self.error_for(&request, -32600, "invalid JSON-RPC version"),
                &mut emit,
            );
            return;
        }
        if let Some(duplicate) = self.control.claim_request(&request) {
            emit(duplicate);
            return;
        }

        let quarantined_session = self.initialized
            && self.client_ready
            && !self.is_recovery_mode()
            && !matches!(
                request.method.as_str(),
                "session/read"
                    | "session/recovery/status"
                    | "runtime/projection-recovery/status"
                    | "operation/probe"
                    | "turn/cancel"
                    | "turn/steer"
                    | "terminal/stop-user"
                    | "terminal/close-user"
                    | "terminal/remove-user"
            )
            && request
                .params
                .get("session_id")
                .and_then(Value::as_str)
                .is_some_and(|session_id| {
                    self.workbench_store
                        .as_ref()
                        .is_some_and(|store| store.session_requires_recovery(session_id))
                });
        if quarantined_session {
            self.emit_all(
                self.error_for(
                    &request,
                    -32115,
                    "session is in read-only recovery; mutation is disabled",
                ),
                &mut emit,
            );
            return;
        }

        let deletion_state = self
            .workbench_store
            .as_ref()
            .and_then(|store| {
                request
                    .params
                    .get("session_id")
                    .and_then(Value::as_str)
                    .and_then(|session_id| store.session_deletion_for_session(session_id).ok())
                    .flatten()
            })
            .map(|receipt| receipt.state);
        if deletion_state.as_deref() == Some("purged")
            && request.method != "session/deletion/status"
        {
            self.emit_all(
                self.error_for(&request, -32023, "session not found"),
                &mut emit,
            );
            return;
        }
        let blocked_operation = if matches!(
            request.method.as_str(),
            "operation/reconcile" | "operation/probe"
        ) {
            None
        } else if let Some(session_id) = request.params.get("session_id").and_then(Value::as_str) {
            if let Some(store) = self.workbench_store.as_ref() {
                match store.session_blocked_operation(session_id) {
                    Ok(operation) => operation.map(|operation| operation.result),
                    Err(error) => {
                        self.emit_all(
                            self.error_for(
                                &request,
                                -32114,
                                format!(
                                    "cannot inspect operation reconciliation: {}",
                                    error.message
                                ),
                            ),
                            &mut emit,
                        );
                        return;
                    }
                }
            } else {
                self.operation_reconciliations
                    .values()
                    .find(|result| result.session_id == session_id && result.writes_blocked)
                    .cloned()
            }
        } else {
            None
        };
        if blocked_operation.is_some()
            && !matches!(
                request.method.as_str(),
                "session/read"
                    | "session/recovery/status"
                    | "session/deletion/status"
                    | "turn/cancel"
                    | "turn/steer"
                    | "terminal/list"
                    | "terminal/read"
                    | "terminal/attach"
                    | "terminal/stop-user"
                    | "terminal/close-user"
                    | "terminal/remove-user"
                    | "artifact/read-command-output"
                    | "workspace/edit/artifact/read"
            )
        {
            self.emit_all(
                self.error_for(
                    &request,
                    -32132,
                    "session has an unknown or unreconciled operation; mutation is blocked",
                ),
                &mut emit,
            );
            return;
        }
        let pending_deletion_read = matches!(
            request.method.as_str(),
            "session/read"
                | "session/deletion/status"
                | "session/delete/preview"
                | "session/export/preview"
                | "session/export"
                | "session/compaction/checkpoint/read"
                | "session/recovery/status"
                | "turn/cancel"
                | "turn/steer"
                | "terminal/list"
                | "terminal/read"
                | "terminal/attach"
                | "terminal/stop-user"
                | "terminal/close-user"
                | "terminal/remove-user"
                | "artifact/read-command-output"
                | "workspace/edit/artifact/read"
        );
        if deletion_state.as_deref() == Some("pending") && !pending_deletion_read {
            self.emit_all(
                self.error_for(
                    &request,
                    -32131,
                    "session deletion is pending; mutation is disabled",
                ),
                &mut emit,
            );
            return;
        }

        if request.method == "turn/start"
            && self.initialized
            && self.client_ready
            && self.is_recovery_mode()
        {
            self.emit_all(
                self.error_for(&request, -32120, "workbench is in read-only recovery mode"),
                &mut emit,
            );
            return;
        }
        if request.method == "turn/start" && self.initialized && self.client_ready {
            self.turn_start_stream(request, &mut emit);
            return;
        }

        let messages = match request.method.as_str() {
            "initialize" => self.initialize(request),
            "initialized" => {
                if self.initialized {
                    self.client_ready = true;
                    self.control.set_protocol_ready(true);
                }
                Vec::new()
            }
            "shutdown" => self.shutdown(request),
            _ if !self.initialized || !self.client_ready => {
                self.error_for(&request, -32002, "initialize handshake required")
            }
            "runtime/recovery/status" => self.recovery_diagnostic(request, false),
            "runtime/recovery/export" => self.recovery_diagnostic(request, true),
            "runtime/restart" => self.runtime_restart(request),
            "runtime/health" => self.runtime_health(request),
            "runtime/degradations" => self.runtime_degradations(request),
            _ if self.is_recovery_mode() => {
                self.error_for(&request, -32120, "workbench is in read-only recovery mode")
            }
            "project/trust-review" => self.project_trust_review(request),
            "project/trust-acknowledge" => self.project_trust_acknowledge(request),
            "project/root-list" => self.project_root_list(request),
            "project/root-add" => self.project_root_add(request),
            "project/root-remove" => self.project_root_remove(request),
            "project/relink" => self.project_relink(request),
            "project/list" => self.project_list(request),
            "project/navigation" => self.project_navigation(request),
            "project/open" => self.project_open(request),
            "session/archive" => self.session_archive(request),
            "session/delete/preview" => self.session_delete_preview(request),
            "session/delete/schedule" => self.session_delete_schedule(request),
            "session/delete/undo" => self.session_delete_undo(request),
            "session/deletion/status" => self.session_deletion_status(request),
            "session/compaction/checkpoint/create" => {
                self.session_compaction_checkpoint_create(request)
            }
            "session/compaction/checkpoint/read" => {
                self.session_compaction_checkpoint_read(request)
            }
            "session/export/preview" => self.session_export_preview(request),
            "session/export" => self.session_export(request),
            "session/import/preview" => self.session_import_preview(request),
            "session/import" => self.session_import(request),
            "session/list" => self.session_list(request),
            "session/search" => self.session_search(request),
            "operation/probe" => self.operation_probe(request),
            "operation/reconcile" => self.operation_reconcile(request),
            "session/recovery/status" => self.session_recovery_status(request),
            "runtime/projection-recovery/status" => self.projection_recovery_status(request),
            "session/start" => self.session_start(request),
            "session/resume" => self.session_resume(request),
            "session/fork" => self.session_fork(request),
            "session/read" => self.session_read(request),
            "session/title" => self.session_title(request),
            "session/unarchive" => self.session_unarchive(request),
            "session/provider-list" => self.provider_thread_list(request),
            "session/provider-read" => self.provider_thread_read(request),
            "retention/policy/read" => self.retention_policy_read(request),
            "retention/policy/remove" => self.retention_policy_remove(request),
            "retention/policy/set" => self.retention_policy_set(request),
            "retention/maintenance/run" => self.retention_maintenance_run(request),
            "turn/cancel" => self.control.cancel_claimed(request),
            "turn/steer" => self.control.steer_claimed(request),
            "workspace/list" => self.workspace_list(request),
            "workspace/read" => self.workspace_read(request),
            "workspace/save-user-text" => self.workspace_save_user_text(request),
            "workspace/metadata" => self.workspace_metadata(request),
            "workspace/watch" => self.workspace_watch(request),
            "workspace/watch/poll" => self.workspace_watch_poll(request),
            "workspace/git-status" => self.workspace_git_status(request),
            "workspace/git/overview" => self.workspace_git_overview(request),
            "workspace/git/log" => self.workspace_git_log(request),
            "workspace/git/commit" => self.workspace_git_commit(request),
            "workspace/git/diff" => self.workspace_git_diff(request),
            "workspace/search" => self.workspace_search(request),
            "workspace/search/cancel" => self.workspace_search_cancel(request),
            "workspace/index" => self.workspace_index(request),
            "workspace/index/cancel" => self.workspace_index_cancel(request),
            "workspace/repository-map" => self.workspace_repository_map(request),
            "workspace/language-servers" => self.workspace_language_servers(request),
            "workspace/language-server/start" => self.workspace_language_server_start(request),
            "workspace/language-server/stop" => self.workspace_language_server_stop(request),
            "workspace/definition" => self.workspace_definition(request),
            "workspace/references" => self.workspace_references(request),
            "workspace/diagnostics" => self.workspace_diagnostics(request),
            "workspace/observed-diagnostics" => self.workspace_observed_diagnostics(request),
            "workspace/diagnostics/raw" => self.workspace_diagnostic_raw(request),
            "artifact/read-command-output" => self.command_artifact_read(request),
            "workspace/edit/preview" => self.workspace_edit_preview(request),
            "workspace/edit/artifact/read" => self.workspace_edit_artifact_read(request),
            "terminal/open-user" => self.terminal_open_user(request),
            "terminal/read" => self.terminal_read(request),
            "terminal/attach" => self.terminal_read(request),
            "terminal/list" => self.terminal_list(request),
            "terminal/input-user" => self.terminal_input_user(request),
            "terminal/resize" => self.terminal_resize(request),
            "terminal/signal-user" => self.terminal_signal_user(request),
            "terminal/close-user" => self.terminal_close_user(request),
            "terminal/stop-user" => self.terminal_close_user(request),
            "terminal/restart-user" => self.terminal_restart_user(request),
            "terminal/remove-user" => self.terminal_remove_user(request),
            _ => self.error_for(&request, -32601, "method not found"),
        };
        self.emit_all(messages, &mut emit);
    }

    pub fn should_shutdown(&self) -> bool {
        self.shutdown
    }

    fn is_recovery_mode(&self) -> bool {
        matches!(&self.backend, Backend::Recovery(_))
    }

    fn runtime_health(&mut self, request: Request) -> Vec<Value> {
        let result = match &mut self.backend {
            Backend::Codex(adapter) => {
                let health = adapter.health();
                let state = health.state.clone();
                let restart_required = matches!(state.as_str(), "exited" | "unknown");
                json!({
                    "schema_version": "runtime-health/0.1",
                    "state": state,
                    "backend": "codex-app-server",
                    "process_id": health.process_id,
                    "exit_code": health.exit_code,
                    "stderr": health.stderr,
                    "restart_required": restart_required
                })
            }
            Backend::Preview => json!({
                "schema_version": "runtime-health/0.1",
                "state": "ready",
                "backend": "preview",
                "restart_required": false
            }),
            Backend::Recovery(_) => json!({
                "schema_version": "runtime-health/0.1",
                "state": "read-only-recovery",
                "backend": "workbench-store",
                "restart_required": false
            }),
            Backend::Unavailable(_) => json!({
                "schema_version": "runtime-health/0.1",
                "state": "unavailable",
                "backend": "codex-app-server",
                "restart_required": true
            }),
        };
        self.success_for(&request, result)
    }

    fn runtime_restart(&mut self, request: Request) -> Vec<Value> {
        if self.is_recovery_mode() {
            return self.error_for(&request, -32120, "workbench is in read-only recovery mode");
        }
        if self.control.has_active_work() {
            return self.error_for(
                &request,
                -32081,
                "runtime restart is blocked while a turn or user terminal is active",
            );
        }

        let backend = std::mem::replace(
            &mut self.backend,
            Backend::Unavailable("Codex runtime restart in progress".into()),
        );
        match backend {
            Backend::Codex(mut adapter) => {
                if adapter.health().state == "running" {
                    self.backend = Backend::Codex(adapter);
                    return self.error_for(
                        &request,
                        -32082,
                        "Codex App Server is still running; restart is not required",
                    );
                }
                drop(adapter);
            }
            Backend::Unavailable(_) => {}
            Backend::Preview => {
                self.backend = Backend::Preview;
                return self.error_for(
                    &request,
                    -32083,
                    "preview runtime has no adapter to restart",
                );
            }
            Backend::Recovery(diagnostic) => {
                self.backend = Backend::Recovery(diagnostic);
                return self.error_for(&request, -32120, "workbench is in read-only recovery mode");
            }
        }

        match CodexAdapter::start() {
            Ok(adapter) => {
                let info = adapter.info();
                self.backend = Backend::Codex(adapter);
                let health = match &mut self.backend {
                    Backend::Codex(adapter) => adapter.health(),
                    _ => unreachable!("restart installed a non-Codex backend"),
                };
                self.success_for(
                    &request,
                    json!({
                        "schema_version": "runtime-restart/0.1",
                        "status": "restarted",
                        "backend": {
                            "adapter": info.adapter,
                            "version": info.version,
                            "permission_profile": info.permission_profile
                        },
                        "health": health
                    }),
                )
            }
            Err(error) => {
                let safe_error = bounded_provider_text(&error, 512);
                self.backend = Backend::Unavailable(safe_error.clone());
                self.error_for(
                    &request,
                    -32110,
                    format!("Codex App Server restart failed: {safe_error}"),
                )
            }
        }
    }

    fn runtime_degradations(&self, request: Request) -> Vec<Value> {
        let degradations = match &self.backend {
            Backend::Codex(_) => vec![
                json!({
                    "feature": "agent-mutation",
                    "state": "disabled",
                    "reason": "Aegisy Codex sessions use read-only sandbox and never approve writes or mutating commands",
                    "scope": "runtime"
                }),
                json!({
                    "feature": "provider-thread-item-content",
                    "state": "metadata-only",
                    "reason": "provider thread list/read omit raw rollout items until stable AAP item mappings exist",
                    "scope": "provider"
                }),
                json!({
                    "feature": "provider-thread-delete",
                    "state": "blocked",
                    "reason": "requires scoped user review, recovery, retention, and compensation",
                    "scope": "provider"
                }),
                json!({
                    "feature": "provider-thread-compact",
                    "state": "blocked",
                    "reason": "requires a durable checkpoint, preservation review, and failure recovery",
                    "scope": "provider"
                }),
            ],
            Backend::Preview => vec![json!({
                "feature": "codex-provider",
                "state": "unavailable",
                "reason": "preview runtime does not launch a provider adapter",
                "scope": "runtime"
            })],
            Backend::Recovery(_) => vec![json!({
                "feature": "workbench-mutation",
                "state": "disabled",
                "reason": "workbench is in read-only recovery",
                "scope": "runtime"
            })],
            Backend::Unavailable(_) => vec![json!({
                "feature": "codex-provider",
                "state": "unavailable",
                "reason": "provider adapter failed before becoming ready",
                "scope": "runtime"
            })],
        };
        self.success_for(
            &request,
            json!({
                "schema_version": "runtime-degradations/0.1",
                "degradations": degradations
            }),
        )
    }

    fn recovery_diagnostic(&self, request: Request, export: bool) -> Vec<Value> {
        let Backend::Recovery(diagnostic) = &self.backend else {
            return self.error_for(&request, -32121, "workbench recovery mode is not active");
        };
        let mut value =
            serde_json::to_value(diagnostic).expect("recovery diagnostic serialization");
        if export {
            let object = value
                .as_object_mut()
                .expect("recovery diagnostic is an object");
            object.insert(
                "export".into(),
                json!({
                    "schema_version": "workbench-recovery-export/0.1",
                    "contains_database_content": false,
                    "contains_project_paths": false,
                    "contains_credentials": false,
                    "remediation": [
                        "preserve-original-database-and-backups",
                        "do-not-delete-history-automatically",
                        "upgrade-or-export-diagnostics-before-repair"
                    ]
                }),
            );
        }
        self.success_for(&request, value)
    }

    fn initialize(&mut self, request: Request) -> Vec<Value> {
        let params: InitializeParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        if params.protocol_version != PROTOCOL_VERSION {
            return self.error_for(&request, -32003, "unsupported AAP protocol version");
        }
        self.initialized = true;
        let recovery_mode = self.is_recovery_mode();
        let (backend, mut capabilities) = match &self.backend {
            Backend::Preview => (
                BackendDescriptor {
                    adapter: "preview".into(),
                    status: "ready".into(),
                    version: env!("CARGO_PKG_VERSION").into(),
                },
                vec!["runtime.preview".into()],
            ),
            Backend::Codex(adapter) => {
                let info = adapter.info();
                (
                    BackendDescriptor {
                        adapter: info.adapter,
                        status: "ready".into(),
                        version: info.version,
                    },
                    vec![
                        "runtime.codex-app-server".into(),
                        "runtime.restart".into(),
                        "timeline.command.structured.read-only".into(),
                        "turn.cancel.interrupt".into(),
                        "turn.steer.same-turn".into(),
                        "session.provider.lifecycle.archive".into(),
                        "session.provider.lifecycle.unarchive".into(),
                        "session.provider.lifecycle.list-read".into(),
                    ],
                )
            }
            Backend::Recovery(diagnostic) => (
                BackendDescriptor {
                    adapter: "aegisy-workbench-store".into(),
                    status: "read-only-recovery".into(),
                    version: diagnostic.schema_version.clone(),
                },
                vec![
                    "runtime.recovery.read-only".into(),
                    "runtime.health".into(),
                    "runtime.degradations".into(),
                    "runtime.recovery.status".into(),
                    "runtime.recovery.diagnostic-export".into(),
                    "permission.read-only".into(),
                ],
            ),
            Backend::Unavailable(error) => (
                BackendDescriptor {
                    adapter: "codex-app-server".into(),
                    status: "unavailable".into(),
                    version: error.clone(),
                },
                vec!["runtime.unavailable".into(), "runtime.restart".into()],
            ),
        };
        if !recovery_mode {
            capabilities.extend([
                "project.open".into(),
                "project.list".into(),
                "project.navigation.persistent".into(),
                "project.trust-review".into(),
                "project.trust-acknowledge".into(),
                "project.roots.scoped".into(),
                "project.relink.explicit".into(),
                "session.chat".into(),
                "session.list".into(),
                "session.resume".into(),
                "session.fork".into(),
                "session.history.paginated".into(),
                "session.metadata.manage".into(),
                "session.portable.export".into(),
                "session.portable.import".into(),
                "session.deletion.two-phase".into(),
                "session.deletion.undo".into(),
                "retention.policy.manage".into(),
                "retention.maintenance.host-triggered".into(),
                "session.recovery.status".into(),
                "operation.reconciliation".into(),
                "operation.reconciliation.probe".into(),
                "runtime.projection-recovery.status".into(),
                "runtime.health".into(),
                "runtime.degradations".into(),
                "session.work.preview".into(),
                "timeline.streaming".into(),
                "turn.context.structured".into(),
                "permission.read-only".into(),
                "workspace.list".into(),
                "workspace.read-text".into(),
                "workspace.save-user-text".into(),
                "workspace.metadata".into(),
                "workspace.watch.poll".into(),
                "workspace.git-status".into(),
                "workspace.git-query.read-only".into(),
                "workspace.search.bounded".into(),
                "workspace.search.cancel".into(),
                "workspace.index.tree-sitter".into(),
                "workspace.index.cancel".into(),
                "workspace.repository-map.budgeted".into(),
                "workspace.language-servers".into(),
                "workspace.definition".into(),
                "workspace.references".into(),
                "workspace.diagnostics.language-server".into(),
                "workspace.diagnostics.command-output".into(),
                "workspace.diagnostics.observed".into(),
                "workspace.diagnostics.raw-reference".into(),
                "artifact.command-output.bounded".into(),
                "workspace.edit.preview.read-only".into(),
                "terminal.environment.session-scoped".into(),
                "terminal.lifecycle.named".into(),
                "terminal.stop.out-of-band".into(),
                TerminalManager::capability().into(),
            ]);
            if self.workbench_store.is_some() && self.compaction_store.is_some() {
                capabilities.push("session.compaction.checkpoint-review".into());
            }
        }
        let result = InitializeResult {
            protocol_version: PROTOCOL_VERSION.to_owned(),
            runtime: Identity {
                name: "aegisy-agentd".into(),
                version: env!("CARGO_PKG_VERSION").into(),
            },
            backend,
            capabilities,
        };
        self.success_for(
            &request,
            serde_json::to_value(result).expect("result serialization"),
        )
    }

    fn project_list(&self, request: Request) -> Vec<Value> {
        let params: ProjectListParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        if !(1..=100).contains(&params.limit) {
            return self.error_for(
                &request,
                -32602,
                "project list limit must be between 1 and 100",
            );
        }
        let entries = if let Some(store) = self.workbench_store.as_ref() {
            match store.list_project_navigation(params.limit) {
                Ok(entries) => entries,
                Err(error) => return self.error_for(&request, -32113, error.message),
            }
        } else {
            let mut projects = self.projects.values().cloned().collect::<Vec<_>>();
            projects.sort_by(|left, right| left.root.cmp(&right.root).then(left.id.cmp(&right.id)));
            projects
                .into_iter()
                .take(params.limit)
                .map(|project| {
                    let project_id = project.id.clone();
                    let available = Path::new(&project.root).is_dir();
                    let navigation = self.project_navigation.get(&project_id).cloned().unwrap_or(
                        workbench_store::StoredProjectNavigation {
                            project_id: project_id.clone(),
                            pinned: false,
                            last_opened_at_ms: 0,
                        },
                    );
                    StoredProjectNavigationEntry {
                        project: workbench_store::StoredProject {
                            project_id: project_id.clone(),
                            canonical_root: project.root,
                            root_identity: String::new(),
                            display_name: project.name,
                            state: if available { "active" } else { "unavailable" }.into(),
                            created_at_ms: 0,
                            updated_at_ms: 0,
                        },
                        navigation,
                        availability: if available {
                            "available"
                        } else {
                            "unavailable"
                        }
                        .into(),
                        relink_required: !available,
                        session_count: 0,
                        active_session_count: 0,
                        live_session_count: 0,
                    }
                })
                .collect()
        };
        self.success_for(
            &request,
            json!({
                "schema_version": "project-list/0.1",
                "projects": entries.into_iter().map(project_navigation_value).collect::<Vec<_>>()
            }),
        )
    }

    fn project_navigation(&mut self, request: Request) -> Vec<Value> {
        let params: ProjectNavigationParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        if !self.projects.contains_key(&params.project_id) {
            return self.error_for(&request, -32023, "project not found");
        }
        let navigation = if let Some(store) = self.workbench_store.as_mut() {
            match store.update_project_navigation(&params.project_id, Some(params.pinned), None) {
                Ok(navigation) => navigation,
                Err(error) => return self.error_for(&request, -32113, error.message),
            }
        } else {
            let navigation = workbench_store::StoredProjectNavigation {
                project_id: params.project_id.clone(),
                pinned: params.pinned,
                last_opened_at_ms: now_ms(),
            };
            self.project_navigation
                .insert(params.project_id.clone(), navigation.clone());
            navigation
        };
        self.project_navigation
            .insert(params.project_id.clone(), navigation.clone());
        self.success_for(
            &request,
            json!({
                "schema_version": "project-navigation/0.1",
                "project_id": params.project_id,
                "navigation": navigation
            }),
        )
    }

    fn project_root_list(&self, request: Request) -> Vec<Value> {
        let params: ProjectRootListParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        if !self.projects.contains_key(&params.project_id) {
            return self.error_for(&request, -32023, "project not found");
        }
        let roots = self
            .project_roots
            .get(&params.project_id)
            .cloned()
            .or_else(|| {
                self.workbench_store
                    .as_ref()
                    .and_then(|store| store.load_project_roots(&params.project_id).ok())
            })
            .unwrap_or_default();
        self.success_for(
            &request,
            json!({
                "schema_version": "project-roots/0.1",
                "project_id": params.project_id,
                "roots": roots.iter().map(project_root_value).collect::<Vec<_>>()
            }),
        )
    }

    fn project_root_add(&mut self, request: Request) -> Vec<Value> {
        let params: ProjectRootAddParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        if !matches!(params.access.as_str(), "read" | "write") {
            return self.error_for(
                &request,
                -32602,
                "project root access must be read or write",
            );
        }
        let Some(project) = self.projects.get(&params.project_id).cloned() else {
            return self.error_for(&request, -32023, "project not found");
        };
        let canonical = match canonical_project_root(Path::new(&params.root)) {
            Ok(path) => path,
            Err(error) => return self.error_for(&request, -32020, error),
        };
        let root_identity = match filesystem_root_identity(&canonical) {
            Ok(identity) => identity,
            Err(error) => return self.error_for(&request, -32020, error),
        };
        let roots = self
            .project_roots
            .entry(params.project_id.clone())
            .or_default();
        if roots.iter().any(|root| {
            root.canonical_root == canonical.to_string_lossy()
                || root.root_identity == root_identity
        }) {
            return self.error_for(&request, -32026, "project root is already registered");
        }
        let mut root_sequence = self.next_id.saturating_add(1);
        while roots
            .iter()
            .any(|root| root.root_id == format!("root-{root_sequence}"))
        {
            root_sequence = root_sequence.saturating_add(1);
        }
        let root_id = format!("root-{root_sequence}");
        let timestamp = now_ms();
        let candidate = workbench_store::StoredProjectRoot {
            project_id: params.project_id.clone(),
            root_id: root_id.clone(),
            canonical_root: canonical.to_string_lossy().into_owned(),
            root_identity: root_identity.clone(),
            access: params.access.clone(),
            created_at_ms: timestamp,
        };
        let stored = if let Some(store) = self.workbench_store.as_mut() {
            match store.add_project_root(workbench_store::StoredProjectRootAdd {
                project_id: candidate.project_id.clone(),
                root_id: candidate.root_id.clone(),
                canonical_root: candidate.canonical_root.clone(),
                root_identity: candidate.root_identity.clone(),
                access: candidate.access.clone(),
                created_at_ms: candidate.created_at_ms,
            }) {
                Ok(root) => root,
                Err(error) => return self.error_for(&request, -32113, error.message),
            }
        } else {
            candidate
        };
        self.next_id = self.next_id.max(root_sequence);
        let roots_snapshot = {
            roots.push(stored.clone());
            roots.clone()
        };
        let project_value = json!({
            "id": project.id,
            "root": project.root,
            "name": project.name
        });
        self.success_for(
            &request,
            json!({
                "schema_version": "project-root-add/0.1",
                "project": project_value,
                "root": project_root_value(&stored),
                "roots": roots_snapshot.iter().map(project_root_value).collect::<Vec<_>>()
            }),
        )
    }

    fn project_root_remove(&mut self, request: Request) -> Vec<Value> {
        let params: ProjectRootRemoveParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        if params.root_id == "root-1" {
            return self.error_for(
                &request,
                -32027,
                "the primary project root cannot be removed",
            );
        }
        let Some(project) = self.projects.get(&params.project_id).cloned() else {
            return self.error_for(&request, -32023, "project not found");
        };
        let Some(roots) = self.project_roots.get(&params.project_id) else {
            return self.error_for(&request, -32023, "project roots are unavailable");
        };
        if roots.len() <= 1 {
            return self.error_for(&request, -32027, "a project must retain at least one root");
        }
        if !roots.iter().any(|root| root.root_id == params.root_id) {
            return self.error_for(&request, -32023, "project root not found");
        }
        if let Some(store) = self.workbench_store.as_mut() {
            if let Err(error) =
                store.remove_project_root(&params.project_id, &params.root_id, now_ms())
            {
                return self.error_for(&request, -32113, error.message);
            }
        }
        let roots_snapshot = {
            let roots = self
                .project_roots
                .get_mut(&params.project_id)
                .expect("project roots checked above");
            roots.retain(|root| root.root_id != params.root_id);
            roots.clone()
        };
        self.success_for(
            &request,
            json!({
                "schema_version": "project-root-remove/0.1",
                "project": {
                    "id": project.id,
                    "root": project.root,
                    "name": project.name
                },
                "removed_root_id": params.root_id,
                "roots": roots_snapshot.iter().map(project_root_value).collect::<Vec<_>>()
            }),
        )
    }

    fn project_relink(&mut self, request: Request) -> Vec<Value> {
        let params: ProjectRelinkParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        let Some(project) = self.projects.get(&params.project_id).cloned() else {
            return self.error_for(&request, -32023, "project not found");
        };
        let Some(existing_root) = self
            .project_roots
            .get(&params.project_id)
            .and_then(|roots| roots.iter().find(|root| root.root_id == params.root_id))
            .cloned()
        else {
            return self.error_for(&request, -32023, "project root not found");
        };
        if existing_root.root_identity != params.expected_root_identity {
            return self.error_for(&request, -32042, "project relink review is stale");
        }
        let canonical = match canonical_project_root(Path::new(&params.root)) {
            Ok(path) => path,
            Err(error) => return self.error_for(&request, -32020, error),
        };
        let root_identity = match filesystem_root_identity(&canonical) {
            Ok(identity) => identity,
            Err(error) => return self.error_for(&request, -32020, error),
        };
        let occupied = self.project_roots.values().flatten().any(|root| {
            !(root.project_id == params.project_id && root.root_id == params.root_id)
                && (root.canonical_root == canonical.to_string_lossy()
                    || root.root_identity == root_identity)
        });
        if occupied {
            return self.error_for(
                &request,
                -32026,
                "relink target is already registered by another project root",
            );
        }
        if let Some(store) = self.workbench_store.as_mut() {
            let stored = workbench_store::StoredProjectRootRelink {
                project_id: params.project_id.clone(),
                root_id: params.root_id.clone(),
                canonical_root: canonical.to_string_lossy().into_owned(),
                root_identity: root_identity.clone(),
                expected_root_identity: params.expected_root_identity,
                updated_at_ms: now_ms(),
            };
            if let Err(error) = store.relink_project_root(stored) {
                return self.error_for(&request, -32113, error.message);
            }
            if let Err(error) =
                store.update_project_navigation(&params.project_id, None, Some(now_ms()))
            {
                return self.error_for(&request, -32113, error.message);
            }
        }
        let canonical_text = canonical.to_string_lossy().into_owned();
        if let Some(roots) = self.project_roots.get_mut(&params.project_id) {
            if let Some(root) = roots.iter_mut().find(|root| root.root_id == params.root_id) {
                root.canonical_root = canonical_text.clone();
                root.root_identity = root_identity.clone();
            }
        }
        if params.root_id == "root-1" {
            let updated = Project {
                id: project.id.clone(),
                root: canonical_text.clone(),
                name: project.name.clone(),
            };
            self.projects.insert(params.project_id.clone(), updated);
        }
        let updated_project = self
            .projects
            .get(&params.project_id)
            .cloned()
            .unwrap_or(project);
        let roots = self
            .project_roots
            .get(&params.project_id)
            .cloned()
            .unwrap_or_default();
        self.success_for(
            &request,
            json!({
                "schema_version": "project-relink/0.1",
                "project": {
                    "id": updated_project.id,
                    "root": updated_project.root,
                    "name": updated_project.name
                },
                "root_id": params.root_id,
                "identity": {
                    "availability": "available",
                    "relink_required": false,
                    "root_identity": root_identity,
                    "canonical_root": canonical_text
                },
                "roots": roots.iter().map(project_root_value).collect::<Vec<_>>()
            }),
        )
    }

    fn annotate_project_trust_review(
        &self,
        project_id: &str,
        root_id: &str,
        current_root_identity: &str,
        mut review: Value,
    ) -> Result<Value, String> {
        let key = format!("{project_id}\0{root_id}");
        let acknowledgement = if let Some(store) = self.workbench_store.as_ref() {
            store
                .load_project_trust_acknowledgement(project_id, root_id)
                .map_err(|error| error.message)?
        } else {
            self.project_trust_acknowledgements.get(&key).cloned()
        };
        let review_id = review
            .get("review_id")
            .and_then(Value::as_str)
            .ok_or_else(|| "project trust review identity is missing".to_owned())?;
        let (trust_state, required, invalidation_reason) = match acknowledgement.as_ref() {
            Some(acknowledgement)
                if acknowledgement.review_id == review_id
                    && acknowledgement.root_identity == current_root_identity =>
            {
                ("acknowledged", false, Value::Null)
            }
            Some(acknowledgement) if acknowledgement.root_identity != current_root_identity => (
                "invalidated",
                true,
                Value::String("root-identity-changed".into()),
            ),
            Some(_) => (
                "invalidated",
                true,
                Value::String("review-content-changed".into()),
            ),
            None => ("unreviewed", true, Value::Null),
        };
        let object = review
            .as_object_mut()
            .ok_or_else(|| "project trust review is invalid".to_owned())?;
        object.insert("trust_state".into(), trust_state.into());
        object.insert("required".into(), required.into());
        object.insert("invalidation_reason".into(), invalidation_reason);
        object.insert(
            "acknowledgement".into(),
            acknowledgement
                .map(|acknowledgement| {
                    json!({
                        "schema_version": "project-trust-acknowledgement/0.1",
                        "review_id": acknowledgement.review_id,
                        "root_identity": acknowledgement.root_identity,
                        "acknowledged_at_ms": acknowledgement.acknowledged_at_ms,
                        "permission_effect": "none-read-only-boundary-unchanged"
                    })
                })
                .unwrap_or(Value::Null),
        );
        Ok(review)
    }

    fn project_open(&mut self, request: Request) -> Vec<Value> {
        let params: ProjectOpenParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        let path = Path::new(&params.root);
        if !path.is_dir() {
            if let Some(store) = self.workbench_store.as_ref() {
                if let Ok(projects) = store.list_projects() {
                    if let Some(stored) = projects
                        .into_iter()
                        .find(|project| project.canonical_root == params.root)
                    {
                        let stored_root_identity = stored.root_identity.clone();
                        let roots = store
                            .load_project_roots(&stored.project_id)
                            .unwrap_or_default();
                        let project = Project {
                            id: stored.project_id,
                            root: stored.canonical_root,
                            name: stored.display_name,
                        };
                        return self.success_for(
                            &request,
                            json!({
                                "project": project,
                                "identity": {
                                    "availability": "unavailable",
                                    "relink_required": true,
                                    "root_id": "root-1",
                                    "root_identity": stored_root_identity.clone(),
                                    "stored_root_identity": stored_root_identity
                                },
                                "roots": roots.iter().map(project_root_value).collect::<Vec<_>>()
                            }),
                        );
                    }
                }
            }
            return self.error_for(&request, -32020, "project root is not a directory");
        }
        let canonical = match path.canonicalize() {
            Ok(path) => path,
            Err(error) => {
                return self.error_for(&request, -32020, format!("cannot open project: {error}"))
            }
        };
        let root_identity = match filesystem_root_identity(&canonical) {
            Ok(identity) => identity,
            Err(error) => {
                return self.error_for(
                    &request,
                    -32020,
                    format!("cannot identify project root: {error}"),
                )
            }
        };
        let trust_review = match project_trust_review(&canonical, &root_identity) {
            Ok(review) => review,
            Err(error) => {
                return self.error_for(
                    &request,
                    -32020,
                    format!("cannot inspect project trust boundary: {error}"),
                )
            }
        };
        if let Some(existing) = self
            .projects
            .values()
            .find(|project| Path::new(&project.root) == canonical.as_path())
            .cloned()
        {
            if let Some(store) = self.workbench_store.as_mut() {
                if let Ok(stored) = store.load_project(&existing.id) {
                    if stored.root_identity.starts_with("path:sha256:")
                        && stored.root_identity != root_identity
                    {
                        if let Err(error) = store.migrate_project_root_identity(
                            &existing.id,
                            "root-1",
                            &root_identity,
                            now_ms(),
                        ) {
                            return self.error_for(
                                &request,
                                -32113,
                                format!("cannot migrate project identity: {}", error.message),
                            );
                        }
                    }
                }
                if let Err(error) =
                    store.update_project_navigation(&existing.id, None, Some(now_ms()))
                {
                    return self.error_for(&request, -32113, error.message);
                }
            }
            self.project_roots
                .entry(existing.id.clone())
                .or_insert_with(|| {
                    vec![workbench_store::StoredProjectRoot {
                        project_id: existing.id.clone(),
                        root_id: "root-1".into(),
                        canonical_root: existing.root.clone(),
                        root_identity: root_identity.clone(),
                        access: "write".into(),
                        created_at_ms: now_ms(),
                    }]
                });
            let trust_review = match self.annotate_project_trust_review(
                &existing.id,
                "root-1",
                &root_identity,
                trust_review,
            ) {
                Ok(review) => review,
                Err(error) => return self.error_for(&request, -32113, error),
            };
            return self.success_for(
                &request,
                json!({
                    "project": existing,
                    "identity": {
                        "availability": "available",
                        "relink_required": false,
                        "root_identity": root_identity
                    },
                    "trust_review": trust_review
                }),
            );
        }
        if let Some(store) = self.workbench_store.as_ref() {
            if let Ok(projects) = store.list_projects() {
                if let Some(stored) = projects
                    .into_iter()
                    .find(|project| project.root_identity == root_identity)
                {
                    let project_id = stored.project_id.clone();
                    let stored_root_identity = stored.root_identity.clone();
                    let roots = store
                        .load_project_roots(&stored.project_id)
                        .unwrap_or_default();
                    let project = Project {
                        id: stored.project_id,
                        root: stored.canonical_root,
                        name: stored.display_name,
                    };
                    let trust_review = match self.annotate_project_trust_review(
                        &project_id,
                        "root-1",
                        &root_identity,
                        trust_review,
                    ) {
                        Ok(review) => review,
                        Err(error) => return self.error_for(&request, -32113, error),
                    };
                    return self.success_for(
                        &request,
                        json!({
                            "project": project,
                                "identity": {
                                    "availability": "moved",
                                    "relink_required": true,
                                "root_id": "root-1",
                                "root_identity": stored_root_identity.clone(),
                                "stored_root_identity": stored_root_identity,
                                "candidate_root": canonical,
                                "candidate_root_identity": root_identity
                                },
                            "roots": roots.iter().map(project_root_value).collect::<Vec<_>>(),
                            "trust_review": trust_review
                        }),
                    );
                }
            }
        }
        let id = self.allocate_id("project");
        let project = Project {
            id: id.clone(),
            root: canonical.to_string_lossy().into_owned(),
            name: canonical
                .file_name()
                .and_then(|name| name.to_str())
                .unwrap_or("Project")
                .to_owned(),
        };
        if let Err(error) = self.persist_project(&project) {
            return self.error_for(&request, -32113, format!("cannot persist project: {error}"));
        }
        self.project_roots.insert(
            id.clone(),
            vec![workbench_store::StoredProjectRoot {
                project_id: id.clone(),
                root_id: "root-1".into(),
                canonical_root: project.root.clone(),
                root_identity: root_identity.clone(),
                access: "write".into(),
                created_at_ms: now_ms(),
            }],
        );
        self.projects.insert(id.clone(), project.clone());
        let trust_review =
            match self.annotate_project_trust_review(&id, "root-1", &root_identity, trust_review) {
                Ok(review) => review,
                Err(error) => return self.error_for(&request, -32113, error),
            };
        self.success_for(
            &request,
            json!({
                "project": project,
                "identity": {
                    "availability": "available",
                    "relink_required": false,
                    "root_identity": root_identity
                },
                "trust_review": trust_review
            }),
        )
    }

    fn project_trust_review(&self, request: Request) -> Vec<Value> {
        let params: ProjectTrustReviewParams = match serde_json::from_value(request.params.clone())
        {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        let path = Path::new(&params.root);
        if !path.is_dir() {
            return self.error_for(&request, -32020, "project root is not a directory");
        }
        let canonical = match path.canonicalize() {
            Ok(path) => path,
            Err(error) => {
                return self.error_for(&request, -32020, format!("cannot inspect project: {error}"))
            }
        };
        let root_identity = match filesystem_root_identity(&canonical) {
            Ok(identity) => identity,
            Err(error) => {
                return self.error_for(
                    &request,
                    -32020,
                    format!("cannot identify project root: {error}"),
                )
            }
        };
        match project_trust_review(&canonical, &root_identity) {
            Ok(review) => {
                let canonical_text = canonical.to_string_lossy();
                let binding = self.project_roots.iter().find_map(|(project_id, roots)| {
                    roots
                        .iter()
                        .find(|root| {
                            root.canonical_root == canonical_text
                                || root.root_identity == root_identity
                        })
                        .map(|root| (project_id.clone(), root.root_id.clone()))
                });
                let review = if let Some((project_id, root_id)) = binding {
                    match self.annotate_project_trust_review(
                        &project_id,
                        &root_id,
                        &root_identity,
                        review,
                    ) {
                        Ok(review) => review,
                        Err(error) => return self.error_for(&request, -32113, error),
                    }
                } else {
                    review
                };
                self.success_for(&request, json!({"review": review}))
            }
            Err(error) => self.error_for(
                &request,
                -32020,
                format!("cannot inspect project trust boundary: {error}"),
            ),
        }
    }

    fn project_trust_acknowledge(&mut self, request: Request) -> Vec<Value> {
        let params: ProjectTrustAcknowledgeParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(error) => {
                    return self.error_for(&request, -32602, format!("invalid params: {error}"))
                }
            };
        let Some(root) = self
            .project_roots
            .get(&params.project_id)
            .and_then(|roots| roots.iter().find(|root| root.root_id == params.root_id))
            .cloned()
        else {
            return self.error_for(&request, -32023, "project root not found");
        };
        if root.root_identity != params.root_identity {
            return self.error_for(&request, -32042, "project trust review root is stale");
        }
        let canonical = match canonical_project_root(Path::new(&root.canonical_root)) {
            Ok(path) => path,
            Err(error) => return self.error_for(&request, -32042, error),
        };
        let current_root_identity = match filesystem_root_identity(&canonical) {
            Ok(identity) => identity,
            Err(error) => return self.error_for(&request, -32042, error),
        };
        if current_root_identity != params.root_identity {
            return self.error_for(
                &request,
                -32042,
                "project trust review root identity changed",
            );
        }
        let current_review = match project_trust_review(&canonical, &current_root_identity) {
            Ok(review) => review,
            Err(error) => return self.error_for(&request, -32020, error),
        };
        if current_review.get("review_id").and_then(Value::as_str)
            != Some(params.review_id.as_str())
        {
            return self.error_for(
                &request,
                -32042,
                "project trust review content changed; review again",
            );
        }
        let acknowledgement = StoredProjectTrustAcknowledgement {
            project_id: params.project_id.clone(),
            root_id: params.root_id.clone(),
            root_identity: params.root_identity.clone(),
            review_id: params.review_id.clone(),
            acknowledged_at_ms: now_ms(),
        };
        let acknowledgement = if let Some(store) = self.workbench_store.as_mut() {
            match store.acknowledge_project_trust_review(StoredProjectTrustAcknowledge {
                project_id: acknowledgement.project_id.clone(),
                root_id: acknowledgement.root_id.clone(),
                root_identity: acknowledgement.root_identity.clone(),
                review_id: acknowledgement.review_id.clone(),
                acknowledged_at_ms: acknowledgement.acknowledged_at_ms,
            }) {
                Ok(acknowledgement) => acknowledgement,
                Err(error) => return self.error_for(&request, -32113, error.message),
            }
        } else {
            acknowledgement
        };
        self.project_trust_acknowledgements.insert(
            format!("{}\0{}", params.project_id, params.root_id),
            acknowledgement.clone(),
        );
        let review = match self.annotate_project_trust_review(
            &params.project_id,
            &params.root_id,
            &current_root_identity,
            current_review,
        ) {
            Ok(review) => review,
            Err(error) => return self.error_for(&request, -32113, error),
        };
        self.success_for(
            &request,
            json!({
                "schema_version": "project-trust-acknowledgement/0.1",
                "project_id": params.project_id,
                "root_id": params.root_id,
                "trust_state": "acknowledged",
                "permission_effect": "none-read-only-boundary-unchanged",
                "acknowledgement": acknowledgement,
                "review": review
            }),
        )
    }

    fn session_list(&self, request: Request) -> Vec<Value> {
        let params: SessionListParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        if params.limit == 0 || params.limit > 200 {
            return self.error_for(
                &request,
                -32602,
                "session list limit must be between 1 and 200",
            );
        }
        if let Some(store) = self.workbench_store.as_ref() {
            let mode = params.mode.as_ref().map(|mode| match mode {
                SessionMode::Chat => StoredSessionMode::Chat,
                SessionMode::Work => StoredSessionMode::Work,
            });
            let sessions = match store.list_sessions(
                params.project_id.as_deref(),
                mode,
                params.include_archived,
                params.limit,
            ) {
                Ok(sessions) => sessions,
                Err(error) => {
                    return self.error_for(
                        &request,
                        -32114,
                        format!("cannot list durable sessions: {}", error.message),
                    )
                }
            };
            let mut session_values = Vec::with_capacity(sessions.len());
            for session in sessions {
                let recovery_required = store.session_requires_recovery(&session.session_id);
                let deletion = match store.session_deletion_for_session(&session.session_id) {
                    Ok(deletion) => deletion,
                    Err(error) => {
                        return self.error_for(
                            &request,
                            -32114,
                            format!("cannot read session deletion state: {}", error.message),
                        )
                    }
                };
                if deletion
                    .as_ref()
                    .is_some_and(|receipt| receipt.state == "purged")
                {
                    continue;
                }
                let mut value =
                    serde_json::to_value(session).expect("stored session listing serialization");
                let object = value
                    .as_object_mut()
                    .expect("stored session listing is an object");
                object.insert("recovery_required".into(), recovery_required.into());
                object.insert(
                    "deletion_pending".into(),
                    deletion
                        .as_ref()
                        .is_some_and(|receipt| receipt.state == "pending")
                        .into(),
                );
                object.insert(
                    "deletion".into(),
                    deletion
                        .map(|receipt| {
                            serde_json::to_value(receipt)
                                .expect("session deletion listing serialization")
                        })
                        .unwrap_or(Value::Null),
                );
                session_values.push(value);
            }
            return self.success_for(
                &request,
                json!({ "sessions": session_values, "durable": true }),
            );
        }

        let mut sessions = self
            .sessions
            .values()
            .filter(|state| {
                (params.include_archived || !self.archived_sessions.contains(&state.session.id))
                    && params.project_id.as_ref().is_none_or(|project_id| {
                        state.session.project_id.as_ref() == Some(project_id)
                    })
                    && params
                        .mode
                        .as_ref()
                        .is_none_or(|mode| &state.session.mode == mode)
            })
            .map(|state| {
                json!({
                    "session_id": state.session.id,
                    "project_id": state.session.project_id,
                    "mode": state.session.mode,
                    "title": state.session.title,
                    "parent_session_id": Value::Null,
                    "lineage_kind": "new",
                    "status": if self.archived_sessions.contains(&state.session.id) {
                        "archived"
                    } else {
                        "active"
                    },
                    "deletion_pending": false,
                    "deletion": Value::Null,
                    "recovery_required": false,
                    "environment_identity": state.environment.summary().environment_id
                })
            })
            .collect::<Vec<_>>();
        sessions.sort_by(|left, right| {
            left["session_id"]
                .as_str()
                .cmp(&right["session_id"].as_str())
        });
        sessions.truncate(params.limit);
        self.success_for(&request, json!({ "sessions": sessions, "durable": false }))
    }

    fn session_search(&self, request: Request) -> Vec<Value> {
        let params: SessionSearchParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        if params.limit == 0 || params.limit > 100 {
            return self.error_for(
                &request,
                -32602,
                "session search limit must be between 1 and 100",
            );
        }
        if params
            .branch
            .as_deref()
            .is_some_and(|value| !value.is_empty())
        {
            return self.error_for(
                &request,
                -32028,
                "session search filter is unavailable: branch",
            );
        }
        if let Some(project_id) = params.project_id.as_deref() {
            if project_id.is_empty()
                || project_id.len() > 128
                || project_id
                    .bytes()
                    .any(|byte| !(byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_')))
            {
                return self.error_for(
                    &request,
                    -32602,
                    "session search project filter is invalid",
                );
            }
        }
        for value in [
            params.model.as_deref(),
            params.runtime.as_deref(),
            params.status.as_deref(),
            params.title.as_deref(),
            params.text.as_deref(),
        ]
        .into_iter()
        .flatten()
        {
            if value.is_empty()
                || value.len() > 256
                || value.bytes().any(|byte| byte.is_ascii_control())
            {
                return self.error_for(&request, -32602, "session search filter is invalid");
            }
        }
        if params.status.as_deref().is_some_and(|status| {
            !matches!(status, "active" | "archived" | "failed" | "interrupted")
        }) {
            return self.error_for(&request, -32602, "session search status filter is invalid");
        }
        if let Some(store) = self.workbench_store.as_ref() {
            let page = match store.search_sessions(&SessionSearchRequest {
                project_id: params.project_id.clone(),
                model: params.model.clone(),
                runtime: params.runtime.clone(),
                status: params.status.clone(),
                title: params.title.clone(),
                text: params.text.clone(),
                include_archived: params.include_archived,
                cursor: params.cursor.clone(),
                limit: params.limit,
            }) {
                Ok(page) => page,
                Err(error) => {
                    return self.error_for(
                        &request,
                        -32114,
                        format!("cannot search durable sessions: {}", error.message),
                    )
                }
            };
            let next_cursor = page.next_cursor;
            let truncated = page.truncated;
            let mut sessions = Vec::with_capacity(page.results.len());
            for result in page.results {
                let recovery_required = store.session_requires_recovery(&result.session.session_id);
                let deletion = match store.session_deletion_for_session(&result.session.session_id)
                {
                    Ok(deletion) => deletion,
                    Err(error) => {
                        return self.error_for(
                            &request,
                            -32114,
                            format!(
                                "cannot read session search deletion state: {}",
                                error.message
                            ),
                        )
                    }
                };
                let mut value = serde_json::to_value(result.session)
                    .expect("session search session serialization");
                let object = value
                    .as_object_mut()
                    .expect("session search session is an object");
                object.insert(
                    "runtime".into(),
                    result
                        .runtime
                        .map(|runtime| {
                            json!({
                                "adapter": runtime.adapter,
                                "adapter_version": runtime.adapter_version,
                                "provider": runtime.provider,
                                "model": runtime.model,
                                "permission_profile": runtime.permission_profile
                            })
                        })
                        .unwrap_or(Value::Null),
                );
                object.insert("matched_fields".into(), json!(result.matched_fields));
                object.insert("recovery_required".into(), recovery_required.into());
                object.insert(
                    "deletion_pending".into(),
                    deletion
                        .as_ref()
                        .is_some_and(|receipt| receipt.state == "pending")
                        .into(),
                );
                object.insert(
                    "deletion".into(),
                    deletion
                        .map(|receipt| {
                            serde_json::to_value(receipt)
                                .expect("session search deletion serialization")
                        })
                        .unwrap_or(Value::Null),
                );
                sessions.push(value);
            }
            return self.success_for(
                &request,
                json!({
                    "schema_version": "session-search/0.1",
                    "sessions": sessions,
                    "durable": true,
                    "next_cursor": next_cursor,
                    "truncated": truncated,
                    "unavailable_filters": []
                }),
            );
        }

        let query_matches = |state: &&SessionState| {
            if !params.include_archived
                && self.archived_sessions.contains(&state.session.id)
                && params.status.as_deref() != Some("archived")
            {
                return false;
            }
            if let Some(project_id) = params.project_id.as_deref() {
                if state.session.project_id.as_deref() != Some(project_id) {
                    return false;
                }
            }
            let status = if self.archived_sessions.contains(&state.session.id) {
                "archived"
            } else {
                "active"
            };
            if params
                .status
                .as_deref()
                .is_some_and(|value| value != status)
            {
                return false;
            }
            if params
                .title
                .as_deref()
                .is_some_and(|value| !contains_case_insensitive(&state.session.title, value))
            {
                return false;
            }
            if params
                .model
                .as_deref()
                .is_some_and(|value| state.backend_info.model.as_deref() != Some(value))
            {
                return false;
            }
            if params.runtime.as_deref().is_some_and(|value| {
                state.backend_info.adapter != value
                    && state.backend_info.version != value
                    && state.backend_info.provider.as_deref() != Some(value)
            }) {
                return false;
            }
            if params.text.as_deref().is_some_and(|value| {
                !contains_case_insensitive(&state.session.title, value)
                    && !state.items.iter().any(|item| {
                        item.kind == "message"
                            && matches!(item.role.as_str(), "user" | "assistant")
                            && contains_case_insensitive(&item.content, value)
                    })
            }) {
                return false;
            }
            true
        };
        let mut sessions = self
            .sessions
            .values()
            .filter(query_matches)
            .map(|state| {
                let status = if self.archived_sessions.contains(&state.session.id) {
                    "archived"
                } else {
                    "active"
                };
                let mut matched_fields = Vec::new();
                if params.title.is_some() {
                    matched_fields.push("title");
                }
                if let Some(query) = params.text.as_deref() {
                    if contains_case_insensitive(&state.session.title, query) {
                        matched_fields.push("title");
                    }
                    if state.items.iter().any(|item| {
                        item.kind == "message"
                            && matches!(item.role.as_str(), "user" | "assistant")
                            && contains_case_insensitive(&item.content, query)
                    }) {
                        matched_fields.push("text");
                    }
                }
                if params.model.is_some() {
                    matched_fields.push("model");
                }
                if params.runtime.is_some() {
                    matched_fields.push("runtime");
                }
                json!({
                    "session_id": state.session.id,
                    "project_id": state.session.project_id,
                    "mode": state.session.mode,
                    "title": state.session.title,
                    "parent_session_id": Value::Null,
                    "lineage_kind": "new",
                    "status": status,
                    "environment_identity": state.environment.summary().environment_id,
                    "runtime": json!({
                        "adapter": &state.backend_info.adapter,
                        "adapter_version": &state.backend_info.version,
                        "provider": &state.backend_info.provider,
                        "model": &state.backend_info.model,
                        "permission_profile": &state.backend_info.permission_profile
                    }),
                    "matched_fields": matched_fields
                })
            })
            .collect::<Vec<_>>();
        sessions.sort_by(|left, right| {
            left["session_id"]
                .as_str()
                .cmp(&right["session_id"].as_str())
        });
        let truncated = sessions.len() > params.limit;
        sessions.truncate(params.limit);
        self.success_for(
            &request,
            json!({
                "schema_version": "session-search/0.1",
                "sessions": sessions,
                "durable": false,
                "next_cursor": Value::Null,
                "truncated": truncated,
                "cursor_supported": false,
                "unavailable_filters": []
            }),
        )
    }

    fn operation_probe(&mut self, request: Request) -> Vec<Value> {
        let params: OperationProbeParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        let skeleton = ReconciliationInput {
            operation_id: params.operation_id.clone(),
            session_id: params.session_id.clone(),
            kind: params.kind,
            evidence: ReconciliationEvidence {
                event: params.event.unwrap_or(EventState::None),
                process: ProcessState::NotObserved,
                workspace: WorkspaceState::NotRequired,
                git: GitState::NotRequired,
            },
        };
        if let Err(error) = reconcile_operation(&skeleton) {
            return self.error_for(
                &request,
                -32602,
                format!("invalid probe params: {}", error.message),
            );
        }
        if !self.sessions.contains_key(&params.session_id)
            && !self
                .workbench_store
                .as_ref()
                .and_then(|store| store.load_session(&params.session_id).ok())
                .is_some()
        {
            return self.error_for(&request, -32023, "session does not exist");
        }
        if params.terminal_id.is_some()
            && !matches!(
                params.kind,
                OperationKind::Terminal | OperationKind::BackgroundJob
            )
        {
            return self.error_for(
                &request,
                -32602,
                "terminal_id is only valid for terminal or background-job probes",
            );
        }
        let process = match params.kind {
            OperationKind::Turn => {
                if self.control.has_active_turn(&params.session_id) {
                    ProcessState::Running
                } else {
                    ProcessState::NotRunning
                }
            }
            OperationKind::Terminal | OperationKind::BackgroundJob => {
                let Some(terminal_id) = params.terminal_id.as_deref() else {
                    return self.error_for(
                        &request,
                        -32602,
                        "terminal_id is required for terminal or background-job probes",
                    );
                };
                let mut terminals = self
                    .control
                    .terminals
                    .lock()
                    .unwrap_or_else(|poisoned| poisoned.into_inner());
                let snapshot_result =
                    terminals.snapshot_value(terminal_id, &params.session_id, u64::MAX);
                drop(terminals);
                let snapshot = match snapshot_result {
                    Ok(snapshot) => snapshot,
                    Err(error) => return self.error_for(&request, error.code, error.message),
                };
                if snapshot
                    .get("running")
                    .and_then(Value::as_bool)
                    .unwrap_or(false)
                {
                    ProcessState::Running
                } else {
                    ProcessState::NotRunning
                }
            }
            OperationKind::WorkspaceEdit | OperationKind::Git => ProcessState::NotObserved,
        };

        let needs_root = matches!(
            params.kind,
            OperationKind::WorkspaceEdit | OperationKind::Git
        ) || params.root_id.is_some()
            || params.workspace_snapshot_hash.is_some()
            || params.git_snapshot_hash.is_some();
        let mut workspace_state = WorkspaceState::NotRequired;
        let mut git_state = GitState::NotRequired;
        let mut workspace_hash = None;
        let mut git_hash = None;
        let mut workspace_truncated = false;
        let mut git_truncated = false;
        if needs_root {
            let root =
                match self.operation_probe_root(&params.session_id, params.root_id.as_deref()) {
                    Ok((_, root)) => Some(root),
                    Err((-32020, _)) => None,
                    Err((code, message)) => return self.error_for(&request, code, message),
                };
            if matches!(params.kind, OperationKind::WorkspaceEdit)
                || params.workspace_snapshot_hash.is_some()
            {
                if let Some(root) = root.as_deref() {
                    match operation_probe::workspace(
                        root,
                        params.workspace_snapshot_hash.as_deref(),
                        matches!(params.kind, OperationKind::WorkspaceEdit),
                    ) {
                        Ok(probe) => {
                            workspace_state = probe.state;
                            workspace_hash = probe.snapshot_hash;
                            workspace_truncated = probe.truncated;
                        }
                        Err(error) => return self.error_for(&request, -32032, error.message),
                    }
                } else {
                    workspace_state = WorkspaceState::Unavailable;
                }
            }
            if matches!(params.kind, OperationKind::Git) || params.git_snapshot_hash.is_some() {
                if let Some(root) = root.as_deref() {
                    match operation_probe::git(
                        root,
                        params.git_snapshot_hash.as_deref(),
                        matches!(params.kind, OperationKind::Git),
                    ) {
                        Ok(probe) => {
                            git_state = probe.state;
                            git_hash = probe.snapshot_hash;
                            git_truncated = probe.truncated;
                        }
                        Err(error) => return self.error_for(&request, -32032, error.message),
                    }
                } else {
                    git_state = GitState::Unavailable;
                }
            }
        }
        let (event, event_source) = if let Some(event) = params.event {
            (event, "caller-supplied")
        } else if let Some(store) = self.workbench_store.as_ref() {
            match store.latest_operation_event_state(&params.session_id, &params.operation_id) {
                Ok(Some(event)) => (event, "durable-event-stream"),
                Ok(None) => (EventState::None, "no-authoritative-event"),
                Err(error) => return self.error_for(&request, -32114, error.message),
            }
        } else {
            (EventState::None, "no-authoritative-event")
        };
        let evidence = ReconciliationEvidence {
            event,
            process,
            workspace: workspace_state,
            git: git_state,
        };
        self.success_for(
            &request,
            json!({
                "schema_version": operation_probe::SCHEMA_VERSION,
                "operation_id": params.operation_id,
                "session_id": params.session_id,
                "kind": params.kind,
                "evidence": evidence,
                "workspace_snapshot_hash": workspace_hash,
                "git_snapshot_hash": git_hash,
                "workspace_truncated": workspace_truncated,
                "git_truncated": git_truncated,
                "event_source": event_source,
                "process_source": match params.kind {
                    OperationKind::Turn => "runtime-control.active-turn",
                    OperationKind::Terminal | OperationKind::BackgroundJob => "runtime-terminal-record",
                    OperationKind::WorkspaceEdit | OperationKind::Git => "not-required"
                }
            }),
        )
    }

    fn operation_probe_root(
        &self,
        session_id: &str,
        root_id: Option<&str>,
    ) -> Result<(String, PathBuf), (i64, String)> {
        let project_id = if let Some(state) = self.sessions.get(session_id) {
            if state.session.mode != SessionMode::Work {
                return Err((
                    -32095,
                    "operation probes require a project-bound Work session".into(),
                ));
            }
            state
                .session
                .project_id
                .clone()
                .ok_or_else(|| (-32021, "Work session requires a project".into()))?
        } else {
            let store = self
                .workbench_store
                .as_ref()
                .ok_or_else(|| (-32023, "session not found".into()))?;
            let session = store
                .load_session(session_id)
                .map_err(|_| (-32023, "session not found".into()))?;
            if session.mode != StoredSessionMode::Work {
                return Err((
                    -32095,
                    "operation probes require a project-bound Work session".into(),
                ));
            }
            session
                .project_id
                .ok_or_else(|| (-32021, "Work session requires a project".into()))?
        };
        let (_, root) = self.workspace_root_binding(&project_id, root_id, false)?;
        Ok((project_id, root))
    }

    fn operation_reconcile(&mut self, request: Request) -> Vec<Value> {
        let input: ReconciliationInput = match serde_json::from_value(request.params.clone()) {
            Ok(input) => input,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        let result = match reconcile_operation(&input) {
            Ok(result) => result,
            Err(error) => {
                return self.error_for(
                    &request,
                    -32602,
                    format!("invalid reconciliation evidence: {}", error.message),
                )
            }
        };
        if let Some(store) = self.workbench_store.as_mut() {
            let record = match store.append_operation_reconciliation(&input, &result, now_ms()) {
                Ok(record) => record,
                Err(error) => {
                    return self.error_for(
                        &request,
                        -32114,
                        format!("cannot persist operation reconciliation: {}", error.message),
                    )
                }
            };
            return self.success_for(
                &request,
                json!({
                    "schema_version": "operation-reconciliation-result/0.1",
                    "reconciliation": record.result,
                    "durable": true,
                    "event_sequence": record.event_sequence,
                    "updated_at_ms": record.updated_at_ms
                }),
            );
        }
        if !self.sessions.contains_key(&input.session_id) {
            return self.error_for(&request, -32023, "session does not exist");
        }
        self.operation_reconciliations.insert(
            Self::operation_reconciliation_key(&input.session_id, &input.operation_id),
            result.clone(),
        );
        self.success_for(
            &request,
            json!({
                "schema_version": "operation-reconciliation-result/0.1",
                "reconciliation": result,
                "durable": false,
                "event_sequence": Value::Null,
                "updated_at_ms": now_ms()
            }),
        )
    }

    fn provider_project_cwd(
        &self,
        project_id: Option<&str>,
    ) -> Result<Option<PathBuf>, (i64, String)> {
        let Some(project_id) = project_id else {
            return Ok(None);
        };
        let root = self
            .projects
            .get(project_id)
            .map(|project| PathBuf::from(&project.root))
            .or_else(|| {
                self.workbench_store.as_ref().and_then(|store| {
                    store
                        .load_project(project_id)
                        .ok()
                        .map(|project| PathBuf::from(project.canonical_root))
                })
            })
            .ok_or_else(|| (-32022, "project not found".to_owned()))?;
        if !root.is_dir() {
            return Err((-32020, "project workspace is unavailable".into()));
        }
        Ok(Some(root))
    }

    fn provider_thread_list(&mut self, request: Request) -> Vec<Value> {
        let params: ProviderThreadListParams = match serde_json::from_value(request.params.clone())
        {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        if !(1..=100).contains(&params.limit) {
            return self.error_for(
                &request,
                -32602,
                "provider thread list limit must be between 1 and 100",
            );
        }
        if params
            .cursor
            .as_deref()
            .is_some_and(|cursor| cursor.len() > 4 * 1024 || cursor.chars().any(char::is_control))
        {
            return self.error_for(&request, -32602, "provider thread cursor is invalid");
        }
        let cwd = match self.provider_project_cwd(params.project_id.as_deref()) {
            Ok(cwd) => cwd,
            Err((code, message)) => return self.error_for(&request, code, message),
        };
        let result = match &mut self.backend {
            Backend::Codex(adapter) => adapter.list_threads(
                params.cursor.as_deref(),
                Some(params.limit as u32),
                cwd.as_deref(),
                params.archived,
            ),
            Backend::Preview => {
                Err("provider thread listing is unavailable in preview runtime".into())
            }
            Backend::Recovery(_) => Err("workbench is in read-only recovery mode".into()),
            Backend::Unavailable(error) => Err(error.clone()),
        };
        let result = match result {
            Ok(result) => result,
            Err(error) => return self.error_for(&request, -32110, error),
        };
        let data = match result.get("data").and_then(Value::as_array) {
            Some(data) => data,
            None => {
                return self.error_for(
                    &request,
                    -32143,
                    "Codex thread/list response is missing data",
                )
            }
        };
        let mut threads = Vec::with_capacity(data.len().min(params.limit));
        for thread in data.iter().take(params.limit) {
            match provider_thread_summary(thread) {
                Ok(summary) => threads.push(summary),
                Err(error) => return self.error_for(&request, -32143, error),
            }
        }
        self.success_for(
            &request,
            json!({
                "schema_version": "provider-thread-list/0.1",
                "adapter": "codex-app-server",
                "project_id": params.project_id,
                "threads": threads,
                "next_cursor": provider_optional_text(result.get("nextCursor"), 4 * 1024),
                "backwards_cursor": provider_optional_text(result.get("backwardsCursor"), 4 * 1024),
                "provider_state_only": true,
                "content_projection": "metadata-only"
            }),
        )
    }

    fn provider_thread_read(&mut self, request: Request) -> Vec<Value> {
        let params: ProviderThreadReadParams = match serde_json::from_value(request.params.clone())
        {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        let thread_id = params.thread_id.trim();
        if thread_id.is_empty() || thread_id.len() > 256 || thread_id.chars().any(char::is_control)
        {
            return self.error_for(&request, -32602, "provider thread ID is invalid");
        }
        let result = match &mut self.backend {
            Backend::Codex(adapter) => adapter.read_thread(thread_id, params.include_turns),
            Backend::Preview => {
                Err("provider thread reading is unavailable in preview runtime".into())
            }
            Backend::Recovery(_) => Err("workbench is in read-only recovery mode".into()),
            Backend::Unavailable(error) => Err(error.clone()),
        };
        let result = match result {
            Ok(result) => result,
            Err(error) => return self.error_for(&request, -32110, error),
        };
        let thread = match result.get("thread") {
            Some(thread) => thread,
            None => {
                return self.error_for(
                    &request,
                    -32143,
                    "Codex thread/read response is missing thread",
                )
            }
        };
        let summary = match provider_thread_summary(thread) {
            Ok(summary) => summary,
            Err(error) => return self.error_for(&request, -32143, error),
        };
        let turns = if params.include_turns {
            match provider_turn_summaries(thread) {
                Ok(turns) => turns,
                Err(error) => return self.error_for(&request, -32143, error),
            }
        } else {
            Vec::new()
        };
        self.success_for(
            &request,
            json!({
                "schema_version": "provider-thread-read/0.1",
                "thread": summary,
                "turns": turns,
                "include_turns": params.include_turns,
                "provider_state_only": true,
                "content_projection": "metadata-only",
                "provider_items_omitted": true
            }),
        )
    }

    fn projection_recovery_status(&self, request: Request) -> Vec<Value> {
        let Some(store) = self.workbench_store.as_ref() else {
            return self.success_for(
                &request,
                json!({
                    "schema_version": "runtime-projection-recovery-status/0.1",
                    "configured": false,
                    "current_quarantined_sessions": 0
                }),
            );
        };
        self.success_for(
            &request,
            json!({
                "schema_version": "runtime-projection-recovery-status/0.1",
                "configured": true,
                "startup": store.startup_projection_recovery(),
                "current_quarantined_sessions": store.quarantined_session_count()
            }),
        )
    }

    fn session_recovery_status(&self, request: Request) -> Vec<Value> {
        let params: SessionIdentityParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        let Some(store) = self.workbench_store.as_ref() else {
            return self.error_for(
                &request,
                -32023,
                "durable session recovery is not configured",
            );
        };
        let recovery_required = store.session_requires_recovery(&params.session_id);
        let projection = store.verify_session_projection(&params.session_id).ok();
        if !recovery_required && projection.is_none() {
            return self.error_for(&request, -32023, "session not found");
        }
        let issues = projection
            .as_ref()
            .map(|report| report.issues.clone())
            .unwrap_or_else(|| vec!["session-projection-missing".into()]);
        self.success_for(
            &request,
            json!({
                "schema_version": "session-recovery-status/0.1",
                "session_id": params.session_id,
                "recovery_required": recovery_required,
                "issues": issues,
                "projection": projection
            }),
        )
    }

    fn session_title(&mut self, request: Request) -> Vec<Value> {
        let params: SessionTitleParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        let title = params.title.trim();
        if title.is_empty() || title.len() > 256 || title.chars().any(char::is_control) {
            return self.error_for(
                &request,
                -32602,
                "session title must contain 1 to 256 printable UTF-8 bytes",
            );
        }
        let exists_in_memory = self.sessions.contains_key(&params.session_id);
        let stored = if let Some(store) = self.workbench_store.as_mut() {
            match store.update_session_title(&params.session_id, title, now_ms()) {
                Ok(session) => Some(session),
                Err(error) => {
                    return self.error_for(
                        &request,
                        -32114,
                        format!("cannot update durable session title: {}", error.message),
                    )
                }
            }
        } else {
            None
        };
        if !exists_in_memory && stored.is_none() {
            return self.error_for(&request, -32023, "session not found");
        }
        if let Some(state) = self.sessions.get_mut(&params.session_id) {
            state.session.title = title.into();
        }
        let status = stored
            .as_ref()
            .map(|session| session.status.as_str())
            .unwrap_or_else(|| {
                if self.archived_sessions.contains(&params.session_id) {
                    "archived"
                } else {
                    "active"
                }
            });
        self.success_for(
            &request,
            json!({
                "session_id": params.session_id,
                "title": title,
                "status": status
            }),
        )
    }

    fn provider_thread_for_lifecycle(
        &self,
        session_id: &str,
    ) -> Result<Option<String>, (i64, String)> {
        if let Some(state) = self.sessions.get(session_id) {
            return Ok(state.backend_session_id.clone());
        }
        let binding = self
            .workbench_store
            .as_ref()
            .and_then(|store| store.load_session_runtime_binding(session_id).ok());
        if binding.as_ref().is_some_and(|binding| {
            binding.adapter == "codex-app-server" && binding.backend_session_id.is_some()
        }) {
            return Err((
                -32141,
                "Codex provider state is not loaded; resume the session before changing its provider lifecycle"
                    .into(),
            ));
        }
        Ok(None)
    }

    fn archive_provider_thread(&mut self, thread_id: &str) -> Result<(), String> {
        match &mut self.backend {
            Backend::Codex(adapter) => adapter.archive_session(thread_id),
            _ => Err("Codex provider adapter is unavailable".into()),
        }
    }

    fn unarchive_provider_thread(&mut self, thread_id: &str) -> Result<CodexSession, String> {
        match &mut self.backend {
            Backend::Codex(adapter) => adapter.unarchive_session(thread_id),
            _ => Err("Codex provider adapter is unavailable".into()),
        }
    }

    fn session_archive(&mut self, request: Request) -> Vec<Value> {
        let params: SessionIdentityParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        if self.control.has_active_turn(&params.session_id) {
            return self.error_for(&request, -32025, "cannot archive an active turn session");
        }
        let has_running_terminal = self
            .control
            .terminals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .records
            .values()
            .any(|terminal| {
                terminal.session_id == params.session_id
                    && matches!(terminal.state.as_str(), "running" | "stopping")
            });
        if has_running_terminal {
            return self.error_for(
                &request,
                -32025,
                "cannot archive a session with a running terminal",
            );
        }
        let exists_in_memory = self.sessions.contains_key(&params.session_id);
        let stored_status = self
            .workbench_store
            .as_ref()
            .and_then(|store| store.load_session(&params.session_id).ok())
            .map(|session| session.status);
        if !exists_in_memory && stored_status.is_none() {
            return self.error_for(&request, -32023, "session not found");
        }
        if self.archived_sessions.contains(&params.session_id)
            || stored_status.as_deref() == Some("archived")
        {
            return self.error_for(&request, -32026, "session is already archived");
        }
        let provider_thread = match self.provider_thread_for_lifecycle(&params.session_id) {
            Ok(thread_id) => thread_id,
            Err((code, message)) => return self.error_for(&request, code, message),
        };
        let provider_archived = if let Some(thread_id) = provider_thread.as_deref() {
            if let Err(error) = self.archive_provider_thread(thread_id) {
                return self.error_for(
                    &request,
                    -32143,
                    format!("cannot archive Codex provider thread: {error}"),
                );
            }
            true
        } else {
            false
        };
        if let Some(store) = self.workbench_store.as_mut() {
            match store.archive_session(&params.session_id, now_ms()) {
                Ok(_) => {}
                Err(error) => {
                    if provider_archived {
                        let compensation_failed =
                            provider_thread.as_deref().is_some_and(|thread_id| {
                                self.unarchive_provider_thread(thread_id).is_err()
                            });
                        if compensation_failed {
                            return self.error_for(
                                &request,
                                -32145,
                                "Codex provider archive was acknowledged but local persistence failed and compensation also failed; session recovery is required",
                            );
                        }
                    }
                    return self.error_for(
                        &request,
                        -32114,
                        format!("cannot archive durable session: {}", error.message),
                    );
                }
            }
        }
        if !self.archived_sessions.insert(params.session_id.clone()) {
            return self.error_for(&request, -32026, "session is already archived");
        }
        self.success_for(
            &request,
            json!({
                "session_id": params.session_id,
                "status": "archived",
                "provider_state_updated": provider_archived
            }),
        )
    }

    fn session_unarchive(&mut self, request: Request) -> Vec<Value> {
        let params: SessionIdentityParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        let exists_in_memory = self.sessions.contains_key(&params.session_id);
        let stored_status = self
            .workbench_store
            .as_ref()
            .and_then(|store| store.load_session(&params.session_id).ok())
            .map(|session| session.status);
        if !exists_in_memory && stored_status.is_none() {
            return self.error_for(&request, -32023, "session not found");
        }
        if !self.archived_sessions.contains(&params.session_id)
            && stored_status.as_deref() != Some("archived")
        {
            return self.error_for(&request, -32026, "session is not archived");
        }
        let provider_thread = match self.provider_thread_for_lifecycle(&params.session_id) {
            Ok(thread_id) => thread_id,
            Err((code, message)) => return self.error_for(&request, code, message),
        };
        let provider_session = if let Some(thread_id) = provider_thread.as_deref() {
            let restored = match self.unarchive_provider_thread(thread_id) {
                Ok(session) => session,
                Err(error) => {
                    return self.error_for(
                        &request,
                        -32143,
                        format!("cannot restore Codex provider thread: {error}"),
                    )
                }
            };
            if restored.thread_id != thread_id {
                let compensation_failed =
                    self.archive_provider_thread(&restored.thread_id).is_err();
                if compensation_failed {
                    return self.error_for(
                        &request,
                        -32145,
                        "Codex provider unarchive returned a different thread and compensation failed; session recovery is required",
                    );
                }
                return self.error_for(
                    &request,
                    -32143,
                    "Codex provider unarchive returned a different thread identity",
                );
            }
            Some(restored)
        } else {
            None
        };
        if let Some(store) = self.workbench_store.as_mut() {
            match store.unarchive_session(&params.session_id, now_ms()) {
                Ok(_) => {}
                Err(error) => {
                    if provider_session.is_some() {
                        let compensation_failed =
                            provider_thread.as_deref().is_some_and(|thread_id| {
                                self.archive_provider_thread(thread_id).is_err()
                            });
                        if compensation_failed {
                            return self.error_for(
                                &request,
                                -32145,
                                "Codex provider unarchive was acknowledged but local persistence failed and compensation also failed; session recovery is required",
                            );
                        }
                    }
                    return self.error_for(
                        &request,
                        -32114,
                        format!("cannot restore durable session: {}", error.message),
                    );
                }
            }
        }
        if let Some(provider_session) = provider_session {
            if let Some(state) = self.sessions.get_mut(&params.session_id) {
                state.backend_session_id = Some(provider_session.thread_id);
                state.backend_info.provider = Some(provider_session.provider);
                state.backend_info.model = Some(provider_session.model);
            }
        }
        self.archived_sessions.remove(&params.session_id);
        self.success_for(
            &request,
            json!({
                "session_id": params.session_id,
                "status": "active",
                "provider_state_updated": provider_thread.is_some()
            }),
        )
    }

    fn session_compaction_checkpoint_create(&mut self, request: Request) -> Vec<Value> {
        let params: SessionCompactionCheckpointCreateParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(error) => {
                    return self.error_for(&request, -32602, format!("invalid params: {error}"))
                }
            };
        if self.control.has_active_turn(&params.session_id) {
            return self.error_for(
                &request,
                -32010,
                "session compaction checkpoint is unavailable during an active turn",
            );
        }
        let Some(compaction_store) = self.compaction_store.as_ref() else {
            return self.error_for(
                &request,
                -32024,
                "session compaction checkpoint storage is unavailable",
            );
        };
        let Some(workbench_store) = self.workbench_store.as_mut() else {
            return self.error_for(
                &request,
                -32024,
                "session compaction checkpoint requires durable workbench storage",
            );
        };
        match compaction_store
            .load_optional_with_descriptor(&params.session_id, &params.checkpoint_id)
        {
            Ok(Some((stored, descriptor))) => {
                if stored.review.preservation_instructions != params.preservation_instructions
                    || stored.review.summary != params.summary
                {
                    return self.error_for(
                        &request,
                        -32009,
                        "session compaction checkpoint ID is bound to different content",
                    );
                }
                let candidate = match workbench_store
                    .rebuild_session_projection_candidate(&params.session_id)
                {
                    Ok(candidate)
                        if candidate.source_complete && candidate.matches_current_projection =>
                    {
                        candidate
                    }
                    Ok(_) => {
                        return self.error_for(
                            &request,
                            -32115,
                            "session event authority is incomplete for compaction checkpoint",
                        )
                    }
                    Err(error) => return self.error_for(&request, -32115, error.message),
                };
                let event = match find_compaction_checkpoint_event(
                    workbench_store,
                    &params.session_id,
                    &descriptor,
                    candidate.source_event_count,
                ) {
                    Ok(event) => event,
                    Err(error) => return self.error_for(&request, -32115, error),
                };
                return self.success_for(
                    &request,
                    json!({
                        "schema_version": "session-compaction-checkpoint-create-result/0.1",
                        "review": stored.review,
                        "descriptor": descriptor,
                        "event_sequence": event.sequence,
                        "idempotent_replay": true,
                        "activation_available": false,
                        "provider_compact_invoked": false,
                        "original_event_history_authoritative": true,
                    }),
                );
            }
            Ok(None) => {}
            Err(error) => return self.error_for(&request, -32113, error.message),
        }
        let candidate = match workbench_store
            .rebuild_session_projection_candidate(&params.session_id)
        {
            Ok(candidate) if candidate.source_complete && candidate.matches_current_projection => {
                candidate
            }
            Ok(_) => {
                return self.error_for(
                    &request,
                    -32115,
                    "session event authority is incomplete for compaction checkpoint",
                )
            }
            Err(error) => return self.error_for(&request, -32115, error.message),
        };
        let Some(source_hash) = candidate.source_hash.as_ref() else {
            return self.error_for(
                &request,
                -32115,
                "session event authority has no content identity",
            );
        };
        if candidate
            .session
            .as_ref()
            .is_none_or(|session| session.status != "active")
        {
            return self.error_for(
                &request,
                -32011,
                "session compaction checkpoint requires an active session",
            );
        }
        let review = match session_compaction::create_review(
            &params.checkpoint_id,
            &params.session_id,
            candidate.source_event_count,
            &source_hash.sha256,
            params.preservation_instructions.as_deref(),
            params.summary,
        ) {
            Ok(review) => review,
            Err(error) => return self.error_for(&request, -32602, error.message),
        };
        let descriptor = match compaction_store.persist(&review) {
            Ok(descriptor) => descriptor,
            Err(error) => return self.error_for(&request, -32113, error.message),
        };
        let event = match workbench_store.append_session_compaction_checkpoint_event(
            &descriptor,
            &review,
            now_ms(),
        ) {
            Ok(event) => event,
            Err(error) => return self.error_for(&request, -32113, error.message),
        };
        self.success_for(
            &request,
            json!({
                "schema_version": "session-compaction-checkpoint-create-result/0.1",
                "review": review,
                "descriptor": descriptor,
                "event_sequence": event.sequence,
                "idempotent_replay": false,
                "activation_available": false,
                "provider_compact_invoked": false,
                "original_event_history_authoritative": true,
            }),
        )
    }

    fn session_compaction_checkpoint_read(&self, request: Request) -> Vec<Value> {
        let params: SessionCompactionCheckpointReadParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(error) => {
                    return self.error_for(&request, -32602, format!("invalid params: {error}"))
                }
            };
        let Some(compaction_store) = self.compaction_store.as_ref() else {
            return self.error_for(
                &request,
                -32024,
                "session compaction checkpoint storage is unavailable",
            );
        };
        let Some(workbench_store) = self.workbench_store.as_ref() else {
            return self.error_for(
                &request,
                -32024,
                "session compaction checkpoint requires durable workbench storage",
            );
        };
        let candidate = match workbench_store
            .rebuild_session_projection_candidate(&params.session_id)
        {
            Ok(candidate) if candidate.source_complete && candidate.matches_current_projection => {
                candidate
            }
            Ok(_) => {
                return self.error_for(
                    &request,
                    -32115,
                    "session event authority is incomplete for compaction checkpoint",
                )
            }
            Err(error) => return self.error_for(&request, -32115, error.message),
        };
        let (stored, descriptor) = match compaction_store
            .load_with_descriptor(&params.session_id, &params.checkpoint_id)
        {
            Ok(value) => value,
            Err(error) => return self.error_for(&request, -32113, error.message),
        };
        let event = match find_compaction_checkpoint_event(
            workbench_store,
            &params.session_id,
            &descriptor,
            candidate.source_event_count,
        ) {
            Ok(event) => event,
            Err(error) => return self.error_for(&request, -32115, error),
        };
        self.success_for(
            &request,
            json!({
                "schema_version": "session-compaction-checkpoint-read-result/0.1",
                "review": stored.review,
                "descriptor": descriptor,
                "event_sequence": event.sequence,
                "activation_available": false,
                "provider_compact_invoked": false,
                "original_event_history_authoritative": true,
            }),
        )
    }

    fn session_export_preview(&self, request: Request) -> Vec<Value> {
        let params: PortableSessionExportPreviewParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(error) => {
                    return self.error_for(&request, -32602, format!("invalid params: {error}"))
                }
            };
        let Some(store) = self.workbench_store.as_ref() else {
            return self.error_for(
                &request,
                -32024,
                "portable session export requires durable workbench storage",
            );
        };
        match store.preview_portable_session_export(&params.session_id, now_ms()) {
            Ok(preview) => self.success_for(
                &request,
                serde_json::to_value(preview)
                    .expect("portable session export preview serialization"),
            ),
            Err(error) => self.error_for(
                &request,
                portable_session_error_code(&error.code),
                format!("cannot preview portable session export: {}", error.message),
            ),
        }
    }

    fn session_export(&self, request: Request) -> Vec<Value> {
        let params: PortableSessionExportParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(error) => {
                    return self.error_for(&request, -32602, format!("invalid params: {error}"))
                }
            };
        let Some(store) = self.workbench_store.as_ref() else {
            return self.error_for(
                &request,
                -32024,
                "portable session export requires durable workbench storage",
            );
        };
        match store.export_portable_session(&params.session_id, &params.package_hash, now_ms()) {
            Ok(package) => self.success_for(
                &request,
                json!({
                    "schema_version": "portable-session-export-result/0.1",
                    "package": package
                }),
            ),
            Err(error) => self.error_for(
                &request,
                portable_session_error_code(&error.code),
                format!("cannot export portable session: {}", error.message),
            ),
        }
    }

    fn session_import_preview(&self, request: Request) -> Vec<Value> {
        let params: PortableSessionImportParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(error) => {
                    return self.error_for(&request, -32602, format!("invalid params: {error}"))
                }
            };
        let Some(store) = self.workbench_store.as_ref() else {
            return self.error_for(
                &request,
                -32024,
                "portable session import requires durable workbench storage",
            );
        };
        let mut preview = match store
            .preview_portable_session_import(&params.package, params.target_project_id.as_deref())
        {
            Ok(preview) => preview,
            Err(error) => {
                return self.error_for(
                    &request,
                    portable_session_error_code(&error.code),
                    format!("cannot preview portable session import: {}", error.message),
                )
            }
        };
        let has_collisions =
            preview.source_session_collision || preview.source_item_id_collisions > 0;
        if params.collision_strategy == PortableSessionCollisionStrategy::Reject
            && has_collisions
            && !preview
                .blocking_reasons
                .contains(&"portable-import-collision-rejected".into())
        {
            preview
                .blocking_reasons
                .push("portable-import-collision-rejected".into());
        }
        let mut value =
            serde_json::to_value(preview).expect("portable session import preview serialization");
        if let Some(object) = value.as_object_mut() {
            object.insert(
                "collision_strategy".into(),
                Value::String(params.collision_strategy.as_str().into()),
            );
            object.insert(
                "copy_will_remap_identifiers".into(),
                (params.collision_strategy == PortableSessionCollisionStrategy::Copy
                    && has_collisions)
                    .into(),
            );
        }
        self.success_for(&request, value)
    }

    fn session_import(&mut self, request: Request) -> Vec<Value> {
        let params: PortableSessionImportParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(error) => {
                    return self.error_for(&request, -32602, format!("invalid params: {error}"))
                }
            };
        let Some(store) = self.workbench_store.as_ref() else {
            return self.error_for(
                &request,
                -32024,
                "portable session import requires durable workbench storage",
            );
        };
        let preview = match store
            .preview_portable_session_import(&params.package, params.target_project_id.as_deref())
        {
            Ok(preview) => preview,
            Err(error) => {
                return self.error_for(
                    &request,
                    portable_session_error_code(&error.code),
                    format!("cannot validate portable session import: {}", error.message),
                )
            }
        };
        if !preview.blocking_reasons.is_empty() {
            return self.error_for(
                &request,
                -32145,
                format!(
                    "portable session import is blocked: {}",
                    preview.blocking_reasons.join(",")
                ),
            );
        }
        if params.collision_strategy == PortableSessionCollisionStrategy::Reject
            && (preview.source_session_collision || preview.source_item_id_collisions > 0)
        {
            return self.error_for(
                &request,
                -32143,
                "portable session import collision was rejected",
            );
        }

        let mode = match params.package.content.mode {
            StoredSessionMode::Chat => SessionMode::Chat,
            StoredSessionMode::Work => SessionMode::Work,
        };
        let cwd = match self.session_cwd(&SessionStartParams {
            mode: mode.clone(),
            project_id: params.target_project_id.clone(),
            title: None,
        }) {
            Ok(cwd) => cwd,
            Err((code, message)) => return self.error_for(&request, code, message),
        };
        let imported_at_ms = now_ms();
        let target_session_id = format!(
            "session-import-{imported_at_ms}-{}",
            self.allocate_id("copy")
        );
        let import_id = format!(
            "portable-import-{imported_at_ms}-{}",
            self.allocate_id("operation")
        );
        let mode_name = if mode == SessionMode::Work {
            "work"
        } else {
            "chat"
        };
        let environment = SessionEnvironment::build(
            &target_session_id,
            params.target_project_id.as_deref(),
            mode_name,
            &cwd,
        );
        let environment_identity = environment.summary().environment_id.clone();
        let import_result = self
            .workbench_store
            .as_mut()
            .expect("portable import store disappeared")
            .import_portable_session(PortableSessionImportCommand {
                target_session_id: &target_session_id,
                import_id: &import_id,
                package: &params.package,
                target_project_id: params.target_project_id.as_deref(),
                reject_source_collisions: params.collision_strategy
                    == PortableSessionCollisionStrategy::Reject,
                target_environment_identity: Some(&environment_identity),
                runtime_binding: None,
                imported_at_ms,
            });
        let receipt = match import_result {
            Ok(receipt) => receipt,
            Err(error) => {
                return self.error_for(
                    &request,
                    portable_session_error_code(&error.code),
                    format!("cannot import portable session: {}", error.message),
                )
            }
        };
        let stored_items = match self
            .workbench_store
            .as_ref()
            .expect("portable import store disappeared")
            .read_session_items(&target_session_id, 0, receipt.imported_items as usize)
        {
            Ok(items) => items,
            Err(error) => {
                return self.error_for(
                    &request,
                    -32114,
                    format!("cannot load imported session history: {}", error.message),
                )
            }
        };
        let session = Session {
            id: receipt.session.session_id.clone(),
            mode,
            project_id: receipt.session.project_id.clone(),
            title: receipt.session.title.clone(),
        };
        let backend_info = match &self.backend {
            Backend::Preview => BackendInfo {
                adapter: "preview".into(),
                version: env!("CARGO_PKG_VERSION").into(),
                provider: Some("local".into()),
                model: Some("deterministic-echo".into()),
                permission_profile: "read-only".into(),
                environment: None,
            },
            Backend::Codex(adapter) => adapter.info(),
            Backend::Unavailable(error) => BackendInfo {
                adapter: "unavailable".into(),
                version: error.clone(),
                provider: None,
                model: None,
                permission_profile: "read-only".into(),
                environment: None,
            },
            Backend::Recovery(_) => unreachable!("recovery mode blocks portable import"),
        };
        self.sessions.insert(
            target_session_id,
            SessionState {
                session: session.clone(),
                items: stored_items.into_iter().map(stored_timeline_item).collect(),
                backend_session_id: None,
                backend_info: backend_info.clone(),
                environment: environment.clone(),
            },
        );
        self.success_for(
            &request,
            json!({
                "schema_version": "portable-session-import-result/0.1",
                "receipt": receipt,
                "session": session,
                "runtime": backend_info,
                "environment": environment.summary(),
                "continuation": {
                    "portable_history_available": true,
                    "provider_state_available": false,
                    "requires_new_runtime_fork": true
                }
            }),
        )
    }

    fn session_delete_preview(&self, request: Request) -> Vec<Value> {
        let params: SessionDeletePreviewParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(error) => {
                    return self.error_for(&request, -32602, format!("invalid params: {error}"))
                }
            };
        let Some(store) = self.workbench_store.as_ref() else {
            return self.error_for(
                &request,
                -32024,
                "durable session deletion is not configured",
            );
        };
        let member_ids = match store.session_deletion_member_ids(&params.session_id, params.scope) {
            Ok(member_ids) => member_ids,
            Err(error) => {
                return self.error_for(
                    &request,
                    session_deletion_error_code(&error.code),
                    format!("cannot preview session deletion: {}", error.message),
                )
            }
        };
        let mut preview = match store.preview_session_deletion(&params.session_id, params.scope) {
            Ok(preview) => preview,
            Err(error) => {
                return self.error_for(
                    &request,
                    session_deletion_error_code(&error.code),
                    format!("cannot preview session deletion: {}", error.message),
                )
            }
        };
        for reason in self.session_deletion_live_reasons(&member_ids) {
            if !preview.blocking_reasons.contains(&reason) {
                preview.blocking_reasons.push(reason);
            }
        }
        self.success_for(
            &request,
            serde_json::to_value(preview).expect("session deletion preview serialization"),
        )
    }

    fn session_delete_schedule(&mut self, request: Request) -> Vec<Value> {
        let params: SessionDeleteScheduleParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(error) => {
                    return self.error_for(&request, -32602, format!("invalid params: {error}"))
                }
            };
        let Some(store) = self.workbench_store.as_ref() else {
            return self.error_for(
                &request,
                -32024,
                "durable session deletion is not configured",
            );
        };
        let member_ids = match store.session_deletion_member_ids(&params.session_id, params.scope) {
            Ok(member_ids) => member_ids,
            Err(error) => {
                return self.error_for(
                    &request,
                    session_deletion_error_code(&error.code),
                    format!("cannot schedule session deletion: {}", error.message),
                )
            }
        };
        let live_reasons = self.session_deletion_live_reasons(&member_ids);
        if !live_reasons.is_empty() {
            return self.error_for(
                &request,
                -32025,
                format!(
                    "cannot schedule deletion while selected sessions have live activity: {}",
                    live_reasons.join(",")
                ),
            );
        }
        let requested_at_ms = now_ms();
        self.next_id = self.next_id.saturating_add(1);
        let deletion_id = format!("deletion-{requested_at_ms}-{}", self.next_id);
        let store = self
            .workbench_store
            .as_mut()
            .expect("durable session deletion store disappeared");
        match store.schedule_session_deletion(
            &deletion_id,
            &params.session_id,
            params.scope,
            &params.plan_hash,
            requested_at_ms,
            params.undo_window_ms,
        ) {
            Ok(receipt) => self.success_for(
                &request,
                serde_json::to_value(receipt).expect("session deletion receipt serialization"),
            ),
            Err(error) => self.error_for(
                &request,
                session_deletion_error_code(&error.code),
                format!("cannot schedule session deletion: {}", error.message),
            ),
        }
    }

    fn session_deletion_status(&self, request: Request) -> Vec<Value> {
        let params: SessionIdentityParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        let Some(store) = self.workbench_store.as_ref() else {
            return self.error_for(
                &request,
                -32024,
                "durable session deletion is not configured",
            );
        };
        match store.session_deletion_for_session(&params.session_id) {
            Ok(deletion) => self.success_for(
                &request,
                json!({
                    "schema_version": "session-deletion-status/0.1",
                    "session_id": params.session_id,
                    "deletion": deletion
                }),
            ),
            Err(error) => self.error_for(
                &request,
                session_deletion_error_code(&error.code),
                format!("cannot read session deletion status: {}", error.message),
            ),
        }
    }

    fn session_delete_undo(&mut self, request: Request) -> Vec<Value> {
        let params: SessionDeleteUndoParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        let Some(store) = self.workbench_store.as_mut() else {
            return self.error_for(
                &request,
                -32024,
                "durable session deletion is not configured",
            );
        };
        match store.undo_session_deletion(&params.deletion_id, now_ms()) {
            Ok(receipt) => self.success_for(
                &request,
                serde_json::to_value(receipt).expect("session deletion undo receipt serialization"),
            ),
            Err(error) => self.error_for(
                &request,
                session_deletion_error_code(&error.code),
                format!("cannot undo session deletion: {}", error.message),
            ),
        }
    }

    fn retention_policy_read(&self, request: Request) -> Vec<Value> {
        let params: RetentionPolicyIdentityParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(error) => {
                    return self.error_for(&request, -32602, format!("invalid params: {error}"))
                }
            };
        let Some(store) = self.workbench_store.as_ref() else {
            return self.error_for(&request, -32024, "durable retention is not configured");
        };
        match store.load_retention_policy(&params.scope_kind, &params.scope_id) {
            Ok(policy) => self.success_for(
                &request,
                json!({
                    "schema_version": "retention-policy-read/0.1",
                    "scope_kind": params.scope_kind,
                    "scope_id": params.scope_id,
                    "policy": policy
                }),
            ),
            Err(error) => self.error_for(
                &request,
                -32114,
                format!("cannot read retention policy: {}", error.message),
            ),
        }
    }

    fn retention_policy_set(&mut self, request: Request) -> Vec<Value> {
        let params: RetentionPolicySetParams = match serde_json::from_value(request.params.clone())
        {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        let Some(store) = self.workbench_store.as_mut() else {
            return self.error_for(&request, -32024, "durable retention is not configured");
        };
        let policy = RetentionPolicy {
            scope_kind: params.scope_kind,
            scope_id: params.scope_id,
            archive_after_ms: params.archive_after_ms,
            delete_after_ms: params.delete_after_ms,
            undo_window_ms: params.undo_window_ms,
            delete_scope: params.delete_scope,
            updated_at_ms: now_ms(),
        };
        match store.set_retention_policy(policy) {
            Ok(policy) => self.success_for(
                &request,
                serde_json::to_value(policy).expect("retention policy serialization"),
            ),
            Err(error) => self.error_for(
                &request,
                -32114,
                format!("cannot set retention policy: {}", error.message),
            ),
        }
    }

    fn retention_policy_remove(&mut self, request: Request) -> Vec<Value> {
        let params: RetentionPolicyIdentityParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(error) => {
                    return self.error_for(&request, -32602, format!("invalid params: {error}"))
                }
            };
        let Some(store) = self.workbench_store.as_mut() else {
            return self.error_for(&request, -32024, "durable retention is not configured");
        };
        match store.remove_retention_policy(&params.scope_kind, &params.scope_id, now_ms()) {
            Ok(removed) => self.success_for(
                &request,
                json!({
                    "schema_version": "retention-policy-remove/0.1",
                    "scope_kind": params.scope_kind,
                    "scope_id": params.scope_id,
                    "removed": removed
                }),
            ),
            Err(error) => self.error_for(
                &request,
                -32114,
                format!("cannot remove retention policy: {}", error.message),
            ),
        }
    }

    fn retention_maintenance_run(&mut self, request: Request) -> Vec<Value> {
        let protected_session_ids = self.control.protected_session_ids();
        let observed_at_ms = now_ms();
        let Some(store) = self.workbench_store.as_mut() else {
            return self.error_for(&request, -32024, "durable retention is not configured");
        };
        let blobs = match store.garbage_collect_durable_blobs(observed_at_ms) {
            Ok(report) => report,
            Err(error) if error.code == "session-recovery-blocks-blob-gc" => {
                return self.error_for(
                    &request,
                    -32115,
                    "content garbage collection is disabled during session recovery",
                )
            }
            Err(error) => {
                return self.error_for(
                    &request,
                    -32114,
                    format!("cannot collect retained content: {}", error.message),
                )
            }
        };
        let retention = match store.apply_retention_policies(observed_at_ms, &protected_session_ids)
        {
            Ok(report) => report,
            Err(error) => {
                return self.error_for(
                    &request,
                    -32114,
                    format!("cannot apply retention policies: {}", error.message),
                )
            }
        };
        let deletions = match store.sweep_session_deletions(observed_at_ms) {
            Ok(report) => report,
            Err(error) => {
                return self.error_for(
                    &request,
                    -32114,
                    format!("cannot sweep session deletions: {}", error.message),
                )
            }
        };
        self.evict_purged_resident_sessions();
        self.success_for(
            &request,
            json!({
                "schema_version": "retention-maintenance/0.1",
                "observed_at_ms": observed_at_ms,
                "protected_session_count": protected_session_ids.len(),
                "retention": retention,
                "deletions": deletions,
                "blobs": blobs
            }),
        )
    }

    fn session_deletion_live_reasons(&self, member_ids: &[String]) -> Vec<String> {
        let members = member_ids
            .iter()
            .map(String::as_str)
            .collect::<BTreeSet<_>>();
        let mut reasons = Vec::new();
        if member_ids
            .iter()
            .any(|session_id| self.control.has_active_turn(session_id))
        {
            reasons.push("session-active-turn".into());
        }
        let terminals = self
            .control
            .terminals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        if terminals.records.values().any(|terminal| {
            members.contains(terminal.session_id.as_str())
                && matches!(terminal.state.as_str(), "running" | "stopping")
        }) {
            reasons.push("session-running-terminal".into());
        }
        reasons
    }

    fn evict_purged_resident_sessions(&mut self) {
        let resident_ids = self.sessions.keys().cloned().collect::<Vec<_>>();
        let purged_ids = self
            .workbench_store
            .as_ref()
            .map(|store| {
                resident_ids
                    .into_iter()
                    .filter(|session_id| {
                        store
                            .session_deletion_for_session(session_id)
                            .ok()
                            .flatten()
                            .is_some_and(|receipt| receipt.state == "purged")
                    })
                    .collect::<Vec<_>>()
            })
            .unwrap_or_default();
        for session_id in purged_ids {
            self.sessions.remove(&session_id);
            self.archived_sessions.remove(&session_id);
            self.command_artifacts.remove_session(&session_id);
            self.workspace_edit_previews.remove_session(&session_id);
            self.control
                .terminals
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner())
                .purge_session(&session_id);
        }
    }

    fn session_start(&mut self, request: Request) -> Vec<Value> {
        let params: SessionStartParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        let cwd = match self.session_cwd(&params) {
            Ok(cwd) => cwd,
            Err((code, message)) => return self.error_for(&request, code, message),
        };
        let chat = params.mode == SessionMode::Chat;
        let backend_result: Result<(Option<String>, BackendInfo), (i64, String)> = match &mut self
            .backend
        {
            Backend::Preview => Ok((
                None,
                BackendInfo {
                    adapter: "preview".into(),
                    version: env!("CARGO_PKG_VERSION").into(),
                    provider: Some("local".into()),
                    model: Some("deterministic-echo".into()),
                    permission_profile: "read-only".into(),
                    environment: None,
                },
            )),
            Backend::Codex(adapter) => match adapter.start_session(&cwd, chat) {
                Ok(codex) => {
                    let mut info = adapter.info();
                    info.provider = Some(codex.provider);
                    info.model = Some(codex.model);
                    Ok((Some(codex.thread_id), info))
                }
                Err(error) => Err((-32110, error)),
            },
            Backend::Recovery(_) => Err((-32120, "workbench is in read-only recovery mode".into())),
            Backend::Unavailable(error) => Err((-32100, error.clone())),
        };
        let (backend_session_id, backend_info) = match backend_result {
            Ok(result) => result,
            Err((code, error)) => return self.error_for(&request, code, error),
        };

        let id = self.allocate_id("session");
        let mode_name = if params.mode == SessionMode::Work {
            "work"
        } else {
            "chat"
        };
        let environment =
            SessionEnvironment::build(&id, params.project_id.as_deref(), mode_name, &cwd);
        let session = Session {
            id: id.clone(),
            mode: params.mode,
            project_id: params.project_id.clone(),
            title: params.title.unwrap_or_else(|| "New session".into()),
        };
        let runtime_binding = StoredSessionRuntimeBindingCreate {
            session_id: id.clone(),
            adapter: backend_info.adapter.clone(),
            adapter_version: backend_info.version.clone(),
            backend_session_id: backend_session_id.clone(),
            provider: backend_info.provider.clone(),
            model: backend_info.model.clone(),
            permission_profile: backend_info.permission_profile.clone(),
            created_at_ms: now_ms(),
        };
        if let Err(error) = self.persist_session(&session, &environment, runtime_binding) {
            return self.error_for(&request, -32113, format!("cannot persist session: {error}"));
        }
        self.sessions.insert(
            id,
            SessionState {
                session: session.clone(),
                items: Vec::new(),
                backend_session_id,
                backend_info: backend_info.clone(),
                environment: environment.clone(),
            },
        );
        self.success_for(
            &request,
            json!({
                "session": session,
                "runtime": backend_info,
                "environment": environment.summary()
            }),
        )
    }

    fn session_resume(&mut self, request: Request) -> Vec<Value> {
        let params: SessionIdentityParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        if self.sessions.contains_key(&params.session_id) {
            let state = self
                .sessions
                .get(&params.session_id)
                .expect("session checked above");
            return self.success_for(
                &request,
                json!({
                    "schema_version": "session-resume/0.1",
                    "session": state.session,
                    "resumed": false,
                    "already_active": true,
                    "runtime": state.backend_info,
                    "environment": state.environment.summary(),
                    "continuation": {
                        "provider_state_available": state.backend_session_id.is_some(),
                        "requires_new_runtime_fork": false
                    }
                }),
            );
        }
        let Some(store) = self.workbench_store.as_ref() else {
            return self.error_for(&request, -32023, "durable session resume is not configured");
        };
        let stored = match store.load_readable_session(&params.session_id) {
            Ok(session) => session,
            Err(error) => return self.error_for(&request, -32023, error.message),
        };
        if stored.status == "archived" {
            return self.error_for(
                &request,
                -32026,
                "archived session must be restored before resume",
            );
        }
        let restored_items = match self.workbench_store.as_ref() {
            Some(store) => match hydrate_runtime_session_items(store, &stored.session_id) {
                Ok(items) => items,
                Err(error) => {
                    return self.error_for(
                        &request,
                        -32114,
                        format!("cannot restore session history: {error}"),
                    )
                }
            },
            None => Vec::new(),
        };
        let binding = match store.load_session_runtime_binding(&params.session_id) {
            Ok(binding) => binding,
            Err(error) => {
                return self.error_for(
                    &request,
                    -32141,
                    format!(
                        "session provider continuation is unavailable; fork is required: {}",
                        error.message
                    ),
                )
            }
        };
        let mode = match stored.mode {
            StoredSessionMode::Chat => SessionMode::Chat,
            StoredSessionMode::Work => SessionMode::Work,
        };
        let start_params = SessionStartParams {
            mode: mode.clone(),
            project_id: stored.project_id.clone(),
            title: Some(stored.title.clone()),
        };
        let cwd = match self.session_cwd(&start_params) {
            Ok(cwd) if cwd.is_dir() => cwd,
            Ok(_) => return self.error_for(&request, -32020, "session workspace is unavailable"),
            Err((code, message)) => return self.error_for(&request, code, message),
        };
        let chat = mode == SessionMode::Chat;
        let (backend_session_id, backend_info) = match &mut self.backend {
            Backend::Preview if binding.adapter == "preview" => (
                None,
                BackendInfo {
                    adapter: "preview".into(),
                    version: env!("CARGO_PKG_VERSION").into(),
                    provider: binding.provider.clone().or_else(|| Some("local".into())),
                    model: binding
                        .model
                        .clone()
                        .or_else(|| Some("deterministic-echo".into())),
                    permission_profile: "read-only".into(),
                    environment: None,
                },
            ),
            Backend::Codex(adapter) if binding.adapter == "codex-app-server" => {
                if binding.adapter_version != adapter.info().version {
                    return self.error_for(
                        &request,
                        -32142,
                        "session runtime binding version is incompatible with this Codex adapter",
                    );
                }
                let Some(thread_id) = binding.backend_session_id.as_deref() else {
                    return self.error_for(
                        &request,
                        -32141,
                        "Codex thread binding is unavailable; fork is required",
                    );
                };
                let resumed = match adapter.resume_session(thread_id, &cwd, chat) {
                    Ok(session) => session,
                    Err(error) => return self.error_for(&request, -32143, error),
                };
                if resumed.thread_id != thread_id {
                    return self.error_for(
                        &request,
                        -32143,
                        "Codex resume returned a different thread identity",
                    );
                }
                let mut info = adapter.info();
                info.provider = Some(resumed.provider);
                info.model = Some(resumed.model);
                (Some(resumed.thread_id), info)
            }
            Backend::Recovery(_) => {
                return self.error_for(&request, -32120, "workbench is in read-only recovery mode")
            }
            Backend::Unavailable(error) => {
                let message = error.clone();
                return self.error_for(&request, -32100, message);
            }
            _ => {
                return self.error_for(
                    &request,
                    -32141,
                    "session runtime adapter is unavailable; fork is required",
                )
            }
        };
        let environment = SessionEnvironment::build(
            &stored.session_id,
            stored.project_id.as_deref(),
            if chat { "chat" } else { "work" },
            &cwd,
        );
        let environment_changed = stored.environment_identity.as_deref()
            != Some(environment.summary().environment_id.as_str());
        let resumed = match self.workbench_store.as_mut() {
            Some(store) => match store.record_session_resume(
                &stored.session_id,
                &environment.summary().environment_id,
                now_ms(),
            ) {
                Ok(session) => session,
                Err(error) => return self.error_for(&request, -32113, error.message),
            },
            None => stored,
        };
        let session = Session {
            id: resumed.session_id.clone(),
            mode,
            project_id: resumed.project_id.clone(),
            title: resumed.title.clone(),
        };
        self.sessions.insert(
            session.id.clone(),
            SessionState {
                session: session.clone(),
                items: restored_items,
                backend_session_id,
                backend_info: backend_info.clone(),
                environment: environment.clone(),
            },
        );
        self.success_for(
            &request,
            json!({
                "schema_version": "session-resume/0.1",
                "session": session,
                "resumed": true,
                "already_active": false,
                "runtime": backend_info,
                "environment": environment.summary(),
                "environment_changed": environment_changed,
                "continuation": {
                    "provider_state_available": true,
                    "requires_new_runtime_fork": false
                }
            }),
        )
    }

    fn session_fork(&mut self, request: Request) -> Vec<Value> {
        let params: SessionForkParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        let source = match self
            .workbench_store
            .as_ref()
            .ok_or_else(|| "durable session fork is not configured".to_owned())
            .and_then(|store| {
                store
                    .load_readable_session(&params.session_id)
                    .map_err(|error| error.message)
            }) {
            Ok(session) => session,
            Err(error) => return self.error_for(&request, -32023, error),
        };
        if self.control.has_active_turn(&params.session_id) {
            return self.error_for(
                &request,
                -32026,
                "cannot fork while the source turn is active",
            );
        }
        let source_binding = self
            .workbench_store
            .as_ref()
            .and_then(|store| store.load_session_runtime_binding(&params.session_id).ok());
        let exported_at_ms = now_ms();
        let preview = {
            let store = self
                .workbench_store
                .as_ref()
                .expect("fork store disappeared");
            match params.last_turn_id.as_deref() {
                Some(turn_id) => store.preview_portable_session_export_through_turn(
                    &params.session_id,
                    turn_id,
                    exported_at_ms,
                ),
                None => store.preview_portable_session_export(&params.session_id, exported_at_ms),
            }
        };
        let preview = match preview {
            Ok(preview) => preview,
            Err(error) => return self.error_for(&request, -32144, error.message),
        };
        if !preview.blocking_reasons.is_empty() {
            return self.error_for(&request, -32144, "source session cannot be forked safely");
        }
        let target_session_id = self.allocate_id("session");
        let import_id = self.allocate_id("fork");
        let mode = match source.mode {
            StoredSessionMode::Chat => SessionMode::Chat,
            StoredSessionMode::Work => SessionMode::Work,
        };
        let start_params = SessionStartParams {
            mode: mode.clone(),
            project_id: source.project_id.clone(),
            title: params.title.clone(),
        };
        let cwd = match self.session_cwd(&start_params) {
            Ok(cwd) if cwd.is_dir() => cwd,
            Ok(_) => {
                return self.error_for(&request, -32020, "source session workspace is unavailable")
            }
            Err((code, message)) => return self.error_for(&request, code, message),
        };
        let chat = mode == SessionMode::Chat;
        let (backend_session_id, backend_info, adapter_name) = match &mut self.backend {
            Backend::Preview => (
                None,
                BackendInfo {
                    adapter: "preview".into(),
                    version: env!("CARGO_PKG_VERSION").into(),
                    provider: Some("local".into()),
                    model: Some("deterministic-echo".into()),
                    permission_profile: "read-only".into(),
                    environment: None,
                },
                "preview",
            ),
            Backend::Codex(adapter) => {
                let Some(binding) = source_binding.as_ref() else {
                    return self.error_for(
                        &request,
                        -32141,
                        "Codex provider state is unavailable; fork requires a portable new runtime",
                    );
                };
                if binding.adapter != "codex-app-server"
                    || binding.adapter_version != adapter.info().version
                {
                    return self.error_for(
                        &request,
                        -32142,
                        "source session Codex binding is incompatible with this adapter",
                    );
                }
                let Some(thread_id) = binding.backend_session_id.as_deref() else {
                    return self.error_for(
                        &request,
                        -32141,
                        "Codex thread binding is unavailable; fork requires a portable new runtime",
                    );
                };
                let forked = match adapter.fork_session(
                    thread_id,
                    params.last_turn_id.as_deref(),
                    &cwd,
                    chat,
                ) {
                    Ok(session) => session,
                    Err(error) => return self.error_for(&request, -32143, error),
                };
                let mut info = adapter.info();
                info.provider = Some(forked.provider);
                info.model = Some(forked.model);
                (Some(forked.thread_id), info, "codex-app-server")
            }
            Backend::Recovery(_) => {
                return self.error_for(&request, -32120, "workbench is in read-only recovery mode")
            }
            Backend::Unavailable(error) => {
                let message = error.clone();
                return self.error_for(&request, -32100, message);
            }
        };
        let environment = SessionEnvironment::build(
            &target_session_id,
            source.project_id.as_deref(),
            if chat { "chat" } else { "work" },
            &cwd,
        );
        let binding = StoredSessionRuntimeBindingCreate {
            session_id: target_session_id.clone(),
            adapter: adapter_name.into(),
            adapter_version: backend_info.version.clone(),
            backend_session_id: backend_session_id.clone(),
            provider: backend_info.provider.clone(),
            model: backend_info.model.clone(),
            permission_profile: backend_info.permission_profile.clone(),
            created_at_ms: now_ms(),
        };
        let package = {
            let store = self
                .workbench_store
                .as_ref()
                .expect("fork store disappeared");
            match params.last_turn_id.as_deref() {
                Some(turn_id) => store.export_portable_session_through_turn(
                    &params.session_id,
                    turn_id,
                    &preview.package_hash,
                    now_ms(),
                ),
                None => store.export_portable_session(
                    &params.session_id,
                    &preview.package_hash,
                    now_ms(),
                ),
            }
        };
        let package = match package {
            Ok(package) => package,
            Err(error) => return self.error_for(&request, -32144, error.message),
        };
        let mut receipt = match self
            .workbench_store
            .as_mut()
            .expect("fork store disappeared")
            .import_portable_session(PortableSessionImportCommand {
                target_session_id: &target_session_id,
                import_id: &import_id,
                package: &package,
                target_project_id: source.project_id.as_deref(),
                reject_source_collisions: false,
                target_environment_identity: Some(&environment.summary().environment_id),
                runtime_binding: Some(&binding),
                imported_at_ms: binding.created_at_ms,
            }) {
            Ok(receipt) => receipt,
            Err(error) => {
                if adapter_name == "codex-app-server" {
                    if let Some(thread_id) = backend_session_id.as_deref() {
                        if let Backend::Codex(adapter) = &mut self.backend {
                            let _ = adapter.archive_session(thread_id);
                        }
                    }
                }
                return self.error_for(&request, -32113, error.message);
            }
        };
        if let Some(title) = params.title.as_deref() {
            let updated = match self
                .workbench_store
                .as_mut()
                .expect("fork store disappeared")
                .update_session_title(&target_session_id, title, now_ms())
            {
                Ok(session) => session,
                Err(error) => return self.error_for(&request, -32113, error.message),
            };
            receipt.session = updated;
        }
        let stored_items = match self
            .workbench_store
            .as_ref()
            .expect("fork store disappeared")
            .read_session_items(&target_session_id, 0, receipt.imported_items as usize)
        {
            Ok(items) => items,
            Err(error) => return self.error_for(&request, -32114, error.message),
        };
        let session = Session {
            id: receipt.session.session_id.clone(),
            mode,
            project_id: receipt.session.project_id.clone(),
            title: receipt.session.title.clone(),
        };
        self.sessions.insert(
            target_session_id,
            SessionState {
                session: session.clone(),
                items: stored_items.into_iter().map(stored_timeline_item).collect(),
                backend_session_id,
                backend_info: backend_info.clone(),
                environment: environment.clone(),
            },
        );
        self.success_for(
            &request,
            json!({
                "schema_version": "session-fork/0.1",
                "session": session,
                "receipt": receipt,
                "source_session_id": params.session_id,
                "boundary_turn_id": params.last_turn_id,
                "runtime": backend_info,
                "environment": environment.summary(),
                "continuation": {
                    "provider_state_available": adapter_name == "codex-app-server",
                    "portable_history_items": receipt.imported_items,
                    "requires_new_runtime_fork": false
                }
            }),
        )
    }

    fn session_cwd(&self, params: &SessionStartParams) -> Result<PathBuf, (i64, String)> {
        if params.mode == SessionMode::Work {
            let project_id = params
                .project_id
                .as_ref()
                .ok_or_else(|| (-32021, "Work session requires a project".to_owned()))?;
            return self
                .projects
                .get(project_id)
                .map(|project| PathBuf::from(&project.root))
                .or_else(|| {
                    self.workbench_store.as_ref().and_then(|store| {
                        store
                            .load_project(project_id)
                            .ok()
                            .map(|project| PathBuf::from(project.canonical_root))
                    })
                })
                .ok_or_else(|| (-32022, "project not found".to_owned()));
        }
        let root = std::env::temp_dir().join("aegisy-agent-chat");
        fs::create_dir_all(&root).map_err(|error| {
            (
                -32024,
                format!("cannot create isolated Chat workspace: {error}"),
            )
        })?;
        Ok(root)
    }

    fn persist_project(&mut self, project: &Project) -> Result<(), String> {
        let Some(store) = self.workbench_store.as_mut() else {
            return Ok(());
        };
        let root_identity = filesystem_root_identity(Path::new(&project.root))?;
        store
            .create_project(StoredProjectCreate {
                project_id: project.id.clone(),
                root_id: "root-1".into(),
                canonical_root: project.root.clone(),
                root_identity,
                display_name: project.name.clone(),
                root_access: "write".into(),
                created_at_ms: now_ms(),
            })
            .map(|_| ())
            .map_err(|cause| cause.message)
    }

    fn persist_session(
        &mut self,
        session: &Session,
        environment: &SessionEnvironment,
        runtime_binding: StoredSessionRuntimeBindingCreate,
    ) -> Result<(), String> {
        let Some(store) = self.workbench_store.as_mut() else {
            return Ok(());
        };
        let mode = match session.mode {
            SessionMode::Chat => StoredSessionMode::Chat,
            SessionMode::Work => StoredSessionMode::Work,
        };
        store
            .create_session_with_runtime_binding(
                StoredSessionCreate {
                    session_id: session.id.clone(),
                    project_id: session.project_id.clone(),
                    mode,
                    title: session.title.clone(),
                    parent_session_id: None,
                    lineage_kind: StoredSessionLineage::New,
                    environment_identity: Some(environment.summary().environment_id.clone()),
                    created_at_ms: runtime_binding.created_at_ms,
                },
                Some(runtime_binding),
            )
            .map(|_| ())
            .map_err(|cause| cause.message)
    }

    fn persist_turn(
        &mut self,
        session_id: &str,
        turn_id: &str,
        input: &str,
        idempotency_key: &str,
    ) -> Result<(), String> {
        let Some(store) = self.workbench_store.as_mut() else {
            return Ok(());
        };
        store
            .create_turn(StoredTurnCreate {
                turn_id: turn_id.into(),
                session_id: session_id.into(),
                idempotency_key: Some(idempotency_key.into()),
                input_hash: ContentHash::for_bytes(input.as_bytes()),
                created_at_ms: now_ms(),
            })
            .map(|_| ())
            .map_err(|cause| cause.message)
    }

    fn persist_item(
        &mut self,
        session_id: &str,
        turn_id: Option<&str>,
        item: &TimelineItem,
    ) -> Result<(), String> {
        let Some(store) = self.workbench_store.as_mut() else {
            return Ok(());
        };
        store
            .append_item(StoredItemAppend {
                session_id: session_id.into(),
                turn_id: turn_id.map(str::to_owned),
                item_id: item.id.clone(),
                item_kind: item.kind.clone(),
                role: item.role.clone(),
                state: item.state.clone(),
                payload: json!({ "content": item.content, "data": item.data }),
                created_at_ms: now_ms(),
            })
            .map(|_| ())
            .map_err(|cause| cause.message)
    }

    fn persist_item_with_command_artifact(
        &mut self,
        session_id: &str,
        turn_id: Option<&str>,
        item: &TimelineItem,
        artifact: Option<&command_artifact::CommandOutputArtifact>,
    ) -> Result<(), String> {
        let Some(store) = self.workbench_store.as_mut() else {
            return Ok(());
        };
        let item_append = StoredItemAppend {
            session_id: session_id.into(),
            turn_id: turn_id.map(str::to_owned),
            item_id: item.id.clone(),
            item_kind: item.kind.clone(),
            role: item.role.clone(),
            state: item.state.clone(),
            payload: json!({ "content": item.content, "data": item.data }),
            created_at_ms: now_ms(),
        };
        let Some(artifact) = artifact else {
            return store
                .append_item(item_append)
                .map(|_| ())
                .map_err(|cause| cause.message);
        };
        let project_id = self
            .sessions
            .get(session_id)
            .and_then(|state| state.session.project_id.clone())
            .or_else(|| {
                store
                    .load_session(session_id)
                    .ok()
                    .and_then(|session| session.project_id)
            });
        let reference_id = durable_blob_reference_id(
            Some(session_id),
            project_id.as_deref(),
            "item",
            &item.id,
            &artifact.reference,
        );
        let retain_until_ms = artifact
            .created_at_ms
            .checked_add(DURABLE_COMMAND_ARTIFACT_RETENTION_MS)
            .ok_or_else(|| "command artifact retention time is out of range".to_owned())?;
        store
            .append_item_with_durable_blob(
                item_append,
                DurableBlobWrite {
                    reference_id,
                    content_reference: artifact.reference.clone(),
                    session_id: Some(session_id.into()),
                    project_id,
                    kind: DurableBlobKind::CommandOutput,
                    media_type: artifact.content_type.clone(),
                    owner_kind: "item".into(),
                    owner_id: item.id.clone(),
                    metadata: json!({
                        "item_id": artifact.item_id,
                        "source_bytes": artifact.source_bytes,
                        "redacted_count": artifact.redacted_count,
                        "redacted": artifact.redacted,
                        "total_bytes": artifact.total_bytes,
                        "retained_bytes": artifact.retained_bytes,
                        "omitted_bytes": artifact.omitted_bytes,
                        "truncated": artifact.truncated
                    }),
                    content: artifact.content.as_bytes().to_vec(),
                    created_at_ms: artifact.created_at_ms,
                    retain_until_ms,
                },
            )
            .map(|_| ())
            .map_err(|cause| cause.message)
    }

    fn persist_workspace_edit_artifacts(
        &mut self,
        session_id: &str,
        project_id: &str,
        edit_id: &str,
        artifacts: Vec<PreviewArtifactSnapshot>,
    ) -> Result<(), String> {
        let Some(store) = self.workbench_store.as_mut() else {
            return Ok(());
        };
        let created_at_ms = now_ms();
        let retain_until_ms = created_at_ms
            .checked_add(DURABLE_PREVIEW_RETENTION_MS)
            .ok_or_else(|| "workspace edit artifact retention time is out of range".to_owned())?;
        let requests = artifacts
            .into_iter()
            .map(|artifact| {
                let reference_id = durable_blob_reference_id(
                    Some(session_id),
                    Some(project_id),
                    "edit",
                    edit_id,
                    &artifact.reference,
                );
                let artifact_role = if artifact.reference.starts_with("workspace-edit-diff:") {
                    "diff"
                } else {
                    "proposed-content"
                };
                DurableBlobWrite {
                    reference_id,
                    content_reference: artifact.reference,
                    session_id: Some(session_id.into()),
                    project_id: Some(project_id.into()),
                    kind: DurableBlobKind::WorkspaceEdit,
                    media_type: artifact.media_type,
                    owner_kind: "edit".into(),
                    owner_id: edit_id.into(),
                    metadata: json!({
                        "edit_id": edit_id,
                        "artifact_role": artifact_role,
                        "retention": "workspace-edit-preview"
                    }),
                    content: artifact.bytes,
                    created_at_ms,
                    retain_until_ms,
                }
            })
            .collect::<Vec<_>>();
        store
            .put_durable_blobs(requests)
            .map(|_| ())
            .map_err(|cause| cause.message)
    }

    fn persist_turn_state(
        &mut self,
        session_id: &str,
        turn_id: &str,
        state: &str,
    ) -> Result<(), String> {
        let Some(store) = self.workbench_store.as_mut() else {
            return Ok(());
        };
        store
            .finish_turn(session_id, turn_id, state, now_ms())
            .map(|_| ())
            .map_err(|cause| cause.message)
    }

    fn turn_start_stream<F>(&mut self, request: Request, emit: &mut F)
    where
        F: FnMut(Value),
    {
        let params: TurnStartParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                self.emit_all(
                    self.error_for(&request, -32602, format!("invalid params: {error}")),
                    emit,
                );
                return;
            }
        };
        if params.input.trim().is_empty() || params.idempotency_key.trim().is_empty() {
            self.emit_all(
                self.error_for(
                    &request,
                    -32602,
                    "input and idempotency_key must not be empty",
                ),
                emit,
            );
            return;
        }
        if self.archived_sessions.contains(&params.session_id) {
            self.emit_all(
                self.error_for(
                    &request,
                    -32026,
                    "cannot start a turn in an archived session",
                ),
                emit,
            );
            return;
        }
        let Some(state) = self.sessions.get(&params.session_id) else {
            self.emit_all(self.error_for(&request, -32023, "session not found"), emit);
            return;
        };
        let backend_session_id = state.backend_session_id.clone();
        let (command_environment, command_environment_binding) =
            if let Some(environment) = state.backend_info.environment.clone() {
                (environment, "codex-adapter-launch-contract")
            } else {
                (
                    state.environment.summary().clone(),
                    "unverified-session-snapshot",
                )
            };
        let context_roots = state.session.project_id.as_ref().map(|project_id| {
            let roots = self
                .project_roots
                .get(project_id)
                .cloned()
                .or_else(|| {
                    self.workbench_store
                        .as_ref()
                        .and_then(|store| store.load_project_roots(project_id).ok())
                })
                .unwrap_or_default();
            roots
                .into_iter()
                .filter_map(|root| {
                    self.workspace_root_binding(project_id, Some(&root.root_id), false)
                        .ok()
                        .map(|(_, path)| (root.root_id, path))
                })
                .collect::<HashMap<_, _>>()
        });
        let prepared_context =
            match prepare_turn_context_scoped(&params.context, context_roots.as_ref()) {
                Ok(context) => context,
                Err(error) => {
                    self.emit_all(self.error_for(&request, error.code, error.message), emit);
                    return;
                }
            };
        let backend_input = if prepared_context.text.is_empty() {
            params.input.clone()
        } else {
            format!("{}\n\n{}", params.input.trim(), prepared_context.text)
        };
        let user_item = TimelineItem {
            id: self.allocate_id("item"),
            kind: "message".into(),
            role: "user".into(),
            state: "completed".into(),
            content: params.input.clone(),
            data: None,
        };

        match &self.backend {
            Backend::Preview => {
                self.preview_turn(
                    request,
                    params,
                    backend_input,
                    prepared_context,
                    user_item,
                    emit,
                );
                return;
            }
            Backend::Recovery(_) => {
                self.emit_all(
                    self.error_for(&request, -32120, "workbench is in read-only recovery mode"),
                    emit,
                );
                return;
            }
            Backend::Unavailable(error) => {
                self.emit_all(self.error_for(&request, -32100, error.clone()), emit);
                return;
            }
            Backend::Codex(_) => {}
        }
        let Some(codex_thread_id) = backend_session_id else {
            self.emit_all(
                self.error_for(&request, -32111, "Codex thread is unavailable"),
                emit,
            );
            return;
        };

        let mut backend = std::mem::replace(
            &mut self.backend,
            Backend::Unavailable("Codex turn is already running".into()),
        );
        let cancellation = self.control.begin_turn(&params.session_id);
        let steering = self
            .control
            .steering_handle(&params.session_id)
            .expect("active turn steering handle");
        let mut started = false;
        let mut persistence_error: Option<String> = None;
        let mut metadata_update_counts = HashMap::<String, usize>::new();
        let mut metadata_truncation_notified = HashSet::<String>::new();
        let result = match &mut backend {
            Backend::Codex(adapter) => adapter.run_turn(
                &codex_thread_id,
                &backend_input,
                &params.idempotency_key,
                &cancellation,
                &steering,
                |event| match event {
                    CodexEvent::TurnStarted { turn_id } => {
                        if let Err(error) = self.persist_turn(
                            &params.session_id,
                            &turn_id,
                            &backend_input,
                            &params.idempotency_key,
                        ) {
                            persistence_error = Some(format!("cannot persist turn: {error}"));
                            return;
                        }
                        if let Err(error) =
                            self.persist_item(&params.session_id, Some(&turn_id), &user_item)
                        {
                            let _ = self.persist_turn_state(&params.session_id, &turn_id, "failed");
                            persistence_error = Some(format!("cannot persist user item: {error}"));
                            return;
                        }
                        started = true;
                        self.control.identify_turn(&params.session_id, &turn_id);
                        emit(
                            serde_json::to_value(Response::success(
                                request.id.clone().unwrap_or(Value::Null),
                                json!({
                                    "turn": { "id": turn_id, "state": "started" },
                                    "context": {
                                        "item_count": prepared_context.item_count,
                                        "bytes": prepared_context.bytes,
                                        "truncated": prepared_context.truncated
                                    }
                                }),
                            ))
                            .expect("response serialization"),
                        );
                        emit(self.event(&params.session_id, Some(&turn_id), "turn.started", None));
                        if let Some(state) = self.sessions.get_mut(&params.session_id) {
                            state.items.push(user_item.clone());
                        }
                        emit(self.event(
                            &params.session_id,
                            Some(&turn_id),
                            "item.completed",
                            Some(user_item.clone()),
                        ));
                    }
                    CodexEvent::AgentDelta {
                        turn_id,
                        item_id,
                        text,
                    } => emit(self.event(
                        &params.session_id,
                        Some(&turn_id),
                        "item.delta",
                        Some(TimelineItem {
                            id: item_id,
                            kind: "message".into(),
                            role: "agent".into(),
                            state: "delta".into(),
                            content: text,
                            data: None,
                        }),
                    )),
                    CodexEvent::AgentCompleted {
                        turn_id,
                        item_id,
                        text,
                    } => {
                        let item = TimelineItem {
                            id: item_id,
                            kind: "message".into(),
                            role: "agent".into(),
                            state: "completed".into(),
                            content: text,
                            data: None,
                        };
                        if let Err(error) =
                            self.persist_item(&params.session_id, Some(&turn_id), &item)
                        {
                            persistence_error = Some(format!("cannot persist agent item: {error}"));
                            return;
                        }
                        if let Some(state) = self.sessions.get_mut(&params.session_id) {
                            state.items.push(item.clone());
                        }
                        emit(self.event(
                            &params.session_id,
                            Some(&turn_id),
                            "item.completed",
                            Some(item),
                        ));
                    }
                    CodexEvent::CommandUpdated {
                        turn_id,
                        command,
                        lifecycle,
                    } => {
                        let mut artifact = if lifecycle == "completed" {
                            self.command_artifacts.record(
                                &params.session_id,
                                &command.item_id,
                                &command.output,
                                command.redactor.source_bytes(),
                                command.redactor.redacted_count(),
                            )
                        } else {
                            None
                        };
                        let command_diagnostics = if lifecycle == "completed" {
                            self.record_command_diagnostics(
                                &params.session_id,
                                &command,
                                artifact.as_ref(),
                            )
                        } else {
                            None
                        };
                        if let Some((_, _, _, source_artifact)) = &command_diagnostics {
                            artifact = Some(source_artifact.clone());
                        }
                        let item = command_timeline_item(
                            &command,
                            &lifecycle,
                            &command_environment,
                            command_environment_binding,
                            artifact.as_ref(),
                            &params.session_id,
                        );
                        if lifecycle == "completed" {
                            if let Err(error) = self.persist_item_with_command_artifact(
                                &params.session_id,
                                Some(&turn_id),
                                &item,
                                artifact.as_ref(),
                            ) {
                                persistence_error =
                                    Some(format!("cannot persist command item: {error}"));
                                return;
                            }
                            if let Some(state) = self.sessions.get_mut(&params.session_id) {
                                state.items.push(item.clone());
                            }
                        }
                        let event_name = match lifecycle.as_str() {
                            "started" => "item.started",
                            "completed" => "item.completed",
                            _ => "item.delta",
                        };
                        emit(self.event(
                            &params.session_id,
                            Some(&turn_id),
                            event_name,
                            Some(item),
                        ));
                        if let Some((project_id, toolchain, observation, _)) = command_diagnostics {
                            let mut data = serde_json::to_value(&observation)
                                .expect("command diagnostic observation serialization");
                            if let Some(object) = data.as_object_mut() {
                                object.insert("project_id".into(), Value::String(project_id));
                                object
                                    .insert("source_kind".into(), Value::String("command".into()));
                                object.insert(
                                    "source_identity".into(),
                                    Value::String(format!("command:{toolchain}")),
                                );
                            }
                            let diagnostic_item = TimelineItem {
                                id: format!("diagnostics-{}", command.item_id),
                                kind: "diagnostic".into(),
                                role: "tool".into(),
                                state: "completed".into(),
                                content: format!(
                                    "{} reported {} observed diagnostic(s)",
                                    toolchain,
                                    observation.diagnostics.len()
                                ),
                                data: Some(data),
                            };
                            if let Err(error) = self.persist_item(
                                &params.session_id,
                                Some(&turn_id),
                                &diagnostic_item,
                            ) {
                                persistence_error =
                                    Some(format!("cannot persist diagnostic item: {error}"));
                                return;
                            }
                            if let Some(state) = self.sessions.get_mut(&params.session_id) {
                                state.items.push(diagnostic_item.clone());
                            }
                            emit(self.event(
                                &params.session_id,
                                Some(&turn_id),
                                "diagnostics.observed",
                                Some(diagnostic_item),
                            ));
                        }
                    }
                    CodexEvent::TokenUsage { turn_id, usage } => {
                        let key = format!("usage:{turn_id}");
                        let count = metadata_update_counts.entry(key.clone()).or_default();
                        if *count < MAX_TURN_METADATA_UPDATES_PER_KIND {
                            *count += 1;
                            let item = TimelineItem {
                                id: self.allocate_id("usage"),
                                kind: "usage".into(),
                                role: "system".into(),
                                state: "updated".into(),
                                content: "Token usage updated".into(),
                                data: Some(usage),
                            };
                            if let Err(error) =
                                self.persist_item(&params.session_id, Some(&turn_id), &item)
                            {
                                persistence_error =
                                    Some(format!("cannot persist usage item: {error}"));
                                return;
                            }
                            if let Some(state) = self.sessions.get_mut(&params.session_id) {
                                state.items.push(item.clone());
                            }
                            emit(self.event(
                                &params.session_id,
                                Some(&turn_id),
                                "usage.updated",
                                Some(item),
                            ));
                        } else if metadata_truncation_notified.insert(key) {
                            let item = TimelineItem {
                                id: self.allocate_id("usage-truncated"),
                                kind: "usage".into(),
                                role: "system".into(),
                                state: "truncated".into(),
                                content: "Token usage updates truncated".into(),
                                data: Some(json!({
                                    "max_updates": MAX_TURN_METADATA_UPDATES_PER_KIND
                                })),
                            };
                            emit(self.event(
                                &params.session_id,
                                Some(&turn_id),
                                "usage.truncated",
                                Some(item),
                            ));
                        }
                    }
                    CodexEvent::TurnDiff { turn_id, diff } => {
                        let key = format!("diff:{turn_id}");
                        let count = metadata_update_counts.entry(key.clone()).or_default();
                        if *count < MAX_TURN_METADATA_UPDATES_PER_KIND {
                            *count += 1;
                            let item = TimelineItem {
                                id: self.allocate_id("diff"),
                                kind: "diff".into(),
                                role: "tool".into(),
                                state: "updated".into(),
                                content: diff,
                                data: Some(json!({ "projection": "bounded-unified-diff" })),
                            };
                            if let Err(error) =
                                self.persist_item(&params.session_id, Some(&turn_id), &item)
                            {
                                persistence_error =
                                    Some(format!("cannot persist diff item: {error}"));
                                return;
                            }
                            if let Some(state) = self.sessions.get_mut(&params.session_id) {
                                state.items.push(item.clone());
                            }
                            emit(self.event(
                                &params.session_id,
                                Some(&turn_id),
                                "turn.diff.updated",
                                Some(item),
                            ));
                        } else if metadata_truncation_notified.insert(key) {
                            let item = TimelineItem {
                                id: self.allocate_id("diff-truncated"),
                                kind: "diff".into(),
                                role: "tool".into(),
                                state: "truncated".into(),
                                content: "Diff updates truncated".into(),
                                data: Some(json!({
                                    "max_updates": MAX_TURN_METADATA_UPDATES_PER_KIND
                                })),
                            };
                            emit(self.event(
                                &params.session_id,
                                Some(&turn_id),
                                "turn.diff.truncated",
                                Some(item),
                            ));
                        }
                    }
                    CodexEvent::TurnPlan {
                        turn_id,
                        explanation,
                        steps,
                    } => {
                        let key = format!("plan:{turn_id}");
                        let count = metadata_update_counts.entry(key.clone()).or_default();
                        if *count < MAX_TURN_METADATA_UPDATES_PER_KIND {
                            *count += 1;
                            let item = TimelineItem {
                                id: self.allocate_id("plan"),
                                kind: "plan".into(),
                                role: "agent".into(),
                                state: "updated".into(),
                                content: explanation.unwrap_or_else(|| "Plan updated".into()),
                                data: Some(json!({ "steps": steps })),
                            };
                            if let Err(error) =
                                self.persist_item(&params.session_id, Some(&turn_id), &item)
                            {
                                persistence_error =
                                    Some(format!("cannot persist plan item: {error}"));
                                return;
                            }
                            if let Some(state) = self.sessions.get_mut(&params.session_id) {
                                state.items.push(item.clone());
                            }
                            emit(self.event(
                                &params.session_id,
                                Some(&turn_id),
                                "turn.plan.updated",
                                Some(item),
                            ));
                        } else if metadata_truncation_notified.insert(key) {
                            let item = TimelineItem {
                                id: self.allocate_id("plan-truncated"),
                                kind: "plan".into(),
                                role: "agent".into(),
                                state: "truncated".into(),
                                content: "Plan updates truncated".into(),
                                data: Some(json!({
                                    "max_updates": MAX_TURN_METADATA_UPDATES_PER_KIND
                                })),
                            };
                            emit(self.event(
                                &params.session_id,
                                Some(&turn_id),
                                "turn.plan.truncated",
                                Some(item),
                            ));
                        }
                    }
                    CodexEvent::TurnCompleted { turn_id } => {
                        if let Err(error) =
                            self.persist_turn_state(&params.session_id, &turn_id, "completed")
                        {
                            persistence_error =
                                Some(format!("cannot persist turn completion: {error}"));
                            return;
                        }
                        emit(self.event(&params.session_id, Some(&turn_id), "turn.completed", None))
                    }
                    CodexEvent::TurnSteeringRequested { turn_id, input } => {
                        let item = TimelineItem {
                            id: self.allocate_id("item"),
                            kind: "message".into(),
                            role: "user".into(),
                            state: "completed".into(),
                            content: input,
                            data: Some(json!({ "source": "turn-steer" })),
                        };
                        if let Err(error) =
                            self.persist_item(&params.session_id, Some(&turn_id), &item)
                        {
                            persistence_error =
                                Some(format!("cannot persist steering item: {error}"));
                            return;
                        }
                        if let Some(state) = self.sessions.get_mut(&params.session_id) {
                            state.items.push(item.clone());
                        }
                        emit(self.event(
                            &params.session_id,
                            Some(&turn_id),
                            "turn.steering-requested",
                            Some(item),
                        ));
                    }
                    CodexEvent::TurnSteeringAcknowledged { turn_id } => emit(self.event(
                        &params.session_id,
                        Some(&turn_id),
                        "turn.steering-acknowledged",
                        None,
                    )),
                    CodexEvent::TurnSteeringFailed { turn_id, message } => {
                        let item = TimelineItem {
                            id: self.allocate_id("error"),
                            kind: "error".into(),
                            role: "system".into(),
                            state: "completed".into(),
                            content: runtime_error_content(&message),
                            data: Some({
                                let mut data = runtime_error_data(&message);
                                data["operation"] = Value::String("turn.steer".into());
                                data
                            }),
                        };
                        emit(self.event(
                            &params.session_id,
                            Some(&turn_id),
                            "turn.steering-failed",
                            Some(item),
                        ));
                    }
                    CodexEvent::TurnCancellationAcknowledged { turn_id } => emit(self.event(
                        &params.session_id,
                        Some(&turn_id),
                        "turn.cancellation-acknowledged",
                        None,
                    )),
                    CodexEvent::TurnCancellationFailed { turn_id, message } => {
                        let item = TimelineItem {
                            id: self.allocate_id("error"),
                            kind: "error".into(),
                            role: "system".into(),
                            state: "completed".into(),
                            content: runtime_error_content(&message),
                            data: Some({
                                let mut data = runtime_error_data(&message);
                                data["operation"] = Value::String("turn.cancel".into());
                                data
                            }),
                        };
                        emit(self.event(
                            &params.session_id,
                            Some(&turn_id),
                            "turn.cancellation-failed",
                            Some(item),
                        ));
                    }
                    CodexEvent::TurnInterrupted { turn_id } => {
                        if let Err(error) =
                            self.persist_turn_state(&params.session_id, &turn_id, "interrupted")
                        {
                            persistence_error =
                                Some(format!("cannot persist interrupted turn: {error}"));
                            return;
                        }
                        emit(self.event(
                            &params.session_id,
                            Some(&turn_id),
                            "turn.interrupted",
                            None,
                        ));
                    }
                    CodexEvent::TurnFailed { turn_id, message } => {
                        if let Err(error) =
                            self.persist_turn_state(&params.session_id, &turn_id, "failed")
                        {
                            persistence_error =
                                Some(format!("cannot persist failed turn: {error}"));
                            return;
                        }
                        let item = TimelineItem {
                            id: self.allocate_id("error"),
                            kind: "error".into(),
                            role: "system".into(),
                            state: "completed".into(),
                            content: runtime_error_content(&message),
                            data: Some(runtime_error_data(&message)),
                        };
                        emit(self.event(
                            &params.session_id,
                            Some(&turn_id),
                            "turn.failed",
                            Some(item),
                        ));
                    }
                },
            ),
            Backend::Preview | Backend::Recovery(_) | Backend::Unavailable(_) => {
                unreachable!("backend was checked before turn execution")
            }
        };
        self.backend = backend;
        self.control.finish_turn(&params.session_id);
        if let Some(error) = persistence_error {
            if started {
                let mut data = runtime_error_data(&error);
                data["operation"] = Value::String("workbench.persistence".into());
                let item = TimelineItem {
                    id: self.allocate_id("error"),
                    kind: "error".into(),
                    role: "system".into(),
                    state: "completed".into(),
                    content: error.clone(),
                    data: Some(data),
                };
                emit(self.event(
                    &params.session_id,
                    None,
                    "turn.persistence-failed",
                    Some(item),
                ));
            } else {
                self.emit_all(self.error_for(&request, -32113, error), emit);
            }
            return;
        }
        if let Err(error) = result {
            if started {
                let item = TimelineItem {
                    id: self.allocate_id("error"),
                    kind: "error".into(),
                    role: "system".into(),
                    state: "completed".into(),
                    content: runtime_error_content(&error),
                    data: Some(runtime_error_data(&error)),
                };
                emit(self.event(&params.session_id, None, "turn.failed", Some(item)));
            } else {
                self.emit_all(
                    self.error_for(&request, -32112, runtime_error_content(&error)),
                    emit,
                );
            }
        }
    }

    fn preview_turn<F>(
        &mut self,
        request: Request,
        params: TurnStartParams,
        backend_input: String,
        prepared_context: turn_context::PreparedTurnContext,
        user_item: TimelineItem,
        emit: &mut F,
    ) where
        F: FnMut(Value),
    {
        let turn_id = self.allocate_id("turn");
        let agent_item = TimelineItem {
            id: self.allocate_id("item"),
            kind: "message".into(),
            role: "agent".into(),
            state: "completed".into(),
            content: format!("AAP runtime preview received:\n{}", backend_input.trim()),
            data: None,
        };
        if let Err(error) = self.persist_turn(
            &params.session_id,
            &turn_id,
            &backend_input,
            &params.idempotency_key,
        ) {
            self.emit_all(
                self.error_for(&request, -32113, format!("cannot persist turn: {error}")),
                emit,
            );
            return;
        }
        if let Err(error) = self.persist_item(&params.session_id, Some(&turn_id), &user_item) {
            let _ = self.persist_turn_state(&params.session_id, &turn_id, "failed");
            self.emit_all(
                self.error_for(
                    &request,
                    -32113,
                    format!("cannot persist user item: {error}"),
                ),
                emit,
            );
            return;
        }
        if let Err(error) = self.persist_item(&params.session_id, Some(&turn_id), &agent_item) {
            let _ = self.persist_turn_state(&params.session_id, &turn_id, "failed");
            self.emit_all(
                self.error_for(
                    &request,
                    -32113,
                    format!("cannot persist agent item: {error}"),
                ),
                emit,
            );
            return;
        }
        if let Err(error) = self.persist_turn_state(&params.session_id, &turn_id, "completed") {
            self.emit_all(
                self.error_for(
                    &request,
                    -32113,
                    format!("cannot persist turn completion: {error}"),
                ),
                emit,
            );
            return;
        }
        if let Some(state) = self.sessions.get_mut(&params.session_id) {
            state.items.push(user_item.clone());
            state.items.push(agent_item.clone());
        }
        emit(
            serde_json::to_value(Response::success(
                request.id.unwrap_or(Value::Null),
                json!({
                    "turn": { "id": turn_id, "state": "started" },
                    "context": {
                        "item_count": prepared_context.item_count,
                        "bytes": prepared_context.bytes,
                        "truncated": prepared_context.truncated
                    }
                }),
            ))
            .expect("response serialization"),
        );
        emit(self.event(&params.session_id, Some(&turn_id), "turn.started", None));
        emit(self.event(
            &params.session_id,
            Some(&turn_id),
            "item.completed",
            Some(user_item),
        ));
        let mut delta = agent_item.clone();
        delta.state = "delta".into();
        emit(self.event(
            &params.session_id,
            Some(&turn_id),
            "item.delta",
            Some(delta),
        ));
        emit(self.event(
            &params.session_id,
            Some(&turn_id),
            "item.completed",
            Some(agent_item),
        ));
        emit(self.event(&params.session_id, Some(&turn_id), "turn.completed", None));
    }

    fn verify_session_projection_for_read(
        &mut self,
        session_id: &str,
        verify: bool,
    ) -> Result<(Option<SessionProjectionConsistency>, &'static str), (i64, String)> {
        if !verify {
            return Ok((None, "not-evaluated"));
        }
        let Some(store) = self.workbench_store.as_mut() else {
            return Ok((None, "not-configured"));
        };
        let (report, rebuilt) = store
            .verify_or_rebuild_session_projection(session_id)
            .map_err(|_| (-32115, "cannot verify session projection".into()))?;
        if !report.consistent {
            return Err((
                -32115,
                "session projection is inconsistent; read-only recovery is required".into(),
            ));
        }
        let startup_rebuilt = store.take_startup_rebuild_notice(session_id);
        Ok((
            Some(report),
            if rebuilt || startup_rebuilt {
                "projection-rebuilt"
            } else {
                "not-needed"
            },
        ))
    }

    fn session_read(&mut self, request: Request) -> Vec<Value> {
        let params: SessionReadParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(error) => {
                return self.error_for(&request, -32602, format!("invalid params: {error}"))
            }
        };
        if let Some(store) = self.workbench_store.as_ref() {
            match store.session_deletion_for_session(&params.session_id) {
                Ok(Some(receipt)) if receipt.state == "purged" => {
                    return self.error_for(&request, -32023, "session not found")
                }
                Ok(_) => {}
                Err(error) => {
                    return self.error_for(
                        &request,
                        -32114,
                        format!("cannot inspect session deletion state: {}", error.message),
                    )
                }
            }
        }
        let before_sequence = match parse_session_history_cursor(params.cursor.as_deref()) {
            Ok(cursor) => cursor,
            Err(error) => return self.error_for(&request, -32602, error),
        };
        if params.limit == 0 || params.limit > 200 {
            return self.error_for(
                &request,
                -32602,
                "session history limit must be between 1 and 200",
            );
        }
        let durable_session = self
            .workbench_store
            .as_ref()
            .is_some_and(|store| store.load_session(&params.session_id).is_ok());
        if durable_session {
            let Some(store) = self.workbench_store.as_ref() else {
                return self.error_for(&request, -32023, "session not found");
            };
            if store.load_session(&params.session_id).is_err() {
                if store.session_requires_recovery(&params.session_id) {
                    return self.error_for(
                        &request,
                        -32115,
                        "session projection is missing; read-only recovery is required",
                    );
                }
                return self.error_for(&request, -32023, "session not found");
            }
            let (consistency, recovery_status) = match self
                .verify_session_projection_for_read(&params.session_id, before_sequence.is_none())
            {
                Ok(result) => result,
                Err((code, message)) => return self.error_for(&request, code, message),
            };
            let store = self
                .workbench_store
                .as_ref()
                .expect("durable session requires a workbench store");
            let stored = match store.load_session(&params.session_id) {
                Ok(session) => session,
                Err(_) => return self.error_for(&request, -32023, "session not found"),
            };
            let mode = match stored.mode {
                StoredSessionMode::Chat => SessionMode::Chat,
                StoredSessionMode::Work => SessionMode::Work,
            };
            let session = Session {
                id: stored.session_id.clone(),
                mode,
                project_id: stored.project_id.clone(),
                title: stored.title.clone(),
            };
            let latest_sequence = match store.latest_session_item_sequence(&stored.session_id) {
                Ok(sequence) => sequence,
                Err(error) => {
                    return self.error_for(
                        &request,
                        -32114,
                        format!("cannot read session history boundary: {}", error.message),
                    )
                }
            };
            let (after_sequence, page_len) =
                match session_history_window(latest_sequence, before_sequence, params.limit) {
                    Ok(window) => window,
                    Err(error) => return self.error_for(&request, -32602, error),
                };
            let page = if page_len == 0 {
                Vec::new()
            } else {
                match store.read_session_items(&stored.session_id, after_sequence, page_len) {
                    Ok(page) => page,
                    Err(error) => {
                        return self.error_for(
                            &request,
                            -32114,
                            format!("cannot replay session items: {}", error.message),
                        )
                    }
                }
            };
            if page.len() != page_len
                || page
                    .last()
                    .is_some_and(|item| item.sequence != after_sequence + page_len as u64)
            {
                return self.error_for(
                    &request,
                    -32114,
                    "cannot replay session items: item replay boundary is inconsistent",
                );
            }
            let first_sequence = page.first().map(|item| item.sequence);
            let last_sequence = page.last().map(|item| item.sequence);
            let items = page
                .into_iter()
                .map(|item| {
                    let sequence = item.sequence;
                    let timeline_item = stored_timeline_item(item);
                    timeline_item_value(&timeline_item, sequence)
                })
                .collect::<Vec<_>>();
            return self.success_for(
                &request,
                json!({
                    "session": session,
                    "status": stored.status,
                    "items": items,
                    "consistency": consistency,
                    "recovery": {
                        "schema_version": "session-projection-read-recovery/0.1",
                        "status": recovery_status,
                        "audit_event": if recovery_status == "projection-rebuilt" {
                            Some("session.projection-rebuilt")
                        } else {
                            None
                        }
                    },
                    "history_page": session_history_page(
                        first_sequence,
                        last_sequence,
                        latest_sequence,
                        params.limit,
                    ),
                    "runtime": {
                        "adapter": "durable-store-replay",
                        "version": env!("CARGO_PKG_VERSION"),
                        "permission_profile": "read-only",
                        "replayed": true
                    },
                    "environment": {
                        "environment_id": stored.environment_identity,
                        "replayed": true,
                        "available": false
                    }
                }),
            );
        }
        let (consistency, recovery_status) = match self
            .verify_session_projection_for_read(&params.session_id, before_sequence.is_none())
        {
            Ok(result) => result,
            Err((code, message)) => return self.error_for(&request, code, message),
        };
        let state = self
            .sessions
            .get(&params.session_id)
            .expect("in-memory session disappeared during replay");
        let latest_sequence = state.items.len() as u64;
        let (after_sequence, page_len) =
            match session_history_window(latest_sequence, before_sequence, params.limit) {
                Ok(window) => window,
                Err(error) => return self.error_for(&request, -32602, error),
            };
        let start = after_sequence as usize;
        let end = start + page_len;
        let items = state.items[start..end]
            .iter()
            .enumerate()
            .map(|(offset, item)| timeline_item_value(item, after_sequence + offset as u64 + 1))
            .collect::<Vec<_>>();
        let first_sequence = (page_len > 0).then_some(after_sequence + 1);
        let last_sequence = (page_len > 0).then_some(after_sequence + page_len as u64);
        self.success_for(
            &request,
            json!({
                "session": &state.session,
                "status": if self.archived_sessions.contains(&state.session.id) {
                    "archived"
                } else {
                    "active"
                },
                "items": items,
                "consistency": consistency,
                "recovery": {
                    "schema_version": "session-projection-read-recovery/0.1",
                    "status": recovery_status,
                    "audit_event": if recovery_status == "projection-rebuilt" {
                        Some("session.projection-rebuilt")
                    } else {
                        None
                    }
                },
                "history_page": session_history_page(
                    first_sequence,
                    last_sequence,
                    latest_sequence,
                    params.limit,
                ),
                "runtime": &state.backend_info,
                "environment": state.environment.summary()
            }),
        )
    }

    fn terminal_open_user(&mut self, request: Request) -> Vec<Value> {
        let params: TerminalOpenParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        let kind = match normalize_terminal_kind(&params.kind) {
            Ok(kind) => kind,
            Err((code, message)) => return self.error_for(&request, code, message),
        };
        let name = match normalize_terminal_name(&kind, params.name.as_deref()) {
            Ok(name) => name,
            Err((code, message)) => return self.error_for(&request, code, message),
        };
        let (project_id, root, environment) =
            match self.terminal_session_binding(&params.session_id) {
                Ok(binding) => binding,
                Err((code, message)) => return self.error_for(&request, code, message),
            };
        if let Err((code, message)) =
            self.validate_terminal_open_identity(&params.session_id, &kind, &name)
        {
            return self.error_for(&request, code, message);
        }
        let terminal_id = self.allocate_id("terminal");
        let mut terminals = self
            .control
            .terminals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        match terminals.manager.open_user(
            terminal_id.clone(),
            TerminalOpenContext {
                session_id: &params.session_id,
                project_id: &project_id,
                root: &root,
                environment: &environment,
            },
            params.rows,
            params.cols,
        ) {
            Ok(snapshot) => {
                let timestamp = now_ms();
                terminals.records.insert(
                    terminal_id.clone(),
                    TerminalRecord {
                        terminal_id: terminal_id.clone(),
                        session_id: params.session_id,
                        project_id,
                        kind,
                        name,
                        generation: 1,
                        state: "running".into(),
                        input_policy: "user-only".into(),
                        created_at_ms: timestamp,
                        updated_at_ms: timestamp,
                    },
                );
                let value = terminals.decorate_snapshot(&terminal_id, snapshot);
                self.success_for(&request, value)
            }
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn terminal_list(&mut self, request: Request) -> Vec<Value> {
        let params: SessionReadParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        if let Err((code, message)) = self.terminal_session_binding(&params.session_id) {
            return self.error_for(&request, code, message);
        }
        let mut terminal_state = self
            .control
            .terminals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let mut ids = terminal_state
            .records
            .values()
            .filter(|record| record.session_id == params.session_id)
            .map(|record| record.terminal_id.clone())
            .collect::<Vec<_>>();
        ids.sort_by_key(|id| {
            terminal_state
                .records
                .get(id)
                .map(|record| (record.created_at_ms, record.terminal_id.clone()))
        });
        let mut terminal_values = Vec::with_capacity(ids.len());
        for terminal_id in ids {
            match terminal_state.snapshot_value(&terminal_id, &params.session_id, u64::MAX) {
                Ok(snapshot) => terminal_values.push(snapshot),
                Err(error) => return self.error_for(&request, error.code, error.message),
            }
        }
        self.success_for(
            &request,
            json!({ "session_id": params.session_id, "terminals": terminal_values }),
        )
    }

    fn terminal_read(&mut self, request: Request) -> Vec<Value> {
        let params: TerminalReadParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        if let Err((code, message)) = self.terminal_session_binding(&params.session_id) {
            return self.error_for(&request, code, message);
        }
        match self.terminal_snapshot_value(&params.terminal_id, &params.session_id, params.after) {
            Ok(snapshot) => self.success_for(&request, snapshot),
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn terminal_input_user(&mut self, request: Request) -> Vec<Value> {
        let params: TerminalInputParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        if let Err((code, message)) = self.terminal_session_binding(&params.session_id) {
            return self.error_for(&request, code, message);
        }
        let result = self
            .control
            .terminals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .manager
            .input_user(&params.terminal_id, &params.session_id, &params.data_base64);
        match result {
            Ok(written) => self.success_for(
                &request,
                json!({
                    "terminal_id": params.terminal_id,
                    "session_id": params.session_id,
                    "written": written
                }),
            ),
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn terminal_resize(&mut self, request: Request) -> Vec<Value> {
        let params: TerminalResizeParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        if let Err((code, message)) = self.terminal_session_binding(&params.session_id) {
            return self.error_for(&request, code, message);
        }
        let mut terminal_state = self
            .control
            .terminals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        match terminal_state.manager.resize(
            &params.terminal_id,
            &params.session_id,
            params.rows,
            params.cols,
        ) {
            Ok(()) => {
                if let Some(record) = terminal_state.records.get_mut(&params.terminal_id) {
                    record.updated_at_ms = now_ms();
                }
                self.success_for(
                    &request,
                    json!({
                        "terminal_id": params.terminal_id,
                        "session_id": params.session_id,
                        "rows": params.rows,
                        "cols": params.cols
                    }),
                )
            }
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn terminal_signal_user(&mut self, request: Request) -> Vec<Value> {
        let params: TerminalSignalParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        if let Err((code, message)) = self.terminal_session_binding(&params.session_id) {
            return self.error_for(&request, code, message);
        }
        let result = self
            .control
            .terminals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .manager
            .signal_user(&params.terminal_id, &params.session_id, &params.signal);
        match result {
            Ok(()) => self.success_for(
                &request,
                json!({
                    "terminal_id": params.terminal_id,
                    "session_id": params.session_id,
                    "signal": params.signal
                }),
            ),
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn terminal_close_user(&mut self, request: Request) -> Vec<Value> {
        let params: TerminalReadParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        if let Err((code, message)) = self.terminal_session_binding(&params.session_id) {
            return self.error_for(&request, code, message);
        }
        let result = self
            .control
            .terminals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .stop_user(&params.terminal_id, &params.session_id);
        match result {
            Ok(snapshot) => self.success_for(&request, snapshot),
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn terminal_restart_user(&mut self, request: Request) -> Vec<Value> {
        let params: TerminalRestartParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        let (project_id, root, environment) =
            match self.terminal_session_binding(&params.session_id) {
                Ok(binding) => binding,
                Err((code, message)) => return self.error_for(&request, code, message),
            };
        let mut terminal_state = self
            .control
            .terminals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        let Some(previous) = terminal_state.records.get(&params.terminal_id).cloned() else {
            return self.error_for(&request, -32093, "terminal not found");
        };
        if previous.session_id != params.session_id {
            return self.error_for(&request, -32093, "terminal does not belong to session");
        }
        let current =
            match terminal_state
                .manager
                .snapshot(&params.terminal_id, &params.session_id, u64::MAX)
            {
                Ok(snapshot) => snapshot,
                Err(error) => return self.error_for(&request, error.code, error.message),
            };
        let current_value =
            serde_json::to_value(&current).expect("terminal snapshot serialization");
        let rows = params
            .rows
            .or_else(|| {
                current_value
                    .get("rows")
                    .and_then(Value::as_u64)
                    .map(|v| v as u16)
            })
            .unwrap_or_else(default_terminal_rows);
        let cols = params
            .cols
            .or_else(|| {
                current_value
                    .get("cols")
                    .and_then(Value::as_u64)
                    .map(|v| v as u16)
            })
            .unwrap_or_else(default_terminal_cols);
        if rows == 0 || cols == 0 || rows > 1_000 || cols > 1_000 {
            return self.error_for(
                &request,
                -32602,
                "terminal rows and cols must be between 1 and 1000",
            );
        }
        if let Err(error) = terminal_state
            .manager
            .remove_user(&params.terminal_id, &params.session_id)
        {
            return self.error_for(&request, error.code, error.message);
        }
        match terminal_state.manager.open_user(
            params.terminal_id.clone(),
            TerminalOpenContext {
                session_id: &params.session_id,
                project_id: &project_id,
                root: &root,
                environment: &environment,
            },
            rows,
            cols,
        ) {
            Ok(snapshot) => {
                let timestamp = now_ms();
                if let Some(record) = terminal_state.records.get_mut(&params.terminal_id) {
                    record.generation = record.generation.saturating_add(1);
                    record.state = "running".into();
                    record.updated_at_ms = timestamp;
                }
                let value = terminal_state.decorate_snapshot(&params.terminal_id, snapshot);
                self.success_for(&request, value)
            }
            Err(error) => {
                terminal_state.records.remove(&params.terminal_id);
                self.error_for(&request, error.code, error.message)
            }
        }
    }

    fn terminal_remove_user(&mut self, request: Request) -> Vec<Value> {
        let params: TerminalReadParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        if let Err((code, message)) = self.terminal_session_binding(&params.session_id) {
            return self.error_for(&request, code, message);
        }
        let snapshot =
            match self.terminal_snapshot_value(&params.terminal_id, &params.session_id, u64::MAX) {
                Ok(snapshot) => snapshot,
                Err(error) => return self.error_for(&request, error.code, error.message),
            };
        if snapshot.get("running").and_then(Value::as_bool) == Some(true) {
            return self.error_for(&request, -32097, "running terminal must be stopped first");
        }
        let mut terminal_state = self
            .control
            .terminals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        if let Err(error) = terminal_state
            .manager
            .remove_user(&params.terminal_id, &params.session_id)
        {
            return self.error_for(&request, error.code, error.message);
        }
        let removed = terminal_state.records.remove(&params.terminal_id);
        self.success_for(
            &request,
            json!({
                "terminal_id": params.terminal_id,
                "session_id": params.session_id,
                "removed": removed.is_some()
            }),
        )
    }

    fn validate_terminal_open_identity(
        &mut self,
        session_id: &str,
        kind: &str,
        name: &str,
    ) -> Result<(), (i64, String)> {
        self.control
            .terminals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .validate_open_identity(session_id, kind, name)
    }

    fn terminal_snapshot_value(
        &mut self,
        terminal_id: &str,
        session_id: &str,
        after: u64,
    ) -> Result<Value, TerminalError> {
        self.control
            .terminals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .snapshot_value(terminal_id, session_id, after)
    }

    fn terminal_session_binding(
        &self,
        session_id: &str,
    ) -> Result<(String, PathBuf, SessionEnvironment), (i64, String)> {
        if self.archived_sessions.contains(session_id) {
            return Err((-32026, "session is archived".into()));
        }
        let state = self
            .sessions
            .get(session_id)
            .ok_or_else(|| (-32023, "session not found".to_owned()))?;
        if state.session.mode != SessionMode::Work {
            return Err((
                -32095,
                "user terminals require a project-bound Work session".into(),
            ));
        }
        let project_id = state
            .session
            .project_id
            .as_ref()
            .ok_or_else(|| (-32021, "Work session requires a project".to_owned()))?;
        let project = self
            .projects
            .get(project_id)
            .ok_or_else(|| (-32022, "project not found".to_owned()))?;
        Ok((
            project_id.clone(),
            PathBuf::from(&project.root),
            state.environment.clone(),
        ))
    }

    fn workspace_edit_session_binding(
        &self,
        session_id: &str,
    ) -> Result<(String, PathBuf), (i64, String)> {
        if self.archived_sessions.contains(session_id) {
            return Err((-32026, "session is archived".into()));
        }
        let Some(state) = self.sessions.get(session_id) else {
            let store = self
                .workbench_store
                .as_ref()
                .ok_or_else(|| (-32023, "session not found".to_owned()))?;
            let session = store
                .load_session(session_id)
                .map_err(|_| (-32023, "session not found".to_owned()))?;
            if session.status == "archived" {
                return Err((-32026, "session is archived".into()));
            }
            if session.mode != StoredSessionMode::Work {
                return Err((
                    -32095,
                    "workspace edit preview requires a project-bound Work session".into(),
                ));
            }
            let project_id = session
                .project_id
                .ok_or_else(|| (-32021, "Work session requires a project".to_owned()))?;
            let project = store
                .load_project(&project_id)
                .map_err(|_| (-32022, "project not found".to_owned()))?;
            return Ok((project_id, PathBuf::from(project.canonical_root)));
        };
        if state.session.mode != SessionMode::Work {
            return Err((
                -32095,
                "workspace edit preview requires a project-bound Work session".into(),
            ));
        }
        let project_id = state
            .session
            .project_id
            .as_ref()
            .ok_or_else(|| (-32021, "Work session requires a project".to_owned()))?;
        let project = self
            .projects
            .get(project_id)
            .ok_or_else(|| (-32022, "project not found".to_owned()))?;
        Ok((project_id.clone(), PathBuf::from(&project.root)))
    }

    fn workspace_root_binding(
        &self,
        project_id: &str,
        root_id: Option<&str>,
        require_write: bool,
    ) -> Result<(workbench_store::StoredProjectRoot, PathBuf), (i64, String)> {
        if !self.projects.contains_key(project_id) {
            return Err((-32022, "project not found".into()));
        }
        let root_id = root_id.unwrap_or("root-1");
        let roots = self
            .project_roots
            .get(project_id)
            .cloned()
            .or_else(|| {
                self.workbench_store
                    .as_ref()
                    .and_then(|store| store.load_project_roots(project_id).ok())
            })
            .ok_or_else(|| (-32022, "project roots are unavailable".to_owned()))?;
        let root = roots
            .into_iter()
            .find(|root| root.root_id == root_id)
            .ok_or_else(|| (-32022, "project root not found".to_owned()))?;
        if require_write && root.access != "write" {
            return Err((-32046, "project root is read-only".into()));
        }
        let path = PathBuf::from(&root.canonical_root);
        if !path.is_dir() {
            return Err((
                -32020,
                "project root is unavailable and requires relink".into(),
            ));
        }
        if fs::symlink_metadata(&path)
            .map(|metadata| metadata.file_type().is_symlink())
            .unwrap_or(true)
        {
            return Err((-32020, "project root symlink policy changed".into()));
        }
        let canonical = path
            .canonicalize()
            .map_err(|_| (-32020, "project root is unavailable".to_owned()))?;
        if canonical != path {
            return Err((-32020, "project root canonical path changed".into()));
        }
        let current_identity = filesystem_root_identity(&canonical)
            .map_err(|_| (-32020, "project root identity is unavailable".to_owned()))?;
        if !root.root_identity.starts_with("path:sha256:") && current_identity != root.root_identity
        {
            return Err((
                -32020,
                "project root identity changed and requires review".into(),
            ));
        }
        Ok((root, canonical))
    }

    fn workspace_scope_key(project_id: &str, root_id: &str) -> String {
        format!("{project_id}\0{root_id}")
    }

    fn operation_reconciliation_key(session_id: &str, operation_id: &str) -> String {
        format!("{session_id}\0{operation_id}")
    }

    fn workspace_list(&self, request: Request) -> Vec<Value> {
        let params: WorkspacePathParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        let (root_binding, root) =
            match self.workspace_root_binding(&params.project_id, params.root_id.as_deref(), false)
            {
                Ok(binding) => binding,
                Err((code, message)) => return self.error_for(&request, code, message),
            };
        match list_directory(&root, &params.path) {
            Ok(mut listing) => {
                let candidates: Vec<String> = listing
                    .entries
                    .iter()
                    .map(|entry| entry.path.clone())
                    .collect();
                let ignored = ignored_paths(&root, &candidates);
                listing
                    .entries
                    .retain(|entry| !ignored.contains(&entry.path));
                listing.ignored_count += ignored.len();
                let mut value =
                    serde_json::to_value(listing).expect("workspace listing serialization");
                if let Some(object) = value.as_object_mut() {
                    object.insert("project_id".into(), params.project_id.into());
                    object.insert("root_id".into(), root_binding.root_id.into());
                    object.insert("root_access".into(), root_binding.access.into());
                }
                self.success_for(&request, value)
            }
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn workspace_read(&self, request: Request) -> Vec<Value> {
        let params: WorkspacePathParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        let (root_binding, root) =
            match self.workspace_root_binding(&params.project_id, params.root_id.as_deref(), false)
            {
                Ok(binding) => binding,
                Err((code, message)) => return self.error_for(&request, code, message),
            };
        if ignored_paths(&root, std::slice::from_ref(&params.path)).contains(&params.path) {
            return self.error_for(
                &request,
                -32035,
                "workspace file is ignored by project policy",
            );
        }
        match read_text_file(&root, &params.path) {
            Ok(file) => {
                let mut value = serde_json::to_value(file).expect("workspace file serialization");
                if let Some(object) = value.as_object_mut() {
                    object.insert("project_id".into(), params.project_id.into());
                    object.insert("root_id".into(), root_binding.root_id.into());
                    object.insert("root_access".into(), root_binding.access.into());
                }
                self.success_for(&request, value)
            }
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn workspace_metadata(&self, request: Request) -> Vec<Value> {
        let params: WorkspacePathParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        let (root_binding, root) =
            match self.workspace_root_binding(&params.project_id, params.root_id.as_deref(), false)
            {
                Ok(binding) => binding,
                Err((code, message)) => return self.error_for(&request, code, message),
            };
        if !params.path.is_empty()
            && ignored_paths(&root, std::slice::from_ref(&params.path)).contains(&params.path)
        {
            return self.error_for(
                &request,
                -32035,
                "workspace path is ignored by project policy",
            );
        }
        match path_metadata(&root, &params.path) {
            Ok(metadata) => {
                let mut value =
                    serde_json::to_value(metadata).expect("workspace metadata serialization");
                if let Some(object) = value.as_object_mut() {
                    object.insert("project_id".into(), params.project_id.into());
                    object.insert("root_id".into(), root_binding.root_id.into());
                    object.insert("root_access".into(), root_binding.access.into());
                }
                self.success_for(&request, value)
            }
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn workspace_save_user_text(&mut self, request: Request) -> Vec<Value> {
        let params: WorkspaceSaveParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        if params.origin != "user" {
            return self.error_for(
                &request,
                -32045,
                "workspace save requires an explicit user origin",
            );
        }
        let (root_binding, root) = match self.workspace_root_binding(
            &params.project_id,
            params.root_id.as_deref(),
            true,
        ) {
            Ok(binding) => binding,
            Err((code, message)) => return self.error_for(&request, code, message),
        };
        if ignored_paths(&root, std::slice::from_ref(&params.path)).contains(&params.path) {
            return self.error_for(
                &request,
                -32035,
                "workspace file is ignored by project policy",
            );
        }
        match write_text_file(
            &root,
            &params.path,
            &params.content,
            &params.expected_revision,
            &params.encoding,
            &params.newline,
        ) {
            Ok(saved) => {
                let scope_key =
                    Self::workspace_scope_key(&params.project_id, &root_binding.root_id);
                if let Some(index) = self.workspace_indexes.get_mut(&scope_key) {
                    index.mark_stale();
                }
                self.language_servers
                    .invalidate_document(&scope_key, &params.path);
                self.diagnostic_store
                    .invalidate_path(&scope_key, &params.path);
                if root_binding.root_id == "root-1" {
                    self.diagnostic_store
                        .invalidate_path(&params.project_id, &params.path);
                }
                refresh_watch_after_user_save(
                    &mut self.workspace_watches,
                    &params.project_id,
                    &root_binding.root_id,
                    &root,
                    &params.path,
                );
                let mut value = serde_json::to_value(saved).expect("workspace save serialization");
                if let Some(object) = value.as_object_mut() {
                    object.insert("project_id".into(), params.project_id.into());
                    object.insert("root_id".into(), root_binding.root_id.into());
                    object.insert("root_access".into(), root_binding.access.into());
                }
                self.success_for(&request, value)
            }
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn workspace_git_status(&self, request: Request) -> Vec<Value> {
        let params: WorkspaceProjectParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        let Some(project) = self.projects.get(&params.project_id) else {
            return self.error_for(&request, -32022, "project not found");
        };
        match git_status(Path::new(&project.root)) {
            Ok(status) => self.success_for(
                &request,
                serde_json::to_value(status).expect("git status serialization"),
            ),
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn workspace_git_overview(&self, request: Request) -> Vec<Value> {
        let params: WorkspaceProjectParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        let Some(project) = self.projects.get(&params.project_id) else {
            return self.error_for(&request, -32022, "project not found");
        };
        match git_overview(Path::new(&project.root)) {
            Ok(result) => self.success_for(
                &request,
                serde_json::to_value(result).expect("Git overview serialization"),
            ),
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn workspace_git_log(&self, request: Request) -> Vec<Value> {
        let params: WorkspaceGitLogParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        let Some(project) = self.projects.get(&params.project_id) else {
            return self.error_for(&request, -32022, "project not found");
        };
        match git_log(
            Path::new(&project.root),
            params.limit,
            params.cursor.as_deref(),
        ) {
            Ok(result) => self.success_for(
                &request,
                serde_json::to_value(result).expect("Git log serialization"),
            ),
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn workspace_git_commit(&self, request: Request) -> Vec<Value> {
        let params: WorkspaceGitCommitParams = match serde_json::from_value(request.params.clone())
        {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        let Some(project) = self.projects.get(&params.project_id) else {
            return self.error_for(&request, -32022, "project not found");
        };
        match git_commit(Path::new(&project.root), &params.oid) {
            Ok(result) => self.success_for(
                &request,
                serde_json::to_value(result).expect("Git commit serialization"),
            ),
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn workspace_git_diff(&self, request: Request) -> Vec<Value> {
        let params: WorkspaceGitDiffParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        let Some(project) = self.projects.get(&params.project_id) else {
            return self.error_for(&request, -32022, "project not found");
        };
        match git_diff(
            Path::new(&project.root),
            &params.scope,
            params.oid.as_deref(),
            params.path.as_deref(),
        ) {
            Ok(result) => self.success_for(
                &request,
                serde_json::to_value(result).expect("Git diff serialization"),
            ),
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn workspace_search(&self, request: Request) -> Vec<Value> {
        let params: WorkspaceSearchParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        if params.search_id.is_empty() || params.search_id.len() > 128 {
            return self.error_for(&request, -32050, "invalid workspace search id");
        }
        let (root_binding, root) =
            match self.workspace_root_binding(&params.project_id, params.root_id.as_deref(), false)
            {
                Ok(binding) => binding,
                Err((code, message)) => return self.error_for(&request, code, message),
            };
        let scope_key = format!(
            "{}\0search:{}",
            Self::workspace_scope_key(&params.project_id, &root_binding.root_id),
            params.search_id
        );
        if self.cancelled_workspace_searches.contains(&scope_key)
            || self
                .cancelled_workspace_searches
                .contains(&params.search_id)
        {
            return self.success_for(
                &request,
                json!({
                    "search_id": params.search_id,
                    "project_id": params.project_id,
                    "root_id": root_binding.root_id,
                    "cancelled": true,
                    "matches": [],
                    "next_cursor": Value::Null,
                    "stale": false,
                    "truncated": false
                }),
            );
        }
        let (candidates, candidate_truncated) = match collect_search_candidates(&root) {
            Ok(candidates) => candidates,
            Err(error) => return self.error_for(&request, error.code, error.message),
        };
        let paths = candidates
            .iter()
            .map(|candidate| candidate.path.clone())
            .collect::<Vec<_>>();
        let ignored = ignored_paths(&root, &paths);
        match search_workspace(
            &root,
            &candidates,
            &ignored,
            &params.query,
            &params.mode,
            params.case_sensitive,
            params.cursor.as_deref(),
            params.limit,
            candidate_truncated,
        ) {
            Ok(result) => {
                let mut value =
                    serde_json::to_value(result).expect("workspace search result serialization");
                if let Some(object) = value.as_object_mut() {
                    object.insert("search_id".into(), Value::String(params.search_id));
                    object.insert("project_id".into(), Value::String(params.project_id));
                    object.insert("root_id".into(), Value::String(root_binding.root_id));
                    object.insert("root_access".into(), Value::String(root_binding.access));
                    object.insert("cancelled".into(), Value::Bool(false));
                }
                self.success_for(&request, value)
            }
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn workspace_search_cancel(&mut self, request: Request) -> Vec<Value> {
        let params: WorkspaceSearchCancelParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(cause) => {
                    return self.error_for(&request, -32602, format!("invalid params: {cause}"))
                }
            };
        if params.search_id.is_empty() || params.search_id.len() > 128 {
            return self.error_for(&request, -32050, "invalid workspace search id");
        }
        if self.cancelled_workspace_searches.len() >= 256 {
            self.cancelled_workspace_searches.clear();
        }
        let cancellation_key = if let Some(project_id) = params.project_id.as_deref() {
            let (root_binding, _) =
                match self.workspace_root_binding(project_id, params.root_id.as_deref(), false) {
                    Ok(binding) => binding,
                    Err((code, message)) => return self.error_for(&request, code, message),
                };
            format!(
                "{}\0search:{}",
                Self::workspace_scope_key(project_id, &root_binding.root_id),
                params.search_id
            )
        } else {
            // Legacy clients predate root-scoped cancellation. Their IDs
            // remain globally unique in the Qt client and are checked as a
            // compatibility fallback by workspace_search.
            params.search_id.clone()
        };
        self.cancelled_workspace_searches.insert(cancellation_key);
        self.success_for(
            &request,
            json!({ "search_id": params.search_id, "cancelled": true }),
        )
    }

    fn workspace_index(&mut self, request: Request) -> Vec<Value> {
        let params: WorkspaceIndexParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        let index_id = if params.index_id.is_empty() {
            self.allocate_id("index")
        } else if params.index_id.len() <= 128 {
            params.index_id.clone()
        } else {
            return self.error_for(&request, -32061, "invalid workspace index id");
        };
        let (root_binding, _) =
            match self.workspace_root_binding(&params.project_id, params.root_id.as_deref(), false)
            {
                Ok(binding) => binding,
                Err((code, message)) => return self.error_for(&request, code, message),
            };
        let scope_key = Self::workspace_scope_key(&params.project_id, &root_binding.root_id);
        if self
            .cancelled_workspace_indexes
            .remove(&(scope_key.clone(), index_id.clone()))
        {
            let result = self
                .workspace_indexes
                .entry(scope_key.clone())
                .or_default()
                .cancelled_result();
            let mut value = serde_json::to_value(result)
                .expect("cancelled workspace index result serialization");
            if let Some(object) = value.as_object_mut() {
                object.insert("project_id".into(), Value::String(params.project_id));
                object.insert("root_id".into(), Value::String(root_binding.root_id));
                object.insert("index_id".into(), Value::String(index_id));
            }
            return self.success_for(&request, value);
        }
        match self.refresh_workspace_index(&params.project_id, Some(&root_binding.root_id)) {
            Ok(result) => {
                let mut value =
                    serde_json::to_value(result).expect("workspace index result serialization");
                if let Some(object) = value.as_object_mut() {
                    object.insert("project_id".into(), Value::String(params.project_id));
                    object.insert("root_id".into(), Value::String(root_binding.root_id));
                    object.insert("index_id".into(), Value::String(index_id));
                }
                self.success_for(&request, value)
            }
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn workspace_index_cancel(&mut self, request: Request) -> Vec<Value> {
        let params: WorkspaceIndexCancelParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(cause) => {
                    return self.error_for(&request, -32602, format!("invalid params: {cause}"))
                }
            };
        if params.index_id.is_empty() || params.index_id.len() > 128 {
            return self.error_for(&request, -32061, "invalid workspace index id");
        }
        let (root_binding, _) =
            match self.workspace_root_binding(&params.project_id, params.root_id.as_deref(), false)
            {
                Ok(binding) => binding,
                Err((code, message)) => return self.error_for(&request, code, message),
            };
        let scope_key = Self::workspace_scope_key(&params.project_id, &root_binding.root_id);
        if self.cancelled_workspace_indexes.len() >= 256 {
            self.cancelled_workspace_indexes.clear();
        }
        self.cancelled_workspace_indexes
            .insert((scope_key, params.index_id.clone()));
        self.success_for(
            &request,
            json!({
                "project_id": params.project_id,
                "root_id": root_binding.root_id,
                "index_id": params.index_id,
                "cancelled": true
            }),
        )
    }

    fn workspace_repository_map(&mut self, request: Request) -> Vec<Value> {
        let params: WorkspaceRepositoryMapParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(cause) => {
                    return self.error_for(&request, -32602, format!("invalid params: {cause}"))
                }
            };
        if params.focus_paths.len() > 64 {
            return self.error_for(&request, -32061, "repository map focus path limit exceeded");
        }
        let (root_binding, _) =
            match self.workspace_root_binding(&params.project_id, params.root_id.as_deref(), false)
            {
                Ok(binding) => binding,
                Err((code, message)) => return self.error_for(&request, code, message),
            };
        let scope_key = Self::workspace_scope_key(&params.project_id, &root_binding.root_id);
        if let Err(error) =
            self.refresh_workspace_index(&params.project_id, Some(&root_binding.root_id))
        {
            return self.error_for(&request, error.code, error.message);
        }
        let Some(index) = self.workspace_indexes.get(&scope_key) else {
            return self.error_for(&request, -32060, "workspace index is unavailable");
        };
        let result = index.repository_map(params.token_budget, &params.focus_paths);
        let mut value = serde_json::to_value(result).expect("repository map result serialization");
        if let Some(object) = value.as_object_mut() {
            object.insert("project_id".into(), Value::String(params.project_id));
            object.insert("root_id".into(), Value::String(root_binding.root_id));
        }
        self.success_for(&request, value)
    }

    fn workspace_language_servers(&mut self, request: Request) -> Vec<Value> {
        let params: WorkspaceProjectParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        let (root_binding, root) =
            match self.workspace_root_binding(&params.project_id, params.root_id.as_deref(), false)
            {
                Ok(binding) => binding,
                Err((code, message)) => return self.error_for(&request, code, message),
            };
        let scope_key = Self::workspace_scope_key(&params.project_id, &root_binding.root_id);
        let servers = self.language_servers.statuses(&scope_key, &root);
        self.success_for(
            &request,
            json!({
                "project_id": params.project_id,
                "root_id": root_binding.root_id,
                "root_access": root_binding.access,
                "servers": servers
            }),
        )
    }

    fn workspace_language_server_start(&mut self, request: Request) -> Vec<Value> {
        let params: WorkspacePathParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        let (root_binding, root) = match self.validate_language_path(
            &params.project_id,
            params.root_id.as_deref(),
            &params.path,
        ) {
            Ok(root) => root,
            Err(error) => return self.error_for(&request, error.code, error.message),
        };
        let scope_key = Self::workspace_scope_key(&params.project_id, &root_binding.root_id);
        match self.language_servers.start(&scope_key, &root, &params.path) {
            Ok(status) => self.success_for(
                &request,
                json!({
                    "project_id": params.project_id,
                    "root_id": root_binding.root_id,
                    "status": status
                }),
            ),
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn workspace_language_server_stop(&mut self, request: Request) -> Vec<Value> {
        let params: WorkspacePathParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        let (root_binding, _) =
            match self.workspace_root_binding(&params.project_id, params.root_id.as_deref(), false)
            {
                Ok(binding) => binding,
                Err((code, message)) => return self.error_for(&request, code, message),
            };
        let scope_key = Self::workspace_scope_key(&params.project_id, &root_binding.root_id);
        match self.language_servers.stop(&scope_key, &params.path) {
            Ok(stopped) => self.success_for(
                &request,
                json!({
                    "project_id": params.project_id,
                    "root_id": root_binding.root_id,
                    "stopped": stopped
                }),
            ),
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn workspace_definition(&mut self, request: Request) -> Vec<Value> {
        let params: WorkspaceLanguageDocumentParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(cause) => {
                    return self.error_for(&request, -32602, format!("invalid params: {cause}"))
                }
            };
        let (root_binding, root) = match self.validate_language_document(&params) {
            Ok(root) => root,
            Err(error) => return self.error_for(&request, error.code, error.message),
        };
        let scope_key = Self::workspace_scope_key(&params.project_id, &root_binding.root_id);
        match self.language_servers.definition(
            &scope_key,
            &root,
            &params.path,
            &params.content,
            &params.revision,
            params.line,
            params.column,
        ) {
            Ok(result) => self.language_result(&request, &params, &root_binding.root_id, result),
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn workspace_references(&mut self, request: Request) -> Vec<Value> {
        let params: WorkspaceLanguageDocumentParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(cause) => {
                    return self.error_for(&request, -32602, format!("invalid params: {cause}"))
                }
            };
        let (root_binding, root) = match self.validate_language_document(&params) {
            Ok(root) => root,
            Err(error) => return self.error_for(&request, error.code, error.message),
        };
        let scope_key = Self::workspace_scope_key(&params.project_id, &root_binding.root_id);
        match self.language_servers.references(
            &scope_key,
            &root,
            &params.path,
            &params.content,
            &params.revision,
            params.line,
            params.column,
        ) {
            Ok(result) => self.language_result(&request, &params, &root_binding.root_id, result),
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn workspace_diagnostics(&mut self, request: Request) -> Vec<Value> {
        let params: WorkspaceLanguageDocumentParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(cause) => {
                    return self.error_for(&request, -32602, format!("invalid params: {cause}"))
                }
            };
        let (root_binding, root) = match self.validate_language_document(&params) {
            Ok(root) => root,
            Err(error) => return self.error_for(&request, error.code, error.message),
        };
        let scope_key = Self::workspace_scope_key(&params.project_id, &root_binding.root_id);
        match self.language_servers.diagnostics(
            &scope_key,
            &root,
            &params.path,
            &params.content,
            &params.revision,
        ) {
            Ok(result) => {
                let observation =
                    self.diagnostic_store
                        .record_language_server(&scope_key, &params.path, &result);
                let mut value =
                    serde_json::to_value(result).expect("language diagnostic serialization");
                if let Some(object) = value.as_object_mut() {
                    object.insert(
                        "project_id".into(),
                        Value::String(params.project_id.clone()),
                    );
                    object.insert(
                        "root_id".into(),
                        Value::String(root_binding.root_id.clone()),
                    );
                    object.insert("path".into(), Value::String(params.path.clone()));
                    object.insert(
                        "observed_diagnostics".into(),
                        serde_json::to_value(&observation.diagnostics)
                            .expect("observed diagnostic serialization"),
                    );
                    object.insert(
                        "raw_output_ref".into(),
                        Value::String(observation.raw_output_ref),
                    );
                    object.insert(
                        "observed_at_ms".into(),
                        Value::from(observation.observed_at_ms),
                    );
                    if observation.truncated {
                        object.insert("truncated".into(), Value::Bool(true));
                    }
                }
                self.success_for(&request, value)
            }
            Err(error) => self.error_for(&request, error.code, error.message),
        }
    }

    fn workspace_observed_diagnostics(&self, request: Request) -> Vec<Value> {
        let params: ObservedDiagnosticsParams = match serde_json::from_value(request.params.clone())
        {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        if !self.projects.contains_key(&params.project_id) {
            return self.error_for(&request, -32022, "project not found");
        }
        let root_binding =
            match self.workspace_root_binding(&params.project_id, params.root_id.as_deref(), false)
            {
                Ok((binding, _)) => binding,
                Err((code, message)) => return self.error_for(&request, code, message),
            };
        let scope_key = Self::workspace_scope_key(&params.project_id, &root_binding.root_id);
        let result = self.diagnostic_store.list_scoped(
            &params.project_id,
            &scope_key,
            params.path.as_deref(),
            params.include_stale,
        );
        let mut value = serde_json::to_value(result).expect("observed diagnostics serialization");
        if let Some(object) = value.as_object_mut() {
            object.insert("project_id".into(), Value::String(params.project_id));
            object.insert("root_id".into(), Value::String(root_binding.root_id));
        }
        self.success_for(&request, value)
    }

    fn workspace_diagnostic_raw(&self, request: Request) -> Vec<Value> {
        let params: RawDiagnosticParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        let root_binding =
            match self.workspace_root_binding(&params.project_id, params.root_id.as_deref(), false)
            {
                Ok((binding, _)) => binding,
                Err((code, message)) => return self.error_for(&request, code, message),
            };
        let scope_key = Self::workspace_scope_key(&params.project_id, &root_binding.root_id);
        if !params.reference.starts_with("diagnostic-raw:sha256:") || params.reference.len() > 128 {
            return self.error_for(&request, -32077, "invalid diagnostic raw reference");
        }
        let artifact = self
            .diagnostic_store
            .raw(&scope_key, &params.reference)
            .or_else(|| {
                (root_binding.root_id == "root-1")
                    .then(|| {
                        self.diagnostic_store
                            .raw(&params.project_id, &params.reference)
                    })
                    .flatten()
            });
        let Some(artifact) = artifact else {
            return self.error_for(&request, -32077, "diagnostic raw reference not found");
        };
        let mut value = serde_json::to_value(artifact).expect("diagnostic artifact serialization");
        if let Some(object) = value.as_object_mut() {
            object.insert("project_id".into(), Value::String(params.project_id));
            object.insert("root_id".into(), Value::String(root_binding.root_id));
        }
        self.success_for(&request, value)
    }

    fn workspace_edit_preview(&mut self, request: Request) -> Vec<Value> {
        let params: WorkspaceEditPreviewParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(cause) => {
                    return self.error_for(&request, -32602, format!("invalid params: {cause}"))
                }
            };
        let (project_id, root) = match self.workspace_edit_session_binding(&params.session_id) {
            Ok(binding) => binding,
            Err((code, message)) => return self.error_for(&request, code, message),
        };
        let declared_project = params.edit.get("project_id").and_then(Value::as_str);
        let declared_root = params
            .edit
            .pointer("/root/canonical_path")
            .and_then(Value::as_str);
        if declared_project != Some(project_id.as_str()) || declared_root != root.to_str() {
            return self.error_for(
                &request,
                -32083,
                "workspace edit is not bound to the active Work project root",
            );
        }
        let edit: WorkspaceEdit = match serde_json::from_value(params.edit) {
            Ok(edit) => edit,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid workspace edit: {cause}"))
            }
        };
        let edit_id = edit.edit_id.clone();
        let ignored = ignored_paths(&root, &workspace_edit_paths(&edit));
        match self.workspace_edit_previews.preview(
            &params.session_id,
            edit,
            params.contents,
            &ignored,
        ) {
            Ok(preview) => {
                let artifacts = match self.workspace_edit_previews.snapshots(
                    &params.session_id,
                    &edit_id,
                    &project_id,
                ) {
                    Ok(artifacts) => artifacts,
                    Err(cause) => {
                        self.workspace_edit_previews
                            .remove(&params.session_id, &edit_id);
                        return self.error_for(&request, -32084, cause.message);
                    }
                };
                if let Err(cause) = self.persist_workspace_edit_artifacts(
                    &params.session_id,
                    &project_id,
                    &edit_id,
                    artifacts,
                ) {
                    self.workspace_edit_previews
                        .remove(&params.session_id, &edit_id);
                    return self.error_for(
                        &request,
                        -32116,
                        format!("cannot persist workspace edit artifacts: {cause}"),
                    );
                }
                self.success_for(
                    &request,
                    serde_json::to_value(preview).expect("workspace edit preview serialization"),
                )
            }
            Err(cause) => self.error_for(&request, -32084, cause.message),
        }
    }

    fn workspace_edit_artifact_read(&mut self, request: Request) -> Vec<Value> {
        let params: WorkspaceEditArtifactParams =
            match serde_json::from_value(request.params.clone()) {
                Ok(params) => params,
                Err(cause) => {
                    return self.error_for(&request, -32602, format!("invalid params: {cause}"))
                }
            };
        let (project_id, _) = match self.workspace_edit_session_binding(&params.session_id) {
            Ok(binding) => binding,
            Err((code, message)) => return self.error_for(&request, code, message),
        };
        if project_id != params.project_id {
            return self.error_for(
                &request,
                -32083,
                "workspace edit artifact is not bound to the active Work project",
            );
        }
        if params.edit_id.is_empty()
            || params.edit_id.len() > 256
            || params.reference.len() > 128
            || params.limit == 0
            || params.limit > 64 * 1024
            || !matches!(
                params.reference.split_once(":sha256:"),
                Some(("workspace-edit-content" | "workspace-edit-diff", digest))
                    if digest.len() == 64
            )
        {
            return self.error_for(&request, -32602, "invalid workspace edit artifact identity");
        }
        match self.workspace_edit_previews.read(
            &params.session_id,
            &params.edit_id,
            &params.project_id,
            &params.reference,
            params.offset,
            params.limit,
        ) {
            Ok(page) => self.success_for(
                &request,
                json!({
                    "session_id": params.session_id,
                    "project_id": params.project_id,
                    "edit_id": params.edit_id,
                    "reference": page.reference,
                    "media_type": page.media_type,
                    "offset": page.offset,
                    "next_offset": page.next_offset,
                    "total_bytes": page.total_bytes,
                    "data_base64": BASE64_STANDARD.encode(page.bytes)
                }),
            ),
            Err(_) => {
                let Some(store) = self.workbench_store.as_mut() else {
                    return self.error_for(&request, -32085, "workspace edit artifact not found");
                };
                let durable =
                    match store.read_durable_blob_for_session(
                        &params.session_id,
                        &params.reference,
                        now_ms(),
                    ) {
                        Ok(durable) => durable,
                        Err(_) => return self.error_for(
                            &request,
                            -32085,
                            "workspace edit artifact not found or failed integrity verification",
                        ),
                    };
                if durable.reference.kind != DurableBlobKind::WorkspaceEdit
                    || durable.reference.project_id.as_deref() != Some(&params.project_id)
                    || durable.reference.owner_kind != "edit"
                    || durable.reference.owner_id != params.edit_id
                {
                    return self.error_for(
                        &request,
                        -32085,
                        "workspace edit artifact ownership is invalid",
                    );
                }
                if params.offset > durable.content.len() {
                    return self.error_for(
                        &request,
                        -32085,
                        "workspace edit artifact offset is out of range",
                    );
                }
                let end = params
                    .offset
                    .saturating_add(params.limit)
                    .min(durable.content.len());
                self.success_for(
                    &request,
                    json!({
                        "session_id": params.session_id,
                        "project_id": params.project_id,
                        "edit_id": params.edit_id,
                        "reference": durable.reference.content_reference,
                        "media_type": durable.reference.media_type,
                        "offset": params.offset,
                        "next_offset": (end < durable.content.len()).then_some(end),
                        "total_bytes": durable.content.len(),
                        "data_base64": BASE64_STANDARD.encode(&durable.content[params.offset..end])
                    }),
                )
            }
        }
    }

    fn command_artifact_read(&mut self, request: Request) -> Vec<Value> {
        let params: CommandArtifactParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        if self
            .workbench_store
            .as_ref()
            .and_then(|store| store.session_deletion_for_session(&params.session_id).ok())
            .flatten()
            .is_some_and(|receipt| receipt.state == "purged")
        {
            return self.error_for(&request, -32023, "session not found");
        }
        let session_exists = self.sessions.contains_key(&params.session_id)
            || self
                .workbench_store
                .as_ref()
                .is_some_and(|store| store.load_session(&params.session_id).is_ok());
        if !session_exists {
            return self.error_for(&request, -32023, "session not found");
        }
        if !params.reference.starts_with("command-output:sha256:")
            || params.reference.len() != "command-output:sha256:".len() + 64
        {
            return self.error_for(&request, -32079, "invalid command output reference");
        }
        let artifact = if let Some(artifact) = self
            .command_artifacts
            .read(&params.session_id, &params.reference)
        {
            artifact
        } else {
            let Some(store) = self.workbench_store.as_mut() else {
                return self.error_for(&request, -32079, "command output artifact not found");
            };
            let durable = match store.read_durable_blob_for_session(
                &params.session_id,
                &params.reference,
                now_ms(),
            ) {
                Ok(durable) => durable,
                Err(_) => {
                    return self.error_for(
                        &request,
                        -32079,
                        "command output artifact not found or failed integrity verification",
                    )
                }
            };
            if durable.reference.kind != DurableBlobKind::CommandOutput {
                return self.error_for(&request, -32079, "command output artifact kind is invalid");
            }
            let metadata = &durable.reference.metadata;
            let Some(item_id) = metadata.get("item_id").and_then(Value::as_str) else {
                return self.error_for(
                    &request,
                    -32079,
                    "command output artifact metadata is invalid",
                );
            };
            let numeric = |key: &str| metadata.get(key).and_then(Value::as_u64);
            let Some(source_bytes) = numeric("source_bytes") else {
                return self.error_for(
                    &request,
                    -32079,
                    "command output artifact metadata is invalid",
                );
            };
            let Some(redacted_count) = numeric("redacted_count") else {
                return self.error_for(
                    &request,
                    -32079,
                    "command output artifact metadata is invalid",
                );
            };
            let Some(total_bytes) = numeric("total_bytes") else {
                return self.error_for(
                    &request,
                    -32079,
                    "command output artifact metadata is invalid",
                );
            };
            let Some(retained_bytes) =
                numeric("retained_bytes").and_then(|value| usize::try_from(value).ok())
            else {
                return self.error_for(
                    &request,
                    -32079,
                    "command output artifact metadata is invalid",
                );
            };
            let Some(omitted_bytes) = numeric("omitted_bytes") else {
                return self.error_for(
                    &request,
                    -32079,
                    "command output artifact metadata is invalid",
                );
            };
            let Some(redacted) = metadata.get("redacted").and_then(Value::as_bool) else {
                return self.error_for(
                    &request,
                    -32079,
                    "command output artifact metadata is invalid",
                );
            };
            let Some(truncated) = metadata.get("truncated").and_then(Value::as_bool) else {
                return self.error_for(
                    &request,
                    -32079,
                    "command output artifact metadata is invalid",
                );
            };
            let content = match String::from_utf8(durable.content) {
                Ok(content) => content,
                Err(_) => {
                    return self.error_for(
                        &request,
                        -32079,
                        "command output artifact encoding is invalid",
                    )
                }
            };
            command_artifact::CommandOutputArtifact {
                reference: durable.reference.content_reference,
                sha256: durable.reference.content_hash.sha256,
                content_type: durable.reference.media_type,
                item_id: item_id.into(),
                created_at_ms: durable.reference.created_at_ms,
                source_bytes,
                redacted_count,
                redacted,
                total_bytes,
                retained_bytes,
                omitted_bytes,
                truncated,
                content,
            }
        };
        self.success_for(
            &request,
            serde_json::to_value(artifact).expect("command output artifact serialization"),
        )
    }

    fn record_command_diagnostics(
        &mut self,
        session_id: &str,
        command: &CommandItem,
        artifact: Option<&command_artifact::CommandOutputArtifact>,
    ) -> Option<(
        String,
        String,
        diagnostic_store::DiagnosticObservation,
        command_artifact::CommandOutputArtifact,
    )> {
        let state = self.sessions.get(session_id)?;
        if state.session.mode != SessionMode::Work {
            return None;
        }
        let project_id = state.session.project_id.clone()?;
        let root = PathBuf::from(&self.projects.get(&project_id)?.root);
        let capture = command.output.artifact();
        let parsed = command_diagnostics::parse(&command.command, &capture.content)?;
        if parsed.diagnostics.is_empty() {
            return None;
        }
        let mut diagnostics = Vec::with_capacity(parsed.diagnostics.len());
        for diagnostic in parsed.diagnostics {
            let Some(file) = command_diagnostic_file(&root, &command.cwd, &diagnostic.path) else {
                continue;
            };
            if diagnostic.line > file.content.lines().count().max(1).saturating_add(1) {
                continue;
            }
            diagnostics.push(diagnostic_store::CommandDiagnosticInput {
                path: file.path,
                line: diagnostic.line,
                column: diagnostic.column,
                end_line: diagnostic.end_line,
                end_column: diagnostic.end_column,
                severity: diagnostic.severity,
                code: diagnostic.code,
                message: diagnostic.message,
                file_hash: file.revision,
            });
        }
        if diagnostics.is_empty() {
            return None;
        }
        let output = command.output.snapshot();
        let sanitized_command = output_redaction::redact_complete(&command.command);
        let source_artifact = artifact.cloned().unwrap_or_else(|| {
            self.command_artifacts.record_diagnostic_source(
                session_id,
                &command.item_id,
                &command.output,
                command.redactor.source_bytes(),
                command.redactor.redacted_count(),
            )
        });
        let observation = self.diagnostic_store.record_command(
            &project_id,
            &command.item_id,
            &parsed.toolchain,
            &sanitized_command,
            &output,
            Some(&source_artifact.reference),
            command.redactor.redacted_count(),
            &diagnostics,
            parsed.truncated,
        );
        Some((project_id, parsed.toolchain, observation, source_artifact))
    }

    fn language_result<T: serde::Serialize>(
        &self,
        request: &Request,
        params: &WorkspaceLanguageDocumentParams,
        root_id: &str,
        result: T,
    ) -> Vec<Value> {
        let mut value = serde_json::to_value(result).expect("language result serialization");
        if let Some(object) = value.as_object_mut() {
            object.insert(
                "project_id".into(),
                Value::String(params.project_id.clone()),
            );
            object.insert("path".into(), Value::String(params.path.clone()));
            object.insert("root_id".into(), Value::String(root_id.to_owned()));
        }
        self.success_for(request, value)
    }

    fn validate_language_path(
        &self,
        project_id: &str,
        root_id: Option<&str>,
        path: &str,
    ) -> Result<(workbench_store::StoredProjectRoot, PathBuf), workspace::WorkspaceError> {
        let (root_binding, root) = self
            .workspace_root_binding(project_id, root_id, false)
            .map_err(|(code, message)| workspace::WorkspaceError { code, message })?;
        if ignored_paths(&root, &[path.to_owned()]).contains(path) {
            return Err(workspace::WorkspaceError {
                code: -32035,
                message: "workspace file is ignored by project policy".into(),
            });
        }
        read_text_file(&root, path)?;
        Ok((root_binding, root))
    }

    fn validate_language_document(
        &self,
        params: &WorkspaceLanguageDocumentParams,
    ) -> Result<(workbench_store::StoredProjectRoot, PathBuf), workspace::WorkspaceError> {
        let (root_binding, root) = self.validate_language_path(
            &params.project_id,
            params.root_id.as_deref(),
            &params.path,
        )?;
        let file = read_text_file(&root, &params.path)?;
        if file.revision != params.revision {
            return Err(workspace::WorkspaceError {
                code: -32042,
                message: "workspace file changed since the language snapshot was created".into(),
            });
        }
        Ok((root_binding, root))
    }

    fn refresh_workspace_index(
        &mut self,
        project_id: &str,
        root_id: Option<&str>,
    ) -> Result<repository_index::WorkspaceIndexResult, workspace::WorkspaceError> {
        let (root_binding, root) = self
            .workspace_root_binding(project_id, root_id, false)
            .map_err(|(code, message)| workspace::WorkspaceError { code, message })?;
        let scope_key = Self::workspace_scope_key(project_id, &root_binding.root_id);
        let (candidates, candidate_truncated) = collect_search_candidates(&root)?;
        let paths = candidates
            .iter()
            .map(|candidate| candidate.path.clone())
            .collect::<Vec<_>>();
        let ignored = ignored_paths(&root, &paths);
        self.workspace_indexes
            .entry(scope_key)
            .or_default()
            .refresh(&root, &candidates, &ignored, candidate_truncated)
    }

    fn workspace_watch(&mut self, request: Request) -> Vec<Value> {
        let params: WorkspaceWatchParams = match serde_json::from_value(request.params.clone()) {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        if params.paths.len() > 128 {
            return self.error_for(&request, -32037, "workspace watch path limit exceeded");
        }
        let (root_binding, root) =
            match self.workspace_root_binding(&params.project_id, params.root_id.as_deref(), false)
            {
                Ok(binding) => binding,
                Err((code, message)) => return self.error_for(&request, code, message),
            };
        let requested_paths = if params.paths.is_empty() {
            vec![String::new()]
        } else {
            params.paths
        };
        let mut paths = Vec::new();
        let mut snapshots = HashMap::new();
        for path in requested_paths {
            let (normalized, snapshot) = match capture_watch_snapshot(&root, &path) {
                Ok(snapshot) => snapshot,
                Err(error) => return self.error_for(&request, error.code, error.message),
            };
            if snapshots.insert(normalized.clone(), snapshot).is_none() {
                paths.push(normalized);
            }
        }
        let watch_id = match params.watch_id {
            Some(watch_id) => {
                let Some(existing) = self.workspace_watches.get(&watch_id) else {
                    return self.error_for(&request, -32038, "workspace watch not found");
                };
                if existing.project_id != params.project_id
                    || existing.root_id != root_binding.root_id
                {
                    return self.error_for(&request, -32038, "workspace watch project mismatch");
                }
                watch_id
            }
            None => self.allocate_id("watch"),
        };
        self.workspace_watches.insert(
            watch_id.clone(),
            WorkspaceWatch {
                project_id: params.project_id,
                root_id: root_binding.root_id.clone(),
                paths: paths.clone(),
                snapshots,
            },
        );
        self.success_for(
            &request,
            json!({
                "watch_id": watch_id,
                "project_id": root_binding.project_id,
                "root_id": root_binding.root_id,
                "root_access": root_binding.access,
                "paths": paths,
                "poll_interval_ms": 1500
            }),
        )
    }

    fn workspace_watch_poll(&mut self, request: Request) -> Vec<Value> {
        let params: WorkspaceWatchPollParams = match serde_json::from_value(request.params.clone())
        {
            Ok(params) => params,
            Err(cause) => {
                return self.error_for(&request, -32602, format!("invalid params: {cause}"))
            }
        };
        let Some(watch) = self.workspace_watches.get(&params.watch_id).cloned() else {
            return self.error_for(&request, -32038, "workspace watch not found");
        };
        let (root_binding, root) =
            match self.workspace_root_binding(&watch.project_id, Some(&watch.root_id), false) {
                Ok(binding) => binding,
                Err((code, message)) => return self.error_for(&request, code, message),
            };
        let mut changes = Vec::new();
        let mut snapshots = HashMap::new();
        for path in &watch.paths {
            let (_, current) = match capture_watch_snapshot(&root, path) {
                Ok(snapshot) => snapshot,
                Err(error) if matches!(error.code, -32030 | -32031) => {
                    changes.push(json!({
                        "parent": path,
                        "path": path,
                        "kind": "unavailable"
                    }));
                    snapshots.insert(path.clone(), BTreeMap::new());
                    continue;
                }
                Err(error) => return self.error_for(&request, error.code, error.message),
            };
            let previous = watch.snapshots.get(path).cloned().unwrap_or_default();
            for (entry_path, revision) in &current {
                match previous.get(entry_path) {
                    None => changes.push(json!({
                        "parent": path,
                        "path": entry_path,
                        "kind": "created",
                        "revision": revision
                    })),
                    Some(previous_revision) if previous_revision != revision => {
                        changes.push(json!({
                            "parent": path,
                            "path": entry_path,
                            "kind": "modified",
                            "revision": revision
                        }));
                    }
                    _ => {}
                }
            }
            for entry_path in previous.keys() {
                if !current.contains_key(entry_path) {
                    changes.push(json!({
                        "parent": path,
                        "path": entry_path,
                        "kind": "deleted"
                    }));
                }
            }
            snapshots.insert(path.clone(), current);
        }
        if let Some(active) = self.workspace_watches.get_mut(&params.watch_id) {
            active.snapshots = snapshots;
        }
        if !changes.is_empty() {
            let scope_key = Self::workspace_scope_key(&watch.project_id, &watch.root_id);
            if let Some(index) = self.workspace_indexes.get_mut(&scope_key) {
                index.mark_stale();
            }
            for change in &changes {
                if let Some(path) = change.get("path").and_then(Value::as_str) {
                    let scope_key = Self::workspace_scope_key(&watch.project_id, &watch.root_id);
                    self.language_servers.invalidate_document(&scope_key, path);
                    self.diagnostic_store.invalidate_path(&scope_key, path);
                }
            }
        }
        self.success_for(
            &request,
            json!({
                "watch_id": params.watch_id,
                "project_id": root_binding.project_id,
                "root_id": root_binding.root_id,
                "root_access": root_binding.access,
                "changes": changes,
                "checked_at_ms": now_ms()
            }),
        )
    }

    fn shutdown(&mut self, request: Request) -> Vec<Value> {
        self.language_servers.shutdown_all();
        let mut terminals = self
            .control
            .terminals
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        terminals.manager.shutdown_all();
        terminals.records.clear();
        self.command_artifacts.clear();
        self.workspace_edit_previews.clear();
        self.control.set_protocol_ready(false);
        self.shutdown = true;
        self.success_for(&request, Value::Null)
    }

    fn event(
        &mut self,
        session_id: &str,
        turn_id: Option<&str>,
        event: &str,
        item: Option<TimelineItem>,
    ) -> Value {
        self.sequence += 1;
        serde_json::to_value(Notification {
            jsonrpc: JSONRPC_VERSION,
            method: "event",
            params: EventEnvelope {
                sequence: self.sequence,
                timestamp_ms: now_ms(),
                session_id: session_id.to_owned(),
                turn_id: turn_id.map(str::to_owned),
                event: event.to_owned(),
                item,
            },
        })
        .expect("event serialization")
    }

    fn allocate_id(&mut self, prefix: &str) -> String {
        self.next_id += 1;
        format!("{prefix}-{}", self.next_id)
    }

    fn emit_all<F>(&self, messages: Vec<Value>, emit: &mut F)
    where
        F: FnMut(Value),
    {
        for message in messages {
            emit(message);
        }
    }

    fn success_for(&self, request: &Request, result: Value) -> Vec<Value> {
        match &request.id {
            Some(id) => vec![serde_json::to_value(Response::success(id.clone(), result))
                .expect("response serialization")],
            None => Vec::new(),
        }
    }

    fn error_for(&self, request: &Request, code: i64, message: impl Into<String>) -> Vec<Value> {
        vec![serde_json::to_value(Response::error(
            request.id.clone().unwrap_or(Value::Null),
            code,
            message,
        ))
        .expect("response serialization")]
    }
}

fn command_diagnostic_file(
    root: &Path,
    command_cwd: &str,
    reported_path: &str,
) -> Option<workspace::TextFile> {
    let root = root.canonicalize().ok()?;
    let reported = Path::new(reported_path.trim_matches(['"', '\'']));
    let relative = if reported.is_absolute() {
        reported.strip_prefix(&root).ok()?.to_path_buf()
    } else {
        let cwd = Path::new(command_cwd);
        let cwd_relative = if cwd.is_absolute() {
            cwd.strip_prefix(&root).ok()?.to_path_buf()
        } else {
            PathBuf::new()
        };
        cwd_relative.join(reported)
    };
    let mut normalized = PathBuf::new();
    for component in relative.components() {
        match component {
            Component::Normal(value) => normalized.push(value),
            Component::CurDir => {}
            Component::ParentDir | Component::RootDir | Component::Prefix(_) => return None,
        }
    }
    let relative = normalized.to_string_lossy().replace('\\', "/");
    if relative.is_empty()
        || ignored_paths(&root, std::slice::from_ref(&relative)).contains(&relative)
    {
        return None;
    }
    read_text_file(&root, &relative).ok()
}

fn workspace_edit_paths(edit: &WorkspaceEdit) -> Vec<String> {
    let mut paths = Vec::with_capacity(edit.operations.len().saturating_mul(2));
    for operation in &edit.operations {
        match operation {
            WorkspaceEditOperation::Create { path, .. }
            | WorkspaceEditOperation::Update { path, .. }
            | WorkspaceEditOperation::Delete { path, .. } => paths.push(path.clone()),
            WorkspaceEditOperation::Rename {
                from_path, to_path, ..
            } => {
                paths.push(from_path.clone());
                paths.push(to_path.clone());
            }
        }
    }
    paths
}

fn command_timeline_item(
    command: &CommandItem,
    lifecycle: &str,
    environment: &session_environment::EnvironmentSummary,
    environment_binding: &str,
    artifact: Option<&command_artifact::CommandOutputArtifact>,
    session_id: &str,
) -> TimelineItem {
    let output = command.output.snapshot();
    let risk = command_action::classify(&command.command, &command.command_actions);
    let duration = command
        .duration_ms
        .map(|value| format!(" · {value} ms"))
        .unwrap_or_default();
    let exit = command
        .exit_code
        .map(|value| format!(" · exit {value}"))
        .unwrap_or_default();
    let redaction = if command.redactor.redacted_count() > 0 {
        format!(
            "\n[Aegisy redacted {} secret value(s)]\n",
            command.redactor.redacted_count()
        )
    } else {
        String::new()
    };
    let omitted = if output.omitted_bytes > 0 {
        format!(
            "\n[Aegisy omitted {} earlier output bytes]\n",
            output.omitted_bytes
        )
    } else {
        String::new()
    };
    let output_display = command_output_display(&output, 32 * 1024);
    let environment_contract = if environment_binding == "codex-adapter-launch-contract" {
        Some(codex_child_environment_contract())
    } else {
        None
    };
    let content = format!(
        "$ {}\ncwd: {}\n{} · risk {}{}{}\n{}{}{}",
        command.command,
        command.cwd,
        command.status,
        risk.level,
        duration,
        exit,
        omitted,
        redaction,
        output_display
    );
    TimelineItem {
        id: command.item_id.clone(),
        kind: "command".into(),
        role: "tool".into(),
        state: if lifecycle == "completed" {
            "completed"
        } else if lifecycle == "started" {
            "started"
        } else {
            "delta"
        }
        .into(),
        content,
        data: Some(json!({
            "command": command.command,
            "command_actions": command.command_actions,
            "cwd": command.cwd,
            "environment": {
                "environment_id": environment.environment_id,
                "authority": if environment_binding == "codex-adapter-launch-contract" {
                    "codex-adapter-process-snapshot"
                } else {
                    "aegisy-session-snapshot"
                },
                "execution_binding": environment_binding,
                "values_exposed": false,
                "contract": environment_contract
            },
            "risk": risk,
            "status": command.status,
            "duration_ms": command.duration_ms,
            "exit_code": command.exit_code,
            "process_id": command.process_id,
            "source": command.source,
            "session_id": session_id,
            "output": {
                "head": output.head,
                "tail": output.tail,
                "total_bytes": output.total_bytes,
                "source_bytes": command.redactor.source_bytes(),
                "retained_bytes": output.retained_bytes,
                "omitted_bytes": output.omitted_bytes,
                "truncated": output.truncated,
                "redacted_count": command.redactor.redacted_count(),
                "redacted": command.redactor.redacted_count() > 0,
                "head_limit": output.head_limit,
                "tail_limit": output.tail_limit,
                "artifact": artifact.map(|artifact| json!({
                    "reference": artifact.reference,
                    "content_type": artifact.content_type,
                    "source_bytes": artifact.source_bytes,
                    "total_bytes": artifact.total_bytes,
                    "retained_bytes": artifact.retained_bytes,
                    "omitted_bytes": artifact.omitted_bytes,
                    "truncated": artifact.truncated,
                    "redacted_count": artifact.redacted_count,
                    "redacted": artifact.redacted
                }))
            }
        })),
    }
}

fn codex_child_environment_contract() -> Value {
    const DESCRIPTOR: &str = concat!(
        "codex-child-environment/0.1\0",
        "env_clear\0allowlisted-platform-inheritance\0",
        "secret-redaction\0loader-injection-denied\0",
        "execution-control-denied"
    );
    let contract_hash = ContentHash::for_bytes(DESCRIPTOR.as_bytes());
    json!({
        "schema_version": "codex-child-environment/0.1",
        "contract_id": format!("codex-child-environment:sha256:{}", contract_hash.sha256),
        "launch": {
            "env_clear": true,
            "inheritance": "allowlisted-platform-system",
            "session_identity": "codex-adapter",
            "tool_identity": "codex-app-server"
        },
        "denied": {
            "credential_names": true,
            "authenticated_proxies": true,
            "loader_injection": true,
            "execution_control": true
        },
        "child_observation": "vendor-command-item-does-not-report-child-environment",
        "values_exposed": false
    })
}

fn command_output_display(
    output: &command_output::CommandOutputSnapshot,
    byte_limit: usize,
) -> String {
    let combined = if output.tail.is_empty() {
        output.head.clone()
    } else {
        format!("{}\n...\n{}", output.head, output.tail)
    };
    if combined.len() <= byte_limit {
        return combined;
    }
    let mut start = combined.len() - byte_limit;
    while start < combined.len() && !combined.is_char_boundary(start) {
        start += 1;
    }
    combined[start..].to_owned()
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

fn session_deletion_error_code(code: &str) -> i64 {
    match code {
        "session-deletion-plan-stale" => -32130,
        "session-deletion-pending"
        | "session-deletion-already-pending"
        | "session-deletion-blocked"
        | "session-deletion-not-undoable"
        | "session-deletion-undo-expired"
        | "session-deletion-undo-stale"
        | "session-deleted"
        | "session-already-deleted" => -32131,
        "session-read-only-recovery" => -32115,
        _ => -32114,
    }
}

fn portable_session_error_code(code: &str) -> i64 {
    match code {
        "portable-session-version-unsupported" => -32140,
        "portable-session-integrity-failed"
        | "portable-session-sequence-invalid"
        | "portable-session-item-duplicate"
        | "portable-session-too-large"
        | "portable-session-export-too-large" => -32141,
        "portable-session-redaction-required" => -32142,
        "portable-import-session-collision" | "portable-import-source-collision" => -32143,
        "portable-session-export-stale" | "portable-session-import-stale" => -32144,
        "portable-session-import-blocked" => -32145,
        "session-read-only-recovery" | "portable-session-export-unverified" => -32115,
        _ => -32114,
    }
}

fn refresh_watch_after_user_save(
    watches: &mut HashMap<String, WorkspaceWatch>,
    project_id: &str,
    root_id: &str,
    root: &Path,
    relative: &str,
) {
    let parent = Path::new(relative)
        .parent()
        .map(|path| path.to_string_lossy().replace('\\', "/"))
        .filter(|path| path != ".")
        .unwrap_or_default();
    for watch in watches.values_mut() {
        if watch.project_id != project_id
            || watch.root_id != root_id
            || !watch.paths.contains(&parent)
        {
            continue;
        }
        if let Ok((normalized, snapshot)) = capture_watch_snapshot(root, &parent) {
            watch.snapshots.insert(normalized, snapshot);
        }
    }
}

fn capture_watch_snapshot(
    root: &Path,
    relative: &str,
) -> Result<(String, BTreeMap<String, String>), workspace::WorkspaceError> {
    let mut listing = list_directory(root, relative)?;
    if !listing.path.is_empty()
        && ignored_paths(root, std::slice::from_ref(&listing.path)).contains(&listing.path)
    {
        return Err(workspace::WorkspaceError {
            code: -32035,
            message: "workspace path is ignored by project policy".into(),
        });
    }
    let candidates: Vec<String> = listing
        .entries
        .iter()
        .map(|entry| entry.path.clone())
        .collect();
    let ignored = ignored_paths(root, &candidates);
    listing
        .entries
        .retain(|entry| !ignored.contains(&entry.path));
    let snapshot = listing
        .entries
        .iter()
        .map(|entry| (entry.path.clone(), entry.revision.clone()))
        .collect();
    Ok((listing.path, snapshot))
}

#[cfg(test)]
mod turn_cancel_tests {
    use super::*;

    #[test]
    fn runtime_error_classification_is_bounded_redacted_and_retry_aware() {
        let secret = "ghp_123456789012345678901234567890";
        let message = format!("network transport failed with token {secret}");
        assert_eq!(runtime_error_data(&message)["class"], "transport");
        assert_eq!(runtime_error_data(&message)["retryable"], true);
        assert!(!runtime_error_content(&message).contains(secret));
        assert!(runtime_error_content(&message).len() <= 8 * 1024);
        assert_eq!(
            runtime_error_data("provider rejected model")["class"],
            "provider"
        );
        assert_eq!(
            runtime_error_data("sqlite persistence failed")["class"],
            "storage"
        );
        for (message, class) in [
            ("invalid AAP protocol frame", "protocol"),
            ("sandbox denied filesystem access", "sandbox"),
            ("managed policy denied command", "policy"),
            ("tool command execution failed", "tool"),
            ("workspace file revision is stale", "workspace"),
            ("Git branch operation failed", "git"),
            ("context window token limit exceeded", "budget"),
            ("adapter state is unavailable", "adapter"),
        ] {
            assert_eq!(runtime_error_data(message)["class"], class, "{message}");
            assert_eq!(runtime_error_data(message)["retryable"], false, "{message}");
        }
        assert_eq!(
            runtime_error_data("Codex App Server closed its output channel")["class"],
            "transport"
        );
        assert_eq!(
            runtime_error_data("cannot read Codex App Server output")["retryable"],
            true
        );
    }

    fn ready_runtime() -> Runtime {
        let mut runtime = Runtime::default();
        let initialized = runtime.handle_line(
            &json!({
                "jsonrpc": "2.0",
                "id": "initialize-cancel",
                "method": "initialize",
                "params": {
                    "protocol_version": "0.1",
                    "client": { "name": "test", "version": "1" }
                }
            })
            .to_string(),
        );
        assert!(initialized[0].get("result").is_some());
        assert!(runtime
            .handle_line(r#"{"jsonrpc":"2.0","method":"initialized"}"#)
            .is_empty());
        runtime
    }

    fn cancel_request(id: &str, session_id: &str, turn_id: &str) -> String {
        json!({
            "jsonrpc": "2.0",
            "id": id,
            "method": "turn/cancel",
            "params": { "session_id": session_id, "turn_id": turn_id }
        })
        .to_string()
    }

    fn steer_request(id: &str, session_id: &str, turn_id: &str, input: &str) -> String {
        json!({
            "jsonrpc": "2.0",
            "id": id,
            "method": "turn/steer",
            "params": {
                "session_id": session_id,
                "turn_id": turn_id,
                "input": input,
                "client_user_message_id": format!("message-{id}")
            }
        })
        .to_string()
    }

    #[test]
    fn out_of_band_turn_cancel_is_identity_scoped_idempotent_and_immediate() {
        let runtime = ready_runtime();
        let control = runtime.control();
        let cancellation = control.begin_turn("session-1");
        control.identify_turn("session-1", "turn-1");

        let accepted = control
            .handle_out_of_band_line(&cancel_request("cancel-1", "session-1", "turn-1"))
            .unwrap();
        assert_eq!(accepted[0]["result"]["state"], "cancellation-requested");
        assert_eq!(accepted[0]["result"]["already_requested"], false);
        assert!(cancellation.is_requested());

        let repeated = control
            .handle_out_of_band_line(&cancel_request("cancel-2", "session-1", "turn-1"))
            .unwrap();
        assert_eq!(repeated[0]["result"]["already_requested"], true);
        let wrong_turn = control
            .handle_out_of_band_line(&cancel_request("cancel-3", "session-1", "turn-old"))
            .unwrap();
        assert_eq!(wrong_turn[0]["error"]["code"], -32081);
        let duplicate = control
            .handle_out_of_band_line(&cancel_request("cancel-3", "session-1", "turn-1"))
            .unwrap();
        assert_eq!(duplicate[0]["error"]["code"], -32001);

        control.finish_turn("session-1");
        let completed = control
            .handle_out_of_band_line(&cancel_request("cancel-4", "session-1", "turn-1"))
            .unwrap();
        assert_eq!(completed[0]["error"]["code"], -32080);
    }

    #[test]
    fn out_of_band_turn_steer_is_identity_scoped_bounded_and_queued() {
        let runtime = ready_runtime();
        let control = runtime.control();
        control.begin_turn("session-1");
        let steering = control.steering_handle("session-1").unwrap();
        control.identify_turn("session-1", "turn-1");

        let accepted = control
            .handle_out_of_band_line(&steer_request(
                "steer-1",
                "session-1",
                "turn-1",
                "focus on the failing test",
            ))
            .unwrap();
        assert_eq!(accepted[0]["result"]["state"], "steering-requested");
        assert_eq!(accepted[0]["result"]["queued"], 1);

        let wrong_turn = control
            .handle_out_of_band_line(&steer_request(
                "steer-wrong",
                "session-1",
                "turn-old",
                "wrong turn",
            ))
            .unwrap();
        assert_eq!(wrong_turn[0]["error"]["code"], -32081);

        for index in 2..=TURN_STEER_QUEUE_CAPACITY {
            let queued = control
                .handle_out_of_band_line(&steer_request(
                    &format!("steer-{index}"),
                    "session-1",
                    "turn-1",
                    &format!("follow-up {index}"),
                ))
                .unwrap();
            assert_eq!(queued[0]["result"]["queued"], index);
        }
        let full = control
            .handle_out_of_band_line(&steer_request(
                "steer-full",
                "session-1",
                "turn-1",
                "one too many",
            ))
            .unwrap();
        assert_eq!(full[0]["error"]["code"], -32004);

        let pending = steering.drain();
        assert_eq!(pending.len(), TURN_STEER_QUEUE_CAPACITY);
        assert_eq!(pending[0].input, "focus on the failing test");
        assert_eq!(
            pending[0].client_user_message_id.as_deref(),
            Some("message-steer-1")
        );

        control.finish_turn("session-1");
        let completed = control
            .handle_out_of_band_line(&steer_request(
                "steer-completed",
                "session-1",
                "turn-1",
                "too late",
            ))
            .unwrap();
        assert_eq!(completed[0]["error"]["code"], -32080);
    }

    #[test]
    fn non_cancel_requests_remain_on_the_normal_dispatch_path() {
        let runtime = ready_runtime();
        assert!(runtime
            .control()
            .handle_out_of_band_line(
                r#"{"jsonrpc":"2.0","id":"read","method":"session/read","params":{}}"#
            )
            .is_none());
    }
}

#[cfg(test)]
mod command_timeline_tests {
    use super::*;

    #[test]
    fn command_timeline_keeps_structured_authority_and_bounded_display() {
        let mut command = CommandItem {
            item_id: "command-1".into(),
            command: "git status --short".into(),
            command_actions: json!([{
                "type": "listFiles",
                "command": "git status --short"
            }]),
            cwd: "/tmp/project".into(),
            status: "completed".into(),
            output: command_output::CommandOutputCapture::default(),
            redactor: output_redaction::OutputRedactor::default(),
            duration_ms: Some(42),
            exit_code: Some(0),
            process_id: Some("pty-1".into()),
            source: "agent".into(),
        };
        codex_adapter::append_bounded_output(&mut command, &"界".repeat(200_000));
        codex_adapter::finish_bounded_output(&mut command);
        let environment = session_environment::EnvironmentSummary {
            environment_id: "environment:sha256:test".into(),
            variable_count: 4,
            inherited_count: 4,
            explicit_count: 0,
            masked_count: 2,
            path_entry_count: 3,
            explicit_variable_names: Vec::new(),
        };
        let item = command_timeline_item(
            &command,
            "completed",
            &environment,
            "unverified-session-snapshot",
            None,
            "session-test",
        );
        let data = item.data.unwrap();
        assert_eq!(item.kind, "command");
        assert_eq!(data["risk"]["level"], "low");
        assert_eq!(data["duration_ms"], 42);
        assert_eq!(data["exit_code"], 0);
        assert_eq!(
            data["environment"]["execution_binding"],
            "unverified-session-snapshot"
        );
        assert!(item.content.len() < command.output.snapshot().total_bytes as usize);
        assert!(item.content.contains("Aegisy omitted"));
    }

    #[test]
    fn command_artifact_aap_read_is_session_scoped_and_validated() {
        fn state(id: &str) -> SessionState {
            let root = std::env::temp_dir();
            SessionState {
                session: Session {
                    id: id.into(),
                    mode: SessionMode::Chat,
                    project_id: None,
                    title: "test".into(),
                },
                items: Vec::new(),
                backend_session_id: None,
                backend_info: BackendInfo {
                    adapter: "preview".into(),
                    version: "test".into(),
                    provider: None,
                    model: None,
                    permission_profile: "read-only".into(),
                    environment: None,
                },
                environment: SessionEnvironment::build(id, None, "chat", &root),
            }
        }
        fn request(session_id: &str, reference: &str) -> Request {
            Request {
                jsonrpc: JSONRPC_VERSION.into(),
                id: Some(json!("artifact-read")),
                method: "artifact/read-command-output".into(),
                params: json!({
                    "session_id": session_id,
                    "reference": reference
                }),
            }
        }

        let mut runtime = Runtime::default();
        runtime
            .sessions
            .insert("session-1".into(), state("session-1"));
        runtime
            .sessions
            .insert("session-2".into(), state("session-2"));
        let mut output = command_output::CommandOutputCapture::default();
        output.append(&"x".repeat(3 * 1024 * 1024));
        let artifact = runtime
            .command_artifacts
            .record("session-1", "command-1", &output, 3 * 1024 * 1024, 0)
            .unwrap();
        let read = runtime.command_artifact_read(request("session-1", &artifact.reference));
        assert_eq!(read[0]["result"]["reference"], artifact.reference);
        assert_eq!(
            read[0]["result"]["content_type"],
            "text/plain; charset=utf-8"
        );
        let denied = runtime.command_artifact_read(request("session-2", &artifact.reference));
        assert_eq!(denied[0]["error"]["code"], -32079);
        let invalid = runtime.command_artifact_read(request("session-1", "bad-reference"));
        assert_eq!(invalid[0]["error"]["code"], -32079);
        let missing = runtime.command_artifact_read(request("missing", &artifact.reference));
        assert_eq!(missing[0]["error"]["code"], -32023);
    }

    #[test]
    fn command_secrets_never_enter_timeline_artifact_or_aap_response() {
        let first_secret = "top-secret-value-1234567890";
        let second_secret = "ghp_123456789012345678901234567890";
        let mut command = CommandItem {
            item_id: "command-secret".into(),
            command: "print diagnostics".into(),
            command_actions: json!([{"type": "unknown"}]),
            cwd: "/tmp/project".into(),
            status: "completed".into(),
            output: command_output::CommandOutputCapture::default(),
            redactor: output_redaction::OutputRedactor::default(),
            duration_ms: Some(10),
            exit_code: Some(0),
            process_id: None,
            source: "agent".into(),
        };
        codex_adapter::append_bounded_output(&mut command, &format!("API_KEY={first_secret}\n"));
        codex_adapter::append_bounded_output(&mut command, &"x".repeat(3 * 1024 * 1024));
        codex_adapter::append_bounded_output(
            &mut command,
            &format!("\nAuthorization: Bearer {second_secret}"),
        );
        codex_adapter::finish_bounded_output(&mut command);

        let mut runtime = Runtime::default();
        let root = std::env::temp_dir();
        runtime.sessions.insert(
            "session-secret".into(),
            SessionState {
                session: Session {
                    id: "session-secret".into(),
                    mode: SessionMode::Chat,
                    project_id: None,
                    title: "test".into(),
                },
                items: Vec::new(),
                backend_session_id: None,
                backend_info: BackendInfo {
                    adapter: "preview".into(),
                    version: "test".into(),
                    provider: None,
                    model: None,
                    permission_profile: "read-only".into(),
                    environment: None,
                },
                environment: SessionEnvironment::build("session-secret", None, "chat", &root),
            },
        );
        let artifact = runtime
            .command_artifacts
            .record(
                "session-secret",
                &command.item_id,
                &command.output,
                command.redactor.source_bytes(),
                command.redactor.redacted_count(),
            )
            .unwrap();
        let environment = session_environment::EnvironmentSummary {
            environment_id: "environment:sha256:test".into(),
            variable_count: 0,
            inherited_count: 0,
            explicit_count: 0,
            masked_count: 0,
            path_entry_count: 0,
            explicit_variable_names: Vec::new(),
        };
        let timeline = command_timeline_item(
            &command,
            "completed",
            &environment,
            "codex-adapter-launch-contract",
            Some(&artifact),
            "session-secret",
        );
        let response = runtime.command_artifact_read(Request {
            jsonrpc: JSONRPC_VERSION.into(),
            id: Some(json!("artifact-secret-read")),
            method: "artifact/read-command-output".into(),
            params: json!({
                "session_id": "session-secret",
                "reference": artifact.reference
            }),
        });
        let serialized = format!(
            "{}\n{}",
            serde_json::to_string(&timeline).unwrap(),
            serde_json::to_string(&response).unwrap()
        );
        assert!(!serialized.contains(first_secret));
        assert!(!serialized.contains(second_secret));
        assert!(serialized.contains("[REDACTED]"));
        assert_eq!(timeline.data.as_ref().unwrap()["output"]["redacted"], true);
        assert_eq!(response[0]["result"]["redacted_count"], 2);
        assert_eq!(
            response[0]["result"]["source_bytes"],
            command.redactor.source_bytes()
        );
    }

    #[test]
    fn command_diagnostics_are_work_project_scoped_filtered_and_navigable() {
        let root = std::env::temp_dir().join(format!(
            "aegisy-command-diagnostic-{}-{}",
            std::process::id(),
            now_ms()
        ));
        fs::create_dir_all(root.join("src")).unwrap();
        fs::write(root.join("src/main.rs"), "fn main() {\n    missing();\n}\n").unwrap();
        fs::write(root.join(".env"), "PRIVATE=value\n").unwrap();
        fs::write(root.join("ignored.rs"), "fn ignored() {}\n").unwrap();
        fs::write(root.join(".gitignore"), "ignored.rs\n").unwrap();
        let git = std::process::Command::new("git")
            .args(["init", "-q"])
            .arg(&root)
            .status()
            .unwrap();
        assert!(git.success());
        #[cfg(unix)]
        std::os::unix::fs::symlink(root.join("src/main.rs"), root.join("linked.rs")).unwrap();
        let outside = root.with_file_name(format!(
            "{}-outside.rs",
            root.file_name().unwrap().to_string_lossy()
        ));
        fs::write(&outside, "fn outside() {}\n").unwrap();
        let canonical_root = root.canonicalize().unwrap();
        let project_id = "project-command-diagnostic";
        let session_id = "session-command-diagnostic";
        let mut runtime = Runtime::default();
        runtime.projects.insert(
            project_id.into(),
            Project {
                id: project_id.into(),
                root: canonical_root.to_string_lossy().into(),
                name: "fixture".into(),
            },
        );
        runtime.sessions.insert(
            session_id.into(),
            SessionState {
                session: Session {
                    id: session_id.into(),
                    mode: SessionMode::Work,
                    project_id: Some(project_id.into()),
                    title: "fixture".into(),
                },
                items: Vec::new(),
                backend_session_id: None,
                backend_info: BackendInfo {
                    adapter: "fixture".into(),
                    version: "test".into(),
                    provider: None,
                    model: None,
                    permission_profile: "read-only".into(),
                    environment: None,
                },
                environment: SessionEnvironment::build(
                    session_id,
                    Some(project_id),
                    "work",
                    &canonical_root,
                ),
            },
        );
        let secret = "ghp_123456789012345678901234567890";
        let mut command = CommandItem {
            item_id: "command-diagnostic".into(),
            command: format!("cargo check --token {secret}"),
            command_actions: json!([{"type": "unknown"}]),
            cwd: canonical_root.to_string_lossy().into(),
            status: "failed".into(),
            output: command_output::CommandOutputCapture::default(),
            redactor: output_redaction::OutputRedactor::default(),
            duration_ms: Some(1),
            exit_code: Some(101),
            process_id: None,
            source: "agent".into(),
        };
        let mut output = format!(
            "error[E0425]: cannot find function `missing`\n --> src/main.rs:2:5\n\
             error: sensitive path must be filtered\n --> .env:1:1\n\
             error: ignored path must be filtered\n --> ignored.rs:1:1\n\
             error: outside path must be filtered\n --> {}:1:1\n",
            outside.to_string_lossy()
        );
        #[cfg(unix)]
        output.push_str("error: symlink path must be filtered\n --> linked.rs:1:1\n");
        codex_adapter::append_bounded_output(&mut command, &output);
        codex_adapter::finish_bounded_output(&mut command);

        let (recorded_project, toolchain, observation, artifact) = runtime
            .record_command_diagnostics(session_id, &command, None)
            .unwrap();
        assert_eq!(recorded_project, project_id);
        assert_eq!(toolchain, "rustc");
        assert_eq!(observation.diagnostics.len(), 1);
        let diagnostic = &observation.diagnostics[0];
        assert_eq!(diagnostic.path, "src/main.rs");
        assert_eq!(diagnostic.line, 2);
        assert!(diagnostic.file_hash.starts_with("content:"));
        assert_eq!(diagnostic.source_kind, "command");
        assert!(!diagnostic
            .source_command
            .as_deref()
            .unwrap()
            .contains(secret));
        assert!(diagnostic
            .source_command
            .as_deref()
            .unwrap()
            .contains("[REDACTED]"));
        assert_eq!(
            runtime
                .command_artifacts
                .read(session_id, &artifact.reference),
            Some(artifact)
        );
        let raw = runtime
            .diagnostic_store
            .raw(project_id, &observation.raw_output_ref)
            .unwrap();
        assert!(raw.content.contains("src/main.rs"));
        assert!(!raw.content.contains(".env"));
        assert!(!raw.content.contains("ignored.rs"));
        assert!(!raw.content.contains("outside.rs"));
        assert!(!raw.content.contains("linked.rs"));
        assert!(!raw.content.contains(secret));
        fs::remove_dir_all(root).unwrap();
        fs::remove_file(outside).unwrap();
    }
}

#[cfg(test)]
mod durable_runtime_tests {
    use super::*;

    fn request(id: &str, method: &str, params: Value) -> String {
        json!({
            "jsonrpc": "2.0",
            "id": id,
            "method": method,
            "params": params
        })
        .to_string()
    }

    fn ready(runtime: &mut Runtime) {
        let initialize = runtime.handle_line(&request(
            "initialize",
            "initialize",
            json!({
                "protocol_version": "0.1",
                "client": { "name": "durable-test", "version": "1" }
            }),
        ));
        assert!(initialize[0].get("result").is_some());
        assert!(runtime
            .handle_line(r#"{"jsonrpc":"2.0","method":"initialized"}"#)
            .is_empty());
    }

    #[test]
    fn project_session_and_preview_turn_survive_runtime_restart() {
        let root = std::env::temp_dir().join(format!(
            "aegisy-runtime-durable-{}-{}",
            std::process::id(),
            now_ms()
        ));
        let data_root = root.join("data");
        let project_root = root.join("project");
        fs::create_dir_all(&data_root).unwrap();
        fs::create_dir_all(&project_root).unwrap();
        let canonical_project = project_root.canonicalize().unwrap();

        let mut runtime = Runtime::with_store(&data_root).unwrap();
        ready(&mut runtime);
        let opened = runtime.handle_line(&request(
            "project-open",
            "project/open",
            json!({ "root": canonical_project }),
        ));
        assert_eq!(opened[0]["result"]["project"]["id"], "project-1");
        let started = runtime.handle_line(&request(
            "session-start",
            "session/start",
            json!({
                "mode": "work",
                "project_id": "project-1",
                "title": "Durable fixture"
            }),
        ));
        assert_eq!(started[0]["result"]["session"]["id"], "session-2");
        let events = runtime.handle_line(&request(
            "turn-start",
            "turn/start",
            json!({
                "session_id": "session-2",
                "input": "hello durable store",
                "idempotency_key": "turn-request-1"
            }),
        ));
        assert_eq!(events[0]["result"]["turn"]["id"], "turn-4");
        assert!(events
            .iter()
            .any(|message| { message["params"]["event"] == "turn.completed" }));

        let mut restarted = Runtime::with_store(&data_root).unwrap();
        ready(&mut restarted);
        let reopened = restarted.handle_line(&request(
            "project-reopen",
            "project/open",
            json!({ "root": canonical_project }),
        ));
        assert_eq!(reopened[0]["result"]["project"]["id"], "project-1");
        let resumed = restarted.handle_line(&request(
            "session-resume",
            "session/resume",
            json!({ "session_id": "session-2" }),
        ));
        assert_eq!(resumed[0]["result"]["session"]["id"], "session-2");
        assert_eq!(
            restarted
                .sessions
                .get("session-2")
                .expect("resumed session state")
                .items
                .len(),
            2
        );
        let _new_session = restarted.handle_line(&request(
            "session-start-after-resume",
            "session/start",
            json!({ "mode": "work", "project_id": "project-1" }),
        ));
        let listed = restarted.handle_line(&request(
            "session-list",
            "session/list",
            json!({ "project_id": "project-1", "mode": "work" }),
        ));
        assert_eq!(listed[0]["result"]["durable"], true);
        assert_eq!(listed[0]["result"]["sessions"].as_array().unwrap().len(), 2);
        let replay = restarted.handle_line(&request(
            "session-read",
            "session/read",
            json!({ "session_id": "session-2" }),
        ));
        assert_eq!(replay[0]["result"]["session"]["id"], "session-2");
        assert_eq!(replay[0]["result"]["items"].as_array().unwrap().len(), 2);
        assert_eq!(replay[0]["result"]["consistency"]["consistent"], true);
        assert_eq!(
            replay[0]["result"]["consistency"]["rebuild_source_complete"],
            true
        );
        assert_eq!(
            replay[0]["result"]["consistency"]["event_projection_matches"],
            true
        );
        assert_eq!(
            replay[0]["result"]["runtime"]["adapter"],
            "durable-store-replay"
        );
        let latest_page = restarted.handle_line(&request(
            "session-read-latest-page",
            "session/read",
            json!({ "session_id": "session-2", "limit": 1 }),
        ));
        assert_eq!(latest_page[0]["result"]["items"][0]["sequence"], 2);
        assert_eq!(
            latest_page[0]["result"]["history_page"]["older_cursor"],
            "before:2"
        );
        let older_page = restarted.handle_line(&request(
            "session-read-older-page",
            "session/read",
            json!({
                "session_id": "session-2",
                "cursor": "before:2",
                "limit": 1
            }),
        ));
        assert_eq!(older_page[0]["result"]["items"][0]["sequence"], 1);
        assert_eq!(older_page[0]["result"]["history_page"]["has_older"], false);

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn project_identity_survives_move_and_missing_roots_require_explicit_relink() {
        let root = std::env::temp_dir().join(format!(
            "aegisy-project-identity-{}-{}",
            std::process::id(),
            now_ms()
        ));
        let data_root = root.join("data");
        let original = root.join("original");
        let moved = root.join("moved");
        fs::create_dir_all(&data_root).unwrap();
        fs::create_dir_all(&original).unwrap();
        let original = original.canonicalize().unwrap();
        let original_text = original.to_string_lossy().into_owned();

        let mut runtime = Runtime::with_store(&data_root).unwrap();
        ready(&mut runtime);
        let opened = runtime.handle_line(&request(
            "project-open",
            "project/open",
            json!({"root": original}),
        ));
        assert_eq!(opened[0]["result"]["project"]["id"], "project-1");
        assert_eq!(opened[0]["result"]["identity"]["availability"], "available");
        let identity = opened[0]["result"]["identity"]["root_identity"].clone();

        fs::rename(&original, &moved).unwrap();
        let moved = moved.canonicalize().unwrap();
        let moved_open = runtime.handle_line(&request(
            "project-moved",
            "project/open",
            json!({"root": moved.clone()}),
        ));
        assert_eq!(moved_open[0]["result"]["project"]["id"], "project-1");
        assert_eq!(moved_open[0]["result"]["project"]["root"], original_text);
        assert_eq!(
            moved_open[0]["result"]["identity"]["root_identity"],
            identity
        );
        assert_eq!(moved_open[0]["result"]["identity"]["availability"], "moved");
        assert_eq!(moved_open[0]["result"]["identity"]["relink_required"], true);
        assert_eq!(
            moved_open[0]["result"]["identity"]["candidate_root"],
            moved.to_string_lossy().as_ref()
        );

        let missing_open = runtime.handle_line(&request(
            "project-missing",
            "project/open",
            json!({"root": original_text}),
        ));
        assert_eq!(missing_open[0]["result"]["project"]["id"], "project-1");
        assert_eq!(
            missing_open[0]["result"]["identity"]["availability"],
            "unavailable"
        );
        assert_eq!(
            missing_open[0]["result"]["identity"]["relink_required"],
            true
        );

        let relinked = runtime.handle_line(&request(
            "project-relink",
            "project/relink",
            json!({
                "project_id": "project-1",
                "root_id": "root-1",
                "root": moved.clone(),
                "expected_root_identity": identity.as_str().unwrap()
            }),
        ));
        assert_eq!(relinked[0]["result"]["project"]["id"], "project-1");
        assert_eq!(
            relinked[0]["result"]["identity"]["availability"],
            "available"
        );
        assert_eq!(relinked[0]["result"]["identity"]["relink_required"], false);
        assert_eq!(
            relinked[0]["result"]["project"]["root"],
            moved.to_string_lossy().as_ref()
        );
        assert_eq!(
            runtime
                .workbench_store
                .as_ref()
                .unwrap()
                .load_project("project-1")
                .unwrap()
                .canonical_root,
            moved.to_string_lossy().as_ref()
        );
        let mut restarted = Runtime::with_store(&data_root).unwrap();
        ready(&mut restarted);
        let reopened = restarted.handle_line(&request(
            "project-relink-reopen",
            "project/open",
            json!({ "root": moved.clone() }),
        ));
        assert_eq!(reopened[0]["result"]["project"]["id"], "project-1");
        assert_eq!(
            reopened[0]["result"]["identity"]["availability"],
            "available"
        );

        let project_count = runtime
            .workbench_store
            .as_ref()
            .unwrap()
            .list_projects()
            .unwrap();
        assert_eq!(project_count.len(), 1);
        assert_eq!(project_count[0].project_id, "project-1");
        assert_eq!(project_count[0].canonical_root, moved.to_string_lossy());
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn first_durable_session_read_automatically_rebuilds_safe_projection_drift() {
        let root = std::env::temp_dir().join(format!(
            "aegisy-runtime-projection-recovery-{}-{}",
            std::process::id(),
            now_ms()
        ));
        let data_root = root.join("data");
        fs::create_dir_all(&data_root).unwrap();

        let mut runtime = Runtime::with_store(&data_root).unwrap();
        ready(&mut runtime);
        let started = runtime.handle_line(&request(
            "session-start",
            "session/start",
            json!({"mode": "chat", "title": "Automatic recovery fixture"}),
        ));
        let session_id = started[0]["result"]["session"]["id"]
            .as_str()
            .unwrap()
            .to_owned();
        let turn = runtime.handle_line(&request(
            "turn-start",
            "turn/start",
            json!({
                "session_id": &session_id,
                "input": "persist two timeline items",
                "idempotency_key": "automatic-recovery-turn"
            }),
        ));
        assert!(turn
            .iter()
            .any(|message| message["params"]["event"] == "turn.completed"));
        drop(runtime);

        let database = data_root.join("aegisy-workbench.sqlite3");
        let connection = rusqlite::Connection::open(&database).unwrap();
        connection
            .execute(
                "UPDATE sessions SET title = 'Drifted title' WHERE session_id = ?1",
                [&session_id],
            )
            .unwrap();
        connection
            .execute(
                "DELETE FROM items
                 WHERE session_id = ?1 AND item_sequence = 1",
                [&session_id],
            )
            .unwrap();
        drop(connection);

        let mut restarted = Runtime::with_store(&data_root).unwrap();
        ready(&mut restarted);
        let replay = restarted.handle_line(&request(
            "session-read",
            "session/read",
            json!({"session_id": &session_id}),
        ));
        assert_eq!(
            replay[0]["result"]["session"]["title"],
            "Automatic recovery fixture"
        );
        assert_eq!(replay[0]["result"]["items"].as_array().unwrap().len(), 2);
        assert_eq!(replay[0]["result"]["consistency"]["consistent"], true);
        assert_eq!(
            replay[0]["result"]["recovery"]["status"],
            "projection-rebuilt"
        );
        assert_eq!(
            replay[0]["result"]["recovery"]["audit_event"],
            "session.projection-rebuilt"
        );
        drop(restarted);

        let connection = rusqlite::Connection::open_with_flags(
            database,
            rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY | rusqlite::OpenFlags::SQLITE_OPEN_NO_MUTEX,
        )
        .unwrap();
        let audit_count: i64 = connection
            .query_row(
                "SELECT COUNT(*) FROM events
                 WHERE session_id = ?1 AND event_kind = 'session.projection-rebuilt'",
                [&session_id],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(audit_count, 1);

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn startup_quarantine_is_visible_and_blocks_session_aap_mutation() {
        let root = std::env::temp_dir().join(format!(
            "aegisy-runtime-startup-quarantine-{}-{}",
            std::process::id(),
            now_ms()
        ));
        let data_root = root.join("data");
        fs::create_dir_all(&data_root).unwrap();

        let mut runtime = Runtime::with_store(&data_root).unwrap();
        ready(&mut runtime);
        let started = runtime.handle_line(&request(
            "session-start",
            "session/start",
            json!({"mode": "chat", "title": "Quarantine fixture"}),
        ));
        let session_id = started[0]["result"]["session"]["id"]
            .as_str()
            .unwrap()
            .to_owned();
        drop(runtime);

        let database = data_root.join("aegisy-workbench.sqlite3");
        let connection = rusqlite::Connection::open(&database).unwrap();
        connection
            .execute(
                "UPDATE events SET payload_json = '{\"tampered\":true}'
                 WHERE session_id = ?1 AND event_kind = 'session.created'",
                [&session_id],
            )
            .unwrap();
        drop(connection);

        let mut restarted = Runtime::with_store(&data_root).unwrap();
        let initialized = restarted.handle_line(&request(
            "initialize",
            "initialize",
            json!({
                "protocol_version": "0.1",
                "client": {"name": "quarantine-test", "version": "1"}
            }),
        ));
        let capabilities = initialized[0]["result"]["capabilities"].as_array().unwrap();
        assert!(capabilities
            .iter()
            .any(|value| value == "runtime.projection-recovery.status"));
        assert!(restarted
            .handle_line(r#"{"jsonrpc":"2.0","method":"initialized"}"#)
            .is_empty());

        let runtime_status = restarted.handle_line(&request(
            "runtime-recovery",
            "runtime/projection-recovery/status",
            json!({}),
        ));
        assert_eq!(
            runtime_status[0]["result"]["current_quarantined_sessions"],
            1
        );
        assert_eq!(
            runtime_status[0]["result"]["startup"]["quarantined_sessions"],
            1
        );
        let listed = restarted.handle_line(&request(
            "session-list",
            "session/list",
            json!({"include_archived": true}),
        ));
        assert_eq!(
            listed[0]["result"]["sessions"][0]["recovery_required"],
            true
        );
        let status = restarted.handle_line(&request(
            "session-recovery",
            "session/recovery/status",
            json!({"session_id": &session_id}),
        ));
        assert_eq!(status[0]["result"]["recovery_required"], true);
        assert!(status[0]["result"]["issues"]
            .as_array()
            .unwrap()
            .iter()
            .any(|issue| issue == "event-payload-or-sequence-invalid"));
        let denied_title = restarted.handle_line(&request(
            "session-title",
            "session/title",
            json!({"session_id": &session_id, "title": "Must not write"}),
        ));
        assert_eq!(denied_title[0]["error"]["code"], -32115);
        let denied_read = restarted.handle_line(&request(
            "session-read",
            "session/read",
            json!({"session_id": &session_id}),
        ));
        assert_eq!(denied_read[0]["error"]["code"], -32115);

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn command_artifact_survives_runtime_restart_with_session_scope_and_integrity() {
        let root = std::env::temp_dir().join(format!(
            "aegisy-runtime-durable-artifact-{}-{}",
            std::process::id(),
            now_ms()
        ));
        let data_root = root.join("data");
        fs::create_dir_all(&data_root).unwrap();

        let mut runtime = Runtime::with_store(&data_root).unwrap();
        ready(&mut runtime);
        let started = runtime.handle_line(&request(
            "session-start",
            "session/start",
            json!({"mode": "chat", "title": "Artifact fixture"}),
        ));
        let session_id = started[0]["result"]["session"]["id"]
            .as_str()
            .unwrap()
            .to_owned();
        let mut output = command_output::CommandOutputCapture::default();
        output.append("durable command output\n");
        let artifact = runtime.command_artifacts.record_diagnostic_source(
            &session_id,
            "command-restart",
            &output,
            23,
            0,
        );
        let item = TimelineItem {
            id: "command-restart".into(),
            kind: "command".into(),
            role: "tool".into(),
            state: "completed".into(),
            content: "Command completed".into(),
            data: Some(json!({
                "artifact": {"reference": artifact.reference}
            })),
        };
        runtime
            .persist_item_with_command_artifact(&session_id, None, &item, Some(&artifact))
            .unwrap();
        let reference = artifact.reference.clone();
        drop(runtime);

        let mut restarted = Runtime::with_store(&data_root).unwrap();
        ready(&mut restarted);
        let read = restarted.handle_line(&request(
            "artifact-read",
            "artifact/read-command-output",
            json!({"session_id": &session_id, "reference": &reference}),
        ));
        assert_eq!(read[0]["result"]["content"], "durable command output\n");
        assert_eq!(read[0]["result"]["item_id"], "command-restart");
        let denied = restarted.handle_line(&request(
            "artifact-cross-session",
            "artifact/read-command-output",
            json!({"session_id": "other-session", "reference": &reference}),
        ));
        assert_eq!(denied[0]["error"]["code"], -32023);
        let replay = restarted.handle_line(&request(
            "session-read",
            "session/read",
            json!({"session_id": &session_id}),
        ));
        assert_eq!(replay[0]["result"]["consistency"]["consistent"], true);
        assert_eq!(
            replay[0]["result"]["consistency"]["checked_blob_references"],
            1
        );

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn workspace_edit_patch_artifact_survives_runtime_restart_and_pages_by_scope() {
        let root = std::env::temp_dir().join(format!(
            "aegisy-runtime-durable-preview-{}-{}",
            std::process::id(),
            now_ms()
        ));
        let data_root = root.join("data");
        let project_root = root.join("project");
        fs::create_dir_all(&data_root).unwrap();
        fs::create_dir_all(project_root.join("src")).unwrap();
        let before = b"fn before() {}\n";
        fs::write(project_root.join("src/main.rs"), before).unwrap();
        let canonical_project = project_root.canonicalize().unwrap();
        let root_text = canonical_project.to_string_lossy().into_owned();
        let root_identity = format!(
            "workspace-root:sha256:{}",
            ContentHash::for_bytes(root_text.as_bytes()).sha256
        );
        let replacement = "fn after() {}\n";
        let replacement_hash = ContentHash::for_bytes(replacement.as_bytes());
        let replacement_reference =
            format!("workspace-edit-content:sha256:{}", replacement_hash.sha256);

        let mut runtime = Runtime::with_store(&data_root).unwrap();
        ready(&mut runtime);
        let opened = runtime.handle_line(&request(
            "project-open",
            "project/open",
            json!({"root": canonical_project}),
        ));
        let project_id = opened[0]["result"]["project"]["id"]
            .as_str()
            .unwrap()
            .to_owned();
        let started = runtime.handle_line(&request(
            "session-start",
            "session/start",
            json!({"mode": "work", "project_id": &project_id}),
        ));
        let session_id = started[0]["result"]["session"]["id"]
            .as_str()
            .unwrap()
            .to_owned();
        let edit = json!({
            "schema_version": "workspace-edit/0.2",
            "edit_id": "durable-edit",
            "project_id": &project_id,
            "root": {"canonical_path": root_text, "identity": root_identity},
            "operations": [{
                "kind": "update",
                "path": "src/main.rs",
                "base": ContentHash::for_bytes(before),
                "content": {
                    "reference": &replacement_reference,
                    "hash": replacement_hash,
                    "format": {
                        "encoding": "utf-8",
                        "newline": "lf",
                        "mode": "preserve"
                    }
                }
            }]
        });
        let preview = runtime.handle_line(&request(
            "preview",
            "workspace/edit/preview",
            json!({
                "session_id": &session_id,
                "edit": edit,
                "contents": [{
                    "reference": &replacement_reference,
                    "content": replacement
                }]
            }),
        ));
        assert_eq!(preview[0]["result"]["applicable"], true);
        let aggregate_reference = preview[0]["result"]["aggregate_diff"]["reference"]
            .as_str()
            .unwrap()
            .to_owned();
        drop(runtime);

        let mut restarted = Runtime::with_store(&data_root).unwrap();
        ready(&mut restarted);
        let page = restarted.handle_line(&request(
            "preview-page",
            "workspace/edit/artifact/read",
            json!({
                "session_id": &session_id,
                "project_id": &project_id,
                "edit_id": "durable-edit",
                "reference": &aggregate_reference,
                "offset": 0,
                "limit": 32
            }),
        ));
        let decoded = BASE64_STANDARD
            .decode(page[0]["result"]["data_base64"].as_str().unwrap())
            .unwrap();
        assert!(String::from_utf8_lossy(&decoded).contains("--- a/src/main.rs"));
        let replay = restarted.handle_line(&request(
            "session-read",
            "session/read",
            json!({"session_id": &session_id}),
        ));
        assert_eq!(replay[0]["result"]["consistency"]["consistent"], true);
        assert!(
            replay[0]["result"]["consistency"]["checked_blob_references"]
                .as_u64()
                .unwrap()
                >= 2
        );

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn session_deletion_aap_previews_freezes_undoes_and_protects_live_work() {
        let root = std::env::temp_dir().join(format!(
            "aegisy-runtime-session-deletion-{}-{}",
            std::process::id(),
            now_ms()
        ));
        let data_root = root.join("data");
        fs::create_dir_all(&data_root).unwrap();

        let mut runtime = Runtime::with_store(&data_root).unwrap();
        ready(&mut runtime);
        let initialized = runtime.handle_line(&request(
            "initialize-capabilities-again",
            "initialize",
            json!({
                "protocol_version": "0.1",
                "client": {"name": "deletion-test", "version": "1"}
            }),
        ));
        assert!(initialized[0]["result"]["capabilities"]
            .as_array()
            .unwrap()
            .iter()
            .any(|capability| capability == "session.deletion.two-phase"));

        let started = runtime.handle_line(&request(
            "session-start",
            "session/start",
            json!({"mode": "chat", "title": "Deletion fixture"}),
        ));
        let session_id = started[0]["result"]["session"]["id"]
            .as_str()
            .unwrap()
            .to_owned();

        runtime.control.begin_turn(&session_id);
        let active_preview = runtime.handle_line(&request(
            "delete-preview-active",
            "session/delete/preview",
            json!({"session_id": &session_id, "scope": "session-only"}),
        ));
        assert!(active_preview[0]["result"]["blocking_reasons"]
            .as_array()
            .unwrap()
            .iter()
            .any(|reason| reason == "session-active-turn"));
        let denied_active = runtime.handle_line(&request(
            "delete-schedule-active",
            "session/delete/schedule",
            json!({
                "session_id": &session_id,
                "scope": "session-only",
                "plan_hash": active_preview[0]["result"]["plan_hash"].clone(),
                "undo_window_ms": 7 * 24 * 60 * 60 * 1_000_u64
            }),
        ));
        assert_eq!(denied_active[0]["error"]["code"], -32025);
        runtime.control.finish_turn(&session_id);

        runtime.control.terminals.lock().unwrap().records.insert(
            "terminal-delete-fixture".into(),
            TerminalRecord {
                terminal_id: "terminal-delete-fixture".into(),
                session_id: session_id.clone(),
                project_id: "project-fixture".into(),
                kind: "foreground".into(),
                name: "Shell".into(),
                generation: 1,
                state: "running".into(),
                input_policy: "user-only".into(),
                created_at_ms: now_ms(),
                updated_at_ms: now_ms(),
            },
        );
        let terminal_preview = runtime.handle_line(&request(
            "delete-preview-terminal",
            "session/delete/preview",
            json!({"session_id": &session_id, "scope": "session-only"}),
        ));
        assert!(terminal_preview[0]["result"]["blocking_reasons"]
            .as_array()
            .unwrap()
            .iter()
            .any(|reason| reason == "session-running-terminal"));
        runtime
            .control
            .terminals
            .lock()
            .unwrap()
            .records
            .remove("terminal-delete-fixture");

        let preview = runtime.handle_line(&request(
            "delete-preview",
            "session/delete/preview",
            json!({"session_id": &session_id, "scope": "session-only"}),
        ));
        assert_eq!(preview[0]["result"]["session_count"], 1);
        assert!(preview[0]["result"]["blocking_reasons"]
            .as_array()
            .unwrap()
            .is_empty());
        let scheduled = runtime.handle_line(&request(
            "delete-schedule",
            "session/delete/schedule",
            json!({
                "session_id": &session_id,
                "scope": "session-only",
                "plan_hash": preview[0]["result"]["plan_hash"].clone(),
                "undo_window_ms": 7 * 24 * 60 * 60 * 1_000_u64
            }),
        ));
        assert_eq!(scheduled[0]["result"]["state"], "pending");
        let deletion_id = scheduled[0]["result"]["deletion_id"]
            .as_str()
            .unwrap()
            .to_owned();

        let listed = runtime.handle_line(&request(
            "session-list-pending",
            "session/list",
            json!({"include_archived": true}),
        ));
        assert_eq!(listed[0]["result"]["sessions"][0]["deletion_pending"], true);
        assert_eq!(
            listed[0]["result"]["sessions"][0]["deletion"]["deletion_id"],
            deletion_id
        );
        let readable = runtime.handle_line(&request(
            "session-read-pending",
            "session/read",
            json!({"session_id": &session_id}),
        ));
        assert_eq!(readable[0]["result"]["session"]["id"], session_id);
        let frozen = runtime.handle_line(&request(
            "session-title-pending",
            "session/title",
            json!({"session_id": &session_id, "title": "Must remain frozen"}),
        ));
        assert_eq!(frozen[0]["error"]["code"], -32131);
        let status = runtime.handle_line(&request(
            "delete-status",
            "session/deletion/status",
            json!({"session_id": &session_id}),
        ));
        assert_eq!(status[0]["result"]["deletion"]["state"], "pending");

        let undone = runtime.handle_line(&request(
            "delete-undo",
            "session/delete/undo",
            json!({"deletion_id": &deletion_id}),
        ));
        assert_eq!(undone[0]["result"]["state"], "cancelled");
        let renamed = runtime.handle_line(&request(
            "session-title-after-undo",
            "session/title",
            json!({"session_id": &session_id, "title": "Deletion undone"}),
        ));
        assert_eq!(renamed[0]["result"]["title"], "Deletion undone");

        let policy = runtime.handle_line(&request(
            "retention-policy-set",
            "retention/policy/set",
            json!({
                "scope_kind": "session",
                "scope_id": &session_id,
                "archive_after_ms": 24 * 60 * 60 * 1_000_u64,
                "delete_after_ms": 7 * 24 * 60 * 60 * 1_000_u64,
                "undo_window_ms": 7 * 24 * 60 * 60 * 1_000_u64,
                "delete_scope": "session-only"
            }),
        ));
        assert_eq!(policy[0]["result"]["scope_id"], session_id);
        let loaded_policy = runtime.handle_line(&request(
            "retention-policy-read",
            "retention/policy/read",
            json!({"scope_kind": "session", "scope_id": &session_id}),
        ));
        assert_eq!(
            loaded_policy[0]["result"]["policy"]["delete_scope"],
            "session-only"
        );
        let removed_policy = runtime.handle_line(&request(
            "retention-policy-remove",
            "retention/policy/remove",
            json!({"scope_kind": "session", "scope_id": &session_id}),
        ));
        assert_eq!(removed_policy[0]["result"]["removed"], true);
        let missing_policy = runtime.handle_line(&request(
            "retention-policy-read-after-remove",
            "retention/policy/read",
            json!({"scope_kind": "session", "scope_id": &session_id}),
        ));
        assert!(missing_policy[0]["result"]["policy"].is_null());

        let due_preview = runtime
            .workbench_store
            .as_ref()
            .unwrap()
            .preview_session_deletion(&session_id, SessionDeletionScope::SessionOnly)
            .unwrap();
        let observed_at_ms = now_ms();
        let requested_at_ms = observed_at_ms - 8 * 24 * 60 * 60 * 1_000;
        runtime
            .workbench_store
            .as_mut()
            .unwrap()
            .schedule_session_deletion(
                "due-runtime-deletion",
                &session_id,
                SessionDeletionScope::SessionOnly,
                &due_preview.plan_hash,
                requested_at_ms,
                7 * 24 * 60 * 60 * 1_000,
            )
            .unwrap();
        let maintenance = runtime.handle_line(&request(
            "retention-maintenance",
            "retention/maintenance/run",
            json!({}),
        ));
        assert_eq!(maintenance[0]["result"]["deletions"]["purged"], 1);
        assert!(!runtime.sessions.contains_key(&session_id));
        let hidden = runtime.handle_line(&request(
            "session-list-after-purge",
            "session/list",
            json!({"include_archived": true}),
        ));
        assert!(hidden[0]["result"]["sessions"]
            .as_array()
            .unwrap()
            .is_empty());
        let deleted_read = runtime.handle_line(&request(
            "session-read-after-purge",
            "session/read",
            json!({"session_id": &session_id}),
        ));
        assert_eq!(deleted_read[0]["error"]["code"], -32023);
        drop(runtime);
        let mut restarted = Runtime::with_store(&data_root).unwrap();
        ready(&mut restarted);
        let recovery = restarted.handle_line(&request(
            "recovery-after-purge",
            "runtime/projection-recovery/status",
            json!({}),
        ));
        assert_eq!(recovery[0]["result"]["current_quarantined_sessions"], 0);
        let restarted_list = restarted.handle_line(&request(
            "list-after-purge-restart",
            "session/list",
            json!({"include_archived": true}),
        ));
        assert!(restarted_list[0]["result"]["sessions"]
            .as_array()
            .unwrap()
            .is_empty());

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn project_open_returns_bounded_trust_review_without_reading_instruction_content() {
        let root = std::env::temp_dir().join(format!(
            "aegisy-trust-review-{}-{}",
            std::process::id(),
            now_ms()
        ));
        fs::create_dir_all(root.join(".git/hooks")).unwrap();
        fs::create_dir_all(root.join("src")).unwrap();
        fs::write(
            root.join("AGENTS.md"),
            "do not treat this as trusted policy\n",
        )
        .unwrap();
        let hook = root.join(".git/hooks/pre-commit");
        fs::write(&hook, "#!/bin/sh\necho hook\n").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&hook, fs::Permissions::from_mode(0o700)).unwrap();
        }
        let canonical = root.canonicalize().unwrap();
        let mut runtime = Runtime::default();
        ready(&mut runtime);
        let response = runtime.handle_line(&request(
            "trust-review",
            "project/open",
            json!({"root": canonical}),
        ));
        let review = response[0]["result"]["trust_review"].clone();
        assert_eq!(review["schema_version"], "project-trust-review/0.1");
        assert_eq!(review["required"], true);
        assert_eq!(review["roots"][0]["access"], "write");
        assert_eq!(review["repositories"][0]["relative_path"], ".");
        assert_eq!(review["instructions"][0]["relative_path"], "AGENTS.md");
        assert_eq!(review["instructions"][0]["content"], "not-read");
        assert_eq!(review["executable_hooks"][0]["execution"], "disabled");
        assert_eq!(review["policy_impact"]["agent_execution"], "read-only");
        assert!(review["review_id"]
            .as_str()
            .unwrap()
            .starts_with("project-trust-review:sha256:"));
        let serialized = serde_json::to_string(&response).unwrap();
        assert!(!serialized.contains("do not treat this as trusted policy"));
        assert!(!serialized.contains("echo hook"));
        let _ = fs::remove_dir_all(root);
    }
}
