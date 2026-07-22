use crate::background_job::{
    BackgroundJobRequest, BackgroundJobState, BackgroundJobStatus, JobCancellationState,
};
use crate::background_notification::{
    BackgroundNotificationIntent, SCHEMA_VERSION as BACKGROUND_NOTIFICATION_SCHEMA_VERSION,
};
use crate::background_recovery_decision::{
    BackgroundRecoveryDecision, SCHEMA_VERSION as BACKGROUND_RECOVERY_DECISION_SCHEMA_VERSION,
};
use crate::background_scheduler::{BackgroundSchedulerSnapshot, SchedulerLeaseState};
use crate::background_scheduler_lease::{BackgroundSchedulerLease, BackgroundSchedulerLeaseStatus};
use crate::durable_blob::{
    available_space, sha256_hex, BlobFileError, DurableBlobFileStore, DurableBlobLocalFile,
    MAX_BLOB_BYTES, MAX_BLOB_OBJECTS, MAX_SCAN_ENTRIES, MAX_STORE_BYTES, MIN_FREE_BYTES,
};
use crate::git_workflow_authorization::{
    GitWorkflowAuthorizationAuthority, GitWorkflowAuthorizationError,
    GitWorkflowAuthorizationEvidence, GitWorkflowAuthorizationRequirement,
    GitWorkflowDecisionReference,
};
use crate::git_workflow_state::{validate_record, GitWorkflowRecord};
use crate::operation_reconciliation::{
    reconcile as reconcile_operation, EventState, ReconciliationInput, ReconciliationResult,
};
use crate::session_compaction::{activate_review, CompactionCheckpointReview};
use crate::session_compaction_store::{
    CompactionCheckpointDescriptor, STORE_SCHEMA_VERSION as COMPACTION_STORE_SCHEMA_VERSION,
};
use crate::turn_trace::{
    SessionMode as TraceSessionMode, TerminalState as TraceTerminalState, TracePayload, TurnTrace,
    LEGACY_SCHEMA_VERSION as LEGACY_TURN_TRACE_SCHEMA_VERSION,
    SCHEMA_VERSION as TURN_TRACE_SCHEMA_VERSION,
};
pub use crate::workbench_migration::WorkbenchRecoveryDiagnostic;
use crate::workbench_migration::{create_pre_upgrade_backup, inspect_recovery};
use crate::workspace_edit::ContentHash;
use rusqlite::{
    params, Connection, OpenFlags, OptionalExtension, Transaction, TransactionBehavior,
};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const DATABASE_FILE: &str = "aegisy-workbench.sqlite3";
const SCHEMA_VERSION: i64 = 13;
const APPLICATION_ID: i64 = 0x4145_4759;
const MAX_AUTHORIZATION_LIFETIME_MS: u64 = 5 * 60 * 1_000;
const MAX_EVENT_BYTES: usize = 72 * 1024;
const MAX_EVENT_PAGE: usize = 200;
const MAX_PROJECTION_VERIFY_ROWS: u64 = 100_000;
const MAX_STARTUP_RECOVERY_PROJECTS: u64 = 10_000;
const MAX_STARTUP_RECOVERY_SESSIONS: u64 = 10_000;
const MAX_STARTUP_RECOVERY_ROWS: u64 = 100_000;
const MAX_SESSION_DELETE_MEMBERS: usize = 10_000;
const MAX_SESSION_DELETE_PREVIEW_MEMBERS: usize = 200;
const MAX_SESSION_DELETE_SWEEP: usize = 32;
const MAX_SESSION_SEARCH_LIMIT: usize = 100;
const MAX_SESSION_SEARCH_TERM_BYTES: usize = 256;
const MAX_OPERATION_RECONCILIATIONS: usize = 10_000;
const MAX_BACKGROUND_JOBS: usize = 10_000;
const MAX_BACKGROUND_RECOVERY_DECISIONS: usize = 10_000;
const MAX_BACKGROUND_NOTIFICATIONS: usize = 10_000;
const MAX_BACKGROUND_NOTIFICATION_PAGE: usize = 100;
const MAX_BACKGROUND_NOTIFICATION_JSON_BYTES: usize = 32 * 1024;
const TURN_TRACE_RECORDED_SCHEMA_VERSION: &str = "turn.trace.recorded/0.1";
const MAX_BACKGROUND_JOB_PAGE: usize = 1_000;
const MAX_BACKGROUND_JOB_JSON_BYTES: usize = 64 * 1024;
const MAX_BACKGROUND_LEASE_JSON_BYTES: usize = 32 * 1024;
const MAX_PORTABLE_SESSION_ITEMS: usize = 2_000;
const MAX_PORTABLE_SESSION_BYTES: usize = 4 * 1024 * 1024;
const DATABASE_WRITE_HEADROOM_BYTES: u64 = 8 * 1024 * 1024;
const MAX_DATABASE_BYTES: u64 = 1024 * 1024 * 1024;
const WAL_AUTOCHECKPOINT_PAGES: i64 = 1_000;
const WAL_JOURNAL_SIZE_LIMIT_BYTES: i64 = 16 * 1024 * 1024;
const MAX_BLOB_METADATA_BYTES: usize = 16 * 1024;
const MIN_BLOB_RETENTION_MS: u64 = 24 * 60 * 60 * 1_000;
const MAX_BLOB_RETENTION_MS: u64 = 365 * 24 * 60 * 60 * 1_000;
const MIN_SESSION_DELETE_UNDO_MS: u64 = 24 * 60 * 60 * 1_000;
const MAX_SESSION_DELETE_UNDO_MS: u64 = 30 * 24 * 60 * 60 * 1_000;
const REQUIRED_TABLES: [&str; 4] = [
    "approval_decisions",
    "authorization_consumptions",
    "session_sequences",
    "events",
];
const REQUIRED_SESSION_TABLES: [&str; 3] = ["projects", "project_roots", "sessions"];
const REQUIRED_TURN_TABLES: [&str; 2] = ["turns", "items"];
const REQUIRED_PROJECTION_TABLES: [&str; 1] = ["session_projection_sources"];
const REQUIRED_BLOB_TABLES: [&str; 2] = ["durable_blobs", "durable_blob_references"];
const REQUIRED_RETENTION_TABLES: [&str; 3] = [
    "retention_policies",
    "session_deletions",
    "session_deletion_members",
];
const REQUIRED_NAVIGATION_TABLES: [&str; 1] = ["project_navigation"];
const REQUIRED_RUNTIME_BINDING_TABLES: [&str; 1] = ["session_runtime_bindings"];
const REQUIRED_WORKSPACE_BINDING_TABLES: [&str; 1] = ["session_workspace_bindings"];
const REQUIRED_BACKGROUND_JOB_TABLES: [&str; 1] = ["background_jobs"];
const REQUIRED_BACKGROUND_JOB_INDEXES: [&str; 2] = [
    "background_jobs_session_idx",
    "background_jobs_recovery_idx",
];
const REQUIRED_BACKGROUND_LEASE_TABLES: [&str; 1] = ["background_job_leases"];
const REQUIRED_BACKGROUND_LEASE_INDEXES: [&str; 2] = [
    "background_job_leases_recovery_idx",
    "background_job_leases_owner_idx",
];
const REQUIRED_BACKGROUND_NOTIFICATION_TABLES: [&str; 1] = ["background_notification_outbox"];
const REQUIRED_BACKGROUND_NOTIFICATION_INDEXES: [&str; 2] = [
    "background_notification_outbox_session_idx",
    "background_notification_outbox_job_idx",
];
const REQUIRED_SESSION_SEARCH_INDEXES: [&str; 4] = [
    "sessions_status_updated_idx",
    "session_runtime_binding_model_idx",
    "session_workspace_binding_branch_idx",
    "items_transcript_search_idx",
];
const SESSION_SEARCH_INDEX_SCHEMA_SQL: &str = "
    CREATE INDEX IF NOT EXISTS sessions_status_updated_idx
        ON sessions(status, updated_at_ms DESC, session_id ASC);
    CREATE INDEX IF NOT EXISTS session_runtime_binding_model_idx
        ON session_runtime_bindings(model, adapter, adapter_version, session_id);
    CREATE INDEX IF NOT EXISTS session_workspace_binding_branch_idx
        ON session_workspace_bindings(branch_sha256, project_id, session_id);
    CREATE INDEX IF NOT EXISTS items_transcript_search_idx
        ON items(session_id, role, item_kind, item_sequence);
";
const SESSION_SCHEMA_SQL: &str = "
    CREATE TABLE projects (
        project_id TEXT PRIMARY KEY,
        canonical_root TEXT NOT NULL,
        root_identity TEXT NOT NULL,
        display_name TEXT NOT NULL,
        state TEXT NOT NULL CHECK(state IN ('active','archived','unavailable')),
        created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),
        updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms >= created_at_ms)
    ) STRICT;
    CREATE TABLE project_roots (
        project_id TEXT NOT NULL,
        root_id TEXT NOT NULL,
        canonical_root TEXT NOT NULL,
        root_identity TEXT NOT NULL,
        access TEXT NOT NULL CHECK(access IN ('read','write')),
        created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),
        PRIMARY KEY(project_id, root_id),
        UNIQUE(project_id, canonical_root),
        FOREIGN KEY(project_id) REFERENCES projects(project_id)
    ) STRICT;
    CREATE TABLE sessions (
        session_id TEXT PRIMARY KEY,
        project_id TEXT,
        mode TEXT NOT NULL CHECK(mode IN ('chat','work')),
        title TEXT NOT NULL,
        parent_session_id TEXT,
        lineage_kind TEXT NOT NULL CHECK(lineage_kind IN ('new','resume','fork')),
        status TEXT NOT NULL CHECK(status IN ('active','archived','failed','interrupted')),
        environment_identity TEXT,
        created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),
        updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms >= created_at_ms),
        FOREIGN KEY(project_id) REFERENCES projects(project_id),
        FOREIGN KEY(parent_session_id) REFERENCES sessions(session_id)
    ) STRICT;
    CREATE INDEX project_roots_project_idx ON project_roots(project_id);
    CREATE INDEX sessions_project_idx ON sessions(project_id, updated_at_ms DESC);
    CREATE INDEX sessions_lineage_idx ON sessions(parent_session_id);
";
const PROJECT_NAVIGATION_SCHEMA_SQL: &str = "
    CREATE TABLE IF NOT EXISTS project_navigation (
        project_id TEXT PRIMARY KEY,
        pinned INTEGER NOT NULL CHECK(pinned IN (0,1)),
        last_opened_at_ms INTEGER NOT NULL CHECK(last_opened_at_ms >= 0),
        FOREIGN KEY(project_id) REFERENCES projects(project_id) ON DELETE CASCADE
    ) STRICT;
    CREATE INDEX IF NOT EXISTS project_navigation_order_idx
        ON project_navigation(pinned DESC, last_opened_at_ms DESC, project_id ASC);
";
const SESSION_RUNTIME_BINDING_SCHEMA_SQL: &str = "
    CREATE TABLE IF NOT EXISTS session_runtime_bindings (
        session_id TEXT PRIMARY KEY,
        adapter TEXT NOT NULL,
        adapter_version TEXT NOT NULL,
        backend_session_id TEXT,
        provider TEXT,
        model TEXT,
        permission_profile TEXT NOT NULL CHECK(permission_profile = 'read-only'),
        created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),
        updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms >= created_at_ms),
        FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
    ) STRICT;
";
const SESSION_WORKSPACE_BINDING_SCHEMA_SQL: &str = "
    CREATE TABLE IF NOT EXISTS session_workspace_bindings (
        session_id TEXT PRIMARY KEY,
        project_id TEXT NOT NULL,
        root_id TEXT NOT NULL,
        root_identity TEXT NOT NULL,
        workspace_kind TEXT NOT NULL CHECK(workspace_kind = 'project-root'),
        git_state TEXT NOT NULL
            CHECK(git_state IN ('unavailable','not-repository','repository-only','worktree')),
        repository_root_identity TEXT,
        worktree_root_identity TEXT,
        branch TEXT,
        branch_sha256 TEXT,
        branch_redacted INTEGER NOT NULL CHECK(branch_redacted IN (0,1)),
        head_oid TEXT,
        detached INTEGER NOT NULL CHECK(detached IN (0,1)),
        unborn INTEGER NOT NULL CHECK(unborn IN (0,1)),
        dedicated_worktree INTEGER NOT NULL CHECK(dedicated_worktree = 0),
        captured_at_ms INTEGER NOT NULL CHECK(captured_at_ms >= 0),
        updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms >= captured_at_ms),
        FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE,
        FOREIGN KEY(project_id, root_id) REFERENCES project_roots(project_id, root_id)
    ) STRICT;
";
const TURN_ITEM_SCHEMA_SQL: &str = "
    CREATE TABLE turns (
        turn_id TEXT PRIMARY KEY,
        session_id TEXT NOT NULL,
        idempotency_key TEXT,
        input_sha256 TEXT NOT NULL,
        input_bytes INTEGER NOT NULL CHECK(input_bytes > 0),
        state TEXT NOT NULL CHECK(state IN ('started','running','completed','failed','interrupted','cancelled')),
        created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),
        updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms >= created_at_ms),
        FOREIGN KEY(session_id) REFERENCES sessions(session_id)
    ) STRICT;
    CREATE UNIQUE INDEX turns_idempotency_idx
        ON turns(session_id, idempotency_key)
        WHERE idempotency_key IS NOT NULL;
    CREATE INDEX turns_session_idx ON turns(session_id, created_at_ms, turn_id);
    CREATE TABLE items (
        session_id TEXT NOT NULL,
        item_sequence INTEGER NOT NULL CHECK(item_sequence >= 1),
        item_id TEXT NOT NULL UNIQUE,
        turn_id TEXT,
        item_kind TEXT NOT NULL,
        role TEXT NOT NULL,
        state TEXT NOT NULL,
        payload_json TEXT NOT NULL,
        payload_sha256 TEXT NOT NULL,
        payload_bytes INTEGER NOT NULL CHECK(payload_bytes >= 0),
        created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),
        PRIMARY KEY(session_id, item_sequence),
        FOREIGN KEY(session_id) REFERENCES sessions(session_id),
        FOREIGN KEY(turn_id) REFERENCES turns(turn_id)
    ) STRICT;
    CREATE INDEX items_turn_idx ON items(session_id, turn_id, item_sequence);
";
const OPTIONAL_EVENT_PROJECT_SCHEMA_SQL: &str = "
    DROP INDEX IF EXISTS events_operation_idx;
    ALTER TABLE events RENAME TO events_required_project_v3;
    CREATE TABLE events (
        session_id TEXT NOT NULL,
        sequence INTEGER NOT NULL CHECK(sequence >= 1),
        event_id TEXT NOT NULL UNIQUE,
        timestamp_ms INTEGER NOT NULL CHECK(timestamp_ms >= 0),
        correlation_id TEXT NOT NULL,
        event_kind TEXT NOT NULL,
        project_id TEXT,
        operation_id TEXT NOT NULL,
        generation INTEGER NOT NULL CHECK(generation >= 0),
        payload_json TEXT NOT NULL,
        payload_sha256 TEXT NOT NULL,
        payload_bytes INTEGER NOT NULL CHECK(payload_bytes >= 0),
        PRIMARY KEY(session_id, sequence)
    ) STRICT;
    INSERT INTO events (
        session_id, sequence, event_id, timestamp_ms, correlation_id, event_kind,
        project_id, operation_id, generation, payload_json, payload_sha256, payload_bytes
    ) SELECT
        session_id, sequence, event_id, timestamp_ms, correlation_id, event_kind,
        project_id, operation_id, generation, payload_json, payload_sha256, payload_bytes
      FROM events_required_project_v3;
    DROP TABLE events_required_project_v3;
    CREATE INDEX events_operation_idx
        ON events(project_id, operation_id, sequence);
";
const PROJECTION_SOURCE_SCHEMA_SQL: &str = "
    CREATE TABLE IF NOT EXISTS session_projection_sources (
        session_id TEXT PRIMARY KEY,
        source_version INTEGER NOT NULL CHECK(source_version = 1),
        created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),
        FOREIGN KEY(session_id) REFERENCES sessions(session_id)
    ) STRICT;
";
const DURABLE_BLOB_SCHEMA_SQL: &str = "
    CREATE TABLE IF NOT EXISTS durable_blobs (
        sha256 TEXT PRIMARY KEY,
        bytes INTEGER NOT NULL CHECK(bytes >= 0),
        storage_key TEXT NOT NULL UNIQUE,
        created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),
        last_verified_at_ms INTEGER NOT NULL CHECK(last_verified_at_ms >= created_at_ms),
        retain_until_ms INTEGER NOT NULL CHECK(retain_until_ms >= created_at_ms)
    ) STRICT;
    CREATE TABLE IF NOT EXISTS durable_blob_references (
        reference_id TEXT PRIMARY KEY,
        content_reference TEXT NOT NULL,
        blob_sha256 TEXT NOT NULL,
        session_id TEXT,
        project_id TEXT,
        kind TEXT NOT NULL CHECK(kind IN (
            'command-output','patch','image','diagnostic','workspace-edit','artifact'
        )),
        media_type TEXT NOT NULL,
        owner_kind TEXT NOT NULL CHECK(owner_kind IN (
            'session','turn','item','edit','diagnostic','checkpoint'
        )),
        owner_id TEXT NOT NULL,
        metadata_json TEXT NOT NULL,
        metadata_sha256 TEXT NOT NULL,
        metadata_bytes INTEGER NOT NULL CHECK(metadata_bytes >= 0),
        state TEXT NOT NULL CHECK(state IN ('active','released')),
        created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),
        last_accessed_at_ms INTEGER NOT NULL CHECK(last_accessed_at_ms >= created_at_ms),
        retain_until_ms INTEGER NOT NULL CHECK(retain_until_ms >= created_at_ms),
        released_at_ms INTEGER,
        CHECK(session_id IS NOT NULL OR project_id IS NOT NULL),
        CHECK(
            (state = 'active' AND released_at_ms IS NULL) OR
            (state = 'released' AND released_at_ms IS NOT NULL)
        ),
        FOREIGN KEY(blob_sha256) REFERENCES durable_blobs(sha256),
        FOREIGN KEY(session_id) REFERENCES sessions(session_id),
        FOREIGN KEY(project_id) REFERENCES projects(project_id)
    ) STRICT;
    CREATE INDEX IF NOT EXISTS durable_blob_session_idx
        ON durable_blob_references(session_id, state, created_at_ms);
    CREATE INDEX IF NOT EXISTS durable_blob_project_idx
        ON durable_blob_references(project_id, state, created_at_ms);
    CREATE INDEX IF NOT EXISTS durable_blob_content_idx
        ON durable_blob_references(content_reference, blob_sha256);
    CREATE INDEX IF NOT EXISTS durable_blob_retention_idx
        ON durable_blob_references(state, retain_until_ms);
";
const RETENTION_SCHEMA_SQL: &str = "
    CREATE TABLE IF NOT EXISTS retention_policies (
        scope_kind TEXT NOT NULL CHECK(scope_kind IN ('project','session')),
        scope_id TEXT NOT NULL,
        archive_after_ms INTEGER CHECK(archive_after_ms IS NULL OR archive_after_ms >= 86400000),
        delete_after_ms INTEGER CHECK(delete_after_ms IS NULL OR delete_after_ms >= 86400000),
        undo_window_ms INTEGER NOT NULL CHECK(undo_window_ms BETWEEN 86400000 AND 2592000000),
        delete_scope TEXT NOT NULL CHECK(delete_scope IN ('session-only','lineage')),
        updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms >= 0),
        PRIMARY KEY(scope_kind, scope_id),
        CHECK(archive_after_ms IS NOT NULL OR delete_after_ms IS NOT NULL)
    ) STRICT;
    CREATE TABLE IF NOT EXISTS session_deletions (
        deletion_id TEXT PRIMARY KEY,
        root_session_id TEXT NOT NULL,
        scope TEXT NOT NULL CHECK(scope IN ('session-only','lineage')),
        plan_sha256 TEXT NOT NULL,
        plan_bytes INTEGER NOT NULL CHECK(plan_bytes > 0),
        session_count INTEGER NOT NULL CHECK(session_count >= 1),
        descendant_count INTEGER NOT NULL CHECK(descendant_count >= 0),
        artifact_reference_count INTEGER NOT NULL CHECK(artifact_reference_count >= 0),
        artifact_bytes INTEGER NOT NULL CHECK(artifact_bytes >= 0),
        requested_at_ms INTEGER NOT NULL CHECK(requested_at_ms >= 0),
        undo_until_ms INTEGER NOT NULL CHECK(undo_until_ms >= requested_at_ms),
        state TEXT NOT NULL CHECK(state IN ('pending','purged')),
        updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms >= requested_at_ms),
        FOREIGN KEY(root_session_id) REFERENCES sessions(session_id)
    ) STRICT;
    CREATE INDEX IF NOT EXISTS session_deletions_state_idx
        ON session_deletions(state, undo_until_ms, deletion_id);
    CREATE TABLE IF NOT EXISTS session_deletion_members (
        deletion_id TEXT NOT NULL,
        session_id TEXT NOT NULL,
        depth INTEGER NOT NULL CHECK(depth >= 0),
        original_status TEXT NOT NULL CHECK(original_status IN ('active','archived','failed','interrupted')),
        PRIMARY KEY(deletion_id, session_id),
        UNIQUE(session_id),
        FOREIGN KEY(deletion_id) REFERENCES session_deletions(deletion_id) ON DELETE CASCADE,
        FOREIGN KEY(session_id) REFERENCES sessions(session_id)
    ) STRICT;
    CREATE INDEX IF NOT EXISTS session_deletion_members_session_idx
        ON session_deletion_members(session_id, deletion_id);
";
const BACKGROUND_JOB_SCHEMA_SQL: &str = "
    CREATE TABLE IF NOT EXISTS background_jobs (
        job_id TEXT PRIMARY KEY,
        session_id TEXT NOT NULL,
        project_id TEXT NOT NULL,
        root_id TEXT NOT NULL,
        request_identity TEXT NOT NULL UNIQUE,
        idempotency_identity TEXT NOT NULL,
        request_json TEXT NOT NULL,
        request_sha256 TEXT NOT NULL CHECK(length(request_sha256) = 64),
        request_bytes INTEGER NOT NULL CHECK(request_bytes > 0),
        state_identity TEXT NOT NULL,
        state_json TEXT NOT NULL,
        state_sha256 TEXT NOT NULL CHECK(length(state_sha256) = 64),
        state_bytes INTEGER NOT NULL CHECK(state_bytes > 0),
        status TEXT NOT NULL CHECK(status IN (
            'queued','running','pause_requested','paused','waiting_approval',
            'cancelling','completed','failed','cancelled','interrupted'
        )),
        cancellation_state TEXT NOT NULL CHECK(cancellation_state IN (
            'not_requested','requested','acknowledged','failed',
            'superseded_by_completion'
        )),
        generation INTEGER NOT NULL CHECK(generation >= 0),
        attempt_count INTEGER NOT NULL CHECK(attempt_count BETWEEN 0 AND 16),
        next_eligible_at_ms INTEGER NOT NULL CHECK(next_eligible_at_ms >= 0),
        created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),
        updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms >= created_at_ms),
        UNIQUE(session_id, idempotency_identity),
        FOREIGN KEY(session_id) REFERENCES sessions(session_id),
        FOREIGN KEY(project_id, root_id) REFERENCES project_roots(project_id, root_id)
    ) STRICT;
    CREATE INDEX IF NOT EXISTS background_jobs_session_idx
        ON background_jobs(session_id, updated_at_ms DESC, job_id ASC);
    CREATE INDEX IF NOT EXISTS background_jobs_recovery_idx
        ON background_jobs(status, next_eligible_at_ms, updated_at_ms, job_id);
";
const BACKGROUND_LEASE_SCHEMA_SQL: &str = "
    CREATE TABLE IF NOT EXISTS background_job_leases (
        job_id TEXT PRIMARY KEY,
        session_id TEXT NOT NULL,
        project_id TEXT NOT NULL,
        root_id TEXT NOT NULL,
        request_identity TEXT NOT NULL,
        state_identity TEXT NOT NULL,
        job_generation INTEGER NOT NULL CHECK(job_generation >= 0),
        owner_identity TEXT NOT NULL,
        lease_generation INTEGER NOT NULL CHECK(lease_generation >= 1),
        status TEXT NOT NULL CHECK(status IN ('active','released','expired')),
        acquired_at_ms INTEGER NOT NULL CHECK(acquired_at_ms > 0),
        renewed_at_ms INTEGER NOT NULL CHECK(renewed_at_ms >= acquired_at_ms),
        expires_at_ms INTEGER NOT NULL CHECK(expires_at_ms > renewed_at_ms),
        updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms >= renewed_at_ms),
        process_registration_identity TEXT,
        process_identity TEXT,
        released_at_ms INTEGER,
        release_reason TEXT CHECK(release_reason IS NULL OR release_reason IN (
            'job_terminal','ownership_yielded','recovery_abandoned','lease_expired'
        )),
        lease_identity TEXT NOT NULL UNIQUE,
        lease_json TEXT NOT NULL,
        lease_sha256 TEXT NOT NULL CHECK(length(lease_sha256) = 64),
        lease_bytes INTEGER NOT NULL CHECK(lease_bytes > 0),
        dispatch_authority INTEGER NOT NULL CHECK(dispatch_authority = 0),
        automatic_takeover INTEGER NOT NULL CHECK(automatic_takeover = 0),
        CHECK((process_registration_identity IS NULL) = (process_identity IS NULL)),
        CHECK((released_at_ms IS NULL) = (release_reason IS NULL)),
        FOREIGN KEY(job_id) REFERENCES background_jobs(job_id) ON DELETE CASCADE,
        FOREIGN KEY(session_id) REFERENCES sessions(session_id),
        FOREIGN KEY(project_id, root_id) REFERENCES project_roots(project_id, root_id)
    ) STRICT;
    CREATE INDEX IF NOT EXISTS background_job_leases_recovery_idx
        ON background_job_leases(status, expires_at_ms, job_id);
    CREATE INDEX IF NOT EXISTS background_job_leases_owner_idx
        ON background_job_leases(owner_identity, status, expires_at_ms, job_id);
";
const BACKGROUND_NOTIFICATION_OUTBOX_SCHEMA_SQL: &str = "
    CREATE TABLE IF NOT EXISTS background_notification_outbox (
        intent_identity TEXT PRIMARY KEY,
        deduplication_identity TEXT NOT NULL UNIQUE,
        job_id TEXT NOT NULL,
        session_id TEXT NOT NULL,
        project_id TEXT NOT NULL,
        root_id TEXT NOT NULL,
        request_identity TEXT NOT NULL,
        state_identity TEXT NOT NULL,
        job_generation INTEGER NOT NULL CHECK(job_generation >= 0),
        kind TEXT NOT NULL CHECK(kind IN (
            'completed','failed','approval_needed','budget_exhausted'
        )),
        job_status TEXT NOT NULL CHECK(job_status IN (
            'queued','running','pause_requested','paused','waiting_approval',
            'cancelling','completed','failed','cancelled','interrupted'
        )),
        intent_json TEXT NOT NULL,
        intent_sha256 TEXT NOT NULL CHECK(length(intent_sha256) = 64),
        intent_bytes INTEGER NOT NULL CHECK(intent_bytes > 0),
        event_sequence INTEGER NOT NULL CHECK(event_sequence >= 1),
        delivery_state TEXT NOT NULL CHECK(delivery_state = 'recorded'),
        delivery_attempt_count INTEGER NOT NULL CHECK(delivery_attempt_count = 0),
        content_included INTEGER NOT NULL CHECK(content_included = 0),
        delivery_available INTEGER NOT NULL CHECK(delivery_available = 0),
        delivery_attempted INTEGER NOT NULL CHECK(delivery_attempted = 0),
        platform_delivery_authority INTEGER NOT NULL CHECK(platform_delivery_authority = 0),
        created_at_ms INTEGER NOT NULL CHECK(created_at_ms > 0),
        recorded_at_ms INTEGER NOT NULL CHECK(recorded_at_ms >= created_at_ms),
        UNIQUE(session_id, event_sequence),
        FOREIGN KEY(job_id) REFERENCES background_jobs(job_id) ON DELETE CASCADE,
        FOREIGN KEY(session_id) REFERENCES sessions(session_id),
        FOREIGN KEY(project_id, root_id) REFERENCES project_roots(project_id, root_id),
        FOREIGN KEY(session_id, event_sequence)
            REFERENCES events(session_id, sequence) ON DELETE CASCADE
    ) STRICT;
    CREATE INDEX IF NOT EXISTS background_notification_outbox_session_idx
        ON background_notification_outbox(
            session_id, recorded_at_ms DESC, intent_identity ASC
        );
    CREATE INDEX IF NOT EXISTS background_notification_outbox_job_idx
        ON background_notification_outbox(job_id, recorded_at_ms, intent_identity);
";

#[derive(Debug)]
pub struct WorkbenchStore {
    connection: Connection,
    path: PathBuf,
    blob_files: DurableBlobFileStore,
    quarantined_projects: BTreeSet<String>,
    quarantined_sessions: BTreeSet<String>,
    startup_rebuilt_sessions: BTreeSet<String>,
    startup_recovery: Box<StartupProjectionRecoveryReport>,
    #[cfg(test)]
    database_available_bytes_override: Option<u64>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GitWorkflowDecisionKind {
    Permission,
    ExplicitApproval,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GitWorkflowDecisionTiming {
    pub issued_at_ms: u64,
    pub expires_at_ms: u64,
    pub observed_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct WorkbenchEvent {
    pub schema_version: String,
    pub session_id: String,
    pub sequence: u64,
    pub event_id: String,
    pub timestamp_ms: u64,
    pub correlation_id: String,
    pub event_kind: String,
    pub project_id: Option<String>,
    pub operation_id: String,
    pub generation: u64,
    pub payload: serde_json::Value,
    pub payload_hash: ContentHash,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredTurnTrace {
    pub trace: TurnTrace,
    pub trace_identity: String,
    pub state: String,
    pub event_sequence: u64,
    pub recorded_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredBackgroundJob {
    pub schema_version: String,
    pub request: BackgroundJobRequest,
    pub state: BackgroundJobState,
    pub request_hash: ContentHash,
    pub state_hash: ContentHash,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredBackgroundSchedulerLease {
    pub schema_version: String,
    pub lease: BackgroundSchedulerLease,
    pub lease_hash: ContentHash,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredBackgroundRecoveryDecision {
    pub schema_version: String,
    pub decision: BackgroundRecoveryDecision,
    pub event_sequence: u64,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum BackgroundNotificationDeliveryState {
    Recorded,
}

impl BackgroundNotificationDeliveryState {
    fn as_str(self) -> &'static str {
        match self {
            Self::Recorded => "recorded",
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredBackgroundNotification {
    pub schema_version: String,
    pub intent: BackgroundNotificationIntent,
    pub intent_hash: ContentHash,
    pub event_sequence: u64,
    pub delivery_state: BackgroundNotificationDeliveryState,
    pub delivery_attempt_count: u32,
    pub recorded_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct BackgroundNotificationCursor {
    pub recorded_at_ms: u64,
    pub intent_identity: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct BackgroundNotificationPage {
    pub schema_version: String,
    pub session_id: String,
    pub notifications: Vec<StoredBackgroundNotification>,
    pub next_cursor: Option<BackgroundNotificationCursor>,
    pub content_included: bool,
    pub delivery_mutation_available: bool,
    pub platform_delivery_authority: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredOperationReconciliation {
    pub input: ReconciliationInput,
    pub result: ReconciliationResult,
    pub event_sequence: u64,
    pub updated_at_ms: u64,
}

#[derive(Debug, Deserialize)]
struct GitWorkflowLifecyclePayload {
    schema_version: String,
    action: String,
    state: String,
    operation_kind: String,
    authorization_id: String,
    requirement_hash: ContentHash,
    record_hash: ContentHash,
    observed_head: String,
    observed_operation: Option<String>,
    visible_conflict_count: usize,
    redacted_conflict_count: usize,
    command_exit_code: Option<i32>,
    outcome: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorkbenchStoreError {
    pub code: String,
    pub message: String,
}

#[derive(Debug)]
pub enum WorkbenchStoreOpen {
    Writable(WorkbenchStore),
    ReadOnlyRecovery(WorkbenchRecoveryDiagnostic),
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum StoredSessionMode {
    Chat,
    Work,
}

impl StoredSessionMode {
    fn as_str(self) -> &'static str {
        match self {
            Self::Chat => "chat",
            Self::Work => "work",
        }
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum StoredSessionLineage {
    New,
    Resume,
    Fork,
}

impl StoredSessionLineage {
    fn as_str(self) -> &'static str {
        match self {
            Self::New => "new",
            Self::Resume => "resume",
            Self::Fork => "fork",
        }
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum SessionDeletionScope {
    SessionOnly,
    Lineage,
}

impl SessionDeletionScope {
    fn as_str(self) -> &'static str {
        match self {
            Self::SessionOnly => "session-only",
            Self::Lineage => "lineage",
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredProject {
    pub project_id: String,
    pub canonical_root: String,
    pub root_identity: String,
    pub display_name: String,
    pub state: String,
    pub created_at_ms: u64,
    pub updated_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredProjectNavigation {
    pub project_id: String,
    pub pinned: bool,
    pub last_opened_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredProjectNavigationEntry {
    pub project: StoredProject,
    pub navigation: StoredProjectNavigation,
    pub availability: String,
    pub relink_required: bool,
    pub session_count: u64,
    pub active_session_count: u64,
    pub live_session_count: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredProjectRoot {
    pub project_id: String,
    pub root_id: String,
    pub canonical_root: String,
    pub root_identity: String,
    pub access: String,
    pub created_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredProjectTrustAcknowledgement {
    pub project_id: String,
    pub root_id: String,
    pub root_identity: String,
    pub review_id: String,
    pub acknowledged_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredProjectTrustAcknowledge {
    pub project_id: String,
    pub root_id: String,
    pub root_identity: String,
    pub review_id: String,
    pub acknowledged_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredSession {
    pub session_id: String,
    pub project_id: Option<String>,
    pub mode: StoredSessionMode,
    pub title: String,
    pub parent_session_id: Option<String>,
    pub lineage_kind: StoredSessionLineage,
    pub status: String,
    pub environment_identity: Option<String>,
    pub created_at_ms: u64,
    pub updated_at_ms: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SessionSearchRequest {
    pub project_id: Option<String>,
    pub branch: Option<String>,
    pub model: Option<String>,
    pub runtime: Option<String>,
    pub status: Option<String>,
    pub title: Option<String>,
    pub text: Option<String>,
    pub include_archived: bool,
    pub cursor: Option<String>,
    pub limit: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredSessionSearchResult {
    pub session: StoredSession,
    pub runtime: Option<StoredSessionRuntimeBinding>,
    pub workspace: Option<StoredSessionWorkspaceBinding>,
    pub matched_fields: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SessionSearchPage {
    pub results: Vec<StoredSessionSearchResult>,
    pub next_cursor: Option<String>,
    pub truncated: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredSessionRuntimeBinding {
    pub session_id: String,
    pub adapter: String,
    pub adapter_version: String,
    pub backend_session_id: Option<String>,
    pub provider: Option<String>,
    pub model: Option<String>,
    pub permission_profile: String,
    pub created_at_ms: u64,
    pub updated_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredSessionRuntimeBindingCreate {
    pub session_id: String,
    pub adapter: String,
    pub adapter_version: String,
    pub backend_session_id: Option<String>,
    pub provider: Option<String>,
    pub model: Option<String>,
    pub permission_profile: String,
    pub created_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredSessionWorkspaceBinding {
    pub session_id: String,
    pub project_id: String,
    pub root_id: String,
    pub root_identity: String,
    pub workspace_kind: String,
    pub git_state: String,
    pub repository_root_identity: Option<String>,
    pub worktree_root_identity: Option<String>,
    pub branch: Option<String>,
    pub branch_sha256: Option<String>,
    pub branch_redacted: bool,
    pub head_oid: Option<String>,
    pub detached: bool,
    pub unborn: bool,
    pub dedicated_worktree: bool,
    pub captured_at_ms: u64,
    pub updated_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredSessionWorkspaceBindingCreate {
    pub session_id: String,
    pub project_id: String,
    pub root_id: String,
    pub root_identity: String,
    pub workspace_kind: String,
    pub git_state: String,
    pub repository_root_identity: Option<String>,
    pub worktree_root_identity: Option<String>,
    pub branch: Option<String>,
    pub branch_sha256: Option<String>,
    pub branch_redacted: bool,
    pub head_oid: Option<String>,
    pub detached: bool,
    pub unborn: bool,
    pub dedicated_worktree: bool,
    pub captured_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredProjectCreate {
    pub project_id: String,
    pub root_id: String,
    pub canonical_root: String,
    pub root_identity: String,
    pub display_name: String,
    pub root_access: String,
    pub created_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredProjectRootAdd {
    pub project_id: String,
    pub root_id: String,
    pub canonical_root: String,
    pub root_identity: String,
    pub access: String,
    pub created_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredProjectRootRelink {
    pub project_id: String,
    pub root_id: String,
    pub canonical_root: String,
    pub root_identity: String,
    pub expected_root_identity: String,
    pub updated_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredSessionCreate {
    pub session_id: String,
    pub project_id: Option<String>,
    pub mode: StoredSessionMode,
    pub title: String,
    pub parent_session_id: Option<String>,
    pub lineage_kind: StoredSessionLineage,
    pub environment_identity: Option<String>,
    pub created_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SessionDeletionAffectedSession {
    pub session_id: String,
    pub title: String,
    pub status: String,
    pub depth: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SessionDeletionPreview {
    pub schema_version: String,
    pub root_session_id: String,
    pub scope: SessionDeletionScope,
    pub plan_hash: ContentHash,
    pub session_count: u64,
    pub descendant_count: u64,
    pub turn_count: u64,
    pub item_count: u64,
    pub event_count: u64,
    pub background_job_count: u64,
    pub artifact_reference_count: u64,
    pub artifact_bytes: u64,
    pub affected_sessions: Vec<SessionDeletionAffectedSession>,
    pub affected_sessions_truncated: bool,
    pub undo_window_min_ms: u64,
    pub undo_window_max_ms: u64,
    pub blocking_reasons: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SessionDeletionReceipt {
    pub schema_version: String,
    pub deletion_id: String,
    pub root_session_id: String,
    pub scope: SessionDeletionScope,
    pub plan_hash: ContentHash,
    pub session_count: u64,
    pub descendant_count: u64,
    pub artifact_reference_count: u64,
    pub artifact_bytes: u64,
    pub requested_at_ms: u64,
    pub undo_until_ms: u64,
    pub state: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SessionDeletionSweepReport {
    pub schema_version: String,
    pub examined: u64,
    pub purged: u64,
    pub released_artifact_references: u64,
    pub retained: u64,
    pub issues: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct RetentionPolicy {
    pub scope_kind: String,
    pub scope_id: String,
    pub archive_after_ms: Option<u64>,
    pub delete_after_ms: Option<u64>,
    pub undo_window_ms: u64,
    pub delete_scope: SessionDeletionScope,
    pub updated_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct RetentionSweepReport {
    pub schema_version: String,
    pub examined_sessions: u64,
    pub archived_sessions: u64,
    pub scheduled_deletions: u64,
    pub protected_sessions: u64,
    pub skipped_sessions: u64,
    pub issues: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PortableSessionItem {
    pub source_sequence: u64,
    pub source_item_id: String,
    pub item_kind: String,
    pub role: String,
    pub state: String,
    pub payload: serde_json::Value,
    pub source_created_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PortableSessionContent {
    pub schema_version: String,
    pub source_session_id: String,
    pub source_had_project: bool,
    pub mode: StoredSessionMode,
    pub title: String,
    pub source_created_at_ms: u64,
    pub source_updated_at_ms: u64,
    pub items: Vec<PortableSessionItem>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PortableSessionPackage {
    pub schema_version: String,
    pub exported_at_ms: u64,
    pub content: PortableSessionContent,
    pub content_hash: ContentHash,
    pub redacted_value_count: u64,
    pub excluded_field_count: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PortableSessionExportPreview {
    pub schema_version: String,
    pub source_session_id: String,
    pub mode: StoredSessionMode,
    pub title: String,
    pub item_count: u64,
    pub package_bytes: u64,
    pub package_hash: ContentHash,
    pub redacted_value_count: u64,
    pub excluded_field_count: u64,
    pub content_categories: Vec<String>,
    pub warnings: Vec<String>,
    pub blocking_reasons: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PortableSessionImportPreview {
    pub schema_version: String,
    pub source_session_id: String,
    pub mode: StoredSessionMode,
    pub title: String,
    pub item_count: u64,
    pub package_bytes: u64,
    pub package_hash: ContentHash,
    pub source_session_collision: bool,
    pub source_item_id_collisions: u64,
    pub target_project_required: bool,
    pub content_categories: Vec<String>,
    pub warnings: Vec<String>,
    pub blocking_reasons: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PortableSessionImportReceipt {
    pub schema_version: String,
    pub session: StoredSession,
    pub source_session_id: String,
    pub package_hash: ContentHash,
    pub imported_items: u64,
    pub source_session_collision: bool,
    pub linked_source_session: bool,
}

pub struct PortableSessionImportCommand<'a> {
    pub target_session_id: &'a str,
    pub import_id: &'a str,
    pub package: &'a PortableSessionPackage,
    pub target_project_id: Option<&'a str>,
    pub reject_source_collisions: bool,
    pub target_environment_identity: Option<&'a str>,
    pub runtime_binding: Option<&'a StoredSessionRuntimeBindingCreate>,
    pub workspace_binding: Option<&'a StoredSessionWorkspaceBindingCreate>,
    pub imported_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredTurn {
    pub turn_id: String,
    pub session_id: String,
    pub idempotency_key: Option<String>,
    pub input_hash: ContentHash,
    pub state: String,
    pub created_at_ms: u64,
    pub updated_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredTurnCreate {
    pub turn_id: String,
    pub session_id: String,
    pub idempotency_key: Option<String>,
    pub input_hash: ContentHash,
    pub created_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredItem {
    pub session_id: String,
    pub sequence: u64,
    pub item_id: String,
    pub turn_id: Option<String>,
    pub item_kind: String,
    pub role: String,
    pub state: String,
    pub payload: serde_json::Value,
    pub payload_hash: ContentHash,
    pub created_at_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredItemAppend {
    pub session_id: String,
    pub turn_id: Option<String>,
    pub item_id: String,
    pub item_kind: String,
    pub role: String,
    pub state: String,
    pub payload: serde_json::Value,
    pub created_at_ms: u64,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum DurableBlobKind {
    CommandOutput,
    Patch,
    Image,
    Diagnostic,
    WorkspaceEdit,
    Artifact,
}

impl DurableBlobKind {
    fn as_str(self) -> &'static str {
        match self {
            Self::CommandOutput => "command-output",
            Self::Patch => "patch",
            Self::Image => "image",
            Self::Diagnostic => "diagnostic",
            Self::WorkspaceEdit => "workspace-edit",
            Self::Artifact => "artifact",
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct DurableBlobWrite {
    pub reference_id: String,
    pub content_reference: String,
    pub session_id: Option<String>,
    pub project_id: Option<String>,
    pub kind: DurableBlobKind,
    pub media_type: String,
    pub owner_kind: String,
    pub owner_id: String,
    pub metadata: serde_json::Value,
    pub content: Vec<u8>,
    pub created_at_ms: u64,
    pub retain_until_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredDurableBlobReference {
    pub reference_id: String,
    pub content_reference: String,
    pub content_hash: ContentHash,
    pub session_id: Option<String>,
    pub project_id: Option<String>,
    pub kind: DurableBlobKind,
    pub media_type: String,
    pub owner_kind: String,
    pub owner_id: String,
    pub metadata: serde_json::Value,
    pub state: String,
    pub created_at_ms: u64,
    pub last_accessed_at_ms: u64,
    pub retain_until_ms: u64,
    pub released_at_ms: Option<u64>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct DurableBlobRead {
    pub reference: StoredDurableBlobReference,
    pub content: Vec<u8>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct DurableBlobConsistency {
    pub schema_version: String,
    pub consistent: bool,
    pub checked_objects: u64,
    pub checked_references: u64,
    pub disk_objects: u64,
    pub unregistered_files: u64,
    pub unknown_disk_entries: u64,
    pub missing_or_corrupt_objects: u64,
    pub dangling_references: u64,
    pub retained_unreferenced_objects: u64,
    pub issues: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct DurableBlobGcReport {
    pub schema_version: String,
    pub examined: u64,
    pub deleted: u64,
    pub retained: u64,
    pub uncertain: u64,
    pub issues: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SessionProjectionConsistency {
    pub schema_version: String,
    pub session_id: String,
    pub consistent: bool,
    pub read_only_recovery_required: bool,
    pub checked_turns: u64,
    pub checked_items: u64,
    pub checked_events: u64,
    pub checked_blob_references: u64,
    pub latest_item_sequence: u64,
    pub latest_event_sequence: u64,
    pub issues: Vec<String>,
    pub rebuild_source_complete: bool,
    pub event_projection_matches: bool,
    pub blob_store_available: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SessionProjectionCandidate {
    pub schema_version: String,
    pub session_id: String,
    pub source_event_count: u64,
    pub source_hash: Option<ContentHash>,
    pub source_complete: bool,
    pub matches_current_projection: bool,
    pub session: Option<StoredSession>,
    pub turns: Vec<StoredTurn>,
    pub items: Vec<StoredItem>,
    pub issues: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ProjectProjectionCandidate {
    pub schema_version: String,
    pub project_id: String,
    pub stream_id: String,
    pub source_event_count: u64,
    pub source_hash: Option<ContentHash>,
    pub source_complete: bool,
    pub matches_current_projection: bool,
    pub project: Option<StoredProject>,
    pub roots: Vec<StoredProjectRoot>,
    pub issues: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct StartupProjectionRecoveryReport {
    pub schema_version: String,
    pub checked_projects: u64,
    pub healthy_projects: u64,
    pub rebuilt_projects: u64,
    pub quarantined_projects: u64,
    pub checked_project_roots: u64,
    pub checked_sessions: u64,
    pub healthy_sessions: u64,
    pub rebuilt_sessions: u64,
    pub quarantined_sessions: u64,
    pub checked_turns: u64,
    pub checked_items: u64,
    pub checked_events: u64,
    pub checked_blob_references: u64,
    pub issues: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct DatabaseMaintenanceReport {
    pub schema_version: String,
    pub database_bytes_before: u64,
    pub database_bytes_after: u64,
    pub wal_bytes_before: u64,
    pub wal_bytes_after: u64,
    pub freelist_pages_before: u64,
    pub freelist_pages_after: u64,
    pub reclaimed_bytes: u64,
    pub integrity_ok: bool,
}

#[derive(Debug)]
struct StoredDecision {
    authority_id: String,
    decision_id: String,
    decision_kind: String,
    requirement_hash: ContentHash,
    scope: String,
    issued_at_ms: u64,
    expires_at_ms: u64,
    status: String,
}

impl GitWorkflowDecisionKind {
    fn as_str(self) -> &'static str {
        match self {
            Self::Permission => "permission",
            Self::ExplicitApproval => "explicit-approval",
        }
    }
}

impl WorkbenchStore {
    pub fn open(data_root: &Path) -> Result<Self, WorkbenchStoreError> {
        let data_root = canonical_data_root(data_root)?;
        Self::open_canonical(&data_root)
    }

    pub fn open_or_recover(data_root: &Path) -> Result<WorkbenchStoreOpen, WorkbenchStoreError> {
        let data_root = canonical_data_root(data_root)?;
        match Self::open_canonical(&data_root) {
            Ok(store) => Ok(WorkbenchStoreOpen::Writable(store)),
            Err(cause) => Ok(WorkbenchStoreOpen::ReadOnlyRecovery(inspect_recovery(
                &data_root,
                &data_root.join(DATABASE_FILE),
                &cause.code,
                SCHEMA_VERSION as u64,
                APPLICATION_ID as u64,
            ))),
        }
    }

    fn open_canonical(data_root: &Path) -> Result<Self, WorkbenchStoreError> {
        let blob_files = DurableBlobFileStore::open(data_root).map_err(blob_file_error)?;
        let path = data_root.join(DATABASE_FILE);
        if let Ok(metadata) = fs::symlink_metadata(&path) {
            if metadata.file_type().is_symlink() || !metadata.is_file() {
                return Err(coded_error(
                    "workbench-database-path-unsafe",
                    "workbench database path is unsafe",
                ));
            }
        }
        let connection = Connection::open_with_flags(
            &path,
            OpenFlags::SQLITE_OPEN_READ_WRITE
                | OpenFlags::SQLITE_OPEN_CREATE
                | OpenFlags::SQLITE_OPEN_NO_MUTEX,
        )
        .map_err(|_| {
            coded_error(
                "workbench-database-open-failed",
                "cannot open workbench database",
            )
        })?;
        secure_file(&path)?;
        let mut store = Self {
            connection,
            path,
            blob_files,
            quarantined_projects: BTreeSet::new(),
            quarantined_sessions: BTreeSet::new(),
            startup_rebuilt_sessions: BTreeSet::new(),
            startup_recovery: Box::new(empty_startup_projection_recovery_report()),
            #[cfg(test)]
            database_available_bytes_override: None,
        };
        store
            .configure()
            .map_err(|cause| coded_error("workbench-database-configure-failed", cause.message))?;
        let version: i64 = store
            .connection
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .map_err(|_| {
                coded_error(
                    "workbench-schema-version-unreadable",
                    "cannot read workbench schema version",
                )
            })?;
        if version > SCHEMA_VERSION {
            return Err(coded_error(
                "workbench-schema-newer",
                "workbench database schema is newer than this runtime",
            ));
        }
        if version > 0 && version < SCHEMA_VERSION {
            let application_id: i64 = store
                .connection
                .pragma_query_value(None, "application_id", |row| row.get(0))
                .map_err(|_| {
                    coded_error(
                        "migration-source-identity-unreadable",
                        "cannot read migration source application ID",
                    )
                })?;
            let application_id = u64::try_from(application_id).map_err(|_| {
                coded_error(
                    "migration-source-identity-invalid",
                    "migration source application ID is invalid",
                )
            })?;
            create_pre_upgrade_backup(
                &store.connection,
                data_root,
                version as u64,
                SCHEMA_VERSION as u64,
                application_id,
            )
            .map_err(|cause| {
                coded_error(cause.code, "cannot create pre-upgrade workbench backup")
            })?;
        }
        store
            .migrate()
            .map_err(|cause| coded_error("workbench-migration-failed", cause.message))?;
        store
            .verify_integrity()
            .map_err(|cause| coded_error("workbench-database-integrity-failed", cause.message))?;
        store
            .claim_application_id()
            .map_err(|cause| coded_error("workbench-application-id-failed", cause.message))?;
        store.startup_recovery = Box::new(
            store
                .recover_session_projections_at_startup()
                .map_err(|cause| {
                    coded_error(cause.code, "cannot complete startup recovery scan")
                })?,
        );
        Ok(store)
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn startup_projection_recovery(&self) -> &StartupProjectionRecoveryReport {
        &self.startup_recovery
    }

    pub fn session_requires_recovery(&self, session_id: &str) -> bool {
        self.quarantined_sessions.contains(session_id)
    }

    pub fn quarantined_session_count(&self) -> u64 {
        self.quarantined_sessions.len() as u64
    }

    pub fn take_startup_rebuild_notice(&mut self, session_id: &str) -> bool {
        self.startup_rebuilt_sessions.remove(session_id)
    }

    fn ensure_session_writable(&self, session_id: &str) -> Result<(), WorkbenchStoreError> {
        validate_identifier(session_id, "session recovery ID")?;
        if self.session_requires_recovery(session_id) {
            return Err(coded_error(
                "session-read-only-recovery",
                "session is in read-only recovery",
            ));
        }
        match self.session_deletion_state(session_id)?.as_deref() {
            Some("pending") => {
                return Err(coded_error(
                    "session-deletion-pending",
                    "session deletion is pending",
                ));
            }
            Some("purged") => {
                return Err(coded_error("session-deleted", "session has been deleted"));
            }
            _ => {}
        }
        Ok(())
    }

    fn ensure_session_readable(&self, session_id: &str) -> Result<(), WorkbenchStoreError> {
        validate_identifier(session_id, "session read ID")?;
        if self.session_requires_recovery(session_id) {
            return Err(coded_error(
                "session-read-only-recovery",
                "session is in read-only recovery",
            ));
        }
        if self.session_deletion_state(session_id)?.as_deref() == Some("purged") {
            return Err(coded_error("session-deleted", "session has been deleted"));
        }
        Ok(())
    }

    fn session_deletion_state(
        &self,
        session_id: &str,
    ) -> Result<Option<String>, WorkbenchStoreError> {
        self.connection
            .query_row(
                "SELECT deletion.state
                 FROM session_deletion_members AS member
                 JOIN session_deletions AS deletion
                   ON deletion.deletion_id = member.deletion_id
                 WHERE member.session_id = ?1",
                [session_id],
                |row| row.get(0),
            )
            .optional()
            .map_err(|_| error("cannot inspect session deletion state"))
    }

    fn ensure_project_writable(&self, project_id: &str) -> Result<(), WorkbenchStoreError> {
        validate_identifier(project_id, "project recovery ID")?;
        if self.quarantined_projects.contains(project_id) {
            return Err(coded_error(
                "project-read-only-recovery",
                "project is in read-only recovery",
            ));
        }
        Ok(())
    }

    fn database_storage_bytes(&self) -> Result<(u64, u64), WorkbenchStoreError> {
        let database_bytes = fs::metadata(&self.path)
            .map_err(|_| error("cannot inspect workbench database size"))?
            .len();
        let mut wal_name = self.path.as_os_str().to_os_string();
        wal_name.push("-wal");
        let wal_path = PathBuf::from(wal_name);
        let wal_bytes = match fs::metadata(wal_path) {
            Ok(metadata) if metadata.is_file() => metadata.len(),
            Ok(_) => return Err(error("workbench WAL path is unsafe")),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => 0,
            Err(_) => return Err(error("cannot inspect workbench WAL size")),
        };
        Ok((database_bytes, wal_bytes))
    }

    fn database_available_bytes(&self) -> Option<u64> {
        #[cfg(test)]
        if self.database_available_bytes_override.is_some() {
            return self.database_available_bytes_override;
        }
        self.path.parent().and_then(available_space)
    }

    fn admit_database_write(&self) -> Result<(), WorkbenchStoreError> {
        let (database_bytes, wal_bytes) = self.database_storage_bytes()?;
        if database_bytes
            .saturating_add(wal_bytes)
            .saturating_add(DATABASE_WRITE_HEADROOM_BYTES)
            > MAX_DATABASE_BYTES
        {
            return Err(coded_error(
                "database-write-size-limit",
                "workbench database write size limit exceeded",
            ));
        }
        if self.database_available_bytes().is_some_and(|available| {
            available < MIN_FREE_BYTES.saturating_add(DATABASE_WRITE_HEADROOM_BYTES)
        }) {
            return Err(coded_error(
                "database-write-low-space",
                "workbench database write rejected by low-space reserve",
            ));
        }
        Ok(())
    }

    fn begin_database_write(
        &mut self,
        failure: &str,
    ) -> Result<Transaction<'_>, WorkbenchStoreError> {
        self.admit_database_write()?;
        self.connection
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .map_err(|_| error(failure))
    }

    pub fn compact_database(&mut self) -> Result<DatabaseMaintenanceReport, WorkbenchStoreError> {
        if !self.quarantined_projects.is_empty() || !self.quarantined_sessions.is_empty() {
            return Err(coded_error(
                "database-maintenance-recovery-blocked",
                "database maintenance is blocked during projection recovery",
            ));
        }
        let (database_bytes_before, wal_bytes_before) = self.database_storage_bytes()?;
        if database_bytes_before > MAX_DATABASE_BYTES {
            return Err(coded_error(
                "database-maintenance-size-limit",
                "database maintenance size limit exceeded",
            ));
        }
        let required = MIN_FREE_BYTES
            .saturating_add(DATABASE_WRITE_HEADROOM_BYTES)
            .saturating_add(database_bytes_before.saturating_mul(2))
            .saturating_add(wal_bytes_before);
        if self
            .database_available_bytes()
            .is_some_and(|available| available < required)
        {
            return Err(coded_error(
                "database-maintenance-low-space",
                "database maintenance rejected by low-space reserve",
            ));
        }
        let freelist_pages_before: i64 = self
            .connection
            .pragma_query_value(None, "freelist_count", |row| row.get(0))
            .map_err(|_| error("cannot inspect database freelist"))?;
        let (busy, _, _): (i64, i64, i64) = self
            .connection
            .query_row("PRAGMA wal_checkpoint(TRUNCATE)", [], |row| {
                Ok((row.get(0)?, row.get(1)?, row.get(2)?))
            })
            .map_err(|_| error("cannot checkpoint workbench WAL"))?;
        if busy != 0 {
            return Err(coded_error(
                "database-maintenance-busy",
                "database maintenance checkpoint is busy",
            ));
        }
        self.connection
            .execute_batch("VACUUM;")
            .map_err(|_| error("cannot compact workbench database"))?;
        let integrity: String = self
            .connection
            .query_row("PRAGMA quick_check(1)", [], |row| row.get(0))
            .map_err(|_| error("cannot verify compacted database"))?;
        if integrity != "ok" {
            return Err(error("compacted database integrity check failed"));
        }
        let freelist_pages_after: i64 = self
            .connection
            .pragma_query_value(None, "freelist_count", |row| row.get(0))
            .map_err(|_| error("cannot inspect compacted database freelist"))?;
        let (database_bytes_after, wal_bytes_after) = self.database_storage_bytes()?;
        Ok(DatabaseMaintenanceReport {
            schema_version: "workbench-database-maintenance/0.1".into(),
            database_bytes_before,
            database_bytes_after,
            wal_bytes_before,
            wal_bytes_after,
            freelist_pages_before: to_u64(freelist_pages_before, "database freelist pages")?,
            freelist_pages_after: to_u64(freelist_pages_after, "database freelist pages")?,
            reclaimed_bytes: database_bytes_before.saturating_sub(database_bytes_after),
            integrity_ok: true,
        })
    }

    #[cfg(test)]
    fn set_database_available_bytes_override(&mut self, available: Option<u64>) {
        self.database_available_bytes_override = available;
    }

    pub fn put_durable_blob(
        &mut self,
        request: DurableBlobWrite,
    ) -> Result<StoredDurableBlobReference, WorkbenchStoreError> {
        self.put_durable_blob_internal(request, None)
    }

    pub fn put_durable_blobs(
        &mut self,
        requests: Vec<DurableBlobWrite>,
    ) -> Result<Vec<StoredDurableBlobReference>, WorkbenchStoreError> {
        if requests.is_empty() || requests.len() > 256 {
            return Err(error("durable blob batch must contain 1 to 256 references"));
        }
        let prepared = requests
            .iter()
            .map(prepare_durable_blob)
            .collect::<Result<Vec<_>, _>>()?;
        let mut reference_ids = BTreeSet::new();
        let mut unique_content = BTreeMap::<String, usize>::new();
        for (index, request) in requests.iter().enumerate() {
            if let Some(session_id) = request.session_id.as_deref() {
                self.ensure_session_writable(session_id)?;
            }
            if !reference_ids.insert(request.reference_id.clone()) {
                return Err(error(
                    "durable blob batch contains a duplicate reference ID",
                ));
            }
            validate_blob_ownership(
                &self.connection,
                request.session_id.as_deref(),
                request.project_id.as_deref(),
            )?;
            unique_content
                .entry(prepared[index].content_hash.sha256.clone())
                .or_insert(index);
        }
        let (object_count, retained_bytes): (i64, i64) = self
            .connection
            .query_row(
                "SELECT COUNT(*), COALESCE(SUM(bytes), 0) FROM durable_blobs",
                [],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .map_err(|_| error("cannot inspect durable blob capacity"))?;
        let mut admitted_objects = to_u64(object_count, "durable blob object count")?;
        let mut admitted_bytes = to_u64(retained_bytes, "durable blob retained bytes")?;
        for (sha256, index) in &unique_content {
            let existing: Option<(i64, String)> = self
                .connection
                .query_row(
                    "SELECT bytes, storage_key FROM durable_blobs WHERE sha256 = ?1",
                    [sha256],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .optional()
                .map_err(|_| error("cannot inspect durable blob batch identity"))?;
            if let Some((bytes, storage_key)) = existing {
                if to_u64(bytes, "durable blob byte count")? != prepared[*index].content_hash.bytes
                    || storage_key != prepared[*index].storage_key
                {
                    return Err(error(
                        "durable blob metadata conflicts with content identity",
                    ));
                }
            } else {
                admitted_objects = admitted_objects.saturating_add(1);
                admitted_bytes = admitted_bytes.saturating_add(prepared[*index].content_hash.bytes);
            }
        }
        if admitted_objects > MAX_BLOB_OBJECTS || admitted_bytes > MAX_STORE_BYTES {
            return Err(error("durable blob admission limit exceeded"));
        }

        let mut created = Vec::new();
        for index in unique_content.values().copied() {
            match self.blob_files.put(
                &prepared[index].content_hash.sha256,
                &requests[index].content,
                None,
            ) {
                Ok(write) if write.created => created.push((
                    prepared[index].content_hash.sha256.clone(),
                    prepared[index].content_hash.bytes,
                )),
                Ok(_) => {}
                Err(cause) => {
                    for (sha256, bytes) in created {
                        self.blob_files.remove_created(&sha256, bytes);
                    }
                    return Err(blob_file_error(cause));
                }
            }
        }
        let result = (|| {
            let transaction =
                self.begin_database_write("cannot start durable blob batch transaction")?;
            let stored = requests
                .iter()
                .zip(&prepared)
                .map(|(request, prepared)| persist_durable_blob_tx(&transaction, request, prepared))
                .collect::<Result<Vec<_>, _>>()?;
            transaction
                .commit()
                .map_err(|_| error("cannot commit durable blob batch transaction"))?;
            Ok(stored)
        })();
        if result.is_err() {
            for (sha256, bytes) in created {
                self.blob_files.remove_created(&sha256, bytes);
            }
        }
        result
    }

    #[cfg(test)]
    fn put_durable_blob_with_available_bytes(
        &mut self,
        request: DurableBlobWrite,
        available_bytes: u64,
    ) -> Result<StoredDurableBlobReference, WorkbenchStoreError> {
        self.put_durable_blob_internal(request, Some(available_bytes))
    }

    fn put_durable_blob_internal(
        &mut self,
        request: DurableBlobWrite,
        available_bytes_override: Option<u64>,
    ) -> Result<StoredDurableBlobReference, WorkbenchStoreError> {
        if let Some(session_id) = request.session_id.as_deref() {
            self.ensure_session_writable(session_id)?;
        }
        let prepared = prepare_durable_blob(&request)?;
        validate_blob_ownership(
            &self.connection,
            request.session_id.as_deref(),
            request.project_id.as_deref(),
        )?;
        let existing: Option<(i64, String)> = self
            .connection
            .query_row(
                "SELECT bytes, storage_key FROM durable_blobs WHERE sha256 = ?1",
                [&prepared.content_hash.sha256],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()
            .map_err(|_| error("cannot inspect durable blob admission state"))?;
        if let Some((bytes, storage_key)) = existing {
            if to_u64(bytes, "durable blob byte count")? != prepared.content_hash.bytes
                || storage_key != prepared.storage_key
            {
                return Err(error(
                    "durable blob metadata conflicts with content identity",
                ));
            }
        } else {
            let (object_count, retained_bytes): (i64, i64) = self
                .connection
                .query_row(
                    "SELECT COUNT(*), COALESCE(SUM(bytes), 0) FROM durable_blobs",
                    [],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .map_err(|_| error("cannot inspect durable blob capacity"))?;
            let object_count = to_u64(object_count, "durable blob object count")?;
            let retained_bytes = to_u64(retained_bytes, "durable blob retained bytes")?;
            if object_count >= MAX_BLOB_OBJECTS
                || retained_bytes.saturating_add(prepared.content_hash.bytes) > MAX_STORE_BYTES
            {
                return Err(error("durable blob admission limit exceeded"));
            }
        }

        let write = self
            .blob_files
            .put(
                &prepared.content_hash.sha256,
                &request.content,
                available_bytes_override,
            )
            .map_err(blob_file_error)?;
        let persist_result = (|| {
            let transaction = self.begin_database_write("cannot start durable blob transaction")?;
            let stored = persist_durable_blob_tx(&transaction, &request, &prepared)?;
            transaction
                .commit()
                .map_err(|_| error("cannot commit durable blob transaction"))?;
            Ok(stored)
        })();
        if persist_result.is_err() && write.created {
            self.blob_files
                .remove_created(&prepared.content_hash.sha256, prepared.content_hash.bytes);
        }
        persist_result
    }

    pub fn read_durable_blob_for_session_read_only(
        &self,
        session_id: &str,
        content_reference: &str,
        accessed_at_ms: u64,
    ) -> Result<DurableBlobRead, WorkbenchStoreError> {
        validate_identifier(session_id, "durable blob session ID")?;
        self.ensure_session_readable(session_id)?;
        validate_content_reference(content_reference, None)?;
        let reference_id: String = self
            .connection
            .query_row(
                "SELECT reference_id FROM durable_blob_references
                 WHERE session_id = ?1 AND content_reference = ?2
                   AND (state = 'active' OR retain_until_ms >= ?3)
                 ORDER BY CASE state WHEN 'active' THEN 0 ELSE 1 END, created_at_ms DESC
                 LIMIT 1",
                params![
                    session_id,
                    content_reference,
                    to_i64(accessed_at_ms, "durable blob access time")?
                ],
                |row| row.get(0),
            )
            .optional()
            .map_err(|_| error("cannot locate durable blob reference"))?
            .ok_or_else(|| error("durable blob reference is unavailable for session"))?;
        let reference = load_durable_blob_reference(&self.connection, &reference_id)?;
        let content = self
            .blob_files
            .read(&reference.content_hash.sha256, reference.content_hash.bytes)
            .map_err(blob_file_error)?;
        Ok(DurableBlobRead { reference, content })
    }

    pub fn read_durable_blob_for_session(
        &mut self,
        session_id: &str,
        content_reference: &str,
        accessed_at_ms: u64,
    ) -> Result<DurableBlobRead, WorkbenchStoreError> {
        let read = self.read_durable_blob_for_session_read_only(
            session_id,
            content_reference,
            accessed_at_ms,
        )?;
        let reference_id = read.reference.reference_id.clone();
        let reference = read.reference;
        let content = read.content;
        let timestamp = to_i64(accessed_at_ms, "durable blob access time")?;
        let transaction =
            match self.begin_database_write("cannot start durable blob access transaction") {
                Ok(transaction) => transaction,
                Err(cause)
                    if matches!(
                        cause.code.as_str(),
                        "database-write-low-space" | "database-write-size-limit"
                    ) =>
                {
                    return Ok(DurableBlobRead { reference, content });
                }
                Err(cause) => return Err(cause),
            };
        transaction
            .execute(
                "UPDATE durable_blob_references
                 SET last_accessed_at_ms = CASE
                     WHEN last_accessed_at_ms < ?1 THEN ?1 ELSE last_accessed_at_ms END
                 WHERE reference_id = ?2",
                params![timestamp, reference_id],
            )
            .map_err(|_| error("cannot update durable blob access metadata"))?;
        transaction
            .execute(
                "UPDATE durable_blobs
                 SET last_verified_at_ms = CASE
                     WHEN last_verified_at_ms < ?1 THEN ?1 ELSE last_verified_at_ms END
                 WHERE sha256 = ?2",
                params![timestamp, reference.content_hash.sha256],
            )
            .map_err(|_| error("cannot update durable blob verification metadata"))?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit durable blob access metadata"))?;
        let mut reference = reference;
        reference.last_accessed_at_ms = reference.last_accessed_at_ms.max(accessed_at_ms);
        Ok(DurableBlobRead { reference, content })
    }

    pub fn inspect_durable_blob_reference(
        &self,
        project_id: &str,
        session_id: Option<&str>,
        content_reference: &str,
    ) -> Result<StoredDurableBlobReference, WorkbenchStoreError> {
        let reference = self.inspect_durable_blob_reference_lifecycle(
            project_id,
            session_id,
            content_reference,
        )?;
        if reference.state != "active" {
            return Err(error(
                "durable Blob metadata is unavailable for pinned context",
            ));
        }
        Ok(reference)
    }

    pub fn inspect_durable_blob_reference_lifecycle(
        &self,
        project_id: &str,
        session_id: Option<&str>,
        content_reference: &str,
    ) -> Result<StoredDurableBlobReference, WorkbenchStoreError> {
        validate_identifier(project_id, "pinned context Blob project ID")?;
        if let Some(session_id) = session_id {
            validate_identifier(session_id, "pinned context Blob session ID")?;
        }
        validate_content_reference(content_reference, None)?;
        let reference_id: String = self
            .connection
            .query_row(
                "SELECT reference_id FROM durable_blob_references
                 WHERE project_id = ?1
                   AND content_reference = ?2
                   AND state IN ('active', 'released')
                   AND ((?3 IS NULL AND session_id IS NULL) OR session_id = ?3)
                 ORDER BY CASE state WHEN 'active' THEN 0 ELSE 1 END,
                          created_at_ms DESC, reference_id ASC
                 LIMIT 1",
                params![project_id, content_reference, session_id],
                |row| row.get(0),
            )
            .optional()
            .map_err(|_| error("cannot locate durable Blob metadata"))?
            .ok_or_else(|| error("durable Blob metadata is unavailable for pinned context"))?;
        let reference = load_durable_blob_reference(&self.connection, &reference_id)?;
        if reference.project_id.as_deref() != Some(project_id)
            || reference.session_id.as_deref() != session_id
            || !matches!(reference.state.as_str(), "active" | "released")
            || reference.content_reference != content_reference
        {
            return Err(error("durable Blob metadata scope is invalid"));
        }
        Ok(reference)
    }

    pub(crate) fn read_durable_blob_for_pinned_context(
        &self,
        project_id: &str,
        session_id: Option<&str>,
        content_reference: &str,
    ) -> Result<DurableBlobRead, WorkbenchStoreError> {
        let reference =
            self.inspect_durable_blob_reference(project_id, session_id, content_reference)?;
        let content = self
            .blob_files
            .read(&reference.content_hash.sha256, reference.content_hash.bytes)
            .map_err(blob_file_error)?;
        Ok(DurableBlobRead { reference, content })
    }

    pub(crate) fn materialize_durable_image_for_turn(
        &self,
        project_id: &str,
        session_id: &str,
        content_reference: &str,
        extension: &str,
    ) -> Result<DurableBlobLocalFile, WorkbenchStoreError> {
        let reference =
            self.inspect_durable_blob_reference(project_id, Some(session_id), content_reference)?;
        if reference.kind != DurableBlobKind::Image {
            return Err(error("durable Blob is not an image"));
        }
        self.blob_files
            .link_verified_local_image(
                &reference.content_hash.sha256,
                reference.content_hash.bytes,
                extension,
            )
            .map_err(blob_file_error)
    }

    pub fn release_durable_blob_reference(
        &mut self,
        reference_id: &str,
        released_at_ms: u64,
    ) -> Result<StoredDurableBlobReference, WorkbenchStoreError> {
        validate_identifier(reference_id, "durable blob reference ID")?;
        let existing = load_durable_blob_reference(&self.connection, reference_id)?;
        if let Some(session_id) = existing.session_id.as_deref() {
            self.ensure_session_writable(session_id)?;
        }
        if existing.state == "released" {
            return Ok(existing);
        }
        let transaction =
            self.begin_database_write("cannot start durable blob release transaction")?;
        let released =
            release_durable_blob_reference_tx(&transaction, reference_id, released_at_ms)?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit durable blob release"))?;
        Ok(released)
    }

    pub fn scan_durable_blobs(&self) -> Result<DurableBlobConsistency, WorkbenchStoreError> {
        let inventory = self.blob_files.inventory().map_err(blob_file_error)?;
        let mut issues = Vec::new();
        let object_count = query_global_count(
            &self.connection,
            "SELECT COUNT(*) FROM durable_blobs",
            "cannot count durable blobs",
        )?;
        let reference_count = query_global_count(
            &self.connection,
            "SELECT COUNT(*) FROM durable_blob_references",
            "cannot count durable blob references",
        )?;
        if object_count > MAX_SCAN_ENTRIES as u64 || reference_count > MAX_SCAN_ENTRIES as u64 {
            push_projection_issue(&mut issues, "blob-scan-limit-exceeded");
        }
        if inventory.limit_exceeded {
            push_projection_issue(&mut issues, "blob-disk-scan-limit-exceeded");
        }
        if inventory.unknown_entries > 0 {
            push_projection_issue(&mut issues, "blob-unknown-disk-entry");
        }
        let mut registered = BTreeSet::new();
        let mut missing_or_corrupt = 0_u64;
        let mut statement = self
            .connection
            .prepare(
                "SELECT sha256, bytes, storage_key FROM durable_blobs
                 ORDER BY sha256 LIMIT ?1",
            )
            .map_err(|_| error("cannot prepare durable blob scan"))?;
        let rows = statement
            .query_map([MAX_SCAN_ENTRIES as i64], |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    row.get::<_, i64>(1)?,
                    row.get::<_, String>(2)?,
                ))
            })
            .map_err(|_| error("cannot scan durable blob metadata"))?;
        for row in rows {
            let (sha256, bytes, storage_key) =
                row.map_err(|_| error("durable blob metadata row is invalid"))?;
            registered.insert(sha256.clone());
            let valid_storage = DurableBlobFileStore::storage_key(&sha256)
                .is_ok_and(|expected| expected == storage_key);
            let bytes = to_u64(bytes, "durable blob byte count")?;
            if !valid_storage || self.blob_files.read(&sha256, bytes).is_err() {
                missing_or_corrupt = missing_or_corrupt.saturating_add(1);
            }
        }
        if missing_or_corrupt > 0 {
            push_projection_issue(&mut issues, "blob-missing-or-corrupt-object");
        }
        let dangling_references = query_global_count(
            &self.connection,
            "SELECT COUNT(*) FROM durable_blob_references AS reference
             LEFT JOIN durable_blobs AS blob ON blob.sha256 = reference.blob_sha256
             WHERE blob.sha256 IS NULL",
            "cannot verify durable blob reference targets",
        )?;
        if dangling_references > 0 {
            push_projection_issue(&mut issues, "blob-dangling-reference");
        }
        let retained_unreferenced_objects = query_global_count(
            &self.connection,
            "SELECT COUNT(*) FROM durable_blobs AS blob
             WHERE NOT EXISTS (
                SELECT 1 FROM durable_blob_references AS reference
                WHERE reference.blob_sha256 = blob.sha256 AND reference.state = 'active'
             )",
            "cannot count retained unreferenced durable blobs",
        )?;
        let unregistered_files = inventory.hashes.difference(&registered).count() as u64;
        if unregistered_files > 0 {
            push_projection_issue(&mut issues, "blob-unregistered-file");
        }
        Ok(DurableBlobConsistency {
            schema_version: "durable-blob-consistency/0.1".into(),
            consistent: issues.is_empty(),
            checked_objects: object_count.min(MAX_SCAN_ENTRIES as u64),
            checked_references: reference_count.min(MAX_SCAN_ENTRIES as u64),
            disk_objects: inventory.hashes.len() as u64,
            unregistered_files,
            unknown_disk_entries: inventory.unknown_entries,
            missing_or_corrupt_objects: missing_or_corrupt,
            dangling_references,
            retained_unreferenced_objects,
            issues,
        })
    }

    pub fn garbage_collect_durable_blobs(
        &mut self,
        now_ms: u64,
    ) -> Result<DurableBlobGcReport, WorkbenchStoreError> {
        if !self.quarantined_sessions.is_empty() {
            return Err(coded_error(
                "session-recovery-blocks-blob-gc",
                "durable blob collection is disabled during session recovery",
            ));
        }
        let mut report = DurableBlobGcReport {
            schema_version: "durable-blob-gc/0.1".into(),
            examined: 0,
            deleted: 0,
            retained: 0,
            uncertain: 0,
            issues: Vec::new(),
        };
        let mut statement = self
            .connection
            .prepare(
                "SELECT blob.sha256, blob.bytes
                 FROM durable_blobs AS blob
                 WHERE blob.retain_until_ms < ?1
                   AND NOT EXISTS (
                       SELECT 1 FROM durable_blob_references AS active_reference
                       WHERE active_reference.blob_sha256 = blob.sha256
                         AND active_reference.state = 'active'
                   )
                   AND NOT EXISTS (
                       SELECT 1 FROM durable_blob_references AS retained_reference
                       WHERE retained_reference.blob_sha256 = blob.sha256
                         AND retained_reference.retain_until_ms >= ?1
                   )
                 ORDER BY blob.retain_until_ms, blob.sha256
                 LIMIT 256",
            )
            .map_err(|_| error("cannot prepare durable blob garbage collection"))?;
        let candidates = statement
            .query_map([to_i64(now_ms, "durable blob collection time")?], |row| {
                Ok((row.get::<_, String>(0)?, row.get::<_, i64>(1)?))
            })
            .map_err(|_| error("cannot read durable blob collection candidates"))?
            .collect::<Result<Vec<_>, _>>()
            .map_err(|_| error("durable blob collection candidate is invalid"))?;
        drop(statement);
        for (sha256, bytes) in candidates {
            report.examined = report.examined.saturating_add(1);
            let bytes = to_u64(bytes, "durable blob byte count")?;
            if self.blob_files.read(&sha256, bytes).is_err() {
                report.uncertain = report.uncertain.saturating_add(1);
                push_projection_issue(&mut report.issues, "blob-gc-integrity-uncertain");
                continue;
            }
            let transaction =
                self.begin_database_write("cannot start durable blob collection transaction")?;
            let eligible: bool = transaction
                .query_row(
                    "SELECT EXISTS(
                        SELECT 1 FROM durable_blobs AS blob
                        WHERE blob.sha256 = ?1 AND blob.retain_until_ms < ?2
                          AND NOT EXISTS (
                              SELECT 1 FROM durable_blob_references AS reference
                              WHERE reference.blob_sha256 = blob.sha256
                                AND (reference.state = 'active' OR reference.retain_until_ms >= ?2)
                          )
                     )",
                    params![sha256, to_i64(now_ms, "durable blob collection time")?],
                    |row| row.get(0),
                )
                .map_err(|_| error("cannot revalidate durable blob collection candidate"))?;
            if !eligible {
                report.retained = report.retained.saturating_add(1);
                continue;
            }
            transaction
                .execute(
                    "DELETE FROM durable_blob_references WHERE blob_sha256 = ?1",
                    [&sha256],
                )
                .map_err(|_| error("cannot remove released durable blob references"))?;
            transaction
                .execute("DELETE FROM durable_blobs WHERE sha256 = ?1", [&sha256])
                .map_err(|_| error("cannot remove durable blob metadata"))?;
            transaction
                .commit()
                .map_err(|_| error("cannot commit durable blob collection"))?;
            match self.blob_files.remove_verified(&sha256, bytes) {
                Ok(()) => report.deleted = report.deleted.saturating_add(1),
                Err(_) => {
                    report.uncertain = report.uncertain.saturating_add(1);
                    push_projection_issue(&mut report.issues, "blob-gc-file-delete-failed");
                }
            }
        }
        Ok(report)
    }

    pub fn create_project(
        &mut self,
        request: StoredProjectCreate,
    ) -> Result<StoredProject, WorkbenchStoreError> {
        validate_identifier(&request.project_id, "project ID")?;
        validate_identifier(&request.root_id, "project root ID")?;
        validate_identity_text(&request.root_identity, "project root identity")?;
        validate_text(&request.display_name, 256, "project display name")?;
        if !matches!(request.root_access.as_str(), "read" | "write") {
            return Err(error("project root access is invalid"));
        }
        let canonical_root = canonical_store_root(&request.canonical_root)?;
        if canonical_root != Path::new(&request.canonical_root) {
            return Err(error("project root must already be canonical"));
        }
        let timestamp = to_i64(request.created_at_ms, "project creation time")?;
        let transaction = self.begin_database_write("cannot start project transaction")?;
        transaction
            .execute(
                "INSERT INTO projects (
                    project_id, canonical_root, root_identity, display_name, state,
                    created_at_ms, updated_at_ms
                 ) VALUES (?1, ?2, ?3, ?4, 'active', ?5, ?5)",
                params![
                    request.project_id,
                    request.canonical_root,
                    request.root_identity,
                    request.display_name,
                    timestamp,
                ],
            )
            .map_err(|_| error("project already exists or is invalid"))?;
        transaction
            .execute(
                "INSERT INTO project_roots (
                    project_id, root_id, canonical_root, root_identity, access, created_at_ms
                 ) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                params![
                    request.project_id,
                    request.root_id,
                    request.canonical_root,
                    request.root_identity,
                    request.root_access,
                    timestamp,
                ],
            )
            .map_err(|_| error("project root already exists or is invalid"))?;
        transaction
            .execute(
                "INSERT INTO project_navigation (
                    project_id, pinned, last_opened_at_ms
                 ) VALUES (?1, 0, ?2)",
                params![request.project_id, timestamp],
            )
            .map_err(|_| error("project navigation already exists or is invalid"))?;
        let stream_id = project_event_stream_id(&request.project_id);
        let event_id = derived_event_id("project-created", request.project_id.as_bytes());
        append_event_tx(
            &transaction,
            EventInput {
                session_id: &stream_id,
                event_id: &event_id,
                timestamp_ms: request.created_at_ms,
                correlation_id: &request.project_id,
                event_kind: "project.created",
                project_id: Some(&request.project_id),
                operation_id: &request.project_id,
                generation: 0,
                payload: json!({
                    "schema_version": "project.created/0.1",
                    "project": {
                        "project_id": request.project_id,
                        "canonical_root": request.canonical_root,
                        "root_identity": request.root_identity,
                        "display_name": request.display_name,
                        "state": "active",
                        "created_at_ms": request.created_at_ms,
                        "updated_at_ms": request.created_at_ms
                    },
                    "root": {
                        "project_id": request.project_id,
                        "root_id": request.root_id,
                        "canonical_root": request.canonical_root,
                        "root_identity": request.root_identity,
                        "access": request.root_access,
                        "created_at_ms": request.created_at_ms
                    }
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit project transaction"))?;
        self.load_project(&request.project_id)
    }

    pub fn migrate_project_root_identity(
        &mut self,
        project_id: &str,
        root_id: &str,
        root_identity: &str,
        updated_at_ms: u64,
    ) -> Result<StoredProject, WorkbenchStoreError> {
        validate_identifier(project_id, "project ID")?;
        validate_identifier(root_id, "project root ID")?;
        validate_identity_text(root_identity, "project root identity")?;
        self.ensure_project_writable(project_id)?;
        let timestamp = to_i64(updated_at_ms, "project identity migration time")?;
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .map_err(|_| error("cannot start project identity migration"))?;
        let binding: Option<(String, String, i64)> = transaction
            .query_row(
                "SELECT project.root_identity, project.canonical_root, project.updated_at_ms
                 FROM projects AS project WHERE project.project_id = ?1",
                [project_id],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )
            .optional()
            .map_err(|_| error("cannot read project identity migration binding"))?;
        let Some((old_identity, canonical_root, previous_updated_at)) = binding else {
            return Err(error("project does not exist"));
        };
        if old_identity == root_identity {
            drop(transaction);
            return self.load_project(project_id);
        }
        if timestamp < previous_updated_at {
            return Err(error("project identity migration time is stale"));
        }
        let root_exists = transaction
            .query_row(
                "SELECT EXISTS(
                    SELECT 1 FROM project_roots
                    WHERE project_id = ?1 AND root_id = ?2 AND canonical_root = ?3
                 )",
                params![project_id, root_id, canonical_root],
                |row| row.get::<_, bool>(0),
            )
            .map_err(|_| error("cannot validate project identity root"))?;
        if !root_exists {
            return Err(error("project identity root does not exist"));
        }
        transaction
            .execute(
                "UPDATE projects SET root_identity = ?1, updated_at_ms = ?2
                 WHERE project_id = ?3",
                params![root_identity, timestamp, project_id],
            )
            .map_err(|_| error("cannot migrate project root identity"))?;
        transaction
            .execute(
                "UPDATE project_roots SET root_identity = ?1
                 WHERE project_id = ?2 AND root_id = ?3",
                params![root_identity, project_id, root_id],
            )
            .map_err(|_| error("cannot migrate project root identity projection"))?;
        let stream_id = project_event_stream_id(project_id);
        let event_id = derived_event_id(
            "project-root-identity-migrated",
            format!("{project_id}\0{root_id}\0{root_identity}").as_bytes(),
        );
        append_event_tx(
            &transaction,
            EventInput {
                session_id: &stream_id,
                event_id: &event_id,
                timestamp_ms: updated_at_ms,
                correlation_id: project_id,
                event_kind: "project.root-identity-migrated",
                project_id: Some(project_id),
                operation_id: root_id,
                generation: 0,
                payload: json!({
                    "schema_version": "project.root-identity/0.1",
                    "project_id": project_id,
                    "root_id": root_id,
                    "root_identity": root_identity,
                    "updated_at_ms": updated_at_ms
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit project identity migration"))?;
        self.load_project(project_id)
    }

    pub fn add_project_root(
        &mut self,
        request: StoredProjectRootAdd,
    ) -> Result<StoredProjectRoot, WorkbenchStoreError> {
        validate_project_root(&StoredProjectRoot {
            project_id: request.project_id.clone(),
            root_id: request.root_id.clone(),
            canonical_root: request.canonical_root.clone(),
            root_identity: request.root_identity.clone(),
            access: request.access.clone(),
            created_at_ms: request.created_at_ms,
        })?;
        self.ensure_project_writable(&request.project_id)?;
        let canonical_root = canonical_store_root(&request.canonical_root)?;
        if canonical_root != Path::new(&request.canonical_root) {
            return Err(error("project root must already be canonical"));
        }
        let timestamp = to_i64(request.created_at_ms, "project root creation time")?;
        let stream_id = project_event_stream_id(&request.project_id);
        let transaction = self.begin_database_write("cannot start project root transaction")?;
        let project_updated_at: Option<i64> = transaction
            .query_row(
                "SELECT updated_at_ms FROM projects
                 WHERE project_id = ?1 AND state != 'archived'",
                [&request.project_id],
                |row| row.get(0),
            )
            .optional()
            .map_err(|_| error("cannot validate project root owner"))?;
        let Some(project_updated_at) = project_updated_at else {
            return Err(error("project does not exist or is archived"));
        };
        if timestamp < project_updated_at {
            return Err(error("project root creation time is stale"));
        }
        transaction
            .execute(
                "INSERT INTO project_roots (
                    project_id, root_id, canonical_root, root_identity, access, created_at_ms
                 ) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                params![
                    request.project_id,
                    request.root_id,
                    request.canonical_root,
                    request.root_identity,
                    request.access,
                    timestamp,
                ],
            )
            .map_err(|_| error("project root already exists or is invalid"))?;
        transaction
            .execute(
                "UPDATE projects SET updated_at_ms = ?1 WHERE project_id = ?2",
                params![timestamp, request.project_id],
            )
            .map_err(|_| error("cannot update project root owner"))?;
        let event_id = derived_event_id(
            "project-root-added",
            format!("{}\0{}", request.project_id, request.root_id).as_bytes(),
        );
        append_event_tx(
            &transaction,
            EventInput {
                session_id: &stream_id,
                event_id: &event_id,
                timestamp_ms: request.created_at_ms,
                correlation_id: &request.project_id,
                event_kind: "project.root-added",
                project_id: Some(&request.project_id),
                operation_id: &request.root_id,
                generation: 0,
                payload: json!({
                    "schema_version": "project.root-added/0.1",
                    "root": {
                        "project_id": request.project_id,
                        "root_id": request.root_id,
                        "canonical_root": request.canonical_root,
                        "root_identity": request.root_identity,
                        "access": request.access,
                        "created_at_ms": request.created_at_ms
                    },
                    "updated_at_ms": request.created_at_ms
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit project root transaction"))?;
        self.load_project_root(&request.project_id, &request.root_id)
    }

    pub fn relink_project_root(
        &mut self,
        request: StoredProjectRootRelink,
    ) -> Result<StoredProject, WorkbenchStoreError> {
        validate_identifier(&request.project_id, "project ID")?;
        validate_identifier(&request.root_id, "project root ID")?;
        validate_identity_text(&request.root_identity, "project root identity")?;
        validate_identity_text(
            &request.expected_root_identity,
            "expected project root identity",
        )?;
        let canonical_root = canonical_store_root(&request.canonical_root)?;
        if canonical_root != Path::new(&request.canonical_root) {
            return Err(error("project root must already be canonical"));
        }
        self.ensure_project_writable(&request.project_id)?;
        let timestamp = to_i64(request.updated_at_ms, "project relink time")?;
        let transaction = self.begin_database_write("cannot start project relink")?;
        let project_binding: Option<(String, String, i64)> = transaction
            .query_row(
                "SELECT canonical_root, root_identity, updated_at_ms
                 FROM projects WHERE project_id = ?1 AND state != 'archived'",
                [&request.project_id],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )
            .optional()
            .map_err(|_| error("cannot read project relink binding"))?;
        let Some((project_root, project_identity, project_updated_at)) = project_binding else {
            return Err(error("project does not exist or is archived"));
        };
        if timestamp < project_updated_at {
            return Err(error("project relink time is stale"));
        }
        let root_binding: Option<(String, String, String)> = transaction
            .query_row(
                "SELECT canonical_root, root_identity, access
                 FROM project_roots WHERE project_id = ?1 AND root_id = ?2",
                params![request.project_id, request.root_id],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )
            .optional()
            .map_err(|_| error("cannot read project root relink binding"))?;
        let Some((old_root, old_identity, access)) = root_binding else {
            return Err(error("project root does not exist"));
        };
        if old_identity != request.expected_root_identity {
            return Err(error("project root changed; relink review is stale"));
        }
        if request.root_id == "root-1"
            && (project_root != old_root || project_identity != old_identity)
        {
            return Err(error("primary project root projection is inconsistent"));
        }
        let occupied: bool = transaction
            .query_row(
                "SELECT EXISTS(
                    SELECT 1 FROM project_roots
                    WHERE NOT (project_id = ?1 AND root_id = ?2)
                      AND (canonical_root = ?3 OR root_identity = ?4)
                 )",
                params![
                    request.project_id,
                    request.root_id,
                    request.canonical_root,
                    request.root_identity
                ],
                |row| row.get(0),
            )
            .map_err(|_| error("cannot validate project relink target"))?;
        if occupied {
            return Err(error(
                "project relink target is already another project root",
            ));
        }
        transaction
            .execute(
                "UPDATE project_roots
                 SET canonical_root = ?1, root_identity = ?2
                 WHERE project_id = ?3 AND root_id = ?4",
                params![
                    request.canonical_root,
                    request.root_identity,
                    request.project_id,
                    request.root_id
                ],
            )
            .map_err(|_| error("cannot update project root relink projection"))?;
        if request.root_id == "root-1" {
            transaction
                .execute(
                    "UPDATE projects
                     SET canonical_root = ?1, root_identity = ?2, updated_at_ms = ?3
                     WHERE project_id = ?4",
                    params![
                        request.canonical_root,
                        request.root_identity,
                        timestamp,
                        request.project_id
                    ],
                )
                .map_err(|_| error("cannot update primary project relink projection"))?;
        } else {
            transaction
                .execute(
                    "UPDATE projects SET updated_at_ms = ?1 WHERE project_id = ?2",
                    params![timestamp, request.project_id],
                )
                .map_err(|_| error("cannot update project relink timestamp"))?;
        }
        let event_id = derived_event_id(
            "project-root-relinked",
            format!(
                "{}\0{}\0{}\0{}",
                request.project_id, request.root_id, request.canonical_root, request.root_identity
            )
            .as_bytes(),
        );
        let stream_id = project_event_stream_id(&request.project_id);
        append_event_tx(
            &transaction,
            EventInput {
                session_id: &stream_id,
                event_id: &event_id,
                timestamp_ms: request.updated_at_ms,
                correlation_id: &request.project_id,
                event_kind: "project.root-relinked",
                project_id: Some(&request.project_id),
                operation_id: &request.root_id,
                generation: 0,
                payload: json!({
                    "schema_version": "project.root-relinked/0.1",
                    "project_id": request.project_id,
                    "root_id": request.root_id,
                    "previous_canonical_root": old_root,
                    "previous_root_identity": old_identity,
                    "canonical_root": request.canonical_root,
                    "root_identity": request.root_identity,
                    "access": access,
                    "updated_at_ms": request.updated_at_ms
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit project relink"))?;
        self.load_project(&request.project_id)
    }

    pub fn remove_project_root(
        &mut self,
        project_id: &str,
        root_id: &str,
        updated_at_ms: u64,
    ) -> Result<StoredProject, WorkbenchStoreError> {
        validate_identifier(project_id, "project ID")?;
        validate_identifier(root_id, "project root ID")?;
        self.ensure_project_writable(project_id)?;
        if root_id == "root-1" {
            return Err(error("the primary project root cannot be removed"));
        }
        let timestamp = to_i64(updated_at_ms, "project root removal time")?;
        let stream_id = project_event_stream_id(project_id);
        let transaction = self.begin_database_write("cannot start project root removal")?;
        let project_updated_at: Option<i64> = transaction
            .query_row(
                "SELECT updated_at_ms FROM projects
                 WHERE project_id = ?1 AND state != 'archived'",
                [project_id],
                |row| row.get(0),
            )
            .optional()
            .map_err(|_| error("cannot validate project root owner"))?;
        let Some(project_updated_at) = project_updated_at else {
            return Err(error("project does not exist or is archived"));
        };
        if timestamp < project_updated_at {
            return Err(error("project root removal time is stale"));
        }
        let root_exists: bool = transaction
            .query_row(
                "SELECT EXISTS(
                    SELECT 1 FROM project_roots WHERE project_id = ?1 AND root_id = ?2
                 )",
                params![project_id, root_id],
                |row| row.get(0),
            )
            .map_err(|_| error("cannot validate project root"))?;
        if !root_exists {
            return Err(error("project root does not exist"));
        }
        let root_count: i64 = transaction
            .query_row(
                "SELECT COUNT(*) FROM project_roots WHERE project_id = ?1",
                [project_id],
                |row| row.get(0),
            )
            .map_err(|_| error("cannot count project roots"))?;
        if root_count <= 1 {
            return Err(error("a project must retain at least one root"));
        }
        transaction
            .execute(
                "DELETE FROM project_roots WHERE project_id = ?1 AND root_id = ?2",
                params![project_id, root_id],
            )
            .map_err(|_| error("cannot remove project root"))?;
        transaction
            .execute(
                "UPDATE projects SET updated_at_ms = ?1 WHERE project_id = ?2",
                params![timestamp, project_id],
            )
            .map_err(|_| error("cannot update project root owner"))?;
        let event_id = derived_event_id(
            "project-root-removed",
            format!("{project_id}\0{root_id}").as_bytes(),
        );
        append_event_tx(
            &transaction,
            EventInput {
                session_id: &stream_id,
                event_id: &event_id,
                timestamp_ms: updated_at_ms,
                correlation_id: project_id,
                event_kind: "project.root-removed",
                project_id: Some(project_id),
                operation_id: root_id,
                generation: 0,
                payload: json!({
                    "schema_version": "project.root-removed/0.1",
                    "project_id": project_id,
                    "root_id": root_id,
                    "updated_at_ms": updated_at_ms
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit project root removal"))?;
        self.load_project(project_id)
    }

    pub fn load_project(&self, project_id: &str) -> Result<StoredProject, WorkbenchStoreError> {
        validate_identifier(project_id, "project ID")?;
        self.connection
            .query_row(
                "SELECT project_id, canonical_root, root_identity, display_name, state,
                        created_at_ms, updated_at_ms
                 FROM projects WHERE project_id = ?1",
                [project_id],
                |row| {
                    Ok(StoredProject {
                        project_id: row.get(0)?,
                        canonical_root: row.get(1)?,
                        root_identity: row.get(2)?,
                        display_name: row.get(3)?,
                        state: row.get(4)?,
                        created_at_ms: to_u64_sql(row.get(5)?, "project creation time")?,
                        updated_at_ms: to_u64_sql(row.get(6)?, "project update time")?,
                    })
                },
            )
            .optional()
            .map_err(|_| error("cannot read project"))?
            .ok_or_else(|| error("project does not exist"))
    }

    pub fn acknowledge_project_trust_review(
        &mut self,
        request: StoredProjectTrustAcknowledge,
    ) -> Result<StoredProjectTrustAcknowledgement, WorkbenchStoreError> {
        validate_identifier(&request.project_id, "project ID")?;
        validate_identifier(&request.root_id, "project root ID")?;
        validate_identity_text(&request.root_identity, "project root identity")?;
        validate_trust_review_id(&request.review_id)?;
        self.ensure_project_writable(&request.project_id)?;
        let current_root = self.load_project_root(&request.project_id, &request.root_id)?;
        if current_root.root_identity != request.root_identity {
            return Err(error(
                "project trust review is stale after root identity changed",
            ));
        }
        if let Some(existing) =
            self.load_project_trust_acknowledgement(&request.project_id, &request.root_id)?
        {
            if existing.review_id == request.review_id
                && existing.root_identity == request.root_identity
            {
                return Ok(existing);
            }
        }

        let timestamp = to_i64(request.acknowledged_at_ms, "trust acknowledgement time")?;
        let transaction =
            self.begin_database_write("cannot start project trust acknowledgement")?;
        let root_identity: Option<String> = transaction
            .query_row(
                "SELECT root_identity FROM project_roots
                 WHERE project_id = ?1 AND root_id = ?2",
                params![request.project_id, request.root_id],
                |row| row.get(0),
            )
            .optional()
            .map_err(|_| error("cannot read project trust root binding"))?;
        let Some(root_identity) = root_identity else {
            return Err(error("project trust root does not exist"));
        };
        if root_identity != request.root_identity {
            return Err(error(
                "project trust review is stale after root identity changed",
            ));
        }
        let stream_id = project_event_stream_id(&request.project_id);
        let event_id = derived_event_id(
            "project-trust-acknowledged",
            format!(
                "{}\0{}\0{}\0{}",
                request.project_id, request.root_id, request.root_identity, request.review_id
            )
            .as_bytes(),
        );
        append_event_tx(
            &transaction,
            EventInput {
                session_id: &stream_id,
                event_id: &event_id,
                timestamp_ms: request.acknowledged_at_ms,
                correlation_id: &request.project_id,
                event_kind: "project.trust-acknowledged",
                project_id: Some(&request.project_id),
                operation_id: "trust-review",
                generation: 0,
                payload: json!({
                    "schema_version": "project.trust-acknowledged/0.1",
                    "project_id": request.project_id,
                    "root_id": request.root_id,
                    "root_identity": request.root_identity,
                    "review_id": request.review_id,
                    "acknowledged_at_ms": request.acknowledged_at_ms,
                    "permission_effect": "none-read-only-boundary-unchanged"
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit project trust acknowledgement"))?;
        Ok(StoredProjectTrustAcknowledgement {
            project_id: request.project_id,
            root_id: request.root_id,
            root_identity: request.root_identity,
            review_id: request.review_id,
            acknowledged_at_ms: u64::try_from(timestamp)
                .map_err(|_| error("trust acknowledgement time is invalid"))?,
        })
    }

    pub fn load_project_trust_acknowledgement(
        &self,
        project_id: &str,
        root_id: &str,
    ) -> Result<Option<StoredProjectTrustAcknowledgement>, WorkbenchStoreError> {
        validate_identifier(project_id, "project ID")?;
        validate_identifier(root_id, "project root ID")?;
        let stream_id = project_event_stream_id(project_id);
        let events = load_projection_source_events(&self.connection, &stream_id)?;
        for event in events.iter().rev() {
            if event.event_kind != "project.trust-acknowledged"
                || event.project_id.as_deref() != Some(project_id)
            {
                continue;
            }
            let acknowledgement =
                serde_json::from_value::<StoredProjectTrustAcknowledgement>(json!({
                    "project_id": event.payload.get("project_id"),
                    "root_id": event.payload.get("root_id"),
                    "root_identity": event.payload.get("root_identity"),
                    "review_id": event.payload.get("review_id"),
                    "acknowledged_at_ms": event.payload.get("acknowledged_at_ms")
                }))
                .map_err(|_| error("project trust acknowledgement event is invalid"))?;
            validate_identity_text(&acknowledgement.root_identity, "project root identity")?;
            validate_trust_review_id(&acknowledgement.review_id)?;
            if acknowledgement.project_id != project_id {
                return Err(error("project trust acknowledgement binding is invalid"));
            }
            if acknowledgement.root_id == root_id {
                return Ok(Some(acknowledgement));
            }
        }
        Ok(None)
    }

    pub fn list_projects(&self) -> Result<Vec<StoredProject>, WorkbenchStoreError> {
        let mut statement = self
            .connection
            .prepare(
                "SELECT project_id, canonical_root, root_identity, display_name, state,
                        created_at_ms, updated_at_ms
                 FROM projects ORDER BY updated_at_ms DESC, project_id ASC",
            )
            .map_err(|_| error("cannot prepare project listing"))?;
        let rows = statement
            .query_map([], |row| {
                Ok(StoredProject {
                    project_id: row.get(0)?,
                    canonical_root: row.get(1)?,
                    root_identity: row.get(2)?,
                    display_name: row.get(3)?,
                    state: row.get(4)?,
                    created_at_ms: to_u64_sql(row.get(5)?, "project creation time")?,
                    updated_at_ms: to_u64_sql(row.get(6)?, "project update time")?,
                })
            })
            .map_err(|_| error("cannot read project listing"))?;
        rows.collect::<Result<Vec<_>, _>>()
            .map_err(|_| error("project listing row is invalid"))
    }

    pub fn list_project_navigation(
        &self,
        limit: usize,
    ) -> Result<Vec<StoredProjectNavigationEntry>, WorkbenchStoreError> {
        if !(1..=256).contains(&limit) {
            return Err(error("project navigation limit must be between 1 and 256"));
        }
        let mut statement = self
            .connection
            .prepare(
                "SELECT project.project_id, project.canonical_root, project.root_identity,
                        project.display_name, project.state, project.created_at_ms,
                        project.updated_at_ms,
                        COALESCE(navigation.pinned, 0),
                        COALESCE(navigation.last_opened_at_ms, project.updated_at_ms),
                        (SELECT COUNT(*) FROM sessions AS session
                         WHERE session.project_id = project.project_id),
                        (SELECT COUNT(*) FROM sessions AS session
                         WHERE session.project_id = project.project_id
                           AND session.status = 'active'),
                        (SELECT COUNT(*) FROM turns AS turn
                         JOIN sessions AS session ON session.session_id = turn.session_id
                         WHERE session.project_id = project.project_id
                           AND turn.state IN ('started', 'running'))
                 FROM projects AS project
                 LEFT JOIN project_navigation AS navigation
                   ON navigation.project_id = project.project_id
                 ORDER BY COALESCE(navigation.pinned, 0) DESC,
                          COALESCE(navigation.last_opened_at_ms, project.updated_at_ms) DESC,
                          project.project_id ASC
                 LIMIT ?1",
            )
            .map_err(|_| error("cannot prepare project navigation listing"))?;
        let rows =
            statement
                .query_map(
                    [i64::try_from(limit)
                        .map_err(|_| error("project navigation limit is invalid"))?],
                    |row| {
                        let canonical_root: String = row.get(1)?;
                        let project = StoredProject {
                            project_id: row.get(0)?,
                            canonical_root: canonical_root.clone(),
                            root_identity: row.get(2)?,
                            display_name: row.get(3)?,
                            state: row.get(4)?,
                            created_at_ms: to_u64_sql(row.get(5)?, "project creation time")?,
                            updated_at_ms: to_u64_sql(row.get(6)?, "project update time")?,
                        };
                        let navigation = StoredProjectNavigation {
                            project_id: project.project_id.clone(),
                            pinned: row.get::<_, i64>(7)? != 0,
                            last_opened_at_ms: to_u64_sql(row.get(8)?, "project last-opened time")?,
                        };
                        let availability = if Path::new(&canonical_root).is_dir() {
                            "available"
                        } else {
                            "unavailable"
                        };
                        Ok(StoredProjectNavigationEntry {
                            project,
                            navigation,
                            availability: availability.into(),
                            relink_required: availability != "available",
                            session_count: to_u64_sql(row.get(9)?, "project session count")?,
                            active_session_count: to_u64_sql(row.get(10)?, "active session count")?,
                            live_session_count: to_u64_sql(row.get(11)?, "live session count")?,
                        })
                    },
                )
                .map_err(|_| error("cannot read project navigation listing"))?;
        rows.collect::<Result<Vec<_>, _>>()
            .map_err(|_| error("project navigation row is invalid"))
    }

    pub fn update_project_navigation(
        &mut self,
        project_id: &str,
        pinned: Option<bool>,
        last_opened_at_ms: Option<u64>,
    ) -> Result<StoredProjectNavigation, WorkbenchStoreError> {
        validate_identifier(project_id, "project ID")?;
        self.ensure_project_writable(project_id)?;
        if pinned.is_none() && last_opened_at_ms.is_none() {
            return Err(error("project navigation update is empty"));
        }
        let timestamp = last_opened_at_ms.unwrap_or_else(now_ms_for_store);
        let transaction = self.begin_database_write("cannot start project navigation update")?;
        let current: Option<(i64, i64)> = transaction
            .query_row(
                "SELECT navigation.pinned, navigation.last_opened_at_ms
                 FROM project_navigation AS navigation
                 WHERE navigation.project_id = ?1",
                [project_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()
            .map_err(|_| error("cannot read project navigation state"))?;
        let Some((current_pinned, current_last_opened)) = current else {
            return Err(error("project navigation state does not exist"));
        };
        let next_pinned = pinned.unwrap_or(current_pinned != 0);
        let next_last_opened =
            last_opened_at_ms.unwrap_or_else(|| u64::try_from(current_last_opened).unwrap_or(0));
        let next_last_opened = to_i64(next_last_opened, "project last-opened time")?;
        transaction
            .execute(
                "UPDATE project_navigation
                 SET pinned = ?2, last_opened_at_ms = ?3
                 WHERE project_id = ?1",
                params![project_id, i64::from(next_pinned), next_last_opened],
            )
            .map_err(|_| error("cannot update project navigation state"))?;
        let stream_id = project_event_stream_id(project_id);
        let next_event_sequence: i64 = transaction
            .query_row(
                "SELECT next_sequence FROM session_sequences WHERE session_id = ?1",
                [&stream_id],
                |row| row.get(0),
            )
            .map_err(|_| error("cannot read project navigation event sequence"))?;
        let event_id = derived_event_id(
            "project-navigation",
            format!("{project_id}:{next_event_sequence}:{next_pinned}:{next_last_opened}")
                .as_bytes(),
        );
        append_event_tx(
            &transaction,
            EventInput {
                session_id: &stream_id,
                event_id: &event_id,
                timestamp_ms: timestamp,
                correlation_id: project_id,
                event_kind: "project.navigation-updated",
                project_id: Some(project_id),
                operation_id: "project-navigation",
                generation: 0,
                payload: json!({
                    "schema_version": "project.navigation-updated/0.1",
                    "project_id": project_id,
                    "pinned": next_pinned,
                    "last_opened_at_ms": next_last_opened
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit project navigation update"))?;
        Ok(StoredProjectNavigation {
            project_id: project_id.into(),
            pinned: next_pinned,
            last_opened_at_ms: u64::try_from(next_last_opened)
                .map_err(|_| error("project last-opened time is invalid"))?,
        })
    }

    pub fn load_project_roots(
        &self,
        project_id: &str,
    ) -> Result<Vec<StoredProjectRoot>, WorkbenchStoreError> {
        validate_identifier(project_id, "project ID")?;
        let mut statement = self
            .connection
            .prepare(
                "SELECT project_id, root_id, canonical_root, root_identity, access,
                        created_at_ms
                 FROM project_roots WHERE project_id = ?1 ORDER BY root_id",
            )
            .map_err(|_| error("cannot prepare project root listing"))?;
        let rows = statement
            .query_map([project_id], stored_project_root_from_row)
            .map_err(|_| error("cannot read project root listing"))?;
        rows.collect::<Result<Vec<_>, _>>()
            .map_err(|_| error("project root listing row is invalid"))
    }

    fn load_project_root(
        &self,
        project_id: &str,
        root_id: &str,
    ) -> Result<StoredProjectRoot, WorkbenchStoreError> {
        self.connection
            .query_row(
                "SELECT project_id, root_id, canonical_root, root_identity, access,
                        created_at_ms
                 FROM project_roots WHERE project_id = ?1 AND root_id = ?2",
                params![project_id, root_id],
                stored_project_root_from_row,
            )
            .optional()
            .map_err(|_| error("cannot read project root"))?
            .ok_or_else(|| error("project root does not exist"))
    }

    pub fn create_session(
        &mut self,
        request: StoredSessionCreate,
    ) -> Result<StoredSession, WorkbenchStoreError> {
        self.create_session_with_bindings(request, None, None)
    }

    pub fn create_session_with_runtime_binding(
        &mut self,
        request: StoredSessionCreate,
        runtime_binding: Option<StoredSessionRuntimeBindingCreate>,
    ) -> Result<StoredSession, WorkbenchStoreError> {
        self.create_session_with_bindings(request, runtime_binding, None)
    }

    pub fn create_session_with_bindings(
        &mut self,
        request: StoredSessionCreate,
        runtime_binding: Option<StoredSessionRuntimeBindingCreate>,
        workspace_binding: Option<StoredSessionWorkspaceBindingCreate>,
    ) -> Result<StoredSession, WorkbenchStoreError> {
        validate_identifier(&request.session_id, "session ID")?;
        if let Some(project_id) = &request.project_id {
            validate_identifier(project_id, "session project ID")?;
            self.ensure_project_writable(project_id)?;
        }
        validate_text(&request.title, 256, "session title")?;
        if let Some(parent_session_id) = &request.parent_session_id {
            validate_identifier(parent_session_id, "parent session ID")?;
            self.ensure_session_writable(parent_session_id)?;
        }
        if request.mode == StoredSessionMode::Work && request.project_id.is_none() {
            return Err(error("Work session requires a project"));
        }
        match (request.parent_session_id.is_some(), request.lineage_kind) {
            (false, StoredSessionLineage::New)
            | (true, StoredSessionLineage::Resume | StoredSessionLineage::Fork) => {}
            _ => return Err(error("session lineage kind does not match parent session")),
        }
        if let Some(environment_identity) = &request.environment_identity {
            validate_identity_text(environment_identity, "session environment identity")?;
        }
        if let Some(binding) = runtime_binding.as_ref() {
            validate_session_runtime_binding_create(binding, &request.session_id)?;
            if binding.created_at_ms != request.created_at_ms {
                return Err(error(
                    "session runtime binding creation time does not match session",
                ));
            }
        }
        if let Some(binding) = workspace_binding.as_ref() {
            validate_session_workspace_binding_create(
                binding,
                &request.session_id,
                request.project_id.as_deref(),
            )?;
            if request.mode != StoredSessionMode::Work {
                return Err(error("Chat session cannot own a workspace binding"));
            }
            if binding.captured_at_ms != request.created_at_ms {
                return Err(error(
                    "session workspace binding capture time does not match session",
                ));
            }
        }
        let timestamp = to_i64(request.created_at_ms, "session creation time")?;
        let transaction = self.begin_database_write("cannot start session transaction")?;
        if let Some(project_id) = &request.project_id {
            let exists: Option<String> = transaction
                .query_row(
                    "SELECT project_id FROM projects WHERE project_id = ?1 AND state != 'archived'",
                    [project_id],
                    |row| row.get(0),
                )
                .optional()
                .map_err(|_| error("cannot validate session project"))?;
            if exists.is_none() {
                return Err(error("session project does not exist or is archived"));
            }
        }
        if let Some(binding) = workspace_binding.as_ref() {
            let root_identity: Option<String> = transaction
                .query_row(
                    "SELECT root_identity FROM project_roots
                     WHERE project_id = ?1 AND root_id = ?2",
                    params![binding.project_id, binding.root_id],
                    |row| row.get(0),
                )
                .optional()
                .map_err(|_| error("cannot validate session workspace root"))?;
            if root_identity.as_deref() != Some(binding.root_identity.as_str()) {
                return Err(error("session workspace root identity is stale"));
            }
        }
        if let Some(parent_session_id) = &request.parent_session_id {
            let parent: Option<(Option<String>, String)> = transaction
                .query_row(
                    "SELECT project_id, mode FROM sessions WHERE session_id = ?1",
                    [parent_session_id],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .optional()
                .map_err(|_| error("cannot validate parent session"))?;
            let Some((parent_project_id, parent_mode)) = parent else {
                return Err(error("parent session does not exist"));
            };
            if parent_project_id != request.project_id || parent_mode != request.mode.as_str() {
                return Err(error("session lineage is not project or mode bound"));
            }
        }
        transaction
            .execute(
                "INSERT INTO sessions (
                    session_id, project_id, mode, title, parent_session_id, lineage_kind,
                    status, environment_identity, created_at_ms, updated_at_ms
                 ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'active', ?7, ?8, ?8)",
                params![
                    request.session_id,
                    request.project_id,
                    request.mode.as_str(),
                    request.title,
                    request.parent_session_id,
                    request.lineage_kind.as_str(),
                    request.environment_identity,
                    timestamp,
                ],
            )
            .map_err(|_| error("session already exists or is invalid"))?;
        transaction
            .execute(
                "INSERT INTO session_projection_sources (
                    session_id, source_version, created_at_ms
                 ) VALUES (?1, 1, ?2)",
                params![request.session_id, timestamp],
            )
            .map_err(|_| error("cannot register session projection event source"))?;
        let event_id = derived_event_id("session-created", request.session_id.as_bytes());
        append_event_tx(
            &transaction,
            EventInput {
                session_id: &request.session_id,
                event_id: &event_id,
                timestamp_ms: request.created_at_ms,
                correlation_id: &request.session_id,
                event_kind: "session.created",
                project_id: request.project_id.as_deref(),
                operation_id: &request.session_id,
                generation: 0,
                payload: json!({
                    "schema_version": "session.created/0.1",
                    "session": {
                        "session_id": request.session_id,
                        "project_id": request.project_id,
                        "mode": request.mode,
                        "title": request.title,
                        "parent_session_id": request.parent_session_id,
                        "lineage_kind": request.lineage_kind,
                        "status": "active",
                        "environment_identity": request.environment_identity,
                        "created_at_ms": request.created_at_ms,
                        "updated_at_ms": request.created_at_ms
                    }
                }),
            },
        )?;
        if let Some(binding) = runtime_binding.as_ref() {
            insert_session_runtime_binding_tx(&transaction, binding)?;
            append_session_runtime_binding_event_tx(
                &transaction,
                binding,
                &request.session_id,
                request.project_id.as_deref(),
            )?;
        }
        if let Some(binding) = workspace_binding.as_ref() {
            insert_session_workspace_binding_tx(&transaction, binding)?;
            append_session_workspace_binding_event_tx(&transaction, binding)?;
        }
        transaction
            .commit()
            .map_err(|_| error("cannot commit session transaction"))?;
        self.load_session(&request.session_id)
    }

    pub fn load_session(&self, session_id: &str) -> Result<StoredSession, WorkbenchStoreError> {
        validate_identifier(session_id, "session ID")?;
        self.connection
            .query_row(
                "SELECT session_id, project_id, mode, title, parent_session_id,
                        lineage_kind, status, environment_identity, created_at_ms, updated_at_ms
                 FROM sessions WHERE session_id = ?1",
                [session_id],
                |row| {
                    Ok(StoredSession {
                        session_id: row.get(0)?,
                        project_id: row.get(1)?,
                        mode: parse_session_mode(&row.get::<_, String>(2)?)?,
                        title: row.get(3)?,
                        parent_session_id: row.get(4)?,
                        lineage_kind: parse_session_lineage(&row.get::<_, String>(5)?)?,
                        status: row.get(6)?,
                        environment_identity: row.get(7)?,
                        created_at_ms: to_u64_sql(row.get(8)?, "session creation time")?,
                        updated_at_ms: to_u64_sql(row.get(9)?, "session update time")?,
                    })
                },
            )
            .optional()
            .map_err(|_| error("cannot read session"))?
            .ok_or_else(|| error("session does not exist"))
    }

    pub fn load_readable_session(
        &self,
        session_id: &str,
    ) -> Result<StoredSession, WorkbenchStoreError> {
        self.ensure_session_readable(session_id)?;
        self.load_session(session_id)
    }

    pub fn create_background_job(
        &mut self,
        request: &BackgroundJobRequest,
        state: &BackgroundJobState,
    ) -> Result<StoredBackgroundJob, WorkbenchStoreError> {
        validate_background_job_contract(request, state)?;
        if state.status != BackgroundJobStatus::Queued
            || state.cancellation != JobCancellationState::NotRequested
            || state.generation != 0
            || !state.attempts.is_empty()
        {
            return Err(coded_error(
                "background-job-initial-state-invalid",
                "new background job is not in its initial queued state",
            ));
        }
        self.ensure_session_writable(&request.session_id)?;
        validate_background_job_relational_binding(&self.connection, request)?;
        let candidate = stored_background_job(request, state)?;
        if let Some(existing) = query_background_job_optional(&self.connection, &request.job_id)? {
            if existing == candidate {
                return Ok(existing);
            }
            return Err(coded_error(
                "background-job-idempotency-conflict",
                "background job identity already has different durable state",
            ));
        }
        if query_background_job_by_idempotency_optional(
            &self.connection,
            &request.session_id,
            &request.idempotency_identity,
        )?
        .is_some()
        {
            return Err(coded_error(
                "background-job-idempotency-conflict",
                "background job idempotency identity is already bound",
            ));
        }

        let transaction =
            self.begin_database_write("cannot start background job creation transaction")?;
        validate_background_job_relational_binding(&transaction, request)?;
        if let Some(existing) = query_background_job_optional(&transaction, &request.job_id)? {
            if existing == candidate {
                transaction
                    .commit()
                    .map_err(|_| error("cannot commit idempotent background job creation"))?;
                return Ok(existing);
            }
            return Err(coded_error(
                "background-job-idempotency-conflict",
                "background job identity changed under the write lock",
            ));
        }
        if query_background_job_by_idempotency_optional(
            &transaction,
            &request.session_id,
            &request.idempotency_identity,
        )?
        .is_some()
        {
            return Err(coded_error(
                "background-job-idempotency-conflict",
                "background job idempotency identity changed under the write lock",
            ));
        }
        let count = query_global_count(
            &transaction,
            "SELECT COUNT(*) FROM background_jobs",
            "cannot count durable background jobs",
        )?;
        if count >= MAX_BACKGROUND_JOBS as u64 {
            return Err(coded_error(
                "background-job-store-limit",
                "durable background job limit is exhausted",
            ));
        }
        insert_background_job_tx(&transaction, &candidate)?;
        append_background_job_event_tx(&transaction, None, &candidate)?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit background job creation"))?;
        self.load_background_job(&request.job_id)
    }

    pub fn update_background_job_state(
        &mut self,
        request: &BackgroundJobRequest,
        previous: &BackgroundJobState,
        next: &BackgroundJobState,
    ) -> Result<StoredBackgroundJob, WorkbenchStoreError> {
        validate_background_job_contract(request, previous)?;
        validate_background_job_contract(request, next)?;
        if next.created_at_ms != previous.created_at_ms
            || next.generation
                != previous.generation.checked_add(1).ok_or_else(|| {
                    coded_error(
                        "background-job-generation-exhausted",
                        "background job generation is exhausted",
                    )
                })?
            || next.updated_at_ms < previous.updated_at_ms
        {
            return Err(coded_error(
                "background-job-state-cas-invalid",
                "background job state update is not the next generation",
            ));
        }
        let event_kind = background_job_event_kind(previous, next)?;
        self.ensure_session_writable(&request.session_id)?;
        validate_background_job_relational_binding(&self.connection, request)?;
        let previous_record = stored_background_job(request, previous)?;
        let next_record = stored_background_job(request, next)?;
        let existing = query_background_job_optional(&self.connection, &request.job_id)?
            .ok_or_else(|| {
                coded_error(
                    "background-job-not-found",
                    "durable background job does not exist",
                )
            })?;
        if existing == next_record {
            return Ok(existing);
        }
        if existing != previous_record {
            return Err(coded_error(
                "background-job-state-stale",
                "durable background job state changed before update",
            ));
        }

        let transaction =
            self.begin_database_write("cannot start background job state transaction")?;
        validate_background_job_relational_binding(&transaction, request)?;
        let current =
            query_background_job_optional(&transaction, &request.job_id)?.ok_or_else(|| {
                coded_error(
                    "background-job-not-found",
                    "durable background job disappeared under the write lock",
                )
            })?;
        if current == next_record {
            transaction
                .commit()
                .map_err(|_| error("cannot commit idempotent background job update"))?;
            return Ok(current);
        }
        if current != previous_record {
            return Err(coded_error(
                "background-job-state-stale",
                "durable background job state changed under the write lock",
            ));
        }
        let changed = transaction
            .execute(
                "UPDATE background_jobs SET
                    state_identity = ?2, state_json = ?3, state_sha256 = ?4,
                    state_bytes = ?5, status = ?6, cancellation_state = ?7,
                    generation = ?8, attempt_count = ?9, next_eligible_at_ms = ?10,
                    updated_at_ms = ?11
                 WHERE job_id = ?1 AND generation = ?12 AND state_sha256 = ?13",
                params![
                    request.job_id,
                    next.identity(request)
                        .map_err(|cause| coded_error(cause.code, cause.message))?,
                    next_record_json(&next_record)?,
                    next_record.state_hash.sha256,
                    to_i64(next_record.state_hash.bytes, "background job state bytes")?,
                    next.status.as_str(),
                    next.cancellation.as_str(),
                    to_i64(next.generation, "background job generation")?,
                    to_i64(next.attempts.len() as u64, "background job attempt count")?,
                    to_i64(next.next_eligible_at_ms, "background job eligibility time")?,
                    to_i64(next.updated_at_ms, "background job update time")?,
                    to_i64(previous.generation, "previous background job generation")?,
                    previous_record.state_hash.sha256,
                ],
            )
            .map_err(|_| error("cannot update durable background job state"))?;
        if changed != 1 {
            return Err(coded_error(
                "background-job-state-stale",
                "durable background job state changed during update",
            ));
        }
        append_background_job_event_tx(&transaction, Some((previous, event_kind)), &next_record)?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit background job state update"))?;
        self.load_background_job(&request.job_id)
    }

    pub fn load_background_job(
        &self,
        job_id: &str,
    ) -> Result<StoredBackgroundJob, WorkbenchStoreError> {
        validate_identifier(job_id, "background job ID")?;
        query_background_job_optional(&self.connection, job_id)?.ok_or_else(|| {
            coded_error(
                "background-job-not-found",
                "durable background job does not exist",
            )
        })
    }

    pub fn load_background_jobs_for_recovery(
        &self,
        limit: usize,
    ) -> Result<Vec<StoredBackgroundJob>, WorkbenchStoreError> {
        if limit == 0 || limit > MAX_BACKGROUND_JOB_PAGE {
            return Err(coded_error(
                "background-job-page-limit",
                "background job recovery page limit is invalid",
            ));
        }
        let mut statement = self
            .connection
            .prepare(
                "SELECT job_id FROM background_jobs
                 WHERE status IN (
                    'queued','running','pause_requested','paused','waiting_approval',
                    'cancelling','failed','interrupted'
                 )
                 OR EXISTS (
                    SELECT 1 FROM background_job_leases AS lease
                    WHERE lease.job_id = background_jobs.job_id
                      AND lease.status = 'active'
                 )
                 ORDER BY next_eligible_at_ms, updated_at_ms, job_id LIMIT ?1",
            )
            .map_err(|_| error("cannot prepare background job recovery listing"))?;
        let job_ids = statement
            .query_map([limit as i64 + 1], |row| row.get::<_, String>(0))
            .map_err(|_| error("cannot read background job recovery listing"))?
            .collect::<Result<Vec<_>, _>>()
            .map_err(|_| error("background job recovery listing row is invalid"))?;
        drop(statement);
        if job_ids.len() > limit {
            return Err(coded_error(
                "background-job-recovery-truncated",
                "background job recovery listing exceeds the requested bound",
            ));
        }
        job_ids
            .iter()
            .map(|job_id| self.load_background_job(job_id))
            .collect()
    }

    pub fn create_background_scheduler_lease(
        &mut self,
        request: &BackgroundJobRequest,
        state: &BackgroundJobState,
        lease: &BackgroundSchedulerLease,
    ) -> Result<StoredBackgroundSchedulerLease, WorkbenchStoreError> {
        validate_background_scheduler_lease_contract(request, lease)?;
        lease
            .validate_for_state(request, state)
            .map_err(|cause| coded_error(cause.code, cause.message))?;
        if lease.lease_generation != 1 || lease.status != BackgroundSchedulerLeaseStatus::Active {
            return Err(coded_error(
                "background-lease-initial-state-invalid",
                "new background scheduler lease is not in its initial active state",
            ));
        }
        self.ensure_session_writable(&request.session_id)?;
        let expected_job = stored_background_job(request, state)?;
        if query_background_job_optional(&self.connection, &request.job_id)?.as_ref()
            != Some(&expected_job)
        {
            return Err(coded_error(
                "background-lease-job-state-stale",
                "background scheduler lease job state is stale",
            ));
        }
        let candidate = stored_background_scheduler_lease(lease)?;
        if let Some(existing) =
            query_background_scheduler_lease_optional(&self.connection, &request.job_id)?
        {
            if existing == candidate {
                return Ok(existing);
            }
            return Err(coded_error(
                "background-lease-conflict",
                "background job already has a different scheduler lease",
            ));
        }

        let transaction =
            self.begin_database_write("cannot start background lease creation transaction")?;
        if query_background_job_optional(&transaction, &request.job_id)?.as_ref()
            != Some(&expected_job)
        {
            return Err(coded_error(
                "background-lease-job-state-stale",
                "background scheduler lease job state changed under the write lock",
            ));
        }
        if let Some(existing) =
            query_background_scheduler_lease_optional(&transaction, &request.job_id)?
        {
            if existing == candidate {
                transaction
                    .commit()
                    .map_err(|_| error("cannot commit idempotent background lease creation"))?;
                return Ok(existing);
            }
            return Err(coded_error(
                "background-lease-conflict",
                "background scheduler lease changed under the write lock",
            ));
        }
        insert_background_scheduler_lease_tx(&transaction, &candidate)?;
        append_background_scheduler_lease_event_tx(&transaction, None, &candidate)?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit background lease creation"))?;
        self.load_background_scheduler_lease(&request.job_id)?
            .ok_or_else(|| error("background scheduler lease disappeared after creation"))
    }

    pub fn update_background_scheduler_lease(
        &mut self,
        request: &BackgroundJobRequest,
        current_state: &BackgroundJobState,
        previous: &BackgroundSchedulerLease,
        next: &BackgroundSchedulerLease,
    ) -> Result<StoredBackgroundSchedulerLease, WorkbenchStoreError> {
        validate_background_scheduler_lease_contract(request, previous)?;
        validate_background_scheduler_lease_contract(request, next)?;
        if next.status == BackgroundSchedulerLeaseStatus::Active {
            next.validate_for_state(request, current_state)
                .map_err(|cause| coded_error(cause.code, cause.message))?;
        }
        if next.lease_generation
            != previous.lease_generation.checked_add(1).ok_or_else(|| {
                coded_error(
                    "background-lease-generation-exhausted",
                    "background scheduler lease generation is exhausted",
                )
            })?
            || next.acquired_at_ms != previous.acquired_at_ms
            || next.owner_identity != previous.owner_identity
            || next.job_id != previous.job_id
            || next.request_identity != previous.request_identity
            || next.updated_at_ms < previous.updated_at_ms
        {
            return Err(coded_error(
                "background-lease-cas-invalid",
                "background scheduler lease update is not the next generation",
            ));
        }
        let event_kind = background_scheduler_lease_event_kind(previous, next)?;
        self.ensure_session_writable(&request.session_id)?;
        let expected_job = stored_background_job(request, current_state)?;
        if query_background_job_optional(&self.connection, &request.job_id)?.as_ref()
            != Some(&expected_job)
        {
            return Err(coded_error(
                "background-lease-job-state-stale",
                "background scheduler lease job state is stale",
            ));
        }
        let previous_record = stored_background_scheduler_lease(previous)?;
        let next_record = stored_background_scheduler_lease(next)?;
        let existing =
            query_background_scheduler_lease_optional(&self.connection, &request.job_id)?
                .ok_or_else(|| {
                    coded_error(
                        "background-lease-not-found",
                        "durable background scheduler lease does not exist",
                    )
                })?;
        if existing == next_record {
            return Ok(existing);
        }
        if existing != previous_record {
            return Err(coded_error(
                "background-lease-stale",
                "durable background scheduler lease changed before update",
            ));
        }

        let transaction =
            self.begin_database_write("cannot start background lease update transaction")?;
        if query_background_job_optional(&transaction, &request.job_id)?.as_ref()
            != Some(&expected_job)
        {
            return Err(coded_error(
                "background-lease-job-state-stale",
                "background scheduler lease job state changed under the write lock",
            ));
        }
        let current = query_background_scheduler_lease_optional(&transaction, &request.job_id)?
            .ok_or_else(|| {
                coded_error(
                    "background-lease-not-found",
                    "durable background scheduler lease disappeared under the write lock",
                )
            })?;
        if current == next_record {
            transaction
                .commit()
                .map_err(|_| error("cannot commit idempotent background lease update"))?;
            return Ok(current);
        }
        if current != previous_record {
            return Err(coded_error(
                "background-lease-stale",
                "durable background scheduler lease changed under the write lock",
            ));
        }
        update_background_scheduler_lease_tx(&transaction, &previous_record, &next_record)?;
        append_background_scheduler_lease_event_tx(
            &transaction,
            Some((previous, event_kind)),
            &next_record,
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit background lease update"))?;
        self.load_background_scheduler_lease(&request.job_id)?
            .ok_or_else(|| error("background scheduler lease disappeared after update"))
    }

    pub fn load_background_scheduler_lease(
        &self,
        job_id: &str,
    ) -> Result<Option<StoredBackgroundSchedulerLease>, WorkbenchStoreError> {
        query_background_scheduler_lease_optional(&self.connection, job_id)
    }

    pub fn append_background_recovery_decision(
        &mut self,
        snapshot: &BackgroundSchedulerSnapshot,
        job_id: &str,
        recorded_at_ms: u64,
    ) -> Result<StoredBackgroundRecoveryDecision, WorkbenchStoreError> {
        let decision = BackgroundRecoveryDecision::from_snapshot(snapshot, job_id, recorded_at_ms)
            .map_err(|cause| coded_error(cause.code, cause.message))?;
        self.ensure_session_writable(&decision.session_id)?;
        validate_current_background_recovery_evidence(&self.connection, &decision)?;
        if let Some(existing) = self.latest_background_recovery_decision(job_id)? {
            if same_background_recovery_evidence(&existing.decision, &decision) {
                return Ok(existing);
            }
        }

        let transaction =
            self.begin_database_write("cannot start background recovery decision transaction")?;
        validate_current_background_recovery_evidence(&transaction, &decision)?;
        let count = query_global_count(
            &transaction,
            "SELECT COUNT(*) FROM events
             WHERE event_kind = 'background-job.recovery-reviewed'",
            "cannot count background recovery decisions",
        )?;
        if count >= MAX_BACKGROUND_RECOVERY_DECISIONS as u64 {
            return Err(coded_error(
                "background-recovery-decision-limit",
                "background recovery decision journal reached its durable limit",
            ));
        }
        if let Some(existing) = query_latest_background_recovery_decision(&transaction, job_id)? {
            if same_background_recovery_evidence(&existing.decision, &decision) {
                transaction
                    .commit()
                    .map_err(|_| error("cannot commit idempotent background recovery decision"))?;
                return Ok(existing);
            }
        }
        let event_id = derived_event_id(
            "background-job-recovery-reviewed",
            decision.decision_identity.as_bytes(),
        );
        let event = append_event_tx(
            &transaction,
            EventInput {
                session_id: &decision.session_id,
                event_id: &event_id,
                timestamp_ms: decision.recorded_at_ms,
                correlation_id: &decision.decision_identity,
                event_kind: "background-job.recovery-reviewed",
                project_id: Some(&decision.project_id),
                operation_id: &decision.job_id,
                generation: decision.scheduler_generation,
                payload: json!({
                    "schema_version": "background-job.recovery-reviewed/0.1",
                    "decision": decision,
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit background recovery decision"))?;
        Ok(StoredBackgroundRecoveryDecision {
            schema_version: "stored-background-recovery-decision/0.1".into(),
            decision,
            event_sequence: event.sequence,
        })
    }

    pub fn latest_background_recovery_decision(
        &self,
        job_id: &str,
    ) -> Result<Option<StoredBackgroundRecoveryDecision>, WorkbenchStoreError> {
        query_latest_background_recovery_decision(&self.connection, job_id)
    }

    pub fn load_background_recovery_decisions(
        &self,
    ) -> Result<Vec<StoredBackgroundRecoveryDecision>, WorkbenchStoreError> {
        let mut statement = self
            .connection
            .prepare(
                "SELECT event.session_id, event.sequence
                 FROM events AS event
                 WHERE event.event_kind = 'background-job.recovery-reviewed'
                   AND NOT EXISTS (
                       SELECT 1 FROM events AS newer
                       WHERE newer.session_id = event.session_id
                         AND newer.event_kind = 'background-job.recovery-reviewed'
                         AND newer.operation_id = event.operation_id
                         AND newer.sequence > event.sequence
                   )
                 ORDER BY event.sequence ASC
                 LIMIT ?1",
            )
            .map_err(|_| error("cannot prepare background recovery decision scan"))?;
        let rows = statement
            .query_map([MAX_BACKGROUND_RECOVERY_DECISIONS as i64 + 1], |row| {
                Ok((row.get::<_, String>(0)?, row.get::<_, i64>(1)?))
            })
            .map_err(|_| error("cannot read background recovery decision scan"))?;
        let mut events = Vec::new();
        for row in rows {
            let (session_id, sequence) =
                row.map_err(|_| error("background recovery decision row is invalid"))?;
            events.push((
                session_id,
                to_u64(sequence, "background recovery decision event sequence")?,
            ));
        }
        drop(statement);
        if events.len() > MAX_BACKGROUND_RECOVERY_DECISIONS {
            return Err(coded_error(
                "background-recovery-decision-limit",
                "background recovery decision scan exceeds its bound",
            ));
        }
        events
            .into_iter()
            .map(|(session_id, sequence)| {
                let event = self
                    .read_session_events(&session_id, sequence.saturating_sub(1), 1)?
                    .into_iter()
                    .next()
                    .ok_or_else(|| error("background recovery decision event is unavailable"))?;
                let stored = parse_background_recovery_decision_event(&event)?;
                validate_historical_background_recovery_binding(
                    &self.connection,
                    &stored.decision,
                )?;
                Ok(stored)
            })
            .collect()
    }

    pub fn enqueue_background_notification(
        &mut self,
        intent: &BackgroundNotificationIntent,
        recorded_at_ms: u64,
    ) -> Result<StoredBackgroundNotification, WorkbenchStoreError> {
        intent
            .validate()
            .map_err(|cause| coded_error(cause.code, cause.message))?;
        if recorded_at_ms == 0 || recorded_at_ms < intent.created_at_ms {
            return Err(coded_error(
                "background-notification-record-time-invalid",
                "background notification record time precedes its intent",
            ));
        }
        self.ensure_session_writable(&intent.session_id)?;
        if let Some(existing) = query_background_notification_by_dedup_optional(
            &self.connection,
            &intent.deduplication_identity,
        )? {
            if same_background_notification_evidence(&existing.intent, intent) {
                return Ok(existing);
            }
            return Err(coded_error(
                "background-notification-dedup-conflict",
                "background notification deduplication identity conflicts",
            ));
        }
        validate_current_background_notification_binding(&self.connection, intent)?;

        let (intent_json, intent_hash) = background_notification_json(intent)?;
        let transaction =
            self.begin_database_write("cannot start background notification transaction")?;
        if let Some(existing) = query_background_notification_by_dedup_optional(
            &transaction,
            &intent.deduplication_identity,
        )? {
            if same_background_notification_evidence(&existing.intent, intent) {
                transaction
                    .commit()
                    .map_err(|_| error("cannot commit idempotent background notification"))?;
                return Ok(existing);
            }
            return Err(coded_error(
                "background-notification-dedup-conflict",
                "background notification deduplication identity conflicts under the write lock",
            ));
        }
        validate_current_background_notification_binding(&transaction, intent)?;
        let count = query_global_count(
            &transaction,
            "SELECT COUNT(*) FROM background_notification_outbox",
            "cannot count background notification outbox",
        )?;
        if count >= MAX_BACKGROUND_NOTIFICATIONS as u64 {
            return Err(coded_error(
                "background-notification-outbox-limit",
                "background notification outbox reached its durable limit",
            ));
        }
        let event_id = derived_event_id(
            "background-job-notification-recorded",
            intent.deduplication_identity.as_bytes(),
        );
        let event = append_event_tx(
            &transaction,
            EventInput {
                session_id: &intent.session_id,
                event_id: &event_id,
                timestamp_ms: recorded_at_ms,
                correlation_id: &intent.deduplication_identity,
                event_kind: "background-job.notification-recorded",
                project_id: Some(&intent.project_id),
                operation_id: &intent.job_id,
                generation: intent.job_generation,
                payload: json!({
                    "schema_version": "background-job.notification-recorded/0.1",
                    "intent": intent,
                    "intent_hash": intent_hash,
                    "delivery_state": "recorded",
                    "delivery_attempt_count": 0,
                    "content_included": false,
                    "delivery_available": false,
                    "delivery_attempted": false,
                    "platform_delivery_authority": false,
                    "recorded_at_ms": recorded_at_ms,
                }),
            },
        )?;
        insert_background_notification_tx(
            &transaction,
            intent,
            &intent_json,
            &intent_hash,
            event.sequence,
            recorded_at_ms,
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit background notification"))?;
        self.load_background_notification(&intent.intent_identity)
    }

    pub fn load_background_notification(
        &self,
        intent_identity: &str,
    ) -> Result<StoredBackgroundNotification, WorkbenchStoreError> {
        query_background_notification_optional(&self.connection, intent_identity)?.ok_or_else(
            || {
                coded_error(
                    "background-notification-not-found",
                    "background notification does not exist",
                )
            },
        )
    }

    pub fn inspect_background_notifications(
        &self,
        session_id: &str,
        cursor: Option<&BackgroundNotificationCursor>,
        limit: usize,
    ) -> Result<BackgroundNotificationPage, WorkbenchStoreError> {
        validate_identifier(session_id, "background notification session ID")?;
        self.load_session(session_id)?;
        if limit == 0 || limit > MAX_BACKGROUND_NOTIFICATION_PAGE {
            return Err(coded_error(
                "background-notification-page-limit",
                "background notification page limit is invalid",
            ));
        }
        if let Some(cursor) = cursor {
            validate_background_notification_cursor(cursor)?;
            let anchor = self.load_background_notification(&cursor.intent_identity)?;
            if anchor.intent.session_id != session_id
                || anchor.recorded_at_ms != cursor.recorded_at_ms
            {
                return Err(coded_error(
                    "background-notification-cursor-invalid",
                    "background notification cursor does not bind this session",
                ));
            }
        }

        let mut intent_ids = if let Some(cursor) = cursor {
            let mut statement = self
                .connection
                .prepare(
                    "SELECT intent_identity FROM background_notification_outbox
                     WHERE session_id = ?1
                       AND (
                           recorded_at_ms < ?2 OR
                           (recorded_at_ms = ?2 AND intent_identity > ?3)
                       )
                     ORDER BY recorded_at_ms DESC, intent_identity ASC LIMIT ?4",
                )
                .map_err(|_| error("cannot prepare background notification page"))?;
            let rows = statement
                .query_map(
                    params![
                        session_id,
                        to_i64(cursor.recorded_at_ms, "background notification cursor time")?,
                        cursor.intent_identity,
                        limit as i64 + 1,
                    ],
                    |row| row.get::<_, String>(0),
                )
                .map_err(|_| error("cannot read background notification page"))?;
            rows.collect::<Result<Vec<_>, _>>()
                .map_err(|_| error("background notification page row is invalid"))?
        } else {
            let mut statement = self
                .connection
                .prepare(
                    "SELECT intent_identity FROM background_notification_outbox
                     WHERE session_id = ?1
                     ORDER BY recorded_at_ms DESC, intent_identity ASC LIMIT ?2",
                )
                .map_err(|_| error("cannot prepare background notification page"))?;
            let rows = statement
                .query_map(params![session_id, limit as i64 + 1], |row| {
                    row.get::<_, String>(0)
                })
                .map_err(|_| error("cannot read background notification page"))?;
            rows.collect::<Result<Vec<_>, _>>()
                .map_err(|_| error("background notification page row is invalid"))?
        };
        let has_more = intent_ids.len() > limit;
        if has_more {
            intent_ids.truncate(limit);
        }
        let notifications = intent_ids
            .iter()
            .map(|intent_identity| self.load_background_notification(intent_identity))
            .collect::<Result<Vec<_>, _>>()?;
        let next_cursor = has_more.then(|| {
            let last = notifications
                .last()
                .expect("a truncated notification page cannot be empty");
            BackgroundNotificationCursor {
                recorded_at_ms: last.recorded_at_ms,
                intent_identity: last.intent.intent_identity.clone(),
            }
        });
        Ok(BackgroundNotificationPage {
            schema_version: "background-notification-page/0.1".into(),
            session_id: session_id.into(),
            notifications,
            next_cursor,
            content_included: false,
            delivery_mutation_available: false,
            platform_delivery_authority: false,
        })
    }

    pub fn load_session_runtime_binding(
        &self,
        session_id: &str,
    ) -> Result<StoredSessionRuntimeBinding, WorkbenchStoreError> {
        validate_identifier(session_id, "runtime binding session ID")?;
        self.connection
            .query_row(
                "SELECT session_id, adapter, adapter_version, backend_session_id,
                        provider, model, permission_profile, created_at_ms, updated_at_ms
                 FROM session_runtime_bindings WHERE session_id = ?1",
                [session_id],
                |row| {
                    Ok(StoredSessionRuntimeBinding {
                        session_id: row.get(0)?,
                        adapter: row.get(1)?,
                        adapter_version: row.get(2)?,
                        backend_session_id: row.get(3)?,
                        provider: row.get(4)?,
                        model: row.get(5)?,
                        permission_profile: row.get(6)?,
                        created_at_ms: to_u64_sql(row.get(7)?, "runtime binding creation time")?,
                        updated_at_ms: to_u64_sql(row.get(8)?, "runtime binding update time")?,
                    })
                },
            )
            .optional()
            .map_err(|_| error("cannot read session runtime binding"))?
            .ok_or_else(|| error("session runtime binding does not exist"))
    }

    pub fn load_session_workspace_binding(
        &self,
        session_id: &str,
    ) -> Result<StoredSessionWorkspaceBinding, WorkbenchStoreError> {
        validate_identifier(session_id, "workspace binding session ID")?;
        let binding = self
            .connection
            .query_row(
                "SELECT session_id, project_id, root_id, root_identity, workspace_kind,
                        git_state, repository_root_identity, worktree_root_identity,
                        branch, branch_sha256, branch_redacted, head_oid, detached,
                        unborn, dedicated_worktree, captured_at_ms, updated_at_ms
                 FROM session_workspace_bindings WHERE session_id = ?1",
                [session_id],
                stored_session_workspace_binding_from_row,
            )
            .optional()
            .map_err(|_| error("cannot read session workspace binding"))?
            .ok_or_else(|| error("session workspace binding does not exist"))?;
        validate_stored_session_workspace_binding(&binding)?;
        Ok(binding)
    }

    pub fn record_session_resume(
        &mut self,
        session_id: &str,
        environment_identity: &str,
        resumed_at_ms: u64,
    ) -> Result<StoredSession, WorkbenchStoreError> {
        validate_identifier(session_id, "session ID")?;
        validate_identity_text(environment_identity, "resumed environment identity")?;
        self.ensure_session_writable(session_id)?;
        let project_id = self.load_session(session_id)?.project_id;
        let timestamp = to_i64(resumed_at_ms, "session resume time")?;
        let transaction = self.begin_database_write("cannot start session resume")?;
        let previous: Option<(String, i64)> = transaction
            .query_row(
                "SELECT COALESCE(environment_identity, ''), updated_at_ms
                 FROM sessions WHERE session_id = ?1",
                [session_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()
            .map_err(|_| error("cannot read session resume state"))?;
        let Some((previous_environment, previous_updated_at)) = previous else {
            return Err(error("session does not exist"));
        };
        if timestamp < previous_updated_at {
            return Err(error("session resume time is stale"));
        }
        transaction
            .execute(
                "UPDATE sessions
                 SET environment_identity = ?2, updated_at_ms = ?3
                 WHERE session_id = ?1",
                params![session_id, environment_identity, timestamp],
            )
            .map_err(|_| error("cannot update resumed session"))?;
        transaction
            .execute(
                "UPDATE session_runtime_bindings SET updated_at_ms = ?2
                 WHERE session_id = ?1",
                params![session_id, timestamp],
            )
            .map_err(|_| error("cannot update resumed runtime binding"))?;
        let event_id = derived_event_id(
            "session-resumed",
            format!("{session_id}:{timestamp}").as_bytes(),
        );
        append_event_tx(
            &transaction,
            EventInput {
                session_id,
                event_id: &event_id,
                timestamp_ms: resumed_at_ms,
                correlation_id: session_id,
                event_kind: "session.resumed",
                project_id: project_id.as_deref(),
                operation_id: session_id,
                generation: 0,
                payload: json!({
                    "schema_version": "session.resumed/0.1",
                    "session_id": session_id,
                    "environment_identity": environment_identity,
                    "environment_changed": previous_environment != environment_identity,
                    "updated_at_ms": resumed_at_ms
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit session resume"))?;
        self.load_session(session_id)
    }

    pub fn list_sessions(
        &self,
        project_id: Option<&str>,
        mode: Option<StoredSessionMode>,
        include_archived: bool,
        limit: usize,
    ) -> Result<Vec<StoredSession>, WorkbenchStoreError> {
        if let Some(project_id) = project_id {
            validate_identifier(project_id, "session project filter")?;
        }
        if limit == 0 || limit > MAX_EVENT_PAGE {
            return Err(error("session list limit is invalid"));
        }
        let limit = i64::try_from(limit).map_err(|_| error("session list limit is invalid"))?;
        let mode = mode.map(StoredSessionMode::as_str);
        let mut statement = self
            .connection
            .prepare(
                "SELECT session_id, project_id, mode, title, parent_session_id,
                        lineage_kind, status, environment_identity, created_at_ms, updated_at_ms
                 FROM sessions
                 WHERE (?1 IS NULL OR project_id = ?1)
                   AND (?2 IS NULL OR mode = ?2)
                   AND (?3 OR status != 'archived')
                   AND NOT EXISTS (
                       SELECT 1
                       FROM session_deletion_members AS deletion_member
                       JOIN session_deletions AS deletion
                         ON deletion.deletion_id = deletion_member.deletion_id
                       WHERE deletion_member.session_id = sessions.session_id
                         AND deletion.state = 'purged'
                   )
                 ORDER BY updated_at_ms DESC, session_id ASC
                 LIMIT ?4",
            )
            .map_err(|_| error("cannot prepare session listing"))?;
        let rows = statement
            .query_map(params![project_id, mode, include_archived, limit], |row| {
                Ok(StoredSession {
                    session_id: row.get(0)?,
                    project_id: row.get(1)?,
                    mode: parse_session_mode(&row.get::<_, String>(2)?)?,
                    title: row.get(3)?,
                    parent_session_id: row.get(4)?,
                    lineage_kind: parse_session_lineage(&row.get::<_, String>(5)?)?,
                    status: row.get(6)?,
                    environment_identity: row.get(7)?,
                    created_at_ms: to_u64_sql(row.get(8)?, "session creation time")?,
                    updated_at_ms: to_u64_sql(row.get(9)?, "session update time")?,
                })
            })
            .map_err(|_| error("cannot read session listing"))?;
        rows.collect::<Result<Vec<_>, _>>()
            .map_err(|_| error("session listing row is invalid"))
    }

    pub fn search_sessions(
        &self,
        request: &SessionSearchRequest,
    ) -> Result<SessionSearchPage, WorkbenchStoreError> {
        if let Some(project_id) = request.project_id.as_deref() {
            validate_identifier(project_id, "session search project filter")?;
        }
        for (term, label) in [
            (request.branch.as_deref(), "session search branch filter"),
            (request.model.as_deref(), "session search model filter"),
            (request.runtime.as_deref(), "session search runtime filter"),
            (request.status.as_deref(), "session search status filter"),
            (request.title.as_deref(), "session search title query"),
            (request.text.as_deref(), "session search transcript query"),
        ] {
            if let Some(term) = term {
                validate_session_search_term(term, label)?;
            }
        }
        if request.status.as_deref().is_some_and(|status| {
            !matches!(status, "active" | "archived" | "failed" | "interrupted")
        }) {
            return Err(error("session search status filter is invalid"));
        }
        if request.limit == 0 || request.limit > MAX_SESSION_SEARCH_LIMIT {
            return Err(error("session search limit is invalid"));
        }
        let (cursor_updated_at, cursor_session_id) = request
            .cursor
            .as_deref()
            .map(parse_session_search_cursor)
            .transpose()?
            .map_or((None, None), |(timestamp, session_id)| {
                (Some(timestamp), Some(session_id))
            });
        let title_pattern = request.title.as_deref().map(session_search_like_pattern);
        let text_pattern = request.text.as_deref().map(session_search_like_pattern);
        let branch_sha256 = request
            .branch
            .as_deref()
            .map(|branch| ContentHash::for_bytes(branch.as_bytes()).sha256);
        let limit = i64::try_from(request.limit.saturating_add(1))
            .map_err(|_| error("session search limit is invalid"))?;
        let mut statement = self
            .connection
            .prepare(
                "SELECT s.session_id, s.project_id, s.mode, s.title,
                        s.parent_session_id, s.lineage_kind, s.status,
                        s.environment_identity, s.created_at_ms, s.updated_at_ms,
                        rb.session_id, rb.adapter, rb.adapter_version,
                        rb.backend_session_id, rb.provider, rb.model,
                        rb.permission_profile, rb.created_at_ms, rb.updated_at_ms,
                        wb.session_id, wb.project_id, wb.root_id, wb.root_identity,
                        wb.workspace_kind, wb.git_state, wb.repository_root_identity,
                        wb.worktree_root_identity, wb.branch, wb.branch_sha256,
                        wb.branch_redacted, wb.head_oid, wb.detached, wb.unborn,
                        wb.dedicated_worktree, wb.captured_at_ms, wb.updated_at_ms,
                        CASE WHEN ?7 IS NULL THEN 0 ELSE EXISTS(
                            SELECT 1 FROM items AS transcript
                            WHERE transcript.session_id = s.session_id
                              AND transcript.item_kind = 'message'
                              AND transcript.role IN ('user', 'assistant')
                              AND (
                                  lower(COALESCE(json_extract(transcript.payload_json, '$.text'), ''))
                                      LIKE lower(?7) ESCAPE '\\'
                                  OR lower(COALESCE(json_extract(transcript.payload_json, '$.content'), ''))
                                      LIKE lower(?7) ESCAPE '\\'
                                  OR lower(COALESCE(json_extract(transcript.payload_json, '$.output'), ''))
                                      LIKE lower(?7) ESCAPE '\\'
                                  OR lower(COALESCE(json_extract(transcript.payload_json, '$.diff'), ''))
                                      LIKE lower(?7) ESCAPE '\\'
                              )
                        ) END AS text_match
                 FROM sessions AS s
                 LEFT JOIN session_runtime_bindings AS rb
                   ON rb.session_id = s.session_id
                 LEFT JOIN session_workspace_bindings AS wb
                   ON wb.session_id = s.session_id
                 WHERE (?1 IS NULL OR s.project_id = ?1)
                   AND (?2 IS NULL OR wb.branch_sha256 = ?2)
                   AND (?3 IS NULL OR rb.model = ?3)
                   AND (?4 IS NULL OR rb.adapter = ?4 OR rb.adapter_version = ?4
                        OR rb.provider = ?4)
                   AND (?5 IS NULL OR s.status = ?5)
                   AND (?6 IS NULL OR lower(s.title) LIKE lower(?6) ESCAPE '\\')
                   AND (?7 IS NULL OR lower(s.title) LIKE lower(?7) ESCAPE '\\'
                        OR EXISTS(
                       SELECT 1 FROM items AS transcript_filter
                       WHERE transcript_filter.session_id = s.session_id
                         AND transcript_filter.item_kind = 'message'
                         AND transcript_filter.role IN ('user', 'assistant')
                         AND (
                             lower(COALESCE(json_extract(transcript_filter.payload_json, '$.text'), ''))
                                 LIKE lower(?7) ESCAPE '\\'
                             OR lower(COALESCE(json_extract(transcript_filter.payload_json, '$.content'), ''))
                                 LIKE lower(?7) ESCAPE '\\'
                             OR lower(COALESCE(json_extract(transcript_filter.payload_json, '$.output'), ''))
                                 LIKE lower(?7) ESCAPE '\\'
                             OR lower(COALESCE(json_extract(transcript_filter.payload_json, '$.diff'), ''))
                                 LIKE lower(?7) ESCAPE '\\'
                         )
                   ))
                   AND (?8 OR ?5 = 'archived' OR s.status != 'archived')
                   AND (?9 IS NULL OR s.updated_at_ms < ?9
                        OR (s.updated_at_ms = ?9 AND s.session_id > ?10))
                   AND NOT EXISTS (
                       SELECT 1
                       FROM session_deletion_members AS deletion_member
                       JOIN session_deletions AS deletion
                         ON deletion.deletion_id = deletion_member.deletion_id
                       WHERE deletion_member.session_id = s.session_id
                         AND deletion.state = 'purged'
                   )
                 ORDER BY s.updated_at_ms DESC, s.session_id ASC
                 LIMIT ?11",
            )
            .map_err(|_| error("cannot prepare session search"))?;
        let rows = statement
            .query_map(
                params![
                    request.project_id.as_deref(),
                    branch_sha256.as_deref(),
                    request.model.as_deref(),
                    request.runtime.as_deref(),
                    request.status.as_deref(),
                    title_pattern,
                    text_pattern,
                    request.include_archived,
                    cursor_updated_at,
                    cursor_session_id,
                    limit,
                ],
                |row| {
                    let session = StoredSession {
                        session_id: row.get(0)?,
                        project_id: row.get(1)?,
                        mode: parse_session_mode(&row.get::<_, String>(2)?)?,
                        title: row.get(3)?,
                        parent_session_id: row.get(4)?,
                        lineage_kind: parse_session_lineage(&row.get::<_, String>(5)?)?,
                        status: row.get(6)?,
                        environment_identity: row.get(7)?,
                        created_at_ms: to_u64_sql(row.get(8)?, "session creation time")?,
                        updated_at_ms: to_u64_sql(row.get(9)?, "session update time")?,
                    };
                    let runtime = row
                        .get::<_, Option<String>>(10)?
                        .map(
                            |session_id| -> rusqlite::Result<StoredSessionRuntimeBinding> {
                                Ok(StoredSessionRuntimeBinding {
                                    session_id,
                                    adapter: row.get(11)?,
                                    adapter_version: row.get(12)?,
                                    backend_session_id: row.get(13)?,
                                    provider: row.get(14)?,
                                    model: row.get(15)?,
                                    permission_profile: row.get(16)?,
                                    created_at_ms: to_u64_sql(
                                        row.get(17)?,
                                        "session runtime binding creation time",
                                    )?,
                                    updated_at_ms: to_u64_sql(
                                        row.get(18)?,
                                        "session runtime binding update time",
                                    )?,
                                })
                            },
                        )
                        .transpose()?;
                    let workspace = row
                        .get::<_, Option<String>>(19)?
                        .map(
                            |session_id| -> rusqlite::Result<StoredSessionWorkspaceBinding> {
                                Ok(StoredSessionWorkspaceBinding {
                                    session_id,
                                    project_id: row.get(20)?,
                                    root_id: row.get(21)?,
                                    root_identity: row.get(22)?,
                                    workspace_kind: row.get(23)?,
                                    git_state: row.get(24)?,
                                    repository_root_identity: row.get(25)?,
                                    worktree_root_identity: row.get(26)?,
                                    branch: row.get(27)?,
                                    branch_sha256: row.get(28)?,
                                    branch_redacted: row.get::<_, i64>(29)? != 0,
                                    head_oid: row.get(30)?,
                                    detached: row.get::<_, i64>(31)? != 0,
                                    unborn: row.get::<_, i64>(32)? != 0,
                                    dedicated_worktree: row.get::<_, i64>(33)? != 0,
                                    captured_at_ms: to_u64_sql(
                                        row.get(34)?,
                                        "workspace binding capture time",
                                    )?,
                                    updated_at_ms: to_u64_sql(
                                        row.get(35)?,
                                        "workspace binding update time",
                                    )?,
                                })
                            },
                        )
                        .transpose()?;
                    let text_match = row.get::<_, i64>(36)? != 0;
                    let mut matched_fields = Vec::new();
                    if request
                        .title
                        .as_deref()
                        .is_some_and(|query| contains_case_insensitive(&session.title, query))
                    {
                        matched_fields.push("title".into());
                    }
                    if text_match {
                        matched_fields.push("text".into());
                    }
                    if request
                        .text
                        .as_deref()
                        .is_some_and(|query| contains_case_insensitive(&session.title, query))
                    {
                        matched_fields.push("title".into());
                    }
                    if request.model.is_some() {
                        matched_fields.push("model".into());
                    }
                    if request.branch.is_some() {
                        matched_fields.push("branch".into());
                    }
                    if request.runtime.is_some() {
                        matched_fields.push("runtime".into());
                    }
                    Ok(StoredSessionSearchResult {
                        session,
                        runtime,
                        workspace,
                        matched_fields,
                    })
                },
            )
            .map_err(|_| error("cannot read session search"))?;
        let mut results = rows
            .collect::<Result<Vec<_>, _>>()
            .map_err(|_| error("session search row is invalid"))?;
        let truncated = results.len() > request.limit;
        if truncated {
            results.truncate(request.limit);
        }
        let next_cursor = if truncated {
            results.last().map(|result| {
                format!(
                    "after:{}:{}",
                    result.session.updated_at_ms, result.session.session_id
                )
            })
        } else {
            None
        };
        Ok(SessionSearchPage {
            results,
            next_cursor,
            truncated,
        })
    }

    pub fn preview_portable_session_export(
        &self,
        session_id: &str,
        exported_at_ms: u64,
    ) -> Result<PortableSessionExportPreview, WorkbenchStoreError> {
        let package = self.build_portable_session_package(session_id, exported_at_ms, None)?;
        portable_export_preview(&package)
    }

    pub fn preview_portable_session_export_through_turn(
        &self,
        session_id: &str,
        turn_id: &str,
        exported_at_ms: u64,
    ) -> Result<PortableSessionExportPreview, WorkbenchStoreError> {
        let boundary = self.session_item_boundary_for_completed_turn(session_id, turn_id)?;
        let package =
            self.build_portable_session_package(session_id, exported_at_ms, Some(boundary))?;
        portable_export_preview(&package)
    }

    pub fn export_portable_session(
        &self,
        session_id: &str,
        expected_package_hash: &ContentHash,
        exported_at_ms: u64,
    ) -> Result<PortableSessionPackage, WorkbenchStoreError> {
        validate_content_hash(expected_package_hash, "portable session package hash")?;
        let package = self.build_portable_session_package(session_id, exported_at_ms, None)?;
        if package.content_hash != *expected_package_hash {
            return Err(coded_error(
                "portable-session-export-stale",
                "session changed after export preview",
            ));
        }
        Ok(package)
    }

    pub fn export_portable_session_through_turn(
        &self,
        session_id: &str,
        turn_id: &str,
        expected_package_hash: &ContentHash,
        exported_at_ms: u64,
    ) -> Result<PortableSessionPackage, WorkbenchStoreError> {
        validate_content_hash(expected_package_hash, "portable session package hash")?;
        let boundary = self.session_item_boundary_for_completed_turn(session_id, turn_id)?;
        let package =
            self.build_portable_session_package(session_id, exported_at_ms, Some(boundary))?;
        if package.content_hash != *expected_package_hash {
            return Err(coded_error(
                "portable-session-export-stale",
                "session changed after export preview",
            ));
        }
        Ok(package)
    }

    pub fn preview_portable_session_import(
        &self,
        package: &PortableSessionPackage,
        target_project_id: Option<&str>,
    ) -> Result<PortableSessionImportPreview, WorkbenchStoreError> {
        let package_bytes = validate_portable_session_package(package)?;
        if let Some(project_id) = target_project_id {
            validate_identifier(project_id, "portable import target project ID")?;
        }
        let mut blocking_reasons = Vec::new();
        if package.content.mode == StoredSessionMode::Work && target_project_id.is_none() {
            push_projection_issue(&mut blocking_reasons, "portable-import-project-required");
        }
        if let Some(project_id) = target_project_id {
            let project_eligible = self
                .connection
                .query_row(
                    "SELECT EXISTS(
                        SELECT 1 FROM projects WHERE project_id = ?1 AND state = 'active'
                     )",
                    [project_id],
                    |row| row.get::<_, bool>(0),
                )
                .map_err(|_| error("cannot inspect portable import project"))?;
            if !project_eligible || self.quarantined_projects.contains(project_id) {
                push_projection_issue(&mut blocking_reasons, "portable-import-project-unavailable");
            }
        }
        let source_session_collision = self
            .connection
            .query_row(
                "SELECT EXISTS(SELECT 1 FROM sessions WHERE session_id = ?1)",
                [&package.content.source_session_id],
                |row| row.get::<_, bool>(0),
            )
            .map_err(|_| error("cannot inspect portable import session collision"))?;
        let mut item_collision_statement = self
            .connection
            .prepare("SELECT EXISTS(SELECT 1 FROM items WHERE item_id = ?1)")
            .map_err(|_| error("cannot prepare portable item collision scan"))?;
        let mut source_item_id_collisions = 0_u64;
        for item in &package.content.items {
            let collision = item_collision_statement
                .query_row([&item.source_item_id], |row| row.get::<_, bool>(0))
                .map_err(|_| error("cannot inspect portable item collision"))?;
            source_item_id_collisions =
                source_item_id_collisions.saturating_add(u64::from(collision));
        }
        let (content_categories, mut warnings) = portable_content_categories(package);
        if source_session_collision {
            push_projection_issue(&mut warnings, "portable-source-session-collision");
        }
        if source_item_id_collisions > 0 {
            push_projection_issue(&mut warnings, "portable-source-item-collisions");
        }
        Ok(PortableSessionImportPreview {
            schema_version: "portable-session-import-preview/0.1".into(),
            source_session_id: package.content.source_session_id.clone(),
            mode: package.content.mode,
            title: package.content.title.clone(),
            item_count: package.content.items.len() as u64,
            package_bytes,
            package_hash: package.content_hash.clone(),
            source_session_collision,
            source_item_id_collisions,
            target_project_required: package.content.mode == StoredSessionMode::Work,
            content_categories,
            warnings,
            blocking_reasons,
        })
    }

    pub fn import_portable_session(
        &mut self,
        command: PortableSessionImportCommand<'_>,
    ) -> Result<PortableSessionImportReceipt, WorkbenchStoreError> {
        let PortableSessionImportCommand {
            target_session_id,
            import_id,
            package,
            target_project_id,
            reject_source_collisions,
            target_environment_identity,
            runtime_binding,
            workspace_binding,
            imported_at_ms,
        } = command;
        validate_identifier(target_session_id, "portable import target session ID")?;
        validate_identifier(import_id, "portable import ID")?;
        if let Some(environment_identity) = target_environment_identity {
            validate_identity_text(
                environment_identity,
                "portable import target environment identity",
            )?;
        }
        if let Some(binding) = runtime_binding.as_ref() {
            validate_session_runtime_binding_create(binding, target_session_id)?;
            if binding.created_at_ms != imported_at_ms {
                return Err(error(
                    "import runtime binding creation time does not match import",
                ));
            }
        }
        if let Some(binding) = workspace_binding.as_ref() {
            validate_session_workspace_binding_create(
                binding,
                target_session_id,
                target_project_id,
            )?;
            if package.content.mode != StoredSessionMode::Work
                || binding.captured_at_ms != imported_at_ms
            {
                return Err(error("import workspace binding does not match import"));
            }
        }
        let preview = self.preview_portable_session_import(package, target_project_id)?;
        if !preview.blocking_reasons.is_empty() {
            return Err(coded_error(
                "portable-session-import-blocked",
                "portable session import preview has blocking reasons",
            ));
        }
        if reject_source_collisions
            && (preview.source_session_collision || preview.source_item_id_collisions > 0)
        {
            return Err(coded_error(
                "portable-import-source-collision",
                "portable import source identifiers already exist",
            ));
        }
        if let Some(project_id) = target_project_id {
            self.ensure_project_writable(project_id)?;
        }
        let existing_target = self
            .connection
            .query_row(
                "SELECT EXISTS(SELECT 1 FROM sessions WHERE session_id = ?1)",
                [target_session_id],
                |row| row.get::<_, bool>(0),
            )
            .map_err(|_| error("cannot inspect portable import target collision"))?;
        if existing_target {
            return Err(coded_error(
                "portable-import-session-collision",
                "portable import target session already exists",
            ));
        }
        let parent_session_id = self
            .connection
            .query_row(
                "SELECT project_id, mode FROM sessions WHERE session_id = ?1",
                [&package.content.source_session_id],
                |row| Ok((row.get::<_, Option<String>>(0)?, row.get::<_, String>(1)?)),
            )
            .optional()
            .map_err(|_| error("cannot inspect portable import source lineage"))?
            .filter(|(project_id, mode)| {
                project_id.as_deref() == target_project_id
                    && mode == package.content.mode.as_str()
                    && self
                        .ensure_session_readable(&package.content.source_session_id)
                        .is_ok()
            })
            .map(|_| package.content.source_session_id.clone());
        let lineage_kind = if parent_session_id.is_some() {
            StoredSessionLineage::Fork
        } else {
            StoredSessionLineage::New
        };
        let mut prepared_items = Vec::with_capacity(package.content.items.len());
        for item in &package.content.items {
            let item_id = derived_event_id(
                "portable-item",
                format!(
                    "{}\0{}\0{}\0{}",
                    package.content_hash.sha256,
                    target_session_id,
                    item.source_sequence,
                    item.source_item_id
                )
                .as_bytes(),
            );
            let request = StoredItemAppend {
                session_id: target_session_id.into(),
                turn_id: None,
                item_id,
                item_kind: item.item_kind.clone(),
                role: item.role.clone(),
                state: item.state.clone(),
                payload: item.payload.clone(),
                created_at_ms: imported_at_ms.saturating_add(item.source_sequence),
            };
            let prepared = prepare_item_append(&request)?;
            prepared_items.push((request, prepared));
        }
        let timestamp = to_i64(imported_at_ms, "portable import time")?;
        let transaction = self.begin_database_write("cannot start portable import transaction")?;
        if reject_source_collisions {
            let source_session_collision = transaction
                .query_row(
                    "SELECT EXISTS(SELECT 1 FROM sessions WHERE session_id = ?1)",
                    [&package.content.source_session_id],
                    |row| row.get::<_, bool>(0),
                )
                .map_err(|_| error("cannot revalidate portable source session collision"))?;
            let mut item_collision_statement = transaction
                .prepare("SELECT EXISTS(SELECT 1 FROM items WHERE item_id = ?1)")
                .map_err(|_| error("cannot revalidate portable source item collisions"))?;
            let mut source_item_collision = false;
            for item in &package.content.items {
                if item_collision_statement
                    .query_row([&item.source_item_id], |row| row.get::<_, bool>(0))
                    .map_err(|_| error("cannot revalidate portable source item collision"))?
                {
                    source_item_collision = true;
                    break;
                }
            }
            drop(item_collision_statement);
            if source_session_collision || source_item_collision {
                return Err(coded_error(
                    "portable-import-source-collision",
                    "portable import source identifiers changed before commit",
                ));
            }
        }
        if let Some(project_id) = target_project_id {
            let eligible = transaction
                .query_row(
                    "SELECT EXISTS(
                        SELECT 1 FROM projects WHERE project_id = ?1 AND state = 'active'
                     )",
                    [project_id],
                    |row| row.get::<_, bool>(0),
                )
                .map_err(|_| error("cannot revalidate portable import project"))?;
            if !eligible {
                return Err(coded_error(
                    "portable-session-import-stale",
                    "portable import project changed before commit",
                ));
            }
        }
        if let Some(binding) = workspace_binding.as_ref() {
            let root_identity: Option<String> = transaction
                .query_row(
                    "SELECT root_identity FROM project_roots
                     WHERE project_id = ?1 AND root_id = ?2",
                    params![binding.project_id, binding.root_id],
                    |row| row.get(0),
                )
                .optional()
                .map_err(|_| error("cannot revalidate import workspace root"))?;
            if root_identity.as_deref() != Some(binding.root_identity.as_str()) {
                return Err(coded_error(
                    "portable-session-import-stale",
                    "portable import workspace root changed before commit",
                ));
            }
        }
        transaction
            .execute(
                "INSERT INTO sessions (
                    session_id, project_id, mode, title, parent_session_id, lineage_kind,
                    status, environment_identity, created_at_ms, updated_at_ms
                 ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'active', ?7, ?8, ?8)",
                params![
                    target_session_id,
                    target_project_id,
                    package.content.mode.as_str(),
                    package.content.title,
                    parent_session_id,
                    lineage_kind.as_str(),
                    target_environment_identity,
                    timestamp,
                ],
            )
            .map_err(|_| error("portable import target session already exists or is invalid"))?;
        transaction
            .execute(
                "INSERT INTO session_projection_sources (
                    session_id, source_version, created_at_ms
                 ) VALUES (?1, 1, ?2)",
                params![target_session_id, timestamp],
            )
            .map_err(|_| error("cannot register portable import projection source"))?;
        let created_event_id = derived_event_id("session-created", target_session_id.as_bytes());
        append_event_tx(
            &transaction,
            EventInput {
                session_id: target_session_id,
                event_id: &created_event_id,
                timestamp_ms: imported_at_ms,
                correlation_id: import_id,
                event_kind: "session.created",
                project_id: target_project_id,
                operation_id: import_id,
                generation: 0,
                payload: json!({
                    "schema_version": "session.created/0.1",
                    "session": {
                        "session_id": target_session_id,
                        "project_id": target_project_id,
                        "mode": package.content.mode,
                        "title": package.content.title,
                        "parent_session_id": parent_session_id,
                        "lineage_kind": lineage_kind,
                        "status": "active",
                        "environment_identity": target_environment_identity,
                        "created_at_ms": imported_at_ms,
                        "updated_at_ms": imported_at_ms
                    }
                }),
            },
        )?;
        if let Some(binding) = runtime_binding.as_ref() {
            insert_session_runtime_binding_tx(&transaction, binding)?;
            append_session_runtime_binding_event_tx(
                &transaction,
                binding,
                target_session_id,
                target_project_id,
            )?;
        }
        if let Some(binding) = workspace_binding.as_ref() {
            insert_session_workspace_binding_tx(&transaction, binding)?;
            append_session_workspace_binding_event_tx(&transaction, binding)?;
        }
        for (request, prepared) in &prepared_items {
            append_item_tx(&transaction, request, prepared)?;
        }
        let imported_event_id = derived_event_id("session-imported", import_id.as_bytes());
        append_event_tx(
            &transaction,
            EventInput {
                session_id: target_session_id,
                event_id: &imported_event_id,
                timestamp_ms: imported_at_ms,
                correlation_id: import_id,
                event_kind: "session.imported",
                project_id: target_project_id,
                operation_id: import_id,
                generation: 0,
                payload: json!({
                    "schema_version": "session.imported/0.1",
                    "source_session_id": package.content.source_session_id,
                    "package_hash": package.content_hash,
                    "item_count": package.content.items.len(),
                    "redacted_value_count": package.redacted_value_count,
                    "excluded_field_count": package.excluded_field_count,
                    "linked_source_session": parent_session_id.is_some(),
                    "imported_at_ms": imported_at_ms
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit portable session import"))?;
        let session = self.load_session(target_session_id)?;
        Ok(PortableSessionImportReceipt {
            schema_version: "portable-session-import-receipt/0.1".into(),
            session,
            source_session_id: package.content.source_session_id.clone(),
            package_hash: package.content_hash.clone(),
            imported_items: package.content.items.len() as u64,
            source_session_collision: preview.source_session_collision,
            linked_source_session: parent_session_id.is_some(),
        })
    }

    fn session_item_boundary_for_completed_turn(
        &self,
        session_id: &str,
        turn_id: &str,
    ) -> Result<u64, WorkbenchStoreError> {
        validate_identifier(session_id, "fork source session ID")?;
        validate_identifier(turn_id, "fork boundary turn ID")?;
        self.ensure_session_readable(session_id)?;
        let state: Option<String> = self
            .connection
            .query_row(
                "SELECT state FROM turns WHERE session_id = ?1 AND turn_id = ?2",
                params![session_id, turn_id],
                |row| row.get(0),
            )
            .optional()
            .map_err(|_| error("cannot inspect fork boundary turn"))?;
        if state.as_deref() != Some("completed") {
            return Err(coded_error(
                "session-fork-boundary-invalid",
                "fork boundary must identify a completed turn",
            ));
        }
        let boundary: Option<i64> = self
            .connection
            .query_row(
                "SELECT MAX(item_sequence) FROM items
                 WHERE session_id = ?1 AND turn_id = ?2",
                params![session_id, turn_id],
                |row| row.get::<_, Option<i64>>(0),
            )
            .map_err(|_| error("cannot read fork boundary items"))?;
        let Some(sequence) = boundary else {
            return Err(coded_error(
                "session-fork-boundary-invalid",
                "completed turn has no items",
            ));
        };
        to_u64_sql(sequence, "fork boundary item sequence")
            .map_err(|_| error("fork boundary item sequence is invalid"))
    }

    fn build_portable_session_package(
        &self,
        session_id: &str,
        exported_at_ms: u64,
        max_sequence: Option<u64>,
    ) -> Result<PortableSessionPackage, WorkbenchStoreError> {
        let session = self.load_readable_session(session_id)?;
        let consistency = self.verify_session_projection(session_id)?;
        if !consistency.consistent {
            return Err(coded_error(
                "portable-session-export-unverified",
                "session projection is not verified for export",
            ));
        }
        let latest_sequence = self.latest_session_item_sequence(session_id)?;
        let selected_sequence = max_sequence.unwrap_or(latest_sequence);
        if selected_sequence > latest_sequence {
            return Err(coded_error(
                "portable-session-export-stale",
                "fork boundary is beyond the current session history",
            ));
        }
        if selected_sequence > MAX_PORTABLE_SESSION_ITEMS as u64 {
            return Err(coded_error(
                "portable-session-export-too-large",
                "session has too many items for one portable package",
            ));
        }
        let stored_items = if selected_sequence == 0 {
            Vec::new()
        } else {
            self.read_session_items(session_id, 0, selected_sequence as usize)?
        };
        let excluded_paths = match session.project_id.as_deref() {
            Some(project_id) => self
                .load_project_roots(project_id)?
                .into_iter()
                .map(|root| root.canonical_root)
                .collect::<Vec<_>>(),
            None => Vec::new(),
        };
        let mut redacted_value_count = 0_u64;
        let mut excluded_field_count = 0_u64;
        let items = stored_items
            .into_iter()
            .map(|item| PortableSessionItem {
                source_sequence: item.sequence,
                source_item_id: item.item_id,
                item_kind: item.item_kind,
                role: item.role,
                state: item.state,
                payload: portable_redact_value(
                    &item.payload,
                    &mut redacted_value_count,
                    &mut excluded_field_count,
                    &excluded_paths,
                ),
                source_created_at_ms: item.created_at_ms,
            })
            .collect::<Vec<_>>();
        let content = PortableSessionContent {
            schema_version: "aegisy-portable-session-content/0.1".into(),
            source_session_id: session.session_id,
            source_had_project: session.project_id.is_some(),
            mode: session.mode,
            title: portable_redact_string(
                &session.title,
                &mut redacted_value_count,
                &excluded_paths,
            ),
            source_created_at_ms: session.created_at_ms,
            source_updated_at_ms: session.updated_at_ms,
            items,
        };
        let content_json = serde_json::to_vec(&content)
            .map_err(|_| error("cannot serialize portable session content"))?;
        let content_hash = ContentHash::for_bytes(&content_json);
        let package = PortableSessionPackage {
            schema_version: "aegisy-portable-session/0.1".into(),
            exported_at_ms,
            content,
            content_hash,
            redacted_value_count,
            excluded_field_count,
        };
        validate_portable_session_package(&package)?;
        Ok(package)
    }

    pub fn preview_session_deletion(
        &self,
        session_id: &str,
        scope: SessionDeletionScope,
    ) -> Result<SessionDeletionPreview, WorkbenchStoreError> {
        validate_identifier(session_id, "session deletion root ID")?;
        let mut plan = build_session_deletion_plan(&self.connection, session_id, scope)?;
        if plan
            .selected
            .iter()
            .any(|member| self.session_requires_recovery(&member.session.session_id))
        {
            push_projection_issue(
                &mut plan.preview.blocking_reasons,
                "session-read-only-recovery",
            );
        }
        Ok(plan.preview)
    }

    pub fn session_deletion_member_ids(
        &self,
        session_id: &str,
        scope: SessionDeletionScope,
    ) -> Result<Vec<String>, WorkbenchStoreError> {
        validate_identifier(session_id, "session deletion root ID")?;
        let plan = build_session_deletion_plan(&self.connection, session_id, scope)?;
        Ok(plan
            .selected
            .into_iter()
            .map(|member| member.session.session_id)
            .collect())
    }

    pub fn schedule_session_deletion(
        &mut self,
        deletion_id: &str,
        session_id: &str,
        scope: SessionDeletionScope,
        expected_plan_hash: &ContentHash,
        requested_at_ms: u64,
        undo_window_ms: u64,
    ) -> Result<SessionDeletionReceipt, WorkbenchStoreError> {
        validate_identifier(deletion_id, "session deletion ID")?;
        validate_identifier(session_id, "session deletion root ID")?;
        validate_content_hash(expected_plan_hash, "session deletion plan hash")?;
        if !(MIN_SESSION_DELETE_UNDO_MS..=MAX_SESSION_DELETE_UNDO_MS).contains(&undo_window_ms) {
            return Err(error("session deletion undo window is invalid"));
        }
        let undo_until_ms = requested_at_ms
            .checked_add(undo_window_ms)
            .ok_or_else(|| error("session deletion undo time is out of range"))?;
        let reviewed = build_session_deletion_plan(&self.connection, session_id, scope)?;
        if reviewed
            .selected
            .iter()
            .any(|member| self.session_requires_recovery(&member.session.session_id))
        {
            return Err(coded_error(
                "session-read-only-recovery",
                "session deletion member is in read-only recovery",
            ));
        }
        if !reviewed.preview.blocking_reasons.is_empty() {
            return Err(coded_error(
                "session-deletion-blocked",
                "session deletion preview has blocking reasons",
            ));
        }
        if &reviewed.preview.plan_hash != expected_plan_hash {
            return Err(coded_error(
                "session-deletion-plan-stale",
                "session deletion plan changed before scheduling",
            ));
        }
        let transaction = self.begin_database_write("cannot start session deletion transaction")?;
        let current = build_session_deletion_plan(&transaction, session_id, scope)?;
        if !current.preview.blocking_reasons.is_empty()
            || current.preview.plan_hash != *expected_plan_hash
        {
            return Err(coded_error(
                "session-deletion-plan-stale",
                "session deletion plan changed under write lock",
            ));
        }
        transaction
            .execute(
                "INSERT INTO session_deletions (
                    deletion_id, root_session_id, scope, plan_sha256, plan_bytes,
                    session_count, descendant_count, artifact_reference_count,
                    artifact_bytes, requested_at_ms, undo_until_ms, state, updated_at_ms
                 ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11,
                           'pending', ?10)",
                params![
                    deletion_id,
                    session_id,
                    scope.as_str(),
                    expected_plan_hash.sha256,
                    to_i64(expected_plan_hash.bytes, "session deletion plan bytes")?,
                    to_i64(
                        current.preview.session_count,
                        "session deletion member count"
                    )?,
                    to_i64(
                        current.preview.descendant_count,
                        "session deletion descendant count"
                    )?,
                    to_i64(
                        current.preview.artifact_reference_count,
                        "session deletion artifact count"
                    )?,
                    to_i64(
                        current.preview.artifact_bytes,
                        "session deletion artifact bytes"
                    )?,
                    to_i64(requested_at_ms, "session deletion request time")?,
                    to_i64(undo_until_ms, "session deletion undo time")?,
                ],
            )
            .map_err(|_| error("session deletion already exists or conflicts"))?;
        for member in &current.selected {
            transaction
                .execute(
                    "INSERT INTO session_deletion_members (
                        deletion_id, session_id, depth, original_status
                     ) VALUES (?1, ?2, ?3, ?4)",
                    params![
                        deletion_id,
                        member.session.session_id,
                        to_i64(member.depth, "session deletion member depth")?,
                        member.session.status,
                    ],
                )
                .map_err(|_| error("session deletion member already exists or is invalid"))?;
        }
        let root = current
            .selected
            .iter()
            .find(|member| member.depth == 0)
            .ok_or_else(|| error("session deletion root is missing"))?;
        let event_id = derived_event_id("session-deletion-scheduled", deletion_id.as_bytes());
        append_event_tx(
            &transaction,
            EventInput {
                session_id,
                event_id: &event_id,
                timestamp_ms: requested_at_ms,
                correlation_id: deletion_id,
                event_kind: "retention.session-deletion-scheduled",
                project_id: root.session.project_id.as_deref(),
                operation_id: deletion_id,
                generation: 0,
                payload: json!({
                    "schema_version": "session-deletion-scheduled/0.1",
                    "deletion_id": deletion_id,
                    "scope": scope,
                    "plan_hash": expected_plan_hash,
                    "session_count": current.preview.session_count,
                    "descendant_count": current.preview.descendant_count,
                    "artifact_reference_count": current.preview.artifact_reference_count,
                    "artifact_bytes": current.preview.artifact_bytes,
                    "requested_at_ms": requested_at_ms,
                    "undo_until_ms": undo_until_ms
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit session deletion transaction"))?;
        Ok(SessionDeletionReceipt {
            schema_version: "session-deletion-receipt/0.1".into(),
            deletion_id: deletion_id.into(),
            root_session_id: session_id.into(),
            scope,
            plan_hash: expected_plan_hash.clone(),
            session_count: current.preview.session_count,
            descendant_count: current.preview.descendant_count,
            artifact_reference_count: current.preview.artifact_reference_count,
            artifact_bytes: current.preview.artifact_bytes,
            requested_at_ms,
            undo_until_ms,
            state: "pending".into(),
        })
    }

    pub fn session_deletion_for_session(
        &self,
        session_id: &str,
    ) -> Result<Option<SessionDeletionReceipt>, WorkbenchStoreError> {
        validate_identifier(session_id, "session deletion lookup ID")?;
        let deletion_id = self
            .connection
            .query_row(
                "SELECT deletion_id FROM session_deletion_members WHERE session_id = ?1",
                [session_id],
                |row| row.get::<_, String>(0),
            )
            .optional()
            .map_err(|_| error("cannot locate session deletion"))?;
        deletion_id
            .map(|deletion_id| load_session_deletion_receipt(&self.connection, &deletion_id))
            .transpose()
    }

    pub fn undo_session_deletion(
        &mut self,
        deletion_id: &str,
        undone_at_ms: u64,
    ) -> Result<SessionDeletionReceipt, WorkbenchStoreError> {
        validate_identifier(deletion_id, "session deletion ID")?;
        let existing = load_session_deletion_receipt(&self.connection, deletion_id)?;
        if existing.state != "pending" {
            return Err(coded_error(
                "session-deletion-not-undoable",
                "session deletion is not pending",
            ));
        }
        if undone_at_ms > existing.undo_until_ms {
            return Err(coded_error(
                "session-deletion-undo-expired",
                "session deletion undo window expired",
            ));
        }
        if self.session_requires_recovery(&existing.root_session_id) {
            return Err(coded_error(
                "session-read-only-recovery",
                "session is in read-only recovery",
            ));
        }
        let transaction =
            self.begin_database_write("cannot start session deletion undo transaction")?;
        let current = load_session_deletion_receipt(&transaction, deletion_id)?;
        if current.state != "pending"
            || current.plan_hash != existing.plan_hash
            || undone_at_ms > current.undo_until_ms
        {
            return Err(coded_error(
                "session-deletion-undo-stale",
                "session deletion changed before undo",
            ));
        }
        let project_id = transaction
            .query_row(
                "SELECT project_id FROM sessions WHERE session_id = ?1",
                [&current.root_session_id],
                |row| row.get::<_, Option<String>>(0),
            )
            .optional()
            .map_err(|_| error("cannot read session deletion undo binding"))?
            .ok_or_else(|| error("session deletion root is missing"))?;
        let event_id = derived_event_id("session-deletion-undone", deletion_id.as_bytes());
        append_event_tx(
            &transaction,
            EventInput {
                session_id: &current.root_session_id,
                event_id: &event_id,
                timestamp_ms: undone_at_ms,
                correlation_id: deletion_id,
                event_kind: "retention.session-deletion-undone",
                project_id: project_id.as_deref(),
                operation_id: deletion_id,
                generation: 0,
                payload: json!({
                    "schema_version": "session-deletion-undone/0.1",
                    "deletion_id": deletion_id,
                    "plan_hash": current.plan_hash,
                    "undone_at_ms": undone_at_ms
                }),
            },
        )?;
        transaction
            .execute(
                "DELETE FROM session_deletions
                 WHERE deletion_id = ?1 AND state = 'pending'",
                [deletion_id],
            )
            .map_err(|_| error("cannot remove undone session deletion"))?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit session deletion undo"))?;
        Ok(SessionDeletionReceipt {
            state: "cancelled".into(),
            ..current
        })
    }

    pub fn sweep_session_deletions(
        &mut self,
        now_ms: u64,
    ) -> Result<SessionDeletionSweepReport, WorkbenchStoreError> {
        let mut statement = self
            .connection
            .prepare(
                "SELECT deletion_id FROM session_deletions
                 WHERE state = 'pending' AND undo_until_ms <= ?1
                 ORDER BY undo_until_ms, deletion_id LIMIT ?2",
            )
            .map_err(|_| error("cannot prepare session deletion sweep"))?;
        let deletion_ids = statement
            .query_map(
                params![
                    to_i64(now_ms, "session deletion sweep time")?,
                    MAX_SESSION_DELETE_SWEEP as i64
                ],
                |row| row.get::<_, String>(0),
            )
            .map_err(|_| error("cannot read session deletion sweep"))?
            .collect::<Result<Vec<_>, _>>()
            .map_err(|_| error("session deletion sweep row is invalid"))?;
        drop(statement);
        let mut report = SessionDeletionSweepReport {
            schema_version: "session-deletion-sweep/0.1".into(),
            examined: 0,
            purged: 0,
            released_artifact_references: 0,
            retained: 0,
            issues: Vec::new(),
        };
        for deletion_id in deletion_ids {
            report.examined = report.examined.saturating_add(1);
            match self.purge_session_deletion(&deletion_id, now_ms) {
                Ok(released) => {
                    report.purged = report.purged.saturating_add(1);
                    report.released_artifact_references =
                        report.released_artifact_references.saturating_add(released);
                }
                Err(cause)
                    if matches!(
                        cause.code.as_str(),
                        "session-read-only-recovery" | "session-deletion-not-due"
                    ) =>
                {
                    report.retained = report.retained.saturating_add(1);
                    push_projection_issue(&mut report.issues, &cause.code);
                }
                Err(cause) => return Err(cause),
            }
        }
        Ok(report)
    }

    fn purge_session_deletion(
        &mut self,
        deletion_id: &str,
        now_ms: u64,
    ) -> Result<u64, WorkbenchStoreError> {
        let receipt = load_session_deletion_receipt(&self.connection, deletion_id)?;
        if receipt.state != "pending" || now_ms < receipt.undo_until_ms {
            return Err(coded_error(
                "session-deletion-not-due",
                "session deletion is not due",
            ));
        }
        let members = load_session_deletion_members(&self.connection, deletion_id)?;
        if members.is_empty() || members.len() as u64 != receipt.session_count {
            return Err(error("session deletion member snapshot is incomplete"));
        }
        if members
            .iter()
            .any(|member| self.session_requires_recovery(&member.session_id))
        {
            return Err(coded_error(
                "session-read-only-recovery",
                "session deletion member is in read-only recovery",
            ));
        }
        let retention_until = now_ms
            .checked_add(MIN_BLOB_RETENTION_MS)
            .ok_or_else(|| error("session deletion retention time is out of range"))?;
        let transaction =
            self.begin_database_write("cannot start session deletion purge transaction")?;
        let current = load_session_deletion_receipt(&transaction, deletion_id)?;
        let current_members = load_session_deletion_members(&transaction, deletion_id)?;
        if current.state != "pending"
            || current.plan_hash != receipt.plan_hash
            || now_ms < current.undo_until_ms
            || current_members != members
        {
            return Err(error("session deletion changed before purge"));
        }
        let mut released = 0_u64;
        for member in &members {
            transaction
                .execute(
                    "UPDATE durable_blobs SET retain_until_ms = CASE
                        WHEN retain_until_ms < ?1 THEN ?1 ELSE retain_until_ms END
                     WHERE sha256 IN (
                        SELECT blob_sha256 FROM durable_blob_references
                        WHERE session_id = ?2
                     )",
                    params![
                        to_i64(retention_until, "session deletion Blob retention time")?,
                        member.session_id
                    ],
                )
                .map_err(|_| error("cannot retain deleted session Blob content"))?;
            let changed = transaction
                .execute(
                    "UPDATE durable_blob_references
                     SET state = 'released', released_at_ms = ?1,
                         retain_until_ms = CASE
                            WHEN retain_until_ms < ?2 THEN ?2 ELSE retain_until_ms END
                     WHERE session_id = ?3 AND state = 'active'",
                    params![
                        to_i64(now_ms, "session deletion purge time")?,
                        to_i64(retention_until, "session deletion Blob retention time")?,
                        member.session_id
                    ],
                )
                .map_err(|_| error("cannot release deleted session Blob references"))?;
            released = released.saturating_add(changed as u64);
            transaction
                .execute(
                    "DELETE FROM approval_decisions WHERE session_id = ?1",
                    [&member.session_id],
                )
                .map_err(|_| error("cannot purge deleted session approvals"))?;
            transaction
                .execute(
                    "DELETE FROM authorization_consumptions WHERE session_id = ?1",
                    [&member.session_id],
                )
                .map_err(|_| error("cannot purge deleted session authorizations"))?;
            transaction
                .execute(
                    "DELETE FROM session_workspace_bindings WHERE session_id = ?1",
                    [&member.session_id],
                )
                .map_err(|_| error("cannot purge deleted session workspace binding"))?;
            transaction
                .execute(
                    "DELETE FROM background_jobs WHERE session_id = ?1",
                    [&member.session_id],
                )
                .map_err(|_| error("cannot purge deleted session background jobs"))?;
            transaction
                .execute(
                    "DELETE FROM items WHERE session_id = ?1",
                    [&member.session_id],
                )
                .map_err(|_| error("cannot purge deleted session items"))?;
            transaction
                .execute(
                    "DELETE FROM turns WHERE session_id = ?1",
                    [&member.session_id],
                )
                .map_err(|_| error("cannot purge deleted session turns"))?;
            transaction
                .execute(
                    "DELETE FROM session_projection_sources WHERE session_id = ?1",
                    [&member.session_id],
                )
                .map_err(|_| error("cannot purge deleted session projection source"))?;
            transaction
                .execute(
                    "DELETE FROM events WHERE session_id = ?1",
                    [&member.session_id],
                )
                .map_err(|_| error("cannot purge deleted session events"))?;
            transaction
                .execute(
                    "DELETE FROM session_sequences WHERE session_id = ?1",
                    [&member.session_id],
                )
                .map_err(|_| error("cannot purge deleted session event cursor"))?;
            transaction
                .execute(
                    "UPDATE sessions SET title = 'Deleted session', status = 'archived',
                        environment_identity = NULL,
                        updated_at_ms = CASE WHEN updated_at_ms < ?1 THEN ?1 ELSE updated_at_ms END
                     WHERE session_id = ?2",
                    params![
                        to_i64(now_ms, "session deletion purge time")?,
                        member.session_id
                    ],
                )
                .map_err(|_| error("cannot install deleted session tombstone"))?;
        }
        transaction
            .execute(
                "UPDATE session_deletions SET state = 'purged', updated_at_ms = ?1
                 WHERE deletion_id = ?2 AND state = 'pending'",
                params![to_i64(now_ms, "session deletion purge time")?, deletion_id],
            )
            .map_err(|_| error("cannot mark session deletion purged"))?;
        let retention_stream = "retention-stream-v1";
        let event_id = derived_event_id("session-deletion-purged", deletion_id.as_bytes());
        let project_id = transaction
            .query_row(
                "SELECT project_id FROM sessions WHERE session_id = ?1",
                [&receipt.root_session_id],
                |row| row.get::<_, Option<String>>(0),
            )
            .optional()
            .map_err(|_| error("cannot read purged session project binding"))?
            .flatten();
        append_event_tx(
            &transaction,
            EventInput {
                session_id: retention_stream,
                event_id: &event_id,
                timestamp_ms: now_ms,
                correlation_id: deletion_id,
                event_kind: "retention.session-deletion-purged",
                project_id: project_id.as_deref(),
                operation_id: deletion_id,
                generation: 0,
                payload: json!({
                    "schema_version": "session-deletion-purged/0.1",
                    "deletion_id": deletion_id,
                    "root_session_id": receipt.root_session_id,
                    "scope": receipt.scope,
                    "plan_hash": receipt.plan_hash,
                    "session_count": receipt.session_count,
                    "artifact_reference_count": receipt.artifact_reference_count,
                    "released_artifact_references": released,
                    "purged_at_ms": now_ms
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit session deletion purge"))?;
        Ok(released)
    }

    pub fn set_retention_policy(
        &mut self,
        policy: RetentionPolicy,
    ) -> Result<RetentionPolicy, WorkbenchStoreError> {
        validate_retention_policy(&policy)?;
        match policy.scope_kind.as_str() {
            "project" => self.ensure_project_writable(&policy.scope_id)?,
            "session" => self.ensure_session_writable(&policy.scope_id)?,
            _ => return Err(error("retention policy scope is invalid")),
        }
        let exists = match policy.scope_kind.as_str() {
            "project" => self
                .connection
                .query_row(
                    "SELECT EXISTS(SELECT 1 FROM projects WHERE project_id = ?1)",
                    [&policy.scope_id],
                    |row| row.get::<_, bool>(0),
                )
                .map_err(|_| error("cannot validate retention project"))?,
            "session" => self
                .connection
                .query_row(
                    "SELECT EXISTS(SELECT 1 FROM sessions WHERE session_id = ?1)",
                    [&policy.scope_id],
                    |row| row.get::<_, bool>(0),
                )
                .map_err(|_| error("cannot validate retention session"))?,
            _ => false,
        };
        if !exists {
            return Err(error("retention policy scope does not exist"));
        }
        let transaction = self.begin_database_write("cannot start retention policy transaction")?;
        let changed = transaction
            .execute(
                "INSERT INTO retention_policies (
                    scope_kind, scope_id, archive_after_ms, delete_after_ms,
                    undo_window_ms, delete_scope, updated_at_ms
                 ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
                 ON CONFLICT(scope_kind, scope_id) DO UPDATE SET
                    archive_after_ms = excluded.archive_after_ms,
                    delete_after_ms = excluded.delete_after_ms,
                    undo_window_ms = excluded.undo_window_ms,
                    delete_scope = excluded.delete_scope,
                    updated_at_ms = excluded.updated_at_ms
                 WHERE retention_policies.updated_at_ms <= excluded.updated_at_ms",
                params![
                    policy.scope_kind,
                    policy.scope_id,
                    policy
                        .archive_after_ms
                        .map(|value| to_i64(value, "retention archive age"))
                        .transpose()?,
                    policy
                        .delete_after_ms
                        .map(|value| to_i64(value, "retention delete age"))
                        .transpose()?,
                    to_i64(policy.undo_window_ms, "retention undo window")?,
                    policy.delete_scope.as_str(),
                    to_i64(policy.updated_at_ms, "retention policy update time")?,
                ],
            )
            .map_err(|_| error("cannot persist retention policy"))?;
        if changed != 1 {
            return Err(error("retention policy update time is stale"));
        }
        let retention_stream = "retention-stream-v1";
        let event_id = derived_event_id(
            "retention-policy-updated",
            format!(
                "{}\0{}\0{}",
                policy.scope_kind, policy.scope_id, policy.updated_at_ms
            )
            .as_bytes(),
        );
        let project_id: Option<String> = if policy.scope_kind == "project" {
            Some(policy.scope_id.clone())
        } else {
            transaction
                .query_row(
                    "SELECT project_id FROM sessions WHERE session_id = ?1",
                    [&policy.scope_id],
                    |row| row.get::<_, Option<String>>(0),
                )
                .optional()
                .map_err(|_| error("cannot read retention policy project binding"))?
                .flatten()
        };
        append_event_tx(
            &transaction,
            EventInput {
                session_id: retention_stream,
                event_id: &event_id,
                timestamp_ms: policy.updated_at_ms,
                correlation_id: &policy.scope_id,
                event_kind: "retention.policy-updated",
                project_id: project_id.as_deref(),
                operation_id: &policy.scope_id,
                generation: 0,
                payload: json!({
                    "schema_version": "retention-policy/0.1",
                    "policy": policy
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit retention policy"))?;
        self.load_retention_policy(&policy.scope_kind, &policy.scope_id)?
            .ok_or_else(|| error("retention policy was not persisted"))
    }

    pub fn load_retention_policy(
        &self,
        scope_kind: &str,
        scope_id: &str,
    ) -> Result<Option<RetentionPolicy>, WorkbenchStoreError> {
        validate_identifier(scope_id, "retention policy scope ID")?;
        if !matches!(scope_kind, "project" | "session") {
            return Err(error("retention policy scope is invalid"));
        }
        self.connection
            .query_row(
                "SELECT scope_kind, scope_id, archive_after_ms, delete_after_ms,
                        undo_window_ms, delete_scope, updated_at_ms
                 FROM retention_policies WHERE scope_kind = ?1 AND scope_id = ?2",
                params![scope_kind, scope_id],
                retention_policy_from_row,
            )
            .optional()
            .map_err(|_| error("cannot read retention policy"))
    }

    pub fn remove_retention_policy(
        &mut self,
        scope_kind: &str,
        scope_id: &str,
        removed_at_ms: u64,
    ) -> Result<bool, WorkbenchStoreError> {
        validate_identifier(scope_id, "retention policy scope ID")?;
        match scope_kind {
            "project" => self.ensure_project_writable(scope_id)?,
            "session" => self.ensure_session_writable(scope_id)?,
            _ => return Err(error("retention policy scope is invalid")),
        }
        let project_id = if scope_kind == "project" {
            Some(scope_id.to_owned())
        } else {
            self.connection
                .query_row(
                    "SELECT project_id FROM sessions WHERE session_id = ?1",
                    [scope_id],
                    |row| row.get::<_, Option<String>>(0),
                )
                .optional()
                .map_err(|_| error("cannot read retention policy project binding"))?
                .flatten()
        };
        let transaction =
            self.begin_database_write("cannot start retention policy removal transaction")?;
        let removed = transaction
            .execute(
                "DELETE FROM retention_policies WHERE scope_kind = ?1 AND scope_id = ?2",
                params![scope_kind, scope_id],
            )
            .map_err(|_| error("cannot remove retention policy"))?;
        if removed == 0 {
            transaction
                .commit()
                .map_err(|_| error("cannot commit empty retention policy removal"))?;
            return Ok(false);
        }
        let event_id = derived_event_id(
            "retention-policy-removed",
            format!("{scope_kind}\0{scope_id}\0{removed_at_ms}").as_bytes(),
        );
        append_event_tx(
            &transaction,
            EventInput {
                session_id: "retention-stream-v1",
                event_id: &event_id,
                timestamp_ms: removed_at_ms,
                correlation_id: scope_id,
                event_kind: "retention.policy-removed",
                project_id: project_id.as_deref(),
                operation_id: scope_id,
                generation: 0,
                payload: json!({
                    "schema_version": "retention-policy-removed/0.1",
                    "scope_kind": scope_kind,
                    "scope_id": scope_id,
                    "removed_at_ms": removed_at_ms
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit retention policy removal"))?;
        Ok(true)
    }

    pub fn apply_retention_policies(
        &mut self,
        now_ms: u64,
        protected_session_ids: &BTreeSet<String>,
    ) -> Result<RetentionSweepReport, WorkbenchStoreError> {
        if protected_session_ids.len() > MAX_SESSION_DELETE_MEMBERS {
            return Err(error("retention protected session limit exceeded"));
        }
        for session_id in protected_session_ids {
            validate_identifier(session_id, "retention protected session ID")?;
        }
        let policies = load_all_retention_policies(&self.connection)?;
        let mut session_policies = BTreeMap::new();
        let mut project_policies = BTreeMap::new();
        for policy in policies {
            if policy.scope_kind == "session" {
                session_policies.insert(policy.scope_id.clone(), policy);
            } else {
                project_policies.insert(policy.scope_id.clone(), policy);
            }
        }
        let sessions = load_all_sessions_bounded(&self.connection)?;
        let mut report = RetentionSweepReport {
            schema_version: "retention-sweep/0.1".into(),
            examined_sessions: 0,
            archived_sessions: 0,
            scheduled_deletions: 0,
            protected_sessions: 0,
            skipped_sessions: 0,
            issues: Vec::new(),
        };
        for session in sessions {
            let policy = session_policies.get(&session.session_id).or_else(|| {
                session
                    .project_id
                    .as_ref()
                    .and_then(|project_id| project_policies.get(project_id))
            });
            let Some(policy) = policy.cloned() else {
                continue;
            };
            report.examined_sessions = report.examined_sessions.saturating_add(1);
            if self.session_deletion_state(&session.session_id)?.is_some() {
                report.skipped_sessions = report.skipped_sessions.saturating_add(1);
                continue;
            }
            let live_turns = query_count(
                &self.connection,
                "SELECT COUNT(*) FROM turns
                 WHERE session_id = ?1 AND state IN ('started','running')",
                &session.session_id,
                "cannot inspect retention live turns",
            )?;
            let live_approvals = self
                .connection
                .query_row(
                    "SELECT COUNT(*) FROM approval_decisions
                     WHERE session_id = ?1 AND status = 'issued' AND expires_at_ms > ?2",
                    params![session.session_id, to_i64(now_ms, "retention sweep time")?],
                    |row| row.get::<_, i64>(0),
                )
                .map_err(|_| error("cannot inspect retention live approvals"))?;
            let live_background_jobs = query_count(
                &self.connection,
                "SELECT COUNT(*) FROM background_jobs
                 WHERE session_id = ?1 AND status NOT IN (
                    'completed','failed','cancelled','interrupted'
                 )",
                &session.session_id,
                "cannot inspect retention live background jobs",
            )?;
            let protected = protected_session_ids.contains(&session.session_id)
                || live_turns > 0
                || live_approvals > 0
                || live_background_jobs > 0;
            if protected {
                report.protected_sessions = report.protected_sessions.saturating_add(1);
                continue;
            }
            if session.status == "active"
                && policy
                    .archive_after_ms
                    .is_some_and(|age| now_ms.saturating_sub(session.updated_at_ms) >= age)
            {
                self.archive_session(&session.session_id, now_ms)?;
                report.archived_sessions = report.archived_sessions.saturating_add(1);
                continue;
            }
            if session.status == "archived"
                && policy
                    .delete_after_ms
                    .is_some_and(|age| now_ms.saturating_sub(session.updated_at_ms) >= age)
            {
                let deletion_members =
                    self.session_deletion_member_ids(&session.session_id, policy.delete_scope)?;
                if deletion_members
                    .iter()
                    .any(|session_id| protected_session_ids.contains(session_id))
                {
                    report.protected_sessions = report.protected_sessions.saturating_add(1);
                    continue;
                }
                let preview =
                    self.preview_session_deletion(&session.session_id, policy.delete_scope)?;
                if !preview.blocking_reasons.is_empty() {
                    report.skipped_sessions = report.skipped_sessions.saturating_add(1);
                    continue;
                }
                let deletion_id = derived_event_id(
                    "retention-deletion",
                    format!(
                        "{}\0{}\0{}",
                        session.session_id, policy.updated_at_ms, now_ms
                    )
                    .as_bytes(),
                );
                self.schedule_session_deletion(
                    &deletion_id,
                    &session.session_id,
                    policy.delete_scope,
                    &preview.plan_hash,
                    now_ms,
                    policy.undo_window_ms,
                )?;
                report.scheduled_deletions = report.scheduled_deletions.saturating_add(1);
            }
        }
        Ok(report)
    }

    pub fn archive_session(
        &mut self,
        session_id: &str,
        updated_at_ms: u64,
    ) -> Result<StoredSession, WorkbenchStoreError> {
        validate_identifier(session_id, "session ID")?;
        self.ensure_session_writable(session_id)?;
        let timestamp = to_i64(updated_at_ms, "session update time")?;
        let transaction = self.begin_database_write("cannot start session archive transaction")?;
        let project_id = transaction
            .query_row(
                "SELECT project_id FROM sessions WHERE session_id = ?1",
                [session_id],
                |row| row.get::<_, Option<String>>(0),
            )
            .optional()
            .map_err(|_| error("cannot read session archive binding"))?
            .ok_or_else(|| error("session does not exist"))?;
        let changed = transaction
            .execute(
                "UPDATE sessions SET status = 'archived', updated_at_ms = ?1
                 WHERE session_id = ?2 AND updated_at_ms <= ?1
                   AND status IN ('active','failed','interrupted')",
                params![timestamp, session_id],
            )
            .map_err(|_| error("cannot archive session"))?;
        if changed != 1 {
            return Err(error(
                "session is missing, already archived, or timestamp is stale",
            ));
        }
        let event_id = derived_event_id(
            "session-archived",
            format!("{session_id}\0{updated_at_ms}").as_bytes(),
        );
        append_event_tx(
            &transaction,
            EventInput {
                session_id,
                event_id: &event_id,
                timestamp_ms: updated_at_ms,
                correlation_id: session_id,
                event_kind: "session.archived",
                project_id: project_id.as_deref(),
                operation_id: session_id,
                generation: 0,
                payload: json!({
                    "schema_version": "session.status/0.1",
                    "status": "archived",
                    "updated_at_ms": updated_at_ms
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit session archive transaction"))?;
        self.load_session(session_id)
    }

    pub fn update_session_title(
        &mut self,
        session_id: &str,
        title: &str,
        updated_at_ms: u64,
    ) -> Result<StoredSession, WorkbenchStoreError> {
        validate_identifier(session_id, "session ID")?;
        self.ensure_session_writable(session_id)?;
        validate_text(title, 256, "session title")?;
        let timestamp = to_i64(updated_at_ms, "session update time")?;
        let transaction = self.begin_database_write("cannot start session title transaction")?;
        let project_id = transaction
            .query_row(
                "SELECT project_id FROM sessions WHERE session_id = ?1",
                [session_id],
                |row| row.get::<_, Option<String>>(0),
            )
            .optional()
            .map_err(|_| error("cannot read session title binding"))?
            .ok_or_else(|| error("session does not exist"))?;
        let changed = transaction
            .execute(
                "UPDATE sessions SET title = ?1, updated_at_ms = ?2
                 WHERE session_id = ?3 AND updated_at_ms <= ?2",
                params![title, timestamp, session_id],
            )
            .map_err(|_| error("cannot update session title"))?;
        if changed != 1 {
            return Err(error(
                "session is missing or title update timestamp is stale",
            ));
        }
        let event_id = derived_event_id(
            "session-title",
            format!("{session_id}\0{updated_at_ms}\0{title}").as_bytes(),
        );
        append_event_tx(
            &transaction,
            EventInput {
                session_id,
                event_id: &event_id,
                timestamp_ms: updated_at_ms,
                correlation_id: session_id,
                event_kind: "session.title-updated",
                project_id: project_id.as_deref(),
                operation_id: session_id,
                generation: 0,
                payload: json!({
                    "schema_version": "session.title/0.1",
                    "title": title,
                    "updated_at_ms": updated_at_ms
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit session title transaction"))?;
        self.load_session(session_id)
    }

    pub fn unarchive_session(
        &mut self,
        session_id: &str,
        updated_at_ms: u64,
    ) -> Result<StoredSession, WorkbenchStoreError> {
        validate_identifier(session_id, "session ID")?;
        self.ensure_session_writable(session_id)?;
        let timestamp = to_i64(updated_at_ms, "session update time")?;
        let transaction =
            self.begin_database_write("cannot start session unarchive transaction")?;
        let project_id = transaction
            .query_row(
                "SELECT project_id FROM sessions WHERE session_id = ?1",
                [session_id],
                |row| row.get::<_, Option<String>>(0),
            )
            .optional()
            .map_err(|_| error("cannot read session unarchive binding"))?
            .ok_or_else(|| error("session does not exist"))?;
        let changed = transaction
            .execute(
                "UPDATE sessions SET status = 'active', updated_at_ms = ?1
                 WHERE session_id = ?2 AND updated_at_ms <= ?1 AND status = 'archived'",
                params![timestamp, session_id],
            )
            .map_err(|_| error("cannot unarchive session"))?;
        if changed != 1 {
            return Err(error(
                "session is missing, not archived, or timestamp is stale",
            ));
        }
        let event_id = derived_event_id(
            "session-unarchived",
            format!("{session_id}\0{updated_at_ms}").as_bytes(),
        );
        append_event_tx(
            &transaction,
            EventInput {
                session_id,
                event_id: &event_id,
                timestamp_ms: updated_at_ms,
                correlation_id: session_id,
                event_kind: "session.unarchived",
                project_id: project_id.as_deref(),
                operation_id: session_id,
                generation: 0,
                payload: json!({
                    "schema_version": "session.status/0.1",
                    "status": "active",
                    "updated_at_ms": updated_at_ms
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit session unarchive transaction"))?;
        self.load_session(session_id)
    }

    pub fn create_turn(
        &mut self,
        request: StoredTurnCreate,
    ) -> Result<StoredTurn, WorkbenchStoreError> {
        validate_identifier(&request.turn_id, "turn ID")?;
        validate_identifier(&request.session_id, "turn session ID")?;
        self.ensure_session_writable(&request.session_id)?;
        if let Some(idempotency_key) = &request.idempotency_key {
            validate_identifier(idempotency_key, "turn idempotency key")?;
        }
        validate_content_hash(&request.input_hash, "turn input hash")?;
        let timestamp = to_i64(request.created_at_ms, "turn creation time")?;
        let transaction = self.begin_database_write("cannot start turn transaction")?;
        let session_binding: Option<(String, Option<String>)> = transaction
            .query_row(
                "SELECT status, project_id FROM sessions WHERE session_id = ?1",
                [&request.session_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()
            .map_err(|_| error("cannot validate turn session"))?;
        let Some((session_state, project_id)) = session_binding else {
            return Err(error("turn session does not exist"));
        };
        if session_state == "archived" {
            return Err(error("cannot create a turn in an archived session"));
        }
        if let Some(idempotency_key) = &request.idempotency_key {
            let existing: Option<(String, String)> = transaction
                .query_row(
                    "SELECT turn_id, input_sha256 FROM turns
                     WHERE session_id = ?1 AND idempotency_key = ?2",
                    params![request.session_id, idempotency_key],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .optional()
                .map_err(|_| error("cannot read turn idempotency record"))?;
            if let Some((turn_id, input_sha256)) = existing {
                if turn_id != request.turn_id || input_sha256 != request.input_hash.sha256 {
                    return Err(error(
                        "turn idempotency key is bound to a different request",
                    ));
                }
                drop(transaction);
                return self.load_turn(&request.turn_id);
            }
        }
        transaction
            .execute(
                "INSERT INTO turns (
                    turn_id, session_id, idempotency_key, input_sha256, input_bytes,
                    state, created_at_ms, updated_at_ms
                 ) VALUES (?1, ?2, ?3, ?4, ?5, 'started', ?6, ?6)",
                params![
                    request.turn_id,
                    request.session_id,
                    request.idempotency_key,
                    request.input_hash.sha256,
                    to_i64(request.input_hash.bytes, "turn input byte count")?,
                    timestamp,
                ],
            )
            .map_err(|_| error("turn already exists or is invalid"))?;
        let event_id = derived_event_id("turn-created", request.turn_id.as_bytes());
        append_event_tx(
            &transaction,
            EventInput {
                session_id: &request.session_id,
                event_id: &event_id,
                timestamp_ms: request.created_at_ms,
                correlation_id: &request.turn_id,
                event_kind: "turn.created",
                project_id: project_id.as_deref(),
                operation_id: &request.turn_id,
                generation: 0,
                payload: json!({
                    "schema_version": "turn.created/0.1",
                    "turn": {
                        "turn_id": &request.turn_id,
                        "session_id": &request.session_id,
                        "idempotency_key": &request.idempotency_key,
                        "input_hash": &request.input_hash,
                        "state": "started",
                        "created_at_ms": request.created_at_ms,
                        "updated_at_ms": request.created_at_ms
                    }
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit turn transaction"))?;
        self.load_turn(&request.turn_id)
    }

    pub fn load_turn(&self, turn_id: &str) -> Result<StoredTurn, WorkbenchStoreError> {
        validate_identifier(turn_id, "turn ID")?;
        self.connection
            .query_row(
                "SELECT turn_id, session_id, idempotency_key, input_sha256, input_bytes,
                        state, created_at_ms, updated_at_ms
                 FROM turns WHERE turn_id = ?1",
                [turn_id],
                |row| {
                    Ok(StoredTurn {
                        turn_id: row.get(0)?,
                        session_id: row.get(1)?,
                        idempotency_key: row.get(2)?,
                        input_hash: ContentHash {
                            sha256: row.get(3)?,
                            bytes: to_u64_sql(row.get(4)?, "turn input byte count")?,
                        },
                        state: row.get(5)?,
                        created_at_ms: to_u64_sql(row.get(6)?, "turn creation time")?,
                        updated_at_ms: to_u64_sql(row.get(7)?, "turn update time")?,
                    })
                },
            )
            .optional()
            .map_err(|_| error("cannot read turn"))?
            .ok_or_else(|| error("turn does not exist"))
    }

    pub fn append_item(
        &mut self,
        request: StoredItemAppend,
    ) -> Result<StoredItem, WorkbenchStoreError> {
        self.ensure_session_writable(&request.session_id)?;
        let prepared = prepare_item_append(&request)?;
        let transaction = self.begin_database_write("cannot start item transaction")?;
        let stored = append_item_tx(&transaction, &request, &prepared)?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit item transaction"))?;
        Ok(stored)
    }

    pub fn append_item_with_durable_blob(
        &mut self,
        item: StoredItemAppend,
        blob: DurableBlobWrite,
    ) -> Result<(StoredItem, StoredDurableBlobReference), WorkbenchStoreError> {
        self.ensure_session_writable(&item.session_id)?;
        let prepared_item = prepare_item_append(&item)?;
        let prepared_blob = prepare_durable_blob(&blob)?;
        if blob.session_id.as_deref() != Some(&item.session_id)
            || blob.owner_kind != "item"
            || blob.owner_id != item.item_id
        {
            return Err(error(
                "durable blob is not bound to the item being appended",
            ));
        }
        validate_blob_ownership(
            &self.connection,
            blob.session_id.as_deref(),
            blob.project_id.as_deref(),
        )?;
        let existing: bool = self
            .connection
            .query_row(
                "SELECT EXISTS(SELECT 1 FROM durable_blobs WHERE sha256 = ?1)",
                [&prepared_blob.content_hash.sha256],
                |row| row.get(0),
            )
            .map_err(|_| error("cannot inspect durable blob admission state"))?;
        if !existing {
            let (object_count, retained_bytes): (i64, i64) = self
                .connection
                .query_row(
                    "SELECT COUNT(*), COALESCE(SUM(bytes), 0) FROM durable_blobs",
                    [],
                    |row| Ok((row.get(0)?, row.get(1)?)),
                )
                .map_err(|_| error("cannot inspect durable blob capacity"))?;
            if to_u64(object_count, "durable blob object count")? >= MAX_BLOB_OBJECTS
                || to_u64(retained_bytes, "durable blob retained bytes")?
                    .saturating_add(prepared_blob.content_hash.bytes)
                    > MAX_STORE_BYTES
            {
                return Err(error("durable blob admission limit exceeded"));
            }
        }
        let write = self
            .blob_files
            .put(&prepared_blob.content_hash.sha256, &blob.content, None)
            .map_err(blob_file_error)?;
        let result = (|| {
            let transaction =
                self.begin_database_write("cannot start item and durable blob transaction")?;
            let stored_blob = persist_durable_blob_tx(&transaction, &blob, &prepared_blob)?;
            let stored_item = append_item_tx(&transaction, &item, &prepared_item)?;
            transaction
                .commit()
                .map_err(|_| error("cannot commit item and durable blob transaction"))?;
            Ok((stored_item, stored_blob))
        })();
        if result.is_err() && write.created {
            self.blob_files.remove_created(
                &prepared_blob.content_hash.sha256,
                prepared_blob.content_hash.bytes,
            );
        }
        result
    }

    pub fn finish_turn(
        &mut self,
        session_id: &str,
        turn_id: &str,
        state: &str,
        updated_at_ms: u64,
    ) -> Result<StoredTurn, WorkbenchStoreError> {
        self.finish_turn_internal(session_id, turn_id, state, updated_at_ms, None)
    }

    pub fn finish_turn_with_trace(
        &mut self,
        session_id: &str,
        turn_id: &str,
        state: &str,
        updated_at_ms: u64,
        trace: &TurnTrace,
    ) -> Result<StoredTurn, WorkbenchStoreError> {
        let prepared_trace = prepare_turn_trace_record(session_id, turn_id, state, trace)?;
        self.finish_turn_internal(
            session_id,
            turn_id,
            state,
            updated_at_ms,
            Some(prepared_trace),
        )
    }

    fn finish_turn_internal(
        &mut self,
        session_id: &str,
        turn_id: &str,
        state: &str,
        updated_at_ms: u64,
        prepared_trace: Option<PreparedTurnTraceRecord>,
    ) -> Result<StoredTurn, WorkbenchStoreError> {
        validate_identifier(session_id, "turn session ID")?;
        self.ensure_session_writable(session_id)?;
        validate_identifier(turn_id, "turn ID")?;
        if !matches!(state, "completed" | "failed" | "interrupted" | "cancelled") {
            return Err(error("turn terminal state is invalid"));
        }
        let timestamp = to_i64(updated_at_ms, "turn update time")?;
        let transaction = self.begin_database_write("cannot start turn completion transaction")?;
        let (project_id, environment_identity, session_mode) = transaction
            .query_row(
                "SELECT session.project_id, session.environment_identity, session.mode
                 FROM turns AS turn_record
                 JOIN sessions AS session ON session.session_id = turn_record.session_id
                 WHERE turn_record.turn_id = ?1 AND turn_record.session_id = ?2",
                params![turn_id, session_id],
                |row| {
                    Ok((
                        row.get::<_, Option<String>>(0)?,
                        row.get::<_, Option<String>>(1)?,
                        parse_session_mode(&row.get::<_, String>(2)?)?,
                    ))
                },
            )
            .optional()
            .map_err(|_| error("cannot read turn completion binding"))?
            .ok_or_else(|| error("turn is missing"))?;
        if let Some(prepared_trace) = prepared_trace.as_ref() {
            if prepared_trace.trace.binding.project_id != project_id
                || prepared_trace.trace.binding.environment_identity != environment_identity
                || !turn_trace_matches_session_mode(&prepared_trace.trace, session_mode)
            {
                return Err(coded_error(
                    "turn-trace-binding-mismatch",
                    "turn trace project, environment, or mode binding does not match the stored turn",
                ));
            }
            if prepared_trace.terminal_at_ms != updated_at_ms {
                return Err(coded_error(
                    "turn-trace-terminal-time-mismatch",
                    "turn trace terminal time does not match the stored turn update",
                ));
            }
            if let Some(existing) = load_turn_trace_record(&transaction, session_id, turn_id)? {
                let (current_state, current_updated_at_ms): (String, i64) = transaction
                    .query_row(
                        "SELECT state, updated_at_ms FROM turns
                         WHERE turn_id = ?1 AND session_id = ?2",
                        params![turn_id, session_id],
                        |row| Ok((row.get(0)?, row.get(1)?)),
                    )
                    .map_err(|_| error("cannot read idempotent turn trace state"))?;
                if existing.trace_identity == prepared_trace.trace_identity
                    && existing.trace == prepared_trace.trace
                    && existing.state == state
                    && existing.recorded_at_ms == updated_at_ms
                    && current_state == state
                    && to_u64(current_updated_at_ms, "idempotent turn trace update time")?
                        == updated_at_ms
                {
                    validate_trace_terminal_pair(&transaction, &existing)?;
                    drop(transaction);
                    return self.load_turn(turn_id);
                }
                return Err(coded_error(
                    "turn-trace-conflict",
                    "turn already has a different durable trace",
                ));
            }
        }
        let changed = transaction
            .execute(
                "UPDATE turns SET state = ?1, updated_at_ms = ?2
                 WHERE turn_id = ?3 AND session_id = ?4
                   AND updated_at_ms <= ?2
                   AND state IN ('started','running')",
                params![state, timestamp, turn_id, session_id],
            )
            .map_err(|_| error("cannot finish turn"))?;
        if changed != 1 {
            return Err(error("turn is missing, terminal, or timestamp is stale"));
        }
        if let Some(prepared_trace) = prepared_trace.as_ref() {
            // Keep the terminal event last: operation reconciliation treats the
            // latest event for this operation as the authoritative Turn state.
            let event_id = derived_event_id(
                "turn-trace-recorded",
                format!("{turn_id}\0{}", prepared_trace.trace_identity).as_bytes(),
            );
            append_event_tx(
                &transaction,
                EventInput {
                    session_id,
                    event_id: &event_id,
                    timestamp_ms: updated_at_ms,
                    correlation_id: turn_id,
                    event_kind: "turn.trace.recorded",
                    project_id: project_id.as_deref(),
                    operation_id: turn_id,
                    generation: 0,
                    payload: prepared_trace.payload(updated_at_ms),
                },
            )?;
        }
        let event_id = derived_event_id(
            "turn-terminal",
            format!("{turn_id}\0{state}\0{updated_at_ms}").as_bytes(),
        );
        let event_kind = format!("turn.{state}");
        append_event_tx(
            &transaction,
            EventInput {
                session_id,
                event_id: &event_id,
                timestamp_ms: updated_at_ms,
                correlation_id: turn_id,
                event_kind: &event_kind,
                project_id: project_id.as_deref(),
                operation_id: turn_id,
                generation: 0,
                payload: json!({
                    "schema_version": "turn.terminal/0.1",
                    "turn_id": turn_id,
                    "state": state,
                    "updated_at_ms": updated_at_ms
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit turn completion transaction"))?;
        self.load_turn(turn_id)
    }

    pub fn read_turn_trace(
        &self,
        session_id: &str,
        turn_id: &str,
    ) -> Result<Option<StoredTurnTrace>, WorkbenchStoreError> {
        validate_identifier(session_id, "turn trace session ID")?;
        validate_identifier(turn_id, "turn trace turn ID")?;
        let stored = load_turn_trace_record(&self.connection, session_id, turn_id)?;
        if let Some(stored) = stored.as_ref() {
            let session_mode = self
                .connection
                .query_row(
                    "SELECT mode FROM sessions WHERE session_id = ?1",
                    [session_id],
                    |row| parse_session_mode(&row.get::<_, String>(0)?),
                )
                .optional()
                .map_err(|_| error("cannot read turn trace session mode"))?
                .ok_or_else(|| error("turn trace session is missing"))?;
            if !turn_trace_matches_session_mode(&stored.trace, session_mode) {
                return Err(coded_error(
                    "turn-trace-binding-mismatch",
                    "turn trace mode binding does not match the stored session",
                ));
            }
            validate_trace_terminal_pair(&self.connection, stored)?;
        }
        Ok(stored)
    }

    pub fn read_session_items(
        &self,
        session_id: &str,
        after_sequence: u64,
        limit: usize,
    ) -> Result<Vec<StoredItem>, WorkbenchStoreError> {
        validate_identifier(session_id, "item session ID")?;
        if limit == 0 || limit > MAX_EVENT_PAGE {
            return Err(error("item page limit is invalid"));
        }
        let mut statement = self
            .connection
            .prepare(
                "SELECT item_sequence, item_id, turn_id, item_kind, role, state,
                        payload_json, payload_sha256, payload_bytes, created_at_ms
                 FROM items WHERE session_id = ?1 AND item_sequence > ?2
                 ORDER BY item_sequence ASC LIMIT ?3",
            )
            .map_err(|_| error("cannot prepare item replay"))?;
        let rows = statement
            .query_map(
                params![
                    session_id,
                    to_i64(after_sequence, "item sequence")?,
                    i64::try_from(limit).map_err(|_| error("item page limit is invalid"))?,
                ],
                |row| {
                    Ok((
                        row.get::<_, i64>(0)?,
                        row.get::<_, String>(1)?,
                        row.get::<_, Option<String>>(2)?,
                        row.get::<_, String>(3)?,
                        row.get::<_, String>(4)?,
                        row.get::<_, String>(5)?,
                        row.get::<_, String>(6)?,
                        row.get::<_, String>(7)?,
                        row.get::<_, i64>(8)?,
                        row.get::<_, i64>(9)?,
                    ))
                },
            )
            .map_err(|_| error("cannot read item replay"))?;
        let mut expected = after_sequence.saturating_add(1);
        let mut items = Vec::new();
        for row in rows {
            let (
                sequence,
                item_id,
                turn_id,
                item_kind,
                role,
                state,
                payload_json,
                payload_sha256,
                payload_bytes,
                created_at_ms,
            ) = row.map_err(|_| error("item replay row is invalid"))?;
            let sequence = to_u64(sequence, "item sequence")?;
            if sequence != expected {
                return Err(error("item replay sequence has a gap"));
            }
            expected = expected.saturating_add(1);
            let payload = serde_json::from_str(&payload_json)
                .map_err(|_| error("item payload JSON is invalid"))?;
            validate_persisted_item_payload(&payload)?;
            let payload_hash = ContentHash {
                sha256: payload_sha256,
                bytes: to_u64(payload_bytes, "item payload byte count")?,
            };
            if ContentHash::for_bytes(payload_json.as_bytes()) != payload_hash {
                return Err(error("item payload integrity check failed"));
            }
            items.push(StoredItem {
                session_id: session_id.into(),
                sequence,
                item_id,
                turn_id,
                item_kind,
                role,
                state,
                payload,
                payload_hash,
                created_at_ms: to_u64(created_at_ms, "item creation time")?,
            });
        }
        Ok(items)
    }

    pub fn latest_session_item_sequence(
        &self,
        session_id: &str,
    ) -> Result<u64, WorkbenchStoreError> {
        validate_identifier(session_id, "item session ID")?;
        let sequence = self
            .connection
            .query_row(
                "SELECT COALESCE(MAX(item_sequence), 0) FROM items WHERE session_id = ?1",
                params![session_id],
                |row| row.get::<_, i64>(0),
            )
            .map_err(|_| error("cannot read latest item sequence"))?;
        to_u64(sequence, "item sequence")
    }

    fn recover_session_projections_at_startup(
        &mut self,
    ) -> Result<StartupProjectionRecoveryReport, WorkbenchStoreError> {
        let counts: (i64, i64, i64, i64, i64, i64, i64) = self
            .connection
            .query_row(
                "SELECT
                    (SELECT COUNT(*) FROM projects),
                    (SELECT COUNT(*) FROM project_roots),
                    (SELECT COUNT(*) FROM sessions),
                    (SELECT COUNT(*) FROM turns),
                    (SELECT COUNT(*) FROM items),
                    (SELECT COUNT(*) FROM events),
                    (SELECT COUNT(*) FROM durable_blob_references)",
                [],
                |row| {
                    Ok((
                        row.get(0)?,
                        row.get(1)?,
                        row.get(2)?,
                        row.get(3)?,
                        row.get(4)?,
                        row.get(5)?,
                        row.get(6)?,
                    ))
                },
            )
            .map_err(|_| {
                coded_error(
                    "startup-recovery-count-unavailable",
                    "cannot count startup recovery rows",
                )
            })?;
        let checked_projects = to_u64(counts.0, "startup project count")?;
        let checked_project_roots = to_u64(counts.1, "startup project root count")?;
        let checked_sessions = to_u64(counts.2, "startup session count")?;
        let checked_turns = to_u64(counts.3, "startup turn count")?;
        let checked_items = to_u64(counts.4, "startup item count")?;
        let checked_events = to_u64(counts.5, "startup event count")?;
        let checked_blob_references = to_u64(counts.6, "startup blob reference count")?;
        if checked_projects > MAX_STARTUP_RECOVERY_PROJECTS
            || checked_project_roots > MAX_STARTUP_RECOVERY_ROWS
            || checked_sessions > MAX_STARTUP_RECOVERY_SESSIONS
            || checked_turns > MAX_STARTUP_RECOVERY_ROWS
            || checked_items > MAX_STARTUP_RECOVERY_ROWS
            || checked_events > MAX_STARTUP_RECOVERY_ROWS
            || checked_blob_references > MAX_STARTUP_RECOVERY_ROWS
        {
            return Err(coded_error(
                "startup-recovery-limit-exceeded",
                "startup recovery row limit exceeded",
            ));
        }

        let project_ids = {
            let mut statement = self
                .connection
                .prepare(
                    "SELECT project_id FROM projects
                     UNION
                     SELECT project_id FROM project_roots
                     UNION
                     SELECT project_id FROM events
                        WHERE project_id IS NOT NULL AND event_kind LIKE 'project.%'
                     ORDER BY project_id
                     LIMIT ?1",
                )
                .map_err(|_| {
                    coded_error(
                        "startup-recovery-scan-unavailable",
                        "cannot prepare startup project recovery scan",
                    )
                })?;
            let rows = statement
                .query_map(
                    [to_i64(
                        MAX_STARTUP_RECOVERY_PROJECTS + 1,
                        "startup project limit",
                    )?],
                    |row| row.get::<_, String>(0),
                )
                .map_err(|_| {
                    coded_error(
                        "startup-recovery-scan-unavailable",
                        "cannot read startup project recovery scan",
                    )
                })?;
            rows.collect::<Result<Vec<_>, _>>().map_err(|_| {
                coded_error(
                    "startup-recovery-scan-invalid",
                    "startup recovery project identity is invalid",
                )
            })?
        };
        if project_ids.len() as u64 > MAX_STARTUP_RECOVERY_PROJECTS {
            return Err(coded_error(
                "startup-recovery-limit-exceeded",
                "startup recovery project limit exceeded",
            ));
        }
        for project_id in &project_ids {
            validate_identifier(project_id, "startup recovery project ID").map_err(|_| {
                coded_error(
                    "startup-recovery-project-invalid",
                    "startup recovery project identity is invalid",
                )
            })?;
        }

        self.quarantined_projects.clear();
        let mut healthy_projects = 0_u64;
        let mut rebuilt_projects = 0_u64;
        for project_id in &project_ids {
            match self.verify_or_rebuild_project_projection(project_id) {
                Ok(true) => rebuilt_projects = rebuilt_projects.saturating_add(1),
                Ok(false) => healthy_projects = healthy_projects.saturating_add(1),
                Err(_) => {
                    self.quarantined_projects.insert(project_id.clone());
                }
            }
        }

        let session_ids = {
            let mut statement = self
                .connection
                .prepare(
                    "SELECT session_id FROM sessions AS candidate_session
                     WHERE NOT EXISTS (
                         SELECT 1
                         FROM session_deletion_members AS deletion_member
                         JOIN session_deletions AS deletion
                           ON deletion.deletion_id = deletion_member.deletion_id
                         WHERE deletion_member.session_id = candidate_session.session_id
                           AND deletion.state = 'purged'
                     )
                     UNION
                     SELECT session_id FROM session_projection_sources
                     UNION
                     SELECT session_id FROM events
                        WHERE event_kind NOT LIKE 'project.%'
                          AND event_kind NOT LIKE 'retention.%'
                     ORDER BY session_id
                     LIMIT ?1",
                )
                .map_err(|_| {
                    coded_error(
                        "startup-recovery-scan-unavailable",
                        "cannot prepare startup recovery scan",
                    )
                })?;
            let rows = statement
                .query_map(
                    [to_i64(
                        MAX_STARTUP_RECOVERY_SESSIONS + 1,
                        "startup session limit",
                    )?],
                    |row| row.get::<_, String>(0),
                )
                .map_err(|_| {
                    coded_error(
                        "startup-recovery-scan-unavailable",
                        "cannot read startup recovery scan",
                    )
                })?;
            rows.collect::<Result<Vec<_>, _>>().map_err(|_| {
                coded_error(
                    "startup-recovery-scan-invalid",
                    "startup recovery session identity is invalid",
                )
            })?
        };
        if session_ids.len() as u64 > MAX_STARTUP_RECOVERY_SESSIONS {
            return Err(coded_error(
                "startup-recovery-limit-exceeded",
                "startup recovery session limit exceeded",
            ));
        }
        for session_id in &session_ids {
            validate_identifier(session_id, "startup recovery session ID").map_err(|_| {
                coded_error(
                    "startup-recovery-session-invalid",
                    "startup recovery session identity is invalid",
                )
            })?;
        }

        self.quarantined_sessions.clear();
        self.startup_rebuilt_sessions.clear();
        let mut healthy_sessions = 0_u64;
        let mut rebuilt_sessions = 0_u64;
        let mut missing_candidates = BTreeMap::<String, Option<String>>::new();
        for session_id in &session_ids {
            let session_project_id = self
                .connection
                .query_row(
                    "SELECT project_id FROM sessions WHERE session_id = ?1",
                    [session_id],
                    |row| row.get::<_, Option<String>>(0),
                )
                .optional()
                .map_err(|_| {
                    coded_error(
                        "startup-recovery-scan-unavailable",
                        "cannot inspect startup session project",
                    )
                })?
                .flatten();
            if session_project_id
                .as_ref()
                .is_some_and(|project_id| self.quarantined_projects.contains(project_id))
            {
                self.quarantined_sessions.insert(session_id.clone());
                continue;
            }
            let exists = self
                .connection
                .query_row(
                    "SELECT EXISTS(SELECT 1 FROM sessions WHERE session_id = ?1)",
                    [session_id],
                    |row| row.get::<_, bool>(0),
                )
                .map_err(|_| {
                    coded_error(
                        "startup-recovery-scan-unavailable",
                        "cannot inspect startup session projection",
                    )
                })?;
            if exists {
                match self.verify_or_rebuild_session_projection(session_id) {
                    Ok((report, true)) if report.consistent => {
                        rebuilt_sessions = rebuilt_sessions.saturating_add(1);
                        self.startup_rebuilt_sessions.insert(session_id.clone());
                    }
                    Ok((report, false)) if report.consistent => {
                        healthy_sessions = healthy_sessions.saturating_add(1);
                    }
                    _ => {
                        self.quarantined_sessions.insert(session_id.clone());
                    }
                }
                continue;
            }
            match self.rebuild_session_projection_candidate(session_id) {
                Ok(candidate) if projection_candidate_is_rebuildable(&candidate) => {
                    if candidate
                        .session
                        .as_ref()
                        .and_then(|session| session.project_id.as_ref())
                        .is_some_and(|project_id| self.quarantined_projects.contains(project_id))
                    {
                        self.quarantined_sessions.insert(session_id.clone());
                        continue;
                    }
                    let parent_session_id = candidate
                        .session
                        .as_ref()
                        .and_then(|session| session.parent_session_id.clone());
                    missing_candidates.insert(session_id.clone(), parent_session_id);
                }
                _ => {
                    self.quarantined_sessions.insert(session_id.clone());
                }
            }
        }

        while !missing_candidates.is_empty() {
            let ready = missing_candidates
                .iter()
                .filter(|(_, parent)| {
                    parent
                        .as_ref()
                        .is_none_or(|parent| !missing_candidates.contains_key(parent))
                })
                .map(|(session_id, _)| session_id.clone())
                .collect::<Vec<_>>();
            if ready.is_empty() {
                break;
            }
            for session_id in ready {
                let parent = missing_candidates.remove(&session_id).flatten();
                if parent
                    .as_ref()
                    .is_some_and(|parent| self.quarantined_sessions.contains(parent))
                {
                    self.quarantined_sessions.insert(session_id);
                    continue;
                }
                match self.rebuild_session_projection(&session_id) {
                    Ok(report) if report.consistent => {
                        rebuilt_sessions = rebuilt_sessions.saturating_add(1);
                        self.startup_rebuilt_sessions.insert(session_id.clone());
                    }
                    _ => {
                        self.quarantined_sessions.insert(session_id);
                    }
                }
            }
        }
        self.quarantined_sessions
            .extend(missing_candidates.into_keys());
        for project_id in &self.quarantined_projects {
            let mut statement = self
                .connection
                .prepare("SELECT session_id FROM sessions WHERE project_id = ?1")
                .map_err(|_| {
                    coded_error(
                        "startup-recovery-scan-unavailable",
                        "cannot prepare quarantined project session scan",
                    )
                })?;
            let rows = statement
                .query_map([project_id], |row| row.get::<_, String>(0))
                .map_err(|_| {
                    coded_error(
                        "startup-recovery-scan-unavailable",
                        "cannot read quarantined project sessions",
                    )
                })?;
            self.quarantined_sessions
                .extend(rows.collect::<Result<Vec<_>, _>>().map_err(|_| {
                    coded_error(
                        "startup-recovery-scan-invalid",
                        "quarantined project session identity is invalid",
                    )
                })?);
        }
        let quarantined_projects = self.quarantined_projects.len() as u64;
        let quarantined_sessions = self.quarantined_sessions.len() as u64;
        Ok(StartupProjectionRecoveryReport {
            schema_version: "startup-projection-recovery/0.2".into(),
            checked_projects: project_ids.len() as u64,
            healthy_projects,
            rebuilt_projects,
            quarantined_projects,
            checked_project_roots,
            checked_sessions: session_ids.len() as u64,
            healthy_sessions,
            rebuilt_sessions,
            quarantined_sessions,
            checked_turns,
            checked_items,
            checked_events,
            checked_blob_references,
            issues: {
                let mut issues = Vec::new();
                if quarantined_projects > 0 {
                    issues.push("startup-project-projection-quarantined".into());
                }
                if quarantined_sessions > 0 {
                    issues.push("startup-session-projection-quarantined".into());
                }
                issues
            },
        })
    }

    pub fn rebuild_project_projection_candidate(
        &self,
        project_id: &str,
    ) -> Result<ProjectProjectionCandidate, WorkbenchStoreError> {
        validate_identifier(project_id, "project ID")?;
        let stream_id = project_event_stream_id(project_id);
        let source_registered = self
            .connection
            .query_row(
                "SELECT EXISTS(
                    SELECT 1 FROM session_sequences WHERE session_id = ?1
                 )",
                [&stream_id],
                |row| row.get::<_, bool>(0),
            )
            .map_err(|_| error("cannot read project projection source registration"))?;
        let event_count = query_count(
            &self.connection,
            "SELECT COUNT(*) FROM events WHERE session_id = ?1",
            &stream_id,
            "cannot count project projection source events",
        )?;
        let mut issues = Vec::new();
        let next_event_sequence = self
            .connection
            .query_row(
                "SELECT next_sequence FROM session_sequences WHERE session_id = ?1",
                [&stream_id],
                |row| row.get::<_, i64>(0),
            )
            .optional()
            .map_err(|_| error("cannot read project projection event cursor"))?
            .map(|value| to_u64(value, "project projection event cursor"))
            .transpose()?;
        if source_registered && next_event_sequence != event_count.checked_add(1) {
            push_projection_issue(&mut issues, "project-event-cursor-mismatch");
        }
        if event_count > MAX_PROJECTION_VERIFY_ROWS {
            push_projection_issue(&mut issues, "project-projection-event-limit-exceeded");
            return Ok(ProjectProjectionCandidate {
                schema_version: "project-projection-candidate/0.1".into(),
                project_id: project_id.into(),
                stream_id,
                source_event_count: event_count,
                source_hash: None,
                source_complete: false,
                matches_current_projection: false,
                project: None,
                roots: Vec::new(),
                issues,
            });
        }
        let events = load_projection_source_events(&self.connection, &stream_id)?;
        let source_hash = projection_event_stream_hash(&events);
        let mut project: Option<StoredProject> = None;
        let mut roots = BTreeMap::<String, StoredProjectRoot>::new();
        for event in &events {
            if event.project_id.as_deref() != Some(project_id) {
                push_projection_issue(&mut issues, "project-event-binding-invalid");
                continue;
            }
            match event.event_kind.as_str() {
                "project.created" => {
                    if projection_event_schema(event) != Some("project.created/0.1") {
                        push_projection_issue(&mut issues, "project-created-event-invalid");
                        continue;
                    }
                    let parsed_project = event
                        .payload
                        .get("project")
                        .cloned()
                        .and_then(|value| serde_json::from_value::<StoredProject>(value).ok());
                    let Some(parsed_project) = parsed_project else {
                        push_projection_issue(&mut issues, "project-created-event-invalid");
                        continue;
                    };
                    let parsed_root = event.payload.get("root").and_then(|value| {
                        serde_json::from_value::<StoredProjectRoot>(value.clone())
                            .ok()
                            .or_else(|| {
                                Some(StoredProjectRoot {
                                    project_id: parsed_project.project_id.clone(),
                                    root_id: value.get("root_id")?.as_str()?.into(),
                                    canonical_root: parsed_project.canonical_root.clone(),
                                    root_identity: parsed_project.root_identity.clone(),
                                    access: value.get("access")?.as_str()?.into(),
                                    created_at_ms: parsed_project.created_at_ms,
                                })
                            })
                    });
                    let Some(parsed_root) = parsed_root else {
                        push_projection_issue(&mut issues, "project-created-event-invalid");
                        continue;
                    };
                    if project.is_some()
                        || parsed_project.project_id != project_id
                        || parsed_project.state != "active"
                        || parsed_project.updated_at_ms != parsed_project.created_at_ms
                        || validate_rebuilt_project(&parsed_project).is_err()
                        || validate_project_root(&parsed_root).is_err()
                        || parsed_root.project_id != project_id
                        || parsed_root.canonical_root != parsed_project.canonical_root
                        || parsed_root.root_identity != parsed_project.root_identity
                        || roots
                            .insert(parsed_root.root_id.clone(), parsed_root)
                            .is_some()
                    {
                        push_projection_issue(&mut issues, "project-created-event-invalid");
                        continue;
                    }
                    project = Some(parsed_project);
                }
                "project.root-added" => {
                    if projection_event_schema(event) != Some("project.root-added/0.1") {
                        push_projection_issue(&mut issues, "project-root-event-invalid");
                        continue;
                    }
                    let parsed_root =
                        event.payload.get("root").cloned().and_then(|value| {
                            serde_json::from_value::<StoredProjectRoot>(value).ok()
                        });
                    let updated_at_ms = event
                        .payload
                        .get("updated_at_ms")
                        .and_then(serde_json::Value::as_u64);
                    match (project.as_mut(), parsed_root, updated_at_ms) {
                        (Some(project), Some(root), Some(updated_at_ms))
                            if validate_project_root(&root).is_ok()
                                && root.project_id == project_id
                                && updated_at_ms >= project.updated_at_ms
                                && root.created_at_ms == updated_at_ms
                                && !roots.contains_key(&root.root_id)
                                && !roots.values().any(|existing| {
                                    existing.canonical_root == root.canonical_root
                                }) =>
                        {
                            project.updated_at_ms = updated_at_ms;
                            roots.insert(root.root_id.clone(), root);
                        }
                        _ => push_projection_issue(&mut issues, "project-root-event-invalid"),
                    }
                }
                "project.root-removed" => {
                    if projection_event_schema(event) != Some("project.root-removed/0.1") {
                        push_projection_issue(&mut issues, "project-root-event-invalid");
                        continue;
                    }
                    let root_id = event
                        .payload
                        .get("root_id")
                        .and_then(serde_json::Value::as_str);
                    let updated_at_ms = event
                        .payload
                        .get("updated_at_ms")
                        .and_then(serde_json::Value::as_u64);
                    let valid = root_id
                        .is_some_and(|root_id| root_id != "root-1" && roots.contains_key(root_id))
                        && event
                            .payload
                            .get("project_id")
                            .and_then(serde_json::Value::as_str)
                            == Some(project_id);
                    match (project.as_mut(), root_id, updated_at_ms, valid) {
                        (Some(project), Some(root_id), Some(updated_at_ms), true)
                            if updated_at_ms >= project.updated_at_ms && roots.len() > 1 =>
                        {
                            project.updated_at_ms = updated_at_ms;
                            roots.remove(root_id);
                        }
                        _ => push_projection_issue(&mut issues, "project-root-event-invalid"),
                    }
                }
                "project.root-relinked" => {
                    if projection_event_schema(event) != Some("project.root-relinked/0.1") {
                        push_projection_issue(&mut issues, "project-root-relink-event-invalid");
                        continue;
                    }
                    let root_id = event
                        .payload
                        .get("root_id")
                        .and_then(serde_json::Value::as_str);
                    let previous_canonical_root = event
                        .payload
                        .get("previous_canonical_root")
                        .and_then(serde_json::Value::as_str);
                    let previous_root_identity = event
                        .payload
                        .get("previous_root_identity")
                        .and_then(serde_json::Value::as_str);
                    let canonical_root = event
                        .payload
                        .get("canonical_root")
                        .and_then(serde_json::Value::as_str);
                    let root_identity = event
                        .payload
                        .get("root_identity")
                        .and_then(serde_json::Value::as_str);
                    let access = event
                        .payload
                        .get("access")
                        .and_then(serde_json::Value::as_str);
                    let updated_at_ms = event
                        .payload
                        .get("updated_at_ms")
                        .and_then(serde_json::Value::as_u64);
                    let valid = root_id.is_some_and(|root_id| roots.contains_key(root_id))
                        && previous_canonical_root
                            .is_some_and(|value| validate_stored_canonical_root(value).is_ok())
                        && canonical_root
                            .is_some_and(|value| validate_stored_canonical_root(value).is_ok())
                        && previous_root_identity.is_some_and(|value| {
                            validate_identity_text(value, "previous root identity").is_ok()
                        })
                        && root_identity.is_some_and(|value| {
                            validate_identity_text(value, "project root identity").is_ok()
                        })
                        && access.is_some_and(|value| matches!(value, "read" | "write"))
                        && event
                            .payload
                            .get("project_id")
                            .and_then(serde_json::Value::as_str)
                            == Some(project_id);
                    match (
                        project.as_mut(),
                        root_id,
                        previous_canonical_root,
                        previous_root_identity,
                        canonical_root,
                        root_identity,
                        access,
                        updated_at_ms,
                        valid,
                    ) {
                        (
                            Some(project),
                            Some(root_id),
                            Some(previous_canonical_root),
                            Some(previous_root_identity),
                            Some(canonical_root),
                            Some(root_identity),
                            Some(access),
                            Some(updated_at_ms),
                            true,
                        ) if updated_at_ms >= project.updated_at_ms
                            && roots.get(root_id).is_some_and(|root| {
                                root.canonical_root == previous_canonical_root
                                    && root.root_identity == previous_root_identity
                                    && root.access == access
                            })
                            && !roots.iter().any(|(existing_id, root)| {
                                existing_id != root_id
                                    && (root.canonical_root == canonical_root
                                        || root.root_identity == root_identity)
                            }) =>
                        {
                            if let Some(root) = roots.get_mut(root_id) {
                                root.canonical_root = canonical_root.into();
                                root.root_identity = root_identity.into();
                            }
                            if root_id == "root-1" {
                                project.canonical_root = canonical_root.into();
                                project.root_identity = root_identity.into();
                            }
                            project.updated_at_ms = updated_at_ms;
                        }
                        _ => {
                            push_projection_issue(&mut issues, "project-root-relink-event-invalid")
                        }
                    }
                }
                "project.root-identity-migrated" => {
                    if projection_event_schema(event) != Some("project.root-identity/0.1") {
                        push_projection_issue(&mut issues, "project-root-identity-event-invalid");
                        continue;
                    }
                    let root_id = event
                        .payload
                        .get("root_id")
                        .and_then(serde_json::Value::as_str);
                    let root_identity = event
                        .payload
                        .get("root_identity")
                        .and_then(serde_json::Value::as_str);
                    let updated_at_ms = event
                        .payload
                        .get("updated_at_ms")
                        .and_then(serde_json::Value::as_u64);
                    let valid = root_id.is_some_and(|root_id| roots.contains_key(root_id))
                        && root_identity.is_some_and(|identity| {
                            validate_identity_text(identity, "project root identity").is_ok()
                        })
                        && event
                            .payload
                            .get("project_id")
                            .and_then(serde_json::Value::as_str)
                            == Some(project_id);
                    match (
                        project.as_mut(),
                        root_id,
                        root_identity,
                        updated_at_ms,
                        valid,
                    ) {
                        (
                            Some(project),
                            Some(root_id),
                            Some(root_identity),
                            Some(updated_at_ms),
                            true,
                        ) if updated_at_ms >= project.updated_at_ms => {
                            project.root_identity = root_identity.into();
                            project.updated_at_ms = updated_at_ms;
                            if let Some(root) = roots.get_mut(root_id) {
                                root.root_identity = root_identity.into();
                            }
                        }
                        _ => push_projection_issue(
                            &mut issues,
                            "project-root-identity-event-invalid",
                        ),
                    }
                }
                "project.navigation-updated" => {
                    if projection_event_schema(event) != Some("project.navigation-updated/0.1")
                        || event
                            .payload
                            .get("project_id")
                            .and_then(serde_json::Value::as_str)
                            != Some(project_id)
                        || event
                            .payload
                            .get("pinned")
                            .and_then(serde_json::Value::as_bool)
                            .is_none()
                        || event
                            .payload
                            .get("last_opened_at_ms")
                            .and_then(serde_json::Value::as_u64)
                            .is_none()
                    {
                        push_projection_issue(&mut issues, "project-navigation-event-invalid");
                    }
                }
                "project.trust-acknowledged" => {
                    let review_id = event
                        .payload
                        .get("review_id")
                        .and_then(serde_json::Value::as_str);
                    let root_id = event
                        .payload
                        .get("root_id")
                        .and_then(serde_json::Value::as_str);
                    let root_identity = event
                        .payload
                        .get("root_identity")
                        .and_then(serde_json::Value::as_str);
                    let valid = projection_event_schema(event)
                        == Some("project.trust-acknowledged/0.1")
                        && event
                            .payload
                            .get("project_id")
                            .and_then(serde_json::Value::as_str)
                            == Some(project_id)
                        && event
                            .payload
                            .get("acknowledged_at_ms")
                            .and_then(serde_json::Value::as_u64)
                            .is_some()
                        && event
                            .payload
                            .get("permission_effect")
                            .and_then(serde_json::Value::as_str)
                            == Some("none-read-only-boundary-unchanged")
                        && root_id.is_some_and(|value| roots.contains_key(value))
                        && root_identity.is_some_and(|value| {
                            validate_identity_text(value, "project root identity").is_ok()
                        })
                        && root_id
                            .zip(root_identity)
                            .is_some_and(|(root_id, identity)| {
                                roots
                                    .get(root_id)
                                    .is_some_and(|root| root.root_identity == identity)
                            })
                        && review_id.is_some_and(|value| validate_trust_review_id(value).is_ok());
                    if !valid {
                        push_projection_issue(&mut issues, "project-trust-event-invalid");
                    }
                }
                "project.pinned-context-updated" => {
                    let set_identity = event
                        .payload
                        .get("set_identity")
                        .and_then(serde_json::Value::as_str);
                    let object_reference = event
                        .payload
                        .get("object_reference")
                        .and_then(serde_json::Value::as_str);
                    let item_count = event
                        .payload
                        .get("item_count")
                        .and_then(serde_json::Value::as_u64);
                    let persisted_at_ms = event
                        .payload
                        .get("persisted_at_ms")
                        .and_then(serde_json::Value::as_u64);
                    let release_batch_identity = event
                        .payload
                        .get("release_batch_identity")
                        .and_then(serde_json::Value::as_str);
                    let released_blob_reference_count = event
                        .payload
                        .get("released_blob_reference_count")
                        .and_then(serde_json::Value::as_u64);
                    let valid = match (
                        project.as_ref(),
                        set_identity,
                        object_reference,
                        item_count,
                        persisted_at_ms,
                    ) {
                        (
                            Some(project),
                            Some(set_identity),
                            Some(object_reference),
                            Some(item_count),
                            Some(persisted_at_ms),
                        ) => {
                            let release_batch_valid =
                                match (release_batch_identity, released_blob_reference_count) {
                                    (None, None) => true,
                                    (Some(identity), Some(count)) => {
                                        count > 0
                                            && count <= 128
                                            && validate_pinned_context_reference(
                                                identity,
                                                "pinned-context-release:sha256:",
                                                "pinned context release batch identity",
                                            )
                                            .is_ok()
                                    }
                                    _ => false,
                                };
                            let event_seed = release_batch_identity.map_or_else(
                                || format!("{project_id}:{set_identity}"),
                                |identity| format!("{project_id}:{set_identity}:{identity}"),
                            );
                            projection_event_schema(event)
                                == Some("project.pinned-context-updated/0.1")
                                && event
                                    .payload
                                    .get("project_id")
                                    .and_then(serde_json::Value::as_str)
                                    == Some(project_id)
                                && event
                                    .payload
                                    .get("content_bodies_persisted")
                                    .and_then(serde_json::Value::as_bool)
                                    == Some(false)
                                && event
                                    .payload
                                    .get("state")
                                    .and_then(serde_json::Value::as_str)
                                    == Some("metadata-persisted")
                                && validate_pinned_context_reference(
                                    set_identity,
                                    "pinned-context:sha256:",
                                    "pinned context set identity",
                                )
                                .is_ok()
                                && validate_pinned_context_reference(
                                    object_reference,
                                    "pinned-context-object:sha256:",
                                    "pinned context object reference",
                                )
                                .is_ok()
                                && item_count <= 128
                                && release_batch_valid
                                && persisted_at_ms == event.timestamp_ms
                                && persisted_at_ms >= project.created_at_ms
                                && event.event_id
                                    == derived_event_id(
                                        "pinned-context-updated",
                                        event_seed.as_bytes(),
                                    )
                                && event.correlation_id == set_identity
                                && event.operation_id
                                    == pinned_context_operation_id(project_id, set_identity)
                                && event.generation == 0
                                && event.project_id.as_deref() == Some(project_id)
                        }
                        _ => false,
                    };
                    if !valid {
                        push_projection_issue(&mut issues, "project-pinned-context-event-invalid");
                    }
                }
                "project.projection-rebuilt" => {
                    if projection_event_schema(event) != Some("project.projection-rebuilt/0.1") {
                        push_projection_issue(&mut issues, "project-audit-event-invalid");
                    }
                }
                _ => push_projection_issue(&mut issues, "project-event-kind-invalid"),
            }
        }
        if source_registered && project.is_none() {
            push_projection_issue(&mut issues, "project-event-source-incomplete");
        }
        let roots = roots.into_values().collect::<Vec<_>>();
        let source_complete = source_registered
            && project.is_some()
            && !roots.is_empty()
            && !issues
                .iter()
                .any(|issue| issue != "project-event-projection-mismatch");
        let mut matches_current_projection = false;
        if source_complete {
            matches_current_projection = self.load_project(project_id).ok() == project
                && self.load_project_roots(project_id).ok().as_ref() == Some(&roots);
            if !matches_current_projection {
                push_projection_issue(&mut issues, "project-event-projection-mismatch");
            }
        }
        Ok(ProjectProjectionCandidate {
            schema_version: "project-projection-candidate/0.1".into(),
            project_id: project_id.into(),
            stream_id,
            source_event_count: event_count,
            source_hash: Some(source_hash),
            source_complete,
            matches_current_projection,
            project,
            roots,
            issues,
        })
    }

    fn verify_or_rebuild_project_projection(
        &mut self,
        project_id: &str,
    ) -> Result<bool, WorkbenchStoreError> {
        let stream_id = project_event_stream_id(project_id);
        let source_registered = self
            .connection
            .query_row(
                "SELECT EXISTS(
                    SELECT 1 FROM session_sequences WHERE session_id = ?1
                 )",
                [&stream_id],
                |row| row.get::<_, bool>(0),
            )
            .map_err(|_| error("cannot inspect project projection source"))?;
        if !source_registered {
            self.verify_legacy_project_projection(project_id)?;
            self.quarantined_projects.remove(project_id);
            return Ok(false);
        }
        let candidate = self.rebuild_project_projection_candidate(project_id)?;
        if candidate.source_complete && candidate.matches_current_projection {
            self.quarantined_projects.remove(project_id);
            return Ok(false);
        }
        if !project_projection_candidate_is_rebuildable(&candidate) {
            return Err(error("project projection is not safely rebuildable"));
        }
        self.apply_project_projection_candidate(&candidate)?;
        let verified = self.rebuild_project_projection_candidate(project_id)?;
        if !verified.source_complete || !verified.matches_current_projection {
            return Err(error("rebuilt project projection did not verify"));
        }
        self.quarantined_projects.remove(project_id);
        Ok(true)
    }

    fn verify_legacy_project_projection(
        &self,
        project_id: &str,
    ) -> Result<(), WorkbenchStoreError> {
        let project = self.load_project(project_id)?;
        validate_rebuilt_project(&project)?;
        let roots = self.load_project_roots(project_id)?;
        if roots.is_empty() || roots.len() as u64 > MAX_PROJECTION_VERIFY_ROWS {
            return Err(error("legacy project roots are incomplete"));
        }
        for root in &roots {
            validate_project_root(root)?;
            if root.project_id != project_id {
                return Err(error("legacy project root owner is invalid"));
            }
        }
        if !roots.iter().any(|root| {
            root.canonical_root == project.canonical_root
                && root.root_identity == project.root_identity
        }) {
            return Err(error("legacy project primary root is missing"));
        }
        Ok(())
    }

    fn apply_project_projection_candidate(
        &mut self,
        candidate: &ProjectProjectionCandidate,
    ) -> Result<(), WorkbenchStoreError> {
        if !project_projection_candidate_is_rebuildable(candidate) {
            return Err(error("project projection candidate is not rebuildable"));
        }
        let project = candidate
            .project
            .as_ref()
            .ok_or_else(|| error("project projection candidate has no project"))?;
        validate_rebuilt_project(project)?;
        if project.project_id != candidate.project_id || candidate.roots.is_empty() {
            return Err(error("project projection candidate identity is invalid"));
        }
        let mut root_ids = BTreeSet::new();
        let mut root_paths = BTreeSet::new();
        for root in &candidate.roots {
            validate_project_root(root)?;
            if root.project_id != candidate.project_id
                || !root_ids.insert(root.root_id.clone())
                || !root_paths.insert(root.canonical_root.clone())
            {
                return Err(error("project projection candidate root is invalid"));
            }
        }
        let transaction = self.begin_database_write("cannot start project projection rebuild")?;
        let current_events = load_projection_source_events(&transaction, &candidate.stream_id)?;
        let current_hash = projection_event_stream_hash(&current_events);
        if current_events.len() as u64 != candidate.source_event_count
            || Some(&current_hash) != candidate.source_hash.as_ref()
        {
            return Err(error("project projection source changed after review"));
        }
        transaction
            .execute(
                "DELETE FROM project_roots WHERE project_id = ?1",
                [&candidate.project_id],
            )
            .map_err(|_| error("cannot clear project root projection for rebuild"))?;
        projection_rebuild_crash_point("project-after-root-clear");
        transaction
            .execute(
                "INSERT INTO projects (
                    project_id, canonical_root, root_identity, display_name, state,
                    created_at_ms, updated_at_ms
                 ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
                 ON CONFLICT(project_id) DO UPDATE SET
                    canonical_root = excluded.canonical_root,
                    root_identity = excluded.root_identity,
                    display_name = excluded.display_name,
                    state = excluded.state,
                    created_at_ms = excluded.created_at_ms,
                    updated_at_ms = excluded.updated_at_ms",
                params![
                    project.project_id,
                    project.canonical_root,
                    project.root_identity,
                    project.display_name,
                    project.state,
                    to_i64(project.created_at_ms, "project creation time")?,
                    to_i64(project.updated_at_ms, "project update time")?,
                ],
            )
            .map_err(|_| error("cannot install rebuilt project projection"))?;
        for root in &candidate.roots {
            transaction
                .execute(
                    "INSERT INTO project_roots (
                        project_id, root_id, canonical_root, root_identity, access, created_at_ms
                     ) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                    params![
                        root.project_id,
                        root.root_id,
                        root.canonical_root,
                        root.root_identity,
                        root.access,
                        to_i64(root.created_at_ms, "project root creation time")?,
                    ],
                )
                .map_err(|_| error("cannot install rebuilt project root projection"))?;
        }
        let source_hash = candidate
            .source_hash
            .as_ref()
            .ok_or_else(|| error("project projection source hash is missing"))?;
        let event_id = derived_event_id(
            "project-projection-rebuilt",
            format!("{}\0{}", candidate.project_id, source_hash.sha256).as_bytes(),
        );
        append_event_tx(
            &transaction,
            EventInput {
                session_id: &candidate.stream_id,
                event_id: &event_id,
                timestamp_ms: project.updated_at_ms,
                correlation_id: &candidate.project_id,
                event_kind: "project.projection-rebuilt",
                project_id: Some(&candidate.project_id),
                operation_id: &candidate.project_id,
                generation: 0,
                payload: json!({
                    "schema_version": "project.projection-rebuilt/0.1",
                    "source_hash": source_hash,
                    "root_count": candidate.roots.len()
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit project projection rebuild"))
    }

    pub fn rebuild_session_projection_candidate(
        &self,
        session_id: &str,
    ) -> Result<SessionProjectionCandidate, WorkbenchStoreError> {
        validate_identifier(session_id, "session ID")?;
        let source_registered = self
            .connection
            .query_row(
                "SELECT EXISTS(
                    SELECT 1 FROM session_projection_sources
                    WHERE session_id = ?1 AND source_version = 1
                 )",
                [session_id],
                |row| row.get::<_, bool>(0),
            )
            .map_err(|_| error("cannot read session projection source registration"))?;
        let (event_count, _, _) = query_sequence_summary(
            &self.connection,
            "SELECT COUNT(*), COALESCE(MIN(sequence), 0), COALESCE(MAX(sequence), 0)
             FROM events WHERE session_id = ?1",
            session_id,
            "cannot summarize projection source events",
        )?;
        let mut issues = Vec::new();
        if event_count > MAX_PROJECTION_VERIFY_ROWS {
            push_projection_issue(&mut issues, "projection-event-limit-exceeded");
            return Ok(SessionProjectionCandidate {
                schema_version: "session-projection-candidate/0.1".into(),
                session_id: session_id.into(),
                source_event_count: event_count,
                source_hash: None,
                source_complete: false,
                matches_current_projection: false,
                session: None,
                turns: Vec::new(),
                items: Vec::new(),
                issues,
            });
        }

        let mut events = Vec::new();
        let mut after_sequence = 0_u64;
        while events.len() < event_count as usize {
            let remaining =
                usize::try_from((event_count - events.len() as u64).min(MAX_EVENT_PAGE as u64))
                    .map_err(|_| error("projection event page is invalid"))?;
            let page = self.read_session_events(session_id, after_sequence, remaining)?;
            if page.is_empty() {
                return Err(error("projection event replay ended before its boundary"));
            }
            after_sequence = page.last().map_or(after_sequence, |event| event.sequence);
            events.extend(page);
        }
        let source_hash = projection_event_stream_hash(&events);

        let mut session: Option<StoredSession> = None;
        let mut turns = BTreeMap::<String, StoredTurn>::new();
        let mut items = Vec::<StoredItem>::new();
        let mut compaction_checkpoints = BTreeMap::<String, (String, String)>::new();
        let mut pending_turn_traces = BTreeMap::<String, StoredTurnTrace>::new();
        for event in &events {
            match event.event_kind.as_str() {
                "session.created" => {
                    if projection_event_schema(event) != Some("session.created/0.1") {
                        push_projection_issue(&mut issues, "session-created-event-invalid");
                        continue;
                    }
                    let parsed = event
                        .payload
                        .get("session")
                        .cloned()
                        .and_then(|value| serde_json::from_value::<StoredSession>(value).ok());
                    let Some(parsed) = parsed else {
                        push_projection_issue(&mut issues, "session-created-event-invalid");
                        continue;
                    };
                    if session.is_some()
                        || parsed.session_id != session_id
                        || parsed.status != "active"
                        || parsed.updated_at_ms != parsed.created_at_ms
                        || validate_rebuilt_session(&parsed).is_err()
                    {
                        push_projection_issue(&mut issues, "session-created-event-invalid");
                        continue;
                    }
                    session = Some(parsed);
                }
                "session.runtime-bound" => {
                    let valid = projection_event_schema(event) == Some("session.runtime-bound/0.1")
                        && event
                            .payload
                            .get("session_id")
                            .and_then(serde_json::Value::as_str)
                            == Some(session_id)
                        && event
                            .payload
                            .get("adapter")
                            .and_then(serde_json::Value::as_str)
                            .is_some()
                        && event
                            .payload
                            .get("adapter_version")
                            .and_then(serde_json::Value::as_str)
                            .is_some()
                        && event
                            .payload
                            .get("permission_profile")
                            .and_then(serde_json::Value::as_str)
                            == Some("read-only")
                        && event
                            .payload
                            .get("binding_hash")
                            .and_then(|value| {
                                serde_json::from_value::<ContentHash>(value.clone()).ok()
                            })
                            .is_some();
                    if !valid {
                        push_projection_issue(&mut issues, "session-runtime-binding-event-invalid");
                    }
                }
                "session.workspace-bound" => {
                    let binding = event.payload.get("binding").cloned().and_then(|value| {
                        serde_json::from_value::<StoredSessionWorkspaceBindingCreate>(value).ok()
                    });
                    let binding_hash = event
                        .payload
                        .get("binding_hash")
                        .cloned()
                        .and_then(|value| serde_json::from_value::<ContentHash>(value).ok());
                    let valid = binding.as_ref().is_some_and(|binding| {
                        let serialized = serde_json::to_vec(binding).ok();
                        projection_event_schema(event) == Some("session.workspace-bound/0.1")
                            && validate_session_workspace_binding_create(
                                binding,
                                session_id,
                                event.project_id.as_deref(),
                            )
                            .is_ok()
                            && serialized.as_deref().is_some_and(|serialized| {
                                binding_hash.as_ref() == Some(&ContentHash::for_bytes(serialized))
                            })
                            && event.event_id
                                == derived_event_id(
                                    "session-workspace-bound",
                                    session_id.as_bytes(),
                                )
                            && event.correlation_id == session_id
                            && event.operation_id == session_id
                            && event.generation == 0
                            && event.timestamp_ms == binding.captured_at_ms
                            && event
                                .payload
                                .get("raw_paths_persisted")
                                .and_then(Value::as_bool)
                                == Some(false)
                            && event
                                .payload
                                .get("permission_granted")
                                .and_then(Value::as_bool)
                                == Some(false)
                    });
                    if !valid {
                        push_projection_issue(
                            &mut issues,
                            "session-workspace-binding-event-invalid",
                        );
                    }
                }
                "session.resumed" => {
                    if projection_event_schema(event) != Some("session.resumed/0.1") {
                        push_projection_issue(&mut issues, "session-resumed-event-invalid");
                        continue;
                    }
                    let updated_at_ms = event
                        .payload
                        .get("updated_at_ms")
                        .and_then(serde_json::Value::as_u64);
                    let environment_identity = event
                        .payload
                        .get("environment_identity")
                        .and_then(serde_json::Value::as_str);
                    match (session.as_mut(), updated_at_ms, environment_identity) {
                        (Some(session), Some(updated_at_ms), Some(environment_identity))
                            if updated_at_ms >= session.updated_at_ms
                                && validate_identity_text(
                                    environment_identity,
                                    "resumed environment identity",
                                )
                                .is_ok() =>
                        {
                            session.environment_identity = Some(environment_identity.into());
                            session.updated_at_ms = updated_at_ms;
                        }
                        _ => push_projection_issue(&mut issues, "session-resumed-event-invalid"),
                    }
                }
                "session.title-updated" => {
                    if projection_event_schema(event) != Some("session.title/0.1") {
                        push_projection_issue(&mut issues, "session-title-event-invalid");
                        continue;
                    }
                    let title = event
                        .payload
                        .get("title")
                        .and_then(serde_json::Value::as_str);
                    let updated_at_ms = event
                        .payload
                        .get("updated_at_ms")
                        .and_then(serde_json::Value::as_u64);
                    match (session.as_mut(), title, updated_at_ms) {
                        (Some(session), Some(title), Some(updated_at_ms))
                            if validate_text(title, 256, "session title").is_ok()
                                && updated_at_ms >= session.updated_at_ms =>
                        {
                            session.title = title.into();
                            session.updated_at_ms = updated_at_ms;
                        }
                        _ => push_projection_issue(&mut issues, "session-title-event-invalid"),
                    }
                }
                "session.archived" | "session.unarchived" => {
                    if projection_event_schema(event) != Some("session.status/0.1") {
                        push_projection_issue(&mut issues, "session-status-event-invalid");
                        continue;
                    }
                    let status = event
                        .payload
                        .get("status")
                        .and_then(serde_json::Value::as_str);
                    let updated_at_ms = event
                        .payload
                        .get("updated_at_ms")
                        .and_then(serde_json::Value::as_u64);
                    let expected_status = if event.event_kind == "session.archived" {
                        "archived"
                    } else {
                        "active"
                    };
                    match (session.as_mut(), status, updated_at_ms) {
                        (Some(session), Some(status), Some(updated_at_ms))
                            if status == expected_status
                                && updated_at_ms >= session.updated_at_ms =>
                        {
                            session.status = status.into();
                            session.updated_at_ms = updated_at_ms;
                        }
                        _ => push_projection_issue(&mut issues, "session-status-event-invalid"),
                    }
                }
                "turn.created" => {
                    if projection_event_schema(event) != Some("turn.created/0.1") {
                        push_projection_issue(&mut issues, "turn-created-event-invalid");
                        continue;
                    }
                    let parsed = event
                        .payload
                        .get("turn")
                        .cloned()
                        .and_then(|value| serde_json::from_value::<StoredTurn>(value).ok());
                    let Some(parsed) = parsed else {
                        push_projection_issue(&mut issues, "turn-created-event-invalid");
                        continue;
                    };
                    if parsed.session_id != session_id
                        || parsed.state != "started"
                        || parsed.updated_at_ms != parsed.created_at_ms
                        || validate_identifier(&parsed.turn_id, "turn ID").is_err()
                        || validate_content_hash(&parsed.input_hash, "turn input hash").is_err()
                        || turns.contains_key(&parsed.turn_id)
                    {
                        push_projection_issue(&mut issues, "turn-created-event-invalid");
                        continue;
                    }
                    turns.insert(parsed.turn_id.clone(), parsed);
                }
                "item.appended" => {
                    if projection_event_schema(event) != Some("item.appended/0.1") {
                        push_projection_issue(&mut issues, "item-appended-event-invalid");
                        continue;
                    }
                    let parsed = event
                        .payload
                        .get("item")
                        .cloned()
                        .and_then(|value| serde_json::from_value::<StoredItem>(value).ok());
                    let Some(parsed) = parsed else {
                        push_projection_issue(&mut issues, "item-appended-event-invalid");
                        continue;
                    };
                    let expected_sequence = items.len() as u64 + 1;
                    let payload_json = match validate_item_payload(&parsed.payload) {
                        Ok(payload_json) => payload_json,
                        Err(_) => {
                            push_projection_issue(&mut issues, "item-appended-event-invalid");
                            continue;
                        }
                    };
                    let turn_valid = parsed.turn_id.as_ref().is_none_or(|turn_id| {
                        turns
                            .get(turn_id)
                            .is_some_and(|turn| turn.session_id == parsed.session_id)
                    });
                    if parsed.session_id != session_id
                        || parsed.sequence != expected_sequence
                        || validate_identifier(&parsed.item_id, "item ID").is_err()
                        || ContentHash::for_bytes(payload_json.as_bytes()) != parsed.payload_hash
                        || !turn_valid
                    {
                        push_projection_issue(&mut issues, "item-appended-event-invalid");
                        continue;
                    }
                    if let Some(turn_id) = &parsed.turn_id {
                        if let Some(turn) = turns.get_mut(turn_id) {
                            if turn.state == "started" {
                                turn.state = "running".into();
                            }
                            turn.updated_at_ms = turn.updated_at_ms.max(parsed.created_at_ms);
                        }
                    }
                    items.push(parsed);
                }
                "session.imported" => {
                    if projection_event_schema(event) != Some("session.imported/0.1") {
                        push_projection_issue(&mut issues, "session-imported-event-invalid");
                        continue;
                    }
                    let source_session_id = event
                        .payload
                        .get("source_session_id")
                        .and_then(serde_json::Value::as_str);
                    let package_hash = event
                        .payload
                        .get("package_hash")
                        .cloned()
                        .and_then(|value| serde_json::from_value::<ContentHash>(value).ok());
                    let item_count = event
                        .payload
                        .get("item_count")
                        .and_then(serde_json::Value::as_u64);
                    let redacted_value_count = event
                        .payload
                        .get("redacted_value_count")
                        .and_then(serde_json::Value::as_u64);
                    let excluded_field_count = event
                        .payload
                        .get("excluded_field_count")
                        .and_then(serde_json::Value::as_u64);
                    let linked_source_session = event
                        .payload
                        .get("linked_source_session")
                        .and_then(serde_json::Value::as_bool);
                    let imported_at_ms = event
                        .payload
                        .get("imported_at_ms")
                        .and_then(serde_json::Value::as_u64);
                    let valid = match (
                        session.as_ref(),
                        source_session_id,
                        package_hash.as_ref(),
                        item_count,
                        redacted_value_count,
                        excluded_field_count,
                        linked_source_session,
                        imported_at_ms,
                    ) {
                        (
                            Some(session),
                            Some(source_session_id),
                            Some(package_hash),
                            Some(item_count),
                            Some(_),
                            Some(_),
                            Some(linked_source_session),
                            Some(imported_at_ms),
                        ) => {
                            let linked_source_matches = session
                                .parent_session_id
                                .as_deref()
                                .is_some_and(|parent| parent == source_session_id);
                            validate_identifier(source_session_id, "portable source session ID")
                                .is_ok()
                                && validate_content_hash(
                                    package_hash,
                                    "portable session package hash",
                                )
                                .is_ok()
                                && item_count == items.len() as u64
                                && linked_source_session == linked_source_matches
                                && imported_at_ms == event.timestamp_ms
                                && imported_at_ms >= session.created_at_ms
                                && event.project_id == session.project_id
                                && event.operation_id == event.correlation_id
                                && event.generation == 0
                        }
                        _ => false,
                    };
                    if !valid {
                        push_projection_issue(&mut issues, "session-imported-event-invalid");
                    }
                }
                "session.compaction-checkpointed" => {
                    let checkpoint_id = event
                        .payload
                        .get("checkpoint_id")
                        .and_then(serde_json::Value::as_str);
                    let review_id = event
                        .payload
                        .get("review_id")
                        .and_then(serde_json::Value::as_str);
                    let object_reference = event
                        .payload
                        .get("object_reference")
                        .and_then(serde_json::Value::as_str);
                    let through_sequence = event
                        .payload
                        .get("through_sequence")
                        .and_then(serde_json::Value::as_u64);
                    let source_context_hash = event
                        .payload
                        .get("source_context_hash")
                        .and_then(serde_json::Value::as_str);
                    let supersedes_valid = match event.payload.get("supersedes") {
                        None => true,
                        Some(Value::Object(supersedes)) => {
                            let source_checkpoint_id =
                                supersedes.get("checkpoint_id").and_then(Value::as_str);
                            let source_review_id =
                                supersedes.get("review_id").and_then(Value::as_str);
                            let source_object_reference =
                                supersedes.get("object_reference").and_then(Value::as_str);
                            match (
                                source_checkpoint_id,
                                source_review_id,
                                source_object_reference,
                            ) {
                                (
                                    Some(source_checkpoint_id),
                                    Some(source_review_id),
                                    Some(source_object_reference),
                                ) => {
                                    validate_identifier(
                                        source_checkpoint_id,
                                        "source compaction checkpoint ID",
                                    )
                                    .is_ok()
                                        && validate_compaction_review_id(source_review_id).is_ok()
                                        && validate_content_reference(source_object_reference, None)
                                            .is_ok()
                                        && compaction_checkpoints.get(source_review_id).is_some_and(
                                            |(known_checkpoint, known_object)| {
                                                known_checkpoint == source_checkpoint_id
                                                    && known_object == source_object_reference
                                            },
                                        )
                                }
                                _ => false,
                            }
                        }
                        Some(_) => false,
                    };
                    let valid = match (
                        session.as_ref(),
                        checkpoint_id,
                        review_id,
                        object_reference,
                        through_sequence,
                        source_context_hash,
                    ) {
                        (
                            Some(session),
                            Some(checkpoint_id),
                            Some(review_id),
                            Some(object_reference),
                            Some(through_sequence),
                            Some(source_context_hash),
                        ) => {
                            projection_event_schema(event)
                                == Some("session.compaction-checkpointed/0.1")
                                && event
                                    .payload
                                    .get("session_id")
                                    .and_then(serde_json::Value::as_str)
                                    == Some(session_id)
                                && event
                                    .payload
                                    .get("state")
                                    .and_then(serde_json::Value::as_str)
                                    == Some("review-persisted")
                                && event
                                    .payload
                                    .get("original_event_history_authoritative")
                                    .and_then(serde_json::Value::as_bool)
                                    == Some(true)
                                && validate_identifier(checkpoint_id, "compaction checkpoint ID")
                                    .is_ok()
                                && validate_compaction_review_id(review_id).is_ok()
                                && validate_content_reference(object_reference, None).is_ok()
                                && object_reference
                                    .starts_with("session-compaction-checkpoint:sha256:")
                                && through_sequence > 0
                                && validate_lower_sha256(
                                    source_context_hash,
                                    "compaction source context hash",
                                )
                                .is_ok()
                                && event.event_id
                                    == derived_event_id(
                                        "session-compaction-checkpoint",
                                        review_id.as_bytes(),
                                    )
                                && event.correlation_id == review_id
                                && event.operation_id == checkpoint_id
                                && event.generation == through_sequence
                                && event.project_id == session.project_id
                                && event.timestamp_ms >= session.created_at_ms
                                && supersedes_valid
                        }
                        _ => false,
                    };
                    if valid {
                        if let (Some(review_id), Some(checkpoint_id), Some(object_reference)) =
                            (review_id, checkpoint_id, object_reference)
                        {
                            compaction_checkpoints.insert(
                                review_id.to_owned(),
                                (checkpoint_id.to_owned(), object_reference.to_owned()),
                            );
                        } else {
                            push_projection_issue(
                                &mut issues,
                                "session-compaction-checkpoint-event-invalid",
                            );
                        }
                    } else {
                        push_projection_issue(
                            &mut issues,
                            "session-compaction-checkpoint-event-invalid",
                        );
                    }
                }
                "turn.trace.recorded" => {
                    let parsed = parse_turn_trace_event(event);
                    let valid = parsed.as_ref().is_ok_and(|trace| {
                        session.as_ref().is_some_and(|session| {
                            trace.trace.binding.project_id == session.project_id
                                && trace.trace.binding.environment_identity
                                    == session.environment_identity
                                && turn_trace_matches_session_mode(&trace.trace, session.mode)
                        }) && turns.get(&trace.trace.binding.turn_id).is_some_and(|turn| {
                            turn.session_id == session_id
                                && matches!(turn.state.as_str(), "started" | "running")
                        }) && !pending_turn_traces.contains_key(&trace.trace.binding.turn_id)
                    });
                    if valid {
                        if let Ok(trace) = parsed {
                            pending_turn_traces.insert(trace.trace.binding.turn_id.clone(), trace);
                        }
                    } else {
                        push_projection_issue(&mut issues, "turn-trace-event-invalid");
                    }
                }
                "turn.completed" | "turn.failed" | "turn.interrupted" | "turn.cancelled" => {
                    if projection_event_schema(event) != Some("turn.terminal/0.1") {
                        push_projection_issue(&mut issues, "turn-terminal-event-invalid");
                        continue;
                    }
                    let turn_id = event
                        .payload
                        .get("turn_id")
                        .and_then(serde_json::Value::as_str);
                    let state = event
                        .payload
                        .get("state")
                        .and_then(serde_json::Value::as_str);
                    let updated_at_ms = event
                        .payload
                        .get("updated_at_ms")
                        .and_then(serde_json::Value::as_u64);
                    let expected_state = event.event_kind.strip_prefix("turn.").unwrap_or("");
                    match (turn_id, state, updated_at_ms) {
                        (Some(turn_id), Some(state), Some(updated_at_ms))
                            if state == expected_state
                                && session.as_ref().is_some_and(|session| {
                                    validate_turn_terminal_event(
                                        event,
                                        session.project_id.as_deref(),
                                        turn_id,
                                        state,
                                        updated_at_ms,
                                    )
                                })
                                && turns.get(turn_id).is_some_and(|turn| {
                                    matches!(turn.state.as_str(), "started" | "running")
                                        && updated_at_ms >= turn.updated_at_ms
                                })
                                && pending_turn_traces.get(turn_id).is_none_or(|trace| {
                                    trace.state == state
                                        && trace.recorded_at_ms == updated_at_ms
                                        && trace.event_sequence.checked_add(1)
                                            == Some(event.sequence)
                                        && trace.trace.binding.project_id == event.project_id
                                }) =>
                        {
                            pending_turn_traces.remove(turn_id);
                            if let Some(turn) = turns.get_mut(turn_id) {
                                turn.state = state.into();
                                turn.updated_at_ms = updated_at_ms;
                            }
                        }
                        _ => push_projection_issue(&mut issues, "turn-terminal-event-invalid"),
                    }
                }
                _ => {}
            }
        }

        if !pending_turn_traces.is_empty() {
            push_projection_issue(&mut issues, "turn-trace-terminal-event-missing");
        }

        let mut candidate_turns = turns.into_values().collect::<Vec<_>>();
        candidate_turns.sort_by(|left, right| left.turn_id.cmp(&right.turn_id));
        if source_registered && session.is_none() {
            push_projection_issue(&mut issues, "projection-event-source-incomplete");
        }
        let source_complete = source_registered
            && session.is_some()
            && !issues.iter().any(|issue| {
                issue.starts_with("session-")
                    || issue.starts_with("turn-")
                    || issue.starts_with("item-")
                    || issue == "projection-event-limit-exceeded"
            });
        let mut matches_current_projection = false;
        if source_complete {
            let current_session = self.load_session(session_id).ok();
            let current_turns = self.load_session_turns(session_id).ok();
            let current_items = self.load_all_session_items(session_id).ok();
            matches_current_projection = session == current_session
                && current_turns.as_ref() == Some(&candidate_turns)
                && current_items.as_ref() == Some(&items);
            if !matches_current_projection {
                push_projection_issue(&mut issues, "event-projection-mismatch");
            }
        }
        Ok(SessionProjectionCandidate {
            schema_version: "session-projection-candidate/0.1".into(),
            session_id: session_id.into(),
            source_event_count: event_count,
            source_hash: Some(source_hash),
            source_complete,
            matches_current_projection,
            session,
            turns: candidate_turns,
            items,
            issues,
        })
    }

    fn load_session_turns(&self, session_id: &str) -> Result<Vec<StoredTurn>, WorkbenchStoreError> {
        let mut statement = self
            .connection
            .prepare(
                "SELECT turn_id, session_id, idempotency_key, input_sha256, input_bytes,
                        state, created_at_ms, updated_at_ms
                 FROM turns WHERE session_id = ?1 ORDER BY turn_id",
            )
            .map_err(|_| error("cannot prepare session turn projection"))?;
        let rows = statement
            .query_map([session_id], |row| {
                Ok(StoredTurn {
                    turn_id: row.get(0)?,
                    session_id: row.get(1)?,
                    idempotency_key: row.get(2)?,
                    input_hash: ContentHash {
                        sha256: row.get(3)?,
                        bytes: to_u64_sql(row.get(4)?, "turn input byte count")?,
                    },
                    state: row.get(5)?,
                    created_at_ms: to_u64_sql(row.get(6)?, "turn creation time")?,
                    updated_at_ms: to_u64_sql(row.get(7)?, "turn update time")?,
                })
            })
            .map_err(|_| error("cannot read session turn projection"))?;
        rows.collect::<Result<Vec<_>, _>>()
            .map_err(|_| error("session turn projection row is invalid"))
    }

    fn load_all_session_items(
        &self,
        session_id: &str,
    ) -> Result<Vec<StoredItem>, WorkbenchStoreError> {
        let latest = self.latest_session_item_sequence(session_id)?;
        let mut items = Vec::new();
        let mut after_sequence = 0_u64;
        while items.len() < latest as usize {
            let remaining =
                usize::try_from((latest - items.len() as u64).min(MAX_EVENT_PAGE as u64))
                    .map_err(|_| error("item projection page is invalid"))?;
            let page = self.read_session_items(session_id, after_sequence, remaining)?;
            if page.is_empty() {
                return Err(error("item projection ended before its boundary"));
            }
            after_sequence = page.last().map_or(after_sequence, |item| item.sequence);
            items.extend(page);
        }
        Ok(items)
    }

    pub fn rebuild_session_projection(
        &mut self,
        session_id: &str,
    ) -> Result<SessionProjectionConsistency, WorkbenchStoreError> {
        let candidate = self.rebuild_session_projection_candidate(session_id)?;
        self.apply_session_projection_candidate(&candidate)?;
        let report = self.verify_session_projection(session_id)?;
        if !report.consistent || !report.event_projection_matches {
            return Err(error("rebuilt session projection did not verify"));
        }
        self.quarantined_sessions.remove(session_id);
        Ok(report)
    }

    pub fn verify_or_rebuild_session_projection(
        &mut self,
        session_id: &str,
    ) -> Result<(SessionProjectionConsistency, bool), WorkbenchStoreError> {
        let report = match self.verify_session_projection(session_id) {
            Ok(report) => report,
            Err(verification_error) => {
                let candidate = self.rebuild_session_projection_candidate(session_id)?;
                if !projection_candidate_is_rebuildable(&candidate) {
                    self.quarantined_sessions.insert(session_id.into());
                    return Err(verification_error);
                }
                return match self.rebuild_session_projection(session_id) {
                    Ok(report) => Ok((report, true)),
                    Err(error) => {
                        self.quarantined_sessions.insert(session_id.into());
                        Err(error)
                    }
                };
            }
        };
        if report.consistent {
            self.quarantined_sessions.remove(session_id);
            return Ok((report, false));
        }
        if !report.rebuild_source_complete
            || report.event_projection_matches
            || !report
                .issues
                .iter()
                .all(|issue| automatic_projection_rebuild_issue(issue))
        {
            self.quarantined_sessions.insert(session_id.into());
            return Ok((report, false));
        }
        match self.rebuild_session_projection(session_id) {
            Ok(rebuilt) => Ok((rebuilt, true)),
            Err(error) => {
                self.quarantined_sessions.insert(session_id.into());
                Err(error)
            }
        }
    }

    fn validate_projection_candidate_external_authority(
        &self,
        candidate: &SessionProjectionCandidate,
    ) -> Result<(), WorkbenchStoreError> {
        let session = candidate
            .session
            .as_ref()
            .ok_or_else(|| error("session projection candidate has no session"))?;
        if session.mode == StoredSessionMode::Work && session.project_id.is_none() {
            return Err(error("rebuilt Work session has no project"));
        }
        if let Some(project_id) = &session.project_id {
            let exists = self
                .connection
                .query_row(
                    "SELECT EXISTS(SELECT 1 FROM projects WHERE project_id = ?1)",
                    [project_id],
                    |row| row.get::<_, bool>(0),
                )
                .map_err(|_| error("cannot validate rebuilt session project"))?;
            if !exists {
                return Err(error("rebuilt session project does not exist"));
            }
        }
        match (&session.parent_session_id, session.lineage_kind) {
            (None, StoredSessionLineage::New) => {}
            (
                Some(parent_session_id),
                StoredSessionLineage::Resume | StoredSessionLineage::Fork,
            ) => {
                let parent = self
                    .connection
                    .query_row(
                        "SELECT project_id, mode FROM sessions WHERE session_id = ?1",
                        [parent_session_id],
                        |row| Ok((row.get::<_, Option<String>>(0)?, row.get::<_, String>(1)?)),
                    )
                    .optional()
                    .map_err(|_| error("cannot validate rebuilt session lineage"))?;
                if !matches!(
                    parent,
                    Some((project_id, mode))
                        if project_id == session.project_id && mode == session.mode.as_str()
                ) {
                    return Err(error("rebuilt session lineage is not authoritative"));
                }
            }
            _ => return Err(error("rebuilt session lineage is invalid")),
        }

        let reference_count = query_count(
            &self.connection,
            "SELECT COUNT(*) FROM durable_blob_references WHERE session_id = ?1",
            &candidate.session_id,
            "cannot count rebuilt session blob references",
        )?;
        if reference_count > MAX_PROJECTION_VERIFY_ROWS {
            return Err(error("rebuilt session blob reference limit exceeded"));
        }
        let mut statement = self
            .connection
            .prepare(
                "SELECT reference_id FROM durable_blob_references
                 WHERE session_id = ?1 ORDER BY created_at_ms, reference_id",
            )
            .map_err(|_| error("cannot prepare rebuilt session blob validation"))?;
        let rows = statement
            .query_map([&candidate.session_id], |row| row.get::<_, String>(0))
            .map_err(|_| error("cannot read rebuilt session blob validation"))?;
        for row in rows {
            let reference_id =
                row.map_err(|_| error("rebuilt session blob reference row is invalid"))?;
            let reference = load_durable_blob_reference(&self.connection, &reference_id)?;
            if reference.session_id.as_deref() != Some(&candidate.session_id)
                || reference.project_id != session.project_id
            {
                return Err(error("rebuilt session blob ownership is invalid"));
            }
            let owner_valid = match reference.owner_kind.as_str() {
                "session" => reference.owner_id == candidate.session_id,
                "turn" => candidate
                    .turns
                    .iter()
                    .any(|turn| turn.turn_id == reference.owner_id),
                "item" => candidate
                    .items
                    .iter()
                    .any(|item| item.item_id == reference.owner_id),
                _ => true,
            };
            if !owner_valid {
                return Err(error("rebuilt session blob projection owner is invalid"));
            }
            self.blob_files
                .read(&reference.content_hash.sha256, reference.content_hash.bytes)
                .map_err(blob_file_error)?;
        }
        Ok(())
    }

    fn apply_session_projection_candidate(
        &mut self,
        candidate: &SessionProjectionCandidate,
    ) -> Result<(), WorkbenchStoreError> {
        validate_identifier(&candidate.session_id, "session ID")?;
        let session = candidate
            .session
            .as_ref()
            .ok_or_else(|| error("session projection candidate has no session"))?;
        validate_rebuilt_session(session)?;
        if !projection_candidate_is_rebuildable(candidate) {
            return Err(error("session projection candidate is not rebuildable"));
        }
        self.validate_projection_candidate_external_authority(candidate)?;
        let mut expected_item_sequence = 1_u64;
        for turn in &candidate.turns {
            validate_identifier(&turn.turn_id, "turn ID")?;
            validate_content_hash(&turn.input_hash, "turn input hash")?;
            if turn.session_id != candidate.session_id
                || !matches!(
                    turn.state.as_str(),
                    "started" | "running" | "completed" | "failed" | "interrupted" | "cancelled"
                )
                || turn.updated_at_ms < turn.created_at_ms
            {
                return Err(error("session projection candidate turn is invalid"));
            }
        }
        for item in &candidate.items {
            validate_identifier(&item.item_id, "item ID")?;
            if item.session_id != candidate.session_id
                || item.sequence != expected_item_sequence
                || item.turn_id.as_ref().is_some_and(|turn_id| {
                    !candidate
                        .turns
                        .iter()
                        .any(|turn| turn.turn_id == *turn_id && turn.session_id == item.session_id)
                })
            {
                return Err(error("session projection candidate item is invalid"));
            }
            let payload_json = validate_item_payload(&item.payload)?;
            if ContentHash::for_bytes(payload_json.as_bytes()) != item.payload_hash {
                return Err(error("session projection candidate item hash is invalid"));
            }
            expected_item_sequence = expected_item_sequence.saturating_add(1);
        }

        let transaction = self.begin_database_write("cannot start session projection rebuild")?;
        let source_registered = transaction
            .query_row(
                "SELECT EXISTS(
                    SELECT 1 FROM session_projection_sources
                    WHERE session_id = ?1 AND source_version = 1
                 )",
                [&candidate.session_id],
                |row| row.get::<_, bool>(0),
            )
            .map_err(|_| error("cannot revalidate session projection source"))?;
        if !source_registered {
            return Err(error("session projection source registration is missing"));
        }
        let current_events = load_projection_source_events(&transaction, &candidate.session_id)?;
        let current_hash = projection_event_stream_hash(&current_events);
        if current_events.len() as u64 != candidate.source_event_count
            || Some(&current_hash) != candidate.source_hash.as_ref()
        {
            return Err(error("session projection source changed after review"));
        }
        let rebuild_timestamp = current_events
            .last()
            .map_or(session.updated_at_ms, |event| {
                event.timestamp_ms.max(session.updated_at_ms)
            });

        transaction
            .execute(
                "DELETE FROM items WHERE session_id = ?1",
                [&candidate.session_id],
            )
            .map_err(|_| error("cannot clear item projection for rebuild"))?;
        transaction
            .execute(
                "DELETE FROM turns WHERE session_id = ?1",
                [&candidate.session_id],
            )
            .map_err(|_| error("cannot clear turn projection for rebuild"))?;
        projection_rebuild_crash_point("session-after-projection-clear");
        transaction
            .execute(
                "INSERT INTO sessions (
                    session_id, project_id, mode, title, parent_session_id, lineage_kind,
                    status, environment_identity, created_at_ms, updated_at_ms
                 ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)
                 ON CONFLICT(session_id) DO UPDATE SET
                    project_id = excluded.project_id,
                    mode = excluded.mode,
                    title = excluded.title,
                    parent_session_id = excluded.parent_session_id,
                    lineage_kind = excluded.lineage_kind,
                    status = excluded.status,
                    environment_identity = excluded.environment_identity,
                    created_at_ms = excluded.created_at_ms,
                    updated_at_ms = excluded.updated_at_ms",
                params![
                    session.session_id,
                    session.project_id,
                    session.mode.as_str(),
                    session.title,
                    session.parent_session_id,
                    session.lineage_kind.as_str(),
                    session.status,
                    session.environment_identity,
                    to_i64(session.created_at_ms, "session creation time")?,
                    to_i64(session.updated_at_ms, "session update time")?,
                ],
            )
            .map_err(|_| error("cannot install rebuilt session projection"))?;
        for turn in &candidate.turns {
            transaction
                .execute(
                    "INSERT INTO turns (
                        turn_id, session_id, idempotency_key, input_sha256, input_bytes,
                        state, created_at_ms, updated_at_ms
                     ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
                    params![
                        turn.turn_id,
                        turn.session_id,
                        turn.idempotency_key,
                        turn.input_hash.sha256,
                        to_i64(turn.input_hash.bytes, "turn input byte count")?,
                        turn.state,
                        to_i64(turn.created_at_ms, "turn creation time")?,
                        to_i64(turn.updated_at_ms, "turn update time")?,
                    ],
                )
                .map_err(|_| error("cannot install rebuilt turn projection"))?;
        }
        for item in &candidate.items {
            let payload_json = validate_item_payload(&item.payload)?;
            transaction
                .execute(
                    "INSERT INTO items (
                        session_id, item_sequence, item_id, turn_id, item_kind, role,
                        state, payload_json, payload_sha256, payload_bytes, created_at_ms
                     ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)",
                    params![
                        item.session_id,
                        to_i64(item.sequence, "item sequence")?,
                        item.item_id,
                        item.turn_id,
                        item.item_kind,
                        item.role,
                        item.state,
                        payload_json,
                        item.payload_hash.sha256,
                        to_i64(item.payload_hash.bytes, "item payload byte count")?,
                        to_i64(item.created_at_ms, "item creation time")?,
                    ],
                )
                .map_err(|_| error("cannot install rebuilt item projection"))?;
        }
        let source_hash = candidate
            .source_hash
            .as_ref()
            .ok_or_else(|| error("session projection source hash is missing"))?;
        let event_id = derived_event_id(
            "projection-rebuilt",
            format!("{}\0{}", candidate.session_id, source_hash.sha256).as_bytes(),
        );
        append_event_tx(
            &transaction,
            EventInput {
                session_id: &candidate.session_id,
                event_id: &event_id,
                timestamp_ms: rebuild_timestamp,
                correlation_id: &candidate.session_id,
                event_kind: "session.projection-rebuilt",
                project_id: session.project_id.as_deref(),
                operation_id: &candidate.session_id,
                generation: 0,
                payload: json!({
                    "schema_version": "session.projection-rebuilt/0.1",
                    "source_hash": source_hash,
                    "turn_count": candidate.turns.len(),
                    "item_count": candidate.items.len()
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit session projection rebuild"))
    }

    pub fn verify_session_projection(
        &self,
        session_id: &str,
    ) -> Result<SessionProjectionConsistency, WorkbenchStoreError> {
        validate_identifier(session_id, "session ID")?;
        let session = self.load_session(session_id)?;
        let mut issues = Vec::new();

        if let Some(project_id) = &session.project_id {
            let project_exists = self
                .connection
                .query_row(
                    "SELECT EXISTS(SELECT 1 FROM projects WHERE project_id = ?1)",
                    [project_id],
                    |row| row.get::<_, bool>(0),
                )
                .map_err(|_| error("cannot verify session project binding"))?;
            if !project_exists {
                push_projection_issue(&mut issues, "session-project-missing");
            }
        } else if session.mode == StoredSessionMode::Work {
            push_projection_issue(&mut issues, "work-session-project-missing");
        }
        if let Some(parent_session_id) = &session.parent_session_id {
            match self.load_session(parent_session_id) {
                Ok(parent)
                    if parent.project_id == session.project_id && parent.mode == session.mode => {}
                Ok(_) => push_projection_issue(&mut issues, "session-lineage-binding-mismatch"),
                Err(_) => push_projection_issue(&mut issues, "session-lineage-parent-missing"),
            }
        } else if session.lineage_kind != StoredSessionLineage::New {
            push_projection_issue(&mut issues, "session-lineage-parent-missing");
        }

        let turn_count = query_count(
            &self.connection,
            "SELECT COUNT(*) FROM turns WHERE session_id = ?1",
            session_id,
            "cannot count session turns",
        )?;
        if turn_count > MAX_PROJECTION_VERIFY_ROWS {
            push_projection_issue(&mut issues, "turn-verification-limit-exceeded");
        } else {
            let mut statement = self
                .connection
                .prepare(
                    "SELECT turn_id, input_sha256, input_bytes
                     FROM turns WHERE session_id = ?1 ORDER BY created_at_ms, turn_id",
                )
                .map_err(|_| error("cannot prepare turn projection verification"))?;
            let rows = statement
                .query_map([session_id], |row| {
                    Ok((
                        row.get::<_, String>(0)?,
                        row.get::<_, String>(1)?,
                        row.get::<_, i64>(2)?,
                    ))
                })
                .map_err(|_| error("cannot read turn projection verification"))?;
            for row in rows {
                let (turn_id, sha256, bytes) =
                    row.map_err(|_| error("turn projection row is invalid"))?;
                let hash = ContentHash {
                    sha256,
                    bytes: match to_u64(bytes, "turn input byte count") {
                        Ok(bytes) => bytes,
                        Err(_) => {
                            push_projection_issue(&mut issues, "turn-input-hash-invalid");
                            continue;
                        }
                    },
                };
                if validate_identifier(&turn_id, "turn ID").is_err()
                    || validate_content_hash(&hash, "turn input hash").is_err()
                {
                    push_projection_issue(&mut issues, "turn-input-hash-invalid");
                }
            }
        }

        let (item_count, item_min, item_max) = query_sequence_summary(
            &self.connection,
            "SELECT COUNT(*), COALESCE(MIN(item_sequence), 0),
                    COALESCE(MAX(item_sequence), 0)
             FROM items WHERE session_id = ?1",
            session_id,
            "cannot summarize session items",
        )?;
        if item_count > 0 && (item_min != 1 || item_max != item_count) {
            push_projection_issue(&mut issues, "item-sequence-gap");
        }
        let invalid_item_turns = query_count(
            &self.connection,
            "SELECT COUNT(*) FROM items AS item
             LEFT JOIN turns AS turn_record ON turn_record.turn_id = item.turn_id
             WHERE item.session_id = ?1 AND item.turn_id IS NOT NULL
               AND (turn_record.turn_id IS NULL OR turn_record.session_id != item.session_id)",
            session_id,
            "cannot verify item turn bindings",
        )?;
        if invalid_item_turns > 0 {
            push_projection_issue(&mut issues, "item-turn-binding-invalid");
        }
        if item_count > MAX_PROJECTION_VERIFY_ROWS {
            push_projection_issue(&mut issues, "item-verification-limit-exceeded");
        } else if item_count > 0 {
            let mut after_sequence = 0_u64;
            let mut checked = 0_u64;
            while checked < item_count {
                let remaining = usize::try_from((item_count - checked).min(MAX_EVENT_PAGE as u64))
                    .map_err(|_| error("item verification page is invalid"))?;
                match self.read_session_items(session_id, after_sequence, remaining) {
                    Ok(page) if !page.is_empty() => {
                        checked += page.len() as u64;
                        after_sequence = page.last().map_or(after_sequence, |item| item.sequence);
                    }
                    _ => {
                        push_projection_issue(&mut issues, "item-payload-or-sequence-invalid");
                        break;
                    }
                }
            }
        }

        let (event_count, event_min, event_max) = query_sequence_summary(
            &self.connection,
            "SELECT COUNT(*), COALESCE(MIN(sequence), 0), COALESCE(MAX(sequence), 0)
             FROM events WHERE session_id = ?1",
            session_id,
            "cannot summarize session events",
        )?;
        if event_count > 0 && (event_min != 1 || event_max != event_count) {
            push_projection_issue(&mut issues, "event-sequence-gap");
        }
        let next_event_sequence = self
            .connection
            .query_row(
                "SELECT next_sequence FROM session_sequences WHERE session_id = ?1",
                [session_id],
                |row| row.get::<_, i64>(0),
            )
            .optional()
            .map_err(|_| error("cannot verify session event cursor"))?
            .map(|value| to_u64(value, "session event cursor"))
            .transpose()?;
        if (event_count == 0 && next_event_sequence.is_some())
            || (event_count > 0 && next_event_sequence != event_max.checked_add(1))
        {
            push_projection_issue(&mut issues, "event-sequence-cursor-mismatch");
        }
        if event_count > MAX_PROJECTION_VERIFY_ROWS {
            push_projection_issue(&mut issues, "event-verification-limit-exceeded");
        } else if event_count > 0 {
            let mut after_sequence = 0_u64;
            let mut checked = 0_u64;
            while checked < event_count {
                let remaining = usize::try_from((event_count - checked).min(MAX_EVENT_PAGE as u64))
                    .map_err(|_| error("event verification page is invalid"))?;
                match self.read_session_events(session_id, after_sequence, remaining) {
                    Ok(page) if !page.is_empty() => {
                        checked += page.len() as u64;
                        after_sequence = page.last().map_or(after_sequence, |event| event.sequence);
                    }
                    _ => {
                        push_projection_issue(&mut issues, "event-payload-or-sequence-invalid");
                        break;
                    }
                }
            }
        }

        let blob_reference_count = query_count(
            &self.connection,
            "SELECT COUNT(*) FROM durable_blob_references WHERE session_id = ?1",
            session_id,
            "cannot count session durable blob references",
        )?;
        if blob_reference_count > MAX_PROJECTION_VERIFY_ROWS {
            push_projection_issue(&mut issues, "blob-reference-verification-limit-exceeded");
        } else if blob_reference_count > 0 {
            let mut statement = self
                .connection
                .prepare(
                    "SELECT reference_id FROM durable_blob_references
                     WHERE session_id = ?1 ORDER BY created_at_ms, reference_id",
                )
                .map_err(|_| error("cannot prepare session durable blob verification"))?;
            let rows = statement
                .query_map([session_id], |row| row.get::<_, String>(0))
                .map_err(|_| error("cannot read session durable blob references"))?;
            for row in rows {
                let reference_id =
                    row.map_err(|_| error("session durable blob reference row is invalid"))?;
                let reference = match load_durable_blob_reference(&self.connection, &reference_id) {
                    Ok(reference) => reference,
                    Err(_) => {
                        push_projection_issue(&mut issues, "blob-reference-target-invalid");
                        continue;
                    }
                };
                if reference.session_id.as_deref() != Some(session_id)
                    || reference.project_id != session.project_id
                {
                    push_projection_issue(&mut issues, "blob-reference-owner-mismatch");
                }
                let owner_exists = match reference.owner_kind.as_str() {
                    "session" => reference.owner_id == session_id,
                    "turn" => self
                        .connection
                        .query_row(
                            "SELECT EXISTS(
                                SELECT 1 FROM turns
                                WHERE turn_id = ?1 AND session_id = ?2
                             )",
                            params![reference.owner_id, session_id],
                            |row| row.get::<_, bool>(0),
                        )
                        .unwrap_or(false),
                    "item" => self
                        .connection
                        .query_row(
                            "SELECT EXISTS(
                                SELECT 1 FROM items
                                WHERE item_id = ?1 AND session_id = ?2
                             )",
                            params![reference.owner_id, session_id],
                            |row| row.get::<_, bool>(0),
                        )
                        .unwrap_or(false),
                    _ => true,
                };
                if !owner_exists {
                    push_projection_issue(&mut issues, "blob-reference-projection-owner-missing");
                }
                if self
                    .blob_files
                    .read(&reference.content_hash.sha256, reference.content_hash.bytes)
                    .is_err()
                {
                    push_projection_issue(&mut issues, "blob-reference-content-invalid");
                }
            }
        }

        let mut rebuild_source_complete = false;
        let mut event_projection_matches = false;
        if !issues.iter().any(|issue| {
            issue.starts_with("event-") || issue == "event-verification-limit-exceeded"
        }) {
            match self.rebuild_session_projection_candidate(session_id) {
                Ok(candidate) => {
                    rebuild_source_complete = candidate.source_complete;
                    event_projection_matches = candidate.matches_current_projection;
                    for issue in candidate.issues {
                        push_projection_issue(&mut issues, &issue);
                    }
                }
                Err(_) => push_projection_issue(&mut issues, "projection-event-replay-invalid"),
            }
        }

        Ok(SessionProjectionConsistency {
            schema_version: "session-projection-consistency/0.1".into(),
            session_id: session_id.into(),
            consistent: issues.is_empty(),
            read_only_recovery_required: !issues.is_empty(),
            checked_turns: turn_count.min(MAX_PROJECTION_VERIFY_ROWS),
            checked_items: item_count.min(MAX_PROJECTION_VERIFY_ROWS),
            checked_events: event_count.min(MAX_PROJECTION_VERIFY_ROWS),
            checked_blob_references: blob_reference_count.min(MAX_PROJECTION_VERIFY_ROWS),
            latest_item_sequence: item_max,
            latest_event_sequence: event_max,
            issues,
            rebuild_source_complete,
            event_projection_matches,
            blob_store_available: true,
        })
    }

    pub fn issue_git_workflow_decision(
        &mut self,
        requirement: &GitWorkflowAuthorizationRequirement,
        kind: GitWorkflowDecisionKind,
        authority_id: &str,
        decision_id: &str,
        timing: GitWorkflowDecisionTiming,
    ) -> Result<GitWorkflowDecisionReference, WorkbenchStoreError> {
        let GitWorkflowDecisionTiming {
            issued_at_ms,
            expires_at_ms,
            observed_at_ms,
        } = timing;
        validate_identifier(authority_id, "approval authority ID")?;
        validate_identifier(decision_id, "approval decision ID")?;
        validate_requirement_summary(requirement)?;
        self.ensure_session_writable(&requirement.session_id)?;
        if issued_at_ms > observed_at_ms
            || observed_at_ms > expires_at_ms
            || expires_at_ms.saturating_sub(issued_at_ms) > MAX_AUTHORIZATION_LIFETIME_MS
        {
            return Err(error("approval decision lifetime is invalid"));
        }
        let reference = GitWorkflowDecisionReference {
            authority_id: authority_id.into(),
            decision_id: decision_id.into(),
            scope: "allow-once".into(),
            scope_hash: requirement.requirement_hash.clone(),
            issued_at_ms,
            expires_at_ms,
        };
        let transaction =
            self.begin_database_write("cannot start approval decision transaction")?;
        transaction
            .execute(
                "INSERT INTO approval_decisions (
                    authority_id, decision_id, decision_kind,
                    requirement_sha256, requirement_bytes,
                    project_id, session_id, operation_id, action, risk_class,
                    scope, issued_at_ms, expires_at_ms, status, created_at_ms
                 ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10,
                           'allow-once', ?11, ?12, 'issued', ?13)",
                params![
                    authority_id,
                    decision_id,
                    kind.as_str(),
                    requirement.requirement_hash.sha256,
                    to_i64(requirement.requirement_hash.bytes, "requirement byte count")?,
                    requirement.project_id,
                    requirement.session_id,
                    requirement.operation_id,
                    action_name(requirement),
                    requirement.risk.class,
                    to_i64(issued_at_ms, "decision issue time")?,
                    to_i64(expires_at_ms, "decision expiry time")?,
                    to_i64(observed_at_ms, "decision observation time")?,
                ],
            )
            .map_err(|_| error("approval decision already exists or is invalid"))?;
        let event_kind = match kind {
            GitWorkflowDecisionKind::Permission => "approval.permission-issued",
            GitWorkflowDecisionKind::ExplicitApproval => "approval.explicit-issued",
        };
        let event_id = derived_event_id(
            "approval-decision",
            format!("{authority_id}\0{decision_id}").as_bytes(),
        );
        append_event_tx(
            &transaction,
            EventInput {
                session_id: &requirement.session_id,
                event_id: &event_id,
                timestamp_ms: observed_at_ms,
                correlation_id: &requirement.operation_id,
                event_kind,
                project_id: Some(&requirement.project_id),
                operation_id: &requirement.operation_id,
                generation: requirement.generation,
                payload: json!({
                    "schema_version": "approval.decision/0.1",
                    "decision_kind": kind.as_str(),
                    "authority_id": authority_id,
                    "decision_id": decision_id,
                    "scope": "allow-once",
                    "scope_hash": requirement.requirement_hash,
                    "risk_class": requirement.risk.class,
                    "expires_at_ms": expires_at_ms,
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit approval decision"))?;
        Ok(reference)
    }

    pub fn git_workflow_authorization_evidence(
        &self,
        requirement: &GitWorkflowAuthorizationRequirement,
        authorization_id: &str,
        permission_authority_id: &str,
        permission_decision_id: &str,
        approval: Option<(&str, &str)>,
    ) -> Result<GitWorkflowAuthorizationEvidence, WorkbenchStoreError> {
        validate_identifier(authorization_id, "authorization ID")?;
        self.ensure_session_writable(&requirement.session_id)?;
        let permission = self.load_decision(
            permission_authority_id,
            permission_decision_id,
            GitWorkflowDecisionKind::Permission,
            requirement,
        )?;
        let explicit_approval = approval
            .map(|(authority_id, decision_id)| {
                self.load_decision(
                    authority_id,
                    decision_id,
                    GitWorkflowDecisionKind::ExplicitApproval,
                    requirement,
                )
            })
            .transpose()?;
        Ok(GitWorkflowAuthorizationEvidence {
            schema_version: "git-workflow-authorization-evidence/0.1".into(),
            authorization_id: authorization_id.into(),
            requirement_hash: requirement.requirement_hash.clone(),
            permission,
            explicit_approval,
        })
    }

    pub fn append_git_workflow_event(
        &mut self,
        record: &GitWorkflowRecord,
        event_kind: &str,
    ) -> Result<WorkbenchEvent, WorkbenchStoreError> {
        validate_record(record).map_err(|cause| error(cause.message))?;
        self.ensure_session_writable(&record.session_id)?;
        if !matches!(
            event_kind,
            "git.workflow.prepared"
                | "git.workflow.dispatching"
                | "git.workflow.in-progress"
                | "git.workflow.conflicted"
                | "git.workflow.completed"
                | "git.workflow.aborted"
                | "git.workflow.failed"
                | "git.workflow.recovered"
        ) {
            return Err(error("Git workflow event kind is invalid"));
        }
        let attempt = record
            .execution
            .as_ref()
            .ok_or_else(|| error("Git workflow event requires an execution attempt"))?;
        let record_bytes = serde_json::to_vec(record)
            .map_err(|_| error("cannot serialize Git workflow event record"))?;
        let record_hash = ContentHash::for_bytes(&record_bytes);
        let event_id = format!("git-workflow-{}-{}", record.operation_id, record.generation);
        let payload = json!({
            "schema_version": "git.workflow.lifecycle/0.1",
            "action": attempt.action,
            "state": record.state,
            "operation_kind": record.operation_kind,
            "authorization_id": attempt.authorization_id,
            "requirement_hash": attempt.requirement_hash,
            "record_hash": record_hash,
            "observed_head": record.observed_head,
            "observed_operation": record.observed_operation,
            "visible_conflict_count": record.conflicts.len(),
            "redacted_conflict_count": record.redacted_conflict_path_count,
            "command_exit_code": attempt.command_exit_code,
            "outcome": attempt.outcome,
        });
        let transaction =
            self.begin_database_write("cannot start Git workflow event transaction")?;
        let event = append_event_tx(
            &transaction,
            EventInput {
                session_id: &record.session_id,
                event_id: &event_id,
                timestamp_ms: record.updated_at_ms,
                correlation_id: &attempt.authorization_id,
                event_kind,
                project_id: Some(&record.project_id),
                operation_id: &record.operation_id,
                generation: record.generation,
                payload,
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit Git workflow event"))?;
        Ok(event)
    }

    pub fn append_session_compaction_checkpoint_event(
        &mut self,
        descriptor: &CompactionCheckpointDescriptor,
        review: &CompactionCheckpointReview,
        persisted_at_ms: u64,
    ) -> Result<WorkbenchEvent, WorkbenchStoreError> {
        self.append_session_compaction_checkpoint_event_internal(
            descriptor,
            review,
            None,
            persisted_at_ms,
        )
    }

    pub fn append_session_compaction_checkpoint_revision_event(
        &mut self,
        descriptor: &CompactionCheckpointDescriptor,
        review: &CompactionCheckpointReview,
        source_descriptor: &CompactionCheckpointDescriptor,
        source_review: &CompactionCheckpointReview,
        persisted_at_ms: u64,
    ) -> Result<WorkbenchEvent, WorkbenchStoreError> {
        validate_compaction_checkpoint_binding(source_descriptor, source_review)?;
        if source_review.session_id != review.session_id
            || source_review.checkpoint_id == review.checkpoint_id
            || source_review.review_id == review.review_id
        {
            return Err(error("compaction checkpoint revision lineage is invalid"));
        }
        self.validate_compaction_checkpoint_event(source_descriptor, source_review)?;
        self.append_session_compaction_checkpoint_event_internal(
            descriptor,
            review,
            Some(source_descriptor),
            persisted_at_ms,
        )
    }

    fn validate_compaction_checkpoint_event(
        &self,
        descriptor: &CompactionCheckpointDescriptor,
        review: &CompactionCheckpointReview,
    ) -> Result<(), WorkbenchStoreError> {
        let event_id =
            derived_event_id("session-compaction-checkpoint", review.review_id.as_bytes());
        let sequence = self
            .connection
            .query_row(
                "SELECT sequence FROM events WHERE session_id = ?1 AND event_id = ?2",
                params![review.session_id, event_id],
                |row| row.get::<_, i64>(0),
            )
            .optional()
            .map_err(|_| error("cannot inspect source compaction checkpoint event"))?
            .ok_or_else(|| error("source compaction checkpoint event is missing"))?;
        let event = self
            .read_session_events(
                &review.session_id,
                to_u64(sequence, "source compaction checkpoint event sequence")?.saturating_sub(1),
                1,
            )?
            .into_iter()
            .next()
            .ok_or_else(|| error("source compaction checkpoint event is unavailable"))?;
        let valid = event.event_kind == "session.compaction-checkpointed"
            && event.correlation_id == review.review_id
            && event.operation_id == review.checkpoint_id
            && event.generation == review.through_sequence
            && event.payload.get("session_id").and_then(Value::as_str)
                == Some(review.session_id.as_str())
            && event.payload.get("checkpoint_id").and_then(Value::as_str)
                == Some(review.checkpoint_id.as_str())
            && event.payload.get("review_id").and_then(Value::as_str)
                == Some(review.review_id.as_str())
            && event
                .payload
                .get("object_reference")
                .and_then(Value::as_str)
                == Some(descriptor.object_reference.as_str())
            && event.payload.get("state").and_then(Value::as_str) == Some("review-persisted")
            && event
                .payload
                .get("original_event_history_authoritative")
                .and_then(Value::as_bool)
                == Some(true);
        if valid {
            Ok(())
        } else {
            Err(error("source compaction checkpoint event is invalid"))
        }
    }

    fn append_session_compaction_checkpoint_event_internal(
        &mut self,
        descriptor: &CompactionCheckpointDescriptor,
        review: &CompactionCheckpointReview,
        source_descriptor: Option<&CompactionCheckpointDescriptor>,
        persisted_at_ms: u64,
    ) -> Result<WorkbenchEvent, WorkbenchStoreError> {
        validate_compaction_checkpoint_binding(descriptor, review)?;
        self.ensure_session_writable(&review.session_id)?;
        let session = self.load_session(&review.session_id)?;
        if session.status != "active" || persisted_at_ms < session.created_at_ms {
            return Err(error("compaction checkpoint session state is invalid"));
        }
        let event_id =
            derived_event_id("session-compaction-checkpoint", review.review_id.as_bytes());
        let mut payload = json!({
            "schema_version": "session.compaction-checkpointed/0.1",
            "session_id": review.session_id,
            "checkpoint_id": review.checkpoint_id,
            "review_id": review.review_id,
            "object_reference": descriptor.object_reference,
            "through_sequence": review.through_sequence,
            "source_context_hash": review.source_context_hash,
            "state": "review-persisted",
            "original_event_history_authoritative": true,
        });
        if let Some(source) = source_descriptor {
            payload
                .as_object_mut()
                .expect("compaction checkpoint payload object")
                .insert(
                    "supersedes".into(),
                    json!({
                        "checkpoint_id": source.checkpoint_id,
                        "review_id": source.review_id,
                        "object_reference": source.object_reference,
                    }),
                );
        }
        let existing_sequence = self
            .connection
            .query_row(
                "SELECT sequence FROM events WHERE session_id = ?1 AND event_id = ?2",
                params![review.session_id, event_id],
                |row| row.get::<_, i64>(0),
            )
            .optional()
            .map_err(|_| error("cannot inspect compaction checkpoint event"))?;
        if let Some(sequence) = existing_sequence {
            let sequence = to_u64(sequence, "compaction checkpoint event sequence")?;
            let event = self
                .read_session_events(&review.session_id, sequence.saturating_sub(1), 1)?
                .into_iter()
                .next()
                .ok_or_else(|| error("compaction checkpoint event is unavailable"))?;
            if event.event_kind == "session.compaction-checkpointed"
                && event.correlation_id == review.review_id
                && event.project_id == session.project_id
                && event.operation_id == review.checkpoint_id
                && event.generation == review.through_sequence
                && event.payload == payload
            {
                return Ok(event);
            }
            return Err(error("compaction checkpoint event identity conflicts"));
        }
        let transaction =
            self.begin_database_write("cannot start compaction checkpoint event transaction")?;
        let event = append_event_tx(
            &transaction,
            EventInput {
                session_id: &review.session_id,
                event_id: &event_id,
                timestamp_ms: persisted_at_ms,
                correlation_id: &review.review_id,
                event_kind: "session.compaction-checkpointed",
                project_id: session.project_id.as_deref(),
                operation_id: &review.checkpoint_id,
                generation: review.through_sequence,
                payload,
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit compaction checkpoint event"))?;
        Ok(event)
    }

    pub fn append_pinned_context_event(
        &mut self,
        project_id: &str,
        set_identity: &str,
        object_reference: &str,
        item_count: usize,
        persisted_at_ms: u64,
    ) -> Result<WorkbenchEvent, WorkbenchStoreError> {
        self.append_pinned_context_event_with_blob_releases(
            project_id,
            set_identity,
            object_reference,
            item_count,
            &[],
            persisted_at_ms,
        )
    }

    pub fn latest_pinned_context_event(
        &self,
        project_id: &str,
    ) -> Result<Option<WorkbenchEvent>, WorkbenchStoreError> {
        validate_identifier(project_id, "pinned context project ID")?;
        let candidate = self.rebuild_project_projection_candidate(project_id)?;
        if !candidate.source_complete {
            return Err(error(
                "pinned context project event authority is incomplete",
            ));
        }
        let events =
            load_projection_source_events(&self.connection, &project_event_stream_id(project_id))?;
        Ok(events
            .into_iter()
            .rev()
            .find(|event| event.event_kind == "project.pinned-context-updated"))
    }

    pub fn append_pinned_context_event_with_blob_releases(
        &mut self,
        project_id: &str,
        set_identity: &str,
        object_reference: &str,
        item_count: usize,
        release_reference_ids: &[String],
        persisted_at_ms: u64,
    ) -> Result<WorkbenchEvent, WorkbenchStoreError> {
        validate_identifier(project_id, "pinned context project ID")?;
        validate_pinned_context_reference(
            set_identity,
            "pinned-context:sha256:",
            "pinned context set identity",
        )?;
        validate_pinned_context_reference(
            object_reference,
            "pinned-context-object:sha256:",
            "pinned context object reference",
        )?;
        if item_count > 128 {
            return Err(error("pinned context item count is invalid"));
        }
        if release_reference_ids.len() > 128 {
            return Err(error("pinned context Blob release count is invalid"));
        }
        let mut unique_release_ids = BTreeSet::new();
        for reference_id in release_reference_ids {
            validate_identifier(reference_id, "pinned context Blob reference ID")?;
            if !unique_release_ids.insert(reference_id.as_str()) {
                return Err(error("pinned context Blob release is duplicated"));
            }
            let reference = load_durable_blob_reference(&self.connection, reference_id)?;
            validate_pinned_image_release(project_id, &reference)?;
            if let Some(session_id) = reference.session_id.as_deref() {
                self.ensure_session_writable(session_id)?;
            }
        }
        let project = self.load_project(project_id)?;
        if persisted_at_ms < project.created_at_ms {
            return Err(error("pinned context event timestamp is stale"));
        }
        let stream_id = project_event_stream_id(project_id);
        let operation_id = pinned_context_operation_id(project_id, set_identity);
        let release_batch_identity = if unique_release_ids.is_empty() {
            None
        } else {
            let batch = serde_json::to_vec(&json!({
                "project_id": project_id,
                "set_identity": set_identity,
                "reference_ids": unique_release_ids,
                "persisted_at_ms": persisted_at_ms,
            }))
            .map_err(|_| error("cannot serialize pinned context Blob release batch"))?;
            Some(format!(
                "pinned-context-release:sha256:{}",
                ContentHash::for_bytes(&batch).sha256
            ))
        };
        let event_seed = release_batch_identity.as_ref().map_or_else(
            || format!("{project_id}:{set_identity}"),
            |release_batch_identity| {
                format!("{project_id}:{set_identity}:{release_batch_identity}")
            },
        );
        let event_id = derived_event_id("pinned-context-updated", event_seed.as_bytes());
        let mut payload = json!({
            "schema_version": "project.pinned-context-updated/0.1",
            "project_id": project_id,
            "set_identity": set_identity,
            "object_reference": object_reference,
            "item_count": item_count,
            "content_bodies_persisted": false,
            "state": "metadata-persisted",
            "persisted_at_ms": persisted_at_ms,
        });
        if let Some(release_batch_identity) = release_batch_identity.as_deref() {
            payload["release_batch_identity"] = release_batch_identity.into();
            payload["released_blob_reference_count"] = unique_release_ids.len().into();
        }
        let existing_sequence = self
            .connection
            .query_row(
                "SELECT sequence FROM events WHERE session_id = ?1 AND event_id = ?2",
                params![stream_id, event_id],
                |row| row.get::<_, i64>(0),
            )
            .optional()
            .map_err(|_| error("cannot inspect pinned context event"))?;
        if let Some(sequence) = existing_sequence {
            let sequence = to_u64(sequence, "pinned context event sequence")?;
            let event = self
                .read_session_events(
                    &project_event_stream_id(project_id),
                    sequence.saturating_sub(1),
                    1,
                )?
                .into_iter()
                .next()
                .ok_or_else(|| error("pinned context event is unavailable"))?;
            if event.event_kind == "project.pinned-context-updated"
                && event.event_id == event_id
                && event.correlation_id == set_identity
                && event.project_id.as_deref() == Some(project_id)
                && event.operation_id == operation_id
                && event.generation == 0
                && event.payload.get("schema_version").and_then(Value::as_str)
                    == Some("project.pinned-context-updated/0.1")
                && event.payload.get("project_id").and_then(Value::as_str) == Some(project_id)
                && event.payload.get("set_identity").and_then(Value::as_str) == Some(set_identity)
                && event
                    .payload
                    .get("object_reference")
                    .and_then(Value::as_str)
                    == Some(object_reference)
                && event.payload.get("item_count").and_then(Value::as_u64)
                    == Some(item_count as u64)
                && event
                    .payload
                    .get("content_bodies_persisted")
                    .and_then(Value::as_bool)
                    == Some(false)
                && event.payload.get("state").and_then(Value::as_str) == Some("metadata-persisted")
                && event
                    .payload
                    .get("release_batch_identity")
                    .and_then(Value::as_str)
                    == release_batch_identity.as_deref()
                && event
                    .payload
                    .get("released_blob_reference_count")
                    .and_then(Value::as_u64)
                    == release_batch_identity
                        .as_ref()
                        .map(|_| unique_release_ids.len() as u64)
            {
                for reference_id in &unique_release_ids {
                    let reference = load_durable_blob_reference(&self.connection, reference_id)?;
                    validate_pinned_image_release(project_id, &reference)?;
                    if reference.state != "released" {
                        return Err(error(
                            "pinned context event exists without its Blob release binding",
                        ));
                    }
                }
                return Ok(event);
            }
            return Err(error("pinned context event identity conflicts"));
        }
        let transaction =
            self.begin_database_write("cannot start pinned context event transaction")?;
        let event = append_event_tx(
            &transaction,
            EventInput {
                session_id: &stream_id,
                event_id: &event_id,
                timestamp_ms: persisted_at_ms,
                correlation_id: set_identity,
                event_kind: "project.pinned-context-updated",
                project_id: Some(project_id),
                operation_id: &operation_id,
                generation: 0,
                payload,
            },
        )?;
        for reference_id in &unique_release_ids {
            let reference = load_durable_blob_reference_tx(&transaction, reference_id)?;
            validate_pinned_image_release(project_id, &reference)?;
            release_durable_blob_reference_tx(&transaction, reference_id, persisted_at_ms)?;
        }
        transaction
            .commit()
            .map_err(|_| error("cannot commit pinned context event"))?;
        Ok(event)
    }

    pub fn append_operation_reconciliation(
        &mut self,
        input: &ReconciliationInput,
        result: &ReconciliationResult,
        updated_at_ms: u64,
    ) -> Result<StoredOperationReconciliation, WorkbenchStoreError> {
        let expected = reconcile_operation(input).map_err(|cause| {
            error(format!(
                "operation reconciliation is invalid: {}",
                cause.message
            ))
        })?;
        if expected != *result {
            return Err(error(
                "operation reconciliation result does not match its evidence",
            ));
        }
        self.ensure_session_writable(&input.session_id)?;
        let session = self.load_session(&input.session_id)?;
        if updated_at_ms < session.created_at_ms {
            return Err(error("operation reconciliation timestamp is stale"));
        }
        if let Some(existing) =
            self.latest_operation_reconciliation(&input.session_id, &input.operation_id)?
        {
            if existing.input == *input && existing.result == *result {
                return Ok(existing);
            }
        }
        let event_id = derived_event_id(
            "operation-reconciled",
            format!("{}:{}", input.operation_id, result.review_id).as_bytes(),
        );
        let payload = json!({
            "schema_version": "operation.reconciled/0.1",
            "input": input,
            "result": result,
            "updated_at_ms": updated_at_ms,
        });
        let transaction =
            self.begin_database_write("cannot start operation reconciliation transaction")?;
        let event = append_event_tx(
            &transaction,
            EventInput {
                session_id: &input.session_id,
                event_id: &event_id,
                timestamp_ms: updated_at_ms,
                correlation_id: &result.review_id,
                event_kind: "operation.reconciled",
                project_id: session.project_id.as_deref(),
                operation_id: &input.operation_id,
                generation: 0,
                payload,
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit operation reconciliation event"))?;
        Ok(StoredOperationReconciliation {
            input: input.clone(),
            result: result.clone(),
            event_sequence: event.sequence,
            updated_at_ms,
        })
    }

    pub fn latest_operation_reconciliation(
        &self,
        session_id: &str,
        operation_id: &str,
    ) -> Result<Option<StoredOperationReconciliation>, WorkbenchStoreError> {
        validate_identifier(session_id, "operation reconciliation session ID")?;
        validate_identifier(operation_id, "operation reconciliation ID")?;
        let sequence = self
            .connection
            .query_row(
                "SELECT sequence FROM events
                 WHERE session_id = ?1 AND event_kind = 'operation.reconciled'
                   AND operation_id = ?2
                 ORDER BY sequence DESC LIMIT 1",
                params![session_id, operation_id],
                |row| row.get::<_, i64>(0),
            )
            .optional()
            .map_err(|_| error("cannot inspect operation reconciliation event"))?;
        let Some(sequence) = sequence else {
            return Ok(None);
        };
        let sequence = to_u64(sequence, "operation reconciliation event sequence")?;
        let event = self
            .read_session_events(session_id, sequence.saturating_sub(1), 1)?
            .into_iter()
            .next()
            .ok_or_else(|| error("operation reconciliation event is unavailable"))?;
        Ok(Some(parse_operation_reconciliation_event(&event)?))
    }

    pub fn latest_operation_event_state(
        &self,
        session_id: &str,
        operation_id: &str,
    ) -> Result<Option<EventState>, WorkbenchStoreError> {
        validate_identifier(session_id, "operation event session ID")?;
        validate_identifier(operation_id, "operation event ID")?;
        let event = self
            .connection
            .query_row(
                "SELECT sequence FROM events
                 WHERE session_id = ?1 AND operation_id = ?2
                 ORDER BY sequence DESC LIMIT 1",
                params![session_id, operation_id],
                |row| row.get::<_, i64>(0),
            )
            .optional()
            .map_err(|_| error("cannot inspect operation event state"))?;
        let Some(sequence) = event else {
            return Ok(None);
        };
        let sequence = to_u64(sequence, "operation event sequence")?;
        let event = self
            .read_session_events(session_id, sequence.saturating_sub(1), 1)?
            .into_iter()
            .next()
            .ok_or_else(|| error("operation event is unavailable"))?;
        if event.operation_id != operation_id {
            return Err(error("operation event identity is invalid"));
        }
        Ok(match event.event_kind.as_str() {
            "turn.completed" => Some(EventState::Completed),
            "turn.failed" => Some(EventState::Failed),
            "turn.interrupted" | "turn.cancelled" => Some(EventState::Interrupted),
            "turn.created" => Some(EventState::Running),
            "git.workflow.prepared" | "git.workflow.dispatching" | "git.workflow.in-progress" => {
                Self::validate_git_workflow_lifecycle_event(&event)?;
                Some(EventState::Running)
            }
            "git.workflow.completed" => {
                Self::validate_git_workflow_lifecycle_event(&event)?;
                Some(EventState::Completed)
            }
            "git.workflow.failed" => {
                Self::validate_git_workflow_lifecycle_event(&event)?;
                Some(EventState::Failed)
            }
            "git.workflow.aborted" => {
                Self::validate_git_workflow_lifecycle_event(&event)?;
                Some(EventState::Interrupted)
            }
            "git.workflow.conflicted" | "git.workflow.recovered" => {
                Self::validate_git_workflow_lifecycle_event(&event)?;
                None
            }
            _ => None,
        })
    }

    fn validate_git_workflow_lifecycle_event(
        event: &WorkbenchEvent,
    ) -> Result<(), WorkbenchStoreError> {
        let payload: GitWorkflowLifecyclePayload = serde_json::from_value(event.payload.clone())
            .map_err(|_| error("Git workflow lifecycle event payload is invalid"))?;
        if payload.schema_version != "git.workflow.lifecycle/0.1"
            || payload.action.is_empty()
            || payload.operation_kind.is_empty()
            || payload.authorization_id.is_empty()
            || payload.observed_head.is_empty()
            || payload.state.is_empty()
            || payload.visible_conflict_count > 5_000
            || payload.redacted_conflict_count > 5_000
        {
            return Err(error("Git workflow lifecycle event identity is invalid"));
        }
        validate_identity_text(&payload.action, "Git workflow action")?;
        validate_identity_text(&payload.state, "Git workflow state")?;
        validate_identity_text(&payload.operation_kind, "Git workflow operation kind")?;
        validate_identity_text(&payload.authorization_id, "Git workflow authorization ID")?;
        validate_identity_text(&payload.observed_head, "Git workflow observed HEAD")?;
        if let Some(observed_operation) = payload.observed_operation.as_deref() {
            validate_identity_text(observed_operation, "Git workflow observed operation")?;
        }
        if let Some(outcome) = payload.outcome.as_deref() {
            validate_identity_text(outcome, "Git workflow outcome")?;
        }
        let _ = payload.command_exit_code;
        validate_content_hash(&payload.requirement_hash, "Git workflow requirement hash")?;
        validate_content_hash(&payload.record_hash, "Git workflow record hash")?;
        Ok(())
    }

    pub fn session_blocked_operation(
        &self,
        session_id: &str,
    ) -> Result<Option<StoredOperationReconciliation>, WorkbenchStoreError> {
        validate_identifier(session_id, "blocked operation session ID")?;
        let mut sequences = self
            .connection
            .prepare(
                "SELECT event.sequence
                 FROM events AS event
                 WHERE event.session_id = ?1
                   AND event.event_kind = 'operation.reconciled'
                   AND NOT EXISTS (
                       SELECT 1 FROM events AS newer
                       WHERE newer.session_id = event.session_id
                         AND newer.event_kind = 'operation.reconciled'
                         AND newer.operation_id = event.operation_id
                         AND newer.sequence > event.sequence
                   )
                 ORDER BY event.sequence DESC
                 LIMIT 257",
            )
            .map_err(|_| error("cannot prepare blocked operation scan"))?;
        let rows = sequences
            .query_map([session_id], |row| row.get::<_, i64>(0))
            .map_err(|_| error("cannot read blocked operation scan"))?;
        let mut event_sequences = Vec::new();
        for row in rows {
            event_sequences.push(to_u64(
                row.map_err(|_| error("blocked operation sequence is invalid"))?,
                "blocked operation sequence",
            )?);
        }
        if event_sequences.len() > 256 {
            return Err(error("blocked operation scan limit exceeded"));
        }
        for sequence in event_sequences {
            let event = self
                .read_session_events(session_id, sequence.saturating_sub(1), 1)?
                .into_iter()
                .next()
                .ok_or_else(|| error("blocked operation event is unavailable"))?;
            let reconciliation = parse_operation_reconciliation_event(&event)?;
            if reconciliation.result.writes_blocked {
                return Ok(Some(reconciliation));
            }
        }
        Ok(None)
    }

    pub fn load_operation_reconciliations(
        &self,
    ) -> Result<Vec<StoredOperationReconciliation>, WorkbenchStoreError> {
        let limit = i64::try_from(MAX_OPERATION_RECONCILIATIONS + 1)
            .map_err(|_| error("operation reconciliation limit is invalid"))?;
        let mut statement = self
            .connection
            .prepare(
                "SELECT session_id, operation_id, MAX(sequence) AS latest_sequence
                 FROM events
                 WHERE event_kind = 'operation.reconciled'
                 GROUP BY session_id, operation_id
                 ORDER BY latest_sequence ASC
                 LIMIT ?1",
            )
            .map_err(|_| error("cannot prepare operation reconciliation startup scan"))?;
        let rows = statement
            .query_map([limit], |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    row.get::<_, String>(1)?,
                    row.get::<_, i64>(2)?,
                ))
            })
            .map_err(|_| error("cannot read operation reconciliation startup scan"))?;
        let mut latest = Vec::new();
        for row in rows {
            let (session_id, operation_id, sequence) =
                row.map_err(|_| error("operation reconciliation startup row is invalid"))?;
            latest.push((
                session_id,
                operation_id,
                to_u64(sequence, "operation reconciliation event sequence")?,
            ));
        }
        if latest.len() > MAX_OPERATION_RECONCILIATIONS {
            return Err(error(
                "operation reconciliation startup scan limit exceeded",
            ));
        }
        latest
            .into_iter()
            .map(|(session_id, operation_id, sequence)| {
                let event = self
                    .read_session_events(&session_id, sequence.saturating_sub(1), 1)?
                    .into_iter()
                    .next()
                    .ok_or_else(|| error("operation reconciliation startup event is missing"))?;
                let record = parse_operation_reconciliation_event(&event)?;
                if record.input.session_id != session_id
                    || record.input.operation_id != operation_id
                    || record.event_sequence != sequence
                {
                    return Err(error(
                        "operation reconciliation startup identity is invalid",
                    ));
                }
                Ok(record)
            })
            .collect()
    }

    pub fn read_session_events(
        &self,
        session_id: &str,
        after_sequence: u64,
        limit: usize,
    ) -> Result<Vec<WorkbenchEvent>, WorkbenchStoreError> {
        validate_identifier(session_id, "session ID")?;
        if limit == 0 || limit > MAX_EVENT_PAGE {
            return Err(error("event page limit is invalid"));
        }
        let mut statement = self
            .connection
            .prepare(
                "SELECT sequence, event_id, timestamp_ms, correlation_id, event_kind,
                        project_id, operation_id, generation, payload_json,
                        payload_sha256, payload_bytes
                 FROM events
                 WHERE session_id = ?1 AND sequence > ?2
                 ORDER BY sequence ASC
                 LIMIT ?3",
            )
            .map_err(|_| error("cannot prepare event replay"))?;
        let rows = statement
            .query_map(
                params![
                    session_id,
                    to_i64(after_sequence, "event sequence")?,
                    i64::try_from(limit).map_err(|_| error("event page limit is invalid"))?,
                ],
                |row| {
                    Ok((
                        row.get::<_, i64>(0)?,
                        row.get::<_, String>(1)?,
                        row.get::<_, i64>(2)?,
                        row.get::<_, String>(3)?,
                        row.get::<_, String>(4)?,
                        row.get::<_, Option<String>>(5)?,
                        row.get::<_, String>(6)?,
                        row.get::<_, i64>(7)?,
                        row.get::<_, String>(8)?,
                        row.get::<_, String>(9)?,
                        row.get::<_, i64>(10)?,
                    ))
                },
            )
            .map_err(|_| error("cannot read event replay"))?;
        let mut events = Vec::new();
        let mut expected_sequence = after_sequence.saturating_add(1);
        for row in rows {
            let (
                sequence,
                event_id,
                timestamp_ms,
                correlation_id,
                event_kind,
                project_id,
                operation_id,
                generation,
                payload_json,
                payload_sha256,
                payload_bytes,
            ) = row.map_err(|_| error("event replay row is invalid"))?;
            let sequence = to_u64(sequence, "event sequence")?;
            if sequence != expected_sequence {
                return Err(error("event replay sequence has a gap"));
            }
            expected_sequence = expected_sequence.saturating_add(1);
            let payload = serde_json::from_str(&payload_json)
                .map_err(|_| error("event payload JSON is invalid"))?;
            validate_persisted_payload(&payload, "event payload")?;
            let payload_hash = ContentHash {
                sha256: payload_sha256,
                bytes: to_u64(payload_bytes, "event payload byte count")?,
            };
            if ContentHash::for_bytes(payload_json.as_bytes()) != payload_hash {
                return Err(error("event payload integrity check failed"));
            }
            events.push(WorkbenchEvent {
                schema_version: "workbench-event/0.1".into(),
                session_id: session_id.into(),
                sequence,
                event_id,
                timestamp_ms: to_u64(timestamp_ms, "event timestamp")?,
                correlation_id,
                event_kind,
                project_id,
                operation_id,
                generation: to_u64(generation, "event generation")?,
                payload,
                payload_hash,
            });
        }
        Ok(events)
    }

    fn configure(&mut self) -> Result<(), WorkbenchStoreError> {
        self.connection
            .busy_timeout(Duration::from_secs(2))
            .map_err(|_| error("cannot configure workbench busy timeout"))?;
        self.connection
            .execute_batch(
                "PRAGMA foreign_keys = ON;
                 PRAGMA synchronous = FULL;
                 PRAGMA trusted_schema = OFF;
                 PRAGMA temp_store = MEMORY;",
            )
            .map_err(|_| error("cannot configure workbench database"))?;
        let journal: String = self
            .connection
            .query_row("PRAGMA journal_mode = WAL", [], |row| row.get(0))
            .map_err(|_| error("cannot enable workbench WAL mode"))?;
        if !journal.eq_ignore_ascii_case("wal") {
            return Err(error("workbench database did not enter WAL mode"));
        }
        self.connection
            .pragma_update(None, "wal_autocheckpoint", WAL_AUTOCHECKPOINT_PAGES)
            .map_err(|_| error("cannot configure workbench WAL checkpoint bound"))?;
        self.connection
            .pragma_update(None, "journal_size_limit", WAL_JOURNAL_SIZE_LIMIT_BYTES)
            .map_err(|_| error("cannot configure workbench WAL size limit"))?;
        let page_size: i64 = self
            .connection
            .pragma_query_value(None, "page_size", |row| row.get(0))
            .map_err(|_| error("cannot read workbench database page size"))?;
        let page_size = to_u64(page_size, "workbench database page size")?;
        if page_size == 0 {
            return Err(error("workbench database page size is invalid"));
        }
        let max_page_count = i64::try_from(MAX_DATABASE_BYTES / page_size)
            .map_err(|_| error("workbench database page limit is invalid"))?;
        self.connection
            .pragma_update(None, "max_page_count", max_page_count)
            .map_err(|_| error("cannot configure workbench database size limit"))?;
        let application_id: i64 = self
            .connection
            .pragma_query_value(None, "application_id", |row| row.get(0))
            .map_err(|_| error("cannot read workbench application ID"))?;
        if application_id != 0 && application_id != APPLICATION_ID {
            return Err(error("workbench database application ID is invalid"));
        }
        Ok(())
    }

    fn claim_application_id(&mut self) -> Result<(), WorkbenchStoreError> {
        let application_id: i64 = self
            .connection
            .pragma_query_value(None, "application_id", |row| row.get(0))
            .map_err(|_| error("cannot read workbench application ID"))?;
        if application_id == 0 {
            self.connection
                .pragma_update(None, "application_id", APPLICATION_ID)
                .map_err(|_| error("cannot set workbench application ID"))?;
        } else if application_id != APPLICATION_ID {
            return Err(error("workbench database application ID is invalid"));
        }
        Ok(())
    }

    fn migrate(&mut self) -> Result<(), WorkbenchStoreError> {
        let version: i64 = self
            .connection
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .map_err(|_| error("cannot read workbench schema version"))?;
        if version > SCHEMA_VERSION {
            return Err(error(
                "workbench database schema is newer than this runtime",
            ));
        }
        if version == SCHEMA_VERSION {
            return Ok(());
        }
        if version == 12 {
            let transaction = self
                .connection
                .transaction_with_behavior(TransactionBehavior::Immediate)
                .map_err(|_| error("cannot start session workspace binding migration"))?;
            apply_session_search_indexes(&transaction)?;
            verify_required_schema(&transaction)?;
            transaction
                .pragma_update(None, "user_version", SCHEMA_VERSION)
                .map_err(|_| error("cannot update workbench schema version"))?;
            return transaction
                .commit()
                .map_err(|_| error("cannot commit session workspace binding migration"));
        }
        if version == 11 {
            let transaction = self
                .connection
                .transaction_with_behavior(TransactionBehavior::Immediate)
                .map_err(|_| error("cannot start background notification schema migration"))?;
            transaction
                .execute_batch(BACKGROUND_NOTIFICATION_OUTBOX_SCHEMA_SQL)
                .map_err(|_| error("cannot apply background notification schema migration"))?;
            apply_session_search_indexes(&transaction)?;
            verify_required_schema(&transaction)?;
            transaction
                .pragma_update(None, "user_version", SCHEMA_VERSION)
                .map_err(|_| error("cannot update workbench schema version"))?;
            return transaction
                .commit()
                .map_err(|_| error("cannot commit background notification schema migration"));
        }
        if version == 10 {
            let transaction = self
                .connection
                .transaction_with_behavior(TransactionBehavior::Immediate)
                .map_err(|_| error("cannot start background lease schema migration"))?;
            transaction
                .execute_batch(BACKGROUND_LEASE_SCHEMA_SQL)
                .map_err(|_| error("cannot apply background lease schema migration"))?;
            transaction
                .execute_batch(BACKGROUND_NOTIFICATION_OUTBOX_SCHEMA_SQL)
                .map_err(|_| error("cannot apply background notification schema migration"))?;
            apply_session_search_indexes(&transaction)?;
            verify_required_schema(&transaction)?;
            transaction
                .pragma_update(None, "user_version", SCHEMA_VERSION)
                .map_err(|_| error("cannot update workbench schema version"))?;
            return transaction
                .commit()
                .map_err(|_| error("cannot commit background lease schema migration"));
        }
        if version == 9 {
            let transaction = self
                .connection
                .transaction_with_behavior(TransactionBehavior::Immediate)
                .map_err(|_| error("cannot start background job schema migration"))?;
            apply_session_search_indexes(&transaction)?;
            verify_required_schema(&transaction)?;
            transaction
                .pragma_update(None, "user_version", SCHEMA_VERSION)
                .map_err(|_| error("cannot update workbench schema version"))?;
            return transaction
                .commit()
                .map_err(|_| error("cannot commit background job schema migration"));
        }
        if version == 8 {
            let transaction = self
                .connection
                .transaction_with_behavior(TransactionBehavior::Immediate)
                .map_err(|_| error("cannot start session search index migration"))?;
            apply_session_search_indexes(&transaction)?;
            verify_required_schema(&transaction)?;
            transaction
                .pragma_update(None, "user_version", SCHEMA_VERSION)
                .map_err(|_| error("cannot update workbench schema version"))?;
            return transaction
                .commit()
                .map_err(|_| error("cannot commit session search index migration"));
        }
        if version == 7 {
            let transaction = self
                .connection
                .transaction_with_behavior(TransactionBehavior::Immediate)
                .map_err(|_| error("cannot start session runtime binding migration"))?;
            transaction
                .execute_batch(SESSION_RUNTIME_BINDING_SCHEMA_SQL)
                .map_err(|_| error("cannot apply session runtime binding migration"))?;
            apply_session_search_indexes(&transaction)?;
            verify_required_schema(&transaction)?;
            transaction
                .pragma_update(None, "user_version", SCHEMA_VERSION)
                .map_err(|_| error("cannot update workbench schema version"))?;
            return transaction
                .commit()
                .map_err(|_| error("cannot commit session runtime binding migration"));
        }
        if version == 6 {
            let transaction = self
                .connection
                .transaction_with_behavior(TransactionBehavior::Immediate)
                .map_err(|_| error("cannot start project navigation schema migration"))?;
            transaction
                .execute_batch(PROJECT_NAVIGATION_SCHEMA_SQL)
                .map_err(|_| error("cannot apply project navigation schema migration"))?;
            transaction
                .execute_batch(SESSION_RUNTIME_BINDING_SCHEMA_SQL)
                .map_err(|_| error("cannot apply session runtime binding migration"))?;
            apply_session_search_indexes(&transaction)?;
            verify_required_schema(&transaction)?;
            transaction
                .pragma_update(None, "user_version", SCHEMA_VERSION)
                .map_err(|_| error("cannot update workbench schema version"))?;
            return transaction
                .commit()
                .map_err(|_| error("cannot commit project navigation schema migration"));
        }
        if version == 5 {
            let transaction = self
                .connection
                .transaction_with_behavior(TransactionBehavior::Immediate)
                .map_err(|_| error("cannot start retention schema migration"))?;
            transaction
                .execute_batch(RETENTION_SCHEMA_SQL)
                .map_err(|_| error("cannot apply retention schema migration"))?;
            transaction
                .execute_batch(PROJECT_NAVIGATION_SCHEMA_SQL)
                .map_err(|_| error("cannot apply project navigation schema migration"))?;
            transaction
                .execute_batch(SESSION_RUNTIME_BINDING_SCHEMA_SQL)
                .map_err(|_| error("cannot apply session runtime binding migration"))?;
            apply_session_search_indexes(&transaction)?;
            verify_required_schema(&transaction)?;
            transaction
                .pragma_update(None, "user_version", SCHEMA_VERSION)
                .map_err(|_| error("cannot update workbench schema version"))?;
            return transaction
                .commit()
                .map_err(|_| error("cannot commit retention schema migration"));
        }
        if version == 4 {
            let transaction = self
                .connection
                .transaction_with_behavior(TransactionBehavior::Immediate)
                .map_err(|_| error("cannot start durable blob schema migration"))?;
            transaction
                .execute_batch(DURABLE_BLOB_SCHEMA_SQL)
                .map_err(|_| error("cannot apply durable blob schema migration"))?;
            transaction
                .execute_batch(RETENTION_SCHEMA_SQL)
                .map_err(|_| error("cannot apply retention schema migration"))?;
            transaction
                .execute_batch(PROJECT_NAVIGATION_SCHEMA_SQL)
                .map_err(|_| error("cannot apply project navigation schema migration"))?;
            transaction
                .execute_batch(SESSION_RUNTIME_BINDING_SCHEMA_SQL)
                .map_err(|_| error("cannot apply session runtime binding migration"))?;
            apply_session_search_indexes(&transaction)?;
            verify_required_schema(&transaction)?;
            transaction
                .pragma_update(None, "user_version", SCHEMA_VERSION)
                .map_err(|_| error("cannot update workbench schema version"))?;
            return transaction
                .commit()
                .map_err(|_| error("cannot commit durable blob schema migration"));
        }
        if version == 3 {
            let transaction = self
                .connection
                .transaction_with_behavior(TransactionBehavior::Immediate)
                .map_err(|_| error("cannot start optional event project migration"))?;
            transaction
                .execute_batch(OPTIONAL_EVENT_PROJECT_SCHEMA_SQL)
                .map_err(|_| error("cannot make event project binding optional"))?;
            transaction
                .execute_batch(PROJECTION_SOURCE_SCHEMA_SQL)
                .map_err(|_| error("cannot create session projection source schema"))?;
            transaction
                .execute_batch(DURABLE_BLOB_SCHEMA_SQL)
                .map_err(|_| error("cannot apply durable blob schema migration"))?;
            transaction
                .execute_batch(RETENTION_SCHEMA_SQL)
                .map_err(|_| error("cannot apply retention schema migration"))?;
            transaction
                .execute_batch(PROJECT_NAVIGATION_SCHEMA_SQL)
                .map_err(|_| error("cannot apply project navigation schema migration"))?;
            transaction
                .execute_batch(SESSION_RUNTIME_BINDING_SCHEMA_SQL)
                .map_err(|_| error("cannot apply session runtime binding migration"))?;
            apply_session_search_indexes(&transaction)?;
            verify_required_schema(&transaction)?;
            transaction
                .pragma_update(None, "user_version", SCHEMA_VERSION)
                .map_err(|_| error("cannot update workbench schema version"))?;
            return transaction
                .commit()
                .map_err(|_| error("cannot commit optional event project migration"));
        }
        if version == 1 {
            let transaction = self
                .connection
                .transaction_with_behavior(TransactionBehavior::Immediate)
                .map_err(|_| error("cannot start workbench schema migration"))?;
            transaction
                .execute_batch(SESSION_SCHEMA_SQL)
                .map_err(|_| error("cannot apply workbench session schema migration"))?;
            transaction
                .execute_batch(TURN_ITEM_SCHEMA_SQL)
                .map_err(|_| error("cannot apply workbench turn schema migration"))?;
            transaction
                .execute_batch(OPTIONAL_EVENT_PROJECT_SCHEMA_SQL)
                .map_err(|_| error("cannot make event project binding optional"))?;
            transaction
                .execute_batch(PROJECTION_SOURCE_SCHEMA_SQL)
                .map_err(|_| error("cannot create session projection source schema"))?;
            transaction
                .execute_batch(DURABLE_BLOB_SCHEMA_SQL)
                .map_err(|_| error("cannot apply durable blob schema migration"))?;
            transaction
                .execute_batch(RETENTION_SCHEMA_SQL)
                .map_err(|_| error("cannot apply retention schema migration"))?;
            transaction
                .execute_batch(PROJECT_NAVIGATION_SCHEMA_SQL)
                .map_err(|_| error("cannot apply project navigation schema migration"))?;
            transaction
                .execute_batch(SESSION_RUNTIME_BINDING_SCHEMA_SQL)
                .map_err(|_| error("cannot apply session runtime binding migration"))?;
            apply_session_search_indexes(&transaction)?;
            verify_required_schema(&transaction)?;
            transaction
                .pragma_update(None, "user_version", SCHEMA_VERSION)
                .map_err(|_| error("cannot update workbench schema version"))?;
            return transaction
                .commit()
                .map_err(|_| error("cannot commit workbench session schema migration"));
        }
        if version == 2 {
            let transaction = self
                .connection
                .transaction_with_behavior(TransactionBehavior::Immediate)
                .map_err(|_| error("cannot start workbench turn schema migration"))?;
            transaction
                .execute_batch(TURN_ITEM_SCHEMA_SQL)
                .map_err(|_| error("cannot apply workbench turn schema migration"))?;
            transaction
                .execute_batch(OPTIONAL_EVENT_PROJECT_SCHEMA_SQL)
                .map_err(|_| error("cannot make event project binding optional"))?;
            transaction
                .execute_batch(PROJECTION_SOURCE_SCHEMA_SQL)
                .map_err(|_| error("cannot create session projection source schema"))?;
            transaction
                .execute_batch(DURABLE_BLOB_SCHEMA_SQL)
                .map_err(|_| error("cannot apply durable blob schema migration"))?;
            transaction
                .execute_batch(RETENTION_SCHEMA_SQL)
                .map_err(|_| error("cannot apply retention schema migration"))?;
            transaction
                .execute_batch(PROJECT_NAVIGATION_SCHEMA_SQL)
                .map_err(|_| error("cannot apply project navigation schema migration"))?;
            transaction
                .execute_batch(SESSION_RUNTIME_BINDING_SCHEMA_SQL)
                .map_err(|_| error("cannot apply session runtime binding migration"))?;
            apply_session_search_indexes(&transaction)?;
            verify_required_schema(&transaction)?;
            transaction
                .pragma_update(None, "user_version", SCHEMA_VERSION)
                .map_err(|_| error("cannot update workbench schema version"))?;
            return transaction
                .commit()
                .map_err(|_| error("cannot commit workbench turn schema migration"));
        }
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .map_err(|_| error("cannot start workbench migration"))?;
        transaction
            .execute_batch(
                "CREATE TABLE approval_decisions (
                    authority_id TEXT NOT NULL,
                    decision_id TEXT NOT NULL,
                    decision_kind TEXT NOT NULL CHECK(decision_kind IN ('permission','explicit-approval')),
                    requirement_sha256 TEXT NOT NULL,
                    requirement_bytes INTEGER NOT NULL CHECK(requirement_bytes >= 0),
                    project_id TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    operation_id TEXT NOT NULL,
                    action TEXT NOT NULL CHECK(action IN ('start','abort','continue')),
                    risk_class TEXT NOT NULL CHECK(risk_class IN ('medium','high')),
                    scope TEXT NOT NULL CHECK(scope = 'allow-once'),
                    issued_at_ms INTEGER NOT NULL CHECK(issued_at_ms >= 0),
                    expires_at_ms INTEGER NOT NULL CHECK(expires_at_ms >= issued_at_ms),
                    status TEXT NOT NULL CHECK(status IN ('issued','consumed','revoked')),
                    created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),
                    consumed_at_ms INTEGER,
                    PRIMARY KEY(authority_id, decision_id)
                 ) STRICT;
                 CREATE INDEX approval_scope_idx
                    ON approval_decisions(requirement_sha256, decision_kind, status);
                 CREATE TABLE authorization_consumptions (
                    authorization_id TEXT PRIMARY KEY,
                    requirement_sha256 TEXT NOT NULL,
                    requirement_bytes INTEGER NOT NULL CHECK(requirement_bytes >= 0),
                    project_id TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    operation_id TEXT NOT NULL,
                    action TEXT NOT NULL,
                    consumed_at_ms INTEGER NOT NULL CHECK(consumed_at_ms >= 0)
                 ) STRICT;
                 CREATE TABLE session_sequences (
                    session_id TEXT PRIMARY KEY,
                    next_sequence INTEGER NOT NULL CHECK(next_sequence >= 1)
                 ) STRICT;
                 CREATE TABLE events (
                    session_id TEXT NOT NULL,
                    sequence INTEGER NOT NULL CHECK(sequence >= 1),
                    event_id TEXT NOT NULL UNIQUE,
                    timestamp_ms INTEGER NOT NULL CHECK(timestamp_ms >= 0),
                    correlation_id TEXT NOT NULL,
                    event_kind TEXT NOT NULL,
                    project_id TEXT,
                    operation_id TEXT NOT NULL,
                    generation INTEGER NOT NULL CHECK(generation >= 0),
                    payload_json TEXT NOT NULL,
                    payload_sha256 TEXT NOT NULL,
                    payload_bytes INTEGER NOT NULL CHECK(payload_bytes >= 0),
                    PRIMARY KEY(session_id, sequence)
                 ) STRICT;
                 CREATE INDEX events_operation_idx
                    ON events(project_id, operation_id, sequence);",
            )
            .map_err(|_| error("cannot apply workbench schema migration"))?;
        transaction
            .execute_batch(SESSION_SCHEMA_SQL)
            .map_err(|_| error("cannot apply workbench session schema migration"))?;
        transaction
            .execute_batch(PROJECTION_SOURCE_SCHEMA_SQL)
            .map_err(|_| error("cannot create session projection source schema"))?;
        transaction
            .execute_batch(TURN_ITEM_SCHEMA_SQL)
            .map_err(|_| error("cannot apply workbench turn schema migration"))?;
        transaction
            .execute_batch(DURABLE_BLOB_SCHEMA_SQL)
            .map_err(|_| error("cannot apply durable blob schema migration"))?;
        transaction
            .execute_batch(RETENTION_SCHEMA_SQL)
            .map_err(|_| error("cannot apply retention schema migration"))?;
        transaction
            .execute_batch(PROJECT_NAVIGATION_SCHEMA_SQL)
            .map_err(|_| error("cannot apply project navigation schema migration"))?;
        transaction
            .execute_batch(SESSION_RUNTIME_BINDING_SCHEMA_SQL)
            .map_err(|_| error("cannot apply session runtime binding migration"))?;
        apply_session_search_indexes(&transaction)?;
        verify_required_schema(&transaction)?;
        transaction
            .pragma_update(None, "user_version", SCHEMA_VERSION)
            .map_err(|_| error("cannot update workbench schema version"))?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit workbench schema migration"))
    }

    fn verify_integrity(&self) -> Result<(), WorkbenchStoreError> {
        let result: String = self
            .connection
            .query_row("PRAGMA quick_check(1)", [], |row| row.get(0))
            .map_err(|_| error("cannot verify workbench database integrity"))?;
        if result != "ok" {
            return Err(error("workbench database integrity check failed"));
        }
        verify_required_schema(&self.connection)?;
        verify_background_job_store(&self.connection)?;
        verify_background_scheduler_lease_store(&self.connection)?;
        verify_background_recovery_decision_store(&self.connection)?;
        verify_background_notification_store(&self.connection)?;
        verify_session_workspace_binding_store(self)
    }

    fn load_decision(
        &self,
        authority_id: &str,
        decision_id: &str,
        kind: GitWorkflowDecisionKind,
        requirement: &GitWorkflowAuthorizationRequirement,
    ) -> Result<GitWorkflowDecisionReference, WorkbenchStoreError> {
        validate_identifier(authority_id, "approval authority ID")?;
        validate_identifier(decision_id, "approval decision ID")?;
        let decision = query_decision(&self.connection, authority_id, decision_id)?;
        validate_stored_decision(&decision, kind, requirement, None)?;
        Ok(decision_reference(&decision))
    }
}

impl GitWorkflowAuthorizationAuthority for WorkbenchStore {
    fn consume_once(
        &mut self,
        requirement: &GitWorkflowAuthorizationRequirement,
        evidence: &GitWorkflowAuthorizationEvidence,
        observed_at_ms: u64,
    ) -> Result<(), GitWorkflowAuthorizationError> {
        self.consume_git_workflow_authorization(requirement, evidence, observed_at_ms)
            .map_err(|cause| GitWorkflowAuthorizationError {
                message: cause.message,
            })
    }
}

impl WorkbenchStore {
    fn consume_git_workflow_authorization(
        &mut self,
        requirement: &GitWorkflowAuthorizationRequirement,
        evidence: &GitWorkflowAuthorizationEvidence,
        observed_at_ms: u64,
    ) -> Result<(), WorkbenchStoreError> {
        validate_requirement_summary(requirement)?;
        self.ensure_session_writable(&requirement.session_id)?;
        validate_store_evidence(requirement, evidence, observed_at_ms)?;
        let transaction =
            self.begin_database_write("cannot start authorization consumption transaction")?;
        consume_decision_tx(
            &transaction,
            &evidence.permission,
            GitWorkflowDecisionKind::Permission,
            requirement,
            observed_at_ms,
        )?;
        if let Some(approval) = &evidence.explicit_approval {
            consume_decision_tx(
                &transaction,
                approval,
                GitWorkflowDecisionKind::ExplicitApproval,
                requirement,
                observed_at_ms,
            )?;
        }
        transaction
            .execute(
                "INSERT INTO authorization_consumptions (
                    authorization_id, requirement_sha256, requirement_bytes,
                    project_id, session_id, operation_id, action, consumed_at_ms
                 ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
                params![
                    evidence.authorization_id,
                    requirement.requirement_hash.sha256,
                    to_i64(requirement.requirement_hash.bytes, "requirement byte count")?,
                    requirement.project_id,
                    requirement.session_id,
                    requirement.operation_id,
                    action_name(requirement),
                    to_i64(observed_at_ms, "authorization consumption time")?,
                ],
            )
            .map_err(|_| error("authorization was already consumed"))?;
        let event_id = derived_event_id("approval-consumed", evidence.authorization_id.as_bytes());
        append_event_tx(
            &transaction,
            EventInput {
                session_id: &requirement.session_id,
                event_id: &event_id,
                timestamp_ms: observed_at_ms,
                correlation_id: &evidence.authorization_id,
                event_kind: "approval.consumed",
                project_id: Some(&requirement.project_id),
                operation_id: &requirement.operation_id,
                generation: requirement.generation,
                payload: json!({
                    "schema_version": "approval.consumed/0.1",
                    "authorization_id": evidence.authorization_id,
                    "requirement_hash": requirement.requirement_hash,
                    "action": action_name(requirement),
                    "risk_class": requirement.risk.class,
                    "permission_authority_id": evidence.permission.authority_id,
                    "permission_decision_id": evidence.permission.decision_id,
                    "approval_authority_id": evidence.explicit_approval.as_ref().map(|value| &value.authority_id),
                    "approval_decision_id": evidence.explicit_approval.as_ref().map(|value| &value.decision_id),
                }),
            },
        )?;
        transaction
            .commit()
            .map_err(|_| error("cannot commit authorization consumption"))
    }
}

fn validate_store_evidence(
    requirement: &GitWorkflowAuthorizationRequirement,
    evidence: &GitWorkflowAuthorizationEvidence,
    observed_at_ms: u64,
) -> Result<(), WorkbenchStoreError> {
    validate_identifier(&evidence.authorization_id, "authorization ID")?;
    if evidence.schema_version != "git-workflow-authorization-evidence/0.1"
        || evidence.requirement_hash != requirement.requirement_hash
    {
        return Err(error(
            "authorization evidence is not bound to the requirement",
        ));
    }
    validate_decision_reference(
        &evidence.permission,
        &requirement.requirement_hash,
        observed_at_ms,
    )?;
    match (
        requirement.risk.requires_explicit_approval,
        evidence.explicit_approval.as_ref(),
    ) {
        (true, Some(approval)) => {
            validate_decision_reference(approval, &requirement.requirement_hash, observed_at_ms)?;
            if approval.authority_id == evidence.permission.authority_id
                && approval.decision_id == evidence.permission.decision_id
            {
                return Err(error("permission and approval decisions must be distinct"));
            }
        }
        (true, None) => return Err(error("authorization requires explicit approval")),
        (false, Some(_)) => return Err(error("unexpected explicit approval evidence")),
        (false, None) => {}
    }
    Ok(())
}

fn validate_decision_reference(
    reference: &GitWorkflowDecisionReference,
    requirement_hash: &ContentHash,
    observed_at_ms: u64,
) -> Result<(), WorkbenchStoreError> {
    validate_identifier(&reference.authority_id, "authorization authority ID")?;
    validate_identifier(&reference.decision_id, "authorization decision ID")?;
    if reference.scope != "allow-once"
        || reference.scope_hash != *requirement_hash
        || reference.issued_at_ms > observed_at_ms
        || observed_at_ms > reference.expires_at_ms
        || reference
            .expires_at_ms
            .saturating_sub(reference.issued_at_ms)
            > MAX_AUTHORIZATION_LIFETIME_MS
    {
        return Err(error("authorization decision is invalid or expired"));
    }
    Ok(())
}

fn validate_background_job_contract(
    request: &BackgroundJobRequest,
    state: &BackgroundJobState,
) -> Result<(), WorkbenchStoreError> {
    request
        .validate()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    state
        .validate(request)
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    Ok(())
}

fn background_job_json<T: Serialize>(
    value: &T,
    label: &str,
) -> Result<(String, ContentHash), WorkbenchStoreError> {
    let persisted =
        serde_json::to_value(value).map_err(|_| error(format!("cannot serialize {label}")))?;
    validate_persisted_payload(&persisted, label)?;
    let json =
        serde_json::to_string(&persisted).map_err(|_| error(format!("cannot encode {label}")))?;
    if json.is_empty() || json.len() > MAX_BACKGROUND_JOB_JSON_BYTES {
        return Err(coded_error(
            "background-job-json-limit",
            format!("{label} exceeds its durable size limit"),
        ));
    }
    let hash = ContentHash::for_bytes(json.as_bytes());
    Ok((json, hash))
}

fn stored_background_job(
    request: &BackgroundJobRequest,
    state: &BackgroundJobState,
) -> Result<StoredBackgroundJob, WorkbenchStoreError> {
    validate_background_job_contract(request, state)?;
    let (_, request_hash) = background_job_json(request, "background job request")?;
    let (_, state_hash) = background_job_json(state, "background job state")?;
    Ok(StoredBackgroundJob {
        schema_version: "stored-background-job/0.1".into(),
        request: request.clone(),
        state: state.clone(),
        request_hash,
        state_hash,
    })
}

fn next_record_json(record: &StoredBackgroundJob) -> Result<String, WorkbenchStoreError> {
    background_job_json(&record.state, "background job state").map(|(json, _)| json)
}

fn validate_background_job_relational_binding(
    connection: &Connection,
    request: &BackgroundJobRequest,
) -> Result<(), WorkbenchStoreError> {
    let binding = connection
        .query_row(
            "SELECT session.project_id, session.mode, session.status, root.access
             FROM sessions AS session
             JOIN project_roots AS root
               ON root.project_id = ?2 AND root.root_id = ?3
             WHERE session.session_id = ?1",
            params![request.session_id, request.project_id, request.root_id],
            |row| {
                Ok((
                    row.get::<_, Option<String>>(0)?,
                    row.get::<_, String>(1)?,
                    row.get::<_, String>(2)?,
                    row.get::<_, String>(3)?,
                ))
            },
        )
        .optional()
        .map_err(|_| error("cannot validate background job relational binding"))?;
    let Some((project_id, mode, status, access)) = binding else {
        return Err(coded_error(
            "background-job-binding-missing",
            "background job session or project root does not exist",
        ));
    };
    if project_id.as_deref() != Some(request.project_id.as_str())
        || mode != "work"
        || status != "active"
        || !matches!(access.as_str(), "read" | "write")
    {
        return Err(coded_error(
            "background-job-binding-invalid",
            "background job is not bound to an active Work session and root",
        ));
    }
    let deletion_state = connection
        .query_row(
            "SELECT deletion.state
             FROM session_deletion_members AS member
             JOIN session_deletions AS deletion
               ON deletion.deletion_id = member.deletion_id
             WHERE member.session_id = ?1",
            [&request.session_id],
            |row| row.get::<_, String>(0),
        )
        .optional()
        .map_err(|_| error("cannot validate background job deletion binding"))?;
    if deletion_state.is_some() {
        return Err(coded_error(
            "background-job-session-unavailable",
            "background job session is pending deletion or purged",
        ));
    }
    Ok(())
}

fn insert_background_job_tx(
    transaction: &Transaction<'_>,
    record: &StoredBackgroundJob,
) -> Result<(), WorkbenchStoreError> {
    let (request_json, request_hash) =
        background_job_json(&record.request, "background job request")?;
    let (state_json, state_hash) = background_job_json(&record.state, "background job state")?;
    if request_hash != record.request_hash || state_hash != record.state_hash {
        return Err(error(
            "background job durable hash changed before insertion",
        ));
    }
    transaction
        .execute(
            "INSERT INTO background_jobs (
                job_id, session_id, project_id, root_id, request_identity,
                idempotency_identity,
                request_json, request_sha256, request_bytes,
                state_identity, state_json, state_sha256, state_bytes,
                status, cancellation_state, generation, attempt_count,
                next_eligible_at_ms, created_at_ms, updated_at_ms
             ) VALUES (
                ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10,
                ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20
             )",
            params![
                record.request.job_id,
                record.request.session_id,
                record.request.project_id,
                record.request.root_id,
                record
                    .request
                    .identity()
                    .map_err(|cause| { coded_error(cause.code, cause.message) })?,
                record.request.idempotency_identity,
                request_json,
                record.request_hash.sha256,
                to_i64(record.request_hash.bytes, "background job request bytes")?,
                record
                    .state
                    .identity(&record.request)
                    .map_err(|cause| { coded_error(cause.code, cause.message) })?,
                state_json,
                record.state_hash.sha256,
                to_i64(record.state_hash.bytes, "background job state bytes")?,
                record.state.status.as_str(),
                record.state.cancellation.as_str(),
                to_i64(record.state.generation, "background job generation")?,
                to_i64(
                    record.state.attempts.len() as u64,
                    "background job attempt count"
                )?,
                to_i64(
                    record.state.next_eligible_at_ms,
                    "background job eligibility time"
                )?,
                to_i64(record.state.created_at_ms, "background job creation time")?,
                to_i64(record.state.updated_at_ms, "background job update time")?,
            ],
        )
        .map_err(|_| error("cannot insert durable background job"))?;
    Ok(())
}

fn query_background_job_optional(
    connection: &Connection,
    job_id: &str,
) -> Result<Option<StoredBackgroundJob>, WorkbenchStoreError> {
    validate_identifier(job_id, "background job ID")?;
    let row = connection
        .query_row(
            "SELECT session_id, project_id, root_id, request_identity,
                    idempotency_identity,
                    request_json, request_sha256, request_bytes,
                    state_identity, state_json, state_sha256, state_bytes,
                    status, cancellation_state, generation, attempt_count,
                    next_eligible_at_ms, created_at_ms, updated_at_ms
             FROM background_jobs WHERE job_id = ?1",
            [job_id],
            |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    row.get::<_, String>(1)?,
                    row.get::<_, String>(2)?,
                    row.get::<_, String>(3)?,
                    row.get::<_, String>(4)?,
                    row.get::<_, String>(5)?,
                    row.get::<_, String>(6)?,
                    row.get::<_, i64>(7)?,
                    row.get::<_, String>(8)?,
                    row.get::<_, String>(9)?,
                    row.get::<_, String>(10)?,
                    row.get::<_, i64>(11)?,
                    row.get::<_, String>(12)?,
                    row.get::<_, String>(13)?,
                    row.get::<_, i64>(14)?,
                    row.get::<_, i64>(15)?,
                    row.get::<_, i64>(16)?,
                    row.get::<_, i64>(17)?,
                    row.get::<_, i64>(18)?,
                ))
            },
        )
        .optional()
        .map_err(|_| error("cannot read durable background job"))?;
    let Some((
        session_id,
        project_id,
        root_id,
        request_identity,
        idempotency_identity,
        request_json,
        request_sha256,
        request_bytes,
        state_identity,
        state_json,
        state_sha256,
        state_bytes,
        status,
        cancellation_state,
        generation,
        attempt_count,
        next_eligible_at_ms,
        created_at_ms,
        updated_at_ms,
    )) = row
    else {
        return Ok(None);
    };
    if request_json.is_empty()
        || request_json.len() > MAX_BACKGROUND_JOB_JSON_BYTES
        || state_json.is_empty()
        || state_json.len() > MAX_BACKGROUND_JOB_JSON_BYTES
    {
        return Err(coded_error(
            "background-job-durable-json-invalid",
            "durable background job JSON is outside its bounds",
        ));
    }
    let request: BackgroundJobRequest = serde_json::from_str(&request_json)
        .map_err(|_| error("durable background job request is invalid"))?;
    let state: BackgroundJobState = serde_json::from_str(&state_json)
        .map_err(|_| error("durable background job state is invalid"))?;
    validate_background_job_contract(&request, &state)?;
    let (canonical_request_json, canonical_request_hash) =
        background_job_json(&request, "background job request")?;
    let (canonical_state_json, canonical_state_hash) =
        background_job_json(&state, "background job state")?;
    let stored_request_hash = ContentHash {
        sha256: request_sha256,
        bytes: to_u64(request_bytes, "background job request bytes")?,
    };
    let stored_state_hash = ContentHash {
        sha256: state_sha256,
        bytes: to_u64(state_bytes, "background job state bytes")?,
    };
    let identity_matches = request
        .identity()
        .map_err(|cause| coded_error(cause.code, cause.message))?
        == request_identity
        && state
            .identity(&request)
            .map_err(|cause| coded_error(cause.code, cause.message))?
            == state_identity;
    if request.job_id != job_id
        || request.session_id != session_id
        || request.project_id != project_id
        || request.root_id != root_id
        || request.idempotency_identity != idempotency_identity
        || canonical_request_json != request_json
        || canonical_state_json != state_json
        || canonical_request_hash != stored_request_hash
        || canonical_state_hash != stored_state_hash
        || !identity_matches
        || status != state.status.as_str()
        || cancellation_state != state.cancellation.as_str()
        || to_u64(generation, "background job generation")? != state.generation
        || to_u64(attempt_count, "background job attempt count")? != state.attempts.len() as u64
        || to_u64(next_eligible_at_ms, "background job eligibility time")?
            != state.next_eligible_at_ms
        || to_u64(created_at_ms, "background job creation time")? != state.created_at_ms
        || to_u64(updated_at_ms, "background job update time")? != state.updated_at_ms
    {
        return Err(coded_error(
            "background-job-durable-integrity-invalid",
            "durable background job metadata failed integrity validation",
        ));
    }
    Ok(Some(StoredBackgroundJob {
        schema_version: "stored-background-job/0.1".into(),
        request,
        state,
        request_hash: stored_request_hash,
        state_hash: stored_state_hash,
    }))
}

fn query_background_job_by_idempotency_optional(
    connection: &Connection,
    session_id: &str,
    idempotency_identity: &str,
) -> Result<Option<StoredBackgroundJob>, WorkbenchStoreError> {
    validate_identifier(session_id, "background job idempotency session ID")?;
    validate_identity_text(idempotency_identity, "background job idempotency identity")?;
    let job_id = connection
        .query_row(
            "SELECT job_id FROM background_jobs
             WHERE session_id = ?1 AND idempotency_identity = ?2",
            params![session_id, idempotency_identity],
            |row| row.get::<_, String>(0),
        )
        .optional()
        .map_err(|_| error("cannot read background job idempotency binding"))?;
    job_id
        .map(|job_id| query_background_job_optional(connection, &job_id))
        .transpose()
        .map(Option::flatten)
}

fn verify_background_job_store(connection: &Connection) -> Result<(), WorkbenchStoreError> {
    let count = query_global_count(
        connection,
        "SELECT COUNT(*) FROM background_jobs",
        "cannot count background jobs during integrity verification",
    )?;
    if count > MAX_BACKGROUND_JOBS as u64 {
        return Err(coded_error(
            "background-job-store-limit",
            "durable background job count exceeds its startup limit",
        ));
    }
    let mut statement = connection
        .prepare("SELECT job_id FROM background_jobs ORDER BY job_id LIMIT ?1")
        .map_err(|_| error("cannot prepare background job integrity verification"))?;
    let job_ids = statement
        .query_map([MAX_BACKGROUND_JOBS as i64 + 1], |row| {
            row.get::<_, String>(0)
        })
        .map_err(|_| error("cannot read background job integrity verification"))?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| error("background job integrity row is invalid"))?;
    drop(statement);
    if job_ids.len() > MAX_BACKGROUND_JOBS {
        return Err(coded_error(
            "background-job-store-limit",
            "durable background job count exceeds its startup limit",
        ));
    }
    for job_id in job_ids {
        query_background_job_optional(connection, &job_id)?.ok_or_else(|| {
            coded_error(
                "background-job-integrity-race",
                "durable background job disappeared during verification",
            )
        })?;
    }
    Ok(())
}

fn validate_background_scheduler_lease_contract(
    request: &BackgroundJobRequest,
    lease: &BackgroundSchedulerLease,
) -> Result<(), WorkbenchStoreError> {
    lease
        .validate(request)
        .map_err(|cause| coded_error(cause.code, cause.message))
}

fn stored_background_scheduler_lease(
    lease: &BackgroundSchedulerLease,
) -> Result<StoredBackgroundSchedulerLease, WorkbenchStoreError> {
    let (lease_json, lease_hash) = background_scheduler_lease_json(lease)?;
    let _ = lease_json;
    Ok(StoredBackgroundSchedulerLease {
        schema_version: "stored-background-scheduler-lease/0.1".into(),
        lease: lease.clone(),
        lease_hash,
    })
}

fn background_scheduler_lease_json(
    lease: &BackgroundSchedulerLease,
) -> Result<(String, ContentHash), WorkbenchStoreError> {
    let json = serde_json::to_string(lease)
        .map_err(|_| error("cannot serialize background scheduler lease"))?;
    if json.is_empty() || json.len() > MAX_BACKGROUND_LEASE_JSON_BYTES {
        return Err(coded_error(
            "background-lease-json-limit",
            "background scheduler lease JSON exceeds its durable bound",
        ));
    }
    validate_persisted_text(&json, "background scheduler lease JSON")?;
    let hash = ContentHash::for_bytes(json.as_bytes());
    Ok((json, hash))
}

fn insert_background_scheduler_lease_tx(
    transaction: &Transaction<'_>,
    record: &StoredBackgroundSchedulerLease,
) -> Result<(), WorkbenchStoreError> {
    let lease = &record.lease;
    let (lease_json, lease_hash) = background_scheduler_lease_json(lease)?;
    if lease_hash != record.lease_hash {
        return Err(error(
            "background scheduler lease hash changed before insertion",
        ));
    }
    transaction
        .execute(
            "INSERT INTO background_job_leases (
                job_id, session_id, project_id, root_id, request_identity,
                state_identity, job_generation, owner_identity, lease_generation,
                status, acquired_at_ms, renewed_at_ms, expires_at_ms, updated_at_ms,
                process_registration_identity, process_identity,
                released_at_ms, release_reason, lease_identity,
                lease_json, lease_sha256, lease_bytes,
                dispatch_authority, automatic_takeover
             ) VALUES (
                ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12,
                ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22, 0, 0
             )",
            params![
                lease.job_id,
                lease.session_id,
                lease.project_id,
                lease.root_id,
                lease.request_identity,
                lease.state_identity,
                to_i64(lease.job_generation, "background lease job generation")?,
                lease.owner_identity,
                to_i64(lease.lease_generation, "background lease generation")?,
                lease.status.as_str(),
                to_i64(lease.acquired_at_ms, "background lease acquisition time")?,
                to_i64(lease.renewed_at_ms, "background lease renewal time")?,
                to_i64(lease.expires_at_ms, "background lease expiry time")?,
                to_i64(lease.updated_at_ms, "background lease update time")?,
                lease.process_registration_identity,
                lease.process_identity,
                lease
                    .released_at_ms
                    .map(|value| to_i64(value, "background lease release time"))
                    .transpose()?,
                lease.release_reason.map(|reason| reason.as_str()),
                lease.lease_identity,
                lease_json,
                lease_hash.sha256,
                to_i64(lease_hash.bytes, "background lease bytes")?,
            ],
        )
        .map_err(|_| error("cannot insert durable background scheduler lease"))?;
    Ok(())
}

fn update_background_scheduler_lease_tx(
    transaction: &Transaction<'_>,
    previous: &StoredBackgroundSchedulerLease,
    next: &StoredBackgroundSchedulerLease,
) -> Result<(), WorkbenchStoreError> {
    let lease = &next.lease;
    let (lease_json, lease_hash) = background_scheduler_lease_json(lease)?;
    if lease_hash != next.lease_hash {
        return Err(error(
            "background scheduler lease hash changed before update",
        ));
    }
    let changed = transaction
        .execute(
            "UPDATE background_job_leases SET
                session_id = ?2, project_id = ?3, root_id = ?4,
                request_identity = ?5, state_identity = ?6, job_generation = ?7,
                owner_identity = ?8, lease_generation = ?9, status = ?10,
                acquired_at_ms = ?11, renewed_at_ms = ?12, expires_at_ms = ?13,
                updated_at_ms = ?14, process_registration_identity = ?15,
                process_identity = ?16, released_at_ms = ?17, release_reason = ?18,
                lease_identity = ?19, lease_json = ?20, lease_sha256 = ?21,
                lease_bytes = ?22, dispatch_authority = 0, automatic_takeover = 0
             WHERE job_id = ?1 AND lease_generation = ?23 AND lease_sha256 = ?24",
            params![
                lease.job_id,
                lease.session_id,
                lease.project_id,
                lease.root_id,
                lease.request_identity,
                lease.state_identity,
                to_i64(lease.job_generation, "background lease job generation")?,
                lease.owner_identity,
                to_i64(lease.lease_generation, "background lease generation")?,
                lease.status.as_str(),
                to_i64(lease.acquired_at_ms, "background lease acquisition time")?,
                to_i64(lease.renewed_at_ms, "background lease renewal time")?,
                to_i64(lease.expires_at_ms, "background lease expiry time")?,
                to_i64(lease.updated_at_ms, "background lease update time")?,
                lease.process_registration_identity,
                lease.process_identity,
                lease
                    .released_at_ms
                    .map(|value| to_i64(value, "background lease release time"))
                    .transpose()?,
                lease.release_reason.map(|reason| reason.as_str()),
                lease.lease_identity,
                lease_json,
                lease_hash.sha256,
                to_i64(lease_hash.bytes, "background lease bytes")?,
                to_i64(
                    previous.lease.lease_generation,
                    "previous background lease generation"
                )?,
                previous.lease_hash.sha256,
            ],
        )
        .map_err(|_| error("cannot update durable background scheduler lease"))?;
    if changed != 1 {
        return Err(coded_error(
            "background-lease-stale",
            "durable background scheduler lease changed during update",
        ));
    }
    Ok(())
}

fn query_background_scheduler_lease_optional(
    connection: &Connection,
    job_id: &str,
) -> Result<Option<StoredBackgroundSchedulerLease>, WorkbenchStoreError> {
    validate_identifier(job_id, "background lease job ID")?;
    let row = connection
        .query_row(
            "SELECT session_id, project_id, root_id, request_identity,
                    state_identity, job_generation, owner_identity, lease_generation,
                    status, acquired_at_ms, renewed_at_ms, expires_at_ms, updated_at_ms,
                    process_registration_identity, process_identity,
                    released_at_ms, release_reason, lease_identity,
                    lease_json, lease_sha256, lease_bytes,
                    dispatch_authority, automatic_takeover
             FROM background_job_leases WHERE job_id = ?1",
            [job_id],
            |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    row.get::<_, String>(1)?,
                    row.get::<_, String>(2)?,
                    row.get::<_, String>(3)?,
                    row.get::<_, String>(4)?,
                    row.get::<_, i64>(5)?,
                    row.get::<_, String>(6)?,
                    row.get::<_, i64>(7)?,
                    row.get::<_, String>(8)?,
                    row.get::<_, i64>(9)?,
                    row.get::<_, i64>(10)?,
                    row.get::<_, i64>(11)?,
                    row.get::<_, i64>(12)?,
                    row.get::<_, Option<String>>(13)?,
                    row.get::<_, Option<String>>(14)?,
                    row.get::<_, Option<i64>>(15)?,
                    row.get::<_, Option<String>>(16)?,
                    row.get::<_, String>(17)?,
                    row.get::<_, String>(18)?,
                    row.get::<_, String>(19)?,
                    row.get::<_, i64>(20)?,
                    row.get::<_, i64>(21)?,
                    row.get::<_, i64>(22)?,
                ))
            },
        )
        .optional()
        .map_err(|_| error("cannot read durable background scheduler lease"))?;
    let Some((
        session_id,
        project_id,
        root_id,
        request_identity,
        state_identity,
        job_generation,
        owner_identity,
        lease_generation,
        status,
        acquired_at_ms,
        renewed_at_ms,
        expires_at_ms,
        updated_at_ms,
        process_registration_identity,
        process_identity,
        released_at_ms,
        release_reason,
        lease_identity,
        lease_json,
        lease_sha256,
        lease_bytes,
        dispatch_authority,
        automatic_takeover,
    )) = row
    else {
        return Ok(None);
    };
    if lease_json.is_empty() || lease_json.len() > MAX_BACKGROUND_LEASE_JSON_BYTES {
        return Err(coded_error(
            "background-lease-durable-json-invalid",
            "durable background scheduler lease JSON is outside its bounds",
        ));
    }
    let lease: BackgroundSchedulerLease = serde_json::from_str(&lease_json)
        .map_err(|_| error("durable background scheduler lease JSON is invalid"))?;
    let job = query_background_job_optional(connection, job_id)?.ok_or_else(|| {
        coded_error(
            "background-lease-job-missing",
            "durable background scheduler lease job is missing",
        )
    })?;
    validate_background_scheduler_lease_contract(&job.request, &lease)?;
    let (canonical_json, canonical_hash) = background_scheduler_lease_json(&lease)?;
    let stored_hash = ContentHash {
        sha256: lease_sha256,
        bytes: to_u64(lease_bytes, "background lease bytes")?,
    };
    if lease.job_id != job_id
        || lease.session_id != session_id
        || lease.project_id != project_id
        || lease.root_id != root_id
        || lease.request_identity != request_identity
        || lease.state_identity != state_identity
        || lease.job_generation != to_u64(job_generation, "background lease job generation")?
        || lease.owner_identity != owner_identity
        || lease.lease_generation != to_u64(lease_generation, "background lease generation")?
        || lease.status.as_str() != status
        || lease.acquired_at_ms != to_u64(acquired_at_ms, "background lease acquisition time")?
        || lease.renewed_at_ms != to_u64(renewed_at_ms, "background lease renewal time")?
        || lease.expires_at_ms != to_u64(expires_at_ms, "background lease expiry time")?
        || lease.updated_at_ms != to_u64(updated_at_ms, "background lease update time")?
        || lease.process_registration_identity != process_registration_identity
        || lease.process_identity != process_identity
        || lease.released_at_ms
            != released_at_ms
                .map(|value| to_u64(value, "background lease release time"))
                .transpose()?
        || lease.release_reason.map(|reason| reason.as_str()) != release_reason.as_deref()
        || lease.lease_identity != lease_identity
        || canonical_json != lease_json
        || canonical_hash != stored_hash
        || dispatch_authority != 0
        || automatic_takeover != 0
    {
        return Err(coded_error(
            "background-lease-durable-integrity-invalid",
            "durable background scheduler lease metadata failed integrity validation",
        ));
    }
    Ok(Some(StoredBackgroundSchedulerLease {
        schema_version: "stored-background-scheduler-lease/0.1".into(),
        lease,
        lease_hash: stored_hash,
    }))
}

fn verify_background_scheduler_lease_store(
    connection: &Connection,
) -> Result<(), WorkbenchStoreError> {
    let count = query_global_count(
        connection,
        "SELECT COUNT(*) FROM background_job_leases",
        "cannot count background scheduler leases during integrity verification",
    )?;
    if count > MAX_BACKGROUND_JOBS as u64 {
        return Err(coded_error(
            "background-lease-store-limit",
            "durable background scheduler lease count exceeds its startup limit",
        ));
    }
    let mut statement = connection
        .prepare("SELECT job_id FROM background_job_leases ORDER BY job_id LIMIT ?1")
        .map_err(|_| error("cannot prepare background scheduler lease verification"))?;
    let job_ids = statement
        .query_map([MAX_BACKGROUND_JOBS as i64 + 1], |row| {
            row.get::<_, String>(0)
        })
        .map_err(|_| error("cannot read background scheduler lease verification"))?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| error("background scheduler lease verification row is invalid"))?;
    drop(statement);
    if job_ids.len() > MAX_BACKGROUND_JOBS {
        return Err(coded_error(
            "background-lease-store-limit",
            "durable background scheduler lease count exceeds its startup limit",
        ));
    }
    for job_id in job_ids {
        query_background_scheduler_lease_optional(connection, &job_id)?.ok_or_else(|| {
            coded_error(
                "background-lease-integrity-race",
                "durable background scheduler lease disappeared during verification",
            )
        })?;
    }
    Ok(())
}

fn validate_current_background_recovery_evidence(
    connection: &Connection,
    decision: &BackgroundRecoveryDecision,
) -> Result<(), WorkbenchStoreError> {
    decision
        .validate()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    let job = query_background_job_optional(connection, &decision.job_id)?.ok_or_else(|| {
        coded_error(
            "background-recovery-job-missing",
            "background recovery decision job is missing",
        )
    })?;
    let request_identity = job
        .request
        .identity()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    let state_identity = job
        .state
        .identity(&job.request)
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    if job.request.session_id != decision.session_id
        || job.request.project_id != decision.project_id
        || job.request.root_id != decision.root_id
        || request_identity != decision.request_identity
        || state_identity != decision.state_identity
        || job.state.generation != decision.job_generation
        || job.state.status != decision.job_status
        || job.state.cancellation != decision.cancellation
        || (decision.next_eligible_at_ms.is_some()
            && decision.next_eligible_at_ms != Some(job.state.next_eligible_at_ms))
        || job.state.updated_at_ms > decision.observed_at_ms
    {
        return Err(coded_error(
            "background-recovery-evidence-stale",
            "background recovery decision no longer matches the durable job",
        ));
    }
    let lease = query_background_scheduler_lease_optional(connection, &decision.job_id)?;
    match (decision.lease_state, lease.as_ref()) {
        (SchedulerLeaseState::Missing, None) => {}
        (SchedulerLeaseState::Missing, Some(_)) | (_, None) => {
            return Err(coded_error(
                "background-recovery-lease-stale",
                "background recovery decision lease evidence is stale",
            ));
        }
        (_, Some(lease))
            if decision.lease_identity.as_deref() == Some(lease.lease.lease_identity.as_str())
                && decision.lease_generation == Some(lease.lease.lease_generation) => {}
        _ => {
            return Err(coded_error(
                "background-recovery-lease-stale",
                "background recovery decision lease evidence is stale",
            ));
        }
    }
    Ok(())
}

fn validate_historical_background_recovery_binding(
    connection: &Connection,
    decision: &BackgroundRecoveryDecision,
) -> Result<(), WorkbenchStoreError> {
    decision
        .validate()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    let job = query_background_job_optional(connection, &decision.job_id)?.ok_or_else(|| {
        coded_error(
            "background-recovery-job-missing",
            "background recovery decision job is missing",
        )
    })?;
    let request_identity = job
        .request
        .identity()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    if job.request.session_id != decision.session_id
        || job.request.project_id != decision.project_id
        || job.request.root_id != decision.root_id
        || request_identity != decision.request_identity
        || decision.observed_at_ms < job.state.created_at_ms
        || decision.recorded_at_ms < job.request.created_at_ms
    {
        return Err(coded_error(
            "background-recovery-history-binding-invalid",
            "background recovery decision history does not bind its durable job",
        ));
    }
    Ok(())
}

fn same_background_recovery_evidence(
    previous: &BackgroundRecoveryDecision,
    next: &BackgroundRecoveryDecision,
) -> bool {
    previous.job_id == next.job_id
        && previous.scheduler_snapshot_identity == next.scheduler_snapshot_identity
        && previous.scheduler_entry_identity == next.scheduler_entry_identity
        && previous.disposition == next.disposition
}

fn query_latest_background_recovery_decision(
    connection: &Connection,
    job_id: &str,
) -> Result<Option<StoredBackgroundRecoveryDecision>, WorkbenchStoreError> {
    validate_identifier(job_id, "background recovery job ID")?;
    let location = connection
        .query_row(
            "SELECT session_id, sequence FROM events
             WHERE event_kind = 'background-job.recovery-reviewed'
               AND operation_id = ?1
             ORDER BY sequence DESC LIMIT 1",
            [job_id],
            |row| Ok((row.get::<_, String>(0)?, row.get::<_, i64>(1)?)),
        )
        .optional()
        .map_err(|_| error("cannot inspect background recovery decision"))?;
    let Some((session_id, sequence)) = location else {
        return Ok(None);
    };
    let event = query_event_at_sequence(
        connection,
        &session_id,
        to_u64(sequence, "background recovery event sequence")?,
    )?;
    let stored = parse_background_recovery_decision_event(&event)?;
    validate_historical_background_recovery_binding(connection, &stored.decision)?;
    Ok(Some(stored))
}

fn parse_background_recovery_decision_event(
    event: &WorkbenchEvent,
) -> Result<StoredBackgroundRecoveryDecision, WorkbenchStoreError> {
    if event.event_kind != "background-job.recovery-reviewed"
        || event.payload.get("schema_version").and_then(Value::as_str)
            != Some("background-job.recovery-reviewed/0.1")
    {
        return Err(coded_error(
            "background-recovery-event-schema-invalid",
            "background recovery decision event schema is invalid",
        ));
    }
    let decision = event
        .payload
        .get("decision")
        .cloned()
        .and_then(|value| serde_json::from_value::<BackgroundRecoveryDecision>(value).ok())
        .ok_or_else(|| {
            coded_error(
                "background-recovery-event-decision-invalid",
                "background recovery decision event payload is invalid",
            )
        })?;
    decision
        .validate()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    let expected_event_id = derived_event_id(
        "background-job-recovery-reviewed",
        decision.decision_identity.as_bytes(),
    );
    if decision.schema_version != BACKGROUND_RECOVERY_DECISION_SCHEMA_VERSION
        || event.session_id != decision.session_id
        || event.project_id.as_deref() != Some(decision.project_id.as_str())
        || event.operation_id != decision.job_id
        || event.correlation_id != decision.decision_identity
        || event.generation != decision.scheduler_generation
        || event.timestamp_ms != decision.recorded_at_ms
        || event.event_id != expected_event_id
    {
        return Err(coded_error(
            "background-recovery-event-identity-invalid",
            "background recovery decision event identity is invalid",
        ));
    }
    Ok(StoredBackgroundRecoveryDecision {
        schema_version: "stored-background-recovery-decision/0.1".into(),
        decision,
        event_sequence: event.sequence,
    })
}

fn query_event_at_sequence(
    connection: &Connection,
    session_id: &str,
    sequence: u64,
) -> Result<WorkbenchEvent, WorkbenchStoreError> {
    validate_identifier(session_id, "event session ID")?;
    let row = connection
        .query_row(
            "SELECT event_id, timestamp_ms, correlation_id, event_kind,
                    project_id, operation_id, generation, payload_json,
                    payload_sha256, payload_bytes
             FROM events WHERE session_id = ?1 AND sequence = ?2",
            params![session_id, to_i64(sequence, "event sequence")?],
            |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    row.get::<_, i64>(1)?,
                    row.get::<_, String>(2)?,
                    row.get::<_, String>(3)?,
                    row.get::<_, Option<String>>(4)?,
                    row.get::<_, String>(5)?,
                    row.get::<_, i64>(6)?,
                    row.get::<_, String>(7)?,
                    row.get::<_, String>(8)?,
                    row.get::<_, i64>(9)?,
                ))
            },
        )
        .optional()
        .map_err(|_| error("cannot read event by sequence"))?
        .ok_or_else(|| error("event does not exist at sequence"))?;
    let (
        event_id,
        timestamp_ms,
        correlation_id,
        event_kind,
        project_id,
        operation_id,
        generation,
        payload_json,
        payload_sha256,
        payload_bytes,
    ) = row;
    let payload =
        serde_json::from_str(&payload_json).map_err(|_| error("event payload JSON is invalid"))?;
    validate_persisted_payload(&payload, "event payload")?;
    let payload_hash = ContentHash {
        sha256: payload_sha256,
        bytes: to_u64(payload_bytes, "event payload byte count")?,
    };
    if ContentHash::for_bytes(payload_json.as_bytes()) != payload_hash {
        return Err(error("event payload integrity check failed"));
    }
    Ok(WorkbenchEvent {
        schema_version: "workbench-event/0.1".into(),
        session_id: session_id.into(),
        sequence,
        event_id,
        timestamp_ms: to_u64(timestamp_ms, "event timestamp")?,
        correlation_id,
        event_kind,
        project_id,
        operation_id,
        generation: to_u64(generation, "event generation")?,
        payload,
        payload_hash,
    })
}

fn verify_background_recovery_decision_store(
    connection: &Connection,
) -> Result<(), WorkbenchStoreError> {
    let count = query_global_count(
        connection,
        "SELECT COUNT(*) FROM events
         WHERE event_kind = 'background-job.recovery-reviewed'",
        "cannot count background recovery decisions during integrity verification",
    )?;
    if count > MAX_BACKGROUND_RECOVERY_DECISIONS as u64 {
        return Err(coded_error(
            "background-recovery-decision-limit",
            "background recovery decision journal exceeds its startup bound",
        ));
    }
    let mut statement = connection
        .prepare(
            "SELECT session_id, sequence FROM events
             WHERE event_kind = 'background-job.recovery-reviewed'
             ORDER BY session_id, sequence LIMIT ?1",
        )
        .map_err(|_| error("cannot prepare background recovery decision verification"))?;
    let rows = statement
        .query_map([MAX_BACKGROUND_RECOVERY_DECISIONS as i64 + 1], |row| {
            Ok((row.get::<_, String>(0)?, row.get::<_, i64>(1)?))
        })
        .map_err(|_| error("cannot read background recovery decision verification"))?;
    let mut locations = Vec::new();
    for row in rows {
        let (session_id, sequence) =
            row.map_err(|_| error("background recovery decision location is invalid"))?;
        locations.push((
            session_id,
            to_u64(sequence, "background recovery event sequence")?,
        ));
    }
    drop(statement);
    if locations.len() > MAX_BACKGROUND_RECOVERY_DECISIONS {
        return Err(coded_error(
            "background-recovery-decision-limit",
            "background recovery decision verification exceeds its bound",
        ));
    }
    for (session_id, sequence) in locations {
        let event = query_event_at_sequence(connection, &session_id, sequence)?;
        let stored = parse_background_recovery_decision_event(&event)?;
        validate_historical_background_recovery_binding(connection, &stored.decision)?;
    }
    Ok(())
}

fn background_notification_json(
    intent: &BackgroundNotificationIntent,
) -> Result<(String, ContentHash), WorkbenchStoreError> {
    intent
        .validate()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    let json = serde_json::to_string(intent)
        .map_err(|_| error("cannot serialize background notification intent"))?;
    if json.is_empty() || json.len() > MAX_BACKGROUND_NOTIFICATION_JSON_BYTES {
        return Err(coded_error(
            "background-notification-json-limit",
            "background notification intent JSON exceeds its durable bound",
        ));
    }
    validate_persisted_text(&json, "background notification intent JSON")?;
    let hash = ContentHash::for_bytes(json.as_bytes());
    Ok((json, hash))
}

fn same_background_notification_evidence(
    previous: &BackgroundNotificationIntent,
    next: &BackgroundNotificationIntent,
) -> bool {
    previous.deduplication_identity == next.deduplication_identity
        && previous.kind == next.kind
        && previous.job_id == next.job_id
        && previous.session_id == next.session_id
        && previous.project_id == next.project_id
        && previous.root_id == next.root_id
        && previous.request_identity == next.request_identity
        && previous.state_identity == next.state_identity
        && previous.job_generation == next.job_generation
        && previous.job_status == next.job_status
        && previous.evidence_identity == next.evidence_identity
        && previous.budget_snapshot_identity == next.budget_snapshot_identity
        && previous.exhausted_dimensions == next.exhausted_dimensions
}

fn validate_current_background_notification_binding(
    connection: &Connection,
    intent: &BackgroundNotificationIntent,
) -> Result<(), WorkbenchStoreError> {
    intent
        .validate()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    let job = query_background_job_optional(connection, &intent.job_id)?.ok_or_else(|| {
        coded_error(
            "background-notification-job-missing",
            "background notification job is missing",
        )
    })?;
    let request_identity = job
        .request
        .identity()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    let state_identity = job
        .state
        .identity(&job.request)
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    if job.request.session_id != intent.session_id
        || job.request.project_id != intent.project_id
        || job.request.root_id != intent.root_id
        || request_identity != intent.request_identity
        || state_identity != intent.state_identity
        || job.state.generation != intent.job_generation
        || job.state.status != intent.job_status
        || intent.created_at_ms < job.state.updated_at_ms
    {
        return Err(coded_error(
            "background-notification-job-state-stale",
            "background notification does not bind the current durable job state",
        ));
    }
    validate_background_notification_lifecycle_binding(connection, intent)
}

fn validate_historical_background_notification_binding(
    connection: &Connection,
    intent: &BackgroundNotificationIntent,
) -> Result<(), WorkbenchStoreError> {
    intent
        .validate()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    let job = query_background_job_optional(connection, &intent.job_id)?.ok_or_else(|| {
        coded_error(
            "background-notification-job-missing",
            "background notification job is missing",
        )
    })?;
    let request_identity = job
        .request
        .identity()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    if job.request.session_id != intent.session_id
        || job.request.project_id != intent.project_id
        || job.request.root_id != intent.root_id
        || request_identity != intent.request_identity
        || intent.created_at_ms < job.request.created_at_ms
    {
        return Err(coded_error(
            "background-notification-history-binding-invalid",
            "background notification history does not bind its durable job",
        ));
    }
    validate_background_notification_lifecycle_binding(connection, intent)
}

fn validate_background_notification_lifecycle_binding(
    connection: &Connection,
    intent: &BackgroundNotificationIntent,
) -> Result<(), WorkbenchStoreError> {
    let mut statement = connection
        .prepare(
            "SELECT sequence FROM events
             WHERE session_id = ?1 AND operation_id = ?2 AND generation = ?3
               AND event_kind GLOB 'background-job.*'
             ORDER BY sequence ASC LIMIT 65",
        )
        .map_err(|_| error("cannot prepare background notification lifecycle binding"))?;
    let sequences = statement
        .query_map(
            params![
                intent.session_id,
                intent.job_id,
                to_i64(
                    intent.job_generation,
                    "background notification job generation"
                )?,
            ],
            |row| row.get::<_, i64>(0),
        )
        .map_err(|_| error("cannot read background notification lifecycle binding"))?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| error("background notification lifecycle row is invalid"))?;
    drop(statement);
    if sequences.len() > 64 {
        return Err(coded_error(
            "background-notification-lifecycle-limit",
            "background notification lifecycle evidence exceeds its bound",
        ));
    }
    for sequence in sequences {
        let event = query_event_at_sequence(
            connection,
            &intent.session_id,
            to_u64(sequence, "background notification lifecycle sequence")?,
        )?;
        if event.payload.get("schema_version").and_then(Value::as_str)
            != Some("background-job-lifecycle-event/0.1")
        {
            continue;
        }
        let job_id = event.payload.get("job_id").and_then(Value::as_str);
        let request_identity = event
            .payload
            .get("request_identity")
            .and_then(Value::as_str);
        let state_identity = event.payload.get("state_identity").and_then(Value::as_str);
        let status = event.payload.get("status").and_then(Value::as_str);
        let generation = event.payload.get("generation").and_then(Value::as_u64);
        if job_id.is_none()
            || request_identity.is_none()
            || state_identity.is_none()
            || status.is_none()
            || generation.is_none()
        {
            return Err(coded_error(
                "background-notification-lifecycle-event-invalid",
                "background notification lifecycle event is invalid",
            ));
        }
        if job_id == Some(intent.job_id.as_str())
            && request_identity == Some(intent.request_identity.as_str())
            && state_identity == Some(intent.state_identity.as_str())
            && status == Some(intent.job_status.as_str())
            && generation == Some(intent.job_generation)
        {
            return Ok(());
        }
    }
    Err(coded_error(
        "background-notification-lifecycle-evidence-missing",
        "background notification has no matching durable job lifecycle evidence",
    ))
}

fn insert_background_notification_tx(
    transaction: &Transaction<'_>,
    intent: &BackgroundNotificationIntent,
    intent_json: &str,
    intent_hash: &ContentHash,
    event_sequence: u64,
    recorded_at_ms: u64,
) -> Result<(), WorkbenchStoreError> {
    let (canonical_json, canonical_hash) = background_notification_json(intent)?;
    if canonical_json != intent_json || &canonical_hash != intent_hash {
        return Err(error(
            "background notification durable hash changed before insertion",
        ));
    }
    transaction
        .execute(
            "INSERT INTO background_notification_outbox (
                intent_identity, deduplication_identity, job_id, session_id,
                project_id, root_id, request_identity, state_identity,
                job_generation, kind, job_status, intent_json, intent_sha256,
                intent_bytes, event_sequence, delivery_state,
                delivery_attempt_count, content_included, delivery_available,
                delivery_attempted, platform_delivery_authority,
                created_at_ms, recorded_at_ms
             ) VALUES (
                ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11,
                ?12, ?13, ?14, ?15, 'recorded', 0, 0, 0, 0, 0, ?16, ?17
             )",
            params![
                intent.intent_identity,
                intent.deduplication_identity,
                intent.job_id,
                intent.session_id,
                intent.project_id,
                intent.root_id,
                intent.request_identity,
                intent.state_identity,
                to_i64(
                    intent.job_generation,
                    "background notification job generation"
                )?,
                intent.kind.as_str(),
                intent.job_status.as_str(),
                intent_json,
                intent_hash.sha256,
                to_i64(intent_hash.bytes, "background notification intent bytes")?,
                to_i64(event_sequence, "background notification event sequence")?,
                to_i64(
                    intent.created_at_ms,
                    "background notification creation time"
                )?,
                to_i64(recorded_at_ms, "background notification record time")?,
            ],
        )
        .map_err(|_| error("cannot insert durable background notification"))?;
    Ok(())
}

fn query_background_notification_by_dedup_optional(
    connection: &Connection,
    deduplication_identity: &str,
) -> Result<Option<StoredBackgroundNotification>, WorkbenchStoreError> {
    validate_identity_text(
        deduplication_identity,
        "background notification deduplication identity",
    )?;
    let intent_identity = connection
        .query_row(
            "SELECT intent_identity FROM background_notification_outbox
             WHERE deduplication_identity = ?1",
            [deduplication_identity],
            |row| row.get::<_, String>(0),
        )
        .optional()
        .map_err(|_| error("cannot read background notification deduplication binding"))?;
    intent_identity
        .map(|identity| query_background_notification_optional(connection, &identity))
        .transpose()
        .map(Option::flatten)
}

fn query_background_notification_optional(
    connection: &Connection,
    intent_identity: &str,
) -> Result<Option<StoredBackgroundNotification>, WorkbenchStoreError> {
    validate_background_notification_identity(intent_identity)?;
    let row = connection
        .query_row(
            "SELECT deduplication_identity, job_id, session_id, project_id, root_id,
                    request_identity, state_identity, job_generation, kind, job_status,
                    intent_json, intent_sha256, intent_bytes, event_sequence,
                    delivery_state, delivery_attempt_count, content_included,
                    delivery_available, delivery_attempted, platform_delivery_authority,
                    created_at_ms, recorded_at_ms
             FROM background_notification_outbox WHERE intent_identity = ?1",
            [intent_identity],
            |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    row.get::<_, String>(1)?,
                    row.get::<_, String>(2)?,
                    row.get::<_, String>(3)?,
                    row.get::<_, String>(4)?,
                    row.get::<_, String>(5)?,
                    row.get::<_, String>(6)?,
                    row.get::<_, i64>(7)?,
                    row.get::<_, String>(8)?,
                    row.get::<_, String>(9)?,
                    row.get::<_, String>(10)?,
                    row.get::<_, String>(11)?,
                    row.get::<_, i64>(12)?,
                    row.get::<_, i64>(13)?,
                    row.get::<_, String>(14)?,
                    row.get::<_, i64>(15)?,
                    row.get::<_, i64>(16)?,
                    row.get::<_, i64>(17)?,
                    row.get::<_, i64>(18)?,
                    row.get::<_, i64>(19)?,
                    row.get::<_, i64>(20)?,
                    row.get::<_, i64>(21)?,
                ))
            },
        )
        .optional()
        .map_err(|_| error("cannot read durable background notification"))?;
    let Some((
        deduplication_identity,
        job_id,
        session_id,
        project_id,
        root_id,
        request_identity,
        state_identity,
        job_generation,
        kind,
        job_status,
        intent_json,
        intent_sha256,
        intent_bytes,
        event_sequence,
        delivery_state,
        delivery_attempt_count,
        content_included,
        delivery_available,
        delivery_attempted,
        platform_delivery_authority,
        created_at_ms,
        recorded_at_ms,
    )) = row
    else {
        return Ok(None);
    };
    if intent_json.is_empty() || intent_json.len() > MAX_BACKGROUND_NOTIFICATION_JSON_BYTES {
        return Err(coded_error(
            "background-notification-durable-json-invalid",
            "durable background notification JSON is outside its bounds",
        ));
    }
    let intent: BackgroundNotificationIntent = serde_json::from_str(&intent_json)
        .map_err(|_| error("durable background notification intent is invalid"))?;
    intent
        .validate()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    let (canonical_json, canonical_hash) = background_notification_json(&intent)?;
    let stored_hash = ContentHash {
        sha256: intent_sha256,
        bytes: to_u64(intent_bytes, "background notification intent bytes")?,
    };
    let event_sequence = to_u64(event_sequence, "background notification event sequence")?;
    let recorded_at_ms = to_u64(recorded_at_ms, "background notification record time")?;
    if intent.intent_identity != intent_identity
        || intent.deduplication_identity != deduplication_identity
        || intent.job_id != job_id
        || intent.session_id != session_id
        || intent.project_id != project_id
        || intent.root_id != root_id
        || intent.request_identity != request_identity
        || intent.state_identity != state_identity
        || intent.job_generation
            != to_u64(job_generation, "background notification job generation")?
        || intent.kind.as_str() != kind
        || intent.job_status.as_str() != job_status
        || canonical_json != intent_json
        || canonical_hash != stored_hash
        || delivery_state != BackgroundNotificationDeliveryState::Recorded.as_str()
        || delivery_attempt_count != 0
        || content_included != 0
        || delivery_available != 0
        || delivery_attempted != 0
        || platform_delivery_authority != 0
        || intent.created_at_ms != to_u64(created_at_ms, "background notification creation time")?
        || recorded_at_ms < intent.created_at_ms
    {
        return Err(coded_error(
            "background-notification-durable-integrity-invalid",
            "durable background notification metadata failed integrity validation",
        ));
    }
    let stored = StoredBackgroundNotification {
        schema_version: "stored-background-notification/0.1".into(),
        intent,
        intent_hash: stored_hash,
        event_sequence,
        delivery_state: BackgroundNotificationDeliveryState::Recorded,
        delivery_attempt_count: 0,
        recorded_at_ms,
    };
    let event = query_event_at_sequence(connection, &session_id, event_sequence)?;
    let event_stored = parse_background_notification_event(&event)?;
    if event_stored != stored {
        return Err(coded_error(
            "background-notification-event-projection-mismatch",
            "background notification event and outbox projection do not match",
        ));
    }
    validate_historical_background_notification_binding(connection, &stored.intent)?;
    Ok(Some(stored))
}

fn parse_background_notification_event(
    event: &WorkbenchEvent,
) -> Result<StoredBackgroundNotification, WorkbenchStoreError> {
    if event.event_kind != "background-job.notification-recorded"
        || event.payload.get("schema_version").and_then(Value::as_str)
            != Some("background-job.notification-recorded/0.1")
    {
        return Err(coded_error(
            "background-notification-event-schema-invalid",
            "background notification event schema is invalid",
        ));
    }
    let intent = event
        .payload
        .get("intent")
        .cloned()
        .and_then(|value| serde_json::from_value::<BackgroundNotificationIntent>(value).ok())
        .ok_or_else(|| {
            coded_error(
                "background-notification-event-intent-invalid",
                "background notification event intent is invalid",
            )
        })?;
    intent
        .validate()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    let intent_hash = event
        .payload
        .get("intent_hash")
        .cloned()
        .and_then(|value| serde_json::from_value::<ContentHash>(value).ok())
        .ok_or_else(|| {
            coded_error(
                "background-notification-event-hash-invalid",
                "background notification event hash is invalid",
            )
        })?;
    let (_, canonical_hash) = background_notification_json(&intent)?;
    let recorded_at_ms = event
        .payload
        .get("recorded_at_ms")
        .and_then(Value::as_u64)
        .ok_or_else(|| {
            coded_error(
                "background-notification-event-time-invalid",
                "background notification event time is invalid",
            )
        })?;
    let expected_event_id = derived_event_id(
        "background-job-notification-recorded",
        intent.deduplication_identity.as_bytes(),
    );
    if intent.schema_version != BACKGROUND_NOTIFICATION_SCHEMA_VERSION
        || canonical_hash != intent_hash
        || event.session_id != intent.session_id
        || event.project_id.as_deref() != Some(intent.project_id.as_str())
        || event.operation_id != intent.job_id
        || event.correlation_id != intent.deduplication_identity
        || event.generation != intent.job_generation
        || event.timestamp_ms != recorded_at_ms
        || event.event_id != expected_event_id
        || recorded_at_ms < intent.created_at_ms
        || event.payload.get("delivery_state").and_then(Value::as_str) != Some("recorded")
        || event
            .payload
            .get("delivery_attempt_count")
            .and_then(Value::as_u64)
            != Some(0)
        || event
            .payload
            .get("content_included")
            .and_then(Value::as_bool)
            != Some(false)
        || event
            .payload
            .get("delivery_available")
            .and_then(Value::as_bool)
            != Some(false)
        || event
            .payload
            .get("delivery_attempted")
            .and_then(Value::as_bool)
            != Some(false)
        || event
            .payload
            .get("platform_delivery_authority")
            .and_then(Value::as_bool)
            != Some(false)
    {
        return Err(coded_error(
            "background-notification-event-identity-invalid",
            "background notification event identity is invalid",
        ));
    }
    Ok(StoredBackgroundNotification {
        schema_version: "stored-background-notification/0.1".into(),
        intent,
        intent_hash,
        event_sequence: event.sequence,
        delivery_state: BackgroundNotificationDeliveryState::Recorded,
        delivery_attempt_count: 0,
        recorded_at_ms,
    })
}

fn verify_background_notification_store(
    connection: &Connection,
) -> Result<(), WorkbenchStoreError> {
    let outbox_count = query_global_count(
        connection,
        "SELECT COUNT(*) FROM background_notification_outbox",
        "cannot count background notification outbox during integrity verification",
    )?;
    let event_count = query_global_count(
        connection,
        "SELECT COUNT(*) FROM events
         WHERE event_kind = 'background-job.notification-recorded'",
        "cannot count background notification events during integrity verification",
    )?;
    if outbox_count > MAX_BACKGROUND_NOTIFICATIONS as u64
        || event_count > MAX_BACKGROUND_NOTIFICATIONS as u64
        || outbox_count != event_count
    {
        return Err(coded_error(
            "background-notification-outbox-limit",
            "background notification outbox or event count is invalid",
        ));
    }
    let mut statement = connection
        .prepare(
            "SELECT intent_identity FROM background_notification_outbox
             ORDER BY intent_identity LIMIT ?1",
        )
        .map_err(|_| error("cannot prepare background notification verification"))?;
    let intent_ids = statement
        .query_map([MAX_BACKGROUND_NOTIFICATIONS as i64 + 1], |row| {
            row.get::<_, String>(0)
        })
        .map_err(|_| error("cannot read background notification verification"))?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| error("background notification verification row is invalid"))?;
    drop(statement);
    if intent_ids.len() > MAX_BACKGROUND_NOTIFICATIONS {
        return Err(coded_error(
            "background-notification-outbox-limit",
            "background notification verification exceeds its bound",
        ));
    }
    for intent_identity in intent_ids {
        query_background_notification_optional(connection, &intent_identity)?.ok_or_else(|| {
            coded_error(
                "background-notification-integrity-race",
                "background notification disappeared during verification",
            )
        })?;
    }
    Ok(())
}

fn validate_background_notification_identity(value: &str) -> Result<(), WorkbenchStoreError> {
    let valid = value
        .strip_prefix("background-job-notification-intent:sha256:")
        .is_some_and(|hex| {
            hex.len() == 64
                && hex
                    .bytes()
                    .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
        });
    if !valid {
        return Err(coded_error(
            "background-notification-identity-invalid",
            "background notification intent identity is invalid",
        ));
    }
    Ok(())
}

fn validate_background_notification_cursor(
    cursor: &BackgroundNotificationCursor,
) -> Result<(), WorkbenchStoreError> {
    if cursor.recorded_at_ms == 0 {
        return Err(coded_error(
            "background-notification-cursor-invalid",
            "background notification cursor time is invalid",
        ));
    }
    validate_background_notification_identity(&cursor.intent_identity)
}

fn background_scheduler_lease_event_kind(
    previous: &BackgroundSchedulerLease,
    next: &BackgroundSchedulerLease,
) -> Result<&'static str, WorkbenchStoreError> {
    let kind = match (previous.status, next.status) {
        (BackgroundSchedulerLeaseStatus::Active, BackgroundSchedulerLeaseStatus::Released)
            if previous.state_identity == next.state_identity
                && previous.job_generation == next.job_generation
                && previous.process_registration_identity == next.process_registration_identity
                && previous.process_identity == next.process_identity =>
        {
            "background-job.lease-released"
        }
        (BackgroundSchedulerLeaseStatus::Active, BackgroundSchedulerLeaseStatus::Expired)
            if previous.state_identity == next.state_identity
                && previous.job_generation == next.job_generation
                && previous.process_registration_identity == next.process_registration_identity
                && previous.process_identity == next.process_identity =>
        {
            "background-job.lease-expired"
        }
        (BackgroundSchedulerLeaseStatus::Active, BackgroundSchedulerLeaseStatus::Active)
            if previous.state_identity != next.state_identity
                || previous.job_generation != next.job_generation =>
        {
            if previous.job_generation.checked_add(1) != Some(next.job_generation) {
                return Err(coded_error(
                    "background-lease-state-generation-invalid",
                    "background scheduler lease state rebind skipped a job generation",
                ));
            }
            "background-job.lease-state-rebound"
        }
        (BackgroundSchedulerLeaseStatus::Active, BackgroundSchedulerLeaseStatus::Active)
            if previous.process_registration_identity.is_none()
                && previous.process_identity.is_none()
                && next.process_registration_identity.is_some()
                && next.process_identity.is_some() =>
        {
            "background-job.lease-process-bound"
        }
        (BackgroundSchedulerLeaseStatus::Active, BackgroundSchedulerLeaseStatus::Active)
            if next.renewed_at_ms > previous.renewed_at_ms
                && next.expires_at_ms > next.renewed_at_ms
                && previous.state_identity == next.state_identity
                && previous.job_generation == next.job_generation
                && previous.process_registration_identity == next.process_registration_identity
                && previous.process_identity == next.process_identity =>
        {
            "background-job.lease-renewed"
        }
        _ => {
            return Err(coded_error(
                "background-lease-transition-unrecognized",
                "background scheduler lease transition has no typed event",
            ));
        }
    };
    Ok(kind)
}

fn append_background_scheduler_lease_event_tx(
    transaction: &Transaction<'_>,
    previous: Option<(&BackgroundSchedulerLease, &str)>,
    record: &StoredBackgroundSchedulerLease,
) -> Result<WorkbenchEvent, WorkbenchStoreError> {
    let lease = &record.lease;
    let event_kind = previous
        .as_ref()
        .map(|(_, kind)| *kind)
        .unwrap_or("background-job.lease-acquired");
    let event_id = derived_event_id(
        "background-job-scheduler-lease",
        format!(
            "{}\0{}\0{}",
            lease.job_id, lease.lease_generation, lease.lease_identity
        )
        .as_bytes(),
    );
    append_event_tx(
        transaction,
        EventInput {
            session_id: &lease.session_id,
            event_id: &event_id,
            timestamp_ms: lease.updated_at_ms,
            correlation_id: &lease.job_id,
            event_kind,
            project_id: Some(&lease.project_id),
            operation_id: &lease.job_id,
            generation: lease.lease_generation,
            payload: json!({
                "schema_version": "background-job-scheduler-lease-event/0.1",
                "job_id": lease.job_id,
                "request_identity": lease.request_identity,
                "state_identity": lease.state_identity,
                "job_generation": lease.job_generation,
                "owner_identity": lease.owner_identity,
                "lease_generation": lease.lease_generation,
                "lease_identity": lease.lease_identity,
                "lease_hash": record.lease_hash,
                "previous_lease_identity": previous.as_ref().map(|(value, _)| &value.lease_identity),
                "status": lease.status.as_str(),
                "acquired_at_ms": lease.acquired_at_ms,
                "renewed_at_ms": lease.renewed_at_ms,
                "expires_at_ms": lease.expires_at_ms,
                "process_registration_identity": lease.process_registration_identity,
                "process_identity": lease.process_identity,
                "released_at_ms": lease.released_at_ms,
                "release_reason": lease.release_reason.map(|reason| reason.as_str()),
                "dispatch_authority": false,
                "automatic_takeover": false
            }),
        },
    )
}

fn background_job_event_kind(
    previous: &BackgroundJobState,
    next: &BackgroundJobState,
) -> Result<&'static str, WorkbenchStoreError> {
    let kind = match next.status {
        BackgroundJobStatus::Completed => "background-job.completed",
        BackgroundJobStatus::Failed => "background-job.failed",
        BackgroundJobStatus::Cancelled => "background-job.cancelled",
        BackgroundJobStatus::Interrupted => "background-job.interrupted",
        _ if next.cancellation == JobCancellationState::Requested
            && previous.cancellation != JobCancellationState::Requested =>
        {
            "background-job.cancellation-requested"
        }
        BackgroundJobStatus::Running
            if previous.status == BackgroundJobStatus::Cancelling
                && next.cancellation == JobCancellationState::Failed =>
        {
            "background-job.cancellation-rejected"
        }
        BackgroundJobStatus::Running if previous.status == BackgroundJobStatus::Queued => {
            "background-job.started"
        }
        BackgroundJobStatus::Running if previous.status == BackgroundJobStatus::WaitingApproval => {
            "background-job.approval-resumed"
        }
        BackgroundJobStatus::PauseRequested if previous.status == BackgroundJobStatus::Running => {
            "background-job.pause-requested"
        }
        BackgroundJobStatus::Paused
            if matches!(
                previous.status,
                BackgroundJobStatus::Queued | BackgroundJobStatus::PauseRequested
            ) =>
        {
            "background-job.paused"
        }
        BackgroundJobStatus::WaitingApproval if previous.status == BackgroundJobStatus::Running => {
            "background-job.approval-needed"
        }
        BackgroundJobStatus::Queued if previous.status == BackgroundJobStatus::Paused => {
            "background-job.resumed"
        }
        BackgroundJobStatus::Queued
            if matches!(
                previous.status,
                BackgroundJobStatus::Failed | BackgroundJobStatus::Interrupted
            ) =>
        {
            "background-job.retry-queued"
        }
        _ => {
            return Err(coded_error(
                "background-job-transition-unrecognized",
                "background job durable transition has no typed event",
            ));
        }
    };
    Ok(kind)
}

fn append_background_job_event_tx(
    transaction: &Transaction<'_>,
    previous: Option<(&BackgroundJobState, &str)>,
    record: &StoredBackgroundJob,
) -> Result<WorkbenchEvent, WorkbenchStoreError> {
    let event_kind = previous
        .as_ref()
        .map(|(_, event_kind)| *event_kind)
        .unwrap_or("background-job.queued");
    let request_identity = record
        .request
        .identity()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    let state_identity = record
        .state
        .identity(&record.request)
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    let event_id = derived_event_id(
        "background-job-lifecycle",
        format!(
            "{}\0{}\0{}",
            record.request.job_id, record.state.generation, state_identity
        )
        .as_bytes(),
    );
    append_event_tx(
        transaction,
        EventInput {
            session_id: &record.request.session_id,
            event_id: &event_id,
            timestamp_ms: record.state.updated_at_ms,
            correlation_id: &record.request.job_id,
            event_kind,
            project_id: Some(&record.request.project_id),
            operation_id: &record.request.job_id,
            generation: record.state.generation,
            payload: json!({
                "schema_version": "background-job-lifecycle-event/0.1",
                "job_id": record.request.job_id,
                "request_identity": request_identity,
                "request_hash": record.request_hash,
                "previous_status": previous.as_ref().map(|(state, _)| state.status.as_str()),
                "previous_cancellation_state": previous
                    .as_ref()
                    .map(|(state, _)| state.cancellation.as_str()),
                "state_identity": state_identity,
                "state_hash": record.state_hash,
                "status": record.state.status.as_str(),
                "cancellation_state": record.state.cancellation.as_str(),
                "generation": record.state.generation,
                "attempt_count": record.state.attempts.len(),
                "next_eligible_at_ms": record.state.next_eligible_at_ms,
                "automatic_retry": false,
                "automatic_approval": false
            }),
        },
    )
}

#[derive(Debug, Clone)]
struct PreparedTurnTraceRecord {
    trace: TurnTrace,
    trace_identity: String,
    state: String,
    terminal_at_ms: u64,
}

impl PreparedTurnTraceRecord {
    fn payload(&self, recorded_at_ms: u64) -> Value {
        json!({
            "schema_version": TURN_TRACE_RECORDED_SCHEMA_VERSION,
            "trace": self.trace,
            "trace_identity": self.trace_identity,
            "state": self.state,
            "recorded_at_ms": recorded_at_ms,
            "content_included": false,
            "execution_authority": false
        })
    }
}

fn trace_terminal_state(state: &str) -> Option<TraceTerminalState> {
    match state {
        "completed" => Some(TraceTerminalState::Completed),
        "failed" => Some(TraceTerminalState::Failed),
        "cancelled" => Some(TraceTerminalState::Cancelled),
        "interrupted" => Some(TraceTerminalState::Interrupted),
        _ => None,
    }
}

fn trace_terminal_event(trace: &TurnTrace) -> Option<(TraceTerminalState, u64)> {
    let event = trace.events.last()?;
    match &event.payload {
        TracePayload::Terminal { state, .. } => Some((*state, event.at_ms)),
        _ => None,
    }
}

fn turn_trace_matches_session_mode(trace: &TurnTrace, mode: StoredSessionMode) -> bool {
    if trace.schema_version == LEGACY_TURN_TRACE_SCHEMA_VERSION {
        return true;
    }
    if trace.schema_version != TURN_TRACE_SCHEMA_VERSION {
        return false;
    }
    let trace_mode = trace.events.iter().find_map(|event| match &event.payload {
        TracePayload::Intent { session_mode, .. } => Some(*session_mode),
        _ => None,
    });
    matches!(
        (trace_mode, mode),
        (Some(TraceSessionMode::Chat), StoredSessionMode::Chat)
            | (Some(TraceSessionMode::Work), StoredSessionMode::Work)
    )
}

fn prepare_turn_trace_record(
    session_id: &str,
    turn_id: &str,
    state: &str,
    trace: &TurnTrace,
) -> Result<PreparedTurnTraceRecord, WorkbenchStoreError> {
    validate_identifier(session_id, "turn trace session ID")?;
    validate_identifier(turn_id, "turn trace turn ID")?;
    let expected_state = trace_terminal_state(state).ok_or_else(|| {
        coded_error(
            "turn-trace-terminal-state-invalid",
            "turn trace terminal state is invalid",
        )
    })?;
    trace
        .validate_complete()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    if trace.binding.session_id != session_id || trace.binding.turn_id != turn_id {
        return Err(coded_error(
            "turn-trace-binding-mismatch",
            "turn trace is not bound to the completed turn",
        ));
    }
    let (terminal_state, terminal_at_ms) = trace_terminal_event(trace).ok_or_else(|| {
        coded_error(
            "turn-trace-terminal-missing",
            "turn trace has no terminal metadata event",
        )
    })?;
    if terminal_state != expected_state {
        return Err(coded_error(
            "turn-trace-terminal-state-mismatch",
            "turn trace terminal metadata does not match the stored turn state",
        ));
    }
    let trace_identity = trace
        .metadata_identity()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    let prepared = PreparedTurnTraceRecord {
        trace: trace.clone(),
        trace_identity,
        state: state.into(),
        terminal_at_ms,
    };
    let maximum_payload = serde_json::to_vec(&prepared.payload(u64::MAX))
        .map_err(|_| error("cannot serialize turn trace event payload"))?;
    if maximum_payload.len() > MAX_EVENT_BYTES {
        return Err(coded_error(
            "turn-trace-event-size-exceeded",
            "turn trace event exceeds the durable metadata bound",
        ));
    }
    validate_persisted_payload(&prepared.payload(u64::MAX), "turn trace event payload")?;
    Ok(prepared)
}

fn parse_turn_trace_event(event: &WorkbenchEvent) -> Result<StoredTurnTrace, WorkbenchStoreError> {
    let payload = event.payload.as_object().ok_or_else(|| {
        coded_error(
            "turn-trace-event-payload-invalid",
            "turn trace event payload is invalid",
        )
    })?;
    let allowed_keys = [
        "schema_version",
        "trace",
        "trace_identity",
        "state",
        "recorded_at_ms",
        "content_included",
        "execution_authority",
    ];
    if event.event_kind != "turn.trace.recorded"
        || payload.len() != allowed_keys.len()
        || payload
            .keys()
            .any(|key| !allowed_keys.contains(&key.as_str()))
        || payload.get("schema_version").and_then(Value::as_str)
            != Some(TURN_TRACE_RECORDED_SCHEMA_VERSION)
        || payload.get("content_included").and_then(Value::as_bool) != Some(false)
        || payload.get("execution_authority").and_then(Value::as_bool) != Some(false)
    {
        return Err(coded_error(
            "turn-trace-event-schema-invalid",
            "turn trace event schema or authority flags are invalid",
        ));
    }
    let trace_value = payload.get("trace").cloned().ok_or_else(|| {
        coded_error(
            "turn-trace-event-trace-invalid",
            "turn trace event metadata is invalid",
        )
    })?;
    let trace_schema_version = trace_value
        .get("schema_version")
        .and_then(Value::as_str)
        .ok_or_else(|| {
            coded_error(
                "turn-trace-event-trace-version-invalid",
                "turn trace event schema version is missing",
            )
        })?;
    if !matches!(
        trace_schema_version,
        LEGACY_TURN_TRACE_SCHEMA_VERSION | TURN_TRACE_SCHEMA_VERSION
    ) {
        return Err(coded_error(
            "turn-trace-event-trace-version-unsupported",
            "turn trace event schema version is unsupported",
        ));
    }
    let trace = serde_json::from_value::<TurnTrace>(trace_value.clone()).map_err(|_| {
        coded_error(
            "turn-trace-event-trace-invalid",
            "turn trace event metadata is invalid",
        )
    })?;
    let canonical_trace = serde_json::to_value(&trace)
        .map_err(|_| error("cannot canonicalize durable turn trace metadata"))?;
    if canonical_trace != trace_value {
        return Err(coded_error(
            "turn-trace-event-trace-noncanonical",
            "turn trace event contains unknown or noncanonical metadata",
        ));
    }
    trace
        .validate_complete()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    let trace_identity = payload
        .get("trace_identity")
        .and_then(Value::as_str)
        .ok_or_else(|| {
            coded_error(
                "turn-trace-event-identity-invalid",
                "turn trace event identity is missing",
            )
        })?
        .to_owned();
    let expected_identity = trace
        .metadata_identity()
        .map_err(|cause| coded_error(cause.code, cause.message))?;
    let state = payload
        .get("state")
        .and_then(Value::as_str)
        .ok_or_else(|| {
            coded_error(
                "turn-trace-event-state-invalid",
                "turn trace event terminal state is missing",
            )
        })?
        .to_owned();
    let recorded_at_ms = payload
        .get("recorded_at_ms")
        .and_then(Value::as_u64)
        .ok_or_else(|| {
            coded_error(
                "turn-trace-event-time-invalid",
                "turn trace event record time is invalid",
            )
        })?;
    let terminal = trace_terminal_state(&state).ok_or_else(|| {
        coded_error(
            "turn-trace-event-state-invalid",
            "turn trace event terminal state is invalid",
        )
    })?;
    let (trace_terminal, terminal_at_ms) = trace_terminal_event(&trace).ok_or_else(|| {
        coded_error(
            "turn-trace-event-terminal-missing",
            "turn trace event has no terminal metadata",
        )
    })?;
    let expected_event_id = derived_event_id(
        "turn-trace-recorded",
        format!("{}\0{trace_identity}", trace.binding.turn_id).as_bytes(),
    );
    if trace_identity != expected_identity
        || terminal != trace_terminal
        || terminal_at_ms != recorded_at_ms
        || event.session_id != trace.binding.session_id
        || event.project_id != trace.binding.project_id
        || event.correlation_id != trace.binding.turn_id
        || event.operation_id != trace.binding.turn_id
        || event.generation != 0
        || event.timestamp_ms != recorded_at_ms
        || event.event_id != expected_event_id
    {
        return Err(coded_error(
            "turn-trace-event-binding-invalid",
            "turn trace event binding or integrity metadata is invalid",
        ));
    }
    Ok(StoredTurnTrace {
        trace,
        trace_identity,
        state,
        event_sequence: event.sequence,
        recorded_at_ms,
    })
}

fn load_turn_trace_record(
    connection: &Connection,
    session_id: &str,
    turn_id: &str,
) -> Result<Option<StoredTurnTrace>, WorkbenchStoreError> {
    let mut statement = connection
        .prepare(
            "SELECT sequence FROM events
             WHERE session_id = ?1 AND event_kind = 'turn.trace.recorded'
               AND operation_id = ?2
             ORDER BY sequence ASC LIMIT 2",
        )
        .map_err(|_| error("cannot prepare durable turn trace lookup"))?;
    let sequences = statement
        .query_map(params![session_id, turn_id], |row| row.get::<_, i64>(0))
        .map_err(|_| error("cannot read durable turn trace lookup"))?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| error("durable turn trace lookup row is invalid"))?;
    drop(statement);
    if sequences.len() > 1 {
        return Err(coded_error(
            "turn-trace-duplicate",
            "turn has more than one durable trace",
        ));
    }
    sequences
        .into_iter()
        .next()
        .map(|sequence| {
            let sequence = to_u64(sequence, "turn trace event sequence")?;
            let event = query_event_at_sequence(connection, session_id, sequence)?;
            parse_turn_trace_event(&event)
        })
        .transpose()
}

fn validate_trace_terminal_pair(
    connection: &Connection,
    stored: &StoredTurnTrace,
) -> Result<(), WorkbenchStoreError> {
    let terminal_sequence = stored.event_sequence.checked_add(1).ok_or_else(|| {
        coded_error(
            "turn-trace-terminal-sequence-invalid",
            "turn trace terminal sequence overflowed",
        )
    })?;
    let event = query_event_at_sequence(
        connection,
        &stored.trace.binding.session_id,
        terminal_sequence,
    )?;
    if !validate_turn_terminal_event(
        &event,
        stored.trace.binding.project_id.as_deref(),
        &stored.trace.binding.turn_id,
        &stored.state,
        stored.recorded_at_ms,
    ) {
        return Err(coded_error(
            "turn-trace-terminal-event-invalid",
            "durable turn trace is not followed by its exact terminal event",
        ));
    }
    Ok(())
}

fn validate_turn_terminal_event(
    event: &WorkbenchEvent,
    project_id: Option<&str>,
    turn_id: &str,
    state: &str,
    updated_at_ms: u64,
) -> bool {
    let payload = match event.payload.as_object() {
        Some(payload) => payload,
        None => return false,
    };
    let allowed_keys = ["schema_version", "turn_id", "state", "updated_at_ms"];
    let expected_event_id = derived_event_id(
        "turn-terminal",
        format!("{turn_id}\0{state}\0{updated_at_ms}").as_bytes(),
    );
    event.event_kind == format!("turn.{state}")
        && event.event_id == expected_event_id
        && event.correlation_id == turn_id
        && event.operation_id == turn_id
        && event.project_id.as_deref() == project_id
        && event.generation == 0
        && event.timestamp_ms == updated_at_ms
        && payload.len() == allowed_keys.len()
        && payload
            .keys()
            .all(|key| allowed_keys.contains(&key.as_str()))
        && payload.get("schema_version").and_then(Value::as_str) == Some("turn.terminal/0.1")
        && payload.get("turn_id").and_then(Value::as_str) == Some(turn_id)
        && payload.get("state").and_then(Value::as_str) == Some(state)
        && payload.get("updated_at_ms").and_then(Value::as_u64) == Some(updated_at_ms)
}

struct EventInput<'a> {
    session_id: &'a str,
    event_id: &'a str,
    timestamp_ms: u64,
    correlation_id: &'a str,
    event_kind: &'a str,
    project_id: Option<&'a str>,
    operation_id: &'a str,
    generation: u64,
    payload: serde_json::Value,
}

fn append_event_tx(
    transaction: &Transaction<'_>,
    input: EventInput<'_>,
) -> Result<WorkbenchEvent, WorkbenchStoreError> {
    validate_identifier(input.session_id, "event session ID")?;
    validate_event_identifier(input.event_id, "event ID")?;
    validate_event_identifier(input.correlation_id, "event correlation ID")?;
    validate_event_kind(input.event_kind)?;
    if let Some(project_id) = input.project_id {
        validate_identifier(project_id, "event project ID")?;
    }
    validate_identifier(input.operation_id, "event operation ID")?;
    validate_persisted_text(input.correlation_id, "event correlation ID")?;
    validate_persisted_text(input.event_kind, "event kind")?;
    if let Some(project_id) = input.project_id {
        validate_persisted_text(project_id, "event project ID")?;
    }
    validate_persisted_text(input.operation_id, "event operation ID")?;
    validate_persisted_payload(&input.payload, "event payload")?;
    let sequence = transaction
        .query_row(
            "SELECT next_sequence FROM session_sequences WHERE session_id = ?1",
            [input.session_id],
            |row| row.get::<_, i64>(0),
        )
        .optional()
        .map_err(|_| error("cannot read session event sequence"))?;
    let sequence = match sequence {
        Some(value) => {
            let sequence = to_u64(value, "session event sequence")?;
            transaction
                .execute(
                    "UPDATE session_sequences SET next_sequence = next_sequence + 1
                     WHERE session_id = ?1",
                    [input.session_id],
                )
                .map_err(|_| error("cannot advance session event sequence"))?;
            sequence
        }
        None => {
            transaction
                .execute(
                    "INSERT INTO session_sequences(session_id, next_sequence) VALUES (?1, 2)",
                    [input.session_id],
                )
                .map_err(|_| error("cannot initialize session event sequence"))?;
            1
        }
    };
    let payload_json = serde_json::to_string(&input.payload)
        .map_err(|_| error("cannot serialize workbench event payload"))?;
    if payload_json.len() > MAX_EVENT_BYTES {
        return Err(error("workbench event payload exceeds size limit"));
    }
    let payload_hash = ContentHash::for_bytes(payload_json.as_bytes());
    transaction
        .execute(
            "INSERT INTO events (
                session_id, sequence, event_id, timestamp_ms, correlation_id,
                event_kind, project_id, operation_id, generation,
                payload_json, payload_sha256, payload_bytes
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)",
            params![
                input.session_id,
                to_i64(sequence, "event sequence")?,
                input.event_id,
                to_i64(input.timestamp_ms, "event timestamp")?,
                input.correlation_id,
                input.event_kind,
                input.project_id,
                input.operation_id,
                to_i64(input.generation, "event generation")?,
                payload_json,
                payload_hash.sha256,
                to_i64(payload_hash.bytes, "event payload byte count")?,
            ],
        )
        .map_err(|_| error("cannot append workbench event"))?;
    Ok(WorkbenchEvent {
        schema_version: "workbench-event/0.1".into(),
        session_id: input.session_id.into(),
        sequence,
        event_id: input.event_id.into(),
        timestamp_ms: input.timestamp_ms,
        correlation_id: input.correlation_id.into(),
        event_kind: input.event_kind.into(),
        project_id: input.project_id.map(str::to_owned),
        operation_id: input.operation_id.into(),
        generation: input.generation,
        payload: input.payload,
        payload_hash,
    })
}

fn consume_decision_tx(
    transaction: &Transaction<'_>,
    reference: &GitWorkflowDecisionReference,
    kind: GitWorkflowDecisionKind,
    requirement: &GitWorkflowAuthorizationRequirement,
    observed_at_ms: u64,
) -> Result<(), WorkbenchStoreError> {
    let decision = query_decision(transaction, &reference.authority_id, &reference.decision_id)?;
    validate_stored_decision(&decision, kind, requirement, Some(observed_at_ms))?;
    if decision_reference(&decision) != *reference {
        return Err(error("authorization decision reference was modified"));
    }
    let changed = transaction
        .execute(
            "UPDATE approval_decisions
             SET status = 'consumed', consumed_at_ms = ?1
             WHERE authority_id = ?2 AND decision_id = ?3 AND status = 'issued'",
            params![
                to_i64(observed_at_ms, "decision consumption time")?,
                reference.authority_id,
                reference.decision_id,
            ],
        )
        .map_err(|_| error("cannot consume authorization decision"))?;
    if changed != 1 {
        return Err(error("authorization decision was already consumed"));
    }
    Ok(())
}

fn query_decision(
    connection: &Connection,
    authority_id: &str,
    decision_id: &str,
) -> Result<StoredDecision, WorkbenchStoreError> {
    connection
        .query_row(
            "SELECT authority_id, decision_id, decision_kind,
                    requirement_sha256, requirement_bytes,
                    scope, issued_at_ms, expires_at_ms, status
             FROM approval_decisions
             WHERE authority_id = ?1 AND decision_id = ?2",
            params![authority_id, decision_id],
            |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    row.get::<_, String>(1)?,
                    row.get::<_, String>(2)?,
                    row.get::<_, String>(3)?,
                    row.get::<_, i64>(4)?,
                    row.get::<_, String>(5)?,
                    row.get::<_, i64>(6)?,
                    row.get::<_, i64>(7)?,
                    row.get::<_, String>(8)?,
                ))
            },
        )
        .optional()
        .map_err(|_| error("cannot read authorization decision"))?
        .map(
            |(
                authority_id,
                decision_id,
                decision_kind,
                requirement_sha256,
                requirement_bytes,
                scope,
                issued_at_ms,
                expires_at_ms,
                status,
            )| {
                Ok(StoredDecision {
                    authority_id,
                    decision_id,
                    decision_kind,
                    requirement_hash: ContentHash {
                        sha256: requirement_sha256,
                        bytes: to_u64(requirement_bytes, "requirement byte count")?,
                    },
                    scope,
                    issued_at_ms: to_u64(issued_at_ms, "decision issue time")?,
                    expires_at_ms: to_u64(expires_at_ms, "decision expiry time")?,
                    status,
                })
            },
        )
        .transpose()?
        .ok_or_else(|| error("authorization decision does not exist"))
}

fn validate_stored_decision(
    decision: &StoredDecision,
    kind: GitWorkflowDecisionKind,
    requirement: &GitWorkflowAuthorizationRequirement,
    observed_at_ms: Option<u64>,
) -> Result<(), WorkbenchStoreError> {
    if decision.decision_kind != kind.as_str()
        || decision.requirement_hash != requirement.requirement_hash
        || decision.scope != "allow-once"
        || decision.status != "issued"
        || observed_at_ms.is_some_and(|observed| {
            decision.issued_at_ms > observed || observed > decision.expires_at_ms
        })
    {
        return Err(error(
            "authorization decision is stale, consumed, or out of scope",
        ));
    }
    Ok(())
}

fn decision_reference(decision: &StoredDecision) -> GitWorkflowDecisionReference {
    GitWorkflowDecisionReference {
        authority_id: decision.authority_id.clone(),
        decision_id: decision.decision_id.clone(),
        scope: decision.scope.clone(),
        scope_hash: decision.requirement_hash.clone(),
        issued_at_ms: decision.issued_at_ms,
        expires_at_ms: decision.expires_at_ms,
    }
}

fn validate_requirement_summary(
    requirement: &GitWorkflowAuthorizationRequirement,
) -> Result<(), WorkbenchStoreError> {
    if requirement.schema_version != "git-workflow-authorization-requirement/0.1" {
        return Err(error("authorization requirement schema is invalid"));
    }
    validate_identifier(&requirement.project_id, "requirement project ID")?;
    validate_identifier(&requirement.session_id, "requirement session ID")?;
    validate_identifier(&requirement.operation_id, "requirement operation ID")?;
    if requirement.requirement_hash.sha256.len() != 64
        || requirement.requirement_hash.bytes == 0
        || !requirement
            .requirement_hash
            .sha256
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
        || !matches!(requirement.risk.class.as_str(), "medium" | "high")
        || !requirement.risk.requires_permission
        || !requirement.risk.requires_explicit_approval
    {
        return Err(error("authorization requirement summary is invalid"));
    }
    Ok(())
}

fn action_name(requirement: &GitWorkflowAuthorizationRequirement) -> &'static str {
    match requirement.action {
        crate::git_workflow_authorization::GitWorkflowAuthorizedAction::Start => "start",
        crate::git_workflow_authorization::GitWorkflowAuthorizedAction::Abort => "abort",
        crate::git_workflow_authorization::GitWorkflowAuthorizedAction::Continue => "continue",
    }
}

fn derived_event_id(prefix: &str, bytes: &[u8]) -> String {
    let hash = ContentHash::for_bytes(bytes);
    format!("{prefix}-{}", &hash.sha256[..32])
}

fn project_event_stream_id(project_id: &str) -> String {
    derived_event_id("project-stream", project_id.as_bytes())
}

fn pinned_context_operation_id(project_id: &str, set_identity: &str) -> String {
    derived_event_id(
        "pinned-context-operation",
        format!("{project_id}:{set_identity}").as_bytes(),
    )
}

#[cfg(test)]
fn projection_rebuild_crash_point(point: &str) {
    if std::env::var("AEGISY_TEST_PROJECTION_REBUILD_CRASH_POINT").as_deref() == Ok(point) {
        std::process::exit(86);
    }
}

#[cfg(not(test))]
fn projection_rebuild_crash_point(_point: &str) {}

fn push_projection_issue(issues: &mut Vec<String>, issue: &str) {
    if !issues.iter().any(|existing| existing == issue) {
        issues.push(issue.into());
    }
}

fn automatic_projection_rebuild_issue(issue: &str) -> bool {
    matches!(
        issue,
        "event-projection-mismatch"
            | "turn-input-hash-invalid"
            | "item-sequence-gap"
            | "item-turn-binding-invalid"
            | "item-payload-or-sequence-invalid"
            | "blob-reference-projection-owner-missing"
    )
}

fn projection_candidate_is_rebuildable(candidate: &SessionProjectionCandidate) -> bool {
    candidate
        .session
        .as_ref()
        .is_some_and(|session| session.session_id == candidate.session_id)
        && candidate.source_complete
        && !candidate.matches_current_projection
        && candidate.source_hash.is_some()
        && candidate
            .issues
            .iter()
            .any(|issue| issue == "event-projection-mismatch")
        && candidate
            .issues
            .iter()
            .all(|issue| issue == "event-projection-mismatch")
}

fn project_projection_candidate_is_rebuildable(candidate: &ProjectProjectionCandidate) -> bool {
    candidate
        .project
        .as_ref()
        .is_some_and(|project| project.project_id == candidate.project_id)
        && !candidate.roots.is_empty()
        && candidate.source_complete
        && !candidate.matches_current_projection
        && candidate.source_hash.is_some()
        && candidate
            .issues
            .iter()
            .any(|issue| issue == "project-event-projection-mismatch")
        && candidate
            .issues
            .iter()
            .all(|issue| issue == "project-event-projection-mismatch")
}

fn empty_startup_projection_recovery_report() -> StartupProjectionRecoveryReport {
    StartupProjectionRecoveryReport {
        schema_version: "startup-projection-recovery/0.2".into(),
        checked_projects: 0,
        healthy_projects: 0,
        rebuilt_projects: 0,
        quarantined_projects: 0,
        checked_project_roots: 0,
        checked_sessions: 0,
        healthy_sessions: 0,
        rebuilt_sessions: 0,
        quarantined_sessions: 0,
        checked_turns: 0,
        checked_items: 0,
        checked_events: 0,
        checked_blob_references: 0,
        issues: Vec::new(),
    }
}

fn projection_event_schema(event: &WorkbenchEvent) -> Option<&str> {
    event
        .payload
        .get("schema_version")
        .and_then(serde_json::Value::as_str)
}

fn projection_event_stream_hash(events: &[WorkbenchEvent]) -> ContentHash {
    let mut hasher = Sha256::new();
    let mut bytes = 0_u64;
    for event in events {
        hash_projection_field(&mut hasher, &mut bytes, &event.sequence.to_be_bytes());
        hash_projection_field(&mut hasher, &mut bytes, event.event_id.as_bytes());
        hash_projection_field(&mut hasher, &mut bytes, &event.timestamp_ms.to_be_bytes());
        hash_projection_field(&mut hasher, &mut bytes, event.correlation_id.as_bytes());
        hash_projection_field(&mut hasher, &mut bytes, event.event_kind.as_bytes());
        hash_projection_field(
            &mut hasher,
            &mut bytes,
            event.project_id.as_deref().unwrap_or("").as_bytes(),
        );
        hash_projection_field(&mut hasher, &mut bytes, event.operation_id.as_bytes());
        hash_projection_field(&mut hasher, &mut bytes, &event.generation.to_be_bytes());
        hash_projection_field(
            &mut hasher,
            &mut bytes,
            event.payload_hash.sha256.as_bytes(),
        );
        hash_projection_field(
            &mut hasher,
            &mut bytes,
            &event.payload_hash.bytes.to_be_bytes(),
        );
    }
    let digest = hasher.finalize();
    ContentHash {
        sha256: digest.iter().map(|byte| format!("{byte:02x}")).collect(),
        bytes,
    }
}

fn hash_projection_field(hasher: &mut Sha256, bytes: &mut u64, value: &[u8]) {
    let length = value.len() as u64;
    hasher.update(length.to_be_bytes());
    hasher.update(value);
    *bytes = bytes.saturating_add(8).saturating_add(length);
}

fn load_projection_source_events(
    connection: &Connection,
    session_id: &str,
) -> Result<Vec<WorkbenchEvent>, WorkbenchStoreError> {
    let count = query_count(
        connection,
        "SELECT COUNT(*) FROM events WHERE session_id = ?1",
        session_id,
        "cannot count projection source events",
    )?;
    if count > MAX_PROJECTION_VERIFY_ROWS {
        return Err(error("projection event limit is exceeded"));
    }
    let mut statement = connection
        .prepare(
            "SELECT sequence, event_id, timestamp_ms, correlation_id, event_kind,
                    project_id, operation_id, generation, payload_json,
                    payload_sha256, payload_bytes
             FROM events WHERE session_id = ?1 ORDER BY sequence",
        )
        .map_err(|_| error("cannot prepare projection source replay"))?;
    let rows = statement
        .query_map([session_id], |row| {
            Ok((
                row.get::<_, i64>(0)?,
                row.get::<_, String>(1)?,
                row.get::<_, i64>(2)?,
                row.get::<_, String>(3)?,
                row.get::<_, String>(4)?,
                row.get::<_, Option<String>>(5)?,
                row.get::<_, String>(6)?,
                row.get::<_, i64>(7)?,
                row.get::<_, String>(8)?,
                row.get::<_, String>(9)?,
                row.get::<_, i64>(10)?,
            ))
        })
        .map_err(|_| error("cannot read projection source replay"))?;
    let mut events = Vec::new();
    for row in rows {
        let (
            sequence,
            event_id,
            timestamp_ms,
            correlation_id,
            event_kind,
            project_id,
            operation_id,
            generation,
            payload_json,
            payload_sha256,
            payload_bytes,
        ) = row.map_err(|_| error("projection source row is invalid"))?;
        let sequence = to_u64(sequence, "event sequence")?;
        if sequence != events.len() as u64 + 1 {
            return Err(error("projection source sequence has a gap"));
        }
        let payload_hash = ContentHash {
            sha256: payload_sha256,
            bytes: to_u64(payload_bytes, "event payload byte count")?,
        };
        if ContentHash::for_bytes(payload_json.as_bytes()) != payload_hash {
            return Err(error("projection source payload integrity check failed"));
        }
        let payload = serde_json::from_str(&payload_json)
            .map_err(|_| error("projection source payload JSON is invalid"))?;
        validate_persisted_payload(&payload, "projection source event payload")?;
        events.push(WorkbenchEvent {
            schema_version: "workbench-event/0.1".into(),
            session_id: session_id.into(),
            sequence,
            event_id,
            timestamp_ms: to_u64(timestamp_ms, "event timestamp")?,
            correlation_id,
            event_kind,
            project_id,
            operation_id,
            generation: to_u64(generation, "event generation")?,
            payload,
            payload_hash,
        });
    }
    if events.len() as u64 != count {
        return Err(error("projection source count is inconsistent"));
    }
    Ok(events)
}

fn parse_operation_reconciliation_event(
    event: &WorkbenchEvent,
) -> Result<StoredOperationReconciliation, WorkbenchStoreError> {
    if event.event_kind != "operation.reconciled"
        || event.payload.get("schema_version").and_then(Value::as_str)
            != Some("operation.reconciled/0.1")
    {
        return Err(error("operation reconciliation event schema is invalid"));
    }
    let input = event
        .payload
        .get("input")
        .cloned()
        .and_then(|value| serde_json::from_value::<ReconciliationInput>(value).ok())
        .ok_or_else(|| error("operation reconciliation event input is invalid"))?;
    let result = event
        .payload
        .get("result")
        .cloned()
        .and_then(|value| serde_json::from_value::<ReconciliationResult>(value).ok())
        .ok_or_else(|| error("operation reconciliation event result is invalid"))?;
    let expected = reconcile_operation(&input).map_err(|cause| {
        error(format!(
            "operation reconciliation event evidence is invalid: {}",
            cause.message
        ))
    })?;
    let updated_at_ms = event
        .payload
        .get("updated_at_ms")
        .and_then(Value::as_u64)
        .ok_or_else(|| error("operation reconciliation event timestamp is invalid"))?;
    if expected != result
        || input.session_id != event.session_id
        || input.operation_id != event.operation_id
        || event.correlation_id != result.review_id
        || event.generation != 0
        || updated_at_ms != event.timestamp_ms
        || event.event_id
            != derived_event_id(
                "operation-reconciled",
                format!("{}:{}", input.operation_id, result.review_id).as_bytes(),
            )
    {
        return Err(error("operation reconciliation event identity is invalid"));
    }
    Ok(StoredOperationReconciliation {
        input,
        result,
        event_sequence: event.sequence,
        updated_at_ms,
    })
}

fn validate_session_runtime_binding_create(
    binding: &StoredSessionRuntimeBindingCreate,
    expected_session_id: &str,
) -> Result<(), WorkbenchStoreError> {
    validate_identifier(&binding.session_id, "runtime binding session ID")?;
    if binding.session_id != expected_session_id {
        return Err(error(
            "session runtime binding owner does not match session",
        ));
    }
    validate_event_identifier(&binding.adapter, "runtime binding adapter")?;
    validate_text(
        &binding.adapter_version,
        256,
        "runtime binding adapter version",
    )?;
    if let Some(backend_session_id) = binding.backend_session_id.as_deref() {
        validate_identity_text(backend_session_id, "runtime binding backend session ID")?;
    }
    if let Some(provider) = binding.provider.as_deref() {
        validate_text(provider, 256, "runtime binding provider")?;
    }
    if let Some(model) = binding.model.as_deref() {
        validate_text(model, 256, "runtime binding model")?;
    }
    if binding.permission_profile != "read-only" {
        return Err(error(
            "session runtime binding permission profile is invalid",
        ));
    }
    Ok(())
}

fn insert_session_runtime_binding_tx(
    transaction: &Transaction<'_>,
    binding: &StoredSessionRuntimeBindingCreate,
) -> Result<(), WorkbenchStoreError> {
    let timestamp = to_i64(binding.created_at_ms, "runtime binding creation time")?;
    transaction
        .execute(
            "INSERT INTO session_runtime_bindings (
                session_id, adapter, adapter_version, backend_session_id, provider,
                model, permission_profile, created_at_ms, updated_at_ms
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?8)",
            params![
                binding.session_id,
                binding.adapter,
                binding.adapter_version,
                binding.backend_session_id,
                binding.provider,
                binding.model,
                binding.permission_profile,
                timestamp,
            ],
        )
        .map_err(|_| error("session runtime binding already exists or is invalid"))?;
    Ok(())
}

fn append_session_runtime_binding_event_tx(
    transaction: &Transaction<'_>,
    binding: &StoredSessionRuntimeBindingCreate,
    session_id: &str,
    project_id: Option<&str>,
) -> Result<(), WorkbenchStoreError> {
    let binding_json = serde_json::to_vec(binding)
        .map_err(|_| error("cannot serialize session runtime binding"))?;
    let binding_hash = ContentHash::for_bytes(&binding_json);
    let event_id = derived_event_id("session-runtime-bound", session_id.as_bytes());
    append_event_tx(
        transaction,
        EventInput {
            session_id,
            event_id: &event_id,
            timestamp_ms: binding.created_at_ms,
            correlation_id: session_id,
            event_kind: "session.runtime-bound",
            project_id,
            operation_id: session_id,
            generation: 0,
            payload: json!({
                "schema_version": "session.runtime-bound/0.1",
                "session_id": session_id,
                "adapter": binding.adapter,
                "adapter_version": binding.adapter_version,
                "provider": binding.provider,
                "model": binding.model,
                "permission_profile": binding.permission_profile,
                "backend_session_id_present": binding.backend_session_id.is_some(),
                "binding_hash": binding_hash,
                "bound_at_ms": binding.created_at_ms
            }),
        },
    )
    .map(|_| ())
}

fn validate_session_workspace_binding_create(
    binding: &StoredSessionWorkspaceBindingCreate,
    expected_session_id: &str,
    expected_project_id: Option<&str>,
) -> Result<(), WorkbenchStoreError> {
    validate_identifier(&binding.session_id, "workspace binding session ID")?;
    validate_identifier(&binding.project_id, "workspace binding project ID")?;
    validate_identifier(&binding.root_id, "workspace binding root ID")?;
    validate_identity_text(&binding.root_identity, "workspace binding root identity")?;
    if binding.session_id != expected_session_id
        || expected_project_id != Some(binding.project_id.as_str())
    {
        return Err(error(
            "session workspace binding owner does not match session",
        ));
    }
    if binding.workspace_kind != "project-root"
        || !matches!(
            binding.git_state.as_str(),
            "unavailable" | "not-repository" | "repository-only" | "worktree"
        )
        || binding.dedicated_worktree
    {
        return Err(error("session workspace binding state is invalid"));
    }
    if let Some(identity) = binding.repository_root_identity.as_deref() {
        validate_identity_text(identity, "workspace repository root identity")?;
    }
    if let Some(identity) = binding.worktree_root_identity.as_deref() {
        validate_identity_text(identity, "workspace worktree root identity")?;
    }
    if let Some(branch) = binding.branch.as_deref() {
        validate_text(branch, 512, "workspace branch")?;
    }
    if let Some(branch_sha256) = binding.branch_sha256.as_deref() {
        validate_lower_sha256(branch_sha256, "workspace branch identity")?;
    }
    if let Some(head_oid) = binding.head_oid.as_deref() {
        if !matches!(head_oid.len(), 40 | 64)
            || !head_oid
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
        {
            return Err(error("workspace HEAD identity is invalid"));
        }
    }
    let branch_hash_matches = binding.branch.as_ref().is_none_or(|branch| {
        binding.branch_sha256.as_deref()
            == Some(ContentHash::for_bytes(branch.as_bytes()).sha256.as_str())
    });
    let branch_state_valid = matches!(
        (
            binding.branch.as_ref(),
            binding.branch_sha256.as_ref(),
            binding.branch_redacted,
        ),
        (Some(_), Some(_), false) | (None, Some(_), true) | (None, None, false)
    );
    let git_state_valid = if binding.git_state == "worktree" {
        binding.repository_root_identity.is_some()
            && binding.worktree_root_identity.is_some()
            && (binding.unborn || binding.head_oid.is_some())
            && !(binding.detached && binding.branch_sha256.is_some())
    } else {
        binding.repository_root_identity.is_none()
            && binding.worktree_root_identity.is_none()
            && binding.branch.is_none()
            && binding.branch_sha256.is_none()
            && !binding.branch_redacted
            && binding.head_oid.is_none()
            && !binding.detached
            && !binding.unborn
    };
    if !branch_hash_matches || !branch_state_valid || !git_state_valid {
        return Err(error("session workspace binding Git evidence is invalid"));
    }
    Ok(())
}

fn validate_stored_session_workspace_binding(
    binding: &StoredSessionWorkspaceBinding,
) -> Result<(), WorkbenchStoreError> {
    validate_session_workspace_binding_create(
        &StoredSessionWorkspaceBindingCreate {
            session_id: binding.session_id.clone(),
            project_id: binding.project_id.clone(),
            root_id: binding.root_id.clone(),
            root_identity: binding.root_identity.clone(),
            workspace_kind: binding.workspace_kind.clone(),
            git_state: binding.git_state.clone(),
            repository_root_identity: binding.repository_root_identity.clone(),
            worktree_root_identity: binding.worktree_root_identity.clone(),
            branch: binding.branch.clone(),
            branch_sha256: binding.branch_sha256.clone(),
            branch_redacted: binding.branch_redacted,
            head_oid: binding.head_oid.clone(),
            detached: binding.detached,
            unborn: binding.unborn,
            dedicated_worktree: binding.dedicated_worktree,
            captured_at_ms: binding.captured_at_ms,
        },
        &binding.session_id,
        Some(&binding.project_id),
    )?;
    if binding.updated_at_ms < binding.captured_at_ms {
        return Err(error("session workspace binding timestamp is invalid"));
    }
    Ok(())
}

fn insert_session_workspace_binding_tx(
    transaction: &Transaction<'_>,
    binding: &StoredSessionWorkspaceBindingCreate,
) -> Result<(), WorkbenchStoreError> {
    let captured_at_ms = to_i64(binding.captured_at_ms, "workspace binding capture time")?;
    transaction
        .execute(
            "INSERT INTO session_workspace_bindings (
                session_id, project_id, root_id, root_identity, workspace_kind,
                git_state, repository_root_identity, worktree_root_identity, branch,
                branch_sha256, branch_redacted, head_oid, detached, unborn,
                dedicated_worktree, captured_at_ms, updated_at_ms
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12,
                       ?13, ?14, ?15, ?16, ?16)",
            params![
                binding.session_id,
                binding.project_id,
                binding.root_id,
                binding.root_identity,
                binding.workspace_kind,
                binding.git_state,
                binding.repository_root_identity,
                binding.worktree_root_identity,
                binding.branch,
                binding.branch_sha256,
                binding.branch_redacted,
                binding.head_oid,
                binding.detached,
                binding.unborn,
                binding.dedicated_worktree,
                captured_at_ms,
            ],
        )
        .map_err(|_| error("session workspace binding already exists or is invalid"))?;
    Ok(())
}

fn append_session_workspace_binding_event_tx(
    transaction: &Transaction<'_>,
    binding: &StoredSessionWorkspaceBindingCreate,
) -> Result<(), WorkbenchStoreError> {
    let binding_json = serde_json::to_vec(binding)
        .map_err(|_| error("cannot serialize session workspace binding"))?;
    let binding_hash = ContentHash::for_bytes(&binding_json);
    let event_id = derived_event_id("session-workspace-bound", binding.session_id.as_bytes());
    append_event_tx(
        transaction,
        EventInput {
            session_id: &binding.session_id,
            event_id: &event_id,
            timestamp_ms: binding.captured_at_ms,
            correlation_id: &binding.session_id,
            event_kind: "session.workspace-bound",
            project_id: Some(&binding.project_id),
            operation_id: &binding.session_id,
            generation: 0,
            payload: json!({
                "schema_version": "session.workspace-bound/0.1",
                "binding": binding,
                "binding_hash": binding_hash,
                "raw_paths_persisted": false,
                "permission_granted": false
            }),
        },
    )
    .map(|_| ())
}

fn stored_session_workspace_binding_from_row(
    row: &rusqlite::Row<'_>,
) -> rusqlite::Result<StoredSessionWorkspaceBinding> {
    Ok(StoredSessionWorkspaceBinding {
        session_id: row.get(0)?,
        project_id: row.get(1)?,
        root_id: row.get(2)?,
        root_identity: row.get(3)?,
        workspace_kind: row.get(4)?,
        git_state: row.get(5)?,
        repository_root_identity: row.get(6)?,
        worktree_root_identity: row.get(7)?,
        branch: row.get(8)?,
        branch_sha256: row.get(9)?,
        branch_redacted: row.get::<_, i64>(10)? != 0,
        head_oid: row.get(11)?,
        detached: row.get::<_, i64>(12)? != 0,
        unborn: row.get::<_, i64>(13)? != 0,
        dedicated_worktree: row.get::<_, i64>(14)? != 0,
        captured_at_ms: to_u64_sql(row.get(15)?, "workspace binding capture time")?,
        updated_at_ms: to_u64_sql(row.get(16)?, "workspace binding update time")?,
    })
}

fn validate_rebuilt_session(session: &StoredSession) -> Result<(), WorkbenchStoreError> {
    validate_identifier(&session.session_id, "session ID")?;
    if let Some(project_id) = &session.project_id {
        validate_identifier(project_id, "session project ID")?;
    }
    validate_text(&session.title, 256, "session title")?;
    if let Some(parent_session_id) = &session.parent_session_id {
        validate_identifier(parent_session_id, "parent session ID")?;
    }
    if let Some(environment_identity) = &session.environment_identity {
        validate_identity_text(environment_identity, "session environment identity")?;
    }
    if session.mode == StoredSessionMode::Work && session.project_id.is_none() {
        return Err(error("Work session requires a project"));
    }
    match (session.parent_session_id.is_some(), session.lineage_kind) {
        (false, StoredSessionLineage::New)
        | (true, StoredSessionLineage::Resume | StoredSessionLineage::Fork) => {}
        _ => return Err(error("session lineage kind does not match parent session")),
    }
    if !matches!(
        session.status.as_str(),
        "active" | "archived" | "failed" | "interrupted"
    ) || session.updated_at_ms < session.created_at_ms
    {
        return Err(error("session state is invalid"));
    }
    Ok(())
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct SessionDeletionMemberSnapshot {
    session_id: String,
    depth: u64,
    original_status: String,
}

#[derive(Debug)]
struct SessionDeletionPlanMember {
    session: StoredSession,
    depth: u64,
}

#[derive(Debug)]
struct SessionDeletionPlan {
    preview: SessionDeletionPreview,
    selected: Vec<SessionDeletionPlanMember>,
}

fn build_session_deletion_plan(
    connection: &Connection,
    root_session_id: &str,
    scope: SessionDeletionScope,
) -> Result<SessionDeletionPlan, WorkbenchStoreError> {
    validate_identifier(root_session_id, "session deletion root ID")?;
    let mut statement = connection
        .prepare(
            "SELECT session_id, project_id, mode, title, parent_session_id,
                    lineage_kind, status, environment_identity, created_at_ms, updated_at_ms
             FROM sessions ORDER BY session_id LIMIT ?1",
        )
        .map_err(|_| error("cannot prepare session deletion lineage"))?;
    let sessions = statement
        .query_map([MAX_SESSION_DELETE_MEMBERS as i64 + 1], |row| {
            Ok(StoredSession {
                session_id: row.get(0)?,
                project_id: row.get(1)?,
                mode: parse_session_mode(&row.get::<_, String>(2)?)?,
                title: row.get(3)?,
                parent_session_id: row.get(4)?,
                lineage_kind: parse_session_lineage(&row.get::<_, String>(5)?)?,
                status: row.get(6)?,
                environment_identity: row.get(7)?,
                created_at_ms: to_u64_sql(row.get(8)?, "session creation time")?,
                updated_at_ms: to_u64_sql(row.get(9)?, "session update time")?,
            })
        })
        .map_err(|_| error("cannot read session deletion lineage"))?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| error("session deletion lineage row is invalid"))?;
    if sessions.len() > MAX_SESSION_DELETE_MEMBERS {
        return Err(coded_error(
            "session-deletion-lineage-limit",
            "session deletion lineage exceeds its limit",
        ));
    }
    let root = sessions
        .iter()
        .find(|session| session.session_id == root_session_id)
        .cloned()
        .ok_or_else(|| error("session deletion root does not exist"))?;
    let mut children = BTreeMap::<String, Vec<StoredSession>>::new();
    for session in &sessions {
        if let Some(parent_session_id) = &session.parent_session_id {
            children
                .entry(parent_session_id.clone())
                .or_default()
                .push(session.clone());
        }
    }
    for child_sessions in children.values_mut() {
        child_sessions.sort_by(|left, right| left.session_id.cmp(&right.session_id));
    }
    let mut lineage = Vec::<SessionDeletionPlanMember>::new();
    let mut queue = std::collections::VecDeque::from([(root, 0_u64)]);
    let mut visited = BTreeSet::new();
    while let Some((session, depth)) = queue.pop_front() {
        if !visited.insert(session.session_id.clone()) {
            return Err(error("session deletion lineage contains a cycle"));
        }
        if lineage.len() >= MAX_SESSION_DELETE_MEMBERS {
            return Err(coded_error(
                "session-deletion-lineage-limit",
                "session deletion lineage exceeds its limit",
            ));
        }
        if let Some(child_sessions) = children.get(&session.session_id) {
            for child in child_sessions {
                queue.push_back((child.clone(), depth.saturating_add(1)));
            }
        }
        lineage.push(SessionDeletionPlanMember { session, depth });
    }
    let selected = match scope {
        SessionDeletionScope::SessionOnly => lineage
            .iter()
            .filter(|member| member.depth == 0)
            .map(|member| SessionDeletionPlanMember {
                session: member.session.clone(),
                depth: member.depth,
            })
            .collect::<Vec<_>>(),
        SessionDeletionScope::Lineage => lineage
            .iter()
            .map(|member| SessionDeletionPlanMember {
                session: member.session.clone(),
                depth: member.depth,
            })
            .collect::<Vec<_>>(),
    };
    let mut turn_count = 0_u64;
    let mut item_count = 0_u64;
    let mut event_count = 0_u64;
    let mut background_job_count = 0_u64;
    let mut artifact_reference_count = 0_u64;
    let mut artifact_bytes = 0_u64;
    let mut blocking_reasons = Vec::new();
    for member in &selected {
        turn_count = turn_count.saturating_add(query_count(
            connection,
            "SELECT COUNT(*) FROM turns WHERE session_id = ?1",
            &member.session.session_id,
            "cannot count session deletion turns",
        )?);
        item_count = item_count.saturating_add(query_count(
            connection,
            "SELECT COUNT(*) FROM items WHERE session_id = ?1",
            &member.session.session_id,
            "cannot count session deletion items",
        )?);
        event_count = event_count.saturating_add(query_count(
            connection,
            "SELECT COUNT(*) FROM events WHERE session_id = ?1",
            &member.session.session_id,
            "cannot count session deletion events",
        )?);
        let member_background_jobs = query_count(
            connection,
            "SELECT COUNT(*) FROM background_jobs WHERE session_id = ?1",
            &member.session.session_id,
            "cannot count session deletion background jobs",
        )?;
        background_job_count = background_job_count.saturating_add(member_background_jobs);
        let active_background_jobs = query_count(
            connection,
            "SELECT COUNT(*) FROM background_jobs
             WHERE session_id = ?1 AND status NOT IN (
                'completed','failed','cancelled','interrupted'
             )",
            &member.session.session_id,
            "cannot inspect session deletion active background jobs",
        )?;
        if active_background_jobs > 0 {
            push_projection_issue(&mut blocking_reasons, "session-background-job-active");
        }
        let active_background_leases = query_count(
            connection,
            "SELECT COUNT(*) FROM background_job_leases
             WHERE session_id = ?1 AND status = 'active'",
            &member.session.session_id,
            "cannot inspect session deletion active background leases",
        )?;
        if active_background_leases > 0 {
            push_projection_issue(&mut blocking_reasons, "session-background-lease-active");
        }
        let (references, bytes): (i64, i64) = connection
            .query_row(
                "SELECT COUNT(*), COALESCE(SUM(blob.bytes), 0)
                 FROM durable_blob_references AS reference
                 JOIN durable_blobs AS blob ON blob.sha256 = reference.blob_sha256
                 WHERE reference.session_id = ?1",
                [&member.session.session_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .map_err(|_| error("cannot count session deletion artifacts"))?;
        artifact_reference_count = artifact_reference_count
            .saturating_add(to_u64(references, "session deletion artifact count")?);
        artifact_bytes =
            artifact_bytes.saturating_add(to_u64(bytes, "session deletion artifact bytes")?);
        let deletion_state: Option<String> = connection
            .query_row(
                "SELECT deletion.state
                 FROM session_deletion_members AS deletion_member
                 JOIN session_deletions AS deletion
                   ON deletion.deletion_id = deletion_member.deletion_id
                 WHERE deletion_member.session_id = ?1",
                [&member.session.session_id],
                |row| row.get(0),
            )
            .optional()
            .map_err(|_| error("cannot inspect existing session deletion"))?;
        if let Some(state) = deletion_state {
            push_projection_issue(
                &mut blocking_reasons,
                if state == "pending" {
                    "session-deletion-already-pending"
                } else {
                    "session-already-deleted"
                },
            );
        }
    }
    let affected_sessions_truncated = lineage.len() > MAX_SESSION_DELETE_PREVIEW_MEMBERS;
    let affected_sessions = lineage
        .iter()
        .take(MAX_SESSION_DELETE_PREVIEW_MEMBERS)
        .map(|member| SessionDeletionAffectedSession {
            session_id: member.session.session_id.clone(),
            title: member.session.title.clone(),
            status: member.session.status.clone(),
            depth: member.depth,
        })
        .collect::<Vec<_>>();
    let plan_value = json!({
        "schema_version": "session-deletion-plan/0.1",
        "root_session_id": root_session_id,
        "scope": scope,
        "selected": selected.iter().map(|member| json!({
            "session_id": member.session.session_id,
            "parent_session_id": member.session.parent_session_id,
            "depth": member.depth,
            "status": member.session.status,
            "updated_at_ms": member.session.updated_at_ms
        })).collect::<Vec<_>>(),
        "lineage": lineage.iter().map(|member| json!({
            "session_id": member.session.session_id,
            "parent_session_id": member.session.parent_session_id,
            "depth": member.depth,
            "updated_at_ms": member.session.updated_at_ms
        })).collect::<Vec<_>>(),
        "turn_count": turn_count,
        "item_count": item_count,
        "event_count": event_count,
        "background_job_count": background_job_count,
        "artifact_reference_count": artifact_reference_count,
        "artifact_bytes": artifact_bytes
    });
    let plan_json = serde_json::to_vec(&plan_value)
        .map_err(|_| error("cannot serialize session deletion plan"))?;
    let preview = SessionDeletionPreview {
        schema_version: "session-deletion-preview/0.1".into(),
        root_session_id: root_session_id.into(),
        scope,
        plan_hash: ContentHash::for_bytes(&plan_json),
        session_count: selected.len() as u64,
        descendant_count: lineage.len().saturating_sub(1) as u64,
        turn_count,
        item_count,
        event_count,
        background_job_count,
        artifact_reference_count,
        artifact_bytes,
        affected_sessions,
        affected_sessions_truncated,
        undo_window_min_ms: MIN_SESSION_DELETE_UNDO_MS,
        undo_window_max_ms: MAX_SESSION_DELETE_UNDO_MS,
        blocking_reasons,
    };
    Ok(SessionDeletionPlan { preview, selected })
}

fn load_session_deletion_receipt(
    connection: &Connection,
    deletion_id: &str,
) -> Result<SessionDeletionReceipt, WorkbenchStoreError> {
    connection
        .query_row(
            "SELECT deletion_id, root_session_id, scope, plan_sha256, plan_bytes,
                    session_count, descendant_count, artifact_reference_count,
                    artifact_bytes, requested_at_ms, undo_until_ms, state
             FROM session_deletions WHERE deletion_id = ?1",
            [deletion_id],
            |row| {
                let scope = parse_session_deletion_scope(&row.get::<_, String>(2)?)?;
                Ok(SessionDeletionReceipt {
                    schema_version: "session-deletion-receipt/0.1".into(),
                    deletion_id: row.get(0)?,
                    root_session_id: row.get(1)?,
                    scope,
                    plan_hash: ContentHash {
                        sha256: row.get(3)?,
                        bytes: to_u64_sql(row.get(4)?, "session deletion plan bytes")?,
                    },
                    session_count: to_u64_sql(row.get(5)?, "session deletion member count")?,
                    descendant_count: to_u64_sql(row.get(6)?, "session deletion descendant count")?,
                    artifact_reference_count: to_u64_sql(
                        row.get(7)?,
                        "session deletion artifact count",
                    )?,
                    artifact_bytes: to_u64_sql(row.get(8)?, "session deletion artifact bytes")?,
                    requested_at_ms: to_u64_sql(row.get(9)?, "session deletion request time")?,
                    undo_until_ms: to_u64_sql(row.get(10)?, "session deletion undo time")?,
                    state: row.get(11)?,
                })
            },
        )
        .optional()
        .map_err(|_| error("cannot read session deletion"))?
        .ok_or_else(|| error("session deletion does not exist"))
}

fn load_session_deletion_members(
    connection: &Connection,
    deletion_id: &str,
) -> Result<Vec<SessionDeletionMemberSnapshot>, WorkbenchStoreError> {
    let mut statement = connection
        .prepare(
            "SELECT session_id, depth, original_status
             FROM session_deletion_members WHERE deletion_id = ?1
             ORDER BY depth, session_id LIMIT ?2",
        )
        .map_err(|_| error("cannot prepare session deletion members"))?;
    let members = statement
        .query_map(
            params![deletion_id, MAX_SESSION_DELETE_MEMBERS as i64 + 1],
            |row| {
                Ok(SessionDeletionMemberSnapshot {
                    session_id: row.get(0)?,
                    depth: to_u64_sql(row.get(1)?, "session deletion member depth")?,
                    original_status: row.get(2)?,
                })
            },
        )
        .map_err(|_| error("cannot read session deletion members"))?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| error("session deletion member row is invalid"))?;
    if members.len() > MAX_SESSION_DELETE_MEMBERS {
        return Err(error("session deletion member limit exceeded"));
    }
    Ok(members)
}

fn parse_session_deletion_scope(value: &str) -> rusqlite::Result<SessionDeletionScope> {
    match value {
        "session-only" => Ok(SessionDeletionScope::SessionOnly),
        "lineage" => Ok(SessionDeletionScope::Lineage),
        _ => Err(rusqlite::Error::FromSqlConversionFailure(
            0,
            rusqlite::types::Type::Text,
            Box::new(std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                "session deletion scope is invalid",
            )),
        )),
    }
}

fn validate_retention_policy(policy: &RetentionPolicy) -> Result<(), WorkbenchStoreError> {
    if !matches!(policy.scope_kind.as_str(), "project" | "session") {
        return Err(error("retention policy scope is invalid"));
    }
    validate_identifier(&policy.scope_id, "retention policy scope ID")?;
    if policy.archive_after_ms.is_none() && policy.delete_after_ms.is_none() {
        return Err(error("retention policy has no action"));
    }
    if policy
        .archive_after_ms
        .is_some_and(|value| value < 24 * 60 * 60 * 1_000)
        || policy
            .delete_after_ms
            .is_some_and(|value| value < 24 * 60 * 60 * 1_000)
        || !(MIN_SESSION_DELETE_UNDO_MS..=MAX_SESSION_DELETE_UNDO_MS)
            .contains(&policy.undo_window_ms)
    {
        return Err(error("retention policy timing is invalid"));
    }
    Ok(())
}

fn retention_policy_from_row(row: &rusqlite::Row<'_>) -> rusqlite::Result<RetentionPolicy> {
    Ok(RetentionPolicy {
        scope_kind: row.get(0)?,
        scope_id: row.get(1)?,
        archive_after_ms: row
            .get::<_, Option<i64>>(2)?
            .map(|value| to_u64_sql(value, "retention archive age"))
            .transpose()?,
        delete_after_ms: row
            .get::<_, Option<i64>>(3)?
            .map(|value| to_u64_sql(value, "retention delete age"))
            .transpose()?,
        undo_window_ms: to_u64_sql(row.get(4)?, "retention undo window")?,
        delete_scope: parse_session_deletion_scope(&row.get::<_, String>(5)?)?,
        updated_at_ms: to_u64_sql(row.get(6)?, "retention policy update time")?,
    })
}

fn load_all_retention_policies(
    connection: &Connection,
) -> Result<Vec<RetentionPolicy>, WorkbenchStoreError> {
    let mut statement = connection
        .prepare(
            "SELECT scope_kind, scope_id, archive_after_ms, delete_after_ms,
                    undo_window_ms, delete_scope, updated_at_ms
             FROM retention_policies ORDER BY scope_kind, scope_id LIMIT ?1",
        )
        .map_err(|_| error("cannot prepare retention policy sweep"))?;
    let policies = statement
        .query_map(
            [MAX_SESSION_DELETE_MEMBERS as i64 + 1],
            retention_policy_from_row,
        )
        .map_err(|_| error("cannot read retention policy sweep"))?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| error("retention policy row is invalid"))?;
    if policies.len() > MAX_SESSION_DELETE_MEMBERS {
        return Err(error("retention policy limit exceeded"));
    }
    for policy in &policies {
        validate_retention_policy(policy)?;
    }
    Ok(policies)
}

fn load_all_sessions_bounded(
    connection: &Connection,
) -> Result<Vec<StoredSession>, WorkbenchStoreError> {
    let mut statement = connection
        .prepare(
            "SELECT session_id, project_id, mode, title, parent_session_id,
                    lineage_kind, status, environment_identity, created_at_ms, updated_at_ms
             FROM sessions ORDER BY updated_at_ms, session_id LIMIT ?1",
        )
        .map_err(|_| error("cannot prepare bounded session scan"))?;
    let sessions = statement
        .query_map([MAX_SESSION_DELETE_MEMBERS as i64 + 1], |row| {
            Ok(StoredSession {
                session_id: row.get(0)?,
                project_id: row.get(1)?,
                mode: parse_session_mode(&row.get::<_, String>(2)?)?,
                title: row.get(3)?,
                parent_session_id: row.get(4)?,
                lineage_kind: parse_session_lineage(&row.get::<_, String>(5)?)?,
                status: row.get(6)?,
                environment_identity: row.get(7)?,
                created_at_ms: to_u64_sql(row.get(8)?, "session creation time")?,
                updated_at_ms: to_u64_sql(row.get(9)?, "session update time")?,
            })
        })
        .map_err(|_| error("cannot read bounded session scan"))?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| error("bounded session row is invalid"))?;
    if sessions.len() > MAX_SESSION_DELETE_MEMBERS {
        return Err(error("bounded session scan limit exceeded"));
    }
    Ok(sessions)
}

fn now_ms_for_store() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_millis().min(u128::from(u64::MAX)) as u64)
        .unwrap_or(0)
}

fn validate_rebuilt_project(project: &StoredProject) -> Result<(), WorkbenchStoreError> {
    validate_identifier(&project.project_id, "project ID")?;
    validate_identity_text(&project.root_identity, "project root identity")?;
    validate_text(&project.display_name, 256, "project display name")?;
    validate_stored_canonical_root(&project.canonical_root)?;
    if !matches!(
        project.state.as_str(),
        "active" | "archived" | "unavailable"
    ) || project.updated_at_ms < project.created_at_ms
    {
        return Err(error("project state is invalid"));
    }
    Ok(())
}

fn validate_project_root(root: &StoredProjectRoot) -> Result<(), WorkbenchStoreError> {
    validate_identifier(&root.project_id, "project root owner ID")?;
    validate_identifier(&root.root_id, "project root ID")?;
    validate_identity_text(&root.root_identity, "project root identity")?;
    if !matches!(root.access.as_str(), "read" | "write") {
        return Err(error("project root access is invalid"));
    }
    validate_stored_canonical_root(&root.canonical_root)?;
    Ok(())
}

fn validate_stored_canonical_root(value: &str) -> Result<(), WorkbenchStoreError> {
    use std::path::Component;
    if value.is_empty()
        || value.len() > 4 * 1024
        || value.bytes().any(|byte| byte.is_ascii_control())
    {
        return Err(error("stored project root is invalid"));
    }
    validate_persisted_text(value, "stored project root")?;
    let path = Path::new(value);
    if !path.is_absolute()
        || path
            .components()
            .any(|component| matches!(component, Component::CurDir | Component::ParentDir))
    {
        return Err(error("stored project root is not canonical"));
    }
    Ok(())
}

fn stored_project_root_from_row(row: &rusqlite::Row<'_>) -> rusqlite::Result<StoredProjectRoot> {
    Ok(StoredProjectRoot {
        project_id: row.get(0)?,
        root_id: row.get(1)?,
        canonical_root: row.get(2)?,
        root_identity: row.get(3)?,
        access: row.get(4)?,
        created_at_ms: to_u64_sql(row.get(5)?, "project root creation time")?,
    })
}

fn query_count(
    connection: &Connection,
    sql: &str,
    session_id: &str,
    failure: &str,
) -> Result<u64, WorkbenchStoreError> {
    let count = connection
        .query_row(sql, [session_id], |row| row.get::<_, i64>(0))
        .map_err(|_| error(failure))?;
    to_u64(count, "projection row count")
}

fn apply_session_search_indexes(connection: &Connection) -> Result<(), WorkbenchStoreError> {
    connection
        .execute_batch(SESSION_WORKSPACE_BINDING_SCHEMA_SQL)
        .map_err(|_| error("cannot apply session workspace binding migration"))?;
    connection
        .execute_batch(SESSION_SEARCH_INDEX_SCHEMA_SQL)
        .map_err(|_| error("cannot apply session search index migration"))?;
    connection
        .execute_batch(BACKGROUND_JOB_SCHEMA_SQL)
        .map_err(|_| error("cannot apply background job schema migration"))?;
    connection
        .execute_batch(BACKGROUND_LEASE_SCHEMA_SQL)
        .map_err(|_| error("cannot apply background lease schema migration"))?;
    connection
        .execute_batch(BACKGROUND_NOTIFICATION_OUTBOX_SCHEMA_SQL)
        .map_err(|_| error("cannot apply background notification schema migration"))
}

fn verify_required_schema(connection: &Connection) -> Result<(), WorkbenchStoreError> {
    for table in REQUIRED_TABLES
        .into_iter()
        .chain(REQUIRED_SESSION_TABLES)
        .chain(REQUIRED_TURN_TABLES)
        .chain(REQUIRED_PROJECTION_TABLES)
        .chain(REQUIRED_BLOB_TABLES)
        .chain(REQUIRED_RETENTION_TABLES)
        .chain(REQUIRED_NAVIGATION_TABLES)
        .chain(REQUIRED_RUNTIME_BINDING_TABLES)
        .chain(REQUIRED_WORKSPACE_BINDING_TABLES)
        .chain(REQUIRED_BACKGROUND_JOB_TABLES)
        .chain(REQUIRED_BACKGROUND_LEASE_TABLES)
        .chain(REQUIRED_BACKGROUND_NOTIFICATION_TABLES)
    {
        let exists: Option<String> = connection
            .query_row(
                "SELECT name FROM sqlite_master
                 WHERE type = 'table' AND name = ?1",
                [table],
                |row| row.get(0),
            )
            .optional()
            .map_err(|_| error("cannot verify workbench schema"))?;
        if exists.as_deref() != Some(table) {
            return Err(error("workbench database schema is incomplete"));
        }
    }
    for index in REQUIRED_SESSION_SEARCH_INDEXES {
        let exists: Option<String> = connection
            .query_row(
                "SELECT name FROM sqlite_master
                 WHERE type = 'index' AND name = ?1",
                [index],
                |row| row.get(0),
            )
            .optional()
            .map_err(|_| error("cannot verify workbench search indexes"))?;
        if exists.as_deref() != Some(index) {
            return Err(error("workbench database search indexes are incomplete"));
        }
    }
    for index in REQUIRED_BACKGROUND_JOB_INDEXES {
        let exists: Option<String> = connection
            .query_row(
                "SELECT name FROM sqlite_master
                 WHERE type = 'index' AND name = ?1",
                [index],
                |row| row.get(0),
            )
            .optional()
            .map_err(|_| error("cannot verify workbench background job indexes"))?;
        if exists.as_deref() != Some(index) {
            return Err(error(
                "workbench database background job indexes are incomplete",
            ));
        }
    }
    for index in REQUIRED_BACKGROUND_LEASE_INDEXES {
        let exists: Option<String> = connection
            .query_row(
                "SELECT name FROM sqlite_master
                 WHERE type = 'index' AND name = ?1",
                [index],
                |row| row.get(0),
            )
            .optional()
            .map_err(|_| error("cannot verify workbench background lease indexes"))?;
        if exists.as_deref() != Some(index) {
            return Err(error(
                "workbench database background lease indexes are incomplete",
            ));
        }
    }
    for index in REQUIRED_BACKGROUND_NOTIFICATION_INDEXES {
        let exists: Option<String> = connection
            .query_row(
                "SELECT name FROM sqlite_master
                 WHERE type = 'index' AND name = ?1",
                [index],
                |row| row.get(0),
            )
            .optional()
            .map_err(|_| error("cannot verify workbench background notification indexes"))?;
        if exists.as_deref() != Some(index) {
            return Err(error(
                "workbench database background notification indexes are incomplete",
            ));
        }
    }
    Ok(())
}

fn verify_session_workspace_binding_store(
    store: &WorkbenchStore,
) -> Result<(), WorkbenchStoreError> {
    let mut statement = store
        .connection
        .prepare(
            "SELECT session_id FROM session_workspace_bindings
             ORDER BY session_id LIMIT ?1",
        )
        .map_err(|_| error("cannot prepare session workspace binding verification"))?;
    let limit = i64::try_from(MAX_STARTUP_RECOVERY_SESSIONS.saturating_add(1))
        .map_err(|_| error("session workspace binding verification limit is invalid"))?;
    let session_ids = statement
        .query_map([limit], |row| row.get::<_, String>(0))
        .map_err(|_| error("cannot scan session workspace bindings"))?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| error("session workspace binding row is invalid"))?;
    if session_ids.len() > MAX_STARTUP_RECOVERY_SESSIONS as usize {
        return Err(error("session workspace binding limit is exceeded"));
    }
    drop(statement);
    for session_id in session_ids {
        let binding = store.load_session_workspace_binding(&session_id)?;
        let owner = store
            .connection
            .query_row(
                "SELECT s.project_id, s.mode, s.created_at_ms, r.root_identity
                 FROM sessions AS s
                 JOIN project_roots AS r
                   ON r.project_id = s.project_id AND r.root_id = ?2
                 WHERE s.session_id = ?1",
                params![session_id, binding.root_id],
                |row| {
                    Ok((
                        row.get::<_, Option<String>>(0)?,
                        row.get::<_, String>(1)?,
                        to_u64_sql(row.get(2)?, "workspace owner session creation time")?,
                        row.get::<_, String>(3)?,
                    ))
                },
            )
            .optional()
            .map_err(|_| error("cannot verify session workspace binding owner"))?;
        if owner
            .as_ref()
            .is_none_or(|(project_id, mode, created_at_ms, root_identity)| {
                project_id.as_deref() != Some(binding.project_id.as_str())
                    || mode != "work"
                    || *created_at_ms != binding.captured_at_ms
                    || root_identity != &binding.root_identity
            })
        {
            return Err(error("session workspace binding owner is invalid"));
        }
    }
    Ok(())
}

fn query_global_count(
    connection: &Connection,
    sql: &str,
    failure: &str,
) -> Result<u64, WorkbenchStoreError> {
    let count = connection
        .query_row(sql, [], |row| row.get::<_, i64>(0))
        .map_err(|_| error(failure))?;
    to_u64(count, "durable blob row count")
}

fn query_sequence_summary(
    connection: &Connection,
    sql: &str,
    session_id: &str,
    failure: &str,
) -> Result<(u64, u64, u64), WorkbenchStoreError> {
    let (count, minimum, maximum) = connection
        .query_row(sql, [session_id], |row| {
            Ok((
                row.get::<_, i64>(0)?,
                row.get::<_, i64>(1)?,
                row.get::<_, i64>(2)?,
            ))
        })
        .map_err(|_| error(failure))?;
    Ok((
        to_u64(count, "projection row count")?,
        to_u64(minimum, "projection minimum sequence")?,
        to_u64(maximum, "projection maximum sequence")?,
    ))
}

#[derive(Debug)]
struct PreparedItemAppend {
    payload_json: String,
    persisted_payload: serde_json::Value,
    payload_hash: ContentHash,
    timestamp: i64,
}

fn prepare_item_append(
    request: &StoredItemAppend,
) -> Result<PreparedItemAppend, WorkbenchStoreError> {
    validate_identifier(&request.session_id, "item session ID")?;
    validate_identifier(&request.item_id, "item ID")?;
    if let Some(turn_id) = &request.turn_id {
        validate_identifier(turn_id, "item turn ID")?;
    }
    validate_event_identifier(&request.item_kind, "item kind")?;
    validate_event_identifier(&request.role, "item role")?;
    validate_event_identifier(&request.state, "item state")?;
    let payload_json = validate_item_payload(&request.payload)?;
    let persisted_payload = serde_json::from_str(&payload_json)
        .map_err(|_| error("sanitized item payload is invalid JSON"))?;
    let payload_hash = ContentHash::for_bytes(payload_json.as_bytes());
    let timestamp = to_i64(request.created_at_ms, "item creation time")?;
    Ok(PreparedItemAppend {
        payload_json,
        persisted_payload,
        payload_hash,
        timestamp,
    })
}

fn append_item_tx(
    transaction: &Transaction<'_>,
    request: &StoredItemAppend,
    prepared: &PreparedItemAppend,
) -> Result<StoredItem, WorkbenchStoreError> {
    let session_project: Option<Option<String>> = transaction
        .query_row(
            "SELECT project_id FROM sessions WHERE session_id = ?1 AND status != 'archived'",
            [&request.session_id],
            |row| row.get(0),
        )
        .optional()
        .map_err(|_| error("cannot validate item session"))?;
    let Some(project_id) = session_project else {
        return Err(error("item session does not exist or is archived"));
    };
    if let Some(turn_id) = &request.turn_id {
        let turn: Option<(String, String)> = transaction
            .query_row(
                "SELECT session_id, state FROM turns WHERE turn_id = ?1",
                [turn_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()
            .map_err(|_| error("cannot validate item turn"))?;
        let Some((turn_session_id, turn_state)) = turn else {
            return Err(error("item turn does not exist"));
        };
        if turn_session_id != request.session_id
            || matches!(
                turn_state.as_str(),
                "completed" | "failed" | "interrupted" | "cancelled"
            )
        {
            return Err(error("item turn is not writable or is not session-bound"));
        }
    }
    let sequence: i64 = transaction
        .query_row(
            "SELECT COALESCE(MAX(item_sequence), 0) + 1 FROM items WHERE session_id = ?1",
            [&request.session_id],
            |row| row.get(0),
        )
        .map_err(|_| error("cannot allocate item sequence"))?;
    transaction
        .execute(
            "INSERT INTO items (
                session_id, item_sequence, item_id, turn_id, item_kind, role, state,
                payload_json, payload_sha256, payload_bytes, created_at_ms
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)",
            params![
                request.session_id,
                sequence,
                request.item_id,
                request.turn_id,
                request.item_kind,
                request.role,
                request.state,
                prepared.payload_json,
                prepared.payload_hash.sha256,
                to_i64(prepared.payload_hash.bytes, "item payload byte count")?,
                prepared.timestamp,
            ],
        )
        .map_err(|_| error("item already exists or is invalid"))?;
    if let Some(turn_id) = &request.turn_id {
        transaction
            .execute(
                "UPDATE turns SET state = CASE WHEN state = 'started' THEN 'running' ELSE state END,
                        updated_at_ms = CASE WHEN updated_at_ms < ?1 THEN ?1 ELSE updated_at_ms END
                 WHERE turn_id = ?2",
                params![prepared.timestamp, turn_id],
            )
            .map_err(|_| error("cannot advance turn state"))?;
    }
    let event_id = derived_event_id("item-appended", request.item_id.as_bytes());
    let operation_id = request.turn_id.as_deref().unwrap_or(&request.item_id);
    append_event_tx(
        transaction,
        EventInput {
            session_id: &request.session_id,
            event_id: &event_id,
            timestamp_ms: request.created_at_ms,
            correlation_id: &request.item_id,
            event_kind: "item.appended",
            project_id: project_id.as_deref(),
            operation_id,
            generation: 0,
            payload: json!({
                "schema_version": "item.appended/0.1",
                "item": {
                    "session_id": &request.session_id,
                    "sequence": sequence,
                    "item_id": &request.item_id,
                    "turn_id": &request.turn_id,
                    "item_kind": &request.item_kind,
                    "role": &request.role,
                    "state": &request.state,
                    "payload": &prepared.persisted_payload,
                    "payload_hash": &prepared.payload_hash,
                    "created_at_ms": request.created_at_ms
                }
            }),
        },
    )?;
    Ok(StoredItem {
        session_id: request.session_id.clone(),
        sequence: to_u64(sequence, "item sequence")?,
        item_id: request.item_id.clone(),
        turn_id: request.turn_id.clone(),
        item_kind: request.item_kind.clone(),
        role: request.role.clone(),
        state: request.state.clone(),
        payload: prepared.persisted_payload.clone(),
        payload_hash: prepared.payload_hash.clone(),
        created_at_ms: request.created_at_ms,
    })
}

#[derive(Debug)]
struct PreparedDurableBlob {
    content_hash: ContentHash,
    storage_key: String,
    metadata_json: String,
    metadata_hash: ContentHash,
}

pub fn durable_blob_reference_id(
    session_id: Option<&str>,
    project_id: Option<&str>,
    owner_kind: &str,
    owner_id: &str,
    content_reference: &str,
) -> String {
    let binding = format!(
        "{}\0{}\0{owner_kind}\0{owner_id}\0{content_reference}",
        session_id.unwrap_or_default(),
        project_id.unwrap_or_default()
    );
    format!("blobref-{}", sha256_hex(binding.as_bytes()))
}

fn prepare_durable_blob(
    request: &DurableBlobWrite,
) -> Result<PreparedDurableBlob, WorkbenchStoreError> {
    validate_identifier(&request.reference_id, "durable blob reference ID")?;
    if let Some(session_id) = &request.session_id {
        validate_identifier(session_id, "durable blob session ID")?;
    }
    if let Some(project_id) = &request.project_id {
        validate_identifier(project_id, "durable blob project ID")?;
    }
    if request.session_id.is_none() && request.project_id.is_none() {
        return Err(error("durable blob requires a session or project owner"));
    }
    if !matches!(
        request.owner_kind.as_str(),
        "session" | "turn" | "item" | "edit" | "diagnostic" | "checkpoint"
    ) {
        return Err(error("durable blob owner kind is invalid"));
    }
    validate_event_identifier(&request.owner_id, "durable blob owner ID")?;
    validate_media_type(&request.media_type)?;
    if request.content.len() as u64 > MAX_BLOB_BYTES {
        return Err(error("durable blob exceeds the per-object limit"));
    }
    let content_hash = ContentHash::for_bytes(&request.content);
    validate_content_reference(&request.content_reference, Some(&content_hash.sha256))?;
    let minimum_retention = request
        .created_at_ms
        .checked_add(MIN_BLOB_RETENTION_MS)
        .ok_or_else(|| error("durable blob retention is out of range"))?;
    let maximum_retention = request
        .created_at_ms
        .checked_add(MAX_BLOB_RETENTION_MS)
        .ok_or_else(|| error("durable blob retention is out of range"))?;
    if request.retain_until_ms < minimum_retention || request.retain_until_ms > maximum_retention {
        return Err(error("durable blob retention interval is invalid"));
    }
    if payload_contains_secret_key(&request.metadata) {
        return Err(error(
            "durable blob metadata contains a forbidden secret field",
        ));
    }
    validate_persisted_payload(&request.metadata, "durable blob metadata")?;
    let metadata_json = serde_json::to_string(&request.metadata)
        .map_err(|_| error("durable blob metadata is not serializable"))?;
    if metadata_json.len() > MAX_BLOB_METADATA_BYTES {
        return Err(error("durable blob metadata exceeds size limit"));
    }
    let metadata_hash = ContentHash::for_bytes(metadata_json.as_bytes());
    let storage_key =
        DurableBlobFileStore::storage_key(&content_hash.sha256).map_err(blob_file_error)?;
    Ok(PreparedDurableBlob {
        content_hash,
        storage_key,
        metadata_json,
        metadata_hash,
    })
}

fn persist_durable_blob_tx(
    transaction: &Transaction<'_>,
    request: &DurableBlobWrite,
    prepared: &PreparedDurableBlob,
) -> Result<StoredDurableBlobReference, WorkbenchStoreError> {
    validate_blob_ownership(
        transaction,
        request.session_id.as_deref(),
        request.project_id.as_deref(),
    )?;
    transaction
        .execute(
            "INSERT INTO durable_blobs (
                sha256, bytes, storage_key, created_at_ms,
                last_verified_at_ms, retain_until_ms
             ) VALUES (?1, ?2, ?3, ?4, ?4, ?5)
             ON CONFLICT(sha256) DO UPDATE SET
                last_verified_at_ms = CASE
                    WHEN last_verified_at_ms < excluded.last_verified_at_ms
                    THEN excluded.last_verified_at_ms ELSE last_verified_at_ms END,
                retain_until_ms = CASE
                    WHEN retain_until_ms < excluded.retain_until_ms
                    THEN excluded.retain_until_ms ELSE retain_until_ms END
             WHERE bytes = excluded.bytes AND storage_key = excluded.storage_key",
            params![
                prepared.content_hash.sha256,
                to_i64(prepared.content_hash.bytes, "durable blob byte count")?,
                prepared.storage_key,
                to_i64(request.created_at_ms, "durable blob creation time")?,
                to_i64(request.retain_until_ms, "durable blob retention time")?,
            ],
        )
        .map_err(|_| error("cannot persist durable blob metadata"))?;
    transaction
        .execute(
            "INSERT INTO durable_blob_references (
                reference_id, content_reference, blob_sha256, session_id, project_id,
                kind, media_type, owner_kind, owner_id, metadata_json,
                metadata_sha256, metadata_bytes, state, created_at_ms,
                last_accessed_at_ms, retain_until_ms, released_at_ms
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10,
                       ?11, ?12, 'active', ?13, ?13, ?14, NULL)
             ON CONFLICT(reference_id) DO NOTHING",
            params![
                request.reference_id,
                request.content_reference,
                prepared.content_hash.sha256,
                request.session_id,
                request.project_id,
                request.kind.as_str(),
                request.media_type,
                request.owner_kind,
                request.owner_id,
                prepared.metadata_json,
                prepared.metadata_hash.sha256,
                to_i64(
                    prepared.metadata_hash.bytes,
                    "durable blob metadata byte count"
                )?,
                to_i64(request.created_at_ms, "durable blob creation time")?,
                to_i64(request.retain_until_ms, "durable blob retention time")?,
            ],
        )
        .map_err(|_| error("cannot persist durable blob reference"))?;
    let mut stored = load_durable_blob_reference_tx(transaction, &request.reference_id)?;
    if stored.content_reference != request.content_reference
        || stored.content_hash != prepared.content_hash
        || stored.session_id != request.session_id
        || stored.project_id != request.project_id
        || stored.kind != request.kind
        || stored.media_type != request.media_type
        || stored.owner_kind != request.owner_kind
        || stored.owner_id != request.owner_id
        || stored.metadata != request.metadata
        || !matches!(stored.state.as_str(), "active" | "released")
    {
        return Err(error(
            "durable blob reference identity already has different data",
        ));
    }
    if stored.state == "released" {
        reactivate_durable_blob_reference_tx(
            transaction,
            &request.reference_id,
            request.created_at_ms,
            request.retain_until_ms,
        )?;
        stored = load_durable_blob_reference_tx(transaction, &request.reference_id)?;
        if stored.state != "active" || stored.released_at_ms.is_some() {
            return Err(error("durable blob reference reactivation failed"));
        }
    }
    Ok(stored)
}

fn validate_blob_ownership(
    connection: &Connection,
    session_id: Option<&str>,
    project_id: Option<&str>,
) -> Result<(), WorkbenchStoreError> {
    if let Some(session_id) = session_id {
        let session: Option<(Option<String>, String)> = connection
            .query_row(
                "SELECT project_id, status FROM sessions WHERE session_id = ?1",
                [session_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()
            .map_err(|_| error("cannot validate durable blob session owner"))?;
        let Some((session_project_id, status)) = session else {
            return Err(error("durable blob session owner does not exist"));
        };
        if status == "archived" {
            return Err(error("durable blob session owner is archived"));
        }
        if session_project_id.as_deref() != project_id {
            return Err(error("durable blob project owner does not match session"));
        }
    } else if let Some(project_id) = project_id {
        let active: bool = connection
            .query_row(
                "SELECT EXISTS(
                    SELECT 1 FROM projects WHERE project_id = ?1 AND state = 'active'
                 )",
                [project_id],
                |row| row.get(0),
            )
            .map_err(|_| error("cannot validate durable blob project owner"))?;
        if !active {
            return Err(error("durable blob project owner is unavailable"));
        }
    } else {
        return Err(error("durable blob owner is missing"));
    }
    Ok(())
}

fn load_durable_blob_reference(
    connection: &Connection,
    reference_id: &str,
) -> Result<StoredDurableBlobReference, WorkbenchStoreError> {
    let row = connection
        .query_row(
            "SELECT reference.reference_id, reference.content_reference,
                    reference.blob_sha256, blob.bytes,
                    reference.session_id, reference.project_id, reference.kind,
                    reference.media_type, reference.owner_kind, reference.owner_id,
                    reference.metadata_json, reference.metadata_sha256,
                    reference.metadata_bytes, reference.state,
                    reference.created_at_ms, reference.last_accessed_at_ms,
                    reference.retain_until_ms, reference.released_at_ms
             FROM durable_blob_references AS reference
             JOIN durable_blobs AS blob ON blob.sha256 = reference.blob_sha256
             WHERE reference.reference_id = ?1",
            [reference_id],
            |row| {
                Ok((
                    row.get::<_, String>(0)?,
                    row.get::<_, String>(1)?,
                    row.get::<_, String>(2)?,
                    row.get::<_, i64>(3)?,
                    row.get::<_, Option<String>>(4)?,
                    row.get::<_, Option<String>>(5)?,
                    row.get::<_, String>(6)?,
                    row.get::<_, String>(7)?,
                    row.get::<_, String>(8)?,
                    row.get::<_, String>(9)?,
                    row.get::<_, String>(10)?,
                    row.get::<_, String>(11)?,
                    row.get::<_, i64>(12)?,
                    row.get::<_, String>(13)?,
                    row.get::<_, i64>(14)?,
                    row.get::<_, i64>(15)?,
                    row.get::<_, i64>(16)?,
                    row.get::<_, Option<i64>>(17)?,
                ))
            },
        )
        .optional()
        .map_err(|_| error("cannot load durable blob reference"))?
        .ok_or_else(|| error("durable blob reference does not exist"))?;
    let (
        reference_id,
        content_reference,
        sha256,
        bytes,
        session_id,
        project_id,
        kind,
        media_type,
        owner_kind,
        owner_id,
        metadata_json,
        metadata_sha256,
        metadata_bytes,
        state,
        created_at_ms,
        last_accessed_at_ms,
        retain_until_ms,
        released_at_ms,
    ) = row;
    let content_hash = ContentHash {
        sha256,
        bytes: to_u64(bytes, "durable blob byte count")?,
    };
    validate_blob_content_hash(&content_hash)?;
    validate_content_reference(&content_reference, Some(&content_hash.sha256))?;
    let metadata_hash = ContentHash {
        sha256: metadata_sha256,
        bytes: to_u64(metadata_bytes, "durable blob metadata byte count")?,
    };
    if metadata_json.len() > MAX_BLOB_METADATA_BYTES
        || ContentHash::for_bytes(metadata_json.as_bytes()) != metadata_hash
    {
        return Err(error(
            "durable blob reference metadata integrity check failed",
        ));
    }
    let metadata = serde_json::from_str(&metadata_json)
        .map_err(|_| error("durable blob reference metadata JSON is invalid"))?;
    if payload_contains_secret_key(&metadata) {
        return Err(error(
            "durable blob reference metadata contains forbidden fields",
        ));
    }
    validate_persisted_payload(&metadata, "durable blob reference metadata")?;
    Ok(StoredDurableBlobReference {
        reference_id,
        content_reference,
        content_hash,
        session_id,
        project_id,
        kind: parse_durable_blob_kind(&kind)?,
        media_type,
        owner_kind,
        owner_id,
        metadata,
        state,
        created_at_ms: to_u64(created_at_ms, "durable blob creation time")?,
        last_accessed_at_ms: to_u64(last_accessed_at_ms, "durable blob access time")?,
        retain_until_ms: to_u64(retain_until_ms, "durable blob retention time")?,
        released_at_ms: released_at_ms
            .map(|value| to_u64(value, "durable blob release time"))
            .transpose()?,
    })
}

fn load_durable_blob_reference_tx(
    transaction: &Transaction<'_>,
    reference_id: &str,
) -> Result<StoredDurableBlobReference, WorkbenchStoreError> {
    load_durable_blob_reference(transaction, reference_id)
}

fn validate_pinned_image_release(
    project_id: &str,
    reference: &StoredDurableBlobReference,
) -> Result<(), WorkbenchStoreError> {
    if reference.project_id.as_deref() != Some(project_id)
        || reference.session_id.as_deref() != Some(reference.owner_id.as_str())
        || reference.kind != DurableBlobKind::Image
        || reference.owner_kind != "session"
        || !matches!(reference.state.as_str(), "active" | "released")
    {
        return Err(error("pinned image Blob release scope is invalid"));
    }
    Ok(())
}

fn release_durable_blob_reference_tx(
    transaction: &Transaction<'_>,
    reference_id: &str,
    released_at_ms: u64,
) -> Result<StoredDurableBlobReference, WorkbenchStoreError> {
    let existing = load_durable_blob_reference_tx(transaction, reference_id)?;
    if existing.state == "released" {
        return Ok(existing);
    }
    let minimum_retention = released_at_ms
        .checked_add(MIN_BLOB_RETENTION_MS)
        .ok_or_else(|| error("durable blob release time is out of range"))?;
    transaction
        .execute(
            "UPDATE durable_blob_references
             SET state = 'released', released_at_ms = ?1,
                 retain_until_ms = CASE
                     WHEN retain_until_ms < ?2 THEN ?2 ELSE retain_until_ms END
             WHERE reference_id = ?3 AND state = 'active'",
            params![
                to_i64(released_at_ms, "durable blob release time")?,
                to_i64(minimum_retention, "durable blob undo retention time")?,
                reference_id
            ],
        )
        .map_err(|_| error("cannot release durable blob reference"))?;
    transaction
        .execute(
            "UPDATE durable_blobs SET retain_until_ms = CASE
                WHEN retain_until_ms < ?1 THEN ?1 ELSE retain_until_ms END
             WHERE sha256 = ?2",
            params![
                to_i64(minimum_retention, "durable blob undo retention time")?,
                existing.content_hash.sha256
            ],
        )
        .map_err(|_| error("cannot retain released durable blob"))?;
    load_durable_blob_reference_tx(transaction, reference_id)
}

fn reactivate_durable_blob_reference_tx(
    transaction: &Transaction<'_>,
    reference_id: &str,
    accessed_at_ms: u64,
    retain_until_ms: u64,
) -> Result<(), WorkbenchStoreError> {
    let accessed_at_ms = to_i64(accessed_at_ms, "durable blob reactivation time")?;
    let retain_until_ms = to_i64(retain_until_ms, "durable blob reactivation retention time")?;
    let updated = transaction
        .execute(
            "UPDATE durable_blob_references
             SET state = 'active', released_at_ms = NULL,
                 last_accessed_at_ms = CASE
                     WHEN last_accessed_at_ms < ?1 THEN ?1 ELSE last_accessed_at_ms END,
                 retain_until_ms = CASE
                     WHEN retain_until_ms < ?2 THEN ?2 ELSE retain_until_ms END
             WHERE reference_id = ?3 AND state = 'released'",
            params![accessed_at_ms, retain_until_ms, reference_id],
        )
        .map_err(|_| error("cannot reactivate durable blob reference"))?;
    if updated != 1 {
        return Err(error("durable blob reference reactivation lost its state"));
    }
    transaction
        .execute(
            "UPDATE durable_blobs SET retain_until_ms = CASE
                WHEN retain_until_ms < ?1 THEN ?1 ELSE retain_until_ms END
             WHERE sha256 = (
                SELECT blob_sha256 FROM durable_blob_references WHERE reference_id = ?2
             )",
            params![retain_until_ms, reference_id],
        )
        .map_err(|_| error("cannot retain reactivated durable blob"))?;
    Ok(())
}

fn parse_durable_blob_kind(value: &str) -> Result<DurableBlobKind, WorkbenchStoreError> {
    match value {
        "command-output" => Ok(DurableBlobKind::CommandOutput),
        "patch" => Ok(DurableBlobKind::Patch),
        "image" => Ok(DurableBlobKind::Image),
        "diagnostic" => Ok(DurableBlobKind::Diagnostic),
        "workspace-edit" => Ok(DurableBlobKind::WorkspaceEdit),
        "artifact" => Ok(DurableBlobKind::Artifact),
        _ => Err(error("durable blob kind is invalid")),
    }
}

fn validate_content_reference(
    value: &str,
    expected_sha256: Option<&str>,
) -> Result<(), WorkbenchStoreError> {
    let Some((namespace, sha256)) = value.rsplit_once(":sha256:") else {
        return Err(error("durable blob content reference is invalid"));
    };
    if namespace.is_empty()
        || namespace.len() > 64
        || !namespace
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
        || sha256.len() != 64
        || !sha256
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
        || expected_sha256.is_some_and(|expected| expected != sha256)
    {
        return Err(error("durable blob content reference is invalid"));
    }
    Ok(())
}

fn validate_pinned_context_reference(
    value: &str,
    prefix: &str,
    label: &str,
) -> Result<(), WorkbenchStoreError> {
    validate_content_reference(value, None)?;
    if !value.starts_with(prefix) {
        return Err(error(format!("{label} is invalid")));
    }
    Ok(())
}

fn validate_media_type(value: &str) -> Result<(), WorkbenchStoreError> {
    if value.is_empty()
        || value.len() > 256
        || !value.is_ascii()
        || !value.contains('/')
        || value.bytes().any(|byte| byte.is_ascii_control())
    {
        return Err(error("durable blob media type is invalid"));
    }
    Ok(())
}

fn blob_file_error(cause: BlobFileError) -> WorkbenchStoreError {
    error(format!(
        "durable blob file operation failed ({})",
        cause.code
    ))
}

fn validate_compaction_checkpoint_binding(
    descriptor: &CompactionCheckpointDescriptor,
    review: &CompactionCheckpointReview,
) -> Result<(), WorkbenchStoreError> {
    activate_review(review, review.through_sequence, &review.source_context_hash)
        .map_err(|_| error("compaction checkpoint review is invalid"))?;
    validate_identifier(&review.session_id, "compaction session ID")?;
    validate_identifier(&review.checkpoint_id, "compaction checkpoint ID")?;
    validate_compaction_review_id(&review.review_id)?;
    validate_lower_sha256(
        &review.source_context_hash,
        "compaction source context hash",
    )?;
    validate_content_reference(&descriptor.object_reference, None)?;
    if !descriptor
        .object_reference
        .starts_with("session-compaction-checkpoint:sha256:")
        || descriptor.schema_version != COMPACTION_STORE_SCHEMA_VERSION
        || descriptor.session_id != review.session_id
        || descriptor.checkpoint_id != review.checkpoint_id
        || descriptor.review_id != review.review_id
        || descriptor.state != "review-persisted"
        || !descriptor.original_event_history_authoritative
    {
        return Err(error("compaction checkpoint descriptor is invalid"));
    }
    Ok(())
}

fn validate_compaction_review_id(value: &str) -> Result<(), WorkbenchStoreError> {
    let Some(hash) = value.strip_prefix("compaction-review:sha256:") else {
        return Err(error("compaction review ID is invalid"));
    };
    validate_lower_sha256(hash, "compaction review hash")
}

fn validate_lower_sha256(value: &str, label: &str) -> Result<(), WorkbenchStoreError> {
    if value.len() != 64
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || matches!(byte, b'a'..=b'f'))
    {
        return Err(error(format!("{label} is invalid")));
    }
    Ok(())
}

fn validate_identifier(value: &str, label: &str) -> Result<(), WorkbenchStoreError> {
    if value.is_empty()
        || value.len() > 128
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
    {
        return Err(error(format!("{label} is invalid")));
    }
    validate_persisted_text(value, label)?;
    Ok(())
}

fn validate_event_identifier(value: &str, label: &str) -> Result<(), WorkbenchStoreError> {
    if value.is_empty() || value.len() > 256 || value.bytes().any(|byte| byte.is_ascii_control()) {
        return Err(error(format!("{label} is invalid")));
    }
    validate_persisted_text(value, label)?;
    Ok(())
}

fn validate_event_kind(value: &str) -> Result<(), WorkbenchStoreError> {
    if value.is_empty()
        || value.len() > 128
        || !value.bytes().all(|byte| {
            byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'.' | b'-')
        })
    {
        return Err(error("event kind is invalid"));
    }
    Ok(())
}

fn validate_trust_review_id(value: &str) -> Result<(), WorkbenchStoreError> {
    let Some(hash) = value.strip_prefix("project-trust-review:sha256:") else {
        return Err(error("project trust review ID is invalid"));
    };
    if hash.len() != 64
        || !hash
            .bytes()
            .all(|byte| byte.is_ascii_digit() || matches!(byte, b'a'..=b'f'))
    {
        return Err(error("project trust review ID is invalid"));
    }
    Ok(())
}

fn validate_session_search_term(value: &str, label: &str) -> Result<(), WorkbenchStoreError> {
    if value.is_empty()
        || value.len() > MAX_SESSION_SEARCH_TERM_BYTES
        || value.bytes().any(|byte| byte.is_ascii_control())
    {
        return Err(error(format!("{label} is invalid")));
    }
    validate_persisted_text(value, label)
}

fn session_search_like_pattern(value: &str) -> String {
    let mut escaped = String::with_capacity(value.len().saturating_add(2));
    escaped.push('%');
    for character in value.chars() {
        match character {
            '\\' | '%' | '_' => {
                escaped.push('\\');
                escaped.push(character);
            }
            _ => escaped.push(character),
        }
    }
    escaped.push('%');
    escaped
}

fn contains_case_insensitive(haystack: &str, needle: &str) -> bool {
    haystack
        .to_ascii_lowercase()
        .contains(&needle.to_ascii_lowercase())
}

fn parse_session_search_cursor(cursor: &str) -> Result<(i64, String), WorkbenchStoreError> {
    let Some(rest) = cursor.strip_prefix("after:") else {
        return Err(error("session search cursor is invalid"));
    };
    let Some((timestamp, session_id)) = rest.split_once(':') else {
        return Err(error("session search cursor is invalid"));
    };
    let timestamp = timestamp
        .parse::<i64>()
        .map_err(|_| error("session search cursor is invalid"))?;
    if timestamp < 0 {
        return Err(error("session search cursor is invalid"));
    }
    validate_identifier(session_id, "session search cursor session ID")?;
    if format!("after:{timestamp}:{session_id}") != cursor {
        return Err(error("session search cursor is non-canonical"));
    }
    Ok((timestamp, session_id.to_owned()))
}

fn to_i64(value: u64, label: &str) -> Result<i64, WorkbenchStoreError> {
    i64::try_from(value).map_err(|_| error(format!("{label} is out of range")))
}

fn to_u64(value: i64, label: &str) -> Result<u64, WorkbenchStoreError> {
    u64::try_from(value).map_err(|_| error(format!("{label} is out of range")))
}

fn to_u64_sql(value: i64, label: &str) -> rusqlite::Result<u64> {
    u64::try_from(value).map_err(|_| {
        rusqlite::Error::ToSqlConversionFailure(Box::new(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            label.to_owned(),
        )))
    })
}

fn parse_session_mode(value: &str) -> rusqlite::Result<StoredSessionMode> {
    match value {
        "chat" => Ok(StoredSessionMode::Chat),
        "work" => Ok(StoredSessionMode::Work),
        _ => Err(rusqlite::Error::ToSqlConversionFailure(Box::new(
            std::io::Error::new(std::io::ErrorKind::InvalidData, "session mode is invalid"),
        ))),
    }
}

fn parse_session_lineage(value: &str) -> rusqlite::Result<StoredSessionLineage> {
    match value {
        "new" => Ok(StoredSessionLineage::New),
        "resume" => Ok(StoredSessionLineage::Resume),
        "fork" => Ok(StoredSessionLineage::Fork),
        _ => Err(rusqlite::Error::ToSqlConversionFailure(Box::new(
            std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                "session lineage is invalid",
            ),
        ))),
    }
}

fn canonical_store_root(value: &str) -> Result<PathBuf, WorkbenchStoreError> {
    if value.is_empty()
        || value.len() > 4 * 1024
        || value.bytes().any(|byte| byte.is_ascii_control())
    {
        return Err(error("canonical project root is invalid"));
    }
    validate_persisted_text(value, "canonical project root")?;
    let path = Path::new(value);
    let metadata =
        fs::symlink_metadata(path).map_err(|_| error("canonical project root is unavailable"))?;
    if metadata.file_type().is_symlink() || !metadata.is_dir() {
        return Err(error("canonical project root is unsafe"));
    }
    path.canonicalize()
        .map_err(|_| error("canonical project root is unavailable"))
}

fn validate_identity_text(value: &str, label: &str) -> Result<(), WorkbenchStoreError> {
    if value.is_empty() || value.len() > 256 || value.bytes().any(|byte| byte.is_ascii_control()) {
        return Err(error(format!("{label} is invalid")));
    }
    validate_persisted_text(value, label)?;
    Ok(())
}

fn validate_content_hash(value: &ContentHash, label: &str) -> Result<(), WorkbenchStoreError> {
    if value.bytes == 0
        || value.sha256.len() != 64
        || !value
            .sha256
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
        return Err(error(format!("{label} is invalid")));
    }
    Ok(())
}

fn validate_blob_content_hash(value: &ContentHash) -> Result<(), WorkbenchStoreError> {
    if value.bytes > MAX_BLOB_BYTES
        || value.sha256.len() != 64
        || !value
            .sha256
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
        return Err(error("durable blob content hash is invalid"));
    }
    Ok(())
}

fn validate_item_payload(payload: &serde_json::Value) -> Result<String, WorkbenchStoreError> {
    if payload_contains_secret_key(payload) {
        return Err(error("item payload contains a credential-like field"));
    }
    let redacted_payload = redact_item_payload(payload);
    if payload_contains_secret_key(&redacted_payload) {
        return Err(error("item payload contains a credential-like field"));
    }
    let redacted = serde_json::to_string(&redacted_payload)
        .map_err(|_| error("cannot serialize redacted item payload"))?;
    if redacted.len() > MAX_EVENT_BYTES {
        return Err(error("item payload exceeds size limit"));
    }
    Ok(redacted)
}

fn validate_persisted_item_payload(payload: &serde_json::Value) -> Result<(), WorkbenchStoreError> {
    if payload_contains_secret_key(payload) || redact_item_payload(payload) != *payload {
        return Err(error(
            "persisted item payload contains an unredacted secret",
        ));
    }
    Ok(())
}

fn validate_persisted_text(value: &str, label: &str) -> Result<(), WorkbenchStoreError> {
    if crate::output_redaction::redact_complete(value) != value {
        return Err(error(format!("{label} contains a secret-like value")));
    }
    Ok(())
}

fn validate_persisted_payload(
    payload: &serde_json::Value,
    label: &str,
) -> Result<(), WorkbenchStoreError> {
    match payload {
        serde_json::Value::String(value) => validate_persisted_text(value, label),
        serde_json::Value::Array(values) => values
            .iter()
            .try_for_each(|value| validate_persisted_payload(value, label)),
        serde_json::Value::Object(values) => {
            for (key, value) in values {
                if secret_field_name(key) {
                    return Err(error(format!("{label} contains a credential-like field")));
                }
                validate_persisted_text(key, label)?;
                validate_persisted_payload(value, label)?;
            }
            Ok(())
        }
        serde_json::Value::Null | serde_json::Value::Bool(_) | serde_json::Value::Number(_) => {
            Ok(())
        }
    }
}

fn redact_item_payload(payload: &serde_json::Value) -> serde_json::Value {
    match payload {
        serde_json::Value::String(value) => {
            serde_json::Value::String(crate::output_redaction::redact_complete(value))
        }
        serde_json::Value::Array(values) => {
            serde_json::Value::Array(values.iter().map(redact_item_payload).collect())
        }
        serde_json::Value::Object(values) => serde_json::Value::Object(
            values
                .iter()
                .map(|(key, value)| (key.clone(), redact_item_payload(value)))
                .collect(),
        ),
        other => other.clone(),
    }
}

fn payload_contains_secret_key(payload: &serde_json::Value) -> bool {
    match payload {
        serde_json::Value::Object(values) => values
            .iter()
            .any(|(key, value)| secret_field_name(key) || payload_contains_secret_key(value)),
        serde_json::Value::Array(values) => values.iter().any(payload_contains_secret_key),
        _ => false,
    }
}

fn secret_field_name(key: &str) -> bool {
    let normalized = key
        .chars()
        .filter(|character| character.is_ascii_alphanumeric())
        .flat_map(char::to_lowercase)
        .collect::<String>();
    matches!(
        normalized.as_str(),
        "apikey"
            | "authorization"
            | "authorizationheader"
            | "accesstoken"
            | "refreshtoken"
            | "idtoken"
            | "authtoken"
            | "token"
            | "jwt"
            | "password"
            | "passwd"
            | "secret"
            | "privatekey"
            | "cookie"
            | "securestoragevalue"
            | "securestorageplaintext"
    ) || normalized.ends_with("apikey")
        || normalized.ends_with("accesstoken")
        || normalized.ends_with("refreshtoken")
        || normalized.ends_with("idtoken")
        || normalized.ends_with("authtoken")
        || normalized.ends_with("tokenvalue")
        || normalized.ends_with("jwtvalue")
        || normalized.ends_with("clientsecret")
        || normalized.ends_with("secretkey")
        || normalized.ends_with("privatekey")
        || normalized.ends_with("password")
        || normalized.ends_with("passwd")
        || normalized.ends_with("cookie")
}

fn portable_redact_string(
    value: &str,
    redacted_value_count: &mut u64,
    excluded_paths: &[String],
) -> String {
    let mut redacted = crate::output_redaction::redact_complete(value);
    for path in excluded_paths {
        if path.is_empty() {
            continue;
        }
        redacted = redacted.replace(path, "[REDACTED_PATH]");
        let alternate = if path.contains('\\') {
            path.replace('\\', "/")
        } else {
            path.replace('/', "\\")
        };
        if alternate != *path {
            redacted = redacted.replace(&alternate, "[REDACTED_PATH]");
        }
    }
    if redacted != value {
        *redacted_value_count = redacted_value_count.saturating_add(1);
    }
    redacted
}

fn portable_excluded_key(key: &str) -> bool {
    let normalized = key
        .chars()
        .filter(|character| character.is_ascii_alphanumeric())
        .flat_map(char::to_lowercase)
        .collect::<String>();
    matches!(
        normalized.as_str(),
        "apikey"
            | "authorization"
            | "authorizationheader"
            | "accesstoken"
            | "refreshtoken"
            | "password"
            | "secret"
            | "privatekey"
            | "cookie"
            | "encryptedreasoning"
            | "hiddenthinking"
            | "rawreasoning"
            | "responseid"
            | "providerresponseid"
            | "cachehandle"
            | "continuationtoken"
            | "artifact"
            | "contentreference"
            | "artifactreference"
            | "environment"
            | "environmentidentity"
    ) || normalized.contains("encryptedreasoning")
        || normalized.contains("hiddenthinking")
        || normalized.contains("rawreasoning")
        || normalized.ends_with("responseid")
        || normalized.ends_with("responseids")
        || normalized.contains("cachehandle")
        || normalized.contains("continuationtoken")
        || matches!(
            normalized.as_str(),
            "artifacts" | "contentrefs" | "artifactrefs"
        )
        || ((normalized.ends_with("reference") || normalized.ends_with("references"))
            && normalized != "sourcereference")
}

fn portable_redact_value(
    value: &Value,
    redacted_value_count: &mut u64,
    excluded_field_count: &mut u64,
    excluded_paths: &[String],
) -> Value {
    match value {
        Value::String(value) => Value::String(portable_redact_string(
            value,
            redacted_value_count,
            excluded_paths,
        )),
        Value::Array(values) => Value::Array(
            values
                .iter()
                .map(|value| {
                    portable_redact_value(
                        value,
                        redacted_value_count,
                        excluded_field_count,
                        excluded_paths,
                    )
                })
                .collect(),
        ),
        Value::Object(values) => {
            let mut redacted = serde_json::Map::new();
            for (key, value) in values {
                if portable_excluded_key(key) {
                    *excluded_field_count = excluded_field_count.saturating_add(1);
                    continue;
                }
                redacted.insert(
                    key.clone(),
                    portable_redact_value(
                        value,
                        redacted_value_count,
                        excluded_field_count,
                        excluded_paths,
                    ),
                );
            }
            Value::Object(redacted)
        }
        other => other.clone(),
    }
}

fn validate_portable_session_package(
    package: &PortableSessionPackage,
) -> Result<u64, WorkbenchStoreError> {
    if package.schema_version != "aegisy-portable-session/0.1"
        || package.content.schema_version != "aegisy-portable-session-content/0.1"
    {
        return Err(coded_error(
            "portable-session-version-unsupported",
            "portable session schema version is unsupported",
        ));
    }
    validate_identifier(
        &package.content.source_session_id,
        "portable source session ID",
    )?;
    validate_text(&package.content.title, 256, "portable session title")?;
    if package.content.items.len() > MAX_PORTABLE_SESSION_ITEMS {
        return Err(coded_error(
            "portable-session-too-large",
            "portable session item limit exceeded",
        ));
    }
    let content_json = serde_json::to_vec(&package.content)
        .map_err(|_| error("cannot serialize portable session content"))?;
    let expected_hash = ContentHash::for_bytes(&content_json);
    if package.content_hash != expected_hash {
        return Err(coded_error(
            "portable-session-integrity-failed",
            "portable session content hash does not match",
        ));
    }
    let mut source_item_ids = BTreeSet::new();
    for (index, item) in package.content.items.iter().enumerate() {
        if item.source_sequence != index as u64 + 1 {
            return Err(coded_error(
                "portable-session-sequence-invalid",
                "portable session item sequence is invalid",
            ));
        }
        validate_identifier(&item.source_item_id, "portable source item ID")?;
        if !source_item_ids.insert(&item.source_item_id) {
            return Err(coded_error(
                "portable-session-item-duplicate",
                "portable session item ID is duplicated",
            ));
        }
        validate_event_identifier(&item.item_kind, "portable item kind")?;
        validate_event_identifier(&item.role, "portable item role")?;
        validate_event_identifier(&item.state, "portable item state")?;
        let sanitized_json = validate_item_payload(&item.payload)?;
        let sanitized: Value = serde_json::from_str(&sanitized_json)
            .map_err(|_| error("portable item payload is invalid"))?;
        let mut redacted_count = 0_u64;
        let mut excluded_count = 0_u64;
        let portable =
            portable_redact_value(&sanitized, &mut redacted_count, &mut excluded_count, &[]);
        if sanitized != item.payload
            || portable != item.payload
            || redacted_count != 0
            || excluded_count != 0
        {
            return Err(coded_error(
                "portable-session-redaction-required",
                "portable session contains unredacted or non-portable fields",
            ));
        }
    }
    let package_json = serde_json::to_vec(package)
        .map_err(|_| error("cannot serialize portable session package"))?;
    if package_json.len() > MAX_PORTABLE_SESSION_BYTES {
        return Err(coded_error(
            "portable-session-too-large",
            "portable session package exceeds size limit",
        ));
    }
    Ok(package_json.len() as u64)
}

fn payload_has_path_metadata(value: &Value) -> bool {
    match value {
        Value::Object(values) => values.iter().any(|(key, value)| {
            matches!(
                key.to_ascii_lowercase().as_str(),
                "path" | "cwd" | "file" | "source_path" | "target_path"
            ) || payload_has_path_metadata(value)
        }),
        Value::Array(values) => values.iter().any(payload_has_path_metadata),
        _ => false,
    }
}

fn portable_content_categories(package: &PortableSessionPackage) -> (Vec<String>, Vec<String>) {
    let mut categories = BTreeSet::from(["session-metadata".to_owned()]);
    for item in &package.content.items {
        if matches!(item.role.as_str(), "user" | "assistant") {
            categories.insert("conversation-transcript".into());
        }
        if item.item_kind.contains("command") {
            categories.insert("command-output".into());
        }
        if item.item_kind.contains("file")
            || item.item_kind.contains("patch")
            || item.item_kind.contains("change")
        {
            categories.insert("code-or-diff".into());
        }
        if payload_has_path_metadata(&item.payload) {
            categories.insert("path-metadata".into());
        }
    }
    let mut warnings = Vec::new();
    for category in &categories {
        match category.as_str() {
            "conversation-transcript" => {
                push_projection_issue(&mut warnings, "portable-export-includes-transcript")
            }
            "command-output" => {
                push_projection_issue(&mut warnings, "portable-export-includes-command-output")
            }
            "code-or-diff" => {
                push_projection_issue(&mut warnings, "portable-export-includes-code-or-diff")
            }
            "path-metadata" => {
                push_projection_issue(&mut warnings, "portable-export-includes-paths")
            }
            _ => {}
        }
    }
    if package.redacted_value_count > 0 {
        push_projection_issue(&mut warnings, "portable-export-redactions-applied");
    }
    if package.excluded_field_count > 0 {
        push_projection_issue(&mut warnings, "portable-export-local-fields-excluded");
    }
    (categories.into_iter().collect(), warnings)
}

fn portable_export_preview(
    package: &PortableSessionPackage,
) -> Result<PortableSessionExportPreview, WorkbenchStoreError> {
    let package_bytes = validate_portable_session_package(package)?;
    let (content_categories, warnings) = portable_content_categories(package);
    Ok(PortableSessionExportPreview {
        schema_version: "portable-session-export-preview/0.1".into(),
        source_session_id: package.content.source_session_id.clone(),
        mode: package.content.mode,
        title: package.content.title.clone(),
        item_count: package.content.items.len() as u64,
        package_bytes,
        package_hash: package.content_hash.clone(),
        redacted_value_count: package.redacted_value_count,
        excluded_field_count: package.excluded_field_count,
        content_categories,
        warnings,
        blocking_reasons: Vec::new(),
    })
}

fn validate_text(value: &str, max_bytes: usize, label: &str) -> Result<(), WorkbenchStoreError> {
    if value.is_empty() || value.len() > max_bytes || value.chars().any(char::is_control) {
        return Err(error(format!("{label} is invalid")));
    }
    validate_persisted_text(value, label)?;
    Ok(())
}

fn canonical_data_root(data_root: &Path) -> Result<PathBuf, WorkbenchStoreError> {
    let metadata = fs::symlink_metadata(data_root).map_err(|_| {
        coded_error(
            "workbench-data-root-unavailable",
            "workbench data root is unavailable",
        )
    })?;
    if metadata.file_type().is_symlink() || !metadata.is_dir() {
        return Err(coded_error(
            "workbench-data-root-unsafe",
            "workbench data root is unsafe",
        ));
    }
    let canonical = data_root.canonicalize().map_err(|_| {
        coded_error(
            "workbench-data-root-unavailable",
            "workbench data root is unavailable",
        )
    })?;
    secure_directory(&canonical)?;
    Ok(canonical)
}

#[cfg(unix)]
fn secure_directory(path: &Path) -> Result<(), WorkbenchStoreError> {
    use std::os::unix::fs::PermissionsExt;
    fs::set_permissions(path, fs::Permissions::from_mode(0o700))
        .map_err(|_| error("cannot secure workbench data root"))
}

#[cfg(not(unix))]
fn secure_directory(_path: &Path) -> Result<(), WorkbenchStoreError> {
    Ok(())
}

#[cfg(unix)]
fn secure_file(path: &Path) -> Result<(), WorkbenchStoreError> {
    use std::os::unix::fs::PermissionsExt;
    fs::set_permissions(path, fs::Permissions::from_mode(0o600))
        .map_err(|_| error("cannot secure workbench database"))
}

#[cfg(not(unix))]
fn secure_file(_path: &Path) -> Result<(), WorkbenchStoreError> {
    Ok(())
}

fn error(message: impl Into<String>) -> WorkbenchStoreError {
    WorkbenchStoreError {
        code: "workbench-store-error".into(),
        message: message.into(),
    }
}

fn coded_error(code: impl Into<String>, message: impl Into<String>) -> WorkbenchStoreError {
    WorkbenchStoreError {
        code: code.into(),
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::background_job::{JobRetryPolicy, JobSchedule, JobScheduleKind};
    use crate::background_notification::{
        BackgroundNotificationIntent, BackgroundNotificationKind,
    };
    use crate::background_recovery_decision::BackgroundRecoveryDecision;
    use crate::background_scheduler::BackgroundJobScheduler;
    use crate::background_scheduler_lease::{
        BackgroundSchedulerLeaseRebind, BackgroundSchedulerLeaseReleaseReason,
    };
    use crate::git_commit_transaction::{
        GitCommitHookPolicy, GitCommitIdentity, GitCommitMessageSource, GitCommitSigningPolicy,
    };
    use crate::git_workflow_authorization::{
        authorization_requirement, verify_git_workflow_authorization, GitWorkflowAuthorizedAction,
    };
    use crate::git_workflow_state::{
        GitMergeMode, GitWorkflowCommitMetadata, GitWorkflowExecutionAttempt, GitWorkflowHazards,
        GitWorkflowPlan, GitWorkflowRequest, GitWorkflowRisk,
    };
    use crate::session_compaction::{create_review, CompactionSummary};
    use crate::session_compaction_store::CompactionCheckpointStore;
    use crate::turn_trace::{
        AuthorityLabel, CompletionDomain, CompletionEvidence, EvidenceRef, EvidenceSource,
        RedactionSummary, SessionMode as TraceSessionMode, TerminalEvidence, TraceBinding,
        TurnAccess, TurnKind,
    };
    use crate::workbench_migration::{
        create_pre_upgrade_backup_with_available_bytes, migration_backup_manifests,
    };
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::Instant;

    static SEQUENCE: AtomicU64 = AtomicU64::new(0);

    struct Root {
        parent: PathBuf,
        path: PathBuf,
    }

    impl Root {
        fn new(label: &str) -> Self {
            let sequence = SEQUENCE.fetch_add(1, Ordering::Relaxed);
            let parent = std::env::temp_dir().join(format!(
                "aegisy-workbench-store-{label}-{}-{sequence}",
                std::process::id()
            ));
            let path = parent.join("data");
            fs::create_dir_all(&path).unwrap();
            Self {
                parent,
                path: path.canonicalize().unwrap(),
            }
        }
    }

    impl Drop for Root {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.parent);
        }
    }

    fn background_job_identity(prefix: &str, byte: char) -> String {
        format!("{prefix}{}", byte.to_string().repeat(64))
    }

    fn trace_environment_identity() -> String {
        background_job_identity("environment:sha256:", '8')
    }

    const LEGACY_COMPLETED_TURN_TRACE_JSON: &str = r#"{
        "schema_version":"turn-trace/0.1",
        "binding":{
            "session_id":"trace-session",
            "turn_id":"trace-turn",
            "project_id":"trace-project",
            "environment_identity":"environment:sha256:8888888888888888888888888888888888888888888888888888888888888888"
        },
        "events":[{
            "event_id":"legacy-completed-terminal",
            "sequence":1,
            "at_ms":4,
            "payload":{
                "kind":"terminal",
                "state":"completed",
                "evidence":{
                    "workspace_identity":"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
                    "git_state_identity":"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
                    "verification_identity":"sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
                    "observed_verification_count":1,
                    "evidence":{
                        "authority":"observed",
                        "source":"test-runner",
                        "identity":"sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
                        "observed_at_ms":4
                    }
                },
                "redaction":{
                    "content_included":false,
                    "raw_bytes":0,
                    "retained_bytes":0,
                    "redacted_fields":0,
                    "omitted_fields":0
                }
            }
        }]
    }"#;

    const LEGACY_COMPLETED_TURN_TRACE_IDENTITY: &str =
        "turn-trace:sha256:c3df0de4005f5135500fc4c2fe313d2f408b95e4f9ec1a38c75bda018304c4ea";

    const LEGACY_FAILED_TURN_TRACE_JSON: &str = r#"{
        "schema_version":"turn-trace/0.1",
        "binding":{
            "session_id":"trace-session",
            "turn_id":"trace-turn",
            "project_id":"trace-project",
            "environment_identity":"environment:sha256:8888888888888888888888888888888888888888888888888888888888888888"
        },
        "events":[{
            "event_id":"legacy-failed-terminal",
            "sequence":1,
            "at_ms":4,
            "payload":{
                "kind":"terminal",
                "state":"failed",
                "evidence":{
                    "observed_verification_count":0,
                    "evidence":{
                        "authority":"unknown",
                        "source":"local-runtime"
                    }
                },
                "redaction":{
                    "content_included":false,
                    "raw_bytes":0,
                    "retained_bytes":0,
                    "redacted_fields":0,
                    "omitted_fields":0
                }
            }
        }]
    }"#;

    const LEGACY_FAILED_TURN_TRACE_IDENTITY: &str =
        "turn-trace:sha256:19f87469e6bbb4b3d7c5d838e84cef5b3e381080873abc3eb123b308ec4946b8";

    fn create_turn_trace_fixture(
        store: &mut WorkbenchStore,
        root: &Root,
        session_id: &str,
        turn_id: &str,
    ) {
        create_turn_trace_fixture_with_mode(
            store,
            root,
            session_id,
            turn_id,
            StoredSessionMode::Work,
        );
    }

    fn create_turn_trace_fixture_with_mode(
        store: &mut WorkbenchStore,
        root: &Root,
        session_id: &str,
        turn_id: &str,
        mode: StoredSessionMode,
    ) {
        let project_root = root.parent.join(format!("{session_id}-project"));
        fs::create_dir(&project_root).unwrap();
        let project_root = project_root.canonicalize().unwrap();
        store
            .create_project(StoredProjectCreate {
                project_id: "trace-project".into(),
                root_id: "trace-root".into(),
                canonical_root: project_root.to_string_lossy().into_owned(),
                root_identity:
                    "root:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                        .into(),
                display_name: "Trace project".into(),
                root_access: "read".into(),
                created_at_ms: 1,
            })
            .unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: session_id.into(),
                project_id: Some("trace-project".into()),
                mode,
                title: "Trace session".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: Some(trace_environment_identity()),
                created_at_ms: 2,
            })
            .unwrap();
        store
            .create_turn(StoredTurnCreate {
                turn_id: turn_id.into(),
                session_id: session_id.into(),
                idempotency_key: Some(format!("{turn_id}-key")),
                input_hash: ContentHash::for_bytes(b"metadata-only trace fixture"),
                created_at_ms: 3,
            })
            .unwrap();
    }

    fn failed_turn_trace(
        session_id: &str,
        turn_id: &str,
        terminal_event_id: &str,
        at_ms: u64,
    ) -> TurnTrace {
        let mut trace = TurnTrace::new_legacy(TraceBinding {
            session_id: session_id.into(),
            turn_id: turn_id.into(),
            project_id: Some("trace-project".into()),
            environment_identity: Some(trace_environment_identity()),
        })
        .unwrap();
        trace
            .append(
                terminal_event_id.into(),
                at_ms,
                TracePayload::Terminal {
                    state: TraceTerminalState::Failed,
                    evidence: TerminalEvidence {
                        workspace_identity: None,
                        git_state_identity: None,
                        verification_identity: None,
                        observed_verification_count: 0,
                        evidence: EvidenceRef {
                            authority: AuthorityLabel::Unknown,
                            source: EvidenceSource::LocalRuntime,
                            identity: None,
                            observed_at_ms: None,
                        },
                        completion: None,
                    },
                    redaction: RedactionSummary::metadata_only(),
                },
            )
            .unwrap();
        trace
    }

    fn golden_turn_trace(json: &str, expected_identity: &str) -> TurnTrace {
        let value = serde_json::from_str::<Value>(json).unwrap();
        let trace = serde_json::from_value::<TurnTrace>(value.clone()).unwrap();
        assert_eq!(serde_json::to_value(&trace).unwrap(), value);
        trace.validate_complete().unwrap();
        assert_eq!(trace.metadata_identity().unwrap(), expected_identity);
        trace
    }

    fn observed_trace_evidence(
        source: EvidenceSource,
        identity_byte: char,
        at_ms: u64,
    ) -> EvidenceRef {
        EvidenceRef {
            authority: AuthorityLabel::Observed,
            source,
            identity: Some(background_job_identity("sha256:", identity_byte)),
            observed_at_ms: Some(at_ms),
        }
    }

    fn completed_turn_trace_v2(session_id: &str, turn_id: &str, at_ms: u64) -> TurnTrace {
        let mut trace = TurnTrace::new(TraceBinding {
            session_id: session_id.into(),
            turn_id: turn_id.into(),
            project_id: Some("trace-project".into()),
            environment_identity: Some(trace_environment_identity()),
        })
        .unwrap();
        trace
            .append(
                "trace-intent".into(),
                at_ms,
                TracePayload::Intent {
                    session_mode: TraceSessionMode::Work,
                    turn_kind: TurnKind::ReadOnlyInspection,
                    access: TurnAccess::ReadOnly,
                    intent_identity: background_job_identity("sha256:", '1'),
                    evidence: observed_trace_evidence(EvidenceSource::Runtime, '2', at_ms),
                    redaction: RedactionSummary::metadata_only(),
                },
            )
            .unwrap();
        trace
            .append(
                "trace-terminal".into(),
                at_ms,
                TracePayload::Terminal {
                    state: TraceTerminalState::Completed,
                    evidence: TerminalEvidence {
                        workspace_identity: None,
                        git_state_identity: None,
                        verification_identity: None,
                        observed_verification_count: 0,
                        evidence: observed_trace_evidence(EvidenceSource::Runtime, '3', at_ms),
                        completion: Some(CompletionEvidence {
                            intent_identity: background_job_identity("sha256:", '1'),
                            workspace_change: CompletionDomain::NotApplicable {
                                evidence: observed_trace_evidence(
                                    EvidenceSource::Runtime,
                                    '4',
                                    at_ms,
                                ),
                            },
                            git_change: CompletionDomain::NotApplicable {
                                evidence: observed_trace_evidence(
                                    EvidenceSource::Runtime,
                                    '5',
                                    at_ms,
                                ),
                            },
                            verification: CompletionDomain::Unknown {
                                evidence: EvidenceRef {
                                    authority: AuthorityLabel::Unknown,
                                    source: EvidenceSource::Runtime,
                                    identity: None,
                                    observed_at_ms: None,
                                },
                            },
                        }),
                    },
                    redaction: RedactionSummary::metadata_only(),
                },
            )
            .unwrap();
        trace.validate_complete().unwrap();
        trace
    }

    fn completed_chat_turn_trace_v2(session_id: &str, turn_id: &str, at_ms: u64) -> TurnTrace {
        let intent_identity = background_job_identity("sha256:", '6');
        let mut trace = TurnTrace::new(TraceBinding {
            session_id: session_id.into(),
            turn_id: turn_id.into(),
            project_id: Some("trace-project".into()),
            environment_identity: Some(trace_environment_identity()),
        })
        .unwrap();
        trace
            .append(
                "trace-intent".into(),
                at_ms,
                TracePayload::Intent {
                    session_mode: TraceSessionMode::Chat,
                    turn_kind: TurnKind::Conversation,
                    access: TurnAccess::NonMutating,
                    intent_identity: intent_identity.clone(),
                    evidence: observed_trace_evidence(EvidenceSource::Runtime, '7', at_ms),
                    redaction: RedactionSummary::metadata_only(),
                },
            )
            .unwrap();
        trace
            .append(
                "trace-terminal".into(),
                at_ms,
                TracePayload::Terminal {
                    state: TraceTerminalState::Completed,
                    evidence: TerminalEvidence {
                        workspace_identity: None,
                        git_state_identity: None,
                        verification_identity: None,
                        observed_verification_count: 0,
                        evidence: observed_trace_evidence(EvidenceSource::Runtime, '8', at_ms),
                        completion: Some(CompletionEvidence {
                            intent_identity,
                            workspace_change: CompletionDomain::NotApplicable {
                                evidence: observed_trace_evidence(
                                    EvidenceSource::Runtime,
                                    '9',
                                    at_ms,
                                ),
                            },
                            git_change: CompletionDomain::NotApplicable {
                                evidence: observed_trace_evidence(
                                    EvidenceSource::Runtime,
                                    'a',
                                    at_ms,
                                ),
                            },
                            verification: CompletionDomain::NotApplicable {
                                evidence: observed_trace_evidence(
                                    EvidenceSource::Runtime,
                                    'b',
                                    at_ms,
                                ),
                            },
                        }),
                    },
                    redaction: RedactionSummary::metadata_only(),
                },
            )
            .unwrap();
        trace.validate_complete().unwrap();
        trace
    }

    fn create_background_job_fixture(
        store: &mut WorkbenchStore,
        root: &Root,
        job_id: &str,
    ) -> (BackgroundJobRequest, BackgroundJobState) {
        let project_root = root.parent.join(format!("{job_id}-project"));
        fs::create_dir(&project_root).unwrap();
        let project_root = project_root.canonicalize().unwrap();
        let project_id = format!("{job_id}-project");
        let session_id = format!("{job_id}-session");
        store
            .create_project(StoredProjectCreate {
                project_id: project_id.clone(),
                root_id: "root-1".into(),
                canonical_root: project_root.to_string_lossy().into_owned(),
                root_identity: background_job_identity("root:sha256:", 'a'),
                display_name: "Background job project".into(),
                root_access: "write".into(),
                created_at_ms: 1_000,
            })
            .unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: session_id.clone(),
                project_id: Some(project_id.clone()),
                mode: StoredSessionMode::Work,
                title: "Background job session".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: Some(background_job_identity("environment:sha256:", 'b')),
                created_at_ms: 1_100,
            })
            .unwrap();
        let request = BackgroundJobRequest {
            schema_version: crate::background_job::REQUEST_SCHEMA_VERSION.into(),
            job_id: job_id.into(),
            session_id,
            project_id,
            root_id: "root-1".into(),
            execution_plan_identity: background_job_identity("unified-execution-plan:sha256:", 'c'),
            idempotency_identity: background_job_identity("idempotency:sha256:", 'd'),
            child_task_identity: None,
            schedule: JobSchedule {
                kind: JobScheduleKind::Manual,
                scheduled_for_ms: None,
            },
            retry: JobRetryPolicy {
                max_attempts: 2,
                backoff_ms: 100,
                safe_retry_boundary_identity: Some(background_job_identity(
                    "retry-boundary:sha256:",
                    'e',
                )),
            },
            created_at_ms: 1_200,
        };
        let state = BackgroundJobState::new(&request, 1_200).unwrap();
        (request, state)
    }

    fn create_blob_owners(store: &mut WorkbenchStore, root: &Root) {
        let project_root = root.parent.join("project");
        fs::create_dir(&project_root).unwrap();
        let project_root = project_root.canonicalize().unwrap();
        store
            .create_project(StoredProjectCreate {
                project_id: "blob-project".into(),
                root_id: "root-1".into(),
                canonical_root: project_root.to_string_lossy().into_owned(),
                root_identity: "path:sha256:blob-project-root".into(),
                display_name: "Blob project".into(),
                root_access: "write".into(),
                created_at_ms: 1_000,
            })
            .unwrap();
        for (index, session_id) in ["blob-session-a", "blob-session-b"].iter().enumerate() {
            store
                .create_session(StoredSessionCreate {
                    session_id: (*session_id).into(),
                    project_id: Some("blob-project".into()),
                    mode: StoredSessionMode::Work,
                    title: format!("Blob session {index}"),
                    parent_session_id: None,
                    lineage_kind: StoredSessionLineage::New,
                    environment_identity: Some(format!("environment:sha256:{index}")),
                    created_at_ms: 1_100 + index as u64,
                })
                .unwrap();
        }
    }

    fn create_portable_session_fixture(store: &mut WorkbenchStore, root: &Root) -> Vec<StoredItem> {
        let project_root = root.parent.join("portable-project");
        fs::create_dir(&project_root).unwrap();
        let project_root = project_root.canonicalize().unwrap();
        store
            .create_project(StoredProjectCreate {
                project_id: "portable-project".into(),
                root_id: "portable-root".into(),
                canonical_root: project_root.to_string_lossy().into_owned(),
                root_identity:
                    "root:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                        .into(),
                display_name: "Portable project".into(),
                root_access: "write".into(),
                created_at_ms: 1_000,
            })
            .unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "portable-source".into(),
                project_id: Some("portable-project".into()),
                mode: StoredSessionMode::Work,
                title: "Portable source".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: Some("environment:sha256:source-only".into()),
                created_at_ms: 1_100,
            })
            .unwrap();
        // Simulate a legacy projection that predates the persistence gate. Export
        // must re-redact it, while normal session writes remain rejected.
        store
            .connection
            .execute(
                "UPDATE sessions SET title = ?1 WHERE session_id = 'portable-source'",
                ["API_KEY=sk-12345678901234567890"],
            )
            .unwrap();
        let items = [
            StoredItemAppend {
                session_id: "portable-source".into(),
                turn_id: None,
                item_id: "portable-user".into(),
                item_kind: "message".into(),
                role: "user".into(),
                state: "completed".into(),
                payload: json!({
                    "text": "Review src/main.rs",
                    "path": "src/main.rs",
                    "cwd": project_root.to_string_lossy().into_owned()
                }),
                created_at_ms: 1_101,
            },
            StoredItemAppend {
                session_id: "portable-source".into(),
                turn_id: None,
                item_id: "portable-change".into(),
                item_kind: "file-change".into(),
                role: "agent".into(),
                state: "completed".into(),
                payload: json!({
                    "diff": "@@ -1 +1 @@\n-old\n+new",
                    "provider_response_id": "provider-opaque-response",
                    "encrypted_reasoning": "provider-opaque-reasoning",
                    "cache_handle": "provider-cache-handle",
                    "artifact_reference": "workspace-edit:sha256:local-only",
                    "environment_identity": "environment:sha256:local-only"
                }),
                created_at_ms: 1_102,
            },
            StoredItemAppend {
                session_id: "portable-source".into(),
                turn_id: None,
                item_id: "portable-command".into(),
                item_kind: "command".into(),
                role: "tool".into(),
                state: "completed".into(),
                payload: json!({"output": "tests passed"}),
                created_at_ms: 1_103,
            },
        ]
        .into_iter()
        .map(|request| store.append_item(request).unwrap())
        .collect();
        // Legacy migrated sessions have no registered event source. Keep the
        // fixture intentionally source-less so export exercises redaction of
        // historical projection text without treating it as new writable data.
        store
            .connection
            .execute_batch(
                "DELETE FROM events WHERE session_id = 'portable-source';
                 DELETE FROM session_sequences WHERE session_id = 'portable-source';
                 DELETE FROM session_projection_sources WHERE session_id = 'portable-source';",
            )
            .unwrap();
        items
    }

    fn rehash_portable_package(package: &mut PortableSessionPackage) {
        package.content_hash =
            ContentHash::for_bytes(&serde_json::to_vec(&package.content).unwrap());
    }

    fn create_compaction_event_fixture(
        store: &mut WorkbenchStore,
        root: &Root,
    ) -> (CompactionCheckpointReview, CompactionCheckpointDescriptor) {
        let project_root = root.parent.join("compaction-project");
        fs::create_dir(&project_root).unwrap();
        let project_root = project_root.canonicalize().unwrap();
        store
            .create_project(StoredProjectCreate {
                project_id: "compaction-project".into(),
                root_id: "root-1".into(),
                canonical_root: project_root.to_string_lossy().into_owned(),
                root_identity: "root:sha256:compaction-project".into(),
                display_name: "Compaction project".into(),
                root_access: "write".into(),
                created_at_ms: 1_000,
            })
            .unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "compaction-session".into(),
                project_id: Some("compaction-project".into()),
                mode: StoredSessionMode::Work,
                title: "Compaction session".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: Some("environment:sha256:compaction".into()),
                created_at_ms: 1_100,
            })
            .unwrap();
        let review = create_review(
            "checkpoint-1",
            "compaction-session",
            7,
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            Some("Preserve open tasks"),
            CompactionSummary {
                decisions: vec!["Keep history authoritative".into()],
                unresolved_tasks: vec!["Add Qt review".into()],
                next_actions: vec!["Integrate recovery".into()],
                ..Default::default()
            },
        )
        .unwrap();
        let descriptor = CompactionCheckpointStore::open(&root.path)
            .unwrap()
            .persist(&review)
            .unwrap();
        (review, descriptor)
    }

    #[test]
    fn compaction_checkpoint_event_is_idempotent_and_replay_validated() {
        let root = Root::new("compaction-event");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let (review, descriptor) = create_compaction_event_fixture(&mut store, &root);
        let event = store
            .append_session_compaction_checkpoint_event(&descriptor, &review, 1_200)
            .unwrap();
        let duplicate = store
            .append_session_compaction_checkpoint_event(&descriptor, &review, 1_200)
            .unwrap();
        assert_eq!(duplicate, event);
        assert_eq!(event.event_kind, "session.compaction-checkpointed");
        assert_eq!(event.payload["original_event_history_authoritative"], true);

        drop(store);
        let reopened = WorkbenchStore::open(&root.path).unwrap();
        let events = reopened
            .read_session_events("compaction-session", 0, MAX_EVENT_PAGE)
            .unwrap();
        assert_eq!(
            events
                .iter()
                .filter(|candidate| candidate.event_kind == "session.compaction-checkpointed")
                .count(),
            1
        );
        let candidate = reopened
            .rebuild_session_projection_candidate("compaction-session")
            .unwrap();
        assert!(candidate.source_complete);
        assert!(CompactionCheckpointStore::open(&root.path)
            .unwrap()
            .load("compaction-session", "checkpoint-1")
            .is_ok());
    }

    #[test]
    fn malformed_compaction_event_invalidates_projection_authority() {
        let root = Root::new("compaction-event-tamper");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let (review, descriptor) = create_compaction_event_fixture(&mut store, &root);
        let event = store
            .append_session_compaction_checkpoint_event(&descriptor, &review, 1_200)
            .unwrap();
        let mut payload = event.payload.clone();
        payload["original_event_history_authoritative"] = json!(false);
        let payload_json = serde_json::to_string(&payload).unwrap();
        let payload_hash = ContentHash::for_bytes(payload_json.as_bytes());
        store
            .connection
            .execute(
                "UPDATE events
                 SET payload_json = ?1, payload_sha256 = ?2, payload_bytes = ?3
                 WHERE session_id = ?4 AND event_id = ?5",
                params![
                    payload_json,
                    payload_hash.sha256,
                    payload_hash.bytes as i64,
                    "compaction-session",
                    event.event_id,
                ],
            )
            .unwrap();
        let candidate = store
            .rebuild_session_projection_candidate("compaction-session")
            .unwrap();
        assert!(!candidate.source_complete);
        assert!(candidate
            .issues
            .contains(&"session-compaction-checkpoint-event-invalid".into()));
    }

    #[test]
    fn compaction_revision_lineage_is_durable_and_replay_validated() {
        let root = Root::new("compaction-revision-event");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let (source_review, source_descriptor) = create_compaction_event_fixture(&mut store, &root);
        store
            .append_session_compaction_checkpoint_event(&source_descriptor, &source_review, 1_200)
            .unwrap();
        let revision = create_review(
            "checkpoint-2",
            "compaction-session",
            8,
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            Some("Preserve reviewed decisions"),
            CompactionSummary {
                decisions: vec!["Keep immutable revision lineage".into()],
                next_actions: vec!["Review before activation".into()],
                ..Default::default()
            },
        )
        .unwrap();
        let revision_descriptor = CompactionCheckpointStore::open(&root.path)
            .unwrap()
            .persist(&revision)
            .unwrap();
        let revision_event = store
            .append_session_compaction_checkpoint_revision_event(
                &revision_descriptor,
                &revision,
                &source_descriptor,
                &source_review,
                1_300,
            )
            .unwrap();
        assert_eq!(
            revision_event.payload["supersedes"]["review_id"],
            source_review.review_id
        );
        assert!(
            store
                .rebuild_session_projection_candidate("compaction-session")
                .unwrap()
                .source_complete
        );

        let mut payload = revision_event.payload.clone();
        payload["supersedes"]["review_id"] = json!(
            "compaction-review:sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
        );
        let payload_json = serde_json::to_string(&payload).unwrap();
        let payload_hash = ContentHash::for_bytes(payload_json.as_bytes());
        store
            .connection
            .execute(
                "UPDATE events
                 SET payload_json = ?1, payload_sha256 = ?2, payload_bytes = ?3
                 WHERE session_id = ?4 AND event_id = ?5",
                params![
                    payload_json,
                    payload_hash.sha256,
                    payload_hash.bytes as i64,
                    "compaction-session",
                    revision_event.event_id,
                ],
            )
            .unwrap();
        let candidate = store
            .rebuild_session_projection_candidate("compaction-session")
            .unwrap();
        assert!(!candidate.source_complete);
        assert!(candidate
            .issues
            .contains(&"session-compaction-checkpoint-event-invalid".into()));
    }

    #[test]
    fn portable_export_previews_categories_redacts_secrets_and_rejects_stale_review() {
        let root = Root::new("portable-export");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_portable_session_fixture(&mut store, &root);

        let preview = store
            .preview_portable_session_export("portable-source", 2_000)
            .unwrap();
        assert_eq!(preview.item_count, 3);
        for category in [
            "session-metadata",
            "conversation-transcript",
            "code-or-diff",
            "command-output",
            "path-metadata",
        ] {
            assert!(preview
                .content_categories
                .iter()
                .any(|value| value == category));
        }
        assert!(preview.redacted_value_count > 0);
        assert!(preview.excluded_field_count >= 5);
        assert!(preview
            .warnings
            .contains(&"portable-export-includes-transcript".into()));
        assert!(preview
            .warnings
            .contains(&"portable-export-includes-code-or-diff".into()));
        assert!(preview
            .warnings
            .contains(&"portable-export-redactions-applied".into()));
        assert!(preview
            .warnings
            .contains(&"portable-export-local-fields-excluded".into()));

        let package = store
            .export_portable_session("portable-source", &preview.package_hash, 2_001)
            .unwrap();
        let serialized = serde_json::to_string(&package).unwrap();
        assert!(!serialized.contains("12345678901234567890"));
        assert!(!serialized.contains("provider-opaque-response"));
        assert!(!serialized.contains("provider-opaque-reasoning"));
        assert!(!serialized.contains("provider-cache-handle"));
        assert!(!serialized.contains("workspace-edit:sha256:local-only"));
        assert!(!serialized.contains("environment:sha256:source-only"));
        assert!(!serialized.contains(&root.parent.to_string_lossy().into_owned()));

        store
            .append_item(StoredItemAppend {
                session_id: "portable-source".into(),
                turn_id: None,
                item_id: "portable-late-item".into(),
                item_kind: "message".into(),
                role: "user".into(),
                state: "completed".into(),
                payload: json!({"text": "late mutation"}),
                created_at_ms: 2_002,
            })
            .unwrap();
        // Preserve the legacy no-source fixture after the late mutation so the
        // export path reaches its reviewed-hash stale check.
        store
            .connection
            .execute_batch(
                "DELETE FROM events WHERE session_id = 'portable-source';
                 DELETE FROM session_sequences WHERE session_id = 'portable-source';",
            )
            .unwrap();
        let stale = store
            .export_portable_session("portable-source", &preview.package_hash, 2_003)
            .unwrap_err();
        assert_eq!(stale.code, "portable-session-export-stale");
    }

    #[test]
    fn portable_package_validation_rejects_version_integrity_sequence_duplicate_and_opaque_state() {
        let root = Root::new("portable-validation");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_portable_session_fixture(&mut store, &root);
        let preview = store
            .preview_portable_session_export("portable-source", 2_000)
            .unwrap();
        let package = store
            .export_portable_session("portable-source", &preview.package_hash, 2_001)
            .unwrap();

        let mut wrong_version = package.clone();
        wrong_version.schema_version = "aegisy-portable-session/9.9".into();
        assert_eq!(
            store
                .preview_portable_session_import(&wrong_version, Some("portable-project"))
                .unwrap_err()
                .code,
            "portable-session-version-unsupported"
        );

        let mut wrong_hash = package.clone();
        wrong_hash.content.title = "tampered title".into();
        assert_eq!(
            store
                .preview_portable_session_import(&wrong_hash, Some("portable-project"))
                .unwrap_err()
                .code,
            "portable-session-integrity-failed"
        );

        let mut wrong_sequence = package.clone();
        wrong_sequence.content.items[0].source_sequence = 9;
        rehash_portable_package(&mut wrong_sequence);
        assert_eq!(
            store
                .preview_portable_session_import(&wrong_sequence, Some("portable-project"))
                .unwrap_err()
                .code,
            "portable-session-sequence-invalid"
        );

        let mut duplicate_item = package.clone();
        duplicate_item.content.items[1].source_item_id =
            duplicate_item.content.items[0].source_item_id.clone();
        rehash_portable_package(&mut duplicate_item);
        assert_eq!(
            store
                .preview_portable_session_import(&duplicate_item, Some("portable-project"))
                .unwrap_err()
                .code,
            "portable-session-item-duplicate"
        );

        let mut opaque_state = package;
        opaque_state.content.items[0].payload["provider_response_id"] =
            Value::String("injected-opaque-state".into());
        rehash_portable_package(&mut opaque_state);
        assert_eq!(
            store
                .preview_portable_session_import(&opaque_state, Some("portable-project"))
                .unwrap_err()
                .code,
            "portable-session-redaction-required"
        );
    }

    #[test]
    fn portable_import_remaps_collisions_links_lineage_and_replays_after_restart() {
        let root = Root::new("portable-import-replay");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let source_items = create_portable_session_fixture(&mut store, &root);
        let preview = store
            .preview_portable_session_export("portable-source", 2_000)
            .unwrap();
        let package = store
            .export_portable_session("portable-source", &preview.package_hash, 2_001)
            .unwrap();

        let missing_project = store
            .preview_portable_session_import(&package, None)
            .unwrap();
        assert!(missing_project
            .blocking_reasons
            .contains(&"portable-import-project-required".into()));
        let import_preview = store
            .preview_portable_session_import(&package, Some("portable-project"))
            .unwrap();
        assert!(import_preview.source_session_collision);
        assert_eq!(import_preview.source_item_id_collisions, 3);

        let rejected = store
            .import_portable_session(PortableSessionImportCommand {
                target_session_id: "portable-rejected",
                import_id: "portable-reject-operation",
                package: &package,
                target_project_id: Some("portable-project"),
                reject_source_collisions: true,
                target_environment_identity: Some("environment:sha256:portable-rejected"),
                runtime_binding: None,
                workspace_binding: None,
                imported_at_ms: 2_500,
            })
            .unwrap_err();
        assert_eq!(rejected.code, "portable-import-source-collision");
        assert!(store.load_session("portable-rejected").is_err());

        let receipt = store
            .import_portable_session(PortableSessionImportCommand {
                target_session_id: "portable-imported",
                import_id: "portable-import-operation",
                package: &package,
                target_project_id: Some("portable-project"),
                reject_source_collisions: false,
                target_environment_identity: Some("environment:sha256:portable-imported"),
                runtime_binding: None,
                workspace_binding: None,
                imported_at_ms: 3_000,
            })
            .unwrap();
        assert!(receipt.source_session_collision);
        assert!(receipt.linked_source_session);
        assert_eq!(receipt.imported_items, 3);
        assert_eq!(receipt.session.lineage_kind, StoredSessionLineage::Fork);
        assert_eq!(
            receipt.session.parent_session_id.as_deref(),
            Some("portable-source")
        );
        assert_eq!(
            store.read_session_items("portable-source", 0, 10).unwrap(),
            source_items
        );
        let imported_items = store
            .read_session_items("portable-imported", 0, 10)
            .unwrap();
        assert_eq!(imported_items.len(), source_items.len());
        for (source, imported) in source_items.iter().zip(&imported_items) {
            assert_ne!(source.item_id, imported.item_id);
            assert_eq!(source.item_kind, imported.item_kind);
            assert_eq!(source.role, imported.role);
            assert_eq!(source.state, imported.state);
        }
        let candidate = store
            .rebuild_session_projection_candidate("portable-imported")
            .unwrap();
        assert!(candidate.source_complete, "{:?}", candidate.issues);
        assert!(
            candidate.matches_current_projection,
            "{:?}",
            candidate.issues
        );
        assert!(
            store
                .verify_session_projection("portable-imported")
                .unwrap()
                .consistent
        );

        drop(store);
        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(
            reopened
                .read_session_items("portable-imported", 0, 10)
                .unwrap(),
            imported_items
        );
        let candidate = reopened
            .rebuild_session_projection_candidate("portable-imported")
            .unwrap();
        assert!(candidate.source_complete, "{:?}", candidate.issues);
        assert!(
            candidate.matches_current_projection,
            "{:?}",
            candidate.issues
        );
    }

    #[test]
    fn portable_import_rolls_back_session_items_events_and_source_on_audit_failure() {
        let root = Root::new("portable-import-rollback");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_portable_session_fixture(&mut store, &root);
        let preview = store
            .preview_portable_session_export("portable-source", 2_000)
            .unwrap();
        let package = store
            .export_portable_session("portable-source", &preview.package_hash, 2_001)
            .unwrap();
        store
            .connection
            .execute_batch(
                "CREATE TRIGGER fail_portable_import_audit
                 BEFORE INSERT ON events
                 WHEN NEW.event_kind = 'session.imported'
                 BEGIN
                     SELECT RAISE(ABORT, 'injected portable import audit failure');
                 END;",
            )
            .unwrap();

        assert!(store
            .import_portable_session(PortableSessionImportCommand {
                target_session_id: "portable-rollback-target",
                import_id: "portable-rollback-operation",
                package: &package,
                target_project_id: Some("portable-project"),
                reject_source_collisions: false,
                target_environment_identity: Some("environment:sha256:portable-rollback"),
                runtime_binding: None,
                workspace_binding: None,
                imported_at_ms: 3_000,
            })
            .is_err());
        for (table, owner_column) in [
            ("sessions", "session_id"),
            ("items", "session_id"),
            ("events", "session_id"),
            ("session_projection_sources", "session_id"),
        ] {
            let query = format!("SELECT COUNT(*) FROM {table} WHERE {owner_column} = ?1");
            let count: i64 = store
                .connection
                .query_row(&query, ["portable-rollback-target"], |row| row.get(0))
                .unwrap();
            assert_eq!(count, 0, "{table} retained partial import state");
        }
        assert_eq!(
            store
                .read_session_items("portable-source", 0, 10)
                .unwrap()
                .len(),
            3
        );
    }

    fn blob_request(
        session_id: &str,
        owner_id: &str,
        content: &[u8],
        created_at_ms: u64,
    ) -> DurableBlobWrite {
        let sha256 = sha256_hex(content);
        let content_reference = format!("command-output:sha256:{sha256}");
        DurableBlobWrite {
            reference_id: durable_blob_reference_id(
                Some(session_id),
                Some("blob-project"),
                "session",
                owner_id,
                &content_reference,
            ),
            content_reference,
            session_id: Some(session_id.into()),
            project_id: Some("blob-project".into()),
            kind: DurableBlobKind::CommandOutput,
            media_type: "text/plain; charset=utf-8".into(),
            owner_kind: "session".into(),
            owner_id: owner_id.into(),
            metadata: json!({
                "source_bytes": content.len(),
                "redacted": false,
                "retention": "session-artifact"
            }),
            content: content.to_vec(),
            created_at_ms,
            retain_until_ms: created_at_ms + 7 * MIN_BLOB_RETENTION_MS,
        }
    }

    fn identity() -> GitCommitIdentity {
        GitCommitIdentity {
            name: "Aegisy Store Test".into(),
            email: "store@example.invalid".into(),
            source: "explicit".into(),
            timestamp_seconds: 1_800_000_000,
            timezone: "+0800".into(),
        }
    }

    fn record_and_plan() -> (GitWorkflowRecord, GitWorkflowPlan) {
        let base = "a".repeat(40);
        let target = "b".repeat(40);
        let request = GitWorkflowRequest::Merge {
            target_oid: target.clone(),
            mode: GitMergeMode::NoFastForward,
            commit: Some(GitWorkflowCommitMetadata {
                message: "Store merge".into(),
                message_source: GitCommitMessageSource::User,
                identity: identity(),
            }),
            hook_policy: GitCommitHookPolicy::Disabled,
            signing_policy: GitCommitSigningPolicy::Unsigned,
        };
        let risk = GitWorkflowRisk {
            class: "medium".into(),
            reasons: vec!["combines-history-and-updates-worktree".into()],
            requires_permission: true,
            requires_explicit_approval: true,
        };
        let root_identity = format!("git-root:sha256:{}", "c".repeat(64));
        let common_identity = format!("git-root:sha256:{}", "d".repeat(64));
        let index_state = ContentHash::for_bytes(b"index");
        let record = GitWorkflowRecord {
            schema_version: "git-workflow-record/0.2".into(),
            operation_id: "operation-1".into(),
            project_id: "project-1".into(),
            session_id: "session-1".into(),
            repository_root: "/tmp/workbench-store-repository".into(),
            root_identity: root_identity.clone(),
            git_common_directory_identity: common_identity.clone(),
            request: request.clone(),
            operation_kind: "merge".into(),
            risk: risk.clone(),
            base_head: base.clone(),
            base_branch: Some("topic".into()),
            base_index_tree: Some("e".repeat(40)),
            base_index_state: index_state.clone(),
            target_oids: vec![target],
            predicted_behavior: "merge-commit".into(),
            base_stash_oid: None,
            state: "planned".into(),
            generation: 0,
            visible_dirty_paths: Vec::new(),
            redacted_dirty_path_count: 0,
            conflicts: Vec::new(),
            redacted_conflict_path_count: 0,
            allowed_actions: vec!["start".into(), "cancel".into()],
            created_at_ms: 10,
            updated_at_ms: 10,
            observed_head: base,
            observed_branch: Some("topic".into()),
            observed_operation: None,
            execution: None,
        };
        let plan = GitWorkflowPlan {
            schema_version: "git-workflow-plan/0.2".into(),
            repository_root: record.repository_root.clone(),
            root_identity,
            git_common_directory_identity: common_identity,
            request,
            operation_kind: "merge".into(),
            expected_head: record.observed_head.clone(),
            expected_branch: record.observed_branch.clone(),
            expected_index_tree: record.base_index_tree.clone(),
            expected_index_state: index_state,
            target_oids: record.target_oids.clone(),
            predicted_behavior: "merge-commit".into(),
            base_stash_oid: None,
            visible_dirty_paths: Vec::new(),
            redacted_dirty_path_count: 0,
            pending_editor_path_count: 0,
            live_operation: None,
            risk,
            hazards: GitWorkflowHazards {
                active_hooks: Vec::new(),
                custom_hooks_path: false,
                custom_merge_driver: false,
                custom_filter_driver: false,
                protected_branch: false,
            },
            blocking_reasons: Vec::new(),
        };
        (record, plan)
    }

    fn issue_pair(
        store: &mut WorkbenchStore,
        requirement: &GitWorkflowAuthorizationRequirement,
        now: u64,
    ) -> GitWorkflowAuthorizationEvidence {
        store
            .issue_git_workflow_decision(
                requirement,
                GitWorkflowDecisionKind::Permission,
                "profile-authority",
                "permission-1",
                GitWorkflowDecisionTiming {
                    issued_at_ms: now - 100,
                    expires_at_ms: now + 60_000,
                    observed_at_ms: now,
                },
            )
            .unwrap();
        store
            .issue_git_workflow_decision(
                requirement,
                GitWorkflowDecisionKind::ExplicitApproval,
                "user-authority",
                "approval-1",
                GitWorkflowDecisionTiming {
                    issued_at_ms: now - 100,
                    expires_at_ms: now + 60_000,
                    observed_at_ms: now,
                },
            )
            .unwrap();
        store
            .git_workflow_authorization_evidence(
                requirement,
                "authorization-1",
                "profile-authority",
                "permission-1",
                Some(("user-authority", "approval-1")),
            )
            .unwrap()
    }

    #[test]
    fn opens_wal_schema_and_private_database_and_reopens() {
        let root = Root::new("open");
        let store = WorkbenchStore::open(&root.path).unwrap();
        assert!(store.path().ends_with(DATABASE_FILE));
        let journal: String = store
            .connection
            .query_row("PRAGMA journal_mode", [], |row| row.get(0))
            .unwrap();
        assert_eq!(journal.to_ascii_lowercase(), "wal");
        let wal_autocheckpoint: i64 = store
            .connection
            .pragma_query_value(None, "wal_autocheckpoint", |row| row.get(0))
            .unwrap();
        assert_eq!(wal_autocheckpoint, WAL_AUTOCHECKPOINT_PAGES);
        let journal_size_limit: i64 = store
            .connection
            .pragma_query_value(None, "journal_size_limit", |row| row.get(0))
            .unwrap();
        assert_eq!(journal_size_limit, WAL_JOURNAL_SIZE_LIMIT_BYTES);
        let page_size: i64 = store
            .connection
            .pragma_query_value(None, "page_size", |row| row.get(0))
            .unwrap();
        let max_page_count: i64 = store
            .connection
            .pragma_query_value(None, "max_page_count", |row| row.get(0))
            .unwrap();
        assert!(
            u64::try_from(page_size).unwrap() * u64::try_from(max_page_count).unwrap()
                <= MAX_DATABASE_BYTES
        );
        let version: i64 = store
            .connection
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .unwrap();
        assert_eq!(version, SCHEMA_VERSION);
        let application_id: i64 = store
            .connection
            .pragma_query_value(None, "application_id", |row| row.get(0))
            .unwrap();
        assert_eq!(application_id, APPLICATION_ID);
        drop(store);
        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(reopened.path(), root.path.join(DATABASE_FILE).as_path());
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                fs::metadata(reopened.path()).unwrap().permissions().mode() & 0o777,
                0o600
            );
        }
    }

    #[test]
    fn background_recovery_decision_is_event_backed_idempotent_and_durable() {
        let root = Root::new("background-recovery-decision");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let (request, state) =
            create_background_job_fixture(&mut store, &root, "recovery-decision-job");
        store.create_background_job(&request, &state).unwrap();
        let owner = background_job_identity("scheduler-owner:sha256:", 'f');
        let scheduler = BackgroundJobScheduler::load(&store, owner, 1_250, 10).unwrap();
        let snapshot = scheduler.snapshot().unwrap();

        let stored = store
            .append_background_recovery_decision(&snapshot, &request.job_id, 1_300)
            .unwrap();
        assert_eq!(
            stored.decision.disposition,
            crate::background_recovery_decision::BackgroundRecoveryDisposition::AdmissionReviewRequired
        );
        assert!(!stored.decision.dispatch_authority);
        assert!(!stored.decision.mutation_authority);
        assert!(!stored.decision.automatic_retry);
        assert!(!stored.decision.automatic_approval);
        assert!(!stored.decision.automatic_takeover);
        assert_eq!(stored.decision.lease_state, SchedulerLeaseState::Missing);
        let mut semantic_tamper = stored.decision.clone();
        semantic_tamper.disposition =
            crate::background_recovery_decision::BackgroundRecoveryDisposition::MonitorOwnedProcess;
        assert_eq!(
            semantic_tamper.validate().unwrap_err().code,
            "background-recovery-disposition-invalid"
        );

        let retried = store
            .append_background_recovery_decision(&snapshot, &request.job_id, 1_400)
            .unwrap();
        assert_eq!(retried, stored);
        assert_eq!(
            query_global_count(
                &store.connection,
                "SELECT COUNT(*) FROM events
                 WHERE event_kind = 'background-job.recovery-reviewed'",
                "count"
            )
            .unwrap(),
            1
        );
        drop(store);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(
            reopened
                .latest_background_recovery_decision(&request.job_id)
                .unwrap()
                .unwrap(),
            stored
        );
        assert_eq!(
            reopened.load_background_recovery_decisions().unwrap(),
            [stored]
        );
    }

    #[test]
    fn background_recovery_decision_rejects_forged_snapshot_and_stale_job() {
        let root = Root::new("background-recovery-stale");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let (request, mut state) =
            create_background_job_fixture(&mut store, &root, "recovery-stale-job");
        store.create_background_job(&request, &state).unwrap();
        let owner = background_job_identity("scheduler-owner:sha256:", 'f');
        let scheduler = BackgroundJobScheduler::load(&store, owner, 1_250, 10).unwrap();
        let snapshot = scheduler.snapshot().unwrap();

        let mut forged = snapshot.clone();
        forged.entries[0].dispatch_available = true;
        assert_eq!(
            BackgroundRecoveryDecision::from_snapshot(&forged, &request.job_id, 1_300)
                .unwrap_err()
                .code,
            "background-recovery-snapshot-invalid"
        );

        let queued = state.clone();
        state.start(&request, 1_300).unwrap();
        store
            .update_background_job_state(&request, &queued, &state)
            .unwrap();
        assert_eq!(
            store
                .append_background_recovery_decision(&snapshot, &request.job_id, 1_310)
                .unwrap_err()
                .code,
            "background-recovery-evidence-stale"
        );
        assert!(store
            .latest_background_recovery_decision(&request.job_id)
            .unwrap()
            .is_none());
    }

    #[test]
    fn background_recovery_event_failure_rolls_back_and_tamper_fails_startup() {
        let root = Root::new("background-recovery-rollback");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let (request, state) =
            create_background_job_fixture(&mut store, &root, "recovery-rollback-job");
        store.create_background_job(&request, &state).unwrap();
        let scheduler = BackgroundJobScheduler::load(
            &store,
            background_job_identity("scheduler-owner:sha256:", 'f'),
            1_250,
            10,
        )
        .unwrap();
        let snapshot = scheduler.snapshot().unwrap();
        store
            .connection
            .execute_batch(
                "CREATE TRIGGER fail_background_recovery_event
                 BEFORE INSERT ON events
                 WHEN NEW.event_kind = 'background-job.recovery-reviewed'
                 BEGIN SELECT RAISE(ABORT, 'injected'); END;",
            )
            .unwrap();
        assert!(store
            .append_background_recovery_decision(&snapshot, &request.job_id, 1_300)
            .is_err());
        assert_eq!(
            query_global_count(
                &store.connection,
                "SELECT COUNT(*) FROM events
                 WHERE event_kind = 'background-job.recovery-reviewed'",
                "count"
            )
            .unwrap(),
            0
        );
        store
            .connection
            .execute_batch("DROP TRIGGER fail_background_recovery_event;")
            .unwrap();
        store
            .append_background_recovery_decision(&snapshot, &request.job_id, 1_300)
            .unwrap();

        let payload_json: String = store
            .connection
            .query_row(
                "SELECT payload_json FROM events
                 WHERE event_kind = 'background-job.recovery-reviewed'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        let mut payload: Value = serde_json::from_str(&payload_json).unwrap();
        payload["decision"]["automatic_retry"] = Value::Bool(true);
        let tampered_json = serde_json::to_string(&payload).unwrap();
        let tampered_hash = ContentHash::for_bytes(tampered_json.as_bytes());
        store
            .connection
            .execute(
                "UPDATE events SET payload_json = ?1, payload_sha256 = ?2,
                    payload_bytes = ?3
                 WHERE event_kind = 'background-job.recovery-reviewed'",
                params![
                    tampered_json,
                    tampered_hash.sha256,
                    tampered_hash.bytes as i64
                ],
            )
            .unwrap();
        drop(store);
        assert_eq!(
            WorkbenchStore::open(&root.path).unwrap_err().code,
            "workbench-database-integrity-failed"
        );
    }

    #[test]
    fn background_notification_outbox_is_durable_idempotent_and_paged() {
        let root = Root::new("background-notification-durable");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let (request, mut state) =
            create_background_job_fixture(&mut store, &root, "notification-durable-job");
        store.create_background_job(&request, &state).unwrap();

        let queued = state.clone();
        state.start(&request, 1_300).unwrap();
        store
            .update_background_job_state(&request, &queued, &state)
            .unwrap();
        let running = state.clone();
        let approval = background_job_identity("approval:sha256:", 'f');
        state.wait_for_approval(&request, &approval, 1_400).unwrap();
        store
            .update_background_job_state(&request, &running, &state)
            .unwrap();
        let approval_intent =
            BackgroundNotificationIntent::from_job_state(&request, &state, 1_450).unwrap();
        let approval_record = store
            .enqueue_background_notification(&approval_intent, 1_500)
            .unwrap();
        assert_eq!(
            approval_record.intent.kind,
            BackgroundNotificationKind::ApprovalNeeded
        );

        let waiting = state.clone();
        state
            .resume_after_approval(&request, &approval, 1_600)
            .unwrap();
        store
            .update_background_job_state(&request, &waiting, &state)
            .unwrap();
        let running = state.clone();
        state
            .complete(
                &request,
                &background_job_identity("artifact:sha256:", 'a'),
                &background_job_identity("job-evidence:sha256:", 'b'),
                1_700,
            )
            .unwrap();
        store
            .update_background_job_state(&request, &running, &state)
            .unwrap();
        let completed_intent =
            BackgroundNotificationIntent::from_job_state(&request, &state, 1_750).unwrap();
        let completed_record = store
            .enqueue_background_notification(&completed_intent, 1_800)
            .unwrap();
        assert_eq!(
            store
                .enqueue_background_notification(&approval_intent, 1_850)
                .unwrap(),
            approval_record
        );
        let later_intent =
            BackgroundNotificationIntent::from_job_state(&request, &state, 1_875).unwrap();
        assert_ne!(
            later_intent.intent_identity,
            completed_intent.intent_identity
        );
        assert_eq!(
            later_intent.deduplication_identity,
            completed_intent.deduplication_identity
        );
        assert_eq!(
            store
                .enqueue_background_notification(&later_intent, 1_900)
                .unwrap(),
            completed_record
        );
        assert_eq!(
            query_global_count(
                &store.connection,
                "SELECT COUNT(*) FROM background_notification_outbox",
                "count",
            )
            .unwrap(),
            2
        );
        assert_eq!(
            query_global_count(
                &store.connection,
                "SELECT COUNT(*) FROM events
                 WHERE event_kind = 'background-job.notification-recorded'",
                "count",
            )
            .unwrap(),
            2
        );

        let first = store
            .inspect_background_notifications(&request.session_id, None, 1)
            .unwrap();
        assert_eq!(
            first.notifications.as_slice(),
            std::slice::from_ref(&completed_record)
        );
        assert!(!first.content_included);
        assert!(!first.delivery_mutation_available);
        assert!(!first.platform_delivery_authority);
        let second = store
            .inspect_background_notifications(&request.session_id, first.next_cursor.as_ref(), 1)
            .unwrap();
        assert_eq!(
            second.notifications.as_slice(),
            std::slice::from_ref(&approval_record)
        );
        assert!(second.next_cursor.is_none());
        let mut forged_cursor = first.next_cursor.unwrap();
        forged_cursor.recorded_at_ms += 1;
        assert_eq!(
            store
                .inspect_background_notifications(&request.session_id, Some(&forged_cursor), 1)
                .unwrap_err()
                .code,
            "background-notification-cursor-invalid"
        );
        drop(store);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(
            reopened
                .load_background_notification(&completed_record.intent.intent_identity)
                .unwrap(),
            completed_record
        );
        assert_eq!(
            reopened
                .load_background_notification(&approval_record.intent.intent_identity)
                .unwrap(),
            approval_record
        );
    }

    #[test]
    fn background_notification_rejects_stale_state_and_rolls_back_event_sequence() {
        let root = Root::new("background-notification-rollback");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let (request, mut state) =
            create_background_job_fixture(&mut store, &root, "notification-rollback-job");
        store.create_background_job(&request, &state).unwrap();
        let queued = state.clone();
        state.start(&request, 1_300).unwrap();
        store
            .update_background_job_state(&request, &queued, &state)
            .unwrap();
        let running = state.clone();
        let approval = background_job_identity("approval:sha256:", 'c');
        state.wait_for_approval(&request, &approval, 1_400).unwrap();
        store
            .update_background_job_state(&request, &running, &state)
            .unwrap();
        let stale_intent =
            BackgroundNotificationIntent::from_job_state(&request, &state, 1_450).unwrap();
        let waiting = state.clone();
        state
            .resume_after_approval(&request, &approval, 1_500)
            .unwrap();
        store
            .update_background_job_state(&request, &waiting, &state)
            .unwrap();
        assert_eq!(
            store
                .enqueue_background_notification(&stale_intent, 1_600)
                .unwrap_err()
                .code,
            "background-notification-job-state-stale"
        );

        let running = state.clone();
        state
            .complete(
                &request,
                &background_job_identity("artifact:sha256:", 'd'),
                &background_job_identity("job-evidence:sha256:", 'e'),
                1_700,
            )
            .unwrap();
        store
            .update_background_job_state(&request, &running, &state)
            .unwrap();
        let intent = BackgroundNotificationIntent::from_job_state(&request, &state, 1_750).unwrap();
        let next_sequence_before: i64 = store
            .connection
            .query_row(
                "SELECT next_sequence FROM session_sequences WHERE session_id = ?1",
                [&request.session_id],
                |row| row.get(0),
            )
            .unwrap();
        store
            .connection
            .execute_batch(
                "CREATE TRIGGER fail_background_notification_outbox
                 BEFORE INSERT ON background_notification_outbox
                 BEGIN SELECT RAISE(ABORT, 'injected'); END;",
            )
            .unwrap();
        assert!(store
            .enqueue_background_notification(&intent, 1_800)
            .is_err());
        assert_eq!(
            query_global_count(
                &store.connection,
                "SELECT COUNT(*) FROM background_notification_outbox",
                "count",
            )
            .unwrap(),
            0
        );
        assert_eq!(
            query_global_count(
                &store.connection,
                "SELECT COUNT(*) FROM events
                 WHERE event_kind = 'background-job.notification-recorded'",
                "count",
            )
            .unwrap(),
            0
        );
        let next_sequence_after: i64 = store
            .connection
            .query_row(
                "SELECT next_sequence FROM session_sequences WHERE session_id = ?1",
                [&request.session_id],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(next_sequence_after, next_sequence_before);
        store
            .connection
            .execute_batch("DROP TRIGGER fail_background_notification_outbox;")
            .unwrap();
        store
            .enqueue_background_notification(&intent, 1_800)
            .unwrap();
    }

    #[test]
    fn background_notification_projection_and_lifecycle_tampering_fail_startup() {
        let projection_root = Root::new("background-notification-projection-tamper");
        let mut store = WorkbenchStore::open(&projection_root.path).unwrap();
        let (request, mut state) = create_background_job_fixture(
            &mut store,
            &projection_root,
            "notification-projection-job",
        );
        store.create_background_job(&request, &state).unwrap();
        let queued = state.clone();
        state.start(&request, 1_300).unwrap();
        store
            .update_background_job_state(&request, &queued, &state)
            .unwrap();
        let running = state.clone();
        state
            .complete(
                &request,
                &background_job_identity("artifact:sha256:", 'f'),
                &background_job_identity("job-evidence:sha256:", 'a'),
                1_400,
            )
            .unwrap();
        store
            .update_background_job_state(&request, &running, &state)
            .unwrap();
        let intent = BackgroundNotificationIntent::from_job_state(&request, &state, 1_450).unwrap();
        store
            .enqueue_background_notification(&intent, 1_500)
            .unwrap();
        let later_intent =
            BackgroundNotificationIntent::from_job_state(&request, &state, 1_475).unwrap();
        let later_json = serde_json::to_string(&later_intent).unwrap();
        let later_hash = ContentHash::for_bytes(later_json.as_bytes());
        store
            .connection
            .execute(
                "UPDATE background_notification_outbox SET
                    intent_identity = ?1, intent_json = ?2, intent_sha256 = ?3,
                    intent_bytes = ?4, created_at_ms = ?5
                 WHERE deduplication_identity = ?6",
                params![
                    later_intent.intent_identity,
                    later_json,
                    later_hash.sha256,
                    later_hash.bytes as i64,
                    later_intent.created_at_ms as i64,
                    later_intent.deduplication_identity,
                ],
            )
            .unwrap();
        drop(store);
        assert_eq!(
            WorkbenchStore::open(&projection_root.path)
                .unwrap_err()
                .code,
            "workbench-database-integrity-failed"
        );

        let lifecycle_root = Root::new("background-notification-lifecycle-tamper");
        let mut store = WorkbenchStore::open(&lifecycle_root.path).unwrap();
        let (request, mut state) = create_background_job_fixture(
            &mut store,
            &lifecycle_root,
            "notification-lifecycle-job",
        );
        store.create_background_job(&request, &state).unwrap();
        let queued = state.clone();
        state.start(&request, 1_300).unwrap();
        store
            .update_background_job_state(&request, &queued, &state)
            .unwrap();
        let running = state.clone();
        state
            .complete(
                &request,
                &background_job_identity("artifact:sha256:", 'b'),
                &background_job_identity("job-evidence:sha256:", 'c'),
                1_400,
            )
            .unwrap();
        store
            .update_background_job_state(&request, &running, &state)
            .unwrap();
        let intent = BackgroundNotificationIntent::from_job_state(&request, &state, 1_450).unwrap();
        store
            .enqueue_background_notification(&intent, 1_500)
            .unwrap();
        let lifecycle_json: String = store
            .connection
            .query_row(
                "SELECT payload_json FROM events
                 WHERE event_kind = 'background-job.completed'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        let mut lifecycle: Value = serde_json::from_str(&lifecycle_json).unwrap();
        lifecycle["state_identity"] =
            Value::String(background_job_identity("background-job-state:sha256:", 'd'));
        let tampered_json = serde_json::to_string(&lifecycle).unwrap();
        let tampered_hash = ContentHash::for_bytes(tampered_json.as_bytes());
        store
            .connection
            .execute(
                "UPDATE events SET payload_json = ?1, payload_sha256 = ?2,
                    payload_bytes = ?3 WHERE event_kind = 'background-job.completed'",
                params![
                    tampered_json,
                    tampered_hash.sha256,
                    tampered_hash.bytes as i64,
                ],
            )
            .unwrap();
        drop(store);
        assert_eq!(
            WorkbenchStore::open(&lifecycle_root.path).unwrap_err().code,
            "workbench-database-integrity-failed"
        );
    }

    #[test]
    fn background_scheduler_lease_is_atomic_generation_cas_and_durable() {
        let root = Root::new("background-lease-durable");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let (request, state) =
            create_background_job_fixture(&mut store, &root, "durable-lease-job");
        store.create_background_job(&request, &state).unwrap();
        let lease = BackgroundSchedulerLease::acquire(
            &request,
            &state,
            background_job_identity("scheduler-owner:sha256:", 'f'),
            1_250,
            2_000,
        )
        .unwrap();
        let created = store
            .create_background_scheduler_lease(&request, &state, &lease)
            .unwrap();
        assert_eq!(
            store
                .create_background_scheduler_lease(&request, &state, &lease)
                .unwrap(),
            created
        );

        let previous = lease.clone();
        let mut renewed = lease;
        renewed
            .renew(
                &request,
                &state,
                &background_job_identity("scheduler-owner:sha256:", 'f'),
                1_500,
                2_000,
            )
            .unwrap();
        let renewed_record = store
            .update_background_scheduler_lease(&request, &state, &previous, &renewed)
            .unwrap();
        assert_eq!(renewed_record.lease, renewed);
        assert_eq!(
            store
                .update_background_scheduler_lease(&request, &state, &previous, &renewed)
                .unwrap(),
            renewed_record
        );

        let mut stale_alternative = previous.clone();
        stale_alternative
            .renew(
                &request,
                &state,
                &background_job_identity("scheduler-owner:sha256:", 'f'),
                1_600,
                2_000,
            )
            .unwrap();
        assert_eq!(
            store
                .update_background_scheduler_lease(&request, &state, &previous, &stale_alternative,)
                .unwrap_err()
                .code,
            "background-lease-stale"
        );

        let before_release = renewed.clone();
        renewed
            .release(
                &request,
                &state,
                &background_job_identity("scheduler-owner:sha256:", 'f'),
                BackgroundSchedulerLeaseReleaseReason::OwnershipYielded,
                1_700,
            )
            .unwrap();
        let released = store
            .update_background_scheduler_lease(&request, &state, &before_release, &renewed)
            .unwrap();
        assert_eq!(
            released.lease.status,
            BackgroundSchedulerLeaseStatus::Released
        );
        let events = store
            .read_session_events(&request.session_id, 0, MAX_EVENT_PAGE)
            .unwrap()
            .into_iter()
            .filter(|event| event.event_kind.starts_with("background-job.lease-"))
            .map(|event| event.event_kind)
            .collect::<Vec<_>>();
        assert_eq!(
            events,
            [
                "background-job.lease-acquired",
                "background-job.lease-renewed",
                "background-job.lease-released",
            ]
        );
        drop(store);
        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(
            reopened
                .load_background_scheduler_lease(&request.job_id)
                .unwrap()
                .unwrap(),
            released
        );
    }

    #[test]
    fn background_scheduler_lease_event_failure_rolls_back_and_tamper_fails_startup() {
        let root = Root::new("background-lease-rollback");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let (request, state) =
            create_background_job_fixture(&mut store, &root, "rollback-lease-job");
        store.create_background_job(&request, &state).unwrap();
        let lease = BackgroundSchedulerLease::acquire(
            &request,
            &state,
            background_job_identity("scheduler-owner:sha256:", 'f'),
            1_250,
            2_000,
        )
        .unwrap();
        store
            .connection
            .execute_batch(
                "CREATE TRIGGER fail_background_lease_event
                 BEFORE INSERT ON events
                 WHEN NEW.event_kind = 'background-job.lease-acquired'
                 BEGIN SELECT RAISE(ABORT, 'injected'); END;",
            )
            .unwrap();
        assert!(store
            .create_background_scheduler_lease(&request, &state, &lease)
            .is_err());
        assert_eq!(
            query_global_count(
                &store.connection,
                "SELECT COUNT(*) FROM background_job_leases",
                "count"
            )
            .unwrap(),
            0
        );
        store
            .connection
            .execute_batch("DROP TRIGGER fail_background_lease_event;")
            .unwrap();
        store
            .create_background_scheduler_lease(&request, &state, &lease)
            .unwrap();
        store
            .connection
            .execute_batch(
                "CREATE TRIGGER fail_background_lease_renew_event
                 BEFORE INSERT ON events
                 WHEN NEW.event_kind = 'background-job.lease-renewed'
                 BEGIN SELECT RAISE(ABORT, 'injected'); END;",
            )
            .unwrap();
        let mut renewed = lease.clone();
        renewed
            .renew(
                &request,
                &state,
                &background_job_identity("scheduler-owner:sha256:", 'f'),
                1_400,
                2_000,
            )
            .unwrap();
        assert!(store
            .update_background_scheduler_lease(&request, &state, &lease, &renewed)
            .is_err());
        assert_eq!(
            store
                .load_background_scheduler_lease(&request.job_id)
                .unwrap()
                .unwrap()
                .lease,
            lease
        );
        store
            .connection
            .execute_batch("DROP TRIGGER fail_background_lease_renew_event;")
            .unwrap();
        store
            .connection
            .execute(
                "UPDATE background_job_leases SET lease_sha256 = ?1 WHERE job_id = ?2",
                params!["0".repeat(64), request.job_id],
            )
            .unwrap();
        assert_eq!(
            store
                .load_background_scheduler_lease(&request.job_id)
                .unwrap_err()
                .code,
            "background-lease-durable-integrity-invalid"
        );
        drop(store);
        assert_eq!(
            WorkbenchStore::open(&root.path).unwrap_err().code,
            "workbench-database-integrity-failed"
        );
    }

    #[test]
    fn background_scheduler_lease_rebinds_explicitly_and_terminal_lease_blocks_deletion() {
        let root = Root::new("background-lease-rebind");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let (request, mut state) =
            create_background_job_fixture(&mut store, &root, "rebind-lease-job");
        store.create_background_job(&request, &state).unwrap();
        let owner = background_job_identity("scheduler-owner:sha256:", 'f');
        let mut lease =
            BackgroundSchedulerLease::acquire(&request, &state, owner.clone(), 1_250, 5_000)
                .unwrap();
        store
            .create_background_scheduler_lease(&request, &state, &lease)
            .unwrap();

        let queued_state = state.clone();
        state.start(&request, 1_300).unwrap();
        store
            .update_background_job_state(&request, &queued_state, &state)
            .unwrap();
        let queued_lease = lease.clone();
        lease
            .rebind_job_state(
                &request,
                BackgroundSchedulerLeaseRebind {
                    previous: &queued_state,
                    next: &state,
                    owner_identity: &owner,
                    process_registration: None,
                    now_ms: 1_310,
                    ttl_ms: 5_000,
                },
            )
            .unwrap();
        store
            .update_background_scheduler_lease(&request, &state, &queued_lease, &lease)
            .unwrap();

        let running_state = state.clone();
        state
            .complete(
                &request,
                &background_job_identity("artifact:sha256:", 'a'),
                &background_job_identity("job-evidence:sha256:", 'b'),
                1_400,
            )
            .unwrap();
        store
            .update_background_job_state(&request, &running_state, &state)
            .unwrap();
        let running_lease = lease.clone();
        lease
            .rebind_job_state(
                &request,
                BackgroundSchedulerLeaseRebind {
                    previous: &running_state,
                    next: &state,
                    owner_identity: &owner,
                    process_registration: None,
                    now_ms: 1_410,
                    ttl_ms: 5_000,
                },
            )
            .unwrap();
        store
            .update_background_scheduler_lease(&request, &state, &running_lease, &lease)
            .unwrap();
        let recovery = store.load_background_jobs_for_recovery(10).unwrap();
        assert_eq!(recovery.len(), 1);
        assert_eq!(recovery[0].state.status, BackgroundJobStatus::Completed);

        let blocked = store
            .preview_session_deletion(&request.session_id, SessionDeletionScope::SessionOnly)
            .unwrap();
        assert!(!blocked
            .blocking_reasons
            .contains(&"session-background-job-active".into()));
        assert!(blocked
            .blocking_reasons
            .contains(&"session-background-lease-active".into()));

        let terminal_lease = lease.clone();
        lease
            .release(
                &request,
                &state,
                &owner,
                BackgroundSchedulerLeaseReleaseReason::JobTerminal,
                1_420,
            )
            .unwrap();
        store
            .update_background_scheduler_lease(&request, &state, &terminal_lease, &lease)
            .unwrap();
        assert!(store
            .load_background_jobs_for_recovery(10)
            .unwrap()
            .is_empty());
        let unblocked = store
            .preview_session_deletion(&request.session_id, SessionDeletionScope::SessionOnly)
            .unwrap();
        assert!(!unblocked
            .blocking_reasons
            .contains(&"session-background-lease-active".into()));
        let lease_events = store
            .read_session_events(&request.session_id, 0, MAX_EVENT_PAGE)
            .unwrap()
            .into_iter()
            .filter(|event| event.event_kind.starts_with("background-job.lease-"))
            .map(|event| event.event_kind)
            .collect::<Vec<_>>();
        assert_eq!(
            lease_events,
            [
                "background-job.lease-acquired",
                "background-job.lease-state-rebound",
                "background-job.lease-state-rebound",
                "background-job.lease-released",
            ]
        );
    }

    #[test]
    fn stale_background_scheduler_lease_can_only_expire_without_state_adoption() {
        let root = Root::new("background-lease-stale-expiry");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let (request, mut state) =
            create_background_job_fixture(&mut store, &root, "stale-expiry-lease-job");
        store.create_background_job(&request, &state).unwrap();
        let mut lease = BackgroundSchedulerLease::acquire(
            &request,
            &state,
            background_job_identity("scheduler-owner:sha256:", 'f'),
            1_250,
            1_000,
        )
        .unwrap();
        store
            .create_background_scheduler_lease(&request, &state, &lease)
            .unwrap();
        let queued = state.clone();
        state.start(&request, 1_300).unwrap();
        store
            .update_background_job_state(&request, &queued, &state)
            .unwrap();

        let active_stale = lease.clone();
        lease.expire(&request, 2_250).unwrap();
        let expired = store
            .update_background_scheduler_lease(&request, &state, &active_stale, &lease)
            .unwrap();
        assert_eq!(
            expired.lease.status,
            BackgroundSchedulerLeaseStatus::Expired
        );
        assert_eq!(expired.lease.job_generation, queued.generation);
        assert_ne!(expired.lease.job_generation, state.generation);
        assert!(!expired.lease.automatic_takeover);
        assert!(!expired.lease.dispatch_authority);
    }

    #[test]
    fn background_job_state_and_typed_events_are_atomic_idempotent_and_durable() {
        let root = Root::new("background-job-durable");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let (request, mut state) = create_background_job_fixture(&mut store, &root, "durable-job");
        let created = store.create_background_job(&request, &state).unwrap();
        assert_eq!(
            store.create_background_job(&request, &state).unwrap(),
            created
        );
        let mut duplicate_request = request.clone();
        duplicate_request.job_id = "durable-job-duplicate".into();
        let duplicate_state = BackgroundJobState::new(&duplicate_request, 1_200).unwrap();
        assert_eq!(
            store
                .create_background_job(&duplicate_request, &duplicate_state)
                .unwrap_err()
                .code,
            "background-job-idempotency-conflict"
        );

        let queued = state.clone();
        state.start(&request, 1_300).unwrap();
        let running = state.clone();
        store
            .update_background_job_state(&request, &queued, &running)
            .unwrap();
        assert_eq!(
            store
                .update_background_job_state(&request, &queued, &running)
                .unwrap()
                .state,
            running
        );

        let approval = background_job_identity("approval:sha256:", 'f');
        state.wait_for_approval(&request, &approval, 1_400).unwrap();
        let waiting = state.clone();
        store
            .update_background_job_state(&request, &running, &waiting)
            .unwrap();
        state
            .resume_after_approval(&request, &approval, 1_500)
            .unwrap();
        let resumed = state.clone();
        store
            .update_background_job_state(&request, &waiting, &resumed)
            .unwrap();
        state
            .complete(
                &request,
                &background_job_identity("artifact:sha256:", 'a'),
                &background_job_identity("job-evidence:sha256:", 'b'),
                1_600,
            )
            .unwrap();
        let completed = store
            .update_background_job_state(&request, &resumed, &state)
            .unwrap();
        assert_eq!(completed.state.status, BackgroundJobStatus::Completed);
        assert!(store
            .load_background_jobs_for_recovery(10)
            .unwrap()
            .is_empty());
        assert_eq!(
            store
                .update_background_job_state(&request, &running, &waiting)
                .unwrap_err()
                .code,
            "background-job-state-stale"
        );

        let events = store
            .read_session_events(&request.session_id, 0, MAX_EVENT_PAGE)
            .unwrap()
            .into_iter()
            .filter(|event| event.event_kind.starts_with("background-job."))
            .collect::<Vec<_>>();
        assert_eq!(
            events
                .iter()
                .map(|event| event.event_kind.as_str())
                .collect::<Vec<_>>(),
            [
                "background-job.queued",
                "background-job.started",
                "background-job.approval-needed",
                "background-job.approval-resumed",
                "background-job.completed",
            ]
        );
        assert!(events.iter().all(|event| {
            event.payload.get("automatic_retry") == Some(&json!(false))
                && event.payload.get("automatic_approval") == Some(&json!(false))
        }));
        assert!(
            store
                .verify_session_projection(&request.session_id)
                .unwrap()
                .consistent
        );

        drop(store);
        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(
            reopened.load_background_job("durable-job").unwrap(),
            completed
        );
    }

    #[test]
    fn background_job_event_failure_rolls_back_projection_and_tampering_fails_closed() {
        let root = Root::new("background-job-rollback");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let (request, state) = create_background_job_fixture(&mut store, &root, "rollback-job");
        store
            .connection
            .execute_batch(
                "CREATE TRIGGER fail_background_job_event
                 BEFORE INSERT ON events
                 WHEN NEW.event_kind = 'background-job.queued'
                 BEGIN SELECT RAISE(ABORT, 'injected'); END;",
            )
            .unwrap();
        assert!(store.create_background_job(&request, &state).is_err());
        assert_eq!(
            query_global_count(
                &store.connection,
                "SELECT COUNT(*) FROM background_jobs",
                "count"
            )
            .unwrap(),
            0
        );
        store
            .connection
            .execute_batch("DROP TRIGGER fail_background_job_event;")
            .unwrap();
        store.create_background_job(&request, &state).unwrap();
        store
            .connection
            .execute(
                "UPDATE background_jobs SET state_sha256 = ?1 WHERE job_id = ?2",
                params!["0".repeat(64), request.job_id],
            )
            .unwrap();
        assert_eq!(
            store.load_background_job(&request.job_id).unwrap_err().code,
            "background-job-durable-integrity-invalid"
        );
        drop(store);
        assert_eq!(
            WorkbenchStore::open(&root.path).unwrap_err().code,
            "workbench-database-integrity-failed"
        );
    }

    #[test]
    fn active_background_job_blocks_session_deletion_until_terminal() {
        let root = Root::new("background-job-deletion");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let (request, mut state) = create_background_job_fixture(&mut store, &root, "deletion-job");
        store.create_background_job(&request, &state).unwrap();
        let active = store
            .preview_session_deletion(&request.session_id, SessionDeletionScope::SessionOnly)
            .unwrap();
        assert_eq!(active.background_job_count, 1);
        assert!(active
            .blocking_reasons
            .contains(&"session-background-job-active".into()));
        store
            .set_retention_policy(RetentionPolicy {
                scope_kind: "session".into(),
                scope_id: request.session_id.clone(),
                archive_after_ms: Some(86_400_000),
                delete_after_ms: None,
                undo_window_ms: MIN_SESSION_DELETE_UNDO_MS,
                delete_scope: SessionDeletionScope::SessionOnly,
                updated_at_ms: 1_201,
            })
            .unwrap();
        let retention = store
            .apply_retention_policies(86_401_201, &BTreeSet::new())
            .unwrap();
        assert_eq!(retention.protected_sessions, 1);
        assert_eq!(
            store.load_session(&request.session_id).unwrap().status,
            "active"
        );

        let queued = state.clone();
        state.start(&request, 1_300).unwrap();
        store
            .update_background_job_state(&request, &queued, &state)
            .unwrap();
        let running = state.clone();
        let approval = background_job_identity("approval:sha256:", 'f');
        state.wait_for_approval(&request, &approval, 1_400).unwrap();
        store
            .update_background_job_state(&request, &running, &state)
            .unwrap();
        let intent = BackgroundNotificationIntent::from_job_state(&request, &state, 1_450).unwrap();
        store
            .enqueue_background_notification(&intent, 1_500)
            .unwrap();
        let waiting = state.clone();
        state.request_cancel(&request, 1_600).unwrap();
        let cancelling = state.clone();
        store
            .update_background_job_state(&request, &waiting, &cancelling)
            .unwrap();
        state
            .acknowledge_cancel(
                &request,
                Some(&background_job_identity("job-evidence:sha256:", 'a')),
                1_700,
            )
            .unwrap();
        store
            .update_background_job_state(&request, &cancelling, &state)
            .unwrap();
        let terminal = store
            .preview_session_deletion(&request.session_id, SessionDeletionScope::SessionOnly)
            .unwrap();
        assert_eq!(terminal.background_job_count, 1);
        assert!(!terminal
            .blocking_reasons
            .contains(&"session-background-job-active".into()));
        assert_eq!(
            query_global_count(
                &store.connection,
                "SELECT COUNT(*) FROM background_notification_outbox",
                "count",
            )
            .unwrap(),
            1
        );
        let undo_window_ms = MIN_SESSION_DELETE_UNDO_MS;
        store
            .schedule_session_deletion(
                "background-job-deletion-purge",
                &request.session_id,
                SessionDeletionScope::SessionOnly,
                &terminal.plan_hash,
                1_800,
                undo_window_ms,
            )
            .unwrap();
        assert_eq!(
            store
                .sweep_session_deletions(1_800 + undo_window_ms)
                .unwrap()
                .purged,
            1
        );
        assert_eq!(
            query_global_count(
                &store.connection,
                "SELECT COUNT(*) FROM background_jobs",
                "count",
            )
            .unwrap(),
            0
        );
        assert_eq!(
            query_global_count(
                &store.connection,
                "SELECT COUNT(*) FROM background_notification_outbox",
                "count",
            )
            .unwrap(),
            0
        );
        assert_eq!(
            query_global_count(
                &store.connection,
                "SELECT COUNT(*) FROM events
                 WHERE event_kind = 'background-job.notification-recorded'",
                "count",
            )
            .unwrap(),
            0
        );
    }

    #[test]
    fn upgrades_schema_v12_to_session_workspace_bindings_after_backup() {
        let root = Root::new("schema-v12-session-workspace-bindings");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "session-v12".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Preserved from v12".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 1,
            })
            .unwrap();
        drop(store);
        let connection = Connection::open(root.path.join(DATABASE_FILE)).unwrap();
        connection
            .execute_batch(
                "DROP INDEX session_workspace_binding_branch_idx;
                 DROP TABLE session_workspace_bindings;
                 PRAGMA user_version = 12;",
            )
            .unwrap();
        drop(connection);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        let version: i64 = reopened
            .connection
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .unwrap();
        assert_eq!(version, SCHEMA_VERSION);
        assert_eq!(
            reopened.load_session("session-v12").unwrap().title,
            "Preserved from v12"
        );
        for table in REQUIRED_WORKSPACE_BINDING_TABLES {
            let exists: bool = reopened
                .connection
                .query_row(
                    "SELECT EXISTS(
                        SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1
                     )",
                    [table],
                    |row| row.get(0),
                )
                .unwrap();
            assert!(exists, "missing session workspace binding table {table}");
        }
        let manifests = migration_backup_manifests(&root.path).unwrap();
        assert_eq!(manifests.len(), 1);
        assert_eq!(manifests[0].source_schema_version, 12);
        assert_eq!(manifests[0].target_schema_version, SCHEMA_VERSION as u64);
    }

    #[test]
    fn upgrades_schema_v11_to_background_notification_outbox_after_backup() {
        let root = Root::new("schema-v11-background-notifications");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "session-v11".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Preserved from v11".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 1,
            })
            .unwrap();
        drop(store);
        let connection = Connection::open(root.path.join(DATABASE_FILE)).unwrap();
        connection
            .execute_batch(
                "DROP TABLE background_notification_outbox;
                 PRAGMA user_version = 11;",
            )
            .unwrap();
        drop(connection);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        let version: i64 = reopened
            .connection
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .unwrap();
        assert_eq!(version, SCHEMA_VERSION);
        assert_eq!(
            reopened.load_session("session-v11").unwrap().title,
            "Preserved from v11"
        );
        for table in REQUIRED_BACKGROUND_NOTIFICATION_TABLES {
            let exists: bool = reopened
                .connection
                .query_row(
                    "SELECT EXISTS(
                        SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1
                     )",
                    [table],
                    |row| row.get(0),
                )
                .unwrap();
            assert!(exists, "missing background notification table {table}");
        }
        for index in REQUIRED_BACKGROUND_NOTIFICATION_INDEXES {
            let exists: bool = reopened
                .connection
                .query_row(
                    "SELECT EXISTS(
                        SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = ?1
                     )",
                    [index],
                    |row| row.get(0),
                )
                .unwrap();
            assert!(exists, "missing background notification index {index}");
        }
        assert_eq!(
            query_global_count(
                &reopened.connection,
                "SELECT COUNT(*) FROM background_notification_outbox",
                "count",
            )
            .unwrap(),
            0
        );
        let manifests = migration_backup_manifests(&root.path).unwrap();
        assert_eq!(manifests.len(), 1);
        assert_eq!(manifests[0].source_schema_version, 11);
        assert_eq!(manifests[0].target_schema_version, SCHEMA_VERSION as u64);
    }

    #[test]
    fn upgrades_schema_v10_to_background_scheduler_leases_after_backup() {
        let root = Root::new("schema-v10-background-leases");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "session-v10".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Preserved from v10".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 1,
            })
            .unwrap();
        drop(store);
        let connection = Connection::open(root.path.join(DATABASE_FILE)).unwrap();
        connection
            .execute_batch("DROP TABLE background_job_leases; PRAGMA user_version = 10;")
            .unwrap();
        drop(connection);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(
            reopened.load_session("session-v10").unwrap().title,
            "Preserved from v10"
        );
        for table in REQUIRED_BACKGROUND_LEASE_TABLES {
            let exists: bool = reopened
                .connection
                .query_row(
                    "SELECT EXISTS(
                        SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1
                     )",
                    [table],
                    |row| row.get(0),
                )
                .unwrap();
            assert!(exists, "missing background lease table {table}");
        }
        for index in REQUIRED_BACKGROUND_LEASE_INDEXES {
            let exists: bool = reopened
                .connection
                .query_row(
                    "SELECT EXISTS(
                        SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = ?1
                     )",
                    [index],
                    |row| row.get(0),
                )
                .unwrap();
            assert!(exists, "missing background lease index {index}");
        }
        let manifests = migration_backup_manifests(&root.path).unwrap();
        assert_eq!(manifests.len(), 1);
        assert_eq!(manifests[0].source_schema_version, 10);
        assert_eq!(manifests[0].target_schema_version, SCHEMA_VERSION as u64);
    }

    #[test]
    fn upgrades_schema_v9_to_background_jobs_after_backup() {
        let root = Root::new("schema-v9-background-jobs");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "session-v9".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Preserved from v9".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 1,
            })
            .unwrap();
        drop(store);
        let connection = Connection::open(root.path.join(DATABASE_FILE)).unwrap();
        connection
            .execute_batch(
                "DROP TABLE background_job_leases;
                 DROP TABLE background_jobs;
                 PRAGMA user_version = 9;",
            )
            .unwrap();
        drop(connection);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(
            reopened.load_session("session-v9").unwrap().title,
            "Preserved from v9"
        );
        for table in REQUIRED_BACKGROUND_JOB_TABLES {
            let exists: bool = reopened
                .connection
                .query_row(
                    "SELECT EXISTS(
                        SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1
                     )",
                    [table],
                    |row| row.get(0),
                )
                .unwrap();
            assert!(exists, "missing background job table {table}");
        }
        for index in REQUIRED_BACKGROUND_JOB_INDEXES {
            let exists: bool = reopened
                .connection
                .query_row(
                    "SELECT EXISTS(
                        SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = ?1
                     )",
                    [index],
                    |row| row.get(0),
                )
                .unwrap();
            assert!(exists, "missing background job index {index}");
        }
        let manifests = migration_backup_manifests(&root.path).unwrap();
        assert_eq!(manifests.len(), 1);
        assert_eq!(manifests[0].source_schema_version, 9);
        assert_eq!(manifests[0].target_schema_version, SCHEMA_VERSION as u64);
    }

    #[test]
    fn upgrades_schema_v8_to_search_indexes_after_backup() {
        let root = Root::new("schema-v8-search-index-migration");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "session-v8".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Preserved from v8".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 1,
            })
            .unwrap();
        drop(store);
        let connection = Connection::open(root.path.join(DATABASE_FILE)).unwrap();
        connection
            .execute_batch(
                "DROP INDEX sessions_status_updated_idx;
                 DROP INDEX session_runtime_binding_model_idx;
                 DROP INDEX items_transcript_search_idx;
                 PRAGMA user_version = 8;",
            )
            .unwrap();
        drop(connection);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(
            reopened.load_session("session-v8").unwrap().title,
            "Preserved from v8"
        );
        for index in REQUIRED_SESSION_SEARCH_INDEXES {
            let exists: bool = reopened
                .connection
                .query_row(
                    "SELECT EXISTS(
                        SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = ?1
                     )",
                    [index],
                    |row| row.get(0),
                )
                .unwrap();
            assert!(exists, "missing session search index {index}");
        }
        let version: i64 = reopened
            .connection
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .unwrap();
        assert_eq!(version, SCHEMA_VERSION);
        let manifests = migration_backup_manifests(&root.path).unwrap();
        assert_eq!(manifests.len(), 1);
        assert_eq!(manifests[0].source_schema_version, 8);
        assert_eq!(manifests[0].target_schema_version, SCHEMA_VERSION as u64);
    }

    #[test]
    fn upgrades_schema_v5_to_retention_tables_after_backup() {
        let root = Root::new("schema-v5-retention-migration");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "session-v5".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Preserved from v5".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 1,
            })
            .unwrap();
        drop(store);
        let connection = Connection::open(root.path.join(DATABASE_FILE)).unwrap();
        connection
            .execute_batch(
                "DROP TABLE session_deletion_members;
                 DROP TABLE session_deletions;
                 DROP TABLE retention_policies;
                 PRAGMA user_version = 5;",
            )
            .unwrap();
        drop(connection);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(
            reopened.load_session("session-v5").unwrap().title,
            "Preserved from v5"
        );
        for table in REQUIRED_RETENTION_TABLES {
            let exists: bool = reopened
                .connection
                .query_row(
                    "SELECT EXISTS(
                        SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1
                     )",
                    [table],
                    |row| row.get(0),
                )
                .unwrap();
            assert!(exists, "missing retention table {table}");
        }
        let manifests = migration_backup_manifests(&root.path).unwrap();
        assert_eq!(manifests.len(), 1);
        assert_eq!(manifests[0].source_schema_version, 5);
        assert_eq!(manifests[0].target_schema_version, SCHEMA_VERSION as u64);
    }

    #[test]
    fn database_writes_reject_low_space_before_transaction_side_effects() {
        let root = Root::new("database-write-low-space");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store.set_database_available_bytes_override(Some(
            MIN_FREE_BYTES
                .saturating_add(DATABASE_WRITE_HEADROOM_BYTES)
                .saturating_sub(1),
        ));
        let error = store
            .create_session(StoredSessionCreate {
                session_id: "session-low-space".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Must not persist".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 1,
            })
            .unwrap_err();
        assert_eq!(error.code, "database-write-low-space");
        let rows: (i64, i64, i64) = store
            .connection
            .query_row(
                "SELECT
                    (SELECT COUNT(*) FROM sessions
                     WHERE session_id = 'session-low-space'),
                    (SELECT COUNT(*) FROM session_projection_sources
                     WHERE session_id = 'session-low-space'),
                    (SELECT COUNT(*) FROM events
                     WHERE session_id = 'session-low-space')",
                [],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )
            .unwrap();
        assert_eq!(rows, (0, 0, 0));
        store.set_database_available_bytes_override(Some(
            MIN_FREE_BYTES + DATABASE_WRITE_HEADROOM_BYTES,
        ));
        store
            .create_session(StoredSessionCreate {
                session_id: "session-low-space".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Now admitted".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 1,
            })
            .unwrap();
    }

    #[test]
    fn low_space_keeps_verified_artifact_reads_available_without_metadata_write() {
        let root = Root::new("database-low-space-artifact-read");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_blob_owners(&mut store, &root);
        let stored = store
            .put_durable_blob(blob_request(
                "blob-session-a",
                "blob-session-a",
                b"readable while low on space",
                1_000,
            ))
            .unwrap();
        store.set_database_available_bytes_override(Some(MIN_FREE_BYTES));
        let read = store
            .read_durable_blob_for_session("blob-session-a", &stored.content_reference, 2_000)
            .unwrap();
        assert_eq!(read.content, b"readable while low on space");
        assert_eq!(read.reference.last_accessed_at_ms, 1_000);
        let persisted_access: i64 = store
            .connection
            .query_row(
                "SELECT last_accessed_at_ms FROM durable_blob_references
                 WHERE reference_id = ?1",
                [&stored.reference_id],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(persisted_access, 1_000);
        let release_error = store
            .release_durable_blob_reference(&stored.reference_id, 2_000)
            .unwrap_err();
        assert_eq!(release_error.code, "database-write-low-space");
        assert_eq!(
            load_durable_blob_reference(&store.connection, &stored.reference_id)
                .unwrap()
                .state,
            "active"
        );
    }

    #[test]
    fn database_busy_timeout_bounds_competing_writes() {
        let root = Root::new("database-busy-timeout");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store.set_database_available_bytes_override(Some(u64::MAX));
        let competing = Connection::open(root.path.join(DATABASE_FILE)).unwrap();
        competing.execute_batch("BEGIN IMMEDIATE;").unwrap();
        let started = Instant::now();
        let error = store
            .create_session(StoredSessionCreate {
                session_id: "session-busy".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Busy".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 1,
            })
            .unwrap_err();
        let elapsed = started.elapsed();
        assert_eq!(error.code, "workbench-store-error");
        assert!(elapsed >= Duration::from_millis(1_500));
        assert!(elapsed < Duration::from_secs(5));
        competing.execute_batch("ROLLBACK;").unwrap();
        assert!(store.load_session("session-busy").is_err());
    }

    #[test]
    fn database_compaction_is_space_admitted_bounded_and_integrity_checked() {
        let root = Root::new("database-compaction");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .connection
            .execute_batch(
                "CREATE TABLE maintenance_fixture (payload BLOB) STRICT;
                 BEGIN IMMEDIATE;
                 INSERT INTO maintenance_fixture(payload) VALUES (zeroblob(8388608));
                 COMMIT;
                 DELETE FROM maintenance_fixture;",
            )
            .unwrap();
        let (database_bytes, wal_bytes) = store.database_storage_bytes().unwrap();
        let required = MIN_FREE_BYTES
            .saturating_add(DATABASE_WRITE_HEADROOM_BYTES)
            .saturating_add(database_bytes.saturating_mul(2))
            .saturating_add(wal_bytes);
        store.set_database_available_bytes_override(Some(required.saturating_sub(1)));
        let error = store.compact_database().unwrap_err();
        assert_eq!(error.code, "database-maintenance-low-space");
        store.set_database_available_bytes_override(Some(required));
        let report = store.compact_database().unwrap();
        assert!(report.integrity_ok);
        assert!(report.database_bytes_after <= report.database_bytes_before);
        assert_eq!(report.freelist_pages_after, 0);
        assert!(report.wal_bytes_after <= WAL_JOURNAL_SIZE_LIMIT_BYTES as u64);
        assert_eq!(
            store
                .connection
                .query_row("PRAGMA quick_check(1)", [], |row| row.get::<_, String>(0))
                .unwrap(),
            "ok"
        );
    }

    #[test]
    fn operation_reconciliation_event_is_durable_idempotent_and_clears_block() {
        let root = Root::new("operation-reconciliation-store");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "reconcile-session".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Reconciliation".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 10,
            })
            .unwrap();
        let unknown = ReconciliationInput {
            operation_id: "operation-unknown".into(),
            session_id: "reconcile-session".into(),
            kind: crate::operation_reconciliation::OperationKind::Turn,
            evidence: crate::operation_reconciliation::ReconciliationEvidence {
                event: crate::operation_reconciliation::EventState::None,
                process: crate::operation_reconciliation::ProcessState::NotObserved,
                workspace: crate::operation_reconciliation::WorkspaceState::NotRequired,
                git: crate::operation_reconciliation::GitState::NotRequired,
            },
        };
        let unknown_result = reconcile_operation(&unknown).unwrap();
        assert!(unknown_result.writes_blocked);
        let stored = store
            .append_operation_reconciliation(&unknown, &unknown_result, 20)
            .unwrap();
        assert_eq!(stored.event_sequence, 2);
        assert_eq!(
            store
                .append_operation_reconciliation(&unknown, &unknown_result, 21)
                .unwrap()
                .event_sequence,
            stored.event_sequence
        );
        assert_eq!(
            store
                .session_blocked_operation("reconcile-session")
                .unwrap()
                .unwrap()
                .result
                .review_id,
            unknown_result.review_id
        );
        drop(store);

        let mut reopened = WorkbenchStore::open(&root.path).unwrap();
        let loaded = reopened
            .latest_operation_reconciliation("reconcile-session", "operation-unknown")
            .unwrap()
            .unwrap();
        assert_eq!(loaded.result, unknown_result);
        let startup_records = reopened.load_operation_reconciliations().unwrap();
        assert_eq!(startup_records.len(), 1);
        assert_eq!(startup_records[0].result, unknown_result);
        let completed = ReconciliationInput {
            evidence: crate::operation_reconciliation::ReconciliationEvidence {
                event: crate::operation_reconciliation::EventState::Completed,
                process: crate::operation_reconciliation::ProcessState::NotRunning,
                workspace: crate::operation_reconciliation::WorkspaceState::NotRequired,
                git: crate::operation_reconciliation::GitState::NotRequired,
            },
            ..unknown
        };
        let completed_result = reconcile_operation(&completed).unwrap();
        assert!(!completed_result.writes_blocked);
        reopened
            .append_operation_reconciliation(&completed, &completed_result, 30)
            .unwrap();
        assert!(reopened
            .session_blocked_operation("reconcile-session")
            .unwrap()
            .is_none());
    }

    #[test]
    fn persists_projects_sessions_and_lineage_after_reopen() {
        let root = Root::new("sessions");
        let project_root = root.parent.canonicalize().unwrap();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let project = store
            .create_project(StoredProjectCreate {
                project_id: "project-session".into(),
                root_id: "root-1".into(),
                canonical_root: project_root.to_string_lossy().into_owned(),
                root_identity:
                    "root:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                        .into(),
                display_name: "Session project".into(),
                root_access: "write".into(),
                created_at_ms: 10,
            })
            .unwrap();
        assert_eq!(project.state, "active");
        let chat = store
            .create_session(StoredSessionCreate {
                session_id: "chat-session".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Chat".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 20,
            })
            .unwrap();
        assert_eq!(chat.mode, StoredSessionMode::Chat);
        let work = store
            .create_session(StoredSessionCreate {
                session_id: "work-session".into(),
                project_id: Some(project.project_id.clone()),
                mode: StoredSessionMode::Work,
                title: "Work".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: Some(
                    "env:sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                        .into(),
                ),
                created_at_ms: 30,
            })
            .unwrap();
        let fork = store
            .create_session(StoredSessionCreate {
                session_id: "fork-session".into(),
                project_id: work.project_id.clone(),
                mode: StoredSessionMode::Work,
                title: "Fork".into(),
                parent_session_id: Some(work.session_id.clone()),
                lineage_kind: StoredSessionLineage::Fork,
                environment_identity: None,
                created_at_ms: 40,
            })
            .unwrap();
        assert_eq!(fork.parent_session_id, Some(work.session_id.clone()));
        let listed = store.list_sessions(None, None, false, 20).unwrap();
        assert_eq!(listed.len(), 3);
        assert_eq!(
            store
                .list_sessions(
                    Some(&project.project_id),
                    Some(StoredSessionMode::Work),
                    false,
                    20
                )
                .unwrap()
                .len(),
            2
        );
        drop(store);
        let mut reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(reopened.load_project(&project.project_id).unwrap(), project);
        assert_eq!(reopened.load_session("fork-session").unwrap(), fork);
        let archived = reopened.archive_session("work-session", 50).unwrap();
        assert_eq!(archived.status, "archived");
        assert_eq!(
            reopened.list_sessions(None, None, false, 20).unwrap().len(),
            2
        );
        assert_eq!(
            reopened.list_sessions(None, None, true, 20).unwrap().len(),
            3
        );
        assert!(reopened.archive_session("work-session", 49).is_err());
        let restored = reopened.unarchive_session("work-session", 60).unwrap();
        assert_eq!(restored.status, "active");
        let renamed = reopened
            .update_session_title("work-session", "Renamed work", 70)
            .unwrap();
        assert_eq!(renamed.title, "Renamed work");
        let rebuilt = reopened
            .rebuild_session_projection_candidate("work-session")
            .unwrap();
        assert!(rebuilt.source_complete);
        assert!(rebuilt.matches_current_projection);
        assert_eq!(rebuilt.session.as_ref(), Some(&renamed));
        let consistency = reopened.verify_session_projection("work-session").unwrap();
        assert!(consistency.consistent);
        assert!(consistency.rebuild_source_complete);
        assert!(consistency.event_projection_matches);
        assert!(reopened
            .update_session_title("work-session", "", 80)
            .is_err());
    }

    #[test]
    fn indexed_session_search_filters_metadata_and_approved_transcript_fields() {
        let root = Root::new("session-search");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let project_root = root.parent.canonicalize().unwrap();
        store
            .create_project(StoredProjectCreate {
                project_id: "search-project".into(),
                root_id: "root-1".into(),
                canonical_root: project_root.to_string_lossy().into_owned(),
                root_identity:
                    "root:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                        .into(),
                display_name: "Search project".into(),
                root_access: "write".into(),
                created_at_ms: 1,
            })
            .unwrap();
        let runtime_binding = StoredSessionRuntimeBindingCreate {
            session_id: "search-session-a".into(),
            adapter: "codex-app-server".into(),
            adapter_version: "codex-cli 0.144.5".into(),
            backend_session_id: Some("thread-search-a".into()),
            provider: Some("aegisy".into()),
            model: Some("gpt-search".into()),
            permission_profile: "read-only".into(),
            created_at_ms: 10,
        };
        let workspace_binding = StoredSessionWorkspaceBindingCreate {
            session_id: "search-session-a".into(),
            project_id: "search-project".into(),
            root_id: "root-1".into(),
            root_identity:
                "root:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                    .into(),
            workspace_kind: "project-root".into(),
            git_state: "worktree".into(),
            repository_root_identity: Some("fs:test:repository".into()),
            worktree_root_identity: Some("fs:test:worktree".into()),
            branch: Some("main".into()),
            branch_sha256: Some(ContentHash::for_bytes(b"main").sha256),
            branch_redacted: false,
            head_oid: Some("a".repeat(40)),
            detached: false,
            unborn: false,
            dedicated_worktree: false,
            captured_at_ms: 10,
        };
        store
            .create_session_with_bindings(
                StoredSessionCreate {
                    session_id: "search-session-a".into(),
                    project_id: Some("search-project".into()),
                    mode: StoredSessionMode::Work,
                    title: "Fix TLS streaming".into(),
                    parent_session_id: None,
                    lineage_kind: StoredSessionLineage::New,
                    environment_identity: None,
                    created_at_ms: 10,
                },
                Some(runtime_binding),
                Some(workspace_binding),
            )
            .unwrap();
        store
            .append_item(StoredItemAppend {
                session_id: "search-session-a".into(),
                turn_id: None,
                item_id: "search-item-a".into(),
                item_kind: "message".into(),
                role: "user".into(),
                state: "completed".into(),
                payload: json!({"text": "Investigate stream disconnect"}),
                created_at_ms: 11,
            })
            .unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "search-session-b".into(),
                project_id: Some("search-project".into()),
                mode: StoredSessionMode::Work,
                title: "Unrelated".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 20,
            })
            .unwrap();
        store
            .append_item(StoredItemAppend {
                session_id: "search-session-b".into(),
                turn_id: None,
                item_id: "search-item-b".into(),
                item_kind: "diagnostic".into(),
                role: "system".into(),
                state: "completed".into(),
                payload: json!({"text": "stream disconnect"}),
                created_at_ms: 21,
            })
            .unwrap();

        let text_page = store
            .search_sessions(&SessionSearchRequest {
                project_id: Some("search-project".into()),
                branch: None,
                model: None,
                runtime: None,
                status: None,
                title: None,
                text: Some("disconnect".into()),
                include_archived: false,
                cursor: None,
                limit: 10,
            })
            .unwrap();
        assert_eq!(text_page.results.len(), 1);
        assert_eq!(text_page.results[0].session.session_id, "search-session-a");
        assert_eq!(
            text_page.results[0]
                .runtime
                .as_ref()
                .unwrap()
                .model
                .as_deref(),
            Some("gpt-search")
        );
        assert_eq!(text_page.results[0].matched_fields, vec!["text"]);
        assert_eq!(
            text_page.results[0]
                .workspace
                .as_ref()
                .and_then(|workspace| workspace.branch.as_deref()),
            Some("main")
        );

        let branch_page = store
            .search_sessions(&SessionSearchRequest {
                project_id: Some("search-project".into()),
                branch: Some("main".into()),
                model: None,
                runtime: None,
                status: None,
                title: None,
                text: None,
                include_archived: false,
                cursor: None,
                limit: 10,
            })
            .unwrap();
        assert_eq!(branch_page.results.len(), 1);
        assert_eq!(branch_page.results[0].matched_fields, vec!["branch"]);

        let model_page = store
            .search_sessions(&SessionSearchRequest {
                project_id: None,
                branch: None,
                model: Some("gpt-search".into()),
                runtime: Some("codex-app-server".into()),
                status: None,
                title: Some("TLS".into()),
                text: None,
                include_archived: false,
                cursor: None,
                limit: 10,
            })
            .unwrap();
        assert_eq!(model_page.results.len(), 1);
        assert_eq!(
            model_page.results[0].matched_fields,
            vec!["title", "model", "runtime"]
        );

        let first_page = store
            .search_sessions(&SessionSearchRequest {
                project_id: Some("search-project".into()),
                branch: None,
                model: None,
                runtime: None,
                status: None,
                title: None,
                text: None,
                include_archived: false,
                cursor: None,
                limit: 1,
            })
            .unwrap();
        assert_eq!(first_page.results[0].session.session_id, "search-session-b");
        assert!(first_page.truncated);
        assert_eq!(
            first_page.next_cursor.as_deref(),
            Some("after:20:search-session-b")
        );
        let second_page = store
            .search_sessions(&SessionSearchRequest {
                project_id: Some("search-project".into()),
                branch: None,
                model: None,
                runtime: None,
                status: None,
                title: None,
                text: None,
                include_archived: false,
                cursor: first_page.next_cursor,
                limit: 1,
            })
            .unwrap();
        assert_eq!(
            second_page.results[0].session.session_id,
            "search-session-a"
        );
        assert!(!second_page.truncated);
        assert!(second_page.next_cursor.is_none());
        let invalid_cursor = store
            .search_sessions(&SessionSearchRequest {
                project_id: None,
                branch: None,
                model: None,
                runtime: None,
                status: None,
                title: None,
                text: None,
                include_archived: false,
                cursor: Some("after:020:search-session-b".into()),
                limit: 10,
            })
            .unwrap_err();
        assert!(invalid_cursor.message.contains("non-canonical"));
    }

    #[test]
    fn session_workspace_binding_is_atomic_and_semantic_tampering_fails_startup() {
        let root = Root::new("session-workspace-binding-integrity");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let project_root = root.parent.canonicalize().unwrap();
        let root_identity =
            "root:sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
        store
            .create_project(StoredProjectCreate {
                project_id: "workspace-project".into(),
                root_id: "root-1".into(),
                canonical_root: project_root.to_string_lossy().into_owned(),
                root_identity: root_identity.into(),
                display_name: "Workspace project".into(),
                root_access: "write".into(),
                created_at_ms: 1,
            })
            .unwrap();
        let session = StoredSessionCreate {
            session_id: "workspace-session".into(),
            project_id: Some("workspace-project".into()),
            mode: StoredSessionMode::Work,
            title: "Workspace session".into(),
            parent_session_id: None,
            lineage_kind: StoredSessionLineage::New,
            environment_identity: None,
            created_at_ms: 10,
        };
        let runtime = StoredSessionRuntimeBindingCreate {
            session_id: "workspace-session".into(),
            adapter: "preview".into(),
            adapter_version: "0.1.0".into(),
            backend_session_id: None,
            provider: Some("local".into()),
            model: Some("deterministic-echo".into()),
            permission_profile: "read-only".into(),
            created_at_ms: 10,
        };
        let workspace = StoredSessionWorkspaceBindingCreate {
            session_id: "workspace-session".into(),
            project_id: "workspace-project".into(),
            root_id: "root-1".into(),
            root_identity: root_identity.into(),
            workspace_kind: "project-root".into(),
            git_state: "worktree".into(),
            repository_root_identity: Some("fs:test:repository".into()),
            worktree_root_identity: Some("fs:test:worktree".into()),
            branch: Some("feature/workspace".into()),
            branch_sha256: Some(ContentHash::for_bytes(b"feature/workspace").sha256),
            branch_redacted: false,
            head_oid: Some("d".repeat(40)),
            detached: false,
            unborn: false,
            dedicated_worktree: false,
            captured_at_ms: 10,
        };
        store
            .connection
            .execute_batch(
                "CREATE TRIGGER fail_workspace_binding_event
                 BEFORE INSERT ON events
                 WHEN NEW.event_kind = 'session.workspace-bound'
                 BEGIN
                     SELECT RAISE(ABORT, 'injected workspace binding event failure');
                 END;",
            )
            .unwrap();
        assert!(store
            .create_session_with_bindings(
                session.clone(),
                Some(runtime.clone()),
                Some(workspace.clone()),
            )
            .is_err());
        for table in [
            "sessions",
            "session_runtime_bindings",
            "session_workspace_bindings",
            "session_projection_sources",
        ] {
            assert_eq!(
                query_global_count(
                    &store.connection,
                    &format!("SELECT COUNT(*) FROM {table}"),
                    "count",
                )
                .unwrap(),
                0,
            );
        }
        store
            .connection
            .execute_batch("DROP TRIGGER fail_workspace_binding_event;")
            .unwrap();
        store
            .create_session_with_bindings(
                session.clone(),
                Some(runtime.clone()),
                Some(workspace.clone()),
            )
            .unwrap();
        let loaded = store
            .load_session_workspace_binding("workspace-session")
            .unwrap();
        assert_eq!(loaded.branch.as_deref(), Some("feature/workspace"));

        let mut deleted_session = session;
        deleted_session.session_id = "workspace-delete-session".into();
        deleted_session.created_at_ms = 20;
        let mut deleted_runtime = runtime;
        deleted_runtime.session_id = deleted_session.session_id.clone();
        deleted_runtime.created_at_ms = 20;
        let mut deleted_workspace = workspace;
        deleted_workspace.session_id = deleted_session.session_id.clone();
        deleted_workspace.captured_at_ms = 20;
        store
            .create_session_with_bindings(
                deleted_session,
                Some(deleted_runtime),
                Some(deleted_workspace),
            )
            .unwrap();
        let preview = store
            .preview_session_deletion(
                "workspace-delete-session",
                SessionDeletionScope::SessionOnly,
            )
            .unwrap();
        store
            .schedule_session_deletion(
                "workspace-delete-plan",
                "workspace-delete-session",
                SessionDeletionScope::SessionOnly,
                &preview.plan_hash,
                30,
                MIN_SESSION_DELETE_UNDO_MS,
            )
            .unwrap();
        assert_eq!(
            store
                .sweep_session_deletions(30 + MIN_SESSION_DELETE_UNDO_MS)
                .unwrap()
                .purged,
            1
        );
        assert!(store
            .load_session_workspace_binding("workspace-delete-session")
            .is_err());
        drop(store);

        let connection = Connection::open(root.path.join(DATABASE_FILE)).unwrap();
        connection
            .execute(
                "UPDATE session_workspace_bindings SET branch_sha256 = ?1
                 WHERE session_id = 'workspace-session'",
                ["0".repeat(64)],
            )
            .unwrap();
        drop(connection);
        let rejected = WorkbenchStore::open(&root.path).unwrap_err();
        assert!(rejected.message.contains("workspace binding"));
    }

    #[test]
    fn session_resume_updates_environment_projection_atomically() {
        let root = Root::new("session-resume-environment");
        let original =
            "env:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        let resumed = "env:sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "resume-environment".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Resume environment".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: Some(original.into()),
                created_at_ms: 10,
            })
            .unwrap();

        let updated = store
            .record_session_resume("resume-environment", resumed, 20)
            .unwrap();
        assert_eq!(updated.environment_identity.as_deref(), Some(resumed));
        assert!(
            store
                .verify_session_projection("resume-environment")
                .unwrap()
                .consistent
        );

        drop(store);
        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(
            reopened
                .load_session("resume-environment")
                .unwrap()
                .environment_identity
                .as_deref(),
            Some(resumed)
        );
    }

    #[test]
    fn upgrades_schema_v1_to_session_tables_transactionally() {
        let root = Root::new("schema-upgrade");
        let store = WorkbenchStore::open(&root.path).unwrap();
        store
            .connection
            .execute_batch(
                "DROP TABLE items;
                 DROP TABLE turns;
                 DROP TABLE sessions;
                 DROP TABLE project_roots;
                 DROP TABLE projects;",
            )
            .unwrap();
        store
            .connection
            .pragma_update(None, "user_version", 1_i64)
            .unwrap();
        drop(store);
        let reopened = WorkbenchStore::open(&root.path).unwrap();
        let version: i64 = reopened
            .connection
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .unwrap();
        assert_eq!(version, SCHEMA_VERSION);
        for table in REQUIRED_SESSION_TABLES {
            let exists: Option<String> = reopened
                .connection
                .query_row(
                    "SELECT name FROM sqlite_master WHERE type = 'table' AND name = ?1",
                    [table],
                    |row| row.get(0),
                )
                .optional()
                .unwrap();
            assert_eq!(exists.as_deref(), Some(table));
        }
        let backups = migration_backup_manifests(&root.path).unwrap();
        assert_eq!(backups.len(), 1);
        assert_eq!(backups[0].source_schema_version, 1);
        assert_eq!(backups[0].target_schema_version, SCHEMA_VERSION as u64);
    }

    #[test]
    fn upgrades_schema_v2_to_turn_item_tables_transactionally() {
        let root = Root::new("schema-upgrade-v2");
        let store = WorkbenchStore::open(&root.path).unwrap();
        store
            .connection
            .execute_batch("DROP TABLE items; DROP TABLE turns;")
            .unwrap();
        store
            .connection
            .pragma_update(None, "user_version", 2_i64)
            .unwrap();
        drop(store);
        let reopened = WorkbenchStore::open(&root.path).unwrap();
        let version: i64 = reopened
            .connection
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .unwrap();
        assert_eq!(version, SCHEMA_VERSION);
        for table in REQUIRED_TURN_TABLES {
            let exists: Option<String> = reopened
                .connection
                .query_row(
                    "SELECT name FROM sqlite_master WHERE type = 'table' AND name = ?1",
                    [table],
                    |row| row.get(0),
                )
                .optional()
                .unwrap();
            assert_eq!(exists.as_deref(), Some(table));
        }
        let backups = migration_backup_manifests(&root.path).unwrap();
        assert_eq!(backups.len(), 1);
        assert_eq!(backups[0].source_schema_version, 2);
        assert_eq!(backups[0].target_schema_version, SCHEMA_VERSION as u64);
    }

    #[test]
    fn upgrades_schema_v3_events_without_loss_and_supports_chat_projection_sources() {
        let root = Root::new("schema-upgrade-v3");
        let project_root = root.parent.canonicalize().unwrap();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_project(StoredProjectCreate {
                project_id: "project-v3".into(),
                root_id: "root-v3".into(),
                canonical_root: project_root.to_string_lossy().into_owned(),
                root_identity: "root-v3".into(),
                display_name: "V3".into(),
                root_access: "read".into(),
                created_at_ms: 1,
            })
            .unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "work-v3".into(),
                project_id: Some("project-v3".into()),
                mode: StoredSessionMode::Work,
                title: "Legacy work".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 2,
            })
            .unwrap();
        let work_events = store.read_session_events("work-v3", 0, 20).unwrap();
        assert_eq!(work_events.len(), 1);
        assert_eq!(work_events[0].project_id.as_deref(), Some("project-v3"));
        store
            .connection
            .execute_batch(
                "DROP TABLE session_projection_sources;
                 DROP INDEX events_operation_idx;
                 ALTER TABLE events RENAME TO events_optional_project_v4;
                 CREATE TABLE events (
                    session_id TEXT NOT NULL,
                    sequence INTEGER NOT NULL CHECK(sequence >= 1),
                    event_id TEXT NOT NULL UNIQUE,
                    timestamp_ms INTEGER NOT NULL CHECK(timestamp_ms >= 0),
                    correlation_id TEXT NOT NULL,
                    event_kind TEXT NOT NULL,
                    project_id TEXT NOT NULL,
                    operation_id TEXT NOT NULL,
                    generation INTEGER NOT NULL CHECK(generation >= 0),
                    payload_json TEXT NOT NULL,
                    payload_sha256 TEXT NOT NULL,
                    payload_bytes INTEGER NOT NULL CHECK(payload_bytes >= 0),
                    PRIMARY KEY(session_id, sequence)
                 ) STRICT;
                 INSERT INTO events (
                    session_id, sequence, event_id, timestamp_ms, correlation_id,
                    event_kind, project_id, operation_id, generation, payload_json,
                    payload_sha256, payload_bytes
                 ) SELECT
                    session_id, sequence, event_id, timestamp_ms, correlation_id,
                    event_kind, project_id, operation_id, generation, payload_json,
                    payload_sha256, payload_bytes
                   FROM events_optional_project_v4;
                 DROP TABLE events_optional_project_v4;
                 CREATE INDEX events_operation_idx
                    ON events(project_id, operation_id, sequence);",
            )
            .unwrap();
        store
            .connection
            .pragma_update(None, "user_version", 3_i64)
            .unwrap();
        drop(store);

        let mut reopened = WorkbenchStore::open(&root.path).unwrap();
        let version: i64 = reopened
            .connection
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .unwrap();
        assert_eq!(version, SCHEMA_VERSION);
        assert_eq!(
            reopened.read_session_events("work-v3", 0, 20).unwrap(),
            work_events
        );
        let project_not_null: i64 = reopened
            .connection
            .query_row(
                "SELECT \"notnull\" FROM pragma_table_info('events')
                 WHERE name = 'project_id'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(project_not_null, 0);
        let legacy = reopened.verify_session_projection("work-v3").unwrap();
        assert!(legacy.consistent);
        assert!(!legacy.rebuild_source_complete);

        reopened
            .create_session(StoredSessionCreate {
                session_id: "chat-v4".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Chat v4".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 3,
            })
            .unwrap();
        let chat_events = reopened.read_session_events("chat-v4", 0, 20).unwrap();
        assert_eq!(chat_events.len(), 1);
        assert_eq!(chat_events[0].project_id, None);
        let chat = reopened.verify_session_projection("chat-v4").unwrap();
        assert!(chat.consistent);
        assert!(chat.rebuild_source_complete);
        assert!(chat.event_projection_matches);
        let backups = migration_backup_manifests(&root.path).unwrap();
        assert_eq!(backups.len(), 1);
        assert_eq!(backups[0].source_schema_version, 3);
        assert_eq!(backups[0].target_schema_version, SCHEMA_VERSION as u64);
    }

    #[test]
    fn rejects_invalid_session_bindings_before_writing_rows() {
        let root = Root::new("session-reject");
        let project_root = root.parent.canonicalize().unwrap();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        assert!(store
            .create_session(StoredSessionCreate {
                session_id: "work-without-project".into(),
                project_id: None,
                mode: StoredSessionMode::Work,
                title: "Invalid".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 1,
            })
            .is_err());
        store
            .create_project(StoredProjectCreate {
                project_id: "project-reject".into(),
                root_id: "root-1".into(),
                canonical_root: project_root.to_string_lossy().into_owned(),
                root_identity: "root".into(),
                display_name: "Project".into(),
                root_access: "read".into(),
                created_at_ms: 1,
            })
            .unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "chat-parent".into(),
                project_id: Some("project-reject".into()),
                mode: StoredSessionMode::Chat,
                title: "Parent".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 2,
            })
            .unwrap();
        assert!(store
            .create_session(StoredSessionCreate {
                session_id: "bad-fork".into(),
                project_id: Some("project-reject".into()),
                mode: StoredSessionMode::Work,
                title: "Bad fork".into(),
                parent_session_id: Some("chat-parent".into()),
                lineage_kind: StoredSessionLineage::Fork,
                environment_identity: None,
                created_at_ms: 3,
            })
            .is_err());
        assert!(store.load_session("bad-fork").is_err());
    }

    #[test]
    fn persists_turn_items_with_idempotency_replay_and_terminal_gate() {
        let root = Root::new("turn-items");
        let project_root = root.parent.canonicalize().unwrap();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_project(StoredProjectCreate {
                project_id: "project-turn".into(),
                root_id: "root-1".into(),
                canonical_root: project_root.to_string_lossy().into_owned(),
                root_identity:
                    "root:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                        .into(),
                display_name: "Turn project".into(),
                root_access: "write".into(),
                created_at_ms: 1,
            })
            .unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "session-turn".into(),
                project_id: Some("project-turn".into()),
                mode: StoredSessionMode::Work,
                title: "Turn session".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 2,
            })
            .unwrap();
        let turn_request = StoredTurnCreate {
            turn_id: "turn-1".into(),
            session_id: "session-turn".into(),
            idempotency_key: Some("turn-key-1".into()),
            input_hash: ContentHash::for_bytes(b"inspect workspace"),
            created_at_ms: 3,
        };
        let turn = store.create_turn(turn_request.clone()).unwrap();
        assert_eq!(turn.state, "started");
        assert_eq!(store.create_turn(turn_request).unwrap(), turn);
        let user_item = store
            .append_item(StoredItemAppend {
                session_id: "session-turn".into(),
                turn_id: Some("turn-1".into()),
                item_id: "item-user".into(),
                item_kind: "message".into(),
                role: "user".into(),
                state: "completed".into(),
                payload: json!({"text": "inspect workspace"}),
                created_at_ms: 4,
            })
            .unwrap();
        let agent_item = store
            .append_item(StoredItemAppend {
                session_id: "session-turn".into(),
                turn_id: Some("turn-1".into()),
                item_id: "item-agent".into(),
                item_kind: "message".into(),
                role: "agent".into(),
                state: "completed".into(),
                payload: json!({"text": "done"}),
                created_at_ms: 5,
            })
            .unwrap();
        let redacted_item = store
            .append_item(StoredItemAppend {
                session_id: "session-turn".into(),
                turn_id: Some("turn-1".into()),
                item_id: "item-redacted".into(),
                item_kind: "diagnostic".into(),
                role: "tool".into(),
                state: "completed".into(),
                payload: json!({"text": "API_KEY=sk-12345678901234567890"}),
                created_at_ms: 5,
            })
            .unwrap();
        assert!(!redacted_item
            .payload
            .to_string()
            .contains("12345678901234567890"));
        assert_eq!(user_item.sequence, 1);
        assert_eq!(agent_item.sequence, 2);
        assert_eq!(redacted_item.sequence, 3);
        assert_eq!(
            store.latest_session_item_sequence("session-turn").unwrap(),
            3
        );
        assert_eq!(store.load_turn("turn-1").unwrap().state, "running");
        let finished = store
            .finish_turn("session-turn", "turn-1", "completed", 6)
            .unwrap();
        assert_eq!(finished.state, "completed");
        assert!(store
            .read_turn_trace("session-turn", "turn-1")
            .unwrap()
            .is_none());
        assert!(store
            .append_item(StoredItemAppend {
                session_id: "session-turn".into(),
                turn_id: Some("turn-1".into()),
                item_id: "item-late".into(),
                item_kind: "message".into(),
                role: "agent".into(),
                state: "completed".into(),
                payload: json!({"text": "late"}),
                created_at_ms: 7,
            })
            .is_err());
        let replay = store.read_session_items("session-turn", 0, 20).unwrap();
        assert_eq!(replay, vec![user_item, agent_item, redacted_item]);
        let rebuilt = store
            .rebuild_session_projection_candidate("session-turn")
            .unwrap();
        assert!(rebuilt.source_complete);
        assert!(rebuilt.matches_current_projection);
        assert_eq!(rebuilt.turns, vec![finished]);
        assert_eq!(rebuilt.items, replay);
        drop(store);
        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert!(reopened
            .read_turn_trace("session-turn", "turn-1")
            .unwrap()
            .is_none());
        assert_eq!(
            reopened.read_session_items("session-turn", 0, 20).unwrap(),
            replay
        );
        assert_eq!(
            reopened
                .latest_session_item_sequence("session-turn")
                .unwrap(),
            3
        );
    }

    #[test]
    fn terminal_turn_trace_is_atomic_idempotent_and_restart_safe() {
        let root = Root::new("turn-trace-atomic");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_turn_trace_fixture(&mut store, &root, "trace-session", "trace-turn");
        let trace = failed_turn_trace("trace-session", "trace-turn", "trace-terminal", 4);

        let mut wrong_environment = trace.clone();
        wrong_environment.binding.environment_identity = None;
        assert_eq!(
            store
                .finish_turn_with_trace(
                    "trace-session",
                    "trace-turn",
                    "failed",
                    4,
                    &wrong_environment,
                )
                .unwrap_err()
                .code,
            "turn-trace-binding-mismatch"
        );

        let finished = store
            .finish_turn_with_trace("trace-session", "trace-turn", "failed", 4, &trace)
            .unwrap();
        assert_eq!(finished.state, "failed");
        let stored = store
            .read_turn_trace("trace-session", "trace-turn")
            .unwrap()
            .unwrap();
        assert_eq!(stored.trace, trace);
        assert_eq!(stored.state, "failed");
        assert_eq!(stored.recorded_at_ms, 4);
        assert_eq!(
            store
                .latest_operation_event_state("trace-session", "trace-turn")
                .unwrap(),
            Some(EventState::Failed)
        );
        assert_eq!(
            store
                .finish_turn_with_trace("trace-session", "trace-turn", "failed", 4, &trace)
                .unwrap(),
            finished
        );
        store
            .connection
            .execute(
                "UPDATE turns SET updated_at_ms = 5 WHERE turn_id = 'trace-turn'",
                [],
            )
            .unwrap();
        assert_eq!(
            store
                .finish_turn_with_trace("trace-session", "trace-turn", "failed", 4, &trace)
                .unwrap_err()
                .code,
            "turn-trace-conflict"
        );
        store
            .connection
            .execute(
                "UPDATE turns SET updated_at_ms = 4 WHERE turn_id = 'trace-turn'",
                [],
            )
            .unwrap();

        let conflicting = failed_turn_trace("trace-session", "trace-turn", "different-terminal", 4);
        assert_eq!(
            store
                .finish_turn_with_trace("trace-session", "trace-turn", "failed", 4, &conflicting,)
                .unwrap_err()
                .code,
            "turn-trace-conflict"
        );
        let candidate = store
            .rebuild_session_projection_candidate("trace-session")
            .unwrap();
        assert!(candidate.source_complete);
        assert!(candidate.matches_current_projection);

        drop(store);
        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(
            reopened
                .read_turn_trace("trace-session", "trace-turn")
                .unwrap(),
            Some(stored)
        );
        assert!(!reopened.session_requires_recovery("trace-session"));
    }

    #[test]
    fn current_turn_trace_mode_must_match_the_persisted_session() {
        let work_root = Root::new("turn-trace-work-chat-mode-mismatch");
        let mut work_store = WorkbenchStore::open(&work_root.path).unwrap();
        create_turn_trace_fixture(&mut work_store, &work_root, "trace-session", "trace-turn");
        let chat_trace = completed_chat_turn_trace_v2("trace-session", "trace-turn", 4);
        assert_eq!(
            work_store
                .finish_turn_with_trace("trace-session", "trace-turn", "completed", 4, &chat_trace,)
                .unwrap_err()
                .code,
            "turn-trace-binding-mismatch"
        );
        assert_eq!(work_store.load_turn("trace-turn").unwrap().state, "started");
        assert!(work_store
            .read_turn_trace("trace-session", "trace-turn")
            .unwrap()
            .is_none());

        let chat_root = Root::new("turn-trace-chat-work-mode-mismatch");
        let mut chat_store = WorkbenchStore::open(&chat_root.path).unwrap();
        create_turn_trace_fixture_with_mode(
            &mut chat_store,
            &chat_root,
            "trace-session",
            "trace-turn",
            StoredSessionMode::Chat,
        );
        let work_trace = completed_turn_trace_v2("trace-session", "trace-turn", 4);
        assert_eq!(
            chat_store
                .finish_turn_with_trace("trace-session", "trace-turn", "completed", 4, &work_trace,)
                .unwrap_err()
                .code,
            "turn-trace-binding-mismatch"
        );
        assert_eq!(chat_store.load_turn("trace-turn").unwrap().state, "started");
        assert!(chat_store
            .read_turn_trace("trace-session", "trace-turn")
            .unwrap()
            .is_none());
    }

    #[test]
    fn legacy_completed_turn_trace_replays_without_migration_or_rewrite() {
        let root = Root::new("turn-trace-legacy-completed");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_turn_trace_fixture(&mut store, &root, "trace-session", "trace-turn");
        let trace = golden_turn_trace(
            LEGACY_COMPLETED_TURN_TRACE_JSON,
            LEGACY_COMPLETED_TURN_TRACE_IDENTITY,
        );
        let finished = store
            .finish_turn_with_trace("trace-session", "trace-turn", "completed", 4, &trace)
            .unwrap();
        assert_eq!(finished.state, "completed");
        let stored = store
            .read_turn_trace("trace-session", "trace-turn")
            .unwrap()
            .unwrap();
        assert_eq!(stored.trace, trace);
        assert_eq!(stored.trace_identity, LEGACY_COMPLETED_TURN_TRACE_IDENTITY);
        let event_before: (String, String, String, i64) = store
            .connection
            .query_row(
                "SELECT event_id, payload_json, payload_sha256, payload_bytes FROM events
                 WHERE session_id = 'trace-session' AND sequence = ?1",
                [stored.event_sequence as i64],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
            )
            .unwrap();
        let payload_before = serde_json::from_str::<Value>(&event_before.1).unwrap();
        assert_eq!(
            payload_before["schema_version"],
            TURN_TRACE_RECORDED_SCHEMA_VERSION
        );
        assert_eq!(
            payload_before["trace"],
            serde_json::from_str::<Value>(LEGACY_COMPLETED_TURN_TRACE_JSON).unwrap()
        );
        let database_version: i64 = store
            .connection
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .unwrap();
        assert_eq!(database_version, SCHEMA_VERSION);
        let candidate = store
            .rebuild_session_projection_candidate("trace-session")
            .unwrap();
        assert!(candidate.source_complete);
        assert!(candidate.matches_current_projection);

        let current = completed_turn_trace_v2("trace-session", "trace-turn", 4);
        assert_eq!(
            store
                .finish_turn_with_trace("trace-session", "trace-turn", "completed", 4, &current,)
                .unwrap_err()
                .code,
            "turn-trace-conflict"
        );

        drop(store);
        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(
            reopened
                .read_turn_trace("trace-session", "trace-turn")
                .unwrap()
                .unwrap(),
            stored
        );
        let event_after: (String, String, String, i64) = reopened
            .connection
            .query_row(
                "SELECT event_id, payload_json, payload_sha256, payload_bytes FROM events
                 WHERE session_id = 'trace-session' AND sequence = ?1",
                [stored.event_sequence as i64],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
            )
            .unwrap();
        assert_eq!(event_after, event_before);
        let reopened_version: i64 = reopened
            .connection
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .unwrap();
        assert_eq!(reopened_version, SCHEMA_VERSION);
        assert!(!reopened.session_requires_recovery("trace-session"));
    }

    #[test]
    fn legacy_failed_turn_trace_replays_with_its_fixed_identity() {
        let root = Root::new("turn-trace-legacy-failed");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_turn_trace_fixture(&mut store, &root, "trace-session", "trace-turn");
        let trace = golden_turn_trace(
            LEGACY_FAILED_TURN_TRACE_JSON,
            LEGACY_FAILED_TURN_TRACE_IDENTITY,
        );
        store
            .finish_turn_with_trace("trace-session", "trace-turn", "failed", 4, &trace)
            .unwrap();
        let stored = store
            .read_turn_trace("trace-session", "trace-turn")
            .unwrap()
            .unwrap();
        assert_eq!(stored.trace_identity, LEGACY_FAILED_TURN_TRACE_IDENTITY);
        assert_eq!(stored.trace, trace);
        drop(store);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(
            reopened
                .read_turn_trace("trace-session", "trace-turn")
                .unwrap()
                .unwrap(),
            stored
        );
        let candidate = reopened
            .rebuild_session_projection_candidate("trace-session")
            .unwrap();
        assert!(candidate.source_complete);
        assert!(candidate.matches_current_projection);
        assert!(!reopened.session_requires_recovery("trace-session"));
    }

    #[test]
    fn current_completed_turn_trace_is_idempotent_and_restart_safe() {
        let root = Root::new("turn-trace-current-completed");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_turn_trace_fixture(&mut store, &root, "trace-session", "trace-turn");
        let trace = completed_turn_trace_v2("trace-session", "trace-turn", 4);
        assert_eq!(trace.schema_version, TURN_TRACE_SCHEMA_VERSION);
        let finished = store
            .finish_turn_with_trace("trace-session", "trace-turn", "completed", 4, &trace)
            .unwrap();
        assert_eq!(
            store
                .finish_turn_with_trace("trace-session", "trace-turn", "completed", 4, &trace)
                .unwrap(),
            finished
        );
        let stored = store
            .read_turn_trace("trace-session", "trace-turn")
            .unwrap()
            .unwrap();
        assert_eq!(stored.trace, trace);
        assert_eq!(stored.state, "completed");
        assert_eq!(
            store
                .latest_operation_event_state("trace-session", "trace-turn")
                .unwrap(),
            Some(EventState::Completed)
        );
        let candidate = store
            .rebuild_session_projection_candidate("trace-session")
            .unwrap();
        assert!(candidate.source_complete);
        assert!(candidate.matches_current_projection);
        drop(store);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(
            reopened
                .read_turn_trace("trace-session", "trace-turn")
                .unwrap()
                .unwrap(),
            stored
        );
        assert!(!reopened.session_requires_recovery("trace-session"));
    }

    #[test]
    fn store_rejects_legacy_new_fields_and_future_trace_versions_without_side_effects() {
        let root = Root::new("turn-trace-version-rejection");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_turn_trace_fixture(&mut store, &root, "trace-session", "trace-turn");
        let baseline_events = store
            .read_session_events("trace-session", 0, MAX_EVENT_PAGE)
            .unwrap();
        let current =
            serde_json::to_value(completed_turn_trace_v2("trace-session", "trace-turn", 4))
                .unwrap();
        let completion = current["events"][1]["payload"]["evidence"]["completion"].clone();

        let mut legacy_with_new_fields =
            serde_json::from_str::<Value>(LEGACY_COMPLETED_TURN_TRACE_JSON).unwrap();
        legacy_with_new_fields["events"][0]["payload"]["evidence"]["completion"] = completion;
        let legacy_with_new_fields =
            serde_json::from_value::<TurnTrace>(legacy_with_new_fields).unwrap();
        assert!(store
            .finish_turn_with_trace(
                "trace-session",
                "trace-turn",
                "completed",
                4,
                &legacy_with_new_fields,
            )
            .is_err());

        let mut future = serde_json::from_str::<Value>(LEGACY_COMPLETED_TURN_TRACE_JSON).unwrap();
        future["schema_version"] = Value::String("turn-trace/0.3".into());
        let future_event_payload = json!({
            "schema_version": TURN_TRACE_RECORDED_SCHEMA_VERSION,
            "trace": future.clone(),
            "trace_identity": LEGACY_COMPLETED_TURN_TRACE_IDENTITY,
            "state": "completed",
            "recorded_at_ms": 4,
            "content_included": false,
            "execution_authority": false
        });
        let future_event_bytes = serde_json::to_vec(&future_event_payload).unwrap();
        let future_event = WorkbenchEvent {
            schema_version: "workbench-event/0.1".into(),
            session_id: "trace-session".into(),
            sequence: 1,
            event_id: "future-turn-trace".into(),
            timestamp_ms: 4,
            correlation_id: "trace-turn".into(),
            event_kind: "turn.trace.recorded".into(),
            project_id: Some("trace-project".into()),
            operation_id: "trace-turn".into(),
            generation: 0,
            payload: future_event_payload,
            payload_hash: ContentHash::for_bytes(&future_event_bytes),
        };
        assert_eq!(
            parse_turn_trace_event(&future_event).unwrap_err().code,
            "turn-trace-event-trace-version-unsupported"
        );
        let future = serde_json::from_value::<TurnTrace>(future).unwrap();
        assert!(store
            .finish_turn_with_trace("trace-session", "trace-turn", "completed", 4, &future)
            .is_err());
        assert_eq!(store.load_turn("trace-turn").unwrap().state, "started");
        assert!(store
            .read_turn_trace("trace-session", "trace-turn")
            .unwrap()
            .is_none());
        assert_eq!(
            store
                .read_session_events("trace-session", 0, MAX_EVENT_PAGE)
                .unwrap(),
            baseline_events
        );
    }

    #[test]
    fn current_completion_semantic_tamper_quarantines_after_restart() {
        let root = Root::new("turn-trace-current-semantic-tamper");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_turn_trace_fixture(&mut store, &root, "trace-session", "trace-turn");
        let trace = completed_turn_trace_v2("trace-session", "trace-turn", 4);
        store
            .finish_turn_with_trace("trace-session", "trace-turn", "completed", 4, &trace)
            .unwrap();
        let stored = store
            .read_turn_trace("trace-session", "trace-turn")
            .unwrap()
            .unwrap();
        let payload_json: String = store
            .connection
            .query_row(
                "SELECT payload_json FROM events
                 WHERE session_id = 'trace-session' AND sequence = ?1",
                [stored.event_sequence as i64],
                |row| row.get(0),
            )
            .unwrap();
        let mut payload = serde_json::from_str::<Value>(&payload_json).unwrap();
        payload["trace"]["events"][1]["payload"]["evidence"]["completion"]["workspace_change"]
            ["state"] = Value::String("unknown".into());
        let tampered_json = serde_json::to_string(&payload).unwrap();
        let tampered_hash = ContentHash::for_bytes(tampered_json.as_bytes());
        store
            .connection
            .execute(
                "UPDATE events SET payload_json = ?1, payload_sha256 = ?2, payload_bytes = ?3
                 WHERE session_id = 'trace-session' AND sequence = ?4",
                params![
                    tampered_json,
                    tampered_hash.sha256,
                    tampered_hash.bytes as i64,
                    stored.event_sequence as i64,
                ],
            )
            .unwrap();
        let candidate = store
            .rebuild_session_projection_candidate("trace-session")
            .unwrap();
        assert!(!candidate.source_complete);
        assert!(candidate
            .issues
            .iter()
            .any(|issue| issue == "turn-trace-event-invalid"));
        drop(store);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert!(reopened.session_requires_recovery("trace-session"));
        assert_eq!(reopened.quarantined_session_count(), 1);
        assert!(reopened
            .read_turn_trace("trace-session", "trace-turn")
            .is_err());
    }

    #[test]
    fn current_trace_mode_semantic_tamper_quarantines_after_restart() {
        let root = Root::new("turn-trace-mode-semantic-tamper");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_turn_trace_fixture(&mut store, &root, "trace-session", "trace-turn");
        let trace = completed_turn_trace_v2("trace-session", "trace-turn", 4);
        store
            .finish_turn_with_trace("trace-session", "trace-turn", "completed", 4, &trace)
            .unwrap();
        let stored = store
            .read_turn_trace("trace-session", "trace-turn")
            .unwrap()
            .unwrap();
        let chat_trace = completed_chat_turn_trace_v2("trace-session", "trace-turn", 4);
        let chat_trace_identity = chat_trace.metadata_identity().unwrap();
        let payload_json: String = store
            .connection
            .query_row(
                "SELECT payload_json FROM events
                 WHERE session_id = 'trace-session' AND sequence = ?1",
                [stored.event_sequence as i64],
                |row| row.get(0),
            )
            .unwrap();
        let mut payload = serde_json::from_str::<Value>(&payload_json).unwrap();
        payload["trace"] = serde_json::to_value(&chat_trace).unwrap();
        payload["trace_identity"] = Value::String(chat_trace_identity.clone());
        let tampered_json = serde_json::to_string(&payload).unwrap();
        let tampered_hash = ContentHash::for_bytes(tampered_json.as_bytes());
        let tampered_event_id = derived_event_id(
            "turn-trace-recorded",
            format!("trace-turn\0{chat_trace_identity}").as_bytes(),
        );
        store
            .connection
            .execute(
                "UPDATE events
                 SET event_id = ?1, payload_json = ?2, payload_sha256 = ?3, payload_bytes = ?4
                 WHERE session_id = 'trace-session' AND sequence = ?5",
                params![
                    tampered_event_id,
                    tampered_json,
                    tampered_hash.sha256,
                    tampered_hash.bytes as i64,
                    stored.event_sequence as i64,
                ],
            )
            .unwrap();
        assert_eq!(
            store
                .read_turn_trace("trace-session", "trace-turn")
                .unwrap_err()
                .code,
            "turn-trace-binding-mismatch"
        );
        let candidate = store
            .rebuild_session_projection_candidate("trace-session")
            .unwrap();
        assert!(!candidate.source_complete);
        assert!(candidate
            .issues
            .iter()
            .any(|issue| issue == "turn-trace-event-invalid"));
        drop(store);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert!(reopened.session_requires_recovery("trace-session"));
        assert_eq!(reopened.quarantined_session_count(), 1);
    }

    #[test]
    fn terminal_turn_trace_event_failure_rolls_back_and_semantic_tamper_quarantines() {
        let root = Root::new("turn-trace-rollback-tamper");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_turn_trace_fixture(&mut store, &root, "trace-session", "trace-turn");
        let trace = failed_turn_trace("trace-session", "trace-turn", "trace-terminal", 4);
        store
            .connection
            .execute_batch(
                "CREATE TEMP TRIGGER fail_turn_terminal_event
                 BEFORE INSERT ON events
                 WHEN NEW.event_kind = 'turn.failed'
                 BEGIN SELECT RAISE(ABORT, 'injected turn terminal failure'); END;",
            )
            .unwrap();
        assert!(store
            .finish_turn_with_trace("trace-session", "trace-turn", "failed", 4, &trace)
            .is_err());
        assert_eq!(store.load_turn("trace-turn").unwrap().state, "started");
        assert!(store
            .read_turn_trace("trace-session", "trace-turn")
            .unwrap()
            .is_none());
        assert_eq!(
            store
                .latest_operation_event_state("trace-session", "trace-turn")
                .unwrap(),
            Some(EventState::Running)
        );
        store
            .connection
            .execute_batch("DROP TRIGGER fail_turn_terminal_event;")
            .unwrap();
        store
            .finish_turn_with_trace("trace-session", "trace-turn", "failed", 4, &trace)
            .unwrap();
        let stored = store
            .read_turn_trace("trace-session", "trace-turn")
            .unwrap()
            .unwrap();
        let payload_json: String = store
            .connection
            .query_row(
                "SELECT payload_json FROM events
                 WHERE session_id = 'trace-session' AND sequence = ?1",
                [stored.event_sequence as i64],
                |row| row.get(0),
            )
            .unwrap();
        let mut payload: Value = serde_json::from_str(&payload_json).unwrap();
        payload["trace"]["unrecognized_content"] =
            Value::String("repository body must never enter a trace".into());
        let tampered_json = serde_json::to_string(&payload).unwrap();
        let tampered_hash = ContentHash::for_bytes(tampered_json.as_bytes());
        store
            .connection
            .execute(
                "UPDATE events SET payload_json = ?1, payload_sha256 = ?2, payload_bytes = ?3
                 WHERE session_id = 'trace-session' AND sequence = ?4",
                params![
                    tampered_json,
                    tampered_hash.sha256,
                    tampered_hash.bytes as i64,
                    stored.event_sequence as i64,
                ],
            )
            .unwrap();
        let candidate = store
            .rebuild_session_projection_candidate("trace-session")
            .unwrap();
        assert!(!candidate.source_complete);
        assert!(candidate
            .issues
            .iter()
            .any(|issue| issue == "turn-trace-event-invalid"));

        drop(store);
        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert!(reopened.session_requires_recovery("trace-session"));
        assert_eq!(reopened.quarantined_session_count(), 1);
        assert!(reopened
            .read_turn_trace("trace-session", "trace-turn")
            .is_err());
    }

    #[test]
    fn terminal_turn_trace_rejects_outer_terminal_tampering() {
        let root = Root::new("turn-trace-terminal-tamper");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_turn_trace_fixture(&mut store, &root, "trace-session", "trace-turn");
        let trace = failed_turn_trace("trace-session", "trace-turn", "trace-terminal", 4);
        store
            .finish_turn_with_trace("trace-session", "trace-turn", "failed", 4, &trace)
            .unwrap();
        let stored = store
            .read_turn_trace("trace-session", "trace-turn")
            .unwrap()
            .unwrap();
        let terminal_sequence = stored.event_sequence + 1;
        store
            .connection
            .execute(
                "UPDATE events SET operation_id = 'other-turn'
                 WHERE session_id = 'trace-session' AND sequence = ?1",
                [terminal_sequence as i64],
            )
            .unwrap();

        assert!(store
            .read_turn_trace("trace-session", "trace-turn")
            .is_err());
        assert_eq!(
            store
                .latest_operation_event_state("trace-session", "trace-turn")
                .unwrap(),
            None
        );
        let candidate = store
            .rebuild_session_projection_candidate("trace-session")
            .unwrap();
        assert!(!candidate.source_complete);
        assert!(candidate
            .issues
            .iter()
            .any(|issue| issue == "turn-terminal-event-invalid"));

        drop(store);
        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert!(reopened.session_requires_recovery("trace-session"));
        assert_eq!(reopened.quarantined_session_count(), 1);
        assert!(reopened
            .read_turn_trace("trace-session", "trace-turn")
            .is_err());
    }

    #[test]
    fn session_projection_verifier_reports_content_free_integrity_failures() {
        let root = Root::new("session-projection-consistency");
        let project_root = root.parent.canonicalize().unwrap();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_project(StoredProjectCreate {
                project_id: "project-consistency".into(),
                root_id: "root-consistency".into(),
                canonical_root: project_root.to_string_lossy().into_owned(),
                root_identity: "root-consistency".into(),
                display_name: "Consistency".into(),
                root_access: "read".into(),
                created_at_ms: 1,
            })
            .unwrap();
        for (session_id, mode) in [
            ("session-consistency", StoredSessionMode::Chat),
            ("session-other", StoredSessionMode::Work),
        ] {
            store
                .create_session(StoredSessionCreate {
                    session_id: session_id.into(),
                    project_id: Some("project-consistency".into()),
                    mode,
                    title: session_id.into(),
                    parent_session_id: None,
                    lineage_kind: StoredSessionLineage::New,
                    environment_identity: None,
                    created_at_ms: 2,
                })
                .unwrap();
        }
        for (turn_id, session_id) in [
            ("turn-consistency", "session-consistency"),
            ("turn-other", "session-other"),
        ] {
            store
                .create_turn(StoredTurnCreate {
                    turn_id: turn_id.into(),
                    session_id: session_id.into(),
                    idempotency_key: None,
                    input_hash: ContentHash::for_bytes(turn_id.as_bytes()),
                    created_at_ms: 3,
                })
                .unwrap();
        }
        for (item_id, role, created_at_ms) in [
            ("item-consistency-1", "user", 4),
            ("item-consistency-2", "agent", 5),
        ] {
            store
                .append_item(StoredItemAppend {
                    session_id: "session-consistency".into(),
                    turn_id: Some("turn-consistency".into()),
                    item_id: item_id.into(),
                    item_kind: "message".into(),
                    role: role.into(),
                    state: "completed".into(),
                    payload: json!({"content": item_id}),
                    created_at_ms,
                })
                .unwrap();
        }
        let transaction = store
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .unwrap();
        append_event_tx(
            &transaction,
            EventInput {
                session_id: "session-consistency",
                event_id: "session-consistency-event-1",
                timestamp_ms: 6,
                correlation_id: "session-consistency",
                event_kind: "session.fixture",
                project_id: Some("project-consistency"),
                operation_id: "session-consistency",
                generation: 0,
                payload: json!({"schema_version": "session.fixture/0.1"}),
            },
        )
        .unwrap();
        transaction.commit().unwrap();

        let healthy = store
            .verify_session_projection("session-consistency")
            .unwrap();
        assert!(healthy.consistent);
        assert!(!healthy.read_only_recovery_required);
        assert_eq!(healthy.checked_turns, 1);
        assert_eq!(healthy.checked_items, 2);
        assert_eq!(healthy.checked_events, 5);
        assert_eq!(healthy.latest_item_sequence, 2);
        assert_eq!(healthy.latest_event_sequence, 5);
        assert!(healthy.rebuild_source_complete);
        assert!(healthy.event_projection_matches);
        assert!(healthy.blob_store_available);
        assert_eq!(healthy.checked_blob_references, 0);
        assert!(healthy.issues.is_empty());
        let candidate = store
            .rebuild_session_projection_candidate("session-consistency")
            .unwrap();
        assert!(candidate.source_complete);
        assert!(candidate.matches_current_projection);
        assert_eq!(candidate.turns.len(), 1);
        assert_eq!(candidate.items.len(), 2);
        assert_eq!(
            candidate.session.as_ref().unwrap().title,
            "session-consistency"
        );

        store
            .connection
            .execute(
                "UPDATE sessions SET title = 'projection drift'
                 WHERE session_id = 'session-consistency'",
                [],
            )
            .unwrap();
        let drifted = store
            .rebuild_session_projection_candidate("session-consistency")
            .unwrap();
        assert!(drifted.source_complete);
        assert!(!drifted.matches_current_projection);
        assert!(drifted
            .issues
            .iter()
            .any(|issue| issue == "event-projection-mismatch"));
        assert!(
            !store
                .verify_session_projection("session-consistency")
                .unwrap()
                .consistent
        );
        let rebuilt = store
            .rebuild_session_projection("session-consistency")
            .unwrap();
        assert!(rebuilt.consistent);
        assert!(rebuilt.event_projection_matches);
        assert_eq!(
            store.load_session("session-consistency").unwrap().title,
            "session-consistency"
        );

        store
            .connection
            .execute(
                "UPDATE sessions
                 SET parent_session_id = 'session-other', lineage_kind = 'resume'
                 WHERE session_id = 'session-consistency'",
                [],
            )
            .unwrap();
        store
            .connection
            .execute("DELETE FROM items WHERE item_id = 'item-consistency-1'", [])
            .unwrap();
        store
            .connection
            .execute(
                "UPDATE items SET turn_id = 'turn-other'
                 WHERE item_id = 'item-consistency-2'",
                [],
            )
            .unwrap();
        let damaged_projection = store
            .verify_session_projection("session-consistency")
            .unwrap();
        assert!(!damaged_projection.consistent);
        let repaired = store
            .rebuild_session_projection("session-consistency")
            .unwrap();
        assert!(repaired.consistent);
        assert!(repaired.event_projection_matches);
        assert_eq!(
            store
                .read_session_items("session-consistency", 0, 20)
                .unwrap()
                .len(),
            2
        );

        store
            .connection
            .execute(
                "UPDATE sessions
                 SET parent_session_id = 'session-other', lineage_kind = 'resume'
                 WHERE session_id = 'session-consistency'",
                [],
            )
            .unwrap();
        store
            .connection
            .execute("DELETE FROM items WHERE item_id = 'item-consistency-1'", [])
            .unwrap();
        store
            .connection
            .execute(
                "UPDATE items
                 SET turn_id = 'turn-other', payload_json = '{\"tampered\":true}'
                 WHERE item_id = 'item-consistency-2'",
                [],
            )
            .unwrap();
        store
            .connection
            .execute(
                "UPDATE events SET payload_json = '{\"tampered\":true}'
                 WHERE event_id = 'session-consistency-event-1'",
                [],
            )
            .unwrap();
        store
            .connection
            .execute(
                "UPDATE session_sequences SET next_sequence = 9
                 WHERE session_id = 'session-consistency'",
                [],
            )
            .unwrap();

        let damaged = store
            .verify_session_projection("session-consistency")
            .unwrap();
        assert!(!damaged.consistent);
        assert!(damaged.read_only_recovery_required);
        for issue in [
            "session-lineage-binding-mismatch",
            "item-sequence-gap",
            "item-turn-binding-invalid",
            "item-payload-or-sequence-invalid",
            "event-sequence-cursor-mismatch",
            "event-payload-or-sequence-invalid",
        ] {
            assert!(damaged.issues.iter().any(|candidate| candidate == issue));
        }
        assert!(store.verify_session_projection("missing-session").is_err());
    }

    #[test]
    fn session_projection_event_failure_rolls_back_metadata_and_detects_missing_source() {
        let root = Root::new("session-projection-atomicity");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let original = store
            .create_session(StoredSessionCreate {
                session_id: "session-atomicity".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Original".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 1,
            })
            .unwrap();
        store
            .connection
            .execute_batch(
                "CREATE TEMP TRIGGER reject_session_title_event
                 BEFORE INSERT ON events
                 WHEN NEW.event_kind = 'session.title-updated'
                 BEGIN
                    SELECT RAISE(ABORT, 'injected projection event failure');
                 END;",
            )
            .unwrap();
        assert!(store
            .update_session_title("session-atomicity", "Must roll back", 2)
            .is_err());
        assert_eq!(store.load_session("session-atomicity").unwrap(), original);
        assert_eq!(
            store
                .read_session_events("session-atomicity", 0, 20)
                .unwrap()
                .len(),
            1
        );
        let next_sequence: i64 = store
            .connection
            .query_row(
                "SELECT next_sequence FROM session_sequences WHERE session_id = ?1",
                ["session-atomicity"],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(next_sequence, 2);
        store
            .connection
            .execute_batch("DROP TRIGGER reject_session_title_event;")
            .unwrap();

        store
            .connection
            .execute(
                "DELETE FROM events WHERE session_id = 'session-atomicity'",
                [],
            )
            .unwrap();
        store
            .connection
            .execute(
                "DELETE FROM session_sequences WHERE session_id = 'session-atomicity'",
                [],
            )
            .unwrap();
        let missing_source = store
            .verify_session_projection("session-atomicity")
            .unwrap();
        assert!(!missing_source.consistent);
        assert!(missing_source.read_only_recovery_required);
        assert!(!missing_source.rebuild_source_complete);
        assert!(missing_source
            .issues
            .iter()
            .any(|issue| issue == "projection-event-source-incomplete"));
    }

    #[test]
    fn session_projection_rebuild_rejects_a_source_changed_after_review() {
        let root = Root::new("session-projection-stale-source");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "session-stale-source".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Stable".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 1,
            })
            .unwrap();
        store
            .connection
            .execute(
                "UPDATE sessions SET title = 'Drifted'
                 WHERE session_id = 'session-stale-source'",
                [],
            )
            .unwrap();
        let reviewed = store
            .rebuild_session_projection_candidate("session-stale-source")
            .unwrap();
        assert!(reviewed.source_complete);
        assert!(!reviewed.matches_current_projection);
        let transaction = store
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .unwrap();
        append_event_tx(
            &transaction,
            EventInput {
                session_id: "session-stale-source",
                event_id: "session-stale-source-later-event",
                timestamp_ms: 2,
                correlation_id: "session-stale-source",
                event_kind: "session.fixture-later",
                project_id: None,
                operation_id: "session-stale-source",
                generation: 0,
                payload: json!({"schema_version": "session.fixture/0.1"}),
            },
        )
        .unwrap();
        transaction.commit().unwrap();

        assert!(store.apply_session_projection_candidate(&reviewed).is_err());
        let fresh = store
            .rebuild_session_projection_candidate("session-stale-source")
            .unwrap();
        assert!(fresh.source_complete);
        assert!(!fresh.matches_current_projection);
        assert_ne!(fresh.source_hash, reviewed.source_hash);
        let repaired = store
            .rebuild_session_projection("session-stale-source")
            .unwrap();
        assert!(repaired.consistent);
        assert!(repaired.event_projection_matches);
        assert_eq!(
            store.load_session("session-stale-source").unwrap().title,
            "Stable"
        );
    }

    #[test]
    fn automatic_projection_recovery_requires_complete_untampered_event_authority() {
        let root = Root::new("automatic-session-projection-recovery");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "session-auto-repair".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Authoritative".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 1,
            })
            .unwrap();
        store
            .create_turn(StoredTurnCreate {
                turn_id: "turn-auto-repair".into(),
                session_id: "session-auto-repair".into(),
                idempotency_key: Some("auto-repair-turn".into()),
                input_hash: ContentHash::for_bytes(b"repair"),
                created_at_ms: 2,
            })
            .unwrap();
        for (item_id, created_at_ms) in [("item-auto-1", 3), ("item-auto-2", 4)] {
            store
                .append_item(StoredItemAppend {
                    session_id: "session-auto-repair".into(),
                    turn_id: Some("turn-auto-repair".into()),
                    item_id: item_id.into(),
                    item_kind: "message".into(),
                    role: "assistant".into(),
                    state: "completed".into(),
                    payload: json!({"content": item_id}),
                    created_at_ms,
                })
                .unwrap();
        }
        store
            .connection
            .execute_batch(
                "UPDATE sessions SET title = 'Projection drift'
                    WHERE session_id = 'session-auto-repair';
                 DELETE FROM items WHERE item_id = 'item-auto-1';",
            )
            .unwrap();

        let (repaired, rebuilt) = store
            .verify_or_rebuild_session_projection("session-auto-repair")
            .unwrap();
        assert!(rebuilt);
        assert!(repaired.consistent);
        assert_eq!(
            store.load_session("session-auto-repair").unwrap().title,
            "Authoritative"
        );
        assert_eq!(
            store
                .read_session_items("session-auto-repair", 0, 20)
                .unwrap()
                .len(),
            2
        );
        let audit_count: i64 = store
            .connection
            .query_row(
                "SELECT COUNT(*) FROM events
                 WHERE session_id = 'session-auto-repair'
                   AND event_kind = 'session.projection-rebuilt'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(audit_count, 1);

        store
            .create_session(StoredSessionCreate {
                session_id: "session-auto-blocked".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Original".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 10,
            })
            .unwrap();
        store
            .connection
            .execute_batch(
                "UPDATE sessions SET title = 'Must remain drifted'
                    WHERE session_id = 'session-auto-blocked';
                 UPDATE events SET payload_json = '{\"tampered\":true}'
                    WHERE session_id = 'session-auto-blocked'
                      AND event_kind = 'session.created';",
            )
            .unwrap();
        let (blocked, rebuilt) = store
            .verify_or_rebuild_session_projection("session-auto-blocked")
            .unwrap();
        assert!(!rebuilt);
        assert!(!blocked.consistent);
        assert!(blocked
            .issues
            .iter()
            .any(|issue| issue == "event-payload-or-sequence-invalid"));
        assert_eq!(
            store.load_session("session-auto-blocked").unwrap().title,
            "Must remain drifted"
        );
        let blocked_audits: i64 = store
            .connection
            .query_row(
                "SELECT COUNT(*) FROM events
                 WHERE session_id = 'session-auto-blocked'
                   AND event_kind = 'session.projection-rebuilt'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(blocked_audits, 0);
    }

    #[test]
    fn startup_recovery_rebuilds_missing_lineage_in_parent_first_order() {
        let root = Root::new("startup-missing-session-lineage");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "session-startup-parent".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Parent".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 1,
            })
            .unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "session-startup-child".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Child".into(),
                parent_session_id: Some("session-startup-parent".into()),
                lineage_kind: StoredSessionLineage::Resume,
                environment_identity: None,
                created_at_ms: 2,
            })
            .unwrap();
        drop(store);

        let connection = Connection::open(root.path.join(DATABASE_FILE)).unwrap();
        connection
            .execute_batch(
                "PRAGMA foreign_keys = OFF;
                 DELETE FROM sessions
                    WHERE session_id IN ('session-startup-child', 'session-startup-parent');",
            )
            .unwrap();
        drop(connection);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        let report = reopened.startup_projection_recovery();
        assert_eq!(report.checked_sessions, 2);
        assert_eq!(report.healthy_sessions, 0);
        assert_eq!(report.rebuilt_sessions, 2);
        assert_eq!(report.quarantined_sessions, 0);
        assert!(report.issues.is_empty());
        let parent = reopened.load_session("session-startup-parent").unwrap();
        let child = reopened.load_session("session-startup-child").unwrap();
        assert_eq!(parent.title, "Parent");
        assert_eq!(child.parent_session_id, Some(parent.session_id));
        assert_eq!(child.lineage_kind, StoredSessionLineage::Resume);
        let audit_count: i64 = reopened
            .connection
            .query_row(
                "SELECT COUNT(*) FROM events
                 WHERE event_kind = 'session.projection-rebuilt'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(audit_count, 2);
    }

    #[test]
    fn startup_recovery_rebuilds_a_missing_multi_root_project_before_sessions() {
        let root = Root::new("startup-missing-multi-root-project");
        let primary_root = root.parent.join("primary-project");
        let additional_root = root.parent.join("additional-project");
        fs::create_dir(&primary_root).unwrap();
        fs::create_dir(&additional_root).unwrap();
        let primary_root = primary_root.canonicalize().unwrap();
        let additional_root = additional_root.canonicalize().unwrap();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_project(StoredProjectCreate {
                project_id: "project-multi-root".into(),
                root_id: "root-primary".into(),
                canonical_root: primary_root.to_string_lossy().into_owned(),
                root_identity: "path:sha256:primary-root".into(),
                display_name: "Multi root".into(),
                root_access: "write".into(),
                created_at_ms: 10,
            })
            .unwrap();
        store
            .add_project_root(StoredProjectRootAdd {
                project_id: "project-multi-root".into(),
                root_id: "root-additional".into(),
                canonical_root: additional_root.to_string_lossy().into_owned(),
                root_identity: "path:sha256:additional-root".into(),
                access: "read".into(),
                created_at_ms: 20,
            })
            .unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "session-multi-root".into(),
                project_id: Some("project-multi-root".into()),
                mode: StoredSessionMode::Work,
                title: "Project session".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 30,
            })
            .unwrap();
        drop(store);

        let connection = Connection::open(root.path.join(DATABASE_FILE)).unwrap();
        connection
            .execute_batch(
                "PRAGMA foreign_keys = OFF;
                 DELETE FROM project_roots WHERE project_id = 'project-multi-root';
                 DELETE FROM projects WHERE project_id = 'project-multi-root';",
            )
            .unwrap();
        drop(connection);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        let report = reopened.startup_projection_recovery();
        assert_eq!(report.checked_projects, 1);
        assert_eq!(report.rebuilt_projects, 1);
        assert_eq!(report.quarantined_projects, 0);
        assert_eq!(report.quarantined_sessions, 0);
        let project = reopened.load_project("project-multi-root").unwrap();
        assert_eq!(project.updated_at_ms, 20);
        let roots = reopened.load_project_roots("project-multi-root").unwrap();
        assert_eq!(roots.len(), 2);
        assert_eq!(roots[0].root_id, "root-additional");
        assert_eq!(roots[0].access, "read");
        assert_eq!(roots[1].root_id, "root-primary");
        assert_eq!(roots[1].access, "write");
        assert_eq!(
            reopened
                .load_session("session-multi-root")
                .unwrap()
                .project_id
                .as_deref(),
            Some("project-multi-root")
        );
        let candidate = reopened
            .rebuild_project_projection_candidate("project-multi-root")
            .unwrap();
        assert!(candidate.source_complete);
        assert!(candidate.matches_current_projection);
    }

    #[test]
    fn project_projection_storage_failure_rolls_back_without_audit() {
        let root = Root::new("project-projection-storage-failure");
        let primary_root = root.parent.join("storage-primary");
        let additional_root = root.parent.join("storage-additional");
        fs::create_dir(&primary_root).unwrap();
        fs::create_dir(&additional_root).unwrap();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_project(StoredProjectCreate {
                project_id: "project-storage-failure".into(),
                root_id: "root-primary".into(),
                canonical_root: primary_root
                    .canonicalize()
                    .unwrap()
                    .to_string_lossy()
                    .into_owned(),
                root_identity: "path:sha256:storage-primary".into(),
                display_name: "Storage failure".into(),
                root_access: "write".into(),
                created_at_ms: 10,
            })
            .unwrap();
        store
            .add_project_root(StoredProjectRootAdd {
                project_id: "project-storage-failure".into(),
                root_id: "root-additional".into(),
                canonical_root: additional_root
                    .canonicalize()
                    .unwrap()
                    .to_string_lossy()
                    .into_owned(),
                root_identity: "path:sha256:storage-additional".into(),
                access: "write".into(),
                created_at_ms: 20,
            })
            .unwrap();
        store
            .connection
            .execute(
                "UPDATE project_roots SET access = 'read'
                 WHERE project_id = 'project-storage-failure'
                   AND root_id = 'root-additional'",
                [],
            )
            .unwrap();
        let candidate = store
            .rebuild_project_projection_candidate("project-storage-failure")
            .unwrap();
        assert!(project_projection_candidate_is_rebuildable(&candidate));
        store
            .connection
            .execute_batch(
                "CREATE TRIGGER fail_project_projection_root_insert
                 BEFORE INSERT ON project_roots
                 BEGIN
                    SELECT RAISE(ABORT, 'injected project projection storage failure');
                 END;",
            )
            .unwrap();
        assert!(store
            .apply_project_projection_candidate(&candidate)
            .is_err());
        let after_failure = store.load_project_roots("project-storage-failure").unwrap();
        assert_eq!(after_failure.len(), 2);
        assert_eq!(after_failure[0].access, "read");
        let audit_count: i64 = store
            .connection
            .query_row(
                "SELECT COUNT(*) FROM events
                 WHERE event_kind = 'project.projection-rebuilt'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(audit_count, 0);
        store
            .connection
            .execute_batch("DROP TRIGGER fail_project_projection_root_insert;")
            .unwrap();
        assert!(store
            .verify_or_rebuild_project_projection("project-storage-failure")
            .unwrap());
        assert_eq!(
            store.load_project_roots("project-storage-failure").unwrap()[0].access,
            "write"
        );
    }

    #[test]
    fn project_projection_rebuild_process_exit_child() {
        let Ok(data_root) = std::env::var("AEGISY_TEST_PROJECTION_REBUILD_DATA_ROOT") else {
            return;
        };
        let mut store = WorkbenchStore::open(Path::new(&data_root)).unwrap();
        store
            .connection
            .execute(
                "UPDATE project_roots SET access = 'read'
                 WHERE project_id = 'project-crash-rebuild'
                   AND root_id = 'root-additional'",
                [],
            )
            .unwrap();
        let candidate = store
            .rebuild_project_projection_candidate("project-crash-rebuild")
            .unwrap();
        assert!(project_projection_candidate_is_rebuildable(&candidate));
        store
            .apply_project_projection_candidate(&candidate)
            .unwrap();
        panic!("projection rebuild crash point did not terminate the child");
    }

    #[test]
    fn project_projection_process_exit_rolls_back_and_next_startup_recovers() {
        let root = Root::new("project-projection-process-exit");
        let primary_root = root.parent.join("crash-primary");
        let additional_root = root.parent.join("crash-additional");
        fs::create_dir(&primary_root).unwrap();
        fs::create_dir(&additional_root).unwrap();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_project(StoredProjectCreate {
                project_id: "project-crash-rebuild".into(),
                root_id: "root-primary".into(),
                canonical_root: primary_root
                    .canonicalize()
                    .unwrap()
                    .to_string_lossy()
                    .into_owned(),
                root_identity: "path:sha256:crash-primary".into(),
                display_name: "Crash rebuild".into(),
                root_access: "write".into(),
                created_at_ms: 10,
            })
            .unwrap();
        store
            .add_project_root(StoredProjectRootAdd {
                project_id: "project-crash-rebuild".into(),
                root_id: "root-additional".into(),
                canonical_root: additional_root
                    .canonicalize()
                    .unwrap()
                    .to_string_lossy()
                    .into_owned(),
                root_identity: "path:sha256:crash-additional".into(),
                access: "write".into(),
                created_at_ms: 20,
            })
            .unwrap();
        drop(store);

        let status = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "workbench_store::tests::project_projection_rebuild_process_exit_child",
                "--nocapture",
            ])
            .env("AEGISY_TEST_PROJECTION_REBUILD_DATA_ROOT", &root.path)
            .env(
                "AEGISY_TEST_PROJECTION_REBUILD_CRASH_POINT",
                "project-after-root-clear",
            )
            .status()
            .unwrap();
        assert_eq!(status.code(), Some(86));

        let connection = Connection::open(root.path.join(DATABASE_FILE)).unwrap();
        let preserved: (i64, String) = connection
            .query_row(
                "SELECT COUNT(*), MIN(access) FROM project_roots
                 WHERE project_id = 'project-crash-rebuild'",
                [],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .unwrap();
        assert_eq!(preserved, (2, "read".into()));
        let audit_count: i64 = connection
            .query_row(
                "SELECT COUNT(*) FROM events
                 WHERE event_kind = 'project.projection-rebuilt'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(audit_count, 0);
        drop(connection);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(reopened.startup_projection_recovery().rebuilt_projects, 1);
        let roots = reopened
            .load_project_roots("project-crash-rebuild")
            .unwrap();
        assert_eq!(roots.len(), 2);
        assert!(roots.iter().all(|root| root.access == "write"));
        let candidate = reopened
            .rebuild_project_projection_candidate("project-crash-rebuild")
            .unwrap();
        assert!(candidate.matches_current_projection);
    }

    #[test]
    fn tampered_project_event_quarantines_its_sessions_without_rewrite() {
        let root = Root::new("tampered-project-projection-source");
        let project_root = root.parent.join("tampered-project");
        fs::create_dir(&project_root).unwrap();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_project(StoredProjectCreate {
                project_id: "project-tampered-source".into(),
                root_id: "root-primary".into(),
                canonical_root: project_root
                    .canonicalize()
                    .unwrap()
                    .to_string_lossy()
                    .into_owned(),
                root_identity: "path:sha256:tampered-project".into(),
                display_name: "Tampered source".into(),
                root_access: "write".into(),
                created_at_ms: 10,
            })
            .unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "session-tampered-project".into(),
                project_id: Some("project-tampered-source".into()),
                mode: StoredSessionMode::Work,
                title: "Must quarantine".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 20,
            })
            .unwrap();
        let stream_id = project_event_stream_id("project-tampered-source");
        store
            .connection
            .execute(
                "UPDATE events SET payload_json = '{\"tampered\":true}'
                 WHERE session_id = ?1 AND event_kind = 'project.created'",
                [&stream_id],
            )
            .unwrap();
        drop(store);

        let mut reopened = WorkbenchStore::open(&root.path).unwrap();
        let report = reopened.startup_projection_recovery();
        assert_eq!(report.quarantined_projects, 1);
        assert_eq!(report.quarantined_sessions, 1);
        assert!(reopened.session_requires_recovery("session-tampered-project"));
        let additional_root = root.parent.join("blocked-additional-root");
        fs::create_dir(&additional_root).unwrap();
        let error = reopened
            .add_project_root(StoredProjectRootAdd {
                project_id: "project-tampered-source".into(),
                root_id: "root-blocked".into(),
                canonical_root: additional_root
                    .canonicalize()
                    .unwrap()
                    .to_string_lossy()
                    .into_owned(),
                root_identity: "path:sha256:blocked-root".into(),
                access: "read".into(),
                created_at_ms: 30,
            })
            .unwrap_err();
        assert_eq!(error.code, "project-read-only-recovery");
        let audit_count: i64 = reopened
            .connection
            .query_row(
                "SELECT COUNT(*) FROM events
                 WHERE event_kind = 'project.projection-rebuilt'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(audit_count, 0);
    }

    fn create_rebuildable_session(store: &mut WorkbenchStore, session_id: &str) {
        store
            .create_session(StoredSessionCreate {
                session_id: session_id.into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Authoritative title".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 10,
            })
            .unwrap();
        store
            .create_turn(StoredTurnCreate {
                turn_id: format!("turn-{session_id}"),
                session_id: session_id.into(),
                idempotency_key: Some(format!("key-{session_id}")),
                input_hash: ContentHash::for_bytes(b"rebuild input"),
                created_at_ms: 20,
            })
            .unwrap();
        store
            .append_item(StoredItemAppend {
                session_id: session_id.into(),
                turn_id: Some(format!("turn-{session_id}")),
                item_id: format!("item-{session_id}"),
                item_kind: "message".into(),
                role: "assistant".into(),
                state: "completed".into(),
                payload: json!({"text": "durable item"}),
                created_at_ms: 30,
            })
            .unwrap();
    }

    fn create_child_session(
        store: &mut WorkbenchStore,
        session_id: &str,
        parent_session_id: &str,
        created_at_ms: u64,
    ) {
        store
            .create_session(StoredSessionCreate {
                session_id: session_id.into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: format!("Child {session_id}"),
                parent_session_id: Some(parent_session_id.into()),
                lineage_kind: StoredSessionLineage::Fork,
                environment_identity: None,
                created_at_ms,
            })
            .unwrap();
    }

    fn session_artifact_request(session_id: &str, created_at_ms: u64) -> DurableBlobWrite {
        let content = b"session deletion artifact".to_vec();
        let sha256 = sha256_hex(&content);
        let content_reference = format!("command-output:sha256:{sha256}");
        DurableBlobWrite {
            reference_id: durable_blob_reference_id(
                Some(session_id),
                None,
                "session",
                session_id,
                &content_reference,
            ),
            content_reference,
            session_id: Some(session_id.into()),
            project_id: None,
            kind: DurableBlobKind::CommandOutput,
            media_type: "text/plain; charset=utf-8".into(),
            owner_kind: "session".into(),
            owner_id: session_id.into(),
            metadata: json!({"retention": "session-deletion-test"}),
            content,
            created_at_ms,
            retain_until_ms: created_at_ms + MIN_BLOB_RETENTION_MS,
        }
    }

    #[test]
    fn session_deletion_preview_binds_scope_descendants_artifacts_and_staleness() {
        let root = Root::new("session-deletion-preview");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_rebuildable_session(&mut store, "delete-parent");
        create_child_session(&mut store, "delete-child", "delete-parent", 40);
        store
            .put_durable_blob(session_artifact_request("delete-parent", 50))
            .unwrap();

        let session_only = store
            .preview_session_deletion("delete-parent", SessionDeletionScope::SessionOnly)
            .unwrap();
        assert_eq!(session_only.session_count, 1);
        assert_eq!(session_only.descendant_count, 1);
        assert_eq!(session_only.artifact_reference_count, 1);
        assert_eq!(session_only.affected_sessions.len(), 2);
        let lineage = store
            .preview_session_deletion("delete-parent", SessionDeletionScope::Lineage)
            .unwrap();
        assert_eq!(lineage.session_count, 2);
        assert_ne!(lineage.plan_hash, session_only.plan_hash);
        assert!(lineage.blocking_reasons.is_empty());

        create_child_session(&mut store, "delete-late-child", "delete-parent", 60);
        let error = store
            .schedule_session_deletion(
                "deletion-stale-plan",
                "delete-parent",
                SessionDeletionScope::Lineage,
                &lineage.plan_hash,
                70,
                MIN_SESSION_DELETE_UNDO_MS,
            )
            .unwrap_err();
        assert_eq!(error.code, "session-deletion-plan-stale");
        assert!(store
            .session_deletion_for_session("delete-parent")
            .unwrap()
            .is_none());
    }

    #[test]
    fn pending_session_deletion_freezes_writes_remains_readable_and_undoes() {
        let root = Root::new("session-deletion-undo");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_rebuildable_session(&mut store, "undo-parent");
        create_child_session(&mut store, "undo-child", "undo-parent", 40);
        let artifact = store
            .put_durable_blob(session_artifact_request("undo-parent", 50))
            .unwrap();
        let preview = store
            .preview_session_deletion("undo-parent", SessionDeletionScope::Lineage)
            .unwrap();
        let receipt = store
            .schedule_session_deletion(
                "deletion-undo",
                "undo-parent",
                SessionDeletionScope::Lineage,
                &preview.plan_hash,
                100,
                MIN_SESSION_DELETE_UNDO_MS,
            )
            .unwrap();
        assert_eq!(receipt.state, "pending");
        let write_error = store
            .update_session_title("undo-child", "Blocked", 101)
            .unwrap_err();
        assert_eq!(write_error.code, "session-deletion-pending");
        assert_eq!(
            store
                .read_durable_blob_for_session("undo-parent", &artifact.content_reference, 102,)
                .unwrap()
                .content,
            b"session deletion artifact"
        );
        let undone = store.undo_session_deletion("deletion-undo", 103).unwrap();
        assert_eq!(undone.state, "cancelled");
        assert!(store
            .session_deletion_for_session("undo-parent")
            .unwrap()
            .is_none());
        assert_eq!(
            store
                .update_session_title("undo-child", "Writable again", 104)
                .unwrap()
                .title,
            "Writable again"
        );
        let candidate = store
            .rebuild_session_projection_candidate("undo-parent")
            .unwrap();
        assert!(candidate.source_complete);
        assert!(candidate.matches_current_projection);
    }

    #[test]
    fn session_only_purge_keeps_lineage_tombstone_and_delays_blob_gc() {
        let root = Root::new("session-deletion-purge");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_rebuildable_session(&mut store, "purge-parent");
        create_child_session(&mut store, "purge-child", "purge-parent", 40);
        let artifact = store
            .put_durable_blob(session_artifact_request("purge-parent", 50))
            .unwrap();
        let preview = store
            .preview_session_deletion("purge-parent", SessionDeletionScope::SessionOnly)
            .unwrap();
        let undo_until = 100 + MIN_SESSION_DELETE_UNDO_MS;
        store
            .schedule_session_deletion(
                "deletion-purge",
                "purge-parent",
                SessionDeletionScope::SessionOnly,
                &preview.plan_hash,
                100,
                MIN_SESSION_DELETE_UNDO_MS,
            )
            .unwrap();
        assert_eq!(
            store
                .sweep_session_deletions(undo_until - 1)
                .unwrap()
                .examined,
            0
        );
        let sweep = store.sweep_session_deletions(undo_until).unwrap();
        assert_eq!(sweep.purged, 1);
        assert_eq!(sweep.released_artifact_references, 1);
        let tombstone = store.load_session("purge-parent").unwrap();
        assert_eq!(tombstone.title, "Deleted session");
        assert_eq!(tombstone.status, "archived");
        assert!(tombstone.environment_identity.is_none());
        let child = store.load_session("purge-child").unwrap();
        assert_eq!(child.parent_session_id.as_deref(), Some("purge-parent"));
        assert_ne!(child.title, "Deleted session");
        assert!(store
            .load_all_session_items("purge-parent")
            .unwrap()
            .is_empty());
        assert!(store
            .read_session_events("purge-parent", 0, 10)
            .unwrap()
            .is_empty());
        let released =
            load_durable_blob_reference(&store.connection, &artifact.reference_id).unwrap();
        assert_eq!(released.state, "released");
        assert!(released.retain_until_ms >= undo_until + MIN_BLOB_RETENTION_MS);
        let undo_error = store
            .undo_session_deletion("deletion-purge", undo_until)
            .unwrap_err();
        assert_eq!(undo_error.code, "session-deletion-not-undoable");
        assert_eq!(
            store
                .garbage_collect_durable_blobs(released.retain_until_ms)
                .unwrap()
                .deleted,
            0
        );
        assert_eq!(
            store
                .garbage_collect_durable_blobs(released.retain_until_ms + 1)
                .unwrap()
                .deleted,
            1
        );
        drop(store);
        let reopened = WorkbenchStore::open(&root.path).unwrap();
        assert_eq!(
            reopened
                .session_deletion_for_session("purge-parent")
                .unwrap()
                .unwrap()
                .state,
            "purged"
        );
        assert_eq!(
            reopened
                .load_session("purge-child")
                .unwrap()
                .parent_session_id
                .as_deref(),
            Some("purge-parent")
        );
    }

    #[test]
    fn session_deletion_storage_failure_rolls_back_plan_content_and_audit() {
        let root = Root::new("session-deletion-storage-failure");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_rebuildable_session(&mut store, "delete-failure-parent");
        let preview = store
            .preview_session_deletion("delete-failure-parent", SessionDeletionScope::SessionOnly)
            .unwrap();
        let undo_until = 100 + MIN_SESSION_DELETE_UNDO_MS;
        store
            .schedule_session_deletion(
                "deletion-storage-failure",
                "delete-failure-parent",
                SessionDeletionScope::SessionOnly,
                &preview.plan_hash,
                100,
                MIN_SESSION_DELETE_UNDO_MS,
            )
            .unwrap();
        store
            .connection
            .execute_batch(
                "CREATE TRIGGER fail_session_deletion_item_purge
                 BEFORE DELETE ON items
                 BEGIN
                    SELECT RAISE(ABORT, 'injected session deletion storage failure');
                 END;",
            )
            .unwrap();
        assert!(store.sweep_session_deletions(undo_until).is_err());
        assert_eq!(
            store
                .session_deletion_for_session("delete-failure-parent")
                .unwrap()
                .unwrap()
                .state,
            "pending"
        );
        assert_eq!(
            store
                .load_all_session_items("delete-failure-parent")
                .unwrap()
                .len(),
            1
        );
        let purge_audits: i64 = store
            .connection
            .query_row(
                "SELECT COUNT(*) FROM events
                 WHERE event_kind = 'retention.session-deletion-purged'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(purge_audits, 0);
        store
            .connection
            .execute_batch("DROP TRIGGER fail_session_deletion_item_purge;")
            .unwrap();
        assert_eq!(store.sweep_session_deletions(undo_until).unwrap().purged, 1);
    }

    #[test]
    fn retention_policy_respects_protection_and_session_override_before_deletion() {
        const DAY_MS: u64 = 24 * 60 * 60 * 1_000;
        let root = Root::new("retention-policy-sweep");
        let project_root = root.parent.join("retention-project");
        fs::create_dir(&project_root).unwrap();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_project(StoredProjectCreate {
                project_id: "retention-project".into(),
                root_id: "root-primary".into(),
                canonical_root: project_root
                    .canonicalize()
                    .unwrap()
                    .to_string_lossy()
                    .into_owned(),
                root_identity: "path:sha256:retention-project".into(),
                display_name: "Retention project".into(),
                root_access: "write".into(),
                created_at_ms: 1,
            })
            .unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "retention-session".into(),
                project_id: Some("retention-project".into()),
                mode: StoredSessionMode::Work,
                title: "Retention session".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 10,
            })
            .unwrap();
        store
            .set_retention_policy(RetentionPolicy {
                scope_kind: "project".into(),
                scope_id: "retention-project".into(),
                archive_after_ms: Some(DAY_MS),
                delete_after_ms: Some(DAY_MS),
                undo_window_ms: MIN_SESSION_DELETE_UNDO_MS,
                delete_scope: SessionDeletionScope::SessionOnly,
                updated_at_ms: 20,
            })
            .unwrap();
        store
            .set_retention_policy(RetentionPolicy {
                scope_kind: "session".into(),
                scope_id: "retention-session".into(),
                archive_after_ms: Some(2 * DAY_MS),
                delete_after_ms: Some(DAY_MS),
                undo_window_ms: MIN_SESSION_DELETE_UNDO_MS,
                delete_scope: SessionDeletionScope::SessionOnly,
                updated_at_ms: 30,
            })
            .unwrap();
        let first_due = 10 + DAY_MS;
        let mut protected = BTreeSet::new();
        protected.insert("retention-session".into());
        let protected_report = store
            .apply_retention_policies(first_due, &protected)
            .unwrap();
        assert_eq!(protected_report.protected_sessions, 1);
        assert_eq!(
            store.load_session("retention-session").unwrap().status,
            "active"
        );
        let override_not_due = store
            .apply_retention_policies(first_due, &BTreeSet::new())
            .unwrap();
        assert_eq!(override_not_due.archived_sessions, 0);
        let archive_at = 10 + 2 * DAY_MS;
        let archived = store
            .apply_retention_policies(archive_at, &BTreeSet::new())
            .unwrap();
        assert_eq!(archived.archived_sessions, 1);
        assert_eq!(archived.scheduled_deletions, 0);
        assert_eq!(
            store.load_session("retention-session").unwrap().status,
            "archived"
        );
        let delete_at = archive_at + DAY_MS;
        let scheduled = store
            .apply_retention_policies(delete_at, &BTreeSet::new())
            .unwrap();
        assert_eq!(scheduled.archived_sessions, 0);
        assert_eq!(scheduled.scheduled_deletions, 1);
        let deletion = store
            .session_deletion_for_session("retention-session")
            .unwrap()
            .unwrap();
        assert_eq!(deletion.state, "pending");
        assert_eq!(deletion.requested_at_ms, delete_at);
        assert_eq!(
            store
                .undo_session_deletion(&deletion.deletion_id, delete_at)
                .unwrap()
                .state,
            "cancelled"
        );
    }

    #[test]
    fn session_projection_storage_failure_preserves_complete_previous_projection() {
        let root = Root::new("session-projection-storage-failure");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_rebuildable_session(&mut store, "session-storage-rebuild");
        store
            .connection
            .execute(
                "UPDATE sessions SET title = 'Drifted title'
                 WHERE session_id = 'session-storage-rebuild'",
                [],
            )
            .unwrap();
        let candidate = store
            .rebuild_session_projection_candidate("session-storage-rebuild")
            .unwrap();
        assert!(projection_candidate_is_rebuildable(&candidate));
        store
            .connection
            .execute_batch(
                "CREATE TRIGGER fail_session_projection_item_insert
                 BEFORE INSERT ON items
                 BEGIN
                    SELECT RAISE(ABORT, 'injected session projection storage failure');
                 END;",
            )
            .unwrap();
        assert!(store
            .apply_session_projection_candidate(&candidate)
            .is_err());
        assert_eq!(
            store.load_session("session-storage-rebuild").unwrap().title,
            "Drifted title"
        );
        assert_eq!(
            store
                .load_session_turns("session-storage-rebuild")
                .unwrap()
                .len(),
            1
        );
        assert_eq!(
            store
                .load_all_session_items("session-storage-rebuild")
                .unwrap()
                .len(),
            1
        );
        let audit_count: i64 = store
            .connection
            .query_row(
                "SELECT COUNT(*) FROM events
                 WHERE event_kind = 'session.projection-rebuilt'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(audit_count, 0);
        store
            .connection
            .execute_batch("DROP TRIGGER fail_session_projection_item_insert;")
            .unwrap();
        let (report, rebuilt) = store
            .verify_or_rebuild_session_projection("session-storage-rebuild")
            .unwrap();
        assert!(rebuilt);
        assert!(report.consistent);
        assert_eq!(
            store.load_session("session-storage-rebuild").unwrap().title,
            "Authoritative title"
        );
    }

    #[test]
    fn session_projection_rebuild_process_exit_child() {
        let Ok(data_root) = std::env::var("AEGISY_TEST_SESSION_REBUILD_DATA_ROOT") else {
            return;
        };
        let mut store = WorkbenchStore::open(Path::new(&data_root)).unwrap();
        store
            .connection
            .execute(
                "UPDATE sessions SET title = 'Drifted title'
                 WHERE session_id = 'session-crash-rebuild'",
                [],
            )
            .unwrap();
        let candidate = store
            .rebuild_session_projection_candidate("session-crash-rebuild")
            .unwrap();
        assert!(projection_candidate_is_rebuildable(&candidate));
        store
            .apply_session_projection_candidate(&candidate)
            .unwrap();
        panic!("session projection rebuild crash point did not terminate the child");
    }

    #[test]
    fn session_projection_process_exit_rolls_back_and_next_startup_recovers() {
        let root = Root::new("session-projection-process-exit");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_rebuildable_session(&mut store, "session-crash-rebuild");
        drop(store);

        let status = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "workbench_store::tests::session_projection_rebuild_process_exit_child",
                "--nocapture",
            ])
            .env("AEGISY_TEST_SESSION_REBUILD_DATA_ROOT", &root.path)
            .env(
                "AEGISY_TEST_PROJECTION_REBUILD_CRASH_POINT",
                "session-after-projection-clear",
            )
            .status()
            .unwrap();
        assert_eq!(status.code(), Some(86));

        let connection = Connection::open(root.path.join(DATABASE_FILE)).unwrap();
        let preserved: (String, i64, i64) = connection
            .query_row(
                "SELECT session.title,
                        (SELECT COUNT(*) FROM turns
                         WHERE session_id = session.session_id),
                        (SELECT COUNT(*) FROM items
                         WHERE session_id = session.session_id)
                 FROM sessions AS session
                 WHERE session.session_id = 'session-crash-rebuild'",
                [],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )
            .unwrap();
        assert_eq!(preserved, ("Drifted title".into(), 1, 1));
        let audit_count: i64 = connection
            .query_row(
                "SELECT COUNT(*) FROM events
                 WHERE event_kind = 'session.projection-rebuilt'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(audit_count, 0);
        drop(connection);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        let report = reopened.startup_projection_recovery();
        assert_eq!(report.rebuilt_sessions, 1);
        assert_eq!(report.quarantined_sessions, 0);
        assert_eq!(
            reopened
                .load_session("session-crash-rebuild")
                .unwrap()
                .title,
            "Authoritative title"
        );
        assert_eq!(
            reopened
                .load_all_session_items("session-crash-rebuild")
                .unwrap()
                .len(),
            1
        );
    }

    #[test]
    fn startup_quarantine_blocks_session_writes_and_destructive_blob_gc() {
        let root = Root::new("startup-session-quarantine");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "session-startup-quarantine".into(),
                project_id: None,
                mode: StoredSessionMode::Chat,
                title: "Protected".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 1,
            })
            .unwrap();
        store
            .connection
            .execute(
                "UPDATE events SET payload_json = '{\"tampered\":true}'
                 WHERE session_id = 'session-startup-quarantine'",
                [],
            )
            .unwrap();
        drop(store);

        let mut reopened = WorkbenchStore::open(&root.path).unwrap();
        assert!(reopened.session_requires_recovery("session-startup-quarantine"));
        assert_eq!(reopened.quarantined_session_count(), 1);
        assert_eq!(
            reopened.startup_projection_recovery().quarantined_sessions,
            1
        );
        let title_error = reopened
            .update_session_title("session-startup-quarantine", "Must not write", 2)
            .unwrap_err();
        assert_eq!(title_error.code, "session-read-only-recovery");
        let turn_error = reopened
            .create_turn(StoredTurnCreate {
                turn_id: "turn-quarantined".into(),
                session_id: "session-startup-quarantine".into(),
                idempotency_key: None,
                input_hash: ContentHash::for_bytes(b"blocked"),
                created_at_ms: 2,
            })
            .unwrap_err();
        assert_eq!(turn_error.code, "session-read-only-recovery");
        let gc_error = reopened
            .garbage_collect_durable_blobs(u64::MAX / 2)
            .unwrap_err();
        assert_eq!(gc_error.code, "session-recovery-blocks-blob-gc");
        assert_eq!(
            reopened
                .load_session("session-startup-quarantine")
                .unwrap()
                .title,
            "Protected"
        );
    }

    #[test]
    fn startup_recovery_limit_enters_content_free_store_recovery() {
        let root = Root::new("startup-recovery-limit");
        drop(WorkbenchStore::open(&root.path).unwrap());
        let mut connection = Connection::open(root.path.join(DATABASE_FILE)).unwrap();
        let transaction = connection.transaction().unwrap();
        {
            let mut statement = transaction
                .prepare(
                    "INSERT INTO sessions (
                        session_id, project_id, mode, title, parent_session_id, lineage_kind,
                        status, environment_identity, created_at_ms, updated_at_ms
                     ) VALUES (?1, NULL, 'chat', 'Limit', NULL, 'new', 'active', NULL, 1, 1)",
                )
                .unwrap();
            for index in 0..=MAX_STARTUP_RECOVERY_SESSIONS {
                statement
                    .execute([format!("session-startup-limit-{index}")])
                    .unwrap();
            }
        }
        transaction.commit().unwrap();
        drop(connection);

        let diagnostic = match WorkbenchStore::open_or_recover(&root.path).unwrap() {
            WorkbenchStoreOpen::ReadOnlyRecovery(diagnostic) => diagnostic,
            WorkbenchStoreOpen::Writable(_) => panic!("oversized startup scan must be read-only"),
        };
        assert_eq!(diagnostic.reason_code, "startup-recovery-limit-exceeded");
        assert!(diagnostic.database_integrity_ok);
        assert_eq!(
            diagnostic.source_schema_version,
            Some(SCHEMA_VERSION as u64)
        );
    }

    #[test]
    fn item_payloads_reject_credentials_and_detect_tampering_or_sequence_gaps() {
        let root = Root::new("item-integrity");
        let project_root = root.parent.canonicalize().unwrap();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_project(StoredProjectCreate {
                project_id: "project-integrity".into(),
                root_id: "root-1".into(),
                canonical_root: project_root.to_string_lossy().into_owned(),
                root_identity: "root".into(),
                display_name: "Integrity".into(),
                root_access: "read".into(),
                created_at_ms: 1,
            })
            .unwrap();
        store
            .create_session(StoredSessionCreate {
                session_id: "session-integrity".into(),
                project_id: Some("project-integrity".into()),
                mode: StoredSessionMode::Chat,
                title: "Integrity".into(),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 2,
            })
            .unwrap();
        store
            .create_turn(StoredTurnCreate {
                turn_id: "turn-integrity".into(),
                session_id: "session-integrity".into(),
                idempotency_key: None,
                input_hash: ContentHash::for_bytes(b"input"),
                created_at_ms: 3,
            })
            .unwrap();
        assert!(store
            .append_item(StoredItemAppend {
                session_id: "session-integrity".into(),
                turn_id: Some("turn-integrity".into()),
                item_id: "secret-item".into(),
                item_kind: "message".into(),
                role: "user".into(),
                state: "completed".into(),
                payload: json!({"api_key": "must-not-persist"}),
                created_at_ms: 4,
            })
            .is_err());
        store
            .append_item(StoredItemAppend {
                session_id: "session-integrity".into(),
                turn_id: Some("turn-integrity".into()),
                item_id: "item-integrity".into(),
                item_kind: "message".into(),
                role: "user".into(),
                state: "completed".into(),
                payload: json!({"text": "safe"}),
                created_at_ms: 5,
            })
            .unwrap();
        store
            .append_item(StoredItemAppend {
                session_id: "session-integrity".into(),
                turn_id: Some("turn-integrity".into()),
                item_id: "item-integrity-2".into(),
                item_kind: "message".into(),
                role: "agent".into(),
                state: "completed".into(),
                payload: json!({"text": "second"}),
                created_at_ms: 6,
            })
            .unwrap();
        store
            .connection
            .execute(
                "UPDATE items SET payload_json = '{\"tampered\":true}' WHERE item_id = 'item-integrity'",
                [],
            )
            .unwrap();
        assert!(store
            .read_session_items("session-integrity", 0, 20)
            .is_err());
        store
            .connection
            .execute("DELETE FROM items WHERE item_id = 'item-integrity'", [])
            .unwrap();
        assert!(store
            .read_session_items("session-integrity", 0, 20)
            .is_err());
    }

    #[test]
    fn persistence_secret_gate_redacts_items_and_rolls_back_event_rejection() {
        let root = Root::new("persistence-secrets");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let rejected_project = store
            .create_project(StoredProjectCreate {
                project_id: "secret-display-project".into(),
                root_id: "secret-root".into(),
                canonical_root: root.path.to_string_lossy().into_owned(),
                root_identity: "root:sha256:secret-display".into(),
                display_name: "API_KEY=sk-12345678901234567890".into(),
                root_access: "read".into(),
                created_at_ms: 1,
            })
            .unwrap_err();
        assert!(rejected_project.message.contains("secret-like"));
        let project_count: i64 = store
            .connection
            .query_row(
                "SELECT COUNT(*) FROM projects WHERE project_id = 'secret-display-project'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(project_count, 0);
        create_blob_owners(&mut store, &root);
        let jwt = format!(
            "eyJ{}.{}.{}",
            "a".repeat(24),
            "b".repeat(24),
            "c".repeat(24)
        );

        let rejected = store
            .create_session(StoredSessionCreate {
                session_id: "secret-title-session".into(),
                project_id: Some("blob-project".into()),
                mode: StoredSessionMode::Work,
                title: format!("Authorization: Bearer {jwt}"),
                parent_session_id: None,
                lineage_kind: StoredSessionLineage::New,
                environment_identity: None,
                created_at_ms: 2_000,
            })
            .unwrap_err();
        assert!(rejected.message.contains("secret-like"));
        let session_count: i64 = store
            .connection
            .query_row(
                "SELECT COUNT(*) FROM sessions WHERE session_id = 'secret-title-session'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(session_count, 0);

        let title_error = store
            .update_session_title("blob-session-a", &format!("JWT {jwt}"), 2_001)
            .unwrap_err();
        assert!(title_error.message.contains("secret-like"));
        assert_eq!(
            store.load_session("blob-session-a").unwrap().title,
            "Blob session 0"
        );

        let stored = store
            .append_item(StoredItemAppend {
                session_id: "blob-session-a".into(),
                turn_id: None,
                item_id: "redacted-jwt-item".into(),
                item_kind: "message".into(),
                role: "user".into(),
                state: "completed".into(),
                payload: json!({"nested": {"text": format!("Authorization: Bearer {jwt}")}}),
                created_at_ms: 2_002,
            })
            .unwrap();
        let item_json = serde_json::to_string(&stored.payload).unwrap();
        assert!(!item_json.contains(&jwt));
        let persisted_item: String = store
            .connection
            .query_row(
                "SELECT payload_json FROM items WHERE item_id = 'redacted-jwt-item'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert!(!persisted_item.contains(&jwt));
        let persisted_events: String = store
            .connection
            .query_row(
                "SELECT COALESCE(GROUP_CONCAT(payload_json, '\\n'), '')
                 FROM events WHERE session_id = 'blob-session-a'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert!(!persisted_events.contains(&jwt));

        let transaction = store.connection.transaction().unwrap();
        transaction
            .execute(
                "UPDATE sessions SET title = 'transient secret gate value'
                 WHERE session_id = 'blob-session-a'",
                [],
            )
            .unwrap();
        let event_error = append_event_tx(
            &transaction,
            EventInput {
                session_id: "blob-session-a",
                event_id: "secret-event",
                timestamp_ms: 2_003,
                correlation_id: "blob-session-a",
                event_kind: "test.secret",
                project_id: Some("blob-project"),
                operation_id: "blob-session-a",
                generation: 0,
                payload: json!({"nested": {"jwt": jwt}}),
            },
        )
        .unwrap_err();
        assert!(
            event_error.message.contains("credential-like")
                || event_error.message.contains("secret-like")
        );
        drop(transaction);
        assert_eq!(
            store.load_session("blob-session-a").unwrap().title,
            "Blob session 0"
        );

        let mut blob = blob_request("blob-session-a", "secret-metadata", b"safe content", 2_004);
        blob.metadata = json!({"note": format!("Authorization: Bearer {jwt}")});
        assert!(store.put_durable_blob(blob).is_err());
        let references: i64 = store
            .connection
            .query_row(
                "SELECT COUNT(*) FROM durable_blob_references
                 WHERE owner_id = 'secret-metadata'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(references, 0);
    }

    #[test]
    fn legacy_event_secret_is_rejected_on_replay_and_quarantines_session() {
        let root = Root::new("legacy-event-secret");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_blob_owners(&mut store, &root);
        let secret = format!(
            "Authorization: Bearer eyJ{}.{}.{}",
            "a".repeat(24),
            "b".repeat(24),
            "c".repeat(24)
        );
        let payload_json = serde_json::to_string(&json!({"legacy": secret})).unwrap();
        let payload_hash = ContentHash::for_bytes(payload_json.as_bytes());
        store
            .connection
            .execute(
                "UPDATE events SET payload_json = ?1, payload_sha256 = ?2, payload_bytes = ?3
                 WHERE session_id = 'blob-session-a' AND event_kind = 'session.created'",
                params![
                    payload_json,
                    payload_hash.sha256,
                    to_i64(payload_hash.bytes, "legacy payload bytes").unwrap()
                ],
            )
            .unwrap();
        drop(store);

        let mut reopened = WorkbenchStore::open(&root.path).unwrap();
        assert!(reopened.session_requires_recovery("blob-session-a"));
        assert!(reopened
            .read_session_events("blob-session-a", 0, 20)
            .is_err());
        let write_error = reopened
            .update_session_title("blob-session-a", "must remain blocked", 3_000)
            .unwrap_err();
        assert_eq!(write_error.code, "session-read-only-recovery");
    }

    #[test]
    fn consumes_scoped_decisions_once_and_replays_events_after_reopen() {
        let root = Root::new("consume");
        let (record, plan) = record_and_plan();
        let requirement =
            authorization_requirement(&record, &plan, GitWorkflowAuthorizedAction::Start).unwrap();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let evidence = issue_pair(&mut store, &requirement, 1_000_000);
        verify_git_workflow_authorization(&mut store, requirement.clone(), &evidence, 1_000_000)
            .unwrap();
        let events = store.read_session_events("session-1", 0, 20).unwrap();
        assert_eq!(events.len(), 3);
        assert_eq!(events[0].sequence, 1);
        assert_eq!(events[0].event_kind, "approval.permission-issued");
        assert_eq!(events[1].event_kind, "approval.explicit-issued");
        assert_eq!(events[2].event_kind, "approval.consumed");
        assert!(verify_git_workflow_authorization(
            &mut store,
            requirement.clone(),
            &evidence,
            1_000_001,
        )
        .is_err());
        drop(store);
        let mut reopened = WorkbenchStore::open(&root.path).unwrap();
        let replay = reopened.read_session_events("session-1", 0, 20).unwrap();
        assert_eq!(replay, events);
        assert!(verify_git_workflow_authorization(
            &mut reopened,
            requirement,
            &evidence,
            1_000_002,
        )
        .is_err());
    }

    #[test]
    fn consumption_rolls_back_decisions_when_consumed_event_cannot_append() {
        let root = Root::new("rollback");
        let (record, plan) = record_and_plan();
        let requirement =
            authorization_requirement(&record, &plan, GitWorkflowAuthorizedAction::Start).unwrap();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let evidence = issue_pair(&mut store, &requirement, 2_000_000);
        let event_id = derived_event_id("approval-consumed", evidence.authorization_id.as_bytes());
        store
            .connection
            .execute(
                "INSERT INTO events (
                    session_id, sequence, event_id, timestamp_ms, correlation_id,
                    event_kind, project_id, operation_id, generation,
                    payload_json, payload_sha256, payload_bytes
                 ) VALUES ('session-1', 99, ?1, 1, 'x', 'approval.consumed',
                           'project-1', 'operation-1', 0, '{}', ?2, 2)",
                params![event_id, ContentHash::for_bytes(b"{}").sha256],
            )
            .unwrap();
        assert!(verify_git_workflow_authorization(
            &mut store,
            requirement.clone(),
            &evidence,
            2_000_000,
        )
        .is_err());
        let permission_status: String = store
            .connection
            .query_row(
                "SELECT status FROM approval_decisions WHERE authority_id = 'profile-authority'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(permission_status, "issued");
        store
            .connection
            .execute("DELETE FROM events WHERE event_id = ?1", [event_id])
            .unwrap();
        verify_git_workflow_authorization(&mut store, requirement, &evidence, 2_000_000).unwrap();
    }

    #[test]
    fn rejects_expired_duplicate_and_tampered_decisions_or_events() {
        let root = Root::new("reject");
        let (record, plan) = record_and_plan();
        let requirement =
            authorization_requirement(&record, &plan, GitWorkflowAuthorizedAction::Start).unwrap();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        assert!(store
            .issue_git_workflow_decision(
                &requirement,
                GitWorkflowDecisionKind::Permission,
                "profile-authority",
                "expired",
                GitWorkflowDecisionTiming {
                    issued_at_ms: 10,
                    expires_at_ms: 20,
                    observed_at_ms: 21,
                },
            )
            .is_err());
        let reference = store
            .issue_git_workflow_decision(
                &requirement,
                GitWorkflowDecisionKind::Permission,
                "profile-authority",
                "duplicate",
                GitWorkflowDecisionTiming {
                    issued_at_ms: 10,
                    expires_at_ms: 60_000,
                    observed_at_ms: 20,
                },
            )
            .unwrap();
        assert!(store
            .issue_git_workflow_decision(
                &requirement,
                GitWorkflowDecisionKind::Permission,
                "profile-authority",
                "duplicate",
                GitWorkflowDecisionTiming {
                    issued_at_ms: 10,
                    expires_at_ms: 60_000,
                    observed_at_ms: 20,
                },
            )
            .is_err());
        let events = store.read_session_events("session-1", 0, 20).unwrap();
        let tampered = events[0].event_id.clone();
        store
            .connection
            .execute(
                "UPDATE events SET payload_json = '{\"tampered\":true}' WHERE event_id = ?1",
                [tampered],
            )
            .unwrap();
        assert!(store.read_session_events("session-1", 0, 20).is_err());
        assert_eq!(reference.scope, "allow-once");
    }

    #[test]
    fn authority_rechecks_explicit_approval_and_evidence_binding() {
        let root = Root::new("authority-rechecks");
        let (record, plan) = record_and_plan();
        let requirement =
            authorization_requirement(&record, &plan, GitWorkflowAuthorizedAction::Start).unwrap();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let permission = store
            .issue_git_workflow_decision(
                &requirement,
                GitWorkflowDecisionKind::Permission,
                "profile-authority",
                "permission-only",
                GitWorkflowDecisionTiming {
                    issued_at_ms: 3_000_000,
                    expires_at_ms: 3_060_000,
                    observed_at_ms: 3_000_000,
                },
            )
            .unwrap();
        let missing_approval = GitWorkflowAuthorizationEvidence {
            schema_version: "git-workflow-authorization-evidence/0.1".into(),
            authorization_id: "authority-recheck-1".into(),
            requirement_hash: requirement.requirement_hash.clone(),
            permission: permission.clone(),
            explicit_approval: None,
        };
        assert!(store
            .consume_once(&requirement, &missing_approval, 3_000_000)
            .is_err());

        let approval = store
            .issue_git_workflow_decision(
                &requirement,
                GitWorkflowDecisionKind::ExplicitApproval,
                "profile-authority",
                "approval-same-authority",
                GitWorkflowDecisionTiming {
                    issued_at_ms: 3_000_000,
                    expires_at_ms: 3_060_000,
                    observed_at_ms: 3_000_000,
                },
            )
            .unwrap();
        let same_decision = GitWorkflowAuthorizationEvidence {
            schema_version: "git-workflow-authorization-evidence/0.1".into(),
            authorization_id: "authority-recheck-2".into(),
            requirement_hash: requirement.requirement_hash.clone(),
            permission: permission.clone(),
            explicit_approval: Some(GitWorkflowDecisionReference {
                decision_id: permission.decision_id.clone(),
                ..approval
            }),
        };
        assert!(store
            .consume_once(&requirement, &same_decision, 3_000_000)
            .is_err());

        let mut tampered = missing_approval;
        tampered.requirement_hash = ContentHash::for_bytes(b"different-requirement");
        assert!(store
            .consume_once(&requirement, &tampered, 3_000_000)
            .is_err());
    }

    #[test]
    fn event_append_requires_execution_attempt_and_preserves_sequence() {
        let root = Root::new("workflow-event");
        let (mut record, plan) = record_and_plan();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        assert!(store
            .append_git_workflow_event(&record, "git.workflow.prepared")
            .is_err());
        record.state = "execution-prepared".into();
        record.allowed_actions.clear();
        record.generation = 1;
        record.updated_at_ms = 20;
        record.execution = Some(GitWorkflowExecutionAttempt {
            schema_version: "git-workflow-execution-attempt/0.1".into(),
            authorization_id: "authorization-event".into(),
            requirement_hash: plan.expected_index_state.clone(),
            action: "start".into(),
            phase: "prepared".into(),
            source_generation: 0,
            started_at_ms: 10,
            updated_at_ms: 20,
            command_exit_code: None,
            outcome: None,
        });
        let event = store
            .append_git_workflow_event(&record, "git.workflow.prepared")
            .unwrap();
        assert_eq!(event.sequence, 1);
        assert_eq!(
            store.read_session_events("session-1", 0, 20).unwrap().len(),
            1
        );
    }

    #[test]
    fn operation_event_state_uses_validated_git_lifecycle_terminals() {
        let root = Root::new("operation-git-events");
        let (mut record, plan) = record_and_plan();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        record.state = "execution-prepared".into();
        record.allowed_actions.clear();
        record.generation = 1;
        record.updated_at_ms = 20;
        record.execution = Some(GitWorkflowExecutionAttempt {
            schema_version: "git-workflow-execution-attempt/0.1".into(),
            authorization_id: "authorization-git-events".into(),
            requirement_hash: plan.expected_index_state.clone(),
            action: "start".into(),
            phase: "prepared".into(),
            source_generation: 0,
            started_at_ms: 10,
            updated_at_ms: 20,
            command_exit_code: None,
            outcome: None,
        });
        store
            .append_git_workflow_event(&record, "git.workflow.prepared")
            .unwrap();
        assert_eq!(
            store
                .latest_operation_event_state("session-1", "operation-1")
                .unwrap(),
            Some(EventState::Running)
        );

        record.state = "completed".into();
        record.generation = 2;
        record.updated_at_ms = 30;
        record.execution = Some(GitWorkflowExecutionAttempt {
            schema_version: "git-workflow-execution-attempt/0.1".into(),
            authorization_id: "authorization-git-events".into(),
            requirement_hash: plan.expected_index_state,
            action: "start".into(),
            phase: "observed".into(),
            source_generation: 1,
            started_at_ms: 10,
            updated_at_ms: 30,
            command_exit_code: Some(0),
            outcome: Some("completed".into()),
        });
        store
            .append_git_workflow_event(&record, "git.workflow.completed")
            .unwrap();
        assert_eq!(
            store
                .latest_operation_event_state("session-1", "operation-1")
                .unwrap(),
            Some(EventState::Completed)
        );
        let malformed = serde_json::json!({
            "schema_version": "git.workflow.lifecycle/0.1"
        });
        let malformed_bytes = serde_json::to_vec(&malformed).unwrap();
        let malformed_hash = ContentHash::for_bytes(&malformed_bytes);
        store
            .connection
            .execute(
                "UPDATE events SET payload_json = ?1, payload_sha256 = ?2, payload_bytes = ?3
                 WHERE event_kind = 'git.workflow.completed'",
                rusqlite::params![
                    String::from_utf8(malformed_bytes).unwrap(),
                    malformed_hash.sha256,
                    i64::try_from(malformed_hash.bytes).unwrap(),
                ],
            )
            .unwrap();
        assert!(store
            .latest_operation_event_state("session-1", "operation-1")
            .is_err());
    }

    #[test]
    fn durable_blobs_deduplicate_reopen_and_isolate_session_reads() {
        let root = Root::new("durable-blob-reopen");
        let content = b"bounded redacted command output\n";
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_blob_owners(&mut store, &root);
        let first = store
            .put_durable_blob(blob_request(
                "blob-session-a",
                "blob-session-a",
                content,
                2_000,
            ))
            .unwrap();
        let second = store
            .put_durable_blob(blob_request(
                "blob-session-b",
                "blob-session-b",
                content,
                2_001,
            ))
            .unwrap();
        assert_eq!(first.content_hash, second.content_hash);
        assert_ne!(first.reference_id, second.reference_id);
        assert_eq!(
            store
                .read_durable_blob_for_session("blob-session-a", &first.content_reference, 3_000,)
                .unwrap()
                .content,
            content
        );
        assert!(store
            .read_durable_blob_for_session("unknown-session", &first.content_reference, 3_000,)
            .is_err());
        let consistency = store.scan_durable_blobs().unwrap();
        assert!(consistency.consistent, "{:?}", consistency.issues);
        assert_eq!(consistency.checked_objects, 1);
        assert_eq!(consistency.checked_references, 2);
        assert_eq!(consistency.disk_objects, 1);
        drop(store);

        let mut reopened = WorkbenchStore::open(&root.path).unwrap();
        let read = reopened
            .read_durable_blob_for_session("blob-session-b", &second.content_reference, 4_000)
            .unwrap();
        assert_eq!(read.reference.reference_id, second.reference_id);
        assert_eq!(read.content, content);

        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let object = root
                .path
                .join("durable-blobs-v1")
                .join(DurableBlobFileStore::storage_key(&first.content_hash.sha256).unwrap());
            assert_eq!(
                fs::metadata(object).unwrap().permissions().mode() & 0o777,
                0o600
            );
            assert_eq!(
                fs::metadata(root.path.join("durable-blobs-v1"))
                    .unwrap()
                    .permissions()
                    .mode()
                    & 0o777,
                0o700
            );
        }
    }

    #[test]
    fn durable_blob_admission_and_reference_failure_leave_no_orphan() {
        let root = Root::new("durable-blob-admission");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_blob_owners(&mut store, &root);
        let content = b"admission content";
        assert!(store
            .put_durable_blob_with_available_bytes(
                blob_request("blob-session-a", "blob-session-a", content, 2_000,),
                crate::durable_blob::MIN_FREE_BYTES - 1,
            )
            .is_err());
        assert_eq!(store.scan_durable_blobs().unwrap().disk_objects, 0);

        let mut forbidden = blob_request("blob-session-a", "blob-session-a", content, 2_000);
        forbidden.metadata = json!({"api_key": "must-not-persist"});
        assert!(store.put_durable_blob(forbidden).is_err());

        store
            .connection
            .execute_batch(
                "CREATE TRIGGER reject_blob_reference
                 BEFORE INSERT ON durable_blob_references
                 BEGIN SELECT RAISE(ABORT, 'injected reference failure'); END;",
            )
            .unwrap();
        assert!(store
            .put_durable_blob(blob_request(
                "blob-session-a",
                "blob-session-a",
                b"rollback content",
                2_100,
            ))
            .is_err());
        let consistency = store.scan_durable_blobs().unwrap();
        assert!(consistency.consistent, "{:?}", consistency.issues);
        assert_eq!(consistency.checked_objects, 0);
        assert_eq!(consistency.disk_objects, 0);
    }

    #[test]
    fn durable_blob_reference_and_item_event_commit_or_rollback_together() {
        let root = Root::new("durable-blob-item-atomicity");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_blob_owners(&mut store, &root);

        let item_id = "command-item-1";
        let mut blob = blob_request("blob-session-a", item_id, b"command artifact", 2_000);
        blob.owner_kind = "item".into();
        blob.owner_id = item_id.into();
        blob.reference_id = durable_blob_reference_id(
            blob.session_id.as_deref(),
            blob.project_id.as_deref(),
            &blob.owner_kind,
            &blob.owner_id,
            &blob.content_reference,
        );
        let item = StoredItemAppend {
            session_id: "blob-session-a".into(),
            turn_id: None,
            item_id: item_id.into(),
            item_kind: "command".into(),
            role: "tool".into(),
            state: "completed".into(),
            payload: json!({
                "artifact": {
                    "reference": blob.content_reference,
                    "bytes": blob.content.len()
                }
            }),
            created_at_ms: 2_000,
        };
        let (stored_item, stored_blob) = store
            .append_item_with_durable_blob(item, blob.clone())
            .unwrap();
        assert_eq!(stored_item.item_id, item_id);
        assert_eq!(stored_blob.owner_id, item_id);
        let projection = store.verify_session_projection("blob-session-a").unwrap();
        assert!(projection.consistent, "{:?}", projection.issues);
        assert_eq!(projection.checked_items, 1);
        assert_eq!(projection.checked_blob_references, 1);

        store
            .connection
            .execute_batch(
                "CREATE TRIGGER reject_atomic_item_event
                 BEFORE INSERT ON events
                 WHEN NEW.event_kind = 'item.appended'
                 BEGIN SELECT RAISE(ABORT, 'injected item event failure'); END;",
            )
            .unwrap();
        let failed_item_id = "command-item-2";
        let mut failed_blob =
            blob_request("blob-session-a", failed_item_id, b"must roll back", 2_100);
        failed_blob.owner_kind = "item".into();
        failed_blob.owner_id = failed_item_id.into();
        failed_blob.reference_id = durable_blob_reference_id(
            failed_blob.session_id.as_deref(),
            failed_blob.project_id.as_deref(),
            &failed_blob.owner_kind,
            &failed_blob.owner_id,
            &failed_blob.content_reference,
        );
        let failed_hash = sha256_hex(&failed_blob.content);
        assert!(store
            .append_item_with_durable_blob(
                StoredItemAppend {
                    session_id: "blob-session-a".into(),
                    turn_id: None,
                    item_id: failed_item_id.into(),
                    item_kind: "command".into(),
                    role: "tool".into(),
                    state: "completed".into(),
                    payload: json!({"artifact": {"reference": failed_blob.content_reference}}),
                    created_at_ms: 2_100,
                },
                failed_blob,
            )
            .is_err());
        let item_retained: bool = store
            .connection
            .query_row(
                "SELECT EXISTS(SELECT 1 FROM items WHERE item_id = ?1)",
                [failed_item_id],
                |row| row.get(0),
            )
            .unwrap();
        assert!(!item_retained);
        let retained: bool = store
            .connection
            .query_row(
                "SELECT EXISTS(SELECT 1 FROM durable_blobs WHERE sha256 = ?1)",
                [failed_hash],
                |row| row.get(0),
            )
            .unwrap();
        assert!(!retained);
        let scan = store.scan_durable_blobs().unwrap();
        assert!(scan.consistent, "{:?}", scan.issues);
        assert_eq!(scan.checked_objects, 1);
        assert_eq!(scan.checked_references, 1);
    }

    #[test]
    fn durable_blob_batch_supports_patch_image_diagnostic_and_generic_artifact_media() {
        let root = Root::new("durable-blob-media-kinds");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_blob_owners(&mut store, &root);
        let fixtures = [
            (
                DurableBlobKind::Patch,
                "patch",
                "text/x-diff; charset=utf-8",
                b"--- a/file\n+++ b/file\n".as_slice(),
            ),
            (
                DurableBlobKind::Image,
                "image",
                "image/png",
                b"\x89PNG\r\n\x1a\nfixture".as_slice(),
            ),
            (
                DurableBlobKind::Diagnostic,
                "diagnostic",
                "application/json",
                br#"{"severity":"error"}"#.as_slice(),
            ),
            (
                DurableBlobKind::Artifact,
                "artifact",
                "application/octet-stream",
                b"generic artifact".as_slice(),
            ),
            (
                DurableBlobKind::Artifact,
                "empty-artifact",
                "application/octet-stream",
                b"".as_slice(),
            ),
        ];
        let requests = fixtures
            .iter()
            .enumerate()
            .map(|(index, (kind, namespace, media_type, content))| {
                let sha256 = sha256_hex(content);
                let content_reference = format!("{namespace}:sha256:{sha256}");
                DurableBlobWrite {
                    reference_id: durable_blob_reference_id(
                        Some("blob-session-a"),
                        Some("blob-project"),
                        "session",
                        "blob-session-a",
                        &content_reference,
                    ),
                    content_reference,
                    session_id: Some("blob-session-a".into()),
                    project_id: Some("blob-project".into()),
                    kind: *kind,
                    media_type: (*media_type).into(),
                    owner_kind: "session".into(),
                    owner_id: "blob-session-a".into(),
                    metadata: json!({"fixture_index": index}),
                    content: content.to_vec(),
                    created_at_ms: 2_000 + index as u64,
                    retain_until_ms: 2_000 + index as u64 + 7 * MIN_BLOB_RETENTION_MS,
                }
            })
            .collect::<Vec<_>>();
        let stored = store.put_durable_blobs(requests).unwrap();
        assert_eq!(stored.len(), fixtures.len());
        for (reference, (_, _, _, expected)) in stored.iter().zip(fixtures) {
            let read = store
                .read_durable_blob_for_session(
                    "blob-session-a",
                    &reference.content_reference,
                    3_000,
                )
                .unwrap();
            assert_eq!(read.content, expected);
        }
        let scan = store.scan_durable_blobs().unwrap();
        assert!(scan.consistent, "{:?}", scan.issues);
        assert_eq!(scan.checked_objects, 5);
        assert_eq!(scan.checked_references, 5);
    }

    #[test]
    fn durable_blob_integrity_failures_are_detected_without_content_evidence() {
        let root = Root::new("durable-blob-corruption");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_blob_owners(&mut store, &root);
        let stored = store
            .put_durable_blob(blob_request(
                "blob-session-a",
                "blob-session-a",
                b"original-content",
                2_000,
            ))
            .unwrap();
        let object = root
            .path
            .join("durable-blobs-v1")
            .join(DurableBlobFileStore::storage_key(&stored.content_hash.sha256).unwrap());
        fs::write(&object, b"modified-content").unwrap();
        let scan = store.scan_durable_blobs().unwrap();
        assert!(!scan.consistent);
        assert_eq!(scan.missing_or_corrupt_objects, 1);
        assert_eq!(scan.issues, vec!["blob-missing-or-corrupt-object"]);
        assert!(store
            .read_durable_blob_for_session("blob-session-a", &stored.content_reference, 3_000,)
            .is_err());
        let projection = store.verify_session_projection("blob-session-a").unwrap();
        assert!(!projection.consistent);
        assert_eq!(projection.checked_blob_references, 1);
        assert!(projection
            .issues
            .contains(&"blob-reference-content-invalid".into()));
        assert!(projection
            .issues
            .iter()
            .all(|issue| !issue.contains("original") && !issue.contains("modified")));

        fs::remove_file(object).unwrap();
        let missing = store.scan_durable_blobs().unwrap();
        assert_eq!(missing.missing_or_corrupt_objects, 1);
    }

    #[test]
    fn durable_blob_release_preserves_undo_window_and_gc_never_deletes_active_data() {
        let root = Root::new("durable-blob-retention");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_blob_owners(&mut store, &root);
        let mut request = blob_request(
            "blob-session-a",
            "blob-session-a",
            b"retained-content",
            2_000,
        );
        request.retain_until_ms = request.created_at_ms + MIN_BLOB_RETENTION_MS;
        let stored = store.put_durable_blob(request).unwrap();
        let before_release = store
            .garbage_collect_durable_blobs(2_000 + 2 * MIN_BLOB_RETENTION_MS)
            .unwrap();
        assert_eq!(before_release.deleted, 0);
        assert!(store
            .read_durable_blob_for_session(
                "blob-session-a",
                &stored.content_reference,
                2_000 + 2 * MIN_BLOB_RETENTION_MS,
            )
            .is_ok());

        let released_at = 2_000 + 3 * MIN_BLOB_RETENTION_MS;
        let released = store
            .release_durable_blob_reference(&stored.reference_id, released_at)
            .unwrap();
        assert_eq!(released.state, "released");
        assert!(released.retain_until_ms >= released_at + MIN_BLOB_RETENTION_MS);
        assert!(store
            .read_durable_blob_for_session(
                "blob-session-a",
                &stored.content_reference,
                released.retain_until_ms,
            )
            .is_ok());
        assert_eq!(
            store
                .garbage_collect_durable_blobs(released.retain_until_ms)
                .unwrap()
                .deleted,
            0
        );
        assert_eq!(
            store
                .garbage_collect_durable_blobs(released.retain_until_ms + 1)
                .unwrap()
                .deleted,
            1
        );
        assert!(store
            .read_durable_blob_for_session(
                "blob-session-a",
                &stored.content_reference,
                released.retain_until_ms + 2,
            )
            .is_err());
        assert!(store.scan_durable_blobs().unwrap().consistent);
    }

    #[test]
    fn pinned_context_event_and_image_release_commit_or_roll_back_together() {
        let root = Root::new("pinned-image-release-transaction");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_blob_owners(&mut store, &root);
        let content = b"bounded-image-fixture".to_vec();
        let sha256 = sha256_hex(&content);
        let content_reference = format!("image:sha256:{sha256}");
        let stored = store
            .put_durable_blob(DurableBlobWrite {
                reference_id: durable_blob_reference_id(
                    Some("blob-session-a"),
                    Some("blob-project"),
                    "session",
                    "blob-session-a",
                    &content_reference,
                ),
                content_reference,
                session_id: Some("blob-session-a".into()),
                project_id: Some("blob-project".into()),
                kind: DurableBlobKind::Image,
                media_type: "image/png".into(),
                owner_kind: "session".into(),
                owner_id: "blob-session-a".into(),
                metadata: json!({"width": 1, "height": 1}),
                content,
                created_at_ms: 2_000,
                retain_until_ms: 2_000 + MIN_BLOB_RETENTION_MS,
            })
            .unwrap();
        store
            .connection
            .execute_batch(
                "CREATE TRIGGER fail_pinned_context_release_event
                 BEFORE INSERT ON events
                 WHEN NEW.event_kind = 'project.pinned-context-updated'
                 BEGIN
                     SELECT RAISE(ABORT, 'injected pinned context event failure');
                 END;",
            )
            .unwrap();
        let set_identity = format!("pinned-context:sha256:{}", "a".repeat(64));
        let object_reference = format!("pinned-context-object:sha256:{}", "b".repeat(64));
        assert!(store
            .append_pinned_context_event_with_blob_releases(
                "blob-project",
                &set_identity,
                &object_reference,
                0,
                std::slice::from_ref(&stored.reference_id),
                3_000,
            )
            .is_err());
        assert_eq!(
            store
                .inspect_durable_blob_reference(
                    "blob-project",
                    Some("blob-session-a"),
                    &stored.content_reference,
                )
                .unwrap()
                .state,
            "active"
        );
        store
            .connection
            .execute_batch("DROP TRIGGER fail_pinned_context_release_event;")
            .unwrap();
        let event = store
            .append_pinned_context_event_with_blob_releases(
                "blob-project",
                &set_identity,
                &object_reference,
                0,
                std::slice::from_ref(&stored.reference_id),
                3_000,
            )
            .unwrap();
        assert_eq!(event.event_kind, "project.pinned-context-updated");
        let candidate = store
            .rebuild_project_projection_candidate("blob-project")
            .unwrap();
        assert!(candidate.source_complete, "{:?}", candidate.issues);
        let released =
            load_durable_blob_reference(&store.connection, &stored.reference_id).unwrap();
        assert_eq!(released.state, "released");
        assert!(released.retain_until_ms >= 3_000 + MIN_BLOB_RETENTION_MS);
    }

    #[test]
    fn durable_blob_scan_reports_but_does_not_delete_unregistered_files() {
        let root = Root::new("durable-blob-orphan");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        let content = b"unknown crash residue";
        let sha256 = sha256_hex(content);
        let parent = root
            .path
            .join("durable-blobs-v1")
            .join("objects")
            .join(&sha256[..2]);
        fs::create_dir(&parent).unwrap();
        let orphan = parent.join(&sha256);
        fs::write(&orphan, content).unwrap();
        let scan = store.scan_durable_blobs().unwrap();
        assert!(!scan.consistent);
        assert_eq!(scan.unregistered_files, 1);
        assert_eq!(scan.issues, vec!["blob-unregistered-file"]);
        assert_eq!(
            store
                .garbage_collect_durable_blobs(u64::MAX / 2)
                .unwrap()
                .deleted,
            0
        );
        assert!(orphan.exists());
    }

    #[test]
    fn migration_failure_preserves_v4_and_enters_content_free_read_only_recovery() {
        let root = Root::new("migration-read-only-recovery");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_blob_owners(&mut store, &root);
        store
            .connection
            .execute_batch(
                "DROP TABLE durable_blob_references;
                 DROP TABLE durable_blobs;
                 CREATE VIEW durable_blobs AS
                    SELECT 'not-a-real-blob-table' AS sha256;
                 PRAGMA user_version = 4;",
            )
            .unwrap();
        drop(store);

        let failure = WorkbenchStore::open(&root.path).unwrap_err();
        assert_eq!(failure.code, "workbench-migration-failed");
        let original = Connection::open_with_flags(
            root.path.join(DATABASE_FILE),
            OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX,
        )
        .unwrap();
        let version: i64 = original
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .unwrap();
        assert_eq!(version, 4);
        let object_type: String = original
            .query_row(
                "SELECT type FROM sqlite_master WHERE name = 'durable_blobs'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(object_type, "view");
        assert!(original
            .query_row(
                "SELECT 1 FROM sessions WHERE session_id = 'blob-session-a'",
                [],
                |row| row.get::<_, i64>(0),
            )
            .is_ok());
        drop(original);
        let backups = migration_backup_manifests(&root.path).unwrap();
        assert_eq!(backups.len(), 1);
        assert_eq!(backups[0].source_schema_version, 4);
        let backup_connection = Connection::open_with_flags(
            root.path
                .join("migration-backups-v1")
                .join(&backups[0].backup_file),
            OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX,
        )
        .unwrap();
        let backed_up_session: String = backup_connection
            .query_row(
                "SELECT session_id FROM sessions WHERE session_id = 'blob-session-a'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(backed_up_session, "blob-session-a");

        let diagnostic = match WorkbenchStore::open_or_recover(&root.path).unwrap() {
            WorkbenchStoreOpen::ReadOnlyRecovery(diagnostic) => diagnostic,
            WorkbenchStoreOpen::Writable(_) => panic!("broken migration must not become writable"),
        };
        assert_eq!(diagnostic.mode, "read-only-recovery");
        assert_eq!(diagnostic.reason_code, "workbench-migration-failed");
        assert_eq!(diagnostic.source_schema_version, Some(4));
        assert!(diagnostic.database_readable);
        assert!(diagnostic.database_integrity_ok);
        assert!(diagnostic.application_id_valid);
        assert!(diagnostic.backup_available);
        assert_eq!(diagnostic.valid_backup_count, 1);

        let mut runtime = crate::Runtime::with_codex_and_store(&root.path).unwrap();
        let initialized = runtime.handle_line(
            &json!({
                "jsonrpc": "2.0",
                "id": "recovery-initialize",
                "method": "initialize",
                "params": {
                    "protocol_version": "0.1",
                    "client": {"name": "recovery-test", "version": "1"}
                }
            })
            .to_string(),
        );
        assert_eq!(
            initialized[0]["result"]["backend"]["status"],
            "read-only-recovery"
        );
        let capabilities = initialized[0]["result"]["capabilities"].as_array().unwrap();
        assert!(capabilities
            .iter()
            .any(|value| value == "runtime.recovery.diagnostic-export"));
        assert!(!capabilities.iter().any(|value| value == "project.open"));
        assert!(runtime
            .handle_line(r#"{"jsonrpc":"2.0","method":"initialized"}"#)
            .is_empty());
        let denied = runtime.handle_line(
            r#"{"jsonrpc":"2.0","id":"project","method":"project/open","params":{"root":"/tmp"}}"#,
        );
        assert_eq!(denied[0]["error"]["code"], -32120);
        let exported = runtime.handle_line(
            r#"{"jsonrpc":"2.0","id":"export","method":"runtime/recovery/export","params":{}}"#,
        );
        assert_eq!(
            exported[0]["result"]["export"]["contains_database_content"],
            false
        );
        let serialized = serde_json::to_string(&exported).unwrap();
        assert!(!serialized.contains(&root.parent.to_string_lossy().into_owned()));
        assert!(!serialized.contains("Blob project"));
    }

    #[test]
    fn corrupt_database_starts_diagnostic_recovery_without_rewriting_original_bytes() {
        let root = Root::new("corrupt-database-recovery");
        let database = root.path.join(DATABASE_FILE);
        let corrupt = b"not a sqlite database; preserve this exact evidence";
        fs::write(&database, corrupt).unwrap();

        let diagnostic = match WorkbenchStore::open_or_recover(&root.path).unwrap() {
            WorkbenchStoreOpen::ReadOnlyRecovery(diagnostic) => diagnostic,
            WorkbenchStoreOpen::Writable(_) => panic!("corrupt database must not open writable"),
        };
        assert!(diagnostic.database_present);
        assert!(!diagnostic.database_readable);
        assert!(!diagnostic.database_integrity_ok);
        assert!(!diagnostic.backup_available);
        assert!(diagnostic
            .issues
            .contains(&"recovery-database-unreadable".into()));
        assert_eq!(fs::read(database).unwrap(), corrupt);
    }

    #[test]
    fn newer_schema_is_never_downgraded_and_exposes_read_only_recovery() {
        let root = Root::new("newer-schema-recovery");
        let store = WorkbenchStore::open(&root.path).unwrap();
        let newer = SCHEMA_VERSION + 1;
        store
            .connection
            .pragma_update(None, "user_version", newer)
            .unwrap();
        drop(store);

        let failure = WorkbenchStore::open(&root.path).unwrap_err();
        assert_eq!(failure.code, "workbench-schema-newer");
        let diagnostic = match WorkbenchStore::open_or_recover(&root.path).unwrap() {
            WorkbenchStoreOpen::ReadOnlyRecovery(diagnostic) => diagnostic,
            WorkbenchStoreOpen::Writable(_) => panic!("newer schema must not be downgraded"),
        };
        assert_eq!(diagnostic.reason_code, "workbench-schema-newer");
        assert_eq!(diagnostic.source_schema_version, Some(newer as u64));
        assert!(diagnostic.database_integrity_ok);
        assert!(!diagnostic.backup_available);
        let original = Connection::open_with_flags(
            root.path.join(DATABASE_FILE),
            OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX,
        )
        .unwrap();
        let retained: i64 = original
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .unwrap();
        assert_eq!(retained, newer);
    }

    #[test]
    fn interrupted_v4_transaction_and_partial_backup_reenter_migration_safely() {
        let root = Root::new("interrupted-migration-reentry");
        let store = WorkbenchStore::open(&root.path).unwrap();
        store
            .connection
            .execute_batch(
                "DROP TABLE durable_blob_references;
                 DROP TABLE durable_blobs;
                 PRAGMA user_version = 4;
                 BEGIN IMMEDIATE;
                 CREATE TABLE interrupted_uncommitted(value TEXT) STRICT;
                 PRAGMA user_version = 5;",
            )
            .unwrap();
        drop(store);
        let backup_root = root.path.join("migration-backups-v1");
        fs::create_dir(&backup_root).unwrap();
        let partial = backup_root.join(".aegisy-migration-sqlite3-crash.tmp");
        fs::write(&partial, b"partial backup must not be trusted or deleted").unwrap();

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        let version: i64 = reopened
            .connection
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .unwrap();
        assert_eq!(version, SCHEMA_VERSION);
        let uncommitted: Option<String> = reopened
            .connection
            .query_row(
                "SELECT name FROM sqlite_master
                 WHERE type = 'table' AND name = 'interrupted_uncommitted'",
                [],
                |row| row.get(0),
            )
            .optional()
            .unwrap();
        assert!(uncommitted.is_none());
        assert!(partial.exists());
        let backups = migration_backup_manifests(&root.path).unwrap();
        assert_eq!(backups.len(), 1);
        assert_eq!(backups[0].source_schema_version, 4);
        let interrupted_diagnostic = inspect_recovery(
            &root.path,
            &root.path.join(DATABASE_FILE),
            "interrupted-migration-audit",
            SCHEMA_VERSION as u64,
            APPLICATION_ID as u64,
        );
        assert_eq!(interrupted_diagnostic.valid_backup_count, 1);
        assert_eq!(interrupted_diagnostic.invalid_backup_count, 1);
        assert!(interrupted_diagnostic
            .issues
            .contains(&"recovery-backup-invalid".into()));
    }

    #[test]
    fn migration_backup_low_space_and_tampering_are_detected_conservatively() {
        let root = Root::new("migration-backup-admission");
        let store = WorkbenchStore::open(&root.path).unwrap();
        store
            .connection
            .execute_batch(
                "DROP TABLE durable_blob_references;
                 DROP TABLE durable_blobs;
                 PRAGMA user_version = 4;",
            )
            .unwrap();
        let application_id: i64 = store
            .connection
            .pragma_query_value(None, "application_id", |row| row.get(0))
            .unwrap();
        let low_space = create_pre_upgrade_backup_with_available_bytes(
            &store.connection,
            &root.path,
            4,
            SCHEMA_VERSION as u64,
            application_id as u64,
            crate::durable_blob::MIN_FREE_BYTES - 1,
        )
        .unwrap_err();
        assert_eq!(low_space.code, "migration-backup-low-space");
        drop(store);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        drop(reopened);
        let backups = migration_backup_manifests(&root.path).unwrap();
        assert_eq!(backups.len(), 1);
        let backup = root
            .path
            .join("migration-backups-v1")
            .join(&backups[0].backup_file);
        fs::write(&backup, b"tampered backup").unwrap();
        let diagnostic = inspect_recovery(
            &root.path,
            &root.path.join(DATABASE_FILE),
            "migration-backup-audit",
            SCHEMA_VERSION as u64,
            APPLICATION_ID as u64,
        );
        assert_eq!(diagnostic.valid_backup_count, 0);
        assert!(diagnostic.invalid_backup_count >= 1);
        assert!(diagnostic
            .issues
            .contains(&"recovery-backup-invalid".into()));
        assert_eq!(fs::read(backup).unwrap(), b"tampered backup");
    }

    #[test]
    fn upgrades_schema_v4_to_durable_blob_schema_without_losing_sessions() {
        let root = Root::new("schema-v4-blob-migration");
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        create_blob_owners(&mut store, &root);
        store
            .connection
            .execute_batch(
                "DROP TABLE durable_blob_references;
                 DROP TABLE durable_blobs;
                 PRAGMA user_version = 4;",
            )
            .unwrap();
        drop(store);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        let version: i64 = reopened
            .connection
            .pragma_query_value(None, "user_version", |row| row.get(0))
            .unwrap();
        assert_eq!(version, SCHEMA_VERSION);
        assert_eq!(
            reopened.load_session("blob-session-a").unwrap().project_id,
            Some("blob-project".into())
        );
        assert!(reopened.scan_durable_blobs().unwrap().consistent);
        let backups = migration_backup_manifests(&root.path).unwrap();
        assert_eq!(backups.len(), 1);
        assert_eq!(backups[0].source_schema_version, 4);
        assert_eq!(backups[0].target_schema_version, SCHEMA_VERSION as u64);
    }

    #[test]
    fn project_root_removal_is_durable_and_replayable() {
        let root = Root::new("project-root-removal");
        let primary = root.parent.join("primary");
        let additional = root.parent.join("additional");
        fs::create_dir_all(&primary).unwrap();
        fs::create_dir_all(&additional).unwrap();
        let primary = primary.canonicalize().unwrap();
        let additional = additional.canonicalize().unwrap();
        let mut store = WorkbenchStore::open(&root.path).unwrap();
        store
            .create_project(StoredProjectCreate {
                project_id: "root-remove-project".into(),
                root_id: "root-1".into(),
                canonical_root: primary.to_string_lossy().into_owned(),
                root_identity: "fs:test:primary".into(),
                display_name: "Root removal".into(),
                root_access: "write".into(),
                created_at_ms: 1,
            })
            .unwrap();
        store
            .add_project_root(StoredProjectRootAdd {
                project_id: "root-remove-project".into(),
                root_id: "root-2".into(),
                canonical_root: additional.to_string_lossy().into_owned(),
                root_identity: "fs:test:additional".into(),
                access: "read".into(),
                created_at_ms: 2,
            })
            .unwrap();
        store
            .remove_project_root("root-remove-project", "root-2", 3)
            .unwrap();
        assert_eq!(
            store
                .load_project_roots("root-remove-project")
                .unwrap()
                .len(),
            1
        );
        assert!(store
            .read_session_events(&project_event_stream_id("root-remove-project"), 0, 20)
            .unwrap()
            .iter()
            .any(|event| event.event_kind == "project.root-removed"));
        drop(store);

        let reopened = WorkbenchStore::open(&root.path).unwrap();
        let roots = reopened.load_project_roots("root-remove-project").unwrap();
        assert_eq!(roots.len(), 1);
        assert_eq!(roots[0].root_id, "root-1");
        let candidate = reopened
            .rebuild_project_projection_candidate("root-remove-project")
            .unwrap();
        assert!(candidate.source_complete);
        assert!(candidate.matches_current_projection);
        assert!(candidate.issues.is_empty());
    }
}
