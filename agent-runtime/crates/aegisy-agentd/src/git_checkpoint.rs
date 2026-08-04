use crate::workspace::is_sensitive_path;
use crate::workspace_edit::{
    inspect_text_format, platform_supports_file_mode, ContentHash, WorkspaceEdit,
    WorkspaceEditOperation,
};
use crate::workspace_edit_apply::WorkspaceEditApplyResult;
use crate::workspace_edit_overlap::{
    proposal_overlap_baseline, restore_overlap_baseline, ExpectedWorkspacePathState,
};
use serde::Serialize;
use std::collections::HashSet;
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Component, Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

const CHECKPOINT_SCHEMA_VERSION: &str = "git-checkpoint/0.1";
const MAX_STATUS_BYTES: usize = 2 * 1024 * 1024;
const MAX_STATUS_ENTRIES: usize = 5_000;
const MAX_FILE_BYTES: u64 = 512 * 1024;
const MAX_TOTAL_BLOB_BYTES: u64 = 8 * 1024 * 1024;
const MAX_MANIFEST_BYTES: usize = 4 * 1024 * 1024;
const MAX_CHECKPOINT_REFS: usize = 128;
static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GitCheckpointError {
    pub message: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitCheckpoint {
    pub schema_version: String,
    pub checkpoint_id: String,
    pub edit_id: String,
    pub project_id: String,
    pub root_identity: String,
    pub repository_root: PathBuf,
    pub git_directory: PathBuf,
    pub reference: String,
    pub commit_oid: String,
    pub tree_oid: String,
    pub manifest_blob_oid: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub head_oid: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub branch: Option<String>,
    pub index_tree_oid: String,
    pub pre_existing_changes: Vec<GitCheckpointDirtyPath>,
    pub pending_user_paths: Vec<String>,
    pub redacted_pre_existing_count: usize,
    pub paths: Vec<GitCheckpointPath>,
    pub retained_blob_bytes: u64,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitCheckpointDirtyPath {
    pub path: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub original_path: Option<String>,
    pub index_status: String,
    pub worktree_status: String,
    pub kind: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitCheckpointPath {
    pub operation: String,
    pub role: String,
    pub path: String,
    pub pre_existing_user_change: bool,
    pub ownership: String,
    pub before: GitCheckpointPathState,
    pub planned_after: GitCheckpointPathState,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitCheckpointApplication {
    pub checkpoint_id: String,
    pub checkpoint_reference: String,
    pub checkpoint_commit_oid: String,
    pub edit_id: String,
    pub project_id: String,
    pub root_identity: String,
    pub paths: Vec<GitCheckpointAppliedPath>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitCheckpointAppliedPath {
    pub operation: String,
    pub role: String,
    pub path: String,
    pub pre_existing_user_change: bool,
    pub ownership: String,
    pub before: GitCheckpointPathState,
    pub agent_after: GitCheckpointPathState,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub enum GitCheckpointPathState {
    Absent,
    File {
        hash: ContentHash,
        #[serde(skip_serializing_if = "Option::is_none")]
        blob_oid: Option<String>,
        mode: String,
    },
}

#[derive(Serialize)]
struct GitCheckpointManifest<'a> {
    schema_version: &'a str,
    checkpoint_id: &'a str,
    edit_id: &'a str,
    project_id: &'a str,
    root_identity: &'a str,
    head_oid: &'a Option<String>,
    branch: &'a Option<String>,
    index_tree_oid: &'a str,
    pre_existing_changes: &'a [GitCheckpointDirtyPath],
    pending_user_paths: &'a [String],
    redacted_pre_existing_count: usize,
    paths: &'a [GitCheckpointPath],
    retained_blob_bytes: u64,
}

pub fn capture_git_checkpoint(
    checkpoint_id: &str,
    edit: &WorkspaceEdit,
    ignored_paths: &HashSet<String>,
    pending_user_paths: &HashSet<String>,
) -> Result<GitCheckpoint, GitCheckpointError> {
    validate_checkpoint_id(checkpoint_id)?;
    validate_edit_binding(edit)?;
    let root = edit.root.canonical_path.as_path();
    // Path::canonicalize produces verbatim \\?\ paths on Windows while the
    // Git-facing discovery helpers normalize to the plain form, so compare
    // the plain forms on every platform.
    let plain_root = crate::plain_path(root);
    let repository_root = discover_repository_root(root)?;
    if repository_root != plain_root {
        return Err(error(
            "Git checkpoint requires the authorized project root to equal the repository root",
        ));
    }
    let git_directory = discover_git_directory(root)?;
    if !git_directory.starts_with(&plain_root) || !git_directory.is_dir() {
        return Err(error(
            "Git checkpoint metadata is outside the authorized project root",
        ));
    }
    let reference = format!("refs/aegisy/checkpoints/{checkpoint_id}");
    git_output(root, &["check-ref-format", &reference], None, &[], false)?;
    let references = git_output(
        root,
        &[
            "for-each-ref",
            "--format=%(refname)",
            "refs/aegisy/checkpoints/",
        ],
        None,
        &[],
        false,
    )?;
    if references
        .split(|byte| *byte == b'\n')
        .filter(|line| !line.is_empty())
        .count()
        >= MAX_CHECKPOINT_REFS
    {
        return Err(error("Git checkpoint reference limit exceeded"));
    }
    if optional_oid(root, &["rev-parse", "--verify", &reference])?.is_some() {
        return Err(error("Git checkpoint reference already exists"));
    }
    let (pending_user_paths, redacted_pending_count) =
        validate_pending_user_paths(pending_user_paths)?;
    let touched_paths = edit_touched_paths(edit);
    if pending_user_paths
        .iter()
        .any(|path| touched_paths.contains(path))
    {
        return Err(error(
            "Git checkpoint cannot capture an Agent path with an unsaved user edit",
        ));
    }

    let head_oid = optional_oid(root, &["rev-parse", "--verify", "HEAD"])?;
    let branch = optional_text(root, &["symbolic-ref", "--short", "-q", "HEAD"])?;
    let index_tree_oid = required_oid(root, &["write-tree"])?;
    let (pre_existing_changes, redacted_status_count) = sanitize_status(read_status(root)?);
    let redacted_pre_existing_count = redacted_status_count.saturating_add(redacted_pending_count);
    let dirty_paths = dirty_path_set(&pre_existing_changes);

    let mut retained_blob_bytes = 0_u64;
    let mut paths = Vec::with_capacity(edit.operations.len().saturating_mul(2));
    for operation in &edit.operations {
        capture_operation(
            root,
            operation,
            ignored_paths,
            &dirty_paths,
            &mut retained_blob_bytes,
            &mut paths,
        )?;
    }

    let manifest = GitCheckpointManifest {
        schema_version: CHECKPOINT_SCHEMA_VERSION,
        checkpoint_id,
        edit_id: &edit.edit_id,
        project_id: &edit.project_id,
        root_identity: &edit.root.identity,
        head_oid: &head_oid,
        branch: &branch,
        index_tree_oid: &index_tree_oid,
        pre_existing_changes: &pre_existing_changes,
        pending_user_paths: &pending_user_paths,
        redacted_pre_existing_count,
        paths: &paths,
        retained_blob_bytes,
    };
    let manifest_bytes = serde_json::to_vec(&manifest)
        .map_err(|cause| error(format!("cannot serialize Git checkpoint manifest: {cause}")))?;
    if manifest_bytes.len() > MAX_MANIFEST_BYTES {
        return Err(error("Git checkpoint manifest exceeds size limit"));
    }
    let manifest_blob_oid = write_blob(root, &manifest_bytes)?;
    let temporary_index = temporary_index_path(checkpoint_id);
    let tree_result = build_checkpoint_tree(root, &temporary_index, &manifest_blob_oid, &paths);
    let _ = fs::remove_file(&temporary_index);
    let tree_oid = tree_result?;
    let commit_oid = create_checkpoint_commit(root, checkpoint_id, &tree_oid)?;
    create_checkpoint_reference(root, &reference, &commit_oid)?;

    Ok(GitCheckpoint {
        schema_version: CHECKPOINT_SCHEMA_VERSION.into(),
        checkpoint_id: checkpoint_id.into(),
        edit_id: edit.edit_id.clone(),
        project_id: edit.project_id.clone(),
        root_identity: edit.root.identity.clone(),
        repository_root,
        git_directory,
        reference,
        commit_oid,
        tree_oid,
        manifest_blob_oid,
        head_oid,
        branch,
        index_tree_oid,
        pre_existing_changes,
        pending_user_paths,
        redacted_pre_existing_count,
        paths,
        retained_blob_bytes,
    })
}

pub fn bind_git_checkpoint_application(
    checkpoint: &GitCheckpoint,
    edit: &WorkspaceEdit,
    applied: &WorkspaceEditApplyResult,
) -> Result<GitCheckpointApplication, GitCheckpointError> {
    validate_edit_binding(edit)?;
    if checkpoint.schema_version != CHECKPOINT_SCHEMA_VERSION
        || checkpoint.edit_id != edit.edit_id
        || checkpoint.project_id != edit.project_id
        || checkpoint.root_identity != edit.root.identity
        || crate::plain_path(&checkpoint.repository_root)
            != crate::plain_path(&edit.root.canonical_path)
        || checkpoint.reference != format!("refs/aegisy/checkpoints/{}", checkpoint.checkpoint_id)
    {
        return Err(error(
            "Git checkpoint does not belong to this workspace edit",
        ));
    }
    validate_checkpoint_id(&checkpoint.checkpoint_id)?;
    validate_checkpoint_manifest(checkpoint)?;
    let current_reference = required_oid(
        &checkpoint.repository_root,
        &["rev-parse", "--verify", &checkpoint.reference],
    )?;
    let tree_spec = format!("{}^{{tree}}", checkpoint.reference);
    let manifest_spec = format!("{}:manifest.json", checkpoint.reference);
    if current_reference != checkpoint.commit_oid
        || required_oid(&checkpoint.repository_root, &["rev-parse", &tree_spec])?
            != checkpoint.tree_oid
        || required_oid(&checkpoint.repository_root, &["rev-parse", &manifest_spec])?
            != checkpoint.manifest_blob_oid
        || discover_git_directory(&checkpoint.repository_root)? != checkpoint.git_directory
        || optional_oid(
            &checkpoint.repository_root,
            &["rev-parse", "--verify", "HEAD"],
        )? != checkpoint.head_oid
        || optional_text(
            &checkpoint.repository_root,
            &["symbolic-ref", "--short", "-q", "HEAD"],
        )? != checkpoint.branch
        || required_oid(&checkpoint.repository_root, &["write-tree"])? != checkpoint.index_tree_oid
    {
        return Err(error(
            "Git repository identity, HEAD, branch, index, or checkpoint ref changed",
        ));
    }

    let before = proposal_overlap_baseline(edit).map_err(|cause| error(cause.message))?;
    let after = restore_overlap_baseline(edit, applied).map_err(|cause| error(cause.message))?;
    if checkpoint.paths.len() != before.paths.len() || checkpoint.paths.len() != after.paths.len() {
        return Err(error("Git checkpoint path set is incomplete"));
    }
    let mut paths = Vec::with_capacity(checkpoint.paths.len());
    for ((checkpoint_path, before_path), after_path) in
        checkpoint.paths.iter().zip(&before.paths).zip(&after.paths)
    {
        if checkpoint_path.operation != before_path.operation
            || checkpoint_path.role != before_path.role
            || checkpoint_path.path != before_path.path
            || checkpoint_path.operation != after_path.operation
            || checkpoint_path.role != after_path.role
            || checkpoint_path.path != after_path.path
            || !checkpoint_state_matches(&checkpoint_path.before, &before_path.expected, true)
            || !checkpoint_state_matches(
                &checkpoint_path.planned_after,
                &after_path.expected,
                false,
            )
            || checkpoint_path.ownership
                != if checkpoint_path.pre_existing_user_change {
                    "agent-on-user-base"
                } else {
                    "agent-only"
                }
        {
            return Err(error(
                "Git checkpoint path state was modified or is inconsistent",
            ));
        }
        if let GitCheckpointPathState::File {
            blob_oid: Some(blob_oid),
            ..
        } = &checkpoint_path.before
        {
            let file_spec = format!("{}:files/{}", checkpoint.reference, checkpoint_path.path);
            if required_oid(&checkpoint.repository_root, &["rev-parse", &file_spec])? != *blob_oid {
                return Err(error(
                    "Git checkpoint preimage blob does not match its tree",
                ));
            }
        }
        paths.push(GitCheckpointAppliedPath {
            operation: checkpoint_path.operation.clone(),
            role: checkpoint_path.role.clone(),
            path: checkpoint_path.path.clone(),
            pre_existing_user_change: checkpoint_path.pre_existing_user_change,
            ownership: checkpoint_path.ownership.clone(),
            before: checkpoint_path.before.clone(),
            agent_after: checkpoint_path.planned_after.clone(),
        });
    }
    validate_applied_modes(&checkpoint.paths, edit, applied)?;
    Ok(GitCheckpointApplication {
        checkpoint_id: checkpoint.checkpoint_id.clone(),
        checkpoint_reference: checkpoint.reference.clone(),
        checkpoint_commit_oid: checkpoint.commit_oid.clone(),
        edit_id: edit.edit_id.clone(),
        project_id: edit.project_id.clone(),
        root_identity: edit.root.identity.clone(),
        paths,
    })
}

pub fn verify_git_checkpoint_application(
    checkpoint: &GitCheckpoint,
    application: &GitCheckpointApplication,
) -> Result<(), GitCheckpointError> {
    validate_checkpoint_id(&checkpoint.checkpoint_id)?;
    validate_checkpoint_manifest(checkpoint)?;
    if checkpoint.schema_version != CHECKPOINT_SCHEMA_VERSION
        || application.checkpoint_id != checkpoint.checkpoint_id
        || application.checkpoint_reference != checkpoint.reference
        || application.checkpoint_commit_oid != checkpoint.commit_oid
        || application.edit_id != checkpoint.edit_id
        || application.project_id != checkpoint.project_id
        || application.root_identity != checkpoint.root_identity
        || application.paths.len() != checkpoint.paths.len()
    {
        return Err(error("Git checkpoint application identity is inconsistent"));
    }
    let repository_root = crate::plain_path(
        &checkpoint
            .repository_root
            .canonicalize()
            .map_err(|_| error("Git checkpoint repository root is unavailable"))?,
    );
    if repository_root != checkpoint.repository_root
        || discover_repository_root(&repository_root)? != checkpoint.repository_root
        || discover_git_directory(&repository_root)? != checkpoint.git_directory
        || required_oid(
            &repository_root,
            &["rev-parse", "--verify", &checkpoint.reference],
        )? != checkpoint.commit_oid
        || required_oid(
            &repository_root,
            &["rev-parse", &format!("{}^{{tree}}", checkpoint.reference)],
        )? != checkpoint.tree_oid
        || required_oid(
            &repository_root,
            &[
                "rev-parse",
                &format!("{}:manifest.json", checkpoint.reference),
            ],
        )? != checkpoint.manifest_blob_oid
        || optional_oid(&repository_root, &["rev-parse", "--verify", "HEAD"])?
            != checkpoint.head_oid
        || optional_text(&repository_root, &["symbolic-ref", "--short", "-q", "HEAD"])?
            != checkpoint.branch
    {
        return Err(error(
            "Git checkpoint repository, HEAD, branch, or reference changed",
        ));
    }
    for (checkpoint_path, applied_path) in checkpoint.paths.iter().zip(&application.paths) {
        if applied_path.operation != checkpoint_path.operation
            || applied_path.role != checkpoint_path.role
            || applied_path.path != checkpoint_path.path
            || applied_path.pre_existing_user_change != checkpoint_path.pre_existing_user_change
            || applied_path.ownership != checkpoint_path.ownership
            || applied_path.before != checkpoint_path.before
            || applied_path.agent_after != checkpoint_path.planned_after
        {
            return Err(error("Git checkpoint application path was modified"));
        }
        if let GitCheckpointPathState::File {
            blob_oid: Some(blob_oid),
            ..
        } = &checkpoint_path.before
        {
            let file_spec = format!("{}:files/{}", checkpoint.reference, checkpoint_path.path);
            if required_oid(&repository_root, &["rev-parse", &file_spec])? != *blob_oid {
                return Err(error("Git checkpoint application preimage blob changed"));
            }
        }
    }
    Ok(())
}

fn validate_applied_modes(
    paths: &[GitCheckpointPath],
    edit: &WorkspaceEdit,
    applied: &WorkspaceEditApplyResult,
) -> Result<(), GitCheckpointError> {
    for (operation, file) in edit.operations.iter().zip(&applied.files) {
        let final_path = match operation {
            WorkspaceEditOperation::Create { path, .. }
            | WorkspaceEditOperation::Update { path, .. } => Some(path.as_str()),
            WorkspaceEditOperation::Delete { .. } => None,
            WorkspaceEditOperation::Rename { to_path, .. } => Some(to_path.as_str()),
        };
        let expected_mode = final_path.and_then(|path| {
            paths.iter().find_map(|candidate| {
                if candidate.path != path {
                    return None;
                }
                match &candidate.planned_after {
                    GitCheckpointPathState::File { mode, .. } => Some(mode.as_str()),
                    GitCheckpointPathState::Absent => None,
                }
            })
        });
        if file.final_mode.as_deref() != expected_mode {
            return Err(error(
                "Git checkpoint applied file mode does not match plan",
            ));
        }
    }
    Ok(())
}

pub fn read_git_checkpoint_preimage(
    checkpoint: &GitCheckpoint,
    path: &str,
) -> Result<Vec<u8>, GitCheckpointError> {
    validate_checkpoint_id(&checkpoint.checkpoint_id)?;
    validate_checkpoint_manifest(checkpoint)?;
    let checkpoint_path = checkpoint
        .paths
        .iter()
        .find(|candidate| candidate.path == path)
        .ok_or_else(|| error("Git checkpoint path is not present"))?;
    let GitCheckpointPathState::File {
        hash,
        blob_oid: Some(blob_oid),
        ..
    } = &checkpoint_path.before
    else {
        return Err(error("Git checkpoint path has no preimage"));
    };
    let current_reference = required_oid(
        &checkpoint.repository_root,
        &["rev-parse", "--verify", &checkpoint.reference],
    )?;
    if current_reference != checkpoint.commit_oid {
        return Err(error("Git checkpoint reference changed"));
    }
    let file_spec = format!("{}:files/{path}", checkpoint.reference);
    let stored_oid = required_oid(&checkpoint.repository_root, &["rev-parse", &file_spec])?;
    if stored_oid != *blob_oid {
        return Err(error("Git checkpoint preimage OID changed"));
    }
    let bytes = git_output(
        &checkpoint.repository_root,
        &["cat-file", "blob", blob_oid],
        None,
        &[],
        false,
    )?;
    if bytes.len() as u64 > MAX_FILE_BYTES || ContentHash::for_bytes(&bytes) != *hash {
        return Err(error("Git checkpoint preimage content hash mismatch"));
    }
    Ok(bytes)
}

fn validate_checkpoint_manifest(checkpoint: &GitCheckpoint) -> Result<(), GitCheckpointError> {
    let manifest = GitCheckpointManifest {
        schema_version: CHECKPOINT_SCHEMA_VERSION,
        checkpoint_id: &checkpoint.checkpoint_id,
        edit_id: &checkpoint.edit_id,
        project_id: &checkpoint.project_id,
        root_identity: &checkpoint.root_identity,
        head_oid: &checkpoint.head_oid,
        branch: &checkpoint.branch,
        index_tree_oid: &checkpoint.index_tree_oid,
        pre_existing_changes: &checkpoint.pre_existing_changes,
        pending_user_paths: &checkpoint.pending_user_paths,
        redacted_pre_existing_count: checkpoint.redacted_pre_existing_count,
        paths: &checkpoint.paths,
        retained_blob_bytes: checkpoint.retained_blob_bytes,
    };
    let bytes = serde_json::to_vec(&manifest)
        .map_err(|cause| error(format!("cannot serialize Git checkpoint manifest: {cause}")))?;
    let oid = hash_blob(&checkpoint.repository_root, &bytes)?;
    if oid != checkpoint.manifest_blob_oid {
        return Err(error(
            "Git checkpoint manifest does not match its stored blob",
        ));
    }
    Ok(())
}

fn checkpoint_state_matches(
    state: &GitCheckpointPathState,
    expected: &ExpectedWorkspacePathState,
    require_blob: bool,
) -> bool {
    match (state, expected) {
        (GitCheckpointPathState::Absent, ExpectedWorkspacePathState::Absent) => true,
        (
            GitCheckpointPathState::File {
                hash,
                blob_oid,
                mode,
            },
            ExpectedWorkspacePathState::File { hash: expected },
        ) => {
            hash == expected
                && matches!(mode.as_str(), "100644" | "100755")
                && if require_blob {
                    blob_oid.as_deref().is_some_and(valid_oid)
                } else {
                    blob_oid.is_none()
                }
        }
        _ => false,
    }
}

fn valid_oid(value: &str) -> bool {
    matches!(value.len(), 40 | 64)
        && value
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
}

fn capture_operation(
    root: &Path,
    operation: &WorkspaceEditOperation,
    ignored_paths: &HashSet<String>,
    dirty_paths: &HashSet<String>,
    retained_blob_bytes: &mut u64,
    paths: &mut Vec<GitCheckpointPath>,
) -> Result<(), GitCheckpointError> {
    match operation {
        WorkspaceEditOperation::Create { path, content } => capture_path(
            root,
            "create",
            "target",
            path,
            None,
            Some(content.hash.clone()),
            Some(&content.format.mode),
            ignored_paths,
            dirty_paths,
            retained_blob_bytes,
            paths,
        ),
        WorkspaceEditOperation::Update {
            path,
            base,
            content,
        } => capture_path(
            root,
            "update",
            "target",
            path,
            Some(base),
            Some(content.hash.clone()),
            Some(&content.format.mode),
            ignored_paths,
            dirty_paths,
            retained_blob_bytes,
            paths,
        ),
        WorkspaceEditOperation::Delete { path, base } => capture_path(
            root,
            "delete",
            "target",
            path,
            Some(base),
            None,
            None,
            ignored_paths,
            dirty_paths,
            retained_blob_bytes,
            paths,
        ),
        WorkspaceEditOperation::Rename {
            from_path,
            to_path,
            base,
        } => {
            let source_index = paths.len();
            capture_path(
                root,
                "rename",
                "source",
                from_path,
                Some(base),
                None,
                None,
                ignored_paths,
                dirty_paths,
                retained_blob_bytes,
                paths,
            )?;
            let source_mode = match &paths[source_index].before {
                GitCheckpointPathState::File { mode, .. } => mode.clone(),
                GitCheckpointPathState::Absent => {
                    return Err(error("Git checkpoint rename source is absent"))
                }
            };
            capture_path(
                root,
                "rename",
                "target",
                to_path,
                None,
                Some(base.clone()),
                Some("preserve"),
                ignored_paths,
                dirty_paths,
                retained_blob_bytes,
                paths,
            )?;
            if let Some(GitCheckpointPath {
                planned_after: GitCheckpointPathState::File { mode, .. },
                ..
            }) = paths.last_mut()
            {
                *mode = source_mode;
            }
            Ok(())
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn capture_path(
    root: &Path,
    operation: &str,
    role: &str,
    path: &str,
    expected_before: Option<&ContentHash>,
    planned_after: Option<ContentHash>,
    planned_mode: Option<&str>,
    ignored_paths: &HashSet<String>,
    dirty_paths: &HashSet<String>,
    retained_blob_bytes: &mut u64,
    paths: &mut Vec<GitCheckpointPath>,
) -> Result<(), GitCheckpointError> {
    if is_sensitive_path(Path::new(path)) || ignored_paths.contains(path) {
        return Err(error("Git checkpoint path is denied by workspace policy"));
    }
    let absolute = resolve_path(root, path, expected_before.is_none())?;
    let before = if let Some(expected) = expected_before {
        let (bytes, mode) = read_bounded_file(&absolute)?;
        let actual = ContentHash::for_bytes(&bytes);
        if actual != *expected {
            return Err(error("Git checkpoint base SHA-256 is stale"));
        }
        *retained_blob_bytes = retained_blob_bytes.saturating_add(bytes.len() as u64);
        if *retained_blob_bytes > MAX_TOTAL_BLOB_BYTES {
            return Err(error("Git checkpoint blob budget exceeded"));
        }
        let blob_oid = write_blob(root, &bytes)?;
        GitCheckpointPathState::File {
            hash: actual,
            blob_oid: Some(blob_oid),
            mode,
        }
    } else {
        if fs::symlink_metadata(&absolute).is_ok() {
            return Err(error("Git checkpoint expected an absent target"));
        }
        GitCheckpointPathState::Absent
    };
    let planned_after = match planned_after {
        Some(hash) => GitCheckpointPathState::File {
            hash,
            blob_oid: None,
            mode: resolve_planned_mode(planned_mode.unwrap_or("preserve"), &before)?,
        },
        None => GitCheckpointPathState::Absent,
    };
    let pre_existing_user_change = dirty_paths.contains(path);
    paths.push(GitCheckpointPath {
        operation: operation.into(),
        role: role.into(),
        path: path.into(),
        pre_existing_user_change,
        ownership: if pre_existing_user_change {
            "agent-on-user-base".into()
        } else {
            "agent-only".into()
        },
        before,
        planned_after,
    });
    Ok(())
}

fn resolve_planned_mode(
    declared: &str,
    before: &GitCheckpointPathState,
) -> Result<String, GitCheckpointError> {
    if !platform_supports_file_mode(declared) {
        return Err(error(
            "Git checkpoint executable mode requires POSIX file-mode support on this platform",
        ));
    }
    match declared {
        "regular" => Ok("100644".into()),
        "executable" => Ok("100755".into()),
        "preserve" => Ok(mode_for_planned_state(before)),
        _ => Err(error("Git checkpoint file mode policy is unsupported")),
    }
}

fn mode_for_planned_state(before: &GitCheckpointPathState) -> String {
    match before {
        GitCheckpointPathState::File { mode, .. } => mode.clone(),
        GitCheckpointPathState::Absent => "100644".into(),
    }
}

fn validate_checkpoint_id(value: &str) -> Result<(), GitCheckpointError> {
    if value.is_empty()
        || value.len() > 128
        || value.starts_with('.')
        || value.ends_with('.')
        || value.contains("..")
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.'))
    {
        return Err(error("Git checkpoint ID is not a safe ref component"));
    }
    Ok(())
}

fn validate_edit_binding(edit: &WorkspaceEdit) -> Result<(), GitCheckpointError> {
    let rebuilt = WorkspaceEdit::define(
        edit.edit_id.clone(),
        edit.project_id.clone(),
        &edit.root.canonical_path,
        edit.operations.clone(),
    )
    .map_err(|cause| error(cause.message))?;
    if rebuilt != *edit {
        return Err(error("Git checkpoint edit binding was modified"));
    }
    Ok(())
}

fn discover_repository_root(root: &Path) -> Result<PathBuf, GitCheckpointError> {
    let value = required_text(
        root,
        &["rev-parse", "--path-format=absolute", "--show-toplevel"],
    )?;
    Ok(crate::plain_path(
        &PathBuf::from(value)
            .canonicalize()
            .map_err(|_| error("Git repository root is unavailable"))?,
    ))
}

fn discover_git_directory(root: &Path) -> Result<PathBuf, GitCheckpointError> {
    let value = required_text(
        root,
        &["rev-parse", "--path-format=absolute", "--absolute-git-dir"],
    )?;
    Ok(crate::plain_path(
        &PathBuf::from(value)
            .canonicalize()
            .map_err(|_| error("Git directory is unavailable"))?,
    ))
}

fn read_status(root: &Path) -> Result<Vec<GitCheckpointDirtyPath>, GitCheckpointError> {
    let bytes = git_output(
        root,
        &[
            "status",
            "--porcelain=v2",
            "-z",
            "--untracked-files=all",
            "--ignore-submodules=dirty",
        ],
        None,
        &[],
        true,
    )?;
    parse_porcelain_v2(&bytes)
}

fn parse_porcelain_v2(bytes: &[u8]) -> Result<Vec<GitCheckpointDirtyPath>, GitCheckpointError> {
    let records = bytes.split(|byte| *byte == 0).collect::<Vec<_>>();
    let mut entries = Vec::new();
    let mut index = 0_usize;
    while index < records.len() {
        let record = records[index];
        if record.is_empty() {
            index += 1;
            continue;
        }
        if entries.len() >= MAX_STATUS_ENTRIES {
            return Err(error("Git checkpoint status entry limit exceeded"));
        }
        match record[0] {
            b'1' | b'u' => {
                let fields = split_prefix_fields(record, if record[0] == b'1' { 9 } else { 11 })?;
                let xy = fields[1];
                entries.push(dirty_entry(
                    fields.last().copied().unwrap_or_default(),
                    None,
                    xy,
                    if record[0] == b'1' {
                        "ordinary"
                    } else {
                        "unmerged"
                    },
                )?);
            }
            b'2' => {
                let fields = split_prefix_fields(record, 10)?;
                index += 1;
                let original = records
                    .get(index)
                    .copied()
                    .ok_or_else(|| error("Git rename status is incomplete"))?;
                entries.push(dirty_entry(
                    fields.last().copied().unwrap_or_default(),
                    Some(original),
                    fields[1],
                    "rename-or-copy",
                )?);
            }
            b'?' => entries.push(dirty_entry(&record[2..], None, b"??", "untracked")?),
            b'!' => {}
            _ => {
                return Err(error(
                    "Git checkpoint received an unsupported porcelain record",
                ))
            }
        }
        index += 1;
    }
    Ok(entries)
}

fn split_prefix_fields(record: &[u8], fields: usize) -> Result<Vec<&[u8]>, GitCheckpointError> {
    let mut result = Vec::with_capacity(fields);
    let mut remainder = record;
    for _ in 0..fields.saturating_sub(1) {
        let position = remainder
            .iter()
            .position(|byte| *byte == b' ')
            .ok_or_else(|| error("Git porcelain record is malformed"))?;
        result.push(&remainder[..position]);
        remainder = &remainder[position + 1..];
    }
    result.push(remainder);
    Ok(result)
}

fn dirty_entry(
    path: &[u8],
    original: Option<&[u8]>,
    xy: &[u8],
    kind: &str,
) -> Result<GitCheckpointDirtyPath, GitCheckpointError> {
    if xy.len() != 2 {
        return Err(error("Git porcelain status is malformed"));
    }
    Ok(GitCheckpointDirtyPath {
        path: normalize_git_path(path)?,
        original_path: original.map(normalize_git_path).transpose()?,
        index_status: char::from(xy[0]).to_string(),
        worktree_status: char::from(xy[1]).to_string(),
        kind: kind.into(),
    })
}

fn normalize_git_path(bytes: &[u8]) -> Result<String, GitCheckpointError> {
    let path = std::str::from_utf8(bytes)
        .map_err(|_| error("Git checkpoint requires UTF-8 repository paths"))?
        .replace('\\', "/");
    if path.is_empty()
        || Path::new(&path).is_absolute()
        || Path::new(&path)
            .components()
            .any(|component| !matches!(component, Component::Normal(_)))
    {
        return Err(error("Git checkpoint received an unsafe repository path"));
    }
    Ok(path)
}

fn dirty_path_set(entries: &[GitCheckpointDirtyPath]) -> HashSet<String> {
    let mut paths = HashSet::new();
    for entry in entries {
        paths.insert(entry.path.clone());
        if let Some(original) = &entry.original_path {
            paths.insert(original.clone());
        }
    }
    paths
}

fn sanitize_status(entries: Vec<GitCheckpointDirtyPath>) -> (Vec<GitCheckpointDirtyPath>, usize) {
    let mut visible = Vec::with_capacity(entries.len());
    let mut redacted = 0_usize;
    for entry in entries {
        let sensitive = is_sensitive_path(Path::new(&entry.path))
            || entry
                .original_path
                .as_deref()
                .is_some_and(|path| is_sensitive_path(Path::new(path)));
        if sensitive {
            redacted += 1;
        } else {
            visible.push(entry);
        }
    }
    (visible, redacted)
}

fn validate_pending_user_paths(
    paths: &HashSet<String>,
) -> Result<(Vec<String>, usize), GitCheckpointError> {
    if paths.len() > 512 {
        return Err(error("Git checkpoint pending-user path limit exceeded"));
    }
    let mut visible = Vec::with_capacity(paths.len());
    let mut redacted = 0_usize;
    for path in paths {
        if path.is_empty()
            || path.len() > 4 * 1024
            || Path::new(path).is_absolute()
            || Path::new(path)
                .components()
                .any(|component| !matches!(component, Component::Normal(_)))
        {
            return Err(error("Git checkpoint pending-user path is not normalized"));
        }
        if is_sensitive_path(Path::new(path)) {
            redacted += 1;
        } else {
            visible.push(path.clone());
        }
    }
    visible.sort();
    Ok((visible, redacted))
}

fn edit_touched_paths(edit: &WorkspaceEdit) -> HashSet<String> {
    let mut paths = HashSet::new();
    for operation in &edit.operations {
        match operation {
            WorkspaceEditOperation::Create { path, .. }
            | WorkspaceEditOperation::Update { path, .. }
            | WorkspaceEditOperation::Delete { path, .. } => {
                paths.insert(path.clone());
            }
            WorkspaceEditOperation::Rename {
                from_path, to_path, ..
            } => {
                paths.insert(from_path.clone());
                paths.insert(to_path.clone());
            }
        }
    }
    paths
}

fn resolve_path(
    root: &Path,
    relative: &str,
    allow_missing_final: bool,
) -> Result<PathBuf, GitCheckpointError> {
    if root
        .canonicalize()
        .ok()
        .map(|canonical| crate::plain_path(&canonical))
        .as_deref()
        != Some(crate::plain_path(root).as_path())
    {
        return Err(error("Git checkpoint root changed"));
    }
    let mut candidate = root.to_path_buf();
    let components = Path::new(relative).components().collect::<Vec<_>>();
    for (index, component) in components.iter().enumerate() {
        let Component::Normal(name) = component else {
            return Err(error("Git checkpoint path is not normalized"));
        };
        candidate.push(name);
        let final_component = index + 1 == components.len();
        match fs::symlink_metadata(&candidate) {
            Ok(metadata) if metadata.file_type().is_symlink() => {
                return Err(error("Git checkpoint path traverses a symlink"))
            }
            Ok(metadata) if !final_component && !metadata.is_dir() => {
                return Err(error("Git checkpoint parent is not a directory"))
            }
            Ok(_) => {}
            Err(cause)
                if allow_missing_final
                    && final_component
                    && cause.kind() == std::io::ErrorKind::NotFound => {}
            Err(_) => return Err(error("Git checkpoint path is unavailable")),
        }
    }
    Ok(candidate)
}

fn read_bounded_file(path: &Path) -> Result<(Vec<u8>, String), GitCheckpointError> {
    let metadata = fs::metadata(path).map_err(|_| error("Git checkpoint base is unavailable"))?;
    if !metadata.is_file() || metadata.len() > MAX_FILE_BYTES {
        return Err(error("Git checkpoint base is not a bounded regular file"));
    }
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    File::open(path)
        .and_then(|file| file.take(MAX_FILE_BYTES + 1).read_to_end(&mut bytes))
        .map_err(|_| error("Git checkpoint base cannot be read"))?;
    if bytes.len() as u64 > MAX_FILE_BYTES {
        return Err(error("Git checkpoint base exceeds size limit"));
    }
    let text = bytes.strip_prefix(&[0xef, 0xbb, 0xbf]).unwrap_or(&bytes);
    if bytes.contains(&0) || std::str::from_utf8(text).is_err() {
        return Err(error("Git checkpoint base must be UTF-8 text"));
    }
    if inspect_text_format(&bytes, "preserve").newline == "mixed" {
        return Err(error("Git checkpoint base has mixed line endings"));
    }
    Ok((bytes, file_mode(&metadata)))
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

fn write_blob(root: &Path, bytes: &[u8]) -> Result<String, GitCheckpointError> {
    let output = git_output(
        root,
        &["hash-object", "-w", "--stdin"],
        Some(bytes),
        &[],
        false,
    )?;
    parse_oid(&output)
}

fn hash_blob(root: &Path, bytes: &[u8]) -> Result<String, GitCheckpointError> {
    let output = git_output(root, &["hash-object", "--stdin"], Some(bytes), &[], false)?;
    parse_oid(&output)
}

fn build_checkpoint_tree(
    root: &Path,
    temporary_index: &Path,
    manifest_blob_oid: &str,
    paths: &[GitCheckpointPath],
) -> Result<String, GitCheckpointError> {
    let index = temporary_index.to_string_lossy().into_owned();
    let environment = [("GIT_INDEX_FILE", index.as_str())];
    git_output(root, &["read-tree", "--empty"], None, &environment, false)?;
    git_output(
        root,
        &[
            "update-index",
            "--add",
            "--cacheinfo",
            &format!("100644,{manifest_blob_oid},manifest.json"),
        ],
        None,
        &environment,
        false,
    )?;
    for path in paths {
        let GitCheckpointPathState::File {
            blob_oid: Some(blob_oid),
            mode,
            ..
        } = &path.before
        else {
            continue;
        };
        let tree_path = format!("files/{}", path.path);
        let cache_info = format!("{mode},{blob_oid},{tree_path}");
        git_output(
            root,
            &["update-index", "--add", "--cacheinfo", &cache_info],
            None,
            &environment,
            false,
        )?;
    }
    let output = git_output(root, &["write-tree"], None, &environment, false)?;
    parse_oid(&output)
}

fn create_checkpoint_commit(
    root: &Path,
    checkpoint_id: &str,
    tree_oid: &str,
) -> Result<String, GitCheckpointError> {
    let timestamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    let date = format!("@{timestamp} +0000");
    let environment = [
        ("GIT_AUTHOR_NAME", "Aegisy Checkpoint"),
        ("GIT_AUTHOR_EMAIL", "checkpoint@aegisy.local"),
        ("GIT_COMMITTER_NAME", "Aegisy Checkpoint"),
        ("GIT_COMMITTER_EMAIL", "checkpoint@aegisy.local"),
        ("GIT_AUTHOR_DATE", date.as_str()),
        ("GIT_COMMITTER_DATE", date.as_str()),
    ];
    let message = format!("Aegisy checkpoint {checkpoint_id}\n");
    let output = git_output(
        root,
        &["commit-tree", tree_oid],
        Some(message.as_bytes()),
        &environment,
        false,
    )?;
    parse_oid(&output)
}

fn create_checkpoint_reference(
    root: &Path,
    reference: &str,
    commit_oid: &str,
) -> Result<(), GitCheckpointError> {
    let zero = "0".repeat(commit_oid.len());
    git_output(
        root,
        &["update-ref", reference, commit_oid, &zero],
        None,
        &[],
        false,
    )?;
    Ok(())
}

fn temporary_index_path(checkpoint_id: &str) -> PathBuf {
    let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    std::env::temp_dir().join(format!(
        "aegisy-checkpoint-{checkpoint_id}-{}-{sequence}.index",
        std::process::id()
    ))
}

fn required_oid(root: &Path, args: &[&str]) -> Result<String, GitCheckpointError> {
    let output = git_output(root, args, None, &[], false)?;
    parse_oid(&output)
}

fn optional_oid(root: &Path, args: &[&str]) -> Result<Option<String>, GitCheckpointError> {
    match git_output(root, args, None, &[], true) {
        Ok(output) if output.is_empty() => Ok(None),
        Ok(output) => parse_oid(&output).map(Some),
        Err(_) => Ok(None),
    }
}

fn required_text(root: &Path, args: &[&str]) -> Result<String, GitCheckpointError> {
    let output = git_output(root, args, None, &[], false)?;
    let value = std::str::from_utf8(&output)
        .map_err(|_| error("Git output is not UTF-8"))?
        .trim();
    if value.is_empty() {
        return Err(error("Git command returned an empty value"));
    }
    Ok(value.into())
}

fn optional_text(root: &Path, args: &[&str]) -> Result<Option<String>, GitCheckpointError> {
    let output = match git_output(root, args, None, &[], true) {
        Ok(output) => output,
        Err(_) => return Ok(None),
    };
    let value = std::str::from_utf8(&output)
        .map_err(|_| error("Git output is not UTF-8"))?
        .trim();
    Ok((!value.is_empty()).then(|| value.into()))
}

fn parse_oid(bytes: &[u8]) -> Result<String, GitCheckpointError> {
    let value = std::str::from_utf8(bytes)
        .map_err(|_| error("Git object ID is not UTF-8"))?
        .trim();
    if !matches!(value.len(), 40 | 64)
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
        return Err(error("Git command returned an invalid object ID"));
    }
    Ok(value.into())
}

fn git_output(
    root: &Path,
    args: &[&str],
    input: Option<&[u8]>,
    environment: &[(&str, &str)],
    allow_failure: bool,
) -> Result<Vec<u8>, GitCheckpointError> {
    let executable = resolve_git_executable(root)?;
    let mut command = Command::new(&executable);
    let hook_override = format!("core.hooksPath={}", null_device());
    command
        .arg("-c")
        .arg(hook_override)
        .arg("-c")
        .arg("core.fsmonitor=false")
        .arg("-c")
        .arg("commit.gpgsign=false")
        .arg("-C")
        .arg(root)
        .args(args)
        .stdin(if input.is_some() {
            Stdio::piped()
        } else {
            Stdio::null()
        })
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .env_clear()
        .env("PATH", minimal_git_path(&executable)?)
        .env("LC_ALL", "C")
        .env("LANG", "C")
        .env("GIT_TERMINAL_PROMPT", "0")
        .env("GIT_CONFIG_NOSYSTEM", "1")
        .env("GIT_CONFIG_GLOBAL", null_device());
    for name in ["SystemRoot", "SYSTEMROOT", "TEMP", "TMP", "TMPDIR"] {
        if let Some(value) = safe_environment_path(name, root) {
            command.env(name, value);
        }
    }
    for (name, value) in environment {
        command.env(name, value);
    }
    let mut child = command
        .spawn()
        .map_err(|cause| error(format!("cannot start Git checkpoint command: {cause}")))?;
    if let Some(bytes) = input {
        child
            .stdin
            .take()
            .ok_or_else(|| error("Git checkpoint stdin is unavailable"))?
            .write_all(bytes)
            .map_err(|_| error("cannot write Git checkpoint input"))?;
    }
    let mut output = Vec::new();
    child
        .stdout
        .take()
        .ok_or_else(|| error("Git checkpoint stdout is unavailable"))?
        .take(MAX_STATUS_BYTES as u64 + 1)
        .read_to_end(&mut output)
        .map_err(|_| error("cannot read Git checkpoint output"))?;
    if output.len() > MAX_STATUS_BYTES {
        let _ = child.kill();
        let _ = child.wait();
        return Err(error("Git checkpoint command output exceeds limit"));
    }
    let status = child
        .wait()
        .map_err(|_| error("cannot wait for Git checkpoint command"))?;
    if !status.success() && !allow_failure {
        return Err(error("Git checkpoint command failed"));
    }
    if !status.success() {
        return Err(error("optional Git checkpoint value is unavailable"));
    }
    Ok(output)
}

fn resolve_git_executable(root: &Path) -> Result<PathBuf, GitCheckpointError> {
    let executable_name = if cfg!(windows) { "git.exe" } else { "git" };
    #[cfg_attr(windows, allow(unused_mut))]
    let mut candidates = std::env::var_os("PATH")
        .map(|path| {
            std::env::split_paths(&path)
                .filter(|directory| directory.is_absolute())
                .map(|directory| directory.join(executable_name))
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    #[cfg(not(windows))]
    {
        candidates.push(PathBuf::from("/usr/bin/git"));
        candidates.push(PathBuf::from("/opt/homebrew/bin/git"));
        candidates.push(PathBuf::from("/usr/local/bin/git"));
    }
    for candidate in candidates {
        let Ok(canonical) = candidate.canonicalize() else {
            continue;
        };
        if canonical.is_file() && !canonical.starts_with(root) {
            return Ok(canonical);
        }
    }
    Err(error(
        "Git checkpoint could not find a trusted executable outside the project root",
    ))
}

fn minimal_git_path(executable: &Path) -> Result<std::ffi::OsString, GitCheckpointError> {
    let mut paths = Vec::new();
    if let Some(parent) = executable.parent() {
        paths.push(parent.to_path_buf());
    }
    #[cfg(not(windows))]
    {
        paths.push(PathBuf::from("/usr/bin"));
        paths.push(PathBuf::from("/bin"));
    }
    std::env::join_paths(paths).map_err(|_| error("cannot construct minimal Git PATH"))
}

fn safe_environment_path(name: &str, root: &Path) -> Option<std::ffi::OsString> {
    let value = std::env::var_os(name)?;
    let path = PathBuf::from(&value);
    if !path.is_absolute() {
        return None;
    }
    match path.canonicalize() {
        Ok(canonical) if !canonical.starts_with(root) => Some(canonical.into_os_string()),
        _ => None,
    }
}

#[cfg(windows)]
fn null_device() -> &'static str {
    "NUL"
}

#[cfg(not(windows))]
fn null_device() -> &'static str {
    "/dev/null"
}

fn error(message: impl Into<String>) -> GitCheckpointError {
    GitCheckpointError {
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::workspace_edit::ProposedContent;
    use crate::workspace_edit_apply::apply_workspace_edit;
    use crate::workspace_edit_preview::ContentInput;
    use serde_json::Value;

    fn fixture_root() -> PathBuf {
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "aegisy-git-checkpoint-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(&root).unwrap();
        git(&root, &["init", "-q"]);
        git(&root, &["config", "user.name", "Aegisy Test"]);
        git(&root, &["config", "user.email", "test@aegisy.local"]);
        fs::write(root.join("agent.txt"), "agent-before\n").unwrap();
        fs::write(root.join("user.txt"), "user-before\n").unwrap();
        fs::write(root.join("staged.txt"), "staged-before\n").unwrap();
        fs::write(root.join("old.txt"), "rename-before\n").unwrap();
        fs::write(root.join(".env"), "secret-before\n").unwrap();
        git(&root, &["add", "."]);
        git(&root, &["commit", "-q", "-m", "initial"]);
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

    fn fixture_edit(root: &Path) -> WorkspaceEdit {
        let agent = ProposedContent::for_bytes(b"agent-after\n");
        let user = ProposedContent::for_bytes(b"user-after\n");
        let created = ProposedContent::for_bytes(b"created\n");
        WorkspaceEdit::define(
            "git-checkpoint-edit",
            "project-1",
            root,
            vec![
                WorkspaceEditOperation::Update {
                    path: "agent.txt".into(),
                    base: ContentHash::for_bytes(b"agent-before\n"),
                    content: agent,
                },
                WorkspaceEditOperation::Update {
                    path: "user.txt".into(),
                    base: ContentHash::for_bytes(b"user-dirty\n"),
                    content: user,
                },
                WorkspaceEditOperation::Create {
                    path: "created.txt".into(),
                    content: created,
                },
                WorkspaceEditOperation::Rename {
                    from_path: "old.txt".into(),
                    to_path: "new.txt".into(),
                    base: ContentHash::for_bytes(b"rename-before\n"),
                },
            ],
        )
        .unwrap()
    }

    #[test]
    fn parses_porcelain_v2_ordinary_rename_unmerged_and_untracked_records() {
        let oid = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        let status = format!(
            "1 M. N... 100644 100644 100644 {oid} {oid} ordinary.txt\0\
             2 R. N... 100644 100644 100644 {oid} {oid} R100 renamed.txt\0original.txt\0\
             u UU N... 100644 100644 100644 100644 {oid} {oid} {oid} conflict.txt\0\
             ? untracked.txt\0"
        );
        let entries = parse_porcelain_v2(status.as_bytes()).unwrap();
        assert_eq!(entries.len(), 4);
        assert_eq!(entries[0].path, "ordinary.txt");
        assert_eq!(entries[1].path, "renamed.txt");
        assert_eq!(entries[1].original_path.as_deref(), Some("original.txt"));
        assert_eq!(entries[2].kind, "unmerged");
        assert_eq!(entries[3].kind, "untracked");
        let dirty = dirty_path_set(&entries);
        assert!(dirty.contains("renamed.txt"));
        assert!(dirty.contains("original.txt"));
    }

    #[test]
    fn captures_preimages_in_an_isolated_ref_without_changing_worktree_or_index() {
        let root = fixture_root();
        fs::write(root.join("user.txt"), "user-dirty\n").unwrap();
        fs::write(root.join("staged.txt"), "staged-dirty\n").unwrap();
        git(&root, &["add", "staged.txt"]);
        fs::write(root.join("untracked.txt"), "untracked\n").unwrap();
        fs::write(root.join(".env"), "secret-dirty\n").unwrap();
        let index_before = String::from_utf8(git(&root, &["write-tree"]))
            .unwrap()
            .trim()
            .to_owned();
        let edit = fixture_edit(&root);

        let checkpoint = capture_git_checkpoint(
            "checkpoint-1",
            &edit,
            &HashSet::new(),
            &HashSet::from(["notes.txt".into(), ".env".into()]),
        )
        .unwrap();
        assert_eq!(checkpoint.index_tree_oid, index_before);
        assert_eq!(checkpoint.paths.len(), 5);
        assert_eq!(checkpoint.pre_existing_changes.len(), 3);
        assert_eq!(checkpoint.pending_user_paths, vec!["notes.txt"]);
        assert_eq!(checkpoint.redacted_pre_existing_count, 2);
        let user = checkpoint
            .paths
            .iter()
            .find(|path| path.path == "user.txt")
            .unwrap();
        assert!(user.pre_existing_user_change);
        assert_eq!(user.ownership, "agent-on-user-base");
        let agent = checkpoint
            .paths
            .iter()
            .find(|path| path.path == "agent.txt")
            .unwrap();
        assert!(!agent.pre_existing_user_change);
        assert_eq!(agent.ownership, "agent-only");

        let index_after = String::from_utf8(git(&root, &["write-tree"]))
            .unwrap()
            .trim()
            .to_owned();
        assert_eq!(index_after, index_before);
        assert_eq!(
            fs::read_to_string(root.join("agent.txt")).unwrap(),
            "agent-before\n"
        );
        assert_eq!(
            fs::read_to_string(root.join("user.txt")).unwrap(),
            "user-dirty\n"
        );
        assert!(!root.join("created.txt").exists());
        assert!(root.join("old.txt").exists());
        assert!(!root.join("new.txt").exists());

        let reference_oid = String::from_utf8(git(&root, &["rev-parse", &checkpoint.reference]))
            .unwrap()
            .trim()
            .to_owned();
        assert_eq!(reference_oid, checkpoint.commit_oid);
        assert_eq!(
            String::from_utf8(git(
                &root,
                &["show", &format!("{}:files/user.txt", checkpoint.reference)]
            ))
            .unwrap(),
            "user-dirty\n"
        );
        let manifest_bytes = git(
            &root,
            &["show", &format!("{}:manifest.json", checkpoint.reference)],
        );
        assert!(!String::from_utf8_lossy(&manifest_bytes).contains(".env"));
        let manifest: Value = serde_json::from_slice(&manifest_bytes).unwrap();
        assert_eq!(manifest["checkpoint_id"], "checkpoint-1");
        assert_eq!(manifest["index_tree_oid"], index_before);

        let agent = ProposedContent::for_bytes(b"agent-after\n");
        let user_content = ProposedContent::for_bytes(b"user-after\n");
        let created = ProposedContent::for_bytes(b"created\n");
        let applied = apply_workspace_edit(
            edit.clone(),
            vec![
                ContentInput {
                    reference: agent.reference,
                    content: "agent-after\n".into(),
                },
                ContentInput {
                    reference: user_content.reference,
                    content: "user-after\n".into(),
                },
                ContentInput {
                    reference: created.reference,
                    content: "created\n".into(),
                },
            ],
            &HashSet::new(),
        )
        .unwrap();
        let application = bind_git_checkpoint_application(&checkpoint, &edit, &applied).unwrap();
        assert_eq!(application.paths.len(), 5);
        assert_eq!(
            application
                .paths
                .iter()
                .find(|path| path.path == "user.txt")
                .unwrap()
                .ownership,
            "agent-on-user-base"
        );
        let mut forged_mode = applied.clone();
        forged_mode.files[0].final_mode = Some("100755".into());
        assert!(bind_git_checkpoint_application(&checkpoint, &edit, &forged_mode).is_err());
        let mut forged = checkpoint.clone();
        forged.paths[0].ownership = "agent-on-user-base".into();
        assert!(bind_git_checkpoint_application(&forged, &edit, &applied).is_err());
        fs::write(root.join("later-staged.txt"), "later\n").unwrap();
        git(&root, &["add", "later-staged.txt"]);
        assert!(bind_git_checkpoint_application(&checkpoint, &edit, &applied).is_err());

        let original_ref = checkpoint.commit_oid.clone();
        assert!(
            capture_git_checkpoint("checkpoint-1", &edit, &HashSet::new(), &HashSet::new())
                .is_err()
        );
        let retained_ref = String::from_utf8(git(&root, &["rev-parse", &checkpoint.reference]))
            .unwrap()
            .trim()
            .to_owned();
        assert_eq!(retained_ref, original_ref);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_stale_ignored_symlink_and_unsafe_checkpoint_inputs() {
        let root = fixture_root();
        fs::write(root.join("user.txt"), "user-dirty\n").unwrap();
        let edit = fixture_edit(&root);
        let mut stale = edit.clone();
        if let WorkspaceEditOperation::Update { base, .. } = &mut stale.operations[0] {
            *base = ContentHash::for_bytes(b"stale\n");
        }
        assert!(capture_git_checkpoint("stale", &stale, &HashSet::new(), &HashSet::new()).is_err());
        assert!(capture_git_checkpoint(
            "ignored",
            &edit,
            &HashSet::from(["agent.txt".into()]),
            &HashSet::new()
        )
        .is_err());
        assert!(capture_git_checkpoint(
            "pending",
            &edit,
            &HashSet::new(),
            &HashSet::from(["agent.txt".into()])
        )
        .is_err());
        assert!(
            capture_git_checkpoint("../unsafe", &edit, &HashSet::new(), &HashSet::new()).is_err()
        );
        assert!(
            capture_git_checkpoint("unsafe.lock", &edit, &HashSet::new(), &HashSet::new()).is_err()
        );

        #[cfg(unix)]
        {
            use std::os::unix::fs::symlink;
            fs::remove_file(root.join("agent.txt")).unwrap();
            symlink(root.join("user.txt"), root.join("agent.txt")).unwrap();
            assert!(
                capture_git_checkpoint("symlink", &edit, &HashSet::new(), &HashSet::new()).is_err()
            );
        }
        assert!(git(
            &root,
            &["for-each-ref", "--format=%(refname)", "refs/aegisy/"]
        )
        .is_empty());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_parent_repository_and_external_worktree_git_directories() {
        let root = fixture_root();
        let nested = root.join("nested");
        fs::create_dir_all(&nested).unwrap();
        let content = ProposedContent::for_bytes(b"new\n");
        let edit = WorkspaceEdit::define(
            "nested-edit",
            "project-1",
            &nested,
            vec![WorkspaceEditOperation::Create {
                path: "new.txt".into(),
                content,
            }],
        )
        .unwrap();
        assert!(capture_git_checkpoint("nested", &edit, &HashSet::new(), &HashSet::new()).is_err());
        fs::remove_dir_all(root).unwrap();
    }
}
