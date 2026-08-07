//! Owner-only Windows named-pipe transport primitive.
//!
//! This transport proves that the connected peer is the sidecar's current
//! supervising process using a token-user DACL plus PID/creation-time process
//! identity. It deliberately does not provide the one-time bootstrap
//! authentication owned by OpenSpec task 4.4.

use std::io::{self, Read, Write};
use std::os::windows::io::{AsRawHandle, FromRawHandle, OwnedHandle, RawHandle};
use std::ptr;
use std::time::{Duration, Instant};

use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, LocalFree, ERROR_INSUFFICIENT_BUFFER, ERROR_IO_PENDING,
    ERROR_PIPE_CONNECTED, FILETIME, HANDLE, INVALID_HANDLE_VALUE, WAIT_OBJECT_0, WAIT_TIMEOUT,
};
use windows_sys::Win32::Security::Authorization::{
    ConvertSidToStringSidW, ConvertStringSecurityDescriptorToSecurityDescriptorW, SDDL_REVISION_1,
};
use windows_sys::Win32::Security::{
    GetTokenInformation, TokenUser, SECURITY_ATTRIBUTES, TOKEN_QUERY, TOKEN_USER,
};
use windows_sys::Win32::Storage::FileSystem::{
    FILE_FLAG_FIRST_PIPE_INSTANCE, FILE_FLAG_OVERLAPPED, PIPE_ACCESS_DUPLEX,
};
use windows_sys::Win32::System::Diagnostics::ToolHelp::{
    CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W, TH32CS_SNAPPROCESS,
};
use windows_sys::Win32::System::Pipes::{
    ConnectNamedPipe, CreateNamedPipeW, DisconnectNamedPipe, GetNamedPipeClientProcessId,
    PIPE_READMODE_BYTE, PIPE_REJECT_REMOTE_CLIENTS, PIPE_TYPE_BYTE, PIPE_WAIT,
};
use windows_sys::Win32::System::Threading::{
    CreateEventW, GetCurrentProcess, GetCurrentProcessId, GetProcessTimes, OpenProcess,
    OpenProcessToken, WaitForSingleObject, PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_SYNCHRONIZE,
};
use windows_sys::Win32::System::IO::{CancelIoEx, GetOverlappedResult, OVERLAPPED};

const ACCEPT_TIMEOUT: Duration = Duration::from_secs(30);
const ACCEPT_POLL_MS: u32 = 250;
const WRITE_TIMEOUT_MS: u32 = 30_000;
const PIPE_BUFFER_BYTES: u32 = 64 * 1024;
const MAX_PIPE_NAME_CHARS: usize = 256;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WindowsNamedPipeError {
    InvalidName,
    SecurityDescriptor,
    CreateFailed,
    ParentQueryFailed,
    ParentChanged,
    AcceptFailed,
    AcceptTimeout,
    PeerQueryFailed,
    PeerMismatch,
}

/// Stable identity for one Windows process object. Numeric PIDs are reusable,
/// so the creation time is retained alongside the PID for generation checks.
#[derive(Debug)]
pub struct ProcessIdentity {
    pub pid: u32,
    creation_time_100ns: u64,
    process: OwnedHandle,
}

impl PartialEq for ProcessIdentity {
    fn eq(&self, other: &Self) -> bool {
        self.pid == other.pid && self.creation_time_100ns == other.creation_time_100ns
    }
}

impl Eq for ProcessIdentity {}

impl ProcessIdentity {
    /// A retained process handle makes the expected generation observable even
    /// if its numeric PID is later reused by a different process.
    fn is_alive(&self) -> bool {
        unsafe { WaitForSingleObject(self.process.as_raw_handle() as _, 0) == WAIT_TIMEOUT }
    }

    fn try_clone(&self) -> io::Result<Self> {
        Ok(Self {
            pid: self.pid,
            creation_time_100ns: self.creation_time_100ns,
            process: self.process.try_clone()?,
        })
    }
}

impl WindowsNamedPipeError {
    pub const fn code(self) -> &'static str {
        match self {
            Self::InvalidName => "windows-named-pipe-invalid-name",
            Self::SecurityDescriptor => "windows-named-pipe-security-descriptor-failed",
            Self::CreateFailed => "windows-named-pipe-create-failed",
            Self::ParentQueryFailed => "windows-named-pipe-parent-query-failed",
            Self::ParentChanged => "windows-named-pipe-parent-changed",
            Self::AcceptFailed => "windows-named-pipe-accept-failed",
            Self::AcceptTimeout => "windows-named-pipe-accept-timeout",
            Self::PeerQueryFailed => "windows-named-pipe-peer-query-failed",
            Self::PeerMismatch => "windows-named-pipe-peer-mismatch",
        }
    }
}

impl std::fmt::Display for WindowsNamedPipeError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(self.code())
    }
}

impl std::error::Error for WindowsNamedPipeError {}

fn filetime_value(time: FILETIME) -> u64 {
    (u64::from(time.dwHighDateTime) << 32) | u64::from(time.dwLowDateTime)
}

fn query_process_identity(
    pid: u32,
    failure: WindowsNamedPipeError,
) -> Result<ProcessIdentity, WindowsNamedPipeError> {
    let process = unsafe {
        OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SYNCHRONIZE,
            0,
            pid,
        )
    };
    if process.is_null() {
        return Err(failure);
    }
    let process = unsafe { OwnedHandle::from_raw_handle(process as RawHandle) };
    let mut creation = FILETIME::default();
    let mut exit = FILETIME::default();
    let mut kernel = FILETIME::default();
    let mut user = FILETIME::default();
    if unsafe {
        GetProcessTimes(
            process.as_raw_handle() as _,
            &mut creation,
            &mut exit,
            &mut kernel,
            &mut user,
        )
    } != 0
    {
        Ok(ProcessIdentity {
            pid,
            creation_time_100ns: filetime_value(creation),
            process,
        })
    } else {
        Err(failure)
    }
}

/// Resolve and verify the process generation behind an already connected pipe.
///
/// Keeping the query injectable makes PID-reuse rejection deterministic in the
/// Windows unit suite. Production supplies `query_process_identity`, and no
/// `VerifiedNamedPipe` can be constructed until this boundary returns success.
fn verify_connected_peer_with<F>(
    expected_parent: &ProcessIdentity,
    client_pid: u32,
    query_identity: F,
) -> Result<ProcessIdentity, WindowsNamedPipeError>
where
    F: FnOnce(u32) -> Result<ProcessIdentity, WindowsNamedPipeError>,
{
    let client = query_identity(client_pid)?;
    if !client.is_alive() || client != *expected_parent {
        return Err(WindowsNamedPipeError::PeerMismatch);
    }
    Ok(client)
}

/// Return the current parent process identity from a point-in-time ToolHelp
/// snapshot plus its kernel creation time. A failed or inconsistent snapshot
/// is never treated as a valid supervising generation.
pub fn current_parent_process_identity() -> Result<ProcessIdentity, WindowsNamedPipeError> {
    let current = unsafe { GetCurrentProcessId() };
    let snapshot = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
    if snapshot == INVALID_HANDLE_VALUE || snapshot.is_null() {
        return Err(WindowsNamedPipeError::ParentQueryFailed);
    }
    let result = (|| {
        let mut entry = PROCESSENTRY32W {
            dwSize: std::mem::size_of::<PROCESSENTRY32W>() as u32,
            ..PROCESSENTRY32W::default()
        };
        let mut found = unsafe { Process32FirstW(snapshot, &mut entry) } != 0;
        while found {
            if entry.th32ProcessID == current {
                return if entry.th32ParentProcessID == 0 {
                    Err(WindowsNamedPipeError::ParentQueryFailed)
                } else {
                    query_process_identity(
                        entry.th32ParentProcessID,
                        WindowsNamedPipeError::ParentQueryFailed,
                    )
                };
            }
            found = unsafe { Process32NextW(snapshot, &mut entry) } != 0;
        }
        Err(WindowsNamedPipeError::ParentQueryFailed)
    })();
    unsafe {
        CloseHandle(snapshot);
    }
    result
}

pub fn current_parent_process_id() -> Result<u32, WindowsNamedPipeError> {
    Ok(current_parent_process_identity()?.pid)
}

fn validate_name(name: &str) -> Result<Vec<u16>, WindowsNamedPipeError> {
    let canonical = if name.starts_with(r"\\.\pipe\") {
        name.to_owned()
    } else if name.starts_with("aegisy-agent-") {
        format!(r"\\.\pipe\{name}")
    } else {
        return Err(WindowsNamedPipeError::InvalidName);
    };
    let suffix = canonical.strip_prefix(r"\\.\pipe\").unwrap_or_default();
    let wide: Vec<u16> = canonical.encode_utf16().collect();
    if suffix.is_empty()
        || wide.len() > MAX_PIPE_NAME_CHARS
        || suffix
            .chars()
            .any(|character| character.is_control() || matches!(character, '\\' | '/'))
    {
        return Err(WindowsNamedPipeError::InvalidName);
    }
    let mut wide = wide;
    wide.push(0);
    Ok(wide)
}

fn current_token_user_sddl() -> Result<Vec<u16>, WindowsNamedPipeError> {
    let mut token = ptr::null_mut();
    if unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) } == 0
        || token.is_null()
    {
        return Err(WindowsNamedPipeError::SecurityDescriptor);
    }
    let token = unsafe { OwnedHandle::from_raw_handle(token as RawHandle) };

    let mut required = 0u32;
    let first = unsafe {
        GetTokenInformation(
            token.as_raw_handle() as _,
            TokenUser,
            ptr::null_mut(),
            0,
            &mut required,
        )
    };
    if first != 0 || unsafe { GetLastError() } != ERROR_INSUFFICIENT_BUFFER || required == 0 {
        return Err(WindowsNamedPipeError::SecurityDescriptor);
    }

    // A usize-aligned buffer is required before viewing the returned
    // TOKEN_USER. The OS supplies the exact byte size through `required`.
    let words = (required as usize).div_ceil(std::mem::size_of::<usize>());
    let mut buffer = vec![0usize; words];
    if unsafe {
        GetTokenInformation(
            token.as_raw_handle() as _,
            TokenUser,
            buffer.as_mut_ptr() as _,
            required,
            &mut required,
        )
    } == 0
    {
        return Err(WindowsNamedPipeError::SecurityDescriptor);
    }
    let token_user = unsafe { &*(buffer.as_ptr() as *const TOKEN_USER) };
    if token_user.User.Sid.is_null() {
        return Err(WindowsNamedPipeError::SecurityDescriptor);
    }

    let mut sid_string = ptr::null_mut();
    if unsafe { ConvertSidToStringSidW(token_user.User.Sid, &mut sid_string) } == 0
        || sid_string.is_null()
    {
        return Err(WindowsNamedPipeError::SecurityDescriptor);
    }
    let sid = unsafe {
        let mut length = 0usize;
        while length < 184 && *sid_string.add(length) != 0 {
            length += 1;
        }
        if length == 184 {
            LocalFree(sid_string as _);
            return Err(WindowsNamedPipeError::SecurityDescriptor);
        }
        let result = std::slice::from_raw_parts(sid_string, length).to_vec();
        LocalFree(sid_string as _);
        result
    };

    let mut sddl = "D:P(A;;GA;;;".encode_utf16().collect::<Vec<_>>();
    sddl.extend(sid);
    sddl.extend(")".encode_utf16());
    sddl.push(0);
    Ok(sddl)
}

fn owner_only_security_attributes(
) -> Result<(*mut core::ffi::c_void, SECURITY_ATTRIBUTES), WindowsNamedPipeError> {
    let sddl = current_token_user_sddl()?;
    let mut descriptor = ptr::null_mut();
    let converted = unsafe {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.as_ptr(),
            SDDL_REVISION_1,
            &mut descriptor,
            ptr::null_mut(),
        )
    };
    if converted == 0 || descriptor.is_null() {
        return Err(WindowsNamedPipeError::SecurityDescriptor);
    }
    Ok((
        descriptor,
        SECURITY_ATTRIBUTES {
            nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: descriptor,
            bInheritHandle: 0,
        },
    ))
}

fn checked_pipe_handle(handle: RawHandle) -> Result<OwnedHandle, WindowsNamedPipeError> {
    if handle.is_null() || handle == INVALID_HANDLE_VALUE {
        Err(WindowsNamedPipeError::CreateFailed)
    } else {
        Ok(unsafe { OwnedHandle::from_raw_handle(handle) })
    }
}

#[derive(Debug)]
pub struct OwnerOnlyNamedPipeListener {
    name: String,
    pipe: Option<OwnedHandle>,
    expected_parent: ProcessIdentity,
}

impl OwnerOnlyNamedPipeListener {
    pub fn bind_fresh(name: &str, expected_parent_pid: u32) -> Result<Self, WindowsNamedPipeError> {
        let expected_parent = query_process_identity(
            expected_parent_pid,
            WindowsNamedPipeError::ParentQueryFailed,
        )?;
        Self::bind_fresh_with_identity(name, expected_parent)
    }

    pub fn bind_fresh_with_identity(
        name: &str,
        expected_parent: ProcessIdentity,
    ) -> Result<Self, WindowsNamedPipeError> {
        let wide_name = validate_name(name)?;
        let (descriptor, attributes) = owner_only_security_attributes()?;
        let handle = unsafe {
            CreateNamedPipeW(
                wide_name.as_ptr(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                1,
                PIPE_BUFFER_BYTES,
                PIPE_BUFFER_BYTES,
                0,
                &attributes,
            )
        };
        unsafe {
            LocalFree(descriptor as _);
        }
        let pipe = checked_pipe_handle(handle as RawHandle)?;
        Ok(Self {
            name: name.to_owned(),
            pipe: Some(pipe),
            expected_parent,
        })
    }

    pub fn pipe_name(&self) -> &str {
        &self.name
    }

    fn verify_parent(&self) -> Result<(), WindowsNamedPipeError> {
        if !self.expected_parent.is_alive() {
            return Err(WindowsNamedPipeError::ParentChanged);
        }
        let current =
            current_parent_process_identity().map_err(|_| WindowsNamedPipeError::ParentChanged)?;
        if current != self.expected_parent || !current.is_alive() {
            return Err(WindowsNamedPipeError::ParentChanged);
        }
        Ok(())
    }

    pub fn accept_one(mut self) -> Result<VerifiedNamedPipe, WindowsNamedPipeError> {
        let deadline = Instant::now() + ACCEPT_TIMEOUT;
        let pipe = self
            .pipe
            .take()
            .ok_or(WindowsNamedPipeError::AcceptFailed)?;
        let event = new_io_event().map_err(|_| WindowsNamedPipeError::AcceptFailed)?;
        let raw = pipe.as_raw_handle() as _;
        let mut overlapped = OVERLAPPED::default();
        overlapped.hEvent = event.as_raw_handle() as _;
        // Overlapped connect: synchronous pipe I/O on one pipe object is
        // serialized by the driver, which can deadlock the response writer
        // behind the pending reader, so every operation uses an event.
        let mut pending = false;
        loop {
            if let Err(error) = self.verify_parent() {
                if pending {
                    cancel_overlapped(raw, &overlapped);
                }
                return Err(error);
            }
            if !pending {
                let connect_result = unsafe { ConnectNamedPipe(raw, &mut overlapped) };
                if connect_result != 0 {
                    break;
                }
                let error = unsafe { GetLastError() };
                if error == ERROR_PIPE_CONNECTED {
                    break;
                }
                if error != ERROR_IO_PENDING {
                    return Err(WindowsNamedPipeError::AcceptFailed);
                }
                pending = true;
            }
            let waited = unsafe { WaitForSingleObject(overlapped.hEvent, ACCEPT_POLL_MS) };
            if waited == WAIT_OBJECT_0 {
                let mut ignored = 0_u32;
                if unsafe { GetOverlappedResult(raw, &overlapped, &mut ignored, 0) } == 0 {
                    return Err(WindowsNamedPipeError::AcceptFailed);
                }
                break;
            }
            if waited != WAIT_TIMEOUT {
                cancel_overlapped(raw, &overlapped);
                return Err(WindowsNamedPipeError::AcceptFailed);
            }
            if Instant::now() >= deadline {
                cancel_overlapped(raw, &overlapped);
                return Err(WindowsNamedPipeError::AcceptTimeout);
            }
        }
        self.verify_parent()?;
        let mut client_pid = 0;
        if unsafe { GetNamedPipeClientProcessId(raw, &mut client_pid) } == 0 {
            return Err(WindowsNamedPipeError::PeerQueryFailed);
        }
        let client = verify_connected_peer_with(&self.expected_parent, client_pid, |pid| {
            query_process_identity(pid, WindowsNamedPipeError::PeerQueryFailed)
        })?;
        Ok(VerifiedNamedPipe {
            pipe,
            io_event: event,
            peer: client,
        })
    }
}

impl Drop for OwnerOnlyNamedPipeListener {
    fn drop(&mut self) {
        if let Some(pipe) = self.pipe.take() {
            unsafe {
                DisconnectNamedPipe(pipe.as_raw_handle() as _);
            }
            drop(pipe);
        }
    }
}

#[derive(Debug)]
pub struct VerifiedNamedPipe {
    pipe: OwnedHandle,
    io_event: OwnedHandle,
    peer: ProcessIdentity,
}

fn new_io_event() -> io::Result<OwnedHandle> {
    // Auto-reset event: each instance has at most one outstanding overlapped
    // operation, so the event returns to unsignaled after every wait.
    let event = unsafe { CreateEventW(ptr::null(), 0, 0, ptr::null()) };
    if event.is_null() {
        Err(io::Error::last_os_error())
    } else {
        Ok(unsafe { OwnedHandle::from_raw_handle(event as RawHandle) })
    }
}

fn cancel_overlapped(pipe: HANDLE, overlapped: &OVERLAPPED) {
    unsafe {
        CancelIoEx(pipe, overlapped);
        let mut ignored = 0_u32;
        GetOverlappedResult(pipe, overlapped, &mut ignored, 1);
    }
}

impl VerifiedNamedPipe {
    /// Duplicate the native pipe handle so the protocol reader and writer can
    /// have independent ownership while retaining the same peer proof.
    pub fn try_clone(&self) -> io::Result<Self> {
        Ok(Self {
            pipe: self.pipe.try_clone()?,
            io_event: new_io_event()?,
            peer: self.peer.try_clone()?,
        })
    }
}

impl Read for VerifiedNamedPipe {
    fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
        let mut overlapped = OVERLAPPED::default();
        overlapped.hEvent = self.io_event.as_raw_handle() as _;
        let mut read = 0_u32;
        let result = unsafe {
            windows_sys::Win32::Storage::FileSystem::ReadFile(
                self.pipe.as_raw_handle() as _,
                buffer.as_mut_ptr() as _,
                u32::try_from(buffer.len()).map_err(|_| io::Error::from_raw_os_error(87))?,
                &mut read,
                &mut overlapped,
            )
        };
        if result != 0 {
            return Ok(read as usize);
        }
        let error = unsafe { GetLastError() };
        if error != ERROR_IO_PENDING {
            return Err(io::Error::from_raw_os_error(error as i32));
        }
        // Blocking read semantics: the protocol reader waits indefinitely.
        let waited = unsafe { WaitForSingleObject(overlapped.hEvent, u32::MAX) };
        if waited != WAIT_OBJECT_0 {
            return Err(io::Error::last_os_error());
        }
        let completed = unsafe {
            GetOverlappedResult(self.pipe.as_raw_handle() as _, &overlapped, &mut read, 0)
        };
        if completed == 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(read as usize)
        }
    }
}

impl Write for VerifiedNamedPipe {
    fn write(&mut self, buffer: &[u8]) -> io::Result<usize> {
        let mut overlapped = OVERLAPPED::default();
        overlapped.hEvent = self.io_event.as_raw_handle() as _;
        let mut written = 0_u32;
        let result = unsafe {
            windows_sys::Win32::Storage::FileSystem::WriteFile(
                self.pipe.as_raw_handle() as _,
                buffer.as_ptr() as _,
                u32::try_from(buffer.len()).map_err(|_| io::Error::from_raw_os_error(87))?,
                &mut written,
                &mut overlapped,
            )
        };
        if result != 0 {
            return Ok(written as usize);
        }
        let error = unsafe { GetLastError() };
        if error != ERROR_IO_PENDING {
            return Err(io::Error::from_raw_os_error(error as i32));
        }
        // A wedged peer must not hang the dispatcher forever.
        let waited = unsafe { WaitForSingleObject(overlapped.hEvent, WRITE_TIMEOUT_MS) };
        if waited != WAIT_OBJECT_0 {
            cancel_overlapped(self.pipe.as_raw_handle() as _, &overlapped);
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "named pipe write timed out",
            ));
        }
        let completed = unsafe {
            GetOverlappedResult(self.pipe.as_raw_handle() as _, &overlapped, &mut written, 0)
        };
        if completed == 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(written as usize)
        }
    }

    fn flush(&mut self) -> io::Result<()> {
        // Named pipes have no server-side userspace buffer: WriteFile already
        // hands the bytes to the kernel pipe, and FlushFileBuffers on a
        // blocking pipe would wait until the client has read everything,
        // deadlocking the dispatcher whenever the client is slow to read.
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::{
        current_token_user_sddl, query_process_identity, validate_name, verify_connected_peer_with,
        WindowsNamedPipeError,
    };
    use windows_sys::Win32::System::Threading::GetCurrentProcessId;

    #[test]
    fn validates_pipe_namespace_and_bounds() {
        assert!(validate_name(r"\\.\pipe\aegisy-test").is_ok());
        assert!(validate_name("aegisy-agent-test").is_ok());
        assert_eq!(
            validate_name("aegisy-test"),
            Err(WindowsNamedPipeError::InvalidName)
        );
        assert!(validate_name(&format!(r"\\.\pipe\{}", "x".repeat(247))).is_ok());
        assert_eq!(
            validate_name(&format!(r"\\.\pipe\{}", "x".repeat(248))),
            Err(WindowsNamedPipeError::InvalidName)
        );
        assert_eq!(
            validate_name(&format!(r"\\.\pipe\{}", "x".repeat(256))),
            Err(WindowsNamedPipeError::InvalidName)
        );
    }

    #[test]
    fn counts_unicode_utf16_units_in_the_canonical_name() {
        let max = format!(r"\\.\pipe\{}", "\u{1f642}".repeat(123));
        assert_eq!(max.encode_utf16().count(), 255);
        assert!(validate_name(&max).is_ok());

        let exact = format!(r"\\.\pipe\{}x", "\u{1f642}".repeat(123));
        assert_eq!(exact.encode_utf16().count(), 256);
        assert!(validate_name(&exact).is_ok());

        let over = format!(r"\\.\pipe\{}", "\u{1f642}".repeat(124));
        assert_eq!(over.encode_utf16().count(), 257);
        assert_eq!(
            validate_name(&over),
            Err(WindowsNamedPipeError::InvalidName)
        );
    }

    #[test]
    fn security_descriptor_names_the_current_token_user_sid() {
        let mut sddl = current_token_user_sddl().expect("current token user SDDL");
        assert_eq!(sddl.pop(), Some(0));
        let sddl = String::from_utf16(&sddl).expect("UTF-16 SDDL");
        assert!(sddl.starts_with("D:P(A;;GA;;;S-1-"));
        assert!(sddl.ends_with(')'));
        assert!(!sddl.contains(";;;OW"));
    }

    #[test]
    fn process_identity_retains_a_live_generation_handle() {
        let pid = unsafe { GetCurrentProcessId() };
        let identity = query_process_identity(pid, WindowsNamedPipeError::ParentQueryFailed)
            .expect("current process identity");
        let clone = identity.try_clone().expect("duplicate identity handle");
        assert_eq!(identity.pid, pid);
        assert!(identity.is_alive());
        assert!(clone.is_alive());
        assert_eq!(identity, clone);
    }

    #[test]
    fn peer_admission_rejects_same_pid_with_different_creation_time() {
        let pid = unsafe { GetCurrentProcessId() };
        let expected = query_process_identity(pid, WindowsNamedPipeError::ParentQueryFailed)
            .expect("current process identity");
        let mut reused_pid = expected.try_clone().expect("duplicate identity handle");
        reused_pid.creation_time_100ns ^= 1;
        assert_eq!(reused_pid.pid, expected.pid);
        assert!(reused_pid.is_alive());
        assert_ne!(reused_pid.creation_time_100ns, expected.creation_time_100ns);

        let error = verify_connected_peer_with(&expected, pid, |queried_pid| {
            assert_eq!(queried_pid, pid);
            Ok(reused_pid)
        })
        .expect_err("a reused PID must not cross the verified-pipe admission boundary");

        assert_eq!(error, WindowsNamedPipeError::PeerMismatch);
    }
}
