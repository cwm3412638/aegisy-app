use crate::workspace::{is_sensitive_path, read_text_file_with_bytes};
use crate::workspace_edit::{
    inspect_text_format, platform_supports_file_mode, ContentHash, ProposedContent,
    ProposedTextFormat, WorkspaceEdit, WorkspaceEditOperation,
};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use similar::{ChangeTag, TextDiff};
use std::collections::{HashMap, HashSet, VecDeque};
use std::fs;
use std::io::{self, Write};
use std::path::{Component, Path};
use std::time::Duration;

const MAX_CONTENT_BYTES: usize = 512 * 1024;
const MAX_CONTENTS_BYTES: usize = 4 * 1024 * 1024;
const MAX_FILE_DIFF_BYTES: usize = 512 * 1024;
const MAX_AGGREGATE_DIFF_BYTES: usize = 2 * 1024 * 1024;
const MAX_INLINE_FILE_DIFF_BYTES: usize = 32 * 1024;
const MAX_INLINE_AGGREGATE_DIFF_BYTES: usize = 64 * 1024;
const MAX_PREVIEWS: usize = 32;
const MAX_STORE_BYTES: usize = 16 * 1024 * 1024;
pub const MAX_PAGE_BYTES: usize = 64 * 1024;
const DIFF_TIMEOUT: Duration = Duration::from_millis(100);

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PreviewError {
    pub message: String,
}

#[derive(Debug, Clone, Deserialize)]
pub struct ContentInput {
    pub reference: String,
    pub content: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceEditPreview {
    pub edit_id: String,
    pub project_id: String,
    pub root_identity: String,
    pub files: Vec<FilePreview>,
    pub additions: usize,
    pub deletions: usize,
    pub warning_count: usize,
    pub applicable: bool,
    pub aggregate_diff: PreviewArtifactDescriptor,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct FilePreview {
    pub kind: String,
    pub path: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub from_path: Option<String>,
    pub additions: usize,
    pub deletions: usize,
    pub base_matches: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub proposed_format: Option<ProposedTextFormat>,
    pub warnings: Vec<PreviewWarning>,
    pub diff: PreviewArtifactDescriptor,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct PreviewWarning {
    pub code: String,
    pub severity: String,
    pub path: String,
    pub message: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct PreviewArtifactDescriptor {
    pub reference: String,
    pub media_type: String,
    pub bytes: usize,
    pub inline: String,
    pub inline_truncated: bool,
    pub source_truncated: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ArtifactPage {
    pub reference: String,
    pub media_type: String,
    pub offset: usize,
    pub next_offset: Option<usize>,
    pub total_bytes: usize,
    pub bytes: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PreviewArtifactSnapshot {
    pub reference: String,
    pub media_type: String,
    pub bytes: Vec<u8>,
}

#[derive(Default)]
pub struct WorkspaceEditPreviewStore {
    records: HashMap<(String, String), PreviewRecord>,
    order: VecDeque<(String, String)>,
    retained_bytes: usize,
}

struct PreviewRecord {
    project_id: String,
    artifacts: HashMap<String, PreviewArtifact>,
    retained_bytes: usize,
}

struct PreviewArtifact {
    media_type: String,
    bytes: Vec<u8>,
}

impl WorkspaceEditPreviewStore {
    pub fn preview(
        &mut self,
        session_id: &str,
        edit: WorkspaceEdit,
        contents: Vec<ContentInput>,
        ignored_paths: &HashSet<String>,
    ) -> Result<WorkspaceEditPreview, PreviewError> {
        let proposed = validate_contents(&edit, contents)?;
        let mut artifacts = HashMap::new();
        for (reference, content) in &proposed {
            artifacts.insert(
                reference.clone(),
                PreviewArtifact {
                    media_type: "text/plain; charset=utf-8".into(),
                    bytes: content.as_bytes().to_vec(),
                },
            );
        }

        let mut files = Vec::with_capacity(edit.operations.len());
        let mut aggregate = BoundedWriter::new(MAX_AGGREGATE_DIFF_BYTES);
        let mut additions = 0_usize;
        let mut deletions = 0_usize;
        let mut file_diff_truncated = false;
        for operation in &edit.operations {
            let file = preview_operation(
                &edit.root.canonical_path,
                operation,
                &proposed,
                ignored_paths,
            )?;
            if !file.diff_bytes.is_empty() {
                aggregate
                    .write_all(&file.diff_bytes)
                    .expect("bounded writer");
                if !file.diff_bytes.ends_with(b"\n") {
                    aggregate.write_all(b"\n").expect("bounded writer");
                }
            }
            additions = additions.saturating_add(file.additions);
            deletions = deletions.saturating_add(file.deletions);
            file_diff_truncated |= file.diff_truncated;
            artifacts.insert(
                file.diff_reference.clone(),
                PreviewArtifact {
                    media_type: "text/x-diff; charset=utf-8".into(),
                    bytes: file.diff_bytes.clone(),
                },
            );
            files.push(file.into_preview());
        }
        let (aggregate_bytes, aggregate_truncated) = aggregate.finish();
        let aggregate_reference = artifact_reference("workspace-edit-diff", &aggregate_bytes);
        artifacts.insert(
            aggregate_reference.clone(),
            PreviewArtifact {
                media_type: "text/x-diff; charset=utf-8".into(),
                bytes: aggregate_bytes.clone(),
            },
        );
        let warning_count = files.iter().map(|file| file.warnings.len()).sum();
        let applicable = files.iter().all(|file| {
            file.warnings
                .iter()
                .all(|warning| warning.severity != "blocking")
        });
        let preview = WorkspaceEditPreview {
            edit_id: edit.edit_id.clone(),
            project_id: edit.project_id.clone(),
            root_identity: edit.root.identity,
            files,
            additions,
            deletions,
            warning_count,
            applicable,
            aggregate_diff: descriptor(
                aggregate_reference,
                "text/x-diff; charset=utf-8",
                &aggregate_bytes,
                MAX_INLINE_AGGREGATE_DIFF_BYTES,
                aggregate_truncated || file_diff_truncated,
            ),
        };
        self.insert(session_id, &edit.edit_id, &edit.project_id, artifacts)?;
        Ok(preview)
    }

    pub fn read(
        &self,
        session_id: &str,
        edit_id: &str,
        project_id: &str,
        reference: &str,
        offset: usize,
        limit: usize,
    ) -> Result<ArtifactPage, PreviewError> {
        if limit == 0 || limit > MAX_PAGE_BYTES {
            return Err(error(format!(
                "workspace edit artifact page must contain 1 to {MAX_PAGE_BYTES} bytes"
            )));
        }
        let record = self
            .records
            .get(&(session_id.to_owned(), edit_id.to_owned()))
            .ok_or_else(|| error("workspace edit preview not found"))?;
        if record.project_id != project_id {
            return Err(error("workspace edit preview does not belong to project"));
        }
        let artifact = record
            .artifacts
            .get(reference)
            .ok_or_else(|| error("workspace edit artifact not found"))?;
        if offset > artifact.bytes.len() {
            return Err(error("workspace edit artifact offset is out of range"));
        }
        let end = offset.saturating_add(limit).min(artifact.bytes.len());
        Ok(ArtifactPage {
            reference: reference.into(),
            media_type: artifact.media_type.clone(),
            offset,
            next_offset: (end < artifact.bytes.len()).then_some(end),
            total_bytes: artifact.bytes.len(),
            bytes: artifact.bytes[offset..end].to_vec(),
        })
    }

    pub fn clear(&mut self) {
        self.records.clear();
        self.order.clear();
        self.retained_bytes = 0;
    }

    pub fn remove_session(&mut self, session_id: &str) {
        let edit_ids = self
            .records
            .keys()
            .filter(|(stored_session_id, _)| stored_session_id == session_id)
            .map(|(_, edit_id)| edit_id.clone())
            .collect::<Vec<_>>();
        for edit_id in edit_ids {
            self.remove(session_id, &edit_id);
        }
    }

    pub fn snapshots(
        &self,
        session_id: &str,
        edit_id: &str,
        project_id: &str,
    ) -> Result<Vec<PreviewArtifactSnapshot>, PreviewError> {
        let key = (session_id.to_owned(), edit_id.to_owned());
        let record = self
            .records
            .get(&key)
            .ok_or_else(|| error("workspace edit preview not found"))?;
        if record.project_id != project_id {
            return Err(error("workspace edit preview does not belong to project"));
        }
        let mut snapshots = record
            .artifacts
            .iter()
            .map(|(reference, artifact)| PreviewArtifactSnapshot {
                reference: reference.clone(),
                media_type: artifact.media_type.clone(),
                bytes: artifact.bytes.clone(),
            })
            .collect::<Vec<_>>();
        snapshots.sort_by(|left, right| left.reference.cmp(&right.reference));
        Ok(snapshots)
    }

    pub fn remove(&mut self, session_id: &str, edit_id: &str) {
        let key = (session_id.to_owned(), edit_id.to_owned());
        if let Some(record) = self.records.remove(&key) {
            self.retained_bytes = self.retained_bytes.saturating_sub(record.retained_bytes);
        }
        self.order.retain(|candidate| candidate != &key);
    }

    fn insert(
        &mut self,
        session_id: &str,
        edit_id: &str,
        project_id: &str,
        artifacts: HashMap<String, PreviewArtifact>,
    ) -> Result<(), PreviewError> {
        let retained_bytes = artifacts
            .values()
            .map(|artifact| artifact.bytes.len())
            .sum();
        if retained_bytes > MAX_STORE_BYTES {
            return Err(error("workspace edit preview exceeds artifact store limit"));
        }
        let key = (session_id.to_owned(), edit_id.to_owned());
        if let Some(previous) = self.records.remove(&key) {
            self.retained_bytes = self.retained_bytes.saturating_sub(previous.retained_bytes);
            self.order.retain(|candidate| candidate != &key);
        }
        while self.records.len() >= MAX_PREVIEWS
            || self.retained_bytes.saturating_add(retained_bytes) > MAX_STORE_BYTES
        {
            let Some(oldest) = self.order.pop_front() else {
                break;
            };
            if let Some(removed) = self.records.remove(&oldest) {
                self.retained_bytes = self.retained_bytes.saturating_sub(removed.retained_bytes);
            }
        }
        self.retained_bytes = self.retained_bytes.saturating_add(retained_bytes);
        self.order.push_back(key.clone());
        self.records.insert(
            key,
            PreviewRecord {
                project_id: project_id.into(),
                artifacts,
                retained_bytes,
            },
        );
        Ok(())
    }
}

struct OperationPreview {
    kind: String,
    path: String,
    from_path: Option<String>,
    additions: usize,
    deletions: usize,
    base_matches: Option<bool>,
    proposed_format: Option<ProposedTextFormat>,
    warnings: Vec<PreviewWarning>,
    diff_reference: String,
    diff_bytes: Vec<u8>,
    diff_truncated: bool,
}

impl OperationPreview {
    fn into_preview(self) -> FilePreview {
        let diff = descriptor(
            self.diff_reference,
            "text/x-diff; charset=utf-8",
            &self.diff_bytes,
            MAX_INLINE_FILE_DIFF_BYTES,
            self.diff_truncated,
        );
        FilePreview {
            kind: self.kind,
            path: self.path,
            from_path: self.from_path,
            additions: self.additions,
            deletions: self.deletions,
            base_matches: self.base_matches,
            proposed_format: self.proposed_format,
            warnings: self.warnings,
            diff,
        }
    }
}

fn preview_operation(
    root: &Path,
    operation: &WorkspaceEditOperation,
    proposed: &HashMap<String, String>,
    ignored_paths: &HashSet<String>,
) -> Result<OperationPreview, PreviewError> {
    match operation {
        WorkspaceEditOperation::Create { path, content } => {
            let mut warnings = path_warnings(root, path, ignored_paths, true);
            if root.join(path).exists() {
                warnings.push(blocking(
                    path,
                    "target-exists",
                    "create target already exists",
                ));
            }
            let new = required_content(proposed, content)?;
            build_text_preview("create", path, None, "", new, None, warnings).map(|mut preview| {
                preview.proposed_format = Some(content.format.clone());
                preview
            })
        }
        WorkspaceEditOperation::Update {
            path,
            base,
            content,
        } => {
            let (old, base_matches, warnings) = base_text(root, path, base, ignored_paths, false);
            let new = required_content(proposed, content)?;
            build_text_preview("update", path, None, &old, new, base_matches, warnings).map(
                |mut preview| {
                    preview.proposed_format = Some(content.format.clone());
                    preview
                },
            )
        }
        WorkspaceEditOperation::Delete { path, base } => {
            let (old, base_matches, warnings) = base_text(root, path, base, ignored_paths, false);
            build_text_preview("delete", path, None, &old, "", base_matches, warnings)
        }
        WorkspaceEditOperation::Rename {
            from_path,
            to_path,
            base,
        } => {
            let (_, base_matches, mut warnings) =
                base_text(root, from_path, base, ignored_paths, false);
            warnings.extend(path_warnings(root, to_path, ignored_paths, true));
            if root.join(to_path).exists() {
                warnings.push(blocking(
                    to_path,
                    "target-exists",
                    "rename target already exists",
                ));
            }
            let text = format!("diff --git a/{from_path} b/{to_path}\nsimilarity index 100%\nrename from {from_path}\nrename to {to_path}\n");
            let bytes = text.into_bytes();
            Ok(OperationPreview {
                kind: "rename".into(),
                path: to_path.clone(),
                from_path: Some(from_path.clone()),
                additions: 0,
                deletions: 0,
                base_matches,
                proposed_format: None,
                warnings,
                diff_reference: artifact_reference("workspace-edit-diff", &bytes),
                diff_bytes: bytes,
                diff_truncated: false,
            })
        }
    }
}

fn build_text_preview(
    kind: &str,
    path: &str,
    from_path: Option<String>,
    old: &str,
    new: &str,
    base_matches: Option<bool>,
    warnings: Vec<PreviewWarning>,
) -> Result<OperationPreview, PreviewError> {
    let mut config = TextDiff::configure();
    config.timeout(DIFF_TIMEOUT);
    let diff = config.diff_lines(old, new);
    let mut additions = 0_usize;
    let mut deletions = 0_usize;
    for change in diff.iter_all_changes() {
        match change.tag() {
            ChangeTag::Insert => additions = additions.saturating_add(1),
            ChangeTag::Delete => deletions = deletions.saturating_add(1),
            ChangeTag::Equal => {}
        }
    }
    let old_header = if kind == "create" {
        "/dev/null".into()
    } else {
        format!("a/{path}")
    };
    let new_header = if kind == "delete" {
        "/dev/null".into()
    } else {
        format!("b/{path}")
    };
    let mut writer = BoundedWriter::new(MAX_FILE_DIFF_BYTES);
    diff.unified_diff()
        .context_radius(3)
        .header(&old_header, &new_header)
        .to_writer(&mut writer)
        .map_err(|cause| error(format!("cannot render workspace edit diff: {cause}")))?;
    let (diff_bytes, diff_truncated) = writer.finish();
    Ok(OperationPreview {
        kind: kind.into(),
        path: path.into(),
        from_path,
        additions,
        deletions,
        base_matches,
        proposed_format: None,
        warnings,
        diff_reference: artifact_reference("workspace-edit-diff", &diff_bytes),
        diff_bytes,
        diff_truncated,
    })
}

fn base_text(
    root: &Path,
    path: &str,
    base: &ContentHash,
    ignored_paths: &HashSet<String>,
    allow_missing: bool,
) -> (String, Option<bool>, Vec<PreviewWarning>) {
    let mut warnings = path_warnings(root, path, ignored_paths, allow_missing);
    if warnings
        .iter()
        .any(|warning| warning.severity == "blocking")
    {
        return (String::new(), None, warnings);
    }
    match read_text_file_with_bytes(root, path) {
        Ok((file, bytes)) => {
            let matches = ContentHash::for_bytes(&bytes) == *base;
            if inspect_text_format(&bytes, "preserve").newline == "mixed" {
                warnings.push(blocking(
                    path,
                    "mixed-line-endings",
                    "base file has mixed or unsupported line endings",
                ));
            }
            if !matches {
                warnings.push(blocking(
                    path,
                    "stale-base",
                    "base SHA-256 does not match the current file",
                ));
            }
            (file.content, Some(matches), warnings)
        }
        Err(cause) => {
            warnings.push(blocking(path, "base-unavailable", cause.message));
            (String::new(), None, warnings)
        }
    }
}

fn path_warnings(
    root: &Path,
    path: &str,
    ignored_paths: &HashSet<String>,
    allow_missing: bool,
) -> Vec<PreviewWarning> {
    let mut warnings = Vec::new();
    if is_sensitive_path(Path::new(path)) {
        warnings.push(blocking(
            path,
            "sensitive-path",
            "path is denied by the sensitive workspace policy",
        ));
    }
    if ignored_paths.contains(path) {
        warnings.push(blocking(
            path,
            "ignored-path",
            "path is ignored by project policy",
        ));
    }
    if let Some(message) = unsafe_component(root, path, allow_missing) {
        warnings.push(blocking(path, "unsafe-path", message));
    }
    warnings
}

fn unsafe_component(root: &Path, relative: &str, allow_missing: bool) -> Option<String> {
    let mut candidate = root.to_path_buf();
    let components = Path::new(relative).components().collect::<Vec<_>>();
    for (index, component) in components.iter().enumerate() {
        let Component::Normal(name) = component else {
            return Some("path is not normalized".into());
        };
        candidate.push(name);
        match fs::symlink_metadata(&candidate) {
            Ok(metadata) if metadata.file_type().is_symlink() => {
                return Some("path traverses a symbolic link".into())
            }
            Ok(_) => {}
            Err(cause)
                if cause.kind() == io::ErrorKind::NotFound
                    && allow_missing
                    && index == components.len().saturating_sub(1) => {}
            Err(cause) if cause.kind() == io::ErrorKind::NotFound && allow_missing => break,
            Err(cause) => return Some(format!("path cannot be inspected: {cause}")),
        }
    }
    None
}

pub(crate) fn validate_contents(
    edit: &WorkspaceEdit,
    contents: Vec<ContentInput>,
) -> Result<HashMap<String, String>, PreviewError> {
    let required = edit
        .operations
        .iter()
        .filter_map(|operation| match operation {
            WorkspaceEditOperation::Create { content, .. }
            | WorkspaceEditOperation::Update { content, .. } => Some(content),
            WorkspaceEditOperation::Delete { .. } | WorkspaceEditOperation::Rename { .. } => None,
        })
        .map(|content| (content.reference.clone(), content.hash.clone()))
        .collect::<HashMap<_, _>>();
    let mut proposed = HashMap::new();
    let mut total_bytes = 0_usize;
    for input in contents {
        let Some(expected) = required.get(&input.reference) else {
            return Err(error(
                "workspace edit contains an unreferenced content body",
            ));
        };
        if proposed.contains_key(&input.reference) {
            return Err(error("workspace edit content reference is duplicated"));
        }
        let bytes = input.content.as_bytes();
        if input.content.contains('\0') {
            return Err(error("workspace edit preview supports UTF-8 text only"));
        }
        if bytes.len() > MAX_CONTENT_BYTES {
            return Err(error("workspace edit content exceeds per-file limit"));
        }
        total_bytes = total_bytes.saturating_add(bytes.len());
        if total_bytes > MAX_CONTENTS_BYTES {
            return Err(error("workspace edit content exceeds aggregate limit"));
        }
        if ContentHash::for_bytes(bytes) != *expected {
            return Err(error("workspace edit content does not match its SHA-256"));
        }
        proposed.insert(input.reference, input.content);
    }
    if proposed.len() != required.len() {
        return Err(error("workspace edit is missing referenced content"));
    }
    for content in edit
        .operations
        .iter()
        .filter_map(|operation| match operation {
            WorkspaceEditOperation::Create { content, .. }
            | WorkspaceEditOperation::Update { content, .. } => Some(content),
            WorkspaceEditOperation::Delete { .. } | WorkspaceEditOperation::Rename { .. } => None,
        })
    {
        if !platform_supports_file_mode(&content.format.mode) {
            return Err(error(
                "workspace edit executable mode requires POSIX file-mode support on this platform",
            ));
        }
        let body = proposed
            .get(&content.reference)
            .expect("required workspace edit content");
        if inspect_text_format(body.as_bytes(), &content.format.mode) != content.format {
            return Err(error(
                "workspace edit content bytes do not match declared encoding/newline/mode policy",
            ));
        }
    }
    Ok(proposed)
}

fn required_content<'a>(
    proposed: &'a HashMap<String, String>,
    content: &ProposedContent,
) -> Result<&'a str, PreviewError> {
    proposed
        .get(&content.reference)
        .map(String::as_str)
        .ok_or_else(|| error("workspace edit is missing referenced content"))
}

fn blocking(path: &str, code: &str, message: impl Into<String>) -> PreviewWarning {
    PreviewWarning {
        code: code.into(),
        severity: "blocking".into(),
        path: path.into(),
        message: message.into(),
    }
}

fn descriptor(
    reference: String,
    media_type: &str,
    bytes: &[u8],
    inline_limit: usize,
    source_truncated: bool,
) -> PreviewArtifactDescriptor {
    let inline_end = utf8_boundary(bytes, inline_limit.min(bytes.len()));
    PreviewArtifactDescriptor {
        reference,
        media_type: media_type.into(),
        bytes: bytes.len(),
        inline: String::from_utf8_lossy(&bytes[..inline_end]).into_owned(),
        inline_truncated: inline_end < bytes.len(),
        source_truncated,
    }
}

fn artifact_reference(kind: &str, bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    format!("{kind}:sha256:{digest:x}")
}

fn utf8_boundary(bytes: &[u8], mut end: usize) -> usize {
    while end > 0 && std::str::from_utf8(&bytes[..end]).is_err() {
        end -= 1;
    }
    end
}

struct BoundedWriter {
    bytes: Vec<u8>,
    limit: usize,
    truncated: bool,
}

impl BoundedWriter {
    fn new(limit: usize) -> Self {
        Self {
            bytes: Vec::new(),
            limit,
            truncated: false,
        }
    }

    fn finish(mut self) -> (Vec<u8>, bool) {
        let end = utf8_boundary(&self.bytes, self.bytes.len());
        self.bytes.truncate(end);
        (self.bytes, self.truncated)
    }
}

impl Write for BoundedWriter {
    fn write(&mut self, input: &[u8]) -> io::Result<usize> {
        let remaining = self.limit.saturating_sub(self.bytes.len());
        let retained = remaining.min(input.len());
        self.bytes.extend_from_slice(&input[..retained]);
        self.truncated |= retained < input.len();
        Ok(input.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

fn error(message: impl Into<String>) -> PreviewError {
    PreviewError {
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::{SystemTime, UNIX_EPOCH};

    static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

    fn root() -> std::path::PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let sequence = FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!("aegisy-edit-preview-{nonce}-{sequence}"));
        fs::create_dir_all(root.join("src")).unwrap();
        root.canonicalize().unwrap()
    }

    #[test]
    fn previews_all_operations_and_pages_content_without_writing() {
        let root = root();
        fs::write(root.join("src/update.rs"), "fn before() {}\n").unwrap();
        fs::write(root.join("src/delete.rs"), "delete me\n").unwrap();
        fs::write(root.join("src/old.rs"), "rename me\n").unwrap();
        let update_base = ContentHash::for_bytes(b"fn before() {}\n");
        let delete_base = ContentHash::for_bytes(b"delete me\n");
        let rename_base = ContentHash::for_bytes(b"rename me\n");
        let created = ProposedContent::for_bytes("新文件\n".as_bytes());
        let updated = ProposedContent::for_bytes(b"fn after() {}\n");
        let edit = WorkspaceEdit::define(
            "edit-preview",
            "project-1",
            &root,
            vec![
                WorkspaceEditOperation::Create {
                    path: "src/create.rs".into(),
                    content: created.clone(),
                },
                WorkspaceEditOperation::Update {
                    path: "src/update.rs".into(),
                    base: update_base,
                    content: updated.clone(),
                },
                WorkspaceEditOperation::Delete {
                    path: "src/delete.rs".into(),
                    base: delete_base,
                },
                WorkspaceEditOperation::Rename {
                    from_path: "src/old.rs".into(),
                    to_path: "src/new.rs".into(),
                    base: rename_base,
                },
            ],
        )
        .unwrap();
        let mut store = WorkspaceEditPreviewStore::default();
        let preview = store
            .preview(
                "session-1",
                edit,
                vec![
                    ContentInput {
                        reference: created.reference.clone(),
                        content: "新文件\n".into(),
                    },
                    ContentInput {
                        reference: updated.reference.clone(),
                        content: "fn after() {}\n".into(),
                    },
                ],
                &HashSet::new(),
            )
            .unwrap();
        assert!(preview.applicable);
        assert_eq!(preview.files.len(), 4);
        assert_eq!(
            preview.files[0]
                .proposed_format
                .as_ref()
                .map(|format| format.mode.as_str()),
            Some("regular")
        );
        assert_eq!(
            preview.files[1]
                .proposed_format
                .as_ref()
                .map(|format| format.mode.as_str()),
            Some("preserve")
        );
        assert!(preview.additions >= 2);
        assert!(preview.deletions >= 2);
        assert!(preview.aggregate_diff.inline.contains("src/update.rs"));
        assert!(!root.join("src/create.rs").exists());
        assert!(root.join("src/old.rs").exists());

        let first = store
            .read(
                "session-1",
                "edit-preview",
                "project-1",
                &created.reference,
                0,
                4,
            )
            .unwrap();
        assert_eq!(first.offset, 0);
        assert_eq!(first.next_offset, Some(4));
        let second = store
            .read(
                "session-1",
                "edit-preview",
                "project-1",
                &created.reference,
                4,
                MAX_PAGE_BYTES,
            )
            .unwrap();
        let mut content = first.bytes;
        content.extend(second.bytes);
        assert_eq!(content, "新文件\n".as_bytes());
        assert!(store
            .read(
                "other-session",
                "edit-preview",
                "project-1",
                &created.reference,
                0,
                10,
            )
            .is_err());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn warns_without_reading_sensitive_ignored_stale_or_symlinked_bases() {
        let root = root();
        fs::write(root.join("src/stale.rs"), "current\n").unwrap();
        fs::write(root.join(".env"), "DO_NOT_READ=secret\n").unwrap();
        fs::write(root.join("src/ignored.rs"), "ignored\n").unwrap();
        #[cfg(unix)]
        std::os::unix::fs::symlink(root.join("src/stale.rs"), root.join("src/link.rs")).unwrap();
        let proposed = ProposedContent::for_bytes(b"replacement\n");
        #[cfg_attr(windows, allow(unused_mut))]
        let mut operations = vec![
            WorkspaceEditOperation::Update {
                path: ".env".into(),
                base: ContentHash::for_bytes(b"DO_NOT_READ=secret\n"),
                content: proposed.clone(),
            },
            WorkspaceEditOperation::Update {
                path: "src/ignored.rs".into(),
                base: ContentHash::for_bytes(b"ignored\n"),
                content: proposed.clone(),
            },
            WorkspaceEditOperation::Update {
                path: "src/stale.rs".into(),
                base: ContentHash::for_bytes(b"old\n"),
                content: proposed.clone(),
            },
        ];
        #[cfg(unix)]
        operations.push(WorkspaceEditOperation::Delete {
            path: "src/link.rs".into(),
            base: ContentHash::for_bytes(b"current\n"),
        });
        let edit = WorkspaceEdit::define("edit-warning", "project-1", &root, operations).unwrap();
        let mut ignored = HashSet::new();
        ignored.insert("src/ignored.rs".into());
        let mut store = WorkspaceEditPreviewStore::default();
        let preview = store
            .preview(
                "session-1",
                edit,
                vec![ContentInput {
                    reference: proposed.reference,
                    content: "replacement\n".into(),
                }],
                &ignored,
            )
            .unwrap();
        assert!(!preview.applicable);
        let codes = preview
            .files
            .iter()
            .flat_map(|file| file.warnings.iter().map(|warning| warning.code.as_str()))
            .collect::<HashSet<_>>();
        assert!(codes.contains("sensitive-path"));
        assert!(codes.contains("ignored-path"));
        assert!(codes.contains("stale-base"));
        #[cfg(unix)]
        assert!(codes.contains("unsafe-path"));
        assert!(!preview.aggregate_diff.inline.contains("DO_NOT_READ"));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_missing_mismatched_and_excessive_content() {
        let root = root();
        let content = ProposedContent::for_bytes(b"expected");
        let edit = WorkspaceEdit::define(
            "edit-invalid-content",
            "project-1",
            &root,
            vec![WorkspaceEditOperation::Create {
                path: "src/new.rs".into(),
                content: content.clone(),
            }],
        )
        .unwrap();
        let mut store = WorkspaceEditPreviewStore::default();
        assert!(store
            .preview("session", edit.clone(), Vec::new(), &HashSet::new())
            .is_err());
        assert!(store
            .preview(
                "session",
                edit.clone(),
                vec![ContentInput {
                    reference: content.reference.clone(),
                    content: "wrong".into(),
                }],
                &HashSet::new(),
            )
            .is_err());
        let mut wrong_format = edit.clone();
        if let WorkspaceEditOperation::Create { content, .. } = &mut wrong_format.operations[0] {
            content.format.newline = "lf".into();
        }
        assert!(store
            .preview(
                "session",
                wrong_format,
                vec![ContentInput {
                    reference: content.reference.clone(),
                    content: "expected".into(),
                }],
                &HashSet::new(),
            )
            .is_err());
        assert!(store
            .preview(
                "session",
                edit,
                vec![ContentInput {
                    reference: content.reference,
                    content: "x".repeat(MAX_CONTENT_BYTES + 1),
                }],
                &HashSet::new(),
            )
            .is_err());
        fs::remove_dir_all(root).unwrap();
    }
}
