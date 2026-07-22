//! Runtime-owned construction of metadata-only Codex turn traces.
//!
//! Callers must convert provider/runtime values into bounded safe identities or
//! SHA-256 identities before constructing these inputs. This module accepts no
//! prompt, response, command, path, diff, terminal output, or credential field.
//! The accumulator is created only after Codex has returned the real Turn ID.

use crate::turn_trace::{
    AuthorityLabel, ErrorClass, EvidenceRef, EvidenceSource, ModelReason, ModelRole,
    RedactionSummary, RuntimeState, TerminalEvidence, TerminalState, TraceBinding, TracePayload,
    TurnTrace, TurnTraceError,
};

const RUNTIME_EVENT_ID: &str = "runtime-1";
const MODEL_EVENT_ID: &str = "model-1";
const CONTEXT_EVENT_ID: &str = "context-1";
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

/// An open metadata trace for one real Codex Turn.
///
/// Terminal methods consume the accumulator. This makes a second terminal
/// transition impossible through this API and gives the Store one complete,
/// terminal-last `TurnTrace` value.
#[derive(Debug)]
pub struct CodexTurnTraceAccumulator {
    trace: TurnTrace,
}

impl CodexTurnTraceAccumulator {
    pub fn started(
        binding: TraceBinding,
        started_at_ms: u64,
        runtime: RuntimeMetadata,
        model: Option<ModelMetadata>,
        context: PreparedContextSummary,
    ) -> Result<Self, TurnTraceProducerError> {
        let mut trace = TurnTrace::new(binding)?;
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
        Ok(Self { trace })
    }

    #[cfg(test)]
    pub fn open_trace(&self) -> &TurnTrace {
        &self.trace
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
        self.append_terminal(TerminalState::Failed, terminal_at_ms, terminal)
    }

    pub fn finalize_interrupted(
        self,
        terminal_at_ms: u64,
        terminal: TerminalMetadata,
    ) -> Result<TurnTrace, TurnTraceProducerError> {
        self.append_terminal(TerminalState::Interrupted, terminal_at_ms, terminal)
    }

    /// Successful completion remains fail-closed until Runtime supplies
    /// authoritative terminal Workspace, Git, and verification evidence.
    #[cfg(test)]
    pub fn finalize_completed(
        self,
        _terminal_at_ms: u64,
    ) -> Result<TurnTrace, TurnTraceProducerError> {
        Err(TurnTraceProducerError {
            code: "turn-trace-completed-unsupported",
            message: "completed Codex traces require authoritative completion evidence",
        })
    }

    fn append_terminal(
        mut self,
        state: TerminalState,
        terminal_at_ms: u64,
        terminal: TerminalMetadata,
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
                },
                redaction: RedactionSummary::metadata_only(),
            },
        )?;
        self.trace.validate_complete()?;
        Ok(self.trace)
    }
}

fn observed(source: EvidenceSource, identity: String, observed_at_ms: u64) -> EvidenceRef {
    EvidenceRef {
        authority: AuthorityLabel::Observed,
        source,
        identity: Some(identity),
        observed_at_ms: Some(observed_at_ms),
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
        CodexTurnTraceAccumulator::started(binding(), 10, runtime(), Some(model()), context())
            .unwrap()
    }

    fn terminal() -> TerminalMetadata {
        TerminalMetadata {
            evidence_source: EvidenceSource::Provider,
            evidence_identity: hash('3'),
        }
    }

    #[test]
    fn turn_started_records_only_runtime_model_and_context_metadata() {
        let accumulator = accumulator();
        let trace = accumulator.open_trace();
        trace.validate_open().unwrap();
        assert_eq!(trace.events.len(), 3);
        assert_eq!(trace.events[0].sequence, 1);
        assert_eq!(trace.events[1].sequence, 2);
        assert_eq!(trace.events[2].sequence, 3);
        assert!(matches!(
            trace.events[0].payload,
            TracePayload::Runtime { .. }
        ));
        assert!(matches!(
            trace.events[1].payload,
            TracePayload::Model { .. }
        ));
        assert!(matches!(
            trace.events[2].payload,
            TracePayload::Context { .. }
        ));
        assert!(trace.events.iter().all(|event| event.at_ms == 10));
    }

    #[test]
    fn failure_records_error_then_one_terminal_with_one_timestamp() {
        let error = ErrorMetadata {
            error_identity: hash('4'),
            stable_class: ErrorClass::Transport,
            source_class: "response-stream-disconnected".into(),
            retryable: true,
            evidence_source: EvidenceSource::Provider,
            evidence_identity: hash('5'),
            source_bytes: 512,
            redacted_fields: 1,
            omitted_fields: 2,
        };
        let trace = accumulator()
            .finalize_failed(20, error, terminal())
            .unwrap();
        trace.validate_complete().unwrap();
        assert_eq!(trace.events.len(), 5);
        assert_eq!(trace.events[3].at_ms, 20);
        assert_eq!(trace.events[4].at_ms, 20);
        assert!(matches!(
            trace.events[3].payload,
            TracePayload::Error { .. }
        ));
        assert!(matches!(
            trace.events[4].payload,
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
    }

    #[test]
    fn interruption_is_terminal_last_without_fabricated_error_or_completion_evidence() {
        let trace = accumulator().finalize_interrupted(21, terminal()).unwrap();
        assert_eq!(trace.events.len(), 4);
        let TracePayload::Terminal {
            state, evidence, ..
        } = &trace.events[3].payload
        else {
            panic!("terminal event is missing")
        };
        assert_eq!(*state, TerminalState::Interrupted);
        assert_eq!(evidence.workspace_identity, None);
        assert_eq!(evidence.git_state_identity, None);
        assert_eq!(evidence.verification_identity, None);
        assert_eq!(evidence.observed_verification_count, 0);
    }

    #[test]
    fn completed_turn_remains_explicitly_unsupported() {
        let error = accumulator().finalize_completed(20).unwrap_err();
        assert_eq!(error.code, "turn-trace-completed-unsupported");
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
            unsafe_runtime,
            Some(model()),
            context(),
        )
        .unwrap_err();
        assert_eq!(identity_error.code, "turn-trace-identity-invalid");
    }

    #[test]
    fn unknown_model_metadata_is_omitted_instead_of_fabricated() {
        let trace = CodexTurnTraceAccumulator::started(binding(), 10, runtime(), None, context())
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
        let serialized = serde_json::to_string(&trace).unwrap();
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
        ] {
            assert!(!serialized.contains(forbidden), "found {forbidden}");
        }
        assert!(serialized.contains("\"content_included\":false"));
    }
}
