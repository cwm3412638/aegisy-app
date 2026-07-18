use serde::Serialize;
use std::cmp::Ordering;
use std::collections::{HashSet, VecDeque};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Component, Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

pub const MAX_DIRECTORY_ENTRIES: usize = 500;
pub const MAX_TEXT_FILE_BYTES: u64 = 512 * 1024;
pub const MAX_SEARCH_FILES: usize = 5_000;
pub const MAX_SEARCH_ENTRIES: usize = 20_000;
pub const MAX_SEARCH_BYTES: u64 = 8 * 1024 * 1024;
pub const MAX_SEARCH_MATCHES: usize = 1_000;
pub const MAX_SEARCH_PAGE_SIZE: usize = 100;
const MAX_SEARCH_MATCHES_PER_FILE: usize = 20;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorkspaceError {
    pub code: i64,
    pub message: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceEntry {
    pub name: String,
    pub path: String,
    pub kind: String,
    pub size: u64,
    pub modified_ms: u64,
    pub readonly: bool,
    pub revision: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct DirectoryListing {
    pub path: String,
    pub entries: Vec<WorkspaceEntry>,
    pub ignored_count: usize,
    pub truncated: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct TextFile {
    pub path: String,
    pub content: String,
    pub size: u64,
    pub encoding: String,
    pub newline: String,
    pub readonly: bool,
    pub save_supported: bool,
    pub modified_ms: u64,
    pub revision: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct SavedTextFile {
    pub path: String,
    pub size: u64,
    pub modified_ms: u64,
    pub revision: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceMetadata {
    pub path: String,
    pub kind: String,
    pub size: u64,
    pub modified_ms: u64,
    pub readonly: bool,
    pub revision: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SearchCandidate {
    pub path: String,
    pub size: u64,
    pub revision: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceSearchMatch {
    pub path: String,
    pub match_type: String,
    pub line: usize,
    pub column: usize,
    pub end_column: usize,
    pub preview: String,
    pub revision: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct WorkspaceSearchResult {
    pub query: String,
    pub mode: String,
    pub matches: Vec<WorkspaceSearchMatch>,
    pub next_cursor: Option<String>,
    pub snapshot: String,
    pub stale: bool,
    pub truncated: bool,
    pub scanned_files: usize,
    pub skipped_files: usize,
}

pub fn collect_search_candidates(
    root: &Path,
) -> Result<(Vec<SearchCandidate>, bool), WorkspaceError> {
    let canonical_root = root
        .canonicalize()
        .map_err(|cause| error(-32030, format!("workspace root is unavailable: {cause}")))?;
    let mut directories = VecDeque::from([canonical_root.clone()]);
    let mut candidates = Vec::new();
    let mut truncated = false;
    let mut visited_entries = 0;
    while let Some(directory) = directories.pop_front() {
        if visited_entries >= MAX_SEARCH_ENTRIES {
            truncated = true;
            break;
        }
        let entries = fs::read_dir(&directory)
            .map_err(|cause| error(-32031, format!("cannot scan workspace: {cause}")))?;
        let remaining_entries = MAX_SEARCH_ENTRIES - visited_entries;
        let mut paths = entries
            .filter_map(Result::ok)
            .take(remaining_entries.saturating_add(1))
            .collect::<Vec<_>>();
        if paths.len() > remaining_entries {
            paths.truncate(remaining_entries);
            truncated = true;
        }
        paths.sort_by_key(|entry| entry.file_name().to_string_lossy().to_ascii_lowercase());
        for entry in paths {
            if visited_entries >= MAX_SEARCH_ENTRIES {
                truncated = true;
                break;
            }
            visited_entries += 1;
            let name = entry.file_name().to_string_lossy().into_owned();
            let absolute = entry.path();
            let relative = absolute
                .strip_prefix(&canonical_root)
                .map_err(|_| error(-32030, "search path escapes workspace root"))?
                .to_string_lossy()
                .replace('\\', "/");
            if is_ignored_name(&name) || is_sensitive_path(Path::new(&relative)) {
                continue;
            }
            let metadata = fs::symlink_metadata(&absolute)
                .map_err(|cause| error(-32031, format!("cannot inspect search path: {cause}")))?;
            if metadata.file_type().is_symlink() {
                continue;
            }
            if metadata.is_dir() {
                directories.push_back(absolute);
                continue;
            }
            if !metadata.is_file() {
                continue;
            }
            if candidates.len() >= MAX_SEARCH_FILES {
                truncated = true;
                break;
            }
            candidates.push(SearchCandidate {
                path: relative,
                size: metadata.len(),
                revision: metadata_revision(&metadata, "file"),
            });
        }
        if truncated {
            break;
        }
    }
    candidates.sort_by(|left, right| natural_name_cmp(&left.path, &right.path));
    Ok((candidates, truncated))
}

#[allow(clippy::too_many_arguments)]
pub fn search_workspace(
    root: &Path,
    candidates: &[SearchCandidate],
    ignored: &HashSet<String>,
    query: &str,
    mode: &str,
    case_sensitive: bool,
    cursor: Option<&str>,
    page_size: usize,
    candidate_truncated: bool,
) -> Result<WorkspaceSearchResult, WorkspaceError> {
    let query = query.trim();
    if query.is_empty() || query.chars().count() > 256 {
        return Err(error(
            -32050,
            "search query must contain 1 to 256 characters",
        ));
    }
    if !matches!(mode, "all" | "files" | "text") {
        return Err(error(-32050, "search mode must be all, files, or text"));
    }
    let page_size = page_size.clamp(1, MAX_SEARCH_PAGE_SIZE);
    let snapshot = search_snapshot(candidates, ignored);
    let (mut offset, stale) = match cursor {
        Some(cursor) => {
            let Some((cursor_snapshot, raw_offset)) = cursor.rsplit_once('|') else {
                return Err(error(-32051, "invalid search cursor"));
            };
            let parsed = raw_offset
                .parse::<usize>()
                .map_err(|_| error(-32051, "invalid search cursor offset"))?;
            if cursor_snapshot == snapshot {
                (parsed, false)
            } else {
                (0, true)
            }
        }
        None => (0, false),
    };
    let needle = if case_sensitive {
        query.to_owned()
    } else {
        query.to_lowercase()
    };
    let mut matches = Vec::new();
    let mut scanned_files = 0;
    let mut skipped_files = 0;
    let mut scanned_bytes = 0_u64;
    let mut truncated = candidate_truncated;
    for candidate in candidates {
        if ignored.contains(&candidate.path) {
            continue;
        }
        if matches.len() >= MAX_SEARCH_MATCHES {
            truncated = true;
            break;
        }
        let comparable_path = if case_sensitive {
            candidate.path.clone()
        } else {
            candidate.path.to_lowercase()
        };
        if mode != "text" {
            if let Some(byte_index) = comparable_path.find(&needle) {
                let column = comparable_path[..byte_index].chars().count() + 1;
                matches.push(WorkspaceSearchMatch {
                    path: candidate.path.clone(),
                    match_type: "filename".into(),
                    line: 0,
                    column,
                    end_column: column + query.chars().count(),
                    preview: candidate.path.clone(),
                    revision: candidate.revision.clone(),
                });
            }
        }
        if mode == "files" {
            continue;
        }
        if candidate.size > MAX_TEXT_FILE_BYTES
            || scanned_bytes.saturating_add(candidate.size) > MAX_SEARCH_BYTES
        {
            skipped_files += 1;
            truncated = true;
            continue;
        }
        let file = match read_text_file(root, &candidate.path) {
            Ok(file) => file,
            Err(error) if matches!(error.code, -32036..=-32033) => {
                skipped_files += 1;
                continue;
            }
            Err(error) => return Err(error),
        };
        scanned_files += 1;
        scanned_bytes = scanned_bytes.saturating_add(file.size);
        let mut file_matches = 0;
        for (line_index, line) in file.content.lines().enumerate() {
            if file_matches >= MAX_SEARCH_MATCHES_PER_FILE || matches.len() >= MAX_SEARCH_MATCHES {
                truncated = true;
                break;
            }
            let comparable_line = if case_sensitive {
                line.to_owned()
            } else {
                line.to_lowercase()
            };
            let mut search_start = 0;
            while search_start <= comparable_line.len() {
                let Some(relative_index) = comparable_line[search_start..].find(&needle) else {
                    break;
                };
                let byte_index = search_start + relative_index;
                let column = comparable_line[..byte_index].chars().count() + 1;
                matches.push(WorkspaceSearchMatch {
                    path: candidate.path.clone(),
                    match_type: "text".into(),
                    line: line_index + 1,
                    column,
                    end_column: column + query.chars().count(),
                    preview: truncate_search_preview(line),
                    revision: file.revision.clone(),
                });
                file_matches += 1;
                if file_matches >= MAX_SEARCH_MATCHES_PER_FILE
                    || matches.len() >= MAX_SEARCH_MATCHES
                {
                    truncated = true;
                    break;
                }
                search_start = byte_index + needle.len().max(1);
            }
        }
    }
    if offset > matches.len() {
        offset = matches.len();
    }
    let end = offset.saturating_add(page_size).min(matches.len());
    let page = matches[offset..end].to_vec();
    let next_cursor = (end < matches.len()).then(|| format!("{snapshot}|{end}"));
    truncated = truncated || next_cursor.is_some();
    Ok(WorkspaceSearchResult {
        query: query.into(),
        mode: mode.into(),
        matches: page,
        next_cursor,
        snapshot,
        stale,
        truncated,
        scanned_files,
        skipped_files,
    })
}

pub fn list_directory(root: &Path, relative: &str) -> Result<DirectoryListing, WorkspaceError> {
    let normalized = normalize_relative(relative)?;
    if is_denied_path(Path::new(&normalized)) {
        return Err(error(
            -32035,
            "workspace path is denied by visibility policy",
        ));
    }
    let directory = resolve(root, &normalized)?;
    if !directory.is_dir() {
        return Err(error(-32031, "workspace path is not a directory"));
    }

    let mut entries = Vec::new();
    let mut ignored_count = 0;
    let read_dir = fs::read_dir(&directory)
        .map_err(|cause| error(-32031, format!("cannot list workspace directory: {cause}")))?;
    for entry in read_dir {
        let entry = entry
            .map_err(|cause| error(-32031, format!("cannot read directory entry: {cause}")))?;
        let name = entry.file_name().to_string_lossy().into_owned();
        let relative_path = join_relative(relative, &name);
        if is_ignored_name(&name) || is_sensitive_path(Path::new(&relative_path)) {
            ignored_count += 1;
            continue;
        }
        let metadata = fs::symlink_metadata(entry.path())
            .map_err(|cause| error(-32031, format!("cannot read file metadata: {cause}")))?;
        let kind = if metadata.file_type().is_symlink() {
            "symlink"
        } else if metadata.is_dir() {
            "directory"
        } else if metadata.is_file() {
            "file"
        } else {
            "other"
        };
        entries.push(WorkspaceEntry {
            name,
            path: relative_path,
            kind: kind.into(),
            size: metadata.len(),
            modified_ms: modified_ms(&metadata),
            readonly: metadata.permissions().readonly(),
            revision: metadata_revision(&metadata, kind),
        });
    }
    entries.sort_by(
        |left, right| match (left.kind.as_str(), right.kind.as_str()) {
            ("directory", "directory") => natural_name_cmp(&left.name, &right.name),
            ("directory", _) => Ordering::Less,
            (_, "directory") => Ordering::Greater,
            _ => natural_name_cmp(&left.name, &right.name),
        },
    );
    let truncated = entries.len() > MAX_DIRECTORY_ENTRIES;
    entries.truncate(MAX_DIRECTORY_ENTRIES);
    Ok(DirectoryListing {
        path: normalized,
        entries,
        ignored_count,
        truncated,
    })
}

pub fn read_text_file(root: &Path, relative: &str) -> Result<TextFile, WorkspaceError> {
    read_text_file_with_bytes(root, relative).map(|(file, _)| file)
}

pub(crate) fn read_text_file_with_bytes(
    root: &Path,
    relative: &str,
) -> Result<(TextFile, Vec<u8>), WorkspaceError> {
    let normalized = normalize_relative(relative)?;
    if normalized.is_empty() || is_denied_path(Path::new(&normalized)) {
        return Err(error(
            -32035,
            "workspace file is denied by sensitive-path policy",
        ));
    }
    let path = resolve(root, &normalized)?;
    let metadata = fs::metadata(&path)
        .map_err(|cause| error(-32032, format!("cannot read workspace metadata: {cause}")))?;
    if !metadata.is_file() {
        return Err(error(-32032, "workspace path is not a regular file"));
    }
    if metadata.len() > MAX_TEXT_FILE_BYTES {
        return Err(error(
            -32033,
            format!("file exceeds {} byte preview limit", MAX_TEXT_FILE_BYTES),
        ));
    }

    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    File::open(&path)
        .and_then(|file| file.take(MAX_TEXT_FILE_BYTES + 1).read_to_end(&mut bytes))
        .map_err(|cause| error(-32032, format!("cannot read workspace file: {cause}")))?;
    if bytes.len() as u64 > MAX_TEXT_FILE_BYTES {
        return Err(error(
            -32033,
            format!("file exceeds {} byte preview limit", MAX_TEXT_FILE_BYTES),
        ));
    }
    if bytes.contains(&0) {
        return Err(error(-32034, "binary file preview is not supported"));
    }
    let (encoding, text_bytes) = if bytes.starts_with(&[0xef, 0xbb, 0xbf]) {
        ("utf-8-bom", &bytes[3..])
    } else {
        ("utf-8", bytes.as_slice())
    };
    let raw = std::str::from_utf8(text_bytes)
        .map_err(|_| error(-32034, "file is not valid UTF-8 text"))?;
    let newline = detect_newline(raw);
    let content = normalize_newlines(raw);
    let readonly = metadata.permissions().readonly();
    Ok((
        TextFile {
            path: normalized,
            size: metadata.len(),
            content,
            encoding: encoding.into(),
            newline: newline.into(),
            readonly,
            save_supported: !readonly && newline != "mixed",
            modified_ms: modified_ms(&metadata),
            revision: content_revision(&bytes),
        },
        bytes,
    ))
}

pub fn write_text_file(
    root: &Path,
    relative: &str,
    content: &str,
    expected_revision: &str,
    encoding: &str,
    newline: &str,
) -> Result<SavedTextFile, WorkspaceError> {
    let normalized = normalize_relative(relative)?;
    if normalized.is_empty() || is_denied_path(Path::new(&normalized)) {
        return Err(error(-32035, "workspace file is denied by write policy"));
    }
    if content.contains('\0') {
        return Err(error(-32044, "text content must not contain NUL bytes"));
    }
    if !matches!(encoding, "utf-8" | "utf-8-bom") {
        return Err(error(-32044, "unsupported text encoding for save"));
    }
    if !matches!(newline, "lf" | "crlf" | "none") {
        return Err(error(
            -32044,
            "mixed or unsupported line endings are read-only",
        ));
    }

    let path = resolve(root, &normalized)?;
    let metadata = fs::metadata(&path)
        .map_err(|cause| error(-32032, format!("cannot read workspace metadata: {cause}")))?;
    if !metadata.is_file() {
        return Err(error(-32032, "workspace path is not a regular file"));
    }
    if metadata.permissions().readonly() {
        return Err(error(-32043, "workspace file is read-only"));
    }
    let mut current = Vec::with_capacity(metadata.len().min(MAX_TEXT_FILE_BYTES) as usize);
    File::open(&path)
        .and_then(|file| file.take(MAX_TEXT_FILE_BYTES + 1).read_to_end(&mut current))
        .map_err(|cause| error(-32032, format!("cannot verify workspace file: {cause}")))?;
    if current.len() as u64 > MAX_TEXT_FILE_BYTES {
        return Err(error(-32033, "file exceeds text save limit"));
    }
    if content_revision(&current) != expected_revision {
        return Err(error(
            -32042,
            "workspace file changed since it was opened; reload before saving",
        ));
    }

    let normalized_content = normalize_newlines(content);
    let serialized = if newline == "crlf" {
        normalized_content.replace('\n', "\r\n")
    } else {
        normalized_content
    };
    let mut bytes = Vec::with_capacity(serialized.len() + 3);
    if encoding == "utf-8-bom" {
        bytes.extend_from_slice(&[0xef, 0xbb, 0xbf]);
    }
    bytes.extend_from_slice(serialized.as_bytes());
    if bytes.len() as u64 > MAX_TEXT_FILE_BYTES {
        return Err(error(-32033, "edited file exceeds text save limit"));
    }

    let parent = path
        .parent()
        .ok_or_else(|| error(-32032, "workspace file has no parent directory"))?;
    let file_name = path
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or("file");
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    let temporary = parent.join(format!(".{file_name}.aegisy-{unique}.tmp"));
    let save_result = (|| -> Result<(), WorkspaceError> {
        let mut output = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary)
            .map_err(|cause| error(-32041, format!("cannot create temporary file: {cause}")))?;
        output
            .write_all(&bytes)
            .and_then(|_| output.sync_all())
            .map_err(|cause| error(-32041, format!("cannot flush temporary file: {cause}")))?;
        fs::set_permissions(&temporary, metadata.permissions()).map_err(|cause| {
            error(
                -32041,
                format!("cannot preserve workspace permissions: {cause}"),
            )
        })?;
        let mut latest = Vec::with_capacity(metadata.len().min(MAX_TEXT_FILE_BYTES) as usize);
        File::open(&path)
            .and_then(|file| file.take(MAX_TEXT_FILE_BYTES + 1).read_to_end(&mut latest))
            .map_err(|cause| error(-32032, format!("cannot revalidate workspace file: {cause}")))?;
        if latest.len() as u64 > MAX_TEXT_FILE_BYTES
            || content_revision(&latest) != expected_revision
        {
            return Err(error(
                -32042,
                "workspace file changed while it was being saved; reload before retrying",
            ));
        }
        fs::rename(&temporary, &path)
            .map_err(|cause| error(-32041, format!("cannot replace workspace file: {cause}")))?;
        #[cfg(unix)]
        File::open(parent)
            .and_then(|directory| directory.sync_all())
            .map_err(|cause| error(-32041, format!("cannot flush workspace directory: {cause}")))?;
        Ok(())
    })();
    if save_result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    save_result?;
    let saved_metadata = fs::metadata(&path)
        .map_err(|cause| error(-32041, format!("cannot verify saved file: {cause}")))?;
    Ok(SavedTextFile {
        path: normalized,
        size: saved_metadata.len(),
        modified_ms: modified_ms(&saved_metadata),
        revision: content_revision(&bytes),
    })
}

pub fn path_metadata(root: &Path, relative: &str) -> Result<WorkspaceMetadata, WorkspaceError> {
    let normalized = normalize_relative(relative)?;
    if is_denied_path(Path::new(&normalized)) {
        return Err(error(
            -32035,
            "workspace path is denied by visibility policy",
        ));
    }
    let path = resolve(root, &normalized)?;
    let metadata = fs::metadata(&path)
        .map_err(|cause| error(-32032, format!("cannot read workspace metadata: {cause}")))?;
    let kind = if metadata.is_dir() {
        "directory"
    } else if metadata.is_file() {
        "file"
    } else {
        "other"
    };
    Ok(WorkspaceMetadata {
        path: normalized,
        kind: kind.into(),
        size: metadata.len(),
        modified_ms: modified_ms(&metadata),
        readonly: metadata.permissions().readonly(),
        revision: metadata_revision(&metadata, kind),
    })
}

fn resolve(root: &Path, relative: &str) -> Result<PathBuf, WorkspaceError> {
    let normalized = normalize_relative(relative)?;
    let canonical_root = root
        .canonicalize()
        .map_err(|cause| error(-32030, format!("workspace root is unavailable: {cause}")))?;
    let mut candidate = canonical_root.clone();
    for component in Path::new(&normalized).components() {
        let Component::Normal(name) = component else {
            continue;
        };
        candidate.push(name);
        let metadata = fs::symlink_metadata(&candidate)
            .map_err(|cause| error(-32030, format!("workspace path is unavailable: {cause}")))?;
        if metadata.file_type().is_symlink() {
            return Err(error(
                -32036,
                "symbolic links are not readable in this milestone",
            ));
        }
    }
    let canonical = candidate
        .canonicalize()
        .map_err(|cause| error(-32030, format!("workspace path is unavailable: {cause}")))?;
    if !canonical.starts_with(&canonical_root) {
        return Err(error(-32030, "workspace path escapes the authorized root"));
    }
    Ok(canonical)
}

fn normalize_relative(relative: &str) -> Result<String, WorkspaceError> {
    let path = Path::new(relative);
    if path.is_absolute() {
        return Err(error(-32030, "absolute workspace paths are not allowed"));
    }
    let mut normalized = PathBuf::new();
    for component in path.components() {
        match component {
            Component::Normal(name) => normalized.push(name),
            Component::CurDir => {}
            Component::ParentDir | Component::RootDir | Component::Prefix(_) => {
                return Err(error(-32030, "workspace path traversal is not allowed"))
            }
        }
    }
    Ok(normalized.to_string_lossy().replace('\\', "/"))
}

fn join_relative(parent: &str, name: &str) -> String {
    let parent = parent.trim_matches(['/', '\\']);
    if parent.is_empty() {
        name.to_owned()
    } else {
        format!("{parent}/{name}")
    }
}

fn is_ignored_name(name: &str) -> bool {
    matches!(
        name,
        ".git"
            | ".svn"
            | ".hg"
            | ".cache"
            | ".claude"
            | ".codex"
            | ".agents"
            | "node_modules"
            | "target"
            | "CMakeFiles"
            | "_deps"
            | "dist"
            | "release"
            | ".DS_Store"
    ) || name == "build"
        || name.starts_with("build-")
        || name.starts_with("build_")
        || name.starts_with("cmake-build-")
}

pub(crate) fn is_sensitive_path(path: &Path) -> bool {
    path.components().any(|component| {
        let Component::Normal(name) = component else {
            return false;
        };
        let name = name.to_string_lossy().to_ascii_lowercase();
        name == ".ssh"
            || name == ".aws"
            || name == ".env"
            || name.starts_with(".env.")
            || name == "id_rsa"
            || name == "id_ed25519"
            || name.ends_with(".pem")
            || name.ends_with(".key")
    })
}

fn is_denied_path(path: &Path) -> bool {
    is_sensitive_path(path)
        || path.components().any(|component| {
            let Component::Normal(name) = component else {
                return false;
            };
            is_ignored_name(&name.to_string_lossy())
        })
}

fn modified_ms(metadata: &fs::Metadata) -> u64 {
    metadata
        .modified()
        .ok()
        .and_then(|modified| modified.duration_since(UNIX_EPOCH).ok())
        .map(|duration| duration.as_millis() as u64)
        .unwrap_or_default()
}

fn metadata_revision(metadata: &fs::Metadata, kind: &str) -> String {
    let modified_ns = metadata
        .modified()
        .ok()
        .and_then(|modified| modified.duration_since(UNIX_EPOCH).ok())
        .map(|duration| duration.as_nanos())
        .unwrap_or_default();
    format!(
        "{kind}:{:x}:{modified_ns:x}:{}",
        metadata.len(),
        u8::from(metadata.permissions().readonly())
    )
}

fn content_revision(bytes: &[u8]) -> String {
    let mut hash = 0xcbf29ce484222325_u64;
    for byte in bytes {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    format!("content:{hash:016x}:{}", bytes.len())
}

fn search_snapshot(candidates: &[SearchCandidate], ignored: &HashSet<String>) -> String {
    let mut hash = 0xcbf29ce484222325_u64;
    for candidate in candidates {
        if ignored.contains(&candidate.path) {
            continue;
        }
        for byte in candidate
            .path
            .as_bytes()
            .iter()
            .chain(candidate.revision.as_bytes())
        {
            hash ^= u64::from(*byte);
            hash = hash.wrapping_mul(0x100000001b3);
        }
    }
    format!(
        "scan:{hash:016x}:{}",
        candidates.len() - ignored.len().min(candidates.len())
    )
}

fn truncate_search_preview(line: &str) -> String {
    const MAX_PREVIEW_CHARS: usize = 240;
    let normalized = line.replace('\t', "    ");
    if normalized.chars().count() <= MAX_PREVIEW_CHARS {
        normalized
    } else {
        let mut preview = normalized
            .chars()
            .take(MAX_PREVIEW_CHARS - 1)
            .collect::<String>();
        preview.push('…');
        preview
    }
}

fn detect_newline(content: &str) -> &'static str {
    let bytes = content.as_bytes();
    let mut lf = 0;
    let mut crlf = 0;
    let mut lone_cr = 0;
    let mut index = 0;
    while index < bytes.len() {
        match bytes[index] {
            b'\r' if bytes.get(index + 1) == Some(&b'\n') => {
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
    match (lf, crlf, lone_cr) {
        (0, 0, 0) => "none",
        (0, _, 0) => "crlf",
        (_, 0, 0) => "lf",
        _ => "mixed",
    }
}

fn normalize_newlines(content: &str) -> String {
    content.replace("\r\n", "\n").replace('\r', "\n")
}

fn natural_name_cmp(left: &str, right: &str) -> Ordering {
    left.to_ascii_lowercase().cmp(&right.to_ascii_lowercase())
}

fn error(code: i64, message: impl Into<String>) -> WorkspaceError {
    WorkspaceError {
        code,
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use std::sync::atomic::{AtomicU64, Ordering as AtomicOrdering};
    use std::time::{SystemTime, UNIX_EPOCH};

    static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

    fn fixture() -> PathBuf {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let sequence = FIXTURE_SEQUENCE.fetch_add(1, AtomicOrdering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "aegisy-workspace-test-{}-{unique}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(root.join("src")).unwrap();
        fs::create_dir_all(root.join(".claude")).unwrap();
        fs::create_dir_all(root.join("build_check")).unwrap();
        fs::write(root.join("src/你好.rs"), "fn main() {}\n").unwrap();
        fs::write(root.join(".env"), "TOKEN=secret\n").unwrap();
        root
    }

    #[test]
    fn lists_unicode_files_and_hides_sensitive_files() {
        let root = fixture();
        let listing = list_directory(&root, "").unwrap();
        assert!(listing.entries.iter().any(|entry| entry.name == "src"));
        assert!(!listing.entries.iter().any(|entry| entry.name == ".env"));
        assert_eq!(listing.ignored_count, 3);
        let source = read_text_file(&root, "src/你好.rs").unwrap();
        assert_eq!(source.content, "fn main() {}\n");
        let metadata = path_metadata(&root, "src/你好.rs").unwrap();
        assert_eq!(metadata.kind, "file");
        assert!(source.revision.starts_with("content:"));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_absolute_parent_and_sensitive_paths() {
        let root = fixture();
        assert_eq!(
            read_text_file(&root, "../outside").unwrap_err().code,
            -32030
        );
        assert_eq!(
            read_text_file(&root, "/etc/passwd").unwrap_err().code,
            -32030
        );
        assert_eq!(read_text_file(&root, ".env").unwrap_err().code, -32035);
        assert_eq!(list_directory(&root, ".claude").unwrap_err().code, -32035);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_binary_and_oversized_files() {
        let root = fixture();
        fs::write(root.join("binary.dat"), [1, 0, 2]).unwrap();
        assert_eq!(
            read_text_file(&root, "binary.dat").unwrap_err().code,
            -32034
        );
        let mut large = File::create(root.join("large.txt")).unwrap();
        large.set_len(MAX_TEXT_FILE_BYTES + 1).unwrap();
        large.flush().unwrap();
        assert_eq!(read_text_file(&root, "large.txt").unwrap_err().code, -32033);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn preserves_bom_and_crlf_and_rejects_stale_saves() {
        let root = fixture();
        let path = root.join("src/windows.txt");
        fs::write(&path, b"\xef\xbb\xbfline one\r\nline two\r\n").unwrap();
        let opened = read_text_file(&root, "src/windows.txt").unwrap();
        assert_eq!(opened.encoding, "utf-8-bom");
        assert_eq!(opened.newline, "crlf");
        assert_eq!(opened.content, "line one\nline two\n");
        let saved = write_text_file(
            &root,
            "src/windows.txt",
            "updated\ncontent\n",
            &opened.revision,
            &opened.encoding,
            &opened.newline,
        )
        .unwrap();
        assert_eq!(
            fs::read(&path).unwrap(),
            b"\xef\xbb\xbfupdated\r\ncontent\r\n"
        );
        assert_eq!(
            write_text_file(
                &root,
                "src/windows.txt",
                "stale\n",
                &opened.revision,
                &opened.encoding,
                &opened.newline,
            )
            .unwrap_err()
            .code,
            -32042
        );
        assert!(saved.revision.starts_with("content:"));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn workspace_search_pages_matches_and_detects_stale_cursors() {
        let root = fixture();
        fs::write(
            root.join("src/search.txt"),
            "Aegisy first\nAegisy second\nAegisy third\n",
        )
        .unwrap();
        let (candidates, candidate_truncated) = collect_search_candidates(&root).unwrap();
        let first = search_workspace(
            &root,
            &candidates,
            &HashSet::new(),
            "Aegisy",
            "text",
            true,
            None,
            2,
            candidate_truncated,
        )
        .unwrap();
        assert_eq!(first.matches.len(), 2);
        assert_eq!(first.matches[0].line, 1);
        let cursor = first
            .next_cursor
            .clone()
            .expect("search should have a next page");
        let second = search_workspace(
            &root,
            &candidates,
            &HashSet::new(),
            "Aegisy",
            "text",
            true,
            Some(&cursor),
            2,
            candidate_truncated,
        )
        .unwrap();
        assert_eq!(second.matches.len(), 1);
        assert!(!second.stale);

        fs::write(root.join("src/new.txt"), "Aegisy added\n").unwrap();
        let (changed_candidates, changed_truncated) = collect_search_candidates(&root).unwrap();
        let stale = search_workspace(
            &root,
            &changed_candidates,
            &HashSet::new(),
            "Aegisy",
            "text",
            true,
            Some(&cursor),
            2,
            changed_truncated,
        )
        .unwrap();
        assert!(stale.stale);
        assert_eq!(stale.matches[0].line, 1);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn workspace_search_respects_visibility_and_git_ignore_inputs() {
        let root = fixture();
        fs::write(root.join("src/ignored.txt"), "secret search term\n").unwrap();
        fs::write(root.join("src/visible.txt"), "public search term\n").unwrap();
        let (candidates, truncated) = collect_search_candidates(&root).unwrap();
        assert!(!candidates.iter().any(|candidate| candidate.path == ".env"));
        let ignored = HashSet::from([String::from("src/ignored.txt")]);
        let result = search_workspace(
            &root,
            &candidates,
            &ignored,
            "search term",
            "all",
            false,
            None,
            50,
            truncated,
        )
        .unwrap();
        assert!(result
            .matches
            .iter()
            .any(|entry| entry.path == "src/visible.txt"));
        assert!(!result
            .matches
            .iter()
            .any(|entry| entry.path == "src/ignored.txt"));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn workspace_search_honors_unicode_paths_and_case_sensitivity() {
        let root = fixture();
        fs::write(
            root.join("src/大小写.txt"),
            "Aegisy exact\naegisy lower\nAEGISY upper\n",
        )
        .unwrap();
        let (candidates, truncated) = collect_search_candidates(&root).unwrap();
        let strict = search_workspace(
            &root,
            &candidates,
            &HashSet::new(),
            "Aegisy",
            "text",
            true,
            None,
            50,
            truncated,
        )
        .unwrap();
        assert_eq!(strict.matches.len(), 1);
        assert_eq!(strict.matches[0].path, "src/大小写.txt");
        assert_eq!(strict.matches[0].line, 1);
        let folded = search_workspace(
            &root,
            &candidates,
            &HashSet::new(),
            "aegisy",
            "text",
            false,
            None,
            50,
            truncated,
        )
        .unwrap();
        assert_eq!(folded.matches.len(), 3);
        fs::remove_dir_all(root).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn rejects_symlinks_even_when_the_target_is_inside_the_root() {
        use std::os::unix::fs::symlink;
        let root = fixture();
        symlink(root.join("src/你好.rs"), root.join("link.rs")).unwrap();
        assert_eq!(read_text_file(&root, "link.rs").unwrap_err().code, -32036);
        fs::remove_dir_all(root).unwrap();
    }
}
