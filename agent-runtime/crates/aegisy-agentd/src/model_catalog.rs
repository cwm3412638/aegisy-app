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
pub const RUNTIME_COMPATIBILITY_SCHEMA_VERSION: &str = "model-runtime-compatibility/0.1";
const MAX_MODELS: usize = 256;
const MAX_PROTOCOLS: usize = 8;
const MAX_ROLES: usize = 16;
const MAX_DEGRADATIONS: usize = 32;
const MAX_RUNTIME_COMPATIBILITIES: usize = 16;
const MAX_RUNTIME_VERSIONS: usize = 16;
const MAX_DEGRADATION_FEATURES: usize = 16;
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

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(rename_all = "kebab-case")]
pub enum RuntimeAdapterFamily {
    CodexAppServer,
    Acp,
    Native,
    Unknown,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum RuntimeCompatibilityState {
    Compatible,
    Degraded,
    Incompatible,
    Unknown,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum RuntimeDegradationSeverity {
    Warning,
    Blocking,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct RuntimeDegradation {
    pub code: String,
    pub severity: RuntimeDegradationSeverity,
    pub affected_features: Vec<String>,
    pub summary: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct RuntimeCompatibility {
    pub adapter: String,
    pub adapter_version: String,
    pub state: String,
    pub known_degradations: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct RuntimeCompatibilityEntry {
    pub schema_version: String,
    pub adapter_family: RuntimeAdapterFamily,
    pub adapter: String,
    pub protocol: String,
    pub exact_versions: Vec<String>,
    pub state: RuntimeCompatibilityState,
    pub authority: FieldAuthority,
    pub evidence_version: Option<String>,
    pub known_degradations: Vec<RuntimeDegradation>,
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
    #[serde(default)]
    pub runtime_compatibility_matrix: Vec<RuntimeCompatibilityEntry>,
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
    pub runtime_version: Option<String>,
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
        validate_text(&self.runtime_compatibility.adapter, "runtime adapter")?;
        validate_text(
            &self.runtime_compatibility.adapter_version,
            "runtime adapter version",
        )?;
        validate_text(
            &self.runtime_compatibility.state,
            "runtime compatibility state",
        )?;
        if self.runtime_compatibility.known_degradations.len() > MAX_DEGRADATIONS {
            return Err(error("runtime degradation list is too large"));
        }
        for degradation in &self.runtime_compatibility.known_degradations {
            validate_text(degradation, "runtime degradation")?;
        }
        if self.runtime_compatibility_matrix.len() > MAX_RUNTIME_COMPATIBILITIES {
            return Err(error("runtime compatibility matrix is too large"));
        }
        let mut runtime_adapters = BTreeSet::new();
        for compatibility in &self.runtime_compatibility_matrix {
            compatibility.validate()?;
            if !runtime_adapters.insert(compatibility.adapter.as_str()) {
                return Err(error("runtime compatibility adapters must be unique"));
            }
        }
        let runtime_matrix_authority = self
            .field_authority
            .get("runtime_compatibility")
            .copied()
            .unwrap_or(FieldAuthority::Unknown);
        if !authority_is_verified(runtime_matrix_authority)
            && self
                .runtime_compatibility_matrix
                .iter()
                .any(|compatibility| authority_is_verified(compatibility.authority))
        {
            return Err(error(
                "runtime compatibility entries exceed matrix authority",
            ));
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

impl RuntimeCompatibilityEntry {
    pub fn validate(&self) -> Result<(), CatalogError> {
        if self.schema_version != RUNTIME_COMPATIBILITY_SCHEMA_VERSION {
            return Err(error("unsupported runtime compatibility schema"));
        }
        validate_identifier(&self.adapter, "runtime adapter")?;
        validate_identifier(&self.protocol, "runtime protocol")?;
        match self.adapter_family {
            RuntimeAdapterFamily::CodexAppServer if self.protocol != "codex-app-server" => {
                return Err(error(
                    "Codex App Server compatibility requires its canonical protocol",
                ));
            }
            RuntimeAdapterFamily::Acp if self.protocol != "acp" => {
                return Err(error("ACP compatibility requires its canonical protocol"));
            }
            RuntimeAdapterFamily::Native if self.protocol != "aap-native" => {
                return Err(error(
                    "native compatibility requires the AAP native protocol",
                ));
            }
            RuntimeAdapterFamily::Unknown
                if self.protocol != "unknown"
                    || self.state != RuntimeCompatibilityState::Unknown
                    || self.authority != FieldAuthority::Unknown =>
            {
                return Err(error(
                    "unknown runtime family cannot claim protocol or compatibility authority",
                ));
            }
            _ => {}
        }
        if self.exact_versions.len() > MAX_RUNTIME_VERSIONS {
            return Err(error("runtime version list is too large"));
        }
        let mut versions = BTreeSet::new();
        for version in &self.exact_versions {
            validate_runtime_version(version, "runtime adapter version")?;
            if !versions.insert(version.as_str()) {
                return Err(error("runtime adapter versions must be unique"));
            }
        }
        if let Some(evidence_version) = &self.evidence_version {
            validate_text(evidence_version, "runtime compatibility evidence version")?;
        }
        if self.known_degradations.len() > MAX_DEGRADATIONS {
            return Err(error("runtime degradation list is too large"));
        }
        let mut degradation_codes = BTreeSet::new();
        for degradation in &self.known_degradations {
            degradation.validate()?;
            if !degradation_codes.insert(degradation.code.as_str()) {
                return Err(error("runtime degradation codes must be unique"));
            }
        }
        let verified = authority_is_verified(self.authority);
        match self.state {
            RuntimeCompatibilityState::Compatible => {
                if !verified
                    || self.exact_versions.is_empty()
                    || self.evidence_version.is_none()
                    || !self.known_degradations.is_empty()
                {
                    return Err(error("compatible runtime metadata lacks verified evidence"));
                }
            }
            RuntimeCompatibilityState::Degraded => {
                if !verified
                    || self.exact_versions.is_empty()
                    || self.evidence_version.is_none()
                    || self.known_degradations.is_empty()
                    || self.known_degradations.iter().any(|degradation| {
                        degradation.severity == RuntimeDegradationSeverity::Blocking
                    })
                {
                    return Err(error("degraded runtime metadata is inconsistent"));
                }
            }
            RuntimeCompatibilityState::Incompatible => {
                if !verified
                    || self.exact_versions.is_empty()
                    || self.evidence_version.is_none()
                    || !self.known_degradations.iter().any(|degradation| {
                        degradation.severity == RuntimeDegradationSeverity::Blocking
                    })
                {
                    return Err(error("incompatible runtime metadata is inconsistent"));
                }
            }
            RuntimeCompatibilityState::Unknown => {
                if verified || self.evidence_version.is_some() {
                    return Err(error(
                        "unknown runtime metadata cannot claim verified evidence",
                    ));
                }
            }
        }
        Ok(())
    }
}

impl RuntimeDegradation {
    fn validate(&self) -> Result<(), CatalogError> {
        validate_identifier(&self.code, "runtime degradation code")?;
        validate_text(&self.summary, "runtime degradation summary")?;
        if self.affected_features.is_empty()
            || self.affected_features.len() > MAX_DEGRADATION_FEATURES
        {
            return Err(error("runtime degradation feature list is invalid"));
        }
        let mut features = BTreeSet::new();
        for feature in &self.affected_features {
            validate_identifier(feature, "runtime degradation feature")?;
            if !features.insert(feature.as_str()) {
                return Err(error("runtime degradation features must be unique"));
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
            "runtime_compatibility",
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
            runtime_compatibility_matrix: vec![offline_runtime_compatibility(
                adapter,
                adapter_version,
            )],
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

fn offline_runtime_compatibility(
    adapter: &str,
    adapter_version: &str,
) -> RuntimeCompatibilityEntry {
    let adapter = safe_identifier(adapter).unwrap_or_else(|| "unknown".into());
    let (adapter_family, protocol) = match adapter.as_str() {
        "codex-app-server" => (RuntimeAdapterFamily::CodexAppServer, "codex-app-server"),
        "acp" => (RuntimeAdapterFamily::Acp, "acp"),
        "preview" | "aegisy-native" => (RuntimeAdapterFamily::Native, "aap-native"),
        _ => (RuntimeAdapterFamily::Unknown, "unknown"),
    };
    RuntimeCompatibilityEntry {
        schema_version: RUNTIME_COMPATIBILITY_SCHEMA_VERSION.into(),
        adapter_family,
        adapter,
        protocol: protocol.into(),
        exact_versions: vec![
            safe_runtime_version(adapter_version).unwrap_or_else(|| "unknown".into())
        ],
        state: RuntimeCompatibilityState::Unknown,
        authority: FieldAuthority::Unknown,
        evidence_version: None,
        known_degradations: vec![
            RuntimeDegradation {
                code: "catalog-not-authenticated".into(),
                severity: RuntimeDegradationSeverity::Warning,
                affected_features: vec!["model-selection".into()],
                summary: "runtime compatibility is not authenticated".into(),
            },
            RuntimeDegradation {
                code: "model-capabilities-unknown".into(),
                severity: RuntimeDegradationSeverity::Warning,
                affected_features: vec!["capability-matching".into()],
                summary: "runtime-specific model capabilities are unknown".into(),
            },
        ],
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
            validate_identifier(runtime, "capability check runtime")?;
        }
        if let Some(runtime_version) = &self.runtime_version {
            validate_runtime_version(runtime_version, "capability check runtime version")?;
            if self.runtime.is_none() {
                return Err(error("capability check runtime version requires a runtime"));
            }
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
        let matrix_authority = model
            .field_authority
            .get("runtime_compatibility")
            .copied()
            .unwrap_or(FieldAuthority::Unknown);
        if model.runtime_compatibility_matrix.is_empty() {
            if model.runtime_compatibility.state == "verified"
                && authority_is_verified(matrix_authority)
                && model.runtime_compatibility.adapter == *runtime
            {
                match requirements.runtime_version.as_deref() {
                    None => unknown_check(
                        &mut checks,
                        &mut has_unknown,
                        "runtime",
                        json!({ "adapter": runtime, "version": Value::Null }),
                        json!(model.runtime_compatibility),
                        FieldAuthority::Unknown,
                    ),
                    Some(runtime_version)
                        if runtime_version != model.runtime_compatibility.adapter_version =>
                    {
                        blocked_check(
                            &mut checks,
                            &mut mismatches,
                            "runtime",
                            (
                                json!({ "adapter": runtime, "version": runtime_version }),
                                json!(model.runtime_compatibility.adapter_version),
                                matrix_authority,
                            ),
                            "runtime-version-not-verified",
                            "the requested runtime version is not verified for this model",
                        )
                    }
                    Some(_) => check(
                        &mut checks,
                        "runtime",
                        json!({ "adapter": runtime, "version": requirements.runtime_version }),
                        json!(model.runtime_compatibility),
                        matrix_authority,
                        "compatible",
                    ),
                }
            } else if model.runtime_compatibility.state == "verified"
                && authority_is_verified(matrix_authority)
            {
                blocked_check(
                    &mut checks,
                    &mut mismatches,
                    "runtime",
                    (
                        json!({
                            "adapter": runtime,
                            "version": requirements.runtime_version,
                        }),
                        json!(model.runtime_compatibility),
                        matrix_authority,
                    ),
                    "runtime-not-declared",
                    "the legacy compatibility summary does not declare this runtime",
                );
            } else {
                unknown_check(
                    &mut checks,
                    &mut has_unknown,
                    "runtime",
                    json!({
                        "adapter": runtime,
                        "version": requirements.runtime_version,
                    }),
                    json!(model.runtime_compatibility),
                    FieldAuthority::Unknown,
                );
            }
        } else {
            let compatibility = model
                .runtime_compatibility_matrix
                .iter()
                .find(|compatibility| compatibility.adapter == *runtime);
            match compatibility {
                Some(compatibility) => {
                    let authority = if authority_is_verified(matrix_authority)
                        && authority_is_verified(compatibility.authority)
                    {
                        compatibility.authority
                    } else {
                        FieldAuthority::Unknown
                    };
                    let version_verified = match requirements.runtime_version.as_deref() {
                        None => {
                            unknown_check(
                                &mut checks,
                                &mut has_unknown,
                                "runtime",
                                json!({ "adapter": runtime, "version": Value::Null }),
                                json!(compatibility),
                                FieldAuthority::Unknown,
                            );
                            false
                        }
                        Some(runtime_version)
                            if authority_is_verified(authority)
                                && !compatibility
                                    .exact_versions
                                    .iter()
                                    .any(|version| version == runtime_version) =>
                        {
                            blocked_check(
                                &mut checks,
                                &mut mismatches,
                                "runtime",
                                (
                                    json!({ "adapter": runtime, "version": runtime_version }),
                                    json!(compatibility.exact_versions),
                                    authority,
                                ),
                                "runtime-version-not-verified",
                                "the requested runtime version is not verified for this model",
                            );
                            false
                        }
                        Some(_) => true,
                    };
                    if version_verified {
                        match compatibility.state {
                            RuntimeCompatibilityState::Compatible
                            | RuntimeCompatibilityState::Degraded
                                if authority_is_verified(authority) =>
                            {
                                check(
                                    &mut checks,
                                    "runtime",
                                    json!({
                                        "adapter": runtime,
                                        "version": requirements.runtime_version,
                                    }),
                                    json!(compatibility),
                                    authority,
                                    "compatible",
                                );
                            }
                            RuntimeCompatibilityState::Incompatible
                                if authority_is_verified(authority) =>
                            {
                                blocked_check(
                                    &mut checks,
                                    &mut mismatches,
                                    "runtime",
                                    (
                                        json!({
                                            "adapter": runtime,
                                            "version": requirements.runtime_version,
                                        }),
                                        json!(compatibility),
                                        authority,
                                    ),
                                    "runtime-incompatible",
                                    "the compatibility matrix marks this runtime incompatible",
                                );
                            }
                            _ => unknown_check(
                                &mut checks,
                                &mut has_unknown,
                                "runtime",
                                json!({
                                    "adapter": runtime,
                                    "version": requirements.runtime_version,
                                }),
                                json!(compatibility),
                                authority,
                            ),
                        }
                    }
                }
                None if authority_is_verified(matrix_authority) => blocked_check(
                    &mut checks,
                    &mut mismatches,
                    "runtime",
                    (
                        json!({
                            "adapter": runtime,
                            "version": requirements.runtime_version,
                        }),
                        json!(model.runtime_compatibility_matrix),
                        matrix_authority,
                    ),
                    "runtime-not-declared",
                    "the verified compatibility matrix does not declare this runtime",
                ),
                None => unknown_check(
                    &mut checks,
                    &mut has_unknown,
                    "runtime",
                    json!({
                        "adapter": runtime,
                        "version": requirements.runtime_version,
                    }),
                    json!(model.runtime_compatibility_matrix),
                    FieldAuthority::Unknown,
                ),
            }
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

fn safe_identifier(value: &str) -> Option<String> {
    validate_identifier(value, "identifier")
        .ok()
        .map(|()| value.to_owned())
}

fn safe_runtime_version(value: &str) -> Option<String> {
    validate_runtime_version(value, "runtime version")
        .ok()
        .map(|()| value.to_owned())
}

fn validate_runtime_version(value: &str, field: &str) -> Result<(), CatalogError> {
    validate_text(value, field)?;
    if value.chars().any(char::is_whitespace)
        || value
            .chars()
            .any(|character| matches!(character, '*' | '^' | '~' | '<' | '>' | '=' | ',' | '|'))
    {
        return Err(error(format!(
            "{field} must be one exact canonical version"
        )));
    }
    Ok(())
}

fn validate_identifier(value: &str, field: &str) -> Result<(), CatalogError> {
    validate_text(value, field)?;
    if !value.chars().all(|character| {
        character.is_ascii_lowercase()
            || character.is_ascii_digit()
            || matches!(character, '-' | '_' | '.')
    }) || !value
        .chars()
        .next()
        .is_some_and(|character| character.is_ascii_lowercase() || character.is_ascii_digit())
    {
        return Err(error(format!("{field} is not a canonical identifier")));
    }
    Ok(())
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

    fn runtime_compatibility(
        adapter_family: RuntimeAdapterFamily,
        adapter: &str,
        protocol: &str,
        state: RuntimeCompatibilityState,
        degradation: Option<RuntimeDegradation>,
    ) -> RuntimeCompatibilityEntry {
        RuntimeCompatibilityEntry {
            schema_version: RUNTIME_COMPATIBILITY_SCHEMA_VERSION.into(),
            adapter_family,
            adapter: adapter.into(),
            protocol: protocol.into(),
            exact_versions: vec!["1.0.0".into()],
            state,
            authority: FieldAuthority::AegisyConfigured,
            evidence_version: Some("runtime-evaluation-v1".into()),
            known_degradations: degradation.into_iter().collect(),
        }
    }

    fn runtime_degradation(code: &str, severity: RuntimeDegradationSeverity) -> RuntimeDegradation {
        RuntimeDegradation {
            code: code.into(),
            severity,
            affected_features: vec!["tool-calls".into()],
            summary: "runtime behavior differs from the complete contract".into(),
        }
    }

    fn fresh_catalog_for_runtime_checks() -> ModelCatalog {
        let mut catalog = offline_for_runtime("preview", "1.0.0", Some("local"), Some("echo"));
        catalog.state = CatalogState::Fresh;
        catalog.signature_validated = true;
        catalog.issued_at_ms = Some(1);
        catalog.expires_at_ms = Some(10_000);
        catalog.validation_errors.clear();
        catalog.models[0].availability = Availability::Available;
        catalog.models[0].entitlement = Entitlement::Allowed;
        for field in ["availability", "entitlement", "runtime_compatibility"] {
            catalog.models[0]
                .field_authority
                .insert(field.into(), FieldAuthority::AegisyConfigured);
        }
        catalog
    }

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
    fn runtime_matrix_represents_codex_acp_and_native_with_structured_degradations() {
        let mut catalog = offline_for_runtime("preview", "1", Some("local"), Some("echo"));
        catalog.models[0].field_authority.insert(
            "runtime_compatibility".into(),
            FieldAuthority::AegisyConfigured,
        );
        catalog.models[0].runtime_compatibility_matrix = vec![
            runtime_compatibility(
                RuntimeAdapterFamily::CodexAppServer,
                "codex-app-server",
                "codex-app-server",
                RuntimeCompatibilityState::Compatible,
                None,
            ),
            runtime_compatibility(
                RuntimeAdapterFamily::Acp,
                "acp",
                "acp",
                RuntimeCompatibilityState::Degraded,
                Some(runtime_degradation(
                    "structured-patch-unavailable",
                    RuntimeDegradationSeverity::Warning,
                )),
            ),
            runtime_compatibility(
                RuntimeAdapterFamily::Native,
                "aegisy-native",
                "aap-native",
                RuntimeCompatibilityState::Incompatible,
                Some(runtime_degradation(
                    "native-adapter-not-released",
                    RuntimeDegradationSeverity::Blocking,
                )),
            ),
        ];
        assert!(catalog.validate().is_ok());
        let value = serde_json::to_value(&catalog).unwrap();
        assert_eq!(
            value["models"][0]["runtime_compatibility_matrix"][0]["adapter_family"],
            "codex-app-server"
        );
        assert_eq!(
            value["models"][0]["runtime_compatibility_matrix"][1]["known_degradations"][0]
                ["severity"],
            "warning"
        );
        assert_eq!(
            value["models"][0]["runtime_compatibility_matrix"][2]["state"],
            "incompatible"
        );
    }

    #[test]
    fn runtime_matrix_rejects_duplicate_inconsistent_and_secret_metadata() {
        let mut catalog = offline_for_runtime("preview", "1", Some("local"), Some("echo"));
        catalog.models[0].field_authority.insert(
            "runtime_compatibility".into(),
            FieldAuthority::AegisyConfigured,
        );
        let compatible = runtime_compatibility(
            RuntimeAdapterFamily::Acp,
            "acp",
            "acp",
            RuntimeCompatibilityState::Compatible,
            None,
        );
        catalog.models[0].runtime_compatibility_matrix = vec![compatible.clone(), compatible];
        assert!(catalog.validate().is_err());

        catalog.models[0].runtime_compatibility_matrix = vec![
            runtime_compatibility(
                RuntimeAdapterFamily::CodexAppServer,
                "shared-adapter",
                "codex-app-server",
                RuntimeCompatibilityState::Compatible,
                None,
            ),
            runtime_compatibility(
                RuntimeAdapterFamily::Acp,
                "shared-adapter",
                "acp",
                RuntimeCompatibilityState::Compatible,
                None,
            ),
        ];
        assert!(catalog.validate().is_err());

        let mut mismatched_protocol = runtime_compatibility(
            RuntimeAdapterFamily::CodexAppServer,
            "codex-app-server",
            "acp",
            RuntimeCompatibilityState::Compatible,
            None,
        );
        catalog.models[0].runtime_compatibility_matrix = vec![mismatched_protocol.clone()];
        assert!(catalog.validate().is_err());

        mismatched_protocol.adapter_family = RuntimeAdapterFamily::Acp;
        mismatched_protocol.exact_versions = vec!["^1.0.0".into()];
        catalog.models[0].runtime_compatibility_matrix = vec![mismatched_protocol];
        assert!(catalog.validate().is_err());

        catalog.models[0].runtime_compatibility_matrix = vec![runtime_compatibility(
            RuntimeAdapterFamily::Acp,
            "acp",
            "acp",
            RuntimeCompatibilityState::Unknown,
            None,
        )];
        assert!(catalog.validate().is_err());

        let mut incompatible = runtime_compatibility(
            RuntimeAdapterFamily::Acp,
            "acp",
            "acp",
            RuntimeCompatibilityState::Incompatible,
            Some(runtime_degradation(
                "tools-blocked",
                RuntimeDegradationSeverity::Blocking,
            )),
        );
        incompatible.known_degradations[0].summary = "token=secret-value".into();
        catalog.models[0].runtime_compatibility_matrix = vec![incompatible];
        assert!(catalog.validate().is_err());
    }

    #[test]
    fn verified_runtime_matrix_blocks_absent_and_incompatible_adapters() {
        let mut catalog = fresh_catalog_for_runtime_checks();
        catalog.models[0].runtime_compatibility_matrix = vec![runtime_compatibility(
            RuntimeAdapterFamily::Acp,
            "acp",
            "acp",
            RuntimeCompatibilityState::Incompatible,
            Some(runtime_degradation(
                "tools-blocked",
                RuntimeDegradationSeverity::Blocking,
            )),
        )];
        let incompatible = check_capabilities(
            &catalog,
            "local:echo",
            CapabilityRequirements {
                mode: "chat".into(),
                attachments: Vec::new(),
                tools: false,
                reasoning: false,
                context_tokens: None,
                runtime: Some("acp".into()),
                runtime_version: Some("1.0.0".into()),
                zero_data_retention: false,
            },
        )
        .unwrap();
        assert_eq!(incompatible.decision, "blocked");
        assert_eq!(incompatible.mismatches[0].code, "runtime-incompatible");

        let absent = check_capabilities(
            &catalog,
            "local:echo",
            CapabilityRequirements {
                mode: "chat".into(),
                attachments: Vec::new(),
                tools: false,
                reasoning: false,
                context_tokens: None,
                runtime: Some("codex-app-server".into()),
                runtime_version: Some("1.0.0".into()),
                zero_data_retention: false,
            },
        )
        .unwrap();
        assert_eq!(absent.decision, "blocked");
        assert_eq!(absent.mismatches[0].code, "runtime-not-declared");
    }

    #[test]
    fn verified_warning_degradation_is_visible_without_becoming_a_blocker() {
        let mut catalog = fresh_catalog_for_runtime_checks();
        catalog.models[0].runtime_compatibility_matrix = vec![runtime_compatibility(
            RuntimeAdapterFamily::CodexAppServer,
            "codex-app-server",
            "codex-app-server",
            RuntimeCompatibilityState::Degraded,
            Some(runtime_degradation(
                "background-jobs-unavailable",
                RuntimeDegradationSeverity::Warning,
            )),
        )];
        let result = check_capabilities(
            &catalog,
            "local:echo",
            CapabilityRequirements {
                mode: "chat".into(),
                attachments: Vec::new(),
                tools: false,
                reasoning: false,
                context_tokens: None,
                runtime: Some("codex-app-server".into()),
                runtime_version: Some("1.0.0".into()),
                zero_data_retention: false,
            },
        )
        .unwrap();
        assert_eq!(result.decision, "compatible");
        assert!(result.selection_allowed);
        assert_eq!(
            result.checks[2].observed["known_degradations"][0]["code"],
            "background-jobs-unavailable"
        );
    }

    #[test]
    fn verified_runtime_matrix_requires_an_exact_requested_version() {
        let mut catalog = fresh_catalog_for_runtime_checks();
        catalog.models[0].runtime_compatibility_matrix = vec![runtime_compatibility(
            RuntimeAdapterFamily::CodexAppServer,
            "codex-app-server",
            "codex-app-server",
            RuntimeCompatibilityState::Compatible,
            None,
        )];

        let missing_version = check_capabilities(
            &catalog,
            "local:echo",
            CapabilityRequirements {
                mode: "chat".into(),
                attachments: Vec::new(),
                tools: false,
                reasoning: false,
                context_tokens: None,
                runtime: Some("codex-app-server".into()),
                runtime_version: None,
                zero_data_retention: false,
            },
        )
        .unwrap();
        assert_eq!(missing_version.decision, "unknown");
        assert!(!missing_version.selection_allowed);
        assert!(missing_version
            .checks
            .iter()
            .any(|check| check.capability == "runtime" && check.result == "unknown"));

        let unsupported_version = check_capabilities(
            &catalog,
            "local:echo",
            CapabilityRequirements {
                mode: "chat".into(),
                attachments: Vec::new(),
                tools: false,
                reasoning: false,
                context_tokens: None,
                runtime: Some("codex-app-server".into()),
                runtime_version: Some("2.0.0".into()),
                zero_data_retention: false,
            },
        )
        .unwrap();
        assert_eq!(unsupported_version.decision, "blocked");
        assert_eq!(
            unsupported_version.mismatches[0].code,
            "runtime-version-not-verified"
        );

        let invalid = CapabilityRequirements {
            mode: "chat".into(),
            attachments: Vec::new(),
            tools: false,
            reasoning: false,
            context_tokens: None,
            runtime: None,
            runtime_version: Some("1.0.0".into()),
            zero_data_retention: false,
        };
        assert!(invalid.validate_and_normalize().is_err());

        let version_range = CapabilityRequirements {
            mode: "chat".into(),
            attachments: Vec::new(),
            tools: false,
            reasoning: false,
            context_tokens: None,
            runtime: Some("codex-app-server".into()),
            runtime_version: Some("1.*".into()),
            zero_data_retention: false,
        };
        assert!(version_range.validate_and_normalize().is_err());
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
                runtime_version: None,
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
                runtime_version: None,
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
        catalog.models[0].field_authority.insert(
            "runtime_compatibility".into(),
            FieldAuthority::AegisyConfigured,
        );
        let runtime = &mut catalog.models[0].runtime_compatibility_matrix[0];
        runtime.state = RuntimeCompatibilityState::Compatible;
        runtime.authority = FieldAuthority::AegisyConfigured;
        runtime.evidence_version = Some("runtime-fixture-v1".into());
        runtime.known_degradations.clear();
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
                runtime_version: Some("1".into()),
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
                runtime_version: None,
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
                runtime_version: None,
                zero_data_retention: true,
            },
        )
        .unwrap();
        assert_eq!(result.decision, "unknown");
        assert!(result.mismatches.is_empty());
        assert!(result.checks.iter().all(|check| check.result == "unknown"));
    }
}
