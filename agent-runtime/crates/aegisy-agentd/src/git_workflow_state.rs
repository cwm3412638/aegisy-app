use crate::git_commit_transaction::{
    validate_identity, validate_message_source, GitCommitHookPolicy, GitCommitIdentity,
    GitCommitMessageSource, GitCommitSigningPolicy,
};
use crate::git_status::{status, GitOutput, GitRunner, GitStatusError};
use crate::workspace::is_sensitive_path;
use crate::workspace_edit::ContentHash;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, HashSet};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

const PLAN_SCHEMA_VERSION: &str = "git-workflow-plan/0.2";
const RECORD_SCHEMA_VERSION: &str = "git-workflow-record/0.2";
const ENVELOPE_SCHEMA_VERSION: &str = "git-workflow-envelope/0.2";
const MAX_MESSAGE_BYTES: usize = 16 * 1024;
const MAX_RECORD_BYTES: u64 = 1024 * 1024;
const MAX_RECORDS: usize = 128;
const MAX_STATUS_PATHS: usize = 5_000;
const MAX_CONFLICT_PATHS: usize = 2_000;
const MAX_GIT_OUTPUT: u64 = 2 * 1024 * 1024;
const KNOWN_WORKFLOW_HOOKS: &[&str] = &[
    "applypatch-msg",
    "commit-msg",
    "post-applypatch",
    "post-commit",
    "post-merge",
    "post-rewrite",
    "pre-applypatch",
    "pre-commit",
    "pre-merge-commit",
    "pre-rebase",
    "prepare-commit-msg",
    "reference-transaction",
];
static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub enum GitWorkflowRequest {
    StashCapture {
        include_untracked: bool,
        message: String,
        message_source: GitCommitMessageSource,
        identity: GitCommitIdentity,
        hook_policy: GitCommitHookPolicy,
        signing_policy: GitCommitSigningPolicy,
    },
    Merge {
        target_oid: String,
        mode: GitMergeMode,
        #[serde(skip_serializing_if = "Option::is_none")]
        commit: Option<GitWorkflowCommitMetadata>,
        hook_policy: GitCommitHookPolicy,
        signing_policy: GitCommitSigningPolicy,
    },
    Rebase {
        upstream_oid: String,
        onto_oid: String,
        committer: GitCommitIdentity,
        hook_policy: GitCommitHookPolicy,
        signing_policy: GitCommitSigningPolicy,
    },
    CherryPick {
        commit_oid: String,
        committer: GitCommitIdentity,
        hook_policy: GitCommitHookPolicy,
        signing_policy: GitCommitSigningPolicy,
    },
    Abort {
        operation_id: String,
        generation: u64,
    },
    Continue {
        operation_id: String,
        generation: u64,
    },
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum GitMergeMode {
    AllowFastForward,
    NoFastForward,
    FastForwardOnly,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GitWorkflowCommitMetadata {
    pub message: String,
    pub message_source: GitCommitMessageSource,
    pub identity: GitCommitIdentity,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitWorkflowPlan {
    pub schema_version: String,
    pub repository_root: String,
    pub root_identity: String,
    pub git_common_directory_identity: String,
    pub request: GitWorkflowRequest,
    pub operation_kind: String,
    pub expected_head: String,
    pub expected_branch: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub expected_index_tree: Option<String>,
    pub expected_index_state: ContentHash,
    pub target_oids: Vec<String>,
    pub predicted_behavior: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub base_stash_oid: Option<String>,
    pub visible_dirty_paths: Vec<String>,
    pub redacted_dirty_path_count: usize,
    pub pending_editor_path_count: usize,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub live_operation: Option<String>,
    pub risk: GitWorkflowRisk,
    pub hazards: GitWorkflowHazards,
    pub blocking_reasons: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GitWorkflowRisk {
    pub class: String,
    pub reasons: Vec<String>,
    pub requires_permission: bool,
    pub requires_explicit_approval: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitWorkflowHazards {
    pub active_hooks: Vec<String>,
    pub custom_hooks_path: bool,
    pub custom_merge_driver: bool,
    pub custom_filter_driver: bool,
    pub protected_branch: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GitWorkflowRecord {
    pub schema_version: String,
    pub operation_id: String,
    pub project_id: String,
    pub session_id: String,
    pub repository_root: String,
    pub root_identity: String,
    pub git_common_directory_identity: String,
    pub request: GitWorkflowRequest,
    pub operation_kind: String,
    pub risk: GitWorkflowRisk,
    pub base_head: String,
    pub base_branch: Option<String>,
    pub base_index_tree: Option<String>,
    pub base_index_state: ContentHash,
    pub target_oids: Vec<String>,
    pub predicted_behavior: String,
    pub base_stash_oid: Option<String>,
    pub state: String,
    pub generation: u64,
    pub visible_dirty_paths: Vec<String>,
    pub redacted_dirty_path_count: usize,
    pub conflicts: Vec<GitWorkflowConflict>,
    pub redacted_conflict_path_count: usize,
    pub allowed_actions: Vec<String>,
    pub created_at_ms: u64,
    pub updated_at_ms: u64,
    pub observed_head: String,
    pub observed_branch: Option<String>,
    pub observed_operation: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub execution: Option<GitWorkflowExecutionAttempt>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GitWorkflowExecutionAttempt {
    pub schema_version: String,
    pub authorization_id: String,
    pub requirement_hash: ContentHash,
    pub action: String,
    pub phase: String,
    pub source_generation: u64,
    pub started_at_ms: u64,
    pub updated_at_ms: u64,
    pub command_exit_code: Option<i32>,
    pub outcome: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GitWorkflowConflict {
    pub path: String,
    pub stages: Vec<GitWorkflowConflictStage>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GitWorkflowConflictStage {
    pub stage: u8,
    pub mode: String,
    pub oid: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GitWorkflowError {
    pub message: String,
}

#[derive(Debug)]
pub struct GitWorkflowStore {
    storage_root: PathBuf,
    records_root: PathBuf,
    repository_root: PathBuf,
    root_identity: String,
}

#[derive(Debug, Serialize, Deserialize)]
struct RecordEnvelope {
    schema_version: String,
    payload_hash: ContentHash,
    payload: GitWorkflowRecord,
}

#[derive(Debug)]
struct IndexState {
    tree_oid: Option<String>,
    state_hash: ContentHash,
}

pub fn plan_git_workflow(
    root: &Path,
    request: GitWorkflowRequest,
    pending_editor_paths: &HashSet<String>,
    active_record: Option<&GitWorkflowRecord>,
) -> Result<GitWorkflowPlan, GitWorkflowError> {
    validate_pending_paths(pending_editor_paths)?;
    validate_request(&request)?;
    let root = canonical_directory(root, "Git repository root")?;
    let root_text = path_to_utf8(&root, "Git repository root")?;
    let root_identity_value = root_identity(&root);
    let snapshot = status(&root).map_err(from_git)?;
    if !snapshot.repository || !snapshot.worktree {
        return Err(error("project is not inside a Git worktree"));
    }
    let expected_head = snapshot
        .head_oid
        .clone()
        .ok_or_else(|| error("Git workflow requires an existing HEAD"))?;
    let runner = GitRunner::new(&root).map_err(from_git)?;
    let repository_root = required_path(&runner, &["rev-parse", "--show-toplevel"])?;
    if repository_root != root {
        return Err(error("Git workflow requires the opened repository root"));
    }
    let common_directory = required_path(
        &runner,
        &["rev-parse", "--path-format=absolute", "--git-common-dir"],
    )?;
    let git_common_directory_identity = root_identity(&common_directory);
    let index = index_state(&runner, !snapshot.conflicts.is_empty())?;
    let mut blocking_reasons = Vec::new();
    if snapshot.truncated {
        blocking_reasons.push("git-status-truncated".into());
    }
    let visible_dirty_paths = visible_paths(snapshot.entries.iter().flat_map(|entry| {
        entry
            .original_path
            .iter()
            .chain(std::iter::once(&entry.path))
    }));
    let dirty_all = snapshot
        .entries
        .iter()
        .flat_map(|entry| {
            entry
                .original_path
                .iter()
                .chain(std::iter::once(&entry.path))
        })
        .collect::<HashSet<_>>();
    let redacted_dirty_path_count = dirty_all
        .iter()
        .filter(|path| is_sensitive_path(Path::new(path.as_str())))
        .count();
    let hazards = inspect_hazards(&runner, &common_directory, snapshot.branch.as_deref())?;
    let operation_kind = request_operation_kind(&request, active_record)?;
    let target_oids = validate_targets(&runner, &request)?;
    let (predicted_behavior, base_stash_oid) = inspect_execution_shape(
        &runner,
        &request,
        &expected_head,
        active_record,
        &mut blocking_reasons,
    )?;
    let risk = classify_risk(&request, &hazards);
    let live_operation = snapshot
        .operation_in_progress
        .as_ref()
        .map(|operation| operation.kind.clone());

    match &request {
        GitWorkflowRequest::StashCapture { .. } => {
            if snapshot.entries.is_empty() {
                blocking_reasons.push("nothing-to-stash".into());
            }
            block_start_state(
                &snapshot,
                pending_editor_paths,
                &hazards,
                true,
                &mut blocking_reasons,
            );
        }
        GitWorkflowRequest::Merge { target_oid, .. } => {
            block_start_state(
                &snapshot,
                pending_editor_paths,
                &hazards,
                false,
                &mut blocking_reasons,
            );
            if target_oid == &expected_head {
                blocking_reasons.push("merge-target-is-current-head".into());
            }
        }
        GitWorkflowRequest::Rebase { .. } => {
            block_start_state(
                &snapshot,
                pending_editor_paths,
                &hazards,
                false,
                &mut blocking_reasons,
            );
            if hazards.protected_branch {
                blocking_reasons.push("protected-branch-history-rewrite".into());
            }
        }
        GitWorkflowRequest::CherryPick { .. } => block_start_state(
            &snapshot,
            pending_editor_paths,
            &hazards,
            false,
            &mut blocking_reasons,
        ),
        GitWorkflowRequest::Abort {
            operation_id,
            generation,
        } => {
            let record = validate_active_record(
                &root_identity_value,
                operation_id,
                *generation,
                active_record,
            )?;
            if live_operation.as_deref() != Some(record.operation_kind.as_str()) {
                blocking_reasons.push("live-operation-does-not-match-record".into());
            }
            if !pending_editor_paths.is_empty() {
                blocking_reasons.push("pending-editor-edits".into());
            }
        }
        GitWorkflowRequest::Continue {
            operation_id,
            generation,
        } => {
            let record = validate_active_record(
                &root_identity_value,
                operation_id,
                *generation,
                active_record,
            )?;
            if live_operation.as_deref() != Some(record.operation_kind.as_str()) {
                blocking_reasons.push("live-operation-does-not-match-record".into());
            }
            if !snapshot.conflicts.is_empty() {
                blocking_reasons.push("unresolved-conflicts".into());
            }
            if !pending_editor_paths.is_empty() {
                blocking_reasons.push("pending-editor-edits".into());
            }
        }
    }
    block_execution_policy(&request, active_record, &mut blocking_reasons)?;
    blocking_reasons.sort();
    blocking_reasons.dedup();
    Ok(GitWorkflowPlan {
        schema_version: PLAN_SCHEMA_VERSION.into(),
        repository_root: root_text,
        root_identity: root_identity_value,
        git_common_directory_identity,
        request,
        operation_kind,
        expected_head,
        expected_branch: snapshot.branch,
        expected_index_tree: index.tree_oid,
        expected_index_state: index.state_hash,
        target_oids,
        predicted_behavior,
        base_stash_oid,
        visible_dirty_paths,
        redacted_dirty_path_count,
        pending_editor_path_count: pending_editor_paths.len(),
        live_operation,
        risk,
        hazards,
        blocking_reasons,
    })
}

impl GitWorkflowStore {
    pub fn open(storage_root: &Path, repository_root: &Path) -> Result<Self, GitWorkflowError> {
        let storage_metadata = fs::symlink_metadata(storage_root)
            .map_err(|_| error("Git workflow storage root is unavailable"))?;
        if storage_metadata.file_type().is_symlink() {
            return Err(error("Git workflow storage root must not be a symlink"));
        }
        let storage_root = canonical_directory(storage_root, "Git workflow storage root")?;
        let repository_root = canonical_directory(repository_root, "Git repository root")?;
        if storage_root.starts_with(&repository_root) || repository_root.starts_with(&storage_root)
        {
            return Err(error("Git workflow storage must be outside the repository"));
        }
        let records_root = storage_root.join("git-workflows");
        ensure_private_directory(&records_root)?;
        Ok(Self {
            storage_root,
            records_root,
            root_identity: root_identity(&repository_root),
            repository_root,
        })
    }

    pub fn create_planned(
        &self,
        operation_id: &str,
        project_id: &str,
        session_id: &str,
        plan: &GitWorkflowPlan,
    ) -> Result<GitWorkflowRecord, GitWorkflowError> {
        validate_identifier(operation_id, "operation ID")?;
        validate_identifier(project_id, "project ID")?;
        validate_identifier(session_id, "session ID")?;
        validate_plan_binding(self, plan)?;
        if !plan.blocking_reasons.is_empty() {
            return Err(error(
                "blocked Git workflow plan cannot be persisted as planned",
            ));
        }
        if self.record_count()? >= MAX_RECORDS {
            return Err(error("Git workflow record limit exceeded"));
        }
        let timestamp = now_ms()?;
        let record = GitWorkflowRecord {
            schema_version: RECORD_SCHEMA_VERSION.into(),
            operation_id: operation_id.into(),
            project_id: project_id.into(),
            session_id: session_id.into(),
            repository_root: path_to_utf8(&self.repository_root, "Git repository root")?,
            root_identity: self.root_identity.clone(),
            git_common_directory_identity: plan.git_common_directory_identity.clone(),
            request: plan.request.clone(),
            operation_kind: plan.operation_kind.clone(),
            risk: plan.risk.clone(),
            base_head: plan.expected_head.clone(),
            base_branch: plan.expected_branch.clone(),
            base_index_tree: plan.expected_index_tree.clone(),
            base_index_state: plan.expected_index_state.clone(),
            target_oids: plan.target_oids.clone(),
            predicted_behavior: plan.predicted_behavior.clone(),
            base_stash_oid: plan.base_stash_oid.clone(),
            state: "planned".into(),
            generation: 0,
            visible_dirty_paths: plan.visible_dirty_paths.clone(),
            redacted_dirty_path_count: plan.redacted_dirty_path_count,
            conflicts: Vec::new(),
            redacted_conflict_path_count: 0,
            allowed_actions: vec!["start".into(), "cancel".into()],
            created_at_ms: timestamp,
            updated_at_ms: timestamp,
            observed_head: plan.expected_head.clone(),
            observed_branch: plan.expected_branch.clone(),
            observed_operation: plan.live_operation.clone(),
            execution: None,
        };
        self.write_new(&record)?;
        Ok(record)
    }

    pub fn load(&self, operation_id: &str) -> Result<GitWorkflowRecord, GitWorkflowError> {
        validate_identifier(operation_id, "operation ID")?;
        let path = self.record_path(operation_id);
        let bytes = read_bounded(&path, MAX_RECORD_BYTES, "Git workflow record")?;
        let envelope: RecordEnvelope = serde_json::from_slice(&bytes)
            .map_err(|_| error("Git workflow record JSON is invalid"))?;
        if envelope.schema_version != ENVELOPE_SCHEMA_VERSION
            || envelope.payload.schema_version != RECORD_SCHEMA_VERSION
            || envelope.payload.operation_id != operation_id
            || envelope.payload.root_identity != self.root_identity
            || envelope.payload.repository_root
                != path_to_utf8(&self.repository_root, "Git repository root")?
        {
            return Err(error("Git workflow record identity is inconsistent"));
        }
        let payload = serde_json::to_vec(&envelope.payload)
            .map_err(|_| error("cannot serialize Git workflow record"))?;
        if ContentHash::for_bytes(&payload) != envelope.payload_hash {
            return Err(error("Git workflow record integrity check failed"));
        }
        validate_record(&envelope.payload)?;
        Ok(envelope.payload)
    }

    pub fn reconcile(&self, operation_id: &str) -> Result<GitWorkflowRecord, GitWorkflowError> {
        let mut record = self.load(operation_id)?;
        let snapshot = status(&self.repository_root).map_err(from_git)?;
        let head = snapshot
            .head_oid
            .clone()
            .ok_or_else(|| error("Git workflow reconciliation requires HEAD"))?;
        let live_operation = snapshot
            .operation_in_progress
            .as_ref()
            .map(|operation| operation.kind.clone());
        let (mut conflicts, mut redacted_conflict_path_count) =
            conflict_evidence(&self.repository_root)?;
        let matches = live_operation.as_deref() == Some(record.operation_kind.as_str());
        let has_conflicts = !conflicts.is_empty() || redacted_conflict_path_count > 0;
        record.state = if live_operation.is_some() && matches && has_conflicts {
            "conflicted"
        } else if live_operation.is_some() && matches {
            "in-progress"
        } else if live_operation.is_some() {
            "blocked-foreign-operation"
        } else if matches!(record.state.as_str(), "in-progress" | "conflicted") {
            "interrupted-needs-reconciliation"
        } else {
            record.state.as_str()
        }
        .into();
        if !matches {
            conflicts.clear();
            redacted_conflict_path_count = 0;
        }
        record.allowed_actions = match record.state.as_str() {
            "conflicted" => vec!["resolve".into(), "abort".into()],
            "in-progress" => vec!["continue".into(), "abort".into()],
            "planned" => vec!["start".into(), "cancel".into()],
            "blocked-foreign-operation" | "interrupted-needs-reconciliation" => {
                vec!["inspect".into()]
            }
            _ => Vec::new(),
        };
        record.conflicts = conflicts;
        record.redacted_conflict_path_count = redacted_conflict_path_count;
        record.generation = record.generation.saturating_add(1);
        record.updated_at_ms = now_ms()?;
        record.observed_head = head;
        record.observed_branch = snapshot.branch;
        record.observed_operation = live_operation;
        self.replace(&record)?;
        Ok(record)
    }

    fn write_new(&self, record: &GitWorkflowRecord) -> Result<(), GitWorkflowError> {
        let bytes = envelope_bytes(record)?;
        let target = self.record_path(&record.operation_id);
        if fs::symlink_metadata(&target).is_ok() {
            return Err(error("Git workflow operation ID is already in use"));
        }
        let temporary = self.temporary_path(&record.operation_id);
        write_private_file(&temporary, &bytes)?;
        if fs::hard_link(&temporary, &target).is_err() {
            let _ = fs::remove_file(&temporary);
            return Err(error(
                "cannot commit Git workflow record without clobbering",
            ));
        }
        let _ = fs::remove_file(&temporary);
        sync_directory(&self.records_root)
    }

    fn replace(&self, record: &GitWorkflowRecord) -> Result<(), GitWorkflowError> {
        let bytes = envelope_bytes(record)?;
        let target = self.record_path(&record.operation_id);
        let metadata = fs::symlink_metadata(&target)
            .map_err(|_| error("Git workflow record is unavailable"))?;
        if metadata.file_type().is_symlink() || !metadata.is_file() {
            return Err(error("Git workflow record is not a regular file"));
        }
        let temporary = self.temporary_path(&record.operation_id);
        write_private_file(&temporary, &bytes)?;
        atomic_replace(&temporary, &target)
            .map_err(|_| error("cannot atomically replace Git workflow record"))?;
        sync_directory(&self.records_root)
    }

    fn record_count(&self) -> Result<usize, GitWorkflowError> {
        let mut count = 0_usize;
        for entry in fs::read_dir(&self.records_root)
            .map_err(|_| error("cannot list Git workflow records"))?
        {
            let entry = entry.map_err(|_| error("cannot inspect Git workflow record"))?;
            let name = entry.file_name();
            if name.to_string_lossy().ends_with(".json") {
                count += 1;
            }
        }
        Ok(count)
    }

    fn record_path(&self, operation_id: &str) -> PathBuf {
        self.records_root.join(format!("{operation_id}.json"))
    }

    fn temporary_path(&self, operation_id: &str) -> PathBuf {
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        self.records_root.join(format!(
            ".{operation_id}.{}.{}.tmp",
            std::process::id(),
            sequence
        ))
    }

    pub fn storage_root(&self) -> &Path {
        &self.storage_root
    }

    pub(crate) fn repository_root(&self) -> &Path {
        &self.repository_root
    }

    pub(crate) fn transition(
        &self,
        expected: &GitWorkflowRecord,
        next: &GitWorkflowRecord,
    ) -> Result<(), GitWorkflowError> {
        validate_record(expected)?;
        validate_record(next)?;
        if expected.operation_id != next.operation_id
            || expected.project_id != next.project_id
            || expected.session_id != next.session_id
            || expected.root_identity != next.root_identity
            || expected.repository_root != next.repository_root
            || next.generation != expected.generation.saturating_add(1)
        {
            return Err(error("Git workflow record transition is invalid"));
        }
        let lock_path = self.records_root.join(".records.lock");
        let lock = open_store_lock(&lock_path)?;
        lock.try_lock()
            .map_err(|_| error("Git workflow record store is busy"))?;
        let result = (|| {
            let current = self.load(&expected.operation_id)?;
            if current != *expected {
                return Err(error("Git workflow record changed before transition"));
            }
            self.replace(next)
        })();
        let _ = lock.unlock();
        result
    }

    pub(crate) fn observe_execution(
        &self,
        expected: &GitWorkflowRecord,
        command_exit_code: Option<i32>,
        recovered_after_restart: bool,
    ) -> Result<GitWorkflowRecord, GitWorkflowError> {
        if !matches!(
            expected.state.as_str(),
            "execution-prepared" | "execution-dispatching"
        ) {
            return Err(error(
                "Git workflow record is not awaiting execution observation",
            ));
        }
        let action = expected
            .execution
            .as_ref()
            .ok_or_else(|| error("Git workflow execution journal is missing"))?
            .action
            .clone();
        let snapshot = status(&self.repository_root).map_err(from_git)?;
        let head = snapshot
            .head_oid
            .clone()
            .ok_or_else(|| error("Git workflow execution observation requires HEAD"))?;
        let live_operation = snapshot
            .operation_in_progress
            .as_ref()
            .map(|operation| operation.kind.clone());
        let (mut conflicts, mut redacted_conflict_path_count) =
            conflict_evidence(&self.repository_root)?;
        let matches = live_operation.as_deref() == Some(expected.operation_kind.as_str());
        let has_conflicts = !conflicts.is_empty() || redacted_conflict_path_count > 0;
        let (state, outcome) = if live_operation.is_some() && matches && has_conflicts {
            ("conflicted", "conflict-observed")
        } else if live_operation.is_some() && matches {
            ("in-progress", "operation-paused")
        } else if live_operation.is_some() {
            conflicts.clear();
            redacted_conflict_path_count = 0;
            ("blocked-foreign-operation", "foreign-operation-observed")
        } else if command_exit_code == Some(0) {
            conflicts.clear();
            redacted_conflict_path_count = 0;
            if verify_execution_postcondition(self, expected, &action, &snapshot)? {
                if action == "abort" {
                    ("aborted", "abort-verified")
                } else {
                    ("completed", "completion-verified")
                }
            } else {
                (
                    "failed-needs-reconciliation",
                    "success-postcondition-mismatch",
                )
            }
        } else if recovered_after_restart || command_exit_code.is_none() {
            conflicts.clear();
            redacted_conflict_path_count = 0;
            if verify_execution_postcondition(self, expected, &action, &snapshot)? {
                if action == "abort" {
                    ("aborted", "abort-recovered-and-verified")
                } else {
                    ("completed", "completion-recovered-and-verified")
                }
            } else {
                (
                    "interrupted-needs-reconciliation",
                    "command-outcome-unknown",
                )
            }
        } else {
            conflicts.clear();
            redacted_conflict_path_count = 0;
            if action == "start" {
                ("failed", "command-failed-without-live-operation")
            } else {
                (
                    "failed-needs-reconciliation",
                    "recovery-command-failed-without-live-operation",
                )
            }
        };
        let timestamp = now_ms()?;
        let mut next = expected.clone();
        next.state = state.into();
        next.allowed_actions = allowed_actions_for_state(state, &action);
        next.conflicts = conflicts;
        next.redacted_conflict_path_count = redacted_conflict_path_count;
        next.generation = expected.generation.saturating_add(1);
        next.updated_at_ms = timestamp;
        next.observed_head = head;
        next.observed_branch = snapshot.branch;
        next.observed_operation = live_operation;
        let attempt = next
            .execution
            .as_mut()
            .ok_or_else(|| error("Git workflow execution journal is missing"))?;
        attempt.phase = if recovered_after_restart {
            "recovered"
        } else {
            "observed"
        }
        .into();
        attempt.updated_at_ms = timestamp;
        attempt.command_exit_code = command_exit_code;
        attempt.outcome = Some(outcome.into());
        self.transition(expected, &next)?;
        Ok(next)
    }
}

fn allowed_actions_for_state(state: &str, execution_action: &str) -> Vec<String> {
    match state {
        "planned" => vec!["start".into(), "cancel".into()],
        "conflicted" => vec!["resolve".into(), "abort".into()],
        "in-progress" => vec!["continue".into(), "abort".into()],
        "failed" if execution_action == "start" => vec!["start".into(), "inspect".into()],
        "blocked-foreign-operation"
        | "interrupted-needs-reconciliation"
        | "failed-needs-reconciliation" => vec!["inspect".into()],
        _ => Vec::new(),
    }
}

fn verify_execution_postcondition(
    store: &GitWorkflowStore,
    record: &GitWorkflowRecord,
    action: &str,
    snapshot: &crate::git_status::GitStatusSnapshot,
) -> Result<bool, GitWorkflowError> {
    if action == "abort" {
        let runner = GitRunner::new(store.repository_root()).map_err(from_git)?;
        let index = index_state(&runner, false)?;
        return Ok(
            snapshot.head_oid.as_deref() == Some(record.base_head.as_str())
                && snapshot.branch == record.base_branch
                && snapshot.entries.is_empty()
                && index.tree_oid == record.base_index_tree
                && index.state_hash == record.base_index_state,
        );
    }
    match &record.request {
        GitWorkflowRequest::StashCapture { .. } => {
            let runner = GitRunner::new(store.repository_root()).map_err(from_git)?;
            let stash = optional_ref_oid(&runner, "refs/stash")?;
            Ok(
                snapshot.head_oid.as_deref() == Some(record.base_head.as_str())
                    && stash.is_some()
                    && stash != record.base_stash_oid,
            )
        }
        GitWorkflowRequest::Merge {
            target_oid, commit, ..
        } => match record.predicted_behavior.as_str() {
            "fast-forward" => Ok(snapshot.head_oid.as_deref() == Some(target_oid.as_str())),
            "merge-commit" => {
                let Some(metadata) = commit else {
                    return Ok(false);
                };
                verify_merge_commit(store.repository_root(), record, target_oid, metadata)
            }
            _ => Ok(false),
        },
        GitWorkflowRequest::Rebase { onto_oid, .. } => {
            let runner = GitRunner::new(store.repository_root()).map_err(from_git)?;
            let Some(head) = snapshot.head_oid.as_deref() else {
                return Ok(false);
            };
            Ok(snapshot.branch == record.base_branch
                && head != record.base_head
                && is_ancestor(&runner, onto_oid, head)?)
        }
        GitWorkflowRequest::CherryPick {
            commit_oid,
            committer,
            ..
        } => verify_cherry_pick_commit(store.repository_root(), record, commit_oid, committer),
        GitWorkflowRequest::Abort { .. } | GitWorkflowRequest::Continue { .. } => Ok(false),
    }
}

fn verify_merge_commit(
    root: &Path,
    record: &GitWorkflowRecord,
    target_oid: &str,
    metadata: &GitWorkflowCommitMetadata,
) -> Result<bool, GitWorkflowError> {
    let runner = GitRunner::new(root).map_err(from_git)?;
    let Some(head) = status(root).map_err(from_git)?.head_oid else {
        return Ok(false);
    };
    let raw = commit_text(&runner, &head)?;
    let (headers, message) = raw
        .split_once("\n\n")
        .ok_or_else(|| error("Git merge commit object is malformed"))?;
    let lines = headers.lines().collect::<Vec<_>>();
    if lines.len() < 5
        || lines[1] != format!("parent {}", record.base_head)
        || lines[2] != format!("parent {target_oid}")
        || lines[3] != identity_header("author", &metadata.identity)
        || lines[4] != identity_header("committer", &metadata.identity)
    {
        return Ok(false);
    }
    Ok(message.trim_end_matches('\n') == metadata.message)
}

fn verify_cherry_pick_commit(
    root: &Path,
    record: &GitWorkflowRecord,
    source_oid: &str,
    committer: &GitCommitIdentity,
) -> Result<bool, GitWorkflowError> {
    let runner = GitRunner::new(root).map_err(from_git)?;
    let Some(head) = status(root).map_err(from_git)?.head_oid else {
        return Ok(false);
    };
    let source = commit_text(&runner, source_oid)?;
    let actual = commit_text(&runner, &head)?;
    let (source_headers, source_message) = source
        .split_once("\n\n")
        .ok_or_else(|| error("Git cherry-pick source commit is malformed"))?;
    let (actual_headers, actual_message) = actual
        .split_once("\n\n")
        .ok_or_else(|| error("Git cherry-pick result commit is malformed"))?;
    let source_author = source_headers
        .lines()
        .find(|line| line.starts_with("author "));
    let actual_lines = actual_headers.lines().collect::<Vec<_>>();
    Ok(
        actual_lines.get(1) == Some(&format!("parent {}", record.base_head).as_str())
            && actual_lines.get(2).copied() == source_author
            && actual_lines.get(3).copied()
                == Some(identity_header("committer", committer).as_str())
            && actual_message == source_message,
    )
}

fn commit_text(runner: &GitRunner, oid: &str) -> Result<String, GitWorkflowError> {
    let output = runner
        .run(
            &["cat-file", "commit", oid],
            None,
            MAX_MESSAGE_BYTES as u64 + 16 * 1024,
        )
        .map_err(from_git)?;
    require_success(&output, "Git workflow commit verification failed")?;
    String::from_utf8(output.stdout)
        .map_err(|_| error("Git workflow commit verification is not UTF-8"))
}

fn identity_header(kind: &str, identity: &GitCommitIdentity) -> String {
    format!(
        "{kind} {} <{}> {} {}",
        identity.name, identity.email, identity.timestamp_seconds, identity.timezone
    )
}

fn validate_pending_paths(paths: &HashSet<String>) -> Result<(), GitWorkflowError> {
    if paths.len() > MAX_STATUS_PATHS
        || paths.iter().any(|path| {
            path.is_empty()
                || path.len() > 4 * 1024
                || Path::new(path).is_absolute()
                || Path::new(path)
                    .components()
                    .any(|component| !matches!(component, Component::Normal(_)))
        })
    {
        return Err(error("pending editor path set is invalid"));
    }
    Ok(())
}

fn validate_request(request: &GitWorkflowRequest) -> Result<(), GitWorkflowError> {
    match request {
        GitWorkflowRequest::StashCapture {
            message,
            message_source,
            identity,
            ..
        } => {
            validate_workflow_message(message, message_source)?;
            validate_workflow_identity(identity)
        }
        GitWorkflowRequest::Merge {
            target_oid, commit, ..
        } => {
            validate_oid(target_oid)?;
            if let Some(commit) = commit {
                validate_commit_metadata(commit)?;
            }
            Ok(())
        }
        GitWorkflowRequest::Rebase {
            upstream_oid,
            onto_oid,
            committer,
            ..
        } => {
            validate_oid(upstream_oid)?;
            validate_oid(onto_oid)?;
            validate_workflow_identity(committer)
        }
        GitWorkflowRequest::CherryPick {
            commit_oid,
            committer,
            ..
        } => {
            validate_oid(commit_oid)?;
            validate_workflow_identity(committer)
        }
        GitWorkflowRequest::Abort {
            operation_id,
            generation: _,
        }
        | GitWorkflowRequest::Continue {
            operation_id,
            generation: _,
        } => validate_identifier(operation_id, "operation ID"),
    }
}

fn validate_commit_metadata(metadata: &GitWorkflowCommitMetadata) -> Result<(), GitWorkflowError> {
    validate_workflow_message(&metadata.message, &metadata.message_source)?;
    validate_workflow_identity(&metadata.identity)
}

fn validate_workflow_message(
    message: &str,
    source: &GitCommitMessageSource,
) -> Result<(), GitWorkflowError> {
    if message.is_empty()
        || message.len() > MAX_MESSAGE_BYTES
        || message.trim() != message
        || message.contains(['\0', '\r'])
        || message
            .chars()
            .any(|character| character.is_control() && !matches!(character, '\n' | '\t'))
        || message
            .lines()
            .next()
            .is_none_or(|subject| subject.trim().is_empty() || subject.chars().count() > 200)
    {
        return Err(error("Git workflow message is invalid"));
    }
    validate_message_source(source).map_err(|cause| error(cause.message))
}

fn validate_workflow_identity(identity: &GitCommitIdentity) -> Result<(), GitWorkflowError> {
    validate_identity(identity).map_err(|cause| error(cause.message))
}

fn validate_oid(oid: &str) -> Result<(), GitWorkflowError> {
    if valid_oid(oid) {
        Ok(())
    } else {
        Err(error("Git workflow object ID is invalid"))
    }
}

fn valid_oid(oid: &str) -> bool {
    matches!(oid.len(), 40 | 64)
        && oid
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
}

fn canonical_directory(path: &Path, label: &str) -> Result<PathBuf, GitWorkflowError> {
    let canonical = path
        .canonicalize()
        .map_err(|_| error(format!("{label} is unavailable")))?;
    if !canonical.is_dir() {
        return Err(error(format!("{label} is not a directory")));
    }
    Ok(canonical)
}

fn path_to_utf8(path: &Path, label: &str) -> Result<String, GitWorkflowError> {
    path.to_str()
        .map(str::to_owned)
        .ok_or_else(|| error(format!("{label} is not UTF-8")))
}

fn root_identity(path: &Path) -> String {
    let digest = Sha256::digest(path.to_string_lossy().as_bytes());
    format!("git-root:sha256:{digest:x}")
}

fn required_path(runner: &GitRunner, args: &[&str]) -> Result<PathBuf, GitWorkflowError> {
    let output = runner.run(args, None, 16 * 1024).map_err(from_git)?;
    require_success(&output, "Git path query failed")?;
    let text = std::str::from_utf8(&output.stdout)
        .map_err(|_| error("Git path query is not UTF-8"))?
        .trim();
    if text.is_empty() {
        return Err(error("Git path query returned an empty path"));
    }
    canonical_directory(Path::new(text), "Git path query result")
}

fn index_state(runner: &GitRunner, conflicted: bool) -> Result<IndexState, GitWorkflowError> {
    let entries = runner
        .run(&["ls-files", "--stage", "-z"], None, MAX_GIT_OUTPUT)
        .map_err(from_git)?;
    require_success(&entries, "Git index query failed")?;
    let tree_oid = if conflicted {
        None
    } else {
        let tree = runner.run(&["write-tree"], None, 1024).map_err(from_git)?;
        require_success(&tree, "Git index tree query failed")?;
        Some(parse_oid(&tree.stdout, "Git index tree")?)
    };
    Ok(IndexState {
        tree_oid,
        state_hash: ContentHash::for_bytes(&entries.stdout),
    })
}

fn visible_paths<'a, I>(paths: I) -> Vec<String>
where
    I: IntoIterator<Item = &'a String>,
{
    let mut visible = paths
        .into_iter()
        .filter(|path| !is_sensitive_path(Path::new(path.as_str())))
        .cloned()
        .collect::<Vec<_>>();
    visible.sort();
    visible.dedup();
    visible.truncate(MAX_STATUS_PATHS);
    visible
}

fn inspect_hazards(
    runner: &GitRunner,
    common_directory: &Path,
    branch: Option<&str>,
) -> Result<GitWorkflowHazards, GitWorkflowError> {
    let hooks_directory = common_directory.join("hooks");
    let mut active_hooks = Vec::new();
    for name in KNOWN_WORKFLOW_HOOKS {
        let path = hooks_directory.join(name);
        match fs::symlink_metadata(&path) {
            Ok(metadata) if metadata.file_type().is_symlink() => active_hooks.push((*name).into()),
            Ok(metadata) if metadata.is_file() && hook_is_executable(&metadata) => {
                active_hooks.push((*name).into());
            }
            Ok(_) => {}
            Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => {}
            Err(_) => return Err(error("Git hook metadata is unavailable")),
        }
    }
    let custom_hooks_path = config_present(
        runner,
        &["config", "--local", "--get", "core.hooksPath"],
        "Git hooksPath query failed",
    )?;
    let custom_merge_driver = config_present(
        runner,
        &["config", "--get-regexp", r"^merge\..*\.driver$"],
        "Git merge-driver configuration query failed",
    )?;
    let custom_filter_driver = config_present(
        runner,
        &[
            "config",
            "--get-regexp",
            r"^filter\..*\.(clean|smudge|process)$",
        ],
        "Git filter-driver configuration query failed",
    )?;
    Ok(GitWorkflowHazards {
        active_hooks,
        custom_hooks_path,
        custom_merge_driver,
        custom_filter_driver,
        protected_branch: branch.is_some_and(is_protected_branch),
    })
}

fn config_present(
    runner: &GitRunner,
    args: &[&str],
    message: &str,
) -> Result<bool, GitWorkflowError> {
    let output = runner.run(args, None, 64 * 1024).map_err(from_git)?;
    if output.success {
        Ok(!output.stdout.iter().all(u8::is_ascii_whitespace))
    } else if output.code == Some(1) {
        Ok(false)
    } else {
        Err(error(message))
    }
}

fn is_protected_branch(branch: &str) -> bool {
    matches!(branch, "main" | "master" | "develop" | "production") || branch.starts_with("release/")
}

#[cfg(unix)]
fn hook_is_executable(metadata: &fs::Metadata) -> bool {
    use std::os::unix::fs::PermissionsExt;
    metadata.permissions().mode() & 0o111 != 0
}

#[cfg(not(unix))]
fn hook_is_executable(_metadata: &fs::Metadata) -> bool {
    true
}

fn request_operation_kind(
    request: &GitWorkflowRequest,
    active_record: Option<&GitWorkflowRecord>,
) -> Result<String, GitWorkflowError> {
    let kind = match request {
        GitWorkflowRequest::StashCapture { .. } => "stash",
        GitWorkflowRequest::Merge { .. } => "merge",
        GitWorkflowRequest::Rebase { .. } => "rebase",
        GitWorkflowRequest::CherryPick { .. } => "cherry-pick",
        GitWorkflowRequest::Abort { .. } | GitWorkflowRequest::Continue { .. } => {
            return active_record
                .map(|record| record.operation_kind.clone())
                .ok_or_else(|| error("Git workflow recovery requires an active record"));
        }
    };
    Ok(kind.into())
}

fn validate_targets(
    runner: &GitRunner,
    request: &GitWorkflowRequest,
) -> Result<Vec<String>, GitWorkflowError> {
    let targets = match request {
        GitWorkflowRequest::Merge { target_oid, .. } => vec![target_oid.clone()],
        GitWorkflowRequest::Rebase {
            upstream_oid,
            onto_oid,
            ..
        } => vec![upstream_oid.clone(), onto_oid.clone()],
        GitWorkflowRequest::CherryPick { commit_oid, .. } => vec![commit_oid.clone()],
        GitWorkflowRequest::StashCapture { .. }
        | GitWorkflowRequest::Abort { .. }
        | GitWorkflowRequest::Continue { .. } => Vec::new(),
    };
    for oid in &targets {
        let expression = format!("{oid}^{{commit}}");
        let output = runner
            .run(&["cat-file", "-e", &expression], None, 1024)
            .map_err(from_git)?;
        require_success(&output, "Git workflow target is not a commit")?;
    }
    Ok(targets)
}

fn inspect_execution_shape(
    runner: &GitRunner,
    request: &GitWorkflowRequest,
    expected_head: &str,
    active_record: Option<&GitWorkflowRecord>,
    blocking_reasons: &mut Vec<String>,
) -> Result<(String, Option<String>), GitWorkflowError> {
    match request {
        GitWorkflowRequest::StashCapture { .. } => Ok((
            "stash-capture".into(),
            optional_ref_oid(runner, "refs/stash")?,
        )),
        GitWorkflowRequest::Merge {
            target_oid,
            mode,
            commit,
            ..
        } => {
            let target_contained = is_ancestor(runner, target_oid, expected_head)?;
            let fast_forward = is_ancestor(runner, expected_head, target_oid)?;
            if target_contained {
                blocking_reasons.push("merge-target-already-contained".into());
            }
            let requires_commit =
                !target_contained && (!fast_forward || matches!(mode, GitMergeMode::NoFastForward));
            if matches!(mode, GitMergeMode::FastForwardOnly) && !fast_forward {
                blocking_reasons.push("merge-fast-forward-not-possible".into());
            }
            if requires_commit && commit.is_none() {
                blocking_reasons.push("merge-commit-metadata-required".into());
            }
            if !requires_commit && commit.is_some() {
                blocking_reasons.push("merge-commit-metadata-not-applicable".into());
            }
            Ok((
                if requires_commit {
                    "merge-commit"
                } else {
                    "fast-forward"
                }
                .into(),
                None,
            ))
        }
        GitWorkflowRequest::Rebase { .. } => Ok(("history-rewrite".into(), None)),
        GitWorkflowRequest::CherryPick { commit_oid, .. } => {
            if commit_parent_count(runner, commit_oid)? > 1 {
                blocking_reasons.push("cherry-pick-merge-commit-requires-mainline".into());
            }
            Ok(("commit-replay".into(), None))
        }
        GitWorkflowRequest::Abort { .. } => {
            let record = active_record
                .ok_or_else(|| error("Git workflow recovery requires an active record"))?;
            Ok((
                format!("abort-{}", record.operation_kind),
                record.base_stash_oid.clone(),
            ))
        }
        GitWorkflowRequest::Continue { .. } => {
            let record = active_record
                .ok_or_else(|| error("Git workflow recovery requires an active record"))?;
            Ok((
                format!("continue-{}", record.operation_kind),
                record.base_stash_oid.clone(),
            ))
        }
    }
}

fn optional_ref_oid(
    runner: &GitRunner,
    reference: &str,
) -> Result<Option<String>, GitWorkflowError> {
    let output = runner
        .run(&["rev-parse", "-q", "--verify", reference], None, 1024)
        .map_err(from_git)?;
    if output.success {
        Ok(Some(parse_oid(&output.stdout, "Git optional ref")?))
    } else if output.code == Some(1) {
        Ok(None)
    } else {
        Err(error("Git optional ref query failed"))
    }
}

fn is_ancestor(
    runner: &GitRunner,
    ancestor: &str,
    descendant: &str,
) -> Result<bool, GitWorkflowError> {
    let output = runner
        .run(
            &["merge-base", "--is-ancestor", ancestor, descendant],
            None,
            1024,
        )
        .map_err(from_git)?;
    if output.success {
        Ok(true)
    } else if output.code == Some(1) {
        Ok(false)
    } else {
        Err(error("Git ancestry query failed"))
    }
}

fn commit_parent_count(runner: &GitRunner, oid: &str) -> Result<usize, GitWorkflowError> {
    let output = runner
        .run(&["rev-list", "--parents", "-n", "1", oid], None, 16 * 1024)
        .map_err(from_git)?;
    require_success(&output, "Git commit parent query failed")?;
    let text = std::str::from_utf8(&output.stdout)
        .map_err(|_| error("Git commit parent query is not UTF-8"))?;
    let fields = text.split_ascii_whitespace().collect::<Vec<_>>();
    if fields.is_empty() || fields.iter().any(|oid| !valid_oid(oid)) {
        return Err(error("Git commit parent query is invalid"));
    }
    Ok(fields.len() - 1)
}

fn block_execution_policy(
    request: &GitWorkflowRequest,
    active_record: Option<&GitWorkflowRecord>,
    blocking_reasons: &mut Vec<String>,
) -> Result<(), GitWorkflowError> {
    let policy_request = match request {
        GitWorkflowRequest::Abort { .. } => return Ok(()),
        GitWorkflowRequest::Continue { .. } => {
            &active_record
                .ok_or_else(|| error("Git workflow recovery requires an active record"))?
                .request
        }
        _ => request,
    };
    let (hook_policy, signing_policy) = match policy_request {
        GitWorkflowRequest::StashCapture {
            hook_policy,
            signing_policy,
            ..
        }
        | GitWorkflowRequest::Merge {
            hook_policy,
            signing_policy,
            ..
        }
        | GitWorkflowRequest::Rebase {
            hook_policy,
            signing_policy,
            ..
        }
        | GitWorkflowRequest::CherryPick {
            hook_policy,
            signing_policy,
            ..
        } => (*hook_policy, *signing_policy),
        GitWorkflowRequest::Abort { .. } | GitWorkflowRequest::Continue { .. } => {
            return Err(error("persisted Git workflow request is invalid"));
        }
    };
    if matches!(hook_policy, GitCommitHookPolicy::Run) {
        blocking_reasons.push("hooks-require-sandbox-permission-approval".into());
    }
    if matches!(signing_policy, GitCommitSigningPolicy::Sign) {
        blocking_reasons.push("signing-requires-secure-signer-approval".into());
    }
    Ok(())
}

fn classify_risk(request: &GitWorkflowRequest, hazards: &GitWorkflowHazards) -> GitWorkflowRisk {
    let (class, mut reasons, requires_explicit_approval) = match request {
        GitWorkflowRequest::StashCapture {
            include_untracked, ..
        } => {
            let mut reasons = vec!["temporarily-rewrites-worktree-and-index".into()];
            if *include_untracked {
                reasons.push("captures-untracked-files".into());
            }
            ("medium", reasons, true)
        }
        GitWorkflowRequest::Merge { .. } => (
            "medium",
            vec!["combines-history-and-updates-worktree".into()],
            true,
        ),
        GitWorkflowRequest::CherryPick { .. } => (
            "medium",
            vec!["replays-commit-and-updates-worktree".into()],
            true,
        ),
        GitWorkflowRequest::Continue { .. } => (
            "medium",
            vec!["continues-partially-completed-history-operation".into()],
            true,
        ),
        GitWorkflowRequest::Rebase { .. } => ("high", vec!["rewrites-commit-history".into()], true),
        GitWorkflowRequest::Abort { .. } => (
            "high",
            vec!["may-discard-conflict-resolutions".into()],
            true,
        ),
    };
    if !hazards.active_hooks.is_empty() || hazards.custom_hooks_path {
        reasons.push("repository-hooks-require-reviewed-policy".into());
    }
    if hazards.custom_merge_driver {
        reasons.push("custom-merge-driver-configured".into());
    }
    if hazards.custom_filter_driver {
        reasons.push("custom-filter-driver-configured".into());
    }
    reasons.sort();
    reasons.dedup();
    GitWorkflowRisk {
        class: class.into(),
        reasons,
        requires_permission: true,
        requires_explicit_approval,
    }
}

fn block_start_state(
    snapshot: &crate::git_status::GitStatusSnapshot,
    pending_editor_paths: &HashSet<String>,
    hazards: &GitWorkflowHazards,
    allow_dirty: bool,
    blocking_reasons: &mut Vec<String>,
) {
    if !allow_dirty && !snapshot.entries.is_empty() {
        blocking_reasons.push("dirty-worktree-or-index".into());
    }
    if !snapshot.conflicts.is_empty() {
        blocking_reasons.push("unresolved-conflicts".into());
    }
    if snapshot.operation_in_progress.is_some() {
        blocking_reasons.push("git-operation-already-in-progress".into());
    }
    if !pending_editor_paths.is_empty() {
        blocking_reasons.push("pending-editor-edits".into());
    }
    if hazards.custom_filter_driver {
        blocking_reasons.push("custom-filter-driver-not-authorized".into());
    }
    if !allow_dirty && hazards.custom_merge_driver {
        blocking_reasons.push("custom-merge-driver-not-authorized".into());
    }
}

fn validate_active_record<'a>(
    expected_root_identity: &str,
    operation_id: &str,
    generation: u64,
    active_record: Option<&'a GitWorkflowRecord>,
) -> Result<&'a GitWorkflowRecord, GitWorkflowError> {
    let record =
        active_record.ok_or_else(|| error("Git workflow recovery requires an active record"))?;
    validate_record(record)?;
    if record.operation_id != operation_id
        || record.generation != generation
        || record.root_identity != expected_root_identity
    {
        return Err(error("Git workflow recovery record binding is stale"));
    }
    if !matches!(record.state.as_str(), "in-progress" | "conflicted") {
        return Err(error("Git workflow record is not recoverable"));
    }
    Ok(record)
}

fn validate_identifier(value: &str, label: &str) -> Result<(), GitWorkflowError> {
    if value.is_empty()
        || value.len() > 128
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
    {
        return Err(error(format!("{label} is invalid")));
    }
    Ok(())
}

fn validate_plan_binding(
    store: &GitWorkflowStore,
    plan: &GitWorkflowPlan,
) -> Result<(), GitWorkflowError> {
    if plan.schema_version != PLAN_SCHEMA_VERSION
        || plan.repository_root != path_to_utf8(&store.repository_root, "Git repository root")?
        || plan.root_identity != store.root_identity
        || matches!(
            plan.request,
            GitWorkflowRequest::Abort { .. } | GitWorkflowRequest::Continue { .. }
        )
    {
        return Err(error("Git workflow plan binding is invalid"));
    }
    let current = plan_git_workflow(
        &store.repository_root,
        plan.request.clone(),
        &HashSet::new(),
        None,
    )?;
    if current != *plan {
        return Err(error("Git workflow plan is stale or was modified"));
    }
    Ok(())
}

pub(crate) fn validate_record(record: &GitWorkflowRecord) -> Result<(), GitWorkflowError> {
    validate_identifier(&record.operation_id, "operation ID")?;
    validate_identifier(&record.project_id, "project ID")?;
    validate_identifier(&record.session_id, "session ID")?;
    validate_request(&record.request)?;
    validate_risk(&record.risk)?;
    validate_oid(&record.base_head)?;
    for oid in &record.target_oids {
        validate_oid(oid)?;
    }
    let (expected_kind, expected_targets) = match &record.request {
        GitWorkflowRequest::StashCapture { .. } => ("stash", Vec::new()),
        GitWorkflowRequest::Merge { target_oid, .. } => ("merge", vec![target_oid.clone()]),
        GitWorkflowRequest::Rebase {
            upstream_oid,
            onto_oid,
            ..
        } => ("rebase", vec![upstream_oid.clone(), onto_oid.clone()]),
        GitWorkflowRequest::CherryPick { commit_oid, .. } => {
            ("cherry-pick", vec![commit_oid.clone()])
        }
        GitWorkflowRequest::Abort { .. } | GitWorkflowRequest::Continue { .. } => {
            return Err(error("persisted Git workflow request is invalid"));
        }
    };
    if record.schema_version != RECORD_SCHEMA_VERSION
        || !Path::new(&record.repository_root).is_absolute()
        || !valid_root_identity(&record.root_identity)
        || !valid_root_identity(&record.git_common_directory_identity)
        || record.operation_kind != expected_kind
        || record.target_oids != expected_targets
        || !valid_record_execution_shape(record)
        || !matches!(
            record.state.as_str(),
            "planned"
                | "in-progress"
                | "conflicted"
                | "blocked-foreign-operation"
                | "interrupted-needs-reconciliation"
                | "execution-prepared"
                | "execution-dispatching"
                | "failed"
                | "failed-needs-reconciliation"
                | "completed"
                | "aborted"
                | "cancelled"
        )
        || record.visible_dirty_paths.len() > MAX_STATUS_PATHS
        || record.conflicts.len() > MAX_CONFLICT_PATHS
        || record.updated_at_ms < record.created_at_ms
        || !valid_optional_branch(record.base_branch.as_deref())
        || !valid_optional_branch(record.observed_branch.as_deref())
    {
        return Err(error("Git workflow record is invalid"));
    }
    validate_hash(&record.base_index_state)?;
    if record.base_index_state.bytes > MAX_GIT_OUTPUT {
        return Err(error("Git workflow index state exceeds size limit"));
    }
    validate_oid(&record.observed_head)?;
    if let Some(tree) = &record.base_index_tree {
        validate_oid(tree)?;
    }
    validate_record_paths(&record.visible_dirty_paths)?;
    validate_execution_attempt(record)?;
    let expected_actions: &[&str] = match record.state.as_str() {
        "planned" => &["start", "cancel"],
        "conflicted" => &["resolve", "abort"],
        "in-progress" => &["continue", "abort"],
        "blocked-foreign-operation" | "interrupted-needs-reconciliation" => &["inspect"],
        "failed" => &["start", "inspect"],
        "failed-needs-reconciliation" => &["inspect"],
        _ => &[],
    };
    if record.allowed_actions
        != expected_actions
            .iter()
            .map(|action| (*action).to_owned())
            .collect::<Vec<_>>()
    {
        return Err(error("Git workflow record actions are inconsistent"));
    }
    let mut previous_conflict: Option<&str> = None;
    for conflict in &record.conflicts {
        validate_relative_path(&conflict.path, "Git conflict path")?;
        if is_sensitive_path(Path::new(&conflict.path))
            || conflict.stages.is_empty()
            || conflict.stages.len() > 3
            || previous_conflict.is_some_and(|path| path >= conflict.path.as_str())
        {
            return Err(error("Git workflow conflict evidence is invalid"));
        }
        previous_conflict = Some(&conflict.path);
        let mut previous = 0_u8;
        for stage in &conflict.stages {
            if !(1..=3).contains(&stage.stage)
                || stage.stage <= previous
                || !valid_mode(&stage.mode)
                || !valid_oid(&stage.oid)
            {
                return Err(error("Git workflow conflict stage is invalid"));
            }
            previous = stage.stage;
        }
    }
    Ok(())
}

fn validate_execution_attempt(record: &GitWorkflowRecord) -> Result<(), GitWorkflowError> {
    let executing = matches!(
        record.state.as_str(),
        "execution-prepared" | "execution-dispatching"
    );
    let Some(attempt) = &record.execution else {
        if executing {
            return Err(error(
                "executing Git workflow record has no journal attempt",
            ));
        }
        return Ok(());
    };
    validate_identifier(&attempt.authorization_id, "authorization ID")?;
    validate_hash(&attempt.requirement_hash)?;
    if attempt.schema_version != "git-workflow-execution-attempt/0.1"
        || !matches!(attempt.action.as_str(), "start" | "abort" | "continue")
        || !matches!(
            attempt.phase.as_str(),
            "prepared" | "dispatching" | "observed" | "recovered"
        )
        || attempt.source_generation >= record.generation
        || attempt.updated_at_ms < attempt.started_at_ms
        || attempt.updated_at_ms > record.updated_at_ms
        || (executing && !matches!(attempt.phase.as_str(), "prepared" | "dispatching"))
        || (!executing && matches!(attempt.phase.as_str(), "prepared" | "dispatching"))
    {
        return Err(error("Git workflow execution journal is invalid"));
    }
    Ok(())
}

fn valid_record_execution_shape(record: &GitWorkflowRecord) -> bool {
    match &record.request {
        GitWorkflowRequest::StashCapture { .. } => {
            record.risk.class == "medium"
                && record.predicted_behavior == "stash-capture"
                && record
                    .base_stash_oid
                    .as_ref()
                    .is_none_or(|oid| valid_oid(oid))
        }
        GitWorkflowRequest::Merge { mode, commit, .. } => {
            record.risk.class == "medium"
                && record.base_stash_oid.is_none()
                && match record.predicted_behavior.as_str() {
                    "fast-forward" => {
                        !matches!(mode, GitMergeMode::NoFastForward) && commit.is_none()
                    }
                    "merge-commit" => {
                        !matches!(mode, GitMergeMode::FastForwardOnly) && commit.is_some()
                    }
                    _ => false,
                }
        }
        GitWorkflowRequest::Rebase { .. } => {
            record.risk.class == "high"
                && record.predicted_behavior == "history-rewrite"
                && record.base_stash_oid.is_none()
        }
        GitWorkflowRequest::CherryPick { .. } => {
            record.risk.class == "medium"
                && record.predicted_behavior == "commit-replay"
                && record.base_stash_oid.is_none()
        }
        GitWorkflowRequest::Abort { .. } | GitWorkflowRequest::Continue { .. } => false,
    }
}

fn validate_risk(risk: &GitWorkflowRisk) -> Result<(), GitWorkflowError> {
    if !matches!(risk.class.as_str(), "medium" | "high")
        || !risk.requires_permission
        || !risk.requires_explicit_approval
        || risk.reasons.is_empty()
        || risk.reasons.len() > 32
        || risk.reasons.iter().any(|reason| {
            reason.is_empty()
                || reason.len() > 256
                || !reason
                    .bytes()
                    .all(|byte| byte.is_ascii_lowercase() || matches!(byte, b'-' | b'0'..=b'9'))
        })
    {
        return Err(error("Git workflow risk classification is invalid"));
    }
    Ok(())
}

fn valid_root_identity(identity: &str) -> bool {
    identity
        .strip_prefix("git-root:sha256:")
        .is_some_and(|digest| {
            digest.len() == 64
                && digest
                    .bytes()
                    .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
        })
}

fn valid_optional_branch(branch: Option<&str>) -> bool {
    branch.is_none_or(|value| {
        !value.is_empty()
            && value.len() <= 1024
            && !value.bytes().any(|byte| byte.is_ascii_control())
    })
}

fn validate_hash(hash: &ContentHash) -> Result<(), GitWorkflowError> {
    if hash.sha256.len() != 64
        || !hash
            .sha256
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
        return Err(error("Git workflow content hash is invalid"));
    }
    Ok(())
}

fn validate_record_paths(paths: &[String]) -> Result<(), GitWorkflowError> {
    let mut previous: Option<&str> = None;
    for path in paths {
        validate_relative_path(path, "Git workflow path")?;
        if is_sensitive_path(Path::new(path))
            || previous.is_some_and(|value| value >= path.as_str())
        {
            return Err(error("Git workflow visible path set is invalid"));
        }
        previous = Some(path);
    }
    Ok(())
}

fn validate_relative_path(path: &str, label: &str) -> Result<(), GitWorkflowError> {
    if path.is_empty()
        || path.len() > 4 * 1024
        || Path::new(path).is_absolute()
        || Path::new(path)
            .components()
            .any(|component| !matches!(component, Component::Normal(_)))
    {
        return Err(error(format!("{label} is invalid")));
    }
    Ok(())
}

fn conflict_evidence(
    repository_root: &Path,
) -> Result<(Vec<GitWorkflowConflict>, usize), GitWorkflowError> {
    let runner = GitRunner::new(repository_root).map_err(from_git)?;
    let output = runner
        .run(
            &["ls-files", "--unmerged", "--stage", "-z"],
            None,
            MAX_GIT_OUTPUT,
        )
        .map_err(from_git)?;
    require_success(&output, "Git conflict index query failed")?;
    let mut grouped: BTreeMap<String, Vec<GitWorkflowConflictStage>> = BTreeMap::new();
    for record in output.stdout.split(|byte| *byte == 0) {
        if record.is_empty() {
            continue;
        }
        let tab = record
            .iter()
            .position(|byte| *byte == b'\t')
            .ok_or_else(|| error("Git conflict index record is malformed"))?;
        let header = std::str::from_utf8(&record[..tab])
            .map_err(|_| error("Git conflict index header is not UTF-8"))?;
        let path = std::str::from_utf8(&record[tab + 1..])
            .map_err(|_| error("Git conflict path is not UTF-8"))?
            .to_owned();
        validate_relative_path(&path, "Git conflict path")?;
        let fields = header.split_ascii_whitespace().collect::<Vec<_>>();
        if fields.len() != 3 || !valid_mode(fields[0]) || !valid_oid(fields[1]) {
            return Err(error("Git conflict index header is invalid"));
        }
        let stage = fields[2]
            .parse::<u8>()
            .map_err(|_| error("Git conflict index stage is invalid"))?;
        if !(1..=3).contains(&stage) {
            return Err(error("Git conflict index stage is invalid"));
        }
        grouped
            .entry(path)
            .or_default()
            .push(GitWorkflowConflictStage {
                stage,
                mode: fields[0].into(),
                oid: fields[1].into(),
            });
    }
    let redacted = grouped
        .keys()
        .filter(|path| is_sensitive_path(Path::new(path.as_str())))
        .count();
    let mut conflicts = Vec::new();
    for (path, mut stages) in grouped {
        if is_sensitive_path(Path::new(&path)) {
            continue;
        }
        if conflicts.len() >= MAX_CONFLICT_PATHS {
            return Err(error("Git conflict path limit exceeded"));
        }
        stages.sort_by_key(|stage| stage.stage);
        if stages.windows(2).any(|pair| pair[0].stage == pair[1].stage) {
            return Err(error("Git conflict index contains duplicate stages"));
        }
        conflicts.push(GitWorkflowConflict { path, stages });
    }
    Ok((conflicts, redacted))
}

fn valid_mode(mode: &str) -> bool {
    mode.len() == 6 && mode.bytes().all(|byte| matches!(byte, b'0'..=b'7'))
}

fn envelope_bytes(record: &GitWorkflowRecord) -> Result<Vec<u8>, GitWorkflowError> {
    validate_record(record)?;
    let payload =
        serde_json::to_vec(record).map_err(|_| error("cannot serialize Git workflow record"))?;
    let envelope = RecordEnvelope {
        schema_version: ENVELOPE_SCHEMA_VERSION.into(),
        payload_hash: ContentHash::for_bytes(&payload),
        payload: record.clone(),
    };
    let bytes = serde_json::to_vec_pretty(&envelope)
        .map_err(|_| error("cannot serialize Git workflow envelope"))?;
    if bytes.len() as u64 > MAX_RECORD_BYTES {
        return Err(error("Git workflow record exceeds size limit"));
    }
    Ok(bytes)
}

fn read_bounded(path: &Path, limit: u64, label: &str) -> Result<Vec<u8>, GitWorkflowError> {
    let metadata =
        fs::symlink_metadata(path).map_err(|_| error(format!("{label} is unavailable")))?;
    if metadata.file_type().is_symlink() || !metadata.is_file() || metadata.len() > limit {
        return Err(error(format!("{label} is not a bounded regular file")));
    }
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    File::open(path)
        .and_then(|file| file.take(limit + 1).read_to_end(&mut bytes))
        .map_err(|_| error(format!("cannot read {label}")))?;
    if bytes.len() as u64 > limit {
        return Err(error(format!("{label} exceeds size limit")));
    }
    Ok(bytes)
}

fn ensure_private_directory(path: &Path) -> Result<(), GitWorkflowError> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() || !metadata.is_dir() => {
            return Err(error("Git workflow storage directory is unsafe"));
        }
        Ok(_) => {}
        Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => {
            fs::create_dir(path)
                .map_err(|_| error("cannot create Git workflow storage directory"))?;
        }
        Err(_) => return Err(error("Git workflow storage directory is unavailable")),
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(path, fs::Permissions::from_mode(0o700))
            .map_err(|_| error("cannot secure Git workflow storage directory"))?;
    }
    Ok(())
}

fn write_private_file(path: &Path, bytes: &[u8]) -> Result<(), GitWorkflowError> {
    let mut options = OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    let mut file = options
        .open(path)
        .map_err(|_| error("cannot create Git workflow record"))?;
    file.write_all(bytes)
        .map_err(|_| error("cannot write Git workflow record"))?;
    file.flush()
        .map_err(|_| error("cannot flush Git workflow record"))?;
    file.sync_all()
        .map_err(|_| error("cannot sync Git workflow record"))
}

fn open_store_lock(path: &Path) -> Result<File, GitWorkflowError> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() || !metadata.is_file() => {
            return Err(error("Git workflow record lock is unsafe"));
        }
        Ok(_) => {}
        Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => {
            if let Err(create_cause) = write_private_file(path, b"") {
                if fs::symlink_metadata(path).is_err() {
                    return Err(create_cause);
                }
            }
        }
        Err(_) => return Err(error("Git workflow record lock is unavailable")),
    }
    let metadata =
        fs::symlink_metadata(path).map_err(|_| error("Git workflow record lock is unavailable"))?;
    if metadata.file_type().is_symlink() || !metadata.is_file() {
        return Err(error("Git workflow record lock is unsafe"));
    }
    OpenOptions::new()
        .read(true)
        .write(true)
        .open(path)
        .map_err(|_| error("cannot open Git workflow record lock"))
}

fn sync_directory(path: &Path) -> Result<(), GitWorkflowError> {
    #[cfg(unix)]
    File::open(path)
        .and_then(|directory| directory.sync_all())
        .map_err(|_| error("cannot sync Git workflow storage directory"))?;
    Ok(())
}

#[cfg(not(windows))]
fn atomic_replace(source: &Path, target: &Path) -> std::io::Result<()> {
    fs::rename(source, target)
}

#[cfg(windows)]
fn atomic_replace(source: &Path, target: &Path) -> std::io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Storage::FileSystem::{
        MoveFileExW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH,
    };
    let source = source
        .as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect::<Vec<_>>();
    let target = target
        .as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect::<Vec<_>>();
    let result = unsafe {
        MoveFileExW(
            source.as_ptr(),
            target.as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    };
    if result == 0 {
        Err(std::io::Error::last_os_error())
    } else {
        Ok(())
    }
}

fn now_ms() -> Result<u64, GitWorkflowError> {
    let milliseconds = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| error("system clock is before the Unix epoch"))?
        .as_millis();
    u64::try_from(milliseconds).map_err(|_| error("system clock is out of range"))
}

fn parse_oid(bytes: &[u8], label: &str) -> Result<String, GitWorkflowError> {
    let oid = std::str::from_utf8(bytes)
        .map_err(|_| error(format!("{label} is not UTF-8")))?
        .trim();
    validate_oid(oid)?;
    Ok(oid.into())
}

fn require_success(output: &GitOutput, message: &str) -> Result<(), GitWorkflowError> {
    if output.success {
        Ok(())
    } else {
        Err(error(message))
    }
}

fn from_git(cause: GitStatusError) -> GitWorkflowError {
    error(cause.message)
}

fn error(message: impl Into<String>) -> GitWorkflowError {
    GitWorkflowError {
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;
    use std::process::Command;

    struct TestRepository {
        parent: PathBuf,
        root: PathBuf,
        storage: PathBuf,
    }

    impl TestRepository {
        fn new(label: &str) -> Self {
            let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
            let parent = std::env::temp_dir().join(format!(
                "aegisy-git-workflow-{label}-{}-{sequence}",
                std::process::id()
            ));
            let root = parent.join("repository");
            let storage = parent.join("storage");
            fs::create_dir_all(&root).unwrap();
            fs::create_dir_all(&storage).unwrap();
            git(&root, &["init"], true);
            git(&root, &["branch", "-M", "main"], true);
            git(&root, &["config", "user.name", "Aegisy Test"], true);
            git(
                &root,
                &["config", "user.email", "aegisy@example.invalid"],
                true,
            );
            write_and_commit(&root, "base.txt", "base\n", "base");
            Self {
                parent,
                root: root.canonicalize().unwrap(),
                storage: storage.canonicalize().unwrap(),
            }
        }

        fn store(&self) -> GitWorkflowStore {
            GitWorkflowStore::open(&self.storage, &self.root).unwrap()
        }
    }

    impl Drop for TestRepository {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.parent);
        }
    }

    fn git(root: &Path, args: &[&str], success: bool) -> Vec<u8> {
        let output = Command::new("git")
            .arg("-C")
            .arg(root)
            .args(args)
            .env("LC_ALL", "C")
            .output()
            .unwrap();
        assert_eq!(
            output.status.success(),
            success,
            "git {args:?}: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        output.stdout
    }

    fn write_and_commit(root: &Path, path: &str, content: &str, message: &str) -> String {
        let target = root.join(path);
        if let Some(parent) = target.parent() {
            fs::create_dir_all(parent).unwrap();
        }
        fs::write(target, content).unwrap();
        git(root, &["add", "--", path], true);
        git(root, &["commit", "-m", message], true);
        head(root)
    }

    fn head(root: &Path) -> String {
        String::from_utf8(git(root, &["rev-parse", "HEAD"], true))
            .unwrap()
            .trim()
            .into()
    }

    fn divergent_target(repository: &TestRepository, path: &str) -> String {
        git(&repository.root, &["switch", "-c", "feature"], true);
        let target = write_and_commit(&repository.root, path, "feature\n", "feature");
        git(&repository.root, &["switch", "main"], true);
        target
    }

    fn identity() -> GitCommitIdentity {
        GitCommitIdentity {
            name: "Aegisy Test".into(),
            email: "aegisy@example.invalid".into(),
            source: "explicit".into(),
            timestamp_seconds: 1_800_000_000,
            timezone: "+0800".into(),
        }
    }

    fn commit_metadata(message: &str) -> GitWorkflowCommitMetadata {
        GitWorkflowCommitMetadata {
            message: message.into(),
            message_source: GitCommitMessageSource::User,
            identity: identity(),
        }
    }

    fn merge_request(target_oid: &str) -> GitWorkflowRequest {
        GitWorkflowRequest::Merge {
            target_oid: target_oid.into(),
            mode: GitMergeMode::NoFastForward,
            commit: Some(commit_metadata("Merge reviewed target")),
            hook_policy: GitCommitHookPolicy::Disabled,
            signing_policy: GitCommitSigningPolicy::Unsigned,
        }
    }

    fn merge_plan(repository: &TestRepository, target_oid: &str) -> GitWorkflowPlan {
        plan_git_workflow(
            &repository.root,
            merge_request(target_oid),
            &HashSet::new(),
            None,
        )
        .unwrap()
    }

    #[test]
    fn plans_supported_start_operations_with_exact_targets_and_risk() {
        let repository = TestRepository::new("plans");
        let target = divergent_target(&repository, "feature.txt");

        let merge = merge_plan(&repository, &target);
        assert_eq!(merge.target_oids, vec![target.clone()]);
        assert_eq!(merge.operation_kind, "merge");
        assert_eq!(merge.risk.class, "medium");
        assert!(merge.risk.requires_permission);
        assert!(merge.risk.requires_explicit_approval);
        assert!(merge.blocking_reasons.is_empty());

        let cherry_pick = plan_git_workflow(
            &repository.root,
            GitWorkflowRequest::CherryPick {
                commit_oid: target.clone(),
                committer: identity(),
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
            &HashSet::new(),
            None,
        )
        .unwrap();
        assert_eq!(cherry_pick.target_oids, vec![target.clone()]);
        assert_eq!(cherry_pick.operation_kind, "cherry-pick");
        assert_eq!(cherry_pick.risk.class, "medium");

        git(&repository.root, &["switch", "-c", "topic"], true);
        let current = head(&repository.root);
        let rebase = plan_git_workflow(
            &repository.root,
            GitWorkflowRequest::Rebase {
                upstream_oid: current.clone(),
                onto_oid: target.clone(),
                committer: identity(),
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
            &HashSet::new(),
            None,
        )
        .unwrap();
        assert_eq!(rebase.target_oids, vec![current, target]);
        assert_eq!(rebase.operation_kind, "rebase");
        assert_eq!(rebase.risk.class, "high");
        assert!(rebase.risk.requires_explicit_approval);
        assert!(rebase.blocking_reasons.is_empty());

        fs::write(repository.root.join("stash.txt"), "dirty\n").unwrap();
        let stash = plan_git_workflow(
            &repository.root,
            GitWorkflowRequest::StashCapture {
                include_untracked: true,
                message: "Save reviewed work".into(),
                message_source: GitCommitMessageSource::User,
                identity: identity(),
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
            &HashSet::new(),
            None,
        )
        .unwrap();
        assert_eq!(stash.operation_kind, "stash");
        assert!(stash.target_oids.is_empty());
        assert_eq!(stash.visible_dirty_paths, vec!["stash.txt"]);
        assert!(stash
            .risk
            .reasons
            .contains(&"captures-untracked-files".into()));
        assert!(stash.blocking_reasons.is_empty());
    }

    #[test]
    fn execution_parameters_are_single_meaning_and_policy_gated() {
        let repository = TestRepository::new("execution-policy");
        let target = divergent_target(&repository, "feature.txt");
        let fast_forward = plan_git_workflow(
            &repository.root,
            GitWorkflowRequest::Merge {
                target_oid: target.clone(),
                mode: GitMergeMode::AllowFastForward,
                commit: None,
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
            &HashSet::new(),
            None,
        )
        .unwrap();
        assert_eq!(fast_forward.predicted_behavior, "fast-forward");
        assert!(fast_forward.blocking_reasons.is_empty());

        let irrelevant_metadata = plan_git_workflow(
            &repository.root,
            GitWorkflowRequest::Merge {
                target_oid: target.clone(),
                mode: GitMergeMode::AllowFastForward,
                commit: Some(commit_metadata("Should not be used")),
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
            &HashSet::new(),
            None,
        )
        .unwrap();
        assert!(irrelevant_metadata
            .blocking_reasons
            .contains(&"merge-commit-metadata-not-applicable".into()));

        let missing_metadata = plan_git_workflow(
            &repository.root,
            GitWorkflowRequest::Merge {
                target_oid: target.clone(),
                mode: GitMergeMode::NoFastForward,
                commit: None,
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
            &HashSet::new(),
            None,
        )
        .unwrap();
        assert_eq!(missing_metadata.predicted_behavior, "merge-commit");
        assert!(missing_metadata
            .blocking_reasons
            .contains(&"merge-commit-metadata-required".into()));

        let unsafe_policy = plan_git_workflow(
            &repository.root,
            GitWorkflowRequest::Merge {
                target_oid: target.clone(),
                mode: GitMergeMode::NoFastForward,
                commit: Some(commit_metadata("Reviewed merge")),
                hook_policy: GitCommitHookPolicy::Run,
                signing_policy: GitCommitSigningPolicy::Sign,
            },
            &HashSet::new(),
            None,
        )
        .unwrap();
        assert!(unsafe_policy
            .blocking_reasons
            .contains(&"hooks-require-sandbox-permission-approval".into()));
        assert!(unsafe_policy
            .blocking_reasons
            .contains(&"signing-requires-secure-signer-approval".into()));

        write_and_commit(&repository.root, "main.txt", "main\n", "main diverges");
        let non_fast_forward = plan_git_workflow(
            &repository.root,
            GitWorkflowRequest::Merge {
                target_oid: target,
                mode: GitMergeMode::FastForwardOnly,
                commit: None,
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
            &HashSet::new(),
            None,
        )
        .unwrap();
        assert!(non_fast_forward
            .blocking_reasons
            .contains(&"merge-fast-forward-not-possible".into()));
    }

    #[test]
    fn stash_plan_binds_previous_stash_and_rejects_invalid_metadata() {
        let repository = TestRepository::new("stash-baseline");
        fs::write(repository.root.join("first.txt"), "first\n").unwrap();
        git(
            &repository.root,
            &["stash", "push", "-u", "-m", "existing stash"],
            true,
        );
        let previous = String::from_utf8(git(&repository.root, &["rev-parse", "refs/stash"], true))
            .unwrap()
            .trim()
            .to_owned();
        fs::write(repository.root.join("second.txt"), "second\n").unwrap();
        let plan = plan_git_workflow(
            &repository.root,
            GitWorkflowRequest::StashCapture {
                include_untracked: true,
                message: "Save second change".into(),
                message_source: GitCommitMessageSource::User,
                identity: identity(),
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
            &HashSet::new(),
            None,
        )
        .unwrap();
        assert_eq!(plan.base_stash_oid.as_deref(), Some(previous.as_str()));
        assert_eq!(plan.predicted_behavior, "stash-capture");

        assert!(plan_git_workflow(
            &repository.root,
            GitWorkflowRequest::StashCapture {
                include_untracked: true,
                message: " bad message ".into(),
                message_source: GitCommitMessageSource::User,
                identity: identity(),
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
            &HashSet::new(),
            None,
        )
        .is_err());
    }

    #[test]
    fn blocks_dirty_pending_protected_and_invalid_or_missing_targets() {
        let repository = TestRepository::new("preflight");
        let target = divergent_target(&repository, "feature.txt");
        fs::write(repository.root.join("dirty.txt"), "dirty\n").unwrap();
        let dirty = merge_plan(&repository, &target);
        assert!(dirty
            .blocking_reasons
            .contains(&"dirty-worktree-or-index".into()));

        fs::remove_file(repository.root.join("dirty.txt")).unwrap();
        let mut pending = HashSet::new();
        pending.insert("open.txt".into());
        let pending_plan =
            plan_git_workflow(&repository.root, merge_request(&target), &pending, None).unwrap();
        assert!(pending_plan
            .blocking_reasons
            .contains(&"pending-editor-edits".into()));

        let protected = plan_git_workflow(
            &repository.root,
            GitWorkflowRequest::Rebase {
                upstream_oid: head(&repository.root),
                onto_oid: target,
                committer: identity(),
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
            &HashSet::new(),
            None,
        )
        .unwrap();
        assert!(protected
            .blocking_reasons
            .contains(&"protected-branch-history-rewrite".into()));
        assert!(plan_git_workflow(
            &repository.root,
            merge_request("ABC"),
            &HashSet::new(),
            None,
        )
        .is_err());
        assert!(plan_git_workflow(
            &repository.root,
            merge_request(&"0".repeat(40)),
            &HashSet::new(),
            None,
        )
        .is_err());
    }

    #[test]
    fn persists_reopens_and_rejects_stale_plans() {
        let repository = TestRepository::new("persist");
        let target = divergent_target(&repository, "feature.txt");
        let plan = merge_plan(&repository, &target);
        let expected = repository
            .store()
            .create_planned("operation-1", "project-1", "session-1", &plan)
            .unwrap();
        let reopened = GitWorkflowStore::open(&repository.storage, &repository.root).unwrap();
        assert_eq!(reopened.load("operation-1").unwrap(), expected);

        let stale = merge_plan(&repository, &target);
        write_and_commit(&repository.root, "later.txt", "later\n", "later");
        assert!(reopened
            .create_planned("operation-2", "project-1", "session-1", &stale)
            .is_err());
    }

    #[test]
    fn rejects_tampered_identity_hash_and_symlink_records() {
        let repository = TestRepository::new("tamper");
        let target = divergent_target(&repository, "feature.txt");
        let plan = merge_plan(&repository, &target);
        let store = repository.store();
        store
            .create_planned("payload", "project-1", "session-1", &plan)
            .unwrap();
        let payload_path = store.record_path("payload");
        let mut payload: Value = serde_json::from_slice(
            &read_bounded(&payload_path, MAX_RECORD_BYTES, "test record").unwrap(),
        )
        .unwrap();
        payload["payload"]["project_id"] = Value::String("project-2".into());
        fs::write(&payload_path, serde_json::to_vec(&payload).unwrap()).unwrap();
        assert!(store.load("payload").is_err());

        store
            .create_planned("identity", "project-1", "session-1", &plan)
            .unwrap();
        let identity_path = store.record_path("identity");
        let mut envelope: RecordEnvelope = serde_json::from_slice(
            &read_bounded(&identity_path, MAX_RECORD_BYTES, "test record").unwrap(),
        )
        .unwrap();
        envelope.payload.repository_root = repository.parent.to_string_lossy().into_owned();
        let payload_bytes = serde_json::to_vec(&envelope.payload).unwrap();
        envelope.payload_hash = ContentHash::for_bytes(&payload_bytes);
        fs::write(&identity_path, serde_json::to_vec(&envelope).unwrap()).unwrap();
        assert!(store.load("identity").is_err());

        store
            .create_planned("hash", "project-1", "session-1", &plan)
            .unwrap();
        let hash_path = store.record_path("hash");
        let mut envelope: RecordEnvelope = serde_json::from_slice(
            &read_bounded(&hash_path, MAX_RECORD_BYTES, "test record").unwrap(),
        )
        .unwrap();
        envelope.payload_hash.sha256 = "f".repeat(64);
        fs::write(&hash_path, serde_json::to_vec(&envelope).unwrap()).unwrap();
        assert!(store.load("hash").is_err());

        #[cfg(unix)]
        {
            use std::os::unix::fs::symlink;
            let target = repository.storage.join("symlink-target.json");
            fs::write(&target, b"{}").unwrap();
            symlink(&target, store.record_path("linked")).unwrap();
            assert!(store.load("linked").is_err());
        }
    }

    #[test]
    fn rejects_storage_that_overlaps_repository() {
        let repository = TestRepository::new("overlap");
        assert!(GitWorkflowStore::open(&repository.root, &repository.root).is_err());
        let nested = repository.root.join("trusted");
        fs::create_dir(&nested).unwrap();
        assert!(GitWorkflowStore::open(&nested, &repository.root).is_err());
    }

    #[test]
    fn record_transition_uses_advisory_lock_and_generation_compare_and_swap() {
        let repository = TestRepository::new("record-lock");
        let target = divergent_target(&repository, "feature.txt");
        let plan = merge_plan(&repository, &target);
        let store = repository.store();
        let record = store
            .create_planned("locked", "project-1", "session-1", &plan)
            .unwrap();
        let lock = open_store_lock(&store.records_root.join(".records.lock")).unwrap();
        lock.try_lock().unwrap();
        let mut cancelled = record.clone();
        cancelled.state = "cancelled".into();
        cancelled.allowed_actions.clear();
        cancelled.generation += 1;
        cancelled.updated_at_ms += 1;
        assert!(store.transition(&record, &cancelled).is_err());
        lock.unlock().unwrap();
        store.transition(&record, &cancelled).unwrap();
        assert_eq!(store.load("locked").unwrap().state, "cancelled");
        assert!(store.transition(&record, &cancelled).is_err());
    }

    #[test]
    fn cherry_pick_requires_a_single_parent_commit() {
        let repository = TestRepository::new("cherry-mainline");
        let base = head(&repository.root);
        git(&repository.root, &["switch", "-c", "side"], true);
        write_and_commit(&repository.root, "side.txt", "side\n", "side");
        git(
            &repository.root,
            &["switch", "-c", "integration", &base],
            true,
        );
        write_and_commit(
            &repository.root,
            "integration.txt",
            "integration\n",
            "integration",
        );
        git(
            &repository.root,
            &["merge", "--no-ff", "side", "-m", "merge side"],
            true,
        );
        let merge_commit = head(&repository.root);
        git(&repository.root, &["switch", "main"], true);
        let plan = plan_git_workflow(
            &repository.root,
            GitWorkflowRequest::CherryPick {
                commit_oid: merge_commit,
                committer: identity(),
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
            &HashSet::new(),
            None,
        )
        .unwrap();
        assert!(plan
            .blocking_reasons
            .contains(&"cherry-pick-merge-commit-requires-mainline".into()));
    }

    #[test]
    fn reconciles_real_merge_conflict_with_three_stage_evidence() {
        let repository = TestRepository::new("conflict");
        write_and_commit(
            &repository.root,
            "conflict.txt",
            "shared base\n",
            "conflict base",
        );
        let target = divergent_target(&repository, "conflict.txt");
        write_and_commit(&repository.root, "conflict.txt", "main\n", "main");
        let plan = merge_plan(&repository, &target);
        let store = repository.store();
        store
            .create_planned("merge-conflict", "project-1", "session-1", &plan)
            .unwrap();

        git(
            &repository.root,
            &["merge", "--no-commit", "--no-ff", &target],
            false,
        );
        let record = store.reconcile("merge-conflict").unwrap();
        assert_eq!(record.state, "conflicted");
        assert_eq!(record.allowed_actions, vec!["resolve", "abort"]);
        assert_eq!(record.observed_operation.as_deref(), Some("merge"));
        assert_eq!(record.conflicts.len(), 1);
        assert_eq!(record.conflicts[0].path, "conflict.txt");
        assert_eq!(
            record.conflicts[0]
                .stages
                .iter()
                .map(|stage| stage.stage)
                .collect::<Vec<_>>(),
            vec![1, 2, 3]
        );
        assert_eq!(record.redacted_conflict_path_count, 0);

        let abort = plan_git_workflow(
            &repository.root,
            GitWorkflowRequest::Abort {
                operation_id: record.operation_id.clone(),
                generation: record.generation,
            },
            &HashSet::new(),
            Some(&record),
        )
        .unwrap();
        assert_eq!(abort.risk.class, "high");
        assert!(abort.risk.requires_explicit_approval);
        assert!(abort.blocking_reasons.is_empty());
        assert!(plan_git_workflow(
            &repository.root,
            GitWorkflowRequest::Continue {
                operation_id: record.operation_id.clone(),
                generation: record.generation + 1,
            },
            &HashSet::new(),
            Some(&record),
        )
        .is_err());
        assert!(plan_git_workflow(
            &repository.root,
            GitWorkflowRequest::Abort {
                operation_id: record.operation_id.clone(),
                generation: record.generation + 1,
            },
            &HashSet::new(),
            Some(&record),
        )
        .is_err());
        git(&repository.root, &["merge", "--abort"], true);
    }

    #[test]
    fn reports_hooks_and_blocks_custom_merge_and_filter_drivers() {
        let repository = TestRepository::new("hazards");
        let target = divergent_target(&repository, "feature.txt");
        let git_directory = PathBuf::from(
            String::from_utf8(git(
                &repository.root,
                &["rev-parse", "--absolute-git-dir"],
                true,
            ))
            .unwrap()
            .trim(),
        );
        let hook = git_directory.join("hooks/pre-rebase");
        fs::write(&hook, "#!/bin/sh\nexit 0\n").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&hook, fs::Permissions::from_mode(0o700)).unwrap();
        }
        git(
            &repository.root,
            &[
                "config",
                "merge.aegisy-test.driver",
                "external-command %O %A %B",
            ],
            true,
        );
        git(
            &repository.root,
            &["config", "filter.aegisy-test.process", "external-filter"],
            true,
        );
        let plan = merge_plan(&repository, &target);
        assert!(plan.hazards.active_hooks.contains(&"pre-rebase".into()));
        assert!(plan.hazards.custom_merge_driver);
        assert!(plan.hazards.custom_filter_driver);
        assert!(plan
            .blocking_reasons
            .contains(&"custom-merge-driver-not-authorized".into()));
        assert!(plan
            .blocking_reasons
            .contains(&"custom-filter-driver-not-authorized".into()));
        assert!(plan
            .risk
            .reasons
            .contains(&"repository-hooks-require-reviewed-policy".into()));
    }

    #[test]
    fn redacts_sensitive_conflict_paths_from_persisted_record() {
        let repository = TestRepository::new("sensitive");
        write_and_commit(&repository.root, ".env", "base\n", "sensitive base");
        let target = divergent_target(&repository, ".env");
        write_and_commit(&repository.root, ".env", "main\n", "sensitive main");
        let plan = merge_plan(&repository, &target);
        let store = repository.store();
        store
            .create_planned("sensitive", "project-1", "session-1", &plan)
            .unwrap();
        git(
            &repository.root,
            &["merge", "--no-commit", "--no-ff", &target],
            false,
        );
        let record = store.reconcile("sensitive").unwrap();
        assert_eq!(record.state, "conflicted");
        assert!(record.conflicts.is_empty());
        assert_eq!(record.redacted_conflict_path_count, 1);
        let bytes = read_bounded(
            &store.record_path("sensitive"),
            MAX_RECORD_BYTES,
            "test record",
        )
        .unwrap();
        assert!(!String::from_utf8(bytes).unwrap().contains(".env"));
        git(&repository.root, &["merge", "--abort"], true);
    }

    #[test]
    fn foreign_and_disappeared_operation_markers_reconcile_conservatively() {
        let repository = TestRepository::new("markers");
        let target = divergent_target(&repository, "feature.txt");
        let plan = merge_plan(&repository, &target);
        let store = repository.store();
        store
            .create_planned("foreign", "project-1", "session-1", &plan)
            .unwrap();
        let git_directory = PathBuf::from(
            String::from_utf8(git(
                &repository.root,
                &["rev-parse", "--absolute-git-dir"],
                true,
            ))
            .unwrap()
            .trim(),
        );
        fs::write(
            git_directory.join("CHERRY_PICK_HEAD"),
            format!("{target}\n"),
        )
        .unwrap();
        let foreign = store.reconcile("foreign").unwrap();
        assert_eq!(foreign.state, "blocked-foreign-operation");
        assert_eq!(foreign.allowed_actions, vec!["inspect"]);
        assert!(foreign.conflicts.is_empty());
        fs::remove_file(git_directory.join("CHERRY_PICK_HEAD")).unwrap();

        store
            .create_planned("interrupted", "project-1", "session-1", &plan)
            .unwrap();
        fs::write(git_directory.join("MERGE_HEAD"), format!("{target}\n")).unwrap();
        let active = store.reconcile("interrupted").unwrap();
        assert_eq!(active.state, "in-progress");
        fs::remove_file(git_directory.join("MERGE_HEAD")).unwrap();
        let interrupted = store.reconcile("interrupted").unwrap();
        assert_eq!(interrupted.state, "interrupted-needs-reconciliation");
        assert_eq!(interrupted.allowed_actions, vec!["inspect"]);
    }
}
