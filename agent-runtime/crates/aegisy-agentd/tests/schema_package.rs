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
            "files",
            "aegisy",
        ]
    ));
    assert_eq!(package["name"], "@aegisy/aap-schema");
    assert_eq!(package["version"], "0.1.0");
    assert_eq!(package["private"], true);
    assert_eq!(package["license"], "UNLICENSED");
    assert_eq!(
        package["files"],
        serde_json::json!(["README.md", "stable", "experimental", "fixtures"])
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
    for version in versions {
        let version = version
            .as_object()
            .expect("stable version must be an object");
        assert!(exact_keys(
            version,
            &["protocol_version", "directory", "schema", "schema_id"]
        ));
        let protocol_version = version["protocol_version"]
            .as_str()
            .expect("protocol version must be a string");
        let directory = version["directory"]
            .as_str()
            .expect("directory version must be a string");
        let schema_path = version["schema"]
            .as_str()
            .expect("schema path must be a string");
        let schema_id = version["schema_id"]
            .as_str()
            .expect("schema ID must be a string");
        assert_eq!(directory, format!("v{protocol_version}"));
        assert!(schema_path.starts_with(&format!("{directory}/")));
        assert!(protocol_versions.insert(protocol_version));
        assert!(schema_ids.insert(schema_id));

        let schema_path = package_file(namespace_root, schema_path);
        assert!(schema_path.is_file());
        let schema = read_json(&schema_path);
        assert_eq!(schema["$id"], schema_id);
        assert_eq!(
            schema_id,
            &format!("https://aegisy.cc/schemas/aap/stable/{directory}/aap.schema.json")
        );
        jsonschema::validator_for(&schema).expect("stable JSON Schema must compile");
        assert_no_experimental_reference(&schema);
    }
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
