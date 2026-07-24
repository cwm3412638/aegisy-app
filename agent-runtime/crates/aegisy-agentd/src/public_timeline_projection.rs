#![allow(dead_code)]

use aegisy_aap::stable::v0_1::{EventEnvelope, TimelineItem, TurnState};
use aegisy_aap::{MAX_SAFE_JSON_INTEGER, MAX_TIMELINE_IDENTIFIER_BYTES};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};

const FLOOR_SCHEMA_VERSION: &str = "public-timeline-visible-floor/0.1";
const FLOOR_IDENTITY_PREFIX: &str = "visible-floor:sha256:";
const FLOOR_IDENTITY_DOMAIN: &[u8] = b"aegisy-public-timeline-visible-floor/0.1\0";
pub(crate) const MAX_VISIBLE_FLOOR_BYTES: usize = 64 * 1024 * 1024;
pub(crate) const MAX_VISIBLE_FLOOR_TURNS: usize = 10_000;
pub(crate) const MAX_VISIBLE_FLOOR_ITEMS: usize = 10_000;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub(crate) struct ProjectionAnchor {
    pub(crate) sequence: u64,
    pub(crate) timestamp_ms: u64,
    pub(crate) event_id: Option<String>,
}

impl ProjectionAnchor {
    pub(crate) fn empty() -> Self {
        Self {
            sequence: 0,
            timestamp_ms: 0,
            event_id: None,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub(crate) struct VisibleTurnState {
    pub(crate) turn_id: String,
    pub(crate) state: TurnState,
    pub(crate) started_anchor: ProjectionAnchor,
    pub(crate) latest_anchor: ProjectionAnchor,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub(crate) struct VisibleTimelineItem {
    pub(crate) turn_id: String,
    pub(crate) revision: u64,
    pub(crate) first_anchor: ProjectionAnchor,
    pub(crate) latest_anchor: ProjectionAnchor,
    pub(crate) item: TimelineItem,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(deny_unknown_fields)]
pub(crate) struct OpenTimelineItem {
    pub(crate) turn_id: String,
    pub(crate) item_id: String,
    pub(crate) state: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub(crate) struct VisibleStateFloorSnapshot {
    pub(crate) schema_version: String,
    pub(crate) session_id: String,
    pub(crate) anchor: ProjectionAnchor,
    pub(crate) turns: Vec<VisibleTurnState>,
    pub(crate) items: Vec<VisibleTimelineItem>,
    pub(crate) running_turn_id: Option<String>,
    pub(crate) open_items: Vec<OpenTimelineItem>,
    pub(crate) snapshot_identity: String,
}

impl VisibleStateFloorSnapshot {
    pub(crate) fn new(
        session_id: String,
        anchor: ProjectionAnchor,
        turns: Vec<VisibleTurnState>,
        items: Vec<VisibleTimelineItem>,
        running_turn_id: Option<String>,
        open_items: Vec<OpenTimelineItem>,
    ) -> Result<Self, ProjectionError> {
        let mut snapshot = Self {
            schema_version: FLOOR_SCHEMA_VERSION.into(),
            session_id,
            anchor,
            turns,
            items,
            running_turn_id,
            open_items,
            snapshot_identity: String::new(),
        };
        snapshot.snapshot_identity = floor_identity(&snapshot)?;
        validate_floor_snapshot(&snapshot)?;
        Ok(snapshot)
    }

    pub(crate) fn from_canonical_json(
        expected_session_id: &str,
        json: &str,
    ) -> Result<Self, ProjectionError> {
        if json.is_empty() || json.len() > MAX_VISIBLE_FLOOR_BYTES {
            return Err(ProjectionError::Bounds("projection-floor-byte-limit"));
        }
        let snapshot: Self = serde_json::from_str(json)
            .map_err(|_| ProjectionError::InvalidFloor("projection-floor-json-invalid"))?;
        validate_floor_snapshot(&snapshot)?;
        if snapshot.session_id != expected_session_id {
            return Err(ProjectionError::SessionMismatch);
        }
        if snapshot.to_canonical_json()? != json {
            return Err(ProjectionError::InvalidFloor(
                "projection-floor-json-noncanonical",
            ));
        }
        Ok(snapshot)
    }

    pub(crate) fn to_canonical_json(&self) -> Result<String, ProjectionError> {
        validate_floor_snapshot(self)?;
        serde_json::to_string(self)
            .map_err(|_| ProjectionError::InvalidFloor("projection-floor-json-invalid"))
    }

    pub(crate) fn identity(&self) -> &str {
        &self.snapshot_identity
    }
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
struct ItemIdentity {
    turn_id: String,
    item_id: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct PublicTimelineProjection {
    session_id: String,
    anchor: ProjectionAnchor,
    turns: BTreeMap<String, VisibleTurnState>,
    turn_order: BTreeMap<u64, String>,
    items: BTreeMap<ItemIdentity, VisibleTimelineItem>,
    item_order: BTreeMap<u64, ItemIdentity>,
    running_turn_id: Option<String>,
    open_items: BTreeSet<ItemIdentity>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum ProjectionError {
    InvalidEnvelope(&'static str),
    InvalidFloor(&'static str),
    Bounds(&'static str),
    SessionMismatch,
    SequenceMismatch { expected: u64, actual: u64 },
    TimestampRegression,
    CorrelationMismatch,
    TurnAlreadyExists,
    TurnMissing,
    TurnNotRunning,
    MultipleRunningTurns,
    TurnStateDrift,
    ItemRevisionMismatch { expected: u64, actual: u64 },
    ItemShapeDrift,
    ItemStateTransition,
    OpenItemsAtTerminal,
}

impl ProjectionError {
    pub(crate) fn code(&self) -> &'static str {
        match self {
            Self::InvalidEnvelope(_) => "projection-envelope-invalid",
            Self::InvalidFloor(code) | Self::Bounds(code) => code,
            Self::SessionMismatch => "projection-session-mismatch",
            Self::SequenceMismatch { .. } => "projection-sequence-mismatch",
            Self::TimestampRegression => "projection-timestamp-regression",
            Self::CorrelationMismatch => "projection-correlation-mismatch",
            Self::TurnAlreadyExists => "projection-turn-already-exists",
            Self::TurnMissing => "projection-turn-missing",
            Self::TurnNotRunning => "projection-turn-not-running",
            Self::MultipleRunningTurns => "projection-multiple-running-turns",
            Self::TurnStateDrift => "projection-turn-state-drift",
            Self::ItemRevisionMismatch { .. } => "projection-item-revision-mismatch",
            Self::ItemShapeDrift => "projection-item-shape-drift",
            Self::ItemStateTransition => "projection-item-state-transition",
            Self::OpenItemsAtTerminal => "projection-open-items-at-terminal",
        }
    }
}

impl PublicTimelineProjection {
    pub(crate) fn empty(session_id: impl Into<String>) -> Result<Self, ProjectionError> {
        let session_id = session_id.into();
        if !valid_binding_identity(&session_id) {
            return Err(ProjectionError::InvalidFloor(
                "projection-floor-session-invalid",
            ));
        }
        Ok(Self {
            session_id,
            anchor: ProjectionAnchor::empty(),
            turns: BTreeMap::new(),
            turn_order: BTreeMap::new(),
            items: BTreeMap::new(),
            item_order: BTreeMap::new(),
            running_turn_id: None,
            open_items: BTreeSet::new(),
        })
    }

    pub(crate) fn from_floor(snapshot: VisibleStateFloorSnapshot) -> Result<Self, ProjectionError> {
        validate_floor_snapshot(&snapshot)?;

        let turns: BTreeMap<_, _> = snapshot
            .turns
            .into_iter()
            .map(|turn| (turn.turn_id.clone(), turn))
            .collect();
        let turn_order = turns
            .iter()
            .map(|(turn_id, turn)| (turn.started_anchor.sequence, turn_id.clone()))
            .collect();
        let items: BTreeMap<_, _> = snapshot
            .items
            .into_iter()
            .map(|visible| {
                (
                    ItemIdentity {
                        turn_id: visible.turn_id.clone(),
                        item_id: visible.item.id.clone(),
                    },
                    visible,
                )
            })
            .collect();
        let item_order = items
            .iter()
            .map(|(identity, visible)| (visible.first_anchor.sequence, identity.clone()))
            .collect();
        let open_items = snapshot
            .open_items
            .into_iter()
            .map(|open| ItemIdentity {
                turn_id: open.turn_id,
                item_id: open.item_id,
            })
            .collect();
        Ok(Self {
            session_id: snapshot.session_id,
            anchor: snapshot.anchor,
            turns,
            turn_order,
            items,
            item_order,
            running_turn_id: snapshot.running_turn_id,
            open_items,
        })
    }

    pub(crate) fn from_floor_and_tail<'a, I>(
        snapshot: VisibleStateFloorSnapshot,
        retained_tail: I,
    ) -> Result<Self, ProjectionError>
    where
        I: IntoIterator<Item = &'a EventEnvelope>,
    {
        let mut projection = Self::from_floor(snapshot)?;
        projection.apply_retained_tail(retained_tail)?;
        Ok(projection)
    }

    pub(crate) fn apply(&mut self, event: &EventEnvelope) -> Result<(), ProjectionError> {
        let mut candidate = self.clone();
        candidate.apply_inner(event)?;
        *self = candidate;
        Ok(())
    }

    pub(crate) fn apply_retained_tail<'a, I>(
        &mut self,
        retained_tail: I,
    ) -> Result<(), ProjectionError>
    where
        I: IntoIterator<Item = &'a EventEnvelope>,
    {
        let mut candidate = self.clone();
        for event in retained_tail {
            candidate.apply_inner(event)?;
        }
        *self = candidate;
        Ok(())
    }

    pub(crate) fn session_id(&self) -> &str {
        &self.session_id
    }

    pub(crate) fn anchor(&self) -> &ProjectionAnchor {
        &self.anchor
    }

    pub(crate) fn running_turn_id(&self) -> Option<&str> {
        self.running_turn_id.as_deref()
    }

    pub(crate) fn active_running_turn(&self) -> Option<&VisibleTurnState> {
        self.running_turn_id
            .as_ref()
            .and_then(|turn_id| self.turns.get(turn_id))
    }

    pub(crate) fn active_open_items(&self) -> impl Iterator<Item = &VisibleTimelineItem> {
        let running_turn_id = self.running_turn_id.as_deref();
        self.item_order
            .values()
            .filter(move |identity| {
                running_turn_id == Some(identity.turn_id.as_str())
                    && self.open_items.contains(identity)
            })
            .map(|identity| &self.items[identity])
    }

    pub(crate) fn visible_items(&self) -> impl Iterator<Item = &VisibleTimelineItem> {
        self.item_order
            .values()
            .map(|identity| &self.items[identity])
    }

    pub(crate) fn turns(&self) -> impl Iterator<Item = &VisibleTurnState> {
        self.turn_order.values().map(|turn_id| &self.turns[turn_id])
    }

    pub(crate) fn open_items(&self) -> impl Iterator<Item = OpenTimelineItem> + '_ {
        self.item_order
            .values()
            .filter(|identity| self.open_items.contains(identity))
            .map(|identity| {
                let item = &self.items[identity];
                OpenTimelineItem {
                    turn_id: identity.turn_id.clone(),
                    item_id: identity.item_id.clone(),
                    state: item.item.state.clone(),
                }
            })
    }

    pub(crate) fn floor_snapshot(&self) -> Result<VisibleStateFloorSnapshot, ProjectionError> {
        let mut snapshot = VisibleStateFloorSnapshot {
            schema_version: FLOOR_SCHEMA_VERSION.into(),
            session_id: self.session_id.clone(),
            anchor: self.anchor.clone(),
            turns: self.turns().cloned().collect(),
            items: self.visible_items().cloned().collect(),
            running_turn_id: self.running_turn_id.clone(),
            open_items: self.open_items().collect(),
            snapshot_identity: String::new(),
        };
        snapshot.snapshot_identity = floor_identity(&snapshot)?;
        validate_floor_snapshot(&snapshot)?;
        Ok(snapshot)
    }

    fn apply_inner(&mut self, event: &EventEnvelope) -> Result<(), ProjectionError> {
        if event.session_id != self.session_id {
            return Err(ProjectionError::SessionMismatch);
        }
        if event.correlation_id != event.turn_id {
            return Err(ProjectionError::CorrelationMismatch);
        }
        event.validate().map_err(ProjectionError::InvalidEnvelope)?;
        let expected_sequence = self
            .anchor
            .sequence
            .checked_add(1)
            .filter(|sequence| *sequence <= MAX_SAFE_JSON_INTEGER)
            .ok_or(ProjectionError::Bounds("projection-sequence-exhausted"))?;
        if event.sequence != expected_sequence {
            return Err(ProjectionError::SequenceMismatch {
                expected: expected_sequence,
                actual: event.sequence,
            });
        }
        if event.timestamp_ms < self.anchor.timestamp_ms {
            return Err(ProjectionError::TimestampRegression);
        }
        let event_anchor = ProjectionAnchor {
            sequence: event.sequence,
            timestamp_ms: event.timestamp_ms,
            event_id: Some(event.event_id.clone()),
        };

        let starting = event.event == "turn.started";
        if starting {
            if self.turns.contains_key(&event.turn_id) {
                return Err(ProjectionError::TurnAlreadyExists);
            }
            if self.running_turn_id.is_some()
                || self
                    .turns
                    .values()
                    .any(|turn| turn.state == TurnState::Running)
            {
                return Err(ProjectionError::MultipleRunningTurns);
            }
            if event.turn_state != TurnState::Running {
                return Err(ProjectionError::TurnStateDrift);
            }
            self.turn_order
                .insert(event.sequence, event.turn_id.clone());
            self.turns.insert(
                event.turn_id.clone(),
                VisibleTurnState {
                    turn_id: event.turn_id.clone(),
                    state: TurnState::Running,
                    started_anchor: event_anchor.clone(),
                    latest_anchor: event_anchor.clone(),
                },
            );
            self.running_turn_id = Some(event.turn_id.clone());
        } else {
            let state = self
                .turns
                .get(&event.turn_id)
                .map(|turn| turn.state)
                .ok_or(ProjectionError::TurnMissing)?;
            if state != TurnState::Running
                || self.running_turn_id.as_deref() != Some(event.turn_id.as_str())
            {
                return Err(ProjectionError::TurnNotRunning);
            }
        }

        if let (Some(item), Some(update)) = (&event.item, &event.item_update) {
            self.apply_item(&event.turn_id, item, update.revision, event_anchor.clone())?;
        }

        let terminal_state = match event.event.as_str() {
            "turn.completed" => Some(TurnState::Completed),
            "turn.failed" => Some(TurnState::Failed),
            "turn.interrupted" => Some(TurnState::Interrupted),
            _ => None,
        };
        if let Some(terminal_state) = terminal_state {
            if event.turn_state != terminal_state {
                return Err(ProjectionError::TurnStateDrift);
            }
            if matches!(
                terminal_state,
                TurnState::Completed | TurnState::Interrupted
            ) && self
                .open_items
                .iter()
                .any(|identity| identity.turn_id == event.turn_id)
            {
                return Err(ProjectionError::OpenItemsAtTerminal);
            }
            self.turns
                .get_mut(&event.turn_id)
                .expect("running turn was checked above")
                .state = terminal_state;
            self.running_turn_id = None;
        } else if event.turn_state != TurnState::Running {
            return Err(ProjectionError::TurnStateDrift);
        }

        self.turns
            .get_mut(&event.turn_id)
            .expect("event turn was installed or checked above")
            .latest_anchor = event_anchor.clone();
        self.anchor = event_anchor;
        Ok(())
    }

    fn apply_item(
        &mut self,
        turn_id: &str,
        item: &TimelineItem,
        revision: u64,
        event_anchor: ProjectionAnchor,
    ) -> Result<(), ProjectionError> {
        let identity = ItemIdentity {
            turn_id: turn_id.into(),
            item_id: item.id.clone(),
        };
        let previous = self.items.get(&identity);
        let expected_revision = match previous {
            None => 1,
            Some(visible) => visible
                .revision
                .checked_add(1)
                .filter(|revision| *revision <= MAX_SAFE_JSON_INTEGER)
                .ok_or(ProjectionError::Bounds(
                    "projection-item-revision-exhausted",
                ))?,
        };
        if revision != expected_revision {
            return Err(ProjectionError::ItemRevisionMismatch {
                expected: expected_revision,
                actual: revision,
            });
        }
        if previous
            .is_some_and(|visible| visible.item.kind != item.kind || visible.item.role != item.role)
        {
            return Err(ProjectionError::ItemShapeDrift);
        }
        let previous_state = previous.map(|visible| visible.item.state.as_str());
        if !valid_item_transition(previous_state, &item.state) {
            return Err(ProjectionError::ItemStateTransition);
        }

        if matches!(item.state.as_str(), "started" | "delta") {
            self.open_items.insert(identity.clone());
        } else {
            self.open_items.remove(&identity);
        }
        let first_anchor = previous
            .map(|visible| visible.first_anchor.clone())
            .unwrap_or_else(|| event_anchor.clone());
        if previous.is_none()
            && self
                .item_order
                .insert(first_anchor.sequence, identity.clone())
                .is_some()
        {
            return Err(ProjectionError::InvalidFloor(
                "projection-item-first-sequence-duplicate",
            ));
        }
        self.items.insert(
            identity,
            VisibleTimelineItem {
                turn_id: turn_id.into(),
                revision,
                first_anchor,
                latest_anchor: event_anchor,
                item: item.clone(),
            },
        );
        Ok(())
    }
}

fn validate_floor_snapshot(snapshot: &VisibleStateFloorSnapshot) -> Result<(), ProjectionError> {
    if snapshot.schema_version != FLOOR_SCHEMA_VERSION {
        return Err(ProjectionError::InvalidFloor(
            "projection-floor-schema-unsupported",
        ));
    }
    if !valid_binding_identity(&snapshot.session_id) {
        return Err(ProjectionError::InvalidFloor(
            "projection-floor-session-invalid",
        ));
    }
    validate_anchor(&snapshot.anchor)?;
    if snapshot.turns.len() > MAX_VISIBLE_FLOOR_TURNS {
        return Err(ProjectionError::Bounds("projection-floor-turn-limit"));
    }
    if snapshot.items.len() > MAX_VISIBLE_FLOOR_ITEMS {
        return Err(ProjectionError::Bounds("projection-floor-item-limit"));
    }
    if snapshot.open_items.len() > snapshot.items.len() {
        return Err(ProjectionError::InvalidFloor(
            "projection-floor-open-item-limit",
        ));
    }

    let mut turns = BTreeMap::new();
    let mut previous_started_sequence = 0_u64;
    let mut previous_latest: Option<&ProjectionAnchor> = None;
    let mut derived_running: Option<&str> = None;
    for turn in &snapshot.turns {
        if !valid_binding_identity(&turn.turn_id) {
            return Err(ProjectionError::InvalidFloor(
                "projection-floor-turn-order-invalid",
            ));
        }
        validate_anchor(&turn.started_anchor)?;
        validate_anchor(&turn.latest_anchor)?;
        if turn.started_anchor.sequence <= previous_started_sequence
            || turn.started_anchor.sequence > turn.latest_anchor.sequence
            || turn.latest_anchor.sequence > snapshot.anchor.sequence
            || turn.started_anchor.timestamp_ms > turn.latest_anchor.timestamp_ms
            || turn.latest_anchor.timestamp_ms > snapshot.anchor.timestamp_ms
            || previous_latest.is_some_and(|previous| {
                previous.sequence >= turn.started_anchor.sequence
                    || previous.timestamp_ms > turn.started_anchor.timestamp_ms
            })
        {
            return Err(ProjectionError::InvalidFloor(
                "projection-floor-turn-order-invalid",
            ));
        }
        if turn.state == TurnState::Running && derived_running.replace(&turn.turn_id).is_some() {
            return Err(ProjectionError::MultipleRunningTurns);
        }
        if turns.insert(turn.turn_id.as_str(), turn).is_some() {
            return Err(ProjectionError::InvalidFloor(
                "projection-floor-turn-duplicate",
            ));
        }
        previous_started_sequence = turn.started_anchor.sequence;
        previous_latest = Some(&turn.latest_anchor);
    }
    if derived_running != snapshot.running_turn_id.as_deref() {
        return Err(ProjectionError::InvalidFloor(
            "projection-floor-running-turn-mismatch",
        ));
    }
    if let Some(last_turn) = snapshot.turns.last() {
        if last_turn.latest_anchor != snapshot.anchor
            || derived_running.is_some_and(|turn_id| turn_id != last_turn.turn_id)
        {
            return Err(ProjectionError::InvalidFloor(
                "projection-floor-latest-turn-mismatch",
            ));
        }
    }

    let mut items = BTreeMap::new();
    let mut item_event_anchors = BTreeMap::new();
    let mut previous_first_sequence = 0_u64;
    for visible in &snapshot.items {
        let turn = turns.get(visible.turn_id.as_str()).copied();
        if !valid_binding_identity(&visible.turn_id)
            || turn.is_none()
            || visible.revision == 0
            || visible.revision > MAX_SAFE_JSON_INTEGER
            || visible.item.validate().is_err()
            || !valid_projected_item_state(&visible.item.state)
        {
            return Err(ProjectionError::InvalidFloor(
                "projection-floor-item-invalid",
            ));
        }
        validate_anchor(&visible.first_anchor)?;
        validate_anchor(&visible.latest_anchor)?;
        let turn = turn.expect("item turn was checked above");
        if visible.first_anchor.sequence <= previous_first_sequence
            || visible.first_anchor.sequence > visible.latest_anchor.sequence
            || visible.first_anchor.sequence <= turn.started_anchor.sequence
            || visible.latest_anchor.sequence > turn.latest_anchor.sequence
            || visible.first_anchor.timestamp_ms > visible.latest_anchor.timestamp_ms
            || visible.first_anchor.timestamp_ms < turn.started_anchor.timestamp_ms
            || visible.latest_anchor.timestamp_ms > turn.latest_anchor.timestamp_ms
            || (visible.revision == 1
                && visible.first_anchor.sequence != visible.latest_anchor.sequence)
            || (visible.revision > 1
                && visible.first_anchor.sequence >= visible.latest_anchor.sequence)
            || visible.revision
                > visible
                    .latest_anchor
                    .sequence
                    .saturating_sub(visible.first_anchor.sequence)
                    .saturating_add(1)
        {
            return Err(ProjectionError::InvalidFloor(
                "projection-floor-item-order-invalid",
            ));
        }
        let identity = ItemIdentity {
            turn_id: visible.turn_id.clone(),
            item_id: visible.item.id.clone(),
        };
        record_item_anchor(&mut item_event_anchors, &visible.first_anchor, &identity)?;
        record_item_anchor(&mut item_event_anchors, &visible.latest_anchor, &identity)?;
        if items.insert(identity, visible).is_some() {
            return Err(ProjectionError::InvalidFloor(
                "projection-floor-item-duplicate",
            ));
        }
        previous_first_sequence = visible.first_anchor.sequence;
    }

    let mut explicit_open = Vec::new();
    for open in &snapshot.open_items {
        if !matches!(open.state.as_str(), "started" | "delta") {
            return Err(ProjectionError::InvalidFloor(
                "projection-floor-open-item-order-invalid",
            ));
        }
        let identity = ItemIdentity {
            turn_id: open.turn_id.clone(),
            item_id: open.item_id.clone(),
        };
        let Some(visible) = items.get(&identity) else {
            return Err(ProjectionError::InvalidFloor(
                "projection-floor-open-item-missing",
            ));
        };
        if visible.item.state != open.state {
            return Err(ProjectionError::InvalidFloor(
                "projection-floor-open-item-state-mismatch",
            ));
        }
        explicit_open.push(identity);
    }

    let derived_open = snapshot
        .items
        .iter()
        .filter(|visible| matches!(visible.item.state.as_str(), "started" | "delta"))
        .map(|visible| ItemIdentity {
            turn_id: visible.turn_id.clone(),
            item_id: visible.item.id.clone(),
        })
        .collect::<Vec<_>>();
    if explicit_open != derived_open {
        return Err(ProjectionError::InvalidFloor(
            "projection-floor-open-items-incomplete",
        ));
    }
    if derived_open.iter().any(|identity| {
        turns
            .get(identity.turn_id.as_str())
            .is_some_and(|turn| matches!(turn.state, TurnState::Completed | TurnState::Interrupted))
    }) {
        return Err(ProjectionError::OpenItemsAtTerminal);
    }

    let empty_anchor = snapshot.anchor.sequence == 0;
    if empty_anchor
        != (snapshot.turns.is_empty()
            && snapshot.items.is_empty()
            && snapshot.running_turn_id.is_none()
            && snapshot.open_items.is_empty())
    {
        return Err(ProjectionError::InvalidFloor(
            "projection-floor-empty-state-mismatch",
        ));
    }
    let encoded = serde_json::to_vec(snapshot)
        .map_err(|_| ProjectionError::InvalidFloor("projection-floor-json-invalid"))?;
    if encoded.len() > MAX_VISIBLE_FLOOR_BYTES {
        return Err(ProjectionError::Bounds("projection-floor-byte-limit"));
    }
    if !valid_floor_identity(&snapshot.snapshot_identity)
        || floor_identity(snapshot)? != snapshot.snapshot_identity
    {
        return Err(ProjectionError::InvalidFloor(
            "projection-floor-identity-mismatch",
        ));
    }
    Ok(())
}

#[derive(Serialize)]
struct FloorIdentityMaterial<'a> {
    schema_version: &'a str,
    session_id: &'a str,
    anchor: &'a ProjectionAnchor,
    turns: &'a [VisibleTurnState],
    items: &'a [VisibleTimelineItem],
    running_turn_id: &'a Option<String>,
    open_items: &'a [OpenTimelineItem],
}

fn floor_identity(snapshot: &VisibleStateFloorSnapshot) -> Result<String, ProjectionError> {
    let encoded = serde_json::to_vec(&FloorIdentityMaterial {
        schema_version: &snapshot.schema_version,
        session_id: &snapshot.session_id,
        anchor: &snapshot.anchor,
        turns: &snapshot.turns,
        items: &snapshot.items,
        running_turn_id: &snapshot.running_turn_id,
        open_items: &snapshot.open_items,
    })
    .map_err(|_| ProjectionError::InvalidFloor("projection-floor-json-invalid"))?;
    let encoded_len = u64::try_from(encoded.len())
        .map_err(|_| ProjectionError::Bounds("projection-floor-byte-limit"))?;
    let mut digest = Sha256::new();
    digest.update(FLOOR_IDENTITY_DOMAIN);
    digest.update(encoded_len.to_be_bytes());
    digest.update(encoded);
    Ok(format!("{FLOOR_IDENTITY_PREFIX}{:x}", digest.finalize()))
}

fn valid_floor_identity(value: &str) -> bool {
    value.len() == FLOOR_IDENTITY_PREFIX.len() + 64
        && value.starts_with(FLOOR_IDENTITY_PREFIX)
        && value[FLOOR_IDENTITY_PREFIX.len()..]
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn validate_anchor(anchor: &ProjectionAnchor) -> Result<(), ProjectionError> {
    let empty = anchor.sequence == 0 && anchor.timestamp_ms == 0 && anchor.event_id.is_none();
    let populated = anchor.sequence > 0
        && anchor.sequence <= MAX_SAFE_JSON_INTEGER
        && anchor.timestamp_ms > 0
        && anchor.timestamp_ms <= MAX_SAFE_JSON_INTEGER
        && anchor.event_id.as_deref().is_some_and(valid_event_id);
    if empty || populated {
        Ok(())
    } else {
        Err(ProjectionError::InvalidFloor(
            "projection-floor-anchor-invalid",
        ))
    }
}

fn record_item_anchor(
    anchors: &mut BTreeMap<u64, (u64, String, ItemIdentity)>,
    anchor: &ProjectionAnchor,
    identity: &ItemIdentity,
) -> Result<(), ProjectionError> {
    let event_id = anchor
        .event_id
        .as_deref()
        .ok_or(ProjectionError::InvalidFloor(
            "projection-floor-item-anchor-invalid",
        ))?;
    match anchors.get(&anchor.sequence) {
        None => {
            anchors.insert(
                anchor.sequence,
                (anchor.timestamp_ms, event_id.into(), identity.clone()),
            );
        }
        Some((timestamp_ms, existing_event_id, existing_identity))
            if *timestamp_ms == anchor.timestamp_ms
                && existing_event_id == event_id
                && existing_identity == identity => {}
        Some(_) => {
            return Err(ProjectionError::InvalidFloor(
                "projection-floor-item-anchor-collision",
            ));
        }
    }
    Ok(())
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

fn valid_projected_item_state(state: &str) -> bool {
    matches!(
        state,
        "started" | "delta" | "updated" | "completed" | "truncated"
    )
}

fn valid_item_transition(previous: Option<&str>, next: &str) -> bool {
    match previous {
        None => matches!(next, "started" | "completed" | "updated" | "truncated"),
        Some("started") => matches!(next, "delta" | "completed"),
        Some("delta") => matches!(next, "delta" | "completed"),
        Some("updated") => matches!(next, "updated" | "truncated" | "completed"),
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use aegisy_aap::stable::v0_1::{timeline_event_id, ItemUpdate};
    use serde_json::json;

    fn item(id: &str, kind: &str, role: &str, state: &str, content: &str) -> TimelineItem {
        TimelineItem {
            id: id.into(),
            kind: kind.into(),
            role: role.into(),
            state: state.into(),
            content: content.into(),
            data: None,
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn event(
        session_id: &str,
        turn_id: &str,
        sequence: u64,
        timestamp_ms: u64,
        name: &str,
        turn_state: TurnState,
        item: Option<TimelineItem>,
        revision: Option<u64>,
    ) -> EventEnvelope {
        let item_update = revision.map(|revision| ItemUpdate {
            revision,
            content_mode: "snapshot-replacement".into(),
        });
        let event_id = timeline_event_id(
            "timeline-event/0.1",
            sequence,
            timestamp_ms,
            turn_id,
            session_id,
            turn_id,
            turn_state,
            name,
            &item,
            &item_update,
        )
        .unwrap();
        EventEnvelope {
            schema_version: "timeline-event/0.1".into(),
            event_id,
            sequence,
            timestamp_ms,
            correlation_id: turn_id.into(),
            session_id: session_id.into(),
            turn_id: turn_id.into(),
            turn_state,
            event: name.into(),
            item,
            item_update,
        }
    }

    fn start(sequence: u64, timestamp_ms: u64, turn_id: &str) -> EventEnvelope {
        event(
            "session",
            turn_id,
            sequence,
            timestamp_ms,
            "turn.started",
            TurnState::Running,
            None,
            None,
        )
    }

    fn error_item(id: &str) -> TimelineItem {
        item(id, "error", "system", "completed", "redacted failure")
    }

    #[test]
    fn reduces_visible_items_unknown_events_and_terminal_turn() {
        let mut projection = PublicTimelineProjection::empty("session").unwrap();
        let events = vec![
            start(1, 10, "turn"),
            event(
                "session",
                "turn",
                2,
                11,
                "item.started",
                TurnState::Running,
                Some(item("message", "message", "agent", "started", "a")),
                Some(1),
            ),
            event(
                "session",
                "turn",
                3,
                11,
                "item.delta",
                TurnState::Running,
                Some(item("message", "message", "agent", "delta", "ab")),
                Some(2),
            ),
            event(
                "session",
                "turn",
                4,
                12,
                "item.completed",
                TurnState::Running,
                Some(item("message", "message", "agent", "completed", "abc")),
                Some(3),
            ),
            event(
                "session",
                "turn",
                5,
                13,
                "usage.updated",
                TurnState::Running,
                Some(item("usage", "usage", "system", "updated", "")),
                Some(1),
            ),
            event(
                "session",
                "turn",
                6,
                14,
                "provider.keepalive",
                TurnState::Running,
                None,
                None,
            ),
            event(
                "session",
                "turn",
                7,
                15,
                "usage.truncated",
                TurnState::Running,
                Some(item("usage", "usage", "system", "truncated", "")),
                Some(2),
            ),
            event(
                "session",
                "turn",
                8,
                16,
                "turn.completed",
                TurnState::Completed,
                None,
                None,
            ),
        ];
        projection.apply_retained_tail(&events).unwrap();

        assert_eq!(projection.anchor().sequence, 8);
        assert_eq!(projection.anchor().timestamp_ms, 16);
        assert_eq!(projection.running_turn_id(), None);
        assert_eq!(
            projection.turns().collect::<Vec<_>>()[0].state,
            TurnState::Completed
        );
        assert!(projection.open_items().next().is_none());
        let visible = projection.visible_items().collect::<Vec<_>>();
        assert_eq!(visible.len(), 2);
        assert_eq!(visible[0].item.content, "abc");
        assert_eq!(visible[0].revision, 3);
        assert_eq!(visible[1].item.state, "truncated");
        assert_eq!(visible[1].revision, 2);
    }

    #[test]
    fn envelope_cursor_and_session_failures_do_not_mutate_projection() {
        let mut projection = PublicTimelineProjection::empty("session").unwrap();
        projection.apply(&start(1, 10, "turn")).unwrap();
        let baseline = projection.clone();

        let gap = start(3, 11, "other-turn");
        assert_eq!(
            projection.apply(&gap).unwrap_err(),
            ProjectionError::SequenceMismatch {
                expected: 2,
                actual: 3
            }
        );
        assert_eq!(projection, baseline);

        let foreign = event(
            "foreign",
            "turn",
            2,
            11,
            "provider.keepalive",
            TurnState::Running,
            None,
            None,
        );
        assert_eq!(
            projection.apply(&foreign),
            Err(ProjectionError::SessionMismatch)
        );
        assert_eq!(projection, baseline);

        let older = event(
            "session",
            "turn",
            2,
            9,
            "provider.keepalive",
            TurnState::Running,
            None,
            None,
        );
        assert_eq!(
            projection.apply(&older),
            Err(ProjectionError::TimestampRegression)
        );
        assert_eq!(projection, baseline);

        let mut forged = event(
            "session",
            "turn",
            2,
            11,
            "provider.keepalive",
            TurnState::Running,
            None,
            None,
        );
        forged.event_id.replace_range(13..14, "f");
        assert!(matches!(
            projection.apply(&forged),
            Err(ProjectionError::InvalidEnvelope(_))
        ));
        assert_eq!(projection, baseline);

        let mut cross_turn = event(
            "session",
            "turn",
            2,
            11,
            "provider.keepalive",
            TurnState::Running,
            None,
            None,
        );
        cross_turn.correlation_id = "other-turn".into();
        assert_eq!(
            projection.apply(&cross_turn),
            Err(ProjectionError::CorrelationMismatch)
        );
        assert_eq!(projection, baseline);
    }

    #[test]
    fn rejects_missing_terminal_and_concurrent_turns() {
        let mut projection = PublicTimelineProjection::empty("session").unwrap();
        assert_eq!(
            projection.apply(&event(
                "session",
                "missing",
                1,
                10,
                "turn.completed",
                TurnState::Completed,
                None,
                None,
            )),
            Err(ProjectionError::TurnMissing)
        );
        projection.apply(&start(1, 10, "turn-a")).unwrap();
        assert_eq!(
            projection.apply(&start(2, 11, "turn-b")),
            Err(ProjectionError::MultipleRunningTurns)
        );
        projection
            .apply(&event(
                "session",
                "turn-a",
                2,
                11,
                "turn.completed",
                TurnState::Completed,
                None,
                None,
            ))
            .unwrap();
        assert_eq!(
            projection.apply(&event(
                "session",
                "turn-a",
                3,
                12,
                "provider.keepalive",
                TurnState::Running,
                None,
                None,
            )),
            Err(ProjectionError::TurnNotRunning)
        );
    }

    #[test]
    fn rejects_item_revision_shape_and_state_drift_transactionally() {
        let mut projection = PublicTimelineProjection::empty("session").unwrap();
        projection.apply(&start(1, 10, "turn")).unwrap();
        projection
            .apply(&event(
                "session",
                "turn",
                2,
                11,
                "item.started",
                TurnState::Running,
                Some(item("item", "message", "agent", "started", "a")),
                Some(1),
            ))
            .unwrap();
        let baseline = projection.clone();

        let gap = event(
            "session",
            "turn",
            3,
            12,
            "item.delta",
            TurnState::Running,
            Some(item("item", "message", "agent", "delta", "b")),
            Some(3),
        );
        assert!(matches!(
            projection.apply(&gap),
            Err(ProjectionError::ItemRevisionMismatch { .. })
        ));
        assert_eq!(projection, baseline);

        let shape = event(
            "session",
            "turn",
            3,
            12,
            "item.delta",
            TurnState::Running,
            Some(item("item", "command", "agent", "delta", "b")),
            Some(2),
        );
        assert_eq!(
            projection.apply(&shape),
            Err(ProjectionError::ItemShapeDrift)
        );
        assert_eq!(projection, baseline);

        let invalid = event(
            "session",
            "turn",
            3,
            12,
            "usage.updated",
            TurnState::Running,
            Some(item("item", "usage", "system", "updated", "b")),
            Some(2),
        );
        assert_eq!(
            projection.apply(&invalid),
            Err(ProjectionError::ItemShapeDrift)
        );
        assert_eq!(projection, baseline);

        projection
            .apply(&event(
                "session",
                "turn",
                3,
                12,
                "item.completed",
                TurnState::Running,
                Some(item("item", "message", "agent", "completed", "done")),
                Some(2),
            ))
            .unwrap();
        let terminal_item = projection.clone();
        assert_eq!(
            projection.apply(&event(
                "session",
                "turn",
                4,
                13,
                "item.delta",
                TurnState::Running,
                Some(item("item", "message", "agent", "delta", "late")),
                Some(3),
            )),
            Err(ProjectionError::ItemStateTransition)
        );
        assert_eq!(projection, terminal_item);
    }

    #[test]
    fn completed_and_interrupted_reject_open_items_but_failed_preserves_them() {
        for (name, state) in [
            ("turn.completed", TurnState::Completed),
            ("turn.interrupted", TurnState::Interrupted),
        ] {
            let mut projection = PublicTimelineProjection::empty("session").unwrap();
            projection.apply(&start(1, 10, "turn")).unwrap();
            projection
                .apply(&event(
                    "session",
                    "turn",
                    2,
                    11,
                    "item.started",
                    TurnState::Running,
                    Some(item("partial", "message", "agent", "started", "a")),
                    Some(1),
                ))
                .unwrap();
            let baseline = projection.clone();
            assert_eq!(
                projection.apply(&event("session", "turn", 3, 12, name, state, None, None,)),
                Err(ProjectionError::OpenItemsAtTerminal)
            );
            assert_eq!(projection, baseline);
        }

        let mut projection = PublicTimelineProjection::empty("session").unwrap();
        projection.apply(&start(1, 10, "turn")).unwrap();
        projection
            .apply(&event(
                "session",
                "turn",
                2,
                11,
                "item.started",
                TurnState::Running,
                Some(item("partial", "message", "agent", "started", "a")),
                Some(1),
            ))
            .unwrap();
        projection
            .apply(&event(
                "session",
                "turn",
                3,
                12,
                "turn.failed",
                TurnState::Failed,
                Some(error_item("failure")),
                Some(1),
            ))
            .unwrap();
        assert_eq!(projection.running_turn_id(), None);
        assert_eq!(projection.open_items().count(), 1);
        assert_eq!(projection.visible_items().count(), 2);
    }

    #[test]
    fn floor_round_trip_and_retained_tail_are_deterministic() {
        let events = vec![
            start(1, 10, "turn"),
            event(
                "session",
                "turn",
                2,
                11,
                "item.started",
                TurnState::Running,
                Some(item("partial", "message", "agent", "started", "a")),
                Some(1),
            ),
            event(
                "session",
                "turn",
                3,
                12,
                "item.delta",
                TurnState::Running,
                Some(item("partial", "message", "agent", "delta", "ab")),
                Some(2),
            ),
        ];
        let mut original = PublicTimelineProjection::empty("session").unwrap();
        original.apply_retained_tail(&events).unwrap();
        let snapshot = original.floor_snapshot().unwrap();
        assert_eq!(original.session_id(), "session");
        assert!(snapshot.identity().starts_with(FLOOR_IDENTITY_PREFIX));
        assert_eq!(snapshot.items[0].first_anchor.sequence, 2);
        assert_eq!(snapshot.items[0].latest_anchor.sequence, 3);
        assert_eq!(snapshot.items[0].revision, 2);
        let canonical = snapshot.to_canonical_json().unwrap();
        assert_eq!(
            VisibleStateFloorSnapshot::from_canonical_json("session", &canonical).unwrap(),
            snapshot
        );
        VisibleStateFloorSnapshot::new(
            snapshot.session_id.clone(),
            snapshot.anchor.clone(),
            snapshot.turns.clone(),
            snapshot.items.clone(),
            snapshot.running_turn_id.clone(),
            snapshot.open_items.clone(),
        )
        .unwrap();
        let restored = PublicTimelineProjection::from_floor(snapshot.clone()).unwrap();
        assert_eq!(restored, original);
        assert_eq!(restored.floor_snapshot().unwrap(), snapshot);

        let tail = vec![
            event(
                "session",
                "turn",
                4,
                13,
                "item.completed",
                TurnState::Running,
                Some(item("partial", "message", "agent", "completed", "abc")),
                Some(3),
            ),
            event(
                "session",
                "turn",
                5,
                14,
                "turn.completed",
                TurnState::Completed,
                None,
                None,
            ),
        ];
        let restored = PublicTimelineProjection::from_floor_and_tail(snapshot, &tail).unwrap();
        assert_eq!(restored.anchor().sequence, 5);
        assert_eq!(restored.visible_items().next().unwrap().item.content, "abc");
    }

    #[test]
    fn floor_identity_rejects_content_tampering() {
        let mut projection = PublicTimelineProjection::empty("session").unwrap();
        let events = vec![
            start(1, 10, "turn"),
            event(
                "session",
                "turn",
                2,
                11,
                "item.completed",
                TurnState::Running,
                Some(item("item", "message", "agent", "completed", "original")),
                Some(1),
            ),
        ];
        projection.apply_retained_tail(&events).unwrap();
        let snapshot = projection.floor_snapshot().unwrap();
        let mut value = serde_json::to_value(snapshot).unwrap();
        value["items"][0]["item"]["content"] = json!("tampered");
        let tampered: VisibleStateFloorSnapshot = serde_json::from_value(value).unwrap();
        assert_eq!(
            PublicTimelineProjection::from_floor(tampered)
                .unwrap_err()
                .code(),
            "projection-floor-identity-mismatch"
        );
    }

    #[test]
    fn turns_and_active_materialization_use_first_event_order() {
        let mut projection = PublicTimelineProjection::empty("session").unwrap();
        projection.apply(&start(1, 10, "turn-z")).unwrap();
        projection
            .apply(&event(
                "session",
                "turn-z",
                2,
                11,
                "turn.completed",
                TurnState::Completed,
                None,
                None,
            ))
            .unwrap();
        projection.apply(&start(3, 12, "turn-a")).unwrap();
        projection
            .apply(&event(
                "session",
                "turn-a",
                4,
                13,
                "item.started",
                TurnState::Running,
                Some(item("open", "message", "agent", "started", "partial")),
                Some(1),
            ))
            .unwrap();
        projection
            .apply(&event(
                "session",
                "turn-a",
                5,
                14,
                "provider.keepalive",
                TurnState::Running,
                None,
                None,
            ))
            .unwrap();

        let turns = projection.turns().collect::<Vec<_>>();
        assert_eq!(
            turns
                .iter()
                .map(|turn| turn.turn_id.as_str())
                .collect::<Vec<_>>(),
            ["turn-z", "turn-a"]
        );
        assert_eq!(turns[0].started_anchor.sequence, 1);
        assert_eq!(turns[0].latest_anchor.sequence, 2);
        assert_eq!(turns[1].started_anchor.sequence, 3);
        assert_eq!(turns[1].latest_anchor.sequence, 5);
        assert_eq!(projection.active_running_turn().unwrap().turn_id, "turn-a");
        assert_eq!(
            projection
                .active_running_turn()
                .unwrap()
                .latest_anchor
                .sequence,
            5
        );
        assert_eq!(projection.active_open_items().count(), 1);
        let snapshot = projection.floor_snapshot().unwrap();
        assert_eq!(snapshot.turns[0].turn_id, "turn-z");
        assert_eq!(snapshot.turns[1].turn_id, "turn-a");
        assert_eq!(snapshot.turns[1].latest_anchor, snapshot.anchor);
        let mut reversed = snapshot.clone();
        reversed.turns.reverse();
        assert_eq!(
            PublicTimelineProjection::from_floor(reversed)
                .unwrap_err()
                .code(),
            "projection-floor-turn-order-invalid"
        );
    }

    #[test]
    fn floor_rejects_incomplete_open_state_and_unsorted_items() {
        let mut projection = PublicTimelineProjection::empty("session").unwrap();
        let events = vec![
            start(1, 10, "turn"),
            event(
                "session",
                "turn",
                2,
                11,
                "item.started",
                TurnState::Running,
                Some(item("z", "message", "agent", "started", "z")),
                Some(1),
            ),
            event(
                "session",
                "turn",
                3,
                12,
                "item.completed",
                TurnState::Running,
                Some(item("a", "message", "agent", "completed", "a")),
                Some(1),
            ),
        ];
        projection.apply_retained_tail(&events).unwrap();
        let presentation = projection
            .visible_items()
            .map(|visible| visible.item.id.as_str())
            .collect::<Vec<_>>();
        assert_eq!(presentation, ["z", "a"]);
        let ordered = projection.floor_snapshot().unwrap();
        assert_eq!(ordered.items[0].first_anchor.sequence, 2);
        assert_eq!(ordered.items[1].first_anchor.sequence, 3);
        let mut missing_open = projection.floor_snapshot().unwrap();
        missing_open.open_items.clear();
        assert_eq!(
            PublicTimelineProjection::from_floor(missing_open)
                .unwrap_err()
                .code(),
            "projection-floor-open-items-incomplete"
        );

        let mut unsorted = projection.floor_snapshot().unwrap();
        unsorted.items.reverse();
        assert_eq!(
            PublicTimelineProjection::from_floor(unsorted)
                .unwrap_err()
                .code(),
            "projection-floor-item-order-invalid"
        );
    }

    #[test]
    fn retained_tail_batch_failure_rolls_back_the_entire_batch() {
        let mut projection = PublicTimelineProjection::empty("session").unwrap();
        let baseline = projection.clone();
        let events = vec![start(1, 10, "turn"), start(3, 11, "other")];
        assert!(matches!(
            projection.apply_retained_tail(&events),
            Err(ProjectionError::SequenceMismatch { .. })
        ));
        assert_eq!(projection, baseline);
    }

    #[test]
    fn snapshot_json_rejects_unknown_fields_before_floor_restore() {
        let snapshot = PublicTimelineProjection::empty("session")
            .unwrap()
            .floor_snapshot()
            .unwrap();
        let mut value = serde_json::to_value(snapshot).unwrap();
        value["unknown"] = json!(true);
        assert!(serde_json::from_value::<VisibleStateFloorSnapshot>(value).is_err());
    }
}
