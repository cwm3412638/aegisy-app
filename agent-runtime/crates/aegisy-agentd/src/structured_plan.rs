//! Bounded, content-free plan state used by later interactive and child-task flows.
//!
//! This module deliberately has no executor or persistence bridge. It validates
//! the plan graph and can mark steps stale when the reviewed base or evidence
//! revision changes before a future owner adds an AAP/UI mutation surface.

use crate::output_redaction::redact_complete;
use serde::{Deserialize, Serialize};
use serde_json::to_vec;
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};

pub const SCHEMA_VERSION: &str = "structured-plan/0.1";
pub const MAX_STEPS: usize = 128;
pub const MAX_DEPENDENCIES_PER_STEP: usize = 16;
pub const MAX_EVIDENCE_PER_STEP: usize = 16;
const MAX_ID_CHARS: usize = 128;
const MAX_TEXT_CHARS: usize = 1_024;
const MAX_REVISION_CHARS: usize = 256;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PlanError {
    pub code: &'static str,
    pub message: String,
}

impl PlanError {
    fn new(code: &'static str, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PlanStepStatus {
    Pending,
    InProgress,
    Completed,
    Blocked,
    Cancelled,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PlanEvidence {
    pub kind: String,
    pub identity: String,
    pub revision: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PlanStep {
    pub id: String,
    pub status: PlanStepStatus,
    pub explanation: String,
    pub owner: String,
    #[serde(default)]
    pub dependencies: Vec<String>,
    #[serde(default)]
    pub evidence: Vec<PlanEvidence>,
    #[serde(default)]
    pub base_revision: Option<String>,
    #[serde(default)]
    pub stale: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct StructuredPlan {
    pub schema_version: String,
    pub plan_id: String,
    pub session_id: String,
    #[serde(default)]
    pub project_id: Option<String>,
    pub revision: String,
    pub steps: Vec<PlanStep>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RevalidationResult {
    pub current_revision: String,
    pub stale_step_ids: Vec<String>,
}

impl StructuredPlan {
    pub fn validate(&self) -> Result<(), PlanError> {
        if self.schema_version != SCHEMA_VERSION {
            return Err(PlanError::new(
                "structured-plan-schema-unsupported",
                "structured plan schema is unsupported",
            ));
        }
        validate_identifier(&self.plan_id, "structured-plan-id-invalid")?;
        validate_identifier(&self.session_id, "structured-plan-session-invalid")?;
        if let Some(project_id) = self.project_id.as_deref() {
            validate_identifier(project_id, "structured-plan-project-invalid")?;
        }
        validate_revision(&self.revision, "structured-plan-revision-invalid")?;
        if self.steps.is_empty() || self.steps.len() > MAX_STEPS {
            return Err(PlanError::new(
                "structured-plan-step-limit",
                format!("structured plan must contain 1..{MAX_STEPS} steps"),
            ));
        }

        let mut ids = BTreeSet::new();
        for step in &self.steps {
            validate_step(step)?;
            if !ids.insert(step.id.as_str()) {
                return Err(PlanError::new(
                    "structured-plan-duplicate-step",
                    "structured plan contains duplicate step IDs",
                ));
            }
        }
        for step in &self.steps {
            for dependency in &step.dependencies {
                if dependency == &step.id {
                    return Err(PlanError::new(
                        "structured-plan-self-dependency",
                        format!("step {} depends on itself", step.id),
                    ));
                }
                if !ids.contains(dependency.as_str()) {
                    return Err(PlanError::new(
                        "structured-plan-unknown-dependency",
                        format!("step {} depends on unknown step", step.id),
                    ));
                }
            }
            if matches!(step.status, PlanStepStatus::Completed) && step.evidence.is_empty() {
                return Err(PlanError::new(
                    "structured-plan-completion-evidence-missing",
                    format!("completed step {} has no evidence", step.id),
                ));
            }
        }
        detect_dependency_cycle(&self.steps)?;

        let status_by_id = self
            .steps
            .iter()
            .map(|step| (step.id.as_str(), step.status))
            .collect::<BTreeMap<_, _>>();
        for step in &self.steps {
            if matches!(
                step.status,
                PlanStepStatus::InProgress | PlanStepStatus::Completed
            ) && step.dependencies.iter().any(|dependency| {
                !matches!(
                    status_by_id.get(dependency.as_str()),
                    Some(PlanStepStatus::Completed)
                )
            }) {
                return Err(PlanError::new(
                    "structured-plan-dependency-incomplete",
                    format!("step {} has an incomplete dependency", step.id),
                ));
            }
        }
        Ok(())
    }

    pub fn identity(&self) -> Result<String, PlanError> {
        self.validate()?;
        let bytes = to_vec(self).map_err(|_| {
            PlanError::new(
                "structured-plan-serialize-failed",
                "structured plan could not be serialized",
            )
        })?;
        Ok(format!(
            "structured-plan:sha256:{:x}",
            Sha256::digest(bytes)
        ))
    }

    /// Rechecks the reviewed plan against the current base and evidence revisions.
    /// The plan is never silently reset: affected steps retain their state and gain
    /// `stale=true` until a future owner explicitly revises them.
    pub fn revalidate(
        &mut self,
        current_revision: &str,
        evidence_revisions: &BTreeMap<String, String>,
    ) -> Result<RevalidationResult, PlanError> {
        validate_revision(current_revision, "structured-plan-current-revision-invalid")?;
        let plan_revision_changed = self.revision != current_revision;
        let mut stale_step_ids = Vec::new();
        for step in &mut self.steps {
            let base_changed = step
                .base_revision
                .as_deref()
                .is_some_and(|revision| revision != current_revision);
            let evidence_changed = step.evidence.iter().any(|evidence| {
                evidence_revisions
                    .get(&evidence.identity)
                    .is_some_and(|revision| revision != &evidence.revision)
            });
            if plan_revision_changed || base_changed || evidence_changed {
                step.stale = true;
                stale_step_ids.push(step.id.clone());
            }
        }
        self.revision = current_revision.to_owned();
        self.validate()?;
        Ok(RevalidationResult {
            current_revision: current_revision.to_owned(),
            stale_step_ids,
        })
    }
}

fn validate_step(step: &PlanStep) -> Result<(), PlanError> {
    validate_identifier(&step.id, "structured-plan-step-id-invalid")?;
    validate_text(&step.explanation, "structured-plan-explanation-invalid")?;
    validate_identifier(&step.owner, "structured-plan-owner-invalid")?;
    if step.dependencies.len() > MAX_DEPENDENCIES_PER_STEP {
        return Err(PlanError::new(
            "structured-plan-dependency-limit",
            format!("step {} has too many dependencies", step.id),
        ));
    }
    let mut dependencies = BTreeSet::new();
    for dependency in &step.dependencies {
        validate_identifier(dependency, "structured-plan-dependency-invalid")?;
        if !dependencies.insert(dependency.as_str()) {
            return Err(PlanError::new(
                "structured-plan-duplicate-dependency",
                format!("step {} repeats a dependency", step.id),
            ));
        }
    }
    if step.evidence.len() > MAX_EVIDENCE_PER_STEP {
        return Err(PlanError::new(
            "structured-plan-evidence-limit",
            format!("step {} has too much evidence", step.id),
        ));
    }
    let mut evidence_ids = BTreeSet::new();
    for evidence in &step.evidence {
        validate_text(&evidence.kind, "structured-plan-evidence-kind-invalid")?;
        validate_sha256(&evidence.identity)?;
        validate_revision(
            &evidence.revision,
            "structured-plan-evidence-revision-invalid",
        )?;
        if !evidence_ids.insert(evidence.identity.as_str()) {
            return Err(PlanError::new(
                "structured-plan-duplicate-evidence",
                format!("step {} repeats evidence", step.id),
            ));
        }
    }
    if let Some(base_revision) = step.base_revision.as_deref() {
        validate_revision(base_revision, "structured-plan-base-revision-invalid")?;
    }
    Ok(())
}

fn detect_dependency_cycle(steps: &[PlanStep]) -> Result<(), PlanError> {
    let by_id = steps
        .iter()
        .map(|step| (step.id.as_str(), step.dependencies.as_slice()))
        .collect::<BTreeMap<_, _>>();
    let mut visiting = BTreeSet::new();
    let mut visited = BTreeSet::new();
    for step in steps {
        visit_dependency(step.id.as_str(), &by_id, &mut visiting, &mut visited)?;
    }
    Ok(())
}

fn visit_dependency<'a>(
    id: &'a str,
    by_id: &BTreeMap<&'a str, &'a [String]>,
    visiting: &mut BTreeSet<&'a str>,
    visited: &mut BTreeSet<&'a str>,
) -> Result<(), PlanError> {
    if visited.contains(id) {
        return Ok(());
    }
    if !visiting.insert(id) {
        return Err(PlanError::new(
            "structured-plan-dependency-cycle",
            "structured plan dependency graph contains a cycle",
        ));
    }
    if let Some(dependencies) = by_id.get(id) {
        for dependency in *dependencies {
            visit_dependency(dependency.as_str(), by_id, visiting, visited)?;
        }
    }
    visiting.remove(id);
    visited.insert(id);
    Ok(())
}

fn validate_identifier(value: &str, code: &'static str) -> Result<(), PlanError> {
    if value.is_empty()
        || value.chars().count() > MAX_ID_CHARS
        || value
            .bytes()
            .any(|byte| !(byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b':')))
    {
        return Err(PlanError::new(code, "plan identifier is invalid"));
    }
    Ok(())
}

fn validate_text(value: &str, code: &'static str) -> Result<(), PlanError> {
    if value.is_empty()
        || value.chars().count() > MAX_TEXT_CHARS
        || value.chars().any(char::is_control)
    {
        return Err(PlanError::new(code, "plan text is invalid"));
    }
    if redact_complete(value) != value {
        return Err(PlanError::new(
            "structured-plan-secret-shaped",
            "plan text is secret-shaped",
        ));
    }
    Ok(())
}

fn validate_revision(value: &str, code: &'static str) -> Result<(), PlanError> {
    if value.is_empty()
        || value.chars().count() > MAX_REVISION_CHARS
        || value.chars().any(char::is_control)
    {
        return Err(PlanError::new(code, "plan revision is invalid"));
    }
    Ok(())
}

fn validate_sha256(value: &str) -> Result<(), PlanError> {
    let Some(hex) = value.strip_prefix("sha256:") else {
        return Err(PlanError::new(
            "structured-plan-evidence-identity-invalid",
            "plan evidence identity must be a SHA-256 reference",
        ));
    };
    if hex.len() != 64
        || !hex
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
        return Err(PlanError::new(
            "structured-plan-evidence-identity-invalid",
            "plan evidence identity must be lowercase SHA-256",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn evidence(identity: char, revision: &str) -> PlanEvidence {
        PlanEvidence {
            kind: "test".into(),
            identity: format!("sha256:{}", identity.to_string().repeat(64)),
            revision: revision.into(),
        }
    }

    fn plan() -> StructuredPlan {
        StructuredPlan {
            schema_version: SCHEMA_VERSION.into(),
            plan_id: "plan-1".into(),
            session_id: "session-1".into(),
            project_id: Some("project-1".into()),
            revision: "rev-1".into(),
            steps: vec![
                PlanStep {
                    id: "inspect".into(),
                    status: PlanStepStatus::Completed,
                    explanation: "Inspect the project".into(),
                    owner: "agent".into(),
                    dependencies: Vec::new(),
                    evidence: vec![evidence('a', "rev-1")],
                    base_revision: Some("rev-1".into()),
                    stale: false,
                },
                PlanStep {
                    id: "verify".into(),
                    status: PlanStepStatus::InProgress,
                    explanation: "Verify the result".into(),
                    owner: "agent".into(),
                    dependencies: vec!["inspect".into()],
                    evidence: vec![evidence('b', "rev-1")],
                    base_revision: Some("rev-1".into()),
                    stale: false,
                },
            ],
        }
    }

    #[test]
    fn validates_dependencies_status_and_content_free_evidence() {
        let plan = plan();
        assert!(plan.validate().is_ok());
        let identity = plan.identity().unwrap();
        assert!(identity.starts_with("structured-plan:sha256:"));
        assert!(!serde_json::to_string(&plan)
            .unwrap()
            .contains("Inspect the project\n"));
    }

    #[test]
    fn rejects_cycles_unknown_dependencies_and_missing_completion_evidence() {
        let mut cyclic = plan();
        cyclic.steps[0].dependencies = vec!["verify".into()];
        assert_eq!(
            cyclic.validate().unwrap_err().code,
            "structured-plan-dependency-cycle"
        );

        let mut unknown = plan();
        unknown.steps[1].dependencies = vec!["missing".into()];
        assert_eq!(
            unknown.validate().unwrap_err().code,
            "structured-plan-unknown-dependency"
        );

        let mut incomplete = plan();
        incomplete.steps[1].status = PlanStepStatus::Completed;
        incomplete.steps[1].evidence.clear();
        assert_eq!(
            incomplete.validate().unwrap_err().code,
            "structured-plan-completion-evidence-missing"
        );
    }

    #[test]
    fn revalidation_marks_base_and_evidence_drift_stale_without_resetting_state() {
        let mut plan = plan();
        let evidence_id = plan.steps[1].evidence[0].identity.clone();
        let mut revisions = BTreeMap::new();
        revisions.insert(evidence_id, "rev-2".into());
        let result = plan.revalidate("rev-2", &revisions).unwrap();
        assert_eq!(result.current_revision, "rev-2");
        assert_eq!(result.stale_step_ids, vec!["inspect", "verify"]);
        assert!(plan.steps.iter().all(|step| step.stale));
        assert_eq!(plan.steps[0].status, PlanStepStatus::Completed);
    }

    #[test]
    fn rejects_secret_shaped_text_and_non_sha_evidence() {
        let mut secret = plan();
        secret.steps[0].explanation = "token=secret-value".into();
        assert_eq!(
            secret.validate().unwrap_err().code,
            "structured-plan-secret-shaped"
        );

        let mut invalid = plan();
        invalid.steps[0].evidence[0].identity = "artifact:sha256:abc".into();
        assert_eq!(
            invalid.validate().unwrap_err().code,
            "structured-plan-evidence-identity-invalid"
        );
    }
}
