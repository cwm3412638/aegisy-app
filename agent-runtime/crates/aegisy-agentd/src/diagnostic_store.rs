use crate::command_output::CommandOutputSnapshot;
use crate::language_server::{LanguageDiagnostic, LanguageDiagnosticsResult};
use serde::Serialize;
use serde_json::json;
use sha2::{Digest, Sha256};
use std::collections::{HashMap, VecDeque};
use std::time::{SystemTime, UNIX_EPOCH};

const MAX_PROJECT_DIAGNOSTICS: usize = 2_000;
const MAX_PROJECT_ARTIFACTS: usize = 128;
const MAX_PROJECT_ARTIFACT_BYTES: usize = 4 * 1024 * 1024;
const MAX_ARTIFACT_BYTES: usize = 256 * 1024;

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct ObservedDiagnostic {
    pub id: String,
    pub path: String,
    pub line: usize,
    pub column: usize,
    pub end_line: usize,
    pub end_column: usize,
    pub severity: String,
    pub code: Option<String>,
    pub message: String,
    pub source_kind: String,
    pub source_identity: String,
    pub source_server: Option<String>,
    pub source_command: Option<String>,
    pub file_hash: String,
    pub observed_at_ms: u64,
    pub freshness: String,
    pub stale_at_ms: Option<u64>,
    pub raw_output_ref: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct DiagnosticObservation {
    pub diagnostics: Vec<ObservedDiagnostic>,
    pub raw_output_ref: String,
    pub observed_at_ms: u64,
    pub truncated: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct ObservedDiagnosticsResult {
    pub diagnostics: Vec<ObservedDiagnostic>,
    pub fresh_count: usize,
    pub stale_count: usize,
    pub truncated: bool,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct RawDiagnosticArtifact {
    pub reference: String,
    pub sha256: String,
    pub content_type: String,
    pub size: usize,
    pub source_kind: String,
    pub source_identity: String,
    pub created_at_ms: u64,
    pub truncated: bool,
    pub content: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct CommandDiagnosticInput {
    pub path: String,
    pub line: usize,
    pub column: usize,
    pub end_line: usize,
    pub end_column: usize,
    pub severity: String,
    pub code: Option<String>,
    pub message: String,
    pub file_hash: String,
}

#[derive(Default)]
pub struct DiagnosticStore {
    projects: HashMap<String, ProjectDiagnostics>,
}

#[derive(Default)]
struct ProjectDiagnostics {
    records: HashMap<String, Vec<ObservedDiagnostic>>,
    record_order: VecDeque<String>,
    artifacts: HashMap<String, RawDiagnosticArtifact>,
    artifact_order: VecDeque<String>,
    artifact_bytes: usize,
    truncated: bool,
}

impl DiagnosticStore {
    pub fn record_language_server(
        &mut self,
        project_id: &str,
        path: &str,
        result: &LanguageDiagnosticsResult,
    ) -> DiagnosticObservation {
        let observed_at_ms = now_ms();
        let (artifact, raw_truncated) = language_artifact(path, result, observed_at_ms);
        let raw_output_ref = artifact.reference.clone();
        let source_key = format!("language-server:{}:{path}", result.server_id);
        let diagnostics = result
            .diagnostics
            .iter()
            .map(|diagnostic| {
                observed_language_diagnostic(
                    diagnostic,
                    &result.server_id,
                    &result.revision,
                    observed_at_ms,
                    &raw_output_ref,
                )
            })
            .collect::<Vec<_>>();
        let project = self.projects.entry(project_id.into()).or_default();
        project.insert_artifact(artifact);
        project.insert_records(source_key, diagnostics.clone());
        DiagnosticObservation {
            diagnostics,
            raw_output_ref,
            observed_at_ms,
            truncated: raw_truncated || project.truncated,
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub fn record_command(
        &mut self,
        project_id: &str,
        item_id: &str,
        toolchain: &str,
        command: &str,
        output: &CommandOutputSnapshot,
        command_output_ref: Option<&str>,
        redacted_count: u64,
        diagnostics: &[CommandDiagnosticInput],
        parse_truncated: bool,
    ) -> DiagnosticObservation {
        let observed_at_ms = now_ms();
        let command = bounded_utf8(command, 1_024);
        let (artifact, raw_truncated) = command_artifact(
            item_id,
            toolchain,
            &command,
            output,
            command_output_ref,
            redacted_count,
            diagnostics,
            parse_truncated,
            observed_at_ms,
        );
        let raw_output_ref = artifact.reference.clone();
        let source_identity = format!("command:{toolchain}");
        let records = diagnostics
            .iter()
            .map(|diagnostic| {
                observed_command_diagnostic(
                    diagnostic,
                    item_id,
                    &source_identity,
                    &command,
                    observed_at_ms,
                    &raw_output_ref,
                )
            })
            .collect::<Vec<_>>();
        let project = self.projects.entry(project_id.into()).or_default();
        project.insert_artifact(artifact);
        project.insert_records(format!("command:{item_id}"), records.clone());
        DiagnosticObservation {
            diagnostics: records,
            raw_output_ref,
            observed_at_ms,
            truncated: raw_truncated || project.truncated,
        }
    }

    pub fn list(
        &self,
        project_id: &str,
        path: Option<&str>,
        include_stale: bool,
    ) -> ObservedDiagnosticsResult {
        let Some(project) = self.projects.get(project_id) else {
            return ObservedDiagnosticsResult {
                diagnostics: Vec::new(),
                fresh_count: 0,
                stale_count: 0,
                truncated: false,
            };
        };
        let mut diagnostics = project
            .records
            .values()
            .flatten()
            .filter(|diagnostic| path.is_none_or(|path| diagnostic.path == path))
            .filter(|diagnostic| include_stale || diagnostic.freshness == "fresh")
            .cloned()
            .collect::<Vec<_>>();
        diagnostics.sort_by(|left, right| {
            right
                .observed_at_ms
                .cmp(&left.observed_at_ms)
                .then_with(|| left.path.cmp(&right.path))
                .then_with(|| left.line.cmp(&right.line))
                .then_with(|| left.column.cmp(&right.column))
        });
        let fresh_count = diagnostics
            .iter()
            .filter(|diagnostic| diagnostic.freshness == "fresh")
            .count();
        let stale_count = diagnostics.len().saturating_sub(fresh_count);
        ObservedDiagnosticsResult {
            diagnostics,
            fresh_count,
            stale_count,
            truncated: project.truncated,
        }
    }

    pub fn list_scoped(
        &self,
        primary_project_id: &str,
        scoped_project_id: &str,
        path: Option<&str>,
        include_stale: bool,
    ) -> ObservedDiagnosticsResult {
        let mut result = self.list(primary_project_id, path, include_stale);
        if scoped_project_id == primary_project_id {
            return result;
        }
        let scoped = self.list(scoped_project_id, path, include_stale);
        result.diagnostics.extend(scoped.diagnostics);
        result.diagnostics.sort_by(|left, right| {
            right
                .observed_at_ms
                .cmp(&left.observed_at_ms)
                .then_with(|| left.path.cmp(&right.path))
                .then_with(|| left.line.cmp(&right.line))
                .then_with(|| left.column.cmp(&right.column))
        });
        result.fresh_count += scoped.fresh_count;
        result.stale_count += scoped.stale_count;
        result.truncated |= scoped.truncated;
        result
    }

    pub fn raw(&self, project_id: &str, reference: &str) -> Option<RawDiagnosticArtifact> {
        self.projects
            .get(project_id)?
            .artifacts
            .get(reference)
            .cloned()
    }

    pub fn invalidate_path(&mut self, project_id: &str, path: &str) {
        let Some(project) = self.projects.get_mut(project_id) else {
            return;
        };
        let stale_at_ms = now_ms();
        for diagnostic in project.records.values_mut().flatten() {
            if diagnostic.path == path && diagnostic.freshness != "stale" {
                diagnostic.freshness = "stale".into();
                diagnostic.stale_at_ms = Some(stale_at_ms);
            }
        }
    }
}

impl ProjectDiagnostics {
    fn insert_records(&mut self, key: String, diagnostics: Vec<ObservedDiagnostic>) {
        self.record_order.retain(|existing| existing != &key);
        self.records.insert(key.clone(), diagnostics);
        self.record_order.push_back(key);
        while self.diagnostic_count() > MAX_PROJECT_DIAGNOSTICS {
            let Some(oldest) = self.record_order.pop_front() else {
                break;
            };
            self.records.remove(&oldest);
            self.truncated = true;
        }
    }

    fn insert_artifact(&mut self, artifact: RawDiagnosticArtifact) {
        if self.artifacts.contains_key(&artifact.reference) {
            return;
        }
        self.artifact_bytes = self.artifact_bytes.saturating_add(artifact.size);
        self.artifact_order.push_back(artifact.reference.clone());
        self.artifacts.insert(artifact.reference.clone(), artifact);
        while self.artifacts.len() > MAX_PROJECT_ARTIFACTS
            || self.artifact_bytes > MAX_PROJECT_ARTIFACT_BYTES
        {
            let Some(oldest) = self.artifact_order.pop_front() else {
                break;
            };
            if let Some(removed) = self.artifacts.remove(&oldest) {
                self.artifact_bytes = self.artifact_bytes.saturating_sub(removed.size);
                self.truncated = true;
            }
        }
    }

    fn diagnostic_count(&self) -> usize {
        self.records.values().map(Vec::len).sum()
    }
}

fn observed_language_diagnostic(
    diagnostic: &LanguageDiagnostic,
    server_id: &str,
    file_hash: &str,
    observed_at_ms: u64,
    raw_output_ref: &str,
) -> ObservedDiagnostic {
    let identity = json!({
        "path": diagnostic.path,
        "line": diagnostic.line,
        "column": diagnostic.column,
        "end_line": diagnostic.end_line,
        "end_column": diagnostic.end_column,
        "severity": diagnostic.severity,
        "code": diagnostic.code,
        "message": diagnostic.message,
        "source": server_id,
        "file_hash": file_hash
    });
    let id = format!("diagnostic:{}", sha256_hex(identity.to_string().as_bytes()));
    ObservedDiagnostic {
        id,
        path: diagnostic.path.clone(),
        line: diagnostic.line,
        column: diagnostic.column,
        end_line: diagnostic.end_line,
        end_column: diagnostic.end_column,
        severity: diagnostic.severity.clone(),
        code: diagnostic.code.clone(),
        message: diagnostic.message.clone(),
        source_kind: "language-server".into(),
        source_identity: server_id.into(),
        source_server: Some(server_id.into()),
        source_command: None,
        file_hash: file_hash.into(),
        observed_at_ms,
        freshness: "fresh".into(),
        stale_at_ms: None,
        raw_output_ref: raw_output_ref.into(),
    }
}

fn observed_command_diagnostic(
    diagnostic: &CommandDiagnosticInput,
    item_id: &str,
    source_identity: &str,
    command: &str,
    observed_at_ms: u64,
    raw_output_ref: &str,
) -> ObservedDiagnostic {
    let identity = json!({
        "path": diagnostic.path,
        "line": diagnostic.line,
        "column": diagnostic.column,
        "end_line": diagnostic.end_line,
        "end_column": diagnostic.end_column,
        "severity": diagnostic.severity,
        "code": diagnostic.code,
        "message": diagnostic.message,
        "source": source_identity,
        "item_id": item_id,
        "file_hash": diagnostic.file_hash
    });
    ObservedDiagnostic {
        id: format!("diagnostic:{}", sha256_hex(identity.to_string().as_bytes())),
        path: diagnostic.path.clone(),
        line: diagnostic.line,
        column: diagnostic.column,
        end_line: diagnostic.end_line,
        end_column: diagnostic.end_column,
        severity: diagnostic.severity.clone(),
        code: diagnostic.code.clone(),
        message: diagnostic.message.clone(),
        source_kind: "command".into(),
        source_identity: source_identity.into(),
        source_server: None,
        source_command: Some(command.into()),
        file_hash: diagnostic.file_hash.clone(),
        observed_at_ms,
        freshness: "fresh".into(),
        stale_at_ms: None,
        raw_output_ref: raw_output_ref.into(),
    }
}

fn language_artifact(
    path: &str,
    result: &LanguageDiagnosticsResult,
    created_at_ms: u64,
) -> (RawDiagnosticArtifact, bool) {
    let mut diagnostics = result.diagnostics.clone();
    let mut truncated = result.truncated;
    let content = loop {
        let payload = json!({
            "source_kind": "language-server",
            "source_identity": result.server_id,
            "path": path,
            "file_hash": result.revision,
            "observed_at_ms": created_at_ms,
            "pending": result.pending,
            "truncated": truncated,
            "diagnostics": diagnostics
        });
        let content = serde_json::to_string(&payload).expect("diagnostic artifact serialization");
        if content.len() <= MAX_ARTIFACT_BYTES || diagnostics.is_empty() {
            break content;
        }
        diagnostics.pop();
        truncated = true;
    };
    let sha256 = sha256_hex(content.as_bytes());
    let reference = format!("diagnostic-raw:sha256:{sha256}");
    (
        RawDiagnosticArtifact {
            reference,
            sha256,
            content_type: "application/vnd.aegisy.diagnostics+json".into(),
            size: content.len(),
            source_kind: "language-server".into(),
            source_identity: result.server_id.clone(),
            created_at_ms,
            truncated,
            content,
        },
        truncated,
    )
}

#[allow(clippy::too_many_arguments)]
fn command_artifact(
    item_id: &str,
    toolchain: &str,
    command: &str,
    output: &CommandOutputSnapshot,
    command_output_ref: Option<&str>,
    redacted_count: u64,
    diagnostics: &[CommandDiagnosticInput],
    parse_truncated: bool,
    created_at_ms: u64,
) -> (RawDiagnosticArtifact, bool) {
    let mut diagnostics = diagnostics.to_vec();
    let mut truncated = parse_truncated || output.truncated;
    let content = loop {
        let payload = json!({
            "source_kind": "command",
            "source_identity": format!("command:{toolchain}"),
            "command_item_id": item_id,
            "source_command": command,
            "observed_at_ms": created_at_ms,
            "command_output_ref": command_output_ref,
            "output_total_bytes": output.total_bytes,
            "output_omitted_bytes": output.omitted_bytes,
            "output_redacted_count": redacted_count,
            "truncated": truncated,
            "diagnostics": diagnostics
        });
        let content = serde_json::to_string(&payload).expect("command diagnostic serialization");
        if content.len() <= MAX_ARTIFACT_BYTES {
            break content;
        }
        if diagnostics.pop().is_some() {
            truncated = true;
            continue;
        }
        break content;
    };
    let sha256 = sha256_hex(content.as_bytes());
    (
        RawDiagnosticArtifact {
            reference: format!("diagnostic-raw:sha256:{sha256}"),
            sha256,
            content_type: "application/vnd.aegisy.diagnostics+json".into(),
            size: content.len(),
            source_kind: "command".into(),
            source_identity: format!("command:{toolchain}"),
            created_at_ms,
            truncated,
            content,
        },
        truncated,
    )
}

fn bounded_utf8(value: &str, limit: usize) -> String {
    if value.len() <= limit {
        return value.into();
    }
    let mut end = limit;
    while end > 0 && !value.is_char_boundary(end) {
        end -= 1;
    }
    value[..end].into()
}

fn sha256_hex(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    digest.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

#[cfg(test)]
mod tests {
    use super::*;

    fn language_result(revision: &str, message: &str) -> LanguageDiagnosticsResult {
        LanguageDiagnosticsResult {
            language: "C / C++".into(),
            server_id: "clangd".into(),
            revision: revision.into(),
            stale: false,
            pending: false,
            truncated: false,
            diagnostics: vec![LanguageDiagnostic {
                path: "src/main.cpp".into(),
                line: 2,
                column: 4,
                end_line: 2,
                end_column: 8,
                severity: "error".into(),
                code: Some("undeclared_var_use_suggest".into()),
                message: message.into(),
                source: "clang".into(),
                provenance: "language-server:clangd".into(),
            }],
        }
    }

    #[test]
    fn records_content_addressed_observations_and_raw_authority() {
        let mut store = DiagnosticStore::default();
        let observation = store.record_language_server(
            "project-1",
            "src/main.cpp",
            &language_result("content:first", "missing name"),
        );
        assert_eq!(observation.diagnostics.len(), 1);
        let diagnostic = &observation.diagnostics[0];
        assert!(diagnostic.id.starts_with("diagnostic:"));
        assert_eq!(diagnostic.source_server.as_deref(), Some("clangd"));
        assert_eq!(diagnostic.file_hash, "content:first");
        assert_eq!(diagnostic.freshness, "fresh");
        let raw = store.raw("project-1", &observation.raw_output_ref).unwrap();
        assert_eq!(raw.sha256.len(), 64);
        assert!(raw.content.contains("missing name"));
        assert!(!raw.content.contains("source content"));
    }

    #[test]
    fn invalidates_and_replaces_source_file_snapshots() {
        let mut store = DiagnosticStore::default();
        store.record_language_server(
            "project-1",
            "src/main.cpp",
            &language_result("content:first", "first"),
        );
        store.invalidate_path("project-1", "src/main.cpp");
        let stale = store.list("project-1", None, true);
        assert_eq!(stale.stale_count, 1);
        assert_eq!(stale.diagnostics[0].freshness, "stale");
        assert!(stale.diagnostics[0].stale_at_ms.is_some());
        assert!(store.list("project-1", None, false).diagnostics.is_empty());

        store.record_language_server(
            "project-1",
            "src/main.cpp",
            &language_result("content:second", "second"),
        );
        let refreshed = store.list("project-1", Some("src/main.cpp"), true);
        assert_eq!(refreshed.diagnostics.len(), 1);
        assert_eq!(refreshed.fresh_count, 1);
        assert_eq!(refreshed.diagnostics[0].message, "second");
        assert_eq!(refreshed.diagnostics[0].file_hash, "content:second");
    }

    #[test]
    fn bounds_large_raw_diagnostic_payloads() {
        let mut result = language_result("content:large", "message");
        result.diagnostics = (0..500)
            .map(|index| LanguageDiagnostic {
                message: format!("{index}:{}", "x".repeat(1_900)),
                ..result.diagnostics[0].clone()
            })
            .collect();
        let mut store = DiagnosticStore::default();
        let observation = store.record_language_server("project-1", "src/main.cpp", &result);
        let raw = store.raw("project-1", &observation.raw_output_ref).unwrap();
        assert!(raw.size <= MAX_ARTIFACT_BYTES);
        assert!(raw.truncated);
    }

    #[test]
    fn records_command_diagnostics_with_filtered_raw_reference_authority() {
        let mut capture = crate::command_output::CommandOutputCapture::default();
        capture.append("src/main.rs:2:4: error: missing value\n");
        let output = capture.snapshot();
        let input = CommandDiagnosticInput {
            path: "src/main.rs".into(),
            line: 2,
            column: 4,
            end_line: 2,
            end_column: 5,
            severity: "error".into(),
            code: Some("E0425".into()),
            message: "missing value".into(),
            file_hash: "content:fixture".into(),
        };
        let mut store = DiagnosticStore::default();
        let observation = store.record_command(
            "project-1",
            "command-1",
            "rustc",
            "cargo check --token [REDACTED]",
            &output,
            Some("command-output:sha256:fixture"),
            1,
            &[input],
            false,
        );
        assert_eq!(observation.diagnostics[0].source_kind, "command");
        assert_eq!(observation.diagnostics[0].source_identity, "command:rustc");
        assert_eq!(
            observation.diagnostics[0].source_command.as_deref(),
            Some("cargo check --token [REDACTED]")
        );
        let raw = store.raw("project-1", &observation.raw_output_ref).unwrap();
        assert!(raw.content.contains("command-output:sha256:fixture"));
        assert!(raw.content.contains("src/main.rs"));
        assert!(!raw.content.contains("src/main.rs:2:4: error"));
        assert!(!raw.content.contains("output_excerpt"));
    }
}
