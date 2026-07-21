//! Cryptographic trust boundary for model catalog metadata.
//!
//! The desktop/runtime verifies Ed25519 catalog envelopes against a caller-
//! supplied trusted key ring. This module never stores private keys, fetches a
//! key ring, authenticates its publication channel, or grants model-selection
//! authority by itself. Key-ring persistence and authenticated cloud refresh
//! remain separate host/backend responsibilities.

use crate::model_catalog::{CatalogState, ModelCatalog};
use base64::engine::general_purpose::STANDARD as BASE64_STANDARD;
use base64::Engine;
use ed25519_dalek::{Signature, VerifyingKey};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};

pub const SIGNATURE_SCHEMA_VERSION: &str = "model-catalog-signature/0.1";
pub const KEY_RING_SCHEMA_VERSION: &str = "model-catalog-key-ring/0.1";
pub const KEY_RING_SIGNATURE_SCHEMA_VERSION: &str = "model-catalog-key-ring-signature/0.1";
const MAX_KEYS: usize = 32;
const MAX_KEY_ID_BYTES: usize = 128;
const MAX_ENCODED_KEY_BYTES: usize = 128;
const MAX_ENCODED_SIGNATURE_BYTES: usize = 256;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CatalogSigningKey {
    pub key_id: String,
    pub public_key_base64: String,
    pub valid_from_ms: u64,
    pub valid_until_ms: Option<u64>,
    pub revoked: bool,
    pub replaces: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CatalogKeyRing {
    pub schema_version: String,
    pub generation: u64,
    pub keys: Vec<CatalogSigningKey>,
    pub ring_identity: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SignedModelCatalog {
    pub schema_version: String,
    pub key_id: String,
    pub catalog: ModelCatalog,
    pub payload_identity: String,
    pub signature_base64: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SignedCatalogKeyRing {
    pub schema_version: String,
    pub signer_key_id: String,
    pub signed_at_ms: u64,
    pub key_ring: CatalogKeyRing,
    pub payload_identity: String,
    pub signature_base64: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum KeyRingWrite {
    Installed { generation: u64 },
    Idempotent { generation: u64 },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CatalogSignatureError {
    pub code: &'static str,
    pub message: &'static str,
}

#[derive(Serialize)]
struct CatalogSigningPayload<'a> {
    schema_version: &'static str,
    key_id: &'a str,
    catalog: &'a ModelCatalog,
}

#[derive(Serialize)]
struct KeyRingSigningPayload<'a> {
    schema_version: &'static str,
    signer_key_id: &'a str,
    signed_at_ms: u64,
    key_ring: &'a CatalogKeyRing,
}

impl CatalogKeyRing {
    pub fn new(
        generation: u64,
        keys: Vec<CatalogSigningKey>,
    ) -> Result<Self, CatalogSignatureError> {
        let mut ring = Self {
            schema_version: KEY_RING_SCHEMA_VERSION.into(),
            generation,
            keys,
            ring_identity: String::new(),
        };
        ring.ring_identity = ring_identity(&ring)?;
        ring.validate()?;
        Ok(ring)
    }

    pub fn validate(&self) -> Result<(), CatalogSignatureError> {
        if self.schema_version != KEY_RING_SCHEMA_VERSION {
            return Err(error(
                "model-catalog-key-ring-schema-unsupported",
                "catalog key ring schema is unsupported",
            ));
        }
        if self.generation == 0 {
            return Err(error(
                "model-catalog-key-ring-generation-invalid",
                "catalog key ring generation is invalid",
            ));
        }
        if self.keys.is_empty() || self.keys.len() > MAX_KEYS {
            return Err(error(
                "model-catalog-key-ring-size-invalid",
                "catalog key ring size is invalid",
            ));
        }
        let mut ids = BTreeSet::new();
        let mut active = 0usize;
        for key in &self.keys {
            validate_key_id(&key.key_id)?;
            if !ids.insert(key.key_id.as_str()) {
                return Err(error(
                    "model-catalog-key-ring-duplicate-key",
                    "catalog key IDs must be unique",
                ));
            }
            if key
                .valid_until_ms
                .is_some_and(|until| until <= key.valid_from_ms)
            {
                return Err(error(
                    "model-catalog-key-ring-validity-invalid",
                    "catalog signing key validity is invalid",
                ));
            }
            if let Some(replaces) = key.replaces.as_deref() {
                validate_key_id(replaces)?;
                if replaces == key.key_id {
                    return Err(error(
                        "model-catalog-key-ring-replacement-invalid",
                        "catalog signing key cannot replace itself",
                    ));
                }
            }
            decode_verifying_key(&key.public_key_base64)?;
            if !key.revoked {
                active = active.saturating_add(1);
            }
        }
        if active == 0 {
            return Err(error(
                "model-catalog-key-ring-no-active-key",
                "catalog key ring has no active key",
            ));
        }
        if self.ring_identity != ring_identity(self)? {
            return Err(error(
                "model-catalog-key-ring-identity-mismatch",
                "catalog key ring identity does not match",
            ));
        }
        Ok(())
    }

    pub fn key(&self, key_id: &str) -> Option<&CatalogSigningKey> {
        self.keys.iter().find(|key| key.key_id == key_id)
    }
}

impl SignedModelCatalog {
    pub fn verify(
        &self,
        key_ring: &CatalogKeyRing,
        now_ms: u64,
    ) -> Result<ModelCatalog, CatalogSignatureError> {
        key_ring.validate()?;
        if self.schema_version != SIGNATURE_SCHEMA_VERSION {
            return Err(error(
                "model-catalog-signature-schema-unsupported",
                "catalog signature schema is unsupported",
            ));
        }
        validate_key_id(&self.key_id)?;
        if self.catalog.state != CatalogState::Fresh || self.catalog.signature_validated {
            return Err(error(
                "model-catalog-signature-state-invalid",
                "signed catalog must be fresh and unvalidated before verification",
            ));
        }
        let mut catalog = self.catalog.clone();
        catalog.signature_validated = true;
        catalog.validate().map_err(|_| {
            error(
                "model-catalog-signature-catalog-invalid",
                "signed catalog failed schema validation",
            )
        })?;
        let issued_at_ms = self.catalog.issued_at_ms.ok_or_else(|| {
            error(
                "model-catalog-signature-issued-at-missing",
                "signed catalog issue time is missing",
            )
        })?;
        let expires_at_ms = self.catalog.expires_at_ms.ok_or_else(|| {
            error(
                "model-catalog-signature-expiry-missing",
                "signed catalog expiry is missing",
            )
        })?;
        if issued_at_ms > now_ms || expires_at_ms <= now_ms || expires_at_ms <= issued_at_ms {
            return Err(error(
                "model-catalog-signature-time-invalid",
                "signed catalog time window is invalid",
            ));
        }
        let key = key_ring.key(&self.key_id).ok_or_else(|| {
            error(
                "model-catalog-signature-key-unknown",
                "catalog signature key is unknown",
            )
        })?;
        if key.revoked
            || issued_at_ms < key.valid_from_ms
            || now_ms < key.valid_from_ms
            || key
                .valid_until_ms
                .is_some_and(|until| issued_at_ms >= until || now_ms >= until)
        {
            return Err(error(
                "model-catalog-signature-key-inactive",
                "catalog signature key is inactive",
            ));
        }
        let payload = signing_payload_bytes(&self.key_id, &self.catalog)?;
        if self.payload_identity != payload_identity(&payload) {
            return Err(error(
                "model-catalog-signature-payload-identity-mismatch",
                "catalog signature payload identity does not match",
            ));
        }
        let signature = decode_signature(&self.signature_base64)?;
        let verifying_key = decode_verifying_key(&key.public_key_base64)?;
        verifying_key
            .verify_strict(&payload, &signature)
            .map_err(|_| {
                error(
                    "model-catalog-signature-invalid",
                    "catalog signature is invalid",
                )
            })?;

        Ok(catalog)
    }
}

impl SignedCatalogKeyRing {
    pub(crate) fn verify_with_key(
        &self,
        expected_signer_key_id: &str,
        public_key_base64: &str,
        now_ms: u64,
    ) -> Result<CatalogKeyRing, CatalogSignatureError> {
        if self.schema_version != KEY_RING_SIGNATURE_SCHEMA_VERSION {
            return Err(error(
                "model-catalog-key-ring-signature-schema-unsupported",
                "catalog key ring signature schema is unsupported",
            ));
        }
        validate_key_id(&self.signer_key_id)?;
        if self.signer_key_id != expected_signer_key_id {
            return Err(error(
                "model-catalog-key-ring-signature-signer-mismatch",
                "catalog key ring signer does not match trusted authority",
            ));
        }
        if self.signed_at_ms == 0 || self.signed_at_ms > now_ms {
            return Err(error(
                "model-catalog-key-ring-signature-time-invalid",
                "catalog key ring signature time is invalid",
            ));
        }
        self.key_ring.validate()?;
        let payload =
            key_ring_signing_payload_bytes(&self.signer_key_id, self.signed_at_ms, &self.key_ring)?;
        if self.payload_identity != key_ring_payload_identity(&payload) {
            return Err(error(
                "model-catalog-key-ring-signature-payload-identity-mismatch",
                "catalog key ring signature payload identity does not match",
            ));
        }
        let signature = decode_signature(&self.signature_base64)?;
        let verifying_key = decode_verifying_key(public_key_base64)?;
        verifying_key
            .verify_strict(&payload, &signature)
            .map_err(|_| {
                error(
                    "model-catalog-key-ring-signature-invalid",
                    "catalog key ring signature is invalid",
                )
            })?;
        Ok(self.key_ring.clone())
    }
}

pub fn validate_key_ring_rotation(
    previous: &CatalogKeyRing,
    next: &CatalogKeyRing,
) -> Result<KeyRingWrite, CatalogSignatureError> {
    previous.validate()?;
    next.validate()?;
    if next.generation < previous.generation {
        return Err(error(
            "model-catalog-key-ring-rollback",
            "catalog key ring generation cannot roll back",
        ));
    }
    if next.generation == previous.generation {
        if next.ring_identity == previous.ring_identity {
            return Ok(KeyRingWrite::Idempotent {
                generation: next.generation,
            });
        }
        return Err(error(
            "model-catalog-key-ring-generation-conflict",
            "catalog key ring generation conflicts with existing content",
        ));
    }
    if next.generation != previous.generation.saturating_add(1) {
        return Err(error(
            "model-catalog-key-ring-generation-gap",
            "catalog key ring generation must advance by one",
        ));
    }

    let previous_by_id = previous
        .keys
        .iter()
        .map(|key| (key.key_id.as_str(), key))
        .collect::<BTreeMap<_, _>>();
    let next_by_id = next
        .keys
        .iter()
        .map(|key| (key.key_id.as_str(), key))
        .collect::<BTreeMap<_, _>>();
    for (key_id, previous_key) in &previous_by_id {
        let next_key = next_by_id.get(key_id).ok_or_else(|| {
            error(
                "model-catalog-key-ring-key-removed",
                "catalog key rotation cannot remove prior key history",
            )
        })?;
        if next_key.public_key_base64 != previous_key.public_key_base64
            || next_key.valid_from_ms != previous_key.valid_from_ms
            || next_key.replaces != previous_key.replaces
            || (previous_key.revoked && !next_key.revoked)
            || validity_widened(previous_key.valid_until_ms, next_key.valid_until_ms)
        {
            return Err(error(
                "model-catalog-key-ring-key-rewritten",
                "catalog key rotation cannot rewrite prior key authority",
            ));
        }
    }

    for key in next
        .keys
        .iter()
        .filter(|key| !previous_by_id.contains_key(key.key_id.as_str()))
    {
        let replaces = key.replaces.as_deref().ok_or_else(|| {
            error(
                "model-catalog-key-ring-lineage-missing",
                "new catalog signing key requires replacement lineage",
            )
        })?;
        if !previous_by_id.contains_key(replaces) {
            return Err(error(
                "model-catalog-key-ring-lineage-unknown",
                "new catalog signing key replaces an unknown prior key",
            ));
        }
    }
    Ok(KeyRingWrite::Installed {
        generation: next.generation,
    })
}

pub fn signing_payload_bytes(
    key_id: &str,
    catalog: &ModelCatalog,
) -> Result<Vec<u8>, CatalogSignatureError> {
    validate_key_id(key_id)?;
    serde_json::to_vec(&CatalogSigningPayload {
        schema_version: SIGNATURE_SCHEMA_VERSION,
        key_id,
        catalog,
    })
    .map_err(|_| {
        error(
            "model-catalog-signature-payload-serialize",
            "catalog signing payload could not be serialized",
        )
    })
}

pub fn signing_payload_identity(payload: &[u8]) -> String {
    payload_identity(payload)
}

pub fn key_ring_signing_payload_bytes(
    signer_key_id: &str,
    signed_at_ms: u64,
    key_ring: &CatalogKeyRing,
) -> Result<Vec<u8>, CatalogSignatureError> {
    validate_key_id(signer_key_id)?;
    if signed_at_ms == 0 {
        return Err(error(
            "model-catalog-key-ring-signature-time-invalid",
            "catalog key ring signature time is invalid",
        ));
    }
    key_ring.validate()?;
    serde_json::to_vec(&KeyRingSigningPayload {
        schema_version: KEY_RING_SIGNATURE_SCHEMA_VERSION,
        signer_key_id,
        signed_at_ms,
        key_ring,
    })
    .map_err(|_| {
        error(
            "model-catalog-key-ring-signature-payload-serialize",
            "catalog key ring signature payload could not be serialized",
        )
    })
}

pub fn key_ring_signing_payload_identity(payload: &[u8]) -> String {
    key_ring_payload_identity(payload)
}

pub(crate) fn validate_trust_key(
    key_id: &str,
    public_key_base64: &str,
) -> Result<(), CatalogSignatureError> {
    validate_key_id(key_id)?;
    decode_verifying_key(public_key_base64)?;
    Ok(())
}

fn validity_widened(previous: Option<u64>, next: Option<u64>) -> bool {
    match (previous, next) {
        (Some(previous), Some(next)) => next > previous,
        (Some(_), None) => true,
        _ => false,
    }
}

fn ring_identity(ring: &CatalogKeyRing) -> Result<String, CatalogSignatureError> {
    let mut copy = ring.clone();
    copy.ring_identity.clear();
    let bytes = serde_json::to_vec(&copy).map_err(|_| {
        error(
            "model-catalog-key-ring-serialize",
            "catalog key ring could not be serialized",
        )
    })?;
    Ok(format!(
        "model-catalog-key-ring:sha256:{:x}",
        Sha256::digest(bytes)
    ))
}

fn payload_identity(payload: &[u8]) -> String {
    format!("model-catalog-payload:sha256:{:x}", Sha256::digest(payload))
}

fn key_ring_payload_identity(payload: &[u8]) -> String {
    format!(
        "model-catalog-key-ring-payload:sha256:{:x}",
        Sha256::digest(payload)
    )
}

fn decode_verifying_key(encoded: &str) -> Result<VerifyingKey, CatalogSignatureError> {
    let bytes = decode_base64(encoded, MAX_ENCODED_KEY_BYTES, "key")?;
    let bytes: [u8; 32] = bytes.try_into().map_err(|_| {
        error(
            "model-catalog-key-ring-key-invalid",
            "catalog signing public key is invalid",
        )
    })?;
    VerifyingKey::from_bytes(&bytes).map_err(|_| {
        error(
            "model-catalog-key-ring-key-invalid",
            "catalog signing public key is invalid",
        )
    })
}

fn decode_signature(encoded: &str) -> Result<Signature, CatalogSignatureError> {
    let bytes = decode_base64(encoded, MAX_ENCODED_SIGNATURE_BYTES, "signature")?;
    Signature::from_slice(&bytes).map_err(|_| {
        error(
            "model-catalog-signature-encoding-invalid",
            "catalog signature encoding is invalid",
        )
    })
}

fn decode_base64(
    encoded: &str,
    max_bytes: usize,
    kind: &'static str,
) -> Result<Vec<u8>, CatalogSignatureError> {
    if encoded.is_empty()
        || encoded.len() > max_bytes
        || encoded
            .chars()
            .any(|character| character.is_control() || character.is_whitespace())
    {
        return Err(if kind == "key" {
            error(
                "model-catalog-key-ring-key-invalid",
                "catalog signing public key is invalid",
            )
        } else {
            error(
                "model-catalog-signature-encoding-invalid",
                "catalog signature encoding is invalid",
            )
        });
    }
    BASE64_STANDARD.decode(encoded).map_err(|_| {
        if kind == "key" {
            error(
                "model-catalog-key-ring-key-invalid",
                "catalog signing public key is invalid",
            )
        } else {
            error(
                "model-catalog-signature-encoding-invalid",
                "catalog signature encoding is invalid",
            )
        }
    })
}

fn validate_key_id(key_id: &str) -> Result<(), CatalogSignatureError> {
    if key_id.is_empty()
        || key_id.len() > MAX_KEY_ID_BYTES
        || !key_id
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.' | b':'))
    {
        return Err(error(
            "model-catalog-signature-key-id-invalid",
            "catalog signature key ID is invalid",
        ));
    }
    Ok(())
}

fn error(code: &'static str, message: &'static str) -> CatalogSignatureError {
    CatalogSignatureError { code, message }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model_catalog::offline_for_runtime;
    use ed25519_dalek::{Signer, SigningKey};

    fn signing_key(byte: u8) -> SigningKey {
        SigningKey::from_bytes(&[byte; 32])
    }

    fn public_key(key: &SigningKey) -> String {
        BASE64_STANDARD.encode(key.verifying_key().to_bytes())
    }

    fn key_entry(key_id: &str, key: &SigningKey, replaces: Option<&str>) -> CatalogSigningKey {
        CatalogSigningKey {
            key_id: key_id.into(),
            public_key_base64: public_key(key),
            valid_from_ms: 100,
            valid_until_ms: Some(10_000),
            revoked: false,
            replaces: replaces.map(str::to_owned),
        }
    }

    fn catalog() -> ModelCatalog {
        let mut catalog = offline_for_runtime("codex", "0.144.5", Some("aegisy"), Some("agent"));
        catalog.state = CatalogState::Fresh;
        catalog.catalog_version = "catalog-1".into();
        catalog.source = "aegisy-cloud".into();
        catalog.issued_at_ms = Some(500);
        catalog.expires_at_ms = Some(2_000);
        catalog.validation_errors.clear();
        catalog
    }

    fn signed_catalog(key_id: &str, key: &SigningKey) -> SignedModelCatalog {
        let catalog = catalog();
        let payload = signing_payload_bytes(key_id, &catalog).unwrap();
        SignedModelCatalog {
            schema_version: SIGNATURE_SCHEMA_VERSION.into(),
            key_id: key_id.into(),
            catalog,
            payload_identity: signing_payload_identity(&payload),
            signature_base64: BASE64_STANDARD.encode(key.sign(&payload).to_bytes()),
        }
    }

    #[test]
    fn verifies_strict_signature_and_sets_validation_only_after_success() {
        let key = signing_key(7);
        let ring = CatalogKeyRing::new(1, vec![key_entry("catalog-key-1", &key, None)]).unwrap();
        let envelope = signed_catalog("catalog-key-1", &key);
        assert!(!envelope.catalog.signature_validated);
        let verified = envelope.verify(&ring, 1_000).unwrap();
        assert!(verified.signature_validated);
        assert_eq!(verified.state, CatalogState::Fresh);
    }

    #[test]
    fn rejects_tampered_payload_wrong_key_unknown_key_and_expired_catalog() {
        let key = signing_key(7);
        let other = signing_key(8);
        let ring = CatalogKeyRing::new(1, vec![key_entry("catalog-key-1", &key, None)]).unwrap();

        let mut tampered = signed_catalog("catalog-key-1", &key);
        tampered.catalog.catalog_version = "catalog-tampered".into();
        assert_eq!(
            tampered.verify(&ring, 1_000).unwrap_err().code,
            "model-catalog-signature-payload-identity-mismatch"
        );

        let wrong_key = signed_catalog("catalog-key-1", &other);
        assert_eq!(
            wrong_key.verify(&ring, 1_000).unwrap_err().code,
            "model-catalog-signature-invalid"
        );

        let unknown = signed_catalog("catalog-key-2", &other);
        assert_eq!(
            unknown.verify(&ring, 1_000).unwrap_err().code,
            "model-catalog-signature-key-unknown"
        );

        let expired = signed_catalog("catalog-key-1", &key);
        assert_eq!(
            expired.verify(&ring, 2_000).unwrap_err().code,
            "model-catalog-signature-time-invalid"
        );
    }

    #[test]
    fn rotation_is_monotonic_preserves_history_and_requires_lineage() {
        let first = signing_key(7);
        let second = signing_key(8);
        let previous =
            CatalogKeyRing::new(1, vec![key_entry("catalog-key-1", &first, None)]).unwrap();
        let next = CatalogKeyRing::new(
            2,
            vec![
                key_entry("catalog-key-1", &first, None),
                key_entry("catalog-key-2", &second, Some("catalog-key-1")),
            ],
        )
        .unwrap();
        assert_eq!(
            validate_key_ring_rotation(&previous, &next).unwrap(),
            KeyRingWrite::Installed { generation: 2 }
        );
        assert_eq!(
            validate_key_ring_rotation(&next, &next).unwrap(),
            KeyRingWrite::Idempotent { generation: 2 }
        );
        assert_eq!(
            validate_key_ring_rotation(&next, &previous)
                .unwrap_err()
                .code,
            "model-catalog-key-ring-rollback"
        );

        let no_lineage = CatalogKeyRing::new(
            2,
            vec![
                key_entry("catalog-key-1", &first, None),
                key_entry("catalog-key-2", &second, None),
            ],
        )
        .unwrap();
        assert_eq!(
            validate_key_ring_rotation(&previous, &no_lineage)
                .unwrap_err()
                .code,
            "model-catalog-key-ring-lineage-missing"
        );
    }

    #[test]
    fn revoked_or_rewritten_key_authority_fails_closed() {
        let first = signing_key(7);
        let second = signing_key(8);
        let previous =
            CatalogKeyRing::new(1, vec![key_entry("catalog-key-1", &first, None)]).unwrap();
        let mut revoked_key = key_entry("catalog-key-1", &first, None);
        revoked_key.revoked = true;
        let next = CatalogKeyRing::new(
            2,
            vec![
                revoked_key,
                key_entry("catalog-key-2", &second, Some("catalog-key-1")),
            ],
        )
        .unwrap();
        validate_key_ring_rotation(&previous, &next).unwrap();
        assert_eq!(
            signed_catalog("catalog-key-1", &first)
                .verify(&next, 1_000)
                .unwrap_err()
                .code,
            "model-catalog-signature-key-inactive"
        );

        let mut rewritten = previous.keys[0].clone();
        rewritten.public_key_base64 = public_key(&second);
        let rewritten = CatalogKeyRing::new(
            2,
            vec![
                rewritten,
                key_entry("catalog-key-2", &second, Some("catalog-key-1")),
            ],
        )
        .unwrap();
        assert_eq!(
            validate_key_ring_rotation(&previous, &rewritten)
                .unwrap_err()
                .code,
            "model-catalog-key-ring-key-rewritten"
        );
    }
}
