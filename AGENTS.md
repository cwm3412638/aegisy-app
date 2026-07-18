# Aegisy Repository Instructions

Before any analysis, command, code change, review, build, packaging, or release
operation in this repository, read `PROJECT-MEMORY.md` completely.

After changing architecture, security boundaries, packaging requirements,
release gates, persistent configuration, incident conclusions, or major product
status, update `PROJECT-MEMORY.md` in the same change.

Never write API keys, access tokens, signing keys, user identifiers, or other
secrets into the memory document. Record only redacted diagnostics and durable
engineering conclusions.

The OpenSpec change at
`openspec/changes/build-aegisy-agent-workbench/` is the detailed product plan.
The memory document is the concise, current operational source of truth and
must link to OpenSpec rather than duplicating every task.
