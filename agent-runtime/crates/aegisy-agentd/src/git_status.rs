use serde::Serialize;
use std::collections::HashSet;
use std::fs;
use std::io::{Read, Write};
use std::path::{Component, Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

const STATUS_SCHEMA_VERSION: &str = "git-status/0.2";
const MINIMUM_GIT_VERSION: GitVersion = GitVersion {
    major: 2,
    minor: 31,
    patch: 0,
};
const MAX_GIT_STATUS_BYTES: u64 = 2 * 1024 * 1024;
const MAX_GIT_STATUS_ENTRIES: usize = 5_000;
const MAX_IGNORE_INPUT_BYTES: usize = 2 * 1024 * 1024;
const MAX_GIT_VERSION_BYTES: u64 = 256;
const GIT_VERSION_TIMEOUT: Duration = Duration::from_secs(5);

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GitStatusError {
    pub code: i64,
    pub message: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitStatusEntry {
    pub path: String,
    pub status: String,
    pub kind: String,
    pub index_status: String,
    pub worktree_status: String,
    pub submodule: String,
    pub staged: bool,
    pub unstaged: bool,
    pub conflicted: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub original_path: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub rename_score: Option<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitOperationState {
    pub kind: String,
    pub conflicted: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitStatusSnapshot {
    pub schema_version: String,
    pub available: bool,
    pub repository: bool,
    pub worktree: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub repository_root: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub head_oid: Option<String>,
    pub unborn: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub branch: Option<String>,
    pub detached: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub upstream: Option<String>,
    pub ahead: u64,
    pub behind: u64,
    pub entries: Vec<GitStatusEntry>,
    pub staged_paths: Vec<String>,
    pub unstaged_paths: Vec<String>,
    pub untracked_paths: Vec<String>,
    pub conflicts: Vec<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub operation_in_progress: Option<GitOperationState>,
    pub truncated: bool,
}

pub(crate) struct GitRunner {
    executable: PathBuf,
    root: PathBuf,
}

pub(crate) struct GitOutput {
    pub success: bool,
    pub code: Option<i32>,
    pub stdout: Vec<u8>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
struct GitVersion {
    major: u32,
    minor: u32,
    patch: u32,
}

pub(crate) struct GitCommitEnvironment<'a> {
    pub author_name: &'a str,
    pub author_email: &'a str,
    pub author_date: &'a str,
    pub committer_name: &'a str,
    pub committer_email: &'a str,
    pub committer_date: &'a str,
}

pub(crate) struct GitWorkflowEnvironment<'a> {
    pub author_name: Option<&'a str>,
    pub author_email: Option<&'a str>,
    pub author_date: Option<&'a str>,
    pub committer_name: &'a str,
    pub committer_email: &'a str,
    pub committer_date: &'a str,
}

enum GitIdentityEnvironment<'a> {
    Commit(&'a GitCommitEnvironment<'a>),
    Workflow(&'a GitWorkflowEnvironment<'a>),
}

#[derive(Default)]
struct ParsedStatus {
    head_oid: Option<String>,
    unborn: bool,
    branch: Option<String>,
    detached: bool,
    upstream: Option<String>,
    ahead: u64,
    behind: u64,
    entries: Vec<GitStatusEntry>,
    truncated: bool,
}

pub fn ignored_paths(root: &Path, paths: &[String]) -> HashSet<String> {
    if paths.is_empty() {
        return HashSet::new();
    }
    let input_bytes = paths
        .iter()
        .fold(0_usize, |total, path| total.saturating_add(path.len() + 1));
    if paths.len() > MAX_GIT_STATUS_ENTRIES || input_bytes > MAX_IGNORE_INPUT_BYTES {
        return HashSet::new();
    }
    let Ok(runner) = GitRunner::new_for_ignore(root) else {
        return HashSet::new();
    };
    let mut input = Vec::with_capacity(input_bytes);
    for path in paths {
        input.extend_from_slice(path.as_bytes());
        input.push(0);
    }
    let Ok(output) = runner.run(
        &["check-ignore", "--stdin", "-z"],
        Some(&input),
        MAX_GIT_STATUS_BYTES,
    ) else {
        return HashSet::new();
    };
    if !output.success && output.code != Some(1) {
        return HashSet::new();
    }
    output
        .stdout
        .split(|byte| *byte == 0)
        .filter(|path| !path.is_empty())
        .filter_map(|path| normalize_git_path(path).ok())
        .collect()
}

pub fn status(root: &Path) -> Result<GitStatusSnapshot, GitStatusError> {
    let runner = match GitRunner::new(root) {
        Ok(runner) => runner,
        Err(cause) if cause.code == -32041 => return Ok(empty_snapshot(false)),
        Err(cause) => return Err(cause),
    };
    let inside = runner.run(&["rev-parse", "--is-inside-work-tree"], None, 128)?;
    if !inside.success {
        return Ok(empty_snapshot(true));
    }
    let worktree = match trim_ascii(&inside.stdout) {
        b"true" => true,
        b"false" => false,
        _ => return Err(error("Git returned an invalid worktree state")),
    };
    let git_dir = required_absolute_path(
        &runner,
        &["rev-parse", "--absolute-git-dir"],
        "Git metadata directory",
    )?;
    if !worktree {
        let mut snapshot = empty_snapshot(true);
        snapshot.repository = true;
        snapshot.operation_in_progress = detect_operation(&git_dir, false);
        return Ok(snapshot);
    }
    let repository_root = required_absolute_path(
        &runner,
        &["rev-parse", "--show-toplevel"],
        "Git repository root",
    )?;
    let output = runner.run(
        &[
            "-c",
            "core.quotepath=false",
            "status",
            "--porcelain=v2",
            "--branch",
            "-z",
            "--untracked-files=normal",
            "--ignore-submodules=none",
        ],
        None,
        MAX_GIT_STATUS_BYTES,
    )?;
    if !output.success {
        return Err(error("Git porcelain-v2 status failed"));
    }
    let parsed = parse_porcelain_v2(&output.stdout)?;
    let mut staged_paths = Vec::new();
    let mut unstaged_paths = Vec::new();
    let mut untracked_paths = Vec::new();
    let mut conflicts = Vec::new();
    for entry in &parsed.entries {
        if entry.staged {
            staged_paths.push(entry.path.clone());
        }
        if entry.unstaged {
            unstaged_paths.push(entry.path.clone());
        }
        if entry.kind == "untracked" {
            untracked_paths.push(entry.path.clone());
        }
        if entry.conflicted {
            conflicts.push(entry.path.clone());
        }
    }
    Ok(GitStatusSnapshot {
        schema_version: STATUS_SCHEMA_VERSION.into(),
        available: true,
        repository: true,
        worktree: true,
        repository_root: Some(path_to_utf8(&repository_root, "Git repository root")?),
        head_oid: parsed.head_oid,
        unborn: parsed.unborn,
        branch: parsed.branch,
        detached: parsed.detached,
        upstream: parsed.upstream,
        ahead: parsed.ahead,
        behind: parsed.behind,
        entries: parsed.entries,
        staged_paths,
        unstaged_paths,
        untracked_paths,
        conflicts: conflicts.clone(),
        operation_in_progress: detect_operation(&git_dir, !conflicts.is_empty()),
        truncated: parsed.truncated,
    })
}

impl GitRunner {
    pub(crate) fn new(root: &Path) -> Result<Self, GitStatusError> {
        let runner = Self::new_for_ignore(root)?;
        verify_git_version(&runner.executable, &runner.root)?;
        Ok(runner)
    }

    fn new_for_ignore(root: &Path) -> Result<Self, GitStatusError> {
        let root = crate::plain_path(
            &root
                .canonicalize()
                .map_err(|_| error("Git project root is unavailable"))?,
        );
        if !root.is_dir() {
            return Err(error("Git project root is not a directory"));
        }
        let executable = resolve_git_executable(&root)?;
        Ok(Self { executable, root })
    }

    pub(crate) fn run(
        &self,
        args: &[&str],
        input: Option<&[u8]>,
        max_output: u64,
    ) -> Result<GitOutput, GitStatusError> {
        self.run_internal(args, input, max_output, None, None)
    }

    pub(crate) fn run_with_index(
        &self,
        args: &[&str],
        input: Option<&[u8]>,
        max_output: u64,
        index_path: &Path,
    ) -> Result<GitOutput, GitStatusError> {
        if !index_path.is_absolute() {
            return Err(error("Git index override must be absolute"));
        }
        self.run_internal(args, input, max_output, Some(index_path), None)
    }

    pub(crate) fn run_with_commit_environment(
        &self,
        args: &[&str],
        input: Option<&[u8]>,
        max_output: u64,
        environment: &GitCommitEnvironment<'_>,
    ) -> Result<GitOutput, GitStatusError> {
        self.run_internal(
            args,
            input,
            max_output,
            None,
            Some(GitIdentityEnvironment::Commit(environment)),
        )
    }

    pub(crate) fn run_with_workflow_environment(
        &self,
        args: &[&str],
        input: Option<&[u8]>,
        max_output: u64,
        environment: &GitWorkflowEnvironment<'_>,
    ) -> Result<GitOutput, GitStatusError> {
        self.run_internal(
            args,
            input,
            max_output,
            None,
            Some(GitIdentityEnvironment::Workflow(environment)),
        )
    }

    fn run_internal(
        &self,
        args: &[&str],
        input: Option<&[u8]>,
        max_output: u64,
        index_path: Option<&Path>,
        identity_environment: Option<GitIdentityEnvironment<'_>>,
    ) -> Result<GitOutput, GitStatusError> {
        let hook_override = format!("core.hooksPath={}", null_device());
        let mut command = Command::new(&self.executable);
        command
            .arg("-c")
            .arg(hook_override)
            .arg("-c")
            .arg("core.fsmonitor=false")
            .arg("-c")
            .arg("commit.gpgsign=false")
            .arg("-c")
            .arg("merge.gpgSign=false")
            .arg("-c")
            .arg("i18n.commitEncoding=UTF-8")
            .arg("-C")
            .arg(&self.root)
            .args(args)
            .stdin(if input.is_some() {
                Stdio::piped()
            } else {
                Stdio::null()
            })
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .env_clear()
            .env("PATH", minimal_git_path(&self.executable)?)
            .env("LC_ALL", "C")
            .env("LANG", "C")
            .env("GIT_TERMINAL_PROMPT", "0")
            .env("GIT_OPTIONAL_LOCKS", "0")
            .env("GIT_CONFIG_NOSYSTEM", "1")
            .env("GIT_CONFIG_GLOBAL", null_device());
        if let Some(index_path) = index_path {
            command.env("GIT_INDEX_FILE", index_path);
        }
        if let Some(environment) = identity_environment {
            match environment {
                GitIdentityEnvironment::Commit(environment) => {
                    command
                        .env("GIT_AUTHOR_NAME", environment.author_name)
                        .env("GIT_AUTHOR_EMAIL", environment.author_email)
                        .env("GIT_AUTHOR_DATE", environment.author_date)
                        .env("GIT_COMMITTER_NAME", environment.committer_name)
                        .env("GIT_COMMITTER_EMAIL", environment.committer_email)
                        .env("GIT_COMMITTER_DATE", environment.committer_date);
                }
                GitIdentityEnvironment::Workflow(environment) => {
                    command
                        .env("GIT_COMMITTER_NAME", environment.committer_name)
                        .env("GIT_COMMITTER_EMAIL", environment.committer_email)
                        .env("GIT_COMMITTER_DATE", environment.committer_date)
                        .env("GIT_EDITOR", "true")
                        .env("GIT_SEQUENCE_EDITOR", "true");
                    if let (Some(name), Some(email), Some(date)) = (
                        environment.author_name,
                        environment.author_email,
                        environment.author_date,
                    ) {
                        command
                            .env("GIT_AUTHOR_NAME", name)
                            .env("GIT_AUTHOR_EMAIL", email)
                            .env("GIT_AUTHOR_DATE", date);
                    }
                }
            }
        }
        for name in ["SystemRoot", "SYSTEMROOT", "TEMP", "TMP", "TMPDIR"] {
            if let Some(value) = safe_environment_path(name, &self.root) {
                command.env(name, value);
            }
        }
        let mut child = command
            .spawn()
            .map_err(|cause| error(format!("cannot start Git status command: {cause}")))?;
        if let Some(input) = input {
            child
                .stdin
                .take()
                .ok_or_else(|| error("Git status stdin is unavailable"))?
                .write_all(input)
                .map_err(|_| error("cannot write Git status input"))?;
        }
        let mut stdout = Vec::new();
        child
            .stdout
            .take()
            .ok_or_else(|| error("Git status stdout is unavailable"))?
            .take(max_output + 1)
            .read_to_end(&mut stdout)
            .map_err(|_| error("cannot read Git status output"))?;
        if stdout.len() as u64 > max_output {
            let _ = child.kill();
            let _ = child.wait();
            return Err(error("Git status output exceeded its limit"));
        }
        let exit = child
            .wait()
            .map_err(|_| error("cannot wait for Git status command"))?;
        Ok(GitOutput {
            success: exit.success(),
            code: exit.code(),
            stdout,
        })
    }
}

fn parse_porcelain_v2(bytes: &[u8]) -> Result<ParsedStatus, GitStatusError> {
    let records = bytes.split(|byte| *byte == 0).collect::<Vec<_>>();
    let mut parsed = ParsedStatus::default();
    let mut index = 0_usize;
    while index < records.len() {
        let record = records[index];
        if record.is_empty() {
            index += 1;
            continue;
        }
        if record.starts_with(b"# ") {
            parse_branch_header(record, &mut parsed)?;
            index += 1;
            continue;
        }
        if parsed.entries.len() >= MAX_GIT_STATUS_ENTRIES {
            parsed.truncated = true;
            if record.starts_with(b"2 ") {
                index = index.saturating_add(1);
            }
            index += 1;
            continue;
        }
        let entry = match record.first() {
            Some(b'1') => parse_ordinary(record)?,
            Some(b'2') => {
                index += 1;
                let original = records
                    .get(index)
                    .copied()
                    .ok_or_else(|| error("Git rename status is incomplete"))?;
                parse_rename(record, original)?
            }
            Some(b'u') => parse_unmerged(record)?,
            Some(b'?') => parse_untracked(record)?,
            Some(b'!') => {
                index += 1;
                continue;
            }
            _ => return Err(error("Git porcelain-v2 record is unsupported")),
        };
        parsed.entries.push(entry);
        index += 1;
    }
    Ok(parsed)
}

fn parse_branch_header(record: &[u8], parsed: &mut ParsedStatus) -> Result<(), GitStatusError> {
    let text = std::str::from_utf8(record).map_err(|_| error("Git branch status is not UTF-8"))?;
    if let Some(value) = text.strip_prefix("# branch.oid ") {
        if value == "(initial)" {
            parsed.unborn = true;
            parsed.head_oid = None;
        } else if valid_oid(value) {
            parsed.head_oid = Some(value.into());
        } else {
            return Err(error("Git branch object ID is invalid"));
        }
    } else if let Some(value) = text.strip_prefix("# branch.head ") {
        if value == "(detached)" {
            parsed.detached = true;
            parsed.branch = None;
        } else if !value.is_empty() {
            parsed.branch = Some(value.into());
        }
    } else if let Some(value) = text.strip_prefix("# branch.upstream ") {
        if !value.is_empty() {
            parsed.upstream = Some(value.into());
        }
    } else if let Some(value) = text.strip_prefix("# branch.ab ") {
        let mut fields = value.split_ascii_whitespace();
        parsed.ahead = parse_branch_count(fields.next(), '+')?;
        parsed.behind = parse_branch_count(fields.next(), '-')?;
        if fields.next().is_some() {
            return Err(error("Git ahead/behind status is malformed"));
        }
    }
    Ok(())
}

fn parse_ordinary(record: &[u8]) -> Result<GitStatusEntry, GitStatusError> {
    let fields = split_prefix_fields(record, 9)?;
    entry(
        fields[8], fields[1], fields[2], "ordinary", None, None, false,
    )
}

fn parse_rename(record: &[u8], original: &[u8]) -> Result<GitStatusEntry, GitStatusError> {
    let fields = split_prefix_fields(record, 10)?;
    entry(
        fields[9],
        fields[1],
        fields[2],
        "rename-or-copy",
        Some(original),
        Some(fields[8]),
        false,
    )
}

fn parse_unmerged(record: &[u8]) -> Result<GitStatusEntry, GitStatusError> {
    let fields = split_prefix_fields(record, 11)?;
    entry(
        fields[10], fields[1], fields[2], "unmerged", None, None, true,
    )
}

fn parse_untracked(record: &[u8]) -> Result<GitStatusEntry, GitStatusError> {
    if !record.starts_with(b"? ") {
        return Err(error("Git untracked status is malformed"));
    }
    let path = normalize_git_path(&record[2..])?;
    Ok(GitStatusEntry {
        path,
        status: "??".into(),
        kind: "untracked".into(),
        index_status: "?".into(),
        worktree_status: "?".into(),
        submodule: "N...".into(),
        staged: false,
        unstaged: true,
        conflicted: false,
        original_path: None,
        rename_score: None,
    })
}

fn entry(
    path: &[u8],
    xy: &[u8],
    submodule: &[u8],
    kind: &str,
    original_path: Option<&[u8]>,
    rename_score: Option<&[u8]>,
    conflicted: bool,
) -> Result<GitStatusEntry, GitStatusError> {
    if xy.len() != 2 {
        return Err(error("Git porcelain-v2 XY status is malformed"));
    }
    let index_status = char::from(xy[0]).to_string();
    let worktree_status = char::from(xy[1]).to_string();
    Ok(GitStatusEntry {
        path: normalize_git_path(path)?,
        status: std::str::from_utf8(xy)
            .map_err(|_| error("Git XY status is invalid"))?
            .into(),
        kind: kind.into(),
        index_status,
        worktree_status,
        submodule: std::str::from_utf8(submodule)
            .map_err(|_| error("Git submodule status is invalid"))?
            .into(),
        staged: conflicted || xy[0] != b'.',
        unstaged: conflicted || xy[1] != b'.',
        conflicted,
        original_path: original_path.map(normalize_git_path).transpose()?,
        rename_score: rename_score
            .map(|value| {
                std::str::from_utf8(value)
                    .map(str::to_owned)
                    .map_err(|_| error("Git rename score is invalid"))
            })
            .transpose()?,
    })
}

fn split_prefix_fields(record: &[u8], fields: usize) -> Result<Vec<&[u8]>, GitStatusError> {
    let mut result = Vec::with_capacity(fields);
    let mut remainder = record;
    for _ in 0..fields.saturating_sub(1) {
        let position = remainder
            .iter()
            .position(|byte| *byte == b' ')
            .ok_or_else(|| error("Git porcelain-v2 record is malformed"))?;
        result.push(&remainder[..position]);
        remainder = &remainder[position + 1..];
    }
    result.push(remainder);
    Ok(result)
}

fn detect_operation(git_dir: &Path, conflicted: bool) -> Option<GitOperationState> {
    let kind = if safe_marker(git_dir, "rebase-merge") {
        "rebase"
    } else if safe_marker(git_dir, "rebase-apply/applying") {
        "apply-mailbox"
    } else if safe_marker(git_dir, "rebase-apply") {
        "rebase"
    } else if safe_marker(git_dir, "MERGE_HEAD") {
        "merge"
    } else if safe_marker(git_dir, "CHERRY_PICK_HEAD") {
        "cherry-pick"
    } else if safe_marker(git_dir, "REVERT_HEAD") {
        "revert"
    } else if safe_marker(git_dir, "BISECT_LOG") {
        "bisect"
    } else if safe_marker(git_dir, "sequencer") {
        "sequencer"
    } else {
        return None;
    };
    Some(GitOperationState {
        kind: kind.into(),
        conflicted,
    })
}

fn safe_marker(git_dir: &Path, relative: &str) -> bool {
    let mut path = git_dir.to_path_buf();
    let components = relative.split('/').collect::<Vec<_>>();
    for (index, component) in components.iter().enumerate() {
        if component.is_empty() || matches!(*component, "." | "..") {
            return false;
        }
        path.push(component);
        let Ok(metadata) = fs::symlink_metadata(&path) else {
            return false;
        };
        if metadata.file_type().is_symlink() {
            return false;
        }
        if index + 1 < components.len() && !metadata.is_dir() {
            return false;
        }
    }
    true
}

fn required_absolute_path(
    runner: &GitRunner,
    args: &[&str],
    label: &str,
) -> Result<PathBuf, GitStatusError> {
    let output = runner.run(args, None, 16 * 1024)?;
    if !output.success {
        return Err(error(format!("{label} is unavailable")));
    }
    let text = std::str::from_utf8(trim_ascii(&output.stdout))
        .map_err(|_| error(format!("{label} is not UTF-8")))?;
    let path = PathBuf::from(text);
    if !path.is_absolute() {
        return Err(error(format!("{label} is not absolute")));
    }
    let canonical = crate::plain_path(
        &path
            .canonicalize()
            .map_err(|_| error(format!("{label} is unavailable")))?,
    );
    if !canonical.is_dir() {
        return Err(error(format!("{label} is not a directory")));
    }
    Ok(canonical)
}

fn normalize_git_path(bytes: &[u8]) -> Result<String, GitStatusError> {
    let path = std::str::from_utf8(bytes).map_err(|_| error("Git path is not UTF-8"))?;
    #[cfg(windows)]
    let path = path.replace('\\', "/");
    #[cfg(not(windows))]
    let path = path.to_owned();
    if path.is_empty()
        || Path::new(&path).is_absolute()
        || Path::new(&path)
            .components()
            .any(|component| !matches!(component, Component::Normal(_)))
    {
        return Err(error("Git returned an unsafe repository path"));
    }
    Ok(path)
}

fn parse_branch_count(value: Option<&str>, prefix: char) -> Result<u64, GitStatusError> {
    let value = value.ok_or_else(|| error("Git ahead/behind status is incomplete"))?;
    value
        .strip_prefix(prefix)
        .ok_or_else(|| error("Git ahead/behind status is malformed"))?
        .parse()
        .map_err(|_| error("Git ahead/behind count is invalid"))
}

fn valid_oid(value: &str) -> bool {
    matches!(value.len(), 40 | 64)
        && value
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
}

fn trim_ascii(bytes: &[u8]) -> &[u8] {
    let start = bytes
        .iter()
        .position(|byte| !byte.is_ascii_whitespace())
        .unwrap_or(bytes.len());
    let end = bytes
        .iter()
        .rposition(|byte| !byte.is_ascii_whitespace())
        .map_or(start, |index| index + 1);
    &bytes[start..end]
}

fn path_to_utf8(path: &Path, label: &str) -> Result<String, GitStatusError> {
    path.to_str()
        .map(str::to_owned)
        .ok_or_else(|| error(format!("{label} is not UTF-8")))
}

fn empty_snapshot(available: bool) -> GitStatusSnapshot {
    GitStatusSnapshot {
        schema_version: STATUS_SCHEMA_VERSION.into(),
        available,
        repository: false,
        worktree: false,
        repository_root: None,
        head_oid: None,
        unborn: false,
        branch: None,
        detached: false,
        upstream: None,
        ahead: 0,
        behind: 0,
        entries: Vec::new(),
        staged_paths: Vec::new(),
        unstaged_paths: Vec::new(),
        untracked_paths: Vec::new(),
        conflicts: Vec::new(),
        operation_in_progress: None,
        truncated: false,
    }
}

fn resolve_git_executable(root: &Path) -> Result<PathBuf, GitStatusError> {
    let executable_name = if cfg!(windows) { "git.exe" } else { "git" };
    #[cfg_attr(windows, allow(unused_mut))]
    let mut candidates = std::env::var_os("PATH")
        .map(|path| {
            std::env::split_paths(&path)
                .filter(|directory| directory.is_absolute())
                .map(|directory| directory.join(executable_name))
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    #[cfg(not(windows))]
    {
        candidates.push(PathBuf::from("/usr/bin/git"));
        candidates.push(PathBuf::from("/opt/homebrew/bin/git"));
        candidates.push(PathBuf::from("/usr/local/bin/git"));
    }
    for candidate in candidates {
        let Ok(canonical) = candidate.canonicalize() else {
            continue;
        };
        if canonical.is_file() && !canonical.starts_with(root) {
            return Ok(canonical);
        }
    }
    Err(GitStatusError {
        code: -32041,
        message: "Git executable is unavailable outside the project root".into(),
    })
}

fn verify_git_version(executable: &Path, root: &Path) -> Result<(), GitStatusError> {
    let mut command = Command::new(executable);
    command
        .arg("--version")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .env_clear()
        .env("PATH", minimal_git_path(executable)?)
        .env("LC_ALL", "C")
        .env("LANG", "C")
        .env("GIT_CONFIG_NOSYSTEM", "1")
        .env("GIT_CONFIG_GLOBAL", null_device());
    for name in ["SystemRoot", "SYSTEMROOT", "TEMP", "TMP", "TMPDIR"] {
        if let Some(value) = safe_environment_path(name, root) {
            command.env(name, value);
        }
    }
    let mut child = command
        .spawn()
        .map_err(|_| git_unavailable("cannot inspect Git version"))?;
    let (exit, stdout) = read_git_version_output(&mut child)?;
    if stdout.len() as u64 > MAX_GIT_VERSION_BYTES {
        return Err(git_unavailable("Git version output exceeded its limit"));
    }
    if !exit.success() {
        return Err(git_unavailable("Git version check failed"));
    }
    let version = parse_git_version(&stdout)
        .ok_or_else(|| git_unavailable("Git returned an unsupported version format"))?;
    if !is_supported_git_version(version) {
        return Err(git_unavailable("Git 2.31.0 or newer is required"));
    }
    Ok(())
}

fn read_git_version_output(child: &mut Child) -> Result<(ExitStatus, Vec<u8>), GitStatusError> {
    read_git_version_output_with_timeout(child, GIT_VERSION_TIMEOUT)
}

fn read_git_version_output_with_timeout(
    child: &mut Child,
    timeout: Duration,
) -> Result<(ExitStatus, Vec<u8>), GitStatusError> {
    let stdout = match child.stdout.take() {
        Some(stdout) => stdout,
        None => {
            terminate_git_version_child(child);
            return Err(git_unavailable("Git version output is unavailable"));
        }
    };
    let (sender, receiver) = mpsc::channel();
    let reader = thread::spawn(move || {
        let mut bytes = Vec::new();
        let result = stdout
            .take(MAX_GIT_VERSION_BYTES + 1)
            .read_to_end(&mut bytes)
            .map(|_| bytes);
        let _ = sender.send(result);
    });
    let deadline = Instant::now() + timeout;
    let mut output: Option<Vec<u8>> = None;
    let exit = loop {
        if output.is_none() {
            if let Ok(result) = receiver.try_recv() {
                match result {
                    Ok(bytes) => {
                        if bytes.len() as u64 > MAX_GIT_VERSION_BYTES {
                            terminate_git_version_child(child);
                            let _ = reader.join();
                            return Err(git_unavailable("Git version output exceeded its limit"));
                        }
                        output = Some(bytes);
                    }
                    Err(_) => {
                        terminate_git_version_child(child);
                        let _ = reader.join();
                        return Err(git_unavailable("cannot read Git version"));
                    }
                }
            }
        }
        match child.try_wait() {
            Ok(Some(status)) => break status,
            Ok(None) if Instant::now() < deadline => {
                thread::sleep(Duration::from_millis(10));
            }
            Ok(None) => {
                terminate_git_version_child(child);
                let _ = reader.join();
                return Err(git_unavailable("Git version check timed out"));
            }
            Err(_) => {
                terminate_git_version_child(child);
                let _ = reader.join();
                return Err(git_unavailable("cannot wait for Git version"));
            }
        }
    };
    let stdout = match output {
        Some(bytes) => {
            let _ = reader.join();
            bytes
        }
        None => {
            let result = receiver
                .recv_timeout(Duration::from_secs(1))
                .map_err(|_| git_unavailable("cannot read Git version"))
                .and_then(|result| result.map_err(|_| git_unavailable("cannot read Git version")));
            let _ = reader.join();
            result?
        }
    };
    Ok((exit, stdout))
}

fn terminate_git_version_child(child: &mut Child) {
    let _ = child.kill();
    let _ = child.wait();
}

fn is_supported_git_version(version: GitVersion) -> bool {
    version >= MINIMUM_GIT_VERSION
}

fn parse_git_version(bytes: &[u8]) -> Option<GitVersion> {
    let text = std::str::from_utf8(bytes).ok()?;
    let text = text.strip_suffix('\n').unwrap_or(text);
    let text = text.strip_suffix('\r').unwrap_or(text);
    let version_text = text.strip_prefix("git version ")?;
    let mut fields = version_text.split_ascii_whitespace();
    let version_token = fields.next()?;
    let extra_fields = fields.collect::<Vec<_>>();
    if !extra_fields.is_empty()
        && !(extra_fields.len() == 2
            && extra_fields[0] == "(Apple"
            && extra_fields[1].starts_with("Git-")
            && extra_fields[1].ends_with(')')
            && is_canonical_version_component(
                &extra_fields[1]["Git-".len()..extra_fields[1].len() - 1],
            ))
    {
        return None;
    }
    let parts = version_token.split('.').collect::<Vec<_>>();
    if parts.len() != 3
        && !(parts.len() == 5 && parts[3] == "windows" && is_canonical_version_component(parts[4]))
    {
        return None;
    }
    Some(GitVersion {
        major: parse_version_component(parts[0])?,
        minor: parse_version_component(parts[1])?,
        patch: parse_version_component(parts[2])?,
    })
}

fn parse_version_component(value: &str) -> Option<u32> {
    if !is_canonical_version_component(value) {
        return None;
    }
    value.parse().ok()
}

fn is_canonical_version_component(value: &str) -> bool {
    !value.is_empty()
        && value.bytes().all(|byte| byte.is_ascii_digit())
        && (value == "0" || !value.starts_with('0'))
}

fn minimal_git_path(executable: &Path) -> Result<std::ffi::OsString, GitStatusError> {
    let mut paths = Vec::new();
    if let Some(parent) = executable.parent() {
        paths.push(parent.to_path_buf());
    }
    #[cfg(not(windows))]
    {
        paths.push(PathBuf::from("/usr/bin"));
        paths.push(PathBuf::from("/bin"));
    }
    std::env::join_paths(paths).map_err(|_| error("cannot construct minimal Git PATH"))
}

fn safe_environment_path(name: &str, root: &Path) -> Option<std::ffi::OsString> {
    let value = std::env::var_os(name)?;
    let path = PathBuf::from(&value);
    if !path.is_absolute() {
        return None;
    }
    match path.canonicalize() {
        Ok(canonical) if !canonical.starts_with(root) => Some(canonical.into_os_string()),
        _ => None,
    }
}

#[cfg(windows)]
fn null_device() -> &'static str {
    "NUL"
}

#[cfg(not(windows))]
fn null_device() -> &'static str {
    "/dev/null"
}

fn error(message: impl Into<String>) -> GitStatusError {
    GitStatusError {
        code: -32040,
        message: message.into(),
    }
}

fn git_unavailable(message: impl Into<String>) -> GitStatusError {
    GitStatusError {
        code: -32041,
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    #[cfg(unix)]
    use std::os::unix::fs::PermissionsExt;

    static SEQUENCE: AtomicU64 = AtomicU64::new(0);

    fn root(label: &str) -> PathBuf {
        let root = std::env::temp_dir().join(format!(
            "aegisy-git-status-{label}-{}-{}",
            std::process::id(),
            SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&root).unwrap();
        root.canonicalize().unwrap()
    }

    fn git(root: &Path, args: &[&str], success: bool) -> Vec<u8> {
        let output = Command::new("git")
            .arg("-C")
            .arg(root)
            .args(args)
            .output()
            .unwrap();
        assert_eq!(output.status.success(), success, "git command: {args:?}");
        output.stdout
    }

    fn initialize(root: &Path) {
        git(root, &["init", "-q"], true);
        git(root, &["config", "user.name", "Aegisy Test"], true);
        git(root, &["config", "user.email", "test@aegisy.local"], true);
    }

    #[cfg(unix)]
    fn version_fixture(root: &Path, label: &str, body: &str) -> PathBuf {
        let executable = root.join(label);
        fs::write(&executable, format!("#!/bin/sh\n{body}\n")).unwrap();
        fs::set_permissions(&executable, fs::Permissions::from_mode(0o755)).unwrap();
        executable
    }

    #[cfg(unix)]
    fn version_output_fixture(root: &Path, label: &str, output: &str, exit_code: i32) -> PathBuf {
        version_fixture(
            root,
            label,
            &format!(
                "printf '%s' '{}'\nexit {}",
                output.replace('\'', "'\\''"),
                exit_code
            ),
        )
    }

    #[test]
    fn parses_supported_git_versions_and_rejects_ambiguous_formats() {
        assert_eq!(
            parse_git_version(b"git version 2.31.0\n"),
            Some(MINIMUM_GIT_VERSION)
        );
        assert_eq!(
            parse_git_version(b"git version 2.50.1 (Apple Git-155)\n"),
            Some(GitVersion {
                major: 2,
                minor: 50,
                patch: 1,
            })
        );
        assert_eq!(
            parse_git_version(b"git version 2.50.1.windows.1\r\n"),
            Some(GitVersion {
                major: 2,
                minor: 50,
                patch: 1,
            })
        );
        for invalid in [
            b"git version 2.31\n".as_slice(),
            b"git version 02.31.0\n".as_slice(),
            b"git version 2.31.0.rc1\n".as_slice(),
            b"git version 2.31.0.windows.x\n".as_slice(),
            b"git version 2.31.0 garbage\n".as_slice(),
            b"git version 2.31.0\x00extra\n".as_slice(),
            b"version 2.31.0\n".as_slice(),
        ] {
            assert_eq!(parse_git_version(invalid), None, "input: {invalid:?}");
        }
        assert!(!is_supported_git_version(GitVersion {
            major: 2,
            minor: 30,
            patch: 9,
        }));
        assert!(is_supported_git_version(MINIMUM_GIT_VERSION));
        assert!(is_supported_git_version(GitVersion {
            major: 3,
            minor: 0,
            patch: 0,
        }));
    }

    #[test]
    fn accepts_the_installed_git_only_after_version_preflight() {
        let root = root("version-preflight");
        let runner = GitRunner::new(&root).unwrap();
        let output = runner
            .run(&["--version"], None, MAX_GIT_VERSION_BYTES)
            .unwrap();
        assert!(output.success);
        assert!(parse_git_version(&output.stdout).is_some());
        fs::remove_dir_all(root).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn rejects_real_git_executable_version_failures() {
        let cases = [
            (
                "below-minimum",
                "git version 2.30.9\n",
                0,
                "Git 2.31.0 or newer is required",
            ),
            (
                "nonzero",
                "git version 2.50.1\n",
                7,
                "Git version check failed",
            ),
            (
                "malformed",
                "not a Git version\n",
                0,
                "Git returned an unsupported version format",
            ),
            (
                "oversized",
                "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
                0,
                "Git version output exceeded its limit",
            ),
        ];
        for (label, output, exit_code, message) in cases {
            let root = root(label);
            let executable = version_output_fixture(&root, "git", output, exit_code);
            let error = verify_git_version(&executable, &root).unwrap_err();
            assert_eq!(error.code, -32041, "fixture: {label}");
            assert_eq!(error.message, message, "fixture: {label}");
            fs::remove_dir_all(root).unwrap();
        }
    }

    #[cfg(unix)]
    #[test]
    fn kills_and_reaps_a_timed_out_git_version_fixture() {
        let root = root("version-timeout");
        let executable = version_fixture(&root, "git", "while :; do :; done");
        let mut child = Command::new(&executable)
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let error = read_git_version_output_with_timeout(&mut child, Duration::from_millis(50))
            .unwrap_err();
        assert_eq!(error.code, -32041);
        assert_eq!(error.message, "Git version check timed out");
        assert!(child.try_wait().unwrap().is_some());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn parses_porcelain_v2_branch_ordinary_rename_unmerged_and_untracked() {
        let oid = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        let bytes = format!(
            "# branch.oid {oid}\0# branch.head main\0# branch.upstream origin/main\0\
             # branch.ab +2 -3\0\
             1 M. N... 100644 100644 100644 {oid} {oid} staged.txt\0\
             2 R. N... 100644 100644 100644 {oid} {oid} R100 new.txt\0old.txt\0\
             u UU N... 100644 100644 100644 100644 {oid} {oid} {oid} conflict.txt\0\
             ? untracked.txt\0"
        );
        let parsed = parse_porcelain_v2(bytes.as_bytes()).unwrap();
        assert_eq!(parsed.head_oid.as_deref(), Some(oid));
        assert_eq!(parsed.branch.as_deref(), Some("main"));
        assert_eq!(parsed.upstream.as_deref(), Some("origin/main"));
        assert_eq!((parsed.ahead, parsed.behind), (2, 3));
        assert_eq!(parsed.entries.len(), 4);
        assert!(parsed.entries[0].staged);
        assert_eq!(parsed.entries[1].original_path.as_deref(), Some("old.txt"));
        assert_eq!(parsed.entries[1].rename_score.as_deref(), Some("R100"));
        assert!(parsed.entries[2].conflicted);
        assert_eq!(parsed.entries[3].kind, "untracked");
    }

    #[test]
    fn discovers_repository_root_and_classifies_real_worktree_changes() {
        let root = root("dirty");
        initialize(&root);
        fs::write(root.join("tracked.txt"), "before\n").unwrap();
        fs::write(root.join("old.txt"), "rename\n").unwrap();
        git(&root, &["add", "."], true);
        git(&root, &["commit", "-q", "-m", "initial"], true);
        fs::write(root.join("tracked.txt"), "after\n").unwrap();
        fs::write(root.join("staged.txt"), "staged\n").unwrap();
        git(&root, &["add", "staged.txt"], true);
        git(&root, &["mv", "old.txt", "new.txt"], true);
        fs::write(root.join("untracked.txt"), "untracked\n").unwrap();
        fs::create_dir(root.join("nested")).unwrap();

        let snapshot = status(&root.join("nested")).unwrap();
        assert!(snapshot.available && snapshot.repository && snapshot.worktree);
        assert_eq!(snapshot.repository_root.as_deref(), root.to_str());
        assert!(snapshot.head_oid.is_some());
        assert!(!snapshot.detached);
        assert!(snapshot.staged_paths.contains(&"staged.txt".into()));
        assert!(snapshot.staged_paths.contains(&"new.txt".into()));
        assert!(snapshot.unstaged_paths.contains(&"tracked.txt".into()));
        assert!(snapshot.untracked_paths.contains(&"untracked.txt".into()));
        assert!(snapshot.entries.iter().any(|entry| {
            entry.path == "new.txt" && entry.original_path.as_deref() == Some("old.txt")
        }));
        assert!(snapshot.conflicts.is_empty());
        assert!(snapshot.operation_in_progress.is_none());

        git(&root, &["reset", "--hard", "-q"], true);
        git(&root, &["clean", "-fdq"], true);
        git(&root, &["checkout", "--detach", "-q"], true);
        let detached = status(&root).unwrap();
        assert!(detached.detached);
        assert!(detached.branch.is_none());
        assert!(detached.head_oid.is_some());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn bounds_porcelain_entries_and_marks_truncation() {
        let oid = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        let mut bytes = format!("# branch.oid {oid}\0# branch.head main\0").into_bytes();
        for index in 0..=MAX_GIT_STATUS_ENTRIES {
            bytes.extend_from_slice(
                format!("1 M. N... 100644 100644 100644 {oid} {oid} file-{index}.txt\0").as_bytes(),
            );
        }
        let parsed = parse_porcelain_v2(&bytes).unwrap();
        assert_eq!(parsed.entries.len(), MAX_GIT_STATUS_ENTRIES);
        assert!(parsed.truncated);
    }

    #[test]
    fn reports_real_merge_conflict_and_operation_in_progress() {
        let root = root("conflict");
        initialize(&root);
        fs::write(root.join("conflict.txt"), "base\n").unwrap();
        git(&root, &["add", "."], true);
        git(&root, &["commit", "-q", "-m", "initial"], true);
        let base_branch = String::from_utf8(git(&root, &["symbolic-ref", "--short", "HEAD"], true))
            .unwrap()
            .trim()
            .to_owned();
        git(&root, &["checkout", "-q", "-b", "other"], true);
        fs::write(root.join("conflict.txt"), "other\n").unwrap();
        git(&root, &["commit", "-qam", "other"], true);
        git(&root, &["checkout", "-q", &base_branch], true);
        fs::write(root.join("conflict.txt"), "base-branch\n").unwrap();
        git(&root, &["commit", "-qam", "base branch"], true);
        git(&root, &["merge", "other"], false);

        let snapshot = status(&root).unwrap();
        assert_eq!(snapshot.conflicts, vec!["conflict.txt"]);
        assert!(snapshot.entries.iter().any(|entry| {
            entry.path == "conflict.txt" && entry.kind == "unmerged" && entry.conflicted
        }));
        assert_eq!(
            snapshot
                .operation_in_progress
                .as_ref()
                .map(|operation| operation.kind.as_str()),
            Some("merge")
        );
        assert!(snapshot.operation_in_progress.unwrap().conflicted);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn non_repository_does_not_fabricate_branch_state() {
        let root = root("none");
        let snapshot = status(&root).unwrap();
        assert!(snapshot.available);
        assert!(!snapshot.repository);
        assert!(!snapshot.worktree);
        assert!(snapshot.branch.is_none());
        assert!(snapshot.head_oid.is_none());
        assert!(snapshot.entries.is_empty());
        fs::remove_dir_all(root).unwrap();
    }
}
