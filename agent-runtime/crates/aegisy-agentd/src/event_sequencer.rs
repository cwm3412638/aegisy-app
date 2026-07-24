use aegisy_aap::stable::v0_1::{
    timeline_event_id, EventEnvelope, ItemUpdate, TimelineItem, TurnState,
};
use aegisy_aap::{MAX_SAFE_JSON_INTEGER, MAX_TIMELINE_IDENTIFIER_BYTES};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::HashMap;

const EVENT_SCHEMA_VERSION: &str = "timeline-event/0.1";
const SNAPSHOT_REPLACEMENT: &str = "snapshot-replacement";
const CHECKPOINT_SCHEMA_VERSION: &str = "event-sequencer-checkpoint/0.1";
const CHECKPOINT_IDENTITY_PREFIX: &str = "event-sequencer-checkpoint:sha256:";
const CHECKPOINT_IDENTITY_DOMAIN: &[u8] = b"aegisy-event-sequencer-checkpoint/0.1\0";
pub(crate) const MAX_CHECKPOINT_BYTES: usize = 16 * 1024 * 1024;
pub(crate) const MAX_CHECKPOINT_TURNS: usize = 100_000;
pub(crate) const MAX_CHECKPOINT_ITEMS: usize = 100_000;

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

#[derive(Debug)]
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

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub(crate) struct EventSequencerCheckpoint {
    schema_version: String,
    session_id: String,
    anchor: CheckpointAnchor,
    turns: Vec<CheckpointTurn>,
    checkpoint_identity: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct CheckpointAnchor {
    sequence: u64,
    timestamp_ms: u64,
    event_id: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct CheckpointTurn {
    turn_id: String,
    phase: String,
    items: Vec<CheckpointItem>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct CheckpointItem {
    item_id: String,
    revision: u64,
    phase: String,
    kind: String,
    role: String,
}

#[derive(Serialize)]
struct CheckpointIdentityMaterial<'a> {
    schema_version: &'a str,
    session_id: &'a str,
    anchor: &'a CheckpointAnchor,
    turns: &'a [CheckpointTurn],
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

impl EventSequencerCheckpoint {
    fn from_session(
        session_id: &str,
        session: Option<&SessionSequence>,
    ) -> Result<Self, SequenceError> {
        if !valid_binding_identity(session_id) {
            return Err(SequenceError::Rejected(
                "checkpoint-session-identity-invalid",
            ));
        }
        let (anchor, mut turns) = match session {
            Some(session) => {
                let mut turns = session
                    .turns
                    .iter()
                    .map(|(turn_id, turn)| {
                        let mut items = if turn.phase == TurnPhase::Running {
                            turn.items
                                .iter()
                                .map(|(item_id, item)| CheckpointItem {
                                    item_id: item_id.clone(),
                                    revision: item.revision,
                                    phase: checkpoint_item_phase(item.phase).into(),
                                    kind: item.kind.clone(),
                                    role: item.role.clone(),
                                })
                                .collect::<Vec<_>>()
                        } else {
                            Vec::new()
                        };
                        items.sort_by(|left, right| left.item_id.cmp(&right.item_id));
                        CheckpointTurn {
                            turn_id: turn_id.clone(),
                            phase: checkpoint_turn_phase(turn.phase).into(),
                            items,
                        }
                    })
                    .collect::<Vec<_>>();
                turns.sort_by(|left, right| left.turn_id.cmp(&right.turn_id));
                (
                    CheckpointAnchor {
                        sequence: session.sequence,
                        timestamp_ms: session.timestamp_ms,
                        event_id: session.last_event_id.clone(),
                    },
                    turns,
                )
            }
            None => (
                CheckpointAnchor {
                    sequence: 0,
                    timestamp_ms: 0,
                    event_id: None,
                },
                Vec::new(),
            ),
        };
        turns.shrink_to_fit();
        let mut checkpoint = Self {
            schema_version: CHECKPOINT_SCHEMA_VERSION.into(),
            session_id: session_id.into(),
            anchor,
            turns,
            checkpoint_identity: String::new(),
        };
        checkpoint.checkpoint_identity = checkpoint_identity(&checkpoint)?;
        checkpoint.validate_for_session(session_id)?;
        if checkpoint.to_canonical_json().len() > MAX_CHECKPOINT_BYTES {
            return Err(SequenceError::Bounds("checkpoint-json-size-invalid"));
        }
        Ok(checkpoint)
    }

    pub(crate) fn from_canonical_json(
        expected_session_id: &str,
        json: &str,
    ) -> Result<Self, SequenceError> {
        if json.is_empty() || json.len() > MAX_CHECKPOINT_BYTES {
            return Err(SequenceError::Bounds("checkpoint-json-size-invalid"));
        }
        let checkpoint: Self = serde_json::from_str(json)
            .map_err(|_| SequenceError::Rejected("checkpoint-json-invalid"))?;
        checkpoint.validate_for_session(expected_session_id)?;
        if checkpoint.to_canonical_json() != json {
            return Err(SequenceError::Rejected("checkpoint-json-noncanonical"));
        }
        Ok(checkpoint)
    }

    pub(crate) fn to_canonical_json(&self) -> String {
        serde_json::to_string(self).expect("checkpoint contains only serializable values")
    }

    pub(crate) fn validate_for_session(
        &self,
        expected_session_id: &str,
    ) -> Result<(), SequenceError> {
        if self.schema_version != CHECKPOINT_SCHEMA_VERSION {
            return Err(SequenceError::Rejected("checkpoint-schema-unsupported"));
        }
        if !valid_binding_identity(expected_session_id)
            || self.session_id != expected_session_id
            || !valid_binding_identity(&self.session_id)
        {
            return Err(SequenceError::Rejected("checkpoint-session-mismatch"));
        }
        if self.anchor.sequence > MAX_SAFE_JSON_INTEGER
            || self.anchor.timestamp_ms > MAX_SAFE_JSON_INTEGER
        {
            return Err(SequenceError::Bounds("checkpoint-anchor-out-of-range"));
        }
        let empty_anchor = self.anchor.sequence == 0
            && self.anchor.timestamp_ms == 0
            && self.anchor.event_id.is_none();
        let populated_anchor = self.anchor.sequence > 0
            && self.anchor.timestamp_ms > 0
            && self.anchor.event_id.as_deref().is_some_and(valid_event_id);
        if !empty_anchor && !populated_anchor {
            return Err(SequenceError::Rejected("checkpoint-anchor-invalid"));
        }
        if empty_anchor != self.turns.is_empty() {
            return Err(SequenceError::Rejected("checkpoint-empty-state-invalid"));
        }
        if self.turns.len() > MAX_CHECKPOINT_TURNS {
            return Err(SequenceError::Bounds("checkpoint-turn-limit"));
        }

        let mut previous_turn_id: Option<&str> = None;
        let mut running_turns = 0_u64;
        let mut minimum_sequence = 0_u64;
        let mut item_count = 0_usize;
        for turn in &self.turns {
            if !valid_binding_identity(&turn.turn_id)
                || previous_turn_id.is_some_and(|previous| previous >= turn.turn_id.as_str())
            {
                return Err(SequenceError::Rejected("checkpoint-turn-order-invalid"));
            }
            previous_turn_id = Some(&turn.turn_id);
            let phase = parse_checkpoint_turn_phase(&turn.phase)?;
            minimum_sequence = minimum_sequence
                .checked_add(1)
                .ok_or(SequenceError::Bounds("checkpoint-state-out-of-range"))?;
            if phase == TurnPhase::Running {
                running_turns = running_turns
                    .checked_add(1)
                    .ok_or(SequenceError::Bounds("checkpoint-state-out-of-range"))?;
            } else {
                minimum_sequence = minimum_sequence
                    .checked_add(1)
                    .ok_or(SequenceError::Bounds("checkpoint-state-out-of-range"))?;
                if !turn.items.is_empty() {
                    return Err(SequenceError::Rejected(
                        "checkpoint-terminal-turn-items-present",
                    ));
                }
            }

            let mut previous_item_id: Option<&str> = None;
            for item in &turn.items {
                item_count = item_count
                    .checked_add(1)
                    .ok_or(SequenceError::Bounds("checkpoint-item-limit"))?;
                if item_count > MAX_CHECKPOINT_ITEMS {
                    return Err(SequenceError::Bounds("checkpoint-item-limit"));
                }
                if !valid_binding_identity(&item.item_id)
                    || previous_item_id.is_some_and(|previous| previous >= item.item_id.as_str())
                {
                    return Err(SequenceError::Rejected("checkpoint-item-order-invalid"));
                }
                previous_item_id = Some(&item.item_id);
                if item.revision == 0 || item.revision > MAX_SAFE_JSON_INTEGER {
                    return Err(SequenceError::Bounds(
                        "checkpoint-item-revision-out-of-range",
                    ));
                }
                parse_checkpoint_item_phase(&item.phase)?;
                if !valid_binding_identity(&item.kind) || !valid_binding_identity(&item.role) {
                    return Err(SequenceError::Rejected("checkpoint-item-shape-invalid"));
                }
                minimum_sequence = minimum_sequence
                    .checked_add(item.revision)
                    .ok_or(SequenceError::Bounds("checkpoint-state-out-of-range"))?;
            }
        }
        if running_turns > 1 {
            return Err(SequenceError::Rejected("checkpoint-multiple-running-turns"));
        }
        if minimum_sequence > self.anchor.sequence {
            return Err(SequenceError::Rejected("checkpoint-state-after-anchor"));
        }
        if self.checkpoint_identity.len() != CHECKPOINT_IDENTITY_PREFIX.len() + 64
            || !self
                .checkpoint_identity
                .strip_prefix(CHECKPOINT_IDENTITY_PREFIX)
                .is_some_and(|digest| {
                    digest
                        .bytes()
                        .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
                })
            || checkpoint_identity(self)? != self.checkpoint_identity
        {
            return Err(SequenceError::Rejected("checkpoint-identity-invalid"));
        }
        Ok(())
    }

    #[cfg(test)]
    pub(crate) fn session_id(&self) -> &str {
        &self.session_id
    }

    pub(crate) fn sequence(&self) -> u64 {
        self.anchor.sequence
    }

    pub(crate) fn timestamp_ms(&self) -> u64 {
        self.anchor.timestamp_ms
    }

    pub(crate) fn event_id(&self) -> Option<&str> {
        self.anchor.event_id.as_deref()
    }

    pub(crate) fn identity(&self) -> &str {
        &self.checkpoint_identity
    }

    pub(crate) fn turn_count(&self) -> u64 {
        self.turns.len() as u64
    }

    pub(crate) fn item_count(&self) -> u64 {
        self.turns.iter().map(|turn| turn.items.len() as u64).sum()
    }

    fn session_sequence(&self) -> Result<SessionSequence, SequenceError> {
        self.validate_for_session(&self.session_id)?;
        let mut turns = HashMap::with_capacity(self.turns.len());
        for turn in &self.turns {
            let phase = parse_checkpoint_turn_phase(&turn.phase)?;
            let mut items = HashMap::with_capacity(turn.items.len());
            for item in &turn.items {
                items.insert(
                    item.item_id.clone(),
                    ItemSequence {
                        revision: item.revision,
                        phase: parse_checkpoint_item_phase(&item.phase)?,
                        kind: item.kind.clone(),
                        role: item.role.clone(),
                    },
                );
            }
            turns.insert(turn.turn_id.clone(), TurnSequence { phase, items });
        }
        Ok(SessionSequence {
            sequence: self.anchor.sequence,
            timestamp_ms: self.anchor.timestamp_ms,
            last_event_id: self.anchor.event_id.clone(),
            turns,
        })
    }
}

fn checkpoint_identity(checkpoint: &EventSequencerCheckpoint) -> Result<String, SequenceError> {
    let material = CheckpointIdentityMaterial {
        schema_version: &checkpoint.schema_version,
        session_id: &checkpoint.session_id,
        anchor: &checkpoint.anchor,
        turns: &checkpoint.turns,
    };
    let bytes = serde_json::to_vec(&material)
        .map_err(|_| SequenceError::Rejected("checkpoint-identity-serialization-failed"))?;
    let length = u64::try_from(bytes.len())
        .map_err(|_| SequenceError::Bounds("checkpoint-identity-material-too-large"))?;
    let mut digest = Sha256::new();
    digest.update(CHECKPOINT_IDENTITY_DOMAIN);
    digest.update(length.to_be_bytes());
    digest.update(&bytes);
    Ok(format!(
        "{CHECKPOINT_IDENTITY_PREFIX}{:x}",
        digest.finalize()
    ))
}

fn checkpoint_turn_phase(phase: TurnPhase) -> &'static str {
    match phase {
        TurnPhase::Running => "running",
        TurnPhase::Completed => "completed",
        TurnPhase::Failed => "failed",
        TurnPhase::Interrupted => "interrupted",
    }
}

fn parse_checkpoint_turn_phase(value: &str) -> Result<TurnPhase, SequenceError> {
    match value {
        "running" => Ok(TurnPhase::Running),
        "completed" => Ok(TurnPhase::Completed),
        "failed" => Ok(TurnPhase::Failed),
        "interrupted" => Ok(TurnPhase::Interrupted),
        _ => Err(SequenceError::Rejected("checkpoint-turn-phase-invalid")),
    }
}

fn checkpoint_item_phase(phase: ItemPhase) -> &'static str {
    match phase {
        ItemPhase::Streaming => "streaming",
        ItemPhase::Updating => "updating",
        ItemPhase::Terminal => "terminal",
    }
}

fn parse_checkpoint_item_phase(value: &str) -> Result<ItemPhase, SequenceError> {
    match value {
        "streaming" => Ok(ItemPhase::Streaming),
        "updating" => Ok(ItemPhase::Updating),
        "terminal" => Ok(ItemPhase::Terminal),
        _ => Err(SequenceError::Rejected("checkpoint-item-phase-invalid")),
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
    #[allow(dead_code)]
    pub(crate) fn checkpoint_at_current(&self) -> Result<EventSequencerCheckpoint, SequenceError> {
        EventSequencerCheckpoint::from_session(&self.session_id, Some(&self.session))
    }

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
    #[cfg(test)]
    pub(crate) fn checkpoint_for_session(
        &self,
        session_id: &str,
    ) -> Result<EventSequencerCheckpoint, SequenceError> {
        EventSequencerCheckpoint::from_session(session_id, self.sessions.get(session_id))
    }

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
        let checkpoint = EventSequencerCheckpoint::from_session(session_id, None)?;
        Self::begin_session_restore_from_checkpoint(
            session_id,
            &checkpoint,
            expected_sequence,
            expected_event_id,
        )
    }

    pub(crate) fn begin_session_restore_from_checkpoint(
        session_id: &str,
        checkpoint: &EventSequencerCheckpoint,
        expected_sequence: u64,
        expected_event_id: Option<&str>,
    ) -> Result<SessionRestoreCandidate, SequenceError> {
        checkpoint.validate_for_session(session_id)?;
        if expected_sequence > MAX_SAFE_JSON_INTEGER {
            return Err(SequenceError::Bounds("restored-sequence-out-of-range"));
        }
        if (expected_sequence == 0) != expected_event_id.is_none()
            || expected_event_id.is_some_and(|event_id| !valid_event_id(event_id))
        {
            return Err(SequenceError::Rejected("restored-watermark-invalid"));
        }
        if expected_sequence < checkpoint.sequence()
            || (expected_sequence == checkpoint.sequence()
                && expected_event_id != checkpoint.event_id())
        {
            return Err(SequenceError::Rejected(
                "restored-checkpoint-after-watermark",
            ));
        }
        Ok(SessionRestoreCandidate {
            session_id: session_id.to_owned(),
            expected_sequence,
            expected_event_id: expected_event_id.map(str::to_owned),
            session: checkpoint.session_sequence()?,
            last_event_id: checkpoint.event_id().map(str::to_owned),
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

    fn sequencer_from_events(events: &[EventEnvelope]) -> EventSequencer {
        let watermark = events.last().unwrap();
        let mut candidate = EventSequencer::begin_session_restore(
            &watermark.session_id,
            watermark.sequence,
            Some(&watermark.event_id),
        )
        .unwrap();
        for event in events {
            candidate = candidate.replay_event(event).unwrap();
        }
        let mut sequencer = EventSequencer::default();
        sequencer.install_restored_session(candidate.complete().unwrap());
        sequencer
    }

    fn reseal_checkpoint(checkpoint: &mut EventSequencerCheckpoint) {
        checkpoint.checkpoint_identity = checkpoint_identity(checkpoint).unwrap();
    }

    #[test]
    fn checkpoint_round_trip_seeds_terminal_state_without_content() {
        let events = completed_turn_events("session", "turn");
        let sequencer = sequencer_from_events(&events);
        let checkpoint = sequencer.checkpoint_for_session("session").unwrap();

        assert_eq!(checkpoint.session_id(), "session");
        assert_eq!(checkpoint.sequence(), 6);
        assert_eq!(checkpoint.timestamp_ms(), 15);
        assert_eq!(checkpoint.event_id(), Some(events[5].event_id.as_str()));
        assert_eq!(checkpoint.turn_count(), 1);
        assert_eq!(checkpoint.item_count(), 0);
        assert!(checkpoint
            .identity()
            .starts_with(CHECKPOINT_IDENTITY_PREFIX));
        let encoded = checkpoint.to_canonical_json();
        assert!(!encoded.contains("\"content\""));
        assert!(!encoded.contains("\"data\""));
        assert!(!encoded.contains("request"));
        assert!(!encoded.contains("response"));
        assert_eq!(
            EventSequencerCheckpoint::from_canonical_json("session", &encoded).unwrap(),
            checkpoint
        );

        let restored = EventSequencer::begin_session_restore_from_checkpoint(
            "session",
            &checkpoint,
            checkpoint.sequence(),
            checkpoint.event_id(),
        )
        .unwrap()
        .complete()
        .unwrap();
        let mut target = EventSequencer::default();
        target.install_restored_session(restored);
        assert_eq!(
            target.sequence(16, "session", "turn", "turn.started", None),
            Err(SequenceError::Rejected("turn-start-rejected"))
        );
        let next = target
            .sequence(16, "session", "next-turn", "turn.started", None)
            .unwrap();
        assert_eq!(next.sequence, 7);
    }

    #[test]
    fn checkpoint_restores_running_item_revision_and_shape() {
        let mut sequencer = EventSequencer::default();
        sequencer
            .sequence(10, "session", "turn", "turn.started", None)
            .unwrap();
        sequencer
            .sequence(
                11,
                "session",
                "turn",
                "item.started",
                Some(item("agent", "started", "first")),
            )
            .unwrap();
        sequencer
            .sequence(
                12,
                "session",
                "turn",
                "item.delta",
                Some(item("agent", "delta", "second")),
            )
            .unwrap();
        sequencer
            .sequence(
                13,
                "session",
                "turn",
                "turn.plan.updated",
                Some(plan_item("updated", "one")),
            )
            .unwrap();
        let checkpoint = sequencer.checkpoint_for_session("session").unwrap();
        assert_eq!(checkpoint.item_count(), 2);

        let restored = EventSequencer::begin_session_restore_from_checkpoint(
            "session",
            &checkpoint,
            checkpoint.sequence(),
            checkpoint.event_id(),
        )
        .unwrap()
        .complete()
        .unwrap();
        let mut target = EventSequencer::default();
        target.install_restored_session(restored);
        let completed = target
            .sequence(
                14,
                "session",
                "turn",
                "item.completed",
                Some(item("agent", "completed", "done")),
            )
            .unwrap();
        assert_eq!(completed.item_update.unwrap().revision, 3);
        let updated = target
            .sequence(
                15,
                "session",
                "turn",
                "turn.plan.updated",
                Some(plan_item("updated", "two")),
            )
            .unwrap();
        assert_eq!(updated.item_update.unwrap().revision, 2);
        assert_eq!(
            target.sequence(
                16,
                "session",
                "turn",
                "item.completed",
                Some(TimelineItem {
                    role: "tool".into(),
                    ..item("agent", "completed", "wrong shape")
                }),
            ),
            Err(SequenceError::TurnViolation("invalid-item-order"))
        );
    }

    #[test]
    fn checkpoint_is_canonical_and_rejects_semantic_or_identity_tampering() {
        let mut sequencer = EventSequencer::default();
        sequencer
            .sequence(10, "session", "turn-z", "turn.started", None)
            .unwrap();
        sequencer
            .sequence(11, "session", "turn-z", "turn.completed", None)
            .unwrap();
        sequencer
            .sequence(12, "session", "turn-a", "turn.started", None)
            .unwrap();
        sequencer
            .sequence(
                13,
                "session",
                "turn-a",
                "item.completed",
                Some(item("item-z", "completed", "z")),
            )
            .unwrap();
        sequencer
            .sequence(
                14,
                "session",
                "turn-a",
                "item.completed",
                Some(item("item-a", "completed", "a")),
            )
            .unwrap();
        let checkpoint = sequencer.checkpoint_for_session("session").unwrap();
        assert_eq!(
            checkpoint
                .turns
                .iter()
                .map(|turn| turn.turn_id.as_str())
                .collect::<Vec<_>>(),
            vec!["turn-a", "turn-z"]
        );
        assert_eq!(
            checkpoint.turns[0]
                .items
                .iter()
                .map(|item| item.item_id.as_str())
                .collect::<Vec<_>>(),
            vec!["item-a", "item-z"]
        );

        let canonical = checkpoint.to_canonical_json();
        let pretty = serde_json::to_string_pretty(&checkpoint).unwrap();
        assert_eq!(
            EventSequencerCheckpoint::from_canonical_json("session", &pretty),
            Err(SequenceError::Rejected("checkpoint-json-noncanonical"))
        );
        let mut with_unknown: serde_json::Value = serde_json::from_str(&canonical).unwrap();
        with_unknown["unknown"] = json!(true);
        assert_eq!(
            EventSequencerCheckpoint::from_canonical_json(
                "session",
                &serde_json::to_string(&with_unknown).unwrap()
            ),
            Err(SequenceError::Rejected("checkpoint-json-invalid"))
        );
        assert_eq!(
            EventSequencerCheckpoint::from_canonical_json("other-session", &canonical),
            Err(SequenceError::Rejected("checkpoint-session-mismatch"))
        );

        let mut invalid = checkpoint.clone();
        invalid.checkpoint_identity = format!("{CHECKPOINT_IDENTITY_PREFIX}{}", "a".repeat(64));
        assert_eq!(
            invalid.validate_for_session("session"),
            Err(SequenceError::Rejected("checkpoint-identity-invalid"))
        );

        let mut invalid = checkpoint.clone();
        invalid.turns.swap(0, 1);
        reseal_checkpoint(&mut invalid);
        assert_eq!(
            invalid.validate_for_session("session"),
            Err(SequenceError::Rejected("checkpoint-turn-order-invalid"))
        );

        let mut invalid = checkpoint.clone();
        invalid.turns.insert(1, invalid.turns[0].clone());
        reseal_checkpoint(&mut invalid);
        assert_eq!(
            invalid.validate_for_session("session"),
            Err(SequenceError::Rejected("checkpoint-turn-order-invalid"))
        );

        let mut invalid = checkpoint.clone();
        invalid.turns[0].phase = "unknown".into();
        reseal_checkpoint(&mut invalid);
        assert_eq!(
            invalid.validate_for_session("session"),
            Err(SequenceError::Rejected("checkpoint-turn-phase-invalid"))
        );

        let mut invalid = checkpoint.clone();
        invalid.turns[0].items[0].phase = "unknown".into();
        reseal_checkpoint(&mut invalid);
        assert_eq!(
            invalid.validate_for_session("session"),
            Err(SequenceError::Rejected("checkpoint-item-phase-invalid"))
        );

        let mut invalid = checkpoint.clone();
        invalid.anchor.sequence = 1;
        reseal_checkpoint(&mut invalid);
        assert_eq!(
            invalid.validate_for_session("session"),
            Err(SequenceError::Rejected("checkpoint-state-after-anchor"))
        );
    }

    #[test]
    fn checkpoint_restore_replays_only_the_retained_tail() {
        let events = completed_turn_events("session", "turn");
        let prefix = sequencer_from_events(&events[..2]);
        let checkpoint = prefix.checkpoint_for_session("session").unwrap();
        let mut candidate = EventSequencer::begin_session_restore_from_checkpoint(
            "session",
            &checkpoint,
            events.last().unwrap().sequence,
            Some(&events.last().unwrap().event_id),
        )
        .unwrap();
        for event in &events[2..] {
            candidate = candidate.replay_event(event).unwrap();
        }
        let restored = candidate.complete().unwrap();
        let mut target = EventSequencer::default();
        target.install_restored_session(restored);
        assert_eq!(target.sessions["session"].sequence, 6);

        assert_eq!(
            EventSequencer::begin_session_restore_from_checkpoint(
                "session",
                &checkpoint,
                1,
                Some(&events[0].event_id),
            )
            .unwrap_err(),
            SequenceError::Rejected("restored-checkpoint-after-watermark")
        );
        let mut forged = events[2].clone();
        forged.sequence += 1;
        let candidate = EventSequencer::begin_session_restore_from_checkpoint(
            "session",
            &checkpoint,
            events.last().unwrap().sequence,
            Some(&events.last().unwrap().event_id),
        )
        .unwrap();
        assert_eq!(
            candidate.replay_event(&forged).unwrap_err(),
            SequenceError::Rejected("restored-event-invalid")
        );
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
