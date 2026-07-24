use crate::event_sequencer::EventSequencer;
use aegisy_aap::stable::v0_1::EventEnvelope;
use aegisy_aap::MAX_AAP_FRAME_BYTES;
use rusqlite::{params, Connection, OptionalExtension, Transaction};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

pub const JOURNAL_SCHEMA_VERSION: &str = "public-timeline-journal/0.1";
pub const SYNC_SCHEMA_VERSION: &str = "timeline-sync-page/0.1";
pub const MAX_SYNC_PAGE: usize = 200;
pub const MAX_SESSION_EVENTS: u64 = 100_000;
pub const MAX_JOURNAL_SESSIONS: u64 = 10_000;
pub const MAX_TOTAL_EVENTS: u64 = 100_000;
pub const MAX_SYNC_EVENTS_BYTES: u64 = MAX_AAP_FRAME_BYTES - 512 * 1024;

pub const SCHEMA_SQL: &str = "
    CREATE TABLE IF NOT EXISTS public_timeline_events (
        session_id TEXT NOT NULL,
        sequence INTEGER NOT NULL CHECK(sequence >= 1 AND sequence <= 9007199254740991),
        event_id TEXT NOT NULL UNIQUE,
        timestamp_ms INTEGER NOT NULL CHECK(timestamp_ms >= 1 AND timestamp_ms <= 9007199254740991),
        turn_id TEXT NOT NULL,
        envelope_json TEXT NOT NULL,
        envelope_sha256 TEXT NOT NULL,
        envelope_bytes INTEGER NOT NULL CHECK(envelope_bytes > 0 AND envelope_bytes <= 3670016),
        PRIMARY KEY(session_id, sequence),
        FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
    ) STRICT;
    CREATE INDEX IF NOT EXISTS public_timeline_events_turn_idx
        ON public_timeline_events(session_id, turn_id, sequence);
    CREATE TABLE IF NOT EXISTS public_timeline_cursors (
        session_id TEXT PRIMARY KEY,
        next_sequence INTEGER NOT NULL CHECK(next_sequence >= 1 AND next_sequence <= 9007199254740991),
        last_timestamp_ms INTEGER NOT NULL CHECK(last_timestamp_ms >= 0 AND last_timestamp_ms <= 9007199254740991),
        latest_event_id TEXT,
        CHECK(
            (next_sequence = 1 AND last_timestamp_ms = 0 AND latest_event_id IS NULL)
            OR
            (next_sequence > 1 AND last_timestamp_ms >= 1 AND latest_event_id IS NOT NULL)
        ),
        FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
    ) STRICT;
    CREATE TRIGGER IF NOT EXISTS public_timeline_session_cursor_insert
    AFTER INSERT ON sessions
    BEGIN
        INSERT OR IGNORE INTO public_timeline_cursors (
            session_id, next_sequence, last_timestamp_ms, latest_event_id
        ) VALUES (NEW.session_id, 1, 0, NULL);
    END;
";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct JournalError {
    pub code: &'static str,
}

impl JournalError {
    fn new(code: &'static str) -> Self {
        Self { code }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct TimelineWatermark {
    pub sequence: u64,
    pub event_id: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct TimelineSyncPage {
    pub schema_version: String,
    pub session_id: String,
    pub after_sequence: u64,
    pub watermark: TimelineWatermark,
    pub events: Vec<EventEnvelope>,
    pub next_after_sequence: Option<u64>,
    pub complete: bool,
}

pub fn apply_schema(connection: &Connection) -> Result<(), JournalError> {
    connection
        .execute_batch(SCHEMA_SQL)
        .map_err(|_| JournalError::new("timeline-journal-schema-failed"))?;
    connection
        .execute(
            "INSERT OR IGNORE INTO public_timeline_cursors (
                session_id, next_sequence, last_timestamp_ms, latest_event_id
             ) SELECT session_id, 1, 0, NULL FROM sessions",
            [],
        )
        .map_err(|_| JournalError::new("timeline-journal-cursor-backfill-failed"))?;
    Ok(())
}

pub fn append_event_tx(
    transaction: &Transaction<'_>,
    event: &EventEnvelope,
) -> Result<(), JournalError> {
    event
        .validate()
        .map_err(|_| JournalError::new("timeline-journal-event-invalid"))?;
    let cursor = transaction
        .query_row(
            "SELECT next_sequence, last_timestamp_ms, latest_event_id
             FROM public_timeline_cursors WHERE session_id = ?1",
            [&event.session_id],
            |row| {
                Ok((
                    row.get::<_, i64>(0)?,
                    row.get::<_, i64>(1)?,
                    row.get::<_, Option<String>>(2)?,
                ))
            },
        )
        .optional()
        .map_err(|_| JournalError::new("timeline-journal-cursor-read-failed"))?;
    let (next_sequence, last_timestamp, latest_event_id) = match cursor {
        Some((next_sequence, last_timestamp, latest_event_id)) => (
            u64::try_from(next_sequence)
                .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?,
            u64::try_from(last_timestamp)
                .map_err(|_| JournalError::new("timeline-journal-timestamp-invalid"))?,
            latest_event_id,
        ),
        None => return Err(JournalError::new("timeline-journal-cursor-missing")),
    };
    validate_cursor_state(next_sequence, last_timestamp, latest_event_id.as_deref())?;
    validate_cursor_anchor(
        transaction,
        &event.session_id,
        next_sequence,
        last_timestamp,
        latest_event_id.as_deref(),
    )?;
    if next_sequence > MAX_SESSION_EVENTS {
        return Err(JournalError::new("timeline-journal-session-limit"));
    }
    let total_events: i64 = transaction
        .query_row("SELECT COUNT(*) FROM public_timeline_events", [], |row| {
            row.get(0)
        })
        .map_err(|_| JournalError::new("timeline-journal-count-read-failed"))?;
    let total_events = u64::try_from(total_events)
        .map_err(|_| JournalError::new("timeline-journal-count-invalid"))?;
    if total_events >= MAX_TOTAL_EVENTS {
        return Err(JournalError::new("timeline-journal-total-limit"));
    }
    if event.sequence != next_sequence {
        return Err(JournalError::new("timeline-journal-sequence-gap"));
    }
    if event.timestamp_ms < last_timestamp {
        return Err(JournalError::new("timeline-journal-timestamp-regression"));
    }

    let envelope_json = serde_json::to_string(event)
        .map_err(|_| JournalError::new("timeline-journal-serialize-failed"))?;
    let envelope_bytes = u64::try_from(envelope_json.len())
        .map_err(|_| JournalError::new("timeline-journal-event-too-large"))?;
    if envelope_bytes == 0 || envelope_bytes > MAX_SYNC_EVENTS_BYTES {
        return Err(JournalError::new(
            "timeline-journal-event-too-large-for-sync",
        ));
    }
    let envelope_sha256 = format!("{:x}", Sha256::digest(envelope_json.as_bytes()));
    transaction
        .execute(
            "INSERT INTO public_timeline_events (
                session_id, sequence, event_id, timestamp_ms, turn_id,
                envelope_json, envelope_sha256, envelope_bytes
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
            params![
                event.session_id,
                i64::try_from(event.sequence)
                    .map_err(|_| JournalError::new("timeline-journal-sequence-invalid"))?,
                event.event_id,
                i64::try_from(event.timestamp_ms)
                    .map_err(|_| JournalError::new("timeline-journal-timestamp-invalid"))?,
                event.turn_id,
                envelope_json,
                envelope_sha256,
                i64::try_from(envelope_bytes)
                    .map_err(|_| JournalError::new("timeline-journal-event-too-large"))?,
            ],
        )
        .map_err(|_| JournalError::new("timeline-journal-append-failed"))?;
    let following_sequence = next_sequence
        .checked_add(1)
        .ok_or_else(|| JournalError::new("timeline-journal-cursor-invalid"))?;
    let cursor_changed = transaction
        .execute(
            "UPDATE public_timeline_cursors
             SET next_sequence = ?2, last_timestamp_ms = ?3, latest_event_id = ?4
             WHERE session_id = ?1 AND next_sequence = ?5
               AND last_timestamp_ms = ?6 AND latest_event_id IS ?7",
            params![
                event.session_id,
                i64::try_from(following_sequence)
                    .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?,
                i64::try_from(event.timestamp_ms)
                    .map_err(|_| JournalError::new("timeline-journal-timestamp-invalid"))?,
                event.event_id,
                i64::try_from(next_sequence)
                    .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?,
                i64::try_from(last_timestamp)
                    .map_err(|_| JournalError::new("timeline-journal-timestamp-invalid"))?,
                latest_event_id,
            ],
        )
        .map_err(|_| JournalError::new("timeline-journal-cursor-write-failed"))?;
    if cursor_changed != 1 {
        return Err(JournalError::new("timeline-journal-cursor-write-conflict"));
    }
    Ok(())
}

fn validate_cursor_state(
    next_sequence: u64,
    last_timestamp_ms: u64,
    latest_event_id: Option<&str>,
) -> Result<(), JournalError> {
    let empty = next_sequence == 1 && last_timestamp_ms == 0 && latest_event_id.is_none();
    let populated =
        next_sequence > 1 && last_timestamp_ms > 0 && latest_event_id.is_some_and(valid_event_id);
    if empty || populated {
        Ok(())
    } else {
        Err(JournalError::new("timeline-journal-cursor-invalid"))
    }
}

fn valid_event_id(value: &str) -> bool {
    value.strip_prefix("event:sha256:").is_some_and(|digest| {
        digest.len() == 64
            && digest
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    })
}

fn validate_cursor_anchor(
    connection: &Connection,
    session_id: &str,
    next_sequence: u64,
    last_timestamp_ms: u64,
    latest_event_id: Option<&str>,
) -> Result<(), JournalError> {
    let anchor = if next_sequence == 1 {
        None
    } else {
        connection
            .query_row(
                "SELECT event_id, timestamp_ms FROM public_timeline_events
                 WHERE session_id = ?1 AND sequence = ?2",
                params![
                    session_id,
                    i64::try_from(next_sequence - 1)
                        .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?
                ],
                |row| Ok((row.get::<_, String>(0)?, row.get::<_, i64>(1)?)),
            )
            .optional()
            .map_err(|_| JournalError::new("timeline-journal-cursor-read-failed"))?
    };
    let anchor_matches = match anchor {
        None => next_sequence == 1 && last_timestamp_ms == 0 && latest_event_id.is_none(),
        Some((event_id, timestamp_ms)) => {
            u64::try_from(timestamp_ms).ok() == Some(last_timestamp_ms)
                && latest_event_id == Some(event_id.as_str())
        }
    };
    let events_at_or_after_cursor: bool = connection
        .query_row(
            "SELECT EXISTS(
                SELECT 1 FROM public_timeline_events
                WHERE session_id = ?1 AND sequence >= ?2
             )",
            params![
                session_id,
                i64::try_from(next_sequence)
                    .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?
            ],
            |row| row.get(0),
        )
        .map_err(|_| JournalError::new("timeline-journal-cursor-read-failed"))?;
    if anchor_matches && !events_at_or_after_cursor {
        Ok(())
    } else {
        Err(JournalError::new("timeline-journal-cursor-mismatch"))
    }
}

fn validate_watermark(watermark: &TimelineWatermark) -> Result<(), JournalError> {
    if (watermark.sequence == 0 && watermark.event_id.is_none())
        || (watermark.sequence > 0 && watermark.event_id.as_deref().is_some_and(valid_event_id))
    {
        Ok(())
    } else {
        Err(JournalError::new("timeline-sync-watermark-invalid"))
    }
}

fn read_current_watermark(
    connection: &Connection,
    session_id: &str,
) -> Result<TimelineWatermark, JournalError> {
    let cursor = connection
        .query_row(
            "SELECT next_sequence, last_timestamp_ms, latest_event_id
             FROM public_timeline_cursors WHERE session_id = ?1",
            [session_id],
            |row| {
                Ok((
                    row.get::<_, i64>(0)?,
                    row.get::<_, i64>(1)?,
                    row.get::<_, Option<String>>(2)?,
                ))
            },
        )
        .optional()
        .map_err(|_| JournalError::new("timeline-sync-watermark-read-failed"))?;
    let Some((next_sequence, last_timestamp_ms, latest_event_id)) = cursor else {
        return Err(JournalError::new("timeline-journal-cursor-missing"));
    };
    let next_sequence = u64::try_from(next_sequence)
        .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?;
    let last_timestamp_ms = u64::try_from(last_timestamp_ms)
        .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?;
    validate_cursor_state(next_sequence, last_timestamp_ms, latest_event_id.as_deref())?;
    validate_cursor_anchor(
        connection,
        session_id,
        next_sequence,
        last_timestamp_ms,
        latest_event_id.as_deref(),
    )?;
    Ok(TimelineWatermark {
        sequence: next_sequence - 1,
        event_id: latest_event_id,
    })
}

pub fn sync_page(
    connection: &Connection,
    session_id: &str,
    after_sequence: u64,
    requested_watermark: Option<&TimelineWatermark>,
    limit: usize,
) -> Result<TimelineSyncPage, JournalError> {
    if session_id.is_empty() || session_id.len() > 128 || limit == 0 || limit > MAX_SYNC_PAGE {
        return Err(JournalError::new("timeline-sync-request-invalid"));
    }
    let session_exists: bool = connection
        .query_row(
            "SELECT EXISTS(SELECT 1 FROM sessions WHERE session_id = ?1)",
            [session_id],
            |row| row.get(0),
        )
        .map_err(|_| JournalError::new("timeline-sync-session-read-failed"))?;
    if !session_exists {
        return Err(JournalError::new("timeline-sync-session-missing"));
    }
    let current_watermark = read_current_watermark(connection, session_id)?;
    let watermark = requested_watermark
        .cloned()
        .unwrap_or_else(|| current_watermark.clone());
    validate_watermark(&watermark)?;
    if watermark.sequence > current_watermark.sequence || after_sequence > watermark.sequence {
        return Err(JournalError::new("timeline-sync-watermark-invalid"));
    }
    if watermark.sequence > 0 {
        let stored_event_id = connection
            .query_row(
                "SELECT event_id FROM public_timeline_events
                 WHERE session_id = ?1 AND sequence = ?2",
                params![
                    session_id,
                    i64::try_from(watermark.sequence)
                        .map_err(|_| JournalError::new("timeline-sync-watermark-invalid"))?
                ],
                |row| row.get::<_, String>(0),
            )
            .optional()
            .map_err(|_| JournalError::new("timeline-sync-watermark-read-failed"))?;
        if stored_event_id.as_deref() != watermark.event_id.as_deref() {
            return Err(JournalError::new("timeline-sync-retention-gap"));
        }
    }

    let mut statement = connection
        .prepare(
            "SELECT sequence, event_id, timestamp_ms, turn_id,
                    envelope_json, envelope_sha256, envelope_bytes
             FROM public_timeline_events
             WHERE session_id = ?1 AND sequence > ?2 AND sequence <= ?3
             ORDER BY sequence ASC LIMIT ?4",
        )
        .map_err(|_| JournalError::new("timeline-sync-query-failed"))?;
    let rows = statement
        .query_map(
            params![
                session_id,
                i64::try_from(after_sequence)
                    .map_err(|_| JournalError::new("timeline-sync-request-invalid"))?,
                i64::try_from(watermark.sequence)
                    .map_err(|_| JournalError::new("timeline-sync-watermark-invalid"))?,
                i64::try_from(limit)
                    .map_err(|_| JournalError::new("timeline-sync-request-invalid"))?,
            ],
            |row| {
                Ok((
                    row.get::<_, i64>(0)?,
                    row.get::<_, String>(1)?,
                    row.get::<_, i64>(2)?,
                    row.get::<_, String>(3)?,
                    row.get::<_, String>(4)?,
                    row.get::<_, String>(5)?,
                    row.get::<_, i64>(6)?,
                ))
            },
        )
        .map_err(|_| JournalError::new("timeline-sync-query-failed"))?;
    let mut events = Vec::new();
    let mut event_bytes = 0_u64;
    let mut expected = after_sequence.saturating_add(1);
    for row in rows {
        let (
            sequence,
            event_id,
            timestamp_ms,
            turn_id,
            envelope_json,
            envelope_sha256,
            envelope_bytes,
        ) = row.map_err(|_| JournalError::new("timeline-sync-row-invalid"))?;
        let sequence =
            u64::try_from(sequence).map_err(|_| JournalError::new("timeline-sync-row-invalid"))?;
        let envelope_bytes = u64::try_from(envelope_bytes)
            .map_err(|_| JournalError::new("timeline-sync-row-invalid"))?;
        if sequence != expected
            || envelope_bytes != u64::try_from(envelope_json.len()).unwrap_or(u64::MAX)
            || format!("{:x}", Sha256::digest(envelope_json.as_bytes())) != envelope_sha256
        {
            return Err(JournalError::new("timeline-sync-integrity-failed"));
        }
        let event: EventEnvelope = serde_json::from_str(&envelope_json)
            .map_err(|_| JournalError::new("timeline-sync-envelope-invalid"))?;
        event
            .validate()
            .map_err(|_| JournalError::new("timeline-sync-envelope-invalid"))?;
        if event.session_id != session_id
            || event.sequence != sequence
            || event.event_id != event_id
            || i64::try_from(event.timestamp_ms).ok() != Some(timestamp_ms)
            || event.turn_id != turn_id
        {
            return Err(JournalError::new("timeline-sync-envelope-invalid"));
        }
        if envelope_bytes > MAX_SYNC_EVENTS_BYTES {
            return Err(JournalError::new("timeline-sync-event-too-large"));
        }
        let next_event_bytes = event_bytes
            .checked_add(envelope_bytes)
            .and_then(|bytes| bytes.checked_add(1))
            .ok_or_else(|| JournalError::new("timeline-sync-response-too-large"))?;
        if next_event_bytes > MAX_SYNC_EVENTS_BYTES {
            if events.is_empty() {
                return Err(JournalError::new("timeline-sync-event-too-large"));
            }
            break;
        }
        event_bytes = next_event_bytes;
        events.push(event);
        expected = expected.saturating_add(1);
    }
    let last_sequence = events
        .last()
        .map(|event| event.sequence)
        .unwrap_or(after_sequence);
    if last_sequence < watermark.sequence && events.is_empty() {
        return Err(JournalError::new("timeline-sync-retention-gap"));
    }
    let complete = last_sequence == watermark.sequence;
    let page = TimelineSyncPage {
        schema_version: SYNC_SCHEMA_VERSION.into(),
        session_id: session_id.into(),
        after_sequence,
        watermark,
        events,
        next_after_sequence: (!complete).then_some(last_sequence),
        complete,
    };
    let response_bytes = serde_json::to_vec(&page)
        .map_err(|_| JournalError::new("timeline-sync-response-invalid"))?
        .len();
    if u64::try_from(response_bytes).unwrap_or(u64::MAX) > MAX_AAP_FRAME_BYTES {
        return Err(JournalError::new("timeline-sync-response-too-large"));
    }
    Ok(page)
}

pub fn verify_all(connection: &Connection) -> Result<(), JournalError> {
    let total_events: i64 = connection
        .query_row("SELECT COUNT(*) FROM public_timeline_events", [], |row| {
            row.get(0)
        })
        .map_err(|_| JournalError::new("timeline-journal-verify-query-failed"))?;
    let total_events = u64::try_from(total_events)
        .map_err(|_| JournalError::new("timeline-journal-verify-row-invalid"))?;
    if total_events > MAX_TOTAL_EVENTS {
        return Err(JournalError::new("timeline-journal-total-limit"));
    }
    let mut statement = connection
        .prepare("SELECT session_id FROM sessions ORDER BY session_id LIMIT ?1")
        .map_err(|_| JournalError::new("timeline-journal-verify-query-failed"))?;
    let rows = statement
        .query_map(
            [i64::try_from(MAX_JOURNAL_SESSIONS + 1)
                .map_err(|_| JournalError::new("timeline-journal-session-limit"))?],
            |row| row.get::<_, String>(0),
        )
        .map_err(|_| JournalError::new("timeline-journal-verify-query-failed"))?;
    let mut sessions = Vec::new();
    for row in rows {
        sessions.push(row.map_err(|_| JournalError::new("timeline-journal-verify-row-invalid"))?);
    }
    drop(statement);
    if sessions.len() as u64 > MAX_JOURNAL_SESSIONS {
        return Err(JournalError::new("timeline-journal-session-limit"));
    }
    for session_id in sessions {
        let count: i64 = connection
            .query_row(
                "SELECT COUNT(*) FROM public_timeline_events WHERE session_id = ?1",
                [&session_id],
                |row| row.get(0),
            )
            .map_err(|_| JournalError::new("timeline-journal-verify-query-failed"))?;
        let count = u64::try_from(count)
            .map_err(|_| JournalError::new("timeline-journal-verify-row-invalid"))?;
        let watermark = read_current_watermark(connection, &session_id)?;
        if count > MAX_SESSION_EVENTS || count != watermark.sequence {
            return Err(JournalError::new("timeline-journal-cursor-mismatch"));
        }
        let mut after = 0;
        let mut sequencer = EventSequencer::default();
        while after < watermark.sequence {
            let page = sync_page(
                connection,
                &session_id,
                after,
                Some(&watermark),
                MAX_SYNC_PAGE,
            )?;
            for event in &page.events {
                let reproduced = sequencer
                    .sequence(
                        event.timestamp_ms,
                        &event.session_id,
                        &event.turn_id,
                        &event.event,
                        event.item.clone(),
                    )
                    .map_err(|_| JournalError::new("timeline-journal-lifecycle-invalid"))?;
                if reproduced != *event {
                    return Err(JournalError::new("timeline-journal-lifecycle-invalid"));
                }
            }
            after = page.next_after_sequence.unwrap_or_else(|| {
                page.events
                    .last()
                    .map(|event| event.sequence)
                    .unwrap_or(after)
            });
            if page.complete {
                break;
            }
        }
        if after != watermark.sequence {
            return Err(JournalError::new("timeline-journal-verify-incomplete"));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use aegisy_aap::stable::v0_1::{timeline_event_id, ItemUpdate, TimelineItem, TurnState};
    use aegisy_aap::Response;
    use rusqlite::TransactionBehavior;
    use serde_json::json;

    fn test_connection() -> Connection {
        let connection = Connection::open_in_memory().unwrap();
        connection
            .execute_batch(
                "PRAGMA foreign_keys = ON;
             CREATE TABLE sessions (session_id TEXT PRIMARY KEY) STRICT;
             INSERT INTO sessions(session_id) VALUES ('session');",
            )
            .unwrap();
        apply_schema(&connection).unwrap();
        connection
    }

    fn event(sequence: u64, timestamp_ms: u64, turn_id: &str) -> EventEnvelope {
        let item = (sequence == 2).then(|| TimelineItem {
            id: "item".into(),
            kind: "message".into(),
            role: "agent".into(),
            state: "completed".into(),
            content: "safe".into(),
            data: None,
        });
        let item_update = item.as_ref().map(|_| ItemUpdate {
            revision: 1,
            content_mode: "snapshot-replacement".into(),
        });
        let name = if sequence == 1 {
            "turn.started"
        } else if sequence == 2 {
            "item.completed"
        } else {
            "turn.completed"
        };
        let turn_state = if name == "turn.completed" {
            TurnState::Completed
        } else {
            TurnState::Running
        };
        let event_id = timeline_event_id(
            "timeline-event/0.1",
            sequence,
            timestamp_ms,
            turn_id,
            "session",
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
            session_id: "session".into(),
            turn_id: turn_id.into(),
            turn_state,
            event: name.into(),
            item,
            item_update,
        }
    }

    fn envelope(
        sequence: u64,
        timestamp_ms: u64,
        turn_id: &str,
        turn_state: TurnState,
        name: &str,
        item: Option<TimelineItem>,
    ) -> EventEnvelope {
        let item_update = item.as_ref().map(|_| ItemUpdate {
            revision: 1,
            content_mode: "snapshot-replacement".into(),
        });
        let event_id = timeline_event_id(
            "timeline-event/0.1",
            sequence,
            timestamp_ms,
            turn_id,
            "session",
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
            session_id: "session".into(),
            turn_id: turn_id.into(),
            turn_state,
            event: name.into(),
            item,
            item_update,
        }
    }

    fn completed_item_event(
        sequence: u64,
        timestamp_ms: u64,
        turn_id: &str,
        item_id: &str,
        data_bytes: usize,
    ) -> EventEnvelope {
        envelope(
            sequence,
            timestamp_ms,
            turn_id,
            TurnState::Running,
            "item.completed",
            Some(TimelineItem {
                id: item_id.into(),
                kind: "message".into(),
                role: "agent".into(),
                state: "completed".into(),
                content: "bounded".into(),
                data: Some(json!({"payload": "x".repeat(data_bytes)})),
            }),
        )
    }

    fn append(connection: &mut Connection, event: &EventEnvelope) -> Result<(), JournalError> {
        let transaction = connection
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .unwrap();
        append_event_tx(&transaction, event)?;
        transaction.commit().unwrap();
        Ok(())
    }

    #[test]
    fn append_is_contiguous_validated_and_atomic() {
        let mut connection = test_connection();
        append(&mut connection, &event(1, 10, "turn")).unwrap();
        assert_eq!(
            append(&mut connection, &event(3, 12, "turn"))
                .unwrap_err()
                .code,
            "timeline-journal-sequence-gap"
        );
        append(&mut connection, &event(2, 11, "turn")).unwrap();
        assert_eq!(
            append(&mut connection, &event(3, 9, "turn"))
                .unwrap_err()
                .code,
            "timeline-journal-timestamp-regression"
        );
        append(&mut connection, &event(3, 12, "turn")).unwrap();
        assert_eq!(
            connection
                .query_row("SELECT COUNT(*) FROM public_timeline_events", [], |row| {
                    row.get::<_, i64>(0)
                })
                .unwrap(),
            3
        );
        verify_all(&connection).unwrap();
    }

    #[test]
    fn cursor_registration_and_append_rollback_are_atomic() {
        let mut connection = test_connection();
        connection
            .execute("INSERT INTO sessions(session_id) VALUES ('second')", [])
            .unwrap();
        let registered: (i64, i64, Option<String>) = connection
            .query_row(
                "SELECT next_sequence, last_timestamp_ms, latest_event_id
                 FROM public_timeline_cursors WHERE session_id = 'second'",
                [],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )
            .unwrap();
        assert_eq!(registered, (1, 0, None));

        connection
            .execute_batch(
                "CREATE TRIGGER fail_public_timeline_cursor_update
                 BEFORE UPDATE ON public_timeline_cursors
                 BEGIN SELECT RAISE(ABORT, 'injected'); END;",
            )
            .unwrap();
        assert_eq!(
            append(&mut connection, &event(1, 10, "turn"))
                .unwrap_err()
                .code,
            "timeline-journal-cursor-write-failed"
        );
        assert_eq!(
            connection
                .query_row("SELECT COUNT(*) FROM public_timeline_events", [], |row| {
                    row.get::<_, i64>(0)
                })
                .unwrap(),
            0
        );
        let cursor: (i64, i64, Option<String>) = connection
            .query_row(
                "SELECT next_sequence, last_timestamp_ms, latest_event_id
                 FROM public_timeline_cursors WHERE session_id = 'session'",
                [],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )
            .unwrap();
        assert_eq!(cursor, (1, 0, None));
    }

    #[test]
    fn fixed_watermark_excludes_events_appended_between_pages() {
        let mut connection = test_connection();
        for event in [
            event(1, 10, "one"),
            event(2, 11, "one"),
            event(3, 12, "one"),
        ] {
            append(&mut connection, &event).unwrap();
        }
        let first = sync_page(&connection, "session", 0, None, 2).unwrap();
        assert_eq!(first.watermark.sequence, 3);
        assert_eq!(
            first.watermark.event_id.as_deref(),
            Some(event(3, 12, "one").event_id.as_str())
        );
        assert_eq!(first.next_after_sequence, Some(2));
        let fourth = envelope(4, 13, "two", TurnState::Running, "turn.started", None);
        append(&mut connection, &fourth).unwrap();
        let second = sync_page(&connection, "session", 2, Some(&first.watermark), 2).unwrap();
        assert_eq!(
            second
                .events
                .iter()
                .map(|event| event.sequence)
                .collect::<Vec<_>>(),
            vec![3]
        );
        assert!(second.complete);
        let fresh = sync_page(&connection, "session", 3, None, 2).unwrap();
        assert_eq!(fresh.watermark.sequence, 4);
        assert_eq!(fresh.events[0].sequence, 4);

        let mut forged = first.watermark.clone();
        forged.event_id = Some(format!("event:sha256:{}", "0".repeat(64)));
        assert_eq!(
            sync_page(&connection, "session", 2, Some(&forged), 2)
                .unwrap_err()
                .code,
            "timeline-sync-retention-gap"
        );
    }

    #[test]
    fn sync_rejects_payload_and_redundant_column_tampering() {
        let mut connection = test_connection();
        append(&mut connection, &event(1, 10, "turn")).unwrap();
        append(&mut connection, &event(2, 11, "turn")).unwrap();
        connection
            .execute(
                "UPDATE public_timeline_events SET envelope_json = '{}',
                        envelope_sha256 = lower(hex(zeroblob(32))), envelope_bytes = 2
                 WHERE sequence = 2",
                [],
            )
            .unwrap();
        assert!(sync_page(&connection, "session", 0, None, 10).is_err());

        let mut connection = test_connection();
        for candidate in [
            event(1, 10, "turn"),
            event(2, 11, "turn"),
            event(3, 12, "turn"),
        ] {
            append(&mut connection, &candidate).unwrap();
        }
        connection
            .execute(
                "UPDATE public_timeline_events SET turn_id = 'other' WHERE sequence = 1",
                [],
            )
            .unwrap();
        assert_eq!(
            sync_page(&connection, "session", 0, None, 10)
                .unwrap_err()
                .code,
            "timeline-sync-envelope-invalid"
        );
    }

    #[test]
    fn startup_verification_rejects_missing_stream_and_invalid_lifecycle() {
        let mut connection = test_connection();
        for candidate in [
            event(1, 10, "turn"),
            event(2, 11, "turn"),
            event(3, 12, "turn"),
        ] {
            append(&mut connection, &candidate).unwrap();
        }
        connection
            .execute("DELETE FROM public_timeline_events", [])
            .unwrap();
        assert_eq!(
            verify_all(&connection).unwrap_err().code,
            "timeline-journal-cursor-mismatch"
        );

        let mut connection = test_connection();
        let started = envelope(1, 10, "turn", TurnState::Running, "turn.started", None);
        let completed = envelope(2, 11, "turn", TurnState::Completed, "turn.completed", None);
        let after_terminal = completed_item_event(3, 12, "turn", "late-item", 0);
        for candidate in [started, completed, after_terminal] {
            append(&mut connection, &candidate).unwrap();
        }
        assert_eq!(
            verify_all(&connection).unwrap_err().code,
            "timeline-journal-lifecycle-invalid"
        );
    }

    #[test]
    fn sync_pages_by_bytes_without_skipping_and_rejects_oversized_events() {
        let mut connection = test_connection();
        append(
            &mut connection,
            &envelope(1, 10, "turn", TurnState::Running, "turn.started", None),
        )
        .unwrap();
        for index in 0..5_u64 {
            append(
                &mut connection,
                &completed_item_event(
                    index + 2,
                    index + 11,
                    "turn",
                    &format!("item-{index}"),
                    1024 * 1024,
                ),
            )
            .unwrap();
        }
        append(
            &mut connection,
            &envelope(7, 16, "turn", TurnState::Completed, "turn.completed", None),
        )
        .unwrap();

        let first = sync_page(&connection, "session", 0, None, MAX_SYNC_PAGE).unwrap();
        assert!(!first.complete);
        assert!(first.events.len() < 7);
        assert!(serde_json::to_vec(&first).unwrap().len() as u64 <= MAX_AAP_FRAME_BYTES);
        let response = Response::success(
            json!("\\".repeat(128)),
            serde_json::to_value(&first).unwrap(),
        );
        assert!(serde_json::to_vec(&response).unwrap().len() as u64 <= MAX_AAP_FRAME_BYTES);
        let mut sequences = first
            .events
            .iter()
            .map(|event| event.sequence)
            .collect::<Vec<_>>();
        let mut after = first.next_after_sequence.unwrap();
        while after < first.watermark.sequence {
            let page = sync_page(
                &connection,
                "session",
                after,
                Some(&first.watermark),
                MAX_SYNC_PAGE,
            )
            .unwrap();
            sequences.extend(page.events.iter().map(|event| event.sequence));
            after = page.next_after_sequence.unwrap_or_else(|| {
                page.events
                    .last()
                    .map(|event| event.sequence)
                    .unwrap_or(after)
            });
            if page.complete {
                break;
            }
        }
        assert_eq!(sequences, (1..=7).collect::<Vec<_>>());

        let mut oversized = completed_item_event(
            8,
            17,
            "turn-two",
            "oversized",
            MAX_SYNC_EVENTS_BYTES as usize,
        );
        oversized.event = "item.completed".into();
        assert_eq!(
            append(&mut connection, &oversized).unwrap_err().code,
            "timeline-journal-event-too-large-for-sync"
        );
        let watermark = read_current_watermark(&connection, "session").unwrap();
        assert_eq!(watermark.sequence, 7);
    }
}
