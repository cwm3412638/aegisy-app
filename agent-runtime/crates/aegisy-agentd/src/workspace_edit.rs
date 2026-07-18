use serde::{Deserialize, Deserializer, Serialize};
use sha2::{Digest, Sha256};
use std::collections::HashSet;
use std::path::{Component, Path, PathBuf};

const SCHEMA_VERSION: &str = "workspace-edit/0.2";
const MAX_OPERATIONS: usize = 256;
const MAX_ID_BYTES: usize = 256;
const MAX_PATH_BYTES: usize = 4 * 1024;
const CONTENT_REFERENCE_PREFIX: &str = "workspace-edit-content:sha256:";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorkspaceEditError {
    pub message: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceEdit {
    pub schema_version: String,
    pub edit_id: String,
    pub project_id: String,
    pub root: WorkspaceEditRoot,
    pub operations: Vec<WorkspaceEditOperation>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct WorkspaceEditRoot {
    pub canonical_path: PathBuf,
    pub identity: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ContentHash {
    pub sha256: String,
    pub bytes: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ProposedContent {
    pub reference: String,
    pub hash: ContentHash,
    pub format: ProposedTextFormat,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ProposedTextFormat {
    pub encoding: String,
    pub newline: String,
    pub mode: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum WorkspaceEditOperation {
    Create {
        path: String,
        content: ProposedContent,
    },
    Update {
        path: String,
        base: ContentHash,
        content: ProposedContent,
    },
    Delete {
        path: String,
        base: ContentHash,
    },
    Rename {
        from_path: String,
        to_path: String,
        base: ContentHash,
    },
}

#[derive(Deserialize)]
struct WorkspaceEditWire {
    schema_version: String,
    edit_id: String,
    project_id: String,
    root: WorkspaceEditRoot,
    operations: Vec<WorkspaceEditOperation>,
}

impl WorkspaceEdit {
    pub fn define(
        edit_id: impl Into<String>,
        project_id: impl Into<String>,
        project_root: &Path,
        mut operations: Vec<WorkspaceEditOperation>,
    ) -> Result<Self, WorkspaceEditError> {
        let edit_id = edit_id.into();
        let project_id = project_id.into();
        validate_identifier("edit_id", &edit_id)?;
        validate_identifier("project_id", &project_id)?;
        if operations.is_empty() || operations.len() > MAX_OPERATIONS {
            return Err(error(format!(
                "workspace edit must contain 1 to {MAX_OPERATIONS} operations"
            )));
        }
        let canonical_root = project_root
            .canonicalize()
            .map_err(|cause| error(format!("workspace edit root is unavailable: {cause}")))?;
        if !canonical_root.is_dir() {
            return Err(error("workspace edit root is not a directory"));
        }

        let mut touched_paths = HashSet::new();
        for operation in &mut operations {
            match operation {
                WorkspaceEditOperation::Create { path, content } => {
                    normalize_in_place(path)?;
                    if content.format.mode == "preserve" {
                        content.format.mode = "regular".into();
                    }
                    validate_content(content)?;
                    claim_path(&mut touched_paths, path)?;
                }
                WorkspaceEditOperation::Update {
                    path,
                    base,
                    content,
                } => {
                    normalize_in_place(path)?;
                    validate_hash(base)?;
                    validate_content(content)?;
                    claim_path(&mut touched_paths, path)?;
                }
                WorkspaceEditOperation::Delete { path, base } => {
                    normalize_in_place(path)?;
                    validate_hash(base)?;
                    claim_path(&mut touched_paths, path)?;
                }
                WorkspaceEditOperation::Rename {
                    from_path,
                    to_path,
                    base,
                } => {
                    normalize_in_place(from_path)?;
                    normalize_in_place(to_path)?;
                    if from_path == to_path {
                        return Err(error("rename source and target must differ"));
                    }
                    validate_hash(base)?;
                    claim_path(&mut touched_paths, from_path)?;
                    claim_path(&mut touched_paths, to_path)?;
                }
            }
        }

        let root_bytes = canonical_root.to_string_lossy();
        let root_digest = Sha256::digest(root_bytes.as_bytes());
        Ok(Self {
            schema_version: SCHEMA_VERSION.into(),
            edit_id,
            project_id,
            root: WorkspaceEditRoot {
                canonical_path: canonical_root,
                identity: format!("workspace-root:sha256:{root_digest:x}"),
            },
            operations,
        })
    }
}

impl<'de> Deserialize<'de> for WorkspaceEdit {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let wire = WorkspaceEditWire::deserialize(deserializer)?;
        if wire.schema_version != SCHEMA_VERSION {
            return Err(serde::de::Error::custom(
                "unsupported workspace edit schema version",
            ));
        }
        let declared_root = wire.root;
        let edit = WorkspaceEdit::define(
            wire.edit_id,
            wire.project_id,
            &declared_root.canonical_path,
            wire.operations,
        )
        .map_err(|cause| serde::de::Error::custom(cause.message))?;
        if edit.root != declared_root {
            return Err(serde::de::Error::custom(
                "workspace edit root identity is not canonical",
            ));
        }
        Ok(edit)
    }
}

impl ContentHash {
    pub fn for_bytes(bytes: &[u8]) -> Self {
        let digest = Sha256::digest(bytes);
        Self {
            sha256: format!("{digest:x}"),
            bytes: bytes.len() as u64,
        }
    }
}

impl ProposedContent {
    pub fn for_bytes(bytes: &[u8]) -> Self {
        let hash = ContentHash::for_bytes(bytes);
        Self {
            reference: format!("{CONTENT_REFERENCE_PREFIX}{}", hash.sha256),
            hash,
            format: inspect_text_format(bytes, "preserve"),
        }
    }

    pub fn with_mode(mut self, mode: &str) -> Self {
        self.format.mode = mode.into();
        self
    }
}

pub fn inspect_text_format(bytes: &[u8], mode: &str) -> ProposedTextFormat {
    let (encoding, text) = if let Some(text) = bytes.strip_prefix(&[0xef, 0xbb, 0xbf]) {
        ("utf-8-bom", text)
    } else {
        ("utf-8", bytes)
    };
    if bytes.contains(&0) || std::str::from_utf8(text).is_err() {
        return ProposedTextFormat {
            encoding: "binary".into(),
            newline: "unsupported".into(),
            mode: mode.into(),
        };
    }
    let mut lf = 0_usize;
    let mut crlf = 0_usize;
    let mut lone_cr = 0_usize;
    let mut index = 0_usize;
    while index < text.len() {
        match text[index] {
            b'\r' if text.get(index + 1) == Some(&b'\n') => {
                crlf += 1;
                index += 2;
            }
            b'\r' => {
                lone_cr += 1;
                index += 1;
            }
            b'\n' => {
                lf += 1;
                index += 1;
            }
            _ => index += 1,
        }
    }
    let newline = match (lf, crlf, lone_cr) {
        (0, 0, 0) => "none",
        (_, 0, 0) => "lf",
        (0, _, 0) => "crlf",
        _ => "mixed",
    };
    ProposedTextFormat {
        encoding: encoding.into(),
        newline: newline.into(),
        mode: mode.into(),
    }
}

pub fn platform_supports_file_mode(mode: &str) -> bool {
    matches!(mode, "preserve" | "regular") || (mode == "executable" && cfg!(unix))
}

fn validate_identifier(label: &str, value: &str) -> Result<(), WorkspaceEditError> {
    if value.is_empty()
        || value.len() > MAX_ID_BYTES
        || value.chars().any(char::is_control)
        || value.trim() != value
    {
        return Err(error(format!(
            "{label} must contain 1 to {MAX_ID_BYTES} printable bytes"
        )));
    }
    Ok(())
}

fn normalize_in_place(path: &mut String) -> Result<(), WorkspaceEditError> {
    *path = normalize_path(path)?;
    Ok(())
}

fn normalize_path(value: &str) -> Result<String, WorkspaceEditError> {
    if value.is_empty()
        || value.len() > MAX_PATH_BYTES
        || value.contains('\\')
        || value.chars().any(char::is_control)
    {
        return Err(error("workspace edit path is invalid or too long"));
    }
    let path = Path::new(value);
    if path.is_absolute() {
        return Err(error("workspace edit paths must be root-relative"));
    }
    let mut segments = Vec::new();
    for component in path.components() {
        match component {
            Component::Normal(segment) => {
                let segment = segment
                    .to_str()
                    .ok_or_else(|| error("workspace edit paths must be valid UTF-8"))?;
                segments.push(segment);
            }
            Component::CurDir => {}
            Component::ParentDir | Component::RootDir | Component::Prefix(_) => {
                return Err(error("workspace edit path escapes its project root"))
            }
        }
    }
    if segments.is_empty() {
        return Err(error("workspace edit path must name a file"));
    }
    Ok(segments.join("/"))
}

fn validate_hash(hash: &ContentHash) -> Result<(), WorkspaceEditError> {
    if hash.sha256.len() != 64
        || !hash
            .sha256
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return Err(error(
            "workspace edit content hash must be lowercase SHA-256",
        ));
    }
    Ok(())
}

fn validate_content(content: &ProposedContent) -> Result<(), WorkspaceEditError> {
    validate_hash(&content.hash)?;
    let expected = format!("{CONTENT_REFERENCE_PREFIX}{}", content.hash.sha256);
    if content.reference != expected {
        return Err(error(
            "workspace edit content reference does not match its SHA-256",
        ));
    }
    if !matches!(content.format.encoding.as_str(), "utf-8" | "utf-8-bom")
        || !matches!(content.format.newline.as_str(), "none" | "lf" | "crlf")
        || !matches!(
            content.format.mode.as_str(),
            "preserve" | "regular" | "executable"
        )
    {
        return Err(error(
            "workspace edit content format or file mode policy is unsupported",
        ));
    }
    Ok(())
}

fn claim_path(paths: &mut HashSet<String>, path: &str) -> Result<(), WorkspaceEditError> {
    if paths.insert(path.to_owned()) {
        Ok(())
    } else {
        Err(error(format!(
            "workspace edit path appears in multiple operations: {path}"
        )))
    }
}

fn error(message: impl Into<String>) -> WorkspaceEditError {
    WorkspaceEditError {
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::{SystemTime, UNIX_EPOCH};

    static TEST_DIRECTORY_SEQUENCE: AtomicU64 = AtomicU64::new(0);

    fn root() -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let sequence = TEST_DIRECTORY_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!("aegisy-workspace-edit-{nonce}-{sequence}"));
        fs::create_dir_all(&root).unwrap();
        root.canonicalize().unwrap()
    }

    #[test]
    fn defines_all_operation_kinds_with_canonical_root_and_base_hashes() {
        let root = root();
        let base = ContentHash::for_bytes(b"before\n");
        let edit = WorkspaceEdit::define(
            "edit-1",
            "project-1",
            &root,
            vec![
                WorkspaceEditOperation::Create {
                    path: "src/./created.rs".into(),
                    content: ProposedContent::for_bytes(b"created\n"),
                },
                WorkspaceEditOperation::Update {
                    path: "src/updated.rs".into(),
                    base: base.clone(),
                    content: ProposedContent::for_bytes(b"updated\n"),
                },
                WorkspaceEditOperation::Delete {
                    path: "src/deleted.rs".into(),
                    base: base.clone(),
                },
                WorkspaceEditOperation::Rename {
                    from_path: "src/old.rs".into(),
                    to_path: "src/new.rs".into(),
                    base,
                },
            ],
        )
        .unwrap();

        assert_eq!(edit.schema_version, SCHEMA_VERSION);
        assert_eq!(edit.root.canonical_path, root);
        assert!(edit.root.identity.starts_with("workspace-root:sha256:"));
        assert!(matches!(
            &edit.operations[0],
            WorkspaceEditOperation::Create { path, .. } if path == "src/created.rs"
        ));
        let encoded = serde_json::to_value(&edit).unwrap();
        assert_eq!(encoded["operations"][0]["kind"], "create");
        assert_eq!(encoded["operations"][1]["kind"], "update");
        assert_eq!(encoded["operations"][2]["kind"], "delete");
        assert_eq!(encoded["operations"][3]["kind"], "rename");
        let decoded: WorkspaceEdit = serde_json::from_value(encoded).unwrap();
        assert_eq!(decoded, edit);

        let mut tampered = serde_json::to_value(&edit).unwrap();
        tampered["root"]["identity"] = serde_json::Value::String("forged".into());
        assert!(serde_json::from_value::<WorkspaceEdit>(tampered).is_err());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_escaping_ambiguous_duplicate_and_unbound_content() {
        let root = root();
        let base = ContentHash::for_bytes(b"base");
        for path in ["", "/tmp/outside", "../outside", "src\\ambiguous.rs"] {
            let failure = WorkspaceEdit::define(
                "edit-invalid",
                "project-1",
                &root,
                vec![WorkspaceEditOperation::Delete {
                    path: path.into(),
                    base: base.clone(),
                }],
            )
            .unwrap_err();
            assert!(failure.message.contains("path"));
        }

        let duplicate = WorkspaceEdit::define(
            "edit-duplicate",
            "project-1",
            &root,
            vec![
                WorkspaceEditOperation::Delete {
                    path: "same.rs".into(),
                    base: base.clone(),
                },
                WorkspaceEditOperation::Update {
                    path: "same.rs".into(),
                    base,
                    content: ProposedContent::for_bytes(b"next"),
                },
            ],
        )
        .unwrap_err();
        assert!(duplicate.message.contains("multiple operations"));

        let mut content = ProposedContent::for_bytes(b"content");
        content.reference = format!("{CONTENT_REFERENCE_PREFIX}{}", "0".repeat(64));
        let mismatch = WorkspaceEdit::define(
            "edit-mismatch",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Create {
                path: "new.rs".into(),
                content,
            }],
        )
        .unwrap_err();
        assert!(mismatch.message.contains("does not match"));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn declares_encoding_newline_and_mode_and_rejects_unsupported_text() {
        let root = root();
        for (bytes, encoding, newline) in [
            (b"plain".as_slice(), "utf-8", "none"),
            (b"line\n".as_slice(), "utf-8", "lf"),
            (b"line\r\n".as_slice(), "utf-8", "crlf"),
            (b"\xef\xbb\xbfline\r\n".as_slice(), "utf-8-bom", "crlf"),
        ] {
            let content = ProposedContent::for_bytes(bytes);
            assert_eq!(content.format.encoding, encoding);
            assert_eq!(content.format.newline, newline);
        }
        assert_eq!(
            ProposedContent::for_bytes(b"script\n")
                .with_mode("executable")
                .format
                .mode,
            "executable"
        );
        let edit = WorkspaceEdit::define(
            "edit-format",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Create {
                path: "script.sh".into(),
                content: ProposedContent::for_bytes(b"echo ok\n"),
            }],
        )
        .unwrap();
        let WorkspaceEditOperation::Create { content, .. } = &edit.operations[0] else {
            unreachable!()
        };
        assert_eq!(content.format.mode, "regular");

        for bytes in [
            b"one\r\ntwo\n".as_slice(),
            b"lone\rreturn".as_slice(),
            b"binary\0body".as_slice(),
            &[0xff, 0xfe],
        ] {
            assert!(WorkspaceEdit::define(
                "edit-unsupported",
                "project-1",
                &root,
                vec![WorkspaceEditOperation::Create {
                    path: "bad.txt".into(),
                    content: ProposedContent::for_bytes(bytes),
                }]
            )
            .is_err());
        }
        let mut encoded = serde_json::to_value(&edit).unwrap();
        encoded["operations"][0]["content"]["format"] = serde_json::Value::Null;
        assert!(serde_json::from_value::<WorkspaceEdit>(encoded).is_err());
        let mut old_schema = serde_json::to_value(&edit).unwrap();
        old_schema["schema_version"] = serde_json::Value::String("workspace-edit/0.1".into());
        assert!(serde_json::from_value::<WorkspaceEdit>(old_schema).is_err());

        let mut unsupported_mode = ProposedContent::for_bytes(b"body");
        unsupported_mode.format.mode = "setuid".into();
        assert!(WorkspaceEdit::define(
            "edit-unsupported-mode",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Create {
                path: "bad-mode.txt".into(),
                content: unsupported_mode,
            }]
        )
        .is_err());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn executable_mode_support_is_explicitly_platform_bound() {
        assert!(platform_supports_file_mode("preserve"));
        assert!(platform_supports_file_mode("regular"));
        assert_eq!(platform_supports_file_mode("executable"), cfg!(unix));
        assert!(!platform_supports_file_mode("setuid"));
    }
}
