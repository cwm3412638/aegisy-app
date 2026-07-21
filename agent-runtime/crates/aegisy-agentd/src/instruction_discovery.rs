use crate::output_redaction::redact_complete;
use crate::workspace::is_sensitive_path;
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::cmp::Ordering;
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Component, Path, PathBuf};
use std::time::UNIX_EPOCH;

pub const SCHEMA_VERSION: &str = "instruction-discovery/0.1";
pub const MAX_FILES: usize = 128;
pub const MAX_SINGLE_BYTES: u64 = 256 * 1024;
pub const MAX_TOTAL_BYTES: u64 = 2 * 1024 * 1024;
pub const MAX_NESTED_DEPTH: usize = 8;
pub const MAX_DIRECTORY_ENTRIES: usize = 2_048;

const MANAGED_RANK: u32 = 1_000;
const USER_RANK: u32 = 800;
const PROJECT_RANK: u32 = 400;

const INSTRUCTION_NAMES: &[&str] = &[
    "AGENTS.md",
    "CLAUDE.md",
    "CODEX.md",
    "GEMINI.md",
    "Aegisy.md",
    "copilot-instructions.md",
];

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SourceScope {
    Managed,
    User,
    Project,
    Nested,
}

impl SourceScope {
    fn as_str(self) -> &'static str {
        match self {
            Self::Managed => "managed",
            Self::User => "user",
            Self::Project => "project",
            Self::Nested => "nested",
        }
    }

    fn source_kind(self) -> &'static str {
        match self {
            Self::Managed | Self::User => self.as_str(),
            Self::Project | Self::Nested => "project",
        }
    }
}

#[derive(Debug, Clone)]
pub struct DiscoveryRoots {
    pub managed: Option<PathBuf>,
    pub user: Option<PathBuf>,
    pub project: PathBuf,
}

#[derive(Debug, Clone)]
pub struct DiscoveryRequest {
    pub target_path: Option<String>,
    pub include_content: bool,
}

#[derive(Debug, Clone, serde::Serialize, PartialEq, Eq)]
pub struct InstructionEntry {
    pub scope: String,
    pub source: String,
    pub relative_path: String,
    pub depth: usize,
    pub precedence_rank: u32,
    pub precedence_reason: String,
    pub trust: String,
    pub bytes: u64,
    pub token_estimate: u64,
    pub content_hash: Option<String>,
    pub revision: Option<String>,
    pub freshness: String,
    pub included: bool,
    pub content_state: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub rejection_reason: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub content: Option<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct InstructionDiscoveryResult {
    pub schema_version: &'static str,
    pub target_path: String,
    pub merge_order: &'static str,
    pub precedence: &'static str,
    pub entries: Vec<InstructionEntry>,
    pub included_files: usize,
    pub included_bytes: u64,
    pub truncated: bool,
    pub truncation_reasons: Vec<String>,
    pub rejection_reasons: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DiscoveryError {
    pub code: i64,
    pub message: String,
}

#[derive(Debug, Clone)]
struct Candidate {
    scope: SourceScope,
    path: PathBuf,
    relative_path: String,
    depth: usize,
    precedence_rank: u32,
    precedence_reason: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct FileRevision {
    size: u64,
    modified_ns: u128,
    file_type: u8,
}

pub fn discover(
    roots: &DiscoveryRoots,
    request: &DiscoveryRequest,
) -> Result<InstructionDiscoveryResult, DiscoveryError> {
    let project_root = canonical_root(&roots.project)?;
    let target = resolve_target(&project_root, request.target_path.as_deref())?;
    let target_path = target
        .strip_prefix(&project_root)
        .map_err(|_| error(-32030, "instruction target escapes project root"))?
        .to_string_lossy()
        .replace('\\', "/");
    let target_path = if target_path.is_empty() {
        ".".to_owned()
    } else {
        target_path
    };

    let mut candidates = Vec::new();
    let mut truncated = false;
    let mut truncation_reasons = BTreeSet::new();
    let mut rejection_reasons = BTreeSet::new();
    if let Some(root) = roots.managed.as_deref() {
        let managed = canonical_root(root)?;
        if managed.starts_with(&project_root) {
            return Err(error(
                -32035,
                "managed instruction root must be outside the project root",
            ));
        }
        collect_external_candidates(
            SourceScope::Managed,
            &managed,
            &mut candidates,
            &mut truncated,
            &mut truncation_reasons,
        )?;
    }
    if let Some(root) = roots.user.as_deref() {
        let user = canonical_root(root)?;
        if user.starts_with(&project_root) {
            return Err(error(
                -32035,
                "user instruction root must be outside the project root",
            ));
        }
        collect_external_candidates(
            SourceScope::User,
            &user,
            &mut candidates,
            &mut truncated,
            &mut truncation_reasons,
        )?;
    }
    collect_project_candidates(
        &project_root,
        &target,
        &mut candidates,
        &mut truncated,
        &mut truncation_reasons,
    )?;

    candidates.sort_by(candidate_cmp);
    let mut entries = Vec::new();
    let mut included_files = 0;
    let mut included_bytes = 0_u64;
    for candidate in candidates {
        let entry = inspect_candidate(
            &candidate,
            &project_root,
            request.include_content,
            &mut included_files,
            &mut included_bytes,
            &mut truncated,
            &mut truncation_reasons,
        )?;
        if let Some(reason) = entry.rejection_reason.as_ref() {
            rejection_reasons.insert(reason.clone());
        }
        entries.push(entry);
    }

    Ok(InstructionDiscoveryResult {
        schema_version: SCHEMA_VERSION,
        target_path,
        merge_order: "weakest-first",
        precedence: "managed > user > nested (closer depth wins) > project",
        entries,
        included_files,
        included_bytes,
        truncated,
        truncation_reasons: truncation_reasons.into_iter().collect(),
        rejection_reasons: rejection_reasons.into_iter().collect(),
    })
}

pub fn roots_from_environment(project: PathBuf) -> DiscoveryRoots {
    DiscoveryRoots {
        managed: std::env::var_os("AEGISY_MANAGED_INSTRUCTIONS_DIR").map(PathBuf::from),
        user: std::env::var_os("AEGISY_USER_INSTRUCTIONS_DIR").map(PathBuf::from),
        project,
    }
}

fn error(code: i64, message: impl Into<String>) -> DiscoveryError {
    DiscoveryError {
        code,
        message: message.into(),
    }
}

fn canonical_root(path: &Path) -> Result<PathBuf, DiscoveryError> {
    let metadata = fs::symlink_metadata(path)
        .map_err(|_| error(-32020, "instruction source root is unavailable"))?;
    if metadata.file_type().is_symlink() || !metadata.is_dir() {
        return Err(error(
            -32020,
            "instruction source root must be a non-symlink directory",
        ));
    }
    path.canonicalize()
        .map_err(|_| error(-32020, "instruction source root is unavailable"))
}

fn resolve_target(
    project_root: &Path,
    target_path: Option<&str>,
) -> Result<PathBuf, DiscoveryError> {
    let Some(target_path) = target_path.filter(|value| !value.is_empty()) else {
        return Ok(project_root.to_path_buf());
    };
    let relative = Path::new(target_path);
    if relative.is_absolute()
        || relative.components().any(|component| {
            matches!(
                component,
                Component::ParentDir | Component::RootDir | Component::Prefix(_)
            )
        })
        || target_path.chars().any(char::is_control)
    {
        return Err(error(-32602, "instruction target path is invalid"));
    }
    let joined = project_root.join(relative);
    let canonical = joined
        .canonicalize()
        .map_err(|_| error(-32020, "instruction target path is unavailable"))?;
    if !canonical.starts_with(project_root) {
        return Err(error(-32030, "instruction target escapes project root"));
    }
    if has_symlink_component(project_root, &canonical)? {
        return Err(error(-32030, "instruction target contains a symlink"));
    }
    if canonical.is_file() {
        canonical
            .parent()
            .map(Path::to_path_buf)
            .ok_or_else(|| error(-32030, "instruction target parent is unavailable"))
    } else if canonical.is_dir() {
        Ok(canonical)
    } else {
        Err(error(
            -32020,
            "instruction target is not a file or directory",
        ))
    }
}

fn has_symlink_component(root: &Path, path: &Path) -> Result<bool, DiscoveryError> {
    let relative = path
        .strip_prefix(root)
        .map_err(|_| error(-32030, "instruction path escapes project root"))?;
    let mut current = root.to_path_buf();
    for component in relative.components() {
        let Component::Normal(name) = component else {
            continue;
        };
        current.push(name);
        let metadata = fs::symlink_metadata(&current)
            .map_err(|_| error(-32020, "instruction path became unavailable"))?;
        if metadata.file_type().is_symlink() {
            return Ok(true);
        }
    }
    Ok(false)
}

fn collect_external_candidates(
    scope: SourceScope,
    root: &Path,
    candidates: &mut Vec<Candidate>,
    truncated: &mut bool,
    reasons: &mut BTreeSet<String>,
) -> Result<(), DiscoveryError> {
    let canonical = canonical_root(root)?;
    if canonical.components().any(|component| {
        matches!(
            component,
            Component::Normal(name) if is_denied_component(&name.to_string_lossy())
        )
    }) {
        return Err(error(
            -32035,
            "managed or user instruction root is denied by path policy",
        ));
    }
    collect_recursive(
        scope, &canonical, &canonical, 0, candidates, truncated, reasons,
    )
}

fn collect_project_candidates(
    root: &Path,
    target: &Path,
    candidates: &mut Vec<Candidate>,
    truncated: &mut bool,
    reasons: &mut BTreeSet<String>,
) -> Result<(), DiscoveryError> {
    let relative = target
        .strip_prefix(root)
        .map_err(|_| error(-32030, "instruction target escapes project root"))?;
    let components = relative
        .components()
        .filter_map(|component| match component {
            Component::Normal(name) => Some(name.to_os_string()),
            _ => None,
        })
        .collect::<Vec<_>>();
    let mut directory = root.to_path_buf();
    let max_depth = components.len().min(MAX_NESTED_DEPTH);
    for depth in 0..=max_depth {
        let scope = if depth == 0 {
            SourceScope::Project
        } else {
            SourceScope::Nested
        };
        let rank = if depth == 0 {
            PROJECT_RANK
        } else {
            PROJECT_RANK.saturating_add(depth.min(MAX_NESTED_DEPTH) as u32)
        };
        collect_direct_candidates(
            scope,
            &directory,
            root,
            depth,
            rank,
            if depth == 0 {
                "project-root"
            } else {
                "nested-project-depth"
            },
            candidates,
            truncated,
            reasons,
        )?;
        if depth < components.len() {
            directory.push(&components[depth]);
        }
    }
    if components.len() > MAX_NESTED_DEPTH {
        *truncated = true;
        reasons.insert("nested-depth-limit".into());
    }
    Ok(())
}

fn collect_recursive(
    scope: SourceScope,
    source_root: &Path,
    directory: &Path,
    depth: usize,
    candidates: &mut Vec<Candidate>,
    truncated: &mut bool,
    reasons: &mut BTreeSet<String>,
) -> Result<(), DiscoveryError> {
    if depth > MAX_NESTED_DEPTH || *truncated {
        return Ok(());
    }
    let rank = match scope {
        SourceScope::Managed => MANAGED_RANK,
        SourceScope::User => USER_RANK,
        SourceScope::Project | SourceScope::Nested => PROJECT_RANK,
    };
    collect_direct_candidates(
        scope,
        directory,
        source_root,
        depth,
        rank,
        if scope == SourceScope::Managed {
            "managed-policy"
        } else {
            "user-default"
        },
        candidates,
        truncated,
        reasons,
    )?;
    if *truncated {
        return Ok(());
    }
    let directories = sorted_entries(directory)?;
    for (index, entry) in directories.into_iter().enumerate() {
        if index >= MAX_DIRECTORY_ENTRIES {
            *truncated = true;
            reasons.insert("directory-entry-limit".into());
            break;
        }
        let path = entry.path();
        let metadata = fs::symlink_metadata(&path)
            .map_err(|_| error(-32031, "instruction directory entry is unavailable"))?;
        if metadata.file_type().is_symlink() || !metadata.is_dir() {
            continue;
        }
        let name = entry.file_name().to_string_lossy().into_owned();
        if is_denied_component(&name) {
            continue;
        }
        collect_recursive(
            scope,
            source_root,
            &path,
            depth + 1,
            candidates,
            truncated,
            reasons,
        )?;
        if *truncated {
            break;
        }
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn collect_direct_candidates(
    scope: SourceScope,
    directory: &Path,
    source_root: &Path,
    depth: usize,
    precedence_rank: u32,
    precedence_reason: &str,
    candidates: &mut Vec<Candidate>,
    truncated: &mut bool,
    reasons: &mut BTreeSet<String>,
) -> Result<(), DiscoveryError> {
    if *truncated {
        return Ok(());
    }
    let entries = sorted_entries(directory)?;
    let mut matching: BTreeMap<String, Vec<fs::DirEntry>> = BTreeMap::new();
    for entry in entries {
        let name = entry.file_name().to_string_lossy().into_owned();
        let Some(candidate) = INSTRUCTION_NAMES
            .iter()
            .find(|candidate| name.eq_ignore_ascii_case(candidate))
        else {
            continue;
        };
        matching
            .entry(candidate.to_ascii_lowercase())
            .or_default()
            .push(entry);
    }
    for (canonical_name, mut entries) in matching {
        entries.sort_by(|left, right| name_cmp(&left.file_name(), &right.file_name()));
        if entries.len() > 1 {
            for entry in entries {
                if candidates.len() >= MAX_FILES {
                    *truncated = true;
                    reasons.insert("file-count-limit".into());
                    return Ok(());
                }
                let relative = logical_relative(scope, source_root, &entry.path())?;
                candidates.push(Candidate {
                    scope,
                    path: entry.path(),
                    relative_path: relative,
                    depth,
                    precedence_rank,
                    precedence_reason: format!("{precedence_reason};ambiguous:{canonical_name}"),
                });
            }
            continue;
        }
        let entry = entries
            .pop()
            .expect("matching instruction entry should exist");
        if candidates.len() >= MAX_FILES {
            *truncated = true;
            reasons.insert("file-count-limit".into());
            return Ok(());
        }
        let relative = logical_relative(scope, source_root, &entry.path())?;
        candidates.push(Candidate {
            scope,
            path: entry.path(),
            relative_path: relative,
            depth,
            precedence_rank,
            precedence_reason: precedence_reason.into(),
        });
    }
    Ok(())
}

fn sorted_entries(directory: &Path) -> Result<Vec<fs::DirEntry>, DiscoveryError> {
    let mut entries = fs::read_dir(directory)
        .map_err(|_| error(-32031, "cannot scan instruction directory"))?
        .filter_map(Result::ok)
        .collect::<Vec<_>>();
    entries.sort_by(|left, right| name_cmp(&left.file_name(), &right.file_name()));
    Ok(entries)
}

fn name_cmp(left: &std::ffi::OsStr, right: &std::ffi::OsStr) -> Ordering {
    let left_text = left.to_string_lossy();
    let right_text = right.to_string_lossy();
    left_text
        .to_ascii_lowercase()
        .cmp(&right_text.to_ascii_lowercase())
        .then_with(|| left_text.as_bytes().cmp(right_text.as_bytes()))
}

fn candidate_cmp(left: &Candidate, right: &Candidate) -> Ordering {
    left.precedence_rank
        .cmp(&right.precedence_rank)
        .then_with(|| left.depth.cmp(&right.depth))
        .then_with(|| left.scope.as_str().cmp(right.scope.as_str()))
        .then_with(|| natural_path_cmp(&left.relative_path, &right.relative_path))
}

fn natural_path_cmp(left: &str, right: &str) -> Ordering {
    left.to_ascii_lowercase()
        .cmp(&right.to_ascii_lowercase())
        .then_with(|| left.as_bytes().cmp(right.as_bytes()))
}

fn logical_relative(
    scope: SourceScope,
    source_root: &Path,
    path: &Path,
) -> Result<String, DiscoveryError> {
    let relative = path
        .strip_prefix(source_root)
        .map_err(|_| error(-32030, "instruction path escapes source root"))?
        .to_string_lossy()
        .replace('\\', "/");
    if relative.is_empty()
        || relative.chars().any(char::is_control)
        || is_sensitive_path(Path::new(&relative))
        || relative.split('/').any(is_denied_component)
    {
        return Err(error(-32035, "instruction path is denied by policy"));
    }
    Ok(format!("{}/{}", scope.as_str(), relative))
}

fn is_denied_component(name: &str) -> bool {
    let lower = name.to_ascii_lowercase();
    lower == ".git"
        || lower == ".svn"
        || lower == ".hg"
        || lower == ".cache"
        || lower == "node_modules"
        || lower == "target"
        || lower == "dist"
        || lower == "release"
        || lower == ".venv"
        || lower == "venv"
        || lower.starts_with("build-")
        || lower.starts_with("build_")
        || lower.starts_with("cmake-build-")
        || is_sensitive_path(Path::new(name))
}

fn inspect_candidate(
    candidate: &Candidate,
    project_root: &Path,
    include_content: bool,
    included_files: &mut usize,
    included_bytes: &mut u64,
    truncated: &mut bool,
    reasons: &mut BTreeSet<String>,
) -> Result<InstructionEntry, DiscoveryError> {
    let scope = candidate.scope;
    if candidate.precedence_reason.contains(";ambiguous:") {
        return Ok(rejected_entry(candidate, "case-collision"));
    }
    let relative_to_project = candidate
        .path
        .strip_prefix(project_root)
        .ok()
        .map(|path| path.to_string_lossy().replace('\\', "/"));
    if scope == SourceScope::Project || scope == SourceScope::Nested {
        let Some(relative) = relative_to_project.as_deref() else {
            return Err(error(-32030, "project instruction path escapes root"));
        };
        if is_sensitive_path(Path::new(relative)) || relative.split('/').any(is_denied_component) {
            return Ok(rejected_entry(candidate, "denied-path"));
        }
        if crate::git_status::ignored_paths(project_root, &[relative.to_owned()]).contains(relative)
        {
            return Ok(rejected_entry(candidate, "git-ignored"));
        }
        if has_symlink_component(project_root, &candidate.path)? {
            return Ok(rejected_entry(candidate, "symlink-component"));
        }
    } else if candidate.relative_path.split('/').any(is_denied_component) {
        return Ok(rejected_entry(candidate, "denied-path"));
    }
    let before = fs::symlink_metadata(&candidate.path)
        .map_err(|_| error(-32020, "instruction file is unavailable"))?;
    if before.file_type().is_symlink() || !before.is_file() {
        return Ok(rejected_entry(candidate, "symlink-or-not-file"));
    }
    let bytes = before.len();
    let base = base_entry(candidate, bytes);
    if bytes > MAX_SINGLE_BYTES {
        return Ok(rejected_entry_with(base, "single-file-limit"));
    }
    if included_files.saturating_add(1) > MAX_FILES {
        *truncated = true;
        reasons.insert("file-count-limit".into());
        return Ok(rejected_entry_with(base, "file-count-limit"));
    }
    if included_bytes.saturating_add(bytes) > MAX_TOTAL_BYTES {
        *truncated = true;
        reasons.insert("total-byte-limit".into());
        return Ok(rejected_entry_with(base, "total-byte-limit"));
    }
    let raw = fs::read(&candidate.path)
        .map_err(|_| error(-32031, "instruction file could not be read"))?;
    let after = fs::symlink_metadata(&candidate.path)
        .map_err(|_| error(-32020, "instruction file disappeared during read"))?;
    if file_revision(&before) != file_revision(&after) {
        let mut entry = base;
        entry.freshness = "stale".into();
        entry.content_state = "stale".into();
        entry.rejection_reason = Some("file-changed-during-read".into());
        return Ok(entry);
    }
    let content = String::from_utf8(raw.clone())
        .map_err(|_| error(-32035, "instruction content is not valid UTF-8"))?;
    if content
        .chars()
        .any(|character| character.is_control() && !matches!(character, '\n' | '\r' | '\t'))
    {
        return Ok(rejected_entry_with(base, "control-character"));
    }
    if redact_complete(&content) != content {
        return Ok(rejected_entry_with(base, "secret-shaped-content"));
    }
    let digest = Sha256::digest(&raw);
    let hash = format!("sha256:{:x}", digest);
    let revision = format!(
        "instruction-revision:{}:{}",
        file_revision(&before).size,
        file_revision(&before).modified_ns
    );
    let token_estimate = ((content.chars().count() as u64).saturating_add(3) / 4).max(1);
    let mut entry = base;
    entry.token_estimate = token_estimate;
    entry.content_hash = Some(hash);
    entry.revision = Some(revision);
    entry.freshness = "fresh".into();
    entry.included = true;
    entry.content_state = "available".into();
    entry.content = include_content.then_some(content);
    *included_files += 1;
    *included_bytes = included_bytes.saturating_add(bytes);
    Ok(entry)
}

fn base_entry(candidate: &Candidate, bytes: u64) -> InstructionEntry {
    InstructionEntry {
        scope: candidate.scope.as_str().into(),
        source: candidate.scope.source_kind().into(),
        relative_path: candidate.relative_path.clone(),
        depth: candidate.depth,
        precedence_rank: candidate.precedence_rank,
        precedence_reason: candidate.precedence_reason.clone(),
        trust: "untrusted-data".into(),
        bytes,
        token_estimate: 0,
        content_hash: None,
        revision: None,
        freshness: "unavailable".into(),
        included: false,
        content_state: "unavailable".into(),
        rejection_reason: None,
        content: None,
    }
}

fn rejected_entry(candidate: &Candidate, reason: &str) -> InstructionEntry {
    rejected_entry_with(base_entry(candidate, 0), reason)
}

fn rejected_entry_with(mut entry: InstructionEntry, reason: &str) -> InstructionEntry {
    entry.content_state = "rejected".into();
    entry.rejection_reason = Some(reason.into());
    entry
}

fn file_revision(metadata: &fs::Metadata) -> FileRevision {
    let modified_ns = metadata
        .modified()
        .ok()
        .and_then(|time| time.duration_since(UNIX_EPOCH).ok())
        .map(|duration| duration.as_nanos())
        .unwrap_or_default();
    FileRevision {
        size: metadata.len(),
        modified_ns,
        file_type: if metadata.is_file() { 1 } else { 0 },
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs::{self, File};
    use std::io::Write;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_root(label: &str) -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("clock")
            .as_nanos();
        let root = std::env::temp_dir().join(format!("aegisy-instruction-{label}-{nonce}"));
        fs::create_dir_all(&root).expect("temp root");
        root
    }

    fn roots(project: &Path, managed: Option<&Path>, user: Option<&Path>) -> DiscoveryRoots {
        DiscoveryRoots {
            project: project.to_path_buf(),
            managed: managed.map(Path::to_path_buf),
            user: user.map(Path::to_path_buf),
        }
    }

    #[test]
    fn deterministic_precedence_is_weakest_first_and_nested_is_closer() {
        let project = temp_root("precedence");
        let managed = temp_root("managed");
        let user = temp_root("user");
        fs::create_dir_all(project.join("src")).expect("src");
        fs::write(project.join("AGENTS.md"), "project\n").expect("project instruction");
        fs::write(project.join("src/AGENTS.md"), "nested\n").expect("nested instruction");
        fs::write(user.join("AGENTS.md"), "user\n").expect("user instruction");
        fs::write(managed.join("AGENTS.md"), "managed\n").expect("managed instruction");

        let result = discover(
            &roots(&project, Some(&managed), Some(&user)),
            &DiscoveryRequest {
                target_path: Some("src".into()),
                include_content: true,
            },
        )
        .expect("discovery");
        let included = result
            .entries
            .iter()
            .filter(|entry| entry.included)
            .collect::<Vec<_>>();
        assert_eq!(
            included
                .iter()
                .map(|entry| entry.content.as_deref().unwrap())
                .collect::<Vec<_>>(),
            vec!["project\n", "nested\n", "user\n", "managed\n"]
        );
        assert_eq!(result.merge_order, "weakest-first");
        assert!(included
            .windows(2)
            .all(|pair| pair[0].precedence_rank <= pair[1].precedence_rank));
        let _ = fs::remove_dir_all(project);
        let _ = fs::remove_dir_all(managed);
        let _ = fs::remove_dir_all(user);
    }

    #[test]
    fn rejects_secrets_controls_and_symlinks_without_content() {
        let project = temp_root("reject");
        fs::write(
            project.join("AGENTS.md"),
            "API_KEY=sk-12345678901234567890\n",
        )
        .expect("secret instruction");
        fs::create_dir_all(project.join("nested")).expect("nested");
        fs::write(project.join("nested/CODEX.md"), "bad\u{0000}\n").expect("control instruction");
        #[cfg(unix)]
        std::os::unix::fs::symlink(project.join("AGENTS.md"), project.join("nested/CLAUDE.md"))
            .expect("symlink");
        let result = discover(
            &roots(&project, None, None),
            &DiscoveryRequest {
                target_path: Some("nested".into()),
                include_content: true,
            },
        )
        .expect("discovery");
        assert!(result
            .entries
            .iter()
            .filter(|entry| entry.rejection_reason.as_deref() == Some("secret-shaped-content"))
            .all(|entry| entry.content.is_none() && !entry.included));
        assert!(result
            .entries
            .iter()
            .any(|entry| entry.rejection_reason.as_deref() == Some("control-character")));
        #[cfg(unix)]
        assert!(result
            .entries
            .iter()
            .any(|entry| entry.rejection_reason.as_deref() == Some("symlink-component")));
        let _ = fs::remove_dir_all(project);
    }

    #[test]
    fn stable_case_collision_is_reported_and_not_merged() {
        let project = temp_root("collision");
        fs::write(project.join("AGENTS.md"), "one\n").expect("first");
        fs::write(project.join("agents.md"), "two\n").expect("second");
        let result = discover(
            &roots(&project, None, None),
            &DiscoveryRequest {
                target_path: None,
                include_content: true,
            },
        )
        .expect("discovery");
        if result.entries.len() == 2 {
            assert!(result.rejection_reasons.contains(&"case-collision".into()));
            assert!(result
                .entries
                .iter()
                .all(|entry| entry.precedence_reason.contains("ambiguous:agents.md")));
            assert!(result
                .entries
                .iter()
                .all(|entry| entry.rejection_reason.as_deref() == Some("case-collision")));
        } else {
            // The default macOS filesystem is case-insensitive and stores only
            // one of the two names; the discovery result remains deterministic.
            assert_eq!(result.entries.len(), 1);
        }
        let _ = fs::remove_dir_all(project);
    }

    #[test]
    fn bounds_single_content() {
        let project = temp_root("limits");
        let huge = "x".repeat((MAX_SINGLE_BYTES + 1) as usize);
        fs::write(project.join("AGENTS.md"), huge).expect("huge");
        let result = discover(
            &roots(&project, None, None),
            &DiscoveryRequest {
                target_path: None,
                include_content: true,
            },
        )
        .expect("discovery");
        assert_eq!(
            result.entries[0].rejection_reason.as_deref(),
            Some("single-file-limit")
        );
        let _ = fs::remove_dir_all(project);
    }

    #[test]
    fn non_instruction_files_are_not_read() {
        let project = temp_root("non-instruction");
        let mut file = File::create(project.join("README.md")).expect("readme");
        file.write_all(b"API_KEY=sk-12345678901234567890")
            .expect("write");
        let result = discover(
            &roots(&project, None, None),
            &DiscoveryRequest {
                target_path: None,
                include_content: true,
            },
        )
        .expect("discovery");
        assert!(result.entries.is_empty());
        let _ = fs::remove_dir_all(project);
    }

    #[test]
    fn large_monorepo_ignores_dependency_build_and_irrelevant_trees_deterministically() {
        let project = temp_root("large-monorepo-project");
        let user = temp_root("large-monorepo-user");
        fs::write(user.join("AGENTS.md"), "user guidance\n").expect("user instruction");

        for directory in ["node_modules", "target", ".cache"] {
            fs::create_dir_all(user.join(directory)).expect("ignored tree");
            fs::write(
                user.join(directory).join("AGENTS.md"),
                "irrelevant instruction body\n",
            )
            .expect("ignored instruction");
        }
        for index in 0..256 {
            let directory = user.join(format!("package-{index:03}"));
            fs::create_dir_all(&directory).expect("package tree");
            fs::write(
                directory.join("README.md"),
                "unrelated repository metadata\n",
            )
            .expect("unrelated file");
        }

        let request = DiscoveryRequest {
            target_path: None,
            include_content: true,
        };
        let first = discover(&roots(&project, None, Some(&user)), &request).expect("discovery");
        let second = discover(&roots(&project, None, Some(&user)), &request).expect("discovery");
        assert_eq!(first.entries.len(), 1);
        assert_eq!(first.entries[0].relative_path, "user/AGENTS.md");
        assert_eq!(first.entries[0].content.as_deref(), Some("user guidance\n"));
        assert!(!first.truncated);
        assert!(first.rejection_reasons.is_empty());
        assert_eq!(
            serde_json::to_string(&first).expect("serialize first"),
            serde_json::to_string(&second).expect("serialize second")
        );
        let _ = fs::remove_dir_all(project);
        let _ = fs::remove_dir_all(user);
    }

    #[test]
    fn nested_instruction_chain_is_target_scoped_and_closer_depth_wins() {
        let project = temp_root("nested-target");
        let target = project.join("packages/app/src");
        fs::create_dir_all(&target).expect("target");
        fs::create_dir_all(project.join("packages/other")).expect("sibling");
        fs::write(project.join("AGENTS.md"), "project\n").expect("project");
        fs::write(project.join("packages/AGENTS.md"), "packages\n").expect("packages");
        fs::write(project.join("packages/app/AGENTS.md"), "app\n").expect("app");
        fs::write(target.join("AGENTS.md"), "src\n").expect("src");
        fs::write(project.join("packages/other/AGENTS.md"), "sibling\n").expect("sibling");

        let result = discover(
            &roots(&project, None, None),
            &DiscoveryRequest {
                target_path: Some("packages/app/src".into()),
                include_content: true,
            },
        )
        .expect("discovery");
        let included = result
            .entries
            .iter()
            .filter(|entry| entry.included)
            .collect::<Vec<_>>();
        assert_eq!(
            included
                .iter()
                .map(|entry| entry.content.as_deref().unwrap())
                .collect::<Vec<_>>(),
            vec!["project\n", "packages\n", "app\n", "src\n"]
        );
        assert!(included
            .iter()
            .map(|entry| entry.relative_path.as_str())
            .all(|path| !path.contains("other")));
        assert!(included
            .windows(2)
            .all(|pair| pair[0].depth < pair[1].depth));
        let _ = fs::remove_dir_all(project);
    }
}
