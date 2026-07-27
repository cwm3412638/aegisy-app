use serde_json::{Map, Value};
use std::collections::HashSet;
use std::fs;
use std::path::{Component, Path, PathBuf};

fn package_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../aap-schema")
}

fn read_json(path: &Path) -> Value {
    serde_json::from_str(&fs::read_to_string(path).expect("schema package file must be readable"))
        .expect("schema package file must contain valid JSON")
}

fn exact_keys(object: &Map<String, Value>, expected: &[&str]) -> bool {
    object.len() == expected.len() && expected.iter().all(|key| object.contains_key(*key))
}

fn package_file(root: &Path, relative: &str) -> PathBuf {
    let relative_path = Path::new(relative);
    assert!(!relative.is_empty() && !relative.contains('\\'));
    assert!(!relative_path.is_absolute());
    assert!(relative_path
        .components()
        .all(|component| matches!(component, Component::Normal(_))));

    let path = root.join(relative_path);
    let metadata = fs::symlink_metadata(&path).expect("registered package path must exist");
    assert!(!metadata.file_type().is_symlink());
    let canonical_root = root.canonicalize().expect("schema package root must exist");
    let canonical_path = path
        .canonicalize()
        .expect("registered package path must resolve");
    assert!(canonical_path.starts_with(canonical_root));
    canonical_path
}

fn assert_no_experimental_reference(value: &Value) {
    match value {
        Value::Object(object) => object.values().for_each(assert_no_experimental_reference),
        Value::Array(values) => values.iter().for_each(assert_no_experimental_reference),
        Value::String(value) => {
            assert!(!value.contains("/experimental/"));
            assert!(!value.starts_with("experimental/"));
            assert!(!value.starts_with("../experimental/"));
        }
        _ => {}
    }
}

#[test]
fn schema_package_declares_isolated_stable_and_experimental_namespaces() {
    let root = package_root();
    let package = read_json(&root.join("package.json"));
    let package = package
        .as_object()
        .expect("package manifest must be an object");
    assert!(exact_keys(
        package,
        &[
            "name",
            "version",
            "private",
            "description",
            "license",
            "scripts",
            "files",
            "aegisy",
        ]
    ));
    assert_eq!(package["name"], "@aegisy/aap-schema");
    assert_eq!(package["version"], "0.1.0");
    assert_eq!(package["private"], true);
    assert_eq!(package["license"], "UNLICENSED");
    assert_eq!(
        package["scripts"],
        serde_json::json!({
            "generate": "node scripts/generate-core-types.mjs",
            "generate:check": "node scripts/generate-core-types.mjs --check",
            "test:generator": "node scripts/test-core-generator-inputs.mjs"
        })
    );
    assert_eq!(
        package["files"],
        serde_json::json!([
            "README.md",
            "stable",
            "experimental",
            "fixtures",
            "generated",
            "scripts"
        ])
    );

    let aegisy = package["aegisy"]
        .as_object()
        .expect("package metadata must be an object");
    assert!(exact_keys(aegisy, &["schema_version", "namespaces"]));
    assert_eq!(aegisy["schema_version"], "aap-schema-package/0.1");
    let namespaces = aegisy["namespaces"]
        .as_array()
        .expect("namespace registry list must be an array");
    assert_eq!(namespaces.len(), 2);

    let mut names = HashSet::new();
    let mut registries = HashSet::new();
    for namespace in namespaces {
        let namespace = namespace
            .as_object()
            .expect("namespace entry must be an object");
        assert!(exact_keys(namespace, &["name", "registry"]));
        let name = namespace["name"]
            .as_str()
            .expect("namespace name must be a string");
        let registry = namespace["registry"]
            .as_str()
            .expect("namespace registry must be a string");
        assert!(matches!(name, "stable" | "experimental"));
        assert!(registry.starts_with(&format!("{name}/")));
        assert!(names.insert(name));
        assert!(registries.insert(registry));
        assert!(package_file(&root, registry).is_file());
    }
    assert_eq!(names, HashSet::from(["stable", "experimental"]));
}

#[test]
fn stable_registry_matches_paths_versions_and_schema_ids() {
    let root = package_root();
    let registry_path = package_file(&root, "stable/namespace.json");
    let registry = read_json(&registry_path);
    let registry = registry
        .as_object()
        .expect("stable registry must be an object");
    assert!(exact_keys(
        registry,
        &[
            "schema_version",
            "namespace",
            "compatibility",
            "wire_available",
            "versions",
        ]
    ));
    assert_eq!(registry["schema_version"], "aap-schema-namespace/0.1");
    assert_eq!(registry["namespace"], "stable");
    assert_eq!(registry["compatibility"], "additive-only");
    assert_eq!(registry["wire_available"], true);

    let namespace_root = registry_path.parent().expect("registry must have a parent");
    let versions = registry["versions"]
        .as_array()
        .expect("stable versions must be an array");
    assert!(!versions.is_empty());
    let mut protocol_versions = HashSet::new();
    let mut schema_ids = HashSet::new();
    let mut component_roles = HashSet::new();
    for version in versions {
        let version = version
            .as_object()
            .expect("stable version must be an object");
        assert!(exact_keys(
            version,
            &[
                "protocol_version",
                "directory",
                "schema",
                "schema_id",
                "components",
            ]
        ));
        let protocol_version = version["protocol_version"]
            .as_str()
            .expect("protocol version must be a string");
        let directory = version["directory"]
            .as_str()
            .expect("directory version must be a string");
        assert_eq!(directory, format!("v{protocol_version}"));
        assert!(protocol_versions.insert(protocol_version));
        let schema_path = version["schema"]
            .as_str()
            .expect("transport schema path must be a string");
        let schema_id = version["schema_id"]
            .as_str()
            .expect("transport schema ID must be a string");
        assert_eq!(schema_path, format!("{directory}/aap.schema.json"));
        assert!(schema_ids.insert(schema_id));
        let schema_path = package_file(namespace_root, schema_path);
        let schema = read_json(&schema_path);
        assert_eq!(schema["$id"], schema_id);
        assert_eq!(
            schema_id,
            &format!("https://aegisy.cc/schemas/aap/stable/{directory}/aap.schema.json")
        );
        jsonschema::validator_for(&schema).expect("transport JSON Schema must compile");
        assert_no_experimental_reference(&schema);

        let components = version["components"]
            .as_array()
            .expect("stable schema component list must be an array");
        assert!(!components.is_empty());
        for schema_entry in components {
            let schema_entry = schema_entry
                .as_object()
                .expect("stable schema component must be an object");
            assert!(exact_keys(schema_entry, &["role", "path", "schema_id"]));
            let role = schema_entry["role"]
                .as_str()
                .expect("stable schema role must be a string");
            let schema_path = schema_entry["path"]
                .as_str()
                .expect("schema path must be a string");
            let schema_id = schema_entry["schema_id"]
                .as_str()
                .expect("schema ID must be a string");
            assert_eq!(role, "core-domains");
            assert!(schema_path.starts_with(&format!("{directory}/")));
            assert!(component_roles.insert((protocol_version, role)));
            assert!(schema_ids.insert(schema_id));

            let schema_path = package_file(namespace_root, schema_path);
            assert!(schema_path.is_file());
            let schema = read_json(&schema_path);
            assert_eq!(schema["$id"], schema_id);
            let file_name = schema_path
                .file_name()
                .and_then(|value| value.to_str())
                .expect("stable schema file name must be UTF-8");
            assert_eq!(
                schema_id,
                &format!("https://aegisy.cc/schemas/aap/stable/{directory}/{file_name}")
            );
            jsonschema::validator_for(&schema).expect("stable JSON Schema must compile");
            assert_no_experimental_reference(&schema);
        }
    }
    assert_eq!(component_roles, HashSet::from([("0.1", "core-domains")]));
}

#[test]
fn experimental_namespace_is_explicitly_empty_and_unavailable() {
    let root = package_root();
    let registry = read_json(&package_file(&root, "experimental/namespace.json"));
    let registry = registry
        .as_object()
        .expect("experimental registry must be an object");
    assert!(exact_keys(
        registry,
        &[
            "schema_version",
            "namespace",
            "compatibility",
            "wire_available",
            "versions",
        ]
    ));
    assert_eq!(registry["schema_version"], "aap-schema-namespace/0.1");
    assert_eq!(registry["namespace"], "experimental");
    assert_eq!(registry["compatibility"], "none");
    assert_eq!(registry["wire_available"], false);
    assert_eq!(registry["versions"], Value::Array(Vec::new()));
}

fn definition_document(schema: &Value, definition: &str) -> Value {
    assert!(schema["$defs"].get(definition).is_some());
    serde_json::json!({
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$ref": format!("#/$defs/{definition}"),
        "$defs": schema["$defs"].clone()
    })
}

fn is_valid_definition(schema: &Value, definition: &str, value: &Value) -> bool {
    let document = definition_document(schema, definition);
    jsonschema::validator_for(&document)
        .unwrap_or_else(|error| panic!("{definition} Schema must compile: {error}"))
        .is_valid(value)
}

fn fixture_value<'a>(fixture: &'a Value, key: &str) -> &'a Value {
    fixture
        .get(key)
        .unwrap_or_else(|| panic!("fixture must contain {key}"))
}

fn with_extra_property(value: &Value) -> Value {
    let mut invalid = value.clone();
    invalid
        .as_object_mut()
        .expect("domain fixture must be an object")
        .insert("unknown".into(), Value::Bool(true));
    invalid
}

fn with_data_key(value: &Value, key: String) -> Value {
    let mut candidate = value.clone();
    let mut data = Map::new();
    data.insert(key, Value::Bool(true));
    candidate["data"] = Value::Object(data);
    candidate
}

fn core_fixture() -> (Value, Value) {
    let root = package_root();
    (
        read_json(&package_file(&root, "stable/v0.1/core.schema.json")),
        read_json(&package_file(&root, "fixtures/aap-core-domains.json")),
    )
}

#[test]
fn core_domain_fixture_validates_each_real_definition() {
    let (schema, fixture) = core_fixture();
    let definitions = [
        ("project", "project"),
        ("projectRoot", "projectRoot"),
        ("projectNavigationEntry", "projectNavigationEntry"),
        ("session", "sessionChat"),
        ("session", "sessionWork"),
        ("sessionProjection", "sessionProjectionChat"),
        ("sessionProjection", "sessionProjectionWork"),
        ("turn", "turn"),
        ("turnStartAcknowledgement", "turnStartAcknowledgement"),
        ("item", "item"),
        ("sessionHistoryItem", "sessionHistoryItem"),
        ("runtimeIdentity", "runtimeIdentity"),
        ("runtimeBackend", "runtimeBackend"),
        ("runtimeLiveBinding", "runtimeLiveBinding"),
        ("runtimeSearchProjection", "runtimeSearchProjection"),
        ("runtimeReplayProjection", "runtimeReplayProjection"),
        ("runtimeReplayFallback", "runtimeReplayFallback"),
        ("approvalAcknowledgement", "approvalAcknowledgement"),
        ("error", "error"),
        ("error", "errorProvider"),
        ("providerError", "providerError"),
        ("usage", "usage"),
        ("usage", "usageObserved"),
        ("commandOutputArtifact", "commandOutputArtifact"),
        ("capabilitySet", "capabilitySet"),
    ];
    for (definition, key) in definitions {
        assert!(
            is_valid_definition(&schema, definition, fixture_value(&fixture, key)),
            "{key} must validate as {definition}"
        );
    }
    for runtime_key in [
        "runtimeIdentity",
        "runtimeBackend",
        "runtimeLiveBinding",
        "runtimeSearchProjection",
        "runtimeReplayProjection",
        "runtimeReplayFallback",
    ] {
        assert!(is_valid_definition(
            &schema,
            "runtime",
            fixture_value(&fixture, runtime_key)
        ));
    }
    let workspaces = fixture["workspaceStates"]
        .as_array()
        .expect("workspaceStates must be an array");
    assert_eq!(workspaces.len(), 7);
    for workspace in workspaces {
        assert!(is_valid_definition(&schema, "workspace", workspace));
    }

    let approval: aegisy_agentd::approval_ack::ApprovalAcknowledgement =
        serde_json::from_value(fixture["approvalAcknowledgement"].clone())
            .expect("approval fixture must pass the typed identity contract");
    approval
        .validate()
        .expect("approval fixture must remain authority-free");
    for key in ["usage", "usageObserved"] {
        let usage: aegisy_agentd::usage_authority::UsageAuthorityReport =
            serde_json::from_value(fixture[key].clone())
                .unwrap_or_else(|error| panic!("{key} must deserialize: {error}"));
        usage
            .validate()
            .unwrap_or_else(|error| panic!("{key} must pass typed validation: {}", error.code));
    }
}

#[test]
fn core_objects_reject_unknown_fields_and_cross_domain_state_drift() {
    let (schema, fixture) = core_fixture();
    let definitions = [
        ("project", "project"),
        ("projectRoot", "projectRoot"),
        ("projectNavigationEntry", "projectNavigationEntry"),
        ("session", "sessionChat"),
        ("session", "sessionWork"),
        ("sessionProjection", "sessionProjectionChat"),
        ("sessionProjection", "sessionProjectionWork"),
        ("turn", "turn"),
        ("turnStartAcknowledgement", "turnStartAcknowledgement"),
        ("item", "item"),
        ("sessionHistoryItem", "sessionHistoryItem"),
        ("runtimeIdentity", "runtimeIdentity"),
        ("runtimeBackend", "runtimeBackend"),
        ("runtimeLiveBinding", "runtimeLiveBinding"),
        ("runtimeSearchProjection", "runtimeSearchProjection"),
        ("runtimeReplayProjection", "runtimeReplayProjection"),
        ("runtimeReplayFallback", "runtimeReplayFallback"),
        ("approvalAcknowledgement", "approvalAcknowledgement"),
        ("error", "error"),
        ("error", "errorProvider"),
        ("providerError", "providerError"),
        ("usage", "usage"),
        ("usage", "usageObserved"),
        ("commandOutputArtifact", "commandOutputArtifact"),
        ("capabilitySet", "capabilitySet"),
    ];
    for (definition, key) in definitions {
        assert!(
            !is_valid_definition(
                &schema,
                definition,
                &with_extra_property(fixture_value(&fixture, key))
            ),
            "{definition} must reject an unknown field"
        );
    }
    for workspace in fixture["workspaceStates"].as_array().unwrap() {
        assert!(!is_valid_definition(
            &schema,
            "workspace",
            &with_extra_property(workspace)
        ));
    }

    let mut chat = fixture["sessionChat"].clone();
    chat["project_id"] = Value::String("project-1".into());
    assert!(!is_valid_definition(&schema, "session", &chat));
    let mut work = fixture["sessionWork"].clone();
    work["project_id"] = Value::Null;
    assert!(!is_valid_definition(&schema, "session", &work));

    let mut chat_projection = fixture["sessionProjectionChat"].clone();
    chat_projection["project_id"] = Value::String("project-1".into());
    assert!(!is_valid_definition(
        &schema,
        "sessionProjection",
        &chat_projection
    ));
    let mut new_projection = fixture["sessionProjectionWork"].clone();
    new_projection["lineage_kind"] = Value::String("new".into());
    assert!(!is_valid_definition(
        &schema,
        "sessionProjection",
        &new_projection
    ));

    let mut turn = fixture["turn"].clone();
    turn["state"] = Value::String("started".into());
    assert!(!is_valid_definition(&schema, "turn", &turn));
    let mut acknowledgement = fixture["turnStartAcknowledgement"].clone();
    acknowledgement["state"] = Value::String("running".into());
    assert!(!is_valid_definition(
        &schema,
        "turnStartAcknowledgement",
        &acknowledgement
    ));
}

#[test]
fn core_item_matches_transport_item_contract() {
    let root = package_root();
    let (core_schema, fixture) = core_fixture();
    let transport_schema = read_json(&package_file(&root, "stable/v0.1/aap.schema.json"));
    let item = fixture["item"].clone();
    assert!(is_valid_definition(&core_schema, "item", &item));
    assert!(is_valid_definition(
        &transport_schema,
        "timelineItem",
        &item
    ));

    let mut invalid_values = Vec::new();
    let mut invalid = item.clone();
    invalid["role"] = Value::String("provider".into());
    invalid_values.push(invalid);
    let mut invalid = item.clone();
    invalid["kind"] = Value::String("a".repeat(65));
    invalid_values.push(invalid);
    let mut invalid = item.clone();
    invalid["data"]["unsafe_integer"] = Value::from(9_007_199_254_740_992_u64);
    invalid_values.push(invalid);
    invalid_values.push(with_extra_property(&item));

    for invalid in invalid_values {
        assert!(!is_valid_definition(&core_schema, "item", &invalid));
        assert!(!is_valid_definition(
            &transport_schema,
            "timelineItem",
            &invalid
        ));
    }
}

#[test]
fn session_history_item_preserves_core_item_data_key_contract() {
    let (schema, fixture) = core_fixture();
    let item = fixture["item"].clone();
    let history = fixture["sessionHistoryItem"].clone();

    let valid_key = "a".repeat(128);
    let valid_item = with_data_key(&item, valid_key.clone());
    let valid_history = with_data_key(&history, valid_key);
    assert!(is_valid_definition(&schema, "item", &valid_item));
    assert!(is_valid_definition(
        &schema,
        "sessionHistoryItem",
        &valid_history
    ));

    for key in [
        "a".repeat(129),
        "unicode-\u{754c}".into(),
        "control\nkey".into(),
    ] {
        let invalid_item = with_data_key(&item, key.clone());
        let invalid_history = with_data_key(&history, key);
        assert!(!is_valid_definition(&schema, "item", &invalid_item));
        assert!(!is_valid_definition(
            &schema,
            "sessionHistoryItem",
            &invalid_history
        ));
    }
}

#[test]
fn core_runtime_handshake_shapes_match_transport_bounds() {
    let root = package_root();
    let (core_schema, fixture) = core_fixture();
    let transport_schema = read_json(&package_file(&root, "stable/v0.1/aap.schema.json"));

    for length in [1, 64] {
        let mut identity = fixture["runtimeIdentity"].clone();
        identity["name"] = Value::String("a".repeat(length));
        assert!(is_valid_definition(
            &core_schema,
            "runtimeIdentity",
            &identity
        ));
        assert!(is_valid_definition(
            &transport_schema,
            "identity",
            &identity
        ));

        let mut backend = fixture["runtimeBackend"].clone();
        backend["adapter"] = Value::String("a".repeat(length));
        assert!(is_valid_definition(
            &core_schema,
            "runtimeBackend",
            &backend
        ));
        assert!(is_valid_definition(&transport_schema, "backend", &backend));
    }

    let mut identity = fixture["runtimeIdentity"].clone();
    identity["name"] = Value::String("a".repeat(65));
    assert!(!is_valid_definition(
        &core_schema,
        "runtimeIdentity",
        &identity
    ));
    assert!(!is_valid_definition(
        &transport_schema,
        "identity",
        &identity
    ));

    let mut backend = fixture["runtimeBackend"].clone();
    backend["adapter"] = Value::String("a".repeat(65));
    assert!(!is_valid_definition(
        &core_schema,
        "runtimeBackend",
        &backend
    ));
    assert!(!is_valid_definition(&transport_schema, "backend", &backend));
}

fn assert_enum_preserves(schema: &Value, pointer: &str, baseline: &[&str]) {
    let values = schema
        .pointer(pointer)
        .and_then(Value::as_array)
        .unwrap_or_else(|| panic!("stable enum must exist at {pointer}"));
    for expected in baseline {
        assert!(
            values.iter().any(|value| value == expected),
            "stable enum at {pointer} removed {expected}"
        );
    }
}

#[test]
fn stable_core_enums_and_bounds_have_an_additive_compatibility_baseline() {
    let (schema, _) = core_fixture();
    let enum_baselines: &[(&str, &[&str])] = &[
        (
            "/$defs/projectRoot/properties/access/enum",
            &["read", "write"],
        ),
        (
            "/$defs/projectRoot/properties/availability/enum",
            &["available", "unavailable"],
        ),
        (
            "/$defs/projectNavigationEntry/properties/state/enum",
            &["active", "archived", "unavailable"],
        ),
        (
            "/$defs/projectNavigationEntry/properties/availability/enum",
            &["available", "unavailable"],
        ),
        ("/$defs/session/properties/mode/enum", &["chat", "work"]),
        (
            "/$defs/sessionProjection/properties/mode/enum",
            &["chat", "work"],
        ),
        (
            "/$defs/sessionProjection/properties/lineage_kind/enum",
            &["new", "resume", "fork"],
        ),
        (
            "/$defs/sessionProjection/properties/status/enum",
            &["active", "archived", "failed", "interrupted"],
        ),
        (
            "/$defs/turn/properties/state/enum",
            &["running", "completed", "failed", "interrupted"],
        ),
        (
            "/$defs/turnStartAcknowledgement/properties/state/enum",
            &["started", "terminal"],
        ),
        (
            "/$defs/item/properties/role/enum",
            &["user", "agent", "system", "tool"],
        ),
        (
            "/$defs/item/properties/state/enum",
            &[
                "started",
                "running",
                "delta",
                "updated",
                "completed",
                "failed",
                "interrupted",
                "truncated",
                "unavailable",
            ],
        ),
        (
            "/$defs/sessionHistoryItem/properties/role/enum",
            &["user", "agent", "system", "tool"],
        ),
        (
            "/$defs/sessionHistoryItem/properties/state/enum",
            &[
                "started",
                "running",
                "delta",
                "updated",
                "completed",
                "failed",
                "interrupted",
                "truncated",
                "unavailable",
            ],
        ),
        (
            "/$defs/runtimeBackend/properties/status/enum",
            &["ready", "unavailable", "read-only-recovery"],
        ),
        (
            "/$defs/workspace/properties/git_state/enum",
            &[
                "unavailable",
                "not-repository",
                "repository-only",
                "worktree",
            ],
        ),
        (
            "/$defs/approvalAcknowledgement/properties/scope/enum",
            &["command-execution", "file-change", "permissions"],
        ),
        (
            "/$defs/approvalAcknowledgement/properties/state/enum",
            &["requested", "resolved", "failed", "reconciliation-required"],
        ),
        (
            "/$defs/approvalAcknowledgement/properties/resolution/enum",
            &["unresolved", "denied", "expired", "not-required"],
        ),
        (
            "/$defs/runtimeErrorClass/enum",
            &[
                "protocol",
                "provider",
                "adapter",
                "transport",
                "timeout",
                "sandbox",
                "policy",
                "tool",
                "storage",
                "workspace",
                "git",
                "budget",
            ],
        ),
        (
            "/$defs/providerError/properties/kind/enum",
            &[
                "request-timeout",
                "unauthorized",
                "bad-request",
                "rate-limit",
                "server-overloaded",
                "http-error",
                "context-window-exceeded",
                "session-budget-exceeded",
                "usage-limit-exceeded",
                "cyber-policy",
                "internal-server-error",
                "thread-rollback-failed",
                "sandbox-error",
                "http-connection-failed",
                "response-stream-connection-failed",
                "response-stream-disconnected",
                "response-too-many-failed-attempts",
                "active-turn-not-steerable",
                "provider-error",
            ],
        ),
        (
            "/$defs/usageEvidence/oneOf/0/properties/source/enum",
            &[
                "provider-response",
                "runtime-observation",
                "gateway-observation",
            ],
        ),
        (
            "/$defs/usageEvidence/oneOf/3/properties/previous_authority/enum",
            &["observed", "catalog-derived", "estimated"],
        ),
        (
            "/$defs/usageEvidence/oneOf/3/properties/reason/enum",
            &[
                "expired",
                "source-unavailable",
                "model-changed",
                "routing-changed",
            ],
        ),
        (
            "/$defs/usageEvidence/oneOf/4/properties/missing_source/enum",
            &[
                "provider-usage",
                "runtime-observation",
                "catalog",
                "estimator",
                "correlation",
            ],
        ),
        (
            "/$defs/usageEvidence/oneOf/4/properties/reason/enum",
            &[
                "not-reported",
                "unavailable",
                "not-applicable",
                "correlation-missing",
            ],
        ),
        (
            "/$defs/usageEntry/properties/metric/enum",
            &["token", "context", "cost", "reasoning"],
        ),
        (
            "/$defs/usageEntry/properties/authority/enum",
            &[
                "observed",
                "catalog-derived",
                "estimated",
                "stale",
                "unknown",
            ],
        ),
    ];
    for (pointer, baseline) in enum_baselines {
        assert_enum_preserves(&schema, pointer, baseline);
    }

    for (pointer, expected) in [
        (
            "/$defs/safeNonNegativeInteger/maximum",
            serde_json::json!(9_007_199_254_740_991_u64),
        ),
        (
            "/$defs/safePositiveInteger/maximum",
            serde_json::json!(9_007_199_254_740_991_u64),
        ),
        ("/$defs/boundedLabel/maxLength", serde_json::json!(256)),
        ("/$defs/boundedPath/maxLength", serde_json::json!(4096)),
        ("/$defs/boundedBranch/maxLength", serde_json::json!(512)),
        ("/$defs/capabilityName/maxLength", serde_json::json!(128)),
        (
            "/$defs/runtimeComponentName/maxLength",
            serde_json::json!(64),
        ),
        (
            "/$defs/item/properties/kind/maxLength",
            serde_json::json!(64),
        ),
        (
            "/$defs/item/properties/content/maxLength",
            serde_json::json!(65_536),
        ),
        (
            "/$defs/item/properties/data/propertyNames/maxLength",
            serde_json::json!(128),
        ),
        (
            "/$defs/sessionHistoryItem/properties/data/propertyNames/maxLength",
            serde_json::json!(128),
        ),
        (
            "/$defs/providerError/properties/http_status/maximum",
            serde_json::json!(65_535),
        ),
        (
            "/$defs/commandOutputArtifact/properties/content/maxLength",
            serde_json::json!(2_097_280),
        ),
    ] {
        assert_eq!(
            schema.pointer(pointer),
            Some(&expected),
            "stable bound drifted at {pointer}"
        );
    }
}

#[test]
fn workspace_schema_enforces_git_states_and_false_authority() {
    let (schema, fixture) = core_fixture();
    let states = fixture["workspaceStates"].as_array().unwrap();
    for state in states {
        assert!(is_valid_definition(&schema, "workspace", state));
    }
    let worktree = &states[3];
    for head in ["d".repeat(40), "e".repeat(64)] {
        let mut valid = worktree.clone();
        valid["head_oid"] = Value::String(head);
        assert!(is_valid_definition(&schema, "workspace", &valid));
    }
    for length in 41..=63 {
        let mut invalid = worktree.clone();
        invalid["head_oid"] = Value::String("a".repeat(length));
        assert!(!is_valid_definition(&schema, "workspace", &invalid));
    }
    let mut uppercase = worktree.clone();
    uppercase["head_oid"] = Value::String("A".repeat(40));
    assert!(!is_valid_definition(&schema, "workspace", &uppercase));

    for non_worktree in &states[..3] {
        for (field, value) in [
            ("branch", Value::String("main".into())),
            ("head_oid", Value::String("a".repeat(40))),
            ("detached", Value::Bool(true)),
            ("unborn", Value::Bool(true)),
        ] {
            let mut invalid = non_worktree.clone();
            invalid[field] = value;
            assert!(!is_valid_definition(&schema, "workspace", &invalid));
        }
    }

    let mut redacted = states[6].clone();
    redacted["branch"] = Value::String("secret-branch".into());
    assert!(!is_valid_definition(&schema, "workspace", &redacted));
    let mut detached = states[4].clone();
    detached["branch"] = Value::String("main".into());
    assert!(!is_valid_definition(&schema, "workspace", &detached));
    let mut unborn = states[5].clone();
    unborn["head_oid"] = Value::String("a".repeat(40));
    assert!(!is_valid_definition(&schema, "workspace", &unborn));
    let mut unborn = states[5].clone();
    unborn["detached"] = Value::Bool(true);
    assert!(!is_valid_definition(&schema, "workspace", &unborn));

    for field in [
        "dedicated_worktree",
        "raw_paths_included",
        "permission_granted",
    ] {
        let mut invalid = worktree.clone();
        invalid[field] = Value::Bool(true);
        assert!(!is_valid_definition(&schema, "workspace", &invalid));
    }
    assert_eq!(states[0]["captured_at_ms"], 0);
}

#[test]
fn approval_and_provider_errors_cannot_fabricate_authority_or_content() {
    let (schema, fixture) = core_fixture();
    let approval = &fixture["approvalAcknowledgement"];
    let mut invalid = approval.clone();
    invalid["resolution"] = Value::String("allowed".into());
    assert!(!is_valid_definition(
        &schema,
        "approvalAcknowledgement",
        &invalid
    ));
    for field in [
        "mutation_authority",
        "approval_authority",
        "user_decision_observed",
        "execution_authority",
    ] {
        let mut invalid = approval.clone();
        invalid[field] = Value::Bool(true);
        assert!(!is_valid_definition(
            &schema,
            "approvalAcknowledgement",
            &invalid
        ));
    }

    let error = &fixture["errorProvider"];
    let mut class_drift = error.clone();
    class_drift["class"] = Value::String("provider".into());
    assert!(!is_valid_definition(&schema, "error", &class_drift));
    let mut retry_drift = error.clone();
    retry_drift["retryable"] = Value::Bool(false);
    assert!(!is_valid_definition(&schema, "error", &retry_drift));

    for (field, value) in [
        ("body", Value::String("provider body".into())),
        ("message", Value::String("dynamic provider message".into())),
        ("credentials", Value::String("redacted".into())),
        ("authorization", Value::String("credential".into())),
        ("unknown", Value::Bool(true)),
    ] {
        let mut invalid = fixture["providerError"].clone();
        invalid.as_object_mut().unwrap().insert(field.into(), value);
        assert!(!is_valid_definition(&schema, "providerError", &invalid));
    }
    for field in ["response_body_included", "credentials_included"] {
        let mut invalid = fixture["providerError"].clone();
        invalid[field] = Value::Bool(true);
        assert!(!is_valid_definition(&schema, "providerError", &invalid));
    }
}

#[test]
fn usage_artifact_and_capability_boundaries_fail_closed() {
    let (schema, fixture) = core_fixture();
    let mut unknown_with_value = fixture["usage"].clone();
    unknown_with_value["entries"][0]["value"] = serde_json::json!({
        "kind": "token",
        "input_tokens": 1,
        "output_tokens": 1,
        "total_tokens": 2,
        "cached_input_tokens": 0,
        "reasoning_output_tokens": 0
    });
    assert!(!is_valid_definition(&schema, "usage", &unknown_with_value));

    let mut all_null = fixture["usageObserved"].clone();
    for field in [
        "input_tokens",
        "output_tokens",
        "total_tokens",
        "cached_input_tokens",
        "reasoning_output_tokens",
    ] {
        all_null["entries"][0]["value"][field] = Value::Null;
    }
    assert!(!is_valid_definition(&schema, "usage", &all_null));
    let mut zero_window = fixture["usageObserved"].clone();
    zero_window["entries"][1]["value"]["window_tokens"] = Value::from(0);
    assert!(!is_valid_definition(&schema, "usage", &zero_window));
    let mut invalid_reasoning = fixture["usageObserved"].clone();
    invalid_reasoning["entries"][3]["value"]["available"] = Value::Bool(false);
    assert!(!is_valid_definition(&schema, "usage", &invalid_reasoning));
    let mut unsafe_integer = fixture["usageObserved"].clone();
    unsafe_integer["entries"][0]["value"]["total_tokens"] = Value::from(9_007_199_254_740_992_u64);
    assert!(!is_valid_definition(&schema, "usage", &unsafe_integer));
    let mut wrong_evidence = fixture["usageObserved"].clone();
    wrong_evidence["entries"][0]["evidence"]["evidence_identity"] =
        Value::String("provider-usage:sha256:INVALID".into());
    assert!(!is_valid_definition(&schema, "usage", &wrong_evidence));
    let mut duplicate_metric = fixture["usage"].clone();
    duplicate_metric["entries"][1]["metric"] = Value::String("token".into());
    assert!(!is_valid_definition(&schema, "usage", &duplicate_metric));

    let mut generic_artifact = fixture["commandOutputArtifact"].clone();
    generic_artifact["reference"] = Value::String(format!("artifact:sha256:{}", "a".repeat(64)));
    assert!(!is_valid_definition(
        &schema,
        "commandOutputArtifact",
        &generic_artifact
    ));
    let mut uppercase_artifact = fixture["commandOutputArtifact"].clone();
    uppercase_artifact["sha256"] = Value::String("A".repeat(64));
    assert!(!is_valid_definition(
        &schema,
        "commandOutputArtifact",
        &uppercase_artifact
    ));

    let capability = &fixture["capabilitySet"];
    let mut experimental = capability.clone();
    experimental["experimental"] = serde_json::json!(["future.feature"]);
    assert!(!is_valid_definition(
        &schema,
        "capabilitySet",
        &experimental
    ));
    let mut duplicate = capability.clone();
    duplicate["stable"] = serde_json::json!(["permission.read-only", "permission.read-only"]);
    assert!(!is_valid_definition(&schema, "capabilitySet", &duplicate));
    let mut illegal = capability.clone();
    illegal["stable"] = serde_json::json!(["Permission.ReadOnly"]);
    assert!(!is_valid_definition(&schema, "capabilitySet", &illegal));
}
