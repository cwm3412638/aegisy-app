use crate::git_status::{ignored_paths, GitRunner, GitStatusError};
use crate::workspace::is_sensitive_path;
use serde::Serialize;
use std::collections::BTreeMap;
use std::path::{Component, Path};

const QUERY_SCHEMA_VERSION: &str = "git-query/0.1";
const MAX_QUERY_BYTES: u64 = 2 * 1024 * 1024;
const MAX_BRANCHES: usize = 256;
const MAX_TAGS: usize = 512;
const MAX_REMOTES: usize = 128;
const MAX_WORKTREES: usize = 128;
const MAX_CHANGED_PATHS: usize = 2_000;

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitOverview {
    pub schema_version: String,
    pub branches: Vec<GitBranch>,
    pub tags: Vec<GitTag>,
    pub remotes: Vec<GitRemote>,
    pub worktrees: Vec<GitWorktree>,
    pub truncated: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitBranch {
    pub name: String,
    pub oid: String,
    pub current: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub upstream: Option<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitTag {
    pub name: String,
    pub oid: String,
    pub object_type: String,
    pub subject: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitRemote {
    pub name: String,
    pub fetch_targets: Vec<String>,
    pub push_targets: Vec<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq, Default)]
pub struct GitWorktree {
    pub path: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub head_oid: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub branch: Option<String>,
    pub detached: bool,
    pub bare: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub locked_reason: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub prunable_reason: Option<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitLogPage {
    pub schema_version: String,
    pub commits: Vec<GitLogEntry>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub next_cursor: Option<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitLogEntry {
    pub oid: String,
    pub parents: Vec<String>,
    pub author_name: String,
    pub author_email: String,
    pub author_time: i64,
    pub subject: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitCommitDetail {
    pub schema_version: String,
    pub oid: String,
    pub parents: Vec<String>,
    pub author_name: String,
    pub author_email: String,
    pub author_time: i64,
    pub committer_name: String,
    pub committer_email: String,
    pub committer_time: i64,
    pub subject: String,
    pub body: String,
    pub changed_paths: Vec<GitChangedPath>,
    pub paths_truncated: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitChangedPath {
    pub status: String,
    pub path: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub original_path: Option<String>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitDiffResult {
    pub schema_version: String,
    pub scope: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub oid: Option<String>,
    pub paths: Vec<String>,
    pub additions: usize,
    pub deletions: usize,
    pub bytes: usize,
    pub patch: String,
    pub truncated: bool,
}

pub fn overview(root: &Path) -> Result<GitOverview, GitStatusError> {
    let runner = worktree_runner(root)?;
    let branches_output = runner.run(
        &[
            "for-each-ref",
            "--format=%(refname:short)%00%(objectname)%00%(HEAD)%00%(upstream:short)",
            "refs/heads",
        ],
        None,
        MAX_QUERY_BYTES,
    )?;
    require_success(&branches_output, "Git branch query failed")?;
    let (branches, branches_truncated) = parse_branches(&branches_output.stdout)?;

    let tags_output = runner.run(
        &[
            "for-each-ref",
            "--format=%(refname:short)%00%(objectname)%00%(objecttype)%00%(subject)",
            "refs/tags",
        ],
        None,
        MAX_QUERY_BYTES,
    )?;
    require_success(&tags_output, "Git tag query failed")?;
    let (tags, tags_truncated) = parse_tags(&tags_output.stdout)?;

    let remotes_output = runner.run(&["remote", "-v"], None, 256 * 1024)?;
    require_success(&remotes_output, "Git remote query failed")?;
    let (remotes, remotes_truncated) = parse_remotes(&remotes_output.stdout)?;

    let worktrees_output = runner.run(
        &["worktree", "list", "--porcelain", "-z"],
        None,
        MAX_QUERY_BYTES,
    )?;
    require_success(&worktrees_output, "Git worktree query failed")?;
    let (worktrees, worktrees_truncated) = parse_worktrees(&worktrees_output.stdout)?;

    Ok(GitOverview {
        schema_version: QUERY_SCHEMA_VERSION.into(),
        branches,
        tags,
        remotes,
        worktrees,
        truncated: branches_truncated || tags_truncated || remotes_truncated || worktrees_truncated,
    })
}

pub fn log(root: &Path, limit: usize, cursor: Option<&str>) -> Result<GitLogPage, GitStatusError> {
    if !(1..=100).contains(&limit) {
        return Err(error("Git log limit must be between 1 and 100"));
    }
    if let Some(cursor) = cursor {
        validate_oid(cursor)?;
    }
    let runner = worktree_runner(root)?;
    let mut owned = vec![
        "log".to_owned(),
        "--no-decorate".into(),
        format!("--max-count={}", limit + 1 + usize::from(cursor.is_some())),
        "--format=%H%x00%P%x00%an%x00%ae%x00%at%x00%s%x00%x00".into(),
    ];
    if let Some(cursor) = cursor {
        owned.push(cursor.into());
    }
    let args = owned.iter().map(String::as_str).collect::<Vec<_>>();
    let output = runner.run(&args, None, MAX_QUERY_BYTES)?;
    require_success(&output, "Git log query failed")?;
    let mut commits = parse_log(&output.stdout)?;
    if cursor.is_some()
        && commits
            .first()
            .is_some_and(|entry| Some(entry.oid.as_str()) == cursor)
    {
        commits.remove(0);
    }
    let has_more = commits.len() > limit;
    commits.truncate(limit);
    let next_cursor = has_more
        .then(|| commits.last().map(|entry| entry.oid.clone()))
        .flatten();
    Ok(GitLogPage {
        schema_version: QUERY_SCHEMA_VERSION.into(),
        commits,
        next_cursor,
    })
}

pub fn commit(root: &Path, oid: &str) -> Result<GitCommitDetail, GitStatusError> {
    validate_oid(oid)?;
    let runner = worktree_runner(root)?;
    let output = runner.run(
        &[
            "show",
            "-s",
            "--no-show-signature",
            "--format=%H%x00%P%x00%an%x00%ae%x00%at%x00%cn%x00%ce%x00%ct%x00%s%x00%B%x00%x00",
            oid,
        ],
        None,
        MAX_QUERY_BYTES,
    )?;
    require_success(&output, "Git commit query failed")?;
    let fields = first_record(&output.stdout, 10, "Git commit detail")?;
    let prefix = repository_prefix(&runner)?;
    let mut path_args = vec![
        "diff-tree".to_owned(),
        "--root".into(),
        "--no-commit-id".into(),
        "--name-status".into(),
        "-r".into(),
        "-z".into(),
        "-M".into(),
        oid.into(),
    ];
    if !prefix.is_empty() {
        path_args.push("--".into());
        path_args.push(top_pathspec(prefix.trim_end_matches('/')));
    }
    let args = path_args.iter().map(String::as_str).collect::<Vec<_>>();
    let paths_output = runner.run(&args, None, MAX_QUERY_BYTES)?;
    require_success(&paths_output, "Git commit path query failed")?;
    let (changed_paths, paths_truncated) =
        parse_changed_paths(root, &prefix, &paths_output.stdout)?;
    Ok(GitCommitDetail {
        schema_version: QUERY_SCHEMA_VERSION.into(),
        oid: utf8(fields[0], "Git commit OID")?,
        parents: split_oids(fields[1])?,
        author_name: utf8(fields[2], "Git author name")?,
        author_email: utf8(fields[3], "Git author email")?,
        author_time: parse_time(fields[4])?,
        committer_name: utf8(fields[5], "Git committer name")?,
        committer_email: utf8(fields[6], "Git committer email")?,
        committer_time: parse_time(fields[7])?,
        subject: utf8(fields[8], "Git commit subject")?,
        body: utf8(fields[9], "Git commit body")?,
        changed_paths,
        paths_truncated,
    })
}

pub fn diff(
    root: &Path,
    scope: &str,
    oid: Option<&str>,
    path: Option<&str>,
) -> Result<GitDiffResult, GitStatusError> {
    if !matches!(scope, "worktree" | "staged" | "commit") {
        return Err(error("Git diff scope must be worktree, staged, or commit"));
    }
    if scope == "commit" {
        validate_oid(oid.ok_or_else(|| error("Git commit diff requires an OID"))?)?;
    } else if oid.is_some() {
        return Err(error("Git worktree/staged diff does not accept an OID"));
    }
    let requested_path = path.map(normalize_project_path).transpose()?;
    let runner = worktree_runner(root)?;
    let prefix = repository_prefix(&runner)?;
    let mut base = diff_base_args(scope, oid);
    base.push("--name-status".into());
    base.push("-M".into());
    base.push("-z".into());
    if let Some(path) = &requested_path {
        base.push("--".into());
        base.push(top_pathspec(&format!("{prefix}{path}")));
    } else if !prefix.is_empty() {
        base.push("--".into());
        base.push(top_pathspec(prefix.trim_end_matches('/')));
    }
    let args = base.iter().map(String::as_str).collect::<Vec<_>>();
    let names = runner.run(&args, None, MAX_QUERY_BYTES)?;
    require_success(&names, "Git diff path query failed")?;
    let (changes, paths_truncated) = parse_changed_paths(root, &prefix, &names.stdout)?;
    if paths_truncated {
        return Err(error("Git diff changed-path limit exceeded"));
    }
    let mut paths = changes
        .into_iter()
        .map(|change| change.path)
        .collect::<Vec<_>>();
    paths.sort();
    paths.dedup();
    if paths.is_empty() {
        return Ok(GitDiffResult {
            schema_version: QUERY_SCHEMA_VERSION.into(),
            scope: scope.into(),
            oid: oid.map(str::to_owned),
            paths: Vec::new(),
            additions: 0,
            deletions: 0,
            bytes: 0,
            patch: String::new(),
            truncated: false,
        });
    }
    let mut patch_args = diff_base_args(scope, oid);
    patch_args.extend([
        "--no-color".into(),
        "--no-ext-diff".into(),
        "--unified=3".into(),
    ]);
    patch_args.push("--".into());
    patch_args.extend(
        paths
            .iter()
            .map(|path| top_pathspec(&format!("{prefix}{path}"))),
    );
    let args = patch_args.iter().map(String::as_str).collect::<Vec<_>>();
    let output = runner.run(&args, None, MAX_QUERY_BYTES)?;
    require_success(&output, "Git diff query failed")?;
    let patch = String::from_utf8(output.stdout).map_err(|_| error("Git diff is not UTF-8"))?;
    let (additions, deletions) = count_patch_lines(&patch);
    Ok(GitDiffResult {
        schema_version: QUERY_SCHEMA_VERSION.into(),
        scope: scope.into(),
        oid: oid.map(str::to_owned),
        paths,
        additions,
        deletions,
        bytes: patch.len(),
        patch,
        truncated: false,
    })
}

fn worktree_runner(root: &Path) -> Result<GitRunner, GitStatusError> {
    let runner = GitRunner::new(root)?;
    let output = runner.run(&["rev-parse", "--is-inside-work-tree"], None, 128)?;
    if !output.success || trim_ascii(&output.stdout) != b"true" {
        return Err(error("project is not inside a Git worktree"));
    }
    Ok(runner)
}

fn repository_prefix(runner: &GitRunner) -> Result<String, GitStatusError> {
    let output = runner.run(&["rev-parse", "--show-prefix"], None, 16 * 1024)?;
    require_success(&output, "Git project prefix query failed")?;
    let prefix = utf8(trim_ascii(&output.stdout), "Git project prefix")?;
    if prefix.is_empty() {
        return Ok(prefix);
    }
    let trimmed = prefix.trim_end_matches('/');
    normalize_project_path(trimmed).map(|path| format!("{path}/"))
}

fn parse_branches(bytes: &[u8]) -> Result<(Vec<GitBranch>, bool), GitStatusError> {
    let mut branches = Vec::new();
    let mut truncated = false;
    for line in bytes
        .split(|byte| *byte == b'\n')
        .filter(|line| !line.is_empty())
    {
        if branches.len() >= MAX_BRANCHES {
            truncated = true;
            continue;
        }
        let fields = line.split(|byte| *byte == 0).collect::<Vec<_>>();
        if fields.len() != 4 {
            return Err(error("Git branch record is malformed"));
        }
        branches.push(GitBranch {
            name: utf8(fields[0], "Git branch name")?,
            oid: checked_oid(fields[1])?,
            current: fields[2] == b"*",
            upstream: (!fields[3].is_empty())
                .then(|| utf8(fields[3], "Git branch upstream"))
                .transpose()?,
        });
    }
    Ok((branches, truncated))
}

fn parse_tags(bytes: &[u8]) -> Result<(Vec<GitTag>, bool), GitStatusError> {
    let mut tags = Vec::new();
    let mut truncated = false;
    for line in bytes
        .split(|byte| *byte == b'\n')
        .filter(|line| !line.is_empty())
    {
        if tags.len() >= MAX_TAGS {
            truncated = true;
            continue;
        }
        let fields = line.split(|byte| *byte == 0).collect::<Vec<_>>();
        if fields.len() != 4 {
            return Err(error("Git tag record is malformed"));
        }
        tags.push(GitTag {
            name: utf8(fields[0], "Git tag name")?,
            oid: checked_oid(fields[1])?,
            object_type: utf8(fields[2], "Git tag object type")?,
            subject: utf8(fields[3], "Git tag subject")?,
        });
    }
    Ok((tags, truncated))
}

fn parse_remotes(bytes: &[u8]) -> Result<(Vec<GitRemote>, bool), GitStatusError> {
    let mut remotes: BTreeMap<String, GitRemote> = BTreeMap::new();
    let mut truncated = false;
    for line in bytes
        .split(|byte| *byte == b'\n')
        .filter(|line| !line.is_empty())
    {
        let line = std::str::from_utf8(line).map_err(|_| error("Git remote is not UTF-8"))?;
        let (name, remainder) = line
            .split_once('\t')
            .ok_or_else(|| error("Git remote record is malformed"))?;
        let (url, kind) = if let Some(url) = remainder.strip_suffix(" (fetch)") {
            (url, "fetch")
        } else if let Some(url) = remainder.strip_suffix(" (push)") {
            (url, "push")
        } else {
            return Err(error("Git remote direction is malformed"));
        };
        if !remotes.contains_key(name) && remotes.len() >= MAX_REMOTES {
            truncated = true;
            continue;
        }
        let remote = remotes.entry(name.into()).or_insert_with(|| GitRemote {
            name: name.into(),
            fetch_targets: Vec::new(),
            push_targets: Vec::new(),
        });
        let target = remote_authority(url);
        let targets = if kind == "fetch" {
            &mut remote.fetch_targets
        } else {
            &mut remote.push_targets
        };
        if !targets.contains(&target) {
            targets.push(target);
        }
    }
    Ok((remotes.into_values().collect(), truncated))
}

fn remote_authority(url: &str) -> String {
    if let Some((scheme, remainder)) = url.split_once("://") {
        let authority = remainder.split(['/', '?', '#']).next().unwrap_or_default();
        let host = authority
            .rsplit_once('@')
            .map_or(authority, |(_, host)| host);
        return if host.is_empty() {
            "remote".into()
        } else {
            format!("{scheme}://{host}")
        };
    }
    if Path::new(url).is_absolute()
        || url.starts_with("./")
        || url.starts_with("../")
        || url.starts_with("file:")
    {
        return "local".into();
    }
    let authority = url.split(':').next().unwrap_or(url);
    authority
        .rsplit_once('@')
        .map_or(authority, |(_, host)| host)
        .to_owned()
}

fn parse_worktrees(bytes: &[u8]) -> Result<(Vec<GitWorktree>, bool), GitStatusError> {
    let mut worktrees = Vec::new();
    let mut current: Option<GitWorktree> = None;
    let mut truncated = false;
    for record in bytes
        .split(|byte| *byte == 0)
        .filter(|record| !record.is_empty())
    {
        let (key, value) = record
            .iter()
            .position(|byte| *byte == b' ')
            .map_or((record, &[][..]), |position| {
                (&record[..position], &record[position + 1..])
            });
        match key {
            b"worktree" => {
                if let Some(worktree) = current.take() {
                    push_bounded(&mut worktrees, worktree, MAX_WORKTREES, &mut truncated);
                }
                current = Some(GitWorktree {
                    path: utf8(value, "Git worktree path")?,
                    ..GitWorktree::default()
                });
            }
            b"HEAD" => current_mut(&mut current)?.head_oid = Some(checked_oid(value)?),
            b"branch" => {
                current_mut(&mut current)?.branch = Some(
                    utf8(value, "Git worktree branch")?
                        .strip_prefix("refs/heads/")
                        .unwrap_or(&utf8(value, "Git worktree branch")?)
                        .to_owned(),
                )
            }
            b"detached" => current_mut(&mut current)?.detached = true,
            b"bare" => current_mut(&mut current)?.bare = true,
            b"locked" => {
                current_mut(&mut current)?.locked_reason = Some(utf8(value, "Git lock reason")?)
            }
            b"prunable" => {
                current_mut(&mut current)?.prunable_reason = Some(utf8(value, "Git prune reason")?)
            }
            _ => {}
        }
    }
    if let Some(worktree) = current {
        push_bounded(&mut worktrees, worktree, MAX_WORKTREES, &mut truncated);
    }
    Ok((worktrees, truncated))
}

fn current_mut(current: &mut Option<GitWorktree>) -> Result<&mut GitWorktree, GitStatusError> {
    current
        .as_mut()
        .ok_or_else(|| error("Git worktree record is missing its path"))
}

fn push_bounded<T>(values: &mut Vec<T>, value: T, limit: usize, truncated: &mut bool) {
    if values.len() < limit {
        values.push(value);
    } else {
        *truncated = true;
    }
}

fn parse_log(bytes: &[u8]) -> Result<Vec<GitLogEntry>, GitStatusError> {
    let mut commits = Vec::new();
    let mut offset = 0_usize;
    while offset < bytes.len() {
        while offset < bytes.len() && bytes[offset].is_ascii_whitespace() {
            offset += 1;
        }
        if offset == bytes.len() {
            break;
        }
        let mut fields = Vec::with_capacity(6);
        for _ in 0..6 {
            let end = bytes[offset..]
                .iter()
                .position(|byte| *byte == 0)
                .map(|position| offset + position)
                .ok_or_else(|| error("Git log record is malformed"))?;
            fields.push(&bytes[offset..end]);
            offset = end + 1;
        }
        if bytes.get(offset) != Some(&0) {
            return Err(error("Git log record terminator is missing"));
        }
        offset += 1;
        commits.push(GitLogEntry {
            oid: checked_oid(fields[0])?,
            parents: split_oids(fields[1])?,
            author_name: utf8(fields[2], "Git author name")?,
            author_email: utf8(fields[3], "Git author email")?,
            author_time: parse_time(fields[4])?,
            subject: utf8(fields[5], "Git commit subject")?,
        });
    }
    Ok(commits)
}

fn first_record<'a>(
    bytes: &'a [u8],
    fields: usize,
    label: &str,
) -> Result<Vec<&'a [u8]>, GitStatusError> {
    let record = bytes
        .split(|byte| *byte == 0)
        .take(fields)
        .collect::<Vec<_>>();
    if record.len() != fields {
        return Err(error(format!("{label} is malformed")));
    }
    Ok(record)
}

fn parse_changed_paths(
    root: &Path,
    prefix: &str,
    bytes: &[u8],
) -> Result<(Vec<GitChangedPath>, bool), GitStatusError> {
    let records = bytes
        .split(|byte| *byte == 0)
        .filter(|record| !record.is_empty())
        .collect::<Vec<_>>();
    let mut changed = Vec::new();
    let mut candidates = Vec::new();
    let mut index = 0_usize;
    while index < records.len() {
        let status = utf8(records[index], "Git changed-path status")?;
        index += 1;
        let rename = status.starts_with('R') || status.starts_with('C');
        let original = if rename {
            let path = records
                .get(index)
                .ok_or_else(|| error("Git rename source is missing"))?;
            index += 1;
            Some(project_relative(prefix, path)?)
        } else {
            None
        };
        let path = records
            .get(index)
            .ok_or_else(|| error("Git changed path is missing"))?;
        index += 1;
        let path = project_relative(prefix, path)?;
        if is_sensitive_path(Path::new(&path))
            || original
                .as_deref()
                .is_some_and(|path| is_sensitive_path(Path::new(path)))
        {
            continue;
        }
        candidates.push(path.clone());
        changed.push(GitChangedPath {
            status,
            path,
            original_path: original,
        });
    }
    let ignored = ignored_paths(root, &candidates);
    changed.retain(|path| !ignored.contains(&path.path));
    let truncated = changed.len() > MAX_CHANGED_PATHS;
    changed.truncate(MAX_CHANGED_PATHS);
    Ok((changed, truncated))
}

fn project_relative(prefix: &str, path: &[u8]) -> Result<String, GitStatusError> {
    let path = utf8(path, "Git changed path")?;
    let Some(relative) = path.strip_prefix(prefix) else {
        return Err(error("Git query path is outside the opened project"));
    };
    normalize_project_path(relative)
}

fn diff_base_args(scope: &str, oid: Option<&str>) -> Vec<String> {
    match scope {
        "worktree" => vec!["diff".into()],
        "staged" => vec!["diff".into(), "--cached".into()],
        "commit" => vec![
            "show".into(),
            "--format=".into(),
            "--first-parent".into(),
            oid.expect("validated commit OID").into(),
        ],
        _ => unreachable!("validated diff scope"),
    }
}

fn top_pathspec(path: &str) -> String {
    format!(":(top){path}")
}

fn count_patch_lines(patch: &str) -> (usize, usize) {
    let mut additions = 0_usize;
    let mut deletions = 0_usize;
    for line in patch.lines() {
        if line.starts_with("+++") || line.starts_with("---") {
            continue;
        }
        if line.starts_with('+') {
            additions += 1;
        } else if line.starts_with('-') {
            deletions += 1;
        }
    }
    (additions, deletions)
}

fn normalize_project_path(value: &str) -> Result<String, GitStatusError> {
    if value.is_empty() || value.contains('\\') || Path::new(value).is_absolute() {
        return Err(error("Git query path is invalid"));
    }
    let mut segments = Vec::new();
    for component in Path::new(value).components() {
        match component {
            Component::Normal(segment) => segments.push(
                segment
                    .to_str()
                    .ok_or_else(|| error("Git query path is not UTF-8"))?,
            ),
            Component::CurDir => {}
            Component::ParentDir | Component::RootDir | Component::Prefix(_) => {
                return Err(error("Git query path escapes the project"))
            }
        }
    }
    if segments.is_empty() {
        return Err(error("Git query path is empty"));
    }
    Ok(segments.join("/"))
}

fn split_oids(bytes: &[u8]) -> Result<Vec<String>, GitStatusError> {
    let text = std::str::from_utf8(bytes).map_err(|_| error("Git parent OIDs are invalid"))?;
    if text.is_empty() {
        return Ok(Vec::new());
    }
    text.split_ascii_whitespace()
        .map(|oid| {
            validate_oid(oid)?;
            Ok(oid.into())
        })
        .collect()
}

fn checked_oid(bytes: &[u8]) -> Result<String, GitStatusError> {
    let oid = utf8(trim_ascii(bytes), "Git object ID")?;
    validate_oid(&oid)?;
    Ok(oid)
}

fn validate_oid(oid: &str) -> Result<(), GitStatusError> {
    if !matches!(oid.len(), 40 | 64)
        || !oid
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
        return Err(error("Git query requires a full lowercase object ID"));
    }
    Ok(())
}

fn parse_time(bytes: &[u8]) -> Result<i64, GitStatusError> {
    std::str::from_utf8(bytes)
        .map_err(|_| error("Git timestamp is invalid"))?
        .parse()
        .map_err(|_| error("Git timestamp is invalid"))
}

fn utf8(bytes: &[u8], label: &str) -> Result<String, GitStatusError> {
    std::str::from_utf8(bytes)
        .map(str::to_owned)
        .map_err(|_| error(format!("{label} is not UTF-8")))
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

fn require_success(
    output: &crate::git_status::GitOutput,
    message: &str,
) -> Result<(), GitStatusError> {
    if output.success {
        Ok(())
    } else {
        Err(error(message))
    }
}

fn error(message: impl Into<String>) -> GitStatusError {
    GitStatusError {
        code: -32042,
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;
    use std::process::Command;
    use std::sync::atomic::{AtomicU64, Ordering};

    static SEQUENCE: AtomicU64 = AtomicU64::new(0);

    fn root() -> (PathBuf, PathBuf) {
        let root = std::env::temp_dir().join(format!(
            "aegisy-git-query-{}-{}",
            std::process::id(),
            SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ));
        let project = root.join("project");
        fs::create_dir_all(&project).unwrap();
        (
            root.canonicalize().unwrap(),
            project.canonicalize().unwrap(),
        )
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

    fn commit_all(root: &Path, message: &str) -> String {
        git(root, &["add", "."], true);
        git(root, &["commit", "-q", "-m", message], true);
        String::from_utf8(git(root, &["rev-parse", "HEAD"], true))
            .unwrap()
            .trim()
            .into()
    }

    #[test]
    fn queries_overview_log_commit_and_project_scoped_diffs_without_remote_secrets() {
        let (root, project) = root();
        git(&root, &["init", "-q"], true);
        git(&root, &["config", "user.name", "Aegisy Test"], true);
        git(&root, &["config", "user.email", "test@aegisy.local"], true);
        fs::write(project.join("visible.txt"), "one\n").unwrap();
        fs::write(project.join(".env"), "TOKEN=initial\n").unwrap();
        fs::write(root.join("outside.txt"), "outside-one\n").unwrap();
        let first = commit_all(&root, "first");
        fs::write(project.join("visible.txt"), "two\n").unwrap();
        let second = commit_all(&root, "second");
        fs::write(project.join("visible.txt"), "three\n").unwrap();
        fs::write(project.join(".env"), "TOKEN=committed-secret\n").unwrap();
        fs::write(root.join("outside.txt"), "outside-three\n").unwrap();
        let third = commit_all(&root, "third");
        git(&root, &["tag", "v1", &first], true);
        git(
            &root,
            &[
                "remote",
                "add",
                "origin",
                "https://user:embedded-secret@example.invalid/repo.git",
            ],
            true,
        );

        let overview = overview(&project).unwrap();
        assert_eq!(overview.schema_version, QUERY_SCHEMA_VERSION);
        assert!(overview.branches.iter().any(|branch| branch.current));
        assert!(overview.tags.iter().any(|tag| tag.name == "v1"));
        assert_eq!(overview.remotes.len(), 1);
        assert_eq!(overview.remotes[0].name, "origin");
        assert_eq!(
            overview.remotes[0].fetch_targets,
            vec!["https://example.invalid"]
        );
        let serialized = serde_json::to_string(&overview).unwrap();
        assert!(!serialized.contains("embedded-secret"));
        assert_eq!(overview.worktrees.len(), 1);

        let first_page = log(&project, 2, None).unwrap();
        assert_eq!(first_page.commits.len(), 2);
        assert_eq!(first_page.commits[0].oid, third);
        assert_eq!(first_page.commits[1].oid, second);
        let second_page = log(&project, 2, first_page.next_cursor.as_deref()).unwrap();
        assert_eq!(second_page.commits.len(), 1);
        assert_eq!(second_page.commits[0].oid, first);

        let detail = commit(&project, &third).unwrap();
        assert_eq!(detail.subject, "third");
        assert!(
            detail
                .changed_paths
                .iter()
                .any(|path| path.path == "visible.txt"),
            "{:?}",
            detail.changed_paths
        );
        assert!(!detail
            .changed_paths
            .iter()
            .any(|path| path.path == ".env" || path.path.contains("outside")));
        let committed = diff(&project, "commit", Some(&third), None).unwrap();
        assert!(committed.patch.contains("three"));
        assert!(!committed.patch.contains("committed-secret"));
        assert!(!committed.patch.contains("outside-three"));

        fs::write(project.join("visible.txt"), "worktree\n").unwrap();
        fs::write(project.join(".env"), "TOKEN=worktree-secret\n").unwrap();
        fs::write(root.join("outside.txt"), "outside-worktree\n").unwrap();
        let worktree = diff(&project, "worktree", None, None).unwrap();
        assert_eq!(worktree.paths, vec!["visible.txt"]);
        assert!(worktree.patch.contains("worktree"));
        assert!(!worktree.patch.contains("worktree-secret"));
        assert!(!worktree.patch.contains("outside-worktree"));

        git(&root, &["add", "project/visible.txt"], true);
        let staged = diff(&project, "staged", None, Some("visible.txt")).unwrap();
        assert_eq!(staged.paths, vec!["visible.txt"]);
        assert!(staged.additions > 0 && staged.deletions > 0);

        git(&root, &["reset", "--hard", "-q"], true);
        git(&root, &["mv", "project/.env", "project/leaked.txt"], true);
        let sensitive_rename = diff(&project, "staged", None, None).unwrap();
        assert!(sensitive_rename.paths.is_empty());
        assert!(sensitive_rename.patch.is_empty());

        assert!(log(&project, 0, None).is_err());
        assert!(commit(&project, "--all").is_err());
        assert!(diff(&project, "commit", Some("HEAD"), None).is_err());
        assert!(diff(&project, "worktree", None, Some("../outside.txt")).is_err());
        fs::remove_dir_all(root).unwrap();
    }
}
