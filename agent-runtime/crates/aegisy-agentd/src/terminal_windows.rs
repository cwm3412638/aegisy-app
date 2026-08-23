use crate::session_environment::{
    EnvironmentSummary, ProcessEnvironment, SessionEnvironment, ToolVariable,
};
use base64::engine::general_purpose::STANDARD as BASE64_STANDARD;
use base64::Engine;
use portable_pty::{native_pty_system, Child, CommandBuilder, MasterPty, PtySize};
use serde::Serialize;
use std::collections::{HashMap, VecDeque};
use std::ffi::{c_void, OsString};
use std::fs;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::thread;
#[cfg(test)]
use windows_sys::Win32::Foundation::WAIT_OBJECT_0;
use windows_sys::Win32::Foundation::{CloseHandle, DuplicateHandle, DUPLICATE_SAME_ACCESS, HANDLE};
use windows_sys::Win32::System::JobObjects::{
    AssignProcessToJobObject, CreateJobObjectW, JobObjectExtendedLimitInformation,
    SetInformationJobObject, TerminateJobObject, JOBOBJECT_EXTENDED_LIMIT_INFORMATION,
    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
};
#[cfg(test)]
use windows_sys::Win32::System::Threading::WaitForSingleObject;
use windows_sys::Win32::System::Threading::{GetCurrentProcess, GetCurrentThread};
use windows_sys::Win32::System::IO::CancelSynchronousIo;

const CAPTURE_LIMIT: usize = 1024 * 1024;
const MAX_TERMINALS: usize = 64;
const MAX_TERMINALS_PER_SESSION: usize = 16;
const MAX_INPUT_BYTES: usize = 64 * 1024;
const MAX_DIMENSION: u16 = 1_000;
const ERROR_BROKEN_PIPE: i32 = 109;
const ERROR_OPERATION_ABORTED: i32 = 995;

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
            self.bytes
                .extend(input[input.len() - CAPTURE_LIMIT..].iter().copied());
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

struct JobObject {
    handle: HANDLE,
}

// SAFETY: a Windows HANDLE is a kernel object reference usable from any
// thread. The JobObject is exclusively owned by one Terminal inside the
// Mutex-protected registry, so no unsynchronized shared access can occur.
unsafe impl Send for JobObject {}

#[derive(Clone, Copy)]
struct ReaderThreadHandle(HANDLE);

// SAFETY: the duplicated thread handle is a kernel object reference usable
// from any thread. It is only used to cancel that thread's blocking
// synchronous read during teardown and is closed exactly once by the owner.
unsafe impl Send for ReaderThreadHandle {}
unsafe impl Sync for ReaderThreadHandle {}

impl JobObject {
    fn assign(child: &dyn Child) -> Result<Self, TerminalError> {
        let handle = unsafe { CreateJobObjectW(std::ptr::null(), std::ptr::null()) };
        if handle.is_null() {
            return Err(last_os_error(-32092, "cannot create terminal Job Object"));
        }
        let job = Self { handle };
        let mut limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        let configured = unsafe {
            SetInformationJobObject(
                job.handle,
                JobObjectExtendedLimitInformation,
                &limits as *const _ as *const c_void,
                std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
            )
        };
        if configured == 0 {
            return Err(last_os_error(
                -32092,
                "cannot configure terminal Job Object",
            ));
        }
        let process = child
            .as_raw_handle()
            .ok_or_else(|| error(-32092, "terminal process handle is unavailable"))?;
        let assigned = unsafe { AssignProcessToJobObject(job.handle, process as HANDLE) };
        if assigned == 0 {
            return Err(last_os_error(
                -32092,
                "cannot assign terminal process to Job Object",
            ));
        }
        Ok(job)
    }

    fn terminate(&self) -> Result<(), TerminalError> {
        let terminated = unsafe { TerminateJobObject(self.handle, 1) };
        if terminated == 0 {
            Err(last_os_error(
                -32094,
                "cannot terminate terminal process tree",
            ))
        } else {
            Ok(())
        }
    }

    #[cfg(test)]
    fn wait_until_empty(&self, timeout_ms: u32) -> bool {
        unsafe { WaitForSingleObject(self.handle, timeout_ms) == WAIT_OBJECT_0 }
    }
}

impl Drop for JobObject {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe {
                CloseHandle(self.handle);
            }
        }
    }
}

struct ShellSpec {
    path: PathBuf,
    profile: &'static str,
    kind: ShellKind,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ShellKind {
    PowerShellCore,
    WindowsPowerShell,
    CommandPrompt,
}

struct Terminal {
    session_id: String,
    project_id: String,
    shell: String,
    shell_profile: String,
    environment: EnvironmentSummary,
    process_id: Option<u32>,
    rows: u16,
    cols: u16,
    master: Option<Box<dyn MasterPty + Send>>,
    writer: Box<dyn Write + Send>,
    child: Box<dyn Child + Send + Sync>,
    job: JobObject,
    capture: Arc<Mutex<Capture>>,
    reader_thread: Arc<Mutex<Option<ReaderThreadHandle>>>,
    reader_join: Option<thread::JoinHandle<()>>,
    exit_code: Option<u32>,
}

#[derive(Default)]
pub struct TerminalManager {
    terminals: HashMap<String, Terminal>,
}

impl TerminalManager {
    pub fn capability() -> &'static str {
        "terminal.conpty.windows.user-initiated"
    }

    pub fn open_user(
        &mut self,
        terminal_id: String,
        context: TerminalOpenContext<'_>,
        rows: u16,
        cols: u16,
    ) -> Result<TerminalSnapshot, TerminalError> {
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

        let shell = discover_shell(context.root)
            .ok_or_else(|| error(-32092, "PowerShell and cmd.exe are unavailable"))?;
        self.open_user_with_shell(terminal_id, context, rows, cols, shell)
    }

    fn open_user_with_shell(
        &mut self,
        terminal_id: String,
        context: TerminalOpenContext<'_>,
        rows: u16,
        cols: u16,
        shell: ShellSpec,
    ) -> Result<TerminalSnapshot, TerminalError> {
        let pty = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            native_pty_system().openpty(PtySize {
                rows,
                cols,
                pixel_width: 0,
                pixel_height: 0,
            })
        }))
        .map_err(|_| {
            error(
                -32090,
                "Windows ConPTY requires Windows 10 version 1809 or newer",
            )
        })?
        .map_err(|cause| error(-32092, format!("cannot open Windows ConPTY: {cause}")))?;
        let mut command = CommandBuilder::new(&shell.path);
        configure_shell_arguments(&mut command, shell.kind);
        command.cwd(context.root);
        let mut tool_variables = vec![
            ToolVariable::new("TERM", "xterm-256color"),
            ToolVariable::new("COLORTERM", "truecolor"),
        ];
        if let Some(module_path) = powershell_module_path(context.root) {
            tool_variables.push(ToolVariable::new("PSModulePath", module_path));
        }
        let process_environment = context
            .environment
            .for_tool("terminal", tool_variables)
            .map_err(|cause| error(-32096, cause.message))?;
        configure_environment(&mut command, &process_environment);
        let mut child = pty
            .slave
            .spawn_command(command)
            .map_err(|cause| error(-32092, format!("cannot start terminal shell: {cause}")))?;
        drop(pty.slave);
        let job = match JobObject::assign(child.as_ref()) {
            Ok(job) => job,
            Err(cause) => {
                let _ = child.kill();
                return Err(cause);
            }
        };
        let process_id = child.process_id();
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
        let reader_thread: Arc<Mutex<Option<ReaderThreadHandle>>> = Arc::new(Mutex::new(None));
        let registered_thread = Arc::clone(&reader_thread);
        let reader_join = thread::Builder::new()
            .name(format!("aegisy-conpty-reader-{terminal_id}"))
            .spawn(move || {
                register_current_thread(&registered_thread);
                read_output(&mut reader, &reader_capture);
            })
            .map_err(|cause| error(-32092, format!("cannot start terminal reader: {cause}")))?;

        self.terminals.insert(
            terminal_id.clone(),
            Terminal {
                session_id: context.session_id.to_owned(),
                project_id: context.project_id.to_owned(),
                shell: shell
                    .path
                    .file_name()
                    .and_then(|name| name.to_str())
                    .unwrap_or("shell.exe")
                    .to_owned(),
                shell_profile: shell.profile.into(),
                environment: process_environment.summary().clone(),
                process_id,
                rows,
                cols,
                master: Some(pty.master),
                writer,
                child,
                job,
                capture,
                reader_thread,
                reader_join: Some(reader_join),
                exit_code: None,
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
        if terminal.exit_code.is_some() {
            return Err(error(-32094, "terminal process has exited"));
        }
        write_terminal(terminal, &input)?;
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
        let master = terminal
            .master
            .as_ref()
            .ok_or_else(|| error(-32094, "terminal console is closing"))?;
        master
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
        let terminal = self.bound_terminal_mut(terminal_id, session_id)?;
        poll_exit(terminal)?;
        if terminal.exit_code.is_some() {
            return Ok(());
        }
        match signal {
            "interrupt" => write_terminal(terminal, b"\x03"),
            "terminate" | "hangup" | "kill" => terminal.job.terminate(),
            _ => Err(error(-32602, "unsupported terminal signal")),
        }
    }

    pub fn close_user(
        &mut self,
        terminal_id: &str,
        session_id: &str,
    ) -> Result<TerminalSnapshot, TerminalError> {
        {
            let terminal = self.bound_terminal_mut(terminal_id, session_id)?;
            poll_exit(terminal)?;
            if terminal.exit_code.is_none() {
                terminal.job.terminate()?;
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
        teardown_terminal(&mut terminal);
        Ok(())
    }

    pub fn shutdown_all(&mut self) {
        for terminal in self.terminals.values_mut() {
            teardown_terminal(terminal);
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
            teardown_terminal(terminal);
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
    // Path::canonicalize returns a verbatim \\?\ path on Windows, so compare
    // the plain forms to avoid rejecting every ordinary project root there.
    if crate::plain_path(&canonical) != crate::plain_path(root) {
        return Err(error(
            -32092,
            "terminal project root changed after project authorization",
        ));
    }
    Ok(())
}

fn discover_shell(root: &Path) -> Option<ShellSpec> {
    discover_shell_with(root, &|name| std::env::var_os(name))
}

fn discover_shell_with<F>(root: &Path, environment: &F) -> Option<ShellSpec>
where
    F: Fn(&str) -> Option<OsString>,
{
    let mut candidates = Vec::new();
    for variable in ["ProgramW6432", "ProgramFiles"] {
        if let Some(base) = absolute_env_path_with(variable, environment) {
            candidates.push((
                base.join("PowerShell").join("7").join("pwsh.exe"),
                ShellKind::PowerShellCore,
            ));
        }
    }
    candidates.extend(path_candidates_with(
        "pwsh.exe",
        ShellKind::PowerShellCore,
        environment,
    ));
    if let Some(system_root) = system_root_with(environment) {
        candidates.push((
            system_root
                .join("System32")
                .join("WindowsPowerShell")
                .join("v1.0")
                .join("powershell.exe"),
            ShellKind::WindowsPowerShell,
        ));
    }
    candidates.extend(path_candidates_with(
        "powershell.exe",
        ShellKind::WindowsPowerShell,
        environment,
    ));
    if let Some(comspec) = absolute_env_path_with("ComSpec", environment) {
        candidates.push((comspec, ShellKind::CommandPrompt));
    }
    if let Some(system_root) = system_root_with(environment) {
        candidates.push((
            system_root.join("System32").join("cmd.exe"),
            ShellKind::CommandPrompt,
        ));
    }

    candidates.into_iter().find_map(|(candidate, kind)| {
        canonical_shell(&candidate, root).map(|path| ShellSpec {
            path,
            profile: match kind {
                ShellKind::PowerShellCore => "pwsh-clean-no-profile",
                ShellKind::WindowsPowerShell => "windows-powershell-clean-no-profile",
                ShellKind::CommandPrompt => "cmd-clean-no-autorun",
            },
            kind,
        })
    })
}

fn canonical_shell(candidate: &Path, root: &Path) -> Option<PathBuf> {
    if !candidate.is_absolute() {
        return None;
    }
    let canonical = candidate.canonicalize().ok()?;
    let metadata = fs::metadata(&canonical).ok()?;
    let executable = canonical
        .extension()
        .and_then(|extension| extension.to_str())
        .is_some_and(|extension| extension.eq_ignore_ascii_case("exe"));
    if metadata.is_file() && executable && !path_is_within(root, &canonical) {
        // PowerShell derives $PSHOME from the executable path; launching pwsh
        // through a `\\?\` verbatim path breaks built-in module loading
        // ("Cannot find the built-in module '\\?\c:\program files\...'").
        Some(crate::plain_path(&canonical))
    } else {
        None
    }
}

fn path_candidates_with<F>(
    name: &str,
    kind: ShellKind,
    environment: &F,
) -> Vec<(PathBuf, ShellKind)>
where
    F: Fn(&str) -> Option<OsString>,
{
    environment("PATH")
        .map(|path| {
            std::env::split_paths(&path)
                .filter(|directory| directory.is_absolute())
                .map(|directory| (directory.join(name), kind))
                .collect()
        })
        .unwrap_or_default()
}

fn absolute_env_path(name: &str) -> Option<PathBuf> {
    absolute_env_path_with(name, &|name| std::env::var_os(name))
}

fn absolute_env_path_with<F>(name: &str, environment: &F) -> Option<PathBuf>
where
    F: Fn(&str) -> Option<OsString>,
{
    environment(name)
        .map(PathBuf::from)
        .filter(|path| path.is_absolute())
}

fn system_root() -> Option<PathBuf> {
    system_root_with(&|name| std::env::var_os(name))
}

fn system_root_with<F>(environment: &F) -> Option<PathBuf>
where
    F: Fn(&str) -> Option<OsString>,
{
    absolute_env_path_with("SystemRoot", environment)
        .or_else(|| absolute_env_path_with("WINDIR", environment))
}

fn configure_shell_arguments(command: &mut CommandBuilder, kind: ShellKind) {
    match kind {
        ShellKind::PowerShellCore | ShellKind::WindowsPowerShell => {
            command.args(["-NoLogo", "-NoProfile", "-NoExit"]);
        }
        ShellKind::CommandPrompt => command.args(["/D", "/Q", "/K", "chcp 65001>nul"]),
    }
}

fn configure_environment(command: &mut CommandBuilder, environment: &ProcessEnvironment) {
    command.env_clear();
    for (name, value) in environment.iter() {
        command.env(name, value);
    }
}

fn powershell_module_path(root: &Path) -> Option<OsString> {
    let system_root = system_root()?;
    let mut modules = vec![system_root
        .join("System32")
        .join("WindowsPowerShell")
        .join("v1.0")
        .join("Modules")];
    if let Some(program_files) = absolute_env_path("ProgramFiles") {
        modules.push(program_files.join("PowerShell").join("Modules"));
    }
    modules.retain(|path| path.is_absolute() && !path_is_within(root, path));
    std::env::join_paths(modules).ok()
}

fn path_is_within(root: &Path, candidate: &Path) -> bool {
    let root = root
        .to_string_lossy()
        .replace('/', "\\")
        .to_ascii_lowercase();
    let candidate = candidate
        .to_string_lossy()
        .replace('/', "\\")
        .to_ascii_lowercase();
    candidate == root
        || candidate
            .strip_prefix(&root)
            .is_some_and(|suffix| suffix.starts_with('\\'))
}

fn write_terminal(terminal: &mut Terminal, bytes: &[u8]) -> Result<(), TerminalError> {
    terminal
        .writer
        .write_all(bytes)
        .and_then(|_| terminal.writer.flush())
        .map_err(|cause| error(-32094, format!("cannot write terminal input: {cause}")))
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
                    if !matches!(
                        cause.raw_os_error(),
                        Some(ERROR_BROKEN_PIPE) | Some(ERROR_OPERATION_ABORTED)
                    ) {
                        state.reader_error = Some(cause.kind().to_string());
                    }
                }
                return;
            }
        }
    }
}

fn poll_exit(terminal: &mut Terminal) -> Result<(), TerminalError> {
    if terminal.exit_code.is_some() {
        return Ok(());
    }
    match terminal.child.try_wait() {
        Ok(Some(status)) => {
            terminal.exit_code = Some(status.exit_code());
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
        shell_profile: terminal.shell_profile.clone(),
        encoding: "utf-8".into(),
        process_tree_policy: "windows-job-object-kill-on-close".into(),
        environment: terminal.environment.clone(),
        process_id: terminal.process_id,
        running: terminal.exit_code.is_none(),
        exit_code: terminal.exit_code,
        exit_signal: None,
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

fn terminate_terminal(terminal: &mut Terminal) {
    if poll_exit(terminal).is_ok() && terminal.exit_code.is_none() {
        let _ = terminal.job.terminate();
    }
}

fn register_current_thread(slot: &Arc<Mutex<Option<ReaderThreadHandle>>>) {
    // GetCurrentThread is a pseudo handle that is only meaningful inside this
    // thread, so duplicate it into a real handle that the owner can use to
    // cancel this thread's blocking synchronous ReadFile during teardown.
    let mut real = std::ptr::null_mut();
    let duplicated = unsafe {
        DuplicateHandle(
            GetCurrentProcess(),
            GetCurrentThread(),
            GetCurrentProcess(),
            &mut real,
            0,
            0,
            DUPLICATE_SAME_ACCESS,
        )
    };
    if duplicated == 0 {
        return;
    }
    match slot.lock() {
        Ok(mut slot) => *slot = Some(ReaderThreadHandle(real)),
        Err(_) => unsafe {
            CloseHandle(real);
        },
    }
}

fn unblock_reader(terminal: &mut Terminal) {
    // ClosePseudoConsole (MasterPty drop) can hang indefinitely while the
    // reader thread is blocked in ReadFile on the ConPTY output pipe. Cancel
    // that read first so the reader exits before the master is dropped.
    let deadline = std::time::Instant::now() + std::time::Duration::from_millis(1_000);
    let handle = loop {
        let registered = terminal
            .reader_thread
            .lock()
            .ok()
            .and_then(|mut slot| slot.take());
        if registered.is_some() || std::time::Instant::now() >= deadline {
            break registered;
        }
        thread::sleep(std::time::Duration::from_millis(5));
    };
    if let Some(handle) = handle {
        unsafe {
            CancelSynchronousIo(handle.0);
            CloseHandle(handle.0);
        }
    }
    if let Some(join) = terminal.reader_join.take() {
        let (sender, receiver) = std::sync::mpsc::channel();
        thread::spawn(move || {
            let _ = join.join();
            let _ = sender.send(());
        });
        let _ = receiver.recv_timeout(std::time::Duration::from_secs(5));
    }
}

fn drop_master_bounded(master: Box<dyn MasterPty + Send>) {
    let (sender, receiver) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        drop(master);
        let _ = sender.send(());
    });
    // If the close wedges, the helper thread and its console handles are
    // intentionally leaked until process exit instead of hanging teardown.
    let _ = receiver.recv_timeout(std::time::Duration::from_secs(5));
}

fn teardown_terminal(terminal: &mut Terminal) {
    terminate_terminal(&mut *terminal);
    // Pre-24H2 ClosePseudoConsole waits for the console host to exit, which
    // requires the output pipe to keep being read. Give the reader a bounded
    // window to observe end-of-file now that the client process is gone.
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
    loop {
        let complete = terminal
            .capture
            .lock()
            .map(|state| state.reader_complete)
            .unwrap_or(true);
        if complete || std::time::Instant::now() >= deadline {
            break;
        }
        thread::sleep(std::time::Duration::from_millis(10));
    }
    // ClosePseudoConsole can still wedge (for example when a surviving
    // grandchild keeps the console alive), so drop the master on a helper
    // thread with a bounded wait and abandon it rather than hanging the
    // owning thread.
    if let Some(master) = terminal.master.take() {
        drop_master_bounded(master);
    }
    unblock_reader(terminal);
}

fn last_os_error(code: i64, context: &str) -> TerminalError {
    error(
        code,
        format!("{context}: {}", std::io::Error::last_os_error()),
    )
}

fn error(code: i64, message: impl Into<String>) -> TerminalError {
    TerminalError {
        code,
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

    fn manager() -> (TerminalManager, PathBuf, SessionEnvironment) {
        let root = std::env::temp_dir().join(format!(
            "aegisy-conpty-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir_all(&root).unwrap();
        let root = root.canonicalize().unwrap();
        let environment = SessionEnvironment::build("session", Some("project"), "work", &root);
        (TerminalManager::default(), root, environment)
    }

    fn wait_for_exit(
        manager: &mut TerminalManager,
        terminal_id: &str,
        cursor_queries: &mut crate::terminal_test_support::CursorPositionQueryTracker,
    ) -> TerminalSnapshot {
        // Cold PowerShell startup under CI load and first-run antivirus
        // scanning can exceed ten seconds before the input is processed.
        let deadline = Instant::now() + Duration::from_secs(60);
        loop {
            let snapshot = snapshot_answering_cursor_queries(manager, terminal_id, cursor_queries);
            if !snapshot.running {
                return snapshot;
            }
            let output = BASE64_STANDARD
                .decode(&snapshot.output_base64)
                .unwrap_or_default();
            if Instant::now() >= deadline {
                panic!(
                    "terminal did not exit; captured output: {:?}",
                    String::from_utf8_lossy(&output)
                );
            }
            thread::sleep(Duration::from_millis(20));
        }
    }

    fn snapshot_answering_cursor_queries(
        manager: &mut TerminalManager,
        terminal_id: &str,
        cursor_queries: &mut crate::terminal_test_support::CursorPositionQueryTracker,
    ) -> TerminalSnapshot {
        let snapshot = manager.snapshot(terminal_id, "session", 0).unwrap();
        let output = BASE64_STANDARD
            .decode(&snapshot.output_base64)
            .unwrap_or_default();
        // A real terminal (xterm.js in production) answers the shell's
        // cursor-position report queries; PSReadLine waits for that reply
        // before processing further input.
        let queries = cursor_queries
            .observe(snapshot.output_start, snapshot.output_end, &output)
            .expect("terminal cursor-query fixture lost output continuity");
        if snapshot.running {
            for _ in 0..queries {
                let reply =
                    BASE64_STANDARD.encode(format!("\x1b[{};{}R", snapshot.rows, snapshot.cols));
                if manager.input_user(terminal_id, "session", &reply).is_err() {
                    let current = manager
                        .snapshot(terminal_id, "session", 0)
                        .unwrap_or_else(|_| panic!("CONPTY_DSR_REPLY_SNAPSHOT_FAILED"));
                    if !current.running {
                        return current;
                    }
                    panic!("CONPTY_DSR_REPLY_FAILED");
                }
            }
        }
        snapshot
    }

    fn output_after<'a>(
        snapshot: &TerminalSnapshot,
        output: &'a [u8],
        after: u64,
        stage: &'static str,
    ) -> &'a [u8] {
        crate::terminal_test_support::retained_output_after(
            snapshot.output_start,
            snapshot.output_end,
            output,
            after,
        )
        .unwrap_or_else(|_| panic!("{stage}:output-range-invalid"))
    }

    fn wait_for_output_after_until(
        manager: &mut TerminalManager,
        terminal_id: &str,
        after: u64,
        expected: &[u8],
        deadline: Instant,
        stage: &'static str,
        cursor_queries: &mut crate::terminal_test_support::CursorPositionQueryTracker,
    ) -> TerminalSnapshot {
        loop {
            let snapshot = snapshot_answering_cursor_queries(manager, terminal_id, cursor_queries);
            let output = BASE64_STANDARD
                .decode(&snapshot.output_base64)
                .unwrap_or_default();
            let matched = output_after(&snapshot, &output, after, stage)
                .windows(expected.len())
                .any(|window| window == expected);
            match crate::terminal_test_support::classify_conpty_wait(
                Instant::now(),
                deadline,
                matched,
                snapshot.running,
            ) {
                crate::terminal_test_support::ConptyWaitDecision::TimedOut => {
                    panic!("{stage}:timeout")
                }
                crate::terminal_test_support::ConptyWaitDecision::Matched => return snapshot,
                crate::terminal_test_support::ConptyWaitDecision::Exited => {
                    panic!("{stage}:terminal-exited")
                }
                crate::terminal_test_support::ConptyWaitDecision::Pending => {}
            }
            thread::sleep(Duration::from_millis(20));
        }
    }

    fn wait_for_ansi_after_until(
        manager: &mut TerminalManager,
        terminal_id: &str,
        after: u64,
        marker: &[u8],
        deadline: Instant,
        stage: &'static str,
        cursor_queries: &mut crate::terminal_test_support::CursorPositionQueryTracker,
    ) -> TerminalSnapshot {
        loop {
            let snapshot = snapshot_answering_cursor_queries(manager, terminal_id, cursor_queries);
            let output = BASE64_STANDARD
                .decode(&snapshot.output_base64)
                .unwrap_or_default();
            let matched = crate::terminal_test_support::contains_non_reset_sgr_wrapped_marker(
                output_after(&snapshot, &output, after, stage),
                marker,
            );
            match crate::terminal_test_support::classify_conpty_wait(
                Instant::now(),
                deadline,
                matched,
                snapshot.running,
            ) {
                crate::terminal_test_support::ConptyWaitDecision::TimedOut => {
                    panic!("{stage}:timeout")
                }
                crate::terminal_test_support::ConptyWaitDecision::Matched => return snapshot,
                crate::terminal_test_support::ConptyWaitDecision::Exited => {
                    panic!("{stage}:terminal-exited")
                }
                crate::terminal_test_support::ConptyWaitDecision::Pending => {}
            }
            thread::sleep(Duration::from_millis(20));
        }
    }

    fn wait_for_exit_until(
        manager: &mut TerminalManager,
        terminal_id: &str,
        deadline: Instant,
        stage: &'static str,
        cursor_queries: &mut crate::terminal_test_support::CursorPositionQueryTracker,
    ) -> TerminalSnapshot {
        loop {
            let snapshot = snapshot_answering_cursor_queries(manager, terminal_id, cursor_queries);
            match crate::terminal_test_support::classify_conpty_wait(
                Instant::now(),
                deadline,
                !snapshot.running,
                snapshot.running,
            ) {
                crate::terminal_test_support::ConptyWaitDecision::TimedOut => {
                    panic!("{stage}:timeout")
                }
                crate::terminal_test_support::ConptyWaitDecision::Matched => return snapshot,
                crate::terminal_test_support::ConptyWaitDecision::Exited => {
                    panic!("{stage}:terminal-exited")
                }
                crate::terminal_test_support::ConptyWaitDecision::Pending => {}
            }
            thread::sleep(Duration::from_millis(20));
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

    fn run_conpty_stage_with_timeout(
        stage: &'static str,
        timeout: Duration,
        body: impl FnOnce() + Send + 'static,
    ) {
        let (sender, receiver) = std::sync::mpsc::channel();
        std::thread::spawn(move || {
            body();
            let _ = sender.send(());
        });
        match receiver.recv_timeout(timeout) {
            Ok(()) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {
                panic!("ConPTY test stage timed out: {stage}");
            }
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
                panic!("ConPTY test stage panicked: {stage}");
            }
        }
    }

    fn run_conpty_stage(stage: &'static str, body: impl FnOnce() + Send + 'static) {
        run_conpty_stage_with_timeout(stage, Duration::from_secs(120), body);
    }

    #[test]
    fn conpty_open_resize_unicode_echo_and_exit_status() {
        run_conpty_stage("open-resize-unicode-exit", || {
            let (mut manager, root, environment) = manager();
            eprintln!("conpty: opening terminal");
            let opened = manager
                .open_user("terminal".into(), open_context(&root, &environment), 24, 80)
                .unwrap();
            assert_eq!(opened.encoding, "utf-8");
            assert_eq!(
                opened.process_tree_policy,
                "windows-job-object-kill-on-close"
            );
            eprintln!("conpty: resizing terminal");
            manager.resize("terminal", "session", 40, 120).unwrap();
            // Give the shell a moment to finish its first prompt before the
            // command arrives; ConPTY buffers early input but a cold
            // PowerShell start under CI load can be slow.
            thread::sleep(Duration::from_millis(500));
            let command = BASE64_STANDARD.encode("echo 终端协议正常\r\nexit 7\r\n");
            eprintln!("conpty: sending echo and exit");
            manager.input_user("terminal", "session", &command).unwrap();
            eprintln!("conpty: waiting for exit");
            let mut cursor_queries =
                crate::terminal_test_support::CursorPositionQueryTracker::default();
            let snapshot = wait_for_exit(&mut manager, "terminal", &mut cursor_queries);
            let output = BASE64_STANDARD.decode(snapshot.output_base64).unwrap();
            assert!(String::from_utf8_lossy(&output).contains("终端协议正常"));
            assert_eq!(snapshot.exit_code, Some(7));
            assert_eq!((snapshot.rows, snapshot.cols), (40, 120));
            eprintln!("conpty: cleanup");
            // Tear the console down before deleting its working directory;
            // Windows refuses to remove a live process working directory.
            manager.shutdown_all();
            drop(manager);
            fs::remove_dir_all(root).unwrap();
        });
    }

    #[test]
    fn conpty_job_termination_kills_long_running_process_tree() {
        run_conpty_stage("job-termination", || {
            let (mut manager, root, environment) = manager();
            eprintln!("conpty: opening long-running terminal");
            let second = manager
                .open_user("long".into(), open_context(&root, &environment), 24, 80)
                .unwrap();
            assert!(second.running);
            let long = BASE64_STANDARD.encode("ping -t 127.0.0.1\r\n");
            eprintln!("conpty: starting long-running command");
            manager.input_user("long", "session", &long).unwrap();
            thread::sleep(Duration::from_millis(100));
            eprintln!("conpty: closing terminal through job termination");
            manager.close_user("long", "session").unwrap();
            let mut cursor_queries =
                crate::terminal_test_support::CursorPositionQueryTracker::default();
            assert!(!wait_for_exit(&mut manager, "long", &mut cursor_queries).running);
            eprintln!("conpty: waiting for empty job");
            assert!(manager
                .terminals
                .get("long")
                .unwrap()
                .job
                .wait_until_empty(5_000));
            eprintln!("conpty: cleanup");
            manager.shutdown_all();
            drop(manager);
            fs::remove_dir_all(root).unwrap();
        });
    }

    #[test]
    fn conpty_interrupt_keeps_shell_alive_and_preserves_ansi() {
        run_conpty_stage_with_timeout("interrupt-ansi", Duration::from_secs(150), || {
            let (mut manager, root, environment) = manager();
            let deadline = Instant::now() + Duration::from_secs(120);
            let opened = manager
                .open_user(
                    "interrupt".into(),
                    open_context(&root, &environment),
                    24,
                    80,
                )
                .unwrap();
            let mut cursor_queries =
                crate::terminal_test_support::CursorPositionQueryTracker::default();
            let initial =
                snapshot_answering_cursor_queries(&mut manager, "interrupt", &mut cursor_queries);
            let command_prompt = opened.shell_profile == "cmd-clean-no-autorun";
            let prompt_setup =
                crate::terminal_test_support::conpty_interrupt_prompt_setup(command_prompt);
            manager
                .input_user(
                    "interrupt",
                    "session",
                    &BASE64_STANDARD.encode(prompt_setup),
                )
                .unwrap();
            let prompt_ready = wait_for_output_after_until(
                &mut manager,
                "interrupt",
                initial.output_end,
                b"AEGISY_POST_INTERRUPT_READY",
                deadline,
                "CONPTY_INTERRUPT_INITIAL_PROMPT",
                &mut cursor_queries,
            );
            let long_running = match opened.shell_profile.as_str() {
                "cmd-clean-no-autorun" => concat!(
                    "set AEGISY_PING_TARGET=127.0.0.^1\r\n",
                    "echo AEGISY_INTERRUPT_^READY & ping -t %AEGISY_PING_TARGET%\r\n"
                ),
                _ => concat!(
                    "$target='127.0.0.'+'1'; ",
                    "Write-Output ('AEGISY_INTERRUPT_' + 'READY'); ping.exe -t $target\r\n"
                ),
            };
            manager
                .input_user(
                    "interrupt",
                    "session",
                    &BASE64_STANDARD.encode(long_running),
                )
                .unwrap();
            wait_for_output_after_until(
                &mut manager,
                "interrupt",
                prompt_ready.output_end,
                b"AEGISY_INTERRUPT_READY",
                deadline,
                "CONPTY_INTERRUPT_COMMAND_READY",
                &mut cursor_queries,
            );
            wait_for_output_after_until(
                &mut manager,
                "interrupt",
                prompt_ready.output_end,
                b"127.0.0.1",
                deadline,
                "CONPTY_INTERRUPT_PING_RUNNING",
                &mut cursor_queries,
            );

            let before_interrupt =
                snapshot_answering_cursor_queries(&mut manager, "interrupt", &mut cursor_queries);

            manager
                .signal_user("interrupt", "session", "interrupt")
                .unwrap();
            let post_interrupt_prompt = wait_for_output_after_until(
                &mut manager,
                "interrupt",
                before_interrupt.output_end,
                b"AEGISY_POST_INTERRUPT_READY",
                deadline,
                "CONPTY_INTERRUPT_SHELL_RECOVERED",
                &mut cursor_queries,
            );
            let ansi = crate::terminal_test_support::conpty_interrupt_ansi_output(command_prompt);
            manager
                .input_user("interrupt", "session", &BASE64_STANDARD.encode(ansi))
                .unwrap();
            wait_for_ansi_after_until(
                &mut manager,
                "interrupt",
                post_interrupt_prompt.output_end,
                b"AEGISY_ANSI_AFTER_INTERRUPT",
                deadline,
                "CONPTY_INTERRUPT_ANSI_OUTPUT",
                &mut cursor_queries,
            );
            let exit = BASE64_STANDARD.encode("exit 23\r\n");
            manager.input_user("interrupt", "session", &exit).unwrap();
            let snapshot = wait_for_exit_until(
                &mut manager,
                "interrupt",
                deadline,
                "CONPTY_INTERRUPT_EXIT",
                &mut cursor_queries,
            );
            if snapshot.exit_code != Some(23) {
                panic!("CONPTY_INTERRUPT_EXIT_CODE");
            }
            if snapshot.reader_error.is_some() {
                panic!("CONPTY_INTERRUPT_READER_ERROR");
            }
            if !manager
                .terminals
                .get("interrupt")
                .unwrap()
                .job
                .wait_until_empty(5_000)
            {
                panic!("CONPTY_INTERRUPT_JOB_NOT_EMPTY");
            }

            manager.shutdown_all();
            drop(manager);
            fs::remove_dir_all(root).unwrap();
        });
    }

    #[test]
    fn conpty_forced_termination_is_idempotent_after_exit() {
        run_conpty_stage("forced-termination-idempotency", || {
            let (mut manager, root, environment) = manager();
            let opened = manager
                .open_user("forced".into(), open_context(&root, &environment), 24, 80)
                .unwrap();
            assert!(opened.running);

            manager
                .signal_user("forced", "session", "terminate")
                .unwrap();
            let mut cursor_queries =
                crate::terminal_test_support::CursorPositionQueryTracker::default();
            let exited = wait_for_exit(&mut manager, "forced", &mut cursor_queries);
            assert_eq!(exited.exit_code, Some(1));
            assert!(manager
                .terminals
                .get("forced")
                .unwrap()
                .job
                .wait_until_empty(5_000));

            manager
                .signal_user("forced", "session", "terminate")
                .unwrap();
            let closed = manager.close_user("forced", "session").unwrap();
            assert!(!closed.running);
            assert_eq!(closed.exit_code, exited.exit_code);

            manager.shutdown_all();
            drop(manager);
            fs::remove_dir_all(root).unwrap();
        });
    }

    #[test]
    fn conpty_failed_shell_start_does_not_register_terminal() {
        run_conpty_stage("failed-shell-start", || {
            let (mut manager, root, environment) = manager();
            let invalid_shell = root.with_extension("invalid-shell.exe");
            fs::write(&invalid_shell, b"not a Windows executable").unwrap();
            let cause = manager
                .open_user_with_shell(
                    "failed".into(),
                    open_context(&root, &environment),
                    24,
                    80,
                    ShellSpec {
                        path: invalid_shell.clone(),
                        profile: "cmd-clean-no-autorun",
                        kind: ShellKind::CommandPrompt,
                    },
                )
                .unwrap_err();
            assert_eq!(cause.code, -32092);
            assert!(cause.message.contains("cannot start terminal shell"));
            assert!(!manager.terminals.contains_key("failed"));

            drop(manager);
            fs::remove_file(invalid_shell).unwrap();
            fs::remove_dir_all(root).unwrap();
        });
    }

    #[test]
    fn capture_and_validation_are_bounded() {
        let mut capture = Capture::default();
        capture.push(&vec![b'x'; CAPTURE_LIMIT + 17]);
        let (output, start, omitted) = capture.read_after(0);
        assert_eq!(output.len(), CAPTURE_LIMIT);
        assert_eq!(start, 17);
        assert_eq!(omitted, 17);

        let (mut manager, root, environment) = manager();
        assert_eq!(
            manager
                .open_user("invalid".into(), open_context(&root, &environment), 0, 80,)
                .unwrap_err()
                .code,
            -32602
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn environment_is_scrubbed_and_shell_is_outside_project() {
        let (_, root, environment) = manager();
        let shell = discover_shell(&root).unwrap();
        assert!(!path_is_within(&root, &shell.path));
        let mut command = CommandBuilder::new(&shell.path);
        let mut tool_variables = vec![
            ToolVariable::new("TERM", "xterm-256color"),
            ToolVariable::new("COLORTERM", "truecolor"),
        ];
        if let Some(module_path) = powershell_module_path(&root) {
            tool_variables.push(ToolVariable::new("PSModulePath", module_path));
        }
        let process_environment = environment.for_tool("terminal", tool_variables).unwrap();
        configure_environment(&mut command, &process_environment);
        assert!(command.get_env("PATH").is_some());
        assert!(command.get_env("SystemRoot").is_some());
        assert!(command.get_env("OPENAI_API_KEY").is_none());
        assert!(command.get_env("ANTHROPIC_API_KEY").is_none());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn shell_discovery_prefers_powershell_and_falls_back_to_cmd() {
        let (_, root, _) = manager();
        let shell_root = root.with_extension("shell-discovery");
        let program_files = shell_root.join("Program Files");
        let path_bin = shell_root.join("Path Bin");
        let system_root = shell_root.join("Windows");
        let pwsh_program_files = program_files.join("PowerShell/7/pwsh.exe");
        let pwsh_path = path_bin.join("pwsh.exe");
        let windows_powershell = system_root.join("System32/WindowsPowerShell/v1.0/powershell.exe");
        let cmd = shell_root.join("cmd.exe");
        for executable in [&pwsh_program_files, &pwsh_path, &windows_powershell, &cmd] {
            fs::create_dir_all(executable.parent().unwrap()).unwrap();
            fs::write(executable, b"fixture").unwrap();
        }

        let mut variables = HashMap::<String, OsString>::new();
        variables.insert("ProgramFiles".into(), program_files.into_os_string());
        variables.insert(
            "PATH".into(),
            std::env::join_paths([path_bin.as_path()]).unwrap(),
        );
        variables.insert("SystemRoot".into(), system_root.into_os_string());
        variables.insert("ComSpec".into(), cmd.clone().into_os_string());

        let plain_canonical = |path: &Path| crate::plain_path(&path.canonicalize().unwrap());
        let core = discover_shell_with(&root, &|name| variables.get(name).cloned()).unwrap();
        assert_eq!(core.kind, ShellKind::PowerShellCore);
        assert_eq!(core.profile, "pwsh-clean-no-profile");
        assert_eq!(core.path, plain_canonical(&pwsh_program_files));

        fs::remove_file(&pwsh_program_files).unwrap();
        let path_core = discover_shell_with(&root, &|name| variables.get(name).cloned()).unwrap();
        assert_eq!(path_core.kind, ShellKind::PowerShellCore);
        assert_eq!(path_core.path, plain_canonical(&pwsh_path));

        fs::remove_file(&pwsh_path).unwrap();
        let windows = discover_shell_with(&root, &|name| variables.get(name).cloned()).unwrap();
        assert_eq!(windows.kind, ShellKind::WindowsPowerShell);
        assert_eq!(windows.profile, "windows-powershell-clean-no-profile");
        assert_eq!(windows.path, plain_canonical(&windows_powershell));

        fs::remove_file(&windows_powershell).unwrap();
        let command = discover_shell_with(&root, &|name| variables.get(name).cloned()).unwrap();
        assert_eq!(command.kind, ShellKind::CommandPrompt);
        assert_eq!(command.profile, "cmd-clean-no-autorun");
        assert_eq!(command.path, plain_canonical(&cmd));

        fs::remove_dir_all(shell_root).unwrap();
        fs::remove_dir_all(root).unwrap();
    }
}
