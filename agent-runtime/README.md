# Aegisy Agent Runtime

This workspace contains the executable Aegisy Agent Protocol (AAP) runtime and
the first Codex App Server adapter. The default runtime launches the installed
Codex CLI and maps its thread, turn, and agent-message events into stable AAP
events.

The adapter is pinned to Codex CLI/App Server `0.144.5`. The generated v2
protocol schema used by the adapter is checked in at
`aap-schema/codex-app-server-0.144.5/v2.schemas.json`; a different installed
version is rejected before the app-server process is launched.

## Run

```sh
cargo run -p aegisy-agentd
```

Messages are JSON-RPC 2.0 objects separated by newlines on stdin/stdout. The
`initialize` request and `initialized` notification must complete before project
or session methods are accepted.

## Supported preview methods

- `initialize`
- `initialized`
- `project/open`
- `session/start`
- `turn/start`
- `session/read`
- `runtime/health`
- `workspace/list`
- `workspace/read`
- `shutdown`

Both Chat and Work currently force Codex into `read-only` sandbox mode with an
approval policy of `never`. Chat uses an isolated empty temporary workspace;
Work binds the selected project root so the Agent can inspect project context.
Neither mode can approve file writes or mutating commands in this milestone.

Codex provider state is exposed only through bounded read-only projections:
`session/provider-list` lists thread metadata with an optional project root and
cursor, and `session/provider-read` reads one thread's metadata plus bounded turn
metadata when requested. Provider item contents, raw rollout fields, delete, and
compact operations are intentionally omitted until their AAP review and recovery
contracts are complete.

Workspace browsing uses project-relative paths only. The sidecar rejects path
traversal, absolute paths, symlinks, sensitive credential filenames, binary
content, and text files larger than the negotiated preview limit. Generated and
runtime-owned directories are omitted from directory listings.

Set `AEGISY_AGENT_BACKEND=preview` to run the deterministic echo backend used by
protocol and UI rendering tests.

## Portable sessions

The runtime exposes preview/commit pairs for redacted session export and import:
`session/export/preview`, `session/export`, `session/import/preview`, and
`session/import`. The versioned package contract, limits, exclusions, collision
strategies, and continuation boundary are documented in
[`docs/PORTABLE-SESSION-FORMAT.md`](../docs/PORTABLE-SESSION-FORMAT.md).
