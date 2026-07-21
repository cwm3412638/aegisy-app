//! Source-qualified token, context, cost, and reasoning metadata.
//!
//! This internal contract describes evidence; it does not grant routing,
//! billing, model, token, turn, or execution authority. Unknown data has no
//! value, stale data cannot remain authoritative, and fresh catalog-derived
//! cost requires both catalog and derivation-input identities.

use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;

pub const SCHEMA_VERSION: &str = "usage-authority/0.1";
const MAX_ESTIMATOR_ID_BYTES: usize = 128;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UsageAuthorityError {
    pub code: &'static str,
    pub message: &'static str,
}

impl UsageAuthorityError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum UsageMetric {
    Token,
    Context,
    Cost,
    Reasoning,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum AuthorityLabel {
    Observed,
    CatalogDerived,
    Estimated,
    Stale,
    Unknown,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ObservationSource {
    ProviderResponse,
    RuntimeObservation,
    GatewayObservation,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum StaleReason {
    Expired,
    SourceUnavailable,
    ModelChanged,
    RoutingChanged,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum MissingSource {
    ProviderUsage,
    RuntimeObservation,
    Catalog,
    Estimator,
    Correlation,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum UnknownReason {
    NotReported,
    Unavailable,
    NotApplicable,
    CorrelationMissing,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub enum AuthorityEvidence {
    Observed {
        source: ObservationSource,
        evidence_identity: String,
        observed_at_ms: u64,
    },
    CatalogDerived {
        catalog_identity: String,
        catalog_issued_at_ms: u64,
        catalog_expires_at_ms: u64,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        derivation_input_identity: Option<String>,
    },
    Estimated {
        estimator_id: String,
        estimator_identity: String,
        estimated_at_ms: u64,
    },
    Stale {
        previous_authority: AuthorityLabel,
        source_identity: String,
        source_observed_at_ms: u64,
        stale_since_ms: u64,
        reason: StaleReason,
    },
    Unknown {
        missing_source: MissingSource,
        reason: UnknownReason,
    },
}

impl AuthorityEvidence {
    pub fn label(&self) -> AuthorityLabel {
        match self {
            Self::Observed { .. } => AuthorityLabel::Observed,
            Self::CatalogDerived { .. } => AuthorityLabel::CatalogDerived,
            Self::Estimated { .. } => AuthorityLabel::Estimated,
            Self::Stale { .. } => AuthorityLabel::Stale,
            Self::Unknown { .. } => AuthorityLabel::Unknown,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TokenUsageValue {
    pub input_tokens: Option<u64>,
    pub output_tokens: Option<u64>,
    pub total_tokens: Option<u64>,
    pub cached_input_tokens: Option<u64>,
    pub reasoning_output_tokens: Option<u64>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ContextUsageValue {
    pub used_tokens: Option<u64>,
    pub window_tokens: Option<u64>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CostUsageValue {
    pub amount_micros: u64,
    pub currency: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ReasoningUsageValue {
    pub available: Option<bool>,
    pub enabled: Option<bool>,
    pub output_tokens: Option<u64>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub enum UsageValue {
    Token(TokenUsageValue),
    Context(ContextUsageValue),
    Cost(CostUsageValue),
    Reasoning(ReasoningUsageValue),
}

impl UsageValue {
    fn metric(&self) -> UsageMetric {
        match self {
            Self::Token(_) => UsageMetric::Token,
            Self::Context(_) => UsageMetric::Context,
            Self::Cost(_) => UsageMetric::Cost,
            Self::Reasoning(_) => UsageMetric::Reasoning,
        }
    }

    fn validate(&self) -> Result<(), UsageAuthorityError> {
        match self {
            Self::Token(value) => validate_token_value(value),
            Self::Context(value) => validate_context_value(value),
            Self::Cost(value) => validate_cost_value(value),
            Self::Reasoning(value) => validate_reasoning_value(value),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct UsageAuthorityEntry {
    pub metric: UsageMetric,
    pub authority: AuthorityLabel,
    pub authoritative: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub value: Option<UsageValue>,
    pub evidence: AuthorityEvidence,
}

impl UsageAuthorityEntry {
    pub fn validate(&self, as_of_ms: u64) -> Result<(), UsageAuthorityError> {
        if self.authority != self.evidence.label() {
            return Err(error(
                "usage-authority-label-mismatch",
                "usage authority label and evidence do not agree",
            ));
        }

        let should_be_authoritative = matches!(
            self.authority,
            AuthorityLabel::Observed | AuthorityLabel::CatalogDerived
        );
        if self.authoritative != should_be_authoritative {
            return Err(error(
                "usage-authority-flag-invalid",
                "usage authoritative flag does not agree with its label",
            ));
        }

        match (&self.authority, &self.value) {
            (AuthorityLabel::Unknown, None) => {}
            (AuthorityLabel::Unknown, Some(_)) => {
                return Err(error(
                    "usage-authority-unknown-has-value",
                    "unknown usage cannot carry a value",
                ));
            }
            (_, None) => {
                return Err(error(
                    "usage-authority-value-missing",
                    "non-unknown usage requires a value",
                ));
            }
            (_, Some(value)) if value.metric() != self.metric => {
                return Err(error(
                    "usage-authority-metric-mismatch",
                    "usage metric and value kind do not agree",
                ));
            }
            (_, Some(value)) => value.validate()?,
        }

        match &self.evidence {
            AuthorityEvidence::Observed {
                evidence_identity,
                observed_at_ms,
                ..
            } => {
                validate_identity(evidence_identity)?;
                validate_evidence_time(*observed_at_ms, as_of_ms)?;
            }
            AuthorityEvidence::CatalogDerived {
                catalog_identity,
                catalog_issued_at_ms,
                catalog_expires_at_ms,
                derivation_input_identity,
            } => {
                validate_identity_with_prefix(catalog_identity, "model-catalog:sha256:")?;
                if *catalog_issued_at_ms > as_of_ms
                    || *catalog_expires_at_ms < as_of_ms
                    || *catalog_expires_at_ms <= *catalog_issued_at_ms
                {
                    return Err(error(
                        "usage-authority-catalog-time-invalid",
                        "catalog-derived usage requires a fresh catalog at the report time",
                    ));
                }
                if let Some(identity) = derivation_input_identity {
                    validate_identity(identity)?;
                }
                if self.metric == UsageMetric::Cost && derivation_input_identity.is_none() {
                    return Err(error(
                        "usage-authority-cost-input-missing",
                        "catalog-derived cost requires a derivation input identity",
                    ));
                }
            }
            AuthorityEvidence::Estimated {
                estimator_id,
                estimator_identity,
                estimated_at_ms,
            } => {
                if !valid_identifier(estimator_id) {
                    return Err(error(
                        "usage-authority-estimator-invalid",
                        "usage estimator identifier is invalid",
                    ));
                }
                validate_identity_with_prefix(estimator_identity, "estimator:sha256:")?;
                validate_evidence_time(*estimated_at_ms, as_of_ms)?;
            }
            AuthorityEvidence::Stale {
                previous_authority,
                source_identity,
                source_observed_at_ms,
                stale_since_ms,
                ..
            } => {
                if matches!(
                    previous_authority,
                    AuthorityLabel::Stale | AuthorityLabel::Unknown
                ) {
                    return Err(error(
                        "usage-authority-stale-source-invalid",
                        "stale usage requires a previously known authority",
                    ));
                }
                validate_identity(source_identity)?;
                if *source_observed_at_ms > *stale_since_ms || *stale_since_ms > as_of_ms {
                    return Err(error(
                        "usage-authority-stale-time-invalid",
                        "stale usage evidence times are inconsistent",
                    ));
                }
            }
            AuthorityEvidence::Unknown { .. } => {}
        }

        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct UsageAuthorityReport {
    pub schema_version: String,
    pub as_of_ms: u64,
    pub entries: Vec<UsageAuthorityEntry>,
}

impl UsageAuthorityReport {
    pub fn new(
        as_of_ms: u64,
        mut entries: Vec<UsageAuthorityEntry>,
    ) -> Result<Self, UsageAuthorityError> {
        entries.sort_by_key(|entry| entry.metric);
        let report = Self {
            schema_version: SCHEMA_VERSION.to_owned(),
            as_of_ms,
            entries,
        };
        report.validate()?;
        Ok(report)
    }

    pub fn validate(&self) -> Result<(), UsageAuthorityError> {
        if self.schema_version != SCHEMA_VERSION {
            return Err(error(
                "usage-authority-schema-invalid",
                "usage authority schema version is unsupported",
            ));
        }
        if self.entries.len() != 4 {
            return Err(error(
                "usage-authority-metrics-incomplete",
                "usage authority report must contain exactly four metrics",
            ));
        }

        let mut metrics = BTreeSet::new();
        for entry in &self.entries {
            if !metrics.insert(entry.metric) {
                return Err(error(
                    "usage-authority-metric-duplicate",
                    "usage authority report contains a duplicate metric",
                ));
            }
            entry.validate(self.as_of_ms)?;
        }
        if metrics
            != BTreeSet::from([
                UsageMetric::Token,
                UsageMetric::Context,
                UsageMetric::Cost,
                UsageMetric::Reasoning,
            ])
        {
            return Err(error(
                "usage-authority-metrics-incomplete",
                "usage authority report is missing a required metric",
            ));
        }

        let token_reasoning = self.entries.iter().find_map(|entry| match &entry.value {
            Some(UsageValue::Token(value)) => value.reasoning_output_tokens,
            _ => None,
        });
        let reasoning = self.entries.iter().find_map(|entry| match &entry.value {
            Some(UsageValue::Reasoning(value)) => value.output_tokens,
            _ => None,
        });
        if token_reasoning
            .zip(reasoning)
            .is_some_and(|(left, right)| left != right)
        {
            return Err(error(
                "usage-authority-reasoning-token-mismatch",
                "token and reasoning usage disagree on reasoning output tokens",
            ));
        }

        Ok(())
    }

    pub fn entry(&self, metric: UsageMetric) -> Option<&UsageAuthorityEntry> {
        self.entries.iter().find(|entry| entry.metric == metric)
    }
}

/// Build the metadata-only authority projection for the normalized Codex
/// `thread/tokenUsage/updated` payload. The raw payload remains caller-owned;
/// this function only reads bounded numeric fields and never infers price or
/// model-selection authority. Provider totals whose component semantics do
/// not reconcile are retained by the raw projection but omitted from the
/// authoritative contract instead of being silently corrected.
pub fn from_provider_token_usage(
    usage: &Value,
    observed_at_ms: u64,
) -> Result<UsageAuthorityReport, UsageAuthorityError> {
    let identity = format!(
        "provider-usage:sha256:{:x}",
        Sha256::digest(serde_json::to_vec(usage).map_err(|_| {
            error(
                "usage-authority-provider-payload-invalid",
                "provider usage payload cannot be canonicalized",
            )
        })?)
    );
    let total = usage
        .get("total")
        .and_then(Value::as_object)
        .ok_or_else(|| {
            error(
                "usage-authority-provider-payload-invalid",
                "provider usage total breakdown is missing",
            )
        })?;
    let last = usage
        .get("last")
        .and_then(Value::as_object)
        .ok_or_else(|| {
            error(
                "usage-authority-provider-payload-invalid",
                "provider usage last breakdown is missing",
            )
        })?;
    let input = required_nonnegative_u64(total, "input_tokens")?;
    let output = required_nonnegative_u64(total, "output_tokens")?;
    let provider_total = required_nonnegative_u64(total, "total_tokens")?;
    let cached = required_nonnegative_u64(total, "cached_input_tokens")?;
    let reasoning_output = required_nonnegative_u64(total, "reasoning_output_tokens")?;
    let context_used = required_nonnegative_u64(last, "input_tokens")?;

    let reconciled_total = input
        .checked_add(output)
        .filter(|expected| *expected == provider_total);
    let token_reasoning = (reasoning_output <= output).then_some(reasoning_output);
    let token = UsageValue::Token(TokenUsageValue {
        input_tokens: Some(input),
        output_tokens: Some(output),
        total_tokens: reconciled_total,
        cached_input_tokens: (cached <= input).then_some(cached),
        reasoning_output_tokens: token_reasoning,
    });
    let observed = |metric: UsageMetric, value: UsageValue| UsageAuthorityEntry {
        metric,
        authority: AuthorityLabel::Observed,
        authoritative: true,
        value: Some(value),
        evidence: AuthorityEvidence::Observed {
            source: ObservationSource::ProviderResponse,
            evidence_identity: identity.clone(),
            observed_at_ms,
        },
    };
    let unknown = |metric: UsageMetric, missing_source: MissingSource, reason: UnknownReason| {
        UsageAuthorityEntry {
            metric,
            authority: AuthorityLabel::Unknown,
            authoritative: false,
            value: None,
            evidence: AuthorityEvidence::Unknown {
                missing_source,
                reason,
            },
        }
    };

    let context_window = usage
        .get("model_context_window")
        .and_then(Value::as_u64)
        .filter(|window| *window > 0);
    let context = context_window.map_or_else(
        || {
            unknown(
                UsageMetric::Context,
                MissingSource::ProviderUsage,
                UnknownReason::NotReported,
            )
        },
        |window| {
            observed(
                UsageMetric::Context,
                UsageValue::Context(ContextUsageValue {
                    used_tokens: Some(context_used),
                    window_tokens: Some(window),
                }),
            )
        },
    );
    let reasoning = if reasoning_output <= output {
        observed(
            UsageMetric::Reasoning,
            UsageValue::Reasoning(ReasoningUsageValue {
                available: None,
                enabled: None,
                output_tokens: Some(reasoning_output),
            }),
        )
    } else {
        unknown(
            UsageMetric::Reasoning,
            MissingSource::ProviderUsage,
            UnknownReason::Unavailable,
        )
    };

    UsageAuthorityReport::new(
        observed_at_ms,
        vec![
            observed(UsageMetric::Token, token),
            context,
            unknown(
                UsageMetric::Cost,
                MissingSource::Catalog,
                UnknownReason::Unavailable,
            ),
            reasoning,
        ],
    )
}

fn required_nonnegative_u64(
    object: &serde_json::Map<String, Value>,
    key: &str,
) -> Result<u64, UsageAuthorityError> {
    object.get(key).and_then(Value::as_u64).ok_or_else(|| {
        error(
            "usage-authority-provider-payload-invalid",
            "provider usage breakdown contains a missing or invalid count",
        )
    })
}

fn validate_token_value(value: &TokenUsageValue) -> Result<(), UsageAuthorityError> {
    if value.input_tokens.is_none()
        && value.output_tokens.is_none()
        && value.total_tokens.is_none()
        && value.cached_input_tokens.is_none()
        && value.reasoning_output_tokens.is_none()
    {
        return Err(error(
            "usage-authority-token-empty",
            "token usage must contain at least one token count",
        ));
    }
    if value
        .cached_input_tokens
        .zip(value.input_tokens)
        .is_some_and(|(cached, input)| cached > input)
    {
        return Err(error(
            "usage-authority-token-cache-invalid",
            "cached input tokens cannot exceed input tokens",
        ));
    }
    if value
        .reasoning_output_tokens
        .zip(value.output_tokens)
        .is_some_and(|(reasoning, output)| reasoning > output)
    {
        return Err(error(
            "usage-authority-token-reasoning-invalid",
            "reasoning output tokens cannot exceed output tokens",
        ));
    }
    if let (Some(input), Some(output), Some(total)) =
        (value.input_tokens, value.output_tokens, value.total_tokens)
    {
        if input.checked_add(output) != Some(total) {
            return Err(error(
                "usage-authority-token-total-invalid",
                "token total does not equal input plus output",
            ));
        }
    }
    Ok(())
}

fn validate_context_value(value: &ContextUsageValue) -> Result<(), UsageAuthorityError> {
    if value.used_tokens.is_none() && value.window_tokens.is_none() {
        return Err(error(
            "usage-authority-context-empty",
            "context usage must contain used or window tokens",
        ));
    }
    if value.window_tokens == Some(0) {
        return Err(error(
            "usage-authority-context-window-invalid",
            "context window tokens must be positive",
        ));
    }
    Ok(())
}

fn validate_cost_value(value: &CostUsageValue) -> Result<(), UsageAuthorityError> {
    if value.currency.len() != 3 || !value.currency.bytes().all(|byte| byte.is_ascii_uppercase()) {
        return Err(error(
            "usage-authority-currency-invalid",
            "cost currency must be a three-letter uppercase code",
        ));
    }
    Ok(())
}

fn validate_reasoning_value(value: &ReasoningUsageValue) -> Result<(), UsageAuthorityError> {
    if value.available.is_none() && value.enabled.is_none() && value.output_tokens.is_none() {
        return Err(error(
            "usage-authority-reasoning-empty",
            "reasoning usage must contain a status or token count",
        ));
    }
    if value.available == Some(false) && value.enabled == Some(true) {
        return Err(error(
            "usage-authority-reasoning-status-invalid",
            "unavailable reasoning cannot be enabled",
        ));
    }
    if value.enabled == Some(false) && value.output_tokens.unwrap_or(0) > 0 {
        return Err(error(
            "usage-authority-reasoning-output-invalid",
            "disabled reasoning cannot report output tokens",
        ));
    }
    Ok(())
}

fn validate_evidence_time(value: u64, as_of_ms: u64) -> Result<(), UsageAuthorityError> {
    if value > as_of_ms {
        return Err(error(
            "usage-authority-evidence-future",
            "usage evidence cannot be newer than its report",
        ));
    }
    Ok(())
}

fn validate_identity(value: &str) -> Result<(), UsageAuthorityError> {
    if value.split_once(":sha256:").is_some_and(|(prefix, hex)| {
        !prefix.is_empty()
            && prefix
                .bytes()
                .all(|byte| byte.is_ascii_lowercase() || byte == b'-')
            && valid_hex(hex)
    }) {
        Ok(())
    } else {
        Err(error(
            "usage-authority-identity-invalid",
            "usage evidence identity is invalid",
        ))
    }
}

fn validate_identity_with_prefix(value: &str, prefix: &str) -> Result<(), UsageAuthorityError> {
    if value.strip_prefix(prefix).is_some_and(valid_hex) {
        Ok(())
    } else {
        Err(error(
            "usage-authority-identity-invalid",
            "usage evidence identity is invalid",
        ))
    }
}

fn valid_hex(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
}

fn valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= MAX_ESTIMATOR_ID_BYTES
        && value.bytes().all(|byte| {
            byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'-' | b'_' | b'.')
        })
}

fn error(code: &'static str, message: &'static str) -> UsageAuthorityError {
    UsageAuthorityError::new(code, message)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn identity(prefix: &str, byte: char) -> String {
        format!("{prefix}{}", byte.to_string().repeat(64))
    }

    fn entry(
        metric: UsageMetric,
        authority: AuthorityLabel,
        authoritative: bool,
        value: Option<UsageValue>,
        evidence: AuthorityEvidence,
    ) -> UsageAuthorityEntry {
        UsageAuthorityEntry {
            metric,
            authority,
            authoritative,
            value,
            evidence,
        }
    }

    fn valid_entries() -> Vec<UsageAuthorityEntry> {
        vec![
            entry(
                UsageMetric::Reasoning,
                AuthorityLabel::Observed,
                true,
                Some(UsageValue::Reasoning(ReasoningUsageValue {
                    available: Some(true),
                    enabled: Some(true),
                    output_tokens: Some(20),
                })),
                AuthorityEvidence::Observed {
                    source: ObservationSource::ProviderResponse,
                    evidence_identity: identity("provider-usage:sha256:", 'a'),
                    observed_at_ms: 900,
                },
            ),
            entry(
                UsageMetric::Token,
                AuthorityLabel::Observed,
                true,
                Some(UsageValue::Token(TokenUsageValue {
                    input_tokens: Some(80),
                    output_tokens: Some(20),
                    total_tokens: Some(100),
                    cached_input_tokens: Some(40),
                    reasoning_output_tokens: Some(20),
                })),
                AuthorityEvidence::Observed {
                    source: ObservationSource::ProviderResponse,
                    evidence_identity: identity("provider-usage:sha256:", 'a'),
                    observed_at_ms: 900,
                },
            ),
            entry(
                UsageMetric::Context,
                AuthorityLabel::CatalogDerived,
                true,
                Some(UsageValue::Context(ContextUsageValue {
                    used_tokens: Some(100),
                    window_tokens: Some(128_000),
                })),
                AuthorityEvidence::CatalogDerived {
                    catalog_identity: identity("model-catalog:sha256:", 'b'),
                    catalog_issued_at_ms: 100,
                    catalog_expires_at_ms: 2_000,
                    derivation_input_identity: None,
                },
            ),
            entry(
                UsageMetric::Cost,
                AuthorityLabel::Estimated,
                false,
                Some(UsageValue::Cost(CostUsageValue {
                    amount_micros: 25,
                    currency: "USD".into(),
                })),
                AuthorityEvidence::Estimated {
                    estimator_id: "catalog-rate-projection".into(),
                    estimator_identity: identity("estimator:sha256:", 'c'),
                    estimated_at_ms: 950,
                },
            ),
        ]
    }

    #[test]
    fn validates_and_canonicalizes_all_authority_labels() {
        let report = UsageAuthorityReport::new(1_000, valid_entries()).unwrap();
        assert_eq!(report.schema_version, SCHEMA_VERSION);
        assert_eq!(report.entries[0].metric, UsageMetric::Token);
        assert_eq!(report.entries[1].metric, UsageMetric::Context);
        assert_eq!(report.entries[2].metric, UsageMetric::Cost);
        assert_eq!(report.entries[3].metric, UsageMetric::Reasoning);
        assert!(report.entry(UsageMetric::Token).unwrap().authoritative);
        assert!(!report.entry(UsageMetric::Cost).unwrap().authoritative);

        let serialized = serde_json::to_value(&report).unwrap();
        assert_eq!(serialized["entries"][0]["authority"], "observed");
        assert_eq!(serialized["entries"][1]["authority"], "catalog-derived");
        assert_eq!(serialized["entries"][2]["authority"], "estimated");
        let decoded: UsageAuthorityReport = serde_json::from_value(serialized).unwrap();
        decoded.validate().unwrap();
    }

    #[test]
    fn validates_stale_and_unknown_without_authoritative_values() {
        let mut entries = valid_entries();
        entries[3] = entry(
            UsageMetric::Cost,
            AuthorityLabel::Stale,
            false,
            Some(UsageValue::Cost(CostUsageValue {
                amount_micros: 25,
                currency: "USD".into(),
            })),
            AuthorityEvidence::Stale {
                previous_authority: AuthorityLabel::CatalogDerived,
                source_identity: identity("cost-calculation:sha256:", 'd'),
                source_observed_at_ms: 800,
                stale_since_ms: 900,
                reason: StaleReason::Expired,
            },
        );
        entries[0] = entry(
            UsageMetric::Reasoning,
            AuthorityLabel::Unknown,
            false,
            None,
            AuthorityEvidence::Unknown {
                missing_source: MissingSource::ProviderUsage,
                reason: UnknownReason::NotReported,
            },
        );

        let report = UsageAuthorityReport::new(1_000, entries).unwrap();
        assert_eq!(
            report.entry(UsageMetric::Cost).unwrap().authority,
            AuthorityLabel::Stale
        );
        assert_eq!(report.entry(UsageMetric::Reasoning).unwrap().value, None);
    }

    #[test]
    fn rejects_label_value_and_authoritative_flag_forgery() {
        let mut entries = valid_entries();
        entries[0].authority = AuthorityLabel::Estimated;
        assert_eq!(
            UsageAuthorityReport::new(1_000, entries).unwrap_err().code,
            "usage-authority-label-mismatch"
        );

        let mut entries = valid_entries();
        entries[3].authoritative = true;
        assert_eq!(
            UsageAuthorityReport::new(1_000, entries).unwrap_err().code,
            "usage-authority-flag-invalid"
        );

        let mut entries = valid_entries();
        entries[0] = entry(
            UsageMetric::Reasoning,
            AuthorityLabel::Unknown,
            false,
            Some(UsageValue::Reasoning(ReasoningUsageValue {
                available: None,
                enabled: Some(false),
                output_tokens: None,
            })),
            AuthorityEvidence::Unknown {
                missing_source: MissingSource::ProviderUsage,
                reason: UnknownReason::NotReported,
            },
        );
        assert_eq!(
            UsageAuthorityReport::new(1_000, entries).unwrap_err().code,
            "usage-authority-unknown-has-value"
        );
    }

    #[test]
    fn rejects_expired_catalog_cost_without_inputs_and_invalid_stale_lineage() {
        let catalog_cost = entry(
            UsageMetric::Cost,
            AuthorityLabel::CatalogDerived,
            true,
            Some(UsageValue::Cost(CostUsageValue {
                amount_micros: 12,
                currency: "USD".into(),
            })),
            AuthorityEvidence::CatalogDerived {
                catalog_identity: identity("model-catalog:sha256:", 'a'),
                catalog_issued_at_ms: 100,
                catalog_expires_at_ms: 900,
                derivation_input_identity: Some(identity("provider-usage:sha256:", 'b')),
            },
        );
        assert_eq!(
            catalog_cost.validate(1_000).unwrap_err().code,
            "usage-authority-catalog-time-invalid"
        );

        let mut no_input = catalog_cost;
        if let AuthorityEvidence::CatalogDerived {
            catalog_expires_at_ms,
            derivation_input_identity,
            ..
        } = &mut no_input.evidence
        {
            *catalog_expires_at_ms = 2_000;
            *derivation_input_identity = None;
        }
        assert_eq!(
            no_input.validate(1_000).unwrap_err().code,
            "usage-authority-cost-input-missing"
        );

        let stale = entry(
            UsageMetric::Cost,
            AuthorityLabel::Stale,
            false,
            Some(UsageValue::Cost(CostUsageValue {
                amount_micros: 12,
                currency: "USD".into(),
            })),
            AuthorityEvidence::Stale {
                previous_authority: AuthorityLabel::Unknown,
                source_identity: identity("cost-calculation:sha256:", 'c'),
                source_observed_at_ms: 100,
                stale_since_ms: 900,
                reason: StaleReason::SourceUnavailable,
            },
        );
        assert_eq!(
            stale.validate(1_000).unwrap_err().code,
            "usage-authority-stale-source-invalid"
        );
    }

    #[test]
    fn rejects_incomplete_duplicate_or_cross_metric_reports() {
        let mut entries = valid_entries();
        entries.pop();
        assert_eq!(
            UsageAuthorityReport::new(1_000, entries).unwrap_err().code,
            "usage-authority-metrics-incomplete"
        );

        let mut entries = valid_entries();
        entries[3].metric = UsageMetric::Token;
        assert_eq!(
            UsageAuthorityReport::new(1_000, entries).unwrap_err().code,
            "usage-authority-metric-duplicate"
        );

        let mut entries = valid_entries();
        entries[0].value = Some(UsageValue::Context(ContextUsageValue {
            used_tokens: Some(1),
            window_tokens: Some(2),
        }));
        assert_eq!(
            UsageAuthorityReport::new(1_000, entries).unwrap_err().code,
            "usage-authority-metric-mismatch"
        );
    }

    #[test]
    fn rejects_numeric_reasoning_identity_and_time_inconsistencies() {
        let mut entries = valid_entries();
        if let Some(UsageValue::Token(value)) = &mut entries[1].value {
            value.total_tokens = Some(99);
        }
        assert_eq!(
            UsageAuthorityReport::new(1_000, entries).unwrap_err().code,
            "usage-authority-token-total-invalid"
        );

        let mut entries = valid_entries();
        if let Some(UsageValue::Reasoning(value)) = &mut entries[0].value {
            value.output_tokens = Some(19);
        }
        assert_eq!(
            UsageAuthorityReport::new(1_000, entries).unwrap_err().code,
            "usage-authority-reasoning-token-mismatch"
        );

        let mut entries = valid_entries();
        if let AuthorityEvidence::Observed {
            evidence_identity,
            observed_at_ms,
            ..
        } = &mut entries[1].evidence
        {
            *evidence_identity = "provider-usage:sha256:ABC".into();
            *observed_at_ms = 1_001;
        }
        assert_eq!(
            UsageAuthorityReport::new(1_000, entries).unwrap_err().code,
            "usage-authority-identity-invalid"
        );

        let mut entries = valid_entries();
        if let AuthorityEvidence::Observed { observed_at_ms, .. } = &mut entries[1].evidence {
            *observed_at_ms = 1_001;
        }
        assert_eq!(
            UsageAuthorityReport::new(1_000, entries).unwrap_err().code,
            "usage-authority-evidence-future"
        );
    }

    #[test]
    fn provider_projection_is_observed_without_inventing_cost_or_rewriting_totals() {
        let usage = json!({
            "last": {
                "cached_input_tokens": 12,
                "input_tokens": 24,
                "output_tokens": 8,
                "reasoning_output_tokens": 4,
                "total_tokens": 48
            },
            "total": {
                "cached_input_tokens": 12,
                "input_tokens": 24,
                "output_tokens": 8,
                "reasoning_output_tokens": 4,
                "total_tokens": 48
            },
            "model_context_window": 128000
        });
        let report = from_provider_token_usage(&usage, 1_000).unwrap();
        let token = report.entry(UsageMetric::Token).unwrap();
        assert_eq!(token.authority, AuthorityLabel::Observed);
        let Some(UsageValue::Token(value)) = &token.value else {
            panic!("expected token value");
        };
        assert_eq!(value.input_tokens, Some(24));
        assert_eq!(value.output_tokens, Some(8));
        assert_eq!(value.total_tokens, None);
        assert_eq!(
            report.entry(UsageMetric::Context).unwrap().authority,
            AuthorityLabel::Observed
        );
        let Some(UsageValue::Context(context)) = &report.entry(UsageMetric::Context).unwrap().value
        else {
            panic!("expected context value");
        };
        assert_eq!(context.used_tokens, Some(24));
        assert_eq!(context.window_tokens, Some(128_000));
        assert_eq!(
            report.entry(UsageMetric::Cost).unwrap().authority,
            AuthorityLabel::Unknown
        );
        assert_eq!(
            report.entry(UsageMetric::Reasoning).unwrap().authority,
            AuthorityLabel::Observed
        );
        report.validate().unwrap();
        assert!(serde_json::to_string(&report)
            .unwrap()
            .contains("provider-usage:sha256:"));
    }

    #[test]
    fn provider_projection_rejects_missing_required_breakdown() {
        let error =
            from_provider_token_usage(&json!({"total": {"input_tokens": 1}}), 1).unwrap_err();
        assert_eq!(error.code, "usage-authority-provider-payload-invalid");
    }
}
