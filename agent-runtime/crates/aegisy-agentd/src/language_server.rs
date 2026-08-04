use crate::git_status::ignored_paths;
use crate::workspace::{path_metadata, WorkspaceError, MAX_TEXT_FILE_BYTES};
use lsp_types::{
    Diagnostic, GotoDefinitionResponse, Location, LocationLink, NumberOrString,
    PublishDiagnosticsParams,
};
use serde::Serialize;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::env;
use std::fs;
use std::io::{self, BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError};
use std::thread;
use std::time::{Duration, Instant};

const MAX_LSP_FRAME_BYTES: usize = 4 * 1024 * 1024;
const MAX_LSP_HEADER_BYTES: usize = 64 * 1024;
const MAX_LANGUAGE_RESULTS: usize = 500;
const MAX_DIAGNOSTIC_MESSAGE_CHARS: usize = 2_000;
#[cfg(not(test))]
const INITIALIZE_TIMEOUT: Duration = Duration::from_secs(8);
// CI runners under full-suite load can take far longer to spawn and answer
// a cold language server, so tests use wider bounds than production.
#[cfg(test)]
const INITIALIZE_TIMEOUT: Duration = Duration::from_secs(30);
#[cfg(not(test))]
const REQUEST_TIMEOUT: Duration = Duration::from_secs(6);
#[cfg(test)]
const REQUEST_TIMEOUT: Duration = Duration::from_secs(30);
#[cfg(not(test))]
const DIAGNOSTIC_TIMEOUT: Duration = Duration::from_millis(1_500);
#[cfg(test)]
const DIAGNOSTIC_TIMEOUT: Duration = Duration::from_secs(15);

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct LanguageServerStatus {
    pub language: String,
    pub server_id: String,
    pub command: String,
    pub installed: bool,
    pub running: bool,
    pub detail: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct LanguageLocation {
    pub path: String,
    pub line: usize,
    pub column: usize,
    pub end_line: usize,
    pub end_column: usize,
    pub provenance: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct LanguageLocationsResult {
    pub language: String,
    pub server_id: String,
    pub revision: String,
    pub stale: bool,
    pub truncated: bool,
    pub denied_locations: usize,
    pub locations: Vec<LanguageLocation>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct LanguageDiagnostic {
    pub path: String,
    pub line: usize,
    pub column: usize,
    pub end_line: usize,
    pub end_column: usize,
    pub severity: String,
    pub code: Option<String>,
    pub message: String,
    pub source: String,
    pub provenance: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct LanguageDiagnosticsResult {
    pub language: String,
    pub server_id: String,
    pub revision: String,
    pub stale: bool,
    pub pending: bool,
    pub truncated: bool,
    pub diagnostics: Vec<LanguageDiagnostic>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LanguageError {
    pub code: i64,
    pub message: String,
}

#[derive(Default)]
pub struct LanguageServerManager {
    servers: HashMap<String, LanguageServer>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
enum LanguageKind {
    Rust,
    Python,
    TypeScript,
    Cpp,
}

struct LanguageServer {
    kind: LanguageKind,
    root: PathBuf,
    root_uri: String,
    child: Child,
    stdin: ChildStdin,
    messages: Receiver<Result<Value, String>>,
    next_request_id: u64,
    open_documents: HashMap<String, i32>,
    diagnostics: HashMap<String, PublishDiagnosticsParams>,
    configuration: Value,
}

#[derive(Debug, Clone)]
struct ExecutableSpec {
    path: PathBuf,
    args: Vec<&'static str>,
}

impl LanguageServerManager {
    pub fn statuses(&mut self, project_id: &str, root: &Path) -> Vec<LanguageServerStatus> {
        LanguageKind::ALL
            .iter()
            .map(|kind| {
                let key = server_key(project_id, *kind);
                let running = self
                    .servers
                    .get_mut(&key)
                    .is_some_and(LanguageServer::is_running);
                let executable = kind.executable(Some(root));
                LanguageServerStatus {
                    language: kind.display_name().into(),
                    server_id: kind.server_id().into(),
                    command: kind.command_name().into(),
                    installed: executable.is_some(),
                    running,
                    detail: if running {
                        "running".into()
                    } else if executable.is_some() {
                        "available".into()
                    } else {
                        "not installed".into()
                    },
                }
            })
            .collect()
    }

    pub fn start(
        &mut self,
        project_id: &str,
        root: &Path,
        path: &str,
    ) -> Result<LanguageServerStatus, LanguageError> {
        let kind = LanguageKind::from_path(path).ok_or_else(|| {
            language_error(
                -32070,
                "no language server is registered for this file type",
            )
        })?;
        self.ensure_server(project_id, root, kind)?;
        Ok(LanguageServerStatus {
            language: kind.display_name().into(),
            server_id: kind.server_id().into(),
            command: kind.command_name().into(),
            installed: true,
            running: true,
            detail: "running".into(),
        })
    }

    pub fn stop(&mut self, project_id: &str, path: &str) -> Result<bool, LanguageError> {
        let kind = LanguageKind::from_path(path).ok_or_else(|| {
            language_error(
                -32070,
                "no language server is registered for this file type",
            )
        })?;
        let Some(mut server) = self.servers.remove(&server_key(project_id, kind)) else {
            return Ok(false);
        };
        server.shutdown();
        Ok(true)
    }

    #[allow(clippy::too_many_arguments)]
    pub fn definition(
        &mut self,
        project_id: &str,
        root: &Path,
        path: &str,
        content: &str,
        revision: &str,
        line: usize,
        column: usize,
    ) -> Result<LanguageLocationsResult, LanguageError> {
        self.locations(
            project_id,
            root,
            path,
            content,
            revision,
            line,
            column,
            "textDocument/definition",
        )
    }

    #[allow(clippy::too_many_arguments)]
    pub fn references(
        &mut self,
        project_id: &str,
        root: &Path,
        path: &str,
        content: &str,
        revision: &str,
        line: usize,
        column: usize,
    ) -> Result<LanguageLocationsResult, LanguageError> {
        self.locations(
            project_id,
            root,
            path,
            content,
            revision,
            line,
            column,
            "textDocument/references",
        )
    }

    pub fn diagnostics(
        &mut self,
        project_id: &str,
        root: &Path,
        path: &str,
        content: &str,
        revision: &str,
    ) -> Result<LanguageDiagnosticsResult, LanguageError> {
        validate_document(content)?;
        let kind = LanguageKind::from_path(path).ok_or_else(|| {
            language_error(
                -32070,
                "no language server is registered for this file type",
            )
        })?;
        let server = self.ensure_server(project_id, root, kind)?;
        let (uri, version) = server.sync_document(path, content)?;
        let diagnostics = server.wait_for_diagnostics(&uri, version, DIAGNOSTIC_TIMEOUT)?;
        let mut mapped = diagnostics
            .as_ref()
            .map(|params| {
                params
                    .diagnostics
                    .iter()
                    .filter_map(|diagnostic| map_diagnostic(root, kind, &uri, diagnostic))
                    .collect::<Vec<_>>()
            })
            .unwrap_or_default();
        let truncated = mapped.len() > MAX_LANGUAGE_RESULTS;
        mapped.truncate(MAX_LANGUAGE_RESULTS);
        Ok(LanguageDiagnosticsResult {
            language: kind.display_name().into(),
            server_id: kind.server_id().into(),
            revision: revision.into(),
            stale: false,
            pending: diagnostics.is_none(),
            truncated,
            diagnostics: mapped,
        })
    }

    pub fn invalidate_document(&mut self, project_id: &str, path: &str) {
        let Some(kind) = LanguageKind::from_path(path) else {
            return;
        };
        if let Some(server) = self.servers.get_mut(&server_key(project_id, kind)) {
            server.invalidate_document(path);
        }
    }

    pub fn shutdown_all(&mut self) {
        for (_, mut server) in self.servers.drain() {
            server.shutdown();
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn locations(
        &mut self,
        project_id: &str,
        root: &Path,
        path: &str,
        content: &str,
        revision: &str,
        line: usize,
        column: usize,
        method: &str,
    ) -> Result<LanguageLocationsResult, LanguageError> {
        validate_document(content)?;
        if line == 0 || column == 0 {
            return Err(language_error(-32071, "line and column must be 1-based"));
        }
        let kind = LanguageKind::from_path(path).ok_or_else(|| {
            language_error(
                -32070,
                "no language server is registered for this file type",
            )
        })?;
        let server = self.ensure_server(project_id, root, kind)?;
        let (uri, _) = server.sync_document(path, content)?;
        let mut params = json!({
            "textDocument": { "uri": uri },
            "position": { "line": line - 1, "character": column - 1 }
        });
        if method == "textDocument/references" {
            params["context"] = json!({ "includeDeclaration": true });
        }
        let result = server.request(method, params, REQUEST_TIMEOUT)?;
        let raw_locations = parse_locations(method, result)?;
        let mut denied_locations = 0;
        let mut locations = Vec::new();
        for location in raw_locations {
            match map_location(root, kind, location) {
                Some(location) => locations.push(location),
                None => denied_locations += 1,
            }
        }
        let paths = locations
            .iter()
            .map(|location| location.path.clone())
            .collect::<Vec<_>>();
        let ignored = ignored_paths(root, &paths);
        denied_locations += locations
            .iter()
            .filter(|location| ignored.contains(&location.path))
            .count();
        locations.retain(|location| !ignored.contains(&location.path));
        let truncated = locations.len() > MAX_LANGUAGE_RESULTS;
        locations.truncate(MAX_LANGUAGE_RESULTS);
        Ok(LanguageLocationsResult {
            language: kind.display_name().into(),
            server_id: kind.server_id().into(),
            revision: revision.into(),
            stale: false,
            truncated,
            denied_locations,
            locations,
        })
    }

    fn ensure_server(
        &mut self,
        project_id: &str,
        root: &Path,
        kind: LanguageKind,
    ) -> Result<&mut LanguageServer, LanguageError> {
        let key = server_key(project_id, kind);
        let must_restart = self
            .servers
            .get_mut(&key)
            .is_some_and(|server| !server.is_running());
        if must_restart {
            self.servers.remove(&key);
        }
        if !self.servers.contains_key(&key) {
            let executable = kind.executable(Some(root)).ok_or_else(|| {
                language_error(
                    -32070,
                    format!(
                        "{} is not installed or not discoverable in PATH",
                        kind.command_name()
                    ),
                )
            })?;
            self.servers
                .insert(key.clone(), LanguageServer::spawn(kind, root, executable)?);
        }
        self.servers
            .get_mut(&key)
            .ok_or_else(|| language_error(-32072, "language server failed to start"))
    }
}

impl Drop for LanguageServerManager {
    fn drop(&mut self) {
        self.shutdown_all();
    }
}

impl LanguageKind {
    const ALL: [Self; 4] = [Self::Rust, Self::Python, Self::TypeScript, Self::Cpp];

    fn from_path(path: &str) -> Option<Self> {
        let extension = Path::new(path)
            .extension()
            .and_then(|extension| extension.to_str())?
            .to_ascii_lowercase();
        match extension.as_str() {
            "rs" => Some(Self::Rust),
            "py" | "pyi" => Some(Self::Python),
            "js" | "jsx" | "mjs" | "cjs" | "ts" | "tsx" | "mts" | "cts" => Some(Self::TypeScript),
            "c" | "cc" | "cpp" | "cxx" | "h" | "hh" | "hpp" | "hxx" => Some(Self::Cpp),
            _ => None,
        }
    }

    const fn display_name(self) -> &'static str {
        match self {
            Self::Rust => "Rust",
            Self::Python => "Python",
            Self::TypeScript => "JavaScript / TypeScript",
            Self::Cpp => "C / C++",
        }
    }

    const fn server_id(self) -> &'static str {
        match self {
            Self::Rust => "rust-analyzer",
            Self::Python => "pyright",
            Self::TypeScript => "typescript-language-server",
            Self::Cpp => "clangd",
        }
    }

    const fn command_name(self) -> &'static str {
        match self {
            Self::Rust => "rust-analyzer",
            Self::Python => "pyright-langserver",
            Self::TypeScript => "typescript-language-server",
            Self::Cpp => "clangd",
        }
    }

    const fn environment_override(self) -> &'static str {
        match self {
            Self::Rust => "AEGISY_LSP_RUST_PATH",
            Self::Python => "AEGISY_LSP_PYTHON_PATH",
            Self::TypeScript => "AEGISY_LSP_TYPESCRIPT_PATH",
            Self::Cpp => "AEGISY_LSP_CPP_PATH",
        }
    }

    fn language_id(self, path: &str) -> &'static str {
        match self {
            Self::Rust => "rust",
            Self::Python => "python",
            Self::TypeScript if path.ends_with(".tsx") => "typescriptreact",
            Self::TypeScript if path.ends_with(".jsx") => "javascriptreact",
            Self::TypeScript
                if path.ends_with(".js") || path.ends_with(".mjs") || path.ends_with(".cjs") =>
            {
                "javascript"
            }
            Self::TypeScript => "typescript",
            Self::Cpp if path.ends_with(".c") => "c",
            Self::Cpp => "cpp",
        }
    }

    fn executable(self, denied_root: Option<&Path>) -> Option<ExecutableSpec> {
        let override_path = env::var_os(self.environment_override()).map(PathBuf::from);
        let path = override_path
            .filter(|path| executable_is_allowed(path, denied_root))
            .or_else(|| {
                let names: &[&str] = match self {
                    Self::Python => &["basedpyright-langserver", "pyright-langserver"],
                    _ => &[self.command_name()],
                };
                names
                    .iter()
                    .find_map(|name| find_executable(name, denied_root))
            })?;
        let args = match self {
            Self::Python | Self::TypeScript => vec!["--stdio"],
            Self::Cpp => vec![
                "--background-index=0",
                "--clang-tidy=0",
                "--log=error",
                "--offset-encoding=utf-16",
            ],
            Self::Rust => Vec::new(),
        };
        Some(ExecutableSpec { path, args })
    }
}

impl LanguageServer {
    fn spawn(
        kind: LanguageKind,
        root: &Path,
        executable: ExecutableSpec,
    ) -> Result<Self, LanguageError> {
        let root = root.canonicalize().map_err(|cause| {
            language_error(-32072, format!("workspace root is unavailable: {cause}"))
        })?;
        let root_uri = file_uri(&root);
        let isolated_home = isolated_server_home(&root, kind)?;
        let mut command = Command::new(&executable.path);
        command
            .args(executable.args)
            .current_dir(&root)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .env_clear()
            .env("HOME", &isolated_home)
            .env("USERPROFILE", &isolated_home)
            .env("XDG_CACHE_HOME", isolated_home.join("cache"))
            .env("TMPDIR", env::temp_dir())
            .env("TEMP", env::temp_dir())
            .env("TMP", env::temp_dir())
            .env("PATH", safe_path_environment(&root));
        for name in ["SystemRoot", "ComSpec", "PATHEXT", "RUSTUP_HOME"] {
            if let Some(value) = env::var_os(name) {
                command.env(name, value);
            }
        }
        let mut child = command.spawn().map_err(|cause| {
            language_error(
                -32072,
                format!("cannot start {}: {cause}", kind.command_name()),
            )
        })?;
        let stdin = child
            .stdin
            .take()
            .ok_or_else(|| language_error(-32072, "language server stdin is unavailable"))?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| language_error(-32072, "language server stdout is unavailable"))?;
        if let Some(stderr) = child.stderr.take() {
            thread::spawn(move || {
                let _ = io::copy(&mut BufReader::new(stderr), &mut io::sink());
            });
        }
        let (sender, messages) = mpsc::channel();
        thread::spawn(move || {
            let mut reader = BufReader::new(stdout);
            loop {
                match read_lsp_message(&mut reader) {
                    Ok(Some(message)) => {
                        if sender.send(Ok(message)).is_err() {
                            break;
                        }
                    }
                    Ok(None) => break,
                    Err(cause) => {
                        let _ = sender.send(Err(cause.to_string()));
                        break;
                    }
                }
            }
        });
        let mut server = Self {
            kind,
            root,
            root_uri,
            child,
            stdin,
            messages,
            next_request_id: 0,
            open_documents: HashMap::new(),
            diagnostics: HashMap::new(),
            configuration: safe_initialization_options(kind),
        };
        let result = server.request(
            "initialize",
            json!({
                "processId": std::process::id(),
                "clientInfo": { "name": "Aegisy Coding", "version": env!("CARGO_PKG_VERSION") },
                "locale": "zh-CN",
                "rootUri": server.root_uri,
                "initializationOptions": server.configuration,
                "capabilities": {
                    "general": { "positionEncodings": ["utf-16"] },
                    "workspace": { "applyEdit": false, "workspaceFolders": true },
                    "textDocument": {
                        "definition": { "linkSupport": true },
                        "references": {},
                        "publishDiagnostics": { "versionSupport": true }
                    }
                },
                "workspaceFolders": [{
                    "uri": server.root_uri,
                    "name": server.root.file_name().and_then(|name| name.to_str()).unwrap_or("Project")
                }]
            }),
            INITIALIZE_TIMEOUT,
        );
        if let Err(error) = result {
            let _ = server.child.kill();
            return Err(error);
        }
        server.notify("initialized", json!({}))?;
        Ok(server)
    }

    fn is_running(&mut self) -> bool {
        matches!(self.child.try_wait(), Ok(None))
    }

    fn sync_document(&mut self, path: &str, content: &str) -> Result<(String, i32), LanguageError> {
        let absolute = self.root.join(path);
        let uri = file_uri(&absolute);
        self.diagnostics.remove(&uri);
        let version = self.open_documents.get(&uri).copied().unwrap_or_default() + 1;
        if version == 1 {
            self.notify(
                "textDocument/didOpen",
                json!({
                    "textDocument": {
                        "uri": uri,
                        "languageId": self.kind.language_id(path),
                        "version": version,
                        "text": content
                    }
                }),
            )?;
        } else {
            self.notify(
                "textDocument/didChange",
                json!({
                    "textDocument": { "uri": uri, "version": version },
                    "contentChanges": [{ "text": content }]
                }),
            )?;
        }
        self.open_documents.insert(uri.clone(), version);
        Ok((uri, version))
    }

    fn invalidate_document(&mut self, path: &str) {
        let uri = file_uri(&self.root.join(path));
        self.diagnostics.remove(&uri);
    }

    fn request(
        &mut self,
        method: &str,
        params: Value,
        timeout: Duration,
    ) -> Result<Value, LanguageError> {
        if !self.is_running() {
            return Err(language_error(
                -32073,
                "language server exited unexpectedly",
            ));
        }
        self.next_request_id += 1;
        let id = self.next_request_id;
        self.write_message(&json!({
            "jsonrpc": "2.0",
            "id": id,
            "method": method,
            "params": params
        }))?;
        let deadline = Instant::now() + timeout;
        loop {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Err(language_error(
                    -32074,
                    format!("language server request timed out: {method}"),
                ));
            }
            let message = self.receive(remaining)?;
            if message.get("id").and_then(Value::as_u64) == Some(id)
                && message.get("method").is_none()
            {
                if let Some(error) = message.get("error") {
                    return Err(language_error(
                        -32075,
                        format!("language server rejected {method}: {error}"),
                    ));
                }
                return Ok(message.get("result").cloned().unwrap_or(Value::Null));
            }
            self.handle_incoming(message)?;
        }
    }

    fn notify(&mut self, method: &str, params: Value) -> Result<(), LanguageError> {
        self.write_message(&json!({
            "jsonrpc": "2.0",
            "method": method,
            "params": params
        }))
    }

    fn wait_for_diagnostics(
        &mut self,
        uri: &str,
        version: i32,
        timeout: Duration,
    ) -> Result<Option<PublishDiagnosticsParams>, LanguageError> {
        let deadline = Instant::now() + timeout;
        loop {
            if let Some(diagnostics) = self.diagnostics.get(uri) {
                if diagnostics.version.is_none() || diagnostics.version == Some(version) {
                    return Ok(Some(diagnostics.clone()));
                }
            }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Ok(None);
            }
            match self.messages.recv_timeout(remaining) {
                Ok(Ok(message)) => self.handle_incoming(message)?,
                Ok(Err(message)) => return Err(language_error(-32073, message)),
                Err(RecvTimeoutError::Timeout) => return Ok(None),
                Err(RecvTimeoutError::Disconnected) => {
                    return Err(language_error(-32073, "language server output closed"))
                }
            }
        }
    }

    fn receive(&self, timeout: Duration) -> Result<Value, LanguageError> {
        match self.messages.recv_timeout(timeout) {
            Ok(Ok(message)) => Ok(message),
            Ok(Err(message)) => Err(language_error(-32073, message)),
            Err(RecvTimeoutError::Timeout) => {
                Err(language_error(-32074, "language server timed out"))
            }
            Err(RecvTimeoutError::Disconnected) => {
                Err(language_error(-32073, "language server output closed"))
            }
        }
    }

    fn handle_incoming(&mut self, message: Value) -> Result<(), LanguageError> {
        let method = message
            .get("method")
            .and_then(Value::as_str)
            .unwrap_or_default();
        if method == "textDocument/publishDiagnostics" {
            if let Ok(params) = serde_json::from_value::<PublishDiagnosticsParams>(
                message.get("params").cloned().unwrap_or(Value::Null),
            ) {
                self.diagnostics
                    .insert(params.uri.as_str().to_owned(), params);
            }
            return Ok(());
        }
        let Some(id) = message.get("id").cloned() else {
            return Ok(());
        };
        if method.is_empty() {
            return Ok(());
        }
        let result = match method {
            "workspace/applyEdit" => json!({
                "applied": false,
                "failureReason": "Aegisy language servers are read-only"
            }),
            "workspace/configuration" => {
                let count = message
                    .pointer("/params/items")
                    .and_then(Value::as_array)
                    .map_or(0, Vec::len);
                Value::Array((0..count).map(|_| self.configuration.clone()).collect())
            }
            "workspace/workspaceFolders" => json!([{
                "uri": self.root_uri,
                "name": self.root.file_name().and_then(|name| name.to_str()).unwrap_or("Project")
            }]),
            _ => Value::Null,
        };
        self.write_message(&json!({ "jsonrpc": "2.0", "id": id, "result": result }))
    }

    fn write_message(&mut self, message: &Value) -> Result<(), LanguageError> {
        let body = serde_json::to_vec(message).map_err(|cause| {
            language_error(-32072, format!("cannot encode LSP message: {cause}"))
        })?;
        if body.len() > MAX_LSP_FRAME_BYTES {
            return Err(language_error(-32076, "outgoing LSP message exceeds limit"));
        }
        write!(self.stdin, "Content-Length: {}\r\n\r\n", body.len())
            .and_then(|_| self.stdin.write_all(&body))
            .and_then(|_| self.stdin.flush())
            .map_err(|cause| {
                language_error(-32073, format!("cannot write to language server: {cause}"))
            })
    }

    fn shutdown(&mut self) {
        if self.is_running() {
            let _ = self.request("shutdown", Value::Null, Duration::from_millis(750));
            let _ = self.notify("exit", Value::Null);
        }
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn parse_locations(method: &str, value: Value) -> Result<Vec<Location>, LanguageError> {
    if value.is_null() {
        return Ok(Vec::new());
    }
    if method == "textDocument/definition" {
        let response: GotoDefinitionResponse = serde_json::from_value(value).map_err(|cause| {
            language_error(-32075, format!("invalid definition response: {cause}"))
        })?;
        return Ok(match response {
            GotoDefinitionResponse::Scalar(location) => vec![location],
            GotoDefinitionResponse::Array(locations) => locations,
            GotoDefinitionResponse::Link(links) => {
                links.into_iter().map(location_from_link).collect()
            }
        });
    }
    serde_json::from_value(value)
        .map_err(|cause| language_error(-32075, format!("invalid reference response: {cause}")))
}

fn safe_initialization_options(kind: LanguageKind) -> Value {
    if kind != LanguageKind::Rust {
        return json!({});
    }
    json!({
        "linkedProjects": [],
        "cargo": {
            "buildScripts": { "enable": false },
            "noDeps": true,
            "sysroot": null
        },
        "procMacro": { "enable": false },
        "checkOnSave": false,
        "cachePriming": { "enable": false }
    })
}

fn location_from_link(link: LocationLink) -> Location {
    Location {
        uri: link.target_uri,
        range: link.target_selection_range,
    }
}

fn map_location(root: &Path, kind: LanguageKind, location: Location) -> Option<LanguageLocation> {
    let path = workspace_relative_from_uri(root, location.uri.as_str())?;
    Some(LanguageLocation {
        path,
        line: location.range.start.line as usize + 1,
        column: location.range.start.character as usize + 1,
        end_line: location.range.end.line as usize + 1,
        end_column: location.range.end.character as usize + 1,
        provenance: format!("language-server:{}", kind.server_id()),
    })
}

fn map_diagnostic(
    root: &Path,
    kind: LanguageKind,
    uri: &str,
    diagnostic: &Diagnostic,
) -> Option<LanguageDiagnostic> {
    let path = workspace_relative_from_uri(root, uri)?;
    let severity = diagnostic
        .severity
        .and_then(|severity| serde_json::to_value(severity).ok())
        .and_then(|value| value.as_i64())
        .map_or("unknown", |severity| match severity {
            1 => "error",
            2 => "warning",
            3 => "information",
            4 => "hint",
            _ => "unknown",
        });
    let code = diagnostic.code.as_ref().map(|code| match code {
        NumberOrString::Number(number) => number.to_string(),
        NumberOrString::String(value) => value.clone(),
    });
    Some(LanguageDiagnostic {
        path,
        line: diagnostic.range.start.line as usize + 1,
        column: diagnostic.range.start.character as usize + 1,
        end_line: diagnostic.range.end.line as usize + 1,
        end_column: diagnostic.range.end.character as usize + 1,
        severity: severity.into(),
        code,
        message: truncate_chars(&diagnostic.message, MAX_DIAGNOSTIC_MESSAGE_CHARS),
        source: diagnostic
            .source
            .clone()
            .unwrap_or_else(|| kind.server_id().into()),
        provenance: format!("language-server:{}", kind.server_id()),
    })
}

fn workspace_relative_from_uri(root: &Path, uri: &str) -> Option<String> {
    let path = crate::plain_path(&path_from_file_uri(uri)?);
    let canonical_root = crate::plain_path(&root.canonicalize().ok()?);
    let relative = path.strip_prefix(&canonical_root).ok()?;
    let relative = relative.to_string_lossy().replace('\\', "/");
    let metadata = path_metadata(&canonical_root, &relative).ok()?;
    (metadata.kind == "file").then_some(relative)
}

fn validate_document(content: &str) -> Result<(), LanguageError> {
    if content.contains('\0') {
        return Err(language_error(
            -32071,
            "language document contains NUL bytes",
        ));
    }
    if content.len() as u64 > MAX_TEXT_FILE_BYTES {
        return Err(language_error(
            -32071,
            "language document exceeds text limit",
        ));
    }
    Ok(())
}

fn read_lsp_message<R: BufRead>(reader: &mut R) -> io::Result<Option<Value>> {
    let mut content_length = None;
    let mut header_bytes = 0_usize;
    loop {
        let mut line = String::new();
        let bytes = reader.read_line(&mut line)?;
        if bytes == 0 {
            return Ok(None);
        }
        header_bytes = header_bytes.saturating_add(bytes);
        if header_bytes > MAX_LSP_HEADER_BYTES {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "LSP headers exceed limit",
            ));
        }
        if line == "\r\n" || line == "\n" {
            break;
        }
        if let Some((name, value)) = line.trim().split_once(':') {
            if name.eq_ignore_ascii_case("Content-Length") {
                content_length = Some(value.trim().parse::<usize>().map_err(|_| {
                    io::Error::new(io::ErrorKind::InvalidData, "invalid LSP content length")
                })?);
            }
        }
    }
    let content_length = content_length
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "missing LSP content length"))?;
    if content_length > MAX_LSP_FRAME_BYTES {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "LSP frame exceeds limit",
        ));
    }
    let mut body = vec![0_u8; content_length];
    reader.read_exact(&mut body)?;
    serde_json::from_slice(&body)
        .map(Some)
        .map_err(|cause| io::Error::new(io::ErrorKind::InvalidData, cause))
}

fn file_uri(path: &Path) -> String {
    // Path::canonicalize returns a verbatim \\?\ path on Windows, which must
    // not leak into file URIs.
    let normalized = crate::plain_path(path).to_string_lossy().replace('\\', "/");
    let prefix = if normalized.starts_with('/') {
        "file://"
    } else {
        "file:///"
    };
    format!("{prefix}{}", percent_encode(&normalized))
}

fn path_from_file_uri(uri: &str) -> Option<PathBuf> {
    let encoded = uri.strip_prefix("file://")?;
    if !encoded.starts_with('/') {
        return None;
    }
    let decoded = percent_decode(encoded)?;
    #[cfg(windows)]
    let decoded = if decoded.as_bytes().get(2) == Some(&b':') {
        // Servers may report the drive letter in either case; Windows drive
        // letters are case-insensitive, so normalize to uppercase.
        let mut path = decoded[1..].to_owned();
        let drive = path[..1].to_ascii_uppercase();
        path.replace_range(..1, &drive);
        path
    } else {
        decoded
    };
    Some(PathBuf::from(decoded))
}

fn percent_encode(value: &str) -> String {
    let mut encoded = String::new();
    for byte in value.as_bytes() {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'.' | b'_' | b'~' | b'/' | b':') {
            encoded.push(char::from(*byte));
        } else {
            encoded.push_str(&format!("%{byte:02X}"));
        }
    }
    encoded
}

fn percent_decode(value: &str) -> Option<String> {
    let bytes = value.as_bytes();
    let mut decoded = Vec::with_capacity(bytes.len());
    let mut index = 0;
    while index < bytes.len() {
        if bytes[index] == b'%' {
            let high = hex_value(*bytes.get(index + 1)?)?;
            let low = hex_value(*bytes.get(index + 2)?)?;
            decoded.push((high << 4) | low);
            index += 3;
        } else {
            decoded.push(bytes[index]);
            index += 1;
        }
    }
    String::from_utf8(decoded).ok()
}

fn hex_value(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

fn find_executable(name: &str, denied_root: Option<&Path>) -> Option<PathBuf> {
    let path = Path::new(name);
    if path.components().count() > 1 && executable_is_allowed(path, denied_root) {
        return Some(path.to_owned());
    }
    let mut directories =
        env::split_paths(&env::var_os("PATH").unwrap_or_default()).collect::<Vec<_>>();
    if let Some(home) = env::var_os("HOME").map(PathBuf::from) {
        directories.extend([home.join(".cargo/bin"), home.join(".local/bin")]);
    }
    directories.extend([
        PathBuf::from("/opt/homebrew/bin"),
        PathBuf::from("/usr/local/bin"),
    ]);
    let extensions: Vec<String> = if cfg!(windows) {
        env::var("PATHEXT")
            .unwrap_or_else(|_| ".EXE;.CMD;.BAT".into())
            .split(';')
            .map(str::to_ascii_lowercase)
            .collect()
    } else {
        vec![String::new()]
    };
    for directory in directories {
        if !directory.is_absolute() || path_is_within(&directory, denied_root) {
            continue;
        }
        for extension in &extensions {
            let candidate = directory.join(format!("{name}{extension}"));
            if executable_is_allowed(&candidate, denied_root) {
                return Some(candidate);
            }
        }
    }
    None
}

fn executable_is_allowed(path: &Path, denied_root: Option<&Path>) -> bool {
    path.is_absolute() && path.is_file() && !path_is_within(path, denied_root)
}

fn path_is_within(path: &Path, root: Option<&Path>) -> bool {
    let Some(root) = root else {
        return false;
    };
    let canonical_root = root.canonicalize().unwrap_or_else(|_| root.to_owned());
    let canonical_path = path.canonicalize().unwrap_or_else(|_| path.to_owned());
    canonical_path.starts_with(canonical_root)
}

fn safe_path_environment(root: &Path) -> std::ffi::OsString {
    let directories = env::split_paths(&env::var_os("PATH").unwrap_or_default())
        .filter(|directory| directory.is_absolute() && !path_is_within(directory, Some(root)))
        .collect::<Vec<_>>();
    env::join_paths(directories).unwrap_or_default()
}

fn isolated_server_home(root: &Path, kind: LanguageKind) -> Result<PathBuf, LanguageError> {
    let mut hash = 0xcbf29ce484222325_u64;
    for byte in root.to_string_lossy().as_bytes() {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    let path = env::temp_dir()
        .join("aegisy-language-servers")
        .join(format!("{hash:016x}"))
        .join(kind.server_id());
    fs::create_dir_all(path.join("cache")).map_err(|cause| {
        language_error(
            -32072,
            format!("cannot create isolated language-server home: {cause}"),
        )
    })?;
    Ok(path)
}

fn server_key(project_id: &str, kind: LanguageKind) -> String {
    format!("{project_id}:{}", kind.server_id())
}

fn truncate_chars(value: &str, limit: usize) -> String {
    if value.chars().count() <= limit {
        return value.into();
    }
    let mut truncated = value
        .chars()
        .take(limit.saturating_sub(1))
        .collect::<String>();
    truncated.push('…');
    truncated
}

fn language_error(code: i64, message: impl Into<String>) -> LanguageError {
    LanguageError {
        code,
        message: message.into(),
    }
}

impl From<WorkspaceError> for LanguageError {
    fn from(error: WorkspaceError) -> Self {
        Self {
            code: error.code,
            message: error.message,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn fixture() -> PathBuf {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root = env::temp_dir().join(format!("aegisy-lsp-test-{}-{unique}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        root.canonicalize().unwrap()
    }

    #[test]
    fn parses_bounded_lsp_frames_and_rejects_oversized_frames() {
        let body = br#"{"jsonrpc":"2.0","id":1,"result":null}"#;
        let frame = format!(
            "Content-Length: {}\r\nContent-Type: application/vscode-jsonrpc\r\n\r\n",
            body.len()
        );
        let mut bytes = frame.into_bytes();
        bytes.extend_from_slice(body);
        let parsed = read_lsp_message(&mut Cursor::new(bytes)).unwrap().unwrap();
        assert_eq!(parsed["id"], 1);

        let oversized = format!("Content-Length: {}\r\n\r\n", MAX_LSP_FRAME_BYTES + 1);
        assert_eq!(
            read_lsp_message(&mut Cursor::new(oversized))
                .unwrap_err()
                .kind(),
            io::ErrorKind::InvalidData
        );
    }

    #[test]
    fn file_uris_round_trip_unicode_and_reject_outside_or_symlinked_locations() {
        let root = fixture();
        let source = root.join("你好 file.cpp");
        fs::write(&source, "int main() {}\n").unwrap();
        let uri = file_uri(&source);
        assert_eq!(
            path_from_file_uri(&uri).unwrap(),
            crate::plain_path(&source)
        );
        assert_eq!(
            workspace_relative_from_uri(&root, &uri).unwrap(),
            "你好 file.cpp"
        );
        assert!(workspace_relative_from_uri(&root, "file:///etc/passwd").is_none());
        #[cfg(unix)]
        {
            std::os::unix::fs::symlink(&source, root.join("link.cpp")).unwrap();
            assert!(
                workspace_relative_from_uri(&root, &file_uri(&root.join("link.cpp"))).is_none()
            );
        }
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn detects_registered_language_servers_without_starting_them() {
        assert_eq!(
            LanguageKind::from_path("src/main.rs"),
            Some(LanguageKind::Rust)
        );
        assert_eq!(
            LanguageKind::from_path("src/view.tsx"),
            Some(LanguageKind::TypeScript)
        );
        assert_eq!(
            LanguageKind::from_path("src/main.cpp"),
            Some(LanguageKind::Cpp)
        );
        assert_eq!(LanguageKind::from_path("README.md"), None);
        let root = fixture();
        let statuses = LanguageServerManager::default().statuses("project-test", &root);
        assert_eq!(statuses.len(), 4);
        assert!(statuses.iter().all(|status| !status.running));
        let rust_options = safe_initialization_options(LanguageKind::Rust);
        assert_eq!(rust_options["cargo"]["buildScripts"]["enable"], false);
        assert_eq!(rust_options["procMacro"]["enable"], false);
        assert_eq!(rust_options["checkOnSave"], false);
        assert_eq!(rust_options["linkedProjects"], json!([]));
        let workspace_binary = root.join("clangd");
        fs::write(&workspace_binary, "not executable").unwrap();
        assert!(!executable_is_allowed(&workspace_binary, Some(&root)));
        assert!(!executable_is_allowed(Path::new("relative-server"), None));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn clangd_definition_and_diagnostics_work_when_clangd_is_available() {
        if LanguageKind::Cpp.executable(None).is_none() {
            return;
        }
        let root = fixture();
        let path = "main.cpp";
        let content = "int add(int a, int b) { return a + b; }\nint main() { return add(1, 2); }\n";
        fs::write(root.join(path), content).unwrap();
        let mut manager = LanguageServerManager::default();
        let definition = manager
            .definition("project-test", &root, path, content, "revision-1", 2, 21)
            .unwrap();
        assert!(definition
            .locations
            .iter()
            .any(|location| location.path == path && location.line == 1));

        let broken = "int main() { return missing_name; }\n";
        fs::write(root.join(path), broken).unwrap();
        let diagnostics = manager
            .diagnostics("project-test", &root, path, broken, "revision-2")
            .unwrap();
        assert!(!diagnostics.pending);
        assert!(diagnostics
            .diagnostics
            .iter()
            .any(|diagnostic| diagnostic.severity == "error"));
        manager.shutdown_all();
        fs::remove_dir_all(root).unwrap();
    }
}
