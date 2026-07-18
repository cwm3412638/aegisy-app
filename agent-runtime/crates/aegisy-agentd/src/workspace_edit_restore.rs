use crate::git_checkpoint::{
    bind_git_checkpoint_application, read_git_checkpoint_preimage, GitCheckpoint,
};
use crate::non_git_checkpoint::{NonGitCheckpointDescriptor, NonGitCheckpointStore};
use crate::workspace_edit::{
    platform_supports_file_mode, ContentHash, ProposedContent, WorkspaceEdit,
    WorkspaceEditOperation,
};
use crate::workspace_edit_apply::{
    apply_workspace_edit, WorkspaceEditApplyFailure, WorkspaceEditApplyResult,
};
use crate::workspace_edit_overlap::{
    detect_workspace_edit_overlaps, restore_overlap_baseline, ExpectedWorkspacePathState,
    ObservedWorkspacePathState, WorkspaceEditOverlapBaseline,
};
use crate::workspace_edit_preview::ContentInput;
use serde::Serialize;
use std::collections::{HashMap, HashSet};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorkspaceRestoreError {
    pub message: String,
    pub apply_failure: Option<Box<WorkspaceEditApplyFailure>>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub enum WorkspaceRestoreSelection {
    AllAgentChanges,
    Operations { indices: Vec<usize> },
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceRestoreReview {
    pub checkpoint_kind: String,
    pub checkpoint_id: String,
    pub edit_id: String,
    pub project_id: String,
    pub root_identity: String,
    pub selection: WorkspaceRestoreSelection,
    pub blocking: bool,
    pub conflict_count: usize,
    pub paths: Vec<WorkspaceRestorePathReview>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceRestorePathReview {
    pub operation_index: usize,
    pub operation: String,
    pub role: String,
    pub path: String,
    pub desired: ExpectedWorkspacePathState,
    pub current: ObservedWorkspacePathState,
    pub status: String,
    pub conflict: bool,
    pub pending_user_edit: bool,
    pub confirmable: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceRestoreConflictConfirmation {
    pub path: String,
    pub current: ObservedWorkspacePathState,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceRestoreResult {
    pub checkpoint_kind: String,
    pub checkpoint_id: String,
    pub restored_paths: Vec<String>,
    pub no_changes: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub apply: Option<WorkspaceEditApplyResult>,
}

pub struct WorkspaceRestorePlan {
    checkpoint_kind: String,
    checkpoint_id: String,
    edit: WorkspaceEdit,
    after: WorkspaceEditOverlapBaseline,
    operations: Vec<RestoreOperation>,
}

struct RestoreOperation {
    targets: Vec<RestoreTarget>,
}

struct RestoreTarget {
    operation: String,
    role: String,
    path: String,
    desired: RestoreDesiredState,
}

enum RestoreDesiredState {
    Absent,
    File {
        hash: ContentHash,
        bytes: Vec<u8>,
        mode: String,
    },
}

pub fn prepare_git_restore_plan(
    checkpoint: &GitCheckpoint,
    edit: &WorkspaceEdit,
    applied: &WorkspaceEditApplyResult,
) -> Result<WorkspaceRestorePlan, WorkspaceRestoreError> {
    let application = bind_git_checkpoint_application(checkpoint, edit, applied)
        .map_err(|cause| error(cause.message))?;
    let mut preimages = HashMap::new();
    for path in &application.paths {
        if matches!(
            path.before,
            crate::git_checkpoint::GitCheckpointPathState::File { .. }
        ) {
            let bytes = read_git_checkpoint_preimage(checkpoint, &path.path)
                .map_err(|cause| error(cause.message))?;
            let mode = match &path.before {
                crate::git_checkpoint::GitCheckpointPathState::File { mode, .. } => mode.clone(),
                crate::git_checkpoint::GitCheckpointPathState::Absent => unreachable!(),
            };
            preimages.insert(path.path.clone(), (bytes, mode));
        }
    }
    build_plan("git", &checkpoint.checkpoint_id, edit, applied, preimages)
}

pub fn prepare_non_git_restore_plan(
    store: &NonGitCheckpointStore,
    descriptor: &NonGitCheckpointDescriptor,
    edit: &WorkspaceEdit,
    applied: &WorkspaceEditApplyResult,
) -> Result<WorkspaceRestorePlan, WorkspaceRestoreError> {
    let application = store
        .bind_application(descriptor, edit, applied)
        .map_err(|cause| error(cause.message))?;
    let mut preimages = HashMap::new();
    for path in &application.paths {
        if let crate::non_git_checkpoint::NonGitCheckpointPathState::File { mode, .. } =
            &path.before
        {
            let bytes = store
                .read_preimage(descriptor, &path.path)
                .map_err(|cause| error(cause.message))?;
            preimages.insert(path.path.clone(), (bytes, mode.clone()));
        }
    }
    build_plan(
        "non-git",
        &descriptor.checkpoint_id,
        edit,
        applied,
        preimages,
    )
}

pub fn review_workspace_restore(
    plan: &WorkspaceRestorePlan,
    selection: WorkspaceRestoreSelection,
    ignored_paths: &HashSet<String>,
    pending_user_paths: &HashSet<String>,
) -> Result<WorkspaceRestoreReview, WorkspaceRestoreError> {
    let selected = selected_operations(&selection, plan.operations.len())?;
    let overlap =
        detect_workspace_edit_overlaps(&plan.edit, &plan.after, ignored_paths, pending_user_paths)
            .map_err(|cause| error(cause.message))?;
    let overlap_by_path = overlap
        .paths
        .into_iter()
        .map(|path| (path.path.clone(), path))
        .collect::<HashMap<_, _>>();
    let mut paths = Vec::new();
    for index in selected {
        for target in &plan.operations[index].targets {
            let overlap = overlap_by_path
                .get(&target.path)
                .ok_or_else(|| error("restore overlap report is incomplete"))?;
            let desired = target.desired.expected();
            let already_restored = observed_matches_expected(&overlap.current, &desired);
            let pending_user_edit = overlap.pending_user_edit;
            let conflict = pending_user_edit || (!already_restored && overlap.overlaps);
            let status = if pending_user_edit {
                "pending-user-edit".into()
            } else if already_restored {
                "already-restored".into()
            } else if conflict {
                overlap.status.clone()
            } else {
                "ready".into()
            };
            let confirmable = conflict
                && !pending_user_edit
                && matches!(
                    overlap.current,
                    ObservedWorkspacePathState::Absent | ObservedWorkspacePathState::File { .. }
                );
            paths.push(WorkspaceRestorePathReview {
                operation_index: index,
                operation: target.operation.clone(),
                role: target.role.clone(),
                path: target.path.clone(),
                desired,
                current: overlap.current.clone(),
                status,
                conflict,
                pending_user_edit,
                confirmable,
            });
        }
    }
    let conflict_count = paths.iter().filter(|path| path.conflict).count();
    Ok(WorkspaceRestoreReview {
        checkpoint_kind: plan.checkpoint_kind.clone(),
        checkpoint_id: plan.checkpoint_id.clone(),
        edit_id: plan.edit.edit_id.clone(),
        project_id: plan.edit.project_id.clone(),
        root_identity: plan.edit.root.identity.clone(),
        selection,
        blocking: conflict_count != 0,
        conflict_count,
        paths,
    })
}

pub fn execute_workspace_restore(
    plan: &WorkspaceRestorePlan,
    review: &WorkspaceRestoreReview,
    confirmations: &[WorkspaceRestoreConflictConfirmation],
    ignored_paths: &HashSet<String>,
    pending_user_paths: &HashSet<String>,
) -> Result<WorkspaceRestoreResult, Box<WorkspaceRestoreError>> {
    let fresh = review_workspace_restore(
        plan,
        review.selection.clone(),
        ignored_paths,
        pending_user_paths,
    )
    .map_err(Box::new)?;
    if fresh != *review {
        return Err(Box::new(error(
            "restore review is stale or was modified; review again",
        )));
    }
    for path in &fresh.paths {
        if !path.conflict {
            continue;
        }
        if !path.confirmable
            || !confirmations.iter().any(|confirmation| {
                confirmation.path == path.path && confirmation.current == path.current
            })
        {
            return Err(Box::new(error(
                "restore conflict requires an exact current-state confirmation",
            )));
        }
    }

    let selected =
        selected_operations(&fresh.selection, plan.operations.len()).map_err(Box::new)?;
    let current = fresh
        .paths
        .iter()
        .map(|path| (path.path.clone(), path.current.clone()))
        .collect::<HashMap<_, _>>();
    let mut operations = Vec::new();
    let mut contents = HashMap::<String, ContentInput>::new();
    let mut restored_paths = Vec::new();
    for index in selected {
        for target in &plan.operations[index].targets {
            let state = current
                .get(&target.path)
                .ok_or_else(|| Box::new(error("restore current state is incomplete")))?;
            compile_target(
                target,
                state,
                &mut operations,
                &mut contents,
                &mut restored_paths,
            )?;
        }
    }
    restored_paths.sort();
    restored_paths.dedup();
    if operations.is_empty() {
        return Ok(WorkspaceRestoreResult {
            checkpoint_kind: plan.checkpoint_kind.clone(),
            checkpoint_id: plan.checkpoint_id.clone(),
            restored_paths,
            no_changes: true,
            apply: None,
        });
    }
    let restore_edit = WorkspaceEdit::define(
        format!("restore-{}", plan.checkpoint_id),
        plan.edit.project_id.clone(),
        &plan.edit.root.canonical_path,
        operations,
    )
    .map_err(|cause| Box::new(error(cause.message)))?;
    let apply = apply_workspace_edit(
        restore_edit,
        contents.into_values().collect(),
        ignored_paths,
    )
    .map_err(|failure| {
        Box::new(WorkspaceRestoreError {
            message: "atomic workspace restore failed".into(),
            apply_failure: Some(failure),
        })
    })?;
    Ok(WorkspaceRestoreResult {
        checkpoint_kind: plan.checkpoint_kind.clone(),
        checkpoint_id: plan.checkpoint_id.clone(),
        restored_paths,
        no_changes: false,
        apply: Some(apply),
    })
}

fn build_plan(
    checkpoint_kind: &str,
    checkpoint_id: &str,
    edit: &WorkspaceEdit,
    applied: &WorkspaceEditApplyResult,
    mut preimages: HashMap<String, (Vec<u8>, String)>,
) -> Result<WorkspaceRestorePlan, WorkspaceRestoreError> {
    let after = restore_overlap_baseline(edit, applied).map_err(|cause| error(cause.message))?;
    let mut operations = Vec::with_capacity(edit.operations.len());
    for operation in &edit.operations {
        let targets = match operation {
            WorkspaceEditOperation::Create { path, .. } => vec![target(
                "create",
                "target",
                path,
                RestoreDesiredState::Absent,
            )],
            WorkspaceEditOperation::Update { path, base, .. } => vec![target(
                "update",
                "target",
                path,
                preimage(path, base, &mut preimages)?,
            )],
            WorkspaceEditOperation::Delete { path, base } => vec![target(
                "delete",
                "target",
                path,
                preimage(path, base, &mut preimages)?,
            )],
            WorkspaceEditOperation::Rename {
                from_path,
                to_path,
                base,
            } => vec![
                target(
                    "rename",
                    "source",
                    from_path,
                    preimage(from_path, base, &mut preimages)?,
                ),
                target("rename", "target", to_path, RestoreDesiredState::Absent),
            ],
        };
        operations.push(RestoreOperation { targets });
    }
    if !preimages.is_empty() {
        return Err(error("checkpoint contains unexpected preimage paths"));
    }
    Ok(WorkspaceRestorePlan {
        checkpoint_kind: checkpoint_kind.into(),
        checkpoint_id: checkpoint_id.into(),
        edit: edit.clone(),
        after,
        operations,
    })
}

fn preimage(
    path: &str,
    expected: &ContentHash,
    preimages: &mut HashMap<String, (Vec<u8>, String)>,
) -> Result<RestoreDesiredState, WorkspaceRestoreError> {
    let (bytes, mode) = preimages
        .remove(path)
        .ok_or_else(|| error("checkpoint preimage is missing"))?;
    let policy = match mode.as_str() {
        "100644" => "regular",
        "100755" => "executable",
        _ => return Err(error("checkpoint file mode is unsupported")),
    };
    if !platform_supports_file_mode(policy) {
        return Err(error(
            "checkpoint executable restore requires POSIX file-mode support on this platform",
        ));
    }
    let hash = ContentHash::for_bytes(&bytes);
    if hash != *expected {
        return Err(error("checkpoint preimage does not match edit base"));
    }
    Ok(RestoreDesiredState::File { hash, bytes, mode })
}

fn target(operation: &str, role: &str, path: &str, desired: RestoreDesiredState) -> RestoreTarget {
    RestoreTarget {
        operation: operation.into(),
        role: role.into(),
        path: path.into(),
        desired,
    }
}

fn selected_operations(
    selection: &WorkspaceRestoreSelection,
    count: usize,
) -> Result<Vec<usize>, WorkspaceRestoreError> {
    match selection {
        WorkspaceRestoreSelection::AllAgentChanges => Ok((0..count).collect()),
        WorkspaceRestoreSelection::Operations { indices } => {
            if indices.is_empty() {
                return Err(error("selective restore requires at least one operation"));
            }
            let mut normalized = indices.clone();
            normalized.sort_unstable();
            normalized.dedup();
            if normalized.len() != indices.len() || normalized.iter().any(|index| *index >= count) {
                return Err(error("restore operation selection is invalid"));
            }
            Ok(normalized)
        }
    }
}

fn compile_target(
    target: &RestoreTarget,
    current: &ObservedWorkspacePathState,
    operations: &mut Vec<WorkspaceEditOperation>,
    contents: &mut HashMap<String, ContentInput>,
    restored_paths: &mut Vec<String>,
) -> Result<(), Box<WorkspaceRestoreError>> {
    match (&target.desired, current) {
        (RestoreDesiredState::Absent, ObservedWorkspacePathState::Absent) => return Ok(()),
        (
            RestoreDesiredState::File { hash: desired, .. },
            ObservedWorkspacePathState::File { hash: current },
        ) if desired == current => return Ok(()),
        (RestoreDesiredState::Absent, ObservedWorkspacePathState::File { hash }) => {
            operations.push(WorkspaceEditOperation::Delete {
                path: target.path.clone(),
                base: hash.clone(),
            });
        }
        (RestoreDesiredState::File { bytes, mode, .. }, ObservedWorkspacePathState::Absent) => {
            let content = ProposedContent::for_bytes(bytes).with_mode(policy_mode(mode)?);
            contents
                .entry(content.reference.clone())
                .or_insert_with(|| ContentInput {
                    reference: content.reference.clone(),
                    content: String::from_utf8(bytes.clone()).expect("validated checkpoint UTF-8"),
                });
            operations.push(WorkspaceEditOperation::Create {
                path: target.path.clone(),
                content,
            });
        }
        (
            RestoreDesiredState::File { bytes, mode, .. },
            ObservedWorkspacePathState::File { hash },
        ) => {
            let content = ProposedContent::for_bytes(bytes).with_mode(policy_mode(mode)?);
            contents
                .entry(content.reference.clone())
                .or_insert_with(|| ContentInput {
                    reference: content.reference.clone(),
                    content: String::from_utf8(bytes.clone()).expect("validated checkpoint UTF-8"),
                });
            operations.push(WorkspaceEditOperation::Update {
                path: target.path.clone(),
                base: hash.clone(),
                content,
            });
        }
        (_, ObservedWorkspacePathState::Unavailable { .. }) => {
            return Err(Box::new(error(
                "restore cannot mutate an unavailable or policy-denied path",
            )))
        }
    }
    restored_paths.push(target.path.clone());
    Ok(())
}

fn policy_mode(mode: &str) -> Result<&'static str, Box<WorkspaceRestoreError>> {
    match mode {
        "100644" => Ok("regular"),
        "100755" => Ok("executable"),
        _ => Err(Box::new(error("checkpoint file mode is unsupported"))),
    }
}

fn observed_matches_expected(
    current: &ObservedWorkspacePathState,
    expected: &ExpectedWorkspacePathState,
) -> bool {
    matches!(
        (current, expected),
        (
            ObservedWorkspacePathState::Absent,
            ExpectedWorkspacePathState::Absent
        )
    ) || matches!(
        (current, expected),
        (
            ObservedWorkspacePathState::File { hash: current },
            ExpectedWorkspacePathState::File { hash: expected }
        ) if current == expected
    )
}

impl RestoreDesiredState {
    fn expected(&self) -> ExpectedWorkspacePathState {
        match self {
            Self::Absent => ExpectedWorkspacePathState::Absent,
            Self::File { hash, .. } => ExpectedWorkspacePathState::File { hash: hash.clone() },
        }
    }
}

fn error(message: impl Into<String>) -> WorkspaceRestoreError {
    WorkspaceRestoreError {
        message: message.into(),
        apply_failure: None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::git_checkpoint::capture_git_checkpoint;
    use crate::non_git_checkpoint::NonGitCheckpointStore;
    use std::fs;
    use std::path::{Path, PathBuf};
    use std::process::Command;
    use std::sync::atomic::{AtomicU64, Ordering};

    static SEQUENCE: AtomicU64 = AtomicU64::new(0);

    fn roots() -> (PathBuf, PathBuf, PathBuf) {
        let sequence = SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let parent =
            std::env::temp_dir().join(format!("aegisy-restore-{}-{sequence}", std::process::id()));
        let project = parent.join("project");
        let storage = parent.join("storage");
        fs::create_dir_all(&project).unwrap();
        fs::create_dir_all(&storage).unwrap();
        (
            parent,
            project.canonicalize().unwrap(),
            storage.canonicalize().unwrap(),
        )
    }

    fn edit(root: &Path) -> (WorkspaceEdit, Vec<ContentInput>) {
        fs::write(root.join("update.txt"), "before\n").unwrap();
        fs::write(root.join("delete.txt"), "delete\n").unwrap();
        fs::write(root.join("old.txt"), "rename\n").unwrap();
        fs::write(root.join("unrelated.txt"), "user-unrelated\n").unwrap();
        let created = ProposedContent::for_bytes(b"created\n");
        let updated = ProposedContent::for_bytes(b"after\n");
        let edit = WorkspaceEdit::define(
            "restore-source",
            "project-1",
            root,
            vec![
                WorkspaceEditOperation::Create {
                    path: "created.txt".into(),
                    content: created.clone(),
                },
                WorkspaceEditOperation::Update {
                    path: "update.txt".into(),
                    base: ContentHash::for_bytes(b"before\n"),
                    content: updated.clone(),
                },
                WorkspaceEditOperation::Delete {
                    path: "delete.txt".into(),
                    base: ContentHash::for_bytes(b"delete\n"),
                },
                WorkspaceEditOperation::Rename {
                    from_path: "old.txt".into(),
                    to_path: "new.txt".into(),
                    base: ContentHash::for_bytes(b"rename\n"),
                },
            ],
        )
        .unwrap();
        (
            edit,
            vec![
                ContentInput {
                    reference: created.reference,
                    content: "created\n".into(),
                },
                ContentInput {
                    reference: updated.reference,
                    content: "after\n".into(),
                },
            ],
        )
    }

    fn non_git_plan(
        project: &Path,
        storage: &Path,
    ) -> (WorkspaceRestorePlan, WorkspaceEdit, NonGitCheckpointStore) {
        let (edit, contents) = edit(project);
        let store = NonGitCheckpointStore::open(storage, project).unwrap();
        let checkpoint = store
            .capture(
                "restore-checkpoint",
                &edit,
                &HashSet::new(),
                &HashSet::new(),
            )
            .unwrap();
        let applied = apply_workspace_edit(edit.clone(), contents, &HashSet::new()).unwrap();
        let plan = prepare_non_git_restore_plan(&store, &checkpoint, &edit, &applied).unwrap();
        (plan, edit, store)
    }

    #[test]
    fn selective_then_full_restore_changes_only_selected_agent_paths() {
        let (parent, project, storage) = roots();
        let (plan, _, _) = non_git_plan(&project, &storage);
        fs::write(project.join("unrelated.txt"), "later-user-unrelated\n").unwrap();

        let selective = review_workspace_restore(
            &plan,
            WorkspaceRestoreSelection::Operations { indices: vec![1] },
            &HashSet::new(),
            &HashSet::new(),
        )
        .unwrap();
        assert!(!selective.blocking);
        let result =
            execute_workspace_restore(&plan, &selective, &[], &HashSet::new(), &HashSet::new())
                .unwrap();
        assert_eq!(result.restored_paths, vec!["update.txt"]);
        assert_eq!(
            fs::read_to_string(project.join("update.txt")).unwrap(),
            "before\n"
        );
        assert!(project.join("created.txt").exists());
        assert!(!project.join("delete.txt").exists());
        assert!(!project.join("old.txt").exists());
        assert!(project.join("new.txt").exists());

        let full = review_workspace_restore(
            &plan,
            WorkspaceRestoreSelection::AllAgentChanges,
            &HashSet::new(),
            &HashSet::new(),
        )
        .unwrap();
        assert!(!full.blocking);
        assert_eq!(
            full.paths
                .iter()
                .find(|path| path.path == "update.txt")
                .unwrap()
                .status,
            "already-restored"
        );
        execute_workspace_restore(&plan, &full, &[], &HashSet::new(), &HashSet::new()).unwrap();
        assert!(!project.join("created.txt").exists());
        assert_eq!(
            fs::read_to_string(project.join("delete.txt")).unwrap(),
            "delete\n"
        );
        assert_eq!(
            fs::read_to_string(project.join("old.txt")).unwrap(),
            "rename\n"
        );
        assert!(!project.join("new.txt").exists());
        assert_eq!(
            fs::read_to_string(project.join("unrelated.txt")).unwrap(),
            "later-user-unrelated\n"
        );

        let again = review_workspace_restore(
            &plan,
            WorkspaceRestoreSelection::AllAgentChanges,
            &HashSet::new(),
            &HashSet::new(),
        )
        .unwrap();
        let no_change =
            execute_workspace_restore(&plan, &again, &[], &HashSet::new(), &HashSet::new())
                .unwrap();
        assert!(no_change.no_changes);
        fs::remove_dir_all(parent).unwrap();
    }

    #[test]
    fn conflicts_require_exact_hash_confirmation_and_pending_edits_cannot_confirm() {
        let (parent, project, storage) = roots();
        let (plan, _, _) = non_git_plan(&project, &storage);
        assert!(review_workspace_restore(
            &plan,
            WorkspaceRestoreSelection::Operations {
                indices: vec![1, 1]
            },
            &HashSet::new(),
            &HashSet::new()
        )
        .is_err());
        let denied = review_workspace_restore(
            &plan,
            WorkspaceRestoreSelection::Operations { indices: vec![1] },
            &HashSet::from(["update.txt".into()]),
            &HashSet::new(),
        )
        .unwrap();
        assert!(denied.blocking);
        assert!(!denied.paths[0].confirmable);
        fs::write(project.join("update.txt"), "later-user\n").unwrap();
        let review = review_workspace_restore(
            &plan,
            WorkspaceRestoreSelection::Operations { indices: vec![1] },
            &HashSet::new(),
            &HashSet::new(),
        )
        .unwrap();
        assert!(review.blocking);
        assert_eq!(review.conflict_count, 1);
        assert!(
            execute_workspace_restore(&plan, &review, &[], &HashSet::new(), &HashSet::new())
                .is_err()
        );
        let confirmation = WorkspaceRestoreConflictConfirmation {
            path: "update.txt".into(),
            current: review.paths[0].current.clone(),
        };
        execute_workspace_restore(
            &plan,
            &review,
            &[confirmation],
            &HashSet::new(),
            &HashSet::new(),
        )
        .unwrap();
        assert_eq!(
            fs::read_to_string(project.join("update.txt")).unwrap(),
            "before\n"
        );

        fs::write(project.join("created.txt"), "user-buffer-base\n").unwrap();
        let pending = review_workspace_restore(
            &plan,
            WorkspaceRestoreSelection::Operations { indices: vec![0] },
            &HashSet::new(),
            &HashSet::from(["created.txt".into()]),
        )
        .unwrap();
        assert!(pending.paths[0].pending_user_edit);
        assert!(!pending.paths[0].confirmable);
        let confirmation = WorkspaceRestoreConflictConfirmation {
            path: "created.txt".into(),
            current: pending.paths[0].current.clone(),
        };
        assert!(execute_workspace_restore(
            &plan,
            &pending,
            &[confirmation],
            &HashSet::new(),
            &HashSet::from(["created.txt".into()])
        )
        .is_err());

        let stale_review = review_workspace_restore(
            &plan,
            WorkspaceRestoreSelection::Operations { indices: vec![2] },
            &HashSet::new(),
            &HashSet::new(),
        )
        .unwrap();
        fs::write(project.join("delete.txt"), "appeared-after-review\n").unwrap();
        assert!(execute_workspace_restore(
            &plan,
            &stale_review,
            &[],
            &HashSet::new(),
            &HashSet::new()
        )
        .is_err());
        fs::remove_dir_all(parent).unwrap();
    }

    #[test]
    fn git_checkpoint_preimages_build_the_same_reviewable_restore_plan() {
        let (parent, project, _) = roots();
        let init = Command::new("git")
            .arg("-C")
            .arg(&project)
            .args(["init", "-q"])
            .status()
            .unwrap();
        assert!(init.success());
        Command::new("git")
            .arg("-C")
            .arg(&project)
            .args(["config", "user.name", "Aegisy Test"])
            .status()
            .unwrap();
        Command::new("git")
            .arg("-C")
            .arg(&project)
            .args(["config", "user.email", "test@aegisy.local"])
            .status()
            .unwrap();
        let (edit, contents) = edit(&project);
        Command::new("git")
            .arg("-C")
            .arg(&project)
            .args(["add", "."])
            .status()
            .unwrap();
        Command::new("git")
            .arg("-C")
            .arg(&project)
            .args(["commit", "-q", "-m", "initial"])
            .status()
            .unwrap();
        let checkpoint =
            capture_git_checkpoint("restore-git", &edit, &HashSet::new(), &HashSet::new()).unwrap();
        let applied = apply_workspace_edit(edit.clone(), contents, &HashSet::new()).unwrap();
        let plan = prepare_git_restore_plan(&checkpoint, &edit, &applied).unwrap();
        let review = review_workspace_restore(
            &plan,
            WorkspaceRestoreSelection::AllAgentChanges,
            &HashSet::new(),
            &HashSet::new(),
        )
        .unwrap();
        assert!(!review.blocking);
        assert_eq!(review.paths.len(), 5);
        fs::remove_dir_all(parent).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn full_restore_recreates_a_deleted_executable_with_its_mode() {
        use std::os::unix::fs::PermissionsExt;

        let (parent, project, storage) = roots();
        fs::write(project.join("script.sh"), "#!/bin/sh\nexit 0\n").unwrap();
        fs::set_permissions(project.join("script.sh"), fs::Permissions::from_mode(0o755)).unwrap();
        let edit = WorkspaceEdit::define(
            "restore-executable",
            "project-1",
            &project,
            vec![WorkspaceEditOperation::Delete {
                path: "script.sh".into(),
                base: ContentHash::for_bytes(b"#!/bin/sh\nexit 0\n"),
            }],
        )
        .unwrap();
        let store = NonGitCheckpointStore::open(&storage, &project).unwrap();
        let checkpoint = store
            .capture(
                "restore-executable",
                &edit,
                &HashSet::new(),
                &HashSet::new(),
            )
            .unwrap();
        let applied = apply_workspace_edit(edit.clone(), Vec::new(), &HashSet::new()).unwrap();
        let plan = prepare_non_git_restore_plan(&store, &checkpoint, &edit, &applied).unwrap();
        let review = review_workspace_restore(
            &plan,
            WorkspaceRestoreSelection::AllAgentChanges,
            &HashSet::new(),
            &HashSet::new(),
        )
        .unwrap();
        execute_workspace_restore(&plan, &review, &[], &HashSet::new(), &HashSet::new()).unwrap();
        assert_ne!(
            fs::metadata(project.join("script.sh"))
                .unwrap()
                .permissions()
                .mode()
                & 0o111,
            0
        );
        fs::remove_dir_all(parent).unwrap();
    }
}
