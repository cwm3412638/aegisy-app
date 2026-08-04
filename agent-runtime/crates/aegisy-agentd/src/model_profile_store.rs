//! Durable, metadata-only storage for global and project model profiles.
//!
//! The store is deliberately independent from AAP and the model router. It
//! persists only validated `model-profile/0.1` metadata in one atomically
//! replaced snapshot, uses compare-and-swap revisions for edits, and never
//! stores credentials or grants a profile selection/execution decision.

use crate::model_profile::{ModelProfile, ProfileScope};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

pub const STORE_SCHEMA_VERSION: &str = "model-profile-store/0.1";
const MAX_PROFILES: usize = 256;
const MAX_PROJECTS: usize = 1_024;
const MAX_SNAPSHOT_BYTES: usize = 2 * 1024 * 1024;
static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ModelProfileStoreSnapshot {
    pub schema_version: String,
    pub generation: u64,
    pub snapshot_identity: String,
    pub global: Option<ModelProfile>,
    pub projects: BTreeMap<String, ModelProfile>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ModelProfileStoreDescriptor {
    pub schema_version: String,
    pub generation: u64,
    pub scope: ProfileScope,
    pub project_id: Option<String>,
    pub profile_id: String,
    pub profile_identity: String,
    pub revision: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProfileStoreWrite {
    Created,
    Updated,
    Idempotent,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModelProfileStoreError {
    pub code: &'static str,
}

#[derive(Debug)]
pub struct ModelProfileStore {
    root: PathBuf,
    snapshot: ModelProfileStoreSnapshot,
}

impl ModelProfileStore {
    pub fn open(data_root: &Path) -> Result<Self, ModelProfileStoreError> {
        let metadata = fs::symlink_metadata(data_root)
            .map_err(|_| error("model-profile-data-root-unavailable"))?;
        if metadata.file_type().is_symlink() || !metadata.is_dir() {
            return Err(error("model-profile-data-root-unsafe"));
        }
        let data_root = data_root
            .canonicalize()
            .map_err(|_| error("model-profile-data-root-unavailable"))?;
        let root = data_root.join("model-profiles-v1");
        create_secure_directory(&root)?;
        let root = root
            .canonicalize()
            .map_err(|_| error("model-profile-store-unavailable"))?;
        if root != data_root.join("model-profiles-v1") {
            return Err(error("model-profile-store-symlink"));
        }
        let snapshot_path = root.join("snapshot.json");
        let snapshot = match fs::symlink_metadata(&snapshot_path) {
            Ok(metadata) => {
                if metadata.file_type().is_symlink() || !metadata.is_file() {
                    return Err(error("model-profile-snapshot-unsafe"));
                }
                let bytes = read_limited(&snapshot_path, MAX_SNAPSHOT_BYTES)?;
                let snapshot: ModelProfileStoreSnapshot = serde_json::from_slice(&bytes)
                    .map_err(|_| error("model-profile-snapshot-invalid"))?;
                validate_snapshot(&snapshot)?;
                snapshot
            }
            Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => empty_snapshot()?,
            Err(_) => return Err(error("model-profile-snapshot-unavailable")),
        };
        Ok(Self { root, snapshot })
    }

    pub fn snapshot(&self) -> Result<ModelProfileStoreSnapshot, ModelProfileStoreError> {
        self.validate()?;
        Ok(self.snapshot.clone())
    }

    pub fn identity(&self) -> Result<String, ModelProfileStoreError> {
        self.validate()?;
        Ok(self.snapshot.snapshot_identity.clone())
    }

    pub fn global(&self) -> Result<Option<ModelProfile>, ModelProfileStoreError> {
        self.validate()?;
        Ok(self.snapshot.global.clone())
    }

    pub fn project(
        &self,
        project_id: &str,
    ) -> Result<Option<ModelProfile>, ModelProfileStoreError> {
        validate_project_id(project_id)?;
        self.validate()?;
        Ok(self.snapshot.projects.get(project_id).cloned())
    }

    pub fn save(
        &mut self,
        profile: ModelProfile,
        expected_revision: Option<u64>,
    ) -> Result<(ProfileStoreWrite, ModelProfileStoreDescriptor), ModelProfileStoreError> {
        self.validate()?;
        profile
            .validate()
            .map_err(|_| error("model-profile-invalid"))?;
        let key = profile_key(&profile)?;
        let current = self.lookup(&key);
        let write_kind = match current {
            None => {
                if expected_revision.is_some() || profile.revision != 0 {
                    return Err(error("model-profile-cas-mismatch"));
                }
                ProfileStoreWrite::Created
            }
            Some(current) => {
                if expected_revision != Some(current.revision) {
                    return Err(error("model-profile-cas-mismatch"));
                }
                let current_identity = current
                    .identity()
                    .map_err(|_| error("model-profile-identity-failed"))?;
                let incoming_identity = profile
                    .identity()
                    .map_err(|_| error("model-profile-identity-failed"))?;
                if current_identity == incoming_identity {
                    return Ok((
                        ProfileStoreWrite::Idempotent,
                        descriptor(&self.snapshot, &profile)?,
                    ));
                }
                if profile.profile_id != current.profile_id
                    || profile.revision != current.revision.saturating_add(1)
                {
                    return Err(error("model-profile-revision-mismatch"));
                }
                ProfileStoreWrite::Updated
            }
        };
        let mut next = self.snapshot.clone();
        next.generation = next
            .generation
            .checked_add(1)
            .ok_or_else(|| error("model-profile-generation-exhausted"))?;
        match key {
            ProfileKey::Global => next.global = Some(profile.clone()),
            ProfileKey::Project(project_id) => {
                next.projects.insert(project_id, profile.clone());
            }
        }
        finalize_snapshot(&mut next)?;
        self.write_snapshot(&next)?;
        let descriptor = descriptor(&next, &profile)?;
        self.snapshot = next;
        Ok((write_kind, descriptor))
    }

    pub fn remove(
        &mut self,
        scope: ProfileScope,
        project_id: Option<&str>,
        expected_revision: u64,
    ) -> Result<ModelProfileStoreDescriptor, ModelProfileStoreError> {
        self.validate()?;
        let key = profile_key_for_scope(scope, project_id)?;
        let current = self
            .lookup(&key)
            .ok_or_else(|| error("model-profile-not-found"))?;
        if current.revision != expected_revision {
            return Err(error("model-profile-cas-mismatch"));
        }
        let mut next = self.snapshot.clone();
        next.generation = next
            .generation
            .checked_add(1)
            .ok_or_else(|| error("model-profile-generation-exhausted"))?;
        match key {
            ProfileKey::Global => next.global = None,
            ProfileKey::Project(project_id) => {
                next.projects.remove(&project_id);
            }
        }
        finalize_snapshot(&mut next)?;
        self.write_snapshot(&next)?;
        let descriptor = descriptor(&next, current)?;
        self.snapshot = next;
        Ok(descriptor)
    }

    fn lookup(&self, key: &ProfileKey) -> Option<&ModelProfile> {
        match key {
            ProfileKey::Global => self.snapshot.global.as_ref(),
            ProfileKey::Project(project_id) => self.snapshot.projects.get(project_id),
        }
    }

    fn validate(&self) -> Result<(), ModelProfileStoreError> {
        validate_snapshot(&self.snapshot)
    }

    fn write_snapshot(
        &self,
        snapshot: &ModelProfileStoreSnapshot,
    ) -> Result<(), ModelProfileStoreError> {
        let bytes =
            serde_json::to_vec(snapshot).map_err(|_| error("model-profile-snapshot-serialize"))?;
        if bytes.len() > MAX_SNAPSHOT_BYTES {
            return Err(error("model-profile-snapshot-too-large"));
        }
        let target = self.root.join("snapshot.json");
        if let Ok(metadata) = fs::symlink_metadata(&target) {
            if metadata.file_type().is_symlink() || !metadata.is_file() {
                return Err(error("model-profile-snapshot-unsafe"));
            }
        }
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let temporary = self.root.join(format!(".snapshot.{sequence}.tmp"));
        let result = (|| {
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&temporary)
                .map_err(|_| error("model-profile-snapshot-stage"))?;
            secure_file(&file)?;
            file.write_all(&bytes)
                .and_then(|()| file.sync_all())
                .map_err(|_| error("model-profile-snapshot-sync"))?;
            replace_file(&temporary, &target)
        })();
        let _ = fs::remove_file(&temporary);
        result?;
        sync_directory(&self.root)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum ProfileKey {
    Global,
    Project(String),
}

fn profile_key(profile: &ModelProfile) -> Result<ProfileKey, ModelProfileStoreError> {
    match profile.scope {
        ProfileScope::Global => {
            if profile.project_id.is_some() {
                return Err(error("model-profile-global-project-invalid"));
            }
            Ok(ProfileKey::Global)
        }
        ProfileScope::Project => Ok(ProfileKey::Project(
            profile
                .project_id
                .clone()
                .ok_or_else(|| error("model-profile-project-missing"))?,
        )),
    }
}

fn profile_key_for_scope(
    scope: ProfileScope,
    project_id: Option<&str>,
) -> Result<ProfileKey, ModelProfileStoreError> {
    match scope {
        ProfileScope::Global if project_id.is_none() => Ok(ProfileKey::Global),
        ProfileScope::Project => Ok(ProfileKey::Project({
            let project_id = project_id.ok_or_else(|| error("model-profile-project-missing"))?;
            validate_project_id(project_id)?;
            project_id.to_owned()
        })),
        _ => Err(error("model-profile-scope-invalid")),
    }
}

fn descriptor(
    snapshot: &ModelProfileStoreSnapshot,
    profile: &ModelProfile,
) -> Result<ModelProfileStoreDescriptor, ModelProfileStoreError> {
    let profile_identity = profile
        .identity()
        .map_err(|_| error("model-profile-identity-failed"))?;
    Ok(ModelProfileStoreDescriptor {
        schema_version: STORE_SCHEMA_VERSION.into(),
        generation: snapshot.generation,
        scope: profile.scope,
        project_id: profile.project_id.clone(),
        profile_id: profile.profile_id.clone(),
        profile_identity,
        revision: profile.revision,
    })
}

fn empty_snapshot() -> Result<ModelProfileStoreSnapshot, ModelProfileStoreError> {
    let mut snapshot = ModelProfileStoreSnapshot {
        schema_version: STORE_SCHEMA_VERSION.into(),
        generation: 0,
        snapshot_identity: String::new(),
        global: None,
        projects: BTreeMap::new(),
    };
    finalize_snapshot(&mut snapshot)?;
    Ok(snapshot)
}

fn finalize_snapshot(
    snapshot: &mut ModelProfileStoreSnapshot,
) -> Result<(), ModelProfileStoreError> {
    snapshot.snapshot_identity = snapshot_identity(snapshot)?;
    validate_snapshot(snapshot)
}

fn validate_snapshot(snapshot: &ModelProfileStoreSnapshot) -> Result<(), ModelProfileStoreError> {
    if snapshot.schema_version != STORE_SCHEMA_VERSION {
        return Err(error("model-profile-store-schema-unsupported"));
    }
    if snapshot.projects.len() > MAX_PROJECTS {
        return Err(error("model-profile-project-limit"));
    }
    let profile_count = snapshot.projects.len() + usize::from(snapshot.global.is_some());
    if profile_count > MAX_PROFILES {
        return Err(error("model-profile-count-limit"));
    }
    let mut profile_ids = std::collections::BTreeSet::new();
    if let Some(global) = &snapshot.global {
        if global.scope != ProfileScope::Global || global.project_id.is_some() {
            return Err(error("model-profile-global-project-invalid"));
        }
        global
            .validate()
            .map_err(|_| error("model-profile-invalid"))?;
        if !profile_ids.insert(global.profile_id.as_str()) {
            return Err(error("model-profile-id-duplicate"));
        }
    }
    for (project_id, profile) in &snapshot.projects {
        validate_project_id(project_id)?;
        if profile.scope != ProfileScope::Project
            || profile.project_id.as_deref() != Some(project_id.as_str())
        {
            return Err(error("model-profile-project-binding-invalid"));
        }
        profile
            .validate()
            .map_err(|_| error("model-profile-invalid"))?;
        if !profile_ids.insert(profile.profile_id.as_str()) {
            return Err(error("model-profile-id-duplicate"));
        }
    }
    if snapshot.snapshot_identity != snapshot_identity_without_identity(snapshot)? {
        return Err(error("model-profile-snapshot-identity-mismatch"));
    }
    let bytes =
        serde_json::to_vec(snapshot).map_err(|_| error("model-profile-snapshot-serialize"))?;
    if bytes.len() > MAX_SNAPSHOT_BYTES {
        return Err(error("model-profile-snapshot-too-large"));
    }
    Ok(())
}

fn snapshot_identity(
    snapshot: &ModelProfileStoreSnapshot,
) -> Result<String, ModelProfileStoreError> {
    Ok(format!(
        "model-profile-store:sha256:{:x}",
        Sha256::digest(snapshot_identity_bytes(snapshot)?)
    ))
}

fn snapshot_identity_without_identity(
    snapshot: &ModelProfileStoreSnapshot,
) -> Result<String, ModelProfileStoreError> {
    snapshot_identity(snapshot)
}

fn snapshot_identity_bytes(
    snapshot: &ModelProfileStoreSnapshot,
) -> Result<Vec<u8>, ModelProfileStoreError> {
    let mut copy = snapshot.clone();
    copy.snapshot_identity.clear();
    serde_json::to_vec(&copy).map_err(|_| error("model-profile-snapshot-serialize"))
}

fn validate_project_id(project_id: &str) -> Result<(), ModelProfileStoreError> {
    if project_id.is_empty()
        || project_id.len() > 128
        || project_id
            .bytes()
            .any(|byte| !(byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.')))
    {
        return Err(error("model-profile-project-invalid"));
    }
    Ok(())
}

fn read_limited(path: &Path, limit: usize) -> Result<Vec<u8>, ModelProfileStoreError> {
    let metadata = fs::symlink_metadata(path).map_err(|_| error("model-profile-snapshot-read"))?;
    if metadata.file_type().is_symlink() || !metadata.is_file() || metadata.len() > limit as u64 {
        return Err(error("model-profile-snapshot-size-or-layout"));
    }
    let mut file = File::open(path).map_err(|_| error("model-profile-snapshot-read"))?;
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    file.read_to_end(&mut bytes)
        .map_err(|_| error("model-profile-snapshot-read"))?;
    if bytes.len() > limit {
        return Err(error("model-profile-snapshot-size-or-layout"));
    }
    Ok(bytes)
}

fn create_secure_directory(path: &Path) -> Result<(), ModelProfileStoreError> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() || !metadata.is_dir() => {
            Err(error("model-profile-directory-unsafe"))
        }
        Ok(_) => secure_directory(path),
        Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => {
            fs::create_dir(path).map_err(|_| error("model-profile-directory-create"))?;
            secure_directory(path)
        }
        Err(_) => Err(error("model-profile-directory-read")),
    }
}

fn secure_directory(path: &Path) -> Result<(), ModelProfileStoreError> {
    #[cfg(not(unix))]
    let _ = path;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(path, fs::Permissions::from_mode(0o700))
            .map_err(|_| error("model-profile-directory-permission"))?;
    }
    Ok(())
}

fn secure_file(file: &File) -> Result<(), ModelProfileStoreError> {
    #[cfg(not(unix))]
    let _ = file;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        file.set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(|_| error("model-profile-file-permission"))?;
    }
    Ok(())
}

fn sync_directory(path: &Path) -> Result<(), ModelProfileStoreError> {
    #[cfg(not(unix))]
    let _ = path;
    #[cfg(unix)]
    File::open(path)
        .and_then(|directory| directory.sync_all())
        .map_err(|_| error("model-profile-directory-sync"))?;
    Ok(())
}

#[cfg(unix)]
fn replace_file(source: &Path, target: &Path) -> Result<(), ModelProfileStoreError> {
    fs::rename(source, target).map_err(|_| error("model-profile-snapshot-commit"))
}

#[cfg(target_os = "windows")]
fn replace_file(source: &Path, target: &Path) -> Result<(), ModelProfileStoreError> {
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
    let moved = unsafe {
        MoveFileExW(
            source.as_ptr(),
            target.as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    };
    if moved == 0 {
        return Err(error("model-profile-snapshot-commit"));
    }
    Ok(())
}

#[cfg(not(any(unix, target_os = "windows")))]
fn replace_file(source: &Path, target: &Path) -> Result<(), ModelProfileStoreError> {
    if target.exists() {
        return Err(error("model-profile-snapshot-replace-unsupported"));
    }
    fs::rename(source, target).map_err(|_| error("model-profile-snapshot-commit"))
}

fn error(code: &'static str) -> ModelProfileStoreError {
    ModelProfileStoreError { code }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn root() -> PathBuf {
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "aegisy-model-profile-store-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(&root).unwrap();
        root
    }

    fn profile(
        scope: ProfileScope,
        project_id: Option<String>,
        profile_id: &str,
        revision: u64,
    ) -> ModelProfile {
        let mut profile =
            ModelProfile::single_model(profile_id, scope, project_id, "provider:model", "test")
                .unwrap();
        profile.revision = revision;
        profile
    }

    #[test]
    fn creates_global_and_project_profiles_and_reopens_them() {
        let root = root();
        let mut store = ModelProfileStore::open(&root).unwrap();
        let (_, global) = store
            .save(profile(ProfileScope::Global, None, "global", 0), None)
            .unwrap();
        let (_, project) = store
            .save(
                profile(
                    ProfileScope::Project,
                    Some("project-1".into()),
                    "project",
                    0,
                ),
                None,
            )
            .unwrap();
        assert_eq!(global.revision, 0);
        assert_eq!(project.project_id.as_deref(), Some("project-1"));
        let reopened = ModelProfileStore::open(&root).unwrap();
        assert_eq!(reopened.global().unwrap().unwrap().profile_id, "global");
        assert_eq!(
            reopened.project("project-1").unwrap().unwrap().profile_id,
            "project"
        );
        assert_eq!(reopened.identity().unwrap(), store.identity().unwrap());
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn revision_cas_and_idempotent_retry_preserve_generation() {
        let root = root();
        let mut store = ModelProfileStore::open(&root).unwrap();
        let first = profile(ProfileScope::Global, None, "global", 0);
        let (_, descriptor) = store.save(first.clone(), None).unwrap();
        let generation = descriptor.generation;
        let (_, same) = store.save(first, Some(0)).unwrap();
        assert_eq!(same.generation, generation);

        let stale = profile(ProfileScope::Global, None, "global", 1);
        let error = store.save(stale.clone(), None).unwrap_err();
        assert_eq!(error.code, "model-profile-cas-mismatch");

        let mut changed = stale;
        changed.name = "Updated".into();
        let (_, updated) = store.save(changed, Some(0)).unwrap();
        assert_eq!(updated.revision, 1);
        assert!(updated.generation > generation);
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn remove_requires_exact_revision_and_is_durable() {
        let root = root();
        let mut store = ModelProfileStore::open(&root).unwrap();
        store
            .save(
                profile(
                    ProfileScope::Project,
                    Some("project-1".into()),
                    "project",
                    0,
                ),
                None,
            )
            .unwrap();
        let error = store
            .remove(ProfileScope::Project, Some("project-1"), 1)
            .unwrap_err();
        assert_eq!(error.code, "model-profile-cas-mismatch");
        store
            .remove(ProfileScope::Project, Some("project-1"), 0)
            .unwrap();
        assert!(store.project("project-1").unwrap().is_none());
        assert!(ModelProfileStore::open(&root)
            .unwrap()
            .project("project-1")
            .unwrap()
            .is_none());
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn rejects_duplicate_profile_ids_and_secret_metadata() {
        let root = root();
        let mut store = ModelProfileStore::open(&root).unwrap();
        store
            .save(profile(ProfileScope::Global, None, "same", 0), None)
            .unwrap();
        let duplicate = profile(ProfileScope::Project, Some("project-1".into()), "same", 0);
        let error = store.save(duplicate, None).unwrap_err();
        assert_eq!(error.code, "model-profile-id-duplicate");

        let mut secret = profile(ProfileScope::Project, Some("project-2".into()), "secret", 0);
        secret.name = "token=sk-secret".into();
        let error = store.save(secret, None).unwrap_err();
        assert_eq!(error.code, "model-profile-invalid");
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn snapshot_tampering_fails_reopen_without_repair() {
        let root = root();
        let mut store = ModelProfileStore::open(&root).unwrap();
        store
            .save(profile(ProfileScope::Global, None, "global", 0), None)
            .unwrap();
        let snapshot_path = root.join("model-profiles-v1").join("snapshot.json");
        let mut snapshot: ModelProfileStoreSnapshot =
            serde_json::from_slice(&fs::read(&snapshot_path).unwrap()).unwrap();
        snapshot.global.as_mut().unwrap().name = "Tampered".into();
        fs::write(&snapshot_path, serde_json::to_vec(&snapshot).unwrap()).unwrap();
        let error = ModelProfileStore::open(&root).unwrap_err();
        assert_eq!(error.code, "model-profile-snapshot-identity-mismatch");
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn invalid_scope_and_project_ids_fail_before_writing() {
        let root = root();
        let mut store = ModelProfileStore::open(&root).unwrap();
        let mut invalid = profile(ProfileScope::Project, Some("project-1".into()), "bad", 0);
        invalid.project_id = Some("../outside".into());
        let error = store.save(invalid, None).unwrap_err();
        assert_eq!(error.code, "model-profile-invalid");
        let error = store.project("../outside").unwrap_err();
        assert_eq!(error.code, "model-profile-project-invalid");
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn private_snapshot_is_bounded_and_identity_is_content_addressed() {
        let root = root();
        let mut store = ModelProfileStore::open(&root).unwrap();
        store
            .save(profile(ProfileScope::Global, None, "global", 0), None)
            .unwrap();
        let snapshot = store.snapshot().unwrap();
        assert!(snapshot
            .snapshot_identity
            .starts_with("model-profile-store:sha256:"));
        let metadata = fs::metadata(root.join("model-profiles-v1").join("snapshot.json")).unwrap();
        assert!(metadata.len() < MAX_SNAPSHOT_BYTES as u64);
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(metadata.permissions().mode() & 0o777, 0o600);
        }
        let _ = fs::remove_dir_all(root);
    }
}
