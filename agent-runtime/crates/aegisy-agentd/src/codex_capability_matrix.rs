use serde::Serialize;
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;

pub const CODEX_ADAPTER: &str = "codex-app-server";
pub const CODEX_VERSION: &str = "codex-cli 0.144.5";
pub const VENDOR_SCHEMA_VERSION: &str = "v2";
pub const VENDOR_SCHEMA_SHA256: &str =
    "e66ff6063c146734a92c9a018e43efefb079278ee597782f30674edcccedbdb2";
pub const CLIENT_REQUEST_COUNT: usize = 87;
pub const SERVER_NOTIFICATION_COUNT: usize = 68;
pub const THREAD_ITEM_COUNT: usize = 18;
pub const RECOVERY_VERSION: &str = "workbench-recovery-diagnostic/0.1";

const CLIENT_REQUEST_NAMES: &[&str] = &[
    "initialize",
    "thread/start",
    "thread/resume",
    "thread/fork",
    "thread/archive",
    "thread/delete",
    "thread/unsubscribe",
    "thread/name/set",
    "thread/goal/set",
    "thread/goal/get",
    "thread/goal/clear",
    "thread/metadata/update",
    "thread/unarchive",
    "thread/compact/start",
    "thread/shellCommand",
    "thread/approveGuardianDeniedAction",
    "thread/rollback",
    "thread/list",
    "thread/loaded/list",
    "thread/read",
    "thread/inject_items",
    "skills/list",
    "skills/extraRoots/set",
    "hooks/list",
    "marketplace/add",
    "marketplace/remove",
    "marketplace/upgrade",
    "plugin/list",
    "plugin/installed",
    "plugin/read",
    "plugin/skill/read",
    "plugin/share/save",
    "plugin/share/updateTargets",
    "plugin/share/list",
    "plugin/share/checkout",
    "plugin/share/delete",
    "app/list",
    "fs/readFile",
    "fs/writeFile",
    "fs/createDirectory",
    "fs/getMetadata",
    "fs/readDirectory",
    "fs/remove",
    "fs/copy",
    "fs/watch",
    "fs/unwatch",
    "skills/config/write",
    "plugin/install",
    "plugin/uninstall",
    "turn/start",
    "turn/steer",
    "turn/interrupt",
    "review/start",
    "model/list",
    "modelProvider/capabilities/read",
    "experimentalFeature/list",
    "permissionProfile/list",
    "experimentalFeature/enablement/set",
    "mcpServer/oauth/login",
    "config/mcpServer/reload",
    "mcpServerStatus/list",
    "mcpServer/resource/read",
    "mcpServer/tool/call",
    "windowsSandbox/setupStart",
    "windowsSandbox/readiness",
    "account/login/start",
    "account/login/cancel",
    "account/logout",
    "account/rateLimits/read",
    "account/rateLimitResetCredit/consume",
    "account/usage/read",
    "account/workspaceMessages/read",
    "account/sendAddCreditsNudgeEmail",
    "feedback/upload",
    "command/exec",
    "command/exec/write",
    "command/exec/terminate",
    "command/exec/resize",
    "config/read",
    "externalAgentConfig/detect",
    "externalAgentConfig/import",
    "externalAgentConfig/import/readHistories",
    "config/value/write",
    "config/batchWrite",
    "configRequirements/read",
    "account/read",
    "fuzzyFileSearch",
];

const SERVER_NOTIFICATION_NAMES: &[&str] = &[
    "error",
    "thread/started",
    "thread/status/changed",
    "thread/archived",
    "thread/deleted",
    "thread/unarchived",
    "thread/closed",
    "skills/changed",
    "thread/name/updated",
    "thread/goal/updated",
    "thread/goal/cleared",
    "thread/settings/updated",
    "thread/tokenUsage/updated",
    "turn/started",
    "hook/started",
    "turn/completed",
    "hook/completed",
    "turn/diff/updated",
    "turn/plan/updated",
    "item/started",
    "item/autoApprovalReview/started",
    "item/autoApprovalReview/completed",
    "item/completed",
    "item/agentMessage/delta",
    "item/plan/delta",
    "command/exec/outputDelta",
    "process/outputDelta",
    "process/exited",
    "item/commandExecution/outputDelta",
    "item/commandExecution/terminalInteraction",
    "item/fileChange/outputDelta",
    "item/fileChange/patchUpdated",
    "serverRequest/resolved",
    "item/mcpToolCall/progress",
    "mcpServer/oauthLogin/completed",
    "mcpServer/startupStatus/updated",
    "account/updated",
    "account/rateLimits/updated",
    "app/list/updated",
    "remoteControl/status/changed",
    "externalAgentConfig/import/progress",
    "externalAgentConfig/import/completed",
    "fs/changed",
    "item/reasoning/summaryTextDelta",
    "item/reasoning/summaryPartAdded",
    "item/reasoning/textDelta",
    "thread/compacted",
    "model/rerouted",
    "model/verification",
    "turn/moderationMetadata",
    "model/safetyBuffering/updated",
    "warning",
    "guardianWarning",
    "deprecationNotice",
    "configWarning",
    "fuzzyFileSearch/sessionUpdated",
    "fuzzyFileSearch/sessionCompleted",
    "thread/realtime/started",
    "thread/realtime/itemAdded",
    "thread/realtime/transcript/delta",
    "thread/realtime/transcript/done",
    "thread/realtime/outputAudio/delta",
    "thread/realtime/sdp",
    "thread/realtime/error",
    "thread/realtime/closed",
    "windows/worldWritableWarning",
    "windowsSandbox/setupCompleted",
    "account/login/completed",
];

pub(crate) fn is_known_server_notification(method: &str) -> bool {
    SERVER_NOTIFICATION_NAMES.contains(&method)
}

const THREAD_ITEM_NAMES: &[&str] = &[
    "userMessage",
    "hookPrompt",
    "agentMessage",
    "plan",
    "reasoning",
    "commandExecution",
    "fileChange",
    "mcpToolCall",
    "dynamicToolCall",
    "collabAgentToolCall",
    "subAgentActivity",
    "webSearch",
    "imageView",
    "sleep",
    "imageGeneration",
    "enteredReviewMode",
    "exitedReviewMode",
    "contextCompaction",
];

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum VendorDomain {
    ClientRequest,
    ServerNotification,
    ThreadItem,
}

impl VendorDomain {
    const fn code(self) -> &'static str {
        match self {
            Self::ClientRequest => "client-request",
            Self::ServerNotification => "server-notification",
            Self::ThreadItem => "thread-item",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
enum MappingStatus {
    Mapped,
    RuntimeOnly,
    Partial,
    Blocked,
    Unsupported,
    ExperimentalDisabled,
}

impl MappingStatus {
    const fn code(self) -> &'static str {
        match self {
            Self::Mapped => "mapped",
            Self::RuntimeOnly => "runtime-only",
            Self::Partial => "partial",
            Self::Blocked => "blocked",
            Self::Unsupported => "unsupported",
            Self::ExperimentalDisabled => "experimental-disabled",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct VendorClassification {
    domain: VendorDomain,
    name: &'static str,
    status: MappingStatus,
    desktop_surface_available: bool,
    authority_granted: bool,
}

fn classify(domain: VendorDomain, name: &'static str) -> VendorClassification {
    let status = match domain {
        VendorDomain::ClientRequest => match name {
            "initialize" | "thread/start" | "thread/resume" | "thread/fork" | "thread/archive"
            | "thread/unarchive" | "turn/start" | "turn/interrupt" => MappingStatus::Mapped,
            "thread/list" | "thread/read" | "turn/steer" => MappingStatus::RuntimeOnly,
            "thread/delete"
            | "thread/compact/start"
            | "fs/writeFile"
            | "fs/createDirectory"
            | "fs/remove"
            | "fs/copy"
            | "skills/config/write"
            | "plugin/install"
            | "plugin/uninstall"
            | "command/exec"
            | "command/exec/write"
            | "config/value/write"
            | "config/batchWrite" => MappingStatus::Blocked,
            "experimentalFeature/list" | "experimentalFeature/enablement/set" => {
                MappingStatus::ExperimentalDisabled
            }
            "thread/loaded/list" | "model/list" | "modelProvider/capabilities/read" => {
                MappingStatus::Partial
            }
            _ => MappingStatus::Unsupported,
        },
        VendorDomain::ServerNotification => match name {
            "error"
            | "thread/tokenUsage/updated"
            | "turn/completed"
            | "turn/diff/updated"
            | "turn/plan/updated"
            | "item/agentMessage/delta"
            | "item/commandExecution/outputDelta" => MappingStatus::Mapped,
            "item/started" | "item/completed" | "warning" | "guardianWarning"
            | "deprecationNotice" | "configWarning" => MappingStatus::Partial,
            name if name.starts_with("thread/realtime/") => MappingStatus::ExperimentalDisabled,
            _ => MappingStatus::Unsupported,
        },
        VendorDomain::ThreadItem => match name {
            "agentMessage" | "commandExecution" => MappingStatus::Mapped,
            "userMessage" | "plan" | "reasoning" | "fileChange" | "mcpToolCall" | "imageView"
            | "dynamicToolCall" | "webSearch" | "imageGeneration" => MappingStatus::Partial,
            "contextCompaction" => MappingStatus::Blocked,
            _ => MappingStatus::Unsupported,
        },
    };
    VendorClassification {
        domain,
        name,
        status,
        desktop_surface_available: status == MappingStatus::Mapped,
        authority_granted: false,
    }
}

fn classifications() -> impl Iterator<Item = VendorClassification> {
    CLIENT_REQUEST_NAMES
        .iter()
        .map(|name| classify(VendorDomain::ClientRequest, name))
        .chain(
            SERVER_NOTIFICATION_NAMES
                .iter()
                .map(|name| classify(VendorDomain::ServerNotification, name)),
        )
        .chain(
            THREAD_ITEM_NAMES
                .iter()
                .map(|name| classify(VendorDomain::ThreadItem, name)),
        )
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct CapabilityMatrixIdentity {
    pub schema_version: &'static str,
    pub identity: String,
    pub adapter: &'static str,
    pub codex_version: &'static str,
    pub vendor_schema_version: &'static str,
    pub vendor_schema_sha256: &'static str,
    pub client_request_count: usize,
    pub server_notification_count: usize,
    pub thread_item_count: usize,
    pub complete: bool,
}

fn matrix_identity() -> String {
    let mut digest = Sha256::new();
    for value in [
        "codex-capability-matrix/0.1",
        CODEX_ADAPTER,
        CODEX_VERSION,
        VENDOR_SCHEMA_VERSION,
        VENDOR_SCHEMA_SHA256,
    ] {
        digest.update(value.as_bytes());
        digest.update([0]);
    }
    for entry in classifications() {
        digest.update(entry.domain.code().as_bytes());
        digest.update([0]);
        digest.update(entry.name.as_bytes());
        digest.update([0]);
        digest.update(entry.status.code().as_bytes());
        digest.update([entry.desktop_surface_available as u8]);
        digest.update([entry.authority_granted as u8]);
    }
    format!("codex-capability-matrix:sha256:{:x}", digest.finalize())
}

fn capability_matrix() -> CapabilityMatrixIdentity {
    CapabilityMatrixIdentity {
        schema_version: "codex-capability-matrix/0.1",
        identity: matrix_identity(),
        adapter: CODEX_ADAPTER,
        codex_version: CODEX_VERSION,
        vendor_schema_version: VENDOR_SCHEMA_VERSION,
        vendor_schema_sha256: VENDOR_SCHEMA_SHA256,
        client_request_count: CLIENT_REQUEST_COUNT,
        server_notification_count: SERVER_NOTIFICATION_COUNT,
        thread_item_count: THREAD_ITEM_COUNT,
        complete: true,
    }
}

#[derive(Debug, Clone, Copy, Serialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum BackendKind {
    Codex,
    Preview,
    Recovery,
    Unavailable,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct BackendIdentity {
    pub kind: BackendKind,
    pub adapter: &'static str,
    pub version: &'static str,
    pub status: &'static str,
}

#[derive(Debug, Clone, Copy, Serialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum FeatureState {
    Disabled,
    MetadataOnly,
    Blocked,
    Unavailable,
    RuntimeOnly,
}

#[derive(Debug, Clone, Copy, Serialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum FeatureScope {
    Runtime,
    Provider,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct FeatureDegradation {
    pub feature: &'static str,
    pub state: FeatureState,
    pub reason: &'static str,
    pub scope: FeatureScope,
    pub authority_granted: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub runtime_supported: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub desktop_surface_available: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub availability: Option<&'static str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub stable_enabled: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub override_available: Option<bool>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub missing_gates: Option<Vec<&'static str>>,
}

impl FeatureDegradation {
    fn fixed(
        feature: &'static str,
        state: FeatureState,
        reason: &'static str,
        scope: FeatureScope,
    ) -> Self {
        Self {
            feature,
            state,
            reason,
            scope,
            authority_granted: false,
            runtime_supported: None,
            desktop_surface_available: None,
            availability: None,
            stable_enabled: None,
            override_available: None,
            missing_gates: None,
        }
    }

    fn runtime_only(feature: &'static str, reason: &'static str) -> Self {
        let mut value = Self::fixed(
            feature,
            FeatureState::RuntimeOnly,
            reason,
            FeatureScope::Provider,
        );
        value.runtime_supported = Some(true);
        value.desktop_surface_available = Some(false);
        value
    }

    fn autonomy(
        feature: &'static str,
        reason: &'static str,
        missing_gates: &[&'static str],
    ) -> Self {
        let mut value = Self::fixed(
            feature,
            FeatureState::Disabled,
            reason,
            FeatureScope::Runtime,
        );
        value.availability = Some("not-advertised");
        value.stable_enabled = Some(false);
        value.override_available = Some(false);
        value.missing_gates = Some(missing_gates.to_vec());
        value
    }
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct RuntimeDegradationSnapshot {
    pub schema_version: &'static str,
    pub backend: BackendIdentity,
    pub capability_matrix: CapabilityMatrixIdentity,
    pub complete: bool,
    pub degradations: Vec<FeatureDegradation>,
}

fn autonomy_degradations() -> Vec<FeatureDegradation> {
    vec![
        FeatureDegradation::autonomy(
            "background-jobs",
            "durable scheduling, recovery, budgets, notifications, and release evidence are incomplete",
            &["21.2", "21.6", "21.8", "21.9", "20.9"],
        ),
        FeatureDegradation::autonomy(
            "multi-agent",
            "child contracts, isolated worktrees, approvals, budgets, recovery, and review are incomplete",
            &["18.3", "21.3", "21.4", "21.5", "21.6", "21.10"],
        ),
        FeatureDegradation::autonomy(
            "unattended-writes",
            "Agent mutation remains read-only until permission, sandbox, approval, checkpoint, and recovery gates complete",
            &["15.3", "16.7", "18.3", "18.4", "18.5", "18.6"],
        ),
    ]
}

fn snapshot(
    backend: BackendIdentity,
    mut degradations: Vec<FeatureDegradation>,
) -> RuntimeDegradationSnapshot {
    degradations.extend(autonomy_degradations());
    let value = RuntimeDegradationSnapshot {
        schema_version: "runtime-degradations/0.2",
        backend,
        capability_matrix: capability_matrix(),
        complete: true,
        degradations,
    };
    value
        .validate()
        .expect("static degradation snapshot is valid");
    value
}

pub fn codex_snapshot() -> RuntimeDegradationSnapshot {
    snapshot(
        BackendIdentity {
            kind: BackendKind::Codex,
            adapter: CODEX_ADAPTER,
            version: CODEX_VERSION,
            status: "ready",
        },
        vec![
            FeatureDegradation::fixed(
                "agent-mutation",
                FeatureState::Disabled,
                "Aegisy Codex sessions use read-only sandbox and never approve writes or mutating commands",
                FeatureScope::Runtime,
            ),
            FeatureDegradation::fixed(
                "provider-thread-item-content",
                FeatureState::MetadataOnly,
                "provider thread list/read omit raw rollout items until stable AAP item mappings exist",
                FeatureScope::Provider,
            ),
            FeatureDegradation::fixed(
                "provider-thread-delete",
                FeatureState::Blocked,
                "requires scoped user review, recovery, retention, and compensation",
                FeatureScope::Provider,
            ),
            FeatureDegradation::fixed(
                "provider-thread-compact",
                FeatureState::Blocked,
                "requires a durable checkpoint, preservation review, and failure recovery",
                FeatureScope::Provider,
            ),
            FeatureDegradation::runtime_only(
                "turn.steer.same-turn",
                "Codex runtime supports same-turn steering but the desktop surface is not complete",
            ),
            FeatureDegradation::runtime_only(
                "session.provider.lifecycle.list-read",
                "Codex runtime supports provider list/read metadata but the desktop surface is not complete",
            ),
        ],
    )
}

pub fn preview_snapshot() -> RuntimeDegradationSnapshot {
    snapshot(
        BackendIdentity {
            kind: BackendKind::Preview,
            adapter: "preview",
            version: env!("CARGO_PKG_VERSION"),
            status: "ready",
        },
        vec![FeatureDegradation::fixed(
            "codex-provider",
            FeatureState::Unavailable,
            "preview runtime does not launch a provider adapter",
            FeatureScope::Runtime,
        )],
    )
}

pub fn recovery_snapshot(
    diagnostic_version: &str,
) -> Result<RuntimeDegradationSnapshot, &'static str> {
    if diagnostic_version != RECOVERY_VERSION {
        return Err("recovery diagnostic schema version mismatch");
    }
    Ok(snapshot(
        BackendIdentity {
            kind: BackendKind::Recovery,
            adapter: "aegisy-workbench-store",
            version: RECOVERY_VERSION,
            status: "read-only-recovery",
        },
        vec![FeatureDegradation::fixed(
            "workbench-mutation",
            FeatureState::Disabled,
            "workbench is in read-only recovery",
            FeatureScope::Runtime,
        )],
    ))
}

pub fn unavailable_snapshot() -> RuntimeDegradationSnapshot {
    snapshot(
        BackendIdentity {
            kind: BackendKind::Unavailable,
            adapter: CODEX_ADAPTER,
            version: CODEX_VERSION,
            status: "unavailable",
        },
        vec![FeatureDegradation::fixed(
            "runtime-adapter",
            FeatureState::Unavailable,
            "provider adapter failed before becoming ready",
            FeatureScope::Runtime,
        )],
    )
}

impl RuntimeDegradationSnapshot {
    fn validate(&self) -> Result<(), &'static str> {
        if self.schema_version != "runtime-degradations/0.2"
            || !self.complete
            || !self.capability_matrix.complete
            || self.capability_matrix.identity != matrix_identity()
        {
            return Err("invalid snapshot or matrix identity");
        }
        if self.capability_matrix.schema_version != "codex-capability-matrix/0.1"
            || self.capability_matrix.adapter != CODEX_ADAPTER
            || self.capability_matrix.codex_version != CODEX_VERSION
            || self.capability_matrix.vendor_schema_version != VENDOR_SCHEMA_VERSION
            || self.capability_matrix.vendor_schema_sha256 != VENDOR_SCHEMA_SHA256
            || self.capability_matrix.client_request_count != CLIENT_REQUEST_COUNT
            || self.capability_matrix.server_notification_count != SERVER_NOTIFICATION_COUNT
            || self.capability_matrix.thread_item_count != THREAD_ITEM_COUNT
        {
            return Err("capability matrix metadata drift");
        }
        let expected_backend = match self.backend.kind {
            BackendKind::Codex => (CODEX_ADAPTER, CODEX_VERSION, "ready"),
            BackendKind::Preview => ("preview", env!("CARGO_PKG_VERSION"), "ready"),
            BackendKind::Recovery => (
                "aegisy-workbench-store",
                RECOVERY_VERSION,
                "read-only-recovery",
            ),
            BackendKind::Unavailable => (CODEX_ADAPTER, CODEX_VERSION, "unavailable"),
        };
        if (
            self.backend.adapter,
            self.backend.version,
            self.backend.status,
        ) != expected_backend
        {
            return Err("backend identity drift");
        }
        let mut features = BTreeSet::new();
        for feature in &self.degradations {
            if !features.insert(feature.feature) || feature.authority_granted {
                return Err("duplicate feature or unexpected authority");
            }
            if feature.state == FeatureState::RuntimeOnly
                && (feature.runtime_supported != Some(true)
                    || feature.desktop_surface_available != Some(false))
            {
                return Err("runtime-only feature has invalid surface state");
            }
            let valid_scope = match feature.state {
                FeatureState::MetadataOnly | FeatureState::Blocked | FeatureState::RuntimeOnly => {
                    feature.scope == FeatureScope::Provider
                }
                FeatureState::Disabled | FeatureState::Unavailable => {
                    feature.scope == FeatureScope::Runtime
                }
            };
            if !valid_scope {
                return Err("feature state and scope disagree");
            }
            if matches!(
                feature.feature,
                "background-jobs" | "multi-agent" | "unattended-writes"
            ) && (feature.availability != Some("not-advertised")
                || feature.stable_enabled != Some(false)
                || feature.override_available != Some(false)
                || feature.missing_gates.as_ref().is_none_or(Vec::is_empty))
            {
                return Err("autonomy gate is incomplete");
            }
        }
        if self.backend.kind == BackendKind::Unavailable
            && self.degradations.iter().any(|feature| {
                feature.scope == FeatureScope::Provider || feature.feature.starts_with("timeline.")
            })
        {
            return Err("unavailable backend exposes provider or timeline state");
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;

    const SCHEMA: &str =
        include_str!("../../../aap-schema/codex-app-server-0.144.5/v2.schemas.json");

    fn schema_names(definition: &str, discriminator: &str) -> Vec<String> {
        let schema: Value = serde_json::from_str(SCHEMA).unwrap();
        schema["definitions"][definition]["oneOf"]
            .as_array()
            .unwrap()
            .iter()
            .map(|entry| {
                entry["properties"][discriminator]["enum"][0]
                    .as_str()
                    .unwrap()
                    .to_owned()
            })
            .collect()
    }

    fn assert_exact_coverage(
        domain: VendorDomain,
        expected: &'static [&'static str],
        schema_names: Vec<String>,
    ) {
        assert_eq!(expected.len(), schema_names.len());
        let expected_set = expected.iter().copied().collect::<BTreeSet<_>>();
        assert_eq!(
            expected_set.len(),
            expected.len(),
            "duplicate classification"
        );
        let schema_set = schema_names
            .iter()
            .map(String::as_str)
            .collect::<BTreeSet<_>>();
        assert_eq!(
            expected_set, schema_set,
            "vendor schema classification drift"
        );
        for name in expected {
            let entry = classify(domain, name);
            assert_eq!(entry.name, *name);
            assert!(!entry.authority_granted);
            if matches!(entry.status, MappingStatus::RuntimeOnly) {
                assert!(!entry.desktop_surface_available);
            }
            if matches!(
                entry.status,
                MappingStatus::Blocked
                    | MappingStatus::Unsupported
                    | MappingStatus::ExperimentalDisabled
            ) {
                assert!(!entry.desktop_surface_available);
            }
        }
    }

    #[test]
    fn pinned_schema_hash_and_counts_are_exact() {
        assert_eq!(
            format!("{:x}", Sha256::digest(SCHEMA.as_bytes())),
            VENDOR_SCHEMA_SHA256
        );
        assert_eq!(CLIENT_REQUEST_NAMES.len(), CLIENT_REQUEST_COUNT);
        assert_eq!(SERVER_NOTIFICATION_NAMES.len(), SERVER_NOTIFICATION_COUNT);
        assert_eq!(THREAD_ITEM_NAMES.len(), THREAD_ITEM_COUNT);
        assert_exact_coverage(
            VendorDomain::ClientRequest,
            CLIENT_REQUEST_NAMES,
            schema_names("ClientRequest", "method"),
        );
        assert!(is_known_server_notification("turn/completed"));
        assert!(!is_known_server_notification("future/private-notification"));
        assert_exact_coverage(
            VendorDomain::ServerNotification,
            SERVER_NOTIFICATION_NAMES,
            schema_names("ServerNotification", "method"),
        );
        assert_exact_coverage(
            VendorDomain::ThreadItem,
            THREAD_ITEM_NAMES,
            schema_names("ThreadItem", "type"),
        );
    }

    #[test]
    fn matrix_identity_is_deterministic_and_experimental_api_is_disabled() {
        assert_eq!(
            matrix_identity(),
            "codex-capability-matrix:sha256:473ddd66cd30b903778c248f28aa55d3cfb2ff37123c4831a23a263703362d04"
        );
        let experimental = classifications()
            .filter(|entry| entry.name.starts_with("experimentalFeature/"))
            .collect::<Vec<_>>();
        assert_eq!(experimental.len(), 2);
        assert!(experimental
            .iter()
            .all(|entry| entry.status == MappingStatus::ExperimentalDisabled));
    }

    #[test]
    fn snapshots_are_strict_and_never_grant_authority() {
        let snapshots = [
            (codex_snapshot(), 9),
            (preview_snapshot(), 4),
            (recovery_snapshot(RECOVERY_VERSION).unwrap(), 4),
            (unavailable_snapshot(), 4),
        ];
        for (snapshot, feature_count) in snapshots {
            snapshot.validate().unwrap();
            assert!(snapshot.complete);
            assert!(snapshot.capability_matrix.complete);
            assert_eq!(snapshot.degradations.len(), feature_count);
            assert!(snapshot
                .degradations
                .iter()
                .all(|feature| !feature.authority_granted));
            for gate in ["background-jobs", "multi-agent", "unattended-writes"] {
                let gate = snapshot
                    .degradations
                    .iter()
                    .find(|feature| feature.feature == gate)
                    .unwrap();
                assert_eq!(gate.availability, Some("not-advertised"));
                assert_eq!(gate.stable_enabled, Some(false));
                assert_eq!(gate.override_available, Some(false));
                assert!(!gate.missing_gates.as_ref().unwrap().is_empty());
            }
        }
        assert!(recovery_snapshot("workbench-recovery-diagnostic/0.2").is_err());

        let mut drifted = codex_snapshot();
        drifted.capability_matrix.client_request_count += 1;
        assert!(drifted.validate().is_err());
        let mut drifted = codex_snapshot();
        drifted.capability_matrix.vendor_schema_sha256 = "invalid";
        assert!(drifted.validate().is_err());
        let mut drifted = codex_snapshot();
        drifted.backend.version = "codex-cli 0.144.6";
        assert!(drifted.validate().is_err());
        let mut drifted = codex_snapshot();
        drifted.degradations[2].scope = FeatureScope::Runtime;
        assert!(drifted.validate().is_err());
        let mut drifted = codex_snapshot();
        drifted.degradations[2].authority_granted = true;
        assert!(drifted.validate().is_err());
    }

    #[test]
    fn runtime_only_support_is_not_desktop_reachability() {
        let snapshot = codex_snapshot();
        for feature in [
            "turn.steer.same-turn",
            "session.provider.lifecycle.list-read",
        ] {
            let feature = snapshot
                .degradations
                .iter()
                .find(|entry| entry.feature == feature)
                .unwrap();
            assert_eq!(feature.state, FeatureState::RuntimeOnly);
            assert_eq!(feature.runtime_supported, Some(true));
            assert_eq!(feature.desktop_surface_available, Some(false));
        }
    }

    #[test]
    fn backend_feature_states_and_scopes_are_fixed() {
        let codex = codex_snapshot();
        let actual = codex.degradations[..6]
            .iter()
            .map(|entry| (entry.feature, entry.state, entry.scope))
            .collect::<Vec<_>>();
        assert_eq!(
            actual,
            vec![
                (
                    "agent-mutation",
                    FeatureState::Disabled,
                    FeatureScope::Runtime
                ),
                (
                    "provider-thread-item-content",
                    FeatureState::MetadataOnly,
                    FeatureScope::Provider,
                ),
                (
                    "provider-thread-delete",
                    FeatureState::Blocked,
                    FeatureScope::Provider,
                ),
                (
                    "provider-thread-compact",
                    FeatureState::Blocked,
                    FeatureScope::Provider,
                ),
                (
                    "turn.steer.same-turn",
                    FeatureState::RuntimeOnly,
                    FeatureScope::Provider,
                ),
                (
                    "session.provider.lifecycle.list-read",
                    FeatureState::RuntimeOnly,
                    FeatureScope::Provider,
                ),
            ]
        );
        for (snapshot, expected) in [
            (
                preview_snapshot(),
                ("codex-provider", FeatureState::Unavailable),
            ),
            (
                recovery_snapshot(RECOVERY_VERSION).unwrap(),
                ("workbench-mutation", FeatureState::Disabled),
            ),
            (
                unavailable_snapshot(),
                ("runtime-adapter", FeatureState::Unavailable),
            ),
        ] {
            assert_eq!(
                (
                    snapshot.degradations[0].feature,
                    snapshot.degradations[0].state
                ),
                expected
            );
            assert_eq!(snapshot.degradations[0].scope, FeatureScope::Runtime);
        }
    }

    #[test]
    fn unavailable_snapshot_has_no_provider_or_timeline_capability() {
        let snapshot = unavailable_snapshot();
        assert!(snapshot.degradations.iter().all(|feature| {
            feature.scope != FeatureScope::Provider && !feature.feature.starts_with("timeline.")
        }));
    }
}
