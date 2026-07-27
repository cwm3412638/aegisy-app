use aegisy_aap::MAX_AAP_FRAME_BYTES;
use aegisy_agentd::{Runtime, STABLE_CAPABILITY_REGISTRY};
use serde_json::{json, Map, Value};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

fn package_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../aap-schema")
}

fn read_json(path: &Path) -> Value {
    serde_json::from_str(&fs::read_to_string(path).expect("JSON file must be readable"))
        .expect("file must contain valid JSON")
}

fn definition_validator(schema: &Value, name: &str) -> jsonschema::Validator {
    let document = json!({
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$ref": format!("#/$defs/{name}"),
        "$defs": schema["$defs"].clone()
    });
    jsonschema::validator_for(&document).expect("definition Schema must compile")
}

fn assert_definition(schema: &Value, name: &str, value: &Value) {
    let validator = definition_validator(schema, name);
    let errors = validator
        .iter_errors(value)
        .map(|error| error.to_string())
        .collect::<Vec<_>>();
    assert!(
        errors.is_empty(),
        "Runtime value failed core $defs/{name}: {errors:?}\n{value}"
    );
}

fn request(id: &str, method: &str, params: Value) -> String {
    json!({"jsonrpc": "2.0", "id": id, "method": method, "params": params}).to_string()
}

fn ready(runtime: &mut Runtime) -> Value {
    let stable = STABLE_CAPABILITY_REGISTRY
        .iter()
        .copied()
        .filter(|capability| *capability != "timeline.subscription.fixed-watermark")
        .collect::<Vec<_>>();
    let initialized = runtime.handle_line(&request(
        "initialize",
        "initialize",
        json!({
            "protocol": {"minimum": "0.1", "maximum": "0.1", "preferred": "0.1"},
            "client": {"name": "core-schema-runtime-test", "version": "1"},
            "platform": {"os": "macos", "architecture": "arm64"},
            "capabilities": {"stable": stable, "experimental": []},
            "limits": {"max_frame_bytes": MAX_AAP_FRAME_BYTES},
            "transport_security": {
                "transport": "stdio",
                "local": true,
                "authenticated": false,
                "encrypted": false,
                "peer_verified": false
            }
        }),
    ));
    assert!(initialized[0].get("result").is_some(), "{initialized:?}");
    assert!(runtime
        .handle_line(r#"{"jsonrpc":"2.0","method":"initialized","params":{}}"#)
        .is_empty());
    initialized[0]["result"].clone()
}

fn result<'a>(messages: &'a [Value], id: &str) -> &'a Value {
    &messages
        .iter()
        .find(|message| message["id"] == id && message.get("result").is_some())
        .unwrap_or_else(|| panic!("missing successful response {id}: {messages:?}"))["result"]
}

fn select(value: &Value, keys: &[&str]) -> Value {
    let source = value
        .as_object()
        .expect("projection source must be an object");
    let mut selected = Map::new();
    for key in keys {
        selected.insert(
            (*key).to_owned(),
            source
                .get(*key)
                .unwrap_or_else(|| panic!("projection source is missing {key}"))
                .clone(),
        );
    }
    Value::Object(selected)
}

fn test_root() -> PathBuf {
    let unique = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("clock must be after epoch")
        .as_nanos();
    std::env::temp_dir().join(format!("aegisy-core-schema-runtime-{unique}"))
}

#[test]
fn real_runtime_core_projections_match_the_registered_schema() {
    let root = test_root();
    let data_root = root.join("data");
    let project_root = root.join("project");
    fs::create_dir_all(&data_root).unwrap();
    fs::create_dir_all(&project_root).unwrap();

    let package = package_root();
    let schema = read_json(&package.join("stable/v0.1/core.schema.json"));
    let transport = read_json(&package.join("stable/v0.1/aap.schema.json"));
    let mut runtime = Runtime::with_store(&data_root).unwrap();
    let initialized = ready(&mut runtime);
    assert_definition(&schema, "runtimeIdentity", &initialized["runtime"]);
    assert_definition(&schema, "runtimeBackend", &initialized["backend"]);
    assert_definition(&schema, "capabilitySet", &initialized["capabilities"]);

    let opened = runtime.handle_line(&request(
        "project-open",
        "project/open",
        json!({"root": project_root}),
    ));
    let opened = result(&opened, "project-open");
    assert_definition(&schema, "project", &opened["project"]);
    let project_id = opened["project"]["id"].as_str().unwrap().to_owned();

    let roots = runtime.handle_line(&request(
        "project-roots",
        "project/root-list",
        json!({"project_id": project_id}),
    ));
    let roots = result(&roots, "project-roots");
    assert_definition(&schema, "projectRoot", &roots["roots"][0]);

    let projects = runtime.handle_line(&request(
        "project-list",
        "project/list",
        json!({"limit": 100}),
    ));
    let projects = result(&projects, "project-list");
    assert_definition(&schema, "projectNavigationEntry", &projects["projects"][0]);

    let chat = runtime.handle_line(&request(
        "chat-start",
        "session/start",
        json!({"mode": "chat", "title": "Schema Chat"}),
    ));
    let chat = result(&chat, "chat-start");
    assert_definition(&schema, "session", &chat["session"]);
    assert_definition(&schema, "runtimeLiveBinding", &chat["runtime"]);
    assert!(chat["workspace"].is_null());
    let chat_id = chat["session"]["id"].as_str().unwrap().to_owned();

    let work = runtime.handle_line(&request(
        "work-start",
        "session/start",
        json!({"mode": "work", "project_id": project_id, "title": "Schema Work"}),
    ));
    let work = result(&work, "work-start");
    assert_definition(&schema, "session", &work["session"]);
    assert_definition(&schema, "runtimeLiveBinding", &work["runtime"]);
    assert_definition(&schema, "workspace", &work["workspace"]);
    let work_id = work["session"]["id"].as_str().unwrap().to_owned();

    let sessions = runtime.handle_line(&request(
        "session-list",
        "session/list",
        json!({"limit": 200, "include_archived": true}),
    ));
    let sessions = result(&sessions, "session-list");
    for session in sessions["sessions"].as_array().unwrap() {
        let projection = select(
            session,
            &[
                "session_id",
                "project_id",
                "mode",
                "title",
                "parent_session_id",
                "lineage_kind",
                "status",
                "environment_identity",
                "created_at_ms",
                "updated_at_ms",
            ],
        );
        assert_definition(&schema, "sessionProjection", &projection);
    }

    let search = runtime.handle_line(&request(
        "session-search",
        "session/search",
        json!({"title": "Schema Work", "limit": 10}),
    ));
    let search = result(&search, "session-search");
    assert_definition(
        &schema,
        "runtimeSearchProjection",
        &search["sessions"][0]["runtime"],
    );

    let turn = runtime.handle_line(&request(
        "turn-start",
        "turn/start",
        json!({
            "session_id": chat_id,
            "input": "schema capture",
            "idempotency_key": "core-schema-turn",
            "generation": 1
        }),
    ));
    let turn_result = result(&turn, "turn-start");
    assert_definition(&schema, "turnStartAcknowledgement", &turn_result["turn"]);
    let turn_id = turn_result["turn"]["id"].as_str().unwrap();
    let terminal_event = turn
        .iter()
        .find(|message| {
            matches!(
                message.pointer("/params/event").and_then(Value::as_str),
                Some("turn.completed" | "turn.failed" | "turn.interrupted")
            )
        })
        .expect("preview turn must emit a terminal lifecycle event");
    let terminal_turn_id = terminal_event["params"]["turn_id"]
        .as_str()
        .expect("terminal event must identify its Turn");
    let terminal_state = terminal_event["params"]["turn_state"]
        .as_str()
        .expect("terminal event must project its Turn state");
    assert_eq!(terminal_turn_id, turn_id);
    assert_eq!(
        terminal_event["params"]["event"],
        format!("turn.{terminal_state}")
    );
    assert_definition(
        &schema,
        "turn",
        &json!({"id": terminal_turn_id, "state": terminal_state}),
    );
    let transport_item = definition_validator(&transport, "timelineItem");
    let mut item_count = 0;
    for item in turn
        .iter()
        .filter_map(|message| message.pointer("/params/item"))
        .filter(|item| !item.is_null())
    {
        assert_definition(&schema, "item", item);
        assert!(transport_item.is_valid(item));
        item_count += 1;
    }
    assert!(item_count > 0, "preview turn must emit Timeline Items");

    drop(runtime);
    let mut restarted = Runtime::with_store(&data_root).unwrap();
    let restarted_initialize = ready(&mut restarted);
    assert_definition(&schema, "runtimeIdentity", &restarted_initialize["runtime"]);
    assert_definition(&schema, "runtimeBackend", &restarted_initialize["backend"]);
    assert_definition(
        &schema,
        "capabilitySet",
        &restarted_initialize["capabilities"],
    );
    let replay = restarted.handle_line(&request(
        "session-read",
        "session/read",
        json!({"session_id": chat_id, "limit": 200}),
    ));
    let replay = result(&replay, "session-read");
    assert_definition(&schema, "session", &replay["session"]);
    assert_definition(&schema, "runtimeReplayProjection", &replay["runtime"]);
    assert!(replay["workspace"].is_null());
    for item in replay["items"].as_array().unwrap() {
        assert_definition(&schema, "sessionHistoryItem", item);
    }

    let work_replay = restarted.handle_line(&request(
        "work-read",
        "session/read",
        json!({"session_id": work_id, "limit": 200}),
    ));
    let work_replay = result(&work_replay, "work-read");
    assert_definition(&schema, "runtimeReplayProjection", &work_replay["runtime"]);
    assert_definition(&schema, "workspace", &work_replay["workspace"]);

    drop(restarted);
    fs::remove_dir_all(root).unwrap();
}
