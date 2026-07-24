use aegisy_aap::stable::v0_1::{
    timeline_event_id, EventEnvelope, ItemUpdate, TimelineItem, TurnState,
};
use aegisy_aap::{MAX_SAFE_JSON_INTEGER, MAX_TIMELINE_IDENTIFIER_BYTES};
use std::collections::HashMap;

const EVENT_SCHEMA_VERSION: &str = "timeline-event/0.1";
const SNAPSHOT_REPLACEMENT: &str = "snapshot-replacement";

#[derive(Debug, Default, Clone)]
pub(crate) struct EventSequencer {
    sessions: HashMap<String, SessionSequence>,
}

pub(crate) struct PreparedEvent {
    session_id: String,
    expected_sequence: u64,
    expected_event_id: Option<String>,
    candidate: SessionSequence,
    envelope: EventEnvelope,
}

pub(crate) struct SessionRestoreCandidate {
    session_id: String,
    expected_sequence: u64,
    expected_event_id: Option<String>,
    session: SessionSequence,
    last_event_id: Option<String>,
}

pub(crate) struct RestoredSession {
    session_id: String,
    session: SessionSequence,
}

#[derive(Debug, Clone, Default)]
struct SessionSequence {
    sequence: u64,
    timestamp_ms: u64,
    last_event_id: Option<String>,
    turns: HashMap<String, TurnSequence>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum TurnPhase {
    Running,
    Completed,
    Failed,
    Interrupted,
}

#[derive(Debug, Clone)]
struct TurnSequence {
    phase: TurnPhase,
    items: HashMap<String, ItemSequence>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ItemPhase {
    Streaming,
    Updating,
    Terminal,
}

#[derive(Debug, Clone)]
struct ItemSequence {
    revision: u64,
    phase: ItemPhase,
    kind: String,
    role: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum SequenceError {
    Rejected(&'static str),
    TurnViolation(&'static str),
    Bounds(&'static str),
}

impl SequenceError {
    pub(crate) fn code(&self) -> &'static str {
        match self {
            Self::Rejected(code) | Self::TurnViolation(code) | Self::Bounds(code) => code,
        }
    }

    pub(crate) fn closes_turn(&self) -> bool {
        matches!(self, Self::TurnViolation(_))
    }
}

impl PreparedEvent {
    pub(crate) fn envelope(&self) -> &EventEnvelope {
        &self.envelope
    }

    pub(crate) fn commit(
        self,
        sequencer: &mut EventSequencer,
    ) -> Result<EventEnvelope, SequenceError> {
        let Self {
            session_id,
            expected_sequence,
            expected_event_id,
            candidate,
            envelope,
        } = self;
        let current = sequencer.sessions.get(&session_id);
        let current_sequence = current.map_or(0, |session| session.sequence);
        let current_event_id = current.and_then(|session| session.last_event_id.as_ref());
        if current_sequence != expected_sequence || current_event_id != expected_event_id.as_ref() {
            return Err(SequenceError::Rejected("prepared-event-stale"));
        }
        sequencer.sessions.insert(session_id, candidate);
        Ok(envelope)
    }
}

impl SessionRestoreCandidate {
    pub(crate) fn replay_event(mut self, event: &EventEnvelope) -> Result<Self, SequenceError> {
        event
            .validate()
            .map_err(|_| SequenceError::Rejected("restored-event-invalid"))?;
        if event.session_id != self.session_id {
            return Err(SequenceError::Rejected("restored-session-mismatch"));
        }
        if self.session.sequence >= self.expected_sequence {
            return Err(SequenceError::Rejected("restored-event-after-watermark"));
        }
        let reproduced = EventSequencer::prepare_session_event(
            &mut self.session,
            event.timestamp_ms,
            &event.session_id,
            &event.turn_id,
            &event.event,
            event.item.clone(),
        )?;
        if reproduced != *event {
            return Err(SequenceError::Rejected("restored-event-mismatch"));
        }
        self.last_event_id = Some(event.event_id.clone());
        Ok(self)
    }

    pub(crate) fn complete(self) -> Result<RestoredSession, SequenceError> {
        if self.session.sequence != self.expected_sequence {
            return Err(SequenceError::Rejected("restored-session-incomplete"));
        }
        if self.last_event_id != self.expected_event_id {
            return Err(SequenceError::Rejected("restored-watermark-mismatch"));
        }
        Ok(RestoredSession {
            session_id: self.session_id,
            session: self.session,
        })
    }
}

impl EventSequencer {
    pub(crate) fn preflight(
        &self,
        observed_at_ms: u64,
        session_id: &str,
        turn_id: &str,
        event: &str,
        item: Option<TimelineItem>,
    ) -> Result<(), SequenceError> {
        let mut candidate = self.sessions.get(session_id).cloned().unwrap_or_default();
        Self::prepare_session_event(
            &mut candidate,
            observed_at_ms,
            session_id,
            turn_id,
            event,
            item,
        )
        .map(drop)
    }

    pub(crate) fn prepare(
        &self,
        observed_at_ms: u64,
        session_id: &str,
        turn_id: &str,
        event: &str,
        item: Option<TimelineItem>,
    ) -> Result<PreparedEvent, SequenceError> {
        let current = self.sessions.get(session_id);
        let expected_sequence = current.map_or(0, |session| session.sequence);
        let expected_event_id = current.and_then(|session| session.last_event_id.clone());
        let mut candidate = current.cloned().unwrap_or_default();
        let envelope = Self::prepare_session_event(
            &mut candidate,
            observed_at_ms,
            session_id,
            turn_id,
            event,
            item,
        )?;
        Ok(PreparedEvent {
            session_id: session_id.to_owned(),
            expected_sequence,
            expected_event_id,
            candidate,
            envelope,
        })
    }

    pub(crate) fn normalized_timestamp(
        &self,
        observed_at_ms: u64,
        session_id: &str,
    ) -> Result<u64, SequenceError> {
        if observed_at_ms == 0 || observed_at_ms > MAX_SAFE_JSON_INTEGER {
            return Err(SequenceError::Bounds("event-timestamp-out-of-range"));
        }
        Ok(observed_at_ms.max(
            self.sessions
                .get(session_id)
                .map_or(0, |session| session.timestamp_ms),
        ))
    }

    pub(crate) fn sequence(
        &mut self,
        observed_at_ms: u64,
        session_id: &str,
        turn_id: &str,
        event: &str,
        item: Option<TimelineItem>,
    ) -> Result<EventEnvelope, SequenceError> {
        let prepared = self.prepare(observed_at_ms, session_id, turn_id, event, item)?;
        prepared.commit(self)
    }

    pub(crate) fn begin_session_restore(
        session_id: &str,
        expected_sequence: u64,
        expected_event_id: Option<&str>,
    ) -> Result<SessionRestoreCandidate, SequenceError> {
        if !valid_binding_identity(session_id) {
            return Err(SequenceError::Rejected("restored-session-identity-invalid"));
        }
        if expected_sequence > MAX_SAFE_JSON_INTEGER {
            return Err(SequenceError::Bounds("restored-sequence-out-of-range"));
        }
        if (expected_sequence == 0) != expected_event_id.is_none()
            || expected_event_id.is_some_and(|event_id| !valid_event_id(event_id))
        {
            return Err(SequenceError::Rejected("restored-watermark-invalid"));
        }
        Ok(SessionRestoreCandidate {
            session_id: session_id.to_owned(),
            expected_sequence,
            expected_event_id: expected_event_id.map(str::to_owned),
            session: SessionSequence::default(),
            last_event_id: None,
        })
    }

    pub(crate) fn install_restored_session(&mut self, restored: RestoredSession) {
        if restored.session.sequence == 0 {
            self.sessions.remove(&restored.session_id);
        } else {
            self.sessions.insert(restored.session_id, restored.session);
        }
    }

    fn prepare_session_event(
        candidate: &mut SessionSequence,
        observed_at_ms: u64,
        session_id: &str,
        turn_id: &str,
        event: &str,
        item: Option<TimelineItem>,
    ) -> Result<EventEnvelope, SequenceError> {
        let (turn_state, item, item_update) = Self::apply(candidate, turn_id, event, item)?;
        let envelope = Self::finish(
            candidate,
            observed_at_ms,
            session_id,
            turn_id,
            turn_state,
            event,
            item,
            item_update,
        )?;
        candidate.sequence = envelope.sequence;
        candidate.timestamp_ms = envelope.timestamp_ms;
        candidate.last_event_id = Some(envelope.event_id.clone());
        Ok(envelope)
    }

    fn apply(
        session: &mut SessionSequence,
        turn_id: &str,
        event: &str,
        item: Option<TimelineItem>,
    ) -> Result<(TurnState, Option<TimelineItem>, Option<ItemUpdate>), SequenceError> {
        if turn_id.is_empty() {
            return Err(SequenceError::Rejected("missing-turn-identity"));
        }
        validate_requested_shape(session, turn_id, event, item.as_ref())?;
        if event == "turn.started" {
            if item.is_some() || session.turns.contains_key(turn_id) {
                return Err(
                    if session
                        .turns
                        .get(turn_id)
                        .is_some_and(|turn| turn.phase == TurnPhase::Running)
                    {
                        SequenceError::TurnViolation("duplicate-turn-start")
                    } else {
                        SequenceError::Rejected("turn-start-rejected")
                    },
                );
            }
            if session
                .turns
                .values()
                .any(|turn| turn.phase == TurnPhase::Running)
            {
                return Err(SequenceError::Rejected("session-turn-already-running"));
            }
            session.turns.insert(
                turn_id.to_owned(),
                TurnSequence {
                    phase: TurnPhase::Running,
                    items: HashMap::new(),
                },
            );
            return Ok((TurnState::Running, None, None));
        }

        let Some(turn) = session.turns.get_mut(turn_id) else {
            return Err(SequenceError::Rejected("turn-not-running"));
        };
        if turn.phase != TurnPhase::Running {
            return Err(SequenceError::Rejected("turn-already-terminal"));
        }

        if let Some(terminal) = terminal_phase(event) {
            if matches!(terminal, TurnPhase::Completed | TurnPhase::Interrupted)
                && turn
                    .items
                    .values()
                    .any(|item| item.phase == ItemPhase::Streaming)
            {
                return Err(SequenceError::TurnViolation("open-item-at-turn-terminal"));
            }
            let item_update = match item.as_ref() {
                Some(item) => Some(apply_item(turn, "item.completed", item)?),
                None => None,
            };
            turn.phase = terminal;
            return Ok((turn_state(terminal), item, item_update));
        }

        let item_update = match item.as_ref() {
            Some(item) => Some(apply_item(turn, event, item)?),
            None => None,
        };
        Ok((TurnState::Running, item, item_update))
    }

    #[allow(clippy::too_many_arguments)]
    fn finish(
        candidate: &SessionSequence,
        observed_at_ms: u64,
        session_id: &str,
        turn_id: &str,
        turn_state: TurnState,
        event: &str,
        item: Option<TimelineItem>,
        item_update: Option<ItemUpdate>,
    ) -> Result<EventEnvelope, SequenceError> {
        let sequence = candidate
            .sequence
            .checked_add(1)
            .filter(|sequence| *sequence <= MAX_SAFE_JSON_INTEGER)
            .ok_or(SequenceError::Bounds("event-sequence-exhausted"))?;
        if observed_at_ms == 0 || observed_at_ms > MAX_SAFE_JSON_INTEGER {
            return Err(SequenceError::Bounds("event-timestamp-out-of-range"));
        }
        let timestamp_ms = observed_at_ms.max(candidate.timestamp_ms);
        let mut envelope = EventEnvelope {
            schema_version: EVENT_SCHEMA_VERSION.into(),
            event_id: String::new(),
            sequence,
            timestamp_ms,
            correlation_id: turn_id.into(),
            session_id: session_id.into(),
            turn_id: turn_id.into(),
            turn_state,
            event: event.into(),
            item,
            item_update,
        };
        envelope.event_id = timeline_event_id(
            &envelope.schema_version,
            envelope.sequence,
            envelope.timestamp_ms,
            &envelope.correlation_id,
            &envelope.session_id,
            &envelope.turn_id,
            envelope.turn_state,
            &envelope.event,
            &envelope.item,
            &envelope.item_update,
        )
        .map_err(|_| SequenceError::Rejected("event-identity-serialization-failed"))?;
        serde_json::to_vec(&envelope)
            .map_err(|_| SequenceError::Rejected("event-envelope-serialization-failed"))?;
        Ok(envelope)
    }
}

fn valid_binding_identity(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= MAX_TIMELINE_IDENTIFIER_BYTES
        && value.bytes().all(|byte| (0x21..=0x7e).contains(&byte))
}

fn valid_event_id(value: &str) -> bool {
    value.len() == 77
        && value.starts_with("event:sha256:")
        && value[13..]
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn validate_requested_shape(
    session: &SessionSequence,
    turn_id: &str,
    event: &str,
    item: Option<&TimelineItem>,
) -> Result<(), SequenceError> {
    let running = session
        .turns
        .get(turn_id)
        .is_some_and(|turn| turn.phase == TurnPhase::Running);
    let invalid = match event {
        "turn.started" | "turn.completed" | "turn.interrupted" => item.is_some(),
        "turn.failed" => !matches!(
            item,
            Some(item)
                if item.kind == "error"
                    && item.role == "system"
                    && item.state == "completed"
                    && item.validate().is_ok()
        ),
        "item.started" => !matches!(item, Some(item) if item.state == "started"),
        "item.delta" => !matches!(item, Some(item) if item.state == "delta"),
        "item.completed" => !matches!(item, Some(item) if item.state == "completed"),
        "diagnostics.observed" => !matches!(
            item,
            Some(item)
                if item.kind == "diagnostic"
                    && item.role == "tool"
                    && item.state == "completed"
        ),
        "usage.updated" => !matches!(
            item,
            Some(item)
                if item.kind == "usage"
                    && item.role == "system"
                    && item.state == "updated"
        ),
        "usage.truncated" => !matches!(
            item,
            Some(item)
                if item.kind == "usage"
                    && item.role == "system"
                    && item.state == "truncated"
        ),
        "turn.diff.updated" => !matches!(
            item,
            Some(item)
                if item.kind == "diff" && item.role == "tool" && item.state == "updated"
        ),
        "turn.diff.truncated" => !matches!(
            item,
            Some(item)
                if item.kind == "diff" && item.role == "tool" && item.state == "truncated"
        ),
        "turn.plan.updated" => !matches!(
            item,
            Some(item)
                if item.kind == "plan" && item.role == "agent" && item.state == "updated"
        ),
        "turn.plan.truncated" => !matches!(
            item,
            Some(item)
                if item.kind == "plan" && item.role == "agent" && item.state == "truncated"
        ),
        "turn.error-observed" => !matches!(
            item,
            Some(item)
                if item.kind == "error" && item.role == "system" && item.state == "updated"
        ),
        "turn.steering-requested" => !matches!(
            item,
            Some(item)
                if item.kind == "message" && item.role == "user" && item.state == "completed"
        ),
        "turn.steering-failed" | "turn.cancellation-failed" => !matches!(
            item,
            Some(item)
                if item.kind == "error" && item.role == "system" && item.state == "completed"
        ),
        "turn.error-observed.truncated"
        | "turn.steering-acknowledged"
        | "turn.cancellation-acknowledged" => item.is_some(),
        _ => item.is_some(),
    };
    if !invalid {
        return Ok(());
    }
    Err(if running {
        SequenceError::TurnViolation("invalid-event-shape")
    } else {
        SequenceError::Rejected("invalid-event-shape")
    })
}

fn terminal_phase(event: &str) -> Option<TurnPhase> {
    match event {
        "turn.completed" => Some(TurnPhase::Completed),
        "turn.failed" => Some(TurnPhase::Failed),
        "turn.interrupted" => Some(TurnPhase::Interrupted),
        _ => None,
    }
}

fn turn_state(phase: TurnPhase) -> TurnState {
    match phase {
        TurnPhase::Running => TurnState::Running,
        TurnPhase::Completed => TurnState::Completed,
        TurnPhase::Failed => TurnState::Failed,
        TurnPhase::Interrupted => TurnState::Interrupted,
    }
}

fn apply_item(
    turn: &mut TurnSequence,
    event: &str,
    item: &TimelineItem,
) -> Result<ItemUpdate, SequenceError> {
    if item.validate().is_err() || item.id.starts_with("sequencer-error:sha256:") {
        return Err(SequenceError::TurnViolation("invalid-item-identity"));
    }
    let current = turn.items.get_mut(&item.id);
    let revision = match (event, item.state.as_str(), current) {
        ("item.started", "started", None) => {
            turn.items.insert(
                item.id.clone(),
                ItemSequence {
                    revision: 1,
                    phase: ItemPhase::Streaming,
                    kind: item.kind.clone(),
                    role: item.role.clone(),
                },
            );
            1
        }
        ("item.delta", "delta", Some(current))
            if current.phase == ItemPhase::Streaming && same_shape(current, item) =>
        {
            advance_revision(current)?
        }
        ("item.completed", "completed", Some(current))
            if current.phase == ItemPhase::Streaming && same_shape(current, item) =>
        {
            current.phase = ItemPhase::Terminal;
            advance_revision(current)?
        }
        (_, "completed", None) | (_, "truncated", None) => {
            turn.items.insert(
                item.id.clone(),
                ItemSequence {
                    revision: 1,
                    phase: ItemPhase::Terminal,
                    kind: item.kind.clone(),
                    role: item.role.clone(),
                },
            );
            1
        }
        (_, "updated", None) => {
            turn.items.insert(
                item.id.clone(),
                ItemSequence {
                    revision: 1,
                    phase: ItemPhase::Updating,
                    kind: item.kind.clone(),
                    role: item.role.clone(),
                },
            );
            1
        }
        (_, "updated", Some(current))
            if current.phase == ItemPhase::Updating && same_shape(current, item) =>
        {
            advance_revision(current)?
        }
        (_, "truncated", Some(current))
            if current.phase == ItemPhase::Updating && same_shape(current, item) =>
        {
            current.phase = ItemPhase::Terminal;
            advance_revision(current)?
        }
        (_, "completed", Some(current))
            if current.phase == ItemPhase::Updating && same_shape(current, item) =>
        {
            current.phase = ItemPhase::Terminal;
            advance_revision(current)?
        }
        _ => return Err(SequenceError::TurnViolation("invalid-item-order")),
    };
    Ok(ItemUpdate {
        revision,
        content_mode: SNAPSHOT_REPLACEMENT.into(),
    })
}

fn same_shape(current: &ItemSequence, item: &TimelineItem) -> bool {
    current.kind == item.kind && current.role == item.role
}

fn advance_revision(item: &mut ItemSequence) -> Result<u64, SequenceError> {
    let revision = item
        .revision
        .checked_add(1)
        .filter(|revision| *revision <= MAX_SAFE_JSON_INTEGER)
        .ok_or(SequenceError::Bounds("item-revision-exhausted"))?;
    item.revision = revision;
    Ok(revision)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn item(id: &str, state: &str, content: &str) -> TimelineItem {
        TimelineItem {
            id: id.into(),
            kind: "message".into(),
            role: "agent".into(),
            state: state.into(),
            content: content.into(),
            data: None,
        }
    }

    fn plan_item(state: &str, content: &str) -> TimelineItem {
        TimelineItem {
            id: "plan".into(),
            kind: "plan".into(),
            role: "agent".into(),
            state: state.into(),
            content: content.into(),
            data: Some(json!({"steps": []})),
        }
    }

    fn error_item(code: &str) -> TimelineItem {
        TimelineItem {
            id: format!("error-{code}"),
            kind: "error".into(),
            role: "system".into(),
            state: "completed".into(),
            content: "Runtime rejected an invalid event lifecycle".into(),
            data: Some(json!({
                "schema_version": "runtime-error/0.1",
                "class": "protocol",
                "retryable": false,
                "operation": "event.sequencing",
                "code": code,
                "terminal_persisted": true
            })),
        }
    }

    fn completed_turn_events(session_id: &str, turn_id: &str) -> Vec<EventEnvelope> {
        let mut sequencer = EventSequencer::default();
        vec![
            sequencer
                .sequence(10, session_id, turn_id, "turn.started", None)
                .unwrap(),
            sequencer
                .sequence(
                    11,
                    session_id,
                    turn_id,
                    "item.completed",
                    Some(item("user", "completed", "request")),
                )
                .unwrap(),
            sequencer
                .sequence(
                    12,
                    session_id,
                    turn_id,
                    "item.started",
                    Some(item("agent", "started", "")),
                )
                .unwrap(),
            sequencer
                .sequence(
                    13,
                    session_id,
                    turn_id,
                    "item.delta",
                    Some(item("agent", "delta", "response")),
                )
                .unwrap(),
            sequencer
                .sequence(
                    14,
                    session_id,
                    turn_id,
                    "item.completed",
                    Some(item("agent", "completed", "response")),
                )
                .unwrap(),
            sequencer
                .sequence(15, session_id, turn_id, "turn.completed", None)
                .unwrap(),
        ]
    }

    #[test]
    fn prepared_ticket_advances_only_when_committed() {
        let mut sequencer = EventSequencer::default();
        let abandoned_id = {
            let abandoned = sequencer
                .prepare(10, "session", "turn", "turn.started", None)
                .unwrap();
            assert_eq!(abandoned.envelope().sequence, 1);
            abandoned.envelope().event_id.clone()
        };
        assert!(!sequencer.sessions.contains_key("session"));

        let committed = sequencer
            .prepare(10, "session", "turn", "turn.started", None)
            .unwrap();
        let committed = committed.commit(&mut sequencer).unwrap();
        assert_eq!(committed.event_id, abandoned_id);
        assert_eq!(sequencer.sessions["session"].sequence, 1);

        {
            let abandoned_item = sequencer
                .prepare(
                    11,
                    "session",
                    "turn",
                    "item.completed",
                    Some(item("candidate", "completed", "discarded")),
                )
                .unwrap();
            assert_eq!(abandoned_item.envelope().sequence, 2);
        }

        let committed_item = sequencer
            .prepare(
                12,
                "session",
                "turn",
                "item.completed",
                Some(item("committed", "completed", "kept")),
            )
            .unwrap();
        let committed_item = committed_item.commit(&mut sequencer).unwrap();
        assert_eq!(committed_item.sequence, 2);
        assert_eq!(committed_item.item_update.unwrap().revision, 1);
        let turn = &sequencer.sessions["session"].turns["turn"];
        assert!(!turn.items.contains_key("candidate"));
        assert!(turn.items.contains_key("committed"));
    }

    #[test]
    fn stale_prepared_ticket_cannot_overwrite_a_committed_event() {
        let mut sequencer = EventSequencer::default();
        let first = sequencer
            .prepare(10, "session", "turn-a", "turn.started", None)
            .unwrap();
        let stale = sequencer
            .prepare(11, "session", "turn-b", "turn.started", None)
            .unwrap();

        let committed = first.commit(&mut sequencer).unwrap();
        assert_eq!(committed.sequence, 1);
        assert_eq!(
            stale.commit(&mut sequencer),
            Err(SequenceError::Rejected("prepared-event-stale"))
        );
        assert_eq!(sequencer.sessions["session"].sequence, 1);
        assert!(sequencer.sessions["session"].turns.contains_key("turn-a"));
        assert!(!sequencer.sessions["session"].turns.contains_key("turn-b"));
    }

    #[test]
    fn restores_page_by_page_and_installs_only_the_complete_candidate() {
        let events = completed_turn_events("session", "turn");
        let watermark = events.last().unwrap();
        let mut candidate = EventSequencer::begin_session_restore(
            "session",
            watermark.sequence,
            Some(&watermark.event_id),
        )
        .unwrap();
        let mut target = EventSequencer::default();

        for page in events.chunks(2) {
            for event in page {
                candidate = candidate.replay_event(event).unwrap();
            }
            assert!(!target.sessions.contains_key("session"));
        }

        let restored = candidate.complete().unwrap();
        assert!(!target.sessions.contains_key("session"));
        target.install_restored_session(restored);
        assert_eq!(target.sessions["session"].sequence, watermark.sequence);
        assert_eq!(
            target.sessions["session"].timestamp_ms,
            watermark.timestamp_ms
        );

        let next = target
            .sequence(16, "session", "next-turn", "turn.started", None)
            .unwrap();
        assert_eq!(next.sequence, watermark.sequence + 1);
    }

    #[test]
    fn incomplete_or_invalid_restore_cannot_replace_live_state() {
        let events = completed_turn_events("session", "restored-turn");
        let watermark = events.last().unwrap();
        let mut target = EventSequencer::default();
        target
            .sequence(20, "session", "live-turn", "turn.started", None)
            .unwrap();

        let partial = EventSequencer::begin_session_restore(
            "session",
            watermark.sequence,
            Some(&watermark.event_id),
        )
        .unwrap()
        .replay_event(&events[0])
        .unwrap();
        let incomplete = match partial.complete() {
            Ok(_) => panic!("partial restore unexpectedly completed"),
            Err(error) => error,
        };
        assert_eq!(
            incomplete,
            SequenceError::Rejected("restored-session-incomplete")
        );

        let candidate = EventSequencer::begin_session_restore(
            "session",
            watermark.sequence,
            Some(&watermark.event_id),
        )
        .unwrap()
        .replay_event(&events[0])
        .unwrap();
        let mut forged = events[1].clone();
        forged.timestamp_ms += 1;
        let invalid = match candidate.replay_event(&forged) {
            Ok(_) => panic!("forged restore event unexpectedly replayed"),
            Err(error) => error,
        };
        assert_eq!(invalid, SequenceError::Rejected("restored-event-invalid"));

        assert_eq!(target.sessions["session"].sequence, 1);
        assert!(target.sessions["session"].turns.contains_key("live-turn"));
        assert!(!target.sessions["session"]
            .turns
            .contains_key("restored-turn"));
        let terminal = target
            .sequence(21, "session", "live-turn", "turn.completed", None)
            .unwrap();
        assert_eq!(terminal.sequence, 2);
    }

    #[test]
    fn restore_requires_the_exact_final_event_watermark() {
        let events = completed_turn_events("session", "turn");
        let watermark = events.last().unwrap();
        let forged_event_id = format!("event:sha256:{}", "a".repeat(64));
        assert_ne!(forged_event_id, watermark.event_id);
        let mut candidate = EventSequencer::begin_session_restore(
            "session",
            watermark.sequence,
            Some(&forged_event_id),
        )
        .unwrap();
        for event in &events {
            candidate = candidate.replay_event(event).unwrap();
        }
        let error = match candidate.complete() {
            Ok(_) => panic!("forged restore watermark unexpectedly completed"),
            Err(error) => error,
        };
        assert_eq!(
            error,
            SequenceError::Rejected("restored-watermark-mismatch")
        );
    }

    #[test]
    fn sequences_streams_with_non_decreasing_time_and_content_bound_ids() {
        let mut sequencer = EventSequencer::default();
        let started = sequencer
            .sequence(20, "session", "turn", "turn.started", None)
            .unwrap();
        let item_started = sequencer
            .sequence(
                19,
                "session",
                "turn",
                "item.started",
                Some(item("item", "started", "")),
            )
            .unwrap();
        let delta = sequencer
            .sequence(
                21,
                "session",
                "turn",
                "item.delta",
                Some(item("item", "delta", "a")),
            )
            .unwrap();
        let completed = sequencer
            .sequence(
                21,
                "session",
                "turn",
                "item.completed",
                Some(item("item", "completed", "ab")),
            )
            .unwrap();
        let terminal = sequencer
            .sequence(18, "session", "turn", "turn.completed", None)
            .unwrap();

        assert_eq!(started.sequence, 1);
        assert_eq!(terminal.sequence, 5);
        assert_eq!(item_started.timestamp_ms, 20);
        assert_eq!(terminal.timestamp_ms, 21);
        assert_eq!(item_started.item_update.unwrap().revision, 1);
        assert_eq!(delta.item_update.unwrap().revision, 2);
        assert_eq!(completed.item_update.unwrap().revision, 3);
        assert_eq!(terminal.turn_state, TurnState::Completed);
        assert!(terminal.event_id.starts_with("event:sha256:"));
        assert_eq!(terminal.event_id.len(), "event:sha256:".len() + 64);
        assert_eq!(terminal.correlation_id, terminal.turn_id);
    }

    #[test]
    fn supports_repeated_updates_then_truncation() {
        let mut sequencer = EventSequencer::default();
        sequencer
            .sequence(1, "session", "turn", "turn.started", None)
            .unwrap();
        let first = sequencer
            .sequence(
                2,
                "session",
                "turn",
                "turn.plan.updated",
                Some(plan_item("updated", "one")),
            )
            .unwrap();
        let second = sequencer
            .sequence(
                3,
                "session",
                "turn",
                "turn.plan.updated",
                Some(plan_item("updated", "two")),
            )
            .unwrap();
        let truncated = sequencer
            .sequence(
                4,
                "session",
                "turn",
                "turn.plan.truncated",
                Some(plan_item("truncated", "limit")),
            )
            .unwrap();
        assert_eq!(first.item_update.unwrap().revision, 1);
        assert_eq!(second.item_update.unwrap().revision, 2);
        assert_eq!(truncated.item_update.unwrap().revision, 3);
        assert!(sequencer
            .sequence(5, "session", "turn", "turn.completed", None)
            .is_ok());
    }

    #[test]
    fn atomic_truncation_marker_starts_at_revision_one() {
        let mut sequencer = EventSequencer::default();
        sequencer
            .sequence(1, "session", "turn", "turn.started", None)
            .unwrap();
        let marker = TimelineItem {
            id: "usage-truncated".into(),
            kind: "usage".into(),
            role: "system".into(),
            state: "truncated".into(),
            content: "Token usage updates truncated".into(),
            data: Some(json!({"max_updates": 32})),
        };
        let event = sequencer
            .sequence(2, "session", "turn", "usage.truncated", Some(marker))
            .unwrap();
        assert_eq!(event.item_update.unwrap().revision, 1);
        assert!(sequencer
            .sequence(3, "session", "turn", "turn.completed", None)
            .is_ok());
    }

    #[test]
    fn out_of_order_or_open_item_terminal_requires_one_explicit_failed_terminal() {
        let mut sequencer = EventSequencer::default();
        sequencer
            .sequence(1, "session", "turn", "turn.started", None)
            .unwrap();
        let failure = sequencer
            .sequence(
                2,
                "session",
                "turn",
                "item.delta",
                Some(item("item", "delta", "bad")),
            )
            .unwrap_err();
        assert_eq!(failure, SequenceError::TurnViolation("invalid-item-order"));
        let failed = sequencer
            .sequence(
                3,
                "session",
                "turn",
                "turn.failed",
                Some(error_item(failure.code())),
            )
            .unwrap();
        assert_eq!(failed.sequence, 2);
        assert_eq!(failed.turn_state, TurnState::Failed);
        assert!(sequencer
            .sequence(
                4,
                "session",
                "turn",
                "turn.failed",
                Some(error_item("late"))
            )
            .is_err());

        sequencer
            .sequence(5, "other", "turn-2", "turn.started", None)
            .unwrap();
        sequencer
            .sequence(
                6,
                "other",
                "turn-2",
                "item.started",
                Some(item("open", "started", "")),
            )
            .unwrap();
        let failure = sequencer
            .sequence(6, "other", "turn-2", "turn.completed", None)
            .unwrap_err();
        assert_eq!(
            failure,
            SequenceError::TurnViolation("open-item-at-turn-terminal")
        );
        let failed = sequencer
            .sequence(
                7,
                "other",
                "turn-2",
                "turn.failed",
                Some(error_item(failure.code())),
            )
            .unwrap();
        assert_eq!(failed.sequence, 3);
    }

    #[test]
    fn invalid_or_reserved_provider_item_identity_requires_runtime_failure() {
        for provider_id in [
            &"x".repeat(129),
            &format!("sequencer-error:sha256:{}", "a".repeat(64)),
        ] {
            let mut sequencer = EventSequencer::default();
            sequencer
                .sequence(1, "session", "turn", "turn.started", None)
                .unwrap();
            let failure = sequencer
                .sequence(
                    2,
                    "session",
                    "turn",
                    "item.completed",
                    Some(item(provider_id, "completed", "invalid")),
                )
                .unwrap_err();
            assert!(failure.closes_turn());
            let failed = sequencer
                .sequence(
                    3,
                    "session",
                    "turn",
                    "turn.failed",
                    Some(error_item(failure.code())),
                )
                .unwrap();
            assert_ne!(failed.item.as_ref().unwrap().id, *provider_id);
        }
    }

    #[test]
    fn sessions_have_independent_sequences_and_terminal_rejects_late_events() {
        let mut sequencer = EventSequencer::default();
        let first = sequencer
            .sequence(10, "one", "turn-one", "turn.started", None)
            .unwrap();
        let second = sequencer
            .sequence(11, "two", "turn-two", "turn.started", None)
            .unwrap();
        assert_eq!(first.sequence, 1);
        assert_eq!(second.sequence, 1);
        sequencer
            .sequence(12, "one", "turn-one", "turn.completed", None)
            .unwrap();
        assert!(sequencer
            .sequence(
                13,
                "one",
                "turn-one",
                "item.completed",
                Some(item("late", "completed", "late")),
            )
            .is_err());
    }

    #[test]
    fn failed_serialization_does_not_consume_item_state_or_sequence() {
        let mut sequencer = EventSequencer::default();
        sequencer
            .sequence(10, "session", "turn", "turn.started", None)
            .unwrap();
        assert!(sequencer
            .sequence(
                MAX_SAFE_JSON_INTEGER + 1,
                "session",
                "turn",
                "item.completed",
                Some(item("valid", "completed", "valid")),
            )
            .is_err());
        let valid = sequencer
            .sequence(
                12,
                "session",
                "turn",
                "item.completed",
                Some(item("valid", "completed", "valid")),
            )
            .unwrap();
        assert_eq!(valid.sequence, 2);
        assert_eq!(valid.item_update.unwrap().revision, 1);
    }

    #[test]
    fn malformed_known_shape_rolls_back_until_runtime_persists_failure() {
        let mut sequencer = EventSequencer::default();
        sequencer
            .sequence(10, "session", "turn", "turn.started", None)
            .unwrap();
        let failure = sequencer
            .sequence(11, "session", "turn", "turn.failed", None)
            .unwrap_err();
        assert_eq!(failure, SequenceError::TurnViolation("invalid-event-shape"));
        let failed = sequencer
            .sequence(
                12,
                "session",
                "turn",
                "turn.failed",
                Some(error_item(failure.code())),
            )
            .unwrap();
        assert_eq!(failed.sequence, 2);
        assert_eq!(failed.event, "turn.failed");
    }

    #[test]
    fn revision_exhaustion_advances_no_sequence_or_item_cursor() {
        let mut sequencer = EventSequencer::default();
        sequencer
            .sequence(10, "session", "turn", "turn.started", None)
            .unwrap();
        let session = sequencer.sessions.get_mut("session").unwrap();
        let turn = session.turns.get_mut("turn").unwrap();
        turn.items.insert(
            "item".into(),
            ItemSequence {
                revision: MAX_SAFE_JSON_INTEGER,
                phase: ItemPhase::Streaming,
                kind: "message".into(),
                role: "agent".into(),
            },
        );

        assert_eq!(
            sequencer
                .sequence(
                    11,
                    "session",
                    "turn",
                    "item.delta",
                    Some(item("item", "delta", "overflow")),
                )
                .unwrap_err(),
            SequenceError::Bounds("item-revision-exhausted")
        );
        let session = sequencer.sessions.get("session").unwrap();
        assert_eq!(session.sequence, 1);
        assert_eq!(
            session.turns["turn"].items["item"].revision,
            MAX_SAFE_JSON_INTEGER
        );
    }

    #[test]
    fn immutable_field_changes_change_event_identity() {
        fn second_event(timestamp_ms: u64, content: &str) -> EventEnvelope {
            let mut sequencer = EventSequencer::default();
            sequencer
                .sequence(10, "session", "turn", "turn.started", None)
                .unwrap();
            sequencer
                .sequence(
                    timestamp_ms,
                    "session",
                    "turn",
                    "item.completed",
                    Some(item("item", "completed", content)),
                )
                .unwrap()
        }

        let baseline = second_event(11, "one");
        assert_ne!(baseline.event_id, second_event(12, "one").event_id);
        assert_ne!(baseline.event_id, second_event(11, "two").event_id);
    }
}
