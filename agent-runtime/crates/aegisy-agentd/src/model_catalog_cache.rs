//! Metadata-only model catalog cache lifecycle.
//!
//! This contract is intentionally below the cloud/authentication boundary. It
//! accepts only catalog metadata marked as signature-validated, then protects
//! local cache state from malformed expiry, clock rollback,
//! duplicate-generation conflicts, and older revisions. The public persistent
//! admission path verifies through root-anchored trust state first. The cache
//! still fixes selection authority to false and does not fetch, sign, refresh,
//! select, route, issue tokens, or authorize a turn.

use crate::model_catalog::{CatalogState, ModelCatalog};
use crate::model_catalog_signature::SignedModelCatalog;
use crate::model_catalog_trust_store::CatalogTrustStore;
use serde::{Deserialize, Serialize};
use serde_json::to_vec;
use sha2::{Digest, Sha256};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

pub const SCHEMA_VERSION: &str = "model-catalog-cache/0.1";
pub const STORE_SCHEMA_VERSION: &str = "model-catalog-cache-store/0.1";
const MAX_STALE_MS: u64 = 30 * 24 * 60 * 60 * 1_000;
const MAX_TTL_MS: u64 = 90 * 24 * 60 * 60 * 1_000;
const MAX_SNAPSHOT_BYTES: usize = 2 * 1024 * 1024;
static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum CacheAvailability {
    Empty,
    Fresh,
    Stale,
    Expired,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CatalogCacheRecord {
    pub schema_version: String,
    pub sequence: u64,
    pub catalog_identity: String,
    pub received_at_ms: u64,
    pub catalog: ModelCatalog,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CatalogCacheSnapshot {
    pub schema_version: String,
    pub max_stale_ms: u64,
    pub current: Option<CatalogCacheRecord>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CatalogCacheView {
    pub schema_version: String,
    pub availability: CacheAvailability,
    pub sequence: Option<u64>,
    pub stored_catalog_identity: Option<String>,
    pub catalog_identity: Option<String>,
    pub received_at_ms: Option<u64>,
    pub expires_at_ms: Option<u64>,
    pub stale_age_ms: Option<u64>,
    pub catalog: Option<ModelCatalog>,
    pub selection_allowed: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CacheWrite {
    Installed { sequence: u64 },
    Idempotent { sequence: u64 },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CatalogCacheError {
    pub code: &'static str,
    pub message: &'static str,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ModelCatalogCache {
    schema_version: String,
    max_stale_ms: u64,
    current: Option<CatalogCacheRecord>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CatalogCacheStoreSnapshot {
    pub schema_version: String,
    pub cache: CatalogCacheSnapshot,
    pub store_identity: String,
}

#[derive(Debug)]
pub struct ModelCatalogCacheStore {
    root: Option<PathBuf>,
    cache: ModelCatalogCache,
}

impl ModelCatalogCacheStore {
    pub fn in_memory(max_stale_ms: u64) -> Result<Self, CatalogCacheError> {
        Ok(Self {
            root: None,
            cache: ModelCatalogCache::new(max_stale_ms)?,
        })
    }

    pub fn open(data_root: &Path, max_stale_ms: u64) -> Result<Self, CatalogCacheError> {
        let metadata = fs::symlink_metadata(data_root).map_err(|_| {
            error(
                "model-catalog-cache-data-root-unavailable",
                "cache data root is unavailable",
            )
        })?;
        if metadata.file_type().is_symlink() || !metadata.is_dir() {
            return Err(error(
                "model-catalog-cache-data-root-unsafe",
                "cache data root is unsafe",
            ));
        }
        let data_root = data_root.canonicalize().map_err(|_| {
            error(
                "model-catalog-cache-data-root-unavailable",
                "cache data root is unavailable",
            )
        })?;
        let root = data_root.join("model-catalog-cache-v1");
        create_secure_directory(&root)?;
        let root = root.canonicalize().map_err(|_| {
            error(
                "model-catalog-cache-store-unavailable",
                "cache store is unavailable",
            )
        })?;
        if root != data_root.join("model-catalog-cache-v1") {
            return Err(error(
                "model-catalog-cache-store-symlink",
                "cache store traverses a symlink",
            ));
        }
        let snapshot_path = root.join("snapshot.json");
        let cache = match fs::symlink_metadata(&snapshot_path) {
            Ok(metadata) => {
                if metadata.file_type().is_symlink() || !metadata.is_file() {
                    return Err(error(
                        "model-catalog-cache-snapshot-unsafe",
                        "cache snapshot is unsafe",
                    ));
                }
                let bytes = read_limited(&snapshot_path)?;
                let stored: CatalogCacheStoreSnapshot =
                    serde_json::from_slice(&bytes).map_err(|_| {
                        error(
                            "model-catalog-cache-snapshot-invalid",
                            "cache snapshot is invalid",
                        )
                    })?;
                if stored.schema_version != STORE_SCHEMA_VERSION
                    || stored.store_identity != store_snapshot_identity(&stored)?
                {
                    return Err(error(
                        "model-catalog-cache-snapshot-identity-mismatch",
                        "cache snapshot identity does not match",
                    ));
                }
                ModelCatalogCache::from_snapshot(stored.cache)?
            }
            Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => {
                ModelCatalogCache::new(max_stale_ms)?
            }
            Err(_) => {
                return Err(error(
                    "model-catalog-cache-snapshot-unavailable",
                    "cache snapshot is unavailable",
                ))
            }
        };
        let store = Self {
            root: Some(root),
            cache,
        };
        store.validate()?;
        Ok(store)
    }

    pub fn view(&self, now_ms: u64) -> Result<CatalogCacheView, CatalogCacheError> {
        self.validate()?;
        self.cache.view(now_ms)
    }

    pub fn snapshot(&self) -> Result<CatalogCacheStoreSnapshot, CatalogCacheError> {
        self.validate()?;
        let mut snapshot = CatalogCacheStoreSnapshot {
            schema_version: STORE_SCHEMA_VERSION.into(),
            cache: self.cache.snapshot()?,
            store_identity: String::new(),
        };
        snapshot.store_identity = store_snapshot_identity(&snapshot)?;
        Ok(snapshot)
    }

    fn install(
        &mut self,
        catalog: ModelCatalog,
        sequence: u64,
        received_at_ms: u64,
    ) -> Result<CacheWrite, CatalogCacheError> {
        self.validate()?;
        let previous = self.cache.clone();
        let write = self.cache.install(catalog, sequence, received_at_ms)?;
        if let Some(root) = &self.root {
            if let Err(error) = self.write_snapshot(root) {
                self.cache = previous;
                return Err(error);
            }
        }
        Ok(write)
    }

    /// Install a catalog only after verification through durable root-anchored
    /// trust state. The host remains responsible for supplying the authentic
    /// compiled or signed-package trust anchor when it opens the Trust Store.
    pub fn install_trusted(
        &mut self,
        signed: &SignedModelCatalog,
        trust_store: &CatalogTrustStore,
        now_ms: u64,
        sequence: u64,
        received_at_ms: u64,
    ) -> Result<CacheWrite, CatalogCacheError> {
        let catalog = trust_store
            .verify_catalog(signed, now_ms)
            .map_err(|trust_error| error(trust_error.code, trust_error.message))?;
        self.install(catalog, sequence, received_at_ms)
    }

    fn validate(&self) -> Result<(), CatalogCacheError> {
        self.cache.validate()
    }

    fn write_snapshot(&self, root: &Path) -> Result<(), CatalogCacheError> {
        let snapshot = self.snapshot()?;
        let bytes = serde_json::to_vec(&snapshot).map_err(|_| {
            error(
                "model-catalog-cache-snapshot-serialize",
                "cache snapshot could not be serialized",
            )
        })?;
        if bytes.len() > MAX_SNAPSHOT_BYTES {
            return Err(error(
                "model-catalog-cache-snapshot-too-large",
                "cache snapshot exceeds its size limit",
            ));
        }
        let target = root.join("snapshot.json");
        if let Ok(metadata) = fs::symlink_metadata(&target) {
            if metadata.file_type().is_symlink() || !metadata.is_file() {
                return Err(error(
                    "model-catalog-cache-snapshot-unsafe",
                    "cache snapshot is unsafe",
                ));
            }
        }
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let temporary = root.join(format!(".snapshot.{sequence}.tmp"));
        let result = (|| {
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&temporary)
                .map_err(|_| {
                    error(
                        "model-catalog-cache-snapshot-stage",
                        "cache snapshot staging failed",
                    )
                })?;
            secure_file(&file)?;
            file.write_all(&bytes)
                .and_then(|()| file.sync_all())
                .map_err(|_| {
                    error(
                        "model-catalog-cache-snapshot-sync",
                        "cache snapshot sync failed",
                    )
                })?;
            replace_file(&temporary, &target)
        })();
        let _ = fs::remove_file(&temporary);
        result?;
        sync_directory(root)
    }
}

impl ModelCatalogCache {
    pub fn new(max_stale_ms: u64) -> Result<Self, CatalogCacheError> {
        validate_stale_window(max_stale_ms)?;
        Ok(Self {
            schema_version: SCHEMA_VERSION.into(),
            max_stale_ms,
            current: None,
        })
    }

    fn install(
        &mut self,
        catalog: ModelCatalog,
        sequence: u64,
        received_at_ms: u64,
    ) -> Result<CacheWrite, CatalogCacheError> {
        self.validate()?;
        let record = CatalogCacheRecord::new(catalog, sequence, received_at_ms)?;
        if let Some(current) = &self.current {
            if sequence < current.sequence {
                return Err(error(
                    "model-catalog-cache-rollback",
                    "catalog sequence moves backwards",
                ));
            }
            if sequence == current.sequence {
                if record.catalog_identity == current.catalog_identity {
                    return Ok(CacheWrite::Idempotent { sequence });
                }
                return Err(error(
                    "model-catalog-cache-generation-conflict",
                    "catalog sequence has a different identity",
                ));
            }
        }
        self.current = Some(record);
        Ok(CacheWrite::Installed { sequence })
    }

    pub fn view(&self, now_ms: u64) -> Result<CatalogCacheView, CatalogCacheError> {
        self.validate()?;
        let Some(record) = &self.current else {
            return Ok(CatalogCacheView {
                schema_version: SCHEMA_VERSION.into(),
                availability: CacheAvailability::Empty,
                sequence: None,
                stored_catalog_identity: None,
                catalog_identity: None,
                received_at_ms: None,
                expires_at_ms: None,
                stale_age_ms: None,
                catalog: None,
                selection_allowed: false,
            });
        };
        if now_ms < record.received_at_ms {
            return Err(error(
                "model-catalog-cache-clock-regression",
                "cache clock precedes catalog receipt",
            ));
        }
        let expires_at_ms = record
            .catalog
            .expires_at_ms
            .expect("validated cache record has expiry");
        let (availability, catalog, stale_age_ms) = if now_ms < expires_at_ms {
            (CacheAvailability::Fresh, record.catalog.clone(), None)
        } else {
            let stale_age_ms = now_ms - expires_at_ms;
            if stale_age_ms <= self.max_stale_ms {
                let mut stale = record.catalog.clone();
                stale.state = CatalogState::Stale;
                (CacheAvailability::Stale, stale, Some(stale_age_ms))
            } else {
                (
                    CacheAvailability::Expired,
                    record.catalog.clone(),
                    Some(stale_age_ms),
                )
            }
        };
        let catalog_identity = if availability == CacheAvailability::Expired {
            None
        } else {
            Some(identity(&catalog)?)
        };
        Ok(CatalogCacheView {
            schema_version: SCHEMA_VERSION.into(),
            availability,
            sequence: Some(record.sequence),
            stored_catalog_identity: Some(record.catalog_identity.clone()),
            catalog_identity,
            received_at_ms: Some(record.received_at_ms),
            expires_at_ms: Some(expires_at_ms),
            stale_age_ms,
            catalog: (availability != CacheAvailability::Expired).then_some(catalog),
            selection_allowed: false,
        })
    }

    pub fn snapshot(&self) -> Result<CatalogCacheSnapshot, CatalogCacheError> {
        self.validate()?;
        Ok(CatalogCacheSnapshot {
            schema_version: SCHEMA_VERSION.into(),
            max_stale_ms: self.max_stale_ms,
            current: self.current.clone(),
        })
    }

    pub fn from_snapshot(snapshot: CatalogCacheSnapshot) -> Result<Self, CatalogCacheError> {
        if snapshot.schema_version != SCHEMA_VERSION {
            return Err(error(
                "model-catalog-cache-schema-unsupported",
                "cache schema is unsupported",
            ));
        }
        validate_stale_window(snapshot.max_stale_ms)?;
        if let Some(record) = &snapshot.current {
            record.validate()?;
        }
        let cache = Self {
            schema_version: SCHEMA_VERSION.into(),
            max_stale_ms: snapshot.max_stale_ms,
            current: snapshot.current,
        };
        cache.validate()?;
        Ok(cache)
    }

    pub fn identity(&self) -> Result<String, CatalogCacheError> {
        let snapshot = self.snapshot()?;
        let bytes = to_vec(&snapshot).map_err(|_| {
            error(
                "model-catalog-cache-serialize",
                "cache snapshot could not be serialized",
            )
        })?;
        Ok(format!(
            "model-catalog-cache:sha256:{:x}",
            Sha256::digest(bytes)
        ))
    }

    fn validate(&self) -> Result<(), CatalogCacheError> {
        if self.schema_version != SCHEMA_VERSION {
            return Err(error(
                "model-catalog-cache-schema-unsupported",
                "cache schema is unsupported",
            ));
        }
        validate_stale_window(self.max_stale_ms)?;
        if let Some(record) = &self.current {
            record.validate()?;
        }
        Ok(())
    }
}

impl CatalogCacheRecord {
    fn new(
        catalog: ModelCatalog,
        sequence: u64,
        received_at_ms: u64,
    ) -> Result<Self, CatalogCacheError> {
        let record = Self {
            schema_version: SCHEMA_VERSION.into(),
            sequence,
            catalog_identity: identity(&catalog)?,
            received_at_ms,
            catalog,
        };
        record.validate()?;
        Ok(record)
    }

    fn validate(&self) -> Result<(), CatalogCacheError> {
        if self.schema_version != SCHEMA_VERSION {
            return Err(error(
                "model-catalog-cache-record-schema-unsupported",
                "cache record schema is unsupported",
            ));
        }
        if self.sequence == 0 {
            return Err(error(
                "model-catalog-cache-sequence-invalid",
                "cache sequence must be positive",
            ));
        }
        self.catalog.validate().map_err(|_| {
            error(
                "model-catalog-cache-catalog-invalid",
                "cached catalog failed validation",
            )
        })?;
        if self.catalog.state != CatalogState::Fresh
            || !self.catalog.signature_validated
            || !self.catalog.validation_errors.is_empty()
        {
            return Err(error(
                "model-catalog-cache-catalog-untrusted",
                "cache accepts only a clean fresh signed catalog",
            ));
        }
        let Some(expires_at_ms) = self.catalog.expires_at_ms else {
            return Err(error(
                "model-catalog-cache-expiry-missing",
                "signed catalog must include an expiry",
            ));
        };
        if expires_at_ms <= self.received_at_ms {
            return Err(error(
                "model-catalog-cache-expiry-invalid",
                "catalog expiry must follow receipt",
            ));
        }
        if expires_at_ms - self.received_at_ms > MAX_TTL_MS {
            return Err(error(
                "model-catalog-cache-expiry-too-long",
                "catalog expiry exceeds the bounded cache lifetime",
            ));
        }
        if self
            .catalog
            .issued_at_ms
            .is_some_and(|issued_at_ms| issued_at_ms > self.received_at_ms)
        {
            return Err(error(
                "model-catalog-cache-issued-in-future",
                "catalog issue time follows receipt",
            ));
        }
        if self.catalog_identity != identity(&self.catalog)? {
            return Err(error(
                "model-catalog-cache-identity-mismatch",
                "cache record identity does not match catalog",
            ));
        }
        Ok(())
    }
}

fn validate_stale_window(max_stale_ms: u64) -> Result<(), CatalogCacheError> {
    if max_stale_ms == 0 || max_stale_ms > MAX_STALE_MS {
        return Err(error(
            "model-catalog-cache-stale-window-invalid",
            "cache stale window is outside its bounds",
        ));
    }
    Ok(())
}

fn identity(catalog: &ModelCatalog) -> Result<String, CatalogCacheError> {
    let bytes = to_vec(catalog).map_err(|_| {
        error(
            "model-catalog-cache-serialize",
            "catalog could not be serialized",
        )
    })?;
    Ok(format!("model-catalog:sha256:{:x}", Sha256::digest(bytes)))
}

fn store_snapshot_identity(
    snapshot: &CatalogCacheStoreSnapshot,
) -> Result<String, CatalogCacheError> {
    let mut copy = snapshot.clone();
    copy.store_identity.clear();
    let bytes = to_vec(&copy).map_err(|_| {
        error(
            "model-catalog-cache-snapshot-serialize",
            "cache snapshot could not be serialized",
        )
    })?;
    Ok(format!(
        "model-catalog-cache-store:sha256:{:x}",
        Sha256::digest(bytes)
    ))
}

fn read_limited(path: &Path) -> Result<Vec<u8>, CatalogCacheError> {
    let metadata = fs::symlink_metadata(path).map_err(|_| {
        error(
            "model-catalog-cache-snapshot-read",
            "cache snapshot could not be read",
        )
    })?;
    if metadata.file_type().is_symlink()
        || !metadata.is_file()
        || metadata.len() > MAX_SNAPSHOT_BYTES as u64
    {
        return Err(error(
            "model-catalog-cache-snapshot-size-or-layout",
            "cache snapshot exceeds its size or layout limit",
        ));
    }
    let mut file = File::open(path).map_err(|_| {
        error(
            "model-catalog-cache-snapshot-read",
            "cache snapshot could not be read",
        )
    })?;
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    file.read_to_end(&mut bytes).map_err(|_| {
        error(
            "model-catalog-cache-snapshot-read",
            "cache snapshot could not be read",
        )
    })?;
    if bytes.len() > MAX_SNAPSHOT_BYTES {
        return Err(error(
            "model-catalog-cache-snapshot-size-or-layout",
            "cache snapshot exceeds its size or layout limit",
        ));
    }
    Ok(bytes)
}

fn create_secure_directory(path: &Path) -> Result<(), CatalogCacheError> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() || !metadata.is_dir() => Err(error(
            "model-catalog-cache-directory-unsafe",
            "cache directory is unsafe",
        )),
        Ok(_) => secure_directory(path),
        Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => {
            fs::create_dir(path).map_err(|_| {
                error(
                    "model-catalog-cache-directory-create",
                    "cache directory could not be created",
                )
            })?;
            secure_directory(path)
        }
        Err(_) => Err(error(
            "model-catalog-cache-directory-read",
            "cache directory could not be read",
        )),
    }
}

fn secure_directory(path: &Path) -> Result<(), CatalogCacheError> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(path, fs::Permissions::from_mode(0o700)).map_err(|_| {
            error(
                "model-catalog-cache-directory-permission",
                "cache directory permissions could not be set",
            )
        })?;
    }
    Ok(())
}

fn secure_file(file: &File) -> Result<(), CatalogCacheError> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        file.set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(|_| {
                error(
                    "model-catalog-cache-file-permission",
                    "cache file permissions could not be set",
                )
            })?;
    }
    Ok(())
}

fn sync_directory(path: &Path) -> Result<(), CatalogCacheError> {
    #[cfg(unix)]
    File::open(path)
        .and_then(|directory| directory.sync_all())
        .map_err(|_| {
            error(
                "model-catalog-cache-directory-sync",
                "cache directory sync failed",
            )
        })?;
    Ok(())
}

#[cfg(unix)]
fn replace_file(source: &Path, target: &Path) -> Result<(), CatalogCacheError> {
    fs::rename(source, target).map_err(|_| {
        error(
            "model-catalog-cache-snapshot-commit",
            "cache snapshot commit failed",
        )
    })
}

#[cfg(target_os = "windows")]
fn replace_file(source: &Path, target: &Path) -> Result<(), CatalogCacheError> {
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
        return Err(error(
            "model-catalog-cache-snapshot-commit",
            "cache snapshot commit failed",
        ));
    }
    Ok(())
}

#[cfg(not(any(unix, target_os = "windows")))]
fn replace_file(source: &Path, target: &Path) -> Result<(), CatalogCacheError> {
    if target.exists() {
        return Err(error(
            "model-catalog-cache-snapshot-commit",
            "cache snapshot replacement is unsupported",
        ));
    }
    fs::rename(source, target).map_err(|_| {
        error(
            "model-catalog-cache-snapshot-commit",
            "cache snapshot commit failed",
        )
    })
}

fn error(code: &'static str, message: &'static str) -> CatalogCacheError {
    CatalogCacheError { code, message }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model_catalog::offline_for_runtime;
    use crate::model_catalog_signature::{
        key_ring_signing_payload_bytes, key_ring_signing_payload_identity, signing_payload_bytes,
        signing_payload_identity, CatalogKeyRing, CatalogSigningKey, SignedCatalogKeyRing,
        KEY_RING_SIGNATURE_SCHEMA_VERSION, SIGNATURE_SCHEMA_VERSION,
    };
    use crate::model_catalog_trust_store::CatalogTrustAnchor;
    use base64::engine::general_purpose::STANDARD as BASE64_STANDARD;
    use base64::Engine;
    use ed25519_dalek::{Signer, SigningKey};

    const STALE_WINDOW_MS: u64 = 1_000;

    fn signed_catalog(received_at_ms: u64, expires_at_ms: u64) -> ModelCatalog {
        let mut catalog = offline_for_runtime("codex", "0.144.5", Some("aegisy"), Some("agent"));
        catalog.state = CatalogState::Fresh;
        catalog.signature_validated = true;
        catalog.catalog_version = "catalog-1".into();
        catalog.issued_at_ms = Some(received_at_ms);
        catalog.expires_at_ms = Some(expires_at_ms);
        catalog.validation_errors.clear();
        catalog
    }

    #[test]
    fn install_and_view_exposes_fresh_then_stale_then_expired() {
        let mut cache = ModelCatalogCache::new(STALE_WINDOW_MS).unwrap();
        let catalog = signed_catalog(100, 1_000);
        assert_eq!(
            cache.install(catalog, 1, 100).unwrap(),
            CacheWrite::Installed { sequence: 1 }
        );

        let fresh = cache.view(999).unwrap();
        assert_eq!(fresh.availability, CacheAvailability::Fresh);
        assert_eq!(fresh.catalog.as_ref().unwrap().state, CatalogState::Fresh);
        assert!(!fresh.selection_allowed);

        let stale = cache.view(1_500).unwrap();
        assert_eq!(stale.availability, CacheAvailability::Stale);
        assert_eq!(stale.stale_age_ms, Some(500));
        assert_eq!(stale.catalog.as_ref().unwrap().state, CatalogState::Stale);

        let expired = cache.view(2_001).unwrap();
        assert_eq!(expired.availability, CacheAvailability::Expired);
        assert!(expired.catalog.is_none());
    }

    #[test]
    fn sequence_rejects_rollback_and_same_generation_conflict() {
        let mut cache = ModelCatalogCache::new(STALE_WINDOW_MS).unwrap();
        let first = signed_catalog(100, 1_000);
        cache.install(first, 2, 100).unwrap();
        let rollback = signed_catalog(101, 1_001);
        let error = cache.install(rollback, 1, 101).unwrap_err();
        assert_eq!(error.code, "model-catalog-cache-rollback");

        let conflict = signed_catalog(102, 1_002);
        let error = cache.install(conflict, 2, 102).unwrap_err();
        assert_eq!(error.code, "model-catalog-cache-generation-conflict");
        assert_eq!(cache.view(200).unwrap().sequence, Some(2));
    }

    #[test]
    fn identical_retry_is_idempotent_without_rewriting_receipt() {
        let mut cache = ModelCatalogCache::new(STALE_WINDOW_MS).unwrap();
        let catalog = signed_catalog(100, 1_000);
        cache.install(catalog.clone(), 1, 100).unwrap();
        assert_eq!(
            cache.install(catalog, 1, 200).unwrap(),
            CacheWrite::Idempotent { sequence: 1 }
        );
        assert_eq!(cache.view(200).unwrap().received_at_ms, Some(100));
    }

    #[test]
    fn rejects_unsigned_offline_and_unbounded_expiry_catalogs() {
        let mut cache = ModelCatalogCache::new(STALE_WINDOW_MS).unwrap();
        let mut unsigned = offline_for_runtime("codex", "0.144.5", Some("aegisy"), Some("agent"));
        unsigned.expires_at_ms = Some(1_000);
        let error = cache.install(unsigned, 1, 100).unwrap_err();
        assert_eq!(error.code, "model-catalog-cache-catalog-untrusted");

        let too_long = signed_catalog(100, 100 + MAX_TTL_MS + 1);
        let error = cache.install(too_long, 1, 100).unwrap_err();
        assert_eq!(error.code, "model-catalog-cache-expiry-too-long");

        let mut inconsistent = signed_catalog(100, 1_000);
        inconsistent
            .validation_errors
            .push("reported-invalid".into());
        let error = cache.install(inconsistent, 1, 100).unwrap_err();
        assert_eq!(error.code, "model-catalog-cache-catalog-untrusted");
    }

    #[test]
    fn snapshot_reopen_checks_identity_and_clock_direction() {
        let mut cache = ModelCatalogCache::new(STALE_WINDOW_MS).unwrap();
        cache.install(signed_catalog(100, 1_000), 1, 100).unwrap();
        let snapshot = cache.snapshot().unwrap();
        let identity = cache.identity().unwrap();
        assert!(identity.starts_with("model-catalog-cache:sha256:"));
        let reopened = ModelCatalogCache::from_snapshot(snapshot.clone()).unwrap();
        assert_eq!(reopened.identity().unwrap(), identity);

        let error = reopened.view(99).unwrap_err();
        assert_eq!(error.code, "model-catalog-cache-clock-regression");

        let mut tampered = snapshot;
        tampered.current.as_mut().unwrap().catalog_identity =
            "model-catalog:sha256:tampered".into();
        let error = ModelCatalogCache::from_snapshot(tampered).unwrap_err();
        assert_eq!(error.code, "model-catalog-cache-identity-mismatch");
    }

    #[test]
    fn signed_install_verifies_before_cache_admission() {
        let signing_key = SigningKey::from_bytes(&[7; 32]);
        let key_id = "catalog-key-1";
        let key_ring = CatalogKeyRing::new(
            1,
            vec![CatalogSigningKey {
                key_id: key_id.into(),
                public_key_base64: BASE64_STANDARD.encode(signing_key.verifying_key().to_bytes()),
                valid_from_ms: 1,
                valid_until_ms: Some(10_000),
                revoked: false,
                replaces: None,
            }],
        )
        .unwrap();
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "aegisy-model-catalog-trust-cache-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(&root).unwrap();
        let anchor = CatalogTrustAnchor::new(
            key_id,
            BASE64_STANDARD.encode(signing_key.verifying_key().to_bytes()),
        )
        .unwrap();
        let mut trust_store = CatalogTrustStore::open(&root, anchor).unwrap();
        let key_ring_payload = key_ring_signing_payload_bytes(key_id, 100, &key_ring).unwrap();
        trust_store
            .install_signed_key_ring(
                &SignedCatalogKeyRing {
                    schema_version: KEY_RING_SIGNATURE_SCHEMA_VERSION.into(),
                    signer_key_id: key_id.into(),
                    signed_at_ms: 100,
                    key_ring,
                    payload_identity: key_ring_signing_payload_identity(&key_ring_payload),
                    signature_base64: BASE64_STANDARD
                        .encode(signing_key.sign(&key_ring_payload).to_bytes()),
                },
                100,
            )
            .unwrap();
        let mut catalog = signed_catalog(100, 1_000);
        catalog.signature_validated = false;
        let payload = signing_payload_bytes(key_id, &catalog).unwrap();
        let envelope = SignedModelCatalog {
            schema_version: SIGNATURE_SCHEMA_VERSION.into(),
            key_id: key_id.into(),
            catalog,
            payload_identity: signing_payload_identity(&payload),
            signature_base64: BASE64_STANDARD.encode(signing_key.sign(&payload).to_bytes()),
        };
        let mut store = ModelCatalogCacheStore::in_memory(STALE_WINDOW_MS).unwrap();
        assert_eq!(
            store
                .install_trusted(&envelope, &trust_store, 100, 1, 100)
                .unwrap(),
            CacheWrite::Installed { sequence: 1 }
        );
        assert!(
            store
                .view(200)
                .unwrap()
                .catalog
                .unwrap()
                .signature_validated
        );

        let other_key = SigningKey::from_bytes(&[8; 32]);
        let mut forged = envelope;
        forged.signature_base64 = BASE64_STANDARD.encode(other_key.sign(&payload).to_bytes());
        let mut empty = ModelCatalogCacheStore::in_memory(STALE_WINDOW_MS).unwrap();
        assert_eq!(
            empty
                .install_trusted(&forged, &trust_store, 100, 1, 100)
                .unwrap_err()
                .code,
            "model-catalog-signature-invalid"
        );
        assert_eq!(
            empty.view(100).unwrap().availability,
            CacheAvailability::Empty
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn persistent_store_reopens_and_rejects_snapshot_tampering() {
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "aegisy-model-catalog-cache-store-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(&root).unwrap();
        let mut store = ModelCatalogCacheStore::open(&root, STALE_WINDOW_MS).unwrap();
        store.install(signed_catalog(100, 1_000), 1, 100).unwrap();
        assert_eq!(
            store.view(200).unwrap().availability,
            CacheAvailability::Fresh
        );
        drop(store);

        let reopened = ModelCatalogCacheStore::open(&root, STALE_WINDOW_MS).unwrap();
        assert_eq!(
            reopened.view(1_500).unwrap().availability,
            CacheAvailability::Stale
        );
        drop(reopened);

        let snapshot_path = root.join("model-catalog-cache-v1").join("snapshot.json");
        let mut value: serde_json::Value =
            serde_json::from_slice(&fs::read(&snapshot_path).unwrap()).unwrap();
        value["store_identity"] = serde_json::Value::String("tampered".into());
        fs::write(&snapshot_path, serde_json::to_vec(&value).unwrap()).unwrap();
        let error = ModelCatalogCacheStore::open(&root, STALE_WINDOW_MS).unwrap_err();
        assert_eq!(error.code, "model-catalog-cache-snapshot-identity-mismatch");
        let _ = fs::remove_dir_all(root);
    }
}
