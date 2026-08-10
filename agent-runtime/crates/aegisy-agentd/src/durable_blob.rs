use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

pub const MAX_BLOB_BYTES: u64 = 16 * 1024 * 1024;
pub const MAX_BLOB_OBJECTS: u64 = 8_192;
pub const MAX_STORE_BYTES: u64 = 512 * 1024 * 1024;
pub const MIN_FREE_BYTES: u64 = 256 * 1024 * 1024;
pub const MAX_SCAN_ENTRIES: usize = 100_000;

const STORE_DIRECTORY: &str = "durable-blobs-v1";
const OBJECT_DIRECTORY: &str = "objects";
const LOCAL_IMAGE_DIRECTORY: &str = "local-images";
static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BlobFileError {
    pub code: &'static str,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BlobWriteOutcome {
    pub created: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BlobDiskInventory {
    pub hashes: BTreeSet<String>,
    pub unknown_entries: u64,
    pub limit_exceeded: bool,
}

#[derive(Debug)]
pub struct DurableBlobFileStore {
    root: PathBuf,
    objects: PathBuf,
    local_images: PathBuf,
}

#[derive(Debug)]
pub struct DurableBlobLocalFile {
    path: PathBuf,
}

impl DurableBlobLocalFile {
    pub fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for DurableBlobLocalFile {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.path);
    }
}

impl DurableBlobFileStore {
    pub fn open(data_root: &Path) -> Result<Self, BlobFileError> {
        let root = data_root.join(STORE_DIRECTORY);
        create_private_directory(&root)?;
        let objects = root.join(OBJECT_DIRECTORY);
        create_private_directory(&objects)?;
        let canonical_root = root
            .canonicalize()
            .map_err(|_| file_error("blob-store-unavailable"))?;
        if canonical_root != root {
            return Err(file_error("blob-store-unsafe"));
        }
        let canonical_objects = objects
            .canonicalize()
            .map_err(|_| file_error("blob-store-unavailable"))?;
        if canonical_objects != objects || !canonical_objects.starts_with(&canonical_root) {
            return Err(file_error("blob-store-unsafe"));
        }
        let local_images = canonical_root.join(LOCAL_IMAGE_DIRECTORY);
        create_private_directory(&local_images)?;
        let canonical_local_images = local_images
            .canonicalize()
            .map_err(|_| file_error("blob-local-image-directory-unavailable"))?;
        if canonical_local_images != local_images
            || !canonical_local_images.starts_with(&canonical_root)
        {
            return Err(file_error("blob-local-image-directory-unsafe"));
        }
        clean_stale_local_images(&canonical_local_images)?;
        Ok(Self {
            root: canonical_root,
            objects: canonical_objects,
            local_images: canonical_local_images,
        })
    }

    pub fn storage_key(sha256: &str) -> Result<String, BlobFileError> {
        validate_sha256(sha256)?;
        Ok(format!("{OBJECT_DIRECTORY}/{}/{}", &sha256[..2], sha256))
    }

    pub fn put(
        &self,
        sha256: &str,
        bytes: &[u8],
        available_bytes_override: Option<u64>,
    ) -> Result<BlobWriteOutcome, BlobFileError> {
        validate_sha256(sha256)?;
        if bytes.len() as u64 > MAX_BLOB_BYTES || sha256_hex(bytes) != sha256 {
            return Err(file_error("blob-content-invalid"));
        }
        self.validate_layout()?;
        let parent = self.object_parent(sha256)?;
        create_private_directory(&parent)?;
        let target = parent.join(sha256);
        if fs::symlink_metadata(&target).is_ok() {
            self.read(sha256, bytes.len() as u64)?;
            return Ok(BlobWriteOutcome { created: false });
        }
        let available = available_bytes_override.or_else(|| available_space(&self.root));
        if available
            .is_some_and(|available| available < MIN_FREE_BYTES.saturating_add(bytes.len() as u64))
        {
            return Err(file_error("blob-admission-low-space"));
        }

        let temporary = temporary_path(&parent);
        let write_result = (|| {
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&temporary)
                .map_err(|_| file_error("blob-stage-create-failed"))?;
            secure_file(&temporary)?;
            file.write_all(bytes)
                .and_then(|()| file.sync_all())
                .map_err(|_| file_error("blob-stage-write-failed"))?;
            match fs::hard_link(&temporary, &target) {
                Ok(()) => Ok(true),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {
                    self.read(sha256, bytes.len() as u64)?;
                    Ok(false)
                }
                Err(_) => Err(file_error("blob-commit-failed")),
            }
        })();
        let _ = fs::remove_file(&temporary);
        let created = write_result?;
        if created {
            secure_file(&target)?;
            sync_directory(&parent)?;
        }
        Ok(BlobWriteOutcome { created })
    }

    pub fn read(&self, sha256: &str, expected_bytes: u64) -> Result<Vec<u8>, BlobFileError> {
        validate_sha256(sha256)?;
        if expected_bytes > MAX_BLOB_BYTES {
            return Err(file_error("blob-size-invalid"));
        }
        self.validate_layout()?;
        let path = self.object_path(sha256)?;
        let metadata = fs::symlink_metadata(&path).map_err(|_| file_error("blob-file-missing"))?;
        if metadata.file_type().is_symlink() || !metadata.is_file() {
            return Err(file_error("blob-file-unsafe"));
        }
        if metadata.len() != expected_bytes {
            return Err(file_error("blob-size-mismatch"));
        }
        let file = File::open(&path).map_err(|_| file_error("blob-read-failed"))?;
        let mut content = Vec::with_capacity(expected_bytes as usize);
        file.take(MAX_BLOB_BYTES.saturating_add(1))
            .read_to_end(&mut content)
            .map_err(|_| file_error("blob-read-failed"))?;
        if content.len() as u64 != expected_bytes || sha256_hex(&content) != sha256 {
            return Err(file_error("blob-integrity-mismatch"));
        }
        Ok(content)
    }

    pub fn link_verified_local_image(
        &self,
        sha256: &str,
        expected_bytes: u64,
        extension: &str,
    ) -> Result<DurableBlobLocalFile, BlobFileError> {
        if extension.is_empty()
            || extension.len() > 8
            || !extension.bytes().all(|byte| byte.is_ascii_lowercase())
        {
            return Err(file_error("blob-local-image-extension-invalid"));
        }
        self.read(sha256, expected_bytes)?;
        let source = self.object_path(sha256)?;
        for _ in 0..32 {
            let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
            let target = self.local_images.join(format!(
                "image-{}-{sequence}.{extension}",
                std::process::id()
            ));
            match fs::hard_link(&source, &target) {
                Ok(()) => {
                    if let Err(cause) = secure_file(&target) {
                        let _ = fs::remove_file(&target);
                        return Err(cause);
                    }
                    return Ok(DurableBlobLocalFile { path: target });
                }
                Err(cause) if cause.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(_) => return Err(file_error("blob-local-image-link-failed")),
            }
        }
        Err(file_error("blob-local-image-link-exhausted"))
    }

    pub fn remove_verified(&self, sha256: &str, expected_bytes: u64) -> Result<(), BlobFileError> {
        self.read(sha256, expected_bytes)?;
        let path = self.object_path(sha256)?;
        fs::remove_file(path).map_err(|_| file_error("blob-delete-failed"))?;
        sync_directory(&self.object_parent(sha256)?)
    }

    pub fn remove_created(&self, sha256: &str, expected_bytes: u64) {
        if self.read(sha256, expected_bytes).is_ok() {
            if let Ok(path) = self.object_path(sha256) {
                let _ = fs::remove_file(path);
            }
            if let Ok(parent) = self.object_parent(sha256) {
                let _ = sync_directory(&parent);
            }
        }
    }

    pub fn inventory(&self) -> Result<BlobDiskInventory, BlobFileError> {
        self.validate_layout()?;
        let mut hashes = BTreeSet::new();
        let mut unknown_entries = 0_u64;
        let mut visited = 0_usize;
        let mut limit_exceeded = false;
        for prefix_entry in
            fs::read_dir(&self.objects).map_err(|_| file_error("blob-inventory-failed"))?
        {
            if visited >= MAX_SCAN_ENTRIES {
                limit_exceeded = true;
                break;
            }
            visited += 1;
            let prefix_entry = prefix_entry.map_err(|_| file_error("blob-inventory-failed"))?;
            let prefix_name = prefix_entry.file_name().to_string_lossy().into_owned();
            let metadata = fs::symlink_metadata(prefix_entry.path())
                .map_err(|_| file_error("blob-inventory-failed"))?;
            if metadata.file_type().is_symlink()
                || !metadata.is_dir()
                || prefix_name.len() != 2
                || !prefix_name.bytes().all(is_lower_hex)
            {
                unknown_entries = unknown_entries.saturating_add(1);
                continue;
            }
            for object_entry in fs::read_dir(prefix_entry.path())
                .map_err(|_| file_error("blob-inventory-failed"))?
            {
                if visited >= MAX_SCAN_ENTRIES {
                    limit_exceeded = true;
                    break;
                }
                visited += 1;
                let object_entry = object_entry.map_err(|_| file_error("blob-inventory-failed"))?;
                let name = object_entry.file_name().to_string_lossy().into_owned();
                let metadata = fs::symlink_metadata(object_entry.path())
                    .map_err(|_| file_error("blob-inventory-failed"))?;
                if metadata.file_type().is_symlink()
                    || !metadata.is_file()
                    || validate_sha256(&name).is_err()
                    || !name.starts_with(&prefix_name)
                {
                    unknown_entries = unknown_entries.saturating_add(1);
                    continue;
                }
                hashes.insert(name);
            }
            if limit_exceeded {
                break;
            }
        }
        Ok(BlobDiskInventory {
            hashes,
            unknown_entries,
            limit_exceeded,
        })
    }

    fn validate_layout(&self) -> Result<(), BlobFileError> {
        for path in [&self.root, &self.objects] {
            let metadata =
                fs::symlink_metadata(path).map_err(|_| file_error("blob-store-unavailable"))?;
            if metadata.file_type().is_symlink() || !metadata.is_dir() {
                return Err(file_error("blob-store-unsafe"));
            }
        }
        Ok(())
    }

    fn object_parent(&self, sha256: &str) -> Result<PathBuf, BlobFileError> {
        validate_sha256(sha256)?;
        Ok(self.objects.join(&sha256[..2]))
    }

    fn object_path(&self, sha256: &str) -> Result<PathBuf, BlobFileError> {
        Ok(self.object_parent(sha256)?.join(sha256))
    }
}

fn clean_stale_local_images(directory: &Path) -> Result<(), BlobFileError> {
    for entry in fs::read_dir(directory)
        .map_err(|_| file_error("blob-local-image-cleanup-failed"))?
        .take(MAX_SCAN_ENTRIES)
    {
        let entry = entry.map_err(|_| file_error("blob-local-image-cleanup-failed"))?;
        let name = entry.file_name().to_string_lossy().into_owned();
        let metadata = fs::symlink_metadata(entry.path())
            .map_err(|_| file_error("blob-local-image-cleanup-failed"))?;
        if metadata.is_file() && !metadata.file_type().is_symlink() && valid_local_image_name(&name)
        {
            fs::remove_file(entry.path())
                .map_err(|_| file_error("blob-local-image-cleanup-failed"))?;
        }
    }
    Ok(())
}

fn valid_local_image_name(value: &str) -> bool {
    let Some((stem, extension)) = value.rsplit_once('.') else {
        return false;
    };
    let Some((process_id, sequence)) = stem
        .strip_prefix("image-")
        .and_then(|suffix| suffix.split_once('-'))
    else {
        return false;
    };
    !process_id.is_empty()
        && !sequence.is_empty()
        && process_id.bytes().all(|byte| byte.is_ascii_digit())
        && sequence.bytes().all(|byte| byte.is_ascii_digit())
        && matches!(extension, "png" | "jpg" | "webp")
}

pub fn sha256_hex(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    digest.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn validate_sha256(value: &str) -> Result<(), BlobFileError> {
    if value.len() != 64 || !value.bytes().all(is_lower_hex) {
        return Err(file_error("blob-hash-invalid"));
    }
    Ok(())
}

fn is_lower_hex(byte: u8) -> bool {
    byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte)
}

fn temporary_path(parent: &Path) -> PathBuf {
    let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    parent.join(format!(
        ".aegisy-blob-{}-{sequence}.tmp",
        std::process::id()
    ))
}

fn create_private_directory(path: &Path) -> Result<(), BlobFileError> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() || !metadata.is_dir() => {
            return Err(file_error("blob-store-unsafe"));
        }
        Ok(_) => {}
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            fs::create_dir(path).map_err(|_| file_error("blob-store-create-failed"))?;
        }
        Err(_) => return Err(file_error("blob-store-unavailable")),
    }
    secure_directory(path)
}

#[cfg(unix)]
fn secure_directory(path: &Path) -> Result<(), BlobFileError> {
    use std::os::unix::fs::PermissionsExt;
    fs::set_permissions(path, fs::Permissions::from_mode(0o700))
        .map_err(|_| file_error("blob-store-permissions-failed"))
}

#[cfg(not(unix))]
fn secure_directory(_path: &Path) -> Result<(), BlobFileError> {
    Ok(())
}

#[cfg(unix)]
fn secure_file(path: &Path) -> Result<(), BlobFileError> {
    use std::os::unix::fs::PermissionsExt;
    fs::set_permissions(path, fs::Permissions::from_mode(0o600))
        .map_err(|_| file_error("blob-file-permissions-failed"))
}

#[cfg(not(unix))]
fn secure_file(_path: &Path) -> Result<(), BlobFileError> {
    Ok(())
}

#[cfg(unix)]
fn sync_directory(path: &Path) -> Result<(), BlobFileError> {
    File::open(path)
        .and_then(|directory| directory.sync_all())
        .map_err(|_| file_error("blob-directory-sync-failed"))
}

#[cfg(not(unix))]
fn sync_directory(_path: &Path) -> Result<(), BlobFileError> {
    Ok(())
}

#[cfg(unix)]
pub(crate) fn available_space(path: &Path) -> Option<u64> {
    use std::ffi::CString;
    use std::os::unix::ffi::OsStrExt;
    let path = CString::new(path.as_os_str().as_bytes()).ok()?;
    let mut status = std::mem::MaybeUninit::<libc::statvfs>::uninit();
    let result = unsafe { libc::statvfs(path.as_ptr(), status.as_mut_ptr()) };
    if result != 0 {
        return None;
    }
    let status = unsafe { status.assume_init() };
    let available_bytes = u128::from(status.f_bavail).saturating_mul(u128::from(status.f_frsize));
    u64::try_from(available_bytes).ok()
}

#[cfg(windows)]
pub(crate) fn available_space(path: &Path) -> Option<u64> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Storage::FileSystem::GetDiskFreeSpaceExW;
    let wide = path
        .as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect::<Vec<_>>();
    let mut available = 0_u64;
    let result = unsafe {
        GetDiskFreeSpaceExW(
            wide.as_ptr(),
            &mut available,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        )
    };
    (result != 0).then_some(available)
}

#[cfg(not(any(unix, windows)))]
pub(crate) fn available_space(_path: &Path) -> Option<u64> {
    None
}

fn file_error(code: &'static str) -> BlobFileError {
    BlobFileError { code }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn local_image_links_drop_normally_and_known_crash_leftovers_are_cleaned_on_open() {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let data_root = std::env::temp_dir().join(format!("aegisy-blob-image-{unique}"));
        fs::create_dir_all(&data_root).unwrap();
        let data_root = data_root.canonicalize().unwrap();
        let content = b"verified-image-placeholder";
        let sha256 = sha256_hex(content);
        let store = DurableBlobFileStore::open(&data_root).unwrap();
        store.put(&sha256, content, Some(u64::MAX)).unwrap();
        let lease = store
            .link_verified_local_image(&sha256, content.len() as u64, "png")
            .unwrap();
        let normal_path = lease.path().to_path_buf();
        assert!(normal_path.is_file());
        drop(lease);
        assert!(!normal_path.exists());

        let leaked = store
            .link_verified_local_image(&sha256, content.len() as u64, "png")
            .unwrap();
        let leaked_path = leaked.path().to_path_buf();
        std::mem::forget(leaked);
        let unknown = store.local_images.join("unknown.keep");
        fs::write(&unknown, b"preserve").unwrap();
        let unknown_image_name = store.local_images.join("image--.png");
        fs::write(&unknown_image_name, b"preserve").unwrap();
        drop(store);
        let _reopened = DurableBlobFileStore::open(&data_root).unwrap();
        assert!(!leaked_path.exists());
        assert!(unknown.exists());
        assert!(unknown_image_name.exists());
        fs::remove_dir_all(data_root).unwrap();
    }
}
