use crate::durable_blob::{available_space, MIN_FREE_BYTES};
use rusqlite::{Connection, OpenFlags, MAIN_DB};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

const BACKUP_DIRECTORY: &str = "migration-backups-v1";
const MANIFEST_SCHEMA: &str = "workbench-migration-backup/0.1";
const RECOVERY_SCHEMA: &str = "workbench-recovery-diagnostic/0.1";
const MAX_BACKUP_BYTES: u64 = 1024 * 1024 * 1024;
const MAX_BACKUPS: usize = 16;
const MAX_MANIFEST_BYTES: usize = 16 * 1024;
const MAX_SCAN_ENTRIES: usize = 64;
static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MigrationBackupError {
    pub code: &'static str,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct MigrationBackupManifest {
    pub schema_version: String,
    pub source_schema_version: u64,
    pub target_schema_version: u64,
    pub source_application_id: u64,
    pub backup_file: String,
    pub backup_sha256: String,
    pub backup_bytes: u64,
    pub created_at_ms: u64,
    pub integrity: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct WorkbenchRecoveryDiagnostic {
    pub schema_version: String,
    pub mode: String,
    pub reason_code: String,
    pub target_schema_version: u64,
    pub source_schema_version: Option<u64>,
    pub database_present: bool,
    pub database_readable: bool,
    pub database_integrity_ok: bool,
    pub application_id_valid: bool,
    pub backup_available: bool,
    pub valid_backup_count: u64,
    pub invalid_backup_count: u64,
    pub database_bytes: Option<u64>,
    pub issues: Vec<String>,
}

#[derive(Debug)]
struct BackupInventory {
    valid: Vec<MigrationBackupManifest>,
    invalid_count: u64,
}

pub fn create_pre_upgrade_backup(
    source: &Connection,
    data_root: &Path,
    source_schema_version: u64,
    target_schema_version: u64,
    application_id: u64,
) -> Result<MigrationBackupManifest, MigrationBackupError> {
    create_pre_upgrade_backup_internal(
        source,
        data_root,
        source_schema_version,
        target_schema_version,
        application_id,
        None,
    )
}

#[cfg(test)]
pub fn create_pre_upgrade_backup_with_available_bytes(
    source: &Connection,
    data_root: &Path,
    source_schema_version: u64,
    target_schema_version: u64,
    application_id: u64,
    available_bytes: u64,
) -> Result<MigrationBackupManifest, MigrationBackupError> {
    create_pre_upgrade_backup_internal(
        source,
        data_root,
        source_schema_version,
        target_schema_version,
        application_id,
        Some(available_bytes),
    )
}

#[cfg(test)]
pub fn migration_backup_manifests(
    data_root: &Path,
) -> Result<Vec<MigrationBackupManifest>, MigrationBackupError> {
    inspect_inventory(&existing_backup_root(data_root)?).map(|inventory| inventory.valid)
}

fn create_pre_upgrade_backup_internal(
    source: &Connection,
    data_root: &Path,
    source_schema_version: u64,
    target_schema_version: u64,
    application_id: u64,
    available_bytes_override: Option<u64>,
) -> Result<MigrationBackupManifest, MigrationBackupError> {
    if source_schema_version == 0
        || source_schema_version >= target_schema_version
        || target_schema_version > i64::MAX as u64
    {
        return Err(backup_error("migration-backup-version-invalid"));
    }
    let quick_check: String = source
        .query_row("PRAGMA quick_check(1)", [], |row| row.get(0))
        .map_err(|_| backup_error("migration-source-unreadable"))?;
    if quick_check != "ok" {
        return Err(backup_error("migration-source-integrity-failed"));
    }
    let page_count: i64 = source
        .pragma_query_value(None, "page_count", |row| row.get(0))
        .map_err(|_| backup_error("migration-source-size-unavailable"))?;
    let page_size: i64 = source
        .pragma_query_value(None, "page_size", |row| row.get(0))
        .map_err(|_| backup_error("migration-source-size-unavailable"))?;
    let estimated_bytes = u64::try_from(page_count)
        .ok()
        .and_then(|pages| {
            u64::try_from(page_size)
                .ok()
                .and_then(|size| pages.checked_mul(size))
        })
        .ok_or_else(|| backup_error("migration-source-size-invalid"))?;
    if estimated_bytes == 0 || estimated_bytes > MAX_BACKUP_BYTES {
        return Err(backup_error("migration-backup-size-limit"));
    }
    let available = available_bytes_override.or_else(|| available_space(data_root));
    if available.is_some_and(|bytes| bytes < estimated_bytes.saturating_add(MIN_FREE_BYTES)) {
        return Err(backup_error("migration-backup-low-space"));
    }

    let backup_root = open_backup_root(data_root)?;
    let inventory = inspect_inventory(&backup_root)?;
    if inventory
        .valid
        .len()
        .saturating_add(inventory.invalid_count as usize)
        >= MAX_BACKUPS
    {
        return Err(backup_error("migration-backup-count-limit"));
    }
    let temporary = temporary_path(&backup_root, "sqlite3");
    OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temporary)
        .and_then(|file| file.sync_all())
        .map_err(|_| backup_error("migration-backup-stage-create-failed"))?;
    secure_file(&temporary)?;
    if let Err(cause) = source
        .backup(MAIN_DB, &temporary, None)
        .map_err(|_| backup_error("migration-backup-copy-failed"))
    {
        remove_sqlite_files(&temporary);
        return Err(cause);
    }
    if let Err(cause) = normalize_backup_database(&temporary) {
        remove_sqlite_files(&temporary);
        return Err(cause);
    }
    secure_file(&temporary)?;
    File::open(&temporary)
        .and_then(|file| file.sync_all())
        .map_err(|_| backup_error("migration-backup-sync-failed"))?;
    validate_backup_database(&temporary, source_schema_version, application_id)?;
    let (backup_sha256, backup_bytes) = hash_bounded_file(&temporary)?;
    let backup_file = format!(
        "schema-v{source_schema_version}-to-v{target_schema_version}-{backup_sha256}.sqlite3"
    );
    let committed = backup_root.join(&backup_file);
    match fs::hard_link(&temporary, &committed) {
        Ok(()) => {
            secure_file(&committed)?;
            sync_directory(&backup_root)?;
        }
        Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {
            let (existing_hash, existing_bytes) = hash_bounded_file(&committed)?;
            if existing_hash != backup_sha256 || existing_bytes != backup_bytes {
                remove_sqlite_files(&temporary);
                return Err(backup_error("migration-backup-no-clobber-conflict"));
            }
        }
        Err(_) => {
            remove_sqlite_files(&temporary);
            return Err(backup_error("migration-backup-commit-failed"));
        }
    }
    remove_sqlite_files(&temporary);

    let manifest_path = backup_root.join(format!("{backup_file}.manifest.json"));
    if manifest_path.exists() {
        let manifest = read_manifest(&manifest_path)?;
        validate_manifest(&backup_root, &manifest)?;
        if manifest.backup_sha256 != backup_sha256
            || manifest.backup_bytes != backup_bytes
            || manifest.source_schema_version != source_schema_version
            || manifest.target_schema_version != target_schema_version
            || manifest.source_application_id != application_id
        {
            return Err(backup_error("migration-backup-manifest-conflict"));
        }
        return Ok(manifest);
    }
    let manifest = MigrationBackupManifest {
        schema_version: MANIFEST_SCHEMA.into(),
        source_schema_version,
        target_schema_version,
        source_application_id: application_id,
        backup_file,
        backup_sha256,
        backup_bytes,
        created_at_ms: now_ms(),
        integrity: "quick-check-ok".into(),
    };
    let encoded = serde_json::to_vec(&manifest)
        .map_err(|_| backup_error("migration-backup-manifest-invalid"))?;
    if encoded.len() > MAX_MANIFEST_BYTES {
        return Err(backup_error("migration-backup-manifest-too-large"));
    }
    write_no_clobber(&manifest_path, &encoded)?;
    validate_manifest(&backup_root, &manifest)?;
    Ok(manifest)
}

pub fn inspect_recovery(
    data_root: &Path,
    database_path: &Path,
    reason_code: &str,
    target_schema_version: u64,
    expected_application_id: u64,
) -> WorkbenchRecoveryDiagnostic {
    let mut issues = vec![bounded_reason(reason_code)];
    let metadata = fs::symlink_metadata(database_path).ok();
    let database_present = metadata
        .as_ref()
        .is_some_and(|metadata| metadata.is_file() && !metadata.file_type().is_symlink());
    let database_bytes = metadata.as_ref().map(fs::Metadata::len);
    let mut database_readable = false;
    let mut database_integrity_ok = false;
    let mut application_id_valid = false;
    let mut source_schema_version = None;
    if database_present {
        if let Ok(connection) = Connection::open_with_flags(
            database_path,
            OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX,
        ) {
            let _ = connection.busy_timeout(std::time::Duration::from_secs(2));
            let _ = connection.execute_batch(
                "PRAGMA query_only = ON;
                 PRAGMA trusted_schema = OFF;",
            );
            let version = connection
                .pragma_query_value(None, "user_version", |row| row.get::<_, i64>(0))
                .ok()
                .and_then(|version| u64::try_from(version).ok());
            let application_id = connection
                .pragma_query_value(None, "application_id", |row| row.get::<_, i64>(0))
                .ok()
                .and_then(|value| u64::try_from(value).ok());
            let quick_check = connection
                .query_row("PRAGMA quick_check(1)", [], |row| row.get::<_, String>(0))
                .ok();
            database_readable = version.is_some() && application_id.is_some();
            database_integrity_ok = quick_check.as_deref() == Some("ok");
            application_id_valid =
                application_id.is_some_and(|value| value == 0 || value == expected_application_id);
            source_schema_version = version;
        }
    }
    if !database_present {
        push_issue(&mut issues, "recovery-database-missing");
    } else if !database_readable {
        push_issue(&mut issues, "recovery-database-unreadable");
    }
    if database_readable && !database_integrity_ok {
        push_issue(&mut issues, "recovery-database-integrity-failed");
    }
    if database_readable && !application_id_valid {
        push_issue(&mut issues, "recovery-application-id-invalid");
    }

    let inventory = existing_backup_root(data_root)
        .and_then(|root| inspect_inventory(&root))
        .ok();
    let valid_backup_count = inventory
        .as_ref()
        .map_or(0, |inventory| inventory.valid.len() as u64);
    let invalid_backup_count = inventory
        .as_ref()
        .map_or(0, |inventory| inventory.invalid_count);
    if invalid_backup_count > 0 {
        push_issue(&mut issues, "recovery-backup-invalid");
    }
    WorkbenchRecoveryDiagnostic {
        schema_version: RECOVERY_SCHEMA.into(),
        mode: "read-only-recovery".into(),
        reason_code: bounded_reason(reason_code),
        target_schema_version,
        source_schema_version,
        database_present,
        database_readable,
        database_integrity_ok,
        application_id_valid,
        backup_available: valid_backup_count > 0,
        valid_backup_count,
        invalid_backup_count,
        database_bytes,
        issues,
    }
}

fn validate_backup_database(
    path: &Path,
    expected_schema_version: u64,
    expected_application_id: u64,
) -> Result<(), MigrationBackupError> {
    let connection = Connection::open_with_flags(
        path,
        OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX,
    )
    .map_err(|_| backup_error("migration-backup-unreadable"))?;
    let quick_check: String = connection
        .query_row("PRAGMA quick_check(1)", [], |row| row.get(0))
        .map_err(|_| backup_error("migration-backup-integrity-failed"))?;
    let schema_version: i64 = connection
        .pragma_query_value(None, "user_version", |row| row.get(0))
        .map_err(|_| backup_error("migration-backup-version-unreadable"))?;
    let application_id: i64 = connection
        .pragma_query_value(None, "application_id", |row| row.get(0))
        .map_err(|_| backup_error("migration-backup-application-id-unreadable"))?;
    if quick_check != "ok"
        || u64::try_from(schema_version).ok() != Some(expected_schema_version)
        || u64::try_from(application_id).ok() != Some(expected_application_id)
    {
        return Err(backup_error("migration-backup-identity-mismatch"));
    }
    Ok(())
}

fn normalize_backup_database(path: &Path) -> Result<(), MigrationBackupError> {
    let connection = Connection::open_with_flags(
        path,
        OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_NO_MUTEX,
    )
    .map_err(|_| backup_error("migration-backup-normalize-failed"))?;
    connection
        .busy_timeout(std::time::Duration::from_secs(2))
        .map_err(|_| backup_error("migration-backup-normalize-failed"))?;
    let journal: String = connection
        .query_row("PRAGMA journal_mode = DELETE", [], |row| row.get(0))
        .map_err(|_| backup_error("migration-backup-normalize-failed"))?;
    if !journal.eq_ignore_ascii_case("delete") {
        return Err(backup_error("migration-backup-normalize-failed"));
    }
    connection
        .execute_batch("PRAGMA synchronous = FULL;")
        .map_err(|_| backup_error("migration-backup-normalize-failed"))?;
    drop(connection);
    Ok(())
}

fn remove_sqlite_files(path: &Path) {
    let _ = fs::remove_file(path);
    let path_text = path.to_string_lossy();
    let _ = fs::remove_file(format!("{path_text}-wal"));
    let _ = fs::remove_file(format!("{path_text}-shm"));
}

fn open_backup_root(data_root: &Path) -> Result<PathBuf, MigrationBackupError> {
    let root = data_root.join(BACKUP_DIRECTORY);
    match fs::symlink_metadata(&root) {
        Ok(metadata) if metadata.file_type().is_symlink() || !metadata.is_dir() => {
            return Err(backup_error("migration-backup-root-unsafe"));
        }
        Ok(_) => {}
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            fs::create_dir(&root)
                .map_err(|_| backup_error("migration-backup-root-create-failed"))?;
        }
        Err(_) => return Err(backup_error("migration-backup-root-unavailable")),
    }
    secure_directory(&root)?;
    let canonical = root
        .canonicalize()
        .map_err(|_| backup_error("migration-backup-root-unavailable"))?;
    if canonical != root {
        return Err(backup_error("migration-backup-root-unsafe"));
    }
    Ok(canonical)
}

fn existing_backup_root(data_root: &Path) -> Result<PathBuf, MigrationBackupError> {
    let root = data_root.join(BACKUP_DIRECTORY);
    let metadata = fs::symlink_metadata(&root)
        .map_err(|_| backup_error("migration-backup-root-unavailable"))?;
    if metadata.file_type().is_symlink() || !metadata.is_dir() {
        return Err(backup_error("migration-backup-root-unsafe"));
    }
    let canonical = root
        .canonicalize()
        .map_err(|_| backup_error("migration-backup-root-unavailable"))?;
    if canonical != root {
        return Err(backup_error("migration-backup-root-unsafe"));
    }
    Ok(canonical)
}

fn inspect_inventory(root: &Path) -> Result<BackupInventory, MigrationBackupError> {
    let mut valid = Vec::new();
    let mut invalid_count = 0_u64;
    let mut non_manifest_files = Vec::new();
    for (entries, entry) in fs::read_dir(root)
        .map_err(|_| backup_error("migration-backup-scan-failed"))?
        .enumerate()
    {
        if entries >= MAX_SCAN_ENTRIES {
            invalid_count = invalid_count.saturating_add(1);
            break;
        }
        let entry = entry.map_err(|_| backup_error("migration-backup-scan-failed"))?;
        let name = entry.file_name().to_string_lossy().into_owned();
        if !name.ends_with(".manifest.json") {
            non_manifest_files.push(name);
            continue;
        }
        match read_manifest(&entry.path()).and_then(|manifest| {
            validate_manifest(root, &manifest)?;
            Ok(manifest)
        }) {
            Ok(manifest) => valid.push(manifest),
            Err(_) => invalid_count = invalid_count.saturating_add(1),
        }
    }
    valid.sort_by_key(|manifest| manifest.created_at_ms);
    let referenced_files = valid
        .iter()
        .map(|manifest| manifest.backup_file.as_str())
        .collect::<BTreeSet<_>>();
    invalid_count = invalid_count.saturating_add(
        non_manifest_files
            .iter()
            .filter(|name| !referenced_files.contains(name.as_str()))
            .count() as u64,
    );
    Ok(BackupInventory {
        valid,
        invalid_count,
    })
}

fn read_manifest(path: &Path) -> Result<MigrationBackupManifest, MigrationBackupError> {
    let metadata = fs::symlink_metadata(path)
        .map_err(|_| backup_error("migration-backup-manifest-missing"))?;
    if metadata.file_type().is_symlink()
        || !metadata.is_file()
        || metadata.len() > MAX_MANIFEST_BYTES as u64
    {
        return Err(backup_error("migration-backup-manifest-unsafe"));
    }
    let bytes = fs::read(path).map_err(|_| backup_error("migration-backup-manifest-unreadable"))?;
    serde_json::from_slice(&bytes).map_err(|_| backup_error("migration-backup-manifest-invalid"))
}

fn validate_manifest(
    root: &Path,
    manifest: &MigrationBackupManifest,
) -> Result<(), MigrationBackupError> {
    if manifest.schema_version != MANIFEST_SCHEMA
        || manifest.source_schema_version == 0
        || manifest.source_schema_version >= manifest.target_schema_version
        || manifest.integrity != "quick-check-ok"
        || manifest.backup_bytes == 0
        || manifest.backup_bytes > MAX_BACKUP_BYTES
        || manifest.backup_sha256.len() != 64
        || !manifest.backup_sha256.bytes().all(is_lower_hex)
        || manifest.backup_file
            != format!(
                "schema-v{}-to-v{}-{}.sqlite3",
                manifest.source_schema_version,
                manifest.target_schema_version,
                manifest.backup_sha256
            )
    {
        return Err(backup_error("migration-backup-manifest-invalid"));
    }
    let backup = root.join(&manifest.backup_file);
    if backup.parent() != Some(root) {
        return Err(backup_error("migration-backup-manifest-unsafe"));
    }
    let (sha256, bytes) = hash_bounded_file(&backup)?;
    if sha256 != manifest.backup_sha256 || bytes != manifest.backup_bytes {
        return Err(backup_error("migration-backup-integrity-mismatch"));
    }
    validate_backup_database(
        &backup,
        manifest.source_schema_version,
        manifest.source_application_id,
    )
}

fn hash_bounded_file(path: &Path) -> Result<(String, u64), MigrationBackupError> {
    let metadata =
        fs::symlink_metadata(path).map_err(|_| backup_error("migration-backup-file-missing"))?;
    if metadata.file_type().is_symlink()
        || !metadata.is_file()
        || metadata.len() == 0
        || metadata.len() > MAX_BACKUP_BYTES
    {
        return Err(backup_error("migration-backup-file-unsafe"));
    }
    let mut file = File::open(path).map_err(|_| backup_error("migration-backup-unreadable"))?;
    let mut digest = Sha256::new();
    let mut buffer = [0_u8; 64 * 1024];
    let mut total = 0_u64;
    loop {
        let read = file
            .read(&mut buffer)
            .map_err(|_| backup_error("migration-backup-unreadable"))?;
        if read == 0 {
            break;
        }
        total = total.saturating_add(read as u64);
        if total > MAX_BACKUP_BYTES {
            return Err(backup_error("migration-backup-size-limit"));
        }
        digest.update(&buffer[..read]);
    }
    Ok((format!("{:x}", digest.finalize()), total))
}

fn write_no_clobber(path: &Path, bytes: &[u8]) -> Result<(), MigrationBackupError> {
    let parent = path
        .parent()
        .ok_or_else(|| backup_error("migration-backup-manifest-unsafe"))?;
    let temporary = temporary_path(parent, "manifest");
    let result = (|| {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary)
            .map_err(|_| backup_error("migration-backup-manifest-stage-failed"))?;
        secure_file(&temporary)?;
        file.write_all(bytes)
            .and_then(|()| file.sync_all())
            .map_err(|_| backup_error("migration-backup-manifest-write-failed"))?;
        fs::hard_link(&temporary, path)
            .map_err(|_| backup_error("migration-backup-manifest-commit-failed"))?;
        secure_file(path)?;
        sync_directory(parent)
    })();
    let _ = fs::remove_file(temporary);
    result
}

fn temporary_path(parent: &Path, kind: &str) -> PathBuf {
    let sequence = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    parent.join(format!(
        ".aegisy-migration-{kind}-{}-{sequence}.tmp",
        std::process::id()
    ))
}

#[cfg(unix)]
fn secure_directory(path: &Path) -> Result<(), MigrationBackupError> {
    use std::os::unix::fs::PermissionsExt;
    fs::set_permissions(path, fs::Permissions::from_mode(0o700))
        .map_err(|_| backup_error("migration-backup-permissions-failed"))
}

#[cfg(not(unix))]
fn secure_directory(_path: &Path) -> Result<(), MigrationBackupError> {
    Ok(())
}

#[cfg(unix)]
fn secure_file(path: &Path) -> Result<(), MigrationBackupError> {
    use std::os::unix::fs::PermissionsExt;
    fs::set_permissions(path, fs::Permissions::from_mode(0o600))
        .map_err(|_| backup_error("migration-backup-permissions-failed"))
}

#[cfg(not(unix))]
fn secure_file(_path: &Path) -> Result<(), MigrationBackupError> {
    Ok(())
}

#[cfg(unix)]
fn sync_directory(path: &Path) -> Result<(), MigrationBackupError> {
    File::open(path)
        .and_then(|directory| directory.sync_all())
        .map_err(|_| backup_error("migration-backup-directory-sync-failed"))
}

#[cfg(not(unix))]
fn sync_directory(_path: &Path) -> Result<(), MigrationBackupError> {
    Ok(())
}

fn push_issue(issues: &mut Vec<String>, issue: &str) {
    if !issues.iter().any(|existing| existing == issue) {
        issues.push(issue.into());
    }
}

fn bounded_reason(value: &str) -> String {
    if value.len() <= 128
        && !value.is_empty()
        && value
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
    {
        value.into()
    } else {
        "workbench-store-open-failed".into()
    }
}

fn is_lower_hex(byte: u8) -> bool {
    byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte)
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

fn backup_error(code: &'static str) -> MigrationBackupError {
    MigrationBackupError { code }
}
