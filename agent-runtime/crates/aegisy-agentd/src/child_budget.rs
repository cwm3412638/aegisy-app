//! Runtime-enforced resource budget foundation for child and background work.
//!
//! This ledger admits and settles bounded work. It does not schedule work,
//! launch a model or tool, grant permissions, or persist a job. Unknown model
//! usage is charged conservatively against the exact reservation, never as zero.

use crate::child_task::ChildTaskRequest;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

pub const SCHEMA_VERSION: &str = "child-runtime-budget/0.1";
const MAX_NETWORK_REQUESTS: u32 = 10_000;
const MIN_WARNING_PERCENT: u8 = 50;
const MAX_WARNING_PERCENT: u8 = 99;
const MAX_OPERATION_ID_BYTES: usize = 128;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ChildBudgetError {
    pub code: &'static str,
    pub message: &'static str,
    pub dimension: Option<BudgetDimension>,
}

impl ChildBudgetError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self {
            code,
            message,
            dimension: None,
        }
    }

    fn dimension(code: &'static str, message: &'static str, dimension: BudgetDimension) -> Self {
        Self {
            code,
            message,
            dimension: Some(dimension),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BudgetDimension {
    Tokens,
    CostMicros,
    WallTimeMs,
    Turns,
    ToolCalls,
    Concurrency,
    NetworkRequests,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BudgetState {
    Ready,
    Warning,
    Exhausted,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum UsageSource {
    Authoritative,
    Estimated,
    Unknown,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct UsageObservation {
    pub amount: Option<u64>,
    pub source: UsageSource,
}

impl UsageObservation {
    pub fn authoritative(amount: u64) -> Self {
        Self {
            amount: Some(amount),
            source: UsageSource::Authoritative,
        }
    }

    pub fn estimated(amount: u64) -> Self {
        Self {
            amount: Some(amount),
            source: UsageSource::Estimated,
        }
    }

    pub fn unknown() -> Self {
        Self {
            amount: None,
            source: UsageSource::Unknown,
        }
    }

    fn validate(self) -> Result<(), ChildBudgetError> {
        match (self.source, self.amount) {
            (UsageSource::Unknown, None)
            | (UsageSource::Authoritative | UsageSource::Estimated, Some(_)) => Ok(()),
            _ => Err(ChildBudgetError::new(
                "child-budget-usage-source-invalid",
                "budget usage amount and source do not agree",
            )),
        }
    }
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct UsageSourceCounts {
    pub authoritative: u32,
    pub estimated: u32,
    pub unknown: u32,
}

impl UsageSourceCounts {
    fn increment(&mut self, source: UsageSource) -> Result<(), ChildBudgetError> {
        let target = match source {
            UsageSource::Authoritative => &mut self.authoritative,
            UsageSource::Estimated => &mut self.estimated,
            UsageSource::Unknown => &mut self.unknown,
        };
        *target = target.checked_add(1).ok_or_else(|| {
            ChildBudgetError::new(
                "child-budget-counter-overflow",
                "budget provenance counter overflowed",
            )
        })?;
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ChildBudgetLimits {
    pub max_tokens: u64,
    pub max_cost_micros: u64,
    pub max_wall_time_ms: u64,
    pub max_turns: u32,
    pub max_tool_calls: u32,
    pub max_concurrency: u16,
    pub max_network_requests: u32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ChildBudgetRemaining {
    pub tokens: u64,
    pub cost_micros: u64,
    pub wall_time_ms: u64,
    pub turns: u32,
    pub tool_calls: u32,
    pub concurrency: u16,
    pub network_requests: u32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ChildBudgetSnapshot {
    pub schema_version: String,
    pub task_identity: String,
    pub generation: u64,
    pub state: BudgetState,
    pub started_at_ms: u64,
    pub observed_at_ms: u64,
    pub warning_percent: u8,
    pub limits: ChildBudgetLimits,
    pub remaining: ChildBudgetRemaining,
    pub tokens_used: u64,
    pub tokens_reserved: u64,
    pub cost_micros_used: u64,
    pub cost_micros_reserved: u64,
    pub wall_time_ms: u64,
    pub turns_started: u32,
    pub tool_calls_started: u32,
    pub network_requests_started: u32,
    pub active_operations: u16,
    pub peak_concurrency: u16,
    pub token_usage_sources: UsageSourceCounts,
    pub cost_usage_sources: UsageSourceCounts,
    pub warning_dimensions: Vec<BudgetDimension>,
    pub saturated_dimensions: Vec<BudgetDimension>,
    pub exhausted_dimensions: Vec<BudgetDimension>,
    pub permission_granted: bool,
    pub execution_available: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ReservationKind {
    ModelTurn,
    ToolCall { network: bool },
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct ActiveReservation {
    kind: ReservationKind,
    tokens: u64,
    cost_micros: u64,
}

#[derive(Debug, Clone)]
pub struct ChildBudgetLedger {
    task_identity: String,
    limits: ChildBudgetLimits,
    warning_percent: u8,
    started_at_ms: u64,
    observed_at_ms: u64,
    generation: u64,
    tokens_used: u64,
    tokens_reserved: u64,
    cost_micros_used: u64,
    cost_micros_reserved: u64,
    turns_started: u32,
    tool_calls_started: u32,
    network_requests_started: u32,
    peak_concurrency: u16,
    token_usage_sources: UsageSourceCounts,
    cost_usage_sources: UsageSourceCounts,
    active: BTreeMap<String, ActiveReservation>,
}

impl ChildBudgetLedger {
    pub fn new(
        request: &ChildTaskRequest,
        max_network_requests: Option<u32>,
        warning_percent: u8,
        now_ms: u64,
    ) -> Result<Self, ChildBudgetError> {
        request.validate().map_err(|_| {
            ChildBudgetError::new(
                "child-budget-request-invalid",
                "child task request is invalid",
            )
        })?;
        if !(MIN_WARNING_PERCENT..=MAX_WARNING_PERCENT).contains(&warning_percent) {
            return Err(ChildBudgetError::new(
                "child-budget-warning-threshold-invalid",
                "budget warning threshold is outside the bounded range",
            ));
        }
        let max_network_requests = match request.permissions.network.as_str() {
            "none" => match max_network_requests {
                None | Some(0) => 0,
                Some(_) => {
                    return Err(ChildBudgetError::new(
                        "child-budget-network-policy-mismatch",
                        "network-disabled child cannot receive a network budget",
                    ));
                }
            },
            "allowlisted" | "full" => match max_network_requests {
                Some(value @ 1..=MAX_NETWORK_REQUESTS) => value,
                _ => {
                    return Err(ChildBudgetError::new(
                        "child-budget-network-limit-required",
                        "network-enabled child requires a bounded request limit",
                    ));
                }
            },
            _ => {
                return Err(ChildBudgetError::new(
                    "child-budget-network-policy-invalid",
                    "child network policy is invalid",
                ));
            }
        };
        let task_identity = request.identity().map_err(|_| {
            ChildBudgetError::new(
                "child-budget-task-identity-invalid",
                "child task identity is invalid",
            )
        })?;
        let ledger = Self {
            task_identity,
            limits: ChildBudgetLimits {
                max_tokens: request.budget.max_tokens,
                max_cost_micros: request.budget.max_cost_micros,
                max_wall_time_ms: request.budget.max_wall_time_ms,
                max_turns: request.budget.max_turns,
                max_tool_calls: request.budget.max_tool_calls,
                max_concurrency: request.budget.max_concurrency,
                max_network_requests,
            },
            warning_percent,
            started_at_ms: now_ms,
            observed_at_ms: now_ms,
            generation: 0,
            tokens_used: 0,
            tokens_reserved: 0,
            cost_micros_used: 0,
            cost_micros_reserved: 0,
            turns_started: 0,
            tool_calls_started: 0,
            network_requests_started: 0,
            peak_concurrency: 0,
            token_usage_sources: UsageSourceCounts::default(),
            cost_usage_sources: UsageSourceCounts::default(),
            active: BTreeMap::new(),
        };
        ledger.validate()?;
        Ok(ledger)
    }

    pub fn reserve_model_turn(
        &mut self,
        operation_id: &str,
        max_tokens: u64,
        max_cost_micros: u64,
        now_ms: u64,
    ) -> Result<ChildBudgetSnapshot, ChildBudgetError> {
        self.preflight_operation(operation_id, now_ms)?;
        if max_tokens == 0 || max_cost_micros == 0 {
            return Err(ChildBudgetError::new(
                "child-budget-model-reservation-invalid",
                "model work requires non-zero token and cost reservations",
            ));
        }
        self.require_wall_time(now_ms)?;
        self.require_concurrency()?;
        if self.turns_started >= self.limits.max_turns {
            return Err(limit_error(BudgetDimension::Turns));
        }
        require_capacity(
            self.tokens_used,
            self.tokens_reserved,
            max_tokens,
            self.limits.max_tokens,
            BudgetDimension::Tokens,
        )?;
        require_capacity(
            self.cost_micros_used,
            self.cost_micros_reserved,
            max_cost_micros,
            self.limits.max_cost_micros,
            BudgetDimension::CostMicros,
        )?;

        self.apply_update(now_ms, |ledger| {
            ledger.turns_started = ledger.turns_started.checked_add(1).ok_or_else(|| {
                ChildBudgetError::new(
                    "child-budget-counter-overflow",
                    "turn budget counter overflowed",
                )
            })?;
            ledger.tokens_reserved = ledger
                .tokens_reserved
                .checked_add(max_tokens)
                .ok_or_else(counter_overflow)?;
            ledger.cost_micros_reserved = ledger
                .cost_micros_reserved
                .checked_add(max_cost_micros)
                .ok_or_else(counter_overflow)?;
            ledger.active.insert(
                operation_id.into(),
                ActiveReservation {
                    kind: ReservationKind::ModelTurn,
                    tokens: max_tokens,
                    cost_micros: max_cost_micros,
                },
            );
            ledger.update_peak_concurrency()?;
            Ok(())
        })
    }

    pub fn settle_model_turn(
        &mut self,
        operation_id: &str,
        token_usage: UsageObservation,
        cost_usage: UsageObservation,
        now_ms: u64,
    ) -> Result<ChildBudgetSnapshot, ChildBudgetError> {
        token_usage.validate()?;
        cost_usage.validate()?;
        self.require_clock(now_ms)?;
        let reservation = self.active.get(operation_id).cloned().ok_or_else(|| {
            ChildBudgetError::new(
                "child-budget-reservation-missing",
                "model budget reservation is missing",
            )
        })?;
        if reservation.kind != ReservationKind::ModelTurn {
            return Err(ChildBudgetError::new(
                "child-budget-reservation-kind-mismatch",
                "budget reservation is not a model turn",
            ));
        }
        let charged_tokens = token_usage.amount.unwrap_or(reservation.tokens);
        let charged_cost = cost_usage.amount.unwrap_or(reservation.cost_micros);

        self.apply_update(now_ms, |ledger| {
            ledger.active.remove(operation_id);
            ledger.tokens_reserved = ledger
                .tokens_reserved
                .checked_sub(reservation.tokens)
                .ok_or_else(counter_overflow)?;
            ledger.cost_micros_reserved = ledger
                .cost_micros_reserved
                .checked_sub(reservation.cost_micros)
                .ok_or_else(counter_overflow)?;
            ledger.tokens_used = ledger
                .tokens_used
                .checked_add(charged_tokens)
                .ok_or_else(counter_overflow)?;
            ledger.cost_micros_used = ledger
                .cost_micros_used
                .checked_add(charged_cost)
                .ok_or_else(counter_overflow)?;
            ledger.token_usage_sources.increment(token_usage.source)?;
            ledger.cost_usage_sources.increment(cost_usage.source)?;
            Ok(())
        })
    }

    pub fn reserve_tool_call(
        &mut self,
        operation_id: &str,
        network: bool,
        now_ms: u64,
    ) -> Result<ChildBudgetSnapshot, ChildBudgetError> {
        self.preflight_operation(operation_id, now_ms)?;
        self.require_wall_time(now_ms)?;
        self.require_committed_model_budget()?;
        self.require_concurrency()?;
        if self.tool_calls_started >= self.limits.max_tool_calls {
            return Err(limit_error(BudgetDimension::ToolCalls));
        }
        if network
            && (self.limits.max_network_requests == 0
                || self.network_requests_started >= self.limits.max_network_requests)
        {
            return Err(limit_error(BudgetDimension::NetworkRequests));
        }

        self.apply_update(now_ms, |ledger| {
            ledger.tool_calls_started = ledger
                .tool_calls_started
                .checked_add(1)
                .ok_or_else(counter_overflow)?;
            if network {
                ledger.network_requests_started = ledger
                    .network_requests_started
                    .checked_add(1)
                    .ok_or_else(counter_overflow)?;
            }
            ledger.active.insert(
                operation_id.into(),
                ActiveReservation {
                    kind: ReservationKind::ToolCall { network },
                    tokens: 0,
                    cost_micros: 0,
                },
            );
            ledger.update_peak_concurrency()?;
            Ok(())
        })
    }

    pub fn finish_tool_call(
        &mut self,
        operation_id: &str,
        now_ms: u64,
    ) -> Result<ChildBudgetSnapshot, ChildBudgetError> {
        self.require_clock(now_ms)?;
        let reservation = self.active.get(operation_id).cloned().ok_or_else(|| {
            ChildBudgetError::new(
                "child-budget-reservation-missing",
                "tool budget reservation is missing",
            )
        })?;
        if !matches!(reservation.kind, ReservationKind::ToolCall { .. }) {
            return Err(ChildBudgetError::new(
                "child-budget-reservation-kind-mismatch",
                "budget reservation is not a tool call",
            ));
        }
        self.apply_update(now_ms, |ledger| {
            ledger.active.remove(operation_id);
            Ok(())
        })
    }

    pub fn observe(&mut self, now_ms: u64) -> Result<ChildBudgetSnapshot, ChildBudgetError> {
        self.require_clock(now_ms)?;
        if now_ms == self.observed_at_ms {
            return Ok(self.snapshot());
        }
        self.apply_update(now_ms, |_| Ok(()))
    }

    pub fn snapshot(&self) -> ChildBudgetSnapshot {
        let wall_time_ms = self.observed_at_ms - self.started_at_ms;
        let mut classification = DimensionClassification::default();

        let tokens_committed = self.tokens_used;
        let tokens_admitted = self.tokens_used.saturating_add(self.tokens_reserved);
        classification.consumptive(
            BudgetDimension::Tokens,
            tokens_committed,
            tokens_admitted,
            self.limits.max_tokens,
            self.warning_percent,
        );
        classification.consumptive(
            BudgetDimension::CostMicros,
            self.cost_micros_used,
            self.cost_micros_used
                .saturating_add(self.cost_micros_reserved),
            self.limits.max_cost_micros,
            self.warning_percent,
        );
        classification.count(
            BudgetDimension::WallTimeMs,
            wall_time_ms,
            self.limits.max_wall_time_ms,
            self.warning_percent,
        );
        classification.count(
            BudgetDimension::Turns,
            u64::from(self.turns_started),
            u64::from(self.limits.max_turns),
            self.warning_percent,
        );
        classification.count(
            BudgetDimension::ToolCalls,
            u64::from(self.tool_calls_started),
            u64::from(self.limits.max_tool_calls),
            self.warning_percent,
        );
        if self.limits.max_network_requests > 0 {
            classification.count(
                BudgetDimension::NetworkRequests,
                u64::from(self.network_requests_started),
                u64::from(self.limits.max_network_requests),
                self.warning_percent,
            );
        }
        let active_operations = u16::try_from(self.active.len()).unwrap_or(u16::MAX);
        if active_operations >= self.limits.max_concurrency {
            classification.saturated.push(BudgetDimension::Concurrency);
        } else if percent_reached(
            u64::from(active_operations),
            u64::from(self.limits.max_concurrency),
            self.warning_percent,
        ) {
            classification.warnings.push(BudgetDimension::Concurrency);
        }

        classification.normalize();
        let state = if classification.exhausted.is_empty() {
            if classification.warnings.is_empty() && classification.saturated.is_empty() {
                BudgetState::Ready
            } else {
                BudgetState::Warning
            }
        } else {
            BudgetState::Exhausted
        };
        let remaining = ChildBudgetRemaining {
            tokens: self.limits.max_tokens.saturating_sub(tokens_admitted),
            cost_micros: self.limits.max_cost_micros.saturating_sub(
                self.cost_micros_used
                    .saturating_add(self.cost_micros_reserved),
            ),
            wall_time_ms: self.limits.max_wall_time_ms.saturating_sub(wall_time_ms),
            turns: self.limits.max_turns.saturating_sub(self.turns_started),
            tool_calls: self
                .limits
                .max_tool_calls
                .saturating_sub(self.tool_calls_started),
            concurrency: self
                .limits
                .max_concurrency
                .saturating_sub(active_operations),
            network_requests: self
                .limits
                .max_network_requests
                .saturating_sub(self.network_requests_started),
        };

        ChildBudgetSnapshot {
            schema_version: SCHEMA_VERSION.into(),
            task_identity: self.task_identity.clone(),
            generation: self.generation,
            state,
            started_at_ms: self.started_at_ms,
            observed_at_ms: self.observed_at_ms,
            warning_percent: self.warning_percent,
            limits: self.limits.clone(),
            remaining,
            tokens_used: self.tokens_used,
            tokens_reserved: self.tokens_reserved,
            cost_micros_used: self.cost_micros_used,
            cost_micros_reserved: self.cost_micros_reserved,
            wall_time_ms,
            turns_started: self.turns_started,
            tool_calls_started: self.tool_calls_started,
            network_requests_started: self.network_requests_started,
            active_operations,
            peak_concurrency: self.peak_concurrency,
            token_usage_sources: self.token_usage_sources,
            cost_usage_sources: self.cost_usage_sources,
            warning_dimensions: classification.warnings,
            saturated_dimensions: classification.saturated,
            exhausted_dimensions: classification.exhausted,
            permission_granted: false,
            execution_available: false,
        }
    }

    fn preflight_operation(&self, operation_id: &str, now_ms: u64) -> Result<(), ChildBudgetError> {
        self.validate()?;
        self.require_clock(now_ms)?;
        validate_operation_id(operation_id)?;
        if self.active.contains_key(operation_id) {
            return Err(ChildBudgetError::new(
                "child-budget-operation-duplicate",
                "budget operation ID is already active",
            ));
        }
        Ok(())
    }

    fn require_clock(&self, now_ms: u64) -> Result<(), ChildBudgetError> {
        if now_ms < self.observed_at_ms {
            return Err(ChildBudgetError::dimension(
                "child-budget-clock-regressed",
                "budget clock moved backwards",
                BudgetDimension::WallTimeMs,
            ));
        }
        Ok(())
    }

    fn require_wall_time(&self, now_ms: u64) -> Result<(), ChildBudgetError> {
        if now_ms.saturating_sub(self.started_at_ms) >= self.limits.max_wall_time_ms {
            return Err(limit_error(BudgetDimension::WallTimeMs));
        }
        Ok(())
    }

    fn require_concurrency(&self) -> Result<(), ChildBudgetError> {
        if self.active.len() >= usize::from(self.limits.max_concurrency) {
            return Err(ChildBudgetError::dimension(
                "child-budget-concurrency-blocked",
                "budget concurrency is temporarily saturated",
                BudgetDimension::Concurrency,
            ));
        }
        Ok(())
    }

    fn require_committed_model_budget(&self) -> Result<(), ChildBudgetError> {
        if self.tokens_used >= self.limits.max_tokens {
            return Err(limit_error(BudgetDimension::Tokens));
        }
        if self.cost_micros_used >= self.limits.max_cost_micros {
            return Err(limit_error(BudgetDimension::CostMicros));
        }
        Ok(())
    }

    fn update_peak_concurrency(&mut self) -> Result<(), ChildBudgetError> {
        let active = u16::try_from(self.active.len()).map_err(|_| {
            ChildBudgetError::new(
                "child-budget-counter-overflow",
                "active budget operation count overflowed",
            )
        })?;
        self.peak_concurrency = self.peak_concurrency.max(active);
        Ok(())
    }

    fn apply_update(
        &mut self,
        now_ms: u64,
        update: impl FnOnce(&mut Self) -> Result<(), ChildBudgetError>,
    ) -> Result<ChildBudgetSnapshot, ChildBudgetError> {
        self.validate()?;
        self.require_clock(now_ms)?;
        let next_generation = self.generation.checked_add(1).ok_or_else(|| {
            ChildBudgetError::new(
                "child-budget-generation-exhausted",
                "budget ledger generation is exhausted",
            )
        })?;
        let previous = self.clone();
        if let Err(error) = update(self) {
            *self = previous;
            return Err(error);
        }
        self.generation = next_generation;
        self.observed_at_ms = now_ms;
        if let Err(error) = self.validate() {
            *self = previous;
            return Err(error);
        }
        Ok(self.snapshot())
    }

    fn validate(&self) -> Result<(), ChildBudgetError> {
        if !valid_task_identity(&self.task_identity)
            || !(MIN_WARNING_PERCENT..=MAX_WARNING_PERCENT).contains(&self.warning_percent)
            || self.observed_at_ms < self.started_at_ms
        {
            return Err(ChildBudgetError::new(
                "child-budget-ledger-invalid",
                "budget ledger invariant is invalid",
            ));
        }
        if self.active.len() > usize::from(self.limits.max_concurrency)
            || self.turns_started > self.limits.max_turns
            || self.tool_calls_started > self.limits.max_tool_calls
            || (self.limits.max_network_requests > 0
                && self.network_requests_started > self.limits.max_network_requests)
            || (self.limits.max_network_requests == 0 && self.network_requests_started != 0)
        {
            return Err(ChildBudgetError::new(
                "child-budget-ledger-invalid",
                "budget ledger counters violate configured limits",
            ));
        }
        let expected_tokens = self
            .active
            .values()
            .try_fold(0_u64, |total, reservation| {
                total.checked_add(reservation.tokens)
            })
            .ok_or_else(counter_overflow)?;
        let expected_cost = self
            .active
            .values()
            .try_fold(0_u64, |total, reservation| {
                total.checked_add(reservation.cost_micros)
            })
            .ok_or_else(counter_overflow)?;
        if expected_tokens != self.tokens_reserved || expected_cost != self.cost_micros_reserved {
            return Err(ChildBudgetError::new(
                "child-budget-reservation-invariant-invalid",
                "budget reservations do not match active operations",
            ));
        }
        Ok(())
    }
}

fn require_capacity(
    used: u64,
    reserved: u64,
    requested: u64,
    limit: u64,
    dimension: BudgetDimension,
) -> Result<(), ChildBudgetError> {
    let admitted = used
        .checked_add(reserved)
        .and_then(|value| value.checked_add(requested))
        .ok_or_else(counter_overflow)?;
    if admitted > limit {
        return Err(limit_error(dimension));
    }
    Ok(())
}

#[derive(Default)]
struct DimensionClassification {
    warnings: Vec<BudgetDimension>,
    saturated: Vec<BudgetDimension>,
    exhausted: Vec<BudgetDimension>,
}

impl DimensionClassification {
    fn consumptive(
        &mut self,
        dimension: BudgetDimension,
        committed: u64,
        admitted: u64,
        limit: u64,
        warning_percent: u8,
    ) {
        if committed >= limit {
            self.exhausted.push(dimension);
        } else if admitted >= limit {
            self.saturated.push(dimension);
        } else if percent_reached(admitted, limit, warning_percent) {
            self.warnings.push(dimension);
        }
    }

    fn count(&mut self, dimension: BudgetDimension, used: u64, limit: u64, warning_percent: u8) {
        if used >= limit {
            self.exhausted.push(dimension);
        } else if percent_reached(used, limit, warning_percent) {
            self.warnings.push(dimension);
        }
    }

    fn normalize(&mut self) {
        self.warnings.sort();
        self.warnings.dedup();
        self.saturated.sort();
        self.saturated.dedup();
        self.exhausted.sort();
        self.exhausted.dedup();
    }
}

fn percent_reached(used: u64, limit: u64, threshold: u8) -> bool {
    used.saturating_mul(100) >= limit.saturating_mul(u64::from(threshold))
}

fn validate_operation_id(operation_id: &str) -> Result<(), ChildBudgetError> {
    if operation_id.is_empty()
        || operation_id.len() > MAX_OPERATION_ID_BYTES
        || operation_id
            .bytes()
            .any(|byte| !(byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b':')))
    {
        return Err(ChildBudgetError::new(
            "child-budget-operation-id-invalid",
            "budget operation ID is invalid",
        ));
    }
    Ok(())
}

fn valid_task_identity(value: &str) -> bool {
    value.strip_prefix("child-task:sha256:").is_some_and(|hex| {
        hex.len() == 64
            && hex
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    })
}

fn counter_overflow() -> ChildBudgetError {
    ChildBudgetError::new("child-budget-counter-overflow", "budget counter overflowed")
}

fn limit_error(dimension: BudgetDimension) -> ChildBudgetError {
    ChildBudgetError::dimension(
        "child-budget-limit-exhausted",
        "budget limit is exhausted or the requested reservation would exceed it",
        dimension,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::child_task::{
        ChildTaskBudget, ExpectedResultShape, PermissionRequest, WorkspaceScope,
        HANDOFF_SCHEMA_VERSION,
    };

    fn request(network: &str) -> ChildTaskRequest {
        ChildTaskRequest {
            schema_version: crate::child_task::SCHEMA_VERSION.into(),
            task_id: "budget-child".into(),
            parent_session_id: "parent-session".into(),
            parent_turn_id: "parent-turn".into(),
            goal: "Perform bounded child work and return evidence".into(),
            context: Vec::new(),
            workspace: WorkspaceScope {
                project_id: "project-1".into(),
                root_id: "root-1".into(),
                isolation: "read_only".into(),
                base_revision: "revision-1".into(),
            },
            tools: vec!["workspace-read".into()],
            model_profile: "agent-default".into(),
            permissions: PermissionRequest {
                profile: "read-only".into(),
                network: network.into(),
                write: false,
            },
            budget: ChildTaskBudget {
                max_tokens: 1_000,
                max_cost_micros: 2_000,
                max_wall_time_ms: 10_000,
                max_turns: 3,
                max_tool_calls: 4,
                max_concurrency: 2,
            },
            expected_result: ExpectedResultShape {
                schema_version: HANDOFF_SCHEMA_VERSION.into(),
                max_summary_bytes: 8 * 1024,
                required_sections: vec!["summary".into()],
            },
        }
    }

    #[test]
    fn reserves_then_settles_authoritative_and_estimated_usage() {
        let mut ledger = ChildBudgetLedger::new(&request("none"), None, 80, 1_000).unwrap();
        let reserved = ledger
            .reserve_model_turn("turn-1", 400, 500, 1_100)
            .unwrap();
        assert_eq!(reserved.tokens_reserved, 400);
        assert_eq!(reserved.cost_micros_reserved, 500);
        assert_eq!(reserved.active_operations, 1);
        let settled = ledger
            .settle_model_turn(
                "turn-1",
                UsageObservation::authoritative(300),
                UsageObservation::estimated(250),
                1_200,
            )
            .unwrap();
        assert_eq!(settled.tokens_used, 300);
        assert_eq!(settled.cost_micros_used, 250);
        assert_eq!(settled.tokens_reserved, 0);
        assert_eq!(settled.active_operations, 0);
        assert_eq!(settled.token_usage_sources.authoritative, 1);
        assert_eq!(settled.cost_usage_sources.estimated, 1);
        assert!(!settled.permission_granted);
        assert!(!settled.execution_available);
    }

    #[test]
    fn unknown_usage_is_charged_as_full_reservation_not_zero() {
        let mut ledger = ChildBudgetLedger::new(&request("none"), None, 80, 1_000).unwrap();
        let warning = ledger
            .reserve_model_turn("turn-unknown", 850, 1_700, 1_100)
            .unwrap();
        assert_eq!(warning.state, BudgetState::Warning);
        assert!(warning
            .warning_dimensions
            .contains(&BudgetDimension::Tokens));
        assert!(warning
            .warning_dimensions
            .contains(&BudgetDimension::CostMicros));
        assert_eq!(warning.remaining.tokens, 150);
        assert_eq!(warning.remaining.cost_micros, 300);
        let settled = ledger
            .settle_model_turn(
                "turn-unknown",
                UsageObservation::unknown(),
                UsageObservation::unknown(),
                1_200,
            )
            .unwrap();
        assert_eq!(settled.tokens_used, 850);
        assert_eq!(settled.cost_micros_used, 1_700);
        assert_eq!(settled.token_usage_sources.unknown, 1);
        assert_eq!(settled.cost_usage_sources.unknown, 1);
    }

    #[test]
    fn reservation_overcommit_fails_without_partial_state() {
        let mut ledger = ChildBudgetLedger::new(&request("none"), None, 80, 1_000).unwrap();
        ledger
            .reserve_model_turn("turn-1", 700, 1_000, 1_100)
            .unwrap();
        let before = ledger.snapshot();
        let error = ledger
            .reserve_model_turn("turn-2", 301, 500, 1_200)
            .unwrap_err();
        assert_eq!(error.dimension, Some(BudgetDimension::Tokens));
        assert_eq!(ledger.snapshot(), before);
        assert_eq!(
            ledger
                .reserve_model_turn("turn-1", 1, 1, 1_200)
                .unwrap_err()
                .code,
            "child-budget-operation-duplicate"
        );
        assert_eq!(ledger.snapshot(), before);
    }

    #[test]
    fn concurrency_blocks_temporarily_and_existing_operations_can_finish() {
        let mut request = request("none");
        request.budget.max_concurrency = 1;
        request.budget.max_turns = 1;
        let mut ledger = ChildBudgetLedger::new(&request, None, 80, 1_000).unwrap();
        let reserved = ledger
            .reserve_model_turn("turn-1", 500, 1_000, 1_100)
            .unwrap();
        assert!(reserved
            .saturated_dimensions
            .contains(&BudgetDimension::Concurrency));
        assert!(reserved
            .exhausted_dimensions
            .contains(&BudgetDimension::Turns));
        let error = ledger
            .reserve_tool_call("tool-blocked", false, 1_200)
            .unwrap_err();
        assert_eq!(error.code, "child-budget-concurrency-blocked");
        ledger
            .settle_model_turn(
                "turn-1",
                UsageObservation::authoritative(100),
                UsageObservation::authoritative(100),
                1_300,
            )
            .unwrap();
        let tool = ledger
            .reserve_tool_call("tool-after-turn", false, 1_400)
            .unwrap();
        assert_eq!(tool.active_operations, 1);
        assert_eq!(
            ledger
                .finish_tool_call("tool-after-turn", 1_500)
                .unwrap()
                .active_operations,
            0
        );
    }

    #[test]
    fn tool_and_network_limits_are_independent_and_policy_bound() {
        assert_eq!(
            ChildBudgetLedger::new(&request("allowlisted"), None, 80, 1_000)
                .unwrap_err()
                .code,
            "child-budget-network-limit-required"
        );
        assert_eq!(
            ChildBudgetLedger::new(&request("none"), Some(1), 80, 1_000)
                .unwrap_err()
                .code,
            "child-budget-network-policy-mismatch"
        );

        let mut request = request("allowlisted");
        request.budget.max_tool_calls = 3;
        let mut ledger = ChildBudgetLedger::new(&request, Some(1), 80, 1_000).unwrap();
        ledger.reserve_tool_call("network-1", true, 1_100).unwrap();
        ledger.finish_tool_call("network-1", 1_200).unwrap();
        let before = ledger.snapshot();
        let error = ledger
            .reserve_tool_call("network-2", true, 1_300)
            .unwrap_err();
        assert_eq!(error.dimension, Some(BudgetDimension::NetworkRequests));
        assert_eq!(ledger.snapshot(), before);
        let offline = ledger.reserve_tool_call("offline-1", false, 1_300).unwrap();
        assert_eq!(offline.tool_calls_started, 2);
        ledger.finish_tool_call("offline-1", 1_400).unwrap();
    }

    #[test]
    fn wall_time_clock_and_generation_fail_closed_without_blocking_settlement() {
        let mut request = request("none");
        request.budget.max_wall_time_ms = 100;
        let mut ledger = ChildBudgetLedger::new(&request, None, 80, 1_000).unwrap();
        ledger
            .reserve_model_turn("turn-1", 100, 100, 1_050)
            .unwrap();
        assert_eq!(
            ledger
                .reserve_tool_call("too-late", false, 1_100)
                .unwrap_err()
                .dimension,
            Some(BudgetDimension::WallTimeMs)
        );
        let settled = ledger
            .settle_model_turn(
                "turn-1",
                UsageObservation::authoritative(50),
                UsageObservation::authoritative(50),
                1_150,
            )
            .unwrap();
        assert!(settled
            .exhausted_dimensions
            .contains(&BudgetDimension::WallTimeMs));
        let before = ledger.snapshot();
        assert_eq!(
            ledger.observe(1_149).unwrap_err().code,
            "child-budget-clock-regressed"
        );
        assert_eq!(ledger.snapshot(), before);

        ledger.generation = u64::MAX;
        let before = ledger.snapshot();
        assert_eq!(
            ledger.observe(1_151).unwrap_err().code,
            "child-budget-generation-exhausted"
        );
        assert_eq!(ledger.snapshot(), before);
    }
}
