use crate::session_compaction::{activate_review, CompactionCheckpointReview, CompactionError};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

pub const STORE_SCHEMA_VERSION: &str = "session-compaction-checkpoint-store/0.1";
const MAX_CHECKPOINT_BYTES: u64 = 96 * 1024;
const MAX_CHECKPOINTS: usize = 256;
const MAX_OBJECTS: usize = 512;
const MAX_STORE_BYTES: u64 = 32 * 1024 * 1024;
static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CompactionCheckpointDescriptor {
    pub schema_version: String,
    pub checkpoint_id: String,
    pub session_id: String,
    pub review_id: String,
    pub object_reference: String,
    pub state: String,
    pub original_event_history_authoritative: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredCompactionCheckpoint {
    pub schema_version: String,
    pub review: CompactionCheckpointReview,
    pub original_event_history_authoritative: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CompactionCheckpointStoreError {
    pub message: String,
}

pub struct CompactionCheckpointStore {
    root: PathBuf,
}

impl CompactionCheckpointStore {
    pub fn open(data_root: &Path) -> Result<Self, CompactionCheckpointStoreError> {
        let metadata = fs::symlink_metadata(data_root)
            .map_err(|_| error("compaction data root is unavailable"))?;
        if metadata.file_type().is_symlink() || !metadata.is_dir() {
            return Err(error("compaction data root is unsafe"));
        }
        let data_root = data_root
            .canonicalize()
            .map_err(|_| error("compaction data root is unavailable"))?;
        let root = data_root.join("session-compaction-checkpoints-v1");
        create_secure_directory(&root)?;
        create_secure_directory(&root.join("objects"))?;
        create_secure_directory(&root.join("pointers"))?;
        let canonical = root
            .canonicalize()
            .map_err(|_| error("compaction checkpoint store is unavailable"))?;
        if canonical != root {
            return Err(error("compaction checkpoint store traverses a symlink"));
        }
        Ok(Self { root: canonical })
    }

    pub fn persist(
        &self,
        review: &CompactionCheckpointReview,
    ) -> Result<CompactionCheckpointDescriptor, CompactionCheckpointStoreError> {
        validate_review(review)?;
        self.validate_layout()?;
        let stored = StoredCompactionCheckpoint {
            schema_version: STORE_SCHEMA_VERSION.into(),
            review: review.clone(),
            original_event_history_authoritative: true,
        };
        let bytes = serde_json::to_vec(&stored)
            .map_err(|_| error("cannot serialize compaction checkpoint"))?;
        if bytes.len() as u64 > MAX_CHECKPOINT_BYTES {
            return Err(error("compaction checkpoint exceeds size limit"));
        }
        let object_hash = sha256_hex(&bytes);
        let pointer_name = pointer_name(&review.session_id, &review.checkpoint_id);
        let pointer_path = self.root.join("pointers").join(&pointer_name);
        match fs::symlink_metadata(&pointer_path) {
            Ok(_) => {
                let existing_hash = self.read_pointer(&pointer_name)?;
                let existing = self.read_object(&existing_hash)?;
                if existing_hash == object_hash && existing == bytes {
                    return Ok(descriptor(review, &object_hash));
                }
                return Err(error("compaction checkpoint ID already exists"));
            }
            Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => {}
            Err(_) => return Err(error("compaction checkpoint pointer is unavailable")),
        }
        self.enforce_store_limits()?;
        self.write_object(&object_hash, &bytes)?;
        self.write_pointer(&pointer_name, &object_hash, &bytes)?;
        Ok(descriptor(review, &object_hash))
    }

    pub fn load(
        &self,
        session_id: &str,
        checkpoint_id: &str,
    ) -> Result<StoredCompactionCheckpoint, CompactionCheckpointStoreError> {
        self.load_with_descriptor(session_id, checkpoint_id)
            .map(|(stored, _)| stored)
    }

    pub fn load_with_descriptor(
        &self,
        session_id: &str,
        checkpoint_id: &str,
    ) -> Result<
        (StoredCompactionCheckpoint, CompactionCheckpointDescriptor),
        CompactionCheckpointStoreError,
    > {
        validate_lookup_id(session_id, "compaction session ID")?;
        validate_lookup_id(checkpoint_id, "compaction checkpoint ID")?;
        self.validate_layout()?;
        let object_hash = self.read_pointer(&pointer_name(session_id, checkpoint_id))?;
        let bytes = self.read_object(&object_hash)?;
        if sha256_hex(&bytes) != object_hash {
            return Err(error("compaction checkpoint object hash mismatch"));
        }
        let stored: StoredCompactionCheckpoint = serde_json::from_slice(&bytes)
            .map_err(|_| error("compaction checkpoint object is invalid"))?;
        if stored.schema_version != STORE_SCHEMA_VERSION
            || !stored.original_event_history_authoritative
            || stored.review.session_id != session_id
            || stored.review.checkpoint_id != checkpoint_id
        {
            return Err(error("compaction checkpoint identity is invalid"));
        }
        validate_review(&stored.review)?;
        let descriptor = descriptor(&stored.review, &object_hash);
        Ok((stored, descriptor))
    }

    pub fn load_optional_with_descriptor(
        &self,
        session_id: &str,
        checkpoint_id: &str,
    ) -> Result<
        Option<(StoredCompactionCheckpoint, CompactionCheckpointDescriptor)>,
        CompactionCheckpointStoreError,
    > {
        validate_lookup_id(session_id, "compaction session ID")?;
        validate_lookup_id(checkpoint_id, "compaction checkpoint ID")?;
        self.validate_layout()?;
        let pointer = self
            .root
            .join("pointers")
            .join(pointer_name(session_id, checkpoint_id));
        match fs::symlink_metadata(pointer) {
            Ok(_) => self
                .load_with_descriptor(session_id, checkpoint_id)
                .map(Some),
            Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(_) => Err(error("compaction checkpoint pointer is unavailable")),
        }
    }

    fn validate_layout(&self) -> Result<(), CompactionCheckpointStoreError> {
        if self.root.canonicalize().ok().as_deref() != Some(self.root.as_path()) {
            return Err(error("compaction checkpoint store identity changed"));
        }
        for directory in ["objects", "pointers"] {
            let path = self.root.join(directory);
            let metadata = fs::symlink_metadata(&path)
                .map_err(|_| error("compaction checkpoint directory is unavailable"))?;
            if metadata.file_type().is_symlink() || !metadata.is_dir() {
                return Err(error("compaction checkpoint directory is unsafe"));
            }
        }
        Ok(())
    }

    fn enforce_store_limits(&self) -> Result<(), CompactionCheckpointStoreError> {
        let pointer_count = bounded_directory_count(&self.root.join("pointers"), MAX_CHECKPOINTS)?;
        if pointer_count >= MAX_CHECKPOINTS {
            return Err(error("compaction checkpoint count limit exceeded"));
        }
        let object_count = bounded_directory_count(&self.root.join("objects"), MAX_OBJECTS)?;
        if object_count >= MAX_OBJECTS {
            return Err(error("compaction checkpoint object limit exceeded"));
        }
        let mut bytes = 0_u64;
        for entry in fs::read_dir(self.root.join("objects"))
            .map_err(|_| error("cannot enumerate compaction checkpoint objects"))?
            .take(MAX_OBJECTS + 1)
        {
            let entry = entry.map_err(|_| error("cannot inspect compaction checkpoint object"))?;
            let metadata = fs::symlink_metadata(entry.path())
                .map_err(|_| error("cannot inspect compaction checkpoint object"))?;
            if !metadata.is_file() || metadata.file_type().is_symlink() {
                return Err(error("compaction checkpoint object layout is unsafe"));
            }
            bytes = bytes
                .checked_add(metadata.len())
                .ok_or_else(|| error("compaction checkpoint store size overflow"))?;
        }
        if bytes > MAX_STORE_BYTES {
            return Err(error("compaction checkpoint store size limit exceeded"));
        }
        Ok(())
    }

    fn write_object(
        &self,
        object_hash: &str,
        bytes: &[u8],
    ) -> Result<(), CompactionCheckpointStoreError> {
        let parent = self.root.join("objects");
        let target = parent.join(object_hash);
        if let Ok(existing) = self.read_object(object_hash) {
            if existing == bytes {
                return Ok(());
            }
            return Err(error("compaction checkpoint object hash collision"));
        }
        let temporary = temporary_path(&parent, "object");
        let result = (|| {
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&temporary)
                .map_err(|_| error("cannot create compaction checkpoint stage"))?;
            secure_file(&file)?;
            file.write_all(bytes)
                .and_then(|()| file.sync_all())
                .map_err(|_| error("cannot flush compaction checkpoint stage"))?;
            match fs::hard_link(&temporary, &target) {
                Ok(()) => Ok(()),
                Err(cause) if cause.kind() == std::io::ErrorKind::AlreadyExists => {
                    let existing = self.read_object(object_hash)?;
                    if existing == bytes {
                        Ok(())
                    } else {
                        Err(error("compaction checkpoint object changed"))
                    }
                }
                Err(_) => Err(error("cannot commit compaction checkpoint object")),
            }
        })();
        let _ = fs::remove_file(&temporary);
        result?;
        sync_directory(&parent)
    }

    fn read_object(&self, object_hash: &str) -> Result<Vec<u8>, CompactionCheckpointStoreError> {
        if !valid_hash(object_hash) {
            return Err(error("compaction checkpoint object name is invalid"));
        }
        let path = self.root.join("objects").join(object_hash);
        let metadata = fs::symlink_metadata(&path)
            .map_err(|_| error("compaction checkpoint object is unavailable"))?;
        if metadata.file_type().is_symlink()
            || !metadata.is_file()
            || metadata.len() > MAX_CHECKPOINT_BYTES
        {
            return Err(error(
                "compaction checkpoint object type or size is invalid",
            ));
        }
        let mut bytes = Vec::with_capacity(metadata.len() as usize);
        File::open(path)
            .and_then(|file| file.take(MAX_CHECKPOINT_BYTES + 1).read_to_end(&mut bytes))
            .map_err(|_| error("cannot read compaction checkpoint object"))?;
        if bytes.len() as u64 > MAX_CHECKPOINT_BYTES {
            return Err(error("compaction checkpoint object exceeds size limit"));
        }
        Ok(bytes)
    }

    fn write_pointer(
        &self,
        pointer_name: &str,
        object_hash: &str,
        expected_bytes: &[u8],
    ) -> Result<(), CompactionCheckpointStoreError> {
        let parent = self.root.join("pointers");
        let target = parent.join(pointer_name);
        if let Ok(existing_hash) = self.read_pointer(pointer_name) {
            let existing = self.read_object(&existing_hash)?;
            if existing_hash == object_hash && existing == expected_bytes {
                return Ok(());
            }
            return Err(error("compaction checkpoint ID already exists"));
        }
        let temporary = temporary_path(&parent, "pointer");
        let result = (|| {
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&temporary)
                .map_err(|_| error("cannot create compaction checkpoint pointer stage"))?;
            secure_file(&file)?;
            file.write_all(format!("{object_hash}\n").as_bytes())
                .and_then(|()| file.sync_all())
                .map_err(|_| error("cannot flush compaction checkpoint pointer"))?;
            fs::hard_link(&temporary, &target)
                .map_err(|_| error("cannot commit compaction checkpoint pointer"))
        })();
        let _ = fs::remove_file(&temporary);
        result?;
        sync_directory(&parent)
    }

    fn read_pointer(&self, pointer_name: &str) -> Result<String, CompactionCheckpointStoreError> {
        if !valid_hash(pointer_name) {
            return Err(error("compaction checkpoint pointer name is invalid"));
        }
        let path = self.root.join("pointers").join(pointer_name);
        let metadata = fs::symlink_metadata(&path)
            .map_err(|_| error("compaction checkpoint pointer is unavailable"))?;
        if metadata.file_type().is_symlink() || !metadata.is_file() || metadata.len() > 65 {
            return Err(error("compaction checkpoint pointer is invalid"));
        }
        let value = fs::read_to_string(path)
            .map_err(|_| error("cannot read compaction checkpoint pointer"))?;
        let object_hash = value.trim();
        if !valid_hash(object_hash) {
            return Err(error("compaction checkpoint pointer hash is invalid"));
        }
        Ok(object_hash.into())
    }
}

fn validate_review(
    review: &CompactionCheckpointReview,
) -> Result<(), CompactionCheckpointStoreError> {
    activate_review(review, review.through_sequence, &review.source_context_hash)
        .map(|_| ())
        .map_err(|CompactionError { .. }| error("compaction checkpoint review is invalid"))
}

fn descriptor(
    review: &CompactionCheckpointReview,
    object_hash: &str,
) -> CompactionCheckpointDescriptor {
    CompactionCheckpointDescriptor {
        schema_version: STORE_SCHEMA_VERSION.into(),
        checkpoint_id: review.checkpoint_id.clone(),
        session_id: review.session_id.clone(),
        review_id: review.review_id.clone(),
        object_reference: format!("session-compaction-checkpoint:sha256:{object_hash}"),
        state: "review-persisted".into(),
        original_event_history_authoritative: true,
    }
}

fn validate_lookup_id(value: &str, label: &str) -> Result<(), CompactionCheckpointStoreError> {
    if value.is_empty() || value.len() > 256 || value.chars().any(char::is_control) {
        return Err(error(format!("{label} is invalid")));
    }
    Ok(())
}

fn pointer_name(session_id: &str, checkpoint_id: &str) -> String {
    let mut hasher = Sha256::new();
    hasher.update(session_id.as_bytes());
    hasher.update([0]);
    hasher.update(checkpoint_id.as_bytes());
    format!("{:x}", hasher.finalize())
}

fn bounded_directory_count(
    path: &Path,
    limit: usize,
) -> Result<usize, CompactionCheckpointStoreError> {
    Ok(fs::read_dir(path)
        .map_err(|_| error("cannot enumerate compaction checkpoint directory"))?
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
    let mut hasher = Sha256::new();
    hasher.update(bytes);
    format!("{:x}", hasher.finalize())
}

fn temporary_path(parent: &Path, kind: &str) -> PathBuf {
    let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    parent.join(format!(
        ".aegisy-session-compaction-{}-{sequence}.{kind}",
        std::process::id()
    ))
}

fn create_secure_directory(path: &Path) -> Result<(), CompactionCheckpointStoreError> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() || !metadata.is_dir() => {
            return Err(error("compaction checkpoint directory is unsafe"));
        }
        Ok(_) => {}
        Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => {
            fs::create_dir(path)
                .map_err(|_| error("cannot create compaction checkpoint directory"))?;
        }
        Err(_) => return Err(error("compaction checkpoint directory is unavailable")),
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(path, fs::Permissions::from_mode(0o700))
            .map_err(|_| error("cannot secure compaction checkpoint directory"))?;
    }
    Ok(())
}

fn secure_file(file: &File) -> Result<(), CompactionCheckpointStoreError> {
    #[cfg(not(unix))]
    let _ = file;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        file.set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(|_| error("cannot secure compaction checkpoint file"))?;
    }
    Ok(())
}

fn sync_directory(path: &Path) -> Result<(), CompactionCheckpointStoreError> {
    #[cfg(not(unix))]
    let _ = path;
    #[cfg(unix)]
    File::open(path)
        .and_then(|directory| directory.sync_all())
        .map_err(|_| error("cannot sync compaction checkpoint directory"))?;
    Ok(())
}

fn error(message: impl Into<String>) -> CompactionCheckpointStoreError {
    CompactionCheckpointStoreError {
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::session_compaction::{create_review, CompactionSummary};

    fn root() -> PathBuf {
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "aegisy-session-compaction-store-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir(&root).unwrap();
        root
    }

    fn review(checkpoint_id: &str) -> CompactionCheckpointReview {
        create_review(
            checkpoint_id,
            "session-1",
            7,
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            Some("Preserve unresolved work"),
            CompactionSummary {
                decisions: vec!["Keep event history authoritative".into()],
                unresolved_tasks: vec!["Integrate AAP review".into()],
                next_actions: vec!["Add durable events".into()],
                ..Default::default()
            },
        )
        .unwrap()
    }

    #[test]
    fn persists_and_reloads_exact_review_after_reopen() {
        let root = root();
        let expected = review("checkpoint-1");
        let descriptor = CompactionCheckpointStore::open(&root)
            .unwrap()
            .persist(&expected)
            .unwrap();
        assert_eq!(descriptor.state, "review-persisted");
        assert!(descriptor.original_event_history_authoritative);

        let stored = CompactionCheckpointStore::open(&root)
            .unwrap()
            .load("session-1", "checkpoint-1")
            .unwrap();
        assert_eq!(stored.review, expected);
        assert!(stored.original_event_history_authoritative);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn duplicate_checkpoint_is_idempotent_only_for_identical_content() {
        let root = root();
        let store = CompactionCheckpointStore::open(&root).unwrap();
        let expected = review("checkpoint-1");
        assert_eq!(
            store.persist(&expected).unwrap(),
            store.persist(&expected).unwrap()
        );

        let mut changed = review("checkpoint-1");
        changed
            .summary
            .next_actions
            .push("Different content".into());
        changed = create_review(
            "checkpoint-1",
            "session-1",
            changed.through_sequence,
            &changed.source_context_hash,
            changed.preservation_instructions.as_deref(),
            changed.summary,
        )
        .unwrap();
        assert_eq!(
            store.persist(&changed).unwrap_err().message,
            "compaction checkpoint ID already exists"
        );
        assert_eq!(fs::read_dir(store.root.join("objects")).unwrap().count(), 1);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn tampered_object_is_rejected_without_rewriting_it() {
        let root = root();
        let store = CompactionCheckpointStore::open(&root).unwrap();
        store.persist(&review("checkpoint-1")).unwrap();
        let pointer = pointer_name("session-1", "checkpoint-1");
        let object_hash = store.read_pointer(&pointer).unwrap();
        let object_path = store.root.join("objects").join(&object_hash);
        fs::write(&object_path, b"tampered").unwrap();
        assert_eq!(
            store.load("session-1", "checkpoint-1").unwrap_err().message,
            "compaction checkpoint object hash mismatch"
        );
        assert_eq!(fs::read(object_path).unwrap(), b"tampered");
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn forged_review_and_unsafe_lookup_are_rejected() {
        let root = root();
        let store = CompactionCheckpointStore::open(&root).unwrap();
        let mut forged = review("checkpoint-1");
        forged.review_id = "compaction-review:sha256:forged".into();
        assert_eq!(
            store.persist(&forged).unwrap_err().message,
            "compaction checkpoint review is invalid"
        );
        assert!(store.load("session-1", "../checkpoint").is_err());
        fs::remove_dir_all(root).unwrap();
    }
}
