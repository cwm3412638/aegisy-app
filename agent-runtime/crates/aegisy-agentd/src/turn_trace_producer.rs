//! Runtime-owned construction of metadata-only Codex turn traces.
//!
//! Callers must convert provider/runtime values into bounded safe identities or
//! SHA-256 identities before constructing these inputs. This module accepts no
//! prompt, response, command, path, diff, terminal output, or credential field.
//! The accumulator is created only after Codex has returned the real Turn ID.

use crate::tool_trace_authority::ToolTraceObservation;
use crate::turn_trace::{
    durable_record_payload, AuthorityLabel, CompletionDomain, CompletionEvidence, ErrorClass,
    EvidenceRef, EvidenceSource, ModelReason, ModelRole, RedactionSummary, RuntimeState,
    SessionMode, TerminalEvidence, TerminalState, ToolProviderStatus, ToolState,
    ToolTimelineBinding, TraceBinding, TracePayload, TurnAccess, TurnKind, TurnTrace,
    TurnTraceError, UsageAccounting, UsageAttribution, UsageReportScope, MAX_DURABLE_EVENT_BYTES,
};
use crate::usage_authority::{UsageAuthorityError, UsageAuthorityReport};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};

const INTENT_EVENT_ID: &str = "intent-1";
const RUNTIME_EVENT_ID: &str = "runtime-1";
const MODEL_EVENT_ID: &str = "model-1";
const CONTEXT_EVENT_ID: &str = "context-1";
const USAGE_REPORT_EVENT_ID: &str = "usage-report-1";
const ERROR_EVENT_ID: &str = "error-1";
const TERMINAL_EVENT_ID: &str = "terminal-1";
const RESERVED_DELIVERY_ORDINAL_BASE: u64 = 1_u64 << 63;
const EMERGENCY_STARTED_ORDINAL: u64 = u64::MAX;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RuntimeMetadata {
    pub runtime_identity: String,
    pub adapter_identity: String,
    pub version: String,
    pub state: RuntimeState,
    pub evidence_identity: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct IntentMetadata {
    pub session_mode: SessionMode,
    pub turn_kind: TurnKind,
    pub access: TurnAccess,
    pub intent_identity: String,
    pub evidence_identity: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModelMetadata {
    pub role: ModelRole,
    pub model_identity: String,
    pub provider_identity: String,
    pub reason: ModelReason,
    pub reason_identity: Option<String>,
    pub evidence_source: EvidenceSource,
    pub evidence_identity: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PreparedContextSummary {
    pub manifest_identity: String,
    pub item_count: u32,
    pub included_items: u32,
    pub excluded_items: u32,
    pub bytes: u64,
    pub omitted_fields: u32,
    pub evidence_identity: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ErrorMetadata {
    pub error_identity: String,
    pub stable_class: ErrorClass,
    pub source_class: String,
    pub retryable: bool,
    pub evidence_source: EvidenceSource,
    pub evidence_identity: String,
    pub source_bytes: u64,
    pub redacted_fields: u32,
    pub omitted_fields: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TerminalMetadata {
    pub evidence_source: EvidenceSource,
    pub evidence_identity: String,
}

/// A Provider thread Usage snapshot whose corresponding Timeline Item has
/// already been persisted successfully.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UsageSnapshotMetadata {
    pub persisted_item_id: String,
    pub report: UsageAuthorityReport,
    pub observed_at_ms: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum DeferredObservation {
    Tool(Box<ToolTraceObservation>),
    Usage(UsageSnapshotMetadata),
}

#[derive(Debug)]
struct DurableReservation {
    completed: Option<TurnTrace>,
    failed: TurnTrace,
    interrupted: TurnTrace,
}

impl DeferredObservation {
    fn observed_at_ms(&self) -> u64 {
        match self {
            Self::Tool(observation) => observation.at_ms,
            Self::Usage(snapshot) => snapshot.observed_at_ms,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TurnTraceProducerError {
    pub code: &'static str,
    pub message: &'static str,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ToolObservationAdmission {
    Recorded,
    TerminalPersistenceDenied,
}

impl From<TurnTraceError> for TurnTraceProducerError {
    fn from(value: TurnTraceError) -> Self {
        Self {
            code: value.code,
            message: value.message,
        }
    }
}

impl From<UsageAuthorityError> for TurnTraceProducerError {
    fn from(value: UsageAuthorityError) -> Self {
        Self {
            code: value.code,
            message: value.message,
        }
    }
}

/// An open metadata trace for one real Codex Turn.
///
/// Terminal methods consume the accumulator. This makes a second terminal
/// transition impossible through this API and gives the Store one complete,
/// terminal-last `TurnTrace` value.
#[derive(Debug, Clone)]
pub struct CodexTurnTraceAccumulator {
    trace: TurnTrace,
    session_mode: SessionMode,
    intent_identity: String,
    completion_basis: EvidenceRef,
    tool_observations: Vec<(u64, ToolTraceObservation)>,
    latest_usage_snapshot: Option<(u64, UsageSnapshotMetadata)>,
    next_delivery_ordinal: u64,
    terminal_persistence_denied: BTreeSet<String>,
}

impl CodexTurnTraceAccumulator {
    pub fn started(
        binding: TraceBinding,
        started_at_ms: u64,
        intent: IntentMetadata,
        runtime: RuntimeMetadata,
        model: Option<ModelMetadata>,
        context: PreparedContextSummary,
    ) -> Result<Self, TurnTraceProducerError> {
        let mut trace = TurnTrace::new(binding)?;
        let completion_basis = observed(
            EvidenceSource::Runtime,
            intent.evidence_identity.clone(),
            started_at_ms,
        );
        let intent_identity = intent.intent_identity.clone();
        trace.append(
            INTENT_EVENT_ID.into(),
            started_at_ms,
            TracePayload::Intent {
                session_mode: intent.session_mode,
                turn_kind: intent.turn_kind,
                access: intent.access,
                intent_identity: intent.intent_identity,
                evidence: completion_basis.clone(),
                redaction: RedactionSummary::metadata_only(),
            },
        )?;
        trace.append(
            RUNTIME_EVENT_ID.into(),
            started_at_ms,
            TracePayload::Runtime {
                runtime_identity: runtime.runtime_identity,
                adapter_identity: runtime.adapter_identity,
                version: runtime.version,
                state: runtime.state,
                evidence: observed(
                    EvidenceSource::Runtime,
                    runtime.evidence_identity,
                    started_at_ms,
                ),
                redaction: RedactionSummary::metadata_only(),
            },
        )?;
        if let Some(model) = model {
            trace.append(
                MODEL_EVENT_ID.into(),
                started_at_ms,
                TracePayload::Model {
                    role: model.role,
                    model_identity: model.model_identity,
                    provider_identity: model.provider_identity,
                    reason: model.reason,
                    reason_identity: model.reason_identity,
                    evidence: observed(
                        model.evidence_source,
                        model.evidence_identity,
                        started_at_ms,
                    ),
                    redaction: RedactionSummary::metadata_only(),
                },
            )?;
        }
        trace.append(
            CONTEXT_EVENT_ID.into(),
            started_at_ms,
            TracePayload::Context {
                manifest_identity: context.manifest_identity.clone(),
                item_count: context.item_count,
                included_items: context.included_items,
                excluded_items: context.excluded_items,
                bytes: context.bytes,
                evidence: observed(
                    EvidenceSource::ContextBuilder,
                    context.evidence_identity,
                    started_at_ms,
                ),
                redaction: RedactionSummary {
                    content_included: false,
                    raw_bytes: context.bytes,
                    retained_bytes: 0,
                    redacted_fields: 0,
                    omitted_fields: context.omitted_fields,
                    content_identity: Some(context.manifest_identity),
                },
            },
        )?;
        trace.validate_open()?;
        let accumulator = Self {
            trace,
            session_mode: intent.session_mode,
            intent_identity,
            completion_basis,
            tool_observations: Vec::new(),
            latest_usage_snapshot: None,
            next_delivery_ordinal: 1,
            terminal_persistence_denied: BTreeSet::new(),
        };
        accumulator.validate_durable_reservation(true)?;
        Ok(accumulator)
    }

    #[cfg(test)]
    pub fn open_trace(&self) -> &TurnTrace {
        &self.trace
    }

    /// Replace the retained Provider thread Usage snapshot after its Timeline
    /// Item has committed. Snapshots are absolute and therefore never summed;
    /// the last successful record is the only one emitted at terminal time.
    pub fn record_persisted_usage_snapshot(
        &mut self,
        snapshot: UsageSnapshotMetadata,
    ) -> Result<(), TurnTraceProducerError> {
        let future_ordinals = self.open_tool_action_identities()?.len();
        let (ordinal, next_ordinal) = self.allocate_delivery_ordinal(future_ordinals)?;
        self.validate_deferred_candidate(None, Some((ordinal, snapshot.clone())))?;
        let mut candidate = self.clone();
        candidate.latest_usage_snapshot = Some((ordinal, snapshot));
        candidate.next_delivery_ordinal = next_ordinal;
        candidate.validate_durable_reservation(true)?;
        *self = candidate;
        Ok(())
    }

    /// Retain one provider-observed, content-free Tool lifecycle observation.
    /// Terminal observations are accepted only after Runtime has successfully
    /// committed the corresponding command Timeline Item and supplied its exact
    /// persisted binding.
    pub fn record_tool_observation(
        &mut self,
        observation: ToolTraceObservation,
    ) -> Result<ToolObservationAdmission, TurnTraceProducerError> {
        let (state, action_identity) = tool_observation_state(&observation)?;
        let action_identity = action_identity.to_owned();
        if state == ToolState::Started && !self.terminal_persistence_denied.is_empty() {
            return Err(producer_error(
                "turn-trace-tool-admission-closed",
                "Tool admission is closed after the durable trace budget was exhausted",
            ));
        }
        if state != ToolState::Started
            && self.terminal_persistence_denied.contains(&action_identity)
        {
            return Err(producer_error(
                "turn-trace-tool-terminal-persistence-denied",
                "the Tool terminal Item was denied before persistence by the durable trace budget",
            ));
        }
        let mut open_actions = self.open_tool_action_identities()?;
        if state == ToolState::Started {
            open_actions.insert(action_identity.clone());
        } else {
            open_actions.remove(&action_identity);
        }
        let (ordinal, next_ordinal) = self.allocate_delivery_ordinal(open_actions.len())?;
        self.validate_deferred_candidate(Some((ordinal, observation.clone())), None)?;
        let mut candidate = self.clone();
        candidate.tool_observations.push((ordinal, observation));
        candidate.next_delivery_ordinal = next_ordinal;
        if candidate.validate_durable_reservation(true).is_ok() {
            *self = candidate;
            return Ok(ToolObservationAdmission::Recorded);
        }
        if state != ToolState::Started {
            return Err(producer_error(
                "turn-trace-durable-budget-exhausted",
                "the Tool terminal observation exceeds the reserved durable trace budget",
            ));
        }
        candidate
            .terminal_persistence_denied
            .insert(action_identity);
        candidate.validate_durable_reservation(false)?;
        *self = candidate;
        Ok(ToolObservationAdmission::TerminalPersistenceDenied)
    }

    fn allocate_delivery_ordinal(
        &self,
        future_ordinals: usize,
    ) -> Result<(u64, u64), TurnTraceProducerError> {
        if self.next_delivery_ordinal >= RESERVED_DELIVERY_ORDINAL_BASE {
            return Err(producer_error(
                "turn-trace-delivery-ordinal-exhausted",
                "the Tool and Usage delivery ordinal namespace is exhausted",
            ));
        }
        let next = self.next_delivery_ordinal.checked_add(1).ok_or_else(|| {
            producer_error(
                "turn-trace-delivery-ordinal-exhausted",
                "the Tool and Usage delivery ordinal namespace is exhausted",
            )
        })?;
        let future_ordinals = u64::try_from(future_ordinals).map_err(|_| {
            producer_error(
                "turn-trace-delivery-ordinal-exhausted",
                "the Tool and Usage delivery ordinal namespace is exhausted",
            )
        })?;
        if next
            .checked_add(future_ordinals)
            .is_none_or(|required| required > RESERVED_DELIVERY_ORDINAL_BASE)
        {
            return Err(producer_error(
                "turn-trace-delivery-ordinal-exhausted",
                "the Tool and Usage delivery ordinal namespace cannot cover open Tool terminals",
            ));
        }
        Ok((self.next_delivery_ordinal, next))
    }

    fn open_tool_action_identities(&self) -> Result<BTreeSet<String>, TurnTraceProducerError> {
        let mut open = BTreeSet::new();
        for (_, observation) in &self.tool_observations {
            let (state, action_identity) = tool_observation_state(observation)?;
            if state == ToolState::Started {
                open.insert(action_identity.to_owned());
            } else {
                open.remove(action_identity);
            }
        }
        for denied in &self.terminal_persistence_denied {
            open.remove(denied);
        }
        Ok(open)
    }

    /// Finalize a provider, adapter, transport, policy, tool, or Runtime
    /// failure. `terminal_at_ms` is used for both the Error observation and the
    /// terminal event so callers sample the terminal timestamp exactly once.
    pub fn finalize_failed(
        mut self,
        terminal_at_ms: u64,
        error: ErrorMetadata,
        terminal: TerminalMetadata,
    ) -> Result<TurnTrace, TurnTraceProducerError> {
        self.append_deferred_observations()?;
        self.trace.append(
            ERROR_EVENT_ID.into(),
            terminal_at_ms,
            TracePayload::Error {
                error_identity: error.error_identity,
                stable_class: error.stable_class,
                source_class: error.source_class,
                retryable: error.retryable,
                evidence: observed(
                    error.evidence_source,
                    error.evidence_identity,
                    terminal_at_ms,
                ),
                redaction: RedactionSummary {
                    content_included: false,
                    raw_bytes: error.source_bytes,
                    retained_bytes: 0,
                    redacted_fields: error.redacted_fields,
                    omitted_fields: error.omitted_fields,
                    content_identity: None,
                },
            },
        )?;
        self.append_terminal_event(TerminalState::Failed, terminal_at_ms, terminal, None)
    }

    pub fn finalize_interrupted(
        self,
        terminal_at_ms: u64,
        terminal: TerminalMetadata,
    ) -> Result<TurnTrace, TurnTraceProducerError> {
        self.append_terminal(TerminalState::Interrupted, terminal_at_ms, terminal, None)
    }

    pub fn finalize_completed(
        self,
        terminal_at_ms: u64,
        terminal: TerminalMetadata,
    ) -> Result<TurnTrace, TurnTraceProducerError> {
        let not_applicable = || CompletionDomain::NotApplicable {
            evidence: self.completion_basis.clone(),
        };
        let completion = match self.session_mode {
            SessionMode::Chat => CompletionEvidence {
                intent_identity: self.intent_identity.clone(),
                workspace_change: not_applicable(),
                git_change: not_applicable(),
                verification: not_applicable(),
            },
            SessionMode::Work => CompletionEvidence {
                intent_identity: self.intent_identity.clone(),
                workspace_change: not_applicable(),
                git_change: not_applicable(),
                // The current adapter does not observe every verification
                // producer, so absence of a Test item is not evidence that no
                // verification ran.
                verification: CompletionDomain::Unknown {
                    evidence: unknown(EvidenceSource::Runtime),
                },
            },
        };
        self.append_terminal(
            TerminalState::Completed,
            terminal_at_ms,
            terminal,
            Some(completion),
        )
    }

    fn append_terminal(
        mut self,
        state: TerminalState,
        terminal_at_ms: u64,
        terminal: TerminalMetadata,
        completion: Option<CompletionEvidence>,
    ) -> Result<TurnTrace, TurnTraceProducerError> {
        self.append_deferred_observations()?;
        self.append_terminal_event(state, terminal_at_ms, terminal, completion)
    }

    fn append_terminal_event(
        mut self,
        state: TerminalState,
        terminal_at_ms: u64,
        terminal: TerminalMetadata,
        completion: Option<CompletionEvidence>,
    ) -> Result<TurnTrace, TurnTraceProducerError> {
        self.trace.append(
            TERMINAL_EVENT_ID.into(),
            terminal_at_ms,
            TracePayload::Terminal {
                state,
                evidence: TerminalEvidence {
                    workspace_identity: None,
                    git_state_identity: None,
                    verification_identity: None,
                    observed_verification_count: 0,
                    evidence: observed(
                        terminal.evidence_source,
                        terminal.evidence_identity,
                        terminal_at_ms,
                    ),
                    completion,
                },
                redaction: RedactionSummary::metadata_only(),
            },
        )?;
        self.trace.validate_complete()?;
        Ok(self.trace)
    }

    fn validate_deferred_candidate(
        &self,
        tool: Option<(u64, ToolTraceObservation)>,
        usage: Option<(u64, UsageSnapshotMetadata)>,
    ) -> Result<(), TurnTraceProducerError> {
        let mut candidate = self.trace.clone();
        let mut observations = self.deferred_observations();
        if let Some(tool) = tool {
            observations.push((tool.0, DeferredObservation::Tool(Box::new(tool.1))));
        }
        if let Some(usage) = usage {
            observations.retain(|(_, value)| !matches!(value, DeferredObservation::Usage(_)));
            observations.push((usage.0, DeferredObservation::Usage(usage.1)));
        }
        append_sorted_observations(&mut candidate, observations)?;
        candidate.validate_open()?;
        Ok(())
    }

    fn deferred_observations(&self) -> Vec<(u64, DeferredObservation)> {
        let mut observations = self
            .tool_observations
            .iter()
            .cloned()
            .map(|(ordinal, value)| (ordinal, DeferredObservation::Tool(Box::new(value))))
            .collect::<Vec<_>>();
        if let Some((ordinal, snapshot)) = self.latest_usage_snapshot.as_ref() {
            observations.push((*ordinal, DeferredObservation::Usage(snapshot.clone())));
        }
        observations
    }

    fn append_deferred_observations(&mut self) -> Result<(), TurnTraceProducerError> {
        let observations = self.deferred_observations();
        append_sorted_observations(&mut self.trace, observations)?;
        self.tool_observations.clear();
        self.latest_usage_snapshot = None;
        Ok(())
    }

    fn validate_durable_reservation(
        &self,
        reserve_emergency_started: bool,
    ) -> Result<(), TurnTraceProducerError> {
        let reservation = self.build_durable_reservation(reserve_emergency_started)?;
        if let Some(completed) = reservation.completed.as_ref() {
            validate_durable_trace_size(completed, "completed")?;
        }
        validate_durable_trace_size(&reservation.failed, "failed")?;
        validate_durable_trace_size(&reservation.interrupted, "interrupted")
    }

    fn build_durable_reservation(
        &self,
        reserve_emergency_started: bool,
    ) -> Result<DurableReservation, TurnTraceProducerError> {
        let mut reserved = self.clone();
        let mut starts = BTreeMap::<String, ToolTraceObservation>::new();
        let mut terminals = BTreeSet::<String>::new();
        let mut occupied_actions = BTreeSet::<String>::new();
        for (_, observation) in &reserved.tool_observations {
            let (state, action_identity) = tool_observation_state(observation)?;
            occupied_actions.insert(action_identity.to_owned());
            if state == ToolState::Started {
                starts.insert(action_identity.to_owned(), observation.clone());
            } else {
                terminals.insert(action_identity.to_owned());
            }
        }
        let mut reservation_ordinal = RESERVED_DELIVERY_ORDINAL_BASE;
        for (action_identity, started) in starts {
            if terminals.contains(&action_identity)
                || reserved
                    .terminal_persistence_denied
                    .contains(&action_identity)
            {
                continue;
            }
            reserved.tool_observations.push((
                reservation_ordinal,
                reserved_terminal_observation(&started)?,
            ));
            reservation_ordinal = reservation_ordinal.checked_add(1).ok_or_else(|| {
                producer_error(
                    "turn-trace-reservation-ordinal-exhausted",
                    "the durable Tool reservation ordinal namespace is exhausted",
                )
            })?;
            if reservation_ordinal == EMERGENCY_STARTED_ORDINAL {
                return Err(producer_error(
                    "turn-trace-reservation-ordinal-exhausted",
                    "the durable Tool reservation ordinal namespace is exhausted",
                ));
            }
        }

        let completed = reserved
            .terminal_persistence_denied
            .is_empty()
            .then(|| {
                reserved
                    .clone()
                    .finalize_completed(u64::MAX, maximal_terminal_metadata())
            })
            .transpose()?;

        let mut failed = reserved.clone();
        if reserve_emergency_started && reserved.terminal_persistence_denied.is_empty() {
            failed.tool_observations.push((
                EMERGENCY_STARTED_ORDINAL,
                emergency_started_observation(&failed.trace, u64::MAX, &occupied_actions)?,
            ));
        }
        let failed = failed.finalize_failed(
            u64::MAX,
            maximal_error_metadata(),
            maximal_terminal_metadata(),
        )?;

        let mut interrupted = reserved;
        if reserve_emergency_started && interrupted.terminal_persistence_denied.is_empty() {
            interrupted.tool_observations.push((
                EMERGENCY_STARTED_ORDINAL,
                emergency_started_observation(&interrupted.trace, u64::MAX, &occupied_actions)?,
            ));
        }
        let interrupted =
            interrupted.finalize_interrupted(u64::MAX, maximal_terminal_metadata())?;
        Ok(DurableReservation {
            completed,
            failed,
            interrupted,
        })
    }
}

fn tool_observation_state(
    observation: &ToolTraceObservation,
) -> Result<(ToolState, &str), TurnTraceProducerError> {
    match &observation.payload {
        TracePayload::Tool {
            state,
            action_identity,
            ..
        } => Ok((*state, action_identity)),
        _ => Err(producer_error(
            "turn-trace-tool-observation-invalid",
            "a Tool observation must contain a Tool payload",
        )),
    }
}

fn reserved_terminal_observation(
    started: &ToolTraceObservation,
) -> Result<ToolTraceObservation, TurnTraceProducerError> {
    let TracePayload::Tool {
        tool_identity,
        action_identity,
        source,
        input_identity,
        ..
    } = &started.payload
    else {
        return Err(producer_error(
            "turn-trace-tool-observation-invalid",
            "a Tool reservation requires a Tool start payload",
        ));
    };
    let duration_ms = u64::MAX.saturating_sub(started.at_ms).min(i64::MAX as u64);
    Ok(ToolTraceObservation {
        at_ms: u64::MAX,
        payload: TracePayload::Tool {
            tool_identity: tool_identity.clone(),
            action_identity: action_identity.clone(),
            state: ToolState::Failed,
            provider_status: Some(ToolProviderStatus::Failed),
            source: *source,
            input_identity: input_identity.clone(),
            output_identity: Some(maximal_hash_identity('b')),
            item_binding: Some(ToolTimelineBinding::Persisted {
                item_identity: maximal_hash_identity('c'),
                payload_identity: maximal_hash_identity('d'),
            }),
            duration_ms: Some(duration_ms),
            exit_code: Some(i32::MIN.into()),
            evidence: observed(
                EvidenceSource::ToolRuntime,
                maximal_hash_identity('e'),
                u64::MAX,
            ),
            redaction: RedactionSummary::metadata_only(),
        },
    })
}

fn emergency_started_observation(
    trace: &TurnTrace,
    at_ms: u64,
    occupied_actions: &BTreeSet<String>,
) -> Result<ToolTraceObservation, TurnTraceProducerError> {
    let action_identity = (0..=occupied_actions.len())
        .map(|nonce| emergency_action_identity(trace, nonce as u64))
        .find(|identity| !occupied_actions.contains(identity))
        .ok_or_else(|| {
            producer_error(
                "turn-trace-emergency-action-exhausted",
                "a collision-free emergency Tool action identity is unavailable",
            )
        })?;
    Ok(ToolTraceObservation {
        at_ms,
        payload: TracePayload::Tool {
            tool_identity: maximal_hash_identity('f'),
            action_identity,
            state: ToolState::Started,
            provider_status: Some(ToolProviderStatus::InProgress),
            source: Some(crate::turn_trace::ToolSource::UnifiedExecInteraction),
            input_identity: Some(maximal_hash_identity('a')),
            output_identity: None,
            item_binding: Some(ToolTimelineBinding::NotPersisted),
            duration_ms: None,
            exit_code: None,
            evidence: observed(
                EvidenceSource::ToolRuntime,
                maximal_hash_identity('9'),
                at_ms,
            ),
            redaction: RedactionSummary::metadata_only(),
        },
    })
}

fn emergency_action_identity(trace: &TurnTrace, nonce: u64) -> String {
    let mut digest = Sha256::new();
    digest.update(b"aegisy-turn-trace-emergency-tool/0.2\0");
    digest.update(trace.binding.session_id.as_bytes());
    digest.update([0]);
    digest.update(trace.binding.turn_id.as_bytes());
    digest.update(nonce.to_be_bytes());
    format!("sha256:{:x}", digest.finalize())
}

fn maximal_error_metadata() -> ErrorMetadata {
    ErrorMetadata {
        error_identity: maximal_identity('1'),
        stable_class: ErrorClass::Transport,
        source_class: "x".repeat(96),
        retryable: false,
        evidence_source: EvidenceSource::ToolRuntime,
        evidence_identity: maximal_identity('2'),
        source_bytes: 16 * 1024 * 1024,
        redacted_fields: u32::MAX,
        omitted_fields: u32::MAX,
    }
}

fn maximal_terminal_metadata() -> TerminalMetadata {
    TerminalMetadata {
        evidence_source: EvidenceSource::ApprovalAuthority,
        evidence_identity: maximal_identity('3'),
    }
}

fn maximal_identity(byte: char) -> String {
    byte.to_string().repeat(128)
}

fn maximal_hash_identity(byte: char) -> String {
    format!("sha256:{}", byte.to_string().repeat(64))
}

fn validate_durable_trace_size(
    trace: &TurnTrace,
    state: &str,
) -> Result<(), TurnTraceProducerError> {
    let trace_identity = trace.metadata_identity()?;
    let payload = durable_record_payload(trace, &trace_identity, state, u64::MAX);
    let bytes = serde_json::to_vec(&payload).map_err(|_| {
        producer_error(
            "turn-trace-durable-payload-invalid",
            "the durable turn trace payload cannot be serialized",
        )
    })?;
    if bytes.len() > MAX_DURABLE_EVENT_BYTES {
        return Err(producer_error(
            "turn-trace-durable-budget-exhausted",
            "the durable turn trace payload exceeds the Workbench event limit",
        ));
    }
    Ok(())
}

fn producer_error(code: &'static str, message: &'static str) -> TurnTraceProducerError {
    TurnTraceProducerError { code, message }
}

fn append_sorted_observations(
    trace: &mut TurnTrace,
    mut observations: Vec<(u64, DeferredObservation)>,
) -> Result<(), TurnTraceProducerError> {
    let mut ordinals = BTreeSet::new();
    if observations
        .iter()
        .any(|(ordinal, _)| !ordinals.insert(*ordinal))
    {
        return Err(producer_error(
            "turn-trace-delivery-ordinal-duplicate",
            "Tool and Usage observations must have unique delivery ordinals",
        ));
    }
    observations.sort_by_key(|(ordinal, observation)| (observation.observed_at_ms(), *ordinal));
    for (ordinal, observation) in observations {
        match observation {
            DeferredObservation::Tool(observation) => {
                let observation = *observation;
                trace.append(
                    format!("tool-{ordinal}"),
                    observation.at_ms,
                    observation.payload,
                )?;
            }
            DeferredObservation::Usage(snapshot) => {
                trace.append(
                    USAGE_REPORT_EVENT_ID.into(),
                    snapshot.observed_at_ms,
                    usage_report_payload(&snapshot)?,
                )?;
            }
        }
    }
    Ok(())
}

fn usage_report_payload(
    snapshot: &UsageSnapshotMetadata,
) -> Result<TracePayload, TurnTraceProducerError> {
    let report_identity = snapshot.report.metadata_identity()?;
    Ok(TracePayload::UsageReport {
        report_identity: report_identity.clone(),
        persisted_item_id: snapshot.persisted_item_id.clone(),
        scope: UsageReportScope::ProviderThread,
        accounting: UsageAccounting::AbsoluteSnapshot,
        attempt_attribution: UsageAttribution::Unavailable,
        retry_attribution: UsageAttribution::Unavailable,
        report: snapshot.report.clone(),
        evidence: observed(
            EvidenceSource::UsageProvider,
            report_identity,
            snapshot.observed_at_ms,
        ),
        redaction: RedactionSummary::metadata_only(),
    })
}

fn observed(source: EvidenceSource, identity: String, observed_at_ms: u64) -> EvidenceRef {
    EvidenceRef {
        authority: AuthorityLabel::Observed,
        source,
        identity: Some(identity),
        observed_at_ms: Some(observed_at_ms),
    }
}

fn unknown(source: EvidenceSource) -> EvidenceRef {
    EvidenceRef {
        authority: AuthorityLabel::Unknown,
        source,
        identity: None,
        observed_at_ms: None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::turn_trace::{ToolProviderStatus, ToolSource, ToolState, ToolTimelineBinding};

    fn hash(byte: char) -> String {
        format!("sha256:{}", byte.to_string().repeat(64))
    }

    fn binding() -> TraceBinding {
        TraceBinding {
            session_id: "session-1".into(),
            turn_id: "turn-1".into(),
            project_id: Some("project-1".into()),
            environment_identity: Some(format!("environment:sha256:{}", "a".repeat(64))),
        }
    }

    fn chat_binding() -> TraceBinding {
        TraceBinding {
            session_id: "chat-session-1".into(),
            turn_id: "chat-turn-1".into(),
            project_id: None,
            environment_identity: Some(format!("environment:sha256:{}", "9".repeat(64))),
        }
    }

    fn work_intent() -> IntentMetadata {
        IntentMetadata {
            session_mode: SessionMode::Work,
            turn_kind: TurnKind::ReadOnlyInspection,
            access: TurnAccess::ReadOnly,
            intent_identity: hash('a'),
            evidence_identity: hash('b'),
        }
    }

    fn chat_intent() -> IntentMetadata {
        IntentMetadata {
            session_mode: SessionMode::Chat,
            turn_kind: TurnKind::Conversation,
            access: TurnAccess::NonMutating,
            intent_identity: hash('8'),
            evidence_identity: hash('9'),
        }
    }

    fn runtime() -> RuntimeMetadata {
        RuntimeMetadata {
            runtime_identity: "aegisy-agentd:0.1.0".into(),
            adapter_identity: "codex-app-server".into(),
            version: "0.144.5".into(),
            state: RuntimeState::Ready,
            evidence_identity: hash('b'),
        }
    }

    fn model() -> ModelMetadata {
        ModelMetadata {
            role: ModelRole::Agent,
            model_identity: hash('c'),
            provider_identity: hash('d'),
            reason: ModelReason::Continuation,
            reason_identity: Some(hash('e')),
            evidence_source: EvidenceSource::Runtime,
            evidence_identity: hash('f'),
        }
    }

    fn context() -> PreparedContextSummary {
        PreparedContextSummary {
            manifest_identity: hash('1'),
            item_count: 3,
            included_items: 2,
            excluded_items: 1,
            bytes: 256,
            omitted_fields: 3,
            evidence_identity: hash('2'),
        }
    }

    fn accumulator() -> CodexTurnTraceAccumulator {
        CodexTurnTraceAccumulator::started(
            binding(),
            10,
            work_intent(),
            runtime(),
            Some(model()),
            context(),
        )
        .unwrap()
    }

    fn chat_accumulator() -> CodexTurnTraceAccumulator {
        CodexTurnTraceAccumulator::started(
            chat_binding(),
            10,
            chat_intent(),
            runtime(),
            Some(model()),
            context(),
        )
        .unwrap()
    }

    fn terminal() -> TerminalMetadata {
        TerminalMetadata {
            evidence_source: EvidenceSource::Provider,
            evidence_identity: hash('3'),
        }
    }

    fn usage_snapshot(
        persisted_item_id: &str,
        observed_at_ms: u64,
        input_tokens: u64,
    ) -> UsageSnapshotMetadata {
        let usage = serde_json::json!({
            "last": {
                "cached_input_tokens": 0,
                "input_tokens": input_tokens,
                "output_tokens": 8,
                "reasoning_output_tokens": 4,
                "total_tokens": input_tokens + 8
            },
            "total": {
                "cached_input_tokens": 0,
                "input_tokens": input_tokens,
                "output_tokens": 8,
                "reasoning_output_tokens": 4,
                "total_tokens": input_tokens + 8
            },
            "model_context_window": 128000
        });
        UsageSnapshotMetadata {
            persisted_item_id: persisted_item_id.into(),
            report: crate::usage_authority::from_provider_token_usage(&usage, observed_at_ms)
                .unwrap(),
            observed_at_ms,
        }
    }

    fn started_tool(action: char, observed_at_ms: u64) -> ToolTraceObservation {
        ToolTraceObservation {
            at_ms: observed_at_ms,
            payload: TracePayload::Tool {
                tool_identity: hash('5'),
                action_identity: hash(action),
                state: ToolState::Started,
                provider_status: Some(ToolProviderStatus::InProgress),
                source: Some(ToolSource::Agent),
                input_identity: Some(hash('7')),
                output_identity: None,
                item_binding: Some(ToolTimelineBinding::NotPersisted),
                duration_ms: None,
                exit_code: None,
                evidence: observed(EvidenceSource::ToolRuntime, hash('8'), observed_at_ms),
                redaction: RedactionSummary::metadata_only(),
            },
        }
    }

    fn completed_tool(action: char, observed_at_ms: u64) -> ToolTraceObservation {
        ToolTraceObservation {
            at_ms: observed_at_ms,
            payload: TracePayload::Tool {
                tool_identity: hash('5'),
                action_identity: hash(action),
                state: ToolState::Completed,
                provider_status: Some(ToolProviderStatus::Completed),
                source: Some(ToolSource::Agent),
                input_identity: Some(hash('7')),
                output_identity: Some(hash('9')),
                item_binding: Some(ToolTimelineBinding::Persisted {
                    item_identity: hash('a'),
                    payload_identity: hash('a'),
                }),
                duration_ms: Some(4),
                exit_code: Some(0),
                evidence: observed(EvidenceSource::ToolRuntime, hash('b'), observed_at_ms),
                redaction: RedactionSummary::metadata_only(),
            },
        }
    }

    fn indexed_tool_observation(
        index: usize,
        observed_at_ms: u64,
        state: ToolState,
    ) -> ToolTraceObservation {
        let identity = |namespace: usize| format!("sha256:{:064x}", namespace + index);
        let terminal = state != ToolState::Started;
        ToolTraceObservation {
            at_ms: observed_at_ms,
            payload: TracePayload::Tool {
                tool_identity: identity(1_000),
                action_identity: identity(2_000),
                state,
                provider_status: Some(if terminal {
                    ToolProviderStatus::Completed
                } else {
                    ToolProviderStatus::InProgress
                }),
                source: Some(crate::turn_trace::ToolSource::Agent),
                input_identity: Some(identity(3_000)),
                output_identity: terminal.then(|| identity(4_000)),
                item_binding: Some(if terminal {
                    ToolTimelineBinding::Persisted {
                        item_identity: identity(5_000),
                        payload_identity: identity(6_000),
                    }
                } else {
                    ToolTimelineBinding::NotPersisted
                }),
                duration_ms: terminal.then_some(1),
                exit_code: terminal.then_some(0),
                evidence: observed(EvidenceSource::ToolRuntime, identity(7_000), observed_at_ms),
                redaction: RedactionSummary::metadata_only(),
            },
        }
    }

    fn transport_error() -> ErrorMetadata {
        ErrorMetadata {
            error_identity: hash('4'),
            stable_class: ErrorClass::Transport,
            source_class: "response-stream-disconnected".into(),
            retryable: true,
            evidence_source: EvidenceSource::Provider,
            evidence_identity: hash('5'),
            source_bytes: 512,
            redacted_fields: 1,
            omitted_fields: 2,
        }
    }

    fn assert_content_free(trace: &TurnTrace) {
        let serialized = serde_json::to_string(trace).unwrap();
        for forbidden in [
            "private prompt",
            "private response",
            "\"command\":",
            "\"cwd\":",
            "\"path\":",
            "\"diff\":",
            "\"output\":",
            "credential",
            "\"authorization\":",
            "\"process_id\":",
            "Bearer",
            "private provider body",
        ] {
            assert!(!serialized.contains(forbidden), "found {forbidden}");
        }
        assert!(serialized.contains("\"content_included\":false"));
    }

    fn durable_payload_bytes(trace: &TurnTrace, state: &str) -> usize {
        let identity = trace.metadata_identity().unwrap();
        serde_json::to_vec(&durable_record_payload(trace, &identity, state, u64::MAX))
            .unwrap()
            .len()
    }

    fn assert_unique_event_ids(trace: &TurnTrace) {
        let ids = trace
            .events
            .iter()
            .map(|event| event.event_id.as_str())
            .collect::<BTreeSet<_>>();
        assert_eq!(ids.len(), trace.events.len());
    }

    #[test]
    fn durable_reservation_uses_the_longest_legal_serialized_sources_and_error_class() {
        let evidence_sources = [
            EvidenceSource::Runtime,
            EvidenceSource::ModelCatalog,
            EvidenceSource::Provider,
            EvidenceSource::ContextBuilder,
            EvidenceSource::ToolRuntime,
            EvidenceSource::ApprovalAuthority,
            EvidenceSource::UsageProvider,
            EvidenceSource::Workspace,
            EvidenceSource::Git,
            EvidenceSource::TestRunner,
            EvidenceSource::Aap,
            EvidenceSource::LocalRuntime,
        ];
        let reserved_source_bytes = serde_json::to_vec(&EvidenceSource::ApprovalAuthority)
            .unwrap()
            .len();
        assert!(evidence_sources
            .iter()
            .all(|source| serde_json::to_vec(source).unwrap().len() <= reserved_source_bytes));

        let error_classes = [
            ErrorClass::Runtime,
            ErrorClass::Protocol,
            ErrorClass::Provider,
            ErrorClass::Transport,
            ErrorClass::Timeout,
            ErrorClass::Sandbox,
            ErrorClass::Policy,
            ErrorClass::Tool,
            ErrorClass::Storage,
            ErrorClass::Workspace,
            ErrorClass::Git,
            ErrorClass::Budget,
            ErrorClass::Unknown,
        ];
        let reserved_class_bytes = serde_json::to_vec(&ErrorClass::Transport).unwrap().len();
        assert!(error_classes
            .iter()
            .all(|class| serde_json::to_vec(class).unwrap().len() <= reserved_class_bytes));
        assert_eq!(
            maximal_terminal_metadata().evidence_source,
            EvidenceSource::ApprovalAuthority
        );
        let error_sources = [
            EvidenceSource::Aap,
            EvidenceSource::Runtime,
            EvidenceSource::Provider,
            EvidenceSource::ToolRuntime,
        ];
        let reserved_error_source_bytes = serde_json::to_vec(&EvidenceSource::ToolRuntime)
            .unwrap()
            .len();
        assert!(error_sources.iter().all(|source| {
            serde_json::to_vec(source).unwrap().len() <= reserved_error_source_bytes
        }));
        assert_eq!(
            maximal_error_metadata().evidence_source,
            EvidenceSource::ToolRuntime
        );
        assert_eq!(maximal_error_metadata().error_identity.len(), 128);
        assert_eq!(maximal_error_metadata().evidence_identity.len(), 128);
        assert_eq!(maximal_terminal_metadata().evidence_identity.len(), 128);
        assert_eq!(maximal_hash_identity('0').len(), 71);
    }

    #[test]
    fn turn_started_records_intent_runtime_model_and_context_metadata() {
        let accumulator = accumulator();
        let trace = accumulator.open_trace();
        trace.validate_open().unwrap();
        assert_eq!(trace.events.len(), 4);
        assert_eq!(trace.events[0].sequence, 1);
        assert_eq!(trace.events[1].sequence, 2);
        assert_eq!(trace.events[2].sequence, 3);
        assert_eq!(trace.events[3].sequence, 4);
        assert!(matches!(
            trace.events[0].payload,
            TracePayload::Intent {
                session_mode: SessionMode::Work,
                turn_kind: TurnKind::ReadOnlyInspection,
                access: TurnAccess::ReadOnly,
                ..
            }
        ));
        assert!(matches!(
            trace.events[1].payload,
            TracePayload::Runtime { .. }
        ));
        assert!(matches!(
            trace.events[2].payload,
            TracePayload::Model { .. }
        ));
        assert!(matches!(
            trace.events[3].payload,
            TracePayload::Context { .. }
        ));
        assert!(trace.events.iter().all(|event| event.at_ms == 10));
    }

    #[test]
    fn no_persisted_usage_snapshot_emits_no_usage_report() {
        let trace = accumulator().finalize_completed(20, terminal()).unwrap();
        assert_eq!(trace.schema_version, "turn-trace/0.4");
        assert!(trace
            .events
            .iter()
            .all(|event| !matches!(event.payload, TracePayload::UsageReport { .. })));
    }

    #[test]
    fn tool_and_usage_observations_are_sorted_by_time_then_delivery_order() {
        let mut accumulator = accumulator();
        accumulator
            .record_tool_observation(started_tool('6', 12))
            .unwrap();
        accumulator
            .record_persisted_usage_snapshot(usage_snapshot("item-usage", 18, 24))
            .unwrap();
        accumulator
            .record_tool_observation(completed_tool('6', 16))
            .unwrap();

        let trace = accumulator.finalize_completed(20, terminal()).unwrap();
        assert_eq!(trace.events[4].event_id, "tool-1");
        assert_eq!(trace.events[4].at_ms, 12);
        assert_eq!(trace.events[5].event_id, "tool-3");
        assert_eq!(trace.events[5].at_ms, 16);
        assert_eq!(trace.events[6].event_id, USAGE_REPORT_EVENT_ID);
        assert_eq!(trace.events[6].at_ms, 18);
        assert!(matches!(
            trace.events[7].payload,
            TracePayload::Terminal {
                state: TerminalState::Completed,
                ..
            }
        ));
        assert_content_free(&trace);
    }

    #[test]
    fn durable_budget_keeps_emergency_started_and_rejects_its_terminal_before_persistence() {
        let mut accumulator = accumulator();
        let mut maximal_completed = accumulator.clone();
        let mut completed_actions = 0_usize;
        loop {
            let started_at_ms = 12 + completed_actions as u64 * 2;
            match accumulator
                .record_tool_observation(indexed_tool_observation(
                    completed_actions,
                    started_at_ms,
                    ToolState::Started,
                ))
                .unwrap()
            {
                ToolObservationAdmission::Recorded => {
                    assert_eq!(
                        accumulator
                            .record_tool_observation(indexed_tool_observation(
                                completed_actions,
                                started_at_ms + 1,
                                ToolState::Completed,
                            ))
                            .unwrap(),
                        ToolObservationAdmission::Recorded
                    );
                    completed_actions += 1;
                    maximal_completed = accumulator.clone();
                }
                ToolObservationAdmission::TerminalPersistenceDenied => break,
            }
        }
        assert!((20..128).contains(&completed_actions));

        let mut terminal_attempt = accumulator.clone();
        let cause = terminal_attempt
            .record_tool_observation(indexed_tool_observation(
                completed_actions,
                13 + completed_actions as u64 * 2,
                ToolState::Completed,
            ))
            .unwrap_err();
        assert_eq!(cause.code, "turn-trace-tool-terminal-persistence-denied");

        let reservation = maximal_completed.build_durable_reservation(true).unwrap();
        let completed = reservation
            .completed
            .as_ref()
            .expect("a reservation without denied terminals must cover completion");
        for (trace, state) in [
            (completed, "completed"),
            (&reservation.failed, "failed"),
            (&reservation.interrupted, "interrupted"),
        ] {
            trace.validate_complete().unwrap();
            assert_unique_event_ids(trace);
            let bytes = durable_payload_bytes(trace, state);
            assert!(
                bytes <= MAX_DURABLE_EVENT_BYTES,
                "reserved {state} payload is {bytes} bytes"
            );
        }
        assert!(reservation.failed.events.iter().any(|event| {
            event.event_id == format!("tool-{EMERGENCY_STARTED_ORDINAL}")
                && matches!(
                    event.payload,
                    TracePayload::Tool {
                        state: ToolState::Started,
                        ..
                    }
                )
        }));

        let denied_reservation = accumulator.build_durable_reservation(false).unwrap();
        assert!(denied_reservation.completed.is_none());
        for (trace, state) in [
            (&denied_reservation.failed, "failed"),
            (&denied_reservation.interrupted, "interrupted"),
        ] {
            assert_unique_event_ids(trace);
            assert!(durable_payload_bytes(trace, state) <= MAX_DURABLE_EVENT_BYTES);
        }

        let trace = accumulator
            .finalize_failed(
                u64::MAX,
                maximal_error_metadata(),
                maximal_terminal_metadata(),
            )
            .unwrap();
        let bytes = durable_payload_bytes(&trace, "failed");
        assert!(
            bytes <= MAX_DURABLE_EVENT_BYTES,
            "reserved payload is {bytes} bytes"
        );
        assert!(matches!(
            trace.events[trace.events.len() - 3].payload,
            TracePayload::Tool {
                state: ToolState::Started,
                ..
            }
        ));
    }

    #[test]
    fn emergency_started_reservation_avoids_a_real_action_identity_collision() {
        let mut accumulator = accumulator();
        let colliding_identity = emergency_action_identity(&accumulator.trace, 0);
        let mut real_started = started_tool('6', 12);
        let TracePayload::Tool {
            action_identity, ..
        } = &mut real_started.payload
        else {
            unreachable!("the helper always returns a Tool observation")
        };
        *action_identity = colliding_identity.clone();
        assert_eq!(
            accumulator.record_tool_observation(real_started).unwrap(),
            ToolObservationAdmission::Recorded
        );

        let occupied = BTreeSet::from([colliding_identity]);
        let emergency =
            emergency_started_observation(&accumulator.trace, u64::MAX, &occupied).unwrap();
        let TracePayload::Tool {
            action_identity, ..
        } = emergency.payload
        else {
            unreachable!("the emergency reservation is always a Tool observation")
        };
        assert!(!occupied.contains(&action_identity));
        accumulator.validate_durable_reservation(true).unwrap();
    }

    #[test]
    fn delivery_ordinals_are_unique_and_cannot_enter_the_reservation_namespace() {
        let mut boundary = accumulator();
        boundary.next_delivery_ordinal = RESERVED_DELIVERY_ORDINAL_BASE - 2;
        let started_at_ms = 12;
        assert_eq!(
            boundary
                .record_tool_observation(started_tool('6', started_at_ms))
                .unwrap(),
            ToolObservationAdmission::Recorded
        );
        let completed = completed_tool('6', started_at_ms + 4);
        assert_eq!(
            boundary.record_tool_observation(completed.clone()).unwrap(),
            ToolObservationAdmission::Recorded
        );
        let trace = boundary.finalize_completed(20, terminal()).unwrap();
        assert!(trace.events.iter().any(|event| {
            event.event_id == format!("tool-{}", RESERVED_DELIVERY_ORDINAL_BASE - 2)
        }));
        assert!(trace.events.iter().any(|event| {
            event.event_id == format!("tool-{}", RESERVED_DELIVERY_ORDINAL_BASE - 1)
        }));
        assert_unique_event_ids(&trace);

        let mut exhausted = accumulator();
        exhausted.next_delivery_ordinal = RESERVED_DELIVERY_ORDINAL_BASE - 1;
        let before = exhausted.clone();
        let cause = exhausted
            .record_tool_observation(started_tool('6', 12))
            .unwrap_err();
        assert_eq!(cause.code, "turn-trace-delivery-ordinal-exhausted");
        assert_eq!(exhausted.tool_observations, before.tool_observations);
        assert_eq!(
            exhausted.next_delivery_ordinal,
            before.next_delivery_ordinal
        );

        let mut duplicate_target = accumulator().trace;
        let duplicate_error = append_sorted_observations(
            &mut duplicate_target,
            vec![
                (
                    7,
                    DeferredObservation::Tool(Box::new(started_tool('6', 12))),
                ),
                (
                    7,
                    DeferredObservation::Usage(usage_snapshot("item-usage", 13, 24)),
                ),
            ],
        )
        .unwrap_err();
        assert_eq!(
            duplicate_error.code,
            "turn-trace-delivery-ordinal-duplicate"
        );
        assert_eq!(duplicate_target.events.len(), 4);
    }

    #[test]
    fn completed_turn_rejects_started_only_tool_but_interruption_preserves_it() {
        let mut completed = accumulator();
        completed
            .record_tool_observation(started_tool('6', 12))
            .unwrap();
        let error = completed.finalize_completed(20, terminal()).unwrap_err();
        assert_eq!(error.code, "turn-trace-tool-incomplete-on-terminal");

        let mut interrupted = accumulator();
        interrupted
            .record_tool_observation(started_tool('6', 12))
            .unwrap();
        let trace = interrupted.finalize_interrupted(20, terminal()).unwrap();
        assert!(matches!(
            trace.events[4].payload,
            TracePayload::Tool {
                state: ToolState::Started,
                ..
            }
        ));
        trace.validate_complete().unwrap();
    }

    #[test]
    fn terminal_tool_without_started_observation_is_rejected_before_mutation() {
        let mut accumulator = accumulator();
        let error = accumulator
            .record_tool_observation(completed_tool('6', 16))
            .unwrap_err();
        assert_eq!(error.code, "turn-trace-tool-terminal-without-start");
        assert_eq!(accumulator.open_trace().events.len(), 4);
    }

    #[test]
    fn repeated_absolute_usage_snapshots_retain_only_the_last_persisted_item() {
        let mut accumulator = accumulator();
        accumulator
            .record_persisted_usage_snapshot(usage_snapshot("item-usage-1", 12, 24))
            .unwrap();
        accumulator
            .record_persisted_usage_snapshot(usage_snapshot("item-usage-2", 15, 41))
            .unwrap();

        let trace = accumulator.finalize_completed(20, terminal()).unwrap();
        let usage_events = trace
            .events
            .iter()
            .filter_map(|event| match &event.payload {
                TracePayload::UsageReport {
                    persisted_item_id,
                    scope,
                    accounting,
                    attempt_attribution,
                    retry_attribution,
                    report,
                    ..
                } => Some((
                    event,
                    persisted_item_id,
                    scope,
                    accounting,
                    attempt_attribution,
                    retry_attribution,
                    report,
                )),
                _ => None,
            })
            .collect::<Vec<_>>();
        assert_eq!(usage_events.len(), 1);
        let (event, item_id, scope, accounting, attempt, retry, report) = usage_events[0];
        assert_eq!(event.at_ms, 15);
        assert_eq!(item_id, "item-usage-2");
        assert_eq!(*scope, UsageReportScope::ProviderThread);
        assert_eq!(*accounting, UsageAccounting::AbsoluteSnapshot);
        assert_eq!(*attempt, UsageAttribution::Unavailable);
        assert_eq!(*retry, UsageAttribution::Unavailable);
        let token = report
            .entry(crate::usage_authority::UsageMetric::Token)
            .unwrap();
        let Some(crate::usage_authority::UsageValue::Token(value)) = &token.value else {
            panic!("expected final token snapshot")
        };
        assert_eq!(value.input_tokens, Some(41));
    }

    #[test]
    fn completed_failed_and_interrupted_traces_retain_usage_before_error_and_terminal() {
        let mut completed = accumulator();
        completed
            .record_persisted_usage_snapshot(usage_snapshot("item-completed", 14, 30))
            .unwrap();
        let completed = completed.finalize_completed(20, terminal()).unwrap();

        let mut failed = accumulator();
        failed
            .record_persisted_usage_snapshot(usage_snapshot("item-failed", 15, 31))
            .unwrap();
        let failed = failed
            .finalize_failed(21, transport_error(), terminal())
            .unwrap();

        let mut interrupted = accumulator();
        interrupted
            .record_persisted_usage_snapshot(usage_snapshot("item-interrupted", 16, 32))
            .unwrap();
        let interrupted = interrupted.finalize_interrupted(22, terminal()).unwrap();

        for trace in [&completed, &failed, &interrupted] {
            assert_eq!(
                trace
                    .events
                    .iter()
                    .filter(|event| matches!(event.payload, TracePayload::UsageReport { .. }))
                    .count(),
                1
            );
            assert!(matches!(
                trace.events.last().unwrap().payload,
                TracePayload::Terminal { .. }
            ));
        }
        for trace in [&completed, &interrupted] {
            assert!(matches!(
                trace.events[trace.events.len() - 2].payload,
                TracePayload::UsageReport { .. }
            ));
        }
        assert!(matches!(
            failed.events[failed.events.len() - 3].payload,
            TracePayload::UsageReport { .. }
        ));
        assert!(matches!(
            failed.events[failed.events.len() - 2].payload,
            TracePayload::Error { .. }
        ));
    }

    #[test]
    fn failure_records_error_then_one_terminal_with_one_timestamp() {
        let trace = accumulator()
            .finalize_failed(20, transport_error(), terminal())
            .unwrap();
        trace.validate_complete().unwrap();
        assert_eq!(trace.events.len(), 6);
        assert_eq!(trace.events[4].at_ms, 20);
        assert_eq!(trace.events[5].at_ms, 20);
        assert!(matches!(
            trace.events[4].payload,
            TracePayload::Error { .. }
        ));
        assert!(matches!(
            trace.events[5].payload,
            TracePayload::Terminal {
                state: TerminalState::Failed,
                ..
            }
        ));
        assert_eq!(
            trace
                .events
                .iter()
                .filter(|event| matches!(event.payload, TracePayload::Terminal { .. }))
                .count(),
            1
        );
        let TracePayload::Terminal { evidence, .. } = &trace.events[5].payload else {
            panic!("failed trace must end with terminal evidence")
        };
        assert_eq!(evidence.completion, None);
    }

    #[test]
    fn interruption_is_terminal_last_without_fabricated_error_or_completion_evidence() {
        let trace = accumulator().finalize_interrupted(21, terminal()).unwrap();
        assert_eq!(trace.events.len(), 5);
        let TracePayload::Terminal {
            state, evidence, ..
        } = &trace.events[4].payload
        else {
            panic!("terminal event is missing")
        };
        assert_eq!(*state, TerminalState::Interrupted);
        assert_eq!(evidence.workspace_identity, None);
        assert_eq!(evidence.git_state_identity, None);
        assert_eq!(evidence.verification_identity, None);
        assert_eq!(evidence.observed_verification_count, 0);
        assert_eq!(evidence.completion, None);
    }

    #[test]
    fn completed_chat_marks_all_completion_domains_not_applicable() {
        let trace = chat_accumulator()
            .finalize_completed(20, terminal())
            .unwrap();
        trace.validate_complete().unwrap();
        let TracePayload::Terminal {
            state: TerminalState::Completed,
            evidence,
            ..
        } = &trace.events.last().unwrap().payload
        else {
            panic!("completed Chat trace must end with completed terminal evidence")
        };
        let completion = evidence.completion.as_ref().unwrap();
        for domain in [
            &completion.workspace_change,
            &completion.git_change,
            &completion.verification,
        ] {
            let CompletionDomain::NotApplicable { evidence } = domain else {
                panic!("Chat completion domains must be not applicable")
            };
            assert_eq!(evidence.authority, AuthorityLabel::Observed);
            assert_eq!(evidence.source, EvidenceSource::Runtime);
            assert!(evidence.identity.is_some());
        }
        assert_content_free(&trace);
    }

    #[test]
    fn completed_read_only_work_keeps_verification_unknown() {
        let trace = accumulator().finalize_completed(20, terminal()).unwrap();
        trace.validate_complete().unwrap();
        let TracePayload::Terminal { evidence, .. } = &trace.events.last().unwrap().payload else {
            panic!("completed Work trace must end with terminal evidence")
        };
        let completion = evidence.completion.as_ref().unwrap();
        assert!(matches!(
            completion.workspace_change,
            CompletionDomain::NotApplicable { .. }
        ));
        assert!(matches!(
            completion.git_change,
            CompletionDomain::NotApplicable { .. }
        ));
        let CompletionDomain::Unknown { evidence } = &completion.verification else {
            panic!("unobserved Work verification must remain unknown")
        };
        assert_eq!(evidence.authority, AuthorityLabel::Unknown);
        assert_eq!(evidence.source, EvidenceSource::Runtime);
        assert_eq!(evidence.identity, None);
        assert_eq!(evidence.observed_at_ms, None);
        assert_content_free(&trace);
    }

    #[test]
    fn invalid_time_or_unsafe_identity_fails_before_a_trace_can_be_persisted() {
        let time_error = accumulator()
            .finalize_interrupted(9, terminal())
            .unwrap_err();
        assert_eq!(time_error.code, "turn-trace-time-order-invalid");

        let mut unsafe_runtime = runtime();
        unsafe_runtime.runtime_identity = "../../private/path".into();
        let identity_error = CodexTurnTraceAccumulator::started(
            binding(),
            10,
            work_intent(),
            unsafe_runtime,
            Some(model()),
            context(),
        )
        .unwrap_err();
        assert_eq!(identity_error.code, "turn-trace-identity-invalid");
    }

    #[test]
    fn observation_and_terminal_time_extremes_fail_closed_without_partial_admission() {
        let mut before_start = accumulator();
        let tool_error = before_start
            .record_tool_observation(started_tool('6', 9))
            .unwrap_err();
        assert_eq!(tool_error.code, "turn-trace-time-order-invalid");
        assert!(before_start.tool_observations.is_empty());
        let usage_error = before_start
            .record_persisted_usage_snapshot(usage_snapshot("item-usage", 9, 24))
            .unwrap_err();
        assert_eq!(usage_error.code, "turn-trace-time-order-invalid");
        assert!(before_start.latest_usage_snapshot.is_none());
        assert_eq!(before_start.next_delivery_ordinal, 1);

        let mut after_terminal = accumulator();
        after_terminal
            .record_tool_observation(started_tool('6', 12))
            .unwrap();
        after_terminal
            .record_tool_observation(completed_tool('6', 16))
            .unwrap();
        let terminal_error = after_terminal
            .clone()
            .finalize_completed(15, terminal())
            .unwrap_err();
        assert_eq!(terminal_error.code, "turn-trace-time-order-invalid");
        assert_eq!(after_terminal.tool_observations.len(), 2);

        let mut at_maximum = CodexTurnTraceAccumulator::started(
            binding(),
            u64::MAX,
            work_intent(),
            runtime(),
            Some(model()),
            context(),
        )
        .unwrap();
        at_maximum
            .record_tool_observation(started_tool('6', u64::MAX))
            .unwrap();
        at_maximum
            .record_persisted_usage_snapshot(usage_snapshot("item-usage-max", u64::MAX, 24))
            .unwrap();
        let mut completed = completed_tool('6', u64::MAX);
        let TracePayload::Tool { duration_ms, .. } = &mut completed.payload else {
            unreachable!("the helper always returns a Tool observation")
        };
        *duration_ms = Some(0);
        at_maximum.record_tool_observation(completed).unwrap();
        let trace = at_maximum
            .finalize_completed(u64::MAX, maximal_terminal_metadata())
            .unwrap();
        assert!(trace.events.iter().all(|event| event.at_ms == u64::MAX));
        assert_unique_event_ids(&trace);
        trace.validate_complete().unwrap();
    }

    #[test]
    fn unknown_model_metadata_is_omitted_instead_of_fabricated() {
        let trace = CodexTurnTraceAccumulator::started(
            binding(),
            10,
            work_intent(),
            runtime(),
            None,
            context(),
        )
        .unwrap()
        .finalize_interrupted(20, terminal())
        .unwrap();
        assert!(trace
            .events
            .iter()
            .all(|event| !matches!(event.payload, TracePayload::Model { .. })));
    }

    #[test]
    fn serialized_trace_has_no_content_or_sensitive_field_names() {
        let trace = accumulator()
            .finalize_failed(
                20,
                ErrorMetadata {
                    error_identity: hash('6'),
                    stable_class: ErrorClass::Transport,
                    source_class: "transport".into(),
                    retryable: true,
                    evidence_source: EvidenceSource::Runtime,
                    evidence_identity: hash('7'),
                    source_bytes: 4_096,
                    redacted_fields: 1,
                    omitted_fields: 4,
                },
                TerminalMetadata {
                    evidence_source: EvidenceSource::Runtime,
                    evidence_identity: hash('8'),
                },
            )
            .unwrap();
        assert_content_free(&trace);
    }
}
