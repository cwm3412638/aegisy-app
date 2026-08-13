//! Durable, content-free consumption receipts for non-Turn mutation evidence.
//!
//! A receipt proves only that one exact internal evidence anchor was consumed in
//! order. It does not prove that a mutation succeeded, advertise an AAP method,
//! dispatch work, or grant permission, approval, mutation, or execution authority.

use crate::mutation_reservation::{
    MutationReservationDraft, MutationReservationKind, MutationReservationSource,
};
use crate::mutation_reservation_outcome::MutationReservationOutcome;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fmt;

pub(crate) const SCHEMA_VERSION: &str = "mutation-reservation-consumption-receipt/0.1";
const RECEIPT_IDENTITY_PREFIX: &str = "mutation-reservation-consumption-receipt:sha256:";
const RECONCILIATION_EVIDENCE_IDENTITY_PREFIX: &str =
    "mutation-reservation-reconciliation-evidence:sha256:";
const RESERVATION_IDENTITY_PREFIX: &str = "mutation-reservation-draft:sha256:";
const SOURCE_IDENTITY_PREFIX: &str = "mutation-reservation-source:sha256:";
const OUTCOME_IDENTITY_PREFIX: &str = "mutation-reservation-outcome:sha256:";
const MAX_IDENTIFIER_BYTES: usize = 128;
const MAX_RECEIPT_JSON_BYTES: usize = 16 * 1024;
const MAX_SAFE_JSON_INTEGER: u64 = 9_007_199_254_740_991;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct MutationReservationConsumptionError {
    pub code: &'static str,
    pub message: &'static str,
}

impl MutationReservationConsumptionError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

impl fmt::Display for MutationReservationConsumptionError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message)
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub(crate) enum MutationReservationConsumptionPhase {
    Source,
    Terminal,
    ReconciliationRequired,
}

impl MutationReservationConsumptionPhase {
    fn as_str(self) -> &'static str {
        match self {
            Self::Source => "source",
            Self::Terminal => "terminal",
            Self::ReconciliationRequired => "reconciliation-required",
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub(crate) struct MutationReservationConsumptionEventAnchor {
    pub event_sequence: u64,
    pub event_id: String,
    pub event_timestamp_ms: u64,
}

impl MutationReservationConsumptionEventAnchor {
    pub(crate) fn new(
        event_sequence: u64,
        event_id: impl Into<String>,
        event_timestamp_ms: u64,
    ) -> Result<Self, MutationReservationConsumptionError> {
        let anchor = Self {
            event_sequence,
            event_id: event_id.into(),
            event_timestamp_ms,
        };
        anchor.validate()?;
        Ok(anchor)
    }

    pub(crate) fn validate(&self) -> Result<(), MutationReservationConsumptionError> {
        if self.event_sequence == 0
            || self.event_sequence > MAX_SAFE_JSON_INTEGER
            || self.event_timestamp_ms == 0
            || self.event_timestamp_ms > MAX_SAFE_JSON_INTEGER
            || !valid_identifier(&self.event_id)
        {
            return Err(MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-anchor-invalid",
                "mutation reservation consumption event anchor is invalid",
            ));
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct MutationReservationConsumptionReceipt {
    pub schema_version: String,
    pub receipt_identity: String,
    pub reservation_identity: String,
    pub session_id: String,
    pub kind: MutationReservationKind,
    pub phase: MutationReservationConsumptionPhase,
    pub reservation_revision: u64,
    pub consumption_revision: u64,
    pub evidence_identity: String,
    pub event_anchor: MutationReservationConsumptionEventAnchor,
    pub previous_receipt_identity: Option<String>,
    pub consumed_at_ms: u64,
    pub dispatch_authority: bool,
    pub mutation_authority: bool,
    pub approval_authority: bool,
    pub execution_authority: bool,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct ReceiptWire {
    schema_version: String,
    receipt_identity: String,
    reservation_identity: String,
    session_id: String,
    kind: MutationReservationKind,
    phase: MutationReservationConsumptionPhase,
    reservation_revision: u64,
    consumption_revision: u64,
    evidence_identity: String,
    event_anchor: MutationReservationConsumptionEventAnchor,
    previous_receipt_identity: Option<String>,
    consumed_at_ms: u64,
    dispatch_authority: bool,
    mutation_authority: bool,
    approval_authority: bool,
    execution_authority: bool,
}

impl MutationReservationConsumptionReceipt {
    pub(crate) fn source(
        draft: &MutationReservationDraft,
        source: &MutationReservationSource,
        reservation_revision: u64,
        event_anchor: MutationReservationConsumptionEventAnchor,
        consumed_at_ms: u64,
    ) -> Result<Self, MutationReservationConsumptionError> {
        validate_source_binding(draft, source)?;
        if !matches!(reservation_revision, 1 | 2) {
            return Err(MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-reservation-revision-invalid",
                "source evidence may be consumed only at reservation revision one or two",
            ));
        }
        let evidence_identity = source.source_identity().map_err(|_| {
            MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-source-invalid",
                "mutation reservation consumption source identity is invalid",
            )
        })?;
        let receipt = Self::build(
            draft,
            MutationReservationConsumptionPhase::Source,
            reservation_revision,
            1,
            evidence_identity,
            event_anchor,
            None,
            consumed_at_ms,
        )?;
        receipt.validate_for_graph(draft, source, None, None)?;
        Ok(receipt)
    }

    pub(crate) fn terminal(
        draft: &MutationReservationDraft,
        source: &MutationReservationSource,
        outcome: &MutationReservationOutcome,
        event_anchor: MutationReservationConsumptionEventAnchor,
        previous: &Self,
        consumed_at_ms: u64,
    ) -> Result<Self, MutationReservationConsumptionError> {
        validate_source_binding(draft, source)?;
        let evidence_identity = outcome.outcome_identity(source).map_err(|_| {
            MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-outcome-invalid",
                "mutation reservation consumption terminal outcome is invalid",
            )
        })?;
        let receipt = Self::build(
            draft,
            MutationReservationConsumptionPhase::Terminal,
            2,
            2,
            evidence_identity,
            event_anchor,
            Some(previous.receipt_identity.clone()),
            consumed_at_ms,
        )?;
        receipt.validate_for_graph(draft, source, Some(outcome), Some(previous))?;
        Ok(receipt)
    }

    pub(crate) fn reconciliation_required(
        draft: &MutationReservationDraft,
        source: &MutationReservationSource,
        evidence_identity: impl Into<String>,
        event_anchor: MutationReservationConsumptionEventAnchor,
        previous: &Self,
        consumed_at_ms: u64,
    ) -> Result<Self, MutationReservationConsumptionError> {
        validate_source_binding(draft, source)?;
        let receipt = Self::build(
            draft,
            MutationReservationConsumptionPhase::ReconciliationRequired,
            2,
            2,
            evidence_identity.into(),
            event_anchor,
            Some(previous.receipt_identity.clone()),
            consumed_at_ms,
        )?;
        receipt.validate_for_graph(draft, source, None, Some(previous))?;
        Ok(receipt)
    }

    #[allow(clippy::too_many_arguments)]
    fn build(
        draft: &MutationReservationDraft,
        phase: MutationReservationConsumptionPhase,
        reservation_revision: u64,
        consumption_revision: u64,
        evidence_identity: String,
        event_anchor: MutationReservationConsumptionEventAnchor,
        previous_receipt_identity: Option<String>,
        consumed_at_ms: u64,
    ) -> Result<Self, MutationReservationConsumptionError> {
        let mut receipt = Self {
            schema_version: SCHEMA_VERSION.into(),
            receipt_identity: String::new(),
            reservation_identity: draft.reservation_identity.clone(),
            session_id: draft.session_id.clone(),
            kind: draft.kind,
            phase,
            reservation_revision,
            consumption_revision,
            evidence_identity,
            event_anchor,
            previous_receipt_identity,
            consumed_at_ms,
            dispatch_authority: false,
            mutation_authority: false,
            approval_authority: false,
            execution_authority: false,
        };
        receipt.receipt_identity = receipt.derived_identity();
        receipt.validate()?;
        Ok(receipt)
    }

    pub(crate) fn validate(&self) -> Result<(), MutationReservationConsumptionError> {
        self.event_anchor.validate()?;
        if self.schema_version != SCHEMA_VERSION
            || !valid_sha256_identity(&self.receipt_identity, RECEIPT_IDENTITY_PREFIX)
            || self.receipt_identity != self.derived_identity()
            || !valid_sha256_identity(&self.reservation_identity, RESERVATION_IDENTITY_PREFIX)
            || !valid_identifier(&self.session_id)
            || self.reservation_revision == 0
            || self.reservation_revision > MAX_SAFE_JSON_INTEGER
            || self.consumption_revision == 0
            || self.consumption_revision > MAX_SAFE_JSON_INTEGER
            || self.consumed_at_ms == 0
            || self.consumed_at_ms > MAX_SAFE_JSON_INTEGER
            || self.event_anchor.event_timestamp_ms > self.consumed_at_ms
            || self.dispatch_authority
            || self.mutation_authority
            || self.approval_authority
            || self.execution_authority
        {
            return Err(MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-receipt-invalid",
                "mutation reservation consumption receipt metadata or authority is invalid",
            ));
        }

        let phase_valid = match self.phase {
            MutationReservationConsumptionPhase::Source => {
                matches!(self.reservation_revision, 1 | 2)
                    && self.consumption_revision == 1
                    && valid_sha256_identity(&self.evidence_identity, SOURCE_IDENTITY_PREFIX)
                    && self.previous_receipt_identity.is_none()
            }
            MutationReservationConsumptionPhase::Terminal => {
                self.reservation_revision == 2
                    && self.consumption_revision == 2
                    && valid_sha256_identity(&self.evidence_identity, OUTCOME_IDENTITY_PREFIX)
                    && self
                        .previous_receipt_identity
                        .as_deref()
                        .is_some_and(|identity| {
                            valid_sha256_identity(identity, RECEIPT_IDENTITY_PREFIX)
                        })
            }
            MutationReservationConsumptionPhase::ReconciliationRequired => {
                self.reservation_revision == 2
                    && self.consumption_revision == 2
                    && valid_sha256_identity(
                        &self.evidence_identity,
                        RECONCILIATION_EVIDENCE_IDENTITY_PREFIX,
                    )
                    && self
                        .previous_receipt_identity
                        .as_deref()
                        .is_some_and(|identity| {
                            valid_sha256_identity(identity, RECEIPT_IDENTITY_PREFIX)
                        })
            }
        };
        if !phase_valid {
            return Err(MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-phase-invalid",
                "mutation reservation evidence consumption phase or revision is invalid",
            ));
        }
        Ok(())
    }

    pub(crate) fn validate_for_graph(
        &self,
        draft: &MutationReservationDraft,
        source: &MutationReservationSource,
        outcome: Option<&MutationReservationOutcome>,
        previous: Option<&Self>,
    ) -> Result<(), MutationReservationConsumptionError> {
        self.validate()?;
        validate_source_binding(draft, source)?;
        if self.reservation_identity != draft.reservation_identity
            || self.session_id != draft.session_id
            || self.kind != draft.kind
        {
            return Err(MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-graph-mismatch",
                "mutation reservation consumption receipt does not match its graph",
            ));
        }

        match self.phase {
            MutationReservationConsumptionPhase::Source => {
                if outcome.is_some()
                    || previous.is_some()
                    || self.evidence_identity
                        != source.source_identity().map_err(|_| {
                            MutationReservationConsumptionError::new(
                                "mutation-reservation-consumption-source-invalid",
                                "mutation reservation consumption source identity is invalid",
                            )
                        })?
                {
                    return Err(MutationReservationConsumptionError::new(
                        "mutation-reservation-consumption-source-mismatch",
                        "source consumption receipt does not bind the exact source evidence",
                    ));
                }
            }
            MutationReservationConsumptionPhase::Terminal => {
                let outcome = outcome.ok_or_else(|| {
                    MutationReservationConsumptionError::new(
                        "mutation-reservation-consumption-outcome-missing",
                        "terminal consumption requires its exact terminal outcome",
                    )
                })?;
                if self.evidence_identity
                    != outcome.outcome_identity(source).map_err(|_| {
                        MutationReservationConsumptionError::new(
                            "mutation-reservation-consumption-outcome-invalid",
                            "mutation reservation consumption terminal outcome is invalid",
                        )
                    })?
                {
                    return Err(MutationReservationConsumptionError::new(
                        "mutation-reservation-consumption-outcome-mismatch",
                        "terminal consumption receipt does not bind the exact outcome evidence",
                    ));
                }
                self.validate_previous_source(draft, source, previous)?;
            }
            MutationReservationConsumptionPhase::ReconciliationRequired => {
                if outcome.is_some()
                    || self.evidence_identity
                        != reconciliation_evidence_identity(draft, source, &self.event_anchor)?
                {
                    return Err(MutationReservationConsumptionError::new(
                        "mutation-reservation-consumption-reconciliation-mismatch",
                        "reconciliation consumption receipt does not bind the exact evidence",
                    ));
                }
                self.validate_previous_source(draft, source, previous)?;
            }
        }
        Ok(())
    }

    fn validate_previous_source(
        &self,
        draft: &MutationReservationDraft,
        source: &MutationReservationSource,
        previous: Option<&Self>,
    ) -> Result<(), MutationReservationConsumptionError> {
        let previous = previous.ok_or_else(|| {
            MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-source-first",
                "source evidence must be consumed before resolution evidence",
            )
        })?;
        previous.validate()?;
        if previous.phase != MutationReservationConsumptionPhase::Source {
            return Err(MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-source-first",
                "source evidence must be consumed before resolution evidence",
            ));
        }
        previous.validate_for_graph(draft, source, None, None)?;
        let ordered = self.previous_receipt_identity.as_deref()
            == Some(previous.receipt_identity.as_str())
            && previous.consumption_revision == 1
            && previous.event_anchor.event_sequence < self.event_anchor.event_sequence
            && previous.event_anchor.event_timestamp_ms <= self.event_anchor.event_timestamp_ms
            && previous.consumed_at_ms <= self.consumed_at_ms;
        if !ordered {
            return Err(MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-order-invalid",
                "mutation reservation consumption receipts are not in source-first order",
            ));
        }
        Ok(())
    }

    pub(crate) fn canonical_bytes(&self) -> Result<Vec<u8>, MutationReservationConsumptionError> {
        self.validate()?;
        let bytes = serde_json::to_vec(self).map_err(|_| {
            MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-serialize-failed",
                "mutation reservation consumption receipt could not be serialized",
            )
        })?;
        if bytes.is_empty() || bytes.len() > MAX_RECEIPT_JSON_BYTES {
            return Err(MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-size-exceeded",
                "mutation reservation consumption receipt exceeds its JSON byte bound",
            ));
        }
        let decoded = Self::from_canonical_bytes(&bytes)?;
        if decoded != *self {
            return Err(MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-round-trip-invalid",
                "mutation reservation consumption receipt changed during serialization",
            ));
        }
        Ok(bytes)
    }

    pub(crate) fn from_canonical_bytes(
        bytes: &[u8],
    ) -> Result<Self, MutationReservationConsumptionError> {
        if bytes.is_empty() || bytes.len() > MAX_RECEIPT_JSON_BYTES {
            return Err(MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-size-exceeded",
                "mutation reservation consumption receipt exceeds its JSON byte bound",
            ));
        }
        let receipt: Self = serde_json::from_slice(bytes).map_err(|_| {
            MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-json-invalid",
                "mutation reservation consumption receipt JSON is invalid",
            )
        })?;
        receipt.validate()?;
        let canonical = serde_json::to_vec(&receipt).map_err(|_| {
            MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-serialize-failed",
                "mutation reservation consumption receipt could not be re-encoded",
            )
        })?;
        if canonical != bytes {
            return Err(MutationReservationConsumptionError::new(
                "mutation-reservation-consumption-canonical-invalid",
                "mutation reservation consumption receipt JSON is not canonical",
            ));
        }
        Ok(receipt)
    }

    pub(crate) fn canonical_sha256(&self) -> Result<String, MutationReservationConsumptionError> {
        Ok(format!("{:x}", Sha256::digest(self.canonical_bytes()?)))
    }

    fn derived_identity(&self) -> String {
        let mut digest = Sha256::new();
        digest.update(b"aegisy-mutation-reservation-consumption-receipt/0.1\0");
        hash_string(&mut digest, &self.schema_version);
        hash_string(&mut digest, &self.reservation_identity);
        hash_string(&mut digest, &self.session_id);
        hash_string(&mut digest, kind_name(self.kind));
        hash_string(&mut digest, self.phase.as_str());
        hash_u64(&mut digest, self.reservation_revision);
        hash_u64(&mut digest, self.consumption_revision);
        hash_string(&mut digest, &self.evidence_identity);
        hash_u64(&mut digest, self.event_anchor.event_sequence);
        hash_string(&mut digest, &self.event_anchor.event_id);
        hash_u64(&mut digest, self.event_anchor.event_timestamp_ms);
        hash_optional_string(&mut digest, self.previous_receipt_identity.as_deref());
        hash_u64(&mut digest, self.consumed_at_ms);
        for authority in [
            self.dispatch_authority,
            self.mutation_authority,
            self.approval_authority,
            self.execution_authority,
        ] {
            hash_bytes(&mut digest, &[u8::from(authority)]);
        }
        format!("{RECEIPT_IDENTITY_PREFIX}{:x}", digest.finalize())
    }
}

pub(crate) fn reconciliation_evidence_identity(
    draft: &MutationReservationDraft,
    source: &MutationReservationSource,
    event_anchor: &MutationReservationConsumptionEventAnchor,
) -> Result<String, MutationReservationConsumptionError> {
    validate_source_binding(draft, source)?;
    event_anchor.validate()?;
    let source_identity = source.source_identity().map_err(|_| {
        MutationReservationConsumptionError::new(
            "mutation-reservation-consumption-source-invalid",
            "mutation reservation consumption source identity is invalid",
        )
    })?;
    let mut digest = Sha256::new();
    digest.update(b"aegisy-mutation-reservation-reconciliation-evidence/0.1\0");
    hash_string(&mut digest, &draft.reservation_identity);
    hash_string(&mut digest, &draft.session_id);
    hash_string(&mut digest, kind_name(draft.kind));
    hash_string(&mut digest, &source_identity);
    hash_u64(&mut digest, 2);
    hash_u64(&mut digest, event_anchor.event_sequence);
    hash_string(&mut digest, &event_anchor.event_id);
    hash_u64(&mut digest, event_anchor.event_timestamp_ms);
    Ok(format!(
        "{RECONCILIATION_EVIDENCE_IDENTITY_PREFIX}{:x}",
        digest.finalize()
    ))
}

fn validate_source_binding(
    draft: &MutationReservationDraft,
    source: &MutationReservationSource,
) -> Result<(), MutationReservationConsumptionError> {
    draft.validate().map_err(|_| {
        MutationReservationConsumptionError::new(
            "mutation-reservation-consumption-reservation-invalid",
            "mutation reservation consumption draft is invalid",
        )
    })?;
    source.validate().map_err(|_| {
        MutationReservationConsumptionError::new(
            "mutation-reservation-consumption-source-invalid",
            "mutation reservation consumption source is invalid",
        )
    })?;
    let derived = source.to_draft().map_err(|_| {
        MutationReservationConsumptionError::new(
            "mutation-reservation-consumption-source-invalid",
            "mutation reservation consumption source is invalid",
        )
    })?;
    if derived != *draft {
        return Err(MutationReservationConsumptionError::new(
            "mutation-reservation-consumption-source-mismatch",
            "mutation reservation consumption source does not derive the reservation",
        ));
    }
    Ok(())
}

fn kind_name(kind: MutationReservationKind) -> &'static str {
    match kind {
        MutationReservationKind::Approval => "approval",
        MutationReservationKind::FileWrite => "file-write",
        MutationReservationKind::GitMutation => "git-mutation",
        MutationReservationKind::JobSubmission => "job-submission",
    }
}

fn valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= MAX_IDENTIFIER_BYTES
        && value.bytes().all(|byte| byte.is_ascii_graphic())
        && !secret_shaped(value)
}

fn valid_sha256_identity(value: &str, prefix: &str) -> bool {
    value.len() == prefix.len() + 64
        && value.starts_with(prefix)
        && value[prefix.len()..]
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn secret_shaped(value: &str) -> bool {
    let lowercase = value.to_ascii_lowercase();
    [
        "api_key",
        "api-key",
        "apikey",
        "access_token",
        "access-token",
        "authorization",
        "bearer",
        "client_secret",
        "client-secret",
        "cookie",
        "credential",
        "password",
        "private_key",
        "private-key",
        "refresh_token",
        "refresh-token",
        "secret",
    ]
    .iter()
    .any(|marker| lowercase.contains(marker))
        || value.split('.').count() == 3
            && value.split('.').all(|segment| {
                segment.len() >= 8
                    && segment
                        .bytes()
                        .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
            })
        || value
            .split(|character: char| {
                !character.is_ascii_alphanumeric() && !matches!(character, '_' | '-' | '.')
            })
            .any(|token| {
                (token.starts_with("sk-") && token.len() >= 20)
                    || (token.starts_with("ghp_") && token.len() >= 20)
                    || (token.starts_with("github_pat_") && token.len() >= 24)
            })
}

fn hash_bytes(digest: &mut Sha256, value: &[u8]) {
    digest.update((value.len() as u64).to_be_bytes());
    digest.update(value);
}

fn hash_string(digest: &mut Sha256, value: &str) {
    hash_bytes(digest, value.as_bytes());
}

fn hash_u64(digest: &mut Sha256, value: u64) {
    hash_bytes(digest, &value.to_be_bytes());
}

fn hash_optional_string(digest: &mut Sha256, value: Option<&str>) {
    match value {
        Some(value) => {
            hash_bytes(digest, &[1]);
            hash_string(digest, value);
        }
        None => hash_bytes(digest, &[0]),
    }
}

impl Serialize for MutationReservationConsumptionReceipt {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        self.validate().map_err(serde::ser::Error::custom)?;
        #[derive(Serialize)]
        struct Wire<'a> {
            schema_version: &'a str,
            receipt_identity: &'a str,
            reservation_identity: &'a str,
            session_id: &'a str,
            kind: MutationReservationKind,
            phase: MutationReservationConsumptionPhase,
            reservation_revision: u64,
            consumption_revision: u64,
            evidence_identity: &'a str,
            event_anchor: &'a MutationReservationConsumptionEventAnchor,
            previous_receipt_identity: Option<&'a str>,
            consumed_at_ms: u64,
            dispatch_authority: bool,
            mutation_authority: bool,
            approval_authority: bool,
            execution_authority: bool,
        }
        Wire {
            schema_version: &self.schema_version,
            receipt_identity: &self.receipt_identity,
            reservation_identity: &self.reservation_identity,
            session_id: &self.session_id,
            kind: self.kind,
            phase: self.phase,
            reservation_revision: self.reservation_revision,
            consumption_revision: self.consumption_revision,
            evidence_identity: &self.evidence_identity,
            event_anchor: &self.event_anchor,
            previous_receipt_identity: self.previous_receipt_identity.as_deref(),
            consumed_at_ms: self.consumed_at_ms,
            dispatch_authority: false,
            mutation_authority: false,
            approval_authority: false,
            execution_authority: false,
        }
        .serialize(serializer)
    }
}

impl<'de> Deserialize<'de> for MutationReservationConsumptionReceipt {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let wire = ReceiptWire::deserialize(deserializer)?;
        let receipt = Self {
            schema_version: wire.schema_version,
            receipt_identity: wire.receipt_identity,
            reservation_identity: wire.reservation_identity,
            session_id: wire.session_id,
            kind: wire.kind,
            phase: wire.phase,
            reservation_revision: wire.reservation_revision,
            consumption_revision: wire.consumption_revision,
            evidence_identity: wire.evidence_identity,
            event_anchor: wire.event_anchor,
            previous_receipt_identity: wire.previous_receipt_identity,
            consumed_at_ms: wire.consumed_at_ms,
            dispatch_authority: wire.dispatch_authority,
            mutation_authority: wire.mutation_authority,
            approval_authority: wire.approval_authority,
            execution_authority: wire.execution_authority,
        };
        receipt.validate().map_err(serde::de::Error::custom)?;
        Ok(receipt)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::approval_ack::{ApprovalRequest, Resolution, Scope, State};
    use serde_json::Value;

    fn fingerprint(byte: char) -> String {
        format!("request:sha256:{}", byte.to_string().repeat(64))
    }

    fn observation(byte: char) -> String {
        format!(
            "approval-observation:sha256:{}",
            byte.to_string().repeat(64)
        )
    }

    fn graph() -> (
        MutationReservationSource,
        MutationReservationDraft,
        MutationReservationOutcome,
    ) {
        let request = ApprovalRequest::new(
            "session-consume-1",
            "turn-consume-1",
            "retry-consume-1",
            fingerprint('1'),
            Scope::FileChange,
        )
        .unwrap();
        let terminal = request
            .acknowledgement(
                State::Resolved,
                Resolution::Denied,
                2,
                Some(observation('a')),
                20,
            )
            .unwrap();
        let source = MutationReservationSource::from_approval(request).unwrap();
        let draft = source.to_draft().unwrap();
        let outcome = MutationReservationOutcome::from_approval(terminal).unwrap();
        (source, draft, outcome)
    }

    fn anchor(
        sequence: u64,
        event_id: &str,
        timestamp: u64,
    ) -> MutationReservationConsumptionEventAnchor {
        MutationReservationConsumptionEventAnchor::new(sequence, event_id, timestamp).unwrap()
    }

    fn source_receipt(
        source: &MutationReservationSource,
        draft: &MutationReservationDraft,
        reservation_revision: u64,
    ) -> MutationReservationConsumptionReceipt {
        MutationReservationConsumptionReceipt::source(
            draft,
            source,
            reservation_revision,
            anchor(
                4,
                "mutation-reservation-source-recorded-0123456789abcdef",
                10,
            ),
            30,
        )
        .unwrap()
    }

    fn assert_identity_binds(
        receipt: &MutationReservationConsumptionReceipt,
        field: &str,
        mutate: impl FnOnce(&mut MutationReservationConsumptionReceipt),
    ) {
        assert_eq!(receipt.receipt_identity, receipt.derived_identity());
        let mut changed = receipt.clone();
        mutate(&mut changed);
        assert_ne!(
            changed.derived_identity(),
            receipt.receipt_identity,
            "receipt identity did not bind {field}"
        );
    }

    #[test]
    fn source_is_consumption_one_without_previous_at_reservation_one_or_two() {
        let (source, draft, _) = graph();
        let at_reserved = source_receipt(&source, &draft, 1);
        let after_resolution = source_receipt(&source, &draft, 2);
        for receipt in [&at_reserved, &after_resolution] {
            assert_eq!(receipt.phase, MutationReservationConsumptionPhase::Source);
            assert_eq!(receipt.consumption_revision, 1);
            assert!(receipt.previous_receipt_identity.is_none());
            receipt
                .validate_for_graph(&draft, &source, None, None)
                .unwrap();
        }
        assert_ne!(
            at_reserved.receipt_identity,
            after_resolution.receipt_identity
        );
        for invalid_revision in [0, 3] {
            let error = MutationReservationConsumptionReceipt::source(
                &draft,
                &source,
                invalid_revision,
                anchor(
                    4,
                    "mutation-reservation-source-recorded-0123456789abcdef",
                    10,
                ),
                30,
            )
            .unwrap_err();
            assert_eq!(
                error.code,
                "mutation-reservation-consumption-reservation-revision-invalid"
            );
        }
    }

    #[test]
    fn terminal_consumption_two_requires_and_binds_the_source_receipt() {
        let (source, draft, outcome) = graph();
        let source_receipt = source_receipt(&source, &draft, 1);
        let terminal = MutationReservationConsumptionReceipt::terminal(
            &draft,
            &source,
            &outcome,
            anchor(
                9,
                "mutation-reservation-outcome-recorded-fedcba9876543210",
                21,
            ),
            &source_receipt,
            31,
        )
        .unwrap();
        assert_eq!(
            terminal.phase,
            MutationReservationConsumptionPhase::Terminal
        );
        assert_eq!(terminal.reservation_revision, 2);
        assert_eq!(terminal.consumption_revision, 2);
        assert_eq!(
            terminal.previous_receipt_identity.as_deref(),
            Some(source_receipt.receipt_identity.as_str())
        );
        terminal
            .validate_for_graph(&draft, &source, Some(&outcome), Some(&source_receipt))
            .unwrap();
    }

    #[test]
    fn reconciliation_consumption_two_binds_exact_event_evidence_after_source() {
        let (source, draft, _) = graph();
        let source_receipt = source_receipt(&source, &draft, 2);
        let reconciliation_anchor =
            anchor(7, "mutation-reservation-reconciled-0011223344556677", 15);
        let evidence =
            reconciliation_evidence_identity(&draft, &source, &reconciliation_anchor).unwrap();
        let receipt = MutationReservationConsumptionReceipt::reconciliation_required(
            &draft,
            &source,
            evidence,
            reconciliation_anchor,
            &source_receipt,
            32,
        )
        .unwrap();
        receipt
            .validate_for_graph(&draft, &source, None, Some(&source_receipt))
            .unwrap();
        assert_eq!(
            receipt.phase,
            MutationReservationConsumptionPhase::ReconciliationRequired
        );
    }

    #[test]
    fn resolution_before_source_or_with_non_source_previous_fails_closed() {
        let (source, draft, outcome) = graph();
        let source_receipt = source_receipt(&source, &draft, 2);
        let terminal = MutationReservationConsumptionReceipt::terminal(
            &draft,
            &source,
            &outcome,
            anchor(
                9,
                "mutation-reservation-outcome-recorded-fedcba9876543210",
                21,
            ),
            &source_receipt,
            31,
        )
        .unwrap();
        let mut missing = terminal.clone();
        missing.previous_receipt_identity = None;
        missing.receipt_identity = missing.derived_identity();
        assert_eq!(
            missing.validate().unwrap_err().code,
            "mutation-reservation-consumption-phase-invalid"
        );

        let result = terminal.validate_for_graph(&draft, &source, Some(&outcome), None);
        assert_eq!(
            result.unwrap_err().code,
            "mutation-reservation-consumption-source-first"
        );

        let result = terminal.validate_for_graph(&draft, &source, Some(&outcome), Some(&terminal));
        assert_eq!(
            result.unwrap_err().code,
            "mutation-reservation-consumption-source-first"
        );
    }

    fn assert_resolution_order_drift_fails(
        receipt: &MutationReservationConsumptionReceipt,
        draft: &MutationReservationDraft,
        source: &MutationReservationSource,
        outcome: Option<&MutationReservationOutcome>,
        previous: &MutationReservationConsumptionReceipt,
        mutate: impl FnOnce(
            &mut MutationReservationConsumptionReceipt,
            &mut MutationReservationConsumptionReceipt,
        ),
    ) {
        let mut changed_previous = previous.clone();
        let mut changed_receipt = receipt.clone();
        mutate(&mut changed_receipt, &mut changed_previous);
        changed_previous.receipt_identity = changed_previous.derived_identity();
        changed_receipt.previous_receipt_identity = Some(changed_previous.receipt_identity.clone());
        changed_receipt.receipt_identity = changed_receipt.derived_identity();
        assert_eq!(
            changed_receipt
                .validate_for_graph(draft, source, outcome, Some(&changed_previous))
                .unwrap_err()
                .code,
            "mutation-reservation-consumption-order-invalid"
        );
    }

    #[test]
    fn terminal_and_reconciliation_reject_hash_consistent_ordering_drift() {
        let (source, draft, outcome) = graph();
        let previous = source_receipt(&source, &draft, 1);
        let terminal = MutationReservationConsumptionReceipt::terminal(
            &draft,
            &source,
            &outcome,
            anchor(
                9,
                "mutation-reservation-outcome-recorded-fedcba9876543210",
                21,
            ),
            &previous,
            31,
        )
        .unwrap();
        let reconciliation_anchor =
            anchor(7, "mutation-reservation-reconciled-0011223344556677", 15);
        let reconciliation = MutationReservationConsumptionReceipt::reconciliation_required(
            &draft,
            &source,
            reconciliation_evidence_identity(&draft, &source, &reconciliation_anchor).unwrap(),
            reconciliation_anchor,
            &previous,
            32,
        )
        .unwrap();

        for (receipt, outcome) in [(&terminal, Some(&outcome)), (&reconciliation, None)] {
            let mut wrong_previous = receipt.clone();
            wrong_previous.previous_receipt_identity =
                Some(format!("{RECEIPT_IDENTITY_PREFIX}{}", "b".repeat(64)));
            wrong_previous.receipt_identity = wrong_previous.derived_identity();
            assert_eq!(
                wrong_previous
                    .validate_for_graph(&draft, &source, outcome, Some(&previous))
                    .unwrap_err()
                    .code,
                "mutation-reservation-consumption-order-invalid"
            );

            assert_resolution_order_drift_fails(
                receipt,
                &draft,
                &source,
                outcome,
                &previous,
                |changed, changed_previous| {
                    changed_previous.event_anchor.event_sequence =
                        changed.event_anchor.event_sequence;
                },
            );
            assert_resolution_order_drift_fails(
                receipt,
                &draft,
                &source,
                outcome,
                &previous,
                |changed, changed_previous| {
                    changed_previous.event_anchor.event_sequence =
                        changed.event_anchor.event_sequence + 1;
                },
            );
            assert_resolution_order_drift_fails(
                receipt,
                &draft,
                &source,
                outcome,
                &previous,
                |changed, changed_previous| {
                    changed_previous.event_anchor.event_timestamp_ms =
                        changed.event_anchor.event_timestamp_ms + 1;
                },
            );
            assert_resolution_order_drift_fails(
                receipt,
                &draft,
                &source,
                outcome,
                &previous,
                |changed, changed_previous| {
                    changed_previous.consumed_at_ms = changed.consumed_at_ms + 1;
                },
            );
        }
    }

    #[test]
    fn canonical_round_trip_and_identity_bind_every_ordering_field() {
        let (source, draft, _) = graph();
        let receipt = source_receipt(&source, &draft, 2);
        let bytes = receipt.canonical_bytes().unwrap();
        assert_eq!(
            MutationReservationConsumptionReceipt::from_canonical_bytes(&bytes).unwrap(),
            receipt
        );
        assert_eq!(receipt.canonical_sha256().unwrap().len(), 64);

        let mut whitespace = bytes.clone();
        whitespace.push(b' ');
        assert_eq!(
            MutationReservationConsumptionReceipt::from_canonical_bytes(&whitespace)
                .unwrap_err()
                .code,
            "mutation-reservation-consumption-canonical-invalid"
        );

        let mut drift = receipt.clone();
        drift.consumed_at_ms += 1;
        assert_eq!(
            drift.validate().unwrap_err().code,
            "mutation-reservation-consumption-receipt-invalid"
        );
        drift.receipt_identity = drift.derived_identity();
        assert_ne!(drift.receipt_identity, receipt.receipt_identity);
        drift.validate().unwrap();
    }

    #[test]
    fn receipt_identity_binds_every_receipt_field() {
        let (source, draft, outcome) = graph();
        let source_receipt = source_receipt(&source, &draft, 1);
        let receipt = MutationReservationConsumptionReceipt::terminal(
            &draft,
            &source,
            &outcome,
            anchor(
                9,
                "mutation-reservation-outcome-recorded-fedcba9876543210",
                21,
            ),
            &source_receipt,
            31,
        )
        .unwrap();

        assert_identity_binds(&receipt, "schema_version", |changed| {
            changed.schema_version.push('x');
        });
        assert_identity_binds(&receipt, "reservation_identity", |changed| {
            changed.reservation_identity =
                format!("{RESERVATION_IDENTITY_PREFIX}{}", "b".repeat(64));
        });
        assert_identity_binds(&receipt, "session_id", |changed| {
            changed.session_id.push('x');
        });
        assert_identity_binds(&receipt, "kind", |changed| {
            changed.kind = MutationReservationKind::FileWrite;
        });
        assert_identity_binds(&receipt, "phase", |changed| {
            changed.phase = MutationReservationConsumptionPhase::ReconciliationRequired;
        });
        assert_identity_binds(&receipt, "reservation_revision", |changed| {
            changed.reservation_revision += 1;
        });
        assert_identity_binds(&receipt, "consumption_revision", |changed| {
            changed.consumption_revision += 1;
        });
        assert_identity_binds(&receipt, "evidence_identity", |changed| {
            changed.evidence_identity = format!("{OUTCOME_IDENTITY_PREFIX}{}", "b".repeat(64));
        });
        assert_identity_binds(&receipt, "event_anchor.event_sequence", |changed| {
            changed.event_anchor.event_sequence += 1;
        });
        assert_identity_binds(&receipt, "event_anchor.event_id", |changed| {
            changed.event_anchor.event_id.push('x');
        });
        assert_identity_binds(&receipt, "event_anchor.event_timestamp_ms", |changed| {
            changed.event_anchor.event_timestamp_ms += 1;
        });
        assert_identity_binds(&receipt, "previous_receipt_identity value", |changed| {
            changed.previous_receipt_identity =
                Some(format!("{RECEIPT_IDENTITY_PREFIX}{}", "b".repeat(64)));
        });
        assert_identity_binds(&receipt, "previous_receipt_identity presence", |changed| {
            changed.previous_receipt_identity = None;
        });
        assert_identity_binds(&receipt, "consumed_at_ms", |changed| {
            changed.consumed_at_ms += 1;
        });
        assert_identity_binds(&receipt, "dispatch_authority", |changed| {
            changed.dispatch_authority = true;
        });
        assert_identity_binds(&receipt, "mutation_authority", |changed| {
            changed.mutation_authority = true;
        });
        assert_identity_binds(&receipt, "approval_authority", |changed| {
            changed.approval_authority = true;
        });
        assert_identity_binds(&receipt, "execution_authority", |changed| {
            changed.execution_authority = true;
        });
    }

    #[test]
    fn strict_json_bounds_unknown_fields_and_authority_fail_closed() {
        let (source, draft, _) = graph();
        let receipt = source_receipt(&source, &draft, 1);
        let mut value = serde_json::to_value(&receipt).unwrap();
        value["unknown"] = Value::Bool(false);
        assert!(serde_json::from_value::<MutationReservationConsumptionReceipt>(value).is_err());

        for field in [
            "dispatch_authority",
            "mutation_authority",
            "approval_authority",
            "execution_authority",
        ] {
            let mut forged = receipt.clone();
            match field {
                "dispatch_authority" => forged.dispatch_authority = true,
                "mutation_authority" => forged.mutation_authority = true,
                "approval_authority" => forged.approval_authority = true,
                "execution_authority" => forged.execution_authority = true,
                _ => unreachable!(),
            }
            forged.receipt_identity = forged.derived_identity();
            assert_eq!(forged.receipt_identity, forged.derived_identity());
            assert_eq!(
                forged.validate().unwrap_err().code,
                "mutation-reservation-consumption-receipt-invalid"
            );

            let mut value = serde_json::to_value(&receipt).unwrap();
            value[field] = Value::Bool(true);
            value["receipt_identity"] = Value::String(forged.receipt_identity);
            assert!(
                serde_json::from_value::<MutationReservationConsumptionReceipt>(value).is_err()
            );
        }

        let mut forged = receipt.clone();
        forged.consumed_at_ms = MAX_SAFE_JSON_INTEGER + 1;
        forged.receipt_identity = forged.derived_identity();
        assert_eq!(forged.receipt_identity, forged.derived_identity());
        assert_eq!(
            forged.validate().unwrap_err().code,
            "mutation-reservation-consumption-receipt-invalid"
        );
        let mut value = serde_json::to_value(&receipt).unwrap();
        value["consumed_at_ms"] = Value::from(MAX_SAFE_JSON_INTEGER + 1);
        value["receipt_identity"] = Value::String(forged.receipt_identity);
        assert!(serde_json::from_value::<MutationReservationConsumptionReceipt>(value).is_err());
        assert!(
            MutationReservationConsumptionEventAnchor::new(1, "event-access_token-secret", 1)
                .is_err()
        );
        assert!(
            MutationReservationConsumptionReceipt::from_canonical_bytes(&vec![
                b'x';
                MAX_RECEIPT_JSON_BYTES
                    + 1
            ])
            .is_err()
        );
    }

    #[test]
    fn receipt_confirms_evidence_without_claiming_success_or_result() {
        let (source, draft, outcome) = graph();
        let source_receipt = source_receipt(&source, &draft, 2);
        let terminal = MutationReservationConsumptionReceipt::terminal(
            &draft,
            &source,
            &outcome,
            anchor(
                9,
                "mutation-reservation-outcome-recorded-fedcba9876543210",
                21,
            ),
            &source_receipt,
            31,
        )
        .unwrap();
        let value = serde_json::to_value(terminal).unwrap();
        for forbidden in [
            "success",
            "succeeded",
            "result",
            "outcome_state",
            "permission",
            "decision",
            "content",
            "prompt",
            "path",
            "command",
            "provider_body",
        ] {
            assert!(value.get(forbidden).is_none());
        }
        assert_eq!(value["dispatch_authority"], false);
        assert_eq!(value["mutation_authority"], false);
        assert_eq!(value["approval_authority"], false);
        assert_eq!(value["execution_authority"], false);
    }
}
