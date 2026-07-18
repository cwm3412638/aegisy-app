use crate::git_workflow_state::{
    validate_record, GitWorkflowPlan, GitWorkflowRecord, GitWorkflowRequest, GitWorkflowRisk,
};
use crate::workspace_edit::ContentHash;
use serde::{Deserialize, Serialize};

const REQUIREMENT_SCHEMA_VERSION: &str = "git-workflow-authorization-requirement/0.1";
const EVIDENCE_SCHEMA_VERSION: &str = "git-workflow-authorization-evidence/0.1";
const MAX_AUTHORIZATION_LIFETIME_MS: u64 = 5 * 60 * 1_000;

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum GitWorkflowAuthorizedAction {
    Start,
    Abort,
    Continue,
}

impl GitWorkflowAuthorizedAction {
    fn as_str(&self) -> &'static str {
        match self {
            Self::Start => "start",
            Self::Abort => "abort",
            Self::Continue => "continue",
        }
    }
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct GitWorkflowAuthorizationRequirement {
    pub schema_version: String,
    pub requirement_hash: ContentHash,
    pub operation_id: String,
    pub project_id: String,
    pub session_id: String,
    pub root_identity: String,
    pub git_common_directory_identity: String,
    pub action: GitWorkflowAuthorizedAction,
    pub operation_kind: String,
    pub generation: u64,
    pub record_hash: ContentHash,
    pub plan_hash: ContentHash,
    pub observed_head: String,
    pub observed_operation: Option<String>,
    pub risk: GitWorkflowRisk,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GitWorkflowAuthorizationEvidence {
    pub schema_version: String,
    pub authorization_id: String,
    pub requirement_hash: ContentHash,
    pub permission: GitWorkflowDecisionReference,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub explicit_approval: Option<GitWorkflowDecisionReference>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct GitWorkflowDecisionReference {
    pub authority_id: String,
    pub decision_id: String,
    pub scope: String,
    pub scope_hash: ContentHash,
    pub issued_at_ms: u64,
    pub expires_at_ms: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GitWorkflowAuthorizationError {
    pub message: String,
}

pub trait GitWorkflowAuthorizationAuthority {
    fn consume_once(
        &mut self,
        requirement: &GitWorkflowAuthorizationRequirement,
        evidence: &GitWorkflowAuthorizationEvidence,
        observed_at_ms: u64,
    ) -> Result<(), GitWorkflowAuthorizationError>;
}

#[derive(Debug)]
pub struct VerifiedGitWorkflowAuthorization {
    requirement: GitWorkflowAuthorizationRequirement,
    authorization_id: String,
    expires_at_ms: u64,
}

#[derive(Serialize)]
struct RequirementBinding<'a> {
    schema_version: &'a str,
    operation_id: &'a str,
    project_id: &'a str,
    session_id: &'a str,
    root_identity: &'a str,
    git_common_directory_identity: &'a str,
    action: &'a GitWorkflowAuthorizedAction,
    operation_kind: &'a str,
    generation: u64,
    record_hash: &'a ContentHash,
    plan_hash: &'a ContentHash,
    observed_head: &'a str,
    observed_operation: Option<&'a str>,
    risk: &'a GitWorkflowRisk,
}

pub fn authorization_requirement(
    record: &GitWorkflowRecord,
    plan: &GitWorkflowPlan,
    action: GitWorkflowAuthorizedAction,
) -> Result<GitWorkflowAuthorizationRequirement, GitWorkflowAuthorizationError> {
    validate_record(record).map_err(|cause| error(cause.message))?;
    validate_action_plan(record, plan, action)?;
    if !record
        .allowed_actions
        .iter()
        .any(|allowed| allowed == action.as_str())
    {
        return Err(error(
            "Git workflow action is not allowed by the current record",
        ));
    }
    if !record.risk.requires_permission {
        return Err(error(
            "Git workflow record is missing its permission requirement",
        ));
    }
    let record_bytes = serde_json::to_vec(record)
        .map_err(|_| error("cannot serialize Git workflow authorization record"))?;
    let record_hash = ContentHash::for_bytes(&record_bytes);
    let plan_bytes = serde_json::to_vec(plan)
        .map_err(|_| error("cannot serialize Git workflow authorization plan"))?;
    let plan_hash = ContentHash::for_bytes(&plan_bytes);
    let binding = RequirementBinding {
        schema_version: REQUIREMENT_SCHEMA_VERSION,
        operation_id: &record.operation_id,
        project_id: &record.project_id,
        session_id: &record.session_id,
        root_identity: &record.root_identity,
        git_common_directory_identity: &record.git_common_directory_identity,
        action: &action,
        operation_kind: &record.operation_kind,
        generation: record.generation,
        record_hash: &record_hash,
        plan_hash: &plan_hash,
        observed_head: &record.observed_head,
        observed_operation: record.observed_operation.as_deref(),
        risk: &plan.risk,
    };
    let binding_bytes = serde_json::to_vec(&binding)
        .map_err(|_| error("cannot serialize Git workflow authorization requirement"))?;
    Ok(GitWorkflowAuthorizationRequirement {
        schema_version: REQUIREMENT_SCHEMA_VERSION.into(),
        requirement_hash: ContentHash::for_bytes(&binding_bytes),
        operation_id: record.operation_id.clone(),
        project_id: record.project_id.clone(),
        session_id: record.session_id.clone(),
        root_identity: record.root_identity.clone(),
        git_common_directory_identity: record.git_common_directory_identity.clone(),
        action,
        operation_kind: record.operation_kind.clone(),
        generation: record.generation,
        record_hash,
        plan_hash,
        observed_head: record.observed_head.clone(),
        observed_operation: record.observed_operation.clone(),
        risk: plan.risk.clone(),
    })
}

pub fn verify_git_workflow_authorization<A: GitWorkflowAuthorizationAuthority>(
    authority: &mut A,
    requirement: GitWorkflowAuthorizationRequirement,
    evidence: &GitWorkflowAuthorizationEvidence,
    observed_at_ms: u64,
) -> Result<VerifiedGitWorkflowAuthorization, GitWorkflowAuthorizationError> {
    validate_requirement(&requirement)?;
    validate_evidence(&requirement, evidence, observed_at_ms)?;
    authority.consume_once(&requirement, evidence, observed_at_ms)?;
    let expires_at_ms =
        evidence
            .explicit_approval
            .as_ref()
            .map_or(evidence.permission.expires_at_ms, |approval| {
                approval
                    .expires_at_ms
                    .min(evidence.permission.expires_at_ms)
            });
    Ok(VerifiedGitWorkflowAuthorization {
        requirement,
        authorization_id: evidence.authorization_id.clone(),
        expires_at_ms,
    })
}

impl VerifiedGitWorkflowAuthorization {
    pub fn authorization_id(&self) -> &str {
        &self.authorization_id
    }

    pub fn requirement(&self) -> &GitWorkflowAuthorizationRequirement {
        &self.requirement
    }

    pub fn revalidate(
        &self,
        record: &GitWorkflowRecord,
        plan: &GitWorkflowPlan,
        action: GitWorkflowAuthorizedAction,
        observed_at_ms: u64,
    ) -> Result<(), GitWorkflowAuthorizationError> {
        if observed_at_ms > self.expires_at_ms {
            return Err(error("Git workflow authorization has expired"));
        }
        let current = authorization_requirement(record, plan, action)?;
        if current != self.requirement {
            return Err(error("Git workflow authorization scope is stale"));
        }
        Ok(())
    }
}

fn validate_requirement(
    requirement: &GitWorkflowAuthorizationRequirement,
) -> Result<(), GitWorkflowAuthorizationError> {
    if requirement.schema_version != REQUIREMENT_SCHEMA_VERSION
        || !valid_hash(&requirement.requirement_hash)
        || !valid_hash(&requirement.record_hash)
        || !valid_hash(&requirement.plan_hash)
        || !requirement.risk.requires_permission
    {
        return Err(error("Git workflow authorization requirement is invalid"));
    }
    let binding = RequirementBinding {
        schema_version: REQUIREMENT_SCHEMA_VERSION,
        operation_id: &requirement.operation_id,
        project_id: &requirement.project_id,
        session_id: &requirement.session_id,
        root_identity: &requirement.root_identity,
        git_common_directory_identity: &requirement.git_common_directory_identity,
        action: &requirement.action,
        operation_kind: &requirement.operation_kind,
        generation: requirement.generation,
        record_hash: &requirement.record_hash,
        plan_hash: &requirement.plan_hash,
        observed_head: &requirement.observed_head,
        observed_operation: requirement.observed_operation.as_deref(),
        risk: &requirement.risk,
    };
    let bytes = serde_json::to_vec(&binding)
        .map_err(|_| error("cannot serialize Git workflow authorization requirement"))?;
    if ContentHash::for_bytes(&bytes) != requirement.requirement_hash {
        return Err(error(
            "Git workflow authorization requirement hash is invalid",
        ));
    }
    Ok(())
}

fn validate_action_plan(
    record: &GitWorkflowRecord,
    plan: &GitWorkflowPlan,
    action: GitWorkflowAuthorizedAction,
) -> Result<(), GitWorkflowAuthorizationError> {
    if plan.schema_version != "git-workflow-plan/0.2"
        || !plan.blocking_reasons.is_empty()
        || plan.repository_root != record.repository_root
        || plan.root_identity != record.root_identity
        || plan.git_common_directory_identity != record.git_common_directory_identity
        || plan.operation_kind != record.operation_kind
        || plan.expected_head != record.observed_head
        || plan.expected_branch != record.observed_branch
        || plan.live_operation != record.observed_operation
    {
        return Err(error("Git workflow authorization plan binding is invalid"));
    }
    let request_matches = match (&action, &plan.request) {
        (GitWorkflowAuthorizedAction::Start, request) => {
            request == &record.request
                && plan.expected_index_tree == record.base_index_tree
                && plan.expected_index_state == record.base_index_state
                && plan.target_oids == record.target_oids
                && plan.predicted_behavior == record.predicted_behavior
                && plan.base_stash_oid == record.base_stash_oid
                && plan.risk == record.risk
        }
        (
            GitWorkflowAuthorizedAction::Abort,
            GitWorkflowRequest::Abort {
                operation_id,
                generation,
            },
        )
        | (
            GitWorkflowAuthorizedAction::Continue,
            GitWorkflowRequest::Continue {
                operation_id,
                generation,
            },
        ) => operation_id == &record.operation_id && *generation == record.generation,
        _ => false,
    };
    if !request_matches {
        return Err(error("Git workflow action plan does not match its record"));
    }
    Ok(())
}

fn validate_evidence(
    requirement: &GitWorkflowAuthorizationRequirement,
    evidence: &GitWorkflowAuthorizationEvidence,
    observed_at_ms: u64,
) -> Result<(), GitWorkflowAuthorizationError> {
    validate_identifier(&evidence.authorization_id, "authorization ID")?;
    if evidence.schema_version != EVIDENCE_SCHEMA_VERSION
        || evidence.requirement_hash != requirement.requirement_hash
    {
        return Err(error(
            "Git workflow authorization evidence is not bound to the requirement",
        ));
    }
    validate_decision(
        &evidence.permission,
        &requirement.requirement_hash,
        observed_at_ms,
    )?;
    match (
        requirement.risk.requires_explicit_approval,
        evidence.explicit_approval.as_ref(),
    ) {
        (true, Some(approval)) => {
            validate_decision(approval, &requirement.requirement_hash, observed_at_ms)?;
            if approval.decision_id == evidence.permission.decision_id
                && approval.authority_id == evidence.permission.authority_id
            {
                return Err(error("permission and approval decisions must be distinct"));
            }
        }
        (true, None) => return Err(error("Git workflow requires explicit approval evidence")),
        (false, Some(_)) => {
            return Err(error(
                "unexpected explicit approval evidence for Git workflow",
            ));
        }
        (false, None) => {}
    }
    Ok(())
}

fn validate_decision(
    decision: &GitWorkflowDecisionReference,
    expected_scope_hash: &ContentHash,
    observed_at_ms: u64,
) -> Result<(), GitWorkflowAuthorizationError> {
    validate_identifier(&decision.authority_id, "authorization authority ID")?;
    validate_identifier(&decision.decision_id, "authorization decision ID")?;
    if decision.scope != "allow-once"
        || decision.scope_hash != *expected_scope_hash
        || decision.issued_at_ms > observed_at_ms
        || observed_at_ms > decision.expires_at_ms
        || decision.expires_at_ms.saturating_sub(decision.issued_at_ms)
            > MAX_AUTHORIZATION_LIFETIME_MS
    {
        return Err(error(
            "Git workflow authorization decision is invalid or expired",
        ));
    }
    Ok(())
}

fn validate_identifier(value: &str, label: &str) -> Result<(), GitWorkflowAuthorizationError> {
    if value.is_empty()
        || value.len() > 128
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
    {
        return Err(error(format!("{label} is invalid")));
    }
    Ok(())
}

fn valid_hash(hash: &ContentHash) -> bool {
    hash.sha256.len() == 64
        && hash
            .sha256
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
}

fn error(message: impl Into<String>) -> GitWorkflowAuthorizationError {
    GitWorkflowAuthorizationError {
        message: message.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::git_commit_transaction::{
        GitCommitHookPolicy, GitCommitIdentity, GitCommitMessageSource, GitCommitSigningPolicy,
    };
    use crate::git_workflow_state::{
        GitMergeMode, GitWorkflowCommitMetadata, GitWorkflowHazards, GitWorkflowPlan,
        GitWorkflowRequest, GitWorkflowRisk,
    };
    use std::collections::HashSet;

    #[derive(Default)]
    struct OnceAuthority {
        consumed: HashSet<String>,
    }

    impl GitWorkflowAuthorizationAuthority for OnceAuthority {
        fn consume_once(
            &mut self,
            requirement: &GitWorkflowAuthorizationRequirement,
            evidence: &GitWorkflowAuthorizationEvidence,
            _observed_at_ms: u64,
        ) -> Result<(), GitWorkflowAuthorizationError> {
            let mut keys = vec![format!("authorization:{}", evidence.authorization_id)];
            keys.push(format!(
                "decision:{}:{}",
                evidence.permission.authority_id, evidence.permission.decision_id
            ));
            if let Some(approval) = &evidence.explicit_approval {
                keys.push(format!(
                    "decision:{}:{}",
                    approval.authority_id, approval.decision_id
                ));
            }
            if evidence.requirement_hash != requirement.requirement_hash
                || keys.iter().any(|key| self.consumed.contains(key))
            {
                return Err(error(
                    "trusted authority rejected or already consumed authorization",
                ));
            }
            self.consumed.extend(keys);
            Ok(())
        }
    }

    fn hash() -> ContentHash {
        ContentHash::for_bytes(b"index")
    }

    fn record(high_risk: bool) -> GitWorkflowRecord {
        let oid = "a".repeat(40);
        GitWorkflowRecord {
            schema_version: "git-workflow-record/0.2".into(),
            operation_id: "operation-1".into(),
            project_id: "project-1".into(),
            session_id: "session-1".into(),
            repository_root: "/tmp/aegisy-authorization-repository".into(),
            root_identity: format!("git-root:sha256:{}", "b".repeat(64)),
            git_common_directory_identity: format!("git-root:sha256:{}", "c".repeat(64)),
            request: GitWorkflowRequest::Merge {
                target_oid: "d".repeat(40),
                mode: GitMergeMode::NoFastForward,
                commit: Some(GitWorkflowCommitMetadata {
                    message: "Merge reviewed target".into(),
                    message_source: GitCommitMessageSource::User,
                    identity: GitCommitIdentity {
                        name: "Aegisy Test".into(),
                        email: "aegisy@example.invalid".into(),
                        source: "explicit".into(),
                        timestamp_seconds: 1_800_000_000,
                        timezone: "+0800".into(),
                    },
                }),
                hook_policy: GitCommitHookPolicy::Disabled,
                signing_policy: GitCommitSigningPolicy::Unsigned,
            },
            operation_kind: "merge".into(),
            risk: GitWorkflowRisk {
                class: if high_risk { "high" } else { "medium" }.into(),
                reasons: vec!["test-risk".into()],
                requires_permission: true,
                requires_explicit_approval: true,
            },
            base_head: oid.clone(),
            base_branch: Some("topic".into()),
            base_index_tree: Some("e".repeat(40)),
            base_index_state: hash(),
            target_oids: vec!["d".repeat(40)],
            predicted_behavior: "merge-commit".into(),
            base_stash_oid: None,
            state: "planned".into(),
            generation: 0,
            visible_dirty_paths: Vec::new(),
            redacted_dirty_path_count: 0,
            conflicts: Vec::new(),
            redacted_conflict_path_count: 0,
            allowed_actions: vec!["start".into(), "cancel".into()],
            created_at_ms: 10,
            updated_at_ms: 10,
            observed_head: oid,
            observed_branch: Some("topic".into()),
            observed_operation: None,
            execution: None,
        }
    }

    fn plan(record: &GitWorkflowRecord) -> GitWorkflowPlan {
        GitWorkflowPlan {
            schema_version: "git-workflow-plan/0.2".into(),
            repository_root: record.repository_root.clone(),
            root_identity: record.root_identity.clone(),
            git_common_directory_identity: record.git_common_directory_identity.clone(),
            request: record.request.clone(),
            operation_kind: record.operation_kind.clone(),
            expected_head: record.observed_head.clone(),
            expected_branch: record.observed_branch.clone(),
            expected_index_tree: record.base_index_tree.clone(),
            expected_index_state: record.base_index_state.clone(),
            target_oids: record.target_oids.clone(),
            predicted_behavior: record.predicted_behavior.clone(),
            base_stash_oid: record.base_stash_oid.clone(),
            visible_dirty_paths: record.visible_dirty_paths.clone(),
            redacted_dirty_path_count: record.redacted_dirty_path_count,
            pending_editor_path_count: 0,
            live_operation: record.observed_operation.clone(),
            risk: record.risk.clone(),
            hazards: GitWorkflowHazards {
                active_hooks: Vec::new(),
                custom_hooks_path: false,
                custom_merge_driver: false,
                custom_filter_driver: false,
                protected_branch: false,
            },
            blocking_reasons: Vec::new(),
        }
    }

    fn abort_plan(record: &GitWorkflowRecord) -> GitWorkflowPlan {
        let mut plan = plan(record);
        plan.request = GitWorkflowRequest::Abort {
            operation_id: record.operation_id.clone(),
            generation: record.generation,
        };
        plan.target_oids.clear();
        plan.predicted_behavior = "abort-merge".into();
        plan.risk = GitWorkflowRisk {
            class: "high".into(),
            reasons: vec!["may-discard-conflict-resolutions".into()],
            requires_permission: true,
            requires_explicit_approval: true,
        };
        plan
    }

    fn decision(
        id: &str,
        requirement_hash: &ContentHash,
        now: u64,
    ) -> GitWorkflowDecisionReference {
        GitWorkflowDecisionReference {
            authority_id: "permission-engine".into(),
            decision_id: id.into(),
            scope: "allow-once".into(),
            scope_hash: requirement_hash.clone(),
            issued_at_ms: now - 1_000,
            expires_at_ms: now + 60_000,
        }
    }

    fn evidence(
        requirement: &GitWorkflowAuthorizationRequirement,
        now: u64,
    ) -> GitWorkflowAuthorizationEvidence {
        GitWorkflowAuthorizationEvidence {
            schema_version: EVIDENCE_SCHEMA_VERSION.into(),
            authorization_id: "authorization-1".into(),
            requirement_hash: requirement.requirement_hash.clone(),
            permission: decision("permission-1", &requirement.requirement_hash, now),
            explicit_approval: requirement
                .risk
                .requires_explicit_approval
                .then(|| decision("approval-1", &requirement.requirement_hash, now)),
        }
    }

    #[test]
    fn requirement_binds_record_action_generation_and_observed_state() {
        let original = record(false);
        let original_plan = plan(&original);
        let start = authorization_requirement(
            &original,
            &original_plan,
            GitWorkflowAuthorizedAction::Start,
        )
        .unwrap();
        assert_eq!(start.operation_id, original.operation_id);
        assert_eq!(start.generation, 0);
        assert_eq!(start.action, GitWorkflowAuthorizedAction::Start);
        assert!(valid_hash(&start.requirement_hash));

        let mut changed = original.clone();
        changed.generation = 1;
        changed.updated_at_ms = 11;
        let changed_plan = plan(&changed);
        let changed_requirement =
            authorization_requirement(&changed, &changed_plan, GitWorkflowAuthorizedAction::Start)
                .unwrap();
        assert_ne!(start.requirement_hash, changed_requirement.requirement_hash);
        assert!(authorization_requirement(
            &original,
            &original_plan,
            GitWorkflowAuthorizedAction::Abort,
        )
        .is_err());
    }

    #[test]
    fn verifies_once_and_rejects_replay_or_stale_record() {
        let now = 1_000_000;
        let original = record(false);
        let original_plan = plan(&original);
        let requirement = authorization_requirement(
            &original,
            &original_plan,
            GitWorkflowAuthorizedAction::Start,
        )
        .unwrap();
        let evidence = evidence(&requirement, now);
        let mut authority = OnceAuthority::default();
        let verified =
            verify_git_workflow_authorization(&mut authority, requirement.clone(), &evidence, now)
                .unwrap();
        assert_eq!(verified.authorization_id(), "authorization-1");
        verified
            .revalidate(
                &original,
                &original_plan,
                GitWorkflowAuthorizedAction::Start,
                now + 1,
            )
            .unwrap();
        let mut replay = evidence.clone();
        replay.authorization_id = "authorization-2".into();
        assert!(
            verify_git_workflow_authorization(&mut authority, requirement, &replay, now,).is_err()
        );

        let mut changed = original;
        changed.generation = 1;
        changed.updated_at_ms = 11;
        let changed_plan = plan(&changed);
        assert!(verified
            .revalidate(
                &changed,
                &changed_plan,
                GitWorkflowAuthorizedAction::Start,
                now + 1,
            )
            .is_err());
    }

    #[test]
    fn high_risk_requires_distinct_short_lived_explicit_approval() {
        let now = 2_000_000;
        let mut high_record = record(false);
        high_record.state = "conflicted".into();
        high_record.generation = 3;
        high_record.updated_at_ms = 11;
        high_record.allowed_actions = vec!["resolve".into(), "abort".into()];
        high_record.observed_operation = Some("merge".into());
        let requirement = authorization_requirement(
            &high_record,
            &abort_plan(&high_record),
            GitWorkflowAuthorizedAction::Abort,
        )
        .unwrap();
        assert_eq!(requirement.risk.class, "high");
        let mut authority = OnceAuthority::default();
        let mut candidate = evidence(&requirement, now);
        candidate.explicit_approval = None;
        assert!(verify_git_workflow_authorization(
            &mut authority,
            requirement.clone(),
            &candidate,
            now,
        )
        .is_err());

        candidate = evidence(&requirement, now);
        candidate.explicit_approval = Some(candidate.permission.clone());
        assert!(verify_git_workflow_authorization(
            &mut authority,
            requirement.clone(),
            &candidate,
            now,
        )
        .is_err());

        candidate = evidence(&requirement, now);
        candidate.permission.expires_at_ms = now + MAX_AUTHORIZATION_LIFETIME_MS + 1;
        assert!(
            verify_git_workflow_authorization(&mut authority, requirement, &candidate, now,)
                .is_err()
        );
    }

    #[test]
    fn rejects_forged_requirement_hash_and_expired_evidence() {
        let now = 3_000_000;
        let first_record = record(false);
        let mut requirement = authorization_requirement(
            &first_record,
            &plan(&first_record),
            GitWorkflowAuthorizedAction::Start,
        )
        .unwrap();
        let forged_evidence = evidence(&requirement, now);
        requirement.observed_head = "f".repeat(40);
        let mut authority = OnceAuthority::default();
        assert!(verify_git_workflow_authorization(
            &mut authority,
            requirement,
            &forged_evidence,
            now,
        )
        .is_err());

        let second_record = record(false);
        let requirement = authorization_requirement(
            &second_record,
            &plan(&second_record),
            GitWorkflowAuthorizedAction::Start,
        )
        .unwrap();
        let mut expired = evidence(&requirement, now);
        expired.permission.expires_at_ms = now - 1;
        assert!(
            verify_git_workflow_authorization(&mut authority, requirement, &expired, now,).is_err()
        );
    }
}
