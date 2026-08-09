use crate::session_environment::{EnvironmentSummary, SessionEnvironment};
use serde::Serialize;
use std::path::Path;

#[derive(Debug)]
pub struct TerminalError {
    pub code: i64,
    pub message: String,
}

#[derive(Debug, Serialize)]
pub struct TerminalSnapshot {
    pub terminal_id: String,
    pub session_id: String,
    pub project_id: String,
    pub shell: String,
    pub shell_profile: String,
    pub encoding: String,
    pub process_tree_policy: String,
    pub environment: EnvironmentSummary,
    pub process_id: Option<u32>,
    pub running: bool,
    pub exit_code: Option<u32>,
    pub exit_signal: Option<String>,
    pub rows: u16,
    pub cols: u16,
    pub output_base64: String,
    pub output_start: u64,
    pub output_end: u64,
    pub omitted_before_start: u64,
    pub capture_limit: usize,
    pub reader_complete: bool,
    pub reader_error: Option<String>,
}

pub struct TerminalOpenContext<'a> {
    pub session_id: &'a str,
    pub project_id: &'a str,
    pub root: &'a Path,
    pub environment: &'a SessionEnvironment,
}

#[derive(Default)]
pub struct TerminalManager;

impl TerminalManager {
    pub fn capability() -> &'static str {
        "terminal.pty.unsupported"
    }

    pub fn open_user(
        &mut self,
        _terminal_id: String,
        context: TerminalOpenContext<'_>,
        _rows: u16,
        _cols: u16,
    ) -> Result<TerminalSnapshot, TerminalError> {
        let _ = (
            context.session_id,
            context.project_id,
            context.root,
            context.environment,
        );
        Err(unsupported())
    }

    pub fn snapshot(
        &mut self,
        _terminal_id: &str,
        _session_id: &str,
        _after: u64,
    ) -> Result<TerminalSnapshot, TerminalError> {
        Err(unsupported())
    }

    pub fn input_user(
        &mut self,
        _terminal_id: &str,
        _session_id: &str,
        _input_base64: &str,
    ) -> Result<usize, TerminalError> {
        Err(unsupported())
    }

    pub fn resize(
        &mut self,
        _terminal_id: &str,
        _session_id: &str,
        _rows: u16,
        _cols: u16,
    ) -> Result<(), TerminalError> {
        Err(unsupported())
    }

    pub fn signal_user(
        &mut self,
        _terminal_id: &str,
        _session_id: &str,
        _signal: &str,
    ) -> Result<(), TerminalError> {
        Err(unsupported())
    }

    pub fn close_user(
        &mut self,
        _terminal_id: &str,
        _session_id: &str,
    ) -> Result<TerminalSnapshot, TerminalError> {
        Err(unsupported())
    }

    pub fn remove_user(
        &mut self,
        _terminal_id: &str,
        _session_id: &str,
    ) -> Result<(), TerminalError> {
        Err(unsupported())
    }

    pub fn shutdown_all(&mut self) {}
}

fn unsupported() -> TerminalError {
    TerminalError {
        code: -32090,
        message: "terminal PTY backend is unsupported on this platform".into(),
    }
}
