use crate::workspace::read_text_file_with_bytes;
use crate::workspace_edit::{ContentHash, ProposedContent, WorkspaceEdit, WorkspaceEditOperation};
use crate::workspace_edit_preview::ContentInput;
use diffy::Patch;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::path::{Component, Path};

const MAX_CHANGES: usize = 256;
const MAX_PROVIDER_ID_BYTES: usize = 128;
const MAX_PROVIDER_PATH_BYTES: usize = 4 * 1024;
const MAX_PROVIDER_DIFF_BYTES: usize = 1024 * 1024;
const MAX_PROVIDER_DIFF_TOTAL_BYTES: usize = 4 * 1024 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct CodexFileChangeError {
    pub message: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub(crate) struct CodexFileUpdateChange {
    pub path: String,
    pub diff: String,
    pub kind: CodexPatchChangeKind,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "type", rename_all = "camelCase", deny_unknown_fields)]
pub(crate) enum CodexPatchChangeKind {
    Add,
    Delete,
    Update {
        #[serde(default)]
        move_path: Option<String>,
    },
}

#[derive(Debug, Clone)]
pub(crate) struct CompiledCodexFileChange {
    pub edit: WorkspaceEdit,
    pub contents: Vec<ContentInput>,
}

pub(crate) fn provider_file_change_edit_id(
    session_id: &str,
    turn_id: &str,
    item_id: &str,
    changes: &[CodexFileUpdateChange],
) -> Result<String, CodexFileChangeError> {
    for (label, value) in [
        ("session", session_id),
        ("turn", turn_id),
        ("item", item_id),
    ] {
        if !valid_provider_id(value) {
            return Err(error(format!(
                "Codex file change {label} identity is invalid"
            )));
        }
    }
    validate_codex_file_changes(changes)?;
    let mut hasher = Sha256::new();
    hash_field(&mut hasher, b"aegisy-codex-file-change-edit/0.1");
    hash_field(&mut hasher, session_id.as_bytes());
    hash_field(&mut hasher, turn_id.as_bytes());
    hash_field(&mut hasher, item_id.as_bytes());
    hash_field(&mut hasher, &(changes.len() as u64).to_be_bytes());
    for change in changes {
        hash_field(&mut hasher, change.path.as_bytes());
        match &change.kind {
            CodexPatchChangeKind::Add => hash_field(&mut hasher, b"add"),
            CodexPatchChangeKind::Delete => hash_field(&mut hasher, b"delete"),
            CodexPatchChangeKind::Update { move_path } => {
                hash_field(&mut hasher, b"update");
                hash_field(
                    &mut hasher,
                    move_path.as_deref().unwrap_or_default().as_bytes(),
                );
            }
        }
        hash_field(&mut hasher, change.diff.as_bytes());
    }
    Ok(format!("codex-file-change:sha256:{:x}", hasher.finalize()))
}

pub(crate) fn compile_codex_file_change(
    edit_id: &str,
    project_id: &str,
    project_root: &Path,
    changes: &[CodexFileUpdateChange],
) -> Result<CompiledCodexFileChange, CodexFileChangeError> {
    validate_codex_file_changes(changes)?;
    let canonical_root = project_root
        .canonicalize()
        .map_err(|cause| error(format!("Codex file change root is unavailable: {cause}")))?;
    if !canonical_root.is_dir() {
        return Err(error("Codex file change root is not a directory"));
    }
    let mut operations = Vec::with_capacity(changes.len());
    let mut contents = Vec::new();
    for change in changes {
        let path = provider_relative_path(&canonical_root, &change.path)?;
        match &change.kind {
            CodexPatchChangeKind::Add => {
                let bytes = change.diff.as_bytes().to_vec();
                let content = ProposedContent::for_bytes(&bytes).with_mode("regular");
                contents.push(content_input(&content, bytes)?);
                operations.push(WorkspaceEditOperation::Create { path, content });
            }
            CodexPatchChangeKind::Delete => {
                let (_, base_bytes) = read_text_file_with_bytes(&canonical_root, &path)
                    .map_err(|cause| error(cause.message))?;
                if change.diff.as_bytes() != base_bytes {
                    return Err(error(
                        "Codex delete content does not match the current base",
                    ));
                }
                operations.push(WorkspaceEditOperation::Delete {
                    path,
                    base: ContentHash::for_bytes(&base_bytes),
                });
            }
            CodexPatchChangeKind::Update { move_path } => {
                let (_, base_bytes) = read_text_file_with_bytes(&canonical_root, &path)
                    .map_err(|cause| error(cause.message))?;
                let base_text = std::str::from_utf8(&base_bytes)
                    .map_err(|_| error("Codex file change base text format is unsupported"))?;
                let update_diff = update_diff(change, move_path.as_deref())?;
                let patched_bytes = if update_diff.is_empty() {
                    base_bytes.clone()
                } else {
                    let patch = parse_patch(update_diff)?;
                    apply_patch(base_text, &patch)?.into_bytes()
                };
                let base = ContentHash::for_bytes(&base_bytes);
                if let Some(move_path) = move_path {
                    let to_path = provider_relative_path(&canonical_root, move_path)?;
                    if patched_bytes != base_bytes {
                        return Err(error(
                            "Codex move with content changes requires a future rename-update contract",
                        ));
                    }
                    operations.push(WorkspaceEditOperation::Rename {
                        from_path: path,
                        to_path,
                        base,
                    });
                } else {
                    if patched_bytes == base_bytes {
                        return Err(error("Codex update patch produced no content change"));
                    }
                    let content = ProposedContent::for_bytes(&patched_bytes);
                    contents.push(content_input(&content, patched_bytes)?);
                    operations.push(WorkspaceEditOperation::Update {
                        path,
                        base,
                        content,
                    });
                }
            }
        }
    }
    let edit = WorkspaceEdit::define(edit_id, project_id, &canonical_root, operations)
        .map_err(|cause| error(cause.message))?;
    Ok(CompiledCodexFileChange { edit, contents })
}

pub(crate) fn validate_codex_file_changes(
    changes: &[CodexFileUpdateChange],
) -> Result<(), CodexFileChangeError> {
    if changes.is_empty() || changes.len() > MAX_CHANGES {
        return Err(error(format!(
            "Codex file change must contain 1 to {MAX_CHANGES} changes"
        )));
    }
    let mut total_diff_bytes = 0_usize;
    for change in changes {
        if change.path.is_empty()
            || change.path.len() > MAX_PROVIDER_PATH_BYTES
            || change.path.chars().any(char::is_control)
            || change.diff.len() > MAX_PROVIDER_DIFF_BYTES
            || change.diff.contains('\0')
        {
            return Err(error("Codex file change path or diff is invalid"));
        }
        if matches!(&change.kind, CodexPatchChangeKind::Update { .. }) && change.diff.is_empty() {
            return Err(error("Codex update diff is empty"));
        }
        total_diff_bytes = total_diff_bytes
            .checked_add(change.diff.len())
            .filter(|bytes| *bytes <= MAX_PROVIDER_DIFF_TOTAL_BYTES)
            .ok_or_else(|| error("Codex file change diff budget is exceeded"))?;
        if let CodexPatchChangeKind::Update {
            move_path: Some(move_path),
        } = &change.kind
        {
            if move_path.is_empty()
                || move_path.len() > MAX_PROVIDER_PATH_BYTES
                || move_path.chars().any(char::is_control)
            {
                return Err(error("Codex file change move path is invalid"));
            }
        }
    }
    Ok(())
}

fn provider_relative_path(
    canonical_root: &Path,
    provider_path: &str,
) -> Result<String, CodexFileChangeError> {
    let path = Path::new(provider_path);
    let relative = if path.is_absolute() {
        path.strip_prefix(canonical_root)
            .map_err(|_| error("Codex file change path is outside the project root"))?
    } else {
        path
    };
    let mut parts = Vec::new();
    for component in relative.components() {
        match component {
            Component::Normal(part) => {
                let part = part
                    .to_str()
                    .ok_or_else(|| error("Codex file change path is not UTF-8"))?;
                if part.is_empty() || part.contains('\\') || part.chars().any(char::is_control) {
                    return Err(error("Codex file change path is ambiguous"));
                }
                parts.push(part);
            }
            _ => return Err(error("Codex file change path is not normalized")),
        }
    }
    if parts.is_empty() {
        return Err(error("Codex file change path is empty"));
    }
    Ok(parts.join("/"))
}

fn parse_patch(diff: &str) -> Result<Patch<'_, str>, CodexFileChangeError> {
    let patch =
        Patch::from_str(diff).map_err(|_| error("Codex file change unified patch is malformed"))?;
    if patch.hunks().is_empty() {
        return Err(error("Codex file change patch contains no text hunks"));
    }
    Ok(patch)
}

fn apply_patch(base: &str, patch: &Patch<'_, str>) -> Result<String, CodexFileChangeError> {
    diffy::apply(base, patch)
        .map_err(|_| error("Codex file change patch does not match the current base"))
}

fn update_diff<'a>(
    change: &'a CodexFileUpdateChange,
    move_path: Option<&str>,
) -> Result<&'a str, CodexFileChangeError> {
    let Some(move_path) = move_path else {
        return Ok(&change.diff);
    };
    let suffix = format!("\n\nMoved to: {move_path}");
    change
        .diff
        .strip_suffix(&suffix)
        .ok_or_else(|| error("Codex move diff does not match its move path"))
}

fn content_input(
    content: &ProposedContent,
    bytes: Vec<u8>,
) -> Result<ContentInput, CodexFileChangeError> {
    let content_text =
        String::from_utf8(bytes).map_err(|_| error("Codex proposed content is not valid UTF-8"))?;
    Ok(ContentInput {
        reference: content.reference.clone(),
        content: content_text,
    })
}

fn valid_provider_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= MAX_PROVIDER_ID_BYTES
        && value.bytes().all(|byte| byte.is_ascii_graphic())
}

fn hash_field(hasher: &mut Sha256, bytes: &[u8]) {
    hasher.update((bytes.len() as u64).to_be_bytes());
    hasher.update(bytes);
}

fn error(message: impl Into<String>) -> CodexFileChangeError {
    CodexFileChangeError {
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_root(label: &str) -> std::path::PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root = std::env::temp_dir().join(format!(
            "aegisy-codex-file-change-{label}-{}-{nonce}",
            std::process::id()
        ));
        fs::create_dir_all(&root).unwrap();
        root
    }

    fn change(path: &str, kind: CodexPatchChangeKind, diff: &str) -> CodexFileUpdateChange {
        CodexFileUpdateChange {
            path: path.into(),
            kind,
            diff: diff.into(),
        }
    }

    #[test]
    fn compiles_add_update_delete_and_preserves_crlf_bom() {
        let root = temp_root("operations");
        fs::write(root.join("update.txt"), b"\xef\xbb\xbfbefore\r\n").unwrap();
        fs::write(root.join("delete.txt"), b"remove\n").unwrap();
        let changes = vec![
            change("added.txt", CodexPatchChangeKind::Add, "added\n"),
            change(
                "update.txt",
                CodexPatchChangeKind::Update { move_path: None },
                "@@ -1 +1 @@\n-\u{feff}before\r\n+\u{feff}after\r\n",
            ),
            change("delete.txt", CodexPatchChangeKind::Delete, "remove\n"),
        ];
        let compiled = compile_codex_file_change("edit-1", "project-1", &root, &changes).unwrap();
        assert_eq!(compiled.edit.operations.len(), 3);
        assert_eq!(compiled.contents.len(), 2);
        assert!(compiled
            .contents
            .iter()
            .any(|content| content.content.as_bytes() == b"\xef\xbb\xbfafter\r\n"));
        assert!(!root.join("added.txt").exists());
        assert_eq!(
            fs::read(root.join("update.txt")).unwrap(),
            b"\xef\xbb\xbfbefore\r\n"
        );
        assert_eq!(fs::read(root.join("delete.txt")).unwrap(), b"remove\n");
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn compiles_pure_rename_but_rejects_rename_with_content_change() {
        let root = temp_root("rename");
        fs::write(root.join("old.txt"), b"same\n").unwrap();
        let rename = change(
            "old.txt",
            CodexPatchChangeKind::Update {
                move_path: Some("new.txt".into()),
            },
            "\n\nMoved to: new.txt",
        );
        let compiled =
            compile_codex_file_change("edit-rename", "project-1", &root, &[rename]).unwrap();
        assert!(matches!(
            compiled.edit.operations.as_slice(),
            [WorkspaceEditOperation::Rename { from_path, to_path, .. }]
                if from_path == "old.txt" && to_path == "new.txt"
        ));
        let changed = change(
            "old.txt",
            CodexPatchChangeKind::Update {
                move_path: Some("new.txt".into()),
            },
            "@@ -1 +1 @@\n-same\n+changed\n\nMoved to: new.txt",
        );
        assert!(
            compile_codex_file_change("edit-changed", "project-1", &root, &[changed])
                .unwrap_err()
                .message
                .contains("rename-update")
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_escape_malformed_stale_and_duplicate_changes_without_writes() {
        let root = temp_root("invalid");
        fs::write(root.join("file.txt"), b"current\n").unwrap();
        let escaped = change(
            "../outside.txt",
            CodexPatchChangeKind::Update { move_path: None },
            "@@ -1 +1 @@\n-old\n+new\n",
        );
        assert!(compile_codex_file_change("edit-escape", "project-1", &root, &[escaped]).is_err());
        let malformed = change(
            "file.txt",
            CodexPatchChangeKind::Update { move_path: None },
            "not a patch",
        );
        assert!(
            compile_codex_file_change("edit-malformed", "project-1", &root, &[malformed]).is_err()
        );
        let stale = change(
            "file.txt",
            CodexPatchChangeKind::Update { move_path: None },
            "@@ -1 +1 @@\n-stale\n+new\n",
        );
        assert!(compile_codex_file_change("edit-stale", "project-1", &root, &[stale]).is_err());
        let valid = change(
            "file.txt",
            CodexPatchChangeKind::Update { move_path: None },
            "@@ -1 +1 @@\n-current\n+new\n",
        );
        assert!(compile_codex_file_change(
            "edit-duplicate",
            "project-1",
            &root,
            &[valid.clone(), valid],
        )
        .is_err());
        assert_eq!(fs::read(root.join("file.txt")).unwrap(), b"current\n");
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn edit_identity_is_deterministic_and_binds_provider_changes() {
        let changes = vec![change("file.txt", CodexPatchChangeKind::Add, "value\n")];
        let first =
            provider_file_change_edit_id("session-1", "turn-1", "item-1", &changes).unwrap();
        let second =
            provider_file_change_edit_id("session-1", "turn-1", "item-1", &changes).unwrap();
        assert_eq!(first, second);
        let mut changed = changes;
        changed[0].diff.push_str("+other\n");
        assert_ne!(
            first,
            provider_file_change_edit_id("session-1", "turn-1", "item-1", &changed).unwrap()
        );
    }
}
