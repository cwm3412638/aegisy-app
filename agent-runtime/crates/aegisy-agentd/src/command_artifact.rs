use crate::command_output::CommandOutputCapture;
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::collections::{HashMap, VecDeque};
use std::time::{SystemTime, UNIX_EPOCH};

const MAX_SESSION_ARTIFACTS: usize = 64;
const MAX_SESSION_ARTIFACT_BYTES: usize = 16 * 1024 * 1024;
const MAX_SAFE_JSON_INTEGER: u64 = 9_007_199_254_740_991;
const MAX_ITEM_ID_BYTES: usize = 128;
const CONTENT_TYPE: &str = "text/plain; charset=utf-8";

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

impl CommandOutputArtifact {
    pub fn validate(&self) -> Result<(), &'static str> {
        let hash_valid = self.sha256.len() == 64
            && self
                .sha256
                .bytes()
                .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte));
        let retained_bytes = u64::try_from(self.retained_bytes)
            .map_err(|_| "command output artifact retained byte count is invalid")?;
        if !hash_valid
            || self.reference != format!("command-output:sha256:{}", self.sha256)
            || self.content_type != CONTENT_TYPE
            || self.item_id.is_empty()
            || self.item_id.len() > MAX_ITEM_ID_BYTES
            || !self.item_id.bytes().all(|byte| byte.is_ascii_graphic())
            || self.created_at_ms > MAX_SAFE_JSON_INTEGER
            || self.source_bytes > MAX_SAFE_JSON_INTEGER
            || self.redacted_count > MAX_SAFE_JSON_INTEGER
            || self.total_bytes > MAX_SAFE_JSON_INTEGER
            || retained_bytes > MAX_SAFE_JSON_INTEGER
            || retained_bytes
                > (crate::command_output::ARTIFACT_HEAD_LIMIT
                    + crate::command_output::ARTIFACT_TAIL_LIMIT) as u64
            || self.omitted_bytes > MAX_SAFE_JSON_INTEGER
            || self.redacted != (self.redacted_count > 0)
            || sha256_hex(self.content.as_bytes()) != self.sha256
        {
            return Err("command output artifact identity or metadata is invalid");
        }
        if self.total_bytes != retained_bytes.saturating_add(self.omitted_bytes)
            || self.truncated != (self.omitted_bytes > 0)
        {
            return Err("command output artifact truncation metadata is invalid");
        }
        if self.truncated {
            let marker = format!(
                "\n[Aegisy omitted {} command output bytes]\n",
                self.omitted_bytes
            );
            let marker_positions = self
                .content
                .match_indices(&marker)
                .map(|(index, _)| index)
                .collect::<Vec<_>>();
            let Some(&head_bytes) = marker_positions.first() else {
                return Err("command output artifact omission marker is invalid");
            };
            let tail_bytes = self
                .content
                .len()
                .saturating_sub(head_bytes.saturating_add(marker.len()));
            if self.content.len() != self.retained_bytes.saturating_add(marker.len())
                || marker_positions.len() != 1
                || !(crate::command_output::ARTIFACT_HEAD_LIMIT.saturating_sub(3)
                    ..=crate::command_output::ARTIFACT_HEAD_LIMIT)
                    .contains(&head_bytes)
                || !(crate::command_output::ARTIFACT_TAIL_LIMIT.saturating_sub(3)
                    ..=crate::command_output::ARTIFACT_TAIL_LIMIT)
                    .contains(&tail_bytes)
                || self.retained_bytes != head_bytes.saturating_add(tail_bytes)
            {
                return Err("command output artifact omission marker is invalid");
            }
        } else if self.content.len() != self.retained_bytes {
            return Err("command output artifact retained byte count is invalid");
        }
        Ok(())
    }
}

#[derive(Default)]
pub struct CommandArtifactStore {
    sessions: HashMap<String, SessionArtifacts>,
}

#[derive(Default)]
struct SessionArtifacts {
    artifacts: HashMap<(String, String), CommandOutputArtifact>,
    order: VecDeque<(String, String)>,
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
            content_type: CONTENT_TYPE.into(),
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
        debug_assert!(artifact.validate().is_ok());
        let session = self.sessions.entry(session_id.into()).or_default();
        let key = (item_id.to_owned(), reference);
        if let Some(existing) = session.artifacts.get(&key) {
            return Some(existing.clone());
        }
        session.bytes = session.bytes.saturating_add(artifact.content.len());
        session.order.push_back(key.clone());
        session.artifacts.insert(key, artifact.clone());
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
            .values()
            .filter(|artifact| artifact.reference == reference)
            .max_by(|left, right| {
                left.created_at_ms
                    .cmp(&right.created_at_ms)
                    .then_with(|| left.item_id.cmp(&right.item_id))
            })
            .cloned()
    }

    pub fn read_for_item(
        &self,
        session_id: &str,
        item_id: &str,
        reference: &str,
    ) -> Option<CommandOutputArtifact> {
        self.sessions
            .get(session_id)?
            .artifacts
            .get(&(item_id.to_owned(), reference.to_owned()))
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
        assert!(artifact.validate().is_ok());
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

        let second_item = store
            .record("session-1", "command-2", &output, 3 * 1024 * 1024, 0)
            .unwrap();
        assert_eq!(second_item.reference, artifact.reference);
        assert_eq!(second_item.item_id, "command-2");
        assert_eq!(
            store
                .read_for_item("session-1", "command-1", &artifact.reference)
                .unwrap()
                .item_id,
            "command-1"
        );
        assert_eq!(
            store
                .read("session-1", &artifact.reference)
                .unwrap()
                .item_id,
            "command-2"
        );

        let mut bad_owner = artifact.clone();
        bad_owner.item_id = "item with spaces".into();
        assert!(bad_owner.validate().is_err());
        let mut bad_arithmetic = artifact.clone();
        bad_arithmetic.omitted_bytes += 1;
        assert!(bad_arithmetic.validate().is_err());
        let mut bad_redaction = artifact.clone();
        bad_redaction.redacted = true;
        assert!(bad_redaction.validate().is_err());

        let mut moved_marker = artifact;
        let marker = format!(
            "\n[Aegisy omitted {} command output bytes]\n",
            moved_marker.omitted_bytes
        );
        moved_marker.content = format!(
            "{}{}",
            marker,
            moved_marker.content.replacen(&marker, "", 1)
        );
        moved_marker.sha256 = sha256_hex(moved_marker.content.as_bytes());
        moved_marker.reference = format!("command-output:sha256:{}", moved_marker.sha256);
        assert!(moved_marker.validate().is_err());
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
    fn retained_source_marker_cannot_collide_with_the_runtime_omission_marker() {
        let omitted_bytes = 1024 * 1024;
        let marker = format!("\n[Aegisy omitted {omitted_bytes} command output bytes]\n");
        let head = format!(
            "{marker}{}",
            "h".repeat(crate::command_output::ARTIFACT_HEAD_LIMIT - marker.len())
        );
        let tail = format!(
            "{marker}{}",
            "t".repeat(crate::command_output::ARTIFACT_TAIL_LIMIT - marker.len())
        );
        let mut output = CommandOutputCapture::default();
        output.append(&format!("{head}{}{}", "m".repeat(omitted_bytes), tail));

        let artifact = CommandArtifactStore::default()
            .record(
                "session-marker",
                "command.marker:1",
                &output,
                (crate::command_output::ARTIFACT_HEAD_LIMIT
                    + omitted_bytes
                    + crate::command_output::ARTIFACT_TAIL_LIMIT) as u64,
                0,
            )
            .unwrap();
        assert!(artifact.validate().is_ok());
        assert_eq!(artifact.content.match_indices(&marker).count(), 1);
        assert_eq!(
            artifact
                .content
                .match_indices(&marker.replacen('[', "{", 1))
                .count(),
            2
        );
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
