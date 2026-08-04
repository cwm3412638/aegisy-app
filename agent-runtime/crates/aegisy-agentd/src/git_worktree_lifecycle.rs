use crate::git_query::{overview, GitWorktree};
use crate::git_status::{status, GitRunner, GitStatusError};
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

const PLAN_SCHEMA_VERSION: &str = "git-worktree-plan/0.1";
const DESCRIPTOR_SCHEMA_VERSION: &str = "git-worktree-descriptor/0.1";
const HEALTH_SCHEMA_VERSION: &str = "git-worktree-health/0.1";
const CLEANUP_SCHEMA_VERSION: &str = "git-worktree-cleanup/0.1";
const MAX_GIT_OUTPUT: u64 = 256 * 1024;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GitWorktreeOwner {
    pub session_id: String,
    pub child_id: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GitWorktreeRequest {
    pub worktree_id: String,
    pub branch: String,
    pub owner: GitWorktreeOwner,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GitWorktreePlan {
    pub schema_version: String,
    pub repository_root: String,
    pub repository_common_dir: String,
    pub storage_root: String,
    pub worktree_path: String,
    pub request: GitWorktreeRequest,
    pub base_head: String,
    pub base_branch: Option<String>,
    pub blocking_reasons: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GitWorktreeDescriptor {
    pub schema_version: String,
    pub repository_root: String,
    pub repository_common_dir: String,
    pub storage_root: String,
    pub worktree_path: String,
    pub worktree_id: String,
    pub branch: String,
    pub base_head: String,
    pub owner: GitWorktreeOwner,
    pub created_at_ms: u64,
    pub last_health_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GitWorktreeHealth {
    pub schema_version: String,
    pub observed_at_ms: u64,
    pub healthy: bool,
    pub registered: bool,
    pub path_present: bool,
    pub locked: bool,
    pub prunable: bool,
    pub branch: Option<String>,
    pub head: Option<String>,
    pub dirty: bool,
    pub conflicted: bool,
    pub operation_in_progress: Option<String>,
    pub reasons: Vec<String>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum GitWorktreeChildState {
    Queued,
    Running,
    Completed,
    Failed,
    Cancelled,
    Interrupted,
}

impl GitWorktreeChildState {
    fn is_terminal(self) -> bool {
        matches!(
            self,
            Self::Completed | Self::Failed | Self::Cancelled | Self::Interrupted
        )
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum GitWorktreeDisposition {
    Pending,
    Keep,
    Integrated,
    DiscardApproved,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GitWorktreeCleanupDecision {
    pub schema_version: String,
    pub eligible: bool,
    pub child_state: GitWorktreeChildState,
    pub disposition: GitWorktreeDisposition,
    pub blocking_reasons: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GitWorktreeLifecycleError {
    pub message: String,
    pub rollback_complete: bool,
}

pub fn plan_dedicated_worktree(
    root: &Path,
    storage_root: &Path,
    request: GitWorktreeRequest,
) -> Result<GitWorktreePlan, GitWorktreeLifecycleError> {
    validate_request(&request)?;
    let storage_is_symlink = fs::symlink_metadata(storage_root)
        .map_err(|_| error("worktree storage root is unavailable"))?
        .file_type()
        .is_symlink();
    let snapshot = status(root).map_err(from_git)?;
    if !snapshot.repository || !snapshot.worktree {
        return Err(error("project is not inside a Git worktree"));
    }
    let base_head = snapshot
        .head_oid
        .ok_or_else(|| error("dedicated worktrees require an existing HEAD commit"))?;
    let repository_root = canonical_directory(root, "Git project root")?;
    let storage_root = canonical_directory(storage_root, "worktree storage root")?;
    let repository_root_text = path_to_utf8(&repository_root, "Git project root")?;
    let storage_root_text = path_to_utf8(&storage_root, "worktree storage root")?;
    let repository_common_dir = common_dir(root)?;
    validate_branch_name(root, &request.branch)?;
    let worktree_path = storage_root.join(&request.worktree_id);
    let worktree_path_text = path_to_utf8(&worktree_path, "dedicated worktree path")?;
    let repository = overview(root).map_err(from_git)?;
    let mut blocking_reasons = Vec::new();

    if snapshot.repository_root.as_deref() != Some(repository_root_text.as_str()) {
        blocking_reasons.push("project-is-not-worktree-root".into());
    }
    if repository_root.starts_with(&storage_root) || storage_root.starts_with(&repository_root) {
        blocking_reasons.push("storage-overlaps-repository".into());
    }
    if !snapshot.entries.is_empty() {
        blocking_reasons.push("dirty-base-worktree".into());
    }
    if !snapshot.conflicts.is_empty() {
        blocking_reasons.push("base-has-conflicts".into());
    }
    if snapshot.operation_in_progress.is_some() {
        blocking_reasons.push("base-operation-in-progress".into());
    }
    if snapshot.truncated {
        blocking_reasons.push("base-status-truncated".into());
    }
    if repository
        .branches
        .iter()
        .any(|branch| branch.name == request.branch)
    {
        blocking_reasons.push("target-branch-exists".into());
    }
    if repository.worktrees.iter().any(|worktree| {
        worktree.branch.as_deref() == Some(request.branch.as_str())
            || same_worktree_path(worktree, &worktree_path)
    }) {
        blocking_reasons.push("target-already-associated".into());
    }
    match fs::symlink_metadata(&worktree_path) {
        Ok(metadata) => {
            blocking_reasons.push(
                if metadata.file_type().is_symlink() {
                    "target-path-is-symlink"
                } else {
                    "target-path-exists"
                }
                .into(),
            );
        }
        Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => {}
        Err(_) => blocking_reasons.push("target-path-unavailable".into()),
    }
    if storage_is_symlink {
        blocking_reasons.push("storage-root-is-symlink".into());
    }

    blocking_reasons.sort();
    blocking_reasons.dedup();
    Ok(GitWorktreePlan {
        schema_version: PLAN_SCHEMA_VERSION.into(),
        repository_root: repository_root_text,
        repository_common_dir,
        storage_root: storage_root_text,
        worktree_path: worktree_path_text,
        request,
        base_head,
        base_branch: snapshot.branch,
        blocking_reasons,
    })
}

pub fn create_dedicated_worktree(
    root: &Path,
    storage_root: &Path,
    plan: &GitWorktreePlan,
) -> Result<GitWorktreeDescriptor, GitWorktreeLifecycleError> {
    create_with_fault(root, storage_root, plan, &NoFault)
}

pub fn refresh_worktree_health(
    root: &Path,
    storage_root: &Path,
    expected_owner: &GitWorktreeOwner,
    descriptor: &GitWorktreeDescriptor,
) -> Result<(GitWorktreeDescriptor, GitWorktreeHealth), GitWorktreeLifecycleError> {
    let observed_at_ms = now_ms()?;
    let mut reasons = validate_descriptor(root, storage_root, expected_owner, descriptor)?;
    let expected_path = Path::new(&descriptor.worktree_path);
    let repository = overview(root).map_err(from_git)?;
    let record = repository
        .worktrees
        .iter()
        .find(|worktree| same_worktree_path(worktree, expected_path));
    let registered = record.is_some();
    let path_metadata = fs::symlink_metadata(expected_path).ok();
    let path_present = path_metadata.is_some();
    if !path_present {
        reasons.push("worktree-path-missing".into());
    } else if path_metadata.is_some_and(|metadata| metadata.file_type().is_symlink()) {
        reasons.push("worktree-path-is-symlink".into());
    }
    if !registered {
        reasons.push("worktree-not-registered".into());
    }

    let locked = record.is_some_and(|worktree| worktree.locked_reason.is_some());
    let prunable = record.is_some_and(|worktree| worktree.prunable_reason.is_some());
    let branch = record.and_then(|worktree| worktree.branch.clone());
    let head = record.and_then(|worktree| worktree.head_oid.clone());
    if registered && !locked {
        reasons.push("worktree-not-locked".into());
    }
    if prunable {
        reasons.push("worktree-prunable".into());
    }
    if branch.as_deref() != Some(descriptor.branch.as_str()) {
        reasons.push("worktree-branch-mismatch".into());
    }

    let mut dirty = false;
    let mut conflicted = false;
    let mut operation_in_progress = None;
    if path_present
        && !reasons
            .iter()
            .any(|reason| reason == "worktree-path-is-symlink")
    {
        match status(expected_path) {
            Ok(snapshot) if snapshot.repository && snapshot.worktree => {
                dirty = !snapshot.entries.is_empty();
                conflicted = !snapshot.conflicts.is_empty();
                operation_in_progress = snapshot
                    .operation_in_progress
                    .map(|operation| operation.kind);
                if snapshot.repository_root.as_deref() != Some(descriptor.worktree_path.as_str()) {
                    reasons.push("worktree-root-mismatch".into());
                }
                if snapshot.branch.as_deref() != Some(descriptor.branch.as_str()) {
                    reasons.push("worktree-status-branch-mismatch".into());
                }
                if snapshot.truncated {
                    reasons.push("worktree-status-truncated".into());
                }
                if common_dir(expected_path).as_deref()
                    != Ok(descriptor.repository_common_dir.as_str())
                {
                    reasons.push("repository-common-dir-mismatch".into());
                }
            }
            Ok(_) | Err(_) => reasons.push("worktree-status-unavailable".into()),
        }
    }
    reasons.sort();
    reasons.dedup();
    let health = GitWorktreeHealth {
        schema_version: HEALTH_SCHEMA_VERSION.into(),
        observed_at_ms,
        healthy: reasons.is_empty(),
        registered,
        path_present,
        locked,
        prunable,
        branch,
        head,
        dirty,
        conflicted,
        operation_in_progress,
        reasons,
    };
    let mut refreshed = descriptor.clone();
    refreshed.last_health_at_ms = observed_at_ms;
    Ok((refreshed, health))
}

pub fn evaluate_cleanup(
    health: &GitWorktreeHealth,
    child_state: GitWorktreeChildState,
    disposition: GitWorktreeDisposition,
    has_pending_editor_edits: bool,
) -> GitWorktreeCleanupDecision {
    let mut blocking_reasons = Vec::new();
    if !child_state.is_terminal() {
        blocking_reasons.push("child-not-terminal".into());
    }
    if !health.healthy {
        blocking_reasons.push("worktree-unhealthy".into());
    }
    if health.dirty {
        blocking_reasons.push("worktree-dirty".into());
    }
    if health.conflicted {
        blocking_reasons.push("worktree-conflicted".into());
    }
    if health.operation_in_progress.is_some() {
        blocking_reasons.push("worktree-operation-in-progress".into());
    }
    if has_pending_editor_edits {
        blocking_reasons.push("pending-editor-edits".into());
    }
    match disposition {
        GitWorktreeDisposition::Pending => blocking_reasons.push("disposition-pending".into()),
        GitWorktreeDisposition::Keep => blocking_reasons.push("worktree-retained".into()),
        GitWorktreeDisposition::Integrated | GitWorktreeDisposition::DiscardApproved => {}
    }
    blocking_reasons.sort();
    blocking_reasons.dedup();
    GitWorktreeCleanupDecision {
        schema_version: CLEANUP_SCHEMA_VERSION.into(),
        eligible: blocking_reasons.is_empty(),
        child_state,
        disposition,
        blocking_reasons,
    }
}

trait FaultInjector {
    fn after_add(&self) -> Result<(), GitWorktreeLifecycleError> {
        Ok(())
    }

    fn after_lock(&self) -> Result<(), GitWorktreeLifecycleError> {
        Ok(())
    }
}

struct NoFault;

impl FaultInjector for NoFault {}

fn create_with_fault<F: FaultInjector>(
    root: &Path,
    storage_root: &Path,
    plan: &GitWorktreePlan,
    fault: &F,
) -> Result<GitWorktreeDescriptor, GitWorktreeLifecycleError> {
    let current = plan_dedicated_worktree(root, storage_root, plan.request.clone())?;
    if current != *plan {
        return Err(error("Git worktree plan is stale or was modified"));
    }
    if !plan.blocking_reasons.is_empty() {
        return Err(error(format!(
            "Git worktree creation is blocked: {}",
            plan.blocking_reasons.join(",")
        )));
    }
    let runner = GitRunner::new(root).map_err(from_git)?;
    let path = Path::new(&plan.worktree_path);
    run(
        &runner,
        &[
            "worktree",
            "add",
            "-b",
            &plan.request.branch,
            &plan.worktree_path,
            &plan.base_head,
        ],
        "Git worktree creation failed",
    )?;
    if let Err(cause) = fault.after_add() {
        return Err(with_rollback(cause, &runner, plan));
    }
    let lock_reason = format!(
        "aegisy session={} child={}",
        plan.request.owner.session_id, plan.request.owner.child_id
    );
    if let Err(cause) = run(
        &runner,
        &[
            "worktree",
            "lock",
            "--reason",
            &lock_reason,
            &plan.worktree_path,
        ],
        "Git worktree lock failed",
    ) {
        return Err(with_rollback(cause, &runner, plan));
    }
    if let Err(cause) = fault.after_lock() {
        return Err(with_rollback(cause, &runner, plan));
    }
    let snapshot = status(path).map_err(from_git)?;
    let repository = overview(root).map_err(from_git)?;
    let record = repository
        .worktrees
        .iter()
        .find(|worktree| same_worktree_path(worktree, path));
    let valid = snapshot.repository
        && snapshot.worktree
        && snapshot.repository_root.as_deref() == Some(plan.worktree_path.as_str())
        && snapshot.branch.as_deref() == Some(plan.request.branch.as_str())
        && snapshot.head_oid.as_deref() == Some(plan.base_head.as_str())
        && snapshot.entries.is_empty()
        && snapshot.operation_in_progress.is_none()
        && record.is_some_and(|worktree| {
            worktree.branch.as_deref() == Some(plan.request.branch.as_str())
                && worktree.head_oid.as_deref() == Some(plan.base_head.as_str())
                && worktree.locked_reason.is_some()
                && worktree.prunable_reason.is_none()
        })
        && common_dir(path).as_deref() == Ok(plan.repository_common_dir.as_str());
    if !valid {
        return Err(with_rollback(
            error("Git worktree post-state verification failed"),
            &runner,
            plan,
        ));
    }
    let timestamp = now_ms()?;
    Ok(GitWorktreeDescriptor {
        schema_version: DESCRIPTOR_SCHEMA_VERSION.into(),
        repository_root: plan.repository_root.clone(),
        repository_common_dir: plan.repository_common_dir.clone(),
        storage_root: plan.storage_root.clone(),
        worktree_path: plan.worktree_path.clone(),
        worktree_id: plan.request.worktree_id.clone(),
        branch: plan.request.branch.clone(),
        base_head: plan.base_head.clone(),
        owner: plan.request.owner.clone(),
        created_at_ms: timestamp,
        last_health_at_ms: timestamp,
    })
}

fn validate_descriptor(
    root: &Path,
    storage_root: &Path,
    expected_owner: &GitWorktreeOwner,
    descriptor: &GitWorktreeDescriptor,
) -> Result<Vec<String>, GitWorktreeLifecycleError> {
    validate_identifier(&descriptor.worktree_id, "worktree ID")?;
    validate_owner(expected_owner)?;
    let mut reasons = Vec::new();
    if descriptor.schema_version != DESCRIPTOR_SCHEMA_VERSION {
        reasons.push("descriptor-schema-mismatch".into());
    }
    if &descriptor.owner != expected_owner {
        reasons.push("worktree-owner-mismatch".into());
    }
    let repository_root = canonical_directory(root, "Git project root")?;
    let storage_root = canonical_directory(storage_root, "worktree storage root")?;
    let expected_path = storage_root.join(&descriptor.worktree_id);
    if descriptor.repository_root != path_to_utf8(&repository_root, "Git project root")? {
        reasons.push("repository-root-mismatch".into());
    }
    if descriptor.storage_root != path_to_utf8(&storage_root, "worktree storage root")? {
        reasons.push("storage-root-mismatch".into());
    }
    if descriptor.worktree_path != path_to_utf8(&expected_path, "dedicated worktree path")? {
        reasons.push("worktree-path-binding-mismatch".into());
    }
    if descriptor.repository_common_dir != common_dir(root)? {
        reasons.push("repository-common-dir-binding-mismatch".into());
    }
    Ok(reasons)
}

fn validate_request(request: &GitWorktreeRequest) -> Result<(), GitWorktreeLifecycleError> {
    validate_identifier(&request.worktree_id, "worktree ID")?;
    validate_owner(&request.owner)?;
    if request.branch.is_empty() || request.branch.len() > 255 || request.branch.starts_with('-') {
        return Err(error("Git worktree branch name is invalid"));
    }
    Ok(())
}

fn validate_owner(owner: &GitWorktreeOwner) -> Result<(), GitWorktreeLifecycleError> {
    validate_identifier(&owner.session_id, "session ID")?;
    validate_identifier(&owner.child_id, "child ID")
}

fn validate_identifier(value: &str, label: &str) -> Result<(), GitWorktreeLifecycleError> {
    if value.is_empty()
        || value.len() > 128
        || matches!(value, "." | "..")
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.'))
    {
        return Err(error(format!("{label} is invalid")));
    }
    Ok(())
}

fn common_dir(root: &Path) -> Result<String, GitWorktreeLifecycleError> {
    let runner = GitRunner::new(root).map_err(from_git)?;
    let output = runner
        .run(
            &["rev-parse", "--path-format=absolute", "--git-common-dir"],
            None,
            16 * 1024,
        )
        .map_err(from_git)?;
    if !output.success {
        return Err(error("Git common directory query failed"));
    }
    let raw = std::str::from_utf8(&output.stdout)
        .map_err(|_| error("Git common directory is not UTF-8"))?
        .trim();
    if raw.is_empty() {
        return Err(error("Git common directory is missing"));
    }
    let path = Path::new(raw);
    let path = if path.is_absolute() {
        path.to_owned()
    } else {
        root.join(path)
    };
    let canonical = crate::plain_path(
        &path
            .canonicalize()
            .map_err(|_| error("Git common directory is unavailable"))?,
    );
    path_to_utf8(&canonical, "Git common directory")
}

fn validate_branch_name(root: &Path, branch: &str) -> Result<(), GitWorktreeLifecycleError> {
    let runner = GitRunner::new(root).map_err(from_git)?;
    let output = runner
        .run(&["check-ref-format", "--branch", branch], None, 1024)
        .map_err(from_git)?;
    if output.success {
        Ok(())
    } else {
        Err(error("Git worktree branch name is invalid"))
    }
}

fn canonical_directory(path: &Path, label: &str) -> Result<PathBuf, GitWorktreeLifecycleError> {
    let canonical = crate::plain_path(
        &path
            .canonicalize()
            .map_err(|_| error(format!("{label} is unavailable")))?,
    );
    if !canonical.is_dir() {
        return Err(error(format!("{label} is not a directory")));
    }
    Ok(canonical)
}

fn path_to_utf8(path: &Path, label: &str) -> Result<String, GitWorktreeLifecycleError> {
    path.to_str()
        .map(str::to_owned)
        .ok_or_else(|| error(format!("{label} is not UTF-8")))
}

fn same_worktree_path(worktree: &GitWorktree, expected: &Path) -> bool {
    let actual = Path::new(&worktree.path);
    actual == expected
        || (actual.exists()
            && expected.exists()
            && actual.canonicalize().ok() == expected.canonicalize().ok())
}

fn rollback(runner: &GitRunner, plan: &GitWorktreePlan) -> Result<(), GitWorktreeLifecycleError> {
    let _ = run(
        runner,
        &["worktree", "unlock", &plan.worktree_path],
        "Git worktree unlock failed",
    );
    run(
        runner,
        &["worktree", "remove", "--force", &plan.worktree_path],
        "Git worktree rollback remove failed",
    )?;
    run(
        runner,
        &["branch", "-D", &plan.request.branch],
        "Git worktree rollback branch removal failed",
    )?;
    if Path::new(&plan.worktree_path).exists() {
        return Err(error("Git worktree rollback left the target path"));
    }
    Ok(())
}

fn with_rollback(
    cause: GitWorktreeLifecycleError,
    runner: &GitRunner,
    plan: &GitWorktreePlan,
) -> GitWorktreeLifecycleError {
    GitWorktreeLifecycleError {
        message: cause.message,
        rollback_complete: rollback(runner, plan).is_ok(),
    }
}

fn run(runner: &GitRunner, args: &[&str], message: &str) -> Result<(), GitWorktreeLifecycleError> {
    let output = runner.run(args, None, MAX_GIT_OUTPUT).map_err(from_git)?;
    if output.success {
        Ok(())
    } else {
        Err(error(message))
    }
}

fn now_ms() -> Result<u64, GitWorktreeLifecycleError> {
    let milliseconds = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| error("system clock is before the Unix epoch"))?
        .as_millis();
    u64::try_from(milliseconds).map_err(|_| error("system clock is out of range"))
}

fn from_git(cause: GitStatusError) -> GitWorktreeLifecycleError {
    GitWorktreeLifecycleError {
        message: cause.message,
        rollback_complete: true,
    }
}

fn error(message: impl Into<String>) -> GitWorktreeLifecycleError {
    GitWorktreeLifecycleError {
        message: message.into(),
        rollback_complete: true,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::Command;
    use std::sync::atomic::{AtomicU64, Ordering};

    static SEQUENCE: AtomicU64 = AtomicU64::new(0);

    enum FailurePoint {
        AfterAdd,
        AfterLock,
    }

    struct FailAt(FailurePoint);

    impl FaultInjector for FailAt {
        fn after_add(&self) -> Result<(), GitWorktreeLifecycleError> {
            if matches!(self.0, FailurePoint::AfterAdd) {
                Err(error("injected failure after worktree add"))
            } else {
                Ok(())
            }
        }

        fn after_lock(&self) -> Result<(), GitWorktreeLifecycleError> {
            if matches!(self.0, FailurePoint::AfterLock) {
                Err(error("injected failure after worktree lock"))
            } else {
                Ok(())
            }
        }
    }

    struct Fixture {
        root: PathBuf,
        storage: PathBuf,
    }

    impl Fixture {
        fn new() -> Self {
            let nonce = SEQUENCE.fetch_add(1, Ordering::Relaxed);
            let parent = std::env::temp_dir().join(format!(
                "aegisy-git-worktree-{}-{nonce}",
                std::process::id()
            ));
            let root = parent.join("repository");
            let storage = parent.join("worktrees");
            fs::create_dir_all(&root).unwrap();
            fs::create_dir_all(&storage).unwrap();
            git(&root, &["init", "-q"], true);
            git(&root, &["config", "user.name", "Aegisy Test"], true);
            git(&root, &["config", "user.email", "test@aegisy.local"], true);
            fs::write(root.join("tracked.txt"), "initial\n").unwrap();
            git(&root, &["add", "."], true);
            git(&root, &["commit", "-q", "-m", "initial"], true);
            Self {
                root: root.canonicalize().unwrap(),
                storage: storage.canonicalize().unwrap(),
            }
        }

        fn request(&self, suffix: &str) -> GitWorktreeRequest {
            GitWorktreeRequest {
                worktree_id: format!("worktree-{suffix}"),
                branch: format!("aegisy/child-{suffix}"),
                owner: GitWorktreeOwner {
                    session_id: "session-1".into(),
                    child_id: format!("child-{suffix}"),
                },
            }
        }
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            let parent = self.root.parent().unwrap();
            let _ = fs::remove_dir_all(parent);
        }
    }

    fn git(root: &Path, args: &[&str], success: bool) -> Vec<u8> {
        let output = Command::new("git")
            .arg("-C")
            .arg(root)
            .args(args)
            .output()
            .unwrap();
        assert_eq!(output.status.success(), success, "git command: {args:?}");
        output.stdout
    }

    #[test]
    fn creates_locks_associates_and_reports_health() {
        let fixture = Fixture::new();
        let request = fixture.request("healthy");
        let plan =
            plan_dedicated_worktree(&fixture.root, &fixture.storage, request.clone()).unwrap();
        assert!(plan.blocking_reasons.is_empty());
        let descriptor = create_dedicated_worktree(&fixture.root, &fixture.storage, &plan).unwrap();
        assert_eq!(descriptor.owner, request.owner);
        assert_eq!(descriptor.base_head, plan.base_head);

        let (refreshed, health) =
            refresh_worktree_health(&fixture.root, &fixture.storage, &request.owner, &descriptor)
                .unwrap();
        assert!(health.healthy, "{:?}", health.reasons);
        assert!(health.registered);
        assert!(health.locked);
        assert!(!health.dirty);
        assert_eq!(health.branch.as_deref(), Some(request.branch.as_str()));
        assert!(refreshed.last_health_at_ms >= descriptor.last_health_at_ms);

        let cleanup = evaluate_cleanup(
            &health,
            GitWorktreeChildState::Completed,
            GitWorktreeDisposition::Integrated,
            false,
        );
        assert!(cleanup.eligible);
        let running = evaluate_cleanup(
            &health,
            GitWorktreeChildState::Running,
            GitWorktreeDisposition::Integrated,
            false,
        );
        assert!(!running.eligible);
        assert!(running
            .blocking_reasons
            .contains(&"child-not-terminal".into()));
    }

    #[test]
    fn blocks_unsafe_storage_dirty_base_reuse_and_stale_plans() {
        let fixture = Fixture::new();
        let request = fixture.request("blocked");
        let overlapping =
            plan_dedicated_worktree(&fixture.root, &fixture.root, request.clone()).unwrap();
        assert!(overlapping
            .blocking_reasons
            .contains(&"storage-overlaps-repository".into()));

        fs::write(fixture.root.join("tracked.txt"), "dirty\n").unwrap();
        let dirty =
            plan_dedicated_worktree(&fixture.root, &fixture.storage, request.clone()).unwrap();
        assert!(dirty
            .blocking_reasons
            .contains(&"dirty-base-worktree".into()));
        git(&fixture.root, &["checkout", "--", "tracked.txt"], true);

        let stale =
            plan_dedicated_worktree(&fixture.root, &fixture.storage, request.clone()).unwrap();
        fs::write(fixture.root.join("next.txt"), "next\n").unwrap();
        git(&fixture.root, &["add", "."], true);
        git(&fixture.root, &["commit", "-q", "-m", "advance"], true);
        assert!(create_dedicated_worktree(&fixture.root, &fixture.storage, &stale).is_err());

        let target = fixture.storage.join(&request.worktree_id);
        fs::create_dir(&target).unwrap();
        let reused = plan_dedicated_worktree(&fixture.root, &fixture.storage, request).unwrap();
        assert!(reused
            .blocking_reasons
            .contains(&"target-path-exists".into()));
    }

    #[test]
    fn detects_dirty_unlocked_tampered_and_pending_cleanup_state() {
        let fixture = Fixture::new();
        let request = fixture.request("tamper");
        let plan =
            plan_dedicated_worktree(&fixture.root, &fixture.storage, request.clone()).unwrap();
        let descriptor = create_dedicated_worktree(&fixture.root, &fixture.storage, &plan).unwrap();
        let path = Path::new(&descriptor.worktree_path);
        fs::write(path.join("tracked.txt"), "dirty child\n").unwrap();
        git(
            &fixture.root,
            &["worktree", "unlock", &descriptor.worktree_path],
            true,
        );

        let (_, health) =
            refresh_worktree_health(&fixture.root, &fixture.storage, &request.owner, &descriptor)
                .unwrap();
        assert!(!health.healthy);
        assert!(health.dirty);
        assert!(health.reasons.contains(&"worktree-not-locked".into()));
        let cleanup = evaluate_cleanup(
            &health,
            GitWorktreeChildState::Completed,
            GitWorktreeDisposition::DiscardApproved,
            true,
        );
        assert!(!cleanup.eligible);
        assert!(cleanup.blocking_reasons.contains(&"worktree-dirty".into()));
        assert!(cleanup
            .blocking_reasons
            .contains(&"pending-editor-edits".into()));

        let mut tampered = descriptor;
        tampered.owner.child_id = "different-child".into();
        let (_, health) =
            refresh_worktree_health(&fixture.root, &fixture.storage, &request.owner, &tampered)
                .unwrap();
        assert!(health.reasons.contains(&"worktree-owner-mismatch".into()));
    }

    #[test]
    fn detects_branch_binding_and_missing_prunable_worktree() {
        let fixture = Fixture::new();
        let request = fixture.request("binding");
        let plan =
            plan_dedicated_worktree(&fixture.root, &fixture.storage, request.clone()).unwrap();
        let descriptor = create_dedicated_worktree(&fixture.root, &fixture.storage, &plan).unwrap();
        git(
            Path::new(&descriptor.worktree_path),
            &["branch", "-m", "aegisy/child-renamed"],
            true,
        );
        let (_, renamed) =
            refresh_worktree_health(&fixture.root, &fixture.storage, &request.owner, &descriptor)
                .unwrap();
        assert!(!renamed.healthy);
        assert!(renamed.reasons.contains(&"worktree-branch-mismatch".into()));
        assert!(renamed
            .reasons
            .contains(&"worktree-status-branch-mismatch".into()));

        git(
            &fixture.root,
            &["worktree", "unlock", &descriptor.worktree_path],
            true,
        );
        fs::remove_dir_all(&descriptor.worktree_path).unwrap();
        let (_, missing) =
            refresh_worktree_health(&fixture.root, &fixture.storage, &request.owner, &descriptor)
                .unwrap();
        assert!(!missing.healthy);
        assert!(!missing.path_present);
        assert!(missing.prunable);
        assert!(missing.reasons.contains(&"worktree-path-missing".into()));
        assert!(missing.reasons.contains(&"worktree-prunable".into()));
    }

    #[test]
    fn cleanup_requires_terminal_child_clean_state_and_explicit_disposition() {
        let healthy = GitWorktreeHealth {
            schema_version: HEALTH_SCHEMA_VERSION.into(),
            observed_at_ms: 1,
            healthy: true,
            registered: true,
            path_present: true,
            locked: true,
            prunable: false,
            branch: Some("aegisy/child-cleanup".into()),
            head: Some("0".repeat(40)),
            dirty: false,
            conflicted: false,
            operation_in_progress: None,
            reasons: Vec::new(),
        };
        for disposition in [
            GitWorktreeDisposition::Pending,
            GitWorktreeDisposition::Keep,
        ] {
            assert!(
                !evaluate_cleanup(
                    &healthy,
                    GitWorktreeChildState::Completed,
                    disposition,
                    false,
                )
                .eligible
            );
        }
        assert!(
            evaluate_cleanup(
                &healthy,
                GitWorktreeChildState::Failed,
                GitWorktreeDisposition::DiscardApproved,
                false,
            )
            .eligible
        );
        assert!(
            !evaluate_cleanup(
                &healthy,
                GitWorktreeChildState::Completed,
                GitWorktreeDisposition::Integrated,
                true,
            )
            .eligible
        );
    }

    #[test]
    fn rejects_invalid_branch_names_before_creation() {
        let fixture = Fixture::new();
        let mut request = fixture.request("invalid-branch");
        request.branch = "invalid..branch".into();
        let failure =
            plan_dedicated_worktree(&fixture.root, &fixture.storage, request).unwrap_err();
        assert_eq!(failure.message, "Git worktree branch name is invalid");
        assert!(fs::read_dir(&fixture.storage).unwrap().next().is_none());
    }

    #[cfg(unix)]
    #[test]
    fn blocks_symlinked_storage_root() {
        use std::os::unix::fs::symlink;

        let fixture = Fixture::new();
        let storage_link = fixture.root.parent().unwrap().join("worktrees-link");
        symlink(&fixture.storage, &storage_link).unwrap();
        let plan =
            plan_dedicated_worktree(&fixture.root, &storage_link, fixture.request("symlink"))
                .unwrap();
        assert!(plan
            .blocking_reasons
            .contains(&"storage-root-is-symlink".into()));
    }

    #[test]
    fn rolls_back_failures_after_add_and_after_lock() {
        for (suffix, failure) in [
            ("after-add", FailurePoint::AfterAdd),
            ("after-lock", FailurePoint::AfterLock),
        ] {
            let fixture = Fixture::new();
            let request = fixture.request(suffix);
            let plan =
                plan_dedicated_worktree(&fixture.root, &fixture.storage, request.clone()).unwrap();
            let failure =
                create_with_fault(&fixture.root, &fixture.storage, &plan, &FailAt(failure))
                    .unwrap_err();
            assert!(failure.rollback_complete);
            assert!(!Path::new(&plan.worktree_path).exists());
            let branches = String::from_utf8(git(
                &fixture.root,
                &["branch", "--list", &request.branch],
                true,
            ))
            .unwrap();
            assert!(branches.trim().is_empty());
            assert!(!overview(&fixture.root)
                .unwrap()
                .worktrees
                .iter()
                .any(|worktree| same_worktree_path(worktree, Path::new(&plan.worktree_path))));
        }
    }
}
