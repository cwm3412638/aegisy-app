use crate::output_redaction::redact_complete;
use serde::{Deserialize, Serialize};
use serde_json::json;
use sha2::{Digest, Sha256};

pub const SCHEMA_VERSION: &str = "session-compaction/0.1";
const MAX_IDENTIFIER_BYTES: usize = 256;
const MAX_PRESERVATION_BYTES: usize = 8 * 1024;
const MAX_SUMMARY_ITEMS: usize = 64;
const MAX_SUMMARY_ITEM_BYTES: usize = 1024;
const MAX_SUMMARY_BYTES: usize = 64 * 1024;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq, Default)]
pub struct CompactionSummary {
    pub decisions: Vec<String>,
    pub unresolved_tasks: Vec<String>,
    pub changed_files: Vec<String>,
    pub commands: Vec<String>,
    pub tests: Vec<String>,
    pub failures: Vec<String>,
    pub next_actions: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CompactionCheckpointReview {
    pub schema_version: String,
    pub checkpoint_id: String,
    pub session_id: String,
    pub through_sequence: u64,
    pub source_context_hash: String,
    pub preservation_instructions: Option<String>,
    pub summary: CompactionSummary,
    pub review_id: String,
    pub state: String,
    pub recovery_options: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CompactionActivation {
    pub schema_version: String,
    pub checkpoint_id: String,
    pub review_id: String,
    pub through_sequence: u64,
    pub source_context_hash: String,
    pub state: String,
    pub original_history_preserved: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CompactionFailure {
    pub schema_version: String,
    pub checkpoint_id: String,
    pub state: String,
    pub reason_class: String,
    pub original_context_preserved: bool,
    pub recovery_options: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CompactionError {
    pub message: String,
}

pub fn create_review(
    checkpoint_id: &str,
    session_id: &str,
    through_sequence: u64,
    source_context_hash: &str,
    preservation_instructions: Option<&str>,
    summary: CompactionSummary,
) -> Result<CompactionCheckpointReview, CompactionError> {
    validate_identifier(checkpoint_id, "compaction checkpoint ID")?;
    validate_identifier(session_id, "compaction session ID")?;
    validate_hash(source_context_hash, "source context hash")?;
    if through_sequence == 0 {
        return Err(error("compaction sequence must be positive"));
    }
    let preservation_instructions = preservation_instructions.map(validate_text).transpose()?;
    if let Some(instructions) = preservation_instructions.as_deref() {
        if instructions.len() > MAX_PRESERVATION_BYTES {
            return Err(error("compaction preservation instructions exceed limit"));
        }
    }
    validate_summary(&summary)?;

    let review_id = review_id_for(
        checkpoint_id,
        session_id,
        through_sequence,
        source_context_hash,
        preservation_instructions.as_deref(),
        &summary,
    )?;
    Ok(CompactionCheckpointReview {
        schema_version: SCHEMA_VERSION.into(),
        checkpoint_id: checkpoint_id.into(),
        session_id: session_id.into(),
        through_sequence,
        source_context_hash: source_context_hash.into(),
        preservation_instructions,
        summary,
        review_id,
        state: "review-required".into(),
        recovery_options: recovery_options(),
    })
}

pub fn activate_review(
    review: &CompactionCheckpointReview,
    current_sequence: u64,
    current_context_hash: &str,
) -> Result<CompactionActivation, CompactionError> {
    if review.schema_version != SCHEMA_VERSION || review.state != "review-required" {
        return Err(error("compaction review is not activatable"));
    }
    validate_identifier(&review.checkpoint_id, "compaction checkpoint ID")?;
    validate_identifier(&review.session_id, "compaction session ID")?;
    validate_hash(&review.source_context_hash, "source context hash")?;
    if review.through_sequence == 0 {
        return Err(error("compaction sequence must be positive"));
    }
    if let Some(instructions) = review.preservation_instructions.as_deref() {
        validate_text(instructions)?;
        if instructions.len() > MAX_PRESERVATION_BYTES {
            return Err(error("compaction preservation instructions exceed limit"));
        }
    }
    validate_summary(&review.summary)?;
    if review.recovery_options != recovery_options()
        || review.review_id
            != review_id_for(
                &review.checkpoint_id,
                &review.session_id,
                review.through_sequence,
                &review.source_context_hash,
                review.preservation_instructions.as_deref(),
                &review.summary,
            )?
    {
        return Err(error("compaction review identity is invalid"));
    }
    validate_hash(current_context_hash, "current context hash")?;
    if current_sequence != review.through_sequence {
        return Err(error("compaction review is stale"));
    }
    if current_context_hash != review.source_context_hash {
        return Err(error("compaction context identity changed"));
    }
    Ok(CompactionActivation {
        schema_version: SCHEMA_VERSION.into(),
        checkpoint_id: review.checkpoint_id.clone(),
        review_id: review.review_id.clone(),
        through_sequence: review.through_sequence,
        source_context_hash: review.source_context_hash.clone(),
        state: "activated-read-only-review".into(),
        original_history_preserved: true,
    })
}

pub fn failed_checkpoint(
    checkpoint_id: &str,
    reason_class: &str,
) -> Result<CompactionFailure, CompactionError> {
    validate_identifier(checkpoint_id, "compaction checkpoint ID")?;
    let reason_class = validate_class(reason_class)?;
    Ok(CompactionFailure {
        schema_version: SCHEMA_VERSION.into(),
        checkpoint_id: checkpoint_id.into(),
        state: "failed".into(),
        reason_class,
        original_context_preserved: true,
        recovery_options: recovery_options(),
    })
}

fn review_id_for(
    checkpoint_id: &str,
    session_id: &str,
    through_sequence: u64,
    source_context_hash: &str,
    preservation_instructions: Option<&str>,
    summary: &CompactionSummary,
) -> Result<String, CompactionError> {
    let review_input = json!({
        "schema_version": SCHEMA_VERSION,
        "checkpoint_id": checkpoint_id,
        "session_id": session_id,
        "through_sequence": through_sequence,
        "source_context_hash": source_context_hash,
        "preservation_instructions": preservation_instructions,
        "summary": summary,
    });
    let bytes = serde_json::to_vec(&review_input)
        .map_err(|_| error("cannot serialize compaction review"))?;
    Ok(format!("compaction-review:sha256:{}", sha256_hex(&bytes)))
}

fn validate_summary(summary: &CompactionSummary) -> Result<(), CompactionError> {
    let fields = [
        &summary.decisions,
        &summary.unresolved_tasks,
        &summary.changed_files,
        &summary.commands,
        &summary.tests,
        &summary.failures,
        &summary.next_actions,
    ];
    let mut total_bytes = 0usize;
    for field in fields {
        if field.len() > MAX_SUMMARY_ITEMS {
            return Err(error("compaction summary field exceeds item limit"));
        }
        for value in field {
            validate_text(value)?;
            if value.len() > MAX_SUMMARY_ITEM_BYTES {
                return Err(error("compaction summary item exceeds byte limit"));
            }
            total_bytes = total_bytes
                .checked_add(value.len())
                .ok_or_else(|| error("compaction summary size overflow"))?;
        }
    }
    if total_bytes > MAX_SUMMARY_BYTES {
        return Err(error("compaction summary exceeds byte limit"));
    }
    Ok(())
}

fn validate_text(value: &str) -> Result<String, CompactionError> {
    let redacted = redact_complete(value);
    if redacted != value {
        return Err(error("compaction content contains secret-shaped text"));
    }
    if value.chars().any(char::is_control) {
        return Err(error("compaction content contains control characters"));
    }
    Ok(value.to_owned())
}

fn validate_identifier(value: &str, label: &str) -> Result<(), CompactionError> {
    if value.is_empty() || value.len() > MAX_IDENTIFIER_BYTES || value.chars().any(char::is_control)
    {
        return Err(error(format!("{} is invalid", label)));
    }
    Ok(())
}

fn validate_hash(value: &str, label: &str) -> Result<(), CompactionError> {
    if value.len() != 64
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
        return Err(error(format!("{} is invalid", label)));
    }
    Ok(())
}

fn validate_class(value: &str) -> Result<String, CompactionError> {
    validate_identifier(value, "compaction failure class")?;
    if !matches!(
        value,
        "provider" | "transport" | "timeout" | "storage" | "adapter" | "budget"
    ) {
        return Err(error("compaction failure class is unsupported"));
    }
    Ok(value.to_owned())
}

fn recovery_options() -> Vec<String> {
    vec![
        "model-change".into(),
        "portable-fork".into(),
        "manual-cleanup".into(),
    ]
}

fn sha256_hex(bytes: &[u8]) -> String {
    let mut hasher = Sha256::new();
    hasher.update(bytes);
    format!("{:x}", hasher.finalize())
}

fn error(message: impl Into<String>) -> CompactionError {
    CompactionError {
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hash() -> &'static str {
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    }

    #[test]
    fn review_is_bounded_hashed_and_activation_is_identity_checked() {
        let summary = CompactionSummary {
            decisions: vec!["Keep read-only mode".into()],
            unresolved_tasks: vec!["Review Windows evidence".into()],
            changed_files: vec!["src/main.rs".into()],
            commands: vec!["cargo test".into()],
            tests: vec!["protocol".into()],
            failures: vec!["provider unavailable".into()],
            next_actions: vec!["fork if provider changes".into()],
        };
        let review = create_review(
            "compact-1",
            "session-1",
            7,
            hash(),
            Some("Preserve all decisions"),
            summary,
        )
        .unwrap();
        assert_eq!(review.schema_version, SCHEMA_VERSION);
        assert!(review.review_id.starts_with("compaction-review:sha256:"));
        assert_eq!(review.state, "review-required");
        assert!(activate_review(&review, 7, hash()).is_ok());
        assert_eq!(
            activate_review(&review, 8, hash()).unwrap_err().message,
            "compaction review is stale"
        );
        assert_eq!(
            activate_review(
                &review,
                7,
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
            )
            .unwrap_err()
            .message,
            "compaction context identity changed"
        );
    }

    #[test]
    fn activation_rejects_tampered_review_identity() {
        let mut review = create_review(
            "compact-1",
            "session-1",
            1,
            hash(),
            None,
            CompactionSummary::default(),
        )
        .unwrap();
        review
            .summary
            .next_actions
            .push("changed after review".into());
        assert_eq!(
            activate_review(&review, 1, hash()).unwrap_err().message,
            "compaction review identity is invalid"
        );
    }

    #[test]
    fn summary_rejects_secrets_controls_and_unbounded_content() {
        let secret = CompactionSummary {
            decisions: vec!["token=ghp_123456789012345678901234567890".into()],
            ..Default::default()
        };
        assert_eq!(
            create_review("compact-1", "session-1", 1, hash(), None, secret)
                .unwrap_err()
                .message,
            "compaction content contains secret-shaped text"
        );
        let oversized = CompactionSummary {
            next_actions: vec!["x".repeat(MAX_SUMMARY_ITEM_BYTES + 1)],
            ..Default::default()
        };
        assert!(create_review("compact-1", "session-1", 1, hash(), None, oversized).is_err());
    }

    #[test]
    fn failure_preserves_original_context_and_has_recovery_options() {
        let failure = failed_checkpoint("compact-1", "provider").unwrap();
        assert_eq!(failure.state, "failed");
        assert!(failure.original_context_preserved);
        assert_eq!(failure.recovery_options.len(), 3);
        assert!(failed_checkpoint("compact-1", "unknown").is_err());
    }
}
