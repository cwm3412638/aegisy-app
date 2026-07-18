use crate::workspace::is_sensitive_path;
use serde::{Deserialize, Serialize};
use std::collections::BTreeSet;
use std::fs;
use std::path::{Component, Path, PathBuf};

const PROFILE_SCHEMA_VERSION: &str = "permission-profile/0.1";
const MAX_PROFILE_ID_BYTES: usize = 128;
const MAX_ROOTS: usize = 16;
const MAX_DENIED_GLOBS: usize = 128;
const MAX_COMMANDS: usize = 128;
const MAX_HOSTS: usize = 128;
const MAX_EXTENSION_IDS: usize = 128;

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum PermissionProfileKind {
    Chat,
    ReadOnly,
    WorkspaceWrite,
    Developer,
    FullAccess,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum FilesystemAccess {
    Read,
    Write,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum CommandAccess {
    None,
    ReadOnly,
    Developer,
    Full,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum NetworkAccess {
    None,
    Allowlisted,
    Full,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PermissionRoot {
    pub path: String,
    pub access: FilesystemAccess,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct FilesystemPolicy {
    pub roots: Vec<PermissionRoot>,
    pub denied_globs: Vec<String>,
    pub follow_symlinks: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct CommandPolicy {
    pub access: CommandAccess,
    pub allowed_executables: Vec<String>,
    pub denied_patterns: Vec<String>,
    pub allow_shell: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct NetworkPolicy {
    pub access: NetworkAccess,
    pub allowed_hosts: Vec<String>,
    pub allow_redirects: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ExtensionPolicy {
    pub allow_mcp: bool,
    pub allow_plugins: bool,
    pub allowed_ids: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct BrowserPolicy {
    pub allow_computer_use: bool,
    pub allowed_hosts: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PermissionProfile {
    pub schema_version: String,
    pub profile_id: String,
    pub kind: PermissionProfileKind,
    pub filesystem: FilesystemPolicy,
    pub command: CommandPolicy,
    pub network: NetworkPolicy,
    pub extensions: ExtensionPolicy,
    pub browser: BrowserPolicy,
    pub allow_background: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ManagedPermissionPolicy {
    pub schema_version: String,
    pub allowed_filesystem_roots: Vec<PermissionRoot>,
    pub filesystem_roots_unrestricted: bool,
    pub denied_globs: Vec<String>,
    pub command_access: CommandAccess,
    pub allowed_executables: Vec<String>,
    pub denied_command_patterns: Vec<String>,
    pub network_access: NetworkAccess,
    pub allowed_hosts: Vec<String>,
    pub allow_mcp: bool,
    pub allow_plugins: bool,
    pub allow_computer_use: bool,
    pub allow_background: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct EffectivePermissionPolicy {
    pub schema_version: String,
    pub profile_id: String,
    pub profile_kind: PermissionProfileKind,
    pub filesystem: FilesystemPolicy,
    pub command: CommandPolicy,
    pub network: NetworkPolicy,
    pub extensions: ExtensionPolicy,
    pub browser: BrowserPolicy,
    pub allow_background: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PermissionDecision {
    Allowed,
    Denied { reason: String },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PermissionPolicyError {
    pub message: String,
}

impl PermissionProfile {
    pub fn for_project(
        profile_id: impl Into<String>,
        kind: PermissionProfileKind,
        project_root: &Path,
    ) -> Result<Self, PermissionPolicyError> {
        let root = canonical_directory(project_root)?;
        let profile_id = profile_id.into();
        validate_identifier(&profile_id, "permission profile ID")?;
        let (filesystem, command, network, extensions, browser, allow_background) = match kind {
            PermissionProfileKind::Chat => (
                FilesystemPolicy {
                    roots: Vec::new(),
                    denied_globs: Vec::new(),
                    follow_symlinks: false,
                },
                CommandPolicy::none(),
                NetworkPolicy::none(),
                ExtensionPolicy::none(),
                BrowserPolicy::none(),
                false,
            ),
            PermissionProfileKind::ReadOnly => (
                FilesystemPolicy {
                    roots: vec![PermissionRoot {
                        path: root.to_string_lossy().into_owned(),
                        access: FilesystemAccess::Read,
                    }],
                    denied_globs: Vec::new(),
                    follow_symlinks: false,
                },
                CommandPolicy::none(),
                NetworkPolicy::none(),
                ExtensionPolicy::none(),
                BrowserPolicy::none(),
                false,
            ),
            PermissionProfileKind::WorkspaceWrite => (
                FilesystemPolicy {
                    roots: vec![PermissionRoot {
                        path: root.to_string_lossy().into_owned(),
                        access: FilesystemAccess::Write,
                    }],
                    denied_globs: Vec::new(),
                    follow_symlinks: false,
                },
                CommandPolicy::none(),
                NetworkPolicy::none(),
                ExtensionPolicy::none(),
                BrowserPolicy::none(),
                false,
            ),
            PermissionProfileKind::Developer => (
                FilesystemPolicy {
                    roots: vec![PermissionRoot {
                        path: root.to_string_lossy().into_owned(),
                        access: FilesystemAccess::Write,
                    }],
                    denied_globs: Vec::new(),
                    follow_symlinks: false,
                },
                CommandPolicy {
                    access: CommandAccess::Developer,
                    allowed_executables: vec![
                        "cargo".into(),
                        "cmake".into(),
                        "git".into(),
                        "node".into(),
                        "npm".into(),
                        "python".into(),
                        "pytest".into(),
                        "rustc".into(),
                    ],
                    denied_patterns: Vec::new(),
                    allow_shell: false,
                },
                NetworkPolicy::none(),
                ExtensionPolicy::none(),
                BrowserPolicy::none(),
                true,
            ),
            PermissionProfileKind::FullAccess => (
                FilesystemPolicy {
                    roots: vec![PermissionRoot {
                        path: root.to_string_lossy().into_owned(),
                        access: FilesystemAccess::Write,
                    }],
                    denied_globs: Vec::new(),
                    follow_symlinks: false,
                },
                CommandPolicy {
                    access: CommandAccess::Full,
                    allowed_executables: Vec::new(),
                    denied_patterns: Vec::new(),
                    allow_shell: false,
                },
                NetworkPolicy {
                    access: NetworkAccess::Full,
                    allowed_hosts: Vec::new(),
                    allow_redirects: false,
                },
                ExtensionPolicy {
                    allow_mcp: true,
                    allow_plugins: true,
                    allowed_ids: Vec::new(),
                },
                BrowserPolicy {
                    allow_computer_use: true,
                    allowed_hosts: Vec::new(),
                },
                true,
            ),
        };
        let profile = Self {
            schema_version: PROFILE_SCHEMA_VERSION.into(),
            profile_id,
            kind,
            filesystem,
            command,
            network,
            extensions,
            browser,
            allow_background,
        };
        profile.validate()?;
        Ok(profile)
    }

    pub fn validate(&self) -> Result<(), PermissionPolicyError> {
        if self.schema_version != PROFILE_SCHEMA_VERSION {
            return Err(error("permission profile schema version is unsupported"));
        }
        validate_identifier(&self.profile_id, "permission profile ID")?;
        validate_filesystem_policy(&self.filesystem)?;
        validate_command_policy(&self.command)?;
        validate_network_policy(&self.network)?;
        validate_extension_policy(&self.extensions)?;
        validate_browser_policy(&self.browser)?;
        Ok(())
    }
}

impl ManagedPermissionPolicy {
    pub fn permissive() -> Self {
        Self {
            schema_version: PROFILE_SCHEMA_VERSION.into(),
            allowed_filesystem_roots: Vec::new(),
            filesystem_roots_unrestricted: true,
            denied_globs: Vec::new(),
            command_access: CommandAccess::Full,
            allowed_executables: Vec::new(),
            denied_command_patterns: Vec::new(),
            network_access: NetworkAccess::Full,
            allowed_hosts: Vec::new(),
            allow_mcp: true,
            allow_plugins: true,
            allow_computer_use: true,
            allow_background: true,
        }
    }

    pub fn validate(&self) -> Result<(), PermissionPolicyError> {
        if self.schema_version != PROFILE_SCHEMA_VERSION {
            return Err(error("managed permission policy schema is unsupported"));
        }
        if self.allowed_filesystem_roots.len() > MAX_ROOTS
            || self.denied_globs.len() > MAX_DENIED_GLOBS
            || self.allowed_executables.len() > MAX_COMMANDS
            || self.denied_command_patterns.len() > MAX_COMMANDS
            || self.allowed_hosts.len() > MAX_HOSTS
        {
            return Err(error("managed permission policy exceeds limits"));
        }
        validate_roots(&self.allowed_filesystem_roots)?;
        validate_globs(&self.denied_globs, "managed denied glob")?;
        validate_executables(&self.allowed_executables)?;
        validate_globs(&self.denied_command_patterns, "managed command pattern")?;
        validate_hosts(&self.allowed_hosts)?;
        Ok(())
    }
}

impl EffectivePermissionPolicy {
    pub fn from_profile(
        profile: &PermissionProfile,
        managed: &ManagedPermissionPolicy,
    ) -> Result<Self, PermissionPolicyError> {
        profile.validate()?;
        managed.validate()?;
        let filesystem_roots = intersect_roots(
            &profile.filesystem.roots,
            &managed.allowed_filesystem_roots,
            managed.filesystem_roots_unrestricted,
        )?;
        let command_access =
            restrict_command_access(profile.command.access, managed.command_access);
        let network_access =
            restrict_network_access(profile.network.access, managed.network_access);
        let command_executables = intersect_names(
            profile.command.access,
            &profile.command.allowed_executables,
            managed.command_access,
            &managed.allowed_executables,
        );
        let network_hosts = intersect_hosts(
            profile.network.access,
            &profile.network.allowed_hosts,
            managed.network_access,
            &managed.allowed_hosts,
        );
        let extension_ids = profile
            .extensions
            .allowed_ids
            .iter()
            .filter(|id| managed.allow_plugins || !id.starts_with("plugin:"))
            .filter(|id| managed.allow_mcp || !id.starts_with("mcp:"))
            .cloned()
            .collect();
        Ok(Self {
            schema_version: "effective-permission-policy/0.1".into(),
            profile_id: profile.profile_id.clone(),
            profile_kind: profile.kind,
            filesystem: FilesystemPolicy {
                roots: filesystem_roots,
                denied_globs: union_strings(
                    &profile.filesystem.denied_globs,
                    &managed.denied_globs,
                ),
                follow_symlinks: profile.filesystem.follow_symlinks,
            },
            command: CommandPolicy {
                access: command_access,
                allowed_executables: command_executables,
                denied_patterns: union_strings(
                    &profile.command.denied_patterns,
                    &managed.denied_command_patterns,
                ),
                allow_shell: profile.command.allow_shell && command_access == CommandAccess::Full,
            },
            network: NetworkPolicy {
                access: network_access,
                allowed_hosts: network_hosts,
                allow_redirects: profile.network.allow_redirects
                    && network_access != NetworkAccess::None,
            },
            extensions: ExtensionPolicy {
                allow_mcp: profile.extensions.allow_mcp && managed.allow_mcp,
                allow_plugins: profile.extensions.allow_plugins && managed.allow_plugins,
                allowed_ids: extension_ids,
            },
            browser: BrowserPolicy {
                allow_computer_use: profile.browser.allow_computer_use
                    && managed.allow_computer_use,
                allowed_hosts: intersect_host_sets(
                    &profile.browser.allowed_hosts,
                    &managed.allowed_hosts,
                ),
            },
            allow_background: profile.allow_background && managed.allow_background,
        })
    }

    pub fn check_path(&self, path: &Path, access: FilesystemAccess) -> PermissionDecision {
        let Ok(canonical) = canonical_existing_path(path, self.filesystem.follow_symlinks) else {
            return denied("path is unavailable or symlinked");
        };
        for root in &self.filesystem.roots {
            let Ok(root_path) = canonical_directory(Path::new(&root.path)) else {
                continue;
            };
            let Some(relative) = canonical.strip_prefix(&root_path).ok() else {
                continue;
            };
            if is_sensitive_path(relative) || matches_globs(relative, &self.filesystem.denied_globs)
            {
                return denied("path is denied by sensitive or managed policy");
            }
            if access == FilesystemAccess::Write && root.access != FilesystemAccess::Write {
                continue;
            }
            return PermissionDecision::Allowed;
        }
        denied("path is outside the effective filesystem roots")
    }

    pub fn check_command(&self, command: &str, cwd: &Path) -> PermissionDecision {
        if command.trim().is_empty() {
            return denied("command is empty");
        }
        if !matches!(
            self.check_path(cwd, FilesystemAccess::Read),
            PermissionDecision::Allowed
        ) {
            return denied("command cwd is outside the effective filesystem roots");
        }
        if self.command.access == CommandAccess::None {
            return denied("commands are disabled by the effective policy");
        }
        if !self.command.allow_shell && shell_wrapper(command) {
            return denied("shell wrappers are disabled by the effective policy");
        }
        if self
            .command
            .denied_patterns
            .iter()
            .any(|pattern| wildcard_match(pattern, command))
        {
            return denied("command matches a managed deny pattern");
        }
        let executable = command
            .split_whitespace()
            .next()
            .and_then(|value| Path::new(value).file_name())
            .map(|value| value.to_string_lossy().to_ascii_lowercase());
        if self.command.access != CommandAccess::Full
            && executable.is_none_or(|value| {
                !self
                    .command
                    .allowed_executables
                    .iter()
                    .any(|allowed| allowed == &value)
            })
        {
            return denied("executable is not in the effective allowlist");
        }
        PermissionDecision::Allowed
    }

    pub fn check_git_action(&self, action: &str, risk_class: &str) -> PermissionDecision {
        if !matches!(action, "start" | "abort" | "continue") {
            return denied("Git action is not recognized by the policy engine");
        }
        if !matches!(risk_class, "medium" | "high") {
            return denied("Git action risk class is invalid");
        }
        if !matches!(
            self.command.access,
            CommandAccess::Developer | CommandAccess::Full
        ) {
            return denied("Git mutation requires Developer or Full Access policy");
        }
        if self.filesystem.roots.is_empty()
            || !self
                .filesystem
                .roots
                .iter()
                .any(|root| root.access == FilesystemAccess::Write)
        {
            return denied("Git mutation has no writable effective root");
        }
        PermissionDecision::Allowed
    }

    pub fn check_host(&self, host: &str) -> PermissionDecision {
        let host = normalize_host(host);
        if host.is_empty() || host.contains('@') || host.contains('/') {
            return denied("network host is invalid");
        }
        match self.network.access {
            NetworkAccess::None => denied("network access is disabled by the effective policy"),
            NetworkAccess::Full => PermissionDecision::Allowed,
            NetworkAccess::Allowlisted => {
                if host_matches_any(&host, &self.network.allowed_hosts) {
                    PermissionDecision::Allowed
                } else {
                    denied("network host is not allowlisted")
                }
            }
        }
    }

    pub fn check_extension(&self, extension_id: &str, mcp: bool) -> PermissionDecision {
        let allowed_kind = if mcp {
            self.extensions.allow_mcp
        } else {
            self.extensions.allow_plugins
        };
        if !allowed_kind {
            return denied("extension kind is disabled by the effective policy");
        }
        if self.extensions.allowed_ids.is_empty()
            || self
                .extensions
                .allowed_ids
                .iter()
                .any(|id| id == extension_id)
        {
            PermissionDecision::Allowed
        } else {
            denied("extension is not allowlisted")
        }
    }

    pub fn check_browser_host(&self, host: &str) -> PermissionDecision {
        if !self.browser.allow_computer_use {
            return denied("browser/computer use is disabled by the effective policy");
        }
        if self.browser.allowed_hosts.is_empty()
            || host_matches_any(&normalize_host(host), &self.browser.allowed_hosts)
        {
            PermissionDecision::Allowed
        } else {
            denied("browser host is not allowlisted")
        }
    }
}

impl CommandPolicy {
    fn none() -> Self {
        Self {
            access: CommandAccess::None,
            allowed_executables: Vec::new(),
            denied_patterns: Vec::new(),
            allow_shell: false,
        }
    }
}

impl NetworkPolicy {
    fn none() -> Self {
        Self {
            access: NetworkAccess::None,
            allowed_hosts: Vec::new(),
            allow_redirects: false,
        }
    }
}

impl ExtensionPolicy {
    fn none() -> Self {
        Self {
            allow_mcp: false,
            allow_plugins: false,
            allowed_ids: Vec::new(),
        }
    }
}

impl BrowserPolicy {
    fn none() -> Self {
        Self {
            allow_computer_use: false,
            allowed_hosts: Vec::new(),
        }
    }
}

fn intersect_roots(
    requested: &[PermissionRoot],
    managed: &[PermissionRoot],
    managed_unrestricted: bool,
) -> Result<Vec<PermissionRoot>, PermissionPolicyError> {
    if managed_unrestricted {
        return Ok(requested.to_vec());
    }
    let mut result = Vec::new();
    for requested_root in requested {
        let requested_path = canonical_directory(Path::new(&requested_root.path))?;
        for managed_root in managed {
            let managed_path = canonical_directory(Path::new(&managed_root.path))?;
            if requested_path.starts_with(&managed_path) {
                let access = if requested_root.access == FilesystemAccess::Write
                    && managed_root.access == FilesystemAccess::Write
                {
                    FilesystemAccess::Write
                } else {
                    FilesystemAccess::Read
                };
                result.push(PermissionRoot {
                    path: requested_path.to_string_lossy().into_owned(),
                    access,
                });
            }
        }
    }
    result.sort_by(|left, right| left.path.cmp(&right.path));
    result.dedup();
    Ok(result)
}

fn intersect_names(
    requested_access: CommandAccess,
    requested: &[String],
    managed_access: CommandAccess,
    managed: &[String],
) -> Vec<String> {
    if restrict_command_access(requested_access, managed_access) == CommandAccess::None {
        return Vec::new();
    }
    if managed.is_empty() {
        return requested.to_vec();
    }
    if requested.is_empty() {
        return managed.to_vec();
    }
    requested
        .iter()
        .filter(|name| managed.iter().any(|managed_name| managed_name == *name))
        .cloned()
        .collect()
}

fn intersect_hosts(
    requested_access: NetworkAccess,
    requested: &[String],
    managed_access: NetworkAccess,
    managed: &[String],
) -> Vec<String> {
    if restrict_network_access(requested_access, managed_access) != NetworkAccess::Allowlisted {
        return Vec::new();
    }
    if managed.is_empty() {
        return requested.to_vec();
    }
    if requested.is_empty() {
        return managed.to_vec();
    }
    intersect_host_sets(requested, managed)
}

fn intersect_host_sets(left: &[String], right: &[String]) -> Vec<String> {
    if left.is_empty() {
        return right.to_vec();
    }
    if right.is_empty() {
        return left.to_vec();
    }
    left.iter()
        .filter(|host| right.iter().any(|other| host_pattern_subset(host, other)))
        .cloned()
        .collect()
}

fn host_pattern_subset(child: &str, parent: &str) -> bool {
    let child = normalize_host(child);
    let parent = normalize_host(parent);
    if parent == "*" {
        return true;
    }
    if let Some(parent_suffix) = parent.strip_prefix("*.") {
        return child.ends_with(parent_suffix)
            && child.len() > parent_suffix.len()
            && child.as_bytes()[child.len() - parent_suffix.len() - 1] == b'.';
    }
    child == parent
}

fn restrict_command_access(left: CommandAccess, right: CommandAccess) -> CommandAccess {
    use CommandAccess::*;
    match (left, right) {
        (None, _) | (_, None) => None,
        (Full, other) | (other, Full) => other,
        (Developer, Developer) => Developer,
        _ => ReadOnly,
    }
}

fn restrict_network_access(left: NetworkAccess, right: NetworkAccess) -> NetworkAccess {
    use NetworkAccess::*;
    match (left, right) {
        (None, _) | (_, None) => None,
        (Full, other) => other,
        (other, Full) => other,
        (Allowlisted, Allowlisted) => Allowlisted,
    }
}

fn union_strings(left: &[String], right: &[String]) -> Vec<String> {
    let mut values = BTreeSet::new();
    values.extend(left.iter().cloned());
    values.extend(right.iter().cloned());
    values.into_iter().collect()
}

fn validate_filesystem_policy(policy: &FilesystemPolicy) -> Result<(), PermissionPolicyError> {
    if policy.roots.len() > MAX_ROOTS || policy.denied_globs.len() > MAX_DENIED_GLOBS {
        return Err(error("filesystem policy exceeds limits"));
    }
    validate_roots(&policy.roots)?;
    validate_globs(&policy.denied_globs, "filesystem denied glob")
}

fn validate_command_policy(policy: &CommandPolicy) -> Result<(), PermissionPolicyError> {
    if policy.allowed_executables.len() > MAX_COMMANDS
        || policy.denied_patterns.len() > MAX_COMMANDS
    {
        return Err(error("command policy exceeds limits"));
    }
    validate_executables(&policy.allowed_executables)?;
    validate_globs(&policy.denied_patterns, "command deny pattern")
}

fn validate_network_policy(policy: &NetworkPolicy) -> Result<(), PermissionPolicyError> {
    if policy.allowed_hosts.len() > MAX_HOSTS {
        return Err(error("network policy exceeds limits"));
    }
    validate_hosts(&policy.allowed_hosts)
}

fn validate_extension_policy(policy: &ExtensionPolicy) -> Result<(), PermissionPolicyError> {
    if policy.allowed_ids.len() > MAX_EXTENSION_IDS {
        return Err(error("extension policy exceeds limits"));
    }
    for id in &policy.allowed_ids {
        validate_extension_id(id)?;
    }
    Ok(())
}

fn validate_browser_policy(policy: &BrowserPolicy) -> Result<(), PermissionPolicyError> {
    if policy.allowed_hosts.len() > MAX_HOSTS {
        return Err(error("browser policy exceeds limits"));
    }
    validate_hosts(&policy.allowed_hosts)
}

fn validate_roots(roots: &[PermissionRoot]) -> Result<(), PermissionPolicyError> {
    for root in roots {
        if root.path.is_empty() || !Path::new(&root.path).is_absolute() {
            return Err(error("permission root must be an absolute path"));
        }
        if !root.path.is_char_boundary(root.path.len()) {
            return Err(error("permission root is not valid UTF-8"));
        }
    }
    Ok(())
}

fn validate_globs(globs: &[String], label: &str) -> Result<(), PermissionPolicyError> {
    for glob in globs {
        if glob.is_empty() || glob.len() > 256 || glob.bytes().any(|byte| byte.is_ascii_control()) {
            return Err(error(format!("{label} is invalid")));
        }
    }
    Ok(())
}

fn validate_executables(executables: &[String]) -> Result<(), PermissionPolicyError> {
    for executable in executables {
        if executable.is_empty()
            || executable.len() > 128
            || executable.contains('/')
            || executable.bytes().any(|byte| !byte.is_ascii_graphic())
        {
            return Err(error("command executable allowlist entry is invalid"));
        }
    }
    Ok(())
}

fn validate_hosts(hosts: &[String]) -> Result<(), PermissionPolicyError> {
    for host in hosts {
        let normalized = normalize_host(host);
        if normalized.is_empty()
            || normalized.len() > 253
            || normalized.contains('@')
            || normalized.contains('/')
            || normalized.bytes().any(|byte| byte.is_ascii_control())
            || !normalized
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'*'))
        {
            return Err(error("network host allowlist entry is invalid"));
        }
    }
    Ok(())
}

fn canonical_directory(path: &Path) -> Result<PathBuf, PermissionPolicyError> {
    let canonical = path
        .canonicalize()
        .map_err(|_| error("permission root is unavailable"))?;
    if !canonical.is_dir() {
        return Err(error("permission root is not a directory"));
    }
    Ok(canonical)
}

fn canonical_existing_path(
    path: &Path,
    follow_symlinks: bool,
) -> Result<PathBuf, PermissionPolicyError> {
    if !follow_symlinks && contains_symlink_component(path)? {
        return Err(error("symlink traversal is disabled"));
    }
    path.canonicalize()
        .map_err(|_| error("path is unavailable"))
}

fn contains_symlink_component(path: &Path) -> Result<bool, PermissionPolicyError> {
    let mut current = PathBuf::new();
    for component in path.components() {
        match component {
            Component::Prefix(prefix) => current.push(prefix.as_os_str()),
            Component::RootDir => current.push(Path::new(std::path::MAIN_SEPARATOR_STR)),
            Component::CurDir => {}
            Component::ParentDir => current.push(".."),
            Component::Normal(name) => {
                current.push(name);
                if fs::symlink_metadata(&current)
                    .map_err(|_| error("path is unavailable"))?
                    .file_type()
                    .is_symlink()
                {
                    return Ok(true);
                }
            }
        }
    }
    Ok(false)
}

fn matches_globs(path: &Path, globs: &[String]) -> bool {
    let value = path.to_string_lossy().replace('\\', "/");
    globs.iter().any(|glob| wildcard_match(glob, &value))
}

fn wildcard_match(pattern: &str, value: &str) -> bool {
    let pattern = pattern.as_bytes();
    let value = value.as_bytes();
    let mut memo = vec![vec![None; value.len() + 1]; pattern.len() + 1];
    fn matches(
        pattern: &[u8],
        value: &[u8],
        pattern_index: usize,
        value_index: usize,
        memo: &mut [Vec<Option<bool>>],
    ) -> bool {
        if let Some(result) = memo[pattern_index][value_index] {
            return result;
        }
        let result = if pattern_index == pattern.len() {
            value_index == value.len()
        } else if pattern[pattern_index] == b'*' {
            let mut next_pattern = pattern_index + 1;
            while next_pattern < pattern.len() && pattern[next_pattern] == b'*' {
                next_pattern += 1;
            }
            let double_star = next_pattern > pattern_index + 1;
            matches(pattern, value, next_pattern, value_index, memo)
                || value_index < value.len()
                    && (double_star || value[value_index] != b'/')
                    && matches(pattern, value, pattern_index, value_index + 1, memo)
        } else if value_index < value.len()
            && (pattern[pattern_index] == b'?' || pattern[pattern_index] == value[value_index])
        {
            matches(pattern, value, pattern_index + 1, value_index + 1, memo)
        } else {
            false
        };
        memo[pattern_index][value_index] = Some(result);
        result
    }
    matches(pattern, value, 0, 0, &mut memo)
}

fn shell_wrapper(command: &str) -> bool {
    let executable = command
        .split_whitespace()
        .next()
        .and_then(|value| Path::new(value).file_name())
        .map(|value| value.to_string_lossy().to_ascii_lowercase());
    matches!(
        executable.as_deref(),
        Some("sh")
            | Some("bash")
            | Some("zsh")
            | Some("fish")
            | Some("cmd")
            | Some("cmd.exe")
            | Some("powershell")
            | Some("powershell.exe")
            | Some("pwsh")
            | Some("pwsh.exe")
    )
}

fn normalize_host(host: &str) -> String {
    host.trim().trim_end_matches('.').to_ascii_lowercase()
}

fn host_matches_any(host: &str, patterns: &[String]) -> bool {
    patterns
        .iter()
        .any(|pattern| host_pattern_subset(host, pattern))
}

fn denied(reason: &str) -> PermissionDecision {
    PermissionDecision::Denied {
        reason: reason.into(),
    }
}

fn validate_identifier(value: &str, label: &str) -> Result<(), PermissionPolicyError> {
    if value.is_empty()
        || value.len() > MAX_PROFILE_ID_BYTES
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.'))
    {
        return Err(error(format!("{label} is invalid")));
    }
    Ok(())
}

fn validate_extension_id(value: &str) -> Result<(), PermissionPolicyError> {
    if value.is_empty()
        || value.len() > MAX_PROFILE_ID_BYTES
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.' | b':'))
    {
        return Err(error("extension ID is invalid"));
    }
    Ok(())
}

fn error(message: impl Into<String>) -> PermissionPolicyError {
    PermissionPolicyError {
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs::{self, File};
    use std::sync::atomic::{AtomicU64, Ordering};

    static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

    struct TempDir {
        path: PathBuf,
    }

    impl TempDir {
        fn path(&self) -> &Path {
            &self.path
        }
    }

    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.path);
        }
    }

    fn tempdir() -> TempDir {
        let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let path = std::env::temp_dir().join(format!(
            "aegisy-permission-profile-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir(&path).unwrap();
        TempDir { path }
    }

    fn project() -> TempDir {
        let root = tempdir();
        fs::create_dir(root.path().join("src")).unwrap();
        File::create(root.path().join("src/main.rs")).unwrap();
        File::create(root.path().join(".env")).unwrap();
        root
    }

    #[test]
    fn built_in_profiles_have_expected_capabilities() {
        let root = project();
        let chat = PermissionProfile::for_project("chat", PermissionProfileKind::Chat, root.path())
            .unwrap();
        let read_only = PermissionProfile::for_project(
            "read-only",
            PermissionProfileKind::ReadOnly,
            root.path(),
        )
        .unwrap();
        let developer = PermissionProfile::for_project(
            "developer",
            PermissionProfileKind::Developer,
            root.path(),
        )
        .unwrap();
        assert!(chat.filesystem.roots.is_empty());
        assert_eq!(read_only.filesystem.roots[0].access, FilesystemAccess::Read);
        assert_eq!(developer.command.access, CommandAccess::Developer);
        assert!(developer.allow_background);
    }

    #[test]
    fn managed_policy_intersection_never_expands_profile() {
        let root = project();
        let profile = PermissionProfile::for_project(
            "developer",
            PermissionProfileKind::Developer,
            root.path(),
        )
        .unwrap();
        let managed = ManagedPermissionPolicy {
            schema_version: PROFILE_SCHEMA_VERSION.into(),
            allowed_filesystem_roots: vec![PermissionRoot {
                path: root.path().to_string_lossy().into_owned(),
                access: FilesystemAccess::Read,
            }],
            filesystem_roots_unrestricted: false,
            denied_globs: vec!["src/*".into()],
            command_access: CommandAccess::ReadOnly,
            allowed_executables: vec!["cargo".into()],
            denied_command_patterns: vec!["* test *".into()],
            network_access: NetworkAccess::None,
            allowed_hosts: Vec::new(),
            allow_mcp: false,
            allow_plugins: false,
            allow_computer_use: false,
            allow_background: false,
        };
        let effective = EffectivePermissionPolicy::from_profile(&profile, &managed).unwrap();
        assert_eq!(effective.filesystem.roots[0].access, FilesystemAccess::Read);
        assert_eq!(effective.command.access, CommandAccess::ReadOnly);
        assert!(!effective.allow_background);
        assert!(matches!(
            effective.check_path(&root.path().join("src/main.rs"), FilesystemAccess::Write),
            PermissionDecision::Denied { .. }
        ));
        assert!(matches!(
            effective.check_command("cargo test", root.path()),
            PermissionDecision::Denied { .. }
        ));
    }

    #[test]
    fn paths_deny_sensitive_files_symlinks_and_outside_roots() {
        let root = project();
        let profile = PermissionProfile::for_project(
            "workspace",
            PermissionProfileKind::WorkspaceWrite,
            root.path(),
        )
        .unwrap();
        let effective = EffectivePermissionPolicy::from_profile(
            &profile,
            &ManagedPermissionPolicy::permissive(),
        )
        .unwrap();
        let canonical_root = PathBuf::from(&effective.filesystem.roots[0].path);
        assert!(matches!(
            effective.check_path(&canonical_root.join("src/main.rs"), FilesystemAccess::Write),
            PermissionDecision::Allowed
        ));
        assert!(matches!(
            effective.check_path(&canonical_root.join(".env"), FilesystemAccess::Read),
            PermissionDecision::Denied { .. }
        ));
        let outside = tempdir();
        assert!(matches!(
            effective.check_path(outside.path(), FilesystemAccess::Read),
            PermissionDecision::Denied { .. }
        ));
        #[cfg(unix)]
        std::os::unix::fs::symlink(root.path().join("src/main.rs"), root.path().join("link"))
            .unwrap();
        #[cfg(unix)]
        assert!(matches!(
            effective.check_path(&root.path().join("link"), FilesystemAccess::Read),
            PermissionDecision::Denied { .. }
        ));
    }

    #[test]
    fn command_network_extension_and_browser_checks_are_explicit() {
        let root = project();
        let mut profile = PermissionProfile::for_project(
            "developer",
            PermissionProfileKind::Developer,
            root.path(),
        )
        .unwrap();
        profile.network = NetworkPolicy {
            access: NetworkAccess::Allowlisted,
            allowed_hosts: vec!["api.example.com".into(), "*.trusted.example".into()],
            allow_redirects: false,
        };
        profile.extensions = ExtensionPolicy {
            allow_mcp: true,
            allow_plugins: false,
            allowed_ids: vec!["mcp:filesystem".into()],
        };
        profile.browser = BrowserPolicy {
            allow_computer_use: true,
            allowed_hosts: vec!["docs.example.com".into()],
        };
        let effective = EffectivePermissionPolicy::from_profile(
            &profile,
            &ManagedPermissionPolicy::permissive(),
        )
        .unwrap();
        let canonical_root = PathBuf::from(&effective.filesystem.roots[0].path);
        assert!(matches!(
            effective.check_command("cargo check", &canonical_root),
            PermissionDecision::Allowed
        ));
        assert!(matches!(
            effective.check_command("sh -c cargo check", &canonical_root),
            PermissionDecision::Denied { .. }
        ));
        assert!(matches!(
            effective.check_host("api.example.com."),
            PermissionDecision::Allowed
        ));
        assert!(matches!(
            effective.check_host("evil.example.com"),
            PermissionDecision::Denied { .. }
        ));
        assert!(matches!(
            effective.check_extension("mcp:filesystem", true),
            PermissionDecision::Allowed
        ));
        assert!(matches!(
            effective.check_extension("plugin:unsafe", false),
            PermissionDecision::Denied { .. }
        ));
        assert!(matches!(
            effective.check_browser_host("docs.example.com"),
            PermissionDecision::Allowed
        ));
        assert!(matches!(
            effective.check_browser_host("evil.example.com"),
            PermissionDecision::Denied { .. }
        ));
    }

    #[test]
    fn wildcard_and_host_matching_are_boundary_aware() {
        assert!(wildcard_match("src/*", "src/main.rs"));
        assert!(!wildcard_match("src/*", "src/nested/main.rs"));
        assert!(host_pattern_subset(
            "api.trusted.example",
            "*.trusted.example"
        ));
        assert!(!host_pattern_subset(
            "trusted.example.evil",
            "*.trusted.example"
        ));
        assert!(!host_pattern_subset("trusted.example", "*.trusted.example"));
    }
}
