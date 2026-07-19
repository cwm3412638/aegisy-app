use crate::output_redaction::redact_complete;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, HashSet};

pub const SCHEMA_VERSION: &str = "pinned-context/0.1";
pub const MAX_ITEMS: usize = 128;
pub const MAX_ITEM_BYTES: u64 = 16 * 1024 * 1024;
pub const MAX_TOTAL_BYTES: u64 = 64 * 1024 * 1024;
const MAX_ID_CHARS: usize = 128;
const MAX_SOURCE_CHARS: usize = 128;
const MAX_LABEL_CHARS: usize = 256;
const MAX_REFERENCE_CHARS: usize = 512;
const MAX_METADATA_ENTRIES: usize = 32;
const MAX_METADATA_KEY_CHARS: usize = 64;
const MAX_METADATA_VALUE_CHARS: usize = 512;
const MAX_METADATA_BYTES: usize = 4 * 1024;

const ALLOWED_KINDS: &[&str] = &[
    "file",
    "selection",
    "image",
    "diagnostic",
    "terminal_excerpt",
    "git_commit",
    "git_diff",
    "artifact",
    "child_handoff",
];

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PinnedContextError {
    pub code: &'static str,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PinnedContextItem {
    pub id: String,
    pub project_id: String,
    #[serde(default)]
    pub session_id: Option<String>,
    #[serde(default)]
    pub root_id: Option<String>,
    pub kind: String,
    pub source: String,
    pub label: String,
    pub reference: String,
    pub content_hash: String,
    pub bytes: u64,
    #[serde(default)]
    pub revision: Option<String>,
    pub freshness: String,
    pub priority: u16,
    #[serde(default)]
    pub metadata: BTreeMap<String, String>,
}

impl PinnedContextItem {
    pub fn validate(&self) -> Result<(), PinnedContextError> {
        bounded_identifier(&self.id, MAX_ID_CHARS, "pinned-context-id-invalid")?;
        bounded_identifier(
            &self.project_id,
            MAX_ID_CHARS,
            "pinned-context-project-invalid",
        )?;
        if let Some(session_id) = self.session_id.as_deref() {
            bounded_identifier(session_id, MAX_ID_CHARS, "pinned-context-session-invalid")?;
        }
        if let Some(root_id) = self.root_id.as_deref() {
            bounded_identifier(root_id, MAX_ID_CHARS, "pinned-context-root-invalid")?;
        }
        if !ALLOWED_KINDS.contains(&self.kind.as_str()) {
            return Err(error("pinned-context-kind-invalid"));
        }
        bounded_text(
            &self.source,
            MAX_SOURCE_CHARS,
            "pinned-context-source-invalid",
        )?;
        bounded_text(&self.label, MAX_LABEL_CHARS, "pinned-context-label-invalid")?;
        validate_reference(&self.reference, self.kind.as_str())?;
        validate_sha256(&self.content_hash)?;
        if self.bytes > MAX_ITEM_BYTES {
            return Err(error("pinned-context-item-too-large"));
        }
        if let Some(revision) = self.revision.as_deref() {
            bounded_text(
                revision,
                MAX_REFERENCE_CHARS,
                "pinned-context-revision-invalid",
            )?;
        }
        if !matches!(self.freshness.as_str(), "fresh" | "stale" | "unknown") {
            return Err(error("pinned-context-freshness-invalid"));
        }
        if self.priority > 1_000 {
            return Err(error("pinned-context-priority-invalid"));
        }
        validate_metadata(&self.metadata)?;
        Ok(())
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PinnedContextSet {
    pub schema_version: String,
    pub project_id: String,
    pub items: Vec<PinnedContextItem>,
}

impl PinnedContextSet {
    pub fn new(project_id: impl Into<String>) -> Result<Self, PinnedContextError> {
        let project_id = project_id.into();
        bounded_identifier(&project_id, MAX_ID_CHARS, "pinned-context-project-invalid")?;
        Ok(Self {
            schema_version: SCHEMA_VERSION.into(),
            project_id,
            items: Vec::new(),
        })
    }

    pub fn validate(&self) -> Result<(), PinnedContextError> {
        if self.schema_version != SCHEMA_VERSION {
            return Err(error("pinned-context-schema-unsupported"));
        }
        bounded_identifier(
            &self.project_id,
            MAX_ID_CHARS,
            "pinned-context-project-invalid",
        )?;
        if self.items.len() > MAX_ITEMS {
            return Err(error("pinned-context-item-limit"));
        }
        let mut ids = HashSet::new();
        let mut total_bytes = 0_u64;
        for item in &self.items {
            item.validate()?;
            if item.project_id != self.project_id {
                return Err(error("pinned-context-project-mismatch"));
            }
            if !ids.insert(item.id.as_str()) {
                return Err(error("pinned-context-duplicate-id"));
            }
            total_bytes = total_bytes.saturating_add(item.bytes);
            if total_bytes > MAX_TOTAL_BYTES {
                return Err(error("pinned-context-total-too-large"));
            }
        }
        Ok(())
    }

    pub fn add(&mut self, item: PinnedContextItem) -> Result<(), PinnedContextError> {
        item.validate()?;
        if item.project_id != self.project_id {
            return Err(error("pinned-context-project-mismatch"));
        }
        if self.items.iter().any(|candidate| candidate.id == item.id) {
            return Err(error("pinned-context-duplicate-id"));
        }
        if self.items.len() >= MAX_ITEMS {
            return Err(error("pinned-context-item-limit"));
        }
        let total_bytes = self
            .items
            .iter()
            .map(|candidate| candidate.bytes)
            .fold(0_u64, u64::saturating_add);
        if total_bytes.saturating_add(item.bytes) > MAX_TOTAL_BYTES {
            return Err(error("pinned-context-total-too-large"));
        }
        self.items.push(item);
        Ok(())
    }

    pub fn remove(&mut self, id: &str) -> bool {
        let before = self.items.len();
        self.items.retain(|item| item.id != id);
        self.items.len() != before
    }

    pub fn identity(&self) -> Result<String, PinnedContextError> {
        self.validate()?;
        let bytes = serde_json::to_vec(self).map_err(|_| error("pinned-context-serialize"))?;
        Ok(format!("pinned-context:sha256:{}", sha256_hex(&bytes)))
    }
}

fn bounded_identifier(
    value: &str,
    max_chars: usize,
    code: &'static str,
) -> Result<(), PinnedContextError> {
    if value.is_empty()
        || value.chars().count() > max_chars
        || value.chars().any(|character| {
            character.is_control()
                || !(character.is_ascii_alphanumeric() || matches!(character, '-' | '_' | ':'))
        })
    {
        return Err(error(code));
    }
    Ok(())
}

fn bounded_text(
    value: &str,
    max_chars: usize,
    code: &'static str,
) -> Result<(), PinnedContextError> {
    if value.is_empty() || value.chars().count() > max_chars || value.chars().any(char::is_control)
    {
        return Err(error(code));
    }
    if redact_complete(value) != value {
        return Err(error("pinned-context-secret-shaped"));
    }
    Ok(())
}

fn validate_reference(value: &str, kind: &str) -> Result<(), PinnedContextError> {
    bounded_text(
        value,
        MAX_REFERENCE_CHARS,
        "pinned-context-reference-invalid",
    )?;
    if value.starts_with('/')
        || value.starts_with('\\')
        || value.starts_with('~')
        || value.contains('\\')
        || value.contains("://")
        || value
            .split('/')
            .any(|component| component == ".." || component.is_empty())
    {
        return Err(error("pinned-context-reference-unsafe"));
    }
    if matches!(kind, "file" | "selection" | "diagnostic") && value.contains(':') {
        return Err(error("pinned-context-reference-path-invalid"));
    }
    Ok(())
}

fn validate_sha256(value: &str) -> Result<(), PinnedContextError> {
    let Some(hex) = value.strip_prefix("sha256:") else {
        return Err(error("pinned-context-hash-invalid"));
    };
    if hex.len() != 64
        || !hex
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
        return Err(error("pinned-context-hash-invalid"));
    }
    Ok(())
}

fn validate_metadata(metadata: &BTreeMap<String, String>) -> Result<(), PinnedContextError> {
    if metadata.len() > MAX_METADATA_ENTRIES {
        return Err(error("pinned-context-metadata-limit"));
    }
    let mut bytes = 0_usize;
    for (key, value) in metadata {
        bounded_identifier(
            key,
            MAX_METADATA_KEY_CHARS,
            "pinned-context-metadata-key-invalid",
        )?;
        bounded_text(
            value,
            MAX_METADATA_VALUE_CHARS,
            "pinned-context-metadata-value-invalid",
        )?;
        let assignment = format!("{key}={value}");
        if redact_complete(&assignment) != assignment {
            return Err(error("pinned-context-secret-shaped"));
        }
        bytes = bytes.saturating_add(key.len()).saturating_add(value.len());
        if bytes > MAX_METADATA_BYTES {
            return Err(error("pinned-context-metadata-too-large"));
        }
    }
    Ok(())
}

fn sha256_hex(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

fn error(code: &'static str) -> PinnedContextError {
    PinnedContextError { code }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn item(kind: &str, reference: &str) -> PinnedContextItem {
        PinnedContextItem {
            id: "pin-1".into(),
            project_id: "project-1".into(),
            session_id: Some("session-1".into()),
            root_id: Some("root-1".into()),
            kind: kind.into(),
            source: "editor-selection".into(),
            label: "src/main.rs:1-2".into(),
            reference: reference.into(),
            content_hash: format!("sha256:{}", "a".repeat(64)),
            bytes: 32,
            revision: Some("revision-1".into()),
            freshness: "fresh".into(),
            priority: 850,
            metadata: BTreeMap::new(),
        }
    }

    #[test]
    fn validates_all_supported_pinned_context_kinds_without_content() {
        for (kind, reference) in [
            ("file", "src/main.rs"),
            ("selection", "src/main.rs"),
            ("image", "blob:sha256:abc"),
            ("diagnostic", "src/main.rs"),
            ("terminal_excerpt", "terminal:output-1"),
            ("git_commit", "git:commit:abc"),
            ("git_diff", "git:diff:abc"),
            ("artifact", "artifact:sha256:abc"),
            ("child_handoff", "handoff:sha256:abc"),
        ] {
            assert_eq!(item(kind, reference).validate(), Ok(()));
        }
    }

    #[test]
    fn rejects_network_absolute_parent_and_secret_references() {
        for reference in ["/tmp/file", "../file", "https://example.invalid/file"] {
            assert_eq!(
                item("file", reference).validate().unwrap_err().code,
                "pinned-context-reference-unsafe"
            );
        }
        let mut secret = item("artifact", "artifact:sha256:abc");
        secret.label = "api_key=do-not-store".into();
        assert_eq!(
            secret.validate().unwrap_err().code,
            "pinned-context-secret-shaped"
        );
    }

    #[test]
    fn set_is_project_bound_bounded_and_content_addressed() {
        let mut set = PinnedContextSet::new("project-1").unwrap();
        set.add(item("file", "src/main.rs")).unwrap();
        assert!(set
            .identity()
            .unwrap()
            .starts_with("pinned-context:sha256:"));
        assert!(!set.remove("missing"));
        assert!(set.remove("pin-1"));
        assert!(set.items.is_empty());
        let mut mismatched = item("file", "src/lib.rs");
        mismatched.project_id = "project-2".into();
        assert_eq!(
            set.add(mismatched).unwrap_err().code,
            "pinned-context-project-mismatch"
        );
    }

    #[test]
    fn rejects_duplicate_ids_and_item_byte_overflow() {
        let mut set = PinnedContextSet::new("project-1").unwrap();
        set.add(item("file", "one.rs")).unwrap();
        assert_eq!(
            set.add(item("selection", "two.rs")).unwrap_err().code,
            "pinned-context-duplicate-id"
        );
        let mut large = item("artifact", "artifact:sha256:abc");
        large.id = "pin-large".into();
        large.bytes = MAX_TOTAL_BYTES;
        assert_eq!(
            set.add(large).unwrap_err().code,
            "pinned-context-item-too-large"
        );

        let mut bounded = PinnedContextSet::new("project-1").unwrap();
        for index in 0..4 {
            let mut candidate = item("artifact", "artifact:sha256:abc");
            candidate.id = format!("pin-{index}");
            candidate.bytes = MAX_ITEM_BYTES;
            bounded.add(candidate).unwrap();
        }
        let mut overflow = item("artifact", "artifact:sha256:def");
        overflow.id = "pin-overflow".into();
        overflow.bytes = 1;
        assert_eq!(
            bounded.add(overflow).unwrap_err().code,
            "pinned-context-total-too-large"
        );
    }

    #[test]
    fn metadata_is_bounded_and_secret_free() {
        let mut pinned = item("file", "src/main.rs");
        pinned.metadata.insert("line".into(), "12".into());
        assert!(pinned.validate().is_ok());
        pinned
            .metadata
            .insert("token".into(), "secret-value".into());
        assert_eq!(
            pinned.validate().unwrap_err().code,
            "pinned-context-secret-shaped"
        );
    }
}
