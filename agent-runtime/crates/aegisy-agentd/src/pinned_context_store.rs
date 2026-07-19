use crate::pinned_context::{PinnedContextSet, SCHEMA_VERSION};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

pub const STORE_SCHEMA_VERSION: &str = "pinned-context-store/0.1";
const MAX_SET_BYTES: u64 = 1024 * 1024;
const MAX_PROJECTS: usize = 1_024;
const MAX_OBJECTS: usize = 4_096;
const MAX_STORE_BYTES: u64 = 256 * 1024 * 1024;
static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PinnedContextStoreDescriptor {
    pub schema_version: String,
    pub project_id: String,
    pub set_identity: String,
    pub object_reference: String,
    pub item_count: usize,
    pub content_bodies_persisted: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredPinnedContextSet {
    pub schema_version: String,
    pub set: PinnedContextSet,
    pub content_bodies_persisted: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PinnedContextStoreError {
    pub code: &'static str,
}

pub struct PinnedContextStore {
    root: PathBuf,
}

impl PinnedContextStore {
    pub fn open(data_root: &Path) -> Result<Self, PinnedContextStoreError> {
        let metadata = fs::symlink_metadata(data_root)
            .map_err(|_| error("pinned-context-data-root-unavailable"))?;
        if metadata.file_type().is_symlink() || !metadata.is_dir() {
            return Err(error("pinned-context-data-root-unsafe"));
        }
        let data_root = data_root
            .canonicalize()
            .map_err(|_| error("pinned-context-data-root-unavailable"))?;
        let root = data_root.join("pinned-context-v1");
        create_secure_directory(&root)?;
        create_secure_directory(&root.join("objects"))?;
        create_secure_directory(&root.join("pointers"))?;
        let canonical = root
            .canonicalize()
            .map_err(|_| error("pinned-context-store-unavailable"))?;
        if canonical != root {
            return Err(error("pinned-context-store-symlink"));
        }
        Ok(Self { root: canonical })
    }

    pub fn persist(
        &self,
        set: &PinnedContextSet,
    ) -> Result<PinnedContextStoreDescriptor, PinnedContextStoreError> {
        set.validate()
            .map_err(|_| error("pinned-context-set-invalid"))?;
        self.validate_layout()?;
        let stored = StoredPinnedContextSet {
            schema_version: STORE_SCHEMA_VERSION.into(),
            set: set.clone(),
            content_bodies_persisted: false,
        };
        let bytes = serde_json::to_vec(&stored).map_err(|_| error("pinned-context-serialize"))?;
        if bytes.len() as u64 > MAX_SET_BYTES {
            return Err(error("pinned-context-object-too-large"));
        }
        let object_hash = sha256_hex(&bytes);
        let pointer_name = pointer_name(&set.project_id);
        let current_hash = self.read_optional_pointer(&pointer_name)?;
        if current_hash.is_some() {
            self.load(&set.project_id)?;
        }
        if current_hash.as_deref() == Some(object_hash.as_str()) {
            let existing = self.read_object(&object_hash)?;
            if existing == bytes {
                return descriptor(set, &object_hash);
            }
            return Err(error("pinned-context-object-changed"));
        }
        let object_exists = fs::symlink_metadata(self.root.join("objects").join(&object_hash))
            .map(|metadata| metadata.is_file() && !metadata.file_type().is_symlink())
            .unwrap_or(false);
        self.enforce_store_limits(current_hash.is_none(), !object_exists, bytes.len() as u64)?;
        self.write_object(&object_hash, &bytes)?;
        self.write_pointer(&pointer_name, &object_hash)?;
        descriptor(set, &object_hash)
    }

    pub fn load(
        &self,
        project_id: &str,
    ) -> Result<(PinnedContextSet, PinnedContextStoreDescriptor), PinnedContextStoreError> {
        validate_project_id(project_id)?;
        self.validate_layout()?;
        let object_hash = self
            .read_optional_pointer(&pointer_name(project_id))?
            .ok_or_else(|| error("pinned-context-pointer-missing"))?;
        let bytes = self.read_object(&object_hash)?;
        if sha256_hex(&bytes) != object_hash {
            return Err(error("pinned-context-object-hash-mismatch"));
        }
        let stored: StoredPinnedContextSet =
            serde_json::from_slice(&bytes).map_err(|_| error("pinned-context-object-invalid"))?;
        if stored.schema_version != STORE_SCHEMA_VERSION
            || stored.set.schema_version != SCHEMA_VERSION
            || stored.set.project_id != project_id
            || stored.content_bodies_persisted
        {
            return Err(error("pinned-context-object-identity-invalid"));
        }
        stored
            .set
            .validate()
            .map_err(|_| error("pinned-context-set-invalid"))?;
        let descriptor = descriptor(&stored.set, &object_hash)?;
        Ok((stored.set, descriptor))
    }

    pub fn load_optional(
        &self,
        project_id: &str,
    ) -> Result<Option<(PinnedContextSet, PinnedContextStoreDescriptor)>, PinnedContextStoreError>
    {
        validate_project_id(project_id)?;
        self.validate_layout()?;
        if self
            .read_optional_pointer(&pointer_name(project_id))?
            .is_none()
        {
            return Ok(None);
        }
        self.load(project_id).map(Some)
    }

    fn validate_layout(&self) -> Result<(), PinnedContextStoreError> {
        if self.root.canonicalize().ok().as_deref() != Some(self.root.as_path()) {
            return Err(error("pinned-context-store-identity-changed"));
        }
        for directory in ["objects", "pointers"] {
            let path = self.root.join(directory);
            let metadata = fs::symlink_metadata(path)
                .map_err(|_| error("pinned-context-directory-unavailable"))?;
            if metadata.file_type().is_symlink() || !metadata.is_dir() {
                return Err(error("pinned-context-directory-unsafe"));
            }
        }
        Ok(())
    }

    fn enforce_store_limits(
        &self,
        new_pointer: bool,
        new_object: bool,
        object_bytes: u64,
    ) -> Result<(), PinnedContextStoreError> {
        let pointer_count = bounded_directory_count(&self.root.join("pointers"), MAX_PROJECTS)?;
        if pointer_count > MAX_PROJECTS || (new_pointer && pointer_count >= MAX_PROJECTS) {
            return Err(error("pinned-context-project-limit"));
        }
        let object_count = bounded_directory_count(&self.root.join("objects"), MAX_OBJECTS)?;
        if object_count > MAX_OBJECTS || (new_object && object_count >= MAX_OBJECTS) {
            return Err(error("pinned-context-object-limit"));
        }
        let mut bytes = 0_u64;
        for entry in fs::read_dir(self.root.join("objects"))
            .map_err(|_| error("pinned-context-object-enumeration-failed"))?
            .take(MAX_OBJECTS + 1)
        {
            let entry = entry.map_err(|_| error("pinned-context-object-inspection-failed"))?;
            let metadata = fs::symlink_metadata(entry.path())
                .map_err(|_| error("pinned-context-object-inspection-failed"))?;
            if !metadata.is_file() || metadata.file_type().is_symlink() {
                return Err(error("pinned-context-object-layout-unsafe"));
            }
            bytes = bytes
                .checked_add(metadata.len())
                .ok_or_else(|| error("pinned-context-store-size-overflow"))?;
        }
        if bytes.saturating_add(if new_object { object_bytes } else { 0 }) > MAX_STORE_BYTES {
            return Err(error("pinned-context-store-size-limit"));
        }
        Ok(())
    }

    fn write_object(&self, object_hash: &str, bytes: &[u8]) -> Result<(), PinnedContextStoreError> {
        let parent = self.root.join("objects");
        let target = parent.join(object_hash);
        if let Ok(existing) = self.read_object(object_hash) {
            if existing == bytes {
                return Ok(());
            }
            return Err(error("pinned-context-object-collision"));
        }
        let temporary = temporary_path(&parent, "object");
        let result = (|| {
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&temporary)
                .map_err(|_| error("pinned-context-object-stage-failed"))?;
            secure_file(&file)?;
            file.write_all(bytes)
                .and_then(|()| file.sync_all())
                .map_err(|_| error("pinned-context-object-flush-failed"))?;
            match fs::hard_link(&temporary, &target) {
                Ok(()) => Ok(()),
                Err(cause) if cause.kind() == std::io::ErrorKind::AlreadyExists => {
                    let existing = self.read_object(object_hash)?;
                    if existing == bytes {
                        Ok(())
                    } else {
                        Err(error("pinned-context-object-changed"))
                    }
                }
                Err(_) => Err(error("pinned-context-object-commit-failed")),
            }
        })();
        let _ = fs::remove_file(&temporary);
        result?;
        sync_directory(&parent)
    }

    fn read_object(&self, object_hash: &str) -> Result<Vec<u8>, PinnedContextStoreError> {
        if !valid_hash(object_hash) {
            return Err(error("pinned-context-object-name-invalid"));
        }
        let path = self.root.join("objects").join(object_hash);
        let metadata =
            fs::symlink_metadata(&path).map_err(|_| error("pinned-context-object-unavailable"))?;
        if metadata.file_type().is_symlink()
            || !metadata.is_file()
            || metadata.len() > MAX_SET_BYTES
        {
            return Err(error("pinned-context-object-type-invalid"));
        }
        let mut bytes = Vec::with_capacity(metadata.len() as usize);
        File::open(path)
            .and_then(|file| file.take(MAX_SET_BYTES + 1).read_to_end(&mut bytes))
            .map_err(|_| error("pinned-context-object-read-failed"))?;
        if bytes.len() as u64 > MAX_SET_BYTES {
            return Err(error("pinned-context-object-too-large"));
        }
        Ok(bytes)
    }

    fn write_pointer(
        &self,
        pointer_name: &str,
        object_hash: &str,
    ) -> Result<(), PinnedContextStoreError> {
        if !valid_hash(pointer_name) || !valid_hash(object_hash) {
            return Err(error("pinned-context-pointer-invalid"));
        }
        let parent = self.root.join("pointers");
        let target = parent.join(pointer_name);
        if let Ok(metadata) = fs::symlink_metadata(&target) {
            if metadata.file_type().is_symlink() || !metadata.is_file() {
                return Err(error("pinned-context-pointer-unsafe"));
            }
        }
        let temporary = temporary_path(&parent, "pointer");
        let result = (|| {
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&temporary)
                .map_err(|_| error("pinned-context-pointer-stage-failed"))?;
            secure_file(&file)?;
            file.write_all(format!("{object_hash}\n").as_bytes())
                .and_then(|()| file.sync_all())
                .map_err(|_| error("pinned-context-pointer-flush-failed"))?;
            replace_file(&temporary, &target)
        })();
        if result.is_err() {
            let _ = fs::remove_file(&temporary);
        }
        result?;
        sync_directory(&parent)
    }

    fn read_optional_pointer(
        &self,
        pointer_name: &str,
    ) -> Result<Option<String>, PinnedContextStoreError> {
        if !valid_hash(pointer_name) {
            return Err(error("pinned-context-pointer-name-invalid"));
        }
        let path = self.root.join("pointers").join(pointer_name);
        let metadata = match fs::symlink_metadata(&path) {
            Ok(metadata) => metadata,
            Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => return Ok(None),
            Err(_) => return Err(error("pinned-context-pointer-unavailable")),
        };
        if metadata.file_type().is_symlink() || !metadata.is_file() || metadata.len() > 65 {
            return Err(error("pinned-context-pointer-unsafe"));
        }
        let value =
            fs::read_to_string(path).map_err(|_| error("pinned-context-pointer-read-failed"))?;
        let object_hash = value.trim();
        if !valid_hash(object_hash) {
            return Err(error("pinned-context-pointer-hash-invalid"));
        }
        Ok(Some(object_hash.into()))
    }
}

fn descriptor(
    set: &PinnedContextSet,
    object_hash: &str,
) -> Result<PinnedContextStoreDescriptor, PinnedContextStoreError> {
    Ok(PinnedContextStoreDescriptor {
        schema_version: STORE_SCHEMA_VERSION.into(),
        project_id: set.project_id.clone(),
        set_identity: set
            .identity()
            .map_err(|_| error("pinned-context-set-invalid"))?,
        object_reference: format!("pinned-context-object:sha256:{object_hash}"),
        item_count: set.items.len(),
        content_bodies_persisted: false,
    })
}

fn validate_project_id(project_id: &str) -> Result<(), PinnedContextStoreError> {
    PinnedContextSet::new(project_id)
        .map(|_| ())
        .map_err(|_| error("pinned-context-project-invalid"))
}

fn pointer_name(project_id: &str) -> String {
    sha256_hex(project_id.as_bytes())
}

fn bounded_directory_count(path: &Path, limit: usize) -> Result<usize, PinnedContextStoreError> {
    Ok(fs::read_dir(path)
        .map_err(|_| error("pinned-context-directory-enumeration-failed"))?
        .take(limit + 1)
        .count())
}

fn valid_hash(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
}

fn sha256_hex(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

fn temporary_path(parent: &Path, kind: &str) -> PathBuf {
    let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    parent.join(format!(
        ".aegisy-pinned-context-{}-{sequence}.{kind}",
        std::process::id()
    ))
}

fn create_secure_directory(path: &Path) -> Result<(), PinnedContextStoreError> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() || !metadata.is_dir() => {
            return Err(error("pinned-context-directory-unsafe"));
        }
        Ok(_) => {}
        Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => {
            fs::create_dir(path).map_err(|_| error("pinned-context-directory-create-failed"))?;
        }
        Err(_) => return Err(error("pinned-context-directory-unavailable")),
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(path, fs::Permissions::from_mode(0o700))
            .map_err(|_| error("pinned-context-directory-permission-failed"))?;
    }
    Ok(())
}

fn secure_file(file: &File) -> Result<(), PinnedContextStoreError> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        file.set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(|_| error("pinned-context-file-permission-failed"))?;
    }
    Ok(())
}

fn sync_directory(path: &Path) -> Result<(), PinnedContextStoreError> {
    #[cfg(unix)]
    File::open(path)
        .and_then(|directory| directory.sync_all())
        .map_err(|_| error("pinned-context-directory-sync-failed"))?;
    Ok(())
}

#[cfg(unix)]
fn replace_file(source: &Path, target: &Path) -> Result<(), PinnedContextStoreError> {
    fs::rename(source, target).map_err(|_| error("pinned-context-pointer-commit-failed"))
}

#[cfg(target_os = "windows")]
fn replace_file(source: &Path, target: &Path) -> Result<(), PinnedContextStoreError> {
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
        return Err(error("pinned-context-pointer-commit-failed"));
    }
    Ok(())
}

#[cfg(not(any(unix, target_os = "windows")))]
fn replace_file(source: &Path, target: &Path) -> Result<(), PinnedContextStoreError> {
    if target.exists() {
        return Err(error("pinned-context-pointer-replace-unsupported"));
    }
    fs::rename(source, target).map_err(|_| error("pinned-context-pointer-commit-failed"))
}

fn error(code: &'static str) -> PinnedContextStoreError {
    PinnedContextStoreError { code }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pinned_context::PinnedContextItem;
    use std::collections::BTreeMap;

    fn root() -> PathBuf {
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "aegisy-pinned-context-store-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir(&root).unwrap();
        root
    }

    fn item(id: &str, reference: &str) -> PinnedContextItem {
        PinnedContextItem {
            id: id.into(),
            project_id: "project-1".into(),
            session_id: Some("session-1".into()),
            root_id: Some("root-1".into()),
            kind: "file".into(),
            source: "file-tree".into(),
            label: reference.into(),
            reference: reference.into(),
            content_hash: format!("sha256:{}", "a".repeat(64)),
            bytes: 32,
            revision: Some("revision-1".into()),
            freshness: "fresh".into(),
            priority: 850,
            metadata: BTreeMap::new(),
        }
    }

    fn set() -> PinnedContextSet {
        let mut set = PinnedContextSet::new("project-1").unwrap();
        set.add(item("pin-1", "src/main.rs")).unwrap();
        set
    }

    #[test]
    fn persists_updates_and_reloads_exact_metadata_after_reopen() {
        let root = root();
        let store = PinnedContextStore::open(&root).unwrap();
        let initial = set();
        let initial_descriptor = store.persist(&initial).unwrap();
        assert!(!initial_descriptor.content_bodies_persisted);
        assert_eq!(initial_descriptor.item_count, 1);

        let mut updated = initial.clone();
        updated.add(item("pin-2", "src/lib.rs")).unwrap();
        let updated_descriptor = store.persist(&updated).unwrap();
        assert_ne!(
            initial_descriptor.object_reference,
            updated_descriptor.object_reference
        );

        let (loaded, loaded_descriptor) = PinnedContextStore::open(&root)
            .unwrap()
            .load("project-1")
            .unwrap();
        assert_eq!(loaded, updated);
        assert_eq!(loaded_descriptor, updated_descriptor);
        assert_eq!(fs::read_dir(store.root.join("objects")).unwrap().count(), 2);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn identical_persist_is_idempotent_and_contains_no_body_field() {
        let root = root();
        let store = PinnedContextStore::open(&root).unwrap();
        let expected = set();
        assert_eq!(
            store.persist(&expected).unwrap(),
            store.persist(&expected).unwrap()
        );
        assert_eq!(fs::read_dir(store.root.join("objects")).unwrap().count(), 1);
        let object_hash = store
            .read_optional_pointer(&pointer_name("project-1"))
            .unwrap()
            .unwrap();
        let value: serde_json::Value =
            serde_json::from_slice(&store.read_object(&object_hash).unwrap()).unwrap();
        assert!(value.pointer("/set/items/0/content").is_none());
        assert_eq!(value["content_bodies_persisted"], false);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn tampered_object_and_pointer_are_rejected_without_rewrite() {
        let root = root();
        let store = PinnedContextStore::open(&root).unwrap();
        store.persist(&set()).unwrap();
        let pointer = pointer_name("project-1");
        let object_hash = store.read_optional_pointer(&pointer).unwrap().unwrap();
        let object_path = store.root.join("objects").join(&object_hash);
        fs::write(&object_path, b"tampered").unwrap();
        assert_eq!(
            store.load("project-1").unwrap_err().code,
            "pinned-context-object-hash-mismatch"
        );
        assert_eq!(fs::read(&object_path).unwrap(), b"tampered");
        let mut updated = set();
        updated.add(item("pin-2", "src/lib.rs")).unwrap();
        assert_eq!(
            store.persist(&updated).unwrap_err().code,
            "pinned-context-object-hash-mismatch"
        );
        assert_eq!(
            store.read_optional_pointer(&pointer).unwrap().as_deref(),
            Some(object_hash.as_str())
        );

        fs::write(store.root.join("pointers").join(pointer), b"invalid\n").unwrap();
        assert_eq!(
            store.load("project-1").unwrap_err().code,
            "pinned-context-pointer-hash-invalid"
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn symlinked_layout_is_rejected() {
        use std::os::unix::fs::symlink;
        let root = root();
        let outside = root.join("outside");
        fs::create_dir(&outside).unwrap();
        let store = PinnedContextStore::open(&root).unwrap();
        fs::remove_dir(store.root.join("objects")).unwrap();
        symlink(&outside, store.root.join("objects")).unwrap();
        assert_eq!(
            store.persist(&set()).unwrap_err().code,
            "pinned-context-directory-unsafe"
        );
        fs::remove_dir_all(root).unwrap();
    }
}
