use crate::git_workflow_authorization::{
    GitWorkflowAuthorizationRequirement, GitWorkflowDecisionReference,
};
use crate::permission_profile::{
    EffectivePermissionPolicy, ManagedPermissionPolicy, PermissionDecision, PermissionProfile,
};
use crate::workbench_store::{
    GitWorkflowDecisionKind, GitWorkflowDecisionTiming, WorkbenchStore, WorkbenchStoreError,
};

const MAX_ISSUANCE_LIFETIME_MS: u64 = 5 * 60 * 1_000;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PermissionDecisionActor {
    ProfilePolicy {
        authority_id: String,
    },
    UserGesture {
        authority_id: String,
        gesture_id: String,
    },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PermissionIssuerError {
    pub message: String,
}

pub struct GitWorkflowDecisionIssuance<'a> {
    pub requirement: &'a GitWorkflowAuthorizationRequirement,
    pub kind: GitWorkflowDecisionKind,
    pub authority_id: &'a str,
    pub decision_id: &'a str,
    pub timing: GitWorkflowDecisionTiming,
    pub actor: &'a PermissionDecisionActor,
    pub profile: &'a PermissionProfile,
    pub managed: &'a ManagedPermissionPolicy,
}

pub fn issue_into_store(
    store: &mut WorkbenchStore,
    request: GitWorkflowDecisionIssuance<'_>,
) -> Result<GitWorkflowDecisionReference, PermissionIssuerError> {
    authorize(&request)?;
    store
        .issue_git_workflow_decision(
            request.requirement,
            request.kind,
            request.authority_id,
            request.decision_id,
            request.timing,
        )
        .map_err(|cause| PermissionIssuerError {
            message: cause.message,
        })
}

fn authorize(request: &GitWorkflowDecisionIssuance<'_>) -> Result<(), PermissionIssuerError> {
    let effective = EffectivePermissionPolicy::from_profile(request.profile, request.managed)
        .map_err(|cause| issuer_error(cause.message))?;
    let action = match request.requirement.action {
        crate::git_workflow_authorization::GitWorkflowAuthorizedAction::Start => "start",
        crate::git_workflow_authorization::GitWorkflowAuthorizedAction::Abort => "abort",
        crate::git_workflow_authorization::GitWorkflowAuthorizedAction::Continue => "continue",
    };
    if matches!(
        effective.check_git_action(action, &request.requirement.risk.class),
        PermissionDecision::Denied { .. }
    ) {
        return Err(issuer_error(
            "effective permission profile denies Git mutation",
        ));
    }
    if request.timing.issued_at_ms > request.timing.observed_at_ms
        || request.timing.observed_at_ms > request.timing.expires_at_ms
        || request
            .timing
            .expires_at_ms
            .saturating_sub(request.timing.issued_at_ms)
            > MAX_ISSUANCE_LIFETIME_MS
    {
        return Err(issuer_error("decision lifetime is invalid"));
    }
    if request.authority_id.is_empty() || request.decision_id.is_empty() {
        return Err(issuer_error("decision authority and ID are required"));
    }
    match (request.kind, request.actor) {
        (
            GitWorkflowDecisionKind::Permission,
            PermissionDecisionActor::ProfilePolicy { authority_id },
        ) if authority_id == request.authority_id => {}
        (
            GitWorkflowDecisionKind::ExplicitApproval,
            PermissionDecisionActor::UserGesture {
                authority_id,
                gesture_id,
            },
        ) if authority_id == request.authority_id
            && valid_gesture_id(gesture_id)
            && request.requirement.risk.requires_explicit_approval => {}
        (GitWorkflowDecisionKind::Permission, PermissionDecisionActor::UserGesture { .. }) => {
            return Err(issuer_error(
                "permission decisions must come from the profile policy authority",
            ));
        }
        (GitWorkflowDecisionKind::ExplicitApproval, _) => {
            return Err(issuer_error(
                "explicit approval requires a distinct user gesture authority",
            ));
        }
        _ => return Err(issuer_error("decision actor authority does not match")),
    }
    Ok(())
}

fn valid_gesture_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 128
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
}

fn issuer_error(message: impl Into<String>) -> PermissionIssuerError {
    PermissionIssuerError {
        message: message.into(),
    }
}

impl From<WorkbenchStoreError> for PermissionIssuerError {
    fn from(cause: WorkbenchStoreError) -> Self {
        issuer_error(cause.message)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::git_workflow_authorization::{
        GitWorkflowAuthorizationRequirement, GitWorkflowAuthorizedAction,
    };
    use crate::git_workflow_state::GitWorkflowRisk;
    use crate::workspace_edit::ContentHash;
    use std::fs;
    use std::path::{Path, PathBuf};
    use std::sync::atomic::{AtomicU64, Ordering};

    static TEMP_SEQUENCE: AtomicU64 = AtomicU64::new(0);

    struct TempDir(PathBuf);

    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn tempdir() -> TempDir {
        let id = TEMP_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let path = std::env::temp_dir().join(format!(
            "aegisy-permission-issuer-{}-{id}",
            std::process::id()
        ));
        fs::create_dir(&path).unwrap();
        TempDir(path)
    }

    fn requirement() -> GitWorkflowAuthorizationRequirement {
        GitWorkflowAuthorizationRequirement {
            schema_version: "git-workflow-authorization-requirement/0.1".into(),
            requirement_hash: ContentHash::for_bytes(b"requirement"),
            operation_id: "operation-1".into(),
            project_id: "project-1".into(),
            session_id: "session-1".into(),
            root_identity: "root".into(),
            git_common_directory_identity: "common".into(),
            action: GitWorkflowAuthorizedAction::Start,
            operation_kind: "merge".into(),
            generation: 0,
            record_hash: ContentHash::for_bytes(b"record"),
            plan_hash: ContentHash::for_bytes(b"plan"),
            observed_head: "a".repeat(40),
            observed_operation: None,
            risk: GitWorkflowRisk {
                class: "medium".into(),
                reasons: vec!["test".into()],
                requires_permission: true,
                requires_explicit_approval: true,
            },
        }
    }

    fn profile(root: &Path) -> PermissionProfile {
        PermissionProfile::for_project(
            "developer",
            crate::permission_profile::PermissionProfileKind::Developer,
            root,
        )
        .unwrap()
    }

    fn timing() -> GitWorkflowDecisionTiming {
        GitWorkflowDecisionTiming {
            issued_at_ms: 100,
            expires_at_ms: 1_000,
            observed_at_ms: 100,
        }
    }

    #[test]
    fn issuer_requires_profile_policy_and_user_gesture_for_distinct_decisions() {
        let root = tempdir();
        let requirement = requirement();
        let profile = profile(&root.0);
        let managed = ManagedPermissionPolicy::permissive();
        let profile_actor = PermissionDecisionActor::ProfilePolicy {
            authority_id: "profile-authority".into(),
        };
        let user_actor = PermissionDecisionActor::UserGesture {
            authority_id: "user-authority".into(),
            gesture_id: "gesture-1".into(),
        };
        let mut store = WorkbenchStore::open(&root.0).unwrap();
        issue_into_store(
            &mut store,
            GitWorkflowDecisionIssuance {
                requirement: &requirement,
                kind: GitWorkflowDecisionKind::Permission,
                authority_id: "profile-authority",
                decision_id: "permission-1",
                timing: timing(),
                actor: &profile_actor,
                profile: &profile,
                managed: &managed,
            },
        )
        .unwrap();
        issue_into_store(
            &mut store,
            GitWorkflowDecisionIssuance {
                requirement: &requirement,
                kind: GitWorkflowDecisionKind::ExplicitApproval,
                authority_id: "user-authority",
                decision_id: "approval-1",
                timing: timing(),
                actor: &user_actor,
                profile: &profile,
                managed: &managed,
            },
        )
        .unwrap();
        let bad_actor = PermissionDecisionActor::ProfilePolicy {
            authority_id: "profile-authority".into(),
        };
        assert!(issue_into_store(
            &mut store,
            GitWorkflowDecisionIssuance {
                requirement: &requirement,
                kind: GitWorkflowDecisionKind::ExplicitApproval,
                authority_id: "profile-authority",
                decision_id: "approval-2",
                timing: timing(),
                actor: &bad_actor,
                profile: &profile,
                managed: &managed,
            },
        )
        .is_err());
    }

    #[test]
    fn issuer_rejects_read_only_and_managed_denials_before_store_mutation() {
        let root = tempdir();
        let requirement = requirement();
        let profile = PermissionProfile::for_project(
            "read-only",
            crate::permission_profile::PermissionProfileKind::ReadOnly,
            &root.0,
        )
        .unwrap();
        let managed = ManagedPermissionPolicy::permissive();
        let actor = PermissionDecisionActor::ProfilePolicy {
            authority_id: "profile-authority".into(),
        };
        let mut store = WorkbenchStore::open(&root.0).unwrap();
        assert!(issue_into_store(
            &mut store,
            GitWorkflowDecisionIssuance {
                requirement: &requirement,
                kind: GitWorkflowDecisionKind::Permission,
                authority_id: "profile-authority",
                decision_id: "permission-denied",
                timing: timing(),
                actor: &actor,
                profile: &profile,
                managed: &managed,
            },
        )
        .is_err());
        assert!(store
            .read_session_events("session-1", 0, 20)
            .unwrap()
            .is_empty());
    }
}
