use crate::workspace::is_sensitive_path;
use crate::workspace_edit::{ContentHash, WorkspaceEdit, WorkspaceEditOperation};
use crate::workspace_edit_apply::WorkspaceEditApplyResult;
use serde::Serialize;
use std::collections::HashSet;
use std::fs::{self, File};
use std::io::Read;
use std::path::{Component, Path, PathBuf};

const BASELINE_SCHEMA_VERSION: &str = "workspace-edit-overlap/0.1";
const MAX_OBSERVED_FILE_BYTES: u64 = 512 * 1024;
const MAX_OBSERVED_TOTAL_BYTES: u64 = 8 * 1024 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorkspaceEditOverlapError {
    pub message: String,
}

#[derive(Debug, Clone, Copy, Serialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum WorkspaceEditOverlapPhase {
    BeforeApply,
    BeforeRestore,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceEditOverlapBaseline {
    pub schema_version: String,
    pub edit_id: String,
    pub project_id: String,
    pub root_identity: String,
    pub phase: WorkspaceEditOverlapPhase,
    pub paths: Vec<WorkspaceEditPathExpectation>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceEditPathExpectation {
    pub operation: String,
    pub role: String,
    pub path: String,
    pub expected: ExpectedWorkspacePathState,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub enum ExpectedWorkspacePathState {
    Absent,
    File { hash: ContentHash },
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub enum ObservedWorkspacePathState {
    Absent,
    File { hash: ContentHash },
    Unavailable { reason: String },
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceEditOverlapReport {
    pub edit_id: String,
    pub project_id: String,
    pub root_identity: String,
    pub phase: WorkspaceEditOverlapPhase,
    pub blocking: bool,
    pub overlap_count: usize,
    pub paths: Vec<WorkspaceEditPathOverlap>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceEditPathOverlap {
    pub operation: String,
    pub role: String,
    pub path: String,
    pub expected: ExpectedWorkspacePathState,
    pub current: ObservedWorkspacePathState,
    pub status: String,
    pub overlaps: bool,
    pub pending_user_edit: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub resolution: Option<String>,
}

pub fn proposal_overlap_baseline(
    edit: &WorkspaceEdit,
) -> Result<WorkspaceEditOverlapBaseline, WorkspaceEditOverlapError> {
    validate_edit_binding(edit)?;
    Ok(baseline_for_phase(
        edit,
        WorkspaceEditOverlapPhase::BeforeApply,
    ))
}

pub fn restore_overlap_baseline(
    edit: &WorkspaceEdit,
    applied: &WorkspaceEditApplyResult,
) -> Result<WorkspaceEditOverlapBaseline, WorkspaceEditOverlapError> {
    validate_edit_binding(edit)?;
    validate_apply_result(edit, applied)?;
    Ok(baseline_for_phase(
        edit,
        WorkspaceEditOverlapPhase::BeforeRestore,
    ))
}

pub fn planned_restore_overlap_baseline(
    edit: &WorkspaceEdit,
) -> Result<WorkspaceEditOverlapBaseline, WorkspaceEditOverlapError> {
    validate_edit_binding(edit)?;
    Ok(baseline_for_phase(
        edit,
        WorkspaceEditOverlapPhase::BeforeRestore,
    ))
}

pub fn detect_workspace_edit_overlaps(
    edit: &WorkspaceEdit,
    baseline: &WorkspaceEditOverlapBaseline,
    ignored_paths: &HashSet<String>,
    pending_user_paths: &HashSet<String>,
) -> Result<WorkspaceEditOverlapReport, WorkspaceEditOverlapError> {
    validate_edit_binding(edit)?;
    let expected_baseline = baseline_for_phase(edit, baseline.phase);
    if *baseline != expected_baseline {
        return Err(error(
            "workspace edit overlap baseline does not match the validated edit",
        ));
    }

    let root = &edit.root.canonical_path;
    validate_root(root)?;
    let mut observed_bytes = 0_u64;
    let mut paths = Vec::with_capacity(baseline.paths.len());
    for expectation in &baseline.paths {
        let pending_user_edit = pending_user_paths.contains(&expectation.path);
        let current = observe_path(root, &expectation.path, ignored_paths, &mut observed_bytes);
        let status = overlap_status(&expectation.expected, &current, pending_user_edit);
        let overlaps = status != "unchanged";
        let resolution = overlaps.then(|| match baseline.phase {
            WorkspaceEditOverlapPhase::BeforeApply => "regenerate-rebase-or-resolve".into(),
            WorkspaceEditOverlapPhase::BeforeRestore => {
                "selective-restore-or-explicit-confirmation".into()
            }
        });
        paths.push(WorkspaceEditPathOverlap {
            operation: expectation.operation.clone(),
            role: expectation.role.clone(),
            path: expectation.path.clone(),
            expected: expectation.expected.clone(),
            current,
            status,
            overlaps,
            pending_user_edit,
            resolution,
        });
    }
    let overlap_count = paths.iter().filter(|path| path.overlaps).count();
    Ok(WorkspaceEditOverlapReport {
        edit_id: edit.edit_id.clone(),
        project_id: edit.project_id.clone(),
        root_identity: edit.root.identity.clone(),
        phase: baseline.phase,
        blocking: overlap_count != 0,
        overlap_count,
        paths,
    })
}

fn baseline_for_phase(
    edit: &WorkspaceEdit,
    phase: WorkspaceEditOverlapPhase,
) -> WorkspaceEditOverlapBaseline {
    let mut paths = Vec::with_capacity(edit.operations.len().saturating_mul(2));
    for operation in &edit.operations {
        match (phase, operation) {
            (
                WorkspaceEditOverlapPhase::BeforeApply,
                WorkspaceEditOperation::Create { path, .. },
            ) => push_expectation(&mut paths, "create", "target", path, None),
            (
                WorkspaceEditOverlapPhase::BeforeApply,
                WorkspaceEditOperation::Update { path, base, .. },
            ) => push_expectation(&mut paths, "update", "target", path, Some(base.clone())),
            (
                WorkspaceEditOverlapPhase::BeforeApply,
                WorkspaceEditOperation::Delete { path, base },
            ) => push_expectation(&mut paths, "delete", "target", path, Some(base.clone())),
            (
                WorkspaceEditOverlapPhase::BeforeApply,
                WorkspaceEditOperation::Rename {
                    from_path,
                    to_path,
                    base,
                },
            ) => {
                push_expectation(
                    &mut paths,
                    "rename",
                    "source",
                    from_path,
                    Some(base.clone()),
                );
                push_expectation(&mut paths, "rename", "target", to_path, None);
            }
            (
                WorkspaceEditOverlapPhase::BeforeRestore,
                WorkspaceEditOperation::Create { path, content },
            ) => push_expectation(
                &mut paths,
                "create",
                "target",
                path,
                Some(content.hash.clone()),
            ),
            (
                WorkspaceEditOverlapPhase::BeforeRestore,
                WorkspaceEditOperation::Update { path, content, .. },
            ) => push_expectation(
                &mut paths,
                "update",
                "target",
                path,
                Some(content.hash.clone()),
            ),
            (
                WorkspaceEditOverlapPhase::BeforeRestore,
                WorkspaceEditOperation::Delete { path, .. },
            ) => push_expectation(&mut paths, "delete", "target", path, None),
            (
                WorkspaceEditOverlapPhase::BeforeRestore,
                WorkspaceEditOperation::Rename {
                    from_path,
                    to_path,
                    base,
                },
            ) => {
                push_expectation(&mut paths, "rename", "source", from_path, None);
                push_expectation(&mut paths, "rename", "target", to_path, Some(base.clone()));
            }
        }
    }
    WorkspaceEditOverlapBaseline {
        schema_version: BASELINE_SCHEMA_VERSION.into(),
        edit_id: edit.edit_id.clone(),
        project_id: edit.project_id.clone(),
        root_identity: edit.root.identity.clone(),
        phase,
        paths,
    }
}

fn push_expectation(
    paths: &mut Vec<WorkspaceEditPathExpectation>,
    operation: &str,
    role: &str,
    path: &str,
    hash: Option<ContentHash>,
) {
    paths.push(WorkspaceEditPathExpectation {
        operation: operation.into(),
        role: role.into(),
        path: path.into(),
        expected: hash.map_or(ExpectedWorkspacePathState::Absent, |hash| {
            ExpectedWorkspacePathState::File { hash }
        }),
    });
}

fn validate_edit_binding(edit: &WorkspaceEdit) -> Result<(), WorkspaceEditOverlapError> {
    let rebuilt = WorkspaceEdit::define(
        edit.edit_id.clone(),
        edit.project_id.clone(),
        &edit.root.canonical_path,
        edit.operations.clone(),
    )
    .map_err(|cause| error(cause.message))?;
    if rebuilt != *edit {
        return Err(error(
            "workspace edit root or normalized schema was modified after validation",
        ));
    }
    Ok(())
}

fn validate_apply_result(
    edit: &WorkspaceEdit,
    applied: &WorkspaceEditApplyResult,
) -> Result<(), WorkspaceEditOverlapError> {
    if !applied.committed
        || applied.edit_id != edit.edit_id
        || applied.project_id != edit.project_id
        || applied.root_identity != edit.root.identity
        || applied.files.len() != edit.operations.len()
    {
        return Err(error(
            "workspace edit apply result is incomplete or belongs to another edit",
        ));
    }
    for (operation, file) in edit.operations.iter().zip(&applied.files) {
        let valid = match operation {
            WorkspaceEditOperation::Create { path, content } => {
                file.kind == "create"
                    && file.path == *path
                    && file.from_path.is_none()
                    && file.base_hash.is_none()
                    && file.final_hash.as_ref() == Some(&content.hash)
                    && result_mode_matches(file.final_mode.as_deref(), &content.format.mode)
            }
            WorkspaceEditOperation::Update {
                path,
                base,
                content,
            } => {
                file.kind == "update"
                    && file.path == *path
                    && file.from_path.is_none()
                    && file.base_hash.as_ref() == Some(base)
                    && file.final_hash.as_ref() == Some(&content.hash)
                    && result_mode_matches(file.final_mode.as_deref(), &content.format.mode)
            }
            WorkspaceEditOperation::Delete { path, base } => {
                file.kind == "delete"
                    && file.path == *path
                    && file.from_path.is_none()
                    && file.base_hash.as_ref() == Some(base)
                    && file.final_hash.is_none()
                    && file.final_mode.is_none()
            }
            WorkspaceEditOperation::Rename {
                from_path,
                to_path,
                base,
            } => {
                file.kind == "rename"
                    && file.path == *to_path
                    && file.from_path.as_ref() == Some(from_path)
                    && file.base_hash.as_ref() == Some(base)
                    && file.final_hash.as_ref() == Some(base)
                    && matches!(file.final_mode.as_deref(), Some("100644" | "100755"))
            }
        };
        if !valid {
            return Err(error(
                "workspace edit apply result final hashes do not match the edit",
            ));
        }
    }
    Ok(())
}

fn result_mode_matches(actual: Option<&str>, declared: &str) -> bool {
    match declared {
        "preserve" => matches!(actual, Some("100644" | "100755")),
        "regular" => actual == Some("100644"),
        "executable" => actual == Some("100755"),
        _ => false,
    }
}

fn validate_root(root: &Path) -> Result<(), WorkspaceEditOverlapError> {
    let canonical = root
        .canonicalize()
        .map_err(|_| error("workspace edit root is unavailable"))?;
    if canonical != root || !canonical.is_dir() {
        return Err(error("workspace edit root changed after proposal"));
    }
    Ok(())
}

fn observe_path(
    root: &Path,
    relative: &str,
    ignored_paths: &HashSet<String>,
    observed_bytes: &mut u64,
) -> ObservedWorkspacePathState {
    if is_sensitive_path(Path::new(relative)) || ignored_paths.contains(relative) {
        return unavailable("policy-denied");
    }
    let path = match resolve_observed_path(root, relative) {
        Ok(path) => path,
        Err(reason) => return unavailable(reason),
    };
    let metadata = match fs::symlink_metadata(&path) {
        Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => {
            return ObservedWorkspacePathState::Absent
        }
        Err(_) => return unavailable("metadata-unavailable"),
        Ok(metadata) => metadata,
    };
    if metadata.file_type().is_symlink() {
        return unavailable("symlink-denied");
    }
    match path.canonicalize() {
        Ok(canonical) if canonical.starts_with(root) => {}
        Ok(_) => return unavailable("outside-root"),
        Err(_) => return unavailable("path-unavailable"),
    }
    if !metadata.is_file() {
        return unavailable("not-a-regular-file");
    }
    if metadata.len() > MAX_OBSERVED_FILE_BYTES {
        return unavailable("file-too-large");
    }
    if observed_bytes.saturating_add(metadata.len()) > MAX_OBSERVED_TOTAL_BYTES {
        return unavailable("read-budget-exceeded");
    }
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    if File::open(&path)
        .and_then(|file| {
            file.take(MAX_OBSERVED_FILE_BYTES + 1)
                .read_to_end(&mut bytes)
        })
        .is_err()
    {
        return unavailable("content-unavailable");
    }
    if bytes.len() as u64 > MAX_OBSERVED_FILE_BYTES {
        return unavailable("file-too-large");
    }
    *observed_bytes = observed_bytes.saturating_add(bytes.len() as u64);
    ObservedWorkspacePathState::File {
        hash: ContentHash::for_bytes(&bytes),
    }
}

fn resolve_observed_path(root: &Path, relative: &str) -> Result<PathBuf, &'static str> {
    let mut candidate = root.to_path_buf();
    let components = Path::new(relative).components().collect::<Vec<_>>();
    if components.is_empty() {
        return Err("path-invalid");
    }
    for (index, component) in components.iter().enumerate() {
        let Component::Normal(name) = component else {
            return Err("path-invalid");
        };
        candidate.push(name);
        let is_final = index + 1 == components.len();
        match fs::symlink_metadata(&candidate) {
            Ok(metadata) if metadata.file_type().is_symlink() => return Err("symlink-denied"),
            Ok(metadata) if !is_final && !metadata.is_dir() => return Err("parent-not-directory"),
            Ok(_) => {}
            Err(cause) if is_final && cause.kind() == std::io::ErrorKind::NotFound => {}
            Err(_) => return Err("path-unavailable"),
        }
    }
    Ok(candidate)
}

fn overlap_status(
    expected: &ExpectedWorkspacePathState,
    current: &ObservedWorkspacePathState,
    pending_user_edit: bool,
) -> String {
    if let ObservedWorkspacePathState::Unavailable { reason } = current {
        return reason.clone();
    }
    if pending_user_edit {
        return "pending-user-edit".into();
    }
    match (expected, current) {
        (ExpectedWorkspacePathState::Absent, ObservedWorkspacePathState::Absent) => {
            "unchanged".into()
        }
        (
            ExpectedWorkspacePathState::File { hash: expected },
            ObservedWorkspacePathState::File { hash: current },
        ) if expected == current => "unchanged".into(),
        (ExpectedWorkspacePathState::Absent, ObservedWorkspacePathState::File { .. }) => {
            "created-since-baseline".into()
        }
        (ExpectedWorkspacePathState::File { .. }, ObservedWorkspacePathState::Absent) => {
            "deleted-since-baseline".into()
        }
        (ExpectedWorkspacePathState::File { .. }, ObservedWorkspacePathState::File { .. }) => {
            "content-changed".into()
        }
        (_, ObservedWorkspacePathState::Unavailable { .. }) => {
            unreachable!("unavailable states return before comparison")
        }
    }
}

fn unavailable(reason: &str) -> ObservedWorkspacePathState {
    ObservedWorkspacePathState::Unavailable {
        reason: reason.into(),
    }
}

fn error(message: impl Into<String>) -> WorkspaceEditOverlapError {
    WorkspaceEditOverlapError {
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::workspace_edit::{ProposedContent, WorkspaceEditOperation};
    use crate::workspace_edit_apply::apply_workspace_edit;
    use crate::workspace_edit_preview::ContentInput;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::{SystemTime, UNIX_EPOCH};

    static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

    fn root() -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let sequence = FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!("aegisy-edit-overlap-{nonce}-{sequence}"));
        fs::create_dir_all(root.join("src")).unwrap();
        root.canonicalize().unwrap()
    }

    fn fixture_edit(root: &Path) -> (WorkspaceEdit, ProposedContent, ProposedContent) {
        fs::write(root.join("src/update.rs"), "update-before\n").unwrap();
        fs::write(root.join("src/delete.rs"), "delete-before\n").unwrap();
        fs::write(root.join("src/old.rs"), "rename-before\n").unwrap();
        let created = ProposedContent::for_bytes(b"created\n");
        let updated = ProposedContent::for_bytes(b"update-after\n");
        let edit = WorkspaceEdit::define(
            "overlap-edit",
            "project-1",
            root,
            vec![
                WorkspaceEditOperation::Create {
                    path: "src/create.rs".into(),
                    content: created.clone(),
                },
                WorkspaceEditOperation::Update {
                    path: "src/update.rs".into(),
                    base: ContentHash::for_bytes(b"update-before\n"),
                    content: updated.clone(),
                },
                WorkspaceEditOperation::Delete {
                    path: "src/delete.rs".into(),
                    base: ContentHash::for_bytes(b"delete-before\n"),
                },
                WorkspaceEditOperation::Rename {
                    from_path: "src/old.rs".into(),
                    to_path: "src/new.rs".into(),
                    base: ContentHash::for_bytes(b"rename-before\n"),
                },
            ],
        )
        .unwrap();
        (edit, created, updated)
    }

    fn input(content: &ProposedContent, body: &str) -> ContentInput {
        ContentInput {
            reference: content.reference.clone(),
            content: body.into(),
        }
    }

    #[test]
    fn proposal_baseline_detects_disk_and_pending_user_overlaps() {
        let root = root();
        let (edit, _, _) = fixture_edit(&root);
        let baseline = proposal_overlap_baseline(&edit).unwrap();
        let clean =
            detect_workspace_edit_overlaps(&edit, &baseline, &HashSet::new(), &HashSet::new())
                .unwrap();
        assert!(!clean.blocking);
        assert_eq!(clean.paths.len(), 5);

        fs::write(root.join("src/create.rs"), "occupied\n").unwrap();
        fs::write(root.join("src/update.rs"), "user update\n").unwrap();
        fs::remove_file(root.join("src/delete.rs")).unwrap();
        fs::write(root.join("src/new.rs"), "occupied rename target\n").unwrap();
        let report = detect_workspace_edit_overlaps(
            &edit,
            &baseline,
            &HashSet::new(),
            &HashSet::from(["src/old.rs".into()]),
        )
        .unwrap();
        assert!(report.blocking);
        assert_eq!(report.overlap_count, 5);
        assert_eq!(report.paths[0].status, "created-since-baseline");
        assert_eq!(report.paths[1].status, "content-changed");
        assert_eq!(report.paths[2].status, "deleted-since-baseline");
        assert_eq!(report.paths[3].status, "pending-user-edit");
        assert_eq!(report.paths[4].status, "created-since-baseline");
        assert!(report
            .paths
            .iter()
            .all(|path| path.resolution.as_deref() == Some("regenerate-rebase-or-resolve")));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn restore_baseline_detects_later_changes_to_every_agent_path() {
        let root = root();
        let (edit, created, updated) = fixture_edit(&root);
        let applied = apply_workspace_edit(
            edit.clone(),
            vec![
                input(&created, "created\n"),
                input(&updated, "update-after\n"),
            ],
            &HashSet::new(),
        )
        .unwrap();
        let baseline = restore_overlap_baseline(&edit, &applied).unwrap();
        let clean =
            detect_workspace_edit_overlaps(&edit, &baseline, &HashSet::new(), &HashSet::new())
                .unwrap();
        assert!(!clean.blocking);

        fs::write(root.join("src/create.rs"), "user create edit\n").unwrap();
        fs::write(root.join("src/update.rs"), "user update edit\n").unwrap();
        fs::write(root.join("src/delete.rs"), "user recreated\n").unwrap();
        fs::write(root.join("src/old.rs"), "user recreated source\n").unwrap();
        fs::write(root.join("src/new.rs"), "user rename edit\n").unwrap();
        let report =
            detect_workspace_edit_overlaps(&edit, &baseline, &HashSet::new(), &HashSet::new())
                .unwrap();
        assert_eq!(report.overlap_count, 5);
        assert!(report.paths.iter().all(|path| path.overlaps));
        assert!(report
            .paths
            .iter()
            .all(|path| path.resolution.as_deref()
                == Some("selective-restore-or-explicit-confirmation")));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_policy_paths_and_forged_apply_results() {
        let root = root();
        let content = ProposedContent::for_bytes(b"new\n");
        let edit = WorkspaceEdit::define(
            "overlap-policy",
            "project-1",
            &root,
            vec![
                WorkspaceEditOperation::Create {
                    path: ".env".into(),
                    content: content.clone(),
                },
                WorkspaceEditOperation::Create {
                    path: "src/ignored.rs".into(),
                    content: content.clone(),
                },
            ],
        )
        .unwrap();
        let baseline = proposal_overlap_baseline(&edit).unwrap();
        let report = detect_workspace_edit_overlaps(
            &edit,
            &baseline,
            &HashSet::from(["src/ignored.rs".into()]),
            &HashSet::new(),
        )
        .unwrap();
        assert_eq!(report.overlap_count, 2);
        assert!(report
            .paths
            .iter()
            .all(|path| path.status == "policy-denied"));

        let (valid_edit, created, updated) = fixture_edit(&root);
        let mut applied = apply_workspace_edit(
            valid_edit.clone(),
            vec![
                input(&created, "created\n"),
                input(&updated, "update-after\n"),
            ],
            &HashSet::new(),
        )
        .unwrap();
        applied.root_identity = "workspace-root:sha256:forged".into();
        assert!(restore_overlap_baseline(&valid_edit, &applied).is_err());
        applied.root_identity = valid_edit.root.identity.clone();
        applied.files[0].final_hash = Some(ContentHash::for_bytes(b"forged\n"));
        assert!(restore_overlap_baseline(&valid_edit, &applied).is_err());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn bounds_observed_file_and_total_hashing() {
        let root = root();
        let body = vec![b'x'; MAX_OBSERVED_FILE_BYTES as usize];
        let mut operations = Vec::new();
        for index in 0..17 {
            let path = format!("src/file-{index}.txt");
            fs::write(root.join(&path), &body).unwrap();
            operations.push(WorkspaceEditOperation::Delete {
                path,
                base: ContentHash::for_bytes(&body),
            });
        }
        fs::write(
            root.join("src/too-large.txt"),
            vec![b'y'; MAX_OBSERVED_FILE_BYTES as usize + 1],
        )
        .unwrap();
        operations.push(WorkspaceEditOperation::Delete {
            path: "src/too-large.txt".into(),
            base: ContentHash::for_bytes(b"declared-small"),
        });
        let edit = WorkspaceEdit::define("overlap-bounds", "project-1", &root, operations).unwrap();
        let baseline = proposal_overlap_baseline(&edit).unwrap();
        let report =
            detect_workspace_edit_overlaps(&edit, &baseline, &HashSet::new(), &HashSet::new())
                .unwrap();
        assert_eq!(report.paths[16].status, "read-budget-exceeded");
        assert_eq!(report.paths[17].status, "file-too-large");
        assert_eq!(report.overlap_count, 2);
        fs::remove_dir_all(root).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn symlinked_current_path_is_an_overlap_without_reading_its_target() {
        use std::os::unix::fs::symlink;

        let root = root();
        let outside = root.parent().unwrap().join(format!(
            "aegisy-overlap-outside-{}",
            FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ));
        fs::write(&outside, "outside\n").unwrap();
        let content = ProposedContent::for_bytes(b"new\n");
        let edit = WorkspaceEdit::define(
            "overlap-symlink",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Create {
                path: "src/link.rs".into(),
                content,
            }],
        )
        .unwrap();
        let baseline = proposal_overlap_baseline(&edit).unwrap();
        symlink(&outside, root.join("src/link.rs")).unwrap();
        let report =
            detect_workspace_edit_overlaps(&edit, &baseline, &HashSet::new(), &HashSet::new())
                .unwrap();
        assert_eq!(report.paths[0].status, "symlink-denied");
        assert_eq!(fs::read_to_string(&outside).unwrap(), "outside\n");
        fs::remove_file(root.join("src/link.rs")).unwrap();
        fs::remove_file(outside).unwrap();
        fs::remove_dir_all(root).unwrap();
    }
}
