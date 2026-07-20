//! Dedicated-worktree admission for child tasks.
//!
//! A verified isolation is not a permission or execution ticket. It only proves
//! that a child request is bound to a live, unique, clean worktree before future
//! write-capable tools can be considered by the permission and sandbox layers.

use crate::child_task::ChildTaskRequest;
use crate::child_task_state::{CancellationState, ChildTaskState, ChildTaskStatus};
use crate::git_worktree_lifecycle::{
    refresh_worktree_health, GitWorktreeDescriptor, GitWorktreeOwner,
};
use serde::{Deserialize, Serialize};
use serde_json::to_vec;
use sha2::{Digest, Sha256};
use std::fs;
use std::path::{Path, PathBuf};

pub const SCHEMA_VERSION: &str = "child-worktree-admission/0.1";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ChildWorktreeGateError {
    pub code: &'static str,
    pub message: &'static str,
}

impl ChildWorktreeGateError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ChildWorkspaceIsolation {
    SharedReadOnly,
    DedicatedWorktree,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ChildWorktreeAdmissionReceipt {
    pub schema_version: String,
    pub task_identity: String,
    pub state_identity: String,
    pub parent_session_id: String,
    pub child_session_id: String,
    pub project_id: String,
    pub root_id: String,
    pub isolation: ChildWorkspaceIsolation,
    pub requested_write: bool,
    pub base_revision_identity: String,
    pub worktree_id: Option<String>,
    pub descriptor_identity: Option<String>,
    pub health_observed_at_ms: Option<u64>,
    pub isolation_ready: bool,
    pub permission_granted: bool,
    pub execution_available: bool,
}

impl ChildWorktreeAdmissionReceipt {
    pub fn identity(&self) -> Result<String, ChildWorktreeGateError> {
        validate_receipt(self)?;
        let bytes = to_vec(self).map_err(|_| {
            ChildWorktreeGateError::new(
                "child-worktree-receipt-serialize-failed",
                "child worktree receipt could not be serialized",
            )
        })?;
        Ok(format!(
            "child-worktree-admission:sha256:{:x}",
            Sha256::digest(bytes)
        ))
    }
}

/// Non-serializable isolation proof retained inside the runtime.
///
/// Callers must still intersect this proof with managed policy, permission,
/// approval, sandbox, budget, and recovery gates before any write execution.
#[derive(Debug)]
pub struct VerifiedChildWorkspace {
    root: PathBuf,
    receipt: ChildWorktreeAdmissionReceipt,
}

impl VerifiedChildWorkspace {
    pub fn root(&self) -> &Path {
        &self.root
    }

    pub fn receipt(&self) -> &ChildWorktreeAdmissionReceipt {
        &self.receipt
    }
}

pub fn admit_child_workspace(
    project_root: &Path,
    worktree_storage_root: Option<&Path>,
    request: &ChildTaskRequest,
    state: &ChildTaskState,
    descriptor: Option<&GitWorktreeDescriptor>,
) -> Result<VerifiedChildWorkspace, ChildWorktreeGateError> {
    request.validate().map_err(|_| {
        ChildWorktreeGateError::new(
            "child-worktree-request-invalid",
            "child task request is invalid",
        )
    })?;
    state.validate().map_err(|_| {
        ChildWorktreeGateError::new(
            "child-worktree-state-invalid",
            "child task state is invalid",
        )
    })?;
    let task_identity = request.identity().map_err(|_| {
        ChildWorktreeGateError::new(
            "child-worktree-request-identity-invalid",
            "child task request identity is invalid",
        )
    })?;
    if state.task_identity != task_identity
        || state.parent_session_id != request.parent_session_id
        || state.parent_turn_id != request.parent_turn_id
    {
        return Err(ChildWorktreeGateError::new(
            "child-worktree-task-binding-mismatch",
            "child task request and lifecycle binding do not match",
        ));
    }
    if state.status != ChildTaskStatus::Running
        || state.cancellation == CancellationState::Requested
    {
        return Err(ChildWorktreeGateError::new(
            "child-worktree-state-not-runnable",
            "child task is not in a runnable state",
        ));
    }
    let child_session_id = state.child_session_id.as_deref().ok_or_else(|| {
        ChildWorktreeGateError::new(
            "child-worktree-child-session-missing",
            "child session is not bound",
        )
    })?;
    let state_identity = state.identity().map_err(|_| {
        ChildWorktreeGateError::new(
            "child-worktree-state-identity-invalid",
            "child task state identity is invalid",
        )
    })?;

    match request.workspace.isolation.as_str() {
        "read_only" => {
            if request.permissions.write {
                return Err(ChildWorktreeGateError::new(
                    "child-worktree-dedicated-required",
                    "write-capable child requires a dedicated worktree",
                ));
            }
            if descriptor.is_some() {
                return Err(ChildWorktreeGateError::new(
                    "child-worktree-read-only-evidence-unexpected",
                    "shared read-only child must not claim worktree authority",
                ));
            }
            let root = canonical_directory(project_root, "child-worktree-project-unavailable")?;
            let receipt = ChildWorktreeAdmissionReceipt {
                schema_version: SCHEMA_VERSION.into(),
                task_identity,
                state_identity,
                parent_session_id: state.parent_session_id.clone(),
                child_session_id: child_session_id.into(),
                project_id: request.workspace.project_id.clone(),
                root_id: request.workspace.root_id.clone(),
                isolation: ChildWorkspaceIsolation::SharedReadOnly,
                requested_write: false,
                base_revision_identity: revision_identity(&request.workspace.base_revision),
                worktree_id: None,
                descriptor_identity: None,
                health_observed_at_ms: None,
                isolation_ready: true,
                permission_granted: false,
                execution_available: false,
            };
            validate_receipt(&receipt)?;
            Ok(VerifiedChildWorkspace { root, receipt })
        }
        "dedicated_worktree" => admit_dedicated_worktree(
            project_root,
            worktree_storage_root,
            request,
            state,
            child_session_id,
            task_identity,
            state_identity,
            descriptor,
        ),
        _ => Err(ChildWorktreeGateError::new(
            "child-worktree-isolation-unsupported",
            "child workspace isolation is unsupported",
        )),
    }
}

#[allow(clippy::too_many_arguments)]
fn admit_dedicated_worktree(
    project_root: &Path,
    worktree_storage_root: Option<&Path>,
    request: &ChildTaskRequest,
    state: &ChildTaskState,
    child_session_id: &str,
    task_identity: String,
    state_identity: String,
    descriptor: Option<&GitWorktreeDescriptor>,
) -> Result<VerifiedChildWorkspace, ChildWorktreeGateError> {
    if request.permissions.write
        && !matches!(
            request.permissions.profile.as_str(),
            "workspace-write" | "developer" | "full-access"
        )
    {
        return Err(ChildWorktreeGateError::new(
            "child-worktree-write-profile-invalid",
            "write request does not name a write-capable permission profile",
        ));
    }
    let storage_root = worktree_storage_root.ok_or_else(|| {
        ChildWorktreeGateError::new(
            "child-worktree-storage-missing",
            "dedicated worktree storage is missing",
        )
    })?;
    let descriptor = descriptor.ok_or_else(|| {
        ChildWorktreeGateError::new(
            "child-worktree-descriptor-missing",
            "dedicated worktree descriptor is missing",
        )
    })?;
    let canonical_project =
        canonical_directory(project_root, "child-worktree-project-unavailable")?;
    let canonical_storage =
        canonical_directory(storage_root, "child-worktree-storage-unavailable")?;
    if canonical_project.starts_with(&canonical_storage)
        || canonical_storage.starts_with(&canonical_project)
    {
        return Err(ChildWorktreeGateError::new(
            "child-worktree-storage-overlap",
            "dedicated worktree storage overlaps the parent project",
        ));
    }
    let expected_owner = GitWorktreeOwner {
        session_id: state.parent_session_id.clone(),
        child_id: child_session_id.into(),
    };
    if descriptor.owner != expected_owner {
        return Err(ChildWorktreeGateError::new(
            "child-worktree-owner-mismatch",
            "dedicated worktree owner does not match the child lineage",
        ));
    }
    if descriptor.base_head != request.workspace.base_revision {
        return Err(ChildWorktreeGateError::new(
            "child-worktree-base-mismatch",
            "dedicated worktree base does not match the delegated revision",
        ));
    }
    let (_, health) =
        refresh_worktree_health(project_root, storage_root, &expected_owner, descriptor).map_err(
            |_| {
                ChildWorktreeGateError::new(
                    "child-worktree-health-unavailable",
                    "dedicated worktree health could not be verified",
                )
            },
        )?;
    if !health.healthy
        || !health.registered
        || !health.path_present
        || !health.locked
        || health.prunable
    {
        return Err(ChildWorktreeGateError::new(
            "child-worktree-unhealthy",
            "dedicated worktree is not healthy and isolated",
        ));
    }
    if health.dirty || health.conflicted || health.operation_in_progress.is_some() {
        return Err(ChildWorktreeGateError::new(
            "child-worktree-admission-not-clean",
            "dedicated worktree must be clean before child admission",
        ));
    }
    if health.head.as_deref() != Some(request.workspace.base_revision.as_str()) {
        return Err(ChildWorktreeGateError::new(
            "child-worktree-head-mismatch",
            "dedicated worktree HEAD does not match the delegated revision",
        ));
    }
    let root = canonical_directory(
        Path::new(&descriptor.worktree_path),
        "child-worktree-path-unavailable",
    )?;
    if root == canonical_project
        || root.starts_with(&canonical_project)
        || canonical_project.starts_with(&root)
    {
        return Err(ChildWorktreeGateError::new(
            "child-worktree-not-isolated",
            "dedicated worktree overlaps the parent project",
        ));
    }
    let descriptor_identity = content_identity("git-worktree-descriptor", descriptor)?;
    let receipt = ChildWorktreeAdmissionReceipt {
        schema_version: SCHEMA_VERSION.into(),
        task_identity,
        state_identity,
        parent_session_id: state.parent_session_id.clone(),
        child_session_id: child_session_id.into(),
        project_id: request.workspace.project_id.clone(),
        root_id: request.workspace.root_id.clone(),
        isolation: ChildWorkspaceIsolation::DedicatedWorktree,
        requested_write: request.permissions.write,
        base_revision_identity: revision_identity(&request.workspace.base_revision),
        worktree_id: Some(descriptor.worktree_id.clone()),
        descriptor_identity: Some(descriptor_identity),
        health_observed_at_ms: Some(health.observed_at_ms),
        isolation_ready: true,
        permission_granted: false,
        execution_available: false,
    };
    validate_receipt(&receipt)?;
    Ok(VerifiedChildWorkspace { root, receipt })
}

fn validate_receipt(receipt: &ChildWorktreeAdmissionReceipt) -> Result<(), ChildWorktreeGateError> {
    if receipt.schema_version != SCHEMA_VERSION
        || !receipt.isolation_ready
        || receipt.permission_granted
        || receipt.execution_available
    {
        return Err(ChildWorktreeGateError::new(
            "child-worktree-receipt-invalid",
            "child worktree receipt violates the isolation-only boundary",
        ));
    }
    for value in [
        &receipt.parent_session_id,
        &receipt.child_session_id,
        &receipt.project_id,
        &receipt.root_id,
    ] {
        if value.is_empty()
            || value.len() > 128
            || value
                .bytes()
                .any(|byte| !(byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b':')))
        {
            return Err(ChildWorktreeGateError::new(
                "child-worktree-receipt-invalid",
                "child worktree receipt contains an invalid identifier",
            ));
        }
    }
    if !valid_sha_identity(&receipt.task_identity, "child-task:sha256:")
        || !valid_sha_identity(&receipt.state_identity, "child-task-state:sha256:")
        || !valid_sha_identity(
            &receipt.base_revision_identity,
            "child-workspace-revision:sha256:",
        )
    {
        return Err(ChildWorktreeGateError::new(
            "child-worktree-receipt-invalid",
            "child worktree receipt contains an invalid content identity",
        ));
    }
    match receipt.isolation {
        ChildWorkspaceIsolation::SharedReadOnly => {
            if receipt.requested_write
                || receipt.worktree_id.is_some()
                || receipt.descriptor_identity.is_some()
                || receipt.health_observed_at_ms.is_some()
            {
                return Err(ChildWorktreeGateError::new(
                    "child-worktree-receipt-invalid",
                    "shared read-only receipt contains worktree authority",
                ));
            }
        }
        ChildWorkspaceIsolation::DedicatedWorktree => {
            if receipt.worktree_id.as_deref().is_none_or(|value| {
                value.is_empty()
                    || value.len() > 128
                    || !value.bytes().all(|byte| {
                        byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.')
                    })
            }) || receipt
                .descriptor_identity
                .as_deref()
                .is_none_or(|value| !valid_sha_identity(value, "git-worktree-descriptor:sha256:"))
                || receipt.health_observed_at_ms.is_none_or(|value| value == 0)
            {
                return Err(ChildWorktreeGateError::new(
                    "child-worktree-receipt-invalid",
                    "dedicated worktree receipt is incomplete",
                ));
            }
        }
    }
    Ok(())
}

fn revision_identity(revision: &str) -> String {
    format!(
        "child-workspace-revision:sha256:{:x}",
        Sha256::digest(revision.as_bytes())
    )
}

fn valid_sha_identity(value: &str, prefix: &str) -> bool {
    value.strip_prefix(prefix).is_some_and(|hex| {
        hex.len() == 64
            && hex
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    })
}

fn canonical_directory(path: &Path, code: &'static str) -> Result<PathBuf, ChildWorktreeGateError> {
    let metadata = fs::symlink_metadata(path).map_err(|_| {
        ChildWorktreeGateError::new(code, "child workspace directory is unavailable")
    })?;
    if metadata.file_type().is_symlink() {
        return Err(ChildWorktreeGateError::new(
            code,
            "child workspace directory cannot be a symlink",
        ));
    }
    let canonical = path.canonicalize().map_err(|_| {
        ChildWorktreeGateError::new(code, "child workspace directory is unavailable")
    })?;
    if !canonical.is_dir() {
        return Err(ChildWorktreeGateError::new(
            code,
            "child workspace path is not a directory",
        ));
    }
    Ok(canonical)
}

fn content_identity(
    prefix: &str,
    value: &impl Serialize,
) -> Result<String, ChildWorktreeGateError> {
    let bytes = to_vec(value).map_err(|_| {
        ChildWorktreeGateError::new(
            "child-worktree-evidence-serialize-failed",
            "child worktree evidence could not be serialized",
        )
    })?;
    Ok(format!("{prefix}:sha256:{:x}", Sha256::digest(bytes)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::child_task::{
        ChildTaskBudget, ExpectedResultShape, PermissionRequest, WorkspaceScope,
        HANDOFF_SCHEMA_VERSION,
    };
    use crate::git_worktree_lifecycle::{
        create_dedicated_worktree, plan_dedicated_worktree, GitWorktreeRequest,
    };
    use std::process::Command;
    use std::sync::atomic::{AtomicU64, Ordering};

    static SEQUENCE: AtomicU64 = AtomicU64::new(0);

    struct Fixture {
        root: PathBuf,
        storage: PathBuf,
        head: String,
    }

    impl Fixture {
        fn new() -> Self {
            let nonce = SEQUENCE.fetch_add(1, Ordering::Relaxed);
            let parent = std::env::temp_dir().join(format!(
                "aegisy-child-worktree-gate-{}-{nonce}",
                std::process::id()
            ));
            let root = parent.join("repository");
            let storage = parent.join("worktrees");
            fs::create_dir_all(&root).unwrap();
            fs::create_dir_all(&storage).unwrap();
            git(&root, &["init", "-q"]);
            git(&root, &["config", "user.name", "Aegisy Test"]);
            git(&root, &["config", "user.email", "test@aegisy.local"]);
            fs::write(root.join("tracked.txt"), "initial\n").unwrap();
            git(&root, &["add", "."]);
            git(&root, &["commit", "-q", "-m", "initial"]);
            let head = String::from_utf8(git(&root, &["rev-parse", "HEAD"]))
                .unwrap()
                .trim()
                .to_owned();
            Self {
                root: root.canonicalize().unwrap(),
                storage: storage.canonicalize().unwrap(),
                head,
            }
        }

        fn request(&self, task_id: &str, isolation: &str, write: bool) -> ChildTaskRequest {
            ChildTaskRequest {
                schema_version: crate::child_task::SCHEMA_VERSION.into(),
                task_id: task_id.into(),
                parent_session_id: "parent-session".into(),
                parent_turn_id: "parent-turn".into(),
                goal: "Apply the reviewed change in an isolated child workspace".into(),
                context: Vec::new(),
                workspace: WorkspaceScope {
                    project_id: "project-1".into(),
                    root_id: "root-1".into(),
                    isolation: isolation.into(),
                    base_revision: self.head.clone(),
                },
                tools: vec!["workspace-read".into()],
                model_profile: "agent-default".into(),
                permissions: PermissionRequest {
                    profile: if write {
                        "workspace-write".into()
                    } else {
                        "read-only".into()
                    },
                    network: "none".into(),
                    write,
                },
                budget: ChildTaskBudget {
                    max_tokens: 100_000,
                    max_cost_micros: 1_000_000,
                    max_wall_time_ms: 300_000,
                    max_turns: 8,
                    max_tool_calls: 32,
                    max_concurrency: 1,
                },
                expected_result: ExpectedResultShape {
                    schema_version: HANDOFF_SCHEMA_VERSION.into(),
                    max_summary_bytes: 8 * 1024,
                    required_sections: vec!["summary".into()],
                },
            }
        }

        fn state(&self, request: &ChildTaskRequest, child_session: &str) -> ChildTaskState {
            let mut state = ChildTaskState::new(
                request.identity().unwrap(),
                request.parent_session_id.clone(),
                request.parent_turn_id.clone(),
                1_000,
            )
            .unwrap();
            state.attach_child_session(child_session, 1_100).unwrap();
            state
        }

        fn descriptor(
            &self,
            request: &ChildTaskRequest,
            child_session: &str,
        ) -> GitWorktreeDescriptor {
            let owner = GitWorktreeOwner {
                session_id: request.parent_session_id.clone(),
                child_id: child_session.into(),
            };
            let worktree_request = GitWorktreeRequest {
                worktree_id: format!("worktree-{}", request.task_id),
                branch: format!("aegisy/{}", request.task_id),
                owner,
            };
            let plan =
                plan_dedicated_worktree(&self.root, &self.storage, worktree_request).unwrap();
            assert!(plan.blocking_reasons.is_empty());
            create_dedicated_worktree(&self.root, &self.storage, &plan).unwrap()
        }
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(self.root.parent().unwrap());
        }
    }

    fn git(root: &Path, args: &[&str]) -> Vec<u8> {
        let output = Command::new("git")
            .arg("-C")
            .arg(root)
            .args(args)
            .output()
            .unwrap();
        assert!(output.status.success(), "git command failed: {args:?}");
        output.stdout
    }

    #[test]
    fn shared_read_only_child_never_receives_write_or_execution_authority() {
        let fixture = Fixture::new();
        let mut request = fixture.request("read-only", "read_only", false);
        request.workspace.base_revision = "private/source/revision".into();
        let state = fixture.state(&request, "child-read-only");
        let verified = admit_child_workspace(&fixture.root, None, &request, &state, None).unwrap();
        assert_eq!(verified.root(), fixture.root);
        assert_eq!(
            verified.receipt().isolation,
            ChildWorkspaceIsolation::SharedReadOnly
        );
        assert!(!verified.receipt().requested_write);
        assert!(!verified.receipt().permission_granted);
        assert!(!verified.receipt().execution_available);
        assert!(verified.receipt().identity().is_ok());
        assert!(!serde_json::to_string(verified.receipt())
            .unwrap()
            .contains("private/source/revision"));
    }

    #[test]
    fn write_child_requires_live_clean_owned_dedicated_worktree() {
        let fixture = Fixture::new();
        let request = fixture.request("writer", "dedicated_worktree", true);
        let state = fixture.state(&request, "child-writer");
        let descriptor = fixture.descriptor(&request, "child-writer");
        let verified = admit_child_workspace(
            &fixture.root,
            Some(&fixture.storage),
            &request,
            &state,
            Some(&descriptor),
        )
        .unwrap();
        assert_ne!(verified.root(), fixture.root);
        assert_eq!(
            verified.receipt().isolation,
            ChildWorkspaceIsolation::DedicatedWorktree
        );
        assert!(verified.receipt().requested_write);
        assert!(!verified.receipt().permission_granted);
        assert!(!verified.receipt().execution_available);
        assert_eq!(
            verified.receipt().base_revision_identity,
            revision_identity(&request.workspace.base_revision)
        );
        assert_eq!(
            admit_child_workspace(
                &fixture.root,
                Some(&fixture.root),
                &request,
                &state,
                Some(&descriptor),
            )
            .unwrap_err()
            .code,
            "child-worktree-storage-overlap"
        );
    }

    #[test]
    fn write_request_fails_before_evidence_when_isolation_is_shared() {
        let fixture = Fixture::new();
        let request = fixture.request("unsafe", "read_only", true);
        let state = fixture.state(&request, "child-unsafe");
        assert_eq!(
            admit_child_workspace(&fixture.root, None, &request, &state, None)
                .unwrap_err()
                .code,
            "child-worktree-dedicated-required"
        );
    }

    #[test]
    fn concurrent_child_cannot_reuse_another_child_worktree() {
        let fixture = Fixture::new();
        let first_request = fixture.request("first", "dedicated_worktree", true);
        let descriptor = fixture.descriptor(&first_request, "child-first");
        let second_request = fixture.request("second", "dedicated_worktree", true);
        let second_state = fixture.state(&second_request, "child-second");
        assert_eq!(
            admit_child_workspace(
                &fixture.root,
                Some(&fixture.storage),
                &second_request,
                &second_state,
                Some(&descriptor),
            )
            .unwrap_err()
            .code,
            "child-worktree-owner-mismatch"
        );
    }

    #[test]
    fn dirty_or_cancel_pending_child_fails_closed_before_admission() {
        let fixture = Fixture::new();
        let request = fixture.request("blocked", "dedicated_worktree", true);
        let mut state = fixture.state(&request, "child-blocked");
        let descriptor = fixture.descriptor(&request, "child-blocked");
        fs::write(
            Path::new(&descriptor.worktree_path).join("tracked.txt"),
            "dirty\n",
        )
        .unwrap();
        assert_eq!(
            admit_child_workspace(
                &fixture.root,
                Some(&fixture.storage),
                &request,
                &state,
                Some(&descriptor),
            )
            .unwrap_err()
            .code,
            "child-worktree-admission-not-clean"
        );

        git(
            Path::new(&descriptor.worktree_path),
            &["checkout", "--", "tracked.txt"],
        );
        state.request_cancel(1_200).unwrap();
        assert_eq!(
            admit_child_workspace(
                &fixture.root,
                Some(&fixture.storage),
                &request,
                &state,
                Some(&descriptor),
            )
            .unwrap_err()
            .code,
            "child-worktree-state-not-runnable"
        );
    }
}
