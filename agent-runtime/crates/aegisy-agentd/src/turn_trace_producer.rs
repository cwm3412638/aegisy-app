//! Runtime-owned construction of metadata-only Codex turn traces.
//!
//! Callers must convert provider/runtime values into bounded safe identities or
//! SHA-256 identities before constructing these inputs. This module accepts no
//! prompt, response, command, path, diff, terminal output, or credential field.
//! The accumulator is created only after Codex has returned the real Turn ID.

use crate::turn_trace::{
    AuthorityLabel, CompletionDomain, CompletionEvidence, ErrorClass, EvidenceRef, EvidenceSource,
    ModelReason, ModelRole, RedactionSummary, RuntimeState, SessionMode, TerminalEvidence,
    TerminalState, TraceBinding, TracePayload, TurnAccess, TurnKind, TurnTrace, TurnTraceError,
    UsageAccounting, UsageAttribution, UsageReportScope,
};
use crate::usage_authority::{UsageAuthorityError, UsageAuthorityReport};

const INTENT_EVENT_ID: &str = "intent-1";
const RUNTIME_EVENT_ID: &str = "runtime-1";
const MODEL_EVENT_ID: &str = "model-1";
const CONTEXT_EVENT_ID: &str = "context-1";
const USAGE_REPORT_EVENT_ID: &str = "usage-report-1";
const ERROR_EVENT_ID: &str = "error-1";
const TERMINAL_EVENT_ID: &str = "terminal-1";

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
pub struct TurnTraceProducerError {
    pub code: &'static str,
    pub message: &'static str,
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
#[derive(Debug)]
pub struct CodexTurnTraceAccumulator {
    trace: TurnTrace,
    session_mode: SessionMode,
    intent_identity: String,
    completion_basis: EvidenceRef,
    latest_usage_snapshot: Option<UsageSnapshotMetadata>,
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
        Ok(Self {
            trace,
            session_mode: intent.session_mode,
            intent_identity,
            completion_basis,
            latest_usage_snapshot: None,
        })
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
        let mut candidate = self.trace.clone();
        candidate.append(
            USAGE_REPORT_EVENT_ID.into(),
            snapshot.observed_at_ms,
            usage_report_payload(&snapshot)?,
        )?;
        candidate.validate_open()?;
        self.latest_usage_snapshot = Some(snapshot);
        Ok(())
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
        self.append_latest_usage_snapshot()?;
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
        self.append_terminal(TerminalState::Failed, terminal_at_ms, terminal, None)
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
        self.append_latest_usage_snapshot()?;
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

    fn append_latest_usage_snapshot(&mut self) -> Result<(), TurnTraceProducerError> {
        let Some(snapshot) = self.latest_usage_snapshot.take() else {
            return Ok(());
        };
        self.trace.append(
            USAGE_REPORT_EVENT_ID.into(),
            snapshot.observed_at_ms,
            usage_report_payload(&snapshot)?,
        )?;
        Ok(())
    }
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
            "prompt",
            "response",
            "command",
            "cwd",
            "path",
            "diff",
            "output",
            "credential",
            "authorization",
            "process_id",
            "Bearer",
            "private provider body",
        ] {
            assert!(!serialized.contains(forbidden), "found {forbidden}");
        }
        assert!(serialized.contains("\"content_included\":false"));
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
        assert_eq!(trace.schema_version, "turn-trace/0.3");
        assert!(trace
            .events
            .iter()
            .all(|event| !matches!(event.payload, TracePayload::UsageReport { .. })));
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
