use aegisy_aap::stable::v0_1::{
    EventEnvelope, TimelineActivateParams, TimelineActivateResult, TimelineAnchor,
    TimelineSessionSnapshotPage, TimelineSnapshotCursor, TimelineSubscribeParams,
    TimelineSubscribeResult, TimelineSubscriptionEvent, TimelineSubscriptionRecoveryProof,
    TimelineSubscriptionSnapshotParams, TimelineSubscriptionSource, TimelineSubscriptionState,
    TimelineSubscriptionSyncParams, TimelineSyncPage, TIMELINE_ACTIVATE_RESULT_SCHEMA,
    TIMELINE_SUBSCRIBE_RESULT_SCHEMA, TIMELINE_SUBSCRIPTION_EVENT_SCHEMA,
};
use aegisy_aap::{
    MAX_SAFE_JSON_INTEGER, MAX_TIMELINE_SNAPSHOT_TOTAL_BYTES, MAX_TIMELINE_SNAPSHOT_TOTAL_ITEMS,
};
use std::collections::{HashMap, HashSet};
use std::fmt;
use std::sync::Arc;

pub const MAX_TIMELINE_SUBSCRIPTION_IDS_PER_CONNECTION: usize = MAX_TIMELINE_SNAPSHOT_TOTAL_ITEMS;
pub const MAX_PENDING_TIMELINE_EVENTS_PER_CONNECTION: usize = MAX_TIMELINE_SNAPSHOT_TOTAL_ITEMS;
pub const MAX_PENDING_TIMELINE_BYTES_PER_CONNECTION: u64 = MAX_TIMELINE_SNAPSHOT_TOTAL_BYTES;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TimelineSubscriptionRoute {
    Sync,
    Snapshot,
}

impl TimelineSubscriptionRoute {
    fn source(self) -> TimelineSubscriptionSource {
        match self {
            Self::Sync => TimelineSubscriptionSource::Sync,
            Self::Snapshot => TimelineSubscriptionSource::Snapshot,
        }
    }

    fn pending_state(self) -> TimelineSubscriptionAttemptState {
        match self {
            Self::Sync => TimelineSubscriptionAttemptState::PendingSync,
            Self::Snapshot => TimelineSubscriptionAttemptState::PendingSnapshot,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TimelineSubscriptionAttemptState {
    PendingSync,
    PendingSnapshot,
    Active,
    Failed,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TimelineSubscriptionRegistryError {
    InvalidConnectionGeneration,
    ConnectionGenerationMismatch,
    InvalidSubscribeRequest,
    InvalidFixedHead,
    FixedHeadDrift,
    SubscriptionIdAlreadyUsed,
    SubscriptionIdCapacityExceeded,
    SessionAlreadyHasAttempt,
    UnknownSubscription,
    SubscriptionAlreadyRetired,
    AttemptFailed,
    RecoveryRouteMismatch,
    RecoveryAlreadyComplete,
    InvalidRecoveryRequest,
    InvalidRecoveryPage,
    RecoveryLimitExceeded,
    InvalidRecoveryProof,
    ActivationTokenMismatch,
    InvalidActivationRequest,
    InvalidEvent,
    EventSessionMismatch,
    EventGap,
    EventDuplicateOrReverse,
    EventTimestampReverse,
    PendingEventCountExceeded,
    PendingEventBytesExceeded,
}

impl fmt::Display for TimelineSubscriptionRegistryError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::InvalidConnectionGeneration => {
                "timeline subscription connection generation is invalid"
            }
            Self::ConnectionGenerationMismatch => {
                "timeline subscription belongs to another connection generation"
            }
            Self::InvalidSubscribeRequest => "timeline subscription request is invalid",
            Self::InvalidFixedHead => "timeline subscription fixed head is invalid",
            Self::FixedHeadDrift => "timeline subscription fixed head drifted",
            Self::SubscriptionIdAlreadyUsed => {
                "timeline subscription identity was already used on this connection"
            }
            Self::SubscriptionIdCapacityExceeded => {
                "timeline subscription identity capacity was exceeded"
            }
            Self::SessionAlreadyHasAttempt => "timeline session already has a subscription attempt",
            Self::UnknownSubscription => "timeline subscription is unknown",
            Self::SubscriptionAlreadyRetired => "timeline subscription was already retired",
            Self::AttemptFailed => "timeline subscription attempt has failed",
            Self::RecoveryRouteMismatch => "timeline subscription recovery route does not match",
            Self::RecoveryAlreadyComplete => "timeline subscription recovery is already complete",
            Self::InvalidRecoveryRequest => "timeline subscription recovery request is invalid",
            Self::InvalidRecoveryPage => "timeline subscription recovery page is invalid",
            Self::RecoveryLimitExceeded => "timeline subscription recovery exceeded its bound",
            Self::InvalidRecoveryProof => "timeline subscription recovery proof is invalid",
            Self::ActivationTokenMismatch => {
                "timeline subscription activation token does not match"
            }
            Self::InvalidActivationRequest => "timeline subscription activation request is invalid",
            Self::InvalidEvent => "timeline subscription event is invalid",
            Self::EventSessionMismatch => "timeline subscription event belongs to another session",
            Self::EventGap => "timeline subscription event sequence has a gap",
            Self::EventDuplicateOrReverse => "timeline subscription event is duplicate or reversed",
            Self::EventTimestampReverse => "timeline subscription event timestamp moved backwards",
            Self::PendingEventCountExceeded => {
                "timeline subscription pending event count was exceeded"
            }
            Self::PendingEventBytesExceeded => {
                "timeline subscription pending event bytes were exceeded"
            }
        })
    }
}

impl std::error::Error for TimelineSubscriptionRegistryError {}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TimelineSubscriptionAttemptStatus {
    pub connection_generation: u64,
    pub session_id: String,
    pub subscription_id: String,
    pub route: TimelineSubscriptionRoute,
    pub state: TimelineSubscriptionAttemptState,
    pub requested_cursor: TimelineAnchor,
    pub fixed_head: TimelineAnchor,
    pub fixed_head_timestamp_ms: u64,
    pub current_cursor: TimelineAnchor,
    pub recovery_complete: bool,
    pub buffered_events: usize,
    pub buffered_bytes: u64,
    pub failure: Option<TimelineSubscriptionRegistryError>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TimelineEventDisposition {
    NoAttempt,
    IgnoredAtOrBeforeFixedHead,
    Buffered,
}

#[derive(Debug, PartialEq)]
pub enum TimelineEventPublication {
    Disposition(TimelineEventDisposition),
    Live(Box<TimelineSubscriptionEvent>),
}

#[derive(Debug, PartialEq)]
pub struct TimelineSubscriptionActivation {
    pub result: TimelineActivateResult,
    pub drain: Vec<TimelineSubscriptionEvent>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RetiredTimelineSubscription {
    pub session_id: String,
    pub subscription_id: String,
    pub final_state: TimelineSubscriptionAttemptState,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TimelineSubscriptionDisconnectReport {
    pub attempts_dropped: usize,
    pub used_ids_dropped: usize,
    pub pending_events_dropped: usize,
    pub pending_bytes_dropped: u64,
}

struct RegistrySeal;

pub struct TimelineSubscriptionActivationToken {
    seal: Arc<RegistrySeal>,
    connection_generation: u64,
    registration_ordinal: u64,
    subscription_id: String,
    proof: TimelineSubscriptionRecoveryProof,
}

impl fmt::Debug for TimelineSubscriptionActivationToken {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("TimelineSubscriptionActivationToken")
            .field("connection_generation", &self.connection_generation)
            .field("registration_ordinal", &self.registration_ordinal)
            .field("subscription_id", &"<bound>")
            .finish_non_exhaustive()
    }
}

enum RecoveryProgress {
    Sync {
        pages: Vec<TimelineSyncPage>,
        expected_after: TimelineAnchor,
        total_events: usize,
        total_bytes: u64,
    },
    Snapshot {
        pages: Vec<TimelineSessionSnapshotPage>,
        expected_after: Option<TimelineSnapshotCursor>,
        snapshot_identity: Option<String>,
        total_items: usize,
        total_bytes: u64,
    },
    Complete,
}

struct SubscriptionAttempt {
    registration_ordinal: u64,
    result: TimelineSubscribeResult,
    route: TimelineSubscriptionRoute,
    fixed_head: TimelineAnchor,
    fixed_head_timestamp_ms: u64,
    state: TimelineSubscriptionAttemptState,
    recovery: RecoveryProgress,
    activation_token_outstanding: bool,
    buffered: Vec<EventEnvelope>,
    buffered_bytes: u64,
    current_cursor: TimelineAnchor,
    last_timestamp_ms: u64,
    failure: Option<TimelineSubscriptionRegistryError>,
}

impl SubscriptionAttempt {
    fn status(&self) -> TimelineSubscriptionAttemptStatus {
        TimelineSubscriptionAttemptStatus {
            connection_generation: self.result.connection_generation,
            session_id: self.result.session_id.clone(),
            subscription_id: self.result.subscription_id.clone(),
            route: self.route,
            state: self.state,
            requested_cursor: self.result.cursor.clone(),
            fixed_head: self.fixed_head.clone(),
            fixed_head_timestamp_ms: self.fixed_head_timestamp_ms,
            current_cursor: self.current_cursor.clone(),
            recovery_complete: matches!(self.recovery, RecoveryProgress::Complete),
            buffered_events: self.buffered.len(),
            buffered_bytes: self.buffered_bytes,
            failure: self.failure,
        }
    }

    fn recovery_totals(&self) -> (usize, u64) {
        match &self.recovery {
            RecoveryProgress::Sync {
                total_events,
                total_bytes,
                ..
            } => (*total_events, *total_bytes),
            RecoveryProgress::Snapshot {
                total_items,
                total_bytes,
                ..
            } => (*total_items, *total_bytes),
            RecoveryProgress::Complete => (0, 0),
        }
    }

    fn pending_totals(&self) -> (usize, u64) {
        let (recovery_units, recovery_bytes) = self.recovery_totals();
        (
            recovery_units
                .checked_add(self.buffered.len())
                .expect("attempt pending count must remain bounded"),
            recovery_bytes
                .checked_add(self.buffered_bytes)
                .expect("attempt pending bytes must remain bounded"),
        )
    }
}

pub struct TimelineSubscriptionRegistry {
    connection_generation: u64,
    seal: Arc<RegistrySeal>,
    next_registration_ordinal: u64,
    used_ids: HashSet<String>,
    attempts: HashMap<String, SubscriptionAttempt>,
    session_attempts: HashMap<String, String>,
    pending_event_limit: usize,
    pending_byte_limit: u64,
    pending_events: usize,
    pending_bytes: u64,
}

impl fmt::Debug for TimelineSubscriptionRegistry {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("TimelineSubscriptionRegistry")
            .field("connection_generation", &self.connection_generation)
            .field("used_ids", &self.used_ids.len())
            .field("attempts", &self.attempts.len())
            .field("pending_events", &self.pending_events)
            .field("pending_bytes", &self.pending_bytes)
            .finish()
    }
}

impl TimelineSubscriptionRegistry {
    pub fn new(connection_generation: u64) -> Result<Self, TimelineSubscriptionRegistryError> {
        Self::with_pending_limits(
            connection_generation,
            MAX_PENDING_TIMELINE_EVENTS_PER_CONNECTION,
            MAX_PENDING_TIMELINE_BYTES_PER_CONNECTION,
        )
    }

    fn with_pending_limits(
        connection_generation: u64,
        pending_event_limit: usize,
        pending_byte_limit: u64,
    ) -> Result<Self, TimelineSubscriptionRegistryError> {
        if connection_generation == 0 || connection_generation > MAX_SAFE_JSON_INTEGER {
            return Err(TimelineSubscriptionRegistryError::InvalidConnectionGeneration);
        }
        Ok(Self {
            connection_generation,
            seal: Arc::new(RegistrySeal),
            next_registration_ordinal: 1,
            used_ids: HashSet::new(),
            attempts: HashMap::new(),
            session_attempts: HashMap::new(),
            pending_event_limit,
            pending_byte_limit,
            pending_events: 0,
            pending_bytes: 0,
        })
    }

    #[cfg(test)]
    fn with_test_pending_limits(
        connection_generation: u64,
        pending_event_limit: usize,
        pending_byte_limit: u64,
    ) -> Result<Self, TimelineSubscriptionRegistryError> {
        Self::with_pending_limits(
            connection_generation,
            pending_event_limit,
            pending_byte_limit,
        )
    }

    pub fn connection_generation(&self) -> u64 {
        self.connection_generation
    }

    pub fn used_id_count(&self) -> usize {
        self.used_ids.len()
    }

    pub fn attempt_count(&self) -> usize {
        self.attempts.len()
    }

    pub fn pending_totals(&self) -> (usize, u64) {
        (self.pending_events, self.pending_bytes)
    }

    fn checked_connection_pending_totals(
        &self,
        added_count: usize,
        added_bytes: usize,
    ) -> Result<(usize, u64), TimelineSubscriptionRegistryError> {
        checked_pending_totals_with_limits(
            self.pending_events,
            self.pending_bytes,
            added_count,
            added_bytes,
            self.pending_event_limit,
            self.pending_byte_limit,
        )
    }

    fn release_pending(&mut self, count: usize, bytes: u64) {
        self.pending_events = self
            .pending_events
            .checked_sub(count)
            .expect("attempt pending count must be included in registry total");
        self.pending_bytes = self
            .pending_bytes
            .checked_sub(bytes)
            .expect("attempt pending bytes must be included in registry total");
    }

    pub fn status(
        &self,
        subscription_id: &str,
    ) -> Result<TimelineSubscriptionAttemptStatus, TimelineSubscriptionRegistryError> {
        if let Some(attempt) = self.attempts.get(subscription_id) {
            return Ok(attempt.status());
        }
        if self.used_ids.contains(subscription_id) {
            Err(TimelineSubscriptionRegistryError::SubscriptionAlreadyRetired)
        } else {
            Err(TimelineSubscriptionRegistryError::UnknownSubscription)
        }
    }

    pub fn register(
        &mut self,
        request: &TimelineSubscribeParams,
        fixed_head: TimelineAnchor,
        fixed_head_timestamp_ms: u64,
        route: TimelineSubscriptionRoute,
    ) -> Result<TimelineSubscribeResult, TimelineSubscriptionRegistryError> {
        request
            .validate()
            .map_err(|_| TimelineSubscriptionRegistryError::InvalidSubscribeRequest)?;
        if request.connection_generation != self.connection_generation {
            return Err(TimelineSubscriptionRegistryError::ConnectionGenerationMismatch);
        }
        fixed_head
            .validate()
            .map_err(|_| TimelineSubscriptionRegistryError::InvalidFixedHead)?;
        validate_fixed_head_timestamp(&fixed_head, fixed_head_timestamp_ms)
            .map_err(|_| TimelineSubscriptionRegistryError::InvalidFixedHead)?;
        validate_anchor_window(&request.cursor, &fixed_head)
            .map_err(|_| TimelineSubscriptionRegistryError::FixedHeadDrift)?;
        if request
            .watermark
            .as_ref()
            .is_some_and(|watermark| watermark != &fixed_head)
        {
            return Err(TimelineSubscriptionRegistryError::FixedHeadDrift);
        }
        if route == TimelineSubscriptionRoute::Snapshot && request.watermark.is_some() {
            return Err(TimelineSubscriptionRegistryError::RecoveryRouteMismatch);
        }
        if self.used_ids.contains(&request.subscription_id) {
            return Err(TimelineSubscriptionRegistryError::SubscriptionIdAlreadyUsed);
        }
        if self.used_ids.len() == MAX_TIMELINE_SUBSCRIPTION_IDS_PER_CONNECTION {
            return Err(TimelineSubscriptionRegistryError::SubscriptionIdCapacityExceeded);
        }
        if self.session_attempts.contains_key(&request.session_id) {
            return Err(TimelineSubscriptionRegistryError::SessionAlreadyHasAttempt);
        }

        let registration_ordinal = self.next_registration_ordinal;
        self.next_registration_ordinal = self
            .next_registration_ordinal
            .checked_add(1)
            .ok_or(TimelineSubscriptionRegistryError::SubscriptionIdCapacityExceeded)?;
        let state = route.pending_state();
        let result = TimelineSubscribeResult {
            schema_version: TIMELINE_SUBSCRIBE_RESULT_SCHEMA.to_string(),
            connection_generation: self.connection_generation,
            session_id: request.session_id.clone(),
            subscription_id: request.subscription_id.clone(),
            state: match route {
                TimelineSubscriptionRoute::Sync => TimelineSubscriptionState::SyncRequired,
                TimelineSubscriptionRoute::Snapshot => TimelineSubscriptionState::SnapshotRequired,
            },
            cursor: request.cursor.clone(),
            watermark: match route {
                TimelineSubscriptionRoute::Sync => Some(fixed_head.clone()),
                TimelineSubscriptionRoute::Snapshot => None,
            },
            next_method: match route {
                TimelineSubscriptionRoute::Sync => "timeline/subscription-sync".to_string(),
                TimelineSubscriptionRoute::Snapshot => "timeline/subscription-snapshot".to_string(),
            },
        };
        result
            .validate_for_request(request)
            .map_err(|_| TimelineSubscriptionRegistryError::InvalidSubscribeRequest)?;
        let recovery = match route {
            TimelineSubscriptionRoute::Sync => RecoveryProgress::Sync {
                pages: Vec::new(),
                expected_after: request.cursor.clone(),
                total_events: 0,
                total_bytes: 0,
            },
            TimelineSubscriptionRoute::Snapshot => RecoveryProgress::Snapshot {
                pages: Vec::new(),
                expected_after: None,
                snapshot_identity: None,
                total_items: 0,
                total_bytes: 0,
            },
        };
        let attempt = SubscriptionAttempt {
            registration_ordinal,
            result: result.clone(),
            route,
            fixed_head: fixed_head.clone(),
            fixed_head_timestamp_ms,
            state,
            recovery,
            activation_token_outstanding: false,
            buffered: Vec::new(),
            buffered_bytes: 0,
            current_cursor: fixed_head,
            last_timestamp_ms: fixed_head_timestamp_ms,
            failure: None,
        };
        self.used_ids.insert(request.subscription_id.clone());
        self.session_attempts
            .insert(request.session_id.clone(), request.subscription_id.clone());
        self.attempts
            .insert(request.subscription_id.clone(), attempt);
        Ok(result)
    }

    pub fn accept_sync_page(
        &mut self,
        subscription_id: &str,
        request: &TimelineSubscriptionSyncParams,
        page: TimelineSyncPage,
    ) -> Result<Option<TimelineSubscriptionActivationToken>, TimelineSubscriptionRegistryError>
    {
        let validation = self.validate_sync_page(subscription_id, request, &page);
        if let Err(error) = validation {
            return self.fail_known_attempt(subscription_id, error);
        }
        let encoded_bytes = match serde_json::to_vec(&page) {
            Ok(encoded) => encoded.len(),
            Err(_) => {
                return self.fail_known_attempt(
                    subscription_id,
                    TimelineSubscriptionRegistryError::InvalidRecoveryPage,
                )
            }
        };
        let complete = page.complete;
        let (current_events, current_bytes) = {
            let attempt = self
                .attempts
                .get(subscription_id)
                .ok_or(TimelineSubscriptionRegistryError::UnknownSubscription)?;
            let RecoveryProgress::Sync {
                total_events,
                total_bytes,
                ..
            } = &attempt.recovery
            else {
                return self.fail_known_attempt(
                    subscription_id,
                    TimelineSubscriptionRegistryError::RecoveryRouteMismatch,
                );
            };
            (*total_events, *total_bytes)
        };
        let (next_events, next_bytes) = match checked_recovery_totals(
            current_events,
            current_bytes,
            page.events.len(),
            encoded_bytes,
        ) {
            Ok(totals) => totals,
            Err(_) => {
                return self.fail_known_attempt(
                    subscription_id,
                    TimelineSubscriptionRegistryError::RecoveryLimitExceeded,
                )
            }
        };
        let (next_pending_events, next_pending_bytes) =
            match self.checked_connection_pending_totals(page.events.len(), encoded_bytes) {
                Ok(totals) => totals,
                Err(_) => {
                    return self.fail_known_attempt(
                        subscription_id,
                        TimelineSubscriptionRegistryError::RecoveryLimitExceeded,
                    )
                }
            };
        {
            let attempt = self
                .attempts
                .get_mut(subscription_id)
                .ok_or(TimelineSubscriptionRegistryError::UnknownSubscription)?;
            let RecoveryProgress::Sync {
                pages,
                expected_after,
                total_events,
                total_bytes,
            } = &mut attempt.recovery
            else {
                return self.fail_known_attempt(
                    subscription_id,
                    TimelineSubscriptionRegistryError::RecoveryRouteMismatch,
                );
            };
            *total_events = next_events;
            *total_bytes = next_bytes;
            if !complete {
                *expected_after = page
                    .next_after
                    .clone()
                    .expect("validated incomplete sync page must have a continuation anchor");
            }
            pages.push(page);
        }
        self.pending_events = next_pending_events;
        self.pending_bytes = next_pending_bytes;
        if !complete {
            return Ok(None);
        }
        self.finish_sync_recovery(subscription_id).map(Some)
    }

    pub fn accept_snapshot_page(
        &mut self,
        subscription_id: &str,
        request: &TimelineSubscriptionSnapshotParams,
        page: TimelineSessionSnapshotPage,
    ) -> Result<Option<TimelineSubscriptionActivationToken>, TimelineSubscriptionRegistryError>
    {
        let validation = self.validate_snapshot_page(subscription_id, request, &page);
        if let Err(error) = validation {
            return self.fail_known_attempt(subscription_id, error);
        }
        let encoded_bytes = match serde_json::to_vec(&page) {
            Ok(encoded) => encoded.len(),
            Err(_) => {
                return self.fail_known_attempt(
                    subscription_id,
                    TimelineSubscriptionRegistryError::InvalidRecoveryPage,
                )
            }
        };
        let complete = page.complete;
        let (current_items, current_bytes) = {
            let attempt = self
                .attempts
                .get(subscription_id)
                .ok_or(TimelineSubscriptionRegistryError::UnknownSubscription)?;
            let RecoveryProgress::Snapshot {
                total_items,
                total_bytes,
                ..
            } = &attempt.recovery
            else {
                return self.fail_known_attempt(
                    subscription_id,
                    TimelineSubscriptionRegistryError::RecoveryRouteMismatch,
                );
            };
            (*total_items, *total_bytes)
        };
        let (next_items, next_bytes) = match checked_recovery_totals(
            current_items,
            current_bytes,
            page.items.len(),
            encoded_bytes,
        ) {
            Ok(totals) => totals,
            Err(_) => {
                return self.fail_known_attempt(
                    subscription_id,
                    TimelineSubscriptionRegistryError::RecoveryLimitExceeded,
                )
            }
        };
        let (next_pending_events, next_pending_bytes) =
            match self.checked_connection_pending_totals(page.items.len(), encoded_bytes) {
                Ok(totals) => totals,
                Err(_) => {
                    return self.fail_known_attempt(
                        subscription_id,
                        TimelineSubscriptionRegistryError::RecoveryLimitExceeded,
                    )
                }
            };
        {
            let attempt = self
                .attempts
                .get_mut(subscription_id)
                .ok_or(TimelineSubscriptionRegistryError::UnknownSubscription)?;
            let RecoveryProgress::Snapshot {
                pages,
                expected_after,
                snapshot_identity,
                total_items,
                total_bytes,
            } = &mut attempt.recovery
            else {
                return self.fail_known_attempt(
                    subscription_id,
                    TimelineSubscriptionRegistryError::RecoveryRouteMismatch,
                );
            };
            *total_items = next_items;
            *total_bytes = next_bytes;
            *snapshot_identity = Some(page.snapshot_identity.clone());
            if !complete {
                *expected_after = page.next_after.clone();
            }
            pages.push(page);
        }
        self.pending_events = next_pending_events;
        self.pending_bytes = next_pending_bytes;
        if !complete {
            return Ok(None);
        }
        self.finish_snapshot_recovery(subscription_id).map(Some)
    }

    pub fn publish_event(
        &mut self,
        event: EventEnvelope,
    ) -> Result<TimelineEventPublication, TimelineSubscriptionRegistryError> {
        if event.validate().is_err() {
            if let Some(subscription_id) = self.session_attempts.get(&event.session_id).cloned() {
                return self.fail_known_attempt(
                    &subscription_id,
                    TimelineSubscriptionRegistryError::InvalidEvent,
                );
            }
            return Err(TimelineSubscriptionRegistryError::InvalidEvent);
        }
        let Some(subscription_id) = self.session_attempts.get(&event.session_id).cloned() else {
            return Ok(TimelineEventPublication::Disposition(
                TimelineEventDisposition::NoAttempt,
            ));
        };
        let encoded_bytes = match serde_json::to_vec(&event) {
            Ok(encoded) => encoded.len(),
            Err(_) => {
                return self.fail_known_attempt(
                    &subscription_id,
                    TimelineSubscriptionRegistryError::InvalidEvent,
                )
            }
        };
        let (state, fixed_head, current_cursor, last_timestamp_ms) = {
            let attempt = self
                .attempts
                .get(&subscription_id)
                .ok_or(TimelineSubscriptionRegistryError::UnknownSubscription)?;
            (
                attempt.state,
                attempt.fixed_head.clone(),
                attempt.current_cursor.clone(),
                attempt.last_timestamp_ms,
            )
        };
        if state == TimelineSubscriptionAttemptState::Failed {
            return Err(TimelineSubscriptionRegistryError::AttemptFailed);
        }
        if event.session_id
            != self
                .attempts
                .get(&subscription_id)
                .expect("session index must reference an attempt")
                .result
                .session_id
        {
            return self.fail_known_attempt(
                &subscription_id,
                TimelineSubscriptionRegistryError::EventSessionMismatch,
            );
        }
        if event.sequence <= fixed_head.sequence {
            if event.sequence == fixed_head.sequence
                && fixed_head.event_id.as_deref() != Some(event.event_id.as_str())
            {
                return self.fail_known_attempt(
                    &subscription_id,
                    TimelineSubscriptionRegistryError::FixedHeadDrift,
                );
            }
            return Ok(TimelineEventPublication::Disposition(
                TimelineEventDisposition::IgnoredAtOrBeforeFixedHead,
            ));
        }
        let expected_sequence = current_cursor
            .sequence
            .checked_add(1)
            .ok_or(TimelineSubscriptionRegistryError::EventGap)?;
        if event.sequence < expected_sequence {
            return self.fail_known_attempt(
                &subscription_id,
                TimelineSubscriptionRegistryError::EventDuplicateOrReverse,
            );
        }
        if event.sequence > expected_sequence {
            return self.fail_known_attempt(
                &subscription_id,
                TimelineSubscriptionRegistryError::EventGap,
            );
        }
        if event.timestamp_ms < last_timestamp_ms {
            return self.fail_known_attempt(
                &subscription_id,
                TimelineSubscriptionRegistryError::EventTimestampReverse,
            );
        }

        if state == TimelineSubscriptionAttemptState::Active {
            return match self.live_event(&subscription_id, event) {
                Ok(live) => Ok(TimelineEventPublication::Live(Box::new(live))),
                Err(error) => self.fail_known_attempt(&subscription_id, error),
            };
        }
        let (next_count, next_bytes) =
            match self.checked_connection_pending_totals(1, encoded_bytes) {
                Ok(totals) => totals,
                Err(error) => return self.fail_known_attempt(&subscription_id, error),
            };
        let attempt = self
            .attempts
            .get_mut(&subscription_id)
            .ok_or(TimelineSubscriptionRegistryError::UnknownSubscription)?;
        attempt.current_cursor = TimelineAnchor {
            sequence: event.sequence,
            event_id: Some(event.event_id.clone()),
        };
        attempt.last_timestamp_ms = event.timestamp_ms;
        attempt.buffered_bytes = attempt
            .buffered_bytes
            .checked_add(
                u64::try_from(encoded_bytes)
                    .map_err(|_| TimelineSubscriptionRegistryError::PendingEventBytesExceeded)?,
            )
            .ok_or(TimelineSubscriptionRegistryError::PendingEventBytesExceeded)?;
        attempt.buffered.push(event);
        self.pending_events = next_count;
        self.pending_bytes = next_bytes;
        Ok(TimelineEventPublication::Disposition(
            TimelineEventDisposition::Buffered,
        ))
    }

    pub fn activate(
        &mut self,
        token: TimelineSubscriptionActivationToken,
        request: &TimelineActivateParams,
    ) -> Result<TimelineSubscriptionActivation, TimelineSubscriptionRegistryError> {
        let subscription_id = token.subscription_id.clone();
        let validation = self.validate_activation(&token, request);
        if let Err(error) = validation {
            return self.fail_known_attempt(&subscription_id, error);
        }
        let result = TimelineActivateResult {
            schema_version: TIMELINE_ACTIVATE_RESULT_SCHEMA.to_string(),
            connection_generation: self.connection_generation,
            session_id: request.session_id.clone(),
            subscription_id: request.subscription_id.clone(),
            state: TimelineSubscriptionState::Active,
            cursor: request.cursor.clone(),
            watermark: request.watermark.clone(),
        };
        if result.validate_for_recovery(request, &token.proof).is_err() {
            return self.fail_known_attempt(
                &subscription_id,
                TimelineSubscriptionRegistryError::InvalidRecoveryProof,
            );
        }

        let (buffered_count, buffered_bytes, buffered) = {
            let attempt = self
                .attempts
                .get_mut(&subscription_id)
                .ok_or(TimelineSubscriptionRegistryError::UnknownSubscription)?;
            let buffered_count = attempt.buffered.len();
            let buffered_bytes = attempt.buffered_bytes;
            attempt.state = TimelineSubscriptionAttemptState::Active;
            attempt.activation_token_outstanding = false;
            attempt.buffered_bytes = 0;
            (
                buffered_count,
                buffered_bytes,
                std::mem::take(&mut attempt.buffered),
            )
        };
        self.release_pending(buffered_count, buffered_bytes);

        let mut cursor = request.watermark.clone();
        let mut drain = Vec::with_capacity(buffered.len());
        for event in buffered {
            let live = TimelineSubscriptionEvent {
                schema_version: TIMELINE_SUBSCRIPTION_EVENT_SCHEMA.to_string(),
                connection_generation: self.connection_generation,
                session_id: request.session_id.clone(),
                subscription_id: request.subscription_id.clone(),
                state: TimelineSubscriptionState::Active,
                cursor: cursor.clone(),
                watermark: request.watermark.clone(),
                event,
            };
            cursor = live.next_cursor();
            drain.push(live);
        }
        debug_assert!(drain.iter().all(|event| event.validate().is_ok()));
        Ok(TimelineSubscriptionActivation { result, drain })
    }

    pub fn fail(&mut self, subscription_id: &str) -> Result<(), TimelineSubscriptionRegistryError> {
        if !self.attempts.contains_key(subscription_id) {
            return if self.used_ids.contains(subscription_id) {
                Err(TimelineSubscriptionRegistryError::SubscriptionAlreadyRetired)
            } else {
                Err(TimelineSubscriptionRegistryError::UnknownSubscription)
            };
        }
        self.mark_failed(
            subscription_id,
            TimelineSubscriptionRegistryError::AttemptFailed,
        );
        Ok(())
    }

    pub fn retire(
        &mut self,
        subscription_id: &str,
    ) -> Result<RetiredTimelineSubscription, TimelineSubscriptionRegistryError> {
        let Some(attempt) = self.attempts.remove(subscription_id) else {
            return if self.used_ids.contains(subscription_id) {
                Err(TimelineSubscriptionRegistryError::SubscriptionAlreadyRetired)
            } else {
                Err(TimelineSubscriptionRegistryError::UnknownSubscription)
            };
        };
        let (pending_count, pending_bytes) = attempt.pending_totals();
        self.release_pending(pending_count, pending_bytes);
        self.session_attempts.remove(&attempt.result.session_id);
        Ok(RetiredTimelineSubscription {
            session_id: attempt.result.session_id,
            subscription_id: attempt.result.subscription_id,
            final_state: attempt.state,
        })
    }

    pub fn disconnect(self) -> TimelineSubscriptionDisconnectReport {
        TimelineSubscriptionDisconnectReport {
            attempts_dropped: self.attempts.len(),
            used_ids_dropped: self.used_ids.len(),
            pending_events_dropped: self.pending_events,
            pending_bytes_dropped: self.pending_bytes,
        }
    }

    fn validate_sync_page(
        &self,
        subscription_id: &str,
        request: &TimelineSubscriptionSyncParams,
        page: &TimelineSyncPage,
    ) -> Result<(), TimelineSubscriptionRegistryError> {
        let attempt = self.lookup_pending(subscription_id)?;
        if attempt.route != TimelineSubscriptionRoute::Sync {
            return Err(TimelineSubscriptionRegistryError::RecoveryRouteMismatch);
        }
        let RecoveryProgress::Sync { expected_after, .. } = &attempt.recovery else {
            return Err(TimelineSubscriptionRegistryError::RecoveryAlreadyComplete);
        };
        request
            .validate_for_subscription(&attempt.result)
            .map_err(|_| TimelineSubscriptionRegistryError::InvalidRecoveryRequest)?;
        if request.connection_generation != self.connection_generation
            || request.session_id != attempt.result.session_id
            || request.subscription_id != attempt.result.subscription_id
            || request.request.after != *expected_after
            || request.request.watermark.as_ref() != Some(&attempt.fixed_head)
        {
            return Err(TimelineSubscriptionRegistryError::InvalidRecoveryRequest);
        }
        page.validate_for_request(&request.request)
            .map_err(|_| TimelineSubscriptionRegistryError::InvalidRecoveryPage)?;
        if page.watermark != attempt.fixed_head {
            return Err(TimelineSubscriptionRegistryError::FixedHeadDrift);
        }
        Ok(())
    }

    fn validate_snapshot_page(
        &self,
        subscription_id: &str,
        request: &TimelineSubscriptionSnapshotParams,
        page: &TimelineSessionSnapshotPage,
    ) -> Result<(), TimelineSubscriptionRegistryError> {
        let attempt = self.lookup_pending(subscription_id)?;
        if attempt.route != TimelineSubscriptionRoute::Snapshot {
            return Err(TimelineSubscriptionRegistryError::RecoveryRouteMismatch);
        }
        let RecoveryProgress::Snapshot {
            pages,
            expected_after,
            snapshot_identity,
            ..
        } = &attempt.recovery
        else {
            return Err(TimelineSubscriptionRegistryError::RecoveryAlreadyComplete);
        };
        request
            .validate_for_subscription(&attempt.result)
            .map_err(|_| TimelineSubscriptionRegistryError::InvalidRecoveryRequest)?;
        let request_matches = if pages.is_empty() {
            request.connection_generation == self.connection_generation
                && request.session_id == attempt.result.session_id
                && request.subscription_id == attempt.result.subscription_id
                && request.request.snapshot_identity.is_none()
                && request.request.watermark.is_none()
                && request.request.after.is_none()
        } else {
            request.connection_generation == self.connection_generation
                && request.session_id == attempt.result.session_id
                && request.subscription_id == attempt.result.subscription_id
                && request.request.snapshot_identity.as_ref() == snapshot_identity.as_ref()
                && request.request.watermark.as_ref() == Some(&attempt.fixed_head)
                && request.request.after == *expected_after
        };
        if !request_matches {
            return Err(TimelineSubscriptionRegistryError::InvalidRecoveryRequest);
        }
        page.validate_for_request(&request.request)
            .map_err(|_| TimelineSubscriptionRegistryError::InvalidRecoveryPage)?;
        if page.watermark != attempt.fixed_head {
            return Err(TimelineSubscriptionRegistryError::FixedHeadDrift);
        }
        Ok(())
    }

    fn finish_sync_recovery(
        &mut self,
        subscription_id: &str,
    ) -> Result<TimelineSubscriptionActivationToken, TimelineSubscriptionRegistryError> {
        let (result, pages, registration_ordinal, recovery_events, recovery_bytes) = {
            let attempt = self
                .attempts
                .get_mut(subscription_id)
                .ok_or(TimelineSubscriptionRegistryError::UnknownSubscription)?;
            let RecoveryProgress::Sync {
                pages,
                total_events,
                total_bytes,
                ..
            } = &mut attempt.recovery
            else {
                return self.fail_known_attempt(
                    subscription_id,
                    TimelineSubscriptionRegistryError::RecoveryRouteMismatch,
                );
            };
            (
                attempt.result.clone(),
                std::mem::take(pages),
                attempt.registration_ordinal,
                *total_events,
                *total_bytes,
            )
        };
        let proof = match TimelineSubscriptionRecoveryProof::from_sync_pages(&result, &pages) {
            Ok(proof) => proof,
            Err(_) => {
                return self.fail_known_attempt(
                    subscription_id,
                    TimelineSubscriptionRegistryError::InvalidRecoveryProof,
                )
            }
        };
        self.release_pending(recovery_events, recovery_bytes);
        let attempt = self
            .attempts
            .get_mut(subscription_id)
            .ok_or(TimelineSubscriptionRegistryError::UnknownSubscription)?;
        attempt.recovery = RecoveryProgress::Complete;
        attempt.activation_token_outstanding = true;
        Ok(TimelineSubscriptionActivationToken {
            seal: Arc::clone(&self.seal),
            connection_generation: self.connection_generation,
            registration_ordinal,
            subscription_id: subscription_id.to_string(),
            proof,
        })
    }

    fn finish_snapshot_recovery(
        &mut self,
        subscription_id: &str,
    ) -> Result<TimelineSubscriptionActivationToken, TimelineSubscriptionRegistryError> {
        let (result, pages, registration_ordinal, recovery_items, recovery_bytes) = {
            let attempt = self
                .attempts
                .get_mut(subscription_id)
                .ok_or(TimelineSubscriptionRegistryError::UnknownSubscription)?;
            let RecoveryProgress::Snapshot {
                pages,
                total_items,
                total_bytes,
                ..
            } = &mut attempt.recovery
            else {
                return self.fail_known_attempt(
                    subscription_id,
                    TimelineSubscriptionRegistryError::RecoveryRouteMismatch,
                );
            };
            (
                attempt.result.clone(),
                std::mem::take(pages),
                attempt.registration_ordinal,
                *total_items,
                *total_bytes,
            )
        };
        let proof = match TimelineSubscriptionRecoveryProof::from_snapshot_pages(&result, &pages) {
            Ok(proof) => proof,
            Err(_) => {
                return self.fail_known_attempt(
                    subscription_id,
                    TimelineSubscriptionRegistryError::InvalidRecoveryProof,
                )
            }
        };
        self.release_pending(recovery_items, recovery_bytes);
        let attempt = self
            .attempts
            .get_mut(subscription_id)
            .ok_or(TimelineSubscriptionRegistryError::UnknownSubscription)?;
        attempt.recovery = RecoveryProgress::Complete;
        attempt.activation_token_outstanding = true;
        Ok(TimelineSubscriptionActivationToken {
            seal: Arc::clone(&self.seal),
            connection_generation: self.connection_generation,
            registration_ordinal,
            subscription_id: subscription_id.to_string(),
            proof,
        })
    }

    fn validate_activation(
        &self,
        token: &TimelineSubscriptionActivationToken,
        request: &TimelineActivateParams,
    ) -> Result<(), TimelineSubscriptionRegistryError> {
        let attempt = self.lookup_pending(&token.subscription_id)?;
        if !Arc::ptr_eq(&self.seal, &token.seal)
            || token.connection_generation != self.connection_generation
            || token.registration_ordinal != attempt.registration_ordinal
            || !attempt.activation_token_outstanding
            || request.connection_generation != self.connection_generation
            || request.session_id != attempt.result.session_id
            || request.subscription_id != attempt.result.subscription_id
            || request.source != attempt.route.source()
            || request.cursor != attempt.fixed_head
            || request.watermark != attempt.fixed_head
        {
            return Err(TimelineSubscriptionRegistryError::ActivationTokenMismatch);
        }
        if !matches!(attempt.recovery, RecoveryProgress::Complete) {
            return Err(TimelineSubscriptionRegistryError::InvalidRecoveryProof);
        }
        request
            .validate_for_recovery(&token.proof)
            .map_err(|_| TimelineSubscriptionRegistryError::InvalidActivationRequest)
    }

    fn live_event(
        &mut self,
        subscription_id: &str,
        event: EventEnvelope,
    ) -> Result<TimelineSubscriptionEvent, TimelineSubscriptionRegistryError> {
        let attempt = self
            .attempts
            .get_mut(subscription_id)
            .ok_or(TimelineSubscriptionRegistryError::UnknownSubscription)?;
        let live = TimelineSubscriptionEvent {
            schema_version: TIMELINE_SUBSCRIPTION_EVENT_SCHEMA.to_string(),
            connection_generation: self.connection_generation,
            session_id: attempt.result.session_id.clone(),
            subscription_id: attempt.result.subscription_id.clone(),
            state: TimelineSubscriptionState::Active,
            cursor: attempt.current_cursor.clone(),
            watermark: attempt.fixed_head.clone(),
            event,
        };
        live.validate()
            .map_err(|_| TimelineSubscriptionRegistryError::InvalidEvent)?;
        attempt.current_cursor = live.next_cursor();
        attempt.last_timestamp_ms = live.event.timestamp_ms;
        Ok(live)
    }

    fn lookup_pending(
        &self,
        subscription_id: &str,
    ) -> Result<&SubscriptionAttempt, TimelineSubscriptionRegistryError> {
        let attempt = self.attempts.get(subscription_id).ok_or_else(|| {
            if self.used_ids.contains(subscription_id) {
                TimelineSubscriptionRegistryError::SubscriptionAlreadyRetired
            } else {
                TimelineSubscriptionRegistryError::UnknownSubscription
            }
        })?;
        match attempt.state {
            TimelineSubscriptionAttemptState::PendingSync
            | TimelineSubscriptionAttemptState::PendingSnapshot => Ok(attempt),
            TimelineSubscriptionAttemptState::Failed => {
                Err(TimelineSubscriptionRegistryError::AttemptFailed)
            }
            TimelineSubscriptionAttemptState::Active => {
                Err(TimelineSubscriptionRegistryError::RecoveryAlreadyComplete)
            }
        }
    }

    fn fail_known_attempt<T>(
        &mut self,
        subscription_id: &str,
        error: TimelineSubscriptionRegistryError,
    ) -> Result<T, TimelineSubscriptionRegistryError> {
        if self.attempts.contains_key(subscription_id) {
            self.mark_failed(subscription_id, error);
        }
        Err(error)
    }

    fn mark_failed(&mut self, subscription_id: &str, error: TimelineSubscriptionRegistryError) {
        let Some(attempt) = self.attempts.get(subscription_id) else {
            return;
        };
        if attempt.state == TimelineSubscriptionAttemptState::Failed {
            return;
        }
        let (pending_count, pending_bytes) = attempt.pending_totals();
        self.release_pending(pending_count, pending_bytes);
        let attempt = self
            .attempts
            .get_mut(subscription_id)
            .expect("known attempt must remain registered while failing");
        attempt.state = TimelineSubscriptionAttemptState::Failed;
        attempt.recovery = RecoveryProgress::Complete;
        attempt.activation_token_outstanding = false;
        attempt.buffered.clear();
        attempt.buffered_bytes = 0;
        attempt.failure = Some(error);
    }
}

fn validate_anchor_window(cursor: &TimelineAnchor, fixed_head: &TimelineAnchor) -> Result<(), ()> {
    cursor.validate().map_err(|_| ())?;
    fixed_head.validate().map_err(|_| ())?;
    if cursor.sequence > fixed_head.sequence
        || (cursor.sequence == fixed_head.sequence && cursor != fixed_head)
    {
        return Err(());
    }
    Ok(())
}

fn validate_fixed_head_timestamp(
    fixed_head: &TimelineAnchor,
    fixed_head_timestamp_ms: u64,
) -> Result<(), ()> {
    if (fixed_head.sequence == 0 && fixed_head_timestamp_ms != 0)
        || (fixed_head.sequence > 0
            && (fixed_head_timestamp_ms == 0 || fixed_head_timestamp_ms > MAX_SAFE_JSON_INTEGER))
    {
        return Err(());
    }
    Ok(())
}

fn checked_pending_totals(
    current_count: usize,
    current_bytes: u64,
    added_count: usize,
    added_bytes: usize,
) -> Result<(usize, u64), TimelineSubscriptionRegistryError> {
    checked_pending_totals_with_limits(
        current_count,
        current_bytes,
        added_count,
        added_bytes,
        MAX_PENDING_TIMELINE_EVENTS_PER_CONNECTION,
        MAX_PENDING_TIMELINE_BYTES_PER_CONNECTION,
    )
}

fn checked_pending_totals_with_limits(
    current_count: usize,
    current_bytes: u64,
    added_count: usize,
    added_bytes: usize,
    event_limit: usize,
    byte_limit: u64,
) -> Result<(usize, u64), TimelineSubscriptionRegistryError> {
    let next_count = current_count
        .checked_add(added_count)
        .ok_or(TimelineSubscriptionRegistryError::PendingEventCountExceeded)?;
    if next_count > event_limit {
        return Err(TimelineSubscriptionRegistryError::PendingEventCountExceeded);
    }
    let added_bytes = u64::try_from(added_bytes)
        .map_err(|_| TimelineSubscriptionRegistryError::PendingEventBytesExceeded)?;
    let next_bytes = current_bytes
        .checked_add(added_bytes)
        .ok_or(TimelineSubscriptionRegistryError::PendingEventBytesExceeded)?;
    if next_bytes > byte_limit {
        return Err(TimelineSubscriptionRegistryError::PendingEventBytesExceeded);
    }
    Ok((next_count, next_bytes))
}

fn checked_recovery_totals(
    current_count: usize,
    current_bytes: u64,
    added_count: usize,
    added_bytes: usize,
) -> Result<(usize, u64), TimelineSubscriptionRegistryError> {
    checked_pending_totals(current_count, current_bytes, added_count, added_bytes)
}

#[cfg(test)]
mod tests {
    use super::*;
    use aegisy_aap::stable::v0_1::{
        timeline_event_id, timeline_snapshot_identity, timeline_snapshot_page_identity,
        TimelineSnapshotParams, TimelineSubscriptionSource, TimelineSyncParams, TurnState,
        TIMELINE_ACTIVATE_REQUEST_SCHEMA, TIMELINE_SUBSCRIBE_REQUEST_SCHEMA,
        TIMELINE_SUBSCRIPTION_SNAPSHOT_REQUEST_SCHEMA, TIMELINE_SUBSCRIPTION_SYNC_REQUEST_SCHEMA,
    };

    const GENERATION: u64 = 7;

    fn event(session_id: &str, sequence: u64, timestamp_ms: u64) -> EventEnvelope {
        let mut event = EventEnvelope {
            schema_version: "timeline-event/0.1".to_string(),
            event_id: String::new(),
            sequence,
            timestamp_ms,
            correlation_id: "turn-1".to_string(),
            session_id: session_id.to_string(),
            turn_id: "turn-1".to_string(),
            turn_state: TurnState::Running,
            event: if sequence == 1 {
                "turn.started".to_string()
            } else {
                "turn.metadata".to_string()
            },
            item: None,
            item_update: None,
        };
        event.event_id = timeline_event_id(
            &event.schema_version,
            event.sequence,
            event.timestamp_ms,
            &event.correlation_id,
            &event.session_id,
            &event.turn_id,
            event.turn_state,
            &event.event,
            &event.item,
            &event.item_update,
        )
        .unwrap();
        event.validate().unwrap();
        event
    }

    fn anchor(event: &EventEnvelope) -> TimelineAnchor {
        TimelineAnchor {
            sequence: event.sequence,
            event_id: Some(event.event_id.clone()),
        }
    }

    fn subscribe(
        session_id: &str,
        subscription_id: &str,
        cursor: TimelineAnchor,
    ) -> TimelineSubscribeParams {
        TimelineSubscribeParams {
            schema_version: TIMELINE_SUBSCRIBE_REQUEST_SCHEMA.to_string(),
            connection_generation: GENERATION,
            session_id: session_id.to_string(),
            subscription_id: subscription_id.to_string(),
            cursor,
            watermark: None,
        }
    }

    fn register_sync(
        registry: &mut TimelineSubscriptionRegistry,
        session_id: &str,
        subscription_id: &str,
        head_event: &EventEnvelope,
    ) -> TimelineSubscribeResult {
        registry
            .register(
                &subscribe(session_id, subscription_id, TimelineAnchor::initial()),
                anchor(head_event),
                head_event.timestamp_ms,
                TimelineSubscriptionRoute::Sync,
            )
            .unwrap()
    }

    fn finish_sync(
        registry: &mut TimelineSubscriptionRegistry,
        result: &TimelineSubscribeResult,
        events: Vec<EventEnvelope>,
    ) -> TimelineSubscriptionActivationToken {
        let request = sync_request(result, result.cursor.clone());
        let page = TimelineSyncPage {
            schema_version: "timeline-sync-page/0.1".to_string(),
            session_id: result.session_id.clone(),
            after: request.request.after.clone(),
            watermark: request.request.watermark.clone().unwrap(),
            events,
            next_after: None,
            complete: true,
        };
        registry
            .accept_sync_page(&result.subscription_id, &request, page)
            .unwrap()
            .unwrap()
    }

    fn sync_request(
        result: &TimelineSubscribeResult,
        after: TimelineAnchor,
    ) -> TimelineSubscriptionSyncParams {
        TimelineSubscriptionSyncParams {
            schema_version: TIMELINE_SUBSCRIPTION_SYNC_REQUEST_SCHEMA.to_string(),
            connection_generation: result.connection_generation,
            session_id: result.session_id.clone(),
            subscription_id: result.subscription_id.clone(),
            request: TimelineSyncParams {
                session_id: result.session_id.clone(),
                after,
                watermark: result.watermark.clone(),
                limit: 200,
            },
        }
    }

    fn sync_page(
        request: &TimelineSubscriptionSyncParams,
        events: Vec<EventEnvelope>,
        complete: bool,
    ) -> TimelineSyncPage {
        let next_after = if complete {
            None
        } else {
            Some(anchor(
                events
                    .last()
                    .expect("an incomplete sync page must advance its cursor"),
            ))
        };
        let page = TimelineSyncPage {
            schema_version: "timeline-sync-page/0.1".to_string(),
            session_id: request.session_id.clone(),
            after: request.request.after.clone(),
            watermark: request.request.watermark.clone().unwrap(),
            events,
            next_after,
            complete,
        };
        page.validate_for_request(&request.request).unwrap();
        page
    }

    fn activate_request(
        result: &TimelineSubscribeResult,
        source: TimelineSubscriptionSource,
        watermark: TimelineAnchor,
        snapshot_identity: Option<String>,
    ) -> TimelineActivateParams {
        TimelineActivateParams {
            schema_version: TIMELINE_ACTIVATE_REQUEST_SCHEMA.to_string(),
            connection_generation: result.connection_generation,
            session_id: result.session_id.clone(),
            subscription_id: result.subscription_id.clone(),
            source,
            cursor: watermark.clone(),
            watermark,
            snapshot_identity,
        }
    }

    fn empty_snapshot_page(session_id: &str) -> TimelineSessionSnapshotPage {
        let floor = TimelineAnchor::initial();
        let watermark = TimelineAnchor::initial();
        let snapshot_identity =
            timeline_snapshot_identity(session_id, &floor, &watermark, None, 0, 0, &[]).unwrap();
        let mut page = TimelineSessionSnapshotPage {
            schema_version: "timeline-session-snapshot-page/0.1".to_string(),
            session_id: session_id.to_string(),
            snapshot_identity,
            floor,
            watermark,
            active_turn: None,
            total_items: 0,
            total_canonical_bytes: 0,
            after: None,
            items: Vec::new(),
            next_after: None,
            complete: true,
            page_identity: String::new(),
        };
        page.page_identity = timeline_snapshot_page_identity(&page).unwrap();
        page.validate().unwrap();
        page
    }

    #[test]
    fn connection_generation_is_fixed_and_validated() {
        assert_eq!(
            TimelineSubscriptionRegistry::new(0).unwrap_err(),
            TimelineSubscriptionRegistryError::InvalidConnectionGeneration
        );
        assert_eq!(
            TimelineSubscriptionRegistry::new(MAX_SAFE_JSON_INTEGER + 1).unwrap_err(),
            TimelineSubscriptionRegistryError::InvalidConnectionGeneration
        );

        let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        let head = event("session-1", 1, 1);
        let mut request = subscribe("session-1", "subscription-1", TimelineAnchor::initial());
        request.connection_generation += 1;
        assert_eq!(
            registry
                .register(
                    &request,
                    anchor(&head),
                    head.timestamp_ms,
                    TimelineSubscriptionRoute::Sync,
                )
                .unwrap_err(),
            TimelineSubscriptionRegistryError::ConnectionGenerationMismatch
        );
        assert_eq!(registry.connection_generation(), GENERATION);
        assert_eq!(registry.used_id_count(), 0);
    }

    #[test]
    fn one_attempt_per_session_and_retired_ids_are_never_reused() {
        let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        let head = event("session-1", 1, 1);
        register_sync(&mut registry, "session-1", "subscription-1", &head);
        assert_eq!(
            registry
                .register(
                    &subscribe("session-1", "subscription-2", TimelineAnchor::initial()),
                    anchor(&head),
                    head.timestamp_ms,
                    TimelineSubscriptionRoute::Sync,
                )
                .unwrap_err(),
            TimelineSubscriptionRegistryError::SessionAlreadyHasAttempt
        );

        let retired = registry.retire("subscription-1").unwrap();
        assert_eq!(
            retired.final_state,
            TimelineSubscriptionAttemptState::PendingSync
        );
        assert_eq!(registry.used_id_count(), 1);
        assert_eq!(
            registry
                .register(
                    &subscribe("session-1", "subscription-1", TimelineAnchor::initial()),
                    anchor(&head),
                    head.timestamp_ms,
                    TimelineSubscriptionRoute::Sync,
                )
                .unwrap_err(),
            TimelineSubscriptionRegistryError::SubscriptionIdAlreadyUsed
        );
        registry
            .register(
                &subscribe("session-1", "subscription-2", TimelineAnchor::initial()),
                anchor(&head),
                head.timestamp_ms,
                TimelineSubscriptionRoute::Sync,
            )
            .unwrap();
    }

    #[test]
    fn used_id_capacity_is_connection_lifetime_bounded() {
        let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        for index in 0..MAX_TIMELINE_SUBSCRIPTION_IDS_PER_CONNECTION {
            let session_id = format!("session-{index}");
            let subscription_id = format!("subscription-{index}");
            registry
                .register(
                    &subscribe(&session_id, &subscription_id, TimelineAnchor::initial()),
                    TimelineAnchor::initial(),
                    0,
                    TimelineSubscriptionRoute::Snapshot,
                )
                .unwrap();
            registry.retire(&subscription_id).unwrap();
        }
        assert_eq!(
            registry.used_id_count(),
            MAX_TIMELINE_SUBSCRIPTION_IDS_PER_CONNECTION
        );
        assert_eq!(registry.attempt_count(), 0);
        assert_eq!(
            registry
                .register(
                    &subscribe(
                        "session-over",
                        "subscription-over",
                        TimelineAnchor::initial()
                    ),
                    TimelineAnchor::initial(),
                    0,
                    TimelineSubscriptionRoute::Snapshot,
                )
                .unwrap_err(),
            TimelineSubscriptionRegistryError::SubscriptionIdCapacityExceeded
        );
    }

    #[test]
    fn sync_activation_atomically_drains_events_buffered_after_fixed_head() {
        let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        let head = event("session-1", 1, 10);
        let result = register_sync(&mut registry, "session-1", "subscription-1", &head);

        assert_eq!(
            registry.publish_event(head.clone()).unwrap(),
            TimelineEventPublication::Disposition(
                TimelineEventDisposition::IgnoredAtOrBeforeFixedHead
            )
        );
        let second = event("session-1", 2, 11);
        let third = event("session-1", 3, 12);
        assert_eq!(
            registry.publish_event(second.clone()).unwrap(),
            TimelineEventPublication::Disposition(TimelineEventDisposition::Buffered)
        );
        assert_eq!(
            registry.publish_event(third.clone()).unwrap(),
            TimelineEventPublication::Disposition(TimelineEventDisposition::Buffered)
        );
        let token = finish_sync(&mut registry, &result, vec![head.clone()]);
        let request = activate_request(
            &result,
            TimelineSubscriptionSource::Sync,
            anchor(&head),
            None,
        );
        let activation = registry.activate(token, &request).unwrap();
        assert_eq!(activation.result.cursor, anchor(&head));
        assert_eq!(activation.drain.len(), 2);
        assert_eq!(activation.drain[0].cursor, anchor(&head));
        assert_eq!(activation.drain[0].event, second);
        assert_eq!(
            activation.drain[1].cursor,
            activation.drain[0].next_cursor()
        );
        assert_eq!(activation.drain[1].event, third);
        assert_eq!(registry.pending_totals(), (0, 0));
        let status = registry.status("subscription-1").unwrap();
        assert_eq!(status.state, TimelineSubscriptionAttemptState::Active);
        assert_eq!(status.current_cursor, activation.drain[1].next_cursor());
    }

    #[test]
    fn first_post_watermark_event_cannot_precede_durable_fixed_head_timestamp() {
        let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        let head = event("session-1", 1, 100);
        register_sync(&mut registry, "session-1", "subscription-1", &head);

        assert_eq!(
            registry
                .publish_event(event("session-1", 2, head.timestamp_ms - 1))
                .unwrap_err(),
            TimelineSubscriptionRegistryError::EventTimestampReverse
        );
        let status = registry.status("subscription-1").unwrap();
        assert_eq!(status.fixed_head_timestamp_ms, head.timestamp_ms);
        assert_eq!(status.state, TimelineSubscriptionAttemptState::Failed);
        assert_eq!(registry.pending_totals(), (0, 0));
    }

    #[test]
    fn registration_rejects_timestamp_that_cannot_describe_the_fixed_head() {
        let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        let head = event("session-1", 1, 100);
        assert_eq!(
            registry
                .register(
                    &subscribe("session-1", "subscription-1", TimelineAnchor::initial()),
                    anchor(&head),
                    0,
                    TimelineSubscriptionRoute::Sync,
                )
                .unwrap_err(),
            TimelineSubscriptionRegistryError::InvalidFixedHead
        );
        assert_eq!(registry.used_id_count(), 0);

        assert_eq!(
            registry
                .register(
                    &subscribe("session-1", "subscription-1", TimelineAnchor::initial()),
                    TimelineAnchor::initial(),
                    1,
                    TimelineSubscriptionRoute::Snapshot,
                )
                .unwrap_err(),
            TimelineSubscriptionRegistryError::InvalidFixedHead
        );
        assert_eq!(registry.used_id_count(), 0);
    }

    #[test]
    fn snapshot_recovery_is_bound_to_the_captured_fixed_head() {
        let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        let request = subscribe("session-1", "subscription-1", TimelineAnchor::initial());
        let result = registry
            .register(
                &request,
                TimelineAnchor::initial(),
                0,
                TimelineSubscriptionRoute::Snapshot,
            )
            .unwrap();
        let page_request = TimelineSubscriptionSnapshotParams {
            schema_version: TIMELINE_SUBSCRIPTION_SNAPSHOT_REQUEST_SCHEMA.to_string(),
            connection_generation: GENERATION,
            session_id: "session-1".to_string(),
            subscription_id: "subscription-1".to_string(),
            request: TimelineSnapshotParams {
                session_id: "session-1".to_string(),
                snapshot_identity: None,
                watermark: None,
                after: None,
                limit: 200,
            },
        };
        let page = empty_snapshot_page("session-1");
        let snapshot_identity = page.snapshot_identity.clone();
        let token = registry
            .accept_snapshot_page("subscription-1", &page_request, page)
            .unwrap()
            .unwrap();
        let activation_request = activate_request(
            &result,
            TimelineSubscriptionSource::Snapshot,
            TimelineAnchor::initial(),
            Some(snapshot_identity),
        );
        let activation = registry.activate(token, &activation_request).unwrap();
        assert!(activation.drain.is_empty());
        assert_eq!(
            registry.status("subscription-1").unwrap().state,
            TimelineSubscriptionAttemptState::Active
        );
    }

    #[test]
    fn subscription_snapshot_wrapper_rejects_forged_subscription_binding() {
        let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        registry
            .register(
                &subscribe("session-1", "subscription-1", TimelineAnchor::initial()),
                TimelineAnchor::initial(),
                0,
                TimelineSubscriptionRoute::Snapshot,
            )
            .unwrap();
        let request = TimelineSubscriptionSnapshotParams {
            schema_version: TIMELINE_SUBSCRIPTION_SNAPSHOT_REQUEST_SCHEMA.to_string(),
            connection_generation: GENERATION,
            session_id: "session-1".to_string(),
            subscription_id: "subscription-forged".to_string(),
            request: TimelineSnapshotParams {
                session_id: "session-1".to_string(),
                snapshot_identity: None,
                watermark: None,
                after: None,
                limit: 200,
            },
        };
        assert_eq!(
            registry
                .accept_snapshot_page("subscription-1", &request, empty_snapshot_page("session-1"),)
                .unwrap_err(),
            TimelineSubscriptionRegistryError::InvalidRecoveryRequest
        );
        assert_eq!(
            registry.status("subscription-1").unwrap().state,
            TimelineSubscriptionAttemptState::Failed
        );
    }

    #[test]
    fn recovery_request_drift_fails_closed_and_clears_pending_events() {
        let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        let head = event("session-1", 1, 10);
        let result = register_sync(&mut registry, "session-1", "subscription-1", &head);
        registry.publish_event(event("session-1", 2, 11)).unwrap();
        let mut request = TimelineSubscriptionSyncParams {
            schema_version: TIMELINE_SUBSCRIPTION_SYNC_REQUEST_SCHEMA.to_string(),
            connection_generation: GENERATION,
            session_id: result.session_id.clone(),
            subscription_id: result.subscription_id.clone(),
            request: TimelineSyncParams {
                session_id: result.session_id.clone(),
                after: result.cursor.clone(),
                watermark: result.watermark.clone(),
                limit: 200,
            },
        };
        request.request.watermark = Some(TimelineAnchor::initial());
        let page = TimelineSyncPage {
            schema_version: "timeline-sync-page/0.1".to_string(),
            session_id: result.session_id,
            after: TimelineAnchor::initial(),
            watermark: anchor(&head),
            events: vec![head],
            next_after: None,
            complete: true,
        };
        assert_eq!(
            registry
                .accept_sync_page("subscription-1", &request, page)
                .unwrap_err(),
            TimelineSubscriptionRegistryError::InvalidRecoveryRequest
        );
        let status = registry.status("subscription-1").unwrap();
        assert_eq!(status.state, TimelineSubscriptionAttemptState::Failed);
        assert_eq!(status.buffered_events, 0);
        assert_eq!(registry.pending_totals(), (0, 0));
    }

    #[test]
    fn subscription_sync_wrapper_rejects_forged_binding_and_stale_generation() {
        enum Drift {
            Subscription,
            Generation,
            Session,
        }
        for drift in [Drift::Subscription, Drift::Generation, Drift::Session] {
            let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
            let head = event("session-1", 1, 10);
            let result = register_sync(&mut registry, "session-1", "subscription-1", &head);
            let mut request = sync_request(&result, TimelineAnchor::initial());
            match drift {
                Drift::Subscription => request.subscription_id = "subscription-forged".to_string(),
                Drift::Generation => request.connection_generation += 1,
                Drift::Session => {
                    request.session_id = "session-cross".to_string();
                    request.request.session_id = "session-cross".to_string();
                }
            }
            let page = TimelineSyncPage {
                schema_version: "timeline-sync-page/0.1".to_string(),
                session_id: "session-1".to_string(),
                after: TimelineAnchor::initial(),
                watermark: anchor(&head),
                events: vec![head],
                next_after: None,
                complete: true,
            };
            assert_eq!(
                registry
                    .accept_sync_page("subscription-1", &request, page)
                    .unwrap_err(),
                TimelineSubscriptionRegistryError::InvalidRecoveryRequest
            );
            assert_eq!(
                registry.status("subscription-1").unwrap().state,
                TimelineSubscriptionAttemptState::Failed
            );
        }
    }

    #[test]
    fn subscription_sync_continuation_drift_fails_closed() {
        let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        let first = event("session-1", 1, 10);
        let second = event("session-1", 2, 11);
        let result = registry
            .register(
                &subscribe("session-1", "subscription-1", TimelineAnchor::initial()),
                anchor(&second),
                second.timestamp_ms,
                TimelineSubscriptionRoute::Sync,
            )
            .unwrap();
        let first_request = sync_request(&result, TimelineAnchor::initial());
        let first_page = TimelineSyncPage {
            schema_version: "timeline-sync-page/0.1".to_string(),
            session_id: "session-1".to_string(),
            after: TimelineAnchor::initial(),
            watermark: anchor(&second),
            events: vec![first.clone()],
            next_after: Some(anchor(&first)),
            complete: false,
        };
        assert!(registry
            .accept_sync_page("subscription-1", &first_request, first_page)
            .unwrap()
            .is_none());

        let drifted_request = sync_request(&result, TimelineAnchor::initial());
        let final_page = TimelineSyncPage {
            schema_version: "timeline-sync-page/0.1".to_string(),
            session_id: "session-1".to_string(),
            after: TimelineAnchor::initial(),
            watermark: anchor(&second),
            events: vec![first, second],
            next_after: None,
            complete: true,
        };
        assert_eq!(
            registry
                .accept_sync_page("subscription-1", &drifted_request, final_page)
                .unwrap_err(),
            TimelineSubscriptionRegistryError::InvalidRecoveryRequest
        );
        assert_eq!(
            registry.status("subscription-1").unwrap().state,
            TimelineSubscriptionAttemptState::Failed
        );
    }

    #[test]
    fn gap_duplicate_reverse_and_timestamp_reverse_fail_closed() {
        let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        let head = event("session-1", 1, 10);
        register_sync(&mut registry, "session-1", "subscription-1", &head);
        assert_eq!(
            registry
                .publish_event(event("session-1", 3, 11))
                .unwrap_err(),
            TimelineSubscriptionRegistryError::EventGap
        );
        assert_eq!(
            registry.status("subscription-1").unwrap().state,
            TimelineSubscriptionAttemptState::Failed
        );

        let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        register_sync(&mut registry, "session-1", "subscription-1", &head);
        registry.publish_event(event("session-1", 2, 12)).unwrap();
        assert_eq!(
            registry
                .publish_event(event("session-1", 3, 11))
                .unwrap_err(),
            TimelineSubscriptionRegistryError::EventTimestampReverse
        );
        assert_eq!(
            registry.status("subscription-1").unwrap().state,
            TimelineSubscriptionAttemptState::Failed
        );

        let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        register_sync(&mut registry, "session-1", "subscription-1", &head);
        registry.publish_event(event("session-1", 2, 11)).unwrap();
        assert_eq!(
            registry
                .publish_event(event("session-1", 2, 11))
                .unwrap_err(),
            TimelineSubscriptionRegistryError::EventDuplicateOrReverse
        );
        assert_eq!(registry.pending_totals(), (0, 0));
    }

    #[test]
    fn malformed_event_for_a_bound_session_fails_the_attempt() {
        let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        let head = event("session-1", 1, 10);
        register_sync(&mut registry, "session-1", "subscription-1", &head);
        let mut malformed = event("session-1", 2, 11);
        malformed.event_id = "event:sha256:invalid".to_string();
        assert_eq!(
            registry.publish_event(malformed).unwrap_err(),
            TimelineSubscriptionRegistryError::InvalidEvent
        );
        assert_eq!(
            registry.status("subscription-1").unwrap().state,
            TimelineSubscriptionAttemptState::Failed
        );
    }

    #[test]
    fn pending_count_and_bytes_limits_allow_exact_boundary_only() {
        assert_eq!(
            checked_pending_totals(
                MAX_PENDING_TIMELINE_EVENTS_PER_CONNECTION - 1,
                MAX_PENDING_TIMELINE_BYTES_PER_CONNECTION - 1,
                1,
                1,
            )
            .unwrap(),
            (
                MAX_PENDING_TIMELINE_EVENTS_PER_CONNECTION,
                MAX_PENDING_TIMELINE_BYTES_PER_CONNECTION
            )
        );
        assert_eq!(
            checked_pending_totals(MAX_PENDING_TIMELINE_EVENTS_PER_CONNECTION, 0, 1, 0,)
                .unwrap_err(),
            TimelineSubscriptionRegistryError::PendingEventCountExceeded
        );
        assert_eq!(
            checked_pending_totals(0, MAX_PENDING_TIMELINE_BYTES_PER_CONNECTION, 0, 1,)
                .unwrap_err(),
            TimelineSubscriptionRegistryError::PendingEventBytesExceeded
        );
    }

    #[test]
    fn recovery_budget_is_shared_across_sessions_and_failure_releases_only_its_owner() {
        let mut registry = TimelineSubscriptionRegistry::with_test_pending_limits(
            GENERATION,
            2,
            MAX_PENDING_TIMELINE_BYTES_PER_CONNECTION,
        )
        .unwrap();
        let head_a = event("session-a", 2, 2);
        let head_b = event("session-b", 2, 2);
        let result_a = register_sync(&mut registry, "session-a", "subscription-a", &head_a);
        let result_b = register_sync(&mut registry, "session-b", "subscription-b", &head_b);
        let first_a = event("session-a", 1, 1);
        let first_b = event("session-b", 1, 1);
        let request_a = sync_request(&result_a, TimelineAnchor::initial());
        let request_b = sync_request(&result_b, TimelineAnchor::initial());

        assert!(registry
            .accept_sync_page(
                "subscription-a",
                &request_a,
                sync_page(&request_a, vec![first_a.clone()], false),
            )
            .unwrap()
            .is_none());
        assert!(registry
            .accept_sync_page(
                "subscription-b",
                &request_b,
                sync_page(&request_b, vec![first_b], false),
            )
            .unwrap()
            .is_none());
        assert_eq!(registry.pending_totals().0, 2);

        let continuation_a = sync_request(&result_a, anchor(&first_a));
        assert_eq!(
            registry
                .accept_sync_page(
                    "subscription-a",
                    &continuation_a,
                    sync_page(&continuation_a, vec![head_a], true),
                )
                .unwrap_err(),
            TimelineSubscriptionRegistryError::RecoveryLimitExceeded
        );
        assert_eq!(
            registry.status("subscription-a").unwrap().state,
            TimelineSubscriptionAttemptState::Failed
        );
        assert_eq!(
            registry.status("subscription-b").unwrap().state,
            TimelineSubscriptionAttemptState::PendingSync
        );
        assert_eq!(registry.pending_totals().0, 1);
    }

    #[test]
    fn recovery_and_live_events_share_the_same_connection_byte_budget() {
        let head = event("session-1", 2, 2);
        let result_template = TimelineSubscribeResult {
            schema_version: TIMELINE_SUBSCRIBE_RESULT_SCHEMA.to_string(),
            connection_generation: GENERATION,
            session_id: "session-1".to_string(),
            subscription_id: "subscription-1".to_string(),
            state: TimelineSubscriptionState::SyncRequired,
            cursor: TimelineAnchor::initial(),
            watermark: Some(anchor(&head)),
            next_method: "timeline/subscription-sync".to_string(),
        };
        let first = event("session-1", 1, 1);
        let request = sync_request(&result_template, TimelineAnchor::initial());
        let page = sync_page(&request, vec![first], false);
        let first_live = event("session-1", 3, 3);
        let byte_limit = u64::try_from(serde_json::to_vec(&page).unwrap().len()).unwrap()
            + u64::try_from(serde_json::to_vec(&first_live).unwrap().len()).unwrap();
        let mut registry =
            TimelineSubscriptionRegistry::with_test_pending_limits(GENERATION, 10, byte_limit)
                .unwrap();
        let result = register_sync(&mut registry, "session-1", "subscription-1", &head);
        assert_eq!(result, result_template);

        assert!(registry
            .accept_sync_page("subscription-1", &request, page)
            .unwrap()
            .is_none());
        assert_eq!(
            registry.publish_event(first_live).unwrap(),
            TimelineEventPublication::Disposition(TimelineEventDisposition::Buffered)
        );
        assert_eq!(registry.pending_totals().1, byte_limit);

        assert_eq!(
            registry
                .publish_event(event("session-1", 4, 4))
                .unwrap_err(),
            TimelineSubscriptionRegistryError::PendingEventBytesExceeded
        );
        assert_eq!(
            registry.status("subscription-1").unwrap().state,
            TimelineSubscriptionAttemptState::Failed
        );
        assert_eq!(registry.pending_totals(), (0, 0));
    }

    #[test]
    fn activation_token_cannot_cross_connection_registries() {
        let mut first = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        let mut second = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        let head = event("session-1", 1, 10);
        let first_result = register_sync(&mut first, "session-1", "subscription-1", &head);
        let second_result = register_sync(&mut second, "session-1", "subscription-1", &head);
        let token = finish_sync(&mut first, &first_result, vec![head.clone()]);
        let request = activate_request(
            &second_result,
            TimelineSubscriptionSource::Sync,
            anchor(&head),
            None,
        );
        assert_eq!(
            second.activate(token, &request).unwrap_err(),
            TimelineSubscriptionRegistryError::ActivationTokenMismatch
        );
        assert_eq!(
            second.status("subscription-1").unwrap().state,
            TimelineSubscriptionAttemptState::Failed
        );
    }

    #[test]
    fn active_events_advance_once_and_duplicate_delivery_fails_closed() {
        let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        let head = event("session-1", 1, 10);
        let result = register_sync(&mut registry, "session-1", "subscription-1", &head);
        let token = finish_sync(&mut registry, &result, vec![head.clone()]);
        let request = activate_request(
            &result,
            TimelineSubscriptionSource::Sync,
            anchor(&head),
            None,
        );
        registry.activate(token, &request).unwrap();
        let next = event("session-1", 2, 11);
        let publication = registry.publish_event(next.clone()).unwrap();
        let TimelineEventPublication::Live(live) = publication else {
            panic!("active event was not published live")
        };
        assert_eq!(live.event, next.clone());
        assert_eq!(live.cursor, anchor(&head));
        assert_eq!(
            registry.publish_event(next).unwrap_err(),
            TimelineSubscriptionRegistryError::EventDuplicateOrReverse
        );
        assert_eq!(
            registry.status("subscription-1").unwrap().state,
            TimelineSubscriptionAttemptState::Failed
        );
    }

    #[test]
    fn disconnect_consumes_all_connection_owned_state() {
        let mut registry = TimelineSubscriptionRegistry::new(GENERATION).unwrap();
        let head = event("session-1", 1, 10);
        register_sync(&mut registry, "session-1", "subscription-1", &head);
        registry.publish_event(event("session-1", 2, 11)).unwrap();
        let (_, pending_bytes) = registry.pending_totals();
        let report = registry.disconnect();
        assert_eq!(report.attempts_dropped, 1);
        assert_eq!(report.used_ids_dropped, 1);
        assert_eq!(report.pending_events_dropped, 1);
        assert_eq!(report.pending_bytes_dropped, pending_bytes);
    }
}
