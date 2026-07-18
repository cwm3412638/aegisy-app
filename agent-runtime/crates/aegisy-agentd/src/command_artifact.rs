use crate::command_output::CommandOutputCapture;
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::collections::{HashMap, VecDeque};
use std::time::{SystemTime, UNIX_EPOCH};

const MAX_SESSION_ARTIFACTS: usize = 64;
const MAX_SESSION_ARTIFACT_BYTES: usize = 16 * 1024 * 1024;

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct CommandOutputArtifact {
    pub reference: String,
    pub sha256: String,
    pub content_type: String,
    pub item_id: String,
    pub created_at_ms: u64,
    pub source_bytes: u64,
    pub redacted_count: u64,
    pub redacted: bool,
    pub total_bytes: u64,
    pub retained_bytes: usize,
    pub omitted_bytes: u64,
    pub truncated: bool,
    pub content: String,
}

#[derive(Default)]
pub struct CommandArtifactStore {
    sessions: HashMap<String, SessionArtifacts>,
}

#[derive(Default)]
struct SessionArtifacts {
    artifacts: HashMap<String, CommandOutputArtifact>,
    order: VecDeque<String>,
    bytes: usize,
}

impl CommandArtifactStore {
    pub fn record(
        &mut self,
        session_id: &str,
        item_id: &str,
        output: &CommandOutputCapture,
        source_bytes: u64,
        redacted_count: u64,
    ) -> Option<CommandOutputArtifact> {
        self.record_internal(
            session_id,
            item_id,
            output,
            source_bytes,
            redacted_count,
            false,
        )
    }

    pub fn record_diagnostic_source(
        &mut self,
        session_id: &str,
        item_id: &str,
        output: &CommandOutputCapture,
        source_bytes: u64,
        redacted_count: u64,
    ) -> CommandOutputArtifact {
        self.record_internal(
            session_id,
            item_id,
            output,
            source_bytes,
            redacted_count,
            true,
        )
        .expect("diagnostic source artifacts are forced")
    }

    #[allow(clippy::too_many_arguments)]
    fn record_internal(
        &mut self,
        session_id: &str,
        item_id: &str,
        output: &CommandOutputCapture,
        source_bytes: u64,
        redacted_count: u64,
        force: bool,
    ) -> Option<CommandOutputArtifact> {
        let snapshot = output.snapshot();
        if !force && !snapshot.truncated {
            return None;
        }
        let capture = output.artifact();
        let sha256 = sha256_hex(capture.content.as_bytes());
        let reference = format!("command-output:sha256:{sha256}");
        let artifact = CommandOutputArtifact {
            reference: reference.clone(),
            sha256,
            content_type: "text/plain; charset=utf-8".into(),
            item_id: item_id.into(),
            created_at_ms: now_ms(),
            source_bytes,
            redacted_count,
            redacted: redacted_count > 0,
            total_bytes: capture.total_bytes,
            retained_bytes: capture.retained_bytes,
            omitted_bytes: capture.omitted_bytes,
            truncated: capture.truncated,
            content: capture.content,
        };
        let session = self.sessions.entry(session_id.into()).or_default();
        if let Some(existing) = session.artifacts.get(&reference) {
            return Some(existing.clone());
        }
        session.bytes = session.bytes.saturating_add(artifact.content.len());
        session.order.push_back(reference.clone());
        session.artifacts.insert(reference, artifact.clone());
        while session.artifacts.len() > MAX_SESSION_ARTIFACTS
            || session.bytes > MAX_SESSION_ARTIFACT_BYTES
        {
            let Some(oldest) = session.order.pop_front() else {
                break;
            };
            if let Some(removed) = session.artifacts.remove(&oldest) {
                session.bytes = session.bytes.saturating_sub(removed.content.len());
            }
        }
        Some(artifact)
    }

    pub fn read(&self, session_id: &str, reference: &str) -> Option<CommandOutputArtifact> {
        self.sessions
            .get(session_id)?
            .artifacts
            .get(reference)
            .cloned()
    }

    pub fn clear(&mut self) {
        self.sessions.clear();
    }

    pub fn remove_session(&mut self, session_id: &str) {
        self.sessions.remove(session_id);
    }
}

fn sha256_hex(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    digest.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stores_large_output_by_content_identity_and_isolates_sessions() {
        let mut output = CommandOutputCapture::default();
        output.append(&"x".repeat(3 * 1024 * 1024));
        let mut store = CommandArtifactStore::default();
        let artifact = store
            .record("session-1", "command-1", &output, 3 * 1024 * 1024, 0)
            .unwrap();
        assert!(artifact.reference.starts_with("command-output:sha256:"));
        assert_eq!(artifact.sha256.len(), 64);
        assert!(artifact.truncated);
        assert!(artifact.omitted_bytes > 0);
        assert!(artifact.content.len() <= 2 * 1024 * 1024 + 80);
        assert_eq!(
            store.read("session-1", &artifact.reference),
            Some(artifact.clone())
        );
        assert!(store.read("session-2", &artifact.reference).is_none());
        let duplicate = store
            .record("session-1", "command-1", &output, 3 * 1024 * 1024, 0)
            .unwrap();
        assert_eq!(duplicate.reference, artifact.reference);
    }

    #[test]
    fn small_inline_output_does_not_create_an_artifact() {
        let mut output = CommandOutputCapture::default();
        output.append("small\n");
        assert!(CommandArtifactStore::default()
            .record("session", "command", &output, 6, 0)
            .is_none());
    }

    #[test]
    fn diagnostic_source_forces_a_session_scoped_small_artifact() {
        let mut output = CommandOutputCapture::default();
        output.append("src/main.rs:2:1: error: missing\n");
        let mut store = CommandArtifactStore::default();
        let artifact = store.record_diagnostic_source("session", "command", &output, 36, 0);
        assert!(!artifact.truncated);
        assert_eq!(artifact.content, "src/main.rs:2:1: error: missing\n");
        assert_eq!(store.read("session", &artifact.reference), Some(artifact));
    }
}
