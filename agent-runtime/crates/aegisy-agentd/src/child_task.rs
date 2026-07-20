//! Bounded child-task request contract.
//!
//! A validated request is only a description. It does not create a session,
//! grant permissions, allocate a worktree, or start a provider. Those actions
//! remain behind the later lineage, approval, isolation, and executor gates.

use crate::output_redaction::redact_complete;
use serde::{Deserialize, Serialize};
use serde_json::to_vec;
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;

pub const SCHEMA_VERSION: &str = "child-task/0.1";
pub const HANDOFF_SCHEMA_VERSION: &str = "child-handoff/0.1";
const MAX_ID_CHARS: usize = 128;
const MAX_TEXT_CHARS: usize = 2_048;
const MAX_CONTEXT_REFS: usize = 16;
const MAX_TOOLS: usize = 32;
const MAX_RESULT_SECTIONS: usize = 16;
pub(crate) const MAX_TOKENS: u64 = 10_000_000;
pub(crate) const MAX_COST_MICROS: u64 = 10_000_000_000;
pub(crate) const MAX_WALL_TIME_MS: u64 = 7 * 24 * 60 * 60 * 1_000;
pub(crate) const MAX_TURNS: u32 = 1_000;
pub(crate) const MAX_TOOL_CALLS: u32 = 10_000;
pub(crate) const MAX_CONCURRENCY: u16 = 16;
const MAX_HANDOFF_SUMMARY_BYTES: u64 = 16 * 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ChildTaskError {
    pub code: &'static str,
    pub message: String,
}

impl ChildTaskError {
    fn new(code: &'static str, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ContextReference {
    pub kind: String,
    pub identity: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct WorkspaceScope {
    pub project_id: String,
    pub root_id: String,
    /// `read_only` is the only currently usable mode; `dedicated_worktree`
    /// describes the required future isolation for write-capable children.
    pub isolation: String,
    pub base_revision: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PermissionRequest {
    /// This names a policy profile; it is not an authorization decision.
    pub profile: String,
    pub network: String,
    pub write: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ChildTaskBudget {
    pub max_tokens: u64,
    pub max_cost_micros: u64,
    pub max_wall_time_ms: u64,
    pub max_turns: u32,
    pub max_tool_calls: u32,
    pub max_concurrency: u16,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ExpectedResultShape {
    pub schema_version: String,
    pub max_summary_bytes: u64,
    pub required_sections: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ChildTaskRequest {
    pub schema_version: String,
    pub task_id: String,
    pub parent_session_id: String,
    pub parent_turn_id: String,
    pub goal: String,
    #[serde(default)]
    pub context: Vec<ContextReference>,
    pub workspace: WorkspaceScope,
    #[serde(default)]
    pub tools: Vec<String>,
    pub model_profile: String,
    pub permissions: PermissionRequest,
    pub budget: ChildTaskBudget,
    pub expected_result: ExpectedResultShape,
}

impl ChildTaskRequest {
    pub fn validate(&self) -> Result<(), ChildTaskError> {
        if self.schema_version != SCHEMA_VERSION {
            return Err(ChildTaskError::new(
                "child-task-schema-unsupported",
                "child-task schema is unsupported",
            ));
        }
        for (value, code) in [
            (&self.task_id, "child-task-id-invalid"),
            (&self.parent_session_id, "child-task-parent-session-invalid"),
            (&self.parent_turn_id, "child-task-parent-turn-invalid"),
            (&self.model_profile, "child-task-model-profile-invalid"),
        ] {
            validate_identifier(value, code)?;
        }
        validate_text(&self.goal, "child-task-goal-invalid")?;
        self.validate_context()?;
        self.workspace.validate()?;
        self.validate_tools()?;
        self.permissions.validate()?;
        self.budget.validate()?;
        self.expected_result.validate()?;
        Ok(())
    }

    pub fn identity(&self) -> Result<String, ChildTaskError> {
        self.validate()?;
        let bytes = to_vec(self).map_err(|_| {
            ChildTaskError::new(
                "child-task-serialize-failed",
                "child-task could not be serialized",
            )
        })?;
        Ok(format!("child-task:sha256:{:x}", Sha256::digest(bytes)))
    }

    fn validate_context(&self) -> Result<(), ChildTaskError> {
        if self.context.len() > MAX_CONTEXT_REFS {
            return Err(ChildTaskError::new(
                "child-task-context-limit",
                "child-task context exceeds the bounded reference limit",
            ));
        }
        let mut identities = BTreeSet::new();
        for reference in &self.context {
            validate_identifier(&reference.kind, "child-task-context-kind-invalid")?;
            validate_content_identity(&reference.identity)?;
            if !identities.insert(reference.identity.as_str()) {
                return Err(ChildTaskError::new(
                    "child-task-duplicate-context",
                    "child-task context contains a duplicate identity",
                ));
            }
        }
        Ok(())
    }

    fn validate_tools(&self) -> Result<(), ChildTaskError> {
        if self.tools.len() > MAX_TOOLS {
            return Err(ChildTaskError::new(
                "child-task-tool-limit",
                "child-task tool list exceeds the bounded limit",
            ));
        }
        let mut tools = BTreeSet::new();
        for tool in &self.tools {
            validate_identifier(tool, "child-task-tool-invalid")?;
            if !tools.insert(tool.as_str()) {
                return Err(ChildTaskError::new(
                    "child-task-duplicate-tool",
                    "child-task tool list contains a duplicate",
                ));
            }
        }
        Ok(())
    }
}

impl WorkspaceScope {
    fn validate(&self) -> Result<(), ChildTaskError> {
        validate_identifier(&self.project_id, "child-task-project-invalid")?;
        validate_identifier(&self.root_id, "child-task-root-invalid")?;
        if !matches!(self.isolation.as_str(), "read_only" | "dedicated_worktree") {
            return Err(ChildTaskError::new(
                "child-task-workspace-isolation-invalid",
                "child-task workspace must be read_only or dedicated_worktree",
            ));
        }
        validate_revision(&self.base_revision, "child-task-base-revision-invalid")
    }
}

impl PermissionRequest {
    fn validate(&self) -> Result<(), ChildTaskError> {
        if !matches!(
            self.profile.as_str(),
            "chat" | "read-only" | "workspace-write" | "developer" | "full-access"
        ) {
            return Err(ChildTaskError::new(
                "child-task-permission-profile-invalid",
                "child-task permission profile is not registered",
            ));
        }
        if !matches!(self.network.as_str(), "none" | "allowlisted" | "full") {
            return Err(ChildTaskError::new(
                "child-task-network-policy-invalid",
                "child-task network policy is invalid",
            ));
        }
        Ok(())
    }
}

impl ChildTaskBudget {
    fn validate(&self) -> Result<(), ChildTaskError> {
        if self.max_tokens == 0 || self.max_tokens > MAX_TOKENS {
            return Err(ChildTaskError::new(
                "child-task-token-budget-invalid",
                "child-task token budget is outside the bounded range",
            ));
        }
        if self.max_cost_micros == 0 || self.max_cost_micros > MAX_COST_MICROS {
            return Err(ChildTaskError::new(
                "child-task-cost-budget-invalid",
                "child-task cost budget is outside the bounded range",
            ));
        }
        if self.max_wall_time_ms == 0 || self.max_wall_time_ms > MAX_WALL_TIME_MS {
            return Err(ChildTaskError::new(
                "child-task-time-budget-invalid",
                "child-task wall-time budget is outside the bounded range",
            ));
        }
        if self.max_turns == 0 || self.max_turns > MAX_TURNS {
            return Err(ChildTaskError::new(
                "child-task-turn-budget-invalid",
                "child-task turn budget is outside the bounded range",
            ));
        }
        if self.max_tool_calls == 0 || self.max_tool_calls > MAX_TOOL_CALLS {
            return Err(ChildTaskError::new(
                "child-task-tool-budget-invalid",
                "child-task tool budget is outside the bounded range",
            ));
        }
        if self.max_concurrency == 0 || self.max_concurrency > MAX_CONCURRENCY {
            return Err(ChildTaskError::new(
                "child-task-concurrency-budget-invalid",
                "child-task concurrency budget is outside the bounded range",
            ));
        }
        Ok(())
    }
}

impl ExpectedResultShape {
    fn validate(&self) -> Result<(), ChildTaskError> {
        if self.schema_version != HANDOFF_SCHEMA_VERSION {
            return Err(ChildTaskError::new(
                "child-task-result-schema-invalid",
                "child-task result must use child-handoff/0.1",
            ));
        }
        if self.max_summary_bytes == 0 || self.max_summary_bytes > MAX_HANDOFF_SUMMARY_BYTES {
            return Err(ChildTaskError::new(
                "child-task-result-size-invalid",
                "child-task result summary exceeds the bounded limit",
            ));
        }
        if self.required_sections.is_empty() || self.required_sections.len() > MAX_RESULT_SECTIONS {
            return Err(ChildTaskError::new(
                "child-task-result-section-limit",
                "child-task result sections are outside the bounded range",
            ));
        }
        let mut sections = BTreeSet::new();
        for section in &self.required_sections {
            validate_text(section, "child-task-result-section-invalid")?;
            if !sections.insert(section.as_str()) {
                return Err(ChildTaskError::new(
                    "child-task-duplicate-result-section",
                    "child-task result sections contain a duplicate",
                ));
            }
        }
        Ok(())
    }
}

fn validate_identifier(value: &str, code: &'static str) -> Result<(), ChildTaskError> {
    if value.is_empty()
        || value.chars().count() > MAX_ID_CHARS
        || value
            .bytes()
            .any(|byte| !(byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b':')))
    {
        return Err(ChildTaskError::new(
            code,
            "child-task identifier is invalid",
        ));
    }
    Ok(())
}

fn validate_text(value: &str, code: &'static str) -> Result<(), ChildTaskError> {
    if value.is_empty()
        || value.chars().count() > MAX_TEXT_CHARS
        || value.chars().any(char::is_control)
    {
        return Err(ChildTaskError::new(code, "child-task text is invalid"));
    }
    if redact_complete(value) != value {
        return Err(ChildTaskError::new(
            "child-task-secret-shaped",
            "child-task text is secret-shaped",
        ));
    }
    Ok(())
}

fn validate_revision(value: &str, code: &'static str) -> Result<(), ChildTaskError> {
    if value.is_empty() || value.chars().count() > 256 || value.chars().any(char::is_control) {
        return Err(ChildTaskError::new(code, "child-task revision is invalid"));
    }
    Ok(())
}

fn validate_content_identity(value: &str) -> Result<(), ChildTaskError> {
    let valid = [
        "pinned-context:sha256:",
        "structured-plan:sha256:",
        "artifact:sha256:",
    ]
    .iter()
    .any(|prefix| {
        value.strip_prefix(prefix).is_some_and(|hex| {
            hex.len() == 64
                && hex
                    .bytes()
                    .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
        })
    });
    if !valid {
        return Err(ChildTaskError::new(
            "child-task-context-identity-invalid",
            "child-task context identity is not an approved content reference",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request() -> ChildTaskRequest {
        ChildTaskRequest {
            schema_version: SCHEMA_VERSION.into(),
            task_id: "child-1".into(),
            parent_session_id: "session-1".into(),
            parent_turn_id: "turn-1".into(),
            goal: "Inspect the selected files and report test risks".into(),
            context: vec![ContextReference {
                kind: "pinned".into(),
                identity: format!("pinned-context:sha256:{}", "a".repeat(64)),
            }],
            workspace: WorkspaceScope {
                project_id: "project-1".into(),
                root_id: "root-1".into(),
                isolation: "read_only".into(),
                base_revision: "rev-1".into(),
            },
            tools: vec!["workspace-read".into(), "test-runner".into()],
            model_profile: "agent-default".into(),
            permissions: PermissionRequest {
                profile: "read-only".into(),
                network: "none".into(),
                write: false,
            },
            budget: ChildTaskBudget {
                max_tokens: 100_000,
                max_cost_micros: 1_000_000,
                max_wall_time_ms: 300_000,
                max_turns: 8,
                max_tool_calls: 32,
                max_concurrency: 1,
            },
            expected_result: ExpectedResultShape {
                schema_version: HANDOFF_SCHEMA_VERSION.into(),
                max_summary_bytes: 8 * 1024,
                required_sections: vec!["summary".into(), "evidence".into(), "risks".into()],
            },
        }
    }

    #[test]
    fn validates_contract_and_produces_content_addressed_identity() {
        let request = request();
        assert!(request.validate().is_ok());
        assert!(request
            .identity()
            .unwrap()
            .starts_with("child-task:sha256:"));
    }

    #[test]
    fn rejects_unsafe_context_workspace_and_permission_requests() {
        let mut invalid = request();
        invalid.context[0].identity = "file:///tmp/source".into();
        assert_eq!(
            invalid.validate().unwrap_err().code,
            "child-task-context-identity-invalid"
        );

        let mut shared = request();
        shared.workspace.isolation = "shared".into();
        assert_eq!(
            shared.validate().unwrap_err().code,
            "child-task-workspace-isolation-invalid"
        );

        let mut permission = request();
        permission.permissions.profile = "full".into();
        assert_eq!(
            permission.validate().unwrap_err().code,
            "child-task-permission-profile-invalid"
        );
    }

    #[test]
    fn rejects_duplicate_resources_and_unbounded_budgets() {
        let mut duplicate = request();
        duplicate.tools.push("workspace-read".into());
        assert_eq!(
            duplicate.validate().unwrap_err().code,
            "child-task-duplicate-tool"
        );

        let mut budget = request();
        budget.budget.max_concurrency = MAX_CONCURRENCY + 1;
        assert_eq!(
            budget.validate().unwrap_err().code,
            "child-task-concurrency-budget-invalid"
        );
    }

    #[test]
    fn rejects_secrets_and_result_shapes_that_cannot_be_handed_off() {
        let mut secret = request();
        secret.goal = "api_key=not-for-storage".into();
        assert_eq!(
            secret.validate().unwrap_err().code,
            "child-task-secret-shaped"
        );

        let mut result = request();
        result.expected_result.schema_version = "unknown/0.1".into();
        assert_eq!(
            result.validate().unwrap_err().code,
            "child-task-result-schema-invalid"
        );
    }
}
