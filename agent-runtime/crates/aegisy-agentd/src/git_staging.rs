use crate::git_checkpoint::{
    read_git_checkpoint_preimage, verify_git_checkpoint_application, GitCheckpoint,
    GitCheckpointApplication, GitCheckpointAppliedPath, GitCheckpointPathState,
};
use crate::git_status::{ignored_paths, status, GitOutput, GitRunner, GitStatusError};
use crate::workspace::is_sensitive_path;
use crate::workspace_edit::ContentHash;
use serde::Serialize;
use similar::{DiffTag, TextDiff};
use std::collections::{HashMap, HashSet};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

const PLAN_SCHEMA_VERSION: &str = "git-stage-plan/0.1";
const RESULT_SCHEMA_VERSION: &str = "git-stage-result/0.1";
const MAX_FILE_BYTES: u64 = 512 * 1024;
const MAX_TOTAL_BYTES: u64 = 8 * 1024 * 1024;
const MAX_INDEX_BYTES: u64 = 64 * 1024 * 1024;
const MAX_GIT_OUTPUT: u64 = 2 * 1024 * 1024;
const MAX_STAGE_PATHS: usize = 256;
static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitStagePlan {
    pub schema_version: String,
    pub checkpoint_id: String,
    pub checkpoint_commit_oid: String,
    pub edit_id: String,
    pub project_id: String,
    pub root_identity: String,
    pub repository_root: String,
    pub head_oid: Option<String>,
    pub branch: Option<String>,
    pub index_path: String,
    pub index_tree_oid: String,
    pub index_file_hash: ContentHash,
    pub paths: Vec<GitStagePathPlan>,
    pub blocking_reasons: Vec<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitStagePathPlan {
    pub operation: String,
    pub path: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub from_path: Option<String>,
    pub ownership: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub before_hash: Option<ContentHash>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub agent_after_hash: Option<ContentHash>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub before_mode: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub agent_after_mode: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub index_blob_oid: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub index_mode: Option<String>,
    pub hunks: Vec<GitStageHunk>,
    pub mode_changed: bool,
    pub whole_path_required: bool,
    pub blocking_reasons: Vec<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitStageHunk {
    pub id: String,
    pub old_start: usize,
    pub old_lines: usize,
    pub new_start: usize,
    pub new_lines: usize,
    pub additions: usize,
    pub deletions: usize,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitStageSelection {
    pub path: String,
    pub whole_path: bool,
    pub hunk_ids: Vec<String>,
    pub include_mode: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitStageResult {
    pub schema_version: String,
    pub checkpoint_id: String,
    pub previous_index_tree_oid: String,
    pub index_tree_oid: String,
    pub staged: Vec<GitStagedPath>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitStagedPath {
    pub operation: String,
    pub path: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub from_path: Option<String>,
    pub whole_path: bool,
    pub hunk_ids: Vec<String>,
    pub mode_included: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GitStageError {
    pub message: String,
    pub rollback_complete: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct IndexEntry {
    mode: String,
    oid: String,
}

pub fn plan_agent_staging(
    checkpoint: &GitCheckpoint,
    application: &GitCheckpointApplication,
    pending_editor_paths: &HashSet<String>,
) -> Result<GitStagePlan, GitStageError> {
    verify_git_checkpoint_application(checkpoint, application).map_err(from_checkpoint)?;
    validate_pending_paths(pending_editor_paths)?;
    let root = checkpoint.repository_root.as_path();
    let snapshot = status(root).map_err(from_git)?;
    let root_text = path_to_utf8(root, "Git repository root")?;
    let mut blocking_reasons = Vec::new();
    if snapshot.repository_root.as_deref() != Some(root_text.as_str()) {
        blocking_reasons.push("project-is-not-repository-root".into());
    }
    if snapshot.head_oid != checkpoint.head_oid || snapshot.branch != checkpoint.branch {
        blocking_reasons.push("head-or-branch-changed".into());
    }
    if snapshot.operation_in_progress.is_some() {
        blocking_reasons.push("git-operation-in-progress".into());
    }
    if !snapshot.conflicts.is_empty() {
        blocking_reasons.push("git-index-conflicted".into());
    }
    if snapshot.truncated {
        blocking_reasons.push("git-status-truncated".into());
    }

    let runner = GitRunner::new(root).map_err(from_git)?;
    let index_path = discover_index_path(&runner, checkpoint)?;
    let index_tree_oid = required_oid(&runner, &["write-tree"], None)?;
    let units = operation_units(&application.paths)?;
    if units.len() > MAX_STAGE_PATHS {
        return Err(error("Git staging path limit exceeded"));
    }
    let all_paths = units
        .iter()
        .flat_map(|unit| {
            unit.from
                .iter()
                .map(|path| path.path.clone())
                .chain(std::iter::once(unit.target.path.clone()))
        })
        .collect::<Vec<_>>();
    let ignored = ignored_paths(root, &all_paths);
    let mut retained_bytes = 0_u64;
    let mut paths = Vec::with_capacity(units.len());
    for unit in units {
        paths.push(build_path_plan(
            checkpoint,
            &runner,
            &unit,
            pending_editor_paths,
            &ignored,
            &mut retained_bytes,
        )?);
    }
    let index_bytes = read_bounded(&index_path, MAX_INDEX_BYTES, "Git index")?;
    let index_file_hash = ContentHash::for_bytes(&index_bytes);
    blocking_reasons.sort();
    blocking_reasons.dedup();
    Ok(GitStagePlan {
        schema_version: PLAN_SCHEMA_VERSION.into(),
        checkpoint_id: checkpoint.checkpoint_id.clone(),
        checkpoint_commit_oid: checkpoint.commit_oid.clone(),
        edit_id: application.edit_id.clone(),
        project_id: application.project_id.clone(),
        root_identity: application.root_identity.clone(),
        repository_root: root_text,
        head_oid: checkpoint.head_oid.clone(),
        branch: checkpoint.branch.clone(),
        index_path: path_to_utf8(&index_path, "Git index")?,
        index_tree_oid,
        index_file_hash,
        paths,
        blocking_reasons,
    })
}

pub fn stage_agent_changes(
    checkpoint: &GitCheckpoint,
    application: &GitCheckpointApplication,
    pending_editor_paths: &HashSet<String>,
    plan: &GitStagePlan,
    selections: &[GitStageSelection],
) -> Result<GitStageResult, GitStageError> {
    stage_with_fault(
        checkpoint,
        application,
        pending_editor_paths,
        plan,
        selections,
        &NoFault,
    )
}

pub fn verify_agent_staging_result(
    checkpoint: &GitCheckpoint,
    application: &GitCheckpointApplication,
    plan: &GitStagePlan,
    result: &GitStageResult,
) -> Result<(), GitStageError> {
    verify_git_checkpoint_application(checkpoint, application).map_err(from_checkpoint)?;
    if plan.schema_version != PLAN_SCHEMA_VERSION
        || result.schema_version != RESULT_SCHEMA_VERSION
        || plan.checkpoint_id != checkpoint.checkpoint_id
        || plan.checkpoint_commit_oid != checkpoint.commit_oid
        || plan.edit_id != application.edit_id
        || plan.project_id != application.project_id
        || plan.root_identity != application.root_identity
        || plan.repository_root != path_to_utf8(&checkpoint.repository_root, "Git repository root")?
        || result.checkpoint_id != checkpoint.checkpoint_id
        || result.previous_index_tree_oid != plan.index_tree_oid
    {
        return Err(error("Git staging result identity is inconsistent"));
    }
    let selections = result
        .staged
        .iter()
        .map(|staged| GitStageSelection {
            path: staged.path.clone(),
            whole_path: staged.whole_path,
            hunk_ids: staged.hunk_ids.clone(),
            include_mode: staged.mode_included,
        })
        .collect::<Vec<_>>();
    let validated = validate_selections(plan, &selections)?;
    for staged in &result.staged {
        let path = validated
            .get(&staged.path)
            .ok_or_else(|| error("Git staging result path is unavailable"))?
            .path_plan;
        if staged.operation != path.operation || staged.from_path != path.from_path {
            return Err(error("Git staging result path metadata was modified"));
        }
    }
    let snapshot = status(&checkpoint.repository_root).map_err(from_git)?;
    if snapshot.head_oid != plan.head_oid
        || snapshot.branch != plan.branch
        || snapshot.operation_in_progress.is_some()
        || !snapshot.conflicts.is_empty()
        || snapshot.truncated
    {
        return Err(error("Git repository state changed after staging"));
    }
    let runner = GitRunner::new(&checkpoint.repository_root).map_err(from_git)?;
    let current_tree = required_oid(&runner, &["write-tree"], None)?;
    if current_tree != result.index_tree_oid {
        return Err(error("Git index tree changed after staging"));
    }
    Ok(())
}

struct OperationUnit<'a> {
    target: &'a GitCheckpointAppliedPath,
    from: Option<&'a GitCheckpointAppliedPath>,
}

fn operation_units(
    paths: &[GitCheckpointAppliedPath],
) -> Result<Vec<OperationUnit<'_>>, GitStageError> {
    let mut units = Vec::new();
    let mut index = 0_usize;
    while index < paths.len() {
        let path = &paths[index];
        if path.operation == "rename" {
            let target = paths
                .get(index + 1)
                .ok_or_else(|| error("Git staging rename path set is incomplete"))?;
            if path.role != "source" || target.operation != "rename" || target.role != "target" {
                return Err(error("Git staging rename path set is invalid"));
            }
            units.push(OperationUnit {
                target,
                from: Some(path),
            });
            index += 2;
        } else {
            if path.role != "target"
                || !matches!(path.operation.as_str(), "create" | "update" | "delete")
            {
                return Err(error("Git staging operation path set is invalid"));
            }
            units.push(OperationUnit {
                target: path,
                from: None,
            });
            index += 1;
        }
    }
    Ok(units)
}

#[allow(clippy::too_many_arguments)]
fn build_path_plan(
    checkpoint: &GitCheckpoint,
    runner: &GitRunner,
    unit: &OperationUnit<'_>,
    pending_editor_paths: &HashSet<String>,
    ignored: &HashSet<String>,
    retained_bytes: &mut u64,
) -> Result<GitStagePathPlan, GitStageError> {
    let target = unit.target;
    let from_path = unit.from.map(|path| path.path.clone());
    let mut blocking_reasons = Vec::new();
    for path in from_path.iter().chain(std::iter::once(&target.path)) {
        if pending_editor_paths.contains(path) {
            blocking_reasons.push("pending-editor-edit".into());
        }
        if is_sensitive_path(Path::new(path)) || ignored.contains(path) {
            blocking_reasons.push("path-denied-by-workspace-policy".into());
        }
    }
    let current_matches = unit
        .from
        .map(|source| worktree_state_matches(&checkpoint.repository_root, source))
        .transpose()?
        .unwrap_or(true)
        && worktree_state_matches(&checkpoint.repository_root, target)?;
    if !current_matches {
        blocking_reasons.push("agent-result-changed".into());
    }

    let index_path = unit.from.unwrap_or(target).path.as_str();
    let source_index_entry = index_entry(runner, index_path)?;
    let target_index_entry = if unit.from.is_some() {
        index_entry(runner, &target.path)?
    } else {
        source_index_entry.clone()
    };
    let (before_hash, before_mode) = state_hash_mode(&unit.from.unwrap_or(target).before);
    let (agent_after_hash, agent_after_mode) = state_hash_mode(&target.agent_after);
    let mut hunks = Vec::new();
    let whole_path_required = target.operation != "update";

    match target.operation.as_str() {
        "create" => {
            if source_index_entry.is_some() {
                blocking_reasons.push("create-target-already-in-index".into());
            }
        }
        "update" => {
            if let Some(entry) = &source_index_entry {
                validate_regular_mode(&entry.mode, &mut blocking_reasons);
                if current_matches {
                    let before = read_git_checkpoint_preimage(checkpoint, &target.path)
                        .map_err(from_checkpoint)?;
                    let after = read_expected_file(&checkpoint.repository_root, target)?;
                    retain(retained_bytes, before.len().saturating_add(after.len()))?;
                    hunks = make_hunks(checkpoint, target, &before, &after)?;
                }
            } else {
                blocking_reasons.push("update-base-missing-from-index".into());
            }
        }
        "delete" => {
            if let Some(entry) = &source_index_entry {
                validate_regular_mode(&entry.mode, &mut blocking_reasons);
                if tree_entry(runner, &checkpoint.index_tree_oid, &target.path)?.as_ref()
                    != Some(entry)
                {
                    blocking_reasons.push("delete-index-changed-after-checkpoint".into());
                }
            } else {
                blocking_reasons.push("delete-base-missing-from-index".into());
            }
            if checkpoint
                .pre_existing_changes
                .iter()
                .any(|change| change.path == target.path && change.index_status != ".")
            {
                blocking_reasons.push("delete-would-replace-user-staged-change".into());
            }
        }
        "rename" => {
            if let Some(entry) = &source_index_entry {
                validate_regular_mode(&entry.mode, &mut blocking_reasons);
            } else {
                blocking_reasons.push("rename-source-missing-from-index".into());
            }
            if target_index_entry.is_some() {
                blocking_reasons.push("rename-target-already-in-index".into());
            }
        }
        _ => return Err(error("Git staging operation is unsupported")),
    }
    blocking_reasons.sort();
    blocking_reasons.dedup();
    let mode_changed = before_mode != agent_after_mode;
    Ok(GitStagePathPlan {
        operation: target.operation.clone(),
        path: target.path.clone(),
        from_path,
        ownership: target.ownership.clone(),
        before_hash,
        agent_after_hash,
        before_mode,
        agent_after_mode,
        index_blob_oid: source_index_entry.as_ref().map(|entry| entry.oid.clone()),
        index_mode: source_index_entry.map(|entry| entry.mode),
        hunks,
        mode_changed,
        whole_path_required,
        blocking_reasons,
    })
}

fn make_hunks(
    checkpoint: &GitCheckpoint,
    path: &GitCheckpointAppliedPath,
    before: &[u8],
    after: &[u8],
) -> Result<Vec<GitStageHunk>, GitStageError> {
    let before_text =
        std::str::from_utf8(before).map_err(|_| error("Git staging preimage is not UTF-8"))?;
    let after_text =
        std::str::from_utf8(after).map_err(|_| error("Git staging result is not UTF-8"))?;
    let diff = TextDiff::from_lines(before_text, after_text);
    let mut hunks = Vec::new();
    for operation in diff
        .ops()
        .iter()
        .filter(|operation| operation.tag() != DiffTag::Equal)
    {
        let old = operation.old_range();
        let new = operation.new_range();
        let mut identity = Vec::new();
        identity.extend_from_slice(checkpoint.commit_oid.as_bytes());
        identity.push(0);
        identity.extend_from_slice(path.path.as_bytes());
        identity.push(0);
        identity.extend_from_slice(
            format!("{}:{}:{}:{}", old.start, old.end, new.start, new.end).as_bytes(),
        );
        identity.push(0);
        for line in new.clone() {
            identity.extend_from_slice(diff.new_slice(line).unwrap_or_default().as_bytes());
        }
        hunks.push(GitStageHunk {
            id: format!("hunk:sha256:{}", ContentHash::for_bytes(&identity).sha256),
            old_start: old.start + 1,
            old_lines: old.len(),
            new_start: new.start + 1,
            new_lines: new.len(),
            additions: new.len(),
            deletions: old.len(),
        });
    }
    Ok(hunks)
}

trait FaultInjector {
    fn after_install(&self) -> Result<(), GitStageError> {
        Ok(())
    }
}

struct NoFault;

impl FaultInjector for NoFault {}

fn stage_with_fault<F: FaultInjector>(
    checkpoint: &GitCheckpoint,
    application: &GitCheckpointApplication,
    pending_editor_paths: &HashSet<String>,
    plan: &GitStagePlan,
    selections: &[GitStageSelection],
    fault: &F,
) -> Result<GitStageResult, GitStageError> {
    if index_lock_path(Path::new(&plan.index_path)).exists() {
        return Err(error("Git index is locked by another operation"));
    }
    let current = plan_agent_staging(checkpoint, application, pending_editor_paths)?;
    if current != *plan {
        return Err(error("Git staging plan is stale or was modified"));
    }
    if !plan.blocking_reasons.is_empty() {
        return Err(error(format!(
            "Git staging is blocked: {}",
            plan.blocking_reasons.join(",")
        )));
    }
    let selected = validate_selections(plan, selections)?;
    let root = checkpoint.repository_root.as_path();
    let runner = GitRunner::new(root).map_err(from_git)?;
    let index_path = Path::new(&plan.index_path);
    let original = read_bounded(index_path, MAX_INDEX_BYTES, "Git index")?;
    if ContentHash::for_bytes(&original) != plan.index_file_hash {
        return Err(error("Git index changed after staging review"));
    }
    let lock_path = index_lock_path(index_path);
    let original_permissions = fs::metadata(index_path)
        .map_err(|_| error("Git index metadata is unavailable"))?
        .permissions();
    let mut lock = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&lock_path)
        .map_err(|_| error("Git index is locked by another operation"))?;
    let result = (|| {
        lock.write_all(&original)
            .map_err(|_| error("cannot initialize Git index lock"))?;
        lock.set_permissions(original_permissions.clone())
            .map_err(|_| error("cannot preserve Git index permissions"))?;
        lock.flush()
            .map_err(|_| error("cannot flush Git index lock"))?;
        lock.sync_all()
            .map_err(|_| error("cannot sync Git index lock"))?;
        drop(lock);

        let mut staged = Vec::with_capacity(selected.len());
        for selection in selected.values() {
            apply_selection(checkpoint, application, &runner, &lock_path, selection)?;
            staged.push(GitStagedPath {
                operation: selection.path_plan.operation.clone(),
                path: selection.path_plan.path.clone(),
                from_path: selection.path_plan.from_path.clone(),
                whole_path: selection.selection.whole_path,
                hunk_ids: selection.selection.hunk_ids.clone(),
                mode_included: selection.path_plan.mode_changed
                    && (selection.selection.include_mode || selection.selection.whole_path),
            });
        }
        verify_git_checkpoint_application(checkpoint, application).map_err(from_checkpoint)?;
        if read_bounded(index_path, MAX_INDEX_BYTES, "Git index")? != original {
            return Err(error("Git index changed while staging lock was held"));
        }
        validate_worktree_states(checkpoint, application)?;
        let expected_tree = required_oid(&runner, &["write-tree"], Some(&lock_path))?;
        let installed = read_bounded(&lock_path, MAX_INDEX_BYTES, "staged Git index")?;
        let installed_hash = ContentHash::for_bytes(&installed);
        atomic_replace(&lock_path, index_path)
            .map_err(|_| error("cannot atomically install staged Git index"))?;
        sync_parent(index_path)?;
        if let Err(cause) = fault.after_install() {
            return Err(with_index_rollback(
                cause,
                index_path,
                &original,
                &installed_hash,
            ));
        }
        let actual_tree = required_oid(&runner, &["write-tree"], None);
        match actual_tree {
            Ok(actual_tree) if actual_tree == expected_tree => {
                staged.sort_by(|left, right| left.path.cmp(&right.path));
                Ok(GitStageResult {
                    schema_version: RESULT_SCHEMA_VERSION.into(),
                    checkpoint_id: checkpoint.checkpoint_id.clone(),
                    previous_index_tree_oid: plan.index_tree_oid.clone(),
                    index_tree_oid: actual_tree,
                    staged,
                })
            }
            Ok(_) | Err(_) => Err(with_index_rollback(
                error("staged Git index verification failed"),
                index_path,
                &original,
                &installed_hash,
            )),
        }
    })();
    if result.is_err() && lock_path.exists() {
        let _ = fs::remove_file(&lock_path);
    }
    result
}

struct ValidatedSelection<'a> {
    path_plan: &'a GitStagePathPlan,
    selection: &'a GitStageSelection,
}

fn validate_selections<'a>(
    plan: &'a GitStagePlan,
    selections: &'a [GitStageSelection],
) -> Result<HashMap<String, ValidatedSelection<'a>>, GitStageError> {
    if selections.is_empty() || selections.len() > MAX_STAGE_PATHS {
        return Err(error("Git staging selection count is invalid"));
    }
    let mut selected = HashMap::new();
    for selection in selections {
        let path_plan = plan
            .paths
            .iter()
            .find(|path| path.path == selection.path)
            .ok_or_else(|| error("Git staging selection path is unavailable"))?;
        if selected.contains_key(&selection.path) {
            return Err(error("Git staging selection path is duplicated"));
        }
        if !path_plan.blocking_reasons.is_empty() {
            return Err(error(format!(
                "Git staging path is blocked: {}",
                path_plan.blocking_reasons.join(",")
            )));
        }
        if path_plan.whole_path_required && !selection.whole_path {
            return Err(error("Git staging operation requires whole-path selection"));
        }
        if selection.whole_path && !selection.hunk_ids.is_empty() {
            return Err(error("whole-path Git staging cannot also select hunks"));
        }
        if !selection.whole_path {
            if path_plan.operation != "update" {
                return Err(error("Git staging hunks are supported only for updates"));
            }
            if selection.hunk_ids.is_empty() && !selection.include_mode {
                return Err(error("Git staging selection is empty"));
            }
            let available = path_plan
                .hunks
                .iter()
                .map(|hunk| hunk.id.as_str())
                .collect::<HashSet<_>>();
            let chosen = selection
                .hunk_ids
                .iter()
                .map(String::as_str)
                .collect::<HashSet<_>>();
            if chosen.len() != selection.hunk_ids.len()
                || !chosen.iter().all(|id| available.contains(id))
            {
                return Err(error("Git staging hunk selection is invalid"));
            }
            if selection.include_mode && !path_plan.mode_changed {
                return Err(error("Git staging path has no Agent mode change"));
            }
        }
        selected.insert(
            selection.path.clone(),
            ValidatedSelection {
                path_plan,
                selection,
            },
        );
    }
    Ok(selected)
}

fn apply_selection(
    checkpoint: &GitCheckpoint,
    application: &GitCheckpointApplication,
    runner: &GitRunner,
    lock_path: &Path,
    selected: &ValidatedSelection<'_>,
) -> Result<(), GitStageError> {
    let plan = selected.path_plan;
    match plan.operation.as_str() {
        "create" => {
            let path = application_path(application, &plan.path)?;
            let bytes = read_expected_file(&checkpoint.repository_root, path)?;
            let oid = write_blob(runner, &bytes)?;
            let mode = plan
                .agent_after_mode
                .as_deref()
                .ok_or_else(|| error("Git staging create mode is missing"))?;
            update_index_entry(runner, lock_path, &plan.path, mode, &oid)
        }
        "update" => {
            let path = application_path(application, &plan.path)?;
            let before =
                read_git_checkpoint_preimage(checkpoint, &plan.path).map_err(from_checkpoint)?;
            let after = read_expected_file(&checkpoint.repository_root, path)?;
            let partial = selected_agent_content(&before, &after, plan, selected.selection)?;
            let index_oid = plan
                .index_blob_oid
                .as_deref()
                .ok_or_else(|| error("Git staging index blob is missing"))?;
            let ours = read_blob(runner, index_oid)?;
            let merged = merge_contents(runner, &ours, &before, &partial)?;
            let oid = write_blob(runner, &merged)?;
            let mode = selected_mode(plan, selected.selection)?;
            update_index_entry(runner, lock_path, &plan.path, &mode, &oid)
        }
        "delete" => remove_index_entry(runner, lock_path, &plan.path),
        "rename" => {
            let from_path = plan
                .from_path
                .as_deref()
                .ok_or_else(|| error("Git staging rename source is missing"))?;
            let oid = plan
                .index_blob_oid
                .as_deref()
                .ok_or_else(|| error("Git staging rename blob is missing"))?;
            let mode = plan
                .index_mode
                .as_deref()
                .ok_or_else(|| error("Git staging rename mode is missing"))?;
            remove_index_entry(runner, lock_path, from_path)?;
            update_index_entry(runner, lock_path, &plan.path, mode, oid)
        }
        _ => Err(error("Git staging operation is unsupported")),
    }
}

fn selected_agent_content(
    before: &[u8],
    after: &[u8],
    plan: &GitStagePathPlan,
    selection: &GitStageSelection,
) -> Result<Vec<u8>, GitStageError> {
    if selection.whole_path {
        return Ok(after.to_vec());
    }
    let before_text =
        std::str::from_utf8(before).map_err(|_| error("Git staging preimage is not UTF-8"))?;
    let after_text =
        std::str::from_utf8(after).map_err(|_| error("Git staging result is not UTF-8"))?;
    let diff = TextDiff::from_lines(before_text, after_text);
    let chosen = selection.hunk_ids.iter().collect::<HashSet<_>>();
    let mut output = Vec::with_capacity(before.len().max(after.len()));
    let mut hunk_index = 0_usize;
    for operation in diff.ops() {
        if operation.tag() == DiffTag::Equal {
            for line in operation.old_range() {
                output.extend_from_slice(diff.old_slice(line).unwrap_or_default().as_bytes());
            }
            continue;
        }
        let hunk = plan
            .hunks
            .get(hunk_index)
            .ok_or_else(|| error("Git staging hunk plan is inconsistent"))?;
        hunk_index += 1;
        let is_selected = chosen.contains(&hunk.id);
        let range = if is_selected {
            operation.new_range()
        } else {
            operation.old_range()
        };
        for line in range {
            let value = if is_selected {
                diff.new_slice(line)
            } else {
                diff.old_slice(line)
            };
            output.extend_from_slice(value.unwrap_or_default().as_bytes());
        }
    }
    Ok(output)
}

fn selected_mode(
    plan: &GitStagePathPlan,
    selection: &GitStageSelection,
) -> Result<String, GitStageError> {
    let ours = plan
        .index_mode
        .as_deref()
        .ok_or_else(|| error("Git staging index mode is missing"))?;
    if !selection.whole_path && !selection.include_mode {
        return Ok(ours.into());
    }
    let base = plan
        .before_mode
        .as_deref()
        .ok_or_else(|| error("Git staging base mode is missing"))?;
    let theirs = plan
        .agent_after_mode
        .as_deref()
        .ok_or_else(|| error("Git staging Agent mode is missing"))?;
    if ours == base {
        Ok(theirs.into())
    } else if theirs == base || ours == theirs {
        Ok(ours.into())
    } else {
        Err(error("Agent and user index mode changes conflict"))
    }
}

fn merge_contents(
    runner: &GitRunner,
    ours: &[u8],
    base: &[u8],
    theirs: &[u8],
) -> Result<Vec<u8>, GitStageError> {
    if ours == base || ours == theirs {
        return Ok(theirs.to_vec());
    }
    if theirs == base {
        return Ok(ours.to_vec());
    }
    let files = MergeFiles::create(ours, base, theirs)?;
    let ours_path = path_to_utf8(&files.ours, "merge ours")?;
    let base_path = path_to_utf8(&files.base, "merge base")?;
    let theirs_path = path_to_utf8(&files.theirs, "merge theirs")?;
    let output = runner
        .run(
            &["merge-file", "-p", &ours_path, &base_path, &theirs_path],
            None,
            MAX_FILE_BYTES + 1,
        )
        .map_err(from_git)?;
    if !output.success {
        return Err(error("Agent staging overlaps a user staged change"));
    }
    if output.stdout.len() as u64 > MAX_FILE_BYTES {
        return Err(error("merged Git staging content exceeds size limit"));
    }
    Ok(output.stdout)
}

struct MergeFiles {
    directory: PathBuf,
    ours: PathBuf,
    base: PathBuf,
    theirs: PathBuf,
}

impl MergeFiles {
    fn create(ours: &[u8], base: &[u8], theirs: &[u8]) -> Result<Self, GitStageError> {
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let directory = std::env::temp_dir().join(format!(
            "aegisy-git-stage-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir(&directory).map_err(|_| error("cannot create Git merge workspace"))?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&directory, fs::Permissions::from_mode(0o700))
                .map_err(|_| error("cannot restrict Git merge workspace"))?;
        }
        let files = Self {
            ours: directory.join("ours"),
            base: directory.join("base"),
            theirs: directory.join("theirs"),
            directory,
        };
        write_private(&files.ours, ours)?;
        write_private(&files.base, base)?;
        write_private(&files.theirs, theirs)?;
        Ok(files)
    }
}

impl Drop for MergeFiles {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.directory);
    }
}

fn write_private(path: &Path, bytes: &[u8]) -> Result<(), GitStageError> {
    let mut options = OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    let mut file = options
        .open(path)
        .map_err(|_| error("cannot create Git merge input"))?;
    file.write_all(bytes)
        .map_err(|_| error("cannot write Git merge input"))?;
    file.flush()
        .map_err(|_| error("cannot flush Git merge input"))
}

fn validate_worktree_states(
    checkpoint: &GitCheckpoint,
    application: &GitCheckpointApplication,
) -> Result<(), GitStageError> {
    for path in &application.paths {
        if !worktree_state_matches(&checkpoint.repository_root, path)? {
            return Err(error("Agent result changed while staging"));
        }
    }
    Ok(())
}

fn worktree_state_matches(
    root: &Path,
    path: &GitCheckpointAppliedPath,
) -> Result<bool, GitStageError> {
    let absolute = resolve_path(root, &path.path)?;
    match &path.agent_after {
        GitCheckpointPathState::Absent => match fs::symlink_metadata(absolute) {
            Ok(_) => Ok(false),
            Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => Ok(true),
            Err(_) => Err(error("Agent result path is unavailable")),
        },
        GitCheckpointPathState::File { hash, mode, .. } => {
            let metadata = fs::symlink_metadata(&absolute)
                .map_err(|_| error("Agent result path is unavailable"))?;
            if metadata.file_type().is_symlink() || !metadata.is_file() {
                return Ok(false);
            }
            let bytes = read_bounded(&absolute, MAX_FILE_BYTES, "Agent result")?;
            Ok(ContentHash::for_bytes(&bytes) == *hash && file_mode(&metadata) == *mode)
        }
    }
}

fn read_expected_file(
    root: &Path,
    path: &GitCheckpointAppliedPath,
) -> Result<Vec<u8>, GitStageError> {
    let GitCheckpointPathState::File { hash, mode, .. } = &path.agent_after else {
        return Err(error("Git staging expected a file result"));
    };
    let absolute = resolve_path(root, &path.path)?;
    let metadata =
        fs::symlink_metadata(&absolute).map_err(|_| error("Agent result path is unavailable"))?;
    if metadata.file_type().is_symlink() || !metadata.is_file() || file_mode(&metadata) != *mode {
        return Err(error("Agent result file type or mode changed"));
    }
    let bytes = read_bounded(&absolute, MAX_FILE_BYTES, "Agent result")?;
    if ContentHash::for_bytes(&bytes) != *hash {
        return Err(error("Agent result content changed"));
    }
    Ok(bytes)
}

fn application_path<'a>(
    application: &'a GitCheckpointApplication,
    path: &str,
) -> Result<&'a GitCheckpointAppliedPath, GitStageError> {
    application
        .paths
        .iter()
        .find(|candidate| candidate.path == path)
        .ok_or_else(|| error("Git staging application path is missing"))
}

fn state_hash_mode(state: &GitCheckpointPathState) -> (Option<ContentHash>, Option<String>) {
    match state {
        GitCheckpointPathState::Absent => (None, None),
        GitCheckpointPathState::File { hash, mode, .. } => (Some(hash.clone()), Some(mode.clone())),
    }
}

fn validate_regular_mode(mode: &str, blocking: &mut Vec<String>) {
    if !matches!(mode, "100644" | "100755") {
        blocking.push("unsupported-index-entry-mode".into());
    }
}

fn retain(total: &mut u64, bytes: usize) -> Result<(), GitStageError> {
    *total = total.saturating_add(bytes as u64);
    if *total > MAX_TOTAL_BYTES {
        Err(error("Git staging content budget exceeded"))
    } else {
        Ok(())
    }
}

fn index_entry(runner: &GitRunner, path: &str) -> Result<Option<IndexEntry>, GitStageError> {
    let output = runner
        .run(
            &["ls-files", "--stage", "-z", "--", path],
            None,
            MAX_GIT_OUTPUT,
        )
        .map_err(from_git)?;
    require_success(&output, "Git index entry query failed")?;
    let records = output
        .stdout
        .split(|byte| *byte == 0)
        .filter(|record| !record.is_empty())
        .collect::<Vec<_>>();
    if records.is_empty() {
        return Ok(None);
    }
    if records.len() != 1 {
        return Err(error("Git index path has unresolved stages"));
    }
    let record =
        std::str::from_utf8(records[0]).map_err(|_| error("Git index entry is not UTF-8"))?;
    let (metadata, record_path) = record
        .split_once('\t')
        .ok_or_else(|| error("Git index entry is malformed"))?;
    if record_path != path {
        return Err(error("Git index returned an unexpected path"));
    }
    let mut fields = metadata.split(' ');
    let mode = fields.next().unwrap_or_default();
    let oid = fields.next().unwrap_or_default();
    let stage = fields.next().unwrap_or_default();
    if fields.next().is_some()
        || !matches!(mode, "100644" | "100755" | "120000" | "160000")
        || !valid_oid(oid)
        || stage != "0"
    {
        return Err(error("Git index entry is invalid or conflicted"));
    }
    Ok(Some(IndexEntry {
        mode: mode.into(),
        oid: oid.into(),
    }))
}

fn tree_entry(
    runner: &GitRunner,
    tree_oid: &str,
    path: &str,
) -> Result<Option<IndexEntry>, GitStageError> {
    if !valid_oid(tree_oid) {
        return Err(error("Git checkpoint index tree object ID is invalid"));
    }
    let output = runner
        .run(
            &["ls-tree", "-z", tree_oid, "--", path],
            None,
            MAX_GIT_OUTPUT,
        )
        .map_err(from_git)?;
    require_success(&output, "Git checkpoint index entry query failed")?;
    let records = output
        .stdout
        .split(|byte| *byte == 0)
        .filter(|record| !record.is_empty())
        .collect::<Vec<_>>();
    if records.is_empty() {
        return Ok(None);
    }
    if records.len() != 1 {
        return Err(error("Git checkpoint index path is ambiguous"));
    }
    let record = std::str::from_utf8(records[0])
        .map_err(|_| error("Git checkpoint index entry is not UTF-8"))?;
    let (metadata, record_path) = record
        .split_once('\t')
        .ok_or_else(|| error("Git checkpoint index entry is malformed"))?;
    if record_path != path {
        return Err(error("Git checkpoint index returned an unexpected path"));
    }
    let mut fields = metadata.split(' ');
    let mode = fields.next().unwrap_or_default();
    let object_type = fields.next().unwrap_or_default();
    let oid = fields.next().unwrap_or_default();
    if fields.next().is_some()
        || object_type != "blob"
        || !matches!(mode, "100644" | "100755" | "120000")
        || !valid_oid(oid)
    {
        return Err(error("Git checkpoint index entry is invalid"));
    }
    Ok(Some(IndexEntry {
        mode: mode.into(),
        oid: oid.into(),
    }))
}

fn update_index_entry(
    runner: &GitRunner,
    index_path: &Path,
    path: &str,
    mode: &str,
    oid: &str,
) -> Result<(), GitStageError> {
    let cache_info = format!("{mode},{oid},{path}");
    let output = runner
        .run_with_index(
            &["update-index", "--add", "--cacheinfo", &cache_info],
            None,
            MAX_GIT_OUTPUT,
            index_path,
        )
        .map_err(from_git)?;
    require_success(&output, "Git temporary index update failed")
}

fn remove_index_entry(
    runner: &GitRunner,
    index_path: &Path,
    path: &str,
) -> Result<(), GitStageError> {
    let output = runner
        .run_with_index(
            &["update-index", "--force-remove", "--", path],
            None,
            MAX_GIT_OUTPUT,
            index_path,
        )
        .map_err(from_git)?;
    require_success(&output, "Git temporary index removal failed")
}

fn write_blob(runner: &GitRunner, bytes: &[u8]) -> Result<String, GitStageError> {
    let output = runner
        .run(&["hash-object", "-w", "--stdin"], Some(bytes), 1024)
        .map_err(from_git)?;
    require_success(&output, "Git staging blob write failed")?;
    parse_oid(&output.stdout)
}

fn read_blob(runner: &GitRunner, oid: &str) -> Result<Vec<u8>, GitStageError> {
    if !valid_oid(oid) {
        return Err(error("Git staging blob object ID is invalid"));
    }
    let output = runner
        .run(&["cat-file", "blob", oid], None, MAX_FILE_BYTES + 1)
        .map_err(from_git)?;
    require_success(&output, "Git staging blob read failed")?;
    if output.stdout.len() as u64 > MAX_FILE_BYTES {
        return Err(error("Git staging index blob exceeds size limit"));
    }
    Ok(output.stdout)
}

fn discover_index_path(
    runner: &GitRunner,
    checkpoint: &GitCheckpoint,
) -> Result<PathBuf, GitStageError> {
    let output = runner
        .run(
            &["rev-parse", "--path-format=absolute", "--git-path", "index"],
            None,
            16 * 1024,
        )
        .map_err(from_git)?;
    require_success(&output, "Git index path query failed")?;
    let value = std::str::from_utf8(&output.stdout)
        .map_err(|_| error("Git index path is not UTF-8"))?
        .trim();
    let path = PathBuf::from(value);
    if !path.is_absolute() || path.file_name().and_then(|name| name.to_str()) != Some("index") {
        return Err(error("Git returned an invalid index path"));
    }
    let parent = path
        .parent()
        .and_then(|parent| parent.canonicalize().ok())
        .ok_or_else(|| error("Git index directory is unavailable"))?;
    if parent != checkpoint.git_directory {
        return Err(error("Git index is outside authorized metadata"));
    }
    let metadata = fs::symlink_metadata(&path).map_err(|_| error("Git index is unavailable"))?;
    if metadata.file_type().is_symlink() || !metadata.is_file() {
        return Err(error("Git index is not a regular file"));
    }
    Ok(path)
}

fn required_oid(
    runner: &GitRunner,
    args: &[&str],
    index: Option<&Path>,
) -> Result<String, GitStageError> {
    let output = match index {
        Some(index) => runner.run_with_index(args, None, 1024, index),
        None => runner.run(args, None, 1024),
    }
    .map_err(from_git)?;
    require_success(&output, "Git object query failed")?;
    parse_oid(&output.stdout)
}

fn parse_oid(bytes: &[u8]) -> Result<String, GitStageError> {
    let oid = std::str::from_utf8(bytes)
        .map_err(|_| error("Git object ID is not UTF-8"))?
        .trim();
    if !valid_oid(oid) {
        return Err(error("Git object ID is invalid"));
    }
    Ok(oid.into())
}

fn valid_oid(oid: &str) -> bool {
    matches!(oid.len(), 40 | 64)
        && oid
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
}

fn require_success(output: &GitOutput, message: &str) -> Result<(), GitStageError> {
    if output.success {
        Ok(())
    } else {
        Err(error(message))
    }
}

fn read_bounded(path: &Path, limit: u64, label: &str) -> Result<Vec<u8>, GitStageError> {
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

fn resolve_path(root: &Path, relative: &str) -> Result<PathBuf, GitStageError> {
    let path = Path::new(relative);
    if path.is_absolute()
        || path
            .components()
            .any(|component| !matches!(component, Component::Normal(_)))
    {
        return Err(error("Git staging path is invalid"));
    }
    let mut current = root.to_path_buf();
    for component in path.components() {
        current.push(component.as_os_str());
        if let Ok(metadata) = fs::symlink_metadata(&current) {
            if metadata.file_type().is_symlink() {
                return Err(error("Git staging path crosses a symlink"));
            }
        }
    }
    Ok(current)
}

fn validate_pending_paths(paths: &HashSet<String>) -> Result<(), GitStageError> {
    if paths.len() > 5_000
        || paths.iter().any(|path| {
            path.len() > 4 * 1024
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

fn index_lock_path(index_path: &Path) -> PathBuf {
    let mut value = index_path.as_os_str().to_os_string();
    value.push(".lock");
    PathBuf::from(value)
}

fn with_index_rollback(
    cause: GitStageError,
    index_path: &Path,
    original: &[u8],
    installed_hash: &ContentHash,
) -> GitStageError {
    GitStageError {
        message: cause.message,
        rollback_complete: restore_index(index_path, original, installed_hash).is_ok(),
    }
}

fn restore_index(
    index_path: &Path,
    original: &[u8],
    installed_hash: &ContentHash,
) -> Result<(), GitStageError> {
    let current = read_bounded(index_path, MAX_INDEX_BYTES, "Git index")?;
    if ContentHash::for_bytes(&current) != *installed_hash {
        return Err(error("Git index changed before rollback"));
    }
    let lock_path = index_lock_path(index_path);
    let permissions = fs::metadata(index_path)
        .map_err(|_| error("Git index rollback metadata is unavailable"))?
        .permissions();
    let mut lock = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&lock_path)
        .map_err(|_| error("Git index rollback lock is unavailable"))?;
    lock.write_all(original)
        .map_err(|_| error("cannot write Git index rollback"))?;
    lock.set_permissions(permissions)
        .map_err(|_| error("cannot preserve Git index rollback permissions"))?;
    lock.flush()
        .map_err(|_| error("cannot flush Git index rollback"))?;
    lock.sync_all()
        .map_err(|_| error("cannot sync Git index rollback"))?;
    drop(lock);
    atomic_replace(&lock_path, index_path)
        .map_err(|_| error("cannot atomically restore Git index"))?;
    sync_parent(index_path)
}

#[cfg(unix)]
fn sync_parent(path: &Path) -> Result<(), GitStageError> {
    let parent = path
        .parent()
        .ok_or_else(|| error("Git index directory is unavailable"))?;
    File::open(parent)
        .and_then(|directory| directory.sync_all())
        .map_err(|_| error("cannot sync Git index directory"))
}

#[cfg(not(unix))]
fn sync_parent(_path: &Path) -> Result<(), GitStageError> {
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

#[cfg(unix)]
fn file_mode(metadata: &fs::Metadata) -> String {
    use std::os::unix::fs::PermissionsExt;
    if metadata.permissions().mode() & 0o111 != 0 {
        "100755".into()
    } else {
        "100644".into()
    }
}

#[cfg(not(unix))]
fn file_mode(_metadata: &fs::Metadata) -> String {
    "100644".into()
}

fn path_to_utf8(path: &Path, label: &str) -> Result<String, GitStageError> {
    path.to_str()
        .map(str::to_owned)
        .ok_or_else(|| error(format!("{label} is not UTF-8")))
}

fn from_checkpoint(cause: crate::git_checkpoint::GitCheckpointError) -> GitStageError {
    error(cause.message)
}

fn from_git(cause: GitStatusError) -> GitStageError {
    error(cause.message)
}

fn error(message: impl Into<String>) -> GitStageError {
    GitStageError {
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
    use crate::workspace_edit::{ProposedContent, WorkspaceEdit, WorkspaceEditOperation};
    use crate::workspace_edit_apply::apply_workspace_edit;
    use crate::workspace_edit_preview::ContentInput;
    use std::process::{Command, Stdio};

    struct FailAfterInstall;

    impl FaultInjector for FailAfterInstall {
        fn after_install(&self) -> Result<(), GitStageError> {
            Err(error("injected failure after index install"))
        }
    }

    struct RewriteIndexAfterInstall {
        root: PathBuf,
    }

    impl FaultInjector for RewriteIndexAfterInstall {
        fn after_install(&self) -> Result<(), GitStageError> {
            let output = Command::new("git")
                .arg("-C")
                .arg(&self.root)
                .args(["add", "other.txt"])
                .output()
                .map_err(|_| error("cannot inject external Git index rewrite"))?;
            if !output.status.success() {
                return Err(error("injected external Git index rewrite failed"));
            }
            Err(error("injected failure after external Git index rewrite"))
        }
    }

    fn root() -> PathBuf {
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "aegisy-git-stage-test-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(&root).unwrap();
        git(&root, &["init", "-q"]);
        git(&root, &["config", "user.name", "Aegisy Test"]);
        git(&root, &["config", "user.email", "test@aegisy.local"]);
        root.canonicalize().unwrap()
    }

    fn commit_all(root: &Path) {
        git(root, &["add", "."]);
        git(root, &["commit", "-q", "-m", "initial"]);
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

    fn bind(
        _root: &Path,
        edit: &WorkspaceEdit,
        contents: Vec<ContentInput>,
    ) -> (GitCheckpoint, GitCheckpointApplication) {
        let checkpoint_id = format!("checkpoint-{}", edit.edit_id);
        let checkpoint =
            capture_git_checkpoint(&checkpoint_id, edit, &HashSet::new(), &HashSet::new()).unwrap();
        let applied = apply_workspace_edit(edit.clone(), contents, &HashSet::new()).unwrap();
        let application = bind_git_checkpoint_application(&checkpoint, edit, &applied).unwrap();
        (checkpoint, application)
    }

    fn whole(path: &str) -> GitStageSelection {
        GitStageSelection {
            path: path.into(),
            whole_path: true,
            hunk_ids: Vec::new(),
            include_mode: false,
        }
    }

    #[test]
    fn stages_selected_agent_hunk_without_user_unstaged_or_unselected_agent_content() {
        let root = root();
        let committed = "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\n";
        let staged_user = "one\ntwo\nthree\nfour\nfive\nsix\nuser-staged\neight\nnine\n";
        let agent_base = "one\nuser-unstaged\nthree\nfour\nfive\nsix\nuser-staged\neight\nnine\n";
        let agent_after =
            "one\nuser-unstaged\nthree\nagent-first\nfive\nsix\nuser-staged\neight\nagent-second\n";
        fs::write(root.join("file.txt"), committed).unwrap();
        commit_all(&root);
        fs::write(root.join("file.txt"), staged_user).unwrap();
        git(&root, &["add", "file.txt"]);
        fs::write(root.join("file.txt"), agent_base).unwrap();

        let proposed = ProposedContent::for_bytes(agent_after.as_bytes());
        let edit = WorkspaceEdit::define(
            "stage-edit",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Update {
                path: "file.txt".into(),
                base: ContentHash::for_bytes(agent_base.as_bytes()),
                content: proposed.clone(),
            }],
        )
        .unwrap();
        let (checkpoint, application) = bind(
            &root,
            &edit,
            vec![ContentInput {
                reference: proposed.reference,
                content: agent_after.into(),
            }],
        );
        let plan = plan_agent_staging(&checkpoint, &application, &HashSet::new()).unwrap();
        assert!(plan.blocking_reasons.is_empty());
        assert!(plan.paths[0].blocking_reasons.is_empty());
        assert_eq!(plan.paths[0].hunks.len(), 2);
        let selection = GitStageSelection {
            path: "file.txt".into(),
            whole_path: false,
            hunk_ids: vec![plan.paths[0].hunks[0].id.clone()],
            include_mode: false,
        };
        stage_agent_changes(
            &checkpoint,
            &application,
            &HashSet::new(),
            &plan,
            &[selection],
        )
        .unwrap();

        assert_eq!(
            text(&root, &["show", ":file.txt"]),
            "one\ntwo\nthree\nagent-first\nfive\nsix\nuser-staged\neight\nnine\n"
        );
        assert_eq!(
            fs::read_to_string(root.join("file.txt")).unwrap(),
            agent_after
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn stages_create_delete_and_rename_without_changing_worktree() {
        let root = root();
        fs::write(root.join("delete.txt"), "delete-base\n").unwrap();
        fs::write(root.join("old.txt"), "rename-base\n").unwrap();
        fs::write(root.join("keep.txt"), "keep\n").unwrap();
        commit_all(&root);
        fs::write(root.join("delete.txt"), "delete-user-unstaged\n").unwrap();
        fs::write(root.join("old.txt"), "rename-user-staged\n").unwrap();
        git(&root, &["add", "old.txt"]);

        let created = ProposedContent::for_bytes(b"created-agent\n");
        let edit = WorkspaceEdit::define(
            "stage-path-edit",
            "project-1",
            &root,
            vec![
                WorkspaceEditOperation::Create {
                    path: "created.txt".into(),
                    content: created.clone(),
                },
                WorkspaceEditOperation::Delete {
                    path: "delete.txt".into(),
                    base: ContentHash::for_bytes(b"delete-user-unstaged\n"),
                },
                WorkspaceEditOperation::Rename {
                    from_path: "old.txt".into(),
                    to_path: "new.txt".into(),
                    base: ContentHash::for_bytes(b"rename-user-staged\n"),
                },
            ],
        )
        .unwrap();
        let (checkpoint, application) = bind(
            &root,
            &edit,
            vec![ContentInput {
                reference: created.reference,
                content: "created-agent\n".into(),
            }],
        );
        let plan = plan_agent_staging(&checkpoint, &application, &HashSet::new()).unwrap();
        assert!(plan
            .paths
            .iter()
            .all(|path| path.blocking_reasons.is_empty()));
        let selections = [whole("created.txt"), whole("delete.txt"), whole("new.txt")];
        stage_agent_changes(
            &checkpoint,
            &application,
            &HashSet::new(),
            &plan,
            &selections,
        )
        .unwrap();

        assert_eq!(text(&root, &["show", ":created.txt"]), "created-agent\n");
        assert!(!Command::new("git")
            .arg("-C")
            .arg(&root)
            .args(["show", ":delete.txt"])
            .output()
            .unwrap()
            .status
            .success());
        assert_eq!(text(&root, &["show", ":new.txt"]), "rename-user-staged\n");
        assert!(!text(&root, &["ls-files", "--", "old.txt"]).contains("old.txt"));
        assert_eq!(
            fs::read_to_string(root.join("created.txt")).unwrap(),
            "created-agent\n"
        );
        assert!(!root.join("delete.txt").exists());
        assert!(!root.join("old.txt").exists());
        assert_eq!(
            fs::read_to_string(root.join("new.txt")).unwrap(),
            "rename-user-staged\n"
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_overlap_stale_index_pending_editor_and_lock_competition() {
        let root = root();
        fs::write(root.join("file.txt"), "one\ntwo\nthree\n").unwrap();
        fs::write(root.join("other.txt"), "other\n").unwrap();
        commit_all(&root);
        let proposed = ProposedContent::for_bytes(b"one\nagent\nthree\n");
        let edit = WorkspaceEdit::define(
            "stage-block-edit",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Update {
                path: "file.txt".into(),
                base: ContentHash::for_bytes(b"one\ntwo\nthree\n"),
                content: proposed.clone(),
            }],
        )
        .unwrap();
        let (checkpoint, application) = bind(
            &root,
            &edit,
            vec![ContentInput {
                reference: proposed.reference,
                content: "one\nagent\nthree\n".into(),
            }],
        );

        fs::write(root.join("file.txt"), "one\nuser-overlap\nthree\n").unwrap();
        git(&root, &["add", "file.txt"]);
        fs::write(root.join("file.txt"), "one\nagent\nthree\n").unwrap();
        let overlap = plan_agent_staging(&checkpoint, &application, &HashSet::new()).unwrap();
        assert_eq!(
            overlap,
            plan_agent_staging(&checkpoint, &application, &HashSet::new()).unwrap()
        );
        let failure = stage_agent_changes(
            &checkpoint,
            &application,
            &HashSet::new(),
            &overlap,
            &[whole("file.txt")],
        )
        .unwrap_err();
        assert!(failure.message.contains("overlaps"), "{}", failure.message);
        assert_eq!(
            text(&root, &["show", ":file.txt"]),
            "one\nuser-overlap\nthree\n"
        );

        git(&root, &["reset", "-q", "HEAD", "--", "file.txt"]);
        let plan = plan_agent_staging(&checkpoint, &application, &HashSet::new()).unwrap();
        fs::write(root.join("other.txt"), "other staged\n").unwrap();
        git(&root, &["add", "other.txt"]);
        assert!(stage_agent_changes(
            &checkpoint,
            &application,
            &HashSet::new(),
            &plan,
            &[whole("file.txt")],
        )
        .unwrap_err()
        .message
        .contains("stale"));
        git(&root, &["reset", "-q", "HEAD", "--", "other.txt"]);

        let pending = HashSet::from(["file.txt".into()]);
        let pending_plan = plan_agent_staging(&checkpoint, &application, &pending).unwrap();
        assert!(pending_plan.paths[0]
            .blocking_reasons
            .contains(&"pending-editor-edit".into()));

        let plan = plan_agent_staging(&checkpoint, &application, &HashSet::new()).unwrap();
        fs::write(index_lock_path(Path::new(&plan.index_path)), b"occupied").unwrap();
        let lock_failure = stage_agent_changes(
            &checkpoint,
            &application,
            &HashSet::new(),
            &plan,
            &[whole("file.txt")],
        )
        .unwrap_err();
        assert!(
            lock_failure.message.contains("locked"),
            "{}",
            lock_failure.message
        );
        fs::remove_file(index_lock_path(Path::new(&plan.index_path))).unwrap();
        fs::remove_dir_all(root).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn stages_only_agent_mode_change_and_blocks_later_staged_delete_content() {
        let root = root();
        fs::write(root.join("mode.txt"), "mode\n").unwrap();
        fs::write(root.join("delete.txt"), "delete\n").unwrap();
        commit_all(&root);

        let mode_content = ProposedContent::for_bytes(b"mode\n").with_mode("executable");
        let mode_edit = WorkspaceEdit::define(
            "stage-mode-edit",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Update {
                path: "mode.txt".into(),
                base: ContentHash::for_bytes(b"mode\n"),
                content: mode_content.clone(),
            }],
        )
        .unwrap();
        let (mode_checkpoint, mode_application) = bind(
            &root,
            &mode_edit,
            vec![ContentInput {
                reference: mode_content.reference,
                content: "mode\n".into(),
            }],
        );
        let mode_plan =
            plan_agent_staging(&mode_checkpoint, &mode_application, &HashSet::new()).unwrap();
        assert!(mode_plan.paths[0].hunks.is_empty());
        assert!(mode_plan.paths[0].mode_changed);
        stage_agent_changes(
            &mode_checkpoint,
            &mode_application,
            &HashSet::new(),
            &mode_plan,
            &[GitStageSelection {
                path: "mode.txt".into(),
                whole_path: false,
                hunk_ids: Vec::new(),
                include_mode: true,
            }],
        )
        .unwrap();
        assert!(text(&root, &["ls-files", "--stage", "--", "mode.txt"]).starts_with("100755 "));

        git(&root, &["reset", "-q", "HEAD", "--", "mode.txt"]);
        let delete_edit = WorkspaceEdit::define(
            "stage-delete-edit",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Delete {
                path: "delete.txt".into(),
                base: ContentHash::for_bytes(b"delete\n"),
            }],
        )
        .unwrap();
        let (delete_checkpoint, delete_application) = bind(&root, &delete_edit, Vec::new());
        fs::write(root.join("delete.txt"), "later staged user content\n").unwrap();
        git(&root, &["add", "delete.txt"]);
        fs::remove_file(root.join("delete.txt")).unwrap();
        let delete_plan =
            plan_agent_staging(&delete_checkpoint, &delete_application, &HashSet::new()).unwrap();
        assert!(delete_plan.paths[0]
            .blocking_reasons
            .contains(&"delete-index-changed-after-checkpoint".into()));
        assert!(stage_agent_changes(
            &delete_checkpoint,
            &delete_application,
            &HashSet::new(),
            &delete_plan,
            &[whole("delete.txt")],
        )
        .is_err());
        assert_eq!(
            text(&root, &["show", ":delete.txt"]),
            "later staged user content\n"
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn injected_post_install_failure_restores_exact_index_and_preserves_worktree() {
        let root = root();
        fs::write(root.join("file.txt"), "before\n").unwrap();
        commit_all(&root);
        let proposed = ProposedContent::for_bytes(b"after\n");
        let edit = WorkspaceEdit::define(
            "stage-rollback-edit",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Update {
                path: "file.txt".into(),
                base: ContentHash::for_bytes(b"before\n"),
                content: proposed.clone(),
            }],
        )
        .unwrap();
        let (checkpoint, application) = bind(
            &root,
            &edit,
            vec![ContentInput {
                reference: proposed.reference,
                content: "after\n".into(),
            }],
        );
        let plan = plan_agent_staging(&checkpoint, &application, &HashSet::new()).unwrap();
        let index_before = fs::read(&plan.index_path).unwrap();
        let failure = stage_with_fault(
            &checkpoint,
            &application,
            &HashSet::new(),
            &plan,
            &[whole("file.txt")],
            &FailAfterInstall,
        )
        .unwrap_err();
        assert!(failure.rollback_complete);
        assert_eq!(fs::read(&plan.index_path).unwrap(), index_before);
        assert_eq!(text(&root, &["show", ":file.txt"]), "before\n");
        assert_eq!(
            fs::read_to_string(root.join("file.txt")).unwrap(),
            "after\n"
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rollback_preserves_an_external_index_rewrite_and_reports_incomplete() {
        let root = root();
        fs::write(root.join("file.txt"), "before\n").unwrap();
        fs::write(root.join("other.txt"), "other before\n").unwrap();
        commit_all(&root);
        let proposed = ProposedContent::for_bytes(b"after\n");
        let edit = WorkspaceEdit::define(
            "stage-external-rewrite-edit",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Update {
                path: "file.txt".into(),
                base: ContentHash::for_bytes(b"before\n"),
                content: proposed.clone(),
            }],
        )
        .unwrap();
        let (checkpoint, application) = bind(
            &root,
            &edit,
            vec![ContentInput {
                reference: proposed.reference,
                content: "after\n".into(),
            }],
        );
        fs::write(root.join("other.txt"), "external staged\n").unwrap();
        let plan = plan_agent_staging(&checkpoint, &application, &HashSet::new()).unwrap();
        let failure = stage_with_fault(
            &checkpoint,
            &application,
            &HashSet::new(),
            &plan,
            &[whole("file.txt")],
            &RewriteIndexAfterInstall { root: root.clone() },
        )
        .unwrap_err();
        assert!(!failure.rollback_complete);
        let staged = text(&root, &["diff", "--cached", "--name-only"]);
        assert!(staged.lines().any(|path| path == "file.txt"));
        assert!(staged.lines().any(|path| path == "other.txt"));
        assert_eq!(text(&root, &["show", ":file.txt"]), "after\n");
        assert_eq!(text(&root, &["show", ":other.txt"]), "external staged\n");
        fs::remove_dir_all(root).unwrap();
    }
}
