use serde::{Deserialize, Serialize};
use serde_json::to_vec;
use sha2::{Digest, Sha256};

pub const SCHEMA_VERSION: &str = "operation-reconciliation/0.1";
const MAX_IDENTIFIER_BYTES: usize = 128;
const MAX_BLOCKERS: usize = 16;
const MAX_DOMAINS: usize = 8;

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum OperationKind {
    Turn,
    WorkspaceEdit,
    Terminal,
    Git,
    BackgroundJob,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum EventState {
    None,
    Running,
    Completed,
    Failed,
    Interrupted,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum ProcessState {
    NotObserved,
    Running,
    NotRunning,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "state", rename_all = "kebab-case")]
pub enum WorkspaceState {
    NotRequired,
    NotObserved,
    Unchanged { snapshot_hash: String },
    Changed { snapshot_hash: String },
    Unavailable,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "state", rename_all = "kebab-case")]
pub enum GitState {
    NotRequired,
    NotObserved,
    Unchanged { snapshot_hash: String },
    Changed { snapshot_hash: String },
    OperationInProgress,
    Unavailable,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ReconciliationEvidence {
    pub event: EventState,
    pub process: ProcessState,
    pub workspace: WorkspaceState,
    pub git: GitState,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ReconciliationInput {
    pub operation_id: String,
    pub session_id: String,
    pub kind: OperationKind,
    pub evidence: ReconciliationEvidence,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ReconciliationResult {
    pub schema_version: String,
    pub operation_id: String,
    pub session_id: String,
    pub kind: OperationKind,
    pub state: String,
    pub decision: String,
    pub writes_blocked: bool,
    pub blockers: Vec<String>,
    pub observed_domains: Vec<String>,
    pub review_id: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReconciliationError {
    pub message: String,
}

pub fn reconcile(input: &ReconciliationInput) -> Result<ReconciliationResult, ReconciliationError> {
    validate_identifier(&input.operation_id, "operation ID")?;
    validate_identifier(&input.session_id, "session ID")?;
    validate_evidence(input)?;

    let mut blockers = Vec::new();
    let mut observed_domains = Vec::new();
    observe_process(input.evidence.process, &mut observed_domains);
    observe_workspace(
        &input.evidence.workspace,
        &mut observed_domains,
        &mut blockers,
    );
    observe_git(&input.evidence.git, &mut observed_domains, &mut blockers);
    if observed_domains.len() > MAX_DOMAINS {
        return Err(error("reconciliation observed-domain limit exceeded"));
    }

    let required_missing = required_evidence_missing(input.kind, &input.evidence);
    blockers.extend(required_missing);
    let (state, mut decision) = match input.evidence.event {
        EventState::Completed => ("completed", "terminal-event-authoritative"),
        EventState::Failed => ("failed", "terminal-event-authoritative"),
        EventState::Interrupted => ("interrupted", "terminal-event-authoritative"),
        EventState::Running if input.evidence.process == ProcessState::Running => {
            ("running", "await-terminal-event")
        }
        EventState::Running | EventState::None => {
            blockers.push("no-authoritative-terminal-event".into());
            ("unknown", "explicit-review-required")
        }
    };

    if input.evidence.event == EventState::Running
        && input.evidence.process == ProcessState::NotRunning
    {
        blockers.push("process-ended-before-terminal-event".into());
    }
    if !blockers.is_empty() && state != "running" {
        decision = "explicit-review-required";
    }
    blockers.sort();
    blockers.dedup();
    if blockers.len() > MAX_BLOCKERS {
        blockers.truncate(MAX_BLOCKERS);
        blockers.push("blocker-list-truncated".into());
    }
    let writes_blocked = state == "running" || state == "unknown" || !blockers.is_empty();
    let review_id = review_id(
        input,
        state,
        decision,
        writes_blocked,
        &blockers,
        &observed_domains,
    )?;
    Ok(ReconciliationResult {
        schema_version: SCHEMA_VERSION.into(),
        operation_id: input.operation_id.clone(),
        session_id: input.session_id.clone(),
        kind: input.kind,
        state: state.into(),
        decision: decision.into(),
        writes_blocked,
        blockers,
        observed_domains,
        review_id,
    })
}

fn validate_evidence(input: &ReconciliationInput) -> Result<(), ReconciliationError> {
    match &input.evidence.workspace {
        WorkspaceState::Unchanged { snapshot_hash } | WorkspaceState::Changed { snapshot_hash } => {
            validate_hash(snapshot_hash, "workspace snapshot hash")?
        }
        WorkspaceState::NotRequired | WorkspaceState::NotObserved | WorkspaceState::Unavailable => {
        }
    }
    match &input.evidence.git {
        GitState::Unchanged { snapshot_hash } | GitState::Changed { snapshot_hash } => {
            validate_hash(snapshot_hash, "Git snapshot hash")?
        }
        GitState::NotRequired
        | GitState::NotObserved
        | GitState::OperationInProgress
        | GitState::Unavailable => {}
    }
    Ok(())
}

fn required_evidence_missing(
    kind: OperationKind,
    evidence: &ReconciliationEvidence,
) -> Vec<String> {
    let mut missing = Vec::new();
    if matches!(kind, OperationKind::WorkspaceEdit)
        && matches!(
            evidence.workspace,
            WorkspaceState::NotRequired | WorkspaceState::NotObserved
        )
    {
        missing.push("workspace-evidence-missing".into());
    }
    if matches!(kind, OperationKind::Git)
        && matches!(evidence.git, GitState::NotRequired | GitState::NotObserved)
    {
        missing.push("git-evidence-missing".into());
    }
    if matches!(kind, OperationKind::Terminal | OperationKind::BackgroundJob)
        && matches!(evidence.process, ProcessState::NotObserved)
    {
        missing.push("process-evidence-missing".into());
    }
    missing
}

fn observe_process(state: ProcessState, domains: &mut Vec<String>) {
    if state != ProcessState::NotObserved {
        domains.push("process".into());
    }
}

fn observe_workspace(
    state: &WorkspaceState,
    domains: &mut Vec<String>,
    blockers: &mut Vec<String>,
) {
    match state {
        WorkspaceState::NotRequired => {}
        WorkspaceState::NotObserved => {}
        WorkspaceState::Unchanged { .. } => domains.push("workspace".into()),
        WorkspaceState::Changed { .. } => {
            domains.push("workspace".into());
            blockers.push("workspace-state-changed".into());
        }
        WorkspaceState::Unavailable => blockers.push("workspace-state-unavailable".into()),
    }
}

fn observe_git(state: &GitState, domains: &mut Vec<String>, blockers: &mut Vec<String>) {
    match state {
        GitState::NotRequired => {}
        GitState::NotObserved => {}
        GitState::Unchanged { .. } => domains.push("git".into()),
        GitState::Changed { .. } => {
            domains.push("git".into());
            blockers.push("git-state-changed".into());
        }
        GitState::OperationInProgress => {
            domains.push("git".into());
            blockers.push("git-operation-in-progress".into());
        }
        GitState::Unavailable => blockers.push("git-state-unavailable".into()),
    }
}

fn review_id(
    input: &ReconciliationInput,
    state: &str,
    decision: &str,
    writes_blocked: bool,
    blockers: &[String],
    observed_domains: &[String],
) -> Result<String, ReconciliationError> {
    let payload = serde_json::json!({
        "schema_version": SCHEMA_VERSION,
        "input": input,
        "state": state,
        "decision": decision,
        "writes_blocked": writes_blocked,
        "blockers": blockers,
        "observed_domains": observed_domains,
    });
    let bytes = to_vec(&payload).map_err(|_| error("cannot serialize reconciliation review"))?;
    let mut hasher = Sha256::new();
    hasher.update(bytes);
    Ok(format!(
        "reconciliation-review:sha256:{:x}",
        hasher.finalize()
    ))
}

fn validate_identifier(value: &str, label: &str) -> Result<(), ReconciliationError> {
    if value.is_empty() || value.len() > MAX_IDENTIFIER_BYTES || value.chars().any(char::is_control)
    {
        return Err(error(format!("{} is invalid", label)));
    }
    Ok(())
}

fn validate_hash(value: &str, label: &str) -> Result<(), ReconciliationError> {
    if value.len() != 64 || !value.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return Err(error(format!("{} is invalid", label)));
    }
    Ok(())
}

fn error(message: impl Into<String>) -> ReconciliationError {
    ReconciliationError {
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hash() -> String {
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa".into()
    }

    fn input(kind: OperationKind, event: EventState, process: ProcessState) -> ReconciliationInput {
        ReconciliationInput {
            operation_id: "operation-1".into(),
            session_id: "session-1".into(),
            kind,
            evidence: ReconciliationEvidence {
                event,
                process,
                workspace: WorkspaceState::NotRequired,
                git: GitState::NotRequired,
            },
        }
    }

    #[test]
    fn missing_terminal_event_is_unknown_and_blocks_writes() {
        let mut operation = input(
            OperationKind::WorkspaceEdit,
            EventState::None,
            ProcessState::NotRunning,
        );
        operation.evidence.workspace = WorkspaceState::Unchanged {
            snapshot_hash: hash(),
        };
        let result = reconcile(&operation).unwrap();
        assert_eq!(result.state, "unknown");
        assert!(result.writes_blocked);
        assert!(result
            .blockers
            .contains(&"no-authoritative-terminal-event".into()));
    }

    #[test]
    fn running_process_waits_for_terminal_event() {
        let result = reconcile(&input(
            OperationKind::Terminal,
            EventState::Running,
            ProcessState::Running,
        ));
        let result = result.unwrap();
        assert_eq!(result.state, "running");
        assert_eq!(result.decision, "await-terminal-event");
        assert!(result.writes_blocked);
    }

    #[test]
    fn completed_event_with_stable_evidence_is_authoritative() {
        let mut operation = input(
            OperationKind::Git,
            EventState::Completed,
            ProcessState::NotRunning,
        );
        operation.evidence.git = GitState::Unchanged {
            snapshot_hash: hash(),
        };
        let result = reconcile(&operation).unwrap();
        assert_eq!(result.state, "completed");
        assert_eq!(result.decision, "terminal-event-authoritative");
        assert!(!result.writes_blocked);
        assert!(result
            .review_id
            .starts_with("reconciliation-review:sha256:"));
    }

    #[test]
    fn terminal_event_with_changed_state_requires_review() {
        let mut operation = input(
            OperationKind::WorkspaceEdit,
            EventState::Completed,
            ProcessState::NotRunning,
        );
        operation.evidence.workspace = WorkspaceState::Changed {
            snapshot_hash: hash(),
        };
        let result = reconcile(&operation).unwrap();
        assert_eq!(result.decision, "explicit-review-required");
        assert!(result.writes_blocked);
        assert!(result.blockers.contains(&"workspace-state-changed".into()));
    }

    #[test]
    fn required_evidence_and_hashes_are_validated() {
        let operation = input(
            OperationKind::WorkspaceEdit,
            EventState::None,
            ProcessState::NotRunning,
        );
        let result = reconcile(&operation).unwrap();
        assert!(result
            .blockers
            .contains(&"workspace-evidence-missing".into()));

        let mut invalid = input(
            OperationKind::Git,
            EventState::Completed,
            ProcessState::NotRunning,
        );
        invalid.evidence.git = GitState::Unchanged {
            snapshot_hash: "not-a-hash".into(),
        };
        assert_eq!(
            reconcile(&invalid).unwrap_err().message,
            "Git snapshot hash is invalid"
        );
    }
}
