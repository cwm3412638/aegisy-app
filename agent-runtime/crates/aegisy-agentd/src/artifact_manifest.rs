use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::fmt;
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::{Path, PathBuf};

const MANIFEST_NAME: &str = "aegisy-agentd.manifest.json";
const MANIFEST_SCHEMA: &str = "aegisy-artifact-manifest/0.1";
const RUNTIME_ID: &str = "aegisy-agentd";
const ADAPTER_ID: &str = "codex-app-server";
const MAX_MANIFEST_BYTES: u64 = 64 * 1024;
const MAX_ARTIFACT_BYTES: u64 = 512 * 1024 * 1024;
const MAX_METADATA_BYTES: usize = 128;
const MAX_RELATIVE_PATH_BYTES: usize = 1024;
const HASH_BUFFER_BYTES: usize = 64 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct VerifiedBundledAdapter {
    runtime_path: PathBuf,
    runtime_identity: FileIdentity,
    path: PathBuf,
    adapter_identity: FileIdentity,
    expected_adapter_version: String,
    manifest_sha256: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct VerifiedArtifact {
    path: PathBuf,
    identity: FileIdentity,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum FileIdentity {
    #[cfg(unix)]
    Unix { device: u64, inode: u64 },
    #[cfg(windows)]
    Windows { volume: u32, file_index: u64 },
}

impl VerifiedBundledAdapter {
    pub(crate) fn path(&self) -> &Path {
        &self.path
    }

    pub(crate) fn reverify(&self) -> Result<(), ArtifactManifestError> {
        let Some(verified) =
            verified_bundled_adapter(&self.runtime_path, &self.expected_adapter_version)?
        else {
            return Err(ArtifactManifestError::new(
                "manifest-missing-after-verification",
            ));
        };
        if verified.manifest_sha256 != self.manifest_sha256 {
            return Err(ArtifactManifestError::new("manifest-identity-mismatch"));
        }
        if verified.runtime_path != self.runtime_path {
            return Err(ArtifactManifestError::artifact(
                "artifact-path-mismatch",
                "runtime",
            ));
        }
        if verified.runtime_identity != self.runtime_identity {
            return Err(ArtifactManifestError::artifact(
                "artifact-identity-mismatch",
                "runtime",
            ));
        }
        if verified.path != self.path {
            return Err(ArtifactManifestError::artifact(
                "artifact-path-mismatch",
                "adapter",
            ));
        }
        if verified.adapter_identity != self.adapter_identity {
            return Err(ArtifactManifestError::artifact(
                "artifact-identity-mismatch",
                "adapter",
            ));
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct ArtifactManifestError {
    code: &'static str,
    artifact: Option<&'static str>,
}

impl ArtifactManifestError {
    fn new(code: &'static str) -> Self {
        Self {
            code,
            artifact: None,
        }
    }

    fn artifact(code: &'static str, artifact: &'static str) -> Self {
        Self {
            code,
            artifact: Some(artifact),
        }
    }

    #[cfg(test)]
    fn code(self) -> &'static str {
        self.code
    }
}

impl fmt::Display for ArtifactManifestError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("artifact manifest verification failed: ")?;
        formatter.write_str(self.code)?;
        if let Some(artifact) = self.artifact {
            formatter.write_str(" (")?;
            formatter.write_str(artifact)?;
            formatter.write_str(")")?;
        }
        Ok(())
    }
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct ArtifactManifestDocument {
    schema_version: String,
    runtime: ArtifactEntry,
    adapter: ArtifactEntry,
}

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
#[serde(deny_unknown_fields)]
struct ArtifactEntry {
    id: String,
    version: String,
    path: String,
    sha256: String,
}

pub(crate) fn verified_bundled_adapter(
    runtime_path: &Path,
    expected_adapter_version: &str,
) -> Result<Option<VerifiedBundledAdapter>, ArtifactManifestError> {
    let runtime_directory = runtime_path
        .parent()
        .ok_or_else(|| ArtifactManifestError::new("runtime-path-invalid"))?;
    let manifest_path = runtime_directory.join(MANIFEST_NAME);
    let manifest_metadata = match fs::symlink_metadata(&manifest_path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(_) => return Err(ArtifactManifestError::new("manifest-unreadable")),
    };
    if metadata_is_link_like(&manifest_metadata) || !manifest_metadata.is_file() {
        return Err(ArtifactManifestError::new("manifest-path-invalid"));
    }
    if manifest_metadata.len() > MAX_MANIFEST_BYTES {
        return Err(ArtifactManifestError::new("manifest-size-limit"));
    }

    let manifest_bytes = read_bounded_file(
        &manifest_path,
        MAX_MANIFEST_BYTES,
        ArtifactManifestError::new("manifest-unreadable"),
        ArtifactManifestError::new("manifest-size-limit"),
    )?;
    let manifest_sha256 = format!("{:x}", Sha256::digest(&manifest_bytes));
    let document: ArtifactManifestDocument =
        serde_json::from_slice(&manifest_bytes).map_err(|error| {
            if error.is_syntax() || error.is_eof() {
                ArtifactManifestError::new("manifest-invalid-json")
            } else {
                ArtifactManifestError::new("manifest-fields-invalid")
            }
        })?;
    if document.schema_version != MANIFEST_SCHEMA {
        return Err(ArtifactManifestError::new("unsupported-schema"));
    }

    let runtime = parse_artifact_entry(document.runtime, RUNTIME_ID, "runtime")?;
    let adapter = parse_artifact_entry(document.adapter, ADAPTER_ID, "adapter")?;
    if runtime.version != env!("CARGO_PKG_VERSION") {
        return Err(ArtifactManifestError::artifact(
            "artifact-version-mismatch",
            "runtime",
        ));
    }
    if adapter.version != expected_adapter_version {
        return Err(ArtifactManifestError::artifact(
            "artifact-version-mismatch",
            "adapter",
        ));
    }

    let base = fs::canonicalize(runtime_directory)
        .map_err(|_| ArtifactManifestError::new("manifest-base-invalid"))?;
    let verified_runtime = verify_artifact(&base, &runtime, Some(runtime_path), "runtime")?;
    let verified_adapter = verify_artifact(&base, &adapter, None, "adapter")?;
    if verified_runtime.path == verified_adapter.path
        || verified_runtime.identity == verified_adapter.identity
    {
        return Err(ArtifactManifestError::new("artifact-path-duplicate"));
    }

    Ok(Some(VerifiedBundledAdapter {
        runtime_path: verified_runtime.path,
        runtime_identity: verified_runtime.identity,
        path: verified_adapter.path,
        adapter_identity: verified_adapter.identity,
        expected_adapter_version: expected_adapter_version.to_owned(),
        manifest_sha256,
    }))
}

fn parse_artifact_entry(
    entry: ArtifactEntry,
    expected_id: &str,
    artifact: &'static str,
) -> Result<ArtifactEntry, ArtifactManifestError> {
    if entry.id != expected_id
        || !valid_printable_metadata(&entry.id)
        || !valid_printable_metadata(&entry.version)
        || !safe_portable_relative_path(&entry.path)
        || (artifact == "adapter" && !valid_bundled_adapter_path(&entry.path))
        || !valid_sha256(&entry.sha256)
    {
        return Err(ArtifactManifestError::artifact(
            "invalid-artifact-entry",
            artifact,
        ));
    }
    Ok(entry)
}

fn valid_bundled_adapter_path(path: &str) -> bool {
    valid_bundled_adapter_path_for_platform(path, cfg!(windows))
}

fn valid_bundled_adapter_path_for_platform(path: &str, windows: bool) -> bool {
    !windows
        || path
            .get(path.len().saturating_sub(4)..)
            .is_some_and(|suffix| suffix.eq_ignore_ascii_case(".exe"))
}

fn valid_printable_metadata(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= MAX_METADATA_BYTES
        && value.bytes().all(|byte| (b' '..=b'~').contains(&byte))
}

fn valid_sha256(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn safe_portable_relative_path(value: &str) -> bool {
    if value.is_empty()
        || value.len() > MAX_RELATIVE_PATH_BYTES
        || value.starts_with('/')
        || value.contains(['\\', ':'])
        || value
            .chars()
            .any(|character| character == '\0' || character.is_control())
    {
        return false;
    }
    value
        .split('/')
        .all(|component| !component.is_empty() && component != "." && component != "..")
}

fn verify_artifact(
    base: &Path,
    entry: &ArtifactEntry,
    expected_path: Option<&Path>,
    artifact: &'static str,
) -> Result<VerifiedArtifact, ArtifactManifestError> {
    let candidate = base.join(&entry.path);
    let mut component_path = base.to_path_buf();
    let mut metadata = None;
    for component in entry.path.split('/') {
        component_path.push(component);
        let component_metadata = fs::symlink_metadata(&component_path)
            .map_err(|_| ArtifactManifestError::artifact("artifact-missing", artifact))?;
        if metadata_is_link_like(&component_metadata) {
            return Err(ArtifactManifestError::artifact(
                "artifact-path-invalid",
                artifact,
            ));
        }
        metadata = Some(component_metadata);
    }
    let metadata = metadata
        .ok_or_else(|| ArtifactManifestError::artifact("artifact-path-invalid", artifact))?;
    if !metadata.is_file() {
        return Err(ArtifactManifestError::artifact(
            "artifact-path-invalid",
            artifact,
        ));
    }
    if metadata.len() > MAX_ARTIFACT_BYTES {
        return Err(ArtifactManifestError::artifact(
            "artifact-size-limit",
            artifact,
        ));
    }
    let canonical = fs::canonicalize(&candidate)
        .map_err(|_| ArtifactManifestError::artifact("artifact-path-invalid", artifact))?;
    if canonical == base || !canonical.starts_with(base) {
        return Err(ArtifactManifestError::artifact(
            "artifact-path-escape",
            artifact,
        ));
    }
    if let Some(expected_path) = expected_path {
        let expected = fs::canonicalize(expected_path)
            .map_err(|_| ArtifactManifestError::artifact("artifact-path-mismatch", artifact))?;
        if expected != canonical {
            return Err(ArtifactManifestError::artifact(
                "artifact-path-mismatch",
                artifact,
            ));
        }
    }

    let (actual_hash, identity) = sha256_bounded_file(&canonical, artifact)?;
    if actual_hash != entry.sha256 {
        return Err(ArtifactManifestError::artifact(
            "artifact-hash-mismatch",
            artifact,
        ));
    }
    Ok(VerifiedArtifact {
        path: canonical,
        identity,
    })
}

fn metadata_is_link_like(metadata: &fs::Metadata) -> bool {
    if metadata.file_type().is_symlink() {
        return true;
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;

        const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;
        metadata.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0
    }
    #[cfg(not(windows))]
    {
        false
    }
}

fn sha256_bounded_file(
    path: &Path,
    artifact: &'static str,
) -> Result<(String, FileIdentity), ArtifactManifestError> {
    let mut file = File::open(path)
        .map_err(|_| ArtifactManifestError::artifact("artifact-read-failed", artifact))?;
    let metadata = file
        .metadata()
        .map_err(|_| ArtifactManifestError::artifact("artifact-read-failed", artifact))?;
    if !metadata.is_file() || metadata.len() > MAX_ARTIFACT_BYTES {
        return Err(ArtifactManifestError::artifact(
            "artifact-size-limit",
            artifact,
        ));
    }
    let (identity, link_count) = file_identity(&file, &metadata, artifact)?;
    if link_count != 1 {
        return Err(ArtifactManifestError::artifact(
            "artifact-hard-link",
            artifact,
        ));
    }
    let mut hash = Sha256::new();
    let mut total_bytes = 0_u64;
    let mut buffer = vec![0_u8; HASH_BUFFER_BYTES];
    loop {
        let bytes_read = file
            .read(&mut buffer)
            .map_err(|_| ArtifactManifestError::artifact("artifact-read-failed", artifact))?;
        if bytes_read == 0 {
            break;
        }
        total_bytes = total_bytes
            .checked_add(u64::try_from(bytes_read).unwrap_or(u64::MAX))
            .ok_or_else(|| ArtifactManifestError::artifact("artifact-size-limit", artifact))?;
        if total_bytes > MAX_ARTIFACT_BYTES {
            return Err(ArtifactManifestError::artifact(
                "artifact-size-limit",
                artifact,
            ));
        }
        hash.update(&buffer[..bytes_read]);
    }
    Ok((format!("{:x}", hash.finalize()), identity))
}

#[cfg(unix)]
fn file_identity(
    _file: &File,
    metadata: &fs::Metadata,
    _artifact: &'static str,
) -> Result<(FileIdentity, u64), ArtifactManifestError> {
    use std::os::unix::fs::MetadataExt;

    Ok((
        FileIdentity::Unix {
            device: metadata.dev(),
            inode: metadata.ino(),
        },
        metadata.nlink(),
    ))
}

#[cfg(windows)]
fn file_identity(
    file: &File,
    _metadata: &fs::Metadata,
    artifact: &'static str,
) -> Result<(FileIdentity, u64), ArtifactManifestError> {
    use std::mem::MaybeUninit;
    use std::os::windows::io::AsRawHandle;
    use windows_sys::Win32::Storage::FileSystem::{
        GetFileInformationByHandle, BY_HANDLE_FILE_INFORMATION,
    };

    let mut information = MaybeUninit::<BY_HANDLE_FILE_INFORMATION>::zeroed();
    let succeeded =
        unsafe { GetFileInformationByHandle(file.as_raw_handle() as _, information.as_mut_ptr()) };
    if succeeded == 0 {
        return Err(ArtifactManifestError::artifact(
            "artifact-identity-unavailable",
            artifact,
        ));
    }
    let information = unsafe { information.assume_init() };
    let file_index =
        (u64::from(information.nFileIndexHigh) << 32) | u64::from(information.nFileIndexLow);
    Ok((
        FileIdentity::Windows {
            volume: information.dwVolumeSerialNumber,
            file_index,
        },
        u64::from(information.nNumberOfLinks),
    ))
}

#[cfg(not(any(unix, windows)))]
fn file_identity(
    _file: &File,
    _metadata: &fs::Metadata,
    artifact: &'static str,
) -> Result<(FileIdentity, u64), ArtifactManifestError> {
    Err(ArtifactManifestError::artifact(
        "artifact-identity-unavailable",
        artifact,
    ))
}

fn read_bounded_file(
    path: &Path,
    maximum_bytes: u64,
    read_error: ArtifactManifestError,
    size_error: ArtifactManifestError,
) -> Result<Vec<u8>, ArtifactManifestError> {
    let file = File::open(path).map_err(|_| read_error)?;
    let mut bytes = Vec::new();
    file.take(maximum_bytes.saturating_add(1))
        .read_to_end(&mut bytes)
        .map_err(|_| read_error)?;
    if u64::try_from(bytes.len()).unwrap_or(u64::MAX) > maximum_bytes {
        return Err(size_error);
    }
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::{
        valid_bundled_adapter_path_for_platform, verified_bundled_adapter, ArtifactManifestError,
        MANIFEST_NAME,
    };
    use serde_json::{json, Value};
    use sha2::{Digest, Sha256};
    use std::fs;
    use std::path::{Path, PathBuf};
    use std::sync::atomic::{AtomicU64, Ordering};

    const PINNED_ADAPTER_VERSION: &str = "codex-cli 0.144.5";
    static NEXT_DIRECTORY: AtomicU64 = AtomicU64::new(1);

    struct TestDirectory(PathBuf);

    impl TestDirectory {
        fn new(label: &str) -> Self {
            let sequence = NEXT_DIRECTORY.fetch_add(1, Ordering::Relaxed);
            let path = std::env::temp_dir().join(format!(
                "aegisy-artifact-manifest-{label}-{}-{sequence}",
                std::process::id()
            ));
            let _ = fs::remove_dir_all(&path);
            fs::create_dir_all(&path).unwrap();
            Self(path)
        }

        fn path(&self) -> &Path {
            &self.0
        }
    }

    impl Drop for TestDirectory {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn sha256(bytes: &[u8]) -> String {
        format!("{:x}", Sha256::digest(bytes))
    }

    fn fixture(label: &str) -> (TestDirectory, PathBuf, PathBuf, Value) {
        let directory = TestDirectory::new(label);
        let runtime_name = if cfg!(windows) {
            "aegisy-agentd.exe"
        } else {
            "aegisy-agentd"
        };
        let adapter_name = if cfg!(windows) { "codex.exe" } else { "codex" };
        let runtime = directory.path().join(runtime_name);
        let adapter = directory.path().join(adapter_name);
        let runtime_bytes = b"runtime fixture";
        let adapter_bytes = b"adapter fixture";
        fs::write(&runtime, runtime_bytes).unwrap();
        fs::write(&adapter, adapter_bytes).unwrap();
        let manifest = json!({
            "schema_version": "aegisy-artifact-manifest/0.1",
            "runtime": {
                "id": "aegisy-agentd",
                "version": env!("CARGO_PKG_VERSION"),
                "path": runtime_name,
                "sha256": sha256(runtime_bytes)
            },
            "adapter": {
                "id": "codex-app-server",
                "version": PINNED_ADAPTER_VERSION,
                "path": adapter_name,
                "sha256": sha256(adapter_bytes)
            }
        });
        (directory, runtime, adapter, manifest)
    }

    fn write_manifest(directory: &TestDirectory, manifest: &Value) {
        fs::write(
            directory.path().join(MANIFEST_NAME),
            serde_json::to_vec_pretty(manifest).unwrap(),
        )
        .unwrap();
    }

    fn error_code(
        result: Result<Option<super::VerifiedBundledAdapter>, ArtifactManifestError>,
    ) -> &'static str {
        result.unwrap_err().code()
    }

    #[test]
    fn missing_manifest_preserves_developer_resolution() {
        let (_directory, runtime, _adapter, _manifest) = fixture("missing");
        assert_eq!(
            verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION).unwrap(),
            None
        );

        let developer = PathBuf::from("developer-codex");
        let selected =
            crate::codex_adapter::resolve_codex_for_runtime(&runtime, Some(developer.clone()))
                .unwrap();
        assert!(matches!(
            selected,
            crate::codex_adapter::CodexExecutable::Developer(path) if path == developer
        ));
    }

    #[test]
    fn valid_manifest_returns_only_the_verified_bundled_adapter() {
        let (directory, runtime, adapter, manifest) = fixture("valid");
        write_manifest(&directory, &manifest);
        let verified = verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)
            .unwrap()
            .unwrap();
        assert_eq!(verified.path(), fs::canonicalize(adapter).unwrap());
        verified.reverify().unwrap();
    }

    #[test]
    fn bundled_manifest_ignores_the_developer_adapter_override() {
        let (directory, runtime, adapter, manifest) = fixture("override");
        write_manifest(&directory, &manifest);
        let unverified_override = directory.path().join("unverified-codex");
        fs::write(&unverified_override, b"unverified override").unwrap();

        let selected =
            crate::codex_adapter::resolve_codex_for_runtime(&runtime, Some(unverified_override))
                .unwrap();
        match selected {
            crate::codex_adapter::CodexExecutable::Bundled(verified) => {
                assert_eq!(verified.path(), fs::canonicalize(adapter).unwrap());
            }
            crate::codex_adapter::CodexExecutable::Developer(_) => {
                panic!("a developer override bypassed the bundled manifest")
            }
        }

        fs::write(directory.path().join(MANIFEST_NAME), b"{}").unwrap();
        let error = crate::codex_adapter::resolve_codex_for_runtime(
            &runtime,
            Some(directory.path().join("unverified-codex")),
        )
        .unwrap_err();
        assert!(error.contains("manifest-fields-invalid"));
    }

    #[test]
    fn adapter_is_reverified_after_the_version_probe() {
        let (directory, runtime, adapter, manifest) = fixture("reverify");
        write_manifest(&directory, &manifest);
        let verified = verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)
            .unwrap()
            .unwrap();
        fs::write(adapter, b"replaced after version probe").unwrap();
        assert_eq!(
            verified.reverify().unwrap_err().code(),
            "artifact-hash-mismatch"
        );
    }

    #[test]
    fn manifest_and_adapter_replacement_cannot_change_one_start_attempt() {
        let (directory, runtime, adapter, mut manifest) = fixture("manifest-replacement");
        write_manifest(&directory, &manifest);
        let verified = verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)
            .unwrap()
            .unwrap();

        let replacement = b"replacement adapter";
        fs::write(adapter, replacement).unwrap();
        manifest["adapter"]["sha256"] = json!(sha256(replacement));
        write_manifest(&directory, &manifest);

        assert_eq!(
            verified.reverify().unwrap_err().code(),
            "manifest-identity-mismatch"
        );
    }

    #[cfg(any(unix, windows))]
    #[test]
    fn byte_identical_adapter_replacement_changes_the_bound_file_identity() {
        let (directory, runtime, adapter, manifest) = fixture("identity-replacement");
        write_manifest(&directory, &manifest);
        let verified = verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)
            .unwrap()
            .unwrap();

        let replacement = directory.path().join("replacement-codex");
        fs::write(&replacement, b"adapter fixture").unwrap();
        fs::remove_file(&adapter).unwrap();
        fs::rename(replacement, adapter).unwrap();

        assert_eq!(
            verified.reverify().unwrap_err().code(),
            "artifact-identity-mismatch"
        );
    }

    #[test]
    fn runtime_and_adapter_hash_drift_fail_closed() {
        let (directory, runtime, adapter, manifest) = fixture("hash");
        write_manifest(&directory, &manifest);
        fs::write(&adapter, b"tampered adapter").unwrap();
        assert_eq!(
            error_code(verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)),
            "artifact-hash-mismatch"
        );

        fs::write(&adapter, b"adapter fixture").unwrap();
        fs::write(&runtime, b"tampered runtime").unwrap();
        assert_eq!(
            error_code(verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)),
            "artifact-hash-mismatch"
        );
    }

    #[test]
    fn exact_runtime_and_adapter_versions_are_required() {
        let (directory, runtime, _adapter, mut manifest) = fixture("versions");
        manifest["runtime"]["version"] = json!("0.1.1");
        write_manifest(&directory, &manifest);
        assert_eq!(
            error_code(verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)),
            "artifact-version-mismatch"
        );

        manifest["runtime"]["version"] = json!(env!("CARGO_PKG_VERSION"));
        write_manifest(&directory, &manifest);
        assert_eq!(
            error_code(verified_bundled_adapter(&runtime, "codex-cli 0.144.6")),
            "artifact-version-mismatch"
        );
    }

    #[test]
    fn unknown_fields_and_non_portable_paths_are_rejected() {
        let (directory, runtime, _adapter, mut manifest) = fixture("shape");
        manifest["unexpected"] = json!(true);
        write_manifest(&directory, &manifest);
        assert_eq!(
            error_code(verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)),
            "manifest-fields-invalid"
        );

        let (_, _, _, clean_manifest) = fixture("shape-source");
        manifest = clean_manifest;
        manifest["adapter"]["path"] = json!("..\\codex");
        write_manifest(&directory, &manifest);
        assert_eq!(
            error_code(verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)),
            "invalid-artifact-entry"
        );
    }

    #[test]
    fn windows_adapter_path_must_name_the_exact_executable_image() {
        assert!(valid_bundled_adapter_path_for_platform("codex.exe", true));
        assert!(valid_bundled_adapter_path_for_platform(
            "bin/CODEX.EXE",
            true
        ));
        assert!(!valid_bundled_adapter_path_for_platform("codex", true));
        assert!(!valid_bundled_adapter_path_for_platform("codex.cmd", true));
        assert!(!valid_bundled_adapter_path_for_platform(
            "codex.exe.backup",
            true
        ));
        assert!(valid_bundled_adapter_path_for_platform("codex", false));
    }

    #[cfg(windows)]
    #[test]
    fn extensionless_adapter_cannot_be_shadowed_by_an_unverified_executable() {
        let (directory, runtime, adapter, mut manifest) = fixture("exe-shadow");
        fs::write(&adapter, b"unverified executable").unwrap();
        let extensionless = directory.path().join("codex");
        let extensionless_bytes = b"manifested non-executable";
        fs::write(&extensionless, extensionless_bytes).unwrap();
        manifest["adapter"]["path"] = json!("codex");
        manifest["adapter"]["sha256"] = json!(sha256(extensionless_bytes));
        write_manifest(&directory, &manifest);

        assert_eq!(
            error_code(verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)),
            "invalid-artifact-entry"
        );
    }

    #[test]
    fn duplicate_manifest_fields_are_rejected() {
        let (directory, runtime, _adapter, manifest) = fixture("duplicate");
        let serialized = serde_json::to_string(&manifest).unwrap();
        let duplicate = serialized.replacen(
            '{',
            "{\"schema_version\":\"aegisy-artifact-manifest/0.1\",",
            1,
        );
        fs::write(directory.path().join(MANIFEST_NAME), duplicate).unwrap();
        assert_eq!(
            error_code(verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)),
            "manifest-fields-invalid"
        );
    }

    #[test]
    fn runtime_binding_and_distinct_artifact_paths_are_required() {
        let (directory, runtime, _adapter, mut manifest) = fixture("binding");
        write_manifest(&directory, &manifest);
        let replacement_runtime = directory.path().join("replacement-agentd");
        fs::write(&replacement_runtime, b"runtime fixture").unwrap();
        assert_eq!(
            error_code(verified_bundled_adapter(
                &replacement_runtime,
                PINNED_ADAPTER_VERSION
            )),
            "artifact-path-mismatch"
        );

        let duplicate_runtime_name = if cfg!(windows) {
            "aegisy-agentd.exe"
        } else {
            "aegisy-agentd"
        };
        manifest["adapter"]["path"] = json!(duplicate_runtime_name);
        manifest["adapter"]["sha256"] = manifest["runtime"]["sha256"].clone();
        write_manifest(&directory, &manifest);
        assert_eq!(
            error_code(verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)),
            "artifact-path-duplicate"
        );
    }

    #[cfg(any(unix, windows))]
    #[test]
    fn runtime_and_adapter_hard_links_are_rejected_as_the_same_file() {
        let (directory, runtime, adapter, mut manifest) = fixture("hard-link");
        fs::remove_file(&adapter).unwrap();
        fs::hard_link(&runtime, &adapter).unwrap();
        manifest["adapter"]["sha256"] = manifest["runtime"]["sha256"].clone();
        write_manifest(&directory, &manifest);

        assert_eq!(
            error_code(verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)),
            "artifact-hard-link"
        );
    }

    #[cfg(any(unix, windows))]
    #[test]
    fn an_extra_hard_link_alias_for_one_artifact_is_rejected() {
        let (directory, runtime, adapter, manifest) = fixture("hard-link-alias");
        fs::hard_link(&adapter, directory.path().join("adapter-alias")).unwrap();
        write_manifest(&directory, &manifest);

        assert_eq!(
            error_code(verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)),
            "artifact-hard-link"
        );
    }

    #[cfg(unix)]
    #[test]
    fn symlinked_manifest_and_adapter_are_rejected() {
        use std::os::unix::fs::symlink;

        let (directory, runtime, adapter, manifest) = fixture("symlink");
        let real_manifest = directory.path().join("real-manifest.json");
        fs::write(&real_manifest, serde_json::to_vec(&manifest).unwrap()).unwrap();
        symlink(&real_manifest, directory.path().join(MANIFEST_NAME)).unwrap();
        assert_eq!(
            error_code(verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)),
            "manifest-path-invalid"
        );

        fs::remove_file(directory.path().join(MANIFEST_NAME)).unwrap();
        fs::remove_file(&adapter).unwrap();
        let real_adapter = directory.path().join("real-codex");
        fs::write(&real_adapter, b"adapter fixture").unwrap();
        symlink(&real_adapter, &adapter).unwrap();
        write_manifest(&directory, &manifest);
        assert_eq!(
            error_code(verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)),
            "artifact-path-invalid"
        );
    }

    #[cfg(unix)]
    #[test]
    fn symlinked_artifact_parent_is_rejected() {
        use std::os::unix::fs::symlink;

        let (directory, runtime, _adapter, mut manifest) = fixture("symlink-parent");
        let real_parent = directory.path().join("real-parent");
        fs::create_dir(&real_parent).unwrap();
        fs::write(real_parent.join("codex"), b"adapter fixture").unwrap();
        symlink(&real_parent, directory.path().join("linked-parent")).unwrap();
        manifest["adapter"]["path"] = json!("linked-parent/codex");
        write_manifest(&directory, &manifest);

        assert_eq!(
            error_code(verified_bundled_adapter(&runtime, PINNED_ADAPTER_VERSION)),
            "artifact-path-invalid"
        );
    }
}
