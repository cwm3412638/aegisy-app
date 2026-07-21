//! Metadata-only model profile contract.
//!
//! Profiles describe which catalog model may serve an explicit role. They do
//! not select a provider, issue a token, persist configuration, or authorize a
//! turn. A single-model profile binds only the Agent role by default so a
//! caller cannot create hidden plan/apply/utility calls by falling back to the
//! primary model.

use crate::output_redaction::redact_complete;
use serde::{Deserialize, Serialize};
use serde_json::to_vec;
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;

pub const SCHEMA_VERSION: &str = "model-profile/0.1";
const MAX_ID_CHARS: usize = 128;
const MAX_NAME_CHARS: usize = 128;
const MAX_SOURCE_CHARS: usize = 64;
const MAX_ROLE_BINDINGS: usize = 7;

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum ProfileScope {
    Global,
    Project,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum ProfileStrategy {
    SingleModel,
    RoleSpecific,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Ord, PartialOrd)]
pub enum ModelRole {
    Agent,
    Plan,
    Apply,
    Review,
    Utility,
    Embedding,
    Rerank,
}

impl ModelRole {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Agent => "agent",
            Self::Plan => "plan",
            Self::Apply => "apply",
            Self::Review => "review",
            Self::Utility => "utility",
            Self::Embedding => "embedding",
            Self::Rerank => "rerank",
        }
    }

    fn parse(value: &str) -> Option<Self> {
        Some(match value {
            "agent" => Self::Agent,
            "plan" => Self::Plan,
            "apply" => Self::Apply,
            "review" => Self::Review,
            "utility" => Self::Utility,
            "embedding" => Self::Embedding,
            "rerank" => Self::Rerank,
            _ => return None,
        })
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ModelRoleBinding {
    pub model_id: String,
    pub enabled: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ModelProfile {
    pub schema_version: String,
    pub profile_id: String,
    pub scope: ProfileScope,
    pub project_id: Option<String>,
    pub name: String,
    pub strategy: ProfileStrategy,
    pub default_model_id: String,
    pub roles: BTreeMap<String, ModelRoleBinding>,
    pub source: String,
    pub revision: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModelProfileError {
    pub code: &'static str,
    pub message: String,
}

impl ModelProfileError {
    fn new(code: &'static str, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RoleResolution {
    pub role: String,
    pub state: &'static str,
    pub model_id: Option<String>,
    pub explicit: bool,
}

impl ModelProfile {
    /// Creates the conservative default profile: only the Agent role is active.
    pub fn single_model(
        profile_id: impl Into<String>,
        scope: ProfileScope,
        project_id: Option<String>,
        default_model_id: impl Into<String>,
        source: impl Into<String>,
    ) -> Result<Self, ModelProfileError> {
        let default_model_id = default_model_id.into();
        let profile = Self {
            schema_version: SCHEMA_VERSION.into(),
            profile_id: profile_id.into(),
            scope,
            project_id,
            name: "Aegisy default".into(),
            strategy: ProfileStrategy::SingleModel,
            default_model_id: default_model_id.clone(),
            roles: BTreeMap::from([(
                ModelRole::Agent.as_str().into(),
                ModelRoleBinding {
                    model_id: default_model_id.clone(),
                    enabled: true,
                },
            )]),
            source: source.into(),
            revision: 0,
        };
        profile.validate()?;
        Ok(profile)
    }

    pub fn validate(&self) -> Result<(), ModelProfileError> {
        if self.schema_version != SCHEMA_VERSION {
            return Err(ModelProfileError::new(
                "model-profile-schema-unsupported",
                "model profile schema is unsupported",
            ));
        }
        validate_identifier(&self.profile_id, "model-profile-id-invalid")?;
        validate_text(&self.name, MAX_NAME_CHARS, "model-profile-name-invalid")?;
        validate_identifier(
            &self.default_model_id,
            "model-profile-default-model-invalid",
        )?;
        validate_text(
            &self.source,
            MAX_SOURCE_CHARS,
            "model-profile-source-invalid",
        )?;
        match self.scope {
            ProfileScope::Global if self.project_id.is_some() => {
                return Err(ModelProfileError::new(
                    "model-profile-global-project-invalid",
                    "global model profile cannot bind a project",
                ));
            }
            ProfileScope::Project => {
                let Some(project_id) = &self.project_id else {
                    return Err(ModelProfileError::new(
                        "model-profile-project-missing",
                        "project model profile requires a project identity",
                    ));
                };
                validate_identifier(project_id, "model-profile-project-invalid")?;
            }
            ProfileScope::Global => {}
        }
        if self.roles.len() > MAX_ROLE_BINDINGS {
            return Err(ModelProfileError::new(
                "model-profile-role-limit",
                "model profile has too many role bindings",
            ));
        }
        for (role, binding) in &self.roles {
            if ModelRole::parse(role).is_none() {
                return Err(ModelProfileError::new(
                    "model-profile-role-invalid",
                    "model profile contains an unsupported role",
                ));
            }
            validate_identifier(&binding.model_id, "model-profile-role-model-invalid")?;
            if self.strategy == ProfileStrategy::SingleModel
                && binding.enabled
                && binding.model_id != self.default_model_id
            {
                return Err(ModelProfileError::new(
                    "model-profile-single-model-mismatch",
                    "single-model profile cannot bind a different enabled model",
                ));
            }
        }
        if !self.roles.contains_key(ModelRole::Agent.as_str()) {
            return Err(ModelProfileError::new(
                "model-profile-agent-binding-missing",
                "model profile must explicitly bind the Agent role",
            ));
        }
        if self.strategy == ProfileStrategy::SingleModel
            && !self
                .roles
                .get(ModelRole::Agent.as_str())
                .is_some_and(|binding| binding.enabled && binding.model_id == self.default_model_id)
        {
            return Err(ModelProfileError::new(
                "model-profile-agent-binding-invalid",
                "single-model profile Agent binding must be enabled and use the default model",
            ));
        }
        Ok(())
    }

    pub fn identity(&self) -> Result<String, ModelProfileError> {
        self.validate()?;
        let bytes = to_vec(self).map_err(|_| {
            ModelProfileError::new(
                "model-profile-serialize-failed",
                "model profile could not be serialized",
            )
        })?;
        Ok(format!("model-profile:sha256:{:x}", Sha256::digest(bytes)))
    }

    /// Resolves only explicit enabled bindings; no role silently falls back.
    pub fn resolve_role(&self, role: ModelRole) -> Result<RoleResolution, ModelProfileError> {
        self.validate()?;
        let key = role.as_str().to_owned();
        let Some(binding) = self.roles.get(&key) else {
            return Ok(RoleResolution {
                role: key,
                state: "disabled",
                model_id: None,
                explicit: false,
            });
        };
        if !binding.enabled {
            return Ok(RoleResolution {
                role: key,
                state: "disabled",
                model_id: None,
                explicit: true,
            });
        }
        Ok(RoleResolution {
            role: key,
            state: "configured",
            model_id: Some(binding.model_id.clone()),
            explicit: true,
        })
    }
}

fn validate_identifier(value: &str, code: &'static str) -> Result<(), ModelProfileError> {
    if value.is_empty()
        || value.chars().count() > MAX_ID_CHARS
        || value.bytes().any(|byte| {
            !(byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b':' | b'.'))
        })
    {
        return Err(ModelProfileError::new(
            code,
            "model profile identifier is invalid",
        ));
    }
    Ok(())
}

fn validate_text(
    value: &str,
    max_chars: usize,
    code: &'static str,
) -> Result<(), ModelProfileError> {
    if value.is_empty() || value.chars().count() > max_chars || value.chars().any(char::is_control)
    {
        return Err(ModelProfileError::new(
            code,
            "model profile text is invalid",
        ));
    }
    if redact_complete(value) != value {
        return Err(ModelProfileError::new(
            "model-profile-secret-shaped",
            "model profile metadata is secret-shaped",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn single_model_profile_defaults_to_agent_only() {
        let profile = ModelProfile::single_model(
            "global-default",
            ProfileScope::Global,
            None,
            "local:echo",
            "user",
        )
        .unwrap();
        let agent = profile.resolve_role(ModelRole::Agent).unwrap();
        let plan = profile.resolve_role(ModelRole::Plan).unwrap();
        assert_eq!(agent.state, "configured");
        assert_eq!(agent.model_id.as_deref(), Some("local:echo"));
        assert!(agent.explicit);
        assert_eq!(plan.state, "disabled");
        assert!(!plan.explicit);
    }

    #[test]
    fn project_profile_requires_matching_scope_identity() {
        let profile = ModelProfile::single_model(
            "project-default",
            ProfileScope::Project,
            Some("project-1".into()),
            "provider:model",
            "migration",
        )
        .unwrap();
        assert_eq!(profile.scope, ProfileScope::Project);
        assert_eq!(profile.project_id.as_deref(), Some("project-1"));
        assert!(ModelProfile::single_model(
            "project-default",
            ProfileScope::Project,
            None,
            "provider:model",
            "migration",
        )
        .is_err());
    }

    #[test]
    fn single_model_rejects_different_enabled_role_model() {
        let mut profile = ModelProfile::single_model(
            "global-default",
            ProfileScope::Global,
            None,
            "provider:model-a",
            "user",
        )
        .unwrap();
        profile.roles.insert(
            ModelRole::Review.as_str().into(),
            ModelRoleBinding {
                model_id: "provider:model-b".into(),
                enabled: true,
            },
        );
        let error = profile.validate().unwrap_err();
        assert_eq!(error.code, "model-profile-single-model-mismatch");
    }

    #[test]
    fn role_specific_profile_never_falls_back_to_default_model() {
        let mut profile = ModelProfile::single_model(
            "global-default",
            ProfileScope::Global,
            None,
            "provider:model-a",
            "user",
        )
        .unwrap();
        profile.strategy = ProfileStrategy::RoleSpecific;
        profile.roles.insert(
            ModelRole::Review.as_str().into(),
            ModelRoleBinding {
                model_id: "provider:model-b".into(),
                enabled: true,
            },
        );
        profile.validate().unwrap();
        let apply = profile.resolve_role(ModelRole::Apply).unwrap();
        let review = profile.resolve_role(ModelRole::Review).unwrap();
        assert_eq!(apply.state, "disabled");
        assert_eq!(review.model_id.as_deref(), Some("provider:model-b"));
    }

    #[test]
    fn profile_identity_is_content_addressed_and_secret_free() {
        let mut profile = ModelProfile::single_model(
            "global-default",
            ProfileScope::Global,
            None,
            "provider:model-a",
            "user",
        )
        .unwrap();
        let identity = profile.identity().unwrap();
        assert!(identity.starts_with("model-profile:sha256:"));
        profile.name = "token=sk-secret".into();
        let error = profile.validate().unwrap_err();
        assert_eq!(error.code, "model-profile-secret-shaped");
    }

    #[test]
    fn disabled_explicit_role_does_not_produce_a_model() {
        let mut profile = ModelProfile::single_model(
            "global-default",
            ProfileScope::Global,
            None,
            "provider:model-a",
            "user",
        )
        .unwrap();
        profile.roles.insert(
            ModelRole::Utility.as_str().into(),
            ModelRoleBinding {
                model_id: "provider:model-a".into(),
                enabled: false,
            },
        );
        let utility = profile.resolve_role(ModelRole::Utility).unwrap();
        assert_eq!(utility.state, "disabled");
        assert!(utility.explicit);
        assert!(utility.model_id.is_none());
    }
}
