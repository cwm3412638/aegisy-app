use crate::workspace_edit::{
    ContentHash, ProposedContent, ProposedTextFormat, WorkspaceEdit, WorkspaceEditOperation,
};
use crate::workspace_edit_overlap::{
    proposal_overlap_baseline, ExpectedWorkspacePathState, WorkspaceEditOverlapPhase,
};
use crate::workspace_edit_preview::{
    PreviewArtifactDescriptor, PreviewArtifactSnapshot, WorkspaceEditPreview,
};
use serde::{Deserialize, Deserializer, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::path::{Component, Path};

pub const SCHEMA_VERSION: &str = "workspace-edit-proposal/0.2";
pub const LEGACY_SCHEMA_VERSION: &str = "workspace-edit-proposal/0.1";
pub const EVENT_SCHEMA_VERSION: &str = "workspace-edit.proposal-recorded/0.1";
const BASELINE_SCHEMA_VERSION: &str = "workspace-edit-proposal-baseline/0.1";
const MAX_IDENTIFIER_BYTES: usize = 256;
const MAX_IDENTITY_BYTES: usize = 512;
const MAX_PATH_BYTES: usize = 4 * 1024;
const MAX_OPERATIONS: usize = 256;
const MAX_ARTIFACTS: usize = 513;
const MAX_CONTENT_BYTES: u64 = 512 * 1024;
const MAX_FILE_DIFF_BYTES: u64 = 512 * 1024;
const MAX_AGGREGATE_DIFF_BYTES: u64 = 2 * 1024 * 1024;
const MAX_INLINE_FILE_DIFF_BYTES: u64 = 32 * 1024;
const MAX_INLINE_AGGREGATE_DIFF_BYTES: u64 = 64 * 1024;
const CONTENT_REFERENCE_PREFIX: &str = "workspace-edit-content:sha256:";
const DIFF_REFERENCE_PREFIX: &str = "workspace-edit-diff:sha256:";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorkspaceEditProposalError {
    pub message: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceEditProposal {
    pub schema_version: String,
    pub proposal_id: String,
    pub session_id: String,
    pub turn_id: String,
    pub provider: Option<String>,
    pub provider_thread_id: String,
    pub provider_item_id: String,
    pub approval_started_at_ms: u64,
    pub project_id: String,
    pub root_id: String,
    pub filesystem_root_identity: String,
    pub workspace_edit_root_identity: String,
    pub edit_id: String,
    pub canonical_edit_identity: String,
    pub operations: Vec<WorkspaceEditProposalOperation>,
    pub artifacts: Vec<WorkspaceEditProposalArtifact>,
    pub overlap_baseline: WorkspaceEditProposalBaseline,
    pub preview: WorkspaceEditProposalPreview,
    pub created_at_ms: u64,
    pub file_mutation_authority: bool,
    pub approval_recorded: bool,
    pub apply_available: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum WorkspaceEditProposalOperation {
    Create {
        path: String,
        content: WorkspaceEditProposalContent,
    },
    Update {
        path: String,
        base: WorkspaceEditProposalHash,
        content: WorkspaceEditProposalContent,
    },
    Delete {
        path: String,
        base: WorkspaceEditProposalHash,
    },
    Rename {
        from_path: String,
        to_path: String,
        base: WorkspaceEditProposalHash,
    },
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct WorkspaceEditProposalHash {
    pub sha256: String,
    pub bytes: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct WorkspaceEditProposalContent {
    pub reference: String,
    pub hash: WorkspaceEditProposalHash,
    pub encoding: String,
    pub newline: String,
    pub mode: String,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(rename_all = "kebab-case")]
pub enum WorkspaceEditProposalArtifactKind {
    ProposedContent,
    Diff,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct WorkspaceEditProposalArtifact {
    pub kind: WorkspaceEditProposalArtifactKind,
    pub reference: String,
    pub sha256: String,
    pub bytes: u64,
    pub media_type: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct WorkspaceEditProposalBaseline {
    pub schema_version: String,
    pub identity: String,
    pub paths: Vec<WorkspaceEditProposalPathExpectation>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct WorkspaceEditProposalPathExpectation {
    pub operation_index: u64,
    pub operation: String,
    pub role: String,
    pub path: String,
    pub expected: WorkspaceEditProposalExpectedState,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "kebab-case", deny_unknown_fields)]
pub enum WorkspaceEditProposalExpectedState {
    Absent,
    File { hash: WorkspaceEditProposalHash },
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct WorkspaceEditProposalPreview {
    pub identity: String,
    pub file_count: u64,
    pub additions: u64,
    pub deletions: u64,
    pub warning_count: u64,
    pub applicable: bool,
    pub aggregate_diff_reference: String,
    pub file_diff_references: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub aggregate_diff: Option<WorkspaceEditProposalDiffSummary>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub files: Option<Vec<WorkspaceEditProposalFileSummary>>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct WorkspaceEditProposalFileSummary {
    pub ordinal: u64,
    pub kind: String,
    pub path: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub from_path: Option<String>,
    pub additions: u64,
    pub deletions: u64,
    pub base_matches: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub proposed_format: Option<WorkspaceEditProposalTextFormat>,
    pub warnings: Vec<WorkspaceEditProposalWarning>,
    pub diff: WorkspaceEditProposalDiffSummary,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct WorkspaceEditProposalTextFormat {
    pub encoding: String,
    pub newline: String,
    pub mode: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct WorkspaceEditProposalWarning {
    pub code: String,
    pub severity: String,
    pub path: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct WorkspaceEditProposalDiffSummary {
    pub reference: String,
    pub sha256: String,
    pub bytes: u64,
    pub media_type: String,
    pub inline_truncated: bool,
    pub source_truncated: bool,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct WorkspaceEditProposalWire {
    schema_version: String,
    proposal_id: String,
    session_id: String,
    turn_id: String,
    provider: Option<String>,
    provider_thread_id: String,
    provider_item_id: String,
    approval_started_at_ms: u64,
    project_id: String,
    root_id: String,
    filesystem_root_identity: String,
    workspace_edit_root_identity: String,
    edit_id: String,
    canonical_edit_identity: String,
    operations: Vec<WorkspaceEditProposalOperation>,
    artifacts: Vec<WorkspaceEditProposalArtifact>,
    overlap_baseline: WorkspaceEditProposalBaseline,
    preview: WorkspaceEditProposalPreviewWire,
    created_at_ms: u64,
    file_mutation_authority: bool,
    approval_recorded: bool,
    apply_available: bool,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct WorkspaceEditProposalPreviewWire {
    identity: String,
    file_count: u64,
    additions: u64,
    deletions: u64,
    warning_count: u64,
    applicable: bool,
    aggregate_diff_reference: String,
    file_diff_references: Vec<String>,
    #[serde(default)]
    aggregate_diff: PresentOption<WorkspaceEditProposalDiffSummary>,
    #[serde(default)]
    files: PresentOption<Vec<WorkspaceEditProposalFileSummary>>,
}

#[derive(Default)]
enum PresentOption<T> {
    #[default]
    Missing,
    Present(Option<T>),
}

impl<'de, T> Deserialize<'de> for PresentOption<T>
where
    T: Deserialize<'de>,
{
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        Option::<T>::deserialize(deserializer).map(Self::Present)
    }
}

impl WorkspaceEditProposalPreviewWire {
    fn into_preview(
        self,
        schema_version: &str,
    ) -> Result<WorkspaceEditProposalPreview, WorkspaceEditProposalError> {
        let (aggregate_diff, files) = match (schema_version, self.aggregate_diff, self.files) {
            (LEGACY_SCHEMA_VERSION, PresentOption::Missing, PresentOption::Missing) => (None, None),
            (
                SCHEMA_VERSION,
                PresentOption::Present(Some(aggregate_diff)),
                PresentOption::Present(Some(files)),
            ) => (Some(aggregate_diff), Some(files)),
            (LEGACY_SCHEMA_VERSION | SCHEMA_VERSION, _, _) => {
                return Err(error(
                    "proposal preview detail presence does not match its schema version",
                ));
            }
            _ => return Err(error("unsupported workspace edit proposal schema")),
        };
        Ok(WorkspaceEditProposalPreview {
            identity: self.identity,
            file_count: self.file_count,
            additions: self.additions,
            deletions: self.deletions,
            warning_count: self.warning_count,
            applicable: self.applicable,
            aggregate_diff_reference: self.aggregate_diff_reference,
            file_diff_references: self.file_diff_references,
            aggregate_diff,
            files,
        })
    }
}

#[derive(Serialize)]
struct CanonicalEditIdentity<'a> {
    schema_version: &'a str,
    edit_id: &'a str,
    project_id: &'a str,
    root_identity: &'a str,
    operations: &'a [WorkspaceEditProposalOperation],
}

#[derive(Serialize)]
struct ProposalIdentity<'a> {
    schema_version: &'a str,
    session_id: &'a str,
    turn_id: &'a str,
    provider: Option<&'a str>,
    provider_thread_id: &'a str,
    provider_item_id: &'a str,
    approval_started_at_ms: u64,
    project_id: &'a str,
    root_id: &'a str,
    filesystem_root_identity: &'a str,
    workspace_edit_root_identity: &'a str,
    edit_id: &'a str,
    canonical_edit_identity: &'a str,
    operations: &'a [WorkspaceEditProposalOperation],
    artifacts: &'a [WorkspaceEditProposalArtifact],
    overlap_baseline: &'a WorkspaceEditProposalBaseline,
    preview: &'a WorkspaceEditProposalPreview,
    created_at_ms: u64,
    file_mutation_authority: bool,
    approval_recorded: bool,
    apply_available: bool,
}

#[derive(Serialize)]
struct PreviewIdentity<'a> {
    file_count: u64,
    additions: u64,
    deletions: u64,
    warning_count: u64,
    applicable: bool,
    aggregate_diff_reference: &'a str,
    file_diff_references: &'a [String],
    #[serde(skip_serializing_if = "Option::is_none")]
    aggregate_diff: Option<&'a WorkspaceEditProposalDiffSummary>,
    #[serde(skip_serializing_if = "Option::is_none")]
    files: Option<&'a [WorkspaceEditProposalFileSummary]>,
}

impl WorkspaceEditProposal {
    #[allow(clippy::too_many_arguments)]
    pub fn from_preview(
        session_id: impl Into<String>,
        turn_id: impl Into<String>,
        provider: Option<String>,
        provider_thread_id: impl Into<String>,
        provider_item_id: impl Into<String>,
        approval_started_at_ms: u64,
        root_id: impl Into<String>,
        filesystem_root_identity: impl Into<String>,
        edit: &WorkspaceEdit,
        preview: &WorkspaceEditPreview,
        mut artifacts: Vec<WorkspaceEditProposalArtifact>,
        created_at_ms: u64,
    ) -> Result<Self, WorkspaceEditProposalError> {
        let rebuilt = WorkspaceEdit::define(
            edit.edit_id.clone(),
            edit.project_id.clone(),
            &edit.root.canonical_path,
            edit.operations.clone(),
        )
        .map_err(|cause| error(cause.message))?;
        if rebuilt != *edit {
            return Err(error("workspace edit changed after validation"));
        }
        validate_preview(edit, preview)?;
        let operations = edit
            .operations
            .iter()
            .map(WorkspaceEditProposalOperation::from)
            .collect::<Vec<_>>();
        artifacts.sort_by(|left, right| {
            left.kind
                .cmp(&right.kind)
                .then_with(|| left.reference.cmp(&right.reference))
        });
        let canonical_edit_identity = prefixed_identity(
            "workspace-edit-canonical",
            &CanonicalEditIdentity {
                schema_version: &edit.schema_version,
                edit_id: &edit.edit_id,
                project_id: &edit.project_id,
                root_identity: &edit.root.identity,
                operations: &operations,
            },
        )?;
        let overlap_baseline = proposal_baseline(edit, &operations)?;
        let aggregate_diff = proposal_diff_summary(&preview.aggregate_diff)?;
        let files = preview
            .files
            .iter()
            .enumerate()
            .map(|(ordinal, file)| {
                Ok(WorkspaceEditProposalFileSummary {
                    ordinal: to_u64(ordinal, "proposal file ordinal")?,
                    kind: file.kind.clone(),
                    path: file.path.clone(),
                    from_path: file.from_path.clone(),
                    additions: to_u64(file.additions, "proposal file additions")?,
                    deletions: to_u64(file.deletions, "proposal file deletions")?,
                    base_matches: file.base_matches,
                    proposed_format: file
                        .proposed_format
                        .as_ref()
                        .map(WorkspaceEditProposalTextFormat::from),
                    warnings: file
                        .warnings
                        .iter()
                        .map(|warning| WorkspaceEditProposalWarning {
                            code: warning.code.clone(),
                            severity: warning.severity.clone(),
                            path: warning.path.clone(),
                        })
                        .collect(),
                    diff: proposal_diff_summary(&file.diff)?,
                })
            })
            .collect::<Result<Vec<_>, WorkspaceEditProposalError>>()?;
        let mut preview = WorkspaceEditProposalPreview {
            identity: String::new(),
            file_count: to_u64(preview.files.len(), "preview file count")?,
            additions: to_u64(preview.additions, "preview additions")?,
            deletions: to_u64(preview.deletions, "preview deletions")?,
            warning_count: to_u64(preview.warning_count, "preview warning count")?,
            applicable: preview.applicable,
            aggregate_diff_reference: preview.aggregate_diff.reference.clone(),
            file_diff_references: preview
                .files
                .iter()
                .map(|file| file.diff.reference.clone())
                .collect(),
            aggregate_diff: Some(aggregate_diff),
            files: Some(files),
        };
        preview.identity = preview_identity(&preview)?;
        let mut proposal = Self {
            schema_version: SCHEMA_VERSION.into(),
            proposal_id: String::new(),
            session_id: session_id.into(),
            turn_id: turn_id.into(),
            provider,
            provider_thread_id: provider_thread_id.into(),
            provider_item_id: provider_item_id.into(),
            approval_started_at_ms,
            project_id: edit.project_id.clone(),
            root_id: root_id.into(),
            filesystem_root_identity: filesystem_root_identity.into(),
            workspace_edit_root_identity: edit.root.identity.clone(),
            edit_id: edit.edit_id.clone(),
            canonical_edit_identity,
            operations,
            artifacts,
            overlap_baseline,
            preview,
            created_at_ms,
            file_mutation_authority: false,
            approval_recorded: false,
            apply_available: false,
        };
        proposal.proposal_id = proposal.expected_proposal_id()?;
        proposal.validate()?;
        Ok(proposal)
    }

    pub fn validate(&self) -> Result<(), WorkspaceEditProposalError> {
        if !matches!(
            self.schema_version.as_str(),
            SCHEMA_VERSION | LEGACY_SCHEMA_VERSION
        ) {
            return Err(error("unsupported workspace edit proposal schema"));
        }
        validate_identifier(&self.session_id, "proposal session ID")?;
        validate_identifier(&self.turn_id, "proposal turn ID")?;
        if let Some(provider) = self.provider.as_deref() {
            validate_identifier(provider, "proposal provider")?;
        }
        validate_identifier(&self.provider_thread_id, "proposal provider thread ID")?;
        validate_identifier(&self.provider_item_id, "proposal provider item ID")?;
        validate_identifier(&self.project_id, "proposal project ID")?;
        validate_identifier(&self.root_id, "proposal root ID")?;
        validate_identifier(&self.edit_id, "proposal edit ID")?;
        validate_identity(
            &self.filesystem_root_identity,
            "proposal filesystem root identity",
        )?;
        validate_prefixed_sha256(
            &self.workspace_edit_root_identity,
            "workspace-root:sha256:",
            "proposal WorkspaceEdit root identity",
        )?;
        validate_prefixed_sha256(
            &self.canonical_edit_identity,
            "workspace-edit-canonical:sha256:",
            "canonical edit identity",
        )?;
        if self.approval_started_at_ms == 0 || self.created_at_ms < self.approval_started_at_ms {
            return Err(error("proposal lifecycle timestamps are invalid"));
        }
        if self.file_mutation_authority || self.approval_recorded || self.apply_available {
            return Err(error(
                "read-only proposal cannot contain mutation authority",
            ));
        }
        validate_operations(&self.operations)?;
        let expected_edit_identity = prefixed_identity(
            "workspace-edit-canonical",
            &CanonicalEditIdentity {
                schema_version: "workspace-edit/0.2",
                edit_id: &self.edit_id,
                project_id: &self.project_id,
                root_identity: &self.workspace_edit_root_identity,
                operations: &self.operations,
            },
        )?;
        if self.canonical_edit_identity != expected_edit_identity {
            return Err(error("canonical edit identity does not match operations"));
        }
        validate_artifacts(self)?;
        validate_baseline(self)?;
        validate_preview_summary(self)?;
        let expected_id = self.expected_proposal_id()?;
        if self.proposal_id != expected_id {
            return Err(error("workspace edit proposal identity is invalid"));
        }
        Ok(())
    }

    fn expected_proposal_id(&self) -> Result<String, WorkspaceEditProposalError> {
        prefixed_identity(
            "workspace-edit-proposal",
            &ProposalIdentity {
                schema_version: &self.schema_version,
                session_id: &self.session_id,
                turn_id: &self.turn_id,
                provider: self.provider.as_deref(),
                provider_thread_id: &self.provider_thread_id,
                provider_item_id: &self.provider_item_id,
                approval_started_at_ms: self.approval_started_at_ms,
                project_id: &self.project_id,
                root_id: &self.root_id,
                filesystem_root_identity: &self.filesystem_root_identity,
                workspace_edit_root_identity: &self.workspace_edit_root_identity,
                edit_id: &self.edit_id,
                canonical_edit_identity: &self.canonical_edit_identity,
                operations: &self.operations,
                artifacts: &self.artifacts,
                overlap_baseline: &self.overlap_baseline,
                preview: &self.preview,
                created_at_ms: self.created_at_ms,
                file_mutation_authority: self.file_mutation_authority,
                approval_recorded: self.approval_recorded,
                apply_available: self.apply_available,
            },
        )
    }

    pub fn provider_thread_identity(&self) -> String {
        provider_identity("codex-provider-thread", &self.provider_thread_id)
    }

    pub fn provider_identity(&self) -> Option<String> {
        self.provider
            .as_deref()
            .map(|provider| provider_identity("codex-provider", provider))
    }

    pub fn provider_item_identity(&self) -> String {
        provider_identity("codex-file-change-item", &self.provider_item_id)
    }
}

impl<'de> Deserialize<'de> for WorkspaceEditProposal {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let wire = WorkspaceEditProposalWire::deserialize(deserializer)?;
        let preview = wire
            .preview
            .into_preview(&wire.schema_version)
            .map_err(|cause| serde::de::Error::custom(cause.message))?;
        let proposal = Self {
            schema_version: wire.schema_version,
            proposal_id: wire.proposal_id,
            session_id: wire.session_id,
            turn_id: wire.turn_id,
            provider: wire.provider,
            provider_thread_id: wire.provider_thread_id,
            provider_item_id: wire.provider_item_id,
            approval_started_at_ms: wire.approval_started_at_ms,
            project_id: wire.project_id,
            root_id: wire.root_id,
            filesystem_root_identity: wire.filesystem_root_identity,
            workspace_edit_root_identity: wire.workspace_edit_root_identity,
            edit_id: wire.edit_id,
            canonical_edit_identity: wire.canonical_edit_identity,
            operations: wire.operations,
            artifacts: wire.artifacts,
            overlap_baseline: wire.overlap_baseline,
            preview,
            created_at_ms: wire.created_at_ms,
            file_mutation_authority: wire.file_mutation_authority,
            approval_recorded: wire.approval_recorded,
            apply_available: wire.apply_available,
        };
        proposal
            .validate()
            .map_err(|cause| serde::de::Error::custom(cause.message))?;
        Ok(proposal)
    }
}

impl WorkspaceEditProposalArtifact {
    pub fn from_snapshot(
        kind: WorkspaceEditProposalArtifactKind,
        snapshot: &PreviewArtifactSnapshot,
    ) -> Result<Self, WorkspaceEditProposalError> {
        Self::for_bytes(
            kind,
            snapshot.reference.clone(),
            snapshot.media_type.clone(),
            &snapshot.bytes,
        )
    }

    pub fn for_bytes(
        kind: WorkspaceEditProposalArtifactKind,
        reference: impl Into<String>,
        media_type: impl Into<String>,
        bytes: &[u8],
    ) -> Result<Self, WorkspaceEditProposalError> {
        let artifact = Self {
            kind,
            reference: reference.into(),
            sha256: format!("{:x}", Sha256::digest(bytes)),
            bytes: bytes.len() as u64,
            media_type: media_type.into(),
        };
        validate_artifact(&artifact)?;
        Ok(artifact)
    }
}

impl From<&ContentHash> for WorkspaceEditProposalHash {
    fn from(value: &ContentHash) -> Self {
        Self {
            sha256: value.sha256.clone(),
            bytes: value.bytes,
        }
    }
}

impl From<&ProposedContent> for WorkspaceEditProposalContent {
    fn from(value: &ProposedContent) -> Self {
        Self {
            reference: value.reference.clone(),
            hash: (&value.hash).into(),
            encoding: value.format.encoding.clone(),
            newline: value.format.newline.clone(),
            mode: value.format.mode.clone(),
        }
    }
}

impl From<&ProposedTextFormat> for WorkspaceEditProposalTextFormat {
    fn from(value: &ProposedTextFormat) -> Self {
        Self {
            encoding: value.encoding.clone(),
            newline: value.newline.clone(),
            mode: value.mode.clone(),
        }
    }
}

impl WorkspaceEditProposalTextFormat {
    fn from_content(content: &WorkspaceEditProposalContent) -> Self {
        Self {
            encoding: content.encoding.clone(),
            newline: content.newline.clone(),
            mode: content.mode.clone(),
        }
    }
}

impl From<&WorkspaceEditOperation> for WorkspaceEditProposalOperation {
    fn from(value: &WorkspaceEditOperation) -> Self {
        match value {
            WorkspaceEditOperation::Create { path, content } => Self::Create {
                path: path.clone(),
                content: content.into(),
            },
            WorkspaceEditOperation::Update {
                path,
                base,
                content,
            } => Self::Update {
                path: path.clone(),
                base: base.into(),
                content: content.into(),
            },
            WorkspaceEditOperation::Delete { path, base } => Self::Delete {
                path: path.clone(),
                base: base.into(),
            },
            WorkspaceEditOperation::Rename {
                from_path,
                to_path,
                base,
            } => Self::Rename {
                from_path: from_path.clone(),
                to_path: to_path.clone(),
                base: base.into(),
            },
        }
    }
}

fn validate_operations(
    operations: &[WorkspaceEditProposalOperation],
) -> Result<(), WorkspaceEditProposalError> {
    if operations.is_empty() || operations.len() > MAX_OPERATIONS {
        return Err(error("proposal operation count is invalid"));
    }
    let mut paths = BTreeSet::new();
    for operation in operations {
        match operation {
            WorkspaceEditProposalOperation::Create { path, content } => {
                validate_and_claim_path(path, &mut paths)?;
                validate_content(content, true)?;
            }
            WorkspaceEditProposalOperation::Update {
                path,
                base,
                content,
            } => {
                validate_and_claim_path(path, &mut paths)?;
                validate_hash(base, "proposal base hash")?;
                validate_content(content, false)?;
            }
            WorkspaceEditProposalOperation::Delete { path, base } => {
                validate_and_claim_path(path, &mut paths)?;
                validate_hash(base, "proposal base hash")?;
            }
            WorkspaceEditProposalOperation::Rename {
                from_path,
                to_path,
                base,
            } => {
                if from_path == to_path {
                    return Err(error("proposal rename source and target must differ"));
                }
                validate_and_claim_path(from_path, &mut paths)?;
                validate_and_claim_path(to_path, &mut paths)?;
                validate_hash(base, "proposal rename base hash")?;
            }
        }
    }
    Ok(())
}

fn validate_artifacts(proposal: &WorkspaceEditProposal) -> Result<(), WorkspaceEditProposalError> {
    if proposal.artifacts.is_empty() || proposal.artifacts.len() > MAX_ARTIFACTS {
        return Err(error("proposal artifact count is invalid"));
    }
    let mut previous: Option<(WorkspaceEditProposalArtifactKind, &str)> = None;
    let mut actual_content = BTreeSet::new();
    let mut actual_diffs = BTreeSet::new();
    for artifact in &proposal.artifacts {
        validate_artifact(artifact)?;
        let current = (artifact.kind, artifact.reference.as_str());
        if previous.is_some_and(|previous| previous >= current) {
            return Err(error("proposal artifacts are duplicated or not canonical"));
        }
        previous = Some(current);
        match artifact.kind {
            WorkspaceEditProposalArtifactKind::ProposedContent => {
                actual_content.insert(artifact.reference.as_str());
            }
            WorkspaceEditProposalArtifactKind::Diff => {
                actual_diffs.insert(artifact.reference.as_str());
            }
        }
    }
    let expected_content = proposal
        .operations
        .iter()
        .filter_map(|operation| match operation {
            WorkspaceEditProposalOperation::Create { content, .. }
            | WorkspaceEditProposalOperation::Update { content, .. } => {
                Some(content.reference.as_str())
            }
            _ => None,
        })
        .collect::<BTreeSet<_>>();
    let mut expected_diffs = proposal
        .preview
        .file_diff_references
        .iter()
        .map(String::as_str)
        .collect::<BTreeSet<_>>();
    expected_diffs.insert(&proposal.preview.aggregate_diff_reference);
    if actual_content != expected_content || actual_diffs != expected_diffs {
        return Err(error(
            "proposal artifacts do not cover the exact content and diff references",
        ));
    }
    Ok(())
}

fn validate_artifact(
    artifact: &WorkspaceEditProposalArtifact,
) -> Result<(), WorkspaceEditProposalError> {
    validate_lower_sha256(&artifact.sha256, "proposal artifact hash")?;
    let (prefix, media_type) = match artifact.kind {
        WorkspaceEditProposalArtifactKind::ProposedContent => {
            (CONTENT_REFERENCE_PREFIX, "text/plain; charset=utf-8")
        }
        WorkspaceEditProposalArtifactKind::Diff => {
            (DIFF_REFERENCE_PREFIX, "text/x-diff; charset=utf-8")
        }
    };
    validate_prefixed_sha256(&artifact.reference, prefix, "proposal artifact reference")?;
    if artifact.reference.strip_prefix(prefix) != Some(artifact.sha256.as_str())
        || artifact.media_type != media_type
    {
        return Err(error(
            "proposal artifact metadata does not match its identity",
        ));
    }
    Ok(())
}

fn validate_content(
    content: &WorkspaceEditProposalContent,
    create: bool,
) -> Result<(), WorkspaceEditProposalError> {
    validate_hash(&content.hash, "proposal content hash")?;
    if content.hash.bytes > MAX_CONTENT_BYTES {
        return Err(error("proposal content exceeds the per-file limit"));
    }
    validate_prefixed_sha256(
        &content.reference,
        CONTENT_REFERENCE_PREFIX,
        "proposal content reference",
    )?;
    if content.reference.strip_prefix(CONTENT_REFERENCE_PREFIX)
        != Some(content.hash.sha256.as_str())
        || !matches!(content.encoding.as_str(), "utf-8" | "utf-8-bom")
        || !matches!(content.newline.as_str(), "none" | "lf" | "crlf")
        || !matches!(content.mode.as_str(), "preserve" | "regular" | "executable")
        || (create && content.mode == "preserve")
    {
        return Err(error("proposal content descriptor is invalid"));
    }
    Ok(())
}

fn proposal_baseline(
    edit: &WorkspaceEdit,
    operations: &[WorkspaceEditProposalOperation],
) -> Result<WorkspaceEditProposalBaseline, WorkspaceEditProposalError> {
    let baseline = proposal_overlap_baseline(edit).map_err(|cause| error(cause.message))?;
    if baseline.phase != WorkspaceEditOverlapPhase::BeforeApply {
        return Err(error("proposal overlap baseline phase is invalid"));
    }
    let mut paths = Vec::with_capacity(baseline.paths.len());
    let mut cursor = 0_usize;
    for (operation_index, operation) in operations.iter().enumerate() {
        let expected_count = usize::from(matches!(
            operation,
            WorkspaceEditProposalOperation::Rename { .. }
        )) + 1;
        for expectation in baseline.paths.iter().skip(cursor).take(expected_count) {
            paths.push(WorkspaceEditProposalPathExpectation {
                operation_index: operation_index as u64,
                operation: expectation.operation.clone(),
                role: expectation.role.clone(),
                path: expectation.path.clone(),
                expected: match &expectation.expected {
                    ExpectedWorkspacePathState::Absent => {
                        WorkspaceEditProposalExpectedState::Absent
                    }
                    ExpectedWorkspacePathState::File { hash } => {
                        WorkspaceEditProposalExpectedState::File { hash: hash.into() }
                    }
                },
            });
        }
        cursor = cursor.saturating_add(expected_count);
    }
    if cursor != baseline.paths.len() {
        return Err(error("proposal overlap baseline is incomplete"));
    }
    let mut result = WorkspaceEditProposalBaseline {
        schema_version: BASELINE_SCHEMA_VERSION.into(),
        identity: String::new(),
        paths,
    };
    result.identity = baseline_identity(&result)?;
    Ok(result)
}

fn validate_baseline(proposal: &WorkspaceEditProposal) -> Result<(), WorkspaceEditProposalError> {
    if proposal.overlap_baseline.schema_version != BASELINE_SCHEMA_VERSION
        || proposal.overlap_baseline.identity != baseline_identity(&proposal.overlap_baseline)?
    {
        return Err(error("proposal overlap baseline identity is invalid"));
    }
    let expected = expected_baseline_paths(&proposal.operations);
    if proposal.overlap_baseline.paths != expected {
        return Err(error("proposal overlap baseline does not match operations"));
    }
    Ok(())
}

fn expected_baseline_paths(
    operations: &[WorkspaceEditProposalOperation],
) -> Vec<WorkspaceEditProposalPathExpectation> {
    let mut paths = Vec::new();
    for (index, operation) in operations.iter().enumerate() {
        let operation_index = index as u64;
        match operation {
            WorkspaceEditProposalOperation::Create { path, .. } => paths.push(path_expectation(
                operation_index,
                "create",
                "target",
                path,
                None,
            )),
            WorkspaceEditProposalOperation::Update { path, base, .. } => paths.push(
                path_expectation(operation_index, "update", "target", path, Some(base)),
            ),
            WorkspaceEditProposalOperation::Delete { path, base } => paths.push(path_expectation(
                operation_index,
                "delete",
                "target",
                path,
                Some(base),
            )),
            WorkspaceEditProposalOperation::Rename {
                from_path,
                to_path,
                base,
            } => {
                paths.push(path_expectation(
                    operation_index,
                    "rename",
                    "source",
                    from_path,
                    Some(base),
                ));
                paths.push(path_expectation(
                    operation_index,
                    "rename",
                    "target",
                    to_path,
                    None,
                ));
            }
        }
    }
    paths
}

fn path_expectation(
    operation_index: u64,
    operation: &str,
    role: &str,
    path: &str,
    hash: Option<&WorkspaceEditProposalHash>,
) -> WorkspaceEditProposalPathExpectation {
    WorkspaceEditProposalPathExpectation {
        operation_index,
        operation: operation.into(),
        role: role.into(),
        path: path.into(),
        expected: hash.map_or(WorkspaceEditProposalExpectedState::Absent, |hash| {
            WorkspaceEditProposalExpectedState::File { hash: hash.clone() }
        }),
    }
}

fn baseline_identity(
    baseline: &WorkspaceEditProposalBaseline,
) -> Result<String, WorkspaceEditProposalError> {
    #[derive(Serialize)]
    struct Identity<'a> {
        schema_version: &'a str,
        paths: &'a [WorkspaceEditProposalPathExpectation],
    }
    prefixed_identity(
        "workspace-edit-proposal-baseline",
        &Identity {
            schema_version: &baseline.schema_version,
            paths: &baseline.paths,
        },
    )
}

fn validate_preview_summary(
    proposal: &WorkspaceEditProposal,
) -> Result<(), WorkspaceEditProposalError> {
    validate_prefixed_sha256(
        &proposal.preview.identity,
        "workspace-edit-preview:sha256:",
        "proposal preview identity",
    )?;
    validate_prefixed_sha256(
        &proposal.preview.aggregate_diff_reference,
        DIFF_REFERENCE_PREFIX,
        "aggregate diff reference",
    )?;
    if proposal.preview.file_count != proposal.operations.len() as u64
        || proposal.preview.file_diff_references.len() != proposal.operations.len()
        || proposal
            .preview
            .file_diff_references
            .iter()
            .any(|reference| {
                validate_prefixed_sha256(reference, DIFF_REFERENCE_PREFIX, "file diff reference")
                    .is_err()
            })
    {
        return Err(error("proposal preview summary is invalid"));
    }
    match (
        proposal.schema_version.as_str(),
        proposal.preview.aggregate_diff.as_ref(),
        proposal.preview.files.as_deref(),
    ) {
        (LEGACY_SCHEMA_VERSION, None, None) => {}
        (SCHEMA_VERSION, Some(aggregate), Some(files)) => {
            validate_complete_preview_summary(proposal, aggregate, files)?;
        }
        (LEGACY_SCHEMA_VERSION | SCHEMA_VERSION, _, _) => {
            return Err(error(
                "proposal preview detail does not match its schema version",
            ));
        }
        _ => return Err(error("unsupported workspace edit proposal schema")),
    }
    if proposal.preview.identity != preview_identity(&proposal.preview)? {
        return Err(error(
            "proposal preview identity does not match its summary",
        ));
    }
    Ok(())
}

fn preview_identity(
    preview: &WorkspaceEditProposalPreview,
) -> Result<String, WorkspaceEditProposalError> {
    prefixed_identity(
        "workspace-edit-preview",
        &PreviewIdentity {
            file_count: preview.file_count,
            additions: preview.additions,
            deletions: preview.deletions,
            warning_count: preview.warning_count,
            applicable: preview.applicable,
            aggregate_diff_reference: &preview.aggregate_diff_reference,
            file_diff_references: &preview.file_diff_references,
            aggregate_diff: preview.aggregate_diff.as_ref(),
            files: preview.files.as_deref(),
        },
    )
}

fn validate_complete_preview_summary(
    proposal: &WorkspaceEditProposal,
    aggregate: &WorkspaceEditProposalDiffSummary,
    files: &[WorkspaceEditProposalFileSummary],
) -> Result<(), WorkspaceEditProposalError> {
    if files.len() != proposal.operations.len()
        || aggregate.reference != proposal.preview.aggregate_diff_reference
    {
        return Err(error("proposal preview detail is incomplete"));
    }
    validate_diff_summary(
        proposal,
        aggregate,
        MAX_AGGREGATE_DIFF_BYTES,
        MAX_INLINE_AGGREGATE_DIFF_BYTES,
        "aggregate diff",
    )?;

    let mut additions = 0_u64;
    let mut deletions = 0_u64;
    let mut warning_count = 0_u64;
    for (index, (operation, file)) in proposal.operations.iter().zip(files).enumerate() {
        if file.ordinal != index as u64
            || proposal.preview.file_diff_references[index] != file.diff.reference
        {
            return Err(error("proposal file summary order is invalid"));
        }
        validate_file_summary(proposal, operation, file)?;
        additions = additions
            .checked_add(file.additions)
            .ok_or_else(|| error("proposal additions overflow"))?;
        deletions = deletions
            .checked_add(file.deletions)
            .ok_or_else(|| error("proposal deletions overflow"))?;
        warning_count = warning_count
            .checked_add(to_u64(file.warnings.len(), "proposal warning count")?)
            .ok_or_else(|| error("proposal warning count overflow"))?;
    }
    let applicable = files.iter().all(|file| {
        file.warnings
            .iter()
            .all(|warning| warning.severity != "blocking")
    });
    if additions != proposal.preview.additions
        || deletions != proposal.preview.deletions
        || warning_count != proposal.preview.warning_count
        || applicable != proposal.preview.applicable
        || (files.iter().any(|file| file.diff.source_truncated) && !aggregate.source_truncated)
    {
        return Err(error(
            "proposal preview aggregates do not match file summaries",
        ));
    }
    Ok(())
}

fn validate_file_summary(
    proposal: &WorkspaceEditProposal,
    operation: &WorkspaceEditProposalOperation,
    file: &WorkspaceEditProposalFileSummary,
) -> Result<(), WorkspaceEditProposalError> {
    validate_path(&file.path)?;
    if let Some(from_path) = file.from_path.as_deref() {
        validate_path(from_path)?;
    }
    let (kind, path, from_path, proposed_format, base_path) = match operation {
        WorkspaceEditProposalOperation::Create { path, content } => (
            "create",
            path.as_str(),
            None,
            Some(WorkspaceEditProposalTextFormat::from_content(content)),
            None,
        ),
        WorkspaceEditProposalOperation::Update { path, content, .. } => (
            "update",
            path.as_str(),
            None,
            Some(WorkspaceEditProposalTextFormat::from_content(content)),
            Some(path.as_str()),
        ),
        WorkspaceEditProposalOperation::Delete { path, .. } => {
            ("delete", path.as_str(), None, None, Some(path.as_str()))
        }
        WorkspaceEditProposalOperation::Rename {
            from_path, to_path, ..
        } => (
            "rename",
            to_path.as_str(),
            Some(from_path.as_str()),
            None,
            Some(from_path.as_str()),
        ),
    };
    if file.kind != kind
        || file.path != path
        || file.from_path.as_deref() != from_path
        || file.proposed_format != proposed_format
        || (kind == "create" && file.base_matches.is_some())
        || (kind == "rename"
            && (file.additions != 0 || file.deletions != 0 || file.diff.source_truncated))
    {
        return Err(error("proposal file summary does not match its operation"));
    }
    if let Some(format) = file.proposed_format.as_ref() {
        validate_text_format(format)?;
    }
    validate_diff_summary(
        proposal,
        &file.diff,
        MAX_FILE_DIFF_BYTES,
        MAX_INLINE_FILE_DIFF_BYTES,
        "file diff",
    )?;
    validate_warnings(file, base_path)?;
    Ok(())
}

fn validate_warnings(
    file: &WorkspaceEditProposalFileSummary,
    base_path: Option<&str>,
) -> Result<(), WorkspaceEditProposalError> {
    let mut warnings = BTreeSet::new();
    for warning in &file.warnings {
        if !matches!(
            warning.code.as_str(),
            "target-exists"
                | "mixed-line-endings"
                | "stale-base"
                | "base-unavailable"
                | "sensitive-path"
                | "ignored-path"
                | "unsafe-path"
        ) || warning.severity != "blocking"
            || (warning.path != file.path
                && file.from_path.as_deref() != Some(warning.path.as_str()))
            || !warnings.insert((warning.code.as_str(), warning.path.as_str()))
        {
            return Err(error("proposal warning summary is invalid"));
        }
    }
    if let Some(base_path) = base_path {
        let stale = file
            .warnings
            .iter()
            .any(|warning| warning.code == "stale-base" && warning.path == base_path);
        let unavailable = file.warnings.iter().any(|warning| {
            warning.path == base_path
                && matches!(
                    warning.code.as_str(),
                    "base-unavailable" | "sensitive-path" | "ignored-path" | "unsafe-path"
                )
        });
        if matches!(file.base_matches, Some(false)) != stale
            || (file.base_matches.is_none() && !unavailable)
            || (file.base_matches.is_some() && unavailable)
        {
            return Err(error("proposal base-match summary is invalid"));
        }
    }
    Ok(())
}

fn validate_diff_summary(
    proposal: &WorkspaceEditProposal,
    diff: &WorkspaceEditProposalDiffSummary,
    max_bytes: u64,
    inline_limit: u64,
    label: &str,
) -> Result<(), WorkspaceEditProposalError> {
    validate_prefixed_sha256(&diff.reference, DIFF_REFERENCE_PREFIX, label)?;
    validate_lower_sha256(&diff.sha256, label)?;
    if diff.reference.strip_prefix(DIFF_REFERENCE_PREFIX) != Some(diff.sha256.as_str())
        || diff.bytes > max_bytes
        || diff.media_type != "text/x-diff; charset=utf-8"
        || diff.inline_truncated != (diff.bytes > inline_limit)
    {
        return Err(error(format!("{label} summary is invalid")));
    }
    let artifact = proposal
        .artifacts
        .iter()
        .find(|artifact| artifact.reference == diff.reference)
        .ok_or_else(|| error(format!("{label} artifact is missing")))?;
    if artifact.kind != WorkspaceEditProposalArtifactKind::Diff
        || artifact.sha256 != diff.sha256
        || artifact.bytes != diff.bytes
        || artifact.media_type != diff.media_type
    {
        return Err(error(format!(
            "{label} artifact does not match its summary"
        )));
    }
    Ok(())
}

fn validate_preview(
    edit: &WorkspaceEdit,
    preview: &WorkspaceEditPreview,
) -> Result<(), WorkspaceEditProposalError> {
    if preview.edit_id != edit.edit_id
        || preview.project_id != edit.project_id
        || preview.root_identity != edit.root.identity
        || preview.files.len() != edit.operations.len()
        || preview.additions
            != preview
                .files
                .iter()
                .map(|file| file.additions)
                .sum::<usize>()
        || preview.deletions
            != preview
                .files
                .iter()
                .map(|file| file.deletions)
                .sum::<usize>()
        || preview.warning_count
            != preview
                .files
                .iter()
                .map(|file| file.warnings.len())
                .sum::<usize>()
        || preview.applicable
            != preview.files.iter().all(|file| {
                file.warnings
                    .iter()
                    .all(|warning| warning.severity != "blocking")
            })
    {
        return Err(error("workspace edit preview does not match the edit"));
    }
    validate_preview_descriptor(
        &preview.aggregate_diff,
        MAX_AGGREGATE_DIFF_BYTES,
        MAX_INLINE_AGGREGATE_DIFF_BYTES,
        "aggregate diff",
    )?;
    for (operation, file) in edit.operations.iter().zip(&preview.files) {
        let matches = match operation {
            WorkspaceEditOperation::Create { path, .. } => {
                file.kind == "create" && file.path == *path && file.from_path.is_none()
            }
            WorkspaceEditOperation::Update { path, .. } => {
                file.kind == "update" && file.path == *path && file.from_path.is_none()
            }
            WorkspaceEditOperation::Delete { path, .. } => {
                file.kind == "delete" && file.path == *path && file.from_path.is_none()
            }
            WorkspaceEditOperation::Rename {
                from_path, to_path, ..
            } => {
                file.kind == "rename"
                    && file.path == *to_path
                    && file.from_path.as_deref() == Some(from_path)
            }
        };
        if !matches {
            return Err(error("workspace edit preview file binding is invalid"));
        }
        validate_preview_descriptor(
            &file.diff,
            MAX_FILE_DIFF_BYTES,
            MAX_INLINE_FILE_DIFF_BYTES,
            "file diff",
        )?;
        if file.warnings.iter().any(|warning| {
            warning.message.is_empty()
                || warning.message.len() > MAX_PATH_BYTES
                || warning.message.chars().any(char::is_control)
        }) {
            return Err(error("workspace edit preview warning is invalid"));
        }
    }
    if preview.files.iter().any(|file| file.diff.source_truncated)
        && !preview.aggregate_diff.source_truncated
    {
        return Err(error("workspace edit aggregate truncation is invalid"));
    }
    Ok(())
}

fn proposal_diff_summary(
    descriptor: &PreviewArtifactDescriptor,
) -> Result<WorkspaceEditProposalDiffSummary, WorkspaceEditProposalError> {
    let sha256 = descriptor
        .reference
        .strip_prefix(DIFF_REFERENCE_PREFIX)
        .ok_or_else(|| error("workspace edit preview diff reference is invalid"))?
        .to_owned();
    validate_lower_sha256(&sha256, "workspace edit preview diff hash")?;
    Ok(WorkspaceEditProposalDiffSummary {
        reference: descriptor.reference.clone(),
        sha256,
        bytes: to_u64(descriptor.bytes, "workspace edit preview diff bytes")?,
        media_type: descriptor.media_type.clone(),
        inline_truncated: descriptor.inline_truncated,
        source_truncated: descriptor.source_truncated,
    })
}

fn validate_preview_descriptor(
    descriptor: &PreviewArtifactDescriptor,
    max_bytes: u64,
    inline_limit: u64,
    label: &str,
) -> Result<(), WorkspaceEditProposalError> {
    validate_prefixed_sha256(&descriptor.reference, DIFF_REFERENCE_PREFIX, label)?;
    let bytes = to_u64(descriptor.bytes, label)?;
    if bytes > max_bytes
        || descriptor.media_type != "text/x-diff; charset=utf-8"
        || descriptor.inline.len() > inline_limit as usize
        || descriptor.inline_truncated != (bytes > inline_limit)
    {
        return Err(error(format!("workspace edit preview {label} is invalid")));
    }
    Ok(())
}

fn validate_text_format(
    format: &WorkspaceEditProposalTextFormat,
) -> Result<(), WorkspaceEditProposalError> {
    if !matches!(format.encoding.as_str(), "utf-8" | "utf-8-bom")
        || !matches!(format.newline.as_str(), "none" | "lf" | "crlf")
        || !matches!(format.mode.as_str(), "preserve" | "regular" | "executable")
    {
        return Err(error("proposal text format is invalid"));
    }
    Ok(())
}

fn validate_and_claim_path<'a>(
    value: &'a str,
    claimed: &mut BTreeSet<&'a str>,
) -> Result<(), WorkspaceEditProposalError> {
    validate_path(value)?;
    if !claimed.insert(value) {
        return Err(error("proposal operations contain a duplicate path"));
    }
    Ok(())
}

fn validate_path(value: &str) -> Result<(), WorkspaceEditProposalError> {
    if value.is_empty()
        || value.len() > MAX_PATH_BYTES
        || value.contains('\\')
        || value.chars().any(char::is_control)
    {
        return Err(error("proposal path is invalid"));
    }
    let path = Path::new(value);
    if path.is_absolute() {
        return Err(error("proposal path must be root-relative"));
    }
    let mut segments = 0_usize;
    for component in path.components() {
        match component {
            Component::Normal(segment) if segment.to_str().is_some() => segments += 1,
            Component::CurDir => {
                return Err(error("proposal path is not normalized"));
            }
            _ => return Err(error("proposal path escapes the workspace root")),
        }
    }
    if segments == 0
        || value
            .split('/')
            .any(|segment| segment.is_empty() || segment == ".")
    {
        return Err(error("proposal path is not normalized"));
    }
    Ok(())
}

fn validate_hash(
    hash: &WorkspaceEditProposalHash,
    label: &str,
) -> Result<(), WorkspaceEditProposalError> {
    validate_lower_sha256(&hash.sha256, label)
}

fn validate_identifier(value: &str, label: &str) -> Result<(), WorkspaceEditProposalError> {
    if value.is_empty()
        || value.len() > MAX_IDENTIFIER_BYTES
        || value.trim() != value
        || value.chars().any(char::is_control)
    {
        return Err(error(format!("{label} is invalid")));
    }
    Ok(())
}

fn validate_identity(value: &str, label: &str) -> Result<(), WorkspaceEditProposalError> {
    if value.is_empty()
        || value.len() > MAX_IDENTITY_BYTES
        || value.trim() != value
        || value.chars().any(char::is_control)
    {
        return Err(error(format!("{label} is invalid")));
    }
    Ok(())
}

fn validate_prefixed_sha256(
    value: &str,
    prefix: &str,
    label: &str,
) -> Result<(), WorkspaceEditProposalError> {
    let digest = value
        .strip_prefix(prefix)
        .ok_or_else(|| error(format!("{label} is invalid")))?;
    validate_lower_sha256(digest, label)
}

fn validate_lower_sha256(value: &str, label: &str) -> Result<(), WorkspaceEditProposalError> {
    if value.len() != 64
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
        return Err(error(format!("{label} is invalid")));
    }
    Ok(())
}

fn prefixed_identity(
    prefix: &str,
    value: &impl Serialize,
) -> Result<String, WorkspaceEditProposalError> {
    let bytes = serde_json::to_vec(value)
        .map_err(|_| error("workspace edit proposal identity is not serializable"))?;
    let mut digest = Sha256::new();
    digest.update(prefix.as_bytes());
    digest.update([0]);
    digest.update(bytes);
    Ok(format!("{prefix}:sha256:{:x}", digest.finalize()))
}

fn provider_identity(prefix: &str, value: &str) -> String {
    let mut digest = Sha256::new();
    digest.update(prefix.as_bytes());
    digest.update([0]);
    digest.update(value.as_bytes());
    format!("{prefix}:sha256:{:x}", digest.finalize())
}

fn to_u64(value: usize, label: &str) -> Result<u64, WorkspaceEditProposalError> {
    u64::try_from(value).map_err(|_| error(format!("{label} is out of range")))
}

fn error(message: impl Into<String>) -> WorkspaceEditProposalError {
    WorkspaceEditProposalError {
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::workbench_store::StoredWorkspaceEditProposal;
    use crate::workspace_edit::{ProposedContent, WorkspaceEditOperation};
    use crate::workspace_edit_preview::{ContentInput, WorkspaceEditPreviewStore};
    use std::collections::HashSet;
    use std::fs;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU64, Ordering};

    static SEQUENCE: AtomicU64 = AtomicU64::new(0);
    const LEGACY_PROPOSAL_ID_FIXTURE: &str =
        "workspace-edit-proposal:sha256:0b7c50b6f395d2709744741854ed81fb9c659c03c8e4fc7d09e720537c83b833";
    const LEGACY_PREVIEW_ID_FIXTURE: &str =
        "workspace-edit-preview:sha256:56faa6585dcdf09eef6cfc2ff54cd2383c3c031ae7f62be47281d3a02a769a5b";
    const LEGACY_PROPOSAL_JSON_FIXTURE: &str = r#"{"schema_version":"workspace-edit-proposal/0.1","proposal_id":"workspace-edit-proposal:sha256:0b7c50b6f395d2709744741854ed81fb9c659c03c8e4fc7d09e720537c83b833","session_id":"session-1","turn_id":"turn-1","provider":"openai","provider_thread_id":"provider-thread-1","provider_item_id":"provider-item-1","approval_started_at_ms":10,"project_id":"project-1","root_id":"root-1","filesystem_root_identity":"filesystem-root:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","workspace_edit_root_identity":"workspace-root:sha256:68b7d8fdb16f0a901d973688fb3f25837669ab0a2429ad919213628f81afa20c","edit_id":"edit-1","canonical_edit_identity":"workspace-edit-canonical:sha256:59bc1e657e3961ef220de1bb39031e60baee8c986aa6ab290e76afe9b36fde3d","operations":[{"kind":"update","path":"old.txt","base":{"sha256":"01d09d19c2139a46aebfb577780d123d7396e97201bc7ead210a2ebff8239dee","bytes":4},"content":{"reference":"workspace-edit-content:sha256:7aa7a5359173d05b63cfd682e3c38487f3cb4f7f1d60659fe59fab1505977d4c","hash":{"sha256":"7aa7a5359173d05b63cfd682e3c38487f3cb4f7f1d60659fe59fab1505977d4c","bytes":4},"encoding":"utf-8","newline":"lf","mode":"preserve"}}],"artifacts":[{"kind":"proposed-content","reference":"workspace-edit-content:sha256:7aa7a5359173d05b63cfd682e3c38487f3cb4f7f1d60659fe59fab1505977d4c","sha256":"7aa7a5359173d05b63cfd682e3c38487f3cb4f7f1d60659fe59fab1505977d4c","bytes":4,"media_type":"text/plain; charset=utf-8"},{"kind":"diff","reference":"workspace-edit-diff:sha256:2ca8ab2ff3700e34b5f1e23baa2c7da4027fe934fdd6a3d2d802ffc745e6ba61","sha256":"2ca8ab2ff3700e34b5f1e23baa2c7da4027fe934fdd6a3d2d802ffc745e6ba61","bytes":50,"media_type":"text/x-diff; charset=utf-8"}],"overlap_baseline":{"schema_version":"workspace-edit-proposal-baseline/0.1","identity":"workspace-edit-proposal-baseline:sha256:592766619e330d78ea71ad0b0d4b97fab61234c522018f91a855b296d2a7613e","paths":[{"operation_index":0,"operation":"update","role":"target","path":"old.txt","expected":{"kind":"file","hash":{"sha256":"01d09d19c2139a46aebfb577780d123d7396e97201bc7ead210a2ebff8239dee","bytes":4}}}]},"preview":{"identity":"workspace-edit-preview:sha256:56faa6585dcdf09eef6cfc2ff54cd2383c3c031ae7f62be47281d3a02a769a5b","file_count":1,"additions":1,"deletions":1,"warning_count":0,"applicable":true,"aggregate_diff_reference":"workspace-edit-diff:sha256:2ca8ab2ff3700e34b5f1e23baa2c7da4027fe934fdd6a3d2d802ffc745e6ba61","file_diff_references":["workspace-edit-diff:sha256:2ca8ab2ff3700e34b5f1e23baa2c7da4027fe934fdd6a3d2d802ffc745e6ba61"]},"created_at_ms":10,"file_mutation_authority":false,"approval_recorded":false,"apply_available":false}"#;

    struct Root(PathBuf);

    impl Root {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!(
                "aegisy-workspace-edit-proposal-{}-{}",
                std::process::id(),
                SEQUENCE.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir_all(&path).unwrap();
            Self(path.canonicalize().unwrap())
        }
    }

    impl Drop for Root {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn proposal_fixture() -> WorkspaceEditProposal {
        let root = Root::new();
        fs::write(root.0.join("old.txt"), b"old\n").unwrap();
        let content = ProposedContent::for_bytes(b"new\n");
        let edit = WorkspaceEdit::define(
            "edit-1",
            "project-1",
            &root.0,
            vec![WorkspaceEditOperation::Update {
                path: "old.txt".into(),
                base: ContentHash::for_bytes(b"old\n"),
                content: content.clone(),
            }],
        )
        .unwrap();
        let mut previews = WorkspaceEditPreviewStore::default();
        let preview = previews
            .preview(
                "session-1",
                edit.clone(),
                vec![ContentInput {
                    reference: content.reference,
                    content: "new\n".into(),
                }],
                &HashSet::new(),
            )
            .unwrap();
        let snapshots = previews
            .snapshots("session-1", "edit-1", "project-1")
            .unwrap();
        let artifacts = snapshots
            .iter()
            .map(|snapshot| {
                let kind = if snapshot.reference.starts_with(DIFF_REFERENCE_PREFIX) {
                    WorkspaceEditProposalArtifactKind::Diff
                } else {
                    WorkspaceEditProposalArtifactKind::ProposedContent
                };
                WorkspaceEditProposalArtifact::from_snapshot(kind, snapshot).unwrap()
            })
            .collect();
        WorkspaceEditProposal::from_preview(
            "session-1",
            "turn-1",
            Some("openai".into()),
            "provider-thread-1",
            "provider-item-1",
            10,
            "root-1",
            "filesystem-root:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            &edit,
            &preview,
            artifacts,
            10,
        )
        .unwrap()
    }

    #[test]
    fn proposal_is_deterministic_strict_and_read_only() {
        let proposal = proposal_fixture();
        proposal.validate().unwrap();
        assert_eq!(proposal.schema_version, SCHEMA_VERSION);
        let files = proposal.preview.files.as_ref().unwrap();
        assert_eq!(files.len(), 1);
        assert_eq!(files[0].ordinal, 0);
        assert_eq!(files[0].kind, "update");
        assert_eq!(files[0].path, "old.txt");
        assert_eq!(files[0].from_path, None);
        assert_eq!(files[0].base_matches, Some(true));
        assert_eq!(files[0].proposed_format.as_ref().unwrap().newline, "lf");
        assert!(files[0].warnings.is_empty());
        assert_eq!(
            files[0].diff.reference,
            proposal.preview.file_diff_references[0]
        );
        assert_eq!(
            proposal.preview.aggregate_diff.as_ref().unwrap().reference,
            proposal.preview.aggregate_diff_reference
        );
        let encoded = serde_json::to_string(&proposal).unwrap();
        assert!(!encoded.contains("\"message\""));
        assert_eq!(
            serde_json::from_str::<WorkspaceEditProposal>(&encoded).unwrap(),
            proposal
        );
        assert!(!proposal.file_mutation_authority);
        assert!(!proposal.approval_recorded);
        assert!(!proposal.apply_available);

        let mut tampered = serde_json::from_str::<serde_json::Value>(&encoded).unwrap();
        tampered["apply_available"] = serde_json::Value::Bool(true);
        assert!(serde_json::from_value::<WorkspaceEditProposal>(tampered).is_err());
    }

    #[test]
    fn public_view_distinguishes_complete_and_legacy_file_summaries() {
        fn stored(proposal: WorkspaceEditProposal) -> StoredWorkspaceEditProposal {
            let encoded = serde_json::to_vec(&proposal).unwrap();
            let artifact_reference_ids = proposal
                .artifacts
                .iter()
                .enumerate()
                .map(|(ordinal, _)| format!("proposal-artifact-reference-{ordinal}"))
                .collect();
            StoredWorkspaceEditProposal {
                proposal,
                proposal_hash: ContentHash::for_bytes(&encoded),
                event_sequence: 7,
                artifact_reference_ids,
            }
        }

        let proposal = proposal_fixture();
        let complete = crate::workspace_edit_proposal_public_view(&stored(proposal.clone()))
            .expect("complete Proposal public view");
        let complete_file = &complete["summary"]["files"][0];
        assert_eq!(complete["summary"]["files_complete"], true);
        assert_eq!(complete_file["summary_state"], "complete");
        assert_eq!(complete_file["kind"], "update");
        assert_eq!(
            complete_file["base"]["sha256"],
            ContentHash::for_bytes(b"old\n").sha256
        );
        assert_eq!(
            complete_file["proposed"]["hash"]["sha256"],
            ContentHash::for_bytes(b"new\n").sha256
        );
        assert_eq!(complete_file["additions"], 1);
        assert_eq!(complete_file["deletions"], 1);
        assert_eq!(complete["file_mutation_authority"], false);
        assert_eq!(complete["approval_recorded"], false);
        assert_eq!(complete["apply_available"], false);

        let mut legacy = proposal;
        legacy.schema_version = LEGACY_SCHEMA_VERSION.into();
        legacy.preview.aggregate_diff = None;
        legacy.preview.files = None;
        legacy.preview.identity = preview_identity(&legacy.preview).unwrap();
        legacy.proposal_id = legacy.expected_proposal_id().unwrap();
        legacy.validate().unwrap();
        let legacy = crate::workspace_edit_proposal_public_view(&stored(legacy))
            .expect("legacy Proposal public view");
        let legacy_file = &legacy["summary"]["files"][0];
        assert_eq!(legacy["summary"]["files_complete"], false);
        assert_eq!(legacy_file["summary_state"], "legacy-incomplete");
        assert!(legacy_file["additions"].is_null());
        assert!(legacy_file["deletions"].is_null());
        assert!(legacy_file["warnings"].is_null());
        assert_eq!(
            legacy_file["base"]["sha256"],
            ContentHash::for_bytes(b"old\n").sha256
        );
        assert_eq!(
            legacy_file["proposed"]["hash"]["sha256"],
            ContentHash::for_bytes(b"new\n").sha256
        );
        assert!(legacy_file["diff"]["inline_truncated"].is_null());
        assert!(legacy_file["diff"]["source_truncated"].is_null());
    }

    #[test]
    fn legacy_proposal_preserves_canonical_reserialization_and_identity() {
        let mut proposal = proposal_fixture();
        proposal.schema_version = LEGACY_SCHEMA_VERSION.into();
        proposal.preview.aggregate_diff = None;
        proposal.preview.files = None;
        proposal.preview.identity = preview_identity(&proposal.preview).unwrap();
        proposal.proposal_id = proposal.expected_proposal_id().unwrap();
        proposal.validate().unwrap();

        let proposal_id = proposal.proposal_id.clone();
        let preview_id = proposal.preview.identity.clone();
        let encoded = serde_json::to_string(&proposal).unwrap();
        assert!(!encoded.contains("\"aggregate_diff\""));
        assert!(!encoded.contains("\"files\""));
        let decoded = serde_json::from_str::<WorkspaceEditProposal>(&encoded).unwrap();
        assert_eq!(decoded.proposal_id, proposal_id);
        assert_eq!(decoded.preview.identity, preview_id);
        assert_eq!(serde_json::to_string(&decoded).unwrap(), encoded);
        let fixed = serde_json::from_str::<WorkspaceEditProposal>(LEGACY_PROPOSAL_JSON_FIXTURE)
            .expect("fixed 0.1 Proposal fixture");
        assert_eq!(fixed.proposal_id, LEGACY_PROPOSAL_ID_FIXTURE);
        assert_eq!(fixed.preview.identity, LEGACY_PREVIEW_ID_FIXTURE);
        assert_eq!(
            serde_json::to_string(&fixed).unwrap(),
            LEGACY_PROPOSAL_JSON_FIXTURE
        );

        let mut invalid = serde_json::to_value(&decoded).unwrap();
        invalid["preview"]["files"] = serde_json::json!([]);
        assert!(serde_json::from_value::<WorkspaceEditProposal>(invalid).is_err());

        for fields in [
            ["aggregate_diff"].as_slice(),
            ["files"].as_slice(),
            ["aggregate_diff", "files"].as_slice(),
        ] {
            let mut invalid = serde_json::to_value(&decoded).unwrap();
            for field in fields {
                invalid["preview"][field] = serde_json::Value::Null;
            }
            assert!(serde_json::from_value::<WorkspaceEditProposal>(invalid).is_err());
        }
    }

    #[test]
    fn proposal_rejects_complete_summary_substitution() {
        let proposal = proposal_fixture();

        let mut tampered = proposal.clone();
        tampered.preview.files.as_mut().unwrap()[0].ordinal = 1;
        tampered.preview.identity = preview_identity(&tampered.preview).unwrap();
        tampered.proposal_id = tampered.expected_proposal_id().unwrap();
        assert!(tampered.validate().is_err());

        let mut tampered = proposal.clone();
        tampered.preview.files.as_mut().unwrap()[0]
            .proposed_format
            .as_mut()
            .unwrap()
            .newline = "crlf".into();
        tampered.preview.identity = preview_identity(&tampered.preview).unwrap();
        tampered.proposal_id = tampered.expected_proposal_id().unwrap();
        assert!(tampered.validate().is_err());

        let mut tampered = proposal.clone();
        tampered.preview.files.as_mut().unwrap()[0].additions += 1;
        tampered.preview.identity = preview_identity(&tampered.preview).unwrap();
        tampered.proposal_id = tampered.expected_proposal_id().unwrap();
        assert!(tampered.validate().is_err());

        let mut tampered = proposal.clone();
        tampered.preview.files.as_mut().unwrap()[0].diff.bytes += 1;
        tampered.preview.identity = preview_identity(&tampered.preview).unwrap();
        tampered.proposal_id = tampered.expected_proposal_id().unwrap();
        assert!(tampered.validate().is_err());

        let mut tampered = serde_json::to_value(&proposal).unwrap();
        tampered["preview"]["files"][0]["warnings"] = serde_json::json!([{
            "code": "stale-base",
            "severity": "blocking",
            "path": "old.txt",
            "message": "dynamic provider or filesystem text"
        }]);
        assert!(serde_json::from_value::<WorkspaceEditProposal>(tampered).is_err());
    }

    #[test]
    fn proposal_rejects_operation_and_artifact_substitution() {
        let proposal = proposal_fixture();
        let mut tampered = serde_json::to_value(&proposal).unwrap();
        tampered["operations"][0]["path"] = serde_json::Value::String("../escape".into());
        assert!(serde_json::from_value::<WorkspaceEditProposal>(tampered).is_err());

        let mut tampered = serde_json::to_value(&proposal).unwrap();
        tampered["artifacts"][0]["sha256"] = serde_json::Value::String("b".repeat(64));
        assert!(serde_json::from_value::<WorkspaceEditProposal>(tampered).is_err());
    }

    #[test]
    fn proposal_rejects_preview_substitution_with_recomputed_outer_identity() {
        let mut proposal = proposal_fixture();
        proposal.preview.additions += 1;
        proposal.proposal_id = proposal.expected_proposal_id().unwrap();
        assert!(proposal.validate().is_err());
    }
}
