//! Read-only model catalog metadata.
//!
//! The first catalog boundary is deliberately local and metadata-only.  It is
//! not a signed cloud catalog and it never carries credentials.  Unknown
//! capability values stay `null` so callers cannot mistake an unverified value
//! for a supported feature.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

pub const SCHEMA_VERSION: &str = "model-catalog/0.1";
const MAX_MODELS: usize = 256;
const MAX_PROTOCOLS: usize = 8;
const MAX_ROLES: usize = 16;
const MAX_DEGRADATIONS: usize = 32;
const MAX_TEXT_BYTES: usize = 256;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum CatalogState {
    Fresh,
    Stale,
    Invalid,
    Offline,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum FieldAuthority {
    UpstreamAuthoritative,
    AegisyConfigured,
    EvaluationDerived,
    Estimated,
    Unknown,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum Availability {
    Available,
    Unavailable,
    Deprecated,
    Unknown,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum Entitlement {
    Allowed,
    Denied,
    Unknown,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum Lifecycle {
    Active,
    Preview,
    Retiring,
    Retired,
    Unknown,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ModelLimits {
    pub context_tokens: Option<u64>,
    pub output_tokens: Option<u64>,
    pub authority: FieldAuthority,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ModelCapabilities {
    pub reasoning: Option<bool>,
    pub tool_calls: Option<bool>,
    pub parallel_tool_calls: Option<bool>,
    pub structured_output: Option<bool>,
    pub prompt_caching: Option<bool>,
    pub image_input: Option<bool>,
    pub audio_input: Option<bool>,
    pub video_input: Option<bool>,
    pub realtime: Option<bool>,
    pub authority: FieldAuthority,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct RoleSuitability {
    pub role: String,
    pub supported: Option<bool>,
    pub authority: FieldAuthority,
    pub evaluation_version: Option<String>,
    pub sample_size: Option<u32>,
    pub known_limitations: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct RuntimeCompatibility {
    pub adapter: String,
    pub adapter_version: String,
    pub state: String,
    pub known_degradations: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ModelPolicy {
    pub zero_data_retention: Option<bool>,
    pub region: Option<String>,
    pub authority: FieldAuthority,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ModelDescriptor {
    pub model_id: String,
    pub provider: String,
    pub upstream_model_id: Option<String>,
    pub protocols: Vec<String>,
    pub aliases: Vec<String>,
    pub availability: Availability,
    pub entitlement: Entitlement,
    pub lifecycle: Lifecycle,
    pub limits: ModelLimits,
    pub capabilities: ModelCapabilities,
    pub roles: Vec<RoleSuitability>,
    pub policy: ModelPolicy,
    pub runtime_compatibility: RuntimeCompatibility,
    pub field_authority: BTreeMap<String, FieldAuthority>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ModelCatalog {
    pub schema_version: String,
    pub catalog_version: String,
    pub state: CatalogState,
    pub source: String,
    pub signature_validated: bool,
    pub refresh_supported: bool,
    pub issued_at_ms: Option<u64>,
    pub expires_at_ms: Option<u64>,
    pub models: Vec<ModelDescriptor>,
    pub validation_errors: Vec<String>,
    pub contains_credentials: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CatalogError {
    pub message: String,
}

impl ModelCatalog {
    pub fn validate(&self) -> Result<(), CatalogError> {
        if self.schema_version != SCHEMA_VERSION {
            return Err(error("unsupported model catalog schema"));
        }
        if self.catalog_version.is_empty() || self.catalog_version.len() > MAX_TEXT_BYTES {
            return Err(error("catalog version is invalid"));
        }
        if self.source.is_empty() || self.source.len() > MAX_TEXT_BYTES {
            return Err(error("catalog source is invalid"));
        }
        if self.contains_credentials {
            return Err(error("model catalog cannot contain credentials"));
        }
        if self.models.len() > MAX_MODELS {
            return Err(error("model catalog contains too many models"));
        }
        if let (Some(issued), Some(expires)) = (self.issued_at_ms, self.expires_at_ms) {
            if expires < issued {
                return Err(error("catalog expiry precedes issue time"));
            }
        }
        if self.validation_errors.len() > MAX_DEGRADATIONS {
            return Err(error("catalog validation error list is too large"));
        }
        for model in &self.models {
            model.validate()?;
        }
        Ok(())
    }
}

impl ModelDescriptor {
    pub fn validate(&self) -> Result<(), CatalogError> {
        validate_text(&self.model_id, "model ID")?;
        validate_text(&self.provider, "provider")?;
        if let Some(upstream) = &self.upstream_model_id {
            validate_text(upstream, "upstream model ID")?;
        }
        if self.protocols.is_empty() || self.protocols.len() > MAX_PROTOCOLS {
            return Err(error("model protocol list is invalid"));
        }
        if self.aliases.len() > MAX_PROTOCOLS {
            return Err(error("model alias list is too large"));
        }
        if self.roles.len() > MAX_ROLES {
            return Err(error("model role list is too large"));
        }
        for protocol in &self.protocols {
            validate_text(protocol, "model protocol")?;
        }
        for alias in &self.aliases {
            validate_text(alias, "model alias")?;
        }
        if self.limits.context_tokens == Some(0) || self.limits.output_tokens == Some(0) {
            return Err(error("model token limits must be positive when present"));
        }
        for role in &self.roles {
            validate_text(&role.role, "model role")?;
            if role.known_limitations.len() > MAX_DEGRADATIONS {
                return Err(error("model role limitation list is too large"));
            }
            for limitation in &role.known_limitations {
                validate_text(limitation, "model role limitation")?;
            }
            if let Some(evaluation) = &role.evaluation_version {
                validate_text(evaluation, "evaluation version")?;
            }
        }
        if self.runtime_compatibility.known_degradations.len() > MAX_DEGRADATIONS {
            return Err(error("runtime degradation list is too large"));
        }
        validate_text(&self.runtime_compatibility.adapter, "runtime adapter")?;
        validate_text(
            &self.runtime_compatibility.adapter_version,
            "runtime adapter version",
        )?;
        for degradation in &self.runtime_compatibility.known_degradations {
            validate_text(degradation, "runtime degradation")?;
        }
        if let Some(region) = &self.policy.region {
            validate_text(region, "model region")?;
        }
        Ok(())
    }
}

/// Produce an explicit offline projection from the active runtime binding.
/// The projection is useful to clients while the signed cloud catalog is not
/// available, but every model capability remains unknown and therefore cannot
/// authorize a feature.
pub fn offline_for_runtime(
    adapter: &str,
    adapter_version: &str,
    provider: Option<&str>,
    model: Option<&str>,
) -> ModelCatalog {
    let mut models = Vec::new();
    if let (Some(provider), Some(model)) = (
        provider.and_then(safe_metadata_text),
        model.and_then(safe_metadata_text),
    ) {
        let model_id = format!("{provider}:{model}");
        let mut field_authority = BTreeMap::new();
        for field in [
            "availability",
            "entitlement",
            "lifecycle",
            "limits",
            "capabilities",
            "roles",
            "policy",
        ] {
            field_authority.insert(field.to_owned(), FieldAuthority::Unknown);
        }
        models.push(ModelDescriptor {
            model_id,
            provider: provider.to_owned(),
            upstream_model_id: Some(model.to_owned()),
            protocols: vec!["unknown".into()],
            aliases: Vec::new(),
            availability: Availability::Unknown,
            entitlement: Entitlement::Unknown,
            lifecycle: Lifecycle::Unknown,
            limits: ModelLimits {
                context_tokens: None,
                output_tokens: None,
                authority: FieldAuthority::Unknown,
            },
            capabilities: ModelCapabilities {
                reasoning: None,
                tool_calls: None,
                parallel_tool_calls: None,
                structured_output: None,
                prompt_caching: None,
                image_input: None,
                audio_input: None,
                video_input: None,
                realtime: None,
                authority: FieldAuthority::Unknown,
            },
            roles: vec![
                "agent",
                "plan",
                "apply",
                "review",
                "fast",
                "embedding",
                "rerank",
            ]
            .into_iter()
            .map(|role| RoleSuitability {
                role: role.into(),
                supported: None,
                authority: FieldAuthority::Unknown,
                evaluation_version: None,
                sample_size: None,
                known_limitations: vec!["catalog-unavailable".into()],
            })
            .collect(),
            policy: ModelPolicy {
                zero_data_retention: None,
                region: None,
                authority: FieldAuthority::Unknown,
            },
            runtime_compatibility: RuntimeCompatibility {
                adapter: safe_metadata_text(adapter).unwrap_or_else(|| "unknown".into()),
                adapter_version: safe_metadata_text(adapter_version)
                    .unwrap_or_else(|| "unknown".into()),
                state: "metadata-only".into(),
                known_degradations: vec![
                    "catalog-not-authenticated".into(),
                    "model-capabilities-unknown".into(),
                ],
            },
            field_authority,
        });
    }
    ModelCatalog {
        schema_version: SCHEMA_VERSION.into(),
        catalog_version: "offline-runtime-binding".into(),
        state: CatalogState::Offline,
        source: "runtime-binding".into(),
        signature_validated: false,
        refresh_supported: false,
        issued_at_ms: None,
        expires_at_ms: None,
        models,
        validation_errors: vec!["signed-cloud-catalog-unavailable".into()],
        contains_credentials: false,
    }
}

fn safe_metadata_text(value: &str) -> Option<String> {
    if value.is_empty()
        || value.len() > MAX_TEXT_BYTES
        || value.chars().any(char::is_control)
        || value.starts_with("sk-")
        || value.starts_with("ghp_")
        || value.starts_with("github_pat_")
        || value.starts_with("eyJ")
    {
        return None;
    }
    Some(value.to_owned())
}

fn validate_text(value: &str, field: &str) -> Result<(), CatalogError> {
    if value.is_empty() || value.len() > MAX_TEXT_BYTES || value.chars().any(char::is_control) {
        return Err(error(format!("{field} is invalid")));
    }
    Ok(())
}

fn error(message: impl Into<String>) -> CatalogError {
    CatalogError {
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn offline_projection_keeps_unknown_values_explicit() {
        let catalog = offline_for_runtime(
            "codex-app-server",
            "0.144.5",
            Some("aegisy"),
            Some("model-1"),
        );
        assert_eq!(catalog.schema_version, SCHEMA_VERSION);
        assert_eq!(catalog.state, CatalogState::Offline);
        assert!(!catalog.signature_validated);
        assert!(!catalog.contains_credentials);
        let model = &catalog.models[0];
        assert_eq!(model.availability, Availability::Unknown);
        assert_eq!(model.capabilities.tool_calls, None);
        assert_eq!(model.limits.context_tokens, None);
        assert_eq!(
            model.field_authority["capabilities"],
            FieldAuthority::Unknown
        );
        assert!(catalog.validate().is_ok());
    }

    #[test]
    fn invalid_catalog_rejects_secret_and_bad_limits() {
        let mut catalog = offline_for_runtime("preview", "1", Some("local"), Some("echo"));
        catalog.contains_credentials = true;
        assert!(catalog.validate().is_err());
        catalog.contains_credentials = false;
        catalog.models[0].limits.context_tokens = Some(0);
        assert!(catalog.validate().is_err());
    }

    #[test]
    fn catalog_serialization_preserves_null_unknowns() {
        let catalog = offline_for_runtime("preview", "1", Some("local"), Some("echo"));
        let value = serde_json::to_value(catalog).unwrap();
        assert!(value["models"][0]["capabilities"]["tool_calls"].is_null());
        assert_eq!(value["state"], "offline");
    }
}
