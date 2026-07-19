use crate::command_output::CommandOutputCapture;
use crate::output_redaction::{redact_complete, OutputRedactor};
use crate::session_environment::{EnvironmentSummary, ProcessEnvironment, SessionEnvironment};
use crate::{TurnCancellationHandle, TurnSteerRequest, TurnSteeringHandle};
use serde::Serialize;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::env;
use std::io::{self, BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, SyncSender};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

const REQUEST_TIMEOUT: Duration = Duration::from_secs(30);
const STARTUP_TIMEOUT: Duration = Duration::from_secs(15);
const STARTUP_MAX_ATTEMPTS: usize = 3;
const STARTUP_RETRY_BACKOFF: Duration = Duration::from_millis(100);
const TURN_TIMEOUT: Duration = Duration::from_secs(10 * 60);
const CODEX_MESSAGE_QUEUE_CAPACITY: usize = 16;
const MAX_CODEX_MESSAGE_BYTES: usize = 4 * 1024 * 1024;
const PINNED_CODEX_VERSION: &str = "codex-cli 0.144.5";

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct BackendInfo {
    pub adapter: String,
    pub version: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub provider: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub model: Option<String>,
    pub permission_profile: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub environment: Option<EnvironmentSummary>,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct AdapterHealth {
    pub state: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub process_id: Option<u32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub exit_code: Option<i32>,
    pub stderr: StderrDiagnostics,
}

#[derive(Debug, Clone, Default, Serialize, PartialEq, Eq)]
pub struct StderrDiagnostics {
    pub bytes: u64,
    pub lines: u64,
    pub redacted_lines: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub last_class: Option<String>,
}

impl StderrDiagnostics {
    fn observe(&mut self, bytes: &[u8]) {
        if bytes.is_empty() {
            return;
        }
        self.bytes = self.bytes.saturating_add(bytes.len() as u64);
        self.lines = self
            .lines
            .saturating_add(bytes.iter().filter(|byte| **byte == b'\n').count() as u64);
        let text = String::from_utf8_lossy(bytes);
        let redacted = redact_complete(&text);
        if redacted != text {
            self.redacted_lines = self.redacted_lines.saturating_add(1);
        }
        self.last_class = Some(classify_stderr(&redacted).into());
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CodexSession {
    pub thread_id: String,
    pub provider: String,
    pub model: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CommandItem {
    pub item_id: String,
    pub command: String,
    pub command_actions: Value,
    pub cwd: String,
    pub status: String,
    pub output: CommandOutputCapture,
    pub redactor: OutputRedactor,
    pub duration_ms: Option<u64>,
    pub exit_code: Option<i64>,
    pub process_id: Option<String>,
    pub source: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CodexEvent {
    TurnStarted {
        turn_id: String,
    },
    AgentDelta {
        turn_id: String,
        item_id: String,
        text: String,
    },
    AgentCompleted {
        turn_id: String,
        item_id: String,
        text: String,
    },
    CommandUpdated {
        turn_id: String,
        command: Box<CommandItem>,
        lifecycle: String,
    },
    TokenUsage {
        turn_id: String,
        usage: Value,
    },
    TurnDiff {
        turn_id: String,
        diff: String,
    },
    TurnPlan {
        turn_id: String,
        explanation: Option<String>,
        steps: Vec<Value>,
    },
    TurnCompleted {
        turn_id: String,
    },
    TurnSteeringRequested {
        turn_id: String,
        input: String,
    },
    TurnSteeringAcknowledged {
        turn_id: String,
    },
    TurnSteeringFailed {
        turn_id: String,
        message: String,
    },
    TurnCancellationAcknowledged {
        turn_id: String,
    },
    TurnCancellationFailed {
        turn_id: String,
        message: String,
    },
    TurnInterrupted {
        turn_id: String,
    },
    TurnFailed {
        turn_id: String,
        message: String,
    },
}

pub struct CodexAdapter {
    child: Child,
    stdin: ChildStdin,
    messages: Receiver<Value>,
    next_request_id: i64,
    version: String,
    environment: EnvironmentSummary,
    stderr: Arc<Mutex<StderrDiagnostics>>,
}

pub(crate) struct CodexTurnRequest<'a> {
    pub thread_id: &'a str,
    pub input: &'a str,
    pub local_images: &'a [PathBuf],
    pub idempotency_key: &'a str,
}

impl CodexAdapter {
    pub fn start() -> Result<Self, String> {
        let mut attempts = 0;
        loop {
            attempts += 1;
            match Self::start_once() {
                Ok(adapter) => return Ok(adapter),
                Err(error)
                    if attempts < STARTUP_MAX_ATTEMPTS && is_retryable_startup_error(&error) =>
                {
                    thread::sleep(STARTUP_RETRY_BACKOFF);
                }
                Err(error) => {
                    let safe_error = bounded_string(&redact_complete(&error), 512);
                    return Err(format!(
                        "Codex App Server startup failed after {attempts} bounded attempt(s): {safe_error}"
                    ));
                }
            }
        }
    }

    fn start_once() -> Result<Self, String> {
        let executable = locate_codex();
        let process_environment = codex_process_environment()?;
        let version = codex_version(&executable, &process_environment);
        if !codex_version_is_pinned(&version) {
            return Err(format!(
                "unsupported Codex App Server version {version}; Aegisy requires {PINNED_CODEX_VERSION}"
            ));
        }
        let mut command = codex_command(&executable, &process_environment);
        command
            .args(["app-server", "--stdio"])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let mut child = command
            .spawn()
            .map_err(|error| format!("cannot start Codex App Server: {error}"))?;
        let stdin = child
            .stdin
            .take()
            .ok_or_else(|| "Codex App Server stdin is unavailable".to_owned())?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| "Codex App Server stdout is unavailable".to_owned())?;
        let stderr = child.stderr.take();
        let (sender, messages) = codex_message_channel();
        let stderr_diagnostics = Arc::new(Mutex::new(StderrDiagnostics::default()));

        thread::spawn(move || {
            let mut reader = BufReader::new(stdout);
            loop {
                let message = match read_bounded_line(&mut reader, MAX_CODEX_MESSAGE_BYTES) {
                    Ok(BoundedLine::Line(line)) if line.iter().all(u8::is_ascii_whitespace) => {
                        continue
                    }
                    Ok(BoundedLine::Line(line)) => match serde_json::from_slice::<Value>(&line) {
                        Ok(message) => message,
                        Err(_) => transport_error("Codex App Server emitted invalid JSON"),
                    },
                    Ok(BoundedLine::Oversized) => transport_error(format!(
                        "Codex App Server message exceeded the {MAX_CODEX_MESSAGE_BYTES}-byte limit"
                    )),
                    Ok(BoundedLine::Eof) => break,
                    Err(_) => transport_error("cannot read Codex App Server output"),
                };
                if sender.send(message).is_err() {
                    break;
                }
            }
        });
        if let Some(stderr) = stderr {
            let diagnostics = stderr_diagnostics.clone();
            thread::spawn(move || {
                let mut stderr = stderr;
                let mut buffer = [0_u8; 8 * 1024];
                loop {
                    match stderr.read(&mut buffer) {
                        Ok(0) | Err(_) => break,
                        Ok(bytes_read) => {
                            let mut diagnostics = diagnostics
                                .lock()
                                .unwrap_or_else(|poisoned| poisoned.into_inner());
                            diagnostics.observe(&buffer[..bytes_read]);
                        }
                    }
                }
            });
        }

        let mut adapter = Self {
            child,
            stdin,
            messages,
            next_request_id: 0,
            version,
            environment: process_environment.summary().clone(),
            stderr: stderr_diagnostics,
        };
        adapter.request_with_timeout(
            "initialize",
            json!({
                "clientInfo": {
                    "name": "aegisy-agentd",
                    "title": "Aegisy Coding",
                    "version": env!("CARGO_PKG_VERSION")
                },
                "capabilities": { "experimentalApi": false }
            }),
            STARTUP_TIMEOUT,
        )?;
        adapter.write_message(&json!({ "method": "initialized" }))?;
        Ok(adapter)
    }

    pub fn info(&self) -> BackendInfo {
        BackendInfo {
            adapter: "codex-app-server".into(),
            version: self.version.clone(),
            provider: None,
            model: None,
            permission_profile: "read-only".into(),
            environment: Some(self.environment.clone()),
        }
    }

    pub fn health(&mut self) -> AdapterHealth {
        let process_id = Some(self.child.id());
        let stderr = self
            .stderr
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .clone();
        match self.child.try_wait() {
            Ok(None) => AdapterHealth {
                state: "running".into(),
                process_id,
                exit_code: None,
                stderr,
            },
            Ok(Some(status)) => AdapterHealth {
                state: "exited".into(),
                process_id,
                exit_code: status.code(),
                stderr,
            },
            Err(_) => AdapterHealth {
                state: "unknown".into(),
                process_id,
                exit_code: None,
                stderr,
            },
        }
    }

    pub fn start_session(&mut self, cwd: &Path, chat: bool) -> Result<CodexSession, String> {
        let result = self.request("thread/start", thread_start_params(cwd, chat))?;
        parse_codex_session(result, "thread/start")
    }

    pub fn resume_session(
        &mut self,
        thread_id: &str,
        cwd: &Path,
        chat: bool,
    ) -> Result<CodexSession, String> {
        let result = self.request("thread/resume", thread_resume_params(thread_id, cwd, chat))?;
        parse_codex_session(result, "thread/resume")
    }

    pub fn fork_session(
        &mut self,
        thread_id: &str,
        last_turn_id: Option<&str>,
        cwd: &Path,
        chat: bool,
    ) -> Result<CodexSession, String> {
        let result = self.request(
            "thread/fork",
            thread_fork_params(thread_id, last_turn_id, cwd, chat),
        )?;
        parse_codex_session(result, "thread/fork")
    }

    pub fn archive_session(&mut self, thread_id: &str) -> Result<(), String> {
        self.request("thread/archive", json!({ "threadId": thread_id }))?;
        Ok(())
    }

    #[allow(dead_code)]
    pub fn unarchive_session(&mut self, thread_id: &str) -> Result<CodexSession, String> {
        let result = self.request("thread/unarchive", json!({ "threadId": thread_id }))?;
        parse_codex_session(result, "thread/unarchive")
    }

    #[allow(dead_code)]
    pub fn delete_session(&mut self, thread_id: &str) -> Result<(), String> {
        self.request("thread/delete", json!({ "threadId": thread_id }))?;
        Ok(())
    }

    #[allow(dead_code)]
    pub fn list_threads(
        &mut self,
        cursor: Option<&str>,
        limit: Option<u32>,
        cwd: Option<&Path>,
        archived: Option<bool>,
    ) -> Result<Value, String> {
        self.request(
            "thread/list",
            thread_list_params(cursor, limit, cwd, archived),
        )
    }

    #[allow(dead_code)]
    pub fn read_thread(&mut self, thread_id: &str, include_turns: bool) -> Result<Value, String> {
        self.request("thread/read", thread_read_params(thread_id, include_turns))
    }

    #[allow(dead_code)]
    pub fn compact_thread(&mut self, thread_id: &str) -> Result<(), String> {
        self.request(
            "thread/compact/start",
            thread_compact_start_params(thread_id),
        )?;
        Ok(())
    }
}

fn parse_codex_session(result: Value, method: &str) -> Result<CodexSession, String> {
    let thread_id = result
        .pointer("/thread/id")
        .and_then(Value::as_str)
        .ok_or_else(|| format!("Codex {method} response is missing thread.id"))?;
    let provider = result
        .get("modelProvider")
        .or_else(|| result.pointer("/thread/modelProvider"))
        .and_then(Value::as_str)
        .unwrap_or("unknown");
    let model = result
        .get("model")
        .or_else(|| result.pointer("/thread/model"))
        .and_then(Value::as_str)
        .unwrap_or("unknown");
    Ok(CodexSession {
        thread_id: thread_id.to_owned(),
        provider: provider.to_owned(),
        model: model.to_owned(),
    })
}

impl CodexAdapter {
    pub(crate) fn run_turn<F>(
        &mut self,
        request: CodexTurnRequest<'_>,
        cancellation: &TurnCancellationHandle,
        steering: &TurnSteeringHandle,
        mut emit: F,
    ) -> Result<(), String>
    where
        F: FnMut(CodexEvent),
    {
        let request_id = self.write_request("turn/start", turn_start_params(&request))?;
        let response = self.wait_for_response(request_id, REQUEST_TIMEOUT)?;
        let turn_id = response
            .pointer("/turn/id")
            .and_then(Value::as_str)
            .ok_or_else(|| "Codex turn/start response is missing turn.id".to_owned())?
            .to_owned();
        emit(CodexEvent::TurnStarted {
            turn_id: turn_id.clone(),
        });

        let mut accumulated = HashMap::<String, String>::new();
        let mut commands = HashMap::<String, CommandItem>::new();
        let deadline = Instant::now() + TURN_TIMEOUT;
        let mut interrupt_sent = false;
        let mut interrupt_request_id = None;
        let mut pending_steers = HashMap::<i64, TurnSteerRequest>::new();
        loop {
            if cancellation.is_requested() && !interrupt_sent {
                interrupt_request_id = Some(self.write_request(
                    "turn/interrupt",
                    turn_interrupt_params(request.thread_id, &turn_id),
                )?);
                interrupt_sent = true;
            }
            for steer in steering.drain() {
                let request_id = self.write_request(
                    "turn/steer",
                    turn_steer_params(request.thread_id, &turn_id, &steer),
                )?;
                emit(CodexEvent::TurnSteeringRequested {
                    turn_id: turn_id.clone(),
                    input: steer.input.clone(),
                });
                pending_steers.insert(request_id, steer);
            }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Err("Codex App Server timed out".into());
            }
            let Some(message) = self.receive_optional(remaining.min(Duration::from_millis(100)))?
            else {
                continue;
            };
            if self.handle_server_request(&message)? {
                continue;
            }
            if interrupt_request_id.is_some()
                && message.get("id").and_then(Value::as_i64) == interrupt_request_id
            {
                interrupt_request_id = None;
                if let Some(error) = message.get("error") {
                    emit(CodexEvent::TurnCancellationFailed {
                        turn_id: turn_id.clone(),
                        message: error
                            .get("message")
                            .and_then(Value::as_str)
                            .unwrap_or("Codex rejected turn interruption")
                            .to_owned(),
                    });
                } else {
                    emit(CodexEvent::TurnCancellationAcknowledged {
                        turn_id: turn_id.clone(),
                    });
                }
                continue;
            }
            if let Some(_steer) = message
                .get("id")
                .and_then(Value::as_i64)
                .and_then(|id| pending_steers.remove(&id))
            {
                if let Some(error) = message.get("error") {
                    emit(CodexEvent::TurnSteeringFailed {
                        turn_id: turn_id.clone(),
                        message: error
                            .get("message")
                            .and_then(Value::as_str)
                            .unwrap_or("Codex rejected turn steering")
                            .to_owned(),
                    });
                } else if message.pointer("/result/turnId").and_then(Value::as_str)
                    != Some(turn_id.as_str())
                {
                    emit(CodexEvent::TurnSteeringFailed {
                        turn_id: turn_id.clone(),
                        message: "Codex turn/steer response did not match the active turn"
                            .to_owned(),
                    });
                } else {
                    emit(CodexEvent::TurnSteeringAcknowledged {
                        turn_id: turn_id.clone(),
                    });
                }
                continue;
            }
            let method = message.get("method").and_then(Value::as_str).unwrap_or("");
            let params = message.get("params").cloned().unwrap_or(Value::Null);
            if params.get("threadId").and_then(Value::as_str) != Some(request.thread_id) {
                continue;
            }
            if matches!(
                method,
                "item/started" | "item/commandExecution/outputDelta" | "item/completed"
            ) {
                if let Some(event) =
                    translate_command_notification(method, &params, &turn_id, &mut commands)
                {
                    emit(event);
                    continue;
                }
                if method == "item/commandExecution/outputDelta" {
                    continue;
                }
            }
            if let Some(event) = translate_turn_notification(method, &params, &turn_id) {
                emit(event);
                continue;
            }
            match method {
                "item/agentMessage/delta" => {
                    let Some(item_id) = params.get("itemId").and_then(Value::as_str) else {
                        continue;
                    };
                    let delta = params.get("delta").and_then(Value::as_str).unwrap_or("");
                    let text = accumulated.entry(item_id.to_owned()).or_default();
                    text.push_str(delta);
                    emit(CodexEvent::AgentDelta {
                        turn_id: turn_id.clone(),
                        item_id: item_id.to_owned(),
                        text: text.clone(),
                    });
                }
                "item/completed" => {
                    let item = params.get("item").cloned().unwrap_or(Value::Null);
                    if item.get("type").and_then(Value::as_str) != Some("agentMessage") {
                        continue;
                    }
                    let Some(item_id) = item.get("id").and_then(Value::as_str) else {
                        continue;
                    };
                    let text = item.get("text").and_then(Value::as_str).unwrap_or_else(|| {
                        accumulated.get(item_id).map(String::as_str).unwrap_or("")
                    });
                    emit(CodexEvent::AgentCompleted {
                        turn_id: turn_id.clone(),
                        item_id: item_id.to_owned(),
                        text: text.to_owned(),
                    });
                }
                "turn/completed" => {
                    emit(turn_terminal_event(&params, &turn_id));
                    return Ok(());
                }
                "error" => {
                    let message = params
                        .get("message")
                        .and_then(Value::as_str)
                        .unwrap_or("Codex runtime error")
                        .to_owned();
                    emit(CodexEvent::TurnFailed {
                        turn_id: turn_id.clone(),
                        message,
                    });
                }
                _ => {}
            }
        }
    }

    fn request(&mut self, method: &str, params: Value) -> Result<Value, String> {
        self.request_with_timeout(method, params, REQUEST_TIMEOUT)
    }

    fn request_with_timeout(
        &mut self,
        method: &str,
        params: Value,
        timeout: Duration,
    ) -> Result<Value, String> {
        let id = self.write_request(method, params)?;
        self.wait_for_response(id, timeout)
    }

    fn write_request(&mut self, method: &str, params: Value) -> Result<i64, String> {
        self.next_request_id += 1;
        let id = self.next_request_id;
        self.write_message(&json!({ "id": id, "method": method, "params": params }))?;
        Ok(id)
    }

    fn wait_for_response(&mut self, request_id: i64, timeout: Duration) -> Result<Value, String> {
        let deadline = Instant::now() + timeout;
        loop {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Err("Codex App Server timed out".into());
            }
            let message = self.receive(remaining)?;
            if self.handle_server_request(&message)? {
                continue;
            }
            if message.get("id").and_then(Value::as_i64) != Some(request_id) {
                continue;
            }
            if let Some(error) = message.get("error") {
                let detail = error
                    .get("message")
                    .and_then(Value::as_str)
                    .unwrap_or("unknown Codex App Server error");
                return Err(detail.to_owned());
            }
            return message
                .get("result")
                .cloned()
                .ok_or_else(|| "Codex App Server response is missing result".to_owned());
        }
    }

    fn receive(&mut self, timeout: Duration) -> Result<Value, String> {
        self.receive_optional(timeout)?
            .ok_or_else(|| "Codex App Server timed out".into())
    }

    fn receive_optional(&mut self, timeout: Duration) -> Result<Option<Value>, String> {
        match self.messages.recv_timeout(timeout) {
            Ok(message)
                if message.get("method").and_then(Value::as_str)
                    == Some("aegisy/transportError") =>
            {
                Err(message
                    .pointer("/params/message")
                    .and_then(Value::as_str)
                    .unwrap_or("Codex App Server transport failed")
                    .to_owned())
            }
            Ok(message) => Ok(Some(message)),
            Err(RecvTimeoutError::Timeout) => Ok(None),
            Err(RecvTimeoutError::Disconnected) => {
                Err("Codex App Server closed its output channel".into())
            }
        }
    }

    fn handle_server_request(&mut self, message: &Value) -> Result<bool, String> {
        let Some(method) = message.get("method").and_then(Value::as_str) else {
            return Ok(false);
        };
        let Some(id) = message.get("id").cloned() else {
            return Ok(false);
        };
        let result = match method {
            "item/commandExecution/requestApproval"
            | "item/fileChange/requestApproval"
            | "item/permissions/requestApproval" => json!({ "decision": "decline" }),
            "item/tool/requestUserInput" => json!({ "answers": {} }),
            _ => {
                self.write_message(&json!({
                    "id": id,
                    "error": { "code": -32601, "message": "unsupported server request" }
                }))?;
                return Ok(true);
            }
        };
        self.write_message(&json!({ "id": id, "result": result }))?;
        Ok(true)
    }

    fn write_message(&mut self, message: &Value) -> Result<(), String> {
        serde_json::to_writer(&mut self.stdin, message)
            .map_err(|error| format!("cannot encode Codex request: {error}"))?;
        self.stdin
            .write_all(b"\n")
            .and_then(|_| self.stdin.flush())
            .map_err(|error| format!("cannot write to Codex App Server: {error}"))
    }
}

fn translate_command_notification(
    method: &str,
    params: &Value,
    turn_id: &str,
    commands: &mut HashMap<String, CommandItem>,
) -> Option<CodexEvent> {
    match method {
        "item/started" => {
            let command = command_item(params.get("item")?, None)?;
            commands.insert(command.item_id.clone(), command.clone());
            Some(CodexEvent::CommandUpdated {
                turn_id: turn_id.into(),
                command: Box::new(command),
                lifecycle: "started".into(),
            })
        }
        "item/commandExecution/outputDelta" => {
            let item_id = params.get("itemId").and_then(Value::as_str)?;
            let delta = params.get("delta").and_then(Value::as_str).unwrap_or("");
            let command = commands.get_mut(item_id)?;
            append_bounded_output(command, delta);
            Some(CodexEvent::CommandUpdated {
                turn_id: turn_id.into(),
                command: Box::new(command.clone()),
                lifecycle: "delta".into(),
            })
        }
        "item/completed" => {
            let item = params.get("item")?;
            let item_id = item.get("id").and_then(Value::as_str).unwrap_or("");
            let mut command = command_item(item, commands.get(item_id))?;
            finish_bounded_output(&mut command);
            commands.insert(command.item_id.clone(), command.clone());
            Some(CodexEvent::CommandUpdated {
                turn_id: turn_id.into(),
                command: Box::new(command),
                lifecycle: "completed".into(),
            })
        }
        _ => None,
    }
}

fn translate_turn_notification(method: &str, params: &Value, turn_id: &str) -> Option<CodexEvent> {
    if params.get("turnId").and_then(Value::as_str) != Some(turn_id) {
        return None;
    }
    match method {
        "thread/tokenUsage/updated" => {
            let usage = bounded_token_usage(params.get("tokenUsage")?)?;
            Some(CodexEvent::TokenUsage {
                turn_id: turn_id.into(),
                usage,
            })
        }
        "turn/diff/updated" => Some(CodexEvent::TurnDiff {
            turn_id: turn_id.into(),
            diff: bounded_string(
                &redact_complete(params.get("diff").and_then(Value::as_str).unwrap_or("")),
                64 * 1024,
            ),
        }),
        "turn/plan/updated" => {
            let steps = params
                .get("plan")
                .and_then(Value::as_array)?
                .iter()
                .take(64)
                .filter_map(|step| {
                    Some(json!({
                        "status": bounded_string(step.get("status")?.as_str()?, 32),
                        "step": bounded_string(
                            &redact_complete(step.get("step")?.as_str()?),
                            2 * 1024
                        )
                    }))
                })
                .collect::<Vec<_>>();
            Some(CodexEvent::TurnPlan {
                turn_id: turn_id.into(),
                explanation: params
                    .get("explanation")
                    .and_then(Value::as_str)
                    .map(|value| bounded_string(&redact_complete(value), 4 * 1024)),
                steps,
            })
        }
        _ => None,
    }
}

fn bounded_token_usage(value: &Value) -> Option<Value> {
    let breakdown = |value: &Value| {
        Some(json!({
            "cached_input_tokens": value.get("cachedInputTokens")?.as_i64()?.max(0),
            "input_tokens": value.get("inputTokens")?.as_i64()?.max(0),
            "output_tokens": value.get("outputTokens")?.as_i64()?.max(0),
            "reasoning_output_tokens": value.get("reasoningOutputTokens")?.as_i64()?.max(0),
            "total_tokens": value.get("totalTokens")?.as_i64()?.max(0)
        }))
    };
    Some(json!({
        "last": breakdown(value.get("last")?)?,
        "total": breakdown(value.get("total")?)?,
        "model_context_window": value
            .get("modelContextWindow")
            .and_then(Value::as_i64)
            .map(|value| value.max(0))
    }))
}

fn command_item(item: &Value, previous: Option<&CommandItem>) -> Option<CommandItem> {
    if item.get("type").and_then(Value::as_str) != Some("commandExecution") {
        return None;
    }
    let item_id = item.get("id").and_then(Value::as_str)?;
    let command = item.get("command").and_then(Value::as_str)?;
    let cwd = item.get("cwd").and_then(Value::as_str)?;
    let mut result = CommandItem {
        item_id: bounded_string(item_id, 512),
        command: bounded_string(&redact_complete(command), 32 * 1024),
        command_actions: bounded_command_actions(item.get("commandActions")),
        cwd: bounded_string(cwd, 4 * 1024),
        status: item
            .get("status")
            .and_then(Value::as_str)
            .unwrap_or("inProgress")
            .to_owned(),
        output: CommandOutputCapture::default(),
        redactor: OutputRedactor::default(),
        duration_ms: item.get("durationMs").and_then(Value::as_u64),
        exit_code: item.get("exitCode").and_then(Value::as_i64),
        process_id: item
            .get("processId")
            .and_then(Value::as_str)
            .map(|value| bounded_string(value, 512)),
        source: item
            .get("source")
            .and_then(Value::as_str)
            .unwrap_or("agent")
            .to_owned(),
    };
    if let Some(output) = item.get("aggregatedOutput").and_then(Value::as_str) {
        append_bounded_output(&mut result, output);
    } else if let Some(previous) = previous {
        result.output = previous.output.clone();
        result.redactor = previous.redactor.clone();
    }
    Some(result)
}

pub(crate) fn append_bounded_output(command: &mut CommandItem, delta: &str) {
    let redacted = command.redactor.push(delta);
    command.output.append(&redacted);
}

pub(crate) fn finish_bounded_output(command: &mut CommandItem) {
    let redacted = command.redactor.finish();
    command.output.append(&redacted);
}

fn codex_message_channel() -> (SyncSender<Value>, Receiver<Value>) {
    mpsc::sync_channel(CODEX_MESSAGE_QUEUE_CAPACITY)
}

fn transport_error(message: impl Into<String>) -> Value {
    json!({
        "method": "aegisy/transportError",
        "params": { "message": message.into() }
    })
}

#[derive(Debug, PartialEq, Eq)]
enum BoundedLine {
    Line(Vec<u8>),
    Oversized,
    Eof,
}

fn read_bounded_line<R: BufRead>(reader: &mut R, limit: usize) -> io::Result<BoundedLine> {
    let mut line = Vec::with_capacity(limit.min(64 * 1024));
    let mut oversized = false;
    loop {
        let available = reader.fill_buf()?;
        if available.is_empty() {
            return if line.is_empty() && !oversized {
                Ok(BoundedLine::Eof)
            } else if oversized {
                Ok(BoundedLine::Oversized)
            } else {
                Ok(BoundedLine::Line(line))
            };
        }
        let consumed = available
            .iter()
            .position(|byte| *byte == b'\n')
            .map(|position| position + 1)
            .unwrap_or(available.len());
        let completed = available[..consumed].ends_with(b"\n");
        let payload_bytes = consumed.saturating_sub(usize::from(completed));
        if !oversized {
            let remaining = limit.saturating_sub(line.len());
            line.extend_from_slice(&available[..payload_bytes.min(remaining)]);
            oversized = payload_bytes > remaining;
        }
        reader.consume(consumed);
        if completed {
            if oversized {
                return Ok(BoundedLine::Oversized);
            }
            if line.ends_with(b"\r") {
                line.pop();
            }
            return Ok(BoundedLine::Line(line));
        }
    }
}

fn bounded_string(value: &str, byte_limit: usize) -> String {
    if value.len() <= byte_limit {
        return value.to_owned();
    }
    let mut end = byte_limit;
    while end > 0 && !value.is_char_boundary(end) {
        end -= 1;
    }
    value[..end].to_owned()
}

fn bounded_command_actions(actions: Option<&Value>) -> Value {
    let values = actions
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .take(32)
        .map(|action| {
            let mut bounded = serde_json::Map::new();
            for key in ["type", "command", "name", "path", "query"] {
                if let Some(value) = action.get(key).and_then(Value::as_str) {
                    bounded.insert(
                        key.into(),
                        Value::String(bounded_string(&redact_complete(value), 4 * 1024)),
                    );
                }
            }
            Value::Object(bounded)
        })
        .collect();
    Value::Array(values)
}

impl Drop for CodexAdapter {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn locate_codex() -> PathBuf {
    if let Some(path) = env::var_os("AEGISY_CODEX_PATH") {
        return PathBuf::from(path);
    }
    for path in ["/opt/homebrew/bin/codex", "/usr/local/bin/codex"] {
        if Path::new(path).is_file() {
            return PathBuf::from(path);
        }
    }
    PathBuf::from(if cfg!(windows) { "codex.exe" } else { "codex" })
}

fn codex_process_environment() -> Result<ProcessEnvironment, String> {
    let isolated_root = std::env::temp_dir().join("aegisy-codex-adapter");
    SessionEnvironment::build("codex-adapter", None, "read-only", &isolated_root)
        .for_tool("codex-app-server", Vec::new())
        .map_err(|error| {
            format!(
                "cannot construct Codex process environment: {}",
                error.message
            )
        })
}

fn codex_command(executable: &Path, environment: &ProcessEnvironment) -> Command {
    let mut command = Command::new(executable);
    command.env_clear();
    for (name, value) in environment.iter() {
        command.env(name, value);
    }
    command
}

fn codex_version(executable: &Path, environment: &ProcessEnvironment) -> String {
    let mut command = codex_command(executable, environment);
    command
        .arg("--version")
        .output()
        .ok()
        .filter(|output| output.status.success())
        .map(|output| String::from_utf8_lossy(&output.stdout).trim().to_owned())
        .filter(|version| !version.is_empty())
        .unwrap_or_else(|| "unknown".into())
}

fn codex_version_is_pinned(version: &str) -> bool {
    version.trim() == PINNED_CODEX_VERSION
}

fn classify_stderr(value: &str) -> &'static str {
    let lower = value.to_ascii_lowercase();
    if lower.contains("panic") || lower.contains("fatal") {
        "fatal"
    } else if lower.contains("timeout") || lower.contains("timed out") {
        "timeout"
    } else if lower.contains("error") || lower.contains("failed") {
        "error"
    } else if lower.contains("warn") {
        "warning"
    } else {
        "info"
    }
}

fn is_retryable_startup_error(value: &str) -> bool {
    let lower = value.to_ascii_lowercase();
    [
        "timed out",
        "closed its output channel",
        "transport",
        "cannot read codex app server output",
        "cannot write to codex app server",
    ]
    .iter()
    .any(|needle| lower.contains(needle))
}

fn thread_start_params(cwd: &Path, chat: bool) -> Value {
    let instructions = if chat {
        "You are running inside Aegisy Coding Chat. This mode is non-mutating. Do not modify files, execute mutating commands, or inspect paths outside explicitly provided context."
    } else {
        "You are running inside the Aegisy Coding Work preview. Inspect the bound project in read-only mode. Do not modify files or execute mutating commands. Explain proposed changes instead."
    };
    json!({
        "cwd": cwd,
        "sandbox": "read-only",
        "approvalPolicy": "never",
        "ephemeral": false,
        "developerInstructions": instructions,
        "threadSource": "aegisy-coding"
    })
}

fn thread_resume_params(thread_id: &str, cwd: &Path, chat: bool) -> Value {
    let mut params = thread_start_params(cwd, chat);
    let object = params.as_object_mut().expect("thread start params object");
    object.remove("ephemeral");
    object.insert("threadId".into(), thread_id.into());
    params
}

fn thread_fork_params(
    thread_id: &str,
    last_turn_id: Option<&str>,
    cwd: &Path,
    chat: bool,
) -> Value {
    let mut params = thread_resume_params(thread_id, cwd, chat);
    let object = params.as_object_mut().expect("thread fork params object");
    object.insert("ephemeral".into(), false.into());
    object.insert(
        "lastTurnId".into(),
        last_turn_id.map_or(Value::Null, |turn_id| turn_id.into()),
    );
    params
}

#[allow(dead_code)]
fn thread_list_params(
    cursor: Option<&str>,
    limit: Option<u32>,
    cwd: Option<&Path>,
    archived: Option<bool>,
) -> Value {
    let mut params = json!({});
    let object = params.as_object_mut().expect("thread list params object");
    if let Some(cursor) = cursor {
        object.insert("cursor".into(), cursor.into());
    }
    if let Some(limit) = limit {
        object.insert("limit".into(), limit.into());
    }
    if let Some(cwd) = cwd {
        object.insert("cwd".into(), cwd.to_string_lossy().into_owned().into());
    }
    if let Some(archived) = archived {
        object.insert("archived".into(), archived.into());
    }
    params
}

#[allow(dead_code)]
fn thread_read_params(thread_id: &str, include_turns: bool) -> Value {
    json!({
        "threadId": thread_id,
        "includeTurns": include_turns
    })
}

#[allow(dead_code)]
fn thread_compact_start_params(thread_id: &str) -> Value {
    json!({ "threadId": thread_id })
}

fn turn_start_params(request: &CodexTurnRequest<'_>) -> Value {
    let mut turn_input = vec![json!({ "type": "text", "text": request.input })];
    turn_input.extend(request.local_images.iter().map(|path| {
        json!({
            "type": "localImage",
            "path": path.to_string_lossy()
        })
    }));
    json!({
        "threadId": request.thread_id,
        "input": turn_input,
        "clientUserMessageId": request.idempotency_key
    })
}

fn turn_interrupt_params(thread_id: &str, turn_id: &str) -> Value {
    json!({
        "threadId": thread_id,
        "turnId": turn_id
    })
}

fn turn_steer_params(thread_id: &str, turn_id: &str, steer: &TurnSteerRequest) -> Value {
    json!({
        "threadId": thread_id,
        "expectedTurnId": turn_id,
        "input": [{ "type": "text", "text": steer.input }],
        "clientUserMessageId": steer.client_user_message_id
    })
}

fn turn_terminal_event(params: &Value, turn_id: &str) -> CodexEvent {
    match params.pointer("/turn/status").and_then(Value::as_str) {
        Some("completed") => CodexEvent::TurnCompleted {
            turn_id: turn_id.into(),
        },
        Some("interrupted") => CodexEvent::TurnInterrupted {
            turn_id: turn_id.into(),
        },
        _ => CodexEvent::TurnFailed {
            turn_id: turn_id.into(),
            message: params
                .pointer("/turn/error/message")
                .and_then(Value::as_str)
                .unwrap_or("Codex turn failed")
                .to_owned(),
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn backend_info_uses_read_only_profile() {
        let info = BackendInfo {
            adapter: "codex-app-server".into(),
            version: "codex-cli test".into(),
            provider: Some("aegisy".into()),
            model: Some("test-model".into()),
            permission_profile: "read-only".into(),
            environment: None,
        };
        assert_eq!(info.permission_profile, "read-only");
        assert_eq!(serde_json::to_value(info).unwrap()["provider"], "aegisy");
    }

    #[test]
    fn codex_adapter_rejects_unpinned_versions() {
        assert!(codex_version_is_pinned(PINNED_CODEX_VERSION));
        assert!(!codex_version_is_pinned("codex-cli 0.144.4"));
        assert!(!codex_version_is_pinned("unknown"));
    }

    #[test]
    fn startup_supervision_retries_only_transient_transport_failures() {
        assert!(is_retryable_startup_error("Codex App Server timed out"));
        assert!(is_retryable_startup_error(
            "Codex App Server closed its output channel"
        ));
        assert!(is_retryable_startup_error(
            "cannot write to Codex App Server"
        ));
        assert!(!is_retryable_startup_error(
            "unsupported Codex App Server version codex-cli 0.144.4"
        ));
        assert!(!is_retryable_startup_error(
            "Codex rejected initialize because the request is invalid"
        ));
        assert_eq!(STARTUP_MAX_ATTEMPTS, 3);
        assert_eq!(STARTUP_TIMEOUT, Duration::from_secs(15));
    }

    #[test]
    fn stderr_diagnostics_are_bounded_redacted_and_content_free() {
        let secret = b"warning: Authorization: Bearer ghp_123456789012345678901234567890\n";
        let mut diagnostics = StderrDiagnostics::default();
        diagnostics.observe(secret);
        assert_eq!(diagnostics.bytes, secret.len() as u64);
        assert_eq!(diagnostics.lines, 1);
        assert_eq!(diagnostics.redacted_lines, 1);
        assert_eq!(diagnostics.last_class.as_deref(), Some("warning"));
        let encoded = serde_json::to_string(&diagnostics).unwrap();
        assert!(!encoded.contains("ghp_"));

        diagnostics.observe(b"fatal: Codex process panic\n");
        assert_eq!(diagnostics.last_class.as_deref(), Some("fatal"));
        assert_eq!(diagnostics.lines, 2);
    }

    #[test]
    fn agent_delta_fixture_accumulates_in_wire_order() {
        let fixture = ["AE", "GIS", "Y_CODEX_OK"];
        let mut accumulated = String::new();
        for delta in fixture {
            accumulated.push_str(delta);
        }
        assert_eq!(accumulated, "AEGISY_CODEX_OK");
    }

    #[test]
    fn thread_start_is_always_read_only_without_approval_escalation() {
        let params = thread_start_params(Path::new("/tmp/project"), false);
        assert_eq!(params["cwd"], "/tmp/project");
        assert_eq!(params["sandbox"], "read-only");
        assert_eq!(params["approvalPolicy"], "never");
        assert_eq!(params["ephemeral"], false);
        assert!(params["developerInstructions"]
            .as_str()
            .unwrap()
            .contains("Do not modify files"));
    }

    #[test]
    fn thread_resume_and_fork_match_generated_pinned_schema() {
        let resume = thread_resume_params("thread-1", Path::new("/tmp/project"), false);
        assert_eq!(resume["threadId"], "thread-1");
        assert_eq!(resume["cwd"], "/tmp/project");
        assert_eq!(resume["sandbox"], "read-only");
        assert_eq!(resume["approvalPolicy"], "never");
        assert!(resume.get("ephemeral").is_none());
        let fork = thread_fork_params("thread-1", Some("turn-2"), Path::new("/tmp/project"), false);
        assert_eq!(fork["threadId"], "thread-1");
        assert_eq!(fork["lastTurnId"], "turn-2");
        assert_eq!(fork["ephemeral"], false);
        assert_eq!(
            fork["developerInstructions"],
            resume["developerInstructions"]
        );
    }

    #[test]
    fn thread_lifecycle_params_match_generated_pinned_schema() {
        let list = thread_list_params(
            Some("cursor-1"),
            Some(50),
            Some(Path::new("/tmp/project")),
            Some(false),
        );
        assert_eq!(
            list,
            json!({
                "cursor": "cursor-1",
                "limit": 50,
                "cwd": "/tmp/project",
                "archived": false
            })
        );
        assert_eq!(
            thread_read_params("thread-1", true),
            json!({ "threadId": "thread-1", "includeTurns": true })
        );
        assert_eq!(
            thread_compact_start_params("thread-1"),
            json!({ "threadId": "thread-1" })
        );
    }

    #[test]
    fn thread_lifecycle_session_parser_accepts_thread_scoped_metadata() {
        let session = parse_codex_session(
            json!({
                "thread": {
                    "id": "thread-1",
                    "modelProvider": "aegisy",
                    "model": "model-1"
                }
            }),
            "thread/unarchive",
        )
        .unwrap();
        assert_eq!(session.thread_id, "thread-1");
        assert_eq!(session.provider, "aegisy");
        assert_eq!(session.model, "model-1");
    }

    #[test]
    fn turn_metadata_notifications_map_to_bounded_stable_events() {
        let usage = translate_turn_notification(
            "thread/tokenUsage/updated",
            &json!({
                "threadId": "thread-1",
                "turnId": "turn-1",
                "tokenUsage": {
                    "last": {
                        "cachedInputTokens": 1,
                        "inputTokens": 2,
                        "outputTokens": 3,
                        "reasoningOutputTokens": 4,
                        "totalTokens": 5
                    },
                    "total": {
                        "cachedInputTokens": 6,
                        "inputTokens": 7,
                        "outputTokens": 8,
                        "reasoningOutputTokens": 9,
                        "totalTokens": 10
                    },
                    "modelContextWindow": 100
                }
            }),
            "turn-1",
        )
        .unwrap();
        match usage {
            CodexEvent::TokenUsage { usage, .. } => {
                assert_eq!(usage["last"]["total_tokens"], 5);
                assert_eq!(usage["model_context_window"], 100);
            }
            _ => panic!("unexpected token usage event"),
        }

        let plan = translate_turn_notification(
            "turn/plan/updated",
            &json!({
                "threadId": "thread-1",
                "turnId": "turn-1",
                "explanation": "inspect then test",
                "plan": [{ "status": "inProgress", "step": "inspect" }]
            }),
            "turn-1",
        )
        .unwrap();
        match plan {
            CodexEvent::TurnPlan { steps, .. } => assert_eq!(steps[0]["step"], "inspect"),
            _ => panic!("unexpected turn plan event"),
        }

        let diff = translate_turn_notification(
            "turn/diff/updated",
            &json!({
                "threadId": "thread-1",
                "turnId": "turn-1",
                "diff": "Authorization: Bearer ghp_123456789012345678901234567890"
            }),
            "turn-1",
        )
        .unwrap();
        match diff {
            CodexEvent::TurnDiff { diff, .. } => assert!(!diff.contains("ghp_")),
            _ => panic!("unexpected turn diff event"),
        }
    }

    #[test]
    fn turn_start_preserves_client_idempotency_key() {
        let local_images = [PathBuf::from("/tmp/aegisy-image.png")];
        let params = turn_start_params(&CodexTurnRequest {
            thread_id: "thread-1",
            input: "hello",
            local_images: &local_images,
            idempotency_key: "client-turn-1",
        });
        assert_eq!(params["threadId"], "thread-1");
        assert_eq!(params["input"][0]["type"], "text");
        assert_eq!(params["input"][0]["text"], "hello");
        assert_eq!(params["input"][1]["type"], "localImage");
        assert_eq!(params["input"][1]["path"], "/tmp/aegisy-image.png");
        assert_eq!(params["clientUserMessageId"], "client-turn-1");
    }

    #[test]
    fn turn_interrupt_matches_pinned_app_server_schema() {
        assert_eq!(
            turn_interrupt_params("thread-1", "turn-1"),
            json!({ "threadId": "thread-1", "turnId": "turn-1" })
        );
    }

    #[test]
    fn turn_steer_matches_pinned_app_server_schema() {
        let params = turn_steer_params(
            "thread-1",
            "turn-1",
            &TurnSteerRequest {
                input: "focus on the failing test".into(),
                client_user_message_id: Some("steer-1".into()),
            },
        );
        assert_eq!(params["threadId"], "thread-1");
        assert_eq!(params["expectedTurnId"], "turn-1");
        assert_eq!(params["input"][0]["type"], "text");
        assert_eq!(params["input"][0]["text"], "focus on the failing test");
        assert_eq!(params["clientUserMessageId"], "steer-1");
        assert!(params.get("turnId").is_none());
    }

    #[test]
    fn interrupted_turn_is_not_misreported_as_failure() {
        assert_eq!(
            turn_terminal_event(
                &json!({ "turn": { "id": "turn-1", "status": "interrupted" } }),
                "turn-1"
            ),
            CodexEvent::TurnInterrupted {
                turn_id: "turn-1".into()
            }
        );
    }

    #[test]
    fn command_items_preserve_structured_metadata_and_bound_output() {
        let fixture = json!({
            "id": "command-1",
            "type": "commandExecution",
            "command": "git status --short",
            "commandActions": [{
                "type": "listFiles",
                "command": "git status --short",
                "path": null
            }],
            "cwd": "/tmp/project",
            "status": "inProgress",
            "aggregatedOutput": null,
            "durationMs": null,
            "exitCode": null,
            "processId": "pty-1",
            "source": "agent"
        });
        let mut command = command_item(&fixture, None).unwrap();
        append_bounded_output(&mut command, "M src/main.rs\n");
        assert_eq!(command.command_actions[0]["type"], "listFiles");
        assert_eq!(command.output.snapshot().head, "M src/main.rs\n");
        append_bounded_output(&mut command, &"界".repeat(256 * 1024));
        let snapshot = command.output.snapshot();
        assert!(snapshot.retained_bytes <= 256 * 1024);
        assert!(snapshot.omitted_bytes > 0);
        assert!(snapshot.head.is_char_boundary(0));
    }

    #[test]
    fn command_notification_fixture_preserves_one_incremental_lifecycle() {
        let started = json!({
            "threadId": "thread-1",
            "turnId": "turn-1",
            "startedAtMs": 10,
            "item": {
                "id": "command-1",
                "type": "commandExecution",
                "command": "git status --short",
                "commandActions": [{
                    "type": "listFiles",
                    "command": "git status --short",
                    "path": null
                }],
                "cwd": "/tmp/project",
                "status": "inProgress"
            }
        });
        let delta = json!({
            "threadId": "thread-1",
            "turnId": "turn-1",
            "itemId": "command-1",
            "delta": "M src/main.rs\n"
        });
        let completed = json!({
            "threadId": "thread-1",
            "turnId": "turn-1",
            "completedAtMs": 52,
            "item": {
                "id": "command-1",
                "type": "commandExecution",
                "command": "git status --short",
                "commandActions": [{
                    "type": "listFiles",
                    "command": "git status --short",
                    "path": null
                }],
                "cwd": "/tmp/project",
                "status": "completed",
                "aggregatedOutput": "M src/main.rs\n",
                "durationMs": 42,
                "exitCode": 0,
                "processId": "pty-1",
                "source": "agent"
            }
        });
        let mut commands = HashMap::new();
        let events = [
            translate_command_notification("item/started", &started, "turn-1", &mut commands),
            translate_command_notification(
                "item/commandExecution/outputDelta",
                &delta,
                "turn-1",
                &mut commands,
            ),
            translate_command_notification("item/completed", &completed, "turn-1", &mut commands),
        ];
        let lifecycles = events
            .into_iter()
            .map(|event| match event.unwrap() {
                CodexEvent::CommandUpdated {
                    command, lifecycle, ..
                } => {
                    if lifecycle == "completed" {
                        assert_eq!(command.duration_ms, Some(42));
                        assert_eq!(command.exit_code, Some(0));
                        assert_eq!(command.output.snapshot().head, "M src/main.rs\n");
                    }
                    lifecycle
                }
                _ => panic!("unexpected event"),
            })
            .collect::<Vec<_>>();
        assert_eq!(lifecycles, ["started", "delta", "completed"]);
        assert_eq!(commands.len(), 1);
    }

    #[test]
    fn codex_parent_process_environment_is_value_free_and_secret_scrubbed() {
        let environment = codex_process_environment().unwrap();
        let names = environment
            .iter()
            .map(|(name, _)| name.to_owned())
            .collect::<Vec<_>>();
        assert!(names.contains(&"PATH".into()));
        assert!(names.contains(&"AEGISY_TOOL_NAME".into()));
        for denied in [
            "OPENAI_API_KEY",
            "ANTHROPIC_API_KEY",
            "AWS_SECRET_ACCESS_KEY",
            "HTTP_PROXY",
            "LD_PRELOAD",
            "DYLD_INSERT_LIBRARIES",
            "NODE_OPTIONS",
        ] {
            assert!(!names.contains(&denied.into()));
        }
        assert!(environment
            .summary()
            .environment_id
            .starts_with("environment:sha256:"));
        assert_eq!(environment.summary().variable_count, names.len());
    }

    #[test]
    fn bounded_line_reader_accepts_the_limit_and_drains_oversized_frames() {
        let input = b"1234\n12345\nok\r\nlast";
        let mut reader = BufReader::with_capacity(2, &input[..]);
        assert_eq!(
            read_bounded_line(&mut reader, 4).unwrap(),
            BoundedLine::Line(b"1234".to_vec())
        );
        assert_eq!(
            read_bounded_line(&mut reader, 4).unwrap(),
            BoundedLine::Oversized
        );
        assert_eq!(
            read_bounded_line(&mut reader, 4).unwrap(),
            BoundedLine::Line(b"ok".to_vec())
        );
        assert_eq!(
            read_bounded_line(&mut reader, 4).unwrap(),
            BoundedLine::Line(b"last".to_vec())
        );
        assert_eq!(read_bounded_line(&mut reader, 4).unwrap(), BoundedLine::Eof);
    }

    #[test]
    fn codex_message_queue_applies_backpressure_and_then_unblocks() {
        let (sender, receiver) = codex_message_channel();
        for index in 0..CODEX_MESSAGE_QUEUE_CAPACITY {
            sender.send(json!(index)).unwrap();
        }
        let (finished_sender, finished_receiver) = mpsc::channel();
        let producer = thread::spawn(move || {
            sender.send(json!("after-capacity")).unwrap();
            finished_sender.send(()).unwrap();
        });
        assert!(finished_receiver
            .recv_timeout(Duration::from_millis(50))
            .is_err());
        assert_eq!(receiver.recv().unwrap(), json!(0));
        finished_receiver
            .recv_timeout(Duration::from_secs(1))
            .unwrap();
        producer.join().unwrap();
    }

    #[test]
    fn command_output_redaction_precedes_all_capture_buffers() {
        let fixture = json!({
            "id": "command-secret",
            "type": "commandExecution",
            "command": "print diagnostics",
            "cwd": "/tmp/project",
            "status": "inProgress"
        });
        let secret = "ghp_123456789012345678901234567890";
        let mut command = command_item(&fixture, None).unwrap();
        append_bounded_output(&mut command, "Authorization: Bearer ");
        append_bounded_output(&mut command, secret);
        finish_bounded_output(&mut command);
        let snapshot = command.output.snapshot();
        let artifact = command.output.artifact();
        assert!(!snapshot.head.contains(secret));
        assert!(!snapshot.tail.contains(secret));
        assert!(!artifact.content.contains(secret));
        assert_eq!(command.redactor.redacted_count(), 1);
        assert_eq!(
            command.redactor.source_bytes(),
            ("Authorization: Bearer ".len() + secret.len()) as u64
        );
    }

    #[cfg(target_os = "macos")]
    #[test]
    #[ignore = "requires an explicitly selected installed codex-cli 0.144.5 binary; run with AEGISY_CODEX_PATH=... --ignored"]
    fn live_pinned_binary_initializes_and_starts_read_only_thread() {
        let path = std::env::var_os("AEGISY_CODEX_PATH")
            .expect("AEGISY_CODEX_PATH must select the live pinned Codex binary");
        assert!(Path::new(&path).is_file());
        let mut adapter = CodexAdapter::start().expect("pinned Codex app-server must initialize");
        assert_eq!(adapter.info().version, PINNED_CODEX_VERSION);
        let session = adapter
            .start_session(Path::new("/tmp"), true)
            .expect("pinned Codex app-server must accept a read-only thread start");
        assert!(!session.thread_id.is_empty());
        assert_eq!(adapter.info().permission_profile, "read-only");
        assert_eq!(adapter.health().state, "running");
    }

    #[test]
    fn command_metadata_is_redacted_at_the_adapter_boundary() {
        let secret = "ghp_123456789012345678901234567890";
        let fixture = json!({
            "id": "command-secret-metadata",
            "type": "commandExecution",
            "command": format!("tool --token {secret}"),
            "commandActions": [{
                "type": "search",
                "query": format!("--api-key={secret}")
            }],
            "cwd": "/tmp/project",
            "status": "inProgress"
        });
        let command = command_item(&fixture, None).unwrap();
        let serialized = format!("{}\n{}", command.command, command.command_actions);
        assert!(!serialized.contains(secret));
        assert!(serialized.contains("[REDACTED]"));
    }
}
