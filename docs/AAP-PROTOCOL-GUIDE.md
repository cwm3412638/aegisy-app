# Aegisy Agent Protocol Guide

Version: `AAP 0.1`  
Audience: Qt host, `aegisy-agentd`, and future Codex/ACP adapters.

This is the internal wire guide for the currently shipped read-only workbench.
The normative schemas and limits live in `agent-runtime/aap-schema`; the JSONL
fixtures under `agent-runtime/aap-schema/fixtures` are deterministic replay
evidence. AAP is JSON-RPC 2.0 over newline-delimited stdio. Each request and
response is one complete JSON object; a notification has no `id`.

## Connection

The client must complete `initialize` before sending project, session, turn,
workspace, terminal, or storage requests. The runtime returns a stable protocol
version, runtime/backend identity, negotiated capabilities, and permission
profile. The client then sends `initialized`.

```jsonl
{"jsonrpc":"2.0","id":"1","method":"initialize","params":{"protocol_version":"0.1","client":{"name":"aegisy-client","version":"fixture"}}}
{"jsonrpc":"2.0","id":"1","result":{"protocol_version":"0.1","runtime":{"name":"aegisy-agentd","version":"0.1.0"},"backend":{"adapter":"preview","status":"ready","version":"0.1.0"},"capabilities":["runtime.preview","runtime.health","runtime.degradations","permission.read-only"]}}
{"jsonrpc":"2.0","method":"initialized"}
```

An unsupported protocol version fails before a session is loaded. Clients must
not infer a missing capability from a successful initialize; they must gate the
dependent action on the explicit capability/degradation state.

## Session And Turn

Session identity is the boundary for history, environment identity, project
roots, terminals, artifacts, and provider continuation. Work sessions require a
project. The current Codex adapter uses `permission_profile: read-only`.

```jsonl
{"jsonrpc":"2.0","id":"2","method":"session/start","params":{"mode":"chat","title":"AAP example"}}
{"jsonrpc":"2.0","id":"2","result":{"session":{"id":"session-1","mode":"chat","title":"AAP example"},"runtime":{"adapter":"preview","version":"0.1.0","provider":"local","model":"deterministic-echo","permission_profile":"read-only"},"environment":{"environment_id":"environment:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","identity":"environment:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}}
{"jsonrpc":"2.0","id":"3","method":"turn/start","params":{"session_id":"session-1","input":"Inspect the project","idempotency_key":"turn-key-1"}}
{"jsonrpc":"2.0","method":"event","params":{"event":"turn.started","session_id":"session-1","turn_id":"turn-1"}}
{"jsonrpc":"2.0","method":"event","params":{"event":"item.delta","session_id":"session-1","turn_id":"turn-1","item":{"id":"item-1","kind":"message","role":"agent","state":"delta","content":"Project summary"}}}
{"jsonrpc":"2.0","method":"event","params":{"event":"turn.completed","session_id":"session-1","turn_id":"turn-1"}}
```

Every item event carries the same session and turn identity until the terminal
turn event. A turn has exactly one terminal state: `completed`, `interrupted`,
or `failed`. Reusing an idempotency key with different input is an error; retrying
the same request returns the original turn identity.

## Structured Errors And Cancellation

Error messages are diagnostics, not a stable UI contract. Clients should use the
numeric code plus bounded `runtime-error/0.1` data when present. Provider,
transport, timeout, persistence, and adapter classes never include credentials,
prompts, response bodies, or raw provider rollout text in AAP timeline data.

```jsonl
{"jsonrpc":"2.0","id":"4","method":"turn/cancel","params":{"session_id":"session-1","turn_id":"turn-1"}}
{"jsonrpc":"2.0","id":"4","result":{"state":"cancellation-requested"}}
{"jsonrpc":"2.0","method":"event","params":{"event":"turn.cancellation-acknowledged","session_id":"session-1","turn_id":"turn-1"}}
{"jsonrpc":"2.0","method":"event","params":{"event":"turn.interrupted","session_id":"session-1","turn_id":"turn-1"}}
{"jsonrpc":"2.0","id":"5","error":{"code":-32110,"message":"provider request failed: [REDACTED]","data":{"schema_version":"runtime-error/0.1","class":"provider","retryable":false}}}
```

Cancellation acknowledgement means the provider accepted the interrupt; it
does not claim the turn is already terminal. Completion may win a cancellation
race. Cancellation is identity-scoped and remains available through the bounded
out-of-band control path when normal dispatch is saturated.

## Capability Degradation

`runtime/degradations` is a versioned, content-free explanation for features that
are unavailable, metadata-only, or blocked. The current Codex adapter reports
read-only Agent mutation, metadata-only provider thread items, and blocked
provider delete/compact. A client must fail closed when the schema is unknown or
the request fails.

```jsonl
{"jsonrpc":"2.0","id":"6","method":"runtime/degradations","params":{}}
{"jsonrpc":"2.0","id":"6","result":{"schema_version":"runtime-degradations/0.1","degradations":[{"feature":"agent-mutation","state":"disabled","scope":"runtime"},{"feature":"provider-thread-item-content","state":"metadata-only","scope":"provider"},{"feature":"provider-thread-compact","state":"blocked","scope":"provider"}]}}
```

No degradation response grants write, command, Hook, network, deletion, or
compaction authority. The Qt host displays the state and does not expose an
action that would simulate a blocked provider operation.

## Replay And Reconnect

The durable session stream is ordered by a session-local sequence. After a
transport loss, the host reads the newest page and then requests older pages with
the strict opaque cursor returned by the runtime. A cursor is exclusive, so a
newer live item cannot duplicate an already rendered item.

```jsonl
{"jsonrpc":"2.0","id":"7","method":"session/read","params":{"session_id":"session-1","limit":100}}
{"jsonrpc":"2.0","id":"7","result":{"session":{"id":"session-1","mode":"chat"},"items":[],"history_page":{"limit":100,"first_sequence":null,"last_sequence":null,"latest_sequence":0,"has_older":false,"older_cursor":null},"runtime":{"adapter":"durable-store-replay","permission_profile":"read-only","replayed":true},"environment":{"replayed":true,"available":false}}}
```

If the requested history is no longer retained, the runtime returns a bounded
snapshot and identifies the replay boundary/gap. The host must not silently
restart from sequence zero or fabricate provider continuation. A resumed Codex
binding is valid only when its pinned adapter version and opaque provider thread
identity still match; otherwise the user must fork a portable, provider-neutral
session.

## Security Boundary

- AAP never carries desktop login tokens, API keys, authenticated proxy values,
  raw environment values, prompts, source content, or provider response bodies
  unless a separately reviewed, bounded content reference explicitly requires it.
- The current Agent/Codex profile is read-only. User editor saves and user
  terminals are separate, explicit operations scoped to the opened project.
- The Qt UI consumes AAP state and does not parse vendor Codex events directly.
- New methods require a schema version, capability/degradation entry, redacted
  fixture, failure/reconnect behavior, persistence implications, and matching
  Qt/sidecar tests before they can be exposed.

## Verification

Use the checked-in fixtures as JSONL, not as prose snapshots. The Rust protocol
tests parse every line and scan for credential-shaped content. Changes to the
pinned Codex schema must update the adapter compatibility runbook and regenerate
the affected fixtures before release.
