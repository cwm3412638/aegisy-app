use crate::git_commit_transaction::{identity_date, GitCommitIdentity};
use crate::git_status::{GitOutput, GitRunner, GitStatusError, GitWorkflowEnvironment};
use crate::git_workflow_authorization::{
    GitWorkflowAuthorizedAction, VerifiedGitWorkflowAuthorization,
};
use crate::git_workflow_state::{
    plan_git_workflow, GitMergeMode, GitWorkflowExecutionAttempt, GitWorkflowPlan,
    GitWorkflowRecord, GitWorkflowRequest, GitWorkflowStore,
};
use crate::workbench_store::{WorkbenchStore, WorkbenchStoreError};
use serde::Serialize;
use std::collections::HashSet;
use std::time::{SystemTime, UNIX_EPOCH};

const RESULT_SCHEMA_VERSION: &str = "git-workflow-execution-result/0.1";
const ATTEMPT_SCHEMA_VERSION: &str = "git-workflow-execution-attempt/0.1";
const MAX_GIT_OUTPUT: u64 = 2 * 1024 * 1024;

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitWorkflowExecutionResult {
    pub schema_version: String,
    pub operation_id: String,
    pub action: GitWorkflowAuthorizedAction,
    pub authorization_id: String,
    pub command_exit_code: Option<i32>,
    pub state: String,
    pub generation: u64,
    pub observed_head: String,
    pub observed_operation: Option<String>,
    pub outcome: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GitWorkflowExecutionError {
    pub message: String,
    pub command_dispatched: bool,
    pub recovery_required: bool,
}

pub fn execute_git_workflow(
    workflow_store: &GitWorkflowStore,
    event_store: &mut WorkbenchStore,
    authorization: VerifiedGitWorkflowAuthorization,
    action: GitWorkflowAuthorizedAction,
    pending_editor_paths: &HashSet<String>,
) -> Result<GitWorkflowExecutionResult, GitWorkflowExecutionError> {
    let operation_id = authorization.requirement().operation_id.clone();
    let record = workflow_store
        .load(&operation_id)
        .map_err(before_dispatch)?;
    let plan = current_action_plan(workflow_store, &record, action, pending_editor_paths)?;
    if !plan.blocking_reasons.is_empty() {
        return Err(before_dispatch_message(format!(
            "Git workflow execution is blocked: {}",
            plan.blocking_reasons.join(", ")
        )));
    }
    let timestamp = now_ms()?;
    authorization
        .revalidate(&record, &plan, action, timestamp)
        .map_err(|cause| before_dispatch_message(cause.message))?;

    let mut prepared = record.clone();
    prepared.state = "execution-prepared".into();
    prepared.allowed_actions.clear();
    prepared.generation = record.generation.saturating_add(1);
    prepared.updated_at_ms = timestamp;
    prepared.execution = Some(GitWorkflowExecutionAttempt {
        schema_version: ATTEMPT_SCHEMA_VERSION.into(),
        authorization_id: authorization.authorization_id().into(),
        requirement_hash: authorization.requirement().requirement_hash.clone(),
        action: action_name(action).into(),
        phase: "prepared".into(),
        source_generation: record.generation,
        started_at_ms: timestamp,
        updated_at_ms: timestamp,
        command_exit_code: None,
        outcome: None,
    });
    workflow_store
        .transition(&record, &prepared)
        .map_err(before_dispatch)?;
    event_store
        .append_git_workflow_event(&prepared, "git.workflow.prepared")
        .map_err(|cause| after_journal(cause, false))?;

    let dispatch_timestamp = now_ms()?;
    let mut dispatching = prepared.clone();
    dispatching.state = "execution-dispatching".into();
    dispatching.generation = prepared.generation.saturating_add(1);
    dispatching.updated_at_ms = dispatch_timestamp;
    let attempt = dispatching
        .execution
        .as_mut()
        .ok_or_else(|| before_dispatch_message("Git workflow execution journal is missing"))?;
    attempt.phase = "dispatching".into();
    attempt.updated_at_ms = dispatch_timestamp;
    workflow_store
        .transition(&prepared, &dispatching)
        .map_err(before_dispatch)?;
    event_store
        .append_git_workflow_event(&dispatching, "git.workflow.dispatching")
        .map_err(|cause| after_journal(cause, false))?;

    let runner = GitRunner::new(workflow_store.repository_root()).map_err(after_dispatch_git)?;
    let output = dispatch_command(&runner, &record, action);
    let exit_code = match output {
        Ok(output) if output.success => Some(output.code.unwrap_or(0)),
        Ok(output) => Some(output.code.unwrap_or(-1)),
        Err(_) => None,
    };
    let observed = workflow_store
        .observe_execution(&dispatching, exit_code, false)
        .map_err(after_dispatch)?;
    event_store
        .append_git_workflow_event(&observed, terminal_event_kind(&observed))
        .map_err(|cause| after_journal(cause, true))?;
    Ok(result_from_record(&observed, action))
}

pub fn recover_git_workflow_execution(
    workflow_store: &GitWorkflowStore,
    event_store: &mut WorkbenchStore,
    operation_id: &str,
) -> Result<GitWorkflowExecutionResult, GitWorkflowExecutionError> {
    let record = workflow_store.load(operation_id).map_err(before_dispatch)?;
    if !matches!(
        record.state.as_str(),
        "execution-prepared" | "execution-dispatching"
    ) {
        return Err(before_dispatch_message(
            "Git workflow record does not require execution recovery",
        ));
    }
    let action = match record
        .execution
        .as_ref()
        .map(|attempt| attempt.action.as_str())
    {
        Some("start") => GitWorkflowAuthorizedAction::Start,
        Some("abort") => GitWorkflowAuthorizedAction::Abort,
        Some("continue") => GitWorkflowAuthorizedAction::Continue,
        _ => {
            return Err(before_dispatch_message(
                "Git workflow recovery action is invalid",
            ))
        }
    };
    let observed = workflow_store
        .observe_execution(&record, None, true)
        .map_err(after_dispatch)?;
    event_store
        .append_git_workflow_event(&observed, "git.workflow.recovered")
        .map_err(|cause| after_journal(cause, false))?;
    Ok(result_from_record(&observed, action))
}

fn current_action_plan(
    store: &GitWorkflowStore,
    record: &GitWorkflowRecord,
    action: GitWorkflowAuthorizedAction,
    pending_editor_paths: &HashSet<String>,
) -> Result<GitWorkflowPlan, GitWorkflowExecutionError> {
    let request = match action {
        GitWorkflowAuthorizedAction::Start => record.request.clone(),
        GitWorkflowAuthorizedAction::Abort => GitWorkflowRequest::Abort {
            operation_id: record.operation_id.clone(),
            generation: record.generation,
        },
        GitWorkflowAuthorizedAction::Continue => GitWorkflowRequest::Continue {
            operation_id: record.operation_id.clone(),
            generation: record.generation,
        },
    };
    plan_git_workflow(
        store.repository_root(),
        request,
        pending_editor_paths,
        (!matches!(action, GitWorkflowAuthorizedAction::Start)).then_some(record),
    )
    .map_err(before_dispatch)
}

fn dispatch_command(
    runner: &GitRunner,
    record: &GitWorkflowRecord,
    action: GitWorkflowAuthorizedAction,
) -> Result<GitOutput, GitStatusError> {
    match action {
        GitWorkflowAuthorizedAction::Start => dispatch_start(runner, record),
        GitWorkflowAuthorizedAction::Abort => {
            let args = match record.operation_kind.as_str() {
                "merge" => ["merge", "--abort"],
                "rebase" => ["rebase", "--abort"],
                "cherry-pick" => ["cherry-pick", "--abort"],
                _ => return invalid_git_result("Git workflow operation cannot be aborted"),
            };
            runner.run(&args, None, MAX_GIT_OUTPUT)
        }
        GitWorkflowAuthorizedAction::Continue => dispatch_continue(runner, record),
    }
}

fn dispatch_start(
    runner: &GitRunner,
    record: &GitWorkflowRecord,
) -> Result<GitOutput, GitStatusError> {
    match &record.request {
        GitWorkflowRequest::StashCapture {
            include_untracked,
            message,
            identity,
            ..
        } => {
            let mut args = vec!["stash", "push"];
            if *include_untracked {
                args.push("--include-untracked");
            }
            args.extend(["--message", message.as_str(), "--"]);
            run_with_identity(runner, &args, identity, true)
        }
        GitWorkflowRequest::Merge {
            target_oid,
            mode,
            commit,
            ..
        } => {
            let mut args = vec![
                "merge",
                "--no-edit",
                "--no-stat",
                "--no-progress",
                "--no-autostash",
                "--no-rerere-autoupdate",
                "--no-verify-signatures",
                "--no-verify",
            ];
            args.push(match mode {
                GitMergeMode::AllowFastForward => "--ff",
                GitMergeMode::NoFastForward => "--no-ff",
                GitMergeMode::FastForwardOnly => "--ff-only",
            });
            if let Some(metadata) = commit {
                args.extend(["--message", metadata.message.as_str()]);
            }
            args.push(target_oid);
            if let Some(metadata) = commit {
                run_with_identity(runner, &args, &metadata.identity, true)
            } else {
                runner.run(&args, None, MAX_GIT_OUTPUT)
            }
        }
        GitWorkflowRequest::Rebase {
            upstream_oid,
            onto_oid,
            committer,
            ..
        } => run_with_identity(
            runner,
            &[
                "rebase",
                "--no-autostash",
                "--no-rerere-autoupdate",
                "--no-verify",
                "--onto",
                onto_oid,
                upstream_oid,
            ],
            committer,
            false,
        ),
        GitWorkflowRequest::CherryPick {
            commit_oid,
            committer,
            ..
        } => run_with_identity(
            runner,
            &[
                "cherry-pick",
                "--no-edit",
                "--no-rerere-autoupdate",
                commit_oid,
            ],
            committer,
            false,
        ),
        GitWorkflowRequest::Abort { .. } | GitWorkflowRequest::Continue { .. } => {
            invalid_git_result("persisted Git workflow request cannot be executed")
        }
    }
}

fn dispatch_continue(
    runner: &GitRunner,
    record: &GitWorkflowRecord,
) -> Result<GitOutput, GitStatusError> {
    match &record.request {
        GitWorkflowRequest::Merge {
            commit: Some(metadata),
            ..
        } => run_with_identity(runner, &["merge", "--continue"], &metadata.identity, true),
        GitWorkflowRequest::Rebase { committer, .. } => {
            run_with_identity(runner, &["rebase", "--continue"], committer, false)
        }
        GitWorkflowRequest::CherryPick { committer, .. } => {
            run_with_identity(runner, &["cherry-pick", "--continue"], committer, false)
        }
        _ => invalid_git_result("Git workflow operation cannot be continued"),
    }
}

fn run_with_identity(
    runner: &GitRunner,
    args: &[&str],
    identity: &GitCommitIdentity,
    include_author: bool,
) -> Result<GitOutput, GitStatusError> {
    let date = identity_date(identity);
    let environment = GitWorkflowEnvironment {
        author_name: include_author.then_some(identity.name.as_str()),
        author_email: include_author.then_some(identity.email.as_str()),
        author_date: include_author.then_some(date.as_str()),
        committer_name: &identity.name,
        committer_email: &identity.email,
        committer_date: &date,
    };
    runner.run_with_workflow_environment(args, None, MAX_GIT_OUTPUT, &environment)
}

fn invalid_git_result(message: &str) -> Result<GitOutput, GitStatusError> {
    Err(GitStatusError {
        code: -32040,
        message: message.into(),
    })
}

fn result_from_record(
    record: &GitWorkflowRecord,
    action: GitWorkflowAuthorizedAction,
) -> GitWorkflowExecutionResult {
    let attempt = record.execution.as_ref();
    GitWorkflowExecutionResult {
        schema_version: RESULT_SCHEMA_VERSION.into(),
        operation_id: record.operation_id.clone(),
        action,
        authorization_id: attempt
            .map(|attempt| attempt.authorization_id.clone())
            .unwrap_or_default(),
        command_exit_code: attempt.and_then(|attempt| attempt.command_exit_code),
        state: record.state.clone(),
        generation: record.generation,
        observed_head: record.observed_head.clone(),
        observed_operation: record.observed_operation.clone(),
        outcome: attempt
            .and_then(|attempt| attempt.outcome.clone())
            .unwrap_or_else(|| "unknown".into()),
    }
}

fn terminal_event_kind(record: &GitWorkflowRecord) -> &'static str {
    match record.state.as_str() {
        "completed" => "git.workflow.completed",
        "aborted" => "git.workflow.aborted",
        "conflicted" => "git.workflow.conflicted",
        "in-progress" => "git.workflow.in-progress",
        _ => "git.workflow.failed",
    }
}

fn action_name(action: GitWorkflowAuthorizedAction) -> &'static str {
    match action {
        GitWorkflowAuthorizedAction::Start => "start",
        GitWorkflowAuthorizedAction::Abort => "abort",
        GitWorkflowAuthorizedAction::Continue => "continue",
    }
}

fn now_ms() -> Result<u64, GitWorkflowExecutionError> {
    let milliseconds = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| before_dispatch_message("system clock is before the Unix epoch"))?
        .as_millis();
    u64::try_from(milliseconds).map_err(|_| before_dispatch_message("system clock is out of range"))
}

fn before_dispatch(cause: impl IntoWorkflowMessage) -> GitWorkflowExecutionError {
    before_dispatch_message(cause.into_message())
}

fn before_dispatch_message(message: impl Into<String>) -> GitWorkflowExecutionError {
    GitWorkflowExecutionError {
        message: message.into(),
        command_dispatched: false,
        recovery_required: false,
    }
}

fn after_dispatch(cause: impl IntoWorkflowMessage) -> GitWorkflowExecutionError {
    GitWorkflowExecutionError {
        message: cause.into_message(),
        command_dispatched: true,
        recovery_required: true,
    }
}

fn after_dispatch_git(cause: GitStatusError) -> GitWorkflowExecutionError {
    after_dispatch(cause)
}

fn after_journal(
    cause: WorkbenchStoreError,
    command_dispatched: bool,
) -> GitWorkflowExecutionError {
    GitWorkflowExecutionError {
        message: cause.message,
        command_dispatched,
        recovery_required: true,
    }
}

trait IntoWorkflowMessage {
    fn into_message(self) -> String;
}

impl IntoWorkflowMessage for crate::git_workflow_state::GitWorkflowError {
    fn into_message(self) -> String {
        self.message
    }
}

impl IntoWorkflowMessage for GitStatusError {
    fn into_message(self) -> String {
        self.message
    }
}

impl IntoWorkflowMessage for String {
    fn into_message(self) -> String {
        self
    }
}

impl IntoWorkflowMessage for &str {
    fn into_message(self) -> String {
        self.into()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::git_commit_transaction::{
        GitCommitHookPolicy, GitCommitIdentity, GitCommitMessageSource, GitCommitSigningPolicy,
    };
    use crate::git_workflow_authorization::{
        authorization_requirement, verify_git_workflow_authorization,
        GitWorkflowAuthorizationAuthority, GitWorkflowAuthorizationError,
        GitWorkflowAuthorizationEvidence, GitWorkflowDecisionReference,
    };
    use crate::git_workflow_state::{GitWorkflowCommitMetadata, GitWorkflowStore};
    use crate::workspace_edit::ContentHash;
    use std::fs;
    use std::path::{Path, PathBuf};
    use std::process::Command;
    use std::sync::atomic::{AtomicU64, Ordering};

    static SEQUENCE: AtomicU64 = AtomicU64::new(0);

    struct Repository {
        parent: PathBuf,
        root: PathBuf,
        storage: PathBuf,
    }

    impl Repository {
        fn new(label: &str) -> Self {
            let sequence = SEQUENCE.fetch_add(1, Ordering::Relaxed);
            let parent = std::env::temp_dir().join(format!(
                "aegisy-git-execution-{label}-{}-{sequence}",
                std::process::id()
            ));
            let root = parent.join("repository");
            let storage = parent.join("storage");
            fs::create_dir_all(&root).unwrap();
            fs::create_dir_all(&storage).unwrap();
            git(&root, &["init"], true);
            git(&root, &["branch", "-M", "main"], true);
            git(&root, &["config", "user.name", "Fixture User"], true);
            git(
                &root,
                &["config", "user.email", "fixture@example.invalid"],
                true,
            );
            write_commit(&root, "base.txt", "base\n", "base");
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

    impl Drop for Repository {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.parent);
        }
    }

    #[derive(Default)]
    struct Authority;

    impl GitWorkflowAuthorizationAuthority for Authority {
        fn consume_once(
            &mut self,
            _requirement: &crate::git_workflow_authorization::GitWorkflowAuthorizationRequirement,
            _evidence: &GitWorkflowAuthorizationEvidence,
            _observed_at_ms: u64,
        ) -> Result<(), GitWorkflowAuthorizationError> {
            Ok(())
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

    fn write_commit(root: &Path, path: &str, content: &str, message: &str) -> String {
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

    fn identity() -> GitCommitIdentity {
        GitCommitIdentity {
            name: "Aegisy Workflow".into(),
            email: "workflow@example.invalid".into(),
            source: "explicit".into(),
            timestamp_seconds: 1_800_000_000,
            timezone: "+0800".into(),
        }
    }

    fn metadata(message: &str) -> GitWorkflowCommitMetadata {
        GitWorkflowCommitMetadata {
            message: message.into(),
            message_source: GitCommitMessageSource::User,
            identity: identity(),
        }
    }

    fn feature_target(repository: &Repository, path: &str, content: &str) -> String {
        git(&repository.root, &["switch", "-c", "feature"], true);
        let target = write_commit(&repository.root, path, content, "feature change");
        git(&repository.root, &["switch", "main"], true);
        target
    }

    fn plan_start(repository: &Repository, request: GitWorkflowRequest) -> GitWorkflowPlan {
        plan_git_workflow(&repository.root, request, &HashSet::new(), None).unwrap()
    }

    fn persist(
        repository: &Repository,
        operation_id: &str,
        plan: &GitWorkflowPlan,
    ) -> (GitWorkflowStore, WorkbenchStore, GitWorkflowRecord) {
        let store = repository.store();
        let event_store = WorkbenchStore::open(&repository.storage).unwrap();
        let record = store
            .create_planned(operation_id, "project-1", "session-1", plan)
            .unwrap();
        (store, event_store, record)
    }

    fn run(
        store: &GitWorkflowStore,
        event_store: &mut WorkbenchStore,
        authorization: VerifiedGitWorkflowAuthorization,
        action: GitWorkflowAuthorizedAction,
    ) -> Result<GitWorkflowExecutionResult, GitWorkflowExecutionError> {
        execute_git_workflow(store, event_store, authorization, action, &HashSet::new())
    }

    fn authorize(
        record: &GitWorkflowRecord,
        plan: &GitWorkflowPlan,
        action: GitWorkflowAuthorizedAction,
    ) -> VerifiedGitWorkflowAuthorization {
        let requirement = authorization_requirement(record, plan, action).unwrap();
        let now = now_ms().unwrap();
        let sequence = SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let decision = |kind: &str| GitWorkflowDecisionReference {
            authority_id: "fixture-authority".into(),
            decision_id: format!("{kind}-{sequence}"),
            scope: "allow-once".into(),
            scope_hash: requirement.requirement_hash.clone(),
            issued_at_ms: now.saturating_sub(1),
            expires_at_ms: now + 60_000,
        };
        let evidence = GitWorkflowAuthorizationEvidence {
            schema_version: "git-workflow-authorization-evidence/0.1".into(),
            authorization_id: format!("authorization-{sequence}"),
            requirement_hash: requirement.requirement_hash.clone(),
            permission: decision("permission"),
            explicit_approval: requirement
                .risk
                .requires_explicit_approval
                .then(|| decision("approval")),
        };
        verify_git_workflow_authorization(&mut Authority, requirement, &evidence, now).unwrap()
    }

    fn action_plan(
        repository: &Repository,
        record: &GitWorkflowRecord,
        action: GitWorkflowAuthorizedAction,
    ) -> GitWorkflowPlan {
        let request = match action {
            GitWorkflowAuthorizedAction::Start => record.request.clone(),
            GitWorkflowAuthorizedAction::Abort => GitWorkflowRequest::Abort {
                operation_id: record.operation_id.clone(),
                generation: record.generation,
            },
            GitWorkflowAuthorizedAction::Continue => GitWorkflowRequest::Continue {
                operation_id: record.operation_id.clone(),
                generation: record.generation,
            },
        };
        plan_git_workflow(
            &repository.root,
            request,
            &HashSet::new(),
            (!matches!(action, GitWorkflowAuthorizedAction::Start)).then_some(record),
        )
        .unwrap()
    }

    #[test]
    fn executes_fast_forward_merge_with_exact_authorized_target() {
        let repository = Repository::new("fast-forward");
        let target = feature_target(&repository, "feature.txt", "feature\n");
        let plan = plan_start(
            &repository,
            GitWorkflowRequest::Merge {
                target_oid: target.clone(),
                mode: GitMergeMode::FastForwardOnly,
                commit: None,
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
        );
        let (store, mut event_store, record) = persist(&repository, "fast-forward", &plan);
        let authorization = authorize(&record, &plan, GitWorkflowAuthorizedAction::Start);
        let result = run(
            &store,
            &mut event_store,
            authorization,
            GitWorkflowAuthorizedAction::Start,
        )
        .unwrap();
        assert_eq!(result.state, "completed");
        assert_eq!(result.command_exit_code, Some(0));
        assert_eq!(head(&repository.root), target);
    }

    #[test]
    fn executes_merge_commit_and_bypasses_untrusted_hook_by_reviewed_policy() {
        let repository = Repository::new("merge-commit");
        let target = feature_target(&repository, "feature.txt", "feature\n");
        let marker = repository.parent.join("hook-ran");
        let hook = repository.root.join(".git/hooks/pre-merge-commit");
        fs::write(
            &hook,
            format!("#!/bin/sh\nprintf ran > '{}'\nexit 1\n", marker.display()),
        )
        .unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&hook, fs::Permissions::from_mode(0o700)).unwrap();
        }
        let plan = plan_start(
            &repository,
            GitWorkflowRequest::Merge {
                target_oid: target.clone(),
                mode: GitMergeMode::NoFastForward,
                commit: Some(metadata("Merge feature safely")),
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
        );
        let (store, mut event_store, record) = persist(&repository, "merge-commit", &plan);
        let authorization = authorize(&record, &plan, GitWorkflowAuthorizedAction::Start);
        let result = run(
            &store,
            &mut event_store,
            authorization,
            GitWorkflowAuthorizedAction::Start,
        )
        .unwrap();
        assert_eq!(result.state, "completed");
        assert!(!marker.exists());
        let parents = String::from_utf8(git(
            &repository.root,
            &["rev-list", "--parents", "-n", "1", "HEAD"],
            true,
        ))
        .unwrap();
        assert_eq!(parents.split_ascii_whitespace().count(), 3);
    }

    #[test]
    fn executes_stash_cherry_pick_and_rebase_with_bound_identity() {
        let stash_repository = Repository::new("stash");
        fs::write(stash_repository.root.join("dirty.txt"), "dirty\n").unwrap();
        let stash_plan = plan_start(
            &stash_repository,
            GitWorkflowRequest::StashCapture {
                include_untracked: true,
                message: "Reviewed stash".into(),
                message_source: GitCommitMessageSource::User,
                identity: identity(),
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
        );
        let (stash_store, mut stash_events, stash_record) =
            persist(&stash_repository, "stash", &stash_plan);
        let stash_result = run(
            &stash_store,
            &mut stash_events,
            authorize(
                &stash_record,
                &stash_plan,
                GitWorkflowAuthorizedAction::Start,
            ),
            GitWorkflowAuthorizedAction::Start,
        )
        .unwrap();
        assert_eq!(stash_result.state, "completed");
        assert!(!stash_repository.root.join("dirty.txt").exists());

        let cherry_repository = Repository::new("cherry");
        let target = feature_target(&cherry_repository, "picked.txt", "picked\n");
        let cherry_plan = plan_start(
            &cherry_repository,
            GitWorkflowRequest::CherryPick {
                commit_oid: target,
                committer: identity(),
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
        );
        let (cherry_store, mut cherry_events, cherry_record) =
            persist(&cherry_repository, "cherry", &cherry_plan);
        let cherry_result = run(
            &cherry_store,
            &mut cherry_events,
            authorize(
                &cherry_record,
                &cherry_plan,
                GitWorkflowAuthorizedAction::Start,
            ),
            GitWorkflowAuthorizedAction::Start,
        )
        .unwrap();
        assert_eq!(cherry_result.state, "completed");
        assert_eq!(
            fs::read_to_string(cherry_repository.root.join("picked.txt")).unwrap(),
            "picked\n"
        );

        let rebase_repository = Repository::new("rebase");
        let base = head(&rebase_repository.root);
        let onto = feature_target(&rebase_repository, "onto.txt", "onto\n");
        git(&rebase_repository.root, &["switch", "-c", "topic"], true);
        write_commit(&rebase_repository.root, "topic.txt", "topic\n", "topic");
        let rebase_plan = plan_start(
            &rebase_repository,
            GitWorkflowRequest::Rebase {
                upstream_oid: base,
                onto_oid: onto.clone(),
                committer: identity(),
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
        );
        let (rebase_store, mut rebase_events, rebase_record) =
            persist(&rebase_repository, "rebase", &rebase_plan);
        let rebase_result = run(
            &rebase_store,
            &mut rebase_events,
            authorize(
                &rebase_record,
                &rebase_plan,
                GitWorkflowAuthorizedAction::Start,
            ),
            GitWorkflowAuthorizedAction::Start,
        )
        .unwrap();
        assert_eq!(rebase_result.state, "completed");
        git(
            &rebase_repository.root,
            &["merge-base", "--is-ancestor", &onto, "HEAD"],
            true,
        );
    }

    fn conflict_fixture(label: &str) -> (Repository, String, GitWorkflowPlan) {
        let repository = Repository::new(label);
        write_commit(&repository.root, "conflict.txt", "base\n", "conflict base");
        let target = feature_target(&repository, "conflict.txt", "feature\n");
        write_commit(&repository.root, "conflict.txt", "main\n", "main conflict");
        let plan = plan_start(
            &repository,
            GitWorkflowRequest::Merge {
                target_oid: target.clone(),
                mode: GitMergeMode::NoFastForward,
                commit: Some(metadata("Merge conflicting feature")),
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
        );
        (repository, target, plan)
    }

    #[test]
    fn executes_conflict_continue_and_abort_as_separate_authorized_actions() {
        let (continue_repository, _, continue_plan) = conflict_fixture("continue");
        let (continue_store, mut continue_events, continue_record) =
            persist(&continue_repository, "continue", &continue_plan);
        let conflicted = run(
            &continue_store,
            &mut continue_events,
            authorize(
                &continue_record,
                &continue_plan,
                GitWorkflowAuthorizedAction::Start,
            ),
            GitWorkflowAuthorizedAction::Start,
        )
        .unwrap();
        assert_eq!(conflicted.state, "conflicted");
        fs::write(continue_repository.root.join("conflict.txt"), "resolved\n").unwrap();
        git(&continue_repository.root, &["add", "conflict.txt"], true);
        let ready = continue_store.reconcile("continue").unwrap();
        assert_eq!(ready.state, "in-progress");
        let continue_action_plan = action_plan(
            &continue_repository,
            &ready,
            GitWorkflowAuthorizedAction::Continue,
        );
        let continued = run(
            &continue_store,
            &mut continue_events,
            authorize(
                &ready,
                &continue_action_plan,
                GitWorkflowAuthorizedAction::Continue,
            ),
            GitWorkflowAuthorizedAction::Continue,
        )
        .unwrap();
        assert_eq!(continued.state, "completed");

        let (abort_repository, _, abort_plan_start) = conflict_fixture("abort");
        let base = head(&abort_repository.root);
        let (abort_store, mut abort_events, abort_record) =
            persist(&abort_repository, "abort", &abort_plan_start);
        run(
            &abort_store,
            &mut abort_events,
            authorize(
                &abort_record,
                &abort_plan_start,
                GitWorkflowAuthorizedAction::Start,
            ),
            GitWorkflowAuthorizedAction::Start,
        )
        .unwrap();
        let conflicted = abort_store.load("abort").unwrap();
        let abort_action_plan = action_plan(
            &abort_repository,
            &conflicted,
            GitWorkflowAuthorizedAction::Abort,
        );
        assert_eq!(abort_action_plan.risk.class, "high");
        let aborted = run(
            &abort_store,
            &mut abort_events,
            authorize(
                &conflicted,
                &abort_action_plan,
                GitWorkflowAuthorizedAction::Abort,
            ),
            GitWorkflowAuthorizedAction::Abort,
        )
        .unwrap();
        assert_eq!(aborted.state, "aborted");
        assert_eq!(head(&abort_repository.root), base);
    }

    #[test]
    fn restart_recovery_adopts_matching_conflict_and_marks_unknown_dispatch() {
        let (repository, target, plan) = conflict_fixture("recover-conflict");
        let (store, mut events, record) = persist(&repository, "recover-conflict", &plan);
        let dispatching = journal_record(&store, &record, "start", "dispatching");
        git(
            &repository.root,
            &["merge", "--no-commit", "--no-ff", &target],
            false,
        );
        let recovered =
            recover_git_workflow_execution(&store, &mut events, "recover-conflict").unwrap();
        assert_eq!(recovered.state, "conflicted");
        assert_eq!(recovered.outcome, "conflict-observed");
        assert!(dispatching.generation < recovered.generation);
        git(&repository.root, &["merge", "--abort"], true);

        let clean_repository = Repository::new("recover-unknown");
        let target = feature_target(&clean_repository, "feature.txt", "feature\n");
        let clean_plan = plan_start(
            &clean_repository,
            GitWorkflowRequest::Merge {
                target_oid: target,
                mode: GitMergeMode::FastForwardOnly,
                commit: None,
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
        );
        let (clean_store, mut clean_events, clean_record) =
            persist(&clean_repository, "recover-unknown", &clean_plan);
        journal_record(&clean_store, &clean_record, "start", "prepared");
        let unknown =
            recover_git_workflow_execution(&clean_store, &mut clean_events, "recover-unknown")
                .unwrap();
        assert_eq!(unknown.state, "interrupted-needs-reconciliation");
        assert_eq!(unknown.outcome, "command-outcome-unknown");

        let completed_repository = Repository::new("recover-completed");
        let target = feature_target(&completed_repository, "feature.txt", "feature\n");
        let completed_plan = plan_start(
            &completed_repository,
            GitWorkflowRequest::Merge {
                target_oid: target.clone(),
                mode: GitMergeMode::FastForwardOnly,
                commit: None,
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
        );
        let (completed_store, mut completed_events, completed_record) =
            persist(&completed_repository, "recover-completed", &completed_plan);
        journal_record(&completed_store, &completed_record, "start", "dispatching");
        git(
            &completed_repository.root,
            &["merge", "--ff-only", &target],
            true,
        );
        let completed = recover_git_workflow_execution(
            &completed_store,
            &mut completed_events,
            "recover-completed",
        )
        .unwrap();
        assert_eq!(completed.state, "completed");
        assert_eq!(completed.outcome, "completion-recovered-and-verified");
    }

    fn journal_record(
        store: &GitWorkflowStore,
        record: &GitWorkflowRecord,
        action: &str,
        phase: &str,
    ) -> GitWorkflowRecord {
        let timestamp = now_ms().unwrap();
        let mut next = record.clone();
        next.state = if phase == "prepared" {
            "execution-prepared"
        } else {
            "execution-dispatching"
        }
        .into();
        next.allowed_actions.clear();
        next.generation += 1;
        next.updated_at_ms = timestamp;
        next.execution = Some(GitWorkflowExecutionAttempt {
            schema_version: ATTEMPT_SCHEMA_VERSION.into(),
            authorization_id: format!("recovery-{}", SEQUENCE.fetch_add(1, Ordering::Relaxed)),
            requirement_hash: ContentHash::for_bytes(b"recovery-scope"),
            action: action.into(),
            phase: phase.into(),
            source_generation: record.generation,
            started_at_ms: timestamp,
            updated_at_ms: timestamp,
            command_exit_code: None,
            outcome: None,
        });
        store.transition(record, &next).unwrap();
        next
    }

    #[test]
    fn authorization_or_pending_editor_failure_happens_before_git_mutation() {
        let repository = Repository::new("stale-authorization");
        let target = feature_target(&repository, "feature.txt", "feature\n");
        let plan = plan_start(
            &repository,
            GitWorkflowRequest::Merge {
                target_oid: target,
                mode: GitMergeMode::FastForwardOnly,
                commit: None,
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
        );
        let (store, mut events, record) = persist(&repository, "stale", &plan);
        let authorization = authorize(&record, &plan, GitWorkflowAuthorizedAction::Start);
        let original_head = head(&repository.root);
        let mut pending = HashSet::new();
        pending.insert("unsaved.txt".into());
        let failure = execute_git_workflow(
            &store,
            &mut events,
            authorization,
            GitWorkflowAuthorizedAction::Start,
            &pending,
        )
        .unwrap_err();
        assert!(!failure.command_dispatched);
        assert_eq!(head(&repository.root), original_head);
        assert_eq!(store.load("stale").unwrap().state, "planned");
    }
}
