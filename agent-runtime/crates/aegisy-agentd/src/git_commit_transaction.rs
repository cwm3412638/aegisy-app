use crate::git_checkpoint::{GitCheckpoint, GitCheckpointApplication};
use crate::git_staging::{
    verify_agent_staging_result, GitStagePlan, GitStageResult, GitStagedPath,
};
use crate::git_status::{GitCommitEnvironment, GitOutput, GitRunner, GitStatusError};
use crate::workspace::is_sensitive_path;
use crate::workspace_edit::ContentHash;
use serde::{Deserialize, Serialize};
use std::collections::HashSet;
use std::fs;
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

const PLAN_SCHEMA_VERSION: &str = "git-commit-plan/0.1";
const RESULT_SCHEMA_VERSION: &str = "git-commit-result/0.1";
const MAX_MESSAGE_BYTES: usize = 16 * 1024;
const MAX_DIFF_BYTES: u64 = 2 * 1024 * 1024;
const MAX_CHANGED_PATHS: usize = 2_000;
const MAX_GIT_OUTPUT: u64 = 2 * 1024 * 1024;
const MAX_TIMESTAMP: i64 = 4_102_444_800;
const KNOWN_HOOKS: &[&str] = &[
    "pre-commit",
    "prepare-commit-msg",
    "commit-msg",
    "post-commit",
    "reference-transaction",
];

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GitCommitRequest {
    pub message: String,
    pub message_source: GitCommitMessageSource,
    pub author: GitCommitIdentity,
    pub committer: GitCommitIdentity,
    pub hook_policy: GitCommitHookPolicy,
    pub signing_policy: GitCommitSigningPolicy,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub enum GitCommitMessageSource {
    User,
    AgentGenerated { generator_id: String },
    Template { template_id: String },
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GitCommitIdentity {
    pub name: String,
    pub email: String,
    pub source: String,
    pub timestamp_seconds: i64,
    pub timezone: String,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum GitCommitHookPolicy {
    Disabled,
    Run,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum GitCommitSigningPolicy {
    Unsigned,
    Sign,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitCommitPlan {
    pub schema_version: String,
    pub checkpoint_id: String,
    pub edit_id: String,
    pub project_id: String,
    pub root_identity: String,
    pub repository_root: String,
    pub expected_head: String,
    pub expected_branch: Option<String>,
    pub target_ref: String,
    pub head_tree_oid: String,
    pub previous_index_tree_oid: String,
    pub staged_index_tree_oid: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub commit_tree_oid: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub commit_oid: Option<String>,
    pub selected_paths: Vec<GitCommitSelectedPath>,
    pub excluded_user_paths: Vec<String>,
    pub excluded_sensitive_path_count: usize,
    pub changed_paths: Vec<String>,
    pub additions: usize,
    pub deletions: usize,
    pub binary_paths: usize,
    pub patch: String,
    pub patch_hash: ContentHash,
    pub request: GitCommitRequest,
    pub committer_behavior: String,
    pub detected_hooks: Vec<GitCommitHook>,
    pub hook_behavior: String,
    pub signing_behavior: String,
    pub blocking_reasons: Vec<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitCommitSelectedPath {
    pub operation: String,
    pub path: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub from_path: Option<String>,
    pub whole_path: bool,
    pub hunk_ids: Vec<String>,
    pub mode_included: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitCommitHook {
    pub name: String,
    pub state: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitCommitResult {
    pub schema_version: String,
    pub event_kind: String,
    pub observed_at_ms: u64,
    pub operation: String,
    pub checkpoint_id: String,
    pub commit_oid: String,
    pub parent_oid: String,
    pub tree_oid: String,
    pub target_ref: String,
    pub branch: Option<String>,
    pub index_tree_oid: String,
    pub message_hash: ContentHash,
    pub message_source: GitCommitMessageSource,
    pub author: GitCommitIdentity,
    pub committer: GitCommitIdentity,
    pub hook_outcome: String,
    pub signing_outcome: String,
    pub selected_path_count: usize,
    pub excluded_user_path_count: usize,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GitCommitError {
    pub message: String,
    pub rollback_complete: bool,
}

pub fn plan_agent_commit(
    checkpoint: &GitCheckpoint,
    application: &GitCheckpointApplication,
    stage_plan: &GitStagePlan,
    stage_result: &GitStageResult,
    request: GitCommitRequest,
) -> Result<GitCommitPlan, GitCommitError> {
    verify_agent_staging_result(checkpoint, application, stage_plan, stage_result)
        .map_err(from_stage)?;
    let request = validate_request(request)?;
    let root = checkpoint.repository_root.as_path();
    let root_text = path_to_utf8(root, "Git repository root")?;
    let expected_head = checkpoint
        .head_oid
        .clone()
        .ok_or_else(|| error("Git commit requires an existing HEAD"))?;
    let runner = GitRunner::new(root).map_err(from_git)?;
    let head_tree_oid = required_oid(
        &runner,
        &["rev-parse", &format!("{expected_head}^{{tree}}")],
    )?;
    let target_ref = symbolic_head(&runner)?.unwrap_or_else(|| "HEAD".into());
    let mut blocking_reasons = Vec::new();
    let detected_hooks = detect_hooks(&runner, &checkpoint.git_directory)?;
    let hook_behavior = match request.hook_policy {
        GitCommitHookPolicy::Disabled => "bypassed-by-reviewed-policy",
        GitCommitHookPolicy::Run => {
            blocking_reasons.push("hook-execution-gate-unavailable".into());
            "blocked-requires-sandbox-permission-approval"
        }
    }
    .to_owned();
    let signing_behavior = match request.signing_policy {
        GitCommitSigningPolicy::Unsigned => "unsigned-by-reviewed-policy",
        GitCommitSigningPolicy::Sign => {
            blocking_reasons.push("commit-signing-gate-unavailable".into());
            "blocked-requires-secure-signer-approval"
        }
    }
    .to_owned();

    let selected_paths = stage_result
        .staged
        .iter()
        .map(selected_path)
        .collect::<Vec<_>>();
    let selected_path_set = selected_paths
        .iter()
        .flat_map(|path| path.from_path.iter().chain(std::iter::once(&path.path)))
        .cloned()
        .collect::<HashSet<_>>();
    let mut selected_path_values = selected_path_set.iter().cloned().collect::<Vec<_>>();
    selected_path_values.sort();
    if custom_merge_driver_is_configured(&runner, &selected_path_values)? {
        blocking_reasons.push("custom-merge-driver-requires-execution-gate".into());
    }
    let excluded = changed_paths_between(
        &runner,
        &head_tree_oid,
        &stage_result.previous_index_tree_oid,
    )?;
    let mut excluded_user_paths = Vec::new();
    let mut excluded_sensitive_path_count = 0_usize;
    for path in excluded {
        if is_sensitive_path(Path::new(&path)) {
            excluded_sensitive_path_count += 1;
        } else {
            excluded_user_paths.push(path);
        }
    }
    excluded_user_paths.sort();
    excluded_user_paths.dedup();

    let commit_tree_oid = if blocking_reasons
        .iter()
        .any(|reason| reason == "custom-merge-driver-requires-execution-gate")
    {
        None
    } else {
        match merge_agent_tree(
            &runner,
            &stage_result.previous_index_tree_oid,
            &head_tree_oid,
            &stage_result.index_tree_oid,
        ) {
            Ok(tree) => Some(tree),
            Err(_) => {
                blocking_reasons.push("agent-stage-conflicts-with-excluded-user-index".into());
                None
            }
        }
    };
    let mut changed_paths = Vec::new();
    let mut additions = 0_usize;
    let mut deletions = 0_usize;
    let mut binary_paths = 0_usize;
    let mut patch = String::new();
    if let Some(tree) = &commit_tree_oid {
        changed_paths = changed_paths_between(&runner, &head_tree_oid, tree)?;
        if changed_paths.is_empty() {
            blocking_reasons.push("agent-commit-tree-is-empty".into());
        }
        if changed_paths
            .iter()
            .any(|path| !selected_path_set.contains(path))
        {
            blocking_reasons.push("commit-tree-contains-unselected-path".into());
        }
        if changed_paths
            .iter()
            .any(|path| is_sensitive_path(Path::new(path)))
        {
            blocking_reasons.push("commit-tree-contains-sensitive-path".into());
        }
        let statistics = diff_statistics(&runner, &head_tree_oid, tree)?;
        additions = statistics.0;
        deletions = statistics.1;
        binary_paths = statistics.2;
        patch = exact_patch(&runner, &head_tree_oid, tree)?;
    }
    let patch_hash = ContentHash::for_bytes(patch.as_bytes());
    blocking_reasons.sort();
    blocking_reasons.dedup();
    let commit_oid = if blocking_reasons.is_empty() {
        let tree = commit_tree_oid
            .as_deref()
            .ok_or_else(|| error("Git commit tree is unavailable"))?;
        Some(create_commit_object(
            &runner,
            tree,
            &expected_head,
            &request,
        )?)
    } else {
        None
    };
    let committer_behavior = if request.author == request.committer {
        "same-as-author"
    } else {
        "explicit-committer"
    }
    .to_owned();
    Ok(GitCommitPlan {
        schema_version: PLAN_SCHEMA_VERSION.into(),
        checkpoint_id: checkpoint.checkpoint_id.clone(),
        edit_id: application.edit_id.clone(),
        project_id: application.project_id.clone(),
        root_identity: application.root_identity.clone(),
        repository_root: root_text,
        expected_head,
        expected_branch: checkpoint.branch.clone(),
        target_ref,
        head_tree_oid,
        previous_index_tree_oid: stage_result.previous_index_tree_oid.clone(),
        staged_index_tree_oid: stage_result.index_tree_oid.clone(),
        commit_tree_oid,
        commit_oid,
        selected_paths,
        excluded_user_paths,
        excluded_sensitive_path_count,
        changed_paths,
        additions,
        deletions,
        binary_paths,
        patch,
        patch_hash,
        request,
        committer_behavior,
        detected_hooks,
        hook_behavior,
        signing_behavior,
        blocking_reasons,
    })
}

pub fn execute_agent_commit(
    checkpoint: &GitCheckpoint,
    application: &GitCheckpointApplication,
    stage_plan: &GitStagePlan,
    stage_result: &GitStageResult,
    request: GitCommitRequest,
    plan: &GitCommitPlan,
) -> Result<GitCommitResult, GitCommitError> {
    execute_with_fault(
        checkpoint,
        application,
        stage_plan,
        stage_result,
        request,
        plan,
        &NoFault,
    )
}

trait FaultInjector {
    fn after_ref_update(&self) -> Result<(), GitCommitError> {
        Ok(())
    }
}

struct NoFault;

impl FaultInjector for NoFault {}

#[allow(clippy::too_many_arguments)]
fn execute_with_fault<F: FaultInjector>(
    checkpoint: &GitCheckpoint,
    application: &GitCheckpointApplication,
    stage_plan: &GitStagePlan,
    stage_result: &GitStageResult,
    request: GitCommitRequest,
    plan: &GitCommitPlan,
    fault: &F,
) -> Result<GitCommitResult, GitCommitError> {
    let current = plan_agent_commit(checkpoint, application, stage_plan, stage_result, request)?;
    if current != *plan {
        return Err(error("Git commit plan is stale or was modified"));
    }
    if !plan.blocking_reasons.is_empty() {
        return Err(error(format!(
            "Git commit is blocked: {}",
            plan.blocking_reasons.join(",")
        )));
    }
    let commit_oid = plan
        .commit_oid
        .as_deref()
        .ok_or_else(|| error("Git commit object is unavailable"))?;
    let tree_oid = plan
        .commit_tree_oid
        .as_deref()
        .ok_or_else(|| error("Git commit tree is unavailable"))?;
    let runner = GitRunner::new(&checkpoint.repository_root).map_err(from_git)?;
    update_ref(
        &runner,
        &plan.target_ref,
        commit_oid,
        &plan.expected_head,
        "aegisy-commit",
    )?;
    if let Err(cause) = fault.after_ref_update() {
        return Err(with_ref_rollback(cause, &runner, plan, commit_oid));
    }
    if let Err(cause) = verify_commit_result(&runner, plan, commit_oid, tree_oid, stage_result) {
        return Err(with_ref_rollback(cause, &runner, plan, commit_oid));
    }
    let observed_at_ms = match now_ms() {
        Ok(timestamp) => timestamp,
        Err(cause) => return Err(with_ref_rollback(cause, &runner, plan, commit_oid)),
    };
    Ok(GitCommitResult {
        schema_version: RESULT_SCHEMA_VERSION.into(),
        event_kind: "git.commit.completed".into(),
        observed_at_ms,
        operation: "committed".into(),
        checkpoint_id: plan.checkpoint_id.clone(),
        commit_oid: commit_oid.into(),
        parent_oid: plan.expected_head.clone(),
        tree_oid: tree_oid.into(),
        target_ref: plan.target_ref.clone(),
        branch: plan.expected_branch.clone(),
        index_tree_oid: stage_result.index_tree_oid.clone(),
        message_hash: ContentHash::for_bytes(plan.request.message.as_bytes()),
        message_source: plan.request.message_source.clone(),
        author: plan.request.author.clone(),
        committer: plan.request.committer.clone(),
        hook_outcome: plan.hook_behavior.clone(),
        signing_outcome: plan.signing_behavior.clone(),
        selected_path_count: plan.selected_paths.len(),
        excluded_user_path_count: plan
            .excluded_user_paths
            .len()
            .saturating_add(plan.excluded_sensitive_path_count),
    })
}

fn verify_commit_result(
    runner: &GitRunner,
    plan: &GitCommitPlan,
    commit_oid: &str,
    tree_oid: &str,
    stage_result: &GitStageResult,
) -> Result<(), GitCommitError> {
    if required_oid(runner, &["rev-parse", "--verify", "HEAD"])? != commit_oid
        || symbolic_head(runner)? != (plan.target_ref != "HEAD").then(|| plan.target_ref.clone())
        || required_oid(runner, &["rev-parse", &format!("{commit_oid}^{{tree}}")])? != tree_oid
        || required_oid(runner, &["rev-parse", &format!("{commit_oid}^1")])? != plan.expected_head
        || required_oid(runner, &["write-tree"])? != stage_result.index_tree_oid
    {
        return Err(error("Git commit post-state verification failed"));
    }
    verify_commit_object(runner, commit_oid, plan)
}

fn selected_path(path: &GitStagedPath) -> GitCommitSelectedPath {
    GitCommitSelectedPath {
        operation: path.operation.clone(),
        path: path.path.clone(),
        from_path: path.from_path.clone(),
        whole_path: path.whole_path,
        hunk_ids: path.hunk_ids.clone(),
        mode_included: path.mode_included,
    }
}

fn merge_agent_tree(
    runner: &GitRunner,
    merge_base: &str,
    head_tree: &str,
    staged_tree: &str,
) -> Result<String, GitCommitError> {
    for oid in [merge_base, head_tree, staged_tree] {
        validate_oid(oid)?;
    }
    let merge_base_argument = format!("--merge-base={merge_base}");
    let output = runner
        .run(
            &[
                "merge-tree",
                "--write-tree",
                "--no-messages",
                &merge_base_argument,
                head_tree,
                staged_tree,
            ],
            None,
            MAX_GIT_OUTPUT,
        )
        .map_err(from_git)?;
    require_success(&output, "Git Agent-only tree merge failed")?;
    parse_oid(&output.stdout)
}

fn create_commit_object(
    runner: &GitRunner,
    tree_oid: &str,
    parent_oid: &str,
    request: &GitCommitRequest,
) -> Result<String, GitCommitError> {
    let author_date = identity_date(&request.author);
    let committer_date = identity_date(&request.committer);
    let environment = GitCommitEnvironment {
        author_name: &request.author.name,
        author_email: &request.author.email,
        author_date: &author_date,
        committer_name: &request.committer.name,
        committer_email: &request.committer.email,
        committer_date: &committer_date,
    };
    let output = runner
        .run_with_commit_environment(
            &["commit-tree", tree_oid, "-p", parent_oid],
            Some(request.message.as_bytes()),
            1024,
            &environment,
        )
        .map_err(from_git)?;
    require_success(&output, "Git commit object creation failed")?;
    parse_oid(&output.stdout)
}

fn exact_patch(runner: &GitRunner, before: &str, after: &str) -> Result<String, GitCommitError> {
    let output = runner
        .run(
            &[
                "diff",
                "--binary",
                "--full-index",
                "--no-ext-diff",
                "--no-textconv",
                "--find-renames",
                before,
                after,
                "--",
            ],
            None,
            MAX_DIFF_BYTES,
        )
        .map_err(from_git)?;
    require_success(&output, "Git commit preview diff failed")?;
    String::from_utf8(output.stdout).map_err(|_| error("Git commit preview diff is not UTF-8"))
}

fn changed_paths_between(
    runner: &GitRunner,
    before: &str,
    after: &str,
) -> Result<Vec<String>, GitCommitError> {
    let output = runner
        .run(
            &[
                "diff",
                "--name-status",
                "-z",
                "--find-renames",
                before,
                after,
                "--",
            ],
            None,
            MAX_GIT_OUTPUT,
        )
        .map_err(from_git)?;
    require_success(&output, "Git changed-path preview failed")?;
    parse_name_status(&output.stdout)
}

fn parse_name_status(bytes: &[u8]) -> Result<Vec<String>, GitCommitError> {
    let records = bytes
        .split(|byte| *byte == 0)
        .filter(|record| !record.is_empty())
        .collect::<Vec<_>>();
    let mut paths = Vec::new();
    let mut index = 0_usize;
    while index < records.len() {
        let status = utf8(records[index], "Git changed-path status")?;
        index += 1;
        let path = records
            .get(index)
            .ok_or_else(|| error("Git changed-path record is incomplete"))?;
        paths.push(utf8(path, "Git changed path")?);
        index += 1;
        if status.starts_with('R') || status.starts_with('C') {
            let target = records
                .get(index)
                .ok_or_else(|| error("Git rename record is incomplete"))?;
            paths.push(utf8(target, "Git changed target path")?);
            index += 1;
        }
        if paths.len() > MAX_CHANGED_PATHS {
            return Err(error("Git commit changed-path limit exceeded"));
        }
    }
    paths.sort();
    paths.dedup();
    Ok(paths)
}

fn diff_statistics(
    runner: &GitRunner,
    before: &str,
    after: &str,
) -> Result<(usize, usize, usize), GitCommitError> {
    let output = runner
        .run(
            &["diff", "--numstat", "-z", before, after, "--"],
            None,
            MAX_GIT_OUTPUT,
        )
        .map_err(from_git)?;
    require_success(&output, "Git commit statistics failed")?;
    let mut additions = 0_usize;
    let mut deletions = 0_usize;
    let mut binary = 0_usize;
    for record in output.stdout.split(|byte| *byte == 0) {
        let record = std::str::from_utf8(record)
            .map_err(|_| error("Git commit statistics are not UTF-8"))?;
        let mut fields = record.splitn(3, '\t');
        let Some(added) = fields.next() else {
            continue;
        };
        let Some(deleted) = fields.next() else {
            continue;
        };
        if added == "-" || deleted == "-" {
            binary += 1;
        } else {
            additions = additions.saturating_add(
                added
                    .parse::<usize>()
                    .map_err(|_| error("Git addition count is invalid"))?,
            );
            deletions = deletions.saturating_add(
                deleted
                    .parse::<usize>()
                    .map_err(|_| error("Git deletion count is invalid"))?,
            );
        }
    }
    Ok((additions, deletions, binary))
}

fn detect_hooks(
    runner: &GitRunner,
    git_directory: &Path,
) -> Result<Vec<GitCommitHook>, GitCommitError> {
    let hooks = git_directory.join("hooks");
    let mut detected = Vec::new();
    for name in KNOWN_HOOKS {
        let path = hooks.join(name);
        match fs::symlink_metadata(&path) {
            Ok(metadata) if metadata.file_type().is_symlink() => detected.push(GitCommitHook {
                name: (*name).into(),
                state: "symlink-denied".into(),
            }),
            Ok(metadata) if metadata.is_file() && hook_is_executable(&metadata) => {
                detected.push(GitCommitHook {
                    name: (*name).into(),
                    state: "active-bypassed".into(),
                })
            }
            Ok(_) => detected.push(GitCommitHook {
                name: (*name).into(),
                state: "inactive".into(),
            }),
            Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => {}
            Err(_) => return Err(error("Git hook metadata is unavailable")),
        }
    }
    let custom = runner
        .run(
            &["config", "--local", "--get", "core.hooksPath"],
            None,
            16 * 1024,
        )
        .map_err(from_git)?;
    if custom.success && !custom.stdout.iter().all(u8::is_ascii_whitespace) {
        detected.push(GitCommitHook {
            name: "core.hooksPath".into(),
            state: "custom-path-configured-bypassed".into(),
        });
    }
    Ok(detected)
}

fn custom_merge_driver_is_configured(
    runner: &GitRunner,
    paths: &[String],
) -> Result<bool, GitCommitError> {
    if paths.is_empty() {
        return Ok(false);
    }
    let configured_drivers = runner
        .run(
            &["config", "--get-regexp", r"^merge\..*\.driver$"],
            None,
            64 * 1024,
        )
        .map_err(from_git)?;
    if configured_drivers.success && !configured_drivers.stdout.is_empty() {
        return Ok(true);
    }
    if !configured_drivers.success && configured_drivers.code != Some(1) {
        return Err(error("Git merge-driver configuration query failed"));
    }
    let mut owned = vec![
        "check-attr".to_owned(),
        "-z".into(),
        "merge".into(),
        "--".into(),
    ];
    owned.extend(paths.iter().cloned());
    let args = owned.iter().map(String::as_str).collect::<Vec<_>>();
    let output = runner.run(&args, None, MAX_GIT_OUTPUT).map_err(from_git)?;
    require_success(&output, "Git merge-attribute query failed")?;
    let records = output
        .stdout
        .split(|byte| *byte == 0)
        .filter(|record| !record.is_empty())
        .collect::<Vec<_>>();
    if records.len() % 3 != 0 {
        return Err(error("Git merge-attribute response is malformed"));
    }
    for record in records.chunks_exact(3) {
        let path = utf8(record[0], "Git merge-attribute path")?;
        let attribute = utf8(record[1], "Git merge attribute")?;
        let value = utf8(record[2], "Git merge-attribute value")?;
        if attribute != "merge" || !paths.iter().any(|candidate| candidate == &path) {
            return Err(error("Git returned an unexpected merge attribute"));
        }
        if matches!(value.as_str(), "unspecified" | "unset" | "set") {
            continue;
        }
        if !matches!(value.as_str(), "text" | "binary" | "union") {
            return Ok(true);
        }
        let key = format!("merge.{value}.driver");
        let configured = runner
            .run(&["config", "--get", &key], None, 16 * 1024)
            .map_err(from_git)?;
        if configured.success && !configured.stdout.iter().all(u8::is_ascii_whitespace) {
            return Ok(true);
        }
    }
    Ok(false)
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

fn validate_request(mut request: GitCommitRequest) -> Result<GitCommitRequest, GitCommitError> {
    if request.message.len() > MAX_MESSAGE_BYTES
        || request.message.contains(['\0', '\r'])
        || request.message.trim().is_empty()
    {
        return Err(error("Git commit message is invalid"));
    }
    let subject = request.message.lines().next().unwrap_or_default();
    if subject.trim().is_empty() || subject.chars().count() > 200 {
        return Err(error("Git commit subject is invalid"));
    }
    while request.message.ends_with('\n') {
        request.message.pop();
    }
    request.message.push('\n');
    if request.message.len() > MAX_MESSAGE_BYTES
        || request
            .message
            .chars()
            .any(|character| character.is_control() && !matches!(character, '\n' | '\t'))
    {
        return Err(error("Git commit message is invalid"));
    }
    validate_message_source(&request.message_source)?;
    validate_identity(&request.author)?;
    validate_identity(&request.committer)?;
    Ok(request)
}

pub(crate) fn validate_message_source(
    source: &GitCommitMessageSource,
) -> Result<(), GitCommitError> {
    match source {
        GitCommitMessageSource::User => Ok(()),
        GitCommitMessageSource::AgentGenerated { generator_id } => {
            validate_identifier(generator_id, "commit message generator")
        }
        GitCommitMessageSource::Template { template_id } => {
            validate_identifier(template_id, "commit message template")
        }
    }
}

pub(crate) fn validate_identity(identity: &GitCommitIdentity) -> Result<(), GitCommitError> {
    if identity.name.is_empty()
        || identity.name.len() > 256
        || identity.name.trim() != identity.name
        || identity
            .name
            .chars()
            .any(|character| character.is_control() || matches!(character, '<' | '>'))
    {
        return Err(error("Git commit identity name is invalid"));
    }
    if identity.email.len() < 3
        || identity.email.len() > 320
        || !identity.email.contains('@')
        || identity.email.chars().any(|character| {
            character.is_control() || character.is_whitespace() || matches!(character, '<' | '>')
        })
    {
        return Err(error("Git commit identity email is invalid"));
    }
    if !matches!(
        identity.source.as_str(),
        "git-config" | "user-profile" | "explicit"
    ) {
        return Err(error("Git commit identity source is invalid"));
    }
    if !(0..=MAX_TIMESTAMP).contains(&identity.timestamp_seconds)
        || !valid_timezone(&identity.timezone)
    {
        return Err(error("Git commit identity time is invalid"));
    }
    Ok(())
}

fn valid_timezone(value: &str) -> bool {
    if value.len() != 5 || !matches!(value.as_bytes()[0], b'+' | b'-') {
        return false;
    }
    let Ok(hours) = value[1..3].parse::<u8>() else {
        return false;
    };
    let Ok(minutes) = value[3..5].parse::<u8>() else {
        return false;
    };
    hours <= 14 && minutes < 60 && (hours != 14 || minutes == 0)
}

fn validate_identifier(value: &str, label: &str) -> Result<(), GitCommitError> {
    if value.is_empty()
        || value.len() > 128
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.'))
    {
        return Err(error(format!("{label} ID is invalid")));
    }
    Ok(())
}

pub(crate) fn identity_date(identity: &GitCommitIdentity) -> String {
    format!("@{} {}", identity.timestamp_seconds, identity.timezone)
}

fn verify_commit_object(
    runner: &GitRunner,
    oid: &str,
    plan: &GitCommitPlan,
) -> Result<(), GitCommitError> {
    let output = runner
        .run(
            &["cat-file", "commit", oid],
            None,
            MAX_MESSAGE_BYTES as u64 + 4096,
        )
        .map_err(from_git)?;
    require_success(&output, "Git commit object read failed")?;
    let text =
        std::str::from_utf8(&output.stdout).map_err(|_| error("Git commit object is not UTF-8"))?;
    let (headers, message) = text
        .split_once("\n\n")
        .ok_or_else(|| error("Git commit object is malformed"))?;
    let expected = [
        format!(
            "tree {}",
            plan.commit_tree_oid.as_deref().unwrap_or_default()
        ),
        format!("parent {}", plan.expected_head),
        identity_header("author", &plan.request.author),
        identity_header("committer", &plan.request.committer),
    ];
    if headers.lines().collect::<Vec<_>>()
        != expected.iter().map(String::as_str).collect::<Vec<_>>()
        || message != plan.request.message
    {
        return Err(error("Git commit object metadata does not match preview"));
    }
    Ok(())
}

fn identity_header(kind: &str, identity: &GitCommitIdentity) -> String {
    format!(
        "{kind} {} <{}> {} {}",
        identity.name, identity.email, identity.timestamp_seconds, identity.timezone
    )
}

fn symbolic_head(runner: &GitRunner) -> Result<Option<String>, GitCommitError> {
    let output = runner
        .run(&["symbolic-ref", "-q", "HEAD"], None, 16 * 1024)
        .map_err(from_git)?;
    if !output.success {
        return Ok(None);
    }
    let value = std::str::from_utf8(&output.stdout)
        .map_err(|_| error("Git symbolic HEAD is not UTF-8"))?
        .trim();
    if !value.starts_with("refs/heads/") || value.len() > 1024 {
        return Err(error("Git symbolic HEAD is invalid"));
    }
    Ok(Some(value.into()))
}

fn update_ref(
    runner: &GitRunner,
    reference: &str,
    new_oid: &str,
    expected_oid: &str,
    reason: &str,
) -> Result<(), GitCommitError> {
    validate_oid(new_oid)?;
    validate_oid(expected_oid)?;
    if reference != "HEAD" && !reference.starts_with("refs/heads/") {
        return Err(error("Git commit target ref is invalid"));
    }
    let output = runner
        .run(
            &["update-ref", "-m", reason, reference, new_oid, expected_oid],
            None,
            16 * 1024,
        )
        .map_err(from_git)?;
    require_success(&output, "Git commit ref update failed")
}

fn with_ref_rollback(
    cause: GitCommitError,
    runner: &GitRunner,
    plan: &GitCommitPlan,
    commit_oid: &str,
) -> GitCommitError {
    let rollback_complete = update_ref(
        runner,
        &plan.target_ref,
        &plan.expected_head,
        commit_oid,
        "aegisy-commit-rollback",
    )
    .and_then(|_| {
        if required_oid(runner, &["rev-parse", "--verify", "HEAD"])? == plan.expected_head {
            Ok(())
        } else {
            Err(error("Git commit rollback HEAD verification failed"))
        }
    })
    .is_ok();
    GitCommitError {
        message: cause.message,
        rollback_complete,
    }
}

fn required_oid(runner: &GitRunner, args: &[&str]) -> Result<String, GitCommitError> {
    let output = runner.run(args, None, 1024).map_err(from_git)?;
    require_success(&output, "Git object query failed")?;
    parse_oid(&output.stdout)
}

fn parse_oid(bytes: &[u8]) -> Result<String, GitCommitError> {
    let oid = std::str::from_utf8(bytes)
        .map_err(|_| error("Git object ID is not UTF-8"))?
        .trim();
    validate_oid(oid)?;
    Ok(oid.into())
}

fn validate_oid(oid: &str) -> Result<(), GitCommitError> {
    if !matches!(oid.len(), 40 | 64)
        || !oid
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
        return Err(error("Git object ID is invalid"));
    }
    Ok(())
}

fn require_success(output: &GitOutput, message: &str) -> Result<(), GitCommitError> {
    if output.success {
        Ok(())
    } else {
        Err(error(message))
    }
}

fn utf8(bytes: &[u8], label: &str) -> Result<String, GitCommitError> {
    let value = std::str::from_utf8(bytes).map_err(|_| error(format!("{label} is not UTF-8")))?;
    if value.is_empty() || value.len() > 4 * 1024 {
        return Err(error(format!("{label} is invalid")));
    }
    Ok(value.into())
}

fn path_to_utf8(path: &Path, label: &str) -> Result<String, GitCommitError> {
    path.to_str()
        .map(str::to_owned)
        .ok_or_else(|| error(format!("{label} is not UTF-8")))
}

fn now_ms() -> Result<u64, GitCommitError> {
    let milliseconds = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| error("system clock is before the Unix epoch"))?
        .as_millis();
    u64::try_from(milliseconds).map_err(|_| error("system clock is out of range"))
}

fn from_stage(cause: crate::git_staging::GitStageError) -> GitCommitError {
    error(cause.message)
}

fn from_git(cause: GitStatusError) -> GitCommitError {
    error(cause.message)
}

fn error(message: impl Into<String>) -> GitCommitError {
    GitCommitError {
        message: message.into(),
        rollback_complete: true,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::git_checkpoint::{
        bind_git_checkpoint_application, capture_git_checkpoint, GitCheckpointApplication,
    };
    use crate::git_staging::{plan_agent_staging, stage_agent_changes, GitStageSelection};
    use crate::workspace_edit::{ProposedContent, WorkspaceEdit, WorkspaceEditOperation};
    use crate::workspace_edit_apply::apply_workspace_edit;
    use crate::workspace_edit_preview::ContentInput;
    use std::path::PathBuf;
    use std::process::{Command, Stdio};
    use std::sync::atomic::{AtomicU64, Ordering};

    static SEQUENCE: AtomicU64 = AtomicU64::new(0);

    struct Prepared {
        root: PathBuf,
        checkpoint: GitCheckpoint,
        application: GitCheckpointApplication,
        stage_plan: GitStagePlan,
        stage_result: GitStageResult,
        original_head: String,
        agent_after: String,
    }

    struct FailAfterRef;

    impl FaultInjector for FailAfterRef {
        fn after_ref_update(&self) -> Result<(), GitCommitError> {
            Err(error("injected failure after commit ref update"))
        }
    }

    struct RewriteRefAfterUpdate {
        root: PathBuf,
        reference: String,
        alternate_oid: String,
    }

    impl FaultInjector for RewriteRefAfterUpdate {
        fn after_ref_update(&self) -> Result<(), GitCommitError> {
            let output = Command::new("git")
                .arg("-C")
                .arg(&self.root)
                .args(["update-ref", &self.reference, &self.alternate_oid])
                .output()
                .map_err(|_| error("cannot inject external ref rewrite"))?;
            if !output.status.success() {
                return Err(error("injected external ref rewrite failed"));
            }
            Err(error("injected failure after external ref rewrite"))
        }
    }

    fn root() -> PathBuf {
        let sequence = SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "aegisy-git-commit-test-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(&root).unwrap();
        git(&root, &["init", "-q"]);
        git(&root, &["config", "user.name", "Aegisy Test"]);
        git(&root, &["config", "user.email", "test@aegisy.local"]);
        root.canonicalize().unwrap()
    }

    fn git(root: &Path, args: &[&str]) -> Vec<u8> {
        let output = Command::new("git")
            .arg("-C")
            .arg(root)
            .args(args)
            .stderr(Stdio::inherit())
            .output()
            .unwrap();
        assert!(output.status.success(), "git command failed: {args:?}");
        output.stdout
    }

    fn text(root: &Path, args: &[&str]) -> String {
        String::from_utf8(git(root, args)).unwrap()
    }

    fn prepare() -> Prepared {
        let root = root();
        let committed = "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\n";
        let staged_user = "one\ntwo\nthree\nfour\nfive\nsix\nuser-staged\neight\n";
        let agent_base = "one\nuser-unstaged\nthree\nfour\nfive\nsix\nuser-staged\neight\n";
        let agent_after =
            "one\nuser-unstaged\nthree\nagent-change\nfive\nsix\nuser-staged\neight\n";
        fs::write(root.join("file.txt"), committed).unwrap();
        fs::write(root.join("other.txt"), "other\n").unwrap();
        git(&root, &["add", "."]);
        git(&root, &["commit", "-q", "-m", "initial"]);
        let original_head = text(&root, &["rev-parse", "HEAD"]).trim().into();
        fs::write(root.join("file.txt"), staged_user).unwrap();
        git(&root, &["add", "file.txt"]);
        fs::write(root.join("file.txt"), agent_base).unwrap();

        let proposed = ProposedContent::for_bytes(agent_after.as_bytes());
        let edit = WorkspaceEdit::define(
            "commit-edit",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Update {
                path: "file.txt".into(),
                base: ContentHash::for_bytes(agent_base.as_bytes()),
                content: proposed.clone(),
            }],
        )
        .unwrap();
        let checkpoint =
            capture_git_checkpoint("commit-checkpoint", &edit, &HashSet::new(), &HashSet::new())
                .unwrap();
        let applied = apply_workspace_edit(
            edit.clone(),
            vec![ContentInput {
                reference: proposed.reference,
                content: agent_after.into(),
            }],
            &HashSet::new(),
        )
        .unwrap();
        let application = bind_git_checkpoint_application(&checkpoint, &edit, &applied).unwrap();
        let stage_plan = plan_agent_staging(&checkpoint, &application, &HashSet::new()).unwrap();
        let stage_result = stage_agent_changes(
            &checkpoint,
            &application,
            &HashSet::new(),
            &stage_plan,
            &[GitStageSelection {
                path: "file.txt".into(),
                whole_path: true,
                hunk_ids: Vec::new(),
                include_mode: false,
            }],
        )
        .unwrap();
        Prepared {
            root,
            checkpoint,
            application,
            stage_plan,
            stage_result,
            original_head,
            agent_after: agent_after.into(),
        }
    }

    fn request(hook_policy: GitCommitHookPolicy) -> GitCommitRequest {
        GitCommitRequest {
            message: "Apply the selected Agent change\n\nBounded commit body.\n".into(),
            message_source: GitCommitMessageSource::AgentGenerated {
                generator_id: "aegisy-default".into(),
            },
            author: GitCommitIdentity {
                name: "Aegisy Author".into(),
                email: "author@aegisy.local".into(),
                source: "user-profile".into(),
                timestamp_seconds: 1_700_000_000,
                timezone: "+0800".into(),
            },
            committer: GitCommitIdentity {
                name: "Aegisy Committer".into(),
                email: "committer@aegisy.local".into(),
                source: "explicit".into(),
                timestamp_seconds: 1_700_000_005,
                timezone: "+0800".into(),
            },
            hook_policy,
            signing_policy: GitCommitSigningPolicy::Unsigned,
        }
    }

    #[test]
    fn previews_and_commits_only_agent_delta_while_preserving_user_index_and_worktree() {
        let prepared = prepare();
        let marker = prepared.root.join("hook-ran");
        let hook = prepared.root.join(".git/hooks/pre-commit");
        fs::write(
            &hook,
            format!(
                "#!/bin/sh\necho ran > '{}'
exit 1\n",
                marker.display()
            ),
        )
        .unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&hook, fs::Permissions::from_mode(0o700)).unwrap();
        }
        let request = request(GitCommitHookPolicy::Disabled);
        let plan = plan_agent_commit(
            &prepared.checkpoint,
            &prepared.application,
            &prepared.stage_plan,
            &prepared.stage_result,
            request.clone(),
        )
        .unwrap();
        assert!(
            plan.blocking_reasons.is_empty(),
            "{:?}",
            plan.blocking_reasons
        );
        assert_eq!(plan.changed_paths, vec!["file.txt"]);
        assert_eq!(plan.excluded_user_paths, vec!["file.txt"]);
        assert!(plan.patch.contains("agent-change"));
        assert!(!plan.patch.contains("user-staged"));
        assert!(!plan.patch.contains("user-unstaged"));
        assert!(plan
            .detected_hooks
            .iter()
            .any(|hook| hook.name == "pre-commit" && hook.state == "active-bypassed"));
        assert_eq!(plan.committer_behavior, "explicit-committer");
        let result = execute_agent_commit(
            &prepared.checkpoint,
            &prepared.application,
            &prepared.stage_plan,
            &prepared.stage_result,
            request,
            &plan,
        )
        .unwrap();
        assert_eq!(result.commit_oid, plan.commit_oid.unwrap());
        assert!(!marker.exists());
        assert_eq!(
            text(&prepared.root, &["show", "HEAD:file.txt"]),
            "one\ntwo\nthree\nagent-change\nfive\nsix\nseven\neight\n"
        );
        assert_eq!(
            text(&prepared.root, &["show", ":file.txt"]),
            "one\ntwo\nthree\nagent-change\nfive\nsix\nuser-staged\neight\n"
        );
        assert_eq!(
            fs::read_to_string(prepared.root.join("file.txt")).unwrap(),
            prepared.agent_after
        );
        let cached = text(&prepared.root, &["diff", "--cached"]);
        assert!(cached.contains("user-staged"));
        assert!(!cached.contains("\n+agent-change\n"));
        fs::remove_dir_all(prepared.root).unwrap();
    }

    #[test]
    fn hook_and_signing_requests_are_reviewed_but_blocked_before_ref_update() {
        let prepared = prepare();
        let mut hook_request = request(GitCommitHookPolicy::Run);
        let hook_plan = plan_agent_commit(
            &prepared.checkpoint,
            &prepared.application,
            &prepared.stage_plan,
            &prepared.stage_result,
            hook_request.clone(),
        )
        .unwrap();
        assert!(hook_plan
            .blocking_reasons
            .contains(&"hook-execution-gate-unavailable".into()));
        assert!(hook_plan.commit_oid.is_none());
        assert!(execute_agent_commit(
            &prepared.checkpoint,
            &prepared.application,
            &prepared.stage_plan,
            &prepared.stage_result,
            hook_request.clone(),
            &hook_plan,
        )
        .is_err());
        assert_eq!(
            text(&prepared.root, &["rev-parse", "HEAD"]).trim(),
            prepared.original_head
        );

        hook_request.hook_policy = GitCommitHookPolicy::Disabled;
        hook_request.signing_policy = GitCommitSigningPolicy::Sign;
        let sign_plan = plan_agent_commit(
            &prepared.checkpoint,
            &prepared.application,
            &prepared.stage_plan,
            &prepared.stage_result,
            hook_request,
        )
        .unwrap();
        assert!(sign_plan
            .blocking_reasons
            .contains(&"commit-signing-gate-unavailable".into()));
        assert!(sign_plan.commit_oid.is_none());
        fs::remove_dir_all(prepared.root).unwrap();
    }

    #[test]
    fn injected_ref_failure_rolls_back_and_external_ref_rewrite_is_preserved() {
        let prepared = prepare();
        let request = request(GitCommitHookPolicy::Disabled);
        let plan = plan_agent_commit(
            &prepared.checkpoint,
            &prepared.application,
            &prepared.stage_plan,
            &prepared.stage_result,
            request.clone(),
        )
        .unwrap();
        let failure = execute_with_fault(
            &prepared.checkpoint,
            &prepared.application,
            &prepared.stage_plan,
            &prepared.stage_result,
            request.clone(),
            &plan,
            &FailAfterRef,
        )
        .unwrap_err();
        assert!(failure.rollback_complete);
        assert_eq!(
            text(&prepared.root, &["rev-parse", "HEAD"]).trim(),
            prepared.original_head
        );

        let alternate_oid = text(
            &prepared.root,
            &[
                "commit-tree",
                &plan.head_tree_oid,
                "-p",
                &prepared.original_head,
                "-m",
                "external",
            ],
        )
        .trim()
        .to_owned();
        let external = RewriteRefAfterUpdate {
            root: prepared.root.clone(),
            reference: plan.target_ref.clone(),
            alternate_oid: alternate_oid.clone(),
        };
        let failure = execute_with_fault(
            &prepared.checkpoint,
            &prepared.application,
            &prepared.stage_plan,
            &prepared.stage_result,
            request,
            &plan,
            &external,
        )
        .unwrap_err();
        assert!(!failure.rollback_complete);
        assert_eq!(
            text(&prepared.root, &["rev-parse", "HEAD"]).trim(),
            alternate_oid
        );
        fs::remove_dir_all(prepared.root).unwrap();
    }

    #[test]
    fn custom_merge_driver_is_blocked_and_custom_hook_path_is_reported_without_execution() {
        let prepared = prepare();
        let marker = prepared.root.join("merge-driver-ran");
        fs::write(
            prepared.root.join(".gitattributes"),
            "file.txt merge=evil\n",
        )
        .unwrap();
        git(
            &prepared.root,
            &[
                "config",
                "merge.evil.driver",
                &format!("sh -c 'echo ran > {}'", marker.display()),
            ],
        );
        git(
            &prepared.root,
            &["config", "core.hooksPath", "custom-hooks"],
        );
        let request = request(GitCommitHookPolicy::Disabled);
        let plan = plan_agent_commit(
            &prepared.checkpoint,
            &prepared.application,
            &prepared.stage_plan,
            &prepared.stage_result,
            request.clone(),
        )
        .unwrap();
        assert!(plan
            .blocking_reasons
            .contains(&"custom-merge-driver-requires-execution-gate".into()));
        assert!(plan.commit_oid.is_none());
        assert!(plan.detected_hooks.iter().any(|hook| {
            hook.name == "core.hooksPath" && hook.state == "custom-path-configured-bypassed"
        }));
        assert!(!marker.exists());
        assert!(execute_agent_commit(
            &prepared.checkpoint,
            &prepared.application,
            &prepared.stage_plan,
            &prepared.stage_result,
            request,
            &plan,
        )
        .is_err());
        assert!(!marker.exists());
        assert_eq!(
            text(&prepared.root, &["rev-parse", "HEAD"]).trim(),
            prepared.original_head
        );
        fs::remove_dir_all(prepared.root).unwrap();
    }

    #[test]
    fn rejects_invalid_metadata_and_stale_index_before_ref_update() {
        let prepared = prepare();
        let mut invalid = request(GitCommitHookPolicy::Disabled);
        invalid.message = "bad\u{1b}message".into();
        assert!(plan_agent_commit(
            &prepared.checkpoint,
            &prepared.application,
            &prepared.stage_plan,
            &prepared.stage_result,
            invalid,
        )
        .is_err());
        let mut invalid = request(GitCommitHookPolicy::Disabled);
        invalid.author.email = "not an email".into();
        assert!(plan_agent_commit(
            &prepared.checkpoint,
            &prepared.application,
            &prepared.stage_plan,
            &prepared.stage_result,
            invalid,
        )
        .is_err());

        let request = request(GitCommitHookPolicy::Disabled);
        let plan = plan_agent_commit(
            &prepared.checkpoint,
            &prepared.application,
            &prepared.stage_plan,
            &prepared.stage_result,
            request.clone(),
        )
        .unwrap();
        fs::write(prepared.root.join("other.txt"), "later staged\n").unwrap();
        git(&prepared.root, &["add", "other.txt"]);
        let failure = execute_agent_commit(
            &prepared.checkpoint,
            &prepared.application,
            &prepared.stage_plan,
            &prepared.stage_result,
            request,
            &plan,
        )
        .unwrap_err();
        assert!(failure.message.contains("index tree changed"));
        assert_eq!(
            text(&prepared.root, &["rev-parse", "HEAD"]).trim(),
            prepared.original_head
        );
        fs::remove_dir_all(prepared.root).unwrap();
    }
}
