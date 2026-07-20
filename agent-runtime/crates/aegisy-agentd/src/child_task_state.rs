//! Content-free child-task lifecycle and handoff state.
//!
//! This is a state-machine foundation only. It does not create sessions, run a
//! provider, allocate a worktree, persist a job, or grant a permission ticket.

use serde::{Deserialize, Serialize};
use serde_json::to_vec;
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;

pub const SCHEMA_VERSION: &str = "child-task-state/0.1";
pub const HANDOFF_SCHEMA_VERSION: &str = "child-handoff/0.1";
const MAX_EVIDENCE_REFS: usize = 32;
const MAX_ARTIFACT_REFS: usize = 32;
const MAX_COUNTER: u32 = 10_000;
const MAX_SUMMARY_BYTES: u64 = 16 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ChildTaskStateError {
    pub code: &'static str,
    pub message: String,
}

impl ChildTaskStateError {
    fn new(code: &'static str, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ChildTaskStatus {
    Queued,
    Running,
    WaitingApproval,
    Completed,
    Failed,
    Cancelled,
    Interrupted,
}

impl ChildTaskStatus {
    fn terminal(self) -> bool {
        matches!(
            self,
            Self::Completed | Self::Failed | Self::Cancelled | Self::Interrupted
        )
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CancellationState {
    NotRequested,
    Requested,
    Acknowledged,
    Failed,
    SupersededByCompletion,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ChildTaskHandoff {
    pub schema_version: String,
    pub task_identity: String,
    pub child_session_id: String,
    pub source_revision: String,
    pub summary_reference: String,
    pub summary_bytes: u64,
    #[serde(default)]
    pub evidence_references: Vec<String>,
    #[serde(default)]
    pub artifact_references: Vec<String>,
    pub changed_path_count: u32,
    pub test_count: u32,
    pub risk_count: u32,
    pub unresolved_count: u32,
    pub truncated: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ChildTaskState {
    pub schema_version: String,
    pub task_identity: String,
    pub parent_session_id: String,
    pub parent_turn_id: String,
    #[serde(default)]
    pub child_session_id: Option<String>,
    pub status: ChildTaskStatus,
    pub cancellation: CancellationState,
    pub generation: u64,
    pub created_at_ms: u64,
    pub updated_at_ms: u64,
    #[serde(default)]
    pub handoff: Option<ChildTaskHandoff>,
}

impl ChildTaskState {
    pub fn new(
        task_identity: impl Into<String>,
        parent_session_id: impl Into<String>,
        parent_turn_id: impl Into<String>,
        now_ms: u64,
    ) -> Result<Self, ChildTaskStateError> {
        let state = Self {
            schema_version: SCHEMA_VERSION.into(),
            task_identity: task_identity.into(),
            parent_session_id: parent_session_id.into(),
            parent_turn_id: parent_turn_id.into(),
            child_session_id: None,
            status: ChildTaskStatus::Queued,
            cancellation: CancellationState::NotRequested,
            generation: 0,
            created_at_ms: now_ms,
            updated_at_ms: now_ms,
            handoff: None,
        };
        state.validate()?;
        Ok(state)
    }

    pub fn validate(&self) -> Result<(), ChildTaskStateError> {
        if self.schema_version != SCHEMA_VERSION {
            return Err(ChildTaskStateError::new(
                "child-task-state-schema-unsupported",
                "child-task state schema is unsupported",
            ));
        }
        validate_identity(
            &self.task_identity,
            "child-task-state-task-identity-invalid",
        )?;
        validate_identifier(
            &self.parent_session_id,
            "child-task-state-parent-session-invalid",
        )?;
        validate_identifier(&self.parent_turn_id, "child-task-state-parent-turn-invalid")?;
        if let Some(child_session_id) = self.child_session_id.as_deref() {
            validate_identifier(child_session_id, "child-task-state-child-session-invalid")?;
            if child_session_id == self.parent_session_id {
                return Err(ChildTaskStateError::new(
                    "child-task-state-parent-child-same",
                    "parent and child sessions must differ",
                ));
            }
        }
        if self.created_at_ms == 0 || self.updated_at_ms < self.created_at_ms {
            return Err(ChildTaskStateError::new(
                "child-task-state-time-invalid",
                "child-task timestamps are invalid",
            ));
        }
        if self.status == ChildTaskStatus::Completed && self.handoff.is_none() {
            return Err(ChildTaskStateError::new(
                "child-task-state-handoff-missing",
                "completed child task requires a bounded handoff",
            ));
        }
        if let Some(handoff) = self.handoff.as_ref() {
            if self.status != ChildTaskStatus::Completed {
                return Err(ChildTaskStateError::new(
                    "child-task-state-handoff-status-invalid",
                    "handoff is only valid for a completed child task",
                ));
            }
            validate_handoff(
                handoff,
                &self.task_identity,
                self.child_session_id.as_deref(),
            )?;
        }
        match self.cancellation {
            CancellationState::NotRequested | CancellationState::Failed => {
                if self.status == ChildTaskStatus::Cancelled {
                    return Err(ChildTaskStateError::new(
                        "child-task-state-cancellation-status-invalid",
                        "cancelled status requires acknowledged cancellation",
                    ));
                }
            }
            CancellationState::Requested => {
                if self.status.terminal() {
                    return Err(ChildTaskStateError::new(
                        "child-task-state-cancellation-status-invalid",
                        "terminal child task cannot retain a pending cancellation",
                    ));
                }
            }
            CancellationState::Acknowledged => {
                if self.status != ChildTaskStatus::Cancelled {
                    return Err(ChildTaskStateError::new(
                        "child-task-state-cancellation-status-invalid",
                        "acknowledged cancellation requires cancelled status",
                    ));
                }
            }
            CancellationState::SupersededByCompletion => {
                if self.status != ChildTaskStatus::Completed {
                    return Err(ChildTaskStateError::new(
                        "child-task-state-cancellation-status-invalid",
                        "superseded cancellation requires completed status",
                    ));
                }
            }
        }
        Ok(())
    }

    pub fn identity(&self) -> Result<String, ChildTaskStateError> {
        self.validate()?;
        let bytes = to_vec(self).map_err(|_| {
            ChildTaskStateError::new(
                "child-task-state-serialize-failed",
                "child-task state could not be serialized",
            )
        })?;
        Ok(format!(
            "child-task-state:sha256:{:x}",
            Sha256::digest(bytes)
        ))
    }

    pub fn attach_child_session(
        &mut self,
        child_session_id: impl Into<String>,
        now_ms: u64,
    ) -> Result<(), ChildTaskStateError> {
        if self.status != ChildTaskStatus::Queued || self.child_session_id.is_some() {
            return Err(ChildTaskStateError::new(
                "child-task-state-child-session-already-bound",
                "child session can only be bound once while queued",
            ));
        }
        let child_session_id = child_session_id.into();
        validate_identifier(&child_session_id, "child-task-state-child-session-invalid")?;
        if child_session_id == self.parent_session_id {
            return Err(ChildTaskStateError::new(
                "child-task-state-parent-child-same",
                "parent and child sessions must differ",
            ));
        }
        self.apply_update(now_ms, move |state| {
            state.child_session_id = Some(child_session_id);
            state.status = ChildTaskStatus::Running;
        })
    }

    pub fn transition(
        &mut self,
        next: ChildTaskStatus,
        now_ms: u64,
    ) -> Result<(), ChildTaskStateError> {
        self.validate()?;
        if !valid_transition(self.status, next) {
            return Err(ChildTaskStateError::new(
                "child-task-state-transition-invalid",
                format!("cannot transition {:?} to {:?}", self.status, next),
            ));
        }
        if next != ChildTaskStatus::Queued && self.child_session_id.is_none() {
            return Err(ChildTaskStateError::new(
                "child-task-state-child-session-required",
                "child session must be bound before execution state",
            ));
        }
        self.apply_update(now_ms, |state| {
            state.status = next;
            if matches!(next, ChildTaskStatus::Failed | ChildTaskStatus::Interrupted)
                && state.cancellation == CancellationState::Requested
            {
                state.cancellation = CancellationState::Failed;
            }
        })
    }

    pub fn request_cancel(&mut self, now_ms: u64) -> Result<bool, ChildTaskStateError> {
        self.validate()?;
        if self.status.terminal() {
            return Err(ChildTaskStateError::new(
                "child-task-state-cancel-terminal",
                "terminal child task cannot be cancelled",
            ));
        }
        if self.cancellation == CancellationState::Requested {
            return Ok(false);
        }
        self.apply_update(now_ms, |state| {
            state.cancellation = CancellationState::Requested;
        })?;
        Ok(true)
    }

    pub fn acknowledge_cancel(&mut self, now_ms: u64) -> Result<(), ChildTaskStateError> {
        if self.cancellation != CancellationState::Requested {
            return Err(ChildTaskStateError::new(
                "child-task-state-cancel-not-requested",
                "child-task cancellation was not requested",
            ));
        }
        self.apply_update(now_ms, |state| {
            state.cancellation = CancellationState::Acknowledged;
            state.status = ChildTaskStatus::Cancelled;
        })
    }

    pub fn reject_cancel(&mut self, now_ms: u64) -> Result<(), ChildTaskStateError> {
        if self.cancellation != CancellationState::Requested {
            return Err(ChildTaskStateError::new(
                "child-task-state-cancel-not-requested",
                "child-task cancellation was not requested",
            ));
        }
        self.apply_update(now_ms, |state| {
            state.cancellation = CancellationState::Failed;
        })
    }

    pub fn complete(
        &mut self,
        handoff: ChildTaskHandoff,
        now_ms: u64,
    ) -> Result<(), ChildTaskStateError> {
        if !matches!(
            self.status,
            ChildTaskStatus::Running | ChildTaskStatus::WaitingApproval
        ) {
            return Err(ChildTaskStateError::new(
                "child-task-state-completion-invalid",
                "child task is not active",
            ));
        }
        validate_handoff(
            &handoff,
            &self.task_identity,
            self.child_session_id.as_deref(),
        )?;
        self.apply_update(now_ms, move |state| {
            state.handoff = Some(handoff);
            state.status = ChildTaskStatus::Completed;
            if state.cancellation == CancellationState::Requested {
                state.cancellation = CancellationState::SupersededByCompletion;
            }
        })
    }

    fn apply_update(
        &mut self,
        now_ms: u64,
        update: impl FnOnce(&mut Self),
    ) -> Result<(), ChildTaskStateError> {
        self.validate()?;
        let next_generation = self.generation.checked_add(1).ok_or_else(|| {
            ChildTaskStateError::new(
                "child-task-state-generation-exhausted",
                "child-task state generation is exhausted",
            )
        })?;
        let previous = self.clone();
        update(self);
        self.generation = next_generation;
        self.updated_at_ms = now_ms.max(previous.updated_at_ms);
        if let Err(error) = self.validate() {
            *self = previous;
            return Err(error);
        }
        Ok(())
    }
}

fn valid_transition(from: ChildTaskStatus, to: ChildTaskStatus) -> bool {
    matches!(
        (from, to),
        (ChildTaskStatus::Queued, ChildTaskStatus::Running)
            | (ChildTaskStatus::Running, ChildTaskStatus::WaitingApproval)
            | (ChildTaskStatus::Running, ChildTaskStatus::Failed)
            | (ChildTaskStatus::Running, ChildTaskStatus::Interrupted)
            | (ChildTaskStatus::WaitingApproval, ChildTaskStatus::Running)
            | (ChildTaskStatus::WaitingApproval, ChildTaskStatus::Failed)
            | (
                ChildTaskStatus::WaitingApproval,
                ChildTaskStatus::Interrupted
            )
    )
}

fn validate_handoff(
    handoff: &ChildTaskHandoff,
    task_identity: &str,
    child_session_id: Option<&str>,
) -> Result<(), ChildTaskStateError> {
    if handoff.schema_version != HANDOFF_SCHEMA_VERSION {
        return Err(ChildTaskStateError::new(
            "child-task-state-handoff-schema-invalid",
            "handoff schema is unsupported",
        ));
    }
    if handoff.task_identity != task_identity {
        return Err(ChildTaskStateError::new(
            "child-task-state-handoff-task-mismatch",
            "handoff task identity does not match state",
        ));
    }
    if child_session_id != Some(handoff.child_session_id.as_str()) {
        return Err(ChildTaskStateError::new(
            "child-task-state-handoff-child-mismatch",
            "handoff child session does not match state",
        ));
    }
    validate_identifier(
        &handoff.child_session_id,
        "child-task-state-child-session-invalid",
    )?;
    validate_revision(
        &handoff.source_revision,
        "child-task-state-source-revision-invalid",
    )?;
    validate_content_reference(&handoff.summary_reference)?;
    if handoff.summary_bytes == 0 || handoff.summary_bytes > MAX_SUMMARY_BYTES {
        return Err(ChildTaskStateError::new(
            "child-task-state-summary-size-invalid",
            "handoff summary exceeds the bounded size",
        ));
    }
    validate_references(&handoff.evidence_references, MAX_EVIDENCE_REFS, "evidence")?;
    validate_references(&handoff.artifact_references, MAX_ARTIFACT_REFS, "artifact")?;
    for value in [
        handoff.changed_path_count,
        handoff.test_count,
        handoff.risk_count,
        handoff.unresolved_count,
    ] {
        if value > MAX_COUNTER {
            return Err(ChildTaskStateError::new(
                "child-task-state-counter-limit",
                "handoff count exceeds the bounded limit",
            ));
        }
    }
    Ok(())
}

fn validate_references(
    references: &[String],
    limit: usize,
    category: &str,
) -> Result<(), ChildTaskStateError> {
    if references.len() > limit {
        return Err(ChildTaskStateError::new(
            "child-task-state-reference-limit",
            format!("handoff {category} references exceed the bounded limit"),
        ));
    }
    let mut unique = BTreeSet::new();
    for reference in references {
        validate_content_reference(reference)?;
        if !unique.insert(reference.as_str()) {
            return Err(ChildTaskStateError::new(
                "child-task-state-duplicate-reference",
                format!("handoff {category} references contain a duplicate"),
            ));
        }
    }
    Ok(())
}

fn validate_content_reference(value: &str) -> Result<(), ChildTaskStateError> {
    let valid = [
        "artifact:sha256:",
        "pinned-context:sha256:",
        "structured-plan:sha256:",
    ]
    .iter()
    .any(|prefix| {
        value.strip_prefix(prefix).is_some_and(|hex| {
            hex.len() == 64
                && hex
                    .bytes()
                    .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
        })
    });
    if !valid {
        return Err(ChildTaskStateError::new(
            "child-task-state-reference-invalid",
            "handoff reference is not a content identity",
        ));
    }
    Ok(())
}

fn validate_identity(value: &str, code: &'static str) -> Result<(), ChildTaskStateError> {
    let prefix = "child-task:sha256:";
    if !value.starts_with(prefix)
        || value.len() != prefix.len() + 64
        || !value[prefix.len()..]
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
        return Err(ChildTaskStateError::new(
            code,
            "child-task identity is invalid",
        ));
    }
    Ok(())
}

fn validate_identifier(value: &str, code: &'static str) -> Result<(), ChildTaskStateError> {
    if value.is_empty()
        || value.len() > 128
        || value
            .bytes()
            .any(|byte| !(byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b':')))
    {
        return Err(ChildTaskStateError::new(
            code,
            "child-task identifier is invalid",
        ));
    }
    Ok(())
}

fn validate_revision(value: &str, code: &'static str) -> Result<(), ChildTaskStateError> {
    if value.is_empty() || value.len() > 256 || value.chars().any(char::is_control) {
        return Err(ChildTaskStateError::new(
            code,
            "child-task revision is invalid",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn state() -> ChildTaskState {
        ChildTaskState::new(
            format!("child-task:sha256:{}", "a".repeat(64)),
            "parent-session",
            "parent-turn",
            1_000,
        )
        .unwrap()
    }

    fn handoff(task_identity: &str, child_session_id: &str) -> ChildTaskHandoff {
        ChildTaskHandoff {
            schema_version: HANDOFF_SCHEMA_VERSION.into(),
            task_identity: task_identity.into(),
            child_session_id: child_session_id.into(),
            source_revision: "rev-2".into(),
            summary_reference: format!("artifact:sha256:{}", "b".repeat(64)),
            summary_bytes: 512,
            evidence_references: vec![format!("structured-plan:sha256:{}", "c".repeat(64))],
            artifact_references: vec![format!("artifact:sha256:{}", "d".repeat(64))],
            changed_path_count: 2,
            test_count: 3,
            risk_count: 1,
            unresolved_count: 0,
            truncated: false,
        }
    }

    #[test]
    fn lifecycle_requires_child_binding_and_preserves_generation() {
        let mut state = state();
        assert_eq!(state.status, ChildTaskStatus::Queued);
        state.attach_child_session("child-session", 1_100).unwrap();
        assert_eq!(state.status, ChildTaskStatus::Running);
        assert_eq!(state.generation, 1);
        state
            .transition(ChildTaskStatus::WaitingApproval, 1_200)
            .unwrap();
        state.transition(ChildTaskStatus::Running, 1_300).unwrap();
        state.transition(ChildTaskStatus::Failed, 1_400).unwrap();
        assert_eq!(
            state.identity().unwrap().len(),
            "child-task-state:sha256:".len() + 64
        );
    }

    #[test]
    fn cancellation_is_separate_idempotent_request_and_acknowledgement() {
        let mut state = state();
        state.attach_child_session("child-session", 1_100).unwrap();
        assert!(state.request_cancel(1_200).unwrap());
        assert!(!state.request_cancel(1_300).unwrap());
        state.acknowledge_cancel(1_400).unwrap();
        assert_eq!(state.status, ChildTaskStatus::Cancelled);
        assert_eq!(state.cancellation, CancellationState::Acknowledged);
    }

    #[test]
    fn rejected_cancellation_can_be_retried_without_ending_child() {
        let mut state = state();
        state.attach_child_session("child-session", 1_100).unwrap();
        state.request_cancel(1_200).unwrap();
        state.reject_cancel(1_300).unwrap();
        assert_eq!(state.status, ChildTaskStatus::Running);
        assert_eq!(state.cancellation, CancellationState::Failed);
        assert!(state.request_cancel(1_400).unwrap());
    }

    #[test]
    fn completion_can_win_cancel_race_with_bounded_handoff() {
        let mut state = state();
        state.attach_child_session("child-session", 1_100).unwrap();
        state.request_cancel(1_200).unwrap();
        let task_identity = state.task_identity.clone();
        state
            .complete(handoff(&task_identity, "child-session"), 1_300)
            .unwrap();
        assert_eq!(state.status, ChildTaskStatus::Completed);
        assert_eq!(
            state.cancellation,
            CancellationState::SupersededByCompletion
        );
    }

    #[test]
    fn rejects_invalid_transitions_and_mismatched_handoffs() {
        let mut state = state();
        assert_eq!(
            state
                .transition(ChildTaskStatus::Completed, 1_100)
                .unwrap_err()
                .code,
            "child-task-state-transition-invalid"
        );
        assert_eq!(state.status, ChildTaskStatus::Queued);
        assert_eq!(state.generation, 0);
        state.attach_child_session("child-session", 1_100).unwrap();
        let task_identity = state.task_identity.clone();
        let mut invalid = handoff(&task_identity, "child-session");
        invalid.summary_bytes = MAX_SUMMARY_BYTES + 1;
        assert_eq!(
            state.complete(invalid, 1_200).unwrap_err().code,
            "child-task-state-summary-size-invalid"
        );
        let mismatch = handoff(&task_identity, "other-child");
        assert_eq!(
            state.complete(mismatch, 1_200).unwrap_err().code,
            "child-task-state-handoff-child-mismatch"
        );
        assert_eq!(state.status, ChildTaskStatus::Running);
        assert!(state.handoff.is_none());
    }

    #[test]
    fn generation_exhaustion_and_terminal_pending_cancel_fail_closed() {
        let mut state = state();
        state.attach_child_session("child-session", 1_100).unwrap();
        state.generation = u64::MAX;
        let before = state.clone();
        assert_eq!(
            state.request_cancel(1_200).unwrap_err().code,
            "child-task-state-generation-exhausted"
        );
        assert_eq!(state, before);

        let mut invalid = state;
        invalid.generation = 1;
        invalid.status = ChildTaskStatus::Failed;
        invalid.cancellation = CancellationState::Requested;
        assert_eq!(
            invalid.validate().unwrap_err().code,
            "child-task-state-cancellation-status-invalid"
        );
    }
}
