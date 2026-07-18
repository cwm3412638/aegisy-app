use crate::session_environment::SessionEnvironment;
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
