//! Content-free context threshold decision contract.
//!
//! This module answers one narrow question: does the currently observed
//! context state require a review before a later compaction workflow may be
//! considered? It does not start a model, create a checkpoint, mutate a
//! session, or grant any execution or permission authority.

use serde::{Deserialize, Serialize};

pub const SCHEMA_VERSION: &str = "context-threshold/0.1";
const BASIS_POINTS: u16 = 10_000;

/// Whether the accounting value is directly observed or a conservative bound.
#[derive(Debug, Clone, Copy, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum EvidenceAuthority {
    Authoritative,
    Conservative,
    Unknown,
}

/// Freshness of the accounting snapshot, independent from its authority.
#[derive(Debug, Clone, Copy, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ContextFreshness {
    Fresh,
    Stale,
    Unknown,
}

/// A bounded accounting snapshot. Units are deliberately opaque: callers may
/// use bytes, tokens, or another context accounting unit, but this contract
/// never identifies a model or provider.
#[derive(Debug, Clone, Copy, Deserialize, Serialize, PartialEq, Eq)]
pub struct ContextObservation {
    pub freshness: ContextFreshness,
    pub authority: EvidenceAuthority,
    pub used_units: Option<u64>,
    /// An optional second accounting component included in the total. Keeping
    /// this separate lets overflow be handled explicitly instead of wrapping.
    pub additional_units: Option<u64>,
    pub hard_limit_units: Option<u64>,
}

impl ContextObservation {
    pub fn fresh_authoritative(used_units: u64, hard_limit_units: Option<u64>) -> Self {
        Self {
            freshness: ContextFreshness::Fresh,
            authority: EvidenceAuthority::Authoritative,
            used_units: Some(used_units),
            additional_units: None,
            hard_limit_units,
        }
    }

    pub fn fresh_conservative(used_units: u64, hard_limit_units: Option<u64>) -> Self {
        Self {
            freshness: ContextFreshness::Fresh,
            authority: EvidenceAuthority::Conservative,
            used_units: Some(used_units),
            additional_units: None,
            hard_limit_units,
        }
    }

    pub fn stale(used_units: u64, hard_limit_units: Option<u64>) -> Self {
        Self {
            freshness: ContextFreshness::Stale,
            authority: EvidenceAuthority::Conservative,
            used_units: Some(used_units),
            additional_units: None,
            hard_limit_units,
        }
    }

    pub fn unknown() -> Self {
        Self {
            freshness: ContextFreshness::Unknown,
            authority: EvidenceAuthority::Unknown,
            used_units: None,
            additional_units: None,
            hard_limit_units: None,
        }
    }

    fn validate(self) -> Result<(), ThresholdError> {
        match (self.freshness, self.authority) {
            (ContextFreshness::Unknown, EvidenceAuthority::Unknown) => {
                if self.used_units.is_some()
                    || self.additional_units.is_some()
                    || self.hard_limit_units.is_some()
                {
                    return Err(ThresholdError::UnknownObservationContainsValues);
                }
            }
            (ContextFreshness::Unknown, _) => {
                return Err(ThresholdError::UnknownObservationAuthority);
            }
            (_, EvidenceAuthority::Unknown) => {
                return Err(ThresholdError::KnownObservationHasUnknownAuthority);
            }
            (_, _) => {
                if self.used_units.is_none() {
                    return Err(ThresholdError::MissingUsedUnits);
                }
            }
        }
        if self.hard_limit_units == Some(0) {
            return Err(ThresholdError::ZeroHardLimit);
        }
        Ok(())
    }

    fn total_units(self) -> Result<Option<u64>, ThresholdError> {
        let Some(used_units) = self.used_units else {
            return Ok(None);
        };
        used_units
            .checked_add(self.additional_units.unwrap_or(0))
            .map(Some)
            .ok_or(ThresholdError::AccountingOverflow)
    }
}

/// Thresholds expressed as basis points of a verified hard limit.
#[derive(Debug, Clone, Copy, Deserialize, Serialize, PartialEq, Eq)]
pub struct ThresholdConfig {
    /// Trigger review at or above this percentage of the hard limit.
    pub soft_threshold_basis_points: u16,
    /// Clear a previously latched review only below this percentage. This
    /// strict gap is the hysteresis that prevents threshold flapping.
    pub clear_threshold_basis_points: u16,
}

impl ThresholdConfig {
    pub const fn new(soft_threshold_basis_points: u16, clear_threshold_basis_points: u16) -> Self {
        Self {
            soft_threshold_basis_points,
            clear_threshold_basis_points,
        }
    }

    fn validate(self) -> Result<(), ThresholdError> {
        if self.soft_threshold_basis_points == 0 || self.soft_threshold_basis_points > BASIS_POINTS
        {
            return Err(ThresholdError::InvalidSoftThreshold);
        }
        if self.clear_threshold_basis_points == 0
            || self.clear_threshold_basis_points >= self.soft_threshold_basis_points
        {
            return Err(ThresholdError::InvalidClearThreshold);
        }
        Ok(())
    }
}

impl Default for ThresholdConfig {
    fn default() -> Self {
        Self::new(9_000, 8_000)
    }
}

/// The previous decision is supplied by the caller so the pure evaluator can
/// apply hysteresis without owning session state.
#[derive(Debug, Clone, Copy, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ThresholdStatus {
    NoAction,
    PreviewRequired,
    HardLimitExceeded,
}

#[derive(Debug, Clone, Copy, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ThresholdReason {
    BelowSoftThreshold,
    SoftThresholdReached,
    HysteresisLatched,
    HardLimitReached,
    StaleEvidence,
    UnknownEvidence,
    HardLimitUnavailable,
    ArithmeticOverflow,
}

/// Content-free output of [`evaluate`]. A `PreviewRequired` or
/// `HardLimitExceeded` result is a review signal only; it grants no model,
/// provider, execution, permission, or checkpoint authority.
#[derive(Debug, Clone, Copy, Serialize, PartialEq, Eq)]
pub struct ThresholdDecision {
    pub schema_version: &'static str,
    pub status: ThresholdStatus,
    pub reason: ThresholdReason,
    pub freshness: ContextFreshness,
    pub authority: EvidenceAuthority,
    pub used_units: Option<u64>,
    pub total_units: Option<u64>,
    pub hard_limit_units: Option<u64>,
    pub soft_threshold_units: Option<u64>,
    pub clear_threshold_units: Option<u64>,
    pub hysteresis_latched: bool,
    pub automatic_compaction_authority: bool,
}

impl ThresholdDecision {
    pub fn review_required(self) -> bool {
        matches!(
            self.status,
            ThresholdStatus::PreviewRequired | ThresholdStatus::HardLimitExceeded
        )
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ThresholdError {
    InvalidSoftThreshold,
    InvalidClearThreshold,
    UnknownObservationContainsValues,
    UnknownObservationAuthority,
    KnownObservationHasUnknownAuthority,
    MissingUsedUnits,
    ZeroHardLimit,
    AccountingOverflow,
}

/// Evaluate one observation without performing any compaction side effect.
pub fn evaluate(
    observation: ContextObservation,
    config: ThresholdConfig,
    previous_status: ThresholdStatus,
) -> Result<ThresholdDecision, ThresholdError> {
    config.validate()?;
    observation.validate()?;

    let base =
        |status, reason, total_units, soft_threshold_units, clear_threshold_units, latched| {
            ThresholdDecision {
                schema_version: SCHEMA_VERSION,
                status,
                reason,
                freshness: observation.freshness,
                authority: observation.authority,
                used_units: observation.used_units,
                total_units,
                hard_limit_units: observation.hard_limit_units,
                soft_threshold_units,
                clear_threshold_units,
                hysteresis_latched: latched,
                automatic_compaction_authority: false,
            }
        };

    if observation.freshness == ContextFreshness::Unknown {
        return Ok(base(
            ThresholdStatus::PreviewRequired,
            ThresholdReason::UnknownEvidence,
            None,
            None,
            None,
            false,
        ));
    }

    let total_units = match observation.total_units() {
        Ok(Some(total_units)) => total_units,
        Ok(None) => unreachable!("known observations require used units"),
        Err(ThresholdError::AccountingOverflow) => {
            return Ok(base(
                ThresholdStatus::PreviewRequired,
                ThresholdReason::ArithmeticOverflow,
                None,
                None,
                None,
                true,
            ));
        }
        Err(error) => return Err(error),
    };

    let Some(hard_limit_units) = observation.hard_limit_units else {
        return Ok(base(
            ThresholdStatus::PreviewRequired,
            ThresholdReason::HardLimitUnavailable,
            Some(total_units),
            None,
            None,
            true,
        ));
    };
    let soft_threshold_units =
        threshold_units(hard_limit_units, config.soft_threshold_basis_points);
    let clear_threshold_units =
        threshold_units(hard_limit_units, config.clear_threshold_basis_points);

    // Stale observations can explain why review is needed but cannot establish
    // a fresh no-action or hard-limit claim.
    if observation.freshness == ContextFreshness::Stale {
        return Ok(base(
            ThresholdStatus::PreviewRequired,
            ThresholdReason::StaleEvidence,
            Some(total_units),
            Some(soft_threshold_units),
            Some(clear_threshold_units),
            true,
        ));
    }

    if total_units >= hard_limit_units {
        return Ok(base(
            ThresholdStatus::HardLimitExceeded,
            ThresholdReason::HardLimitReached,
            Some(total_units),
            Some(soft_threshold_units),
            Some(clear_threshold_units),
            true,
        ));
    }

    let previously_latched = matches!(
        previous_status,
        ThresholdStatus::PreviewRequired | ThresholdStatus::HardLimitExceeded
    );
    if previously_latched && total_units >= clear_threshold_units {
        return Ok(base(
            ThresholdStatus::PreviewRequired,
            ThresholdReason::HysteresisLatched,
            Some(total_units),
            Some(soft_threshold_units),
            Some(clear_threshold_units),
            true,
        ));
    }
    if total_units >= soft_threshold_units {
        return Ok(base(
            ThresholdStatus::PreviewRequired,
            ThresholdReason::SoftThresholdReached,
            Some(total_units),
            Some(soft_threshold_units),
            Some(clear_threshold_units),
            false,
        ));
    }
    Ok(base(
        ThresholdStatus::NoAction,
        ThresholdReason::BelowSoftThreshold,
        Some(total_units),
        Some(soft_threshold_units),
        Some(clear_threshold_units),
        false,
    ))
}

fn threshold_units(hard_limit_units: u64, basis_points: u16) -> u64 {
    // Ceiling avoids a low hard limit rounding a threshold down to zero.
    let product = u128::from(hard_limit_units) * u128::from(basis_points);
    let rounded = product.div_ceil(u128::from(BASIS_POINTS));
    rounded.min(u128::from(u64::MAX)) as u64
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config() -> ThresholdConfig {
        ThresholdConfig::new(9_000, 8_000)
    }

    #[test]
    fn fresh_authoritative_below_soft_threshold_is_no_action() {
        let decision = evaluate(
            ContextObservation::fresh_authoritative(79, Some(100)),
            config(),
            ThresholdStatus::NoAction,
        )
        .unwrap();
        assert_eq!(decision.status, ThresholdStatus::NoAction);
        assert_eq!(decision.reason, ThresholdReason::BelowSoftThreshold);
        assert!(!decision.review_required());
        assert!(!decision.automatic_compaction_authority);
    }

    #[test]
    fn conservative_usage_reaching_soft_threshold_requires_preview() {
        let decision = evaluate(
            ContextObservation::fresh_conservative(90, Some(100)),
            config(),
            ThresholdStatus::NoAction,
        )
        .unwrap();
        assert_eq!(decision.status, ThresholdStatus::PreviewRequired);
        assert_eq!(decision.reason, ThresholdReason::SoftThresholdReached);
        assert_eq!(decision.authority, EvidenceAuthority::Conservative);
    }

    #[test]
    fn hard_limit_is_explicit_and_content_free() {
        let decision = evaluate(
            ContextObservation::fresh_authoritative(100, Some(100)),
            config(),
            ThresholdStatus::NoAction,
        )
        .unwrap();
        assert_eq!(decision.status, ThresholdStatus::HardLimitExceeded);
        assert_eq!(decision.reason, ThresholdReason::HardLimitReached);
        assert_eq!(decision.total_units, Some(100));
        assert!(decision.review_required());
    }

    #[test]
    fn stale_data_never_claims_no_action_or_hard_limit() {
        let decision = evaluate(
            ContextObservation::stale(10, Some(100)),
            config(),
            ThresholdStatus::NoAction,
        )
        .unwrap();
        assert_eq!(decision.status, ThresholdStatus::PreviewRequired);
        assert_eq!(decision.reason, ThresholdReason::StaleEvidence);
        assert_ne!(decision.status, ThresholdStatus::HardLimitExceeded);
    }

    #[test]
    fn unknown_data_is_preview_required_without_values() {
        let decision = evaluate(
            ContextObservation::unknown(),
            config(),
            ThresholdStatus::NoAction,
        )
        .unwrap();
        assert_eq!(decision.status, ThresholdStatus::PreviewRequired);
        assert_eq!(decision.reason, ThresholdReason::UnknownEvidence);
        assert_eq!(decision.used_units, None);
        assert_eq!(decision.total_units, None);
        assert_eq!(decision.hard_limit_units, None);
    }

    #[test]
    fn missing_hard_limit_fails_closed_to_preview() {
        let decision = evaluate(
            ContextObservation::fresh_authoritative(1, None),
            config(),
            ThresholdStatus::NoAction,
        )
        .unwrap();
        assert_eq!(decision.status, ThresholdStatus::PreviewRequired);
        assert_eq!(decision.reason, ThresholdReason::HardLimitUnavailable);
    }

    #[test]
    fn hysteresis_keeps_preview_latched_until_clear_threshold() {
        let first = evaluate(
            ContextObservation::fresh_authoritative(90, Some(100)),
            config(),
            ThresholdStatus::NoAction,
        )
        .unwrap();
        assert_eq!(first.status, ThresholdStatus::PreviewRequired);

        let held = evaluate(
            ContextObservation::fresh_authoritative(85, Some(100)),
            config(),
            first.status,
        )
        .unwrap();
        assert_eq!(held.status, ThresholdStatus::PreviewRequired);
        assert_eq!(held.reason, ThresholdReason::HysteresisLatched);
        assert!(held.hysteresis_latched);

        let cleared = evaluate(
            ContextObservation::fresh_authoritative(79, Some(100)),
            config(),
            held.status,
        )
        .unwrap();
        assert_eq!(cleared.status, ThresholdStatus::NoAction);
        assert!(!cleared.hysteresis_latched);
    }

    #[test]
    fn accounting_overflow_requires_preview_instead_of_wrapping() {
        let mut observation = ContextObservation::fresh_authoritative(u64::MAX, Some(u64::MAX));
        observation.additional_units = Some(1);
        let decision = evaluate(observation, config(), ThresholdStatus::NoAction).unwrap();
        assert_eq!(decision.status, ThresholdStatus::PreviewRequired);
        assert_eq!(decision.reason, ThresholdReason::ArithmeticOverflow);
        assert_eq!(decision.total_units, None);
    }

    #[test]
    fn malformed_known_and_unknown_observations_are_rejected() {
        let mut missing_used = ContextObservation::fresh_authoritative(1, Some(10));
        missing_used.used_units = None;
        assert_eq!(
            evaluate(missing_used, config(), ThresholdStatus::NoAction),
            Err(ThresholdError::MissingUsedUnits)
        );

        let mut unknown_with_value = ContextObservation::unknown();
        unknown_with_value.used_units = Some(1);
        assert_eq!(
            evaluate(unknown_with_value, config(), ThresholdStatus::NoAction),
            Err(ThresholdError::UnknownObservationContainsValues)
        );
    }

    #[test]
    fn invalid_thresholds_are_rejected_before_evaluation() {
        assert_eq!(
            evaluate(
                ContextObservation::fresh_authoritative(1, Some(10)),
                ThresholdConfig::new(8_000, 8_000),
                ThresholdStatus::NoAction,
            ),
            Err(ThresholdError::InvalidClearThreshold)
        );
        assert_eq!(
            evaluate(
                ContextObservation::fresh_authoritative(1, Some(10)),
                ThresholdConfig::new(0, 1),
                ThresholdStatus::NoAction,
            ),
            Err(ThresholdError::InvalidSoftThreshold)
        );
    }
}
