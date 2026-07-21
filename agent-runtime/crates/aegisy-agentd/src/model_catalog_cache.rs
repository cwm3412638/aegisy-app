//! Metadata-only model catalog cache lifecycle.
//!
//! This contract is intentionally below the cloud/authentication boundary. It
//! accepts only catalog metadata marked as signature-validated, then protects
//! local cache state from malformed expiry, clock rollback,
//! duplicate-generation conflicts, and older revisions. This boundary does not
//! authenticate that mark, so it fixes selection authority to false and does
//! not fetch, sign, refresh, select, route, issue tokens, or authorize a turn.

use crate::model_catalog::{CatalogState, ModelCatalog};
use serde::{Deserialize, Serialize};
use serde_json::to_vec;
use sha2::{Digest, Sha256};

pub const SCHEMA_VERSION: &str = "model-catalog-cache/0.1";
const MAX_STALE_MS: u64 = 30 * 24 * 60 * 60 * 1_000;
const MAX_TTL_MS: u64 = 90 * 24 * 60 * 60 * 1_000;

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

impl ModelCatalogCache {
    pub fn new(max_stale_ms: u64) -> Result<Self, CatalogCacheError> {
        validate_stale_window(max_stale_ms)?;
        Ok(Self {
            schema_version: SCHEMA_VERSION.into(),
            max_stale_ms,
            current: None,
        })
    }

    pub fn install(
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

fn error(code: &'static str, message: &'static str) -> CatalogCacheError {
    CatalogCacheError { code, message }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model_catalog::offline_for_runtime;

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
}
