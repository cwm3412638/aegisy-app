use aegisy_aap::generated_transport::{
    decode_transport_definition_raw, decode_transport_message_raw, TRANSPORT_SCHEMA_ID,
    TRANSPORT_SCHEMA_JSON,
};
use aegisy_aap::transport_json::{
    canonical_transport_json, parse_transport_json, MAX_TRANSPORT_JSON_BYTES,
};
use serde_json::{json, Map, Value};
use sha2::{Digest, Sha256};
use std::collections::HashSet;
use std::{env, fs, process};

const FIXTURE_SCHEMA: &str = "aap-transport-fixture-catalog/0.1";
const CORPUS_SCHEMA: &str = "aap-transport-validation-corpus/0.1";
const CURRENT_PARSER_PROFILE: &str =
    "exact-json-number-schema-bounded-integer-unicode-scalar-no-duplicate-keys/0.1";

fn exact_object<'a>(
    value: &'a Value,
    keys: &[&str],
    context: &str,
) -> Result<&'a Map<String, Value>, String> {
    let object = value
        .as_object()
        .ok_or_else(|| format!("{context} must be an object"))?;
    if object.len() != keys.len()
        || object.keys().any(|key| !keys.contains(&key.as_str()))
        || keys.iter().any(|key| !object.contains_key(*key))
    {
        return Err(format!("{context} must contain the exact required fields"));
    }
    Ok(object)
}

fn metadata(path: &str, kind: &str) -> Result<Value, String> {
    let bytes = fs::read(path).map_err(|_| format!("{kind} must be readable"))?;
    if bytes.len() > MAX_TRANSPORT_JSON_BYTES {
        return Err(format!("{kind} exceeds the metadata limit"));
    }
    let strict = parse_transport_json(&bytes).map_err(|_| format!("{kind} must be strict JSON"))?;
    #[cfg(debug_assertions)]
    {
        let reference: Value =
            serde_json::from_slice(&bytes).map_err(|_| format!("{kind} must be valid JSON"))?;
        debug_assert_eq!(strict, reference);
    }
    Ok(strict)
}

fn lowercase_sha256(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn definition_names() -> Result<Vec<String>, String> {
    let schema = parse_transport_json(TRANSPORT_SCHEMA_JSON.as_bytes())
        .map_err(|_| "generated transport schema must be strict JSON".to_owned())?;
    if schema.get("$id").and_then(Value::as_str) != Some(TRANSPORT_SCHEMA_ID) {
        return Err("generated transport schema identity mismatch".to_owned());
    }
    let definitions = schema
        .get("$defs")
        .and_then(Value::as_object)
        .ok_or_else(|| "generated transport schema definitions are missing".to_owned())?;
    let mut names = definitions.keys().cloned().collect::<Vec<_>>();
    names.sort_unstable();
    Ok(names)
}

fn fixture_identity(path: &str) -> Result<(), String> {
    let fixture = metadata(path, "fixture")?;
    let fixture = exact_object(
        &fixture,
        &[
            "schema_version",
            "schema_id",
            "canonical_bytes",
            "canonical_sha256",
            "entries",
        ],
        "fixture",
    )?;
    if fixture.get("schema_version").and_then(Value::as_str) != Some(FIXTURE_SCHEMA)
        || fixture.get("schema_id").and_then(Value::as_str) != Some(TRANSPORT_SCHEMA_ID)
    {
        return Err("fixture version or schema identity mismatch".to_owned());
    }
    let expected_bytes = fixture
        .get("canonical_bytes")
        .and_then(Value::as_u64)
        .filter(|value| *value > 0)
        .ok_or_else(|| "fixture canonical byte count is invalid".to_owned())?;
    let expected_digest = fixture
        .get("canonical_sha256")
        .and_then(Value::as_str)
        .filter(|value| lowercase_sha256(value))
        .ok_or_else(|| "fixture canonical identity is invalid".to_owned())?;
    let entries = fixture
        .get("entries")
        .and_then(Value::as_array)
        .ok_or_else(|| "fixture entries must be an array".to_owned())?;
    let names = definition_names()?;
    if entries.len() != names.len() {
        return Err(format!(
            "fixture must contain exactly {} definitions",
            names.len()
        ));
    }

    let mut canonical = format!("{FIXTURE_SCHEMA}\n{TRANSPORT_SCHEMA_ID}\n").into_bytes();
    let mut seen = HashSet::new();
    for (index, (entry, expected_name)) in entries.iter().zip(&names).enumerate() {
        let entry = exact_object(entry, &["definition", "value_json"], "fixture entry")?;
        let definition = entry
            .get("definition")
            .and_then(Value::as_str)
            .ok_or_else(|| format!("fixture entry {index} has an invalid definition"))?;
        if definition != expected_name || !seen.insert(definition) {
            return Err(format!(
                "fixture entry {index} is unsorted, unknown, or duplicated"
            ));
        }
        let value_json = entry
            .get("value_json")
            .and_then(Value::as_str)
            .ok_or_else(|| format!("fixture {definition} raw JSON is invalid"))?;
        let value = decode_transport_definition_raw(definition, value_json.as_bytes())
            .map_err(|_| format!("fixture {definition} fails generated Rust validation"))?;
        let value_json = canonical_transport_json(&value)
            .map_err(|_| format!("fixture {definition} canonical JSON is invalid"))?;
        canonical.extend_from_slice(definition.as_bytes());
        canonical.push(b'\t');
        canonical.extend_from_slice(value_json.len().to_string().as_bytes());
        canonical.push(b'\n');
        canonical.extend_from_slice(&value_json);
        canonical.push(b'\n');
    }

    let actual_bytes = u64::try_from(canonical.len())
        .map_err(|_| "fixture canonical byte count overflows".to_owned())?;
    let actual_digest = format!("{:x}", Sha256::digest(&canonical));
    if actual_bytes != expected_bytes || actual_digest != expected_digest {
        return Err(format!(
            "fixture identity differs from golden: computed {actual_bytes} {actual_digest}"
        ));
    }
    println!("{actual_bytes} {actual_digest}");
    Ok(())
}

fn valid_case_name(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 96
        && value.split('-').all(|segment| {
            !segment.is_empty()
                && segment
                    .bytes()
                    .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit())
        })
}

fn probe_schema(target: &str) -> Option<Value> {
    match target {
        "$probe:anyOf" => Some(json!({
            "$schema": "https://json-schema.org/draft/2020-12/schema",
            "anyOf": [
                {"type": "string", "minLength": 1},
                {"type": "integer", "minimum": 1}
            ]
        })),
        "$probe:constNumber" => Some(json!({
            "$schema": "https://json-schema.org/draft/2020-12/schema",
            "const": 1
        })),
        "$probe:uniqueItems" => Some(json!({
            "$schema": "https://json-schema.org/draft/2020-12/schema",
            "type": "array",
            "uniqueItems": true,
            "items": true
        })),
        _ => None,
    }
}

fn validate_case(target: &str, value_json: &str, definitions: &HashSet<String>) -> bool {
    if target == "$root" {
        return decode_transport_message_raw(value_json.as_bytes()).is_ok();
    }
    if definitions.contains(target) {
        return decode_transport_definition_raw(target, value_json.as_bytes()).is_ok();
    }
    let Some(schema) = probe_schema(target) else {
        return false;
    };
    let Ok(value) = parse_transport_json(value_json.as_bytes()) else {
        return false;
    };
    jsonschema::validator_for(&schema).is_ok_and(|validator| validator.is_valid(&value))
}

fn corpus_identity(path: &str) -> Result<(), String> {
    let corpus = metadata(path, "corpus")?;
    let corpus = exact_object(
        &corpus,
        &[
            "schema_version",
            "schema_id",
            "parser_profile",
            "expected_decisions_sha256",
            "cases",
        ],
        "corpus",
    )?;
    if corpus.get("schema_version").and_then(Value::as_str) != Some(CORPUS_SCHEMA)
        || corpus.get("schema_id").and_then(Value::as_str) != Some(TRANSPORT_SCHEMA_ID)
        || corpus.get("parser_profile").and_then(Value::as_str) != Some(CURRENT_PARSER_PROFILE)
    {
        return Err("corpus version, schema identity, or parser profile mismatch".to_owned());
    }
    let expected_digest = corpus
        .get("expected_decisions_sha256")
        .and_then(Value::as_str)
        .filter(|value| lowercase_sha256(value))
        .ok_or_else(|| "corpus decision identity is invalid".to_owned())?;
    let cases = corpus
        .get("cases")
        .and_then(Value::as_array)
        .filter(|cases| !cases.is_empty() && cases.len() <= 128)
        .ok_or_else(|| "corpus must contain 1 through 128 cases".to_owned())?;
    let definitions = definition_names()?.into_iter().collect::<HashSet<_>>();
    let mut names = HashSet::new();
    let mut decisions = format!("{CORPUS_SCHEMA}\t{CURRENT_PARSER_PROFILE}\n").into_bytes();

    for (index, case) in cases.iter().enumerate() {
        let case = exact_object(case, &["name", "target", "valid", "value_json"], "case")?;
        let name = case
            .get("name")
            .and_then(Value::as_str)
            .ok_or_else(|| format!("case {index} has an invalid name"))?;
        if !valid_case_name(name) || !names.insert(name) {
            return Err(format!("case {index} has an invalid or duplicate name"));
        }
        let target = case
            .get("target")
            .and_then(Value::as_str)
            .ok_or_else(|| format!("case {name} has an invalid target"))?;
        if target != "$root" && !definitions.contains(target) && probe_schema(target).is_none() {
            return Err(format!("case {name} has an unknown target"));
        }
        let expected = case
            .get("valid")
            .and_then(Value::as_bool)
            .ok_or_else(|| format!("case {name} has an invalid expectation"))?;
        let value_json = case
            .get("value_json")
            .and_then(Value::as_str)
            .filter(|value| value.len() <= MAX_TRANSPORT_JSON_BYTES)
            .ok_or_else(|| format!("case {name} has invalid raw JSON"))?;
        let accepted = validate_case(target, value_json, &definitions);
        if accepted != expected {
            return Err(format!(
                "case {name} expected {} but generated Rust validator {} it",
                if expected { "accept" } else { "reject" },
                if accepted { "accepted" } else { "rejected" }
            ));
        }
        decisions.extend_from_slice(name.as_bytes());
        decisions.push(b'\t');
        decisions.extend_from_slice(target.as_bytes());
        decisions.push(b'\t');
        decisions.extend_from_slice(if accepted { b"accept" } else { b"reject" });
        decisions.push(b'\n');
    }

    let actual_digest = format!("{:x}", Sha256::digest(&decisions));
    if actual_digest != expected_digest {
        return Err(format!(
            "corpus decision identity differs from golden: computed {actual_digest}"
        ));
    }
    println!("{} {actual_digest}", cases.len());
    Ok(())
}

fn run() -> Result<(), String> {
    let arguments = env::args().skip(1).collect::<Vec<_>>();
    match arguments.as_slice() {
        [path] => fixture_identity(path),
        [flag, path] if flag == "--corpus" => corpus_identity(path),
        _ => Err("usage: aap_transport_fixture [--corpus] <path>".to_owned()),
    }
}

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn metadata_rejects_unknown_fields() {
        let value = json!({"schema_version": FIXTURE_SCHEMA, "unknown": true});
        assert!(exact_object(&value, &["schema_version"], "fixture").is_err());
    }

    #[test]
    fn metadata_parser_rejects_duplicates_and_non_scalar_unicode() {
        for raw in [
            br#"{"schema_version":"a","schema_version":"b"}"#.as_slice(),
            br#"{"schema_version":"\ud800"}"#,
        ] {
            assert!(parse_transport_json(raw).is_err());
        }
    }
}
