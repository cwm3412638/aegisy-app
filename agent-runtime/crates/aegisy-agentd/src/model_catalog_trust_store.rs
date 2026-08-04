//! Durable trust state for signed model catalogs.
//!
//! The store is anchored by an exact public root supplied by the signed host or
//! another authenticated caller. It never invents or downloads a trust root.
//! Initial key-ring admission must contain that anchor; later updates must pass
//! the monotonic rotation contract before an atomically replaced private
//! snapshot becomes authoritative.

use crate::model_catalog::ModelCatalog;
use crate::model_catalog_signature::{
    validate_key_ring_rotation, validate_trust_key, CatalogKeyRing, CatalogSignatureError,
    KeyRingWrite, SignedCatalogKeyRing, SignedModelCatalog,
};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

pub const ANCHOR_SCHEMA_VERSION: &str = "model-catalog-trust-anchor/0.1";
pub const STORE_SCHEMA_VERSION: &str = "model-catalog-trust-store/0.1";
const STORE_DIRECTORY: &str = "model-catalog-trust-v1";
const SNAPSHOT_FILE: &str = "snapshot.json";
const MAX_SNAPSHOT_BYTES: usize = 128 * 1024;
static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CatalogTrustAnchor {
    pub schema_version: String,
    pub key_id: String,
    pub public_key_base64: String,
    pub anchor_identity: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CatalogTrustStoreSnapshot {
    pub schema_version: String,
    pub anchor: CatalogTrustAnchor,
    pub current: Option<CatalogKeyRing>,
    pub store_identity: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CatalogTrustStoreWrite {
    Bootstrapped { generation: u64 },
    Rotated { generation: u64 },
    Idempotent { generation: u64 },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CatalogTrustStoreError {
    pub code: &'static str,
    pub message: &'static str,
}

#[derive(Debug)]
pub struct CatalogTrustStore {
    root: PathBuf,
    snapshot: CatalogTrustStoreSnapshot,
}

impl CatalogTrustAnchor {
    pub fn new(
        key_id: impl Into<String>,
        public_key_base64: impl Into<String>,
    ) -> Result<Self, CatalogTrustStoreError> {
        let mut anchor = Self {
            schema_version: ANCHOR_SCHEMA_VERSION.into(),
            key_id: key_id.into(),
            public_key_base64: public_key_base64.into(),
            anchor_identity: String::new(),
        };
        validate_trust_key(&anchor.key_id, &anchor.public_key_base64).map_err(signature_error)?;
        anchor.anchor_identity = anchor_identity(&anchor)?;
        validate_anchor(&anchor)?;
        Ok(anchor)
    }
}

impl CatalogTrustStore {
    pub fn open(
        data_root: &Path,
        expected_anchor: CatalogTrustAnchor,
    ) -> Result<Self, CatalogTrustStoreError> {
        validate_anchor(&expected_anchor)?;
        let metadata = fs::symlink_metadata(data_root).map_err(|_| {
            error(
                "model-catalog-trust-data-root-unavailable",
                "catalog trust data root is unavailable",
            )
        })?;
        if metadata.file_type().is_symlink() || !metadata.is_dir() {
            return Err(error(
                "model-catalog-trust-data-root-unsafe",
                "catalog trust data root is unsafe",
            ));
        }
        let data_root = data_root.canonicalize().map_err(|_| {
            error(
                "model-catalog-trust-data-root-unavailable",
                "catalog trust data root is unavailable",
            )
        })?;
        let root = data_root.join(STORE_DIRECTORY);
        let root_preexisting = match fs::symlink_metadata(&root) {
            Ok(metadata) => {
                if metadata.file_type().is_symlink() || !metadata.is_dir() {
                    return Err(error(
                        "model-catalog-trust-directory-unsafe",
                        "catalog trust directory is unsafe",
                    ));
                }
                true
            }
            Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => false,
            Err(_) => {
                return Err(error(
                    "model-catalog-trust-directory-read",
                    "catalog trust directory could not be read",
                ))
            }
        };
        create_secure_directory(&root)?;
        let root = root.canonicalize().map_err(|_| {
            error(
                "model-catalog-trust-store-unavailable",
                "catalog trust store is unavailable",
            )
        })?;
        if root != data_root.join(STORE_DIRECTORY) {
            return Err(error(
                "model-catalog-trust-store-symlink",
                "catalog trust store traverses a symlink",
            ));
        }
        let path = root.join(SNAPSHOT_FILE);
        let snapshot = match fs::symlink_metadata(&path) {
            Ok(metadata) => {
                if metadata.file_type().is_symlink() || !metadata.is_file() {
                    return Err(error(
                        "model-catalog-trust-snapshot-unsafe",
                        "catalog trust snapshot is unsafe",
                    ));
                }
                let bytes = read_limited(&path)?;
                let snapshot: CatalogTrustStoreSnapshot =
                    serde_json::from_slice(&bytes).map_err(|_| {
                        error(
                            "model-catalog-trust-snapshot-invalid",
                            "catalog trust snapshot is invalid",
                        )
                    })?;
                validate_snapshot(&snapshot)?;
                if snapshot.anchor != expected_anchor {
                    return Err(error(
                        "model-catalog-trust-anchor-mismatch",
                        "catalog trust snapshot does not match the expected anchor",
                    ));
                }
                snapshot
            }
            Err(cause) if cause.kind() == std::io::ErrorKind::NotFound && !root_preexisting => {
                empty_snapshot(expected_anchor)?
            }
            Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => {
                return Err(error(
                    "model-catalog-trust-snapshot-missing",
                    "catalog trust snapshot is missing",
                ))
            }
            Err(_) => {
                return Err(error(
                    "model-catalog-trust-snapshot-unavailable",
                    "catalog trust snapshot is unavailable",
                ))
            }
        };
        let store = Self { root, snapshot };
        store.validate()?;
        if !root_preexisting {
            store.persist()?;
        }
        Ok(store)
    }

    pub fn snapshot(&self) -> Result<CatalogTrustStoreSnapshot, CatalogTrustStoreError> {
        self.validate()?;
        Ok(self.snapshot.clone())
    }

    pub fn anchor(&self) -> Result<CatalogTrustAnchor, CatalogTrustStoreError> {
        self.validate()?;
        Ok(self.snapshot.anchor.clone())
    }

    pub fn key_ring(&self) -> Result<Option<CatalogKeyRing>, CatalogTrustStoreError> {
        self.validate()?;
        Ok(self.snapshot.current.clone())
    }

    pub fn verify_catalog(
        &self,
        signed: &SignedModelCatalog,
        now_ms: u64,
    ) -> Result<ModelCatalog, CatalogTrustStoreError> {
        self.validate()?;
        let ring = self.snapshot.current.as_ref().ok_or_else(|| {
            error(
                "model-catalog-trust-key-ring-unavailable",
                "catalog trust key ring is unavailable",
            )
        })?;
        signed.verify(ring, now_ms).map_err(signature_error)
    }

    pub fn install_signed_key_ring(
        &mut self,
        signed: &SignedCatalogKeyRing,
        now_ms: u64,
    ) -> Result<CatalogTrustStoreWrite, CatalogTrustStoreError> {
        self.validate()?;
        let write = match self.snapshot.current.as_ref() {
            None => {
                let next = signed
                    .verify_with_key(
                        &self.snapshot.anchor.key_id,
                        &self.snapshot.anchor.public_key_base64,
                        now_ms,
                    )
                    .map_err(signature_error)?;
                validate_bootstrap(&self.snapshot.anchor, &next, signed.signed_at_ms, now_ms)?;
                self.commit_key_ring(next)?;
                CatalogTrustStoreWrite::Bootstrapped {
                    generation: signed.key_ring.generation,
                }
            }
            Some(previous) => {
                let signer = previous.key(&signed.signer_key_id).ok_or_else(|| {
                    error(
                        "model-catalog-trust-rotation-signer-unknown",
                        "catalog key ring rotation signer is unknown",
                    )
                })?;
                validate_active_signer(signer, signed.signed_at_ms, now_ms)?;
                let next = signed
                    .verify_with_key(&signer.key_id, &signer.public_key_base64, now_ms)
                    .map_err(signature_error)?;
                match validate_key_ring_rotation(previous, &next).map_err(signature_error)? {
                    KeyRingWrite::Installed { generation } => {
                        self.commit_key_ring(next)?;
                        CatalogTrustStoreWrite::Rotated { generation }
                    }
                    KeyRingWrite::Idempotent { generation } => {
                        return Ok(CatalogTrustStoreWrite::Idempotent { generation })
                    }
                }
            }
        };
        Ok(write)
    }

    fn commit_key_ring(&mut self, next: CatalogKeyRing) -> Result<(), CatalogTrustStoreError> {
        let previous = self.snapshot.clone();
        self.snapshot.current = Some(next);
        self.snapshot.store_identity = snapshot_identity(&self.snapshot)?;
        if let Err(cause) = self.persist() {
            self.snapshot = previous;
            return Err(cause);
        }
        Ok(())
    }

    fn validate(&self) -> Result<(), CatalogTrustStoreError> {
        validate_snapshot(&self.snapshot)
    }

    fn persist(&self) -> Result<(), CatalogTrustStoreError> {
        self.validate()?;
        let bytes = serde_json::to_vec(&self.snapshot).map_err(|_| {
            error(
                "model-catalog-trust-snapshot-serialize",
                "catalog trust snapshot could not be serialized",
            )
        })?;
        if bytes.len() > MAX_SNAPSHOT_BYTES {
            return Err(error(
                "model-catalog-trust-snapshot-too-large",
                "catalog trust snapshot exceeds its size limit",
            ));
        }
        let target = self.root.join(SNAPSHOT_FILE);
        if let Ok(metadata) = fs::symlink_metadata(&target) {
            if metadata.file_type().is_symlink() || !metadata.is_file() {
                return Err(error(
                    "model-catalog-trust-snapshot-unsafe",
                    "catalog trust snapshot is unsafe",
                ));
            }
        }
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let temporary = self.root.join(format!(".snapshot.{sequence}.tmp"));
        let result = (|| {
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&temporary)
                .map_err(|_| {
                    error(
                        "model-catalog-trust-snapshot-stage",
                        "catalog trust snapshot staging failed",
                    )
                })?;
            secure_file(&file)?;
            file.write_all(&bytes)
                .and_then(|()| file.sync_all())
                .map_err(|_| {
                    error(
                        "model-catalog-trust-snapshot-sync",
                        "catalog trust snapshot sync failed",
                    )
                })?;
            replace_file(&temporary, &target)
        })();
        let _ = fs::remove_file(&temporary);
        result?;
        sync_directory(&self.root)
    }
}

fn validate_bootstrap(
    anchor: &CatalogTrustAnchor,
    ring: &CatalogKeyRing,
    signed_at_ms: u64,
    now_ms: u64,
) -> Result<(), CatalogTrustStoreError> {
    if ring.generation != 1 {
        return Err(error(
            "model-catalog-trust-bootstrap-generation-invalid",
            "initial catalog key ring generation must be one",
        ));
    }
    let key = ring.key(&anchor.key_id).ok_or_else(|| {
        error(
            "model-catalog-trust-bootstrap-anchor-missing",
            "initial catalog key ring is missing the trust anchor",
        )
    })?;
    if key.public_key_base64 != anchor.public_key_base64 || key.revoked || key.replaces.is_some() {
        return Err(error(
            "model-catalog-trust-bootstrap-anchor-mismatch",
            "initial catalog key ring does not preserve the trust anchor",
        ));
    }
    validate_active_signer(key, signed_at_ms, now_ms)?;
    Ok(())
}

fn validate_active_signer(
    key: &crate::model_catalog_signature::CatalogSigningKey,
    signed_at_ms: u64,
    now_ms: u64,
) -> Result<(), CatalogTrustStoreError> {
    if key.revoked
        || signed_at_ms < key.valid_from_ms
        || now_ms < key.valid_from_ms
        || key
            .valid_until_ms
            .is_some_and(|until| signed_at_ms >= until || now_ms >= until)
    {
        return Err(error(
            "model-catalog-trust-rotation-signer-inactive",
            "catalog key ring rotation signer is inactive",
        ));
    }
    Ok(())
}

fn empty_snapshot(
    anchor: CatalogTrustAnchor,
) -> Result<CatalogTrustStoreSnapshot, CatalogTrustStoreError> {
    let mut snapshot = CatalogTrustStoreSnapshot {
        schema_version: STORE_SCHEMA_VERSION.into(),
        anchor,
        current: None,
        store_identity: String::new(),
    };
    snapshot.store_identity = snapshot_identity(&snapshot)?;
    validate_snapshot(&snapshot)?;
    Ok(snapshot)
}

fn validate_anchor(anchor: &CatalogTrustAnchor) -> Result<(), CatalogTrustStoreError> {
    if anchor.schema_version != ANCHOR_SCHEMA_VERSION {
        return Err(error(
            "model-catalog-trust-anchor-schema-unsupported",
            "catalog trust anchor schema is unsupported",
        ));
    }
    validate_trust_key(&anchor.key_id, &anchor.public_key_base64).map_err(signature_error)?;
    if anchor.anchor_identity != anchor_identity(anchor)? {
        return Err(error(
            "model-catalog-trust-anchor-identity-mismatch",
            "catalog trust anchor identity does not match",
        ));
    }
    Ok(())
}

fn validate_snapshot(snapshot: &CatalogTrustStoreSnapshot) -> Result<(), CatalogTrustStoreError> {
    if snapshot.schema_version != STORE_SCHEMA_VERSION {
        return Err(error(
            "model-catalog-trust-store-schema-unsupported",
            "catalog trust store schema is unsupported",
        ));
    }
    validate_anchor(&snapshot.anchor)?;
    if let Some(ring) = &snapshot.current {
        ring.validate().map_err(signature_error)?;
        let anchor_key = ring.key(&snapshot.anchor.key_id).ok_or_else(|| {
            error(
                "model-catalog-trust-anchor-history-missing",
                "catalog trust key ring lost its anchor history",
            )
        })?;
        if anchor_key.public_key_base64 != snapshot.anchor.public_key_base64
            || anchor_key.replaces.is_some()
        {
            return Err(error(
                "model-catalog-trust-anchor-history-mismatch",
                "catalog trust key ring rewrote its anchor history",
            ));
        }
    }
    if snapshot.store_identity != snapshot_identity(snapshot)? {
        return Err(error(
            "model-catalog-trust-store-identity-mismatch",
            "catalog trust store identity does not match",
        ));
    }
    let bytes = serde_json::to_vec(snapshot).map_err(|_| {
        error(
            "model-catalog-trust-snapshot-serialize",
            "catalog trust snapshot could not be serialized",
        )
    })?;
    if bytes.len() > MAX_SNAPSHOT_BYTES {
        return Err(error(
            "model-catalog-trust-snapshot-too-large",
            "catalog trust snapshot exceeds its size limit",
        ));
    }
    Ok(())
}

fn anchor_identity(anchor: &CatalogTrustAnchor) -> Result<String, CatalogTrustStoreError> {
    let mut copy = anchor.clone();
    copy.anchor_identity.clear();
    let bytes = serde_json::to_vec(&copy).map_err(|_| {
        error(
            "model-catalog-trust-anchor-serialize",
            "catalog trust anchor could not be serialized",
        )
    })?;
    Ok(format!(
        "model-catalog-trust-anchor:sha256:{:x}",
        Sha256::digest(bytes)
    ))
}

fn snapshot_identity(
    snapshot: &CatalogTrustStoreSnapshot,
) -> Result<String, CatalogTrustStoreError> {
    let mut copy = snapshot.clone();
    copy.store_identity.clear();
    let bytes = serde_json::to_vec(&copy).map_err(|_| {
        error(
            "model-catalog-trust-snapshot-serialize",
            "catalog trust snapshot could not be serialized",
        )
    })?;
    Ok(format!(
        "model-catalog-trust-store:sha256:{:x}",
        Sha256::digest(bytes)
    ))
}

fn read_limited(path: &Path) -> Result<Vec<u8>, CatalogTrustStoreError> {
    let metadata = fs::symlink_metadata(path).map_err(|_| {
        error(
            "model-catalog-trust-snapshot-read",
            "catalog trust snapshot could not be read",
        )
    })?;
    if metadata.file_type().is_symlink()
        || !metadata.is_file()
        || metadata.len() > MAX_SNAPSHOT_BYTES as u64
    {
        return Err(error(
            "model-catalog-trust-snapshot-size-or-layout",
            "catalog trust snapshot exceeds its size or layout limit",
        ));
    }
    let mut file = File::open(path).map_err(|_| {
        error(
            "model-catalog-trust-snapshot-read",
            "catalog trust snapshot could not be read",
        )
    })?;
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    file.read_to_end(&mut bytes).map_err(|_| {
        error(
            "model-catalog-trust-snapshot-read",
            "catalog trust snapshot could not be read",
        )
    })?;
    if bytes.len() > MAX_SNAPSHOT_BYTES {
        return Err(error(
            "model-catalog-trust-snapshot-size-or-layout",
            "catalog trust snapshot exceeds its size or layout limit",
        ));
    }
    Ok(bytes)
}

fn create_secure_directory(path: &Path) -> Result<(), CatalogTrustStoreError> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() || !metadata.is_dir() => Err(error(
            "model-catalog-trust-directory-unsafe",
            "catalog trust directory is unsafe",
        )),
        Ok(_) => secure_directory(path),
        Err(cause) if cause.kind() == std::io::ErrorKind::NotFound => {
            fs::create_dir(path).map_err(|_| {
                error(
                    "model-catalog-trust-directory-create",
                    "catalog trust directory could not be created",
                )
            })?;
            secure_directory(path)
        }
        Err(_) => Err(error(
            "model-catalog-trust-directory-read",
            "catalog trust directory could not be read",
        )),
    }
}

fn secure_directory(path: &Path) -> Result<(), CatalogTrustStoreError> {
    #[cfg(not(unix))]
    let _ = path;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(path, fs::Permissions::from_mode(0o700)).map_err(|_| {
            error(
                "model-catalog-trust-directory-permission",
                "catalog trust directory permissions could not be set",
            )
        })?;
    }
    Ok(())
}

fn secure_file(file: &File) -> Result<(), CatalogTrustStoreError> {
    #[cfg(not(unix))]
    let _ = file;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        file.set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(|_| {
                error(
                    "model-catalog-trust-file-permission",
                    "catalog trust file permissions could not be set",
                )
            })?;
    }
    Ok(())
}

fn sync_directory(path: &Path) -> Result<(), CatalogTrustStoreError> {
    #[cfg(not(unix))]
    let _ = path;
    #[cfg(unix)]
    File::open(path)
        .and_then(|directory| directory.sync_all())
        .map_err(|_| {
            error(
                "model-catalog-trust-directory-sync",
                "catalog trust directory sync failed",
            )
        })?;
    Ok(())
}

#[cfg(unix)]
fn replace_file(source: &Path, target: &Path) -> Result<(), CatalogTrustStoreError> {
    fs::rename(source, target).map_err(|_| {
        error(
            "model-catalog-trust-snapshot-commit",
            "catalog trust snapshot commit failed",
        )
    })
}

#[cfg(target_os = "windows")]
fn replace_file(source: &Path, target: &Path) -> Result<(), CatalogTrustStoreError> {
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
            "model-catalog-trust-snapshot-commit",
            "catalog trust snapshot commit failed",
        ));
    }
    Ok(())
}

#[cfg(not(any(unix, target_os = "windows")))]
fn replace_file(source: &Path, target: &Path) -> Result<(), CatalogTrustStoreError> {
    if target.exists() {
        return Err(error(
            "model-catalog-trust-snapshot-commit",
            "catalog trust snapshot replacement is unsupported",
        ));
    }
    fs::rename(source, target).map_err(|_| {
        error(
            "model-catalog-trust-snapshot-commit",
            "catalog trust snapshot commit failed",
        )
    })
}

fn signature_error(cause: CatalogSignatureError) -> CatalogTrustStoreError {
    error(cause.code, cause.message)
}

fn error(code: &'static str, message: &'static str) -> CatalogTrustStoreError {
    CatalogTrustStoreError { code, message }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model_catalog::{offline_for_runtime, CatalogState};
    use crate::model_catalog_cache::{CacheAvailability, ModelCatalogCacheStore};
    use crate::model_catalog_signature::{
        key_ring_signing_payload_bytes, key_ring_signing_payload_identity, signing_payload_bytes,
        signing_payload_identity, CatalogSigningKey, KEY_RING_SIGNATURE_SCHEMA_VERSION,
        SIGNATURE_SCHEMA_VERSION,
    };
    use base64::engine::general_purpose::STANDARD as BASE64_STANDARD;
    use base64::Engine;
    use ed25519_dalek::{Signer, SigningKey};

    fn signing_key(byte: u8) -> SigningKey {
        SigningKey::from_bytes(&[byte; 32])
    }

    fn encoded_key(key: &SigningKey) -> String {
        BASE64_STANDARD.encode(key.verifying_key().to_bytes())
    }

    fn anchor(key_id: &str, key: &SigningKey) -> CatalogTrustAnchor {
        CatalogTrustAnchor::new(key_id, encoded_key(key)).unwrap()
    }

    fn ring_one(key: &SigningKey) -> CatalogKeyRing {
        CatalogKeyRing::new(
            1,
            vec![CatalogSigningKey {
                key_id: "catalog-key-1".into(),
                public_key_base64: encoded_key(key),
                valid_from_ms: 1,
                valid_until_ms: Some(10_000),
                revoked: false,
                replaces: None,
            }],
        )
        .unwrap()
    }

    fn ring_two(first: &SigningKey, second: &SigningKey) -> CatalogKeyRing {
        CatalogKeyRing::new(
            2,
            vec![
                CatalogSigningKey {
                    key_id: "catalog-key-1".into(),
                    public_key_base64: encoded_key(first),
                    valid_from_ms: 1,
                    valid_until_ms: Some(10_000),
                    revoked: false,
                    replaces: None,
                },
                CatalogSigningKey {
                    key_id: "catalog-key-2".into(),
                    public_key_base64: encoded_key(second),
                    valid_from_ms: 500,
                    valid_until_ms: Some(20_000),
                    revoked: false,
                    replaces: Some("catalog-key-1".into()),
                },
            ],
        )
        .unwrap()
    }

    fn signed_catalog(key_id: &str, key: &SigningKey) -> SignedModelCatalog {
        let mut catalog = offline_for_runtime("codex", "0.144.5", Some("aegisy"), Some("agent"));
        catalog.state = CatalogState::Fresh;
        catalog.catalog_version = "catalog-1".into();
        catalog.source = "aegisy-cloud".into();
        catalog.issued_at_ms = Some(700);
        catalog.expires_at_ms = Some(2_000);
        catalog.validation_errors.clear();
        let payload = signing_payload_bytes(key_id, &catalog).unwrap();
        SignedModelCatalog {
            schema_version: SIGNATURE_SCHEMA_VERSION.into(),
            key_id: key_id.into(),
            catalog,
            payload_identity: signing_payload_identity(&payload),
            signature_base64: BASE64_STANDARD.encode(key.sign(&payload).to_bytes()),
        }
    }

    fn signed_ring(
        signer_key_id: &str,
        signer: &SigningKey,
        signed_at_ms: u64,
        key_ring: CatalogKeyRing,
    ) -> SignedCatalogKeyRing {
        let payload =
            key_ring_signing_payload_bytes(signer_key_id, signed_at_ms, &key_ring).unwrap();
        SignedCatalogKeyRing {
            schema_version: KEY_RING_SIGNATURE_SCHEMA_VERSION.into(),
            signer_key_id: signer_key_id.into(),
            signed_at_ms,
            key_ring,
            payload_identity: key_ring_signing_payload_identity(&payload),
            signature_base64: BASE64_STANDARD.encode(signer.sign(&payload).to_bytes()),
        }
    }

    fn temporary_root(name: &str) -> PathBuf {
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "aegisy-model-catalog-trust-{name}-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(&root).unwrap();
        root
    }

    #[test]
    fn bootstraps_rotates_reopens_and_verifies_with_pinned_anchor() {
        let root = temporary_root("lifecycle");
        let first = signing_key(7);
        let second = signing_key(8);
        let expected_anchor = anchor("catalog-key-1", &first);
        let store = CatalogTrustStore::open(&root, expected_anchor.clone()).unwrap();
        assert!(store.key_ring().unwrap().is_none());
        assert!(root.join(STORE_DIRECTORY).join(SNAPSHOT_FILE).is_file());
        drop(store);

        let mut store = CatalogTrustStore::open(&root, expected_anchor.clone()).unwrap();
        assert!(store.key_ring().unwrap().is_none());
        assert_eq!(
            store
                .install_signed_key_ring(
                    &signed_ring("catalog-key-1", &first, 100, ring_one(&first)),
                    100,
                )
                .unwrap(),
            CatalogTrustStoreWrite::Bootstrapped { generation: 1 }
        );
        assert!(
            store
                .verify_catalog(&signed_catalog("catalog-key-1", &first), 1_000)
                .unwrap()
                .signature_validated
        );
        drop(store);

        let mut reopened = CatalogTrustStore::open(&root, expected_anchor.clone()).unwrap();
        assert_eq!(reopened.key_ring().unwrap().unwrap().generation, 1);
        assert_eq!(
            reopened
                .install_signed_key_ring(
                    &signed_ring("catalog-key-1", &first, 500, ring_two(&first, &second),),
                    500,
                )
                .unwrap(),
            CatalogTrustStoreWrite::Rotated { generation: 2 }
        );
        drop(reopened);

        let reopened = CatalogTrustStore::open(&root, expected_anchor).unwrap();
        assert_eq!(reopened.key_ring().unwrap().unwrap().generation, 2);
        assert!(
            reopened
                .verify_catalog(&signed_catalog("catalog-key-2", &second), 1_000)
                .unwrap()
                .signature_validated
        );
        let mut cache = ModelCatalogCacheStore::in_memory(1_000).unwrap();
        cache
            .install_trusted(
                &signed_catalog("catalog-key-2", &second),
                &reopened,
                1_000,
                1,
                1_000,
            )
            .unwrap();
        assert_eq!(
            cache.view(1_100).unwrap().availability,
            CacheAvailability::Fresh
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_wrong_anchor_and_snapshot_tampering_on_restart() {
        let root = temporary_root("restart");
        let first = signing_key(7);
        let other = signing_key(8);
        let expected_anchor = anchor("catalog-key-1", &first);
        let mut store = CatalogTrustStore::open(&root, expected_anchor.clone()).unwrap();
        store
            .install_signed_key_ring(
                &signed_ring("catalog-key-1", &first, 100, ring_one(&first)),
                100,
            )
            .unwrap();
        drop(store);

        assert_eq!(
            CatalogTrustStore::open(&root, anchor("catalog-key-1", &other))
                .unwrap_err()
                .code,
            "model-catalog-trust-anchor-mismatch"
        );

        let path = root.join(STORE_DIRECTORY).join(SNAPSHOT_FILE);
        let mut value: serde_json::Value =
            serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
        value["store_identity"] = serde_json::Value::String("tampered".into());
        fs::write(&path, serde_json::to_vec(&value).unwrap()).unwrap();
        assert_eq!(
            CatalogTrustStore::open(&root, expected_anchor)
                .unwrap_err()
                .code,
            "model-catalog-trust-store-identity-mismatch"
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn bootstrap_requires_root_signature_and_rotation_rejects_generation_gap() {
        let root = temporary_root("admission");
        let first = signing_key(7);
        let other = signing_key(8);
        let mut store = CatalogTrustStore::open(&root, anchor("catalog-key-1", &first)).unwrap();
        assert_eq!(
            store
                .install_signed_key_ring(
                    &signed_ring("catalog-key-1", &other, 100, ring_one(&other)),
                    100,
                )
                .unwrap_err()
                .code,
            "model-catalog-key-ring-signature-invalid"
        );
        assert!(store.key_ring().unwrap().is_none());
        store
            .install_signed_key_ring(
                &signed_ring("catalog-key-1", &first, 100, ring_one(&first)),
                100,
            )
            .unwrap();

        let mut gap = ring_two(&first, &other);
        gap.generation = 3;
        gap.ring_identity = String::new();
        gap = CatalogKeyRing::new(3, gap.keys).unwrap();
        assert_eq!(
            store
                .install_signed_key_ring(&signed_ring("catalog-key-1", &first, 200, gap), 200,)
                .unwrap_err()
                .code,
            "model-catalog-key-ring-generation-gap"
        );
        assert_eq!(store.key_ring().unwrap().unwrap().generation, 1);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rotation_requires_a_current_active_signer() {
        let root = temporary_root("rotation-signer");
        let first = signing_key(7);
        let second = signing_key(8);
        let third = signing_key(9);
        let mut store = CatalogTrustStore::open(&root, anchor("catalog-key-1", &first)).unwrap();
        store
            .install_signed_key_ring(
                &signed_ring("catalog-key-1", &first, 100, ring_one(&first)),
                100,
            )
            .unwrap();

        assert_eq!(
            store
                .install_signed_key_ring(
                    &signed_ring("catalog-key-2", &second, 500, ring_two(&first, &second),),
                    500,
                )
                .unwrap_err()
                .code,
            "model-catalog-trust-rotation-signer-unknown"
        );
        assert_eq!(store.key_ring().unwrap().unwrap().generation, 1);

        assert_eq!(
            store
                .install_signed_key_ring(
                    &signed_ring("catalog-key-1", &first, 9_999, ring_two(&first, &second),),
                    10_000,
                )
                .unwrap_err()
                .code,
            "model-catalog-trust-rotation-signer-inactive"
        );
        assert_eq!(store.key_ring().unwrap().unwrap().generation, 1);

        let mut revoked_ring = ring_two(&first, &second);
        revoked_ring.keys[0].revoked = true;
        let revoked_ring = CatalogKeyRing::new(2, revoked_ring.keys).unwrap();
        store
            .install_signed_key_ring(
                &signed_ring("catalog-key-1", &first, 500, revoked_ring.clone()),
                500,
            )
            .unwrap();
        let mut generation_three_keys = revoked_ring.keys;
        generation_three_keys.push(CatalogSigningKey {
            key_id: "catalog-key-3".into(),
            public_key_base64: encoded_key(&third),
            valid_from_ms: 600,
            valid_until_ms: Some(30_000),
            revoked: false,
            replaces: Some("catalog-key-2".into()),
        });
        let generation_three = CatalogKeyRing::new(3, generation_three_keys).unwrap();
        assert_eq!(
            store
                .install_signed_key_ring(
                    &signed_ring("catalog-key-1", &first, 600, generation_three),
                    600,
                )
                .unwrap_err()
                .code,
            "model-catalog-trust-rotation-signer-inactive"
        );
        assert_eq!(store.key_ring().unwrap().unwrap().generation, 2);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn failed_snapshot_replacement_rolls_back_in_memory_authority() {
        let root = temporary_root("rollback");
        let first = signing_key(7);
        let second = signing_key(8);
        let expected_anchor = anchor("catalog-key-1", &first);
        let mut store = CatalogTrustStore::open(&root, expected_anchor.clone()).unwrap();
        store
            .install_signed_key_ring(
                &signed_ring("catalog-key-1", &first, 100, ring_one(&first)),
                100,
            )
            .unwrap();

        let path = root.join(STORE_DIRECTORY).join(SNAPSHOT_FILE);
        fs::remove_file(&path).unwrap();
        fs::create_dir(&path).unwrap();
        assert_eq!(
            store
                .install_signed_key_ring(
                    &signed_ring("catalog-key-1", &first, 500, ring_two(&first, &second),),
                    500,
                )
                .unwrap_err()
                .code,
            "model-catalog-trust-snapshot-unsafe"
        );
        assert_eq!(store.key_ring().unwrap().unwrap().generation, 1);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn deleted_snapshot_cannot_rebootstrap_generation_one() {
        let root = temporary_root("deleted");
        let first = signing_key(7);
        let expected_anchor = anchor("catalog-key-1", &first);
        let mut store = CatalogTrustStore::open(&root, expected_anchor.clone()).unwrap();
        store
            .install_signed_key_ring(
                &signed_ring("catalog-key-1", &first, 100, ring_one(&first)),
                100,
            )
            .unwrap();
        drop(store);

        fs::remove_file(root.join(STORE_DIRECTORY).join(SNAPSHOT_FILE)).unwrap();
        assert_eq!(
            CatalogTrustStore::open(&root, expected_anchor)
                .unwrap_err()
                .code,
            "model-catalog-trust-snapshot-missing"
        );
        fs::remove_dir_all(root).unwrap();
    }
}
