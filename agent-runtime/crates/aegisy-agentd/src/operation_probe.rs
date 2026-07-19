use crate::git_status::status as git_status;
use crate::operation_reconciliation::{GitState, WorkspaceState};
use crate::workspace::collect_search_candidates;
use sha2::{Digest, Sha256};
use std::path::Path;

pub const SCHEMA_VERSION: &str = "operation-reconciliation-probe/0.1";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorkspaceProbe {
    pub state: WorkspaceState,
    pub snapshot_hash: Option<String>,
    pub truncated: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GitProbe {
    pub state: GitState,
    pub snapshot_hash: Option<String>,
    pub truncated: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProbeError {
    pub message: String,
}

pub fn workspace(
    root: &Path,
    expected_snapshot_hash: Option<&str>,
    required: bool,
) -> Result<WorkspaceProbe, ProbeError> {
    let Ok((mut candidates, truncated)) = collect_search_candidates(root) else {
        return Ok(WorkspaceProbe {
            state: WorkspaceState::Unavailable,
            snapshot_hash: None,
            truncated: false,
        });
    };
    candidates.sort_by(|left, right| left.path.cmp(&right.path));
    let snapshot_hash = digest_workspace(&candidates, truncated);
    let state = match expected_snapshot_hash {
        Some(expected) if expected == snapshot_hash => WorkspaceState::Unchanged {
            snapshot_hash: snapshot_hash.clone(),
        },
        Some(_) => WorkspaceState::Changed {
            snapshot_hash: snapshot_hash.clone(),
        },
        None if required => WorkspaceState::NotObserved,
        None => WorkspaceState::NotRequired,
    };
    Ok(WorkspaceProbe {
        state,
        snapshot_hash: Some(snapshot_hash),
        truncated,
    })
}

pub fn git(
    root: &Path,
    expected_snapshot_hash: Option<&str>,
    required: bool,
) -> Result<GitProbe, ProbeError> {
    let Ok(snapshot) = git_status(root) else {
        return Ok(GitProbe {
            state: GitState::Unavailable,
            snapshot_hash: None,
            truncated: false,
        });
    };
    if !snapshot.available || !snapshot.repository {
        return Ok(GitProbe {
            state: if required {
                GitState::Unavailable
            } else {
                GitState::NotRequired
            },
            snapshot_hash: None,
            truncated: snapshot.truncated,
        });
    }
    let snapshot_hash = digest_git(&snapshot)?;
    let state = if snapshot.operation_in_progress.is_some() {
        GitState::OperationInProgress
    } else {
        match expected_snapshot_hash {
            Some(expected) if expected == snapshot_hash => GitState::Unchanged {
                snapshot_hash: snapshot_hash.clone(),
            },
            Some(_) => GitState::Changed {
                snapshot_hash: snapshot_hash.clone(),
            },
            None if required => GitState::NotObserved,
            None => GitState::NotRequired,
        }
    };
    Ok(GitProbe {
        state,
        snapshot_hash: Some(snapshot_hash),
        truncated: snapshot.truncated,
    })
}

fn digest_workspace(candidates: &[crate::workspace::SearchCandidate], truncated: bool) -> String {
    let mut hasher = Sha256::new();
    hasher.update(SCHEMA_VERSION.as_bytes());
    hasher.update(b"/workspace\0");
    hasher.update([u8::from(truncated)]);
    for candidate in candidates {
        update_text(&mut hasher, &candidate.path);
        hasher.update(candidate.size.to_le_bytes());
        update_text(&mut hasher, &candidate.revision);
    }
    format!("sha256:{:x}", hasher.finalize())
}

fn digest_git(snapshot: &crate::git_status::GitStatusSnapshot) -> Result<String, ProbeError> {
    let bytes = serde_json::to_vec(snapshot).map_err(|_| error("Git probe hash failed"))?;
    let mut hasher = Sha256::new();
    hasher.update(SCHEMA_VERSION.as_bytes());
    hasher.update(b"/git\0");
    hasher.update(bytes);
    Ok(format!("sha256:{:x}", hasher.finalize()))
}

fn update_text(hasher: &mut Sha256, value: &str) {
    hasher.update((value.len() as u64).to_le_bytes());
    hasher.update(value.as_bytes());
}

fn error(message: &str) -> ProbeError {
    ProbeError {
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_root(label: &str) -> std::path::PathBuf {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = std::env::temp_dir().join(format!("aegisy-operation-probe-{label}-{unique}"));
        fs::create_dir_all(&path).unwrap();
        path
    }

    #[test]
    fn workspace_probe_is_stable_and_detects_revision_changes() {
        let root = temp_root("workspace");
        fs::write(root.join("main.rs"), "fn main() {}\n").unwrap();
        let first = workspace(&root, None, true).unwrap();
        assert!(matches!(first.state, WorkspaceState::NotObserved));
        let hash = first.snapshot_hash.clone().unwrap();
        let same = workspace(&root, Some(&hash), true).unwrap();
        assert!(matches!(same.state, WorkspaceState::Unchanged { .. }));
        fs::write(
            root.join("main.rs"),
            "fn main() { println!(\"changed\"); }\n",
        )
        .unwrap();
        let changed = workspace(&root, Some(&hash), true).unwrap();
        assert!(matches!(changed.state, WorkspaceState::Changed { .. }));
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn non_repository_is_not_required_for_non_git_operations() {
        let root = temp_root("non-git");
        let probe = git(&root, None, false).unwrap();
        assert!(matches!(probe.state, GitState::NotRequired));
        assert!(probe.snapshot_hash.is_none());
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn non_repository_blocks_required_git_operation() {
        let root = temp_root("required-git");
        let probe = git(&root, None, true).unwrap();
        assert!(matches!(probe.state, GitState::Unavailable));
        let _ = fs::remove_dir_all(root);
    }
}
