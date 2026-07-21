//! Read-only model catalog metadata.
//!
//! The first catalog boundary is deliberately local and metadata-only.  It is
//! not a signed cloud catalog and it never carries credentials.  Unknown
//! capability values stay `null` so callers cannot mistake an unverified value
//! for a supported feature.

use crate::output_redaction::redact_complete;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};

pub const SCHEMA_VERSION: &str = "model-catalog/0.1";
pub const CAPABILITY_CHECK_SCHEMA_VERSION: &str = "model-capability-check/0.1";
const MAX_MODELS: usize = 256;
const MAX_PROTOCOLS: usize = 8;
const MAX_ROLES: usize = 16;
const MAX_DEGRADATIONS: usize = 32;
const MAX_AUTHORITIES: usize = 32;
const MAX_ALIASES: usize = 16;
const MAX_TEXT_BYTES: usize = 256;

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum CatalogState {
    Fresh,
    Stale,
    Invalid,
    Offline,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum FieldAuthority {
    UpstreamAuthoritative,
    AegisyConfigured,
    EvaluationDerived,
    Estimated,
    Unknown,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum Availability {
    Available,
    Unavailable,
    Deprecated,
    Unknown,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum Entitlement {
    Allowed,
    Denied,
    Unknown,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
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

#[derive(Debug, Clone, Deserialize, PartialEq, Eq)]
pub struct CapabilityRequirements {
    pub mode: String,
    #[serde(default)]
    pub attachments: Vec<String>,
    #[serde(default)]
    pub tools: bool,
    #[serde(default)]
    pub reasoning: bool,
    #[serde(default)]
    pub context_tokens: Option<u64>,
    #[serde(default)]
    pub runtime: Option<String>,
    #[serde(default)]
    pub zero_data_retention: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct CapabilityCheckEntry {
    pub capability: String,
    pub required: Value,
    pub observed: Value,
    pub authority: FieldAuthority,
    pub result: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct CapabilityMismatch {
    pub code: String,
    pub capability: String,
    pub reason: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct CapabilityCheckResult {
    pub schema_version: String,
    pub model_id: String,
    pub catalog_state: CatalogState,
    pub decision: String,
    pub selection_allowed: bool,
    pub checks: Vec<CapabilityCheckEntry>,
    pub mismatches: Vec<CapabilityMismatch>,
}

impl ModelCatalog {
    pub fn validate(&self) -> Result<(), CatalogError> {
        if self.schema_version != SCHEMA_VERSION {
            return Err(error("unsupported model catalog schema"));
        }
        validate_text(&self.catalog_version, "catalog version")?;
        validate_text(&self.source, "catalog source")?;
        if self.contains_credentials {
            return Err(error("model catalog cannot contain credentials"));
        }
        if self.state == CatalogState::Fresh && !self.signature_validated {
            return Err(error("fresh catalog must be signature validated"));
        }
        if self.state == CatalogState::Invalid && self.signature_validated {
            return Err(error("invalid catalog cannot be signature validated"));
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
        if self.aliases.len() > MAX_ALIASES {
            return Err(error("model alias list is too large"));
        }
        if self.roles.len() > MAX_ROLES {
            return Err(error("model role list is too large"));
        }
        for protocol in &self.protocols {
            validate_text(protocol, "model protocol")?;
        }
        let mut aliases = BTreeSet::new();
        for alias in &self.aliases {
            validate_text(alias, "model alias")?;
            if alias == &self.model_id || !aliases.insert(alias.as_str()) {
                return Err(error(
                    "model aliases must be unique and differ from model ID",
                ));
            }
        }
        if self.limits.context_tokens == Some(0) || self.limits.output_tokens == Some(0) {
            return Err(error("model token limits must be positive when present"));
        }
        let mut role_names = BTreeSet::new();
        for role in &self.roles {
            validate_text(&role.role, "model role")?;
            if !role_names.insert(role.role.as_str()) {
                return Err(error("model roles must be unique"));
            }
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
        if self.field_authority.len() > MAX_AUTHORITIES {
            return Err(error("model authority map is too large"));
        }
        for field in self.field_authority.keys() {
            validate_text(field, "model authority field")?;
            if !matches!(
                field.as_str(),
                "availability"
                    | "entitlement"
                    | "lifecycle"
                    | "limits"
                    | "capabilities"
                    | "roles"
                    | "policy"
                    | "runtime_compatibility"
            ) {
                return Err(error("model authority field is unsupported"));
            }
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

impl CapabilityRequirements {
    pub fn validate_and_normalize(mut self) -> Result<Self, CatalogError> {
        if !matches!(self.mode.as_str(), "chat" | "work") {
            return Err(error("capability check mode must be chat or work"));
        }
        validate_text(&self.mode, "capability check mode")?;
        if self.attachments.len() > 8 {
            return Err(error("capability check attachment list is too large"));
        }
        for attachment in &self.attachments {
            validate_text(attachment, "capability check attachment")?;
            if !matches!(attachment.as_str(), "image" | "audio" | "video") {
                return Err(error("capability check attachment kind is unsupported"));
            }
        }
        if self.context_tokens == Some(0) {
            return Err(error(
                "capability check context requirement must be positive",
            ));
        }
        if let Some(runtime) = &self.runtime {
            validate_text(runtime, "capability check runtime")?;
        }
        if self.mode == "work" {
            self.tools = true;
        }
        Ok(self)
    }
}

pub fn check_capabilities(
    catalog: &ModelCatalog,
    model_id: &str,
    requirements: CapabilityRequirements,
) -> Result<CapabilityCheckResult, CatalogError> {
    validate_text(model_id, "capability check model ID")?;
    let requirements = requirements.validate_and_normalize()?;
    let Some(model) = catalog
        .models
        .iter()
        .find(|model| model.model_id == model_id)
    else {
        return Ok(CapabilityCheckResult {
            schema_version: CAPABILITY_CHECK_SCHEMA_VERSION.into(),
            model_id: model_id.into(),
            catalog_state: catalog.state,
            decision: "blocked".into(),
            selection_allowed: false,
            checks: Vec::new(),
            mismatches: vec![CapabilityMismatch {
                code: "model-not-in-catalog".into(),
                capability: "model".into(),
                reason: "the requested model is not present in the validated catalog".into(),
            }],
        });
    };

    let mut checks = Vec::new();
    let mut mismatches = Vec::new();
    let mut has_unknown = false;

    let availability_authority = model
        .field_authority
        .get("availability")
        .copied()
        .unwrap_or(FieldAuthority::Unknown);
    match model.availability {
        Availability::Available if authority_is_verified(availability_authority) => check(
            &mut checks,
            "availability",
            json!("available"),
            json!("available"),
            availability_authority,
            "compatible",
        ),
        Availability::Available => unknown_check(
            &mut checks,
            &mut has_unknown,
            "availability",
            json!("known"),
            json!("available"),
            availability_authority,
        ),
        Availability::Unknown => unknown_check(
            &mut checks,
            &mut has_unknown,
            "availability",
            json!("known"),
            json!("unknown"),
            FieldAuthority::Unknown,
        ),
        Availability::Unavailable | Availability::Deprecated
            if authority_is_verified(availability_authority) =>
        {
            blocked_check(
                &mut checks,
                &mut mismatches,
                "availability",
                (
                    json!("available"),
                    json!(match model.availability {
                        Availability::Unavailable => "unavailable",
                        Availability::Deprecated => "deprecated",
                        _ => unreachable!("availability branch is exhaustive"),
                    }),
                    availability_authority,
                ),
                "model-unavailable",
                "the catalog marks this model unavailable or deprecated",
            )
        }
        Availability::Unavailable | Availability::Deprecated => unknown_check(
            &mut checks,
            &mut has_unknown,
            "availability",
            json!("available"),
            json!(match model.availability {
                Availability::Unavailable => "unavailable",
                Availability::Deprecated => "deprecated",
                _ => unreachable!("availability branch is exhaustive"),
            }),
            availability_authority,
        ),
    }

    let entitlement_authority = model
        .field_authority
        .get("entitlement")
        .copied()
        .unwrap_or(FieldAuthority::Unknown);
    match model.entitlement {
        Entitlement::Allowed if authority_is_verified(entitlement_authority) => check(
            &mut checks,
            "entitlement",
            json!("allowed"),
            json!("allowed"),
            entitlement_authority,
            "compatible",
        ),
        Entitlement::Allowed => unknown_check(
            &mut checks,
            &mut has_unknown,
            "entitlement",
            json!("allowed"),
            json!("allowed"),
            entitlement_authority,
        ),
        Entitlement::Unknown => unknown_check(
            &mut checks,
            &mut has_unknown,
            "entitlement",
            json!("allowed"),
            json!("unknown"),
            FieldAuthority::Unknown,
        ),
        Entitlement::Denied if authority_is_verified(entitlement_authority) => blocked_check(
            &mut checks,
            &mut mismatches,
            "entitlement",
            (json!("allowed"), json!("denied"), entitlement_authority),
            "entitlement-denied",
            "the catalog does not grant access to this model",
        ),
        Entitlement::Denied => unknown_check(
            &mut checks,
            &mut has_unknown,
            "entitlement",
            json!("allowed"),
            json!("denied"),
            entitlement_authority,
        ),
    }

    if let Some(runtime) = &requirements.runtime {
        if model.runtime_compatibility.adapter != *runtime
            && model.runtime_compatibility.state == "verified"
        {
            blocked_check(
                &mut checks,
                &mut mismatches,
                "runtime",
                (
                    json!(runtime),
                    json!(model.runtime_compatibility.adapter),
                    FieldAuthority::AegisyConfigured,
                ),
                "runtime-mismatch",
                "the model is not declared compatible with the requested runtime",
            );
        } else if model.runtime_compatibility.state == "verified" {
            check(
                &mut checks,
                "runtime",
                json!(runtime),
                json!(model.runtime_compatibility.adapter),
                FieldAuthority::AegisyConfigured,
                "compatible",
            );
        } else {
            unknown_check(
                &mut checks,
                &mut has_unknown,
                "runtime",
                json!(runtime),
                json!(model.runtime_compatibility.adapter),
                FieldAuthority::Unknown,
            );
        }
    }

    if requirements.tools {
        check_bool(
            &mut checks,
            &mut mismatches,
            &mut has_unknown,
            "tool-calls",
            model.capabilities.tool_calls,
            "tool-calls-unsupported",
            model.capabilities.authority,
        );
    }
    if requirements.reasoning {
        check_bool(
            &mut checks,
            &mut mismatches,
            &mut has_unknown,
            "reasoning",
            model.capabilities.reasoning,
            "reasoning-unsupported",
            model.capabilities.authority,
        );
    }
    for attachment in &requirements.attachments {
        let (capability, observed) = match attachment.as_str() {
            "image" => ("image-input", model.capabilities.image_input),
            "audio" => ("audio-input", model.capabilities.audio_input),
            "video" => ("video-input", model.capabilities.video_input),
            _ => unreachable!("attachment kinds are validated above"),
        };
        check_bool(
            &mut checks,
            &mut mismatches,
            &mut has_unknown,
            capability,
            observed,
            &format!("{capability}-unsupported"),
            model.capabilities.authority,
        );
    }
    if let Some(required_context) = requirements.context_tokens {
        match model.limits.context_tokens {
            Some(observed)
                if observed >= required_context
                    && authority_is_verified(model.limits.authority) =>
            {
                check(
                    &mut checks,
                    "context-tokens",
                    json!(required_context),
                    json!(observed),
                    model.limits.authority,
                    "compatible",
                )
            }
            Some(observed) if observed >= required_context => unknown_check(
                &mut checks,
                &mut has_unknown,
                "context-tokens",
                json!(required_context),
                json!(observed),
                model.limits.authority,
            ),
            Some(observed) if authority_is_verified(model.limits.authority) => blocked_check(
                &mut checks,
                &mut mismatches,
                "context-tokens",
                (
                    json!(required_context),
                    json!(observed),
                    model.limits.authority,
                ),
                "context-too-small",
                "the declared context limit is smaller than the requirement",
            ),
            Some(observed) => unknown_check(
                &mut checks,
                &mut has_unknown,
                "context-tokens",
                json!(required_context),
                json!(observed),
                model.limits.authority,
            ),
            None => unknown_check(
                &mut checks,
                &mut has_unknown,
                "context-tokens",
                json!(required_context),
                Value::Null,
                model.limits.authority,
            ),
        }
    }
    if requirements.zero_data_retention {
        match model.policy.zero_data_retention {
            Some(true) if authority_is_verified(model.policy.authority) => check(
                &mut checks,
                "zero-data-retention",
                json!(true),
                json!(true),
                model.policy.authority,
                "compatible",
            ),
            Some(true) => unknown_check(
                &mut checks,
                &mut has_unknown,
                "zero-data-retention",
                json!(true),
                json!(true),
                model.policy.authority,
            ),
            Some(false) if authority_is_verified(model.policy.authority) => blocked_check(
                &mut checks,
                &mut mismatches,
                "zero-data-retention",
                (json!(true), json!(false), model.policy.authority),
                "retention-policy-mismatch",
                "the model policy does not satisfy zero-data-retention",
            ),
            Some(false) => unknown_check(
                &mut checks,
                &mut has_unknown,
                "zero-data-retention",
                json!(true),
                json!(false),
                model.policy.authority,
            ),
            None => unknown_check(
                &mut checks,
                &mut has_unknown,
                "zero-data-retention",
                json!(true),
                Value::Null,
                model.policy.authority,
            ),
        }
    }

    let decision = if !mismatches.is_empty() {
        "blocked"
    } else if has_unknown || catalog.state != CatalogState::Fresh || !catalog.signature_validated {
        "unknown"
    } else {
        "compatible"
    };
    Ok(CapabilityCheckResult {
        schema_version: CAPABILITY_CHECK_SCHEMA_VERSION.into(),
        model_id: model_id.into(),
        catalog_state: catalog.state,
        selection_allowed: decision == "compatible",
        decision: decision.into(),
        checks,
        mismatches,
    })
}

fn check(
    checks: &mut Vec<CapabilityCheckEntry>,
    capability: &str,
    required: Value,
    observed: Value,
    authority: FieldAuthority,
    result: &str,
) {
    checks.push(CapabilityCheckEntry {
        capability: capability.into(),
        required,
        observed,
        authority,
        result: result.into(),
    });
}

fn unknown_check(
    checks: &mut Vec<CapabilityCheckEntry>,
    has_unknown: &mut bool,
    capability: &str,
    required: Value,
    observed: Value,
    authority: FieldAuthority,
) {
    *has_unknown = true;
    check(checks, capability, required, observed, authority, "unknown");
}

fn blocked_check(
    checks: &mut Vec<CapabilityCheckEntry>,
    mismatches: &mut Vec<CapabilityMismatch>,
    capability: &str,
    (required, observed, authority): (Value, Value, FieldAuthority),
    code: &str,
    reason: &str,
) {
    check(checks, capability, required, observed, authority, "blocked");
    mismatches.push(CapabilityMismatch {
        code: code.into(),
        capability: capability.into(),
        reason: reason.into(),
    });
}

fn check_bool(
    checks: &mut Vec<CapabilityCheckEntry>,
    mismatches: &mut Vec<CapabilityMismatch>,
    has_unknown: &mut bool,
    capability: &str,
    observed: Option<bool>,
    mismatch_code: &str,
    authority: FieldAuthority,
) {
    match observed {
        Some(true) if authority_is_verified(authority) => check(
            checks,
            capability,
            json!(true),
            json!(true),
            authority,
            "compatible",
        ),
        Some(true) => unknown_check(
            checks,
            has_unknown,
            capability,
            json!(true),
            json!(true),
            authority,
        ),
        Some(false) if authority_is_verified(authority) => blocked_check(
            checks,
            mismatches,
            capability,
            (json!(true), json!(false), authority),
            mismatch_code,
            "the catalog explicitly reports that the required capability is unavailable",
        ),
        Some(false) => unknown_check(
            checks,
            has_unknown,
            capability,
            json!(true),
            json!(false),
            authority,
        ),
        None => unknown_check(
            checks,
            has_unknown,
            capability,
            json!(true),
            Value::Null,
            authority,
        ),
    }
}

fn authority_is_verified(authority: FieldAuthority) -> bool {
    matches!(
        authority,
        FieldAuthority::UpstreamAuthoritative
            | FieldAuthority::AegisyConfigured
            | FieldAuthority::EvaluationDerived
    )
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
    if redact_complete(value) != value {
        return Err(error(format!("{field} is secret-shaped")));
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
    fn catalog_policy_rejects_alias_role_authority_and_signature_conflicts() {
        let mut catalog = offline_for_runtime("preview", "1", Some("local"), Some("echo"));
        catalog.models[0].aliases = vec!["local:echo".into()];
        assert!(catalog.validate().is_err());

        catalog.models[0].aliases.clear();
        let duplicate_role = catalog.models[0].roles[0].clone();
        catalog.models[0].roles.push(duplicate_role);
        assert!(catalog.validate().is_err());

        catalog.models[0].roles.pop();
        catalog.models[0]
            .field_authority
            .insert("untrusted-field".into(), FieldAuthority::Unknown);
        assert!(catalog.validate().is_err());

        catalog.models[0].field_authority.remove("untrusted-field");
        catalog.state = CatalogState::Fresh;
        assert!(catalog.validate().is_err());
        catalog.signature_validated = true;
        catalog.state = CatalogState::Invalid;
        assert!(catalog.validate().is_err());
    }

    #[test]
    fn catalog_policy_rejects_secret_shaped_metadata() {
        let mut catalog = offline_for_runtime("preview", "1", Some("local"), Some("echo"));
        catalog.catalog_version = "token=abc".into();
        assert!(catalog.validate().is_err());
    }

    #[test]
    fn catalog_serialization_preserves_null_unknowns() {
        let catalog = offline_for_runtime("preview", "1", Some("local"), Some("echo"));
        let value = serde_json::to_value(catalog).unwrap();
        assert!(value["models"][0]["capabilities"]["tool_calls"].is_null());
        assert_eq!(value["state"], "offline");
    }

    #[test]
    fn work_requirements_make_unknown_tools_and_attachments_non_selectable() {
        let catalog = offline_for_runtime("preview", "1", Some("local"), Some("echo"));
        let result = check_capabilities(
            &catalog,
            "local:echo",
            CapabilityRequirements {
                mode: "work".into(),
                attachments: vec!["image".into()],
                tools: false,
                reasoning: false,
                context_tokens: Some(1_000),
                runtime: Some("preview".into()),
                zero_data_retention: false,
            },
        )
        .unwrap();
        assert_eq!(result.decision, "unknown");
        assert!(!result.selection_allowed);
        assert!(result
            .checks
            .iter()
            .any(|check| check.capability == "tool-calls" && check.result == "unknown"));
        assert!(result
            .checks
            .iter()
            .any(|check| check.capability == "image-input" && check.result == "unknown"));
    }

    #[test]
    fn explicit_unavailable_model_blocks_before_unknown_fallback() {
        let mut catalog = offline_for_runtime("preview", "1", Some("local"), Some("echo"));
        catalog.models[0].availability = Availability::Unavailable;
        catalog.models[0]
            .field_authority
            .insert("availability".into(), FieldAuthority::AegisyConfigured);
        let result = check_capabilities(
            &catalog,
            "local:echo",
            CapabilityRequirements {
                mode: "chat".into(),
                attachments: Vec::new(),
                tools: false,
                reasoning: false,
                context_tokens: None,
                runtime: None,
                zero_data_retention: false,
            },
        )
        .unwrap();
        assert_eq!(result.decision, "blocked");
        assert_eq!(result.mismatches[0].code, "model-unavailable");
        assert!(!result.selection_allowed);
    }

    #[test]
    fn fresh_signed_catalog_allows_only_complete_compatible_requirements() {
        let mut catalog = offline_for_runtime("preview", "1", Some("local"), Some("echo"));
        catalog.state = CatalogState::Fresh;
        catalog.signature_validated = true;
        catalog.models[0].availability = Availability::Available;
        catalog.models[0].entitlement = Entitlement::Allowed;
        catalog.models[0]
            .field_authority
            .insert("availability".into(), FieldAuthority::AegisyConfigured);
        catalog.models[0]
            .field_authority
            .insert("entitlement".into(), FieldAuthority::AegisyConfigured);
        catalog.models[0].runtime_compatibility.state = "verified".into();
        catalog.models[0].limits.context_tokens = Some(8_192);
        catalog.models[0].limits.authority = FieldAuthority::UpstreamAuthoritative;
        catalog.models[0].capabilities.tool_calls = Some(true);
        catalog.models[0].capabilities.reasoning = Some(true);
        catalog.models[0].capabilities.image_input = Some(true);
        catalog.models[0].capabilities.authority = FieldAuthority::UpstreamAuthoritative;
        catalog.models[0].policy.zero_data_retention = Some(true);
        catalog.models[0].policy.authority = FieldAuthority::AegisyConfigured;
        let result = check_capabilities(
            &catalog,
            "local:echo",
            CapabilityRequirements {
                mode: "work".into(),
                attachments: vec!["image".into()],
                tools: false,
                reasoning: true,
                context_tokens: Some(4_096),
                runtime: Some("preview".into()),
                zero_data_retention: true,
            },
        )
        .unwrap();
        assert_eq!(result.decision, "compatible");
        assert!(result.selection_allowed);
        assert!(result.mismatches.is_empty());
        assert!(result
            .checks
            .iter()
            .all(|check| check.result == "compatible"));
    }

    #[test]
    fn present_values_with_unknown_authority_do_not_become_compatible() {
        let mut catalog = offline_for_runtime("preview", "1", Some("local"), Some("echo"));
        catalog.state = CatalogState::Fresh;
        catalog.signature_validated = true;
        catalog.models[0].availability = Availability::Available;
        catalog.models[0].entitlement = Entitlement::Allowed;
        catalog.models[0].capabilities.tool_calls = Some(true);
        let result = check_capabilities(
            &catalog,
            "local:echo",
            CapabilityRequirements {
                mode: "work".into(),
                attachments: Vec::new(),
                tools: false,
                reasoning: false,
                context_tokens: None,
                runtime: None,
                zero_data_retention: false,
            },
        )
        .unwrap();
        assert_eq!(result.decision, "unknown");
        assert!(!result.selection_allowed);
        assert!(result
            .checks
            .iter()
            .any(|check| check.capability == "tool-calls" && check.result == "unknown"));
    }

    #[test]
    fn negative_values_with_unknown_authority_do_not_become_blocked() {
        let mut catalog = offline_for_runtime("preview", "1", Some("local"), Some("echo"));
        catalog.state = CatalogState::Fresh;
        catalog.signature_validated = true;
        catalog.models[0].availability = Availability::Unavailable;
        catalog.models[0].entitlement = Entitlement::Denied;
        catalog.models[0].limits.context_tokens = Some(128);
        catalog.models[0].capabilities.tool_calls = Some(false);
        catalog.models[0].policy.zero_data_retention = Some(false);
        let result = check_capabilities(
            &catalog,
            "local:echo",
            CapabilityRequirements {
                mode: "work".into(),
                attachments: Vec::new(),
                tools: false,
                reasoning: false,
                context_tokens: Some(1_024),
                runtime: None,
                zero_data_retention: true,
            },
        )
        .unwrap();
        assert_eq!(result.decision, "unknown");
        assert!(result.mismatches.is_empty());
        assert!(result.checks.iter().all(|check| check.result == "unknown"));
    }
}
