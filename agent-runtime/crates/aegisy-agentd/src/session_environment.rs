use serde::Serialize;
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::env;
use std::ffi::{OsStr, OsString};
use std::fmt;
use std::path::{Path, PathBuf};

const MAX_VARIABLES: usize = 128;
const MAX_TOOL_VARIABLES: usize = 32;
const MAX_VALUE_BYTES: usize = 4 * 1024;
const MAX_ENVIRONMENT_BYTES: usize = 32 * 1024;
const MAX_PATH_ENTRIES: usize = 128;

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct EnvironmentSummary {
    pub environment_id: String,
    pub variable_count: usize,
    pub inherited_count: usize,
    pub explicit_count: usize,
    pub masked_count: usize,
    pub path_entry_count: usize,
    pub explicit_variable_names: Vec<String>,
}

#[derive(Clone)]
pub struct SessionEnvironment {
    variables: BTreeMap<String, EnvironmentVariable>,
    summary: EnvironmentSummary,
}

impl fmt::Debug for SessionEnvironment {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("SessionEnvironment")
            .field("summary", &self.summary)
            .finish_non_exhaustive()
    }
}

#[derive(Clone)]
struct EnvironmentVariable {
    value: OsString,
    source: VariableSource,
}

#[derive(Clone, Copy)]
enum VariableSource {
    Inherited,
    Aegisy,
    Tool,
}

#[derive(Debug, Clone)]
pub struct ToolVariable {
    pub name: String,
    pub value: OsString,
}

impl ToolVariable {
    #[cfg(any(test, target_os = "macos", target_os = "windows"))]
    pub fn new(name: impl Into<String>, value: impl Into<OsString>) -> Self {
        Self {
            name: name.into(),
            value: value.into(),
        }
    }
}

#[derive(Debug)]
pub struct EnvironmentError {
    pub message: String,
}

pub struct ProcessEnvironment {
    variables: BTreeMap<String, EnvironmentVariable>,
    summary: EnvironmentSummary,
}

impl SessionEnvironment {
    pub fn build(session_id: &str, project_id: Option<&str>, mode: &str, root: &Path) -> Self {
        Self::build_from(session_id, project_id, mode, root, env::vars_os().collect())
    }

    fn build_from(
        session_id: &str,
        project_id: Option<&str>,
        mode: &str,
        root: &Path,
        source: Vec<(OsString, OsString)>,
    ) -> Self {
        let mut variables = BTreeMap::new();
        let mut masked_count = 0;
        let mut path_entry_count = 0;
        let mut environment_bytes = 0;

        for (raw_name, value) in source {
            let Some(name) = raw_name.to_str().map(normalize_name) else {
                continue;
            };
            if is_sensitive_name(&name) {
                masked_count += 1;
                continue;
            }
            if name == "PATH" {
                if let Some((path, count)) = sanitized_path(&value, root) {
                    environment_bytes += name.len() + os_value_bytes(&path).len();
                    path_entry_count = count;
                    variables.insert(
                        name,
                        EnvironmentVariable {
                            value: path,
                            source: VariableSource::Inherited,
                        },
                    );
                }
                continue;
            }
            if !is_inherited_name(&name)
                || variables.len() >= MAX_VARIABLES.saturating_sub(4)
                || os_value_bytes(&value).len() > MAX_VALUE_BYTES
                || !valid_inherited_value(&name, &value, root)
                || environment_bytes + name.len() + os_value_bytes(&value).len()
                    > MAX_ENVIRONMENT_BYTES
            {
                continue;
            }
            environment_bytes += name.len() + os_value_bytes(&value).len();
            variables.insert(
                name,
                EnvironmentVariable {
                    value,
                    source: VariableSource::Inherited,
                },
            );
        }

        insert_aegisy(&mut variables, "AEGISY_SESSION_ID", session_id);
        insert_aegisy(&mut variables, "AEGISY_SESSION_MODE", mode);
        if let Some(project_id) = project_id {
            insert_aegisy(&mut variables, "AEGISY_PROJECT_ID", project_id);
        }
        let summary = summarize(&variables, masked_count, path_entry_count);
        Self { variables, summary }
    }

    pub fn summary(&self) -> &EnvironmentSummary {
        &self.summary
    }

    pub fn for_tool(
        &self,
        tool_name: &str,
        variables: Vec<ToolVariable>,
    ) -> Result<ProcessEnvironment, EnvironmentError> {
        if !valid_tool_name(tool_name) {
            return Err(environment_error("invalid environment tool name"));
        }
        if variables.len() > MAX_TOOL_VARIABLES {
            return Err(environment_error(
                "tool environment variable limit exceeded",
            ));
        }
        let mut result = self.variables.clone();
        insert_explicit(
            &mut result,
            "AEGISY_TOOL_NAME",
            tool_name,
            VariableSource::Aegisy,
        );
        let mut tool_bytes = 0;
        for variable in variables {
            let name = normalize_name(&variable.name);
            let value_bytes = os_value_bytes(&variable.value);
            if !valid_variable_name(&name) {
                return Err(environment_error("invalid tool environment variable name"));
            }
            if name.starts_with("AEGISY_")
                || is_sensitive_name(&name)
                || is_dangerous_tool_name(&name)
            {
                return Err(environment_error(
                    "sensitive or reserved tool environment variable rejected",
                ));
            }
            if result.contains_key(&name) {
                return Err(environment_error(
                    "tool environment variable cannot override session state",
                ));
            }
            if value_bytes.len() > MAX_VALUE_BYTES {
                return Err(environment_error(
                    "tool environment variable value is too large",
                ));
            }
            tool_bytes += name.len() + value_bytes.len();
            if tool_bytes > MAX_ENVIRONMENT_BYTES {
                return Err(environment_error("tool environment byte limit exceeded"));
            }
            result.insert(
                name,
                EnvironmentVariable {
                    value: variable.value,
                    source: VariableSource::Tool,
                },
            );
        }
        if result.len() > MAX_VARIABLES {
            return Err(environment_error(
                "process environment variable limit exceeded",
            ));
        }
        let summary = summarize(
            &result,
            self.summary.masked_count,
            self.summary.path_entry_count,
        );
        Ok(ProcessEnvironment {
            variables: result,
            summary,
        })
    }
}

impl ProcessEnvironment {
    pub fn iter(&self) -> impl Iterator<Item = (&str, &OsStr)> {
        self.variables
            .iter()
            .map(|(name, variable)| (name.as_str(), variable.value.as_os_str()))
    }

    pub fn summary(&self) -> &EnvironmentSummary {
        &self.summary
    }
}

fn insert_aegisy(variables: &mut BTreeMap<String, EnvironmentVariable>, name: &str, value: &str) {
    insert_explicit(variables, name, value, VariableSource::Aegisy);
}

fn insert_explicit(
    variables: &mut BTreeMap<String, EnvironmentVariable>,
    name: &str,
    value: impl Into<OsString>,
    source: VariableSource,
) {
    variables.insert(
        name.into(),
        EnvironmentVariable {
            value: value.into(),
            source,
        },
    );
}

fn summarize(
    variables: &BTreeMap<String, EnvironmentVariable>,
    masked_count: usize,
    path_entry_count: usize,
) -> EnvironmentSummary {
    let mut inherited_count = 0;
    let mut explicit_names = BTreeSet::new();
    let mut hasher = Sha256::new();
    for (name, variable) in variables {
        let source = match variable.source {
            VariableSource::Inherited => {
                inherited_count += 1;
                b"inherited".as_slice()
            }
            VariableSource::Aegisy => {
                explicit_names.insert(name.clone());
                b"aegisy".as_slice()
            }
            VariableSource::Tool => {
                explicit_names.insert(name.clone());
                b"tool".as_slice()
            }
        };
        hasher.update(name.as_bytes());
        hasher.update([0]);
        hasher.update(source);
        hasher.update([0]);
        hasher.update(os_value_bytes(&variable.value));
        hasher.update([0xff]);
    }
    let explicit_variable_names = explicit_names.into_iter().collect::<Vec<_>>();
    EnvironmentSummary {
        environment_id: format!("environment:sha256:{:x}", hasher.finalize()),
        variable_count: variables.len(),
        inherited_count,
        explicit_count: explicit_variable_names.len(),
        masked_count,
        path_entry_count,
        explicit_variable_names,
    }
}

fn sanitized_path(value: &OsStr, root: &Path) -> Option<(OsString, usize)> {
    let mut entries = Vec::new();
    let mut seen = BTreeSet::new();
    for entry in env::split_paths(value).take(MAX_PATH_ENTRIES) {
        if !entry.is_absolute() || path_is_within(&entry, root) {
            continue;
        }
        let Ok(canonical) = entry.canonicalize() else {
            continue;
        };
        if !canonical.is_dir() || path_is_within(&canonical, root) {
            continue;
        }
        let key = path_key(&canonical);
        if seen.insert(key) {
            entries.push(canonical);
        }
    }
    for fallback in platform_path_fallbacks() {
        if entries.len() >= MAX_PATH_ENTRIES {
            break;
        }
        let Ok(canonical) = fallback.canonicalize() else {
            continue;
        };
        if !canonical.is_dir() || path_is_within(&canonical, root) {
            continue;
        }
        if seen.insert(path_key(&canonical)) {
            entries.push(canonical);
        }
    }
    let count = entries.len();
    if count == 0 {
        return None;
    }
    let joined = env::join_paths(entries).ok()?;
    if os_value_bytes(&joined).len() > MAX_ENVIRONMENT_BYTES {
        return None;
    }
    Some((joined, count))
}

fn platform_path_fallbacks() -> Vec<PathBuf> {
    #[cfg(target_os = "macos")]
    {
        [
            "/opt/homebrew/bin",
            "/usr/local/bin",
            "/usr/bin",
            "/bin",
            "/usr/sbin",
            "/sbin",
        ]
        .into_iter()
        .map(PathBuf::from)
        .collect()
    }
    #[cfg(target_os = "windows")]
    {
        let mut paths = Vec::new();
        if let Some(root) = env::var_os("SystemRoot").map(PathBuf::from) {
            paths.extend([root.join("System32"), root]);
        }
        paths
    }
    #[cfg(not(any(target_os = "macos", target_os = "windows")))]
    {
        vec![PathBuf::from("/usr/bin"), PathBuf::from("/bin")]
    }
}

fn is_inherited_name(name: &str) -> bool {
    const COMMON: &[&str] = &["LANG", "LC_ALL", "LC_CTYPE", "TZ"];
    if COMMON.contains(&name) {
        return true;
    }
    #[cfg(target_os = "macos")]
    {
        ["HOME", "USER", "LOGNAME", "TMPDIR"].contains(&name)
    }
    #[cfg(target_os = "windows")]
    {
        [
            "SYSTEMROOT",
            "WINDIR",
            "SYSTEMDRIVE",
            "COMSPEC",
            "PATHEXT",
            "TEMP",
            "TMP",
            "USERPROFILE",
            "HOMEDRIVE",
            "HOMEPATH",
            "USERNAME",
            "PROGRAMFILES",
            "PROGRAMW6432",
            "PROGRAMFILES(X86)",
            "APPDATA",
            "LOCALAPPDATA",
        ]
        .contains(&name)
    }
    #[cfg(not(any(target_os = "macos", target_os = "windows")))]
    {
        ["HOME", "USER", "LOGNAME", "TMPDIR"].contains(&name)
    }
}

fn is_sensitive_name(name: &str) -> bool {
    let name = name.to_ascii_uppercase();
    if name.starts_with("AEGISY_")
        || matches!(name.as_str(), "HTTP_PROXY" | "HTTPS_PROXY" | "ALL_PROXY")
        || matches!(
            name.as_str(),
            "SSH_AUTH_SOCK" | "GPG_AGENT_INFO" | "GOOGLE_APPLICATION_CREDENTIALS"
        )
    {
        return true;
    }
    let parts = name
        .split(|character: char| !character.is_ascii_alphanumeric())
        .filter(|part| !part.is_empty())
        .collect::<Vec<_>>();
    parts.iter().any(|part| {
        matches!(
            *part,
            "TOKEN"
                | "SECRET"
                | "PASSWORD"
                | "PASSWD"
                | "CREDENTIAL"
                | "CREDENTIALS"
                | "COOKIE"
                | "AUTHORIZATION"
                | "JWT"
        )
    }) || name.contains("API_KEY")
        || name.contains("APIKEY")
        || name.contains("PRIVATE_KEY")
        || name.contains("ACCESS_KEY")
        || name.contains("SECRET_KEY")
}

fn is_dangerous_tool_name(name: &str) -> bool {
    let name = name.to_ascii_uppercase();
    matches!(
        name.as_str(),
        "LD_PRELOAD"
            | "LD_LIBRARY_PATH"
            | "DYLD_INSERT_LIBRARIES"
            | "DYLD_LIBRARY_PATH"
            | "DYLD_FRAMEWORK_PATH"
            | "BASH_ENV"
            | "ENV"
            | "ZDOTDIR"
            | "NODE_OPTIONS"
            | "PYTHONPATH"
            | "PYTHONHOME"
            | "RUBYOPT"
            | "PERL5OPT"
            | "GIT_SSH_COMMAND"
            | "GIT_ASKPASS"
            | "SSH_ASKPASS"
            | "SUDO_ASKPASS"
            | "PROMPT_COMMAND"
            | "CDPATH"
            | "IFS"
    ) || name.starts_with("DYLD_")
        || name.starts_with("GIT_CONFIG_")
}

fn valid_inherited_value(name: &str, value: &OsStr, root: &Path) -> bool {
    if matches!(name, "SYSTEMDRIVE" | "HOMEDRIVE") {
        let value = value.to_string_lossy();
        return value.len() == 2
            && value.as_bytes()[0].is_ascii_alphabetic()
            && value.as_bytes()[1] == b':';
    }
    if !matches!(
        name,
        "HOME"
            | "TMPDIR"
            | "SYSTEMROOT"
            | "WINDIR"
            | "COMSPEC"
            | "TEMP"
            | "TMP"
            | "USERPROFILE"
            | "HOMEPATH"
            | "APPDATA"
            | "LOCALAPPDATA"
            | "PROGRAMFILES"
            | "PROGRAMW6432"
            | "PROGRAMFILES(X86)"
    ) {
        return true;
    }
    let path = PathBuf::from(value);
    if !path.is_absolute() {
        return false;
    }
    let Ok(canonical) = path.canonicalize() else {
        return false;
    };
    if path_is_within(&canonical, root) {
        return false;
    }
    if name == "COMSPEC" {
        canonical.is_file()
    } else {
        canonical.is_dir()
    }
}

fn valid_variable_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 64
        && name.bytes().enumerate().all(|(index, byte)| {
            if index == 0 {
                byte == b'_' || byte.is_ascii_alphabetic()
            } else {
                byte == b'_' || byte.is_ascii_alphanumeric()
            }
        })
}

fn valid_tool_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 64
        && name
            .bytes()
            .all(|byte| byte == b'-' || byte == b'_' || byte.is_ascii_alphanumeric())
}

fn normalize_name(name: &str) -> String {
    #[cfg(target_os = "windows")]
    {
        name.to_ascii_uppercase()
    }
    #[cfg(not(target_os = "windows"))]
    {
        name.to_owned()
    }
}

fn path_is_within(path: &Path, root: &Path) -> bool {
    #[cfg(target_os = "windows")]
    {
        let root = path_key(root);
        let candidate = path_key(path);
        candidate == root
            || candidate
                .strip_prefix(&root)
                .is_some_and(|suffix| suffix.starts_with('\\'))
    }
    #[cfg(not(target_os = "windows"))]
    {
        path.starts_with(root)
    }
}

fn path_key(path: &Path) -> String {
    let value = path.to_string_lossy();
    #[cfg(target_os = "windows")]
    {
        value.replace('/', "\\").to_ascii_lowercase()
    }
    #[cfg(not(target_os = "windows"))]
    {
        value.into_owned()
    }
}

#[cfg(unix)]
fn os_value_bytes(value: &OsStr) -> Vec<u8> {
    use std::os::unix::ffi::OsStrExt;
    value.as_bytes().to_vec()
}

#[cfg(windows)]
fn os_value_bytes(value: &OsStr) -> Vec<u8> {
    use std::os::windows::ffi::OsStrExt;
    value
        .encode_wide()
        .flat_map(u16::to_le_bytes)
        .collect::<Vec<_>>()
}

#[cfg(not(any(unix, windows)))]
fn os_value_bytes(value: &OsStr) -> Vec<u8> {
    value.to_string_lossy().as_bytes().to_vec()
}

fn environment_error(message: impl Into<String>) -> EnvironmentError {
    EnvironmentError {
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::to_string;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn root() -> PathBuf {
        let root = env::temp_dir().join(format!(
            "aegisy-environment-{}",
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir_all(&root).unwrap();
        root.canonicalize().unwrap()
    }

    #[test]
    fn filters_secrets_and_project_path_entries_without_exposing_values() {
        let root = root();
        let tool_dir = root.join("tools");
        fs::create_dir_all(&tool_dir).unwrap();
        let safe_dir = env::temp_dir().canonicalize().unwrap();
        let source = vec![
            (
                OsString::from("PATH"),
                env::join_paths([&tool_dir, &safe_dir]).unwrap(),
            ),
            (
                OsString::from("OPENAI_API_KEY"),
                OsString::from("never-serialize-me"),
            ),
            (
                OsString::from("CI_JOB_TOKEN"),
                OsString::from("token-value"),
            ),
            (
                OsString::from("openai_api_key"),
                OsString::from("lowercase-secret"),
            ),
            (OsString::from("HOME"), OsString::from("/safe/home")),
            (OsString::from("TMPDIR"), root.as_os_str().to_owned()),
        ];
        let environment =
            SessionEnvironment::build_from("session-a", Some("project-a"), "work", &root, source);
        assert_eq!(environment.summary.masked_count, 3);
        assert!(environment.summary.path_entry_count >= 1);
        let serialized = to_string(environment.summary()).unwrap();
        assert!(!serialized.contains("never-serialize-me"));
        assert!(!serialized.contains("lowercase-secret"));
        assert!(!serialized.contains("OPENAI"));
        let process = environment.for_tool("terminal", Vec::new()).unwrap();
        let path = process.iter().find(|(name, _)| *name == "PATH").unwrap().1;
        assert!(!env::split_paths(path).any(|entry| entry.starts_with(&root)));
        assert!(process.iter().all(|(name, _)| name != "TMPDIR"));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn explicit_tool_variables_are_bounded_and_cannot_override_or_add_secrets() {
        let root = root();
        let environment =
            SessionEnvironment::build_from("session-a", Some("project-a"), "work", &root, vec![]);
        let process = environment
            .for_tool(
                "terminal",
                vec![
                    ToolVariable::new("TERM", "xterm-256color"),
                    ToolVariable::new("COLORTERM", "truecolor"),
                ],
            )
            .unwrap();
        assert!(process
            .summary()
            .explicit_variable_names
            .contains(&"TERM".into()));
        assert!(environment
            .for_tool(
                "terminal",
                vec![ToolVariable::new("OPENAI_API_KEY", "secret")]
            )
            .is_err());
        assert!(environment
            .for_tool(
                "terminal",
                vec![ToolVariable::new("openai_api_key", "secret")]
            )
            .is_err());
        assert!(environment
            .for_tool(
                "terminal",
                vec![ToolVariable::new("LD_PRELOAD", "/tmp/inject.dylib")]
            )
            .is_err());
        assert!(environment
            .for_tool(
                "terminal",
                vec![ToolVariable::new("AEGISY_SESSION_ID", "override")]
            )
            .is_err());
        assert!(environment
            .for_tool(
                "terminal",
                vec![ToolVariable::new("TERM", "x".repeat(5000))]
            )
            .is_err());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn environment_identity_is_deterministic_and_session_scoped() {
        let root = root();
        let source = vec![(OsString::from("LANG"), OsString::from("en_US.UTF-8"))];
        let first = SessionEnvironment::build_from(
            "session-a",
            Some("project-a"),
            "work",
            &root,
            source.clone(),
        );
        let repeated = SessionEnvironment::build_from(
            "session-a",
            Some("project-a"),
            "work",
            &root,
            source.clone(),
        );
        let other =
            SessionEnvironment::build_from("session-b", Some("project-a"), "work", &root, source);
        assert_eq!(
            first.summary.environment_id,
            repeated.summary.environment_id
        );
        assert_ne!(first.summary.environment_id, other.summary.environment_id);
        assert!(first
            .summary
            .environment_id
            .starts_with("environment:sha256:"));
        fs::remove_dir_all(root).unwrap();
    }
}
