use crate::pinned_context::{PinnedContextSet, SCHEMA_VERSION};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::UNIX_EPOCH;

pub const STORE_SCHEMA_VERSION: &str = "pinned-context-store/0.1";
pub const PUBLICATION_SCHEMA_VERSION: &str = "pinned-context-publication/0.1";
pub const ORPHAN_OBJECT_GRACE_MS: u64 = 24 * 60 * 60 * 1_000;
const MAX_SET_BYTES: u64 = 1024 * 1024;
const MAX_PROJECTS: usize = 1_024;
const MAX_OBJECTS: usize = 4_096;
const MAX_STORE_BYTES: u64 = 256 * 1024 * 1024;
const MAX_GC_ENTRIES: usize = 256;
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

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PinnedContextPublication {
    pub schema_version: String,
    pub publication_id: String,
    pub project_id: String,
    pub previous_set_identity: Option<String>,
    pub previous_object_reference: Option<String>,
    pub next_set_identity: String,
    pub next_object_reference: String,
    pub created_at_ms: u64,
    pub content_bodies_persisted: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PinnedContextObjectGcReport {
    pub schema_version: String,
    pub examined: u64,
    pub deleted: u64,
    pub deleted_bytes: u64,
    pub retained: u64,
    pub uncertain: u64,
    pub issues: Vec<String>,
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
        create_secure_directory(&root.join("publications"))?;
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
        let (descriptor, current_hash, object_hash, pointer_name) = self.persist_object(set)?;
        if current_hash.as_deref() != Some(object_hash.as_str()) {
            self.write_pointer(&pointer_name, &object_hash)?;
        }
        Ok(descriptor)
    }

    pub fn persist_publication(
        &self,
        set: &PinnedContextSet,
        created_at_ms: u64,
    ) -> Result<
        (
            PinnedContextStoreDescriptor,
            Option<PinnedContextPublication>,
        ),
        PinnedContextStoreError,
    > {
        let previous = self.load_optional(&set.project_id)?;
        let (descriptor, current_hash, object_hash, pointer_name) = self.persist_object(set)?;
        let pending = self.read_optional_publication(&pointer_name)?;
        if let Some(pending) = pending {
            let previous_set_identity = previous
                .as_ref()
                .map(|(_, descriptor)| descriptor.set_identity.as_str());
            let previous_object_reference = previous
                .as_ref()
                .map(|(_, descriptor)| descriptor.object_reference.as_str());
            if pending.previous_set_identity.as_deref() != previous_set_identity
                || pending.previous_object_reference.as_deref() != previous_object_reference
                || pending.next_set_identity != descriptor.set_identity
                || pending.next_object_reference != descriptor.object_reference
            {
                return Err(error("pinned-context-publication-identity-changed"));
            }
            if current_hash.as_deref() == Some(object_hash.as_str()) {
                return Err(error("pinned-context-publication-pending"));
            }
            let expected_previous_hash = pending
                .previous_object_reference
                .as_deref()
                .and_then(|value| value.strip_prefix("pinned-context-object:sha256:"));
            if current_hash.as_deref() != expected_previous_hash {
                return Err(error("pinned-context-publication-current-changed"));
            }
            self.write_pointer(&pointer_name, &object_hash)?;
            return Ok((descriptor, Some(pending)));
        }
        if current_hash.as_deref() == Some(object_hash.as_str()) {
            return Ok((descriptor, None));
        }
        let observed_hash = self.read_optional_pointer(&pointer_name)?;
        if observed_hash != current_hash {
            return Err(error("pinned-context-publication-current-changed"));
        }
        let mut publication = PinnedContextPublication {
            schema_version: PUBLICATION_SCHEMA_VERSION.into(),
            publication_id: String::new(),
            project_id: set.project_id.clone(),
            previous_set_identity: previous
                .as_ref()
                .map(|(_, descriptor)| descriptor.set_identity.clone()),
            previous_object_reference: previous
                .as_ref()
                .map(|(_, descriptor)| descriptor.object_reference.clone()),
            next_set_identity: descriptor.set_identity.clone(),
            next_object_reference: descriptor.object_reference.clone(),
            created_at_ms,
            content_bodies_persisted: false,
        };
        publication.publication_id = publication_identity(&publication)?;
        self.write_publication(&pointer_name, &publication)?;
        self.write_pointer(&pointer_name, &object_hash)?;
        Ok((descriptor, Some(publication)))
    }

    pub fn complete_publication(
        &self,
        project_id: &str,
        publication_id: &str,
    ) -> Result<(), PinnedContextStoreError> {
        validate_project_id(project_id)?;
        let path = self
            .root
            .join("publications")
            .join(pointer_name(project_id));
        let publication = self.read_publication_path(&path)?;
        if publication.project_id != project_id || publication.publication_id != publication_id {
            return Err(error("pinned-context-publication-identity-changed"));
        }
        fs::remove_file(path).map_err(|_| error("pinned-context-publication-remove-failed"))?;
        sync_directory(&self.root.join("publications"))
    }

    fn read_optional_publication(
        &self,
        pointer_file_name: &str,
    ) -> Result<Option<PinnedContextPublication>, PinnedContextStoreError> {
        if !valid_hash(pointer_file_name) {
            return Err(error("pinned-context-publication-name-invalid"));
        }
        let path = self.root.join("publications").join(pointer_file_name);
        match fs::symlink_metadata(&path) {
            Ok(_) => self.read_publication_path(&path).map(Some),
            Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(_) => Err(error("pinned-context-publication-unavailable")),
        }
    }

    pub fn pending_publications(
        &self,
    ) -> Result<Vec<PinnedContextPublication>, PinnedContextStoreError> {
        self.validate_layout()?;
        let directory = self.root.join("publications");
        let mut publications = Vec::new();
        for entry in fs::read_dir(&directory)
            .map_err(|_| error("pinned-context-publication-enumeration-failed"))?
            .take(MAX_PROJECTS + 1)
        {
            if publications.len() >= MAX_PROJECTS {
                return Err(error("pinned-context-publication-limit"));
            }
            let entry = entry.map_err(|_| error("pinned-context-publication-inspection-failed"))?;
            let name = entry
                .file_name()
                .to_str()
                .map(str::to_owned)
                .ok_or_else(|| error("pinned-context-publication-name-invalid"))?;
            if !valid_hash(&name) {
                return Err(error("pinned-context-publication-name-invalid"));
            }
            publications.push(self.read_publication_path(&entry.path())?);
        }
        publications.sort_by(|left, right| left.project_id.cmp(&right.project_id));
        Ok(publications)
    }

    pub fn garbage_collect_orphan_objects(
        &self,
        now_ms: u64,
    ) -> Result<PinnedContextObjectGcReport, PinnedContextStoreError> {
        self.validate_layout()?;
        let protected = self.protected_object_hashes()?;
        let mut report = PinnedContextObjectGcReport {
            schema_version: "pinned-context-object-gc/0.1".into(),
            examined: 0,
            deleted: 0,
            deleted_bytes: 0,
            retained: 0,
            uncertain: 0,
            issues: Vec::new(),
        };
        let entries = fs::read_dir(self.root.join("objects"))
            .map_err(|_| error("pinned-context-object-enumeration-failed"))?
            .take(MAX_GC_ENTRIES + 1)
            .collect::<Result<Vec<_>, _>>()
            .map_err(|_| error("pinned-context-object-inspection-failed"))?;
        if entries.len() > MAX_GC_ENTRIES {
            report.uncertain = report.uncertain.saturating_add(1);
            report
                .issues
                .push("pinned-context-gc-limit-exceeded".into());
            return Ok(report);
        }
        for entry in entries {
            report.examined = report.examined.saturating_add(1);
            let name = match entry.file_name().to_str().map(str::to_owned) {
                Some(name) if valid_hash(&name) => name,
                _ => {
                    report.uncertain = report.uncertain.saturating_add(1);
                    report
                        .issues
                        .push("pinned-context-gc-unknown-object".into());
                    continue;
                }
            };
            let path = entry.path();
            let metadata = match fs::symlink_metadata(&path) {
                Ok(metadata) if metadata.is_file() && !metadata.file_type().is_symlink() => {
                    metadata
                }
                _ => {
                    report.uncertain = report.uncertain.saturating_add(1);
                    report
                        .issues
                        .push("pinned-context-gc-object-layout-unsafe".into());
                    continue;
                }
            };
            if protected.contains(&name) {
                report.retained = report.retained.saturating_add(1);
                continue;
            }
            let bytes = match self.read_object(&name) {
                Ok(bytes) if sha256_hex(&bytes) == name => bytes,
                _ => {
                    report.uncertain = report.uncertain.saturating_add(1);
                    report
                        .issues
                        .push("pinned-context-gc-integrity-uncertain".into());
                    continue;
                }
            };
            let stored: StoredPinnedContextSet =
                match serde_json::from_slice::<StoredPinnedContextSet>(&bytes) {
                    Ok(stored)
                        if stored.schema_version == STORE_SCHEMA_VERSION
                            && stored.set.schema_version == SCHEMA_VERSION
                            && !stored.content_bodies_persisted
                            && stored.set.validate().is_ok() =>
                    {
                        stored
                    }
                    _ => {
                        report.uncertain = report.uncertain.saturating_add(1);
                        report
                            .issues
                            .push("pinned-context-gc-integrity-uncertain".into());
                        continue;
                    }
                };
            if descriptor(&stored.set, &name).is_err() {
                report.uncertain = report.uncertain.saturating_add(1);
                report
                    .issues
                    .push("pinned-context-gc-integrity-uncertain".into());
                continue;
            }
            let modified_ms = match modified_time_ms(&metadata) {
                Some(modified_ms) => modified_ms,
                None => {
                    report.uncertain = report.uncertain.saturating_add(1);
                    report
                        .issues
                        .push("pinned-context-gc-mtime-unavailable".into());
                    continue;
                }
            };
            if modified_ms > now_ms || now_ms.saturating_sub(modified_ms) < ORPHAN_OBJECT_GRACE_MS {
                report.retained = report.retained.saturating_add(1);
                continue;
            }
            if self.protected_object_hashes()?.contains(&name) {
                report.retained = report.retained.saturating_add(1);
                continue;
            }
            let latest = match fs::symlink_metadata(&path) {
                Ok(latest)
                    if latest.is_file()
                        && !latest.file_type().is_symlink()
                        && latest.len() == metadata.len()
                        && modified_time_ms(&latest) == Some(modified_ms) =>
                {
                    latest
                }
                _ => {
                    report.uncertain = report.uncertain.saturating_add(1);
                    report
                        .issues
                        .push("pinned-context-gc-candidate-changed".into());
                    continue;
                }
            };
            if fs::remove_file(&path).is_err() {
                report.uncertain = report.uncertain.saturating_add(1);
                report.issues.push("pinned-context-gc-delete-failed".into());
                continue;
            }
            report.deleted = report.deleted.saturating_add(1);
            report.deleted_bytes = report.deleted_bytes.saturating_add(latest.len());
        }
        if report.deleted > 0 {
            sync_directory(&self.root.join("objects"))?;
        }
        Ok(report)
    }

    fn protected_object_hashes(
        &self,
    ) -> Result<std::collections::BTreeSet<String>, PinnedContextStoreError> {
        let mut protected = std::collections::BTreeSet::new();
        let pointers = fs::read_dir(self.root.join("pointers"))
            .map_err(|_| error("pinned-context-pointer-enumeration-failed"))?
            .take(MAX_PROJECTS + 1)
            .collect::<Result<Vec<_>, _>>()
            .map_err(|_| error("pinned-context-pointer-inspection-failed"))?;
        if pointers.len() > MAX_PROJECTS {
            return Err(error("pinned-context-project-limit"));
        }
        for entry in pointers {
            let name = entry
                .file_name()
                .to_str()
                .map(str::to_owned)
                .filter(|name| valid_hash(name))
                .ok_or_else(|| error("pinned-context-pointer-name-invalid"))?;
            let metadata = fs::symlink_metadata(entry.path())
                .map_err(|_| error("pinned-context-pointer-inspection-failed"))?;
            if metadata.file_type().is_symlink() || !metadata.is_file() {
                return Err(error("pinned-context-pointer-unsafe"));
            }
            let object_hash = self
                .read_optional_pointer(&name)?
                .ok_or_else(|| error("pinned-context-pointer-missing"))?;
            let bytes = self.read_object(&object_hash)?;
            if sha256_hex(&bytes) != object_hash {
                return Err(error("pinned-context-object-hash-mismatch"));
            }
            let stored: StoredPinnedContextSet = serde_json::from_slice(&bytes)
                .map_err(|_| error("pinned-context-object-invalid"))?;
            if stored.schema_version != STORE_SCHEMA_VERSION
                || stored.set.schema_version != SCHEMA_VERSION
                || stored.content_bodies_persisted
                || pointer_name(&stored.set.project_id) != name
            {
                return Err(error("pinned-context-object-identity-invalid"));
            }
            stored
                .set
                .validate()
                .map_err(|_| error("pinned-context-set-invalid"))?;
            descriptor(&stored.set, &object_hash)?;
            protected.insert(object_hash);
        }
        for publication in self.pending_publications()? {
            self.load_object_reference(
                &publication.project_id,
                &publication.next_object_reference,
            )?;
            protected.insert(
                publication
                    .next_object_reference
                    .strip_prefix("pinned-context-object:sha256:")
                    .ok_or_else(|| error("pinned-context-publication-identity-invalid"))?
                    .into(),
            );
            if let Some(previous) = publication.previous_object_reference.as_deref() {
                self.load_object_reference(&publication.project_id, previous)?;
                protected.insert(
                    previous
                        .strip_prefix("pinned-context-object:sha256:")
                        .ok_or_else(|| error("pinned-context-publication-identity-invalid"))?
                        .into(),
                );
            }
        }
        Ok(protected)
    }

    fn persist_object(
        &self,
        set: &PinnedContextSet,
    ) -> Result<
        (PinnedContextStoreDescriptor, Option<String>, String, String),
        PinnedContextStoreError,
    > {
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
                return Ok((
                    descriptor(set, &object_hash)?,
                    current_hash,
                    object_hash,
                    pointer_name,
                ));
            }
            return Err(error("pinned-context-object-changed"));
        }
        let object_exists = fs::symlink_metadata(self.root.join("objects").join(&object_hash))
            .map(|metadata| metadata.is_file() && !metadata.file_type().is_symlink())
            .unwrap_or(false);
        self.enforce_store_limits(current_hash.is_none(), !object_exists, bytes.len() as u64)?;
        self.write_object(&object_hash, &bytes)?;
        Ok((
            descriptor(set, &object_hash)?,
            current_hash,
            object_hash,
            pointer_name,
        ))
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
        self.load_object_hash(project_id, &object_hash)
    }

    pub fn load_object_reference(
        &self,
        project_id: &str,
        object_reference: &str,
    ) -> Result<(PinnedContextSet, PinnedContextStoreDescriptor), PinnedContextStoreError> {
        validate_project_id(project_id)?;
        self.validate_layout()?;
        let object_hash = object_reference
            .strip_prefix("pinned-context-object:sha256:")
            .filter(|value| valid_hash(value))
            .ok_or_else(|| error("pinned-context-object-reference-invalid"))?;
        self.load_object_hash(project_id, object_hash)
    }

    fn load_object_hash(
        &self,
        project_id: &str,
        object_hash: &str,
    ) -> Result<(PinnedContextSet, PinnedContextStoreDescriptor), PinnedContextStoreError> {
        let bytes = self.read_object(object_hash)?;
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
        let descriptor = descriptor(&stored.set, object_hash)?;
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
        for directory in ["objects", "pointers", "publications"] {
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

    fn write_publication(
        &self,
        pointer_file_name: &str,
        publication: &PinnedContextPublication,
    ) -> Result<(), PinnedContextStoreError> {
        validate_publication(publication)?;
        if pointer_file_name != pointer_name(&publication.project_id) {
            return Err(error("pinned-context-publication-project-mismatch"));
        }
        let parent = self.root.join("publications");
        let target = parent.join(pointer_file_name);
        if fs::symlink_metadata(&target).is_ok() {
            let existing = self.read_publication_path(&target)?;
            if existing == *publication {
                return Ok(());
            }
            return Err(error("pinned-context-publication-pending"));
        }
        let bytes = serde_json::to_vec(publication)
            .map_err(|_| error("pinned-context-publication-serialize"))?;
        if bytes.len() as u64 > MAX_SET_BYTES {
            return Err(error("pinned-context-publication-too-large"));
        }
        let temporary = temporary_path(&parent, "publication");
        let result = (|| {
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&temporary)
                .map_err(|_| error("pinned-context-publication-stage-failed"))?;
            secure_file(&file)?;
            file.write_all(&bytes)
                .and_then(|()| file.sync_all())
                .map_err(|_| error("pinned-context-publication-flush-failed"))?;
            fs::hard_link(&temporary, &target)
                .map_err(|_| error("pinned-context-publication-commit-failed"))?;
            Ok(())
        })();
        let _ = fs::remove_file(&temporary);
        result?;
        sync_directory(&parent)
    }

    fn read_publication_path(
        &self,
        path: &Path,
    ) -> Result<PinnedContextPublication, PinnedContextStoreError> {
        let metadata = fs::symlink_metadata(path)
            .map_err(|_| error("pinned-context-publication-unavailable"))?;
        if metadata.file_type().is_symlink()
            || !metadata.is_file()
            || metadata.len() > MAX_SET_BYTES
        {
            return Err(error("pinned-context-publication-type-invalid"));
        }
        let mut bytes = Vec::with_capacity(metadata.len() as usize);
        File::open(path)
            .and_then(|file| file.take(MAX_SET_BYTES + 1).read_to_end(&mut bytes))
            .map_err(|_| error("pinned-context-publication-read-failed"))?;
        if bytes.len() as u64 > MAX_SET_BYTES {
            return Err(error("pinned-context-publication-too-large"));
        }
        let publication: PinnedContextPublication = serde_json::from_slice(&bytes)
            .map_err(|_| error("pinned-context-publication-invalid"))?;
        validate_publication(&publication)?;
        let name = path
            .file_name()
            .and_then(|value| value.to_str())
            .ok_or_else(|| error("pinned-context-publication-name-invalid"))?;
        if name != pointer_name(&publication.project_id) {
            return Err(error("pinned-context-publication-project-mismatch"));
        }
        Ok(publication)
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

fn validate_publication(
    publication: &PinnedContextPublication,
) -> Result<(), PinnedContextStoreError> {
    validate_project_id(&publication.project_id)?;
    if publication.schema_version != PUBLICATION_SCHEMA_VERSION
        || publication.content_bodies_persisted
        || publication.created_at_ms == 0
        || !valid_prefixed_hash(
            &publication.publication_id,
            "pinned-context-publication:sha256:",
        )
        || !valid_prefixed_hash(&publication.next_set_identity, "pinned-context:sha256:")
        || !valid_prefixed_hash(
            &publication.next_object_reference,
            "pinned-context-object:sha256:",
        )
        || publication.previous_set_identity.is_some()
            != publication.previous_object_reference.is_some()
        || publication
            .previous_set_identity
            .as_deref()
            .is_some_and(|value| !valid_prefixed_hash(value, "pinned-context:sha256:"))
        || publication
            .previous_object_reference
            .as_deref()
            .is_some_and(|value| !valid_prefixed_hash(value, "pinned-context-object:sha256:"))
        || publication.previous_set_identity.as_deref()
            == Some(publication.next_set_identity.as_str())
        || publication_identity(publication)? != publication.publication_id
    {
        return Err(error("pinned-context-publication-identity-invalid"));
    }
    Ok(())
}

fn publication_identity(
    publication: &PinnedContextPublication,
) -> Result<String, PinnedContextStoreError> {
    let mut identity = publication.clone();
    identity.publication_id.clear();
    let bytes =
        serde_json::to_vec(&identity).map_err(|_| error("pinned-context-publication-serialize"))?;
    Ok(format!(
        "pinned-context-publication:sha256:{}",
        sha256_hex(&bytes)
    ))
}

fn valid_prefixed_hash(value: &str, prefix: &str) -> bool {
    value.strip_prefix(prefix).is_some_and(valid_hash)
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

fn modified_time_ms(metadata: &fs::Metadata) -> Option<u64> {
    metadata
        .modified()
        .ok()?
        .duration_since(UNIX_EPOCH)
        .ok()?
        .as_millis()
        .try_into()
        .ok()
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
    #[cfg(not(unix))]
    let _ = file;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        file.set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(|_| error("pinned-context-file-permission-failed"))?;
    }
    Ok(())
}

fn sync_directory(path: &Path) -> Result<(), PinnedContextStoreError> {
    #[cfg(not(unix))]
    let _ = path;
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
    use std::time::{SystemTime, UNIX_EPOCH};

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

    fn future_gc_time() -> u64 {
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_millis() as u64
            + ORPHAN_OBJECT_GRACE_MS
            + 1
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
    fn publication_journal_is_explicitly_completed_after_pointer_publish() {
        let root = root();
        let store = PinnedContextStore::open(&root).unwrap();
        let expected = set();
        let (descriptor, publication) = store.persist_publication(&expected, 1).unwrap();
        let publication = publication.expect("new object has a publication journal");
        assert_eq!(publication.next_set_identity, descriptor.set_identity);
        assert_eq!(
            publication.next_object_reference,
            descriptor.object_reference
        );
        assert_eq!(
            store.pending_publications().unwrap(),
            vec![publication.clone()]
        );
        let (loaded, loaded_descriptor) = store
            .load_object_reference("project-1", &publication.next_object_reference)
            .unwrap();
        assert_eq!(loaded, expected);
        assert_eq!(loaded_descriptor, descriptor);

        store
            .complete_publication("project-1", &publication.publication_id)
            .unwrap();
        assert!(store.pending_publications().unwrap().is_empty());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn publication_retry_reuses_journal_without_changing_identity() {
        let root = root();
        let store = PinnedContextStore::open(&root).unwrap();
        let initial = set();
        let (initial_descriptor, initial_publication) =
            store.persist_publication(&initial, 1).unwrap();
        let initial_publication = initial_publication.unwrap();
        let pointer = store.root.join("pointers").join(pointer_name("project-1"));
        fs::remove_file(&pointer).unwrap();
        let (retried_descriptor, retried_publication) =
            store.persist_publication(&initial, 2).unwrap();
        assert_eq!(retried_descriptor, initial_descriptor);
        assert_eq!(retried_publication, Some(initial_publication.clone()));
        assert!(store.load("project-1").is_ok());
        store
            .complete_publication("project-1", &initial_publication.publication_id)
            .unwrap();

        let mut updated = initial.clone();
        updated.add(item("pin-2", "src/lib.rs")).unwrap();
        let (updated_descriptor, updated_publication) =
            store.persist_publication(&updated, 3).unwrap();
        let updated_publication = updated_publication.unwrap();
        let initial_hash = initial_descriptor
            .object_reference
            .strip_prefix("pinned-context-object:sha256:")
            .unwrap();
        fs::write(&pointer, format!("{initial_hash}\n")).unwrap();
        let (retried_updated_descriptor, retried_updated_publication) =
            store.persist_publication(&updated, 4).unwrap();
        assert_eq!(retried_updated_descriptor, updated_descriptor);
        assert_eq!(
            retried_updated_publication,
            Some(updated_publication.clone())
        );
        assert_eq!(store.load("project-1").unwrap().0, updated);
        store
            .complete_publication("project-1", &updated_publication.publication_id)
            .unwrap();
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn tampered_publication_journal_is_rejected_without_guessing_pointer_state() {
        let root = root();
        let store = PinnedContextStore::open(&root).unwrap();
        let (_, publication) = store.persist_publication(&set(), 1).unwrap();
        let publication = publication.unwrap();
        let journal = store
            .root
            .join("publications")
            .join(pointer_name("project-1"));
        let mut bytes = fs::read(&journal).unwrap();
        bytes.extend_from_slice(b"tampered");
        fs::write(&journal, bytes).unwrap();
        assert_eq!(
            store.pending_publications().unwrap_err().code,
            "pinned-context-publication-invalid"
        );
        assert!(store.load("project-1").is_ok());
        assert!(store
            .complete_publication("project-1", &publication.publication_id)
            .is_err());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn orphan_gc_protects_current_and_journal_objects_then_reclaims_old_orphans() {
        let root = root();
        let store = PinnedContextStore::open(&root).unwrap();
        let initial = set();
        let initial_descriptor = store.persist(&initial).unwrap();
        let mut updated = initial.clone();
        updated.add(item("pin-2", "src/lib.rs")).unwrap();
        let (updated_descriptor, publication) = store.persist_publication(&updated, 1).unwrap();
        let publication = publication.unwrap();
        let initial_hash = initial_descriptor
            .object_reference
            .strip_prefix("pinned-context-object:sha256:")
            .unwrap();
        let updated_hash = updated_descriptor
            .object_reference
            .strip_prefix("pinned-context-object:sha256:")
            .unwrap();
        let journal_gc = store
            .garbage_collect_orphan_objects(future_gc_time())
            .unwrap();
        assert_eq!(journal_gc.deleted, 0);
        assert!(store.root.join("objects").join(initial_hash).exists());
        assert!(store.root.join("objects").join(updated_hash).exists());

        store
            .complete_publication("project-1", &publication.publication_id)
            .unwrap();
        let reclaimed = store
            .garbage_collect_orphan_objects(future_gc_time())
            .unwrap();
        assert_eq!(reclaimed.deleted, 1);
        assert!(reclaimed.deleted_bytes > 0);
        assert!(!store.root.join("objects").join(initial_hash).exists());
        assert!(store.root.join("objects").join(updated_hash).exists());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn orphan_gc_preserves_unknown_and_corrupt_objects() {
        let root = root();
        let store = PinnedContextStore::open(&root).unwrap();
        let unknown = store.root.join("objects").join("unknown-entry");
        fs::write(&unknown, b"not an object").unwrap();
        let report = store
            .garbage_collect_orphan_objects(future_gc_time())
            .unwrap();
        assert_eq!(report.deleted, 0);
        assert!(report.uncertain > 0);
        assert!(report
            .issues
            .iter()
            .any(|issue| issue == "pinned-context-gc-unknown-object"));
        assert!(unknown.exists());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn orphan_gc_fails_closed_when_current_pointer_object_is_tampered() {
        let root = root();
        let store = PinnedContextStore::open(&root).unwrap();
        let descriptor = store.persist(&set()).unwrap();
        let object_hash = descriptor
            .object_reference
            .strip_prefix("pinned-context-object:sha256:")
            .unwrap();
        fs::write(store.root.join("objects").join(object_hash), b"tampered").unwrap();
        assert_eq!(
            store
                .garbage_collect_orphan_objects(future_gc_time())
                .unwrap_err()
                .code,
            "pinned-context-object-hash-mismatch"
        );
        assert!(store.root.join("objects").join(object_hash).exists());
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
        fs::remove_dir(store.root.join("publications")).unwrap();
        symlink(&outside, store.root.join("publications")).unwrap();
        assert_eq!(
            store.pending_publications().unwrap_err().code,
            "pinned-context-directory-unsafe"
        );
        fs::remove_dir_all(root).unwrap();
    }
}
