use crate::workspace::is_sensitive_path;
use crate::workspace_edit::{
    inspect_text_format, platform_supports_file_mode, ContentHash, WorkspaceEdit,
};
use crate::workspace_edit_apply::WorkspaceEditApplyResult;
use crate::workspace_edit_overlap::{
    planned_restore_overlap_baseline, proposal_overlap_baseline, restore_overlap_baseline,
    ExpectedWorkspacePathState,
};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::HashSet;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

const SCHEMA_VERSION: &str = "non-git-checkpoint/0.1";
const MAX_FILE_BYTES: u64 = 512 * 1024;
const MAX_TOTAL_BLOB_BYTES: u64 = 8 * 1024 * 1024;
const MAX_MANIFEST_BYTES: usize = 4 * 1024 * 1024;
const MAX_CHECKPOINTS: usize = 128;
const MAX_STORE_BYTES: u64 = 256 * 1024 * 1024;
const MAX_STORE_ENTRIES: usize = 4_096;
const MAX_CAPTURE_OBJECTS: usize = 257;
static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

const WEAKER_GUARANTEE: &str = "weaker-non-git-content-addressed";
const LIMITATIONS: [&str; 4] = [
    "does-not-anchor-head-or-index",
    "does-not-capture-unrelated-files",
    "does-not-capture-directory-metadata",
    "store-loss-removes-recovery",
];

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NonGitCheckpointError {
    pub message: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct NonGitCheckpointDescriptor {
    pub checkpoint_id: String,
    pub manifest_hash: ContentHash,
    pub manifest_reference: String,
    pub guarantee: String,
    pub limitations: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct NonGitCheckpointManifest {
    pub schema_version: String,
    pub checkpoint_id: String,
    pub edit_id: String,
    pub project_id: String,
    pub root_identity: String,
    pub storage_identity: String,
    pub guarantee: String,
    pub limitations: Vec<String>,
    pub pending_user_paths: Vec<String>,
    pub redacted_pending_count: usize,
    pub paths: Vec<NonGitCheckpointPath>,
    pub retained_blob_bytes: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct NonGitCheckpointPath {
    pub operation: String,
    pub role: String,
    pub path: String,
    pub before: NonGitCheckpointPathState,
    pub planned_after: NonGitCheckpointPathState,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub enum NonGitCheckpointPathState {
    Absent,
    File {
        hash: ContentHash,
        #[serde(skip_serializing_if = "Option::is_none")]
        blob_reference: Option<String>,
        mode: String,
    },
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct NonGitCheckpointApplication {
    pub checkpoint_id: String,
    pub edit_id: String,
    pub project_id: String,
    pub root_identity: String,
    pub guarantee: String,
    pub limitations: Vec<String>,
    pub paths: Vec<NonGitCheckpointPath>,
}

pub struct NonGitCheckpointStore {
    root: PathBuf,
    storage_identity: String,
}

impl NonGitCheckpointStore {
    pub fn open(storage_root: &Path, project_root: &Path) -> Result<Self, NonGitCheckpointError> {
        let storage_root = storage_root
            .canonicalize()
            .map_err(|_| error("non-Git checkpoint storage root is unavailable"))?;
        let project_root = project_root
            .canonicalize()
            .map_err(|_| error("non-Git checkpoint project root is unavailable"))?;
        if storage_root == project_root
            || storage_root.starts_with(&project_root)
            || project_root.starts_with(&storage_root)
        {
            return Err(error(
                "non-Git checkpoint storage must be disjoint from the project root",
            ));
        }
        if contains_git_metadata(&project_root) {
            return Err(error(
                "non-Git checkpoint fallback is unavailable for a Git worktree",
            ));
        }
        let root = storage_root.join("non-git-checkpoints-v1");
        create_secure_directory(&root)?;
        create_secure_directory(&root.join("blobs"))?;
        create_secure_directory(&root.join("manifests"))?;
        create_secure_directory(&root.join("checkpoints"))?;
        let canonical = root
            .canonicalize()
            .map_err(|_| error("non-Git checkpoint storage is unavailable"))?;
        if canonical != root {
            return Err(error("non-Git checkpoint storage traverses a symlink"));
        }
        let digest = Sha256::digest(canonical.to_string_lossy().as_bytes());
        Ok(Self {
            root: canonical,
            storage_identity: format!("non-git-checkpoint-store:sha256:{digest:x}"),
        })
    }

    pub fn capture(
        &self,
        checkpoint_id: &str,
        edit: &WorkspaceEdit,
        ignored_paths: &HashSet<String>,
        pending_user_paths: &HashSet<String>,
    ) -> Result<NonGitCheckpointDescriptor, NonGitCheckpointError> {
        validate_id(checkpoint_id)?;
        validate_edit(edit)?;
        if contains_git_metadata(&edit.root.canonical_path) {
            return Err(error("non-Git checkpoint project became a Git worktree"));
        }
        self.validate_layout()?;
        if edit.root.canonical_path.starts_with(&self.root)
            || self.root.starts_with(&edit.root.canonical_path)
        {
            return Err(error(
                "checkpoint store and project root are no longer disjoint",
            ));
        }
        let pointer = self.root.join("checkpoints").join(checkpoint_id);
        if fs::symlink_metadata(&pointer).is_ok() {
            return Err(error("non-Git checkpoint ID already exists"));
        }
        self.enforce_store_limits()?;
        let (pending_user_paths, redacted_pending_count) =
            validate_pending_paths(pending_user_paths)?;
        let before = proposal_overlap_baseline(edit).map_err(|cause| error(cause.message))?;
        let after = planned_restore_overlap_baseline(edit).map_err(|cause| error(cause.message))?;
        if before.paths.len() != after.paths.len() {
            return Err(error("non-Git checkpoint path baseline is inconsistent"));
        }
        if before
            .paths
            .iter()
            .any(|path| pending_user_paths.binary_search(&path.path).is_ok())
        {
            return Err(error(
                "non-Git checkpoint cannot capture a path with an unsaved user edit",
            ));
        }

        let mut retained_blob_bytes = 0_u64;
        let mut paths = Vec::with_capacity(before.paths.len());
        for (before_path, after_path) in before.paths.iter().zip(&after.paths) {
            if before_path.operation != after_path.operation
                || before_path.role != after_path.role
                || before_path.path != after_path.path
            {
                return Err(error(
                    "non-Git checkpoint operation baseline is inconsistent",
                ));
            }
            if is_sensitive_path(Path::new(&before_path.path))
                || ignored_paths.contains(&before_path.path)
            {
                return Err(error("non-Git checkpoint path is denied by policy"));
            }
            let before_state = self.capture_before_state(
                &edit.root.canonical_path,
                &before_path.path,
                &before_path.expected,
                &mut retained_blob_bytes,
            )?;
            let mode = if before_path.operation == "rename" && before_path.role == "target" {
                paths
                    .last()
                    .map(|path: &NonGitCheckpointPath| state_mode(&path.before))
                    .ok_or_else(|| error("non-Git rename source mode is missing"))?
            } else {
                resolve_planned_mode(
                    declared_mode(edit, &before_path.path),
                    &state_mode(&before_state),
                )?
            };
            let planned_after = expected_to_state(&after_path.expected, None, mode);
            paths.push(NonGitCheckpointPath {
                operation: before_path.operation.clone(),
                role: before_path.role.clone(),
                path: before_path.path.clone(),
                before: before_state,
                planned_after,
            });
        }
        let manifest = NonGitCheckpointManifest {
            schema_version: SCHEMA_VERSION.into(),
            checkpoint_id: checkpoint_id.into(),
            edit_id: edit.edit_id.clone(),
            project_id: edit.project_id.clone(),
            root_identity: edit.root.identity.clone(),
            storage_identity: self.storage_identity.clone(),
            guarantee: WEAKER_GUARANTEE.into(),
            limitations: LIMITATIONS.iter().map(|value| (*value).into()).collect(),
            pending_user_paths,
            redacted_pending_count,
            paths,
            retained_blob_bytes,
        };
        let bytes = serde_json::to_vec(&manifest)
            .map_err(|_| error("cannot serialize non-Git checkpoint manifest"))?;
        if bytes.len() > MAX_MANIFEST_BYTES {
            return Err(error("non-Git checkpoint manifest exceeds size limit"));
        }
        let manifest_hash = ContentHash::for_bytes(&bytes);
        self.write_object("manifests", &manifest_hash.sha256, &bytes)?;
        self.write_pointer(&pointer, &manifest_hash.sha256)?;
        Ok(NonGitCheckpointDescriptor {
            checkpoint_id: checkpoint_id.into(),
            manifest_reference: format!(
                "non-git-checkpoint-manifest:sha256:{}",
                manifest_hash.sha256
            ),
            manifest_hash,
            guarantee: manifest.guarantee,
            limitations: manifest.limitations,
        })
    }

    pub fn load(
        &self,
        descriptor: &NonGitCheckpointDescriptor,
    ) -> Result<NonGitCheckpointManifest, NonGitCheckpointError> {
        validate_id(&descriptor.checkpoint_id)?;
        self.validate_layout()?;
        if descriptor.guarantee != WEAKER_GUARANTEE
            || descriptor.limitations
                != LIMITATIONS
                    .iter()
                    .map(|value| (*value).to_owned())
                    .collect::<Vec<_>>()
            || descriptor.manifest_reference
                != format!(
                    "non-git-checkpoint-manifest:sha256:{}",
                    descriptor.manifest_hash.sha256
                )
        {
            return Err(error("non-Git checkpoint descriptor was modified"));
        }
        let pointer = self.read_pointer(&descriptor.checkpoint_id)?;
        if pointer != descriptor.manifest_hash.sha256 {
            return Err(error(
                "non-Git checkpoint pointer does not match descriptor",
            ));
        }
        let bytes = self.read_object("manifests", &pointer, MAX_MANIFEST_BYTES as u64)?;
        if ContentHash::for_bytes(&bytes) != descriptor.manifest_hash {
            return Err(error("non-Git checkpoint manifest hash mismatch"));
        }
        let manifest: NonGitCheckpointManifest = serde_json::from_slice(&bytes)
            .map_err(|_| error("non-Git checkpoint manifest is invalid"))?;
        self.validate_manifest(descriptor, &manifest)?;
        Ok(manifest)
    }

    pub fn bind_application(
        &self,
        descriptor: &NonGitCheckpointDescriptor,
        edit: &WorkspaceEdit,
        applied: &WorkspaceEditApplyResult,
    ) -> Result<NonGitCheckpointApplication, NonGitCheckpointError> {
        validate_edit(edit)?;
        let manifest = self.load(descriptor)?;
        if manifest.edit_id != edit.edit_id
            || manifest.project_id != edit.project_id
            || manifest.root_identity != edit.root.identity
        {
            return Err(error(
                "non-Git checkpoint belongs to another workspace edit",
            ));
        }
        let before = proposal_overlap_baseline(edit).map_err(|cause| error(cause.message))?;
        let after =
            restore_overlap_baseline(edit, applied).map_err(|cause| error(cause.message))?;
        if manifest.paths.len() != before.paths.len() || manifest.paths.len() != after.paths.len() {
            return Err(error("non-Git checkpoint path set is incomplete"));
        }
        for ((path, expected_before), expected_after) in
            manifest.paths.iter().zip(&before.paths).zip(&after.paths)
        {
            if path.operation != expected_before.operation
                || path.role != expected_before.role
                || path.path != expected_before.path
                || path.operation != expected_after.operation
                || path.role != expected_after.role
                || path.path != expected_after.path
                || !state_matches(&path.before, &expected_before.expected, true)
                || !state_matches(&path.planned_after, &expected_after.expected, false)
            {
                return Err(error("non-Git checkpoint path state is inconsistent"));
            }
        }
        validate_applied_modes(&manifest.paths, edit, applied)?;
        Ok(NonGitCheckpointApplication {
            checkpoint_id: manifest.checkpoint_id,
            edit_id: manifest.edit_id,
            project_id: manifest.project_id,
            root_identity: manifest.root_identity,
            guarantee: manifest.guarantee,
            limitations: manifest.limitations,
            paths: manifest.paths,
        })
    }

    pub fn read_preimage(
        &self,
        descriptor: &NonGitCheckpointDescriptor,
        path: &str,
    ) -> Result<Vec<u8>, NonGitCheckpointError> {
        let manifest = self.load(descriptor)?;
        let checkpoint_path = manifest
            .paths
            .iter()
            .find(|candidate| candidate.path == path)
            .ok_or_else(|| error("non-Git checkpoint path is not present"))?;
        let NonGitCheckpointPathState::File {
            hash,
            blob_reference: Some(reference),
            ..
        } = &checkpoint_path.before
        else {
            return Err(error("non-Git checkpoint path has no preimage"));
        };
        if reference != &format!("non-git-checkpoint-blob:sha256:{}", hash.sha256) {
            return Err(error("non-Git checkpoint preimage reference is invalid"));
        }
        let bytes = self.read_object("blobs", &hash.sha256, MAX_FILE_BYTES)?;
        if ContentHash::for_bytes(&bytes) != *hash {
            return Err(error("non-Git checkpoint preimage hash mismatch"));
        }
        Ok(bytes)
    }

    fn capture_before_state(
        &self,
        root: &Path,
        relative: &str,
        expected: &ExpectedWorkspacePathState,
        retained: &mut u64,
    ) -> Result<NonGitCheckpointPathState, NonGitCheckpointError> {
        let path = resolve_path(
            root,
            relative,
            matches!(expected, ExpectedWorkspacePathState::Absent),
        )?;
        match expected {
            ExpectedWorkspacePathState::Absent => {
                if fs::symlink_metadata(path).is_ok() {
                    return Err(error("non-Git checkpoint expected an absent target"));
                }
                Ok(NonGitCheckpointPathState::Absent)
            }
            ExpectedWorkspacePathState::File { hash: expected } => {
                let (bytes, mode) = read_text_file(&path)?;
                let hash = ContentHash::for_bytes(&bytes);
                if hash != *expected {
                    return Err(error("non-Git checkpoint base SHA-256 is stale"));
                }
                *retained = retained.saturating_add(bytes.len() as u64);
                if *retained > MAX_TOTAL_BLOB_BYTES {
                    return Err(error("non-Git checkpoint blob budget exceeded"));
                }
                self.write_object("blobs", &hash.sha256, &bytes)?;
                Ok(NonGitCheckpointPathState::File {
                    blob_reference: Some(format!("non-git-checkpoint-blob:sha256:{}", hash.sha256)),
                    hash,
                    mode,
                })
            }
        }
    }

    fn validate_manifest(
        &self,
        descriptor: &NonGitCheckpointDescriptor,
        manifest: &NonGitCheckpointManifest,
    ) -> Result<(), NonGitCheckpointError> {
        if manifest.schema_version != SCHEMA_VERSION
            || manifest.checkpoint_id != descriptor.checkpoint_id
            || manifest.storage_identity != self.storage_identity
            || manifest.guarantee != WEAKER_GUARANTEE
            || manifest.limitations != descriptor.limitations
            || manifest.paths.len() > 512
            || manifest.retained_blob_bytes > MAX_TOTAL_BLOB_BYTES
        {
            return Err(error("non-Git checkpoint manifest binding is invalid"));
        }
        let mut retained = 0_u64;
        for path in &manifest.paths {
            if let NonGitCheckpointPathState::File {
                hash,
                blob_reference: Some(reference),
                ..
            } = &path.before
            {
                if reference != &format!("non-git-checkpoint-blob:sha256:{}", hash.sha256) {
                    return Err(error("non-Git checkpoint blob reference is invalid"));
                }
                let bytes = self.read_object("blobs", &hash.sha256, MAX_FILE_BYTES)?;
                if ContentHash::for_bytes(&bytes) != *hash {
                    return Err(error("non-Git checkpoint blob hash mismatch"));
                }
                retained = retained.saturating_add(bytes.len() as u64);
            }
        }
        if retained != manifest.retained_blob_bytes {
            return Err(error("non-Git checkpoint retained-byte count is invalid"));
        }
        Ok(())
    }

    fn write_object(
        &self,
        directory: &str,
        name: &str,
        bytes: &[u8],
    ) -> Result<(), NonGitCheckpointError> {
        let parent = self.root.join(directory);
        let target = parent.join(name);
        if let Ok(existing) = self.read_object(directory, name, MAX_MANIFEST_BYTES as u64) {
            if existing == bytes {
                return Ok(());
            }
            return Err(error("content-addressed checkpoint object hash collision"));
        }
        let temporary = temporary_path(&parent, "object");
        let result = (|| {
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&temporary)
                .map_err(|_| error("cannot create checkpoint object stage"))?;
            secure_file(&file)?;
            file.write_all(bytes)
                .and_then(|()| file.sync_all())
                .map_err(|_| error("cannot flush checkpoint object"))?;
            match fs::hard_link(&temporary, &target) {
                Ok(()) => Ok(()),
                Err(cause) if cause.kind() == std::io::ErrorKind::AlreadyExists => {
                    let existing = self.read_object(directory, name, MAX_MANIFEST_BYTES as u64)?;
                    if existing == bytes {
                        Ok(())
                    } else {
                        Err(error("content-addressed checkpoint object changed"))
                    }
                }
                Err(_) => Err(error("cannot commit checkpoint object")),
            }
        })();
        let _ = fs::remove_file(&temporary);
        result?;
        sync_directory(&parent)
    }

    fn read_object(
        &self,
        directory: &str,
        name: &str,
        limit: u64,
    ) -> Result<Vec<u8>, NonGitCheckpointError> {
        if !valid_hash(name) {
            return Err(error("checkpoint object name is invalid"));
        }
        let path = self.root.join(directory).join(name);
        let metadata =
            fs::symlink_metadata(&path).map_err(|_| error("checkpoint object is unavailable"))?;
        if metadata.file_type().is_symlink() || !metadata.is_file() || metadata.len() > limit {
            return Err(error("checkpoint object type or size is invalid"));
        }
        let mut bytes = Vec::with_capacity(metadata.len() as usize);
        File::open(path)
            .and_then(|file| file.take(limit + 1).read_to_end(&mut bytes))
            .map_err(|_| error("cannot read checkpoint object"))?;
        if bytes.len() as u64 > limit {
            return Err(error("checkpoint object exceeds limit"));
        }
        Ok(bytes)
    }

    fn write_pointer(&self, path: &Path, hash: &str) -> Result<(), NonGitCheckpointError> {
        let parent = path
            .parent()
            .ok_or_else(|| error("checkpoint pointer has no parent"))?;
        let temporary = temporary_path(parent, "pointer");
        let result = (|| {
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&temporary)
                .map_err(|_| error("cannot create checkpoint pointer stage"))?;
            secure_file(&file)?;
            file.write_all(format!("{hash}\n").as_bytes())
                .and_then(|()| file.sync_all())
                .map_err(|_| error("cannot flush checkpoint pointer"))?;
            fs::hard_link(&temporary, path)
                .map_err(|_| error("cannot commit no-clobber checkpoint pointer"))
        })();
        let _ = fs::remove_file(&temporary);
        result?;
        sync_directory(parent)
    }

    fn read_pointer(&self, checkpoint_id: &str) -> Result<String, NonGitCheckpointError> {
        let path = self.root.join("checkpoints").join(checkpoint_id);
        let metadata = fs::symlink_metadata(&path)
            .map_err(|_| error("non-Git checkpoint pointer is unavailable"))?;
        if metadata.file_type().is_symlink() || !metadata.is_file() || metadata.len() > 65 {
            return Err(error("non-Git checkpoint pointer is invalid"));
        }
        let value = fs::read_to_string(path)
            .map_err(|_| error("cannot read non-Git checkpoint pointer"))?;
        let hash = value.trim();
        if !valid_hash(hash) {
            return Err(error("non-Git checkpoint pointer hash is invalid"));
        }
        Ok(hash.into())
    }

    fn validate_layout(&self) -> Result<(), NonGitCheckpointError> {
        if self.root.canonicalize().ok().as_deref() != Some(self.root.as_path()) {
            return Err(error("non-Git checkpoint store identity changed"));
        }
        for directory in ["blobs", "manifests", "checkpoints"] {
            let path = self.root.join(directory);
            let metadata = fs::symlink_metadata(&path)
                .map_err(|_| error("non-Git checkpoint directory is unavailable"))?;
            if metadata.file_type().is_symlink() || !metadata.is_dir() {
                return Err(error("non-Git checkpoint directory is unsafe"));
            }
        }
        Ok(())
    }

    fn enforce_store_limits(&self) -> Result<(), NonGitCheckpointError> {
        let checkpoints = fs::read_dir(self.root.join("checkpoints"))
            .map_err(|_| error("cannot enumerate non-Git checkpoints"))?
            .take(MAX_CHECKPOINTS + 1)
            .count();
        if checkpoints >= MAX_CHECKPOINTS {
            return Err(error("non-Git checkpoint count limit exceeded"));
        }
        let mut entries = 0_usize;
        let mut bytes = 0_u64;
        for directory in ["blobs", "manifests"] {
            for entry in fs::read_dir(self.root.join(directory))
                .map_err(|_| error("cannot enumerate checkpoint objects"))?
            {
                let entry = entry.map_err(|_| error("cannot inspect checkpoint object"))?;
                entries += 1;
                if entries > MAX_STORE_ENTRIES.saturating_sub(MAX_CAPTURE_OBJECTS) {
                    return Err(error("non-Git checkpoint object limit exceeded"));
                }
                let metadata = fs::symlink_metadata(entry.path())
                    .map_err(|_| error("cannot inspect checkpoint object metadata"))?;
                if metadata.file_type().is_symlink() || !metadata.is_file() {
                    return Err(error("non-Git checkpoint object entry is unsafe"));
                }
                bytes = bytes.saturating_add(metadata.len());
                if bytes
                    > MAX_STORE_BYTES
                        .saturating_sub(MAX_TOTAL_BLOB_BYTES)
                        .saturating_sub(MAX_MANIFEST_BYTES as u64)
                {
                    return Err(error("non-Git checkpoint store byte limit exceeded"));
                }
            }
        }
        Ok(())
    }
}

fn validate_applied_modes(
    paths: &[NonGitCheckpointPath],
    edit: &WorkspaceEdit,
    applied: &WorkspaceEditApplyResult,
) -> Result<(), NonGitCheckpointError> {
    for (operation, file) in edit.operations.iter().zip(&applied.files) {
        let final_path = match operation {
            crate::workspace_edit::WorkspaceEditOperation::Create { path, .. }
            | crate::workspace_edit::WorkspaceEditOperation::Update { path, .. } => {
                Some(path.as_str())
            }
            crate::workspace_edit::WorkspaceEditOperation::Delete { .. } => None,
            crate::workspace_edit::WorkspaceEditOperation::Rename { to_path, .. } => {
                Some(to_path.as_str())
            }
        };
        let expected_mode = final_path.and_then(|path| {
            paths.iter().find_map(|candidate| {
                if candidate.path != path {
                    return None;
                }
                match &candidate.planned_after {
                    NonGitCheckpointPathState::File { mode, .. } => Some(mode.as_str()),
                    NonGitCheckpointPathState::Absent => None,
                }
            })
        });
        if file.final_mode.as_deref() != expected_mode {
            return Err(error(
                "non-Git checkpoint applied file mode does not match plan",
            ));
        }
    }
    Ok(())
}

fn expected_to_state(
    expected: &ExpectedWorkspacePathState,
    blob_reference: Option<String>,
    mode: String,
) -> NonGitCheckpointPathState {
    match expected {
        ExpectedWorkspacePathState::Absent => NonGitCheckpointPathState::Absent,
        ExpectedWorkspacePathState::File { hash } => NonGitCheckpointPathState::File {
            hash: hash.clone(),
            blob_reference,
            mode,
        },
    }
}

fn state_matches(
    state: &NonGitCheckpointPathState,
    expected: &ExpectedWorkspacePathState,
    require_blob: bool,
) -> bool {
    match (state, expected) {
        (NonGitCheckpointPathState::Absent, ExpectedWorkspacePathState::Absent) => true,
        (
            NonGitCheckpointPathState::File {
                hash,
                blob_reference,
                mode,
            },
            ExpectedWorkspacePathState::File { hash: expected },
        ) => {
            hash == expected
                && matches!(mode.as_str(), "100644" | "100755")
                && if require_blob {
                    blob_reference.as_deref()
                        == Some(format!("non-git-checkpoint-blob:sha256:{}", hash.sha256).as_str())
                } else {
                    blob_reference.is_none()
                }
        }
        _ => false,
    }
}

fn state_mode(state: &NonGitCheckpointPathState) -> String {
    match state {
        NonGitCheckpointPathState::File { mode, .. } => mode.clone(),
        NonGitCheckpointPathState::Absent => "100644".into(),
    }
}

fn declared_mode<'a>(edit: &'a WorkspaceEdit, path: &str) -> &'a str {
    edit.operations
        .iter()
        .find_map(|operation| match operation {
            crate::workspace_edit::WorkspaceEditOperation::Create {
                path: candidate,
                content,
            }
            | crate::workspace_edit::WorkspaceEditOperation::Update {
                path: candidate,
                content,
                ..
            } if candidate == path => Some(content.format.mode.as_str()),
            _ => None,
        })
        .unwrap_or("preserve")
}

fn resolve_planned_mode(declared: &str, preserved: &str) -> Result<String, NonGitCheckpointError> {
    if !platform_supports_file_mode(declared) {
        return Err(error(
            "non-Git checkpoint executable mode requires POSIX file-mode support on this platform",
        ));
    }
    match declared {
        "preserve" => Ok(preserved.into()),
        "regular" => Ok("100644".into()),
        "executable" => Ok("100755".into()),
        _ => Err(error("non-Git checkpoint file mode policy is unsupported")),
    }
}

fn validate_id(value: &str) -> Result<(), NonGitCheckpointError> {
    if value.is_empty()
        || value.len() > 128
        || value.starts_with('.')
        || value.contains("..")
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.'))
    {
        return Err(error("non-Git checkpoint ID is unsafe"));
    }
    Ok(())
}

fn validate_edit(edit: &WorkspaceEdit) -> Result<(), NonGitCheckpointError> {
    let rebuilt = WorkspaceEdit::define(
        edit.edit_id.clone(),
        edit.project_id.clone(),
        &edit.root.canonical_path,
        edit.operations.clone(),
    )
    .map_err(|cause| error(cause.message))?;
    if rebuilt != *edit {
        return Err(error("non-Git checkpoint edit binding was modified"));
    }
    Ok(())
}

fn validate_pending_paths(
    paths: &HashSet<String>,
) -> Result<(Vec<String>, usize), NonGitCheckpointError> {
    if paths.len() > 512 {
        return Err(error("pending-user path limit exceeded"));
    }
    let mut visible = Vec::new();
    let mut redacted = 0_usize;
    for path in paths {
        if path.is_empty()
            || path.len() > 4 * 1024
            || Path::new(path).is_absolute()
            || Path::new(path)
                .components()
                .any(|component| !matches!(component, Component::Normal(_)))
        {
            return Err(error("pending-user path is not normalized"));
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

fn contains_git_metadata(root: &Path) -> bool {
    root.ancestors()
        .any(|ancestor| fs::symlink_metadata(ancestor.join(".git")).is_ok())
}

fn resolve_path(
    root: &Path,
    relative: &str,
    allow_missing_final: bool,
) -> Result<PathBuf, NonGitCheckpointError> {
    if root.canonicalize().ok().as_deref() != Some(root) {
        return Err(error("non-Git checkpoint project root changed"));
    }
    let mut path = root.to_path_buf();
    let components = Path::new(relative).components().collect::<Vec<_>>();
    for (index, component) in components.iter().enumerate() {
        let Component::Normal(name) = component else {
            return Err(error("non-Git checkpoint path is not normalized"));
        };
        path.push(name);
        let final_component = index + 1 == components.len();
        match fs::symlink_metadata(&path) {
            Ok(metadata) if metadata.file_type().is_symlink() => {
                return Err(error("non-Git checkpoint path traverses a symlink"))
            }
            Ok(metadata) if !final_component && !metadata.is_dir() => {
                return Err(error("non-Git checkpoint parent is not a directory"))
            }
            Ok(_) => {}
            Err(cause)
                if allow_missing_final
                    && final_component
                    && cause.kind() == std::io::ErrorKind::NotFound => {}
            Err(_) => return Err(error("non-Git checkpoint path is unavailable")),
        }
    }
    Ok(path)
}

fn read_text_file(path: &Path) -> Result<(Vec<u8>, String), NonGitCheckpointError> {
    let metadata = fs::metadata(path).map_err(|_| error("checkpoint base is unavailable"))?;
    if !metadata.is_file() || metadata.len() > MAX_FILE_BYTES {
        return Err(error("checkpoint base is not a bounded regular file"));
    }
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    File::open(path)
        .and_then(|file| file.take(MAX_FILE_BYTES + 1).read_to_end(&mut bytes))
        .map_err(|_| error("cannot read checkpoint base"))?;
    let text = bytes.strip_prefix(&[0xef, 0xbb, 0xbf]).unwrap_or(&bytes);
    if bytes.len() as u64 > MAX_FILE_BYTES
        || bytes.contains(&0)
        || std::str::from_utf8(text).is_err()
    {
        return Err(error("checkpoint base must be bounded UTF-8 text"));
    }
    if inspect_text_format(&bytes, "preserve").newline == "mixed" {
        return Err(error("checkpoint base has mixed line endings"));
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

fn valid_hash(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
}

fn temporary_path(parent: &Path, kind: &str) -> PathBuf {
    let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    parent.join(format!(
        ".aegisy-checkpoint-{}-{sequence}.{kind}",
        std::process::id()
    ))
}

fn secure_directory(path: &Path) -> Result<(), NonGitCheckpointError> {
    let metadata = fs::symlink_metadata(path)
        .map_err(|_| error("checkpoint storage directory is unavailable"))?;
    if metadata.file_type().is_symlink() || !metadata.is_dir() {
        return Err(error("checkpoint storage directory is unsafe"));
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(path, fs::Permissions::from_mode(0o700))
            .map_err(|_| error("cannot secure checkpoint storage directory"))?;
    }
    Ok(())
}

fn create_secure_directory(path: &Path) -> Result<(), NonGitCheckpointError> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() || !metadata.is_dir() => {
            return Err(error("checkpoint storage directory is unsafe"))
        }
        Ok(_) => {}
        Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => {
            fs::create_dir(path).map_err(|_| error("cannot create non-Git checkpoint storage"))?;
        }
        Err(_) => return Err(error("checkpoint storage directory is unavailable")),
    }
    secure_directory(path)
}

fn secure_file(file: &File) -> Result<(), NonGitCheckpointError> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        file.set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(|_| error("cannot secure checkpoint file"))?;
    }
    Ok(())
}

fn sync_directory(path: &Path) -> Result<(), NonGitCheckpointError> {
    #[cfg(unix)]
    File::open(path)
        .and_then(|directory| directory.sync_all())
        .map_err(|_| error("cannot sync checkpoint directory"))?;
    Ok(())
}

fn error(message: impl Into<String>) -> NonGitCheckpointError {
    NonGitCheckpointError {
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::workspace_edit::{ProposedContent, WorkspaceEditOperation};
    use crate::workspace_edit_apply::apply_workspace_edit;
    use crate::workspace_edit_preview::ContentInput;

    fn roots() -> (PathBuf, PathBuf, PathBuf) {
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let parent = std::env::temp_dir().join(format!(
            "aegisy-non-git-checkpoint-{}-{sequence}",
            std::process::id()
        ));
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
        fs::write(root.join("delete.txt"), "shared\n").unwrap();
        fs::write(root.join("old.txt"), "shared\n").unwrap();
        let created = ProposedContent::for_bytes(b"created\n");
        let updated = ProposedContent::for_bytes(b"after\n");
        let edit = WorkspaceEdit::define(
            "non-git-edit",
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
                    base: ContentHash::for_bytes(b"shared\n"),
                },
                WorkspaceEditOperation::Rename {
                    from_path: "old.txt".into(),
                    to_path: "new.txt".into(),
                    base: ContentHash::for_bytes(b"shared\n"),
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

    #[test]
    fn persists_deduplicated_preimages_and_binds_a_real_apply_after_reopen() {
        let (parent, project, storage) = roots();
        let (edit, contents) = edit(&project);
        let store = NonGitCheckpointStore::open(&storage, &project).unwrap();
        let descriptor = store
            .capture(
                "checkpoint-1",
                &edit,
                &HashSet::new(),
                &HashSet::from(["notes.txt".into(), ".env".into()]),
            )
            .unwrap();
        assert_eq!(descriptor.guarantee, WEAKER_GUARANTEE);
        assert_eq!(descriptor.limitations.len(), 4);
        assert_eq!(
            fs::read_dir(store.root.join("blobs")).unwrap().count(),
            2,
            "identical delete/rename preimages must deduplicate"
        );
        assert_eq!(
            fs::read_to_string(project.join("update.txt")).unwrap(),
            "before\n"
        );
        assert!(!project.join("created.txt").exists());

        let reopened = NonGitCheckpointStore::open(&storage, &project).unwrap();
        let manifest = reopened.load(&descriptor).unwrap();
        assert_eq!(manifest.pending_user_paths, vec!["notes.txt"]);
        assert_eq!(manifest.redacted_pending_count, 1);
        assert_eq!(manifest.paths.len(), 5);
        assert_eq!(
            manifest.retained_blob_bytes,
            b"before\n".len() as u64 + 2 * b"shared\n".len() as u64
        );

        let applied = apply_workspace_edit(edit.clone(), contents, &HashSet::new()).unwrap();
        let application = reopened
            .bind_application(&descriptor, &edit, &applied)
            .unwrap();
        assert_eq!(application.paths.len(), 5);
        assert_eq!(application.guarantee, WEAKER_GUARANTEE);
        let mut forged_mode = applied.clone();
        forged_mode.files[1].final_mode = Some("100755".into());
        assert!(reopened
            .bind_application(&descriptor, &edit, &forged_mode)
            .is_err());
        fs::remove_dir_all(parent).unwrap();
    }

    #[test]
    fn rejects_stale_ignored_pending_symlink_git_and_overlapping_storage() {
        let (parent, project, storage) = roots();
        let (edit, _) = edit(&project);
        let store = NonGitCheckpointStore::open(&storage, &project).unwrap();
        let mut stale = edit.clone();
        if let WorkspaceEditOperation::Update { base, .. } = &mut stale.operations[1] {
            *base = ContentHash::for_bytes(b"stale\n");
        }
        assert!(store
            .capture("stale", &stale, &HashSet::new(), &HashSet::new())
            .is_err());
        assert!(store
            .capture(
                "ignored",
                &edit,
                &HashSet::from(["update.txt".into()]),
                &HashSet::new()
            )
            .is_err());
        assert!(store
            .capture(
                "pending",
                &edit,
                &HashSet::new(),
                &HashSet::from(["update.txt".into()])
            )
            .is_err());
        assert!(NonGitCheckpointStore::open(&project, &project).is_err());

        #[cfg(unix)]
        {
            use std::os::unix::fs::symlink;
            fs::remove_file(project.join("update.txt")).unwrap();
            symlink(project.join("delete.txt"), project.join("update.txt")).unwrap();
            assert!(store
                .capture("symlink", &edit, &HashSet::new(), &HashSet::new())
                .is_err());
            fs::remove_file(project.join("update.txt")).unwrap();
            fs::write(project.join("update.txt"), "before\n").unwrap();
        }

        let output = std::process::Command::new("git")
            .arg("-C")
            .arg(&project)
            .args(["init", "-q"])
            .output()
            .unwrap();
        assert!(output.status.success());
        assert!(store
            .capture("git", &edit, &HashSet::new(), &HashSet::new())
            .is_err());
        fs::remove_dir_all(parent).unwrap();
    }

    #[test]
    fn detects_manifest_blob_pointer_and_layout_tampering() {
        let (parent, project, storage) = roots();
        let (edit, _) = edit(&project);
        let store = NonGitCheckpointStore::open(&storage, &project).unwrap();
        let descriptor = store
            .capture("tamper", &edit, &HashSet::new(), &HashSet::new())
            .unwrap();
        let manifest = store.load(&descriptor).unwrap();
        let blob_hash = manifest
            .paths
            .iter()
            .find_map(|path| match &path.before {
                NonGitCheckpointPathState::File { hash, .. } => Some(hash.sha256.clone()),
                NonGitCheckpointPathState::Absent => None,
            })
            .unwrap();
        fs::write(store.root.join("blobs").join(blob_hash), "tampered\n").unwrap();
        assert!(store.load(&descriptor).is_err());

        let mut forged = descriptor.clone();
        forged.guarantee = "strong".into();
        assert!(store.load(&forged).is_err());
        fs::write(
            store.root.join("checkpoints").join("tamper"),
            format!("{}\n", "0".repeat(64)),
        )
        .unwrap();
        assert!(store.load(&descriptor).is_err());
        fs::remove_dir_all(parent).unwrap();
    }

    #[test]
    fn enforces_file_checkpoint_object_and_reserved_store_limits() {
        let (parent, project, storage) = roots();
        let store = NonGitCheckpointStore::open(&storage, &project).unwrap();
        fs::write(
            project.join("large.txt"),
            vec![b'x'; MAX_FILE_BYTES as usize + 1],
        )
        .unwrap();
        let content = ProposedContent::for_bytes(b"after\n");
        let large_edit = WorkspaceEdit::define(
            "large-edit",
            "project-1",
            &project,
            vec![WorkspaceEditOperation::Update {
                path: "large.txt".into(),
                base: ContentHash::for_bytes(&vec![b'x'; MAX_FILE_BYTES as usize + 1]),
                content,
            }],
        )
        .unwrap();
        assert!(store
            .capture("large", &large_edit, &HashSet::new(), &HashSet::new())
            .is_err());
        assert!(!store.root.join("checkpoints/large").exists());

        let reserved_limit = MAX_STORE_BYTES
            .saturating_sub(MAX_TOTAL_BLOB_BYTES)
            .saturating_sub(MAX_MANIFEST_BYTES as u64);
        let sparse = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(store.root.join("blobs").join("a".repeat(64)))
            .unwrap();
        sparse.set_len(reserved_limit + 1).unwrap();
        let (small_edit, _) = edit(&project);
        assert!(store
            .capture("full", &small_edit, &HashSet::new(), &HashSet::new())
            .is_err());
        assert!(!store.root.join("checkpoints/full").exists());
        fs::remove_dir_all(parent).unwrap();
    }
}
