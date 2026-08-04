use crate::workspace::is_sensitive_path;
use crate::workspace_edit::{
    inspect_text_format, ContentHash, WorkspaceEdit, WorkspaceEditOperation,
};
use crate::workspace_edit_preview::{validate_contents, ContentInput};
use serde::Serialize;
use std::collections::{HashMap, HashSet};
use std::fs::{self, File, OpenOptions, Permissions};
use std::io::{self, Read, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

const MAX_BASE_FILE_BYTES: u64 = 512 * 1024;
const MAX_JOURNAL_BYTES: usize = 8 * 1024 * 1024;
static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceEditApplyResult {
    pub edit_id: String,
    pub project_id: String,
    pub root_identity: String,
    pub committed: bool,
    pub journal_entries: usize,
    pub files: Vec<AppliedFile>,
    pub cleanup_warnings: Vec<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct AppliedFile {
    pub kind: String,
    pub path: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub from_path: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub base_hash: Option<ContentHash>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub final_hash: Option<ContentHash>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub final_mode: Option<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceEditApplyFailure {
    pub stage: String,
    pub message: String,
    pub stale_paths: Vec<String>,
    pub rollback_complete: bool,
    pub authoritative_states: Vec<AuthoritativeFileState>,
    pub recovery_artifacts: Vec<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct AuthoritativeFileState {
    pub path: String,
    pub exists: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub hash: Option<ContentHash>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
}

pub fn apply_workspace_edit(
    edit: WorkspaceEdit,
    contents: Vec<ContentInput>,
    ignored_paths: &HashSet<String>,
) -> Result<WorkspaceEditApplyResult, Box<WorkspaceEditApplyFailure>> {
    apply_with_fault(edit, contents, ignored_paths, &NoFault)
}

trait FaultInjector {
    fn write_stage(&self, file: &mut File, content: &[u8]) -> io::Result<()> {
        file.write_all(content)
    }

    fn after_stage(&self) -> io::Result<()> {
        Ok(())
    }

    fn after_commit(&self, _committed: usize) -> io::Result<()> {
        Ok(())
    }
}

struct NoFault;

impl FaultInjector for NoFault {}

fn apply_with_fault<F: FaultInjector>(
    edit: WorkspaceEdit,
    contents: Vec<ContentInput>,
    ignored_paths: &HashSet<String>,
    fault: &F,
) -> Result<WorkspaceEditApplyResult, Box<WorkspaceEditApplyFailure>> {
    let root = validate_edit_binding(&edit)?;
    let proposed = validate_contents(&edit, contents).map_err(|cause| {
        Box::new(failure(
            "preflight",
            cause.message,
            Vec::new(),
            true,
            &root,
            &edit,
            Vec::new(),
        ))
    })?;
    let mut prepared = prepare_operations(&root, &edit, &proposed, ignored_paths)?;
    if let Err(cause) = stage_new_content(&mut prepared, fault) {
        let recovery_artifacts = cleanup_stages(&prepared);
        return Err(Box::new(failure(
            "stage",
            cause.to_string(),
            Vec::new(),
            recovery_artifacts.is_empty(),
            &root,
            &edit,
            recovery_artifacts,
        )));
    }
    if let Err(cause) = fault.after_stage() {
        let recovery_artifacts = cleanup_stages(&prepared);
        return Err(Box::new(failure(
            "stage",
            cause.to_string(),
            Vec::new(),
            recovery_artifacts.is_empty(),
            &root,
            &edit,
            recovery_artifacts,
        )));
    }
    if let Err(cause) = revalidate_prepared(&prepared) {
        let recovery_artifacts = cleanup_stages(&prepared);
        return Err(Box::new(failure(
            "revalidate",
            cause.message,
            cause.stale_paths,
            recovery_artifacts.is_empty(),
            &root,
            &edit,
            recovery_artifacts,
        )));
    }

    let mut journal = Vec::with_capacity(prepared.len());
    let mut committed = 0_usize;
    for index in 0..prepared.len() {
        if let Err(cause) = revalidate_root(&root) {
            return Err(rollback_failure(
                "commit",
                cause.to_string(),
                &root,
                &edit,
                journal,
                &prepared,
            ));
        }
        let commit_result = commit_operation(&mut prepared[index], &mut journal);
        if let Err(cause) = commit_result {
            return Err(rollback_failure(
                "commit",
                cause.to_string(),
                &root,
                &edit,
                journal,
                &prepared,
            ));
        }
        committed += 1;
        if let Err(cause) = fault.after_commit(committed) {
            return Err(rollback_failure(
                "commit",
                cause.to_string(),
                &root,
                &edit,
                journal,
                &prepared,
            ));
        }
    }
    let stage_cleanup_warnings = cleanup_stages(&prepared);

    if let Err(cause) = sync_transaction_directories(&prepared) {
        return Err(rollback_failure(
            "sync",
            cause.to_string(),
            &root,
            &edit,
            journal,
            &prepared,
        ));
    }
    let files = match verify_final_states(&prepared) {
        Ok(files) => files,
        Err(cause) => {
            return Err(rollback_failure(
                "verify",
                cause.to_string(),
                &root,
                &edit,
                journal,
                &prepared,
            ))
        }
    };
    let journal_entries = journal.len();
    let mut cleanup_warnings = stage_cleanup_warnings
        .into_iter()
        .map(|artifact| format!("could not remove transaction stage {artifact}"))
        .collect::<Vec<_>>();
    cleanup_warnings.extend(cleanup_backups(journal));
    if let Err(cause) = sync_transaction_directories(&prepared) {
        cleanup_warnings.push(format!(
            "could not sync transaction directories after backup cleanup: {cause}"
        ));
    }
    Ok(WorkspaceEditApplyResult {
        edit_id: edit.edit_id,
        project_id: edit.project_id,
        root_identity: edit.root.identity,
        committed: true,
        journal_entries,
        files,
        cleanup_warnings,
    })
}

fn validate_edit_binding(edit: &WorkspaceEdit) -> Result<PathBuf, Box<WorkspaceEditApplyFailure>> {
    let rebuilt = WorkspaceEdit::define(
        edit.edit_id.clone(),
        edit.project_id.clone(),
        &edit.root.canonical_path,
        edit.operations.clone(),
    )
    .map_err(|cause| {
        Box::new(WorkspaceEditApplyFailure {
            stage: "preflight".into(),
            message: cause.message,
            stale_paths: Vec::new(),
            rollback_complete: true,
            authoritative_states: Vec::new(),
            recovery_artifacts: Vec::new(),
        })
    })?;
    if rebuilt != *edit {
        return Err(Box::new(WorkspaceEditApplyFailure {
            stage: "preflight".into(),
            message: "workspace edit root or normalized schema was modified after validation"
                .into(),
            stale_paths: Vec::new(),
            rollback_complete: true,
            authoritative_states: Vec::new(),
            recovery_artifacts: Vec::new(),
        }));
    }
    Ok(rebuilt.root.canonical_path)
}

enum PreparedOperation {
    Create {
        path: String,
        target: PathBuf,
        content: Vec<u8>,
        mode: String,
        stage: Option<PathBuf>,
    },
    Update {
        path: String,
        target: PathBuf,
        base: ContentHash,
        content: Vec<u8>,
        mode: String,
        permissions: Permissions,
        stage: Option<PathBuf>,
    },
    Delete {
        path: String,
        target: PathBuf,
        base: ContentHash,
    },
    Rename {
        from_path: String,
        to_path: String,
        source: PathBuf,
        target: PathBuf,
        base: ContentHash,
    },
}

impl PreparedOperation {
    fn paths(&self) -> Vec<&Path> {
        match self {
            Self::Create { target, .. }
            | Self::Update { target, .. }
            | Self::Delete { target, .. } => vec![target],
            Self::Rename { source, target, .. } => vec![source, target],
        }
    }
}

fn prepare_operations(
    root: &Path,
    edit: &WorkspaceEdit,
    proposed: &HashMap<String, String>,
    ignored_paths: &HashSet<String>,
) -> Result<Vec<PreparedOperation>, Box<WorkspaceEditApplyFailure>> {
    let mut prepared = Vec::with_capacity(edit.operations.len());
    let mut retained_base_bytes = 0_usize;
    let mut stale_paths = Vec::new();
    for operation in &edit.operations {
        let result = match operation {
            WorkspaceEditOperation::Create { path, content } => {
                validate_write_policy(path, ignored_paths).and_then(|()| {
                    let target = resolve_new_target(root, path)?;
                    let body = proposed
                        .get(&content.reference)
                        .ok_or_else(|| io_error("missing proposed content"))?;
                    Ok(PreparedOperation::Create {
                        path: path.clone(),
                        target,
                        content: body.as_bytes().to_vec(),
                        mode: content.format.mode.clone(),
                        stage: None,
                    })
                })
            }
            WorkspaceEditOperation::Update {
                path,
                base,
                content,
            } => validate_write_policy(path, ignored_paths).and_then(|()| {
                let (target, bytes, permissions) = read_existing(root, path)?;
                retained_base_bytes = retained_base_bytes.saturating_add(bytes.len());
                if ContentHash::for_bytes(&bytes) != *base {
                    stale_paths.push(path.clone());
                }
                let body = proposed
                    .get(&content.reference)
                    .ok_or_else(|| io_error("missing proposed content"))?;
                Ok(PreparedOperation::Update {
                    path: path.clone(),
                    target,
                    base: base.clone(),
                    content: body.as_bytes().to_vec(),
                    mode: content.format.mode.clone(),
                    permissions,
                    stage: None,
                })
            }),
            WorkspaceEditOperation::Delete { path, base } => {
                validate_write_policy(path, ignored_paths).and_then(|()| {
                    let (target, bytes, _) = read_existing(root, path)?;
                    retained_base_bytes = retained_base_bytes.saturating_add(bytes.len());
                    if ContentHash::for_bytes(&bytes) != *base {
                        stale_paths.push(path.clone());
                    }
                    Ok(PreparedOperation::Delete {
                        path: path.clone(),
                        target,
                        base: base.clone(),
                    })
                })
            }
            WorkspaceEditOperation::Rename {
                from_path,
                to_path,
                base,
            } => validate_write_policy(from_path, ignored_paths).and_then(|()| {
                validate_write_policy(to_path, ignored_paths)?;
                let (source, bytes, _) = read_existing(root, from_path)?;
                retained_base_bytes = retained_base_bytes.saturating_add(bytes.len());
                if ContentHash::for_bytes(&bytes) != *base {
                    stale_paths.push(from_path.clone());
                }
                let target = resolve_new_target(root, to_path)?;
                Ok(PreparedOperation::Rename {
                    from_path: from_path.clone(),
                    to_path: to_path.clone(),
                    source,
                    target,
                    base: base.clone(),
                })
            }),
        };
        match result {
            Ok(operation) => prepared.push(operation),
            Err(cause) => {
                return Err(Box::new(failure(
                    "preflight",
                    cause.to_string(),
                    stale_paths,
                    true,
                    root,
                    edit,
                    Vec::new(),
                )))
            }
        }
        if retained_base_bytes > MAX_JOURNAL_BYTES {
            return Err(Box::new(failure(
                "preflight",
                "workspace edit rollback journal exceeds memory limit",
                stale_paths,
                true,
                root,
                edit,
                Vec::new(),
            )));
        }
    }
    if !stale_paths.is_empty() {
        return Err(Box::new(failure(
            "preflight",
            "workspace edit base SHA-256 is stale",
            stale_paths.clone(),
            true,
            root,
            edit,
            Vec::new(),
        )));
    }
    Ok(prepared)
}

fn validate_write_policy(path: &str, ignored_paths: &HashSet<String>) -> io::Result<()> {
    if is_sensitive_path(Path::new(path)) {
        return Err(io_error(
            "workspace edit path is denied by sensitive policy",
        ));
    }
    if ignored_paths.contains(path) {
        return Err(io_error("workspace edit path is ignored by project policy"));
    }
    Ok(())
}

fn read_existing(root: &Path, relative: &str) -> io::Result<(PathBuf, Vec<u8>, Permissions)> {
    let target = resolve_components(root, relative, false)?;
    let metadata = fs::metadata(&target)?;
    if !metadata.is_file() {
        return Err(io_error("workspace edit source is not a regular file"));
    }
    if metadata.permissions().readonly() {
        return Err(io_error("workspace edit source is read-only"));
    }
    if metadata.len() > MAX_BASE_FILE_BYTES {
        return Err(io_error(
            "workspace edit base exceeds text transaction limit",
        ));
    }
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    File::open(&target)?
        .take(MAX_BASE_FILE_BYTES + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() as u64 > MAX_BASE_FILE_BYTES {
        return Err(io_error(
            "workspace edit base exceeds text transaction limit",
        ));
    }
    if bytes.contains(&0) || std::str::from_utf8(strip_utf8_bom(&bytes)).is_err() {
        return Err(io_error("workspace edit base must be UTF-8 text"));
    }
    if inspect_text_format(&bytes, "preserve").newline == "mixed" {
        return Err(io_error(
            "workspace edit base has mixed or unsupported line endings",
        ));
    }
    Ok((target, bytes, metadata.permissions()))
}

fn resolve_new_target(root: &Path, relative: &str) -> io::Result<PathBuf> {
    let target = resolve_components(root, relative, true)?;
    match fs::symlink_metadata(&target) {
        Ok(_) => Err(io::Error::new(
            io::ErrorKind::AlreadyExists,
            "workspace edit target already exists",
        )),
        Err(cause) if cause.kind() == io::ErrorKind::NotFound => Ok(target),
        Err(cause) => Err(cause),
    }
}

fn resolve_components(
    root: &Path,
    relative: &str,
    allow_missing_final: bool,
) -> io::Result<PathBuf> {
    let canonical_root = root.canonicalize()?;
    if canonical_root != root {
        return Err(io_error("workspace edit root changed after validation"));
    }
    let components = Path::new(relative).components().collect::<Vec<_>>();
    if components.is_empty() {
        return Err(io_error("workspace edit path is empty"));
    }
    let mut candidate = canonical_root;
    for (index, component) in components.iter().enumerate() {
        let Component::Normal(name) = component else {
            return Err(io_error("workspace edit path is not normalized"));
        };
        candidate.push(name);
        let is_final = index + 1 == components.len();
        match fs::symlink_metadata(&candidate) {
            Ok(metadata) if metadata.file_type().is_symlink() => {
                return Err(io_error("workspace edit path traverses a symbolic link"))
            }
            Ok(metadata) if !is_final && !metadata.is_dir() => {
                return Err(io_error("workspace edit parent is not a directory"))
            }
            Ok(_) => {}
            Err(cause)
                if allow_missing_final && is_final && cause.kind() == io::ErrorKind::NotFound => {}
            Err(cause) => return Err(cause),
        }
    }
    Ok(candidate)
}

fn stage_new_content<F: FaultInjector>(
    prepared: &mut [PreparedOperation],
    fault: &F,
) -> io::Result<()> {
    for operation in prepared {
        let (target, content, permissions, mode, stage_slot) = match operation {
            PreparedOperation::Create {
                target,
                content,
                mode,
                stage,
                ..
            } => (target, content, None, mode.as_str(), stage),
            PreparedOperation::Update {
                target,
                content,
                mode,
                permissions,
                stage,
                ..
            } => (
                target,
                content,
                Some(permissions.clone()),
                mode.as_str(),
                stage,
            ),
            PreparedOperation::Delete { .. } | PreparedOperation::Rename { .. } => continue,
        };
        let stage_path = sibling_temp_path(target, "stage");
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&stage_path)?;
        *stage_slot = Some(stage_path.clone());
        fault.write_stage(&mut file, content)?;
        file.sync_all()?;
        if let Some(permissions) = permissions_for_mode(permissions, mode)? {
            fs::set_permissions(&stage_path, permissions)?;
            file.sync_all()?;
        }
    }
    Ok(())
}

fn permissions_for_mode(
    original: Option<Permissions>,
    mode: &str,
) -> io::Result<Option<Permissions>> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;

        let bits = original.as_ref().map(PermissionsExt::mode).unwrap_or(0o644);
        let resolved = match mode {
            "preserve" => original.unwrap_or_else(|| Permissions::from_mode(0o644)),
            "regular" => Permissions::from_mode(bits & !0o111),
            "executable" => Permissions::from_mode(bits | 0o111),
            _ => return Err(io_error("workspace edit file mode policy is unsupported")),
        };
        Ok(Some(resolved))
    }
    #[cfg(not(unix))]
    {
        if mode == "executable" {
            return Err(io_error(
                "workspace edit executable mode requires POSIX file-mode support on this platform",
            ));
        }
        if !matches!(mode, "preserve" | "regular") {
            return Err(io_error("workspace edit file mode policy is unsupported"));
        }
        Ok(original)
    }
}

fn revalidate_root(root: &Path) -> io::Result<()> {
    if root.canonicalize()? != root {
        return Err(io_error("workspace edit root changed before commit"));
    }
    Ok(())
}

struct RevalidateError {
    message: String,
    stale_paths: Vec<String>,
}

fn revalidate_prepared(prepared: &[PreparedOperation]) -> Result<(), RevalidateError> {
    let mut stale_paths = Vec::new();
    for operation in prepared {
        let result = match operation {
            PreparedOperation::Create { path, target, .. } => ensure_absent(target, path),
            PreparedOperation::Update {
                path, target, base, ..
            }
            | PreparedOperation::Delete {
                path, target, base, ..
            } => verify_existing_hash(target, path, base),
            PreparedOperation::Rename {
                from_path,
                to_path,
                source,
                target,
                base,
            } => verify_existing_hash(source, from_path, base)
                .and_then(|()| ensure_absent(target, to_path)),
        };
        if let Err(path) = result {
            stale_paths.push(path);
        }
    }
    if stale_paths.is_empty() {
        Ok(())
    } else {
        Err(RevalidateError {
            message: "workspace changed while edit content was staged".into(),
            stale_paths,
        })
    }
}

fn ensure_absent(path: &Path, label: &str) -> Result<(), String> {
    match fs::symlink_metadata(path) {
        Err(cause) if cause.kind() == io::ErrorKind::NotFound => Ok(()),
        _ => Err(label.into()),
    }
}

fn verify_existing_hash(path: &Path, label: &str, expected: &ContentHash) -> Result<(), String> {
    match read_raw_bounded(path) {
        Ok(bytes) if ContentHash::for_bytes(&bytes) == *expected => Ok(()),
        _ => Err(label.into()),
    }
}

enum UndoEntry {
    Created {
        target: PathBuf,
        installed_hash: ContentHash,
    },
    Backup {
        target: PathBuf,
        backup: PathBuf,
        original_hash: ContentHash,
        installed_hash: Option<ContentHash>,
    },
    Renamed {
        source: PathBuf,
        target: PathBuf,
        original_hash: ContentHash,
    },
}

fn commit_operation(
    operation: &mut PreparedOperation,
    journal: &mut Vec<UndoEntry>,
) -> io::Result<()> {
    match operation {
        PreparedOperation::Create {
            path,
            target,
            content,
            stage,
            ..
        } => {
            ensure_absent(target, path).map_err(|_| io_error("create target became stale"))?;
            let stage_path = stage
                .as_ref()
                .ok_or_else(|| io_error("create stage is missing"))?
                .clone();
            let installed_hash = ContentHash::for_bytes(content);
            fs::hard_link(&stage_path, &*target)?;
            journal.push(UndoEntry::Created {
                target: target.clone(),
                installed_hash,
            });
            if fs::remove_file(&stage_path).is_ok() {
                *stage = None;
            }
        }
        PreparedOperation::Update {
            path,
            target,
            base,
            content,
            stage,
            ..
        } => {
            verify_existing_hash(target, path, base)
                .map_err(|_| io_error("update base became stale"))?;
            let stage_path = stage
                .as_ref()
                .ok_or_else(|| io_error("update stage is missing"))?
                .clone();
            let backup = create_backup_link(target)?;
            journal.push(UndoEntry::Backup {
                target: target.clone(),
                backup,
                original_hash: base.clone(),
                installed_hash: None,
            });
            fs::remove_file(&*target)?;
            fs::hard_link(&stage_path, &*target)?;
            let installed_hash = ContentHash::for_bytes(content);
            if let Some(UndoEntry::Backup {
                installed_hash: journal_hash,
                ..
            }) = journal.last_mut()
            {
                *journal_hash = Some(installed_hash);
            }
            if fs::remove_file(&stage_path).is_ok() {
                *stage = None;
            }
        }
        PreparedOperation::Delete {
            path, target, base, ..
        } => {
            verify_existing_hash(target, path, base)
                .map_err(|_| io_error("delete base became stale"))?;
            let backup = create_backup_link(target)?;
            journal.push(UndoEntry::Backup {
                target: target.clone(),
                backup,
                original_hash: base.clone(),
                installed_hash: None,
            });
            fs::remove_file(&*target)?;
        }
        PreparedOperation::Rename {
            from_path,
            to_path,
            source,
            target,
            base,
        } => {
            verify_existing_hash(source, from_path, base)
                .map_err(|_| io_error("rename source became stale"))?;
            ensure_absent(target, to_path).map_err(|_| io_error("rename target became stale"))?;
            fs::hard_link(&*source, &*target)?;
            journal.push(UndoEntry::Renamed {
                source: source.clone(),
                target: target.clone(),
                original_hash: base.clone(),
            });
            fs::remove_file(&*source)?;
        }
    }
    Ok(())
}

fn create_backup_link(target: &Path) -> io::Result<PathBuf> {
    for _ in 0..64 {
        let backup = sibling_temp_path(target, "backup");
        match fs::hard_link(target, &backup) {
            Ok(()) => return Ok(backup),
            Err(cause) if cause.kind() == io::ErrorKind::AlreadyExists => continue,
            Err(cause) => return Err(cause),
        }
    }
    Err(io_error("could not allocate a unique rollback backup"))
}

fn rollback_failure(
    stage: &str,
    message: String,
    root: &Path,
    edit: &WorkspaceEdit,
    journal: Vec<UndoEntry>,
    prepared: &[PreparedOperation],
) -> Box<WorkspaceEditApplyFailure> {
    let (mut rollback_complete, mut recovery_artifacts) = rollback(journal);
    recovery_artifacts.extend(cleanup_stages(prepared));
    recovery_artifacts.sort();
    recovery_artifacts.dedup();
    if sync_transaction_directories(prepared).is_err() || !recovery_artifacts.is_empty() {
        rollback_complete = false;
    }
    Box::new(failure(
        stage,
        message,
        Vec::new(),
        rollback_complete,
        root,
        edit,
        recovery_artifacts,
    ))
}

fn rollback(mut journal: Vec<UndoEntry>) -> (bool, Vec<String>) {
    let mut complete = true;
    let mut recovery_artifacts = Vec::new();
    while let Some(entry) = journal.pop() {
        let recovery_backup = match &entry {
            UndoEntry::Backup { backup, .. } => Some(backup.clone()),
            UndoEntry::Created { .. } | UndoEntry::Renamed { .. } => None,
        };
        let result = match entry {
            UndoEntry::Created {
                target,
                installed_hash,
            } => rollback_created(&target, &installed_hash),
            UndoEntry::Backup {
                target,
                backup,
                original_hash,
                installed_hash,
            } => rollback_backup(&target, &backup, &original_hash, installed_hash.as_ref()),
            UndoEntry::Renamed {
                source,
                target,
                original_hash,
            } => rollback_rename(&source, &target, &original_hash),
        };
        if result.is_err() {
            complete = false;
        }
        if let Some(backup) = recovery_backup {
            if backup.exists() {
                recovery_artifacts.push(recovery_artifact_name(&backup));
            }
        }
    }
    (complete, recovery_artifacts)
}

fn rollback_created(target: &Path, installed_hash: &ContentHash) -> io::Result<()> {
    match current_hash(target)? {
        None => Ok(()),
        Some(hash) if hash == *installed_hash => fs::remove_file(target),
        Some(_) => Err(io_error(
            "created target changed after commit; rollback preserved it",
        )),
    }
}

fn rollback_backup(
    target: &Path,
    backup: &Path,
    original_hash: &ContentHash,
    installed_hash: Option<&ContentHash>,
) -> io::Result<()> {
    let backup_hash =
        current_hash(backup)?.ok_or_else(|| io_error("rollback backup is missing"))?;
    if backup_hash != *original_hash {
        return Err(io_error("rollback backup changed after creation"));
    }
    match current_hash(target)? {
        None => restore_backup_no_clobber(backup, target),
        Some(hash) if hash == *original_hash => fs::remove_file(backup),
        Some(hash) if installed_hash == Some(&hash) => {
            fs::remove_file(target)?;
            restore_backup_no_clobber(backup, target)
        }
        Some(_) => Err(io_error(
            "target changed after commit; rollback preserved it and its backup",
        )),
    }
}

fn restore_backup_no_clobber(backup: &Path, target: &Path) -> io::Result<()> {
    fs::hard_link(backup, target)?;
    fs::remove_file(backup)
}

fn rollback_rename(source: &Path, target: &Path, original_hash: &ContentHash) -> io::Result<()> {
    let source_hash = current_hash(source)?;
    let target_hash = current_hash(target)?;
    match (source_hash, target_hash) {
        (Some(source_hash), Some(target_hash))
            if source_hash == *original_hash && target_hash == *original_hash =>
        {
            fs::remove_file(target)
        }
        (Some(source_hash), None) if source_hash == *original_hash => Ok(()),
        (None, Some(target_hash)) if target_hash == *original_hash => {
            fs::hard_link(target, source)?;
            fs::remove_file(target)
        }
        _ => Err(io_error(
            "rename paths changed after commit; rollback preserved authoritative files",
        )),
    }
}

fn current_hash(path: &Path) -> io::Result<Option<ContentHash>> {
    match fs::symlink_metadata(path) {
        Err(cause) if cause.kind() == io::ErrorKind::NotFound => Ok(None),
        Ok(metadata) if metadata.is_file() && !metadata.file_type().is_symlink() => {
            Ok(Some(ContentHash::for_bytes(&read_raw_bounded(path)?)))
        }
        Ok(_) => Err(io_error("transaction path is not a regular file")),
        Err(cause) => Err(cause),
    }
}

fn cleanup_backups(journal: Vec<UndoEntry>) -> Vec<String> {
    let mut warnings = Vec::new();
    for entry in journal {
        if let UndoEntry::Backup { backup, .. } = entry {
            if let Err(cause) = fs::remove_file(&backup) {
                warnings.push(format!(
                    "could not remove rollback backup {}: {cause}",
                    recovery_artifact_name(&backup)
                ));
            }
        }
    }
    warnings
}

fn cleanup_stages(prepared: &[PreparedOperation]) -> Vec<String> {
    let mut recovery_artifacts = Vec::new();
    for operation in prepared {
        let stage = match operation {
            PreparedOperation::Create { stage, .. } | PreparedOperation::Update { stage, .. } => {
                stage.as_deref()
            }
            PreparedOperation::Delete { .. } | PreparedOperation::Rename { .. } => None,
        };
        if let Some(stage) = stage {
            if let Err(cause) = fs::remove_file(stage) {
                if cause.kind() != io::ErrorKind::NotFound {
                    recovery_artifacts.push(recovery_artifact_name(stage));
                }
            }
        }
    }
    recovery_artifacts
}

fn recovery_artifact_name(path: &Path) -> String {
    path.file_name()
        .unwrap_or_default()
        .to_string_lossy()
        .into_owned()
}

fn verify_final_states(prepared: &[PreparedOperation]) -> io::Result<Vec<AppliedFile>> {
    let mut files = Vec::with_capacity(prepared.len());
    for operation in prepared {
        match operation {
            PreparedOperation::Create {
                path,
                target,
                content,
                mode,
                ..
            } => {
                let expected = ContentHash::for_bytes(content);
                let actual = ContentHash::for_bytes(&read_raw_bounded(target)?);
                if actual != expected {
                    return Err(io_error(
                        "created file final SHA-256 does not match proposal",
                    ));
                }
                let final_mode = file_mode(target)?;
                verify_declared_mode(mode, &final_mode)?;
                files.push(AppliedFile {
                    kind: "create".into(),
                    path: path.clone(),
                    from_path: None,
                    base_hash: None,
                    final_hash: Some(actual),
                    final_mode: Some(final_mode),
                });
            }
            PreparedOperation::Update {
                path,
                target,
                base,
                content,
                mode,
                ..
            } => {
                let expected = ContentHash::for_bytes(content);
                let actual = ContentHash::for_bytes(&read_raw_bounded(target)?);
                if actual != expected {
                    return Err(io_error(
                        "updated file final SHA-256 does not match proposal",
                    ));
                }
                let final_mode = file_mode(target)?;
                verify_declared_mode(mode, &final_mode)?;
                files.push(AppliedFile {
                    kind: "update".into(),
                    path: path.clone(),
                    from_path: None,
                    base_hash: Some(base.clone()),
                    final_hash: Some(actual),
                    final_mode: Some(final_mode),
                });
            }
            PreparedOperation::Delete { path, target, base } => {
                if fs::symlink_metadata(target).is_ok() {
                    return Err(io_error("deleted workspace edit target still exists"));
                }
                files.push(AppliedFile {
                    kind: "delete".into(),
                    path: path.clone(),
                    from_path: None,
                    base_hash: Some(base.clone()),
                    final_hash: None,
                    final_mode: None,
                });
            }
            PreparedOperation::Rename {
                from_path,
                to_path,
                source,
                target,
                base,
            } => {
                if fs::symlink_metadata(source).is_ok() {
                    return Err(io_error("renamed workspace edit source still exists"));
                }
                let actual = ContentHash::for_bytes(&read_raw_bounded(target)?);
                if actual != *base {
                    return Err(io_error("renamed file final SHA-256 does not match base"));
                }
                files.push(AppliedFile {
                    kind: "rename".into(),
                    path: to_path.clone(),
                    from_path: Some(from_path.clone()),
                    base_hash: Some(base.clone()),
                    final_hash: Some(actual),
                    final_mode: Some(file_mode(target)?),
                });
            }
        }
    }
    Ok(files)
}

fn verify_declared_mode(declared: &str, actual: &str) -> io::Result<()> {
    match declared {
        "preserve" => Ok(()),
        "regular" if actual == "100644" => Ok(()),
        "executable" if actual == "100755" => Ok(()),
        "regular" | "executable" => Err(io_error(
            "workspace edit final file mode does not match declared policy",
        )),
        _ => Err(io_error("workspace edit file mode policy is unsupported")),
    }
}

fn file_mode(path: &Path) -> io::Result<String> {
    let metadata = fs::metadata(path)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        Ok(if metadata.permissions().mode() & 0o111 != 0 {
            "100755".into()
        } else {
            "100644".into()
        })
    }
    #[cfg(not(unix))]
    {
        let _ = metadata;
        Ok("100644".into())
    }
}

fn authoritative_states(root: &Path, edit: &WorkspaceEdit) -> Vec<AuthoritativeFileState> {
    let mut paths = Vec::new();
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
    paths.sort();
    paths.dedup();
    paths
        .into_iter()
        .map(|path| {
            let absolute = root.join(&path);
            match fs::symlink_metadata(&absolute) {
                Err(cause) if cause.kind() == io::ErrorKind::NotFound => AuthoritativeFileState {
                    path,
                    exists: false,
                    hash: None,
                    error: None,
                },
                Ok(metadata) if metadata.is_file() && !metadata.file_type().is_symlink() => {
                    match read_raw_bounded(&absolute) {
                        Ok(bytes) => AuthoritativeFileState {
                            path,
                            exists: true,
                            hash: Some(ContentHash::for_bytes(&bytes)),
                            error: None,
                        },
                        Err(cause) => AuthoritativeFileState {
                            path,
                            exists: true,
                            hash: None,
                            error: Some(cause.to_string()),
                        },
                    }
                }
                Ok(_) => AuthoritativeFileState {
                    path,
                    exists: true,
                    hash: None,
                    error: Some("path is not a regular file".into()),
                },
                Err(cause) => AuthoritativeFileState {
                    path,
                    exists: false,
                    hash: None,
                    error: Some(cause.to_string()),
                },
            }
        })
        .collect()
}

fn failure(
    stage: &str,
    message: impl Into<String>,
    stale_paths: Vec<String>,
    rollback_complete: bool,
    root: &Path,
    edit: &WorkspaceEdit,
    recovery_artifacts: Vec<String>,
) -> WorkspaceEditApplyFailure {
    WorkspaceEditApplyFailure {
        stage: stage.into(),
        message: message.into(),
        stale_paths,
        rollback_complete,
        authoritative_states: authoritative_states(root, edit),
        recovery_artifacts,
    }
}

fn sync_transaction_directories(prepared: &[PreparedOperation]) -> io::Result<()> {
    #[cfg(not(unix))]
    let _ = prepared;
    #[cfg(unix)]
    {
        let mut directories = HashSet::new();
        for operation in prepared {
            for path in operation.paths() {
                if let Some(parent) = path.parent() {
                    directories.insert(parent.to_path_buf());
                }
            }
        }
        for directory in directories {
            File::open(directory)?.sync_all()?;
        }
    }
    Ok(())
}

fn sibling_temp_path(target: &Path, kind: &str) -> PathBuf {
    let parent = target.parent().unwrap_or_else(|| Path::new("."));
    let name = target
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or("file");
    loop {
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let candidate = parent.join(format!(
            ".{name}.aegisy-edit-{}-{sequence}.{kind}",
            std::process::id()
        ));
        if !candidate.exists() {
            return candidate;
        }
    }
}

fn read_raw_bounded(path: &Path) -> io::Result<Vec<u8>> {
    let metadata = fs::metadata(path)?;
    if !metadata.is_file() || metadata.len() > MAX_BASE_FILE_BYTES {
        return Err(io_error("workspace edit file is unavailable or too large"));
    }
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    File::open(path)?
        .take(MAX_BASE_FILE_BYTES + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() as u64 > MAX_BASE_FILE_BYTES {
        return Err(io_error("workspace edit file exceeds transaction limit"));
    }
    Ok(bytes)
}

fn strip_utf8_bom(bytes: &[u8]) -> &[u8] {
    bytes.strip_prefix(&[0xef, 0xbb, 0xbf]).unwrap_or(bytes)
}

fn io_error(message: impl Into<String>) -> io::Error {
    io::Error::other(message.into())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::workspace_edit::ProposedContent;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::{SystemTime, UNIX_EPOCH};

    static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

    fn root() -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let sequence = FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!("aegisy-edit-apply-{nonce}-{sequence}"));
        fs::create_dir_all(root.join("src")).unwrap();
        root.canonicalize().unwrap()
    }

    fn content_input(content: &ProposedContent, body: &str) -> ContentInput {
        ContentInput {
            reference: content.reference.clone(),
            content: body.into(),
        }
    }

    fn assert_no_transaction_files(root: &Path) {
        let entries = fs::read_dir(root.join("src")).unwrap();
        assert!(entries.filter_map(Result::ok).all(|entry| {
            !entry
                .file_name()
                .to_string_lossy()
                .contains(".aegisy-edit-")
        }));
    }

    #[test]
    fn applies_all_operation_kinds_and_returns_final_hashes() {
        let root = root();
        fs::write(root.join("src/update.rs"), "before\n").unwrap();
        fs::write(root.join("src/delete.rs"), "delete\n").unwrap();
        fs::write(root.join("src/old.rs"), "rename\n").unwrap();
        let created = ProposedContent::for_bytes(b"created\n");
        let updated = ProposedContent::for_bytes(b"after\n");
        let edit = WorkspaceEdit::define(
            "apply-all",
            "project-1",
            &root,
            vec![
                WorkspaceEditOperation::Create {
                    path: "src/create.rs".into(),
                    content: created.clone(),
                },
                WorkspaceEditOperation::Update {
                    path: "src/update.rs".into(),
                    base: ContentHash::for_bytes(b"before\n"),
                    content: updated.clone(),
                },
                WorkspaceEditOperation::Delete {
                    path: "src/delete.rs".into(),
                    base: ContentHash::for_bytes(b"delete\n"),
                },
                WorkspaceEditOperation::Rename {
                    from_path: "src/old.rs".into(),
                    to_path: "src/new.rs".into(),
                    base: ContentHash::for_bytes(b"rename\n"),
                },
            ],
        )
        .unwrap();
        let result = apply_workspace_edit(
            edit,
            vec![
                content_input(&created, "created\n"),
                content_input(&updated, "after\n"),
            ],
            &HashSet::new(),
        )
        .unwrap();
        assert!(result.committed);
        assert_eq!(result.files.len(), 4);
        assert_eq!(
            fs::read_to_string(root.join("src/create.rs")).unwrap(),
            "created\n"
        );
        assert_eq!(
            fs::read_to_string(root.join("src/update.rs")).unwrap(),
            "after\n"
        );
        assert!(!root.join("src/delete.rs").exists());
        assert!(!root.join("src/old.rs").exists());
        assert_eq!(
            fs::read_to_string(root.join("src/new.rs")).unwrap(),
            "rename\n"
        );
        assert!(result
            .files
            .iter()
            .filter(|file| file.kind != "delete")
            .all(|file| file.final_hash.is_some()));
        assert_no_transaction_files(&root);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_stale_base_before_any_path_changes() {
        let root = root();
        fs::write(root.join("src/update.rs"), "current\n").unwrap();
        let proposed = ProposedContent::for_bytes(b"after\n");
        let edit = WorkspaceEdit::define(
            "apply-stale",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Update {
                path: "src/update.rs".into(),
                base: ContentHash::for_bytes(b"old\n"),
                content: proposed.clone(),
            }],
        )
        .unwrap();
        let failure = apply_workspace_edit(
            edit,
            vec![content_input(&proposed, "after\n")],
            &HashSet::new(),
        )
        .unwrap_err();
        assert_eq!(failure.stage, "preflight");
        assert_eq!(failure.stale_paths, vec!["src/update.rs"]);
        assert!(failure.rollback_complete);
        assert_eq!(
            fs::read_to_string(root.join("src/update.rs")).unwrap(),
            "current\n"
        );
        assert_no_transaction_files(&root);
        fs::remove_dir_all(root).unwrap();
    }

    struct FailAfter(usize);

    impl FaultInjector for FailAfter {
        fn after_commit(&self, committed: usize) -> io::Result<()> {
            if committed == self.0 {
                Err(io_error("injected commit failure"))
            } else {
                Ok(())
            }
        }
    }

    struct PartialStageWrite;

    impl FaultInjector for PartialStageWrite {
        fn write_stage(&self, file: &mut File, content: &[u8]) -> io::Result<()> {
            let retained = content.len().min(3);
            file.write_all(&content[..retained])?;
            Err(io::Error::new(
                io::ErrorKind::WriteZero,
                "injected partial stage write",
            ))
        }
    }

    struct DiskFullStageWrite;

    impl FaultInjector for DiskFullStageWrite {
        fn write_stage(&self, file: &mut File, content: &[u8]) -> io::Result<()> {
            if let Some(first) = content.first() {
                file.write_all(std::slice::from_ref(first))?;
            }
            Err(io::Error::new(
                io::ErrorKind::StorageFull,
                "injected disk full",
            ))
        }
    }

    struct PermissionLossAfterCommit;

    impl FaultInjector for PermissionLossAfterCommit {
        fn after_commit(&self, committed: usize) -> io::Result<()> {
            if committed == 1 {
                Err(io::Error::new(
                    io::ErrorKind::PermissionDenied,
                    "injected permission loss",
                ))
            } else {
                Ok(())
            }
        }
    }

    struct ExitAfterCommit;

    impl FaultInjector for ExitAfterCommit {
        fn after_commit(&self, committed: usize) -> io::Result<()> {
            if committed == 1 {
                std::process::exit(86);
            }
            Ok(())
        }
    }

    struct ChangeAfterStage {
        path: PathBuf,
        content: &'static str,
    }

    impl FaultInjector for ChangeAfterStage {
        fn after_stage(&self) -> io::Result<()> {
            fs::write(&self.path, self.content)
        }
    }

    struct RewriteAndFail {
        path: PathBuf,
        content: &'static str,
    }

    impl FaultInjector for RewriteAndFail {
        fn after_commit(&self, committed: usize) -> io::Result<()> {
            if committed == 1 {
                fs::write(&self.path, self.content)?;
                Err(io_error("injected external rewrite"))
            } else {
                Ok(())
            }
        }
    }

    #[test]
    fn rejects_a_base_that_changes_after_staging_and_removes_stages() {
        let root = root();
        let target = root.join("src/update.rs");
        fs::write(&target, "before\n").unwrap();
        let proposed = ProposedContent::for_bytes(b"after\n");
        let edit = WorkspaceEdit::define(
            "apply-stage-stale",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Update {
                path: "src/update.rs".into(),
                base: ContentHash::for_bytes(b"before\n"),
                content: proposed.clone(),
            }],
        )
        .unwrap();
        let failure = apply_with_fault(
            edit,
            vec![content_input(&proposed, "after\n")],
            &HashSet::new(),
            &ChangeAfterStage {
                path: target.clone(),
                content: "external\n",
            },
        )
        .unwrap_err();
        assert_eq!(failure.stage, "revalidate");
        assert_eq!(failure.stale_paths, vec!["src/update.rs"]);
        assert!(failure.rollback_complete);
        assert_eq!(fs::read_to_string(target).unwrap(), "external\n");
        assert_no_transaction_files(&root);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn partial_write_and_disk_full_remove_incomplete_stages_without_mutation() {
        fn assert_stage_failure<F: FaultInjector>(fault: &F, edit_id: &str) {
            let root = root();
            let target = root.join("src/update.rs");
            fs::write(&target, "before\n").unwrap();
            let proposed = ProposedContent::for_bytes(b"after-content\n");
            let edit = WorkspaceEdit::define(
                edit_id,
                "project-1",
                &root,
                vec![WorkspaceEditOperation::Update {
                    path: "src/update.rs".into(),
                    base: ContentHash::for_bytes(b"before\n"),
                    content: proposed.clone(),
                }],
            )
            .unwrap();
            let failure = apply_with_fault(
                edit,
                vec![content_input(&proposed, "after-content\n")],
                &HashSet::new(),
                fault,
            )
            .unwrap_err();
            assert_eq!(failure.stage, "stage");
            assert!(failure.rollback_complete);
            assert_eq!(fs::read_to_string(target).unwrap(), "before\n");
            assert_no_transaction_files(&root);
            fs::remove_dir_all(root).unwrap();
        }

        assert_stage_failure(&PartialStageWrite, "apply-partial-write");
        assert_stage_failure(&DiskFullStageWrite, "apply-disk-full");
    }

    #[test]
    fn permission_loss_after_commit_rolls_back_content_and_permissions() {
        let root = root();
        let target = root.join("src/update.sh");
        fs::write(&target, "before\n").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&target, fs::Permissions::from_mode(0o755)).unwrap();
        }
        let original_permissions = fs::metadata(&target).unwrap().permissions();
        let proposed = ProposedContent::for_bytes(b"after\n").with_mode("regular");
        let edit = WorkspaceEdit::define(
            "apply-permission-loss",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Update {
                path: "src/update.sh".into(),
                base: ContentHash::for_bytes(b"before\n"),
                content: proposed.clone(),
            }],
        )
        .unwrap();
        let failure = apply_with_fault(
            edit,
            vec![content_input(&proposed, "after\n")],
            &HashSet::new(),
            &PermissionLossAfterCommit,
        )
        .unwrap_err();
        assert_eq!(failure.stage, "commit");
        assert!(failure.message.contains("permission loss"));
        assert!(failure.rollback_complete);
        assert_eq!(fs::read_to_string(&target).unwrap(), "before\n");
        assert_eq!(
            fs::metadata(&target).unwrap().permissions(),
            original_permissions
        );
        assert_no_transaction_files(&root);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn process_crash_leaves_authoritative_target_and_recoverable_backup_evidence() {
        let root = root();
        let target = root.join("src/update.rs");
        fs::write(&target, "before\n").unwrap();
        let output = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "workspace_edit_apply::tests::process_crash_failure_fixture",
                "--nocapture",
            ])
            .env("AEGISY_TEST_WORKSPACE_EDIT_CRASH_ROOT", &root)
            .output()
            .unwrap();
        assert_eq!(output.status.code(), Some(86));
        assert_eq!(fs::read_to_string(&target).unwrap(), "after\n");

        let artifacts = fs::read_dir(root.join("src"))
            .unwrap()
            .filter_map(Result::ok)
            .filter(|entry| {
                entry
                    .file_name()
                    .to_string_lossy()
                    .contains(".aegisy-edit-")
            })
            .collect::<Vec<_>>();
        assert!(!artifacts.is_empty());
        assert!(artifacts
            .iter()
            .any(|entry| fs::read(entry.path()).unwrap() == b"before\n"));

        let proposed = ProposedContent::for_bytes(b"after\n");
        let retry = WorkspaceEdit::define(
            "apply-crash-retry",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Update {
                path: "src/update.rs".into(),
                base: ContentHash::for_bytes(b"before\n"),
                content: proposed.clone(),
            }],
        )
        .unwrap();
        let failure = apply_workspace_edit(
            retry,
            vec![content_input(&proposed, "after\n")],
            &HashSet::new(),
        )
        .unwrap_err();
        assert_eq!(failure.stage, "preflight");
        assert_eq!(failure.stale_paths, vec!["src/update.rs"]);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn process_crash_failure_fixture() {
        let Some(root) = std::env::var_os("AEGISY_TEST_WORKSPACE_EDIT_CRASH_ROOT") else {
            return;
        };
        let root = PathBuf::from(root).canonicalize().unwrap();
        let proposed = ProposedContent::for_bytes(b"after\n");
        let edit = WorkspaceEdit::define(
            "apply-crash-child",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Update {
                path: "src/update.rs".into(),
                base: ContentHash::for_bytes(b"before\n"),
                content: proposed.clone(),
            }],
        )
        .unwrap();
        let _ = apply_with_fault(
            edit,
            vec![content_input(&proposed, "after\n")],
            &HashSet::new(),
            &ExitAfterCommit,
        );
        panic!("crash fixture did not terminate after commit");
    }

    #[test]
    fn rolls_back_a_partially_committed_multi_file_edit() {
        let root = root();
        fs::write(root.join("src/one.rs"), "one-before\n").unwrap();
        fs::write(root.join("src/two.rs"), "two-before\n").unwrap();
        fs::write(root.join("src/old.rs"), "rename-before\n").unwrap();
        let one = ProposedContent::for_bytes(b"one-after\n");
        let two = ProposedContent::for_bytes(b"two-after\n");
        let edit = WorkspaceEdit::define(
            "apply-rollback",
            "project-1",
            &root,
            vec![
                WorkspaceEditOperation::Update {
                    path: "src/one.rs".into(),
                    base: ContentHash::for_bytes(b"one-before\n"),
                    content: one.clone(),
                },
                WorkspaceEditOperation::Create {
                    path: "src/created.rs".into(),
                    content: two.clone(),
                },
                WorkspaceEditOperation::Rename {
                    from_path: "src/old.rs".into(),
                    to_path: "src/new.rs".into(),
                    base: ContentHash::for_bytes(b"rename-before\n"),
                },
            ],
        )
        .unwrap();
        let failure = apply_with_fault(
            edit,
            vec![
                content_input(&one, "one-after\n"),
                content_input(&two, "two-after\n"),
            ],
            &HashSet::new(),
            &FailAfter(2),
        )
        .unwrap_err();
        assert_eq!(failure.stage, "commit");
        assert!(failure.rollback_complete);
        assert_eq!(
            fs::read_to_string(root.join("src/one.rs")).unwrap(),
            "one-before\n"
        );
        assert!(!root.join("src/created.rs").exists());
        assert_eq!(
            fs::read_to_string(root.join("src/old.rs")).unwrap(),
            "rename-before\n"
        );
        assert!(!root.join("src/new.rs").exists());
        assert_no_transaction_files(&root);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rolls_back_create_update_delete_and_rename_after_all_committed() {
        let root = root();
        fs::write(root.join("src/update.rs"), "update-before\n").unwrap();
        fs::write(root.join("src/delete.rs"), "delete-before\n").unwrap();
        fs::write(root.join("src/old.rs"), "rename-before\n").unwrap();
        let created = ProposedContent::for_bytes(b"created\n");
        let updated = ProposedContent::for_bytes(b"update-after\n");
        let edit = WorkspaceEdit::define(
            "apply-rollback-all",
            "project-1",
            &root,
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
        let failure = apply_with_fault(
            edit,
            vec![
                content_input(&created, "created\n"),
                content_input(&updated, "update-after\n"),
            ],
            &HashSet::new(),
            &FailAfter(4),
        )
        .unwrap_err();
        assert!(failure.rollback_complete);
        assert!(!root.join("src/create.rs").exists());
        assert_eq!(
            fs::read_to_string(root.join("src/update.rs")).unwrap(),
            "update-before\n"
        );
        assert_eq!(
            fs::read_to_string(root.join("src/delete.rs")).unwrap(),
            "delete-before\n"
        );
        assert_eq!(
            fs::read_to_string(root.join("src/old.rs")).unwrap(),
            "rename-before\n"
        );
        assert!(!root.join("src/new.rs").exists());
        assert_no_transaction_files(&root);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rollback_preserves_an_external_rewrite_and_reports_the_backup() {
        let root = root();
        let target = root.join("src/update.rs");
        fs::write(&target, "before\n").unwrap();
        let proposed = ProposedContent::for_bytes(b"after\n");
        let edit = WorkspaceEdit::define(
            "apply-conservative-rollback",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Update {
                path: "src/update.rs".into(),
                base: ContentHash::for_bytes(b"before\n"),
                content: proposed.clone(),
            }],
        )
        .unwrap();
        let failure = apply_with_fault(
            edit,
            vec![content_input(&proposed, "after\n")],
            &HashSet::new(),
            &RewriteAndFail {
                path: target.clone(),
                content: "external\n",
            },
        )
        .unwrap_err();
        assert!(!failure.rollback_complete);
        assert_eq!(fs::read_to_string(&target).unwrap(), "external\n");
        assert_eq!(failure.recovery_artifacts.len(), 1);
        let backup = root.join("src").join(&failure.recovery_artifacts[0]);
        assert_eq!(fs::read_to_string(&backup).unwrap(), "before\n");
        let state = failure
            .authoritative_states
            .iter()
            .find(|state| state.path == "src/update.rs")
            .unwrap();
        assert_eq!(state.hash, Some(ContentHash::for_bytes(b"external\n")));
        fs::remove_file(backup).unwrap();
        assert_no_transaction_files(&root);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_sensitive_ignored_symlink_and_existing_targets_without_changes() {
        let root = root();
        fs::write(root.join("src/existing.rs"), "existing\n").unwrap();
        let content = ProposedContent::for_bytes(b"new\n");
        let sensitive = WorkspaceEdit::define(
            "apply-sensitive",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Create {
                path: ".env".into(),
                content: content.clone(),
            }],
        )
        .unwrap();
        assert!(apply_workspace_edit(
            sensitive,
            vec![content_input(&content, "new\n")],
            &HashSet::new()
        )
        .is_err());

        let existing = WorkspaceEdit::define(
            "apply-existing",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Create {
                path: "src/existing.rs".into(),
                content: content.clone(),
            }],
        )
        .unwrap();
        assert!(apply_workspace_edit(
            existing,
            vec![content_input(&content, "new\n")],
            &HashSet::new()
        )
        .is_err());
        assert_eq!(
            fs::read_to_string(root.join("src/existing.rs")).unwrap(),
            "existing\n"
        );

        let ignored = WorkspaceEdit::define(
            "apply-ignored",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Create {
                path: "src/ignored.rs".into(),
                content: content.clone(),
            }],
        )
        .unwrap();
        assert!(apply_workspace_edit(
            ignored,
            vec![content_input(&content, "new\n")],
            &HashSet::from(["src/ignored.rs".into()])
        )
        .is_err());
        assert!(!root.join("src/ignored.rs").exists());

        #[cfg(unix)]
        {
            use std::os::unix::fs::symlink;

            let outside = root.parent().unwrap().join(format!(
                "aegisy-edit-outside-{}",
                FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed)
            ));
            fs::write(&outside, "outside\n").unwrap();
            symlink(&outside, root.join("src/link.rs")).unwrap();
            let linked = WorkspaceEdit::define(
                "apply-symlink",
                "project-1",
                &root,
                vec![WorkspaceEditOperation::Update {
                    path: "src/link.rs".into(),
                    base: ContentHash::for_bytes(b"outside\n"),
                    content: content.clone(),
                }],
            )
            .unwrap();
            assert!(apply_workspace_edit(
                linked,
                vec![content_input(&content, "new\n")],
                &HashSet::new()
            )
            .is_err());
            assert_eq!(fs::read_to_string(&outside).unwrap(), "outside\n");
            fs::remove_file(root.join("src/link.rs")).unwrap();
            fs::remove_file(outside).unwrap();

            let outside_directory = root.parent().unwrap().join(format!(
                "aegisy-edit-outside-directory-{}",
                FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir(&outside_directory).unwrap();
            symlink(&outside_directory, root.join("src/linked-parent")).unwrap();
            let linked_parent = WorkspaceEdit::define(
                "apply-symlink-parent",
                "project-1",
                &root,
                vec![WorkspaceEditOperation::Create {
                    path: "src/linked-parent/new.rs".into(),
                    content: content.clone(),
                }],
            )
            .unwrap();
            assert!(apply_workspace_edit(
                linked_parent,
                vec![content_input(&content, "new\n")],
                &HashSet::new()
            )
            .is_err());
            assert!(!outside_directory.join("new.rs").exists());
            fs::remove_file(root.join("src/linked-parent")).unwrap();
            fs::remove_dir(outside_directory).unwrap();
        }
        assert!(!root.join(".env").exists());
        fs::remove_dir_all(root).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn preserves_declared_bom_crlf_and_applies_explicit_file_modes() {
        use std::os::unix::fs::PermissionsExt;

        let root = root();
        fs::write(root.join("src/update.sh"), "before\n").unwrap();
        fs::set_permissions(
            root.join("src/update.sh"),
            fs::Permissions::from_mode(0o755),
        )
        .unwrap();
        fs::write(root.join("src/promote.sh"), "regular\n").unwrap();
        fs::write(root.join("src/demote.sh"), "executable\n").unwrap();
        fs::set_permissions(
            root.join("src/demote.sh"),
            fs::Permissions::from_mode(0o755),
        )
        .unwrap();
        fs::write(root.join("src/old.sh"), "rename\n").unwrap();
        fs::set_permissions(root.join("src/old.sh"), fs::Permissions::from_mode(0o755)).unwrap();

        let created_bytes = b"\xef\xbb\xbfline\r\n";
        let created = ProposedContent::for_bytes(created_bytes).with_mode("executable");
        let updated = ProposedContent::for_bytes(b"after\n");
        let promoted = ProposedContent::for_bytes(b"promoted\n").with_mode("executable");
        let demoted = ProposedContent::for_bytes(b"demoted\n").with_mode("regular");
        let edit = WorkspaceEdit::define(
            "apply-format-mode",
            "project-1",
            &root,
            vec![
                WorkspaceEditOperation::Create {
                    path: "src/created.sh".into(),
                    content: created.clone(),
                },
                WorkspaceEditOperation::Update {
                    path: "src/update.sh".into(),
                    base: ContentHash::for_bytes(b"before\n"),
                    content: updated.clone(),
                },
                WorkspaceEditOperation::Update {
                    path: "src/promote.sh".into(),
                    base: ContentHash::for_bytes(b"regular\n"),
                    content: promoted.clone(),
                },
                WorkspaceEditOperation::Update {
                    path: "src/demote.sh".into(),
                    base: ContentHash::for_bytes(b"executable\n"),
                    content: demoted.clone(),
                },
                WorkspaceEditOperation::Rename {
                    from_path: "src/old.sh".into(),
                    to_path: "src/new.sh".into(),
                    base: ContentHash::for_bytes(b"rename\n"),
                },
            ],
        )
        .unwrap();
        let result = apply_workspace_edit(
            edit,
            vec![
                content_input(&created, std::str::from_utf8(created_bytes).unwrap()),
                content_input(&updated, "after\n"),
                content_input(&promoted, "promoted\n"),
                content_input(&demoted, "demoted\n"),
            ],
            &HashSet::new(),
        )
        .unwrap();
        assert_eq!(
            fs::read(root.join("src/created.sh")).unwrap(),
            created_bytes
        );
        for path in [
            "src/created.sh",
            "src/update.sh",
            "src/promote.sh",
            "src/new.sh",
        ] {
            assert_ne!(
                fs::metadata(root.join(path)).unwrap().permissions().mode() & 0o111,
                0,
                "{path} should be executable"
            );
        }
        assert_eq!(
            fs::metadata(root.join("src/demote.sh"))
                .unwrap()
                .permissions()
                .mode()
                & 0o111,
            0
        );
        assert_eq!(
            result
                .files
                .iter()
                .find(|file| file.path == "src/demote.sh")
                .and_then(|file| file.final_mode.as_deref()),
            Some("100644")
        );
        assert!(result
            .files
            .iter()
            .filter(|file| file.path != "src/demote.sh")
            .all(|file| file.final_mode.as_deref() == Some("100755")));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_binary_mixed_and_oversized_bases_before_mutation() {
        let root = root();
        for (name, bytes) in [
            ("binary.txt", b"binary\0body".to_vec()),
            ("mixed.txt", b"one\r\ntwo\n".to_vec()),
            ("large.txt", vec![b'x'; MAX_BASE_FILE_BYTES as usize + 1]),
        ] {
            fs::write(root.join("src").join(name), &bytes).unwrap();
            let proposed = ProposedContent::for_bytes(b"after\n");
            let edit = WorkspaceEdit::define(
                format!("reject-base-{name}"),
                "project-1",
                &root,
                vec![WorkspaceEditOperation::Update {
                    path: format!("src/{name}"),
                    base: ContentHash::for_bytes(&bytes),
                    content: proposed.clone(),
                }],
            )
            .unwrap();
            let failure = apply_workspace_edit(
                edit,
                vec![content_input(&proposed, "after\n")],
                &HashSet::new(),
            )
            .unwrap_err();
            assert_eq!(failure.stage, "preflight");
            assert_eq!(fs::read(root.join("src").join(name)).unwrap(), bytes);
        }
        assert_no_transaction_files(&root);
        fs::remove_dir_all(root).unwrap();
    }
}
