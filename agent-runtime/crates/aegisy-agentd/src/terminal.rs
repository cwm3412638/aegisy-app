use crate::session_environment::{
    EnvironmentSummary, ProcessEnvironment, SessionEnvironment, ToolVariable,
};
use base64::engine::general_purpose::STANDARD as BASE64_STANDARD;
use base64::Engine;
use portable_pty::{native_pty_system, Child, CommandBuilder, MasterPty, PtySize};
use serde::Serialize;
use std::collections::{HashMap, VecDeque};
use std::fs;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

const CAPTURE_LIMIT: usize = 1024 * 1024;
const MAX_TERMINALS: usize = 64;
const MAX_TERMINALS_PER_SESSION: usize = 16;
const MAX_INPUT_BYTES: usize = 64 * 1024;
const MAX_DIMENSION: u16 = 1_000;
const STOP_SIGNAL_GRACE: Duration = Duration::from_millis(150);

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
struct Capture {
    bytes: VecDeque<u8>,
    output_end: u64,
    reader_complete: bool,
    reader_error: Option<String>,
}

impl Capture {
    fn push(&mut self, input: &[u8]) {
        self.output_end = self.output_end.saturating_add(input.len() as u64);
        if input.len() >= CAPTURE_LIMIT {
            self.bytes.clear();
            self.bytes.extend(
                input[input.len().saturating_sub(CAPTURE_LIMIT)..]
                    .iter()
                    .copied(),
            );
            return;
        }
        let overflow = self
            .bytes
            .len()
            .saturating_add(input.len())
            .saturating_sub(CAPTURE_LIMIT);
        self.bytes.drain(..overflow);
        self.bytes.extend(input.iter().copied());
    }

    fn read_after(&self, after: u64) -> (Vec<u8>, u64, u64) {
        let retained_start = self.output_end.saturating_sub(self.bytes.len() as u64);
        let output_start = after.max(retained_start).min(self.output_end);
        let skip = output_start.saturating_sub(retained_start) as usize;
        let output = self.bytes.iter().skip(skip).copied().collect();
        (output, output_start, retained_start)
    }
}

struct Terminal {
    session_id: String,
    project_id: String,
    shell: String,
    environment: EnvironmentSummary,
    process_id: Option<u32>,
    session_leader: Option<libc::pid_t>,
    rows: u16,
    cols: u16,
    master: Box<dyn MasterPty + Send>,
    writer: Box<dyn Write + Send>,
    child: Box<dyn Child + Send + Sync>,
    capture: Arc<Mutex<Capture>>,
    exit_code: Option<u32>,
    exit_signal: Option<String>,
}

#[derive(Default)]
pub struct TerminalManager {
    terminals: HashMap<String, Terminal>,
}

impl TerminalManager {
    pub fn supported() -> bool {
        cfg!(target_os = "macos")
    }

    pub fn capability() -> &'static str {
        "terminal.pty.macos.user-initiated"
    }

    pub fn open_user(
        &mut self,
        terminal_id: String,
        context: TerminalOpenContext<'_>,
        rows: u16,
        cols: u16,
    ) -> Result<TerminalSnapshot, TerminalError> {
        if !Self::supported() {
            return Err(error(
                -32090,
                "terminal PTY backend is unsupported on this platform",
            ));
        }
        validate_size(rows, cols)?;
        validate_project_root(context.root)?;
        if self.terminals.len() >= MAX_TERMINALS {
            return Err(error(-32091, "terminal limit exceeded"));
        }
        if self
            .terminals
            .values()
            .filter(|terminal| terminal.session_id == context.session_id)
            .count()
            >= MAX_TERMINALS_PER_SESSION
        {
            return Err(error(-32091, "session terminal limit exceeded"));
        }

        let shell = discover_shell().ok_or_else(|| error(-32092, "no executable shell found"))?;
        let pty = native_pty_system()
            .openpty(PtySize {
                rows,
                cols,
                pixel_width: 0,
                pixel_height: 0,
            })
            .map_err(|cause| error(-32092, format!("cannot open pseudoterminal: {cause}")))?;
        let mut command = CommandBuilder::new(&shell);
        configure_shell_arguments(&mut command, &shell);
        command.cwd(context.root);
        let process_environment = context
            .environment
            .for_tool(
                "terminal",
                terminal_tool_variables(&shell, context.environment),
            )
            .map_err(|cause| error(-32096, cause.message))?;
        configure_environment(&mut command, &process_environment);
        let child = pty
            .slave
            .spawn_command(command)
            .map_err(|cause| error(-32092, format!("cannot start terminal shell: {cause}")))?;
        drop(pty.slave);
        let process_id = child.process_id();
        let session_leader = process_id.and_then(|pid| libc::pid_t::try_from(pid).ok());
        let mut reader = pty
            .master
            .try_clone_reader()
            .map_err(|cause| error(-32092, format!("cannot open terminal reader: {cause}")))?;
        let writer = pty
            .master
            .take_writer()
            .map_err(|cause| error(-32092, format!("cannot open terminal writer: {cause}")))?;
        let capture = Arc::new(Mutex::new(Capture::default()));
        let reader_capture = Arc::clone(&capture);
        thread::Builder::new()
            .name(format!("aegisy-pty-reader-{terminal_id}"))
            .spawn(move || read_output(&mut reader, &reader_capture))
            .map_err(|cause| error(-32092, format!("cannot start terminal reader: {cause}")))?;

        self.terminals.insert(
            terminal_id.clone(),
            Terminal {
                session_id: context.session_id.to_owned(),
                project_id: context.project_id.to_owned(),
                shell: shell
                    .file_name()
                    .and_then(|name| name.to_str())
                    .unwrap_or("shell")
                    .to_owned(),
                environment: process_environment.summary().clone(),
                process_id,
                session_leader,
                rows,
                cols,
                master: pty.master,
                writer,
                child,
                capture,
                exit_code: None,
                exit_signal: None,
            },
        );
        self.snapshot(&terminal_id, context.session_id, 0)
    }

    pub fn snapshot(
        &mut self,
        terminal_id: &str,
        session_id: &str,
        after: u64,
    ) -> Result<TerminalSnapshot, TerminalError> {
        let terminal = self.bound_terminal_mut(terminal_id, session_id)?;
        poll_exit(terminal)?;
        Ok(snapshot_of(terminal_id, terminal, after))
    }

    pub fn input_user(
        &mut self,
        terminal_id: &str,
        session_id: &str,
        input_base64: &str,
    ) -> Result<usize, TerminalError> {
        if input_base64.len() > MAX_INPUT_BYTES.saturating_mul(2) {
            return Err(error(-32602, "terminal input limit exceeded"));
        }
        let input = BASE64_STANDARD
            .decode(input_base64)
            .map_err(|_| error(-32602, "terminal input is not valid base64"))?;
        if input.is_empty() || input.len() > MAX_INPUT_BYTES {
            return Err(error(
                -32602,
                "terminal input must contain 1 to 65536 bytes",
            ));
        }
        let terminal = self.bound_terminal_mut(terminal_id, session_id)?;
        poll_exit(terminal)?;
        if terminal.exit_code.is_some() || terminal.exit_signal.is_some() {
            return Err(error(-32094, "terminal process has exited"));
        }
        terminal
            .writer
            .write_all(&input)
            .and_then(|_| terminal.writer.flush())
            .map_err(|cause| error(-32094, format!("cannot write terminal input: {cause}")))?;
        Ok(input.len())
    }

    pub fn resize(
        &mut self,
        terminal_id: &str,
        session_id: &str,
        rows: u16,
        cols: u16,
    ) -> Result<(), TerminalError> {
        validate_size(rows, cols)?;
        let terminal = self.bound_terminal_mut(terminal_id, session_id)?;
        terminal
            .master
            .resize(PtySize {
                rows,
                cols,
                pixel_width: 0,
                pixel_height: 0,
            })
            .map_err(|cause| error(-32094, format!("cannot resize terminal: {cause}")))?;
        terminal.rows = rows;
        terminal.cols = cols;
        Ok(())
    }

    pub fn signal_user(
        &mut self,
        terminal_id: &str,
        session_id: &str,
        signal: &str,
    ) -> Result<(), TerminalError> {
        let signal = match signal {
            "interrupt" => libc::SIGINT,
            "terminate" => libc::SIGTERM,
            "hangup" => libc::SIGHUP,
            "kill" => libc::SIGKILL,
            _ => return Err(error(-32602, "unsupported terminal signal")),
        };
        let terminal = self.bound_terminal_mut(terminal_id, session_id)?;
        poll_exit(terminal)?;
        if terminal.exit_code.is_some() || terminal.exit_signal.is_some() {
            return Ok(());
        }
        signal_foreground_group(terminal, signal)
    }

    pub fn close_user(
        &mut self,
        terminal_id: &str,
        session_id: &str,
    ) -> Result<TerminalSnapshot, TerminalError> {
        let process_groups = {
            let terminal = self.bound_terminal_mut(terminal_id, session_id)?;
            poll_exit(terminal)?;
            if terminal.exit_code.is_some() || terminal.exit_signal.is_some() {
                Vec::new()
            } else {
                terminal_process_groups(terminal)
            }
        };
        if !process_groups.is_empty() {
            signal_process_groups(&process_groups, libc::SIGHUP);
            if !wait_for_process_groups(&process_groups, STOP_SIGNAL_GRACE) {
                signal_process_groups(&process_groups, libc::SIGTERM);
                if !wait_for_process_groups(&process_groups, STOP_SIGNAL_GRACE) {
                    signal_process_groups(&process_groups, libc::SIGKILL);
                    let terminal = self.bound_terminal_mut(terminal_id, session_id)?;
                    let _ = terminal.child.kill();
                    let _ = wait_for_process_groups(&process_groups, STOP_SIGNAL_GRACE);
                }
            }
        }
        self.snapshot(terminal_id, session_id, 0)
    }

    pub fn remove_user(
        &mut self,
        terminal_id: &str,
        session_id: &str,
    ) -> Result<(), TerminalError> {
        let belongs_to_session = self
            .terminals
            .get(terminal_id)
            .map(|terminal| terminal.session_id == session_id)
            .ok_or_else(|| error(-32093, "terminal not found"))?;
        if !belongs_to_session {
            return Err(error(-32093, "terminal does not belong to session"));
        }
        let mut terminal = self
            .terminals
            .remove(terminal_id)
            .ok_or_else(|| error(-32093, "terminal not found"))?;
        terminate_terminal(&mut terminal);
        Ok(())
    }

    pub fn shutdown_all(&mut self) {
        for terminal in self.terminals.values_mut() {
            terminate_terminal(terminal);
        }
        self.terminals.clear();
    }

    fn bound_terminal_mut(
        &mut self,
        terminal_id: &str,
        session_id: &str,
    ) -> Result<&mut Terminal, TerminalError> {
        let terminal = self
            .terminals
            .get_mut(terminal_id)
            .ok_or_else(|| error(-32093, "terminal not found"))?;
        if terminal.session_id != session_id {
            return Err(error(-32093, "terminal does not belong to session"));
        }
        Ok(terminal)
    }
}

impl Drop for TerminalManager {
    fn drop(&mut self) {
        for terminal in self.terminals.values_mut() {
            terminate_terminal(terminal);
        }
    }
}

fn validate_size(rows: u16, cols: u16) -> Result<(), TerminalError> {
    if rows == 0 || cols == 0 || rows > MAX_DIMENSION || cols > MAX_DIMENSION {
        return Err(error(
            -32602,
            "terminal rows and cols must be between 1 and 1000",
        ));
    }
    Ok(())
}

fn validate_project_root(root: &Path) -> Result<(), TerminalError> {
    if !root.is_absolute() || !root.is_dir() {
        return Err(error(-32092, "terminal project root is unavailable"));
    }
    let canonical = root.canonicalize().map_err(|cause| {
        error(
            -32092,
            format!("cannot validate terminal project root: {cause}"),
        )
    })?;
    if canonical != root {
        return Err(error(
            -32092,
            "terminal project root changed after project authorization",
        ));
    }
    Ok(())
}

fn discover_shell() -> Option<PathBuf> {
    std::env::var_os("SHELL")
        .map(PathBuf::from)
        .filter(|path| valid_shell(path))
        .or_else(|| {
            [PathBuf::from("/bin/zsh"), PathBuf::from("/bin/sh")]
                .into_iter()
                .find(|path| valid_shell(path))
        })
}

fn valid_shell(path: &Path) -> bool {
    if !path.is_absolute() {
        return false;
    }
    let Ok(metadata) = fs::metadata(path) else {
        return false;
    };
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        metadata.is_file() && metadata.permissions().mode() & 0o111 != 0
    }
    #[cfg(not(unix))]
    {
        metadata.is_file()
    }
}

fn configure_environment(command: &mut CommandBuilder, environment: &ProcessEnvironment) {
    command.env_clear();
    for (name, value) in environment.iter() {
        command.env(name, value);
    }
}

/// An explicit locale inherited from the launching process always wins; any
/// one of these already governs character classification for the shell.
const LOCALE_VARIABLES: [&str; 3] = ["LC_ALL", "LC_CTYPE", "LANG"];

fn terminal_tool_variables(shell: &Path, environment: &SessionEnvironment) -> Vec<ToolVariable> {
    let mut variables = vec![
        ToolVariable::new("TERM", "xterm-256color"),
        ToolVariable::new("COLORTERM", "truecolor"),
        ToolVariable::new("SHELL", shell.as_os_str().to_owned()),
    ];
    // The terminal advertises `encoding: utf-8`, yet a launching process
    // without any locale variable leaves macOS bash 3.2 readline in the C
    // locale, where it classifies input byte-wise and mangles UTF-8 into an
    // unterminated quote so the shell never exits. macOS accepts the
    // region-less `UTF-8` locale name, so `LC_CTYPE=UTF-8` is the minimal
    // deliberate default restoring UTF-8 character classification. The guard
    // keeps an inherited explicit locale authoritative and routes the
    // injection through the same bounded `for_tool` validation as every
    // other tool variable, so environment scrubbing is not weakened. This
    // module is macOS-only; the Windows ConPTY channel is already UTF-8 and
    // remains unaffected.
    if !LOCALE_VARIABLES
        .iter()
        .any(|name| environment.contains(name))
    {
        variables.push(ToolVariable::new("LC_CTYPE", "UTF-8"));
    }
    variables
}

fn configure_shell_arguments(command: &mut CommandBuilder, shell: &Path) {
    match shell.file_name().and_then(|name| name.to_str()) {
        Some("zsh") => command.args(["-f", "-i"]),
        Some("bash") => command.args(["--noprofile", "--norc", "-i"]),
        _ => command.arg("-i"),
    }
}

fn read_output(reader: &mut dyn Read, capture: &Arc<Mutex<Capture>>) {
    let mut buffer = [0_u8; 16 * 1024];
    loop {
        match reader.read(&mut buffer) {
            Ok(0) => {
                if let Ok(mut state) = capture.lock() {
                    state.reader_complete = true;
                }
                return;
            }
            Ok(count) => {
                if let Ok(mut state) = capture.lock() {
                    state.push(&buffer[..count]);
                } else {
                    return;
                }
            }
            Err(cause) => {
                if let Ok(mut state) = capture.lock() {
                    state.reader_complete = true;
                    if cause.raw_os_error() != Some(libc::EIO) {
                        state.reader_error = Some(cause.kind().to_string());
                    }
                }
                return;
            }
        }
    }
}

fn poll_exit(terminal: &mut Terminal) -> Result<(), TerminalError> {
    if terminal.exit_code.is_some() || terminal.exit_signal.is_some() {
        return Ok(());
    }
    match terminal.child.try_wait() {
        Ok(Some(status)) => {
            terminal.exit_code = Some(status.exit_code());
            terminal.exit_signal = status.signal().map(str::to_owned);
            Ok(())
        }
        Ok(None) => Ok(()),
        Err(cause) => Err(error(
            -32094,
            format!("cannot read terminal exit status: {cause}"),
        )),
    }
}

fn snapshot_of(terminal_id: &str, terminal: &Terminal, after: u64) -> TerminalSnapshot {
    let capture = terminal
        .capture
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    let (output, output_start, retained_start) = capture.read_after(after);
    TerminalSnapshot {
        terminal_id: terminal_id.to_owned(),
        session_id: terminal.session_id.clone(),
        project_id: terminal.project_id.clone(),
        shell: terminal.shell.clone(),
        shell_profile: "clean-interactive".into(),
        encoding: "utf-8".into(),
        process_tree_policy: "unix-session-and-foreground-process-group".into(),
        environment: terminal.environment.clone(),
        process_id: terminal.process_id,
        running: terminal.exit_code.is_none() && terminal.exit_signal.is_none(),
        exit_code: terminal.exit_code,
        exit_signal: terminal.exit_signal.clone(),
        rows: terminal.rows,
        cols: terminal.cols,
        output_base64: BASE64_STANDARD.encode(output),
        output_start,
        output_end: capture.output_end,
        omitted_before_start: retained_start,
        capture_limit: CAPTURE_LIMIT,
        reader_complete: capture.reader_complete,
        reader_error: capture.reader_error.clone(),
    }
}

fn signal_foreground_group(terminal: &Terminal, signal: libc::c_int) -> Result<(), TerminalError> {
    let process_group = terminal
        .master
        .process_group_leader()
        .or(terminal.session_leader)
        .ok_or_else(|| error(-32094, "terminal process group is unavailable"))?;
    signal_group(process_group, signal)
}

fn terminal_process_groups(terminal: &Terminal) -> Vec<libc::pid_t> {
    let mut groups = Vec::with_capacity(2);
    if let Some(group) = terminal.master.process_group_leader() {
        if group > 1 {
            groups.push(group);
        }
    }
    if let Some(group) = terminal.session_leader {
        if group > 1 && !groups.contains(&group) {
            groups.push(group);
        }
    }
    groups
}

fn signal_process_groups(process_groups: &[libc::pid_t], signal: libc::c_int) {
    for process_group in process_groups {
        let _ = signal_group(*process_group, signal);
    }
}

fn wait_for_process_groups(process_groups: &[libc::pid_t], timeout: Duration) -> bool {
    let deadline = Instant::now() + timeout;
    loop {
        if process_groups
            .iter()
            .all(|group| !process_group_exists(*group))
        {
            return true;
        }
        if Instant::now() >= deadline {
            return false;
        }
        thread::sleep(Duration::from_millis(10));
    }
}

fn process_group_exists(process_group: libc::pid_t) -> bool {
    if process_group <= 1 {
        return false;
    }
    if unsafe { libc::kill(-process_group, 0) } == 0 {
        return true;
    }
    std::io::Error::last_os_error().raw_os_error() != Some(libc::ESRCH)
}

fn signal_group(process_group: libc::pid_t, signal: libc::c_int) -> Result<(), TerminalError> {
    if process_group <= 1 {
        return Err(error(-32094, "invalid terminal process group"));
    }
    let result = unsafe { libc::kill(-process_group, signal) };
    if result == 0 {
        return Ok(());
    }
    let cause = std::io::Error::last_os_error();
    if cause.raw_os_error() == Some(libc::ESRCH) {
        Ok(())
    } else {
        Err(error(
            -32094,
            format!("cannot signal terminal process group: {cause}"),
        ))
    }
}

fn signal_session_leader(terminal: &Terminal, signal: libc::c_int) {
    if let Some(process_group) = terminal.session_leader {
        let _ = signal_group(process_group, signal);
    }
}

fn terminate_terminal(terminal: &mut Terminal) {
    if poll_exit(terminal).is_ok() && terminal.exit_code.is_none() && terminal.exit_signal.is_none()
    {
        let _ = signal_foreground_group(terminal, libc::SIGHUP);
        signal_session_leader(terminal, libc::SIGHUP);
        let _ = terminal.child.kill();
    }
}

fn error(code: i64, message: impl Into<String>) -> TerminalError {
    TerminalError {
        code,
        message: message.into(),
    }
}

#[cfg(all(test, target_os = "macos"))]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::{Duration, Instant};

    static TEST_DIRECTORY_SEQUENCE: AtomicU64 = AtomicU64::new(0);

    fn manager() -> (TerminalManager, PathBuf, SessionEnvironment) {
        let root = std::env::temp_dir().join(format!(
            "aegisy-terminal-{}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos(),
            TEST_DIRECTORY_SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&root).unwrap();
        let root = root.canonicalize().unwrap();
        let environment = SessionEnvironment::build("session", Some("project"), "work", &root);
        (TerminalManager::default(), root, environment)
    }

    fn wait_for_exit(manager: &mut TerminalManager, terminal_id: &str) -> TerminalSnapshot {
        wait_for_exit_with_timeout(manager, terminal_id, Duration::from_secs(5))
    }

    fn wait_for_exit_with_timeout(
        manager: &mut TerminalManager,
        terminal_id: &str,
        timeout: Duration,
    ) -> TerminalSnapshot {
        let deadline = Instant::now() + timeout;
        loop {
            let snapshot = manager.snapshot(terminal_id, "session", 0).unwrap();
            if !snapshot.running {
                return snapshot;
            }
            assert!(Instant::now() < deadline, "terminal did not exit");
            thread::sleep(Duration::from_millis(10));
        }
    }

    fn open_context<'a>(
        root: &'a Path,
        environment: &'a SessionEnvironment,
    ) -> TerminalOpenContext<'a> {
        TerminalOpenContext {
            session_id: "session",
            project_id: "project",
            root,
            environment,
        }
    }

    #[test]
    fn interactive_unicode_resize_and_exit_status() {
        let (mut manager, root, environment) = manager();
        manager
            .open_user("terminal".into(), open_context(&root, &environment), 24, 80)
            .unwrap();
        manager.resize("terminal", "session", 40, 120).unwrap();
        let command = BASE64_STANDARD.encode("printf '\\033[31m你好 Aegisy\\033[0m\\n'; exit 7\n");
        manager.input_user("terminal", "session", &command).unwrap();
        let snapshot = wait_for_exit(&mut manager, "terminal");
        let output = BASE64_STANDARD.decode(snapshot.output_base64).unwrap();
        assert!(String::from_utf8_lossy(&output).contains("你好 Aegisy"));
        assert!(output.windows(5).any(|window| window == b"\x1b[31m"));
        assert_eq!(snapshot.exit_code, Some(7));
        assert_eq!((snapshot.rows, snapshot.cols), (40, 120));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn bounded_capture_retains_tail_and_reports_omission() {
        let (mut manager, root, environment) = manager();
        manager
            .open_user("terminal".into(), open_context(&root, &environment), 24, 80)
            .unwrap();
        let command = BASE64_STANDARD.encode("yes x | head -c 1200000; exit\n");
        manager.input_user("terminal", "session", &command).unwrap();
        // This fixture transfers more than the complete retained PTY budget. Keep
        // its correctness deadline independent from workspace-test CPU contention;
        // performance budgets are measured by the dedicated release fixture.
        let snapshot =
            wait_for_exit_with_timeout(&mut manager, "terminal", Duration::from_secs(20));
        let output = BASE64_STANDARD.decode(snapshot.output_base64).unwrap();
        assert_eq!(output.len(), CAPTURE_LIMIT);
        assert!(snapshot.omitted_before_start > 0);
        assert!(snapshot.output_end > CAPTURE_LIMIT as u64);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn locale_guard_injects_lc_ctype_only_without_an_inherited_locale() {
        use std::ffi::OsString;

        let root = std::env::temp_dir().join(format!(
            "aegisy-terminal-locale-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir_all(&root).unwrap();
        let root = root.canonicalize().unwrap();
        let shell = discover_shell().unwrap();
        let without_locale =
            SessionEnvironment::build_from("session", Some("project"), "work", &root, vec![]);
        let variables = terminal_tool_variables(&shell, &without_locale);
        assert_eq!(
            variables
                .iter()
                .filter(|variable| variable.name == "LC_CTYPE")
                .count(),
            1
        );
        assert!(variables
            .iter()
            .any(|variable| variable.name == "LC_CTYPE" && variable.value == "UTF-8"));
        let process = without_locale.for_tool("terminal", variables).unwrap();
        assert!(process
            .summary()
            .explicit_variable_names
            .contains(&"LC_CTYPE".into()));
        for name in LOCALE_VARIABLES {
            let with_locale = SessionEnvironment::build_from(
                "session",
                Some("project"),
                "work",
                &root,
                vec![(OsString::from(name), OsString::from("C"))],
            );
            let variables = terminal_tool_variables(&shell, &with_locale);
            assert!(!variables.iter().any(|variable| variable.name == "LC_CTYPE"));
            // The inherited locale stays authoritative and unoverridden.
            with_locale.for_tool("terminal", variables).unwrap();
        }
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn rejects_invalid_dimensions_and_cross_session_access() {
        let (mut manager, root, environment) = manager();
        assert_eq!(
            manager
                .open_user("invalid".into(), open_context(&root, &environment), 0, 80,)
                .unwrap_err()
                .code,
            -32602
        );
        manager
            .open_user("terminal".into(), open_context(&root, &environment), 24, 80)
            .unwrap();
        assert_eq!(
            manager
                .snapshot("terminal", "other-session", 0)
                .unwrap_err()
                .code,
            -32093
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn validates_shells_scrubs_environment_and_reports_startup_failure() {
        use std::os::unix::fs::PermissionsExt;

        let (mut manager, root, environment) = manager();
        let relative = PathBuf::from("relative-shell");
        assert!(!valid_shell(&relative));
        let fake_shell = root.join("fake-shell");
        fs::write(&fake_shell, "#!/bin/sh\n").unwrap();
        assert!(!valid_shell(&fake_shell));
        fs::set_permissions(&fake_shell, fs::Permissions::from_mode(0o700)).unwrap();
        assert!(valid_shell(&fake_shell));

        let shell = discover_shell().unwrap();
        let mut command = CommandBuilder::new(&shell);
        let process_environment = environment
            .for_tool(
                "terminal",
                vec![
                    ToolVariable::new("TERM", "xterm-256color"),
                    ToolVariable::new("COLORTERM", "truecolor"),
                    ToolVariable::new("SHELL", shell.clone()),
                ],
            )
            .unwrap();
        configure_environment(&mut command, &process_environment);
        assert!(command.get_env("PATH").is_some());
        assert!(command.get_env("TERM").is_some());
        assert!(command.get_env("OPENAI_API_KEY").is_none());
        assert!(command.get_env("ANTHROPIC_API_KEY").is_none());

        let missing = root.join("removed-workspace");
        let failure = manager
            .open_user(
                "terminal".into(),
                open_context(&missing, &environment),
                24,
                80,
            )
            .unwrap_err();
        assert_eq!(failure.code, -32092);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn signal_targets_foreground_process_group() {
        let (mut manager, root, environment) = manager();
        manager
            .open_user("terminal".into(), open_context(&root, &environment), 24, 80)
            .unwrap();
        let command = BASE64_STANDARD.encode("sleep 30\n");
        manager.input_user("terminal", "session", &command).unwrap();
        let deadline = Instant::now() + Duration::from_secs(2);
        loop {
            let terminal = manager.terminals.get("terminal").unwrap();
            if terminal.master.process_group_leader() != terminal.session_leader {
                break;
            }
            assert!(
                Instant::now() < deadline,
                "command did not enter foreground"
            );
            thread::sleep(Duration::from_millis(10));
        }
        manager
            .signal_user("terminal", "session", "interrupt")
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(2);
        loop {
            let terminal = manager.terminals.get("terminal").unwrap();
            if terminal.master.process_group_leader() == terminal.session_leader {
                break;
            }
            assert!(Instant::now() < deadline, "shell did not regain foreground");
            thread::sleep(Duration::from_millis(10));
        }
        let next_command = BASE64_STANDARD.encode("printf 'alive\\n'; exit\n");
        manager
            .input_user("terminal", "session", &next_command)
            .unwrap();
        let snapshot = wait_for_exit(&mut manager, "terminal");
        let output = BASE64_STANDARD.decode(snapshot.output_base64).unwrap();
        assert!(String::from_utf8_lossy(&output).contains("alive"));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn close_signals_a_running_foreground_process_and_reports_exit() {
        let (mut manager, root, environment) = manager();
        manager
            .open_user("terminal".into(), open_context(&root, &environment), 24, 80)
            .unwrap();
        let command = BASE64_STANDARD.encode("sleep 30\n");
        manager.input_user("terminal", "session", &command).unwrap();
        let deadline = Instant::now() + Duration::from_secs(2);
        loop {
            let terminal = manager.terminals.get("terminal").unwrap();
            if terminal.master.process_group_leader() != terminal.session_leader {
                break;
            }
            assert!(
                Instant::now() < deadline,
                "command did not enter foreground"
            );
            thread::sleep(Duration::from_millis(10));
        }
        manager.close_user("terminal", "session").unwrap();
        let snapshot = wait_for_exit(&mut manager, "terminal");
        assert!(!snapshot.running);
        assert!(snapshot.exit_code.is_some() || snapshot.exit_signal.is_some());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn close_forces_a_foreground_group_that_ignores_graceful_signals() {
        let (mut manager, root, environment) = manager();
        manager
            .open_user("terminal".into(), open_context(&root, &environment), 24, 80)
            .unwrap();
        let command =
            BASE64_STANDARD.encode("sh -c 'trap \"\" HUP TERM; while :; do sleep 1; done'\n");
        manager.input_user("terminal", "session", &command).unwrap();
        let deadline = Instant::now() + Duration::from_secs(2);
        let foreground_group = loop {
            let terminal = manager.terminals.get("terminal").unwrap();
            if let Some(group) = terminal.master.process_group_leader() {
                if Some(group) != terminal.session_leader {
                    break group;
                }
            }
            assert!(
                Instant::now() < deadline,
                "command did not enter foreground"
            );
            thread::sleep(Duration::from_millis(10));
        };
        thread::sleep(Duration::from_millis(50));

        let started = Instant::now();
        manager.close_user("terminal", "session").unwrap();
        assert!(
            started.elapsed() < Duration::from_secs(2),
            "forced terminal stop exceeded its bounded grace periods"
        );
        let snapshot = wait_for_exit(&mut manager, "terminal");
        assert!(!snapshot.running);
        assert!(!process_group_exists(foreground_group));
        fs::remove_dir_all(root).unwrap();
    }
}
