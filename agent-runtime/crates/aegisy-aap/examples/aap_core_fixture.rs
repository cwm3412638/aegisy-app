use aegisy_aap::generated_core::{
    canonical_core_fixture_catalog, decode_core_fixture_catalog, validate_core_definition,
};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::collections::HashSet;
use std::{env, fs, process};

const CORPUS_SCHEMA: &str = "aap-core-generated-corpus/0.1";

fn object_with_exact_keys<'a>(
    value: &'a Value,
    keys: &[&str],
    context: &str,
) -> Result<&'a serde_json::Map<String, Value>, String> {
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

fn is_case_name(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 96
        && value.split('-').all(|part| {
            !part.is_empty()
                && part
                    .bytes()
                    .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit())
        })
}

fn is_definition_name(value: &str) -> bool {
    let mut bytes = value.bytes();
    bytes.next().is_some_and(|byte| byte.is_ascii_alphabetic())
        && bytes.all(|byte| byte.is_ascii_alphanumeric())
}

fn emit_corpus_identity(path: &str) -> Result<(), String> {
    let bytes = fs::read(path).map_err(|_| "corpus must be readable".to_owned())?;
    let corpus: Value =
        serde_json::from_slice(&bytes).map_err(|_| "corpus must be valid JSON".to_owned())?;
    let corpus = object_with_exact_keys(
        &corpus,
        &["schema_version", "expected_decisions_sha256", "cases"],
        "corpus",
    )?;
    if corpus.get("schema_version").and_then(Value::as_str) != Some(CORPUS_SCHEMA) {
        return Err("unsupported corpus version".to_owned());
    }
    let expected_digest = corpus
        .get("expected_decisions_sha256")
        .and_then(Value::as_str)
        .ok_or_else(|| "corpus decision identity must be a string".to_owned())?;
    if expected_digest.len() != 64
        || !expected_digest
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return Err("corpus decision identity must be a lowercase SHA-256 digest".to_owned());
    }
    let cases = corpus
        .get("cases")
        .and_then(Value::as_array)
        .ok_or_else(|| "corpus cases must be an array".to_owned())?;
    if cases.is_empty() || cases.len() > 128 {
        return Err("corpus cases must contain 1 through 128 entries".to_owned());
    }

    let mut names = HashSet::new();
    let mut decisions = format!("{CORPUS_SCHEMA}\n");
    for (index, entry) in cases.iter().enumerate() {
        let entry = object_with_exact_keys(
            entry,
            &["name", "definition", "valid", "value_json"],
            &format!("case {index}"),
        )?;
        let name = entry
            .get("name")
            .and_then(Value::as_str)
            .ok_or_else(|| format!("case {index} has an invalid name"))?;
        if !is_case_name(name) || !names.insert(name) {
            return Err(format!("case {index} has an invalid or duplicate name"));
        }
        let definition = entry
            .get("definition")
            .and_then(Value::as_str)
            .ok_or_else(|| format!("case {name} has an invalid definition"))?;
        if !is_definition_name(definition) {
            return Err(format!("case {name} has an invalid definition"));
        }
        let expected = entry
            .get("valid")
            .and_then(Value::as_bool)
            .ok_or_else(|| format!("case {name} has an invalid expectation"))?;
        let value_json = entry
            .get("value_json")
            .and_then(Value::as_str)
            .filter(|value| value.len() <= 4 * 1024 * 1024)
            .ok_or_else(|| format!("case {name} has invalid raw JSON"))?;
        let accepted = serde_json::from_str::<Value>(value_json)
            .ok()
            .is_some_and(|value| validate_core_definition(definition, &value).is_ok());
        if accepted != expected {
            return Err(format!(
                "case {name} expected {} but validator {} it",
                if expected { "accept" } else { "reject" },
                if accepted { "accepted" } else { "rejected" }
            ));
        }
        decisions.push_str(name);
        decisions.push('\t');
        decisions.push_str(definition);
        decisions.push('\t');
        decisions.push_str(if accepted { "accept" } else { "reject" });
        decisions.push('\n');
    }

    let digest = format!("{:x}", Sha256::digest(decisions.as_bytes()));
    if digest != expected_digest {
        return Err(format!(
            "corpus decision identity differs from golden: computed {digest}"
        ));
    }
    println!("{} {digest}", cases.len());
    Ok(())
}

fn emit_fixture_identity(path: &str) -> Result<(), String> {
    let bytes = fs::read(path).map_err(|_| "fixture must be readable".to_owned())?;
    let fixture = decode_core_fixture_catalog(&bytes)
        .map_err(|_| "fixture must match generated Rust types".to_owned())?;
    let canonical = canonical_core_fixture_catalog(&fixture)
        .map_err(|_| "generated Rust fixture must serialize canonically".to_owned())?;
    println!("{} {:x}", canonical.len(), Sha256::digest(&canonical));
    Ok(())
}

fn run() -> Result<(), String> {
    let arguments: Vec<String> = env::args().skip(1).collect();
    match arguments.as_slice() {
        [path] => emit_fixture_identity(path),
        [flag, path] if flag == "--corpus" => emit_corpus_identity(path),
        _ => Err("usage: aap_core_fixture [--corpus] <path>".to_owned()),
    }
}

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        process::exit(1);
    }
}
