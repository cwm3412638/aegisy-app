use crate::event_sequencer::{EventSequencer, EventSequencerCheckpoint};
use aegisy_aap::stable::v0_1::EventEnvelope;
use aegisy_aap::MAX_AAP_FRAME_BYTES;
use rusqlite::{params, Connection, OptionalExtension, Transaction};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

pub const JOURNAL_SCHEMA_VERSION: &str = "public-timeline-journal/0.1";
pub const CHECKPOINT_SCHEMA_VERSION: &str = "public-timeline-checkpoint/0.1";
pub const SYNC_SCHEMA_VERSION: &str = "timeline-sync-page/0.1";
pub const MAX_SYNC_PAGE: usize = 200;
pub const MAX_SESSION_EVENTS: u64 = 100_000;
pub const MAX_JOURNAL_SESSIONS: u64 = 10_000;
pub const MAX_TOTAL_EVENTS: u64 = 100_000;
pub const MAX_CHECKPOINT_BYTES: u64 = crate::event_sequencer::MAX_CHECKPOINT_BYTES as u64;
pub const MAX_TOTAL_CHECKPOINT_BYTES: u64 = 64 * 1024 * 1024;
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
        FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE RESTRICT
    ) STRICT;
    CREATE INDEX IF NOT EXISTS public_timeline_events_turn_idx
        ON public_timeline_events(session_id, turn_id, sequence);
    CREATE TABLE IF NOT EXISTS public_timeline_cursors (
        session_id TEXT PRIMARY KEY,
        next_sequence INTEGER NOT NULL CHECK(next_sequence >= 1 AND next_sequence <= 9007199254740991),
        last_timestamp_ms INTEGER NOT NULL CHECK(last_timestamp_ms >= 0 AND last_timestamp_ms <= 9007199254740991),
        latest_event_id TEXT,
        pruned_through_sequence INTEGER NOT NULL DEFAULT 0 CHECK(pruned_through_sequence >= 0 AND pruned_through_sequence <= 9007199254740991),
        pruned_through_event_id TEXT,
        pruned_through_timestamp_ms INTEGER NOT NULL DEFAULT 0 CHECK(pruned_through_timestamp_ms >= 0 AND pruned_through_timestamp_ms <= 9007199254740991),
        CHECK(
            (next_sequence = 1 AND last_timestamp_ms = 0 AND latest_event_id IS NULL)
            OR
            (next_sequence > 1 AND last_timestamp_ms >= 1 AND latest_event_id IS NOT NULL)
        ),
        CHECK(
            (pruned_through_sequence = 0 AND pruned_through_event_id IS NULL AND pruned_through_timestamp_ms = 0)
            OR
            (pruned_through_sequence > 0 AND pruned_through_event_id IS NOT NULL AND pruned_through_timestamp_ms > 0)
        ),
        CHECK(pruned_through_sequence < next_sequence),
        FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE RESTRICT
    ) STRICT;
    CREATE TABLE IF NOT EXISTS public_timeline_checkpoints (
        session_id TEXT PRIMARY KEY,
        schema_version TEXT NOT NULL CHECK(schema_version = 'public-timeline-checkpoint/0.1'),
        through_sequence INTEGER NOT NULL CHECK(through_sequence >= 0 AND through_sequence <= 9007199254740991),
        through_event_id TEXT,
        through_timestamp_ms INTEGER NOT NULL CHECK(through_timestamp_ms >= 0 AND through_timestamp_ms <= 9007199254740991),
        checkpoint_json TEXT,
        checkpoint_identity TEXT,
        checkpoint_bytes INTEGER NOT NULL CHECK(checkpoint_bytes >= 0 AND checkpoint_bytes <= 16777216),
        turn_count INTEGER NOT NULL CHECK(turn_count >= 0 AND turn_count <= 100000),
        item_count INTEGER NOT NULL CHECK(item_count >= 0 AND item_count <= 100000),
        created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0 AND created_at_ms <= 9007199254740991),
        CHECK(
            (through_sequence = 0 AND through_event_id IS NULL AND through_timestamp_ms = 0
                AND checkpoint_json IS NULL AND checkpoint_identity IS NULL
                AND checkpoint_bytes = 0 AND turn_count = 0 AND item_count = 0
                AND created_at_ms = 0)
            OR
            (through_sequence > 0 AND through_event_id IS NOT NULL AND through_timestamp_ms > 0
                AND checkpoint_json IS NOT NULL AND checkpoint_identity IS NOT NULL
                AND checkpoint_bytes > 0 AND created_at_ms > 0)
        ),
        FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE RESTRICT
    ) STRICT;
    CREATE TRIGGER IF NOT EXISTS public_timeline_session_cursor_insert
    AFTER INSERT ON sessions
    BEGIN
        INSERT OR IGNORE INTO public_timeline_cursors (
            session_id, next_sequence, last_timestamp_ms, latest_event_id,
            pruned_through_sequence, pruned_through_event_id,
            pruned_through_timestamp_ms
        ) VALUES (NEW.session_id, 1, 0, NULL, 0, NULL, 0);
        INSERT OR IGNORE INTO public_timeline_checkpoints (
            session_id, schema_version, through_sequence, through_event_id,
            through_timestamp_ms, checkpoint_json, checkpoint_identity,
            checkpoint_bytes, turn_count, item_count, created_at_ms
        ) VALUES (
            NEW.session_id, 'public-timeline-checkpoint/0.1', 0, NULL,
            0, NULL, NULL, 0, 0, 0, 0
        );
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
pub struct TimelineAnchor {
    pub sequence: u64,
    pub event_id: Option<String>,
    pub timestamp_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct TimelineRetentionState {
    pub session_id: String,
    pub head: TimelineAnchor,
    pub floor: TimelineAnchor,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct TimelinePruneResult {
    pub session_id: String,
    pub previous_floor: TimelineAnchor,
    pub floor: TimelineAnchor,
    pub head: TimelineAnchor,
    pub deleted_events: u64,
    pub checkpoint_identity: Option<String>,
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

#[derive(Debug, Clone)]
struct CursorState {
    next_sequence: u64,
    last_timestamp_ms: u64,
    latest_event_id: Option<String>,
    pruned_through_sequence: u64,
    pruned_through_event_id: Option<String>,
    pruned_through_timestamp_ms: u64,
}

impl CursorState {
    #[allow(dead_code)]
    fn head(&self) -> TimelineAnchor {
        TimelineAnchor {
            sequence: self.next_sequence - 1,
            event_id: self.latest_event_id.clone(),
            timestamp_ms: self.last_timestamp_ms,
        }
    }

    #[allow(dead_code)]
    fn floor(&self) -> TimelineAnchor {
        TimelineAnchor {
            sequence: self.pruned_through_sequence,
            event_id: self.pruned_through_event_id.clone(),
            timestamp_ms: self.pruned_through_timestamp_ms,
        }
    }
}

pub fn apply_schema(connection: &Connection) -> Result<(), JournalError> {
    connection
        .execute_batch(SCHEMA_SQL)
        .map_err(|_| JournalError::new("timeline-journal-schema-failed"))?;
    connection
        .execute(
            "INSERT OR IGNORE INTO public_timeline_cursors (
                session_id, next_sequence, last_timestamp_ms, latest_event_id,
                pruned_through_sequence, pruned_through_event_id,
                pruned_through_timestamp_ms
             ) SELECT session_id, 1, 0, NULL, 0, NULL, 0 FROM sessions",
            [],
        )
        .map_err(|_| JournalError::new("timeline-journal-cursor-backfill-failed"))?;
    connection
        .execute(
            "INSERT OR IGNORE INTO public_timeline_checkpoints (
                session_id, schema_version, through_sequence, through_event_id,
                through_timestamp_ms, checkpoint_json, checkpoint_identity,
                checkpoint_bytes, turn_count, item_count, created_at_ms
             ) SELECT session_id, 'public-timeline-checkpoint/0.1', 0, NULL,
                      0, NULL, NULL, 0, 0, 0, 0 FROM sessions",
            [],
        )
        .map_err(|_| JournalError::new("timeline-journal-checkpoint-backfill-failed"))?;
    Ok(())
}

pub(crate) fn migrate_v15_to_v16(transaction: &Transaction<'_>) -> Result<(), JournalError> {
    transaction
        .execute_batch(
            "DROP TRIGGER IF EXISTS public_timeline_session_cursor_insert;
             DROP INDEX IF EXISTS public_timeline_events_turn_idx;
             ALTER TABLE public_timeline_events RENAME TO public_timeline_events_v15;
             ALTER TABLE public_timeline_cursors RENAME TO public_timeline_cursors_v15;
             CREATE TABLE public_timeline_cursors (
                session_id TEXT PRIMARY KEY,
                next_sequence INTEGER NOT NULL CHECK(next_sequence >= 1 AND next_sequence <= 9007199254740991),
                last_timestamp_ms INTEGER NOT NULL CHECK(last_timestamp_ms >= 0 AND last_timestamp_ms <= 9007199254740991),
                latest_event_id TEXT,
                pruned_through_sequence INTEGER NOT NULL DEFAULT 0 CHECK(pruned_through_sequence >= 0 AND pruned_through_sequence <= 9007199254740991),
                pruned_through_event_id TEXT,
                pruned_through_timestamp_ms INTEGER NOT NULL DEFAULT 0 CHECK(pruned_through_timestamp_ms >= 0 AND pruned_through_timestamp_ms <= 9007199254740991),
                CHECK(
                    (next_sequence = 1 AND last_timestamp_ms = 0 AND latest_event_id IS NULL)
                    OR
                    (next_sequence > 1 AND last_timestamp_ms >= 1 AND latest_event_id IS NOT NULL)
                ),
                CHECK(
                    (pruned_through_sequence = 0 AND pruned_through_event_id IS NULL AND pruned_through_timestamp_ms = 0)
                    OR
                    (pruned_through_sequence > 0 AND pruned_through_event_id IS NOT NULL AND pruned_through_timestamp_ms > 0)
                ),
                CHECK(pruned_through_sequence < next_sequence),
                FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE RESTRICT
             ) STRICT;
             INSERT INTO public_timeline_cursors (
                session_id, next_sequence, last_timestamp_ms, latest_event_id,
                pruned_through_sequence, pruned_through_event_id,
                pruned_through_timestamp_ms
             ) SELECT session_id, next_sequence, last_timestamp_ms, latest_event_id,
                      0, NULL, 0 FROM public_timeline_cursors_v15;
             DROP TABLE public_timeline_cursors_v15;",
        )
        .map_err(|_| JournalError::new("timeline-journal-v16-cursor-migration-failed"))?;
    transaction
        .execute_batch(SCHEMA_SQL)
        .map_err(|_| JournalError::new("timeline-journal-v16-schema-migration-failed"))?;
    transaction
        .execute(
            "INSERT INTO public_timeline_events (
                session_id, sequence, event_id, timestamp_ms, turn_id,
                envelope_json, envelope_sha256, envelope_bytes
             ) SELECT session_id, sequence, event_id, timestamp_ms, turn_id,
                      envelope_json, envelope_sha256, envelope_bytes
               FROM public_timeline_events_v15",
            [],
        )
        .map_err(|_| JournalError::new("timeline-journal-v16-event-migration-failed"))?;
    transaction
        .execute(
            "INSERT INTO public_timeline_checkpoints (
                session_id, schema_version, through_sequence, through_event_id,
                through_timestamp_ms, checkpoint_json, checkpoint_identity,
                checkpoint_bytes, turn_count, item_count, created_at_ms
             ) SELECT session_id, 'public-timeline-checkpoint/0.1', 0, NULL,
                      0, NULL, NULL, 0, 0, 0, 0 FROM sessions",
            [],
        )
        .map_err(|_| JournalError::new("timeline-journal-v16-checkpoint-migration-failed"))?;
    transaction
        .execute_batch("DROP TABLE public_timeline_events_v15;")
        .map_err(|_| JournalError::new("timeline-journal-v16-event-cleanup-failed"))?;
    Ok(())
}

pub fn append_event_tx(
    transaction: &Transaction<'_>,
    event: &EventEnvelope,
) -> Result<(), JournalError> {
    event
        .validate()
        .map_err(|_| JournalError::new("timeline-journal-event-invalid"))?;
    let cursor = read_cursor(transaction, &event.session_id)?;
    validate_cursor_anchor(transaction, &event.session_id, &cursor)?;
    let retained_events = cursor
        .next_sequence
        .checked_sub(cursor.pruned_through_sequence)
        .and_then(|value| value.checked_sub(1))
        .ok_or_else(|| JournalError::new("timeline-journal-cursor-invalid"))?;
    if retained_events >= MAX_SESSION_EVENTS {
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
    if event.sequence != cursor.next_sequence {
        return Err(JournalError::new("timeline-journal-sequence-gap"));
    }
    if event.timestamp_ms < cursor.last_timestamp_ms {
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
    let following_sequence = cursor
        .next_sequence
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
                i64::try_from(cursor.next_sequence)
                    .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?,
                i64::try_from(cursor.last_timestamp_ms)
                    .map_err(|_| JournalError::new("timeline-journal-timestamp-invalid"))?,
                cursor.latest_event_id,
            ],
        )
        .map_err(|_| JournalError::new("timeline-journal-cursor-write-failed"))?;
    if cursor_changed != 1 {
        return Err(JournalError::new("timeline-journal-cursor-write-conflict"));
    }
    Ok(())
}

fn read_cursor(connection: &Connection, session_id: &str) -> Result<CursorState, JournalError> {
    let row = connection
        .query_row(
            "SELECT next_sequence, last_timestamp_ms, latest_event_id,
                    pruned_through_sequence, pruned_through_event_id,
                    pruned_through_timestamp_ms
             FROM public_timeline_cursors WHERE session_id = ?1",
            [session_id],
            |row| {
                Ok((
                    row.get::<_, i64>(0)?,
                    row.get::<_, i64>(1)?,
                    row.get::<_, Option<String>>(2)?,
                    row.get::<_, i64>(3)?,
                    row.get::<_, Option<String>>(4)?,
                    row.get::<_, i64>(5)?,
                ))
            },
        )
        .optional()
        .map_err(|_| JournalError::new("timeline-journal-cursor-read-failed"))?
        .ok_or_else(|| JournalError::new("timeline-journal-cursor-missing"))?;
    let cursor = CursorState {
        next_sequence: u64::try_from(row.0)
            .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?,
        last_timestamp_ms: u64::try_from(row.1)
            .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?,
        latest_event_id: row.2,
        pruned_through_sequence: u64::try_from(row.3)
            .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?,
        pruned_through_event_id: row.4,
        pruned_through_timestamp_ms: u64::try_from(row.5)
            .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?,
    };
    validate_cursor_state(&cursor)?;
    Ok(cursor)
}

fn validate_cursor_state(cursor: &CursorState) -> Result<(), JournalError> {
    let head_empty = cursor.next_sequence == 1
        && cursor.last_timestamp_ms == 0
        && cursor.latest_event_id.is_none();
    let head_populated = cursor.next_sequence > 1
        && cursor.last_timestamp_ms > 0
        && cursor
            .latest_event_id
            .as_deref()
            .is_some_and(valid_event_id);
    let floor_empty = cursor.pruned_through_sequence == 0
        && cursor.pruned_through_timestamp_ms == 0
        && cursor.pruned_through_event_id.is_none();
    let floor_populated = cursor.pruned_through_sequence > 0
        && cursor.pruned_through_timestamp_ms > 0
        && cursor
            .pruned_through_event_id
            .as_deref()
            .is_some_and(valid_event_id);
    if (head_empty || head_populated)
        && (floor_empty || floor_populated)
        && cursor.pruned_through_sequence < cursor.next_sequence
        && cursor.pruned_through_timestamp_ms <= cursor.last_timestamp_ms
    {
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
    cursor: &CursorState,
) -> Result<(), JournalError> {
    validate_checkpoint(connection, session_id, cursor)?;
    let head_sequence = cursor.next_sequence - 1;
    let anchor = if head_sequence == cursor.pruned_through_sequence {
        Some((
            cursor.pruned_through_event_id.clone(),
            cursor.pruned_through_timestamp_ms,
        ))
    } else {
        connection
            .query_row(
                "SELECT event_id, timestamp_ms FROM public_timeline_events
                 WHERE session_id = ?1 AND sequence = ?2",
                params![
                    session_id,
                    i64::try_from(head_sequence)
                        .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?
                ],
                |row| {
                    Ok((
                        Some(row.get::<_, String>(0)?),
                        u64::try_from(row.get::<_, i64>(1)?)
                            .map_err(|_| rusqlite::Error::IntegralValueOutOfRange(1, 0))?,
                    ))
                },
            )
            .optional()
            .map_err(|_| JournalError::new("timeline-journal-cursor-read-failed"))?
    };
    let anchor_matches = match anchor {
        None => false,
        Some((event_id, timestamp_ms)) => {
            timestamp_ms == cursor.last_timestamp_ms && event_id == cursor.latest_event_id
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
                i64::try_from(cursor.next_sequence)
                    .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?
            ],
            |row| row.get(0),
        )
        .map_err(|_| JournalError::new("timeline-journal-cursor-read-failed"))?;
    let retained: (i64, Option<i64>, Option<i64>) = connection
        .query_row(
            "SELECT COUNT(*), MIN(sequence), MAX(sequence)
             FROM public_timeline_events WHERE session_id = ?1",
            [session_id],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
        )
        .map_err(|_| JournalError::new("timeline-journal-cursor-read-failed"))?;
    let expected_count = head_sequence
        .checked_sub(cursor.pruned_through_sequence)
        .ok_or_else(|| JournalError::new("timeline-journal-cursor-invalid"))?;
    let count = u64::try_from(retained.0)
        .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?;
    let range_matches = if expected_count == 0 {
        count == 0 && retained.1.is_none() && retained.2.is_none()
    } else {
        count == expected_count
            && retained.1.and_then(|value| u64::try_from(value).ok())
                == Some(cursor.pruned_through_sequence + 1)
            && retained.2.and_then(|value| u64::try_from(value).ok()) == Some(head_sequence)
    };
    if anchor_matches && !events_at_or_after_cursor && range_matches {
        Ok(())
    } else {
        Err(JournalError::new("timeline-journal-cursor-mismatch"))
    }
}

fn validate_checkpoint(
    connection: &Connection,
    session_id: &str,
    cursor: &CursorState,
) -> Result<Option<EventSequencerCheckpoint>, JournalError> {
    let row = connection
        .query_row(
            "SELECT schema_version, through_sequence, through_event_id,
                    through_timestamp_ms, checkpoint_json, checkpoint_identity,
                    checkpoint_bytes, turn_count, item_count, created_at_ms
             FROM public_timeline_checkpoints WHERE session_id = ?1",
            [session_id],
            |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    row.get::<_, i64>(1)?,
                    row.get::<_, Option<String>>(2)?,
                    row.get::<_, i64>(3)?,
                    row.get::<_, Option<String>>(4)?,
                    row.get::<_, Option<String>>(5)?,
                    row.get::<_, i64>(6)?,
                    row.get::<_, i64>(7)?,
                    row.get::<_, i64>(8)?,
                    row.get::<_, i64>(9)?,
                ))
            },
        )
        .optional()
        .map_err(|_| JournalError::new("timeline-journal-checkpoint-read-failed"))?
        .ok_or_else(|| JournalError::new("timeline-journal-checkpoint-missing"))?;
    let through_sequence = u64::try_from(row.1)
        .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?;
    let through_timestamp_ms = u64::try_from(row.3)
        .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?;
    let checkpoint_bytes = u64::try_from(row.6)
        .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?;
    let turn_count = u64::try_from(row.7)
        .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?;
    let item_count = u64::try_from(row.8)
        .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?;
    let created_at_ms = u64::try_from(row.9)
        .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?;
    if row.0 != CHECKPOINT_SCHEMA_VERSION
        || through_sequence != cursor.pruned_through_sequence
        || row.2 != cursor.pruned_through_event_id
        || through_timestamp_ms != cursor.pruned_through_timestamp_ms
    {
        return Err(JournalError::new(
            "timeline-journal-checkpoint-anchor-mismatch",
        ));
    }
    if through_sequence == 0 {
        if row.4.is_none()
            && row.5.is_none()
            && checkpoint_bytes == 0
            && turn_count == 0
            && item_count == 0
            && created_at_ms == 0
        {
            return Ok(None);
        }
        return Err(JournalError::new("timeline-journal-checkpoint-invalid"));
    }
    let json = row
        .4
        .ok_or_else(|| JournalError::new("timeline-journal-checkpoint-invalid"))?;
    let identity = row
        .5
        .ok_or_else(|| JournalError::new("timeline-journal-checkpoint-invalid"))?;
    if checkpoint_bytes == 0
        || checkpoint_bytes > MAX_CHECKPOINT_BYTES
        || checkpoint_bytes != u64::try_from(json.len()).unwrap_or(u64::MAX)
        || created_at_ms == 0
    {
        return Err(JournalError::new("timeline-journal-checkpoint-invalid"));
    }
    let checkpoint = EventSequencerCheckpoint::from_canonical_json(session_id, &json)
        .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?;
    if checkpoint.sequence() != through_sequence
        || checkpoint.timestamp_ms() != through_timestamp_ms
        || checkpoint.event_id() != row.2.as_deref()
        || checkpoint.identity() != identity
        || checkpoint.turn_count() != turn_count
        || checkpoint.item_count() != item_count
        || checkpoint.to_canonical_json() != json
    {
        return Err(JournalError::new("timeline-journal-checkpoint-invalid"));
    }
    Ok(Some(checkpoint))
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

fn anchor_event_id(
    connection: &Connection,
    session_id: &str,
    sequence: u64,
    cursor: &CursorState,
) -> Result<Option<String>, JournalError> {
    if sequence == 0 {
        return Ok(None);
    }
    if sequence == cursor.pruned_through_sequence {
        return Ok(cursor.pruned_through_event_id.clone());
    }
    connection
        .query_row(
            "SELECT event_id FROM public_timeline_events
             WHERE session_id = ?1 AND sequence = ?2",
            params![
                session_id,
                i64::try_from(sequence)
                    .map_err(|_| JournalError::new("timeline-sync-request-invalid"))?
            ],
            |row| row.get::<_, String>(0),
        )
        .optional()
        .map_err(|_| JournalError::new("timeline-sync-anchor-read-failed"))
}

#[cfg(test)]
fn read_current_watermark(
    connection: &Connection,
    session_id: &str,
) -> Result<TimelineWatermark, JournalError> {
    let cursor = read_cursor(connection, session_id)?;
    validate_cursor_anchor(connection, session_id, &cursor)?;
    Ok(TimelineWatermark {
        sequence: cursor.next_sequence - 1,
        event_id: cursor.latest_event_id,
    })
}

pub fn sync_page(
    connection: &Connection,
    session_id: &str,
    after_sequence: u64,
    requested_watermark: Option<&TimelineWatermark>,
    limit: usize,
) -> Result<TimelineSyncPage, JournalError> {
    sync_page_with_ownership(
        connection,
        session_id,
        after_sequence,
        requested_watermark,
        limit,
        true,
    )
}

fn sync_page_with_ownership(
    connection: &Connection,
    session_id: &str,
    after_sequence: u64,
    requested_watermark: Option<&TimelineWatermark>,
    limit: usize,
    require_session_ownership: bool,
) -> Result<TimelineSyncPage, JournalError> {
    if session_id.is_empty() || session_id.len() > 128 || limit == 0 || limit > MAX_SYNC_PAGE {
        return Err(JournalError::new("timeline-sync-request-invalid"));
    }
    if require_session_ownership {
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
    }
    let cursor = read_cursor(connection, session_id)?;
    validate_cursor_anchor(connection, session_id, &cursor)?;
    let current_watermark = TimelineWatermark {
        sequence: cursor.next_sequence - 1,
        event_id: cursor.latest_event_id.clone(),
    };
    let watermark = requested_watermark
        .cloned()
        .unwrap_or_else(|| current_watermark.clone());
    validate_watermark(&watermark)?;
    if watermark.sequence > current_watermark.sequence || after_sequence > watermark.sequence {
        return Err(JournalError::new("timeline-sync-watermark-invalid"));
    }
    if after_sequence < cursor.pruned_through_sequence
        || watermark.sequence < cursor.pruned_through_sequence
    {
        return Err(JournalError::new("timeline-sync-retention-gap"));
    }
    if watermark.sequence > 0 {
        let stored_event_id = anchor_event_id(connection, session_id, watermark.sequence, &cursor)?;
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
        return Err(JournalError::new("timeline-sync-integrity-failed"));
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

pub fn sync_page_from_anchor(
    connection: &Connection,
    session_id: &str,
    after: &TimelineWatermark,
    requested_watermark: Option<&TimelineWatermark>,
    limit: usize,
) -> Result<TimelineSyncPage, JournalError> {
    validate_watermark(after)?;
    let cursor = read_cursor(connection, session_id)?;
    validate_cursor_anchor(connection, session_id, &cursor)?;
    if after.sequence < cursor.pruned_through_sequence {
        return Err(JournalError::new("timeline-sync-retention-gap"));
    }
    if after.sequence > 0 {
        let stored_event_id = anchor_event_id(connection, session_id, after.sequence, &cursor)?;
        if stored_event_id.as_deref() != after.event_id.as_deref() {
            return Err(JournalError::new("timeline-sync-anchor-drift"));
        }
    }
    sync_page(
        connection,
        session_id,
        after.sequence,
        requested_watermark,
        limit,
    )
}

#[cfg(test)]
pub(crate) fn read_retention_state(
    connection: &Connection,
    session_id: &str,
) -> Result<TimelineRetentionState, JournalError> {
    let cursor = read_cursor(connection, session_id)?;
    validate_cursor_anchor(connection, session_id, &cursor)?;
    Ok(TimelineRetentionState {
        session_id: session_id.to_owned(),
        head: cursor.head(),
        floor: cursor.floor(),
    })
}

#[allow(dead_code)]
pub(crate) fn checkpoint_and_prune_tx(
    transaction: &Transaction<'_>,
    session_id: &str,
    through: &TimelineWatermark,
    created_at_ms: u64,
) -> Result<TimelinePruneResult, JournalError> {
    if session_id.is_empty()
        || session_id.len() > 128
        || created_at_ms == 0
        || created_at_ms > aegisy_aap::MAX_SAFE_JSON_INTEGER
    {
        return Err(JournalError::new("timeline-journal-prune-request-invalid"));
    }
    validate_watermark(through)?;
    let cursor = read_cursor(transaction, session_id)?;
    validate_cursor_anchor(transaction, session_id, &cursor)?;
    let previous_checkpoint = validate_checkpoint(transaction, session_id, &cursor)?;
    let previous_floor = cursor.floor();
    let head = cursor.head();
    if through.sequence <= previous_floor.sequence || through.sequence > head.sequence {
        return Err(JournalError::new("timeline-journal-prune-boundary-invalid"));
    }
    let boundary_event_id = anchor_event_id(transaction, session_id, through.sequence, &cursor)?;
    if boundary_event_id.as_deref() != through.event_id.as_deref() {
        return Err(JournalError::new("timeline-journal-prune-anchor-drift"));
    }

    let mut candidate = match previous_checkpoint.as_ref() {
        Some(checkpoint) => EventSequencer::begin_session_restore_from_checkpoint(
            session_id,
            checkpoint,
            head.sequence,
            head.event_id.as_deref(),
        ),
        None => EventSequencer::begin_session_restore(
            session_id,
            head.sequence,
            head.event_id.as_deref(),
        ),
    }
    .map_err(|_| JournalError::new("timeline-journal-lifecycle-invalid"))?;
    let mut after = previous_floor.sequence;
    let mut next_checkpoint = None;
    while after < head.sequence {
        let page = sync_page(
            transaction,
            session_id,
            after,
            Some(&TimelineWatermark {
                sequence: head.sequence,
                event_id: head.event_id.clone(),
            }),
            MAX_SYNC_PAGE,
        )?;
        if page.events.is_empty() {
            return Err(JournalError::new("timeline-journal-verify-incomplete"));
        }
        for event in &page.events {
            candidate = candidate
                .replay_event(event)
                .map_err(|_| JournalError::new("timeline-journal-lifecycle-invalid"))?;
            if event.sequence == through.sequence {
                next_checkpoint = Some(
                    candidate
                        .checkpoint_at_current()
                        .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?,
                );
            }
        }
        after = page
            .events
            .last()
            .map(|event| event.sequence)
            .ok_or_else(|| JournalError::new("timeline-journal-verify-incomplete"))?;
    }
    candidate
        .complete()
        .map_err(|_| JournalError::new("timeline-journal-lifecycle-invalid"))?;
    let checkpoint = next_checkpoint
        .ok_or_else(|| JournalError::new("timeline-journal-prune-boundary-invalid"))?;
    if checkpoint.sequence() != through.sequence
        || checkpoint.event_id() != through.event_id.as_deref()
    {
        return Err(JournalError::new(
            "timeline-journal-checkpoint-anchor-mismatch",
        ));
    }
    let checkpoint_json = checkpoint.to_canonical_json();
    let checkpoint_bytes = u64::try_from(checkpoint_json.len())
        .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?;
    if checkpoint_bytes == 0 || checkpoint_bytes > MAX_CHECKPOINT_BYTES {
        return Err(JournalError::new("timeline-journal-checkpoint-limit"));
    }
    let previous_checkpoint_bytes: i64 = transaction
        .query_row(
            "SELECT checkpoint_bytes FROM public_timeline_checkpoints WHERE session_id = ?1",
            [session_id],
            |row| row.get(0),
        )
        .map_err(|_| JournalError::new("timeline-journal-checkpoint-read-failed"))?;
    let total_checkpoint_bytes: i64 = transaction
        .query_row(
            "SELECT COALESCE(SUM(checkpoint_bytes), 0) FROM public_timeline_checkpoints",
            [],
            |row| row.get(0),
        )
        .map_err(|_| JournalError::new("timeline-journal-checkpoint-read-failed"))?;
    let total_checkpoint_bytes = u64::try_from(total_checkpoint_bytes)
        .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?;
    let previous_checkpoint_bytes = u64::try_from(previous_checkpoint_bytes)
        .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?;
    let next_total_checkpoint_bytes = total_checkpoint_bytes
        .checked_sub(previous_checkpoint_bytes)
        .and_then(|bytes| bytes.checked_add(checkpoint_bytes))
        .ok_or_else(|| JournalError::new("timeline-journal-checkpoint-limit"))?;
    if next_total_checkpoint_bytes > MAX_TOTAL_CHECKPOINT_BYTES {
        return Err(JournalError::new("timeline-journal-checkpoint-limit"));
    }

    let old_identity = previous_checkpoint
        .as_ref()
        .map(|checkpoint| checkpoint.identity().to_owned());
    let checkpoint_changed = transaction
        .execute(
            "UPDATE public_timeline_checkpoints
             SET through_sequence = ?2, through_event_id = ?3,
                 through_timestamp_ms = ?4, checkpoint_json = ?5,
                 checkpoint_identity = ?6, checkpoint_bytes = ?7,
                 turn_count = ?8, item_count = ?9, created_at_ms = ?10
             WHERE session_id = ?1 AND through_sequence = ?11
               AND checkpoint_identity IS ?12",
            params![
                session_id,
                i64::try_from(checkpoint.sequence())
                    .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?,
                checkpoint.event_id(),
                i64::try_from(checkpoint.timestamp_ms())
                    .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?,
                checkpoint_json,
                checkpoint.identity(),
                i64::try_from(checkpoint_bytes)
                    .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?,
                i64::try_from(checkpoint.turn_count())
                    .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?,
                i64::try_from(checkpoint.item_count())
                    .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?,
                i64::try_from(created_at_ms)
                    .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?,
                i64::try_from(previous_floor.sequence)
                    .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?,
                old_identity,
            ],
        )
        .map_err(|_| JournalError::new("timeline-journal-checkpoint-write-failed"))?;
    if checkpoint_changed != 1 {
        return Err(JournalError::new(
            "timeline-journal-checkpoint-write-conflict",
        ));
    }

    let floor_changed = transaction
        .execute(
            "UPDATE public_timeline_cursors
             SET pruned_through_sequence = ?2, pruned_through_event_id = ?3,
                 pruned_through_timestamp_ms = ?4
             WHERE session_id = ?1 AND next_sequence = ?5
               AND last_timestamp_ms = ?6 AND latest_event_id IS ?7
               AND pruned_through_sequence = ?8
               AND pruned_through_event_id IS ?9
               AND pruned_through_timestamp_ms = ?10",
            params![
                session_id,
                i64::try_from(checkpoint.sequence())
                    .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?,
                checkpoint.event_id(),
                i64::try_from(checkpoint.timestamp_ms())
                    .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?,
                i64::try_from(cursor.next_sequence)
                    .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?,
                i64::try_from(cursor.last_timestamp_ms)
                    .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?,
                cursor.latest_event_id,
                i64::try_from(previous_floor.sequence)
                    .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?,
                previous_floor.event_id,
                i64::try_from(previous_floor.timestamp_ms)
                    .map_err(|_| JournalError::new("timeline-journal-cursor-invalid"))?,
            ],
        )
        .map_err(|_| JournalError::new("timeline-journal-floor-write-failed"))?;
    if floor_changed != 1 {
        return Err(JournalError::new("timeline-journal-floor-write-conflict"));
    }
    let deleted = transaction
        .execute(
            "DELETE FROM public_timeline_events
             WHERE session_id = ?1 AND sequence > ?2 AND sequence <= ?3",
            params![
                session_id,
                i64::try_from(previous_floor.sequence)
                    .map_err(|_| JournalError::new("timeline-journal-prune-boundary-invalid"))?,
                i64::try_from(through.sequence)
                    .map_err(|_| JournalError::new("timeline-journal-prune-boundary-invalid"))?,
            ],
        )
        .map_err(|_| JournalError::new("timeline-journal-prune-delete-failed"))?;
    let expected_deleted = through
        .sequence
        .checked_sub(previous_floor.sequence)
        .ok_or_else(|| JournalError::new("timeline-journal-prune-boundary-invalid"))?;
    if u64::try_from(deleted).ok() != Some(expected_deleted) {
        return Err(JournalError::new("timeline-journal-prune-delete-mismatch"));
    }
    let updated = read_cursor(transaction, session_id)?;
    validate_cursor_anchor(transaction, session_id, &updated)?;
    Ok(TimelinePruneResult {
        session_id: session_id.to_owned(),
        previous_floor,
        floor: updated.floor(),
        head: updated.head(),
        deleted_events: expected_deleted,
        checkpoint_identity: Some(checkpoint.identity().to_owned()),
    })
}

pub(crate) fn reset_session_tx(
    transaction: &Transaction<'_>,
    session_id: &str,
) -> Result<(), JournalError> {
    let cursor = read_cursor(transaction, session_id)?;
    validate_cursor_anchor(transaction, session_id, &cursor)?;
    transaction
        .execute(
            "DELETE FROM public_timeline_events WHERE session_id = ?1",
            [session_id],
        )
        .map_err(|_| JournalError::new("timeline-journal-reset-events-failed"))?;
    let checkpoint_reset = transaction
        .execute(
            "UPDATE public_timeline_checkpoints
             SET through_sequence = 0, through_event_id = NULL,
                 through_timestamp_ms = 0, checkpoint_json = NULL,
                 checkpoint_identity = NULL, checkpoint_bytes = 0,
                 turn_count = 0, item_count = 0, created_at_ms = 0
             WHERE session_id = ?1",
            [session_id],
        )
        .map_err(|_| JournalError::new("timeline-journal-reset-checkpoint-failed"))?;
    if checkpoint_reset != 1 {
        return Err(JournalError::new("timeline-journal-checkpoint-missing"));
    }
    let cursor_reset = transaction
        .execute(
            "UPDATE public_timeline_cursors
             SET next_sequence = 1, last_timestamp_ms = 0, latest_event_id = NULL,
                 pruned_through_sequence = 0, pruned_through_event_id = NULL,
                 pruned_through_timestamp_ms = 0
             WHERE session_id = ?1",
            [session_id],
        )
        .map_err(|_| JournalError::new("timeline-journal-reset-cursor-failed"))?;
    if cursor_reset != 1 {
        return Err(JournalError::new("timeline-journal-cursor-missing"));
    }
    let reset = read_cursor(transaction, session_id)?;
    validate_cursor_anchor(transaction, session_id, &reset)
}

pub fn verify_all(connection: &Connection) -> Result<(), JournalError> {
    restore_all_with_ownership(connection, true).map(drop)
}

pub(crate) fn verify_before_projection_recovery(
    connection: &Connection,
) -> Result<(), JournalError> {
    restore_all_with_ownership(connection, false).map(drop)
}

pub(crate) fn restore_all(connection: &Connection) -> Result<EventSequencer, JournalError> {
    restore_all_with_ownership(connection, true)
}

fn restore_all_with_ownership(
    connection: &Connection,
    require_session_ownership: bool,
) -> Result<EventSequencer, JournalError> {
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
    let total_checkpoint_bytes: i64 = connection
        .query_row(
            "SELECT COALESCE(SUM(checkpoint_bytes), 0) FROM public_timeline_checkpoints",
            [],
            |row| row.get(0),
        )
        .map_err(|_| JournalError::new("timeline-journal-verify-query-failed"))?;
    if u64::try_from(total_checkpoint_bytes)
        .map_err(|_| JournalError::new("timeline-journal-checkpoint-invalid"))?
        > MAX_TOTAL_CHECKPOINT_BYTES
    {
        return Err(JournalError::new("timeline-journal-checkpoint-limit"));
    }
    let ownership_sql = if require_session_ownership {
        "SELECT EXISTS(
            SELECT 1 FROM sessions s
            LEFT JOIN public_timeline_cursors c ON c.session_id = s.session_id
            LEFT JOIN public_timeline_checkpoints p ON p.session_id = s.session_id
            WHERE c.session_id IS NULL OR p.session_id IS NULL
            UNION ALL
            SELECT 1 FROM public_timeline_cursors c
            LEFT JOIN sessions s ON s.session_id = c.session_id
            LEFT JOIN public_timeline_checkpoints p ON p.session_id = c.session_id
            WHERE s.session_id IS NULL OR p.session_id IS NULL
            UNION ALL
            SELECT 1 FROM public_timeline_checkpoints p
            LEFT JOIN sessions s ON s.session_id = p.session_id
            LEFT JOIN public_timeline_cursors c ON c.session_id = p.session_id
            WHERE s.session_id IS NULL OR c.session_id IS NULL
            UNION ALL
            SELECT 1 FROM public_timeline_events e
            LEFT JOIN sessions s ON s.session_id = e.session_id
            LEFT JOIN public_timeline_cursors c ON c.session_id = e.session_id
            WHERE s.session_id IS NULL OR c.session_id IS NULL
         )"
    } else {
        "SELECT EXISTS(
            SELECT 1 FROM sessions s
            LEFT JOIN public_timeline_cursors c ON c.session_id = s.session_id
            LEFT JOIN public_timeline_checkpoints p ON p.session_id = s.session_id
            WHERE c.session_id IS NULL OR p.session_id IS NULL
            UNION ALL
            SELECT 1 FROM public_timeline_cursors c
            LEFT JOIN public_timeline_checkpoints p ON p.session_id = c.session_id
            WHERE p.session_id IS NULL
            UNION ALL
            SELECT 1 FROM public_timeline_checkpoints p
            LEFT JOIN public_timeline_cursors c ON c.session_id = p.session_id
            WHERE c.session_id IS NULL
            UNION ALL
            SELECT 1 FROM public_timeline_events e
            LEFT JOIN public_timeline_cursors c ON c.session_id = e.session_id
            WHERE c.session_id IS NULL
         )"
    };
    let ownership_drift: bool = connection
        .query_row(ownership_sql, [], |row| row.get(0))
        .map_err(|_| JournalError::new("timeline-journal-verify-query-failed"))?;
    if ownership_drift {
        return Err(JournalError::new("timeline-journal-ownership-mismatch"));
    }
    let mut statement = connection
        .prepare(
            "SELECT session_id FROM public_timeline_cursors
             ORDER BY session_id LIMIT ?1",
        )
        .map_err(|_| JournalError::new("timeline-journal-verify-query-failed"))?;
    let session_limit = if require_session_ownership {
        MAX_JOURNAL_SESSIONS
    } else {
        MAX_JOURNAL_SESSIONS * 10
    };
    let rows = statement
        .query_map(
            [i64::try_from(session_limit + 1)
                .map_err(|_| JournalError::new("timeline-journal-session-limit"))?],
            |row| row.get::<_, String>(0),
        )
        .map_err(|_| JournalError::new("timeline-journal-verify-query-failed"))?;
    let mut sessions = Vec::new();
    for row in rows {
        sessions.push(row.map_err(|_| JournalError::new("timeline-journal-verify-row-invalid"))?);
    }
    drop(statement);
    if sessions.len() as u64 > session_limit {
        return Err(JournalError::new("timeline-journal-session-limit"));
    }
    let mut sequencer = EventSequencer::default();
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
        let cursor = read_cursor(connection, &session_id)?;
        validate_cursor_anchor(connection, &session_id, &cursor)?;
        let checkpoint = validate_checkpoint(connection, &session_id, &cursor)?;
        let watermark = TimelineWatermark {
            sequence: cursor.next_sequence - 1,
            event_id: cursor.latest_event_id.clone(),
        };
        let retained = watermark
            .sequence
            .checked_sub(cursor.pruned_through_sequence)
            .ok_or_else(|| JournalError::new("timeline-journal-cursor-invalid"))?;
        if count > MAX_SESSION_EVENTS || count != retained {
            return Err(JournalError::new("timeline-journal-cursor-mismatch"));
        }
        let mut after = cursor.pruned_through_sequence;
        let mut candidate = match checkpoint.as_ref() {
            Some(checkpoint) => EventSequencer::begin_session_restore_from_checkpoint(
                &session_id,
                checkpoint,
                watermark.sequence,
                watermark.event_id.as_deref(),
            ),
            None => EventSequencer::begin_session_restore(
                &session_id,
                watermark.sequence,
                watermark.event_id.as_deref(),
            ),
        }
        .map_err(|_| JournalError::new("timeline-journal-lifecycle-invalid"))?;
        while after < watermark.sequence {
            let page = sync_page_with_ownership(
                connection,
                &session_id,
                after,
                Some(&watermark),
                MAX_SYNC_PAGE,
                require_session_ownership,
            )?;
            for event in &page.events {
                candidate = candidate
                    .replay_event(event)
                    .map_err(|_| JournalError::new("timeline-journal-lifecycle-invalid"))?;
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
        let restored = candidate
            .complete()
            .map_err(|_| JournalError::new("timeline-journal-lifecycle-invalid"))?;
        sequencer.install_restored_session(restored);
    }
    Ok(sequencer)
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

    fn prune(
        connection: &mut Connection,
        through: &TimelineWatermark,
        created_at_ms: u64,
    ) -> Result<TimelinePruneResult, JournalError> {
        let transaction = connection
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .unwrap();
        let result = checkpoint_and_prune_tx(&transaction, "session", through, created_at_ms)?;
        transaction.commit().unwrap();
        Ok(result)
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
    fn session_projection_delete_cannot_cascade_public_timeline_authority() {
        let mut connection = test_connection();
        append(&mut connection, &event(1, 10, "turn")).unwrap();

        for table in [
            "public_timeline_events",
            "public_timeline_cursors",
            "public_timeline_checkpoints",
        ] {
            let on_delete: String = connection
                .query_row(&format!("PRAGMA foreign_key_list({table})"), [], |row| {
                    row.get(6)
                })
                .unwrap();
            assert_eq!(on_delete, "RESTRICT");
        }
        assert!(connection
            .execute("DELETE FROM sessions WHERE session_id = 'session'", [])
            .is_err());
        assert_eq!(
            connection
                .query_row("SELECT COUNT(*) FROM public_timeline_events", [], |row| {
                    row.get::<_, i64>(0)
                })
                .unwrap(),
            1
        );
        verify_all(&connection).unwrap();
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

    #[test]
    fn checkpoint_prunes_prefix_restores_tail_and_continues_sequence() {
        let mut connection = test_connection();
        let events = [
            envelope(1, 10, "turn-one", TurnState::Running, "turn.started", None),
            completed_item_event(2, 11, "turn-one", "item-one", 0),
            envelope(
                3,
                12,
                "turn-one",
                TurnState::Completed,
                "turn.completed",
                None,
            ),
            envelope(4, 13, "turn-two", TurnState::Running, "turn.started", None),
            envelope(
                5,
                14,
                "turn-two",
                TurnState::Completed,
                "turn.completed",
                None,
            ),
        ];
        for candidate in &events {
            append(&mut connection, candidate).unwrap();
        }
        let boundary = TimelineWatermark {
            sequence: 3,
            event_id: Some(events[2].event_id.clone()),
        };
        let result = prune(&mut connection, &boundary, 20).unwrap();
        assert_eq!(result.previous_floor.sequence, 0);
        assert_eq!(result.floor.sequence, 3);
        assert_eq!(result.head.sequence, 5);
        assert_eq!(result.deleted_events, 3);
        assert!(result.checkpoint_identity.is_some());
        assert_eq!(
            connection
                .query_row("SELECT COUNT(*) FROM public_timeline_events", [], |row| row
                    .get::<_, i64>(0),)
                .unwrap(),
            2
        );
        let state = read_retention_state(&connection, "session").unwrap();
        assert_eq!(state.floor.sequence, 3);
        assert_eq!(state.floor.event_id, boundary.event_id);
        assert_eq!(state.floor.timestamp_ms, 12);

        assert_eq!(
            sync_page(&connection, "session", 0, None, 10)
                .unwrap_err()
                .code,
            "timeline-sync-retention-gap"
        );
        let page = sync_page_from_anchor(&connection, "session", &boundary, None, 10).unwrap();
        assert_eq!(
            page.events
                .iter()
                .map(|event| event.sequence)
                .collect::<Vec<_>>(),
            vec![4, 5]
        );
        let forged = TimelineWatermark {
            sequence: 3,
            event_id: Some(format!("event:sha256:{}", "0".repeat(64))),
        };
        assert_eq!(
            sync_page_from_anchor(&connection, "session", &forged, None, 10)
                .unwrap_err()
                .code,
            "timeline-sync-anchor-drift"
        );

        let mut restored = restore_all(&connection).unwrap();
        assert_eq!(
            restored.sequence(21, "session", "turn-one", "turn.started", None),
            Err(crate::event_sequencer::SequenceError::Rejected(
                "turn-start-rejected"
            ))
        );
        let next = restored
            .sequence(21, "session", "turn-three", "turn.started", None)
            .unwrap();
        assert_eq!(next.sequence, 6);
        append(&mut connection, &next).unwrap();
        assert_eq!(
            read_current_watermark(&connection, "session")
                .unwrap()
                .sequence,
            6
        );
        verify_all(&connection).unwrap();
    }

    #[test]
    fn checkpoint_at_head_supports_empty_tail_and_later_append() {
        let mut connection = test_connection();
        let events = [
            envelope(1, 10, "turn", TurnState::Running, "turn.started", None),
            envelope(2, 11, "turn", TurnState::Completed, "turn.completed", None),
        ];
        for candidate in &events {
            append(&mut connection, candidate).unwrap();
        }
        let head = TimelineWatermark {
            sequence: 2,
            event_id: Some(events[1].event_id.clone()),
        };
        prune(&mut connection, &head, 20).unwrap();
        assert_eq!(
            connection
                .query_row("SELECT COUNT(*) FROM public_timeline_events", [], |row| row
                    .get::<_, i64>(0),)
                .unwrap(),
            0
        );
        let page = sync_page_from_anchor(&connection, "session", &head, None, 10).unwrap();
        assert!(page.events.is_empty());
        assert!(page.complete);
        let mut restored = restore_all(&connection).unwrap();
        let next = restored
            .sequence(21, "session", "next-turn", "turn.started", None)
            .unwrap();
        assert_eq!(next.sequence, 3);
        append(&mut connection, &next).unwrap();
        verify_all(&connection).unwrap();
    }

    #[test]
    fn prune_failures_roll_back_checkpoint_floor_and_events() {
        let trigger_cases = [
            (
                "fail_checkpoint_update",
                "BEFORE UPDATE OF through_sequence ON public_timeline_checkpoints",
                "timeline-journal-checkpoint-write-failed",
            ),
            (
                "fail_floor_update",
                "BEFORE UPDATE OF pruned_through_sequence ON public_timeline_cursors",
                "timeline-journal-floor-write-failed",
            ),
            (
                "fail_event_delete",
                "BEFORE DELETE ON public_timeline_events",
                "timeline-journal-prune-delete-failed",
            ),
        ];
        for (name, timing, expected_code) in trigger_cases {
            let mut connection = test_connection();
            let events = [
                envelope(1, 10, "turn", TurnState::Running, "turn.started", None),
                envelope(2, 11, "turn", TurnState::Completed, "turn.completed", None),
            ];
            for candidate in &events {
                append(&mut connection, candidate).unwrap();
            }
            connection
                .execute_batch(&format!(
                    "CREATE TRIGGER {name} {timing}
                     BEGIN SELECT RAISE(ABORT, 'injected prune failure'); END;"
                ))
                .unwrap();
            let boundary = TimelineWatermark {
                sequence: 2,
                event_id: Some(events[1].event_id.clone()),
            };
            assert_eq!(
                prune(&mut connection, &boundary, 20).unwrap_err().code,
                expected_code
            );
            connection
                .execute_batch(&format!("DROP TRIGGER {name};"))
                .unwrap();
            let state = read_retention_state(&connection, "session").unwrap();
            assert_eq!(state.floor.sequence, 0);
            assert_eq!(state.head.sequence, 2);
            assert_eq!(
                connection
                    .query_row("SELECT COUNT(*) FROM public_timeline_events", [], |row| row
                        .get::<_, i64>(0),)
                    .unwrap(),
                2
            );
            assert_eq!(
                connection
                    .query_row(
                        "SELECT through_sequence FROM public_timeline_checkpoints",
                        [],
                        |row| row.get::<_, i64>(0),
                    )
                    .unwrap(),
                0
            );
            verify_all(&connection).unwrap();
        }
    }

    #[test]
    fn checkpoint_tamper_or_retained_gap_fails_startup_and_reset_is_atomic() {
        let mut connection = test_connection();
        let events = [
            envelope(1, 10, "turn-one", TurnState::Running, "turn.started", None),
            envelope(
                2,
                11,
                "turn-one",
                TurnState::Completed,
                "turn.completed",
                None,
            ),
            envelope(3, 12, "turn-two", TurnState::Running, "turn.started", None),
        ];
        for candidate in &events {
            append(&mut connection, candidate).unwrap();
        }
        let boundary = TimelineWatermark {
            sequence: 2,
            event_id: Some(events[1].event_id.clone()),
        };
        prune(&mut connection, &boundary, 20).unwrap();
        let checkpoint_json: String = connection
            .query_row(
                "SELECT checkpoint_json FROM public_timeline_checkpoints",
                [],
                |row| row.get(0),
            )
            .unwrap();
        connection
            .execute(
                "UPDATE public_timeline_checkpoints SET checkpoint_json = '{}', checkpoint_bytes = 2",
                [],
            )
            .unwrap();
        assert_eq!(
            verify_all(&connection).unwrap_err().code,
            "timeline-journal-checkpoint-invalid"
        );
        connection
            .execute(
                "UPDATE public_timeline_checkpoints SET checkpoint_json = ?1, checkpoint_bytes = ?2",
                params![checkpoint_json, checkpoint_json.len() as i64],
            )
            .unwrap();
        connection
            .execute("DELETE FROM public_timeline_events WHERE sequence = 3", [])
            .unwrap();
        assert_eq!(
            verify_all(&connection).unwrap_err().code,
            "timeline-journal-cursor-mismatch"
        );

        let mut connection = test_connection();
        for candidate in &events {
            append(&mut connection, candidate).unwrap();
        }
        prune(&mut connection, &boundary, 20).unwrap();
        let transaction = connection
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .unwrap();
        reset_session_tx(&transaction, "session").unwrap();
        transaction.commit().unwrap();
        let state = read_retention_state(&connection, "session").unwrap();
        assert_eq!(state.floor.sequence, 0);
        assert_eq!(state.head.sequence, 0);
        assert_eq!(
            connection
                .query_row(
                    "SELECT through_sequence FROM public_timeline_checkpoints",
                    [],
                    |row| row.get::<_, i64>(0),
                )
                .unwrap(),
            0
        );
        verify_all(&connection).unwrap();
    }
}
