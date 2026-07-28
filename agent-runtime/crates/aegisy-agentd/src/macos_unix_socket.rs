//! Owner-only macOS Unix-domain socket transport primitive.
//!
//! This module proves local peer identity but deliberately does not provide the
//! one-time bootstrap authentication owned by OpenSpec task 4.4.

use std::ffi::{CStr, CString, OsStr};
use std::fs;
#[cfg(test)]
use std::fs::Metadata;
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{FileTypeExt, MetadataExt};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::Component;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

const SOCKET_FILE_NAME: &str = "agent.sock";
const PRIVATE_DIRECTORY_MODE: libc::mode_t = 0o700;
const PRIVATE_SOCKET_MODE: libc::mode_t = 0o600;
const ACCEPT_TIMEOUT: Duration = Duration::from_secs(30);
const ACCEPT_POLL_MILLISECONDS: libc::c_int = 100;
const ACL_TYPE_EXTENDED: libc::c_int = 0x0000_0100;

type Acl = *mut libc::c_void;

extern "C" {
    fn acl_get_fd_np(descriptor: libc::c_int, acl_type: libc::c_int) -> Acl;
    fn acl_get_link_np(path: *const libc::c_char, acl_type: libc::c_int) -> Acl;
    fn acl_free(object: *mut libc::c_void) -> libc::c_int;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnixSocketError {
    InvalidPath,
    UnsafeParent,
    EndpointExists,
    EndpointCreateFailed,
    EndpointInvalid,
    AcceptFailed,
    AcceptTimeout,
    ParentChanged,
    PeerQueryFailed,
    PeerMismatch,
    CleanupMismatch,
}

impl UnixSocketError {
    pub const fn code(self) -> &'static str {
        match self {
            Self::InvalidPath => "unix-socket-invalid-path",
            Self::UnsafeParent => "unix-socket-unsafe-parent",
            Self::EndpointExists => "unix-socket-endpoint-exists",
            Self::EndpointCreateFailed => "unix-socket-endpoint-create-failed",
            Self::EndpointInvalid => "unix-socket-endpoint-invalid",
            Self::AcceptFailed => "unix-socket-accept-failed",
            Self::AcceptTimeout => "unix-socket-accept-timeout",
            Self::ParentChanged => "unix-socket-parent-changed",
            Self::PeerQueryFailed => "unix-socket-peer-query-failed",
            Self::PeerMismatch => "unix-socket-peer-mismatch",
            Self::CleanupMismatch => "unix-socket-cleanup-mismatch",
        }
    }
}

impl std::fmt::Display for UnixSocketError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(self.code())
    }
}

impl std::error::Error for UnixSocketError {}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct ObjectIdentity {
    device: u64,
    inode: u64,
    uid: u32,
}

impl ObjectIdentity {
    fn from_stat(status: &libc::stat) -> Self {
        Self {
            device: status.st_dev as u64,
            inode: status.st_ino,
            uid: status.st_uid,
        }
    }

    fn matches_stat(self, status: &libc::stat) -> bool {
        self.device == status.st_dev as u64
            && self.inode == status.st_ino
            && self.uid == status.st_uid
    }
}

#[derive(Debug)]
struct EndpointCleanup {
    parent_fd: OwnedFd,
    directory_fd: OwnedFd,
    directory_name: CString,
    directory_identity: ObjectIdentity,
    socket: PathBuf,
    socket_identity: Option<ObjectIdentity>,
    socket_unlinked: bool,
}

impl EndpointCleanup {
    fn directory_entry_matches(&self) -> bool {
        stat_at(self.parent_fd.as_raw_fd(), &self.directory_name).is_ok_and(|status| {
            (status.st_mode & libc::S_IFMT) == libc::S_IFDIR
                && self.directory_identity.matches_stat(&status)
                && permission_bits_from_mode(status.st_mode) == PRIVATE_DIRECTORY_MODE
        })
    }

    fn unlink_socket(&mut self) -> Result<(), UnixSocketError> {
        self.unlink_socket_with_hook(|| {})
    }

    fn unlink_socket_with_hook<F>(&mut self, before_quarantine: F) -> Result<(), UnixSocketError>
    where
        F: FnOnce(),
    {
        if self.socket_unlinked {
            return Ok(());
        }
        let expected = self
            .socket_identity
            .ok_or(UnixSocketError::CleanupMismatch)?;
        let socket_name = socket_file_name();
        let status = stat_at(self.directory_fd.as_raw_fd(), socket_name)
            .map_err(|_| UnixSocketError::CleanupMismatch)?;
        if (status.st_mode & libc::S_IFMT) != libc::S_IFSOCK
            || !expected.matches_stat(&status)
            || permission_bits_from_mode(status.st_mode) != PRIVATE_SOCKET_MODE
        {
            return Err(UnixSocketError::CleanupMismatch);
        }
        before_quarantine();
        let quarantine =
            rename_to_quarantine(self.directory_fd.as_raw_fd(), socket_name, "socket")?;
        let quarantined = stat_at(self.directory_fd.as_raw_fd(), &quarantine)
            .map_err(|_| UnixSocketError::CleanupMismatch)?;
        if !expected.matches_stat(&quarantined)
            || (quarantined.st_mode & libc::S_IFMT) != libc::S_IFSOCK
        {
            return Err(UnixSocketError::CleanupMismatch);
        }
        if unsafe { libc::unlinkat(self.directory_fd.as_raw_fd(), quarantine.as_ptr(), 0) } != 0 {
            return Err(UnixSocketError::CleanupMismatch);
        }
        self.socket_unlinked = true;
        Ok(())
    }

    fn remove_directory(&self) -> Result<(), UnixSocketError> {
        self.remove_directory_with_hook(|| {})
    }

    fn remove_directory_with_hook<F>(&self, before_quarantine: F) -> Result<(), UnixSocketError>
    where
        F: FnOnce(),
    {
        if !self.directory_entry_matches() {
            return Err(UnixSocketError::CleanupMismatch);
        }
        before_quarantine();
        let quarantine = rename_to_quarantine(
            self.parent_fd.as_raw_fd(),
            &self.directory_name,
            "directory",
        )?;
        let quarantined = stat_at(self.parent_fd.as_raw_fd(), &quarantine)
            .map_err(|_| UnixSocketError::CleanupMismatch)?;
        if !self.directory_identity.matches_stat(&quarantined)
            || (quarantined.st_mode & libc::S_IFMT) != libc::S_IFDIR
        {
            return Err(UnixSocketError::CleanupMismatch);
        }
        if unsafe {
            libc::unlinkat(
                self.parent_fd.as_raw_fd(),
                quarantine.as_ptr(),
                libc::AT_REMOVEDIR,
            )
        } != 0
        {
            return Err(UnixSocketError::CleanupMismatch);
        }
        Ok(())
    }

    fn best_effort(&mut self) {
        if self.unlink_socket().is_ok() {
            let _ = self.remove_directory();
        }
    }
}

#[derive(Debug)]
pub struct OwnerOnlyUnixListener {
    listener: UnixListener,
    expected_client_pid: libc::pid_t,
    cleanup: Option<EndpointCleanup>,
}

impl OwnerOnlyUnixListener {
    pub fn bind_fresh(
        directory: &Path,
        expected_client_pid: libc::pid_t,
    ) -> Result<Self, UnixSocketError> {
        validate_directory_path(directory, expected_client_pid)?;
        let parent = directory.parent().ok_or(UnixSocketError::InvalidPath)?;
        let parent_fd = open_directory(parent).map_err(|_| UnixSocketError::UnsafeParent)?;
        validate_private_directory_fd(parent_fd.as_raw_fd(), UnixSocketError::UnsafeParent)?;
        let directory_name =
            cstring_from_os(directory.file_name().ok_or(UnixSocketError::InvalidPath)?)?;
        if unsafe {
            libc::mkdirat(
                parent_fd.as_raw_fd(),
                directory_name.as_ptr(),
                PRIVATE_DIRECTORY_MODE,
            )
        } != 0
        {
            return Err(
                if io::Error::last_os_error().kind() == io::ErrorKind::AlreadyExists {
                    UnixSocketError::EndpointExists
                } else {
                    UnixSocketError::EndpointCreateFailed
                },
            );
        }
        let directory_fd = open_directory_at(parent_fd.as_raw_fd(), &directory_name)
            .map_err(|_| UnixSocketError::EndpointCreateFailed)?;
        if unsafe { libc::fchmod(directory_fd.as_raw_fd(), PRIVATE_DIRECTORY_MODE) } != 0 {
            return Err(UnixSocketError::EndpointCreateFailed);
        }
        let directory_status = validate_private_directory_fd(
            directory_fd.as_raw_fd(),
            UnixSocketError::EndpointInvalid,
        )?;
        let socket = directory.join(SOCKET_FILE_NAME);
        validate_socket_path_length(&socket)?;
        let mut cleanup = EndpointCleanup {
            parent_fd,
            directory_fd,
            directory_name,
            directory_identity: ObjectIdentity::from_stat(&directory_status),
            socket,
            socket_identity: None,
            socket_unlinked: false,
        };
        if !cleanup.directory_entry_matches() {
            return Err(UnixSocketError::EndpointInvalid);
        }
        let anchored_directory = descriptor_path(cleanup.directory_fd.as_raw_fd())?;
        let anchored_socket = anchored_directory.join(SOCKET_FILE_NAME);
        validate_socket_path_length(&anchored_socket)?;
        let listener = UnixListener::bind(&anchored_socket)
            .map_err(|_| UnixSocketError::EndpointCreateFailed)?;
        let initial_socket = stat_at(cleanup.directory_fd.as_raw_fd(), socket_file_name())
            .map_err(|_| UnixSocketError::EndpointInvalid)?;
        cleanup.socket_identity = Some(ObjectIdentity::from_stat(&initial_socket));
        if unsafe {
            libc::fchmodat(
                cleanup.directory_fd.as_raw_fd(),
                socket_file_name().as_ptr(),
                PRIVATE_SOCKET_MODE,
                libc::AT_SYMLINK_NOFOLLOW,
            )
        } != 0
        {
            return Err(UnixSocketError::EndpointCreateFailed);
        }
        validate_owned_socket_at(&cleanup, &anchored_socket)?;
        if !cleanup.directory_entry_matches() {
            return Err(UnixSocketError::EndpointInvalid);
        }
        listener
            .set_nonblocking(true)
            .map_err(|_| UnixSocketError::EndpointCreateFailed)?;
        Ok(Self {
            listener,
            expected_client_pid,
            cleanup: Some(cleanup),
        })
    }

    pub fn socket_path(&self) -> &Path {
        &self
            .cleanup
            .as_ref()
            .expect("listener cleanup exists until accept")
            .socket
    }

    pub fn accept_one(self) -> Result<VerifiedUnixStream, UnixSocketError> {
        self.accept_one_with_parent_probe(ACCEPT_TIMEOUT, || unsafe { libc::getppid() })
    }

    fn accept_one_with_parent_probe<F>(
        mut self,
        timeout: Duration,
        mut current_parent_pid: F,
    ) -> Result<VerifiedUnixStream, UnixSocketError>
    where
        F: FnMut() -> libc::pid_t,
    {
        let deadline = Instant::now() + timeout;
        let stream = loop {
            if current_parent_pid() != self.expected_client_pid {
                return Err(UnixSocketError::ParentChanged);
            }
            if Instant::now() >= deadline {
                return Err(UnixSocketError::AcceptTimeout);
            }
            let mut descriptor = libc::pollfd {
                fd: self.listener.as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            };
            let poll_result = unsafe {
                libc::poll(
                    std::ptr::addr_of_mut!(descriptor),
                    1,
                    ACCEPT_POLL_MILLISECONDS,
                )
            };
            if poll_result < 0 {
                if io::Error::last_os_error().kind() == io::ErrorKind::Interrupted {
                    continue;
                }
                return Err(UnixSocketError::AcceptFailed);
            }
            if poll_result == 0 {
                continue;
            }
            match self.listener.accept() {
                Ok((stream, _)) => break stream,
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => continue,
                Err(_) => return Err(UnixSocketError::AcceptFailed),
            }
        };
        stream
            .set_nonblocking(false)
            .map_err(|_| UnixSocketError::AcceptFailed)?;
        if current_parent_pid() != self.expected_client_pid {
            return Err(UnixSocketError::ParentChanged);
        }
        verify_peer(
            stream.as_raw_fd(),
            unsafe { libc::geteuid() },
            self.expected_client_pid,
        )?;
        if current_parent_pid() != self.expected_client_pid {
            return Err(UnixSocketError::ParentChanged);
        }
        let mut cleanup = self
            .cleanup
            .take()
            .ok_or(UnixSocketError::CleanupMismatch)?;
        cleanup.unlink_socket()?;
        Ok(VerifiedUnixStream {
            stream,
            cleanup: Some(cleanup),
        })
    }

    #[cfg(test)]
    fn accept_one_for_test(self) -> Result<VerifiedUnixStream, UnixSocketError> {
        let expected = self.expected_client_pid;
        self.accept_one_with_parent_probe(Duration::from_secs(2), || expected)
    }
}

impl Drop for OwnerOnlyUnixListener {
    fn drop(&mut self) {
        if let Some(cleanup) = self.cleanup.as_mut() {
            cleanup.best_effort();
        }
    }
}

#[derive(Debug)]
pub struct VerifiedUnixStream {
    stream: UnixStream,
    cleanup: Option<EndpointCleanup>,
}

impl VerifiedUnixStream {
    pub fn try_clone(&self) -> io::Result<UnixStream> {
        self.stream.try_clone()
    }
}

impl Read for VerifiedUnixStream {
    fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
        self.stream.read(buffer)
    }
}

impl Write for VerifiedUnixStream {
    fn write(&mut self, buffer: &[u8]) -> io::Result<usize> {
        self.stream.write(buffer)
    }

    fn flush(&mut self) -> io::Result<()> {
        self.stream.flush()
    }
}

impl AsRawFd for VerifiedUnixStream {
    fn as_raw_fd(&self) -> RawFd {
        self.stream.as_raw_fd()
    }
}

impl Drop for VerifiedUnixStream {
    fn drop(&mut self) {
        if let Some(mut cleanup) = self.cleanup.take() {
            cleanup.best_effort();
        }
    }
}

#[cfg(test)]
fn permission_bits(metadata: &Metadata) -> libc::mode_t {
    (metadata.mode() & 0o777) as libc::mode_t
}

fn permission_bits_from_mode(mode: libc::mode_t) -> libc::mode_t {
    mode & 0o777
}

fn socket_file_name() -> &'static CStr {
    c"agent.sock"
}

fn cstring_from_os(value: &OsStr) -> Result<CString, UnixSocketError> {
    CString::new(value.as_bytes()).map_err(|_| UnixSocketError::InvalidPath)
}

fn open_directory(path: &Path) -> io::Result<OwnedFd> {
    let path = CString::new(path.as_os_str().as_bytes())
        .map_err(|_| io::Error::from(io::ErrorKind::InvalidInput))?;
    let descriptor = unsafe {
        libc::open(
            path.as_ptr(),
            libc::O_RDONLY | libc::O_DIRECTORY | libc::O_CLOEXEC | libc::O_NOFOLLOW,
        )
    };
    if descriptor < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(unsafe { OwnedFd::from_raw_fd(descriptor) })
}

fn open_directory_at(parent: RawFd, name: &CStr) -> io::Result<OwnedFd> {
    let descriptor = unsafe {
        libc::openat(
            parent,
            name.as_ptr(),
            libc::O_RDONLY | libc::O_DIRECTORY | libc::O_CLOEXEC | libc::O_NOFOLLOW,
        )
    };
    if descriptor < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(unsafe { OwnedFd::from_raw_fd(descriptor) })
}

fn descriptor_stat(descriptor: RawFd) -> io::Result<libc::stat> {
    let mut status = unsafe { std::mem::zeroed::<libc::stat>() };
    if unsafe { libc::fstat(descriptor, std::ptr::addr_of_mut!(status)) } != 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(status)
}

fn stat_at(directory: RawFd, name: &CStr) -> io::Result<libc::stat> {
    let mut status = unsafe { std::mem::zeroed::<libc::stat>() };
    if unsafe {
        libc::fstatat(
            directory,
            name.as_ptr(),
            std::ptr::addr_of_mut!(status),
            libc::AT_SYMLINK_NOFOLLOW,
        )
    } != 0
    {
        return Err(io::Error::last_os_error());
    }
    Ok(status)
}

fn acl_is_absent(acl: Acl) -> Result<bool, UnixSocketError> {
    if acl.is_null() {
        return if io::Error::last_os_error().raw_os_error() == Some(libc::ENOENT) {
            Ok(true)
        } else {
            Err(UnixSocketError::EndpointInvalid)
        };
    }
    let free_result = unsafe { acl_free(acl) };
    if free_result != 0 {
        return Err(UnixSocketError::EndpointInvalid);
    }
    Ok(false)
}

fn descriptor_has_no_extended_acl(descriptor: RawFd) -> Result<bool, UnixSocketError> {
    acl_is_absent(unsafe { acl_get_fd_np(descriptor, ACL_TYPE_EXTENDED) })
}

fn path_has_no_extended_acl(path: &Path) -> Result<bool, UnixSocketError> {
    let path =
        CString::new(path.as_os_str().as_bytes()).map_err(|_| UnixSocketError::InvalidPath)?;
    acl_is_absent(unsafe { acl_get_link_np(path.as_ptr(), ACL_TYPE_EXTENDED) })
}

fn validate_private_directory_fd(
    descriptor: RawFd,
    error: UnixSocketError,
) -> Result<libc::stat, UnixSocketError> {
    let status = descriptor_stat(descriptor).map_err(|_| error)?;
    if (status.st_mode & libc::S_IFMT) != libc::S_IFDIR
        || status.st_uid != unsafe { libc::geteuid() }
        || permission_bits_from_mode(status.st_mode) != PRIVATE_DIRECTORY_MODE
        || !descriptor_has_no_extended_acl(descriptor).map_err(|_| error)?
    {
        return Err(error);
    }
    Ok(status)
}

fn descriptor_path(descriptor: RawFd) -> Result<PathBuf, UnixSocketError> {
    let mut buffer = [0 as libc::c_char; 1024];
    if unsafe { libc::fcntl(descriptor, libc::F_GETPATH, buffer.as_mut_ptr()) } < 0 {
        return Err(UnixSocketError::EndpointInvalid);
    }
    let path = unsafe { CStr::from_ptr(buffer.as_ptr()) };
    if path.to_bytes().is_empty() {
        return Err(UnixSocketError::EndpointInvalid);
    }
    Ok(PathBuf::from(OsStr::from_bytes(path.to_bytes())))
}

fn validate_owned_socket_at(
    cleanup: &EndpointCleanup,
    anchored_socket: &Path,
) -> Result<(), UnixSocketError> {
    let expected = cleanup
        .socket_identity
        .ok_or(UnixSocketError::EndpointInvalid)?;
    let before = stat_at(cleanup.directory_fd.as_raw_fd(), socket_file_name())
        .map_err(|_| UnixSocketError::EndpointInvalid)?;
    let path_before =
        fs::symlink_metadata(&cleanup.socket).map_err(|_| UnixSocketError::EndpointInvalid)?;
    if (before.st_mode & libc::S_IFMT) != libc::S_IFSOCK
        || !expected.matches_stat(&before)
        || path_before.dev() != expected.device
        || path_before.ino() != expected.inode
        || path_before.uid() != expected.uid
        || !path_before.file_type().is_socket()
        || before.st_uid != unsafe { libc::geteuid() }
        || permission_bits_from_mode(before.st_mode) != PRIVATE_SOCKET_MODE
        || !path_has_no_extended_acl(anchored_socket)?
    {
        return Err(UnixSocketError::EndpointInvalid);
    }
    let after = stat_at(cleanup.directory_fd.as_raw_fd(), socket_file_name())
        .map_err(|_| UnixSocketError::EndpointInvalid)?;
    if !expected.matches_stat(&after) || before.st_mode != after.st_mode {
        return Err(UnixSocketError::EndpointInvalid);
    }
    Ok(())
}

fn random_quarantine_name(kind: &str) -> Result<CString, UnixSocketError> {
    let mut random = [0_u8; 16];
    if unsafe { libc::getentropy(random.as_mut_ptr().cast(), random.len()) } != 0 {
        return Err(UnixSocketError::CleanupMismatch);
    }
    let mut suffix = String::with_capacity(random.len() * 2);
    for byte in random {
        use std::fmt::Write as _;
        write!(&mut suffix, "{byte:02x}").map_err(|_| UnixSocketError::CleanupMismatch)?;
    }
    CString::new(format!(".aegisy-{kind}-{suffix}")).map_err(|_| UnixSocketError::CleanupMismatch)
}

fn rename_to_quarantine(
    directory: RawFd,
    source: &CStr,
    kind: &str,
) -> Result<CString, UnixSocketError> {
    for _ in 0..4 {
        let quarantine = random_quarantine_name(kind)?;
        if unsafe {
            libc::renameatx_np(
                directory,
                source.as_ptr(),
                directory,
                quarantine.as_ptr(),
                libc::RENAME_EXCL,
            )
        } == 0
        {
            return Ok(quarantine);
        }
        if io::Error::last_os_error().kind() != io::ErrorKind::AlreadyExists {
            return Err(UnixSocketError::CleanupMismatch);
        }
    }
    Err(UnixSocketError::CleanupMismatch)
}

fn validate_directory_path(
    directory: &Path,
    expected_client_pid: libc::pid_t,
) -> Result<(), UnixSocketError> {
    if !directory.is_absolute() || expected_client_pid <= 1 {
        return Err(UnixSocketError::InvalidPath);
    }
    if directory
        .components()
        .any(|component| matches!(component, Component::CurDir | Component::ParentDir))
        || directory
            .as_os_str()
            .as_bytes()
            .split(|byte| *byte == b'/')
            .any(|component| component == b"." || component == b"..")
    {
        return Err(UnixSocketError::InvalidPath);
    }
    let name = directory
        .file_name()
        .and_then(OsStr::to_str)
        .ok_or(UnixSocketError::InvalidPath)?;
    let suffix = name
        .strip_prefix("aegisy-agent-")
        .ok_or(UnixSocketError::InvalidPath)?;
    if !(16..=32).contains(&suffix.len())
        || !suffix
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
        return Err(UnixSocketError::InvalidPath);
    }
    validate_socket_path_length(&directory.join(SOCKET_FILE_NAME))
}

fn validate_socket_path_length(socket: &Path) -> Result<(), UnixSocketError> {
    let bytes = socket.as_os_str().as_bytes();
    let capacity = unsafe { std::mem::zeroed::<libc::sockaddr_un>() }
        .sun_path
        .len();
    if bytes.is_empty() || bytes.contains(&0) || bytes.len() >= capacity {
        return Err(UnixSocketError::InvalidPath);
    }
    Ok(())
}

fn verify_peer(
    descriptor: RawFd,
    expected_uid: libc::uid_t,
    expected_pid: libc::pid_t,
) -> Result<(), UnixSocketError> {
    let mut uid: libc::uid_t = 0;
    let mut gid: libc::gid_t = 0;
    if unsafe { libc::getpeereid(descriptor, &mut uid, &mut gid) } != 0 {
        return Err(UnixSocketError::PeerQueryFailed);
    }
    let mut pid: libc::pid_t = 0;
    let mut length = libc::socklen_t::try_from(std::mem::size_of::<libc::pid_t>())
        .map_err(|_| UnixSocketError::PeerQueryFailed)?;
    if unsafe {
        libc::getsockopt(
            descriptor,
            libc::SOL_LOCAL,
            libc::LOCAL_PEERPID,
            std::ptr::addr_of_mut!(pid).cast(),
            &mut length,
        )
    } != 0
        || usize::try_from(length).ok() != Some(std::mem::size_of::<libc::pid_t>())
    {
        return Err(UnixSocketError::PeerQueryFailed);
    }
    verify_peer_values(uid, pid, expected_uid, expected_pid)
}

fn verify_peer_values(
    actual_uid: libc::uid_t,
    actual_pid: libc::pid_t,
    expected_uid: libc::uid_t,
    expected_pid: libc::pid_t,
) -> Result<(), UnixSocketError> {
    if actual_uid != expected_uid || actual_pid != expected_pid {
        return Err(UnixSocketError::PeerMismatch);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Runtime;
    use aegisy_aap::MAX_AAP_FRAME_BYTES;
    use serde_json::json;
    use std::fs::{DirBuilder, File, Permissions};
    use std::os::unix::fs::{symlink, DirBuilderExt, PermissionsExt};
    use std::process::Command;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::thread;

    static NEXT_TEST_ID: AtomicU64 = AtomicU64::new(1);

    struct TestRoot(PathBuf);

    impl TestRoot {
        fn new() -> Self {
            let id = NEXT_TEST_ID.fetch_add(1, Ordering::Relaxed);
            let path = Path::new("/tmp").join(format!("au-{}-{id}", std::process::id()));
            let _ = fs::remove_dir_all(&path);
            let mut builder = DirBuilder::new();
            builder.mode(u32::from(PRIVATE_DIRECTORY_MODE));
            builder.create(&path).unwrap();
            fs::set_permissions(
                &path,
                Permissions::from_mode(u32::from(PRIVATE_DIRECTORY_MODE)),
            )
            .unwrap();
            Self(path)
        }

        fn endpoint(&self, suffix: &str) -> PathBuf {
            self.0.join(format!("aegisy-agent-{suffix}"))
        }
    }

    impl Drop for TestRoot {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn bind_creates_owner_only_directory_and_socket() {
        let root = TestRoot::new();
        let endpoint = root.endpoint("0123456789abcdef");
        let listener = OwnerOnlyUnixListener::bind_fresh(&endpoint, unsafe { libc::getpid() })
            .expect("bind owner-only endpoint");
        let directory = fs::symlink_metadata(&endpoint).unwrap();
        let socket = fs::symlink_metadata(listener.socket_path()).unwrap();
        assert_eq!(permission_bits(&directory), PRIVATE_DIRECTORY_MODE);
        assert_eq!(permission_bits(&socket), PRIVATE_SOCKET_MODE);
        assert_eq!(directory.uid(), unsafe { libc::geteuid() });
        assert_eq!(socket.uid(), unsafe { libc::geteuid() });
        assert!(socket.file_type().is_socket());
        drop(listener);
        assert!(!endpoint.exists());
    }

    #[test]
    fn bind_rejects_existing_file_directory_symlink_relative_and_long_paths() {
        let root = TestRoot::new();
        let pid = unsafe { libc::getpid() };

        let existing = root.endpoint("1111111111111111");
        fs::create_dir(&existing).unwrap();
        assert_eq!(
            OwnerOnlyUnixListener::bind_fresh(&existing, pid).unwrap_err(),
            UnixSocketError::EndpointExists
        );

        let file = root.endpoint("2222222222222222");
        File::create(&file).unwrap();
        assert_eq!(
            OwnerOnlyUnixListener::bind_fresh(&file, pid).unwrap_err(),
            UnixSocketError::EndpointExists
        );

        let target = root.endpoint("3333333333333333");
        fs::create_dir(&target).unwrap();
        let link = root.endpoint("4444444444444444");
        symlink(&target, &link).unwrap();
        assert_eq!(
            OwnerOnlyUnixListener::bind_fresh(&link, pid).unwrap_err(),
            UnixSocketError::EndpointExists
        );
        assert_eq!(
            OwnerOnlyUnixListener::bind_fresh(Path::new("aegisy-agent-5555555555555555"), pid)
                .unwrap_err(),
            UnixSocketError::InvalidPath
        );
        let dotted = PathBuf::from(format!(
            "{}/./aegisy-agent-5555555555555555",
            root.0.display()
        ));
        assert_eq!(
            OwnerOnlyUnixListener::bind_fresh(&dotted, pid).unwrap_err(),
            UnixSocketError::InvalidPath
        );
        let parent_escape = root
            .0
            .join("unused")
            .join("..")
            .join("aegisy-agent-5555555555555555");
        assert_eq!(
            OwnerOnlyUnixListener::bind_fresh(&parent_escape, pid).unwrap_err(),
            UnixSocketError::InvalidPath
        );

        let long_root = TestRoot::new();
        let long_parent = long_root.0.join("x".repeat(90));
        fs::create_dir(&long_parent).unwrap();
        fs::set_permissions(
            &long_parent,
            Permissions::from_mode(u32::from(PRIVATE_DIRECTORY_MODE)),
        )
        .unwrap();
        assert_eq!(
            OwnerOnlyUnixListener::bind_fresh(
                &long_parent.join("aegisy-agent-6666666666666666"),
                pid,
            )
            .unwrap_err(),
            UnixSocketError::InvalidPath
        );
    }

    #[test]
    fn accept_verifies_current_uid_and_expected_pid_then_unlinks_listener() {
        let root = TestRoot::new();
        let endpoint = root.endpoint("7777777777777777");
        let listener = OwnerOnlyUnixListener::bind_fresh(&endpoint, unsafe { libc::getpid() })
            .expect("bind endpoint");
        let socket = listener.socket_path().to_path_buf();
        let client = thread::spawn(move || {
            let mut stream = UnixStream::connect(socket).expect("connect");
            stream.write_all(b"ping").unwrap();
            let mut reply = [0_u8; 4];
            stream.read_exact(&mut reply).unwrap();
            reply
        });
        let mut stream = listener.accept_one_for_test().expect("verify peer");
        assert!(!endpoint.join(SOCKET_FILE_NAME).exists());
        let mut request = [0_u8; 4];
        stream.read_exact(&mut request).unwrap();
        assert_eq!(&request, b"ping");
        stream.write_all(b"pong").unwrap();
        assert_eq!(&client.join().unwrap(), b"pong");
        drop(stream);
        assert!(!endpoint.exists());
    }

    #[test]
    fn accept_rejects_wrong_pid_before_returning_stream() {
        let root = TestRoot::new();
        let endpoint = root.endpoint("8888888888888888");
        let listener = OwnerOnlyUnixListener::bind_fresh(
            &endpoint,
            unsafe { libc::getpid() }.saturating_add(1),
        )
        .expect("bind endpoint");
        let socket = listener.socket_path().to_path_buf();
        let client = thread::spawn(move || UnixStream::connect(socket).expect("connect"));
        assert_eq!(
            listener.accept_one_for_test().unwrap_err(),
            UnixSocketError::PeerMismatch
        );
        drop(client.join().unwrap());
        assert!(!endpoint.exists());
    }

    #[test]
    fn peer_value_comparison_rejects_uid_or_pid_drift() {
        assert_eq!(verify_peer_values(10, 20, 10, 20), Ok(()));
        assert_eq!(
            verify_peer_values(11, 20, 10, 20),
            Err(UnixSocketError::PeerMismatch)
        );
        assert_eq!(
            verify_peer_values(10, 21, 10, 20),
            Err(UnixSocketError::PeerMismatch)
        );
    }

    #[test]
    fn bind_rejects_parent_with_extended_acl() {
        let root = TestRoot::new();
        let add = Command::new("/bin/chmod")
            .args(["+a", "everyone allow read"])
            .arg(&root.0)
            .status()
            .expect("run chmod +a");
        assert!(add.success());
        let endpoint = root.endpoint("bbbbbbbbbbbbbbbb");
        assert_eq!(
            OwnerOnlyUnixListener::bind_fresh(&endpoint, unsafe { libc::getpid() }).unwrap_err(),
            UnixSocketError::UnsafeParent
        );
        let clear = Command::new("/bin/chmod")
            .arg("-N")
            .arg(&root.0)
            .status()
            .expect("run chmod -N");
        assert!(clear.success());
    }

    #[test]
    fn accept_fails_when_supervising_parent_changes_or_times_out() {
        let root = TestRoot::new();
        let expected = unsafe { libc::getpid() };
        let parent_changed = root.endpoint("cccccccccccccccc");
        let listener = OwnerOnlyUnixListener::bind_fresh(&parent_changed, expected)
            .expect("bind parent-change endpoint");
        assert_eq!(
            listener
                .accept_one_with_parent_probe(Duration::from_millis(10), || 1)
                .unwrap_err(),
            UnixSocketError::ParentChanged
        );
        assert!(!parent_changed.exists());

        let timed_out = root.endpoint("dddddddddddddddd");
        let listener =
            OwnerOnlyUnixListener::bind_fresh(&timed_out, expected).expect("bind timeout endpoint");
        assert_eq!(
            listener
                .accept_one_with_parent_probe(Duration::from_millis(1), || expected)
                .unwrap_err(),
            UnixSocketError::AcceptTimeout
        );
        assert!(!timed_out.exists());
    }

    #[test]
    fn quarantine_preserves_replacement_inserted_during_cleanup_race() {
        let root = TestRoot::new();
        let endpoint = root.endpoint("eeeeeeeeeeeeeeee");
        let mut listener = OwnerOnlyUnixListener::bind_fresh(&endpoint, unsafe { libc::getpid() })
            .expect("bind cleanup-race endpoint");
        let socket = listener.socket_path().to_path_buf();
        let cleanup = listener.cleanup.as_mut().expect("listener cleanup");
        let result = cleanup.unlink_socket_with_hook(|| {
            fs::remove_file(&socket).unwrap();
            let mut replacement = File::create(&socket).unwrap();
            replacement.write_all(b"replacement").unwrap();
        });
        assert_eq!(result, Err(UnixSocketError::CleanupMismatch));
        let quarantined = fs::read_dir(&endpoint)
            .unwrap()
            .filter_map(Result::ok)
            .find(|entry| {
                entry
                    .file_name()
                    .to_string_lossy()
                    .starts_with(".aegisy-socket-")
            })
            .expect("replacement retained under quarantine");
        assert!(quarantined.file_type().unwrap().is_file());
        assert_eq!(fs::read(quarantined.path()).unwrap(), b"replacement");
        drop(listener);
        assert!(endpoint.exists());
    }

    #[test]
    fn quarantine_preserves_replacement_directory_inserted_during_cleanup_race() {
        let root = TestRoot::new();
        let endpoint = root.endpoint("ffffffffffffffff");
        let original = root.0.join("original-endpoint");
        let mut listener = OwnerOnlyUnixListener::bind_fresh(&endpoint, unsafe { libc::getpid() })
            .expect("bind directory cleanup-race endpoint");
        let cleanup = listener.cleanup.as_mut().expect("listener cleanup");
        cleanup.unlink_socket().expect("unlink owned socket");
        let result = cleanup.remove_directory_with_hook(|| {
            fs::rename(&endpoint, &original).unwrap();
            fs::create_dir(&endpoint).unwrap();
            fs::set_permissions(
                &endpoint,
                Permissions::from_mode(u32::from(PRIVATE_DIRECTORY_MODE)),
            )
            .unwrap();
            File::create(endpoint.join("replacement")).unwrap();
        });
        assert_eq!(result, Err(UnixSocketError::CleanupMismatch));
        let quarantined = fs::read_dir(&root.0)
            .unwrap()
            .filter_map(Result::ok)
            .find(|entry| {
                entry
                    .file_name()
                    .to_string_lossy()
                    .starts_with(".aegisy-directory-")
            })
            .expect("replacement directory retained under quarantine");
        assert!(quarantined.file_type().unwrap().is_dir());
        assert!(quarantined.path().join("replacement").is_file());
        assert!(original.is_dir());
        drop(listener);
        assert!(quarantined.path().is_dir());
    }

    #[test]
    fn verified_stream_binds_truthful_runtime_handshake_without_authentication() {
        let root = TestRoot::new();
        let endpoint = root.endpoint("9999999999999999");
        let listener = OwnerOnlyUnixListener::bind_fresh(&endpoint, unsafe { libc::getpid() })
            .expect("bind endpoint");
        let socket = listener.socket_path().to_path_buf();
        let client = thread::spawn(move || UnixStream::connect(socket).expect("connect"));
        let stream = listener.accept_one_for_test().expect("verify peer");
        let client = client.join().unwrap();

        let mut runtime = Runtime::default();
        runtime
            .bind_verified_unix_socket_transport(&stream)
            .expect("bind Runtime transport facts");
        let request = |id: &str, transport_security| {
            json!({
                "jsonrpc": "2.0",
                "id": id,
                "method": "initialize",
                "params": {
                    "protocol": {"minimum": "0.1", "maximum": "0.1", "preferred": "0.1"},
                    "client": {"name": "socket-test", "version": "1"},
                    "platform": {"os": "macos", "architecture": "arm64"},
                    "capabilities": {
                        "stable": ["runtime.preview", "permission.read-only"],
                        "experimental": []
                    },
                    "limits": {"max_frame_bytes": MAX_AAP_FRAME_BYTES},
                    "transport_security": transport_security
                }
            })
            .to_string()
        };
        let mismatch = runtime.handle_line(&request(
            "initialize-mismatch",
            json!({
                "transport": "stdio",
                "local": true,
                "authenticated": false,
                "encrypted": false,
                "peer_verified": false
            }),
        ));
        assert_eq!(mismatch[0]["error"]["code"], -32602);

        let response = runtime.handle_line(&request(
            "initialize-valid",
            json!({
                "transport": "unix-domain-socket",
                "local": true,
                "authenticated": false,
                "encrypted": false,
                "peer_verified": true
            }),
        ));
        assert_eq!(
            response[0]["result"]["transport_security"],
            json!({
                "transport": "unix-domain-socket",
                "local": true,
                "authenticated": false,
                "encrypted": false,
                "peer_verified": true
            }),
            "unexpected initialize response: {response:?}"
        );
        drop(client);
        drop(stream);
    }

    #[test]
    fn cleanup_preserves_a_replaced_socket_object() {
        let root = TestRoot::new();
        let endpoint = root.endpoint("aaaaaaaaaaaaaaaa");
        let listener = OwnerOnlyUnixListener::bind_fresh(&endpoint, unsafe { libc::getpid() })
            .expect("bind endpoint");
        let socket = listener.socket_path().to_path_buf();
        fs::remove_file(&socket).unwrap();
        File::create(&socket).unwrap();
        drop(listener);
        assert!(socket.is_file());
        assert!(endpoint.is_dir());
    }
}
